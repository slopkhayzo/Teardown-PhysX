#include "trace/gpu_kernels.h"

#include "core/config.h"
#include "core/log.h"

#if TDPX_CUPTI

#include <windows.h>

#include <cupti_activity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpukernels {
namespace {

using RegisterFn = CUptiResult(CUPTIAPI*)(CUpti_BuffersCallbackRequestFunc, CUpti_BuffersCallbackCompleteFunc);
using KindFn = CUptiResult(CUPTIAPI*)(CUpti_ActivityKind);
using FlushFn = CUptiResult(CUPTIAPI*)(uint32_t);
using NextFn = CUptiResult(CUPTIAPI*)(uint8_t*, size_t, CUpti_Activity**);
using ResultFn = CUptiResult(CUPTIAPI*)(CUptiResult, const char**);
RegisterFn pRegister = nullptr;
KindFn pEnable = nullptr, pDisable = nullptr;
FlushFn pFlush = nullptr;
NextFn pNext = nullptr;
ResultFn pResult = nullptr;

struct Sum {
    uint64_t ns = 0, calls = 0;
};
std::mutex g_mu;  // the window, filled from CUPTI's buffer callbacks
std::unordered_map<std::string, Sum> g_kernels;
uint64_t g_memNs = 0, g_memBytes = 0, g_memCalls = 0;
uint64_t g_upNs = 0, g_upBytes = 0, g_downNs = 0, g_downBytes = 0;  // to the GPU / back to the host
uint64_t g_first = UINT64_MAX, g_last = 0;  // the window's kernels on the GPU clock
bool g_on = false;

void CUPTIAPI BufferRequested(uint8_t** buffer, size_t* size, size_t* maxRecords) {
    constexpr size_t kSize = size_t(8) << 20;
    *buffer = static_cast<uint8_t*>(_aligned_malloc(kSize, 8));
    *size = *buffer ? kSize : 0;
    *maxRecords = 0;
}

void CUPTIAPI BufferCompleted(CUcontext, uint32_t, uint8_t* buffer, size_t, size_t valid) {
    if (buffer && valid && pNext) {
        std::lock_guard<std::mutex> lk(g_mu);
        CUpti_Activity* rec = nullptr;
        while (pNext(buffer, valid, &rec) == CUPTI_SUCCESS) {
            if (rec->kind == CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL || rec->kind == CUPTI_ACTIVITY_KIND_KERNEL) {
                const auto* k = reinterpret_cast<const CUpti_ActivityKernel9*>(rec);
                if (k->end <= k->start) continue;
                Sum& s = g_kernels[k->name ? k->name : "?"];
                s.ns += k->end - k->start;
                ++s.calls;
                g_first = std::min<uint64_t>(g_first, k->start);
                g_last = std::max<uint64_t>(g_last, k->end);
            } else if (rec->kind == CUPTI_ACTIVITY_KIND_MEMCPY) {
                const auto* m = reinterpret_cast<const CUpti_ActivityMemcpy6*>(rec);
                const uint64_t ns = m->end > m->start ? m->end - m->start : 0;
                g_memNs += ns;
                g_memBytes += m->bytes;
                ++g_memCalls;
                if (m->copyKind == CUPTI_ACTIVITY_MEMCPY_KIND_HTOD) {
                    g_upNs += ns;
                    g_upBytes += m->bytes;
                } else if (m->copyKind == CUPTI_ACTIVITY_MEMCPY_KIND_DTOH) {
                    g_downNs += ns;
                    g_downBytes += m->bytes;
                }
            }
        }
    }
    _aligned_free(buffer);
}

std::string CuptiPath() {
    char root[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("CUDA_PATH", root, MAX_PATH);
    std::string dir = (n > 0 && n < MAX_PATH ? std::string(root)
                                             : std::string("C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.8")) +
                      "\\extras\\CUPTI\\lib64\\";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "cupti64_*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    FindClose(h);
    return dir + fd.cFileName;
}

const char* Err(CUptiResult r) {
    const char* s = "?";
    if (pResult) pResult(r, &s);
    return s;
}

}  // namespace

void Start() {
    if (!g_config.gpuKernels || g_on) return;
    std::string path = CuptiPath();
    HMODULE m = path.empty() ? nullptr : LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        LOG("gpu kernels: CUPTI not found (%s): off", path.empty() ? "no cupti64_*.dll under CUDA_PATH" : path.c_str());
        return;
    }
    pRegister = reinterpret_cast<RegisterFn>(GetProcAddress(m, "cuptiActivityRegisterCallbacks"));
    pEnable = reinterpret_cast<KindFn>(GetProcAddress(m, "cuptiActivityEnable"));
    pDisable = reinterpret_cast<KindFn>(GetProcAddress(m, "cuptiActivityDisable"));
    pFlush = reinterpret_cast<FlushFn>(GetProcAddress(m, "cuptiActivityFlushAll"));
    pNext = reinterpret_cast<NextFn>(GetProcAddress(m, "cuptiActivityGetNextRecord"));
    pResult = reinterpret_cast<ResultFn>(GetProcAddress(m, "cuptiGetResultString"));
    if (!pRegister || !pEnable || !pDisable || !pFlush || !pNext) {
        LOG("gpu kernels: %s lacks the activity API: off", path.c_str());
        return;
    }
    CUptiResult r = pRegister(BufferRequested, BufferCompleted);
    if (r == CUPTI_SUCCESS) r = pEnable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
    if (r == CUPTI_SUCCESS) pEnable(CUPTI_ACTIVITY_KIND_MEMCPY);
    if (r != CUPTI_SUCCESS) {
        LOG("gpu kernels: CUPTI %s: off", Err(r));
        return;
    }
    g_on = true;
    LOG("gpu kernels: timing every CUDA kernel with %s", path.c_str());
}

void Report(unsigned ticks) {
    if (!g_on) return;
    pFlush(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED);  // the buffers so far, through BufferCompleted on this thread
    std::vector<std::pair<std::string, Sum>> top;
    uint64_t memNs, memBytes, memCalls, first, last, upNs, upBytes, downNs, downBytes;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        top.assign(g_kernels.begin(), g_kernels.end());
        g_kernels.clear();
        memNs = g_memNs;
        memBytes = g_memBytes;
        memCalls = g_memCalls;
        upNs = g_upNs;
        upBytes = g_upBytes;
        downNs = g_downNs;
        downBytes = g_downBytes;
        g_upNs = g_upBytes = g_downNs = g_downBytes = 0;
        first = g_first;
        last = g_last;
        g_memNs = g_memBytes = g_memCalls = 0;
        g_first = UINT64_MAX;
        g_last = 0;
    }
    if (top.empty()) return;
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    uint64_t total = 0, calls = 0;
    for (const auto& [name, s] : top) {
        total += s.ns;
        calls += s.calls;
    }
    const double t = double(std::max(1u, ticks));
    char line[2600];
    int len = std::snprintf(line, sizeof(line),
                            "gpu kernels over %u ticks: %.2f ms of kernels per tick (%.0f launches), span %.2f ms per "
                            "tick; copies %.2f ms per tick (%.1f MB, %.0f; to the GPU %.2f ms %.1f MB, back %.2f ms %.1f "
                            "MB); top:",
                            ticks, double(total) / 1e6 / t, double(calls) / t,
                            last > first ? double(last - first) / 1e6 / t : 0.0, double(memNs) / 1e6 / t,
                            double(memBytes) / 1e6 / t, double(memCalls) / t, double(upNs) / 1e6 / t,
                            double(upBytes) / 1e6 / t, double(downNs) / 1e6 / t, double(downBytes) / 1e6 / t);
    for (size_t k = 0; k < top.size() && k < 14 && len > 0 && len < int(sizeof(line)) - 160; ++k) {
        std::string name = top[k].first;
        if (name.size() > 70) name.resize(70);
        len += std::snprintf(line + len, sizeof(line) - size_t(len), "%s %s %.3f ms (%.1f)", k ? ";" : "",
                             name.c_str(), double(top[k].second.ns) / 1e6 / t, double(top[k].second.calls) / t);
    }
    LOG("%s", line);
}

void Stop() {
    if (!g_on) return;
    pDisable(CUPTI_ACTIVITY_KIND_CONCURRENT_KERNEL);
    pDisable(CUPTI_ACTIVITY_KIND_MEMCPY);
    pFlush(CUPTI_ACTIVITY_FLAG_FLUSH_FORCED);
    g_on = false;
}

}  // namespace gpukernels

#else  // built without the CUDA toolkit's CUPTI headers

namespace gpukernels {
void Start() {
    if (g_config.gpuKernels) LOG("gpu kernels: this build has no CUPTI support");
}
void Report(unsigned) {}
void Stop() {}
}  // namespace gpukernels

#endif
