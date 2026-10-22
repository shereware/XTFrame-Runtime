// T-405 (Docs/design/43-compatibility-and-versions.md §43.5, 02 §2.2): the frozen `.xtir` fixtures
// of every supported IR minor (Tests/ir/fixtures/v2.<minor>.xtir, each compiled from the same
// Tests/ir/fixtures/v2_source.xt by the compiler of that minor, taken from this repository's git
// history) must keep opening and rendering exactly like the current compiler's output of that
// source; plus the 2.8 rules: hit-slop longhands vs the biased 2.7 encoding, the PORT `nested[]`
// tail being optional, and META's optional `behavior` tail.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/text_shaper.h"

namespace fs = std::filesystem;
using namespace xt;

namespace {

constexpr int kW = 280;
constexpr int kH = 200;

fs::path fixtureDir() { return fs::path(XT_TEST_XT_DIR).parent_path() / "ir" / "fixtures"; }

std::vector<uint8_t> readBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// The current compiler's IR of fixtures/<name>.xt.
std::vector<uint8_t> compileFixture(const char* name) {
    const std::string text = readText(fixtureDir() / name);
    REQUIRE_FALSE(text.empty());
    compile::SourceFile files[1] = {compile::SourceFile{name, text}};
    compile::CompileOptions options;
    compile::CompileResult result = compile::compileDocumentFromStrings(files, name, options);
    for (const compile::Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != compile::Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());
    return result.ir;
}

uint16_t headerMinor(const std::vector<uint8_t>& ir) {
    REQUIRE(ir.size() >= sizeof(IrHeader));
    IrHeader h{};
    std::memcpy(&h, ir.data(), sizeof(h));
    return h.minor;
}

/// One document loaded from `ir` (kept alive by the caller) in its own Context.
struct Loaded {
    NullTextShaper shaper;
    FileAssetResolver assets;
    Context ctx;
    DocumentHandle handle = 0;

    explicit Loaded(const std::vector<uint8_t>& ir)
        : assets(fixtureDir().string(), nullptr), ctx(ContextConfig{&shaper, &assets}) {
        Result<DocumentHandle> loaded =
            ctx.loadDocument(IrBlob{ir.data(), ir.size()}, /*takeOwnership=*/false);
        REQUIRE(loaded.ok());
        handle = loaded.value();
        ctx.setViewport(Rect{0, 0, static_cast<float>(kW), static_cast<float>(kH)}, 1.0f);
    }

    Document& doc() {
        Document* d = ctx.document(handle);
        REQUIRE(d != nullptr);
        return *d;
    }

    /// update(dt) then a full software render.
    std::vector<uint8_t> frame(float dt) {
        ctx.update(dt);
        CommandList list;
        ctx.render(list, nullptr);
        std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4, 0);
        sw::SwRenderer renderer;
        renderer.setTarget(sw::SwTarget{px.data(), kW, kH, kW * 4});
        renderer.clear(Color{1, 1, 1, 1});
        renderer.render(list, nullptr, nullptr);
        return px;
    }
};

/// Two frames (t = 0 and t = 0.25 s into the `slide` animation) after a click on #dot and hp = 80.
std::vector<std::vector<uint8_t>> renderSource(const std::vector<uint8_t>& ir) {
    Loaded m(ir);
    std::vector<std::vector<uint8_t>> frames;
    frames.push_back(m.frame(0.0f));
    const PortId hp = m.doc().lookupPort("hp");
    REQUIRE(hp != kInvalidPort);
    CHECK(m.doc().setPort(hp, Value::ofFloat(80.0f)) == Status::kOk);
    InputEvent down;
    down.kind = InputEvent::PointerDown;
    down.pos = Vec2{64, 110};
    m.doc().dispatchInput(down);
    InputEvent up = down;
    up.kind = InputEvent::PointerUp;
    m.doc().dispatchInput(up);
    frames.push_back(m.frame(0.25f));
    return frames;
}

size_t nonWhitePixels(const std::vector<uint8_t>& px) {
    size_t n = 0;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] != 255 || px[i + 1] != 255 || px[i + 2] != 255) {
            ++n;
        }
    }
    return n;
}

/// Index of the payload of the first chunk `type` in `ir` and its length, or false.
bool findChunk(const std::vector<uint8_t>& ir, uint32_t type, size_t& payload, uint32_t& length) {
    IrHeader h{};
    std::memcpy(&h, ir.data(), sizeof(h));
    size_t off = sizeof(IrHeader);
    for (uint32_t i = 0; i < h.chunkCount && off + sizeof(IrChunkHeader) <= ir.size(); ++i) {
        IrChunkHeader ch{};
        std::memcpy(&ch, ir.data() + off, sizeof(ch));
        if (fourcc(ch.type[0], ch.type[1], ch.type[2], ch.type[3]) == type) {
            payload = off + sizeof(IrChunkHeader);
            length = ch.payloadLength;
            return true;
        }
        off += sizeof(IrChunkHeader) + ch.payloadLength;
    }
    return false;
}

}  // namespace

TEST_CASE("T-405: the frozen fixture of every IR minor opens and renders like the current compiler") {
    const std::vector<uint8_t> current = compileFixture("v2_source.xt");
    CHECK(headerMinor(current) == kIrFormatMinor);
    const std::vector<std::vector<uint8_t>> golden = renderSource(current);
    REQUIRE(golden.size() == 2);
    CHECK(nonWhitePixels(golden[0]) > 1000);
    CHECK(golden[0] != golden[1]);  // The click, the binding and the animation all show.

    for (uint16_t minor = 5; minor <= kIrFormatMinor; ++minor) {
        const std::string name = "v2." + std::to_string(minor) + ".xtir";
        CAPTURE(name);
        const std::vector<uint8_t> ir = readBytes(fixtureDir() / name);
        REQUIRE_FALSE(ir.empty());
        CHECK(headerMinor(ir) == minor);
        IrReadOptions options;
        options.verifyCrc = true;
        Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, options);
        REQUIRE(view.ok());
        CHECK(view.value().formatMinor() == minor);
        CHECK(view.value().meta().behavior() == 0);
        const std::vector<std::vector<uint8_t>> frames = renderSource(ir);
        REQUIRE(frames.size() == golden.size());
        for (size_t f = 0; f < frames.size(); ++f) {
            CAPTURE(f);
            CHECK(frames[f] == golden[f]);
        }
    }
}

TEST_CASE("T-405: a 2.7 file's biased hit-slop and the 2.8 Length longhands hit-test the same") {
    const std::vector<uint8_t> v27 = readBytes(fixtureDir() / "v2.7-hitslop.xtir");
    REQUIRE_FALSE(v27.empty());
    CHECK(headerMinor(v27) == 7);
    const std::vector<uint8_t> v28 = compileFixture("v2_hitslop.xt");

    // 2.8 writes no StyleProp 276 any more, only 277-280 Length declarations.
    Result<IrView> view = IrView::open(IrBlob{v28.data(), v28.size()}, {});
    REQUIRE(view.ok());
    int longhands = 0;
    for (const IrDecl& d : view.value().style().decls()) {
        CHECK(d.prop != 276);
        if (d.prop >= 277 && d.prop <= 280) {
            CHECK(d.valueType == static_cast<uint8_t>(ValueType::Length));
            ++longhands;
        }
    }
    CHECK(longhands == 12);

    Loaded a(v27);
    Loaded b(v28);
    a.frame(0.0f);
    b.frame(0.0f);
    const Vec2 probes[] = {{60, 64},  {60, 42},   {12, 52},   {60, 67},   {210, 48}, {210, 46},
                           {210, 58}, {192, 152}, {248, 152}, {220, 145}, {189, 152}};
    for (const Vec2& p : probes) {
        CAPTURE(p.x);
        CAPTURE(p.y);
        // The id of the element hit ("" for an element without one, e.g. the body).
        auto hitId = [&](Loaded& m) -> std::string {
            const NodeId n = m.doc().hitTest(p);
            if (n == kInvalidNode || m.doc().arena().kind(n) != NodeKind::Element) {
                return std::string();
            }
            const StringId id = m.doc().arena().element(n).id;
            return id != kInvalidString ? std::string(m.doc().string(id)) : std::string();
        };
        CHECK(hitId(a) == hitId(b));
    }
    CHECK(b.doc().hitTest(Vec2{60, 64}) == b.doc().byId("track").node);
    CHECK(b.doc().hitTest(Vec2{192, 152}) == b.doc().byId("em").node);
    for (const char* id : {"track", "pct", "em"}) {
        CAPTURE(id);
        const Value va = a.doc().byId(id).computedStyleValue(276);
        const Value vb = b.doc().byId(id).computedStyleValue(276);
        REQUIRE(va.type == ValueType::Color);
        REQUIRE(vb.type == ValueType::Color);
        CHECK(va.c.r == doctest::Approx(vb.c.r));
        CHECK(va.c.g == doctest::Approx(vb.c.g));
        CHECK(va.c.b == doctest::Approx(vb.c.b));
        CHECK(va.c.a == doctest::Approx(vb.c.a));
    }
    // 1em = 10 px left and right on #em.
    CHECK(b.doc().byId("em").computedStyleValue(276).c.g == doctest::Approx(10.0f));
}

TEST_CASE("T-405: a 2.8 file's StyleProp 276 is ignored; a 2.8 longhand outside px/%/em is invalid") {
    std::vector<uint8_t> ir = compileFixture("v2_hitslop.xt");
    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, {});
    REQUIRE(view.ok());
    // Rewrite the first longhand as Rem (not a hit-slop unit): rejected.
    compile::IrWriter w = compile::IrWriter::fromView(view.value());
    std::vector<uint8_t> clean = w.finish();
    CHECK(clean == ir);
    size_t payload = 0;
    uint32_t length = 0;
    REQUIRE(findChunk(ir, kChunkSTYL, payload, length));
    bool patched = false;
    for (size_t off = payload; off + sizeof(IrDecl) <= payload + length && !patched; off += 4) {
        IrDecl d{};
        std::memcpy(&d, ir.data() + off, sizeof(d));
        if (d.prop == 277 && d.valueType == static_cast<uint8_t>(ValueType::Length) && d.important == 0 &&
            d._pad == 0) {
            ir[off + offsetof(IrDecl, value) + 4] = static_cast<uint8_t>(LengthUnit::Rem);
            patched = true;
        }
    }
    REQUIRE(patched);
    CHECK_FALSE(IrView::open(IrBlob{ir.data(), ir.size()}, {}).ok());
}

TEST_CASE("T-405: the PORT nested[] tail is optional -- missing means no nesting") {
    const std::vector<uint8_t> withTail = compileFixture("v2_tail.xt");
    Result<IrView> view = IrView::open(IrBlob{withTail.data(), withTail.size()}, {});
    REQUIRE(view.ok());
    REQUIRE_FALSE(view.value().ports().nested().empty());

    // The same document without the tail (what a pre-T-391 writer would emit for the fields).
    compile::IrWriter w = compile::IrWriter::fromView(view.value());
    compile::PortBuilder pb;
    pb.ports.assign(view.value().ports().ports().begin(), view.value().ports().ports().end());
    pb.fields.assign(view.value().ports().fields().begin(), view.value().ports().fields().end());
    pb.defaults.assign(view.value().ports().defaults().begin(), view.value().ports().defaults().end());
    w.setPorts(pb);
    const std::vector<uint8_t> noTail = w.finish();
    REQUIRE(noTail.size() < withTail.size());
    Result<IrView> bare = IrView::open(IrBlob{noTail.data(), noTail.size()}, {});
    REQUIRE(bare.ok());
    CHECK(bare.value().ports().nested().empty());
    const uint32_t rowsField = bare.value().ports().ports()[0].fieldOffset + 1;
    CHECK(bare.value().ports().nestedOf(rowsField) == nullptr);

    // Both load and take the outer list; the nested field's entries are kept only with the tail.
    for (const std::vector<uint8_t>* ir : {&withTail, &noTail}) {
        CAPTURE(ir == &noTail);
        Loaded m(*ir);
        auto children = [&] {
            int n = 0;
            for (ElementRef c = m.doc().byId("Rows").firstChild(); c.valid(); c = c.nextSibling()) {
                ++n;
            }
            return n;
        };
        m.frame(0.016f);
        const int before = children();
        const PortId groups = m.doc().lookupPort("groups");
        REQUIRE(groups != kInvalidPort);
        CHECK(m.doc().setPortText(groups, R"([{"name":"A","rows":[{"t":"a0"}]},{"name":"B","rows":[]}])") ==
              Status::kOk);
        m.frame(0.016f);
        m.frame(0.016f);
        CHECK(children() - before == 2);
    }
}

TEST_CASE("T-405: META's behavior tail -- omitted when 0, read back otherwise") {
    std::vector<uint8_t> ir = compileFixture("v2_source.xt");
    size_t payload = 0;
    uint32_t length = 0;
    REQUIRE(findChunk(ir, kChunkMETA, payload, length));
    CHECK(length == sizeof(IrMeta));  // Behavior 0: the 16 bytes an IR minor <= 7 reader requires.

    Result<IrView> view = IrView::open(IrBlob{ir.data(), ir.size()}, {});
    REQUIRE(view.ok());
    CHECK(view.value().meta().behavior() == 0);
    compile::IrWriter w = compile::IrWriter::fromView(view.value());
    const IrMeta& meta = *view.value().meta().meta();
    w.setMeta(meta.sourceFile, meta.compilerVersion, meta.compileTimeUnix, 3);
    const std::vector<uint8_t> withBehavior = w.finish();
    REQUIRE(findChunk(withBehavior, kChunkMETA, payload, length));
    CHECK(length == sizeof(IrMeta) + sizeof(uint32_t));
    Result<IrView> reread = IrView::open(IrBlob{withBehavior.data(), withBehavior.size()}, {});
    REQUIRE(reread.ok());
    CHECK(reread.value().meta().behavior() == 3);
    // Round trip keeps it.
    CHECK(compile::IrWriter::fromView(reread.value()).finish() == withBehavior);
}
