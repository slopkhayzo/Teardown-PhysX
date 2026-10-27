// Engine option: the per-body setup after a split runs on worker threads for smaller splits too
// ([Fix] SplitJobsMin, default 16 = the game's own threshold).
//
// Split/finish (0x14013a200) gives every new body its setup (0x14002e180: its shapes, then mass properties over its
// voxels, ~36 us a body) inline on the main thread when a split made fewer than 16 bodies holding under 16,384
// voxels; bigger splits go to BreakageJobs on the workers (0x140133770, one job per worker), the same work for
// distinct bodies. A cutter or an explosion making a few bodies per call pays it all on the main thread. The
// option lowers the 16 (one byte of `cmp eax,0x10`).
#pragma once

namespace fix {

bool InstallSplitJobsMin(int bodies);

}  // namespace fix
