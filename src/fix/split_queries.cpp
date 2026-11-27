#include "fix/split_queries.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"
#include "trace/trace.h"

using td::Addr;

namespace fix {
namespace {

// The queries served, as return addresses of their calls (2.2.0.20261118): split_per_body's first gather over the
// region (0x14013b010, 2.1.0 0x1401336b0) and its flood's per-shape query (0x140140880, 2.1.0 0x140138f20).
constexpr uintptr_t kCollectReturn = 0x13b08a;
constexpr uintptr_t kFloodReturn = 0x140a6e;

// SceneQueryShapes(scene, body, out list, &min, &max, filter): the body's shapes whose leaf box overlaps [min, max]
// in the two trees of its kind and that the filter passes, appended to the list.
using QueryFn = void(__fastcall*)(uint8_t* scene, uint8_t* body, int* out, const float* mn, const float* mx,
                                  const uint32_t* filter);
// The trees' leaf primitives (one per change of any tree): treeRef is the tree's slot in the physics object.
using InsertFn = void*(__fastcall*)(void** treeRef, const float* mn, const float* mx, uint8_t* payload);
using UpdateFn = void(__fastcall*)(void** treeRef, void* node, const float* mn, const float* mx);
using RemoveFn = void(__fastcall*)(void** treeRef, void* node);
using ClearFn = void(__fastcall*)(void** treeRef);
using ReserveFn = void(__fastcall*)(int* list, int capacity);

QueryFn oQuery = nullptr;
InsertFn oInsert = nullptr;
UpdateFn oUpdate = nullptr;
RemoveFn oRemove = nullptr;
ClearFn oClear = nullptr;
DWORD g_main = 0;

// Two grids: 4 m cells for leaves over at most kFineMax of them, 32 m cells for larger ones (floors, walls of a whole
// building), and a list every query scans for leaves over more than kCoarseMax coarse cells (a level's ground). One grid
// of 4 m cells for everything made vertigo's mirror 153,000 cells (a 14-25 ms walk); a list for leaves over 64 cells
// made each query test 333 of them.
constexpr float kInvCell[2] = {1.0f / 4.0f, 1.0f / 32.0f};
constexpr int kFineMax = 512;
constexpr int kCoarseMax = 4096;
constexpr int kMaxQueryCells = 512;      // a query over more fine cells goes to the engine
constexpr int kLimit = (1 << 19) - 1;    // cell coordinates are kept within +-2^19

// Tree node (0x140522df0 and the walk 0x140522780): +0x08 min xyz, +0x14 max xyz, +0x20 payload (a leaf) or first
// child, +0x28 second child (0: a leaf). A tree object holds its root at +0x18.
inline const float* NodeBox(void* node) { return reinterpret_cast<const float*>(static_cast<uint8_t*>(node) + 8); }
inline void* NodeA(void* node) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + 0x20); }
inline void* NodeB(void* node) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(node) + 0x28); }

// The walk's leaf test (0x140522780): strict, boxes that only share a face don't overlap.
inline bool Overlap(const float* a, const float* q) {
    return q[0] < a[3] && q[1] < a[4] && q[2] < a[5] && a[0] < q[3] && a[1] < q[4] && a[2] < q[5];
}

inline int Cell(float v, int level) {
    const float c = std::floor(v * kInvCell[level]);
    if (!(c > -float(kLimit))) return -kLimit;  // (NaN too)
    if (c > float(kLimit)) return kLimit;
    return int(c);
}
inline uint64_t CellKey(int x, int y, int z, int level) {
    return (1ull << 63) | (uint64_t(level) << 60) | (uint64_t(uint32_t(x + (1 << 19))) << 40) |
           (uint64_t(uint32_t(y + (1 << 19))) << 20) | uint64_t(uint32_t(z + (1 << 19)));
}
inline int64_t Cells(const int* lo, const int* hi) {
    return (int64_t(hi[0]) - lo[0] + 1) * (int64_t(hi[1]) - lo[1] + 1) * (int64_t(hi[2]) - lo[2] + 1);
}

// Open-addressing map: cell key -> list of leaf slots (cells are never removed; an empty one stays).
struct CellTable {
    std::vector<uint64_t> keys;
    std::vector<uint32_t> vals;
    std::vector<std::vector<uint32_t>> lists;
    size_t mask = 0;
    static size_t Hash(uint64_t k) {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        k ^= k >> 33;
        return size_t(k);
    }
    void Clear() {
        keys.assign(1024, 0);
        vals.assign(1024, 0);
        lists.clear();
        mask = 1023;
    }
    std::vector<uint32_t>* Find(uint64_t key) {
        for (size_t i = Hash(key) & mask;; i = (i + 1) & mask) {
            if (keys[i] == key) return &lists[vals[i]];
            if (keys[i] == 0) return nullptr;
        }
    }
    std::vector<uint32_t>& Get(uint64_t key) {
        if ((lists.size() + 1) * 2 > keys.size()) Grow();
        size_t i = Hash(key) & mask;
        for (; keys[i] != 0; i = (i + 1) & mask)
            if (keys[i] == key) return lists[vals[i]];
        keys[i] = key;
        vals[i] = uint32_t(lists.size());
        lists.emplace_back();
        return lists.back();
    }
    void Grow() {
        std::vector<uint64_t> k2(keys.size() * 2, 0);
        std::vector<uint32_t> v2(keys.size() * 2, 0);
        const size_t m2 = k2.size() - 1;
        for (size_t i = 0; i < keys.size(); ++i) {
            if (!keys[i]) continue;
            size_t j = Hash(keys[i]) & m2;
            while (k2[j]) j = (j + 1) & m2;
            k2[j] = keys[i];
            v2[j] = vals[i];
        }
        keys.swap(k2);
        vals.swap(v2);
        mask = m2;
    }
};

struct Leaf {
    void* node;
    uint8_t* shape;
    float box[6];
    int lo[3], hi[3];  // its cells, in the grid of its level
    uint32_t stamp;
    int8_t tree;       // 0, 1; -1: a free slot
    int8_t level;      // 0 fine grid, 1 coarse grid, 2 the big list
};

// The mirror of the two static trees. Under g_lock: the leaf primitives may run on any thread.
struct Mirror {
    bool valid = false;
    uint8_t* physics = nullptr;
    std::vector<Leaf> leaves;
    std::vector<uint32_t> freeSlots;
    std::unordered_map<void*, uint32_t> byNode;
    CellTable cells;
    std::vector<uint32_t> big;
    uint32_t stamp = 0;
    size_t live = 0;

    void Reset() {
        valid = false;
        physics = nullptr;
        leaves.clear();
        freeSlots.clear();
        byNode.clear();
        cells.Clear();
        big.clear();
        stamp = 0;
        live = 0;
    }

    // The level and cells a box goes to.
    static int Place(const float* box, int* lo, int* hi) {
        for (int level = 0; level < 2; ++level) {
            for (int a = 0; a < 3; ++a) {
                lo[a] = Cell(box[a], level);
                hi[a] = Cell(box[a + 3], level);
            }
            const int64_t n = Cells(lo, hi);
            if (n <= 0) return 2;  // (NaN)
            if (n <= (level == 0 ? kFineMax : kCoarseMax)) return level;
        }
        return 2;
    }

    void Index(uint32_t i) {
        Leaf& l = leaves[i];
        l.level = int8_t(Place(l.box, l.lo, l.hi));
        if (l.level == 2) {
            big.push_back(i);
            return;
        }
        for (int x = l.lo[0]; x <= l.hi[0]; ++x)
            for (int y = l.lo[1]; y <= l.hi[1]; ++y)
                for (int z = l.lo[2]; z <= l.hi[2]; ++z) cells.Get(CellKey(x, y, z, l.level)).push_back(i);
    }

    static void Erase(std::vector<uint32_t>& v, uint32_t i) {
        auto it = std::find(v.begin(), v.end(), i);
        if (it != v.end()) {
            *it = v.back();
            v.pop_back();
        }
    }

    void Unindex(uint32_t i) {
        Leaf& l = leaves[i];
        if (l.level == 2) {
            Erase(big, i);
            return;
        }
        for (int x = l.lo[0]; x <= l.hi[0]; ++x)
            for (int y = l.lo[1]; y <= l.hi[1]; ++y)
                for (int z = l.lo[2]; z <= l.hi[2]; ++z)
                    if (std::vector<uint32_t>* c = cells.Find(CellKey(x, y, z, l.level))) Erase(*c, i);
    }

    void Add(void* node, uint8_t* shape, const float* mn, const float* mx, int tree) {
        uint32_t i;
        if (!freeSlots.empty()) {
            i = freeSlots.back();
            freeSlots.pop_back();
        } else {
            i = uint32_t(leaves.size());
            leaves.emplace_back();
        }
        Leaf& l = leaves[i];
        l.node = node;
        l.shape = shape;
        std::memcpy(l.box, mn, 3 * sizeof(float));
        std::memcpy(l.box + 3, mx, 3 * sizeof(float));
        l.stamp = 0;
        l.tree = int8_t(tree);
        byNode[node] = i;
        Index(i);
        ++live;
    }

    bool Move(void* node, const float* mn, const float* mx) {
        auto it = byNode.find(node);
        if (it == byNode.end()) return false;
        Leaf& l = leaves[it->second];
        float box[6];
        std::memcpy(box, mn, 3 * sizeof(float));
        std::memcpy(box + 3, mx, 3 * sizeof(float));
        int lo[3], hi[3];
        const int level = Place(box, lo, hi);
        bool sameCells = level == l.level && level != 2;
        for (int a = 0; a < 3 && sameCells; ++a) sameCells = lo[a] == l.lo[a] && hi[a] == l.hi[a];
        std::memcpy(l.box, box, sizeof(box));
        if (!sameCells) {
            Unindex(it->second);
            Index(it->second);
        }
        return true;
    }

    bool Remove(void* node) {
        auto it = byNode.find(node);
        if (it == byNode.end()) return false;
        const uint32_t i = it->second;
        Unindex(i);
        leaves[i].tree = -1;
        leaves[i].node = nullptr;
        leaves[i].shape = nullptr;
        freeSlots.push_back(i);
        byNode.erase(it);
        --live;
        return true;
    }

    // Every leaf of the two static trees, walked from their roots.
    void Build(uint8_t* phys) {
        Reset();
        physics = phys;
        std::vector<void*> stack;
        for (int t = 0; t < 2; ++t) {
            uint8_t* tree = *reinterpret_cast<uint8_t**>(phys + 8 + 8 * t);
            void* root = tree ? *reinterpret_cast<void**>(tree + 0x18) : nullptr;
            if (!root) continue;
            stack.assign(1, root);
            while (!stack.empty()) {
                void* n = stack.back();
                stack.pop_back();
                if (NodeB(n)) {
                    stack.push_back(NodeA(n));
                    stack.push_back(NodeB(n));
                } else {
                    const float* b = NodeBox(n);
                    Add(n, static_cast<uint8_t*>(NodeA(n)), b, b + 3, t);
                }
            }
        }
        valid = true;
    }
};

SRWLOCK g_lock = SRWLOCK_INIT;
Mirror g_m;
uint8_t* g_hookPhysics = nullptr;  // the physics object whose trees the hooks follow (as g_m.physics, read unlocked)

struct Stats {
    uint64_t calls = 0, served = 0, notServed = 0, notStatic = 0, offThread = 0, otherFilter = 0, tooBig = 0;
    uint64_t multiplayer = 0, builds = 0, validated = 0, mismatch = 0, results = 0, invalidations = 0;
    uint64_t ticksServed = 0, ticksBuild = 0;
} g_st;

// Which static tree (0, 1) a tree slot is, or -1.
inline int StaticTree(void** treeRef) {
    uint8_t* p = g_hookPhysics;
    if (!p) return -1;
    const uintptr_t r = reinterpret_cast<uintptr_t>(treeRef), b = reinterpret_cast<uintptr_t>(p);
    return r == b + 8 ? 0 : r == b + 0x10 ? 1 : -1;
}

void Invalidate() {
    g_m.valid = false;
    ++g_st.invalidations;
}

void* __fastcall HkInsert(void** treeRef, const float* mn, const float* mx, uint8_t* payload) {
    void* node = oInsert(treeRef, mn, mx, payload);
    const int t = StaticTree(treeRef);
    if (t >= 0 && node) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_m.valid) g_m.Add(node, payload, mn, mx, t);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return node;
}

void __fastcall HkUpdate(void** treeRef, void* node, const float* mn, const float* mx) {
    oUpdate(treeRef, node, mn, mx);
    if (StaticTree(treeRef) >= 0) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_m.valid && !g_m.Move(node, mn, mx)) Invalidate();  // a leaf the mirror doesn't know: walk again
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void __fastcall HkRemove(void** treeRef, void* node) {
    if (StaticTree(treeRef) >= 0) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_m.valid && !g_m.Remove(node)) Invalidate();
        ReleaseSRWLockExclusive(&g_lock);
    }
    oRemove(treeRef, node);
}

void __fastcall HkClear(void** treeRef) {
    if (StaticTree(treeRef) >= 0) {
        AcquireSRWLockExclusive(&g_lock);
        Invalidate();
        ReleaseSRWLockExclusive(&g_lock);
    }
    oClear(treeRef);
}

bool Multiplayer() {
    uint8_t* g = td::GameObj();
    uint8_t* net = g ? td::At<uint8_t*>(g, 0x160) : nullptr;
    return !net || td::At<int>(net, 4) != 0;
}

// The query's filter as 0x1400dbc60 applies it: {any flags, all flags, no flags, layers (low byte)}, then lists of
// excluded bodies (+0x10 count) and shapes (+0x40 count). Served only without exclusions.
inline bool Passes(const uint32_t* f, uint8_t* shape) {
    const uint32_t flags = td::At<uint16_t>(shape, 0xc8);
    return (f[0] & flags) != 0 && (uint8_t(f[3]) & td::At<uint8_t>(shape, 0xca)) != 0 && (f[1] & flags) == f[1] &&
           (f[2] & flags) == 0;
}

// The mirror's answer: shapes of `body` with leaves strictly overlapping [mn, mx], passing the filter. False when the
// box spans too many cells (the engine answers it).
bool Answer(uint8_t* body, const float* mn, const float* mx, const uint32_t* filter, std::vector<uint8_t*>& out) {
    out.clear();
    const float q[6] = {mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]};
    int lo[2][3], hi[2][3];
    for (int level = 0; level < 2; ++level)
        for (int a = 0; a < 3; ++a) {
            lo[level][a] = Cell(q[a], level);
            hi[level][a] = Cell(q[a + 3], level);
        }
    const int64_t n = Cells(lo[0], hi[0]);
    if (n > kMaxQueryCells || n <= 0) return false;
    if (++g_m.stamp == 0) {
        for (Leaf& l : g_m.leaves) l.stamp = 0;
        g_m.stamp = 1;
    }
    const uint32_t stamp = g_m.stamp;
    // the leaves whose box overlaps, then their shapes' fields: fetched together (each a cache miss otherwise)
    thread_local std::vector<uint8_t*> hit;
    hit.clear();
    auto test = [&](uint32_t i) {
        Leaf& l = g_m.leaves[i];
        if (l.stamp == stamp) return;
        l.stamp = stamp;
        if (!Overlap(l.box, q)) return;
        uint8_t* s = l.shape;
        _mm_prefetch(reinterpret_cast<const char*>(s + 0x10), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(s + 0xc8), _MM_HINT_T0);
        hit.push_back(s);
    };
    for (uint32_t i : g_m.big) test(i);
    for (int level = 0; level < 2; ++level)
        for (int x = lo[level][0]; x <= hi[level][0]; ++x)
            for (int y = lo[level][1]; y <= hi[level][1]; ++y)
                for (int z = lo[level][2]; z <= hi[level][2]; ++z)
                    if (std::vector<uint32_t>* c = g_m.cells.Find(CellKey(x, y, z, level)))
                        for (uint32_t i : *c) test(i);
    for (uint8_t* s : hit)
        if (td::ShapeBody(s) == body && Passes(filter, s)) out.push_back(s);
    return true;
}

// The engine's list {count, capacity, data, inline storage}, for a validation's copy of the engine's answer.
struct EngineList {
    int count = 0;
    int cap = 256;
    uint8_t** data;
    uint8_t* inl[256];
    EngineList() : data(inl) {}
    ~EngineList() {
        if (data != inl) td::Fn<void(__fastcall*)(void*)>(Addr::EngineFree)(data);
    }
};
static_assert(offsetof(EngineList, inl) == 0x10);

void Append(int* list, const std::vector<uint8_t*>& v) {
    for (uint8_t* s : v) {
        if (list[1] <= list[0]) td::Fn<ReserveFn>(Addr::ListReserve)(list, list[1] * 2 + 1);
        uint8_t** data = *reinterpret_cast<uint8_t***>(list + 2);
        data[list[0]++] = s;
    }
}

void __fastcall HkQuery(uint8_t* scene, uint8_t* body, int* out, const float* mn, const float* mx,
                        const uint32_t* filter) {
    const uint64_t t0 = g_config.traceEnabled ? trace::Now() : 0;
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - td::Base();
    ++g_st.calls;
    bool served = false;
    uint64_t found = 0;
    if ((ret == kFloodReturn || ret == kCollectReturn) && out && body && filter) {
        thread_local std::vector<uint8_t*> mine;
        if (GetCurrentThreadId() != g_main) {
            ++g_st.offThread;
        } else if (td::BodyDynamic(body)) {
            ++g_st.notStatic;
        } else if (reinterpret_cast<const int*>(filter)[4] != 0 || reinterpret_cast<const int*>(filter)[16] != 0) {
            ++g_st.otherFilter;
        } else if (Multiplayer()) {
            ++g_st.multiplayer;
        } else {
            AcquireSRWLockExclusive(&g_lock);  // (exclusive: answering stamps the leaves)
            uint8_t* phys = scene ? td::At<uint8_t*>(scene, 8) : nullptr;
            if (phys && (!g_m.valid || g_m.physics != phys)) {
                const uint64_t tb = trace::Now();
                g_m.Build(phys);
                g_hookPhysics = phys;
                ++g_st.builds;
                g_st.ticksBuild += trace::Now() - tb;
            }
            const bool answered = phys && g_m.valid && Answer(body, mn, mx, filter, mine);
            ReleaseSRWLockExclusive(&g_lock);
            if (!answered) {
                ++g_st.tooBig;
            } else {
                served = true;
                ++g_st.served;
                g_st.results += mine.size();
                found = mine.size();
                const int every = g_config.splitQueriesValidate;
                if (every > 0 && g_st.served % uint64_t(every) == 0) {
                    // the engine's answer goes to the caller; the mirror's must be the same set
                    ++g_st.validated;
                    const int before = out[0];
                    oQuery(scene, body, out, mn, mx, filter);
                    uint8_t** data = *reinterpret_cast<uint8_t***>(out + 2);
                    std::vector<uint8_t*> truth(data + before, data + out[0]);
                    std::vector<uint8_t*> got(mine);
                    std::sort(truth.begin(), truth.end());
                    std::sort(got.begin(), got.end());
                    if (truth != got && ++g_st.mismatch <= 20)
                        LOG("split queries MISMATCH: body %d, box (%.2f %.2f %.2f)-(%.2f %.2f %.2f): engine %zu shapes, "
                            "mirror %zu (%zu leaves)",
                            td::At<int>(body, 0x0c), mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], truth.size(), got.size(),
                            g_m.live);
                } else {
                    Append(out, mine);
                }
                if (t0) g_st.ticksServed += trace::Now() - t0;
            }
        }
    }
    if (!served) {
        ++g_st.notServed;
        const int before = out ? out[0] : 0;
        oQuery(scene, body, out, mn, mx, filter);
        found = out ? uint64_t(out[0] - before) : 0;
    }
    if (t0) trace::Emit(trace::kSceneQuery, t0, trace::Now(), found | (uint64_t(served) << 63));
}

bool CallsQuery(uintptr_t ret) {
    const uint8_t* call = reinterpret_cast<const uint8_t*>(td::Base() + ret - 5);
    int32_t rel;
    std::memcpy(&rel, call + 1, 4);
    return call[0] == 0xe8 && td::Base() + ret + intptr_t(rel) == td::VA(Addr::SceneQueryShapes);
}

template <typename F> bool Hook(Addr a, void* detour, F* original) {
    return td::CodeMatches(a) &&
           MH_CreateHook(reinterpret_cast<void*>(td::VA(a)), detour, reinterpret_cast<void**>(original)) == MH_OK;
}

}  // namespace

bool InstallSplitQueries() {
    if (!td::CodeMatches(Addr::SceneQueryShapes) || !td::CodeMatches(Addr::ListReserve) ||
        !td::CodeMatches(Addr::EngineFree) || !CallsQuery(kCollectReturn) || !CallsQuery(kFloodReturn)) {
        LOG("fix: split queries: code differs, not installed");
        return false;
    }
    g_m.Reset();
    const bool ok = Hook(Addr::TreeInsert, reinterpret_cast<void*>(&HkInsert), &oInsert) &&
                    Hook(Addr::TreeUpdateLeaf, reinterpret_cast<void*>(&HkUpdate), &oUpdate) &&
                    Hook(Addr::TreeRemove, reinterpret_cast<void*>(&HkRemove), &oRemove) &&
                    Hook(Addr::TreeClear, reinterpret_cast<void*>(&HkClear), &oClear) &&
                    Hook(Addr::SceneQueryShapes, reinterpret_cast<void*>(&HkQuery), &oQuery);
    LOG("fix: split queries: %s (validate every %d served queries)", ok ? "installed" : "FAILED",
        g_config.splitQueriesValidate);
    return ok;
}

void SplitQueriesSetMainThread(unsigned long tid) { g_main = tid; }

void SplitQueriesLevelStart() {
    // Walked while the level starts rather than at its first hole (a few ms on a big level).
    if (Multiplayer()) return;
    uint8_t* phys = td::PhysicsObj();
    if (!phys) return;
    const uint64_t t0 = trace::Now();
    AcquireSRWLockExclusive(&g_lock);
    g_m.Build(phys);
    g_hookPhysics = phys;
    ++g_st.builds;
    g_st.ticksBuild += trace::Now() - t0;
    const size_t live = g_m.live;
    ReleaseSRWLockExclusive(&g_lock);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    LOG("split queries: mirror of the static trees: %zu leaves, %.1f ms", live,
        double(trace::Now() - t0) * 1000.0 / double(f.QuadPart));
}

void SplitQueriesLevelChange() {
    AcquireSRWLockExclusive(&g_lock);
    g_m.Reset();
    g_hookPhysics = nullptr;
    ReleaseSRWLockExclusive(&g_lock);
}

void LogSplitQueriesStats() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / double(f.QuadPart);
    AcquireSRWLockShared(&g_lock);
    const size_t live = g_m.live, cells = g_m.cells.lists.size(), big = g_m.big.size();
    ReleaseSRWLockShared(&g_lock);
    LOG("split queries: %llu scene queries: %llu served from the mirror (%.2f shapes each, %.1f ms when traced), %llu by "
        "the engine (the split's: %llu dynamic bodies, %llu off the main thread, %llu other filters, %llu multiplayer, "
        "%llu boxes over %d cells); %llu checked against the engine, %llu mismatches; mirror walked %llu times "
        "(%.1f ms), %llu invalidations; %zu leaves in %zu cells, %zu big",
        g_st.calls, g_st.served, g_st.served ? double(g_st.results) / double(g_st.served) : 0.0,
        double(g_st.ticksServed) * ms, g_st.notServed, g_st.notStatic, g_st.offThread, g_st.otherFilter,
        g_st.multiplayer, g_st.tooBig, kMaxQueryCells, g_st.validated, g_st.mismatch, g_st.builds,
        double(g_st.ticksBuild) * ms, g_st.invalidations, live, cells, big);
}

}  // namespace fix
