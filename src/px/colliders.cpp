#include "px/colliders.h"

#include <algorithm>
#include <cstring>

namespace px {

namespace {

inline size_t Index(const int n[3], int x, int y, int z) {
    return size_t(x) + size_t(n[0]) * (size_t(y) + size_t(n[1]) * size_t(z));
}

// Greedy merge over a dense occupancy grid (1 = solid) of size m[0..2]; boxes in grid units.
bool Greedy(const std::vector<uint8_t>& occ, const int m[3], size_t maxBoxes, std::vector<VoxBox>& out) {
    std::vector<uint8_t> used(occ.size(), 0);
    auto free_ = [&](int x, int y, int z) {
        size_t i = Index(m, x, y, z);
        return occ[i] && !used[i];
    };
    for (int z = 0; z < m[2]; ++z)
        for (int y = 0; y < m[1]; ++y)
            for (int x = 0; x < m[0]; ++x) {
                if (!free_(x, y, z)) continue;
                int x2 = x + 1;
                while (x2 < m[0] && free_(x2, y, z)) ++x2;
                int y2 = y + 1;
                for (; y2 < m[1]; ++y2) {
                    bool ok = true;
                    for (int xx = x; xx < x2 && ok; ++xx) ok = free_(xx, y2, z);
                    if (!ok) break;
                }
                int z2 = z + 1;
                for (; z2 < m[2]; ++z2) {
                    bool ok = true;
                    for (int yy = y; yy < y2 && ok; ++yy)
                        for (int xx = x; xx < x2 && ok; ++xx) ok = free_(xx, yy, z2);
                    if (!ok) break;
                }
                for (int zz = z; zz < z2; ++zz)
                    for (int yy = y; yy < y2; ++yy)
                        std::memset(&used[Index(m, x, yy, zz)], 1, size_t(x2 - x));
                if (out.size() >= maxBoxes) return false;
                out.push_back(VoxBox{{x, y, z}, {x2, y2, z2}});
            }
    return true;
}

}  // namespace

bool DecomposeRegion(const VoxGrid& g, const int lo[3], const int hi[3], size_t maxBoxes, std::vector<VoxBox>& out) {
    int m[3];
    for (int k = 0; k < 3; ++k) m[k] = std::max(0, hi[k] - lo[k]);
    if (!g.data || !m[0] || !m[1] || !m[2]) return true;
    std::vector<uint8_t> occ(size_t(m[0]) * m[1] * m[2]);
    for (int z = 0; z < m[2]; ++z)
        for (int y = 0; y < m[1]; ++y) {
            const uint8_t* row = g.data + Index(g.n, lo[0], lo[1] + y, lo[2] + z);
            uint8_t* dst = &occ[Index(m, 0, y, z)];
            for (int x = 0; x < m[0]; ++x) dst[x] = row[x] != 0;
        }
    size_t first = out.size();
    bool ok = Greedy(occ, m, maxBoxes, out);
    for (size_t i = first; i < out.size(); ++i)
        for (int k = 0; k < 3; ++k) {
            out[i].lo[k] += lo[k];
            out[i].hi[k] += lo[k];
        }
    return ok;
}

int DecomposeShape(const VoxGrid& g, size_t budget, std::vector<VoxBox>& out, uint64_t maxCells) {
    const int lo[3] = {0, 0, 0};
    int f0 = 1;
    while (f0 < 64 && uint64_t((g.n[0] + f0 - 1) / f0) * ((g.n[1] + f0 - 1) / f0) * ((g.n[2] + f0 - 1) / f0) > maxCells)
        f0 *= 2;
    for (int f = f0; f <= 64; f *= 2) {
        out.clear();
        if (f == 1) {
            if (DecomposeRegion(g, lo, g.n, budget, out)) return 1;
            continue;
        }
        int m[3];
        for (int k = 0; k < 3; ++k) m[k] = (g.n[k] + f - 1) / f;
        // a coarse voxel is solid when at least half of the voxels it covers are (so boxes neither swell into the
        // neighbours nor shrink much: a conservative "any voxel" rule makes adjacent debris overlap)
        std::vector<uint32_t> cnt(size_t(m[0]) * m[1] * m[2], 0), tot(cnt.size(), 0);
        for (int z = 0; z < g.n[2]; ++z)
            for (int y = 0; y < g.n[1]; ++y) {
                const uint8_t* row = g.data + Index(g.n, 0, y, z);
                size_t base = Index(m, 0, y / f, z / f);
                for (int x = 0; x < g.n[0]; ++x) {
                    ++tot[base + size_t(x / f)];
                    if (row[x]) ++cnt[base + size_t(x / f)];
                }
            }
        std::vector<uint8_t> occ(cnt.size(), 0);
        for (size_t i = 0; i < occ.size(); ++i) occ[i] = tot[i] && cnt[i] * 2 >= tot[i];
        bool ok = Greedy(occ, m, budget, out);
        for (VoxBox& b : out)
            for (int k = 0; k < 3; ++k) {
                b.lo[k] *= f;
                b.hi[k] = std::min(b.hi[k] * f, g.n[k]);
            }
        if (ok) return f;
        if (m[0] == 1 && m[1] == 1 && m[2] == 1) break;
    }
    return 0;
}

uint64_t CountSolid(const VoxGrid& g, const int lo[3], const int hi[3]) {
    uint64_t n = 0;
    if (!g.data) return 0;
    for (int z = lo[2]; z < hi[2]; ++z)
        for (int y = lo[1]; y < hi[1]; ++y) {
            const uint8_t* row = g.data + Index(g.n, lo[0], y, z);
            for (int x = 0; x < hi[0] - lo[0]; ++x) n += row[x] != 0;
        }
    return n;
}

uint64_t HashRegion(const VoxGrid& g, const int lo[3], const int hi[3]) {
    uint64_t h = 1469598103934665603ull;
    if (!g.data) return h;
    for (int z = lo[2]; z < hi[2]; ++z)
        for (int y = lo[1]; y < hi[1]; ++y) {
            const uint8_t* row = g.data + Index(g.n, lo[0], y, z);
            int w = hi[0] - lo[0];
            // solid/empty is all collision cares about; 8 voxels per step
            int x = 0;
            for (; x + 8 <= w; x += 8) {
                uint64_t bits = 0;
                for (int i = 0; i < 8; ++i) bits |= uint64_t(row[x + i] != 0) << i;
                h = (h ^ bits) * 1099511628211ull;
            }
            for (; x < w; ++x) h = (h ^ uint64_t(row[x] != 0)) * 1099511628211ull;
        }
    return h;
}

}  // namespace px
