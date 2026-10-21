#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/lottie_parser.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/xtfx.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/text_shaper.h"

/// T-257 (docs/tasks/T-257.md): hostile nesting in the compiler's parsers ends in a diagnostic,
/// never in a stack overflow -- the `.xt` parser (D1), a `<symbol>` dropped by `<glyph>` (D2), the
/// HLSL subset, Lottie shape groups and Lottie parent chains (D5).

using namespace xt;
using namespace xt::compile;

namespace {

int countErrorsContaining(const std::vector<Diagnostic>& diagnostics, std::string_view needle) {
    int n = 0;
    for (const Diagnostic& d : diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            ++n;
        }
    }
    return n;
}

std::string repeat(std::string_view text, int count) {
    std::string out;
    out.reserve(text.size() * static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        out += text;
    }
    return out;
}

/// `<xt><body>` + `depth` nested `<div>`s, all closed.
std::string nestedDivs(int depth) {
    return "<xt><body>" + repeat("<div>", depth) + repeat("</div>", depth) + "</body></xt>";
}

XtfxResult compileBody(const std::string& body) { return compileXtfx("param k float 1\n---\n" + body); }

const char* const kLottieTransform =
    R"("ks":{"a":{"a":0,"k":[0,0,0]},"p":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]},)"
    R"("r":{"a":0,"k":0},"o":{"a":0,"k":100}})";

const char* const kGroupTransform =
    R"({"ty":"tr","p":{"a":0,"k":[0,0]},"a":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},)"
    R"("r":{"a":0,"k":0},"o":{"a":0,"k":100}})";

std::string lottieDoc(const std::string& layers) {
    return R"({"v":"5.6.5","fr":30,"ip":0,"op":60,"w":100,"h":100,"layers":[)" + layers + "]}";
}

/// One shape layer whose content is `depth` nested groups around a rectangle.
std::string nestedGroupsLottie(int depth) {
    std::string shape =
        R"({"ty":"rc","nm":"r","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[10,10]},"r":{"a":0,"k":0}})";
    for (int i = 0; i < depth; ++i) {
        shape = R"({"ty":"gr","nm":"g","it":[)" + shape + "," + kGroupTransform + "]}";
    }
    return lottieDoc(R"({"ty":4,"ind":0,"nm":"S","ip":0,"op":60,"st":0,"sr":1,)" +
                     std::string(kLottieTransform) + R"(,"shapes":[)" + shape + "]}");
}

/// A null layer with index `ind` and (when >= 0) parent `parent`.
std::string nullLayer(int ind, int parent) {
    std::string layer =
        R"({"ty":3,"ind":)" + std::to_string(ind) + R"(,"nm":"L)" + std::to_string(ind) + "\"";
    if (parent >= 0) {
        layer += R"(,"parent":)" + std::to_string(parent);
    }
    return layer + R"(,"ip":0,"op":60,"st":0,"sr":1,)" + kLottieTransform + "}";
}

/// `count` null layers in array order, layer i parented to layer i-1: the last one (built first)
/// has a parent chain `count` layers long.
std::string parentChainLottie(int count) {
    std::string layers;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            layers += ",";
        }
        layers += nullLayer(i, i - 1);
    }
    return lottieDoc(layers);
}

struct LottieRun {
    LottieResult result;
    DiagnosticSink sink;
};

LottieRun runLottie(const std::string& json) {
    LottieRun run;
    IrWriter writer;
    LottieOptions options;
    options.sourceName = "deep.json";
    run.result =
        parseLottie(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json.data()), json.size()),
                    options, writer, run.sink);
    return run;
}

}  // namespace

TEST_CASE("T-257 D1: 20000 nested elements stop at the depth limit with one error") {
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(nestedDivs(20000), "<mem>", sink, tree));
    CHECK(countErrorsContaining(sink.all(), "maximum nesting depth (256) exceeded") == 1);
}

TEST_CASE("T-257 D1: deep content of an illegal <head> tag stops at the same limit") {
    const std::string text =
        "<xt><head><foo>" + repeat("<div>", 20000) + repeat("</div>", 20000) + "</foo></head><body/></xt>";
    DiagnosticSink sink;
    DomTree tree;
    CHECK_FALSE(parseXt(text, "<mem>", sink, tree));
    CHECK(countErrorsContaining(sink.all(), "maximum nesting depth (256) exceeded") == 1);
}

TEST_CASE("T-257 D1: nesting up to the limit still parses") {
    // <xt> is depth 1 and <body> depth 2, so 254 <div>s reach exactly 256.
    DiagnosticSink sink;
    DomTree tree;
    CHECK(parseXt(nestedDivs(254), "<mem>", sink, tree));
    CHECK_FALSE(sink.hasErrors());
    DiagnosticSink deeper;
    CHECK_FALSE(parseXt(nestedDivs(255), "<mem>", deeper, tree));
    CHECK(countErrorsContaining(deeper.all(), "maximum nesting depth") == 1);
}

TEST_CASE("T-257 D2: a <symbol> inside <glyph> leaves no stale symbolDefs index") {
    constexpr std::string_view kDoc = R"(<xt><body><glyph key="a"><symbol name="S"/></glyph></body></xt>)";
    DiagnosticSink sink;
    DomTree tree;
    parseXt(kDoc, "doc.xt", sink, tree);
    for (uint32_t idx : tree.symbolDefs) {
        CHECK(idx < tree.nodes.size());
    }

    std::array<SourceFile, 1> files{SourceFile{"doc.xt", kDoc}};
    const CompileResult result = compileDocumentFromStrings(files, "doc.xt", CompileOptions{});
    CHECK_FALSE(result.diagnostics.empty());  // at least "<glyph> content ignored"
    CHECK(countErrorsContaining(result.diagnostics, "<symbol> requires a name") == 0);
}

TEST_CASE("T-257 D2: a <symbol> dropped with <glyph> content inside a symbol definition is not registered") {
    // A symbol definition holding a glyph that wraps another symbol: only the outer one survives,
    // and the dropped one's index (reused by <body>'s nodes) is not taken for a definition.
    constexpr std::string_view kDoc =
        R"(<xt><head><symbol name="Outer"><div><glyph key="g"><symbol name="Inner"/></glyph></div></symbol>)"
        R"(</head><body><use symbol="Outer"/></body></xt>)";
    DiagnosticSink sink;
    DomTree tree;
    parseXt(kDoc, "doc.xt", sink, tree);
    REQUIRE(tree.symbolDefs.size() == 1);
    REQUIRE(tree.symbolDefs[0] < tree.nodes.size());
    CHECK(tree.nodes[tree.symbolDefs[0]].tag == "symbol");
}

TEST_CASE("T-257 D5: 10000 nested parentheses in an .xtfx body are a nesting error") {
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nreturn float4(" + repeat("(", 10000) +
                                     "1.0" + repeat(")", 10000) + ", 0, 0, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.message == "expression nesting too deep (max 64)");
}

TEST_CASE("T-257 D5: 10000 unary minus signs are a nesting error") {
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nfloat x = " + repeat("- ", 10000) +
                                     "1.0;\nreturn float4(x, 0, 0, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.message == "expression nesting too deep (max 64)");
}

TEST_CASE("T-257 D5: 10000 nested blocks in an .xtfx body are a nesting error") {
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\n" + repeat("{", 10000) +
                                     repeat("}", 10000) + "\nreturn float4(1, 1, 1, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.message == "block nesting too deep (max 64)");
}

TEST_CASE("T-257: a 10000-term operator chain in an .xtfx body is too long") {
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nfloat x = 1" + repeat(" + 1", 9999) +
                                     ";\nreturn float4(x, 0, 0, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.message == "expression too long (max 256 terms)");
}

TEST_CASE("T-257: a 10000-step swizzle chain in an .xtfx body is too long") {
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nfloat2 v = i.uv" + repeat(".xy", 10000) +
                                     ";\nreturn float4(v, 0, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.message == "expression too long (max 256 terms)");
}

TEST_CASE("T-257: 100-term chains in an .xtfx body still translate") {
    const XtfxResult r =
        compileBody("float4 xtMain(XtInput i) {\nfloat x = 1.0" + repeat(" + 1.0", 99) +
                    ";\nfloat2 v = i.uv" + repeat(".xy", 99) + ";\nreturn float4(v, x, 1);\n}\n");
    CHECK_MESSAGE(r.ok, r.error.message);
}

TEST_CASE("T-257: a 200-term chain of parenthesised 200-term chains is too deep") {
    // Each chain stays under 256 terms and the parentheses nest one level only; the tree does not.
    const std::string inner = "(1.0" + repeat(" + 1.0", 199) + ")";
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nfloat x = " + inner +
                                     repeat(" + " + inner, 199) + ";\nreturn float4(x, 0, 0, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.message == "expression too deep (max 256 levels)");
}

TEST_CASE("T-257: three levels of moderate chains still translate") {
    const std::string term = "(1.0" + repeat(" * 2.0", 9) + ")";
    const std::string middle = "(" + term + repeat(" - " + term, 9) + ")";
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nfloat x = " + middle +
                                     repeat(" + " + middle, 9) + ";\nreturn float4(x, 0, 0, 1);\n}\n");
    CHECK_MESSAGE(r.ok, r.error.message);
}

TEST_CASE("T-257 D5: moderate nesting in an .xtfx body still translates") {
    const XtfxResult r =
        compileBody("float4 xtMain(XtInput i) {\n" + repeat("{", 30) + "float x = " + repeat("(", 30) +
                    "-1.0" + repeat(")", 30) + ";" + repeat("}", 30) + "\nreturn float4(1, 1, 1, 1);\n}\n");
    CHECK_MESSAGE(r.ok, r.error.message);
}

TEST_CASE("T-257 D5: Lottie shape groups nested 100 deep fail the import with an error") {
    LottieRun run = runLottie(nestedGroupsLottie(100));
    CHECK_FALSE(run.result.ok);
    CHECK(countErrorsContaining(run.sink.all(), "shape group nesting too deep (max 64)") == 1);

    LottieRun atLimit = runLottie(nestedGroupsLottie(64));
    CHECK(atLimit.result.ok);
    CHECK_FALSE(atLimit.sink.hasErrors());
    LottieRun overLimit = runLottie(nestedGroupsLottie(65));
    CHECK_FALSE(overLimit.result.ok);
}

TEST_CASE("T-257 D5: a Lottie parent cycle through three layers is an error") {
    LottieRun run = runLottie(lottieDoc(nullLayer(0, 2) + "," + nullLayer(1, 0) + "," + nullLayer(2, 1)));
    CHECK_FALSE(run.result.ok);
    CHECK(countErrorsContaining(run.sink.all(), "parent cycle detected") >= 1);

    LottieRun self = runLottie(lottieDoc(nullLayer(0, 0)));
    CHECK_FALSE(self.result.ok);
    CHECK(countErrorsContaining(self.sink.all(), "parent cycle detected") == 1);
}

TEST_CASE("T-257 D5: a Lottie parent chain 300 layers long is an error; 200 is fine") {
    LottieRun run = runLottie(parentChainLottie(300));
    CHECK_FALSE(run.result.ok);
    CHECK(countErrorsContaining(run.sink.all(), "parent chain longer than 256 layers") == 1);

    LottieRun ok = runLottie(parentChainLottie(200));
    CHECK(ok.result.ok);
    CHECK_FALSE(ok.sink.hasErrors());
}

// T-257b (docs/tasks/T-257b.md): every limit above is also run end to end with an input exactly at
// it, in Debug on the test thread's default stack: what the parser lets through, every later
// stage (symbol expansion, emit, GLSL, Lottie build, IR writing, load, update, destruction) must
// survive too.

namespace {

/// Loads `ir` into a Context (IrView validation included) and runs one update.
void loadAndUpdate(const std::vector<uint8_t>& ir) {
    NullTextShaper shaper;
    Context ctx{ContextConfig{&shaper, nullptr}};
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, true);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
    ctx.update(0.0f);
}

void checkNoErrors(const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& d : diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
}

/// compileLottie() on `json`, written to a temp file named `name`.
CompileResult compileLottieText(const std::string& json, const std::string& name) {
    const std::filesystem::path dir = xt::test::tempDir() / "xt_t257b";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / name;
    {
        std::ofstream out(path, std::ios::binary);
        out << json;
    }
    return compileLottie(path.generic_string(), CompileOptions{});
}

std::string xtfxChain(int terms) { return "1.0" + repeat(" + 1.0", terms - 1); }

}  // namespace

TEST_CASE("T-257b: an .xt nested exactly kMaxXtDepth deep compiles, loads and updates") {
    // <xt> is depth 1 and <body> depth 2, so kMaxXtDepth - 2 <div>s reach the limit.
    const int divs = kMaxXtDepth - 2;
    const std::string text = "<xt><head><style>div { padding: 1px; }</style></head><body>" +
                             repeat("<div class=\"d\">", divs) + "deep" + repeat("</div>", divs) +
                             "</body></xt>";
    std::array<SourceFile, 1> files{SourceFile{"deep.xt", text}};
    const CompileResult result = compileDocumentFromStrings(files, "deep.xt", CompileOptions{});
    checkNoErrors(result.diagnostics);
    REQUIRE(result.ok());
    loadAndUpdate(result.ir);
}

TEST_CASE("T-257b: a kMaxXtDepth-deep symbol used two levels deep compiles or is a depth error") {
    // <xt> <head> <symbol> are depths 1-3, so kMaxXtDepth - 3 <div>s in the symbol reach the limit;
    // expanded through two <use>s that tree is deeper than the runtime allows (T-255's diagnostic).
    // 240 <div>s expand to a tree the runtime still takes.
    auto run = [](int divs) {
        CAPTURE(divs);
        const std::string text = "<xt><head><symbol name=\"Inner\">" + repeat("<div>", divs) + "deep" +
                                 repeat("</div>", divs) +
                                 "</symbol><symbol name=\"Outer\"><div><use symbol=\"Inner\" id=\"i\"/></div>"
                                 "</symbol></head><body><use symbol=\"Outer\" id=\"o\"/></body></xt>";
        std::array<SourceFile, 1> files{SourceFile{"deep.xt", text}};
        const CompileResult result = compileDocumentFromStrings(files, "deep.xt", CompileOptions{});
        if (result.ok()) {
            loadAndUpdate(result.ir);
        } else {
            CHECK(countErrorsContaining(result.diagnostics,
                                        "element nesting too deep after symbol expansion") == 1);
        }
        return result.ok();
    };
    CHECK_FALSE(run(kMaxXtDepth - 3));
    CHECK(run(240));
}

TEST_CASE("T-257b: an .xtfx expression tree exactly 256 deep translates") {
    // A 256-term chain is 256 levels (255 binary nodes over a leaf); one unary minus more is 257.
    const XtfxResult r = compileBody("float4 xtMain(XtInput i) {\nfloat x = " + xtfxChain(256) +
                                     ";\nreturn float4(x, 0, 0, 1);\n}\n");
    CHECK_MESSAGE(r.ok, r.error.message);
    CHECK_FALSE(r.glsl.empty());
    const XtfxResult over = compileBody("float4 xtMain(XtInput i) {\nfloat x = -(" + xtfxChain(256) +
                                        ");\nreturn float4(x, 0, 0, 1);\n}\n");
    CHECK_FALSE(over.ok);
    CHECK(over.error.message == "expression too deep (max 256 levels)");
}

TEST_CASE("T-257b: .xtfx parentheses and blocks nested exactly 64 deep translate") {
    // The statement and its right-hand side are two expression levels and the function body is
    // one block level, so these reach exactly 64 of each.
    constexpr int kParens = 62;
    constexpr int kBlocks = 63;
    auto body = [](int parens, int blocks) {
        return "float4 xtMain(XtInput i) {\nfloat y = 0.0;\n" + repeat("{", blocks) +
               "y = " + repeat("(", parens) + "1.0" + repeat(")", parens) + ";" + repeat("}", blocks) +
               "\nreturn float4(y, 0, 0, 1);\n}\n";
    };
    const XtfxResult r = compileBody(body(kParens, kBlocks));
    CHECK_MESSAGE(r.ok, r.error.message);
    CHECK_FALSE(r.glsl.empty());
    const XtfxResult moreParens = compileBody(body(kParens + 1, kBlocks));
    CHECK(moreParens.error.message == "expression nesting too deep (max 64)");
    const XtfxResult moreBlocks = compileBody(body(kParens, kBlocks + 1));
    CHECK(moreBlocks.error.message == "block nesting too deep (max 64)");
}

TEST_CASE("T-257b: Lottie shape groups nested 64 deep build and write the IR") {
    const CompileResult result = compileLottieText(nestedGroupsLottie(64), "groups64.json");
    checkNoErrors(result.diagnostics);
    CHECK(result.ok());
}

TEST_CASE("T-257b: a Lottie parent chain exactly 256 layers long builds and writes the IR") {
    const CompileResult result = compileLottieText(parentChainLottie(256), "chain256.json");
    checkNoErrors(result.diagnostics);
    CHECK(result.ok());
    const CompileResult over = compileLottieText(parentChainLottie(257), "chain257.json");
    CHECK_FALSE(over.ok());
    CHECK(countErrorsContaining(over.diagnostics, "parent chain longer than 256 layers") == 1);
}
