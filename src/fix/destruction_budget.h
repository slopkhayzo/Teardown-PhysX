// [Fix] DestructionBudgetMs: destruction spread over frames instead of all in the frame it happens in.
//
// Every hole goes through the engine's MakeHoleCore (0x14013f6c0 in 2.2.0.20261118): carve, breakage, the check for
// what breaks off, new bodies. It runs on the main thread (its heaviest passes on the job workers, which the main
// thread waits for), so a frame that makes many holes is long by all of them: in a collapse dozens of impact holes,
// with the debris tool its cuts on top, with a laser several of its holes at once. With this fix the holes made in a
// frame are timed, and once they took the budget, the holes that can wait do: they are queued and made at the start
// of the next ticks, oldest first, within each frame's budget (at least one a frame).
//
// What can wait: impact damage, the game's (Physics::update's call at 0x1400bacbe, one per body hit in the tick) and
// PhysX's (src/px/shadow.cpp); the debris tool's cuts (its own queue, src/bench/bench.cpp, waits while the frame's
// budget is spent); and holes made by scripts with Lua MakeHole when the script drops the count it returns (a call
// made as a statement, as the Gluon laser and most tools do; [Fix] DestructionBudgetScripts=0: never), seen from the
// game's Lua 5.1 call info. What never waits: a script's hole whose count the script uses, explosions and
// projectiles (their code reads the destroyed-voxel count right after). A single hole costs what it costs: the budget
// can only move whole holes.
//
// A waiting hole on a body keeps its place on that body (its spheres are stored in the body's frame) and is dropped if
// the body is destroyed meanwhile; a script's hole stays where the script put it. A hole is made regardless of the
// budget once it waited MaxWaitTicks ticks. Off in multiplayer. DestructionBudgetMs=0: off, MakeHoleCore isn't even
// hooked (the game's own behaviour).
#pragma once

#include <cstdint>

namespace fix {

bool InstallDestructionBudget();  // replaces the timed MakeHoleCore hook (hooks.cpp skips that one then)
bool DestructionBudgetOn();
void DestructionFrameStart();     // the frame starts (FrameUpdate): a new budget
void DestructionTick();           // a tick starts (before the debris tool and scripts): queued holes, oldest first
void DestructionLevelChange();    // the level ends: queued holes are dropped
// The plugin's own carvers: ms of this frame's budget left (a large number when off, or in multiplayer).
double DestructionBudgetLeftMs();
// Queues the plugin's own impact holes ({pos xyz, radii soft/medium/hard} x count, world space) on a body; false when
// it can't (off, multiplayer, not the main thread): carve them now then.
bool DestructionDefer(uint8_t* body, const void* spheres, int count, bool sound);
void LogDestructionBudgetStats();

}  // namespace fix
