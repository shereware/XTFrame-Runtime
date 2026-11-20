#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xt/command_list.h"
#include "xt/types.h"

/// T-737 (Docs/tasks/T-737.md A): the Painter's cache of `dashed` / `dotted` border geometry. One
/// entry is the output of one appendBorderDashes() call (painter_element.cpp) -- the Move / Line /
/// Close contours of one side, in the element's own frame (relative to its border box's origin, so
/// identical elements anywhere share an entry) -- keyed by every input of that call, compared bit
/// for bit. A hit appends exactly the stored verbs and points, so the command list is byte-identical
/// to an uncached paint. One cache per Painter (never static or global).

namespace xt {

/// appendBorderDashes()'s working vectors, kept (cleared, capacity retained) across calls so a
/// recomputed side allocates nothing once warm. The computation and its results are unchanged.
struct BorderDashScratch {
    std::vector<PathVerb> lineVerbs;
    std::vector<float> linePoints;
    std::vector<Vec2> flat;
    std::vector<uint32_t> flatStarts;
    std::vector<uint8_t> flatClosed;
    std::vector<Vec2> dashed;
    std::vector<uint32_t> dashedStarts;
    std::vector<uint8_t> dashedClosed;
    std::vector<uint8_t> dashOpen;
    std::vector<Vec2> fill;
    std::vector<uint32_t> fillStarts;
    std::vector<Vec2> poly;
    std::vector<Vec2> half;
    std::vector<Vec2> clipped;
};

class BorderDashCache {
public:
    /// The key's 4-byte words: the 36 floats of BorderCorners, then side, width, dotted, tolerance
    /// and extension (painter_element.cpp builds it).
    static constexpr size_t kKeyWords = 41;
    struct Key {
        uint32_t words[kKeyWords] = {};
    };
    /// The main model's limits: 2 MiB of path data (verbs + points) and 256 entries per Painter.
    static constexpr size_t kDefaultBudgetBytes = size_t{2} << 20;
    static constexpr size_t kDefaultMaxEntries = 256;
    /// Entries not used by this many consecutive paints are dropped (SwShadowCache's rule).
    static constexpr uint32_t kMaxIdlePaints = 120;

    /// Opens one paint(): resets the per-paint counters.
    void beginPaint();
    /// Closes it: drops the entries idle for kMaxIdlePaints paints, then advances the paint serial.
    void endPaint();

    bool enabled() const { return m_enabled; }
    /// Test / measurement switch (true by default). Off: lookup()/store() are not called by the
    /// Painter; the entries age like unused ones.
    void setEnabled(bool on) { m_enabled = on; }
    /// Test hook: other limits than the defaults (entries over them are evicted on the next store).
    void setLimits(size_t bytes, size_t entries) {
        m_budget = bytes;
        m_maxEntries = entries;
    }

    /// On a hit appends the entry's verbs and points to `verbs` / `points` and returns true (counted
    /// as a hit); otherwise false (counted as a miss), nothing appended.
    bool lookup(const Key& key, std::vector<PathVerb>& verbs, std::vector<float>& points);
    /// Stores `verbs[verbStart..]` / `points[pointStart..]` under `key` (after a missed lookup()),
    /// evicting the least recently used entries until it fits the limits; an entry bigger than the
    /// whole budget is not stored.
    void store(const Key& key, const std::vector<PathVerb>& verbs, size_t verbStart,
               const std::vector<float>& points, size_t pointStart);
    /// Frees every entry (the counters too).
    void clear();

    /// Counters of the last paint (beginPaint() resets them).
    uint32_t hits() const { return m_hits; }
    uint32_t misses() const { return m_misses; }
    /// What is held now.
    size_t entries() const { return m_entries.size(); }
    size_t bytes() const { return m_bytes; }

    BorderDashScratch& scratch() { return m_scratch; }

private:
    struct Entry {
        Key key;
        uint64_t hash = 0;
        uint32_t lastUsed = 0;
        std::vector<PathVerb> verbs;
        std::vector<float> points;
    };

    static uint64_t hashKey(const Key& key);
    static size_t entryBytes(const Entry& e);
    /// The first entry whose hash is not below `hash` (m_entries is sorted by hash).
    size_t lowerBound(uint64_t hash) const;
    void removeAt(size_t i);
    void evictLeastRecentlyUsed();

    std::vector<Entry> m_entries;
    BorderDashScratch m_scratch;
    size_t m_budget = kDefaultBudgetBytes;
    size_t m_maxEntries = kDefaultMaxEntries;
    size_t m_bytes = 0;
    uint32_t m_paint = 0;
    uint32_t m_hits = 0;
    uint32_t m_misses = 0;
    bool m_enabled = true;
};

}  // namespace xt
