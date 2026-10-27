// Diagnostics for crashes and hangs inside the game process.
// - Fatal exceptions (access violation, stack overflow, ...): the faulting module+offset and an unwound stack go to
//   plugin.log and a minidump to <run>\crash.dmp. One in the plugin's own code ends the game right there (the game's
//   crash reporter froze on those); one in the game's code continues to the game's own handler.
// - Hangs: a watchdog thread checks a heartbeat ticked once per frame; when the main thread has not come back for
//   5 s in a level, its stack is logged (once per hang); after hangKillSeconds the game is terminated (exit code 4),
//   so a bug can't keep the shared queue busy.
#pragma once
#include <string>

namespace crash {

void Install(const std::string& runDir, int hangKillSeconds);  // InitializeASI
void Heartbeat();                         // main thread, once per frame
void SetWatch(bool on);                   // watch for hangs only while it makes sense (a level running)
void IgnoreThread(unsigned long tid);     // a thread that handles its own faults (the stack sampler)

// While above zero on a thread, faults there are left to the code's own __try/__except: reads of engine memory that
// may be stale by now (see shadow.cpp, Tolerant).
extern thread_local int t_tolerateFaults;

}  // namespace crash
