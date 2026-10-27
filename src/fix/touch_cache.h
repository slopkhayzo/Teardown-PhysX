// Track B fix 1: memoise the engine's shape-touch test (0x1400b1100).
//
// Splitting a multi-shape body (split_per_body, 0x1401385b0) flood-fills over "do these two shapes touch?" tests,
// and the laser re-runs it on the same structure every hole: in the Villa Gordon bench 27,869 tests covered only
// 5,339 distinct pairs, and the tests were 99% of the split time. The result depends only on both shapes' voxels
// and their relative transform, so it is cached per pair and reused while all of these still match: entity
// handles, voxel buffer, dims, solid-voxel count, relative transform, and no MakeHole touching either shape since.
// Every Nth hit is re-checked against the original (TouchCacheValidate) and mismatches are counted.
#pragma once
#include <cstdint>

namespace fix {

bool InstallTouchCache();                                     // typed hook; replaces the timing hook of the test
void SetMainThread(unsigned long tid);                         // cache only serves the main thread
void OnShapesModified(const uint8_t* entries, int count);      // breakage driver list: {Shape*, ...} stride 200
void OnLevelChange();                                          // drop everything (shapes are about to go)
void LogTouchCacheStats();

}  // namespace fix
