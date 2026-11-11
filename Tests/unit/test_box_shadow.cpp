#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "rounded_rect.h"  // Code/Runtime/core/src
#include "sw_paint.h"      // Code/Runtime/sw/src: PaintEval::makeBoxShadow (the row-shared span path)
#include "xt/box_shadow.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/compile/compile.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-668 (Docs/design/10-backends.md §10.12 item 2 and "10.12 定案 2 的規格補充", Docs/tasks/T-668.md
/// items 2, 3 and 6): the DrawBoxShadow coverage formula against a brute-force Gaussian convolution,
/// the Painter's six-condition test (one just-failing case per condition), and the conversion
/// itself.

using namespace xt;
using xt::sw::SwRenderer;
using xt::sw::SwTarget;

namespace {

// -------------------------------------------------------------------------------------------------
// Brute-force reference: the shape's coverage sampled on a 1/64 px grid (64 x 64 samples per
// pixel), convolved with a Gaussian of sigma and evaluated at the pixel centre. Rows of samples are
// summed with prefix sums of the separable x weights; every sample still counts once. Double
// precision and its own span geometry, independent of box_shadow.cpp.
// -------------------------------------------------------------------------------------------------

struct RefShape {
    double x0, y0, x1, y1;
    double r[4];  // tl, tr, br, bl
};

bool refSpan(const RefShape& s, double y, double& xl, double& xr) {
    if (y < s.y0 || y > s.y1) {
        return false;
    }
    auto circle = [](double r, double d) { return r - std::sqrt(std::max(0.0, r * r - d * d)); };
    xl = s.x0;
    if (y < s.y0 + s.r[0]) {
        xl = s.x0 + circle(s.r[0], s.y0 + s.r[0] - y);
    } else if (y > s.y1 - s.r[3]) {
        xl = s.x0 + circle(s.r[3], y - (s.y1 - s.r[3]));
    }
    xr = s.x1;
    if (y < s.y0 + s.r[1]) {
        xr = s.x1 - circle(s.r[1], s.y0 + s.r[1] - y);
    } else if (y > s.y1 - s.r[2]) {
        xr = s.x1 - circle(s.r[2], y - (s.y1 - s.r[2]));
    }
    return xr > xl;
}

constexpr double kSub = 64.0;

class Reference {
public:
    Reference(const RefShape& s, double sigma) : m_s(s), m_sigma(sigma) {
        m_nx = static_cast<int>(std::ceil((s.x1 - s.x0) * kSub));
        m_ny = static_cast<int>(std::ceil((s.y1 - s.y0) * kSub));
    }

    /// The blurred coverage at (px, py).
    double blurred(double px, double py) {
        const std::vector<double>& prefix = prefixFor(px);
        const double h = 1.0 / kSub;
        const double inv2s2 = 1.0 / (2.0 * m_sigma * m_sigma);
        const int j0 = std::max(0, static_cast<int>(std::floor((py - 6.0 * m_sigma - m_s.y0) * kSub)));
        const int j1 = std::min(m_ny - 1, static_cast<int>(std::ceil((py + 6.0 * m_sigma - m_s.y0) * kSub)));
        double sum = 0.0;
        for (int j = j0; j <= j1; ++j) {
            const double yc = m_s.y0 + (j + 0.5) * h;
            double xl = 0, xr = 0;
            if (!refSpan(m_s, yc, xl, xr)) {
                continue;
            }
            int lo = static_cast<int>(std::ceil((xl - m_s.x0) * kSub - 0.5));
            int hi = static_cast<int>(std::floor((xr - m_s.x0) * kSub - 0.5));
            lo = std::max(lo, 0);
            hi = std::min(hi, m_nx - 1);
            if (lo > hi) {
                continue;
            }
            const double dy = yc - py;
            sum += std::exp(-dy * dy * inv2s2) *
                   (prefix[static_cast<size_t>(hi) + 1] - prefix[static_cast<size_t>(lo)]);
        }
        return sum * h * h / (2.0 * 3.14159265358979323846 * m_sigma * m_sigma);
    }

    /// The pixel's own coverage (no blur): the fraction of its 64 x 64 samples inside the shape.
    double sharp(int ix, int iy) const {
        const double h = 1.0 / kSub;
        int inside = 0;
        for (int j = 0; j < 64; ++j) {
            const double yc = iy + (j + 0.5) * h;
            double xl = 0, xr = 0;
            if (!refSpan(m_s, yc, xl, xr)) {
                continue;
            }
            for (int i = 0; i < 64; ++i) {
                const double xc = ix + (i + 0.5) * h;
                inside += (xc >= xl && xc <= xr) ? 1 : 0;
            }
        }
        return inside / (64.0 * 64.0);
    }

private:
    const std::vector<double>& prefixFor(double px) {
        auto it = m_prefix.find(px);
        if (it != m_prefix.end()) {
            return it->second;
        }
        std::vector<double> p(static_cast<size_t>(m_nx) + 1, 0.0);
        const double inv2s2 = 1.0 / (2.0 * m_sigma * m_sigma);
        for (int i = 0; i < m_nx; ++i) {
            const double dx = m_s.x0 + (i + 0.5) / kSub - px;
            p[static_cast<size_t>(i) + 1] = p[static_cast<size_t>(i)] + std::exp(-dx * dx * inv2s2);
        }
        return m_prefix.emplace(px, std::move(p)).first->second;
    }

    RefShape m_s;
    double m_sigma;
    int m_nx = 0, m_ny = 0;
    std::map<double, std::vector<double>> m_prefix;
};

struct ShadowCase {
    const char* name;
    Rect rect;
    float radii[4];
    Vec2 offset;
    float blur;
    float spread;
};

/// Renders `c` (black, alpha 1) with xt::sw into a transparent 128 x 128 target and returns the
/// largest |alpha / 255 - reference| over every second pixel of the drawn area.
double maxErrorAgainstReference(const ShadowCase& c) {
    const int w = 128, h = 128;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{0, 0, 0, 0});
    CommandList cl;
    cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    cl.devicePixelRatio = 1;
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = c.rect;
    for (int i = 0; i < 4; ++i) {
        bs.radii[i] = c.radii[i];
    }
    bs.offset = c.offset;
    bs.blur = c.blur;
    bs.spread = c.spread;
    bs.color = Color{0, 0, 0, 1};
    cl.boxShadows.push_back(bs);
    cl.commands.push_back(Command{CmdKind::DrawBoxShadow, {}, 0});
    renderer.render(cl, nullptr, nullptr);

    BoxShadowDraw d;
    REQUIRE(resolveBoxShadow(bs, Mat2x3{}, d));
    const BoxShadowShape& s = d.shape;
    RefShape rs{s.x0, s.y0, s.x1, s.y1, {s.r[0], s.r[1], s.r[2], s.r[3]}};
    Reference ref(rs, d.sigmaLocal > 0 ? d.sigmaLocal : 1.0);
    const float reach = d.sharp ? 1.0f : kBoxShadowSigmaReach * d.sigmaLocal + 1.0f;
    const int ix0 = std::max(0, static_cast<int>(std::floor(s.x0 - reach)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(s.y0 - reach)));
    const int ix1 = std::min(w - 1, static_cast<int>(std::ceil(s.x1 + reach)));
    const int iy1 = std::min(h - 1, static_cast<int>(std::ceil(s.y1 + reach)));
    double worst = 0.0;
    for (int y = iy0; y <= iy1; y += 2) {
        for (int x = ix0; x <= ix1; x += 2) {
            const double expected = d.sharp ? ref.sharp(x, y) : ref.blurred(x + 0.5, y + 0.5);
            const double got = buf[(static_cast<size_t>(y) * w + x) * 4 + 3] / 255.0;
            worst = std::max(worst, std::fabs(got - expected));
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("BoxShadow: A&S 7.1.26 erf within 1.5e-7 of std::erf (plus float rounding)") {
    double worst = 0.0;
    for (int i = -4000; i <= 4000; ++i) {
        const float x = static_cast<float>(i) / 1000.0f;
        worst = std::max(worst,
                         std::fabs(static_cast<double>(boxShadowErf(x)) - std::erf(static_cast<double>(x))));
    }
    CHECK(worst < 1e-6);
    CHECK(boxShadowErf(0.0f) == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(boxShadowErf(-1.0f) == doctest::Approx(-boxShadowErf(1.0f)));
}

TEST_CASE("BoxShadow: Gauss-Legendre weights sum to 1 on each half (16 points on [-1, 1])") {
    double sum = 0.0;
    for (float w : kBoxShadowQuadratureWeights) {
        sum += w;
    }
    CHECK(sum == doctest::Approx(1.0).epsilon(1e-6));
}

TEST_CASE("BoxShadow: xt::sw against a 64x64-supersampled brute-force Gaussian convolution, <= 2/255") {
    // T-668 item 6: square corners, equal and unequal radii, a radius above 3 sigma and below sigma,
    // positive and negative spread, a blur whose 3 sigma exceeds the box, the threshold both sides.
    const ShadowCase cases[] = {
        {"square corners", Rect{34, 40, 60, 40}, {0, 0, 0, 0}, Vec2{2, 3}, 8.0f, 0.0f},
        {"equal radii", Rect{34, 40, 60, 40}, {10, 10, 10, 10}, Vec2{0, 4}, 8.0f, 0.0f},
        {"unequal radii", Rect{30, 34, 64, 56}, {0, 6, 14, 22}, Vec2{-2, 3}, 10.0f, 0.0f},
        {"radius > 3 sigma", Rect{30, 30, 64, 64}, {20, 20, 20, 20}, Vec2{0, 0}, 6.0f, 0.0f},
        {"radius < sigma", Rect{40, 40, 44, 40}, {2, 2, 2, 2}, Vec2{0, 2}, 12.0f, 0.0f},
        {"spread +6", Rect{40, 42, 46, 40}, {8, 8, 8, 8}, Vec2{1, 1}, 8.0f, 6.0f},
        {"spread -6", Rect{30, 34, 64, 56}, {12, 12, 12, 12}, Vec2{0, 3}, 8.0f, -6.0f},
        {"3 sigma beyond the box", Rect{59, 60, 10, 8}, {3, 3, 3, 3}, Vec2{0, 0}, 30.0f, 0.0f},
        {"blur just above the threshold", Rect{30.3f, 30.6f, 60, 50}, {9, 9, 9, 9}, Vec2{0, 0}, 2.9f, 0.0f},
    };
    for (const ShadowCase& c : cases) {
        const double err = maxErrorAgainstReference(c);
        MESSAGE(std::string(c.name) << ": max error " << err * 255.0 << " / 255");
        CAPTURE(c.name);
        CHECK(err <= 2.0 / 255.0);
    }
}

TEST_CASE("BoxShadow: just below the threshold xt::sw draws R' exactly as a FillPath of R'") {
    // §10.12: below the effect path's no-blur threshold the coverage is R''s own (the rasterizer),
    // so the shadow equals filling the rounded rectangle R' (same flattening, same rasterizer). Its
    // distance from the exact geometric coverage is the flattening tolerance's, not the formula's.
    const int w = 128, h = 128;
    auto render = [&](CommandList& cl) {
        std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4, 0);
        SwRenderer renderer;
        renderer.setTarget(SwTarget{buf.data(), w, h, w * 4});
        renderer.clear(Color{0, 0, 0, 0});
        cl.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
        cl.devicePixelRatio = 1;
        renderer.render(cl, nullptr, nullptr);
        return buf;
    };
    CommandList shadow;
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = Rect{30.3f, 30.6f, 60, 50};
    for (float& r : bs.radii) {
        r = 9;
    }
    bs.offset = Vec2{1.5f, 2};
    bs.blur = 2.8f;
    bs.spread = 3;
    bs.color = Color{0.1f, 0.2f, 0.3f, 0.7f};
    shadow.boxShadows.push_back(bs);
    shadow.commands.push_back(Command{CmdKind::DrawBoxShadow, {}, 0});
    BoxShadowDraw d;
    REQUIRE(resolveBoxShadow(bs, Mat2x3{}, d));
    REQUIRE(d.sharp);
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    const BoxShadowShape& s = d.shape;
    // The rounded rectangle R' as the Painter's background builds one (rounded_rect.cpp).
    appendRoundedRectPath(Rect{s.x0, s.y0, s.x1 - s.x0, s.y1 - s.y0}, s.r[0], s.r[1], s.r[2], s.r[3], verbs,
                          points);
    CommandList fill;
    FillCmd fc{};
    fc.xf = Mat2x3{};
    fc.path = PathRef{0, static_cast<uint32_t>(verbs.size()), 0, static_cast<uint32_t>(points.size())};
    fc.paint.kind = PaintKind::Solid;
    fc.paint.color = bs.color;
    fc.rule = FillRule::NonZero;
    fc.antialias = 1;
    fill.verbs = verbs;
    fill.points = points;
    fill.fills.push_back(fc);
    fill.commands.push_back(Command{CmdKind::FillPath, {}, 0});
    const std::vector<uint8_t> a = render(shadow);
    const std::vector<uint8_t> b = render(fill);
    size_t differ = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        differ += a[i] != b[i] ? 1 : 0;
    }
    CHECK(differ == 0);
    // For the report: the distance from the exact (64 x 64 supersampled) coverage of R'.
    RefShape rs{s.x0, s.y0, s.x1, s.y1, {s.r[0], s.r[1], s.r[2], s.r[3]}};
    Reference ref(rs, 1.0);
    double worst = 0.0;
    for (int y = 28; y < 90; ++y) {
        for (int x = 28; x < 98; ++x) {
            worst = std::max(worst, std::fabs(a[(static_cast<size_t>(y) * w + x) * 4 + 3] / 255.0 -
                                              0.7 * ref.sharp(x, y)));
        }
    }
    MESSAGE("sharp R' vs exact geometric coverage: max error " << worst * 255.0 << " / 255 (flattening)");
}

TEST_CASE("BoxShadow: the threshold is where the effect path's three-box width reaches 3") {
    CHECK(boxShadowIsSharp(0.0f));
    CHECK(boxShadowIsSharp(1.4f));
    CHECK_FALSE(boxShadowIsSharp(1.45f));
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = Rect{0, 0, 10, 10};
    bs.color = Color{0, 0, 0, 1};
    BoxShadowDraw d;
    bs.blur = 2.8f;
    REQUIRE(resolveBoxShadow(bs, Mat2x3{}, d));
    CHECK(d.sharp);
    bs.blur = 2.9f;
    REQUIRE(resolveBoxShadow(bs, Mat2x3{}, d));
    CHECK_FALSE(d.sharp);
    // dpr 2 doubles the device sigma: 2.8 local px is no longer sharp.
    bs.blur = 2.8f;
    REQUIRE(resolveBoxShadow(bs, scaling(Vec2{2, 2}), d));
    CHECK_FALSE(d.sharp);
}

TEST_CASE("BoxShadow: blur and a positive spread are capped at 64 device px") {
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = Rect{0, 0, 10, 10};
    bs.color = Color{0, 0, 0, 1};
    bs.blur = 200.0f;
    bs.spread = 100.0f;
    BoxShadowDraw d;
    REQUIRE(resolveBoxShadow(bs, scaling(Vec2{2, 2}), d));
    CHECK(d.sigmaDevice == doctest::Approx(32.0f));
    CHECK(d.shape.x0 == doctest::Approx(-32.0f));  // spread 64 device px = 32 local px
}

TEST_CASE("BoxShadow: xt::sw's row-shared span equals the direct formula per pixel") {
    BoxShadowShape s;
    const float radii[4] = {3, 9, 0, 14};
    REQUIRE(boxShadowShape(Rect{10.25f, 7.5f, 40, 30}, radii, 2.0f, Vec2{1.5f, -2}, s));
    const float sigma = 3.7f;
    for (const Mat2x3& xf :
         {Mat2x3{}, scaling(Vec2{1.5f, 1.5f}), mul(translation(Vec2{3, 4}), rotationDeg(7.0f))}) {
        sw::PaintEval eval = sw::PaintEval::makeBoxShadow(s, sigma, Color{0.2f, 0.4f, 0.6f, 0.8f}, xf);
        Mat2x3 inv;
        REQUIRE(invert(xf, inv));
        for (int y = 0; y < 60; y += 3) {
            float out[90 * 4];
            const sw::PaintEval::SpanInfo info = eval.spanPremul(0, y, 90, 1.0f, out);
            (void)info;
            for (int i = 0; i < 90; ++i) {
                const Vec2 p = apply(inv, Vec2{static_cast<float>(i) + 0.5f, static_cast<float>(y) + 0.5f});
                const float a = 0.8f * boxShadowCoverage(s, p.x, p.y, sigma);
                if (a > 0.0f) {
                    CHECK(out[i * 4 + 3] == a);
                    CHECK(out[i * 4 + 0] == 0.2f * a);
                }
            }
        }
    }
}

// -------------------------------------------------------------------------------------------------
// The six conditions (pure function): each passes on the base input and fails just past it.
// -------------------------------------------------------------------------------------------------

namespace {

EffectInstance dropShadow(float ox, float oy, float blur, float spread) {
    EffectInstance e{};
    e.kind = EffectKind::DropShadow;
    e.enabled = 1;
    e.params[0] = ox;
    e.params[1] = oy;
    e.params[2] = blur;
    e.params[3] = spread;
    e.params[4] = 1.0f;
    e.colors[0] = Color{0, 0, 0, 0.5f};
    return e;
}

struct JudgeFixture {
    EffectInstance effects[2] = {dropShadow(4, 6, 10, 0), dropShadow(0, 1, 2, 0)};
    BoxShadowJudgeInput in;
    JudgeFixture() {
        in.effects = std::span<const EffectInstance>(effects, 1);
        in.isElement = true;
        in.backgroundDrawn = true;
        in.backgroundAlpha = 1.0f;
        in.borderBox = Rect{0, 0, 100, 60};
        in.contentKnown = true;
        in.contentBounds = Rect{0, 0, 100, 60};
    }
};

}  // namespace

TEST_CASE("BoxShadow judge: the base input converts; two drop shadows convert too") {
    JudgeFixture f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::None);
    f.in.effects = std::span<const EffectInstance>(f.effects, 2);
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::None);
}

TEST_CASE("BoxShadow judge (1): another effect kind or an inset shadow in the chain") {
    JudgeFixture f;
    f.effects[1].kind = EffectKind::Blur;
    f.in.effects = std::span<const EffectInstance>(f.effects, 2);
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::EffectChain);
    JudgeFixture g;
    g.effects[0].params[5] = 1.0f;  // inset
    CHECK(judgeBoxShadow(g.in) == BoxShadowReject::EffectChain);
}

TEST_CASE("BoxShadow judge (2): opacity just below 1, a blend mode, 3D, a faded paint alpha") {
    JudgeFixture f;
    f.in.opacity = 0.999f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::NodeLayer);
    f.in.opacity = 1.0f;
    f.in.blend = static_cast<uint8_t>(BlendMode::Multiply);
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::NodeLayer);
    f.in.blend = 0;
    f.in.in3D = true;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::NodeLayer);
    f.in.in3D = false;
    f.in.paintAlpha = 0.999f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::NodeLayer);
}

TEST_CASE("BoxShadow judge (3): background alpha just below 1, a translucent stop, hidden, clip-path") {
    JudgeFixture f;
    f.in.backgroundAlpha = 0.999f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Background);
    f.in.gradientOpaque = true;  // an opaque gradient over a translucent colour still fills the box
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::None);
    f.in.gradientOpaque = false;
    f.in.backgroundAlpha = 1.0f;
    f.in.backgroundDrawn = false;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Background);
    f.in.backgroundDrawn = true;
    f.in.clipPath = true;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Background);
    f.in.clipPath = false;
    f.in.isElement = false;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Background);
}

TEST_CASE("BoxShadow judge (4): content one hundredth of a px past the box, unknown content") {
    JudgeFixture f;
    f.in.contentBounds = Rect{0, 0, 100.01f, 60};
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::ContentOverflow);
    f.in.contentBounds = Rect{-0.01f, 0, 50, 60};
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::ContentOverflow);
    f.in.overflowClips = true;  // an overflow clip makes the overflow harmless
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::None);
    f.in.overflowClips = false;
    f.in.contentKnown = false;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::ContentOverflow);
    f.in.contentKnown = true;
    f.in.contentEmpty = true;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::None);
}

TEST_CASE("BoxShadow judge (5): an outline") {
    JudgeFixture f;
    f.in.outline = true;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Outline);
}

TEST_CASE("BoxShadow judge (6): a blur just below 0, a non-finite offset") {
    JudgeFixture f;
    f.effects[0].params[2] = -0.001f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Params);
    f.effects[0].params[2] = 10.0f;
    f.effects[0].params[0] = std::nanf("");
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Params);
}

// -------------------------------------------------------------------------------------------------
// The Painter's conversion.
// -------------------------------------------------------------------------------------------------

namespace {

NodeId makeBox(NodeArena& arena, NodeId parent, float w, float h, Color bg) {
    NodeId id = arena.create(NodeKind::Element);
    if (parent != kInvalidNode) {
        arena.appendChild(parent, id);
    }
    ElementData& data = arena.element(id);
    data.box.width = w;
    data.box.height = h;
    data.style.base[prop::BackgroundColor] = Value::ofColor(bg);
    return id;
}

void setRadius(ElementData& data, float r) {
    data.style.base[prop::BorderRadiusTL] = Value::ofLength(r, LengthUnit::Px);
    data.style.base[prop::BorderRadiusTR] = Value::ofLength(r, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBR] = Value::ofLength(r, LengthUnit::Px);
    data.style.base[prop::BorderRadiusBL] = Value::ofLength(r, LengthUnit::Px);
}

std::string paint(NodeArena& arena, NodeId root, CommandList& cmds,
                  std::vector<Painter::BoxShadowDecision>* log = nullptr) {
    cmds.viewport = Rect{-10000, -10000, 20000, 20000};
    updateWorldTransforms(arena, root, /*force=*/true);
    Painter painter;
    painter.setBoxShadowLog(log);
    painter.paint(arena, root, cmds);
    std::string out;
    dumpCommandList(cmds, out);
    return out;
}

}  // namespace

TEST_CASE("BoxShadow painter: an opaque rounded box's shadow is one DrawBoxShadow before its content") {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 120, 80, Color{0.2f, 0.4f, 0.8f, 1});
    setRadius(arena.element(root), 16);
    arena.common(root).translate = Vec2{30, 40};
    EffectInstance e = dropShadow(6, 8, 10, -3);
    e.params[4] = 0.5f;  // opacity
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
    CommandList cmds;
    std::vector<Painter::BoxShadowDecision> log;
    const std::string out = paint(arena, root, cmds, &log);
    CHECK(out.find("PushEffect") == std::string::npos);
    CHECK(out.find("PopEffect") == std::string::npos);
    CHECK(cmds.effectCmds.empty());
    CHECK(cmds.effects.empty());
    REQUIRE(cmds.commands.size() >= 2);
    REQUIRE(cmds.commands[0].kind == CmdKind::DrawBoxShadow);
    CHECK(cmds.commands[1].kind == CmdKind::FillPath);
    const BoxShadowCmd& bs = cmds.boxShadows[0];
    CHECK(bs.xf.e == doctest::Approx(30.0f));
    CHECK(bs.xf.f == doctest::Approx(40.0f));
    CHECK(bs.rect.w == doctest::Approx(120.0f));
    CHECK(bs.radii[2] == doctest::Approx(16.0f));
    CHECK(bs.offset.x == doctest::Approx(6.0f));
    CHECK(bs.blur == doctest::Approx(10.0f));
    CHECK(bs.spread == doctest::Approx(0.0f));    // a negative spread is 0, as the effect path treats it
    CHECK(bs.color.a == doctest::Approx(0.25f));  // colour alpha x opacity
    CHECK(
        out.find("DrawBoxShadow xf=[1.000 0.000 0.000 1.000 30.000 40.000] rect=(0.000,0.000,120.000,80.000) "
                 "radii=[16.000 16.000 16.000 16.000] offset=(6.000,8.000) blur=10.000 spread=0.000 "
                 "color=(0.000,0.000,0.000,0.250)") != std::string::npos);
    REQUIRE(log.size() == 1);
    CHECK(log[0].node == root);
    CHECK(log[0].reason == BoxShadowReject::None);
    CHECK(log[0].shadows == 1);
    CHECK(validateCommandList(cmds));
    // commandBounds: R' (spread 0, moved by the offset) outset by 3 sigma = 15.
    const Rect b = commandBounds(cmds, cmds.commands[0]);
    CHECK(b.x == doctest::Approx(30 + 6 - 15));
    CHECK(b.w == doctest::Approx(120 + 30));
}

TEST_CASE("BoxShadow painter: two drop shadows -> two commands, the last effect drawn first") {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 50, 50, Color{1, 1, 1, 1});
    const EffectInstance e[2] = {dropShadow(1, 1, 4, 0), dropShadow(9, 9, 20, 2)};
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(e, 2), {}) == Status::kOk);
    CommandList cmds;
    paint(arena, root, cmds);
    REQUIRE(cmds.commands.size() >= 3);
    CHECK(cmds.commands[0].kind == CmdKind::DrawBoxShadow);
    CHECK(cmds.commands[1].kind == CmdKind::DrawBoxShadow);
    CHECK(cmds.boxShadows[cmds.commands[0].index].offset.x == doctest::Approx(9.0f));
    CHECK(cmds.boxShadows[cmds.commands[1].index].offset.x == doctest::Approx(1.0f));
}

TEST_CASE("BoxShadow painter: a child's effect keeps a valid payload after the parent converts") {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 100, 100, Color{1, 1, 1, 1});
    NodeId child = makeBox(arena, root, 40, 40, Color{1, 0, 0, 0.5f});
    arena.common(child).translate = Vec2{20, 20};
    EffectInstance blur{};
    blur.kind = EffectKind::Blur;
    blur.enabled = 1;
    blur.params[0] = blur.params[1] = 2;
    const EffectInstance shadow = dropShadow(0, 4, 8, 0);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);
    REQUIRE(arena.assignEffects(child, std::span<const EffectInstance>(&blur, 1), {}) == Status::kOk);
    CommandList cmds;
    const std::string out = paint(arena, root, cmds);
    CHECK(cmds.commands[0].kind == CmdKind::DrawBoxShadow);
    REQUIRE(cmds.effectCmds.size() == 1);
    REQUIRE(cmds.effects.size() == 1);
    CHECK(cmds.effects[cmds.effectCmds[0].effectOffset].kind == EffectKind::Blur);
    bool found = false;
    for (const Command& c : cmds.commands) {
        if (c.kind == CmdKind::PushEffect) {
            CHECK(c.index == 0);
            found = true;
        }
    }
    CHECK(found);
    CHECK(validateCommandList(cmds));
}

TEST_CASE(
    "BoxShadow painter: kept as PushEffect -- blur in the chain, outline, overflowing child, "
    "translucent background") {
    struct Variant {
        const char* name;
        BoxShadowReject reason;
    };
    const Variant variants[] = {
        {"blur", BoxShadowReject::EffectChain},
        {"outline", BoxShadowReject::Outline},  // T-680: a translucent outline (an opaque one converts)
        {"child", BoxShadowReject::ContentOverflow},
        {"translucent", BoxShadowReject::Background},
    };
    for (const Variant& v : variants) {
        CAPTURE(v.name);
        NodeArena arena;
        const std::string name = v.name;
        NodeId root = makeBox(arena, kInvalidNode, 80, 60,
                              name == "translucent" ? Color{1, 1, 1, 0.9f} : Color{1, 1, 1, 1});
        std::vector<EffectInstance> fx{dropShadow(3, 3, 6, 0)};
        if (name == "blur") {
            EffectInstance blur{};
            blur.kind = EffectKind::Blur;
            blur.enabled = 1;
            blur.params[0] = blur.params[1] = 2;
            fx.push_back(blur);
        }
        if (name == "outline") {
            ElementData& data = arena.element(root);
            data.style.outlineStyle = 1;
            data.style.base[prop::OutlineWidth] = Value::ofLength(2, LengthUnit::Px);
            data.style.base[prop::OutlineOffset] = Value::ofLength(1, LengthUnit::Px);
            data.style.base[prop::OutlineColor] = Value::ofColor(Color{0, 0, 1, 0.5f});
        }
        if (name == "child") {
            NodeId child = makeBox(arena, root, 30, 30, Color{0, 1, 0, 1});
            arena.common(child).translate = Vec2{60, 10};  // reaches x = 90 > 80
        }
        REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(fx.data(), fx.size()), {}) ==
                Status::kOk);
        CommandList cmds;
        std::vector<Painter::BoxShadowDecision> log;
        const std::string out = paint(arena, root, cmds, &log);
        CHECK(out.find("PushEffect") != std::string::npos);
        CHECK(out.find("DrawBoxShadow") == std::string::npos);
        REQUIRE(log.size() == 1);
        CHECK(log[0].reason == v.reason);
    }
}

TEST_CASE("BoxShadow painter: an overflowing child under overflow: hidden still converts") {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 80, 60, Color{1, 1, 1, 1});
    arena.element(root).style.overflowX = arena.element(root).style.overflowY = Overflow::Hidden;
    NodeId child = makeBox(arena, root, 30, 30, Color{0, 1, 0, 1});
    arena.common(child).translate = Vec2{60, 10};
    const EffectInstance shadow = dropShadow(3, 3, 6, 0);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);
    CommandList cmds;
    const std::string out = paint(arena, root, cmds);
    CHECK(out.find("DrawBoxShadow") != std::string::npos);
    CHECK(out.find("PushEffect") == std::string::npos);
}

TEST_CASE("BoxShadow painter: a rotated card's centred child is judged in the card's own frame") {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 100, 60, Color{1, 1, 1, 1});
    arena.common(root).rotation = 7.0f;
    NodeId child = makeBox(arena, root, 90, 50, Color{0, 0, 1, 1});
    arena.common(child).translate = Vec2{5, 5};
    const EffectInstance shadow = dropShadow(0, 6, 12, 0);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&shadow, 1), {}) == Status::kOk);
    CommandList cmds;
    const std::string out = paint(arena, root, cmds);
    CHECK(out.find("DrawBoxShadow") != std::string::npos);
}

TEST_CASE("BoxShadow simplifier: dropped with the effects, kept with the layer opacity folded in") {
    CommandList cl;
    cl.viewport = Rect{0, 0, 100, 100};
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = Rect{10, 10, 20, 20};
    bs.blur = 4;
    bs.color = Color{0, 0, 0, 0.8f};
    cl.boxShadows.push_back(bs);
    cl.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, Rect{0, 0, 100, 100}});
    cl.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
    cl.commands.push_back(Command{CmdKind::DrawBoxShadow, {}, 0});
    cl.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    REQUIRE(validateCommandList(cl));
    CommandListSimplifier simplifier;
    SimplifyOptions options;
    SimplifyStats stats = simplifier.simplify(cl, options, nullptr);
    (void)stats;
    CHECK(cl.boxShadows.empty());
    CommandList kept;
    kept.viewport = Rect{0, 0, 100, 100};
    kept.boxShadows.push_back(bs);
    kept.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, Rect{0, 0, 100, 100}});
    kept.commands.push_back(Command{CmdKind::PushLayer, {}, 0});
    kept.commands.push_back(Command{CmdKind::DrawBoxShadow, {}, 0});
    kept.commands.push_back(Command{CmdKind::PopLayer, {}, 0});
    options.effects = false;
    simplifier.simplify(kept, options, nullptr);
    REQUIRE(kept.boxShadows.size() == 1);
    CHECK(kept.boxShadows[0].color.a == doctest::Approx(0.4f));
}

// -------------------------------------------------------------------------------------------------
// The golden fixtures (Tests/golden/ui): every shadow of ui_box_shadow converts, none of
// ui_box_shadow_kept does (T-668 item 5).
// -------------------------------------------------------------------------------------------------

namespace {

void countFixtureCommands(const char* file, float time, size_t& boxShadows, size_t& pushEffects) {
    const std::filesystem::path path =
        std::filesystem::path(__FILE__).parent_path() / ".." / "golden" / "ui" / file;
    compile::CompileOptions options;
    options.rootDir = path.parent_path().string();
    options.strict = false;
    compile::CompileResult result = compile::compileDocument(path.string(), options);
    REQUIRE(result.ok());
    NullTextShaper shaper;
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 480, 320});
    doc->update(time);
    CommandList list;
    doc->paint(list);
    boxShadows = 0;
    pushEffects = 0;
    for (const Command& c : list.commands) {
        boxShadows += c.kind == CmdKind::DrawBoxShadow ? 1 : 0;
        pushEffects += c.kind == CmdKind::PushEffect ? 1 : 0;
    }
}

}  // namespace

TEST_CASE("BoxShadow fixtures: ui_box_shadow converts all 12 shadows, ui_box_shadow_kept 1 of its 4") {
    size_t boxShadows = 0, pushEffects = 0;
    countFixtureCommands("ui_box_shadow.xt", 0.4f, boxShadows, pushEffects);
    CHECK(boxShadows == 12);
    CHECK(pushEffects == 0);
    // T-680 ("補充 2" B.1): its #outline (2 px solid #202020, offset 2 px) converts now.
    countFixtureCommands("ui_box_shadow_kept.xt", 0.0f, boxShadows, pushEffects);
    CHECK(boxShadows == 1);
    CHECK(pushEffects == 3);
}

// =================================================================================================
// T-680 (Docs/design/10-backends.md "10.12 定案 2 補充 2", Docs/tasks/T-680.md item 5): the
// relaxations B.1 (a solid opaque outline: signed terms) and B.2 (the font-metric glyph box), each
// with a case that converts and one just past it. B.3 (a near-opaque background) is not part of
// this change: its fixture cell differed from the offscreen path by more than the golden tolerance
// (the task's stop condition).
// =================================================================================================

namespace {

void setOutline(ElementData& data, float width, float offset, Color color, uint8_t style = 1) {
    data.style.outlineStyle = style;
    data.style.base[prop::OutlineWidth] = Value::ofLength(width, LengthUnit::Px);
    data.style.base[prop::OutlineOffset] = Value::ofLength(offset, LengthUnit::Px);
    data.style.base[prop::OutlineColor] = Value::ofColor(color);
}

/// Ascent 0.8 em, descent -0.2 em; every glyph's outline spans x 0..0.5 em.
class MetricShaper : public NullTextShaper {
public:
    FontMetrics metrics(FontId) override {
        FontMetrics m;
        m.ascent = 0.8f;
        m.descent = -0.2f;
        return m;
    }
    bool glyphOutline(FontId, uint32_t, std::vector<PathVerb>& verbs, std::vector<float>& points) override {
        verbs = {PathVerb::Move, PathVerb::Line, PathVerb::Line, PathVerb::Close};
        points = {0.0f, -0.7f, 0.5f, -0.7f, 0.25f, 0.1f};
        return true;
    }
};

/// One glyph of size 20 at `origin` (identity transform): with MetricShaper its box is
/// [origin.x - 10, origin.x + 20] x [origin.y - 16, origin.y + 4]; without a shaper the origins
/// grown by the size, [origin.x - 20, origin.x + 20] x [origin.y - 20, origin.y + 20].
CommandList oneGlyphRun(Vec2 origin) {
    CommandList cl;
    GlyphsCmd g{};
    g.xf = Mat2x3{};
    g.size = 20.0f;
    g.color = Color{0, 0, 0, 1};
    g.glyphOffset = 0;
    g.glyphCount = 1;
    cl.glyphs.push_back(7);
    cl.glyphPos.push_back(origin);
    cl.glyphRuns.push_back(g);
    cl.commands.push_back(Command{CmdKind::DrawGlyphs, {}, 0});
    return cl;
}

/// The DrawBoxShadow commands of `cmds` alone, rendered with xt::sw into a transparent w x h target.
std::vector<uint8_t> renderShadowsOnly(const CommandList& cmds, int w, int h) {
    CommandList one = cmds;
    one.commands.clear();
    for (const Command& c : cmds.commands) {
        if (c.kind == CmdKind::DrawBoxShadow) {
            one.commands.push_back(c);
        }
    }
    one.viewport = Rect{0, 0, static_cast<float>(w), static_cast<float>(h)};
    one.devicePixelRatio = 1;
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), w, h, w * 4});
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(one, nullptr, nullptr);
    return buf;
}

/// Brute-force reference of an outlined box's shadow: the union of the box and the ring (outer
/// edge minus inner edge), each its own rounded rectangle in double precision, sampled 64 x 64 per
/// pixel and convolved with a Gaussian of sigma, like Reference above but over interval sets.
class UnionReference {
public:
    UnionReference(const RefShape& box, const RefShape& outer, const RefShape& inner, bool innerEmpty,
                   double sigma)
        : m_box(box), m_outer(outer), m_inner(inner), m_innerEmpty(innerEmpty), m_sigma(sigma) {
        m_x0 = std::min(box.x0, outer.x0);
        m_y0 = std::min(box.y0, outer.y0);
        const double x1 = std::max(box.x1, outer.x1);
        const double y1 = std::max(box.y1, outer.y1);
        m_nx = static_cast<int>(std::ceil((x1 - m_x0) * kSub));
        m_ny = static_cast<int>(std::ceil((y1 - m_y0) * kSub));
    }

    double blurred(double px, double py) const {
        const double h = 1.0 / kSub;
        const double inv2s2 = 1.0 / (2.0 * m_sigma * m_sigma);
        std::vector<double> prefix(static_cast<size_t>(m_nx) + 1, 0.0);
        for (int i = 0; i < m_nx; ++i) {
            const double dx = m_x0 + (i + 0.5) * h - px;
            prefix[static_cast<size_t>(i) + 1] = prefix[static_cast<size_t>(i)] + std::exp(-dx * dx * inv2s2);
        }
        double sum = 0.0;
        std::vector<std::pair<double, double>> iv;
        for (int j = 0; j < m_ny; ++j) {
            const double yc = m_y0 + (j + 0.5) * h;
            const double dy = yc - py;
            if (std::fabs(dy) > 6.0 * m_sigma) {
                continue;
            }
            iv.clear();
            double a0 = 0, a1 = 0, b0 = 0, b1 = 0, c0 = 0, c1 = 0;
            if (refSpan(m_outer, yc, a0, a1)) {
                if (!m_innerEmpty && refSpan(m_inner, yc, b0, b1)) {
                    iv.push_back({a0, std::min(a1, b0)});
                    iv.push_back({std::max(a0, b1), a1});
                } else {
                    iv.push_back({a0, a1});
                }
            }
            if (refSpan(m_box, yc, c0, c1)) {
                iv.push_back({c0, c1});
            }
            std::sort(iv.begin(), iv.end());
            double rowSum = 0.0;
            auto flush = [&](double l, double r) {
                int lo = static_cast<int>(std::ceil((l - m_x0) * kSub - 0.5));
                int hi = static_cast<int>(std::floor((r - m_x0) * kSub - 0.5));
                lo = std::max(lo, 0);
                hi = std::min(hi, m_nx - 1);
                if (lo <= hi) {
                    rowSum += prefix[static_cast<size_t>(hi) + 1] - prefix[static_cast<size_t>(lo)];
                }
            };
            bool open = false;
            double curL = 0, curR = 0;
            for (const auto& span : iv) {
                if (!(span.second > span.first)) {
                    continue;
                }
                if (open && span.first <= curR) {
                    curR = std::max(curR, span.second);
                } else {
                    if (open) {
                        flush(curL, curR);
                    }
                    curL = span.first;
                    curR = span.second;
                    open = true;
                }
            }
            if (open) {
                flush(curL, curR);
            }
            sum += std::exp(-dy * dy * inv2s2) * rowSum;
        }
        return sum * h * h / (2.0 * 3.14159265358979323846 * m_sigma * m_sigma);
    }

private:
    RefShape m_box, m_outer, m_inner;
    bool m_innerEmpty;
    double m_sigma;
    double m_x0 = 0, m_y0 = 0;
    int m_nx = 0, m_ny = 0;
};

/// The rectangle (x, y, w, h) grown by `g` on every side, each radius + g (at least 0), moved by
/// (dx, dy).
RefShape refGrow(double x, double y, double w, double h, const double radii[4], double g, double dx,
                 double dy) {
    RefShape s{x - g + dx, y - g + dy, x + w + g + dx, y + h + g + dy, {0, 0, 0, 0}};
    for (int i = 0; i < 4; ++i) {
        s.r[i] = std::max(0.0, radii[i] + g);
    }
    return s;
}

struct OutlineCase {
    const char* name;
    float width, offset;         // the outline
    float ox, oy, blur, spread;  // the drop shadow
};

/// Paints a 60 x 40 box (radius 8) at (50, 50) with a solid opaque outline and `effect: shadow()`,
/// renders its DrawBoxShadow with xt::sw and returns the largest |alpha / 255 - reference| over
/// every second pixel. The reference is the Gaussian blur of the box plus the ring, grown by the
/// spread (the box and the ring's outer edge grown, its inner edge shrunk), built here from the
/// outline geometry of 05 §5.9.8 (inner radius r + offset, outer radius inner + width).
double outlineCaseError(const OutlineCase& c, uint32_t& extraCount) {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 60, 40, Color{0.2f, 0.4f, 0.8f, 1});
    setRadius(arena.element(root), 8);
    setOutline(arena.element(root), c.width, c.offset, Color{0.9f, 0.1f, 0.1f, 1});
    arena.common(root).translate = Vec2{50, 50};
    EffectInstance e = dropShadow(c.ox, c.oy, c.blur, c.spread);
    e.colors[0] = Color{0, 0, 0, 1};
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
    CommandList cmds;
    std::vector<Painter::BoxShadowDecision> log;
    paint(arena, root, cmds, &log);
    REQUIRE(log.size() == 1);
    REQUIRE(log[0].reason == BoxShadowReject::None);
    REQUIRE(cmds.boxShadows.size() == 1);
    extraCount = cmds.boxShadows[0].extraCount;
    const int w = 180, h = 160;
    const std::vector<uint8_t> buf = renderShadowsOnly(cmds, w, h);

    const double r = 8, o = c.offset, wd = c.width, s = c.spread;
    const double dx = 50 + c.ox, dy = 50 + c.oy;
    const double boxR[4] = {r, r, r, r};
    const double ri = std::max(0.0, r + o);
    const double ro = ri > 0 ? ri + wd : 0.0;
    const double innerR[4] = {ri, ri, ri, ri};
    const double outerR[4] = {ro, ro, ro, ro};
    const RefShape box = refGrow(0, 0, 60, 40, boxR, s, dx, dy);
    const RefShape outer =
        refGrow(-(o + wd), -(o + wd), 60 + 2 * (o + wd), 40 + 2 * (o + wd), outerR, s, dx, dy);
    const RefShape inner = refGrow(-o, -o, 60 + 2 * o, 40 + 2 * o, innerR, -s, dx, dy);
    const bool innerEmpty = !(inner.x1 > inner.x0 && inner.y1 > inner.y0);
    const double sigma = 0.5 * c.blur;
    UnionReference ref(box, outer, inner, innerEmpty, sigma);
    const double reach = 3.0 * sigma + 1.0;
    const int ix0 = std::max(0, static_cast<int>(std::floor(std::min(box.x0, outer.x0) - reach)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(std::min(box.y0, outer.y0) - reach)));
    const int ix1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(box.x1, outer.x1) + reach)));
    const int iy1 = std::min(h - 1, static_cast<int>(std::ceil(std::max(box.y1, outer.y1) + reach)));
    double worst = 0.0;
    for (int y = iy0; y <= iy1; y += 2) {
        for (int x = ix0; x <= ix1; x += 2) {
            const double expected = ref.blurred(x + 0.5, y + 0.5);
            const double got = buf[(static_cast<size_t>(y) * w + x) * 4 + 3] / 255.0;
            worst = std::max(worst, std::fabs(got - expected));
        }
    }
    return worst;
}

}  // namespace

TEST_CASE(
    "BoxShadow T-680 (B.1): outlined boxes against a brute-force blur of the box plus the ring, <= 2/255") {
    // Outline offsets positive (a gap: three terms), zero and negative (one term, O_out or R), a
    // ring wider than 3 sigma and one under 1 px, a spread that leaves the gap open and two that
    // close it (the second like Sherefox's litpulse: 3 px at offset 1 px, spread 6 px).
    const struct {
        OutlineCase c;
        uint32_t extra;
    } cases[] = {
        {{"gap: offset +4, width 3", 3, 4, 2, 3, 8, 0}, 2},
        {{"touching: offset 0, width 3", 3, 0, 2, 3, 8, 0}, 0},
        {{"overlap: offset -2, width 4", 4, -2, -2, 3, 8, 0}, 0},
        {{"inside: offset -6, width 2", 2, -6, 0, 3, 8, 0}, 0},
        {{"gap, ring wider than 3 sigma: offset +3, width 12, blur 6", 12, 3, 1, 2, 6, 0}, 2},
        {{"gap, ring under 1 px: offset +2, width 0.6, blur 6", 0.6f, 2, 1, 2, 6, 0}, 2},
        {{"gap kept open by a spread: offset +6, width 3, spread 2", 3, 6, 0, 2, 8, 2}, 2},
        {{"gap closed by a spread: offset +3, width 3, spread 2", 3, 3, 0, 2, 8, 2}, 2},
        {{"litpulse: offset +1, width 3, blur 22, spread 6", 3, 1, 0, 0, 22, 6}, 2},
    };
    for (const auto& k : cases) {
        CAPTURE(k.c.name);
        uint32_t extra = 99;
        const double err = outlineCaseError(k.c, extra);
        MESSAGE(std::string(k.c.name) << ": max error " << err * 255.0 << " / 255, extra terms " << extra);
        CHECK(extra == k.extra);
        CHECK(err <= 2.0 / 255.0);
    }
}

TEST_CASE("BoxShadow T-680 (B.1): the terms the Painter emits for an outline, and the dump") {
    struct Case {
        float width, offset;
        uint32_t extra;
        bool outerShape;  // the first term is O_out (else R)
    };
    const Case cases[] = {{3, 4, 2, true}, {3, 0, 0, true}, {4, -2, 0, true}, {2, -6, 0, false}};
    for (const Case& c : cases) {
        CAPTURE(c.offset);
        NodeArena arena;
        NodeId root = makeBox(arena, kInvalidNode, 60, 40, Color{1, 1, 1, 1});
        setRadius(arena.element(root), 8);
        setOutline(arena.element(root), c.width, c.offset, Color{0, 0, 0, 1});
        const EffectInstance e = dropShadow(2, 3, 8, 0);
        REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
        CommandList cmds;
        std::vector<Painter::BoxShadowDecision> log;
        const std::string out = paint(arena, root, cmds, &log);
        REQUIRE(log.size() == 1);
        CHECK(log[0].reason == BoxShadowReject::None);
        REQUIRE(cmds.boxShadows.size() == 1);
        const BoxShadowCmd& bs = cmds.boxShadows[0];
        CHECK(bs.extraCount == c.extra);
        const float grow = c.outerShape ? c.offset + c.width : 0.0f;
        CHECK(bs.rect.x == doctest::Approx(-grow));
        CHECK(bs.rect.w == doctest::Approx(60 + 2 * grow));
        if (c.extra == 2) {
            CHECK(bs.extra[0].sign == -1.0f);
            CHECK(bs.extra[0].rect.x == doctest::Approx(-c.offset));
            CHECK(bs.extra[0].radii[0] == doctest::Approx(8 + c.offset));
            CHECK(bs.extra[1].sign == 1.0f);
            CHECK(bs.extra[1].rect.w == doctest::Approx(60.0f));
            CHECK(bs.radii[0] == doctest::Approx(8 + c.offset + c.width));
            CHECK(out.find(" term=(-1.000 (-4.000,-4.000,68.000,48.000) [12.000 12.000 12.000 12.000]) "
                           "term=(1.000 (0.000,0.000,60.000,40.000) [8.000 8.000 8.000 8.000])") !=
                  std::string::npos);
        } else {
            CHECK(out.find("term=") == std::string::npos);
        }
        CHECK(validateCommandList(cmds));
    }
}

TEST_CASE("BoxShadow T-680 judge (5): a solid opaque outline converts; a dashed or translucent one not") {
    JudgeFixture f;
    f.in.outline = true;
    f.in.outlineSolid = true;
    f.in.outlineAlpha = 1.0f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::None);
    f.in.outlineSolid = false;  // dashed / dotted
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Outline);
    f.in.outlineSolid = true;
    f.in.outlineAlpha = 0.999f;
    CHECK(judgeBoxShadow(f.in) == BoxShadowReject::Outline);
}

TEST_CASE("BoxShadow T-680 painter: a translucent outline keeps the effect, an opaque one converts") {
    for (float alpha : {0.99f, 1.0f}) {
        CAPTURE(alpha);
        NodeArena arena;
        NodeId root = makeBox(arena, kInvalidNode, 60, 40, Color{1, 1, 1, 1});
        setOutline(arena.element(root), 2, 2, Color{0, 0, 0, alpha});
        const EffectInstance e = dropShadow(2, 3, 8, 0);
        REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
        CommandList cmds;
        std::vector<Painter::BoxShadowDecision> log;
        paint(arena, root, cmds, &log);
        REQUIRE(log.size() == 1);
        CHECK(log[0].reason == (alpha == 1.0f ? BoxShadowReject::None : BoxShadowReject::Outline));
    }
}

TEST_CASE("BoxShadow T-680 (B.2): a glyph run judged by its font-metric box; 1 px past it is overflow") {
    MetricShaper shaper;
    JudgeFixture f;
    f.in.borderBox = Rect{0, 0, 100, 40};
    auto judgeAt = [&](Vec2 origin, TextShaper* sh) {
        const CommandList cl = oneGlyphRun(origin);
        Rect r;
        bool empty = true;
        REQUIRE(commandBoundsInFrame(cl, cl.commands[0], Mat2x3{}, r, empty, sh));
        REQUIRE_FALSE(empty);
        f.in.contentBounds = r;
        return judgeBoxShadow(f.in);
    };
    // Metric box [0, 30] x [20, 40]: inside the border box, touching its left and bottom edges.
    CHECK(judgeAt(Vec2{10, 36}, &shaper) == BoxShadowReject::None);
    CHECK(f.in.contentBounds.y + f.in.contentBounds.h == doctest::Approx(40.0f));
    CHECK(f.in.contentBounds.x == doctest::Approx(0.0f));
    CHECK(f.in.contentBounds.x + f.in.contentBounds.w == doctest::Approx(30.0f));  // 0.5 em + half a size
    // The coarse box (origins grown by the size) reaches 20 px past it.
    CHECK(judgeAt(Vec2{10, 36}, nullptr) == BoxShadowReject::ContentOverflow);
    // One px lower / one px further left: the metric box is 1 px past the border box.
    CHECK(judgeAt(Vec2{10, 37}, &shaper) == BoxShadowReject::ContentOverflow);
    CHECK(judgeAt(Vec2{9, 36}, &shaper) == BoxShadowReject::ContentOverflow);
    // Without usable metrics the coarse box stays.
    NullTextShaper none;
    CHECK(judgeAt(Vec2{10, 36}, &none) == BoxShadowReject::ContentOverflow);
}

TEST_CASE("BoxShadow T-680: glyphRunInkBounds is glyphRunInkBox under the run's transform") {
    MetricShaper shaper;
    CommandList cl = oneGlyphRun(Vec2{10, 36});
    cl.glyphRuns[0].xf = mul(translation(Vec2{5, 7}), scaling(Vec2{2, 2}));
    Rect local, world;
    REQUIRE(glyphRunInkBox(cl, cl.glyphRuns[0], &shaper, local));
    REQUIRE(glyphRunInkBounds(cl, cl.glyphRuns[0], &shaper, world));
    CHECK(world.x == doctest::Approx(5 + 2 * local.x));
    CHECK(world.y == doctest::Approx(7 + 2 * local.y));
    CHECK(world.w == doctest::Approx(2 * local.w));
    CHECK(world.h == doctest::Approx(2 * local.h));
}

TEST_CASE("BoxShadow T-680: the node's own outline ring is contour, a child's outline is content") {
    const EffectInstance e = dropShadow(0, 2, 6, 0);
    {
        NodeArena arena;
        NodeId root = makeBox(arena, kInvalidNode, 60, 40, Color{1, 1, 1, 1});
        setOutline(arena.element(root), 3, 5, Color{0, 0, 0, 1});  // far past the border box
        REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
        CommandList cmds;
        std::vector<Painter::BoxShadowDecision> log;
        paint(arena, root, cmds, &log);
        REQUIRE(log.size() == 1);
        CHECK(log[0].reason == BoxShadowReject::None);
    }
    {
        NodeArena arena;
        NodeId parent = makeBox(arena, kInvalidNode, 60, 40, Color{1, 1, 1, 1});
        NodeId child = makeBox(arena, parent, 20, 20, Color{1, 0, 0, 1});
        arena.common(child).translate = Vec2{20, 10};
        setOutline(arena.element(child), 2, 30, Color{0, 0, 0, 1});
        REQUIRE(arena.assignEffects(parent, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
        CommandList cmds;
        std::vector<Painter::BoxShadowDecision> log;
        paint(arena, parent, cmds, &log);
        REQUIRE(log.size() == 1);
        CHECK(log[0].reason == BoxShadowReject::ContentOverflow);
    }
}

TEST_CASE("BoxShadow T-680: with box shadow commands off nothing is considered and the effect stays") {
    NodeArena arena;
    NodeId root = makeBox(arena, kInvalidNode, 60, 40, Color{1, 1, 1, 1});
    const EffectInstance e = dropShadow(2, 3, 8, 0);
    REQUIRE(arena.assignEffects(root, std::span<const EffectInstance>(&e, 1), {}) == Status::kOk);
    CommandList cmds;
    cmds.viewport = Rect{-10000, -10000, 20000, 20000};
    updateWorldTransforms(arena, root, /*force=*/true);
    std::vector<Painter::BoxShadowDecision> log;
    Painter painter;
    painter.setBoxShadowLog(&log);
    painter.setBoxShadowCommands(false);
    painter.paint(arena, root, cmds);
    CHECK(log.empty());
    CHECK(cmds.boxShadows.empty());
    REQUIRE(cmds.commands.size() >= 2);
    CHECK(cmds.commands[0].kind == CmdKind::PushEffect);
}

TEST_CASE("BoxShadow T-680: xt::sw's row-shared three-term span equals boxShadowCoverageSum per pixel") {
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = Rect{3, 4, 50, 36};
    for (float& r : bs.radii) {
        r = 12;
    }
    bs.extraCount = 2;
    bs.extra[0] = BoxShadowTerm{Rect{8, 9, 40, 26}, {7, 7, 7, 7}, -1.0f};
    bs.extra[1] = BoxShadowTerm{Rect{12, 13, 32, 18}, {3, 3, 3, 3}, 1.0f};
    bs.offset = Vec2{1.5f, 2};
    bs.blur = 7.4f;
    bs.color = Color{0.2f, 0.4f, 0.6f, 0.8f};
    BoxShadowDraw d;
    REQUIRE(resolveBoxShadow(bs, Mat2x3{}, d));
    REQUIRE(d.extraCount == 2);
    for (const Mat2x3& xf : {Mat2x3{}, mul(translation(Vec2{3, 4}), rotationDeg(7.0f))}) {
        sw::PaintEval eval = sw::PaintEval::makeBoxShadow(d, d.sigmaLocal, Color{0.2f, 0.4f, 0.6f, 0.8f}, xf);
        Mat2x3 inv;
        REQUIRE(invert(xf, inv));
        for (int y = 0; y < 60; y += 3) {
            float out[70 * 4];
            (void)eval.spanPremul(0, y, 70, 1.0f, out);
            for (int i = 0; i < 70; ++i) {
                const Vec2 p = apply(inv, Vec2{static_cast<float>(i) + 0.5f, static_cast<float>(y) + 0.5f});
                const float a = 0.8f * boxShadowCoverageSum(d, p.x, p.y, d.sigmaLocal);
                if (a > 0.0f) {
                    CHECK(out[i * 4 + 3] == doctest::Approx(a).epsilon(1e-6));
                }
            }
        }
    }
}

TEST_CASE("BoxShadow T-680: below the threshold the three terms fill as the ring plus the box") {
    BoxShadowCmd bs{};
    bs.xf = Mat2x3{};
    bs.rect = Rect{10, 10, 80, 60};
    bs.extraCount = 2;
    bs.extra[0] = BoxShadowTerm{Rect{16, 16, 68, 48}, {0, 0, 0, 0}, -1.0f};
    bs.extra[1] = BoxShadowTerm{Rect{20, 20, 60, 40}, {0, 0, 0, 0}, 1.0f};
    bs.blur = 1.0f;  // sharp
    bs.color = Color{0, 0, 0, 1};
    CommandList cl;
    cl.boxShadows.push_back(bs);
    cl.commands.push_back(Command{CmdKind::DrawBoxShadow, {}, 0});
    const std::vector<uint8_t> buf = renderShadowsOnly(cl, 100, 80);
    auto alphaAt = [&](int x, int y) { return buf[(static_cast<size_t>(y) * 100 + x) * 4 + 3]; };
    CHECK(alphaAt(12, 40) == 255);  // the ring
    CHECK(alphaAt(18, 40) == 0);    // the gap
    CHECK(alphaAt(50, 40) == 255);  // the box
    CHECK(alphaAt(5, 40) == 0);     // outside
}
