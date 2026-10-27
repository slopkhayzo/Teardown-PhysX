// Statistical profiler for the game's main thread ([Trace] Sampler=1): a thread suspends it SamplerHz times a
// second, unwinds its stack and appends the return addresses to <run>/samples.bin (loaded modules in
// <run>/modules.txt). tools/sample_report.py names them (the exe's functions and Lua API bindings, the plugin's map
// file) and sums the time per function and per Lua API call. With [Trace] SamplerPhysX=1 PhysX's threads are
// sampled too, each only when it ran since its last sample, weighted by the CPU time it used (--threads physx).
#pragma once

namespace sampler {

void Start();                          // from plugin init: the thread waits for SetMainThread
void SetMainThread(unsigned long tid); // the game's main thread, from the first frame
void Stop();

}  // namespace sampler
