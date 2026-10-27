// [Session] SerialJobs=1: every engine job runs on one worker thread, in queue order (determinism checks).
//
// With 15 workers the narrowphase files contacts into per-worker buffers (the record carries the worker's index),
// so contact order, and with it the sequential-impulse solve, depends on which worker took which pairs: two
// replays of the same input diverge once debris touches. With SerialJobs the job manager is created with one
// worker (GetSystemInfo reports 2 processors to teardown.exe), the extra worker that normally stands in for a
// blocked waiter is never woken, threads that wait for a job spin instead of handing over, and the worker itself,
// when it waits on another job, runs queued jobs until that one is done. Two serial replays are then identical;
// they are slower, and not comparable with a parallel recording beyond its inputs.
#pragma once

namespace serialjobs {

void InstallEarly();   // InitializeASI, before the game builds its job manager
bool InstallEngine();  // with the engine hooks, once .text is decrypted

}  // namespace serialjobs
