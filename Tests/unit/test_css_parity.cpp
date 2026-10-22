#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "mini_css.h"
#include "style_decode.h"
#include "xt/charconv.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/property.h"
#include "xt/timeline.h"
#include "xt/value.h"

/// T-413 (Docs/tasks/T-413.md item 1): the CSS value parsers of the runtime (core's mini_css) and
/// of the compiler (compile's `.xts` parser) read the same declaration text into the same values.
/// Every case runs one declaration through both and compares a canonical rendering of the result
/// (prop numbers, value kinds, every float to 9 significant digits). The few places where 05 says
/// the runtime accepts less than the compiler are pinned separately at the end.

using namespace xt;

namespace {

std::string num(float v) {
    char buf[40];
    return std::string(buf, formatFloat(buf, sizeof(buf), static_cast<double>(v), 9));
}

std::string len(const Length& l) {
    return "L(" + num(l.value) + "," + std::to_string(static_cast<int>(l.unit)) + ")";
}

std::string color(const Color& c) {
    return "C(" + num(c.r) + "," + num(c.g) + "," + num(c.b) + "," + num(c.a) + ")";
}

std::string stops(const GradientStop* s, size_t n) {
    std::string out = "[";
    for (size_t i = 0; i < n; ++i) {
        out += num(s[i].offset) + ":" + color(s[i].color) + ";";
    }
    return out + "]";
}

std::string easing(EasingKind kind, unsigned jumpEnd, unsigned steps, float x1, float y1, float x2, float y2,
                   const EasingPoint* points, size_t pointCount) {
    std::string out = "ease(" + std::to_string(static_cast<int>(kind));
    if (kind == EasingKind::Steps) {
        out += ",n" + std::to_string(steps) + ",j" + std::to_string(jumpEnd);
    } else if (kind == EasingKind::LinearPoints) {
        for (size_t i = 0; i < pointCount; ++i) {
            out += "," + num(points[i].in) + "/" + num(points[i].out);
        }
    } else if (kind != EasingKind::Linear) {
        out += "," + num(x1) + "," + num(y1) + "," + num(x2) + "," + num(y2);
    }
    return out + ")";
}

std::string effect(EffectKind kind, const float* params, const Color* colors, const GradientStop* s,
                   size_t n) {
    std::string out = "fx" + std::to_string(static_cast<int>(kind)) + "(";
    for (int i = 0; i < 8; ++i) {
        out += num(params[i]) + ",";
    }
    out += color(colors[0]) + color(colors[1]) + stops(s, n) + ")";
    return out;
}

std::string gradient(GradientKind kind, float angle, const Length& rx, const Length& ry, const Length& cx,
                     const Length& cy, const std::vector<GradientStop>& s) {
    return "G(" + std::to_string(static_cast<int>(kind)) + "," + num(angle) + "," + len(rx) + len(ry) +
           len(cx) + len(cy) + stops(s.data(), s.size()) + ")";
}

std::string clip(const ElementClipShape& shape) {
    std::string out = "CLIP(" + std::to_string(static_cast<int>(shape.kind));
    for (const Length& l : shape.params) {
        out += "," + len(l);
    }
    return out + ")";
}

std::string value(const Value& v) {
    switch (v.type) {
        case ValueType::Float:
            return "F(" + num(v.f) + ")";
        case ValueType::Length:
            return len(v.len);
        case ValueType::Color:
            return color(v.c);
        case ValueType::Enum:
            return "E(" + std::to_string(v.e) + ")";
        default:
            return "T" + std::to_string(static_cast<int>(v.type));
    }
}

/// The runtime's reading of one declaration, or "" when it rejects it.
std::string runtime(std::string_view css) {
    std::vector<mini_css::Decl> decls;
    if (mini_css::parseDeclarations(css, decls) != Status::kOk) {
        return "";
    }
    std::string out;
    for (const mini_css::Decl& d : decls) {
        out += "p" + std::to_string(d.prop) + "=";
        if (d.varRef) {
            out += "var(" + d.text + (d.hasVarFallback ? "," + value(d.varFallback) : std::string()) + ")";
        } else if (d.prop == style_decode::styleprop::Effect) {
            out += "FX[";
            for (const EffectInstance& e : d.effects) {
                out += effect(e.kind, e.params, e.colors, d.effectStops.data() + e.stopOffset, e.stopCount);
            }
            out += "]";
        } else if (d.prop == style_decode::styleprop::BackgroundGradient) {
            const ElementGradient& g = d.gradient;
            out += g.kind == GradientKind::None ? std::string("G(none)")
                                                : gradient(g.kind, g.angle, g.rx, g.ry, g.cx, g.cy, g.stops);
        } else if (d.prop == style_decode::styleprop::ClipPath) {
            out += clip(d.clipShape);
        } else if (d.prop == style_decode::styleprop::Animation) {
            out += "A[";
            for (const mini_css::AnimationDecl& a : d.animations) {
                const IrAnimationSpec& s = a.spec;
                const EasingKind kind = static_cast<EasingKind>(s.easing.kind);
                out += a.name + "," + num(s.duration) + "," + num(s.delay) + "," +
                       easing(kind, s.easing.stepsJumpEnd, s.easing.stepCount, s.easing.x1, s.easing.y1,
                              s.easing.x2, s.easing.y2, a.easingPoints.data(), a.easingPoints.size()) +
                       ",i" + (s.iterations == 0 ? std::string("inf") : std::to_string(s.iterations)) + ",d" +
                       std::to_string(s.direction) + ",f" + std::to_string(s.fill) + ",s" +
                       std::to_string(s.playState) + ";";
            }
            out += "]";
        } else if (d.prop == style_decode::styleprop::FontFamily ||
                   d.prop == style_decode::styleprop::BackgroundImage) {
            out += "S(" + d.text + ")";
        } else {
            out += value(d.value);
        }
        out += " ";
    }
    return out;
}

std::string styleValue(const compile::StyleValue& v, const std::vector<EasingPoint>& points) {
    using K = compile::StyleValueKind;
    switch (v.kind) {
        case K::Float:
            return "F(" + num(v.f) + ")";
        case K::Length:
            return len(v.len);
        case K::Color:
            return color(v.color);
        case K::Enum:
            return "E(" + std::to_string(v.e) + ")";
        case K::String:
            return "S(" + v.str + ")";
        case K::FontFamilyList:
            return "S(" + (v.families.empty() ? std::string() : v.families[0]) + ")";
        case K::VarRef:
            return "var(" + v.varName +
                   (v.varFallback.empty() ? std::string() : "," + styleValue(v.varFallback[0], points)) + ")";
        case K::EffectList: {
            std::string out = "FX[";
            for (const compile::EffectDeclEntry& e : v.effects) {
                out += effect(e.kind, e.params, e.colors, e.stops.data(), e.stops.size());
            }
            return out + "]";
        }
        case K::Gradient: {
            const compile::GradientDecl& g = v.gradient;
            return g.kind == GradientKind::None ? std::string("G(none)")
                                                : gradient(g.kind, g.angle, g.rx, g.ry, g.cx, g.cy, g.stops);
        }
        case K::ClipShape:
            return clip(v.clipShape);
        case K::AnimationList: {
            std::string out = "A[";
            for (const compile::AnimationSpec& a : v.animations) {
                const Easing& e = a.easing;
                const EasingPoint* p =
                    e.kind == EasingKind::LinearPoints ? points.data() + e.pointOffset : nullptr;
                out += a.name + "," + num(a.duration) + "," + num(a.delay) + "," +
                       easing(e.kind, e.stepsJumpEnd, e.stepCount, e.x1, e.y1, e.x2, e.y2, p, e.pointCount) +
                       ",i" + (a.iterations == 0xFFFF ? std::string("inf") : std::to_string(a.iterations)) +
                       ",d" + std::to_string(a.direction) + ",f" + std::to_string(a.fill) + ",s" +
                       std::to_string(a.playState) + ";";
            }
            return out + "]";
        }
        default:
            return "K" + std::to_string(static_cast<int>(v.kind));
    }
}

/// The compiler's reading of one declaration, or "" when it drops it.
std::string compiler(std::string_view css) {
    compile::DiagnosticSink sink;
    std::vector<compile::StyleDecl> decls;
    std::vector<EasingPoint> points;
    compile::parseDeclarations(css, "parity.xts", 1, sink, decls, &points);
    std::string out;
    for (const compile::StyleDecl& d : decls) {
        out += "p" + std::to_string(d.prop) + "=" + styleValue(d.value, points) + " ";
    }
    return out;
}

void sameAccepted(std::string_view css) {
    INFO(css);
    const std::string r = runtime(css);
    CHECK_FALSE(r.empty());
    CHECK(r == compiler(css));
}

void bothReject(std::string_view css) {
    INFO(css);
    CHECK(runtime(css).empty());
    CHECK(compiler(css).empty());
}

}  // namespace

TEST_CASE("css parity: lengths and plain numbers") {
    for (const char* css : {"width: 10px",
                            "height: 50%",
                            "margin-top: 2em",
                            "padding-left: 1.5rem",
                            "max-width: auto",
                            "padding-right: 0",
                            "left: -3px",
                            "top: 1e2px",
                            "font-size: 12.5px",
                            "line-height: 1.5",
                            "line-height: 0",
                            "line-height: 20px",
                            "perspective: none",
                            "perspective: 800px",
                            "opacity: 0.25",
                            "flex-grow: 2",
                            "flex-shrink: 0",
                            "z-index: 3",
                            "order: 2",
                            "order: -1",
                            "aspect-ratio: 16 / 9",
                            "aspect-ratio: 2",
                            "aspect-ratio: auto",
                            "image-opacity: 1",
                            "clip-rate: 1.5",
                            "clip-start: 0.2s",
                            "clip-start: 200ms",
                            "clip-start: 1.5",
                            "flex-basis: auto",
                            "row-gap: 4px",
                            "letter-spacing: 0.1em",
                            "outline-offset: 2px",
                            "text-decoration-thickness: auto",
                            "text-decoration-thickness: 2px"}) {
        sameAccepted(css);
    }
    for (const char* css :
         {"width: 10", "width: 10deg", "opacity: 1px", "order: 1.5", "aspect-ratio: 0", "aspect-ratio: 1 / 0",
          "order: 2147483648", "order: -2147483648", "line-height: auto", "aspect-ratio: -2", "clip-rate: 0",
          "clip-rate: -1", "order: 3e9", "text-decoration-thickness: -1px", "width: 10px 20px",
          "width: 1e999px"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: colours") {
    for (const char* css : {"color: #f00",
                            "color: #F00",
                            "color: #123",
                            "color: #456",
                            "color: #789",
                            "color: #abc",
                            "color: #def",
                            "color: #fff",
                            "color: #1234",
                            "color: #9abc",
                            "color: #ff0000",
                            "color: #00ff0080",
                            "color: #12345678",
                            "color: rgb(0, 0, 255)",
                            "color: rgb(10%, 20%, 30%)",
                            "color: rgba(255, 0, 0, 0.25)",
                            "color: transparent",
                            "color: orange",
                            "color: Red",
                            "color: rebeccapurple",
                            "color: hsl(120, 50%, 50%)",
                            "color: hsl(120deg 50% 50% / 0.5)",
                            "color: hsla(0.5turn, 10%, 20%, 50%)",
                            "color: hsl(1rad 0% 100%)",
                            "background-color: #00000040",
                            "border-color: red",
                            "border-color: red blue",
                            "border-color: red blue green",
                            "border-color: red blue green #fff",
                            "outline-color: #0f0",
                            "text-decoration-color: rgb(1, 2, 3)"}) {
        sameAccepted(css);
    }
    for (const char* css : {"color: #12345", "color: #ggg", "color: rgb(1, 2)", "color: rgba(1, 2, 3)",
                            "color: hsl(10, 20, 30)", "color: notacolor", "color: red blue",
                            "border-color: red blue green #fff #000"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: enums and keyword sets") {
    for (const char* css : {"display: flex",
                            "display: grid",
                            "display: none",
                            "font-weight: bold",
                            "font-weight: 700",
                            "font-weight: 100",
                            "line-clamp: 3",
                            "line-clamp: none",
                            "-webkit-line-clamp: 2",
                            "text-decoration-line: underline overline",
                            "text-decoration-line: none",
                            "text-decoration-line: line-through",
                            "overflow: hidden",
                            "overflow: hidden scroll",
                            "overflow-x: auto",
                            "mix-blend-mode: additive",
                            "mix-blend-mode: color-dodge",
                            "text-rendering: geometricPrecision",
                            "justify-content: space-between",
                            "align-self: end",
                            "justify-items: center",
                            "border-style: dashed",
                            "border-style: solid dotted",
                            "border-style: none solid dashed",
                            "border-style: none solid dashed dotted",
                            "border-top-style: dotted",
                            "outline-style: solid",
                            "background-repeat: repeat-x",
                            "image-rendering: pixelated",
                            "font-variant-numeric: tabular-nums",
                            "media-override: hide",
                            "app-region: no-drag",
                            "cursor: pointer",
                            "white-space: nowrap",
                            "pointer-watch: true",
                            "focus-nav: explicit",
                            "layer: overlay",
                            "writing-mode: vertical-rl"}) {
        sameAccepted(css);
    }
    for (const char* css :
         {"font-weight: 750", "font-weight: 1000", "line-clamp: 0", "line-clamp: 1.5", "display: inline",
          "text-decoration-line: underline underline", "overflow: hidden scroll auto", "border-style: wavy",
          "border-style: solid solid solid solid solid", "display: flex block"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: outline and text-decoration shorthands") {
    for (const char* css : {"outline: 2px solid red", "outline: none", "outline: solid", "outline: red 1px",
                            "outline: 1em solid #00f", "text-decoration: underline",
                            "text-decoration: underline red 2px", "text-decoration: none",
                            "text-decoration: line-through auto", "text-decoration: 1px overline blue"}) {
        sameAccepted(css);
    }
    for (const char* css : {"outline: 2px 3px", "outline: 10% solid", "outline: solid solid",
                            "text-decoration: underline red blue", "text-decoration: -1px"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: transform") {
    for (const char* css : {"transform: none",
                            "transform: translate(10px, 20%) rotate(45deg) scale(2)",
                            "transform: translateX(5px) translateZ(3px)",
                            "transform: translateX(5px) translateY(6px) translateZ(3px)",
                            "transform: translate3d(1px, 2px, 3px) rotateX(10deg) rotateY(20deg)",
                            "transform: translate(1rem)",
                            "transform: skew(10deg, 5deg) scale(2, 3)",
                            "transform: skewX(4deg) scaleY(0.5)",
                            "transform: rotateZ(30deg)",
                            "transform: scale(2) rotate(45deg)",
                            "transform: rotateY(18deg) translateZ(40px)",
                            "transform: rotate(10deg) translate(10px, 5px) rotate(-10deg)",
                            "transform: matrix(1, 0, 0, 1, 10, 20)",
                            "transform: matrix(0, 1, -1, 0, 0, 0)",
                            "translate: 10px",
                            "translate: 10px 20%",
                            "translate: 1px 2px 3px",
                            "translate: 1em 2em",
                            "translate: 1rem",
                            "translate: 1rem 2rem 3rem",
                            "translate: none",
                            "rotate: 45deg",
                            "rotate: z 10deg",
                            "rotate: none",
                            "scale: 2",
                            "scale: 2 3",
                            "scale: none",
                            "transform-origin: left top",
                            "transform-origin: center",
                            "transform-origin: 10px 20%",
                            "transform-origin: right bottom",
                            "perspective-origin: 30% 70%",
                            "background-position: center top",
                            "background-position: 5px",
                            "background-size: cover",
                            "background-size: contain",
                            "background-size: 10px auto",
                            "background-size: 50%"}) {
        sameAccepted(css);
    }
    for (const char* css : {"transform: none rotate(10deg)",
                            "transform: translateZ(10%)",
                            "transform: rotate3d(1, 0, 0, 10deg)",
                            "transform: perspective(100px)",
                            "transform: wobble(1px)",
                            "transform: rotate(10px)",
                            "transform: translate(1em) rotate(10deg) translate(2px)",
                            "transform: translateX(5px) translateY(1em)",
                            "transform: scale(1px)",
                            "transform: translate(10px",
                            "translate: 1px 2px 3%",
                            "translate: 1px 2px 3px 4px",
                            "rotate: x 10deg",
                            "rotate: 10px",
                            "scale: 1px",
                            "scale: 1 2 3",
                            "transform-origin: top left",
                            "transform-origin: 1px 2px 3px",
                            "background-size: -5px",
                            "background-size: cover cover"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: effect") {
    for (const char* css :
         {"effect: none", "effect: shadow(0 4px 12px #00000040)", "effect: shadow(1px 2px 3px 4px red 0.5)",
          "effect: glow(8px #f00 2)", "effect: blur(4px)", "effect: blur(2px 3px)", "effect: blur(1em)",
          "effect: color-adjust(brightness(1.2) saturate(0.5))",
          "effect: color-adjust(hue-rotate(30deg) contrast(1.1))",
          "effect: gradient-mask(linear, 0 0, 0 100px, #fff 0%, #0000 100%)",
          "effect: gradient-mask(radial, 10px 10px, 1em 0, red 0%, blue 50%, invert)",
          "effect: shine(45deg 20px #fff 0.8 0.5 0.3)", "effect: outline(1px #fff inner)",
          "effect: outline(2px red 1px center)", "effect: backdrop-blur(10px) shadow(0 1px 2px #000)",
          "effect: glow(4px red) blur(2px)"}) {
        sameAccepted(css);
    }
    for (const char* css : {"effect: gradient-mask(0 0, 0 100px, #fff 0%)", "effect: outline(2px 1px red)",
                            "effect: gradient-mask(linear, 0 0, 0 1px, red)", "effect: shine(45deg 20% #fff)",
                            "effect: shadow(10% 4px 12px #000)", "effect: blur()", "effect: wobble(1px)",
                            "effect: shadow(0 4px 12px #000) backdrop-blur(10px)", "effect: blur(4px",
                            "effect: glow(4px red) none"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: background-gradient") {
    for (const char* css :
         {"background-gradient: none", "background-gradient: linear-gradient(90deg, red, blue)",
          "background-gradient: linear-gradient(0.25turn, red 10%, blue 90%)",
          "background-gradient: linear-gradient(1rad, red, green, blue)",
          "background-gradient: linear-gradient(45deg, red 0%, #0f0, #00f, white 100%)",
          "background-gradient: radial-gradient(10px 20px at 50% 50%, red, blue)",
          "background-gradient: radial-gradient(30% 40% at 0 100%, red 20%, blue)",
          "background-gradient: conic-gradient(from 45deg at 10px 20px, red, blue 50%, green)",
          "background-gradient: conic-gradient(90deg, red, blue)"}) {
        sameAccepted(css);
    }
    for (const char* css :
         {"background-gradient: linear-gradient(90deg)", "background-gradient: linear-gradient(red, blue)",
          "background-gradient: radial-gradient(10px at 5px 5px, red, blue)",
          "background-gradient: repeating-linear-gradient(90deg, red, blue)",
          "background-gradient: linear-gradient(90deg, red 10px, blue)"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: clip-path") {
    for (const char* css :
         {"clip-path: none", "clip-path: inset(10px)", "clip-path: inset(10px 20px round 5px)",
          "clip-path: inset(1px 2px 3px 4px round 5px 6px / 7px)", "clip-path: circle(50% at 10px 20px)",
          "clip-path: circle()", "clip-path: circle(farthest-side)", "clip-path: ellipse(10px 20px)",
          "clip-path: ellipse(closest-side farthest-side at 30% 40%)",
          "clip-path: polygon(0 0, 100% 0, 50% 100%)"}) {
        sameAccepted(css);
    }
    for (const char* css :
         {"clip-path: path(\"M0 0\")", "clip-path: url(#c)", "clip-path: polygon(0 0, 1px 1px)",
          "clip-path: circle(1px 2px)", "clip-path: border-box", "clip-path: inset()"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: animation") {
    for (const char* css :
         {"animation: spin 2s", "animation: spin 2s ease-in 1s infinite alternate forwards paused",
          "animation: spin 500ms steps(4, start) 3", "animation: spin 1s step-end",
          "animation: a 1s linear(0, 0.5 50%, 1)",
          "animation: a 1s, b 2s cubic-bezier(0.1, 0.2, 0.3, 0.4) reverse both",
          "animation: a 1s ease-out 2.7", "animation: a 1s steps(0)", "animation: a 1s steps(70000)",
          "animation: a 1s 0, b 2s"}) {
        sameAccepted(css);
    }
    for (const char* css :
         {"animation: 2s spin", "animation: spin", "animation: spin 2s 3 4", "animation: spin 2s wobble"}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: font-family, background-image, ui-scale, hit-slop, grid lines, var()") {
    for (const char* css : {"font-family: \"Noto Sans\", serif",
                            "font-family: Arial",
                            "font-family: sans serif, mono",
                            "ui-scale: auto",
                            "ui-scale: none",
                            "ui-scale: clamp(0.5, 2)",
                            "hit-slop: 4px",
                            "hit-slop: 1px 2px",
                            "hit-slop: 1px 2px 3px 4px",
                            "hit-slop: 10% 1em",
                            "grid-column-start: 2",
                            "grid-row-end: span 3",
                            "grid-column-end: auto",
                            "grid-column: 1 / 3",
                            "grid-row: span 2",
                            "grid-column: 2",
                            "width: var(--w)",
                            "width: var(--w, 10px)",
                            "color: var(--c, #fff)",
                            "display: var(--d, flex)",
                            "opacity: var(--o, 0.5)"}) {
        sameAccepted(css);
    }
    for (const char* css :
         {"font-family: \"A\",", "font-family: 12", "ui-scale: clamp(2, 1)", "ui-scale: clamp(0, 1)",
          "hit-slop: -1px", "hit-slop: 1px 2px 3px 4px 5px", "hit-slop: auto", "grid-column-start: 0",
          "grid-column-start: 1.5", "grid-column: 1 / 2 / 3", "font-family: \"\""}) {
        bothReject(css);
    }
}

TEST_CASE("css parity: the differences the design documents keep") {
    // 05 §5.9.3 (T-176): setStyle does not take the viewport / safe-area / uiscale units.
    for (const char* css : {"width: 10vw", "height: 5vh", "left: 1vmin", "top: 2vmax",
                            "padding-top: 1safe-top", "font-size: 12uiscale"}) {
        INFO(std::string(css));
        CHECK(runtime(css).empty());
        CHECK_FALSE(compiler(css).empty());
    }
    // 05 §5.y: the runtime `background-image` takes a bare or quoted asset name, no url(); the
    // compiler takes `none` or url().
    CHECK(runtime("background-image: foo") == "p182=S(foo) ");
    CHECK(compiler("background-image: foo").empty());
    CHECK(runtime("background-image: url(foo.png)").empty());
    CHECK(compiler("background-image: url(foo.png)") == "p182=S(foo.png) ");
    // 18.4: custom() effects compile; setStyle reports them unsupported.
    CHECK(runtime("effect: custom(\"fx.xtfx\")").empty());
    CHECK_FALSE(compiler("effect: custom(\"fx.xtfx\")").empty());
    // 05.x (T-272): a finite iteration count saturates at 65535 at runtime, the compiler clamps it
    // to 65534 (both finite).
    CHECK(runtime("animation: a 1s 70000").find(",i65535,") != std::string::npos);
    CHECK(compiler("animation: a 1s 70000").find(",i65534,") != std::string::npos);
}

TEST_CASE("css parity: keyword case (05 §5.3, ruling 2026-10-21; T-421)") {
    // Keywords match ASCII case-insensitively in both parsers; named colours always did (pinned
    // above by `color: Red`).
    for (const char* css : {"display: FLEX", "width: AUTO", "transform-origin: LEFT TOP",
                            "animation: a 1s EASE-IN", "clip-path: CIRCLE()", "background-size: COVER",
                            "outline: 1px SOLID red", "text-decoration-line: UNDERLINE", "ui-scale: NONE",
                            "perspective: None", "animation: NONE", "position: Absolute"}) {
        sameAccepted(css);
    }
    // The upper-case spelling reads exactly as the lower-case one.
    CHECK(compiler("display: FLEX") == compiler("display: flex"));
    CHECK(compiler("transform-origin: LEFT TOP") == compiler("transform-origin: left top"));
    // An animation name is not a keyword: it keeps its case.
    CHECK(compiler("animation: Spin 1s") != compiler("animation: spin 1s"));
}

TEST_CASE("css parity: unit case (05 §5.3, ruling 2026-10-21; T-421)") {
    // Units are ASCII case-insensitive in both parsers and read as their lower-case spelling.
    for (const char* css : {"width: 10PX", "width: 2Em", "font-size: 1REM", "transform: rotate(45DEG)",
                            "animation: a 500MS", "animation: a 2S", "effect: blur(4PX)"}) {
        sameAccepted(css);
    }
    CHECK(runtime("width: 10PX") == runtime("width: 10px"));
    CHECK(compiler("width: 10PX") == compiler("width: 10px"));
    CHECK(runtime("animation: a 500MS") == runtime("animation: a 500ms"));
    // The T-176 units stay compiler-only (05 §5.9.3), in any case.
    CHECK(runtime("width: 10VW").empty());
    CHECK(compiler("width: 10VW") == compiler("width: 10vw"));
}

TEST_CASE("css parity: function names and the remaining keywords (05 §5.3; T-421)") {
    for (const char* css :
         {"transform: ROTATE(45deg)", "transform: NONE", "transform: translateX(5px) TRANSLATEY(6px)",
          "transform: Matrix(1, 0, 0, 1, 5, 6)", "aspect-ratio: AUTO", "grid-column-start: AUTO",
          "grid-row-end: SPAN 3", "effect: NONE",
          "effect: gradient-mask(LINEAR, 0 0, 0 100px, #fff 0%, #0000 100%)",
          "effect: gradient-mask(Radial, 10px 10px, 1em 0, red 0%, blue 50%, INVERT)",
          "effect: outline(1px #fff INNER)", "effect: SHADOW(0 1px 2px #000)"}) {
        sameAccepted(css);
    }
    CHECK(compiler("transform: ROTATE(45deg)") == compiler("transform: rotate(45deg)"));
    CHECK(runtime("transform: ROTATE(45deg)") == runtime("transform: rotate(45deg)"));
    for (const char* css : {"clip-path: PATH(\"M0 0\")", "clip-path: URL(#c)"}) {
        bothReject(css);
    }
    // Compiler-only syntax: calc() / min() / var().
    CHECK(compiler("width: CALC(10px + 2em)") == compiler("width: calc(10px + 2em)"));
    CHECK(compiler("width: Min(10px, 2em)") == compiler("width: min(10px, 2em)"));
    CHECK(compiler("width: VAR(--w, 10px)") == compiler("width: var(--w, 10px)"));
    CHECK_FALSE(compiler("width: CALC(10px + 2em)").empty());
}

namespace {

/// The compiler's reading of one `transition` declaration: whether it was kept, its list and the
/// warnings it raised.
struct TransitionResult {
    bool kept = false;
    std::vector<compile::TransitionSpec> list;
    std::vector<std::string> warnings;
};

TransitionResult compileTransition(std::string_view css) {
    compile::DiagnosticSink sink;
    std::vector<compile::StyleDecl> decls;
    std::vector<EasingPoint> points;
    compile::parseDeclarations(css, "parity.xts", 1, sink, decls, &points);
    TransitionResult r;
    r.kept = !decls.empty();
    if (r.kept) {
        r.list = decls[0].value.transitions;
    }
    for (const compile::Diagnostic& d : sink.all()) {
        r.warnings.push_back(d.message);
    }
    return r;
}

}  // namespace

TEST_CASE("css parity: a malformed transition item drops the declaration (05 §5.3; T-421)") {
    // Well formed: kept, no warning; keywords and units in any case.
    TransitionResult r = compileTransition("transition: opacity 200ms EASE-IN 50MS, ALL 1S");
    CHECK(r.kept);
    CHECK(r.list.size() == 2);
    CHECK(r.warnings.empty());
    // `none` is the empty list, without a warning.
    for (const char* css : {"transition: none", "transition: NONE"}) {
        INFO(std::string(css));
        r = compileTransition(css);
        CHECK(r.kept);
        CHECK(r.list.empty());
        CHECK(r.warnings.empty());
    }
    // One malformed item drops the whole declaration with one warning.
    for (const char* css : {"transition: opacity 1s, 2s", "transition: opacity 1s, left wobble",
                            "transition: opacity 1s, left 1s 2s 3s", "transition: opacity 1s,",
                            "transition: opacity", "transition: opacity 1s cubic-bezier(1)"}) {
        INFO(std::string(css));
        r = compileTransition(css);
        CHECK_FALSE(r.kept);
        REQUIRE(r.warnings.size() == 1);
        CHECK(r.warnings[0].find("invalid value for \"transition\"") != std::string::npos);
    }
    // A well-formed item naming an unknown or non-animatable property is skipped with its own
    // warning; the rest of the list stays.
    r = compileTransition("transition: opacity 1s, nosuch 1s");
    CHECK(r.kept);
    REQUIRE(r.list.size() == 1);
    CHECK(r.list[0].prop == prop::Opacity);
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings[0].find("unknown or non-animatable transition property") != std::string::npos);
}
