// The PhysX side: one foundation/physics/scene for the plugin's lifetime per level, a CPU dispatcher with its own
// threads, one default material, PhysX errors in plugin.log. Created when a level starts (first tick in PLAY),
// released when the game leaves PLAY.
#pragma once

#include <cstddef>
#include <functional>

namespace physx {
class PxPhysics;
class PxScene;
class PxMaterial;
class PxCpuDispatcher;
class PxSimulationEventCallback;
}  // namespace physx

namespace px {

struct WorldParams {
    int threads = 4;
    float gravity = -10.0f;
    float staticFriction = 0.6f, dynamicFriction = 0.6f, restitution = 0.1f;
    bool tgs = false;  // TGS solver instead of PGS
    bool stabilization = false;
    // kinematic-kinematic and kinematic-static pairs dropped in the broadphase (else kept, without contacts)
    bool killKinematicPairs = false;
    // contact reports: pairs with a moving (non-kinematic) dynamic actor report contacts whose force passes that
    // actor's PxRigidBody::setContactReportThreshold, with contact points (impact sounds and damage)
    physx::PxSimulationEventCallback* events = nullptr;
    bool reportContacts = false;  // contact reports for `events` (sleep and wake events come regardless)
    bool noKinematicContacts = false;  // diagnostics: kinematic actors (proxies, the player) touch no debris
    // GPU rigid bodies (CUDA): needs an NVIDIA GPU and PhysXGpu_64.dll next to the plugin; without them the scene
    // runs on the CPU. Buffer sizes for the GPU pipeline (PxGpuDynamicsMemoryConfig; PhysX logs when one is short).
    bool gpu = false;
    bool gpuBroadphase = true;   // the broadphase on the GPU too (PxBroadPhaseType::eGPU), else PABP on the CPU
    unsigned gpuContacts = 1u << 20, gpuPatches = 1u << 19;      // contact points / patches per step
    unsigned gpuPairs = 1u << 18, gpuAggregatePairs = 1u << 20;  // broadphase found/lost pairs; static blocks' pairs
    unsigned gpuPartitions = 8;  // solver partitions (power of two, at most 32)
    bool gpuSync = false;        // synchronous kernel launches (debugging; only the first scene's setting counts)
};

// Filter data of the PhysX shapes made from the game's shapes: word0 its collision layer, word1 its mask, word2 its
// group, word3 these flags. The scene's filter shader pairs them as the game's broadphase does (0x1400ad920): layer
// and mask must match both ways, and shapes in the same non-zero group never collide. Shapes without kFilterGame
// (the player's capsule) collide with everything.
constexpr unsigned kFilterGame = 1;
constexpr unsigned kFilterStaticOnly = 2;  // meets static geometry only (the game's small debris, flag 0x08)

bool CreateWorld(const WorldParams& p);
void DestroyWorld();
physx::PxPhysics* Physics();
physx::PxScene* Scene();
physx::PxMaterial* Material();
bool GpuActive();  // the current scene simulates on the GPU (convex meshes need GPU data)
bool GpuBroadphase();  // ... and its broadphase runs there too
// PhysX reported that it cannot go on (a CUDA error puts the GPU pipeline in abort mode for good).
bool Failed();
// Forgets a failed scene without touching it (releasing it would run into the broken GPU state); the next
// CreateWorld makes a new one, on the CPU.
void AbandonWorld();

// fn(begin, end) over slices of [0, n): on PhysX's dispatcher threads (idle between steps) and the calling thread.
// Main thread only, and never while the scene simulates. Small n runs inline.
void ParallelFor(size_t n, size_t minSlice, const std::function<void(size_t, size_t)>& fn);

}  // namespace px
