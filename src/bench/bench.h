// Session driver for [Session] Mode = bench | record | replay: skips the splash, starts [Level] by itself, then
//   bench:  synthetic "laser": carves HolesPerSecond holes (radii R0/R1/R2, like the Gluon Plasma Diffuser's
//           MakeHole(pos, 2, 1.9, 1.8)) at the first solid voxel along a ray sweeping across the camera's view;
//   record: hands over to the input recorder from the first PLAY frame until the game leaves PLAY;
//   replay: hands over to the input replayer from the first PLAY frame until the recording ends;
// and quits when done. Mode = browse (tools\browse.ps1) is free play from the main menu: each level start is logged
// ("level: ..."), and quitting terminates the game.
// The debris tool ([Tool], any mode, including your own play): while its key is held (middle mouse by default) it
// cuts what the camera aims at into loose pieces (Mode=fragment: a FragmentSize cube into PieceSize cubes, every
// FragmentTicks ticks) or drills it (Mode=spray). No particles or paint: a pure source of debris. The trigger comes
// from the input messages the recorder sees, so replays fire it at the same moments; AutoFire fires it by itself
// during a bench run (the pile benchmark).
#pragma once

namespace bench {

void Init();                              // after hooks are installed
void OnFrame();                           // main thread, start of each frame
void OnTick();                            // main thread, start of each 60 Hz scene update
void OnState(int oldState, int newState);
void Quit(bool ok);                       // flush the trace and end the game process

}  // namespace bench
