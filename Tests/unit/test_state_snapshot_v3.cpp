#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/animator.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-651 (Docs/design/23-host-integration.md XF-097 "快照版本 3"): Document::snapshotState() /
/// restoreState() carry the CSS `animation` items, so the frames after a restore are the frames of
/// the uninterrupted run, bit for bit.

using namespace xt;
using namespace xt::compile;

namespace {

/// Six shapes, all started by the `go` class on #Root: #alt 1.4 s infinite alternate (in its
/// reverse leg at the save), #once a 320 ms forwards one-shot (ended), #held paused by `hold`,
/// #late still in its 2 s delay, #rev an infinite `reverse`. `more` (the recompiled IR only) gives
/// #fresh an animation the snapshot does not know.
constexpr const char* kStyle = R"X(
@keyframes fade { from { opacity: 1 } to { opacity: 0.1 } }
@keyframes grow { from { width: 10px } to { width: 90px } }
@keyframes tall { from { height: 4px } to { height: 40px } }
#alt, #once, #held, #late, #rev, #fresh { width: 20px; height: 10px; opacity: 1; }
.go #alt { animation: fade 1.4s ease-in-out infinite alternate, tall 0.7s linear infinite; }
.go #once { animation: grow 320ms linear forwards; }
.go #held { animation: fade 1s linear infinite; }
.go.hold #held { animation: fade 1s linear infinite paused; }
.go #late { animation: grow 1s linear 2s infinite; }
.go #rev { animation: tall 0.9s ease-out infinite reverse; }
)X";

constexpr const char* kBody = R"X(</style></head><body id="Root">
<div id="alt"></div><div id="once"></div><div id="held"></div><div id="late"></div><div id="rev"></div>
<div id="fresh"></div>
</body></xt>)X";

constexpr const char* kIds[] = {"alt", "once", "held", "late", "rev"};

std::vector<uint8_t> compiled(std::string_view extraStyle) {
    CompileOptions options;
    options.emitMeta = false;
    const std::string xt = "<xt><head><style>" + std::string(kStyle) + std::string(extraStyle) + kBody;
    std::vector<SourceFile> files{{"main.xt", xt}};
    CompileResult r = compileDocumentFromStrings(files, "main.xt", options);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    return r.ir;
}

/// One Context with `ir` loaded (not updated yet); `ir` must outlive it.
struct Surface {
    Context ctx{ContextConfig{}};
    DocumentHandle handle = 0;
    explicit Surface(const std::vector<uint8_t>& ir) {
        ctx.setViewport(Rect{0, 0, 400, 300}, 1.0f);
        Result<DocumentHandle> h = ctx.loadDocument(IrBlob{ir.data(), ir.size()}, false);
        REQUIRE(h.ok());
        handle = h.value();
    }
    Document& doc() { return *ctx.document(handle); }
    void frame() { ctx.update(1.0f / 60.0f); }
};

/// The bits of every computed value the six shapes animate.
std::vector<uint32_t> computedBits(Document& d) {
    std::vector<uint32_t> out;
    for (const char* id : kIds) {
        const ElementRef e = d.byId(id);
        for (const float v : {e.property(prop::Opacity).f, e.property(prop::Width).len.value,
                              e.property(prop::Height).len.value}) {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            out.push_back(bits);
        }
    }
    return out;
}

/// The original run up to its save: `go` at frame 10, `hold` at frame 40, saved after frame 110.
std::string runAndSave(Surface& a) {
    for (int f = 0; f < 110; ++f) {
        if (f == 10) {
            REQUIRE(a.doc().byId("Root").addClass("go"));
        }
        if (f == 40) {
            REQUIRE(a.doc().byId("Root").addClass("hold"));
        }
        a.frame();
    }
    return a.doc().snapshotState();
}

}  // namespace

TEST_CASE("t651: six CSS animation shapes round-trip; 30 frames equal the uninterrupted run bit for bit") {
    const std::vector<uint8_t> ir = compiled("");
    Surface a(ir);
    const std::string snap = runAndSave(a);
    CAPTURE(snap);
    CHECK(snap.find(R"({"version":3,)") == 0);
    CHECK(snap.find(R"({"id":"alt","index":0,"name":"fade","elapsed":)") != std::string::npos);
    CHECK(snap.find(R"("direction":"alternate"})") != std::string::npos);
    CHECK(snap.find(R"({"id":"alt","index":1,"name":"tall",)") != std::string::npos);
    CHECK(snap.find(R"("direction":"reverse"})") != std::string::npos);
    CHECK(snap.find(R"("paused":true)") != std::string::npos);  // #held.
    CHECK(snap.find(R"({"id":"late","index":0,"name":"grow","elapsed":-)") !=
          std::string::npos);  // In its delay.

    Surface b(ir);
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    CHECK(b.doc().snapshotState() == snap);  // Before any update: the same state, the clock included.
    for (int f = 0; f < 30; ++f) {
        CAPTURE(f);
        if (f == 20) {  // Resumed on both sides: #held goes on from where it was paused.
            REQUIRE(a.doc().byId("Root").removeClass("hold"));
            REQUIRE(b.doc().byId("Root").removeClass("hold"));
        }
        a.frame();
        b.frame();
        REQUIRE(computedBits(b.doc()) == computedBits(a.doc()));
    }
    CHECK(b.doc().snapshotState() == a.doc().snapshotState());
    // #late's delay ended inside the window: it is playing on both sides.
    CHECK(a.doc().byId("late").property(prop::Width).len.value > 10.0f);

    // Without the snapshot every item starts from the beginning: other values.
    Surface c(ir);
    REQUIRE(c.doc().byId("Root").addClass("go"));
    c.frame();
    CHECK(computedBits(c.doc()) != computedBits(a.doc()));
}

TEST_CASE("t651: an animation the snapshot does not list starts from 0; the listed ones are matched") {
    const std::vector<uint8_t> ir = compiled("");
    Surface a(ir);
    const std::string snap = runAndSave(a);
    // The recompiled document gives #fresh an animation (a class the snapshot restores starts it).
    const std::vector<uint8_t> ir2 = compiled(".go #fresh { animation: fade 1s linear infinite; }");
    Surface b(ir2);
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    const std::vector<uint32_t> fresh = b.doc().animationsOf(b.doc().byId("fresh"));
    REQUIRE(fresh.size() == 1);
    CHECK(b.doc().animator().elapsedSeconds(fresh[0], b.ctx.clock()) == 0.0);
    for (int f = 0; f < 30; ++f) {
        a.frame();
        b.frame();
        CAPTURE(f);
        REQUIRE(computedBits(b.doc()) == computedBits(a.doc()));
    }
    CHECK(b.doc().animator().elapsedSeconds(fresh[0], b.ctx.clock()) == doctest::Approx(0.5).epsilon(1e-4));
    CHECK(b.doc().byId("fresh").property(prop::Opacity).f == doctest::Approx(0.55f).epsilon(1e-3));

    // Entries without an instance (unknown id, wrong name or index) are ignored.
    Surface c(ir);
    REQUIRE(
        c.doc().restoreState(
            R"({"version":3,"animations":[{"id":"nope","index":0,"name":"fade","elapsed":1},)"
            R"({"id":"alt","index":5,"name":"fade","elapsed":1},{"id":"rev","index":0,"name":"x","elapsed":1}]})") ==
        Status::kOk);
}

TEST_CASE("t651: version 1 and 2 snapshots still load; version 4 does not") {
    const std::vector<uint8_t> ir = compiled("");
    Surface a(ir);
    const std::string snap = runAndSave(a);
    std::string v2 = snap;
    v2.replace(0, std::strlen(R"({"version":3)"), R"({"version":2)");
    const size_t at = v2.find(R"(,"animations":[)");
    REQUIRE(at != std::string::npos);
    v2.erase(at, v2.find("],\"classes\"", at) + 1 - at);
    CAPTURE(v2);
    Surface b(ir);
    REQUIRE(b.doc().restoreState(v2) == Status::kOk);
    CHECK(b.doc().byId("Root").hasClass("go"));
    // Without `animations` the items start at the restored clock: #alt's fade is back at its start.
    b.frame();
    CHECK(b.doc().byId("alt").property(prop::Opacity).f == doctest::Approx(1.0f).epsilon(1e-3));

    Surface c(ir);
    CHECK(c.doc().restoreState(R"({"version":1,"states":{}})") == Status::kOk);
    std::string v4 = snap;
    v4.replace(0, std::strlen(R"({"version":3)"), R"({"version":4)");
    CHECK(c.doc().restoreState(v4) == Status::kInvalidArgument);
}
