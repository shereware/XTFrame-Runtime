#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/animator.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-176: viewport / safe-area / uiscale units, calc(), @media and Context::setDevice(). See
/// Docs/design/05-style-and-css.md §5.9.2-5.9.3, 06-layout.md §6.4, 12-compiler.md §12.7,
/// 13-api.md §13.8.2, Docs/tasks/T-176.md "測試".

using namespace xt;

namespace {

compile::CompileResult compileRaw(const std::string& css, const std::string& body) {
    const std::string text = "<xt><head><style>" + css + "</style></head><body>" + body + "</body></xt>";
    const compile::SourceFile file{"adaptive.xt", text};
    return compile::compileDocumentFromStrings({&file, 1}, "adaptive.xt", compile::CompileOptions{});
}

std::vector<uint8_t> compileDoc(const std::string& css, const std::string& body) {
    compile::CompileResult result = compileRaw(css, body);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

bool hasDiagnostic(const compile::CompileResult& r, const std::string& needle, bool error) {
    return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const compile::Diagnostic& d) {
        return d.message.find(needle) != std::string::npos &&
               (d.level == compile::Diagnostic::Level::Error) == error;
    });
}

/// One Context + one loaded Document laid out in a `w` x `h` viewport.
struct Loaded {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Document* doc = nullptr;

    Loaded(const std::vector<uint8_t>& ir, float w, float h, const DeviceInfo& device = DeviceInfo{}) {
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
        REQUIRE(handle.ok());
        doc = ctx.document(handle.value());
        REQUIRE(doc != nullptr);
        ctx.setViewport(Rect{0, 0, w, h}, 1.0f);
        REQUIRE(ctx.setDevice(device) == Status::kOk);
        ctx.update(0.0f);
    }

    NodeId node(const char* id) {
        ElementRef ref = doc->byId(id);
        REQUIRE(ref.valid());
        return ref.node;
    }
    const LayoutBox& box(const char* id) { return doc->arena().element(node(id)).box; }
    float width(const char* id) { return box(id).width; }
    float height(const char* id) { return box(id).height; }
    float fontSize(const char* id) {
        return doc->arena().element(node(id)).style.base[prop::FontSize].len.value;
    }
    void resize(float w, float h) {
        ctx.setViewport(Rect{0, 0, w, h}, 1.0f);
        ctx.update(0.0f);
    }
    void device(const DeviceInfo& d) {
        REQUIRE(ctx.setDevice(d) == Status::kOk);
        ctx.update(0.0f);
    }
};

const char* kAbs = "position:absolute;left:0;top:0;";

}  // namespace

// -------------------------------------------------------------------------------------------
// Units
// -------------------------------------------------------------------------------------------

TEST_CASE("T-176: vw/vh/vmin/vmax resolve against the viewport and follow a resize") {
    const std::string css =
        std::string("#a{") + kAbs + "width:50vw;height:10vh} #b{" + kAbs + "width:10vmin;height:10vmax}";
    Loaded l(compileDoc(css, R"(<div id="a"></div><div id="b"></div>)"), 400, 200);
    CHECK(l.width("a") == doctest::Approx(200));
    CHECK(l.height("a") == doctest::Approx(20));
    CHECK(l.width("b") == doctest::Approx(20));
    CHECK(l.height("b") == doctest::Approx(40));
    l.resize(100, 300);
    CHECK(l.width("a") == doctest::Approx(50));
    CHECK(l.height("a") == doctest::Approx(30));
    CHECK(l.width("b") == doctest::Approx(10));
    CHECK(l.height("b") == doctest::Approx(30));
}

TEST_CASE("T-176: safe-* units are the safe-area insets (fractions of the viewport), with factors") {
    const std::string css =
        "#c{position:absolute;left:1safe-left;bottom:1safe-bottom;width:2safe-top;"
        "height:1.5safe-right}";
    DeviceInfo device;
    device.safeArea = Insets{0.1f, 0.05f, 0.1f, 0.05f};
    Loaded l(compileDoc(css, R"(<div id="c"></div>)"), 400, 200, device);
    // top/bottom = 0.1 x 200 = 20, left/right = 0.05 x 400 = 20.
    CHECK(l.box("c").x == doctest::Approx(20));
    CHECK(l.width("c") == doctest::Approx(40));
    CHECK(l.height("c") == doctest::Approx(30));
    CHECK(l.box("c").y == doctest::Approx(200 - 20 - 30));
    // A new safe area relayouts without recompiling.
    device.safeArea = Insets{0, 0.1f, 0, 0.1f};
    l.device(device);
    CHECK(l.box("c").x == doctest::Approx(40));
    CHECK(l.width("c") == doctest::Approx(0));
    CHECK(l.height("c") == doctest::Approx(60));
}

TEST_CASE("T-176: uiscale follows Context::setUiScale(), and font-size in device units restyles") {
    const std::string css = std::string("#d{") + kAbs + "width:10uiscale;height:1uiscale} #e{" + kAbs +
                            "font-size:5vh} #f{" + kAbs + "font-size:2uiscale}";
    Loaded l(compileDoc(css, R"(<div id="d"></div><div id="e"></div><div id="f"></div>)"), 400, 200);
    CHECK(l.width("d") == doctest::Approx(10));
    CHECK(l.fontSize("e") == doctest::Approx(10));
    CHECK(l.fontSize("f") == doctest::Approx(2));
    REQUIRE(l.ctx.setUiScale(2.5f) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.ctx.uiScale() == doctest::Approx(2.5f));
    CHECK(l.width("d") == doctest::Approx(25));
    CHECK(l.height("d") == doctest::Approx(2.5f));
    CHECK(l.fontSize("f") == doctest::Approx(5));
    l.resize(400, 400);
    CHECK(l.fontSize("e") == doctest::Approx(20));
    CHECK(l.ctx.setUiScale(0) == Status::kInvalidArgument);
    CHECK(l.ctx.setUiScale(-1) == Status::kInvalidArgument);
    CHECK(l.ctx.uiScale() == doctest::Approx(2.5f));
}

TEST_CASE("T-176: lengthToPxEnv converts every unit") {
    LengthEnv env;
    env.viewportW = 800;
    env.viewportH = 400;
    env.safePx = Insets{10, 20, 30, 40};
    env.uiScale = 1.5f;
    float px = 0;
    struct Case {
        LengthUnit unit;
        float value;
        float want;
    } cases[] = {
        {LengthUnit::Px, 7, 7},           {LengthUnit::Percent, 50, 50},  {LengthUnit::Vw, 10, 80},
        {LengthUnit::Vh, 10, 40},         {LengthUnit::Vmin, 10, 40},     {LengthUnit::Vmax, 10, 80},
        {LengthUnit::SafeTop, 2, 20},     {LengthUnit::SafeRight, 1, 20}, {LengthUnit::SafeBottom, 1, 30},
        {LengthUnit::SafeLeft, 0.5f, 20}, {LengthUnit::UiScale, 4, 6},
    };
    for (const Case& c : cases) {
        CAPTURE(static_cast<int>(c.unit));
        REQUIRE(lengthToPxEnv(env, Length{c.value, c.unit}, 100, px));
        CHECK(px == doctest::Approx(c.want));
    }
    CHECK_FALSE(lengthToPxEnv(env, Length{50, LengthUnit::Percent}, kUndefinedRef, px));
    CHECK_FALSE(lengthToPxEnv(env, Length{0, LengthUnit::Auto}, 100, px));
    CHECK_FALSE(lengthToPxEnv(env, Length{1, LengthUnit::Em}, 100, px));
    CHECK_FALSE(lengthToPxEnv(env, Length{0, LengthUnit::Calc}, 100, px));  // No program 0.
}

TEST_CASE("T-176: a length token (--x: 10vw) and a style attribute accept the new units") {
    const std::string css = std::string(":root{--w:25vw} #a{") + kAbs + "width:var(--w)}";
    Loaded l(compileDoc(css, R"(<div id="a"></div><div id="b" style="position:absolute;width:1vmax"></div>)"),
             400, 200);
    CHECK(l.width("a") == doctest::Approx(100));
    CHECK(l.width("b") == doctest::Approx(4));
}

// -------------------------------------------------------------------------------------------
// calc()
// -------------------------------------------------------------------------------------------

TEST_CASE("T-176: calc() precedence, parentheses and mixed units") {
    const std::string css = std::string("#p{") + kAbs + "width:400px;height:100px;font-size:16px}" +
                            "#a{position:absolute;width:calc(10px + 2 * 5px)}"
                            "#b{position:absolute;width:calc((10px + 2px) * 2)}"
                            "#c{position:absolute;width:calc(100% - 40px)}"
                            "#d{position:absolute;width:calc(50vw + 2uiscale)}"
                            "#e{position:absolute;width:calc(100% / 4 - 1em)}"
                            "#f{position:absolute;width:calc(20px - 4px - 6px)}"
                            "#g{position:absolute;width:calc(60px / 2 / 3)}"
                            "#h{position:absolute;width:calc(1safe-left + 10%)}";
    const std::string body =
        R"(<div id="p"><div id="a"></div><div id="b"></div><div id="c"></div><div id="d"></div>)"
        R"(<div id="e"></div><div id="f"></div><div id="g"></div><div id="h"></div></div>)";
    DeviceInfo device;
    device.safeArea = Insets{0, 0, 0, 0.05f};  // 0.05 x 1000 = 50.
    Loaded l(compileDoc(css, body), 1000, 500, device);
    CHECK(l.width("a") == doctest::Approx(20));
    CHECK(l.width("b") == doctest::Approx(24));
    CHECK(l.width("c") == doctest::Approx(360));  // % of the parent's 400px padding box.
    CHECK(l.width("d") == doctest::Approx(502));
    CHECK(l.width("e") == doctest::Approx(84));  // 400 / 4 - 16.
    CHECK(l.width("f") == doctest::Approx(10));  // Left-associative.
    CHECK(l.width("g") == doctest::Approx(10));
    CHECK(l.width("h") == doctest::Approx(90));
    // Device units inside calc() follow a resize and a uiscale change. T-345 (05 §5.9.9): the
    // parent's 400px width follows uiscale too (1200px), so h's 10% is 120.
    REQUIRE(l.ctx.setUiScale(3) == Status::kOk);
    l.resize(600, 500);
    CHECK(l.width("d") == doctest::Approx(306));
    CHECK(l.width("h") == doctest::Approx(30 + 120));
}

TEST_CASE("T-176: calc() in height against an indefinite height behaves as auto") {
    const std::string css =
        "#p{position:absolute;width:100px} #a{height:calc(50% + 10px)} #b{height:calc(2vh + 1px)}";
    Loaded l(compileDoc(css, R"(<div id="p"><div id="a"></div><div id="b"></div></div>)"), 400, 200);
    CHECK(l.height("a") == doctest::Approx(0));
    CHECK(l.height("b") == doctest::Approx(5));
}

TEST_CASE("T-176: var() inside calc() is looked up at resolve time, with a fallback") {
    const std::string css = std::string(":root{--gap:8px;--n:3} #a{") + kAbs +
                            "width:calc(100px - var(--gap) * 2)} #b{" + kAbs +
                            "width:calc(var(--missing, 4px) * var(--n))}";
    compile::CompileResult r = compileRaw(css, R"(<div id="a"></div><div id="b"></div>)");
    REQUIRE(r.ok());
    Loaded l(r.ir, 400, 200);
    CHECK(l.width("a") == doctest::Approx(84));
    CHECK(l.width("b") == doctest::Approx(12));
    // A theme/token change re-resolves without recompiling.
    REQUIRE(l.ctx.setToken("--gap", Value::ofLength(10, LengthUnit::Px)) == Status::kOk);
    REQUIRE(l.ctx.setToken("--n", Value::ofFloat(5)) == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == doctest::Approx(80));
    CHECK(l.width("b") == doctest::Approx(20));
}

TEST_CASE("T-176: an undefined var() without a fallback inside calc() warns and drops the declaration") {
    const std::string css = std::string("#a{") + kAbs + "width:30px} #a{width:calc(var(--nope) + 1px)}";
    compile::CompileResult r = compileRaw(css, R"(<div id="a"></div>)");
    REQUIRE(r.ok());
    CHECK(hasDiagnostic(r, "var(--nope) inside calc() is undefined", false));
    Loaded l(r.ir, 400, 200);
    CHECK(l.width("a") == doctest::Approx(30));
}

TEST_CASE("T-176: calc() in the IR: CALC chunk, kDeclCalc declarations, byte-exact round trip") {
    std::vector<uint8_t> ir = compileDoc("#a{width:calc(100% - 2 * var(--g, 3px))}", R"(<div id="a"></div>)");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    const CalcChunk& calcs = view.value().calcs();
    REQUIRE(calcs.present());
    REQUIRE(calcs.calcs().size() == 1);
    REQUIRE(calcs.calcs()[0].opCount == 5);  // 100% 2 var * -
    const IrCalcOp* ops = calcs.ops().data();
    CHECK(ops[0].op == static_cast<uint8_t>(CalcOpKind::PushLength));
    CHECK(ops[0].unit == static_cast<uint8_t>(LengthUnit::Percent));
    CHECK(ops[1].op == static_cast<uint8_t>(CalcOpKind::PushNumber));
    CHECK(ops[2].op == static_cast<uint8_t>(CalcOpKind::PushVar));
    CHECK(ops[2].hasFallback == 1);
    CHECK(ops[2].value == doctest::Approx(3));
    CHECK(view.value().string(ops[2].name) == "--g");
    CHECK(ops[3].op == static_cast<uint8_t>(CalcOpKind::Mul));
    CHECK(ops[4].op == static_cast<uint8_t>(CalcOpKind::Sub));
    bool found = false;
    for (const IrDecl& d : view.value().style().decls()) {
        found = found || (d.valueType == kDeclCalc && d.prop == prop::Width && irValueU32(d.value, 0) == 0);
    }
    CHECK(found);
    CHECK(compile::IrWriter::fromView(view.value()).finish() == ir);
}

TEST_CASE("T-176: malformed calc() is rejected with a reason; calc() in @keyframes is an error") {
    struct Case {
        const char* value;
        const char* reason;
    } cases[] = {
        {"calc(10px + )", "ends with an operator"},   {"calc(10px * 10px)", "cannot multiply two lengths"},
        {"calc(10px + 2)", "cannot add or subtract"}, {"calc(10px / 2px)", "cannot divide by a length"},
        {"calc(10px / 0)", "divides by zero"},        {"calc(10px+5px)", "expected an operator"},
        {"calc(2 * 3)", "must produce a length"},     {"calc((10px + 1px)", "unbalanced"},
    };
    for (const Case& c : cases) {
        const std::string value = c.value;
        CAPTURE(value);
        compile::CompileResult r =
            compileRaw(std::string("#a{width:") + c.value + "}", R"(<div id="a"></div>)");
        CHECK(hasDiagnostic(r, c.reason, false));
    }
    compile::CompileResult r = compileRaw("#a{opacity:calc(1px + 1px)}", R"(<div id="a"></div>)");
    CHECK(hasDiagnostic(r, "only supported on length properties", false));
    r = compileRaw("@keyframes k{from{width:calc(1px + 1px)}to{width:2px}} #a{animation:k 1s}",
                   R"(<div id="a"></div>)");
    CHECK(hasDiagnostic(r, "calc() is not supported here", true));
}

// -------------------------------------------------------------------------------------------
// @media
// -------------------------------------------------------------------------------------------

namespace {

/// `#a` is 1px wide, 10px when the `@media` prelude `query` holds.
float mediaWidth(const std::string& query, float w, float h, const DeviceInfo& device = DeviceInfo{}) {
    const std::string css = std::string("#a{") + kAbs + "width:1px} @media " + query + " { #a{width:10px} }";
    Loaded l(compileDoc(css, R"(<div id="a"></div>)"), w, h, device);
    return l.width("a");
}

}  // namespace

TEST_CASE("T-176: every @media condition") {
    CHECK(mediaWidth("(min-width: 300px)", 400, 200) == 10);
    CHECK(mediaWidth("(min-width: 300px)", 200, 200) == 1);
    CHECK(mediaWidth("(max-width: 300px)", 200, 200) == 10);
    CHECK(mediaWidth("(max-width: 300px)", 400, 200) == 1);
    CHECK(mediaWidth("(min-height: 300px)", 200, 400) == 10);
    CHECK(mediaWidth("(min-height: 300px)", 200, 200) == 1);
    CHECK(mediaWidth("(max-height: 300px)", 200, 200) == 10);
    CHECK(mediaWidth("(max-height: 300px)", 200, 400) == 1);
    CHECK(mediaWidth("(aspect-ratio >= 21/9)", 420, 180) == 10);
    CHECK(mediaWidth("(aspect-ratio >= 21/9)", 320, 180) == 1);
    CHECK(mediaWidth("(aspect-ratio <= 16/9)", 320, 180) == 10);  // Exactly 16:9 counts.
    CHECK(mediaWidth("(aspect-ratio <= 16/9)", 420, 180) == 1);
    CHECK(mediaWidth("(aspect-ratio >= 2.3)", 420, 180) == 10);
    CHECK(mediaWidth("(orientation: portrait)", 200, 400) == 10);
    CHECK(mediaWidth("(orientation: portrait)", 400, 200) == 1);
    CHECK(mediaWidth("(orientation: landscape)", 400, 200) == 10);
    DeviceInfo console;
    console.platform = DeviceKind::Console;
    console.input = InputKind::Gamepad;
    CHECK(mediaWidth("(platform: console)", 400, 200, console) == 10);
    CHECK(mediaWidth("(platform: console)", 400, 200) == 1);
    CHECK(mediaWidth("(platform: desktop)", 400, 200) == 10);
    CHECK(mediaWidth("(input: gamepad)", 400, 200, console) == 10);
    CHECK(mediaWidth("(input: touch)", 400, 200, console) == 1);
    // DeviceInfo::aspect overrides the viewport's own ratio.
    DeviceInfo wide;
    wide.aspect = 21.0f / 9.0f;
    CHECK(mediaWidth("(aspect-ratio >= 21/9)", 320, 180, wide) == 10);
    // `and` needs every condition, a comma any query.
    CHECK(mediaWidth("(min-width: 300px) and (platform: console)", 400, 200, console) == 10);
    CHECK(mediaWidth("(min-width: 300px) and (platform: console)", 400, 200) == 1);
    CHECK(mediaWidth("(platform: handheld), (min-width: 300px)", 400, 200) == 10);
    CHECK(mediaWidth("(platform: handheld), (min-width: 900px)", 400, 200) == 1);
}

TEST_CASE("T-176: switching the device re-hits @media rules and marks only their elements") {
    const std::string css = std::string("#a{") + kAbs + "width:1px} #z{" + kAbs +
                            "width:5px} @media (platform: console) { #a{width:10px} } "
                            "@media (min-width: 500px) { #a{height:7px} }";
    Loaded l(compileDoc(css, R"(<div id="a"></div><div id="z"></div>)"), 400, 200);
    CHECK(l.width("a") == 1);
    DeviceInfo console;
    console.platform = DeviceKind::Console;
    REQUIRE(l.ctx.setDevice(console) == Status::kOk);
    CHECK((l.doc->arena().dirty(l.node("a")) & kDirtyStyle) != 0);
    CHECK((l.doc->arena().dirty(l.node("z")) & kDirtyStyle) == 0);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == 10);
    CHECK(l.height("a") == 0);
    l.resize(600, 200);  // A viewport change re-evaluates too.
    CHECK(l.height("a") == 7);
    l.device(DeviceInfo{});
    CHECK(l.width("a") == 1);
    CHECK(l.width("z") == 5);
}

TEST_CASE("T-176: @media rules keep cascade order and specificity") {
    // A later non-media rule of equal specificity still wins over an earlier @media one.
    const std::string css =
        std::string("#a{") + kAbs + "width:1px} @media (min-width: 1px) { #a{width:10px} } #a{width:20px}";
    Loaded l(compileDoc(css, R"(<div id="a"></div>)"), 400, 200);
    CHECK(l.width("a") == 20);
}

TEST_CASE("T-176: MDIA chunk layout and byte-exact round trip") {
    std::vector<uint8_t> ir = compileDoc(
        "#a{width:1px} @media (min-width: 640px) and (input: gamepad), (aspect-ratio <= 4/3) { #a{width:2px} "
        ".b{width:3px} }",
        R"(<div id="a" class="b"></div>)");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    const MediaChunk& media = view.value().media();
    REQUIRE(media.present());
    REQUIRE(media.media().size() == 1);
    REQUIRE(media.media()[0].queryCount == 2);
    REQUIRE(media.queries().size() == 2);
    CHECK(media.queries()[0].condCount == 2);
    CHECK(media.queries()[1].condCount == 1);
    REQUIRE(media.conds().size() == 3);
    CHECK(media.conds()[0].feature == static_cast<uint8_t>(MediaFeature::MinWidth));
    CHECK(media.conds()[0].value == 640);
    CHECK(media.conds()[1].feature == static_cast<uint8_t>(MediaFeature::Input));
    CHECK(media.conds()[1].value == static_cast<float>(InputKind::Gamepad));
    CHECK(media.conds()[2].feature == static_cast<uint8_t>(MediaFeature::MaxAspect));
    CHECK(media.conds()[2].value == doctest::Approx(4.0f / 3.0f));
    REQUIRE(media.rules().size() == 2);
    CHECK(media.rules()[0].rule == 1);
    CHECK(media.rules()[1].rule == 2);
    CHECK(compile::IrWriter::fromView(view.value()).finish() == ir);
    // A document without @media / calc() writes neither chunk.
    std::vector<uint8_t> plain = compileDoc("#a{width:1px}", R"(<div id="a"></div>)");
    Result<IrView> plainView = IrView::open(IrBlob{plain.data(), plain.size()}, IrReadOptions{});
    REQUIRE(plainView.ok());
    CHECK_FALSE(plainView.value().media().present());
    CHECK_FALSE(plainView.value().calcs().present());
}

TEST_CASE("T-176: unsupported @media queries warn and skip the block; misuse inside one is an error") {
    struct Case {
        const char* source;
        const char* message;
        bool error;
    } cases[] = {
        {"@media screen { #a{width:5px} }", "expected '('", false},
        {"@media (color) { #a{width:5px} }", "expected ':'", false},
        {"@media (hover: hover) { #a{width:5px} }", "unsupported @media feature", false},
        {"@media (aspect-ratio: 16/9) { #a{width:5px} }", "needs >= or <=", false},
        {"@media (min-width: 10em) { #a{width:5px} }", "takes a px length", false},
        {"@media (platform: tv) { #a{width:5px} }", "unknown value", false},
        {"@media (min-width: 1px) { @media (min-width: 2px) { #a{width:5px} } }", "cannot be nested", true},
        {"@media (min-width: 1px) { @keyframes k { from{opacity:0} } }", "not allowed inside @media", true},
        {"@media (min-width: 1px) { #a{--x: 1px} }", "cannot be declared inside @media", true},
        {"symbol(S) { @media (min-width: 1px) { #a{width:5px} } }", "not allowed inside symbol()", true},
    };
    for (const Case& c : cases) {
        const std::string source = c.source;
        CAPTURE(source);
        compile::CompileResult r =
            compileRaw(std::string("#a{position:absolute;width:1px} ") + source, R"(<div id="a"></div>)");
        CHECK(hasDiagnostic(r, c.message, c.error));
        if (!c.error) {
            // The skipped block contributes nothing; the document still compiles and loads.
            REQUIRE(r.ok());
            Loaded l(r.ir, 400, 200);
            CHECK(l.width("a") == 1);
        }
    }
}

// -------------------------------------------------------------------------------------------
// Context::setDevice
// -------------------------------------------------------------------------------------------

TEST_CASE("T-176: setDevice validates its input and is remembered for later documents") {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    DeviceInfo bad;
    bad.safeArea.top = 0.6f;
    CHECK(ctx.setDevice(bad) == Status::kInvalidArgument);
    bad = DeviceInfo{};
    bad.aspect = -1;
    CHECK(ctx.setDevice(bad) == Status::kInvalidArgument);
    bad = DeviceInfo{};
    bad.dpr = 0;
    CHECK(ctx.setDevice(bad) == Status::kInvalidArgument);
    CHECK(ctx.device().platform == DeviceKind::Desktop);

    DeviceInfo device;
    device.platform = DeviceKind::Handheld;
    device.safeArea = Insets{0.1f, 0, 0, 0};
    REQUIRE(ctx.setDevice(device) == Status::kOk);
    REQUIRE(ctx.setUiScale(2) == Status::kOk);
    ctx.setViewport(Rect{0, 0, 300, 100}, 1.0f);
    const std::string css = std::string("#a{") + kAbs +
                            "width:1px;height:1safe-top} @media (platform: handheld) { #a{width:4uiscale} }";
    std::vector<uint8_t> ir = compileDoc(css, R"(<div id="a"></div>)");
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(handle.ok());
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    doc->setViewport(Rect{0, 0, 300, 100});
    ctx.update(0.0f);
    const LayoutBox& box = doc->arena().element(doc->byId("a").node).box;
    CHECK(box.width == doctest::Approx(8));
    CHECK(box.height == doctest::Approx(10));
    CHECK(doc->device().platform == DeviceKind::Handheld);
    CHECK(doc->uiScale() == doctest::Approx(2));
}

// -------------------------------------------------------------------------------------------
// T-210: the layout contract (Docs/design/25-editor-blueprint.md §25.4.1)
// -------------------------------------------------------------------------------------------

TEST_CASE("T-210: Document::fixedRoot() finds a px-sized UI root (body's only child, then body)") {
    {
        Loaded l(
            compileDoc("body{width:1280px;height:720px} #root{position:relative;width:1280px;height:720px}",
                       R"(<div id="root"><div id="a"></div></div>)"),
            1920, 1080);
        const ElementRef fixed = l.doc->fixedRoot();
        REQUIRE(fixed.valid());
        CHECK(fixed.id() == "root");
    }
    {
        // Only the height is px: still a fixed root.
        Loaded l(compileDoc("#root{position:relative;width:100vw;height:720px}", R"(<div id="root"></div>)"),
                 1920, 1080);
        CHECK(l.doc->fixedRoot().id() == "root");
    }
    {
        // The child fills the viewport, body is px: body is reported.
        Loaded l(compileDoc("body{width:1280px} #root{position:relative;width:100vw;height:100vh}",
                            R"(<div id="root"></div>)"),
                 1920, 1080);
        const ElementRef fixed = l.doc->fixedRoot();
        REQUIRE(fixed.valid());
        CHECK(fixed.tag() == "body");
    }
    {
        // A fill-viewport root (viewport units or nothing at all) is not fixed.
        Loaded l(compileDoc("#root{position:relative;width:100vw;height:100vh}", R"(<div id="root"></div>)"),
                 1920, 1080);
        CHECK_FALSE(l.doc->fixedRoot().valid());
        Loaded bare(compileDoc("#a{width:10px}", R"(<div id="a"></div><div id="b"></div>)"), 1920, 1080);
        CHECK_FALSE(bare.doc->fixedRoot().valid());  // Two children: only body is a candidate.
    }
}

TEST_CASE("T-481: Document::fixedRoots() lists every board-sized px child of body") {
    {
        // #d is px on both axes but a 1280x3 scan line: not a root.
        Loaded l(
            compileDoc("#a{position:relative;width:1280px;height:720px} #b{position:relative;width:100vw;"
                       "height:100vh} #c{position:absolute;width:1280px;height:720px} "
                       "#d{position:absolute;width:1280px;height:3px}",
                       R"(<div id="a"></div><div id="b"></div><div id="c"></div><div id="d"></div>)"),
            1920, 1080);
        const std::vector<ElementRef> roots = l.doc->fixedRoots(Vec2{1280, 720});
        REQUIRE(roots.size() == 2);
        CHECK(roots[0].id() == "a");
        CHECK(roots[1].id() == "c");
        CHECK(l.doc->fixedRoots(Vec2{1281, 721}).size() == 2);  // 1 px tolerance.
        CHECK(l.doc->fixedRoots(Vec2{1920, 1080}).empty());     // Smaller than the board.
        CHECK(l.doc->fixedRoots().empty());                     // No board: several children never count.
        CHECK_FALSE(l.doc->fixedRoot().valid());
    }
    {
        // Several children, none with both sizes px, body px: body is the one root.
        Loaded l(compileDoc("body{width:1280px} #a{width:10px} #b{height:10px}",
                            R"(<div id="a"></div><div id="b"></div>)"),
                 1920, 1080);
        const std::vector<ElementRef> roots = l.doc->fixedRoots(Vec2{1280, 720});
        REQUIRE(roots.size() == 1);
        CHECK(roots[0].tag() == "body");
    }
    {
        Loaded l(compileDoc("#root{position:relative;width:100vw;height:100vh}", R"(<div id="root"></div>)"),
                 1920, 1080);
        CHECK(l.doc->fixedRoots().empty());
    }
}

TEST_CASE("T-210: a fill-viewport root keeps safe-area anchors on the screen edges at any aspect") {
    const std::string css =
        "#root{position:relative;width:100vw;height:100vh}"
        "#tl{position:absolute;left:calc(1safe-left + 16px);top:calc(1safe-top + "
        "26px);width:300px;height:100px}"
        "#br{position:absolute;right:calc(1safe-right + 16px);bottom:calc(1safe-bottom + 6px);width:200px;"
        "height:50px}"
        "#mid{position:absolute;left:calc(50% - 160px);top:0;width:320px;height:10px}"
        "#bg{position:absolute;left:0;right:0;top:0;bottom:0}";
    DeviceInfo device;
    device.safeArea = Insets{0.025f, 0.025f, 0.025f, 0.025f};
    Loaded l(compileDoc(css, R"(<div id="root"><div id="tl"></div><div id="br"></div><div id="mid"></div>)"
                             R"(<div id="bg"></div></div>)"),
             1280, 720, device);
    // At the design size the anchors reproduce the board's px (safe = 32 px across, 18 px down).
    CHECK(l.width("root") == doctest::Approx(1280));
    CHECK(l.height("root") == doctest::Approx(720));
    CHECK(l.box("tl").x == doctest::Approx(48));
    CHECK(l.box("tl").y == doctest::Approx(44));
    CHECK(l.box("br").x == doctest::Approx(1280 - 32 - 16 - 200));
    CHECK(l.box("br").y == doctest::Approx(720 - 18 - 6 - 50));
    CHECK(l.box("mid").x == doctest::Approx(480));
    CHECK(l.width("bg") == doctest::Approx(1280));
    // 21:9 at the same logical height: the right anchor follows the wider screen, the bleed grows.
    l.resize(1680, 720);
    CHECK(l.width("root") == doctest::Approx(1680));
    CHECK(l.box("tl").x == doctest::Approx(42 + 16));
    CHECK(l.box("br").x == doctest::Approx(1680 - 42 - 16 - 200));
    CHECK(l.box("br").y == doctest::Approx(720 - 18 - 6 - 50));
    CHECK(l.box("mid").x == doctest::Approx(680));
    CHECK(l.width("bg") == doctest::Approx(1680));
    CHECK(l.height("bg") == doctest::Approx(720));
}

// -------------------------------------------------------------------------------------------
// T-330 (05 §5.9.8): `@media (prefers-reduced-motion)` / `(resolution)`, `min()` / `max()` /
// `clamp()`.
// -------------------------------------------------------------------------------------------

TEST_CASE("T-330: @media (prefers-reduced-motion) follows DeviceInfo::reducedMotion") {
    DeviceInfo reduced;
    reduced.reducedMotion = true;
    CHECK(mediaWidth("(prefers-reduced-motion: reduce)", 400, 200, reduced) == 10);
    CHECK(mediaWidth("(prefers-reduced-motion: reduce)", 400, 200) == 1);
    CHECK(mediaWidth("(prefers-reduced-motion: no-preference)", 400, 200) == 10);
    CHECK(mediaWidth("(prefers-reduced-motion: no-preference)", 400, 200, reduced) == 1);
    compile::CompileResult r =
        compileRaw("@media (prefers-reduced-motion: always) { #a{width:1px} }", R"(<div id="a"></div>)");
    CHECK(hasDiagnostic(r, "unknown value for @media (prefers-reduced-motion)", false));
}

TEST_CASE("T-330: switching reduced motion re-hits the rule, by setDevice or the host flag") {
    const std::string css =
        std::string("#a{") + kAbs + "width:1px} @media (prefers-reduced-motion: reduce) { #a{width:10px} }";
    Loaded l(compileDoc(css, R"(<div id="a"></div>)"), 400, 200);
    CHECK(l.width("a") == 1);
    DeviceInfo reduced;
    reduced.reducedMotion = true;
    REQUIRE(l.ctx.setDevice(reduced) == Status::kOk);
    CHECK((l.doc->arena().dirty(l.node("a")) & kDirtyStyle) != 0);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == 10);
    l.device(DeviceInfo{});
    CHECK(l.width("a") == 1);
    // The host flag `system.reducedMotion` sets DeviceInfo::reducedMotion too.
    REQUIRE(l.ctx.postSystemEvent("reducedMotion", "reduce") == Status::kOk);
    CHECK(l.ctx.device().reducedMotion);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == 10);
    REQUIRE(l.ctx.postSystemEvent("reducedMotion", "no-preference") == Status::kOk);
    CHECK(!l.ctx.device().reducedMotion);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == 1);
    CHECK(l.ctx.postSystemEvent("reducedMotion", "sometimes") == Status::kInvalidArgument);
    CHECK(!l.ctx.device().reducedMotion);
}

TEST_CASE("T-330: @media (min-/max-/resolution) against DeviceInfo::dpr") {
    DeviceInfo retina;
    retina.dpr = 2;
    CHECK(mediaWidth("(min-resolution: 2dppx)", 400, 200, retina) == 10);
    CHECK(mediaWidth("(min-resolution: 2dppx)", 400, 200) == 1);
    CHECK(mediaWidth("(max-resolution: 1.5x)", 400, 200) == 10);
    CHECK(mediaWidth("(max-resolution: 1.5x)", 400, 200, retina) == 1);
    CHECK(mediaWidth("(resolution: 2x)", 400, 200, retina) == 10);
    CHECK(mediaWidth("(resolution: 2x)", 400, 200) == 1);
    CHECK(mediaWidth("(min-resolution: 192dpi)", 400, 200, retina) == 10);  // 192 / 96 = 2.
    CHECK(mediaWidth("(min-resolution: 193dpi)", 400, 200, retina) == 1);
    // Switching the DPR re-evaluates.
    const std::string css =
        std::string("#a{") + kAbs + "width:1px} @media (min-resolution: 2dppx) { #a{width:10px} }";
    Loaded l(compileDoc(css, R"(<div id="a"></div>)"), 400, 200);
    CHECK(l.width("a") == 1);
    l.device(retina);
    CHECK(l.width("a") == 10);
    compile::CompileResult r =
        compileRaw("@media (min-resolution: 2dpcm) { #a{width:1px} }", R"(<div id="a"></div>)");
    CHECK(hasDiagnostic(r, "takes a positive dppx, x or dpi value", false));
}

TEST_CASE("T-330: MDIA keeps the new features; the conditions round-trip") {
    std::vector<uint8_t> ir = compileDoc(
        "@media (prefers-reduced-motion: reduce) and (min-resolution: 96dpi) and (max-resolution: 3x), "
        "(resolution: 2dppx) { #a{width:1px} }",
        R"(<div id="a"></div>)");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    std::span<const IrMediaCond> conds = view.value().media().conds();
    REQUIRE(conds.size() == 4);
    CHECK(conds[0].feature == static_cast<uint8_t>(MediaFeature::ReducedMotion));
    CHECK(conds[0].value == 1);
    CHECK(conds[1].feature == static_cast<uint8_t>(MediaFeature::MinResolution));
    CHECK(conds[1].value == doctest::Approx(1));
    CHECK(conds[2].feature == static_cast<uint8_t>(MediaFeature::MaxResolution));
    CHECK(conds[2].value == doctest::Approx(3));
    CHECK(conds[3].feature == static_cast<uint8_t>(MediaFeature::Resolution));
    CHECK(conds[3].value == doctest::Approx(2));
    CHECK(compile::IrWriter::fromView(view.value()).finish() == ir);
}

TEST_CASE("T-330: min()/max()/clamp() resolve at layout and follow the viewport") {
    const std::string css = std::string("#p{") + kAbs +
                            "width:50vw;height:100px;font-size:10px}"
                            "#a{position:absolute;width:clamp(100px, 50%, 300px)}"
                            "#b{position:absolute;width:min(50%, 2em + 100px)}"
                            "#c{position:absolute;width:max(10vw, 120px)}"
                            "#d{position:absolute;width:calc(max(10px, 20%) + 1px)}"
                            "#e{position:absolute;width:min(var(--w, 70px), 50%)}";
    const std::string body = R"(<div id="p"><div id="a"></div><div id="b"></div><div id="c"></div>)"
                             R"(<div id="d"></div><div id="e"></div></div>)";
    Loaded l(compileDoc(css, body), 800, 400);  // #p is 400px wide.
    CHECK(l.width("a") == doctest::Approx(200));
    CHECK(l.width("b") == doctest::Approx(120));
    CHECK(l.width("c") == doctest::Approx(120));
    CHECK(l.width("d") == doctest::Approx(81));
    CHECK(l.width("e") == doctest::Approx(70));
    l.resize(1600, 400);  // #p 800px: clamp hits its maximum.
    CHECK(l.width("a") == doctest::Approx(300));
    CHECK(l.width("c") == doctest::Approx(160));
    l.resize(100, 400);  // #p 50px: clamp hits its minimum.
    CHECK(l.width("a") == doctest::Approx(100));
    CHECK(l.width("b") == doctest::Approx(25));
    CHECK(l.width("e") == doctest::Approx(25));
}

TEST_CASE("T-330: an all-px min()/max()/clamp() is a plain px declaration; others go to CALC") {
    std::vector<uint8_t> ir =
        compileDoc("#a{width:clamp(100px, 500px, 300px)} #b{width:clamp(10px, 50%, 30px)}",
                   R"(<div id="a"></div><div id="b"></div>)");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    const CalcChunk& calcs = view.value().calcs();
    REQUIRE(calcs.calcs().size() == 1);  // Only #b's.
    REQUIRE(calcs.calcs()[0].opCount == 5);
    const IrCalcOp* ops = calcs.ops().data();
    CHECK(ops[3].op == static_cast<uint8_t>(CalcOpKind::Min));
    CHECK(ops[4].op == static_cast<uint8_t>(CalcOpKind::Max));
    int px = 0;
    for (const IrDecl& d : view.value().style().decls()) {
        px += d.prop == prop::Width && d.valueType != kDeclCalc ? 1 : 0;
    }
    CHECK(px == 1);
    CHECK(compile::IrWriter::fromView(view.value()).finish() == ir);
    Loaded l(ir, 400, 200);
    CHECK(l.width("a") == doctest::Approx(300));
}

TEST_CASE("T-330: min()/max()/clamp() have calc()'s limits: @keyframes and non-length properties") {
    compile::CompileResult r = compileRaw(
        "@keyframes k{from{width:min(1px, 2px)}to{width:2px}} #a{animation:k 1s}", R"(<div id="a"></div>)");
    CHECK(hasDiagnostic(r, "calc() is not supported here", true));
    r = compileRaw("#a{opacity:max(0.5, 1)}", R"(<div id="a"></div>)");
    CHECK(hasDiagnostic(r, "only supported on length properties", false));
}

// -------------------------------------------------------------------------------------------
// T-335 (T-322 retracted): rem
// -------------------------------------------------------------------------------------------

// The root element is `<xt>` (the document's `html`), not `<body>`.
TEST_CASE("T-335: rem resolves against the root element's font size at style time") {
    const std::string css = std::string("xt{font-size:20px} #p{") + kAbs +
                            "width:400px;height:100px;font-size:10px}"
                            "#a{position:absolute;width:2rem;height:calc(1rem + 1em)}"
                            "#b{position:absolute;font-size:1.5rem;width:1em}"
                            "#c{position:absolute;width:3em;padding-left:0.5rem}";
    const std::string body = R"(<div id="p"><div id="a"></div><div id="b"></div><div id="c"></div></div>)";
    Loaded l(compileDoc(css, body), 1000, 500);
    CHECK(l.width("a") == doctest::Approx(40));   // 2 x 20 (not the parent's 10).
    CHECK(l.height("a") == doctest::Approx(30));  // calc(): 20 + 10.
    CHECK(l.fontSize("b") == doctest::Approx(30));
    CHECK(l.width("b") == doctest::Approx(30));
    CHECK(l.width("c") == doctest::Approx(30));  // 3em of the parent's 10px (border-box).
    CHECK(l.doc->arena().element(l.node("c")).style.base[prop::PaddingLeft].len.value == doctest::Approx(10));

    // A new root font size reaches every rem user on the next update.
    REQUIRE(l.doc->byId("p").valid());
    ElementRef root = l.doc->byId("p");
    while (root.parent().valid()) {
        root = root.parent();
    }
    CHECK(root.setStyle("font-size: 10px") == Status::kOk);
    l.ctx.update(0.0f);
    CHECK(l.width("a") == doctest::Approx(20));
    CHECK(l.height("a") == doctest::Approx(20));
    CHECK(l.fontSize("b") == doctest::Approx(15));
}

TEST_CASE("T-335: without a root font size rem is 16px, and the root's own rem is too") {
    const std::string css = std::string("xt{font-size:2rem} #a{") + kAbs + "width:1rem}";
    Loaded l(compileDoc(css, R"(<div id="a"></div>)"), 1000, 500);
    CHECK(l.width("a") == doctest::Approx(32));  // The root's 2rem is 2 x 16.
    const std::string plain = std::string("#a{") + kAbs + "width:1.5rem}";
    Loaded m(compileDoc(plain, R"(<div id="a"></div>)"), 1000, 500);
    CHECK(m.width("a") == doctest::Approx(24));
}

// -------------------------------------------------------------------------------------------
// T-335: `linear()` easing at runtime and in the IR (STYL `easingPoints[]`, IR minor 6).
// -------------------------------------------------------------------------------------------

namespace {

/// `linear(0, 0.8 20%, 1)`: 0.4 at p = 0.1, 0.85 at p = 0.4 (hand-computed).
constexpr const char* kLin = "linear(0, 0.8 20%, 1)";

}  // namespace

TEST_CASE("T-335: linear() drives a transition, an animation, @keyframes, @timeline and setStyle") {
    const std::string css = std::string("#t{") + kAbs + "height:10px;width:0px;transition:width 1s " + kLin +
                            "}" + "@keyframes grow{from{width:0px}to{width:100px}}" +
                            "@keyframes stopEase{from{width:0px;animation-timing-function:" + kLin +
                            "}to{width:100px}}" + "#a{" + kAbs + "height:10px;animation:grow 1s " + kLin +
                            "}" + "#k{" + kAbs + "height:10px;animation:stopEase 1s}" + "#m{" + kAbs +
                            "height:10px;width:0px}" + "#g{" + kAbs + "height:10px;width:0px}" +
                            "@timeline tl{#g{0s{width:0px} 1s " + kLin + "{width:100px}}}";
    const std::string body =
        R"(<div id="t"></div><div id="a"></div><div id="k"></div><div id="m"></div><div id="g"></div>)";
    Loaded l(compileDoc(css, body), 400, 200);
    REQUIRE(l.doc->byId("t").setStyle("width: 100px") == Status::kOk);
    REQUIRE(l.doc->byId("m").setStyle("animation: grow 1s linear(0, 0.8 20%, 1)") == Status::kOk);
    REQUIRE(l.doc->playTimeline("", "tl") == Status::kOk);
    l.ctx.update(0.0f);  // The transition and the runtime animation start here.
    l.ctx.update(0.1f);
    CHECK(l.width("a") == doctest::Approx(40).epsilon(0.01));
    CHECK(l.width("k") == doctest::Approx(40).epsilon(0.01));
    CHECK(l.width("t") == doctest::Approx(40).epsilon(0.01));
    CHECK(l.width("m") == doctest::Approx(40).epsilon(0.01));
    CHECK(l.width("g") == doctest::Approx(40).epsilon(0.01));
    l.ctx.update(0.3f);
    CHECK(l.width("a") == doctest::Approx(85).epsilon(0.01));
    CHECK(l.width("t") == doctest::Approx(85).epsilon(0.01));
    CHECK(l.width("m") == doctest::Approx(85).epsilon(0.01));
    // The runtime points were interned once, after the IR pool's.
    const std::span<const EasingPoint> pool = l.doc->animator().easingPoints();
    CHECK(pool.size() == l.doc->ir().style().easingPoints().size());  // Same points: reused.
}

TEST_CASE("T-335: the IR carries easingPoints[], minor 6, and round-trips") {
    const std::string css =
        std::string("#a{transition:opacity 1s linear(0, 0.5 25% 75%, 1)}") +
        "@keyframes k{from{opacity:0;animation-timing-function:linear(0, 1)}to{opacity:1}}" +
        "#b{animation:k 1s linear(0, 0.25, 1)}";
    std::vector<uint8_t> ir = compileDoc(css, R"(<div id="a"></div><div id="b"></div>)");
    IrHeader header{};
    std::memcpy(&header, ir.data(), sizeof header);
    CHECK(header.minor == kIrFormatMinor);
    CHECK(kIrFormatMinor >= 7);  // T-405: 8.
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    const StyleChunk& style = view.value().style();
    REQUIRE(style.transitions().size() == 1);
    const IrEasing& tr = style.transitions()[0].easing;
    CHECK(tr.kind == static_cast<uint8_t>(EasingKind::LinearPoints));
    CHECK(tr.stepCount == 4);
    REQUIRE(irEasingPointOffset(tr) + 4 <= style.easingPoints().size());
    CHECK(style.easingPoints()[irEasingPointOffset(tr) + 2].in == doctest::Approx(0.75f));
    REQUIRE(style.animations().size() == 1);
    CHECK(style.animations()[0].easing.kind == static_cast<uint8_t>(EasingKind::LinearPoints));
    bool keyframeLinear = false;
    for (uint32_t i = 0; i < view.value().timelineCount(); ++i) {
        for (const IrKeyframe& kf : view.value().timeline(i).keyframes()) {
            keyframeLinear =
                keyframeLinear || kf.easing.kind == static_cast<uint8_t>(EasingKind::LinearPoints);
        }
    }
    CHECK(keyframeLinear);
    CHECK(style.easingPoints().size() == 4 + 2 + 3);
    CHECK(compile::IrWriter::fromView(view.value()).finish() == ir);

    // A document without linear() has an empty pool.
    std::vector<uint8_t> plain = compileDoc("#a{transition:opacity 1s ease}", R"(<div id="a"></div>)");
    Result<IrView> plainView = IrView::open(IrBlob{plain.data(), plain.size()}, IrReadOptions{});
    REQUIRE(plainView.ok());
    CHECK(plainView.value().style().easingPoints().empty());
}

TEST_CASE("T-335: a LinearPoints easing outside easingPoints[] is invalid IR") {
    std::vector<uint8_t> ir =
        compileDoc("#a{transition:opacity 1s linear(0, 0.3, 1)}", R"(<div id="a"></div>)");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    auto rewrite = [&](auto edit) {
        compile::IrWriter w = compile::IrWriter::fromView(view.value());
        compile::StyleBuilder sb;
        const StyleChunk& s = view.value().style();
        sb.rules.assign(s.rules().begin(), s.rules().end());
        sb.selectorParts.assign(s.selectorParts().begin(), s.selectorParts().end());
        sb.decls.assign(s.decls().begin(), s.decls().end());
        sb.keyframesRefs.assign(s.keyframesRefs().begin(), s.keyframesRefs().end());
        sb.classIds.assign(s.classIds().begin(), s.classIds().end());
        sb.transitions.assign(s.transitions().begin(), s.transitions().end());
        sb.animations.assign(s.animations().begin(), s.animations().end());
        sb.fontFamilies.assign(s.fontFamilies().begin(), s.fontFamilies().end());
        sb.fontFaces.assign(s.fontFaces().begin(), s.fontFaces().end());
        sb.effects.assign(s.effects().begin(), s.effects().end());
        sb.effectStops.assign(s.effectStops().begin(), s.effectStops().end());
        sb.gradients.assign(s.gradients().begin(), s.gradients().end());
        sb.floatPool.assign(s.floatPool().begin(), s.floatPool().end());
        sb.easingPoints.assign(s.easingPoints().begin(), s.easingPoints().end());
        edit(sb);
        w.setStyle(std::move(sb));
        std::vector<uint8_t> out = w.finish();
        return IrView::open(IrBlob{out.data(), out.size()}, IrReadOptions{}).status();
    };
    CHECK(rewrite([](compile::StyleBuilder&) {}) == Status::kOk);
    CHECK(rewrite([](compile::StyleBuilder& sb) { sb.easingPoints.pop_back(); }) == Status::kInvalidIr);
    CHECK(rewrite([](compile::StyleBuilder& sb) {
              sb.transitions[0].easing = irLinearPointsEasing(0, 1);  // Fewer than two points.
          }) == Status::kInvalidIr);
    CHECK(rewrite([](compile::StyleBuilder& sb) { std::swap(sb.easingPoints[0], sb.easingPoints[2]); }) ==
          Status::kInvalidIr);  // Inputs out of order.
}

TEST_CASE("T-335: IR minor 5 (and older) files still load: no easingPoints[], no rem") {
    // A current file rewritten as minor 5: the STYL payload ends after floatPool[].
    std::vector<uint8_t> ir =
        compileDoc("#a{width:10px;transition:opacity 1s ease}", R"(<div id="a"></div>)");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    REQUIRE(view.value().style().easingPoints().empty());
    // Drop the trailing empty `easingPoints[]` count from STYL and mark the header minor 5.
    std::vector<uint8_t> old;
    {
        IrHeader header{};
        std::memcpy(&header, ir.data(), sizeof header);
        header.minor = 5;
        old.resize(sizeof header);
        size_t at = sizeof header;
        for (uint32_t c = 0; c < header.chunkCount; ++c) {
            IrChunkHeader ch{};
            std::memcpy(&ch, ir.data() + at, sizeof ch);
            std::vector<uint8_t> payload(
                ir.begin() + static_cast<std::ptrdiff_t>(at + sizeof ch),
                ir.begin() + static_cast<std::ptrdiff_t>(at + sizeof ch + ch.payloadLength));
            if (std::memcmp(ch.type, "STYL", 4) == 0) {
                REQUIRE(payload.size() >= 4);
                uint32_t last = 0;
                std::memcpy(&last, payload.data() + payload.size() - 4, 4);
                REQUIRE(last == 0);  // The empty easingPoints[] count.
                payload.resize(payload.size() - 4);
                ch.payloadLength = static_cast<uint32_t>(payload.size());
            }
            const size_t base = old.size();
            old.resize(base + sizeof ch);
            std::memcpy(old.data() + base, &ch, sizeof ch);
            old.insert(old.end(), payload.begin(), payload.end());
            at += sizeof ch + ch.payloadLength + (std::memcmp(ch.type, "STYL", 4) == 0 ? 4 : 0);
        }
        header.totalLength = static_cast<uint32_t>(old.size());
        std::memcpy(old.data(), &header, sizeof header);
    }
    Result<IrView> oldView = IrView::open(IrBlob{old.data(), old.size()}, IrReadOptions{});
    REQUIRE(oldView.ok());
    CHECK(oldView.value().style().easingPoints().empty());
    Loaded l(old, 400, 200);
    CHECK(l.width("a") == doctest::Approx(10));
}
