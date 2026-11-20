// T-762 (Docs/design/19-3d-space.md §19.y.2): Tests/layout/flat_3d_flatten.xt painted into a
// CommandList, shared by the SW checks (Tests/unit/test_painter_3d_flatten.cpp) and the GL
// (Tests/gl/test_gl_3d.cpp) and wgpu (Tests/wgpu/test_wgpu_flatten_3d.cpp) cross-checks. Each of the
// four stages holds a grouping flat node with two overlapping 3D children (red translateZ(40px),
// blue translateZ(-40px)) and, outside the group, a green translateZ(10px) layer between them in
// depth. Needs XT_TEST_LAYOUT_DIR.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

namespace xt::test::flatten {

inline constexpr int kW = 400, kH = 300;

/// The stages' top-left corners: #s1 opacity, #s2 shadow, #s3 mask, #s4 multiply.
inline constexpr Vec2 kStage[4] = {Vec2{0, 0}, Vec2{200, 0}, Vec2{0, 150}, Vec2{200, 150}};

// Probe points in stage coordinates (perspective 400px about the stage centre (90, 70): the red box
// projects to [23, 112] x [37, 114], the blue one to [72, 144] x [43, 106], the green one to
// [49, 131] x [80, 101]).
inline constexpr Vec2 kOverlap{95, 55};       ///< red and blue, not green
inline constexpr Vec2 kGreenOverRed{60, 90};  ///< green and red, not blue
inline constexpr Vec2 kGreenOverBlue{125, 90};
inline constexpr Vec2 kBlueOnly{125, 60};
inline constexpr Vec2 kRedOnly{50, 60};
inline constexpr Vec2 kShadowOnly{148, 95};  ///< inside the blue box's 8px 8px shadow, nothing else

inline std::string fixturePath() { return std::string(XT_TEST_LAYOUT_DIR) + "/flat_3d_flatten.xt"; }

/// Compiles and loads the fixture at a kW x kH viewport, update(0). Empty `error` on success.
inline std::unique_ptr<Document> loadFixture(TextShaper* shaper, AssetResolver* assets, std::string& error) {
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocument(fixturePath(), options);
    if (!result.ok()) {
        error = "compile failed";
        for (const compile::Diagnostic& d : result.diagnostics) {
            error += "; " + d.message;
        }
        return nullptr;
    }
    std::unique_ptr<Document> doc;
    if (Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc) !=
        Status::kOk) {
        error = "load failed";
        return nullptr;
    }
    doc->setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)});
    doc->update(0.0f);
    return doc;
}

/// The fixture's CommandList (empty commands and a message in `error` when it does not load).
inline CommandList paintFixture(std::string& error) {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    CommandList cl;
    std::unique_ptr<Document> doc = loadFixture(&shaper, &assets, error);
    if (doc != nullptr) {
        doc->paint(cl);
    }
    return cl;
}

/// RGB of the top-down RGBA8 buffer `px` (kW wide) at stage `stage`'s point `p`.
struct Rgb {
    int r, g, b;
};
inline Rgb at(const std::vector<uint8_t>& px, int stage, Vec2 p) {
    const int x = static_cast<int>(kStage[stage].x + p.x);
    const int y = static_cast<int>(kStage[stage].y + p.y);
    const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4;
    return Rgb{px[i], px[i + 1], px[i + 2]};
}

/// Within `tol` of (r, g, b) per channel.
inline bool near(Rgb c, int r, int g, int b, int tol = 10) {
    return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol;
}

}  // namespace xt::test::flatten
