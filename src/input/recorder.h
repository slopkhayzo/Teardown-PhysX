// Input recording and replay (research/docs/test-harness-plan.md).
//
// Recording: from the first frame in PLAY, every keyboard/mouse window message the game receives (WM_KEY*, WM_CHAR,
// WM_*BUTTON*, WM_MOUSEWHEEL, WM_INPUT with its raw-mouse packet) is stored with the frame it arrived in, together
// with that frame's raw frame time (the game's frame-timer read in FrameUpdate), what the game's window-active
// check returned, and checkpoints: camera, body count, a hash of all body transforms, main-thread rand() calls.
// Replay: the same level is started in the same sandboxed profile; from the first PLAY frame the recorded frame
// times are returned to the game and the recorded messages are fed to the window procedure after the message
// pump, at the same frame. Real input is ignored and the game is kept "focused". Same frame times means the same
// ticks per frame and the same Lua tick(dt); the checkpoints show how faithful the replay is, frame by frame.
// Both: the CRT rand() (Lua math.random uses it) is seeded from the recording at level start and at frame 0, and
// the game's own srand(time(NULL)) is made deterministic. Inside worker jobs rand() draws from a job-local MSVC-LCG
// state seeded from (seed, session tick, job type), so a job's numbers no longer depend on which thread runs it
// (the broadphase tree optimisation picks random leaves this way). Each run writes <run dir>\track.csv.
#pragma once
#include <string>

namespace inputrec {

bool Install();                            // hooks; only in record/replay mode
bool LoadReplay(const std::string& path);  // reads the whole recording
void OnLevelStart();                       // main thread, right before the session starts the level
void OnStateSwitching(int oldState, int newState);  // before the game switches state (LOADING -> PLAY: scene init)
void ArmRecord();                          // the next frame in PLAY becomes frame 0 of the recording
void ArmReplay();                          // the next frame in PLAY becomes frame 0 of the replay
void OnFrameStart();                       // FrameUpdate entry, main thread
void OnTick();                             // SceneUpdate entry (each 60 Hz tick), main thread
void OnLeavePlay();                        // the game left PLAY (recording ends here)
void FlushAtExit();                        // process exit: finish the recording file
// The input messages the game gets, live while recording and as fed back while replaying (both through the same
// path), for plugin features that must behave the same in a replay (the debris tool's trigger).
using InputObserver = void (*)(unsigned msg, unsigned long long wp, long long lp);
void SetInputObserver(InputObserver fn);
bool Driving();                            // record or replay: input state comes from SetInputObserver

}  // namespace inputrec
