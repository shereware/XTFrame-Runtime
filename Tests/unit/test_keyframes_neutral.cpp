#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/node.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-869 (Docs/design/12-compiler.md §12.3, rewritten 2026-11-22): a `@keyframes` block without a
/// `from`, a `to` or both interpolates toward the property's base value at that moment, as CSS does.
/// The compiler marks the missing end with a Neutral keyframe; the Animator substitutes the base
/// value of the property (the value it has without this animation) and interpolates.

using namespace xt;
using namespace xt::compile;

namespace {

struct Loaded {
    NullTextShaper shaper;
    std::unique_ptr<FileAssetResolver> assets;
    std::unique_ptr<Document> doc;
};

/// Compiles `source` (an `.xt` text) and loads it; the first update (clock 0) starts its animations.
void load(std::string_view source, Loaded& out) {
    SourceFile files[1] = {SourceFile{"neutral.xt", source}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "neutral.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    out.assets = std::make_unique<FileAssetResolver>(XT_TEST_LAYOUT_DIR, nullptr);
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &out.shaper, out.assets.get(),
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 300});
    out.doc->update(0.0f);
}

/// One `#b` element with `boxStyle` and the `@keyframes` text `keyframes`.
std::string page(std::string_view boxStyle, std::string_view keyframes, std::string_view extraRules = {}) {
    std::string s =
        "<xt>\n  <head>\n    <style>\n      #b { position: absolute; left: 0px; top: 0px; "
        "width: 100px; height: 40px; ";
    s += boxStyle;
    s += " }\n      ";
    s += extraRules;
    s += "\n      ";
    s += keyframes;
    s += "\n    </style>\n  </head>\n  <body><div id=\"b\"></div></body>\n</xt>\n";
    return s;
}

float opacityAt(Loaded& l, float clock) {
    l.doc->update(clock);
    return l.doc->byId("b").property(prop::Opacity).f;
}

float floatAt(Loaded& l, float clock, PropId p) {
    l.doc->update(clock);
    return l.doc->byId("b").property(p).f;
}

}  // namespace

TEST_CASE("T-869: from-only keyframes interpolate to the base value under every fill") {
    const char* fills[] = {"none", "forwards", "backwards", "both"};
    for (const char* fill : fills) {
        CAPTURE(fill);
        Loaded l;
        load(page(std::string("opacity: 0.8; animation: a 1s linear ") + fill + ";",
                  "@keyframes a { from { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.0f) == doctest::Approx(0.0f));
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 0.5f) == doctest::Approx(0.4f));
        CHECK(opacityAt(l, 0.75f) == doctest::Approx(0.6f));
        CHECK(opacityAt(l, 1.0f) == doctest::Approx(0.8f));
        CHECK(opacityAt(l, 1.1f) == doctest::Approx(0.8f));  // After the end: the base value.
    }
}

TEST_CASE("T-869: to-only keyframes start from the base value under every fill") {
    const char* fills[] = {"none", "forwards", "backwards", "both"};
    for (const char* fill : fills) {
        CAPTURE(fill);
        const bool forwards = std::string(fill) == "forwards" || std::string(fill) == "both";
        Loaded l;
        load(page(std::string("opacity: 0.8; animation: a 1s linear ") + fill + ";",
                  "@keyframes a { to { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.0f) == doctest::Approx(0.8f));
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.6f));
        CHECK(opacityAt(l, 0.5f) == doctest::Approx(0.4f));
        CHECK(opacityAt(l, 0.75f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 1.0f) == doctest::Approx(0.0f));
        CHECK(opacityAt(l, 1.1f) == doctest::Approx(forwards ? 0.0f : 0.8f));
    }
}

TEST_CASE("T-869: 50%-only keyframes run base -> value -> base under every fill") {
    const char* fills[] = {"none", "forwards", "backwards", "both"};
    for (const char* fill : fills) {
        CAPTURE(fill);
        Loaded l;
        load(page(std::string("opacity: 0.8; animation: a 1s linear ") + fill + ";",
                  "@keyframes a { 50% { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.0f) == doctest::Approx(0.8f));
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.4f));
        CHECK(opacityAt(l, 0.5f) == doctest::Approx(0.0f));
        CHECK(opacityAt(l, 0.75f) == doctest::Approx(0.4f));
        CHECK(opacityAt(l, 1.0f) == doctest::Approx(0.8f));
        CHECK(opacityAt(l, 1.1f) == doctest::Approx(0.8f));
    }
}

TEST_CASE("T-869: the delay of a backwards fill writes the start key, or nothing when it is Neutral") {
    {
        Loaded l;
        load(page("opacity: 0.8; animation: a 1s linear 0.5s backwards;",
                  "@keyframes a { from { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.0f));  // In the delay: the from key.
        CHECK(opacityAt(l, 1.0f) == doctest::Approx(0.4f));
    }
    {
        Loaded l;
        load(page("opacity: 0.8; animation: a 1s linear 0.5s both;", "@keyframes a { to { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.8f));  // In the delay: 0% is Neutral.
        CHECK(opacityAt(l, 1.0f) == doctest::Approx(0.4f));
        CHECK(opacityAt(l, 1.6f) == doctest::Approx(0.0f));
    }
}

TEST_CASE("T-869: from-only scaleY(0) grows to the element's own transform") {
    {
        Loaded l;
        load(page("animation: g 1s linear both;", "@keyframes g { from { transform: scaleY(0) } }"), l);
        CHECK(floatAt(l, 0.0f, prop::ScaleY) == doctest::Approx(0.0f));
        CHECK(floatAt(l, 0.5f, prop::ScaleY) == doctest::Approx(0.5f));
        CHECK(floatAt(l, 1.0f, prop::ScaleY) == doctest::Approx(1.0f));
        CHECK(floatAt(l, 1.5f, prop::ScaleY) == doctest::Approx(1.0f));
    }
    {
        Loaded l;
        load(page("transform: translateX(10px) rotate(20deg); animation: g 1s linear both;",
                  "@keyframes g { from { transform: scaleY(0) } }"),
             l);
        // 05.5 component model: the key declares ScaleY only, so translate and rotate are not
        // animated and keep the element's own values.
        CHECK(floatAt(l, 0.0f, prop::ScaleY) == doctest::Approx(0.0f));
        CHECK(floatAt(l, 0.0f, prop::TranslateX) == doctest::Approx(10.0f));
        CHECK(floatAt(l, 0.25f, prop::ScaleY) == doctest::Approx(0.25f));
        CHECK(floatAt(l, 0.25f, prop::Rotation) == doctest::Approx(20.0f));
        CHECK(floatAt(l, 1.0f, prop::ScaleY) == doctest::Approx(1.0f));
        CHECK(floatAt(l, 2.0f, prop::ScaleY) == doctest::Approx(1.0f));
        CHECK(floatAt(l, 2.0f, prop::Rotation) == doctest::Approx(20.0f));
    }
    {
        // The same components on both sides: each one interpolates to the element's own value.
        Loaded l;
        load(page("transform: translateX(10px) rotate(20deg); animation: g 1s linear both;",
                  "@keyframes g { from { transform: translateX(0px) rotate(0deg) } }"),
             l);
        CHECK(floatAt(l, 0.0f, prop::TranslateX) == doctest::Approx(0.0f));
        CHECK(floatAt(l, 0.25f, prop::TranslateX) == doctest::Approx(2.5f));
        CHECK(floatAt(l, 0.25f, prop::Rotation) == doctest::Approx(5.0f));
        CHECK(floatAt(l, 0.75f, prop::Rotation) == doctest::Approx(15.0f));
        CHECK(floatAt(l, 1.0f, prop::TranslateX) == doctest::Approx(10.0f));
        CHECK(floatAt(l, 2.0f, prop::Rotation) == doctest::Approx(20.0f));
    }
    {
        // A percent translate base met by a px key interpolates in the (px, %) form.
        Loaded l;
        load(page("transform: translateX(-50%); animation: g 1s linear both;",
                  "@keyframes g { from { transform: translateX(0px) scale(0) } }"),
             l);
        l.doc->update(0.5f);
        CHECK(l.doc->byId("b").property(prop::TranslateX).f == doctest::Approx(0.0f));  // the px part
        CHECK(floatAt(l, 0.5f, prop::ScaleX) == doctest::Approx(0.5f));
    }
}

TEST_CASE("T-869: an effect parameter interpolates to the element's own effect") {
    Loaded l;
    load(page("effect: blur(8px); animation: f 1s linear both;",
              "@keyframes f { from { effect: blur(0px) } }"),
         l);
    const PropId radius = effectParam(0, 1);
    CHECK(floatAt(l, 0.0f, radius) == doctest::Approx(0.0f));
    CHECK(floatAt(l, 0.25f, radius) == doctest::Approx(2.0f));
    CHECK(floatAt(l, 0.5f, radius) == doctest::Approx(4.0f));
    CHECK(floatAt(l, 1.0f, radius) == doctest::Approx(8.0f));
    CHECK(floatAt(l, 1.5f, radius) == doctest::Approx(8.0f));
}

TEST_CASE("T-869: an enumerated property switches at 50% against the base value") {
    Loaded l;
    load(page("animation: v 1s linear both;", "@keyframes v { from { visibility: hidden } }"), l);
    auto visibleAt = [&](float clock) {
        l.doc->update(clock);
        return l.doc->byId("b").property(prop::Visible).e;
    };
    CHECK(visibleAt(0.0f) == 0u);
    CHECK(visibleAt(0.25f) == 0u);
    CHECK(visibleAt(0.5f) == 1u);
    CHECK(visibleAt(0.75f) == 1u);
    CHECK(visibleAt(1.5f) == 1u);
}

TEST_CASE("T-869: a base value that changes mid-animation is followed") {
    Loaded l;
    load(page("opacity: 0.8; animation: a 1s linear both;", "@keyframes a { from { opacity: 0 } }",
              "#b.dim { opacity: 0.4; }"),
         l);
    CHECK(opacityAt(l, 0.5f) == doctest::Approx(0.4f));
    REQUIRE(l.doc->byId("b").addClass("dim"));
    CHECK(opacityAt(l, 0.5f) == doctest::Approx(0.2f));
    CHECK(opacityAt(l, 0.75f) == doctest::Approx(0.3f));
    CHECK(opacityAt(l, 1.5f) == doctest::Approx(0.4f));

    // A layout property (written into the element's computed style by the Animator) too.
    Loaded w;
    load(page("animation: grow 1s linear both;", "@keyframes grow { from { width: 0px } }",
              "#b.wide { width: 200px; }"),
         w);
    CHECK(floatAt(w, 0.5f, prop::Width) == doctest::Approx(50.0f));
    REQUIRE(w.doc->byId("b").addClass("wide"));
    CHECK(floatAt(w, 0.5f, prop::Width) == doctest::Approx(100.0f));
    CHECK(floatAt(w, 0.75f, prop::Width) == doctest::Approx(150.0f));
    CHECK(floatAt(w, 1.5f, prop::Width) == doctest::Approx(200.0f));
}

TEST_CASE("T-869: reverse and alternate directions run through the base value") {
    {
        Loaded l;
        load(page("opacity: 0.8; animation: a 1s linear both reverse;",
                  "@keyframes a { from { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.0f) == doctest::Approx(0.8f));
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.6f));
        CHECK(opacityAt(l, 0.75f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 1.5f) == doctest::Approx(0.0f));  // T-872: forwards ends on 0%.
    }
    {
        // T-872: a finite alternate runs back in its second iteration and ends on 0%.
        Loaded l;
        load(page("opacity: 0.8; animation: a 1s linear 2 both alternate;",
                  "@keyframes a { from { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 1.25f) == doctest::Approx(0.6f));
        CHECK(opacityAt(l, 1.75f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 2.5f) == doctest::Approx(0.0f));
    }
    {
        Loaded l;
        load(page("opacity: 0.8; animation: a 1s linear infinite alternate;",
                  "@keyframes a { from { opacity: 0 } }"),
             l);
        CHECK(opacityAt(l, 0.25f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 1.25f) == doctest::Approx(0.6f));
        CHECK(opacityAt(l, 1.75f) == doctest::Approx(0.2f));
        CHECK(opacityAt(l, 2.25f) == doctest::Approx(0.2f));
    }
}

TEST_CASE("T-869 A: a stop without `effect` is not an effect keyframe") {
    // 0% blur(4px), 52% only opacity, 100% blur(0px): the blur runs 4 -> 0 over the whole span,
    // like the same block without the 52% stop.
    Loaded gap;
    load(page("effect: blur(8px); opacity: 0.8; animation: a 1s linear both;",
              "@keyframes a { 0% { effect: blur(4px); opacity: 0 } 52% { opacity: 0.5 } 100% { effect: "
              "blur(0px) } }"),
         gap);
    Loaded plain;
    load(page("effect: blur(8px); opacity: 0.8; animation: a 1s linear both;",
              "@keyframes a { 0% { effect: blur(4px) } 100% { effect: blur(0px) } }"),
         plain);
    const PropId radius = effectParam(0, 1);
    const float samples[] = {0.0f, 0.26f, 0.52f, 0.75f, 1.0f, 1.5f};
    for (float t : samples) {
        CAPTURE(t);
        CHECK(floatAt(gap, t, radius) == doctest::Approx(floatAt(plain, t, radius)));
    }
    CHECK(floatAt(gap, 0.26f, radius) == doctest::Approx(2.96f));
    // The opacity track still has its 52% key and a Neutral end toward 0.8.
    CHECK(opacityAt(gap, 0.52f) == doctest::Approx(0.5f));
    CHECK(opacityAt(gap, 1.0f) == doctest::Approx(0.8f));
}

TEST_CASE("T-869 A: a block whose only effect stop is in the middle interpolates from and to the base") {
    Loaded l;
    load(page("effect: blur(8px); animation: a 1s linear both;",
              "@keyframes a { 0% { opacity: 1 } 50% { effect: blur(0px) } 100% { opacity: 1 } }"),
         l);
    const PropId radius = effectParam(0, 1);
    CHECK(floatAt(l, 0.0f, radius) == doctest::Approx(8.0f));
    CHECK(floatAt(l, 0.25f, radius) == doctest::Approx(4.0f));
    CHECK(floatAt(l, 0.5f, radius) == doctest::Approx(0.0f));
    CHECK(floatAt(l, 0.75f, radius) == doctest::Approx(4.0f));
    CHECK(floatAt(l, 1.0f, radius) == doctest::Approx(8.0f));
}
