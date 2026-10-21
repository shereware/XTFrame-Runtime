#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "compiled_assets.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/lottie_parser.h"
#include "xt/compile/utf8_path.h"
#include "xt/version.h"

/// `compileLottie`: standalone Lottie -> `.xtir` (Scene+Timeline+a 3-element DOCT wrapping it in an
/// `<anim>`), for xtframe-view's direct preview use case. `compileDocument` (full `.xt` documents) is
/// left for T-23. See docs/design/12-compiler.md §12.1, docs/tasks/T-16.md "compile.h".

namespace xt::compile {

namespace {

bool readFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in.good()) {
        return false;
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        in.read(reinterpret_cast<char*>(out.data()), size);
    }
    return static_cast<bool>(in) || in.eof();
}

}  // namespace

CompileResult compileLottie(std::string_view jsonPath, const CompileOptions& options) {
    CompileResult result;
    std::string path(jsonPath);
    DiagnosticSink sink;

    std::vector<uint8_t> bytes;
    if (!readFileBytes(path, bytes)) {
        sink.error(path, "failed to read file");
        result.diagnostics = sink.all();
        return result;
    }

    IrWriter writer;
    LottieOptions lopts;
    lopts.strict = options.strict;
    lopts.sourceName = pathToUtf8(pathFromUtf8(path).filename());
    lopts.rootDir = options.rootDir;
    lopts.embedImages = options.embedImages;

    LottieResult lresult =
        parseLottie(std::span<const uint8_t>(bytes.data(), bytes.size()), lopts, writer, sink);
    if (!lresult.ok) {
        result.diagnostics = sink.all();
        return result;
    }

    // Empty STYL: no <link>/<style> in a standalone Lottie compile.
    writer.setStyle(StyleBuilder{});

    // DOCT: xt > body > anim (scene 0, autoplay/loop true).
    DocBuilder doc;

    StringId trueId = writer.intern("true");
    StringId autoplayName = writer.intern("autoplay");
    StringId loopName = writer.intern("loop");

    IrDocElement xtEl{};
    xtEl.parent = kInvalidNode;
    xtEl.tag = writer.intern("xt");
    xtEl.id = kInvalidString;
    xtEl.classOffset = xtEl.classCount = 0;
    xtEl.attrOffset = xtEl.attrCount = 0;
    xtEl.inlineDeclOffset = xtEl.inlineDeclCount = 0;
    xtEl.scene = kInvalidScene;
    xtEl.firstTextChild = kInvalidNode;
    xtEl.cameraIndex = kInvalidNode;
    doc.elements.push_back(xtEl);

    IrDocElement bodyEl{};
    bodyEl.parent = 0;
    bodyEl.tag = writer.intern("body");
    bodyEl.id = kInvalidString;
    bodyEl.classOffset = bodyEl.classCount = 0;
    bodyEl.attrOffset = bodyEl.attrCount = 0;
    bodyEl.inlineDeclOffset = bodyEl.inlineDeclCount = 0;
    bodyEl.scene = kInvalidScene;
    bodyEl.firstTextChild = kInvalidNode;
    bodyEl.cameraIndex = kInvalidNode;
    doc.elements.push_back(bodyEl);

    IrDocElement animEl{};
    animEl.parent = 1;
    animEl.tag = writer.intern("anim");
    animEl.id = kInvalidString;
    animEl.classOffset = animEl.classCount = 0;
    animEl.attrOffset = static_cast<uint32_t>(doc.attrPool.size());
    doc.attrPool.push_back(IrAttr{autoplayName, trueId});
    doc.attrPool.push_back(IrAttr{loopName, trueId});
    animEl.attrCount = 2;
    animEl.inlineDeclOffset = animEl.inlineDeclCount = 0;
    animEl.scene = lresult.scene;
    animEl.firstTextChild = kInvalidNode;
    animEl.cameraIndex = kInvalidNode;
    doc.elements.push_back(animEl);

    writer.setDoc(std::move(doc));

    if (options.emitMeta) {
        StringId sourceFileId = writer.intern(lopts.sourceName);
        StringId versionId = writer.intern(std::string("xtframe-compile ") + versionString());
        // <ctime> instead of <chrono>: libstdc++ 14's <chrono> pulls in <format>, which clang 15 cannot
        // compile.
        uint64_t unixTime = static_cast<uint64_t>(std::time(nullptr));
        writer.setMeta(sourceFileId, versionId, unixTime, options.behavior);  // T-407
    }

    result.assets = collectCompiledAssets(writer);
    result.ir = writer.finish();
    result.diagnostics = sink.all();
    return result;
}

}  // namespace xt::compile
