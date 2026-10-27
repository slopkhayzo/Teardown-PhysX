#include "px/world.h"

#include <immintrin.h>

#include <PxPhysicsAPI.h>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iterator>

#include "core/log.h"

using namespace physx;

namespace px {

namespace {

std::atomic<bool> g_failed{false};
bool g_gpu = false;  // the current scene runs on the GPU

class ErrorLog : public PxErrorCallback {
public:
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        // warnings (e.g. a sliver of a hull the GPU leaves to the CPU) have their own, smaller allowance
        if (code == PxErrorCode::eDEBUG_WARNING || code == PxErrorCode::ePERF_WARNING) {
            if (++warnings_ <= 5) LOG("physx warning %d: %s (%s:%d)", int(code), message, file, line);
        } else if (++count_ <= 200) {
            LOG("physx error %d: %s (%s:%d)", int(code), message, file, line);
        }
        // the GPU pipeline reports CUDA failures as internal errors, then refuses to simulate (abort)
        if (code == PxErrorCode::eABORT || (g_gpu && code == PxErrorCode::eINTERNAL_ERROR)) g_failed.store(true);
    }

private:
    int count_ = 0, warnings_ = 0;
};

PxDefaultAllocator g_alloc;
ErrorLog g_errors;
PxFoundation* g_foundation = nullptr;
PxPhysics* g_physics = nullptr;
PxDefaultCpuDispatcher* g_dispatcher = nullptr;
PxScene* g_scene = nullptr;
PxMaterial* g_material = nullptr;
PxCudaContextManager* g_cuda = nullptr;  // kept for the process's lifetime, like the foundation
bool g_cudaTried = false;

// Everything collides that the game's collision filter lets collide (kFilterGame, world.h); PhysX itself skips
// static-static and kinematic-kinematic pairs. Pairs with a simulated dynamic actor also report contacts above
// that actor's force threshold (impacts), with their contact points.
bool g_reportImpacts = false;
bool g_noKinematicContacts = false;
PxFilterFlags FilterAll(PxFilterObjectAttributes a0, PxFilterData d0, PxFilterObjectAttributes a1, PxFilterData d1,
                        PxPairFlags& pairFlags, const void*, PxU32) {
    if (g_noKinematicContacts && (PxFilterObjectIsKinematic(a0) || PxFilterObjectIsKinematic(a1)))
        return PxFilterFlag::eKILL;
    if ((d0.word3 & kFilterGame) && (d1.word3 & kFilterGame)) {
        if (!(d0.word0 & d1.word1) || !(d0.word1 & d1.word0)) return PxFilterFlag::eKILL;
        if (d0.word2 != 0 && d0.word2 == d1.word2) return PxFilterFlag::eKILL;
        // static-only shapes meet static blocks, not debris, proxies (the game's dynamic bodies) or frozen pieces
        const bool static0 = PxGetFilterObjectType(a0) == PxFilterObjectType::eRIGID_STATIC;
        const bool static1 = PxGetFilterObjectType(a1) == PxFilterObjectType::eRIGID_STATIC;
        if (((d0.word3 & kFilterStaticOnly) && !static1) || ((d1.word3 & kFilterStaticOnly) && !static0))
            return PxFilterFlag::eKILL;
    }
    pairFlags = PxPairFlag::eCONTACT_DEFAULT;
    auto simulated = [](PxFilterObjectAttributes a) {
        return PxGetFilterObjectType(a) == PxFilterObjectType::eRIGID_DYNAMIC && !PxFilterObjectIsKinematic(a);
    };
    if (g_reportImpacts && (simulated(a0) || simulated(a1)))
        pairFlags |= PxPairFlag::eNOTIFY_THRESHOLD_FORCE_FOUND | PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}

// PhysX looks its GPU module up by name (GetModuleHandle, then the DLL search path, which starts at the game's
// folder): load it from the plugin's own folder first.
bool LoadGpuModule() {
    if (GetModuleHandleW(L"PhysXGpu_64.dll")) return true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LoadGpuModule), &self);
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n > 0 && path[n - 1] != L'\\') --n;
    wcscpy_s(path + n, std::size(path) - n, L"PhysXGpu_64.dll");
    if (LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return true;
    LOG("physx: GPU: could not load %ls (error %lu)", path, GetLastError());
    return false;
}

// The CUDA context, created on first use; null when there is no usable NVIDIA GPU (the scene then runs on the CPU).
PxCudaContextManager* Cuda(bool sync) {
    if (g_cudaTried) return g_cuda;
    g_cudaTried = true;
    if (!LoadGpuModule()) return nullptr;
    PxCudaContextManagerDesc desc;
    g_cuda = PxCreateCudaContextManager(*g_foundation, desc, PxGetProfilerCallback(), sync);
    if (g_cuda && !g_cuda->contextIsValid()) {
        g_cuda->release();
        g_cuda = nullptr;
    }
    if (!g_cuda) {
        LOG("physx: GPU: no CUDA context (no NVIDIA GPU or driver too old)");
        return nullptr;
    }
    int driver = g_cuda->getDriverVersion();
    LOG("physx: GPU: %s, %d SMs at %u MHz, %.1f GB, CUDA driver %d.%d%s", g_cuda->getDeviceName(),
        g_cuda->getMultiprocessorCount(), g_cuda->getClockRate() / 1000, double(g_cuda->getDeviceTotalMemBytes()) / 1e9,
        driver / 1000, driver % 1000 / 10, sync ? ", synchronous launches" : "");
    return g_cuda;
}

}  // namespace

bool CreateWorld(const WorldParams& p) {
    if (g_scene) return true;
    if (!g_foundation) g_foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_alloc, g_errors);
    if (!g_foundation) {
        LOG("physx: PxCreateFoundation failed");
        return false;
    }
    if (!g_physics) g_physics = PxCreatePhysics(PX_PHYSICS_VERSION, *g_foundation, PxTolerancesScale(), false, nullptr);
    if (!g_physics) {
        LOG("physx: PxCreatePhysics failed");
        return false;
    }
    g_dispatcher = PxDefaultCpuDispatcherCreate(PxU32(p.threads > 0 ? p.threads : 1));
    PxSceneDesc sd(g_physics->getTolerancesScale());
    sd.gravity = PxVec3(0.0f, p.gravity, 0.0f);
    sd.cpuDispatcher = g_dispatcher;
    sd.filterShader = FilterAll;
    sd.simulationEventCallback = p.events;
    g_reportImpacts = p.events != nullptr && p.reportContacts;
    g_noKinematicContacts = p.noKinematicContacts;
    sd.broadPhaseType = PxBroadPhaseType::ePABP;
    sd.solverType = p.tgs ? PxSolverType::eTGS : PxSolverType::ePGS;
    sd.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_ACTIVE_ACTORS;  // (5.12: patch friction only)
    if (p.stabilization) sd.flags |= PxSceneFlag::eENABLE_STABILIZATION;
    if (p.killKinematicPairs) sd.kineKineFilteringMode = sd.staticKineFilteringMode = PxPairFilteringMode::eKILL;
    sd.sceneQueryUpdateMode = PxSceneQueryUpdateMode::eBUILD_DISABLED_COMMIT_DISABLED;  // nothing raycasts this scene
    g_gpu = false;
    if (p.gpu && Cuda(p.gpuSync)) {
        sd.cudaContextManager = g_cuda;
        sd.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS;
        if (p.gpuBroadphase) sd.broadPhaseType = PxBroadPhaseType::eGPU;
        PxGpuDynamicsMemoryConfig& m = sd.gpuDynamicsConfig;
        m.maxRigidContactCount = p.gpuContacts;
        m.maxRigidPatchCount = p.gpuPatches;
        m.foundLostPairsCapacity = p.gpuPairs;
        m.foundLostAggregatePairsCapacity = p.gpuPairs;
        m.totalAggregatePairsCapacity = p.gpuAggregatePairs;
        // nothing deformable or particle-based in this scene
        m.maxDeformableSurfaceContacts = m.maxDeformableVolumeContacts = m.maxParticleContacts = 1024;
        sd.gpuMaxNumPartitions = std::clamp(PxNextPowerOfTwo(std::max(p.gpuPartitions, 1u) - 1), 1u, 32u);
        g_gpu = true;
    }
    g_scene = g_physics->createScene(sd);
    if (!g_scene && g_gpu) {
        LOG("physx: createScene failed on the GPU, trying the CPU");
        sd.cudaContextManager = nullptr;
        sd.flags.clear(PxSceneFlag::eENABLE_GPU_DYNAMICS);
        sd.broadPhaseType = PxBroadPhaseType::ePABP;
        g_gpu = false;
        g_scene = g_physics->createScene(sd);
    }
    if (!g_scene) {
        LOG("physx: createScene failed");
        return false;
    }
    g_material = g_physics->createMaterial(p.staticFriction, p.dynamicFriction, p.restitution);
    LOG("physx: %d.%d.%d scene created (%s, %d CPU threads, %s, gravity %.2f, friction %.2f/%.2f, restitution %.2f)",
        PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX,
        !g_gpu ? "CPU" : sd.broadPhaseType == PxBroadPhaseType::eGPU ? "GPU dynamics + GPU broadphase" : "GPU dynamics",
        p.threads, p.tgs ? "TGS" : "PGS", p.gravity, p.staticFriction, p.dynamicFriction, p.restitution);
    if (g_gpu)
        LOG("physx: GPU buffers: %u contacts, %u patches, %u found/lost pairs, %u aggregate pairs, %u partitions",
            sd.gpuDynamicsConfig.maxRigidContactCount, sd.gpuDynamicsConfig.maxRigidPatchCount,
            sd.gpuDynamicsConfig.foundLostPairsCapacity, sd.gpuDynamicsConfig.totalAggregatePairsCapacity,
            sd.gpuMaxNumPartitions);
    return true;
}

void DestroyWorld() {
    if (g_scene) g_scene->release();
    g_scene = nullptr;
    if (g_material) g_material->release();
    g_material = nullptr;
    if (g_dispatcher) g_dispatcher->release();
    g_dispatcher = nullptr;
    g_gpu = false;
    // physics and foundation stay for the next level
}

PxPhysics* Physics() { return g_physics; }
PxScene* Scene() { return g_scene; }
PxMaterial* Material() { return g_material; }
bool GpuActive() { return g_scene && g_gpu; }
bool GpuBroadphase() { return g_scene && g_gpu && g_scene->getBroadPhaseType() == PxBroadPhaseType::eGPU; }
bool Failed() { return g_failed.load(); }

void AbandonWorld() {
    LOG("physx: the failed scene is abandoned%s", g_gpu ? "; the GPU is not used again in this session" : "");
    g_scene = nullptr;
    g_material = nullptr;
    g_dispatcher = nullptr;
    if (g_gpu) g_cuda = nullptr;  // (Cuda() does not try again)
    g_gpu = false;
    g_failed.store(false);
}

namespace {

// One slice of a ParallelFor; the dispatcher's worker runs it and then calls release().
class SliceTask final : public PxBaseTask {
public:
    const std::function<void(size_t, size_t)>* fn = nullptr;
    size_t begin = 0, end = 0;
    std::atomic<int>* left = nullptr;
    void run() override { (*fn)(begin, end); }
    const char* getName() const override { return "tdpx.slice"; }
    void addReference() override {}
    void removeReference() override {}
    int32_t getReference() const override { return 1; }
    void release() override { left->fetch_sub(1, std::memory_order_acq_rel); }
};
SliceTask g_slices[64];

}  // namespace

void ParallelFor(size_t n, size_t minSlice, const std::function<void(size_t, size_t)>& fn) {
    uint32_t workers = g_dispatcher ? g_dispatcher->getWorkerCount() : 0;
    minSlice = minSlice ? minSlice : 1;
    size_t slices = std::min<size_t>({size_t(workers) + 1, (n + minSlice - 1) / minSlice, std::size(g_slices)});
    if (slices < 2) {
        if (n) fn(0, n);
        return;
    }
    size_t per = (n + slices - 1) / slices;
    std::atomic<int> left{int(slices - 1)};
    for (size_t s = 1; s < slices; ++s) {
        SliceTask& t = g_slices[s];
        t.fn = &fn;
        t.begin = std::min(n, s * per);
        t.end = std::min(n, (s + 1) * per);
        t.left = &left;
        g_dispatcher->submitTask(t);
    }
    fn(0, std::min(n, per));  // this thread takes the first slice
    while (left.load(std::memory_order_acquire) > 0) _mm_pause();
}

}  // namespace px
