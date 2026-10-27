#include "trace/threads.h"

#include <windows.h>
#include <tlhelp32.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/config.h"
#include "core/log.h"
#include "trace/trace.h"

namespace threads {
namespace {

enum Group : int { kMain, kGame, kPhysx, kGraphics, kPlugin, kOther, kGroups };

struct Info {
    HANDLE h = nullptr;
    int group = kOther;
    uint64_t cycles = 0;
    uint32_t seen = 0;
};

std::atomic<DWORD> g_main{0};
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_set<DWORD> g_helpers;
bool g_started = false;

using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

bool Helper(DWORD tid) {
    AcquireSRWLockShared(&g_lock);
    bool h = g_helpers.count(tid) != 0;
    ReleaseSRWLockShared(&g_lock);
    return h;
}

// By the module the thread started in (ThreadQuerySetWin32StartAddress).
int Classify(DWORD tid, HANDLE h, QueryThreadFn query) {
    if (tid == g_main.load()) return kMain;
    if (Helper(tid)) return kPlugin;
    void* start = nullptr;
    if (!query || query(h, 9, &start, sizeof(start), nullptr) != 0 || !start) return kOther;
    HMODULE m = nullptr, self = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (!GetModuleHandleExW(flags, static_cast<LPCWSTR>(start), &m) || !m) return kOther;
    if (m == GetModuleHandleW(nullptr)) return kGame;
    GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&Classify), &self);
    if (m == self) return kPhysx;  // PhysX's dispatcher threads (the plugin's helpers are registered)
    wchar_t path[MAX_PATH] = L"";
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (!_wcsnicmp(name, L"nv", 2) || !_wcsnicmp(name, L"amd", 3) || !_wcsnicmp(name, L"d3d", 3) ||
        !_wcsnicmp(name, L"dxgi", 4) || !_wcsnicmp(name, L"dxcore", 6))
        return kGraphics;
    return kOther;
}

uint64_t Ticks(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

DWORD WINAPI Loop(void*) {
    Register(GetCurrentThreadId());
    auto query = reinterpret_cast<QueryThreadFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    std::string path = g_config.runDir + "\\threads.csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return 0;
    std::fputs("qpc,interval_ms,machine_busy_pct,cpus,main_ms,game_ms,game_n,physx_ms,physx_n,graphics_ms,"
               "plugin_ms,other_ms,other_n\n", f);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    std::unordered_map<DWORD, Info> known;
    uint64_t q0 = trace::Now(), tsc0 = __rdtsc();
    FILETIME i0, k0, u0;
    GetSystemTimes(&i0, &k0, &u0);
    const DWORD pid = GetCurrentProcessId();
    for (uint32_t round = 1;; ++round) {
        Sleep(1000);
        uint64_t q1 = trace::Now(), tsc1 = __rdtsc();
        double ms = double(q1 - q0) * 1000.0 / double(freq.QuadPart);
        double tscPerMs = ms > 0.0 ? double(tsc1 - tsc0) / ms : 1.0;  // thread cycle times count TSC cycles
        double groupMs[kGroups] = {};
        int groupN[kGroups] = {};
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                DWORD tid = te.th32ThreadID;
                auto it = known.find(tid);
                if (it == known.end()) {  // first sight: a baseline for the next round
                    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
                    if (!h) continue;
                    Info inf;
                    inf.h = h;
                    inf.group = Classify(tid, h, query);
                    ULONG64 c = 0;
                    QueryThreadCycleTime(h, &c);
                    inf.cycles = c;
                    inf.seen = round;
                    known.emplace(tid, inf);
                    continue;
                }
                Info& inf = it->second;
                if (tid == g_main.load())  // the main thread is known only after the first frame
                    inf.group = kMain;
                else if (inf.group == kPhysx && Helper(tid))
                    inf.group = kPlugin;
                ULONG64 c = 0;
                if (!QueryThreadCycleTime(inf.h, &c)) continue;
                double d = double(c - inf.cycles) / tscPerMs;
                inf.cycles = c;
                inf.seen = round;
                groupMs[inf.group] += d;
                groupN[inf.group] += d > 0.05;
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        for (auto it = known.begin(); it != known.end();) {
            if (it->second.seen != round) {
                CloseHandle(it->second.h);
                it = known.erase(it);
            } else {
                ++it;
            }
        }
        FILETIME i1, k1, u1;
        GetSystemTimes(&i1, &k1, &u1);
        uint64_t di = Ticks(i1) - Ticks(i0), dk = Ticks(k1) - Ticks(k0), du = Ticks(u1) - Ticks(u0);
        double busy = dk + du ? 100.0 * double(dk + du - di) / double(dk + du) : 0.0;  // kernel time includes idle
        std::fprintf(f, "%llu,%.1f,%.1f,%lu,%.1f,%.1f,%d,%.1f,%d,%.1f,%.1f,%.1f,%d\n", q1, ms, busy,
                     si.dwNumberOfProcessors, groupMs[kMain], groupMs[kGame], groupN[kGame], groupMs[kPhysx],
                     groupN[kPhysx], groupMs[kGraphics], groupMs[kPlugin], groupMs[kOther], groupN[kOther]);
        std::fflush(f);  // the game is killed, not closed
        q0 = q1;
        tsc0 = tsc1;
        i0 = i1;
        k0 = k1;
        u0 = u1;
    }
}

}  // namespace

void Start() {
    if (!g_config.threadsEnabled || g_started) return;
    g_started = true;
    HANDLE th = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

void SetMainThread(unsigned long tid) { g_main.store(tid); }

bool IsHelper(unsigned long tid) { return Helper(tid); }

void Register(unsigned long tid) {
    AcquireSRWLockExclusive(&g_lock);
    g_helpers.insert(tid);
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace threads
