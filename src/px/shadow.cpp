#include "px/shadow.h"

#include <windows.h>

#include <intrin.h>

#include <MinHook.h>
#include <PxPhysicsAPI.h>

#include <algorithm>
#include <climits>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "px/colliders.h"
#include "px/world.h"
#include "td/addresses.h"
#include "td/game.h"
#include "td/shape_events.h"
#include "trace/gpu_kernels.h"
#include "trace/trace.h"

using namespace physx;
using td::Addr;

namespace shadow {

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kCell = 8.0f;  // static-shape grid cell (m)

#pragma pack(push, 1)
struct Tr {
    float p[3];
    float q[4];  // x, y, z, w
};
#pragma pack(pop)

struct Box3 {
    float mn[3], mx[3];
};
bool Overlap(const Box3& z, const float mn[3], const float mx[3]) {
    return z.mn[0] <= mx[0] && z.mn[1] <= mx[1] && z.mn[2] <= mx[2] && mn[0] <= z.mx[0] && mn[1] <= z.mx[1] &&
           mn[2] <= z.mx[2];
}

// ---- engine fields (research/docs/re-notes.md) -----------------------------------------------------------
inline Tr& BodyTr(uint8_t* b) { return td::At<Tr>(b, 0x40); }
inline Tr& BodyPredicted(uint8_t* b) { return td::At<Tr>(b, 0x78); }  // where the next step sweeps the body to
inline float* BodyVel(uint8_t* b) { return &td::At<float>(b, 0xd8); }
inline float* BodyAngVel(uint8_t* b) { return &td::At<float>(b, 0xe4); }
inline const float* BodyInvInertia(uint8_t* b) { return &td::At<float>(b, 0xf0); }
inline float BodyInvMass(uint8_t* b) { return td::At<float>(b, 0xfc); }
inline const float* BodyCom(uint8_t* b) { return &td::At<float>(b, 0x100); }
inline const float* BodyInertiaRot(uint8_t* b) { return &td::At<float>(b, 0x10c); }  // principal axes, body frame
inline bool BodyDynamic(uint8_t* b) { return td::At<uint8_t>(b, 0x138) != 0; }
inline uint8_t& SleepCounter(uint8_t* b) { return td::At<uint8_t>(b, 0x140); }  // 60 on wake, 0 = asleep
inline bool BodyAwake(uint8_t* b) { return td::At<uint8_t>(b, 0x140) != 0; }
inline bool BodyTouched(uint8_t* b) { return td::At<uint8_t>(b, 0x141) != 0; }  // contacts in the last native step
inline int Handle(uint8_t* e) { return td::At<int>(e, 0x0c); }
inline uint8_t* FirstShape(uint8_t* b) { return td::At<uint8_t*>(b, 0x28); }
inline uint8_t* NextShape(uint8_t* s) { return td::At<uint8_t*>(s, 0x18); }
inline uint8_t* ShapeBody(uint8_t* s) { return td::At<uint8_t*>(s, 0x10); }
inline const Tr& ShapeLocal(uint8_t* s) { return td::At<Tr>(s, 0x44); }
inline bool ShapePhysical(uint8_t* s) { return (td::ShapeFlags(s) & 0x10) != 0; }
inline bool ShapeJointed(uint8_t* s) { return td::At<void*>(s, 0xf8) != nullptr; }

// A shape's collision filter as PhysX filter data (world.h). As in the game's broadphase (0x1400ad920), a shape
// that isn't physical or belongs to a dynamic body without mass pairs with nothing (layer 0), and the small-debris
// flag 0x08 (static geometry only) counts unless [PhysX] SmallDebrisCollide keeps small debris colliding.
PxFilterData GameFilter(uint8_t* s) {
    const uint16_t flags = td::ShapeFlags(s);
    uint8_t* b = ShapeBody(s);
    PxU32 layer = td::At<uint8_t>(s, 0xca);
    if (!(flags & 0x10) || (b && BodyDynamic(b) && !(BodyInvMass(b) > 0.0f))) layer = 0;
    PxU32 tag = px::kFilterGame;
    if ((flags & 0x08) && !g_config.pxSmallDebrisCollide) tag |= px::kFilterStaticOnly;
    return PxFilterData(layer, td::At<uint8_t>(s, 0xcb), PxU32(td::At<int>(s, 0xcc)), tag);
}
inline uint32_t VoxSolid(uint8_t* v) { return td::At<uint32_t>(v, 0x5c); }

using ShapeWorldFn = td::Transform*(__fastcall*)(void* shape, td::Transform* out);
using BodyUpdateShapesFn = void(__fastcall*)(uint8_t* body, uint8_t swept, uint8_t updateTree);
using BodyWakeFn = void(__fastcall*)(uint8_t* body, uint8_t force);
using LuaArgIntFn = int(__fastcall*)(void* args, int index, int64_t def);
using LuaPushBoolFn = void(__fastcall*)(void* ret, bool value);
using MutexFn = void(__fastcall*)(void* mutex);
using SolverAddBodyFn = int(__fastcall*)(void* solver, const void* pose, const void* com, float invMass,
                                          const float* invInertia, float* vel, float* angVel, const float* massFrame,
                                          float f148, uint8_t b150, float f14c, uint8_t b151, float f144, int handle,
                                          void* body);

// The shapes' world AABBs from the body pose, and their AABB-tree nodes (the tree follows the sleep counter).
void UpdateShapes(uint8_t* b) { td::Fn<BodyUpdateShapesFn>(Addr::BodyUpdateShapes)(b, 0, 1); }
// Sleep counter to 60; a body that was asleep moves its shapes to the awake-body trees.
void WakeNative(uint8_t* b) { td::Fn<BodyWakeFn>(Addr::BodyWake)(b, 0); }

PxQuat Quat(const float* q) {
    PxQuat r(q[0], q[1], q[2], q[3]);
    float m = r.magnitudeSquared();
    if (!(m > 1e-12f) || !std::isfinite(m)) return PxQuat(PxIdentity);
    return r.getNormalized();
}
PxTransform ToPx(const Tr& t) { return PxTransform(PxVec3(t.p[0], t.p[1], t.p[2]), Quat(t.q)); }
PxVec3 V(const float* v) { return PxVec3(v[0], v[1], v[2]); }
PxTransform ShapeWorld(uint8_t* shape) {
    td::Transform t{};
    td::Fn<ShapeWorldFn>(Addr::ShapeWorldTransform)(shape, &t);
    return PxTransform(PxVec3(t.pos[0], t.pos[1], t.pos[2]), Quat(t.rot));
}
px::VoxGrid Grid(uint8_t* vox) {
    px::VoxGrid g;
    g.data = td::VoxData(vox);
    const int* n = td::VoxDims(vox);
    for (int k = 0; k < 3; ++k) g.n[k] = n[k];
    return g;
}
bool GridOk(const px::VoxGrid& g) { return g.data && g.n[0] > 0 && g.n[1] > 0 && g.n[2] > 0; }

// ---- tracked dynamic bodies --------------------------------------------------------------------------------
struct ShapeSig {
    uint8_t* shape;
    uint8_t* vox;
    uint32_t solid;
    int n[3];
    Tr local;
    PxFilterData filter;  // a script can change it (SetShapeCollisionFilter): the shape's colliders are rebuilt
};
bool Same(const ShapeSig& a, const ShapeSig& b) {
    return a.shape == b.shape && a.vox == b.vox && a.solid == b.solid && a.n[0] == b.n[0] && a.n[1] == b.n[1] &&
           a.n[2] == b.n[2] && std::memcmp(&a.local, &b.local, sizeof(Tr)) == 0 && a.filter == b.filter;
}

struct BodyEntry {
    PxRigidDynamic* actor = nullptr;  // null: offload is waiting for the body's static surroundings
    bool kinematic = false;
    std::vector<ShapeSig> sig;
    uint32_t boxes = 0, voxels = 0;
    uint32_t seen = 0;
    bool fresh = true;  // actor created this tick (free mode: copy the native state once)
    // native state before the step
    Tr pre{};
    float v[3]{}, w[3]{};
    bool awake = false;
    Tr placed{};  // proxies: the native pose last given to PhysX
    // last static query (world AABB, expanded) and whether all its blocks exist
    float qmn[3]{1e30f, 1e30f, 1e30f}, qmx[3]{-1e30f, -1e30f, -1e30f};
    bool staticsOk = false;
    // offload (Shadow=3)
    bool owned = false;       // PhysX simulates it; the game keeps it asleep
    bool treesDirty = false;  // its sleep counter was reset after the step's start: re-place its shapes
    bool bad = false;         // the watchdog saw a bad PhysX state: hand it back
    uint32_t holdUntil = 0;   // stays with the game until this tick
    Tr expTr{};               // the body as the last write-back left it (what the pre-step diff compares with)
    float expV[3]{}, expW[3]{};
    uint32_t ownedSince = 0;  // diagnostics
    uint32_t born = 0;        // tick the entry was made (diagnostics: age at death)
    // wedged pieces (convex hulls of concave pieces stuck in what they were cut from vibrate in place for ever and
    // keep their whole island awake): where it was a second ago
    PxVec3 anchor{0.0f};
    uint32_t anchorTick = 0;
    uint8_t stuck = 0;  // settle checks in a row it got nowhere
    // whether putting it to sleep lasts (its island has a piece that wasn't put to sleep, or something keeps waking
    // it: the player walking on the pile, a proxy)
    uint32_t sleptTick = 0;  // the settle check put it to sleep at this tick (0: it hasn't, since its last look)
    uint8_t retries = 0;     // put to sleep again at once since, as PhysX moved it again in the step after
    uint8_t rewakes = 0;     // checks in a row that happened with every retry
    bool held = false;       // so: not put to sleep before g_holdRelease (until it sleeps through such a step)
    uint8_t settled = 0;      // settle checks in a row it got nowhere or stayed slower than FreezeSpeed (Freeze)
    float maxSpeedWin = 0.0f; // fastest since the last settle check
    float lastForward = 0.0f; // speed a script/tool last set (diagnostics)
    uint32_t builtTick = 0;   // colliders last built (large proxies are rebuilt at most every 30 ticks)
    float radius = 0.0f;      // half the colliders' diagonal at the last build (a proxy's fastest point)
    float movedSpeed = 0.0f;  // diagnostics: a proxy's fastest point at its last move (m/s), and when
    uint32_t movedTick = 0;
    bool movedTeleport = false;
    PxTransform jiggleFrom{PxIdentity};  // a proxy's pose when it last went somewhere (SyncProxy: jiggles)
    bool jiggleSet = false;
    bool inWater = false;  // the game keeps it because it is in water (where it bobs for ever)
    // A proxy with many shapes keeps each shape's colliders on an actor of its own (see BuildBody); `actor` is then
    // an anchor without colliders, moved with them.
    struct Part {
        uint8_t* shape = nullptr;
        PxRigidDynamic* actor = nullptr;
        uint32_t boxes = 0;
    };
    std::vector<Part> parts;
    bool split = false;
    // Since its last full look: 1 a shape's voxels changed, 2 a shape was destroyed (its colliders name a dead shape:
    // the next scan rebuilds it, quiet or not, rate limit or not).
    uint8_t dirty = 0;
    bool frozen = false;  // settled: kinematic in PhysX until something comes near it (see Freeze)
    // changes collected for PhysX (CollectForward / ApplyForward)
    bool fwdMoved = false, fwdPushed = false, fwdWoken = false;
    Tr fwdPose{};
    float fwdDv[3]{}, fwdDw[3]{};
    bool physxAwake = false;  // the actor's state at the last write-back (read while PhysX may be stepping)
    uint32_t cacheSlot = UINT32_MAX;  // index in g_listCache that points here
    bool stepImpulse = false, stepWoken = false;  // AfterNativeStep
    // [PhysX] CatchUp: changes made while ticks passed without PhysX, for its next turn (ApplyPending)
    float pendDv[3]{}, pendDw[3]{};
    bool pending = false, pendWake = false;
    PxBounds3 bounds = PxBounds3::empty();  // owned: world bounds at the last write-back (or takeover)
    PxBounds3 thawFrom = PxBounds3::empty();  // its bounds when it last thawed its surroundings (see Freeze)
    uint32_t thawFromTick = 0;
    uint32_t frozenTick = 0;  // when it last froze
    // diagnostics, since the last report (proxies): moves, how far its centre went, thaw boxes it made
    uint32_t movesWin = 0, thawBoxesWin = 0;
    float travelWin = 0.0f;
    uint8_t thawsSoon = 0;    // thaws within a minute of freezing, in a row: each asks one more settle check
    bool freezeQueued = false;                // in g_freezeQueue
};

// ---- static world: 32^3-voxel blocks of boxes, built near tracked bodies ---------------------------------------
struct Block {
    PxRigidStatic* actor = nullptr;
    PxAggregate* agg = nullptr;  // one broadphase entry for all the block's boxes
    uint64_t hash = 0;
    uint32_t boxes = 0;
};
struct StaticShape {
    uint8_t* vox = nullptr;
    uint8_t* owner = nullptr;  // the static body the blocks were built for (a shape can become debris whole)
    uint32_t solid = 0;
    PxFilterData filter;       // its collision filter, as its blocks' colliders have it
    PxTransform inv{PxIdentity};  // world -> shape (static shapes don't move)
    std::unordered_map<uint32_t, Block> blocks;
};

struct Stats {
    uint64_t ticks = 0;
    double prepareMs = 0, syncMs = 0, stepMs = 0, compareMs = 0, writeMs = 0, nativeMs = 0;
    double bodyBuildMs = 0, blockBuildMs = 0;
    uint64_t bodyBuilds = 0, blockBuilds = 0, blockChecks = 0;
    double stepMaxMs = 0, nativeMaxMs = 0, totalMaxMs = 0;  // total: native step + everything PhysX adds
    double realStepMs = 0, realStepMaxMs = 0;  // the PhysX step from start to finish (async: mostly in the background)
};
// One-step divergence per class: 0 resting (native speed < 0.05 m/s after the step), 1 moving with native contacts
// in that step, 2 free flight (moving, no native contacts).
struct Divergence {
    uint64_t n = 0;
    double dp = 0, dq = 0, dv = 0, step = 0;
    uint64_t hist[32] = {};  // dp in mm, bins: 0, then 2^(i-12) mm
    void Add(double dpM, double dqDeg, double dvMs, double stepM) {
        ++n;
        dp += dpM;
        dq += dqDeg;
        dv += dvMs;
        step += stepM;
        double mm = dpM * 1000.0;
        int bin = mm <= 0.0 ? 0 : std::clamp(int(std::floor(std::log2(mm))) + 13, 1, 31);
        ++hist[bin];
    }
    double P95mm() const {
        uint64_t want = (n * 95 + 99) / 100, acc = 0;
        for (int i = 0; i < 32; ++i) {
            acc += hist[i];
            if (acc >= want && n) return i == 0 ? 0.0 : std::exp2(i - 12);
        }
        return 0.0;
    }
};

// Why the game keeps (or takes back) a body in offload mode. Zone: near the held body or a moving vehicle, where
// the game's two-way contacts (finite masses) matter. Water: the game's buoyancy and drag.
enum Keep : int { kKeepHeld, kKeepNotFree, kKeepVehicle, kKeepConstraint, kKeepWatchdog, kKeepZone, kKeepWater,
                  kKeepOutside, kKeepCount };
struct OffStats {
    uint64_t takeovers = 0, handBacks[kKeepCount] = {}, poseDiffs = 0, velDiffs = 0, nativeWakes = 0, stepDv = 0,
             written = 0, waiting = 0;
    uint64_t supportWakes = 0;     // sleeping PhysX bodies woken because geometry near them changed
    uint64_t staticReleased = 0;   // static shapes whose blocks went away (their body became dynamic)
    uint64_t newStatics = 0;       // static shapes indexed as they appeared (a split of the level, a static spawn)
    uint64_t madeStatic = 0;       // tracked bodies the game made static, indexed as static geometry
    uint64_t movingStarted = 0, movingStopped = 0;  // static bodies that moved (kinematic actor) / came to rest
    uint64_t proxyTeleports = 0;   // proxies moved without sweeping (too fast to push debris sensibly)
    uint64_t skippedTicks = 0;     // [PhysX] CatchUp: ticks that passed without PhysX (its step was still running)
};

SRWLOCK g_lock = SRWLOCK_INIT;
bool g_active = false;  // PhysX world exists for this level
uint32_t g_tick = 0;
std::unordered_map<uint8_t*, BodyEntry> g_bodies;
std::unordered_map<uint8_t*, StaticShape> g_static;
std::unordered_map<int64_t, std::vector<uint8_t*>> g_grid;
std::unordered_set<uint8_t*> g_gridShapes;  // shapes the grid refers to
std::unordered_set<uint8_t*> g_deadShapes;  // destroyed since the grid was built: skipped until the next rebuild
std::vector<uint8_t*> g_modified;           // shapes the breakage driver is about to change (OnShapesModified)
std::vector<uint8_t*> g_staticOrder;        // g_static's shapes, for the round-robin check in RefreshStatic
size_t g_staticNext = 0;
// Shapes the game made since the last tick (td::OnShapeCreated: split jobs make them on worker threads, so they have
// their own lock) and that are still alive: IndexNewStatics puts those that ended up in a static body into the grid.
SRWLOCK g_newLock = SRWLOCK_INIT;
std::unordered_set<uint8_t*> g_newShapes;
// Static bodies with static blocks -> their pose when the blocks were built (WatchStatics checks every tick that
// they haven't moved).
std::unordered_map<uint8_t*, Tr> g_staticPose;
// Static bodies a script moves (SetBodyTransform: a platform, a door, an elevator). Static blocks would stay where
// they were built, so a body that moves gets a kinematic actor instead, swept each tick from where it was placed to
// where the body is (SyncMoving): debris in its way is pushed, debris on it carried. kStillTicks after its last
// move it goes back to static blocks.
struct MovingStatic {
    PxRigidDynamic* actor = nullptr;
    std::vector<ShapeSig> sig;
    Tr placed{};
    uint32_t moved = 0;  // tick of its last move
};
std::unordered_map<uint8_t*, MovingStatic> g_moving;
constexpr uint32_t kStillTicks = 300;
bool g_gridDirty = true;
uint32_t g_gridTick = 0;
float g_worldMinY = -1e30f;  // lowest static geometry (BuildGrid): debris far below it has fallen off the world
std::vector<px::VoxBox> g_boxes;
uint64_t g_preStart = 0, g_preEnd = 0;
Stats g_win, g_all;
Divergence g_div[3], g_divAll[3];
uint32_t g_wokenByPhysx = 0;
uint32_t g_lastActive = 0;  // PhysX actors that moved in the last step
uint32_t g_ownedMoved = 0;  // owned bodies among them (awake in PhysX)
double g_tickBuildMs = 0.0;   // static block building this tick (capped by [PhysX] BuildMsPerTick)
double g_tickBodyBuildMs = 0.0;  // body colliders built this tick ([PhysX] BodyBuildMsPerTick)
uint64_t g_buildsDeferred = 0;   // since the last report: new debris left to the game a tick longer for the budget
uint32_t g_skippedQueries = 0;
uint32_t g_proxyJointed = 0, g_proxyLarge = 0;  // why bodies became proxies (last scan)
FILE* g_csv = nullptr;
FILE* g_dump = nullptr;  // [PhysX] DumpTicks

// offload (Shadow=3)
bool g_offload = false;                             // running for this level
std::atomic<bool> g_inStep{false};                  // the native step runs: engine jobs may call our hooks
std::unordered_map<uint8_t*, BodyEntry*> g_owned;   // read without a lock by HkNarrowWake during the step
uint64_t g_ownedVersion = 0;                        // bumped whenever g_owned changes
SRWLOCK g_stepLock = SRWLOCK_INIT;                  // the two lists below, filled during the step
std::vector<uint8_t*> g_stepWoken;                  // tracked bodies a native constraint woke during the step
std::vector<uint8_t*> g_stepDead;                   // tracked bodies destroyed during the step
std::atomic<uint64_t> g_contactSkips{0}, g_constraintWakes{0};
std::atomic<uint64_t> g_heldWakes{0};   // HkPlayerConstraints: held bodies woken for the grab
uint64_t g_droppedConstraints = 0;      // HkSolverIslands: constraints left out of a step
PxRigidDynamic* g_player = nullptr;                 // kinematic capsule
uint32_t g_playerMovedTick = 0;                     // it last moved (wakes what it touches, and their islands)
OffStats g_off, g_offAll;
FILE* g_offCsv = nullptr;
// Where the prepare time goes (offload scan), in TSC cycles per report window: cheap enough to keep on.
enum Phase : int { kPhSignature, kPhClassify, kPhStatics, kPhBuild, kPhForward, kPhCleanup, kPhRefresh, kPhCount };
uint64_t g_phase[kPhCount] = {};
uint64_t g_phaseTsc0 = 0, g_phaseQpc0 = 0;  // window start, to convert cycles to ms
struct PhaseScope {
    int i;
    uint64_t t;
    explicit PhaseScope(int phase) : i(phase), t(__rdtsc()) {}
    ~PhaseScope() { g_phase[i] += __rdtsc() - t; }
};
// diagnostics: the first events of each kind go to plugin.log
int g_diagTrips = 0, g_diagForwards = 0, g_diagProxies = 0, g_diagSpikes = 0, g_diagZones = 0;
uint64_t g_launches = 0;  // owned bodies PhysX sped up by more than 30 m/s in one step
float g_maxSpeed = 0.0f;  // fastest owned body after a step, this report window
uint64_t g_speedHist[4] = {};  // write-backs this window by speed: <5 cm/s, <20 cm/s, <1 m/s, faster
// What keeps a resting pile awake (diagnostics): proxies moved this window, the ones moved in the last tick, and
// the fastest owned bodies of the last write-back.
uint64_t g_proxyMovesWin = 0, g_proxyHeldWin = 0, g_writeRestWin = 0;  // moved / held by the deadbands
uint64_t g_proxyJiggleWin = 0;  // proxies moved without waking anything (SyncProxy)
uint64_t g_gameWokeWin = 0;     // owned bodies the game's step woke (AfterNativeStep), so PhysX's too
uint64_t g_wedgedWin = 0, g_wedgedAll = 0;  // pieces put to sleep by the settle check
uint32_t g_settleLast = 0, g_settleNext = 0;  // ticks of the last and the next settle check
uint32_t g_sleptAt = 0;  // a check that put pieces to sleep, until the step after it shows which woke again at once
// Pieces held back after waking again at once are all let go at this tick, together: halves of one island the grid
// groups split (two piles in Volkograd) were put to sleep in turn, each while the other was held, and never slept.
uint32_t g_holdRelease = 0;
// The last pieces that woke again did while the player moved (walking on a pile wakes all of it, every step): they
// are let go once the player has stood still for a check, not when the hold is up (up to 64 s of a woken pile).
bool g_holdForPlayer = false;
std::vector<uint8_t*> g_proxiesMovedTick;  // their bodies (an actor may be released by the time it is read)
struct Mover {
    uint8_t* b = nullptr;
    float speed = 0.0f;
};
Mover g_movers[3];
int g_maxSpeedHandle = 0;
float g_maxSpeedMass = 0.0f;

using DtorFn = void*(__fastcall*)(uint8_t* self, uint32_t flags);
DtorFn oBodyDtor = nullptr, oShapeDtor = nullptr;
using NarrowWakeFn = void(__fastcall*)(uint8_t* physics, uint8_t* body);
NarrowWakeFn oNarrowWake = nullptr;
using LuaFn = void(__fastcall*)(void* ctx, void* args, void* ret);
LuaFn oIsBodyActive = nullptr;

double Ms(uint64_t a, uint64_t b) {
    static double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / double(f.QuadPart);
    }();
    return double(b - a) * inv;
}

// Places where PhysX geometry went away or changed this tick (a block rebuilt, a body removed or rebuilt). PhysX
// wakes bodies that were touching a removed actor, but debris put to sleep at takeover never had contacts, so the
// sleeping bodies there are woken explicitly before the step (WakeAroundChanges).
std::vector<PxBounds3> g_changed;
void Changed(const PxRigidActor* a) {
    if (a && g_active) g_changed.push_back(a->getWorldBounds());
}

// [PhysX] Async: PhysX steps from the end of Physics::update until the next tick's start, while the game runs the
// rest of its frame. No PhysX call is allowed in that window; the game's destructors still release actors then, so
// those wait here until the step is fetched.
bool g_async = false;
bool g_stepPending = false;  // simulate() called, fetchResults() not yet
double g_lastKickMs = 0.0;   // simulate() call itself (async)
// [PhysX] CatchUp: when the next tick starts before PhysX's step is done, that tick passes without PhysX (only what
// the game changes on owned bodies is collected) and the next step covers it too.
bool g_skipTick = false;           // this tick passes without PhysX
uint32_t g_pendingTicks = 0;       // native steps since the last simulate(), skipped ones included
uint32_t g_stepSpan = 1;           // ticks the step about to start covers
std::vector<uint8_t*> g_pendBodies;     // owned bodies with changes waiting for PhysX (BodyEntry::pending)
std::vector<uint8_t*> g_pendKinematic;  // handed back while PhysX stepped: their actors become proxies after it

// simulate()'s completion task: PhysX adds a reference when the step starts and removes it when the step is done
// (on one of its threads), which timestamps the step's real duration, also when nobody waits for it.
class StepDoneTask final : public PxBaseTask {
public:
    std::atomic<uint64_t> doneAt{0};
    void run() override {}
    const char* getName() const override { return "tdpx.stepDone"; }
    void addReference() override {}
    void removeReference() override { doneAt.store(trace::Now(), std::memory_order_release); }
    int32_t getReference() const override { return 1; }
    void release() override {}
};
StepDoneTask g_stepDone;
uint64_t g_kickAt = 0;
struct Deferred {
    PxRigidActor* actor;
    PxAggregate* agg;  // or null
};
std::vector<Deferred> g_deferred;

bool g_broken = false;  // PhysX failed: its objects are left alone (GiveUp)
bool g_staticAggregates = true;  // static blocks go into the scene as aggregates ([PhysX] StaticAggregates)

void ReleaseNow(PxRigidActor* a, PxAggregate* agg) {
    if (g_broken) return;
    Changed(a);
    PxScene* s = px::Scene();
    if (agg) {
        if (s) s->removeAggregate(*agg);  // takes its actor out of the scene too
        agg->release();
    } else if (s && a) {
        s->removeActor(*a);
    }
    if (a) a->release();
}

void ReleaseDeferred() {
    for (const Deferred& d : g_deferred) ReleaseNow(d.actor, d.agg);
    g_deferred.clear();
}

template <typename T> void ReleaseActor(T*& a) {
    if (!a) return;
    if (g_stepPending)
        g_deferred.push_back({a, nullptr});
    else
        ReleaseNow(a, nullptr);
    a = nullptr;
}

void ReleaseBlock(Block& b) {
    if (b.actor || b.agg) {
        if (g_stepPending)
            g_deferred.push_back({b.actor, b.agg});
        else
            ReleaseNow(b.actor, b.agg);
    }
    b.actor = nullptr;
    b.agg = nullptr;
}

void ReleaseStatic(StaticShape& ss) {
    for (auto& [k, b] : ss.blocks) ReleaseBlock(b);
    ss.blocks.clear();
}

// A tracked body leaves PhysX (destroyed, gone from the body list).
// The bodies with a kinematic actor (proxies): Sync visits these instead of every tracked body.
std::unordered_set<uint8_t*> g_proxies;

// Native shape -> the body whose PhysX colliders were built from it. A shape that dies marks that body for a rebuild
// (HkShapeDtor) even when the engine unlinked it from its body first.
std::unordered_map<uint8_t*, uint8_t*> g_shapeOwner;
uint64_t g_shapeDeathsUnowned = 0;  // diagnostics: dying shapes no tracked body was built from
std::vector<uint8_t*> g_diedOwners;  // bodies that lost a shape since the last scan

void Unown(uint8_t* b, const std::vector<ShapeSig>& sig) {
    for (const ShapeSig& ss : sig) {
        auto it = g_shapeOwner.find(ss.shape);
        if (it != g_shapeOwner.end() && it->second == b) g_shapeOwner.erase(it);
    }
}

// ---- frozen debris -------------------------------------------------------------------------------------------
// A big pile is one PhysX island: anything touching it (a landing piece, a door the game swings) woke all of it, and
// on Acacia Apartments ~13,000 pieces stayed awake (a PhysX step of 11-15 ms per tick) while few of them moved. A
// piece that got nowhere, or crept slower than FreezeSpeed, through [PhysX] FreezeAfter settle checks is frozen:
// kinematic in PhysX, part of no island, no cost. It thaws before a step that could move it: something moving
// reaches its bounds (a piece faster than ThawSpeed that travelled ThawDistance since it last thawed its
// surroundings, a proxy the game moved, the player), what holds it up changes (WakeAroundChanges), the game moves,
// pushes or wakes it, or its colliders are rebuilt. Like the game's own wakes, only what is reached wakes.
// Switching is dear for PhysX: a broadphase reinsert of the piece's colliders (its pairs are lost, which wakes what
// touched it, and found again) and, to thaw, a linear search of the island edges activated in the last step for
// each of its edges. A first version thawed around anything faster than 5 cm/s: jiggling pieces thawed their frozen
// neighbours as fast as they froze (~1,000 of each a second on Acacia), which cost ~25 ms per frame on the main
// thread and 150-260 ms steps (300k broadphase pairs lost and found at once). Hence travel and speed together, and
// FreezePerTick. Creeping pieces (~1,500 of the Acacia pile creep at 5-20 cm/s for ever: the solver never quite holds
// a pile of 13,000) count as settled. A piece thawed again soon after freezing needs longer (FreezeNeeds). Leaving
// alone pieces near recent movement made nearly the whole Acacia pile ineligible (debris keeps landing and creeping
// there), so freezing goes ahead and the thaws sort it out.
// OFF by default ([PhysX] FreezeAfter=0): on Acacia it never paid. The pile is never quiet for long, and every switch
// drops the piece's contacts (no warm start) and wakes its neighbours, so a frozen pile shook more: ~300 freezes and
// as many thaws a second, at most ~800 of 12,000 pieces frozen, p90 frame 55 ms against 43 without (research/docs,
// "Settled debris").
// Frozen pieces are kept in a grid of 2 m cells by the bounds they froze with (they don't move while frozen).
constexpr float kFrozenCell = 2.0f;
std::unordered_map<uint64_t, std::vector<BodyEntry*>> g_frozenGrid;
// thaw boxes since the last report, by source: pieces PhysX moved, proxies the game moved, the player, impulses
// native code gave owned pieces
enum ThawSource { kThawMover, kThawProxy, kThawPlayer, kThawImpulse, kThawSources };
uint64_t g_thawBoxesBy[kThawSources] = {};
size_t g_frozenCount = 0;
uint64_t g_freezes = 0, g_thaws = 0;  // since the last report
// settle checks since the last report: pieces looked at, settled long enough; queued pieces that were no longer
// settled at their turn (thawed or changed meanwhile) or couldn't freeze
uint64_t g_settleLooked = 0, g_settleLong = 0, g_freezeUnsettled = 0, g_freezeFailed = 0;
std::vector<PxBounds3> g_thawBoxes;   // where something will move during the next step
std::vector<uint8_t*> g_freezeQueue;  // settled pieces waiting for their turn (FreezePerTick)

bool Freezing() { return g_config.pxFreezeAfter > 0; }
// Settle checks a piece needs before it freezes: one more for each time in a row it thawed soon after freezing (next
// to something that keeps moving, freezing it again and again only costs the switches).
int FreezeNeeds(const BodyEntry& e) { return g_config.pxFreezeAfter + std::min<int>(e.thawsSoon, 8); }

uint64_t FrozenKey(int x, int y, int z) {
    auto c = [](int v) { return uint64_t(uint32_t(v + (1 << 20)) & 0x1fffffu); };
    return (c(x) << 42) | (c(y) << 21) | c(z);
}
// Cells of a box; false for a box over 512 cells (the caller looks at every frozen piece instead).
bool FrozenCells(const PxBounds3& b, int lo[3], int hi[3]) {
    if (b.isEmpty() || !b.isFinite()) return false;
    long long n = 1;
    for (int k = 0; k < 3; ++k) {
        lo[k] = int(std::floor(b.minimum[k] / kFrozenCell));
        hi[k] = int(std::floor(b.maximum[k] / kFrozenCell));
        n *= (long long)(hi[k] - lo[k] + 1);
    }
    return n <= 512;
}
template <typename F> void ForCells(const int lo[3], const int hi[3], F f) {
    for (int x = lo[0]; x <= hi[0]; ++x)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int z = lo[2]; z <= hi[2]; ++z) f(FrozenKey(x, y, z));
}

// How far a piece's bounds moved from `from` (the most any face moved); infinitely far from empty bounds.
float Travelled(const PxBounds3& from, const PxBounds3& now) {
    if (from.isEmpty() || now.isEmpty()) return PX_MAX_F32;
    return std::max((now.minimum - from.minimum).abs().maxElement(), (now.maximum - from.maximum).abs().maxElement());
}

// No PhysX call: the piece leaves the frozen set (its actor is released, or becomes a proxy, or thaws).
void Unfreeze(BodyEntry& e) {
    if (!e.frozen) return;
    int lo[3], hi[3];
    if (FrozenCells(e.bounds, lo, hi))
        ForCells(lo, hi, [&](uint64_t key) {
            auto it = g_frozenGrid.find(key);
            if (it == g_frozenGrid.end()) return;
            auto& v = it->second;
            for (size_t k = 0; k < v.size(); ++k)
                if (v[k] == &e) {
                    v[k] = v.back();
                    v.pop_back();
                    break;
                }
            if (v.empty()) g_frozenGrid.erase(it);
        });
    e.frozen = false;
    --g_frozenCount;
}

bool Freeze(BodyEntry& e) {
    int lo[3], hi[3];
    if (e.frozen || e.kinematic || e.bad || !e.actor || !e.owned || !FrozenCells(e.bounds, lo, hi)) return false;
    e.actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    ForCells(lo, hi, [&](uint64_t key) { g_frozenGrid[key].push_back(&e); });
    e.frozen = true;
    e.frozenTick = g_tick;
    e.physxAwake = false;
    ++g_frozenCount;
    ++g_freezes;
    return true;
}

// Something reached the piece, or changed it: a frozen one becomes dynamic again (at rest: PhysX would hand it back
// the velocity it froze with) and its settle check starts over; one that isn't frozen starts its freeze count over.
// PhysX must not be stepping.
void Thaw(BodyEntry& e) {
    e.settled = 0;
    if (!e.frozen) return;
    e.stuck = 0;
    e.anchorTick = 0;
    Unfreeze(e);
    e.thawsSoon = g_tick - e.frozenTick < uint32_t(60.0f / kDt) ? uint8_t(std::min(255, e.thawsSoon + 1)) : uint8_t(0);
    if (e.actor) {
        e.actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, false);
        e.actor->setLinearVelocity(PxVec3(0.0f), false);
        e.actor->setAngularVelocity(PxVec3(0.0f), false);
        e.actor->wakeUp();
        e.physxAwake = true;
    }
    e.thawFrom = e.bounds;
    e.thawFromTick = g_tick;
    ++g_thaws;
}

// Thaws every frozen piece whose bounds meet one of the boxes. PhysX must not be stepping.
void ThawIn(const std::vector<PxBounds3>& boxes) {
    if (boxes.empty() || !g_frozenCount) return;
    static std::vector<BodyEntry*> hit;
    static std::vector<PxBounds3> big;  // boxes too big for the grid: every frozen piece is looked at once for them
    hit.clear();
    big.clear();
    for (const PxBounds3& q : boxes) {
        int lo[3], hi[3];
        if (!FrozenCells(q, lo, hi)) {
            if (q.isFinite() && !q.isEmpty()) big.push_back(q);
            continue;
        }
        ForCells(lo, hi, [&](uint64_t key) {
            auto it = g_frozenGrid.find(key);
            if (it == g_frozenGrid.end()) return;
            for (BodyEntry* e : it->second)
                if (e->bounds.intersects(q)) hit.push_back(e);
        });
    }
    if (!big.empty())
        for (auto& [key, v] : g_frozenGrid)
            for (BodyEntry* e : v)
                for (const PxBounds3& q : big)
                    if (e->bounds.intersects(q)) {
                        hit.push_back(e);
                        break;
                    }
    for (BodyEntry* e : hit) Thaw(*e);  // (repeats: Thaw does nothing the second time)
}

// Where a body moving at `speed` may reach before PhysX's next results.
void ThawAround(const PxBounds3& bounds, float speed, ThawSource from) {
    if (!Freezing() || bounds.isEmpty()) return;
    ++g_thawBoxesBy[from];
    PxBounds3 q = bounds;
    q.fattenFast(speed * kDt * float(std::max(2, g_config.pxCatchUpMax)) + 0.05f);
    g_thawBoxes.push_back(q);
}

// Freezes queued pieces, FreezePerTick at most: those still settled. Runs after the tick's thaws.
void FreezeQueued() {
    size_t k = 0;
    int n = 0;
    const int most = std::max(1, g_config.pxFreezePerTick);
    for (; k < g_freezeQueue.size() && n < most; ++k) {
        auto it = g_owned.find(g_freezeQueue[k]);
        if (it == g_owned.end()) {  // handed back meanwhile (it may be owned again later)
            auto bt = g_bodies.find(g_freezeQueue[k]);
            if (bt != g_bodies.end()) bt->second.freezeQueued = false;
            continue;
        }
        if (!it->second->freezeQueued) continue;
        BodyEntry& e = *it->second;
        e.freezeQueued = false;
        if (e.settled < FreezeNeeds(e))
            ++g_freezeUnsettled;
        else if (Freeze(e))
            ++n;
        else
            ++g_freezeFailed;
    }
    g_freezeQueue.erase(g_freezeQueue.begin(), g_freezeQueue.begin() + ptrdiff_t(k));
}

void ReleaseActors(BodyEntry& e) {
    ReleaseActor(e.actor);
    for (BodyEntry::Part& p : e.parts) ReleaseActor(p.actor);
    e.parts.clear();
    e.split = false;
}

void Forget(uint8_t* b, BodyEntry& e) {
    Unfreeze(e);
    g_ownedVersion += g_owned.erase(b);
    g_proxies.erase(b);
    Unown(b, e.sig);
    ReleaseActors(e);
}

// The engine's body list barely changes order between ticks: the entry found at an index last tick is checked
// first, which saves most of the hash lookups (a few hundred microseconds per tick with thousands of bodies).
struct ListSlot {
    uint8_t* body = nullptr;
    BodyEntry* entry = nullptr;
};
std::vector<ListSlot> g_listCache;

BodyEntry& EntryAt(int i, uint8_t* b) {
    if (size_t(i) >= g_listCache.size()) g_listCache.resize(size_t(i) + 1024);
    ListSlot& slot = g_listCache[size_t(i)];
    if (slot.body == b && slot.entry) return *slot.entry;
    BodyEntry& e = g_bodies[b];
    if (!e.born) e.born = std::max(1u, g_tick);
    if (e.cacheSlot < g_listCache.size() && g_listCache[e.cacheSlot].entry == &e) g_listCache[e.cacheSlot] = {};
    if (slot.entry) slot.entry->cacheSlot = UINT32_MAX;
    slot = {b, &e};
    e.cacheSlot = uint32_t(i);
    return e;
}

// Every erase from g_bodies goes through here, so no list slot keeps pointing at a freed entry.
std::unordered_map<uint8_t*, BodyEntry>::iterator EraseBody(std::unordered_map<uint8_t*, BodyEntry>::iterator it) {
    BodyEntry& e = it->second;
    if (e.cacheSlot < g_listCache.size() && g_listCache[e.cacheSlot].entry == &e) g_listCache[e.cacheSlot] = {};
    Forget(it->first, e);
    return g_bodies.erase(it);
}

// Boxes of one voxel shape onto an actor (shape-local pose = `local`, voxel size `size`). Every PhysX shape keeps the
// native Shape* it was made from in userData (impact reports name the native shapes), and its collision filter.
uint32_t AddBoxes(PxRigidActor& actor, const std::vector<px::VoxBox>& boxes, const PxTransform& local, float size,
                  uint8_t* native) {
    PxMaterial* mat = px::Material();
    const PxFilterData filter = GameFilter(native);
    uint32_t added = 0;
    for (const px::VoxBox& b : boxes) {
        PxVec3 half(float(b.hi[0] - b.lo[0]) * size * 0.5f, float(b.hi[1] - b.lo[1]) * size * 0.5f,
                    float(b.hi[2] - b.lo[2]) * size * 0.5f);
        PxVec3 center(float(b.hi[0] + b.lo[0]) * size * 0.5f, float(b.hi[1] + b.lo[1]) * size * 0.5f,
                      float(b.hi[2] + b.lo[2]) * size * 0.5f);
        if (!(half.x > 0 && half.y > 0 && half.z > 0)) continue;
        PxShape* s = PxRigidActorExt::createExclusiveShape(actor, PxBoxGeometry(half), *mat, PxShapeFlag::eSIMULATION_SHAPE);
        if (!s) continue;
        s->setLocalPose(local * PxTransform(center));
        s->setSimulationFilterData(filter);
        s->userData = native;
        ++added;
    }
    return added;
}

// Convex hulls around a shape's voxels (the corners of its exact box decomposition), in shape space.
uint64_t g_hullFailures = 0;
// hulls used / pieces too irregular even for HullParts hulls (boxes instead) / pieces made of several hulls
uint64_t g_hullsMade = 0, g_hullsBloated = 0, g_hullsSplit = 0;

// One hull around boxes [b, b + n); null if cooking failed. `bloat`: its volume over the voxels' (1 = exact).
PxConvexMesh* CookHull(const px::VoxBox* b, size_t n, float size, double& bloat) {
    static std::vector<PxVec3> pts;
    pts.clear();
    double vox = 0.0;
    for (size_t k = 0; k < n; ++k) {
        for (int i = 0; i < 8; ++i)
            pts.emplace_back(float(i & 1 ? b[k].hi[0] : b[k].lo[0]) * size, float(i & 2 ? b[k].hi[1] : b[k].lo[1]) * size,
                             float(i & 4 ? b[k].hi[2] : b[k].lo[2]) * size);
        vox += double(b[k].hi[0] - b[k].lo[0]) * double(b[k].hi[1] - b[k].lo[1]) * double(b[k].hi[2] - b[k].lo[2]);
    }
    if (pts.size() < 4) return nullptr;
    PxConvexMeshDesc d;
    d.points.count = PxU32(pts.size());
    d.points.stride = sizeof(PxVec3);
    d.points.data = pts.data();
    d.flags = PxConvexFlag::eCOMPUTE_CONVEX | PxConvexFlag::eDISABLE_MESH_VALIDATION |
              PxConvexFlag::eFAST_INERTIA_COMPUTATION | PxConvexFlag::eSHIFT_VERTICES;
    d.vertexLimit = PxU16(std::clamp(g_config.pxHullVertices, 8, 255));
    PxCookingParams params(px::Physics()->getTolerancesScale());
    params.buildGPUData = px::GpuActive();  // GPU contacts need it (and at most 64 vertices and faces: cooking caps them)
    PxConvexMesh* mesh = PxCreateConvexMesh(params, d, px::Physics()->getPhysicsInsertionCallback());
    if (!mesh) {
        ++g_hullFailures;
        return nullptr;
    }
    vox *= double(size) * double(size) * double(size);
    PxReal volume;  // the mass at density 1
    PxMat33 inertia;
    PxVec3 com;
    mesh->getMassInformation(volume, inertia, com);
    bloat = vox > 0.0 ? double(volume) / vox : 1e9;
    return mesh;
}

// Where to split boxes [b, b + n) in two: the axis and count (after sorting them by centre along it) whose two halves
// have the smallest bounding boxes together. Sorts the range along the axis it picks.
size_t SplitBoxes(px::VoxBox* b, size_t n) {
    auto centre = [](const px::VoxBox& x, int k) { return x.lo[k] + x.hi[k]; };
    auto volume = [](const int lo[3], const int hi[3]) {
        return double(hi[0] - lo[0]) * double(hi[1] - lo[1]) * double(hi[2] - lo[2]);
    };
    static std::vector<double> left;
    double best = 1e300;
    int bestAxis = 0;
    size_t bestAt = n / 2;
    for (int axis = 0; axis < 3; ++axis) {
        std::sort(b, b + n, [&](const px::VoxBox& x, const px::VoxBox& y) { return centre(x, axis) < centre(y, axis); });
        left.assign(n, 0.0);
        int lo[3] = {INT_MAX, INT_MAX, INT_MAX}, hi[3] = {INT_MIN, INT_MIN, INT_MIN};
        for (size_t k = 0; k < n; ++k) {
            for (int j = 0; j < 3; ++j) {
                lo[j] = std::min(lo[j], b[k].lo[j]);
                hi[j] = std::max(hi[j], b[k].hi[j]);
            }
            left[k] = volume(lo, hi);  // boxes [0, k]
        }
        int rlo[3] = {INT_MAX, INT_MAX, INT_MAX}, rhi[3] = {INT_MIN, INT_MIN, INT_MIN};
        for (size_t k = n - 1; k > 0; --k) {
            for (int j = 0; j < 3; ++j) {
                rlo[j] = std::min(rlo[j], b[k].lo[j]);
                rhi[j] = std::max(rhi[j], b[k].hi[j]);
            }
            double cost = left[k - 1] + volume(rlo, rhi);  // [0, k) and [k, n)
            if (cost < best) {
                best = cost;
                bestAxis = axis;
                bestAt = k;
            }
        }
    }
    std::sort(b, b + n, [&](const px::VoxBox& x, const px::VoxBox& y) { return centre(x, bestAxis) < centre(y, bestAxis); });
    return bestAt;
}

// Colliders for one shape from its exact boxes: one hull when it fits the voxels closely enough ([PhysX]
// HullMaxBloat; a looser one wedges into the neighbours of a concave piece and keeps a pile vibrating), else the
// boxes split in two where their halves' bounds are smallest, each half the same way, up to HullParts hulls in all
// (a part of one box stays that box). Exact boxes, ~9 a piece on Acacia's pile, gave PhysX 60% more colliders and
// a slower step. Returns the colliders added, 0 when even HullParts hulls would bulge (the caller adds the boxes).
uint32_t AddHulls(PxRigidActor& actor, std::vector<px::VoxBox>& boxes, const PxTransform& local, float size,
                  uint8_t* native) {
    if (boxes.empty()) return 0;
    const double maxBloat = g_config.pxHullMaxBloat > 0.0f ? double(g_config.pxHullMaxBloat) : 1e300;
    const size_t maxParts = size_t(std::clamp(g_config.pxHullParts, 1, 16));
    struct Part {
        PxConvexMesh* mesh;  // null: a single box
        size_t lo, hi;       // boxes
    };
    static std::vector<Part> done;
    static std::vector<std::pair<size_t, size_t>> todo;
    done.clear();
    todo.clear();
    todo.push_back({0, boxes.size()});
    bool ok = true;
    while (!todo.empty() && ok) {
        auto [lo, hi] = todo.back();
        todo.pop_back();
        if (hi - lo == 1) {
            done.push_back({nullptr, lo, hi});
            continue;
        }
        double bloat = 1e9;
        PxConvexMesh* mesh = CookHull(&boxes[lo], hi - lo, size, bloat);
        if (mesh && bloat <= maxBloat) {
            done.push_back({mesh, lo, hi});
            continue;
        }
        if (mesh) mesh->release();
        if (!mesh || done.size() + todo.size() + 2 > maxParts) {
            ok = false;
            break;
        }
        size_t at = lo + SplitBoxes(&boxes[lo], hi - lo);
        todo.push_back({lo, at});
        todo.push_back({at, hi});
    }
    if (!ok) {
        for (Part& p : done)
            if (p.mesh) p.mesh->release();
        ++g_hullsBloated;
        return 0;
    }
    const PxFilterData filter = GameFilter(native);
    uint32_t added = 0;
    for (Part& p : done) {
        PxShape* s = nullptr;
        if (p.mesh) {
            s = PxRigidActorExt::createExclusiveShape(actor, PxConvexMeshGeometry(p.mesh), *px::Material(),
                                                      PxShapeFlag::eSIMULATION_SHAPE);
            p.mesh->release();  // the shape holds its own reference
            if (s) s->setLocalPose(local);
            ++g_hullsMade;
        } else {
            const px::VoxBox& b = boxes[p.lo];
            PxVec3 half(float(b.hi[0] - b.lo[0]) * size * 0.5f, float(b.hi[1] - b.lo[1]) * size * 0.5f,
                        float(b.hi[2] - b.lo[2]) * size * 0.5f);
            PxVec3 centre(float(b.hi[0] + b.lo[0]) * size * 0.5f, float(b.hi[1] + b.lo[1]) * size * 0.5f,
                          float(b.hi[2] + b.lo[2]) * size * 0.5f);
            if (half.x > 0 && half.y > 0 && half.z > 0)
                s = PxRigidActorExt::createExclusiveShape(actor, PxBoxGeometry(half), *px::Material(),
                                                          PxShapeFlag::eSIMULATION_SHAPE);
            if (s) s->setLocalPose(local * PxTransform(centre));
        }
        if (!s) continue;
        s->setSimulationFilterData(filter);
        s->userData = native;
        ++added;
    }
    if (done.size() > 1) ++g_hullsSplit;
    return added;
}

// ---- impacts (Phase 3): PhysX contacts strong enough to be heard -------------------------------------------
// Collected while PhysX results are fetched (onContact runs inside fetchResults, on this thread) and handed to the
// game during its next physics step, where its own impact pass plays them (InjectImpacts).
struct Impact {
    uint8_t* shape[2];  // native shapes
    PxVec3 pos;         // strongest contact point
    float hit;          // approach speed along the normal before the impact (m/s), as the game measures it
    float scrape;       // sliding speed there afterwards (m/s)
};
std::vector<Impact> g_impacts;
uint64_t g_impactReports = 0, g_impactsInjected = 0;
uint64_t g_gameEvents = 0, g_gameAudible = 0;  // the game's own impact events (counted in every shadow mode)
// Native shapes destroyed since the last scan rebuilt the colliders: until then PhysX shapes can still name them
// (the step's actors, blocks released only after it, colliders of bodies carved since), so contact reports can too.
// A report naming one is dropped before anything reads the game's memory.
std::unordered_set<uint8_t*> g_deadSinceKick;
bool DeadShape(uint8_t* s) { return s && !g_deadSinceKick.empty() && g_deadSinceKick.count(s); }
// A shape PhysX names is still one: its first word is Shape's vftable (a destroyed one has Entity's, or whatever
// reused the memory). Read where faults are tolerated.
uint64_t g_deadNamed = 0;  // contact report points dropped for naming a destroyed shape
bool LiveShape(uint8_t* s) { return s && *reinterpret_cast<uintptr_t*>(s) == td::VA(Addr::ShapeVftable); }

// Impact damage: contact points that may break voxels (see BuildDamage).
inline uint8_t EntityFlags(uint8_t* e) { return td::At<uint8_t>(e, 0x0a); }  // 0x04 unbreakable (see BuildDamage)
inline float ShapeStrength(uint8_t* s) { return td::At<float>(s, 0xd4); }
struct DamagePoint {
    uint8_t* shape[2];  // native shapes
    PxVec3 pos;
    float impulse;  // PhysX's impulse at the point over the step
    float speed;    // how fast the two points moved apart or together before the step (m/s)
};
std::vector<DamagePoint> g_damagePts;
uint64_t g_damagePoints = 0;

bool Kinematic(const PxActor* a) {
    const PxRigidDynamic* d = a ? a->is<PxRigidDynamic>() : nullptr;
    return d && (d->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC);
}
PxVec3 PointVelocity(const PxActor* a, const PxVec3& p) {
    const PxRigidDynamic* d = a ? a->is<PxRigidDynamic>() : nullptr;
    if (!d) return PxVec3(0.0f);
    PxVec3 com = d->getGlobalPose().transform(d->getCMassLocalPose().p);
    return d->getLinearVelocity() + d->getAngularVelocity().cross(p - com);
}
// The velocity at a point before the step whose contacts are reported: for an owned body, what was written back
// before that step (the game's changes since go into the next one). PhysX's own velocities are already past it.
PxVec3 PreStepVelocity(const PxActor* a, const PxVec3& p) {
    const PxRigidDynamic* d = a ? a->is<PxRigidDynamic>() : nullptr;
    if (!d) return PxVec3(0.0f);
    auto it = g_owned.find(static_cast<uint8_t*>(d->userData));
    if (it == g_owned.end()) return PointVelocity(a, p);
    PxVec3 com = d->getGlobalPose().transform(d->getCMassLocalPose().p);
    return V(it->second->expV) + V(it->second->expW).cross(p - com);
}

// Reads of shapes PhysX reports, where a fault drops the point (defined with MaterialAt).
template <typename F> bool Tolerant(F& f);

class ImpactCollector final : public PxSimulationEventCallback {
public:
    void onContact(const PxContactPairHeader& header, const PxContactPair* pairs, PxU32 count) override {
        if (header.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1))
            return;
        // The game already hears its own moving bodies (proxies) and the player hitting owned debris: those
        // contacts still exist on its side.
        if (Kinematic(header.actors[0]) || Kinematic(header.actors[1])) return;
        for (PxU32 i = 0; i < count; ++i) {
            const PxContactPair& cp = pairs[i];
            if (!(cp.events & PxPairFlag::eNOTIFY_THRESHOLD_FORCE_FOUND)) continue;
            if (cp.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1)) continue;
            PxContactPairPoint pts[16];
            PxU32 n = cp.extractContacts(pts, 16);
            float best = 0.0f;
            PxU32 at = 0;
            for (PxU32 k = 0; k < n; ++k) {
                float j = pts[k].impulse.magnitude();
                if (j > best) {
                    best = j;
                    at = k;
                }
            }
            if (!n || !(best > 0.0f)) continue;
            Impact imp;
            imp.shape[0] = static_cast<uint8_t*>(cp.shapes[0]->userData);
            imp.shape[1] = static_cast<uint8_t*>(cp.shapes[1]->userData);
            if (!imp.shape[0] && !imp.shape[1]) continue;
            if (DeadShape(imp.shape[0]) || DeadShape(imp.shape[1])) continue;
            if (g_config.pxDamage && imp.shape[0] && imp.shape[1]) {
                auto damage = [&] { Damage(header, imp.shape, pts, n); };
                if (!Tolerant(damage)) continue;
            }
            if (!g_config.pxImpacts) continue;
            imp.pos = pts[at].position;
            PxVec3 nrm = pts[at].normal;  // from shape 1 to shape 0
            // The game's hit is the approach speed before its solver. The impulse alone overstates it wherever it
            // carries weight: a light piece under a heavy one would ring every time the pile settles.
            PxVec3 pre = PreStepVelocity(header.actors[0], imp.pos) - PreStepVelocity(header.actors[1], imp.pos);
            imp.hit = std::max(0.0f, -pre.dot(nrm));
            PxVec3 rel = PointVelocity(header.actors[0], imp.pos) - PointVelocity(header.actors[1], imp.pos);
            imp.scrape = (rel - nrm * rel.dot(nrm)).magnitude();
            g_impacts.push_back(imp);
            ++g_impactReports;
        }
    }
    // Points the game's add-contact would give a break threshold: the two points close faster than 6 m/s, or an
    // entity flag lifts the speed limit (0x40 on either shape or body, 0x20 on either shape).
    static void Damage(const PxContactPairHeader& header, uint8_t* const shape[2], const PxContactPairPoint* pts,
                       PxU32 n) {
        uint8_t* body[2] = {ShapeBody(shape[0]), ShapeBody(shape[1])};
        if (!body[0] || !body[1]) return;
        bool any = ((EntityFlags(shape[0]) | EntityFlags(shape[1])) & 0x60) ||
                   ((EntityFlags(body[0]) | EntityFlags(body[1])) & 0x40);
        for (PxU32 k = 0; k < n; ++k) {
            float j = pts[k].impulse.magnitude();
            if (!(j > 0.0f)) continue;
            const PxVec3& p = pts[k].position;
            float speed = (PreStepVelocity(header.actors[0], p) - PreStepVelocity(header.actors[1], p)).magnitude();
            if (speed > 6.0f || any) {
                g_damagePts.push_back({{shape[0], shape[1]}, p, j, speed});
                ++g_damagePoints;
            }
        }
    }
    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    // PhysX's own sleep and wake changes (actors created with eSEND_SLEEP_NOTIFIES). Its active-actor list can't
    // tell: a body falling asleep is no longer in it in the step it does.
    void onWake(PxActor** actors, PxU32 count) override { SetAwake(actors, count, true); }
    void onSleep(PxActor** actors, PxU32 count) override { SetAwake(actors, count, false); }
    static void SetAwake(PxActor** actors, PxU32 count, bool awake) {
        for (PxU32 i = 0; i < count; ++i) {
            auto it = g_bodies.find(static_cast<uint8_t*>(actors[i]->userData));
            if (it != g_bodies.end() && it->second.actor == actors[i] && !it->second.frozen)
                it->second.physxAwake = awake;
        }
    }
    void onTrigger(PxTriggerPair*, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, const PxU32) override {}
};
ImpactCollector g_collector;

// The game's impact sound event (the list at Physics+0xa3058, filled by its contact code, played at the end of
// Physics::update: hit volume = clamp((hit - 0.5) * 0.05, 0, 1), size class from each shape's voxel count).
#pragma pack(push, 1)
struct SoundEvent {
    uint8_t* shape[2];
    int material[2];  // voxel material type of each dynamic side at the contact; 0: that side makes no sound
    float pos[3];
    float hit, scrape;  // approach and sliding speed (m/s)
    uint32_t pad;
};
#pragma pack(pop)
static_assert(sizeof(SoundEvent) == 0x30, "the game's impact event is 0x30 bytes");
std::vector<SoundEvent> g_sounds;  // for the game's next physics step (HkSoundListMerge)
uint64_t g_soundsMade = 0, g_soundsInjected = 0;

using PaletteFn = uint8_t*(__fastcall*)(uint8_t* voxels);
using AllocFn = void*(__fastcall*)(size_t size);
using FreeFn = void(__fastcall*)(void* p);

// Material type of the voxel at a world point, or of the nearest solid one around it (a hull's contact point can
// sit in an empty voxel): palette entries are 40 bytes with the type at +4, as the game's contact code reads them.
// Contact reports name native shapes by pointer, and the dead-shape checks can't see every way the game destroys or
// empties a shape: a pile benchmark with box colliders read one stale (a voxel grid of garbage). Reads of reported
// shapes therefore run here, where a fault drops that point instead of ending the game.
uint64_t g_staleReads = 0;
bool CallTolerant(void (*fn)(void*), void* ctx) {
    ++crash::t_tolerateFaults;
    bool ok = true;
    __try {
        fn(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    --crash::t_tolerateFaults;
    if (!ok) ++g_staleReads;
    return ok;
}
template <typename F> bool Tolerant(F& f) {
    return CallTolerant([](void* c) { (*static_cast<F*>(c))(); }, &f);
}

int MaterialAt(uint8_t* shape, const PxVec3& p) {
    uint8_t* vox = td::ShapeVoxels(shape);
    if (!vox) return 0;
    px::VoxGrid g = Grid(vox);
    float size = td::VoxSize(vox);
    if (!GridOk(g) || !(size > 0.0f)) return 0;
    PxVec3 l = ShapeWorld(shape).getInverse().transform(p) * (1.0f / size);
    int c[3] = {int(std::floor(l.x)), int(std::floor(l.y)), int(std::floor(l.z))};
    int pal = 0;
    for (int r = 0; r <= 2 && !pal; ++r)
        for (int dz = -r; dz <= r && !pal; ++dz)
            for (int dy = -r; dy <= r && !pal; ++dy)
                for (int dx = -r; dx <= r && !pal; ++dx) {
                    int x = std::clamp(c[0] + dx, 0, g.n[0] - 1), y = std::clamp(c[1] + dy, 0, g.n[1] - 1),
                        z = std::clamp(c[2] + dz, 0, g.n[2] - 1);
                    pal = g.data[size_t(x) + size_t(g.n[0]) * (size_t(y) + size_t(g.n[1]) * size_t(z))];
                }
    if (!pal) return 0;
    uint8_t* entries = td::Fn<PaletteFn>(Addr::VoxPalette)(vox);
    return entries ? *reinterpret_cast<int*>(entries + size_t(pal) * 40 + 4) : 0;
}

// After PhysX results are fetched: the strongest audible impacts become the game's events. Static shapes get no
// material, like in the game's own contacts (only dynamic sides make sound).
void BuildSounds() {
    std::sort(g_impacts.begin(), g_impacts.end(),
              [](const Impact& a, const Impact& b) { return std::max(a.hit, a.scrape) > std::max(b.hit, b.scrape); });
    size_t n = std::min<size_t>(g_impacts.size(), 48);
    for (size_t k = 0; k < n; ++k) {
        const Impact& im = g_impacts[k];
        if (std::max(im.hit, im.scrape) <= 0.7f) break;  // (v - 0.5) * 0.05 must pass the game's 0.01
        if (!g_deadSinceKick.empty() && (g_deadSinceKick.count(im.shape[0]) || g_deadSinceKick.count(im.shape[1])))
            continue;
        SoundEvent ev{};
        bool dead = false;
        auto materials = [&] {
            for (int s = 0; s < 2; ++s)
                dead |= im.shape[s] && !LiveShape(im.shape[s]);
            if (dead) return;
            for (int s = 0; s < 2; ++s) {
                ev.shape[s] = im.shape[s];
                uint8_t* body = im.shape[s] ? ShapeBody(im.shape[s]) : nullptr;
                if (body && BodyDynamic(body)) ev.material[s] = MaterialAt(im.shape[s], im.pos);
            }
        };
        if (!Tolerant(materials)) continue;
        if (dead) {
            ++g_deadNamed;
            continue;
        }
        if (!ev.material[0] && !ev.material[1]) continue;
        ev.pos[0] = im.pos.x;
        ev.pos[1] = im.pos.y;
        ev.pos[2] = im.pos.z;
        ev.hit = im.hit;
        ev.scrape = im.scrape;
        g_sounds.push_back(ev);
        ++g_soundsMade;
    }
    g_impacts.clear();
    if (g_sounds.size() > 256) g_sounds.erase(g_sounds.begin(), g_sounds.end() - 256);  // the game may not step
}

// Appends to the game's event list the way its own merge (0x1400b5390) does: capacity * 2 + 1, the engine's
// allocator, the inline buffer at +0xa3068 is never freed.
void AppendSoundEvent(uint8_t* physics, const SoundEvent& ev) {
    int& count = td::At<int>(physics, 0xa3058);
    int& cap = td::At<int>(physics, 0xa305c);
    SoundEvent*& data = td::At<SoundEvent*>(physics, 0xa3060);
    if (count >= cap) {
        int ncap = cap * 2 + 1;
        auto* fresh = static_cast<SoundEvent*>(td::Fn<AllocFn>(Addr::EngineAlloc)(size_t(ncap) * sizeof(SoundEvent)));
        if (!fresh) return;
        if (data && count > 0) std::memcpy(fresh, data, size_t(count) * sizeof(SoundEvent));
        if (data && reinterpret_cast<uint8_t*>(data) != physics + 0xa3068) td::Fn<FreeFn>(Addr::EngineFree)(data);
        data = fresh;
        cap = ncap;
    }
    data[count++] = ev;
}

// ---- impact damage (Phase 3): PhysX impacts break voxels the way the game's contacts do ----------------------
// The game's add-contact (0x1400acb40) gives each side of a contact a break threshold when the two points close
// faster than 6 m/s: the strength of the voxel's material (table 0x140bef708) x 1200 x the shape's strength
// (+0xd4). A side flagged unbreakable (0x04 on its shape or body) or of strength 0 gets none; a shape flagged
// 0x20 gives the other side 0.01 at any speed. After the solve, Physics::update keeps the contacts whose impulse
// reached the lower threshold, merges those of the same two bodies within 1 m, and gives each side whose
// impulse / threshold passes 0.05 a hole there: radius ratio x 1.2 for soft and x 0.8 for medium materials,
// carved per body with MakeHoleCore (with sound). Its solver stops such a contact's impulse at the lower
// threshold, so the weaker side's ratio is 1 (native impact holes stay under ~1,600 voxels); PhysX does not stop
// it, so the ratio is capped the same way here.
#pragma pack(push, 1)
struct HoleSphere {
    float pos[3];
    float soft, medium, hard;  // radii per material class, as Lua MakeHole takes them
};
#pragma pack(pop)
static_assert(sizeof(HoleSphere) == 0x18, "MakeHoleCore's sphere is 0x18 bytes");
struct HoleList {  // the engine's list
    int count, cap;
    HoleSphere* data;
};
using MakeHoleCoreFn = void(__fastcall*)(void* world, uint8_t* body, HoleList* spheres, uint8_t sound,
                                         const void* impulse);
std::vector<std::pair<uint8_t*, HoleSphere>> g_holes;  // by target body; carved once the lock is released
bool g_damageOk = false;                                // MakeHoleCore and the strength table matched at install
bool g_carving = false;                                 // CarveImpacts is running (main thread)
std::unordered_set<uint8_t*> g_carveDead;               // bodies destroyed meanwhile
uint64_t g_damageHits = 0, g_damageHoles = 0, g_damageCarves = 0, g_damageSkipped = 0;

void BuildDamage() {
    if (g_damagePts.empty()) return;
    const float* strength = reinterpret_cast<const float*>(td::VA(Addr::MaterialStrength));
    struct Hit {
        uint8_t* body[2];
        PxVec3 mn, mx;
        float thr[2];
        bool merged;
    };
    static std::vector<Hit> hits;
    hits.clear();
    for (const DamagePoint& d : g_damagePts) {
        if (!g_deadSinceKick.empty() && (g_deadSinceKick.count(d.shape[0]) || g_deadSinceKick.count(d.shape[1])))
            continue;
        Hit h{};
        bool hit = false;
        auto judge = [&] {
            if (!LiveShape(d.shape[0]) || !LiveShape(d.shape[1])) {
                ++g_deadNamed;
                return;
            }
            uint8_t* body[2] = {ShapeBody(d.shape[0]), ShapeBody(d.shape[1])};
            if (!body[0] || !body[1]) return;
            uint8_t fs[2] = {EntityFlags(d.shape[0]), EntityFlags(d.shape[1])};
            uint8_t fb[2] = {EntityFlags(body[0]), EntityFlags(body[1])};
            float thr[2] = {FLT_MAX, FLT_MAX}, str[2];
            bool can[2];
            for (int s = 0; s < 2; ++s) {
                int mat = MaterialAt(d.shape[s], d.pos);
                str[s] = mat > 0 && mat < 16 ? strength[mat] * 1200.0f * ShapeStrength(d.shape[s]) : 0.0f;
                can[s] = !(fs[s] & 0x04) && !(fb[s] & 0x04) && str[s] > 0.0f;
            }
            if (d.speed > 6.0f || ((fs[0] | fs[1] | fb[0] | fb[1]) & 0x40))
                for (int s = 0; s < 2; ++s)
                    if (can[s]) thr[s] = str[s];
            if ((fs[0] & 0x20) && can[1]) thr[1] = 0.01f;
            if ((fs[1] & 0x20) && can[0]) thr[0] = 0.01f;
            if (d.impulse < thr[0] && d.impulse < thr[1]) return;
            h = {{body[0], body[1]}, d.pos, d.pos, {thr[0], thr[1]}, false};
            hit = true;
        };
        if (Tolerant(judge) && hit) hits.push_back(h);
    }
    g_damagePts.clear();
    for (size_t i = 0; i < hits.size(); ++i) {
        Hit& h = hits[i];
        if (h.merged) continue;
        for (size_t j = i + 1; j < hits.size(); ++j) {
            Hit& o = hits[j];
            bool same = (o.body[0] == h.body[0] && o.body[1] == h.body[1]) ||
                        (o.body[0] == h.body[1] && o.body[1] == h.body[0]);
            if (o.merged || !same || (o.mn - (h.mn + h.mx) * 0.5f).magnitude() >= 1.0f) continue;
            h.mn = h.mn.minimum(o.mn);
            h.mx = h.mx.maximum(o.mx);
            o.merged = true;
        }
        ++g_damageHits;
        PxVec3 c = (h.mn + h.mx) * 0.5f;
        float low = std::min(h.thr[0], h.thr[1]);
        for (int s = 0; s < 2; ++s) {
            float ratio = h.thr[s] < FLT_MAX ? low / h.thr[s] : 0.0f;
            if (ratio > 0.05f)
                g_holes.push_back({h.body[s], HoleSphere{{c.x, c.y, c.z}, ratio * 1.2f, ratio * 0.8f, 0.0f}});
        }
    }
}

void SleepNative(uint8_t* b);

// On the main thread with the lock released (MakeHoleCore destroys and splits bodies, and our destructor and
// breakage hooks take the lock), before the native step: like a script's MakeHole.
void CarveImpacts() {
    static std::vector<std::pair<uint8_t*, HoleSphere>> holes;
    uint8_t* scene = td::SceneObj();
    AcquireSRWLockExclusive(&g_lock);
    holes.swap(g_holes);
    g_holes.clear();
    if (!scene) holes.clear();
    g_carving = !holes.empty();
    ReleaseSRWLockExclusive(&g_lock);
    if (holes.empty()) return;
    std::stable_sort(holes.begin(), holes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    void* world = td::At<void*>(scene, 0x10);
    const void* noImpulse = reinterpret_cast<const void*>(td::VA(Addr::NoImpulseDesc));
    static std::vector<HoleSphere> spheres;
    int carves = 0;
    for (size_t i = 0; i < holes.size();) {
        uint8_t* body = holes[i].first;
        spheres.clear();
        for (; i < holes.size() && holes[i].first == body; ++i) spheres.push_back(holes[i].second);
        AcquireSRWLockShared(&g_lock);
        bool dead = g_carveDead.count(body) != 0;
        ReleaseSRWLockShared(&g_lock);
        if (dead || carves >= std::max(0, g_config.pxDamageMax)) {
            g_damageSkipped += spheres.size();
            continue;
        }
        HoleList list{int(spheres.size()), int(spheres.size()), spheres.data()};
        td::Fn<MakeHoleCoreFn>(Addr::MakeHoleCore)(world, body, &list, 1, noImpulse);
        ++carves;
        ++g_damageCarves;
        g_damageHoles += spheres.size();
    }
    holes.clear();
    AcquireSRWLockExclusive(&g_lock);
    g_carving = false;
    g_carveDead.clear();
    // Destruction wakes what it touched. This tick's changes were collected already, so owned bodies woken here
    // are handed to PhysX now; the native step would move them otherwise.
    if (carves)
        for (auto& [b, e] : g_owned)
            if (BodyAwake(b)) {
                ++g_off.nativeWakes;
                SleepNative(b);
                if (e->actor && e->actor->isSleeping()) e->actor->wakeUp();
            }
    ReleaseSRWLockExclusive(&g_lock);
}

// After PhysX's results are fetched: its contacts become the game's impact sounds and damage.
void AfterFetch() {
    BuildSounds();
    if (g_damageOk) BuildDamage();
    g_damagePts.clear();
}

// Mass, inertia and centre of mass from the native body. The native inverse inertia (+0xf0) is diagonal in the
// principal frame +0x10c (a rotation relative to the body frame), which is PhysX's mass frame.
void SetMass(PxRigidDynamic& a, uint8_t* body) {
    float im = BodyInvMass(body);
    const float* ii = BodyInvInertia(body);
    PxVec3 inertia(ii[0] > 0 ? 1.0f / ii[0] : 1.0f, ii[1] > 0 ? 1.0f / ii[1] : 1.0f, ii[2] > 0 ? 1.0f / ii[2] : 1.0f);
    float mass = im > 0 ? 1.0f / im : 1.0f;
    a.setMass(mass);
    a.setMassSpaceInertiaTensor(inertia);
    // impacts are reported when a contact pushes harder than stopping this body from ImpactSpeed within a tick
    a.setContactReportThreshold(mass * std::max(0.1f, g_config.pxImpactSpeed) / kDt);
    a.setCMassLocalPose(PxTransform(V(BodyCom(body)), Quat(BodyInertiaRot(body))));
}

bool BodyAabb(uint8_t* body, float mn[3], float mx[3]);

// A proxy follows the game's body with infinite mass. Squeezing debris against something else (the game's state is
// a tick ahead of PhysX's), it flung pieces at 30-85 m/s (a 12-voxel swinging object on Acacia Apartments). Its
// contacts now push at most as hard as its own mass moving at [PhysX] ProxyImpulseSpeed; a heavy one still bulldozes.
void CapProxy(PxRigidDynamic* a, uint8_t* body) {
    float im = BodyInvMass(body);
    a->setMaxContactImpulse(g_config.pxProxyImpulseSpeed > 0.0f && im > 0.0f ? g_config.pxProxyImpulseSpeed / im
                                                                               : PX_MAX_F32);
}

// A new actor for a tracked body, colliders to follow.
PxRigidDynamic* NewActor(uint8_t* body, const Tr& pose, bool kinematic, bool notify) {
    PxRigidDynamic* a = px::Physics()->createRigidDynamic(ToPx(pose));
    if (!a) return nullptr;
    a->userData = body;
    if (kinematic) {
        a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        CapProxy(a, body);
    }
    if (g_offload && g_config.pxCcd) a->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_SPECULATIVE_CCD, true);
    if (g_offload && notify) a->setActorFlag(PxActorFlag::eSEND_SLEEP_NOTIFIES, true);  // physxAwake
    a->setLinearDamping(std::max(0.0f, g_config.pxLinearDamping));
    a->setAngularDamping(std::max(0.0f, g_config.pxAngularDamping));
    a->setMaxDepenetrationVelocity(g_config.pxMaxDepenetration);
    a->setSolverIterationCounts(PxU32(std::clamp(g_config.pxPositionIters, 1, 255)),
                                PxU32(std::clamp(g_config.pxVelocityIters, 0, 255)));
    if (g_config.pxSleepThreshold > 0.0f) a->setSleepThreshold(g_config.pxSleepThreshold);
    return a;
}

// Proxies with more shapes than [PhysX] SplitShapes get an actor per shape. PhysX's cost to add or remove one
// collider grows with all the contacts of its actor: on the top of a tower cut loose (1,900 shapes, 42,000 boxes, a
// pile resting on it) each box changed cost 40-70 us, and a carve of 45 shapes 150 ms.

void BuildBody(BodyEntry& e, uint8_t* body, const std::vector<ShapeSig>& sig, bool kinematic) {
    uint64_t t0 = trace::Now();
    PxScene* scene = px::Scene();
    const bool split = kinematic && sig.size() > size_t(std::max(1, g_config.pxSplitShapes));
    if (e.frozen) Thaw(e);  // (carved: its pieces may fall)
    if (e.actor && (e.kinematic != kinematic || e.split != split)) Forget(body, e);
    // An actor rebuilt as the same kind keeps the colliders of the shapes that didn't change. Rebuilt whole, a big
    // proxy took 0.2-1.1 s twice a second, nearly all of it detaching boxes the pile rested on (PhysX tears down
    // each one's contact pairs and wakes what touched it, so the whole pile woke with every rebuild).
    static std::unordered_set<uint8_t*> kept;  // native shapes whose PhysX shapes stay
    kept.clear();
    uint32_t keptBoxes = 0;
    if (!e.actor) {
        e.actor = NewActor(body, BodyTr(body), kinematic, true);
        if (!e.actor) return;
        scene->addActor(*e.actor);
        e.fresh = true;
        e.placed = BodyTr(body);
        e.split = split;
    } else {
        static std::unordered_map<uint8_t*, const ShapeSig*> before;
        before.clear();
        for (const ShapeSig& s : e.sig) before.emplace(s.shape, &s);
        for (const ShapeSig& s : sig) {
            auto it = before.find(s.shape);
            if (it != before.end() && Same(*it->second, s)) kept.insert(s.shape);
        }
        if (split) {  // the parts of changed shapes go (ReleaseNow wakes what rested on them)
            size_t n = 0;
            for (BodyEntry::Part& p : e.parts) {
                if (kept.count(p.shape)) {
                    keptBoxes += p.boxes;
                    e.parts[n++] = p;
                } else {
                    ReleaseActor(p.actor);
                }
            }
            e.parts.resize(n);
        } else {
            static std::vector<PxShape*> shapes;
            static std::unordered_map<void*, PxBounds3> gone;  // per native shape: where its colliders were
            gone.clear();
            shapes.resize(e.actor->getNbShapes());
            e.actor->getShapes(shapes.data(), PxU32(shapes.size()));
            for (PxShape* s : shapes) {
                if (kept.count(static_cast<uint8_t*>(s->userData))) {
                    ++keptBoxes;
                    continue;
                }
                auto [it, fresh] = gone.try_emplace(s->userData, PxBounds3::empty());
                it->second.include(PxShapeExt::getWorldBounds(*s, *e.actor));
                e.actor->detachShape(*s);
            }
            for (auto& [native, bounds] : gone)  // what rested on those colliders may have lost its support
                if (g_active && !bounds.isEmpty()) g_changed.push_back(bounds);
        }
    }
    e.kinematic = kinematic;
    if (kinematic)
        g_proxies.insert(body);
    else
        g_proxies.erase(body);
    e.builtTick = g_tick;
    uint32_t boxes = keptBoxes, voxels = 0;
    size_t budget = size_t(std::max(1, g_config.pxBoxBudget));
    // where a slow build's time goes (diagnostics)
    const uint64_t tShapes = trace::Now();
    uint64_t tDecompose = 0, tHull = 0, tBoxes = 0, maxCells = 0;
    size_t pieces = 0;
    int shapes = 0, coarse = 1;
    for (const ShapeSig& s : sig) {
        if (!kept.empty() && kept.count(s.shape)) {
            voxels += s.solid;
            continue;
        }
        px::VoxGrid g = Grid(s.vox);
        if (!GridOk(g)) continue;
        ++shapes;
        maxCells = std::max(maxCells, uint64_t(g.n[0]) * uint64_t(g.n[1]) * uint64_t(g.n[2]));
        uint64_t a = trace::Now();
        // huge grids start coarse (DecomposeShape); a bounding box would hold debris up in the air
        coarse = std::max(coarse, px::DecomposeShape(g, kinematic ? budget * 8 : budget, g_boxes));
        uint64_t b = trace::Now();
        tDecompose += b - a;
        pieces += g_boxes.size();
        PxRigidDynamic* to = split ? NewActor(body, e.placed, true, false) : e.actor;
        if (!to) continue;
        uint32_t added =
            g_config.pxHull && !kinematic ? AddHulls(*to, g_boxes, ToPx(s.local), td::VoxSize(s.vox), s.shape) : 0;
        uint64_t c = trace::Now();
        tHull += c - b;  // also hulls tried and refused
        if (!added) {
            added = AddBoxes(*to, g_boxes, ToPx(s.local), td::VoxSize(s.vox), s.shape);
            tBoxes += trace::Now() - c;
        }
        boxes += added;
        voxels += s.solid;
        if (split) {
            scene->addActor(*to);
            e.parts.push_back({s.shape, to, added});
        }
    }
    if (!kinematic)
        SetMass(*e.actor, body);
    else if (!split)
        CapProxy(e.actor, body);  // (carving changes its mass)
    Unown(body, e.sig);
    for (const ShapeSig& ss : sig) g_shapeOwner[ss.shape] = body;
    e.sig = sig;
    e.boxes = boxes;
    e.voxels = voxels;
    float mn[3], mx[3];  // (an anchor has no colliders: the native bounds)
    if (!split)
        e.radius = 0.5f * e.actor->getWorldBounds().getDimensions().magnitude();
    else if (BodyAabb(body, mn, mx))
        e.radius = 0.5f * PxVec3(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]).magnitude();
    const uint64_t t1 = trace::Now();
    const double ms = Ms(t0, t1);
    g_win.bodyBuildMs += ms;
    g_tickBodyBuildMs += ms;
    ++g_win.bodyBuilds;
    static uint32_t slowLogged = 0;
    if (ms > 20.0 && (++slowLogged <= 40 || slowLogged % 100 == 0))
        LOG("shadow: slow %s build %.1f ms (body %d, %d shapes rebuilt and %zu kept%s, %u voxels, largest grid %llu "
            "cells, %zu boxes from the decomposition at 1/%d, %u colliders, %u of them kept): decompose %.1f, hull "
            "%.1f, boxes %.1f, before %.1f, after %.1f",
            kinematic ? "proxy" : "debris", ms, Handle(body), shapes, kept.size(), split ? ", an actor each" : "",
            voxels, maxCells, pieces, coarse, boxes, keptBoxes, Ms(0, tDecompose), Ms(0, tHull), Ms(0, tBoxes),
            Ms(t0, tShapes), Ms(tShapes, t1) - Ms(0, tDecompose + tHull + tBoxes));
}

// A dynamic actor becomes a proxy without rebuilding its colliders (offload hand-back).
void MakeKinematic(BodyEntry& e, uint8_t* body) {
    Unfreeze(e);
    e.actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    CapProxy(e.actor, body);
    e.actor->setGlobalPose(ToPx(BodyTr(body)));
    e.placed = BodyTr(body);
    e.kinematic = true;
    g_proxies.insert(body);
}

// World AABB of a body (union of its shapes' engine-maintained bounds).
bool BodyAabb(uint8_t* body, float mn[3], float mx[3]) {
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        mn[k] = 1e30f;
        mx[k] = -1e30f;
    }
    int guard = 0;
    for (uint8_t* s = FirstShape(body); s && guard < 4096; s = NextShape(s), ++guard) {
        if (td::EntityType(s) != 2) break;
        if (!ShapePhysical(s)) continue;
        const float* a = td::ShapeAabbMin(s);
        const float* b = td::ShapeAabbMax(s);
        bool sane = true;
        for (int k = 0; k < 3 && sane; ++k)
            sane = std::isfinite(a[k]) && std::isfinite(b[k]) && a[k] <= b[k] && b[k] - a[k] < 500.0f &&
                   std::fabs(a[k]) < 1e5f;
        if (!sane) continue;
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], a[k]);
            mx[k] = std::max(mx[k], b[k]);
        }
        any = true;
    }
    return any;
}

int64_t CellKey(int x, int y, int z) {
    return (int64_t(x & 0x1FFFFF) << 42) | (int64_t(y & 0x1FFFFF) << 21) | int64_t(z & 0x1FFFFF);
}

// A static shape into the grid's cells by its world bounds now. `again`: also when the grid has it already (a body
// that moved: the cells it left keep pointing at it, which only costs NeedStatic a bounds test). False when it was
// there already, or its bounds are unusable or span too many cells.
bool GridAdd(uint8_t* s, bool again) {
    const float* a = td::ShapeAabbMin(s);
    const float* c = td::ShapeAabbMax(s);
    for (int k = 0; k < 3; ++k)
        if (!(std::isfinite(a[k]) && std::isfinite(c[k]) && a[k] <= c[k] && std::fabs(a[k]) < 1e5f &&
              std::fabs(c[k]) < 1e5f))
            return false;
    int lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = int(std::floor(a[k] / kCell));
        hi[k] = int(std::floor(c[k] / kCell));
    }
    if (int64_t(hi[0] - lo[0] + 1) * (hi[1] - lo[1] + 1) * (hi[2] - lo[2] + 1) > 200000) return false;
    if (!g_gridShapes.insert(s).second && !again) return false;
    g_deadShapes.erase(s);  // (a new shape where a destroyed one was)
    for (int z = lo[2]; z <= hi[2]; ++z)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int x = lo[0]; x <= hi[0]; ++x) g_grid[CellKey(x, y, z)].push_back(s);
    return true;
}

void BuildGrid() {
    g_grid.clear();
    g_gridShapes.clear();
    g_deadShapes.clear();
    AcquireSRWLockExclusive(&g_newLock);  // the grid has every static shape now
    g_newShapes.clear();
    ReleaseSRWLockExclusive(&g_newLock);
    int n = 0;
    uint8_t** shapes = td::SceneList<uint8_t*>(0x238, n);
    int added = 0;
    float minY = 1e30f;
    for (int i = 0; shapes && i < n; ++i) {
        uint8_t* s = shapes[i];
        if (!s || td::EntityType(s) != 2 || !ShapePhysical(s)) continue;
        uint8_t* b = ShapeBody(s);
        if (!b || BodyDynamic(b) || !td::ShapeVoxels(s)) continue;
        const float* a = td::ShapeAabbMin(s);
        if (std::isfinite(a[1]) && std::fabs(a[1]) < 1e5f) minY = std::min(minY, a[1]);
        added += GridAdd(s, false);
    }
    g_gridDirty = false;
    g_gridTick = g_tick;
    if (minY < 1e29f) g_worldMinY = minY;
    static int logged = 0;
    if (logged++ < 3 || g_tick % 600 == 0)
        LOG("shadow: static grid: %d static shapes in %zu cells, lowest at y %.1f", added, g_grid.size(), g_worldMinY);
}

void BuildBlock(uint8_t* shape, StaticShape& ss, uint32_t key, const int lo[3], const int hi[3], Block& b) {
    uint64_t t0 = trace::Now();
    ReleaseBlock(b);
    px::VoxGrid g = Grid(ss.vox);
    g_boxes.clear();
    bool complete = px::DecomposeRegion(g, lo, hi, size_t(std::max(1, g_config.pxBlockBoxes)), g_boxes);
    b.hash = px::HashRegion(g, lo, hi);
    b.boxes = 0;
    if (!g_boxes.empty()) {
        b.actor = px::Physics()->createRigidStatic(ShapeWorld(shape));
        if (b.actor) {
            b.boxes = AddBoxes(*b.actor, g_boxes, PxTransform(PxIdentity), td::VoxSize(ss.vox), shape);
            b.agg = g_staticAggregates ? px::Physics()->createAggregate(
                                             1, b.boxes + 1, PxGetAggregateFilterHint(PxAggregateType::eSTATIC, false))
                                       : nullptr;
            if (b.agg && b.agg->addActor(*b.actor)) {
                px::Scene()->addAggregate(*b.agg);
            } else {
                if (b.agg) b.agg->release();
                b.agg = nullptr;
                px::Scene()->addActor(*b.actor);
            }
        }
    }
    static int incomplete = 0;
    if (!complete && ++incomplete <= 10) LOG("shadow: static block %08x over the box budget (%u boxes)", key, b.boxes);
    double ms = Ms(t0, trace::Now());
    g_win.blockBuildMs += ms;
    g_tickBuildMs += ms;
    ++g_win.blockBuilds;
}

// Make sure the static blocks overlapping a world box exist and match the voxels. kNeedBudget: this tick's build
// budget ran out first (ask again next tick; `force` ignores the budget). kNeedSkipped: part of the box was too
// large to build (a body that size isn't debris).
enum Need { kNeedDone, kNeedBudget, kNeedSkipped };
Need NeedStatic(const float mn[3], const float mx[3], bool force) {
    if (!force && g_tickBuildMs >= g_config.pxBuildMsPerTick) return kNeedBudget;
    int lo[3], hi[3];
    int64_t cells = 1;
    for (int k = 0; k < 3; ++k) {
        lo[k] = int(std::floor(mn[k] / kCell));
        hi[k] = int(std::floor(mx[k] / kCell));
        cells *= int64_t(hi[k] - lo[k] + 1);
    }
    if (cells > 4096) {  // don't sweep half the map
        ++g_skippedQueries;
        return kNeedSkipped;
    }
    static std::vector<uint8_t*> cand;
    cand.clear();
    for (int z = lo[2]; z <= hi[2]; ++z)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int x = lo[0]; x <= hi[0]; ++x) {
                auto it = g_grid.find(CellKey(x, y, z));
                if (it == g_grid.end()) continue;
                cand.insert(cand.end(), it->second.begin(), it->second.end());
            }
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    const int bs = std::max(4, g_config.pxBlockSize);
    bool skipped = false;
    for (uint8_t* s : cand) {
        if (!g_deadShapes.empty() && g_deadShapes.count(s)) continue;  // destroyed: the pointer is stale
        const float* a = td::ShapeAabbMin(s);
        const float* c = td::ShapeAabbMax(s);
        if (a[0] > mx[0] || a[1] > mx[1] || a[2] > mx[2] || c[0] < mn[0] || c[1] < mn[1] || c[2] < mn[2]) continue;
        uint8_t* vox = td::ShapeVoxels(s);
        uint8_t* owner = ShapeBody(s);
        // emptied, became debris (reparented), or its body moves (a kinematic actor follows it: g_moving)
        if (!vox || !owner || BodyDynamic(owner) || g_moving.count(owner)) {
            auto it = g_static.find(s);
            if (it != g_static.end()) {
                ReleaseStatic(it->second);
                g_static.erase(it);
            }
            continue;
        }
        auto [sit, inserted] = g_static.try_emplace(s);
        if (inserted) g_staticOrder.push_back(s);
        StaticShape& ss = sit->second;
        if (ss.vox != vox || ss.owner != owner) {  // a new voxel buffer or body: rebuild everything of this shape
            ReleaseStatic(ss);
            ss.vox = vox;
            ss.owner = owner;
            ss.solid = VoxSolid(vox);
            ss.inv = ShapeWorld(s).getInverse();
            ss.filter = GameFilter(s);
            g_staticPose.try_emplace(owner, BodyTr(owner));  // WatchStatics: blocks stay right while it stays put
        }
        px::VoxGrid g = Grid(vox);
        if (!GridOk(g)) continue;
        float size = td::VoxSize(vox);
        if (!(size > 0.0f)) continue;
        // the query box in the shape's voxel coordinates
        const PxTransform& inv = ss.inv;
        float vlo[3] = {1e30f, 1e30f, 1e30f}, vhi[3] = {-1e30f, -1e30f, -1e30f};
        for (int i = 0; i < 8; ++i) {
            PxVec3 p(i & 1 ? mx[0] : mn[0], i & 2 ? mx[1] : mn[1], i & 4 ? mx[2] : mn[2]);
            PxVec3 l = inv.transform(p) * (1.0f / size);
            const float* lp = &l.x;
            for (int k = 0; k < 3; ++k) {
                vlo[k] = std::min(vlo[k], lp[k]);
                vhi[k] = std::max(vhi[k], lp[k]);
            }
        }
        int b0[3], b1[3];
        bool empty = false;
        for (int k = 0; k < 3; ++k) {
            int v0 = std::max(0, int(std::floor(vlo[k]))), v1 = std::min(g.n[k] - 1, int(std::floor(vhi[k])));
            if (v0 > v1) empty = true;
            b0[k] = v0 / bs;
            b1[k] = v1 / bs;
        }
        if (empty) continue;
        if (int64_t(b1[0] - b0[0] + 1) * (b1[1] - b0[1] + 1) * (b1[2] - b0[2] + 1) > 256) {
            ++g_skippedQueries;
            skipped = true;
            continue;
        }
        for (int bz = b0[2]; bz <= b1[2]; ++bz)
            for (int by = b0[1]; by <= b1[1]; ++by)
                for (int bx = b0[0]; bx <= b1[0]; ++bx) {
                    uint32_t key = uint32_t(bx) | (uint32_t(by) << 10) | (uint32_t(bz) << 20);
                    auto it = ss.blocks.find(key);
                    if (it != ss.blocks.end()) continue;  // built; changes are handled in RefreshStatic
                    int lo3[3] = {bx * bs, by * bs, bz * bs};
                    int hi3[3] = {std::min(g.n[0], lo3[0] + bs), std::min(g.n[1], lo3[1] + bs),
                                  std::min(g.n[2], lo3[2] + bs)};
                    BuildBlock(s, ss, key, lo3, hi3, ss.blocks[key]);
                    if (!force && g_tickBuildMs >= g_config.pxBuildMsPerTick) return kNeedBudget;
                }
    }
    return skipped ? kNeedSkipped : kNeedDone;
}

// Static shapes whose voxels changed (holes): re-check their built blocks, rebuild the changed ones. A shape with
// a new voxel buffer loses all its blocks, and every body asks for its surroundings again: bodies resting inside
// their last query box would otherwise keep colliding with the old voxels.
uint64_t g_rebuffered = 0;
// New filter data on every collider of an actor in the scene; PhysX runs the filter again for its pairs.
void Refilter(PxRigidActor* a, const PxFilterData& filter) {
    if (!a) return;
    static std::vector<PxShape*> shapes;
    shapes.resize(a->getNbShapes());
    a->getShapes(shapes.data(), PxU32(shapes.size()));
    for (PxShape* s : shapes) s->setSimulationFilterData(filter);
    if (a->getScene()) a->getScene()->resetFiltering(*a);
}
// One static shape: false when it should leave g_static. A shape whose body became dynamic (or that moved to
// another body) loses its blocks, which would otherwise keep holding debris up where it used to be; carved blocks
// are re-hashed and rebuilt.
bool RefreshOne(uint8_t* s, StaticShape& ss, std::vector<Box3>& requery) {
    const int bs = std::max(4, g_config.pxBlockSize);
    uint8_t* owner = ShapeBody(s);
    bool rebuffered = ss.vox && td::ShapeVoxels(s) != ss.vox;
    bool loose = !owner || owner != ss.owner || BodyDynamic(owner) || g_moving.count(owner);
    if (rebuffered || loose) {
        ReleaseStatic(ss);
        if (rebuffered) ++g_rebuffered;
        if (loose) ++g_off.staticReleased;
        // still static but needs new blocks: bodies whose last query covered it ask again
        if (owner && !BodyDynamic(owner) && !g_moving.count(owner)) {
            Box3 q;
            for (int k = 0; k < 3; ++k) {
                q.mn[k] = td::ShapeAabbMin(s)[k];
                q.mx[k] = td::ShapeAabbMax(s)[k];
            }
            requery.push_back(q);
        }
        return false;
    }
    if (!ss.vox) return true;
    // a script changed its collision filter (SetShapeCollisionFilter): its blocks' colliders take the new one
    const PxFilterData filter = GameFilter(s);
    if (!(filter == ss.filter)) {
        ss.filter = filter;
        for (auto& [key, b] : ss.blocks) Refilter(b.actor, filter);
        static int logged = 0;
        if (logged++ < 20)
            LOG("shadow: static shape %d of body %d: collision filter now layer %02x, mask %02x, group %u%s",
                Handle(s), Handle(owner), filter.word0, filter.word1, filter.word2,
                filter.word0 ? "" : " (collides with nothing)");
    }
    uint32_t solid = VoxSolid(ss.vox);
    if (solid == ss.solid) return true;
    ss.solid = solid;
    px::VoxGrid g = Grid(ss.vox);
    for (auto& [key, b] : ss.blocks) {
        int lo[3] = {int(key & 1023) * bs, int((key >> 10) & 1023) * bs, int(key >> 20) * bs};
        int hi[3] = {std::min(g.n[0], lo[0] + bs), std::min(g.n[1], lo[1] + bs), std::min(g.n[2], lo[2] + bs)};
        ++g_win.blockChecks;
        if (px::HashRegion(g, lo, hi) != b.hash) BuildBlock(s, ss, key, lo, hi, b);
    }
    return true;
}

// Static shapes the breakage driver is about to change are checked the next tick (OnShapesModified), every other
// one once every 8 ticks (round robin): looking at all ~5,000 every tick cost up to a millisecond.
void RefreshStatic(std::vector<Box3>& requery) {
    for (uint8_t* s : g_modified) {
        auto it = g_static.find(s);
        if (it != g_static.end() && !RefreshOne(s, it->second, requery)) g_static.erase(it);
    }
    g_modified.clear();
    size_t n = g_staticOrder.size();
    size_t todo = std::min(n, std::max<size_t>(64, n / 8));
    for (size_t k = 0; k < todo && !g_staticOrder.empty(); ++k) {
        if (g_staticNext >= g_staticOrder.size()) g_staticNext = 0;
        uint8_t* s = g_staticOrder[g_staticNext];
        auto it = g_static.find(s);
        bool keep = it != g_static.end() && RefreshOne(s, it->second, requery);
        if (keep) {
            ++g_staticNext;
            continue;
        }
        if (it != g_static.end()) g_static.erase(it);
        g_staticOrder[g_staticNext] = g_staticOrder.back();
        g_staticOrder.pop_back();
    }
}

// Static geometry appeared, went or changed in these boxes: bodies whose last static query covered one query again
// (StaticsAround builds what is there now).
void Requery(const std::vector<Box3>& boxes) {
    if (boxes.empty()) return;
    Box3 all = boxes[0];
    for (const Box3& q : boxes)
        for (int k = 0; k < 3; ++k) {
            all.mn[k] = std::min(all.mn[k], q.mn[k]);
            all.mx[k] = std::max(all.mx[k], q.mx[k]);
        }
    for (auto& [b, e] : g_bodies) {
        if (!Overlap(all, e.qmn, e.qmx)) continue;
        bool hit = false;
        for (const Box3& q : boxes) hit |= Overlap(q, e.qmn, e.qmx);
        if (!hit) continue;
        for (int k = 0; k < 3; ++k) {
            e.qmn[k] = 1e30f;
            e.qmx[k] = -1e30f;
        }
    }
}

// The static blocks around a body (its bounds plus a margin that grows with speed). Re-queried when the body
// leaves the box of the last query. True when every block in that box exists.
bool StaticsAround(BodyEntry& e, const float mn[3], const float mx[3], const float v[3], bool force) {
    bool inside = true;
    for (int k = 0; k < 3 && inside; ++k) inside = mn[k] >= e.qmn[k] && mx[k] <= e.qmx[k];
    if (inside) return e.staticsOk;
    float pad = 1.5f + 2.0f * kDt * std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float qmn[3], qmx[3];
    for (int k = 0; k < 3; ++k) {
        qmn[k] = mn[k] - pad;
        qmx[k] = mx[k] + pad;
    }
    Need r = NeedStatic(qmn, qmx, force);
    if (r == kNeedBudget) return false;  // again next tick
    std::memcpy(e.qmn, qmn, sizeof(qmn));
    std::memcpy(e.qmx, qmx, sizeof(qmx));
    e.staticsOk = r == kNeedDone;
    return e.staticsOk;
}

// Shapes of a dynamic body worth a collider: physical, with voxels. False when it has none.
bool Signature(uint8_t* b, std::vector<ShapeSig>& sig, uint32_t& voxels, bool& jointed) {
    sig.clear();
    voxels = 0;
    jointed = false;
    int guard = 0;
    for (uint8_t* s = FirstShape(b); s && guard < 4096; s = NextShape(s), ++guard) {
        if (td::EntityType(s) != 2) break;
        if (ShapeJointed(s)) jointed = true;
        if (!ShapePhysical(s)) continue;
        uint8_t* vox = td::ShapeVoxels(s);
        if (!vox) continue;
        ShapeSig ss{};
        ss.shape = s;
        ss.vox = vox;
        ss.solid = VoxSolid(vox);
        const int* d = td::VoxDims(vox);
        for (int k = 0; k < 3; ++k) ss.n[k] = d[k];
        ss.local = ShapeLocal(s);
        ss.filter = GameFilter(s);
        voxels += ss.solid;
        sig.push_back(ss);
    }
    return !sig.empty() && voxels > 0;
}

// ---- static geometry that appears or moves while the level runs ------------------------------------------------

bool SameSig(const std::vector<ShapeSig>& a, const std::vector<ShapeSig>& b);

// The game made a shape (td::OnShapeCreated; any thread): looked at next tick, when it is filled in and linked.
void OnNewShape(uint8_t* s) {
    if (!g_active) return;
    AcquireSRWLockExclusive(&g_newLock);
    g_newShapes.insert(s);
    ReleaseSRWLockExclusive(&g_newLock);
}

Box3 ShapeBox(uint8_t* s) {
    Box3 q;
    for (int k = 0; k < 3; ++k) {
        q.mn[k] = td::ShapeAabbMin(s)[k];
        q.mx[k] = td::ShapeAabbMax(s)[k];
    }
    return q;
}

// Static shapes made since the last tick: pieces of a carved static shape that still hold on (a split adds them to
// the static body), a script's static spawn. Into the grid now rather than at its next rebuild.
void IndexNewStatics(std::vector<Box3>& requery) {
    static std::vector<uint8_t*> fresh;
    AcquireSRWLockExclusive(&g_newLock);
    fresh.assign(g_newShapes.begin(), g_newShapes.end());
    g_newShapes.clear();
    ReleaseSRWLockExclusive(&g_newLock);
    for (uint8_t* s : fresh) {
        if (td::EntityType(s) != 2 || !ShapePhysical(s) || !td::ShapeVoxels(s)) continue;
        uint8_t* b = ShapeBody(s);
        if (!b || BodyDynamic(b)) continue;  // debris: the body scan has it
        if (!GridAdd(s, false)) continue;
        requery.push_back(ShapeBox(s));
        ++g_off.newStatics;
    }
}

// A tracked body the game made static (SetBodyDynamic, or destruction): its shapes become static geometry, which the
// grid never had. Bodies around it query again next tick.
std::vector<Box3> g_requeryNext;
void IndexStaticBody(uint8_t* b) {
    bool any = false;
    int guard = 0;
    for (uint8_t* s = FirstShape(b); s && guard < 4096; s = NextShape(s), ++guard) {
        if (td::EntityType(s) != 2) break;
        if (ShapePhysical(s) && td::ShapeVoxels(s)) any |= GridAdd(s, true);
    }
    Box3 q;
    if (any && BodyAabb(b, q.mn, q.mx)) g_requeryNext.push_back(q);
    g_off.madeStatic += any;
}

// A moving static body's kinematic actor, at m.placed, colliders from `sig` (as a proxy's: boxes).
void BuildMoving(uint8_t* b, MovingStatic& m, const std::vector<ShapeSig>& sig) {
    ReleaseActor(m.actor);
    m.sig = sig;
    m.actor = NewActor(b, m.placed, true, false);
    if (!m.actor) return;
    const size_t budget = size_t(std::max(1, g_config.pxBoxBudget)) * 8;
    for (const ShapeSig& s : sig) {
        px::VoxGrid g = Grid(s.vox);
        if (!GridOk(g)) continue;
        px::DecomposeShape(g, budget, g_boxes);
        AddBoxes(*m.actor, g_boxes, ToPx(s.local), td::VoxSize(s.vox), s.shape);
    }
    px::Scene()->addActor(*m.actor);
}

// A static body's pose changed, beyond float noise (0.1 mm, or about 0.05 degrees).
bool Moved(const Tr& a, const Tr& b) {
    const float dx = a.p[0] - b.p[0], dy = a.p[1] - b.p[1], dz = a.p[2] - b.p[2];
    if (dx * dx + dy * dy + dz * dz > 1e-8f) return true;
    const float dot = a.q[0] * b.q[0] + a.q[1] * b.q[1] + a.q[2] * b.q[2] + a.q[3] * b.q[3];
    return std::fabs(dot) < 1.0f - 1e-7f;
}

// The static blocks of one body go (what rested on them is woken: ReleaseNow).
void ReleaseBodyBlocks(uint8_t* b) {
    for (auto it = g_static.begin(); it != g_static.end();) {
        if (it->second.owner != b) {
            ++it;
            continue;
        }
        ReleaseStatic(it->second);
        it = g_static.erase(it);
    }
}

// Bodies with more shapes than this aren't followed (the level's own geometry, should a script move it): their
// blocks are built again where they are.
constexpr size_t kMaxMovingShapes = 256;

// Every tick, before the scan: a static body with blocks that moved (from where its blocks were built) loses them,
// and a kinematic actor takes over, still where the blocks were, for SyncMoving to sweep to the body. Moving ones
// whose shapes changed get new colliders; those still for kStillTicks go back to static blocks (built around them
// this tick: bodies near them query again), and ones the game made dynamic leave (the body scan takes them).
void WatchStatics(std::vector<Box3>& requery) {
    static std::vector<ShapeSig> sig;
    for (auto it = g_staticPose.begin(); it != g_staticPose.end();) {
        uint8_t* b = it->first;
        if (BodyDynamic(b)) {  // (RefreshOne lets its blocks go)
            it = g_staticPose.erase(it);
            continue;
        }
        const Tr& now = BodyTr(b);
        if (!Moved(now, it->second)) {
            ++it;
            continue;
        }
        const Tr from = it->second;
        ReleaseBodyBlocks(b);
        uint32_t voxels = 0;
        bool jointed = false;
        const bool any = Signature(b, sig, voxels, jointed);
        if (any && sig.size() > kMaxMovingShapes) {
            Box3 q;
            if (BodyAabb(b, q.mn, q.mx)) requery.push_back(q);
            it->second = now;
            continue;
        }
        it = g_staticPose.erase(it);
        if (!any) continue;
        MovingStatic& m = g_moving[b];
        m.placed = from;
        m.moved = g_tick;
        BuildMoving(b, m, sig);
        ++g_off.movingStarted;
        static int logged = 0;
        if (logged++ < 20)
            LOG("offload: static body %d moved (%.1f cm since its blocks were built): a kinematic actor follows it "
                "(%zu shapes, %u voxels)",
                Handle(b), 100.0f * (ToPx(now).p - ToPx(from).p).magnitude(), sig.size(), voxels);
    }
    for (auto it = g_moving.begin(); it != g_moving.end();) {
        uint8_t* b = it->first;
        MovingStatic& m = it->second;
        uint32_t voxels = 0;
        bool jointed = false;
        if (BodyDynamic(b) || !Signature(b, sig, voxels, jointed)) {
            ReleaseActor(m.actor);
            it = g_moving.erase(it);
            continue;
        }
        if (g_tick - m.moved > kStillTicks) {
            Box3 q;
            if (BodyAabb(b, q.mn, q.mx)) requery.push_back(q);
            for (const ShapeSig& s : sig) GridAdd(s.shape, true);  // the grid has it where it stopped
            ReleaseActor(m.actor);  // (what rested on it wakes and lands on the blocks built for it)
            it = g_moving.erase(it);
            ++g_off.movingStopped;
            continue;
        }
        if (!SameSig(m.sig, sig)) BuildMoving(b, m, sig);  // carved, or a shape came or went
        ++it;
    }
}

// After the native step: moving static bodies' actors follow them, swept from where they were placed (pushing and
// carrying debris), or put there when the body jumped more than 5 m (teleported). Sleeping debris on or around them
// is woken (WakeAroundChanges).
void SyncMoving() {
    for (auto& [b, m] : g_moving) {
        const Tr& now = BodyTr(b);
        if (!m.actor || std::memcmp(&now, &m.placed, sizeof(Tr)) == 0) continue;
        const PxTransform target = ToPx(now), from = ToPx(m.placed);
        PxBounds3 bounds = m.actor->getWorldBounds();
        g_changed.push_back(bounds);
        bounds.minimum += target.p - from.p;
        bounds.maximum += target.p - from.p;
        g_changed.push_back(bounds);
        if ((target.p - from.p).magnitude() > 5.0f)
            m.actor->setGlobalPose(target);
        else
            m.actor->setKinematicTarget(target);
        m.placed = now;
        m.moved = g_tick;
    }
}

// A body is destroyed: its static bookkeeping goes.
void ForgetStatic(uint8_t* b) {
    g_staticPose.erase(b);
    auto it = g_moving.find(b);
    if (it == g_moving.end()) return;
    ReleaseActor(it->second.actor);
    g_moving.erase(it);
}

// The static side before the scan, in this order: blocks of static bodies that moved go, blocks of changed shapes
// are rebuilt, static shapes that appeared join the grid; then bodies near any of those places query again.
void UpdateStatics() {
    static std::vector<Box3> requery;
    requery.clear();
    requery.swap(g_requeryNext);
    WatchStatics(requery);
    RefreshStatic(requery);
    IndexNewStatics(requery);
    Requery(requery);
}

void Scan() {
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    static std::vector<ShapeSig> sig;
    const uint32_t maxVox = uint32_t(std::max(1, g_config.pxMaxBodyVoxels));
    g_proxyJointed = g_proxyLarge = 0;
    for (int i = 0; bodies && i < n; ++i) {
        uint8_t* b = bodies[i];
        if (!b || td::EntityType(b) != 1 || Handle(b) <= 0 || !BodyDynamic(b)) continue;
        uint32_t voxels;
        bool jointed;
        if (!Signature(b, sig, voxels, jointed)) continue;
        bool kinematic = jointed || voxels > maxVox;
        g_proxyJointed += jointed;
        g_proxyLarge += !jointed && voxels > maxVox;
        BodyEntry& e = EntryAt(i, b);
        bool same = e.actor && e.kinematic == kinematic && e.sig.size() == sig.size();
        for (size_t k = 0; same && k < sig.size(); ++k) same = Same(e.sig[k], sig[k]);
        if (!same) BuildBody(e, b, sig, kinematic);
        if (!e.actor) continue;
        e.seen = g_tick;
        e.pre = BodyTr(b);
        std::memcpy(e.v, BodyVel(b), sizeof(e.v));
        std::memcpy(e.w, BodyAngVel(b), sizeof(e.w));
        e.awake = BodyAwake(b);
        if (kinematic) continue;
        // static surroundings, re-queried when the body leaves the box of the last query
        float mn[3], mx[3];
        if (g_config.pxShadow == 2 && !e.fresh) {  // free mode: around PhysX's own copy of the body
            PxBounds3 wb = e.actor->getWorldBounds();
            for (int k = 0; k < 3; ++k) {
                mn[k] = (&wb.minimum.x)[k];
                mx[k] = (&wb.maximum.x)[k];
            }
        } else if (!BodyAabb(b, mn, mx)) {
            continue;
        }
        bool inside = true;
        for (int k = 0; k < 3 && inside; ++k) inside = mn[k] >= e.qmn[k] && mx[k] <= e.qmx[k];
        if (inside) continue;
        float pad = 1.0f + 2.0f * kDt * std::sqrt(e.v[0] * e.v[0] + e.v[1] * e.v[1] + e.v[2] * e.v[2]);
        float qmn[3], qmx[3];
        for (int k = 0; k < 3; ++k) {
            qmn[k] = mn[k] - pad;
            qmx[k] = mx[k] + pad;
        }
        if (NeedStatic(qmn, qmx, false) != kNeedBudget) {
            std::memcpy(e.qmn, qmn, sizeof(qmn));
            std::memcpy(e.qmx, qmx, sizeof(qmx));
        }
    }
    for (auto it = g_bodies.begin(); it != g_bodies.end();) {
        if (it->second.seen != g_tick) {
            it = EraseBody(it);
        } else {
            ++it;
        }
    }
}

// ---- offload (Shadow=3) ------------------------------------------------------------------------------------

// The game puts an owned body to sleep: its integrator, solver and broadphase skip it. The per-body pass at the
// start of Physics::update then zeroes its velocities and leaves its pose alone.
void SleepNative(uint8_t* b) {
    if (!SleepCounter(b)) return;
    SleepCounter(b) = 0;
    UpdateShapes(b);  // into the sleeping-body trees
}

void Expect(uint8_t* b, BodyEntry& e) {
    e.expTr = BodyTr(b);
    std::memcpy(e.expV, BodyVel(b), sizeof(e.expV));
    std::memcpy(e.expW, BodyAngVel(b), sizeof(e.expW));
}

void TakeOver(uint8_t* b, BodyEntry& e) {
    e.actor->setGlobalPose(ToPx(BodyTr(b)));
    e.actor->setLinearVelocity(V(BodyVel(b)));
    e.actor->setAngularVelocity(V(BodyAngVel(b)));
    if (BodyAwake(b))
        e.actor->wakeUp();
    else
        e.actor->putToSleep();
    e.physxAwake = BodyAwake(b);
    e.owned = true;
    e.ownedSince = g_tick;
    g_owned[b] = &e;
    ++g_ownedVersion;
    SleepNative(b);
    Expect(b, e);
    float mn[3], mx[3];
    e.bounds = BodyAabb(b, mn, mx) ? PxBounds3(V(mn), V(mx)) : PxBounds3::empty();
    ++g_off.takeovers;
}

// Before the step: the body holds what PhysX wrote last unless something changed it since (a script, a tool, an
// explosion, destruction waking its neighbours). Collect reads those changes as differences from what was written;
// Apply hands them to PhysX. With the async step the write-back of the step that ran meanwhile comes between the
// two, so velocity changes are added to PhysX's newer velocities rather than replacing them.
// The comparison alone: reads the body, writes only the entry's fwd fields (safe on several threads at once).
void CollectForwardPure(uint8_t* b, BodyEntry& e) {
    const Tr& now = BodyTr(b);
    const float* v = BodyVel(b);
    const float* w = BodyAngVel(b);
    e.fwdMoved = std::memcmp(&now, &e.expTr, sizeof(Tr)) != 0;
    if (e.fwdMoved) e.fwdPose = now;
    e.fwdPushed = std::memcmp(v, e.expV, sizeof(e.expV)) != 0 || std::memcmp(w, e.expW, sizeof(e.expW)) != 0;
    if (e.fwdPushed)
        for (int k = 0; k < 3; ++k) {
            e.fwdDv[k] = v[k] - e.expV[k];
            e.fwdDw[k] = w[k] - e.expW[k];
        }
    e.fwdWoken = BodyAwake(b);
}

// The rest, on the main thread: counters, diagnostics, and a woken body goes back to sleep in the game.
void CollectForwardFinish(uint8_t* b, BodyEntry& e) {
    if (e.fwdMoved) ++g_off.poseDiffs;
    if (e.fwdPushed) {
        ++g_off.velDiffs;
        const float* v = BodyVel(b);
        e.lastForward = V(v).magnitude();
        if (e.lastForward > 50.0f && g_diagForwards++ < 30)
            LOG("offload diag: tick %u, something set body %d to %.1f m/s, %.1f rad/s (we left %.1f m/s); %u voxels, "
                "mass %.3g", g_tick, Handle(b), e.lastForward, V(BodyAngVel(b)).magnitude(), V(e.expV).magnitude(),
                e.voxels, BodyInvMass(b) > 0 ? 1.0f / BodyInvMass(b) : 0.0f);
    }
    if (e.fwdWoken) {
        ++g_off.nativeWakes;
        SleepNative(b);
    }
}

void CollectForward(uint8_t* b, BodyEntry& e) {
    CollectForwardPure(b, e);
    if (e.fwdMoved || e.fwdPushed || e.fwdWoken) CollectForwardFinish(b, e);
}

void ApplyForward(BodyEntry& e) {
    if (e.fwdMoved || e.fwdPushed || e.fwdWoken) Thaw(e);  // the game moved, pushed or woke it
    if (e.fwdMoved) e.actor->setGlobalPose(ToPx(e.fwdPose));
    if (e.fwdPushed) {
        e.actor->setLinearVelocity(e.actor->getLinearVelocity() + V(e.fwdDv));
        e.actor->setAngularVelocity(e.actor->getAngularVelocity() + V(e.fwdDw));
    }
    if ((e.fwdMoved || e.fwdPushed || e.fwdWoken) && e.actor->isSleeping()) e.actor->wakeUp();
    e.fwdMoved = e.fwdPushed = e.fwdWoken = false;
}

// The game takes a body back before the step. Its fields hold PhysX's last write-back (after a watchdog trip, the
// last good one); waking it now keeps that velocity, since the per-body pass zeroes sleeping bodies only.
void HandBack(uint8_t* b, BodyEntry& e, int why) {
    Unfreeze(e);  // (its actor becomes a proxy: kinematic already)
    e.owned = false;
    g_ownedVersion += g_owned.erase(b);
    WakeNative(b);
    e.holdUntil = g_tick + uint32_t(std::max(0, g_config.pxRegainTicks)) * (e.bad ? 10u : 1u);
    e.bad = false;
    ++g_off.handBacks[why];
}

bool BodyBox(uint8_t* b, float margin, Box3& out) {
    if (!BodyAabb(b, out.mn, out.mx)) return false;
    for (int k = 0; k < 3; ++k) {
        out.mn[k] -= margin;
        out.mx[k] += margin;
    }
    return true;
}
float Speed(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

using PointInWaterFn = bool(__fastcall*)(void* scene, const float* point, float* depth, void** water);
bool g_waterOk = false;  // the engine's point-in-water test matched at install

// The scan's per-body reads, done first on PhysX's idle threads (they only read the game's bodies and our entries):
// signature, bounds, and whether the signature still matches the entry the list cache has for that slot.
struct ScanItem {
    std::vector<ShapeSig> sig;
    uint32_t voxels = 0;
    float mn[3]{}, mx[3]{};
    bool valid = false, jointed = false, boxed = false;
    bool quiet = false;  // a resting owned body nothing touched: not read, not reclassified (see Quiet)
    int8_t same = -1;  // 1/0: matches the cached entry's colliders; -1: not known yet
};
std::vector<ScanItem> g_scan;

// A big pile is mostly owned bodies at rest, and each tick's scan would read and classify every one of them again.
// One is quiet when PhysX has it asleep and the game hasn't woken it (owned bodies are kept asleep natively; holes,
// impulses, scripts and grabs wake what they touch), its static surroundings are complete, and it still sits where
// the body list had it last tick. Every tick a rotating 1/16 of them gets the full look anyway, for changes that
// wake nothing.
constexpr uint32_t kQuietRecheck = 16;
uint64_t g_quietWin = 0;  // quiet bodies passed over this window (diagnostics)

bool Quiet(uint8_t* b, const BodyEntry& e, size_t i) {
    return e.owned && e.actor && !e.kinematic && !e.physxAwake && !e.bad && !e.pending && !e.dirty &&
           e.qmn[0] <= e.qmx[0] && !e.bounds.isEmpty() && !BodyAwake(b) && BodyDynamic(b) &&
           (i + g_tick) % kQuietRecheck != 0;
}

bool SameSig(const std::vector<ShapeSig>& a, const std::vector<ShapeSig>& b) {
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k)
        if (!Same(a[k], b[k])) return false;
    return true;
}

void ScanRead(uint8_t** bodies, size_t lo, size_t hi, bool allowQuiet = true) {
    for (size_t i = lo; i < hi; ++i) {
        ScanItem& it = g_scan[i];
        uint8_t* b = bodies[i];
        it.same = -1;
        it.quiet = false;
        if (allowQuiet && b && i < g_listCache.size()) {
            const ListSlot& slot = g_listCache[i];
            if (slot.body == b && slot.entry && Quiet(b, *slot.entry, i)) {
                it.valid = it.quiet = true;
                continue;
            }
        }
        it.valid = b && td::EntityType(b) == 1 && Handle(b) > 0 && BodyDynamic(b) &&
                   Signature(b, it.sig, it.voxels, it.jointed);
        if (!it.valid) continue;
        it.boxed = BodyAabb(b, it.mn, it.mx);
        if (i < g_listCache.size()) {
            const ListSlot& slot = g_listCache[i];
            if (slot.body == b && slot.entry && slot.entry->actor) it.same = SameSig(slot.entry->sig, it.sig) ? 1 : 0;
        }
    }
}

void ScanOffload() {
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    const uint32_t maxVox = uint32_t(std::max(1, g_config.pxMaxBodyVoxels));
    if (bodies && n > 0) {
        uint64_t r0 = __rdtsc();
        if (g_scan.size() < size_t(n)) g_scan.resize(size_t(n) + 1024);
        px::ParallelFor(size_t(n), 128, [bodies](size_t lo, size_t hi) { ScanRead(bodies, lo, hi); });
        g_phase[kPhSignature] += __rdtsc() - r0;
    }
    g_proxyJointed = g_proxyLarge = 0;
    // What the player holds and what vehicles are made of stays with the game, and so does debris close to the
    // held body or a moving vehicle: there the game's two-way contacts with finite masses matter (a PhysX proxy
    // pushes with infinite mass and launched debris at hundreds of m/s when a held piece was swung around).
    // A zone is the body's shapes, each box grown by the margin: a vehicle's whole box (a crane's spans tens of
    // metres) took in every piece under it, 1,890 at once when your crane fell.
    struct Zone {
        Box3 whole;                 // all parts together, for a quick rejection
        size_t first = 0, count = 0;  // its part boxes in zoneParts
        uint8_t* body = nullptr;    // diagnostics: what it is around, and how many bodies it kept this scan
        bool vehicle = false;
        float speed = 0.0f;
        uint32_t kept = 0;
    };
    static std::vector<Zone> zones;
    static std::vector<Box3> zoneParts, water;
    zones.clear();
    zoneParts.clear();
    water.clear();
    auto addZone = [](uint8_t* b, float grow, bool vehicle, float speed) {
        Zone zn;
        zn.whole = {{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        zn.first = zoneParts.size();
        int guard = 0;
        for (uint8_t* s = FirstShape(b); s && guard < 4096; s = NextShape(s), ++guard) {
            if (td::EntityType(s) != 2 || !ShapePhysical(s)) continue;
            const float* a = td::ShapeAabbMin(s);
            const float* c = td::ShapeAabbMax(s);
            Box3 part;
            bool sane = true;
            for (int k = 0; k < 3 && sane; ++k) {
                sane = std::isfinite(a[k]) && std::isfinite(c[k]) && a[k] <= c[k];
                part.mn[k] = a[k] - grow;
                part.mx[k] = c[k] + grow;
            }
            if (!sane) continue;
            for (int k = 0; k < 3; ++k) {
                zn.whole.mn[k] = std::min(zn.whole.mn[k], part.mn[k]);
                zn.whole.mx[k] = std::max(zn.whole.mx[k], part.mx[k]);
            }
            zoneParts.push_back(part);
        }
        zn.count = zoneParts.size() - zn.first;
        if (!zn.count) return;
        zn.body = b;
        zn.vehicle = vehicle;
        zn.speed = speed;
        zones.push_back(zn);
    };
    uint8_t* held = nullptr;
    if (uint8_t* p = td::LocalPlayer()) held = td::At<uint8_t*>(p, 0x788);
    if (held && td::EntityType(held) != 1) held = nullptr;
    // margins grow with speed up to 2 m (a held piece swung around can move hundreds of m/s for a tick)
    auto margin = [](float base, float ticks, float speed) { return std::min(2.0f, base + ticks * kDt * speed); };
    if (held) addZone(held, margin(0.5f, 3.0f, Speed(BodyVel(held))), false, Speed(BodyVel(held)));
    static std::unordered_set<uint8_t*> vehicles;
    vehicles.clear();
    int nv = 0;
    uint8_t** veh = td::SceneList<uint8_t*>(0x260, nv);
    for (int i = 0; veh && i < nv; ++i)
        if (veh[i] && td::EntityType(veh[i]) == 7)
            if (uint8_t* vb = td::At<uint8_t*>(veh[i], 0x40)) {
                vehicles.insert(vb);
                float sp = Speed(BodyVel(vb));
                if (sp > 2.0f) addZone(vb, margin(1.0f, 4.0f, sp), true, sp);
            }
    // Water: buoyancy and drag are the game's (and seem to skip sleeping bodies). Water volumes keep their world
    // bounds at +0xf0 (min) / +0xfc (max); the exact test is the one Lua IsPointInWater uses.
    int nw = 0;
    uint8_t** waters = td::SceneList<uint8_t*>(0x250, nw);
    for (int i = 0; waters && i < nw; ++i)
        if (uint8_t* w = waters[i]) {
            Box3 wb;
            for (int k = 0; k < 3; ++k) {
                wb.mn[k] = td::At<float>(w, 0xf0 + 4 * k);
                wb.mx[k] = td::At<float>(w, 0xfc + 4 * k);
            }
            if (wb.mn[0] <= wb.mx[0] && wb.mn[1] <= wb.mx[1] && wb.mn[2] <= wb.mx[2]) water.push_back(wb);
        }
    uint8_t* scene = td::SceneObj();
    auto inWater = [&](const float mn[3], const float mx[3]) {
        bool nearWater = false;  // ("near" is a macro in the Windows headers)
        for (const Box3& wb : water) nearWater |= Overlap(wb, mn, mx);
        if (!nearWater) return false;
        float p[3] = {(mn[0] + mx[0]) * 0.5f, mn[1], (mn[2] + mx[2]) * 0.5f};  // lowest point of its bounds
        float depth = 0.0f;
        return td::Fn<PointInWaterFn>(Addr::ScenePointInWater)(scene, p, &depth, nullptr);
    };
    for (int i = 0; bodies && i < n; ++i) {
        ScanItem& item = g_scan[size_t(i)];
        if (!item.valid) {
            // A tracked body that stopped qualifying (no longer dynamic, or no shapes left) leaves PhysX now. One the
            // game made static kept PhysX's motion: its velocity, which the per-body pass zeroes only on sleeping
            // dynamic bodies, came back from every native step as an "impulse" and doubled (debris flung at 100+ m/s
            // on Acacia Apartments).
            if (size_t(i) < g_listCache.size() && g_listCache[size_t(i)].body == bodies[i] &&
                g_listCache[size_t(i)].entry && g_listCache[size_t(i)].entry->actor) {
                static int logged = 0;
                if (g_listCache[size_t(i)].entry->owned && logged++ < 20)
                    LOG("offload diag: tick %u, owned body %d stopped qualifying (dynamic %d, handle %d): released",
                        g_tick, Handle(bodies[i]), int(BodyDynamic(bodies[i])), Handle(bodies[i]));
                auto it = g_bodies.find(bodies[i]);
                if (it != g_bodies.end()) EraseBody(it);
                if (bodies[i] && td::EntityType(bodies[i]) == 1 && !BodyDynamic(bodies[i])) IndexStaticBody(bodies[i]);
            }
            continue;
        }
        uint8_t* b = bodies[i];
        if (item.quiet) {
            BodyEntry& qe = *g_listCache[size_t(i)].entry;
            bool inZone = false;
            for (const Zone& zn : zones)
                if (Overlap(zn.whole, &qe.bounds.minimum.x, &qe.bounds.maximum.x)) inZone = true;
            if (!inZone) {
                qe.seen = g_tick;
                ++g_quietWin;
                continue;
            }
            ScanRead(bodies, size_t(i), size_t(i) + 1, false);  // near the held body or a vehicle: the full look
            if (!item.valid) continue;
        }
        uint64_t c1 = __rdtsc();
        const std::vector<ShapeSig>& sig = item.sig;
        const uint32_t voxels = item.voxels;
        const bool jointed = item.jointed, boxed = item.boxed;
        const float* mn = item.mn;
        const float* mx = item.mx;
        int keep = -1;
        if (jointed || voxels > maxVox || !(BodyInvMass(b) > 0.0f)) {
            keep = kKeepNotFree;
        } else if (b == held) {
            keep = kKeepHeld;
        } else if (!vehicles.empty() && vehicles.count(b)) {
            keep = kKeepVehicle;
        } else if (boxed && mx[1] < g_worldMinY - 20.0f) {
            keep = kKeepOutside;  // fell off the world: nothing in PhysX to land on
        } else if (boxed) {
            for (Zone& zn : zones) {
                if (!Overlap(zn.whole, mn, mx)) continue;
                for (size_t k = zn.first; k < zn.first + zn.count; ++k)
                    if (Overlap(zoneParts[k], mn, mx)) {
                        keep = kKeepZone;
                        ++zn.kept;
                        break;
                    }
            }
            if (keep < 0 && g_waterOk && !water.empty() && inWater(mn, mx)) keep = kKeepWater;
        }
        BodyEntry& e = EntryAt(i, b);
        const uint8_t dirty = e.dirty;  // (this is its full look)
        e.dirty = 0;
        if (e.bad) keep = kKeepWatchdog;
        e.seen = g_tick;
        if (keep == kKeepZone || keep == kKeepWater || keep == kKeepOutside)  // native while there, and a bit longer
            e.holdUntil = std::max(e.holdUntil, g_tick + uint32_t(std::max(0, g_config.pxRegainTicks)));
        e.inWater = keep == kKeepWater;
        bool wantOwn = keep < 0 && g_tick >= e.holdUntil;
        uint64_t c2 = __rdtsc();
        g_phase[kPhClassify] += c2 - c1;
        // owned bodies get their static surroundings whatever the build budget says
        bool ready = wantOwn && boxed && StaticsAround(e, mn, mx, BodyVel(b), e.owned);
        bool own = wantOwn && (e.owned || ready);
        uint64_t c3 = __rdtsc();
        g_phase[kPhStatics] += c3 - c2;
        if (e.owned && !own) HandBack(b, e, keep < 0 ? kKeepConstraint : keep);
        g_proxyJointed += !own && jointed;
        g_proxyLarge += !own && !jointed && voxels > maxVox;
        if (!own && wantOwn && !e.actor) {  // new debris waiting for its static surroundings: native for now
            ++g_off.waiting;
            continue;
        }
        // New debris the game has asleep can wait for a tick with build time to spare: a level's start builds every
        // dynamic body (11,000 on Voxel Plaza, ~23 ms a tick for two seconds with split hulls). It stays the game's,
        // asleep, meanwhile. Moving debris and proxies are built at once.
        if (own && !e.actor && !BodyAwake(b) && g_tickBodyBuildMs >= g_config.pxBodyBuildMsPerTick) {
            ++g_buildsDeferred;
            continue;
        }
        bool kinematic = !own;
        bool same = e.actor && (item.same >= 0 ? item.same == 1 : SameSig(e.sig, sig));
        // large proxies (carved all the time when the laser works on them) are rebuilt at most twice a second
        if (!same && kinematic && e.actor && e.kinematic && voxels > maxVox && g_tick - e.builtTick < 30 && !(dirty & 2))
            same = true;
        if (!same || (e.kinematic && !kinematic))
            BuildBody(e, b, sig, kinematic);  // proxy colliders are finer than debris ones: rebuild to own
        else if (!e.kinematic && kinematic)
            MakeKinematic(e, b);
        uint64_t c4 = __rdtsc();
        g_phase[kPhBuild] += c4 - c3;
        if (!e.actor) continue;
        if (!own) {  // proxies: Sync moves them from this pre-step pose to the post-step one
            e.pre = BodyTr(b);
            e.awake = BodyAwake(b);
            continue;
        }
        if (!e.owned) {
            TakeOver(b, e);
        } else if (!g_async) {  // async: done around the write-back at the tick's start
            CollectForward(b, e);
            ApplyForward(e);
        }
        g_phase[kPhForward] += __rdtsc() - c4;
    }
    for (const Zone& zn : zones)
        if (zn.kept >= 100 && g_diagZones++ < 40) {
            const Box3& zb = zn.whole;
            LOG("offload diag: tick %u: %u bodies kept native near %s %d (%zu parts in a %.1f x %.1f x %.1f m box at "
                "(%.0f, %.0f, %.0f), %.1f m/s)",
                g_tick, zn.kept, zn.vehicle ? "vehicle body" : "held body", Handle(zn.body), zn.count,
                zb.mx[0] - zb.mn[0], zb.mx[1] - zb.mn[1], zb.mx[2] - zb.mn[2], (zb.mn[0] + zb.mx[0]) * 0.5f,
                (zb.mn[1] + zb.mx[1]) * 0.5f, (zb.mn[2] + zb.mx[2]) * 0.5f, zn.speed);
        }
    // bodies gone from the list without their destructor (rare: the destructor hook forgets the rest at once);
    // every body still listed was seen this tick, quiet ones included
    PhaseScope ps(kPhCleanup);
    // A body that lost a shape and wasn't seen by this scan (it lost its last one, or stopped being a dynamic body)
    // gives up its colliders now rather than at the next cleanup: they may name the destroyed shape.
    for (uint8_t* b : g_diedOwners) {
        auto it = g_bodies.find(b);
        if (it != g_bodies.end() && it->second.seen != g_tick) EraseBody(it);
    }
    g_diedOwners.clear();
    if (g_tick % 8 == 0)
        for (auto it = g_bodies.begin(); it != g_bodies.end();) {
            if (it->second.seen != g_tick) {
                it = EraseBody(it);
            } else {
                ++it;
            }
        }
}

// Owned bodies as AfterNativeStep found them (after the step's destroyed bodies were forgotten), for the rest of
// the post-step work: a flat list instead of the map.
std::vector<std::pair<uint8_t*, BodyEntry*>> g_ownedNow;

// g_ownedNow, rebuilt only when g_owned changed: walking the map's 13,000 nodes on Acacia took ~0.6 ms, and three
// passes a tick did it.
std::vector<std::pair<uint8_t*, BodyEntry*>>& OwnedNow() {
    static uint64_t at = UINT64_MAX;
    if (at != g_ownedVersion) {
        g_ownedNow.assign(g_owned.begin(), g_owned.end());
        at = g_ownedVersion;
    }
    return g_ownedNow;
}

void MarkPending(uint8_t* b, BodyEntry& e) {
    if (e.pending) return;
    e.pending = true;
    g_pendBodies.push_back(b);
}

// After the native step, before PhysX's. `defer`: a tick without PhysX (CatchUp), whose step still runs: nothing
// may touch PhysX, so hand-backs and impulses wait for ApplyPending.
void AfterNativeStep(bool defer) {
    std::vector<uint8_t*> dead, woken;
    AcquireSRWLockExclusive(&g_stepLock);
    dead.swap(g_stepDead);
    woken.swap(g_stepWoken);
    ReleaseSRWLockExclusive(&g_stepLock);
    for (uint8_t* b : dead) {  // destroyed while the step ran: forget them before touching anything else
        ForgetStatic(b);
        auto it = g_bodies.find(b);
        if (it == g_bodies.end()) continue;
        EraseBody(it);
    }
    // A native constraint took these during the step (HkNarrowWake let it wake them): the game has them now.
    const uint32_t hold = uint32_t(std::max(0, g_config.pxRegainTicks));
    for (uint8_t* b : woken) {
        auto it = g_bodies.find(b);
        if (it == g_bodies.end()) continue;
        BodyEntry& e = it->second;
        e.holdUntil = std::max(e.holdUntil, g_tick + hold);
        if (!e.owned) continue;
        e.owned = false;
        g_ownedVersion += g_owned.erase(b);
        if (e.actor && !e.kinematic) {
            if (defer)
                g_pendKinematic.push_back(b);
            else
                MakeKinematic(e, b);
        }
        ++g_off.handBacks[kKeepConstraint];
    }
    // Impulses native code gave owned bodies during the step: the per-body pass zeroed their velocities at its
    // start, so whatever is there now was added since. Then the bodies get PhysX's velocities back, which scripts
    // read for the rest of the frame (and the next CollectForward compares with). The body reads and writes run on
    // several threads; PhysX calls and the tree update for the few bodies concerned stay on this one.
    auto& owned = OwnedNow();
    static std::vector<uint32_t> flagged;  // bodies the step gave an impulse or woke
    static std::atomic<uint32_t> nflagged;
    flagged.resize(owned.size());
    nflagged.store(0, std::memory_order_relaxed);
    auto restore = [](size_t lo, size_t hi) {
        for (size_t k = lo; k < hi; ++k) {
            uint8_t* b = g_ownedNow[k].first;
            BodyEntry& e = *g_ownedNow[k].second;
            float* v = BodyVel(b);
            float* w = BodyAngVel(b);
            // (only a dynamic body's velocity was zeroed by the per-body pass: a static one's is still PhysX's)
            e.stepImpulse = BodyDynamic(b) && (v[0] != 0.0f || v[1] != 0.0f || v[2] != 0.0f || w[0] != 0.0f ||
                                               w[1] != 0.0f || w[2] != 0.0f);
            if (e.stepImpulse)
                for (int i = 0; i < 3; ++i) {
                    e.fwdDv[i] = v[i];
                    e.fwdDw[i] = w[i];
                }
            std::memcpy(v, e.expV, sizeof(e.expV));
            std::memcpy(w, e.expW, sizeof(e.expW));
            e.stepWoken = BodyAwake(b);  // something woke it during the step
            if (e.stepWoken) SleepCounter(b) = 0;
            if (e.stepImpulse || e.stepWoken) flagged[nflagged.fetch_add(1, std::memory_order_relaxed)] = uint32_t(k);
        }
    };
    if (defer)
        restore(0, owned.size());  // PhysX's threads are busy with its step
    else
        px::ParallelFor(owned.size(), 256, restore);
    static int diagImpulses = 0;
    for (uint32_t fi = 0, nf = nflagged.load(); fi < nf; ++fi) {
        auto& [b, e] = owned[flagged[fi]];
        if (!defer && (e->stepImpulse || e->stepWoken)) {
            Thaw(*e);
            if (e->stepImpulse) ThawAround(e->bounds, V(e->fwdDv).magnitude(), kThawImpulse);
        }
        // Diagnostics: escalating "impulses" (debris doubled its speed each step on Acacia Apartments). The fields
        // were zeroed by the per-body pass if the body slept natively, so a step impulse equal to PhysX's own
        // velocity means it didn't.
        if (e->stepImpulse && V(e->fwdDv).magnitude() > 10.0f && diagImpulses < 60) {
            ++diagImpulses;
            LOG("offload diag: tick %u, body %d left the native step with %.1f m/s (PhysX had %.1f m/s, expected "
                "(%.1f, %.1f, %.1f), got (%.1f, %.1f, %.1f)); solver index %d, %s after the step, %u voxels, owned %u "
                "ticks%s",
                g_tick, Handle(b), V(e->fwdDv).magnitude(), V(e->expV).magnitude(), e->expV[0], e->expV[1],
                e->expV[2], e->fwdDv[0], e->fwdDv[1], e->fwdDv[2], td::At<int>(b, 0x13c),
                e->stepWoken ? "awake" : "asleep", e->voxels, g_tick - e->ownedSince,
                defer ? ", a tick without PhysX" : "");
        }
        if (e->stepImpulse) {
            if (defer) {
                for (int i = 0; i < 3; ++i) {
                    e->pendDv[i] += e->fwdDv[i];
                    e->pendDw[i] += e->fwdDw[i];
                }
                MarkPending(b, *e);
            } else {
                e->actor->setLinearVelocity(e->actor->getLinearVelocity() + V(e->fwdDv));
                e->actor->setAngularVelocity(e->actor->getAngularVelocity() + V(e->fwdDw));
            }
            ++g_off.stepDv;
        }
        if (e->stepWoken) {
            UpdateShapes(b);  // back into the sleeping-body trees
            // diagnostics: one the settle check just put to sleep (a whole pile woke again at once in Volkograd)
            static int logged = 0;
            if (e->sleptTick != 0 && e->sleptTick == g_sleptAt && logged < 20) {
                ++logged;
                const float* c = BodyCom(b);
                LOG("offload settle diag: tick %u: the game's step woke body %d (%u voxels at (%.1f, %.1f, %.1f)) "
                    "%u ticks after the settle check put it to sleep%s",
                    g_tick, Handle(b), e->voxels, c[0], c[1], c[2], g_tick - e->sleptTick,
                    e->stepImpulse ? ", with an impulse" : "");
            }
            ++g_gameWokeWin;
            if (defer) {
                e->pendWake = true;
                MarkPending(b, *e);
            } else if (e->actor->isSleeping()) {
                e->actor->wakeUp();
            }
        }
    }
}

// A tick without PhysX (CatchUp): what scripts and tools changed on owned bodies since the last write-back is kept
// for PhysX's next turn. Velocities above all: the native step zeroes them on these natively sleeping bodies. A pose
// change needs nothing: it stays different from the expectation and is forwarded at the next write-back.
void CollectWhileStepping() {
    const auto& owned = OwnedNow();
    for (size_t k = 0; k < owned.size(); ++k) {
        auto [b, e] = owned[k];
        // resting and untouched (as at the tick's start, the rest every 16th tick): nothing to keep
        if (!e->physxAwake && !e->pending && !BodyAwake(b) && (k + g_tick) % kQuietRecheck != 0) continue;
        CollectForwardPure(b, *e);  // on this thread: PhysX's threads are busy with its step
        if (e->fwdPushed) {
            ++g_off.velDiffs;
            for (int i = 0; i < 3; ++i) {
                e->pendDv[i] += e->fwdDv[i];
                e->pendDw[i] += e->fwdDw[i];
            }
            MarkPending(b, *e);
        }
        if (e->fwdWoken) {
            ++g_off.nativeWakes;
            SleepNative(b);
            e->pendWake = true;
            MarkPending(b, *e);
        }
        e->fwdMoved = e->fwdPushed = e->fwdWoken = false;
    }
}

// PhysX's next turn, after the write-back: the changes kept while ticks passed without it.
void ApplyPending() {
    for (uint8_t* b : g_pendKinematic) {
        auto it = g_bodies.find(b);
        if (it != g_bodies.end() && it->second.actor && !it->second.owned && !it->second.kinematic)
            MakeKinematic(it->second, b);
    }
    g_pendKinematic.clear();
    for (uint8_t* b : g_pendBodies) {
        auto it = g_bodies.find(b);
        if (it == g_bodies.end()) continue;
        BodyEntry& e = it->second;
        if (e.owned && e.actor && !e.kinematic) {
            Thaw(e);
            PxVec3 dv = V(e.pendDv), dw = V(e.pendDw);
            if (!dv.isZero()) e.actor->setLinearVelocity(e.actor->getLinearVelocity() + dv);
            if (!dw.isZero()) e.actor->setAngularVelocity(e.actor->getAngularVelocity() + dw);
            if ((e.pendWake || !dv.isZero() || !dw.isZero()) && e.actor->isSleeping()) e.actor->wakeUp();
        }
        e.pending = e.pendWake = false;
        std::memset(e.pendDv, 0, sizeof(e.pendDv));
        std::memset(e.pendDw, 0, sizeof(e.pendDw));
    }
    g_pendBodies.clear();
}

// Sleeping PhysX debris next to geometry that went away or changed this tick (g_changed) is woken, so it falls
// when what held it up is gone. A union box rejects most bodies with one test.
void WakeAroundChanges() {
    if (g_changed.empty()) return;
    PxBounds3 all = PxBounds3::empty();
    for (PxBounds3& c : g_changed) {
        c.fattenSafe(0.1f);
        all.include(c);
    }
    // Bounds as the last write-back left them (a body that fell asleep moved by less than the margin since). Only
    // the bodies near a change ask PhysX whether they sleep: physxAwake can be stale, since a body isn't among
    // PhysX's active actors in the step it falls asleep.
    for (auto& [b, e] : g_ownedNow) {
        PxRigidDynamic* a = e->actor;
        if (!a || !all.intersects(e->bounds)) continue;
        for (const PxBounds3& c : g_changed)
            if (c.intersects(e->bounds)) {
                if (e->frozen) {
                    Thaw(*e);
                    ++g_off.supportWakes;
                } else if (a->isSleeping()) {
                    a->wakeUp();
                    ++g_off.supportWakes;
                }
                break;
            }
    }
}

// Before the step: frozen pieces thaw where pieces PhysX moved (collected at the write-back), proxies the game
// moved this tick and the player will be (frozen pieces are kinematic: kinematic proxies and the player's capsule
// would pass through them). Then the queued settled pieces freeze, where nothing moved.
void ThawAroundMovers() {
    if (!Freezing()) return;
    for (uint8_t* pb : g_proxiesMovedTick) {
        auto it = g_bodies.find(pb);
        if (it == g_bodies.end()) continue;
        // Whatever its speed, but not while it jiggles in place (native jointed bodies do: their surroundings
        // would never freeze): ThawDistance since its last thaw. (A jiggle can reach that far into a frozen piece.)
        float bmn[3], bmx[3];
        if (!BodyAabb(pb, bmn, bmx)) continue;
        const PxBounds3 now(V(bmn), V(bmx));
        if (Travelled(it->second.thawFrom, now) <= g_config.pxThawDistance) continue;
        it->second.thawFrom = now;
        if (it->second.sig.size() > 16) {  // a big one (a tower's top resting on the pile): around its shapes
            int guard = 0;
            for (uint8_t* s = FirstShape(pb); s && guard < 4096; s = NextShape(s), ++guard) {
                if (td::EntityType(s) != 2 || !ShapePhysical(s)) continue;
                ThawAround(PxBounds3(V(td::ShapeAabbMin(s)), V(td::ShapeAabbMax(s))), it->second.movedSpeed,
                           kThawProxy);
                ++it->second.thawBoxesWin;
            }
            continue;
        }
        ThawAround(now, it->second.movedSpeed, kThawProxy);
        ++it->second.thawBoxesWin;
    }
    ThawIn(g_thawBoxes);
    g_thawBoxes.clear();
    FreezeQueued();
}

// The player as a kinematic capsule from 10 cm above the feet (player+0x570) to the top of the head: debris the
// player stands on is a static obstacle for the game's player controller, and the capsule shouldn't grind it.
// Large jumps (respawn, teleport) move the capsule without sweeping through the debris in between.
void UpdatePlayer() {
    if (!g_config.pxPlayer) return;
    uint8_t* p = td::LocalPlayer();
    if (!p) return;
    const float* feet = &td::At<float>(p, 0x570);
    constexpr float kLift = 0.1f;
    float h = std::max(0.3f, g_config.pxPlayerHeight) - kLift;
    float r = std::clamp(g_config.pxPlayerRadius, 0.05f, h * 0.5f);
    PxTransform pose(PxVec3(feet[0], feet[1] + kLift + h * 0.5f, feet[2]));
    if (!pose.isValid()) return;
    static PxVec3 lastThaw(1e30f);
    if (Freezing() && (pose.p - lastThaw).magnitude() > 0.02f) {  // kinematic too: frozen pieces near it thaw
        lastThaw = pose.p;
        g_thawBoxes.push_back(PxBounds3::centerExtents(pose.p, PxVec3(r + 0.3f, h * 0.5f + 0.3f, r + 0.3f)));
        ++g_thawBoxesBy[kThawPlayer];
    }
    if (!g_player) {
        g_player = px::Physics()->createRigidDynamic(pose);
        if (!g_player) return;
        g_player->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        PxShape* s = PxRigidActorExt::createExclusiveShape(*g_player, PxCapsuleGeometry(r, std::max(0.01f, h * 0.5f - r)),
                                                           *px::Material(), PxShapeFlag::eSIMULATION_SHAPE);
        if (s) s->setLocalPose(PxTransform(PxQuat(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f))));  // PhysX capsules lie along x
        px::Scene()->addActor(*g_player);
        return;
    }
    const float moved = (g_player->getGlobalPose().p - pose.p).magnitude();
    if (moved > 0.001f) g_playerMovedTick = g_tick;
    if (moved > 2.0f)
        g_player->setGlobalPose(pose);
    else if (moved > 0.001f)  // standing still: no target (a kinematic given one wakes what it touches, every step)
        g_player->setKinematicTarget(pose);
}

// PhysX's result into the body: pose (also as the predicted pose, so nothing sweeps), velocities, then the
// shapes' world bounds (the engine's 0x1404ddd00 without its tree update). Runs on several threads at once: it only
// writes this body and its shapes. False (nothing written) when the watchdog doesn't like the state.
using ShapeBoundsFn = void(__fastcall*)(uint8_t* shape, uint8_t swept, uint8_t updateTree);
using TreeUpdateFn = void(__fastcall*)(uint8_t* physics, uint8_t* shape);
bool WriteBackFields(uint8_t* b, BodyEntry& e, float& speed, bool& rest) {
    PxTransform p = e.actor->getGlobalPose();
    PxVec3 v = e.actor->getLinearVelocity(), w = e.actor->getAngularVelocity();
    speed = v.magnitude();
    rest = false;
    if (!p.isValid() || !v.isFinite() || !w.isFinite() || speed > g_config.pxMaxSpeed || w.magnitude() > 1000.0f ||
        p.p.abs().maxElement() > 1e5f) {
        e.bad = true;  // handed back before the next step, with its last good state
        return false;
    }
    Tr& t = BodyTr(b);
    if (g_config.pxWriteDeadband > 0.0f && !e.bounds.isEmpty()) {
        // creep the game doesn't need to see: it keeps the body where it is, at rest (no trees to update either)
        PxTransform was = ToPx(t);
        PxQuat dq = p.q * was.q.getConjugate();
        float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(dq.w)));
        float radius = 0.5f * e.bounds.getDimensions().magnitude();
        if ((p.p - was.p).magnitude() + angle * radius < g_config.pxWriteDeadband) {
            float* nv = BodyVel(b);
            float* nw = BodyAngVel(b);
            for (int k = 0; k < 3; ++k) nv[k] = nw[k] = 0.0f;
            rest = true;
            return true;
        }
    }
    t.p[0] = p.p.x;
    t.p[1] = p.p.y;
    t.p[2] = p.p.z;
    t.q[0] = p.q.x;
    t.q[1] = p.q.y;
    t.q[2] = p.q.z;
    t.q[3] = p.q.w;
    BodyPredicted(b) = t;
    float* nv = BodyVel(b);
    nv[0] = v.x;
    nv[1] = v.y;
    nv[2] = v.z;
    float* nw = BodyAngVel(b);
    nw[0] = w.x;
    nw[1] = w.y;
    nw[2] = w.z;
    auto bounds = td::Fn<ShapeBoundsFn>(Addr::ShapeUpdateBounds);
    int guard = 0;
    for (uint8_t* s = FirstShape(b); s && guard < 4096; s = NextShape(s), ++guard)
        if (td::EntityType(s) == 2) bounds(s, 0, 0);
    float mn[3], mx[3];
    e.bounds = BodyAabb(b, mn, mx) ? PxBounds3(V(mn), V(mx)) : PxBounds3::empty();
    return true;
}

// Settle check, second half ([PhysX] SettleClusters): awake pieces close together (sharing a cell of a 1 m grid with
// their bounds grown by 5 cm, or overlapping a large piece's grown bounds: whole PhysX islands, and more) sleep
// together when they are quiet: still or slower than FreezeSpeed at the last two checks (a pile creeps at 5-20 cm/s
// for ever), or nearly all of them: up to 1 in 100 may still move, none faster than 1.5 m/s since the last check.
// Those stop where they are. A big pile always has a few wedged pieces that rattle in place and creepers that slip a
// few centimetres now and then: in Volkograd 15,000 pieces stayed awake for minutes for 12-19 of them (an 11 ms PhysX
// step), and in de_vertigo 1,500 for 1-28. A group with a piece held back (it woke again at once the last time) stays
// awake.
struct SettleItem {
    BodyEntry* e;
    PxBounds3 bounds;
    float speed, moved;  // fastest since the last check (m/s); how far its centre got (m)
    bool quiet;          // still or slow at the last two checks
};
std::vector<SettleItem> g_settleItems;

// A piece the settle check puts to sleep (its next write-back tells whether that lasted: sleptTick)
void SettleSleep(BodyEntry& e) {
    e.actor->putToSleep();
    e.physxAwake = false;
    e.sleptTick = g_tick;
}
constexpr uint32_t kSettleLoudShare = 100;  // 1 in this many of a group may still move
constexpr float kSettleLoudSpeed = 1.5f;    // if none of those went faster than this (m/s)
uint64_t g_clusterSleptWin = 0, g_clusterSleptAll = 0, g_settleStoppedWin = 0, g_settleStoppedAll = 0;
uint64_t g_settleRewokeWin = 0, g_settleRetriedWin = 0, g_settleHeldWin = 0, g_settleLargeWin = 0;
uint64_t g_settleLoneWin = 0;
uint32_t g_clustersQuietWin = 0, g_clustersLoudWin = 0, g_largestLoudWin = 0, g_largestLoudMovers = 0;
uint32_t g_largestLoudHeld = 0;
float g_largestLoudSpeed = 0.0f, g_largestLoudMoved = 0.0f;

int SettleRoot(std::vector<int>& parent, int i) {
    while (parent[size_t(i)] != i) {
        parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        i = parent[size_t(i)];
    }
    return i;
}

double g_settleMsWin = 0.0, g_settleMaxMsWin = 0.0;  // the group pass itself
void SettleClustersImpl();

// Diagnostics (a big group was just put to sleep): kinematic actors PhysX has awake now. One touching the group keeps
// its contacts with the sleeping pieces alive for the next step, and a contact found or lost there wakes the whole
// island again (PhysX's lost-touch and active-kinematic wake rules).
void LogAwakeKinematics(const PxBounds3& slept, uint32_t count) {
    static int logged = 0;
    if (logged >= 30) return;
    ++logged;
    PxBounds3 around = slept;  // ("near" is a macro in the Windows headers)
    around.fattenFast(0.5f);
    uint32_t awake = 0, touching = 0;
    char list[1024];
    int len = 0;
    list[0] = 0;
    for (uint8_t* pb : g_proxies) {
        auto it = g_bodies.find(pb);
        if (it == g_bodies.end() || !it->second.actor) continue;
        BodyEntry& e = it->second;
        bool on = !e.actor->isSleeping();
        PxBounds3 wb = e.actor->getNbShapes() ? e.actor->getWorldBounds() : PxBounds3::empty();
        for (BodyEntry::Part& p : e.parts) {
            on |= p.actor && !p.actor->isSleeping();
            if (p.actor && p.actor->getNbShapes()) wb.include(p.actor->getWorldBounds());
        }
        if (!on) continue;
        ++awake;
        const bool touches = !wb.isEmpty() && wb.intersects(around);
        touching += touches;
        if (touches && len < int(sizeof(list)) - 160)
            len += std::snprintf(list + len, sizeof(list) - size_t(len),
                                 "%sbody %d (%u voxels%s%s, moved %u ticks ago, natively %s)", len ? "; " : "",
                                 Handle(pb), e.voxels, e.inWater ? ", in water" : "", e.split ? ", split" : "",
                                 g_tick - e.movedTick, BodyAwake(pb) ? "awake" : "asleep");
    }
    const bool playerAwake = g_player && !g_player->isSleeping();
    LOG("offload settle diag: tick %u: %u pieces put to sleep together; kinematic actors awake: %u proxies (%u by "
        "them: %s), player capsule %s (moved %u ticks ago)",
        g_tick, count, awake, touching, list[0] ? list : "none", playerAwake ? "awake" : "asleep",
        g_tick - g_playerMovedTick);
}
void SettleClusters() {
    const uint64_t t0 = trace::Now();
    SettleClustersImpl();
    const double ms = Ms(t0, trace::Now());
    g_settleMsWin += ms;
    g_settleMaxMsWin = std::max(g_settleMaxMsWin, ms);
}

void SettleClustersImpl() {
    const size_t n = g_settleItems.size();
    if (!n) return;
    static std::vector<int> parent;
    parent.resize(n);
    for (size_t i = 0; i < n; ++i) parent[i] = int(i);
    // each piece's cells (bounds grown by kGrow, on a kSize grid). A piece over 512 cells joins every piece its grown
    // bounds overlap instead (one piece left out can split an island in two groups: see g_holdRelease).
    constexpr float kGrow = 0.05f, kSize = 1.0f;
    struct Span {
        int lo[3], hi[3];
    };
    static std::vector<Span> spans;
    static std::vector<uint32_t> large;
    spans.resize(n);
    large.clear();
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        const PxBounds3& b = g_settleItems[i].bounds;
        Span& s = spans[i];
        s.lo[0] = 1;
        s.hi[0] = 0;  // (empty)
        if (b.isEmpty() || !b.isFinite()) {
            ++g_settleLoneWin;
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            s.lo[k] = int(std::floor(((&b.minimum.x)[k] - kGrow) / kSize));
            s.hi[k] = int(std::floor(((&b.maximum.x)[k] + kGrow) / kSize));
        }
        const int64_t cells = int64_t(s.hi[0] - s.lo[0] + 1) * (s.hi[1] - s.lo[1] + 1) * (s.hi[2] - s.lo[2] + 1);
        if (cells > 512) {
            s.lo[0] = 1;
            s.hi[0] = 0;
            large.push_back(uint32_t(i));
            continue;
        }
        total += size_t(cells);
    }
    // cell -> the first piece seen in it, open addressing at most half full (a std::unordered_map took ~4 ms for
    // 21,000 pieces). Cell keys are never negative: -1 marks a free slot.
    static std::vector<int64_t> keys;
    static std::vector<int> first;
    int bits = 4;
    while ((size_t(1) << bits) < total * 2) ++bits;
    const size_t cap = size_t(1) << bits, mask = cap - 1;
    keys.assign(cap, -1);
    first.resize(cap);
    for (size_t i = 0; i < n; ++i) {
        const Span& s = spans[i];
        for (int z = s.lo[2]; z <= s.hi[2] && s.lo[0] <= s.hi[0]; ++z)
            for (int y = s.lo[1]; y <= s.hi[1]; ++y)
                for (int x = s.lo[0]; x <= s.hi[0]; ++x) {
                    const int64_t key = CellKey(x, y, z);
                    size_t at = size_t((uint64_t(key) * 0x9E3779B97F4A7C15ull) >> (64 - bits));
                    while (keys[at] != -1 && keys[at] != key) at = (at + 1) & mask;
                    if (keys[at] == -1) {
                        keys[at] = key;
                        first[at] = int(i);
                        continue;
                    }
                    const int a = SettleRoot(parent, int(i)), c = SettleRoot(parent, first[at]);
                    if (a != c) parent[size_t(a)] = c;
                }
    }
    // large pieces: against every piece (~40 us each with 20,000; past 32 of them, they are left on their own)
    g_settleLargeWin += large.size();
    for (size_t k = 0; k < large.size() && k < 32; ++k) {
        const size_t i = large[k];
        PxBounds3 a = g_settleItems[i].bounds;
        a.fattenFast(2.0f * kGrow);
        for (size_t j = 0; j < n; ++j) {
            const PxBounds3& b = g_settleItems[j].bounds;
            if (j == i || b.isEmpty() || !a.intersects(b)) continue;
            const int ra = SettleRoot(parent, int(i)), rb = SettleRoot(parent, int(j));
            if (ra != rb) parent[size_t(ra)] = rb;
        }
    }
    // per group (at its root): pieces, those still moving and the fastest of them, those held back
    static std::vector<uint32_t> size, movers, held;
    static std::vector<float> fastest;
    size.assign(n, 0);
    movers.assign(n, 0);
    held.assign(n, 0);
    fastest.assign(n, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        const SettleItem& s = g_settleItems[i];
        const size_t r = size_t(SettleRoot(parent, int(i)));
        ++size[r];
        held[r] += s.e->held && g_tick < g_holdRelease;
        if (!s.quiet) {
            ++movers[r];
            fastest[r] = std::max(fastest[r], s.speed);
        }
    }
    auto sleeps = [](size_t r) {
        return !held[r] && (!movers[r] || (movers[r] * kSettleLoudShare <= size[r] && fastest[r] < kSettleLoudSpeed));
    };
    uint32_t slept = 0;
    PxBounds3 sleptBounds = PxBounds3::empty();
    for (size_t i = 0; i < n; ++i) {
        const size_t r = size_t(SettleRoot(parent, int(i)));
        if (r == i) {  // a group's own tally (diagnostics: the largest that stays awake, and why)
            if (!sleeps(r)) {
                ++g_clustersLoudWin;
                if (size[i] > g_largestLoudWin) {
                    g_largestLoudWin = size[i];
                    g_largestLoudMovers = movers[i];
                    g_largestLoudHeld = held[i];
                }
            } else {
                ++g_clustersQuietWin;
            }
        }
        if (!sleeps(r)) continue;
        BodyEntry& e = *g_settleItems[i].e;
        if (e.actor && !e.actor->isSleeping()) {
            SettleSleep(e);
            e.retries = 0;
            e.held = false;
            ++g_clusterSleptWin;
            ++g_clusterSleptAll;
            ++slept;
            sleptBounds.include(g_settleItems[i].bounds);
            if (!g_settleItems[i].quiet) {
                ++g_settleStoppedWin;
                ++g_settleStoppedAll;
            }
        }
    }
    if (slept >= 1000) LogAwakeKinematics(sleptBounds, slept);
    // the fastest mover of the largest group that stays awake
    g_largestLoudSpeed = g_largestLoudMoved = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const SettleItem& s = g_settleItems[i];
        if (s.quiet || size[size_t(SettleRoot(parent, int(i)))] != g_largestLoudWin) continue;
        if (s.speed > g_largestLoudSpeed) {
            g_largestLoudSpeed = s.speed;
            g_largestLoudMoved = s.moved;
        }
    }
    g_settleItems.clear();
}

// `changed`: owned bodies whose fields something else changed this tick (async: the collected script changes),
// whose expectations must follow too. In sync mode those were woken and are among the actors PhysX moved.
uint32_t WriteBackAll(const std::vector<std::pair<uint8_t*, BodyEntry*>>* changed = nullptr) {
    PxU32 n = 0;
    PxActor** act = px::Scene()->getActiveActors(n);
    g_lastActive = n;
    struct Item {
        uint8_t* b;
        BodyEntry* e;
        float speed;
        bool ok;
        bool rest;  // moved too little to write (the write deadband)
        // settle check (settleNow): how far its centre got since the last check, the fastest it went meanwhile;
        // the step after a check (verify): it was put to sleep there, and PhysX moved it again
        bool still, rewoke;
        float moved, winSpeed;
    };
    static std::vector<Item> items;
    items.clear();
    for (Mover& m : g_movers) m = Mover{};
    const bool settleNow = g_offload && g_tick >= g_settleNext;
    const uint32_t verify = g_sleptAt;
    g_sleptAt = 0;
    for (PxU32 i = 0; i < n; ++i) {
        uint8_t* b = static_cast<uint8_t*>(act[i]->userData);
        if (!b) continue;
        auto it = g_owned.find(b);
        // (a frozen piece's kinematic actor can stay listed for a few frames: nothing to write)
        if (it == g_owned.end() || it->second->actor != act[i] || it->second->frozen) continue;
        items.push_back({b, it->second, 0.0f, false, false, false, false, 0.0f, 0.0f});
    }
    g_ownedMoved = uint32_t(items.size());
    px::ParallelFor(items.size(), 128, [settleNow, verify](size_t lo, size_t hi) {
        for (size_t k = lo; k < hi; ++k) {
            Item& it = items[k];
            it.ok = WriteBackFields(it.b, *it.e, it.speed, it.rest);
            if (!it.ok) continue;
            if (it.rest) {  // most of a big awake pile: per-body work only, so on these threads too
                Expect(it.b, *it.e);
                it.e->physxAwake = !it.e->actor->isSleeping();
            }
            BodyEntry& e = *it.e;
            // Put to sleep at the check (or retry) just before this step, and PhysX moved it again: its island
            // didn't sleep. PhysX itself woke whole piles again at once like this: in Volkograd all 21,600 pieces at
            // every try, on the GPU and on the CPU, with no kinematic actor, contact report, wake from the game or
            // lost contact with the level involved (each ruled out in turn); de_vertigo's 2,612 slept at a retry.
            // So it is put to sleep again at once, once (below), then held back for 2 s, then 4, 8 ... 64 while
            // that goes on (g_holdRelease, moved on below): putting 22,000 pieces to sleep that woke again at once
            // cost a 24 ms check and a 22 ms step, every second, in Volkograd. One that sleeps through that step
            // and wakes later was woken by something new.
            if (verify && e.sleptTick == verify) {
                it.rewoke = true;
                e.sleptTick = 0;
                if (e.retries >= 1) {
                    e.rewakes = uint8_t(std::min(255, e.rewakes + 1));
                    e.held = true;
                }
            }
            // Settle check, once a second for every awake piece at once (the piece's own part; what it decides
            // is applied after this, on the main thread): one that got nowhere since the last check (its centre
            // moved under 2 cm) is still, and one that stayed slower than FreezeSpeed is slow. (Anchored at an
            // earlier check: one it slept through still counts; 0 after a thaw or a change.)
            e.maxSpeedWin = std::max(e.maxSpeedWin, it.speed);
            if (!settleNow) continue;
            const PxTransform pose = e.actor->getGlobalPose();
            const PxVec3 at = pose.transform(e.actor->getCMassLocalPose().p);
            const bool seen = e.anchorTick != 0;
            it.moved = seen ? (at - e.anchor).magnitude() : 0.0f;
            it.still = seen && it.moved < 0.02f;
            const bool slow = seen && e.maxSpeedWin < g_config.pxFreezeSpeed;  // (creep: 5-20 cm/s in a big pile)
            e.stuck = it.still ? uint8_t(std::min(255, e.stuck + 1)) : uint8_t(0);
            e.settled = it.still || slow ? uint8_t(std::min(255, e.settled + 1)) : uint8_t(0);
            if (e.sleptTick != 0) {  // it slept through the step after the check that put it to sleep: that worked
                e.sleptTick = 0;
                e.rewakes = 0;
                e.held = false;
            }
            it.winSpeed = e.maxSpeedWin;
            e.anchor = at;
            e.anchorTick = g_tick;
            e.maxSpeedWin = 0.0f;
        }
    });
    // A piece PhysX moved again in the step after it was put to sleep is put to sleep again at once, once.
    // After that it holds back every piece held: they are let go all at once, at the first check after the last
    // such wake's hold, or after the player stood still, if the player moved then.
    if (verify) {
        bool rewoke = false, retried = false;
        for (const Item& it : items) {
            if (!it.ok || !it.rewoke) continue;
            BodyEntry& e = *it.e;
            ++g_settleRewokeWin;
            if (!e.held) {  // again at once (verified at the next step)
                SettleSleep(e);
                ++e.retries;
                ++g_settleRetriedWin;
                retried = true;
                continue;
            }
            rewoke = true;
            g_holdRelease = std::max(g_holdRelease, g_tick + (120u << std::min(5, e.rewakes - 1)));
        }
        if (retried) g_sleptAt = g_tick;
        if (rewoke) g_holdForPlayer = g_playerMovedTick >= verify;
    }
    if (settleNow && g_holdForPlayer && g_playerMovedTick <= g_settleLast) {
        g_holdRelease = std::min(g_holdRelease, g_tick);
        g_holdForPlayer = false;
    }
    // the AABB trees are shared: one thread
    uint8_t* physics = td::PhysicsObj();
    auto tree = td::Fn<TreeUpdateFn>(Addr::TreeUpdateShape);
    uint32_t written = 0;
    for (const Item& it : items) {
        BodyEntry& e = *it.e;
        if (!it.ok) {
            if (g_diagTrips++ < 40) {
                PxTransform p = e.actor->getGlobalPose();
                PxVec3 com = p.transform(e.actor->getCMassLocalPose().p);
                LOG("offload diag: tick %u, watchdog: body %d at %.1f m/s, %.1f rad/s, centre (%.1f, %.1f, %.1f); %u "
                    "voxels, mass %.3g, %u shapes, owned for %u ticks, last forwarded %.1f m/s",
                    g_tick, Handle(it.b), it.speed, e.actor->getAngularVelocity().magnitude(), com.x, com.y, com.z,
                    e.voxels, e.actor->getMass(), e.actor->getNbShapes(), g_tick - e.ownedSince, e.lastForward);
            }
            continue;
        }
        // Settle check (each piece was looked at above, on the worker threads). PhysX sleeps whole islands only, and
        // a pile is one island: wedged pieces (hulls of concave pieces overlapping by centimetres) rattle in place at
        // up to 1-4 m/s for ever and keep it awake, with everything that rests on them.
        // - By groups (SettleClusters, after this loop): a quiet group sleeps whole, rattlers too.
        // - Piece by piece ([PhysX] SettleClusters=0): a still piece sleeps (slower than 2 m/s, or still for three
        //   checks in a row whatever its speed). In a pile with a few movers they all woke again at once: in
        //   Volkograd 22,000 pieces were put to sleep and woken again every second.
        if (settleNow) {
            // settled at FreezeAfter checks in a row: frozen soon (FreezeQueued)
            ++g_settleLooked;
            const bool held = e.held && g_tick < g_holdRelease;
            g_settleHeldWin += held;
            if (Freezing() && e.settled >= FreezeNeeds(e) && !e.freezeQueued) {
                ++g_settleLong;
                e.freezeQueued = true;
                g_freezeQueue.push_back(it.b);
            }
            if (g_config.pxSettleClusters) {
                g_settleItems.push_back({&e, e.bounds, it.winSpeed, it.moved, e.settled >= 2});
            } else if (!held && it.still && (it.speed < 2.0f || e.stuck >= 3)) {
                SettleSleep(e);
                e.retries = 0;
                e.held = false;
                ++g_wedgedWin;
                ++g_wedgedAll;
            }
        }
        if (it.rest) {  // the game keeps it where it was (its expectations were set with the fields)
            ++g_writeRestWin;
            continue;
        }
        // Diagnostics: debris flung out of the level at ~100 m/s (Acacia Apartments). What sped a body up by more
        // than 30 m/s in one step (the field now holds PhysX's new velocity, the expectation the last one)?
        if (const float* nv = BodyVel(it.b);
            PxVec3(nv[0], nv[1], nv[2]).magnitude() > PxVec3(e.expV[0], e.expV[1], e.expV[2]).magnitude() + 25.0f) {
            ++g_launches;
            if (g_launches <= 40 || g_launches % 200 == 0) {
                PxTransform pose = e.actor->getGlobalPose();
                PxVec3 com = pose.transform(e.actor->getCMassLocalPose().p);
                float nearest = 1e9f;
                int nearHandle = 0;
                const BodyEntry* np = nullptr;
                uint8_t* nb = nullptr;
                for (uint8_t* pb : g_proxies) {
                    float mn[3], mx[3];
                    if (!BodyAabb(pb, mn, mx)) continue;
                    PxVec3 c(std::clamp(com.x, mn[0], mx[0]), std::clamp(com.y, mn[1], mx[1]),
                             std::clamp(com.z, mn[2], mx[2]));
                    auto pit = g_bodies.find(pb);
                    if ((c - com).magnitude() < nearest && pit != g_bodies.end()) {
                        nearest = (c - com).magnitude();
                        nearHandle = Handle(pb);
                        np = &pit->second;
                        nb = pb;
                    }
                }
                char prox[256] = "none";
                if (np)
                    std::snprintf(prox, sizeof(prox),
                                  "%d at %.2f m (%u voxels, %u colliders%s, natively %.1f m/s %s; last moved %u ticks "
                                  "ago at %.1f m/s%s)",
                                  nearHandle, nearest, np->voxels, np->boxes, np->split ? ", split" : "",
                                  Speed(BodyVel(nb)), BodyAwake(nb) ? "awake" : "asleep", g_tick - np->movedTick,
                                  np->movedSpeed, np->movedTeleport ? ", teleported" : "");
                LOG("offload diag: launch %llu, tick %u: body %d (%u voxels, %u colliders, mass %.3g) at (%.1f, %.1f, "
                    "%.1f) went from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f) m/s in a %u-tick step; owned %u ticks, "
                    "colliders built %u ticks ago, last forwarded %.1f m/s, native impulse in the last step %s, woken "
                    "natively %s; nearest proxy %s",
                    g_launches, g_tick, Handle(it.b), e.voxels, e.boxes, e.actor->getMass(), com.x, com.y, com.z,
                    e.expV[0], e.expV[1], e.expV[2], nv[0], nv[1], nv[2], g_stepSpan, g_tick - e.ownedSince,
                    g_tick - e.builtTick, e.lastForward, e.stepImpulse ? "yes" : "no", e.stepWoken ? "yes" : "no",
                    prox);
            }
        }
        int guard = 0;
        for (uint8_t* s = FirstShape(it.b); s && guard < 4096; s = NextShape(s), ++guard)
            if (td::EntityType(s) == 2) tree(physics, s);
        e.treesDirty = false;
        Expect(it.b, e);
        e.physxAwake = !e.actor->isSleeping();
        ++written;
        if (Freezing()) {
            // Real movement thaws (and heats) its surroundings: ThawDistance travelled within the time that takes at
            // ThawSpeed. Jiggling in place and creeping (5-20 cm/s for ever in a big pile) don't count.
            const uint32_t window = uint32_t(
                std::max(1.0f, g_config.pxThawDistance / std::max(0.01f, g_config.pxThawSpeed) / kDt));
            if (!e.thawFrom.isEmpty() && g_tick - e.thawFromTick >= window) {
                e.thawFrom = e.bounds;
                e.thawFromTick = g_tick;
            } else if (Travelled(e.thawFrom, e.bounds) > g_config.pxThawDistance) {
                ThawAround(e.bounds, it.speed, kThawMover);
                e.thawFrom = e.bounds;
                e.thawFromTick = g_tick;
            }
        }
        ++g_speedHist[it.speed < 0.05f ? 0 : it.speed < 0.2f ? 1 : it.speed < 1.0f ? 2 : 3];
        if (it.speed > g_movers[2].speed) {  // keep the three fastest, fastest first
            g_movers[2] = {it.b, it.speed};
            if (g_movers[2].speed > g_movers[1].speed) std::swap(g_movers[2], g_movers[1]);
            if (g_movers[1].speed > g_movers[0].speed) std::swap(g_movers[1], g_movers[0]);
        }
        if (it.speed > g_maxSpeed) {
            g_maxSpeed = it.speed;
            g_maxSpeedHandle = Handle(it.b);
            g_maxSpeedMass = e.actor->getMass();
        }
    }
    // Bodies PhysX didn't move keep their expectations, unless something else changed them (the rest of the owned
    // bodies: thousands of cache misses saved per tick).
    if (changed)
        for (auto& [b, e] : *changed)
            if (e->fwdMoved || e->fwdPushed || e->fwdWoken) {
                Expect(b, *e);
                e->physxAwake = !e->actor->isSleeping();
            }
    if (settleNow) {
        SettleClusters();
        g_settleLast = g_sleptAt = g_tick;
        g_settleNext = g_tick + 60;
    }
    g_off.written += written;
    return written;
}

// ---- Phase 1 sync/compare ----------------------------------------------------------------------------------

// A proxy follows its native body; a still one stays where it is.
void SyncProxy(uint8_t* b, BodyEntry& e, float stepDt) {
    const Tr& now = BodyTr(b);
    if (std::memcmp(&now, &e.placed, sizeof(Tr)) == 0) return;
    if (g_offload && g_config.pxProxyDeadband > 0.0f) {
        // a jiggle (measured from where PhysX has it) leaves the proxy where it is
        PxTransform a = ToPx(e.placed), c = ToPx(now);
        PxVec3 com = V(BodyCom(b));
        PxQuat dq = c.q * a.q.getConjugate();
        float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(dq.w)));
        if ((c.transform(com) - a.transform(com)).magnitude() + angle * e.radius < g_config.pxProxyDeadband) {
            ++g_proxyHeldWin;
            return;
        }
    }
    // a step that covers skipped ticks (CatchUp) sweeps from where the proxy was last placed
    const Tr& start = g_stepSpan > 1 ? e.placed : e.pre;
    PxTransform target = ToPx(now), from = ToPx(start);
    auto each = [&e](auto&& f) {  // the actor, and a split proxy's parts with it
        f(e.actor);
        for (BodyEntry::Part& p : e.parts) f(p.actor);
    };
    PxVec3 com = V(BodyCom(b));  // the origin can be far from the voxels; compare where they are
    // A body that jiggles in place (within 3 cm of where it last went somewhere from, at its fastest point), or
    // bobs in water, is teleported there without waking anything. Given targets, its proxy woke what it touched,
    // and with that its whole island, 10-30 times a second: in Volkograd pieces fallen into the water by the pile
    // and a 4,000-voxel one lying on it kept 22,000 pieces awake for good. (In the game's step the pile stays a
    // static obstacle for it.) A body that goes somewhere pushes and wakes as before.
    if (g_offload) {
        const PxQuat jq = target.q * e.jiggleFrom.q.getConjugate();
        const float jangle = 2.0f * std::acos(std::min(1.0f, std::fabs(jq.w)));
        if (e.inWater || (e.jiggleSet && (target.transform(com) - e.jiggleFrom.transform(com)).magnitude() +
                                                 jangle * e.radius < 0.03f)) {
            each([&](PxRigidDynamic* a) { a->setGlobalPose(target, false); });
            e.placed = now;
            ++g_proxyJiggleWin;
            g_proxiesMovedTick.push_back(b);
            return;
        }
        e.jiggleFrom = target;
        e.jiggleSet = true;
    }
    if (std::memcmp(&start, &e.placed, sizeof(Tr)) != 0) each([&](PxRigidDynamic* a) { a->setGlobalPose(from); });
    float moved = (target.transform(com) - from.transform(com)).magnitude();
    // fastest point: centre of mass plus rotation times the body's half extent
    PxQuat dq = target.q * from.q.getConjugate();
    float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(dq.w)));
    float pointSpeed = (moved + angle * e.radius) / stepDt;
    // Teleported, or too fast to push debris sensibly with infinite mass: move without sweeping (debris it lands in
    // is pushed out at the depenetration limit instead of being launched).
    e.movedSpeed = pointSpeed;
    e.movedTick = g_tick;
    ++e.movesWin;
    e.travelWin += moved;
    e.movedTeleport = moved > 5.0f || (g_offload && pointSpeed > g_config.pxProxyMaxSpeed);
    if (e.movedTeleport) {
        each([&](PxRigidDynamic* a) { a->setGlobalPose(target); });
        g_off.proxyTeleports += g_offload;
    } else {
        each([&](PxRigidDynamic* a) { a->setKinematicTarget(target); });
    }
    if (g_offload && pointSpeed > 40.0f && g_diagProxies++ < 30)
        LOG("offload diag: tick %u, proxy body %d moves %.1f m/s at its fastest point (%u voxels, %u boxes)", g_tick,
            Handle(b), pointSpeed, e.voxels, e.boxes);
    e.placed = now;
    ++g_proxyMovesWin;
    g_proxiesMovedTick.push_back(b);
}

void Sync() {
    const bool sync = g_config.pxShadow == 1;
    const float stepDt = kDt * float(g_stepSpan);
    g_proxiesMovedTick.clear();
    SyncMoving();
    if (g_offload) {  // owned bodies are PhysX's own: only the proxies (a few hundred of thousands) need a look
        for (uint8_t* b : g_proxies) {
            auto it = g_bodies.find(b);
            if (it != g_bodies.end() && it->second.actor && it->second.kinematic) SyncProxy(b, it->second, stepDt);
        }
        return;
    }
    for (auto& [b, e] : g_bodies) {
        if (!e.actor) continue;
        if (e.kinematic) {
            SyncProxy(b, e, stepDt);
            continue;
        }
        if (!sync && !e.fresh) continue;
        e.fresh = false;
        if (!e.awake && e.actor->isSleeping()) continue;  // native and PhysX both at rest
        e.actor->setGlobalPose(ToPx(e.pre), false);
        e.actor->setLinearVelocity(V(e.v), false);
        e.actor->setAngularVelocity(V(e.w), false);
        if (e.awake)
            e.actor->wakeUp();
        else
            e.actor->putToSleep();
    }
}

void Compare() {
    for (auto& [b, e] : g_bodies) {
        if (!e.actor || e.kinematic) continue;
        if (!e.awake) {
            if (!e.actor->isSleeping()) ++g_wokenByPhysx;
            continue;
        }
        // Positions at the centre of mass: debris keeps its parent's frame, so a body origin can sit tens of metres
        // from the voxels and swing by metres for a fraction of a degree of rotation.
        PxTransform pp = e.actor->getGlobalPose();
        PxTransform npost = ToPx(BodyTr(b));
        PxVec3 com = V(BodyCom(b));
        PxVec3 pxCom = pp.transform(e.actor->getCMassLocalPose().p);
        PxVec3 np = npost.transform(com);
        PxVec3 preCom = ToPx(e.pre).transform(com);
        PxQuat nq = npost.q;
        double dp = (pxCom - np).magnitude();
        double dot = std::min(1.0, std::fabs(double(pp.q.dot(nq))));
        double dq = 2.0 * std::acos(dot) * 57.29578;
        PxVec3 nv = V(BodyVel(b));
        double dv = (e.actor->getLinearVelocity() - nv).magnitude();
        double speed = nv.magnitude();
        double step = (np - preCom).magnitude();
        int cls = speed < 0.05 ? 0 : BodyTouched(b) ? 1 : 2;  // resting / moving in contact / free flight
        g_div[cls].Add(dp, dq, dv, step);
        g_divAll[cls].Add(dp, dq, dv, step);
        if (int(g_tick) >= g_config.pxDumpFrom && int(g_tick) <= g_config.pxDumpTo) {
            if (!g_dump) {
                g_dump = std::fopen((g_config.runDir + "\\shadow_bodies.csv").c_str(), "w");
                if (g_dump)
                    std::fputs("tick,handle,voxels,boxes,cls,pre_x,pre_y,pre_z,post_x,post_y,post_z,px_x,px_y,px_z,"
                               "pre_vx,pre_vy,pre_vz,post_vx,post_vy,post_vz,px_vx,px_vy,px_vz,dq,com_x,com_y,com_z\n",
                               g_dump);
            }
            if (g_dump) {
                PxVec3 pv = e.actor->getLinearVelocity();
                const float* c = BodyCom(b);
                std::fprintf(g_dump,
                             "%u,%d,%u,%u,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
                             "%.4f,%.4f,%.4f,%.3f,%.4f,%.4f,%.4f\n",
                             g_tick, Handle(b), e.voxels, e.boxes, cls, preCom.x, preCom.y, preCom.z, np.x, np.y,
                             np.z, pxCom.x, pxCom.y, pxCom.z, e.v[0], e.v[1], e.v[2], nv.x, nv.y, nv.z, pv.x, pv.y,
                             pv.z, dq, c[0], c[1], c[2]);
            }
        }
    }
}

// ---- reporting ---------------------------------------------------------------------------------------------

// Diagnostics for pieces that never settle: how deep a body's colliders overlap the static blocks and the other
// owned bodies around it (PxGeometryQuery, exact). A piece wedged in geometry vibrates in place for ever.
struct Overlaps {
    float statics = 0.0f, debris = 0.0f;  // deepest overlap, metres
    int staticPairs = 0, debrisPairs = 0;  // pairs overlapping by more than 1 mm
    int staleBlocks = 0;  // overlapped static blocks whose voxels changed since they were built
};
Overlaps MeasureOverlaps(PxRigidDynamic* a) {
    Overlaps o;
    PxBounds3 wb = a->getWorldBounds();
    PxShape* mine[16];
    PxU32 nm = a->getShapes(mine, 16);
    auto test = [&](PxRigidActor* other, float& deepest, int& pairs) {
        PxShape* theirs[64];
        const PxU32 total = other->getNbShapes();
        for (PxU32 start = 0; start < total; start += 64) {  // static blocks hold thousands of boxes
            PxU32 nt = other->getShapes(theirs, 64, start);
            for (PxU32 j = 0; j < nt; ++j) {
                PxTransform tp = PxShapeExt::getGlobalPose(*theirs[j], *other);
                if (!PxShapeExt::getWorldBounds(*theirs[j], *other).intersects(wb)) continue;
                for (PxU32 i = 0; i < nm; ++i) {
                    PxVec3 dir;
                    PxF32 depth = 0.0f;
                    if (PxGeometryQuery::computePenetration(dir, depth, mine[i]->getGeometry(),
                                                            PxShapeExt::getGlobalPose(*mine[i], *a),
                                                            theirs[j]->getGeometry(), tp)) {
                        deepest = std::max(deepest, float(depth));
                        pairs += depth > 0.001f;
                    }
                }
            }
        }
    };
    const int bs = std::max(4, g_config.pxBlockSize);
    for (auto& [sh, ss] : g_static)
        for (auto& [k, bl] : ss.blocks)
            if (bl.actor && bl.actor->getWorldBounds().intersects(wb)) {
                int before = o.staticPairs;
                test(bl.actor, o.statics, o.staticPairs);
                if (o.staticPairs > before && ss.vox && td::ShapeVoxels(sh) == ss.vox) {
                    px::VoxGrid g = Grid(ss.vox);
                    int lo[3] = {int(k & 1023) * bs, int((k >> 10) & 1023) * bs, int(k >> 20) * bs};
                    int hi[3] = {std::min(g.n[0], lo[0] + bs), std::min(g.n[1], lo[1] + bs),
                                 std::min(g.n[2], lo[2] + bs)};
                    if (GridOk(g) && px::HashRegion(g, lo, hi) != bl.hash) ++o.staleBlocks;
                }
            }
    for (auto& [b, e] : g_owned)
        if (e->actor && e->actor != a && e->actor->getWorldBounds().intersects(wb))
            test(e->actor, o.debris, o.debrisPairs);
    return o;
}

// `proxies`, `waiting`: tracked bodies with a kinematic actor, and without an actor (Report counts them). Awake in
// PhysX: the owned bodies the last step moved (asking each of 25,000 actors took part of a ~7 ms report).
void ReportOffload(bool final, const Stats& s, uint32_t proxies, uint32_t waiting) {
    const OffStats& o = final ? g_offAll : g_off;
    uint64_t t = s.ticks ? s.ticks : 1;
    const uint32_t awake = g_ownedMoved;
    uint64_t hb = 0;
    for (uint64_t h : o.handBacks) hb += h;
    static uint64_t skipsMark = 0, cwakesMark = 0;  // hook counters at the last window report
    uint64_t sk = g_contactSkips.load(), cw = g_constraintWakes.load();
    uint64_t skips = final ? sk : sk - skipsMark, cwakes = final ? cw : cw - cwakesMark;
    if (!final) {
        skipsMark = sk;
        cwakesMark = cw;
    }
    LOG("offload%s tick %u: %zu owned (%u awake in PhysX), %u proxies, %u waiting for static blocks; takeovers %llu, "
        "hand-backs %llu (held %llu, not free %llu, vehicle %llu, constraint %llu, watchdog %llu, near held/vehicle "
        "%llu, water %llu, fell off %llu); changes forwarded: %llu poses, %llu velocities, %llu wakes, %llu in-step impulses; static "
        "slots for native contacts %llu, constraint wakes %llu (since start: held-body wakes %llu, solver constraints left out %llu); impacts %llu, sounds %llu made, %llu played; support "
        "changes: %llu sleeping bodies woken, %llu static "
        "shapes broke loose, %llu static shapes appeared, %llu bodies made static, %zu static bodies moving (%llu "
        "started, %llu came to rest); %llu proxy teleports; written %llu; %llu ticks without PhysX; fastest %.1f m/s (body %d, "
        "mass %.3g); ms/tick: native "
        "%.2f, PhysX prepare %.2f, sync %.2f, step %.2f (whole step %.2f, max %.1f), write-back %.2f, total max %.1f",
        final ? " TOTAL" : "", g_tick, g_owned.size(), awake, proxies, waiting, o.takeovers, hb,
        o.handBacks[kKeepHeld], o.handBacks[kKeepNotFree], o.handBacks[kKeepVehicle], o.handBacks[kKeepConstraint],
        o.handBacks[kKeepWatchdog], o.handBacks[kKeepZone], o.handBacks[kKeepWater], o.handBacks[kKeepOutside],
        o.poseDiffs, o.velDiffs,
        o.nativeWakes, o.stepDv, skips, cwakes, g_heldWakes.load(), g_droppedConstraints, g_impactReports, g_soundsMade,
        g_soundsInjected, o.supportWakes,
        o.staticReleased, o.newStatics, o.madeStatic, g_moving.size(), o.movingStarted, o.movingStopped,
        o.proxyTeleports, o.written, o.skippedTicks,
        g_maxSpeed, g_maxSpeedHandle, g_maxSpeedMass, s.nativeMs / t, s.prepareMs / t, s.syncMs / t, s.stepMs / t,
        s.realStepMs / t, s.realStepMaxMs, s.writeMs / t, s.totalMaxMs);
    if (!final) {
        const uint64_t* h = g_speedHist;
        const double n = double(std::max<uint64_t>(1, t));
        LOG("offload bodies PhysX moved, per tick by speed: <5 cm/s %.0f, 5-20 cm/s %.0f, 0.2-1 m/s %.0f, faster %.0f",
            double(h[0]) / n, double(h[1]) / n, double(h[2]) / n, double(h[3]) / n);
        for (uint64_t& c : g_speedHist) c = 0;
        char movers[2048];
        int len = 0;
        movers[0] = 0;
        auto clampLen = [&] { len = std::clamp(len, 0, int(sizeof(movers)) - 1); };
        // every proxy's native bounds, once for the three (BodyAabb walks its shapes: 1,500 proxies in Volkograd)
        static std::vector<std::pair<uint8_t*, PxBounds3>> proxyBounds;
        proxyBounds.clear();
        if (g_movers[0].b)
            for (uint8_t* pb : g_proxies) {
                float mn[3], mx[3];
                if (BodyAabb(pb, mn, mx)) proxyBounds.push_back({pb, PxBounds3(V(mn), V(mx))});
            }
        for (const Mover& m : g_movers) {
            auto it = m.b ? g_owned.find(m.b) : g_owned.end();
            if (it == g_owned.end() || !it->second->actor) continue;
            BodyEntry& e = *it->second;
            PxTransform pose = e.actor->getGlobalPose();
            PxVec3 com = pose.transform(e.actor->getCMassLocalPose().p);
            float nearest = 1e9f;  // distance to the nearest proxy moved in the last tick
            for (uint8_t* pb : g_proxiesMovedTick) {
                auto pit = g_bodies.find(pb);
                float mn[3], mx[3];  // (a split proxy's actor has no colliders: the native bounds)
                if (pit == g_bodies.end() || !pit->second.actor || !pit->second.kinematic || !BodyAabb(pb, mn, mx))
                    continue;
                PxVec3 c(std::clamp(com.x, mn[0], mx[0]), std::clamp(com.y, mn[1], mx[1]),
                         std::clamp(com.z, mn[2], mx[2]));
                nearest = std::min(nearest, (c - com).magnitude());
            }
            // the nearest proxy of any kind, by its native bounds: a piece a proxy presses against the world can't
            // get out (a kinematic doesn't give way) and jitters for ever
            float nearAny = 1e9f;
            const BodyEntry* nearEntry = nullptr;
            uint8_t* nearBody = nullptr;
            for (const auto& [pb, bb] : proxyBounds) {
                PxVec3 c(std::clamp(com.x, bb.minimum.x, bb.maximum.x), std::clamp(com.y, bb.minimum.y, bb.maximum.y),
                         std::clamp(com.z, bb.minimum.z, bb.maximum.z));
                float d = (c - com).magnitude();
                if (d >= nearAny) continue;
                auto pit = g_bodies.find(pb);
                if (pit == g_bodies.end()) continue;
                nearAny = d;
                nearEntry = &pit->second;
                nearBody = pb;
            }
            // (walks every static block and owned body: ~15 ms with a big pile, so only when asked for)
            Overlaps ov = g_config.pxDiagOverlaps ? MeasureOverlaps(e.actor) : Overlaps{};
            len += std::snprintf(movers + len, sizeof(movers) - size_t(len),
                                 "%sbody %d %.2f m/s at (%.1f, %.1f, %.1f), %u voxels, %u colliders, owned %u ticks, "
                                 "overlaps static %.0f mm (%d pairs, %d stale blocks) debris %.0f mm (%d pairs), nearest "
                                 "moving proxy %s",
                                 len ? "; " : "", Handle(m.b), m.speed, com.x, com.y, com.z, e.voxels, e.boxes,
                                 g_tick - e.ownedSince, ov.statics * 1000.0f, ov.staticPairs, ov.staleBlocks,
                                 ov.debris * 1000.0f,
                                 ov.debrisPairs, nearest < 1e8f ? "" : "none");
            clampLen();
            if (nearest < 1e8f) len += std::snprintf(movers + len, sizeof(movers) - size_t(len), "%.1f m", nearest);
            clampLen();
            if (nearEntry)
                len += std::snprintf(movers + len, sizeof(movers) - size_t(len),
                                     ", nearest proxy body %d %.1f m (%u voxels, %u colliders, %s natively)",
                                     Handle(nearBody), nearAny, nearEntry->voxels, nearEntry->boxes,
                                     BodyAwake(nearBody) ? "awake" : "asleep");
            clampLen();
        }
        LOG("offload creep: %.1f proxies moved per tick (%zu in the last), %.1f held by the deadband, %.1f jiggling "
            "(moved without waking anything); %.1f moves per "
            "tick too small to write; %.0f quiet bodies passed over per tick; %llu wedged pieces put to sleep (%llu in "
            "all); %llu put to sleep with their group (%llu in all), %llu of them still moving (%llu in all; groups: "
            "%u put to sleep, %u not, the largest awake %u pieces, %u of them moving, %u held back, the fastest %.2f "
            "m/s and %.1f cm on; %llu large pieces joined by their bounds, %llu without bounds; %.1f ms, at most "
            "%.1f); %llu woke again in the step after a check (%llu put to sleep again at once), %llu held back, "
            "%llu woken by the game's step; "
            "%zu frozen (%llu froze, %llu thawed, %zu queued, %llu no longer settled at their turn, %llu failed; "
            "settle checks looked at %llu, %llu settled long enough; thaw boxes from %llu movers, %llu proxies, %llu "
            "player, %llu impulses); %llu builds deferred; %llu pieces launched (+25 m/s in a step) since start; fastest "
            "owned bodies: %s",
            double(g_proxyMovesWin) / n, g_proxiesMovedTick.size(), double(g_proxyHeldWin) / n,
            double(g_proxyJiggleWin) / n, double(g_writeRestWin) / n, double(g_quietWin) / n, g_wedgedWin, g_wedgedAll,
            g_clusterSleptWin, g_clusterSleptAll, g_settleStoppedWin, g_settleStoppedAll, g_clustersQuietWin,
            g_clustersLoudWin,
            g_largestLoudWin, g_largestLoudMovers, g_largestLoudHeld, g_largestLoudSpeed, 100.0f * g_largestLoudMoved,
            g_settleLargeWin, g_settleLoneWin, g_settleMsWin, g_settleMaxMsWin, g_settleRewokeWin, g_settleRetriedWin,
            g_settleHeldWin, g_gameWokeWin, g_frozenCount, g_freezes,
            g_thaws, g_freezeQueue.size(), g_freezeUnsettled, g_freezeFailed, g_settleLooked, g_settleLong,
            g_thawBoxesBy[kThawMover], g_thawBoxesBy[kThawProxy], g_thawBoxesBy[kThawPlayer],
            g_thawBoxesBy[kThawImpulse], g_buildsDeferred, g_launches, movers[0] ? movers : "none");
        {  // which proxies move (and thaw around them)
            struct Top {
                uint8_t* b;
                BodyEntry* e;
            } top[5] = {};
            for (uint8_t* pb : g_proxies) {
                auto pit = g_bodies.find(pb);
                if (pit == g_bodies.end()) continue;
                BodyEntry* pe = &pit->second;
                if (pe->movesWin) {
                    for (int k = 0; k < 5; ++k)
                        if (!top[k].e || pe->movesWin > top[k].e->movesWin) {
                            for (int j = 4; j > k; --j) top[j] = top[j - 1];
                            top[k] = {pb, pe};
                            break;
                        }
                }
            }
            char list[2400];
            int pl = 0;
            list[0] = 0;
            for (const Top& tp : top) {
                if (!tp.e || pl >= int(sizeof(list)) - 400) break;
                uint8_t* s0 = FirstShape(tp.b);
                bool jointed = false;
                int guard = 0;
                for (uint8_t* sh = s0; sh && guard < 4096; sh = NextShape(sh), ++guard)
                    if (td::EntityType(sh) == 2 && ShapeJointed(sh)) {
                        jointed = true;
                        break;
                    }
                // what its joints hold on to: o owned, p proxy, s static, u untracked (first 4)
                char joined[96] = "";
                int jl = 0, nj = 0;
                guard = 0;
                for (uint8_t* sh = s0; sh && guard < 4096 && nj < 4; sh = NextShape(sh), ++guard) {
                    if (td::EntityType(sh) != 2) break;
                    int jg = 0;
                    for (uint8_t* j = td::At<uint8_t*>(sh, 0xf8); j && jg < 16 && nj < 4; ++jg) {
                        uint8_t* a = td::At<uint8_t*>(j, 0x40);
                        uint8_t* bsh = td::At<uint8_t*>(j, 0x48);
                        uint8_t* other = a == sh ? bsh : a;
                        uint8_t* ob = other ? ShapeBody(other) : nullptr;
                        char kind = 'u';
                        if (!ob)
                            kind = '-';
                        else if (!BodyDynamic(ob))
                            kind = 's';
                        else if (g_owned.count(ob))
                            kind = 'o';
                        else if (g_proxies.count(ob))
                            kind = 'p';
                        jl += std::snprintf(joined + jl, sizeof(joined) - size_t(jl), "%s%c%d", nj ? "," : "", kind,
                                            ob ? Handle(ob) : 0);
                        if (jl >= int(sizeof(joined)) - 16) break;
                        ++nj;
                        j = td::At<uint8_t*>(j, a == sh ? 0x50 : 0x58);
                    }
                }
                const float* c = BodyCom(tp.b);
                pl += std::snprintf(list + pl, sizeof(list) - size_t(pl),
                                     "%sbody %d: %u moves, %.2f m, %u thaw boxes (%u voxels, %zu shapes%s%s, %s "
                                     "natively at %.1f m/s, at (%.1f, %.1f, %.1f); joined to %s)",
                                     pl ? "; " : "", Handle(tp.b), tp.e->movesWin, tp.e->travelWin, tp.e->thawBoxesWin,
                                     tp.e->voxels, tp.e->sig.size(), tp.e->split ? ", split" : "",
                                     jointed ? ", jointed" : "", BodyAwake(tp.b) ? "awake" : "asleep",
                                     V(BodyVel(tp.b)).magnitude(), c[0], c[1], c[2], joined[0] ? joined : "nothing");
            }
            if (list[0]) LOG("offload proxies that moved most: %s", list);
            for (uint8_t* pb : g_proxies) {
                auto pit = g_bodies.find(pb);
                if (pit == g_bodies.end()) continue;
                pit->second.movesWin = pit->second.thawBoxesWin = 0;
                pit->second.travelWin = 0.0f;
            }
        }
        g_freezes = g_thaws = 0;
        g_settleLooked = g_settleLong = g_freezeUnsettled = g_freezeFailed = g_buildsDeferred = 0;
        for (uint64_t& c : g_thawBoxesBy) c = 0;
        g_proxyMovesWin = g_proxyHeldWin = g_proxyJiggleWin = g_writeRestWin = g_quietWin = g_wedgedWin = 0;
        g_clusterSleptWin = g_settleStoppedWin = g_settleRewokeWin = g_settleRetriedWin = g_settleHeldWin = 0;
        g_gameWokeWin = 0;
        g_settleLargeWin = g_settleLoneWin = 0;
        g_clustersQuietWin = g_clustersLoudWin = g_largestLoudWin = g_largestLoudMovers = g_largestLoudHeld = 0;
        g_largestLoudSpeed = g_largestLoudMoved = 0.0f;
        g_settleMsWin = g_settleMaxMsWin = 0.0;
        g_maxSpeed = 0.0f;
        uint64_t tscNow = __rdtsc(), qpcNow = trace::Now();
        double ms = g_phaseQpc0 ? Ms(g_phaseQpc0, qpcNow) : 0.0;
        double perMs = ms > 0.0 ? double(tscNow - g_phaseTsc0) / ms : 0.0;  // TSC cycles per ms
        auto ph = [&](int i) { return perMs > 0.0 ? double(g_phase[i]) / perMs / double(t) : 0.0; };
        if (perMs > 0.0)
            LOG("offload prepare ms/tick: signature %.2f, classify %.2f, statics %.2f, build %.2f, takeover/forward "
                "%.2f, cleanup %.2f, grid/refresh %.2f",
                ph(kPhSignature), ph(kPhClassify), ph(kPhStatics), ph(kPhBuild), ph(kPhForward), ph(kPhCleanup),
                ph(kPhRefresh));
        for (uint64_t& p : g_phase) p = 0;
        g_phaseTsc0 = tscNow;
        g_phaseQpc0 = qpcNow;
    }
    if (g_offCsv && !final) {
        logx::Printf(g_offCsv, "%u,%zu,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                               "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%llu\n",
                     g_tick, g_owned.size(), awake, proxies, waiting, o.takeovers, hb, o.handBacks[kKeepHeld],
                     o.handBacks[kKeepNotFree], o.handBacks[kKeepVehicle], o.handBacks[kKeepConstraint],
                     o.handBacks[kKeepWatchdog], o.poseDiffs, o.velDiffs, o.nativeWakes, o.stepDv, skips, cwakes,
                     o.written, s.nativeMs / t, s.nativeMaxMs, s.prepareMs / t, s.syncMs / t, s.stepMs / t,
                     s.writeMs / t, s.totalMaxMs, o.handBacks[kKeepZone], o.handBacks[kKeepWater], o.supportWakes,
                     o.staticReleased, o.proxyTeleports, o.handBacks[kKeepOutside]);
    }
}

void ReportDeaths(bool final);

void Report(bool final) {
    const Stats& s = final ? g_all : g_win;
    const Divergence* d = final ? g_divAll : g_div;
    uint64_t t = s.ticks ? s.ticks : 1;
    uint32_t bodies = 0, proxies = 0, awake = 0, boxes = 0, waiting = 0;
    for (auto& [b, e] : g_bodies) {
        if (!e.actor) {
            ++waiting;
            continue;
        }
        if (e.kinematic) {
            ++proxies;
        } else {
            ++bodies;
            awake += e.awake;
        }
        boxes += e.boxes;
    }
    uint32_t blocks = 0, blockBoxes = 0;
    for (auto& [sh, ss] : g_static)
        for (auto& [k, bl] : ss.blocks) {
            blocks += bl.actor != nullptr;
            blockBoxes += bl.boxes;
        }
    char div[768];
    int len = 0;
    for (int c = 0; c < 3; ++c) {
        double n = double(d[c].n ? d[c].n : 1);
        len += std::snprintf(div + len, sizeof(div) - size_t(len),
                             "%s %llu: dp %.2f/%.1f mm (native step %.2f mm), dq %.3f deg, dv %.3f m/s; ",
                             c == 0 ? "rest" : c == 1 ? "contact" : "free", d[c].n, d[c].dp / n * 1000.0, d[c].P95mm(),
                             d[c].step / n * 1000.0, d[c].dq / n, d[c].dv / n);
    }
    LOG("shadow%s tick %u: %u bodies (%u awake natively, %u moving in PhysX), %u proxies (%u jointed, %u large), %u boxes; "
        "static %zu shapes, "
        "%u blocks (%u boxes); "
        "ms/tick: native physics %.2f (max %.1f), PhysX prepare %.2f, sync %.2f, step %.2f (max %.1f), compare %.2f; "
        "builds: %llu bodies %.1f ms, %llu blocks %.1f ms, %llu block checks, %u oversized queries skipped, %llu hull "
        "failures, %llu static shapes re-buffered; hulls %llu, pieces in several hulls %llu, too irregular for those "
        "(boxes) %llu",
        final ? " TOTAL" : "", g_tick, bodies, awake, g_lastActive, proxies, g_proxyJointed, g_proxyLarge, boxes, g_static.size(), blocks, blockBoxes,
        s.nativeMs / t, s.nativeMaxMs, s.prepareMs / t, s.syncMs / t, s.stepMs / t, s.stepMaxMs, s.compareMs / t,
        s.bodyBuilds, s.bodyBuildMs, s.blockBuilds, s.blockBuildMs, s.blockChecks, g_skippedQueries, g_hullFailures,
        g_rebuffered, g_hullsMade, g_hullsSplit, g_hullsBloated);
    if (g_config.pxShadow == 1) LOG("shadow%s one-step divergence vs native: %s", final ? " TOTAL" : "", div);
    if (g_offload) ReportOffload(final, s, proxies, waiting);
    LOG("shadow%s impact events since start: the game's own %llu (%llu audible); PhysX %llu reports, %llu sounds made, "
        "%llu played; damage: %llu fast contact points, %llu breaking contacts, %llu holes in %llu carves (%llu "
        "skipped); %llu stale shape reads dropped, %llu points naming a destroyed shape dropped (%llu shapes died "
        "whose body was gone)",
        final ? " TOTAL" : "", g_gameEvents, g_gameAudible, g_impactReports, g_soundsMade, g_soundsInjected,
        g_damagePoints, g_damageHits, g_damageHoles, g_damageCarves, g_damageSkipped, g_staleReads, g_deadNamed,
        g_shapeDeathsUnowned);
    ReportDeaths(final);
    if (PxScene* sc = px::Scene()) {
        PxSimulationStatistics st;
        sc->getSimulationStatistics(st);
        LOG("shadow%s PhysX last step: %u active dynamic, %u active kinematic, %u static actors, %u aggregates; contact "
            "pairs %u (%u touching, %u cache hits), new/lost touches %u/%u, solver constraints %u, partitions %u",
            final ? " TOTAL" : "", st.nbActiveDynamicBodies, st.nbActiveKinematicBodies, st.nbStaticBodies,
            st.nbAggregates, st.nbDiscreteContactPairsTotal, st.nbDiscreteContactPairsWithContacts,
            st.nbDiscreteContactPairsWithCacheHits, st.nbNewTouches, st.nbLostTouches, st.nbAxisSolverConstraints,
            st.nbPartitions);
        if (px::GpuActive()) {
            const PxGpuDynamicsMemoryConfigStatistics& g = st.gpuDynamicsMemoryConfigStatistics;
            LOG("shadow%s PhysX GPU buffers, most needed so far: %u contacts, %u patches, %u found/lost pairs, %u "
                "found/lost aggregate pairs, %u aggregate pairs, temp %.1f MB, collision stack %.1f MB",
                final ? " TOTAL" : "", g.rigidContactCount, g.rigidPatchCount, g.foundLostPairs,
                g.foundLostAggregatePairs, g.totalAggregatePairs, double(g.tempBufferCapacity) / 1048576.0,
                double(g.collisionStackSize) / 1048576.0);
        }
    }
    if (!final) gpukernels::Report(unsigned(s.ticks));
    if (g_csv && !final) {
        logx::Printf(g_csv, "%u,%u,%u,%u,%u,%zu,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%.3f,%llu,%.3f",
                     g_tick, bodies, awake, proxies, boxes, g_static.size(), blocks, blockBoxes, s.nativeMs / t,
                     s.nativeMaxMs, s.prepareMs / t, s.syncMs / t, s.stepMs / t, s.stepMaxMs, s.compareMs / t,
                     s.bodyBuilds, s.bodyBuildMs, s.blockBuilds, s.blockBuildMs);
        for (int c = 0; c < 3; ++c) {
            double n = double(d[c].n ? d[c].n : 1);
            logx::Printf(g_csv, ",%llu,%.4f,%.4f,%.4f,%.4f", d[c].n, d[c].dp / n * 1000.0, d[c].P95mm(), d[c].dq / n,
                         d[c].dv / n);
        }
        logx::Printf(g_csv, ",%u\n", g_wokenByPhysx);
    }
}

void Accumulate() {
    g_all.ticks += g_win.ticks;
    g_all.prepareMs += g_win.prepareMs;
    g_all.syncMs += g_win.syncMs;
    g_all.stepMs += g_win.stepMs;
    g_all.compareMs += g_win.compareMs;
    g_all.writeMs += g_win.writeMs;
    g_all.nativeMs += g_win.nativeMs;
    g_all.bodyBuildMs += g_win.bodyBuildMs;
    g_all.blockBuildMs += g_win.blockBuildMs;
    g_all.bodyBuilds += g_win.bodyBuilds;
    g_all.blockBuilds += g_win.blockBuilds;
    g_all.blockChecks += g_win.blockChecks;
    g_all.stepMaxMs = std::max(g_all.stepMaxMs, g_win.stepMaxMs);
    g_all.realStepMs += g_win.realStepMs;
    g_all.realStepMaxMs = std::max(g_all.realStepMaxMs, g_win.realStepMaxMs);
    g_all.nativeMaxMs = std::max(g_all.nativeMaxMs, g_win.nativeMaxMs);
    g_all.totalMaxMs = std::max(g_all.totalMaxMs, g_win.totalMaxMs);
    g_win = Stats{};
    for (Divergence& d : g_div) d = Divergence{};
    g_wokenByPhysx = 0;
    g_offAll.takeovers += g_off.takeovers;
    for (int i = 0; i < kKeepCount; ++i) g_offAll.handBacks[i] += g_off.handBacks[i];
    g_offAll.poseDiffs += g_off.poseDiffs;
    g_offAll.velDiffs += g_off.velDiffs;
    g_offAll.nativeWakes += g_off.nativeWakes;
    g_offAll.stepDv += g_off.stepDv;
    g_offAll.written += g_off.written;
    g_offAll.waiting += g_off.waiting;
    g_offAll.supportWakes += g_off.supportWakes;
    g_offAll.staticReleased += g_off.staticReleased;
    g_offAll.newStatics += g_off.newStatics;
    g_offAll.madeStatic += g_off.madeStatic;
    g_offAll.movingStarted += g_off.movingStarted;
    g_offAll.movingStopped += g_off.movingStopped;
    g_offAll.proxyTeleports += g_off.proxyTeleports;
    g_offAll.skippedTicks += g_off.skippedTicks;
    g_off = OffStats{};
}

// ---- lifetime ----------------------------------------------------------------------------------------------

bool Start() {
    if (g_config.pxShadow == 3) {
        // Multiplayer replicates bodies itself. The network object's second field is the session state (names at
        // 0x140a1c1f8: 0 OFFLINE, 1 CONNECTING, 2 ONLINE, 3 LOADING, 4 PLAYING; the engine sends body messages in
        // state 4, 0x140030f70). The first field is 1 in single player too.
        uint8_t* g = td::GameObj();
        uint8_t* net = g ? td::At<uint8_t*>(g, 0x160) : nullptr;
        int n0 = net ? td::At<int>(net, 0) : -1, n1 = net ? td::At<int>(net, 4) : -1;
        LOG("offload: network state %d/%d", n0, n1);
        if (n1 != 0) {
            LOG("offload: multiplayer session, PhysX stays off");
            return false;
        }
    }
    px::WorldParams p;
    // PhysX steps after the native step, while the engine's workers wait: most cores are free then
    p.threads = g_config.pxThreads > 0 ? g_config.pxThreads
                                       : std::max(2, int(std::thread::hardware_concurrency()) - 4);
    p.gravity = g_config.pxGravity;
    p.staticFriction = p.dynamicFriction = g_config.pxFriction;
    p.restitution = g_config.pxRestitution;
    p.tgs = g_config.pxTgs;
    p.stabilization = g_config.pxStabilization;
    p.killKinematicPairs = g_config.pxKillKinematicPairs;
    p.events = g_config.pxShadow == 3 ? &g_collector : nullptr;  // sleep/wake events keep physxAwake exact
    p.reportContacts = g_config.pxImpacts || g_config.pxDamage;
    p.noKinematicContacts = g_config.pxDebugNoKinematicContacts;
    p.gpu = g_config.pxGpu;
    p.gpuBroadphase = g_config.pxGpuBroadphase;
    p.gpuContacts = unsigned(std::max(1024, g_config.pxGpuContacts));
    p.gpuPatches = unsigned(std::max(1024, g_config.pxGpuPatches));
    p.gpuPairs = unsigned(std::max(1024, g_config.pxGpuPairs));
    p.gpuAggregatePairs = unsigned(std::max(1024, g_config.pxGpuAggregatePairs));
    p.gpuPartitions = unsigned(std::clamp(g_config.pxGpuPartitions, 1, 32));
    p.gpuSync = g_config.pxGpuSync;
    g_impacts.clear();
    if (!px::CreateWorld(p)) return false;
    if (px::GpuActive()) gpukernels::Start();
    g_staticAggregates = g_config.pxStaticAggregates > 0 || (g_config.pxStaticAggregates < 0 && !px::GpuBroadphase());
    g_active = true;
    g_offload = g_config.pxShadow == 3;
    g_async = g_offload && g_config.pxAsync;
    g_stepPending = false;
    for (uint64_t& ph : g_phase) ph = 0;
    g_phaseTsc0 = __rdtsc();
    g_phaseQpc0 = trace::Now();
    g_gridDirty = true;
    g_all = Stats{};
    g_win = Stats{};
    g_off = OffStats{};
    g_offAll = OffStats{};
    for (Divergence& d : g_divAll) d = Divergence{};
    for (Divergence& d : g_div) d = Divergence{};
    if (!g_csv) {
        g_csv = std::fopen((g_config.runDir + "\\shadow.csv").c_str(), "w");
        if (g_csv)
            std::fputs("tick,bodies,awake,proxies,boxes,static_shapes,blocks,block_boxes,native_ms,native_max_ms,"
                       "prepare_ms,sync_ms,step_ms,step_max_ms,compare_ms,body_builds,body_build_ms,"
                       "block_builds,block_build_ms,rest_n,rest_dp_mm,rest_dp95_mm,rest_dq_deg,rest_dv,contact_n,"
                       "contact_dp_mm,contact_dp95_mm,contact_dq_deg,contact_dv,free_n,free_dp_mm,free_dp95_mm,free_dq_deg,"
                       "free_dv,woken_by_physx\n",
                       g_csv);
    }
    if (g_offload && !g_offCsv) {
        g_offCsv = std::fopen((g_config.runDir + "\\offload.csv").c_str(), "w");
        if (g_offCsv)
            std::fputs("tick,owned,awake,proxies,waiting,takeovers,hand_backs,hb_held,hb_not_free,hb_vehicle,"
                       "hb_constraint,hb_watchdog,pose_diffs,vel_diffs,native_wakes,step_impulses,contact_skips,"
                       "constraint_wakes,written,native_ms,native_max_ms,prepare_ms,sync_ms,step_ms,writeback_ms,"
                       "total_max_ms,hb_zone,hb_water,support_wakes,static_released,proxy_teleports,hb_outside\n",
                       g_offCsv);
    }
    if (g_offload)
        if (uint8_t* pl = td::LocalPlayer())
            LOG("offload: player feet y %.2f, eye y %.2f (capsule height %.2f, radius %.2f)", td::At<float>(pl, 0x574),
                td::At<float>(pl, 0x644), g_config.pxPlayerHeight, g_config.pxPlayerRadius);
    LOG("shadow: started (mode %s%s, max %d voxels per owned body, %d boxes per shape, %d^3 static blocks%s; the "
        "game's collision filters, small debris %s)",
        g_config.pxShadow == 1 ? "sync" : g_config.pxShadow == 2 ? "free" : "offload", g_async ? ", async step" : "",
        g_config.pxMaxBodyVoxels, g_config.pxBoxBudget, g_config.pxBlockSize,
        g_staticAggregates ? " as aggregates" : " as plain actors",
        g_config.pxSmallDebrisCollide ? "colliding with all debris" : "hitting static geometry only, as in the game");
    return true;
}

void Stop() {
    if (!g_active) return;
    g_inStep.store(false);
    if (g_stepPending) {  // async: the step started last tick must finish before anything is released
        px::Scene()->fetchResults(true);
        g_stepPending = false;
    }
    ReleaseDeferred();
    Accumulate();
    Report(true);
    if (g_offload) {
        // the game carries on with every owned body from PhysX's last state
        for (auto& [b, e] : g_owned) WakeNative(b);
        g_owned.clear();
        ++g_ownedVersion;
        g_offload = false;
    }
    for (auto& [b, e] : g_bodies) ReleaseActors(e);
    g_bodies.clear();
    g_frozenGrid.clear();
    g_frozenCount = 0;
    g_thawBoxes.clear();
    g_freezeQueue.clear();
    g_shapeOwner.clear();
    g_diedOwners.clear();
    g_proxies.clear();
    g_listCache.clear();
    for (auto& [s, ss] : g_static) ReleaseStatic(ss);
    g_static.clear();
    g_grid.clear();
    g_gridShapes.clear();
    g_deadShapes.clear();
    g_modified.clear();
    g_staticOrder.clear();
    g_staticNext = 0;
    for (auto& [b, m] : g_moving) ReleaseActor(m.actor);
    g_moving.clear();
    g_staticPose.clear();
    g_requeryNext.clear();
    AcquireSRWLockExclusive(&g_newLock);
    g_newShapes.clear();
    ReleaseSRWLockExclusive(&g_newLock);
    ReleaseActor(g_player);
    g_stepWoken.clear();
    g_stepDead.clear();
    g_changed.clear();
    g_impacts.clear();
    g_sounds.clear();
    g_deadSinceKick.clear();
    g_damagePts.clear();
    g_holes.clear();
    g_carveDead.clear();
    g_pendBodies.clear();
    g_pendKinematic.clear();
    g_sleptAt = 0;
    g_holdForPlayer = false;
    g_skipTick = false;
    g_pendingTicks = 0;
    px::DestroyWorld();
    g_active = false;
    logx::Flush();  // the CSV rows queued for the log writer
    if (g_csv) std::fflush(g_csv);
    if (g_offCsv) std::fflush(g_offCsv);
}

// ---- hooks -------------------------------------------------------------------------------------------------

// Who destroys bodies, and how old and large they were (diagnostics, in the report): the caller is the engine code
// that deleted the body (return address of the destructor call).
struct DeathStats {
    uint64_t total = 0, tracked = 0, owned = 0;
    uint64_t age[4]{};  // ticks since the plugin first saw it: <10, <60, <600, more
    uint64_t vox[5]{};  // voxels at its last collider build: 0, 1-16, 17-64, 65-512, more
    std::unordered_map<uint32_t, uint64_t> callers;  // caller RVA in teardown.exe (0xffffffff: elsewhere)
};
DeathStats g_deaths;
SRWLOCK g_deathLock = SRWLOCK_INIT;

void NoteDeath(uintptr_t ret, const BodyEntry* e) {
    const uintptr_t base = td::Base();
    const uint32_t rva = ret >= base && ret - base < 0x10000000 ? uint32_t(ret - base) : 0xffffffffu;
    AcquireSRWLockExclusive(&g_deathLock);
    ++g_deaths.total;
    ++g_deaths.callers[rva];
    if (e) {
        ++g_deaths.tracked;
        g_deaths.owned += e->owned;
        const uint32_t age = e->born && g_tick >= e->born ? g_tick - e->born : 0;
        ++g_deaths.age[age < 10 ? 0 : age < 60 ? 1 : age < 600 ? 2 : 3];
        const uint32_t v = e->voxels;
        ++g_deaths.vox[v == 0 ? 0 : v <= 16 ? 1 : v <= 64 ? 2 : v <= 512 ? 3 : 4];
    }
    ReleaseSRWLockExclusive(&g_deathLock);
}

void ReportDeaths(bool final) {
    AcquireSRWLockShared(&g_deathLock);
    std::vector<std::pair<uint32_t, uint64_t>> top(g_deaths.callers.begin(), g_deaths.callers.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    char callers[256];
    int len = 0;
    callers[0] = 0;
    for (size_t i = 0; i < top.size() && i < 6; ++i)
        len += std::snprintf(callers + len, sizeof(callers) - size_t(len), "%s%#x x%llu", i ? ", " : "", top[i].first,
                             top[i].second);
    const DeathStats& d = g_deaths;
    LOG("shadow%s bodies destroyed since start: %llu (%llu tracked, %llu simulated by PhysX); age in ticks "
        "<10/<60/<600/more: %llu/%llu/%llu/%llu; voxels 0/1-16/17-64/65-512/more: %llu/%llu/%llu/%llu/%llu; "
        "destroyed from %s",
        final ? " TOTAL" : "", d.total, d.tracked, d.owned, d.age[0], d.age[1], d.age[2], d.age[3], d.vox[0], d.vox[1],
        d.vox[2], d.vox[3], d.vox[4], callers);
    ReleaseSRWLockShared(&g_deathLock);
}

void* __fastcall HkBodyDtor(uint8_t* self, uint32_t flags) {
    if (g_active) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (g_inStep.load(std::memory_order_acquire)) {  // engine jobs may be reading the maps: forget it after
            NoteDeath(ret, nullptr);
            AcquireSRWLockExclusive(&g_stepLock);
            g_stepDead.push_back(self);
            ReleaseSRWLockExclusive(&g_stepLock);
        } else {
            AcquireSRWLockExclusive(&g_lock);
            ForgetStatic(self);
            auto it = g_bodies.find(self);
            NoteDeath(ret, it != g_bodies.end() ? &it->second : nullptr);
            if (it != g_bodies.end()) EraseBody(it);
            if (!g_holes.empty())
                g_holes.erase(std::remove_if(g_holes.begin(), g_holes.end(),
                                             [self](const auto& h) { return h.first == self; }),
                              g_holes.end());
            if (g_carving) g_carveDead.insert(self);
            ReleaseSRWLockExclusive(&g_lock);
        }
    }
    return oBodyDtor(self, flags);
}

void* __fastcall HkShapeDtor(uint8_t* self, uint32_t flags) {
    if (g_active) {
        AcquireSRWLockExclusive(&g_lock);
        g_deadSinceKick.insert(self);  // PhysX may still name it until the next scan
        // which rebuilds the colliders built from it, quiet or not (the engine may have unlinked it from its body)
        auto oit = g_shapeOwner.find(self);
        uint8_t* owner = oit != g_shapeOwner.end() ? oit->second : ShapeBody(self);
        auto bit = owner ? g_bodies.find(owner) : g_bodies.end();
        if (bit != g_bodies.end()) {
            bit->second.dirty |= 2;
            g_diedOwners.push_back(owner);
        } else if (oit != g_shapeOwner.end())
            ++g_shapeDeathsUnowned;
        if (oit != g_shapeOwner.end()) g_shapeOwner.erase(oit);
        for (SoundEvent& ev : g_sounds)
            for (int k = 0; k < 2; ++k)
                if (ev.shape[k] == self) {
                    ev.shape[k] = nullptr;
                    ev.material[k] = 0;
                }
        auto it = g_static.find(self);
        if (it != g_static.end()) {
            ReleaseStatic(it->second);
            g_static.erase(it);
        }
        if (g_gridShapes.erase(self)) g_deadShapes.insert(self);  // the grid still refers to it
        if (g_deadShapes.size() > 4096) g_gridDirty = true;
        AcquireSRWLockExclusive(&g_newLock);  // made and gone before IndexNewStatics saw it
        g_newShapes.erase(self);
        ReleaseSRWLockExclusive(&g_newLock);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return oShapeDtor(self, flags);
}

// NarrowWake's calls from contact processing, as return addresses: add-contact 0x1400acb40 and the manifold
// loops in 0x1400b4850 / 0x1400b59a8. The other callers (vehicles, joints, wheels, script constraints) want to
// constrain the body.
constexpr uint32_t kContactWakeReturns[] = {0xacbae, 0xacbc5, 0xb49e3, 0xb4a0e, 0xb5a13, 0xb5a3d};

// A contact with an owned body needs a solver slot (the solver maps every contact's body index through a table;
// -1 reads outside it). Give the body a static one, as Physics::update does for static bodies each step: zero
// inverse mass and inertia, identity mass frame, under the lock NarrowWake takes. The per-body pass resets the
// index to -1 at the next step's start.
void StaticSlot(uint8_t* physics, uint8_t* body) {
    static const float kZero[3] = {0.0f, 0.0f, 0.0f};
    static const float kFrame[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    void* mutex = physics + 0xa2f80;
    td::Fn<MutexFn>(Addr::MutexLock)(mutex);
    if (td::At<int>(body, 0x13c) < 0)
        td::At<int>(body, 0x13c) = td::Fn<SolverAddBodyFn>(Addr::SolverAddBody)(
            physics + 0x180c0, body + 0x40, body + 0x100, 0.0f, kZero, BodyVel(body), BodyAngVel(body), kFrame,
            td::At<float>(body, 0x148), td::At<uint8_t>(body, 0x150), td::At<float>(body, 0x14c),
            td::At<uint8_t>(body, 0x151), td::At<float>(body, 0x144), Handle(body), body);
    td::Fn<MutexFn>(Addr::MutexUnlock)(mutex);
}

// Runs on engine job threads during the native step; the maps are not modified while g_inStep is set.
void __fastcall HkNarrowWake(uint8_t* physics, uint8_t* body) {
    if (g_inStep.load(std::memory_order_acquire)) {
        uint32_t ra = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - td::Base());
        bool contact = false;
        for (uint32_t r : kContactWakeReturns) contact |= r == ra;
        auto it = g_owned.find(body);
        if (it != g_owned.end()) {
            if (contact) {  // owned debris is a static obstacle for the native body touching it
                StaticSlot(physics, body);
                g_contactSkips.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // a native constraint takes it: back with PhysX's velocity (the per-body pass zeroed it), then wake
            std::memcpy(BodyVel(body), it->second->expV, sizeof(float) * 3);
            std::memcpy(BodyAngVel(body), it->second->expW, sizeof(float) * 3);
            g_constraintWakes.fetch_add(1, std::memory_order_relaxed);
        }
        if (!contact && (it != g_owned.end() || g_bodies.count(body))) {
            AcquireSRWLockExclusive(&g_stepLock);
            g_stepWoken.push_back(body);
            ReleaseSRWLockExclusive(&g_stepLock);
        }
    }
    oNarrowWake(physics, body);
}

// The player's constraints (0x1400bcd50(player, solver), from Physics::update after the narrowphase): the grab is
// six constraints between the held body (player+0x788) and the grab anchor (player+0x7b8), made with the held
// body's solver index as it is, without the wake other constraint makers go through. A piece grabbed while PhysX
// owns it (asleep natively, index -1) gets that wake here, so the game simulates it this step and the offload
// hands it back after; the classification normally does it before the step, but not on a CatchUp tick.
using PlayerConstraintsFn = void(__fastcall*)(uint8_t* player, uint8_t* solver);
PlayerConstraintsFn oPlayerConstraints = nullptr;
void __fastcall HkPlayerConstraints(uint8_t* player, uint8_t* solver) {
    uint8_t* held = player ? td::At<uint8_t*>(player, 0x788) : nullptr;
    if (held && td::At<uint8_t*>(player, 0x7b8)) {
        const int slot = td::At<int>(held, 0x13c);
        if (slot < 0 || slot >= td::At<int>(solver, 0x48)) {
            td::Fn<NarrowWakeFn>(Addr::NarrowWake)(solver - 0x180c0, held);  // through HkNarrowWake
            g_heldWakes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    oPlayerConstraints(player, solver);
}

// The solver's first look at a step's constraints: TSolver::run (0x1405fd110) merges the threads' lists, then
// builds islands here (0x1405f80a0), mapping both bodies of every constraint through tables with one entry per
// solver body. A constraint naming a body without a solver index this step reads outside them and crashes
// (0x1405f84af, seen once). The engine gives every body it constrains an index; this catches any path where a body
// PhysX keeps asleep natively slipped through: the constraint is left out of this step, and logged.
// Constraint records: +0 type, +4 / +8 the two bodies' solver indices; TSolver+0x48 body count, +0x80328 constraint
// count, +0x80330 their byte offsets into the buffer at +0x84338.
using SolverIslandsFn = void(__fastcall*)(uint8_t* solver);
SolverIslandsFn oSolverIslands = nullptr;
void __fastcall HkSolverIslands(uint8_t* solver) {
    const int bodies = td::At<int>(solver, 0x48);
    int& count = td::At<int>(solver, 0x80328);
    int* offsets = td::At<int*>(solver, 0x80330);
    const uint8_t* data = td::At<uint8_t*>(solver, 0x84338);
    int kept = 0;
    for (int k = 0; offsets && data && k < count; ++k) {
        const int* rec = reinterpret_cast<const int*>(data + offsets[k]);
        if (rec[1] >= 0 && rec[1] < bodies && rec[2] >= 0 && rec[2] < bodies) {
            offsets[kept++] = offsets[k];
            continue;
        }
        if (++g_droppedConstraints <= 20 || g_droppedConstraints % 1000 == 0)
            LOG("offload: solver constraint left out (type %d, bodies %d and %d of %d; %llu so far)", rec[0], rec[1],
                rec[2], bodies, g_droppedConstraints);
    }
    count = kept;
    oSolverIslands(solver);
}

// The game merges its contact jobs' impact events into Physics+0xa3058 (0x1400b5390, from the narrowphase driver
// on the main thread); PhysX's impacts from the last step join them there, and the game's impact pass at the end
// of Physics::update plays them with its own volume, pitch, size classes and material sounds.
using MergeFn = void(__fastcall*)(void* a, void* jobs);
MergeFn oSoundListMerge = nullptr;
void __fastcall HkSoundListMerge(void* a, void* jobs) {
    uint8_t* physics = td::PhysicsObj();
    int before = physics ? td::At<int>(physics, 0xa3058) : 0;
    oSoundListMerge(a, jobs);
    if (physics) {
        int after = td::At<int>(physics, 0xa3058);
        const SoundEvent* data = td::At<SoundEvent*>(physics, 0xa3060);
        for (int i = std::max(0, before); data && i < after; ++i) {
            ++g_gameEvents;
            g_gameAudible += data[i].hit > 0.7f;  // (hit - 0.5) * 0.05 passes the game's 0.01
        }
    }
    if (!g_offload || !g_config.pxImpacts) return;
    AcquireSRWLockExclusive(&g_lock);
    if (physics)
        for (const SoundEvent& ev : g_sounds)
            if (ev.material[0] || ev.material[1]) {
                AppendSoundEvent(physics, ev);
                ++g_soundsInjected;
            }
    g_sounds.clear();
    ReleaseSRWLockExclusive(&g_lock);
}

uint8_t* BodyByHandle(int h) {
    uint8_t* s = td::SceneObj();
    if (!s || h == 0) return nullptr;
    uint8_t* e = nullptr;
    if (h > 0) {
        if (h < td::At<int>(s, 0xa18)) e = td::At<uint8_t**>(s, 0xa20)[h];
    } else if (-h < td::At<int>(s, 0x208)) {
        e = td::At<uint8_t**>(s, 0x210)[-h];
    }
    return e && td::EntityType(e) == 1 ? e : nullptr;
}

// Lua IsBodyActive reads the native sleep counter, which is 0 for every owned body: answer from PhysX.
void __fastcall HkIsBodyActive(void* ctx, void* args, void* ret) {
    if (g_offload) {
        if (uint8_t* b = BodyByHandle(td::Fn<LuaArgIntFn>(Addr::LuaArgInt)(args, 0, 0))) {
            int answer = -1;
            AcquireSRWLockShared(&g_lock);
            auto it = g_owned.find(b);
            if (it != g_owned.end()) answer = it->second->physxAwake ? 1 : 0;  // PhysX may be stepping now
            ReleaseSRWLockShared(&g_lock);
            if (answer >= 0) {
                td::Fn<LuaPushBoolFn>(Addr::LuaPushBool)(ret, answer != 0);
                return;
            }
        }
    }
    oIsBodyActive(ctx, args, ret);
}

bool Hook(Addr a, void* detour, void** original) {
    return td::CodeMatches(a) &&
           MH_CreateHook(reinterpret_cast<void*>(td::VA(a)), detour, original) == MH_OK;
}

}  // namespace

bool Install() {
    if (!g_config.pxShadow) return true;
    bool ok = Hook(Addr::BodyDtor, reinterpret_cast<void*>(&HkBodyDtor), reinterpret_cast<void**>(&oBodyDtor)) &&
              Hook(Addr::ShapeDtor, reinterpret_cast<void*>(&HkShapeDtor), reinterpret_cast<void**>(&oShapeDtor));
    // static shapes that appear while the level runs join the grid the next tick (else at its rebuild, every 600)
    const bool shapeEvents = ok && td::OnShapeCreated(&OnNewShape);
    LOG("shadow: PhysX mode %d, destructor hooks %s, new-shape events %s", g_config.pxShadow,
        ok ? "installed" : "FAILED", shapeEvents ? "on" : "off (new static shapes wait for the grid's rebuild)");
    if (!ok) {
        g_config.pxShadow = 0;
        return false;
    }
    // The game's impact event list: counted in every mode (what the game itself would play), joined by PhysX's
    // impacts with the offload.
    bool soundHook = td::CodeMatches(Addr::VoxPalette) && td::CodeMatches(Addr::EngineAlloc) &&
                     td::CodeMatches(Addr::EngineFree) &&
                     Hook(Addr::SoundListMerge, reinterpret_cast<void*>(&HkSoundListMerge),
                          reinterpret_cast<void**>(&oSoundListMerge));
    if (g_config.pxShadow == 3) {
        bool off = td::CodeMatches(Addr::BodyUpdateShapes) && td::CodeMatches(Addr::BodyWake) &&
                   td::CodeMatches(Addr::SolverAddBody) && td::CodeMatches(Addr::MutexLock) &&
                   td::CodeMatches(Addr::MutexUnlock) && td::CodeMatches(Addr::ShapeUpdateBounds) &&
                   td::CodeMatches(Addr::TreeUpdateShape) &&
                   Hook(Addr::NarrowWake, reinterpret_cast<void*>(&HkNarrowWake), reinterpret_cast<void**>(&oNarrowWake));
        // owned bodies are asleep natively (no solver index): the native constraint makers that skip the wake
        bool guards = off &&
                      Hook(Addr::PlayerConstraints, reinterpret_cast<void*>(&HkPlayerConstraints),
                           reinterpret_cast<void**>(&oPlayerConstraints)) &&
                      Hook(Addr::SolverIslands, reinterpret_cast<void*>(&HkSolverIslands),
                           reinterpret_cast<void**>(&oSolverIslands));
        LOG("offload: held-body wake and solver constraint guard %s", guards ? "installed" : "MISSING");
        bool lua = off && td::CodeMatches(Addr::LuaArgInt) && td::CodeMatches(Addr::LuaPushBool) &&
                   Hook(Addr::LuaIsBodyActive, reinterpret_cast<void*>(&HkIsBodyActive),
                        reinterpret_cast<void**>(&oIsBodyActive));
        g_waterOk = off && td::CodeMatches(Addr::ScenePointInWater);
        bool sounds = off && g_config.pxImpacts && soundHook;
        if (!sounds) g_config.pxImpacts = false;
        g_damageOk = off && g_config.pxDamage && td::CodeMatches(Addr::MakeHoleCore) &&
                     td::CodeMatches(Addr::VoxPalette);
        if (!g_damageOk) g_config.pxDamage = false;
        LOG("offload: impact sounds %s, impact damage %s", sounds ? "on" : "off", g_damageOk ? "on" : "off");
        LOG("offload: engine hooks %s, IsBodyActive %s, water test %s",
            off ? "installed" : "FAILED (free shadow mode instead)", lua ? "patched" : "unchanged",
            g_waterOk ? "found" : "missing (debris stays PhysX's in water)");
        if (!off) g_config.pxShadow = 2;
    }
    return true;
}

// PhysX failed (a CUDA error stops its GPU pipeline for good): the level goes on as if it had ended for the plugin.
// Every owned body goes back to the game, which simulates it from its last written state; the broken scene is
// abandoned, not released. The next tick starts a new scene, on the CPU.
void GiveUp() {
    LOG("offload: PhysX failed, handing %zu bodies back to the game; a new scene on the CPU takes over", g_owned.size());
    g_broken = true;
    g_stepPending = false;  // (Stop() would wait for it)
    px::AbandonWorld();
    Stop();
    g_broken = false;
}

// Diagnostics: every owned body must enter the native step asleep (the per-body pass zeroes only sleeping bodies'
// velocities; AfterNativeStep takes what an owned body has after the step for new impulses).
void CheckOwnedAsleep(bool skipTick) {
    static int logged = 0;
    if (logged >= 40 || g_tick % 120 != 0) return;  // (a look at every owned body: ~1.4 ms on Acacia, never found any)
    AcquireSRWLockShared(&g_lock);
    for (auto& [b, e] : g_owned)
        if (SleepCounter(b) && logged++ < 40)
            LOG("offload diag: tick %u%s, owned body %d enters the native step awake (counter %u, %.1f m/s, PhysX "
                "%s, pending %s, owned %u ticks)",
                g_tick, skipTick ? " (without PhysX)" : "", Handle(b), unsigned(SleepCounter(b)),
                V(BodyVel(b)).magnitude(), e->physxAwake ? "awake" : "asleep", e->pending ? "yes" : "no",
                g_tick - e->ownedSince);
    ReleaseSRWLockShared(&g_lock);
}

void OnPhysicsPre() {
    if (!g_config.pxShadow || td::State() != td::kPlay) return;
    if (!g_active && !Start()) {
        g_config.pxShadow = 0;
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    uint64_t t0 = trace::Now();
    g_preStart = t0;
    ++g_tick;
    g_tickBuildMs = 0.0;
    g_tickBodyBuildMs = 0.0;
    if (g_stepPending && g_offload && g_config.pxCatchUp &&
        g_pendingTicks + 2 <= uint32_t(std::max(1, g_config.pxCatchUpMax))) {
        // CatchUp: the step started at the end of the last tick isn't done (the game runs several ticks in this
        // frame). Rather than wait, this tick passes without PhysX and the next step covers it too. A step that is
        // about to finish is waited for.
        static const double qpcPerMs = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return double(f.QuadPart) / 1000.0;
        }();
        uint64_t until = t0 + uint64_t(std::max(0.0f, g_config.pxCatchUpWaitMs) * qpcPerMs);
        bool done = g_stepDone.doneAt.load(std::memory_order_acquire) != 0;
        while (!done && trace::Now() < until) {
            _mm_pause();
            done = g_stepDone.doneAt.load(std::memory_order_acquire) != 0;
        }
        if (!done) {
            g_skipTick = true;
            ++g_off.skippedTicks;
            CollectWhileStepping();
            uint64_t t1 = trace::Now();
            trace::Emit(trace::kPxPrepare, t0, t1, 0);
            g_win.prepareMs += Ms(t0, t1);
            g_preEnd = t1;
            ReleaseSRWLockExclusive(&g_lock);
            CheckOwnedAsleep(true);
            g_inStep.store(true, std::memory_order_release);
            return;
        }
    }
    if (g_stepPending) {
        // Async: the step started at the end of the last Physics::update ran during the rest of the frame. What
        // scripts and tools changed since is collected before its results overwrite the bodies, then added on top.
        px::Scene()->fetchResults(true);
        g_stepPending = false;
        if (px::Failed()) {
            ReleaseSRWLockExclusive(&g_lock);
            GiveUp();
            return;
        }
        AfterFetch();
        uint64_t f1 = trace::Now();
        uint64_t done = g_stepDone.doneAt.load(std::memory_order_acquire);
        if (done > g_kickAt) {
            double real = Ms(g_kickAt, done);
            g_win.realStepMs += real;
            g_win.realStepMaxMs = std::max(g_win.realStepMaxMs, real);
        }
        double wait = Ms(t0, f1);
        g_win.stepMs += wait;  // the part of the step the game waited for
        g_win.stepMaxMs = std::max(g_win.stepMaxMs, g_lastKickMs + wait);
        static std::vector<std::pair<uint8_t*, BodyEntry*>> changed;
        static std::vector<uint32_t> changedAt;
        static std::atomic<uint32_t> nchanged;
        const auto& owned = OwnedNow();
        changedAt.resize(owned.size());
        nchanged.store(0, std::memory_order_relaxed);
        px::ParallelFor(owned.size(), 256, [](size_t lo, size_t hi) {
            for (size_t k = lo; k < hi; ++k) {
                BodyEntry& e = *g_ownedNow[k].second;
                uint8_t* b = g_ownedNow[k].first;
                if (!e.physxAwake && !e.pending && !BodyAwake(b) && (k + g_tick) % kQuietRecheck != 0) {
                    e.fwdMoved = e.fwdPushed = e.fwdWoken = false;  // resting, untouched: nothing to forward
                    continue;
                }
                CollectForwardPure(b, e);
                if (e.fwdMoved || e.fwdPushed || e.fwdWoken)
                    changedAt[nchanged.fetch_add(1, std::memory_order_relaxed)] = uint32_t(k);
            }
        });
        changed.clear();  // the few bodies something changed: the serial passes below visit only those
        for (uint32_t i = 0, nc = nchanged.load(); i < nc; ++i) changed.push_back(owned[changedAt[i]]);
        for (auto& [b, e] : changed) CollectForwardFinish(b, *e);
        uint32_t written = WriteBackAll(&changed);
        for (auto& [b, e] : changed) ApplyForward(*e);
        ApplyPending();
        ReleaseDeferred();
        uint64_t f2 = trace::Now();
        trace::Emit(trace::kPxStep, t0, f1, 1);
        trace::Emit(trace::kPxWriteBack, f1, f2, written);
        g_win.writeMs += Ms(f1, f2);
        ++g_win.ticks;
        if (g_win.ticks >= uint64_t(std::max(1, g_config.pxLogEvery))) {
            Report(false);
            Accumulate();
        }
        t0 = f2;
    }
    {
        PhaseScope ps(kPhRefresh);
        if (g_gridDirty || g_tick - g_gridTick > 600) BuildGrid();
        UpdateStatics();
    }
    if (g_tick <= 3) LOG("shadow: tick %u: scanning", g_tick);
    g_deadSinceKick.clear();  // the scan rebuilds the colliders of bodies whose shapes changed
    if (g_offload)
        ScanOffload();
    else
        Scan();
    if (g_tick <= 3) LOG("shadow: tick %u: %zu bodies tracked, %zu static shapes", g_tick, g_bodies.size(), g_static.size());
    uint64_t t1 = trace::Now();
    trace::Emit(trace::kPxPrepare, t0, t1, g_bodies.size());
    g_win.prepareMs += Ms(t0, t1);
    g_preEnd = t1;
    ReleaseSRWLockExclusive(&g_lock);
    if (g_offload) {
        CarveImpacts();
        CheckOwnedAsleep(false);
        g_inStep.store(true, std::memory_order_release);
    }
}

void OnPhysicsPost() {
    if (!g_config.pxShadow || !g_active) return;
    g_inStep.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_lock);
    uint64_t t0 = trace::Now();
    double native = Ms(g_preEnd, t0);
    if (g_skipTick) {  // CatchUp: a tick without PhysX, whose step still runs: the native step's side only
        g_skipTick = false;
        AfterNativeStep(true);
        ++g_pendingTicks;
        uint64_t t1 = trace::Now();
        trace::Emit(trace::kPxSync, t0, t1, 0);
        g_win.nativeMs += native;
        g_win.nativeMaxMs = std::max(g_win.nativeMaxMs, native);
        g_win.syncMs += Ms(t0, t1);
        ++g_win.ticks;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (g_offload) AfterNativeStep(false);
    g_stepSpan = std::clamp<uint32_t>(g_pendingTicks + 1, 1, uint32_t(std::max(1, g_config.pxCatchUpMax)));
    Sync();
    if (g_offload) {
        UpdatePlayer();
        WakeAroundChanges();
        ThawAroundMovers();
    }
    g_changed.clear();
    uint64_t t1 = trace::Now();
    PxScene* scene = px::Scene();
    if (px::Failed()) {
        ReleaseSRWLockExclusive(&g_lock);
        GiveUp();
        return;
    }
    if (g_tick <= 3) LOG("shadow: tick %u: stepping", g_tick);
    g_stepDone.doneAt.store(0, std::memory_order_relaxed);
    g_kickAt = t1;
    scene->simulate(kDt * float(g_stepSpan), &g_stepDone);  // CatchUp: also the ticks that passed without PhysX
    g_pendingTicks = 0;
    if (g_async) {
        // runs on PhysX's threads during the rest of the frame; fetched and written back at the next tick's start
        g_stepPending = true;
        uint64_t t2 = trace::Now();
        g_lastKickMs = Ms(t1, t2);
        trace::Emit(trace::kPxSync, t0, t1, 0);
        g_win.nativeMs += native;
        g_win.nativeMaxMs = std::max(g_win.nativeMaxMs, native);
        g_win.syncMs += Ms(t0, t1);
        g_win.stepMs += g_lastKickMs;
        g_win.totalMaxMs = std::max(g_win.totalMaxMs, Ms(g_preStart, t2));
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    scene->fetchResults(true);
    if (px::Failed()) {
        ReleaseSRWLockExclusive(&g_lock);
        GiveUp();
        return;
    }
    AfterFetch();
    if (g_tick <= 3) LOG("shadow: tick %u: stepped", g_tick);
    uint64_t t2 = trace::Now();
    PxU32 active = 0;
    scene->getActiveActors(active);
    g_lastActive = active;
    if (g_config.pxShadow == 1) Compare();
    uint64_t t3 = trace::Now();
    uint32_t written = g_offload ? WriteBackAll() : 0;
    uint64_t t4 = trace::Now();
    trace::Emit(trace::kPxSync, t0, t1, 0);
    trace::Emit(trace::kPxStep, t1, t2, active);
    if (g_config.pxShadow == 1) trace::Emit(trace::kPxCompare, t2, t3, 0);
    if (g_offload) trace::Emit(trace::kPxWriteBack, t3, t4, written);
    ++g_win.ticks;
    g_win.nativeMs += native;
    g_win.nativeMaxMs = std::max(g_win.nativeMaxMs, native);
    g_win.totalMaxMs = std::max(g_win.totalMaxMs, Ms(g_preStart, t4));
    g_win.syncMs += Ms(t0, t1);
    double step = Ms(t1, t2);
    g_win.stepMs += step;
    g_win.stepMaxMs = std::max(g_win.stepMaxMs, step);
    g_win.realStepMs += step;
    g_win.realStepMaxMs = std::max(g_win.realStepMaxMs, step);
    g_win.compareMs += Ms(t2, t3);
    g_win.writeMs += Ms(t3, t4);
    if (g_offload && step > 30.0 && g_diagSpikes++ < 20) {
        PxSimulationStatistics st;
        scene->getSimulationStatistics(st);
        LOG("offload diag: tick %u, PhysX step %.1f ms: %u active, contact pairs %u (%u touching), new/lost touches "
            "%u/%u; fastest owned body this window %d at %.1f m/s (mass %.3g)",
            g_tick, step, active, st.nbDiscreteContactPairsTotal, st.nbDiscreteContactPairsWithContacts,
            st.nbNewTouches, st.nbLostTouches, g_maxSpeedHandle, g_maxSpeed, g_maxSpeedMass);
    }
    if (g_win.ticks >= uint64_t(std::max(1, g_config.pxLogEvery))) {
        Report(false);
        Accumulate();
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void OnLeavePlay() {
    AcquireSRWLockExclusive(&g_lock);
    Stop();
    ReleaseSRWLockExclusive(&g_lock);
}

void OnShapesModified(const uint8_t* entries, int count) {
    if (!g_active || !entries || count <= 0 || count > 100000) return;
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < count; ++i)
        if (uint8_t* s = *reinterpret_cast<uint8_t* const*>(entries + size_t(i) * 200)) {
            g_modified.push_back(s);
            if (uint8_t* b = ShapeBody(s)) {  // a quiet body gets its full look next tick (fire burns without waking)
                auto bit = g_bodies.find(b);
                if (bit != g_bodies.end()) bit->second.dirty |= 1;
            }
        }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace shadow
