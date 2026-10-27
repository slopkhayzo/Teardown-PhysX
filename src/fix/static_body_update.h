// Engine fix: a split doesn't refresh the static body it cut pieces from ([Fix] StaticBodyUpdate, default on).
//
// After a split, split/finish (0x14013a200) and its twin in the breakage driver refresh every body they touched with
// 0x14002e180: collect the body's shapes (a walk of its child list, 0x14014fdd0) and run 0x14002e450 on them. For a
// dynamic body that recomputes the mass properties and the small-debris collision flags. For a static body it only
// sets every shape's static flags again (+0xc8: |1 &~2, and |4 &~8 for physical ones) and zeroes the body's mass
// fields, which a split doesn't change for the shapes the body keeps (it loses shapes to the new bodies). On Acacia
// Apartments the level's static body has 6,254 shapes and was walked on every carve: 13,463 times in the cutting
// phase of the acacia1 recording, 730 of the 766 ms all body refreshes took (54 us each, up to 0.8 ms).
// But a split also adds shapes to the static body (new shapes for the pieces of a carved static shape that still hold
// on), and those need the static flags. So calls from those two sites on a static body only give the static flags to
// the shapes Shape's constructor made since that ended up in the body (and to the ones it gave them last time, and
// to unflagged shapes at the list's head); the full original runs every 32nd time (the other 46 callers are
// untouched). Every Nth partial refresh ([Fix] StaticBodyUpdateValidate) runs the original too and compares every
// shape's flags before and after.
#pragma once

#include <cstdint>

namespace fix {

bool InstallStaticBodyUpdate();
void LogStaticBodyUpdateStats();  // and forgets the bodies (at the end of a level)

}  // namespace fix
