#include "td/shape_events.h"

#include <MinHook.h>
#include <intrin.h>

#include <atomic>

#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"

namespace td {
namespace {

// listeners are added while the plugin installs its hooks, before any is enabled: read without a lock
constexpr int kMaxListeners = 4;

template <typename Fn>
struct Listeners {
    Fn fns[kMaxListeners] = {};
    std::atomic<int> count{0};
    bool tried = false, hooked = false;

    bool Add(Fn fn, Addr addr, void* detour, void** original, const char* what) {
        if (!tried) {
            tried = true;
            if (!CodeMatches(addr)) {
                LOG("shape events: the %s's code differs, no events from it", what);
            } else {
                MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(VA(addr)), detour, original);
                hooked = st == MH_OK;
                if (!hooked) LOG("shape events: hook on the %s: %s", what, MH_StatusToString(st));
            }
        }
        const int n = count.load();
        if (!hooked || n >= kMaxListeners) return false;
        fns[n] = fn;
        count.store(n + 1, std::memory_order_release);
        return true;
    }
};

using ShapeCtorFn = uint8_t*(__fastcall*)(uint8_t* self, uint8_t* parent);
ShapeCtorFn oCtor = nullptr;
Listeners<ShapeCreatedFn> g_created;

uint8_t* __fastcall HkCtor(uint8_t* self, uint8_t* parent) {
    uint8_t* r = oCtor(self, parent);
    for (int i = 0, n = g_created.count.load(std::memory_order_acquire); i < n; ++i) g_created.fns[i](self);
    return r;
}

using DtorFn = void*(__fastcall*)(uint8_t* self, uint32_t flags);
DtorFn oBodyDtor = nullptr, oShapeDtor = nullptr;
Listeners<BodyDestroyedFn> g_destroyed;
Listeners<ShapeDestroyedFn> g_shapeDestroyed;

void* __fastcall HkBodyDtor(uint8_t* self, uint32_t flags) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    for (int i = 0, n = g_destroyed.count.load(std::memory_order_acquire); i < n; ++i) g_destroyed.fns[i](self, caller);
    return oBodyDtor(self, flags);
}

void* __fastcall HkShapeDtor(uint8_t* self, uint32_t flags) {
    for (int i = 0, n = g_shapeDestroyed.count.load(std::memory_order_acquire); i < n; ++i) g_shapeDestroyed.fns[i](self);
    return oShapeDtor(self, flags);
}

}  // namespace

bool OnShapeCreated(ShapeCreatedFn fn) {
    return g_created.Add(fn, Addr::ShapeCtor, reinterpret_cast<void*>(&HkCtor), reinterpret_cast<void**>(&oCtor),
                         "Shape constructor");
}

bool OnBodyDestroyed(BodyDestroyedFn fn) {
    return g_destroyed.Add(fn, Addr::BodyDtor, reinterpret_cast<void*>(&HkBodyDtor),
                           reinterpret_cast<void**>(&oBodyDtor), "Body destructor");
}

bool OnShapeDestroyed(ShapeDestroyedFn fn) {
    return g_shapeDestroyed.Add(fn, Addr::ShapeDtor, reinterpret_cast<void*>(&HkShapeDtor),
                                reinterpret_cast<void**>(&oShapeDtor), "Shape destructor");
}

}  // namespace td
