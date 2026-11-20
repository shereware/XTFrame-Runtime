#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/document.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/types.h"

/// T-738 (Docs/design/56-symbol-identity-style-scope.md §56.4): state groups that conflict across
/// origins (a symbol definition or the document) become symbol-scoped (the `STGS` chunk) instead
/// of a compile error; everything that compiled before compiles to the same STAT and no STGS.

using namespace xt;
using namespace xt::compile;

namespace {

std::string page(std::string_view body, std::string_view style = "", std::string_view bodyAttrs = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"Root\" " +
           std::string(bodyAttrs) + ">" + std::string(body) + "</body></xt>";
}

CompileResult compileXt(const std::string& xt, bool strict = false) {
    CompileOptions options;
    options.emitMeta = false;
    options.strict = strict;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasDiag(const CompileResult& r, Diagnostic::Level level, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == level && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

size_t countDiag(const CompileResult& r, std::string_view needle) {
    size_t n = 0;
    for (const Diagnostic& d : r.diagnostics) {
        n += d.message.find(needle) != std::string::npos ? 1u : 0u;
    }
    return n;
}

void requireOk(const CompileResult& r) { test::requireCompiled(r); }

Result<IrView> viewOf(const CompileResult& r) { return IrView::open(IrBlob{r.ir.data(), r.ir.size()}); }

/// The custom-state mask of the group named `group` in `groups`, or 0.
uint32_t maskOf(const IrView& ir, std::span<const IrStateGroup> groups, std::string_view group) {
    for (const IrStateGroup& g : groups) {
        if (ir.string(g.name) == group) {
            return g.mask;
        }
    }
    return 0;
}

uint32_t bitOf(const IrView& ir, std::string_view state) {
    std::span<const uint32_t> names = ir.customStates().names();
    for (uint32_t i = 0; i < names.size(); ++i) {
        if (ir.string(names[i]) == state) {
            return 1u << i;
        }
    }
    return 0;
}

/// The groups of the STGS scope of symbol `symbol` (empty when it has none).
std::span<const IrStateGroup> scopeGroups(const IrView& ir, std::string_view symbol) {
    const StateScopeChunk& s = ir.stateScopes();
    for (const IrStateScope& scope : s.scopes()) {
        if (ir.string(scope.symbolName) == symbol) {
            return s.groups().subspan(scope.groupOffset, scope.groupCount);
        }
    }
    return {};
}

constexpr const char* kTwoAims = R"X(
    <symbol name="SA" state-groups="Aim: Idle|Target"><div id="In"></div></symbol>
    <symbol name="SB" state-groups="Aim: Free|Target"><div id="In"></div></symbol>
    <use symbol="SA" id="A"/><use symbol="SB" id="B"/><div id="D"></div>)X";

struct Ui {
    CompileResult result;
    std::unique_ptr<Document> doc;

    ElementRef el(std::string_view id) const {
        ElementRef e = doc->byId(id);
        REQUIRE(e.valid());
        return e;
    }
    void set(std::string_view id, std::string_view state, bool on = true) {
        REQUIRE(doc->setCustomState(el(id), state, on) == Status::kOk);
    }
    bool has(std::string_view id, std::string_view state) const { return doc->hasCustomState(el(id), state); }
};

Ui open(const std::string& xt) {
    Ui out;
    out.result = compileXt(xt);
    requireOk(out.result);
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 200});
    out.doc->update(0.0f);
    return out;
}

}  // namespace

// -- Compile ------------------------------------------------------------------------------------

TEST_CASE("T-738: the same group name with different members in two symbols is scoped, with notes") {
    CompileResult r = compileXt(page(kTwoAims));
    requireOk(r);
    CHECK(countDiag(r, "is scoped to symbol") == 2);
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();
    CHECK(ir.customStates().groups().empty());  // Neither Aim is global.
    const StateScopeChunk& s = ir.stateScopes();
    REQUIRE(s.present());
    REQUIRE(s.scopes().size() == 2);
    CHECK(maskOf(ir, scopeGroups(ir, "SA"), "Aim") == (bitOf(ir, "Idle") | bitOf(ir, "Target")));
    CHECK(maskOf(ir, scopeGroups(ir, "SB"), "Aim") == (bitOf(ir, "Free") | bitOf(ir, "Target")));
    // One root per instance: A (scope SA) and B (scope SB), ascending.
    REQUIRE(s.roots().size() == 2);
    CHECK(ir.string(ir.doc().elements()[s.roots()[0].element].id) == "A");
    CHECK(ir.string(s.scopes()[s.roots()[0].scope].symbolName) == "SA");
    CHECK(ir.string(ir.doc().elements()[s.roots()[1].element].id) == "B");
    CHECK(ir.string(s.scopes()[s.roots()[1].scope].symbolName) == "SB");
    // Round trip keeps STGS.
    CHECK(IrWriter::fromView(ir).finish() == r.ir);
    // A strict compile still passes: the notes do not count.
    requireOk(compileXt(page(kTwoAims), true));
}

TEST_CASE("T-738: different group names sharing members in two symbols are scoped") {
    CompileResult r = compileXt(page(R"X(
        <symbol name="Hp" state-groups="Level: Normal|Low"><div id="In"></div></symbol>
        <symbol name="Vit" state-groups="Vital: Normal|Low"><div id="In"></div></symbol>
        <use symbol="Hp" id="H"/><use symbol="Vit" id="V"/>)X"));
    requireOk(r);
    CHECK(countDiag(r, "is scoped to symbol") == 2);
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();
    CHECK(ir.customStates().groups().empty());
    CHECK(maskOf(ir, scopeGroups(ir, "Hp"), "Level") != 0);
    CHECK(maskOf(ir, scopeGroups(ir, "Vit"), "Vital") != 0);
}

TEST_CASE("T-738: a conflict inside one origin is still an error") {
    CompileResult inSymbol = compileXt(page(R"X(
        <symbol name="S" state-groups="G: a|b; H: b|c"><div id="In"></div></symbol>
        <use symbol="S" id="X"/>)X"));
    CHECK_FALSE(inSymbol.ok());
    CHECK(hasDiag(inSymbol, Diagnostic::Level::Error, "is in two state groups"));

    CompileResult inSymbolBody = compileXt(page(R"X(
        <symbol name="S" state-groups="G: a|b"><div id="In" state-groups="G: a|c"></div></symbol>
        <use symbol="S" id="X"/>)X"));
    CHECK_FALSE(inSymbolBody.ok());
    CHECK(hasDiag(inSymbolBody, Diagnostic::Level::Error, "is declared with different members"));

    CompileResult inDocument = compileXt(
        page(R"X(<div id="P" state-groups="G: a|b"></div><div id="Q" state-groups="G: a|c"></div>)X"));
    CHECK_FALSE(inDocument.ok());
    CHECK(hasDiag(inDocument, Diagnostic::Level::Error, "is declared with different members"));

    CompileResult shared = compileXt(
        page(R"X(<div id="P" state-groups="G: a|b"></div><div id="Q" state-groups="H: b|c"></div>)X"));
    CHECK_FALSE(shared.ok());
    CHECK(hasDiag(shared, Diagnostic::Level::Error, "is in two state groups"));
}

TEST_CASE("T-738: a symbol group conflicting with a document group is scoped; the document's stays global") {
    CompileResult r = compileXt(page(R"X(
        <symbol name="Vit" state-groups="Vital: Normal|Low"><div id="In"></div></symbol>
        <use symbol="Vit" id="V"/><div id="D"></div>)X",
                                     "", R"X(state-groups="Level: Normal|Low")X"));
    requireOk(r);
    CHECK(countDiag(r, "is scoped to symbol") == 1);
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();
    REQUIRE(ir.customStates().groups().size() == 1);
    CHECK(ir.string(ir.customStates().groups()[0].name) == "Level");
    REQUIRE(ir.stateScopes().scopes().size() == 1);
    CHECK(maskOf(ir, scopeGroups(ir, "Vit"), "Vital") == (bitOf(ir, "Normal") | bitOf(ir, "Low")));
}

TEST_CASE("T-738: identical or disjoint symbol groups stay global and write no STGS") {
    CompileResult same = compileXt(page(R"X(
        <symbol name="SA" state-groups="Aim: Idle|Target"><div id="In"></div></symbol>
        <symbol name="SC" state-groups="Aim: Target|Idle"><div id="In"></div></symbol>
        <use symbol="SA" id="A"/><use symbol="SC" id="C"/>)X"));
    requireOk(same);
    CHECK(countDiag(same, "is scoped to symbol") == 0);
    Result<IrView> a = viewOf(same);
    REQUIRE(a.ok());
    CHECK_FALSE(a.value().stateScopes().present());
    REQUIRE(a.value().customStates().groups().size() == 1);

    CompileResult disjoint = compileXt(page(R"X(
        <symbol name="SA" state-groups="Aim: Idle|Target"><div id="In"></div></symbol>
        <symbol name="SD" state-groups="Tone: Info|Warn"><div id="In"></div></symbol>
        <use symbol="SA" id="A"/><use symbol="SD" id="E"/>)X"));
    requireOk(disjoint);
    Result<IrView> b = viewOf(disjoint);
    REQUIRE(b.ok());
    CHECK_FALSE(b.value().stateScopes().present());
    CHECK(b.value().customStates().groups().size() == 2);
}

TEST_CASE("T-738: more than 24 custom state names is a warning naming each and where it comes from") {
    std::string groups = "G: ";
    for (int i = 0; i < 25; ++i) {
        groups += (i == 0 ? "" : "|") + std::string("S") + std::to_string(i);
    }
    CompileResult r =
        compileXt(page(R"X(<symbol name="Sym" state-groups="K: Q1|Q2"><div id="In"></div></symbol>
        <use symbol="Sym" id="U"/><div id="A" state-groups=")X" +
                       groups + "\"></div>"));
    requireOk(r);
    CHECK(hasDiag(r, Diagnostic::Level::Warning, "27 custom state names"));
    CHECK(hasDiag(r, Diagnostic::Level::Warning, "Q1 (symbol \"Sym\")"));
    CHECK(hasDiag(r, Diagnostic::Level::Warning, "S24 (document)"));
    CompileResult few = compileXt(page(R"X(<div id="A" state-groups="G: a|b"></div>)X"));
    requireOk(few);
    CHECK(countDiag(few, "custom state names") == 0);
}

// -- Runtime ------------------------------------------------------------------------------------

TEST_CASE("T-738: setting a state clears only the group of the element's symbol scope") {
    Ui ui = open(page(kTwoAims));
    // A: SA's Aim (Idle|Target).
    ui.set("A", "Idle");
    ui.set("A", "Target");
    CHECK_FALSE(ui.has("A", "Idle"));
    ui.set("A", "Free");  // Not in SA's groups, no global group: nothing cleared.
    CHECK(ui.has("A", "Target"));
    CHECK(ui.has("A", "Free"));
    // B: SB's Aim (Free|Target).
    ui.set("B", "Free");
    ui.set("B", "Idle");
    ui.set("B", "Target");
    CHECK_FALSE(ui.has("B", "Free"));
    CHECK(ui.has("B", "Idle"));
    // An element inside an instance uses that instance's scope.
    ui.set("A/In", "Idle");
    ui.set("A/In", "Target");
    CHECK_FALSE(ui.has("A/In", "Idle"));
    ui.set("B/In", "Free");
    ui.set("B/In", "Target");
    CHECK_FALSE(ui.has("B/In", "Free"));
    // Outside every instance: STAT (empty here) -- no exclusivity.
    ui.set("D", "Idle");
    ui.set("D", "Target");
    CHECK(ui.has("D", "Idle"));
    CHECK(ui.has("D", "Target"));
    // Off and Toggle unchanged.
    ui.set("A", "Target", false);
    CHECK_FALSE(ui.has("A", "Target"));
}

TEST_CASE("T-738: a state not in the scope falls back to the global groups") {
    Ui ui = open(page(R"X(
        <symbol name="Vit" state-groups="Vital: Normal|Low"><div id="In"></div></symbol>
        <symbol name="SA" state-groups="Aim: Idle|Target"><div id="In"></div></symbol>
        <symbol name="SD" state-groups="Tone: Info|Warn"><div id="In"></div></symbol>
        <use symbol="Vit" id="V"/><use symbol="SA" id="A"/><use symbol="SD" id="E"/><div id="D"></div>)X",
                      "", R"X(state-groups="Level: Normal|Low|Dead")X"));
    // V: Vital is scoped (it conflicts with the document's Level) -- Normal / Low exclude each other
    // there, and Dead (not in Vital) falls back to Level.
    ui.set("V", "Normal");
    ui.set("V", "Low");
    CHECK_FALSE(ui.has("V", "Normal"));
    ui.set("V", "Dead");
    CHECK_FALSE(ui.has("V", "Low"));
    // A: SA's groups are global (no conflict); Normal / Low use the document's Level.
    ui.set("A", "Normal");
    ui.set("A", "Dead");
    CHECK_FALSE(ui.has("A", "Normal"));
    // A non-conflicting symbol group keeps working for document elements, as before.
    ui.set("D", "Info");
    ui.set("D", "Warn");
    CHECK_FALSE(ui.has("D", "Info"));
    ui.set("D", "Idle");
    ui.set("D", "Target");
    CHECK_FALSE(ui.has("D", "Idle"));
}

TEST_CASE("T-738: nested instances look outwards for the first scope holding the state") {
    Ui ui = open(page(R"X(
        <symbol name="SB" state-groups="Aim: Free|Target"><div id="In"></div></symbol>
        <symbol name="SN" state-groups="Aim: Idle|Target"><div id="Body"><use symbol="SB" id="Inner"/></div></symbol>
        <use symbol="SN" id="N"/>)X"));
    // N's own elements: SN's Aim.
    ui.set("N", "Idle");
    ui.set("N", "Target");
    CHECK_FALSE(ui.has("N", "Idle"));
    ui.set("N/Body", "Idle");
    ui.set("N/Body", "Target");
    CHECK_FALSE(ui.has("N/Body", "Idle"));
    // N/Inner (an SB root): Target is in SB's Aim -> only Free is cleared.
    ui.set("N/Inner", "Idle");
    ui.set("N/Inner", "Free");
    ui.set("N/Inner", "Target");
    CHECK_FALSE(ui.has("N/Inner", "Free"));
    CHECK(ui.has("N/Inner", "Idle"));
    // Idle is not in SB's groups: the next scope out (SN) clears Target.
    ui.set("N/Inner/In", "Target");
    ui.set("N/Inner/In", "Idle");
    CHECK_FALSE(ui.has("N/Inner/In", "Target"));
}

TEST_CASE("T-738: instances made from a <repeat> template use their template's scope") {
    Ui ui = open(page(R"X(
        <symbol name="SA" state-groups="Aim: Idle|Target"><div id="In"></div></symbol>
        <symbol name="SB" state-groups="Aim: Free|Target"><div id="In"></div></symbol>
        <use symbol="SB" id="B"/>
        <div id="P" port:items="list<struct{name: string}>"></div>
        <repeat port="items" id="Item"><use symbol="SA" id="Cell"/></repeat>)X"));
    PortId items = ui.doc->lookupPort("items");
    REQUIRE(items != kInvalidPort);
    REQUIRE(ui.doc->setPortText(items, R"([{"name":"a"},{"name":"b"}])") == Status::kOk);
    ui.doc->update(0.0f);
    REQUIRE(ui.doc->repeatInstances("Item").size() == 2);
    ui.set("Item/1/Cell", "Idle");
    ui.set("Item/1/Cell", "Free");
    ui.set("Item/1/Cell", "Target");
    CHECK_FALSE(ui.has("Item/1/Cell", "Idle"));  // SA's Aim.
    CHECK(ui.has("Item/1/Cell", "Free"));        // Not SB's.
    ui.set("Item/0/Cell/In", "Idle");
    ui.set("Item/0/Cell/In", "Target");
    CHECK_FALSE(ui.has("Item/0/Cell/In", "Idle"));
}

TEST_CASE("T-738: state-enter / state-exit and trigger state actions follow the scoped groups") {
    Ui ui = open(page(R"X(
        <symbol name="SA" state-groups="Aim: Idle|Target"><div id="In"></div></symbol>
        <symbol name="SB" state-groups="Aim: Free|Target"><div id="In"></div></symbol>
        <use symbol="SA" id="A" on:state(Idle).exit="class(idle-out)" on:state(Target).enter="class(on)"/>
        <use symbol="SB" id="B" on:click="state(Target)"/>)X",
                      "#B { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px; }"));
    ui.set("A", "Idle");
    ui.set("A", "Target");
    CHECK(ui.el("A").hasClass("idle-out"));
    CHECK(ui.el("A").hasClass("on"));
    ui.set("B", "Free");
    ui.set("B", "Idle");
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{20, 20};
    down.button = PointerButton::Left;
    ui.doc->dispatchInput(down);
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    ui.doc->dispatchInput(up);
    CHECK(ui.has("B", "Target"));
    CHECK_FALSE(ui.has("B", "Free"));
    CHECK(ui.has("B", "Idle"));
}

// -- IR validation ------------------------------------------------------------------------------

TEST_CASE("T-738: IrView rejects a broken STGS chunk") {
    CompileResult r = compileXt(page(kTwoAims));
    requireOk(r);
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    const StateScopeChunk& s = opened.value().stateScopes();
    REQUIRE(s.scopes().size() == 2);
    REQUIRE(s.groups().size() == 2);
    REQUIRE(s.roots().size() == 2);
    auto offsetOf = [&](const void* p) {
        return static_cast<size_t>(reinterpret_cast<const uint8_t*>(p) - r.ir.data());
    };
    auto opens = [&](auto mutate) {
        std::vector<uint8_t> bytes = r.ir;
        mutate(bytes);
        return IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{/*verifyCrc=*/false}).ok();
    };
    auto putU32 = [](std::vector<uint8_t>& bytes, size_t at, uint32_t v) { std::memcpy(&bytes[at], &v, 4); };
    const size_t scope0 = offsetOf(&s.scopes()[0]);
    const size_t group0 = offsetOf(&s.groups()[0]);
    const size_t group1 = offsetOf(&s.groups()[1]);
    const size_t root0 = offsetOf(&s.roots()[0]);
    const size_t root1 = offsetOf(&s.roots()[1]);
    CHECK(opens([](std::vector<uint8_t>&) {}));
    // Scope: name string id, group range.
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, scope0, 0xFFFFFFu); }));
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, scope0 + 4, 2); }));
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, scope0 + 8, 3); }));
    // Group: name string id; mask with a bit past STAT's names.
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, group0, 0xFFFFFFu); }));
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, group0 + 4, 0x80000000u); }));
    // Two groups of one scope overlapping: put both groups in scope 0.
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) {
        putU32(b, scope0 + 4, 0);
        putU32(b, scope0 + 8, 2);
    }));
    (void)group1;
    // Root: element range, scope range, ascending order.
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, root0, 0xFFFFFFu); }));
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) { putU32(b, root0 + 4, 2); }));
    CHECK_FALSE(opens([&](std::vector<uint8_t>& b) {
        uint32_t e1 = 0;
        std::memcpy(&e1, &b[root1], 4);
        putU32(b, root0, e1);  // A duplicate (not ascending).
    }));
}

TEST_CASE("T-738: a second STGS chunk is rejected") {
    CompileResult r = compileXt(page(kTwoAims));
    requireOk(r);
    // The STGS chunk is the writer's last: append a copy of it and bump the header counts.
    Result<IrView> opened = viewOf(r);
    REQUIRE(opened.ok());
    size_t at = sizeof(IrHeader);
    size_t stgs = 0;
    size_t stgsLen = 0;
    while (at + sizeof(IrChunkHeader) <= r.ir.size()) {
        IrChunkHeader ch{};
        std::memcpy(&ch, &r.ir[at], sizeof(ch));
        if (std::memcmp(ch.type, "STGS", 4) == 0) {
            stgs = at;
            stgsLen = sizeof(IrChunkHeader) + ch.payloadLength;
        }
        at += sizeof(IrChunkHeader) + ch.payloadLength;
    }
    REQUIRE(stgsLen != 0);
    std::vector<uint8_t> bytes = r.ir;
    bytes.insert(bytes.end(), r.ir.begin() + static_cast<std::ptrdiff_t>(stgs),
                 r.ir.begin() + static_cast<std::ptrdiff_t>(stgs + stgsLen));
    IrHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    header.chunkCount += 1;
    header.totalLength = static_cast<uint32_t>(bytes.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    CHECK_FALSE(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{false}).ok());
}
