#include "trace/census.h"

#include <windows.h>

#include <cstdio>

#include "core/log.h"
#include "td/game.h"
#include "trace/trace.h"

namespace census {

namespace {

FILE* g_file = nullptr;
int g_ticks = 0;
uint64_t g_start = 0;
double g_freq = 0;

// Bins of solid voxels per awake free body.
constexpr int kBins = 6;
constexpr unsigned kBinTop[kBins] = {64, 512, 4096, 32768, 262144, 0xffffffffu};

}  // namespace

void Start(const std::string& runDir) {
    g_file = std::fopen((runDir + "\\census.csv").c_str(), "w");
    if (!g_file) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = double(f.QuadPart);
    g_start = trace::Now();
    std::fprintf(g_file, "t_s,state,bodies,dynamic,awake,awake_free,awake_jointed,awake_shapes,awake_voxels,"
                         "free_lt64,free_lt512,free_lt4k,free_lt32k,free_lt256k,free_ge256k,awake_free_voxels\n");
    std::fflush(g_file);
}

void OnTick() {
    if (!g_file || ++g_ticks % 60 != 0) return;
    int n = 0;
    uint8_t** bodies = td::SceneList<uint8_t*>(0x230, n);
    if (!bodies) return;
    int dynamic = 0, awake = 0, awakeFree = 0, awakeJointed = 0, awakeShapes = 0;
    unsigned long long awakeVoxels = 0, freeVoxels = 0;
    int bins[kBins] = {};
    for (int i = 0; i < n; ++i) {
        uint8_t* b = bodies[i];
        if (!b || td::EntityType(b) != 1 || !td::At<uint8_t>(b, 0x138)) continue;
        ++dynamic;
        if (!td::At<uint8_t>(b, 0x140)) continue;
        ++awake;
        bool jointed = false;
        unsigned vox = 0;
        int shapes = 0;
        for (uint8_t* s = td::At<uint8_t*>(b, 0x28); s; s = td::At<uint8_t*>(s, 0x18)) {
            if (td::EntityType(s) != 2) continue;
            ++shapes;
            if (td::At<void*>(s, 0xf8)) jointed = true;
            if (uint8_t* v = td::ShapeVoxels(s)) vox += td::At<uint32_t>(v, 0x5c);
        }
        awakeShapes += shapes;
        awakeVoxels += vox;
        if (jointed) {
            ++awakeJointed;
            continue;
        }
        ++awakeFree;
        freeVoxels += vox;
        for (int k = 0; k < kBins; ++k)
            if (vox < kBinTop[k]) {
                ++bins[k];
                break;
            }
    }
    double t = double(trace::Now() - g_start) / g_freq;
    // written by the log writer thread: a stalled disk must not stall the game
    logx::Printf(g_file, "%.2f,%d,%d,%d,%d,%d,%d,%d,%llu,%d,%d,%d,%d,%d,%d,%llu\n", t, td::State(), n, dynamic,
                 awake, awakeFree, awakeJointed, awakeShapes, awakeVoxels, bins[0], bins[1], bins[2], bins[3],
                 bins[4], bins[5], freeVoxels);
}

}  // namespace census
