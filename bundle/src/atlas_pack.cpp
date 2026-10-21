#include "atlas_pack.h"

#include <algorithm>
#include <climits>
#include <utility>

/// See atlas_pack.h. The MaxRects free-list algorithm (Jukka Jylanki, "A Thousand Ways to Pack
/// the Bin"): every placement splits the free rectangles it overlaps into up to four maximal
/// leftovers; contained free rectangles are pruned. Best short side fit picks, per input, the
/// free rectangle whose shorter leftover is smallest.

namespace xt::bundle {

namespace {

struct FreeRect {
    int x, y, w, h;
};

class MaxRects {
public:
    MaxRects(int width, int height) { m_free.push_back(FreeRect{0, 0, width, height}); }

    /// Places a `w` x `h` rectangle; false when nothing fits.
    bool insert(int w, int h, int& outX, int& outY) {
        int bestShort = INT_MAX;
        int bestLong = INT_MAX;
        size_t best = m_free.size();
        for (size_t i = 0; i < m_free.size(); ++i) {
            const FreeRect& f = m_free[i];
            if (f.w < w || f.h < h) {
                continue;
            }
            const int leftoverW = f.w - w;
            const int leftoverH = f.h - h;
            const int shortSide = std::min(leftoverW, leftoverH);
            const int longSide = std::max(leftoverW, leftoverH);
            if (shortSide < bestShort || (shortSide == bestShort && longSide < bestLong)) {
                bestShort = shortSide;
                bestLong = longSide;
                best = i;
            }
        }
        if (best == m_free.size()) {
            return false;
        }
        const FreeRect placed{m_free[best].x, m_free[best].y, w, h};
        outX = placed.x;
        outY = placed.y;
        split(placed);
        prune();
        return true;
    }

private:
    void split(const FreeRect& used) {
        std::vector<FreeRect> next;
        for (const FreeRect& f : m_free) {
            if (used.x >= f.x + f.w || used.x + used.w <= f.x || used.y >= f.y + f.h ||
                used.y + used.h <= f.y) {
                next.push_back(f);  // No overlap.
                continue;
            }
            if (used.x > f.x) {
                next.push_back(FreeRect{f.x, f.y, used.x - f.x, f.h});
            }
            if (used.x + used.w < f.x + f.w) {
                next.push_back(FreeRect{used.x + used.w, f.y, f.x + f.w - (used.x + used.w), f.h});
            }
            if (used.y > f.y) {
                next.push_back(FreeRect{f.x, f.y, f.w, used.y - f.y});
            }
            if (used.y + used.h < f.y + f.h) {
                next.push_back(FreeRect{f.x, used.y + used.h, f.w, f.y + f.h - (used.y + used.h)});
            }
        }
        m_free = std::move(next);
    }

    static bool contains(const FreeRect& a, const FreeRect& b) {
        return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
    }

    void prune() {
        for (size_t i = 0; i < m_free.size(); ++i) {
            for (size_t j = i + 1; j < m_free.size();) {
                if (contains(m_free[j], m_free[i])) {
                    m_free.erase(m_free.begin() + static_cast<std::ptrdiff_t>(i));
                    --i;
                    break;
                }
                if (contains(m_free[i], m_free[j])) {
                    m_free.erase(m_free.begin() + static_cast<std::ptrdiff_t>(j));
                } else {
                    ++j;
                }
            }
        }
    }

    std::vector<FreeRect> m_free;
};

int nextPow2(int v) {
    if (v > (1 << 30)) {
        return 1 << 30;  // T-258a: the next shift would reach the sign bit and loop forever.
    }
    int p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

/// Tries to place every input (already sorted) into one `w` x `h` page; returns how many fit
/// (the first `n` of `inputs`, in order) and their placements.
size_t tryPage(const std::vector<AtlasInput>& inputs, int w, int h, int padding,
               std::vector<AtlasPlacement>& out) {
    out.clear();
    MaxRects bin(w, h);
    size_t placed = 0;
    for (const AtlasInput& input : inputs) {
        int x = 0;
        int y = 0;
        if (!bin.insert(input.width + padding * 2, input.height + padding * 2, x, y)) {
            break;
        }
        out.push_back(AtlasPlacement{input.id, x + padding, y + padding, input.width, input.height});
        ++placed;
    }
    return placed;
}

}  // namespace

void packAtlas(const std::vector<AtlasInput>& inputs, int maxSize, int padding, std::vector<AtlasPage>& pages,
               std::vector<std::string>& tooLarge) {
    pages.clear();
    tooLarge.clear();
    padding = std::max(0, padding);
    maxSize = std::max(4, maxSize);
    std::vector<AtlasInput> pending;
    for (const AtlasInput& input : inputs) {
        if (input.width <= 0 || input.height <= 0 || input.width + padding * 2 > maxSize ||
            input.height + padding * 2 > maxSize) {
            tooLarge.push_back(input.id);
            continue;
        }
        pending.push_back(input);
    }
    std::stable_sort(pending.begin(), pending.end(), [](const AtlasInput& a, const AtlasInput& b) {
        const int sa = std::max(a.width, a.height);
        const int sb = std::max(b.width, b.height);
        if (sa != sb) {
            return sa > sb;
        }
        return a.width * a.height > b.width * b.height;
    });

    while (!pending.empty()) {
        // The smallest power-of-two page (w >= h, grown alternately) that takes every pending
        // input; at maxSize x maxSize, whatever fits becomes the page and the rest waits.
        int minSide = 1;
        for (const AtlasInput& input : pending) {
            minSide = std::max(minSide, nextPow2(std::max(input.width, input.height) + padding * 2));
        }
        int w = std::min(maxSize, minSide);
        int h = w;
        std::vector<AtlasPlacement> placements;
        size_t placed = 0;
        for (;;) {
            placed = tryPage(pending, w, h, padding, placements);
            if (placed == pending.size() || (w >= maxSize && h >= maxSize)) {
                break;
            }
            if (h < w) {
                h *= 2;
            } else {
                w *= 2;
            }
            w = std::min(w, maxSize);
            h = std::min(h, maxSize);
        }
        if (placed == 0) {
            for (const AtlasInput& input : pending) {
                tooLarge.push_back(input.id);
            }
            break;
        }
        // Trim the height to the used extent (rounded up to a power of two) when it left room.
        int usedH = 0;
        for (const AtlasPlacement& p : placements) {
            usedH = std::max(usedH, p.y + p.height + padding);
        }
        AtlasPage page;
        page.width = w;
        page.height = std::min(h, std::max(4, nextPow2(usedH)));
        page.entries = std::move(placements);
        pages.push_back(std::move(page));
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(placed));
    }
}

}  // namespace xt::bundle
