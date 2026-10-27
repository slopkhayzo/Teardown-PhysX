// GPU time per CUDA kernel ([Trace] GpuKernels=1): a diagnostic that needs the CUDA toolkit's CUPTI (loaded at run
// time from %CUDA_PATH%\extras\CUPTI\lib64; nothing is shipped). Every kernel PhysX's GPU module launches is timed on
// the GPU and summed per kernel name, and Report() logs the busiest since the last report. In-process, unlike
// Nsight Systems, whose injection leaves MinHook no trampoline memory (the plugin then ends the game to protect the
// real profile).
#pragma once

namespace gpukernels {

void Start();                    // once a CUDA context exists (PhysX's GPU scene); no-op unless configured
void Report(unsigned ticks);     // logs the kernels since the last report, per tick, and starts a new window
void Stop();

}  // namespace gpukernels
