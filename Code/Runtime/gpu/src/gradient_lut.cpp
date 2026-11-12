#include "gradient_lut.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "xt/gpu/gpu_frame.h"

namespace xt::gpu::detail {

namespace {

uint64_t hashStops(std::span<const GradientStop> stops) {
    // FNV-1a over the raw stop bytes (offset + color floats).
    uint64_t h = 1469598103934665603ull;
    for (const GradientStop& s : stops) {
        unsigned char bytes[sizeof(float) * 5];
        std::memcpy(bytes, &s.offset, sizeof(float));
        std::memcpy(bytes + sizeof(float), &s.color.r, sizeof(float));
        std::memcpy(bytes + 2 * sizeof(float), &s.color.g, sizeof(float));
        std::memcpy(bytes + 3 * sizeof(float), &s.color.b, sizeof(float));
        std::memcpy(bytes + 4 * sizeof(float), &s.color.a, sizeof(float));
        for (unsigned char b : bytes) {
            h ^= b;
            h *= 1099511628211ull;
        }
    }
    // Mix the count in so a prefix and its extension never collide.
    h ^= static_cast<uint64_t>(stops.size());
    h *= 1099511628211ull;
    return h;
}

uint8_t toByte(float v) {
    float c = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(c * 255.0f));
}

}  // namespace

void bakeGradientLut(std::span<const GradientStop> stops, uint32_t count, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(count) * 4, 0);
    if (stops.empty() || count == 0) {
        return;
    }
    const size_t n = stops.size();
    for (uint32_t i = 0; i < count; ++i) {
        float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        Color c;
        if (n == 1 || t <= stops[0].offset) {
            c = stops[0].color;
        } else if (t >= stops[n - 1].offset) {
            c = stops[n - 1].color;
        } else {
            c = stops[n - 1].color;
            for (size_t k = 0; k + 1 < n; ++k) {
                float a = stops[k].offset;
                float b = stops[k + 1].offset;
                if (t >= a && t <= b) {
                    float span = b - a;
                    float f = span > 0.0f ? (t - a) / span : 0.0f;
                    const Color& ca = stops[k].color;
                    const Color& cb = stops[k + 1].color;
                    c = Color{ca.r + (cb.r - ca.r) * f, ca.g + (cb.g - ca.g) * f, ca.b + (cb.b - ca.b) * f,
                              ca.a + (cb.a - ca.a) * f};
                    break;
                }
            }
        }
        out[static_cast<size_t>(i) * 4 + 0] = toByte(c.r);
        out[static_cast<size_t>(i) * 4 + 1] = toByte(c.g);
        out[static_cast<size_t>(i) * 4 + 2] = toByte(c.b);
        out[static_cast<size_t>(i) * 4 + 3] = toByte(c.a);
    }
}

void GradientLutCache::shutdown() {
    if (m_device != nullptr) {
        for (Entry& e : m_entries) {
            m_device->destroyTexture(e.texture);
        }
    }
    m_entries.clear();
}

TextureHandle GradientLutCache::get(std::span<const GradientStop> stops) {
    if (stops.empty() || m_device == nullptr) {
        return 0;
    }
    const uint64_t h = hashStops(stops);
    for (Entry& e : m_entries) {
        if (e.hash == h) {
            e.lastUsed = m_frame;
            return e.texture;
        }
    }
    bakeGradientLut(stops, kGradientLutWidth, m_scratch);
    TextureHandle tex = m_device->createTexture("xt gradient lut", kGradientLutWidth, 1, TextureFormat::Rgba8,
                                                m_scratch.data());
    if (tex == 0) {
        return 0;
    }
    if (m_entries.size() >= kGradientLutMaxTables) {
        // T-695: the cap replaces the least recently used table. One this frame drew with stays (the
        // frame being built references it), so a frame drawing more than kGradientLutMaxTables
        // distinct ramps keeps them all until a later endFrame.
        const size_t victim = leastRecentlyUnused();
        if (victim < m_entries.size()) {
            m_device->destroyTexture(m_entries[victim].texture);
            m_entries[victim] = Entry{h, tex, m_frame};
            return tex;
        }
    }
    m_entries.push_back(Entry{h, tex, m_frame});
    return tex;
}

size_t GradientLutCache::leastRecentlyUnused() const {
    size_t best = m_entries.size();
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (e.lastUsed == m_frame) continue;
        if (best == m_entries.size() || e.lastUsed < m_entries[best].lastUsed) best = i;
    }
    return best;
}

void GradientLutCache::endFrame() {
    size_t w = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& e = m_entries[i];
        if (m_frame - e.lastUsed > kGradientLutKeepFrames) {
            m_device->destroyTexture(e.texture);
            continue;
        }
        m_entries[w++] = e;
    }
    m_entries.resize(w);
    while (m_entries.size() > kGradientLutMaxTables) {
        const size_t victim = leastRecentlyUnused();
        if (victim == m_entries.size()) break;
        m_device->destroyTexture(m_entries[victim].texture);
        m_entries[victim] = m_entries.back();
        m_entries.pop_back();
    }
}

}  // namespace xt::gpu::detail
