#include "td/addresses.h"

#include <windows.h>

#include <cstring>

namespace td {

#define TD_BUILD(stamp, size, ver)
#define TD_CODE(name, rva, label, ...) {#name, rva, true, label, __VA_ARGS__},
#define TD_DATA(name, rva) {#name, rva, false, "", {}},
const AddrInfo kAddrs[static_cast<int>(Addr::Count)] = {
#include "addresses_2_1_0.inc"
};
#undef TD_BUILD
#undef TD_CODE
#undef TD_DATA

#define TD_BUILD(stamp, size, ver) const uint32_t kBuildStamp = stamp; const uint32_t kBuildImageSize = size; \
    const char* const kBuildVersion = ver;
#define TD_CODE(name, rva, label, ...)
#define TD_DATA(name, rva)
#include "addresses_2_1_0.inc"
#undef TD_BUILD
#undef TD_CODE
#undef TD_DATA

uintptr_t Base() {
    static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return base;
}

bool BuildMatches() {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base());
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base() + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp == kBuildStamp && nt->OptionalHeader.SizeOfImage == kBuildImageSize;
}

bool CodeMatches(Addr a) {
    const AddrInfo& i = Info(a);
    if (!i.code) return true;
    return std::memcmp(reinterpret_cast<const void*>(Base() + i.rva), i.bytes, sizeof(i.bytes)) == 0;
}

}  // namespace td
