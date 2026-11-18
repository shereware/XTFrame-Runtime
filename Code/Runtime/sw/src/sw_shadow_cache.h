#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xt/box_shadow.h"
#include "xt/types.h"

/// T-724: the software renderer's cache of blurred DrawBoxShadow coverage (Docs/tasks/T-724.md
/// approach (c), with the main model's rules of 2026-11-15): a static shadow's per-pixel coverage is
/// kept between frames and read back instead of evaluating the §10.12 formula again.

namespace xt::sw {

/// Everything one shadow's coverage depends on: the resolved shapes (offset, spread and the radii
/// already applied), the Gaussian in local units, the full device transform (DPR, sub-pixel phase,
/// rotation) and the device rectangle the entry covers. The colour is applied after the coverage
/// and is not part of it. Plain 4-byte fields only (no padding), compared bit for bit.
struct ShadowCacheKey {
    BoxShadowShape shape{};
    BoxShadowShape extra[2]{};
    float extraSign[2] = {0, 0};
    uint32_t extraCount = 0;
    float sigma = 0.0f;
    Mat2x3 xf{};
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  ///< device pixels [x0, x1) x [y0, y1)
};

/// One cached rectangle of coverage: `cov[(y - y0) * w + (x - x0)]` is the coverage of device pixel
/// (x, y), or NaN while that pixel has not been evaluated yet (it is filled the first time a span
/// reaches it, with exactly the value the uncached path computes).
struct ShadowCoverage {
    float* cov = nullptr;
    int32_t x0 = 0, y0 = 0, w = 0, h = 0;
};

class SwShadowCache {
public:
    /// The main model's budget: 16 MiB of coverage per renderer instance.
    static constexpr size_t kDefaultBudgetBytes = size_t{16} << 20;
    /// Entries unused for this many consecutive frames are dropped.
    static constexpr uint32_t kMaxIdleFrames = 120;
    /// Keys remembered per frame for the "seen in the previous frame" rule; a frame with more
    /// blurred shadows than this stores none of the extra ones.
    static constexpr size_t kMaxSeenPerFrame = 256;

    /// The coverage for `key`: a hit returns the stored entry; a miss stores a new (unevaluated)
    /// entry when the same key was drawn in the previous frame and it fits the budget (least
    /// recently used entries are evicted first), else returns an empty ShadowCoverage (cov null:
    /// evaluate uncached). Allocates nothing on a hit.
    ShadowCoverage acquire(const ShadowCacheKey& key);
    /// Ends one render(): ages the entries (dropping the idle ones) and makes this frame's keys the
    /// "previous frame".
    void endFrame();
    /// Frees every entry and the remembered keys (releaseScratch, tests).
    void clear();

    void setBudget(size_t bytes) { m_budget = bytes; }
    size_t bytes() const { return m_bytes; }
    size_t entries() const { return m_entries.size(); }
    /// Totals since construction / clear(): acquire() calls that hit, that stored a new entry.
    uint64_t hits() const { return m_hits; }
    uint64_t stores() const { return m_stores; }

private:
    struct Entry {
        ShadowCacheKey key;
        uint64_t hash = 0;
        uint32_t lastUsed = 0;
        std::vector<float> cov;
    };
    struct Seen {
        ShadowCacheKey key;
        uint64_t hash = 0;
    };

    static uint64_t hashKey(const ShadowCacheKey& key);
    static bool sameKey(const ShadowCacheKey& a, const ShadowCacheKey& b);
    static size_t entryBytes(const ShadowCacheKey& key);
    void removeAt(size_t i);

    std::vector<Entry> m_entries;
    std::vector<Seen> m_prevSeen;
    std::vector<Seen> m_curSeen;
    size_t m_budget = kDefaultBudgetBytes;
    size_t m_bytes = 0;
    uint32_t m_frame = 0;
    uint64_t m_hits = 0;
    uint64_t m_stores = 0;
};

}  // namespace xt::sw

namespace xt::sw {

/// T-724: the key of a blurred shadow `d` drawn under `deviceXf` whose quad has the device corners
/// `quad`, on a target covering device pixels [tx0, tx1) x [ty0, ty1): the quad's pixel bounding box
/// clipped to the target is the cached rectangle. False when that rectangle is empty or a corner
/// is not finite (the shadow is then drawn uncached).
bool makeShadowCacheKey(const BoxShadowDraw& d, const Mat2x3& deviceXf, const Vec2 quad[4], int tx0, int ty0,
                        int tx1, int ty1, ShadowCacheKey& out);

}  // namespace xt::sw
