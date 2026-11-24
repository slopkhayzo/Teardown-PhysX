#include "fix/split_jobs.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "td/addresses.h"

namespace fix {
namespace {

// In split/finish 0x140141b60 (2.2.0.20261118; 0x140142470 in 2.2.0, 0x14013a200 in 2.1.0):
//   142581 call (voxels of the new bodies) ; 142586 cmp rax,0x4000 ; 14258c jae jobs
//   14258e movsxd rax,[rbp+0x3b0] (new bodies) ; 142595 cmp eax,0x10 ; 142598 jge jobs
constexpr uint32_t kSite = 0x142586;
constexpr uint8_t kExpect[] = {0x48, 0x3d, 0x00, 0x40, 0x00, 0x00, 0x73, 0x44, 0x48, 0x63, 0x85,
                               0xb0, 0x03, 0x00, 0x00, 0x83, 0xf8, 0x10, 0x7d, 0x38};
constexpr uint32_t kMinBodies = 0x142597;  // the 0x10 of cmp eax,0x10

}  // namespace

bool InstallSplitJobsMin(int bodies) {
    const uintptr_t base = td::Base();
    if (std::memcmp(reinterpret_cast<const void*>(base + kSite), kExpect, sizeof(kExpect)) != 0) {
        LOG("fix: split jobs: code differs, not installed");
        return false;
    }
    auto* at = reinterpret_cast<uint8_t*>(base + kMinBodies);
    DWORD old = 0;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
    *at = uint8_t(std::clamp(bodies, 1, 127));  // a signed 8-bit immediate
    VirtualProtect(at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    LOG("fix: split jobs from %d new bodies (the game: 16)", int(*at));
    return true;
}

}  // namespace fix
