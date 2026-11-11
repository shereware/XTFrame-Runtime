#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/geom.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

#ifndef XT_GPU_NO_TEXT
#include "xt/text/ft_text_shaper.h"

/// T-681 (Docs/tasks/T-681.md item 6; Docs/design/10-backends.md §10.12, the ruling of the T-680
/// record): drop shadows of a node whose background has a uniform alpha of at least 0.9 ("補充 2"
/// B.3), against ground truth. For each of five cells -- ui_box_shadow_relaxed's a90, a92 and
/// a92text, text against the bottom-right edge with a large offset and a small blur, and an opaque
/// child box against the edge of a 0.9 background -- three images are compared with the truth:
///   - analytic: xt::sw with Context::setBoxShadowCommands on (the node's shadow a DrawBoxShadow,
///     B.3's `a x S(R)`),
///   - offscreen: the same with it off (the PushEffect path),
///   - truth: the node's real content alpha (the translucent background with its children
///     composited over it) convolved with the shadow's Gaussian (sigma = blur / 2), in the test by
///     brute force on a 1/64 px grid (64 x 64 samples per pixel), offset and coloured as the effect
///     defines; under the content (xt::sw's render of the node without its effect) over white.
/// B.3 stays only if, in every cell, the analytic image's largest channel error against the truth is
/// no larger than the offscreen image's and it has no more pixels with an error over 8.

using namespace xt;

namespace {

std::string fontsDir() { return std::string(XT_TEST_FONTS_DIR); }

/// One cell: the CSS of `#n` (the node), its inner markup and the extra rules for that markup, the
/// viewport, and its shadow -- as CSS, and as the numbers the truth uses (they must agree).
struct TruthCell {
    const char* name;
    std::string nodeCss;
    std::string inner;
    std::string innerLayout;
    std::string innerStyle;
    int w, h;
    std::string effect;
    Vec2 offset;
    float blur;
    Color color;  ///< straight; black in every cell
};

std::string cellSource(const TruthCell& c, bool withEffect, const std::string& nodeSize) {
    std::string css = c.nodeCss;
    if (withEffect) {
        css += "effect: shadow(" + c.effect + ");";
    }
    return "<xt version=\"2\"><body><div id=\"n\">" + c.inner +
           "</div></body><layout>body { width: " + std::to_string(c.w) +
           "px; height: " + std::to_string(c.h) +
           "px; margin: 0; } #n { position: absolute; left: 30px; top: 30px; " + nodeSize + " } " +
           c.innerLayout +
           "</layout><style>"
           " body { font-family: \"Noto Sans\"; } #n { " +
           css + " } " + c.innerStyle + "</style></xt>";
}

/// Renders `xt` with xt::sw into `px` (straight RGBA8, w x h) over `clear`, shaping with `shaper`
/// (fresh: Noto Sans is registered on it here); `list` gets the frame's commands. False
/// when it does not compile or load.
bool renderCell(const std::string& xt, int w, int h, bool boxShadows, Color clear, text::FtTextShaper& shaper,
                std::vector<uint8_t>& px, CommandList& list) {
    if (!shaper.addFontFile(fontsDir() + "/NotoSans-Regular.subset.ttf")) {
        return false;
    }
    Context ctx(ContextConfig{&shaper, nullptr});
    ctx.setBoxShadowCommands(boxShadows);
    compile::SourceFile files[1] = {compile::SourceFile{"cell.xt", xt}};
    compile::CompileOptions options;
    options.rootDir = fontsDir();
    compile::CompileResult compiled = compile::compileDocumentFromStrings(files, "cell.xt", options);
    for (const compile::Diagnostic& d : compiled.diagnostics) {
        std::printf("    compile: %s\n", d.message.c_str());
    }
    if (!compiled.ok()) {
        return false;
    }
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, false);
    if (!handle.ok()) {
        return false;
    }
    ctx.setViewport(Rect{0, 0, static_cast<float>(w), static_cast<float>(h)}, 1.0f);
    ctx.update(0.0f);
    ctx.render(list);
    px.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{px.data(), w, h, w * 4});
    renderer.clear(clear);
    renderer.render(list, &shaper, nullptr);
    return true;
}

size_t countKind(const CommandList& list, CmdKind kind) {
    size_t n = 0;
    for (const Command& c : list.commands) {
        n += c.kind == kind ? 1 : 0;
    }
    return n;
}

/// One filled region of the content: flattened closed contours (device px) and the alpha it paints.
struct TruthShape {
    std::vector<Vec2> points;
    std::vector<uint32_t> starts;
    float alpha;
};

/// The content's regions in paint order: every FillPath (solid paint) and every glyph run's outlines
/// (from `shaper`, the one `list` was painted with) of `list`, flattened finely. False when the list
/// holds anything else that draws.
bool contentShapes(const CommandList& list, text::FtTextShaper& shaper, std::vector<TruthShape>& out) {
    std::vector<uint8_t> closed;
    std::vector<PathVerb> verbs;
    std::vector<float> points;
    constexpr float kTol = 0.005f;
    for (const Command& c : list.commands) {
        if (c.kind == CmdKind::FillPath) {
            const FillCmd& f = list.fills[c.index];
            if (f.paint.kind != PaintKind::Solid) {
                return false;
            }
            TruthShape s;
            s.alpha = f.paint.color.a;
            flattenPath(std::span<const PathVerb>(list.verbs.data() + f.path.verbOffset, f.path.verbCount),
                        std::span<const float>(list.points.data() + f.path.pointOffset, f.path.pointCount),
                        f.xf, kTol, s.points, s.starts, closed);
            out.push_back(std::move(s));
        } else if (c.kind == CmdKind::DrawGlyphs) {
            const GlyphsCmd& g = list.glyphRuns[c.index];
            TruthShape s;
            s.alpha = g.color.a;
            std::vector<Vec2> pts;
            std::vector<uint32_t> starts;
            for (uint32_t i = 0; i < g.glyphCount; ++i) {
                verbs.clear();
                points.clear();
                if (!shaper.glyphOutline(g.font, list.glyphs[g.glyphOffset + i], verbs, points)) {
                    continue;
                }
                const Mat2x3 local = mul(
                    g.xf, mul(translation(list.glyphPos[g.glyphOffset + i]), scaling(Vec2{g.size, g.size})));
                pts.clear();
                starts.clear();
                flattenPath(verbs, points, local, kTol, pts, starts, closed);
                for (uint32_t& st : starts) {
                    st += static_cast<uint32_t>(s.points.size());
                }
                s.points.insert(s.points.end(), pts.begin(), pts.end());
                s.starts.insert(s.starts.end(), starts.begin(), starts.end());
            }
            out.push_back(std::move(s));
        } else if (c.kind != CmdKind::PushClipRect && c.kind != CmdKind::PopClip) {
            return false;
        }
    }
    return true;
}

/// The nonzero-winding spans of `s` on the horizontal line y, sorted.
void spansAt(const TruthShape& s, double y, std::vector<std::pair<double, double>>& out) {
    out.clear();
    static thread_local std::vector<std::pair<double, int>> xs;
    xs.clear();
    for (size_t k = 0; k < s.starts.size(); ++k) {
        const size_t b = s.starts[k];
        const size_t e = k + 1 < s.starts.size() ? s.starts[k + 1] : s.points.size();
        for (size_t i = b; i < e; ++i) {
            const Vec2 p = s.points[i];
            const Vec2 q = s.points[i + 1 < e ? i + 1 : b];
            const double y0 = p.y, y1 = q.y;
            if ((y0 <= y && y < y1) || (y1 <= y && y < y0)) {
                const double t = (y - y0) / (y1 - y0);
                xs.emplace_back(p.x + t * (q.x - p.x), y1 > y0 ? 1 : -1);
            }
        }
    }
    std::sort(xs.begin(), xs.end());
    int wind = 0;
    double start = 0.0;
    for (const auto& [x, d] : xs) {
        const int before = wind;
        wind += d;
        if (before == 0 && wind != 0) {
            start = x;
        } else if (before != 0 && wind == 0) {
            out.emplace_back(start, x);
        }
    }
}

bool inside(const std::vector<std::pair<double, double>>& spans, double x) {
    for (const auto& [a, b] : spans) {
        if (x >= a && x < b) {
            return true;
        }
    }
    return false;
}

/// The truth: the shadow's alpha factor (the content alpha convolved with the unit Gaussian of
/// sigma, shifted by `offset`) at every pixel centre of the w x h viewport, by brute force on the
/// 1/64 px grid. Rows of samples are piecewise constant in x (the shapes' spans composited in
/// order); each sample still counts once, through prefix sums of the x weights per pixel column.
std::vector<double> truthShadow(const std::vector<TruthShape>& shapes, int w, int h, Vec2 offset,
                                double sigma) {
    constexpr double kSub = 64.0;
    const double hstep = 1.0 / kSub;
    const int nx = static_cast<int>(w * kSub), ny = static_cast<int>(h * kSub);
    const double inv2s2 = 1.0 / (2.0 * sigma * sigma);
    const double reach = 6.0 * sigma;
    // prefix[px][i]: sum of the x weights of samples 0..i-1 seen from pixel column px.
    std::vector<std::vector<double>> prefix(static_cast<size_t>(w));
    for (int px = 0; px < w; ++px) {
        const double cx = px + 0.5 - offset.x;
        std::vector<double>& p = prefix[static_cast<size_t>(px)];
        p.assign(static_cast<size_t>(nx) + 1, 0.0);
        for (int i = 0; i < nx; ++i) {
            const double dx = (i + 0.5) * hstep - cx;
            p[static_cast<size_t>(i) + 1] =
                p[static_cast<size_t>(i)] + (std::fabs(dx) <= reach ? std::exp(-dx * dx * inv2s2) : 0.0);
        }
    }
    std::vector<double> out(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0);
    std::vector<std::vector<std::pair<double, double>>> spans(shapes.size());
    std::vector<double> cuts;
    std::vector<double> row(static_cast<size_t>(w));
    for (int j = 0; j < ny; ++j) {
        const double yc = (j + 0.5) * hstep;
        bool any = false;
        cuts.clear();
        for (size_t k = 0; k < shapes.size(); ++k) {
            spansAt(shapes[k], yc, spans[k]);
            for (const auto& [a, b] : spans[k]) {
                cuts.push_back(a);
                cuts.push_back(b);
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        std::sort(cuts.begin(), cuts.end());
        std::fill(row.begin(), row.end(), 0.0);
        for (size_t c = 0; c + 1 < cuts.size(); ++c) {
            const double xl = cuts[c], xr = cuts[c + 1];
            if (!(xr > xl)) {
                continue;
            }
            const double xm = 0.5 * (xl + xr);
            double alpha = 0.0;
            for (size_t k = 0; k < shapes.size(); ++k) {
                if (inside(spans[k], xm)) {
                    alpha += (1.0 - alpha) * shapes[k].alpha;
                }
            }
            if (alpha <= 0.0) {
                continue;
            }
            // Samples i with (i + 0.5) / 64 in [xl, xr).
            const int lo = std::max(0, static_cast<int>(std::ceil(xl * kSub - 0.5)));
            const int hi = std::min(nx, static_cast<int>(std::ceil(xr * kSub - 0.5)));
            if (lo >= hi) {
                continue;
            }
            for (int px = 0; px < w; ++px) {
                const std::vector<double>& p = prefix[static_cast<size_t>(px)];
                row[static_cast<size_t>(px)] +=
                    alpha * (p[static_cast<size_t>(hi)] - p[static_cast<size_t>(lo)]);
            }
        }
        const int py0 = std::max(0, static_cast<int>(std::floor(yc + offset.y - reach - 0.5)));
        const int py1 = std::min(h - 1, static_cast<int>(std::ceil(yc + offset.y + reach - 0.5)));
        for (int py = py0; py <= py1; ++py) {
            const double dy = yc - (py + 0.5 - offset.y);
            const double gy = std::exp(-dy * dy * inv2s2);
            double* o = out.data() + static_cast<size_t>(py) * static_cast<size_t>(w);
            for (int px = 0; px < w; ++px) {
                o[px] += gy * row[static_cast<size_t>(px)];
            }
        }
    }
    const double norm = hstep * hstep / (2.0 * 3.14159265358979323846 * sigma * sigma);
    for (double& v : out) {
        v *= norm;
    }
    return out;
}

struct ErrorStats {
    double maxErr = 0.0;
    int over8 = 0;
};

/// Largest channel error and pixels with an error over 8 (of 255) of `img` (opaque straight RGBA8)
/// against the float truth image (RGB, 0..255).
ErrorStats compare(const std::vector<uint8_t>& img, const std::vector<double>& truth, int w, int h) {
    ErrorStats e;
    for (int i = 0; i < w * h; ++i) {
        double worst = 0.0;
        for (int ch = 0; ch < 3; ++ch) {
            worst = std::max(worst, std::fabs(img[static_cast<size_t>(i) * 4 + ch] -
                                              truth[static_cast<size_t>(i) * 3 + ch]));
        }
        e.maxErr = std::max(e.maxErr, worst);
        e.over8 += worst > 8.0 ? 1 : 0;
    }
    return e;
}

struct CellResult {
    size_t analyticBoxShadows = 0;
    ErrorStats analytic, offscreen;
};

CellResult runCell(const TruthCell& c, const std::string& nodeSize) {
    CellResult r;
    std::vector<uint8_t> analytic, offscreen, content;
    CommandList la, lo, lc;
    const Color white{1, 1, 1, 1};
    text::FtTextShaper shA, shO, shC;
    REQUIRE(renderCell(cellSource(c, true, nodeSize), c.w, c.h, true, white, shA, analytic, la));
    REQUIRE(renderCell(cellSource(c, true, nodeSize), c.w, c.h, false, white, shO, offscreen, lo));
    REQUIRE(renderCell(cellSource(c, false, nodeSize), c.w, c.h, true, Color{0, 0, 0, 0}, shC, content, lc));
    r.analyticBoxShadows = countKind(la, CmdKind::DrawBoxShadow);
    CHECK(countKind(lo, CmdKind::DrawBoxShadow) == 0);
    std::vector<TruthShape> shapes;
    REQUIRE(contentShapes(lc, shC, shapes));
    const std::vector<double> s = truthShadow(shapes, c.w, c.h, c.offset, 0.5 * c.blur);
    std::vector<double> truth(static_cast<size_t>(c.w) * static_cast<size_t>(c.h) * 3);
    for (int i = 0; i < c.w * c.h; ++i) {
        const double sa = c.color.a * s[static_cast<size_t>(i)];  // shadow alpha
        const double ca = content[static_cast<size_t>(i) * 4 + 3] / 255.0;
        for (int ch = 0; ch < 3; ++ch) {
            const double shadowRgb = (c.color.r * 255.0 * sa) + (1.0 - sa) * 255.0;  // over white
            const double cp = content[static_cast<size_t>(i) * 4 + ch] * ca;         // premultiplied
            truth[static_cast<size_t>(i) * 3 + ch] = cp + (1.0 - ca) * shadowRgb;
        }
    }
    r.analytic = compare(analytic, truth, c.w, c.h);
    r.offscreen = compare(offscreen, truth, c.w, c.h);
    return r;
}

std::vector<TruthCell> truthCells() {
    const std::string card =
        "border-radius: 4px; background-color: #161b22eb; color: #ffffff; font-size: 24px; line-height: 33px;"
        " white-space: nowrap; ";
    return {
        // ui_box_shadow_relaxed's three cells, as the fixture styles them.
        {"a90", "border-radius: 10px; background-color: #8040c0e6; ", "", "", "", 160, 130,
         "3px 5px 10px #000000a0", Vec2{3, 5}, 10.0f, Color{0, 0, 0, 160.0f / 255.0f}},
        {"a92", "border-radius: 10px; background-color: #8040c0eb; ", "", "", "", 160, 130,
         "3px 5px 10px #000000a0", Vec2{3, 5}, 10.0f, Color{0, 0, 0, 160.0f / 255.0f}},
        {"a92text", card, "Hxgjpq", "", "", 220, 110, "6px 8px 6px #000000ff", Vec2{6, 8}, 6.0f,
         Color{0, 0, 0, 1}},
        // Text against the bottom-right edge, a large offset and a small blur, an opaque black shadow.
        {"textEdge",
         "background-color: #161b22e6; color: #ffffff; font-size: 24px; line-height: 33px; white-space: "
         "nowrap;"
         " ",
         "Hxgjpq", "", "", 200, 110, "10px 10px 4px #000000ff", Vec2{10, 10}, 4.0f, Color{0, 0, 0, 1}},
        // An opaque child box in the bottom-right corner of a 0.9 background, the same kind of shadow.
        {"childEdge", "background-color: #8040c0e6; ", "<div id=\"c\"></div>",
         "#c { position: absolute; right: 0px; bottom: 0px; width: 30px; height: 20px; }",
         "#c { background-color: #ffcc00; }", 160, 130, "8px 8px 4px #000000ff", Vec2{8, 8}, 4.0f,
         Color{0, 0, 0, 1}},
    };
}

}  // namespace

TEST_CASE("gpu: T-681 near-opaque box shadows (B.3) against ground truth") {
    std::printf("T-681 B.3 vs truth: cell, converted, analytic max / >8, offscreen max / >8\n");
    for (const TruthCell& c : truthCells()) {
        INFO(std::string(c.name));
        std::string size = std::string(c.name) == "a92text" ? "height: 33px; padding: 0 13px;"
                                                            : "width: 80px; height: 52px;";
        if (std::string(c.name) == "textEdge") {
            // The text against the right and bottom edges: the box ends where the glyphs' ink box
            // (xt::glyphRunInkBounds, T-681) does, rounded up -- the tightest box condition 4 takes.
            std::vector<uint8_t> px;
            CommandList list;
            text::FtTextShaper shaper;
            REQUIRE(renderCell(cellSource(c, false, "height: 33px;"), c.w, c.h, true, Color{0, 0, 0, 0},
                               shaper, px, list));
            float right = 0.0f, bottom = 0.0f;
            for (const Command& cmd : list.commands) {
                if (cmd.kind == CmdKind::DrawGlyphs) {
                    Rect ink;
                    REQUIRE(glyphRunInkBounds(list, list.glyphRuns[cmd.index], &shaper, ink));
                    right = std::max(right, ink.x + ink.w);
                    bottom = std::max(bottom, ink.y + ink.h);
                }
            }
            size = "width: " + std::to_string(static_cast<int>(std::ceil(right - 30.0f))) +
                   "px; height: " + std::to_string(static_cast<int>(std::ceil(bottom - 30.0f))) + "px;";
        }
        const CellResult r = runCell(c, size);
        std::printf("  %-10s %zu  %6.2f / %4d   %6.2f / %4d\n", c.name, r.analyticBoxShadows,
                    r.analytic.maxErr, r.analytic.over8, r.offscreen.maxErr, r.offscreen.over8);
        // B.3 is kept only on these terms (Docs/tasks/T-681.md item 6).
        CHECK(r.analyticBoxShadows == 1);
        CHECK(r.analytic.maxErr <= r.offscreen.maxErr);
        CHECK(r.analytic.over8 <= r.offscreen.over8);
    }
}

#endif  // XT_GPU_NO_TEXT
