#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/perf.h"
#include "xt/perf_record.h"
#include "xt/types.h"

/// T-753 (Docs/design/57-measurement.md §57.2 item 3, §57.3 item 4): the redundant-work counts
/// (FrameStats::redundantStyled / redundantLaidOut / redundantCommands) and the second-level zones
/// (XT_PERF_ZONE, PerfTracker::zones()). The repaintedUnchanged counts are in
/// Tests/gpu/test_repainted_unchanged.cpp.

using namespace xt;
using namespace xt::compile;

namespace {

struct Ui {
    std::unique_ptr<Context> ctx = std::make_unique<Context>(ContextConfig{});
    DocumentHandle handle = 0;
    CommandList list;
    DamageList damage;

    Document& doc() { return *ctx->document(handle); }
    FrameStats frame() {
        ctx->update(1.0f / 60.0f);
        ctx->render(list, &damage);
        return ctx->perf().current();
    }
};

std::unique_ptr<Ui> open(std::string_view style, std::string_view body, int detail) {
    auto ui = std::make_unique<Ui>();
    const std::string text = "<xt><head><style>" + std::string(style) + "</style></head>\n<body>" +
                             std::string(body) + "</body></xt>";
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", text}};
    CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
    REQUIRE(result.ok());
    Result<DocumentHandle> loaded = ui->ctx->loadDocument(IrBlob{result.ir.data(), result.ir.size()}, true);
    REQUIRE(loaded.ok());
    ui->handle = loaded.value();
    ui->ctx->setViewport(Rect{0, 0, 320, 240}, 1.0f);
    ui->ctx->perf().setEnabled(true);
    ui->ctx->perf().setDetail(detail);
    return ui;
}

const char* kStyle =
    ".b { width: 40px; height: 20px; background-color: #f80; }"
    ".row { display: flex; } .t { font-size: 12px; border: 1px solid #000; }";
const char* kBody = "<div class=\"row\"><div id=\"x\" class=\"b\"></div><div class=\"b\"></div></div>";

/// Spins until `ms` of wall time passed.
void spin(double ms) {
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() < ms) {
    }
}

}  // namespace

TEST_CASE("redundant work: writing the style value an element already has -- every re-style redundant") {
    auto ui = open(kStyle, kBody, 1);
    const FrameStats first = ui->frame();
    CHECK(first.styledElements > 0u);
    CHECK(first.redundantStyled == 0u);  // a first resolve is not a re-style
    ui->frame();
    REQUIRE(ui->doc().byId("x").setStyle("width: 40px") == Status::kOk);
    const FrameStats same = ui->frame();
    // T-763: #x alone re-styled (a runtime declaration marks no ancestor), into the style it had.
    CHECK(same.styledElements == 1u);
    CHECK(same.redundantStyled == 1u);
    CHECK(same.laidOutElements == 0u);  // the same width dirties no layout
    // Painted again (the write moved the visual generation) into the same commands.
    CHECK(same.commands > 0u);
    CHECK(same.redundantCommands == same.commands);
    const FrameRecord r = makeFrameRecord(same);
    CHECK(r.redundantStyled == same.styledElements);
    CHECK(r.redundantCommands == same.commands);
}

TEST_CASE("redundant work: laid out again with the same boxes") {
    auto ui = open(kStyle, kBody, 1);
    ui->frame();
    ui->frame();
    // A layout property whose value does not change the box (40px is above the minimum).
    REQUIRE(ui->doc().byId("x").setStyle("min-width: 10px") == Status::kOk);
    const FrameStats f = ui->frame();
    CHECK(f.styledElements >= 1u);
    CHECK(f.redundantStyled + 1u == f.styledElements);  // #x's own style did change
    CHECK(f.laidOutElements >= 1u);
    CHECK(f.redundantLaidOut >= 1u);  // every laid-out element kept its box
    CHECK(f.redundantLaidOut <= f.laidOutElements);
}

TEST_CASE("redundant work: a value that changes is not redundant") {
    auto ui = open(kStyle, kBody, 1);
    ui->frame();
    ui->frame();
    REQUIRE(ui->doc().byId("x").setStyle("width: 60px") == Status::kOk);
    const FrameStats changed = ui->frame();
    CHECK(changed.styledElements >= 1u);
    CHECK(changed.redundantStyled + 1u == changed.styledElements);  // #x changed
    CHECK(changed.laidOutElements >= 1u);
    CHECK(changed.redundantLaidOut < changed.laidOutElements);  // #x's box changed
    CHECK(changed.redundantCommands < changed.commands);        // its fill changed
}

TEST_CASE("redundant work: nothing is counted at detail level 0") {
    auto ui = open(kStyle, kBody, 0);
    ui->frame();
    ui->frame();
    REQUIRE(ui->doc().byId("x").setStyle("width: 40px") == Status::kOk);
    const FrameStats f = ui->frame();
    CHECK(f.styledElements >= 1u);
    CHECK(f.redundantStyled == 0u);
    CHECK(f.redundantLaidOut == 0u);
    CHECK(f.redundantCommands == 0u);
}

TEST_CASE("zones: the fixed list, each under its phase") {
    for (uint32_t z = 0; z < kPerfZoneCount; ++z) {
        const auto zone = static_cast<PerfZone>(z);
        const std::string name = perfZoneName(zone);
        const std::string phase = perfZonePhase(zone);
        CHECK(name.rfind(phase + ".", 0) == 0);
        CHECK(std::string(perfZoneCsvName(zone)).find('.') == std::string::npos);
    }
}

TEST_CASE("zones: nested zones are exclusive and counted once each") {
    PerfTracker t;
    t.setEnabled(true);
    t.setDetail(2);
    REQUIRE(t.zoneTiming());
    // T-799: only relations that hold however the scheduler slices the wall time. The windows are
    // taken with the same steady clock outside each zone, so a zone can never last longer than the
    // window around it; the spins only make an inclusive (non-exclusive) total likely to break them.
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    Clock::time_point outerBegin, innerBegin, innerEnd, outerEnd;
    outerBegin = Clock::now();
    {
        XT_PERF_ZONE(&t, PaintText);
        spin(0.2);
        innerBegin = Clock::now();
        {
            XT_PERF_ZONE(&t, PaintImages);
            spin(1.0);
        }
        innerEnd = Clock::now();
        spin(0.2);
    }
    outerEnd = Clock::now();
    const PerfZoneTotals& z = t.zones();
    const auto text = static_cast<uint32_t>(PerfZone::PaintText);
    const auto images = static_cast<uint32_t>(PerfZone::PaintImages);
    constexpr double kEps = 1e-6;  // the sum of two doubles against one
    CHECK(z.count[text] == 1u);
    CHECK(z.count[images] == 1u);
    CHECK(z.ms[text] >= 0.0);
    CHECK(z.ms[images] >= 0.0);
    // The inner zone lies inside its window; exclusive: the two zones together are the outer
    // zone's wall time once, which lies inside the outer window.
    CHECK(z.ms[images] <= ms(innerBegin, innerEnd) + kEps);
    CHECK(z.ms[text] + z.ms[images] <= ms(outerBegin, outerEnd) + kEps);
    t.reset();
    CHECK(t.zones().count[text] == 0u);
    PerfZoneScope null(nullptr, PerfZone::PaintText);  // a null tracker is allowed
}

TEST_CASE("zones: not timed below detail level 2 or while disabled") {
    for (int detail : {0, 1}) {
        auto ui = open(kStyle, kBody, detail);
        ui->frame();
        ui->frame();
        for (uint32_t z = 0; z < kPerfZoneCount; ++z) {
            CHECK(ui->ctx->perf().zones().count[z] == 0u);
            CHECK(ui->ctx->perf().zones().ms[z] == 0.0);
        }
    }
    PerfTracker t;
    t.setDetail(2);
    CHECK_FALSE(t.zoneTiming());  // disabled
    {
        XT_PERF_ZONE(&t, BuildWalk);
    }
    CHECK(t.zones().count[static_cast<uint32_t>(PerfZone::BuildWalk)] == 0u);
}

TEST_CASE("zones: a document frame at detail level 2 times its style, layout and paint zones") {
    auto ui = open(kStyle, std::string(kBody) + "<div class=\"t\">Hello</div>", 2);
    ui->frame();
    const PerfZoneTotals& z = ui->ctx->perf().zones();
    auto count = [&](PerfZone zone) { return z.count[static_cast<uint32_t>(zone)]; };
    CHECK(count(PerfZone::UpdateStyleMatch) >= 4u);  // every element resolved once
    CHECK(count(PerfZone::UpdateStyleCascade) == count(PerfZone::UpdateStyleMatch));
    CHECK(count(PerfZone::UpdateLayoutFlex) >= 1u);
    CHECK(count(PerfZone::PaintBorders) >= 1u);
    CHECK(count(PerfZone::PaintText) >= 1u);
    CHECK(count(PerfZone::UpdateLayoutGrid) == 0u);
    CHECK(count(PerfZone::BuildWalk) == 0u);  // no GPU backend here
}
