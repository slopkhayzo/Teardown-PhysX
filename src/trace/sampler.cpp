#include "trace/sampler.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "trace/threads.h"
#include "trace/trace.h"

namespace sampler {
namespace {

// A loaded module and its unwind table, read from memory. While the main thread is suspended the sampler takes no
// locks (the main thread may hold any of them): lookups use these copies, and RtlVirtualUnwind is lock-free.
struct Module {
    uintptr_t base = 0, end = 0;
    const RUNTIME_FUNCTION* pdata = nullptr;
    uint32_t count = 0;
    char name[64]{};
};
std::vector<Module> g_mods;  // sampler thread only
std::atomic<DWORD> g_mainTid{0};
std::atomic<bool> g_run{false};
HANDLE g_thread = nullptr;

void ReadModules() {
    HMODULE hm[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof(hm), &need)) return;
    std::vector<Module> mods;
    DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 1024);
    for (DWORD i = 0; i < n; ++i) {
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof(mi))) continue;
        Module m;
        m.base = uintptr_t(mi.lpBaseOfDll);
        m.end = m.base + mi.SizeOfImage;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (d.VirtualAddress && d.Size >= sizeof(RUNTIME_FUNCTION)) {
            m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + d.VirtualAddress);
            m.count = d.Size / sizeof(RUNTIME_FUNCTION);
        }
        GetModuleBaseNameA(GetCurrentProcess(), hm[i], m.name, sizeof(m.name));
        mods.push_back(m);
    }
    std::sort(mods.begin(), mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    g_mods.swap(mods);
    std::string path = g_config.runDir + "\\modules.txt";
    if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (const Module& m : g_mods) std::fprintf(f, "%llx %llx %s\n", uint64_t(m.base), uint64_t(m.end - m.base), m.name);
        std::fclose(f);
    }
}

const Module* FindModule(uintptr_t pc) {
    size_t lo = 0, hi = g_mods.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_mods[mid].base <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0) return nullptr;
    const Module& m = g_mods[lo - 1];
    return pc < m.end ? &m : nullptr;
}

const RUNTIME_FUNCTION* FindFunction(const Module& m, uintptr_t pc) {
    uint32_t rva = uint32_t(pc - m.base);
    size_t lo = 0, hi = m.count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m.pdata[mid].BeginAddress <= rva)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0) return nullptr;
    const RUNTIME_FUNCTION* f = &m.pdata[lo - 1];
    return rva < f->EndAddress ? f : nullptr;
}

// Return addresses from the innermost frame out. A function without unwind data is a leaf only as the innermost
// frame; anywhere else the walk stops (code we cannot unwind).
int Unwind(CONTEXT& c, uint64_t* out, int max) {
    int n = 0;
    out[n++] = c.Rip;
    while (n < max) {
        const Module* m = FindModule(c.Rip);
        if (!m) break;
        const RUNTIME_FUNCTION* rf = m->pdata ? FindFunction(*m, c.Rip) : nullptr;
        if (rf) {
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, c.Rip, const_cast<PRUNTIME_FUNCTION>(rf), &c, &handler,
                             &frame, nullptr);
        } else if (n == 1) {
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += 8;
        } else {
            break;
        }
        if (c.Rip < 0x10000) break;
        out[n++] = c.Rip;
    }
    return n;
}

int SafeUnwind(CONTEXT& c, uint64_t* out, int max) {
    __try {
        return Unwind(c, out, max);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;  // the innermost address is still good
    }
}

// [Trace] SamplerPhysX: PhysX's threads, those started in the plugin's module (its CPU dispatcher; the plugin's own
// helpers are registered and left out) or in PhysXGpu_64.dll. Looked for again every second.
struct Watched {
    DWORD tid = 0;
    HANDLE h = nullptr;
    ULONG64 cycles = 0;  // at its last look
    bool seen = false;
};
std::vector<Watched> g_watched;  // sampler thread only

using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

void FindPhysxThreads() {
    static auto query =
        reinterpret_cast<QueryThreadFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query) return;
    for (Watched& w : g_watched) w.seen = false;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId(), main = g_mainTid.load();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || te.th32ThreadID == main) continue;
            auto it = std::find_if(g_watched.begin(), g_watched.end(),
                                   [&](const Watched& w) { return w.tid == te.th32ThreadID; });
            if (it != g_watched.end()) {
                it->seen = true;
                continue;
            }
            if (threads::IsHelper(te.th32ThreadID)) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID);
            if (!h) continue;
            void* start = nullptr;
            const Module* m = query(h, 9, &start, sizeof(start), nullptr) == 0 && start  // ThreadQuerySetWin32StartAddress
                                  ? FindModule(uintptr_t(start))
                                  : nullptr;
            if (!m || (_stricmp(m->name, "TeardownPhysX.asi") != 0 && _stricmp(m->name, "PhysXGpu_64.dll") != 0)) {
                CloseHandle(h);
                continue;
            }
            Watched w;
            w.tid = te.th32ThreadID;
            w.h = h;
            QueryThreadCycleTime(h, &w.cycles);
            w.seen = true;
            g_watched.push_back(w);
        } while (Thread32Next(snap, &te));
    }
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    for (size_t k = 0; k < g_watched.size();) {  // gone
        if (!g_watched[k].seen) {
            CloseHandle(g_watched[k].h);
            g_watched[k] = g_watched.back();
            g_watched.pop_back();
        } else {
            ++k;
        }
    }
}

DWORD WINAPI Loop(void*) {
    crash::IgnoreThread(GetCurrentThreadId());  // SafeUnwind handles its own faults
    threads::Register(GetCurrentThreadId());
    while (g_run.load() && !g_mainTid.load()) Sleep(20);
    if (!g_run.load()) return 0;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                           g_mainTid.load());
    std::string path = g_config.runDir + "\\samples.bin";
    FILE* f = th ? std::fopen(path.c_str(), "wb") : nullptr;
    if (!f) {
        LOG("sampler: cannot start (thread %lu, %s)", g_mainTid.load(), path.c_str());
        if (th) CloseHandle(th);
        return 0;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    const int hz = std::clamp(g_config.samplerHz, 50, 4000);
    const int depth = std::clamp(g_config.samplerDepth, 4, 128);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    // Version 2: each record is the time, (thread id << 32) | frame count, the thread's CPU time it stands for (us:
    // its cycles since its last sample; the main thread is sampled whatever it does), then the return addresses.
    struct Header {
        char magic[8];
        uint32_t version, hz;
        uint64_t qpcFreq;
        uint64_t exeBase;
        uint32_t mainTid, depth;
    } h{{'T', 'D', 'P', 'X', 'S', 'M', 'P', '1'}, 2, uint32_t(hz), uint64_t(freq.QuadPart),
        uint64_t(uintptr_t(GetModuleHandleW(nullptr))), g_mainTid.load(), uint32_t(depth)};
    std::fwrite(&h, sizeof(h), 1, f);
    ReadModules();
    // thread cycle times count TSC cycles
    uint64_t q0 = trace::Now(), c0 = __rdtsc();
    Sleep(50);
    const double tscPerUs = double(__rdtsc() - c0) / (double(trace::Now() - q0) * 1e6 / double(freq.QuadPart));
    const bool physx = g_config.samplerPhysx;
    if (physx) FindPhysxThreads();
    LOG("sampler: main thread %lu at %d Hz, %d frames deep, %zu modules%s", g_mainTid.load(), hz, depth, g_mods.size(),
        physx ? ", PhysX's threads too" : "");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(10000000LL / hz);
    std::vector<uint64_t> rec(size_t(depth) + 3);
    uint64_t samples = 0, physxSamples = 0, lastFlush = trace::Now(), lastMods = lastFlush, lastFind = lastFlush;
    ULONG64 mainCycles = 0;
    QueryThreadCycleTime(th, &mainCycles);
    const double capUs = 2.0e6 / hz;  // a thread's sample stands for at most two intervals
    // Suspends a thread, unwinds it, resumes it and writes the record.
    auto take = [&](HANDLE h, DWORD tid, uint64_t t, double us) -> bool {
        if (SuspendThread(h) == DWORD(-1)) return false;
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int n = GetThreadContext(h, &c) ? SafeUnwind(c, rec.data() + 3, depth) : 0;
        ResumeThread(h);
        if (n <= 0) return true;
        rec[0] = t;
        rec[1] = (uint64_t(tid) << 32) | uint64_t(n);
        rec[2] = uint64_t(std::min(us, capUs) + 0.5);
        std::fwrite(rec.data(), sizeof(uint64_t), size_t(n) + 3, f);
        return true;
    };
    while (g_run.load()) {
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, 100);
        else
            Sleep(1);
        uint64_t t = trace::Now();
        ULONG64 mc = 0;
        QueryThreadCycleTime(th, &mc);
        if (!take(th, g_mainTid.load(), t, double(mc - mainCycles) / tscPerUs)) break;
        mainCycles = mc;
        ++samples;
        for (Watched& w : g_watched) {  // only those that ran since their last look
            ULONG64 c = 0;
            if (!QueryThreadCycleTime(w.h, &c) || double(c - w.cycles) < 5.0 * tscPerUs) continue;
            double us = double(c - w.cycles) / tscPerUs;
            w.cycles = c;
            if (take(w.h, w.tid, t, us)) ++physxSamples;
        }
        if (t - lastFlush > uint64_t(freq.QuadPart)) {  // the game is killed, not closed: keep the file current
            std::fflush(f);
            lastFlush = t;
        }
        if (t - lastMods > uint64_t(freq.QuadPart) * 10) {  // modules loaded since
            ReadModules();
            lastMods = trace::Now();
        }
        if (physx && t - lastFind > uint64_t(freq.QuadPart)) {
            FindPhysxThreads();
            lastFind = trace::Now();
        }
    }
    std::fclose(f);
    if (timer) CloseHandle(timer);
    CloseHandle(th);
    for (Watched& w : g_watched) CloseHandle(w.h);
    g_watched.clear();
    LOG("sampler: %llu samples of the main thread, %llu of PhysX's threads", samples, physxSamples);
    return 0;
}

}  // namespace

void Start() {
    if (!g_config.samplerEnabled || g_run.exchange(true)) return;
    g_thread = CreateThread(nullptr, 0, Loop, nullptr, 0, nullptr);
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

void SetMainThread(unsigned long tid) { g_mainTid.store(tid); }

void Stop() {
    if (!g_run.exchange(false) || !g_thread) return;
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

}  // namespace sampler
