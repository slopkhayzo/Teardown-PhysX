#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace logx {

// Lines are formatted on the calling thread and written by a writer thread: a stalled disk (a Volume Shadow Copy
// snapshot froze writes for 5.5 s once) must not stall the game's main thread, which logs every second.
namespace {
struct Pending {
    FILE* file;
    std::string text;
};
FILE* g_file = nullptr;
LARGE_INTEGER g_start, g_freq;
std::mutex g_queueMutex;  // g_queue
std::condition_variable g_wake;
std::vector<Pending> g_queue;
std::mutex g_writeMutex;  // the writes themselves: the writer thread or a Flush
HANDLE g_writer = nullptr;
std::atomic<bool> g_stop{false};

void Push(FILE* f, std::string&& text) {
    {
        std::lock_guard lock(g_queueMutex);
        g_queue.push_back({f, std::move(text)});
    }
    g_wake.notify_one();
}

// Writes what is queued, in order, then flushes the files it wrote to (the game is killed, not closed: files stay
// current).
void WriteQueued() {
    std::vector<Pending> batch;
    {
        std::lock_guard lock(g_queueMutex);
        batch.swap(g_queue);
    }
    if (batch.empty()) return;
    std::lock_guard lock(g_writeMutex);
    FILE* touched[8]{};
    int nt = 0;
    for (const Pending& p : batch) {
        if (!p.file) continue;
        std::fwrite(p.text.data(), 1, p.text.size(), p.file);
        bool seen = false;
        for (int i = 0; i < nt; ++i) seen |= touched[i] == p.file;
        if (!seen && nt < 8) touched[nt++] = p.file;
        else if (!seen) std::fflush(p.file);
    }
    for (int i = 0; i < nt; ++i) std::fflush(touched[i]);
}

DWORD WINAPI Writer(void*) {
    while (!g_stop.load()) {
        {
            std::unique_lock lock(g_queueMutex);
            g_wake.wait_for(lock, std::chrono::milliseconds(200), [] { return !g_queue.empty() || g_stop.load(); });
        }
        WriteQueued();
    }
    return 0;
}

std::string Format(const char* fmt, va_list ap) {
    char buf[2048];
    va_list copy;
    va_copy(copy, ap);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n < 0) return {};
    if (size_t(n) < sizeof(buf)) return std::string(buf, size_t(n));
    std::string big(size_t(n) + 1, '\0');
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    big.resize(size_t(n));
    return big;
}
}  // namespace

void Open(const char* path) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_file = std::fopen(path, "w");
    if (g_file && !g_writer) g_writer = CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr);
}

// At process exit the writer thread is already gone, maybe in the middle of a write: its locks are not waited for.
void Close() {
    g_stop.store(true);
    std::unique_lock queue(g_queueMutex, std::try_to_lock);
    if (!queue) return;
    std::vector<Pending> batch;
    batch.swap(g_queue);
    queue.unlock();
    std::unique_lock write(g_writeMutex, std::try_to_lock);
    if (!write) return;
    for (const Pending& p : batch)
        if (p.file) std::fwrite(p.text.data(), 1, p.text.size(), p.file);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void Flush() { WriteQueued(); }

void Info(const char* fmt, ...) {
    if (!g_file) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
    char head[48];
    int hn = std::snprintf(head, sizeof(head), "%9.3f [%5lu] ", t, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    std::string line = Format(fmt, ap);
    va_end(ap);
    line.insert(0, head, size_t(hn > 0 ? hn : 0));
    line += '\n';
    Push(g_file, std::move(line));
}

void Printf(FILE* f, const char* fmt, ...) {
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::string text = Format(fmt, ap);
    va_end(ap);
    Push(f, std::move(text));
}

}  // namespace logx
