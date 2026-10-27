#include "core/crash.h"

#include <windows.h>

#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "trace/threads.h"

#pragma comment(lib, "dbghelp.lib")

namespace crash {

thread_local int t_tolerateFaults = 0;

namespace {

std::string g_dumpPath;
HANDLE g_mainThread = nullptr;
std::atomic<uint64_t> g_beat{0};
std::atomic<bool> g_watch{false};
LONG g_reported = 0;
int g_killAfter = 30;  // seconds; 0 = never
std::atomic<DWORD> g_ignored{0};

bool InPlugin(uint64_t addr) {
    HMODULE self = nullptr, at = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&InPlugin), &self);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(addr), &at);
    // PhysX's GPU module runs the plugin's scene: its faults are the plugin's too
    return at && (at == self || at == GetModuleHandleW(L"PhysXGpu_64.dll"));
}

void Describe(uint64_t addr, char* out, size_t n) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &m) &&
        m) {
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* base = std::strrchr(path, '\\');
        std::snprintf(out, n, "%s+%#llx", base ? base + 1 : path, addr - reinterpret_cast<uint64_t>(m));
    } else {
        std::snprintf(out, n, "%#llx", addr);
    }
}

void LogStack(CONTEXT ctx, const char* what) {
    for (int i = 0; i < 32 && ctx.Rip; ++i) {
        char where[300];
        Describe(ctx.Rip, where, sizeof(where));
        LOG("  %s #%02d %s", what, i, where);
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {  // leaf function: return address on top of the stack
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
        }
    }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* ep) {
    if (t_tolerateFaults > 0) return EXCEPTION_CONTINUE_SEARCH;  // the faulting code handles it
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case 0xC0000409:  // stack buffer overrun / fail fast
            break;
        default:
            return EXCEPTION_CONTINUE_SEARCH;
    }
    if (GetCurrentThreadId() == g_ignored.load()) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_reported) > 3) return EXCEPTION_CONTINUE_SEARCH;
    char where[300];
    Describe(uint64_t(ep->ExceptionRecord->ExceptionAddress), where, sizeof(where));
    const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
    LOG("CRASH: exception %08lx at %s, thread %lu%s%s %p", code, where, GetCurrentThreadId(),
        code == EXCEPTION_ACCESS_VIOLATION ? ", " : "",
        code == EXCEPTION_ACCESS_VIOLATION ? (info[0] == 0 ? "reading" : info[0] == 1 ? "writing" : "executing") : "",
        code == EXCEPTION_ACCESS_VIOLATION ? reinterpret_cast<void*>(info[1]) : nullptr);
    if (code != EXCEPTION_STACK_OVERFLOW) LogStack(*ep->ContextRecord, "at");
    logx::Flush();  // on disk before anything else can go wrong
    if (g_reported == 1 && !g_dumpPath.empty()) {
        HANDLE f = CreateFileA(g_dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
            BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                                        MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory |
                                                      MiniDumpWithThreadInfo),
                                        &mei, nullptr, nullptr);
            CloseHandle(f);
            LOG("CRASH: minidump %s %s", g_dumpPath.c_str(), ok ? "written" : "FAILED");
            logx::Flush();
        }
    }
    if (InPlugin(uint64_t(ep->ExceptionRecord->ExceptionAddress))) {
        LOG("CRASH: in the plugin: ending the game");
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), code);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI Watchdog(void*) {
    threads::Register(GetCurrentThreadId());
    uint64_t last = 0;
    int still = 0;
    bool reported = false;
    for (;;) {
        Sleep(1000);
        uint64_t b = g_beat.load();
        if (!g_watch.load() || b != last) {
            last = b;
            still = 0;
            reported = false;
            continue;
        }
        ++still;
        if (!g_mainThread) continue;
        if (still >= 5 && !reported) {
            reported = true;
            if (SuspendThread(g_mainThread) != DWORD(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(g_mainThread, &ctx)) {
                    LOG("HANG: the main thread has not finished a frame for %d s; its stack:", still);
                    LogStack(ctx, "main");
                }
                ResumeThread(g_mainThread);
            }
        }
        // A level frame that takes this long is a bug: don't keep the shared queue (and the machine) busy with it.
        if (g_killAfter > 0 && still >= g_killAfter) {
            LOG("HANG: still stuck after %d s, terminating the game", still);
            logx::Flush();
            TerminateProcess(GetCurrentProcess(), 4);
        }
    }
}

}  // namespace

void Install(const std::string& runDir, int hangKillSeconds) {
    g_dumpPath = runDir + "\\crash.dmp";
    g_killAfter = hangKillSeconds;
    AddVectoredExceptionHandler(1, OnException);
    HANDLE th = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

void IgnoreThread(unsigned long tid) { g_ignored.store(tid); }

void Heartbeat() {
    if (!g_mainThread)
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    g_beat.fetch_add(1, std::memory_order_relaxed);
}

void SetWatch(bool on) { g_watch.store(on); }

}  // namespace crash
