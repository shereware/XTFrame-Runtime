#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/property.h"
#include "xt/types.h"

/// T-654 (Docs/design/23-host-integration.md XF-097 "快照版本 4"): Document::snapshotState() /
/// restoreState() carry the in-flight CSS transitions and the pointer state, so the frames after a
/// restore are those of the uninterrupted run and the pointer goes on without PointerEnter / Leave.

using namespace xt;
using namespace xt::compile;

namespace {

/// `go` on #Root starts #a's opacity (1 s linear) and background-color (0.8 s ease-in-out)
/// transitions and #b's opacity one (0.6 s cubic-bezier after a 0.1 s delay); `more` starts #c's
/// (0.4 s steps), which no snapshot below lists.
constexpr const char* kXt = R"X(<xt><head><style>
#a, #b, #c { position: absolute; top: 0px; width: 40px; height: 40px; opacity: 1; }
#a { left: 0px; background-color: #ff0000;
     transition: opacity 1s linear, background-color 0.8s ease-in-out; }
.go #a { opacity: 0.2; background-color: #0000ff; }
#b { left: 50px; transition: opacity 0.6s cubic-bezier(0.3, 0.1, 0.2, 1) 0.1s; }
.go #b { opacity: 0.5; }
#c { left: 100px; transition: opacity 0.4s steps(4, end); }
.more #c { opacity: 0; }
</style></head><body id="Root"><div id="a"></div><div id="b"></div><div id="c"></div></body></xt>)X";

std::vector<uint8_t> compiled() {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"main.xt", kXt}};
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
    void pointer(InputEvent::Kind kind, float x, float y, uint8_t player = 0) {
        InputEvent e;
        e.kind = kind;
        e.pos = Vec2{x, y};
        e.player = player;
        ctx.pushInput(e);
    }
};

uint32_t bitsOf(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/// The bits of every value the transitions write.
std::vector<uint32_t> computedBits(Document& d) {
    std::vector<uint32_t> out;
    for (const char* id : {"a", "b", "c"}) {
        out.push_back(bitsOf(d.byId(id).property(prop::Opacity).f));
    }
    const Color c = d.byId("a").property(prop::BackgroundColor).c;
    for (const float v : {c.r, c.g, c.b, c.a}) {
        out.push_back(bitsOf(v));
    }
    return out;
}

/// The original run: `go` at frame 5, saved after frame 8 (#b still in its delay).
std::string runAndSave(Surface& a) {
    for (int f = 0; f < 8; ++f) {
        if (f == 5) {
            REQUIRE(a.doc().byId("Root").addClass("go"));
        }
        a.frame();
    }
    return a.doc().snapshotState();
}

/// Counts the pointer events on one element.
struct Counts {
    int enter = 0;
    int leave = 0;
    int click = 0;
    uint8_t clickPlayer = 0xFF;
    void listen(Document& d, const char* id) {
        d.addListener(d.byId(id), EventType::PointerEnter, [this](Event&) { ++enter; });
        d.addListener(d.byId(id), EventType::PointerLeave, [this](Event&) { ++leave; });
        d.addListener(d.byId(id), EventType::Click, [this](Event& e) {
            ++click;
            clickPlayer = e.player;
        });
    }
};

}  // namespace

TEST_CASE("t654: mid-transition save / restore: 30 frames equal the uninterrupted run bit for bit") {
    const std::vector<uint8_t> ir = compiled();
    Surface a(ir);
    const std::string snap = runAndSave(a);
    CAPTURE(snap);
    CHECK(snap.find(R"({"version":4,)") == 0);
    // Two properties of #a, #b's in its delay, with their style text and easing.
    CHECK(snap.find(R"X({"id":"a","property":"BackgroundColor","from":"color(srgb 1 0 0 / 1)",)X"
                    R"X("to":"color(srgb 0 0 1 / 1)","elapsed":)X") != std::string::npos);
    CHECK(snap.find(R"({"id":"a","property":"Opacity","from":"1","to":"0.200000003","elapsed":)") !=
          std::string::npos);
    CHECK(snap.find(R"("duration":1,"easing":"linear"})") != std::string::npos);
    CHECK(snap.find(R"({"id":"b","property":"Opacity","from":"1","to":"0.5","elapsed":-)") !=
          std::string::npos);
    CHECK(snap.find(R"X("easing":"cubic-bezier(0.300000012,0.100000001,0.200000003,1)"})X") !=
          std::string::npos);
    CHECK(snap.find(R"("pointer":{"0":{"x":0,"y":0,"buttons":0,"hover":"","pressed":"","inside":false}})") !=
          std::string::npos);

    Surface b(ir);
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    CHECK(b.doc().snapshotState() == snap);  // Before any update: the same state.
    for (int f = 0; f < 30; ++f) {
        CAPTURE(f);
        a.frame();
        b.frame();
        REQUIRE(computedBits(b.doc()) == computedBits(a.doc()));
    }
    CHECK(b.doc().snapshotState() == a.doc().snapshotState());
    CHECK(a.doc().byId("b").property(prop::Opacity).f < 1.0f);  // #b's delay ended inside the window.

    // Without the snapshot's transitions the restored classes show their end values at once.
    std::string noTransitions = snap;
    const size_t at = noTransitions.find(R"(,"transitions":[)");
    REQUIRE(at != std::string::npos);
    noTransitions.erase(at, noTransitions.find("],\"pointer\"", at) + 1 - at);
    Surface c(ir);
    REQUIRE(c.doc().restoreState(noTransitions) == Status::kOk);
    c.frame();
    CHECK(c.doc().byId("a").property(prop::Opacity).f == doctest::Approx(0.2f));
}

TEST_CASE("t654: a transition the snapshot does not list runs as usual; unknown entries are ignored") {
    const std::vector<uint8_t> ir = compiled();
    Surface a(ir);
    const std::string snap = runAndSave(a);
    Surface b(ir);
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    float mid = 0;  // #c in its 0.4 s steps transition.
    for (int f = 0; f < 30; ++f) {
        CAPTURE(f);
        if (f == 4) {  // #c's steps transition starts on both sides after the restore.
            REQUIRE(a.doc().byId("Root").addClass("more"));
            REQUIRE(b.doc().byId("Root").addClass("more"));
        }
        a.frame();
        b.frame();
        REQUIRE(computedBits(b.doc()) == computedBits(a.doc()));
        if (f == 15) {
            mid = b.doc().byId("c").property(prop::Opacity).f;
        }
    }
    CHECK(mid > 0.0f);
    CHECK(mid < 1.0f);

    // Entries without their element, property or a value the property takes change nothing.
    Surface c(ir);
    REQUIRE(c.doc().restoreState(
                R"({"version":4,"transitions":[)"
                R"({"id":"nope","property":"Opacity","from":"1","to":"0","elapsed":0.1,"duration":1},)"
                R"({"id":"a","property":"NoSuchProp","from":"1","to":"0","elapsed":0.1,"duration":1},)"
                R"({"id":"a","property":"Opacity","from":"1px","to":"0px","elapsed":0.1,"duration":1},)"
                R"({"id":"a","property":"Opacity","from":"x","to":"0","elapsed":0.1,"duration":1},)"
                R"({"id":"a","property":"Opacity","from":"1","to":"0","elapsed":0.1,"duration":0}]})") ==
            Status::kOk);
    CHECK(c.doc().animator().transitionCount() == 0u);
}

TEST_CASE("t654: a hover restored sends no PointerEnter; moving off sends PointerLeave") {
    const std::vector<uint8_t> ir = compiled();
    Surface a(ir);
    a.frame();
    a.pointer(InputEvent::PointerMove, 20, 20);
    a.frame();
    REQUIRE(a.doc().byId("a").hasState(kStateHover));
    const std::string snap = a.doc().snapshotState();
    CAPTURE(snap);
    CHECK(
        snap.find(R"("pointer":{"0":{"x":20,"y":20,"buttons":0,"hover":"a","pressed":"","inside":true}})") !=
        std::string::npos);

    Surface b(ir);
    Counts counts;
    counts.listen(b.doc(), "a");
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    CHECK(b.doc().byId("a").hasState(kStateHover));
    CHECK(b.doc().byId("Root").hasState(kStateHover));
    b.frame();
    b.pointer(InputEvent::PointerMove, 22, 21);  // Still on #a.
    b.frame();
    b.frame();
    CHECK(counts.enter == 0);
    CHECK(counts.leave == 0);
    b.pointer(InputEvent::PointerMove, 300, 200);  // Off #a.
    b.frame();
    CHECK(counts.enter == 0);
    CHECK(counts.leave == 1);
    CHECK_FALSE(b.doc().byId("a").hasState(kStateHover));
}

TEST_CASE("t654: a press restored ends in one Click at its PointerUp, with no PointerEnter") {
    const std::vector<uint8_t> ir = compiled();
    Surface a(ir);
    a.frame();
    a.pointer(InputEvent::PointerMove, 20, 20);
    a.pointer(InputEvent::PointerDown, 20, 20);
    a.frame();
    REQUIRE(a.doc().byId("a").hasState(kStateActive));
    const std::string snap = a.doc().snapshotState();
    CAPTURE(snap);
    CHECK(
        snap.find(R"("pointer":{"0":{"x":20,"y":20,"buttons":1,"hover":"a","pressed":"a","inside":true}})") !=
        std::string::npos);

    Surface b(ir);
    Counts counts;
    counts.listen(b.doc(), "a");
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    CHECK(b.doc().byId("a").hasState(kStateActive));
    b.frame();
    b.pointer(InputEvent::PointerUp, 21, 20);
    b.frame();
    CHECK(counts.click == 1);
    CHECK(counts.enter == 0);
    CHECK(counts.leave == 0);
    CHECK_FALSE(b.doc().byId("a").hasState(kStateActive));
    const std::string after = b.doc().snapshotState();
    CHECK(after.find(R"("buttons":0,"hover":"a","pressed":"")") != std::string::npos);

    // Released off the pressed element: no Click, as for an ordinary press (08.4).
    Surface c(ir);
    Counts off;
    off.listen(c.doc(), "a");
    REQUIRE(c.doc().restoreState(snap) == Status::kOk);
    c.frame();
    c.pointer(InputEvent::PointerUp, 300, 200);
    c.frame();
    CHECK(off.click == 0);
    CHECK_FALSE(c.doc().byId("a").hasState(kStateActive));
}

TEST_CASE("t654: the pointer of player 1 is saved under its player and its Click carries it") {
    const std::vector<uint8_t> ir = compiled();
    Surface a(ir);
    a.frame();
    a.pointer(InputEvent::PointerMove, 70, 10, 1);
    a.pointer(InputEvent::PointerDown, 70, 10, 1);
    a.frame();
    const std::string snap = a.doc().snapshotState();
    CAPTURE(snap);
    CHECK(
        snap.find(R"("pointer":{"1":{"x":70,"y":10,"buttons":1,"hover":"b","pressed":"b","inside":true}})") !=
        std::string::npos);

    Surface b(ir);
    Counts counts;
    counts.listen(b.doc(), "b");
    REQUIRE(b.doc().restoreState(snap) == Status::kOk);
    CHECK(b.doc().snapshotState() == snap);
    b.frame();
    b.pointer(InputEvent::PointerUp, 70, 10, 1);
    b.frame();
    CHECK(counts.click == 1);
    CHECK(counts.clickPlayer == 1);
    CHECK(counts.enter == 0);

    // A snapshot of player 0's pointer moves it back to player 0 (one pointer per document).
    Surface c(ir);
    REQUIRE(
        c.doc().restoreState(
            R"({"version":4,"pointer":{"0":{"x":20,"y":20,"buttons":0,"hover":"a","pressed":"","inside":true}}})") ==
        Status::kOk);
    CHECK(c.doc().byId("a").hasState(kStateHover));
    CHECK_FALSE(c.doc().byId("b").hasState(kStateHover));
    CHECK(c.doc().snapshotState().find(R"("pointer":{"0":{"x":20,"y":20,)") != std::string::npos);
}

TEST_CASE("t654: version 1 to 3 snapshots still load; version 5 does not") {
    const std::vector<uint8_t> ir = compiled();
    Surface a(ir);
    a.frame();
    a.pointer(InputEvent::PointerMove, 20, 20);
    a.frame();
    for (const char* v :
         {R"({"version":1,"states":{}})", R"({"version":2,"classes":{"Root":{"add":["go"]}}})",
          R"({"version":3,"animations":[]})"}) {
        CAPTURE(v);
        Surface b(ir);
        CHECK(b.doc().restoreState(v) == Status::kOk);
    }
    // A version 3 snapshot (no `transitions`, no `pointer`) leaves the pointer as it is.
    CHECK(a.doc().restoreState(R"({"version":3,"ports":{}})") == Status::kOk);
    CHECK(a.doc().byId("a").hasState(kStateHover));
    std::string v5 = a.doc().snapshotState();
    REQUIRE(v5.find(R"({"version":4)") == 0);
    v5.replace(0, std::strlen(R"({"version":4)"), R"({"version":5)");
    CHECK(a.doc().restoreState(v5) == Status::kInvalidArgument);
}
