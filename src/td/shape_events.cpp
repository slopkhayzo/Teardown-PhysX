#include "td/shape_events.h"

#include <MinHook.h>

#include <atomic>

#include "core/log.h"
#include "td/addresses.h"
#include "td/game.h"

namespace td {
namespace {

using ShapeCtorFn = uint8_t*(__fastcall*)(uint8_t* self, uint8_t* parent);
ShapeCtorFn oCtor = nullptr;
// listeners are added while the plugin installs its hooks, before any is enabled: read without a lock
constexpr int kMaxListeners = 4;
ShapeCreatedFn g_listeners[kMaxListeners] = {};
std::atomic<int> g_count{0};
bool g_tried = false, g_hooked = false;

uint8_t* __fastcall HkCtor(uint8_t* self, uint8_t* parent) {
    uint8_t* r = oCtor(self, parent);
    for (int i = 0, n = g_count.load(std::memory_order_acquire); i < n; ++i) g_listeners[i](self);
    return r;
}

}  // namespace

bool OnShapeCreated(ShapeCreatedFn fn) {
    if (!g_tried) {
        g_tried = true;
        if (!CodeMatches(Addr::ShapeCtor)) {
            LOG("shape events: the Shape constructor's code differs, no new-shape events");
        } else {
            MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(VA(Addr::ShapeCtor)), reinterpret_cast<void*>(&HkCtor),
                                         reinterpret_cast<void**>(&oCtor));
            g_hooked = st == MH_OK;
            if (!g_hooked) LOG("shape events: hook on the Shape constructor: %s", MH_StatusToString(st));
        }
    }
    const int n = g_count.load();
    if (!g_hooked || n >= kMaxListeners) return false;
    g_listeners[n] = fn;
    g_count.store(n + 1, std::memory_order_release);
    return true;
}

}  // namespace td
