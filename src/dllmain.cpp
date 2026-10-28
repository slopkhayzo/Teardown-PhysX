// TeardownPhysX.asi, loaded by Ultimate ASI Loader (installed as dxgi.dll): PhysX simulates the game's free debris
// (src/px), fixes to the engine's destruction work (src/fix), and the development tools around them: a timing tracer,
// benchmarks, input record/replay. See research/docs/physx-integration-plan.md.
#include <windows.h>

#include <MinHook.h>

#include <ctime>
#include <filesystem>
#include <string>

#include "bench/bench.h"
#include "core/config.h"
#include "core/crash.h"
#include "core/log.h"
#include "core/version.h"
#include "fix/overrides.h"
#include "fix/serial_jobs.h"
#include "hooks/hooks.h"
#include "input/recorder.h"
#include "td/addresses.h"
#include "trace/census.h"
#include "trace/sampler.h"
#include "trace/threads.h"
#include "trace/trace.h"

namespace {

HMODULE g_module = nullptr;
std::string g_runDir;

std::string ModuleDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_module, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().string();
}

std::string MakeRunDir() {
    if (!g_config.runDir.empty()) return g_config.runDir;
    // not tracing: one folder, the last two sessions' logs (see InitializeASI)
    if (!g_config.traceEnabled && g_config.outDir.empty()) return g_config.moduleDir + "\\TeardownPhysX_logs";
    std::string base = g_config.outDir.empty() ? g_config.moduleDir + "\\TeardownPhysX_runs" : g_config.outDir;
    char stamp[32];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return base + "\\" + stamp + "-" + g_config.label;
}

bool EngineCodeMatches() {
    // ProfilerAdd's first instruction loads the Game pointer RIP-relative, so its bytes also pin GamePtr's RVA.
    return td::CodeMatches(td::Addr::FrameUpdate) && td::CodeMatches(td::Addr::ProfilerAdd);
}

double ProcessUptimeSeconds() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    auto u = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return double(u(now) - u(created)) / 1e7;
}

// With a sandbox configured, the redirect must be in place before the game first asks for LocalAppData.
// If the game's log in the real profile was written after this process started, we came too late: stop now,
// before the game's exit-time saves (mods.xml, mod lists) can touch the real profile.
void CheckSandboxInTime() {
    char real[MAX_PATH] = {};  // the environment variable, not the (now hooked) shell API
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", real, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::string log = std::string(real) + "\\Teardown\\log.txt";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    FILETIME created, exited, kernel, user;
    if (!GetFileAttributesExA(log.c_str(), GetFileExInfoStandard, &a) ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return;
    if (CompareFileTime(&a.ftLastWriteTime, &created) > 0) {
        LOG("sandbox: the game already wrote %s before the redirect was installed: terminating to protect the "
            "real profile", log.c_str());
        logx::Flush();
        TerminateProcess(GetCurrentProcess(), 91);
    }
}

DWORD WINAPI InitThread(void*) {
    // SteamStub decrypts .text shortly after start; hook only once the bytes we recorded are really there.
    for (int i = 0; i < 6000 && !EngineCodeMatches(); ++i) Sleep(10);
    if (!EngineCodeMatches()) {
        LOG("the game's code doesn't match Teardown %s, the build this version of the plugin is made for (a game "
            "update?): the plugin stays off and the game runs unmodified", td::kBuildVersion);
        return 0;
    }
    LOG("engine code decrypted; installing hooks");
    if (g_config.traceEnabled && !trace::Start(g_runDir)) LOG("trace disabled");
    if (g_config.traceEnabled) census::Start(g_runDir);
    if (g_config.traceEnabled) sampler::Start();
    if (g_config.traceEnabled) threads::Start();
    hooks::InstallEngineHooks();
    trace::WriteMeta();
    bench::Init();
    return 0;
}

}  // namespace

extern "C" __declspec(dllexport) void InitializeASI() {
    static bool started = false;
    if (started) return;
    started = true;

    g_config.moduleDir = ModuleDir();
    char env[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TDPX_INI", env, MAX_PATH);
    g_config.Load(n > 0 && n < MAX_PATH ? std::string(env) : g_config.moduleDir + "\\TeardownPhysX.ini");

    const bool logsFolder = !g_config.traceEnabled && g_config.runDir.empty() && g_config.outDir.empty();
    g_runDir = MakeRunDir();
    g_config.runDir = g_runDir;  // the resolved folder, for everything that writes run output
    std::error_code ec;
    std::filesystem::create_directories(g_runDir, ec);
    const std::string logPath = g_runDir + "\\plugin.log";
    if (logsFolder) MoveFileExA(logPath.c_str(), (g_runDir + "\\plugin.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    logx::Open(logPath.c_str());
    LOG("TeardownPhysX %s for Teardown %s, ini %s", TDPX_VERSION, td::kBuildVersion, g_config.iniPath.c_str());
    LOG("run dir %s", g_runDir.c_str());
    crash::Install(g_runDir, g_config.hangKillSeconds);
    {
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(td::Base());
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(td::Base() + dos->e_lfanew);
        LOG("loaded %.3f s after process start; exe header: timestamp %08x, image size %x (table: %08x, %x)",
            ProcessUptimeSeconds(), nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, td::kBuildStamp,
            td::kBuildImageSize);
    }
    if (MH_Initialize() != MH_OK) {
        LOG("MinHook init failed");
        if (!g_config.appDataRoot.empty()) {  // no redirect possible: see below
            LOG("sandbox: no LocalAppData redirect without hooks: terminating to protect the real profile");
            logx::Flush();
            TerminateProcess(GetCurrentProcess(), 92);
        }
        return;
    }
    if (!g_config.appDataRoot.empty()) {
        std::filesystem::create_directories(g_config.appDataRoot, ec);
        // Without the redirect the game would run on the real profile (under Nsight Systems MinHook could not
        // allocate its trampolines and a bench ran unsandboxed): stop before the game reads or writes anything there.
        if (!hooks::InstallApiHooks()) {
            LOG("sandbox: the LocalAppData redirect could not be installed: terminating to protect the real profile");
            logx::Flush();
            TerminateProcess(GetCurrentProcess(), 92);
        }
        CheckSandboxInTime();
    }
    serialjobs::InstallEarly();
    fix::InstallOverrides();  // before the game lists and loads mods
    HANDLE th = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // Process exit: other threads are gone; write out what the trace rings still hold.
        inputrec::FlushAtExit();
        trace::FinalDrain();
        logx::Close();
    }
    return TRUE;
}
