// Event trace: each thread writes fixed-size records into its own ring buffer; one writer thread drains them
// into <run>/trace.bin. tools/trace_report.py turns that into per-frame breakdowns and a Perfetto trace.
#pragma once
#include <cstdint>
#include <string>

namespace trace {

// Ids 1..199 are timed hooks (their names are registered at hook install); fixed ids below.
enum : uint32_t {
    kProfilerCategory = 200,  // arg = category | (float seconds bits << 32), t0 == t1
    kStateSwitch = 201,       // arg = old | (new << 32)
    kBenchHole = 202,         // t0..t1 = our MakeHole call; arg = voxels removed | (flags << 32)
    kBenchMarker = 203,       // arg = marker id (see bench.cpp)
    kDrops = 204,             // arg = records dropped by a full ring since the last report
    kTouchTest = 205,         // shape touch test via the cache: arg = pair id (62 bits) | bypass << 62 | hit << 63
    kPxPrepare = 206,         // PhysX shadow, before Physics::update: track bodies, colliders, static blocks
    kPxSync = 207,            // after Physics::update: copy native pre-step state into PhysX
    kPxStep = 208,            // PxScene simulate + fetchResults; arg = awake actors
    kPxCompare = 209,         // compare PhysX's step with the native one
    kPxWriteBack = 210,       // offload: PhysX results into the game's bodies; arg = bodies written
    kBodyUpdate = 211,        // 0x14002e180 via [Fix] StaticBodyUpdate: arg = body handle | skipped << 63
};

struct Record {
    uint64_t t0;
    uint64_t t1;
    uint32_t id;
    uint32_t tid;
    uint64_t arg;
};
static_assert(sizeof(Record) == 32);

bool Start(const std::string& runDir);     // opens trace.bin, starts the writer thread
void RegisterName(uint32_t id, const char* name);
void WriteMeta();                           // meta.json: names, QPC frequency, build
void Emit(uint32_t id, uint64_t t0, uint64_t t1, uint64_t arg);
void Flush();                               // synchronous drain + fflush (before a deliberate exit)
void FinalDrain();                          // from DllMain at process exit: other threads are gone
uint64_t Now();                             // QueryPerformanceCounter ticks

}  // namespace trace
