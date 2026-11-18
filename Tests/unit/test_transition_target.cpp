#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-723 (Docs/design/05-style-and-css.md §5.6a): a transition starts, goes on or retargets by the
/// property's target value (the last cascade result), never by the value it is animating through.
///   1. A re-resolve that leaves the target unchanged does not restart a running transition.
///   2. Hover entered and left again at t = 0: the element heads back to its own value at once.
///   3. A third target retargets from the current animated value, with the full duration.

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

std::unique_ptr<Document> loadFromString(const std::string& name, const std::string& css,
                                         const std::string& body, TextShaper& shaper) {
    const fs::path dir = xt::test::tempDir() / "xt_t723_transition_target";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / (name + ".xt");
    const std::string source =
        "<xt>\n<head>\n<style>\n" + css + "\n</style>\n</head>\n<body>\n" + body + "\n</body>\n</xt>\n";
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    REQUIRE(file != nullptr);
    std::fwrite(source.data(), 1, source.size(), file);
    std::fclose(file);

    CompileResult result = compileDocument(path.string(), CompileOptions{});
    REQUIRE(result.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, &shaper, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

constexpr const char* kCss =
    "#el { position: absolute; left: 0px; top: 0px; width: 100px; height: 100px;"
    "      background-color: #ffffff; transition: background-color 1s linear; }"
    "#el.on { background-color: #000000; }"
    "#el.third { background-color: #cccccc; }"
    "#el:hover { background-color: #000000; }";

/// The red channel of the element's background color (the three CSS colors above are grays):
/// background-color is a property whose animated value lands in ComputedStyle::base.
float red(const ElementRef& el) { return el.property(prop::BackgroundColor).c.r; }

InputEvent pointerAt(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    return ev;
}

}  // namespace

TEST_CASE("transition target (05 5.6a): a re-resolve with an unchanged target keeps the progress") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadFromString("unchanged", kCss, "<div id=\"el\"></div>", shaper);
    ElementRef el = doc->byId("el");
    REQUIRE(el.valid());
    el.addClass("on");
    doc->update(
        1.0f);  // white -> black (red 1 -> 0) over 1 s, starting now (Document::update takes the clock).
    doc->update(1.5f);
    CHECK(red(el) == doctest::Approx(0.5f).epsilon(0.001));
    // A class no rule names: the element (and its subtree) re-resolves, the target stays 0.
    el.addClass("unrelated");
    doc->update(1.75f);
    // Progress 0.75 of the original transition, not a fresh 1 s one from 0.5 (0.375).
    CHECK(red(el) == doctest::Approx(0.25f).epsilon(0.001));
    doc->update(2.0f);
    CHECK(red(el) == doctest::Approx(0.0f));
    CHECK(doc->animator().transitionCount() == 0);
}

TEST_CASE("transition target (05 5.6a): hover entered and left at t = 0 heads back to the own value") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadFromString("hover_t0", kCss, "<div id=\"el\"></div>", shaper);
    ElementRef el = doc->byId("el");
    REQUIRE(el.valid());
    doc->dispatchInput(pointerAt(Vec2{50, 50}));
    REQUIRE(el.hasState(kStateHover));
    doc->update(1.0f);  // white -> black starts; at t = 0 the animated value is still 1.
    CHECK(red(el) == doctest::Approx(1.0f));
    doc->dispatchInput(pointerAt(Vec2{300, 250}));
    REQUIRE(!el.hasState(kStateHover));
    // The new target (white, red 1) equals the current animated value: the transition ends there.
    doc->update(1.0f);
    doc->update(1.5f);
    CHECK(red(el) == doctest::Approx(1.0f));
    CHECK(doc->animator().transitionCount() == 0);
}

TEST_CASE("transition target (05 5.6a): a third target retargets from the current animated value") {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc = loadFromString("retarget", kCss, "<div id=\"el\"></div>", shaper);
    ElementRef el = doc->byId("el");
    REQUIRE(el.valid());
    el.addClass("on");
    doc->update(1.0f);
    doc->update(1.5f);
    CHECK(red(el) == doctest::Approx(0.5f).epsilon(0.001));
    el.addClass("third");  // Declared after `.on`, same specificity: the target is now #cccccc (red 0.8).
    doc->update(1.5f);     // 0.5 -> 0.8 over the full 1 s from here.
    CHECK(red(el) == doctest::Approx(0.5f).epsilon(0.001));
    doc->update(2.0f);
    CHECK(red(el) == doctest::Approx(0.65f).epsilon(0.001));
    doc->update(2.5f);
    CHECK(red(el) == doctest::Approx(0.8f).epsilon(0.001));
}
