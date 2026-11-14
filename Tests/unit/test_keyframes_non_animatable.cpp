#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/text_shaper.h"

/// T-705 (Docs/tasks/T-705.md; Docs/design/05-style-and-css.md §5.4, 02-ir-format.md TLIN): a
/// property that is not animatable, written in an `@keyframes` stop or an `@timeline` key, is
/// diagnosed at compile time and its track is left out, so the document still loads and the
/// animatable tracks next to it still play (like `transition`'s "unknown or non-animatable"
/// warning and `order`'s in `@keyframes`, T-336).

using namespace xt;

namespace {

compile::CompileResult compileRaw(const std::string& css, const std::string& body) {
    // The style text starts on line 1 of the file, so a declaration on css line N is on line N.
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"non_animatable.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "non_animatable.xt", compile::CompileOptions{});
}

/// One non-animatable declaration of each value kind the sheet names.
struct PropCase {
    const char* name;  // the CSS property name the diagnostic must quote
    const char* decl;  // the declaration written in the keyframe / key
};

const PropCase kProps[] = {
    {"world-layer", "world-layer: on"},  // Enum StyleProp 281 (T-704's finding)
    {"display", "display: none"},        // Enum StyleProp 160
    {"grid-area", "grid-area: main"},    // String StyleProp 272
    {"hit-slop", "hit-slop: 4px"},       // Length StyleProps 277-280 (from a shorthand)
    {"z-index", "z-index: 3"},           // Float StyleProp
};

/// Where the declaration sits.
enum class Where { Keyframes, Timeline, RuntimeAnimation };

constexpr const char* kBox = "position:absolute;left:0px;top:0px;width:100px;height:50px;";

/// The stylesheet: line 1 the element rule, line 2 opens the block, line 3 holds `decl` next to an
/// animatable `opacity`, line 4 the last stop / key.
std::string cssFor(Where where, const char* decl) {
    switch (where) {
        case Where::Keyframes:
            return std::string("#a{") + kBox + "animation: flip 1s linear forwards}\n" +
                   "@keyframes flip{\n" + "from{opacity: 0; " + decl + "}\n" + "to{opacity: 1}}";
        case Where::Timeline:
            return std::string("#a{") + kBox + "}\n" + "@timeline flip{#a{\n" + "0s{opacity: 0; " + decl +
                   "}\n" + "1s{opacity: 1}}}";
        case Where::RuntimeAnimation:
            return std::string("#a{") + kBox + "}\n" + "@keyframes flip{\n" + "from{opacity: 0; " + decl +
                   "}\n" + "to{opacity: 1}}";
    }
    return {};
}

const char* whereName(Where where) {
    switch (where) {
        case Where::Keyframes:
            return "@keyframes in a rule's animation";
        case Where::Timeline:
            return "@timeline";
        case Where::RuntimeAnimation:
            return "@keyframes played by setStyle(animation)";
    }
    return "";
}

}  // namespace

TEST_CASE("T-705: a non-animatable property in a keyframe or timeline key warns and still loads") {
    for (Where where : {Where::Keyframes, Where::Timeline, Where::RuntimeAnimation}) {
        for (const PropCase& p : kProps) {
            const std::string context = std::string(whereName(where)) + " / " + p.decl;
            CAPTURE(context);
            compile::CompileResult result = compileRaw(cssFor(where, p.decl), R"(<div id="a"></div>)");
            int matching = 0;
            for (const compile::Diagnostic& d : result.diagnostics) {
                CAPTURE(d.message);
                CHECK(d.level != compile::Diagnostic::Level::Error);
                const std::string quoted = std::string("\"") + p.name + "\" is not animatable";
                if (d.message.find(quoted) != std::string::npos) {
                    ++matching;
                    CHECK(d.level == compile::Diagnostic::Level::Warning);
                    CHECK(d.line == 3);
                    CHECK(d.file == "non_animatable.xt");
                    CHECK(d.message.find(where == Where::Timeline
                                             ? "@timeline \"flip\""
                                             : "@keyframes \"flip\"") != std::string::npos);
                    if (where == Where::Timeline) {
                        CHECK(d.message.find("#a") != std::string::npos);
                    }
                }
            }
            CHECK(matching == 1);
            CHECK(result.ok());
            if (!result.ok()) {
                continue;
            }

            const IrBlob blob{result.ir.data(), result.ir.size()};
            const std::string why = IrView::openError(blob);
            CAPTURE(why);
            CHECK(why.empty());
            if (!why.empty()) {
                continue;
            }

            NullTextShaper shaper;
            Context ctx{ContextConfig{&shaper, nullptr}};
            Result<DocumentHandle> handle = ctx.loadDocument(blob, false);
            REQUIRE(handle.ok());
            Document* doc = ctx.document(handle.value());
            REQUIRE(doc != nullptr);
            ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
            ctx.update(0.0f);
            if (where == Where::Timeline) {
                REQUIRE(doc->playTimeline("", "flip") == Status::kOk);
            } else if (where == Where::RuntimeAnimation) {
                REQUIRE(doc->byId("a").setStyle("animation: flip 1s linear forwards") == Status::kOk);
            }
            ctx.update(0.0f);
            ctx.update(0.5f);
            // The animatable track next to the dropped one plays.
            ElementRef a = doc->byId("a");
            REQUIRE(a.valid());
            CHECK(doc->arena().common(a.node).opacity == doctest::Approx(0.5f));
        }
    }
}

TEST_CASE("T-705: animatable and specially handled properties in keyframes do not get the warning") {
    // visibility drives the Visible flag (T-229); effect expands per parameter (18.4); transform
    // components and colors are ordinary tracks.
    compile::CompileResult result = compileRaw(
        std::string("#a{") + kBox +
            "animation: k 1s linear}\n"
            "@keyframes k{from{visibility: hidden; transform: translateX(0px); background-color: red;"
            " effect: blur(0px)} to{visibility: visible; transform: translateX(10px); background-color: blue;"
            " effect: blur(4px)}}\n"
            "@timeline t{#a{0s{visibility: hidden; opacity: 0} 1s{visibility: visible; opacity: 1}}}",
        R"(<div id="a"></div>)");
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.message.find("is not animatable") == std::string::npos);
    }
    REQUIRE(result.ok());
    CHECK(IrView::openError(IrBlob{result.ir.data(), result.ir.size()}).empty());
}

TEST_CASE("T-705: a keyframes block holding only non-animatable properties loads as an empty timeline") {
    compile::CompileResult result =
        compileRaw(std::string("#a{") + kBox +
                       "animation: k 1s}\n@keyframes k{from{world-layer: off}to{world-layer: on}}",
                   R"(<div id="a"></div>)");
    REQUIRE(result.ok());
    int warnings = 0;
    for (const compile::Diagnostic& d : result.diagnostics) {
        if (d.message.find("\"world-layer\" is not animatable") != std::string::npos) {
            ++warnings;
        }
    }
    CHECK(warnings == 1);  // One per track, not one per stop.
    CHECK(IrView::openError(IrBlob{result.ir.data(), result.ir.size()}).empty());
}

TEST_CASE("T-705: IrView::openError names a track's unknown property by its number") {
    compile::CompileResult r = compileRaw(
        std::string("#a{") + kBox + "animation: k 1s}\n@keyframes k{from{opacity: 0}to{opacity: 1}}",
        R"(<div id="a"></div>)");
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    // The opacity track retargeted at StyleProp 281 (`world-layer`), which no track can animate --
    // what the compiler wrote before T-705.
    std::vector<uint8_t> bad = r.ir;
    bool edited = false;
    for (uint32_t i = 0; i < view.value().timelineCount(); ++i) {
        for (const IrTrack& t : view.value().timeline(i).tracks()) {
            if (t.prop == prop::Opacity) {
                const size_t at = static_cast<size_t>(reinterpret_cast<const uint8_t*>(&t) - r.ir.data());
                IrTrack track = t;
                track.prop = 281;
                std::memcpy(bad.data() + at, &track, sizeof track);
                edited = true;
            }
        }
    }
    REQUIRE(edited);
    CHECK_FALSE(IrView::open(IrBlob{bad.data(), bad.size()}).ok());
    const std::string why = IrView::openError(IrBlob{bad.data(), bad.size()});
    CAPTURE(why);
    CHECK(why.find("TLIN") != std::string::npos);
    CHECK(why.find("(prop 281)") != std::string::npos);
    CHECK(why.find("(?)") == std::string::npos);
}
