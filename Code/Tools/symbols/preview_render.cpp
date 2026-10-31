#include "preview_render.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/charconv.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_png.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"
#ifdef XT_SYMBOLS_HAS_TEXT
#include "xt/text/ft_text_shaper.h"
#endif

namespace xt::tools::symbols {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;

namespace {

/// Splits a script line into its command word and whitespace separated arguments; false for a blank
/// or `#` comment line.
bool splitLine(const std::string& line, std::string& command, std::vector<std::string>& args) {
    std::istringstream words(line);
    command.clear();
    args.clear();
    if (!(words >> command) || command.front() == '#') {
        return false;
    }
    std::string word;
    while (words >> word) {
        args.push_back(word);
    }
    return true;
}

/// One line of the event script, the subset a preview needs (`Tests/golden`'s grammar): `port`,
/// `placeholders`, `update`. `error` names what is wrong; each `port` and the line itself end in the
/// zero-length update the golden harness runs.
bool runLine(Context& ctx, Document& doc, const std::string& line, std::string& error) {
    std::string command;
    std::vector<std::string> args;
    if (!splitLine(line, command, args)) {
        return true;
    }
    if (command == "placeholders" && args.empty()) {
        ctx.setExternalPlaceholders(true);
        return true;
    }
    if (command == "port" && args.size() >= 2) {
        std::string value = args[1];
        for (size_t i = 2; i < args.size(); ++i) {
            value += " " + args[i];
        }
        const PortId port = doc.lookupPort(args[0]);
        if (port == kInvalidPort || doc.setPortText(port, value) != Status::kOk) {
            error = "cannot set port '" + args[0] + "' to '" + value + "'";
            return false;
        }
        ctx.update(0.0f);
        return true;
    }
    if (command == "update" && args.size() == 1) {
        float seconds = 0;
        const char* first = args[0].data();
        const char* last = first + args[0].size();
        const std::from_chars_result parsed = fromChars(first, last, seconds);
        if (parsed.ec != std::errc() || parsed.ptr != last || seconds < 0.0f || seconds > 60.0f) {
            error = "bad update time '" + args[0] + "'";
            return false;
        }
        ctx.update(seconds);
        return true;
    }
    error = "unsupported event line '" + line + "'";
    return false;
}

}  // namespace

bool renderPreview(const fs::path& groupDir, const bundle::PreviewSpec& spec, Image& out,
                   std::string& error) {
    out = Image();
    error.clear();
    const fs::path xtPath = groupDir / pathFromUtf8(spec.xtFile);
    compile::CompileOptions options;
    options.rootDir = genericPathToUtf8(xtPath.parent_path());
    options.strict = false;
    options.searchDirs = {genericPathToUtf8(groupDir)};
    const compile::CompileResult result = compile::compileDocument(genericPathToUtf8(xtPath), options);
    if (!result.ok()) {
        error = spec.xtFile + " does not compile";
        for (const compile::Diagnostic& d : result.diagnostics) {
            if (d.level == compile::Diagnostic::Level::Error) {
                error += ": " + d.message;
                break;
            }
        }
        return false;
    }

#ifdef XT_SYMBOLS_HAS_TEXT
    text::FtTextShaper shaper;
#else
    NullTextShaper shaper;
#endif
    FileAssetResolver resolver({genericPathToUtf8(xtPath.parent_path()), genericPathToUtf8(groupDir)},
                               sw::decodeImageBytes);
    Context ctx(ContextConfig{&shaper, &resolver});
    ctx.setViewport(Rect{0, 0, static_cast<float>(spec.width), static_cast<float>(spec.height)}, 1.0f);
    Result<DocumentHandle> handle =
        ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/true);
    if (!handle.ok()) {
        error = spec.xtFile + " does not load (" + statusName(handle.status()) + ")";
        return false;
    }
    Document* doc = ctx.document(handle.value());
    if (doc == nullptr) {
        error = "the loaded document is missing";
        return false;
    }
    ctx.update(0.0f);
    std::istringstream script(bundle::previewEventScript(spec));
    std::string line;
    while (std::getline(script, line)) {
        if (!runLine(ctx, *doc, line, error)) {
            return false;
        }
    }
    for (int frame = 0; frame < spec.frames; ++frame) {
        ctx.update(1.0f / 60.0f);
    }

    CommandList commands;
    ctx.render(commands);
    out.width = spec.width;
    out.height = spec.height;
    out.rgba.assign(static_cast<size_t>(spec.width) * static_cast<size_t>(spec.height) * 4u, 0);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{out.rgba.data(), spec.width, spec.height, spec.width * 4});
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(commands, &shaper, &resolver, &doc->ir());
    return true;
}

bool composeGroupThumbnail(const std::vector<Image>& cells, int width, int height, Image& out) {
    out = Image();
    if (cells.empty() || width < 2 || height < 2) {
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 255);
    const int cellW = width / 2;
    const int cellH = height / 2;
    for (size_t index = 0; index < cells.size() && index < 4; ++index) {
        const Image& src = cells[index];
        if (src.width < 1 || src.height < 1 ||
            src.rgba.size() != static_cast<size_t>(src.width) * static_cast<size_t>(src.height) * 4u) {
            continue;
        }
        const int cellX = static_cast<int>(index % 2) * cellW;
        const int cellY = static_cast<int>(index / 2) * cellH;
        // The margin: the colour of the image's top-left pixel.
        for (int y = 0; y < cellH; ++y) {
            for (int x = 0; x < cellW; ++x) {
                std::copy_n(src.rgba.data(), 4,
                            out.rgba.data() + (static_cast<size_t>(cellY + y) * static_cast<size_t>(width) +
                                               static_cast<size_t>(cellX + x)) *
                                                  4u);
            }
        }
        const double scale =
            std::min(static_cast<double>(cellW) / src.width, static_cast<double>(cellH) / src.height);
        const int dw = std::max(1, std::min(cellW, static_cast<int>(src.width * scale + 0.5)));
        const int dh = std::max(1, std::min(cellH, static_cast<int>(src.height * scale + 0.5)));
        const int offsetX = cellX + (cellW - dw) / 2;
        const int offsetY = cellY + (cellH - dh) / 2;
        const double stepX = static_cast<double>(src.width) / dw;
        const double stepY = static_cast<double>(src.height) / dh;
        for (int y = 0; y < dh; ++y) {
            const double y0 = y * stepY;
            const double y1 = std::min(static_cast<double>(src.height), y0 + stepY);
            for (int x = 0; x < dw; ++x) {
                const double x0 = x * stepX;
                const double x1 = std::min(static_cast<double>(src.width), x0 + stepX);
                double sum[4] = {0, 0, 0, 0};
                double weight = 0;
                for (int sy = static_cast<int>(y0); sy < src.height && sy < y1; ++sy) {
                    const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                    for (int sx = static_cast<int>(x0); sx < src.width && sx < x1; ++sx) {
                        const double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                        const double w = wx * wy;
                        const uint8_t* p =
                            src.rgba.data() + (static_cast<size_t>(sy) * static_cast<size_t>(src.width) +
                                               static_cast<size_t>(sx)) *
                                                  4u;
                        for (int c = 0; c < 4; ++c) {
                            sum[c] += w * p[c];
                        }
                        weight += w;
                    }
                }
                uint8_t* dst =
                    out.rgba.data() + (static_cast<size_t>(offsetY + y) * static_cast<size_t>(width) +
                                       static_cast<size_t>(offsetX + x)) *
                                          4u;
                for (int c = 0; c < 4; ++c) {
                    dst[c] = static_cast<uint8_t>(weight > 0 ? std::min(255.0, sum[c] / weight + 0.5) : 0);
                }
            }
        }
    }
    return true;
}

bool writePng(const fs::path& path, const Image& image) {
    if (image.width < 1 || image.height < 1) {
        return false;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    return sw::writePng(genericPathToUtf8(path), image.rgba.data(), image.width, image.height,
                        image.width * 4);
}

bool readPng(const fs::path& path, Image& out) {
    out = Image();
    if (!sw::readPng(genericPathToUtf8(path), out.rgba, out.width, out.height) || out.width < 1 ||
        out.height < 1) {
        out = Image();
        return false;
    }
    return true;
}

}  // namespace xt::tools::symbols
