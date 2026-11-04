#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostic_report.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/types.h"

/// T-568 (Docs/design/27-interaction.md §27.8; Docs/reviews/2026-11-01-midnight-grid-report-triage.md
/// R13 / R16 / F42 / F43 / F110): binding diagnostics, number text and the floor / ceil converters.

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

TEST_CASE("T-568 F88: a format() argument chain warns and stays literal text") {
    const std::string src =
        page("",
             "<div id=\"P\" port:target_hp=\"number\" port:dmg_final=\"number\"></div>"
             "<span id=\"T\" bind:text=\"format('{0} -> {1}', target_hp, dmg_final | scale(-1) | "
             "offset(target_hp) | round(0))\"></span>");
    const CompileResult r = compileSrc(src);
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found =
        withMessage(r, "bind: format() argument chains are not evaluated before IR 3.0");
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Warning);
    CHECK(found[0].code == "bind.formatChain");
    CHECK(found[0].line == 1);
    CHECK(fromCompileDiagnostic(found[0]).code == "bind.formatChain");
    // No behaviour change: the chain is still the literal text.
    Ui ui = open(src);
    ui.set("target_hp", 70);
    CHECK(ui.text("T") == "70 -> dmg_final | scale(-1) | offset(target_hp) | round(0)");
    // A converter-form format() argument too; plain ports and literals do not warn.
    CHECK(withMessage(compileSrc(page("",
                                      "<div port:a=\"number\" port:b=\"number\"></div>"
                                      "<span bind:text=\"a | format('{0}/{1}', b | round(0))\"></span>")),
                      "bind: format() argument chains are not evaluated before IR 3.0")
              .size() == 1);
    CHECK(withMessage(compileSrc(page("",
                                      "<div port:a=\"number\" port:b=\"number\"></div>"
                                      "<span bind:text=\"format('{0} | {1}', a, 'x | y', b)\"></span>")),
                      "bind: format() argument chains are not evaluated before IR 3.0")
              .empty());
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

TEST_CASE("T-568 F104: a calc() value bound to a length property warns") {
    const std::string body =
        "<div id=\"P\" port:step=\"number\"></div><div id=\"M\" style=\"left: 600px\" "
        "bind:left=\"step | step(1: 'calc(50% - 40px)', 2: 'calc(100% - 280px)', 3: 1000px)\">"
        "</div>";
    const CompileResult r = compileSrc(page("#M { position: absolute; }", body));
    REQUIRE(r.ok());
    const std::vector<Diagnostic> found =
        withMessage(r, "bind: calc() is not evaluated at runtime before IR 3.0 (05 §5.4)");
    REQUIRE(found.size() == 1);
    CHECK(found[0].level == Diagnostic::Level::Warning);
    CHECK(found[0].code == "bind.calcNotEvaluated");
    // Not on a text binding.
    CHECK(withMessage(compileSrc(page("",
                                      "<div port:a=\"number\"></div><span bind:text=\"a | "
                                      "select(1: 'calc(1px)', default: 'x')\"></span>")),
                      "bind: calc() is not evaluated at runtime before IR 3.0 (05 §5.4)")
              .empty());
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
