#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "doctest.h"
#include "fake_text_shaper.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/sw/sw_png.h"

/// External content layers: `<video>`, `<sequence>`, `<external>`, IExternalTexture, host requests,
/// `media(<id>).end` and `sync` clips (docs/design/25-editor-blueprint.md §25.3.5b, 03 §3.12.1,
/// 05 §5.9.1, 13 §13.8.3, 04 §4.10.2, 28 §28.2, docs/tasks/T-191.md). Frames live in
/// tests/xt/media: seq/frame_0000..0003.png (8x4, red/green/blue/white) and atlas.png (16x8, the
/// same four colors as 2x2 cells of 8x4).

using namespace xt;
using namespace xt::compile;

namespace {

/// Counts imageView() calls per asset so the preload window can be checked.
class CountingResolver : public FileAssetResolver {
public:
    CountingResolver() : FileAssetResolver(std::string(XT_TEST_XT_DIR), sw::decodeImageBytes) {}
    const ImageData* imageView(AssetId asset, const IrView& view) override {
        views.push_back(asset);
        return FileAssetResolver::imageView(asset, view);
    }
    std::vector<AssetId> views;
};

std::string doc(std::string_view body, std::string_view style = "") {
    return "<xt><head><style>" + std::string(style) + "</style></head><body id=\"body\">" +
           std::string(body) + "</body></xt>";
}

CompileResult compileXt(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    options.rootDir = XT_TEST_XT_DIR;
    std::vector<SourceFile> files{{"main.xt", xt}};
    return compileDocumentFromStrings(files, "main.xt", options);
}

bool hasError(const CompileResult& r, std::string_view needle) {
    for (const Diagnostic& d : r.diagnostics) {
        if (d.level == Diagnostic::Level::Error && d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string attrOf(const Document& d, NodeId node, std::string_view name) {
    const ElementData& data = d.arena().element(node);
    for (const auto& [key, value] : data.attributes) {
        if (d.ir().string(key) == name) {
            return std::string(d.ir().string(value));
        }
    }
    return {};
}

struct Loaded {
    CompileResult result;
    std::unique_ptr<Document> doc;
};

Loaded load(const std::string& xt, AssetResolver* assets, TextShaper* shaper = nullptr) {
    Loaded out;
    out.result = compileXt(xt);
    for (const Diagnostic& d : out.result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(out.result.ok());
    REQUIRE(Document::load(IrBlob{out.result.ir.data(), out.result.ir.size()}, false, shaper, assets,
                           out.doc) == Status::kOk);
    out.doc->setViewport(Rect{0, 0, 400, 300});
    return out;
}

NodeId nodeOf(Document& d, std::string_view id) { return d.byId(id).node; }

const ExternalNodeData& ext(Document& d, std::string_view id) {
    const ExternalNodeData* data = d.externalData(nodeOf(d, id));
    REQUIRE(data != nullptr);
    return *data;
}

}  // namespace

TEST_CASE("external content: compile -- tags, engine:// assets, frames, errors") {
    CompileResult r = compileXt(doc(R"~(<video id="Intro" src="movies/intro.mp4" loop autoplay></video>
        <sequence id="Seq" src="media/seq" fps="4"></sequence>
        <sequence id="Pat" src="media/seq/frame_%04d.png"></sequence>
        <sequence id="Atlas" src="media/atlas.png" frame-size="8x4" frames="3"></sequence>
        <external id="Cam" source="scene-view:main"></external>
        <external id="Eng" source="engine://minimap"></external>
        <external id="Host" source="host:portrait"></external>)~"));
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    std::vector<std::string> names;
    for (const CompiledAsset& a : r.assets) {
        names.push_back(a.name);
    }
    auto has = [&](std::string_view n) { return std::find(names.begin(), names.end(), n) != names.end(); };
    CHECK(has("engine://video:Intro"));
    CHECK(has("engine://scene-view:main"));
    CHECK(has("engine://minimap"));
    CHECK(has("engine://host:portrait"));
    CHECK(has("media/seq/frame_0000.png"));
    CHECK(has("media/seq/frame_0003.png"));
    CHECK(has("media/atlas.png"));
    CHECK_FALSE(has("movies/intro.mp4"));  // A video file is the host's, never an ASST image.

    std::unique_ptr<Document> d;
    REQUIRE(Document::load(IrBlob{r.ir.data(), r.ir.size()}, false, nullptr, nullptr, d) == Status::kOk);
    // Folder and pattern resolve to the same four frames, in order.
    CHECK(attrOf(*d, nodeOf(*d, "Seq"), "data-xt-frames") == attrOf(*d, nodeOf(*d, "Pat"), "data-xt-frames"));
    CHECK(ext(*d, "Seq").frames.size() == 4);
    CHECK(ext(*d, "Seq").duration == doctest::Approx(1.0f));
    CHECK(ext(*d, "Pat").fps == doctest::Approx(24.0f));
    CHECK(ext(*d, "Atlas").frames.empty());
    CHECK(ext(*d, "Atlas").frameSize.x == doctest::Approx(8.0f));
    CHECK(ext(*d, "Atlas").atlasFrames == 3);
    CHECK(ext(*d, "Intro").source == ExternalSourceKind::Video);
    CHECK(ext(*d, "Intro").loop == 1);
    CHECK(ext(*d, "Intro").autoplay == 1);
    CHECK(ext(*d, "Cam").source == ExternalSourceKind::SceneView);
    CHECK(ext(*d, "Eng").source == ExternalSourceKind::Engine);
    CHECK(ext(*d, "Host").source == ExternalSourceKind::Host);
    // The External child: its own node kind, a child of the element, reachable both ways.
    const NodeId child = d->externalOf(nodeOf(*d, "Cam"));
    REQUIRE(child != kInvalidNode);
    CHECK(d->arena().kind(child) == NodeKind::External);
    CHECK(d->arena().parent(child) == nodeOf(*d, "Cam"));
    CHECK(d->externalOf(child) == child);
    CHECK(d->externalOf(nodeOf(*d, "body")) == kInvalidNode);
    CHECK(d->externalElements().size() == 7);

    CHECK(hasError(compileXt(doc(R"~(<video id="V"></video>)~")), "<video> without src"));
    CHECK(
        hasError(compileXt(doc(R"~(<external source="file:x"></external>)~")), "<external> source must be"));
    CHECK(hasError(compileXt(doc(R"~(<sequence src="media/nothing"></sequence>)~")), "has no frames"));
    CHECK(hasError(compileXt(doc(R"~(<sequence src="media/atlas.png" frame-size="8"></sequence>)~")),
                   "invalid \"frame-size\""));
    CHECK(hasError(compileXt(doc(R"~(<video src="a.mp4"><div></div></video>)~")), "must not have children"));
    CHECK(hasError(compileXt(doc(R"~(<div on:click="play(Nope)"></div><video id="V" src="a.mp4"></video>)~")),
                   "unknown timeline \"Nope\""));
    CHECK(hasError(compileXt(doc(R"~(<div on:media(Nope).end="class(x)"></div>)~")),
                   "unknown video or sequence \"Nope\""));
    CHECK(hasError(
        compileXt(doc(R"~(<div on:click="play(V, reverse)"></div><video id="V" src="a.mp4"></video>)~")),
        "only takes the option loop"));
}

TEST_CASE("external content: clip-start / clip-rate parse and cascade") {
    CountingResolver assets;
    Loaded l = load(
        doc(R"~(<sequence id="S" src="media/seq"></sequence><sequence id="T" src="media/seq"></sequence>)~",
            "#S { clip-start: 250ms; clip-rate: 2 } #T { clip-start: 1.5 }"),
        &assets);
    l.doc->update(0);
    CHECK(l.doc->arena().element(nodeOf(*l.doc, "S")).style.clipStart == doctest::Approx(0.25f));
    CHECK(l.doc->arena().element(nodeOf(*l.doc, "S")).style.clipRate == doctest::Approx(2.0f));
    CHECK(l.doc->arena().element(nodeOf(*l.doc, "T")).style.clipStart == doctest::Approx(1.5f));
    CHECK(l.doc->arena().element(nodeOf(*l.doc, "T")).style.clipRate == doctest::Approx(1.0f));
    CompileResult bad = compileXt(doc("<div id=\"A\"></div>", "#A { clip-rate: 0 }"));
    bool warned = false;
    for (const Diagnostic& d : bad.diagnostics) {
        warned = warned || d.message.find("clip-rate") != std::string::npos;
    }
    CHECK(warned);
}

TEST_CASE("external content: intrinsic size from the first frame, atlas cell, host binding, placeholder") {
    CountingResolver assets;
    Loaded l = load(doc(R"~(<sequence id="S" src="media/seq"></sequence>
        <sequence id="A" src="media/atlas.png" frame-size="8x4"></sequence>
        <video id="V" src="a.mp4"></video>
        <external id="E" source="engine://minimap"></external>)~",
                        "sequence, video, external { position: absolute; left: 0; top: 0; }"),
                    &assets);
    Document& d = *l.doc;
    d.update(0);
    auto size = [&](std::string_view id) {
        const LayoutBox& b = d.arena().element(nodeOf(d, id)).box;
        return Vec2{b.width, b.height};
    };
    CHECK(size("S").x == doctest::Approx(8.0f));
    CHECK(size("S").y == doctest::Approx(4.0f));
    CHECK(size("A").x == doctest::Approx(8.0f));
    CHECK(size("A").y == doctest::Approx(4.0f));
    CHECK(size("V").x == doctest::Approx(0.0f));  // Unbound: nothing reported yet.

    d.setExternalPlaceholders(true);
    d.update(0.1f);
    CHECK(size("V").x == doctest::Approx(320.0f));
    CHECK(size("V").y == doctest::Approx(180.0f));
    CHECK(ext(d, "V").showPlaceholder == 1);
    CHECK(ext(d, "S").showPlaceholder == 0);  // A sequence is never a placeholder.

    REQUIRE(d.bindTexture(nodeOf(d, "V"), 7, 640, 360) == Status::kOk);
    std::vector<Rect> damage;
    d.takeExternalDamage(damage);
    CHECK(damage.size() == 1);
    d.update(0.2f);
    CHECK(size("V").x == doctest::Approx(640.0f));
    CHECK(size("V").y == doctest::Approx(360.0f));
    CHECK(ext(d, "V").bound == 1);
    CHECK(ext(d, "V").showPlaceholder == 0);
    // Through the child node too, and via the T-150 resolver name.
    REQUIRE(d.bindTexture(d.externalOf(nodeOf(d, "E")), 9, 64, 32) == Status::kOk);
    REQUIRE(assets.externalTexture("minimap") != nullptr);
    CHECK(assets.externalTexture("minimap")->texture == 9u);
    // CPU pixels for a software renderer keep the texture and take the pixels' size.
    ImageData pixels;
    pixels.width = 2;
    pixels.height = 2;
    pixels.rgba.assign(16, 255);
    REQUIRE(d.bindImage(nodeOf(d, "E"), &pixels) == Status::kOk);
    CHECK(assets.externalTexture("minimap")->texture == 9u);
    CHECK(assets.externalTexture("minimap")->pixels == &pixels);
    CHECK(assets.externalTexture("minimap")->width == 2);
    REQUIRE(d.unbindExternal(nodeOf(d, "E")) == Status::kOk);
    CHECK(assets.externalTexture("minimap") == nullptr);

    CHECK(d.bindTexture(nodeOf(d, "S"), 1, 8, 8) == Status::kInvalidArgument);  // Core-played.
    CHECK(d.bindTexture(nodeOf(d, "body"), 1, 8, 8) == Status::kInvalidArgument);
    CHECK(d.bindTexture(nodeOf(d, "V"), 1, -1, 8) == Status::kInvalidArgument);
    CHECK(d.setPlayback(nodeOf(d, "E"), MediaState::Playing, 0) == Status::kInvalidArgument);
}

TEST_CASE("external content: a sequence plays itself, preloads ahead, loops and ends") {
    CountingResolver assets;
    Loaded l = load(doc(R"~(<sequence id="S" src="media/seq" fps="4" autoplay
                                 on:media(S).end="class(done)"></sequence>
                             <sequence id="L" src="media/seq" fps="4" loop></sequence>)~"),
                    &assets);
    Document& d = *l.doc;
    d.setSequencePreload(2);
    const std::vector<AssetId> frames = ext(d, "S").frames;
    REQUIRE(frames.size() == 4);
    int ended = 0;
    d.addListener(d.byId("S"), EventType::MediaEnd, [&](Event&) { ++ended; });

    assets.views.clear();
    d.update(0);  // autoplay: frame 0, frames 0 and 1 decoded ahead.
    CHECK(ext(d, "S").state == MediaState::Playing);
    CHECK(d.arena().element(nodeOf(d, "S")).imageAsset == frames[0]);
    CHECK(std::count(assets.views.begin(), assets.views.end(), frames[0]) >= 1);
    CHECK(std::count(assets.views.begin(), assets.views.end(), frames[1]) >= 1);
    CHECK(std::count(assets.views.begin(), assets.views.end(), frames[2]) == 0);
    CHECK(std::count(assets.views.begin(), assets.views.end(), frames[3]) == 0);

    d.update(0.26f);
    CHECK(ext(d, "S").frame == 1);
    CHECK(d.arena().element(nodeOf(d, "S")).imageAsset == frames[1]);
    CHECK(std::count(assets.views.begin(), assets.views.end(), frames[2]) >= 1);
    CHECK(std::count(assets.views.begin(), assets.views.end(), frames[3]) == 0);
    d.update(0.6f);
    CHECK(ext(d, "S").frame == 2);
    CHECK(ended == 0);
    d.update(1.1f);  // Past 4 frames / 4 fps: holds the last frame, ends once.
    CHECK(ext(d, "S").state == MediaState::Ended);
    CHECK(d.arena().element(nodeOf(d, "S")).imageAsset == frames[3]);
    CHECK(ended == 1);
    CHECK(d.byId("S").hasClass("done"));
    d.update(1.5f);
    CHECK(ended == 1);

    // `loop`: wraps; pause holds; seek jumps; stop rewinds.
    const NodeId loopNode = nodeOf(d, "L");
    REQUIRE(d.playMedia(loopNode) == Status::kOk);
    d.update(2.0f);   // Starts at 1.5 + 0.5 = 2.0 -> 0.5 s in -> frame 2.
    d.update(2.75f);  // 1.25 s -> wraps to 0.25 s -> frame 1.
    CHECK(ext(d, "L").frame == 1);
    CHECK(ext(d, "L").state == MediaState::Playing);
    REQUIRE(d.pauseMedia(loopNode) == Status::kOk);
    d.update(3.5f);
    CHECK(ext(d, "L").frame == 1);
    REQUIRE(d.seekMedia(loopNode, 0.76f) == Status::kOk);
    CHECK(ext(d, "L").frame == 3);
    REQUIRE(d.stopMedia(loopNode) == Status::kOk);
    CHECK(ext(d, "L").frame == 0);
    CHECK(ext(d, "L").state == MediaState::Stopped);
    CHECK(d.setMediaRate(loopNode, 0) == Status::kInvalidArgument);
    CHECK(d.playMedia(nodeOf(d, "body")) == Status::kInvalidArgument);
}

TEST_CASE("external content: an atlas sequence draws one cell at a time") {
    CountingResolver assets;
    Loaded l = load(doc(R"~(<sequence id="A" src="media/atlas.png" frame-size="8x4" fps="4"></sequence>)~"),
                    &assets);
    Document& d = *l.doc;
    d.update(0);
    CHECK(ext(d, "A").atlasFrames == 4);  // Every whole cell of the 16x8 atlas.
    CHECK(ext(d, "A").duration == doctest::Approx(1.0f));
    REQUIRE(d.seekMedia(nodeOf(d, "A"), 0.8f) == Status::kOk);
    const Rect uv = d.arena().element(nodeOf(d, "A")).imageSubUv;
    CHECK(uv.x == doctest::Approx(0.5f));
    CHECK(uv.y == doctest::Approx(0.5f));
    CHECK(uv.w == doctest::Approx(0.5f));
    CHECK(uv.h == doctest::Approx(0.5f));
}

TEST_CASE("external content: video requests go to the host, ended comes back") {
    CountingResolver assets;
    Context ctx(ContextConfig{nullptr, &assets});
    struct Listener : ContextListener {
        std::vector<ExternalRequest> requests;
        std::vector<std::string> ids;
        void onExternalRequest(const ExternalRequest& r) override {
            requests.push_back(r);
            ids.push_back(std::string(r.elementId) + ":" + std::string(r.src));
        }
    } listener;
    ctx.addListener(&listener);
    // The surface's `show` fires on the first update.
    CompileResult r = compileXt(R"~(<xt><body id="body" on:show="play(V) then class(after)">
        <video id="V" src="movies/intro.mp4" on:media(V).end="class(done)"></video>
        <div id="Seek" on:click="seek(V, 2s)"></div></body></xt>)~");
    REQUIRE(r.ok());
    Result<DocumentHandle> h = ctx.loadDocument(IrBlob{r.ir.data(), r.ir.size()}, false);
    REQUIRE(h.ok());
    Document& d = *ctx.document(h.value());
    ctx.setViewport(Rect{0, 0, 200, 100}, 1);
    ctx.update(0);  // `show` fires: play(V) -> a Play request; the chain waits for the end.
    REQUIRE(listener.requests.size() == 1);
    CHECK(listener.requests[0].kind == ExternalRequestKind::Play);
    CHECK(listener.requests[0].value == doctest::Approx(0.0f));
    CHECK(listener.requests[0].document == h.value());
    CHECK(listener.ids[0] == "V:movies/intro.mp4");
    CHECK(ext(d, "V").state == MediaState::Playing);
    CHECK_FALSE(d.byId("body").hasClass("after"));

    const NodeId v = nodeOf(d, "V");
    REQUIRE(d.setMediaDuration(v, 12) == Status::kOk);
    REQUIRE(d.setPlayback(v, MediaState::Playing, 3.5f) == Status::kOk);
    CHECK(ext(d, "V").time == doctest::Approx(3.5f));
    REQUIRE(d.seekMedia(v, 20) == Status::kOk);  // Clamped to the known duration.
    CHECK(listener.requests.back().kind == ExternalRequestKind::Seek);
    CHECK(listener.requests.back().value == doctest::Approx(12.0f));
    REQUIRE(d.setMediaRate(v, 1.5f) == Status::kOk);
    CHECK(listener.requests.back().kind == ExternalRequestKind::Rate);
    REQUIRE(d.setMediaLoop(v, true) == Status::kOk);
    CHECK(listener.requests.back().kind == ExternalRequestKind::Loop);
    CHECK(listener.requests.back().value == doctest::Approx(1.0f));
    REQUIRE(d.pauseMedia(v) == Status::kOk);
    CHECK(listener.requests.back().kind == ExternalRequestKind::Pause);

    REQUIRE(d.setPlayback(v, MediaState::Ended, 12) == Status::kOk);
    ctx.update(0.1f);
    CHECK(d.byId("V").hasClass("done"));
    CHECK(d.byId("body").hasClass("after"));
    CHECK(ext(d, "V").state == MediaState::Ended);
    ctx.removeListener(&listener);
}

TEST_CASE("external content: sync clips follow a @timeline group with clip-start / clip-rate") {
    CountingResolver assets;
    Loaded l = load(doc(R"~(<div id="A"></div>
        <sequence id="S" src="media/seq" fps="4" sync></sequence>
        <video id="V" src="a.mp4" sync></video>)~",
                        R"~(#S { clip-start: 0.5s; clip-rate: 2 }
                           #V { clip-start: 1s }
                           @timeline intro { #A { 0s { opacity: 0 } 3s { opacity: 1 } } })~"),
                    &assets);
    Document& d = *l.doc;
    std::vector<ExternalRequest> requests;
    d.setExternalRequestListener([&](const ExternalRequest& r) { requests.push_back(r); });
    d.update(0);

    // Seek (paused group): the sequence shows local (t - 0.5) * 2; the video is paused + seeked.
    REQUIRE(d.seekTimeline("", "intro", 0.75f) == Status::kOk);
    CHECK(ext(d, "S").time == doctest::Approx(0.5f));
    CHECK(ext(d, "S").frame == 2);
    REQUIRE_FALSE(requests.empty());
    CHECK(requests.back().kind == ExternalRequestKind::Seek);
    CHECK(requests.back().value == doctest::Approx(0.0f));  // Before its clip-start: held at 0.
    REQUIRE(d.seekTimeline("", "intro", 2.0f) == Status::kOk);
    CHECK(requests.back().value == doctest::Approx(1.0f));

    // Playing: the video is asked to play once when the group reaches its clip-start.
    requests.clear();
    REQUIRE(d.playTimeline("", "intro") == Status::kOk);
    d.update(0.5f);
    int plays = 0;
    for (const ExternalRequest& r : requests) {
        plays += r.kind == ExternalRequestKind::Play ? 1 : 0;
    }
    CHECK(plays == 0);  // t = 0.5 < clip-start 1.
    d.update(1.2f);
    d.update(1.4f);
    plays = 0;
    for (const ExternalRequest& r : requests) {
        plays += r.kind == ExternalRequestKind::Play ? 1 : 0;
    }
    CHECK(plays == 1);
    CHECK(ext(d, "S").frame == 3);  // (1.4 - 0.5) * 2 = 1.8 s, past its 1 s: the last frame.
    REQUIRE(d.stopTimeline("", "intro") == Status::kOk);
    CHECK(requests.back().kind == ExternalRequestKind::Pause);
}

TEST_CASE("external content: trigger play/stop/seek on a sequence") {
    CountingResolver assets;
    Loaded l = load(R"~(<xt><body id="body" on:show="seek(S, 0.5s)">
        <sequence id="S" src="media/seq" fps="4"></sequence><div id="Go" on:click="play(S, loop)"></div>
        <div id="Halt" on:click="stop(S)"></div></body></xt>)~",
                    &assets);
    Document& d = *l.doc;
    d.update(0);
    CHECK(ext(d, "S").frame == 2);
    CHECK(ext(d, "S").state == MediaState::Stopped);
}
