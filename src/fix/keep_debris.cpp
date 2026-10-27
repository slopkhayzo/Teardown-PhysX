#include "fix/keep_debris.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "td/addresses.h"

namespace fix {
namespace {

// The size test of the rubble cleanup in 0x1400f0c30:
//   f0fc2 mov rcx,[rbx+0xf0] ; test rcx,rcx ; je skip ; mov eax,[rcx+8] ; add eax,[rcx+4] ; add eax,[rcx]
//   f0fda cmp eax,0x32 ; f0fdd jge skip (0x1400f153a, the next body)
constexpr uint32_t kSite = 0xf0fc2;
constexpr uint8_t kExpect[] = {0x48, 0x8b, 0x8b, 0xf0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x0f,
                               0x84, 0x68, 0x05, 0x00, 0x00, 0x8b, 0x41, 0x08, 0x03, 0x41, 0x04,
                               0x03, 0x01, 0x83, 0xf8, 0x32, 0x0f, 0x8d, 0x57, 0x05, 0x00};
constexpr uint32_t kLimit = 0xf0fdc;  // the 0x32 of cmp eax,0x32

}  // namespace

bool InstallKeepDebris() {
    const uintptr_t base = td::Base();
    if (std::memcmp(reinterpret_cast<const void*>(base + kSite), kExpect, sizeof(kExpect)) != 0) {
        LOG("fix: keep debris: code differs, not installed");
        return false;
    }
    auto* at = reinterpret_cast<uint8_t*>(base + kLimit);
    DWORD old = 0;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *at = 0x00;  // no piece is that small: the cleanup passes over every body
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    LOG("fix: keep debris installed (the rubble cleanup deletes no small pieces)");
    return true;
}

}  // namespace fix
