#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/utf8_path.h"
#include "xt/context.h"
#include "xt/dds.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/painter.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-199 (Docs/design/26-layers-symbols-library.md §26.3.4, 05 §5.9.2, 22 §22.10): the
/// `border-image` nine-slice (compiler encoding, the Painter's stretch / repeat / round tiles,
/// percentage and `auto` slices), atlas sub-rectangles (`imageSubUv` with object-fit), the `@2x`
/// variant selection of FileAssetResolver and the DDS header reader.

using namespace xt;

namespace {

namespace fs = std::filesystem;

void writeBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

/// Fake decoder: a 1-pixel-high image whose width is the byte count.
bool fakeDecode(const uint8_t* bytes, size_t size, ImageData& out) {
    if (size == 0) {
        return false;
    }
    out.width = static_cast<int>(size);
    out.height = 1;
    out.rgba.assign(size * 4, 255);
    (void)bytes;
    return true;
}

/// A resolver reporting a fixed 40 x 20 image for every asset (no bytes needed).
class FixedSizeResolver : public AssetResolver {
public:
    bool readBytes(std::string_view, std::vector<uint8_t>&) override { return false; }
    bool decodeImage(AssetId, const IrView&, ImageData& out) override {
        out.width = 40;
        out.height = 20;
        out.rgba.assign(40 * 20 * 4, 255);
        return true;
    }
    Vec2 imageSize(AssetId, const IrView&) override { return Vec2{40, 20}; }
};

compile::CompileResult compileXt(const std::string& text) {
    compile::CompileOptions options;
    options.emitMeta = false;
    const compile::SourceFile file{"doc.xt", text};
    return compile::compileDocumentFromStrings(std::span<const compile::SourceFile>(&file, 1), "doc.xt",
                                               options);
}

/// The first alive Element with a border-image (or, `image`, with an `<img>` asset).
const ElementData* findElement(const Document& doc, bool image) {
    const NodeArena& arena = doc.arena();
    for (NodeId n = 0; n < arena.capacity(); ++n) {
        if (!arena.alive(n) || arena.kind(n) != NodeKind::Element) {
            continue;
        }
        const ElementData& data = arena.element(n);
        if ((image && data.imageAsset != kInvalidAsset) ||
            (!image && data.style.borderImage != kInvalidAsset)) {
            return &data;
        }
    }
    return nullptr;
}

NodeId makeBorderImageElement(NodeArena& arena, float w, float h, float slice, uint8_t repeat, uint8_t fill,
                              Rect subUv = Rect{0, 0, 1, 1}) {
    NodeId id = arena.create(NodeKind::Element);
    ElementData& data = arena.element(id);
    data.box.width = w;
    data.box.height = h;
    data.style.borderImage = 0;
    data.style.borderImageRepeat = repeat;
    data.style.borderImageFill = fill;
    data.borderImageSize = Vec2{24, 24};
    for (float& s : data.borderImageSlicePx) {
        s = slice;
    }
    data.borderImageSubUv = subUv;
    return id;
}

size_t countImages(const CommandList& cmds) {
    size_t n = 0;
    for (const Command& c : cmds.commands) {
        n += c.kind == CmdKind::DrawImage ? 1 : 0;
    }
    return n;
}

}  // namespace

TEST_CASE("border-image: the shorthand compiles into the four longhands") {
    compile::CompileResult result = compileXt(
        "<xt><head><style>#a { border-image: url(x.png) 4 5 6 7 fill round; }</style></head>"
        "<body><div id=\"a\"></div></body></xt>");
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(view.ok());
    bool sawSource = false, sawSlice = false, sawRepeat = false, sawFill = false;
    for (const IrDecl& decl : view.value().style().decls()) {
        if (decl.prop == 197) {
            sawSource = view.value().string(irValueU32(decl.value, 0)) == "x.png";
        } else if (decl.prop == 198) {
            sawSlice = irValueF32(decl.value, 0) == 4.0f && irValueF32(decl.value, 1) == 5.0f &&
                       irValueF32(decl.value, 2) == 6.0f && irValueF32(decl.value, 3) == 7.0f;
        } else if (decl.prop == 199) {
            sawRepeat = irValueU32(decl.value, 0) == 2;
        } else if (decl.prop == 200) {
            sawFill = irValueU32(decl.value, 0) == 1;
        }
    }
    CHECK(sawSource);
    CHECK(sawSlice);
    CHECK(sawRepeat);
    CHECK(sawFill);
    // The asset was registered (kind 0) so the runtime can resolve it by name.
    bool asset = false;
    for (const IrAsset& a : view.value().assets().assets()) {
        asset = asset || (a.kind == 0 && view.value().string(a.name) == "x.png");
    }
    CHECK(asset);
}

TEST_CASE("border-image: percentages are negative fractions, no slices = auto, a bad value is rejected") {
    compile::CompileResult result = compileXt(
        "<xt><head><style>#a { border-image: url(x.png) 25% 50%; } #b { border-image: url(x.png); }"
        "</style></head><body><div id=\"a\"></div><div id=\"b\"></div></body></xt>");
    REQUIRE(result.ok());
    Result<IrView> view = IrView::open(IrBlob{result.ir.data(), result.ir.size()});
    REQUIRE(view.ok());
    int sliceDecls = 0;
    bool sawPercent = false, sawAuto = false;
    for (const IrDecl& decl : view.value().style().decls()) {
        if (decl.prop != 198) {
            continue;
        }
        ++sliceDecls;
        const float l = irValueF32(decl.value, 0);
        const float t = irValueF32(decl.value, 2);
        if (l == -0.25f && t == -0.5f) {
            sawPercent = true;
        }
        if (std::isnan(l) && std::isnan(t)) {
            sawAuto = true;
        }
    }
    CHECK(sliceDecls == 2);
    CHECK(sawPercent);
    CHECK(sawAuto);
    compile::CompileResult bad = compileXt(
        "<xt><head><style>#a { border-image: url(x.png) 1 2 3; }</style></head><body><div "
        "id=\"a\"></div></body></xt>");
    bool warned = false;
    for (const compile::Diagnostic& d : bad.diagnostics) {
        warned = warned || d.message.find("border-image") != std::string::npos;
    }
    CHECK(warned);
}

TEST_CASE("border-image painter: stretch draws 8 tiles (9 with fill), inside the box and the cell") {
    for (uint8_t fill = 0; fill <= 1; ++fill) {
        NodeArena arena;
        NodeId root = makeBorderImageElement(arena, 100, 60, 8, 0, fill, Rect{0.5f, 0.0f, 0.25f, 0.25f});
        updateWorldTransforms(arena, root, /*force=*/true);
        Painter painter;
        CommandList cmds;
        painter.paint(arena, root, cmds);
        CHECK(countImages(cmds) == (fill != 0 ? 9u : 8u));
        for (const ImageCmd& ic : cmds.images) {
            CHECK(ic.dst.x >= -0.001f);
            CHECK(ic.dst.y >= -0.001f);
            CHECK(ic.dst.x + ic.dst.w <= 100.001f);
            CHECK(ic.dst.y + ic.dst.h <= 60.001f);
            CHECK(ic.srcUv.x >= 0.5f - 0.001f);
            CHECK(ic.srcUv.x + ic.srcUv.w <= 0.75f + 0.001f);
            CHECK(ic.srcUv.y >= -0.001f);
            CHECK(ic.srcUv.y + ic.srcUv.h <= 0.25f + 0.001f);
        }
        // The top-left corner tile: 8 x 8 at the origin, the first 8/24 of the cell.
        bool corner = false;
        for (const ImageCmd& ic : cmds.images) {
            if (ic.dst.x == 0.0f && ic.dst.y == 0.0f && ic.dst.w == 8.0f && ic.dst.h == 8.0f) {
                corner = std::fabs(ic.srcUv.w - 0.25f * 8.0f / 24.0f) < 1e-4f;
            }
        }
        CHECK(corner);
    }
}

TEST_CASE("border-image painter: repeat tiles the edges, round uses whole tiles") {
    // Middle source tile is 8 px wide; a 100 px box has a 84 px top edge.
    NodeArena arena;
    NodeId root = makeBorderImageElement(arena, 100, 60, 8, 1, 0);
    updateWorldTransforms(arena, root, true);
    Painter painter;
    CommandList cmds;
    painter.paint(arena, root, cmds);
    // 4 corners + top/bottom edges (ceil(84/8) + partial ends = 11 or 12 tiles each) + left/right
    // edges (44 px: 6 or 7 tiles each).
    CHECK(countImages(cmds) > 8);
    float topTiles = 0;
    float topWidth = 0;
    for (const ImageCmd& ic : cmds.images) {
        if (ic.dst.y == 0.0f && ic.dst.h == 8.0f && ic.dst.x > 0.0f && ic.dst.x + ic.dst.w < 100.0f) {
            topTiles += 1;
            topWidth += ic.dst.w;
            CHECK(ic.dst.w <= 8.0f + 1e-4f);
        }
    }
    CHECK(topTiles >= 11);
    CHECK(std::fabs(topWidth - 84.0f) < 1e-3f);

    NodeArena arena2;
    NodeId root2 = makeBorderImageElement(arena2, 100, 60, 8, 2, 0);
    updateWorldTransforms(arena2, root2, true);
    CommandList cmds2;
    painter.paint(arena2, root2, cmds2);
    // round: 84 / 8 = 10.5 -> 11 tiles of 84 / 11 px, no partial source rectangles.
    int roundTiles = 0;
    for (const ImageCmd& ic : cmds2.images) {
        if (ic.dst.y == 0.0f && ic.dst.h == 8.0f && ic.dst.x > 0.0f && ic.dst.x + ic.dst.w < 100.0f) {
            ++roundTiles;
            CHECK(std::fabs(ic.dst.w - 84.0f / 11.0f) < 1e-3f);
            CHECK(std::fabs(ic.srcUv.w - 8.0f / 24.0f) < 1e-4f);
        }
    }
    CHECK(roundTiles == 11);
}

TEST_CASE("border-image: percentage slices resolve against the source size, auto takes the manifest slice") {
    compile::CompileResult result = compileXt(
        "<xt><head><style>"
        "#a { position: absolute; width: 100px; height: 50px; border-image: url(x.png) 25% 50%; }"
        "#b { position: absolute; width: 100px; height: 50px; border-image: url(x.png); }"
        "</style></head><body><div id=\"a\"></div><div id=\"b\"></div></body></xt>");
    REQUIRE(result.ok());
    FixedSizeResolver assets;
    AssetResolver::ImageMeta meta;
    meta.hasSlice = true;
    meta.slice[0] = 3;
    meta.slice[1] = 4;
    meta.slice[2] = 5;
    meta.slice[3] = 6;
    assets.setImageMeta("x.png", meta);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    ctx.update(0.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    const NodeArena& arena = doc->arena();
    int checked = 0;
    for (NodeId n = 0; n < arena.capacity(); ++n) {
        if (!arena.alive(n) || arena.kind(n) != NodeKind::Element) {
            continue;
        }
        const ElementData& data = arena.element(n);
        if (data.style.borderImage == kInvalidAsset) {
            continue;
        }
        CHECK(data.borderImageSize.x == 40.0f);
        CHECK(data.borderImageSize.y == 20.0f);
        if (std::isnan(data.style.borderImageSlice[0])) {
            CHECK(data.borderImageSlicePx[0] == 3.0f);
            CHECK(data.borderImageSlicePx[1] == 4.0f);
            CHECK(data.borderImageSlicePx[2] == 5.0f);
            CHECK(data.borderImageSlicePx[3] == 6.0f);
        } else {
            CHECK(data.borderImageSlicePx[0] == 10.0f);  // 25 % of 40
            CHECK(data.borderImageSlicePx[1] == 10.0f);
            CHECK(data.borderImageSlicePx[2] == 10.0f);  // 50 % of 20
            CHECK(data.borderImageSlicePx[3] == 10.0f);
        }
        ++checked;
    }
    CHECK(checked == 2);
    // The nine-slice replaces the border: the frame is image commands, no border fill. #a's
    // 50 % top/bottom slices leave no middle row, so its left/right edges draw nothing (6 tiles);
    // #b draws 8.
    CommandList list;
    ctx.render(list);
    CHECK(countImages(list) == 14);
}

TEST_CASE("atlas cell: <img> takes imageSubUv from the resolver and object-fit stays inside it") {
    compile::CompileResult result = compileXt(
        "<xt><head><style>#i { position: absolute; width: 80px; height: 20px; object-fit: cover; "
        "}</style></head>"
        "<body><img id=\"i\" src=\"x.png\"/></body></xt>");
    REQUIRE(result.ok());
    FixedSizeResolver assets;
    AssetResolver::ImageMeta meta;
    meta.inAtlas = true;
    meta.subUv = Rect{0.25f, 0.5f, 0.5f, 0.25f};
    assets.setImageMeta("x.png", meta);
    NullTextShaper shaper;
    Context ctx(ContextConfig{&shaper, &assets});
    Result<DocumentHandle> handle = ctx.loadDocument(IrBlob{result.ir.data(), result.ir.size()}, false);
    REQUIRE(handle.ok());
    ctx.setViewport(Rect{0, 0, 200, 100}, 1.0f);
    ctx.update(0.0f);
    Document* doc = ctx.document(handle.value());
    REQUIRE(doc != nullptr);
    const ElementData* img = findElement(*doc, true);
    REQUIRE(img != nullptr);
    CHECK(img->imageSubUv.x == 0.25f);
    CHECK(img->imageSubUv.w == 0.5f);
    CommandList list;
    ctx.render(list);
    REQUIRE(countImages(list) == 1);
    const ImageCmd& ic = list.images[0];
    // cover on a 40x20 (2:1) source into an 80x20 (4:1) box crops the top/bottom halves:
    // the visible vertical fraction is 0.5 of the cell.
    CHECK(ic.srcUv.x == doctest::Approx(0.25f));
    CHECK(ic.srcUv.w == doctest::Approx(0.5f));
    CHECK(ic.srcUv.y == doctest::Approx(0.5f + 0.25f * 0.25f));
    CHECK(ic.srcUv.h == doctest::Approx(0.25f * 0.5f));
}

TEST_CASE("FileAssetResolver: dpr >= 1.5 picks the @2x variant, keeps the 1x size, falls back") {
    const fs::path dir = xt::test::tempDir() / "xt_t199_2x";
    std::error_code ec;
    fs::remove_all(dir, ec);
    writeBytes(dir / "img.png", "abcd");
    writeBytes(dir / "img@2x.png", "abcdefgh");
    writeBytes(dir / "only.png", "xyz");
    compile::IrWriter writer;
    compile::AssetBuilder a;
    a.name = writer.intern("img.png");
    writer.addAsset(a);
    compile::AssetBuilder b;
    b.name = writer.intern("only.png");
    writer.addAsset(b);
    compile::SceneBuilder scene;
    scene.width = 1;
    scene.height = 1;
    writer.addScene(scene);
    std::vector<uint8_t> blob = writer.finish();
    Result<IrView> view = IrView::open(IrBlob{blob.data(), blob.size()});
    REQUIRE(view.ok());
    FileAssetResolver resolver(dir.generic_string(), fakeDecode);
    const ImageData* one = resolver.imageView(0, view.value(), 1.0f);
    const ImageData* two = resolver.imageView(0, view.value(), 2.0f);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    CHECK(one->width == 4);
    CHECK(two->width == 8);
    CHECK(resolver.imageSize(0, view.value()).x == 4.0f);
    CHECK(resolver.imageView(0, view.value(), 1.4f) == one);
    CHECK(resolver.imageView(1, view.value(), 2.0f)->width == 3);  // no variant: the 1x image
    CHECK(resolver.compressedImage(0, view.value(), 1.0f) == nullptr);
    fs::remove_all(dir, ec);
}

TEST_CASE(
    "FileAssetResolver: UTF-8 directory and asset names the ANSI code page can or cannot spell (T-261e)") {
    // Directory xt_t261e_資產, assets 角色頭像.png and アイコン.png (made at run time).
    const fs::path dir = xt::test::tempDir() / compile::pathFromUtf8("xt_t261e_\xE8\xB3\x87\xE7\x94\xA2");
    const std::string cp950 = "\xE8\xA7\x92\xE8\x89\xB2\xE9\xA0\xAD\xE5\x83\x8F.png";
    const std::string kana = "\xE3\x82\xA2\xE3\x82\xA4\xE3\x82\xB3\xE3\x83\xB3.png";
    std::error_code ec;
    fs::remove_all(dir, ec);
    writeBytes(dir / compile::pathFromUtf8(cp950), "abcde");
    writeBytes(dir / compile::pathFromUtf8(kana), "abcdefg");
    compile::IrWriter writer;
    for (const std::string& name : {cp950, kana}) {
        compile::AssetBuilder a;
        a.name = writer.intern(name);
        writer.addAsset(a);
    }
    compile::SceneBuilder scene;
    scene.width = 1;
    scene.height = 1;
    writer.addScene(scene);
    std::vector<uint8_t> blob = writer.finish();
    Result<IrView> view = IrView::open(IrBlob{blob.data(), blob.size()});
    REQUIRE(view.ok());
    FileAssetResolver resolver(compile::genericPathToUtf8(dir), fakeDecode);
    std::vector<uint8_t> bytes;
    CHECK(resolver.readBytes(cp950, bytes));
    CHECK(bytes.size() == 5);
    CHECK(resolver.readBytes(kana, bytes));
    CHECK(bytes.size() == 7);
    ImageData decoded;
    CHECK(resolver.decodeImage(0, view.value(), decoded));
    CHECK(decoded.width == 5);
    CHECK(resolver.decodeImage(1, view.value(), decoded));
    CHECK(decoded.width == 7);
    fs::remove_all(dir, ec);
}

TEST_CASE("DDS header: BC1 / BC3 (FourCC), BC7 (DX10), sizes, truncation") {
    auto header = [](uint32_t w, uint32_t h, const char* fourCc, uint32_t dxgi, size_t dataBytes) {
        std::vector<uint8_t> out(fourCc[0] == 'D' && fourCc[1] == 'X' && fourCc[2] == '1' ? 148 : 128, 0);
        auto put = [&](size_t at, uint32_t v) {
            out[at] = static_cast<uint8_t>(v);
            out[at + 1] = static_cast<uint8_t>(v >> 8);
            out[at + 2] = static_cast<uint8_t>(v >> 16);
            out[at + 3] = static_cast<uint8_t>(v >> 24);
        };
        std::memcpy(out.data(), "DDS ", 4);
        put(4, 124);
        put(8, 0x1 | 0x2 | 0x4 | 0x1000);
        put(12, h);
        put(16, w);
        put(76, 32);
        put(80, 0x4);
        std::memcpy(out.data() + 84, fourCc, 4);
        if (out.size() == 148) {
            put(128, dxgi);
        }
        out.resize(out.size() + dataBytes, 0);
        return out;
    };
    DdsInfo info;
    std::vector<uint8_t> bc1 = header(10, 6, "DXT1", 0, 3 * 2 * 8);
    REQUIRE(parseDdsHeader(bc1.data(), bc1.size(), info));
    CHECK(info.format == DdsFormat::Bc1);
    CHECK(info.width == 10);
    CHECK(info.height == 6);
    CHECK(info.level0Size == 48);
    CHECK(info.dataOffset == 128);
    std::vector<uint8_t> bc3 = header(4, 4, "DXT5", 0, 16);
    REQUIRE(parseDdsHeader(bc3.data(), bc3.size(), info));
    CHECK(info.format == DdsFormat::Bc3);
    std::vector<uint8_t> bc7 = header(8, 8, "DX10", 98, 4 * 16);
    REQUIRE(parseDdsHeader(bc7.data(), bc7.size(), info));
    CHECK(info.format == DdsFormat::Bc7);
    CHECK(info.dataOffset == 148);
    std::vector<uint8_t> truncated = header(8, 8, "DXT1", 0, 3);
    CHECK_FALSE(parseDdsHeader(truncated.data(), truncated.size(), info));
    CHECK(isDds(bc1.data(), bc1.size()));
    CHECK_FALSE(isDds(reinterpret_cast<const uint8_t*>("PNG "), 4));
}

TEST_CASE("atlas names: atlas:<page>#<asset>") {
    CHECK(isAtlasAssetName("atlas:hud-0#assets/abc.png"));
    CHECK_FALSE(isAtlasAssetName("assets/abc.png"));
    CHECK_FALSE(isAtlasAssetName("atlas:nohash"));
    CHECK(atlasAssetPage("atlas:hud-0#assets/abc.png") == "hud-0");
    CHECK(atlasAssetSource("atlas:hud-0#assets/abc.png") == "assets/abc.png");
    CHECK(atlasAssetSource("assets/abc.png") == "assets/abc.png");
}
