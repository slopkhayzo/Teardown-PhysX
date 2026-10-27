// Engine option: the rubble cleanup leaves debris alone ([Fix] KeepDebris, default off).
//
// The scene update (0x1400f0c30) checks 10 bodies a frame, round robin, and deletes a small piece of debris when it
// touches another shape (the voxel shape-touch test 0x1400b1100). Small means: one voxel shape, no joint, dims
// x+y+z < 50, the debris flag (+0x0a bit 3). Pieces within 5 m of a player's camera are kept, and so are pieces
// within 30 m inside a 45-degree cone around the view. So rubble vanishes when you look away or walk off, and
// pieces farther than 30 m vanish as soon as they touch something, even in plain view. With the option, the size
// test never passes (one byte: the 50 becomes 0), and every piece stays.
#pragma once

namespace fix {

bool InstallKeepDebris();

}  // namespace fix
