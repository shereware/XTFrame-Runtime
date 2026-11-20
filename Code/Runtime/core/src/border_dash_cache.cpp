#include "border_dash_cache.h"

#include <cstring>
#include <utility>

/// See border_dash_cache.h and Docs/tasks/T-737.md A.

namespace xt {

uint64_t BorderDashCache::hashKey(const Key& key) {
    // FNV-1a style, one 32-bit word per step (every word is a field: no padding).
    uint64_t h = 1469598103934665603ull;
    for (uint32_t w : key.words) {
        h = (h ^ w) * 1099511628211ull;
    }
    return h;
}

size_t BorderDashCache::entryBytes(const Entry& e) {
    return e.verbs.size() * sizeof(PathVerb) + e.points.size() * sizeof(float);
}

size_t BorderDashCache::lowerBound(uint64_t hash) const {
    size_t lo = 0;
    size_t hi = m_entries.size();
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (m_entries[mid].hash < hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void BorderDashCache::removeAt(size_t i) {
    m_bytes -= entryBytes(m_entries[i]);
    m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
}

void BorderDashCache::evictLeastRecentlyUsed() {
    size_t lru = 0;
    for (size_t i = 1; i < m_entries.size(); ++i) {
        // The age, not the serial, so a wrapped serial still finds the oldest entry.
        if (m_paint - m_entries[i].lastUsed > m_paint - m_entries[lru].lastUsed) {
            lru = i;
        }
    }
    removeAt(lru);
}

void BorderDashCache::beginPaint() {
    m_hits = 0;
    m_misses = 0;
}

void BorderDashCache::endPaint() {
    for (size_t i = m_entries.size(); i-- > 0;) {
        if (m_paint - m_entries[i].lastUsed >= kMaxIdlePaints) {
            removeAt(i);
        }
    }
    ++m_paint;
}

bool BorderDashCache::lookup(const Key& key, std::vector<PathVerb>& verbs, std::vector<float>& points) {
    const uint64_t hash = hashKey(key);
    for (size_t i = lowerBound(hash); i < m_entries.size() && m_entries[i].hash == hash; ++i) {
        Entry& e = m_entries[i];
        if (std::memcmp(e.key.words, key.words, sizeof key.words) != 0) {
            continue;
        }
        e.lastUsed = m_paint;
        verbs.insert(verbs.end(), e.verbs.begin(), e.verbs.end());
        points.insert(points.end(), e.points.begin(), e.points.end());
        ++m_hits;
        return true;
    }
    ++m_misses;
    return false;
}

void BorderDashCache::store(const Key& key, const std::vector<PathVerb>& verbs, size_t verbStart,
                            const std::vector<float>& points, size_t pointStart) {
    if (verbStart > verbs.size() || pointStart > points.size() || m_maxEntries == 0) {
        return;
    }
    const size_t need =
        (verbs.size() - verbStart) * sizeof(PathVerb) + (points.size() - pointStart) * sizeof(float);
    if (need > m_budget) {
        return;
    }
    while (!m_entries.empty() && (m_entries.size() >= m_maxEntries || m_bytes + need > m_budget)) {
        evictLeastRecentlyUsed();
    }
    Entry e;
    e.key = key;
    e.hash = hashKey(key);
    e.lastUsed = m_paint;
    e.verbs.assign(verbs.begin() + static_cast<std::ptrdiff_t>(verbStart), verbs.end());
    e.points.assign(points.begin() + static_cast<std::ptrdiff_t>(pointStart), points.end());
    const size_t at = lowerBound(e.hash);
    m_entries.insert(m_entries.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
    m_bytes += need;
}

void BorderDashCache::clear() {
    std::vector<Entry>().swap(m_entries);
    m_bytes = 0;
    m_hits = 0;
    m_misses = 0;
}

}  // namespace xt
