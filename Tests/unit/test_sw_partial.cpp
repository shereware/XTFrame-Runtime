#include <cstdint>
#include <span>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/damage.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// SwRenderer partial redraw (Docs/design/23-host-integration.md §23.3, Docs/tasks/T-99.md tests
/// 2 and 3): command skipping driven by the DamageList, and clearRects()'s "inside the rectangles
/// only" contract. Test 1 (the byte-exact equivalence over 60 animated frames) lives in
/// Tests/golden/golden_partial.cpp, where the `.xt` fixtures are.

using namespace xt;
using namespace xt::sw;

namespace {

/// Appends a closed rectangle path (Move, Line x3, Close) to `cl`'s pools; returns its PathRef.
PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    const float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void addFillRect(CommandList& cl, float x, float y, float w, float h, Color color) {
    PathRef path = addRectPath(cl, x, y, w, h);
    FillCmd f{};
    f.path = path;
    f.paint.kind = PaintKind::Solid;
    f.paint.color = color;
    f.rule = FillRule::NonZero;
    f.antialias = 1;
    cl.fills.push_back(f);
    Command cmd{};
    cmd.kind = CmdKind::FillPath;
    cmd.index = static_cast<uint32_t>(cl.fills.size() - 1);
    cl.commands.push_back(cmd);
}

struct Rgba {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
};

Rgba pixelAt(const std::vector<uint8_t>& buf, int width, int x, int y) {
    const uint8_t* p =
        buf.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
    return Rgba{p[0], p[1], p[2], p[3]};
}

constexpr int kWidth = 200;
constexpr int kHeight = 100;

}  // namespace

TEST_CASE("SwRenderer skips draw commands outside the damage rectangles") {
    // Two far-apart boxes; only the left one is dirty this frame.
    CommandList list;
    list.viewport = Rect{0, 0, kWidth, kHeight};
    list.devicePixelRatio = 1.0f;
    addFillRect(list, 10, 10, 30, 30, Color{1, 0, 0, 1});
    addFillRect(list, 150, 10, 30, 30, Color{0, 0, 1, 1});

    std::vector<uint8_t> buf(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), kWidth, kHeight, kWidth * 4});

    SUBCASE("a full render executes both") {
        renderer.clear(Color{1, 1, 1, 1});
        renderer.render(list, nullptr, nullptr);
        CHECK(renderer.lastExecutedDrawCommands() == 2);
        CHECK(renderer.lastSkippedDrawCommands() == 0);
        CHECK(pixelAt(buf, kWidth, 20, 20).r == 255);
        CHECK(pixelAt(buf, kWidth, 160, 20).b == 255);
    }

    SUBCASE("damage covering only the left box skips the right one") {
        DamageList damage;
        damage.full = false;
        damage.viewport = list.viewport;
        damage.rects.push_back(Rect{5, 5, 40, 40});

        renderer.clearRects(Color{1, 1, 1, 1}, damage.rects);
        renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);
        CHECK(renderer.lastExecutedDrawCommands() == 1);
        CHECK(renderer.lastSkippedDrawCommands() == 1);
        // The left box was drawn; the right box never touched the target (still the zeroed
        // buffer, not the white clear color -- clearRects only cleared the damage rectangle).
        CHECK(pixelAt(buf, kWidth, 20, 20).r == 255);
        CHECK(pixelAt(buf, kWidth, 20, 20).a == 255);
        CHECK(pixelAt(buf, kWidth, 160, 20).a == 0);
    }

    SUBCASE("damage with no rectangles at all draws nothing") {
        // "Nothing changed on screen" (an identical command list); the target must survive intact.
        DamageList damage;
        damage.full = false;
        damage.viewport = list.viewport;
        renderer.clear(Color{1, 1, 1, 1});
        renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);
        CHECK(renderer.lastExecutedDrawCommands() == 0);
        CHECK(renderer.lastSkippedDrawCommands() == 2);
        CHECK(pixelAt(buf, kWidth, 20, 20).g == 255);
        CHECK(pixelAt(buf, kWidth, 160, 20).g == 255);
    }

    SUBCASE("a full DamageList renders everything") {
        DamageList damage;  // Default-constructed == full.
        renderer.clear(Color{1, 1, 1, 1});
        renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);
        CHECK(renderer.lastExecutedDrawCommands() == 2);
        CHECK(renderer.lastSkippedDrawCommands() == 0);
        CHECK(pixelAt(buf, kWidth, 160, 20).b == 255);
    }
}

TEST_CASE("SwRenderer partial redraw writes only inside the damage rectangles") {
    CommandList list;
    list.viewport = Rect{0, 0, kWidth, kHeight};
    list.devicePixelRatio = 1.0f;
    // One wide box spanning both the damaged and the undamaged half of the target.
    addFillRect(list, 0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight), Color{1, 0, 0, 1});

    std::vector<uint8_t> buf(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 7);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), kWidth, kHeight, kWidth * 4});

    DamageList damage;
    damage.full = false;
    damage.viewport = list.viewport;
    damage.rects.push_back(Rect{0, 0, 50, static_cast<float>(kHeight)});
    renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);

    CHECK(renderer.lastExecutedDrawCommands() == 1);
    CHECK(pixelAt(buf, kWidth, 25, 50).r == 255);
    CHECK(pixelAt(buf, kWidth, 49, 50).r == 255);
    // Outside the rectangle the sentinel value survives untouched.
    const Rgba outside = pixelAt(buf, kWidth, 50, 50);
    CHECK(outside.r == 7);
    CHECK(outside.g == 7);
    CHECK(outside.b == 7);
    CHECK(outside.a == 7);
}

TEST_CASE("SwRenderer::clearRects only touches pixels inside the rectangles") {
    std::vector<uint8_t> buf(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0x5A);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), kWidth, kHeight, kWidth * 4});

    const Rect rects[] = {Rect{10, 10, 20, 20}, Rect{100.5f, 40.25f, 10, 10}};
    renderer.clearRects(Color{0, 0, 0, 0}, std::span<const Rect>(rects, 2));

    // Inside the first rectangle.
    CHECK(pixelAt(buf, kWidth, 10, 10).a == 0);
    CHECK(pixelAt(buf, kWidth, 29, 29).a == 0);
    // Inside the second: the fractional rectangle is snapped outwards, so x == 100 and y == 40
    // are covered and x == 111 / y == 51 are the first pixels past it.
    CHECK(pixelAt(buf, kWidth, 100, 40).a == 0);
    CHECK(pixelAt(buf, kWidth, 110, 50).a == 0);
    CHECK(pixelAt(buf, kWidth, 111, 50).a == 0x5A);
    CHECK(pixelAt(buf, kWidth, 110, 51).a == 0x5A);
    // Outside both: the sentinel survives.
    CHECK(pixelAt(buf, kWidth, 9, 10).r == 0x5A);
    CHECK(pixelAt(buf, kWidth, 30, 30).r == 0x5A);
    CHECK(pixelAt(buf, kWidth, 199, 99).r == 0x5A);

    SUBCASE("an empty rectangle list changes nothing") {
        renderer.clearRects(Color{1, 1, 1, 1}, std::span<const Rect>{});
        CHECK(pixelAt(buf, kWidth, 15, 15).a == 0);
        CHECK(pixelAt(buf, kWidth, 199, 99).r == 0x5A);
    }
}

TEST_CASE("T-239 SwTarget origin: a window onto the canvas matches the same pixels of a full render") {
    CommandList list;
    list.viewport = Rect{0, 0, kWidth, kHeight};
    list.devicePixelRatio = 1.0f;
    addFillRect(list, 10.5f, 10.25f, 60, 50, Color{1, 0, 0, 0.5f});
    addFillRect(list, 120, 30, 60, 40, Color{0, 0, 1, 1});

    std::vector<uint8_t> full(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{full.data(), kWidth, kHeight, kWidth * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);

    // A 100 x 60 window at (50, 20).
    constexpr int kW = 100;
    constexpr int kH = 60;
    std::vector<uint8_t> window(static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4, 0);
    SwTarget target{window.data(), kW, kH, kW * 4};
    target.originX = 50;
    target.originY = 20;
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const Rgba a = pixelAt(window, kW, x, y);
            const Rgba b = pixelAt(full, kWidth, x + 50, y + 20);
            REQUIRE(a.r == b.r);
            REQUIRE(a.g == b.g);
            REQUIRE(a.b == b.b);
            REQUIRE(a.a == b.a);
        }
    }

    // Damage and clearRects() rectangles are canvas coordinates too: a rectangle over the blue box
    // (canvas x 120..180) lands at window x 70..100 and leaves the rest alone.
    std::vector<uint8_t> before = window;
    DamageList damage;
    damage.full = false;
    damage.viewport = list.viewport;
    damage.rects.push_back(Rect{118, 28, 70, 50});
    renderer.clearRects(Color{1, 1, 1, 1}, damage.rects);
    CHECK(pixelAt(window, kW, 75, 30).r == 255);  // Cleared: the blue box is gone for now.
    CHECK(pixelAt(window, kW, 45, 30).r == 255);  // Outside the rectangle: untouched red/white.
    renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);
    CHECK(renderer.lastExecutedDrawCommands() == 1);
    CHECK(renderer.lastSkippedDrawCommands() == 1);
    CHECK(window == before);
}

TEST_CASE("T-239 partial redraw skips a whole layer section outside the damage") {
    // Left: a plain box. Right: a half-opacity layer holding two boxes.
    CommandList list;
    list.viewport = Rect{0, 0, kWidth, kHeight};
    list.devicePixelRatio = 1.0f;
    addFillRect(list, 10, 10, 30, 30, Color{1, 0, 0, 1});
    list.layers.push_back(LayerCmd{0.5f, BlendMode::Normal, Rect{140, 5, 50, 50}});
    Command push{};
    push.kind = CmdKind::PushLayer;
    push.index = 0;
    list.commands.push_back(push);
    addFillRect(list, 150, 10, 30, 30, Color{0, 0, 1, 1});
    addFillRect(list, 160, 20, 20, 20, Color{0, 1, 0, 1});
    Command pop{};
    pop.kind = CmdKind::PopLayer;
    list.commands.push_back(pop);
    addFillRect(list, 60, 60, 20, 20, Color{0, 0, 0, 1});

    std::vector<uint8_t> full(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{full.data(), kWidth, kHeight, kWidth * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    CHECK(renderer.lastExecutedDrawCommands() == 4);

    // Damage over the left box only: the layer section (two draws) is skipped as a whole, and
    // the damaged pixels still match the full render.
    std::vector<uint8_t> before = full;
    DamageList damage;
    damage.full = false;
    damage.viewport = list.viewport;
    damage.rects.push_back(Rect{5, 5, 40, 40});
    renderer.clearRects(Color{1, 1, 1, 1}, damage.rects);
    renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);
    CHECK(renderer.lastExecutedDrawCommands() == 1);
    CHECK(renderer.lastSkippedDrawCommands() == 3);
    CHECK(full == before);

    // Damage touching the layer's buffer runs the section.
    damage.rects.clear();
    damage.rects.push_back(Rect{185, 50, 10, 10});
    renderer.clearRects(Color{1, 1, 1, 1}, damage.rects);
    renderer.render(list, nullptr, nullptr, nullptr, nullptr, &damage);
    CHECK(renderer.lastExecutedDrawCommands() == 2);
    CHECK(full == before);
}

namespace {

/// Every glyph is a 10 x 10 half-coverage square sitting on the baseline: re-blending one over
/// itself changes its pixels, which is what T-263a E2's test looks for.
class SquareGlyphShaper : public TextShaper {
public:
    FontId match(const FontRequest&) override { return 1; }
    FontMetrics metrics(FontId) override { return FontMetrics{}; }
    void shape(std::string_view, const ShapeParams&, ShapeResult& out) override { out = ShapeResult{}; }
    bool glyphOutline(FontId, uint32_t, std::vector<PathVerb>& verbs, std::vector<float>& points) override {
        verbs.clear();
        points.clear();
        return false;
    }
    bool glyphBitmap(FontId, uint32_t, float, float, GlyphBitmap& out) override {
        out.width = 10;
        out.height = 10;
        out.bearingX = 0;
        out.bearingY = 10;
        out.alpha.assign(100, 128);
        return true;
    }
};

}  // namespace

// T-263a E2: the glyph bitmap and glyph effect paths honour the damage mask like every other
// draw, so a run that straddles the damage edge leaves its glyphs outside the damage untouched.
TEST_CASE("SwRenderer: T-263a partial redraw of a glyph run touches only the damaged pixels") {
    CommandList list;
    list.viewport = Rect{0, 0, kWidth, kHeight};
    list.devicePixelRatio = 1.0f;
    GlyphsCmd run{};
    run.font = 1;
    run.size = 12;
    run.color = Color{0, 0, 0, 1};
    run.rendering = TextRendering::Auto;
    run.glyphOffset = 0;
    run.glyphCount = 2;
    SUBCASE("bitmap path") {}
    SUBCASE("glyph effect path") {
        run.effect = GlyphEffect::Dilate;
        run.effectRadius = 1;
    }
    list.glyphs = {65, 66};
    list.glyphPos = {Vec2{20, 50}, Vec2{150, 50}};
    list.glyphRuns.push_back(run);
    list.commands.push_back(Command{CmdKind::DrawGlyphs, {}, 0});

    SquareGlyphShaper shaper;
    std::vector<uint8_t> buf(static_cast<size_t>(kWidth) * static_cast<size_t>(kHeight) * 4, 0);
    SwRenderer renderer;
    renderer.setTarget(SwTarget{buf.data(), kWidth, kHeight, kWidth * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, &shaper, nullptr);
    const std::vector<uint8_t> before = buf;
    REQUIRE(pixelAt(buf, kWidth, 155, 45).r < 255);  // the second glyph was drawn

    // The damage covers the first glyph only; the run's bounds still meet it, so it executes.
    DamageList damage;
    damage.full = false;
    damage.viewport = list.viewport;
    damage.rects.push_back(Rect{10, 30, 30, 30});
    for (int pass = 0; pass < 2; ++pass) {
        renderer.clearRects(Color{1, 1, 1, 1}, damage.rects);
        renderer.render(list, &shaper, nullptr, nullptr, nullptr, &damage);
        CHECK(renderer.lastExecutedDrawCommands() == 1);
        CHECK(buf == before);
    }
}
