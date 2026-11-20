#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/ir.h"
#include "xt/perf.h"
#include "xt/property.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// `@timeline` and TimelineGroup (Docs/design/28-animation-editing.md §28.1, 04-timeline.md
/// §4.10.1-4.10.2, 12-compiler.md §12.7, 13-api.md §13.8.2, 11-lottie-mapping.md §11.9,
/// Docs/tasks/T-172.md).

using namespace xt;
using namespace xt::compile;

namespace {

constexpr const char* kIntroStyle = R"(
  #A { position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; }
  #B { position: absolute; left: 30px; top: 0px; width: 20px; height: 20px; }
  @timeline intro {
    marker: ready 1s;
    #A { 0s { opacity: 0 } .4s ease-out { opacity: 1 } }
    #B { .3s { opacity: 0 } 700ms { opacity: 1 } }
    sound { .4s ui_whoosh }
  }
)";

std::string doc(std::string_view style, std::string_view body = "<div id=\"A\"></div><div id=\"B\"></div>") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"body\">" +
           std::string(body) + "</body></xt>";
}

CompileResult compileStrings(const std::vector<SourceFile>& files, std::string_view mainName = "main.xt") {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_DATA_DIR;
    return compileDocumentFromStrings(files, mainName, options);
}

bool hasError(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
};

Loaded load(const std::string& xt, const std::vector<SourceFile>& extra = {}) {
    Loaded out;
    std::vector<SourceFile> files{{"main.xt", xt}};
    files.insert(files.end(), extra.begin(), extra.end());
    out.result = compileStrings(files);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    Status status =
        Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, nullptr, nullptr, out.doc);
    REQUIRE(status == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 200, 100});
    out.doc->update(0.0f);
    return out;
}

float opacityOf(Document& d, std::string_view id) { return d.byId(id).property(prop::Opacity).f; }

}  // namespace

TEST_CASE("@timeline: parses name, markers, tracks, per-key easing, sound and ms times") {
    DiagnosticSink sink;
    RuleSet rs;
    REQUIRE(parseXts(kIntroStyle, "t.xts", 1, sink, rs));
    REQUIRE(rs.timelines.size() == 1);
    const TimelineDef& tl = rs.timelines[0];
    CHECK(tl.name == "intro");
    CHECK(tl.duration < 0);
    REQUIRE(tl.markers.size() == 1);
    CHECK(tl.markers[0].first == "ready");
    CHECK(tl.markers[0].second == doctest::Approx(1.0f));
    REQUIRE(tl.tracks.size() == 2);
    CHECK(tl.tracks[0].path == "A");
    REQUIRE(tl.tracks[0].keys.size() == 2);
    CHECK_FALSE(tl.tracks[0].keys[0].hasEasing);
    CHECK(tl.tracks[0].keys[1].hasEasing);
    CHECK(tl.tracks[0].keys[1].easing.kind == EasingKind::CubicBezier);
    CHECK(tl.tracks[0].keys[1].time == doctest::Approx(0.4f));
    CHECK(tl.tracks[1].keys[1].time == doctest::Approx(0.7f));  // 700ms
    REQUIRE(tl.sounds.size() == 1);
    CHECK(tl.sounds[0].first == doctest::Approx(0.4f));
    CHECK(tl.sounds[0].second == "ui_whoosh");
}

TEST_CASE("@timeline: path ids, effect addresses, transform expansion, duration and from") {
    DiagnosticSink sink;
    RuleSet rs;
    REQUIRE(parseXts(R"(
      @timeline t {
        duration: 1.2s;
        #Play/Bg { 0s { transform: translateY(20px); effect.glow.intensity: 0 } 0.5s { effect.0.radius: 4px } }
        #X { 0s { effect.12.color: #ff0000 } }
      }
      @timeline main from "ui.xtanim.json#main";
    )",
                     "t.xts", 1, sink, rs));
    REQUIRE(rs.timelines.size() == 2);
    const TimelineDef& t = rs.timelines[0];
    CHECK(t.duration == doctest::Approx(1.2f));
    REQUIRE(t.tracks.size() == 2);
    CHECK(t.tracks[0].path == "Play/Bg");
    const TimelineKey& k0 = t.tracks[0].keys[0];
    bool sawTranslateY = false;
    for (const StyleDecl& d : k0.decls) {
        sawTranslateY = sawTranslateY || d.prop == prop::TranslateY;
    }
    CHECK(sawTranslateY);
    REQUIRE(k0.effectDecls.size() == 1);
    CHECK(k0.effectDecls[0].effect == "glow");
    CHECK(k0.effectDecls[0].param == "intensity");
    CHECK(t.tracks[0].keys[1].effectDecls[0].effect == "0");
    // T-534 (18.13): the value stays source text; the emitter parses it by the parameter's type.
    CHECK(k0.effectDecls[0].valueText == "0");
    CHECK(t.tracks[0].keys[1].effectDecls[0].valueText == "4px");
    CHECK(t.tracks[1].keys[0].effectDecls[0].effect == "12");
    CHECK(t.tracks[1].keys[0].effectDecls[0].valueText == "#ff0000");
    CHECK(rs.timelines[1].fromFile == "ui.xtanim.json");
    CHECK(rs.timelines[1].fromName == "main");
}

TEST_CASE("@timeline: selector other than #id, bad time and duplicate element/prop/time are errors") {
    {
        DiagnosticSink sink;
        RuleSet rs;
        CHECK_FALSE(parseXts("@timeline t { .card { 0s { opacity: 0 } } }", "t.xts", 1, sink, rs));
        CHECK(rs.timelines.empty());
    }
    {
        DiagnosticSink sink;
        RuleSet rs;
        CHECK_FALSE(parseXts("@timeline t { #A { 5 { opacity: 0 } } }", "t.xts", 1, sink, rs));
    }
    std::string xt = doc("@timeline t { #A { 0s { opacity: 0 } } #A { 0ms { opacity: 1 } } }");
    CompileResult r = compileStrings({{"main.xt", xt}});
    CHECK_FALSE(r.ok());
    CHECK(hasError(r, "same property twice"));
    CompileResult missing = compileStrings({{"main.xt", doc("@timeline t { #Nope { 0s { opacity: 0 } } }")}});
    CHECK(hasError(missing, "no element with id \"Nope\""));
}

TEST_CASE("@timeline: compiles to normalized per-track keyframes and a TGRP group") {
    CompileResult r = compileStrings({{"main.xt", doc(kIntroStyle)}});
    REQUIRE(r.ok());
    Result<IrView> opened = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(opened.ok());
    const IrView& ir = opened.value();
    const TimelineGroupChunk& tg = ir.timelineGroups();
    REQUIRE(tg.present());
    REQUIRE(tg.groups().size() == 1);
    const IrTimelineGroup& g = tg.groups()[0];
    CHECK(ir.string(g.name) == "intro");
    CHECK(g.scopeElement == 0);
    // No `duration`: the last key (0.7 s) extended to the last marker (1.0 s), see the report.
    CHECK(g.duration == doctest::Approx(1.0f));
    REQUIRE(g.markerCount == 1);
    CHECK(ir.string(tg.markers()[g.markerOffset].name) == "ready");
    REQUIRE(g.soundCount == 1);
    CHECK(ir.string(tg.sounds()[g.soundOffset].cue) == "ui_whoosh");
    REQUIRE(g.memberCount == 2);
    const IrTimelineGroupMember& a = tg.members()[g.memberOffset];
    const IrTimelineGroupMember& b = tg.members()[g.memberOffset + 1];
    CHECK(a.delay == doctest::Approx(0.0f));
    CHECK(a.duration == doctest::Approx(0.4f));
    CHECK(b.delay == doctest::Approx(0.3f));
    CHECK(b.duration == doctest::Approx(0.4f));
    const TimelineChunk& tlA = ir.timeline(a.timeline);
    CHECK(ir.string(tlA.name()) == "intro__A");
    CHECK(tlA.duration() == doctest::Approx(1.0f));
    REQUIRE(tlA.tracks().size() == 1);
    CHECK(tlA.tracks()[0].prop == prop::Opacity);
    std::span<const IrKeyframe> kfs =
        tlA.keyframes().subspan(tlA.tracks()[0].kfOffset, tlA.tracks()[0].kfCount);
    REQUIRE(kfs.size() == 2);
    CHECK(kfs[0].time == doctest::Approx(0.0f));
    CHECK(kfs[1].time == doctest::Approx(1.0f));
    // The `.4s ease-out` arrival easing sits on the segment leaving the 0 s key.
    CHECK(kfs[0].easing.kind == static_cast<uint8_t>(EasingKind::CubicBezier));
    CHECK(kfs[0].easing.x2 == doctest::Approx(0.58f));
    // Also reachable by name, like any @keyframes.
    bool found = false;
    for (const IrKeyframesRef& ref : ir.style().keyframesRefs()) {
        found = found || ir.string(ref.name) == "intro__B";
    }
    CHECK(found);
}

TEST_CASE("TimelineGroup: shared clock, marker via seek, end at 1.0 s, events") {
    Loaded l = load(doc(kIntroStyle));
    Document& d = *l.doc;
    CHECK(opacityOf(d, "A") == doctest::Approx(1.0f));  // Idle: styled values.

    std::vector<std::string> events;
    d.setTimelineListener([&](const TimelineEvent& e) {
        events.push_back(std::string(e.kind == TimelineEventKind::End ? "end:" : "marker:") +
                         std::string(e.timeline) + (e.marker.empty() ? "" : ":" + std::string(e.marker)));
    });
    int bubbledEnd = 0;
    d.addListener(d.root(), EventType::TimelineEnd, [&](Event& ev) {
        CHECK(d.string(ev.timelineName) == "intro");
        ++bubbledEnd;
    });

    CHECK(d.playTimeline("", "nope") == Status::kNotFound);
    REQUIRE(d.playTimeline("", "intro") == Status::kOk);
    d.update(0.2f);
    float a = opacityOf(d, "A");
    CHECK(a > 0.05f);
    CHECK(a < 0.95f);                                   // Half transparent (ease-out at 0.5).
    CHECK(opacityOf(d, "B") == doctest::Approx(0.0f));  // Before its 0.3 s start: first key.
    CHECK(events.empty());

    REQUIRE(d.seekTimeline("", "intro", 1.0f) == Status::kOk);
    d.update(0.2f);
    REQUIRE(events.size() == 2);
    CHECK(events[0] == "marker:intro:ready");
    CHECK(events[1] == "end:intro");
    CHECK(bubbledEnd == 1);
    CHECK_FALSE(d.isTimelinePlaying("", "intro"));
    CHECK(opacityOf(d, "A") == doctest::Approx(1.0f));
    CHECK(opacityOf(d, "B") == doctest::Approx(1.0f));

    // Natural playback: the end fires exactly when the clock reaches 1.0 s.
    events.clear();
    REQUIRE(d.playTimeline("", "intro") == Status::kOk);
    d.update(1.19f);
    CHECK(events.empty());
    CHECK(d.timelineTime("", "intro") == doctest::Approx(0.99f));
    d.update(1.2f);
    REQUIRE(events.size() == 2);
    CHECK(events[0] == "marker:intro:ready");
    CHECK(events[1] == "end:intro");
}

TEST_CASE("TimelineGroup: reverse, hold, loop, fromMarker and stop") {
    Loaded l = load(doc(kIntroStyle));
    Document& d = *l.doc;
    int ends = 0, markers = 0;
    d.setTimelineListener([&](const TimelineEvent& e) {
        if (e.kind == TimelineEventKind::End) {
            ++ends;
        } else {
            ++markers;
        }
    });

    SUBCASE("reverse + hold stays on the first frame") {
        PlayOptions opts;
        opts.reverse = true;
        opts.hold = true;
        REQUIRE(d.playTimeline("", "intro", opts) == Status::kOk);
        d.update(0.5f);  // t = 0.5: A done (1), B half way back.
        CHECK(opacityOf(d, "A") == doctest::Approx(1.0f));
        CHECK(opacityOf(d, "B") == doctest::Approx(0.5f));
        d.update(1.5f);
        CHECK(ends == 1);
        CHECK(d.timelineTime("", "intro") == doctest::Approx(0.0f));
        CHECK(opacityOf(d, "A") == doctest::Approx(0.0f));
        CHECK(opacityOf(d, "B") == doctest::Approx(0.0f));
        d.update(2.0f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.0f));  // Held.
        REQUIRE(d.stopTimeline("", "intro") == Status::kOk);
        d.update(2.1f);
        CHECK(opacityOf(d, "A") == doctest::Approx(1.0f));  // Released: styled value.
        CHECK(ends == 1);                                   // stop fires no End.
    }
    SUBCASE("without hold the members are released at the end") {
        PlayOptions opts;
        opts.reverse = true;
        REQUIRE(d.playTimeline("", "intro", opts) == Status::kOk);
        d.update(1.5f);
        CHECK(ends == 1);
        CHECK(opacityOf(d, "A") == doctest::Approx(1.0f));
    }
    SUBCASE("loop runs two laps without an End") {
        PlayOptions opts;
        opts.loop = true;
        REQUIRE(d.playTimeline("", "intro", opts) == Status::kOk);
        for (int i = 1; i <= 25; ++i) {
            d.update(0.1f * static_cast<float>(i));  // 2.5 s = two full laps + half.
        }
        CHECK(ends == 0);
        CHECK(markers == 2);
        CHECK(d.isTimelinePlaying("", "intro"));
        CHECK(d.timelineTime("", "intro") == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(opacityOf(d, "B") == doctest::Approx(0.5f).epsilon(0.02));
    }
    SUBCASE("fromMarker starts at the marker and rate scales the clock") {
        PlayOptions opts;
        opts.fromMarker = "ready";
        opts.reverse = true;
        opts.rate = 2;
        REQUIRE(d.playTimeline("", "intro", opts) == Status::kOk);
        d.update(0.25f);  // 0.5 s of group time backwards from 1.0.
        CHECK(d.timelineTime("", "intro") == doctest::Approx(0.5f));
        CHECK(markers == 0);
        opts.fromMarker = "missing";
        CHECK(d.playTimeline("", "intro", opts) == Status::kNotFound);
        PlayOptions bad;
        bad.rate = 0;
        CHECK(d.playTimeline("", "intro", bad) == Status::kInvalidArgument);
    }
}

TEST_CASE("TimelineGroup: an effect parameter track drives an existing effect") {
    std::string xt = doc(R"(
      #Card { position: absolute; left: 0px; top: 0px; width: 40px; height: 40px;
              background-color: #336699; effect: shadow(0px 2px 4px #000000) glow(8px #ffffff 0.2); }
      @timeline pulse { #Card { 0s { effect.glow.intensity: 0 } .5s ease-in-out { effect.glow.intensity: 1 } } }
    )",
                         "<div id=\"Card\"></div>");
    Loaded l = load(xt);
    Document& d = *l.doc;
    ElementRef card = d.byId("Card");
    REQUIRE(card.effectCount() == 2);
    CHECK(card.effect(1).params[1] == doctest::Approx(0.2f));
    PlayOptions hold;
    hold.hold = true;
    REQUIRE(d.playTimeline("", "pulse", hold) == Status::kOk);
    d.update(0.25f);
    CHECK(card.effect(1).params[1] == doctest::Approx(0.5f));  // ease-in-out is symmetric.
    CHECK(card.effect(0).params[2] == doctest::Approx(4.0f));  // The shadow is untouched.
    d.update(0.6f);
    CHECK(card.effect(1).params[1] == doctest::Approx(1.0f));  // Held on the last key.
    REQUIRE(d.stopTimeline("", "pulse") == Status::kOk);       // Released: the styled intensity.
    CHECK(card.effect(1).params[1] == doctest::Approx(0.2f));
    d.update(0.7f);
    CHECK(card.effect(1).params[1] == doctest::Approx(0.2f));

    CompileResult bad = compileStrings(
        {{"main.xt",
          doc("#Card { effect: blur(2px); } @timeline t { #Card { 0s { effect.glow.intensity: 0 } } }",
              "<div id=\"Card\"></div>")}});
    CHECK(hasError(bad, "no such effect"));
}

TEST_CASE("TimelineGroup: the group wins over the element's own animation on shared properties") {
    std::string xt = doc(R"(
      @keyframes dim { from { opacity: 0.5 } to { opacity: 0.5 } }
      #A { animation: dim 10s linear; }
      @timeline show { #A { 0s { opacity: 1 } 1s { opacity: 0 } } }
    )",
                         "<div id=\"A\"></div>");
    Loaded l = load(xt);
    Document& d = *l.doc;
    d.update(0.1f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
    REQUIRE(d.playTimeline("", "show") == Status::kOk);
    d.update(0.35f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.75f));
}

TEST_CASE("TimelineGroup: .xtanim.json via `from` is equivalent to the inline timeline") {
    constexpr const char* kJson = R"({
      "v": 1,
      "extra": true,
      "timelines": [
        { "name": "main", "duration": 1.0, "markers": [[1.0, "ready"]],
          "tracks": [
            { "layer": "A", "prop": "opacity", "keys": [[0, 0], [0.4, 1, "ease-out"]] },
            { "layer": "B", "prop": "opacity", "keys": [[0.3, 0, null], [0.7, 1]] }
          ] }
      ] })";
    std::string fromDoc =
        doc("#A, #B { position: absolute; width: 20px; height: 20px; } "
            "@timeline intro from \"x.xtanim.json#main\";");
    std::string inlineDoc = doc(R"(#A, #B { position: absolute; width: 20px; height: 20px; }
      @timeline intro { duration: 1s; marker: ready 1s;
        #A { 0s { opacity: 0 } .4s ease-out { opacity: 1 } } #B { .3s { opacity: 0 } .7s { opacity: 1 } } })");
    Loaded viaFile = load(fromDoc, {{"x.xtanim.json", kJson}});
    bool warned = false;
    for (const Diagnostic& d : viaFile.result.diagnostics) {
        warned = warned ||
                 (d.level == Diagnostic::Level::Warning && d.message.find("\"extra\"") != std::string::npos);
    }
    CHECK(warned);
    Loaded viaInline = load(inlineDoc);

    // Same TGRP and the same member keyframes.
    Result<IrView> a = IrView::open(IrBlob{viaFile.result.ir.data(), viaFile.result.ir.size()});
    Result<IrView> b = IrView::open(IrBlob{viaInline.result.ir.data(), viaInline.result.ir.size()});
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    const TimelineGroupChunk& ga = a.value().timelineGroups();
    const TimelineGroupChunk& gb = b.value().timelineGroups();
    REQUIRE(ga.groups().size() == 1);
    REQUIRE(gb.groups().size() == 1);
    CHECK(ga.groups()[0].duration == gb.groups()[0].duration);
    REQUIRE(ga.members().size() == gb.members().size());
    for (size_t i = 0; i < ga.members().size(); ++i) {
        CHECK(ga.members()[i].delay == gb.members()[i].delay);
        CHECK(ga.members()[i].duration == gb.members()[i].duration);
        const TimelineChunk& ta = a.value().timeline(ga.members()[i].timeline);
        const TimelineChunk& tb = b.value().timeline(gb.members()[i].timeline);
        REQUIRE(ta.keyframes().size() == tb.keyframes().size());
        for (size_t k = 0; k < ta.keyframes().size(); ++k) {
            CHECK(ta.keyframes()[k].time == tb.keyframes()[k].time);
            CHECK(ta.keyframes()[k].easing.kind == tb.keyframes()[k].easing.kind);
            CHECK(std::memcmp(ta.keyframes()[k].value, tb.keyframes()[k].value, 16) == 0);
        }
    }
    for (Loaded* l : {&viaFile, &viaInline}) {
        REQUIRE(l->doc->playTimeline("", "intro") == Status::kOk);
        l->doc->update(0.5f);
    }
    CHECK(opacityOf(*viaFile.doc, "B") == doctest::Approx(opacityOf(*viaInline.doc, "B")));
    CHECK(opacityOf(*viaFile.doc, "B") == doctest::Approx(0.5f));

    CompileResult missing = compileStrings(
        {{"main.xt", doc("@timeline intro from \"x.xtanim.json#nope\";")}, {"x.xtanim.json", kJson}});
    CHECK(hasError(missing, "has no timeline \"nope\""));
}

TEST_CASE("TimelineGroup: <anim sync> follows the group's seek") {
    std::string xt = doc("@timeline t { duration: 2s; }",
                         "<anim id=\"clip\" src=\"min_solid_anim.json\" sync=\"true\"></anim>");
    Loaded l = load(xt);
    Document& d = *l.doc;
    ElementRef clip = d.byId("clip");
    uint32_t inst = d.defaultAnimation(clip);
    REQUIRE(inst != 0);
    d.update(0.1f);
    REQUIRE(d.seekTimeline("", "t", 1.5f) == Status::kOk);
    d.update(0.2f);
    CHECK(d.animationTime(inst) == doctest::Approx(1.5f));
    REQUIRE(d.seekTimeline("", "t", 0.25f) == Status::kOk);
    d.update(0.3f);
    CHECK(d.animationTime(inst) == doctest::Approx(0.25f));
    d.update(0.4f);  // Paused while the group drives it: no drift.
    CHECK(d.animationTime(inst) == doctest::Approx(0.25f));
    REQUIRE(d.stopTimeline("", "t") == Status::kOk);  // Released: the clip resumes on its own clock.
    d.update(0.6f);
    CHECK(d.animationTime(inst) == doctest::Approx(0.45f));
}

TEST_CASE("Context: playTimeline / seekTimeline / timeline listeners") {
    CompileResult r = compileStrings({{"main.xt", doc(kIntroStyle)}});
    REQUIRE(r.ok());
    Context ctx(ContextConfig{});
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, true);
    REQUIRE(h.ok());
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    ctx.update(0.0f);
    std::vector<TimelineEventKind> kinds;
    uint32_t id = ctx.addTimelineListener([&](const TimelineEvent& e) { kinds.push_back(e.kind); });
    CHECK(ctx.playTimeline("", "missing") == Status::kNotFound);
    REQUIRE(ctx.playTimeline("", "intro") == Status::kOk);
    ctx.update(0.5f);
    REQUIRE(ctx.seekTimeline("", "intro", 1.0f) == Status::kOk);
    ctx.update(0.0f);
    REQUIRE(kinds.size() == 2);
    CHECK(kinds[0] == TimelineEventKind::Marker);
    CHECK(kinds[1] == TimelineEventKind::End);
    ctx.removeTimelineListener(id);
    REQUIRE(ctx.playTimeline("", "intro") == Status::kOk);
    ctx.update(2.0f);
    CHECK(kinds.size() == 2);
    CHECK(ctx.stopTimeline("", "intro") == Status::kOk);
}

TEST_CASE("TGRP: IrWriter::fromView round-trips and a bad member is rejected") {
    CompileResult r = compileStrings({{"main.xt", doc(kIntroStyle)}});
    REQUIRE(r.ok());
    Result<IrView> v = IrView::open(IrBlob{r.ir.data(), r.ir.size()});
    REQUIRE(v.ok());
    std::vector<uint8_t> again = IrWriter::fromView(v.value()).finish();
    CHECK(again == r.ir);

    IrWriter w = IrWriter::fromView(v.value());
    TimelineGroupBuilder gb;
    gb.groups.push_back(IrTimelineGroup{0, 0, 1.0f, 0, 0, 0, 0, 0, 1});
    gb.members.push_back(IrTimelineGroupMember{0, 999, 0.0f, 1.0f});  // No TLIN 999.
    w.setTimelineGroups(std::move(gb));
    std::vector<uint8_t> broken = w.finish();
    CHECK_FALSE(IrView::open(IrBlob{broken.data(), broken.size()}).ok());
}

TEST_CASE("TimelineGroup: a path-id track targets an element inside a symbol instance (T-171)") {
    std::string xt = R"(<xt><head>
      <symbol name="Button"><div id="Bg"></div></symbol>
      <style>
        @timeline intro { #Play/Bg { 0s { opacity: 0 } 1s { opacity: 1 } } }
      </style></head>
      <body><use symbol="Button" id="Play"/></body></xt>)";
    Loaded l = load(xt);
    Document& d = *l.doc;
    REQUIRE(d.byId("Play/Bg").valid());
    REQUIRE(d.playTimeline("", "intro") == Status::kOk);
    d.update(0.5f);
    CHECK(opacityOf(d, "Play/Bg") == doctest::Approx(0.5f));
}

TEST_CASE("TimelineGroup: T-262c an .xtanim.json nesting 1100 levels is an error and the process lives") {
    // A valid timeline plus an unknown key `arrays` arrays deep; the root object is level 1.
    const auto xtanim = [](int arrays) {
        return std::string(R"({"v": 1, "timelines": [{"name": "main", "duration": 1,)"
                           R"( "tracks": [{"layer": "A", "prop": "opacity", "keys": [[0, 0], [1, 1]]}]}],)"
                           R"( "deep": )") +
               std::string(static_cast<size_t>(arrays), '[') + std::string(static_cast<size_t>(arrays), ']') +
               "}";
    };
    const std::string fromDoc =
        doc("#A, #B { position: absolute; width: 20px; height: 20px; } "
            "@timeline intro from \"x.xtanim.json#main\";");

    const CompileResult deep = compileStrings({{"main.xt", fromDoc}, {"x.xtanim.json", xtanim(1100)}});
    CHECK_FALSE(deep.ok());
    CHECK(hasError(deep, "invalid JSON: nesting too deep"));

    // 32 levels: compiles (with the unknown-field warning for "deep").
    const CompileResult bounded = compileStrings({{"main.xt", fromDoc}, {"x.xtanim.json", xtanim(31)}});
    for (const Diagnostic& d : bounded.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    CHECK(bounded.ok());
}

// -- T-774 (58 §58.6 item 2): a timeline's own marker / end pausing, seeking or replaying it ---------

namespace {

/// `#A` opacity runs 0 -> 1 linearly over the timeline `x` (2 s by default: opacity = t / 2).
std::string selfDoc(std::string_view markers, std::string_view triggers, std::string_view duration = "2s") {
    return doc(
        "#A, #B { position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; }"
        "@timeline x { duration: " +
            std::string(duration) + "; marker: " + std::string(markers) + "; #A { 0s { opacity: 0 } " +
            std::string(duration) + " { opacity: 1 } } }",
        "<div id=\"A\" " + std::string(triggers) + "></div><div id=\"B\"></div>");
}

}  // namespace

TEST_CASE("T-774: pause() from the timeline's own marker stops at the marker time, not past it") {
    Loaded l = load(selfDoc("m 1s", "on:timeline(x).marker(m)=\"pause(x)\""));
    Document& d = *l.doc;
    SUBCASE("one step of 0.1 s from 0.95 s") {
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        REQUIRE(d.seekTimeline("", "x", 0.95f) == Status::kOk);
        d.update(0.1f);
        MESSAGE("t=" << d.timelineTime("", "x") << " opacity=" << opacityOf(d, "A"));
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == 1.0f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
        d.update(0.3f);
        d.update(0.7f);
        CHECK(d.timelineTime("", "x") == 1.0f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
    }
    SUBCASE("uneven frame steps") {
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        for (float clock : {0.31f, 0.68f, 0.97f, 1.13f}) {
            d.update(clock);
        }
        MESSAGE("t=" << d.timelineTime("", "x") << " opacity=" << opacityOf(d, "A"));
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == 1.0f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
    }
}

TEST_CASE("T-774: seek() / play(from) from the own marker continue with the rest of the step") {
    SUBCASE("play(x, from=back)") {
        Loaded l = load(selfDoc("m 1s, back 0.25s", "on:timeline(x).marker(m)=\"play(x, from=back)\""));
        Document& d = *l.doc;
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        REQUIRE(d.seekTimeline("", "x", 0.95f) == Status::kOk);
        d.update(0.1f);
        CHECK(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.30f).epsilon(1e-5));
        CHECK(opacityOf(d, "A") == doctest::Approx(0.15f).epsilon(1e-4));
        d.update(0.2f);  // Continuous: 0.1 s later.
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.40f).epsilon(1e-5));
    }
    SUBCASE("seek(x, back)") {
        Loaded l = load(selfDoc("m 1s, back 0.25s", "on:timeline(x).marker(m)=\"seek(x, back)\""));
        Document& d = *l.doc;
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        REQUIRE(d.seekTimeline("", "x", 0.95f) == Status::kOk);
        d.update(0.1f);
        CHECK(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.30f).epsilon(1e-5));
        CHECK(opacityOf(d, "A") == doctest::Approx(0.15f).epsilon(1e-4));
    }
    SUBCASE("rate 2: the rest of the step is clock time, replayed at the new play's rate 1") {
        Loaded l = load(selfDoc("m 0.5s, back 0.1s", "on:timeline(x).marker(m)=\"play(x, from=back)\""));
        Document& d = *l.doc;
        PlayOptions options;
        options.rate = 2.0f;
        REQUIRE(d.playTimeline("", "x", options) == Status::kOk);
        d.update(0.3f);  // 0.25 s of clock reach the marker; 0.05 s are left.
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.15f).epsilon(1e-5));
    }
}

TEST_CASE("T-774: the end replaying the own loop marker loops seamlessly") {
    const std::string xt = selfDoc("loop 0.5s", "on:timeline(x).end=\"play(x, from=loop)\"", "1s");
    SUBCASE("1/60 s frames: the time never drifts from the clock") {
        Loaded l = load(xt);
        Document& d = *l.doc;
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        for (int frame = 1; frame <= 240; ++frame) {
            d.update(static_cast<float>(frame) / 60.0f);
            const double elapsed = frame / 60.0;
            const double expected = elapsed <= 1.0 ? elapsed : 0.5 + std::fmod(elapsed - 1.0, 0.5);
            const double diff = std::fmod(std::fabs(d.timelineTime("", "x") - expected), 0.5);
            CAPTURE(frame);
            CHECK(std::min(diff, 0.5 - diff) < 1e-3);
            CHECK(d.isTimelinePlaying("", "x"));
        }
    }
    SUBCASE("uneven frames: the rest of the end step continues from the loop marker") {
        Loaded l = load(xt);
        Document& d = *l.doc;
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        d.update(0.9f);
        d.update(1.2f);  // 0.2 s past the end -> 0.7.
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.7f).epsilon(1e-5));
        CHECK(opacityOf(d, "A") == doctest::Approx(0.7f).epsilon(1e-4));
        d.update(1.5f);  // Exactly the end -> the loop marker.
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.5f).epsilon(1e-5));
    }
    SUBCASE("three rounds show the same values") {
        Loaded l = load(xt);
        Document& d = *l.doc;
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        std::vector<float> values;
        for (int frame = 1; frame <= 8 + 12; ++frame) {
            d.update(static_cast<float>(frame) * 0.125f);
            if (frame > 8) {
                values.push_back(opacityOf(d, "A"));
            }
        }
        for (size_t i = 0; i < 4; ++i) {
            CAPTURE(i);
            CHECK(values[i] == doctest::Approx(values[i + 4]));
            CHECK(values[i] == doctest::Approx(values[i + 8]));
        }
    }
}

TEST_CASE("T-774: several marks in one step are handled in time order") {
    SUBCASE("a jump lands before a pause further on") {
        // Declared out of time order on purpose.
        Loaded l = load(selfDoc("r 0.75s, q 0.7s, p 0.4s",
                                "on:timeline(x).marker(p)=\"play(x, from=q)\" "
                                "on:timeline(x).marker(r)=\"pause(x)\""));
        Document& d = *l.doc;
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        d.update(0.5f);  // 0.4 -> 0.7, then 0.1 more crosses r at 0.75.
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == 0.75f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.375f).epsilon(1e-4));
    }
    SUBCASE("markers past a pause do not fire; ones before it do") {
        Loaded l = load(selfDoc("b 0.8s, m 0.5s, a 0.3s",
                                "on:timeline(x).marker(a)=\"class(a)\" on:timeline(x).marker(m)=\"pause(x)\" "
                                "on:timeline(x).marker(b)=\"class(b)\""));
        Document& d = *l.doc;
        std::vector<std::string> events;
        d.setTimelineListener([&](const TimelineEvent& e) { events.emplace_back(e.marker); });
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        d.update(1.0f);
        CHECK(d.timelineTime("", "x") == 0.5f);
        CHECK(d.byId("A").hasClass("a"));
        CHECK_FALSE(d.byId("A").hasClass("b"));
        // Both fire (in the IR's marker order, as before); `b` lies past the pause.
        REQUIRE(events.size() == 2);
        CHECK(std::find(events.begin(), events.end(), "a") != events.end());
        CHECK(std::find(events.begin(), events.end(), "m") != events.end());
    }
    SUBCASE("looping playback pauses at a marker after the wrap") {
        Loaded l = load(selfDoc("m 0.25s", "on:timeline(x).marker(m)=\"pause(x)\"", "1s"));
        Document& d = *l.doc;
        PlayOptions options;
        options.loop = true;
        REQUIRE(d.playTimeline("", "x", options) == Status::kOk);
        REQUIRE(d.seekTimeline("", "x", 0.9f) == Status::kOk);
        d.update(0.5f);
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == 0.25f);
    }
    SUBCASE("reverse playback pauses at the marker") {
        Loaded l = load(selfDoc("m 0.5s", "on:timeline(x).marker(m)=\"pause(x)\"", "1s"));
        Document& d = *l.doc;
        PlayOptions options;
        options.reverse = true;
        REQUIRE(d.playTimeline("", "x", options) == Status::kOk);
        d.update(0.6f);
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == 0.5f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
    }
    SUBCASE("stop() at the own marker: nothing after it fires, no end") {
        Loaded l = load(selfDoc("m 0.5s, b 0.8s", "on:timeline(x).marker(m)=\"stop(x)\"", "1s"));
        Document& d = *l.doc;
        std::vector<std::string> events;
        d.setTimelineListener([&](const TimelineEvent& e) {
            events.emplace_back(e.kind == TimelineEventKind::End ? "end" : std::string(e.marker));
        });
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        d.update(1.5f);
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        REQUIRE(events.size() == 1);
        CHECK(events[0] == "m");
        CHECK(opacityOf(d, "A") == doctest::Approx(1.0f));  // Released: the styled value.
    }
}

TEST_CASE("T-774: the trigger's other actions still run after the frame's evaluation, in order") {
    Loaded l = load(selfDoc("m 1s",
                            "on:timeline(x).marker(m)=\"class(k); pause(x) then class(after)\" "
                            "on:timeline(x).end=\"class(ended)\""));
    Document& d = *l.doc;
    std::vector<std::string> order;
    d.setTimelineListener([&](const TimelineEvent& e) {
        order.push_back(std::string(e.marker) + (d.byId("A").hasClass("k") ? "+k" : "-k"));
    });
    REQUIRE(d.playTimeline("", "x") == Status::kOk);
    d.update(1.3f);
    CHECK(d.timelineTime("", "x") == 1.0f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
    CHECK(d.byId("A").hasClass("k"));
    CHECK(d.byId("A").hasClass("after"));
    CHECK_FALSE(d.byId("A").hasClass("ended"));
    REQUIRE(order.size() == 1);
    CHECK(order[0] == "m+k");  // The trigger ran before the listener, as before.
    d.update(1.6f);
    CHECK(d.timelineTime("", "x") == 1.0f);
}

// A member paused between two keys is still evaluated every frame (T-717's static hold covers a
// member paused at its end stop only), the same as a group paused by seekTimeline(); not T-774's.
TEST_CASE("T-774: paused at its own marker under the static-skip verify mode: it stays at the marker") {
    const std::string xt = selfDoc("m 0.1s", "on:timeline(x).marker(m)=\"pause(x)\"");
    CompileResult compiled = compileStrings({{"main.xt", xt}});
    REQUIRE(compiled.ok());
    ContextConfig config;
    Context ctx(config);  // xt_tests: the static-skip verify mode is on.
    ctx.perf().setEnabled(true);
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{compiled.ir.data(), compiled.ir.size()}, false);
    REQUIRE(handle.ok());
    Document& d = *ctx.document(handle.value());
    CommandList list;
    REQUIRE(d.playTimeline("", "x") == Status::kOk);
    for (int frame = 0; frame < 6; ++frame) {  // 0.15 s at 1/40 s: the marker is passed at 0.1.
        ctx.update(1.0f / 40.0f);
        ctx.render(list, nullptr);
    }
    CHECK_FALSE(d.isTimelinePlaying("", "x"));
    CHECK(d.timelineTime("", "x") == 0.1f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.05f));
    for (int frame = 0; frame < 4; ++frame) {
        ctx.update(1.0f / 40.0f);
        ctx.render(list, nullptr);
        CAPTURE(frame);
        CHECK(ctx.perf().current().staticSkipMismatch == 0);
        CHECK(d.timelineTime("", "x") == 0.1f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.05f));
    }
}

// -- T-783: the self-control cases T-774 left on the old timing ------------------------------------

TEST_CASE("T-783: an if-guarded pause / seek at the own marker is decided at the marker's time") {
    const std::string triggers =
        "port:halt=\"number\" on:timeline(x).marker(m)=\"if port(halt) eq 1 pause(x) else seek(x, back)\"";
    Loaded l = load(selfDoc("m 1s, back 0.25s", triggers));
    Document& d = *l.doc;
    const PortId halt = d.lookupPort("halt");
    REQUIRE(halt != 0);
    SUBCASE("the condition holds: paused at the marker") {
        REQUIRE(d.setPort(halt, Value::ofFloat(1.0f)) == Status::kOk);
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        REQUIRE(d.seekTimeline("", "x", 0.95f) == Status::kOk);
        d.update(0.1f);
        CHECK_FALSE(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == 1.0f);
        CHECK(opacityOf(d, "A") == doctest::Approx(0.5f));
        d.update(0.3f);
        CHECK(d.timelineTime("", "x") == 1.0f);
    }
    SUBCASE("it does not: the else branch's seek continues with the rest of the step") {
        REQUIRE(d.setPort(halt, Value::ofFloat(0.0f)) == Status::kOk);
        REQUIRE(d.playTimeline("", "x") == Status::kOk);
        REQUIRE(d.seekTimeline("", "x", 0.95f) == Status::kOk);
        d.update(0.1f);
        CHECK(d.isTimelinePlaying("", "x"));
        CHECK(d.timelineTime("", "x") == doctest::Approx(0.30f).epsilon(1e-5));
        CHECK(opacityOf(d, "A") == doctest::Approx(0.15f).epsilon(1e-4));
    }
}

TEST_CASE("T-783: an if-guarded pause whose condition fails plays on through the marker") {
    Loaded l = load(
        selfDoc("m 1s", "port:halt=\"number\" on:timeline(x).marker(m)=\"if port(halt) eq 1 pause(x)\""));
    Document& d = *l.doc;
    REQUIRE(d.playTimeline("", "x") == Status::kOk);
    REQUIRE(d.seekTimeline("", "x", 0.95f) == Status::kOk);
    d.update(0.1f);
    CHECK(d.isTimelinePlaying("", "x"));
    CHECK(d.timelineTime("", "x") == doctest::Approx(1.05f).epsilon(1e-5));
}

TEST_CASE("T-783: a seek from the own marker over another self-control marker applies it where it lands") {
    Loaded l = load(selfDoc("m 0.5s, p 1s, z 1.5s",
                            "on:timeline(x).marker(m)=\"seek(x, z)\" on:timeline(x).marker(p)=\"pause(x)\""));
    Document& d = *l.doc;
    std::vector<std::string> events;
    d.setTimelineListener([&](const TimelineEvent& e) { events.emplace_back(e.marker); });
    REQUIRE(d.playTimeline("", "x") == Status::kOk);
    d.update(0.6f);  // m at 0.5 jumps to z (1.5) over p: paused there, not 0.1 s further.
    CHECK_FALSE(d.isTimelinePlaying("", "x"));
    CHECK(d.timelineTime("", "x") == 1.5f);
    CHECK(opacityOf(d, "A") == doctest::Approx(0.75f));
    CHECK(std::count(events.begin(), events.end(), "m") == 1);
    CHECK(std::count(events.begin(), events.end(), "p") == 1);
    d.update(0.9f);
    CHECK(d.timelineTime("", "x") == 1.5f);
}
