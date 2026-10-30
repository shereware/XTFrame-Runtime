#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "xt/bundle/ui_project.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/perf.h"
#include "xt/text_shaper.h"

/// xt_perf_flow (T-475; Docs/design/21-perf-tracing.md §21.6, 27-interaction.md §27.4.2 item 6):
/// the headless flow benchmark runner. It loads one Tests/perf/flow_big fixture (its single UI's
/// `.xt` and `.xtflow`) into a Context, then runs the frame loop of `xtframe-view --headless --perf
/// --synth-pointer 16 --synth-ports 20` (input, Context::update(), FlowRuntime::tick(), paint) and
/// prints the flow stages, counters and the allocations per tick (FrameStats::flowAllocs, which
/// only this executable fills: flow_alloc_hook.cpp replaces operator new). No rendering backend
/// and no fonts (NullTextShaper): the flow numbers do not depend on either.
///
///   xt_perf_flow --fixture <dir> [--frames 2000] [--keep N] [--behavior 0|1] [--synth-pointer 16]
///                [--synth-ports 20] [--csv <file>] [--json] [--plain]
///
/// --keep: frames measured (the last N; default frames - frames/5). --behavior: the compile
/// behavior version (1 = coalesced value signals, 43 §43.3); without it, the fixture's own
/// `.xtui` `spec.behavior`, which `xtframe-view --project` compiles it at too (T-476: the flow_big
/// fixtures say 1). --plain: tracing off (no stage timers, no counters), only the flow
/// tick timed from outside -- the cost of the instrumentation is the difference.

namespace {

struct Args {
    std::string fixture;
    uint32_t frames = 2000;
    uint32_t keep = 0;
    uint32_t behavior = 0;
    bool behaviorGiven = false;  // T-476: else the fixture's `.xtui` decides.
    uint32_t synthPointer = 16;
    uint32_t synthPorts = 20;
    std::string csv;
    bool json = false;
    bool plain = false;
};

bool parseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](uint32_t& field) {
            if (i + 1 >= argc) {
                return false;
            }
            field = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            return true;
        };
        bool ok = true;
        if (arg == "--fixture" && i + 1 < argc) {
            out.fixture = argv[++i];
        } else if (arg == "--csv" && i + 1 < argc) {
            out.csv = argv[++i];
        } else if (arg == "--frames") {
            ok = value(out.frames);
        } else if (arg == "--keep") {
            ok = value(out.keep);
        } else if (arg == "--behavior") {
            ok = value(out.behavior);
            out.behaviorGiven = ok;
        } else if (arg == "--synth-pointer") {
            ok = value(out.synthPointer);
        } else if (arg == "--synth-ports") {
            ok = value(out.synthPorts);
        } else if (arg == "--json") {
            out.json = true;
        } else if (arg == "--plain") {
            out.plain = true;
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "xt_perf_flow: bad argument '%s'\n", arg.c_str());
            return false;
        }
    }
    if (out.fixture.empty() || out.frames == 0) {
        return false;
    }
    if (out.keep == 0 || out.keep > out.frames) {
        out.keep = out.frames - out.frames / 5;
    }
    return true;
}

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    out = text.str();
    return true;
}

/// The fixture's single UI: `<dir>/ui/<name>/<name>.xt` and the `.xtflow` next to it.
bool findUi(const std::filesystem::path& dir, std::filesystem::path& doc, std::filesystem::path& flow) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir / "ui", ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        doc = entry.path() / (name + ".xt");
        flow = entry.path() / (name + ".xtflow");
        if (std::filesystem::exists(doc) && std::filesystem::exists(flow)) {
            return true;
        }
    }
    return false;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double k = static_cast<double>(values.size() - 1) * p;
    const auto lo = static_cast<size_t>(k);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (values[hi] - values[lo]) * (k - static_cast<double>(lo));
}

void pushPointer(xt::Context& ctx, xt::Vec2 pos) {
    xt::InputEvent event;
    event.kind = xt::InputEvent::PointerMove;
    event.pos = pos;
    event.button = xt::PointerButton::Left;
    ctx.pushInput(event);
}

/// One measured column: its name and how to read it from a frame.
struct Column {
    const char* name;
    double (*read)(const xt::FrameStats&);
};

double bindingMs(const xt::FrameStats& f) {
    const float phases = f.inputMs + f.styleMs + f.layoutMs + f.animateMs + f.transformMs;
    return std::max(0.0, static_cast<double>(f.updateMs - phases));
}

double otherMs(const xt::FrameStats& f) {
    const float stages = f.flowDeliverMs + f.flowEvalMs + f.flowSinkMs + f.flowPollMs;
    return std::max(0.0, static_cast<double>(f.flowMs - stages));
}

// clang-format off
const Column kColumns[] = {
    {"flowMs", [](const xt::FrameStats& f) { return static_cast<double>(f.flowMs); }},
    {"flowDeliverMs", [](const xt::FrameStats& f) { return static_cast<double>(f.flowDeliverMs); }},
    {"flowEvalMs", [](const xt::FrameStats& f) { return static_cast<double>(f.flowEvalMs); }},
    {"flowSinkMs", [](const xt::FrameStats& f) { return static_cast<double>(f.flowSinkMs); }},
    {"flowPollMs", [](const xt::FrameStats& f) { return static_cast<double>(f.flowPollMs); }},
    {"flowOtherMs", otherMs},
    {"bindingMs", bindingMs},
    {"inputMs", [](const xt::FrameStats& f) { return static_cast<double>(f.inputMs); }},
    {"flowSignals", [](const xt::FrameStats& f) { return static_cast<double>(f.flowSignals); }},
    {"flowSignalsCoalesced", [](const xt::FrameStats& f) { return static_cast<double>(f.flowSignalsCoalesced); }},
    {"flowNodeEvals", [](const xt::FrameStats& f) { return static_cast<double>(f.flowNodeEvals); }},
    {"flowSinkWrites", [](const xt::FrameStats& f) { return static_cast<double>(f.flowSinkWrites); }},
    {"flowFanOutEdges", [](const xt::FrameStats& f) { return static_cast<double>(f.flowFanOutEdges); }},
    {"flowSinkTyped", [](const xt::FrameStats& f) { return static_cast<double>(f.flowSinkTyped); }},
    {"flowSinkText", [](const xt::FrameStats& f) { return static_cast<double>(f.flowSinkText); }},
    {"flowAllocs", [](const xt::FrameStats& f) { return static_cast<double>(f.flowAllocs); }},
    {"setStyleCalls", [](const xt::FrameStats& f) { return static_cast<double>(f.setStyleCalls); }},
};
// clang-format on

void writeCsv(const std::string& path, const std::vector<xt::FrameStats>& frames) {
    std::ofstream out(std::filesystem::path(path), std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "xt_perf_flow: cannot write '%s'\n", path.c_str());
        return;
    }
    out << "frameIndex";
#define XT_FLOW_CSV_HEAD(name) out << "," << #name;
    XT_FRAME_STATS_FLOAT_FIELDS(XT_FLOW_CSV_HEAD)
    XT_FRAME_STATS_UINT_FIELDS(XT_FLOW_CSV_HEAD)
#undef XT_FLOW_CSV_HEAD
    out << "\n";
    char buffer[32];
    for (const xt::FrameStats& f : frames) {
        out << f.frameIndex;
#define XT_FLOW_CSV_FLOAT(name)                                                 \
    std::snprintf(buffer, sizeof(buffer), "%.5f", static_cast<double>(f.name)); \
    out << "," << buffer;
        XT_FRAME_STATS_FLOAT_FIELDS(XT_FLOW_CSV_FLOAT)
#undef XT_FLOW_CSV_FLOAT
#define XT_FLOW_CSV_UINT(name) out << "," << f.name;
        XT_FRAME_STATS_UINT_FIELDS(XT_FLOW_CSV_UINT)
#undef XT_FLOW_CSV_UINT
        out << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parseArgs(argc, argv, args)) {
        std::fprintf(stderr,
                     "usage: xt_perf_flow --fixture <dir> [--frames N] [--keep N] [--behavior N] "
                     "[--synth-pointer N] [--synth-ports N] [--csv <file>] [--json] [--plain]\n");
        return 2;
    }
    std::filesystem::path docPath;
    std::filesystem::path flowPath;
    if (!findUi(std::filesystem::path(args.fixture), docPath, flowPath)) {
        std::fprintf(stderr, "xt_perf_flow: no ui/<name>/<name>.xt + .xtflow under '%s'\n",
                     args.fixture.c_str());
        return 1;
    }
    xt::compile::CompileOptions options;
    options.rootDir = args.fixture;
    options.behavior = args.behavior;
    if (!args.behaviorGiven) {  // T-476: the project's `spec.behavior`, as xtframe-view --project.
        xt::Result<xt::bundle::UiProject> project = xt::bundle::UiProject::load(args.fixture);
        options.behavior = project.ok() ? project.value().spec.behavior : 0;
    }
    xt::compile::CompileResult compiled = xt::compile::compileDocument(docPath.string(), options);
    if (!compiled.ok()) {
        for (const xt::compile::Diagnostic& d : compiled.diagnostics) {
            std::fprintf(stderr, "xt_perf_flow: %s\n", d.message.c_str());
        }
        return 1;
    }
    xt::NullTextShaper shaper;
    xt::ContextConfig config;
    config.shaper = &shaper;
    xt::Context ctx(config);
    ctx.setViewport(xt::Rect{0, 0, 1280, 720}, 1.0f);
    xt::Result<xt::DocumentHandle> loaded =
        ctx.loadDocument(xt::IrBlob{compiled.ir.data(), compiled.ir.size()}, true);
    if (!loaded.ok()) {
        std::fprintf(stderr, "xt_perf_flow: the document did not load\n");
        return 1;
    }
    const xt::DocumentHandle handle = loaded.value();
    xt::Document* doc = ctx.document(handle);

    std::string json;
    std::vector<std::string> errors;
    xt::flow::FlowGraph graph;
    xt::flow::FlowRuntime flow;
    if (!readFile(flowPath, json) || xt::flow::FlowGraph::parse(json, graph, errors) != xt::Status::kOk ||
        flow.load(graph, *doc, errors) != xt::Status::kOk) {
        for (const std::string& e : errors) {
            std::fprintf(stderr, "xt_perf_flow: flow: %s\n", e.c_str());
        }
        return 1;
    }
    flow.setPerfTracker(&ctx.perf());
    ctx.perf().setEnabled(!args.plain);

    // The ports --synth-ports writes (xtframe-view's rule: the first N document-level numbers).
    std::vector<xt::PortId> ports;
    for (const xt::PortRecord& port : doc->ports()) {
        if (ports.size() < args.synthPorts && port.documentLevel && port.type == xt::PortValueType::Number) {
            ports.push_back(port.id);
        }
    }

    if (ports.size() < args.synthPorts) {
        std::fprintf(stderr, "xt_perf_flow: note: only %zu document-level number ports to write\n",
                     ports.size());
    }
    const float dt = 1.0f / 60.0f;
    std::vector<xt::FrameStats> frames;
    std::vector<double> plainTicks;
    frames.reserve(args.frames);
    xt::CommandList list;
    for (uint32_t frame = 0; frame < args.frames; ++frame) {
        const float frameDt = frame == 0 ? 0.0f : dt;
        // T-389's synthetic fast mouse along the diagonal, bouncing (xtframe-view's formula).
        const xt::Rect box = doc->root().boundingBox();
        const uint32_t steps = 600;
        for (uint32_t i = 0; i < args.synthPointer; ++i) {
            const uint32_t k = (frame * args.synthPointer + i) % (2 * steps);
            const float t = static_cast<float>(k < steps ? k : 2 * steps - k) / static_cast<float>(steps);
            pushPointer(ctx, xt::Vec2{box.x + box.w * t, box.y + box.h * t});
        }
        for (size_t p = 0; p < ports.size(); ++p) {
            const uint32_t k = (frame * 3u + static_cast<uint32_t>(p) * 7u) % 200u;
            (void)doc->setPort(ports[p], xt::Value::ofFloat(static_cast<float>(k < 100u ? k : 200u - k)));
        }
        // The previous frame is taken only now: the port writes above run before update()
        // opens this frame, so what they report (port signals) still lands in the previous
        // record, which keeps the per-frame counts whole.
        if (!args.plain && frame > 0) {
            frames.push_back(ctx.perf().current());
        }
        ctx.update(frameDt);
        if (args.plain) {
            const auto start = std::chrono::steady_clock::now();
            flow.tick(frameDt);
            const std::chrono::duration<double, std::milli> elapsed =
                std::chrono::steady_clock::now() - start;
            plainTicks.push_back(elapsed.count());
        } else {
            xt::PerfScope flowScope(ctx.perf(), &xt::FrameStats::flowMs);
            flow.tick(frameDt);
        }
        ctx.render(list, nullptr);
    }
    if (!args.plain) {
        frames.push_back(ctx.perf().current());
    }

    const size_t first = args.frames - args.keep;
    if (args.plain) {
        std::vector<double> kept(plainTicks.begin() + static_cast<std::ptrdiff_t>(first), plainTicks.end());
        double sum = 0;
        for (double v : kept) {
            sum += v;
        }
        const double avg = kept.empty() ? 0.0 : sum / static_cast<double>(kept.size());
        if (args.json) {
            std::printf("{\"frames\": %zu, \"plainFlowMs\": {\"avg\": %.5f, \"p95\": %.5f}}\n", kept.size(),
                        avg, percentile(kept, 0.95));
        } else {
            std::printf("plain flow tick (tracing off), %zu frames: avg %.4f ms, p95 %.4f ms\n", kept.size(),
                        avg, percentile(kept, 0.95));
        }
        return 0;
    }
    if (!args.csv.empty()) {
        writeCsv(args.csv, frames);
    }
    const std::vector<xt::FrameStats> window(frames.begin() + static_cast<std::ptrdiff_t>(first),
                                             frames.end());
    if (args.json) {
        std::printf("{\"frames\": %zu", window.size());
    } else {
        std::printf("%-22s %12s %12s   (%zu frames)\n", "column", "avg", "p95", window.size());
    }
    for (const Column& column : kColumns) {
        std::vector<double> values;
        values.reserve(window.size());
        double sum = 0;
        for (const xt::FrameStats& f : window) {
            values.push_back(column.read(f));
            sum += values.back();
        }
        const double avg = values.empty() ? 0.0 : sum / static_cast<double>(values.size());
        const double p95 = percentile(values, 0.95);
        if (args.json) {
            std::printf(", \"%s\": {\"avg\": %.5f, \"p95\": %.5f}", column.name, avg, p95);
        } else {
            std::printf("%-22s %12.4f %12.4f\n", column.name, avg, p95);
        }
    }
    if (args.json) {
        std::printf("}\n");
    }
    return 0;
}
