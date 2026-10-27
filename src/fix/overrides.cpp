#include "fix/overrides.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <unordered_map>

#include "core/config.h"
#include "core/log.h"

namespace fix {
namespace {

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn oCreateFileW = nullptr;
using GetAttrWFn = DWORD(WINAPI*)(LPCWSTR);
GetAttrWFn oGetAttrW = nullptr;  // (the original, once hooked: Lookup's own checks don't go through the hook)
std::wstring g_root;  // the override folder, ending in a backslash
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<std::wstring, std::wstring> g_known;  // lowercased path -> its override ("" = none)

// Where mods live: the workshop's content folder for Teardown (app 1167630), and local mods (Documents) or the
// game's own mods folder, both ...\teardown\mods\. What follows is <mod folder>\<path inside the mod>.
constexpr const wchar_t* kModRoots[] = {L"\\workshop\\content\\1167630\\", L"\\teardown\\mods\\"};

std::wstring Lookup(const wchar_t* name) {
    std::wstring path(name);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::wstring key = path;
    for (wchar_t& c : key) c = wchar_t(std::towlower(c));
    AcquireSRWLockShared(&g_lock);
    auto it = g_known.find(key);
    bool known = it != g_known.end();
    std::wstring repl = known ? it->second : std::wstring();
    ReleaseSRWLockShared(&g_lock);
    if (known) return repl;
    for (const wchar_t* root : kModRoots) {
        size_t at = key.find(root);
        if (at == std::wstring::npos) continue;
        std::wstring candidate = g_root + path.substr(at + std::wcslen(root));
        DWORD attr = (oGetAttrW ? oGetAttrW : &GetFileAttributesW)(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) repl = candidate;
        break;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_known.emplace(key, repl);
    ReleaseSRWLockExclusive(&g_lock);
    if (!repl.empty()) LOG("mods: %ls is read from %ls", path.c_str(), repl.c_str());
    return repl;
}

HANDLE WINAPI HkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                            DWORD flags, HANDLE templ) {
    const DWORD writes = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
    if (name && disposition == OPEN_EXISTING && !(access & writes)) {
        std::wstring repl = Lookup(name);
        if (!repl.empty()) return oCreateFileW(repl.c_str(), access, share, sa, disposition, flags, templ);
    }
    return oCreateFileW(name, access, share, sa, disposition, flags, templ);
}

// The game checks that a file exists before it loads some (a tool's prefab and the vox files it names): a file only
// the override folder has must exist for those checks too. The A versions take the ANSI code page, as Windows does.
using GetAttrExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using GetAttrAFn = DWORD(WINAPI*)(LPCSTR);
using GetAttrExAFn = BOOL(WINAPI*)(LPCSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using FindFirstAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
GetAttrExWFn oGetAttrExW = nullptr;
GetAttrAFn oGetAttrA = nullptr;
GetAttrExAFn oGetAttrExA = nullptr;
FindFirstAFn oFindFirstA = nullptr;

std::wstring Widen(const char* s) {
    const UINT cp = AreFileApisANSI() ? CP_ACP : CP_OEMCP;
    int n = MultiByteToWideChar(cp, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(std::max(n, 1)) - 1, L'\0');
    if (n > 1) MultiByteToWideChar(cp, 0, s, -1, w.data(), n);
    return w;
}

DWORD WINAPI HkGetAttrW(LPCWSTR name) {
    std::wstring repl = name ? Lookup(name) : std::wstring();
    return oGetAttrW(repl.empty() ? name : repl.c_str());
}
BOOL WINAPI HkGetAttrExW(LPCWSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    std::wstring repl = name ? Lookup(name) : std::wstring();
    return oGetAttrExW(repl.empty() ? name : repl.c_str(), level, info);
}
DWORD WINAPI HkGetAttrA(LPCSTR name) {
    std::wstring repl = name ? Lookup(Widen(name).c_str()) : std::wstring();
    return repl.empty() ? oGetAttrA(name) : oGetAttrW(repl.c_str());
}
BOOL WINAPI HkGetAttrExA(LPCSTR name, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    std::wstring repl = name ? Lookup(Widen(name).c_str()) : std::wstring();
    return repl.empty() ? oGetAttrExA(name, level, info) : oGetAttrExW(repl.c_str(), level, info);
}
// One file asked for by its full name (no wildcards): the override's, which has the same name.
HANDLE WINAPI HkFindFirstA(LPCSTR name, LPWIN32_FIND_DATAA data) {
    if (name && !std::strpbrk(name, "*?")) {
        std::wstring repl = Lookup(Widen(name).c_str());
        if (!repl.empty()) {
            int n = WideCharToMultiByte(CP_ACP, 0, repl.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string a(size_t(std::max(n, 1)) - 1, '\0');
            if (n > 1) WideCharToMultiByte(CP_ACP, 0, repl.c_str(), -1, a.data(), n, nullptr, nullptr);
            return oFindFirstA(a.c_str(), data);
        }
    }
    return oFindFirstA(name, data);
}

}  // namespace

bool InstallOverrides() {
    if (g_config.modOverrides.empty()) return true;
    int n = MultiByteToWideChar(CP_UTF8, 0, g_config.modOverrides.c_str(), -1, nullptr, 0);
    g_root.resize(size_t(std::max(n, 1)) - 1);
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, g_config.modOverrides.c_str(), -1, g_root.data(), n);
    std::replace(g_root.begin(), g_root.end(), L'/', L'\\');
    if (!g_root.empty() && g_root.back() != L'\\') g_root += L'\\';
    struct Api {
        const char* name;
        void* detour;
        void** original;
    } apis[] = {
        {"CreateFileW", reinterpret_cast<void*>(&HkCreateFileW), reinterpret_cast<void**>(&oCreateFileW)},
        {"GetFileAttributesW", reinterpret_cast<void*>(&HkGetAttrW), reinterpret_cast<void**>(&oGetAttrW)},
        {"GetFileAttributesExW", reinterpret_cast<void*>(&HkGetAttrExW), reinterpret_cast<void**>(&oGetAttrExW)},
        {"GetFileAttributesA", reinterpret_cast<void*>(&HkGetAttrA), reinterpret_cast<void**>(&oGetAttrA)},
        {"GetFileAttributesExA", reinterpret_cast<void*>(&HkGetAttrExA), reinterpret_cast<void**>(&oGetAttrExA)},
        {"FindFirstFileA", reinterpret_cast<void*>(&HkFindFirstA), reinterpret_cast<void**>(&oFindFirstA)},
    };
    MH_STATUS st = MH_OK;
    for (const Api& a : apis) {
        st = MH_CreateHookApi(L"kernelbase", a.name, a.detour, a.original);
        if (st != MH_OK) {
            LOG("mods: hook %s: %s", a.name, MH_StatusToString(st));
            break;
        }
    }
    if (st == MH_OK) st = MH_EnableHook(MH_ALL_HOOKS);
    LOG("mods: overrides from %s: %s", g_config.modOverrides.c_str(), MH_StatusToString(st));
    return st == MH_OK;
}

}  // namespace fix
