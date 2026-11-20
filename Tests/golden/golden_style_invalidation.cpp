#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "golden_common.h"
#include "style_invalidation.h"  // Code/Runtime/core/src: StyleInvalidationTestAccess.
#include "xt/asset_resolver.h"
#include "xt/compile/compile.h"
#include "xt/context.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/input.h"
#include "xt/ir.h"
#include "xt/node.h"
#include "xt/node_arena.h"
#include "xt/property.h"
#include "xt/sw/sw_png.h"
#include "xt/text_shaper.h"
#ifdef XT_GOLDEN_TEXT
#include "xt/text/ft_text_shaper.h"
#endif
#include "xt/types.h"
#include "xt/value.h"

/// T-723 (Docs/design/10-backends.md §10.16 W5, Docs/tasks/T-723.md part C): the style
/// invalidation equivalence check. Every UI fixture of Tests/golden/ui/manifest.txt and the
/// Examples UIs other tests already load is opened twice, once with the narrow style invalidation
/// (the default) and once with the old full-range one (StyleInvalidationTestAccess::setLegacy),
/// and both Contexts get the same synthetic interaction: the fixture's own event script, the
/// pointer along both diagonals and along a grid over the whole viewport, a press and release on
/// every interactive element, Tab through every focusable element, and every scalar `in` port
/// written to two different values, the second one again, then back. After every step the
/// computed style of every element must be identical in both (floats compared bit for bit).
/// T-763 adds the state writes outside the dispatcher (stateWrites(): ElementRef::setState,
/// Document::setCustomState, removeClass / addClass, restoreState) before the ports.
///
/// Not covered by the sheet (choices made here):
///   - "Interactive element": focusable (button, input, tabindex), carrying a listener, or with a
///     computed `cursor: pointer`; pressed at its layout-box center (transforms ignored), at most
///     64 per UI. Tab is pressed (focusable count + 1) times, at most 64.
///   - The grid is 8 rows of 16 points walked back and forth; each diagonal has 24 points; every
///     step runs update(1/60) on both Contexts, so transitions advance the same way in both.
///   - Port values: Number 37.5 / -3, Bool true / false, Color #ff0000 / #00ff80, Vec2 / Vec3
///     component lists, String / Texture "A" / "BB", Enum its first / last value name; Entity,
///     Struct, List, Event and `out` / `call` ports are not written. "Back" is the port's text
///     before the first write (skipped when the port has no text form).
///   - A fixture that does not compile or load is a failure, like in the image check.
///   - Examples UIs are drawn at 1280 x 720.

namespace xt::golden {

namespace {

namespace fs = std::filesystem;

struct EquivCase {
    std::string name;
    fs::path xtPath;
    int width = 0;
    int height = 0;
    std::string eventsPath;  // Absolute, or empty.
    std::vector<std::string> assetRoots;
};

/// The Examples UIs other tests load (Tests/golden/ui/manifest.txt's game-ui HUD comes with the
/// manifest): the game-ui and hud-overlay bundles (Tests/capi), the world-layers stage
/// (Tests/gpu, Tests/capi), the desktop app (Tests/flow) and the npdo project (Tests/capi).
constexpr const char* kExampleUis[] = {
    "demos/game-ui/menus/pause.xt",
    "demos/game-ui/sceneview/sceneview.xt",
    "demos/hud-overlay/hud/hud.xt",
    "demos/world-layers/stage/stage.xt",
    "desktop_app/ui/app.xt",
    "demos/npdo/ui-project/hud/hud.xt",
    "demos/npdo/ui-project/main-menu/main-menu.xt",
    "demos/npdo/ui-project/settings/settings.xt",
    "demos/npdo/ui-project/zero/zero.xt",
};

constexpr float kDt = 1.0f / 60.0f;
constexpr size_t kMaxPresses = 64;

uint32_t floatBits(float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

bool sameFloat(float a, float b) { return floatBits(a) == floatBits(b); }

bool sameColor(const Color& a, const Color& b) {
    return sameFloat(a.r, b.r) && sameFloat(a.g, b.g) && sameFloat(a.b, b.b) && sameFloat(a.a, b.a);
}

bool sameValue(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case ValueType::Float:
            return sameFloat(a.f, b.f);
        case ValueType::Vec2:
            return sameFloat(a.v2.x, b.v2.x) && sameFloat(a.v2.y, b.v2.y);
        case ValueType::Vec3:
            return sameFloat(a.v3.x, b.v3.x) && sameFloat(a.v3.y, b.v3.y) && sameFloat(a.v3.z, b.v3.z);
        case ValueType::Color:
            return sameColor(a.c, b.c);
        case ValueType::Length:
            return a.len.unit == b.len.unit && sameFloat(a.len.value, b.len.value);
        default:
            return valueEquals(a, b);
    }
}

/// A short text of `v` for a failure message.
std::string describe(const Value& v) {
    char buf[96];
    switch (v.type) {
        case ValueType::Float:
            std::snprintf(buf, sizeof(buf), "float %.9g", static_cast<double>(v.f));
            break;
        case ValueType::Color:
            std::snprintf(buf, sizeof(buf), "color %.9g %.9g %.9g %.9g", static_cast<double>(v.c.r),
                          static_cast<double>(v.c.g), static_cast<double>(v.c.b), static_cast<double>(v.c.a));
            break;
        case ValueType::Length:
            std::snprintf(buf, sizeof(buf), "length %.9g unit %u", static_cast<double>(v.len.value),
                          static_cast<unsigned>(v.len.unit));
            break;
        default:
            std::snprintf(buf, sizeof(buf), "type %u", static_cast<unsigned>(v.type));
            break;
    }
    return buf;
}

/// The first ComputedStyle field that differs between `a` and `b`, or nullptr when they are equal.
const char* firstStyleDifference(const ComputedStyle& a, const ComputedStyle& b) {
#define XT_SAME(field)           \
    if (!(a.field == b.field)) { \
        return #field;           \
    }
#define XT_SAME_F(field)                \
    if (!sameFloat(a.field, b.field)) { \
        return #field;                  \
    }
    XT_SAME(display)
    XT_SAME(position)
    XT_SAME(flexDir)
    XT_SAME(flexWrap)
    XT_SAME(justify)
    XT_SAME(alignItems)
    XT_SAME(alignSelf)
    XT_SAME(alignContent)
    XT_SAME(overflowX)
    XT_SAME(overflowY)
    XT_SAME_F(zIndex)
    XT_SAME(visibility)
    XT_SAME(pointerEvents)
    XT_SAME(backfaceHidden)
    XT_SAME(worldLayer)
    XT_SAME(pointerWatch)
    XT_SAME(focusNav)
    XT_SAME(layerOverlay)
    XT_SAME(fontFamilyOffset)
    XT_SAME(fontFamilyCount)
    XT_SAME(fontWeight)
    XT_SAME(fontStyle)
    XT_SAME(textAlign)
    XT_SAME(whiteSpace)
    XT_SAME(textOverflow)
    XT_SAME(textTransform)
    XT_SAME(lineClamp)
    XT_SAME(fontVariantNumeric)
    XT_SAME(gridColumns)
    XT_SAME(gridRows)
    XT_SAME(gridAreas)
    XT_SAME(justifyItems)
    for (int i = 0; i < 4; ++i) {
        XT_SAME(gridLines[i])
        XT_SAME_F(hitSlop[i])
        XT_SAME_F(borderImageSlice[i])
        XT_SAME(borderStyle[i])
    }
    XT_SAME(gridAreaName)
    XT_SAME(uiScaleMode)
    XT_SAME(mediaOverride)
    XT_SAME(appRegion)
    XT_SAME_F(uiScaleMin)
    XT_SAME_F(uiScaleMax)
    XT_SAME_F(uiScaleEff)
    XT_SAME(cursor)
    XT_SAME(objectFit)
    XT_SAME(direction)
    XT_SAME(writingMode)
    XT_SAME(textOrientation)
    XT_SAME(textRendering)
    XT_SAME(lang)
    XT_SAME(transitionOffset)
    XT_SAME(transitionCount)
    XT_SAME(animationOffset)
    XT_SAME(animationCount)
    XT_SAME(effectOffset)
    XT_SAME(effectCount)
    XT_SAME(gradientOffset)
    XT_SAME(gradientCount)
    XT_SAME(textEffectOffset)
    XT_SAME(textEffectCount)
    XT_SAME(backgroundImage)
    XT_SAME(borderImage)
    XT_SAME(borderImageRepeat)
    XT_SAME(borderImageFill)
    XT_SAME(outlineStyle)
    XT_SAME(borderColorSides)
    XT_SAME(textDecorationLine)
    XT_SAME(backgroundRepeat)
    XT_SAME(imageRendering)
    XT_SAME(textDecorations)
    XT_SAME(inheritedDecorations)
    if (!sameColor(a.inheritedDecorationColor, b.inheritedDecorationColor)) {
        return "inheritedDecorationColor";
    }
    XT_SAME_F(inheritedDecorationThickness)
    XT_SAME_F(decoAscent)
    XT_SAME_F(decoDescent)
    XT_SAME_F(decoUnderlineOffset)
    XT_SAME_F(decoUnderlineThickness)
    XT_SAME(mixBlendMode)
    XT_SAME(clipShape)
    XT_SAME(resolvedFont)
    XT_SAME(lastAnimationOffset)
    XT_SAME(lastAnimationCount)
    XT_SAME_F(clipStart)
    XT_SAME_F(clipRate)
#undef XT_SAME
#undef XT_SAME_F
    for (PropId p = 0; p < prop::kElementPropCount; ++p) {
        if (!sameValue(a.base[p], b.base[p])) {
            static thread_local std::string name;
            name = "base[" + std::to_string(p) + "] (" + describe(a.base[p]) + " vs " + describe(b.base[p]) +
                   ")";
            return name.c_str();
        }
    }
    return nullptr;
}

/// One opened copy of a UI: its own shaper, resolver and Context, so the two copies share no state.
struct Instance {
#ifdef XT_GOLDEN_TEXT
    xt::text::FtTextShaper shaper;
#else
    xt::NullTextShaper shaper;
#endif
    std::unique_ptr<FileAssetResolver> resolver;
    std::unique_ptr<Context> ctx;
    Document* doc = nullptr;
};

bool openInstance(const EquivCase& ec, const std::vector<uint8_t>& ir, bool legacy, Instance& out) {
    std::vector<std::string> roots{ec.xtPath.parent_path().string()};
    roots.insert(roots.end(), ec.assetRoots.begin(), ec.assetRoots.end());
    out.resolver = std::make_unique<FileAssetResolver>(std::move(roots), xt::sw::decodeImageBytes);
    out.ctx = std::make_unique<Context>(ContextConfig{&out.shaper, out.resolver.get()});
    out.ctx->setViewport(Rect{0, 0, static_cast<float>(ec.width), static_cast<float>(ec.height)}, 1.0f);
    Result<DocumentHandle> handle =
        out.ctx->loadDocument(IrBlob{ir.data(), ir.size()}, /*takeOwnership=*/true);
    if (!handle.ok()) {
        return false;
    }
    out.doc = out.ctx->document(handle.value());
    if (out.doc == nullptr) {
        return false;
    }
    StyleInvalidationTestAccess::setLegacy(*out.doc, legacy);
    return true;
}

/// Runs both instances through the same steps and compares them after each one.
class Pair {
public:
    Pair(Instance& narrow, Instance& legacy) : m_narrow(narrow), m_legacy(legacy) {}

    /// Compares every element; reports the first difference. False on a difference.
    bool compare(const std::string& step) {
        ++m_steps;
        const NodeArena& a = m_narrow.doc->arena();
        const NodeArena& b = m_legacy.doc->arena();
        if (a.capacity() != b.capacity()) {
            return fail(step, "node capacity " + std::to_string(a.capacity()) + " vs " +
                                  std::to_string(b.capacity()));
        }
        for (NodeId n = 0; n < a.capacity(); ++n) {
            if (a.alive(n) != b.alive(n)) {
                return fail(step, "node " + std::to_string(n) + " alive in one copy only");
            }
            if (!a.alive(n) || a.kind(n) != NodeKind::Element) {
                continue;
            }
            if (b.kind(n) != NodeKind::Element) {
                return fail(step, "node " + std::to_string(n) + " kind differs");
            }
            const char* field = firstStyleDifference(a.element(n).style, b.element(n).style);
            if (field != nullptr) {
                const StringId id = a.element(n).id;
                const std::string idText = id != kInvalidString ? std::string(m_narrow.doc->string(id)) : "";
                return fail(step, "element " + std::to_string(n) + (idText.empty() ? "" : " #" + idText) +
                                      ": computed style differs in " + field);
            }
        }
        return true;
    }

    /// The same input event to both, then update(kDt) on both, then compare.
    bool input(const InputEvent& ev, const std::string& step) {
        m_narrow.ctx->pushInput(ev);
        m_legacy.ctx->pushInput(ev);
        return update(kDt, step);
    }

    bool update(float dt, const std::string& step) {
        m_narrow.ctx->update(dt);
        m_legacy.ctx->update(dt);
        return compare(step);
    }

    bool pointer(InputEvent::Kind kind, float x, float y, const std::string& step) {
        InputEvent ev;
        ev.kind = kind;
        ev.pos = Vec2{x, y};
        ev.button = PointerButton::Left;
        return input(ev, step);
    }

    bool key(KeyCode code, const std::string& step) {
        InputEvent down;
        down.kind = InputEvent::KeyDown;
        down.key = code;
        if (!input(down, step + " down")) {
            return false;
        }
        InputEvent up;
        up.kind = InputEvent::KeyUp;
        up.key = code;
        return input(up, step + " up");
    }

    /// The same port text to both, update(kDt), compare. A text the port rejects in one copy only
    /// is a difference; rejected by both, the step is still compared.
    /// T-763: the same write to `node` of both documents (the NodeIds are equal: compare() checks
    /// it), update(kDt), compare.
    template <class Fn>
    bool write(NodeId node, Fn&& fn, const std::string& step) {
        for (Instance* inst : {&m_narrow, &m_legacy}) {
            Document& doc = *inst->doc;
            if (doc.arena().alive(node) && doc.arena().kind(node) == NodeKind::Element) {
                fn(doc, ElementRef{&doc, node, doc.arena().generation(node)});
            }
        }
        return update(kDt, step);
    }

    /// T-763: Document::snapshotState() of both, for restore().
    void snapshot() {
        m_snapNarrow = m_narrow.doc->snapshotState();
        m_snapLegacy = m_legacy.doc->snapshotState();
    }
    /// Document::restoreState() of each one's snapshot, update(kDt), compare.
    bool restore(const std::string& step) {
        const Status sa = m_narrow.doc->restoreState(m_snapNarrow);
        const Status sb = m_legacy.doc->restoreState(m_snapLegacy);
        if (sa != sb) {
            return fail(step, "restoreState status differs");
        }
        return update(kDt, step);
    }

    bool portText(PortId port, const std::string& text, const std::string& step) {
        const Status sa = m_narrow.doc->setPortText(port, text);
        const Status sb = m_legacy.doc->setPortText(port, text);
        if (sa != sb) {
            return fail(step, "setPortText status differs");
        }
        return update(kDt, step);
    }

    uint32_t steps() const { return m_steps; }
    const std::string& failure() const { return m_failure; }

private:
    bool fail(const std::string& step, const std::string& what) {
        m_failure = "after step '" + step + "' (" + std::to_string(m_steps) + "): " + what;
        return false;
    }

    Instance& m_narrow;
    Instance& m_legacy;
    uint32_t m_steps = 0;
    std::string m_failure;
    std::string m_snapNarrow;
    std::string m_snapLegacy;
};

/// `node`'s layout-box center in viewport coordinates, from the layout alone.
Vec2 layoutCenter(const NodeArena& arena, NodeId node) {
    const LayoutBox& self = arena.element(node).box;
    Vec2 p{self.width * 0.5f, self.height * 0.5f};
    for (NodeId n = node; n != kInvalidNode && arena.alive(n) && arena.kind(n) == NodeKind::Element;) {
        const LayoutBox& box = arena.element(n).box;
        p.x += box.x;
        p.y += box.y;
        const NodeId parent = arena.parent(n);
        if (parent != kInvalidNode && arena.alive(parent) && arena.kind(parent) == NodeKind::Element) {
            const Rect content = arena.element(parent).box.contentBox();
            p.x += content.x;
            p.y += content.y;
        }
        n = parent;
    }
    return p;
}

std::vector<std::string> portValues(const PortRecord& rec) {
    switch (rec.type) {
        case PortValueType::Number:
            return {"37.5", "-3"};
        case PortValueType::Bool:
            return {"true", "false"};
        case PortValueType::Color:
            return {"#ff0000", "#00ff80"};
        case PortValueType::Vec2:
            return {"3,4", "-12,40"};
        case PortValueType::Vec3:
            return {"3,4,5", "-12,40,1"};
        case PortValueType::String:
        case PortValueType::Texture:
            return {"A", "BB"};
        case PortValueType::Enum:
            if (rec.enumValues.size() >= 2) {
                return {rec.enumValues.front(), rec.enumValues.back()};
            }
            return {};
        default:
            return {};
    }
}

/// T-763: the state-write phase of interact(). Up to kMaxStateTargets elements spread evenly over
/// the document order; on each, Disabled and Checked on and off, each of the first
/// kMaxCustomStateNames custom states on and off, its first class removed and added back; then
/// restoreState() to the snapshot taken before.
constexpr size_t kMaxStateTargets = 16;
constexpr size_t kMaxCustomStateNames = 3;

bool stateWrites(Instance& narrow, Pair& pair) {
    const NodeArena& arena = narrow.doc->arena();
    std::vector<NodeId> elements;
    for (NodeId n = 0; n < arena.capacity(); ++n) {
        if (arena.alive(n) && arena.kind(n) == NodeKind::Element) {
            elements.push_back(n);
        }
    }
    std::vector<NodeId> targets;
    const size_t stride = std::max<size_t>(1, elements.size() / kMaxStateTargets);
    for (size_t i = 0; i < elements.size() && targets.size() < kMaxStateTargets; i += stride) {
        targets.push_back(elements[i]);
    }
    std::vector<std::string> customNames;
    for (uint32_t id : narrow.doc->ir().customStates().names()) {
        if (customNames.size() < kMaxCustomStateNames) {
            customNames.emplace_back(narrow.doc->ir().string(id));
        }
    }
    pair.snapshot();
    for (NodeId n : targets) {
        const std::string tag = "state node " + std::to_string(n);
        for (const ElementState state : {kStateDisabled, kStateChecked}) {
            const std::string name = state == kStateDisabled ? " disabled" : " checked";
            for (const bool on : {true, false}) {
                if (!pair.write(
                        n, [&](Document&, ElementRef el) { el.setState(state, on); },
                        tag + name + (on ? " on" : " off"))) {
                    return false;
                }
            }
        }
        for (const std::string& custom : customNames) {
            for (const bool on : {true, false}) {
                if (!pair.write(
                        n, [&](Document& doc, ElementRef el) { doc.setCustomState(el, custom, on); },
                        tag + " :state(" + custom + (on ? ") on" : ") off"))) {
                    return false;
                }
            }
        }
        if (!narrow.doc->arena().alive(n) || narrow.doc->arena().kind(n) != NodeKind::Element ||
            narrow.doc->arena().element(n).classes.empty()) {
            continue;
        }
        const std::string cls(narrow.doc->string(narrow.doc->arena().element(n).classes.front()));
        if (!pair.write(
                n, [&](Document&, ElementRef el) { el.removeClass(cls); }, tag + " -." + cls) ||
            !pair.write(n, [&](Document&, ElementRef el) { el.addClass(cls); }, tag + " +." + cls)) {
            return false;
        }
    }
    return pair.restore("restoreState");
}

/// The whole synthetic interaction. False on the first difference (Pair::failure()).
bool interact(const EquivCase& ec, Instance& narrow, Pair& pair) {
    const float w = static_cast<float>(ec.width);
    const float h = static_cast<float>(ec.height);
    // Diagonals.
    for (int d = 0; d < 2; ++d) {
        for (int i = 0; i < 24; ++i) {
            const float t = (static_cast<float>(i) + 0.5f) / 24.0f;
            const float x = d == 0 ? t * w : (1.0f - t) * w;
            if (!pair.pointer(InputEvent::PointerMove, x, t * h,
                              "diagonal " + std::to_string(d) + "." + std::to_string(i))) {
                return false;
            }
        }
    }
    // Grid, walked back and forth.
    for (int row = 0; row < 8; ++row) {
        const float y = (static_cast<float>(row) + 0.5f) * h / 8.0f;
        for (int col = 0; col < 16; ++col) {
            const int c = (row % 2 == 0) ? col : 15 - col;
            const float x = (static_cast<float>(c) + 0.5f) * w / 16.0f;
            if (!pair.pointer(InputEvent::PointerMove, x, y,
                              "grid " + std::to_string(row) + "." + std::to_string(col))) {
                return false;
            }
        }
    }
    InputEvent leave;
    leave.kind = InputEvent::PointerLeave;
    if (!pair.input(leave, "pointer leave")) {
        return false;
    }
    // Press and release on every interactive element.
    const NodeArena& arena = narrow.doc->arena();
    std::vector<NodeId> interactive;
    uint32_t focusable = 0;
    for (NodeId n = 0; n < arena.capacity(); ++n) {
        if (!arena.alive(n) || arena.kind(n) != NodeKind::Element) {
            continue;
        }
        const ElementData& data = arena.element(n);
        focusable += data.focusable != 0 ? 1u : 0u;
        if (data.focusable != 0 || data.listenerHead != 0xFFFFFFFFu || data.style.cursor == Cursor::Pointer) {
            interactive.push_back(n);
        }
    }
    if (interactive.size() > kMaxPresses) {
        interactive.resize(kMaxPresses);
    }
    for (NodeId n : interactive) {
        if (!narrow.doc->arena().alive(n)) {
            continue;
        }
        const Vec2 c = layoutCenter(narrow.doc->arena(), n);
        const std::string tag = "press node " + std::to_string(n);
        if (!pair.pointer(InputEvent::PointerMove, c.x, c.y, tag + " move") ||
            !pair.pointer(InputEvent::PointerDown, c.x, c.y, tag + " down") ||
            !pair.pointer(InputEvent::PointerUp, c.x, c.y, tag + " up")) {
            return false;
        }
    }
    // Tab through the focusable elements.
    const uint32_t tabs = std::min<uint32_t>(focusable + 1, static_cast<uint32_t>(kMaxPresses));
    for (uint32_t i = 0; i < tabs; ++i) {
        if (!pair.key(KeyCode::Tab, "tab " + std::to_string(i))) {
            return false;
        }
    }
    // T-763: the state writes outside the dispatcher -- Disabled / Checked (ElementRef::setState),
    // each custom state, a class removed and added back -- on a spread of elements, then
    // restoreState() back to the snapshot taken before them.
    if (!stateWrites(narrow, pair)) {
        return false;
    }
    // Ports: two values, the second again, then back.
    for (const PortRecord& rec : narrow.doc->ports()) {
        if (rec.dir != PortDir::In) {
            continue;
        }
        const std::vector<std::string> values = portValues(rec);
        if (values.empty()) {
            continue;
        }
        const std::string original = narrow.doc->portText(rec.id);
        const std::string tag = "port " + rec.qualifiedName;
        if (!pair.portText(rec.id, values[0], tag + " = " + values[0]) ||
            !pair.portText(rec.id, values[1], tag + " = " + values[1]) ||
            !pair.portText(rec.id, values[1], tag + " = " + values[1] + " again")) {
            return false;
        }
        if (!original.empty() && !pair.portText(rec.id, original, tag + " back")) {
            return false;
        }
    }
    // Let transitions and animations run on.
    return pair.update(0.5f, "settle 0.5 s") && pair.update(kDt, "settle frame");
}

bool runCase(const EquivCase& ec, uint32_t& steps, std::string& err) {
    compile::CompileOptions options;
    options.rootDir = ec.xtPath.parent_path().string();
    options.strict = false;
    options.searchDirs = ec.assetRoots;
    compile::CompileResult result = compile::compileDocument(ec.xtPath.string(), options);
    if (!result.ok()) {
        err = "could not compile '" + ec.xtPath.string() + "'";
        return false;
    }
    Instance narrow;
    Instance legacy;
    if (!openInstance(ec, result.ir, /*legacy=*/false, narrow) ||
        !openInstance(ec, result.ir, /*legacy=*/true, legacy)) {
        err = "could not load '" + ec.xtPath.string() + "'";
        return false;
    }
    Pair pair(narrow, legacy);
    bool ok = pair.update(0.0f, "load");
    if (ok && !ec.eventsPath.empty()) {
        std::string errA;
        std::string errB;
        const bool ranA = runEventScript(*narrow.ctx, *narrow.doc, ec.eventsPath, errA);
        const bool ranB = runEventScript(*legacy.ctx, *legacy.doc, ec.eventsPath, errB);
        if (!ranA || !ranB) {
            err = "event script failed: " + (ranA ? errB : errA);
            return false;
        }
        ok = pair.compare("event script");
    }
    ok = ok && interact(ec, narrow, pair);
    steps = pair.steps();
    if (!ok) {
        err = pair.failure();
    }
    return ok;
}

}  // namespace

void runStyleInvalidationChecks(const std::string& uiDir, const std::string& examplesDir,
                                const std::string& filter, int& ran, int& failed) {
    std::vector<EquivCase> cases;
    for (const UiFixture& fx : loadUiManifest((fs::path(uiDir) / "manifest.txt").string())) {
        EquivCase ec;
        ec.name = "style_equiv_" + fx.name;
        ec.xtPath = fs::path(uiDir) / fx.xtPath;
        ec.width = fx.width;
        ec.height = fx.height;
        ec.eventsPath = fx.eventsPath.empty() ? std::string() : (fs::path(uiDir) / fx.eventsPath).string();
        ec.assetRoots = fx.assetRoots;
        cases.push_back(std::move(ec));
    }
    for (const char* rel : kExampleUis) {
        EquivCase ec;
        ec.xtPath = fs::path(examplesDir) / rel;
        ec.name = "style_equiv_example_" + ec.xtPath.stem().string() + "_" +
                  ec.xtPath.parent_path().parent_path().filename().string();
        ec.width = 1280;
        ec.height = 720;
        cases.push_back(std::move(ec));
    }
    uint32_t totalSteps = 0;
    int casesRan = 0;
    for (const EquivCase& ec : cases) {
        if (!filter.empty() && ec.name.find(filter) == std::string::npos) {
            continue;
        }
        ++ran;
        ++casesRan;
        uint32_t steps = 0;
        std::string err;
        if (!runCase(ec, steps, err)) {
            report("[FAIL] %s: %s\n", ec.name.c_str(), err.c_str());
            ++failed;
            continue;
        }
        totalSteps += steps;
        if (goldenVerbose()) {
            report("[PASS] %s (%u steps)\n", ec.name.c_str(), steps);
        }
    }
    if (casesRan > 0) {
        report("xt_golden: style invalidation equivalence: %d UI(s), %u compared step(s)\n", casesRan,
               totalSteps);
    }
}

}  // namespace xt::golden
