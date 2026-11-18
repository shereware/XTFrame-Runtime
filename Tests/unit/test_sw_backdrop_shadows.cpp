#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/node.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-729 (Docs/design/10-backends.md §10.18 "B 定案", Docs/tasks/T-729.md "測試"): a stack that is
/// BackdropBlur followed only by shadow-type effects (outer / inset drop shadow, glow, inner glow)
/// runs those effects on the element's own content S (R'), then puts R' over the blurred backdrop B
/// masked by S's alpha: out = R' + B * S.a * (1 - R'.a). The shadows no longer contain the
/// backdrop's alpha. Every other stack keeps its sequential order, byte for byte.
///
/// The panel is a 32 x 32 rectangle on whole pixels filled with white at alpha 0.5, so S.a is exactly
/// 0.5 inside it and exactly 0 outside. Targets are premultiplied RGBA8.

using namespace xt;

namespace {

constexpr int kW = 64, kH = 64;
const Rect kPanel{16, 16, 32, 32};
const Rect kAll{0, 0, 64, 64};
const Color kUniform{0.2f, 0.6f, 0.4f, 1};

struct Image {
    std::vector<uint8_t> px;
    float at(int x, int y, int c) const {
        return static_cast<float>(
                   px[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4 + static_cast<size_t>(c)]) /
               255.0f;
    }
    uint64_t hash() const {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t b : px) {
            h = (h ^ b) * 1099511628211ull;
        }
        return h;
    }
};

void fillRect(CommandList& cl, Rect r, Color color) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    const float pts[8] = {r.x, r.y, r.x + r.w, r.y, r.x + r.w, r.y + r.h, r.x, r.y + r.h};
    for (int i = 0; i < 4; ++i) {
        cl.verbs.push_back(i == 0 ? PathVerb::Move : PathVerb::Line);
        cl.points.push_back(pts[i * 2]);
        cl.points.push_back(pts[i * 2 + 1]);
    }
    cl.verbs.push_back(PathVerb::Close);
    ref.verbCount = 5;
    ref.pointCount = 8;
    FillCmd f{};
    f.path = ref;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    cl.commands.push_back(Command{CmdKind::FillPath, {}, static_cast<uint32_t>(cl.fills.size() - 1)});
}

EffectInstance fx(EffectKind kind, std::initializer_list<float> params, Color color = Color{0, 0, 0, 1}) {
    EffectInstance e{};
    e.kind = kind;
    int i = 0;
    for (float p : params) {
        e.params[i++] = p;
    }
    e.colors[0] = color;
    return e;
}

/// Saturation and brightness 0 by default: the formula checks compare with the backdrop as it is.
EffectInstance backdrop(float saturation = 0, float brightness = 0) {
    return fx(EffectKind::BackdropBlur, {8, saturation, brightness});
}
EffectInstance outer(float oy = 6, float blur = 8) {
    return fx(EffectKind::DropShadow, {2, oy, blur, 0, 1, 0}, Color{0.05f, 0, 0.1f, 0.8f});
}
EffectInstance inset() { return fx(EffectKind::DropShadow, {0, 3, 6, 0, 1, 1}, Color{0, 0, 0.2f, 0.7f}); }
EffectInstance glow(bool inner) {
    return fx(EffectKind::Glow, {8, 1.3f, inner ? 1.0f : 0.0f}, Color{1, 0.8f, 0.2f, 1});
}

/// `backdropKind` 0: the uniform colour kUniform; 1: two hard-edged halves; 2: nothing (transparent).
Image render(std::initializer_list<EffectInstance> effects, int backdropKind) {
    CommandList cl;
    cl.viewport = kAll;
    cl.devicePixelRatio = 1;
    if (backdropKind == 0) {
        fillRect(cl, kAll, kUniform);
    } else if (backdropKind == 1) {
        fillRect(cl, Rect{0, 0, 32, 64}, Color{0.85f, 0.25f, 0.2f, 1});
        fillRect(cl, Rect{32, 0, 32, 64}, Color{0.15f, 0.4f, 0.8f, 1});
    }
    EffectCmd e{};
    e.effectOffset = static_cast<uint32_t>(cl.effects.size());
    for (const EffectInstance& f : effects) {
        cl.effects.push_back(f);
    }
    e.effectCount = static_cast<uint32_t>(effects.size());
    e.xf = Mat2x3{};
    e.boundsHint = kAll;
    e.contentBounds = kPanel;
    cl.effectCmds.push_back(e);
    cl.commands.push_back(Command{CmdKind::PushEffect, {}, 0});
    fillRect(cl, kPanel, Color{1, 1, 1, 0.5f});
    cl.commands.push_back(Command{CmdKind::PopEffect, {}, 0});

    Image img;
    img.px.assign(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{img.px.data(), kW, kH, kW * 4};
    target.premultiplied = true;  // R' read back as the effect layer holds it
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(cl, nullptr, nullptr);
    return img;
}

bool insidePanel(int x, int y) { return x >= 16 && x < 48 && y >= 16 && y < 48; }

/// Every pixel outside the panel is the same byte in both images.
size_t outsideDiffer(const Image& a, const Image& b) {
    size_t n = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (insidePanel(x, y)) {
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                n += a.at(x, y, c) != b.at(x, y, c) ? 1u : 0u;
            }
        }
    }
    return n;
}

/// `glass` (BackdropBlur + the shadows over the uniform backdrop) against the formula: R' is the
/// same element with the shadows alone on a transparent target (premultiplied), B the uniform colour
/// as the target holds it, S.a = 0.5 inside the panel and 0 outside; out = R' + B * S.a * (1 - R'.a),
/// then src-over onto the backdrop. Returns the largest channel error.
float formulaError(const Image& glass, const Image& shadowsOnly, const Image& plainBackdrop) {
    float worst = 0.0f;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const float sa = insidePanel(x, y) ? 0.5f : 0.0f;
            const float ra = shadowsOnly.at(x, y, 3);
            const float outA = ra + 1.0f * sa * (1.0f - ra);
            for (int c = 0; c < 4; ++c) {
                const float b = plainBackdrop.at(x, y, c);
                const float out = shadowsOnly.at(x, y, c) + b * sa * (1.0f - ra);
                const float expected = out + b * (1.0f - outA);
                worst = std::fmax(worst, std::fabs(glass.at(x, y, c) - expected));
            }
        }
    }
    return worst;
}

Image backdropOnly() {
    CommandList cl;
    cl.viewport = kAll;
    cl.devicePixelRatio = 1;
    fillRect(cl, kAll, kUniform);
    Image img;
    img.px.assign(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{img.px.data(), kW, kH, kW * 4};
    target.premultiplied = true;  // R' read back as the effect layer holds it
    renderer.setTarget(target);
    renderer.clear(Color{0, 0, 0, 0});
    renderer.render(cl, nullptr, nullptr);
    return img;
}

constexpr float kTol = 2.5f / 255.0f;

}  // namespace

TEST_CASE(
    "SW backdrop shadows: an outer shadow under a glass panel is the shadow without BackdropBlur (T-729)") {
    // Two hard-edged halves: a blurred backdrop leaking into the shadow would show their mix.
    const Image glass = render({backdrop(), outer()}, 1);
    const Image plain = render({outer()}, 1);
    CHECK(outsideDiffer(glass, plain) == 0u);
    // The shadow just below the panel: as strong as without BackdropBlur (it was stronger before,
    // computed from the panel's alpha 0.75 over the opaque backdrop instead of 0.5).
    CHECK(glass.at(32, 52, 0) == plain.at(32, 52, 0));
    // Inside the panel where the shadow does not reach (its offset 20, blur 4: from y = 30), the
    // blurred backdrop is what a plain BackdropBlur gives.
    const Image high = render({backdrop(), outer(20, 4)}, 1);
    const Image blurOnly = render({backdrop()}, 1);
    float worst = 0.0f;
    for (int y = 16; y < 28; ++y) {
        for (int x = 16; x < 48; ++x) {
            for (int c = 0; c < 4; ++c) {
                worst = std::fmax(worst, std::fabs(high.at(x, y, c) - blurOnly.at(x, y, c)));
            }
        }
    }
    CHECK(worst <= 1.0f / 255.0f);
    CHECK(formulaError(render({backdrop(), outer()}, 0), render({outer()}, 2), backdropOnly()) <= kTol);
}

TEST_CASE(
    "SW backdrop shadows: inset shadow, glow and inner glow follow out = R' + B * S.a * (1 - R'.a) (T-729)") {
    const Image bd = backdropOnly();
    SUBCASE("inset") {
        CHECK(formulaError(render({backdrop(), inset()}, 0), render({inset()}, 2), bd) <= kTol);
    }
    SUBCASE("glow") {
        CHECK(formulaError(render({backdrop(), glow(false)}, 0), render({glow(false)}, 2), bd) <= kTol);
        CHECK(outsideDiffer(render({backdrop(), glow(false)}, 1), render({glow(false)}, 1)) == 0u);
    }
    SUBCASE("inner glow") {
        CHECK(formulaError(render({backdrop(), glow(true)}, 0), render({glow(true)}, 2), bd) <= kTol);
    }
    SUBCASE("outer and inset together") {
        CHECK(formulaError(render({backdrop(), outer(), inset()}, 0), render({outer(), inset()}, 2), bd) <=
              kTol);
        CHECK(outsideDiffer(render({backdrop(), outer(), inset()}, 1), render({outer(), inset()}, 1)) == 0u);
    }
}

// Rule 3 / rule 4 of §10.18 B: the hashes are those of the build before T-729 (same SW renderer
// otherwise), so these stacks are byte-identical to before.
TEST_CASE("SW backdrop shadows: other stacks are byte-identical to before (T-729)") {
    struct Case {
        const char* name;
        Image img;
        uint64_t before;
    };
    const Case cases[] = {
        {"plain backdrop blur", render({backdrop(0.3f, 0.05f)}, 1), 0x6b71c016f0604a03ull},
        {"backdrop blur then color adjust",
         render({backdrop(0.3f, 0.05f), fx(EffectKind::ColorAdjust, {0, 0, -0.5f, 0, 1})}, 1),
         0x0518dbe101179e83ull},
        {"backdrop blur, shadow, color adjust",
         render({backdrop(0.3f, 0.05f), outer(), fx(EffectKind::ColorAdjust, {0.1f, 0, 0, 0, 1})}, 1),
         0x31025e221394f541ull},
        {"backdrop blur not first", render({outer(), backdrop(0.3f, 0.05f)}, 1), 0x41994c6a97e84cacull},
        {"backdrop blur, shadow, outline",
         render({backdrop(0.3f, 0.05f), outer(), fx(EffectKind::Outline, {2, 1, 0})}, 1),
         0xa0151cb2e6229ecdull},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        std::printf("sw backdrop shadows %s: hash 0x%016llxull\n", c.name,
                    static_cast<unsigned long long>(c.img.hash()));
        CHECK(c.img.hash() == c.before);
    }
}
