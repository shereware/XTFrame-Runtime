#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/command_list.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/sw/sw_renderer.h"
#include "xt/types.h"

/// T-867 item 4: an inline `<svg>` whose `viewBox` origin is not 0 (`viewBox="-100 -100 200 200"`)
/// maps viewBox (min-x, min-y) to the box's top-left, like SVG: it paints the same pixels as the
/// equivalent origin-0 viewBox with the content moved by `<g transform="translate()">`. Source: a
/// conversion report on 0.19.2 that said the min-x / min-y were ignored: the layout's fit wrote the
/// scene root's translate and dropped the (-min-x, -min-y) the compiler had put there (12.2).

using namespace xt;
using namespace xt::compile;

namespace {

constexpr int kW = 240;
constexpr int kH = 240;

std::vector<uint8_t> compileOk(const std::string& xt) {
    CompileOptions options;
    options.emitMeta = false;
    std::vector<SourceFile> files{{"vb.xt", xt}};
    CompileResult r = compileDocumentFromStrings(files, "vb.xt", options);
    for (const Diagnostic& d : r.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(r.ok());
    return r.ir;
}

std::vector<uint8_t> renderXt(const std::string& xt) {
    const std::vector<uint8_t> ir = compileOk(xt);
    std::unique_ptr<Document> doc;
    REQUIRE(Document::load(IrBlob{ir.data(), ir.size()}, false, nullptr, nullptr, doc) == Status::kOk);
    doc->setViewport(Rect{0, 0, kW, kH});
    doc->update(0.0f);
    CommandList list;
    doc->paint(list);
    std::vector<uint8_t> px(static_cast<size_t>(kW) * kH * 4, 0);
    sw::SwRenderer renderer;
    sw::SwTarget target{px.data(), kW, kH, kW * 4};
    renderer.setTarget(target);
    renderer.clear(Color{1, 1, 1, 1});
    renderer.render(list, nullptr, nullptr);
    return px;
}

/// `box` is the svg element's CSS box; `svgAttrs` its attributes; `content` its children.
std::string docWith(const std::string& box, const std::string& svgAttrs, const std::string& content) {
    return "<xt><head><style>#S { position: absolute; " + box + " }</style></head><body>\n<svg id=\"S\" " +
           svgAttrs + ">" + content + "</svg>\n</body></xt>";
}

bool redAt(const std::vector<uint8_t>& px, int x, int y) {
    const size_t o = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4;
    return px[o] > 200 && px[o + 1] < 60 && px[o + 2] < 60;
}

int differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    int n = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] || a[i + 3] != b[i + 3]) {
            ++n;
        }
    }
    return n;
}

const char* const kShapes =
    R"~(<circle cx="0" cy="0" r="30" fill="#ff0000"/><rect x="40" y="-90" width="40" height="20" fill="#0000ff"/>)~";

}  // namespace

TEST_CASE("T-867 viewBox: a negative origin paints like origin 0 plus translate (1:1 box)~") {
    const std::string box = "left: 20px; top: 20px; width: 200px; height: 200px;";
    const std::vector<uint8_t> neg =
        renderXt(docWith(box, R"~(viewBox="-100 -100 200 200" width="200" height="200")~", kShapes));
    const std::vector<uint8_t> ref =
        renderXt(docWith(box, R"~(viewBox="0 0 200 200" width="200" height="200")~",
                         std::string(R"~(<g transform="translate(100,100)">)~") + kShapes + "</g>"));
    // The circle's center (0,0) in viewBox units is the box center: (20+100, 20+100).
    CHECK(redAt(ref, 120, 120));
    CHECK(redAt(neg, 120, 120));
    CHECK(differing(neg, ref) == 0);
}

TEST_CASE("T-867 viewBox: a positive origin and a scaled box") {
    // viewBox 50 50 100 100 drawn into a 200x200 box: 2x, (50,50) at the top-left.
    const std::string box = "left: 20px; top: 20px; width: 200px; height: 200px;";
    const std::string shapes = R"~(<circle cx="100" cy="100" r="15" fill="#ff0000"/>)~";
    const std::vector<uint8_t> pos = renderXt(docWith(box, R"~(viewBox="50 50 100 100")~", shapes));
    const std::vector<uint8_t> ref =
        renderXt(docWith(box, R"~(viewBox="0 0 100 100")~",
                         std::string(R"~(<g transform="translate(-50,-50)">)~") + shapes + "</g>"));
    CHECK(redAt(ref, 120, 120));
    CHECK(redAt(pos, 120, 120));
    CHECK(differing(pos, ref) == 0);
}

TEST_CASE("T-867 viewBox: a negative origin, letterboxed (the default xMidYMid meet)") {
    // A 2:1 box around a square viewBox: the default fit (06.10 contain = SVG `xMidYMid meet`)
    // centers it horizontally; the origin offset is scaled with the content.
    const std::string box = "left: 0px; top: 20px; width: 240px; height: 120px;";
    const std::vector<uint8_t> neg = renderXt(docWith(box, R"~(viewBox="-100 -100 200 200")~", kShapes));
    const std::vector<uint8_t> ref =
        renderXt(docWith(box, R"~(viewBox="0 0 200 200")~",
                         std::string(R"~(<g transform="translate(100,100)">)~") + kShapes + "</g>"));
    CHECK(redAt(ref, 120, 80));
    CHECK(redAt(neg, 120, 80));
    CHECK(differing(neg, ref) == 0);
}
