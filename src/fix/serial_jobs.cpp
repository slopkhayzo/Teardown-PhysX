#include "fix/serial_jobs.h"

#include <windows.h>

#include <MinHook.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>

#include "core/config.h"
#include "core/log.h"
#include "td/addresses.h"

using td::Addr;

namespace serialjobs {

namespace {

using GetSystemInfoFn = void(WINAPI*)(LPSYSTEM_INFO);
using JobFn = void(__fastcall*)(uint8_t* job);
using EnqueueFn = void(__fastcall*)(void* managerRef, uint8_t* job);
using PopFn = uint8_t*(__fastcall*)(void* managerRef);
using SignalFn = char(__fastcall*)(void* managerRef, int n);
using IsWorkerFn = bool(__fastcall*)();
using CountFn = int(__fastcall*)();

GetSystemInfoFn oGetSystemInfo = nullptr;
JobFn oWait = nullptr;
std::atomic<uint64_t> g_helped{0};

void WINAPI HkGetSystemInfo(LPSYSTEM_INFO si) {
    oGetSystemInfo(si);
    uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    uintptr_t base = td::Base();
    if (si && ra > base && ra < base + td::kBuildImageSize) si->dwNumberOfProcessors = 2;  // one worker
}

// Job state at +0x08: 0 new, 2 queued, 3 running, 4 done. The queue holds a reference (+0x20).
void __fastcall HkWait(uint8_t* job) {
    volatile LONG* state = reinterpret_cast<volatile LONG*>(job + 8);
    if (*state == 4) return;
    void* manager = reinterpret_cast<void*>(td::VA(Addr::JobManagerPtr));
    if (*state == 0 && InterlockedCompareExchange(state, 2, 0) == 0) {
        td::Fn<EnqueueFn>(Addr::JobEnqueue)(manager, job);
        td::Fn<SignalFn>(Addr::JobSignal)(manager, 1);
    }
    if (td::Fn<IsWorkerFn>(Addr::JobIsWorker)()) {
        // The only worker waits for another job: run queued jobs here until that one is done.
        while (*state != 4) {
            uint8_t* j = td::Fn<PopFn>(Addr::JobPop)(manager);
            if (!j) {
                SwitchToThread();
                continue;
            }
            volatile LONG* js = reinterpret_cast<volatile LONG*>(j + 8);
            LONG seen = *js;
            if (InterlockedCompareExchange(js, 3, seen) == seen) {
                td::Fn<JobFn>(Addr::JobRun)(j);
                g_helped.fetch_add(1, std::memory_order_relaxed);
            }
            td::Fn<JobFn>(Addr::JobRelease)(j);
        }
        return;
    }
    // Any other thread: the worker runs it (the extra worker that would stand in is not woken).
    for (int spins = 0; *state != 4; ++spins) {
        if (spins < 64)
            YieldProcessor();
        else
            SwitchToThread();
    }
}

}  // namespace

void InstallEarly() {
    if (!g_config.serialJobs) return;
    MH_STATUS st = MH_CreateHookApi(L"kernel32", "GetSystemInfo", reinterpret_cast<void*>(&HkGetSystemInfo),
                                    reinterpret_cast<void**>(&oGetSystemInfo));
    if (st == MH_OK) st = MH_EnableHook(MH_ALL_HOOKS);
    LOG("serial jobs: GetSystemInfo hook %s (the game sees 2 processors: one job worker)", MH_StatusToString(st));
}

bool InstallEngine() {
    if (!g_config.serialJobs) return true;
    int workers = td::CodeMatches(Addr::JobWorkerCount) ? td::Fn<CountFn>(Addr::JobWorkerCount)() : -1;
    bool ok = td::CodeMatches(Addr::JobWait) && td::CodeMatches(Addr::JobEnqueue) && td::CodeMatches(Addr::JobPop) &&
              td::CodeMatches(Addr::JobSignal) && td::CodeMatches(Addr::JobRelease) &&
              td::CodeMatches(Addr::JobIsWorker) && td::CodeMatches(Addr::JobRun);
    MH_STATUS st = ok ? MH_CreateHook(reinterpret_cast<void*>(td::VA(Addr::JobWait)), reinterpret_cast<void*>(&HkWait),
                                      reinterpret_cast<void**>(&oWait))
                      : MH_ERROR_NOT_EXECUTABLE;
    LOG("serial jobs: job manager has %d worker thread(s); wait hook %s", workers, MH_StatusToString(st));
    if (workers != 1) LOG("serial jobs: WARNING expected exactly one worker; replays will not be deterministic");
    return st == MH_OK;
}

}  // namespace serialjobs
