#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "compile_check.h"
#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/perf.h"
#include "xt/perf_record.h"
#include "xt/sw/sw_renderer.h"
#include "xt/thread_clock.h"
#include "xt/types.h"

/// T-750 (Docs/design/57-measurement.md §57.2, §57.5): FrameRecord v1 -- the column table, the closed
/// budget of marked frames, the counts against FrameStats, the frame marks and the thread CPU switch.

using namespace xt;
using namespace xt::compile;

namespace {

std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out(1);
    for (char c : line) {
        if (c == ',') {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    return out;
}

std::string header() {
    std::string text(frameRecordCsvHeader(nullptr, 0), '\0');
    frameRecordCsvHeader(text.data(), text.size() + 1);
    return text;
}

std::string row(const FrameRecord& r) {
    std::string text(frameRecordCsvRow(r, nullptr, 0), '\0');
    frameRecordCsvRow(r, text.data(), text.size() + 1);
    return text;
}

struct Ui {
    std::unique_ptr<Context> ctx = std::make_unique<Context>(ContextConfig{});
    DocumentHandle handle = 0;
    CommandList list;
    DamageList damage;

    Document& doc() { return *ctx->document(handle); }
    /// One marked frame, as a host's loop: mark, update, `between` (a host write), render, mark.
    FrameStats frame(void (*between)(Ui&) = nullptr) {
        ctx->perf().markFrameStart();
        ctx->update(1.0f / 60.0f);
        if (between != nullptr) {
            between(*this);
        }
        ctx->render(list, &damage);
        ctx->perf().markFrameEnd();
        return ctx->perf().current();
    }
};

std::unique_ptr<Ui> open(std::string_view style, std::string_view body) {
    auto ui = std::make_unique<Ui>();
    const std::string text = "<xt><head><style>" + std::string(style) + "</style></head>\n<body>" +
                             std::string(body) + "</body></xt>";
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", text}};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    test::requireCompiled(result);
    Result<DocumentHandle> loaded = ui->ctx->loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(loaded.ok());
    ui->handle = loaded.value();
    ui->ctx->setViewport(Rect{0, 0, 320, 240}, 1.0f);
    ui->ctx->perf().setEnabled(true);
    return ui;
}

const char* kBoxes =
    ".b { width: 40px; height: 20px; background-color: #f80; }"
    "@keyframes p { from { opacity: 0.2 } to { opacity: 1 } }"
    ".a { animation: p 1s linear 0s infinite; }";

/// The budget closes: the eight phases plus unaccounted are the frame (float tolerance).
void checkClosed(const FrameRecord& r) {
    REQUIRE(r.frameMarked == 1u);
    const double sum = double(r.inputMs) + r.updateMs + r.flowMs + r.paintMs + r.damageMs + r.buildMs +
                       r.submitMs + r.waitMs + r.unaccountedMs;
    CHECK(std::fabs(sum - double(r.frameWallMs)) < 1e-3);
    CHECK(r.frameWallMs > 0.0f);
}

/// The record's counts are FrameStats's.
void checkCounts(const FrameRecord& r, const FrameStats& f) {
    CHECK(r.frame == f.frameIndex);
    CHECK(r.styledElements == f.styledElements);
    CHECK(r.laidOutElements == f.laidOutElements);
    CHECK(r.layoutRoots == f.layoutRoots);
    CHECK(r.layoutFullPasses == f.layoutFullPasses);
    CHECK(r.activeAnimations == f.activeAnimations);
    CHECK(r.animEvaluated == f.animEvaluated);
    CHECK(r.commands == f.commands);
    CHECK(r.glyphs == f.glyphs);
    CHECK(r.paintSkipped == f.paintSkipped);
    CHECK(r.arenaNodes == f.arenaNodes);
    CHECK(r.commandBytes == f.commandBytes);
    CHECK(r.idlePaintMs == f.idlePaintMs);
    CHECK(std::memcmp(r.causes, f.causes, sizeof(r.causes)) == 0);
    CHECK(r.inputMs == f.inputMs);
    CHECK(r.updateMs == f.updateMs - f.inputMs);
    CHECK(r.paintMs == f.paintMs);
    CHECK(r.damageMs == f.damageMs);
    CHECK(r.updateStyleMs == f.styleMs);
    CHECK(r.updateLayoutMs == f.layoutMs);
}

}  // namespace

TEST_CASE("FrameRecord: the column table writes every field once, header and rows alike") {
    const std::string h = header();
    const std::vector<std::string> names = splitCsv(h);
    CHECK(names.size() == kFrameRecordCsvColumns);
    CHECK(std::set<std::string>(names.begin(), names.end()).size() == names.size());
    CHECK(names.front() == "version");
    CHECK(names[6] == "frame_wall_ms");
    CHECK(names[8] == "unaccounted_ms");
    // T-753: the fields appended after the causes come last.
    size_t tail = 0;
#define XT_TEST_COUNT_TAIL(member, csv, kind) ++tail;
    XT_FRAME_RECORD_TAIL_FIELDS(XT_TEST_COUNT_TAIL)
#undef XT_TEST_COUNT_TAIL
    CHECK(tail == 7u);
    CHECK(names[names.size() - tail - 1] == "cause_load_paint");
    CHECK(names[names.size() - tail] == "redundant_styled");
    CHECK(names.back() == "repainted_unchanged_first_sight");
    // Every scalar is written (4 + 83 * 4 bytes) plus the causes: the struct holds nothing else.
    size_t scalars = tail;
#define XT_TEST_COUNT(member, csv, kind) ++scalars;
    XT_FRAME_RECORD_FIELDS(XT_TEST_COUNT)
#undef XT_TEST_COUNT
    CHECK(sizeof(FrameRecord) == sizeof(uint32_t) * (1 + scalars + kDirtyCauseCount * kDirtyKindCount));
    FrameRecord r;
    r.version = kFrameRecordVersion;
    r.frameWallMs = 1.25f;
    r.causes[3][1] = 7;
    r.redundantLaidOut = 9;
    const std::vector<std::string> values = splitCsv(row(r));
    REQUIRE(values.size() == names.size());
    CHECK(values[0] == "1");
    CHECK(values[6] == "1.2500");
    CHECK(values[kFrameRecordCsvColumns - tail - 44 + 3 * 4 + 1] == "7");
    CHECK(values[kFrameRecordCsvColumns - tail + 1] == "9");
}

TEST_CASE("FrameRecord: the CSV writers truncate like snprintf and the append helpers count") {
    const size_t need = frameRecordCsvHeader(nullptr, 0);
    REQUIRE(need > 100);
    char small[16];
    CHECK(frameRecordCsvHeader(small, sizeof(small)) == need);
    CHECK(std::strlen(small) == sizeof(small) - 1);
    CHECK(std::string(small) == header().substr(0, sizeof(small) - 1));
    char buf[64] = "a";
    size_t n = csvAppendText(buf, sizeof(buf), 1, "host.x");
    n = csvAppendNumber(buf, sizeof(buf), n, 2.5, 2);
    CHECK(std::string(buf) == "a,host.x,2.50");
    CHECK(n == 13);
}

TEST_CASE("FrameRecord: unmarked frames carry no frame time and unaccounted 0") {
    auto ui = open(kBoxes, "<div class=\"b\"></div>");
    ui->ctx->update(1.0f / 60.0f);
    ui->ctx->render(ui->list, &ui->damage);
    const FrameRecord r = makeFrameRecord(ui->ctx->perf().current());
    CHECK(r.frameMarked == 0u);
    CHECK(r.frameWallMs == 0.0f);
    CHECK(r.unaccountedMs == 0.0f);
    // A start without its end does not leak into the next frame either.
    ui->ctx->perf().markFrameStart();
    ui->ctx->update(1.0f / 60.0f);
    ui->ctx->update(1.0f / 60.0f);
    ui->ctx->perf().markFrameEnd();
    CHECK(ui->ctx->perf().current().frameMarked == 0u);
}

TEST_CASE("FrameRecord: a static frame closes its budget and matches FrameStats") {
    auto ui = open(kBoxes, "<div class=\"b\"></div><div class=\"b\"></div>");
    ui->frame();
    const FrameStats f = ui->frame();
    const FrameRecord r = makeFrameRecord(f);
    checkClosed(r);
    checkCounts(r, f);
    CHECK(r.version == kFrameRecordVersion);
    CHECK(r.buildMs == 0.0f);     // no backend in this test
    CHECK(r.inputCpuMs == 0.0f);  // thread CPU timing is off by default
}

TEST_CASE("FrameRecord: an animated frame closes its budget and matches FrameStats") {
    auto ui = open(kBoxes, "<div class=\"b a\"></div><div class=\"b a\"></div><div class=\"b\"></div>");
    ui->frame();
    const FrameStats f = ui->frame();
    const FrameRecord r = makeFrameRecord(f);
    checkClosed(r);
    checkCounts(r, f);
    CHECK(r.animEvaluated > 0u);
}

TEST_CASE("FrameRecord: a port write frame closes its budget and matches FrameStats") {
    auto ui = open(kBoxes, "<div id=\"x\" class=\"b\" port:hp=\"number\" bind:width=\"hp\"></div>");
    ui->frame();
    ui->frame(
        [](Ui& u) { REQUIRE(u.doc().setPort(u.doc().lookupPort("hp"), Value::ofFloat(77)) == Status::kOk); });
    const FrameStats f = ui->frame();
    const FrameRecord r = makeFrameRecord(f);
    checkClosed(r);
    checkCounts(r, f);
    CHECK(r.laidOutElements > 0u);
}

TEST_CASE("FrameRecord: thread CPU timing is its own switch, not setDetail") {
    auto ui = open(kBoxes, "<div class=\"b a\"></div>");
    ui->ctx->perf().setDetail(1);
    ui->frame();
    CHECK(ui->ctx->perf().current().updateCpuMs == 0.0f);
    CHECK(ui->ctx->perf().current().frameCpuMs == 0.0f);
    ui->ctx->perf().setDetail(0);
    ui->ctx->perf().setThreadCpuTiming(true);
    CHECK(ui->ctx->perf().threadClock().calibrated());
    FrameStats f;
    for (int i = 0; i < 4; ++i) {
        f = ui->frame();
    }
    CHECK(f.updateCpuMs >= 0.0f);
    CHECK(f.frameCpuMs > 0.0f);
    CHECK(f.frameCpuMs <= f.frameWallMs * 1.5f + 0.5f);
    const ThreadClock& clock = ui->ctx->perf().threadClock();
    const double c0 = clock.nowMs();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 2000000; ++i) {
        sink = sink + uint64_t(i);
    }
    CHECK(clock.nowMs() > c0);
    char text[160];
    CHECK(clock.describe(text, sizeof(text)) > 0);
}

TEST_CASE("FrameRecord: the SW backend's render is the build phase, inside the closed budget") {
    auto ui = open(kBoxes, "<div class=\"b a\"></div><div class=\"b\"></div>");
    ui->ctx->perf().setThreadCpuTiming(true);
    std::vector<uint8_t> pixels(320u * 240u * 4u);
    sw::SwRenderer renderer;
    renderer.setTarget(sw::SwTarget{pixels.data(), 320, 240, 320 * 4});
    FrameStats f;
    for (int i = 0; i < 3; ++i) {
        ui->ctx->perf().markFrameStart();
        ui->ctx->update(1.0f / 60.0f);
        ui->ctx->render(ui->list, &ui->damage);
        renderer.render(ui->list, nullptr, nullptr, &ui->doc().ir(), &ui->ctx->perf());
        ui->ctx->perf().markFrameEnd();
        f = ui->ctx->perf().current();
    }
    const FrameRecord r = makeFrameRecord(f);
    CHECK(r.backend == static_cast<uint32_t>(PerfBackend::Sw));
    CHECK(r.buildMs == f.renderMs);
    CHECK(r.buildMs > 0.0f);
    CHECK(r.buildCpuMs >= 0.0f);
    CHECK(r.submitMs == 0.0f);
    checkClosed(r);
    // The SW render ran after Context::render(): unaccounted is only the gaps between the scopes.
    CHECK(r.unaccountedMs < r.frameWallMs);
}
