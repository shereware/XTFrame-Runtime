#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/flow/flow_graph.h"
#include "xt/flow/flow_runtime.h"
#include "xt/input.h"
#include "xt/perf.h"
#include "xt/style_template.h"
#include "xt/text_shaper.h"

/// T-476 / T-478 (Docs/design/27-interaction.md §27.4.2 item 5): in steady state a
/// FlowRuntime::tick() allocates nothing (FrameStats::flowAllocs == 0). Since T-478 that includes
/// the sink writes into the Document -- the typed `set(port)` writes, the port listeners they
/// notify and the template setStyle writes; only a `setStyle` that goes in as text (the CSS parse)
/// is left out, and the fixtures have none. This executable, unlike xt_flow_tests, links
/// Tests/perf/flow_alloc_hook.cpp (XT_PERF_ALLOC_COUNT), so the count is real.

using namespace xt;
using namespace xt::flow;

namespace {

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// One Tests/perf/flow_big fixture at behavior 1, driven like xtframe-view --synth-pointer 16
/// --synth-ports 20.
struct BigFlow {
    NullTextShaper shaper;
    std::unique_ptr<Document> doc;
    FlowRuntime flow;
    std::vector<PortId> ports;

    void load(const std::string& dir, const std::string& name) {
        compile::CompileOptions options;
        options.rootDir = dir;
        options.behavior = 1;
        compile::CompileResult compiled =
            compile::compileDocument(dir + "/ui/" + name + "/" + name + ".xt", options);
        REQUIRE(compiled.ok());
        REQUIRE(Document::load(IrBlob{compiled.ir.data(), compiled.ir.size()}, true, &shaper, nullptr, doc) ==
                Status::kOk);
        doc->setViewport(Rect{0, 0, 1280, 720});
        doc->update(0.0f);
        FlowGraph graph;
        std::vector<std::string> errors;
        REQUIRE(FlowGraph::parse(readText(dir + "/ui/" + name + "/" + name + ".xtflow"), graph, errors) ==
                Status::kOk);
        REQUIRE(flow.load(graph, *doc, errors) == Status::kOk);
        for (const PortRecord& port : doc->ports()) {
            if (ports.size() < 20 && port.documentLevel && port.type == PortValueType::Number) {
                ports.push_back(port.id);
            }
        }
    }

    void frame(uint32_t index) {
        for (uint32_t i = 0; i < 16; ++i) {
            InputEvent ev;
            ev.kind = InputEvent::PointerMove;
            const float t = static_cast<float>((index * 16 + i) % 600) / 600.0f;
            ev.pos = Vec2{1280.0f * t, 720.0f * t};
            doc->dispatchInput(ev);
        }
        for (size_t p = 0; p < ports.size(); ++p) {
            (void)doc->setPort(ports[p], Value::ofFloat(static_cast<float>((index * 3 + p * 7) % 100)));
        }
        doc->update(1.0f / 60.0f);
        flow.tick(1.0f / 60.0f);
    }
};

/// T-478 (Docs/tasks/T-478.md item 1): 200 frames; from frame 50 on every tick allocates nothing,
/// its sink writes included.
void checkSteadyStateAllocs(const char* dir, const char* name) {
    BigFlow big;
    big.load(std::string(XT_TEST_FLOW_DIR) + "/../perf/flow_big/" + dir, name);
    PerfTracker tracker;
    tracker.setEnabled(true);
    big.flow.setPerfTracker(&tracker);
    constexpr uint32_t kFrames = 200;
    constexpr uint32_t kSteadyFrom = 50;  // The warm-up grows every buffer to its longest text.
    uint32_t allocFrames = 0;
    uint32_t evals = 0;
    uint32_t typed = 0;
    uint32_t text = 0;
    for (uint32_t f = 0; f < kFrames; ++f) {
        tracker.beginFrame(0.0f);
        big.frame(f);
        const FrameStats& stats = tracker.current();
        if (f < kSteadyFrom) {
            continue;
        }
        evals += stats.flowNodeEvals;
        typed += stats.flowSinkTyped;
        text += stats.flowSinkText;
        if (stats.flowAllocs != 0) {
            CAPTURE(f);
            CAPTURE(stats.flowAllocs);
            CHECK(stats.flowAllocs == 0u);
            ++allocFrames;
        }
    }
    CHECK(evals > 0u);  // The ticks did work.
    CHECK(typed > 0u);  // ... and wrote `set(port)` / setStyle sinks into the Document,
    CHECK(text == 0u);  // all of them typed (the one excluded path is not taken).
    CHECK(allocFrames == 0u);
}

}  // namespace

TEST_CASE("T-477: a typed setStyle write and a pointer port read allocate nothing") {
    BigFlow big;
    big.load(std::string(XT_TEST_FLOW_DIR) + "/../perf/flow_big/typical", "flow-typical");
    for (uint32_t f = 0; f < 30; ++f) {
        big.frame(f);
    }
    Document& doc = *big.doc;
    ElementRef target = doc.root().firstChild();
    REQUIRE(target.valid());
    StyleTemplate tmpl;
    const std::string_view literals[] = {"transform: translate(", "px, ", "px)"};
    REQUIRE(compileStyleTemplate(literals, tmpl));
    // T-477: the template write (ElementRef::setStyleTemplate()); setStyle() of the same texts
    // parses each one (and allocates while doing so).
    // The first write grows the element's runtime declaration list; the rest reuse it.
    REQUIRE(target.setStyleTemplate(tmpl, "transform: translate(1px, 2px)") == Status::kOk);
    uint64_t allocs = 0;
    char text[96];
    for (int i = 0; i < 100; ++i) {
        const int n = std::snprintf(text, sizeof(text), "transform: translate(%d.5px, %dpx)", i, -i);
        const uint64_t before = xt::perf::AllocCounter::total();
        CHECK(target.setStyleTemplate(tmpl, std::string_view(text, static_cast<size_t>(n))) == Status::kOk);
        allocs += xt::perf::AllocCounter::total() - before;
    }
    CHECK(allocs == 0u);
    // T-477: the root's bounding box (`#document.pointer*` / viewport reads): no space root above
    // it, so Document::nodeToScreen() builds nothing (an MSVC debug std::vector allocates even empty).
    const uint64_t before = xt::perf::AllocCounter::total();
    for (int i = 0; i < 100; ++i) {
        (void)doc.root().boundingBox();
    }
    CHECK(xt::perf::AllocCounter::total() == before);
}

TEST_CASE("T-478: a port write that notifies port listeners allocates nothing") {
    // Before T-478 BindingRuntime::notifyListeners() copied the listener list, every std::function
    // (and its heap-held capture) included, on each port change. Listeners whose captures are too
    // big for std::function's inline buffer make that copy allocate.
    compile::CompileOptions options;
    options.behavior = 1;
    options.emitMeta = true;
    std::vector<compile::SourceFile> files{
        {"main.xt",
         "<xt><head><style></style></head><body id=\"Root\"><div port:a=\"number\"></div></body></xt>"}};
    compile::CompileResult compiled = compile::compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(compiled.ok());
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{compiled.ir.data(), compiled.ir.size()}, false, nullptr, nullptr, doc) ==
            Status::kOk);
    doc->update(0.0f);
    const PortId port = doc->lookupPort("a");
    REQUIRE(port != kInvalidPort);
    struct Big {
        double pad[8] = {};
    };
    uint32_t calls = 0;
    for (int i = 0; i < 4; ++i) {
        Big big;
        big.pad[0] = i;
        doc->addPortListener([&calls, big](const PortEvent&) { calls += big.pad[0] >= 0 ? 1u : 0u; });
    }
    (void)doc->setPort(port, Value::ofFloat(-1.0f));  // The first change grows what it needs to.
    doc->update(0.0f);
    calls = 0;
    uint64_t allocs = 0;
    for (int i = 0; i < 100; ++i) {
        const uint64_t before = xt::perf::AllocCounter::total();
        CHECK(doc->setPort(port, Value::ofFloat(static_cast<float>(i))) == Status::kOk);
        allocs += xt::perf::AllocCounter::total() - before;
    }
    CHECK(calls == 400u);
    CHECK(allocs == 0u);
}

TEST_CASE("T-476: the allocation hook counts") {
    const uint64_t before = xt::perf::AllocCounter::total();
    auto probe = std::make_unique<int>(1);
    CHECK(xt::perf::AllocCounter::total() > before);
}

TEST_CASE("T-476: a steady-state tick of the large flow fixture allocates nothing") {
    checkSteadyStateAllocs("large", "flow-big");
}

TEST_CASE("T-476: a steady-state tick of the typical flow fixture allocates nothing") {
    checkSteadyStateAllocs("typical", "flow-typical");
}
