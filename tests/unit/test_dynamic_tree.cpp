#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/document.h"
#include "xt/event.h"
#include "xt/input.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/text_shaper.h"
#include "xt/types.h"

/// Document::createElement/appendChild/insertBefore/remove: tree edits, the layout that follows
/// them, and remove()'s cleanup (listeners, focus/hover, `anim` scene instances, animations).
/// See docs/design/13-api.md §13.2, docs/tasks/T-35.md "remove".

using namespace xt;
using namespace xt::compile;

namespace {

std::string layoutPath(const std::string& name) { return std::string(XT_TEST_LAYOUT_DIR) + "/" + name; }

std::unique_ptr<Document> loadDoc(const std::string& name, TextShaper* shaper, AssetResolver* assets) {
    CompileOptions options;
    CompileResult result = compileDocument(layoutPath(name), options);
    for (const Diagnostic& d : result.diagnostics) {
        CAPTURE(d.message);
        CHECK(d.level != Diagnostic::Level::Error);
    }
    REQUIRE(result.ok());

    std::unique_ptr<Document> doc;
    Status status = Document::load(IrBlob{result.ir.data(), result.ir.size()}, true, shaper, assets, doc);
    REQUIRE(status == Status::kOk);
    doc->setViewport(Rect{0, 0, 400, 300});
    doc->update(0.0f);
    return doc;
}

/// The ids of `parent`'s Element children, in order ("" for an element without one).
std::vector<std::string> childIds(ElementRef parent) {
    std::vector<std::string> ids;
    for (ElementRef c = parent.firstChild(); c.valid(); c = c.nextSibling()) {
        ids.emplace_back(c.id());
    }
    return ids;
}

InputEvent pointerMove(Vec2 pos) {
    InputEvent ev;
    ev.kind = InputEvent::PointerMove;
    ev.pos = pos;
    return ev;
}

}  // namespace

TEST_CASE("createElement: detached, interned tag, tag-derived focusability") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("dynamic_tree.xt", &shaper, &assets);

    ElementRef made = doc->createElement("div");
    REQUIRE(made.valid());
    CHECK(made.tag() == "div");
    CHECK_FALSE(made.parent().valid());
    CHECK(doc->arena().element(made.node).focusable == 0);

    ElementRef button = doc->createElement("button");
    CHECK(doc->arena().element(button.node).focusable == 1);

    // A tag that does not appear in the IR string table interns into the runtime pool.
    ElementRef exotic = doc->createElement("mywidget");
    CHECK(exotic.tag() == "mywidget");

    doc->remove(made);
    doc->remove(button);
    doc->remove(exotic);
}

TEST_CASE("appendChild / insertBefore: order, layout and preconditions") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("dynamic_tree.xt", &shaper, &assets);

    ElementRef host = doc->byId("host");
    ElementRef first = doc->byId("first");
    REQUIRE(host.valid());
    REQUIRE(first.valid());
    CHECK(childIds(host) == std::vector<std::string>{"first"});

    ElementRef last = doc->createElement("div");
    CHECK(last.setAttribute("id", "last") == Status::kOk);
    doc->appendChild(host, last);
    ElementRef middle = doc->createElement("div");
    CHECK(middle.setAttribute("id", "middle") == Status::kOk);
    doc->insertBefore(host, middle, last);
    CHECK(childIds(host) == std::vector<std::string>{"first", "middle", "last"});

    // The new children take part in layout (`div { height: 10px }`).
    doc->update(0.0f);
    CHECK(middle.layoutBox().y == doctest::Approx(10));
    CHECK(last.layoutBox().y == doctest::Approx(20));
    CHECK(middle.layoutBox().height == doctest::Approx(10));

    // Preconditions: an already-attached child, an invalid ref, a `before` that is not a child.
    doc->appendChild(host, first);  // No-op: `first` is attached.
    CHECK(childIds(host) == std::vector<std::string>{"first", "middle", "last"});
    doc->appendChild(host, ElementRef{});
    doc->appendChild(ElementRef{}, last);
    ElementRef orphan = doc->createElement("div");
    doc->insertBefore(host, orphan, doc->byId("btn"));  // `btn` is not a child of `host`.
    CHECK_FALSE(orphan.parent().valid());
    doc->remove(orphan);
}

TEST_CASE("remove: subtree, listeners and refs") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("dynamic_tree.xt", &shaper, &assets);

    ElementRef host = doc->byId("host");
    ElementRef first = doc->byId("first");
    REQUIRE(host.valid());
    uint32_t aliveBefore = doc->arena().aliveCount();

    ElementRef child = doc->createElement("div");
    doc->appendChild(host, child);
    ElementRef grandChild = doc->createElement("div");
    doc->appendChild(child, grandChild);
    grandChild.setText("hello");
    CHECK(doc->arena().aliveCount() == aliveBefore + 3);  // 2 elements + 1 text node.

    int clicks = 0;
    doc->addListener(grandChild, EventType::Click, [&clicks](Event&) { ++clicks; });

    doc->remove(child);
    CHECK_FALSE(child.valid());
    CHECK_FALSE(grandChild.valid());
    CHECK(doc->arena().aliveCount() == aliveBefore);
    CHECK(first.valid());  // Siblings are untouched.
    doc->update(0.1f);
    CHECK(clicks == 0);

    // Removing the document root leaves an empty document that still updates/paints.
    doc->remove(doc->root());
    CHECK_FALSE(doc->root().valid());
    doc->update(0.2f);
    CHECK(doc->arena().aliveCount() == 0);
}

TEST_CASE("remove: the focused element gets its Blur, the hovered one its Leave") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("dynamic_tree.xt", &shaper, &assets);

    ElementRef btn = doc->byId("btn");
    REQUIRE(btn.valid());
    int blurs = 0;
    doc->addListener(btn, EventType::Blur, [&blurs](Event&) { ++blurs; });
    btn.focus();
    REQUIRE(doc->focus().node == btn.node);

    doc->remove(btn);
    CHECK(blurs == 1);
    CHECK_FALSE(doc->focus().valid());

    ElementRef host = doc->byId("host");
    REQUIRE(host.valid());
    int leaves = 0;
    doc->addListener(host, EventType::PointerLeave, [&leaves](Event&) { ++leaves; });
    doc->dispatchInput(pointerMove(Vec2{5, 5}));
    REQUIRE(host.hasState(kStateHover));
    doc->remove(host);
    CHECK(leaves == 1);
    doc->dispatchInput(pointerMove(Vec2{6, 6}));  // Must not touch the destroyed hover node.
    doc->update(0.1f);
}

TEST_CASE("remove: an `anim` element drops its scene subtree and stops its animation") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("dynamic_tree.xt", &shaper, &assets);

    ElementRef anim = doc->byId("a");
    REQUIRE(anim.valid());
    uint32_t instance = anim.defaultAnimation();
    REQUIRE(instance != 0);
    CHECK(doc->isPlaying(instance));
    NodeId sceneRoot = doc->arena().element(anim.node).sceneRoot;
    REQUIRE(sceneRoot != kInvalidNode);
    REQUIRE(doc->arena().alive(sceneRoot));
    CHECK(doc->sceneInstance(anim) != nullptr);

    doc->remove(anim);
    CHECK_FALSE(anim.valid());
    CHECK_FALSE(doc->arena().alive(sceneRoot));
    CHECK_FALSE(doc->isPlaying(instance));
    doc->update(0.1f);  // Animator must not evaluate the stopped instance onto freed slots.
}

TEST_CASE("remove: 100 created elements come and go without leaks") {
    NullTextShaper shaper;
    FileAssetResolver assets(XT_TEST_LAYOUT_DIR, nullptr);
    std::unique_ptr<Document> doc = loadDoc("dynamic_tree.xt", &shaper, &assets);

    ElementRef host = doc->byId("host");
    REQUIRE(host.valid());
    uint32_t aliveBefore = doc->arena().aliveCount();
    uint32_t transitionsBefore = doc->animator().transitionCount();

    std::vector<ElementRef> made;
    int fired = 0;
    for (int i = 0; i < 100; ++i) {
        ElementRef ref = doc->createElement("div");
        CHECK(ref.setAttribute("class", "item") == Status::kOk);
        doc->appendChild(host, ref);
        doc->addListener(ref, EventType::Click, [&fired](Event&) { ++fired; });
        made.push_back(ref);
    }
    doc->update(0.1f);
    std::vector<ElementRef> found;
    CHECK(doc->query("#host > .item", found) == Status::kOk);
    CHECK(found.size() == 101);  // The fixture's own #first carries `class="item"` too.
    CHECK(doc->arena().aliveCount() == aliveBefore + 100);

    for (ElementRef& ref : made) {
        doc->remove(ref);
    }
    doc->update(0.2f);
    CHECK(doc->arena().aliveCount() == aliveBefore);
    CHECK(doc->animator().transitionCount() == transitionsBefore);
    CHECK(doc->query("#host > .item", found) == Status::kOk);
    CHECK(found.size() == 1);
    CHECK(fired == 0);
    for (const ElementRef& ref : made) {
        CHECK_FALSE(ref.valid());
    }
}
