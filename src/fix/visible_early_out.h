// Engine fix: IsBodyVisible stops at the first visible point ([Fix] VisibleEarlyOut, default on).
//
// The shape visibility test 0x1404de770(shape, camPos, camDir, maxDist, float* outDist, filter) casts one ray from
// the camera to each of a shape's sample points (centre and box corners) and keeps the nearest unobstructed one;
// the shape is visible when there is any. Lua IsBodyVisible passes no outDist, so only "any" matters, yet every
// ray is cast: mods that test hundreds of bodies a frame (the Gluon Plasma Diffuser checks every body within 4 m of
// its hit, every frame) spend milliseconds on rays whose answer is already known. A 5-byte jump after the
// "unobstructed" branch leaves the loop at the first visible point when outDist is null; callers that ask for the
// distance get the original loop. The result is the same.
#pragma once

namespace fix {

bool InstallVisibleEarlyOut();

}  // namespace fix
