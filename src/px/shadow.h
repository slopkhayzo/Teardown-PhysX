// PhysX next to the native physics (research/docs/physx-integration-plan.md).
//
// Every 60 Hz tick, around the engine's Physics::update:
//   before: dynamic bodies are scanned; each free body (no joints, at most [PhysX] MaxBodyVoxels solid voxels)
//           gets a PhysX dynamic actor made of boxes or a hull from its voxels, other dynamic bodies get kinematic
//           proxies, and the static world is built lazily as 32^3-voxel blocks of boxes near those bodies (rebuilt
//           when their voxels change).
//   after:  PhysX steps once (1/60 s).
//
// Shadow=1 (sync, Phase 1): PhysX actors start each tick from the native pre-step state; the result is compared
//          with what the native engine produced, per body class (resting / in contact / free flight).
// Shadow=2 (free, Phase 1): PhysX keeps simulating its own copy; measures what PhysX would cost on that debris.
// Shadow=3 (offload, Phase 2): PhysX owns free debris. The game is changed:
//   - owned bodies are kept asleep natively (sleep counter Body+0x140 = 0), so the native integrator, solver and
//     broadphase leave them alone; native movers that touch them see a static obstacle (the contact wake
//     0x1400adc50 is refused for them);
//   - before the step, whatever changed an owned body since the last write-back (scripts, tools, explosions,
//     wakes) is applied to its PhysX actor; impulses native code applies during the step are added after it;
//   - after the step, PhysX's pose and velocities are written into the body and its shapes' bounds and tree
//     nodes are updated (0x14002dbc0). Rendering, Lua, raycasts and saves read the body as before;
//   - a body goes back to the native engine (woken with PhysX's velocity) when the player grabs it, it gets a
//     joint, belongs to a vehicle, grows past the voxel limit, a native constraint takes it, or the watchdog
//     sees a bad state; it stays native for [PhysX] RegainTicks;
//   - Lua IsBodyActive answers from PhysX for owned bodies; a kinematic capsule follows the player.
// Costs are trace events (px_prepare, px_sync, px_step, px_compare, px_writeback); summaries go to plugin.log,
// <run>\shadow.csv and, in offload mode, <run>\offload.csv.
#pragma once
#include <cstdint>

namespace shadow {

bool Install();         // engine hooks (no-op unless [PhysX] Shadow is set)
void OnPhysicsPre();    // Physics::update entry, main thread
void OnPhysicsPost();   // Physics::update exit, main thread
void OnLeavePlay();     // release the PhysX world (level end, menu); offload hands every body back first
// The breakage driver is about to change these shapes (its list: {Shape*, ...} entries, 200 bytes apart): their
// static blocks are checked at the next tick instead of waiting for the round robin.
void OnShapesModified(const uint8_t* entries, int count);

}  // namespace shadow
