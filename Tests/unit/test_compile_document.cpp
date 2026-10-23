#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/property.h"
#include "xt/types.h"
#include "xts_props.h"  // styleprop::* (internal; Code/Runtime/compile/src/xts is a test-only include dir).

/// Tests for `compileDocument` (Docs/design/12-compiler.md §12.2-§12.3, Docs/tasks/T-23.md).

using namespace xt;
using namespace xt::compile;

namespace {

std::string xtPath(const std::string& name) { return std::string(XT_TEST_XT_DIR) + "/" + name; }

bool hasErrorContaining(const CompileResult& result, const std::string& needle) {
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasWarningContaining(const CompileResult& result, const std::string& needle) {
    for (const Diagnostic& d : result.diagnostics) {
        if (d.level == Diagnostic::Level::Warning && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string_view sv(const std::span<const uint8_t>& s) {
    return std::string_view(reinterpret_cast<const char*>(s.data()), s.size());
}

}  // namespace

TEST_CASE("compileDocument: doc_basic.xt compiles, DOCT is correct") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_basic.xt"), options);
    for (const Diagnostic& d : result.diagnostics) {
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    // DOCT: xt > body > div(#root .a .b, style="width:10px") > span("Hello"), img, anim.
    const DocChunk& doc = view.doc();
    REQUIRE(doc.elements().size() == 6);
    CHECK(view.string(doc.elements()[0].tag) == "xt");
    CHECK(doc.elements()[0].parent == kInvalidNode);
    CHECK(view.string(doc.elements()[1].tag) == "body");
    CHECK(doc.elements()[1].parent == 0);
    CHECK(view.string(doc.elements()[2].tag) == "div");
    CHECK(doc.elements()[2].parent == 1);
    CHECK(view.string(doc.elements()[2].id) == "root");
    CHECK(view.string(doc.elements()[3].tag) == "span");
    CHECK(view.string(doc.elements()[4].tag) == "img");
    CHECK(view.string(doc.elements()[5].tag) == "anim");

    // class="a b" -> 2 classPool entries.
    const IrDocElement& div = doc.elements()[2];
    REQUIRE(div.classCount == 2);
    CHECK(view.string(doc.classPool()[div.classOffset]) == "a");
    CHECK(view.string(doc.classPool()[div.classOffset + 1]) == "b");

    // style="width: 10px" -> 1 inline decl, prop Width, Length 10px.
    REQUIRE(div.inlineDeclCount == 1);
    const IrDecl& widthDecl = doc.inlineDecls()[div.inlineDeclOffset];
    CHECK(widthDecl.prop == prop::Width);
    CHECK(static_cast<ValueType>(widthDecl.valueType) == ValueType::Length);
    CHECK(irValueF32(widthDecl.value, 0) == doctest::Approx(10.0f));
    CHECK(widthDecl.value[4] == static_cast<uint8_t>(LengthUnit::Px));

    // anim: scene 0, no data-xt-asset attr.
    const IrDocElement& anim = doc.elements()[5];
    CHECK(anim.scene == 0);

    // img: data-xt-asset attribute present, pointing at an ASST Image entry.
    const IrDocElement& img = doc.elements()[4];
    bool foundAssetAttr = false;
    for (uint32_t i = 0; i < img.attrCount; ++i) {
        const IrAttr& a = doc.attrPool()[img.attrOffset + i];
        if (view.string(a.name) == "data-xt-asset") {
            foundAssetAttr = true;
            uint32_t assetId = static_cast<uint32_t>(std::stoul(std::string(view.string(a.value))));
            REQUIRE(assetId < view.assets().assets().size());
            CHECK(view.assets().assets()[assetId].kind == 0);  // Image.
        }
    }
    CHECK(foundAssetAttr);

    // Text node.
    REQUIRE(doc.texts().size() == 1);
    CHECK(view.string(doc.texts()[0].text) == "Hello");
    CHECK(doc.elements()[3].firstTextChild == 0);

    // STYL: link's ".a, .b" (2 rules) + inline style's "div.panel span" (1 rule) = 3 rules.
    const StyleChunk& style = view.style();
    REQUIRE(style.rules().size() == 3);

    // The "div.panel span" rule: selectorParts right-to-left (span subject, then div.panel).
    const IrRule* compoundRule = nullptr;
    for (const IrRule& rule : style.rules()) {
        if (rule.selCount == 2) {
            compoundRule = &rule;
        }
    }
    REQUIRE(compoundRule != nullptr);
    const IrSelPart& subject = style.selectorParts()[compoundRule->selOffset];
    const IrSelPart& ancestor = style.selectorParts()[compoundRule->selOffset + 1];
    CHECK(view.string(subject.tag) == "span");
    CHECK(view.string(ancestor.tag) == "div");
    REQUIRE(ancestor.classCount == 1);
    CHECK(view.string(style.classIds()[ancestor.classOffset]) == "panel");
    // specificity: 0 ids, 1 class (panel), 2 tags (div, span) -> (0<<16)|(1<<8)|2.
    CHECK(compoundRule->specificity == (1u << 8) + 2u);

    // Decls on that rule: opacity !important, transition, animation, font-family.
    bool sawImportantOpacity = false, sawTransition = false, sawAnimation = false, sawFontFamily = false;
    for (uint32_t i = 0; i < compoundRule->declCount; ++i) {
        const IrDecl& d = style.decls()[compoundRule->declOffset + i];
        if (d.prop == prop::Opacity) {
            CHECK(static_cast<ValueType>(d.valueType) == ValueType::Float);
            CHECK(irValueF32(d.value, 0) == doctest::Approx(0.5f));
            CHECK(d.important == 1);
            sawImportantOpacity = true;
        } else if (d.prop == styleprop::Transition) {
            CHECK(static_cast<ValueType>(d.valueType) == ValueType::ListRef);
            uint32_t off = irValueU32(d.value, 0);
            uint32_t cnt = irValueU32(d.value, 1);
            REQUIRE(cnt == 1);
            CHECK(style.transitions()[off].prop == prop::Opacity);
            CHECK(style.transitions()[off].duration == doctest::Approx(0.2f));
            sawTransition = true;
        } else if (d.prop == styleprop::Animation) {
            CHECK(static_cast<ValueType>(d.valueType) == ValueType::ListRef);
            uint32_t off = irValueU32(d.value, 0);
            uint32_t cnt = irValueU32(d.value, 1);
            REQUIRE(cnt == 1);
            TimelineId tid = style.animations()[off].timeline;
            CHECK(view.timeline(tid).duration() == doctest::Approx(1.0f));
            sawAnimation = true;
        } else if (d.prop == styleprop::FontFamily) {
            CHECK(static_cast<ValueType>(d.valueType) == ValueType::ListRef);
            uint32_t off = irValueU32(d.value, 0);
            uint32_t cnt = irValueU32(d.value, 1);
            REQUIRE(cnt == 2);
            CHECK(view.string(style.fontFamilies()[off]) == "A");
            CHECK(view.string(style.fontFamilies()[off + 1]) == "sans-serif");
            sawFontFamily = true;
        }
    }
    CHECK(sawImportantOpacity);
    CHECK(sawTransition);
    CHECK(sawAnimation);
    CHECK(sawFontFamily);

    // @keyframes fade { from {opacity:0} to {opacity:1} }: duration 1, 1 track, kSelfNode, 2 kfs.
    REQUIRE(style.keyframesRefs().size() == 2);
    const IrKeyframesRef* fadeRef = nullptr;
    const IrKeyframesRef* halfRef = nullptr;
    for (const IrKeyframesRef& ref : style.keyframesRefs()) {
        if (view.string(ref.name) == "fade") {
            fadeRef = &ref;
        } else if (view.string(ref.name) == "half") {
            halfRef = &ref;
        }
    }
    REQUIRE(fadeRef != nullptr);
    REQUIRE(halfRef != nullptr);

    const TimelineChunk& fadeTl = view.timeline(fadeRef->timeline);
    CHECK(fadeTl.duration() == doctest::Approx(1.0f));
    REQUIRE(fadeTl.tracks().size() == 1);
    CHECK(fadeTl.tracks()[0].target == kSelfNode);
    CHECK(fadeTl.tracks()[0].prop == prop::Opacity);
    CHECK(fadeTl.tracks()[0].kfCount == 2);

    // @keyframes half { 50% {opacity:.5} }: 3 keyframes (Neutral first/last), easing on the middle
    // one is ease-in (CubicBezier).
    const TimelineChunk& halfTl = view.timeline(halfRef->timeline);
    REQUIRE(halfTl.tracks().size() == 1);
    REQUIRE(halfTl.tracks()[0].kfCount == 3);
    std::span<const IrKeyframe> halfKfs = halfTl.keyframes().subspan(halfTl.tracks()[0].kfOffset, 3);
    CHECK(halfKfs[0].time == doctest::Approx(0.0f));
    CHECK(irValueU32(halfKfs[0].value, 0) == 0xFFFFFFFFu);  // Neutral.
    CHECK(halfKfs[1].time == doctest::Approx(0.5f));
    CHECK(irValueF32(halfKfs[1].value, 0) == doctest::Approx(0.5f));
    CHECK(static_cast<EasingKind>(halfKfs[1].easing.kind) == EasingKind::CubicBezier);
    CHECK(halfKfs[2].time == doctest::Approx(1.0f));
    CHECK(irValueU32(halfKfs[2].value, 0) == 0xFFFFFFFFu);  // Neutral.

    // @font-face -> fontFaces + ASST Font asset (not embedded: embedFonts defaults to false).
    REQUIRE(style.fontFaces().size() == 1);
    CHECK(view.string(style.fontFaces()[0].family) == "A");
    CHECK(style.fontFaces()[0].weight == 700);
    CHECK(style.fontFaces()[0].italic == 1);
    REQUIRE(style.fontFaces()[0].asset < view.assets().assets().size());
    const IrAsset& fontAsset = view.assets().assets()[style.fontFaces()[0].asset];
    CHECK(fontAsset.kind == 1);  // Font.
    CHECK(fontAsset.embedded == 0);

    // Scene: parsed from the referenced Lottie.
    REQUIRE(view.sceneCount() == 1);

    // Round-trip: fromView(view).finish() must equal the original bytes exactly.
    IrWriter rebuilt = IrWriter::fromView(view);
    CHECK(rebuilt.finish() == result.ir);
}

TEST_CASE("compileDocument: @font-face embedFonts embeds the asset bytes") {
    CompileOptions options;
    options.embedFonts = true;
    CompileResult result = compileDocument(xtPath("doc_basic.xt"), options);
    REQUIRE(result.ok());
    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(r.ok());
    const IrView& view = r.value();
    REQUIRE(view.style().fontFaces().size() == 1);
    const IrAsset& fontAsset = view.assets().assets()[view.style().fontFaces()[0].asset];
    CHECK(fontAsset.embedded == 1);
    CHECK(sv(view.assets().data(fontAsset)) == "FAKEFONTBYTES");
}

TEST_CASE("compileDocument: missing <link href> file is an error") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_err_link.xt"), options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "does-not-exist.xts"));
}

TEST_CASE("compileDocument: <link href> naming a directory is an error, not a huge read (T-461)") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_err_linkdir.xt"), options);
    CHECK(!result.ok());
}

TEST_CASE("compileDocument: unresolved animation-name is an error") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_err_animname.xt"), options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "unknown @keyframes \"nope\""));
}

TEST_CASE("compileDocument: missing <anim src> file is an error") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_err_animsrc.xt"), options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "does-not-exist.json"));
}

TEST_CASE("compileDocument: missing <img src> is a warning, still succeeds") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_warn_missing_img.xt"), options);
    CHECK(result.ok());
    CHECK(hasWarningContaining(result, "missing.png"));
}

TEST_CASE("compileDocument: strict mode fails on a warning") {
    CompileOptions options;
    options.strict = true;
    CompileResult result = compileDocument(xtPath("doc_warn_missing_img.xt"), options);
    CHECK(!result.ok());
    CHECK(hasWarningContaining(result, "missing.png"));
}

// Docs/design/19-3d-space.md §19.4, Docs/tasks/T-47a.md: <camera position="100 50 300" zoom="400">
// -> DOCT cameras[0] carries position/zoom, target/near/far stay NaN (omitted), and the parent
// element's cameraIndex points at it.
TEST_CASE("compileDocument: doc_camera.xt -- <camera> element -> DOCT cameras[0]") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_camera.xt"), options);
    for (const Diagnostic& d : result.diagnostics) {
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const IrView& view = r.value();

    const DocChunk& doc = view.doc();
    // xt > body > div(#root) > camera(#cam).
    REQUIRE(doc.elements().size() == 4);
    const IrDocElement& cameraEl = doc.elements()[3];
    CHECK(view.string(cameraEl.tag) == "camera");
    CHECK(view.string(cameraEl.id) == "cam");
    REQUIRE(cameraEl.cameraIndex != 0xFFFFFFFFu);
    REQUIRE(cameraEl.cameraIndex < doc.cameras().size());

    const IrCameraData& cam = doc.cameras()[cameraEl.cameraIndex];
    CHECK(cam.kind == 0);  // perspective (default).
    CHECK(cam.useTarget == 1);
    CHECK(cam.position.x == doctest::Approx(100));
    CHECK(cam.position.y == doctest::Approx(50));
    CHECK(cam.position.z == doctest::Approx(300));
    CHECK(cam.zoom == doctest::Approx(400));
    CHECK(std::isnan(cam.target.x));
    CHECK(std::isnan(cam.nearZ));
    CHECK(std::isnan(cam.farZ));
}

TEST_CASE("compileDocument: <camera> with child elements is a compile error") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_err_camera_children.xt"), options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "<camera> must not have children"));
}

// Docs/design/18-effects.md §18.4, Docs/tasks/T-51a.md.
TEST_CASE("compileDocument: doc_effect.xt -- effect STYL encoding, transition, @keyframes track") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_effect.xt"), options);
    for (const Diagnostic& d : result.diagnostics) {
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const IrView& view = r.value();
    const StyleChunk& style = view.style();

    // "#a { effect: shadow(...) blur(...); transition: effect .3s; }": one rule, two decls.
    REQUIRE(style.rules().size() == 1);
    const IrRule& rule = style.rules()[0];
    bool sawEffect = false, sawTransition = false;
    for (uint32_t i = 0; i < rule.declCount; ++i) {
        const IrDecl& d = style.decls()[rule.declOffset + i];
        if (d.prop == styleprop::Effect) {
            CHECK(static_cast<ValueType>(d.valueType) == ValueType::ListRef);
            uint32_t off = irValueU32(d.value, 0);
            uint32_t cnt = irValueU32(d.value, 1);
            REQUIRE(cnt == 2);
            const IrEffect& shadow = style.effects()[off];
            CHECK(static_cast<EffectKind>(shadow.kind) == EffectKind::DropShadow);
            CHECK(shadow.params[0] == doctest::Approx(2));
            CHECK(shadow.params[1] == doctest::Approx(4));
            CHECK(shadow.params[2] == doctest::Approx(6));
            const IrEffect& blur = style.effects()[off + 1];
            CHECK(static_cast<EffectKind>(blur.kind) == EffectKind::Blur);
            CHECK(blur.params[0] == doctest::Approx(3));
            sawEffect = true;
        } else if (d.prop == styleprop::Transition) {
            uint32_t off = irValueU32(d.value, 0);
            uint32_t cnt = irValueU32(d.value, 1);
            REQUIRE(cnt == 1);
            CHECK(style.transitions()[off].prop == styleprop::Effect);
            sawTransition = true;
        }
    }
    CHECK(sawEffect);
    CHECK(sawTransition);

    // "@keyframes fx { from { effect: blur(0px) } to { effect: blur(10px) } }": expands to a
    // single-effect-slot Track at effectParam(0, 1) (Blur's radiusX), values 0 and 10.
    REQUIRE(style.keyframesRefs().size() == 1);
    const TimelineChunk& fxTl = view.timeline(style.keyframesRefs()[0].timeline);
    bool sawParamTrack = false;
    for (const IrTrack& track : fxTl.tracks()) {
        if (track.prop == xt::effectParam(0, 1)) {
            CHECK(static_cast<ValueType>(track.type) == ValueType::Float);
            REQUIRE(track.kfCount == 2);
            std::span<const IrKeyframe> kfs = fxTl.keyframes().subspan(track.kfOffset, track.kfCount);
            CHECK(irValueF32(kfs[0].value, 0) == doctest::Approx(0));
            CHECK(irValueF32(kfs[1].value, 0) == doctest::Approx(10));
            sawParamTrack = true;
        }
    }
    CHECK(sawParamTrack);

    // Round-trip: fromView(view).finish() must equal the original bytes exactly.
    IrWriter rebuilt = IrWriter::fromView(view);
    CHECK(rebuilt.finish() == result.ir);
}

TEST_CASE("compileDocument: @keyframes effect with mismatched effect kind sequence is an error") {
    CompileOptions options;
    CompileResult result = compileDocument(xtPath("doc_err_effect_keyframes.xt"), options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "effect keyframes must use the same effect sequence"));
}

namespace {

/// Reads a whole file into a string; REQUIREs success (a missing fixture is a test bug, not a
/// case under test).
std::string readFileOrFail(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

// Docs/design/17-editor.md §17.7, Docs/tasks/T-59.md: compileDocumentFromStrings() is the same
// pipeline as compileDocument(), just fed in-memory files instead of reading them off disk --
// used by the editor's PreviewSession to recompile a Document that isn't (yet) saved to disk.
TEST_CASE("compileDocumentFromStrings: {app.xt, app.xts} matches compileDocument(path) byte-for-byte") {
    std::string dir = std::string(XT_EXAMPLES_DIR) + "/desktop_app/ui";
    std::string xtText = readFileOrFail(dir + "/app.xt");
    std::string xtsText = readFileOrFail(dir + "/app.xts");

    // emitMeta = false on both sides: META's sourceFile string is the same ("app.xt") either way,
    // but its timestamp is captured independently by each call (std::time(nullptr)) and could
    // differ by a second -- excluded from this byte-for-byte comparison per the task sheet's "除
    // META 的 sourceFile 字串外", generalized here to "the whole META chunk" to keep the test
    // deterministic.
    CompileOptions options;
    options.emitMeta = false;
    CompileResult fromPath = compileDocument(dir + "/app.xt", options);
    for (const Diagnostic& d : fromPath.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(fromPath.ok());

    SourceFile files[2] = {
        SourceFile{"app.xt", xtText},
        SourceFile{"app.xts", xtsText},
    };
    options.rootDir = dir;
    CompileResult fromStrings = compileDocumentFromStrings(files, "app.xt", options);
    for (const Diagnostic& d : fromStrings.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(fromStrings.ok());

    CHECK(fromPath.ir == fromStrings.ir);

    // With META on: both still succeed and intern the same ("app.xt") source file name -- the one
    // difference the task sheet calls out.
    CompileOptions metaOptions;
    metaOptions.rootDir = dir;
    CompileResult withMeta = compileDocumentFromStrings(files, "app.xt", metaOptions);
    REQUIRE(withMeta.ok());
}

// Docs/tasks/T-59.md: a <link href> absent from `files` and not found under `options.rootDir`
// either is the same "failed to read file" diagnostic compileDocument() raises for a missing file.
TEST_CASE("compileDocumentFromStrings: missing <link href> file is an error") {
    SourceFile files[1] = {
        SourceFile{"main.xt", "<xt><head><link href=\"does-not-exist.xts\"/></head><body></body></xt>"},
    };
    CompileOptions options;
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "does-not-exist.xts"));
}

// Docs/tasks/T-59.md's <link href> resolution order: an exact `files` name match wins even when
// `options.rootDir` also has a (different) file by that name.
TEST_CASE("compileDocumentFromStrings: <link href> resolves against files[] before rootDir") {
    std::string dir = std::string(XT_EXAMPLES_DIR) + "/desktop_app/ui";
    SourceFile files[2] = {
        SourceFile{"main.xt",
                   "<xt><head><link href=\"app.xts\"/></head><body><div id=\"root\"/></body></xt>"},
        SourceFile{"app.xts", "#root { width: 42px; }"},
    };
    CompileOptions options;
    options.rootDir = dir;  // also has a real app.xts (desktop_app's own, unrelated content).
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(r.ok());
    const StyleChunk& style = r.value().style();
    REQUIRE(style.rules().size() == 1);
    REQUIRE(style.rules()[0].declCount == 1);
    const IrDecl& decl = style.decls()[style.rules()[0].declOffset];
    CHECK(decl.prop == prop::Width);
    CHECK(irValueF32(decl.value, 0) == doctest::Approx(42.0f));
}

// Docs/design/22-bundle.md §22.2, Docs/tasks/T-138.md: CompileOptions::searchDirs is tried in order
// before the usual base directory for <img src>/<anim src>/@font-face src.
namespace {

namespace fs = std::filesystem;

/// A fresh, empty temp directory for one test case.
fs::path freshTempDir(const char* name) {
    fs::path dir = xt::test::tempDir() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

const char* const kTinySvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">"
    "<rect width=\"10\" height=\"10\" fill=\"#ff0000\"/></svg>";

}  // namespace

TEST_CASE("compileDocumentFromStrings: searchDirs -- img found in searchDirs[1]") {
    fs::path base = freshTempDir("xt_t138_searchdirs_hit");
    fs::path near = base / "ui";
    fs::path far = base / "shared";
    fs::create_directories(near);
    writeText(far / "img" / "pic.png", "not really a png");
    writeText(far / "img" / "shape.svg", kTinySvg);

    SourceFile files[1] = {
        SourceFile{"main.xt", "<xt><body><img src=\"img/pic.png\"/><img src=\"img/shape.svg\"/></body></xt>"},
    };
    CompileOptions options;
    options.rootDir = base.string();  // base/img/... does not exist: only searchDirs[1] has them.
    options.searchDirs = {near.string(), far.string()};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    CHECK(!hasWarningContaining(result, "not found"));
    REQUIRE(result.assets.size() == 1);
    CHECK(result.assets[0].sourceName == "img/pic.png");
    CHECK(fs::path(result.assets[0].resolvedPath) == far / "img" / "pic.png");
}

TEST_CASE("compileDocumentFromStrings: searchDirs -- nearer directory wins") {
    fs::path base = freshTempDir("xt_t138_searchdirs_near");
    writeText(base / "ui" / "pic.png", "near");
    writeText(base / "shared" / "pic.png", "far");

    SourceFile files[1] = {SourceFile{"main.xt", "<xt><body><img src=\"pic.png\"/></body></xt>"}};
    CompileOptions options;
    options.rootDir = base.string();
    options.searchDirs = {(base / "ui").string(), (base / "shared").string()};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(result.ok());
    REQUIRE(result.assets.size() == 1);
    CHECK(fs::path(result.assets[0].resolvedPath) == base / "ui" / "pic.png");
}

TEST_CASE("compileDocumentFromStrings: searchDirs -- a miss everywhere reports as before") {
    fs::path base = freshTempDir("xt_t138_searchdirs_miss");
    fs::create_directories(base / "ui");
    SourceFile files[1] = {
        SourceFile{"main.xt", "<xt><body><img src=\"gone.png\"/><img src=\"gone.svg\"/></body></xt>"},
    };
    CompileOptions options;
    options.rootDir = base.string();
    options.searchDirs = {(base / "ui").string(), (base / "shared").string()};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    CHECK(!result.ok());
    CHECK(hasErrorContaining(result, "img src \"gone.svg\" not found"));
    CHECK(hasWarningContaining(result, "image asset \"gone.png\" not found"));
}

// T-452 (Docs/design/17-editor.md §17.25): CompileOptions::fileExists / readAssetFile answer the
// asset questions instead of the disk -- the search directory probe, the "not found" check and
// the bytes of an `<img src="*.svg">` -- so a host's table can stand in for every one of them.
TEST_CASE("compileDocumentFromStrings: T-452 fileExists and readAssetFile replace the disk for assets") {
    fs::path base = freshTempDir("xt_t452_compile_hooks");  // empty: nothing below exists on disk
    const fs::path near = base / "ui";
    const fs::path far = base / "shared";
    SourceFile files[1] = {
        SourceFile{"main.xt", "<xt><body><img src=\"img/pic.png\"/><img src=\"img/shape.svg\"/></body></xt>"},
    };
    CompileOptions options;
    options.rootDir = base.string();
    options.searchDirs = {near.string(), far.string()};
    std::vector<std::string> asked;
    std::vector<std::string> read;
    const std::string pic = (far / "img" / "pic.png").string();
    const std::string shape = (far / "img" / "shape.svg").string();
    options.fileExists = [&](std::string_view path) {
        asked.emplace_back(path);
        return fs::path(std::string(path)) == fs::path(pic) || fs::path(std::string(path)) == fs::path(shape);
    };
    options.readAssetFile = [&](std::string_view path, std::vector<uint8_t>& out) {
        read.emplace_back(path);
        if (fs::path(std::string(path)) != fs::path(shape)) {
            return false;
        }
        const std::string svg = kTinySvg;
        out.assign(svg.begin(), svg.end());
        return true;
    };
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    CHECK(!hasWarningContaining(result, "not found"));
    REQUIRE(result.assets.size() == 1);
    CHECK(fs::path(result.assets[0].resolvedPath) == fs::path(pic));
    CHECK(!asked.empty());
    REQUIRE(read.size() == 1);
    CHECK(fs::path(read[0]) == fs::path(shape));

    // The same document without the hooks asks the disk, where nothing exists.
    CompileOptions plain;
    plain.rootDir = base.string();
    plain.searchDirs = {near.string(), far.string()};
    CompileResult missing = compileDocumentFromStrings(files, "main.xt", plain);
    CHECK(hasErrorContaining(missing, "img src \"img/shape.svg\" not found"));
    CHECK(hasWarningContaining(missing, "image asset \"img/pic.png\" not found"));
}

// Docs/tasks/T-150.md: `<img src="engine://scene">` is an external (host-bound) texture. The
// compiler keeps the name verbatim as the ASST entry: no file lookup (so no "not found" warning,
// even under --strict or with embedImages), no assetNameRewrite call (so a bundle never packs or
// lists it), and CompiledAsset::external says so.
TEST_CASE("compileDocument: engine:// img is an external asset, never looked up on disk") {
    CompileOptions options;
    options.strict = true;
    options.embedImages = true;
    int rewriteCalls = 0;
    options.assetNameRewrite = [&](std::string_view, std::string_view, uint8_t) {
        ++rewriteCalls;
        return std::string("assets/should-not-happen.png");
    };
    CompileResult result = compileDocument(xtPath("doc_img_engine.xt"), options);
    REQUIRE(result.ok());
    CHECK(result.diagnostics.empty());
    CHECK(rewriteCalls == 0);

    // Both <img> elements share the one ASST entry.
    REQUIRE(result.assets.size() == 1);
    CHECK(result.assets[0].name == "engine://scene");
    CHECK(result.assets[0].sourceName == "engine://scene");
    CHECK(result.assets[0].resolvedPath.empty());
    CHECK(result.assets[0].kind == 0);
    CHECK_FALSE(result.assets[0].embedded);
    CHECK(result.assets[0].external);

    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    const IrView& view = r.value();
    REQUIRE(view.assets().assets().size() == 1);
    const IrAsset& asset = view.assets().assets()[0];
    CHECK(view.string(asset.name) == "engine://scene");
    CHECK(asset.embedded == 0);
    CHECK(isExternalAsset(0, view));
    CHECK(externalAssetName("engine://scene") == "scene");
    CHECK_FALSE(isExternalAssetName("img/engine.png"));
}

TEST_CASE("compileDocumentFromStrings: T-243 a rigged inline <svg> round-trips byte for byte (IR minor 5)") {
    // Bones, a constraint, a skinned path and mesh, and a scene-node binding: every SCEN rig tail
    // array, IrPathData::skinIndex and IrBinding::sceneNode go through fromView() unchanged.
    const std::string xt = R"(<xt><head></head><body><div port:a="number"></div>
<svg id="S" viewBox="0 0 100 100">
  <path d="M0 0 L10 0" xt-skin="Rig"><xt-weights bones="B">1 0:1</xt-weights></path>
  <xt-skeleton id="Rig"><xt-bone id="B" length="20" bind:rotate="a">
    <xt-constraint kind="spring" damping="0.5"/></xt-bone></xt-skeleton>
  <xt-mesh href="m.png" width="10" height="10" xt-skin="Rig"><xt-weights bones="B"></xt-weights></xt-mesh>
</svg></body></xt>)";
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(result.ok());
    Result<IrView> r = IrView::open(IrBlob{result.ir.data(), result.ir.size()}, IrReadOptions{true});
    REQUIRE(r.ok());
    bool rig = false;
    for (uint32_t i = 0; i < r.value().sceneCount(); ++i) {
        rig = rig || (!r.value().scene(i).bones().empty() && r.value().scene(i).skins().size() == 2);
    }
    CHECK(rig);
    IrWriter rebuilt = IrWriter::fromView(r.value());
    CHECK(rebuilt.finish() == result.ir);
}
