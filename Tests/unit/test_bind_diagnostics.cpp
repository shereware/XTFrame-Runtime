#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostic_report.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/types.h"

/// T-568 (Docs/design/27-interaction.md §27.8; Docs/reviews/2026-11-01-midnight-grid-report-triage.md
/// R13 / R16 / F42 / F43 / F110): binding diagnostics, number text and the floor / ceil converters.
/// T-585 (03 §3.12.11): converter sub-chains as format() / select() / step() arguments (BCHN).
/// T-621 (27 §27.3.3): select() / step() branch values typed by the binding target.

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\">" +
           std::string(body) + "</body></xt>";
}

CompileResult compileSrc(const std::string& xt) {
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", CompileOptions{});
}

/// The diagnostics whose message is exactly `message`.
std::vector<Diagnostic> withMessage(const CompileResult& r, std::string_view message) {
    std::vector<Diagnostic> out;
    for (const Diagnostic& d : r.diagnostics) {
        if (d.message == message) {
            out.push_back(d);
        }
    }
    return out;
}

struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;

    void step(float dt) {
        clock += dt;
        doc->update(clock);
    }
    ElementRef el(std::string_view id) const { return doc->byId(id); }
    void set(std::string_view name, float v) {
        const PortId p = doc->lookupPort(name);
        REQUIRE(p != kInvalidPort);
        REQUIRE(doc->setPort(p, Value::ofFloat(v)) == Status::kOk);
    }
    std::string text(std::string_view id) const {
        doc->update(clock);
        return std::string(el(id).text());
    }
};

Ui open(const std::string& xt) {
    Ui out;
    out.result = compileSrc(xt);
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 200});
    out.doc->update(0.0f);
    return out;
}

}  // namespace

TEST_CASE("T-585 F88: a format() argument chain is a sub-chain, evaluated before format()") {
    // 03 §3.12.11: the second value compiles into a BCHN sub-chain; the old warning is gone.
    const std::string src =
        page("",
             "<div id=\"P\" port:target_hp=\"number\" port:dmg=\"number\"></div>"
             "<span id=\"T\" bind:text=\"format('{0} \xE2\x86\x92 {1}', target_hp, dmg | scale(-1) | "
             "offset(target_hp) | round(0))\"></span>");
    const CompileResult r = compileSrc(src);
    REQUIRE(r.ok());
    for (const Diagnostic& d : r.diagnostics) {
        CHECK(d.code != "bind.formatChain");
    }
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const BindingChunk& bc = view.value().bindings();
    REQUIRE(bc.chainsPresent());
    REQUIRE(bc.chains().size() == 1);
    CHECK(bc.chains()[0].input == 1u);      // dmg
    CHECK(bc.chains()[0].stepCount == 3u);  // scale, offset, round
    REQUIRE(bc.bindings().size() == 1);
    const IrBinding& b = bc.bindings()[0];
    REQUIRE(b.sourceCount == 2);  // target_hp (the input), then dmg (the sub-chain's port).
    CHECK(bc.sources()[b.sourceOffset] == 0u);
    CHECK(bc.sources()[b.sourceOffset + 1] == 1u);
    const IrConverterStep& format = bc.steps()[b.stepOffset];
    CHECK(format.op == static_cast<uint8_t>(ConverterOp::Format));
    CHECK(converterArgKind(format, 1) == ConverterArg::SubChain);
    IrWriter again = IrWriter::fromView(view.value());
    CHECK(again.finish() == r.ir);  // BCHN round-trips.

    Ui ui = open(src);
    ui.set("target_hp", 70);
    ui.set("dmg", 40);
    CHECK(ui.text("T") == "70 \xE2\x86\x92 30");
    ui.set("dmg", 10);  // Only the sub-chain reads dmg: it is a dependency of the binding.
    CHECK(ui.text("T") == "70 \xE2\x86\x92 60");
    ui.set("target_hp", 100);
    CHECK(ui.text("T") == "100 \xE2\x86\x92 90");
}

TEST_CASE("T-585: sub-chains in format() / select() / step() arguments and the outer input") {
    const std::string src =
        page("",
             "<div port:a=\"number\" port:b=\"number\" port:mode=\"string\"></div>"
             "<span id=\"F\" bind:text=\"a | format('{0}/{1}', b | scale(2))\"></span>"
             "<span id=\"O\" bind:text=\"a | format('{0}={1}', scale(2) | round(0))\"></span>"
             "<span id=\"S\" bind:text=\"mode | select(x: b | offset(1), default: 'none')\"></span>"
             "<span id=\"P\" bind:text=\"a | step(10: port(b) | scale(3), default: 0)\"></span>"
             "<span id=\"Q\" bind:text=\"format('{0} | {1}', a, 'x | y')\"></span>");
    Ui ui = open(src);
    ui.set("a", 3.4f);
    ui.set("b", 5);
    CHECK(ui.text("F") == "3.4/10");
    CHECK(ui.text("O") == "3.4=7");        // The sub-chain starts from the outer chain's input.
    CHECK(ui.text("Q") == "3.4 | x | y");  // A quoted argument is still a literal.
    CHECK(ui.text("P") == "0");
    ui.set("a", 12);
    CHECK(ui.text("P") == "15");
    CHECK(ui.text("O") == "12=24");
    ui.set("b", 2);  // The sub-chain ports re-evaluate their bindings.
    CHECK(ui.text("F") == "12/4");
    CHECK(ui.text("P") == "6");
    REQUIRE(ui.doc->setPortText(ui.doc->lookupPort("mode"), "x") == Status::kOk);
    CHECK(ui.text("S") == "3");
    REQUIRE(ui.doc->setPortText(ui.doc->lookupPort("mode"), "y") == Status::kOk);
    CHECK(ui.text("S") == "none");
}

namespace {

/// `a | format('{0}{1}', <level 2>)` ... down to `a | round(0)` at level `depth`.
std::string nestedChain(int level, int depth) {
    if (level == depth) {
        return "a | round(0)";
    }
    return "a | format('{0}{1}', " + nestedChain(level + 1, depth) + ")";
}

std::string nestedPage(int depth) {
    return page("", "<div port:a=\"number\"></div><span id=\"T\" bind:text=\"format('{0}{1}', a, " +
                        nestedChain(1, depth) + ")\"></span>");
}

}  // namespace

TEST_CASE("T-585: converter argument chains nest at most 4 deep") {
    const CompileResult four = compileSrc(nestedPage(4));
    REQUIRE(four.ok());
    Ui ui = open(nestedPage(4));
    ui.set("a", 1);
    CHECK(ui.text("T") == "11111");
    const CompileResult five = compileSrc(nestedPage(5));
    CHECK(!five.ok());
    CHECK(withMessage(five, "bind: converter argument chains nest deeper than 4").size() == 1);
    // tween / delay keep per-step state: not inside a sub-chain.
    const CompileResult tween = compileSrc(
        page("", "<div port:a=\"number\"></div><span bind:text=\"format('{0}', a, a | tween(1s))\"></span>"));
    CHECK(!tween.ok());
}

TEST_CASE("T-585: IrView::open validates the BCHN pool") {
    const CompileResult r = compileSrc(nestedPage(2));
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const BindingChunk& bc = view.value().bindings();
    REQUIRE(bc.chains().size() == 2);  // The inner chain first (index 0), named by chain 1.
    auto reopen = [&](auto mutate) {
        BindingBuilder bb;
        bb.bindings.assign(bc.bindings().begin(), bc.bindings().end());
        bb.sources.assign(bc.sources().begin(), bc.sources().end());
        bb.steps.assign(bc.steps().begin(), bc.steps().end());
        bb.chains.assign(bc.chains().begin(), bc.chains().end());
        bb.chainSteps.assign(bc.chainSteps().begin(), bc.chainSteps().end());
        mutate(bb);
        IrWriter w = IrWriter::fromView(view.value());
        w.setBindings(bb);
        const std::vector<uint8_t> blob = w.finish();
        return IrView::open(IrBlob{blob.data(), blob.size()}).ok();
    };
    // Points `bb`'s SubChain argument in chain `chain` at `target`.
    auto retarget = [](BindingBuilder& bb, size_t chain, uint32_t target) {
        const IrSubChain& c = bb.chains[chain];
        for (uint32_t s = c.stepOffset; s < c.stepOffset + c.stepCount; ++s) {
            for (uint32_t k = 0; k < 4; ++k) {
                if (converterArgKind(bb.chainSteps[s], k) == ConverterArg::SubChain) {
                    std::memcpy(&bb.chainSteps[s].args[k], &target, sizeof(target));
                }
            }
        }
    };
    CHECK(reopen([](BindingBuilder&) {}));
    CHECK(!reopen([&](BindingBuilder& bb) { retarget(bb, 1, 1); }));  // Self-reference.
    CHECK(!reopen([&](BindingBuilder& bb) { retarget(bb, 1, 7); }));  // Past the pool.
    CHECK(!reopen([](BindingBuilder& bb) { bb.chains[0].input = 99; }));
    CHECK(!reopen([](BindingBuilder& bb) { bb.chains[0].stepCount = 99; }));
    CHECK(!reopen([&](BindingBuilder& bb) {
        // A cycle 0 -> 1 -> 0: chain 0 gets a SubChain argument naming chain 1.
        IrConverterStep& step = bb.chainSteps[bb.chains[0].stepOffset];
        const uint32_t one = 1;
        step.argCount = 1;
        step.argKinds = static_cast<uint16_t>(ConverterArg::SubChain);
        std::memcpy(&step.args[0], &one, sizeof(one));
    }));
    CHECK(!reopen([](BindingBuilder& bb) {
        // A BIND step naming a chain without a BCHN chunk.
        bb.chains.clear();
        bb.chainSteps.clear();
    }));
    // Depth 5 built by hand (the compiler refuses it): chains 0..4, each naming the previous one.
    CHECK(!reopen([](BindingBuilder& bb) {
        const IrSubChain inner = bb.chains[0];
        const IrSubChain outer = bb.chains[1];
        const std::vector<IrConverterStep> outerSteps(
            bb.chainSteps.begin() + outer.stepOffset,
            bb.chainSteps.begin() + outer.stepOffset + outer.stepCount);
        bb.chains.assign(1, inner);
        for (uint32_t level = 1; level < 5; ++level) {
            IrSubChain c = outer;
            c.stepOffset = static_cast<uint32_t>(bb.chainSteps.size());
            for (IrConverterStep step : outerSteps) {
                for (uint32_t k = 0; k < 4; ++k) {
                    if (converterArgKind(step, k) == ConverterArg::SubChain) {
                        const uint32_t prev = level - 1;
                        std::memcpy(&step.args[k], &prev, sizeof(prev));
                    }
                }
                bb.chainSteps.push_back(step);
            }
            bb.chains.push_back(c);
        }
        // The binding's own chain names the outermost (index 4).
        for (IrConverterStep& step : bb.steps) {
            for (uint32_t k = 0; k < 4; ++k) {
                if (converterArgKind(step, k) == ConverterArg::SubChain) {
                    const uint32_t top = 4;
                    std::memcpy(&step.args[k], &top, sizeof(top));
                }
            }
        }
    }));
}

TEST_CASE("T-573: a binding that reads an out port warns (bind.outPortSource)") {
    // T-568 made it an error; the main model's ruling on the T-568 record makes it a warning: the
    // host's setPort on an out port and a flow `emit` to it both update its bindings.
    const CompileResult r = compileSrc(page("",
                                            "<div id=\"P\" port:slot_pick=\"number out\"></div>"
                                            "<span id=\"T\" bind:text=\"slot_pick\"></span>"));
    CHECK(r.ok());
    const std::vector<Diagnostic> found = withMessage(r, "bind: source 'slot_pick' is an out port");
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Warning);
    CHECK(found[0].code == "bind.outPortSource");
    // As a converter argument too.
    const CompileResult arg = compileSrc(page("",
                                              "<div port:a=\"number\" port:k=\"number out\"></div>"
                                              "<span bind:text=\"a | scale(k)\"></span>"));
    CHECK(arg.ok());
    CHECK(withMessage(arg, "bind: source 'k' is an out port").size() == 1);
    // An in port (the default direction) reads without a warning.
    const CompileResult in =
        compileSrc(page("", "<div port:a=\"number in\"></div><span bind:text=\"a\"></span>"));
    CHECK(in.ok());
    CHECK(withMessage(in, "bind: source 'a' is an out port").empty());
    // The host's setPort on the out port re-evaluates the binding.
    Ui ui = open(
        page("", "<div id=\"P\" port:score=\"number out\"></div><span id=\"T\" bind:text=\"score\"></span>"));
    ui.set("score", 42);
    CHECK(ui.text("T") == "42");
}

TEST_CASE("T-586 F104: a static calc() bound to a length property is a CALC program") {
    const std::string body =
        "<div id=\"P\" port:step=\"number\"></div><div id=\"M\" style=\"left: 600px\" "
        "bind:left=\"step | step(1: 'calc(50% - 40px)', 2: 'calc(100% - 280px)', 3: 1000px)\">"
        "</div>";
    const std::string src = page("#M { position: absolute; width: 10px; height: 10px; }", body);
    const CompileResult r = compileSrc(src);
    REQUIRE(r.ok());
    CHECK(withMessage(r, "bind: calc() is not evaluated at runtime (05 §5.4)").empty());
    // Both values are Calc arguments into the document's CALC pool.
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    int calcArgs = 0;
    for (const IrConverterStep& step : view.value().bindings().steps()) {
        for (uint32_t i = 0; i < 4; ++i) {
            if (converterArgKind(step, i) == ConverterArg::Calc) {
                uint32_t index = 0;
                std::memcpy(&index, &step.args[i], sizeof(index));
                CHECK(index < view.value().calcs().calcs().size());
                ++calcArgs;
            }
        }
    }
    CHECK(calcArgs == 2);
    // Each value applies; a calc() re-evaluates when the viewport changes (layout reruns it).
    Ui ui = open(src);
    auto left = [&ui]() {
        ui.doc->update(ui.clock);
        return ui.doc->arena().element(ui.el("M").node).box.x;
    };
    ui.set("step", 1);
    CHECK(left() == doctest::Approx(400 * 0.5f - 40));
    ui.doc->setViewport(Rect{0, 0, 800, 200});
    CHECK(left() == doctest::Approx(800 * 0.5f - 40));
    ui.set("step", 2);
    CHECK(left() == doctest::Approx(800 - 280));
    ui.doc->setViewport(Rect{0, 0, 600, 200});
    CHECK(left() == doctest::Approx(600 - 280));
    ui.set("step", 3);
    CHECK(left() == doctest::Approx(1000));
    ui.set("step", 1);
    CHECK(left() == doctest::Approx(600 * 0.5f - 40));
}

TEST_CASE("T-586: a calc() with a port in it still warns bind.calcNotEvaluated") {
    const std::string body =
        "<div id=\"P\" port:x=\"number\"></div><div id=\"M\" "
        "bind:left=\"x | select(1: 'calc(port(x) + 10px)', default: 'calc(10px + 5%)')\"></div>";
    const CompileResult r = compileSrc(page("#M { position: absolute; }", body));
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found =
        withMessage(r, "bind: calc() is not evaluated at runtime (05 §5.4)");
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Warning);
    CHECK(found[0].code == "bind.calcNotEvaluated");
    // Static calc() only: no warning; not on a text binding either.
    CHECK(withMessage(compileSrc(page("#M { position: absolute; }",
                                      "<div port:a=\"number\"></div><div id=\"M\" bind:width=\"a | "
                                      "select(1: calc(10px + 5%), default: 'calc(100% - 1em)')\"></div>")),
                      "bind: calc() is not evaluated at runtime (05 §5.4)")
              .empty());
    CHECK(withMessage(compileSrc(page("",
                                      "<div port:a=\"number\"></div><span bind:text=\"a | "
                                      "select(1: 'calc(1px)', default: 'x')\"></span>")),
                      "bind: calc() is not evaluated at runtime (05 §5.4)")
              .empty());
}

TEST_CASE("T-586: IR validation rejects a Calc converter argument outside the CALC pool") {
    const CompileResult r = compileSrc(page("",
                                            "<div port:a=\"number\"></div><div id=\"M\" bind:width=\"a | "
                                            "select(1: 'calc(10px + 5%)', default: 4px)\"></div>"));
    REQUIRE(r.ok());
    std::vector<uint8_t> bytes = r.ir;
    Result<IrView> view = IrView::open(IrBlob{bytes.data(), bytes.size()});
    REQUIRE(view.ok());
    const uint32_t calcCount = static_cast<uint32_t>(view.value().calcs().calcs().size());
    size_t offset = bytes.size();
    for (const IrConverterStep& step : view.value().bindings().steps()) {
        for (uint32_t i = 0; i < 4; ++i) {
            if (converterArgKind(step, i) == ConverterArg::Calc) {
                offset = static_cast<size_t>(reinterpret_cast<const uint8_t*>(&step.args[i]) - bytes.data());
            }
        }
    }
    REQUIRE(offset < bytes.size());
    std::memcpy(bytes.data() + offset, &calcCount, sizeof(calcCount));
    CHECK(IrView::open(IrBlob{bytes.data(), bytes.size()}).status() == Status::kInvalidIr);
}

TEST_CASE("T-568 F107: a bare argument that is both an enum value and a port warns") {
    const std::string body =
        "<div id=\"P\" port:filter=\"enum(all|earned|locked)\" port:earned=\"number\"></div>"
        "<div id=\"A\" bind:class.active=\"filter | eq(earned)\"></div>";
    const CompileResult r = compileSrc(page("", body));
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found =
        withMessage(r, "bind: 'earned' is a port; quote it to mean the enum value");
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Warning);
    CHECK(found[0].code == "bind.enumPortName");
    // Quoted: the enum value, no warning; an enum value that is no port: no warning.
    CHECK(compileSrc(page("",
                          "<div port:filter=\"enum(all|earned|locked)\" port:earned=\"number\"></div>"
                          "<div bind:class.active=\"filter | eq('earned')\"></div>"))
              .diagnostics.empty());
    CHECK(compileSrc(page("",
                          "<div port:filter=\"enum(all|earned|locked)\"></div>"
                          "<div bind:class.active=\"filter | eq(locked)\"></div>"))
              .diagnostics.empty());
}

TEST_CASE("T-568 F42: binding number text uses formatFloat(v, 6)") {
    Ui ui =
        open(page("",
                  "<div id=\"P\" port:v=\"number\"></div><span id=\"T\" bind:text=\"v | scale(10)\"></span>"
                  "<span id=\"F\" bind:text=\"v | format('{0} m')\"></span>"));
    ui.set("v", 12.345f);
    CHECK(ui.text("T") == "123.45");
    ui.set("v", 0.5f);
    CHECK(ui.text("T") == "5");
    CHECK(ui.text("F") == "0.5 m");
    ui.set("v", 1234567.0f);
    CHECK(ui.text("T") == "1.23457e+07");  // %.6g, as the flow's toString (53 §53.4.9).
    ui.set("v", -0.0f);
    CHECK(ui.text("T") == "0");
}

TEST_CASE("T-568 F43: floor and ceil converters") {
    Ui ui = open(page("",
                      "<div id=\"P\" port:v=\"number\"></div><span id=\"A\" bind:text=\"v | floor\"></span>"
                      "<span id=\"B\" bind:text=\"v | ceil\"></span>"));
    ui.set("v", 2.5f);
    CHECK(ui.text("A") == "2");
    CHECK(ui.text("B") == "3");
    ui.set("v", -2.5f);
    CHECK(ui.text("A") == "-3");
    CHECK(ui.text("B") == "-2");
    CHECK_FALSE(
        compileSrc(page("", "<div port:v=\"number\"></div><span bind:text=\"v | floor(1)\"></span>")).ok());
    CHECK(static_cast<int>(ConverterOp::Floor) == 42);
    CHECK(static_cast<int>(ConverterOp::Ceil) == 43);
    CHECK(kConverterOpLast == 43);
}

TEST_CASE("T-568 F110: the effect-missing warning names the element and the track") {
    Ui ui = open(page(".fx { effect: gradient-mask(radial, 0px 0px, 0px 0px, #000f 0%, #0000 100%); }",
                      "<div id=\"P\" port:px=\"number\"></div>"
                      "<div id=\"M\" class=\"fx\" bind:effect.gradient-mask.p0x=\"px\"></div>"));
    ui.step(0.016f);
    REQUIRE(ui.el("M").removeClass("fx"));
    ui.step(0.016f);
    ui.set("px", 10);
    ui.step(0.016f);
    std::vector<std::string> missing;
    for (const std::string& w : ui.doc->warnings()) {
        if (w.rfind(kWarnEffectMissing, 0) == 0) {
            missing.push_back(w);
        }
    }
    REQUIRE(missing.size() == 1);
    CAPTURE(missing[0]);
    CHECK(missing[0].rfind(std::string(kWarnEffectMissing) + ": #M effect[0].param", 0) == 0);
}

// -- T-621 (27 §27.3.3, 03 §3.12): select() / step() branch values typed by the binding target ---

namespace {

/// A symbol with a bool and a number in-port, instances bound from the document port `mode`.
std::string branchPage(std::string_view style, std::string_view body) {
    return "<xt><head><style>" + std::string(style) +
           "</style><symbol name=\"R\"><span id=\"T\" port:ready=\"bool in\" port:count=\"number in\">"
           "</span></symbol></head><body id=\"Root\"><div id=\"P\" port:mode=\"number\" "
           "port:label=\"string\"></div>" +
           std::string(body) + "</body></xt>";
}

/// The kinds of the default and the first value argument of binding `index`'s first step.
std::vector<ConverterArg> branchKinds(const CompileResult& r, size_t index) {
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const BindingChunk& bc = view.value().bindings();
    REQUIRE(index < bc.bindings().size());
    const IrBinding& b = bc.bindings()[index];
    const IrConverterStep& head = bc.steps()[b.stepOffset];
    std::vector<ConverterArg> out;
    out.push_back(converterArgKind(head, 0));  // default
    for (uint32_t a = 2; a < head.argCount && a < 4; a += 2) {
        out.push_back(converterArgKind(head, a));
    }
    return out;
}

bool readyOf(Ui& ui, std::string_view use) {
    ui.step(0.016f);
    const PortId p = ui.doc->lookupPort(std::string(use) + ".ready");
    REQUIRE(p != kInvalidPort);
    return ui.doc->portText(p) == "true";
}

}  // namespace

TEST_CASE("T-621: select() / step() into a bool port: literal, quoted and step branch values") {
    const std::string src = branchPage(
        "",
        "<use symbol=\"R\" id=\"A\" bind:ready=\"mode | select(0: true, 1: true, default: false)\"></use>"
        "<use symbol=\"R\" id=\"B\" bind:ready=\"mode | select(1: 'true', default: 'false')\"></use>"
        "<use symbol=\"R\" id=\"C\" bind:ready=\"mode | step(1: false, 2: true, default: true) | "
        "tween(0s)\"></use>");
    const CompileResult r = compileSrc(src);
    REQUIRE(r.ok());
    for (size_t i = 0; i < 3; ++i) {
        for (ConverterArg k : branchKinds(r, i)) {
            CHECK(k == ConverterArg::Number);  // Typed at compile time; no strings left.
        }
    }
    Ui ui = open(src);
    CHECK(readyOf(ui, "A"));
    CHECK_FALSE(readyOf(ui, "B"));
    CHECK(readyOf(ui, "C"));
    ui.set("mode", 1);
    CHECK(readyOf(ui, "A"));
    CHECK(readyOf(ui, "B"));
    CHECK_FALSE(readyOf(ui, "C"));
    ui.set("mode", 2);
    CHECK_FALSE(readyOf(ui, "A"));
    CHECK_FALSE(readyOf(ui, "B"));
    CHECK(readyOf(ui, "C"));
}

TEST_CASE("T-621: select() into a number port converts quoted numbers") {
    const std::string src = branchPage(
        "", "<use symbol=\"R\" id=\"A\" bind:count=\"mode | select(0: '7', 1: 2.5, default: '-1')\"></use>");
    Ui ui = open(src);
    const PortId p = ui.doc->lookupPort("A.count");
    REQUIRE(p != kInvalidPort);
    CHECK(ui.doc->getPort(p).f == doctest::Approx(7.0f));
    ui.set("mode", 1);
    ui.step(0.016f);
    CHECK(ui.doc->getPort(p).f == doctest::Approx(2.5f));
    ui.set("mode", 5);
    ui.step(0.016f);
    CHECK(ui.doc->getPort(p).f == doctest::Approx(-1.0f));
}

TEST_CASE("T-621: bind:display and bind:state.X branch values; display keeps its value form") {
    const std::string src =
        branchPage("#A, #B, #C { height: 20px; } #S:state(Low) { opacity: 0.5; }",
                   "<div id=\"A\" bind:display=\"mode | select(0: 'false', default: 'true')\"></div>"
                   "<div id=\"B\" bind:display=\"mode | select(0: none, default: block)\"></div>"
                   "<div id=\"C\"></div>"
                   "<div id=\"S\" bind:state.Low=\"mode | select(1: 'true', default: 'false')\"></div>");
    const CompileResult r = compileSrc(src);
    REQUIRE(r.ok());
    for (ConverterArg k : branchKinds(r, 0)) {
        CHECK(k == ConverterArg::Number);
    }
    for (ConverterArg k : branchKinds(r, 1)) {
        CHECK(k == ConverterArg::String);  // A display value, not a bool spelling.
    }
    for (ConverterArg k : branchKinds(r, 2)) {
        CHECK(k == ConverterArg::Number);
    }
    Ui ui = open(src);
    ui.step(0.016f);
    CHECK(ui.el("C").boundingBox().y == doctest::Approx(0));  // A and B take no room.
    CHECK_FALSE(ui.doc->hasCustomState(ui.el("S"), "Low"));
    ui.set("mode", 1);
    ui.step(0.016f);
    CHECK(ui.el("C").boundingBox().y == doctest::Approx(40));
    CHECK(ui.doc->hasCustomState(ui.el("S"), "Low"));
}

TEST_CASE("T-621: a branch value the target type cannot take is bind.branchType") {
    const CompileResult r = compileSrc(branchPage(
        "", "<use symbol=\"R\" id=\"A\" bind:ready=\"mode | select(0: on, default: false)\"></use>"));
    CHECK(!r.ok());
    const std::vector<Diagnostic> found =
        withMessage(r, "bind:ready: select() branch value \"on\" is not a bool (true, false or a number)");
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Error);
    CHECK(found[0].code == "bind.branchType");
    CHECK(!found[0].fix.empty());
    const CompileResult num = compileSrc(branchPage(
        "", "<use symbol=\"R\" id=\"A\" bind:count=\"mode | step(1: 'many', default: 0)\"></use>"));
    CHECK(withMessage(num, "bind:count: step() branch value \"many\" is not a number").size() == 1);
    // Only the last value step is typed: a select() feeding eq() keeps its strings; a string
    // target keeps `true` / `false` as text.
    const std::string ok = branchPage(
        "",
        "<use symbol=\"R\" id=\"A\" bind:ready=\"mode | select(0: 'a', default: 'b') | eq('a')\"></use>"
        "<span id=\"X\" bind:text=\"mode | select(0: true, default: false)\"></span>");
    REQUIRE(compileSrc(ok).ok());
    Ui ui = open(ok);
    CHECK(readyOf(ui, "A"));
    CHECK(ui.text("X") == "true");
}

TEST_CASE("T-621: a bool port takes the strings \"true\" / \"false\" at run time (older IR)") {
    // IR written before T-621: the port binding's select() carries String branch values. Build it
    // by giving the port binding the text binding's (string) steps.
    const std::string src =
        branchPage("",
                   "<use symbol=\"R\" id=\"A\" bind:ready=\"mode | select(1: true, default: false)\"></use>"
                   "<span id=\"X\" bind:text=\"mode | select(1: true, default: false)\"></span>"
                   "<use symbol=\"R\" id=\"L\" bind:ready=\"label\"></use>");
    const CompileResult r = compileSrc(src);
    REQUIRE(r.ok());
    Result<IrView> view = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(view.ok());
    const BindingChunk& bc = view.value().bindings();
    BindingBuilder bindings;
    bindings.bindings.assign(bc.bindings().begin(), bc.bindings().end());
    bindings.sources.assign(bc.sources().begin(), bc.sources().end());
    bindings.steps.assign(bc.steps().begin(), bc.steps().end());
    const IrBinding* port = nullptr;
    const IrBinding* text = nullptr;
    for (const IrBinding& b : bindings.bindings) {
        if (static_cast<BindTarget>(b.target) == BindTarget::Port && b.stepCount > 0) {
            port = &b;
        } else if (static_cast<BindTarget>(b.target) == BindTarget::Text) {
            text = &b;
        }
    }
    REQUIRE(port != nullptr);
    REQUIRE(text != nullptr);
    REQUIRE(port->stepCount == text->stepCount);
    for (uint32_t i = 0; i < port->stepCount; ++i) {
        bindings.steps[port->stepOffset + i] = bindings.steps[text->stepOffset + i];
    }
    IrWriter writer = IrWriter::fromView(view.value());
    writer.setBindings(bindings);
    const std::vector<uint8_t> blob = writer.finish();
    Ui ui;
    REQUIRE(Document::load(IrBlob{blob.data(), blob.size()}, false, nullptr, nullptr, ui.doc) == Status::kOk);
    ui.doc->setViewport(Rect{0, 0, 400, 200});
    ui.doc->update(0.0f);
    CHECK_FALSE(readyOf(ui, "A"));
    ui.set("mode", 1);
    CHECK(readyOf(ui, "A"));  // "true" -> true (toNum() alone read 0).
    ui.set("mode", 0);
    CHECK_FALSE(readyOf(ui, "A"));
    // A string port bound to a bool port: the same run-time conversion.
    REQUIRE(ui.doc->setPortText(ui.doc->lookupPort("label"), "true") == Status::kOk);
    CHECK(readyOf(ui, "L"));
    REQUIRE(ui.doc->setPortText(ui.doc->lookupPort("label"), "false") == Status::kOk);
    CHECK_FALSE(readyOf(ui, "L"));
}
