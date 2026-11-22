#include <cmath>
#include <memory>
#include <string>
#include <string_view>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-872 (Docs/design/04-timeline.md §4.5, CSS Animations 1): `animation-direction` x
/// `animation-iteration-count` x `animation-fill-mode` of a CSS animation, sampled against the
/// values CSS gives -- every iteration's 25% / 50% / 75%, the delay and after the end.

using namespace xt;
using namespace xt::compile;

namespace {

struct Loaded {
    NullTextShaper shaper;
    std::unique_ptr<FileAssetResolver> assets;
    std::unique_ptr<Document> doc;
};

void load(std::string_view source, Loaded& out) {
    SourceFile files[1] = {SourceFile{"direction.xt", source}};
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "direction.xt", options);
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

/// `#b` with opacity 0.5 (the base value) and `animation: a 1s linear <delay> <rest>` over
/// `@keyframes a { from { opacity: 0 } to { opacity: 1 } }`: the opacity is the directed progress.
std::string page(std::string_view animationRest, float delay) {
    std::string s =
        "<xt>\n  <head>\n    <style>\n      #b { position: absolute; left: 0px; top: 0px; width: 100px; "
        "height: 40px; opacity: 0.5; animation: a 1s linear ";
    s += std::to_string(delay) + "s ";
    s += animationRest;
    s += "; }\n      @keyframes a { from { opacity: 0 } to { opacity: 1 } }\n    </style>\n  </head>\n"
         "  <body><div id=\"b\"></div></body>\n</xt>\n";
    return s;
}

float opacityAt(Loaded& l, float clock) {
    l.doc->update(clock);
    return l.doc->byId("b").property(prop::Opacity).f;
}

/// CSS Animations 1's directed progress of iteration `iter` at iteration progress `frac`.
float directed(const std::string& direction, unsigned iter, float frac) {
    const bool odd = (iter % 2u) == 1u;
    if (direction == "reverse") {
        return 1.0f - frac;
    }
    if (direction == "alternate") {
        return odd ? 1.0f - frac : frac;
    }
    if (direction == "alternate-reverse") {
        return odd ? frac : 1.0f - frac;
    }
    return frac;
}

}  // namespace

TEST_CASE("T-872: direction x finite iteration count x fill matches CSS") {
    const char* directions[] = {"normal", "reverse", "alternate", "alternate-reverse"};
    const char* fills[] = {"none", "forwards", "backwards", "both"};
    const unsigned counts[] = {1, 2, 3};
    const float delay = 0.5f;
    for (const char* direction : directions) {
        for (unsigned count : counts) {
            for (const char* fill : fills) {
                const std::string dir = direction;
                const std::string fl = fill;
                CAPTURE(dir);
                CAPTURE(count);
                CAPTURE(fl);
                const bool backwards = fl == "backwards" || fl == "both";
                const bool forwards = fl == "forwards" || fl == "both";
                Loaded l;
                load(page(dir + " " + std::to_string(count) + " " + fl, delay), l);

                // The delay: iteration 0's start under a backwards fill, else the base value.
                CHECK(opacityAt(l, 0.25f) == doctest::Approx(backwards ? directed(dir, 0, 0.0f) : 0.5f));
                for (unsigned iter = 0; iter < count; ++iter) {
                    const float fracs[] = {0.25f, 0.5f, 0.75f};
                    for (float frac : fracs) {
                        CAPTURE(iter);
                        CAPTURE(frac);
                        const float clock = delay + static_cast<float>(iter) + frac;
                        CHECK(opacityAt(l, clock) == doctest::Approx(directed(dir, iter, frac)));
                    }
                }
                // After the end: the last iteration's end under a forwards fill, else the base value.
                const float end = delay + static_cast<float>(count);
                const float last = directed(dir, count - 1, 1.0f);
                CHECK(opacityAt(l, end + 0.25f) == doctest::Approx(forwards ? last : 0.5f));
                CHECK(opacityAt(l, end + 2.0f) == doctest::Approx(forwards ? last : 0.5f));
            }
        }
    }
}

TEST_CASE("T-872: the end value of a finished animation (reverse 1 = 0%, alternate 2 = 0%)") {
    {
        Loaded l;
        load(page("reverse 1 forwards", 0.0f), l);
        CHECK(opacityAt(l, 1.5f) == doctest::Approx(0.0f));
    }
    {
        Loaded l;
        load(page("alternate 2 forwards", 0.0f), l);
        CHECK(opacityAt(l, 1.5f) == doctest::Approx(0.5f));  // The second iteration, running back.
        CHECK(opacityAt(l, 2.5f) == doctest::Approx(0.0f));
    }
}

TEST_CASE("T-872: an infinite count under every direction and fill") {
    const char* directions[] = {"normal", "reverse", "alternate", "alternate-reverse"};
    const char* fills[] = {"none", "forwards", "backwards", "both"};
    const float delay = 0.5f;
    for (const char* direction : directions) {
        for (const char* fill : fills) {
            const std::string dir = direction;
            const std::string fl = fill;
            CAPTURE(dir);
            CAPTURE(fl);
            const bool backwards = fl == "backwards" || fl == "both";
            Loaded l;
            load(page(dir + " infinite " + fl, delay), l);
            CHECK(opacityAt(l, 0.25f) == doctest::Approx(backwards ? directed(dir, 0, 0.0f) : 0.5f));
            for (unsigned iter = 0; iter < 4; ++iter) {
                const float fracs[] = {0.25f, 0.5f, 0.75f};
                for (float frac : fracs) {
                    CAPTURE(iter);
                    CAPTURE(frac);
                    const float clock = delay + static_cast<float>(iter) + frac;
                    CHECK(opacityAt(l, clock) == doctest::Approx(directed(dir, iter, frac)));
                }
            }
        }
    }
}

TEST_CASE("T-872: a fractional iteration count truncates (2.5 plays two iterations)") {
    // The compiler truncates a fractional count (css_values parseAnimIterationTok); CSS would play
    // half a third iteration. Recorded as is, not changed by T-872.
    Loaded l;
    load(page("alternate 2.5 forwards", 0.0f), l);
    CHECK(opacityAt(l, 1.5f) == doctest::Approx(0.5f));
    CHECK(opacityAt(l, 2.25f) == doctest::Approx(0.0f));
}
