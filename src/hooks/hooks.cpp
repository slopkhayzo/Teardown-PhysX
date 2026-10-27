#include "hooks/hooks.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include "bench/bench.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "fix/serial_jobs.h"
#include "fix/touch_cache.h"
#include "fix/keep_debris.h"
#include "fix/split_jobs.h"
#include "fix/static_body_update.h"
#include "fix/visible_early_out.h"
#include "input/recorder.h"
#include "px/shadow.h"
#include "trace/sampler.h"
#include "trace/threads.h"
#include "td/addresses.h"
#include "td/game.h"
#include "trace/census.h"
#include "trace/trace.h"

using td::Addr;

namespace {

struct HookCtx;
using PreFn = void (*)(HookCtx*, uint64_t* gprs, void* xmms, uint64_t* stackArgs);
using PostFn = uint64_t (*)(HookCtx*, uint64_t arg);

struct HookCtx {
    void* original;  // +0: called by TdpxTimingThunk
    uint32_t id;
    Addr addr;
    PreFn pre;
    PostFn post;
};

constexpr int kMaxTimed = 128;
HookCtx g_ctx[kMaxTimed];
int g_ctxCount = 0;
uint8_t* g_stubs = nullptr;
thread_local uint64_t t_arg[kMaxTimed + 1];

}  // namespace

extern "C" void TdpxTimingThunk();

extern "C" uint64_t TdpxThunkEnter(HookCtx* ctx, uint64_t* gprs, void* xmms, uint64_t* stackArgs) {
    t_arg[ctx->id] = 0;
    if (ctx->pre) ctx->pre(ctx, gprs, xmms, stackArgs);
    return trace::Now();
}

extern "C" void TdpxThunkLeave(HookCtx* ctx, uint64_t start) {
    uint64_t end = trace::Now();
    uint64_t arg = t_arg[ctx->id];
    if (ctx->post) arg = ctx->post(ctx, arg);
    trace::Emit(ctx->id, start, end, arg);
}

namespace {

// ---- per-hook argument capture -------------------------------------------------------------------------

void PreFrame(HookCtx*, uint64_t*, void*, uint64_t*) {
    static bool mainSet = false;
    if (!mainSet) {
        mainSet = true;
        fix::SetMainThread(GetCurrentThreadId());
        sampler::SetMainThread(GetCurrentThreadId());
        threads::SetMainThread(GetCurrentThreadId());
    }
    crash::Heartbeat();
    inputrec::OnFrameStart();
    bench::OnFrame();
}

// The engine's log(level, fmt, ...): mirror each line into plugin.log (the game's own log.txt is only written at
// a normal exit). x64 varargs live in r8, r9, then the stack, 8 bytes each, which is exactly a va_list.
void PreGameLog(HookCtx*, uint64_t* g, void*, uint64_t* st) {
    const char* fmt = reinterpret_cast<const char*>(g[1]);
    uintptr_t base = td::Base();
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    uintptr_t f = reinterpret_cast<uintptr_t>(fmt);
    if (f < base || f >= base + nt->OptionalHeader.SizeOfImage) return;
    uint64_t slots[18];
    slots[0] = g[2];
    slots[1] = g[3];
    std::memcpy(slots + 2, st, 16 * sizeof(uint64_t));
    char buf[1024];
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, reinterpret_cast<va_list>(slots));
    size_t n = std::strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    LOG("game: %s", buf);
}
uint64_t PostFrame(HookCtx*, uint64_t) {
    return uint32_t(td::State()) | (uint64_t(uint32_t(td::BodyCount())) << 32);
}

void PreSceneUpdate(HookCtx*, uint64_t*, void*, uint64_t*) {
    inputrec::OnTick();
    census::OnTick();
    bench::OnTick();
}
uint64_t PostSceneUpdate(HookCtx*, uint64_t) {
    return uint32_t(td::BodyCount()) | (uint64_t(uint32_t(td::ShapeCount())) << 32);
}

void PrePhysics(HookCtx*, uint64_t*, void*, uint64_t*) { shadow::OnPhysicsPre(); }
uint64_t PostPhysics(HookCtx*, uint64_t) {
    uint64_t simulated = uint32_t(td::SimulatedBodies());
    shadow::OnPhysicsPost();
    return simulated;
}

// MakeHoleCore(world, body, sphereList*, sound, impulse): arg = voxels removed | spheres << 32 | impact << 63.
thread_local int t_mhBefore;
void PreMakeHole(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    t_mhBefore = td::DestroyedVoxels();
    const int* list = reinterpret_cast<const int*>(g[2]);
    uint64_t spheres = list ? uint32_t(list[0]) : 0;
    // a target body: impact damage (Physics::update, the plugin's PhysX contacts) and the plugin's debris tool
    uint64_t impact = g[1] != 0 ? 1ull : 0ull;
    t_arg[c->id] = (spheres << 32) | (impact << 63);
}
uint64_t PostMakeHole(HookCtx*, uint64_t arg) {
    return arg | uint32_t(td::DestroyedVoxels() - t_mhBefore);
}

// DestrCarve(shape records {int count, ...}, holes {int count, ...}): arg = shapes | holes << 32. Every hole meets
// every gathered shape (a 960-byte record per pair), so the pairs are the product.
void PreDestrPairs(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    const int* shapes = reinterpret_cast<const int*>(g[0]);
    const int* holes = reinterpret_cast<const int*>(g[1]);
    t_arg[c->id] = uint64_t(shapes ? uint32_t(shapes[0]) : 0) | (uint64_t(holes ? uint32_t(holes[0]) : 0) << 32);
}

// Functions whose first argument is a list {int count, ...}: arg = count.
void PreListCount(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    const int* list = reinterpret_cast<const int*>(g[0]);
    t_arg[c->id] = list ? uint32_t(list[0]) : 0;
}

// BreakageDriver(list {int count; int cap; entries*}, ...): every listed shape is about to be modified.
void PreBreakageDriver(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    const int* list = reinterpret_cast<const int*>(g[0]);
    if (!list) return;
    t_arg[c->id] = uint32_t(list[0]);
    const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(reinterpret_cast<const uint8_t*>(list) + 8);
    if (g_config.touchCache) fix::OnShapesModified(entries, list[0]);
    if (g_config.pxShadow) shadow::OnShapesModified(entries, list[0]);
}

// Functions whose first argument is a Voxels*: arg = voxel volume (dx*dy*dz).
void PreVoxVolume(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    t_arg[c->id] = td::VoxVolume(reinterpret_cast<uint8_t*>(g[0]));
}
// First argument points at a struct whose first field is the Voxels*.
void PreVoxVolumeIndirect(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    auto p = reinterpret_cast<uint8_t**>(g[0]);
    t_arg[c->id] = p ? td::VoxVolume(*p) : 0;
}
// VoxRegionUpdate(vox, z0, z1, x0, y0, x1, y1): arg = region volume | shape volume << 32 (saturated).
void PreVoxRegion(HookCtx* c, uint64_t* g, void*, uint64_t* st) {
    int z0 = int(g[1]), z1 = int(g[2]), x0 = int(g[3]);
    int y0 = int(st[0]), x1 = int(st[1]), y1 = int(st[2]);
    int64_t vol = int64_t(z1 - z0) * (x1 - x0 + 1) * (y1 - y0 + 1);
    if (vol < 0 || vol > 0xffffffffll) vol = 0;
    uint64_t sv = td::VoxVolume(reinterpret_cast<uint8_t*>(g[0]));
    if (sv > 0xffffffffull) sv = 0xffffffffull;
    t_arg[c->id] = uint64_t(vol) | (sv << 32);
}

// ShapeTouchTest(physics, shapeA, shapeB): arg = order-independent hash of the pair (to count unique pairs).
void PreTouchPair(HookCtx* c, uint64_t* g, void*, uint64_t*) {
    uint64_t a = g[1], b = g[2];
    if (a > b) std::swap(a, b);
    uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull + (a << 6) + (a >> 2));
    t_arg[c->id] = h;
}

struct HookSpec {
    Addr addr;
    PreFn pre;
    PostFn post;
};

const HookSpec kSpecs[] = {
    // FrameUpdate runs once per frame in every state (FrameDriver does not run in the splash/menu loop).
    {Addr::FrameUpdate, PreFrame, PostFrame},
    {Addr::SceneUpdate, PreSceneUpdate, PostSceneUpdate},
    {Addr::PhysicsUpdate, PrePhysics, PostPhysics},
    {Addr::MakeHoleCore, PreMakeHole, PostMakeHole},
    {Addr::BreakageDriver, PreBreakageDriver, nullptr},
    {Addr::DestrCarve, PreDestrPairs, nullptr},
    {Addr::BreakIslands, PreListCount, nullptr},
    {Addr::BreakIslandsMT, PreListCount, nullptr},
    {Addr::DestrFinish, PreListCount, nullptr},
    {Addr::VoxBoundsScan, PreVoxVolume, nullptr},
    {Addr::VoxWholeA, PreVoxVolume, nullptr},
    {Addr::VoxWholeB, PreVoxVolume, nullptr},
    {Addr::VoxRegionUpdate, PreVoxRegion, nullptr},
    {Addr::VoxRegionFinish, PreVoxVolume, nullptr},
    {Addr::VoxRegionCcl, PreVoxVolumeIndirect, nullptr},
    {Addr::ShapeTouchTest, PreTouchPair, nullptr},
    {Addr::GameLog, PreGameLog, nullptr},
};

const HookSpec* FindSpec(Addr a) {
    for (const HookSpec& s : kSpecs)
        if (s.addr == a) return &s;
    return nullptr;
}

void* MakeStub(HookCtx* ctx) {
    if (!g_stubs) {
        g_stubs = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kMaxTimed * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_stubs) return nullptr;
    }
    uint8_t* p = g_stubs + (ctx - g_ctx) * 32;
    uint64_t c = reinterpret_cast<uint64_t>(ctx);
    uint64_t t = reinterpret_cast<uint64_t>(&TdpxTimingThunk);
    p[0] = 0x49; p[1] = 0xBA; std::memcpy(p + 2, &c, 8);    // mov r10, ctx
    p[10] = 0x49; p[11] = 0xBB; std::memcpy(p + 12, &t, 8); // mov r11, TdpxTimingThunk
    p[20] = 0x41; p[21] = 0xFF; p[22] = 0xE3;                // jmp r11
    FlushInstructionCache(GetCurrentProcess(), p, 32);
    return p;
}

// ---- typed hooks --------------------------------------------------------------------------------------

using ProfilerAddFn = void(__fastcall*)(void* profiler, int category, float seconds);
ProfilerAddFn oProfilerAdd = nullptr;
void __fastcall HkProfilerAdd(void* profiler, int category, float seconds) {
    oProfilerAdd(profiler, category, seconds);
    uint32_t bits;
    std::memcpy(&bits, &seconds, 4);
    uint64_t now = trace::Now();
    trace::Emit(trace::kProfilerCategory, now, now, uint32_t(category) | (uint64_t(bits) << 32));
}

// One of the level descriptor's 32-byte strings (byte 31 set: on the heap), copied with faults tolerated.
void DescString(const uint8_t* s, char* out, size_t n) {
    out[0] = 0;
    ++crash::t_tolerateFaults;
    __try {
        const char* p = s[31] ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
        size_t i = 0;
        for (; i + 1 < n && p[i]; ++i) out[i] = p[i];
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lstrcpynA(out, "?", int(n));
    }
    --crash::t_tolerateFaults;
}

// Solid voxels of the static and of the dynamic shapes (browse sessions: how much there is to cut).
void CountVoxels(uint64_t& staticVox, uint64_t& dynamicVox) {
    staticVox = dynamicVox = 0;
    ++crash::t_tolerateFaults;
    __try {
        int n = 0;
        uint8_t** shapes = td::SceneList<uint8_t*>(0x238, n);
        for (int i = 0; shapes && i < n; ++i) {
            uint8_t* v = td::ShapeVoxels(shapes[i]);
            const uint8_t* d = v ? td::VoxData(v) : nullptr;
            if (!d) continue;
            uint64_t solid = 0, cells = td::VoxVolume(v);
            for (uint64_t k = 0; k < cells; ++k) solid += d[k] != 0;
            uint8_t* b = td::ShapeBody(shapes[i]);
            (b && td::BodyDynamic(b) ? dynamicVox : staticVox) += solid;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    --crash::t_tolerateFaults;
}

// The level now running, however it was started (menu, mods.play, the bench): StartLevel (0x140079b80) keeps its
// descriptor at Game+0x538 (strings: +0x20 mission, +0x40 path, +0x60 layers, +0x80 mod).
void LogLevel(void* game) {
    const uint8_t* d = static_cast<const uint8_t*>(game) + 0x538;
    char mission[128], path[512], layers[256], mod[128];
    DescString(d + 0x20, mission, sizeof(mission));
    DescString(d + 0x40, path, sizeof(path));
    DescString(d + 0x60, layers, sizeof(layers));
    DescString(d + 0x80, mod, sizeof(mod));
    LOG("level: mission=%s path=%s layers=%s mod=%s; bodies %d, shapes %d", mission, path, layers, mod,
        td::BodyCount(), td::ShapeCount());
    if (g_config.mode != "browse") return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    uint64_t t0 = trace::Now(), staticVox, dynamicVox;
    CountVoxels(staticVox, dynamicVox);
    LOG("level: %.1f M solid voxels in static shapes, %.1f M in dynamic ones (counted in %.0f ms)",
        double(staticVox) / 1e6, double(dynamicVox) / 1e6, double(trace::Now() - t0) * 1000.0 / double(f.QuadPart));
}

using StateSwitchFn = void(__fastcall*)(void* game, int state);
StateSwitchFn oStateSwitch = nullptr;
void __fastcall HkStateSwitch(void* game, int state) {
    if (state == td::kQuit && g_config.mode == "browse") {
        // ends the session the way every test run ends: no shutdown, nothing saved on the way out
        LOG("session: the game is quitting");
        bench::Quit(true);
    }
    int old = game ? td::At<int>(game, 0x08) : -1;
    inputrec::OnStateSwitching(old, state);
    if (old == td::kPlay && state != td::kPlay) shadow::OnLeavePlay();  // before the level's entities go away
    crash::SetWatch(false);
    uint64_t t0 = trace::Now();
    oStateSwitch(game, state);
    trace::Emit(trace::kStateSwitch, t0, trace::Now(), uint32_t(old) | (uint64_t(uint32_t(state)) << 32));
    LOG("state %s -> %s", td::StateName(old), td::StateName(state));
    if (state == td::kLoading || state == td::kMenuLoading) fix::OnLevelChange();
    if (old == td::kPlay && g_config.touchCache) fix::LogTouchCacheStats();
    if (old == td::kPlay && g_config.staticBodyUpdate) fix::LogStaticBodyUpdateStats();
    if (state == td::kPlay) {
        crash::SetWatch(true);
        LogLevel(game);
    }
    bench::OnState(old, state);
}

using SHGetSpecialFolderPathAFn = BOOL(WINAPI*)(HWND, LPSTR, int, BOOL);
SHGetSpecialFolderPathAFn oSHGetSpecialFolderPathA = nullptr;
BOOL WINAPI HkSHGetSpecialFolderPathA(HWND hwnd, LPSTR path, int csidl, BOOL create) {
    constexpr int kCsidlLocalAppData = 0x1c;
    if ((csidl & 0xff) == kCsidlLocalAppData && !g_config.appDataRoot.empty()) {
        lstrcpynA(path, g_config.appDataRoot.c_str(), MAX_PATH);
        static LONG logged = 0;
        if (InterlockedExchange(&logged, 1) == 0) LOG("sandbox: LocalAppData -> %s", path);
        return TRUE;
    }
    return oSHGetSpecialFolderPathA(hwnd, path, csidl, create);
}

bool CreateTyped(Addr a, void* detour, void** original) {
    if (!td::CodeMatches(a)) {
        LOG("hook %s: bytes differ, skipped", td::Info(a).name);
        return false;
    }
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(td::VA(a)), detour, original);
    if (st != MH_OK) LOG("hook %s: MH_CreateHook %s", td::Info(a).name, MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace

namespace hooks {

bool InstallApiHooks() {
    if (g_config.appDataRoot.empty()) return true;
    MH_STATUS st = MH_CreateHookApi(L"shell32", "SHGetSpecialFolderPathA", &HkSHGetSpecialFolderPathA,
                                    reinterpret_cast<void**>(&oSHGetSpecialFolderPathA));
    if (st == MH_OK) st = MH_EnableHook(MH_ALL_HOOKS);
    LOG("api hook SHGetSpecialFolderPathA: %s", MH_StatusToString(st));
    return st == MH_OK;
}

bool InstallEngineHooks() {
    int timed = 0, skipped = 0;
    for (int i = 0; i < static_cast<int>(Addr::Count); ++i) {
        const td::AddrInfo& info = td::kAddrs[i];
        if (!info.code || !info.label[0]) continue;
        Addr a = static_cast<Addr>(i);
        if (g_ctxCount >= kMaxTimed) break;
        if (a == Addr::ShapeTouchTest && g_config.touchCache) continue;  // the cache hook times it itself
        if (a == Addr::SplitNewBodyUpdate && g_config.staticBodyUpdate) continue;  // (so does the fix's)
        if (!td::CodeMatches(a)) {
            LOG("timed %s: bytes differ, skipped", info.name);
            ++skipped;
            continue;
        }
        HookCtx* ctx = &g_ctx[g_ctxCount];
        ctx->id = uint32_t(g_ctxCount + 1);
        ctx->addr = a;
        if (const HookSpec* s = FindSpec(a)) {
            ctx->pre = s->pre;
            ctx->post = s->post;
        }
        void* stub = MakeStub(ctx);
        MH_STATUS st = stub ? MH_CreateHook(reinterpret_cast<void*>(td::VA(a)), stub, &ctx->original)
                            : MH_ERROR_MEMORY_ALLOC;
        if (st != MH_OK) {
            LOG("timed %s: MH_CreateHook %s", info.name, MH_StatusToString(st));
            ++skipped;
            continue;
        }
        trace::RegisterName(ctx->id, info.label);
        ++g_ctxCount;
        ++timed;
    }
    CreateTyped(Addr::ProfilerAdd, reinterpret_cast<void*>(&HkProfilerAdd),
                reinterpret_cast<void**>(&oProfilerAdd));
    CreateTyped(Addr::StateSwitch, reinterpret_cast<void*>(&HkStateSwitch),
                reinterpret_cast<void**>(&oStateSwitch));
    if (g_config.touchCache) fix::InstallTouchCache();
    if (g_config.visibleEarlyOut) fix::InstallVisibleEarlyOut();
    if (g_config.staticBodyUpdate) fix::InstallStaticBodyUpdate();
    if (g_config.keepDebris) fix::InstallKeepDebris();
    if (g_config.splitJobsMin != 16) fix::InstallSplitJobsMin(g_config.splitJobsMin);
    if (g_config.mode == "record" || g_config.mode == "replay") inputrec::Install();
    serialjobs::InstallEngine();
    shadow::Install();
    MH_STATUS st = MH_EnableHook(MH_ALL_HOOKS);
    LOG("engine hooks: %d timed, %d skipped, enable: %s", timed, skipped, MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace hooks
