#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/baked.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// BakedWriter/BakedPlayer round-trip tests. See docs/design/09-command-list.md §9.6,
/// docs/design/12-compiler.md §12.5, docs/design/19-3d-space.md §19.7/§19.8, docs/tasks/T-37.md
/// "測試" / "test_baked.cpp", docs/tasks/T-49.md.

using namespace xt;
using namespace xt::compile;

namespace {

namespace fs = std::filesystem;

PathRef addRectPath(CommandList& cl, float x, float y, float w, float h) {
    PathRef ref{};
    ref.verbOffset = static_cast<uint32_t>(cl.verbs.size());
    ref.pointOffset = static_cast<uint32_t>(cl.points.size());
    cl.verbs.push_back(PathVerb::Move);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Line);
    cl.verbs.push_back(PathVerb::Close);
    float pts[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cl.points.insert(cl.points.end(), pts, pts + 8);
    ref.verbCount = 5;
    ref.pointCount = 8;
    return ref;
}

void pushCmd(CommandList& cl, CmdKind kind, uint32_t index) {
    Command cmd{};
    cmd.kind = kind;
    cmd.index = index;
    cl.commands.push_back(cmd);
}

/// Builds a CommandList containing at least one of every CmdKind, exercising every payload pool.
CommandList buildFullCommandList(float seed) {
    CommandList cl;
    cl.viewport = Rect{0, 0, 100, 80};
    cl.devicePixelRatio = 2.0f;

    // FillPath (Solid).
    FillCmd fill{};
    fill.xf = Mat2x3{1, 0, 0, 1, seed, 0};
    fill.path = addRectPath(cl, 0, 0, 10, 10);
    fill.paint.kind = PaintKind::Solid;
    fill.paint.color = Color{1, 0, 0, 1};
    fill.rule = FillRule::NonZero;
    fill.antialias = 1;
    cl.fills.push_back(fill);
    pushCmd(cl, CmdKind::FillPath, 0);

    // FillPath (Linear gradient), exercises the stops pool.
    cl.stops.push_back(GradientStop{0.0f, Color{0, 0, 0, 1}});
    cl.stops.push_back(GradientStop{1.0f, Color{1, 1, 1, 1}});
    FillCmd gradientFill{};
    gradientFill.path = addRectPath(cl, 20, 0, 10, 10);
    gradientFill.paint.kind = PaintKind::Linear;
    gradientFill.paint.color = Color{1, 1, 1, 1};
    gradientFill.paint.p0 = Vec2{20, 0};
    gradientFill.paint.p1 = Vec2{30, 0};
    gradientFill.paint.stopOffset = 0;
    gradientFill.paint.stopCount = 2;
    gradientFill.rule = FillRule::EvenOdd;
    gradientFill.antialias = 1;
    cl.fills.push_back(gradientFill);
    pushCmd(cl, CmdKind::FillPath, 1);

    // StrokePath, exercises the dashes pool.
    cl.dashes.push_back(4.0f);
    cl.dashes.push_back(2.0f);
    StrokeCmd stroke{};
    stroke.path = addRectPath(cl, 0, 20, 10, 10);
    stroke.paint.kind = PaintKind::Solid;
    stroke.paint.color = Color{0, 1, 0, 1};
    stroke.width = 2;
    stroke.cap = LineCap::Round;
    stroke.join = LineJoin::Round;
    stroke.miterLimit = 4;
    stroke.dashOffset = 0;
    stroke.dashCount = 2;
    stroke.dashPhase = 0.5f;
    cl.strokes.push_back(stroke);
    pushCmd(cl, CmdKind::StrokePath, 0);

    // DrawImage.
    ImageCmd image{};
    image.asset = 7;
    image.dst = Rect{0, 40, 16, 16};
    image.srcUv = Rect{0, 0, 1, 1};
    image.opacity = 0.75f;
    image.smooth = 1;
    cl.images.push_back(image);
    pushCmd(cl, CmdKind::DrawImage, 0);

    // DrawGlyphs, exercises the glyphs/glyphPos pools.
    cl.glyphs.push_back(65);
    cl.glyphs.push_back(66);
    cl.glyphPos.push_back(Vec2{0, 60});
    cl.glyphPos.push_back(Vec2{10, 60});
    GlyphsCmd glyphs{};
    glyphs.font = 3;
    glyphs.size = 12;
    glyphs.color = Color{0, 0, 1, 1};
    glyphs.glyphOffset = 0;
    glyphs.glyphCount = 2;
    cl.glyphRuns.push_back(glyphs);
    pushCmd(cl, CmdKind::DrawGlyphs, 0);

    // PushClipRect / PopClip.
    ClipRectCmd clipRect{};
    clipRect.rect = Rect{0, 0, 100, 80};
    cl.clipRects.push_back(clipRect);
    pushCmd(cl, CmdKind::PushClipRect, 0);
    pushCmd(cl, CmdKind::PopClip, 0);

    // PushClipPath / PopClip.
    ClipPathCmd clipPath{};
    clipPath.path = addRectPath(cl, 5, 5, 6, 6);
    clipPath.rule = FillRule::NonZero;
    cl.clipPaths.push_back(clipPath);
    pushCmd(cl, CmdKind::PushClipPath, 0);
    pushCmd(cl, CmdKind::PopClip, 0);

    // PushLayer / PopLayer.
    LayerCmd layer{};
    layer.opacity = 0.5f;
    layer.blend = BlendMode::Multiply;
    layer.boundsHint = Rect{0, 0, 100, 80};
    cl.layers.push_back(layer);
    pushCmd(cl, CmdKind::PushLayer, 0);
    pushCmd(cl, CmdKind::PopLayer, 0);

    // PushMask / EndMaskContent / PopMask.
    MaskCmd mask{};
    mask.mode = MaskMode::Luma;
    mask.boundsHint = Rect{0, 0, 100, 80};
    cl.masks.push_back(mask);
    pushCmd(cl, CmdKind::PushMask, 0);
    pushCmd(cl, CmdKind::EndMaskContent, 0);
    pushCmd(cl, CmdKind::PopMask, 0);

    return cl;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Converts a freshly-written (current, minor >= 4) `.xtcl` byte buffer's `fills`/`strokes`
/// sections to the pre-T-116 (minor <= 3) layout, by removing each packed paint's 8-byte `p2`
/// field (PackedPaintRef: kind[1] + r/g/b/a[16] + p0x/p0y/p1x/p1y[16] = 33 bytes, then p2x/p2y[8]).
/// The minor-0/1/2 legacy-file tests below hand-build an old-format file by patching a real
/// writer's current-format output; T-116 changed the fills/strokes section itself (not just an
/// appended trailing one, unlike every earlier format bump), so those tests need this extra
/// mid-payload conversion before patching the header and stripping their own trailing sections.
/// No-op (removed == 0) when the frame has no fills/strokes, which is why it is safe to call
/// unconditionally.
std::vector<uint8_t> toPreT116Layout(std::vector<uint8_t> bytes) {
    constexpr size_t kHeaderSize = 28;
    constexpr size_t kFrameHeaderSize = 8;  // frameIndex + payloadLength.
    constexpr size_t kCommandSize = 5;
    constexpr size_t kPaintGap = 40 + 33;   // xf[24]+path[16] prefix, then PackedPaintRef's p2 offset.
    constexpr size_t kFillNewSize = 91;     // xf[24]+path[16]+paint[49]+rule[1]+antialias[1].
    constexpr size_t kStrokeNewSize = 111;  // xf[24]+path[16]+paint[49]+width[4]+cap[1]+join[1]+
                                            // miterLimit[4]+dashOffset[4]+dashCount[4]+dashPhase[4].

    auto stripField = [](const uint8_t* data, uint32_t count, size_t structSize, size_t gapOffset) {
        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(count) * (structSize - 8));
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* s = data + static_cast<size_t>(i) * structSize;
            out.insert(out.end(), s, s + gapOffset);
            out.insert(out.end(), s + gapOffset + 8, s + structSize);
        }
        return out;
    };

    size_t pos = kHeaderSize + kFrameHeaderSize;
    uint32_t cmdCount = 0;
    std::memcpy(&cmdCount, bytes.data() + pos, 4);
    pos += 4 + static_cast<size_t>(cmdCount) * kCommandSize;

    size_t fillsCountAt = pos;
    uint32_t fillsCount = 0;
    std::memcpy(&fillsCount, bytes.data() + fillsCountAt, 4);
    size_t fillsStart = fillsCountAt + 4;
    std::vector<uint8_t> newFills =
        stripField(bytes.data() + fillsStart, fillsCount, kFillNewSize, kPaintGap);
    size_t fillsEnd = fillsStart + static_cast<size_t>(fillsCount) * kFillNewSize;

    size_t strokesCountAt = fillsEnd;
    uint32_t strokesCount = 0;
    std::memcpy(&strokesCount, bytes.data() + strokesCountAt, 4);
    size_t strokesStart = strokesCountAt + 4;
    std::vector<uint8_t> newStrokes =
        stripField(bytes.data() + strokesStart, strokesCount, kStrokeNewSize, kPaintGap);
    size_t strokesEnd = strokesStart + static_cast<size_t>(strokesCount) * kStrokeNewSize;

    size_t removed = static_cast<size_t>(fillsCount) * 8 + static_cast<size_t>(strokesCount) * 8;

    std::vector<uint8_t> out;
    out.reserve(bytes.size() - removed);
    out.insert(out.end(), bytes.begin(), bytes.begin() + static_cast<long>(fillsCountAt));
    const uint8_t* fillsCountBytes = reinterpret_cast<const uint8_t*>(&fillsCount);
    out.insert(out.end(), fillsCountBytes, fillsCountBytes + 4);
    out.insert(out.end(), newFills.begin(), newFills.end());
    const uint8_t* strokesCountBytes = reinterpret_cast<const uint8_t*>(&strokesCount);
    out.insert(out.end(), strokesCountBytes, strokesCountBytes + 4);
    out.insert(out.end(), newStrokes.begin(), newStrokes.end());
    out.insert(out.end(), bytes.begin() + static_cast<long>(strokesEnd), bytes.end());

    uint32_t payloadLength = 0;
    std::memcpy(&payloadLength, out.data() + kHeaderSize + 4, 4);
    payloadLength -= static_cast<uint32_t>(removed);
    std::memcpy(out.data() + kHeaderSize + 4, &payloadLength, 4);

    return out;
}

}  // namespace

TEST_CASE("Baked: 3-frame round trip, dumpCommandList matches per frame") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_roundtrip.xtcl";

    std::vector<CommandList> frames = {buildFullCommandList(0.0f), buildFullCommandList(1.0f),
                                       buildFullCommandList(2.0f)};

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 3, 24.0f, Rect{0, 0, 100, 80}, 2.0f));
    for (uint32_t i = 0; i < 3; ++i) {
        REQUIRE(writer.writeFrame(i, frames[i]));
    }
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);
    CHECK(player.frameCount() == 3);
    CHECK(player.fps() == doctest::Approx(24.0f));
    CHECK(player.viewport().w == doctest::Approx(100.0f));
    CHECK(player.viewport().h == doctest::Approx(80.0f));
    CHECK(player.dpr() == doctest::Approx(2.0f));

    for (uint32_t i = 0; i < 3; ++i) {
        std::string expected;
        dumpCommandList(frames[i], expected);
        std::string actual;
        dumpCommandList(player.frame(i), actual);
        CHECK(actual == expected);
    }

    std::error_code ec;
    fs::remove(path, ec);
}

// T-263d: the format carries no source table. A list of one source is written as it always was and
// played back naming none; a list painted from several Documents is an error.
TEST_CASE("Baked: T-263d one source is written as before, several are rejected, a played frame names none") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_sources.xtcl";
    static const char storage[2] = {0, 0};  // two addresses, never dereferenced
    const IrView* irA = reinterpret_cast<const IrView*>(&storage[0]);
    const IrView* irB = reinterpret_cast<const IrView*>(&storage[1]);

    CommandList plain = buildFullCommandList(0.0f);
    CommandList one = plain;
    one.sources.push_back(CommandSource{irA, 1});
    CommandList two = one;
    two.sources.push_back(CommandSource{irB, 2});

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 2, 24.0f, Rect{0, 0, 100, 80}, 2.0f));
    REQUIRE(writer.writeFrame(0, plain));
    REQUIRE(writer.writeFrame(1, one));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    {
        BakedPlayer player;
        REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/false) == Status::kOk);
        std::string expected;
        dumpCommandList(plain, expected);
        for (uint32_t i = 0; i < 2; ++i) {
            const CommandList& frame = player.frame(i);
            CHECK(frame.sources.empty());
            std::string actual;
            dumpCommandList(frame, actual);
            CHECK(actual == expected);  // byte for byte the frame of the list without a table
        }
    }

    BakedWriter rejecting;
    REQUIRE(rejecting.open(path.string(), 1, 24.0f, Rect{0, 0, 100, 80}, 2.0f));
    CHECK(std::string(rejecting.lastError()).empty());
    CHECK_FALSE(rejecting.writeFrame(0, two));
    // T-263g item 11: the refusal says why.
    CHECK(std::string(rejecting.lastError()) == "baked frames cannot store a list with more than one source");
    CHECK_FALSE(rejecting.close());

    std::error_code ec;
    fs::remove(path, ec);
}

// T-263f item 8: the format has no particles / particleInstances / imageMeshes / meshVertices /
// meshIndices pools, so a DrawParticles or DrawImageMesh command read back indexes a pool that is
// not there (the played frame fails validateCommandList() and comes out empty). Until the format
// stores them, the writer refuses the frame.
TEST_CASE(
    "Baked: T-263f a frame with <particles> or an <xt-mesh> is refused instead of written to play empty") {
    struct Case {
        const char* name;
        const char* body;
        CmdKind kind;
        const char* error;  // T-263g item 11: BakedWriter::lastError() after the refusal
    };
    const Case cases[] = {
        {"particles",
         R"~(<div style="width: 40px; height: 40px; background-color: #ff0000"></div>
             <particles burst="6" rate="0" lifetime="5" size="4" color="#ffffff80" autoplay
                        style="position: absolute; left: 20px; top: 30px; width: 40px; height: 40px">
             </particles>)~",
         CmdKind::DrawParticles, "baked frames cannot store DrawParticles commands yet"},
        {"xt-mesh",
         R"~(<div style="width: 40px; height: 40px; background-color: #ff0000"></div>
             <svg viewBox="0 0 100 100" style="width: 100px; height: 100px">
               <xt-mesh href="m.png" width="50" height="50"></xt-mesh></svg>)~",
         CmdKind::DrawImageMesh, "baked frames cannot store DrawImageMesh commands yet"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.name);
        const std::string text = std::string("<xt><body>") + c.body + "</body></xt>";
        CompileOptions options;
        options.emitMeta = false;
        std::vector<SourceFile> files{{"main.xt", text}};
        CompileResult result = compileDocumentFromStrings(files, "main.xt", options);
        REQUIRE(result.ok());

        // The flow of xtframe-bake: one Context, one Document, frame 0.
        NullTextShaper shaper;
        Context ctx(ContextConfig{&shaper, nullptr});
        REQUIRE(ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/false).ok());
        const Rect viewport{0, 0, 200, 200};
        ctx.setViewport(viewport, 1.0f);
        ctx.update(0.0f);
        CommandList list;
        ctx.render(list);
        size_t unsupported = 0;
        for (const Command& command : list.commands) {
            unsupported += command.kind == c.kind ? 1 : 0;
        }
        REQUIRE(unsupported == 1);          // the document does draw the command in question
        REQUIRE(list.commands.size() > 1);  // and something else beside it
        REQUIRE(validateCommandList(list));

        const fs::path path = xt::test::tempDir() / (std::string("xt_test_baked_t263f_") + c.name + ".xtcl");
        BakedWriter writer;
        REQUIRE(writer.open(path.string(), 1, 10.0f, viewport, 1.0f));
        const bool wrote = writer.writeFrame(0, list);
        const std::string error = writer.lastError();
        const bool closed = writer.close();
        // Before T-263f both calls succeeded, BakedPlayer::load() took the file and frame(0) came
        // back with no command at all (the decoded list failed validateCommandList()).
        CHECK_FALSE(wrote);
        CHECK(error == c.error);  // T-263g item 11: named as dumpCommandList() prints the command
        CHECK_FALSE(closed);
        {
            // Nothing of the refused frame reached the file: the header alone, no frame to play.
            std::vector<uint8_t> bytes = readFile(path.string());
            CHECK(bytes.size() == sizeof(BakedHeader));
        }

        // The same list without that command is written as before.
        CommandList supported = list;
        std::erase_if(supported.commands, [&](const Command& command) { return command.kind == c.kind; });
        BakedWriter again;
        REQUIRE(again.open(path.string(), 1, 10.0f, viewport, 1.0f));
        CHECK(again.writeFrame(0, supported));
        CHECK(std::string(again.lastError()).empty());
        CHECK(again.close());
        std::error_code ec;
        fs::remove(path, ec);
    }
}

TEST_CASE("Baked: the writer opens a UTF-8 path the ANSI code page cannot spell (T-261e)") {
    // xt_test_baked_アイコン.xtcl, made at run time.
    const fs::path path = xt::test::tempDir() /
                          pathFromUtf8("xt_test_baked_\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xB3\xE3\x83\xB3.xtcl");
    BakedWriter writer;
    REQUIRE(writer.open(pathToUtf8(path), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, buildFullCommandList(0.0f)));
    REQUIRE(writer.close());
    std::error_code ec;
    CHECK(fs::file_size(path, ec) > 0);
    fs::remove(path, ec);
}

TEST_CASE("Baked: truncated file is rejected as kInvalidIr") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_truncated.xtcl";
    CommandList frame = buildFullCommandList(0.0f);

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, frame));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    REQUIRE(bytes.size() > 40);
    bytes.resize(bytes.size() - 20);  // Chop off the tail of the last frame's payload.

    BakedPlayer player;
    CHECK(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kInvalidIr);

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: out-of-range frame index returns an empty CommandList") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_oob.xtcl";
    CommandList frame = buildFullCommandList(0.0f);

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, frame));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);

    const CommandList& oob = player.frame(999);
    CHECK(oob.commands.empty());
    CHECK(oob.fills.empty());

    std::error_code ec;
    fs::remove(path, ec);
}

// T-263a E1: a frame whose decoded list fails validateCommandList() (here a FillPath pointing past
// the fills, and a path range past the point pool) reads as an empty frame, so a backend never
// indexes its pools; the valid frame next to it still decodes.
TEST_CASE("Baked: a frame with out-of-range indices decodes as an empty CommandList") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_invalid.xtcl";
    CommandList good = buildFullCommandList(0.0f);
    CommandList badIndex = buildFullCommandList(0.0f);
    pushCmd(badIndex, CmdKind::FillPath, 1000);
    CommandList badRange = buildFullCommandList(0.0f);
    badRange.fills[0].path.pointOffset = 0xFFFFFFF0u;

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 3, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, good));
    REQUIRE(writer.writeFrame(1, badIndex));
    REQUIRE(writer.writeFrame(2, badRange));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);
    CHECK(!player.frame(0).commands.empty());
    for (uint32_t i = 1; i < 3; ++i) {
        CAPTURE(i);
        const CommandList& bad = player.frame(i);
        CHECK(bad.commands.empty());
        CHECK(bad.fills.empty());
        CHECK(bad.viewport.w == doctest::Approx(64.0f));
    }

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: repeated frame() calls do not grow capacity") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_capacity.xtcl";
    CommandList frame = buildFullCommandList(0.0f);

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, frame));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);

    const CommandList& first = player.frame(0);
    size_t capCommands = first.commands.capacity();
    size_t capFills = first.fills.capacity();
    size_t capVerbs = first.verbs.capacity();
    size_t capPoints = first.points.capacity();

    const CommandList& second = player.frame(0);
    CHECK(second.commands.capacity() == capCommands);
    CHECK(second.fills.capacity() == capFills);
    CHECK(second.verbs.capacity() == capVerbs);
    CHECK(second.points.capacity() == capPoints);

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: 3D commands (PushProjection/PushTransform3D/DrawImageQuad) round trip") {
    // docs/design/19-3d-space.md §19.7-19.8, docs/tasks/T-49.md: `.xtcl` minor 1 appends
    // projections/transforms3/imageQuads after `dashes`.
    fs::path path = xt::test::tempDir() / "xt_test_baked_3d.xtcl";

    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1.0f;

    ProjectionCmd proj{};
    proj.viewProj = mat4Identity();
    proj.rootXf = Mat2x3{};
    proj.boxRect = Rect{0, 0, 64, 64};
    proj.depthTest = 0;
    proj.boundsHint = Rect{4, 5, 20, 30};  // T-105: minor 3 carries the hint.
    cl.projections.push_back(proj);
    pushCmd(cl, CmdKind::PushProjection, 0);

    Transform3DCmd xf3{};
    xf3.world3 = mat4Translate(Vec3{1, 2, 3});
    cl.transforms3.push_back(xf3);
    pushCmd(cl, CmdKind::PushTransform3D, 0);

    ImageQuadCmd quad{};
    quad.asset = 9;
    quad.p[0] = Vec2{0, 0};
    quad.p[1] = Vec2{10, 0};
    quad.p[2] = Vec2{10, 10};
    quad.p[3] = Vec2{0, 10};
    quad.srcUv = Rect{0, 0, 1, 1};
    quad.opacity = 0.8f;
    quad.smooth = 1;
    cl.imageQuads.push_back(quad);
    pushCmd(cl, CmdKind::DrawImageQuad, 0);

    pushCmd(cl, CmdKind::PopTransform3D, 0);
    pushCmd(cl, CmdKind::PopProjection, 0);

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, cl));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);

    std::string expected;
    dumpCommandList(cl, expected);
    std::string actual;
    dumpCommandList(player.frame(0), actual);
    CHECK(actual == expected);

    const CommandList& loaded = player.frame(0);
    REQUIRE(loaded.projections.size() == 1);
    REQUIRE(loaded.transforms3.size() == 1);
    REQUIRE(loaded.imageQuads.size() == 1);
    CHECK(loaded.imageQuads[0].asset == 9);
    CHECK(loaded.imageQuads[0].opacity == doctest::Approx(0.8f));
    CHECK(loaded.projections[0].boundsHint.x == doctest::Approx(4.0f));
    CHECK(loaded.projections[0].boundsHint.y == doctest::Approx(5.0f));
    CHECK(loaded.projections[0].boundsHint.w == doctest::Approx(20.0f));
    CHECK(loaded.projections[0].boundsHint.h == doctest::Approx(30.0f));

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: effect commands round trip") {
    // docs/design/18-effects.md §18.5, docs/tasks/T-52.md: `.xtcl` minor 2 appends
    // effectCmds/effects/shaders/shaderParams/shaderText after imageQuads.
    fs::path path = xt::test::tempDir() / "xt_test_baked_effects.xtcl";

    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1.0f;

    EffectInstance shadow{};
    shadow.kind = EffectKind::DropShadow;
    shadow.enabled = 1;
    shadow.shader = kInvalidAsset;
    shadow.params[0] = 10;
    shadow.params[1] = 0;
    shadow.params[2] = 4;
    shadow.params[3] = 0;
    shadow.params[4] = 1;
    shadow.colors[0] = Color{0, 0, 0, 1};
    cl.effects.push_back(shadow);

    EffectCmd ec{};
    ec.effectOffset = 0;
    ec.effectCount = 1;
    ec.xf = Mat2x3{1, 0, 0, 1, 20, 20};
    ec.boundsHint = Rect{4, 4, 132, 82};
    ec.contentBounds = Rect{20, 20, 100, 50};
    cl.effectCmds.push_back(ec);
    pushCmd(cl, CmdKind::PushEffect, 0);

    FillCmd fill{};
    fill.xf = Mat2x3{};
    fill.path = addRectPath(cl, 20, 20, 100, 50);
    fill.paint.kind = PaintKind::Solid;
    fill.paint.color = Color{1, 1, 1, 1};
    fill.rule = FillRule::NonZero;
    fill.antialias = 1;
    cl.fills.push_back(fill);
    pushCmd(cl, CmdKind::FillPath, 0);

    pushCmd(cl, CmdKind::PopEffect, 0);

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, cl));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    REQUIRE(bytes.size() >= sizeof(BakedHeader));
    uint16_t minor = 0;
    std::memcpy(&minor, bytes.data() + 6, sizeof(minor));
    CHECK(minor == 4);

    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);

    std::string expected;
    dumpCommandList(cl, expected);
    std::string actual;
    dumpCommandList(player.frame(0), actual);
    CHECK(actual == expected);

    const CommandList& loaded = player.frame(0);
    REQUIRE(loaded.effectCmds.size() == 1);
    REQUIRE(loaded.effects.size() == 1);
    CHECK(loaded.effects[0].kind == EffectKind::DropShadow);
    CHECK(loaded.effects[0].params[0] == doctest::Approx(10.0f));

    std::error_code ec2;
    fs::remove(path, ec2);
}

TEST_CASE("Baked: minor 2 file (pre-T-105) still loads, boundsHint read as the viewport") {
    // docs/tasks/T-105.md: minor 3 appends ProjectionCmd::boundsHint (4 floats) to each packed
    // projection. This hand-builds a minor-2 file the same way the minor-1 test does: write a
    // normal minor-3 file holding exactly one PushProjection, then drop that projection's last
    // 16 bytes (the hint) -- the payload's tail after it is seven "count = 0" u32s (transforms3,
    // imageQuads and the five effect vectors), so the hint's position is known.
    fs::path path = xt::test::tempDir() / "xt_test_baked_minor2.xtcl";

    CommandList cl;
    cl.viewport = Rect{0, 0, 64, 64};
    cl.devicePixelRatio = 1.0f;
    ProjectionCmd proj{};
    proj.viewProj = mat4Identity();
    proj.rootXf = Mat2x3{};
    proj.boxRect = Rect{0, 0, 64, 64};
    proj.depthTest = 0;
    proj.boundsHint = Rect{4, 5, 20, 30};
    cl.projections.push_back(proj);
    pushCmd(cl, CmdKind::PushProjection, 0);
    // T-263a: frame() rejects a list that leaves a projection open (the extra 5-byte command sits
    // before every pool, so the tail offsets below are unchanged).
    pushCmd(cl, CmdKind::PopProjection, 0);

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, cl));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = toPreT116Layout(readFile(path.string()));
    const size_t kTail = 7 * 4;  // transforms3, imageQuads + the five effect vectors: all empty.
    REQUIRE(bytes.size() > 28 + 8 + kTail + 16);
    bytes[6] = 2;  // BakedHeader::minor, offset 6 -> 2.
    bytes[7] = 0;

    uint32_t payloadLength = 0;
    std::memcpy(&payloadLength, bytes.data() + 28 + 4, 4);  // header(28) + frameIndex(4).
    uint32_t newLength = payloadLength - 16;
    std::memcpy(bytes.data() + 28 + 4, &newLength, 4);
    size_t hintAt = bytes.size() - kTail - 16;
    bytes.erase(bytes.begin() + static_cast<long>(hintAt), bytes.begin() + static_cast<long>(hintAt + 16));

    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);
    const CommandList& loaded = player.frame(0);
    REQUIRE(loaded.projections.size() == 1);
    CHECK(loaded.projections[0].boxRect.w == doctest::Approx(64.0f));
    // A pre-T-105 file carries no hint: the viewport, what computeDamage() used before T-105.
    CHECK(loaded.projections[0].boundsHint == Rect{0, 0, 64, 64});

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: minor 1 file (pre-T-52) still loads, effect vectors read as empty") {
    // BakedWriter always writes the current (minor 3) format, so this test hand-builds a minor-1
    // file (same technique as the minor-0 test below) by writing a normal minor-2 file whose
    // CommandList carries no effect content, then stripping the five trailing "count = 0" u32s
    // that a minor-2+ writer always appends for the (here, empty) effectCmds/effects/shaders/
    // shaderParams/shaderText vectors -- byte-for-byte what a real pre-T-52 (minor-1) BakedWriter
    // would have produced for the same CommandList.
    fs::path path = xt::test::tempDir() / "xt_test_baked_minor1.xtcl";
    CommandList frame = buildFullCommandList(0.0f);
    REQUIRE(frame.effectCmds.empty());
    REQUIRE(frame.effects.empty());

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, frame));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = toPreT116Layout(readFile(path.string()));
    REQUIRE(bytes.size() > 28 + 8 + 20);

    bytes[6] = 1;  // BakedHeader::minor, offset 6 -> 1.
    bytes[7] = 0;

    uint32_t payloadLength = 0;
    std::memcpy(&payloadLength, bytes.data() + 28 + 4, 4);  // header(28) + frameIndex(4).
    REQUIRE(payloadLength >= 20);
    uint32_t newLength = payloadLength - 20;
    std::memcpy(bytes.data() + 28 + 4, &newLength, 4);

    for (size_t i = 1; i <= 20; ++i) {
        CHECK(bytes[bytes.size() - i] == 0);  // The five empty effect vectors' u32 counts.
    }
    bytes.resize(bytes.size() - 20);

    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);
    const CommandList& loaded = player.frame(0);
    CHECK(loaded.effectCmds.empty());
    CHECK(loaded.effects.empty());
    CHECK(loaded.shaders.empty());
    CHECK(loaded.shaderParams.empty());
    CHECK(loaded.shaderText.empty());
    // minor 1 already carried the 3D vectors (all empty here too, same as buildFullCommandList).
    CHECK(loaded.projections.empty());
    CHECK(loaded.transforms3.empty());
    CHECK(loaded.imageQuads.empty());

    std::string expected;
    dumpCommandList(frame, expected);
    std::string actual;
    dumpCommandList(loaded, actual);
    CHECK(actual == expected);

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: minor 0 file (pre-T-49) still loads, 3D vectors read as empty") {
    // docs/tasks/T-49.md: "BakedPlayer::load 接受 minor 0（三個向量視為空）". BakedWriter always
    // writes the current (minor 1) format, so this test hand-builds a minor-0 file by writing a
    // normal v1 file whose CommandList carries no 3D content (buildFullCommandList doesn't add
    // any), then stripping the three trailing "count = 0" u32s that v1 always appends for the
    // (here, empty) projections/transforms3/imageQuads vectors -- byte-for-byte what a real
    // pre-T-49 BakedWriter would have produced for the same CommandList.
    fs::path path = xt::test::tempDir() / "xt_test_baked_minor0.xtcl";
    CommandList frame = buildFullCommandList(0.0f);
    REQUIRE(frame.projections.empty());
    REQUIRE(frame.transforms3.empty());
    REQUIRE(frame.imageQuads.empty());

    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 1, 30.0f, Rect{0, 0, 64, 64}, 1.0f));
    REQUIRE(writer.writeFrame(0, frame));
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = toPreT116Layout(readFile(path.string()));
    REQUIRE(bytes.size() > 28 + 8 + 12);

    bytes[6] = 0;  // BakedHeader::minor, offset 6 (magic[4] + major u16) -> 0.
    bytes[7] = 0;

    uint32_t payloadLength = 0;
    std::memcpy(&payloadLength, bytes.data() + 28 + 4, 4);  // header(28) + frameIndex(4).
    REQUIRE(payloadLength >= 12);
    uint32_t newLength = payloadLength - 12;
    std::memcpy(bytes.data() + 28 + 4, &newLength, 4);

    for (size_t i = 1; i <= 12; ++i) {
        CHECK(bytes[bytes.size() - i] == 0);  // The three empty vectors' u32 counts.
    }
    bytes.resize(bytes.size() - 12);

    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);
    const CommandList& loaded = player.frame(0);
    CHECK(loaded.projections.empty());
    CHECK(loaded.transforms3.empty());
    CHECK(loaded.imageQuads.empty());

    std::string expected;
    dumpCommandList(frame, expected);
    std::string actual;
    dumpCommandList(loaded, actual);
    CHECK(actual == expected);

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_CASE("Baked: compileLottie + Context + BakedWriter/Player, same flow as xtframe-bake") {
    std::string jsonPath = std::string(XT_TEST_DATA_DIR) + "/min_solid_anim.json";
    CompileOptions options;
    options.rootDir = XT_TEST_DATA_DIR;
    CompileResult result = compileLottie(jsonPath, options);
    REQUIRE(result.ok());

    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, nullptr});
    Result<DocumentHandle> docResult =
        ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, /*takeOwnership=*/false);
    REQUIRE(docResult.ok());

    constexpr float fps = 10.0f;
    constexpr uint32_t frameCount = 5;
    Rect viewport{0, 0, 64, 64};
    ctx.setViewport(viewport, 1.0f);

    fs::path path = xt::test::tempDir() / "xt_test_baked_pipeline.xtcl";
    BakedWriter writer;
    REQUIRE(writer.open(path.string(), frameCount, fps, viewport, 1.0f));
    for (uint32_t i = 0; i < frameCount; ++i) {
        ctx.update(i == 0 ? 0.0f : 1.0f / fps);
        CommandList list;
        ctx.render(list);
        REQUIRE(writer.writeFrame(i, list));
    }
    REQUIRE(writer.close());

    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    REQUIRE(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/true) == Status::kOk);
    CHECK(player.frameCount() == frameCount);
    CHECK(player.fps() == doctest::Approx(fps));
    CHECK(player.viewport().w == doctest::Approx(64.0f));
    CHECK(player.viewport().h == doctest::Approx(64.0f));

    std::error_code ec;
    fs::remove(path, ec);
}

// T-255b (A10): BakedPlayer::load rejects a frame index at or past the header's frameCount, so
// BakedWriter refuses to write one: the writer cannot produce a file the player refuses.
TEST_CASE("Baked: T-255b writeFrame refuses an index at or past frameCount") {
    fs::path path = xt::test::tempDir() / "xt_test_baked_frame_range.xtcl";
    const CommandList frame = buildFullCommandList(0.0f);
    BakedWriter writer;
    REQUIRE(writer.open(path.string(), 2, 24.0f, Rect{0, 0, 100, 80}, 2.0f));
    REQUIRE(writer.writeFrame(1, frame));
    CHECK_FALSE(writer.writeFrame(2, frame));
    CHECK(std::string(writer.lastError()) == "baked frame index 2 is outside the frame count 2");
    CHECK_FALSE(writer.close());

    BakedWriter empty;
    REQUIRE(empty.open(path.string(), 0, 24.0f, Rect{0, 0, 100, 80}, 2.0f));
    CHECK_FALSE(empty.writeFrame(0, frame));
    CHECK_FALSE(empty.close());

    BakedWriter good;
    REQUIRE(good.open(path.string(), 2, 24.0f, Rect{0, 0, 100, 80}, 2.0f));
    REQUIRE(good.writeFrame(1, frame));
    REQUIRE(good.writeFrame(0, frame));
    REQUIRE(good.close());
    std::vector<uint8_t> bytes = readFile(path.string());
    BakedPlayer player;
    CHECK(player.load(IrBlob{bytes.data(), bytes.size()}, /*takeOwnership=*/false) == Status::kOk);

    std::error_code ec;
    fs::remove(path, ec);
}
