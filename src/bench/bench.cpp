#include "bench/bench.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "fix/touch_cache.h"
#include "input/recorder.h"
#include "px/shadow.h"
#include "td/addresses.h"
#include "td/game.h"
#include "trace/trace.h"

using td::Addr;

namespace bench {

namespace {

enum class Phase { Off, WaitMenu, Loading, Warmup, Run, Recording, Replaying, Done };
enum Marker : uint64_t { kLevelRequested = 4, kPlayReached = 1, kRunStart = 2, kRunEnd = 3, kFailed = 9 };

Phase g_phase = Phase::Off;
uint64_t g_splashQpc = 0, g_menuQpc = 0;
bool g_splashSkipped = false;
uint64_t g_initQpc = 0;
int g_warmTicks = 0;
double g_runTime = 0.0;
double g_nextLog = 5.0;
double g_holeAcc = 0.0;
int g_holes = 0, g_hits = 0, g_misses = 0;
long long g_removed = 0;
double g_holeMsSum = 0.0, g_holeMsMax = 0.0;
uint64_t g_rng = 0;

double Seconds(uint64_t a, uint64_t b) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(b - a) / double(f.QuadPart);
}

void Mark(uint64_t id) {
    uint64_t now = trace::Now();
    trace::Emit(trace::kBenchMarker, now, now, id);
}

double Rand01() {  // xorshift64*
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return double((g_rng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
}

// ---- small vector math ----------------------------------------------------------------------------------
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Norm(V3 a) {
    float l = std::sqrt(Dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}
V3 Rotate(const float q[4], V3 v) {  // q = (x, y, z, w)
    V3 u{q[0], q[1], q[2]};
    V3 t = Cross(u, v) * 2.0f;
    return v + t * q[3] + Cross(u, t);
}
V3 RotateInv(const float q[4], V3 v) {
    float c[4] = {-q[0], -q[1], -q[2], q[3]};
    return Rotate(c, v);
}
V3 RotateAxis(V3 v, V3 axis, float a) {  // Rodrigues
    float c = std::cos(a), s = std::sin(a);
    return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1 - c));
}

// ---- ray vs voxel shapes --------------------------------------------------------------------------------
bool Slab(V3 o, V3 d, const float* mn, const float* mx, float& t0, float& t1) {
    t0 = 0.0f;
    t1 = 1e30f;
    const float* oo = &o.x;
    const float* dd = &d.x;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dd[i]) < 1e-9f) {
            if (oo[i] < mn[i] || oo[i] > mx[i]) return false;
            continue;
        }
        float inv = 1.0f / dd[i];
        float a = (mn[i] - oo[i]) * inv, b = (mx[i] - oo[i]) * inv;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

using ShapeWorldTransformFn = td::Transform*(__fastcall*)(void* shape, td::Transform* out);

// First solid voxel of one shape along the ray within [ta, tb] (world metres). Shape origin = voxel (0,0,0)
// corner, as the renderer's raymarcher assumes.
bool MarchShape(uint8_t* shape, V3 o, V3 d, float ta, float tb, float& tHit) {
    uint8_t* vox = td::ShapeVoxels(shape);
    const int* dim = td::VoxDims(vox);
    const uint8_t* data = td::VoxData(vox);
    float size = td::VoxSize(vox);
    if (!data || size <= 0.0f || dim[0] <= 0 || dim[1] <= 0 || dim[2] <= 0) return false;
    td::Transform tr;
    td::Fn<ShapeWorldTransformFn>(Addr::ShapeWorldTransform)(shape, &tr);
    V3 lo = RotateInv(tr.rot, o - V3{tr.pos[0], tr.pos[1], tr.pos[2]}) * (1.0f / size);
    V3 ld = RotateInv(tr.rot, d) * (1.0f / size);
    float t = ta + 1e-4f;
    V3 p = lo + ld * t;
    int idx[3] = {int(std::floor(p.x)), int(std::floor(p.y)), int(std::floor(p.z))};
    const float* lp = &p.x;
    const float* ldd = &ld.x;
    int step[3];
    float tMax[3], tDelta[3];
    for (int i = 0; i < 3; ++i) {
        if (idx[i] < 0) idx[i] = 0;
        if (idx[i] >= dim[i]) idx[i] = dim[i] - 1;
        if (ldd[i] > 0) {
            step[i] = 1;
            tDelta[i] = 1.0f / ldd[i];
            tMax[i] = t + (float(idx[i] + 1) - lp[i]) / ldd[i];
        } else if (ldd[i] < 0) {
            step[i] = -1;
            tDelta[i] = -1.0f / ldd[i];
            tMax[i] = t + (float(idx[i]) - lp[i]) / ldd[i];
        } else {
            step[i] = 0;
            tDelta[i] = 1e30f;
            tMax[i] = 1e30f;
        }
    }
    int guard = dim[0] + dim[1] + dim[2] + 4;
    while (guard-- > 0 && t <= tb) {
        size_t i = size_t(idx[0]) + size_t(dim[0]) * (size_t(idx[1]) + size_t(dim[1]) * size_t(idx[2]));
        if (data[i] != 0) {
            tHit = t;
            return true;
        }
        int ax = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
        t = tMax[ax];
        tMax[ax] += tDelta[ax];
        idx[ax] += step[ax];
        if (idx[ax] < 0 || idx[ax] >= dim[ax]) return false;
    }
    return false;
}

// staticOnly: shapes of dynamic bodies (debris, props) are looked through. hitShape: the shape hit, if wanted.
bool Raycast(V3 o, V3 d, float maxDist, float& tHit, bool staticOnly = false, uint8_t** hitShape = nullptr) {
    int n = 0;
    uint8_t** shapes = td::SceneList<uint8_t*>(0x238, n);
    if (!shapes) return false;
    struct Cand {
        float t0, t1;
        uint8_t* s;
    };
    std::vector<Cand> cands;
    for (int i = 0; i < n; ++i) {
        uint8_t* s = shapes[i];
        if (!s || td::EntityType(s) != 2 || !(td::ShapeFlags(s) & 0x10) || !td::ShapeVoxels(s)) continue;
        if (staticOnly) {
            uint8_t* b = td::ShapeBody(s);
            if (!b || td::BodyDynamic(b)) continue;
        }
        float t0, t1;
        if (!Slab(o, d, td::ShapeAabbMin(s), td::ShapeAabbMax(s), t0, t1) || t0 > maxDist) continue;
        cands.push_back({t0, std::min(t1, maxDist), s});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.t0 < b.t0; });
    float best = maxDist;
    bool hit = false;
    for (const Cand& c : cands) {
        if (c.t0 > best) break;
        float t;
        if (MarchShape(c.s, o, d, c.t0, std::min(c.t1, best), t) && t < best) {
            best = t;
            hit = true;
            if (hitShape) *hitShape = c.s;
        }
    }
    tHit = best;
    return hit;
}

// ---- engine calls ---------------------------------------------------------------------------------------
using PlayerSetTransformFn = void(__fastcall*)(void* player, const td::Transform* feet, bool withPitch);
using LevelDescCtorFn = void*(__fastcall*)(void* desc);
using TdStringFromCStrFn = void*(__fastcall*)(void* dst, const char* s);
using TdStringCopyFn = void*(__fastcall*)(void* dst, const void* src);
using TdStringDtorFn = void(__fastcall*)(void* s);
using PathResolveFn = void*(__fastcall*)(void* out, const void* path, const void* folder);
using StartLevelFn = void(__fastcall*)(void* game, void* desc, bool passThrough);
using RequestStateFn = void(__fastcall*)(void* game, int state, int a, int b);
using PaintSceneFn = void(__fastcall*)(void* scene, const float* pos, float radius, int explosion, float strength,
                                       int* color);
using BodyWakeFn = void(__fastcall*)(void* body, char force);

V3 g_lastHit{0, 0, 0};
uint64_t g_lastHitQpc = 0;
long long g_painted = 0, g_pushed = 0;

// What the Gluon Plasma Diffuser does to nearby bodies after each hit (see bench.h / config.h). The mod runs this
// from tick(), i.e. once per rendered frame: at high frame rates a hit pushes the same bodies many times.
void LaserEffects() {
    if (!g_config.laserEffects || !g_lastHitQpc) return;
    double since = Seconds(g_lastHitQpc, trace::Now());
    if (since > g_config.effectSeconds) return;
    bool push = since < 0.05;
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    uint8_t* scene = td::SceneObj();
    if (!bodies || !scene) return;
    auto paint = td::Fn<PaintSceneFn>(Addr::PaintScene);
    auto wake = td::Fn<BodyWakeFn>(Addr::BodyWake);
    const float R = float(g_config.effectRadius);
    for (int i = 0; i < n; ++i) {
        uint8_t* b = bodies[i];
        if (!b || td::EntityType(b) != 1 || !td::At<uint8_t>(b, 0x138)) continue;
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
        for (uint8_t* s = td::At<uint8_t*>(b, 0x28); s; s = td::At<uint8_t*>(s, 0x18)) {
            if (td::EntityType(s) != 2) continue;
            const float* a0 = td::ShapeAabbMin(s);
            const float* a1 = td::ShapeAabbMax(s);
            for (int k = 0; k < 3; ++k) {
                mn[k] = std::min(mn[k], a0[k]);
                mx[k] = std::max(mx[k], a1[k]);
            }
            any = true;
        }
        if (!any) continue;
        V3 c{(mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f};
        V3 d = c - g_lastHit;
        float dist = std::sqrt(Dot(d, d));
        if (dist >= R) continue;
        float distScale = 1.0f - std::min(dist / R, 0.9f);
        V3 dir = dist > 1e-4f ? d * (1.0f / dist) : V3{0, 1, 0};
        float inv = td::At<float>(b, 0xfc);
        double mass = inv > 0 ? 1.0 / inv : 1e30;
        if (push) {
            float sp = float((mass < g_config.lightMass ? g_config.pushLight : g_config.pushHeavy) * distScale);
            float* v = &td::At<float>(b, 0xd8);
            v[0] += dir.x * sp;
            v[1] += dir.y * sp;
            v[2] += dir.z * sp;
            if (mass < g_config.lightMass) {
                float* w = &td::At<float>(b, 0xe4);
                float spin = float(g_config.spinLight * distScale);
                w[0] += dir.x * spin;
                w[1] += dir.y * spin;
                w[2] += dir.z * spin;
            }
            wake(b, 0);
            ++g_pushed;
        }
        const float* bp = &td::At<float>(b, 0x40);
        const float* bq = &td::At<float>(b, 0x4c);
        V3 com = Rotate(bq, V3{td::At<float>(b, 0x100), td::At<float>(b, 0x104), td::At<float>(b, 0x108)}) +
                 V3{bp[0], bp[1], bp[2]};
        float pos[3] = {com.x, com.y, com.z};
        int color = -1;
        paint(scene, pos, float(g_config.paintMin + (g_config.paintMax - g_config.paintMin) * Rand01()), 1, 1.0f,
              &color);
        ++g_painted;
    }
}
using MakeHoleFn = void(__fastcall*)(void* world, void* shape, const float* pos, float r0, float r1, float r2,
                                     bool sound, const void* impulse);

alignas(16) uint8_t g_desc[0x200];  // kept alive: the engine copies it, but never free what it may reference

using ModByIdFn = uint8_t*(__fastcall*)(void* modManager, const void* id);

const char* CStr(const uint8_t* s) {  // the engine's 32-byte SSO string
    return s[31] ? *reinterpret_cast<const char* const*>(s) : reinterpret_cast<const char*>(s);
}

// Returns false while a requested mod is not listed yet (Steam lists workshop items a little after start).
bool StartLevelNative() {
    uint8_t* game = td::GameObj();
    auto fromC = td::Fn<TdStringFromCStrFn>(Addr::TdStringFromCStr);
    auto copy = td::Fn<TdStringCopyFn>(Addr::TdStringCopy);
    auto dtor = td::Fn<TdStringDtorFn>(Addr::TdStringDtor);
    alignas(16) uint8_t tmp[32] = {}, folder[32] = {}, path[32] = {};
    std::string modPath;
    if (!g_config.mod.empty()) {
        // What Command("mods.play", ...) does: look the mod up by id, path = <mod folder>/<file>, mod id in +0x80.
        fromC(tmp, g_config.mod.c_str());
        uint8_t* modObj = td::Fn<ModByIdFn>(Addr::ModById)(td::At<void*>(game, 0x108), tmp);
        dtor(tmp);
        if (!modObj || !td::At<uint8_t>(modObj, 0x3bd)) return false;
        modPath = std::string(CStr(modObj + 0x300)) + "/" + g_config.levelFile;
    }
    td::Fn<LevelDescCtorFn>(Addr::LevelDescCtor)(g_desc);
    fromC(tmp, g_config.mission.c_str());
    copy(g_desc + 0x20, tmp);
    dtor(tmp);
    if (!modPath.empty()) {
        fromC(path, modPath.c_str());
        fromC(tmp, g_config.mod.c_str());
        copy(g_desc + 0x80, tmp);
        dtor(tmp);
    } else {
        fromC(tmp, g_config.levelFile.c_str());
        fromC(folder, "level");
        td::Fn<PathResolveFn>(Addr::PathResolveLevel)(path, tmp, folder);
        dtor(tmp);
        dtor(folder);
    }
    copy(g_desc + 0x40, path);
    LOG("bench: starting level mission=%s path=%s layers=%s mod=%s", g_config.mission.c_str(), CStr(path),
        g_config.layers.c_str(), g_config.mod.c_str());
    dtor(path);
    fromC(tmp, g_config.layers.c_str());
    copy(g_desc + 0x60, tmp);
    dtor(tmp);
    copy(g_desc + 0xa0, game + 0x470);
    td::At<uint8_t>(game, 0x828) = 0;
    td::At<uint8_t>(game, 0x8ec) = 0;
    inputrec::OnLevelStart();
    td::Fn<StartLevelFn>(Addr::StartLevel)(game, g_desc, false);
    Mark(kLevelRequested);
    return true;
}

void SkipSplash() {
    // What Lua Menu() does (0x1404b6530): clear Game+0x828, request MENU_LOADING.
    uint8_t* game = td::GameObj();
    td::At<uint8_t>(game, 0x828) = 0;
    td::Fn<RequestStateFn>(Addr::RequestState)(game, td::kMenuLoading, 0, 0);
    LOG("bench: splash skipped (requested MENU_LOADING)");
}

void Finish(bool ok) {
    if (g_phase == Phase::Done) return;
    g_phase = Phase::Done;
    LOG("bench: %s. holes=%d hits=%d misses=%d removed=%lld voxels, MakeHole avg %.2f ms max %.2f ms, "
        "bodies pushed %lld, painted %lld",
        ok ? "done" : "FAILED", g_holes, g_hits, g_misses, g_removed, g_hits ? g_holeMsSum / g_hits : 0.0,
        g_holeMsMax, g_pushed, g_painted);
    Mark(ok ? kRunEnd : kFailed);
    if (g_config.quitWhenDone) Quit(ok);
}

void DoHole() {
    ++g_holes;
    td::Transform cam;
    if (!td::CameraTransform(cam)) return;
    V3 fwd = Norm(Rotate(cam.rot, V3{0, 0, -1}));
    V3 up{0, 1, 0};
    const double kPi = 3.14159265358979;
    double ph = 2 * kPi * g_runTime / g_config.sweepPeriodSeconds;
    // [Bench] AimYawDeg (positive: right) / AimPitchDeg (positive: up) turn the sweep's centre from the spawn view
    float yaw = float((g_config.sweepYawDeg * std::sin(ph) - g_config.aimYawDeg) * kPi / 180 + (Rand01() - 0.5) * 0.05);
    float pitch = float((g_config.aimPitchDeg + g_config.sweepPitchDeg * std::sin(ph * 1.63 + 0.7)) * kPi / 180 +
                        (Rand01() - 0.5) * 0.05);
    V3 turned = RotateAxis(fwd, up, yaw);
    V3 dir = Norm(RotateAxis(turned, Norm(Cross(turned, up)), pitch));
    V3 o{cam.pos[0], cam.pos[1], cam.pos[2]};
    float t;
    if (!Raycast(o, dir, float(g_config.maxRayDistance), t)) {
        ++g_misses;
        uint64_t now = trace::Now();
        trace::Emit(trace::kBenchHole, now, now, 0);
        return;
    }
    V3 p = o + dir * (t + 0.05f);
    float pos[3] = {p.x, p.y, p.z};
    uint8_t* scene = td::SceneObj();
    void* world = td::At<void*>(scene, 0x10);
    int before = td::DestroyedVoxels();
    uint64_t t0 = trace::Now();
    td::Fn<MakeHoleFn>(Addr::MakeHole)(world, nullptr, pos, g_config.r0, g_config.r1, g_config.r2, g_config.sound,
                                         reinterpret_cast<const void*>(td::VA(Addr::NoImpulseDesc)));
    uint64_t t1 = trace::Now();
    int removed = td::DestroyedVoxels() - before;
    ++g_hits;
    g_removed += removed;
    g_lastHit = p;
    g_lastHitQpc = t1;
    double ms = Seconds(t0, t1) * 1000.0;
    g_holeMsSum += ms;
    g_holeMsMax = std::max(g_holeMsMax, ms);
    trace::Emit(trace::kBenchHole, t0, t1, uint32_t(removed) | (1ull << 32));
}

bool LoadTimedOut() {
    return Seconds(g_initQpc, trace::Now()) > g_config.loadTimeoutSeconds;
}

// ---- the debris tool ([Tool]) -------------------------------------------------------------------------------
// While its key is held, every tick carves HolesPerTick holes at random points within Spread metres of where the
// camera aims: no particles, paint or pushes, just voxels removed and the pieces that fall. The trigger follows the
// input messages the game receives (the recorder passes them on, live and when replaying), and the holes come from
// the tool's own generator, seeded at level start: a replay carves the same holes.
using MakeHoleCoreFn = void(__fastcall*)(void* world, void* body, void* spheres, uint8_t sound, const void* impulse);
std::atomic<bool> g_toolDown{false};
uint64_t g_toolRng = 1;
uint64_t g_toolHoles = 0, g_toolTicks = 0, g_toolLogged = 0;

#pragma pack(push, 1)
struct HoleSphere {
    float pos[3];
    float soft, medium, hard;  // radius per material class
};
#pragma pack(pop)
struct HoleList {
    int count, cap;
    HoleSphere* data;
};

// The engine carves every hole of a MakeHoleCore call into every voxel shape inside the bounding box of all of them
// (about 1 us per hole and shape): one call with thousands of holes spread over metres costs seconds, while small
// batches of neighbouring holes see only the shapes around them. Fragment cuts wait here in such batches, and each
// tick carves batches until CarveMsPerTick is spent; the pieces come loose once their last cut is made.
// Each batch carves only the static body the tool aimed at: pieces it cut loose (now debris bodies) are never cut
// again by the cubes that follow.
struct CarveBatch {
    uint8_t* body;  // static body to carve (MakeHoleCore's body filter)
    std::vector<HoleSphere> holes;
};
std::deque<CarveBatch> g_carveQueue;
size_t g_carveWaiting = 0;  // holes in the queue
uint64_t g_carveCalls = 0, g_carveSkipped = 0;
double g_carveMs = 0.0, g_carveMaxTickMs = 0.0;
// The budget is per frame: after a long frame the game runs two ticks in the next one, and a per-tick budget would
// carve twice as long there, making that frame long too.
double g_frameCarveMs = 0.0;

void CarveNow(std::vector<HoleSphere>& holes, uint8_t* body, bool sound) {
    uint8_t* scene = td::SceneObj();
    if (!scene || holes.empty()) return;
    HoleList list{int(holes.size()), int(holes.size()), holes.data()};
    td::Fn<MakeHoleCoreFn>(Addr::MakeHoleCore)(td::At<void*>(scene, 0x10), body, &list, sound ? 1 : 0,
                                               reinterpret_cast<const void*>(td::VA(Addr::NoImpulseDesc)));
    g_toolHoles += holes.size();
    ++g_carveCalls;
}

// The body still exists and is still static (it may have been destroyed since the batch was queued).
bool StaticBodyAlive(uint8_t* body) {
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    for (int i = 0; bodies && i < n; ++i)
        if (bodies[i] == body) return !td::BodyDynamic(body);
    return false;
}

void DrainCarves() {
    if (g_carveQueue.empty()) return;
    const uint64_t t0 = trace::Now();
    const double budget = std::max(0.0, double(g_config.toolCarveMsPerTick) - g_frameCarveMs);
    if (budget <= 0.0) return;  // this frame's carving is done: the next frame goes on
    double ms = 0.0;
    uint8_t* checked = nullptr;
    bool alive = false;
    do {  // at least one batch per tick
        CarveBatch& b = g_carveQueue.front();
        g_carveWaiting -= b.holes.size();
        if (b.body != checked) {
            checked = b.body;
            alive = StaticBodyAlive(b.body);
        }
        if (alive)
            CarveNow(b.holes, b.body, false);
        else
            ++g_carveSkipped;
        g_carveQueue.pop_front();
        ms = Seconds(t0, trace::Now()) * 1000.0;
    } while (!g_carveQueue.empty() && ms < budget);
    g_frameCarveMs += ms;
    g_carveMs += ms;
    g_carveMaxTickMs = std::max(g_carveMaxTickMs, ms);
}

int ToolVk() {
    const std::string& k = g_config.toolKey;
    if (k == "mmb") return VK_MBUTTON;
    if (k == "xbutton1") return VK_XBUTTON1;
    if (k == "xbutton2") return VK_XBUTTON2;
    if (k.size() == 1 && std::isalnum(static_cast<unsigned char>(k[0]))) return std::toupper(static_cast<unsigned char>(k[0]));
    return VK_MBUTTON;
}

void ObserveInput(unsigned msg, unsigned long long wp, long long) {
    const int vk = ToolVk();
    switch (msg) {
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            if (vk == VK_MBUTTON) g_toolDown = msg == WM_MBUTTONDOWN;
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            if ((vk == VK_XBUTTON1 && HIWORD(wp) == XBUTTON1) || (vk == VK_XBUTTON2 && HIWORD(wp) == XBUTTON2))
                g_toolDown = msg == WM_XBUTTONDOWN;
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (int(wp) == vk) g_toolDown = true;
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (int(wp) == vk) g_toolDown = false;
            break;
        default:
            break;
    }
}

bool ToolTrigger() {
    if (inputrec::Driving()) return g_toolDown.load();
    // live play without the recorder: the key itself, while the game has the focus
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(ToolVk()) & 0x8000);
}

double ToolRand() {
    g_toolRng ^= g_toolRng >> 12;
    g_toolRng ^= g_toolRng << 25;
    g_toolRng ^= g_toolRng >> 27;
    return double((g_toolRng * 0x2545F4914F6CDD1Dull) >> 11) * (1.0 / 9007199254740992.0);
}

// Where the camera aims; firing by itself, turned by AimYawDeg/AimPitchDeg (a target beside or above the spawn
// view) and swept across it like the synthetic laser.
V3 Aim(const td::Transform& cam, bool sweep) {
    V3 fwd = Norm(Rotate(cam.rot, V3{0, 0, -1}));
    if (!sweep) return fwd;
    V3 up{0, 1, 0};
    const double kPi = 3.14159265358979;
    double ph = 2 * kPi * g_runTime / g_config.sweepPeriodSeconds;
    // a positive angle about +y turns left: AimYawDeg is positive to the right
    float yaw = float((g_config.sweepYawDeg * std::sin(ph) - g_config.toolAimYawDeg) * kPi / 180);
    float pitch = float((g_config.toolAimPitchDeg + g_config.sweepPitchDeg * std::sin(ph * 1.63 + 0.7)) * kPi / 180);
    V3 turned = RotateAxis(fwd, up, yaw);
    return Norm(RotateAxis(turned, Norm(Cross(turned, up)), pitch));  // pitch about the turned view's right axis
}

// [Bench] TurnYawDeg (right) / TurnPitchDeg (up): turn the player's view once, early in the warm-up, so the camera
// faces what the benchmark shoots at (de_vertigo's spawn looks away from the skyscraper). Player::setTransform is
// what Lua SetPlayerTransform calls: given the player's own feet position it only turns the view. [Bench] View
// moves the player too, so that the camera ends up at the given place.
void TurnView() {
    if (!g_config.viewSet && g_config.turnYawDeg == 0.0 && g_config.turnPitchDeg == 0.0) return;
    uint8_t* pl = td::LocalPlayer();
    td::Transform cam;
    if (!pl || !td::CameraTransform(cam) || !td::CodeMatches(Addr::PlayerSetTransform)) {
        LOG("bench: could not turn the view");
        return;
    }
    const double kPi = 3.14159265358979;
    V3 up{0, 1, 0};
    V3 d;
    if (g_config.viewSet) {
        d = Norm(V3{g_config.view[3], g_config.view[4], g_config.view[5]});
    } else {
        V3 fwd = Norm(Rotate(cam.rot, V3{0, 0, -1}));
        V3 turned = RotateAxis(fwd, up, float(-g_config.turnYawDeg * kPi / 180));  // positive angles turn left
        d = Norm(RotateAxis(turned, Norm(Cross(turned, up)), float(g_config.turnPitchDeg * kPi / 180)));
    }
    // a rotation whose -z is d and whose y points up as far as it can
    V3 back = d * -1.0f;
    V3 right = Norm(Cross(up, back));
    V3 up2 = Cross(back, right);
    const float m00 = right.x, m01 = up2.x, m02 = back.x;
    const float m10 = right.y, m11 = up2.y, m12 = back.y;
    const float m20 = right.z, m21 = up2.z, m22 = back.z;
    float q[4];  // x, y, z, w
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
    td::Transform feet = td::At<td::Transform>(pl, 0x570);
    for (int i = 0; i < 4; ++i) feet.rot[i] = q[i];
    if (g_config.viewSet)  // the camera keeps its height above the feet
        for (int i = 0; i < 3; ++i) feet.pos[i] += g_config.view[i] - cam.pos[i];
    td::Fn<PlayerSetTransformFn>(Addr::PlayerSetTransform)(pl, &feet, true);
    if (g_config.viewSet)
        LOG("bench: camera placed at (%.2f %.2f %.2f) looking along (%.3f %.3f %.3f)", g_config.view[0],
            g_config.view[1], g_config.view[2], d.x, d.y, d.z);
    else
        LOG("bench: view turned %.0f deg right and %.0f deg up", g_config.turnYawDeg, g_config.turnPitchDeg);
}

void ToolTick() {
    DrainCarves();  // also after the key is released
    const bool autoFire = g_config.toolAutoFire && g_phase == Phase::Run;
    static bool wasFiring = false, burstLogged = false;
    const bool firing = autoFire ? g_runTime < g_config.toolAutoFireSeconds : ToolTrigger();
    if (firing && !wasFiring) burstLogged = false;
    wasFiring = firing;
    if (!firing) return;
    if (++g_toolTicks - g_toolLogged >= 300) {  // every 5 s of firing
        g_toolLogged = g_toolTicks;
        LOG("tool: %llu holes in %llu ticks of firing (%llu engine calls, %.1f ms carving, at most %.1f ms in a "
            "tick, %zu holes waiting, %llu batches dropped with their body); bodies %d, simulated %d",
            g_toolHoles, g_toolTicks, g_carveCalls, g_carveMs, g_carveMaxTickMs, g_carveWaiting, g_carveSkipped,
            td::BodyCount(), td::SimulatedBodies());
        g_carveMaxTickMs = 0.0;
    }
    td::Transform cam;
    if (!td::CameraTransform(cam)) return;
    V3 o{cam.pos[0], cam.pos[1], cam.pos[2]};
    V3 fwd = Aim(cam, autoFire);
    float t;
    uint8_t* hitShape = nullptr;
    // aim at static geometry only: falling or fallen debris in front of it is looked through
    if (!Raycast(o, fwd, g_config.toolRange, t, true, &hitShape) || !hitShape) return;
    uint8_t* hitBody = td::ShapeBody(hitShape);
    V3 hit = o + fwd * (t + 0.05f);
    if (!burstLogged) {  // where you cut: a benchmark can place its camera there
        burstLogged = true;
        LOG("tool: firing from camera (%.2f %.2f %.2f) along (%.3f %.3f %.3f), hit at %.1f m (%.2f %.2f %.2f); bodies %d",
            o.x, o.y, o.z, fwd.x, fwd.y, fwd.z, t, hit.x, hit.y, hit.z, td::BodyCount());
    }
    static std::vector<HoleSphere> spheres;
    spheres.clear();
    if (g_config.toolMode == "fragment") {
        // Cut planes through a cube just behind the surface: holes 0.2 m apart with radius 0.18 m overlap into
        // a cut at least two voxels thick, so every PieceSize cube between the planes comes loose. Each plane goes
        // into the queue as tiles of CarveBatch x CarveBatch holes.
        static uint64_t ticks = 0;
        if (ticks++ % uint64_t(std::max(1, g_config.toolFragmentTicks))) return;
        const float size = std::clamp(g_config.toolFragmentSize, 0.5f, 8.0f);
        const float piece = std::clamp(g_config.toolPieceSize, 0.3f, size);
        const float step = 0.2f, r = 0.18f;
        const int planes = int(size / piece) + 1, along = int(size / step) + 1;
        const int tile = std::clamp(g_config.toolCarveBatch, 1, 64);
        // the tool waits for its cuts rather than queueing more than two cubes
        if (g_carveWaiting > size_t(2 * 3 * planes * along * along)) return;
        V3 c = hit + fwd * (size * 0.5f);
        V3 lo = c - V3{size * 0.5f, size * 0.5f, size * 0.5f};
        for (int axis = 0; axis < 3; ++axis)
            for (int pl = 0; pl < planes; ++pl)
                for (int ti = 0; ti < along; ti += tile)
                    for (int tj = 0; tj < along; tj += tile) {
                        CarveBatch batch{hitBody, {}};
                        for (int i = ti; i < std::min(along, ti + tile); ++i)
                            for (int j = tj; j < std::min(along, tj + tile); ++j) {
                                float q[3];
                                q[axis] = float(pl) * piece;
                                q[(axis + 1) % 3] = float(i) * step;
                                q[(axis + 2) % 3] = float(j) * step;
                                V3 p = lo + V3{q[0], q[1], q[2]};
                                batch.holes.push_back({{p.x, p.y, p.z}, r, r, r});
                            }
                        g_carveWaiting += batch.holes.size();
                        g_carveQueue.push_back(std::move(batch));
                    }
        DrainCarves();
        return;
    } else {
        // Spray: holes at random points of a ball reaching Spread metres into the surface, not into the air
        // in front of it.
        const int n = std::clamp(g_config.toolHolesPerTick, 1, 64);
        V3 c = hit + fwd * (g_config.toolSpread * 0.5f);
        for (int k = 0; k < n; ++k) {
            V3 d;
            do {
                d = V3{float(ToolRand() * 2 - 1), float(ToolRand() * 2 - 1), float(ToolRand() * 2 - 1)};
            } while (Dot(d, d) > 1.0f);
            V3 p = c + d * g_config.toolSpread;
            spheres.push_back({{p.x, p.y, p.z}, g_config.toolR0, g_config.toolR1, g_config.toolR2});
        }
    }
    CarveNow(spheres, hitBody, g_config.toolSound);  // spray: a few holes, at once
}

// Browse sessions run without the trace (an hour of it is gigabytes): frame rate and worst frame every 10 s of play.
void BrowseFrameStats() {
    static uint64_t last = 0, windowStart = 0;
    static int frames = 0;
    static double worstMs = 0.0;
    const uint64_t now = trace::Now();
    if (td::State() != td::kPlay) {
        last = 0;
        return;
    }
    if (!last) {
        windowStart = now;
        frames = 0;
        worstMs = 0.0;
    } else {
        worstMs = std::max(worstMs, Seconds(last, now) * 1000.0);
        ++frames;
    }
    last = now;
    const double span = Seconds(windowStart, now);
    if (span < 10.0) return;
    LOG("frames: %.0f fps over %.0f s, worst frame %.1f ms; bodies %d, simulated %d", frames / span, span, worstMs,
        td::BodyCount(), td::SimulatedBodies());
    windowStart = now;
    frames = 0;
    worstMs = 0.0;
}

}  // namespace

void Quit(bool ok) {
    if (g_config.touchCache) fix::LogTouchCacheStats();
    shadow::OnLeavePlay();  // PhysX shadow totals
    trace::Flush();
    LOG("session: terminating the game (%s)", ok ? "done" : "failed");
    logx::Flush();
    TerminateProcess(GetCurrentProcess(), ok ? 0 : 3);
}

void Init() {
    if (g_config.toolEnabled) {
        inputrec::SetInputObserver(&ObserveInput);
        if (g_config.toolMode == "fragment")
            LOG("tool: debris tool on (key %s, fragment: %.1f m cubes cut into %.2f m pieces every %d ticks, carved in "
                "batches of %dx%d holes, %.1f ms per tick)",
                g_config.toolKey.c_str(), g_config.toolFragmentSize, g_config.toolPieceSize,
                g_config.toolFragmentTicks, g_config.toolCarveBatch, g_config.toolCarveBatch,
                g_config.toolCarveMsPerTick);
        else
            LOG("tool: debris tool on (key %s, spray: %d holes per tick within %.1f m, radii %.2f/%.2f/%.2f)",
                g_config.toolKey.c_str(), g_config.toolHolesPerTick, g_config.toolSpread, g_config.toolR0,
                g_config.toolR1, g_config.toolR2);
    }
    const std::string& m = g_config.mode;
    if (m != "bench" && m != "record" && m != "replay") return;
    if (m == "replay" && !inputrec::LoadReplay(g_config.replayFile)) {
        LOG("session: replay file unusable, nothing to do");
        return;
    }
    g_phase = Phase::WaitMenu;
    g_initQpc = trace::Now();
    g_rng = 0x9E3779B97F4A7C15ull ^ (uint64_t(g_config.seed) * 0x100000001B3ull);
    if (!g_rng) g_rng = 1;
    if (m == "bench")
        LOG("bench: armed (mission %s, %.0f holes/s for %.0f s, radii %.2f/%.2f/%.2f)", g_config.mission.c_str(),
            g_config.holesPerSecond, g_config.durationSeconds, g_config.r0, g_config.r1, g_config.r2);
    else
        LOG("session: %s armed for level %s / %s / %s", m.c_str(), g_config.mod.c_str(), g_config.levelFile.c_str(),
            g_config.layers.c_str());
}

void OnFrame() {
    g_frameCarveMs = 0.0;
    if (g_config.mode == "browse") BrowseFrameStats();
    if (g_phase == Phase::WaitMenu) {
        uint64_t now = trace::Now();
        int st = td::State();
        if (st == td::kSplash && !g_splashSkipped) {
            if (!g_splashQpc) g_splashQpc = now;
            if (Seconds(g_splashQpc, now) >= g_config.splashSkipSeconds) {
                g_splashSkipped = true;
                SkipSplash();
            }
        } else if (st == td::kMenu) {
            if (!g_menuQpc) g_menuQpc = now;
            double inMenu = Seconds(g_menuQpc, now);
            if (inMenu >= g_config.startDelaySeconds) {
                if (StartLevelNative()) {
                    g_phase = Phase::Loading;
                } else if (inMenu > g_config.startDelaySeconds + g_config.modWaitSeconds) {
                    LOG("bench: mod %s not listed after %.0f s in the menu", g_config.mod.c_str(), inMenu);
                    Finish(false);
                }
            }
        }
    }
    if (g_phase == Phase::Run && td::State() == td::kPlay) LaserEffects();
    if ((g_phase == Phase::WaitMenu || g_phase == Phase::Loading) && LoadTimedOut()) {
        LOG("bench: level did not start within %.0f s", g_config.loadTimeoutSeconds);
        Finish(false);
    }
}

void OnTick() {
    if (g_config.toolEnabled && td::State() == td::kPlay) ToolTick();
    if (g_phase == Phase::Warmup) {
        if (++g_warmTicks == 30) TurnView();  // half a second in: the player is in place
        if (g_warmTicks >= int(g_config.warmupSeconds * 60.0)) {
            g_phase = Phase::Run;
            g_runTime = 0.0;
            LOG("bench: carving starts (bodies %d, shapes %d)", td::BodyCount(), td::ShapeCount());
            Mark(kRunStart);
        }
        return;
    }
    if (g_phase != Phase::Run) return;
    double dt = td::TickDt();
    if (dt <= 0.0 || dt > 0.1) dt = 1.0 / 60.0;
    g_runTime += dt;
    g_holeAcc += g_config.holesPerSecond * dt;
    while (g_holeAcc >= 1.0) {
        g_holeAcc -= 1.0;
        DoHole();
    }
    if (g_runTime >= g_nextLog) {
        g_nextLog += 5.0;
        LOG("bench: t=%.0f s, %d holes, %d hits, %lld voxels removed, bodies %d, simulated %d", g_runTime, g_holes,
            g_hits, g_removed, td::BodyCount(), td::SimulatedBodies());
    }
    if (g_runTime >= g_config.durationSeconds) Finish(true);
}

void OnState(int, int newState) {
    if (newState == td::kPlay) {  // the debris tool's holes: the same sequence in a recording and its replays
        g_toolRng = 0x9E3779B97F4A7C15ull;
        g_toolDown = false;
    } else {  // cuts still waiting belong to the level being left
        g_carveQueue.clear();
        g_carveWaiting = 0;
    }
    if (g_phase == Phase::Loading && newState == td::kPlay) {
        Mark(kPlayReached);
        if (g_config.mode == "record") {
            g_phase = Phase::Recording;
            inputrec::ArmRecord();
            LOG("session: level running, recording starts with the first PLAY frame");
        } else if (g_config.mode == "replay") {
            g_phase = Phase::Replaying;
            inputrec::ArmReplay();
            LOG("session: level running, replay starts with the first PLAY frame");
        } else {
            g_phase = Phase::Warmup;
            g_warmTicks = 0;
            LOG("bench: level running, warming up %.0f s", g_config.warmupSeconds);
        }
    } else if ((g_phase == Phase::Recording || g_phase == Phase::Replaying) && newState != td::kPlay) {
        g_phase = Phase::Done;
        inputrec::OnLeavePlay();
    } else if ((g_phase == Phase::Warmup || g_phase == Phase::Run) && newState != td::kPlay) {
        LOG("bench: left PLAY during the run (state %s)", td::StateName(newState));
        Finish(false);
    }
}

}  // namespace bench
