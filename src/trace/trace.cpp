#include "trace/trace.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>

#include "core/log.h"
#include "td/addresses.h"
#include "trace/threads.h"

namespace trace {

namespace {

constexpr uint64_t kCap = 1u << 15;  // records per thread ring (1 MiB)
constexpr int kMaxThreads = 512;

struct ThreadBuf {
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> drops{0};
    uint32_t tid = 0;
    Record rec[kCap];
};

std::atomic<bool> g_on{false};
std::atomic<bool> g_writerRun{false};
ThreadBuf* g_bufs[kMaxThreads];
std::atomic<int> g_bufCount{0};
std::mutex g_regMutex;
std::mutex g_drainMutex;
FILE* g_file = nullptr;
std::string g_runDir;
std::string g_names[256];
LARGE_INTEGER g_freq, g_start;
int64_t g_wallStartMs = 0;  // Unix time (ms, UTC) at g_start: lines other logs (gpu.csv) up with the trace
thread_local ThreadBuf* t_buf = nullptr;

ThreadBuf* MyBuf() {
    if (t_buf) return t_buf;
    void* mem = VirtualAlloc(nullptr, sizeof(ThreadBuf), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) return nullptr;
    auto* b = new (mem) ThreadBuf();
    b->tid = GetCurrentThreadId();
    std::lock_guard lock(g_regMutex);
    int n = g_bufCount.load();
    if (n >= kMaxThreads) return nullptr;
    g_bufs[n] = b;
    g_bufCount.store(n + 1, std::memory_order_release);
    t_buf = b;
    return b;
}

// Caller holds g_drainMutex (or is the only thread left). nolock: plain stdio calls would take the stream lock,
// which a thread killed at process exit may still hold.
void DrainLocked(bool nolock) {
    if (!g_file) return;
    auto write = [nolock](const void* p, size_t size, size_t count) {
        if (nolock) _fwrite_nolock(p, size, count, g_file);
        else std::fwrite(p, size, count, g_file);
    };
    int n = g_bufCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        ThreadBuf* b = g_bufs[i];
        uint64_t t = b->tail.load(std::memory_order_relaxed);
        uint64_t h = b->head.load(std::memory_order_acquire);
        if (h == t) continue;
        uint64_t first = t & (kCap - 1);
        uint64_t count = h - t;
        uint64_t part = count < kCap - first ? count : kCap - first;
        write(&b->rec[first], sizeof(Record), size_t(part));
        if (part < count) write(&b->rec[0], sizeof(Record), size_t(count - part));
        b->tail.store(h, std::memory_order_release);
        uint64_t d = b->drops.exchange(0);
        if (d) {
            uint64_t now = Now();
            Record r{now, now, kDrops, b->tid, d};
            write(&r, sizeof(r), 1);
        }
    }
}

void DrainOnce() {
    std::lock_guard lock(g_drainMutex);
    DrainLocked(false);
}

DWORD WINAPI WriterLoop(void*) {
    threads::Register(GetCurrentThreadId());
    int ticks = 0;
    while (g_writerRun.load()) {
        DrainOnce();
        if (++ticks % 50 == 0) {
            std::lock_guard lock(g_drainMutex);
            if (g_file) std::fflush(g_file);
        }
        Sleep(4);
    }
    return 0;
}

}  // namespace

uint64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

bool Start(const std::string& runDir) {
    g_runDir = runDir;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    g_wallStartMs = int64_t((uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000ull) - 11644473600000ll;
    std::string path = runDir + "\\trace.bin";
    g_file = std::fopen(path.c_str(), "wb");
    if (!g_file) {
        LOG("trace: cannot open %s", path.c_str());
        return false;
    }
    std::setvbuf(g_file, nullptr, _IOFBF, 1 << 20);
    struct Header {
        char magic[8];
        uint32_t version;
        uint32_t recordSize;
        uint64_t qpcFreq;
        uint64_t qpcStart;
    } h{{'T', 'D', 'P', 'X', 'T', 'R', 'C', '1'}, 1, sizeof(Record), uint64_t(g_freq.QuadPart),
        uint64_t(g_start.QuadPart)};
    std::fwrite(&h, sizeof(h), 1, g_file);
    g_writerRun = true;
    HANDLE th = CreateThread(nullptr, 0, WriterLoop, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    g_on = true;
    return true;
}

void RegisterName(uint32_t id, const char* name) {
    if (id < 256) g_names[id] = name;
}

void WriteMeta() {
    std::string path = g_runDir + "\\meta.json";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "{\n  \"format\": 1,\n  \"game_build\": \"%s\",\n  \"pe_timestamp\": %u,\n", td::kBuildVersion,
                 td::kBuildStamp);
    std::fprintf(f, "  \"qpc_freq\": %lld,\n  \"qpc_start\": %lld,\n  \"wall_start_ms\": %lld,\n  \"pid\": %lu,\n",
                 g_freq.QuadPart, g_start.QuadPart, g_wallStartMs, GetCurrentProcessId());
    g_names[kProfilerCategory] = "profiler_category";
    g_names[kStateSwitch] = "state_switch";
    g_names[kBenchHole] = "bench_hole";
    g_names[kBenchMarker] = "bench_marker";
    g_names[kDrops] = "trace_drops";
    if (g_names[kTouchTest].empty()) g_names[kTouchTest] = "shape_touch_test";
    g_names[kPxPrepare] = "px_prepare";
    g_names[kPxSync] = "px_sync";
    g_names[kPxStep] = "px_step";
    g_names[kPxCompare] = "px_compare";
    g_names[kPxWriteBack] = "px_writeback";
    if (g_names[kBodyUpdate].empty()) g_names[kBodyUpdate] = "split_newbody_update";
    std::fprintf(f, "  \"events\": {");
    bool first = true;
    for (int i = 0; i < 256; ++i) {
        if (g_names[i].empty()) continue;
        std::fprintf(f, "%s\n    \"%d\": \"%s\"", first ? "" : ",", i, g_names[i].c_str());
        first = false;
    }
    std::fprintf(f, "\n  },\n  \"profiler_categories\": [\"cpu_total\", \"destruction\", \"particle\", "
                    "\"path_planning\", \"physics\", \"rendering\", \"screen\", \"script_server\", \"script\", "
                    "\"water\"]\n}\n");
    std::fclose(f);
}

void Emit(uint32_t id, uint64_t t0, uint64_t t1, uint64_t arg) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    ThreadBuf* b = MyBuf();
    if (!b) return;
    uint64_t h = b->head.load(std::memory_order_relaxed);
    if (h - b->tail.load(std::memory_order_acquire) >= kCap) {
        b->drops.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    b->rec[h & (kCap - 1)] = Record{t0, t1, id, b->tid, arg};
    b->head.store(h + 1, std::memory_order_release);
}

void Flush() {
    DrainOnce();
    std::lock_guard lock(g_drainMutex);
    if (g_file) std::fflush(g_file);
}

void FinalDrain() {
    // At process exit Windows has already terminated the writer (possibly holding g_drainMutex or the stream
    // lock), so never block here: write without locks; we are the only thread left.
    g_on = false;
    g_writerRun = false;
    bool locked = g_drainMutex.try_lock();
    DrainLocked(true);
    if (g_file) {
        _fflush_nolock(g_file);
        _fclose_nolock(g_file);
        g_file = nullptr;
    }
    if (locked) g_drainMutex.unlock();
}

}  // namespace trace
