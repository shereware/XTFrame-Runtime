#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-872 (Docs/design/12-compiler.md §12.3 "effect keyframe sequences", 18-effects.md §18.3.1):
/// effect keyframes whose sequences differ only by a prefix (or `none`) pad the shorter side with
/// each effect's neutral parameters and interpolate -- the same parameters, at every moment, as the
/// block written out by hand with the neutral effects in place. Anything else warns and does not
/// animate.

using namespace xt;
using namespace xt::compile;

namespace {

struct Loaded {
    NullTextShaper shaper;
    std::unique_ptr<FileAssetResolver> assets;
    std::unique_ptr<Document> doc;
    std::vector<std::string> warnings;
};

void load(std::string_view source, Loaded& out) {
    SourceFile files[1] = {SourceFile{"padding.xt", source}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "padding.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
        if (d.level == Diagnostic::Level::Warning) {
            out.warnings.push_back(d.message);
        }
    }
    REQUIRE(result.ok());
    out.assets = std::make_unique<FileAssetResolver>(XT_TEST_LAYOUT_DIR, nullptr);
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &out.shaper, out.assets.get(),
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 300});
    out.doc->update(0.0f);
}

bool warnedAboutSequences(const Loaded& l) {
    for (const std::string& w : l.warnings) {
        if (w.find("different effect sequences") != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// `#b` with the static effect `effect` and `animation: a 1s linear both` over `keyframes`.
std::string page(std::string_view effect, std::string_view keyframes) {
    std::string s =
        "<xt>\n  <head>\n    <style>\n      #b { position: absolute; left: 0px; top: 0px; width: 100px; "
        "height: 40px; effect: ";
    s += effect;
    s += "; animation: a 1s linear both; }\n      @keyframes a { ";
    s += keyframes;
    s += " }\n    </style>\n  </head>\n  <body><div id=\"b\"></div></body>\n</xt>\n";
    return s;
}

/// Every built-in float parameter (params[0..7]) and colors[0] of effects 0..count-1 at `clock`.
std::vector<float> effectParamsAt(Loaded& l, float clock, uint32_t count) {
    l.doc->update(clock);
    ElementRef b = l.doc->byId("b");
    std::vector<float> out;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t p = 0; p < 8; ++p) {
            out.push_back(b.property(effectParam(i, prop::kEffectParamFloatBase + p)).f);
        }
        const Value c = b.property(effectParam(i, prop::kEffectParamColorBase));
        out.push_back(c.c.r);
        out.push_back(c.c.g);
        out.push_back(c.c.b);
        out.push_back(c.c.a);
    }
    return out;
}

/// The padded block `padded` and the hand-written block `byHand` give the same parameters.
void checkSameAsByHand(std::string_view effect, uint32_t count, std::string_view padded,
                       std::string_view byHand) {
    Loaded a;
    load(page(effect, padded), a);
    Loaded b;
    load(page(effect, byHand), b);
    CHECK_FALSE(warnedAboutSequences(a));
    CHECK_FALSE(warnedAboutSequences(b));
    const float samples[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f};
    for (float t : samples) {
        CAPTURE(t);
        const std::vector<float> pa = effectParamsAt(a, t, count);
        const std::vector<float> pb = effectParamsAt(b, t, count);
        REQUIRE(pa.size() == pb.size());
        for (size_t k = 0; k < pa.size(); ++k) {
            CAPTURE(k);
            CHECK(pa[k] == doctest::Approx(pb[k]));
        }
    }
}

/// The effect keeps its static parameters at every moment and the compiler warned.
void checkWarnsAndStatic(std::string_view effect, uint32_t count, std::string_view keyframes) {
    Loaded l;
    load(page(effect, keyframes), l);
    CHECK(warnedAboutSequences(l));
    const std::vector<float> start = effectParamsAt(l, 0.0f, count);
    const float samples[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.5f};
    for (float t : samples) {
        CAPTURE(t);
        const std::vector<float> now = effectParamsAt(l, t, count);
        REQUIRE(now.size() == start.size());
        for (size_t k = 0; k < now.size(); ++k) {
            CAPTURE(k);
            CHECK(now[k] == doctest::Approx(start[k]));
        }
    }
}

}  // namespace

TEST_CASE("T-872: none -> blur(8px) grows the radius from 0") {
    checkSameAsByHand("blur(8px)", 1, "from { effect: none } to { effect: blur(8px) }",
                      "from { effect: blur(0px) } to { effect: blur(8px) }");
    Loaded l;
    load(page("blur(8px)", "from { effect: none } to { effect: blur(8px) }"), l);
    CHECK(effectParamsAt(l, 0.25f, 1)[0] == doctest::Approx(2.0f));
    CHECK(effectParamsAt(l, 0.5f, 1)[1] == doctest::Approx(4.0f));
}

TEST_CASE("T-872: a prefix is padded with the neutral color adjustment") {
    checkSameAsByHand("blur(4px) color-adjust(brightness(0.5))", 2,
                      "from { effect: blur(4px) } to { effect: blur(4px) color-adjust(brightness(0.5)) }",
                      "from { effect: blur(4px) color-adjust() } to { effect: blur(4px) "
                      "color-adjust(brightness(0.5)) }");
    // Reversed: the longer sequence first.
    checkSameAsByHand(
        "blur(4px) color-adjust(contrast(0.4) opacity(0.5))", 2,
        "from { effect: blur(2px) color-adjust(contrast(0.4) opacity(0.5)) } to { effect: blur(6px) }",
        "from { effect: blur(2px) color-adjust(contrast(0.4) opacity(0.5)) } to { effect: blur(6px) "
        "color-adjust() }");
}

TEST_CASE("T-872: three stops blur -> blur shadow -> blur pad both ends") {
    checkSameAsByHand(
        "blur(2px) shadow(4px 6px 3px 1px #102030 0.5)", 2,
        "0% { effect: blur(2px) } 50% { effect: blur(6px) shadow(4px 6px 3px 1px #102030 0.5) } "
        "100% { effect: blur(0px) }",
        "0% { effect: blur(2px) shadow(0px 0px 0px 0px rgba(16, 32, 48, 0) 0.5) } 50% { effect: "
        "blur(6px) shadow(4px 6px 3px 1px #102030 0.5) } 100% { effect: blur(0px) shadow(0px 0px 0px "
        "0px rgba(16, 32, 48, 0) 0.5) }");
}

TEST_CASE("T-872: glow, outline and shine pad with their neutral parameters") {
    checkSameAsByHand(
        "glow(6px #ff8000 2) outline(3px #00ff00 1px center) shine(30deg 20px #ffffff 1.5 0.4 0.3)", 3,
        "from { effect: none } to { effect: glow(6px #ff8000 2) outline(3px #00ff00 1px center) "
        "shine(30deg 20px #ffffff 1.5 0.4 0.3) }",
        "from { effect: glow(6px #ff8000 0) outline(0px rgba(0, 255, 0, 0) 1px center) shine(30deg "
        "20px #ffffff 0 0.4 0.3) } to { effect: glow(6px #ff8000 2) outline(3px #00ff00 1px center) "
        "shine(30deg 20px #ffffff 1.5 0.4 0.3) }");
}

TEST_CASE("T-872: merged color-adjust terms are one effect and interpolate as before") {
    // The importer merges `brightness()` / `contrast()` into one color-adjust(); the sequence is
    // the same on both stops, so the parameters interpolate without padding.
    Loaded l;
    load(page("color-adjust(brightness(0.2))",
              "from { effect: color-adjust(brightness(0.2)) } to { effect: color-adjust(brightness(0.2) "
              "contrast(0.1)) }"),
         l);
    CHECK_FALSE(warnedAboutSequences(l));
    CHECK(effectParamsAt(l, 0.5f, 1)[1] == doctest::Approx(0.05f));
}

TEST_CASE("T-872: sequences that are not prefixes warn and stay static") {
    checkWarnsAndStatic("blur(4px)", 1, "from { effect: blur(2px) } to { effect: glow(2px #fff) }");
    checkWarnsAndStatic(
        "blur(4px) shadow(2px 2px 2px #000)", 2,
        "from { effect: blur(2px) shadow(2px 2px 2px #000) } to { effect: shadow(2px 2px 4px #000) "
        "blur(4px) }");
    // One incompatible pair among compatible ones: the whole track does not animate.
    checkWarnsAndStatic(
        "blur(4px) shadow(2px 2px 2px #000)", 2,
        "0% { effect: none } 40% { effect: blur(2px) } 70% { effect: blur(4px) shadow(2px 2px 2px "
        "#000) } 100% { effect: glow(3px #fff) }");
    // Each adjacent pair is a prefix, but the two longer ones diverge: no common padding exists.
    checkWarnsAndStatic(
        "blur(4px) shadow(2px 2px 2px #000)", 2,
        "0% { effect: blur(2px) shadow(2px 2px 2px #000) } 50% { effect: blur(3px) } 100% { effect: "
        "blur(4px) glow(3px #fff) }");
}

TEST_CASE("T-872: effects without neutral parameters are not padded") {
    checkWarnsAndStatic("backdrop-blur(8px)", 1, "from { effect: none } to { effect: backdrop-blur(8px) }");
    checkWarnsAndStatic("backdrop-blur(4px) blur(2px)", 2,
                        "from { effect: backdrop-blur(4px) } to { effect: backdrop-blur(4px) blur(2px) }");
    checkWarnsAndStatic(
        "blur(2px) gradient-mask(linear, 0px 0px, 100px 0px, #000 0%, #0000 100%)", 2,
        "from { effect: blur(2px) } to { effect: blur(2px) gradient-mask(linear, 0px 0px, 100px 0px, "
        "#000 0%, #0000 100%) }");
}
