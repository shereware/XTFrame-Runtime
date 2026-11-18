#include "sw_shadow_cache.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

/// See sw_shadow_cache.h and Docs/tasks/T-724.md.

namespace xt::sw {

static_assert(sizeof(ShadowCacheKey) % 4 == 0, "ShadowCacheKey holds 4-byte fields only");

uint64_t SwShadowCache::hashKey(const ShadowCacheKey& key) {
    // FNV-1a over the key's bytes (every byte is a field: no padding).
    const auto* p = reinterpret_cast<const unsigned char*>(&key);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof key; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

bool SwShadowCache::sameKey(const ShadowCacheKey& a, const ShadowCacheKey& b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

size_t SwShadowCache::entryBytes(const ShadowCacheKey& key) {
    if (key.x1 <= key.x0 || key.y1 <= key.y0) {
        return 0;
    }
    return static_cast<size_t>(key.x1 - key.x0) * static_cast<size_t>(key.y1 - key.y0) * sizeof(float);
}

void SwShadowCache::removeAt(size_t i) {
    m_bytes -= m_entries[i].cov.size() * sizeof(float);
    if (i + 1 != m_entries.size()) {
        m_entries[i] = std::move(m_entries.back());
    }
    m_entries.pop_back();
}

ShadowCoverage SwShadowCache::acquire(const ShadowCacheKey& key) {
    const uint64_t hash = hashKey(key);
    if (m_curSeen.size() < kMaxSeenPerFrame) {
        m_curSeen.push_back(Seen{key, hash});
    }
    ShadowCoverage out;
    Entry* hit = nullptr;
    for (Entry& e : m_entries) {
        if (e.hash == hash && sameKey(e.key, key)) {
            hit = &e;
            break;
        }
    }
    if (hit != nullptr) {
        ++m_hits;
    } else {
        // Store only a shadow drawn with the same key in the previous frame: one that moves or
        // animates never is.
        bool seen = false;
        for (const Seen& s : m_prevSeen) {
            if (s.hash == hash && sameKey(s.key, key)) {
                seen = true;
                break;
            }
        }
        const size_t need = entryBytes(key);
        if (!seen || need == 0 || need > m_budget) {
            return out;
        }
        while (m_bytes + need > m_budget && !m_entries.empty()) {
            size_t lru = 0;
            for (size_t i = 1; i < m_entries.size(); ++i) {
                if (m_entries[i].lastUsed < m_entries[lru].lastUsed) {
                    lru = i;
                }
            }
            removeAt(lru);
        }
        m_entries.emplace_back();
        hit = &m_entries.back();
        hit->key = key;
        hit->hash = hash;
        hit->cov.assign(need / sizeof(float), std::numeric_limits<float>::quiet_NaN());
        m_bytes += need;
        ++m_stores;
    }
    hit->lastUsed = m_frame;
    out.cov = hit->cov.data();
    out.x0 = key.x0;
    out.y0 = key.y0;
    out.w = key.x1 - key.x0;
    out.h = key.y1 - key.y0;
    return out;
}

void SwShadowCache::endFrame() {
    for (size_t i = m_entries.size(); i-- > 0;) {
        if (m_frame - m_entries[i].lastUsed >= kMaxIdleFrames) {
            removeAt(i);
        }
    }
    ++m_frame;
    m_prevSeen.swap(m_curSeen);
    m_curSeen.clear();
}

void SwShadowCache::clear() {
    std::vector<Entry>().swap(m_entries);
    std::vector<Seen>().swap(m_prevSeen);
    std::vector<Seen>().swap(m_curSeen);
    m_bytes = 0;
    m_hits = 0;
    m_stores = 0;
}

}  // namespace xt::sw

namespace xt::sw {

bool makeShadowCacheKey(const BoxShadowDraw& d, const Mat2x3& deviceXf, const Vec2 quad[4], int tx0, int ty0,
                        int tx1, int ty1, ShadowCacheKey& out) {
    float x0 = quad[0].x, x1 = quad[0].x, y0 = quad[0].y, y1 = quad[0].y;
    for (int i = 1; i < 4; ++i) {
        x0 = std::min(x0, quad[i].x);
        x1 = std::max(x1, quad[i].x);
        y0 = std::min(y0, quad[i].y);
        y1 = std::max(y1, quad[i].y);
    }
    if (!(x0 <= x1) || !(y0 <= y1) || !std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) ||
        !std::isfinite(y1)) {
        return false;
    }
    const auto clampI = [](float v, int lo, int hi) {
        return static_cast<int32_t>(std::clamp(v, static_cast<float>(lo), static_cast<float>(hi)));
    };
    out = ShadowCacheKey{};
    out.x0 = clampI(std::floor(x0), tx0, tx1);
    out.x1 = clampI(std::ceil(x1), tx0, tx1);
    out.y0 = clampI(std::floor(y0), ty0, ty1);
    out.y1 = clampI(std::ceil(y1), ty0, ty1);
    if (out.x1 <= out.x0 || out.y1 <= out.y0) {
        return false;
    }
    out.shape = d.shape;
    out.extraCount = d.extraCount < 2 ? d.extraCount : 2;
    for (uint32_t k = 0; k < out.extraCount; ++k) {
        out.extra[k] = d.extra[k];
        out.extraSign[k] = d.extraSign[k];
    }
    out.sigma = d.sigmaLocal;
    out.xf = deviceXf;
    return true;
}

}  // namespace xt::sw
