#include "golden_common.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "xt/sw/sw_png.h"

namespace xt::golden {

namespace {

std::vector<GoldenFixture>& mutableFixtures() {
    static std::vector<GoldenFixture> fixtures;
    return fixtures;
}

int& manifestErrorCount() {
    static int count = 0;
    return count;
}

/// The value of environment variable `name`, or an empty string.
std::string envValue(const char* name) {
    std::string text;
#ifdef _MSC_VER
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        text = value;
        std::free(value);
    }
#else
    if (const char* value = std::getenv(name)) {
        text = value;
    }
#endif
    return text;
}

}  // namespace

SwTolerance swTolerance() {
    SwTolerance tolerance;
    const std::string channel = envValue("XT_GOLDEN_CHANNEL");
    if (!channel.empty()) {
        char* end = nullptr;
        const long value = std::strtol(channel.c_str(), &end, 10);
        if (end != channel.c_str() && value >= 0 && value <= 255) {
            tolerance.channel = static_cast<int>(value);
        }
    }
    const std::string ratio = envValue("XT_GOLDEN_RATIO");
    if (!ratio.empty()) {
        char* end = nullptr;
        const float value = std::strtof(ratio.c_str(), &end);
        if (end != ratio.c_str() && value >= 0.0f && value <= 1.0f) {
            tolerance.ratio = value;
        }
    }
    return tolerance;
}

bool goldenVerbose() { return envValue("XT_GOLDEN_VERBOSE") == "1"; }

std::string describeDiff(const char* tag, const std::string& name, const GoldenResult& result, float ratio,
                         int channel) {
    char line[512];
    std::snprintf(
        line, sizeof(line), "%s %s: %u px differ, max channel diff %u (allowed: ratio %g, channel %d)", tag,
        name.c_str(), result.diffPixels, result.maxChannelDiff, static_cast<double>(ratio), channel);
    return line;
}

void report(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fflush(stdout);
}

int manifestErrors() { return manifestErrorCount(); }

void noteManifestError() { ++manifestErrorCount(); }

GoldenResult compareRgba(const uint8_t* a, const uint8_t* b, int w, int h, int channelTolerance,
                         float maxDiffRatio) {
    GoldenResult result{};
    if (w <= 0 || h <= 0) {
        result.pass = true;
        return result;
    }
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    uint32_t diffPixels = 0;
    uint32_t maxChannelDiff = 0;
    for (size_t i = 0; i < pixelCount; ++i) {
        bool pixelDiffers = false;
        for (size_t c = 0; c < 4; ++c) {
            size_t idx = i * 4 + c;
            int delta = static_cast<int>(a[idx]) - static_cast<int>(b[idx]);
            uint32_t absDelta = static_cast<uint32_t>(delta < 0 ? -delta : delta);
            if (absDelta > maxChannelDiff) {
                maxChannelDiff = absDelta;
            }
            if (static_cast<int>(absDelta) > channelTolerance) {
                pixelDiffers = true;
            }
        }
        if (pixelDiffers) {
            ++diffPixels;
        }
    }
    result.diffPixels = diffPixels;
    result.maxChannelDiff = maxChannelDiff;
    float ratio = static_cast<float>(diffPixels) / (static_cast<float>(w) * static_cast<float>(h));
    result.pass = ratio <= maxDiffRatio;
    return result;
}

bool writeDiffPng(const std::string& path, const uint8_t* a, const uint8_t* b, int w, int h,
                  int channelTolerance) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < pixelCount; ++i) {
        size_t base = i * 4;
        bool pixelDiffers = false;
        for (size_t c = 0; c < 4; ++c) {
            int delta = static_cast<int>(a[base + c]) - static_cast<int>(b[base + c]);
            int absDelta = delta < 0 ? -delta : delta;
            if (absDelta > channelTolerance) {
                pixelDiffers = true;
                break;
            }
        }
        if (pixelDiffers) {
            out[base + 0] = 255;
            out[base + 1] = 0;
            out[base + 2] = 0;
            out[base + 3] = 255;
        } else {
            uint32_t luma =
                (static_cast<uint32_t>(a[base + 0]) * 30 + static_cast<uint32_t>(a[base + 1]) * 59 +
                 static_cast<uint32_t>(a[base + 2]) * 11) /
                100;
            uint8_t gray = static_cast<uint8_t>(luma);
            out[base + 0] = gray;
            out[base + 1] = gray;
            out[base + 2] = gray;
            out[base + 3] = 255;
        }
    }
    return xt::sw::writePng(path, out.data(), w, h, w * 4);
}

void registerFixture(const GoldenFixture& fixture) { mutableFixtures().push_back(fixture); }

const std::vector<GoldenFixture>& fixtures() { return mutableFixtures(); }

void appendCircle(std::vector<xt::PathVerb>& verbs, std::vector<float>& points, xt::Vec2 c, float r) {
    // Standard 4-cubic-segment circle approximation. kappa = 4/3 * (sqrt(2) - 1).
    const float k = 0.5522847498307936f * r;
    const xt::Vec2 p0{c.x + r, c.y};
    const xt::Vec2 p1{c.x, c.y + r};
    const xt::Vec2 p2{c.x - r, c.y};
    const xt::Vec2 p3{c.x, c.y - r};

    auto pushPoint = [&points](xt::Vec2 p) {
        points.push_back(p.x);
        points.push_back(p.y);
    };

    verbs.push_back(xt::PathVerb::Move);
    pushPoint(p0);

    verbs.push_back(xt::PathVerb::Cubic);
    pushPoint(xt::Vec2{p0.x, p0.y + k});
    pushPoint(xt::Vec2{p1.x + k, p1.y});
    pushPoint(p1);

    verbs.push_back(xt::PathVerb::Cubic);
    pushPoint(xt::Vec2{p1.x - k, p1.y});
    pushPoint(xt::Vec2{p2.x, p2.y + k});
    pushPoint(p2);

    verbs.push_back(xt::PathVerb::Cubic);
    pushPoint(xt::Vec2{p2.x, p2.y - k});
    pushPoint(xt::Vec2{p3.x - k, p3.y});
    pushPoint(p3);

    verbs.push_back(xt::PathVerb::Cubic);
    pushPoint(xt::Vec2{p3.x + k, p3.y});
    pushPoint(xt::Vec2{p0.x, p0.y - k});
    pushPoint(p0);

    verbs.push_back(xt::PathVerb::Close);
}

uint32_t addPath(xt::compile::SceneBuilder& scene, const PathSpec& spec) {
    xt::IrPathData p{};
    p.verbOffset = static_cast<uint32_t>(scene.verbPool.size());
    for (xt::PathVerb v : spec.verbs) {
        scene.verbPool.push_back(static_cast<uint8_t>(v));
    }
    p.verbCount = static_cast<uint32_t>(spec.verbs.size());

    p.pointOffset = static_cast<uint32_t>(scene.floatPool.size());
    scene.floatPool.insert(scene.floatPool.end(), spec.points.begin(), spec.points.end());
    p.pointCount = static_cast<uint32_t>(spec.points.size());

    p.fillRule = static_cast<uint8_t>(spec.fillRule);
    p.fillKind = static_cast<uint8_t>(spec.fillKind);
    p.strokeKind = static_cast<uint8_t>(spec.strokeKind);
    p.cap = static_cast<uint8_t>(spec.cap);
    p.join = static_cast<uint8_t>(spec.join);
    p.fillColor = spec.fillColor;
    p.strokeColor = spec.strokeColor;
    p.fillP0 = spec.fillP0;
    p.fillP1 = spec.fillP1;
    p.strokeP0 = spec.strokeP0;
    p.strokeP1 = spec.strokeP1;
    p.strokeWidth = spec.strokeWidth;
    p.miterLimit = spec.miterLimit;
    p.trimStart = spec.trimStart;
    p.trimEnd = spec.trimEnd;
    p.trimOffset = spec.trimOffset;

    p.fillStopOffset = static_cast<uint32_t>(scene.stopPool.size());
    scene.stopPool.insert(scene.stopPool.end(), spec.fillStops.begin(), spec.fillStops.end());
    p.fillStopCount = static_cast<uint32_t>(spec.fillStops.size());

    p.strokeStopOffset = static_cast<uint32_t>(scene.stopPool.size());
    scene.stopPool.insert(scene.stopPool.end(), spec.strokeStops.begin(), spec.strokeStops.end());
    p.strokeStopCount = static_cast<uint32_t>(spec.strokeStops.size());

    p.dashOffset = static_cast<uint32_t>(scene.floatPool.size());
    scene.floatPool.insert(scene.floatPool.end(), spec.dashes.begin(), spec.dashes.end());
    p.dashCount = static_cast<uint32_t>(spec.dashes.size());
    p.dashPhase = spec.dashPhase;

    p.taperStart = spec.taperStart;
    p.taperEnd = spec.taperEnd;
    if (!spec.widthProfile.empty()) {
        p.profileOffset = static_cast<uint32_t>(scene.floatPool.size());
        scene.floatPool.insert(scene.floatPool.end(), spec.widthProfile.begin(), spec.widthProfile.end());
        p.profileCount = static_cast<uint32_t>(spec.widthProfile.size() / 2);
    }

    scene.paths.push_back(p);
    return static_cast<uint32_t>(scene.paths.size() - 1);
}

uint32_t addNode(xt::compile::SceneBuilder& scene, xt::NodeKind kind, uint32_t parent, uint32_t kindIndex,
                 const NodeOpts& opts) {
    xt::IrSceneNode n{};
    n.kind = static_cast<uint8_t>(kind);
    n.visible = opts.visible;
    n.blend = opts.blend;
    n.maskMode = opts.maskMode;
    n.parent = parent;
    n.kindIndex = kindIndex;
    n.opacity = opts.opacity;
    n.anchor = opts.anchor;
    n.translate = opts.translate;
    n.scale = opts.scale;
    n.rotation = opts.rotation;
    n.skew = opts.skew;
    n.name = xt::kInvalidString;
    scene.nodes.push_back(n);
    return static_cast<uint32_t>(scene.nodes.size() - 1);
}

}  // namespace xt::golden
