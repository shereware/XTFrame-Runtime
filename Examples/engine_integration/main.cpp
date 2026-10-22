#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "example_args.h"
#include "json_coexist.h"
// The core host contract in one include: Docs/design/23-host-integration.md §23.6.
#include "xt/xt.h"
// The non-core rows of the §23.6 table this example uses.
#include "xt/bundle/bundle.h"
#include "xt/flow/flow_runtime.h"
#include "xt/flow/host_ports.h"
#include "xt/sw/sw_renderer.h"
// Outside the frozen §23.6 surface, and outside what a host has to integrate: compiling a .xt
// source, writing the off-screen frame as a PNG, the --simplify demo, and a real text shaper when
// this example is built with XT_BUILD_TEXT.
#include "xt/command_simplifier.h"
#include "xt/compile/compile.h"
#include "xt/sw/sw_png.h"
#ifdef XT_EXAMPLE_HAS_TEXT
#include "xt/text/ft_text_shaper.h"
#endif

// --perf-every: the process working set comes from the OS (Docs/tasks/T-145.md).
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h must come first.
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

/// engine_integration: shows how a host *engine* (no SDL, no GL) integrates XTFrame directly.
/// It drives xt::Context itself (compile/load, setViewport, pushInput, update, render), walks the
/// resulting xt::CommandList with its own backend (MyBackend below, one switch arm per
/// xt::CmdKind -- the exact spot a real engine plugs in its own draw calls), and separately
/// rasterizes the same CommandList off-screen with xt_sw so this example has something visual to
/// check. See README.md in this directory for the full walkthrough and
/// Docs/design/09-command-list.md for the CommandList contract a backend must honor.
///
/// Usage: engine_integration --file <doc.xt|doc.xtir> [--headless] [--frames N] [--width W]
///                            [--height H] [--simplify]
///        engine_integration --bundle <dir> --ui <id> [--watch] [--port-demo] [...same options]
///        either form also takes [--perf-every N]
///
/// The second form is the published-bundle path (Docs/design/22-bundle.md §22.5): the manifest at
/// `<dir>/manifest.json` is opened, the UI `<id>` is loaded with xt::bundle::loadUi() and its
/// `.xtflow` graph (when it has one) is run by xt::flow::FlowRuntime. `--watch` polls
/// Bundle::reload() every 30 frames and hot-reloads the UI in place (reloadUi + FlowRuntime::
/// rebind) whenever the editor re-packed it. `--port-demo` drives the UI's connection points
/// straight from C++ with xt::flow::HostPorts (Docs/tasks/T-144.md): every 60 frames it sets
/// `#score.setText`, and it prints every pulse output the UI fires.
///
/// `--perf-every N` (Docs/tasks/T-145.md) prints one `perf frame=...` line every N frames with the
/// interval's average update/render time, the three largest SW render sub-phases, the fraction of
/// the texture uploaded and the process working set -- a baseline for spotting per-frame
/// accumulation outside any engine. It replaces the per-frame command tally lines.
///
/// Exit codes: 0 success (prints "done"), 1 load/compile error, 2 argument error (usage printed
/// to stderr). `--headless` only skips opening a window -- this example never opens one, so it is
/// accepted for command-line consistency with the other examples and otherwise ignored.

namespace fs = std::filesystem;

namespace {

/// A minimal "engine" backend: tallies how many of each xt::CmdKind occurred instead of actually
/// drawing (a real engine would issue its own GPU/CPU draw calls in each switch arm instead). The
/// switch covers every xt::CmdKind (Docs/design/09-command-list.md §9.1) -- the table in §9.5
/// lists which subset an integrator must support at minimum and how to degrade the rest, which is
/// exactly what xt::CommandListSimplifier (--simplify below) does automatically.
struct MyBackend {
    uint32_t counts[12] = {};
    /// DrawImageQuad (19.8's flatten3D output for a non-parallelogram-projected DrawImage): a
    /// minimal host draws it as two triangles (IRenderAuxGeom::DrawBuffer-style), so this tallies
    /// triangles (2 per command) rather than commands.
    uint32_t imageQuadTriangles = 0;
    /// PushProjection/PopProjection/PushTransform3D/PopTransform3D (CmdKind 12-15): a minimal
    /// host does not understand projection at all; these must never reach here after --simplify
    /// (CommandListSimplifier's flatten3D rule removes them, Docs/design/19-3d-space.md §19.8).
    uint32_t projectionCmds = 0;
    /// PushEffect/PopEffect (CmdKind 17-18, Docs/design/18-effects.md §18.5): a minimal host does
    /// not understand effects at all; these must never reach here after --simplify
    /// (CommandListSimplifier's `options.effects` rule removes them by default).
    uint32_t effectCmds = 0;

    void draw(const xt::CommandList& list) {
        for (const xt::Command& cmd : list.commands) {
            switch (cmd.kind) {
                case xt::CmdKind::FillPath:
                    ++counts[0];
                    break;
                case xt::CmdKind::StrokePath:
                    ++counts[1];
                    break;
                case xt::CmdKind::DrawImage:
                    ++counts[2];
                    break;
                case xt::CmdKind::DrawGlyphs:
                    ++counts[3];
                    break;
                case xt::CmdKind::PushClipRect:
                    ++counts[4];
                    break;
                case xt::CmdKind::PushClipPath:
                    ++counts[5];
                    break;
                case xt::CmdKind::PopClip:
                    ++counts[6];
                    break;
                case xt::CmdKind::PushLayer:
                    ++counts[7];
                    break;
                case xt::CmdKind::PopLayer:
                    ++counts[8];
                    break;
                case xt::CmdKind::PushMask:
                    ++counts[9];
                    break;
                case xt::CmdKind::EndMaskContent:
                    ++counts[10];
                    break;
                case xt::CmdKind::PopMask:
                    ++counts[11];
                    break;
                case xt::CmdKind::DrawImageQuad:
                    imageQuadTriangles += 2;
                    break;
                case xt::CmdKind::PushProjection:
                case xt::CmdKind::PopProjection:
                case xt::CmdKind::PushTransform3D:
                case xt::CmdKind::PopTransform3D:
                    ++projectionCmds;
                    break;
                case xt::CmdKind::PushEffect:
                case xt::CmdKind::PopEffect:
                    ++effectCmds;
                    break;
                case xt::CmdKind::DrawParticles:
                case xt::CmdKind::DrawImageMesh:
                    break;
            }
        }
    }

    void print(uint32_t frameIndex) const {
        std::printf(
            "engine_integration: frame %u commands: fill=%u stroke=%u image=%u glyphs=%u "
            "clipRect=%u clipPath=%u popClip=%u pushLayer=%u popLayer=%u pushMask=%u "
            "endMaskContent=%u popMask=%u imageQuadTriangles=%u effectCmds=%u\n",
            frameIndex, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6],
            counts[7], counts[8], counts[9], counts[10], counts[11], imageQuadTriangles, effectCmds);
    }
};

/// The host engine's texture (Docs/design/23-host-integration.md §23.1, §23.7). XTFrame never
/// allocates it and never learns what it really is: the host owns the RGBA8 pixels the SwTarget
/// points at, and uploads only the regions XTFrame reports as dirty. A real engine's
/// uploadRegion() would be a graphics-API sub-image upload (glTexSubImage2D, UpdateSubresource,
/// ...); this stand-in copies into a second buffer so the example can tally how much it moved.
struct HostTexture {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba;
    uint64_t uploadedPixels = 0;
    uint32_t uploads = 0;

    void create(int width, int height) {
        w = width;
        h = height;
        rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    }

    /// Uploads the pixels of `r` (device px, snapped outwards to whole pixels and clipped to the
    /// texture) from `src`, whose row pitch is `stride` bytes and whose (0, 0) is the texture's
    /// (0, 0). A rectangle that falls entirely outside the texture is not counted as an upload.
    void uploadRegion(xt::Rect r, const uint8_t* src, int stride) {
        const int x0 = std::clamp(static_cast<int>(std::floor(r.x)), 0, w);
        const int y0 = std::clamp(static_cast<int>(std::floor(r.y)), 0, h);
        const int x1 = std::clamp(static_cast<int>(std::ceil(r.x + r.w)), 0, w);
        const int y1 = std::clamp(static_cast<int>(std::ceil(r.y + r.h)), 0, h);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }
        for (int y = y0; y < y1; ++y) {
            const uint8_t* srcRow =
                src + static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x0) * 4;
            uint8_t* dstRow =
                rgba.data() + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x0)) * 4;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(x1 - x0) * 4);
        }
        uploadedPixels += static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
        ++uploads;
    }
};

/// Docs/design/21-perf-tracing.md §21.3 "公開 API": a host reads xt::Context::perf() itself and
/// decides how to present it. This example prints one line every 120 frames -- the shape a real
/// engine would feed into its own profiler HUD.
void printPerfSummary(const xt::PerfSummary& s) {
    std::printf(
        "engine_integration: perf over %u frames: fps %.1f | update %.3f ms (style %.3f, layout "
        "%.3f, animate %.3f, transform %.3f) | paint %.3f ms | render %.3f ms | commands %u\n",
        s.frames, static_cast<double>(s.fpsAvg), static_cast<double>(s.avg.updateMs),
        static_cast<double>(s.avg.styleMs), static_cast<double>(s.avg.layoutMs),
        static_cast<double>(s.avg.animateMs), static_cast<double>(s.avg.transformMs),
        static_cast<double>(s.avg.paintMs), static_cast<double>(s.avg.renderMs), s.avg.commands);
}

/// The process working set in MB (Docs/tasks/T-145.md): GetProcessMemoryInfo on Windows,
/// /proc/self/statm on Linux, 0 elsewhere or on failure.
double workingSetMb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
        return 0.0;
    }
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    unsigned long long sizePages = 0;
    unsigned long long residentPages = 0;
    if (!(statm >> sizePages >> residentPages)) {
        return 0.0;
    }
    const long pageSize = sysconf(_SC_PAGESIZE);
    return static_cast<double>(residentPages) * static_cast<double>(pageSize > 0 ? pageSize : 4096) /
           (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}

/// --perf-every's per-interval accumulator (Docs/tasks/T-145.md). add() is called once per frame
/// after the backend render, when ctx.perf().current() holds that frame's complete record; the
/// sums are kept here rather than read back through PerfTracker::summary() because an interval may
/// be longer than the tracker's 240-frame history.
struct PerfInterval {
    /// The SW render sub-phases that do not overlap each other (Docs/design/21-perf-tracing.md
    /// §21.1; the span/fx sub-splits are left out so no time is counted twice).
    struct SwPhase {
        const char* name;
        float xt::FrameStats::* field;
    };
    static constexpr SwPhase kSwPhases[] = {
        {"sw:path", &xt::FrameStats::swPathMs},      {"sw:raster", &xt::FrameStats::swRasterMs},
        {"sw:span", &xt::FrameStats::swSpanMs},      {"sw:image", &xt::FrameStats::swSpanImageMs},
        {"sw:clip", &xt::FrameStats::swClipMs},      {"sw:layer", &xt::FrameStats::swLayerMs},
        {"sw:fx.blur", &xt::FrameStats::swFxBlurMs}, {"sw:fx.dilate", &xt::FrameStats::swFxDilateMs},
        {"sw:fx.sdf", &xt::FrameStats::swFxSdfMs},   {"sw:fx.pix", &xt::FrameStats::swFxPixMs},
    };
    static constexpr size_t kSwPhaseCount = sizeof(kSwPhases) / sizeof(kSwPhases[0]);

    uint32_t frames = 0;
    double updateMs = 0;
    double renderMs = 0;
    double sw[kSwPhaseCount] = {};

    void add(const xt::FrameStats& f) {
        ++frames;
        updateMs += f.updateMs;
        renderMs += f.renderMs;
        for (size_t i = 0; i < kSwPhaseCount; ++i) {
            sw[i] += f.*(kSwPhases[i].field);
        }
    }

    /// Prints `perf frame=<n> update=<ms> render=<ms> top=[...] damage=<%> ws=<MB>` and resets.
    /// `uploadedFraction` is the interval's uploaded pixels over (texture pixels x frames).
    void printAndReset(uint32_t frame, double uploadedFraction) {
        const double n = frames == 0 ? 1.0 : static_cast<double>(frames);
        size_t order[kSwPhaseCount];
        for (size_t i = 0; i < kSwPhaseCount; ++i) {
            order[i] = i;
        }
        std::stable_sort(order, order + kSwPhaseCount, [this](size_t a, size_t b) { return sw[a] > sw[b]; });
        std::printf(
            "perf frame=%u update=%.3f render=%.3f top=[%s %.3f, %s %.3f, %s %.3f] damage=%.1f ws=%.1f\n",
            frame, updateMs / n, renderMs / n, kSwPhases[order[0]].name, sw[order[0]] / n,
            kSwPhases[order[1]].name, sw[order[1]] / n, kSwPhases[order[2]].name, sw[order[2]] / n,
            uploadedFraction * 100.0, workingSetMb());
        std::fflush(stdout);
        *this = PerfInterval{};
    }
};

/// Loads `<bundle dir>/<flowFile>` into `flow` and binds it to `doc`. Returns false (with the
/// reason on stderr) when the graph cannot be read, parsed or bound; the UI still runs without it.
bool loadFlow(xt::flow::FlowRuntime& flow, xt::bundle::Bundle& bundle, const std::string& flowFile,
              xt::Document& doc) {
    const std::string path = std::string(bundle.dir()) + "/" + flowFile;
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) {
        std::fprintf(stderr, "engine_integration: cannot read flow '%s'\n", path.c_str());
        return false;
    }
    std::ostringstream text;
    text << file.rdbuf();

    xt::flow::FlowGraph graph;
    std::vector<std::string> errors;
    if (xt::flow::FlowGraph::parse(text.str(), graph, errors) != xt::Status::kOk ||
        flow.load(graph, doc, errors) != xt::Status::kOk) {
        for (const std::string& error : errors) {
            std::fprintf(stderr, "engine_integration: flow '%s': %s\n", path.c_str(), error.c_str());
        }
        return false;
    }
    return true;
}

void printUsage() {
    std::fprintf(stderr,
                 "usage: engine_integration --file <doc.xt|doc.xtir> [--headless] [--frames N] "
                 "[--width W] [--height H] [--simplify] [--perf-every N]\n"
                 "       engine_integration --bundle <dir> --ui <id> [--watch] [--port-demo] [...]\n"
                 "       engine_integration --json-coexist\n");
}

void printDiagnostic(const xt::compile::Diagnostic& d) {
    const char* level = d.level == xt::compile::Diagnostic::Level::Error ? "error" : "warning";
    std::fprintf(stderr, "%s: %s\n", level, d.message.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    // T-396 (23 §23.ae): XTFrame's nlohmann next to the engine's own copy, then exit.
    if (argc == 2 && std::strcmp(argv[1], "--json-coexist") == 0) {
        if (!runJsonCoexistDemo()) return 1;
        std::printf("done\n");
        return 0;
    }
    // --bundle/--ui take a value parseCommonArgs() does not know about (it would take the first
    // one as the input file), so they are pulled out of argv first and the rest is forwarded.
    std::string bundleDir;
    std::string uiId;
    bool watch = false;
    bool portDemo = false;
    uint32_t perfEvery = 0;
    std::vector<char*> forwarded;
    forwarded.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--bundle" || arg == "--ui") {
            if (i + 1 >= argc) {
                printUsage();
                return 2;
            }
            (arg == "--bundle" ? bundleDir : uiId) = argv[++i];
        } else if (arg == "--watch") {
            watch = true;
        } else if (arg == "--port-demo") {
            portDemo = true;
        } else if (arg == "--perf-every") {
            char* end = nullptr;
            const unsigned long value = i + 1 < argc ? std::strtoul(argv[i + 1], &end, 10) : 0;
            if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || value == 0 || value > 1000000) {
                printUsage();
                return 2;
            }
            perfEvery = static_cast<uint32_t>(value);
            ++i;
        } else {
            forwarded.push_back(argv[i]);
        }
    }

    xt::examples::CommonArgs args;
    args.width = 640;
    args.height = 480;
    std::vector<std::string> remaining;
    if (!xt::examples::parseCommonArgs(static_cast<int>(forwarded.size()), forwarded.data(), args,
                                       remaining)) {
        printUsage();
        return 2;
    }
    bool simplify = false;
    for (const std::string& arg : remaining) {
        if (arg == "--simplify") {
            simplify = true;
        } else {
            std::fprintf(stderr, "engine_integration: unknown option '%s'\n", arg.c_str());
            printUsage();
            return 2;
        }
    }
    // --bundle <dir> --ui <id> replaces --file; asking for both at once is an argument error.
    const bool bundleMode = !bundleDir.empty();
    if (args.frames == 0 || (bundleMode ? (uiId.empty() || !args.file.empty()) : args.file.empty())) {
        printUsage();
        return 2;
    }

    fs::path inputPath(args.file);
    std::string rootDir = inputPath.has_parent_path() ? inputPath.parent_path().string() : ".";

    // NOTE: a real host normally supplies its own xt::TextShaper wrapping the engine's font/text
    // system (Docs/design/07-text.md §7.2), or registers its own font assets with
    // addFontMemory(). This example stands in for that with the bundled FreeType shaper over the
    // platform's system fonts (Docs/design/23-host-integration.md §23.4); without xt_text it
    // falls back to NullTextShaper, and Text nodes then produce no DrawGlyphs commands.
#ifdef XT_EXAMPLE_HAS_TEXT
    xt::text::FtTextShaper shaper;
    shaper.addSystemFonts();
#else
    xt::NullTextShaper shaper;
#endif
    xt::FileAssetResolver assets(rootDir, xt::sw::decodeImageBytes);
    // In bundle mode the Context must resolve assets through the bundle itself: a UI references
    // its images by their packed "assets/<hash>.<ext>" name (Docs/design/22-bundle.md §22.5).
    std::unique_ptr<xt::bundle::Bundle> bundle;
    xt::AssetResolver* resolver = &assets;
    if (bundleMode) {
        std::string error;
        xt::Result<std::unique_ptr<xt::bundle::Bundle>> opened =
            xt::bundle::Bundle::open(bundleDir, xt::sw::decodeImageBytes, &error);
        if (!opened.ok()) {
            std::fprintf(stderr, "engine_integration: cannot open bundle '%s': %s\n", bundleDir.c_str(),
                         error.c_str());
            return 1;
        }
        bundle = std::move(opened.value());
        resolver = &bundle->assets();
    }
    xt::Context ctx(xt::ContextConfig{&shaper, resolver});
    // Declared after `ctx` so its listeners come off before the Documents are destroyed.
    xt::flow::FlowRuntime flow;
    bool flowLoaded = false;
    // --port-demo: the host's own view of the UI's connection points, no .xtflow involved.
    xt::flow::HostPorts hostPorts;

    xt::Result<xt::DocumentHandle> loadResult{xt::Result<xt::DocumentHandle>::error(xt::Status::kInternal)};
    xt::compile::CompileResult compileResult;
    if (bundleMode) {
        // The published form: no compiler involved, the `.xtir` comes straight out of the bundle.
        loadResult = xt::bundle::loadUi(ctx, *bundle, uiId);
        if (!loadResult.ok()) {
            std::fprintf(stderr, "engine_integration: failed to load ui '%s' from '%s' (%s)\n", uiId.c_str(),
                         bundleDir.c_str(), xt::statusName(loadResult.status()));
            return 1;
        }
    } else if (inputPath.extension() == ".xt") {
        // `.xt` source: compile it at runtime (Docs/design/12-compiler.md §12.2-§12.3) instead of
        // requiring the host to run xtframe-compile first.
        xt::compile::CompileOptions options;
        options.rootDir = rootDir;
        compileResult = xt::compile::compileDocument(args.file, options);
        for (const xt::compile::Diagnostic& d : compileResult.diagnostics) {
            printDiagnostic(d);
        }
        if (!compileResult.ok()) {
            std::fprintf(stderr, "engine_integration: failed to compile '%s'\n", args.file.c_str());
            return 1;
        }
        loadResult = ctx.loadDocument(xt::IrBlob{compileResult.ir.data(), compileResult.ir.size()}, true);
    } else {
        // `.xtir`: already-baked IR, e.g. from xtframe-compile (Docs/tasks/T-16.md).
        loadResult = ctx.loadDocumentFile(args.file);
    }
    if (!loadResult.ok()) {
        std::fprintf(stderr, "engine_integration: failed to load '%s' (%s)\n", args.file.c_str(),
                     xt::statusName(loadResult.status()));
        return 1;
    }
    const xt::DocumentHandle handle = loadResult.value();

    // A bundled UI may ship a flow graph (Docs/design/17-editor.md §17.6); from here on it drives
    // the document's connection points.
    if (bundleMode) {
        const xt::bundle::UiInfo* info = bundle->ui(uiId);
        if (info != nullptr && !info->flow.empty()) {
            flowLoaded = loadFlow(flow, *bundle, info->flow, *ctx.document(handle));
        }
        if (portDemo) {
            hostPorts.bind(*ctx.document(handle));
            for (const xt::flow::PortDesc& port : hostPorts.ports()) {
                if (!port.isInput && port.type == xt::flow::PortType::Pulse) {
                    const std::string name = port.owner + "." + port.port;
                    hostPorts.addOutputListener(name, [name](const xt::flow::PortValue&) {
                        std::printf("port-demo: %s\n", name.c_str());
                    });
                }
            }
        }
    }

    ctx.setViewport(xt::Rect{0, 0, static_cast<float>(args.width), static_cast<float>(args.height)}, 1.0f);

    // Demonstrate host -> Context input: a pointer move into the viewport followed by a
    // down/up click. Context::update() drains and dispatches queued input before its own
    // Document::update(), so this works the same whether or not a real window/display exists
    // (Docs/design/08-events.md §8.1, Docs/tasks/T-32.md).
    xt::InputEvent moveEvent;
    moveEvent.kind = xt::InputEvent::PointerMove;
    moveEvent.pos = xt::Vec2{static_cast<float>(args.width) * 0.5f, static_cast<float>(args.height) * 0.5f};
    ctx.pushInput(moveEvent);
    xt::InputEvent downEvent = moveEvent;
    downEvent.kind = xt::InputEvent::PointerDown;
    downEvent.button = xt::PointerButton::Left;
    ctx.pushInput(downEvent);
    xt::InputEvent upEvent = downEvent;
    upEvent.kind = xt::InputEvent::PointerUp;
    ctx.pushInput(upEvent);

    // The Document's IrView is needed by SwRenderer::render() to resolve DrawImage's AssetIds
    // (Docs/design/10-backends.md §10.4); Context does not expose it directly, so fetch it via the
    // Document the handle refers to.
    // Both are refreshed after every hot reload: reloadUi() destroys the previous Document along
    // with its IrView (Docs/design/22-bundle.md §22.5).
    const xt::Document* doc = ctx.document(handle);
    const xt::IrView* irView = doc != nullptr ? &doc->ir() : nullptr;

    // The host owns the pixel buffer and the texture; XTFrame only ever writes into the buffer
    // (Docs/design/23-host-integration.md §23.1). setTarget() is called once, not per frame: a
    // partial redraw must find last frame's pixels still there.
    xt::sw::SwRenderer renderer;
    std::vector<uint8_t> pixels(static_cast<size_t>(args.width) * static_cast<size_t>(args.height) * 4);
    const int strideBytes = args.width * 4;
    xt::sw::SwTarget target{pixels.data(), args.width, args.height, strideBytes};
    renderer.setTarget(target);
    HostTexture texture;
    texture.create(args.width, args.height);
    const xt::Color clearColor{1, 1, 1, 1};
    const xt::Rect fullRect{0, 0, static_cast<float>(args.width), static_cast<float>(args.height)};
    const uint64_t framePixels = static_cast<uint64_t>(args.width) * static_cast<uint64_t>(args.height);

    xt::CommandListSimplifier simplifier;
    xt::SimplifyOptions simplifyOptions;
    xt::CommandList list;
    // --simplify: the simplified commands go into a list of their own (T-263e), see below.
    xt::CommandList simplified;
    xt::DamageList damage;
    uint64_t uploadedAtMark = 0;
    uint32_t uploadsAtMark = 0;
    const float dt = 1.0f / 60.0f;
    const std::string outputPath = "engine_integration_out.png";
    // Docs/design/21-perf-tracing.md: tracing is off by default; a host opts in like this.
    ctx.perf().setEnabled(true);
    const uint32_t perfInterval = 120;
    PerfInterval perfEveryInterval;
    uint64_t uploadedAtPerfMark = 0;
    for (uint32_t frame = 0; frame < args.frames; ++frame) {
        // --watch: poll the manifest (the editor re-packs on save) and hot-reload this UI in place.
        if (watch && bundleMode && frame != 0 && frame % 30 == 0) {
            for (const std::string& changed : bundle->reload()) {
                if (changed != uiId) {
                    continue;  // Another UI of the same bundle; this example only shows one.
                }
                if (!xt::bundle::reloadUi(ctx, *bundle, handle, uiId).ok()) {
                    std::fprintf(stderr, "engine_integration: failed to reload '%s'\n", uiId.c_str());
                    continue;
                }
                doc = ctx.document(handle);
                irView = doc != nullptr ? &doc->ir() : nullptr;
                uint32_t warningCount = 0;
                if (flowLoaded && doc != nullptr) {
                    // Every ElementRef and listener the graph held died with the old Document.
                    std::vector<std::string> warnings;
                    flow.rebind(*ctx.document(handle), warnings);
                    for (const std::string& warning : warnings) {
                        std::fprintf(stderr, "engine_integration: %s\n", warning.c_str());
                    }
                    warningCount = static_cast<uint32_t>(warnings.size());
                }
                if (portDemo && doc != nullptr) {
                    std::vector<std::string> warnings;
                    hostPorts.rebind(*ctx.document(handle), warnings);
                }
                std::printf("reloaded %s (%u warnings)\n", uiId.c_str(), warningCount);
            }
        }

        ctx.update(frame == 0 ? 0.0f : dt);
        if (flowLoaded) {
            flow.tick(frame == 0 ? 0.0f : dt);
        }
        if (portDemo) {
            hostPorts.tick(frame == 0 ? 0.0f : dt);
            if (frame % 60 == 0) {
                xt::flow::PortValue score;
                score.type = xt::flow::PortType::String;
                score.s = std::to_string(frame / 60 * 10);
                if (hostPorts.setInput("#score.setText", score) != xt::Status::kOk) {
                    std::fprintf(stderr, "port-demo: %s\n", hostPorts.lastError().c_str());
                }
            }
        }
        // §23.2: the same call that produces the frame's commands reports what changed on screen.
        // `list` deliberately still holds the previous frame's commands -- that is the diff's
        // base, and render() clears it itself (step 1).
        ctx.render(list, &damage);
        // T-263e: simplified out of place. `list` stays exactly as render() wrote it, which is
        // what makes it the next frame's damage base; a list simplified in place is not one, and
        // every frame after it would report full damage. The host draws `frameList`.
        const xt::CommandList* frameList = &list;
        if (simplify) {
            simplifier.simplify(list, simplified, simplifyOptions, &shaper);
            frameList = &simplified;
        }

        // The host engine's own draw path (this is the only part a real integrator writes).
        MyBackend backend;
        backend.draw(*frameList);
        if (perfEvery == 0) {
            backend.print(frame);
        }

        // Docs/design/19-3d-space.md §19.8: --simplify's flatten3D rule removes every
        // PushProjection/PopProjection/PushTransform3D/PopTransform3D command, so a minimal host
        // (this example's MyBackend) must never see one after simplification.
        if (simplify && backend.projectionCmds != 0) {
            std::fprintf(stderr,
                         "engine_integration: frame %u: %u unflattened 3D command(s) remain after "
                         "--simplify\n",
                         frame, backend.projectionCmds);
            return 1;
        }

        // Rasterize into the host's buffer and upload only what changed (§23.3). A real engine
        // backend draws from `list` itself and would not normally also link xt_sw; this example
        // does both so it has something visual to check. In bundle mode the images resolve through
        // the bundle (`resolver`), exactly as the Context does.
        if (damage.full) {
            renderer.clear(clearColor);
            renderer.render(*frameList, &shaper, resolver, irView, &ctx.perf());
            texture.uploadRegion(fullRect, pixels.data(), strideBytes);
        } else {
            renderer.clearRects(clearColor, damage.rects);
            renderer.render(*frameList, &shaper, resolver, irView, &ctx.perf(), &damage);
            for (const xt::Rect& r : damage.rects) {
                texture.uploadRegion(r, pixels.data(), strideBytes);
            }
        }

        if (perfEvery != 0) {
            perfEveryInterval.add(ctx.perf().current());
            if ((frame + 1) % perfEvery == 0 || frame + 1 == args.frames) {
                const double fraction = static_cast<double>(texture.uploadedPixels - uploadedAtPerfMark) /
                                        static_cast<double>(framePixels * perfEveryInterval.frames);
                perfEveryInterval.printAndReset(frame + 1, fraction);
                uploadedAtPerfMark = texture.uploadedPixels;
            }
        } else if ((frame + 1) % perfInterval == 0) {
            printPerfSummary(ctx.perf().summary(perfInterval));
            // §23.7: how much of the texture the host actually had to move, averaged over the
            // interval. A HUD with only a clock and one animated bar stays far below 100%.
            const double avgFraction = static_cast<double>(texture.uploadedPixels - uploadedAtMark) /
                                       static_cast<double>(framePixels * perfInterval);
            const double regionsPerFrame =
                static_cast<double>(texture.uploads - uploadsAtMark) / static_cast<double>(perfInterval);
            std::printf("upload: avg %.1f%% of frame, %.1f regions/frame\n", avgFraction * 100.0,
                        regionsPerFrame);
            uploadedAtMark = texture.uploadedPixels;
            uploadsAtMark = texture.uploads;
        }

        if (frame + 1 == args.frames) {
            // Written from the *texture*, not from the render buffer: a region the host failed to
            // upload would show up here as a stale patch.
            if (!xt::sw::writePng(outputPath, texture.rgba.data(), args.width, args.height, args.width * 4)) {
                std::fprintf(stderr, "engine_integration: failed to write '%s'\n", outputPath.c_str());
                return 1;
            }
            std::printf("engine_integration: wrote %s (%dx%d)\n", outputPath.c_str(), args.width,
                        args.height);
        }
    }

    std::printf("done\n");
    return 0;
}
