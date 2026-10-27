// Engine object accessors for Teardown 2.1.0 (offsets: research/docs/re-notes.md).
#pragma once
#include <cstdint>

#include "td/addresses.h"

namespace td {

enum GameState : int { kNone = 0, kSplash = 1, kMenu = 2, kUi = 3, kLoading = 4, kMenuLoading = 5, kPlay = 6,
                       kEdit = 7, kQuit = 8 };

inline const char* StateName(int s) {
    static const char* names[] = {"NONE", "SPLASH", "MENU", "UI", "LOADING", "MENU_LOADING", "PLAY", "EDIT",
                                  "QUIT"};
    return s >= 0 && s <= 8 ? names[s] : "unknown";
}

template <typename T> inline T& At(void* base, uintptr_t off) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}

inline uint8_t* GameObj() { return *reinterpret_cast<uint8_t**>(VA(Addr::GamePtr)); }
inline int State() { uint8_t* g = GameObj(); return g ? At<int>(g, 0x08) : -1; }
inline uint8_t* SceneObj() { uint8_t* g = GameObj(); return g ? At<uint8_t*>(g, 0x50) : nullptr; }
inline uint8_t* PhysicsObj() { uint8_t* s = SceneObj(); return s ? At<uint8_t*>(s, 0x08) : nullptr; }
inline float TickDt() { uint8_t* g = GameObj(); return g ? At<float>(g, 0x200) : 1.0f / 60.0f; }

// Scene lists are {int count; int capacity; T* data}.
template <typename T> inline T* SceneList(uintptr_t off, int& count) {
    count = 0;
    uint8_t* s = SceneObj();
    if (!s) return nullptr;
    uint8_t* v = At<uint8_t*>(s, off);
    if (!v) return nullptr;
    int n = At<int>(v, 0);
    if (n < 0 || n > 4000000) return nullptr;
    count = n;
    return At<T*>(v, 8);
}
inline int BodyCount() { int n; SceneList<uint8_t*>(0x230, n); return n; }
inline int ShapeCount() { int n; SceneList<uint8_t*>(0x238, n); return n; }
inline int SimulatedBodies() { uint8_t* p = PhysicsObj(); return p ? At<int>(p, 0xa2fd0) : 0; }
inline int DestroyedVoxels() { uint8_t* s = SceneObj(); return s ? At<int>(s, 0xa2c) : 0; }

struct Transform {
    float pos[3];
    float rot[4];  // x, y, z, w
};

inline bool CameraTransform(Transform& t) {
    uint8_t* g = GameObj();
    if (!g) return false;
    uint8_t* cam = At<uint8_t*>(g, 0x60);
    if (!cam) return false;
    const float* p = &At<float>(cam, 0x1d0);
    const float* q = &At<float>(cam, 0x1dc);
    for (int i = 0; i < 3; ++i) t.pos[i] = p[i];
    for (int i = 0; i < 4; ++i) t.rot[i] = q[i];
    return true;
}

// The local player: Game+0xb8 count, +0xc0 array, +0x168 local index. Fields: +0x570 transform (feet: pos, rot),
// +0x5fc velocity, +0x614 a second velocity, +0x640 eye transform (server), +0x678 eye transform (client copy),
// +0x6fc god mode, +0x700 pitch, +0x704 yaw, +0x708 "teleported", +0x7c8 health.
inline uint8_t* LocalPlayer() {
    uint8_t* g = GameObj();
    if (!g) return nullptr;
    int n = At<int>(g, 0xb8), i = At<int>(g, 0x168);
    uint8_t** a = At<uint8_t**>(g, 0xc0);
    return a && i >= 0 && i < n ? a[i] : nullptr;
}

// Shape fields.
inline uint16_t EntityType(uint8_t* e) { return At<uint16_t>(e, 0x08); }
inline uint16_t ShapeFlags(uint8_t* s) { return At<uint16_t>(s, 0xc8); }
inline const float* ShapeAabbMin(uint8_t* s) { return &At<float>(s, 0xa4); }
inline const float* ShapeAabbMax(uint8_t* s) { return &At<float>(s, 0xb0); }
inline uint8_t* ShapeVoxels(uint8_t* s) { return At<uint8_t*>(s, 0xf0); }
inline uint8_t* ShapeBody(uint8_t* s) { return At<uint8_t*>(s, 0x10); }
inline bool BodyDynamic(uint8_t* b) { return At<uint8_t>(b, 0x138) != 0; }

// Voxels: dims at +0, data at +0x10 (index x + dx*(y + dy*z), 0 = empty), voxel size in metres at +0x20.
inline const int* VoxDims(uint8_t* v) { return &At<int>(v, 0); }
inline const uint8_t* VoxData(uint8_t* v) { return At<uint8_t*>(v, 0x10); }
inline float VoxSize(uint8_t* v) { return At<float>(v, 0x20); }
inline uint64_t VoxVolume(uint8_t* v) {
    if (!v) return 0;
    const int* d = VoxDims(v);
    return uint64_t(uint32_t(d[0])) * uint32_t(d[1]) * uint32_t(d[2]);
}

}  // namespace td
