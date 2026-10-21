#define SDL_MAIN_HANDLED  // doctest owns main(); SDL must not rename it.

#include <functional>
#include <utility>

#include "doctest.h"
#include "xt/shell/app.h"

/// T-376 (docs/design/23-host-integration.md §23.ab): xt::shell::hitRegionFor(), the borderless
/// window's hit-test decision, without a window: the eight resize bands (corners first, none while
/// maximized or with a zero border), then the `app-region` answer.

using namespace xt;
using namespace xt::shell;

namespace {

constexpr int kW = 800;
constexpr int kH = 600;
constexpr int kB = 8;

HitRegion at(int x, int y, bool maximized = false, int border = kB, AppRegion region = AppRegion::None) {
    return hitRegionFor(x, y, kW, kH, border, maximized, [region](int, int) { return region; });
}

}  // namespace

TEST_CASE("T-376: hitRegionFor answers the eight resize bands and the interior") {
    CHECK(at(0, 0) == HitRegion::ResizeTopLeft);
    CHECK(at(400, 0) == HitRegion::ResizeTop);
    CHECK(at(kW - 1, 0) == HitRegion::ResizeTopRight);
    CHECK(at(kW - 1, 300) == HitRegion::ResizeRight);
    CHECK(at(kW - 1, kH - 1) == HitRegion::ResizeBottomRight);
    CHECK(at(400, kH - 1) == HitRegion::ResizeBottom);
    CHECK(at(0, kH - 1) == HitRegion::ResizeBottomLeft);
    CHECK(at(0, 300) == HitRegion::ResizeLeft);
    CHECK(at(400, 300) == HitRegion::Normal);
    // The band's inner edge: kB - 1 is in, kB is out.
    CHECK(at(kB - 1, 300) == HitRegion::ResizeLeft);
    CHECK(at(kB, 300) == HitRegion::Normal);
    CHECK(at(kW - kB, 300) == HitRegion::ResizeRight);
    CHECK(at(kW - kB - 1, 300) == HitRegion::Normal);
}

TEST_CASE("T-376: hitRegionFor corners win over the edges") {
    CHECK(at(kB - 1, kB - 1) == HitRegion::ResizeTopLeft);
    CHECK(at(kW - kB, kB - 1) == HitRegion::ResizeTopRight);
    CHECK(at(kW - kB, kH - kB) == HitRegion::ResizeBottomRight);
    CHECK(at(kB - 1, kH - kB) == HitRegion::ResizeBottomLeft);
}

TEST_CASE("T-376: hitRegionFor has no resize bands while maximized or with a zero border") {
    const std::pair<int, int> edges[] = {{0, 0},           {400, 0},      {kW - 1, 0}, {kW - 1, 300},
                                         {kW - 1, kH - 1}, {400, kH - 1}, {0, kH - 1}, {0, 300}};
    for (const auto& p : edges) {
        CAPTURE(p.first);
        CAPTURE(p.second);
        CHECK(at(p.first, p.second, /*maximized=*/true) == HitRegion::Normal);
        CHECK(at(p.first, p.second, /*maximized=*/false, /*border=*/0) == HitRegion::Normal);
    }
    CHECK(at(0, 0, /*maximized=*/true, kB, AppRegion::Drag) == HitRegion::Draggable);
}

TEST_CASE("T-376: hitRegionFor maps app-region drag to Draggable, no-drag and none to Normal") {
    CHECK(at(400, 20, false, kB, AppRegion::Drag) == HitRegion::Draggable);
    CHECK(at(400, 20, false, kB, AppRegion::NoDrag) == HitRegion::Normal);
    CHECK(at(400, 20, false, kB, AppRegion::None) == HitRegion::Normal);
    CHECK(hitRegionFor(400, 20, kW, kH, kB, false, {}) == HitRegion::Normal);  // No callback.
    // The callback sees the window coordinates.
    int seenX = -1, seenY = -1;
    hitRegionFor(123, 45, kW, kH, kB, false, [&](int x, int y) {
        seenX = x;
        seenY = y;
        return AppRegion::None;
    });
    CHECK(seenX == 123);
    CHECK(seenY == 45);
}

TEST_CASE("T-376: hitRegionFor resize bands win over a drag region") {
    CHECK(at(400, 0, false, kB, AppRegion::Drag) == HitRegion::ResizeTop);
    CHECK(at(0, 0, false, kB, AppRegion::Drag) == HitRegion::ResizeTopLeft);
    CHECK(at(kW - 1, 20, false, kB, AppRegion::Drag) == HitRegion::ResizeRight);
    CHECK(at(400, kB, false, kB, AppRegion::Drag) == HitRegion::Draggable);
}
