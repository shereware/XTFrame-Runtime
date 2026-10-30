#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "mini_css.h"  // parseDeclarationsCount() (Code/Runtime/core/src, on the test include path).
#include "xt/charconv.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/num_guard.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/style_template.h"

/// T-477 (Docs/design/27-interaction.md §27.4.2 item 4): StyleTemplate / ElementRef::
/// setStyleTemplate() write exactly what setStyle() of the same text writes, and the typed
/// Document::setPort() is observably the same as setPortText() of the text the flow used to send.

using namespace xt;
using namespace xt::compile;

namespace {

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
    float clock = 0;
    void step() {
        clock += 0.016f;
        doc->update(clock);
    }
};

Loaded load(const std::string& style, const std::string& body) {
    Loaded out;
    const std::string xt =
        "<xt><head><style>" + style + "</style></head><body id=\"Root\">" + body + "</body></xt>";
    std::vector<SourceFile> files{{"main.xt", xt}};
    out.result = compileDocumentFromStrings(files, "main.xt", CompileOptions{});
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 300});
    out.doc->update(0.0f);
    return out;
}

/// `format` split at its "{}" holes.
std::vector<std::string_view> holes(std::string_view format) {
    std::vector<std::string_view> out;
    size_t from = 0;
    for (size_t at = format.find("{}"); at != std::string_view::npos; at = format.find("{}", from)) {
        out.push_back(format.substr(from, at - from));
        from = at + 2;
    }
    out.push_back(format.substr(from));
    return out;
}

bool compiles(std::string_view format, StyleTemplate& out) {
    const std::vector<std::string_view> literals = holes(format);
    return compileStyleTemplate(literals, out);
}

/// `format` with its holes filled by `numbers` as the flow prints them (%.6g).
std::string fill(std::string_view format, const std::vector<double>& numbers) {
    const std::vector<std::string_view> literals = holes(format);
    std::string text;
    for (size_t i = 0; i < literals.size(); ++i) {
        text.append(literals[i]);
        if (i + 1 < literals.size()) {
            char buf[64];
            text.append(buf, formatFloat(buf, sizeof(buf), numbers[i % numbers.size()], 6));
        }
    }
    return text;
}

bool sameValue(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case ValueType::Float:
            return std::memcmp(&a.f, &b.f, sizeof(float)) == 0;
        case ValueType::Length:
            return std::memcmp(&a.len.value, &b.len.value, sizeof(float)) == 0 && a.len.unit == b.len.unit;
        case ValueType::Vec2:
            return std::memcmp(&a.v2, &b.v2, sizeof(Vec2)) == 0;
        case ValueType::Vec3:
            return std::memcmp(&a.v3, &b.v3, sizeof(Vec3)) == 0;
        case ValueType::Color:
            return std::memcmp(&a.c, &b.c, sizeof(Color)) == 0;
        default:
            return a.e == b.e;
    }
}

/// The runtime declarations of `a` and `b`, compared field by field.
void checkSameDecls(ElementRef a, ElementRef b) {
    const std::vector<RuntimeStyleDecl>& da = a.doc->arena().element(a.node).runtimeDecls;
    const std::vector<RuntimeStyleDecl>& db = b.doc->arena().element(b.node).runtimeDecls;
    REQUIRE(da.size() == db.size());
    for (size_t i = 0; i < da.size(); ++i) {
        CAPTURE(i);
        CHECK(da[i].prop == db[i].prop);
        CHECK(da[i].important == db[i].important);
        CHECK(da[i].varName == db[i].varName);
        CHECK(sameValue(da[i].value, db[i].value));
    }
}

std::string paintDump(Document& doc) {
    CommandList list;
    doc.paint(list);
    std::string out;
    dumpCommandList(list, out);
    return out;
}

constexpr const char* kStyle =
    "#A, #B { position: absolute; left: 10px; top: 10px; width: 50px; height: 40px; background-color: "
    "#336699; }";
constexpr const char* kBody = "<div id=\"A\"></div><div id=\"B\"></div>";

// The `format -> setStyle` shapes the flow writes (27 §27.4.2 item 4 names them all).
constexpr const char* kHits[] = {
    "transform: translate({}px, {}px)",
    "width: {}px",
    "height: {}%",
    "left: {}px; top: {}px",
    "opacity: {}",
    "transform: rotate({}deg)",
    "transform: scale({})",
    "rotate: {}deg",
    "scale: {} {}",
};

}  // namespace

TEST_CASE("T-477: the format shapes of 27 s27.4.2 compile into style templates") {
    for (const char* format : kHits) {
        CAPTURE(format);
        StyleTemplate t;
        CHECK(compiles(format, t));
        CHECK(t.valid());
    }
    // Pointer-driven numbers go negative: translate / left / top / rotate take every sign.
    for (const char* format :
         {"transform: translate({}px, {}px)", "left: {}px; top: {}px", "transform: rotate({}deg)"}) {
        CAPTURE(format);
        StyleTemplate t;
        REQUIRE(compiles(format, t));
        for (uint8_t domain : t.holeDomain) {
            CHECK(domain == (StyleTemplate::kHoleNegative | StyleTemplate::kHoleZero));
        }
    }
    StyleTemplate constant;  // No hole: a constant text still compiles.
    CHECK(compiles("opacity: 0.5; width: 20px", constant));
    CHECK(constant.holeCount() == 0);
}

TEST_CASE("T-477: shapes a typed write cannot reproduce do not compile") {
    StyleTemplate t;
    CHECK_FALSE(compiles("color: rgb({}, 0, 0)", t));      // Converted (/ 255).
    CHECK_FALSE(compiles("width: 1{}px", t));              // The hole merges with a digit.
    CHECK_FALSE(compiles("width: {}{}px", t));             // Two holes in a row.
    CHECK_FALSE(compiles("width: {}e3px", t));             // An exponent after the hole.
    CHECK_FALSE(compiles("effect: blur({}px)", t));        // A pooled declaration.
    CHECK_FALSE(compiles("width: {}", t));                 // Unitless: only 0 parses.
    CHECK_FALSE(compiles("bogus-prop: {}px", t));          // An error in every probe.
    CHECK_FALSE(compiles("transform: rotate({}rad)", t));  // Not a runtime unit.
    CHECK_FALSE(compiles("font-family: x{}", t));
}

TEST_CASE("T-477: setStyleTemplate writes what setStyle writes, and paints the same") {
    const std::vector<std::vector<double>> numbers = {{12.5, -3.25}, {0.0, 1.0},  {123456789.0, 0.000123456},
                                                      {-0.0, 7.0},   {1e-7, 2.0}, {33.3333333, 99.999999}};
    size_t written = 0;
    for (const char* format : kHits) {
        StyleTemplate t;
        REQUIRE(compiles(format, t));
        for (const std::vector<double>& n : numbers) {
            const std::string text = fill(format, n);
            CAPTURE(text);
            Loaded viaText = load(kStyle, kBody);
            Loaded viaTemplate = load(kStyle, kBody);
            const Status expected = viaText.doc->byId("A").setStyle(text);
            const Status got = viaTemplate.doc->byId("A").setStyleTemplate(t, text);
            if (got == Status::kNotFound) {
                // Outside the template's domain (a negative width): the caller uses setStyle().
                CHECK(viaTemplate.doc->arena().element(viaTemplate.doc->byId("A").node).runtimeDecls.empty());
                continue;
            }
            ++written;
            CHECK(got == expected);
            checkSameDecls(viaText.doc->byId("A"), viaTemplate.doc->byId("A"));
            viaText.step();
            viaTemplate.step();
            CHECK(paintDump(*viaText.doc) == paintDump(*viaTemplate.doc));
        }
    }
    CHECK(written >= 45u);  // Of 54: only the negative width / height / scale ones fall back.
}

TEST_CASE("T-477: a text of another shape is left to setStyle (kNotFound, nothing written)") {
    StyleTemplate t;
    REQUIRE(compiles("transform: translate({}px, {}px)", t));
    Loaded m = load(kStyle, kBody);
    ElementRef a = m.doc->byId("A");
    CHECK(a.setStyleTemplate(t, "transform: translate(5px, 3px) scale(2)") == Status::kNotFound);
    CHECK(a.setStyleTemplate(t, "transform: translate(px, 3px)") == Status::kNotFound);
    CHECK(a.setStyleTemplate(t, "transform: translate(5em, 3px)") == Status::kNotFound);
    CHECK(a.setStyleTemplate(t, "transform: translate(1e99px, 3px)") == Status::kNotFound);  // Not a float.
    CHECK(a.setStyleTemplate(t, "") == Status::kNotFound);
    CHECK(m.doc->arena().element(a.node).runtimeDecls.empty());
    CHECK(a.setStyleTemplate(t, "transform: translate(5px, 3px)") == Status::kOk);
    CHECK_FALSE(m.doc->arena().element(a.node).runtimeDecls.empty());
}

TEST_CASE("T-477: setStyleTemplate keeps setStyle's same-text no-op and its setStyleCalls count") {
    StyleTemplate t;
    REQUIRE(compiles("width: {}px", t));
    Loaded m = load(kStyle, kBody);
    PerfTracker tracker;
    tracker.setEnabled(true);
    m.doc->setPerfTracker(&tracker);
    tracker.beginFrame(0.0f);
    ElementRef a = m.doc->byId("A");
    const uint64_t parses = mini_css::parseDeclarationsCount();
    CHECK(a.setStyleTemplate(t, "width: 30px") == Status::kOk);
    CHECK(a.setStyle("width: 30px") == Status::kOk);  // The same text: setStyle's no-op.
    CHECK(mini_css::parseDeclarationsCount() == parses);
    CHECK(tracker.current().setStyleCalls == 2u);
    m.step();
    CHECK(a.layoutBox().width == doctest::Approx(30.0f));
    // setStyle of another text, then the template's text again: applied again.
    CHECK(a.setStyle("width: 40px") == Status::kOk);
    CHECK(a.setStyleTemplate(t, "width: 30px") == Status::kOk);
    m.step();
    CHECK(a.layoutBox().width == doctest::Approx(30.0f));
    m.doc->setPerfTracker(nullptr);
}

TEST_CASE("T-477: Document::setPort with a Value is observably setPortText of the flow's text") {
    // What the flow sends a declared port: a number printed with 17 digits, `true` / `false`, or
    // nothing for an event; typed, the Value setPortText() would have parsed it to.
    const char* body =
        "<div id=\"P\" port:n=\"number\" port:b=\"bool\" port:who=\"entity\" port:hit=\"event\" "
        "port:c=\"number call\" bind:text=\"n\" on:port(hit)=\"state(Hit)\"></div>";
    Loaded typed = load("", body);
    Loaded text = load("", body);
    struct Log {
        std::vector<std::string> events;
        void attach(Document& doc) {
            doc.addPortListener([this, &doc](const PortEvent& ev) {
                events.push_back(std::to_string(ev.port) + ":" + std::to_string(static_cast<int>(ev.kind)) +
                                 ":" + doc.portText(ev.port));
            });
        }
    };
    Log typedLog;
    Log textLog;
    typedLog.attach(*typed.doc);
    textLog.attach(*text.doc);
    const double numbers[] = {0.1, 0.30000000000000004, -2.5, 1e-30, 123456789.123, 7.0, 0.0, -0.0, 3.0,
                              1e30};
    auto send = [](Document& doc, std::string_view name, const Value* v, std::string_view t) {
        const PortId port = doc.lookupPort(name);
        REQUIRE(port != kInvalidPort);
        if (v != nullptr) {
            CHECK(doc.setPort(port, *v) == Status::kOk);
        } else {
            CHECK(doc.setPortText(port, t) == Status::kOk);
        }
    };
    for (double n : numbers) {
        char buf[64];
        const std::string_view digits(buf, formatFloat(buf, sizeof(buf), n, 17));
        const Value number = Value::ofFloat(static_cast<float>(n));
        send(*typed.doc, "P.n", &number, {});
        send(*text.doc, "P.n", nullptr, digits);
        if (n >= 0.0) {
            const Value entity = Value::ofEnum(toUint32Sat(n));  // As the flow (num_guard.h).
            send(*typed.doc, "P.who", &entity, {});
            send(*text.doc, "P.who", nullptr, digits);
        }
        const bool flag = n > 1.0;
        const Value boolean = Value::ofFloat(flag ? 1.0f : 0.0f);
        send(*typed.doc, "P.b", &boolean, {});
        send(*text.doc, "P.b", nullptr, flag ? "true" : "false");
        const Value zero = Value::ofFloat(0.0f);
        send(*typed.doc, "P.hit", &zero, {});
        send(*text.doc, "P.hit", nullptr, "");
        // A `call` port: the value, then the call.
        const PortId typedCall = typed.doc->lookupPort("P.c");
        const PortId textCall = text.doc->lookupPort("P.c");
        CHECK(typed.doc->setPort(typedCall, number) == Status::kOk);
        CHECK(text.doc->setPortText(textCall, digits) == Status::kOk);
        CHECK(typed.doc->callPort(typedCall, {}).ok());
        CHECK(text.doc->callPort(textCall, {}).ok());
        typed.step();
        text.step();
        for (const std::string name : {"P.n", "P.b", "P.who", "P.c"}) {
            CAPTURE(name);
            const Value a = typed.doc->getPort(typed.doc->lookupPort(name));
            const Value b = text.doc->getPort(text.doc->lookupPort(name));
            CHECK(sameValue(a, b));
        }
        CHECK(typed.doc->byId("P").text() == text.doc->byId("P").text());
        CHECK(typed.doc->hasCustomState(typed.doc->byId("P"), "Hit") ==
              text.doc->hasCustomState(text.doc->byId("P"), "Hit"));
    }
    CHECK(typedLog.events == textLog.events);
    CHECK(paintDump(*typed.doc) == paintDump(*text.doc));
}
