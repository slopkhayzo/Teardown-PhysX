#include "fix/static_body_update.h"

#include <windows.h>

#include <intrin.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"
#include "td/shape_events.h"
#include "trace/trace.h"

using td::Addr;

namespace fix {
namespace {

using BodyUpdateFn = void(__fastcall*)(uint8_t* body);
BodyUpdateFn oUpdate = nullptr;

// return addresses of the two calls: split/finish's inline per-body loop (0x14013ac53) and its twin (0x14013b2c4)
constexpr uintptr_t kSplitFinish = 0x13ac58;
constexpr uintptr_t kSplitTwin = 0x13b2c9;

// A split does add shapes to the static body: carving a static shape apart makes new shapes for its pieces, and
// those still holding on stay in it. They are made with flags 0x30 and need the static bits (0x35). Where they land
// in the child list follows no rule we could rely on (mostly at the end, sometimes a few places before it), so the
// fix records every shape Shape's constructor (0x1404dd9e0) makes, and a partial refresh gives the static bits to
// those that ended up in the body, to those it gave them last time (the breakage driver finishes its new shapes after
// the refresh inside it, which can reset their flags), and to unflagged shapes at the list's head (setParent puts a
// shape moved in from another body first). The full original runs every kFullEvery-th time.
constexpr uint32_t kFullEvery = 32;
constexpr size_t kMaxCreated = 200000;  // more since a refresh (a level loading): the next one is a full one
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<uint8_t*> g_created;  // shapes made since they were last looked at
bool g_overflow = false;
std::vector<uint8_t*> g_recent;  // shapes the last refresh gave the static bits
std::unordered_map<uint8_t*, uint32_t> g_sinceFull;  // static body -> partial refreshes since its full one
std::atomic<uint64_t> g_calls{0}, g_full{0}, g_partial{0}, g_added{0}, g_validated{0}, g_mismatch{0};

inline uint8_t* FirstChild(uint8_t* e) { return td::At<uint8_t*>(e, 0x28); }
inline uint8_t* NextSibling(uint8_t* e) { return td::At<uint8_t*>(e, 0x18); }
inline uint8_t* Parent(uint8_t* e) { return td::At<uint8_t*>(e, 0x10); }
inline bool IsShape(uint8_t* e) { return td::At<uint16_t>(e, 0x08) == 2; }
inline bool LiveShape(uint8_t* s) { return s && *reinterpret_cast<uintptr_t*>(s) == td::VA(Addr::ShapeVftable); }
inline bool IsStatic(uint8_t* body) { return td::At<uint8_t>(body, 0x138) == 0; }

// What 0x14002e450 does to a static body's shape: static (|1 &~2), and a physical one collides with everything
// (|4 &~8).
inline bool Unflagged(uint8_t* s) {
    uint16_t f = td::ShapeFlags(s);
    return !(f & 1) || (f & 2) || ((f & 0x10) && (f & 0x0c) != 4);
}
inline void MakeStatic(uint8_t* s) {
    uint16_t& f = td::At<uint16_t>(s, 0xc8);
    f = uint16_t((f | 1) & ~2);
    if (f & 0x10) f = uint16_t((f | 4) & ~8);
}

// The created shapes a refresh of `body` doesn't need any more: its own, dead ones, those of dynamic bodies (their
// refresh is the original). Kept: those not attached yet and those of other static bodies.
void PruneCreated(uint8_t* body) {
    size_t k = 0;
    for (uint8_t* s : g_created) {
        if (!LiveShape(s)) continue;
        uint8_t* p = Parent(s);
        if (p == body || (p && !IsStatic(p))) continue;
        g_created[k++] = s;
    }
    g_created.resize(k);
}

// The original after a partial refresh, every Nth: it must change no shape's flags.
void Validate(uint8_t* body, uint32_t sinceFull) {
    thread_local std::vector<std::pair<uint8_t*, uint16_t>> before;
    before.clear();
    int guard = 0;
    for (uint8_t* e = FirstChild(body); e && guard < 1000000; e = NextSibling(e), ++guard)
        if (IsShape(e)) before.push_back({e, td::ShapeFlags(e)});
    oUpdate(body);
    ++g_validated;
    size_t changed = 0;
    char detail[400] = "";
    int len = 0;
    for (size_t k = 0; k < before.size(); ++k) {
        uint16_t f = td::ShapeFlags(before[k].first);
        if (f == before[k].second) continue;
        if (++changed <= 6 && len < int(sizeof(detail)) - 60)
            len += std::snprintf(detail + len, sizeof(detail) - size_t(len), "%s#%zu shape %d %04x->%04x",
                                 changed > 1 ? ", " : "", k, td::At<int>(before[k].first, 0x0c), before[k].second, f);
    }
    if (changed && ++g_mismatch <= 40)
        LOG("static body update MISMATCH: body %d: %zu of %zu shapes changed, %u partial refreshes since the full one: "
            "%s",
            td::At<int>(body, 0x0c), changed, before.size(), sinceFull, detail);
}

void Created(uint8_t* self) {  // (td::OnShapeCreated)
    AcquireSRWLockExclusive(&g_lock);
    if (g_created.size() < kMaxCreated)
        g_created.push_back(self);
    else
        g_overflow = true;
    ReleaseSRWLockExclusive(&g_lock);
}

void __fastcall HkUpdate(uint8_t* body) {
    const uint64_t t0 = trace::Now();
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - td::Base();
    ++g_calls;
    bool partial = false;
    if (body && IsStatic(body) && (ret == kSplitFinish || ret == kSplitTwin)) {
        AcquireSRWLockExclusive(&g_lock);
        uint32_t& since = g_sinceFull[body];  // 0: never refreshed in full
        partial = since > 0 && since < kFullEvery && !g_overflow;
        if (partial) {
            thread_local std::vector<uint8_t*> touched;
            touched.clear();
            auto fix = [&](uint8_t* e) {
                if (Unflagged(e)) {
                    MakeStatic(e);
                    ++g_added;
                }
                touched.push_back(e);
            };
            for (uint8_t* e : g_recent)
                if (LiveShape(e) && Parent(e) == body) fix(e);
            for (uint8_t* e : g_created)
                if (LiveShape(e) && Parent(e) == body) fix(e);
            int guard = 0;
            for (uint8_t* e = FirstChild(body); e && IsShape(e) && Unflagged(e) && guard < 4096;
                 e = NextSibling(e), ++guard)
                fix(e);
            g_recent.swap(touched);
            if (g_recent.size() > 4096) g_recent.resize(4096);
            PruneCreated(body);
            ++since;
            uint64_t n = ++g_partial;
            int every = g_config.staticBodyUpdateValidate;
            if (every > 0 && n % uint64_t(every) == 0) Validate(body, since);
        } else {
            oUpdate(body);
            since = 1;
            ++g_full;
            g_recent.clear();
            if (g_overflow) {
                g_created.clear();  // (shapes of other static bodies too: their next refresh is a full one)
                g_sinceFull.clear();
                g_sinceFull[body] = 1;
                g_overflow = false;
            } else {
                PruneCreated(body);
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    } else {
        oUpdate(body);
    }
    trace::Emit(trace::kBodyUpdate, t0, trace::Now(),
                (body ? uint64_t(uint32_t(td::At<int>(body, 0x0c))) : 0) | (uint64_t(partial) << 63));
}

}  // namespace

bool InstallStaticBodyUpdate() {
    if (!td::CodeMatches(Addr::SplitNewBodyUpdate) || !td::CodeMatches(Addr::ShapeCtor)) {
        LOG("fix: static body update: code differs, not installed");
        return false;
    }
    // the two call sites must still be calls to it
    for (uintptr_t ret : {kSplitFinish, kSplitTwin}) {
        const uint8_t* call = reinterpret_cast<const uint8_t*>(td::Base() + ret - 5);
        int32_t rel;
        std::memcpy(&rel, call + 1, 4);
        if (call[0] != 0xe8 || td::Base() + ret + intptr_t(rel) != td::VA(Addr::SplitNewBodyUpdate)) {
            LOG("fix: static body update: the call at 0x%llx is not there, not installed",
                (unsigned long long)(0x140000000 + ret - 5));
            return false;
        }
    }
    MH_STATUS st = td::OnShapeCreated(&Created) ? MH_OK : MH_ERROR_NOT_CREATED;
    if (st == MH_OK)
        st = MH_CreateHook(reinterpret_cast<void*>(td::VA(Addr::SplitNewBodyUpdate)),
                           reinterpret_cast<void*>(&HkUpdate), reinterpret_cast<void**>(&oUpdate));
    LOG("fix: static body update: %s (validate every %d partial refreshes)", MH_StatusToString(st),
        g_config.staticBodyUpdateValidate);
    return st == MH_OK;
}

void LogStaticBodyUpdateStats() {
    LOG("static body update: %llu body refreshes; of static bodies after a split %llu full, %llu partial (%llu shapes "
        "given the static bits); %llu checked against the original, %llu mismatches",
        g_calls.load(), g_full.load(), g_partial.load(), g_added.load(), g_validated.load(), g_mismatch.load());
    AcquireSRWLockExclusive(&g_lock);
    g_sinceFull.clear();
    g_created.clear();
    g_recent.clear();
    g_overflow = false;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace fix
