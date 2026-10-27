// Engine addresses for one game build, generated into addresses_<build>.inc by tools/gen_addresses.py.
#pragma once
#include <cstdint>

namespace td {

enum class Addr : int {
#define TD_BUILD(stamp, size, ver)
#define TD_CODE(name, rva, label, ...) name,
#define TD_DATA(name, rva) name,
#include "addresses_2_1_0.inc"
#undef TD_BUILD
#undef TD_CODE
#undef TD_DATA
    Count
};

struct AddrInfo {
    const char* name;
    uint32_t rva;
    bool code;
    const char* label;  // timed-event name; empty for functions that are not timed
    uint8_t bytes[16];  // expected first bytes (code only)
};

extern const AddrInfo kAddrs[static_cast<int>(Addr::Count)];
extern const uint32_t kBuildStamp;
extern const uint32_t kBuildImageSize;
extern const char* const kBuildVersion;

uintptr_t Base();                     // load address of teardown.exe
bool BuildMatches();                  // the running exe is the build the table was made from
bool CodeMatches(Addr a);             // live bytes equal the recorded ones (false while still encrypted)

inline const AddrInfo& Info(Addr a) { return kAddrs[static_cast<int>(a)]; }
inline uintptr_t VA(Addr a) { return Base() + Info(a).rva; }
template <typename T> inline T Fn(Addr a) { return reinterpret_cast<T>(VA(a)); }

}  // namespace td
