#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "compile_check.h"
#include "damage_internal.h"
#include "doctest.h"
#include "test_temp.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// T-737 (Docs/tasks/T-737.md B): Context::render reuses the previous frame's command hashes when the
/// base is provably the list they came from. Every frame's DamageList must equal computeDamage()'s,
/// which hashes both lists afresh (the forced recompute path), over a copy of the base kept aside.

using namespace xt;

namespace {

constexpr float kViewW = 400;
constexpr float kViewH = 300;

std::vector<uint8_t> compileXt(const std::string& xt, const std::string& rootDir = std::string()) {
    compile::SourceFile files[1] = {compile::SourceFile{"main.xt", xt}};
    compile::CompileOptions options;
    if (!rootDir.empty()) {
        options.rootDir = rootDir;
        options.emitMeta = false;
    }
    compile::CompileResult result = compile::compileDocumentFromStrings(files, "main.xt", options);
    test::requireCompiled(result);
    return result.ir;
}

struct Harness {
    NullTextShaper shaper;
    FileAssetResolver assets;
    Context ctx;
    CommandList list;
    DamageList damage;
    DocumentHandle handle = 0;
    uint32_t compared = 0;

    explicit Harness(const std::vector<uint8_t>& ir)
        : assets(XT_TEST_LAYOUT_DIR, nullptr), ctx(ContextConfig{&shaper, &assets}) {
        Result<DocumentHandle> loaded =
            ctx.loadDocument(IrBlob{ir.data(), ir.size()}, /*takeOwnership=*/true);
        REQUIRE(loaded.ok());
        handle = loaded.value();
        ctx.setViewport(Rect{0, 0, kViewW, kViewH}, 1.0f);
    }

    Document& doc() {
        Document* d = ctx.document(handle);
        REQUIRE(d != nullptr);
        return *d;
    }
    void setStyle(const char* id, const char* declarations) {
        ElementRef ref = doc().byId(id);
        REQUIRE(ref.valid());
        CHECK(ref.setStyle(declarations) == Status::kOk);
    }

    /// One render of `target` (the list handed back) with damage; when `expectBase`, the damage must
    /// be computeDamage(copy of the base, new list).
    void render(CommandList& target, bool expectBase) {
        const CommandList base = target;  // the reference's base, kept aside
        ctx.render(target, &damage);
        if (!expectBase) {
            CHECK(damage.full);
            return;
        }
        DamageList ref;
        computeDamage(base, target, Rect{0, 0, kViewW, kViewH}, ref);
        CHECK(damage.full == ref.full);
        CHECK(damage.viewport == ref.viewport);
        REQUIRE(damage.rects.size() == ref.rects.size());
        for (size_t i = 0; i < ref.rects.size(); ++i) {
            CHECK(damage.rects[i] == ref.rects[i]);
        }
        ++compared;
    }
    void frame(bool expectBase = true, float dt = 1.0f / 60.0f) {
        ctx.update(dt);
        render(list, expectBase);
    }
};

std::string boxes() {
    return "<xt><head><style>"
           "#a { position: absolute; left: 10px; top: 10px; width: 30px; height: 20px; background-color: "
           "#f00; }"
           "#b { position: absolute; left: 300px; top: 240px; width: 30px; height: 20px; background-color: "
           "#00f; }"
           "#c { position: absolute; left: 150px; top: 100px; width: 60px; height: 40px;"
           "     border: 2px dashed #0f0; }"
           "</style></head><body><div id=\"a\"></div><div id=\"b\"></div><div id=\"c\"></div></body></xt>";
}

}  // namespace

TEST_CASE("T-737 damage hash reuse: static, edited, inserted and deleted frames match the recompute") {
    Harness h(compileXt(boxes()));
    h.frame(false);  // first frame: full
    for (int i = 0; i < 3; ++i) {
        h.frame();
        CHECK(h.damage.rects.empty());
    }
    h.setStyle("a", "left: 50px;");  // one node changes
    h.frame();
    CHECK(!h.damage.rects.empty());
    h.frame();
    h.setStyle("b", "display: none;");  // commands deleted
    h.frame();
    CHECK(!h.damage.rects.empty());
    h.setStyle("b", "display: block;");  // and inserted again
    h.frame();
    CHECK(!h.damage.rects.empty());
    h.setStyle("c", "border-top-width: 5px;");
    h.frame();
    h.frame();
    CHECK(h.compared == 9);
}

TEST_CASE("T-737 damage hash reuse: a reload, a copied list and a list kept aside") {
    std::vector<uint8_t> ir = compileXt(boxes());
    Harness h(ir);
    h.frame(false);
    h.frame();

    SUBCASE("reload") {
        std::vector<uint8_t> again = ir;
        REQUIRE(h.ctx.reloadDocument(h.handle, IrBlob{again.data(), again.size()}, true).ok());
        h.frame(false);  // a reload forces `full`
        h.frame();
        h.setStyle("a", "left: 60px;");
        h.frame();
    }
    SUBCASE("the list copied into another object") {
        CommandList copy = h.list;  // same serial, other storage: hashed afresh
        h.setStyle("a", "left: 70px;");
        h.ctx.update(1.0f / 60.0f);
        h.render(copy, true);
        h.ctx.update(1.0f / 60.0f);
        h.render(copy, true);
        h.list = copy;
        h.frame();
    }
    SUBCASE("a list that is not the last one") {
        CommandList older = h.list;
        h.frame();
        h.ctx.update(1.0f / 60.0f);
        h.render(older, false);  // not this Context's previous list: full
        h.ctx.update(1.0f / 60.0f);
        h.render(older, true);
    }
    SUBCASE("a frame without damage in between") {
        h.ctx.update(1.0f / 60.0f);
        h.ctx.render(h.list, nullptr);
        h.frame(false);
        h.frame();
    }
}

TEST_CASE("T-737 damage hash reuse: two Contexts rendered in turn keep their own hashes") {
    std::vector<uint8_t> ir = compileXt(boxes());
    Harness one(ir);
    Harness two(ir);
    one.frame(false);
    two.frame(false);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        if (i == 1) {
            one.setStyle("a", "left: 80px;");
        }
        if (i == 2) {
            two.setStyle("b", "top: 10px;");
        }
        one.frame();
        two.frame();
    }
    CHECK(one.compared == 4);
    CHECK(two.compared == 4);
}

TEST_CASE("T-737 damage hash reuse: the host renders the same frame twice") {
    Harness h(compileXt(boxes()));
    h.frame(false);
    h.setStyle("a", "left: 90px;");
    h.ctx.update(1.0f / 60.0f);
    h.render(h.list, true);
    CHECK(!h.damage.rects.empty());
    h.render(h.list, true);  // no update in between
    CHECK(h.damage.rects.empty());
    h.frame();
}

TEST_CASE("T-737 damage hash reuse: a custom shader's clock moving with no command changed") {
    const std::filesystem::path dir = xt::test::tempDir() / "xt_t737_shader";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "ripple.xtfx", std::ios::binary);
        out << "name: Ripple\nparam amplitude float 4 [0, 32]\nexpand: 8px\n---\n"
               "float4 xtMain(XtInput i)\n{\n"
               "    float2 uv = i.uv + sin(i.uv.yx * 40 + xtTime) * (amplitude * i.texelSize);\n"
               "    return xtSample(uv);\n}\n";
    }
    Harness h(
        compileXt("<xt><head><style>#a { position: absolute; left: 20px; top: 20px; width: 40px;"
                  " height: 40px; background-color: #f00; effect: custom(\"ripple.xtfx\"); }"
                  "#b { position: absolute; left: 200px; top: 150px; width: 40px; height: 40px;"
                  " background-color: #00f; }</style></head>"
                  "<body><div id=\"a\"></div><div id=\"b\"></div></body></xt>",
                  dir.string()));
    h.frame(false);
    REQUIRE(!h.list.shaders.empty());
    for (int i = 0; i < 3; ++i) {
        h.frame();  // the clock moves: the effect's bounds are dirty though no command changed
        CHECK(!h.damage.rects.empty());
    }
    h.setStyle("b", "left: 220px;");
    h.frame();
    h.frame(true, 0.0f);  // the clock stands still
    CHECK(h.damage.rects.empty());
}
