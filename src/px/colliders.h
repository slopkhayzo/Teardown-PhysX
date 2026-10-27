// Voxel grids → boxes (PhysX colliders need no cooking for boxes). Greedy 3D merge: grow a box along x, then y,
// then z while every voxel it would cover is solid and not yet covered. Pure code, no engine or PhysX types.
#pragma once
#include <cstdint>
#include <vector>

namespace px {

struct VoxBox {
    int lo[3];  // first voxel (inclusive)
    int hi[3];  // last voxel + 1
};

struct VoxGrid {
    const uint8_t* data = nullptr;  // x + nx*(y + ny*z), 0 = empty
    int n[3] = {};
};

// Boxes covering every solid voxel of the grid inside [lo, hi). Returns false if more than maxBoxes were needed
// (out then holds the first maxBoxes).
bool DecomposeRegion(const VoxGrid& g, const int lo[3], const int hi[3], size_t maxBoxes, std::vector<VoxBox>& out);

// A whole shape within a box budget: decomposes at full resolution, and while over budget at 2x, 4x, ... coarser
// voxels (a coarse voxel is solid when at least half of its voxels are). Boxes are in full-resolution voxel units.
// Huge grids start coarser, at the first factor that leaves at most maxCells coarse voxels.
// Returns the coarsening factor used (1 = exact), 0 if even the coarsest pass stayed over budget.
int DecomposeShape(const VoxGrid& g, size_t budget, std::vector<VoxBox>& out, uint64_t maxCells = 1ull << 21);

// Number of solid voxels (for a quick change check: the engine keeps its own count at Voxels+0x5c).
uint64_t CountSolid(const VoxGrid& g, const int lo[3], const int hi[3]);

// Cheap content hash of a region (detects carving inside a static block).
uint64_t HashRegion(const VoxGrid& g, const int lo[3], const int hi[3]);

}  // namespace px
