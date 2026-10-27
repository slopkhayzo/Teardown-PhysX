#include "fix/touch_cache.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "core/config.h"
#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"
#include "trace/trace.h"

using td::Addr;

namespace fix {

namespace {

using TouchFn = uint64_t(__fastcall*)(void* physics, uint8_t* a, uint8_t* b);
using InterpTransformFn = td::Transform*(__fastcall*)(void* shape, td::Transform* out);

TouchFn oTouch = nullptr;
DWORD g_mainThread = 0;

struct Side {
    int handle;
    const void* vox;
    int dims[3];
    uint32_t count;
    uint32_t epoch;
};
struct Entry {
    Side a, b;
    float relPos[3];
    float relRot[4];
    uint64_t ret;
};

std::unordered_map<uint64_t, Entry> g_cache;
std::unordered_map<const void*, uint32_t> g_epoch;  // bumped when MakeHole works on a shape
uint64_t g_hits = 0, g_misses = 0, g_stale = 0, g_validated = 0, g_mismatch = 0, g_bypass = 0;

uint32_t EpochOf(const uint8_t* shape) {
    auto it = g_epoch.find(shape);
    return it == g_epoch.end() ? 0 : it->second;
}

bool FillSide(uint8_t* s, Side& out) {
    uint8_t* vox = td::ShapeVoxels(s);
    if (!vox) return false;
    out.handle = td::At<int>(s, 0x0c);
    out.vox = td::VoxData(vox);
    const int* d = td::VoxDims(vox);
    out.dims[0] = d[0];
    out.dims[1] = d[1];
    out.dims[2] = d[2];
    out.count = td::At<uint32_t>(vox, 0x5c);
    out.epoch = EpochOf(s);
    return true;
}

bool SameSide(const Side& x, const Side& y) {
    return x.handle == y.handle && x.vox == y.vox && x.dims[0] == y.dims[0] && x.dims[1] == y.dims[1] &&
           x.dims[2] == y.dims[2] && x.count == y.count && x.epoch == y.epoch;
}

// Transform of b expressed in a's frame (what the touch test depends on).
void Relative(uint8_t* a, uint8_t* b, float pos[3], float rot[4]) {
    auto interp = td::Fn<InterpTransformFn>(Addr::ShapeInterpTransform);
    td::Transform ta, tb;
    interp(a, &ta);
    interp(b, &tb);
    const float* q = ta.rot;  // conj(qa) * (pb - pa)
    float v[3] = {tb.pos[0] - ta.pos[0], tb.pos[1] - ta.pos[1], tb.pos[2] - ta.pos[2]};
    float u[3] = {-q[0], -q[1], -q[2]};
    float t[3] = {2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    pos[0] = v[0] + q[3] * t[0] + (u[1] * t[2] - u[2] * t[1]);
    pos[1] = v[1] + q[3] * t[1] + (u[2] * t[0] - u[0] * t[2]);
    pos[2] = v[2] + q[3] * t[2] + (u[0] * t[1] - u[1] * t[0]);
    const float* r = tb.rot;  // conj(qa) * qb
    float aw = q[3], ax = -q[0], ay = -q[1], az = -q[2];
    rot[0] = aw * r[0] + ax * r[3] + ay * r[2] - az * r[1];
    rot[1] = aw * r[1] - ax * r[2] + ay * r[3] + az * r[0];
    rot[2] = aw * r[2] + ax * r[1] - ay * r[0] + az * r[3];
    rot[3] = aw * r[3] - ax * r[0] - ay * r[1] - az * r[2];
}

bool Close(const float* x, const float* y, int n, float eps) {
    for (int i = 0; i < n; ++i)
        if (std::fabs(x[i] - y[i]) > eps) return false;
    return true;
}

uint64_t Key(uint8_t* a, uint8_t* b, bool& swapped) {
    uint32_t ha = uint32_t(td::At<int>(a, 0x0c)), hb = uint32_t(td::At<int>(b, 0x0c));
    swapped = g_config.touchCacheSymmetric && ha > hb;
    if (swapped) std::swap(ha, hb);
    return (uint64_t(ha) << 32) | hb;
}

uint64_t __fastcall HkTouch(void* physics, uint8_t* a, uint8_t* b) {
    uint64_t t0 = trace::Now();
    if (GetCurrentThreadId() != g_mainThread || !a || !b) {
        ++g_bypass;
        uint64_t r = oTouch(physics, a, b);
        trace::Emit(trace::kTouchTest, t0, trace::Now(), 1ull << 62);
        return r;
    }
    bool swapped;
    uint64_t key = Key(a, b, swapped);
    uint8_t* first = swapped ? b : a;
    uint8_t* second = swapped ? a : b;
    Side sa{}, sb{};
    bool sides = FillSide(first, sa) && FillSide(second, sb);
    float rp[3], rr[4];
    if (sides) Relative(first, second, rp, rr);
    uint64_t tag = (key * 0x9E3779B97F4A7C15ull) >> 2;  // pair id for the trace (low 62 bits)
    if (sides) {
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            const Entry& e = it->second;
            if (SameSide(e.a, sa) && SameSide(e.b, sb) && Close(e.relPos, rp, 3, 1e-4f) &&
                Close(e.relRot, rr, 4, 1e-5f)) {
                ++g_hits;
                uint64_t ret = e.ret;
                int every = g_config.touchCacheValidate;
                if (every > 0 && g_hits % uint64_t(every) == 0) {
                    ++g_validated;
                    uint64_t truth = oTouch(physics, a, b);
                    if ((truth & 0xff) != (ret & 0xff)) {
                        ++g_mismatch;
                        if (g_mismatch <= 20)
                            LOG("touch cache MISMATCH: shapes %d/%d cached %d, engine %d", sa.handle, sb.handle,
                                int(ret & 0xff), int(truth & 0xff));
                        ret = truth;
                        it->second.ret = truth;
                    }
                }
                trace::Emit(trace::kTouchTest, t0, trace::Now(), tag | (1ull << 63));
                return ret;
            }
            ++g_stale;
        }
    }
    ++g_misses;
    uint64_t r = oTouch(physics, a, b);
    if (sides) {
        if (g_cache.size() > 400000) g_cache.clear();
        Entry e{sa, sb, {rp[0], rp[1], rp[2]}, {rr[0], rr[1], rr[2], rr[3]}, r};
        g_cache[key] = e;
    }
    trace::Emit(trace::kTouchTest, t0, trace::Now(), tag);
    return r;
}

}  // namespace

bool InstallTouchCache() {
    g_mainThread = 0;
    if (!td::CodeMatches(Addr::ShapeTouchTest) || !td::CodeMatches(Addr::ShapeInterpTransform)) {
        LOG("touch cache: bytes differ, not installed");
        return false;
    }
    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(td::VA(Addr::ShapeTouchTest)),
                                 reinterpret_cast<void*>(&HkTouch), reinterpret_cast<void**>(&oTouch));
    LOG("touch cache: %s (validate every %d hits, symmetric %d)", MH_StatusToString(st),
        g_config.touchCacheValidate, int(g_config.touchCacheSymmetric));
    return st == MH_OK;
}

void SetMainThread(unsigned long tid) { g_mainThread = tid; }

void OnShapesModified(const uint8_t* entries, int count) {
    if (!entries || count <= 0 || count > 100000) return;
    for (int i = 0; i < count; ++i) {
        const uint8_t* shape = *reinterpret_cast<uint8_t* const*>(entries + size_t(i) * 200);
        if (shape) ++g_epoch[shape];
    }
}

void OnLevelChange() {
    g_cache.clear();
    g_epoch.clear();
}

void LogTouchCacheStats() {
    uint64_t total = g_hits + g_misses;
    LOG("touch cache: %llu tests, %llu hits (%.1f%%), %llu misses (%llu stale), %llu validated, %llu mismatches, "
        "%llu off-main-thread",
        total, g_hits, total ? 100.0 * double(g_hits) / double(total) : 0.0, g_misses, g_stale, g_validated,
        g_mismatch, g_bypass);
}

}  // namespace fix
