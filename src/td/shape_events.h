// The game's Shape constructor (Addr::ShapeCtor, 0x1404dd9e0) as an event: every shape the game makes, from the
// level loader, scripts' spawns and splits (these also on the split jobs' worker threads). A listener runs right
// after the constructor returns, on that thread: the shape exists but has no voxels and no body yet.
// MinHook can hook a function once, so the features that want new shapes share this hook.
#pragma once

#include <cstdint>

namespace td {

using ShapeCreatedFn = void (*)(uint8_t* shape);

// Adds a listener (before MH_EnableHook(MH_ALL_HOOKS); the first one creates the hook). False when the constructor's
// code differs from the expected or the hook can't be made: no listener hears anything then.
bool OnShapeCreated(ShapeCreatedFn fn);

}  // namespace td
