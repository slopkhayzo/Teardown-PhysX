// Once a second, how much CPU each group of the game's threads used, and how busy the whole machine was
// ([Trace] Threads, default on): <run>/threads.csv, CPU milliseconds per second (1000 = one logical processor kept
// busy). Groups by where a thread started: the game's main thread, the game's own workers (teardown.exe), PhysX's
// workers (the plugin's module, minus its registered helpers), the graphics driver and runtime, the plugin's
// helpers, everything else. Answers whether the CPU has room to spare, and what PhysX takes of it.
#pragma once

namespace threads {

void Start();
void SetMainThread(unsigned long tid);
void Register(unsigned long tid);  // one of the plugin's own helper threads (trace writer, sampler, watchdog)
bool IsHelper(unsigned long tid);

}  // namespace threads
