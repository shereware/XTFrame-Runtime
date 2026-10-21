#include "xt/bundle/design_color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "color_math.h"
#include "xt/asset_resolver.h"
#include "xt/sw/sw_png.h"

/// The colour tools of docs/design/35-design-system.md §35.3.3 (docs/tasks/T-309.md item 2). See
/// xt/bundle/design_color.h.

namespace xt::bundle::design {

namespace {

constexpr int kMaxSide = 256;
constexpr int kMaxIterations = 20;
constexpr int kMaxCount = 32;
constexpr int kMaxSteps = 64;
constexpr double kScaleLightest = 0.97;
constexpr double kScaleDarkest = 0.20;

/// xorshift32 from a fixed seed: the "random" choices of k-means++ are the same on every run.
class FixedRandom {
public:
    /// A value in [0, 1).
    double next() {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        return static_cast<double>(m_state) / 4294967296.0;
    }

private:
    uint32_t m_state = 0x2545F491u;
};

/// One distinct colour of the sampled picture.
struct Point {
    uint32_t rgb = 0;
    double weight = 0.0;
    bundle::detail::Lab lab;
};

double distance2(const bundle::detail::Lab& x, const bundle::detail::Lab& y) {
    const double dl = x.l - y.l;
    const double da = x.a - y.a;
    const double db = x.b - y.b;
    return dl * dl + da * da + db * db;
}

/// Index of the point at `target` (0..total) along the cumulative weights `w`.
size_t pickWeighted(const std::vector<double>& w, double target) {
    double sum = 0.0;
    for (size_t i = 0; i < w.size(); ++i) {
        sum += w[i];
        if (target < sum && w[i] > 0.0) {
            return i;
        }
    }
    for (size_t i = w.size(); i-- > 0;) {
        if (w[i] > 0.0) {
            return i;
        }
    }
    return 0;
}

}  // namespace

std::vector<ExtractedColor> extractPalette(const uint8_t* rgba, int w, int h, int count) {
    std::vector<ExtractedColor> out;
    if (rgba == nullptr || w <= 0 || h <= 0 || count < 1) {
        return out;
    }
    count = std::min(count, kMaxCount);
    const int longSide = std::max(w, h);
    const double scale = longSide > kMaxSide ? static_cast<double>(kMaxSide) / longSide : 1.0;
    const int tw = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int th = std::max(1, static_cast<int>(std::lround(h * scale)));

    // The distinct opaque colours and how many sampled pixels have each.
    std::unordered_map<uint32_t, double> histogram;
    for (int y = 0; y < th; ++y) {
        const int sy = std::min(h - 1, static_cast<int>((y + 0.5) * h / th));
        for (int x = 0; x < tw; ++x) {
            const int sx = std::min(w - 1, static_cast<int>((x + 0.5) * w / tw));
            const uint8_t* p =
                rgba + (static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 4;
            if (p[3] < 128) {
                continue;
            }
            histogram[(static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2]] += 1.0;
        }
    }
    if (histogram.empty()) {
        return out;
    }
    std::vector<Point> points;
    points.reserve(histogram.size());
    for (const auto& [rgb, weight] : histogram) {
        Point point;
        point.rgb = rgb;
        point.weight = weight;
        point.lab = bundle::detail::srgbToLab(((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
                                              (rgb & 0xff) / 255.0);
        points.push_back(point);
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.rgb < b.rgb; });
    double total = 0.0;
    for (const Point& p : points) {
        total += p.weight;
    }

    // k-means++ seeding; stops early when every point already sits on a centre.
    FixedRandom random;
    std::vector<bundle::detail::Lab> centres;
    std::vector<double> weights(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        weights[i] = points[i].weight;
    }
    centres.push_back(points[pickWeighted(weights, random.next() * total)].lab);
    while (static_cast<int>(centres.size()) < count) {
        double sum = 0.0;
        for (size_t i = 0; i < points.size(); ++i) {
            double best = distance2(points[i].lab, centres.front());
            for (size_t c = 1; c < centres.size(); ++c) {
                best = std::min(best, distance2(points[i].lab, centres[c]));
            }
            weights[i] = best * points[i].weight;
            sum += weights[i];
        }
        if (sum <= 0.0) {
            break;
        }
        centres.push_back(points[pickWeighted(weights, random.next() * sum)].lab);
    }

    // Lloyd iterations.
    std::vector<size_t> assignment(points.size(), static_cast<size_t>(-1));
    for (int iteration = 0; iteration < kMaxIterations; ++iteration) {
        bool changed = false;
        for (size_t i = 0; i < points.size(); ++i) {
            size_t best = 0;
            double bestD = distance2(points[i].lab, centres[0]);
            for (size_t c = 1; c < centres.size(); ++c) {
                const double d = distance2(points[i].lab, centres[c]);
                if (d < bestD) {
                    bestD = d;
                    best = c;
                }
            }
            if (assignment[i] != best) {
                assignment[i] = best;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
        std::vector<bundle::detail::Lab> sums(centres.size());
        std::vector<double> mass(centres.size(), 0.0);
        for (size_t i = 0; i < points.size(); ++i) {
            const size_t c = assignment[i];
            sums[c].l += points[i].lab.l * points[i].weight;
            sums[c].a += points[i].lab.a * points[i].weight;
            sums[c].b += points[i].lab.b * points[i].weight;
            mass[c] += points[i].weight;
        }
        for (size_t c = 0; c < centres.size(); ++c) {
            if (mass[c] > 0.0) {
                centres[c] =
                    bundle::detail::Lab{sums[c].l / mass[c], sums[c].a / mass[c], sums[c].b / mass[c]};
            }
        }
    }

    // Each cluster's colour: the mean sRGB of its pixels. Clusters that round to one hex merge.
    std::vector<double> r(centres.size(), 0.0), g(centres.size(), 0.0), b(centres.size(), 0.0),
        mass(centres.size(), 0.0);
    for (size_t i = 0; i < points.size(); ++i) {
        const size_t c = assignment[i];
        r[c] += ((points[i].rgb >> 16) & 0xff) * points[i].weight;
        g[c] += ((points[i].rgb >> 8) & 0xff) * points[i].weight;
        b[c] += (points[i].rgb & 0xff) * points[i].weight;
        mass[c] += points[i].weight;
    }
    std::map<std::string, double> merged;
    for (size_t c = 0; c < centres.size(); ++c) {
        if (mass[c] <= 0.0) {
            continue;
        }
        merged[bundle::detail::formatHex(r[c] / mass[c] / 255.0, g[c] / mass[c] / 255.0,
                                         b[c] / mass[c] / 255.0)] += mass[c];
    }
    for (const auto& [value, weight] : merged) {
        out.push_back(ExtractedColor{value, static_cast<float>(weight / total)});
    }
    std::stable_sort(out.begin(), out.end(), [](const ExtractedColor& lhs, const ExtractedColor& rhs) {
        return lhs.share > rhs.share;
    });
    return out;
}

Status extractPaletteFromFile(std::string_view path, int count, std::vector<ExtractedColor>& out,
                              std::string* error) {
    out.clear();
    std::string bytes;
    if (!bundle::detail::readFile(std::string(path), bytes)) {
        if (error != nullptr) {
            *error = "cannot read '" + std::string(path) + "'";
        }
        return Status::kNotFound;
    }
    ImageData image;
    if (!sw::decodeImageBytes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), image) ||
        image.width <= 0 || image.height <= 0 ||
        image.rgba.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4) {
        if (error != nullptr) {
            *error = "'" + std::string(path) + "' is not an image this build can decode";
        }
        return Status::kInvalidArgument;
    }
    out = extractPalette(image.rgba.data(), image.width, image.height, count);
    return Status::kOk;
}

std::vector<std::string> deriveScale(std::string_view hex, int steps, std::string_view method) {
    std::vector<std::string> out;
    float rgba[4];
    if (method != "oklch-lightness" || steps < 1 || steps > kMaxSteps ||
        !bundle::detail::parseHexColor(hex, rgba)) {
        return out;
    }
    if (steps == 1) {
        out.push_back(bundle::detail::formatHex(rgba[0], rgba[1], rgba[2]));
        return out;
    }
    const bundle::detail::Oklab base = bundle::detail::srgbToOklab(rgba[0], rgba[1], rgba[2]);
    const double chroma = std::sqrt(base.a * base.a + base.b * base.b);
    const double hue = std::atan2(base.b, base.a);
    for (int i = 0; i < steps; ++i) {
        const double l = kScaleLightest - (kScaleLightest - kScaleDarkest) * i / (steps - 1);
        const auto at = [&](double c, double rgb[3]) {
            bundle::detail::oklabToSrgb(bundle::detail::Oklab{l, c * std::cos(hue), c * std::sin(hue)}, rgb);
        };
        const auto inGamut = [](const double rgb[3]) {
            constexpr double kEps = 1e-6;
            return rgb[0] >= -kEps && rgb[0] <= 1.0 + kEps && rgb[1] >= -kEps && rgb[1] <= 1.0 + kEps &&
                   rgb[2] >= -kEps && rgb[2] <= 1.0 + kEps;
        };
        double rgb[3];
        at(chroma, rgb);
        if (!inGamut(rgb)) {
            // Bisect the largest chroma that fits (chroma 0 always fits for 0 < l < 1).
            double lo = 0.0;
            double hi = chroma;
            for (int k = 0; k < 32; ++k) {
                const double mid = (lo + hi) / 2.0;
                at(mid, rgb);
                (inGamut(rgb) ? lo : hi) = mid;
            }
            at(lo, rgb);
        }
        out.push_back(bundle::detail::formatHex(rgb[0], rgb[1], rgb[2]));
    }
    return out;
}

float contrastRatio(std::string_view hexA, std::string_view hexB) {
    float a[4];
    float b[4];
    if (!bundle::detail::parseHexColor(hexA, a) || !bundle::detail::parseHexColor(hexB, b)) {
        return 0.0f;
    }
    const double la = bundle::detail::relativeLuminance(a[0], a[1], a[2]);
    const double lb = bundle::detail::relativeLuminance(b[0], b[1], b[2]);
    const double hi = std::max(la, lb);
    const double lo = std::min(la, lb);
    return static_cast<float>((hi + 0.05) / (lo + 0.05));
}

float deltaE2000(std::string_view hexA, std::string_view hexB) {
    double d = 0.0;
    if (!bundle::detail::hexDeltaE2000(hexA, hexB, &d)) {
        return -1.0f;
    }
    return static_cast<float>(d);
}

std::string normalizeHex(std::string_view text) {
    float rgba[4];
    if (!bundle::detail::parseHexColor(text, rgba)) {
        return std::string();
    }
    std::string out = bundle::detail::formatHex(rgba[0], rgba[1], rgba[2]);
    const int alpha = static_cast<int>(std::lround(rgba[3] * 255.0f));
    if (alpha != 255) {
        char buffer[4];
        std::snprintf(buffer, sizeof(buffer), "%02x", alpha);
        out += buffer;
    }
    return out;
}

}  // namespace xt::bundle::design
