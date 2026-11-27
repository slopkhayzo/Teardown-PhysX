// [Fix] SplitQueries: after a hole, the engine's shape-level connectivity search (split_per_body) floods a growing
// region with one scene query per shape it reaches (up to ~6,000 in one call on a big structure: ~55% of its time).
// Each query walks the two static broadphase trees from their roots. This serves the queries of that search (its first
// gather, 0x14013b010, and its flood, 0x140140880) from a mirror of the static trees' leaves in a hash grid instead.
//
// The mirror holds every leaf of the two static trees (physics+8, +0x10) with its exact box: built by walking the trees
// the first time it is needed, then kept by hooks on the trees' leaf insert, update and remove and on their clear
// (0x14052f5d0, 0x140530450, 0x14052f6a0, 0x14052f510 in 2.2.0.20261118). A served query returns the same shapes as the
// engine's (leaves whose box strictly overlaps the query box, of the asked body, passing the query's filter), in another
// order: new pieces can come out of a split in another order than in the unmodified game (the same pieces), so it
// stays off in multiplayer. Every Nth served query also runs the engine's and compares.
// Write-up: research/docs/destruction-stutter.md "Fix 5".
#pragma once

namespace fix {

bool InstallSplitQueries();
void SplitQueriesSetMainThread(unsigned long tid);
void SplitQueriesLevelStart();  // the level runs: walk the trees now
void SplitQueriesLevelChange();
void LogSplitQueriesStats();

}  // namespace fix
