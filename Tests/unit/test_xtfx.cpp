#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/command_simplifier.h"
#include "xt/compile/compile.h"
#include "xt/compile/xtfx.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/property.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"

/// T-203 (Docs/design/18-effects.md §18.7, §18.4, §18.12): the `.xtfx` header, the HLSL subset
/// (one allowed and one rejected example per rule), the GLSL translation table, the loop cap,
/// error positions, the compiler's SHDR/ASST output, Document::shaderRef() and the SW fallback.

using namespace xt;
using namespace xt::compile;
namespace fs = std::filesystem;

namespace {

const char* const kRipple =
    "// header\n"
    "name: Ripple\n"
    "param amplitude float 4 [0, 32]\n"
    "param speed float 1.5 [0, 10]\n"
    "param tint color #ffffff\n"
    "fallback: blur(2px)\n"
    "expand: 8px\n"
    "---\n"
    "float4 xtMain(XtInput i)\n"
    "{\n"
    "    float2 uv = i.uv + sin(i.uv.yx * 40 + xtTime * speed) * (amplitude * i.texelSize);\n"
    "    return xtSample(uv) * tint;\n"
    "}\n";

/// A header with one float `k`, then `body` (the whole xtMain definition).
std::string withBody(const std::string& body) { return "param k float 1\n---\n" + body; }

/// Wraps `stmts` into `float4 xtMain(XtInput i) { <stmts> return float4(1, 1, 1, 1); }`.
std::string mainWith(const std::string& stmts) {
    return withBody("float4 xtMain(XtInput i) {\n" + stmts + "\nreturn float4(1, 1, 1, 1);\n}\n");
}

XtfxResult run(const std::string& text) { return compileXtfx(text); }

bool accepts(const std::string& stmts) {
    XtfxResult r = run(mainWith(stmts));
    if (!r.ok) {
        MESSAGE(stmts << " -> " << r.error.message);
    }
    return r.ok;
}

bool rejects(const std::string& stmts) { return !run(mainWith(stmts)).ok; }

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

fs::path freshTempDir(const char* name) {
    fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

CompileResult compileWithAsset(const char* dirName, const std::string& xt, const std::string& xtfxName,
                               const std::string& xtfx) {
    fs::path dir = freshTempDir(dirName);
    writeText(dir / xtfxName, xtfx);
    SourceFile files[1] = {SourceFile{"main.xt", xt}};
    CompileOptions options;
    options.rootDir = dir.string();
    options.emitMeta = false;
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasError(const CompileResult& r, const std::string& needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && contains(d.message, needle)) {
            return true;
        }
    }
    return false;
}

class NoAssets : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData&) override { return false; }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{0, 0}; }
};

}  // namespace

// -------------------------------------------------------------------------------------------
// Header
// -------------------------------------------------------------------------------------------

TEST_CASE("xtfx header: every field of the 18.7 example") {
    XtfxResult r = run(kRipple);
    REQUIRE_MESSAGE(r.ok, r.error.message);
    CHECK(r.header.name == "Ripple");
    REQUIRE(r.header.params.size() == 3);
    CHECK(r.header.params[0].name == "amplitude");
    CHECK(r.header.params[0].type == 0);
    CHECK(r.header.params[0].def[0] == doctest::Approx(4));
    CHECK(r.header.params[0].hasRange);
    CHECK(r.header.params[0].min == doctest::Approx(0));
    CHECK(r.header.params[0].max == doctest::Approx(32));
    CHECK(r.header.params[0].slot == 0);
    CHECK(r.header.params[1].slot == 1);
    CHECK(r.header.params[2].type == 2);
    CHECK(r.header.params[2].slot == 0);
    CHECK(r.header.params[2].def[3] == doctest::Approx(1));
    CHECK_FALSE(r.header.params[2].hasRange);
    CHECK(r.header.fallback.kind == EffectKind::Blur);
    CHECK(r.header.fallback.params[0] == doctest::Approx(2));
    CHECK(r.header.expandPx == doctest::Approx(8));
    CHECK(contains(r.source, "float4 xtMain(XtInput i)"));
    CHECK(r.source.rfind("float4", 0) == 0);  // body starts right after the `---` line
}

TEST_CASE("xtfx header: vec2 params, fallback none, no name") {
    XtfxResult r =
        run("param center vec2 0.5 0.25 [0, 1]\nparam k float 2\nfallback: none\n---\n"
            "float4 xtMain(XtInput i) { return xtSample(center + k * i.texelSize); }\n");
    REQUIRE_MESSAGE(r.ok, r.error.message);
    CHECK(r.header.name.empty());
    REQUIRE(r.header.params.size() == 2);
    CHECK(r.header.params[0].type == 1);
    CHECK(r.header.params[0].def[1] == doctest::Approx(0.25));
    CHECK(r.header.params[0].slot == 0);
    CHECK(r.header.params[1].slot == 2);  // a vec2 takes two float slots
    CHECK(r.header.fallback.kind == EffectKind::None);
    CHECK(r.header.expandPx == 0);
    uint32_t index = 0;
    CHECK(xtfxParamIndex(r.header.params, "center", index));
    CHECK(index == prop::kEffectParamVec2Base);  // T-534: the whole vec2 from slot 0
    CHECK(xtfxParamIndex(r.header.params, "k", index));
    CHECK(index == 3);
    CHECK(xtfxParamIndex(r.header.params, "enabled", index));
    CHECK(index == prop::kEffectParamEnabled);
    CHECK_FALSE(xtfxParamIndex(r.header.params, "nope", index));
}

TEST_CASE("xtfx header: errors carry the offending line") {
    struct Case {
        const char* text;
        int line;
        const char* message;
    };
    const Case cases[] = {
        {"name: A\nwhat: 1\n---\n", 2, "unknown key"},
        {"name: A\n", 2, "'---'"},  // the body's first line is not a header line
        {"param a float 1\nparam a float 2\n---\n", 2, "duplicate"},
        {"param a double 1\n---\n", 1, "unknown type"},
        {"param a float\n---\n", 1, "expected"},
        {"param a color #fff [0, 1]\n---\n", 1, "no range"},
        {"param a float 1 [2, 1]\n---\n", 1, "min must be below max"},
        {"param a color red\n---\n", 1, "#hex"},
        {"\nfallback: custom(\"x\")\n---\n", 2, "fallback"},
        {"fallback: blur(2px) glow(2px #fff)\n---\n", 1, "fallback"},
        {"expand: -2px\n---\n", 1, "expand"},
        {"param xtTime float 1\n---\n", 1, "reserved"},
        // T-534: 16 float components and 4 colors at most.
        {"param a vec2 1 1\nparam b vec2 1 1\nparam c vec2 1 1\nparam d vec2 1 1\nparam e vec2 1 1\n"
         "param f vec2 1 1\nparam g vec2 1 1\nparam h vec2 1 1\nparam i float 1\n---\n",
         10, "too many parameters"},
        {"param a color #fff\nparam b color #fff\nparam c color #fff\nparam d color #fff\nparam e color "
         "#fff\n---\n",
         6, "too many parameters"},
        // T-534 (18.13): `p<n>` / `c<n>` / `v<n>` are slot addresses.
        {"param k float 1\nparam p3 float 1\n---\n", 2, "reserved parameter name"},
        {"param c0 color #fff\n---\n", 1, "reserved parameter name"},
        {"param v12 vec2 0 0\n---\n", 1, "reserved parameter name"},
    };
    for (const Case& c : cases) {
        const std::string text = c.text;
        CAPTURE(text);
        XtfxResult r = run(text + "float4 xtMain(XtInput i) { return xtSample(i.uv); }\n");
        CAPTURE(r.error.message);
        CHECK_FALSE(r.ok);
        CHECK(r.error.line == c.line);
        CHECK(contains(r.error.message, c.message));
    }
}

// -------------------------------------------------------------------------------------------
// HLSL subset: one allowed and one rejected example per rule
// -------------------------------------------------------------------------------------------

TEST_CASE("xtfx subset: types") {
    CHECK(
        accepts("float a = 1; float2 b = float2(1, 2); float3 c = float3(b, 3); float4 d = float4(c, 1);"
                " int e = 2; bool f = true;"));
    CHECK(rejects("half a = 1;"));
    CHECK(rejects("float4x4 m;"));
    CHECK(rejects("int2 v = int2(1, 2);"));
    CHECK(rejects("uint u = 1;"));
}

TEST_CASE("xtfx subset: operators and swizzles") {
    CHECK(
        accepts("float a = 1 + 2 * 3 - 4 / 2; int m = 7 % 3; bool c = a < 2 && !(a >= 3) || a == 1;"
                " float2 v = i.uv.yx; float4 w = float4(1, 2, 3, 4); w.rg = v; w.a += 1; a++; --a;"));
    CHECK(rejects("int b = 1 << 2;"));        // shifts are not listed
    CHECK(rejects("float a = 1.0 % 2.0;"));   // `%` is int-only; fmod for floats
    CHECK(rejects("float2 v = i.uv.xz;"));    // swizzle outside the vector
    CHECK(rejects("float2 v = i.uv.xg;"));    // mixed swizzle sets
    CHECK(rejects("bool b = i.uv < i.uv;"));  // comparisons are scalar
    CHECK(rejects("float a = true ? 1 : 2;"));
}

TEST_CASE("xtfx subset: scalar swizzles are errors with line and column (T-208)") {
    // GLSL 330 has no `f.x`; the subset rejects it for float, int and bool alike.
    CHECK(rejects("float f = 1; float g = f.x;"));
    CHECK(rejects("float f = 1; float g = f.r;"));
    CHECK(rejects("float g = k.x;"));       // a float parameter
    CHECK(rejects("float g = xtTime.x;"));  // the clock
    CHECK(rejects("float2 v = i.uv.x.xx;"));
    CHECK(rejects("int n = 1; int m = n.x;"));
    CHECK(rejects("bool b = true; bool c = b.r;"));
    CHECK(accepts("float2 v = i.uv; float g = v.x; float4 w = float4(1, 2, 3, 4); float h = w.a;"));

    // Header lines 1-2, body from line 3; the error points at the member.
    XtfxResult r =
        run("param k float 1\n---\nfloat4 xtMain(XtInput i)\n{\n    float a = 1;\n    float b = a.x;\n"
            "    return float4(1, 1, 1, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.line == 6);
    CHECK(r.error.col > 0);
    CHECK(contains(r.error.message, "scalar"));
    for (const char* stmt : {"int n = 1;\n    int m = n.x;", "bool c = true;\n    bool d = c.r;"}) {
        r = run("param k float 1\n---\nfloat4 xtMain(XtInput i)\n{\n    " + std::string(stmt) +
                "\n    return float4(1, 1, 1, 1);\n}\n");
        CHECK_FALSE(r.ok);
        CHECK(r.error.line == 6);
        CHECK(r.error.col > 0);
    }
}

TEST_CASE("xtfx subset: builtin functions") {
    CHECK(accepts(
        "float a = abs(-1) + sin(1) + cos(1) + tan(1) + atan2(1, 2) + exp(1) + log(2) + pow(2, 3) + sqrt(4)"
        " + floor(1.5) + ceil(1.5) + frac(1.5) + fmod(5, 3) + min(1, 2) + max(1, 2) + clamp(3, 0, 1)"
        " + saturate(2) + lerp(0, 1, 0.5) + step(0.5, 1) + smoothstep(0, 1, 0.5) + length(i.uv)"
        " + distance(i.uv, i.layerSize) + dot(i.uv, i.uv);"
        " float2 n = normalize(i.uv); float3 x = cross(float3(1, 0, 0), float3(0, 1, 0));"
        " float4 s = xtSample(i.uv);"));
    CHECK(rejects("float a = mul(1, 2);"));
    CHECK(rejects("float a = rsqrt(2);"));
    CHECK(rejects("float a = noSuchFn(2);"));
    CHECK(rejects("float a = pow(2);"));              // arity
    CHECK(rejects("float3 x = cross(i.uv, i.uv);"));  // cross needs float3
    CHECK(rejects("float4 s = xtSample(1);"));        // xtSample needs a float2
}

TEST_CASE("xtfx subset: if/else and constant-bounded for") {
    CHECK(accepts("float a = 0; if (a > 1) { a = 2; } else if (a < 0) a = 3; else { a = 4; }"));
    CHECK(accepts("float s = 0; for (int n = 0; n < 64; n++) { s += n; }"));
    CHECK(accepts("float s = 0; for (int n = 10; n > 0; n -= 2) s += 1;"));
    CHECK(rejects("float a = 0; if (a) { a = 1; }"));  // condition must be bool
    CHECK(rejects("float s = 0; for (int n = 0; n < 65; n++) { s += n; }"));
    CHECK(rejects("float s = 0; for (int n = 0; n <= 64; n++) { s += n; }"));
    CHECK(rejects("int m = 4; float s = 0; for (int n = 0; n < m; n++) { s += n; }"));  // non-constant bound
    CHECK(rejects("float s = 0; for (float n = 0; n < 4; n++) { s += n; }"));           // counter must be int
    CHECK(rejects("float s = 0; for (int n = 0; n < 4; n++) { n = 0; }"));              // counter written
    CHECK(rejects("float s = 0; for (int n = 0; n < 4; n += 0) { s += 1; }"));
    CHECK(rejects("float s = 0; while (s < 1) { s += 1; }"));
    CHECK(rejects("float s = 0; do { s += 1; } while (s < 1);"));
    CHECK(rejects("for (int n = 0; n < 4; n++) { break; }"));
    CHECK(rejects("int k2 = 1; switch (k2) { }"));
}

TEST_CASE("xtfx subset: local variables and scopes") {
    CHECK(accepts("float a = 1; { float b = a; a = b; } const float c = 2;"));
    CHECK(rejects("float a = 1; float a = 2;"));  // same scope
    CHECK(rejects("a = 1;"));                     // undeclared
    CHECK(rejects("float mix = 1;"));             // a GLSL name
    CHECK(rejects("float a = 1, b = 2;"));
}

TEST_CASE("xtfx subset: rejected constructs") {
    CHECK(rejects("struct S { float a; };"));              // custom struct
    CHECK(rejects("float arr[4];"));                       // array
    CHECK(rejects("Texture2D tex;"));                      // texture object
    CHECK(rejects("float4 c = uLayer.Sample(s, i.uv);"));  // Sample
    CHECK(rejects("discard;"));                            // discard
    CHECK(rejects("k = 2;"));                              // parameters are read-only
    CHECK(rejects("xtTime = 2;"));                         // xtTime is read-only
    CHECK_FALSE(run(withBody("float g = 1;\nfloat4 xtMain(XtInput i) { return float4(g, g, g, 1); }\n")).ok);
    CHECK_FALSE(
        run(withBody("static float g;\nfloat4 xtMain(XtInput i) { return float4(1, 1, 1, 1); }\n")).ok);
    // Recursion (and any other function) is impossible: only xtMain may be defined.
    CHECK_FALSE(run(withBody("float f(float x) { return f(x); }\n"
                             "float4 xtMain(XtInput i) { return float4(1, 1, 1, 1); }\n"))
                    .ok);
    CHECK_FALSE(run(withBody("float4 xtMain(XtInput i) { return xtMain(i); }\n")).ok);
    CHECK_FALSE(run(withBody("float4 xtMain(float2 uv) { return float4(1, 1, 1, 1); }\n")).ok);
    CHECK_FALSE(run(withBody("float4 xtMain(XtInput i) { float a = 1; }\n")).ok);   // no return
    CHECK_FALSE(run(withBody("float4 xtMain(XtInput i) { return 1; }\n")).ok);      // wrong type
    CHECK_FALSE(run(withBody("float4 xtMain(XtInput i) { return i.foo; }\n")).ok);  // no such member
    CHECK_FALSE(run(withBody("float4 xtMain(XtInput i) { return float4(1,1,1,1); } @\n")).ok);
}

TEST_CASE("xtfx subset: errors report line and column in the whole file") {
    // Header lines 1-2, body from line 3.
    XtfxResult r =
        run("param k float 1\n---\nfloat4 xtMain(XtInput i)\n{\n    float a = 1;\n    discard;\n"
            "    return float4(1, 1, 1, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.line == 6);
    CHECK(r.error.col == 5);
    CHECK(contains(r.error.message, "discard"));

    r =
        run("param k float 1\n---\nfloat4 xtMain(XtInput i)\n{\n  float a = 1;\n  float b = a +  nope;\n"
            "  return float4(1, 1, 1, 1);\n}\n");
    CHECK_FALSE(r.ok);
    CHECK(r.error.line == 6);
    CHECK(r.error.col == 18);
    CHECK(contains(r.error.message, "nope"));
}

// -------------------------------------------------------------------------------------------
// GLSL translation table (18.7)
// -------------------------------------------------------------------------------------------

TEST_CASE("xtfx translation: every row of the 18.7 table") {
    XtfxResult r =
        run("param k float 1\nparam c vec2 0 0\nparam t color #ff0000\n---\n"
            "float4 xtMain(XtInput i) {\n"
            "    float2 a = float2(1, 2);\n"
            "    float3 b = float3(a, 3);\n"
            "    float4 d = float4(b, 1);\n"
            "    float l = lerp(0.0, 1.0, 0.5);\n"
            "    float f = frac(1.5);\n"
            "    float s = saturate(k);\n"
            "    float at = atan2(1.0, 2.0);\n"
            "    float m = fmod(5.0, 3.0);\n"
            "    float4 x = xtSample(i.uv);\n"
            "    float p = 2 * k;\n"
            "    return x * t + d * (l + f + s + at + m + p);\n"
            "}\n");
    REQUIRE_MESSAGE(r.ok, r.error.message);
    const std::string& g = r.glsl;
    CHECK(contains(g, "vec2 a = vec2(1.0, 2.0);"));
    CHECK(contains(g, "vec3 b = vec3(a, 3.0);"));
    CHECK(contains(g, "vec4 d = vec4(b, 1.0);"));
    CHECK(contains(g, "mix(0.0, 1.0, 0.5)"));
    CHECK(contains(g, "fract(1.5)"));
    CHECK(contains(g, "clamp(k, 0.0, 1.0)"));
    CHECK(contains(g, "atan(1.0, 2.0)"));
    CHECK(contains(g, "xtFmod(5.0, 3.0)"));
    CHECK(contains(g, "float xtFmod(float a, float b) { return a - b * trunc(a / b); }"));
    CHECK(contains(g, "xtSample(i.uv)"));
    CHECK(contains(g, "texture(uLayer, xtTexCoord(uv))"));
    CHECK(contains(g, "2.0 * k"));  // integer literal in float context gets `.0`
    // Wrapper-facing declarations.
    CHECK(contains(g, "uniform sampler2D uLayer;"));
    CHECK(contains(g, "uniform float xtTime;"));
    CHECK(contains(g, "uniform float k;"));
    CHECK(contains(g, "uniform vec2 c;"));
    CHECK(contains(g, "uniform vec4 t;"));
    CHECK(contains(g, "struct XtInput { vec2 uv; vec2 texelSize; vec2 layerSize; };"));
    CHECK(contains(g, "vec4 xtMain(XtInput i)"));
    CHECK_FALSE(contains(g, "float2"));
    CHECK_FALSE(contains(g, "#version"));
    CHECK_FALSE(contains(g, "void main"));
}

TEST_CASE("xtfx translation: int variables and scalar broadcasts are promoted") {
    XtfxResult r =
        run(withBody("float4 xtMain(XtInput i) {\n"
                     "    int n = 3;\n"
                     "    float a = n * 0.5;\n"
                     "    float2 v = clamp(i.uv, 0, 1);\n"
                     "    v *= 2;\n"
                     "    return float4(v, a, 1);\n"
                     "}\n"));
    REQUIRE_MESSAGE(r.ok, r.error.message);
    CHECK(contains(r.glsl, "float(n) * 0.5"));
    CHECK(contains(r.glsl, "clamp(i.uv, vec2(0.0), vec2(1.0))"));
    CHECK(contains(r.glsl, "v *= vec2(2.0)"));
    CHECK(contains(r.glsl, "return vec4(v, a, 1.0);"));
}

// -------------------------------------------------------------------------------------------
// Compiler: custom() -> ASST kind 2 + SHDR; Document::shaderRef(); SW fallback
// -------------------------------------------------------------------------------------------

TEST_CASE("xtframe-compile: custom() writes ASST kind 2, SHDR and the parameter slots") {
    CompileResult result =
        compileWithAsset("xt_t203_compile",
                         "<xt><head><style>#a { width: 40px; height: 40px; background-color: #f00;"
                         " effect: custom(\"ripple.xtfx\", amplitude 6, tint #00ff00); }</style></head>"
                         "<body><div id=\"a\"></div></body></xt>",
                         "ripple.xtfx", kRipple);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    REQUIRE(result.assets.size() == 1);
    CHECK(result.assets[0].kind == 2);
    CHECK(result.assets[0].sourceName == "ripple.xtfx");

    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    const ShaderChunk& shdr = view.value().shaders();
    REQUIRE(shdr.present());
    REQUIRE(shdr.shaders().size() == 1);
    const IrShaderRef& ref = shdr.shaders()[0];
    CHECK(ref.asset == 0);
    CHECK(ref.expandPx == doctest::Approx(8));
    CHECK(ref.fallback.kind == static_cast<uint8_t>(EffectKind::Blur));
    CHECK(ref.paramCount == 3);
    CHECK(view.value().string(shdr.params()[ref.paramOffset].name) == "amplitude");
    CHECK(contains(std::string(shdr.text().substr(ref.sourceOffset, ref.sourceLength)), "i.uv.yx * 40"));
    CHECK(contains(std::string(shdr.text().substr(ref.glslOffset, ref.glslLength)), "i.uv.yx * 40.0"));

    bool found = false;
    for (const IrEffect& fx : view.value().style().effects()) {
        if (fx.kind != static_cast<uint8_t>(EffectKind::Custom)) {
            continue;
        }
        found = true;
        CHECK(fx.shader == 0);
        CHECK(fx.params[0] == doctest::Approx(6));    // override
        CHECK(fx.params[1] == doctest::Approx(1.5));  // default
        CHECK(fx.colors[0].g == doctest::Approx(1));  // override
        CHECK(fx.colors[0].r == doctest::Approx(0));
    }
    CHECK(found);
}

TEST_CASE("xtframe-compile: custom() errors") {
    const char* xt =
        "<xt><head><style>#a { effect: custom(\"%s\"%s); }</style></head>"
        "<body><div id=\"a\"></div></body></xt>";
    auto doc = [&](const char* asset, const char* rest) {
        char buf[512];
        std::snprintf(buf, sizeof(buf), xt, asset, rest);
        return std::string(buf);
    };
    CHECK(hasError(compileWithAsset("xt_t203_err1", doc("missing.xtfx", ""), "ripple.xtfx", kRipple),
                   "not found"));
    CHECK(hasError(compileWithAsset("xt_t203_err2", doc("ripple.xtfx", ", nope 1"), "ripple.xtfx", kRipple),
                   "unknown parameter"));
    CHECK(hasError(compileWithAsset("xt_t203_err3", doc("ripple.xtfx", ", tint 1"), "ripple.xtfx", kRipple),
                   "takes a color"));
    CHECK(hasError(compileWithAsset("xt_t203_err4", doc("bad.xtfx", ""), "bad.xtfx",
                                    "---\nfloat4 xtMain(XtInput i) { discard; }\n"),
                   "discard"));
    CHECK(hasError(compileWithAsset("xt_t203_err5", doc("bad.xtfx", ""), "bad.xtfx",
                                    "fallback: gradient-mask(linear, 0 0, 1 1, #000 0%, #fff 100%)\n---\n"
                                    "float4 xtMain(XtInput i) { return xtSample(i.uv); }\n"),
                   "fallback"));
}

TEST_CASE("xtframe-compile: @timeline addresses effect.<header name>.<param> (18.12)") {
    CompileResult result = compileWithAsset(
        "xt_t203_timeline",
        "<xt><head><style>#a { width: 40px; height: 40px; effect: custom(\"ripple.xtfx\"); }"
        "@timeline wave { #a { 0s { effect.Ripple.amplitude: 0; } 1s { effect.Ripple.amplitude: 16; }"
        " } }</style></head><body><div id=\"a\"></div></body></xt>",
        "ripple.xtfx", kRipple);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    bool found = false;
    for (uint32_t t = 0; t < view.value().timelineCount(); ++t) {
        for (const IrTrack& track : view.value().timeline(t).tracks()) {
            found = found || track.prop == effectParam(0, 1);
        }
    }
    CHECK(found);
}

// T-532 (Docs/design/18-effects.md §18.13): every vec2 parameter of a custom effect is
// addressable by component, `.x` -> PropId `1 + slot`, `.y` -> `2 + slot`.
namespace {

const char* const kWobble =
    "name: Wobble\n"
    "param a float 1\n"
    "param b float 2\n"
    "param c vec2 0 0\n"
    "---\n"
    "float4 xtMain(XtInput i) { return xtSample(i.uv + c * (a * b) * i.texelSize); }\n";

const char* const kShiftFx =
    "name: Shift\n"
    "param o vec2 0 0\n"
    "param k float 1\n"
    "---\n"
    "float4 xtMain(XtInput i) { return xtSample(i.uv + o * k * i.texelSize); }\n";

std::string effectDoc(const std::string& effect, const std::string& timeline) {
    return "<xt><head><style>#a { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px;"
           " background-color: #ff0000; effect: " +
           effect + "; }" + timeline + "</style></head><body><div id=\"a\"></div></body></xt>";
}

bool hasTrack(const CompileResult& result, PropId prop) {
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    for (uint32_t t = 0; t < view.value().timelineCount(); ++t) {
        for (const IrTrack& track : view.value().timeline(t).tracks()) {
            if (track.prop == prop) {
                return true;
            }
        }
    }
    return false;
}

void checkNoErrors(const CompileResult& result) {
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
}

}  // namespace

TEST_CASE("xtframe-compile: T-532 @timeline addresses a vec2 parameter by component (18.13)") {
    CompileResult result =
        compileWithAsset("xt_t532_component",
                         effectDoc("custom(\"wobble.xtfx\")",
                                   "@timeline wob { #a { 0s { effect.Wobble.c.x: 0; effect.Wobble.c.y: 0; }"
                                   " 1s { effect.Wobble.c.x: 10; effect.Wobble.c.y: 20; } } }"),
                         "wobble.xtfx", kWobble);
    checkNoErrors(result);
    REQUIRE(result.ok());
    CHECK(hasTrack(result, effectParam(0, 3)));  // c occupies params[2..3]: .x -> 1 + 2
    CHECK(hasTrack(result, effectParam(0, 4)));  // .y -> 2 + 2
    CHECK_FALSE(hasTrack(result, effectParam(0, prop::kEffectParamVec2Base + 2)));

    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{result.ir.data(), result.ir.size()}, false, nullptr, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 64, 64});
    doc->update(0.0f);
    ElementRef a = doc->byId("a");
    REQUIRE(a.effectCount() == 1);
    PlayOptions hold;
    hold.hold = true;
    REQUIRE(doc->playTimeline("", "wob", hold) == Status::kOk);
    doc->update(0.5f);
    CHECK(a.effect(0).params[2] == doctest::Approx(5.0f));
    CHECK(a.effect(0).params[3] == doctest::Approx(10.0f));
    CHECK(a.effect(0).params[0] == doctest::Approx(1.0f));  // a and b are untouched.
    CHECK(a.effect(0).params[1] == doctest::Approx(2.0f));
}

TEST_CASE("xtframe-compile: T-532 a slot-0 vec2: component, whole, and both is an error") {
    CompileResult component = compileWithAsset(
        "xt_t532_slot0_x",
        effectDoc("custom(\"shift.xtfx\")",
                  "@timeline s { #a { 0s { effect.Shift.o.x: 0; } 1s { effect.Shift.o.x: 4; } } }"),
        "shift.xtfx", kShiftFx);
    checkNoErrors(component);
    REQUIRE(component.ok());
    CHECK(hasTrack(component, effectParam(0, 1)));

    // T-534: the whole vec2 takes `<x> <y>` and is a Vec2 track on kEffectParamVec2Base + 0.
    CompileResult whole = compileWithAsset(
        "xt_t532_slot0_whole",
        effectDoc("custom(\"shift.xtfx\")",
                  "@timeline s { #a { 0s { effect.Shift.o: 0 0; } 1s { effect.Shift.o: 4px 8px; } } }"),
        "shift.xtfx", kShiftFx);
    checkNoErrors(whole);
    REQUIRE(whole.ok());
    CHECK(hasTrack(whole, effectParam(0, prop::kEffectParamVec2Base)));

    CompileResult both = compileWithAsset(
        "xt_t532_slot0_both",
        effectDoc("custom(\"shift.xtfx\")",
                  "@timeline s { #a { 0s { effect.Shift.o: 0 0; } 1s { effect.Shift.o.y: 4; } } }"),
        "shift.xtfx", kShiftFx);
    CHECK_FALSE(both.ok());
    CHECK(hasError(both, "vec2 parameter addressed both whole and by component"));
}

TEST_CASE("xtframe-compile: T-532 component errors") {
    CompileResult onFloat = compileWithAsset(
        "xt_t532_err_float",
        effectDoc("custom(\"wobble.xtfx\")", "@timeline w { #a { 0s { effect.Wobble.a.x: 0; } } }"),
        "wobble.xtfx", kWobble);
    CHECK(hasError(onFloat, "parameter has no components"));

    CompileResult badComponent = compileWithAsset(
        "xt_t532_err_z",
        effectDoc("custom(\"wobble.xtfx\")", "@timeline w { #a { 0s { effect.Wobble.c.z: 0; } } }"),
        "wobble.xtfx", kWobble);
    CHECK(hasError(badComponent, "expected .x or .y"));

    CompileResult unknown = compileWithAsset(
        "xt_t532_err_unknown",
        effectDoc("custom(\"wobble.xtfx\")", "@timeline w { #a { 0s { effect.Wobble.nope.x: 0; } } }"),
        "wobble.xtfx", kWobble);
    CHECK(hasError(unknown, "unknown effect parameter"));

    CompileResult builtin = compileWithAsset(
        "xt_t532_err_builtin",
        effectDoc("glow(8px #ffffff 0.2)", "@timeline w { #a { 0s { effect.glow.radius.x: 0; } } }"),
        "unused.xtfx", kWobble);
    CHECK(hasError(builtin, "unknown effect parameter"));

    uint32_t index = 0;
    XtfxResult r = run(kWobble);
    REQUIRE(r.ok);
    CHECK(xtfxParamIndex(r.header.params, "c.x", index));
    CHECK(index == 3);
    CHECK(xtfxParamIndex(r.header.params, "c.y", index));
    CHECK(index == 4);
    CHECK(xtfxParamIndex(r.header.params, "c", index));  // T-534: a whole vec2 from any slot
    CHECK(index == prop::kEffectParamVec2Base + 2);
    CHECK_FALSE(xtfxParamIndex(r.header.params, "a.x", index));
    CHECK_FALSE(xtfxParamIndex(r.header.params, "nope.y", index));
}

TEST_CASE("xtframe-compile: T-532 .xtanim.json tracks address a vec2 component") {
    fs::path dir = freshTempDir("xt_t532_xtanim");
    writeText(dir / "wobble.xtfx", kWobble);
    const std::string json = R"({ "v": 1, "timelines": [ { "name": "main", "tracks": [
        { "layer": "a", "prop": "effect.Wobble.c.y", "keys": [[0, 0], [1, 20]] } ] } ] })";
    const std::string xt = effectDoc("custom(\"wobble.xtfx\")", "@timeline wob from \"x.xtanim.json#main\";");
    SourceFile files[2] = {SourceFile{"main.xt", xt}, SourceFile{"x.xtanim.json", json}};
    CompileOptions options;
    options.rootDir = dir.string();
    options.emitMeta = false;
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    checkNoErrors(result);
    REQUIRE(result.ok());
    CHECK(hasTrack(result, effectParam(0, 4)));
}

// T-534 (Docs/design/18-effects.md §18.2, §18.13): 16 float + 4 color slots, the whole vec2 from
// any slot, slot addressing `p<n>` / `c<n>` / `v<n>`, and the value typed by the parameter.
namespace {

// 16 float components (three vec2: c at 2, center at 6, g at 9) and 4 colors.
const char* const kFull =
    "name: Fx\n"
    "param a float 1\n"
    "param b float 2\n"
    "param c vec2 3 4\n"
    "param d float 5\n"
    "param e float 6\n"
    "param center vec2 7 8\n"
    "param f float 9\n"
    "param g vec2 10 11\n"
    "param h float 12\n"
    "param n float 13\n"
    "param j float 14\n"
    "param k float 15\n"
    "param l float 16\n"
    "param tint color #ff8800\n"
    "param t1 color #00ff00\n"
    "param t2 color #0000ff\n"
    "param t3 color #ffffff80\n"
    "---\n"
    "float4 xtMain(XtInput i) { return xtSample(i.uv + center * i.texelSize) * tint; }\n";

CompileResult compileFull(const char* dirName, const std::string& timeline) {
    return compileWithAsset(dirName, effectDoc("custom(\"fx.xtfx\")", timeline), "fx.xtfx", kFull);
}

/// The type of `prop`'s track (0xFF when there is none).
uint8_t trackType(const CompileResult& result, PropId prop) {
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    for (uint32_t t = 0; t < view.value().timelineCount(); ++t) {
        for (const IrTrack& track : view.value().timeline(t).tracks()) {
            if (track.prop == prop) {
                return track.type;
            }
        }
    }
    return 0xFF;
}

}  // namespace

TEST_CASE("xtfx header: T-534 16 float components + 4 colors fit; applyXtfxDefaults fills every slot") {
    XtfxResult r = run(kFull);
    REQUIRE_MESSAGE(r.ok, r.error.message);
    REQUIRE(r.header.params.size() == 17);
    float params[16] = {};
    float colors[4][4] = {};
    applyXtfxDefaults(r.header.params, params, colors);
    for (int i = 0; i < 16; ++i) {
        CAPTURE(i);
        CHECK(params[i] == doctest::Approx(static_cast<float>(i + 1)));  // defaults 1..16 in slot order
    }
    CHECK(colors[0][0] == doctest::Approx(1.0f));
    CHECK(colors[0][1] == doctest::Approx(0x88 / 255.0f));
    CHECK(colors[1][1] == doctest::Approx(1.0f));
    CHECK(colors[2][2] == doctest::Approx(1.0f));
    CHECK(colors[3][3] == doctest::Approx(0x80 / 255.0f));

    uint32_t index = 0;
    CHECK(xtfxParamIndex(r.header.params, "center", index));
    CHECK(index == prop::kEffectParamVec2Base + 6);
    CHECK(xtfxParamIndex(r.header.params, "center.x", index));
    CHECK(index == prop::kEffectParamFloatBase + 6);
    CHECK(xtfxParamIndex(r.header.params, "g.y", index));
    CHECK(index == prop::kEffectParamFloatBase + 10);
    CHECK(xtfxParamIndex(r.header.params, "l", index));
    CHECK(index == 16);
    CHECK(xtfxParamIndex(r.header.params, "t3", index));
    CHECK(index == prop::kEffectParamColorBase + 3);

    // One more float component, or a fifth color, is a header error.
    const std::string body = "---\nfloat4 xtMain(XtInput i) { return xtSample(i.uv); }\n";
    const std::string full(kFull);
    const std::string head = full.substr(0, full.find("---\n"));
    XtfxResult floats17 = run(head + "param m float 17\n" + body);
    CHECK_FALSE(floats17.ok);
    CHECK(contains(floats17.error.message, "too many parameters (at most 16 float components and 4 colors)"));
    XtfxResult colors5 = run(head + "param t4 color #000\n" + body);
    CHECK_FALSE(colors5.ok);
    CHECK(contains(colors5.error.message, "too many parameters"));

    // And it compiles: the custom effect's IrEffect carries all 16 + 4 defaults.
    CompileResult result = compileFull("xt_t534_full", "");
    checkNoErrors(result);
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{});
    REQUIRE(view.ok());
    bool found = false;
    for (const IrEffect& fx : view.value().style().effects()) {
        if (fx.kind == static_cast<uint8_t>(EffectKind::Custom)) {
            found = true;
            CHECK(fx.params[15] == doctest::Approx(16.0f));
            CHECK(fx.params[6] == doctest::Approx(7.0f));
            CHECK(fx.colors[3].a == doctest::Approx(0x80 / 255.0f));
        }
    }
    CHECK(found);
}

TEST_CASE("xtframe-compile: T-534 a vec2 in slot 6 addressed whole, by component, and both") {
    CompileResult whole = compileFull(
        "xt_t534_whole6",
        "@timeline m { #a { 0s { effect.Fx.center: 0 0; } 1s { effect.Fx.center: 10px 20px; } } }");
    checkNoErrors(whole);
    REQUIRE(whole.ok());
    CHECK(trackType(whole, effectParam(0, prop::kEffectParamVec2Base + 6)) ==
          static_cast<uint8_t>(ValueType::Vec2));

    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{whole.ir.data(), whole.ir.size()}, false, nullptr, nullptr, doc) ==
            Status::kOk);
    doc->setViewport(Rect{0, 0, 64, 64});
    doc->update(0.0f);
    ElementRef a = doc->byId("a");
    REQUIRE(a.effectCount() == 1);
    PlayOptions hold;
    hold.hold = true;
    REQUIRE(doc->playTimeline("", "m", hold) == Status::kOk);
    doc->update(0.5f);
    CHECK(a.effect(0).params[6] == doctest::Approx(5.0f));
    CHECK(a.effect(0).params[7] == doctest::Approx(10.0f));
    CHECK(a.effect(0).params[5] == doctest::Approx(6.0f));  // neighbours untouched
    CHECK(a.effect(0).params[8] == doctest::Approx(9.0f));

    CompileResult component =
        compileFull("xt_t534_comp6",
                    "@timeline m { #a { 0s { effect.Fx.center.x: 0; } 1s { effect.Fx.center.x: 4; } } }");
    checkNoErrors(component);
    REQUIRE(component.ok());
    CHECK(trackType(component, effectParam(0, prop::kEffectParamFloatBase + 6)) ==
          static_cast<uint8_t>(ValueType::Float));

    CompileResult both =
        compileFull("xt_t534_both6",
                    "@timeline m { #a { 0s { effect.Fx.center: 0 0; } 1s { effect.Fx.center.y: 4; } } }");
    CHECK_FALSE(both.ok());
    CHECK(hasError(both, "vec2 parameter addressed both whole and by component"));
}

TEST_CASE("xtframe-compile: T-534 slot addressing p<n> / c<n> / v<n>") {
    CompileResult slots =
        compileFull("xt_t534_slots",
                    "@timeline m { #a { 0s { effect.Fx.p9: 0; effect.Fx.c2: #000000; effect.Fx.v2: 0 0; }"
                    " 1s { effect.Fx.p9: 3; effect.Fx.c2: #ff0000; effect.Fx.v2: 1 2; } } }");
    checkNoErrors(slots);
    REQUIRE(slots.ok());
    CHECK(trackType(slots, effectParam(0, prop::kEffectParamFloatBase + 9)) ==
          static_cast<uint8_t>(ValueType::Float));
    CHECK(trackType(slots, effectParam(0, prop::kEffectParamColorBase + 2)) ==
          static_cast<uint8_t>(ValueType::Color));
    CHECK(trackType(slots, effectParam(0, prop::kEffectParamVec2Base + 2)) ==
          static_cast<uint8_t>(ValueType::Vec2));

    // Built-in effects take slots too: `v0` is DropShadow's offset, `p1` its offsetY.
    const char* shadow = "shadow(1px 2px 3px #000000)";
    CompileResult v0 = compileWithAsset(
        "xt_t534_shadow_v0",
        effectDoc(shadow,
                  "@timeline m { #a { 0s { effect.shadow.v0: 0 0; } 1s { effect.shadow.v0: 4px 4px; } } }"),
        "unused.xtfx", kFull);
    checkNoErrors(v0);
    REQUIRE(v0.ok());
    CHECK(trackType(v0, effectParam(0, prop::kEffectParamVec2Base)) == static_cast<uint8_t>(ValueType::Vec2));
    CompileResult offset = compileWithAsset(
        "xt_t534_shadow_offset",
        effectDoc(
            shadow,
            "@timeline m { #a { 0s { effect.shadow.offset: 0 0; } 1s { effect.shadow.offset: 4px 4px; } } }"),
        "unused.xtfx", kFull);
    checkNoErrors(offset);
    REQUIRE(offset.ok());
    CHECK(trackType(offset, effectParam(0, prop::kEffectParamVec2Base)) ==
          static_cast<uint8_t>(ValueType::Vec2));
    CompileResult p1 = compileWithAsset(
        "xt_t534_shadow_p1",
        effectDoc(shadow, "@timeline m { #a { 0s { effect.shadow.p1: 0; } 1s { effect.shadow.p1: 2px; } } }"),
        "unused.xtfx", kFull);
    checkNoErrors(p1);
    REQUIRE(p1.ok());
    CHECK(trackType(p1, effectParam(0, 2)) == static_cast<uint8_t>(ValueType::Float));

    for (const char* bad : {"p16", "c4", "v15"}) {
        CAPTURE(bad);
        CompileResult r = compileFull("xt_t534_range",
                                      std::string("@timeline m { #a { 0s { effect.Fx.") + bad + ": 1; } } }");
        CHECK_FALSE(r.ok());
        CHECK(hasError(r, "slot out of range"));
    }

    XtfxResult reserved = run("param p3 float 1\n---\nfloat4 xtMain(XtInput i) { return xtSample(i.uv); }\n");
    CHECK_FALSE(reserved.ok);
    CHECK(contains(reserved.error.message, "reserved parameter name"));
}

TEST_CASE("xtframe-compile: T-534 the value's type follows the parameter") {
    CompileResult tint = compileFull(
        "xt_t534_tint",
        "@timeline m { #a { 0s { effect.Fx.tint: #000000ff; } 1s { effect.Fx.tint: #ff8800ff; } } }");
    checkNoErrors(tint);
    REQUIRE(tint.ok());
    CHECK(trackType(tint, effectParam(0, prop::kEffectParamColorBase)) ==
          static_cast<uint8_t>(ValueType::Color));

    CompileResult tintNumber =
        compileFull("xt_t534_tint3", "@timeline m { #a { 0s { effect.Fx.tint: 3; } } }");
    CHECK_FALSE(tintNumber.ok());
    CHECK(hasError(tintNumber, "value does not match parameter type"));

    CompileResult centerNumber =
        compileFull("xt_t534_center5", "@timeline m { #a { 0s { effect.Fx.center: 5; } } }");
    CHECK_FALSE(centerNumber.ok());
    CHECK(hasError(centerNumber, "value does not match parameter type"));

    CompileResult floatPair = compileFull("xt_t534_a12", "@timeline m { #a { 0s { effect.Fx.a: 1 2; } } }");
    CHECK_FALSE(floatPair.ok());
    CHECK(hasError(floatPair, "value does not match parameter type"));
}

TEST_CASE("xtframe-compile: T-534 shadow offset @keyframes expand to the Vec2 track 64") {
    CompileResult result = compileWithAsset(
        "xt_t534_kf_shadow",
        "<xt><head><style>@keyframes k { from { effect: shadow(0 0 4px #000000); }"
        " to { effect: shadow(6px 8px 4px #000000); } }"
        "#a { width: 40px; height: 40px; effect: shadow(0 0 4px #000000); animation: k 1s; }</style></head>"
        "<body><div id=\"a\"></div></body></xt>",
        "unused.xtfx", kFull);
    checkNoErrors(result);
    REQUIRE(result.ok());
    CHECK(trackType(result, effectParam(0, prop::kEffectParamVec2Base)) ==
          static_cast<uint8_t>(ValueType::Vec2));
    CHECK(trackType(result, effectParam(0, 1)) == 0xFF);  // no separate offsetX / offsetY tracks
    CHECK(trackType(result, effectParam(0, 2)) == 0xFF);
}

TEST_CASE("IrView: T-534 a 2.x header is rejected and versionError() says to recompile") {
    CompileResult result = compileFull("xt_t534_major", "");
    REQUIRE(result.ok());
    std::vector<uint8_t> bytes = result.ir;
    CHECK(IrView::versionError(IrBlob{bytes.data(), bytes.size()}).empty());
    bytes[4] = 2;  // IrHeader::major (u16 at offset 4)
    bytes[5] = 0;
    CHECK(IrView::open(IrBlob{bytes.data(), bytes.size()}, IrReadOptions{}).status() == Status::kInvalidIr);
    const std::string why = IrView::versionError(IrBlob{bytes.data(), bytes.size()});
    CHECK(contains(why, "IR major 2"));
    CHECK(contains(why, "recompile the source"));
}

TEST_CASE("xtfx header: T-532 blurb") {
    const std::string body = "---\nfloat4 xtMain(XtInput i) { return xtSample(i.uv); }\n";
    XtfxResult r = run("name: Ripple\nblurb:   \xE6\xB0\xB4\xE6\xB3\xA2\xE7\xB4\x8B ripple  \n" + body);
    REQUIRE_MESSAGE(r.ok, r.error.message);
    CHECK(r.header.blurb == "\xE6\xB0\xB4\xE6\xB3\xA2\xE7\xB4\x8B ripple");
    CHECK(r.header.warnings.empty());

    XtfxResult none = run("name: Ripple\n" + body);
    REQUIRE(none.ok);
    CHECK(none.header.blurb.empty());

    XtfxResult repeated = run("blurb: first\nblurb: second\nblurb: third\n" + body);
    REQUIRE_MESSAGE(repeated.ok, repeated.error.message);
    CHECK(repeated.header.blurb == "third");
    REQUIRE(repeated.header.warnings.size() == 1);  // warned once
    CHECK(repeated.header.warnings[0].line == 2);
    CHECK(contains(repeated.header.warnings[0].message, "blurb"));
}

TEST_CASE("Document::shaderRef and the SW fallback (18.7)") {
    const std::string body = "<body><div id=\"a\"></div></body></xt>";
    const std::string style =
        "#a { position: absolute; left: 16px; top: 16px; width: 32px; height: 32px;"
        " background-color: #ff0000; ";
    // T-263f: the fallback must be a blur that draws, or the comparison below is between two
    // unblurred pictures. kRipple's blur(2px) draws nothing in SW at dpr 1 (sigma 1 gives the
    // three-box approximation a box 1 px wide; a radius under about 2.83 device px does), so this
    // test gives the effect a 6 px fallback. Its 3 sigma = 9 px is more than the header's
    // `expand: 8px`, so the custom effect's layer is the reference's layer exactly: with a
    // smaller blur (4 px was tried) the custom layer is the larger one and the two pictures
    // differ in a few bytes that show nothing -- pixels of alpha 0 that carry a colour in the
    // rows the reference's layer does not reach.
    const std::string blur = "blur(6px)";
    std::string ripple = kRipple;
    const size_t fallbackAt = ripple.find("blur(2px)");
    REQUIRE(fallbackAt != std::string::npos);
    ripple.replace(fallbackAt, 9, blur);
    CompileResult custom = compileWithAsset(
        "xt_t203_sw",
        "<xt><head><style>" + style + "effect: custom(\"ripple.xtfx\"); }</style></head>" + body,
        "ripple.xtfx", ripple);
    CompileResult reference = compileWithAsset(
        "xt_t203_swref", "<xt><head><style>" + style + "effect: " + blur + "; }</style></head>" + body,
        "unused.xtfx", ripple);
    CompileResult plain = compileWithAsset(
        "xt_t203_swplain", "<xt><head><style>" + style + "}</style></head>" + body, "unused.xtfx", ripple);
    REQUIRE(custom.ok());
    REQUIRE(reference.ok());
    REQUIRE(plain.ok());

    NullTextShaper shaper;
    NoAssets assets;
    auto render = [&](const CompileResult& r, std::vector<uint8_t>& pixels, CommandList& list) {
        Context ctx(ContextConfig{&shaper, &assets});
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
        REQUIRE(handle.ok());
        ctx.setViewport(Rect{0, 0, 64, 64}, 1.0f);
        ctx.update(0.25f);
        ctx.render(list);
        pixels.assign(64 * 64 * 4, 0);
        sw::SwRenderer sw;
        sw.setTarget(sw::SwTarget{pixels.data(), 64, 64, 64 * 4});
        sw.clear(Color{0, 0, 0, 0});
        sw.render(list, &shaper, &assets);
        return ctx.document(handle.value())->shaderRef(0) != nullptr;
    };

    // Document::shaderRef (the host contract).
    {
        NullTextShaper s;
        Context ctx(ContextConfig{&s, &assets});
        Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{custom.ir.data(), custom.ir.size()}, false);
        REQUIRE(handle.ok());
        const Document* doc = ctx.document(handle.value());
        const ShaderRef* ref = doc->shaderRef(0);
        REQUIRE(ref != nullptr);
        CHECK(doc->shaderRef(7) == nullptr);
        CHECK(ref->expandPx == doctest::Approx(8));
        CHECK(ref->fallback.kind == EffectKind::Blur);
        REQUIRE(ref->paramCount == 3);
        CHECK(doc->shaderParams()[ref->paramOffset + 2].type == 2);
        const std::string source(doc->shaderText().substr(ref->sourceOffset, ref->sourceLength));
        CHECK(source.rfind("float4 xtMain(XtInput i)", 0) == 0);
        const std::string names(doc->shaderText().substr(ref->namesOffset, ref->namesLength));
        CHECK(names == std::string("amplitude\0speed\0tint\0", 21));
    }

    std::vector<uint8_t> a, b;
    CommandList listA, listB;
    CHECK(render(custom, a, listA));
    render(reference, b, listB);
    // The Painter copied the ShaderRef into the list, with the clock, and expanded the bounds by
    // max(expand 8px, the fallback's 3 sigma = 9 px).
    REQUIRE(listA.shaders.size() == 1);
    CHECK(listA.time == doctest::Approx(0.25f));
    REQUIRE(listA.effectCmds.size() == 1);
    CHECK(listA.effectCmds[0].boundsHint.x == doctest::Approx(16 - 9));
    REQUIRE(listB.effectCmds.size() == 1);
    CHECK(listB.effectCmds[0].boundsHint.x == doctest::Approx(16 - 9));
    // SW never runs the body: the custom effect renders exactly like its fallback.
    CHECK(a == b);
    // T-263f: and the fallback did blur -- neither picture is the element drawn without an effect.
    std::vector<uint8_t> unblurred;
    CommandList listPlain;
    render(plain, unblurred, listPlain);
    CHECK(listPlain.effectCmds.empty());
    CHECK(a != unblurred);
    CHECK(b != unblurred);
}

// T-263d (Docs/tasks/T-263d.md points 1, 2, 4): Context::render names every Document's IR in
// CommandList::sources and the Painter stamps the asset-carrying commands with their Document's
// index; two Documents' shader of the same id are two ShaderRefs.
TEST_CASE("Context::render: T-263d one CommandSource per Document, PushEffect and ShaderRef stamped") {
    const std::string xt =
        "<xt><head><style>#a { position: absolute; left: 16px; top: 16px; width: 32px; height: 32px;"
        " background-color: #ff0000; effect: custom(\"fx.xtfx\"); }</style></head>"
        "<body><div id=\"a\"></div></body></xt>";
    CompileResult first = compileWithAsset("xt_t263d_sources_a", xt, "fx.xtfx", kRipple);
    CompileResult second = compileWithAsset(
        "xt_t263d_sources_b", xt, "fx.xtfx",
        "fallback: none\n---\nfloat4 xtMain(XtInput i)\n{\n    return xtSample(i.uv) * 0.5;\n}\n");
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    NullTextShaper shaper;
    NoAssets assets;
    Context ctx(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> a = ctx.loadDocument(IrBlob{first.ir.data(), first.ir.size()}, false);
    Result<DocumentHandle> b = ctx.loadDocument(IrBlob{second.ir.data(), second.ir.size()}, false);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    Document* docA = ctx.document(a.value());
    Document* docB = ctx.document(b.value());
    ctx.setViewport(Rect{0, 0, 64, 64}, 1.0f);
    ctx.update(0.0f);
    CommandList list;
    ctx.render(list);

    // One entry per Document, in paint order (the first loaded is the bottom surface).
    REQUIRE(list.sources.size() == 2);
    CHECK(list.sources[0].ir == &docA->ir());
    CHECK(list.sources[0].loadSerial == docA->ir().loadSerial());
    CHECK(list.sources[1].ir == &docB->ir());
    CHECK(list.sources[1].loadSerial == docB->ir().loadSerial());
    CHECK(list.sources[0].loadSerial != list.sources[1].loadSerial);
    REQUIRE(list.effectCmds.size() == 2);
    CHECK(list.effectCmds[0].source == 0);
    CHECK(list.effectCmds[1].source == 1);
    // Both shaders are asset 0 of their own IR: one ShaderRef each, not one for both.
    REQUIRE(list.shaders.size() == 2);
    CHECK(list.shaders[0].asset == list.shaders[1].asset);
    CHECK(list.shaders[0].source == 0);
    CHECK(list.shaders[1].source == 1);
    CHECK(list.shaders[0].fallback.kind == EffectKind::Blur);
    CHECK(list.shaders[1].fallback.kind == EffectKind::None);
    CHECK(validateCommandList(list));
    std::string dump;
    dumpCommandList(list, dump);
    CHECK(dump.find(" source=1") != std::string::npos);

    // The table is rebuilt every frame, also when the list is handed back as the damage base.
    DamageList damage;
    ctx.render(list, &damage);
    ctx.update(1.0f / 60.0f);
    ctx.render(list, &damage);
    CHECK(list.sources.size() == 2);
    CHECK(list.sources[1].ir == &docB->ir());

    // One Document is no special case: one entry, index 0.
    Context single(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> only = single.loadDocument(IrBlob{second.ir.data(), second.ir.size()}, false);
    REQUIRE(only.ok());
    single.setViewport(Rect{0, 0, 64, 64}, 1.0f);
    single.update(0.0f);
    CommandList one;
    single.render(one);
    REQUIRE(one.sources.size() == 1);
    CHECK(one.sources[0].ir == &single.document(only.value())->ir());
    REQUIRE(one.effectCmds.size() == 1);
    CHECK(one.effectCmds[0].source == 0);

    // Document::paint called directly names no source: the call IR is the list's.
    CommandList direct;
    direct.viewport = Rect{0, 0, 64, 64};
    single.document(only.value())->paint(direct);
    CHECK(direct.sources.empty());
    REQUIRE(direct.effectCmds.size() == 1);
    CHECK(direct.effectCmds[0].source == 0);
    REQUIRE(direct.shaders.size() == 1);
    CHECK(direct.shaders[0].source == 0);
    std::string directDump;
    dumpCommandList(direct, directDump);
    CHECK(directDump.find("source=") == std::string::npos);
}

// T-263e ruling C (and the T-263d review addition): the simplifier used to drop the list's clock
// and its shader pools, so a custom effect that survives simplification (options.effects == false,
// the SW-fallback hosts) read xtTime as 0 and pointed at a ShaderRef that was no longer there.
TEST_CASE("CommandListSimplifier: T-263e a kept custom effect keeps its ShaderRef, params, text and time") {
    const std::string xt =
        "<xt><head><style>#a { position: absolute; left: 16px; top: 16px; width: 32px; height: 32px;"
        " background-color: #ff0000; effect: custom(\"fx.xtfx\", amplitude 6); }</style></head>"
        "<body><div id=\"a\"></div></body></xt>";
    CompileResult first = compileWithAsset("xt_t263e_simplify_a", xt, "fx.xtfx", kRipple);
    CompileResult second = compileWithAsset("xt_t263e_simplify_b", xt, "fx.xtfx", kRipple);
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    NullTextShaper shaper;
    NoAssets assets;
    Context ctx(ContextConfig{&shaper, &assets});
    REQUIRE(ctx.loadDocument(IrBlob{first.ir.data(), first.ir.size()}, false).ok());
    REQUIRE(ctx.loadDocument(IrBlob{second.ir.data(), second.ir.size()}, false).ok());
    ctx.setViewport(Rect{0, 0, 64, 64}, 2.0f);
    ctx.update(0.25f);
    CommandList list;
    ctx.render(list);
    REQUIRE(list.shaders.size() == 2);
    REQUIRE(list.sources.size() == 2);
    REQUIRE(list.effectCmds.size() == 2);
    REQUIRE(list.time == doctest::Approx(0.25f));
    REQUIRE(list.frameSerial != 0);
    const CommandList original = list;

    SimplifyOptions options;
    options.effects = false;  // the custom effects stay in the list
    CommandListSimplifier simplifier;
    auto check = [&](const CommandList& result) {
        CHECK(result.time == doctest::Approx(0.25f));
        CHECK(result.viewport == original.viewport);
        CHECK(result.devicePixelRatio == doctest::Approx(2.0f));
        // The shader pools, as they were: the effects' shader indices still mean the same.
        REQUIRE(result.shaders.size() == original.shaders.size());
        for (size_t i = 0; i < original.shaders.size(); ++i) {
            const ShaderRef& a = original.shaders[i];
            const ShaderRef& b = result.shaders[i];
            CHECK(b.asset == a.asset);
            CHECK(b.source == a.source);
            CHECK(b.sourceOffset == a.sourceOffset);
            CHECK(b.sourceLength == a.sourceLength);
            CHECK(b.glslOffset == a.glslOffset);
            CHECK(b.glslLength == a.glslLength);
            CHECK(b.namesOffset == a.namesOffset);
            CHECK(b.namesLength == a.namesLength);
            CHECK(b.paramOffset == a.paramOffset);
            CHECK(b.paramCount == a.paramCount);
            CHECK(b.expandPx == a.expandPx);
            CHECK(b.fallback.kind == a.fallback.kind);
        }
        CHECK(result.shaderParams.size() == original.shaderParams.size());
        CHECK(result.shaderText == original.shaderText);
        // The source table and every command's index into it.
        REQUIRE(result.sources.size() == 2);
        CHECK(result.sources[0].ir == original.sources[0].ir);
        CHECK(result.sources[1].loadSerial == original.sources[1].loadSerial);
        REQUIRE(result.effectCmds.size() == 2);
        CHECK(result.effectCmds[0].source == 0);
        CHECK(result.effectCmds[1].source == 1);
        // A list the backends take without checking it again must be one that would pass.
        CHECK(result.structureChecked);
        CHECK(validateCommandList(result));
        // Not a damage base: the simplified list is not what Context::render produced.
        CHECK(result.frameSerial == 0);
    };

    SUBCASE("in place") {
        simplifier.simplify(list, options, &shaper);
        check(list);
    }
    SUBCASE("out of place") {
        CommandList out;
        simplifier.simplify(list, out, options, &shaper);
        check(out);
        // The input is only read: still the list Context::render() wrote.
        CHECK(list.frameSerial == original.frameSerial);
        CHECK(list.structureChecked);
        std::string before, after;
        dumpCommandList(original, before);
        dumpCommandList(list, after);
        CHECK(before == after);
    }
    SUBCASE("out of place, with the default options (the effects are dropped, the pools stay valid)") {
        CommandList out;
        const SimplifyStats stats = simplifier.simplify(list, out, SimplifyOptions{}, &shaper);
        CHECK(stats.droppedEffects == 2);
        CHECK(out.effectCmds.empty());
        CHECK(out.shaders.empty());  // nothing refers to them: not carried (as before T-263e)
        CHECK(out.shaderText.empty());
        CHECK(out.time == doctest::Approx(0.25f));
        CHECK(out.structureChecked);
        CHECK(validateCommandList(out));
        CHECK(out.frameSerial == 0);
    }
    SUBCASE("the same list as input and output is the in-place form") {
        simplifier.simplify(list, list, options, &shaper);
        check(list);
    }
    SUBCASE("a list that fails validation is copied as it is, unmarked") {
        CommandList broken = original;
        broken.structureChecked = false;
        broken.commands.push_back(Command{CmdKind::FillPath, {}, 0xFFFFu});  // no such fill
        CommandList out;
        const SimplifyStats stats = simplifier.simplify(broken, out, options, &shaper);
        CHECK(stats.droppedEffects == 0);
        CHECK(out.commands.size() == broken.commands.size());
        CHECK_FALSE(out.structureChecked);
        CHECK_FALSE(validateCommandList(out));
    }
}
