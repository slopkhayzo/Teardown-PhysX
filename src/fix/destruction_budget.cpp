#include "fix/destruction_budget.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"
#include "td/shape_events.h"
#include "trace/trace.h"

using td::Addr;

namespace fix {
namespace {

// The callers of MakeHoleCore in 2.2.0.20261118, as return addresses: Physics::update's impact damage (after the
// solve, once per body hit in the tick, with the engine's no-push descriptor and sound; nothing after the call reads
// what it did), the MakeHole wrapper (Lua MakeHole, projectiles, vehicles: it passes its body argument through) and
// explosions (0x1401a1c40: an impulse on its stack).
constexpr uintptr_t kImpactReturn = 0xbacc3;
constexpr uintptr_t kWrapperReturn = 0x13fc43;
constexpr uintptr_t kExplosionReturn = 0x1a330c;

#pragma pack(push, 1)
struct Sphere {
    float pos[3];
    float r[3];  // radii for soft, medium and hard materials
};
#pragma pack(pop)
static_assert(sizeof(Sphere) == 0x18, "MakeHoleCore's sphere is 0x18 bytes");
struct HoleList {  // the engine's list
    int count, cap;
    Sphere* data;
};
using MakeHoleCoreFn = void(__fastcall*)(void* world, uint8_t* body, HoleList* holes, uint8_t sound,
                                         const void* impulse);
MakeHoleCoreFn oMakeHole = nullptr;
// Lua MakeHole(pos, r0, r1, r2, [silent]) (0x14048ef50): (script, args {lua_State*, argument count}, results); it
// calls the wrapper with no body (every shape in range) and the no-push descriptor, and returns the destroyed-voxel
// counter's difference.
using LuaFn = void(__fastcall*)(void* script, void* args, void* ret);
LuaFn oLuaMakeHole = nullptr;

enum Source : uint8_t { kGameImpact, kPhysxImpact, kScript, kSources };
// who made a hole at once (by the call's return address), and the holes made from the queue
enum Caller { kCGameImpact, kCWrapper, kCExplosion, kCPlugin, kCOther, kCQueue, kCallers };

struct Entry {
    uint8_t* body;  // null: every shape in range (a script's hole)
    uint32_t gen;   // the body's generation when queued (g_gen)
    uint32_t tick;  // the tick it was queued in
    uint8_t sound;
    Source source;
    std::vector<Sphere> spheres;  // positions in the body's frame (world space without a body)
};

bool g_on = false;
bool g_impactSite = false;  // the impact damage call is where this build has it
bool g_scripts = false;     // scripts' holes may wait ([Fix] DestructionBudgetScripts and the binding's hook)
DWORD g_main = 0;
const void* g_noImpulse = nullptr;
double g_budget = 0.0;
uint32_t g_maxWait = 30;
double g_msPerQpc = 0.0;
uintptr_t g_mine[2] = {};  // the plugin's image
thread_local bool t_scriptMayWait = false;  // inside Lua MakeHole, whose caller drops the result

std::deque<Entry> g_queue;  // main thread
std::atomic<int> g_queued{0};
// Bodies that have holes queued: how many times a body at that address was destroyed since. A queued hole is made
// only on the body it was queued for (the same generation), not on a new body that got the same address.
std::unordered_map<uint8_t*, uint32_t> g_gen;
SRWLOCK g_genLock = SRWLOCK_INIT;
uint32_t g_tick = 0;
double g_frameMs = 0.0;  // MakeHoleCore time on the main thread in this frame
int g_frameMade = 0;     // queued holes made in this frame

struct Stats {
    uint64_t calls[kCallers] = {};
    double ms[kCallers] = {};
    uint64_t queued[kSources] = {};
    uint64_t overdue = 0, dead = 0, dropped = 0, waitTicks = 0;
    uint64_t luaDropped = 0, luaUsed = 0;  // Lua MakeHole calls whose caller drops / keeps the count
    uint32_t maxWaited = 0;
    size_t maxQueue = 0;
    uint64_t frames = 0, overFrames = 0;  // frames with holes; whose holes took more than the budget
    uint64_t hist[5] = {};                // frames whose holes took <2, <4, <8, <16, more ms
    double maxFrameMs = 0.0;
} g_st;

bool Multiplayer() {
    uint8_t* g = td::GameObj();
    uint8_t* net = g ? td::At<uint8_t*>(g, 0x160) : nullptr;
    return !net || td::At<int>(net, 4) != 0;
}

// q = (x, y, z, w): v + w t + q.xyz x t, t = 2 q.xyz x v
void Rotate(const float* q, const float* v, float* out) {
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}
// The body's transform: +0x40 position, +0x4c rotation (x, y, z, w).
void ToBody(uint8_t* body, float* p) {
    const float* bp = &td::At<float>(body, 0x40);
    const float* bq = &td::At<float>(body, 0x4c);
    const float d[3] = {p[0] - bp[0], p[1] - bp[1], p[2] - bp[2]};
    const float inv[4] = {-bq[0], -bq[1], -bq[2], bq[3]};
    Rotate(inv, d, p);
}
void ToWorld(uint8_t* body, float* p) {
    const float* bp = &td::At<float>(body, 0x40);
    const float* bq = &td::At<float>(body, 0x4c);
    float r[3];
    Rotate(bq, p, r);
    for (int i = 0; i < 3; ++i) p[i] = r[i] + bp[i];
}

// The game's Lua is 5.1 (statically linked; lua_State and CallInfo as in the reference implementation, seen in its
// index2adr 0x140623e90): L->ci (+0x28) is the running C function's CallInfo, whose nresults (+0x20) is how many
// results its caller wants: 0 for a call made as a statement, which drops MakeHole's count (-1 for all of them: a
// call that is the last argument of another, a return, a pcall). Checked: ci within [base_ci +0x50, end_ci +0x48) on
// a CallInfo boundary (0x28 bytes), and its base the stack base L->base (+0x18), as for a running C function.
bool CallerDropsResult(const uint8_t* L) {
    __try {
        const uint8_t* ci = *reinterpret_cast<const uint8_t* const*>(L + 0x28);
        const uint8_t* endCi = *reinterpret_cast<const uint8_t* const*>(L + 0x48);
        const uint8_t* baseCi = *reinterpret_cast<const uint8_t* const*>(L + 0x50);
        if (!ci || !baseCi || ci < baseCi || ci >= endCi || (ci - baseCi) % 0x28 != 0) return false;
        if (*reinterpret_cast<const uint8_t* const*>(ci) != *reinterpret_cast<const uint8_t* const*>(L + 0x18))
            return false;
        return *reinterpret_cast<const int*>(ci + 0x20) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The engine's call, timed: trace arg = voxels removed | spheres << 32 | from the queue << 62 | a target body << 63.
double Call(void* world, uint8_t* body, HoleList* holes, uint8_t sound, const void* impulse, Caller who) {
    const int before = td::DestroyedVoxels();
    const uint64_t t0 = trace::Now();
    oMakeHole(world, body, holes, sound, impulse);
    const uint64_t t1 = trace::Now();
    const double ms = double(t1 - t0) * g_msPerQpc;
    if (GetCurrentThreadId() == g_main) g_frameMs += ms;
    ++g_st.calls[who];
    g_st.ms[who] += ms;
    const uint64_t spheres = holes ? uint64_t(uint32_t(holes->count) & 0x3fffffffu) : 0;
    trace::Emit(trace::kMakeHoleCore, t0, t1,
                uint32_t(td::DestroyedVoxels() - before) | (spheres << 32) | (uint64_t(who == kCQueue) << 62) |
                    (uint64_t(body != nullptr) << 63));
    return ms;
}

bool Queue(uint8_t* body, const Sphere* s, int n, uint8_t sound, Source source) {
    if (!s || n <= 0) return false;
    Entry e{body, 0, g_tick, sound, source, std::vector<Sphere>(s, s + n)};
    if (body) {
        for (Sphere& sp : e.spheres) ToBody(body, sp.pos);
        AcquireSRWLockExclusive(&g_genLock);
        e.gen = g_gen[body];
        ReleaseSRWLockExclusive(&g_genLock);
    }
    g_queue.push_back(std::move(e));
    g_queued.store(int(g_queue.size()), std::memory_order_release);
    ++g_st.queued[source];
    g_st.maxQueue = std::max(g_st.maxQueue, g_queue.size());
    const uint64_t now = trace::Now();
    trace::Emit(trace::kHoleQueued, now, now, uint64_t(source) | (uint64_t(uint32_t(n)) << 8));
    return true;
}

void OnBodyDestroyed(uint8_t* body, uintptr_t) {
    if (g_queued.load(std::memory_order_acquire) == 0) return;
    AcquireSRWLockExclusive(&g_genLock);
    auto it = g_gen.find(body);
    if (it != g_gen.end()) ++it->second;
    ReleaseSRWLockExclusive(&g_genLock);
}

void __fastcall HkMakeHole(void* world, uint8_t* body, HoleList* holes, uint8_t sound, const void* impulse) {
    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uintptr_t ret = ra - td::Base();
    const bool over = g_frameMs >= g_budget;
    const bool usable = holes && holes->count > 0 && holes->data && impulse == g_noImpulse;
    Caller who = kCOther;
    if (ret == kImpactReturn) {
        who = kCGameImpact;
        if (over && usable && g_impactSite && body && GetCurrentThreadId() == g_main && !Multiplayer() &&
            Queue(body, holes->data, holes->count, sound, kGameImpact))
            return;
    } else if (ret == kWrapperReturn) {
        who = kCWrapper;
        // (t_scriptMayWait: inside Lua MakeHole on the main thread, outside multiplayer)
        if (over && usable && t_scriptMayWait && !body && Queue(nullptr, holes->data, holes->count, sound, kScript))
            return;
    } else if (ret == kExplosionReturn) {
        who = kCExplosion;
    } else if (ra >= g_mine[0] && ra < g_mine[1]) {
        who = kCPlugin;
    }
    Call(world, body, holes, sound, impulse, who);
}

void __fastcall HkLuaMakeHole(void* script, void* args, void* ret) {
    bool may = false;
    if (g_scripts && args && GetCurrentThreadId() == g_main) {
        ++crash::t_tolerateFaults;
        const bool drops = CallerDropsResult(*static_cast<const uint8_t* const*>(args));
        --crash::t_tolerateFaults;
        ++(drops ? g_st.luaDropped : g_st.luaUsed);
        may = drops && !Multiplayer();
    }
    t_scriptMayWait = may;
    oLuaMakeHole(script, args, ret);
    t_scriptMayWait = false;
}

}  // namespace

bool InstallDestructionBudget() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_msPerQpc = 1000.0 / double(f.QuadPart);
    g_budget = std::max(0.0, double(g_config.destructionBudgetMs));
    g_maxWait = uint32_t(std::max(1, g_config.destructionMaxWaitTicks));
    HMODULE mine = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&HkMakeHole), &mine) && mine) {
        const uintptr_t b = reinterpret_cast<uintptr_t>(mine);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
        g_mine[0] = b;
        g_mine[1] = b + nt->OptionalHeader.SizeOfImage;
    }
    if (!td::CodeMatches(Addr::MakeHoleCore)) {
        LOG("fix: destruction budget: MakeHoleCore's code differs, not installed");
        return false;
    }
    // the impact damage call: E8 rel32 to MakeHoleCore, returning to kImpactReturn
    const uint8_t* call = reinterpret_cast<const uint8_t*>(td::Base() + kImpactReturn - 5);
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    g_impactSite = call[0] == 0xe8 && td::Base() + kImpactReturn + intptr_t(rel) == td::VA(Addr::MakeHoleCore);
    g_noImpulse = reinterpret_cast<const void*>(td::VA(Addr::NoImpulseDesc));
    if (!td::OnBodyDestroyed(&OnBodyDestroyed)) {
        LOG("fix: destruction budget: no body destructor events, not installed");
        return false;
    }
    const MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(td::VA(Addr::MakeHoleCore)),
                                       reinterpret_cast<void*>(&HkMakeHole), reinterpret_cast<void**>(&oMakeHole));
    if (st != MH_OK) {
        LOG("fix: destruction budget: hook on MakeHoleCore: %s", MH_StatusToString(st));
        return false;
    }
    // scripts' holes: the binding, and the wrapper's call into MakeHoleCore where this build has it
    if (g_config.destructionBudgetScripts) {
        const uint8_t* wcall = reinterpret_cast<const uint8_t*>(td::Base() + kWrapperReturn - 5);
        std::memcpy(&rel, wcall + 1, 4);
        g_scripts = wcall[0] == 0xe8 && td::Base() + kWrapperReturn + intptr_t(rel) == td::VA(Addr::MakeHoleCore) &&
                    td::CodeMatches(Addr::LuaMakeHole) &&
                    MH_CreateHook(reinterpret_cast<void*>(td::VA(Addr::LuaMakeHole)),
                                  reinterpret_cast<void*>(&HkLuaMakeHole),
                                  reinterpret_cast<void**>(&oLuaMakeHole)) == MH_OK;
    }
    trace::RegisterName(trace::kMakeHoleCore, "makehole_core");
    trace::RegisterName(trace::kHoleQueued, "hole_queued");
    g_on = true;
    LOG("fix: destruction budget: %.1f ms of holes a frame, then impact damage, the debris tool's cuts%s wait (at "
        "most %u ticks); the game's impact damage %s",
        g_budget, g_scripts ? " and scripts' holes whose count the script drops" : " (not scripts' holes)", g_maxWait,
        g_impactSite ? "can wait" : "is made at once (its call differs in this build)");
    return true;
}

bool DestructionBudgetOn() { return g_on; }

void DestructionFrameStart() {
    if (!g_on) return;
    if (!g_main) g_main = GetCurrentThreadId();
    if (g_frameMs > 0.0) {
        ++g_st.frames;
        if (g_frameMs > g_budget) ++g_st.overFrames;
        g_st.maxFrameMs = std::max(g_st.maxFrameMs, g_frameMs);
        ++g_st.hist[g_frameMs < 2.0 ? 0 : g_frameMs < 4.0 ? 1 : g_frameMs < 8.0 ? 2 : g_frameMs < 16.0 ? 3 : 4];
    }
    g_frameMs = 0.0;
    g_frameMade = 0;
}

void DestructionTick() {
    if (!g_on) return;
    ++g_tick;
    if (g_queue.empty()) return;
    uint8_t* scene = td::SceneObj();
    void* world = scene ? td::At<void*>(scene, 0x10) : nullptr;
    if (!world || td::State() != td::kPlay) return;
    while (!g_queue.empty()) {
        const uint32_t waited = g_tick - g_queue.front().tick;
        const bool overdue = waited >= g_maxWait;
        // At least one a frame, so they keep coming while the holes that can't wait spend the budget by themselves;
        // not in a frame already twice over it (those that waited too long are made regardless).
        if (!overdue && g_frameMs >= g_budget && (g_frameMade > 0 || g_frameMs >= 2.0 * g_budget)) break;
        Entry e = std::move(g_queue.front());
        g_queue.pop_front();
        g_queued.store(int(g_queue.size()), std::memory_order_release);
        if (e.body) {
            AcquireSRWLockShared(&g_genLock);
            auto it = g_gen.find(e.body);
            const bool alive = it != g_gen.end() && it->second == e.gen;
            ReleaseSRWLockShared(&g_genLock);
            if (!alive) {
                ++g_st.dead;
                continue;
            }
            for (Sphere& sp : e.spheres) ToWorld(e.body, sp.pos);
        }
        HoleList list{int(e.spheres.size()), int(e.spheres.size()), e.spheres.data()};
        Call(world, e.body, &list, e.sound, g_noImpulse, kCQueue);
        ++g_frameMade;
        g_st.waitTicks += waited;
        g_st.maxWaited = std::max(g_st.maxWaited, waited);
        if (overdue) ++g_st.overdue;
    }
    if (g_queue.empty()) {
        AcquireSRWLockExclusive(&g_genLock);
        g_gen.clear();
        ReleaseSRWLockExclusive(&g_genLock);
    }
}

void DestructionLevelChange() {
    if (!g_on) return;
    g_st.dropped += g_queue.size();
    g_queue.clear();
    g_queued.store(0, std::memory_order_release);
    AcquireSRWLockExclusive(&g_genLock);
    g_gen.clear();
    ReleaseSRWLockExclusive(&g_genLock);
}

double DestructionBudgetLeftMs() {
    if (!g_on || Multiplayer()) return 1e9;
    return g_budget - g_frameMs;
}

bool DestructionDefer(uint8_t* body, const void* spheres, int count, bool sound) {
    if (!g_on || !body || GetCurrentThreadId() != g_main || Multiplayer()) return false;
    return Queue(body, static_cast<const Sphere*>(spheres), count, sound ? 1 : 0, kPhysxImpact);
}

void LogDestructionBudgetStats() {
    if (!g_on) return;
    const Stats& s = g_st;
    const uint64_t made = s.calls[kCQueue];
    LOG("destruction budget (%.1f ms a frame): %llu frames made holes, %llu of them for longer than the budget (the "
        "longest %.1f ms); frames by hole time <2/<4/<8/<16/more ms: %llu/%llu/%llu/%llu/%llu",
        g_budget, s.frames, s.overFrames, s.maxFrameMs, s.hist[0], s.hist[1], s.hist[2], s.hist[3], s.hist[4]);
    LOG("destruction budget: made at once: the game's impact damage %llu (%.0f ms), MakeHole (scripts, projectiles) "
        "%llu (%.0f ms), explosions %llu (%.0f ms), the plugin's (PhysX impacts, debris tool) %llu (%.0f ms), other "
        "%llu (%.0f ms); queued: the game's impact damage %llu, PhysX's %llu, scripts' %llu (Lua MakeHole: %llu calls "
        "dropped the count, %llu used it); made from the queue %llu (%.0f ms, waited %.1f ticks on average, at most "
        "%u; %llu after waiting %u ticks); %llu dropped with their body, %llu with the level; at most %zu waiting",
        s.calls[kCGameImpact], s.ms[kCGameImpact], s.calls[kCWrapper], s.ms[kCWrapper], s.calls[kCExplosion],
        s.ms[kCExplosion], s.calls[kCPlugin], s.ms[kCPlugin], s.calls[kCOther], s.ms[kCOther], s.queued[kGameImpact],
        s.queued[kPhysxImpact], s.queued[kScript], s.luaDropped, s.luaUsed, made, s.ms[kCQueue],
        made ? double(s.waitTicks) / double(made) : 0.0, s.maxWaited, s.overdue, g_maxWait, s.dead, s.dropped,
        s.maxQueue);
}

}  // namespace fix
