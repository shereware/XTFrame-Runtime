// T-668 (Docs/design/10-backends.md §10.12 item 2, Docs/tasks/T-668.md item 6): DrawBoxShadow on
// the wgpu device -- no offscreen target, no pass break -- against xt::sw at 15 §15.3's GPU
// tolerance, and the pass count of a frame with ten shadows before (PushEffect) and after.

#include <cstdint>
#include <vector>

#include "doctest.h"
#include "test_wgpu_draw_util.h"
#include "xt/command_list.h"

using namespace xt;
using namespace xt::wgpu::test;

namespace {

void addBoxShadow(CommandList& cl, Rect rect, float radius, Vec2 offset, float blur, float spread,
                  Color color, Mat2x3 xf = Mat2x3{}) {
    BoxShadowCmd bs{};
    bs.xf = xf;
    bs.rect = rect;
    for (float& r : bs.radii) {
        r = radius;
    }
    bs.offset = offset;
    bs.blur = blur;
    bs.spread = spread;
    bs.color = color;
    cl.commands.push_back(Command{CmdKind::DrawBoxShadow, {}, static_cast<uint32_t>(cl.boxShadows.size())});
    cl.boxShadows.push_back(bs);
}

/// Ten 40 x 28 cards on a 5 x 2 grid, each with `shadow(2px 4px 8px #00000099)` drawn either as a
/// PushEffect around its fill (`effects`) or as a DrawBoxShadow before it.
CommandList tenCards(int w, int h, bool effects) {
    CommandList cl = makeList(w, h);
    addFillRect(cl, 0, 0, static_cast<float>(w), static_cast<float>(h), Color{0.95f, 0.95f, 0.95f, 1});
    for (int i = 0; i < 10; ++i) {
        const float x = 12.0f + static_cast<float>(i % 5) * 60.0f;
        const float y = 16.0f + static_cast<float>(i / 5) * 60.0f;
        const Color card{0.2f + 0.06f * static_cast<float>(i), 0.4f, 0.8f, 1};
        if (effects) {
            EffectInstance e = effect(EffectKind::DropShadow, {2, 4, 8, 0, 1, 0}, Color{0, 0, 0, 0.6f});
            e.enabled = 1;
            addPushEffect(cl, {e}, Rect{x - 14, y - 14, 40 + 28, 28 + 28}, Rect{x, y, 40, 28});
            addFillRect(cl, x, y, 40, 28, card);
            addPopEffect(cl);
        } else {
            addBoxShadow(cl, Rect{x, y, 40, 28}, 0, Vec2{2, 4}, 8, 0, Color{0, 0, 0, 0.6f});
            addFillRect(cl, x, y, 40, 28, card);
        }
    }
    return cl;
}

}  // namespace

TEST_CASE("wgpu box shadow: ten DrawBoxShadow cards are one pass and match SW (T-668)") {
    constexpr int kW = 320, kH = 140;
    Session before(kW, kH);
    before.render(tenCards(kW, kH, true));
    CHECK_MESSAGE(before.error.empty(), before.error);
    const uint32_t passesBefore = before.renderer.stats().passes;
    MESSAGE("ten drop shadows through PushEffect: " << passesBefore << " passes");
    CHECK(passesBefore >= 20u);

    Session after(kW, kH);
    const CommandList cl = tenCards(kW, kH, false);
    const std::vector<uint8_t> gpu = after.render(cl);
    CHECK_MESSAGE(after.error.empty(), after.error);
    CHECK(after.renderer.stats().passes == 1u);
    CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "ten box shadows"));
}

TEST_CASE("wgpu box shadow: rounded, spread, rotated, sharp and 4x shadows match SW (T-668)") {
    constexpr int kW = 256, kH = 160;
    CommandList cl = makeList(kW, kH);
    addFillRect(cl, 0, 0, kW, kH, Color{1, 1, 1, 1});
    addBoxShadow(cl, Rect{20, 20, 60, 40}, 12, Vec2{0, 6}, 14, 0, Color{0, 0, 0, 0.6f});
    addBoxShadow(cl, Rect{110, 24, 50, 34}, 6, Vec2{3, 3}, 10, 5, Color{0.6f, 0.1f, 0.1f, 0.8f});
    addBoxShadow(cl, Rect{-30, -20, 60, 40}, 10, Vec2{0, 8}, 16, 0, Color{0, 0, 0, 0.7f},
                 mul(translation(Vec2{210, 50}), rotationDeg(7.0f)));
    addBoxShadow(cl, Rect{20.3f, 100.6f, 60, 40}, 9, Vec2{3, 3}, 2.8f, 0, Color{0, 0, 0, 0.75f});  // sharp
    addBoxShadow(cl, Rect{110.3f, 100.6f, 60, 40}, 9, Vec2{3, 3}, 2.9f, 0, Color{0, 0, 0, 0.75f});
    addBoxShadow(cl, Rect{200, 110, 10, 8}, 3, Vec2{0, 0}, 40, 0, Color{0, 0, 0, 1});
    for (uint32_t samples : {1u, 4u}) {
        CAPTURE(samples);
        Session s(kW, kH, samples);
        const std::vector<uint8_t> gpu = s.render(cl);
        CHECK_MESSAGE(s.error.empty(), s.error);
        CHECK(s.renderer.stats().passes == 1u);
        CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "box shadow cases"));
    }
}

// T-680 (Docs/design/10-backends.md "10.12 定案 2 補充 2" B.1): an outlined box's shadow as three
// signed terms (O_out - O_in + R) -- blurred, with a spread that keeps the gap, one that closes it,
// below the no-blur threshold (the ring and the box filled) and rotated -- in one pass, against xt::sw.
TEST_CASE("wgpu box shadow: three-term (outlined) shadows are one pass and match SW (T-680)") {
    constexpr int kW = 300, kH = 180;
    CommandList cl = makeList(kW, kH);
    addFillRect(cl, 0, 0, kW, kH, Color{1, 1, 1, 1});
    auto outlined = [&](float x, float y, float offset, float width, float blur, float spread, Mat2x3 xf) {
        const float r = 8;
        BoxShadowCmd bs{};
        bs.xf = xf;
        const float g = offset + width;
        bs.rect = Rect{x - g, y - g, 60 + 2 * g, 40 + 2 * g};
        for (float& radius : bs.radii) {
            radius = r + g;
        }
        bs.extraCount = 2;
        bs.extra[0] = BoxShadowTerm{Rect{x - offset, y - offset, 60 + 2 * offset, 40 + 2 * offset},
                                    {r + offset, r + offset, r + offset, r + offset},
                                    -1.0f};
        bs.extra[1] = BoxShadowTerm{Rect{x, y, 60, 40}, {r, r, r, r}, 1.0f};
        bs.offset = Vec2{2, 3};
        bs.blur = blur;
        bs.spread = spread;
        bs.color = Color{0, 0, 0, 0.8f};
        cl.commands.push_back(
            Command{CmdKind::DrawBoxShadow, {}, static_cast<uint32_t>(cl.boxShadows.size())});
        cl.boxShadows.push_back(bs);
    };
    outlined(24, 24, 5, 3, 8, 0, Mat2x3{});
    outlined(124, 24, 7, 3, 10, 2, Mat2x3{});
    outlined(214, 24, 2, 3, 12, 4, Mat2x3{});    // the spread closes the gap: one term
    outlined(24, 110, 5, 3, 2.0f, 0, Mat2x3{});  // sharp: the ring and the box filled
    outlined(-30, -20, 5, 3, 8, 0, mul(translation(Vec2{190, 130}), rotationDeg(7.0f)));
    for (uint32_t samples : {1u, 4u}) {
        CAPTURE(samples);
        Session s(kW, kH, samples);
        const std::vector<uint8_t> gpu = s.render(cl);
        CHECK_MESSAGE(s.error.empty(), s.error);
        CHECK(s.renderer.stats().passes == 1u);
        CHECK(matches(gpu, renderSw(cl, kW, kH), kW, kH, "three-term box shadows"));
    }
}
