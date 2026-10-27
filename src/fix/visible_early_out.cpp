#include "fix/visible_early_out.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "td/addresses.h"

namespace fix {
namespace {

// The ray loop's tail in 0x1404de770, from the raycast call to the first instruction after the loop:
//   4ded24 call 0x1400e9920 ; 4ded29 test al,al ; 4ded2b jne 4ded32 ; 4ded2d minss xmm10,xmm9 (visible: nearest)
//   4ded32 inc edi ; add rbx,0xc ; cmp edi,[rbp-0x10] ; jl loop ; 4ded41 mov rbx,[rbp-0x40] (after the loop)
constexpr uint32_t kSite = 0x4ded24;
constexpr uint8_t kExpect[] = {0xe8, 0xf7, 0xab, 0xc0, 0xff, 0x84, 0xc0, 0x75, 0x05, 0xf3, 0x45, 0x0f, 0x5d,
                               0xd1, 0xff, 0xc7, 0x48, 0x83, 0xc3, 0x0c, 0x3b, 0x7d, 0xf0, 0x0f, 0x8c, 0xd3,
                               0xfe, 0xff, 0xff, 0x48, 0x8b, 0x5d, 0xc0};
constexpr uint32_t kPatch = 0x4ded2d;  // minss xmm10, xmm9 (5 bytes): becomes jmp stub
constexpr uint32_t kNext = 0x4ded32;   // the loop's increment
constexpr uint32_t kExit = 0x4ded41;   // after the loop

// Executable memory within +-2 GB of `target`, for a rel32 jump from there.
void* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t d = gran; d < 0x7ff00000; d += gran) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uintptr_t at = (target & ~(gran - 1)) + uintptr_t(intptr_t(sign) * intptr_t(d));
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) || mbi.State != MEM_FREE) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT,
                                       PAGE_EXECUTE_READWRITE))
                return p;
        }
    }
    return nullptr;
}

void AbsJump(uint8_t*& p, uintptr_t to) {  // jmp [rip+0]; dq to
    const uint8_t op[] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(p, op, sizeof(op));
    std::memcpy(p + sizeof(op), &to, sizeof(to));
    p += sizeof(op) + sizeof(to);
}

}  // namespace

bool InstallVisibleEarlyOut() {
    const uintptr_t base = td::Base();
    if (std::memcmp(reinterpret_cast<const void*>(base + kSite), kExpect, sizeof(kExpect)) != 0) {
        LOG("fix: visibility early-out: code differs, not installed");
        return false;
    }
    auto* stub = static_cast<uint8_t*>(AllocNear(base + kPatch, 64));
    if (!stub) {
        LOG("fix: visibility early-out: no memory near the code, not installed");
        return false;
    }
    uint8_t* p = stub;
    const uint8_t head[] = {
        0xf3, 0x45, 0x0f, 0x5d, 0xd1,                    // minss xmm10, xmm9 (the instruction replaced)
        0x48, 0x83, 0xbd, 0x00, 0x02, 0x00, 0x00, 0x00,  // cmp qword ptr [rbp+0x200], 0 (outDist)
        0x75, 0x0e,                                      // jne next (the caller wants the nearest distance)
    };
    std::memcpy(p, head, sizeof(head));
    p += sizeof(head);
    AbsJump(p, base + kExit);  // a visible point decides it
    AbsJump(p, base + kNext);
    FlushInstructionCache(GetCurrentProcess(), stub, size_t(p - stub));

    auto* site = reinterpret_cast<uint8_t*>(base + kPatch);
    int64_t rel = int64_t(reinterpret_cast<uintptr_t>(stub)) - int64_t(base + kPatch + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) return false;
    uint8_t jmp[5] = {0xe9};
    int32_t rel32 = int32_t(rel);
    std::memcpy(jmp + 1, &rel32, 4);
    DWORD old = 0;
    if (!VirtualProtect(site, sizeof(jmp), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(site, jmp, sizeof(jmp));
    VirtualProtect(site, sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(jmp));
    LOG("fix: visibility early-out installed (IsBodyVisible stops at the first visible point)");
    return true;
}

}  // namespace fix
