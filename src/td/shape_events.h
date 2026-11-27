// Engine object lifetimes as events, for the features that need them. MinHook can hook a function once, so the
// features share these hooks.
// - The game's Shape constructor (Addr::ShapeCtor, 0x1404dd9e0): every shape the game makes, from the level loader,
//   scripts' spawns and splits (these also on the split jobs' worker threads). A listener runs right after the
//   constructor returns, on that thread: the shape exists but has no voxels and no body yet.
// - The Body and Shape destructors (Addr::BodyDtor, Addr::ShapeDtor, the scalar deleting destructors): a listener runs
//   before the destructor, on the thread that destroys the object, the body's with the address it was called from.
#pragma once

#include <cstdint>

namespace td {

using ShapeCreatedFn = void (*)(uint8_t* shape);
using BodyDestroyedFn = void (*)(uint8_t* body, uintptr_t caller);
using ShapeDestroyedFn = void (*)(uint8_t* shape);

// Adds a listener (before MH_EnableHook(MH_ALL_HOOKS); the first one creates the hook). False when the function's
// code differs from the expected or the hook can't be made: no listener hears anything then.
bool OnShapeCreated(ShapeCreatedFn fn);
bool OnBodyDestroyed(BodyDestroyedFn fn);
bool OnShapeDestroyed(ShapeDestroyedFn fn);

}  // namespace td
