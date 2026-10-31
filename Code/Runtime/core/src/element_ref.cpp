#include <algorithm>
#include <cctype>
#include <cstdint>
#include <list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "binding_runtime.h"
#include "dispatch_guard.h"
#include "event_dispatcher.h"
#include "mini_css.h"
#include "repeat_controller.h"
#include "style_decode.h"
#include "style_template_internal.h"
#include "text_edit.h"
#include "xt/assert.h"
#include "xt/document.h"
#include "xt/property.h"

/// ElementRef: the public per-element API of Docs/design/13-api.md §13.3 -- identity, classes,
/// states, attributes, text, runtime style, property read/write, geometry and tree navigation.
/// See Docs/tasks/T-24.md (the class/state subset, moved here from document.cpp) and
/// Docs/tasks/T-35.md (everything else).

namespace xt {

namespace {

/// Marks `node` kDirtyStyle, its ancestors (automatic, NodeArena::markDirty) and every descendant
/// (explicit, non-recursive traversal) -- 05.8's conservative rule for class/state/style changes.
void markStyleDirtySubtree(NodeArena& arena, NodeId node) {
    arena.markDirty(node, static_cast<uint8_t>(kDirtyStyle));
    arena.traversePreorder(node, [&arena](NodeId n) {
        arena.markDirty(n, static_cast<uint8_t>(kDirtyStyle));
        return true;
    });
}

ElementRef makeRef(Document* doc, NodeId node) {
    if (node == kInvalidNode || !doc->arena().alive(node) || doc->arena().kind(node) != NodeKind::Element) {
        return ElementRef{};
    }
    return ElementRef{doc, node, doc->arena().generation(node)};
}

/// T-270f: the one check at the top of every mutating ElementRef call. A node of a recycled
/// `<repeat>` instance waiting in its pool (alive, detached, flagged kPooled until its reuse resets
/// it from the template) is treated exactly as a removed node. O(1): bindings write through the
/// setters every frame.
bool acceptsMutation(const ElementRef& ref) {
    return ref.valid() && (ref.doc->arena().common(ref.node).flags2 & kPooled) == 0;
}

bool isAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

/// T-270e: an empty name, `none` or a CSS-wide keyword names nothing to look up.
bool namesNothing(std::string_view name) {
    if (name.empty()) {
        return true;
    }
    for (std::string_view keyword : {"none", "inherit", "initial", "unset", "revert"}) {
        if (name.size() == keyword.size() &&
            std::equal(name.begin(), name.end(), keyword.begin(),
                       [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; })) {
            return true;
        }
    }
    return false;
}

/// T-389: FNV-1a over a setStyle() text (never 0, which ElementData::setStyleHash reserves).
uint64_t styleTextHash(std::string_view text) {
    uint64_t h = 1469598103934665603ull;
    for (char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h == 0 ? 1 : h;
}

/// T-389 (27 §27.4.1): mini_css::parseDeclarations() results by text, the 256 most recently used
/// (per thread: they hold no Document state). The returned list stays valid until the next call.
const std::vector<mini_css::Decl>& parsedStyle(std::string_view text, uint64_t hash, Status& status) {
    struct Entry {
        std::string text;
        Status status = Status::kOk;
        std::vector<mini_css::Decl> decls;
    };
    struct Cache {
        std::list<Entry> lru;  // Most recently used first.
        std::unordered_map<uint64_t, std::list<Entry>::iterator> index;
    };
    constexpr size_t kCapacity = 256;
    thread_local Cache cache;
    auto it = cache.index.find(hash);
    if (it != cache.index.end()) {
        if (it->second->text == text) {
            cache.lru.splice(cache.lru.begin(), cache.lru, it->second);
            status = it->second->status;
            return it->second->decls;
        }
        cache.lru.erase(it->second);  // A hash collision: the newer text takes the slot.
        cache.index.erase(it);
    }
    Entry entry;
    entry.text = std::string(text);
    entry.status = mini_css::parseDeclarations(text, entry.decls);
    cache.lru.push_front(std::move(entry));
    cache.index[hash] = cache.lru.begin();
    if (cache.lru.size() > kCapacity) {
        cache.index.erase(styleTextHash(cache.lru.back().text));
        cache.lru.pop_back();
    }
    status = cache.lru.front().status;
    return cache.lru.front().decls;
}

/// The runtime declaration write setStyle() and setStyleTemplate() (T-477) share: `out` replaces
/// the element's declaration of the same property (keeping its T-270e warning flag) or is
/// appended. Returns the stored declaration.
RuntimeStyleDecl& storeRuntimeDecl(ElementData& data, RuntimeStyleDecl out) {
    if (out.prop == prop::BorderColor) {
        // T-333 (05 §5.9.8): like CSS's `border-color` shorthand, the one-color form resets
        // the sides an earlier setStyle gave their own colors.
        data.runtimeDecls.erase(std::remove_if(data.runtimeDecls.begin(), data.runtimeDecls.end(),
                                               [](const RuntimeStyleDecl& d) {
                                                   return d.prop >= prop::BorderTopColor &&
                                                          d.prop <= prop::BorderLeftColor;
                                               }),
                                data.runtimeDecls.end());
    }
    for (RuntimeStyleDecl& existing : data.runtimeDecls) {
        if (existing.prop == out.prop) {
            out.undeclaredNameWarned = existing.undeclaredNameWarned;
            existing = out;
            return existing;
        }
    }
    data.runtimeDecls.push_back(out);
    return data.runtimeDecls.back();
}

/// T-270e: the warning for a setStyle value that names what the compiled document does not have,
/// or an empty string. `font-family` (T-270f): the list's first family -- the only one setStyle
/// uses -- is not in the IR's font-family pool (the only families the runtime cascade can select,
/// style_engine.cpp applyRuntimeDecl), so the declaration has no effect. `background-image` /
/// `border-image-source`: no Image / AtlasImage asset has the name (style_engine.cpp
/// resolveBackgroundImageByName's lookup).
std::string undeclaredNameWarning(const Document& doc, const mini_css::Decl& decl) {
    namespace sp = style_decode::styleprop;
    if (decl.varRef || namesNothing(decl.text)) {
        return std::string{};
    }
    const IrView& ir = doc.ir();
    if (decl.prop == sp::FontFamily) {
        const std::span<const uint32_t> pool = ir.style().fontFamilies();
        if (std::any_of(pool.begin(), pool.end(), [&](uint32_t id) { return ir.string(id) == decl.text; })) {
            return std::string{};
        }
        return "font-family \"" + decl.text +
               "\" is not declared by the compiled document; setStyle uses only the first family of a "
               "list and can only select families the document names (declare it in a stylesheet, or "
               "use @font-fallback)";
    }
    if (decl.prop == sp::BackgroundImage || decl.prop == sp::BorderImageSource) {
        for (const IrAsset& asset : ir.assets().assets()) {
            // Kind 0 = Image, 3 = AtlasImage, like resolveBackgroundImageByName.
            if ((asset.kind == 0 || asset.kind == 3) && ir.string(asset.name) == decl.text) {
                return std::string{};
            }
        }
        return "image \"" + decl.text + "\" is not an asset of the compiled document";
    }
    return std::string{};
}

}  // namespace

bool ElementRef::valid() const {
    return doc != nullptr && doc->arena().alive(node) && doc->arena().generation(node) == generation;
}

std::string_view ElementRef::tag() const {
    if (!valid()) {
        return std::string_view{};
    }
    return doc->string(doc->arena().element(node).tag);
}

std::string_view ElementRef::id() const {
    if (!valid()) {
        return std::string_view{};
    }
    return doc->string(doc->arena().element(node).id);
}

bool ElementRef::hasClass(std::string_view cls) const {
    if (!valid()) {
        return false;
    }
    const ElementData& data = doc->arena().element(node);
    for (StringId sid : data.classes) {
        if (doc->string(sid) == cls) {
            return true;
        }
    }
    return false;
}

bool ElementRef::addClass(std::string_view cls) {
    if (!acceptsMutation(*this) || cls.empty()) {
        return false;
    }
    if (hasClass(cls)) {
        return false;
    }
    StringId sid = doc->intern(cls);
    NodeArena& arena = doc->arena();
    ElementData& data = arena.element(node);
    data.classes.insert(std::upper_bound(data.classes.begin(), data.classes.end(), sid), sid);
    markStyleDirtySubtree(arena, node);
    return true;
}

bool ElementRef::removeClass(std::string_view cls) {
    if (!acceptsMutation(*this)) {
        return false;
    }
    NodeArena& arena = doc->arena();
    ElementData& data = arena.element(node);
    for (auto it = data.classes.begin(); it != data.classes.end(); ++it) {
        if (doc->string(*it) == cls) {
            data.classes.erase(it);
            markStyleDirtySubtree(arena, node);
            return true;
        }
    }
    return false;
}

bool ElementRef::toggleClass(std::string_view cls) {
    if (!acceptsMutation(*this)) {
        return false;
    }
    if (hasClass(cls)) {
        removeClass(cls);
        return false;
    }
    return addClass(cls);
}

bool ElementRef::hasState(ElementState state) const {
    if (!valid()) {
        return false;
    }
    return (doc->arena().element(node).stateBits & static_cast<uint16_t>(state)) != 0;
}

bool ElementRef::setState(ElementState state, bool on) {
    // T-260a: check / uncheck triggers. Like every ElementRef call below that runs triggers or
    // listeners, it holds a DispatchGuard of the Document's Context (null when standalone).
    DispatchGuard dispatch(doc != nullptr ? doc->m_context : nullptr);
    if (!acceptsMutation(*this)) {
        return false;
    }
    if (state != kStateDisabled && state != kStateChecked) {
        return false;
    }
    NodeArena& arena = doc->arena();
    ElementData& data = arena.element(node);
    bool currentlyOn = (data.stateBits & static_cast<uint16_t>(state)) != 0;
    if (currentlyOn == on) {
        return false;
    }
    if (on) {
        data.stateBits = static_cast<uint16_t>(data.stateBits | static_cast<uint16_t>(state));
    } else {
        data.stateBits = static_cast<uint16_t>(data.stateBits & ~static_cast<uint16_t>(state));
    }
    markStyleDirtySubtree(arena, node);
    if (state == kStateChecked) {
        doc->checkedChanged(node, on);  // T-174: check / uncheck triggers.
    }
    return true;
}

std::string_view ElementRef::value() const {
    if (!valid()) {
        return std::string_view{};
    }
    const EditState* state = doc->editStateForElement(node);
    return state != nullptr ? std::string_view(state->value) : std::string_view{};
}

void ElementRef::setValue(std::string_view text) {
    DispatchGuard dispatch(doc != nullptr ? doc->m_context : nullptr);  // T-260a: Change listeners.
    if (!acceptsMutation(*this)) {
        return;
    }
    EditState* state = doc->editStateForElement(node);
    if (state == nullptr) {
        return;
    }
    bool changed = state->value != text;
    state->value.assign(text);
    state->cursor = static_cast<uint32_t>(state->value.size());
    state->anchor = state->cursor;
    doc->m_events->afterEdit(doc->m_arena, node, changed);
}

namespace {

/// T-397: `off` clamped to `value`'s length and moved back to a UTF-8 character start.
uint32_t clampToCharStart(const std::string& value, uint32_t off) {
    uint32_t o = std::min(off, static_cast<uint32_t>(value.size()));
    while (o > 0 && o < value.size() && (static_cast<unsigned char>(value[o]) & 0xC0u) == 0x80u) {
        --o;
    }
    return o;
}

}  // namespace

Status ElementRef::selectAll() {
    if (!acceptsMutation(*this)) {
        return Status::kInvalidArgument;
    }
    EditState* state = doc->editStateForElement(node);
    if (state == nullptr) {
        return Status::kUnsupported;
    }
    xt::selectAll(*state);
    doc->m_arena.markDirty(node, static_cast<uint8_t>(kDirtyPaint));
    return Status::kOk;
}

Status ElementRef::setSelection(uint32_t start, uint32_t end) {
    if (!acceptsMutation(*this)) {
        return Status::kInvalidArgument;
    }
    EditState* state = doc->editStateForElement(node);
    if (state == nullptr) {
        return Status::kUnsupported;
    }
    const uint32_t e = clampToCharStart(state->value, end);
    const uint32_t b = std::min(clampToCharStart(state->value, start), e);
    state->anchor = b;
    state->cursor = e;
    doc->m_arena.markDirty(node, static_cast<uint8_t>(kDirtyPaint));
    return Status::kOk;
}

TextSelection ElementRef::selection() const {
    if (!valid()) {
        return TextSelection{};
    }
    const EditState* state = doc->editStateForElement(node);
    if (state == nullptr) {
        return TextSelection{};
    }
    return TextSelection{selectionBegin(*state), selectionEnd(*state)};
}

std::string_view ElementRef::attribute(std::string_view name) const {
    if (!valid()) {
        return std::string_view{};
    }
    const ElementData& data = doc->arena().element(node);
    for (const auto& entry : data.attributes) {
        if (doc->string(entry.first) == name) {
            return doc->string(entry.second);
        }
    }
    return std::string_view{};
}

Status ElementRef::setAttribute(std::string_view name, std::string_view text) {
    DispatchGuard dispatch(doc != nullptr ? doc->m_context : nullptr);  // T-260a: value / checked.
    if (!acceptsMutation(*this) || name.empty()) {
        return Status::kInvalidArgument;
    }
    NodeArena& arena = doc->arena();
    if (name == "style") {
        Status status = setStyle(text);
        arena.element(node).inlineStyle = doc->intern(text);
        return status;
    }
    if (name == "src") {
        // 05.2: `src` resolves to an AssetId/SceneId at compile time; there is no runtime asset
        // pipeline in this version (Docs/tasks/T-35.md "createElement").
        return Status::kUnsupported;
    }
    if (name == "value") {
        setValue(text);
        return Status::kOk;
    }
    if (name == "disabled" || name == "checked") {
        setState(name == "disabled" ? kStateDisabled : kStateChecked, text == "true");
        return Status::kOk;
    }
    if (name == "id") {
        arena.element(node).id = text.empty() ? kInvalidString : doc->intern(text);
        markStyleDirtySubtree(arena, node);
        return Status::kOk;
    }
    if (name == "class") {
        std::vector<StringId> classes;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && isAsciiSpace(text[i])) {
                ++i;
            }
            size_t start = i;
            while (i < text.size() && !isAsciiSpace(text[i])) {
                ++i;
            }
            if (i > start) {
                classes.push_back(doc->intern(text.substr(start, i - start)));
            }
        }
        std::sort(classes.begin(), classes.end());
        classes.erase(std::unique(classes.begin(), classes.end()), classes.end());
        arena.element(node).classes = std::move(classes);
        markStyleDirtySubtree(arena, node);
        return Status::kOk;
    }
    StringId nameId = doc->intern(name);
    StringId valueId = doc->intern(text);
    ElementData& data = arena.element(node);
    bool replaced = false;
    for (auto& entry : data.attributes) {
        if (entry.first == nameId) {
            entry.second = valueId;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        data.attributes.emplace_back(nameId, valueId);
    }
    markStyleDirtySubtree(arena, node);
    if (name == "placeholder") {
        arena.markDirty(node, static_cast<uint8_t>(kDirtyLayout));  // T-395: the placeholder runs.
    }
    return Status::kOk;
}

std::string_view ElementRef::text() const {
    if (!valid()) {
        return std::string_view{};
    }
    const EditState* state = doc->editStateForElement(node);
    if (state != nullptr) {
        return std::string_view(state->value);
    }
    NodeId textNode = firstTextChild(doc->arena(), node);
    if (textNode == kInvalidNode) {
        return std::string_view{};
    }
    return doc->string(doc->arena().text(textNode).text);
}

void ElementRef::setText(std::string_view content) {
    DispatchGuard dispatch(doc != nullptr ? doc->m_context : nullptr);  // T-260a: via setValue().
    if (!acceptsMutation(*this)) {
        return;
    }
    if (doc->editStateForElement(node) != nullptr) {
        setValue(content);  // An editable `input`'s string lives in the EditState pool (08.5).
        return;
    }
    NodeArena& arena = doc->arena();
    NodeId textNode = firstTextChild(arena, node);
    if (textNode == kInvalidNode) {
        textNode = arena.create(NodeKind::Text);
        arena.appendChild(node, textNode);
    }
    // T-270a (C6): the text is the node's own (Document::setRuntimeText), not an interned string,
    // so a value that changes every frame reuses one slot instead of growing the string pool.
    if (arena.text(textNode).text != kInvalidString && doc->string(arena.text(textNode).text) == content) {
        return;
    }
    const StringId sid = doc->setRuntimeText(textNode, content);
    TextNodeData& data = arena.text(textNode);
    data.text = sid;
    data.shapedValid = 0;
    arena.markDirty(textNode, static_cast<uint8_t>(kDirtyLayout));
    arena.markDirty(node, static_cast<uint8_t>(kDirtyLayout));
}

Status ElementRef::setStyle(std::string_view cssDeclarations) {
    if (!acceptsMutation(*this)) {
        return Status::kInvalidArgument;
    }
    if (doc->m_perf->enabled()) {
        ++doc->m_perf->current().setStyleCalls;  // T-389 (21 §21.1).
    }
    NodeArena& arena = doc->arena();
    ElementData& data = arena.element(node);
    // T-389 (27 §27.4.1): the same text as this element's last setStyle() changes nothing.
    const uint64_t hash = styleTextHash(cssDeclarations);
    if (data.setStyleHash == hash && data.setStyleLength == cssDeclarations.size()) {
        return static_cast<Status>(data.setStyleStatus);
    }
    Status status = Status::kOk;
    const std::vector<mini_css::Decl>& parsed = parsedStyle(cssDeclarations, hash, status);
    for (const mini_css::Decl& decl : parsed) {
        RuntimeStyleDecl out;
        out.prop = decl.prop;
        out.important = decl.important;
        if (decl.varRef) {
            // T-170: keep the reference; StyleEngine resolves it against the token tables on
            // every resolve, so a later setTheme()/setToken() reaches this declaration too.
            out.varName = doc->intern(decl.text);
            out.varFallback = decl.varFallback;
            out.hasVarFallback = decl.hasVarFallback ? 1 : 0;
        } else if (decl.prop == style_decode::styleprop::Effect) {
            // 18-effects.md §18.4/T-51b: write the parsed sequence into Document's runtime
            // effect pool and cascade the resulting tagged ListRef, instead of interning text.
            // T-270a: into this element's own segment, so a repeated setStyle does not append.
            out.value = doc->declareRuntimeEffectsFor(node, decl.effects, decl.effectStops);
        } else if (decl.prop == style_decode::styleprop::BackgroundGradient) {
            // 05.5c/T-115: same pattern for `background-gradient`.
            // T-270c: into one of the element's two alternating slots (see RuntimeDeclSlots).
            out.value = doc->declareRuntimeGradientFor(node, decl.gradient);
        } else if (decl.prop == style_decode::styleprop::ClipPath) {
            // T-332 (05 §5.9.8): the parsed shape into the element's clip-shape slot; the cascaded
            // Enum is its tagged offset (0 for `none`).
            out.value = doc->declareRuntimeClipShapeFor(node, decl.clipShape);
        } else if (decl.prop == style_decode::styleprop::Animation) {
            // 05.6/17.6 (T-123): resolve each item's `@keyframes` name against the IR's table --
            // the one part of the declaration mini_css cannot do without a Document -- then
            // cascade the runtime pool's ListRef, like `effect` above. A name this document has
            // no `@keyframes` for is dropped with a warning (it has no timeline to play).
            std::vector<IrAnimationSpec> specs;
            specs.reserve(decl.animations.size());
            for (const mini_css::AnimationDecl& item : decl.animations) {
                TimelineId timeline = kInvalidTimeline;
                for (const IrKeyframesRef& ref : doc->ir().style().keyframesRefs()) {
                    if (doc->string(ref.name) == item.name) {
                        timeline = ref.timeline;
                        break;
                    }
                }
                if (timeline == kInvalidTimeline) {
                    doc->addWarning("animation: no @keyframes named \"" + item.name + "\"");
                    continue;
                }
                IrAnimationSpec spec = item.spec;
                spec.timeline = timeline;
                if (spec.easing.kind == static_cast<uint8_t>(EasingKind::LinearPoints)) {
                    // T-335: the `linear()` points move into the document's easing point pool.
                    uint16_t offset = 0;
                    if (!doc->animator().internEasingPoints(item.easingPoints, offset)) {
                        doc->addWarning("animation: too many linear() easing points; using linear");
                        spec.easing = IrEasing{};
                    } else {
                        spec.easing = irLinearPointsEasing(offset, spec.easing.stepCount);
                    }
                }
                specs.push_back(spec);
            }
            out.value = doc->declareRuntimeAnimationsFor(node, specs);  // T-270c: likewise.
        } else {
            out.value = decl.value;
            if (!decl.text.empty()) {
                // T-270c: a text some string table already has keeps its id (StyleEngine resolves
                // only compiled names); any other goes into this element's own slot for the
                // property instead of the runtime string pool, so new values do not accumulate.
                const StringId known = doc->findString(decl.text);
                out.value.sid =
                    known != kInvalidString ? known : doc->setNodeText(node, decl.prop, decl.text);
            }
        }
        RuntimeStyleDecl* written = &storeRuntimeDecl(data, out);
        // T-270e: a font family / image the compiled document does not have still does nothing,
        // but says so, once per element and property, here where the value is written.
        if (written->undeclaredNameWarned == 0) {
            std::string warning = undeclaredNameWarning(*doc, decl);
            if (!warning.empty()) {
                written->undeclaredNameWarned = 1;
                doc->addWarning(std::move(warning));
            }
        }
    }
    markStyleDirtySubtree(arena, node);
    // Taken again: the declarations above may have grown the arena's element storage.
    ElementData& written = arena.element(node);
    // An `animation` declared again restarts (17.6), so a text holding one is never a no-op.
    bool restarts = false;
    for (const mini_css::Decl& decl : parsed) {
        restarts = restarts || decl.prop == style_decode::styleprop::Animation;
    }
    written.setStyleHash = restarts ? 0 : hash;
    written.setStyleLength = static_cast<uint32_t>(cssDeclarations.size());
    written.setStyleStatus = static_cast<uint8_t>(status);
    return status;
}

Status ElementRef::setStyleTemplate(const StyleTemplate& tmpl, std::string_view cssDeclarations) {
    // Read first: a text of another shape is setStyle()'s, and nothing may have happened then.
    float numbers[StyleTemplate::kMaxHoles];
    if (!readStyleTemplateNumbers(tmpl, cssDeclarations, numbers)) {
        return Status::kNotFound;
    }
    if (!acceptsMutation(*this)) {
        return Status::kInvalidArgument;
    }
    if (doc->m_perf->enabled()) {
        ++doc->m_perf->current().setStyleCalls;  // As setStyle() (T-389, 21 §21.1).
    }
    NodeArena& arena = doc->arena();
    ElementData& data = arena.element(node);
    // setStyle()'s own shortcut, by the same text hash: this text parses to the template's
    // declarations with status kOk, which is what setStyle() would have stored for it.
    const uint64_t hash = styleTextHash(cssDeclarations);
    if (data.setStyleHash == hash && data.setStyleLength == cssDeclarations.size()) {
        return static_cast<Status>(data.setStyleStatus);
    }
    // Plain values only (compileStyleTemplate()): nothing to intern or pool, no `animation`, and
    // no T-270e name warning (it needs a name).
    for (size_t i = 0; i < tmpl.decls.size(); ++i) {
        RuntimeStyleDecl out;
        out.prop = tmpl.decls[i].prop;
        out.important = tmpl.decls[i].important;
        fillStyleTemplateValue(tmpl, i, numbers, out.value);
        storeRuntimeDecl(data, out);
    }
    markStyleDirtySubtree(arena, node);
    ElementData& written = arena.element(node);
    written.setStyleHash = hash;
    written.setStyleLength = static_cast<uint32_t>(cssDeclarations.size());
    written.setStyleStatus = static_cast<uint8_t>(Status::kOk);
    return Status::kOk;
}

void ElementRef::setProperty(PropId prop, const Value& value, bool sticky) {
    if (!acceptsMutation(*this)) {
        return;
    }
    if (!propertyApplies(NodeKind::Element, prop) || value.type != propertyType(prop)) {
        return;
    }
    if (sticky) {
        doc->animator().setOverride(node, prop, value);
        doc->arena().markDirty(node, propertyDirtyBits(prop));
        return;
    }
    xt::setProperty(doc->arena(), node, prop, value);
}

void ElementRef::setPropertyTween(PropId prop, const Value& value, float duration, Easing easing) {
    if (!acceptsMutation(*this)) {
        return;
    }
    if (!propertyApplies(NodeKind::Element, prop) || value.type != propertyType(prop)) {
        return;
    }
    doc->animator().setOverride(node, prop, value, duration, easing);
    doc->arena().markDirty(node, propertyDirtyBits(prop));
}

void ElementRef::clearOverride(PropId prop) {
    if (!acceptsMutation(*this) || !propertyApplies(NodeKind::Element, prop)) {
        return;
    }
    doc->animator().clearOverride(node, prop);
    // ComputedStyle::base *is* an Element's live property storage (xt/node.h), so the override's
    // last value is still sitting there and nothing would rewrite it: the cascade only writes a
    // property whose resolved value differs from what is stored. Marking the element kDirtyStyle
    // makes the next update() re-resolve it, which restores the cascade's value (and, if the
    // element declares a `transition` for that property, transitions back to it).
    markStyleDirtySubtree(doc->arena(), node);
    doc->arena().markDirty(node, propertyDirtyBits(prop));
}

Value ElementRef::property(PropId prop) const {
    if (!valid() || !propertyApplies(NodeKind::Element, prop)) {
        return Value{};
    }
    return getProperty(doc->arena(), node, prop);
}

Value ElementRef::computedStyleValue(uint16_t propOrStyleProp) const {
    if (!valid()) {
        return Value{};
    }
    // An animatable PropId (0-108, the general/Element ranges T-24/T-35's property()/setProperty()
    // cover): the cascaded value already lives in ComputedStyle::base (see getProperty()).
    if (propertyDefined(static_cast<PropId>(propOrStyleProp)) &&
        propertyApplies(NodeKind::Element, static_cast<PropId>(propOrStyleProp))) {
        return property(static_cast<PropId>(propOrStyleProp));
    }
    // A non-animatable StyleProp (160-188, Code/Runtime/core/src/style_decode.h): read straight off the
    // cascaded ComputedStyle. Only the Enum-valued fields are supported (Docs/tasks/T-59.md);
    // every other StyleProp (z-index, font-weight, font-family, lang, transition/animation/effect,
    // background-image, backface-visibility) has no PropertyType::Length/Enum panel encoding here
    // and reports an empty Value, per the task's "其他不支援者回 Value{}".
    // Qualified alias, not `using namespace`: style_decode::styleprop's names (Display, Cursor,
    // WritingMode, ...) collide with core's own like-named enum *types* (xt::Display, xt::Cursor,
    // ...) visible in this same xt:: scope, which a `using namespace` would make ambiguous at
    // every `case` label below.
    namespace sp = style_decode::styleprop;
    const ComputedStyle& style = doc->arena().element(node).style;
    switch (propOrStyleProp) {
        case sp::Display:
            return Value::ofEnum(static_cast<uint32_t>(style.display));
        case sp::Position:
            return Value::ofEnum(static_cast<uint32_t>(style.position));
        case sp::FlexDirection:
            return Value::ofEnum(static_cast<uint32_t>(style.flexDir));
        case sp::FlexWrap:
            return Value::ofEnum(static_cast<uint32_t>(style.flexWrap));
        case sp::JustifyContent:
            return Value::ofEnum(static_cast<uint32_t>(style.justify));
        case sp::AlignItems:
            return Value::ofEnum(static_cast<uint32_t>(style.alignItems));
        case sp::AlignSelf:
            return Value::ofEnum(static_cast<uint32_t>(style.alignSelf));
        case sp::AlignContent:
            return Value::ofEnum(static_cast<uint32_t>(style.alignContent));
        case sp::Overflow:  // T-336: the shorthand reads as the horizontal axis.
        case sp::OverflowX:
            return Value::ofEnum(static_cast<uint32_t>(style.overflowX));
        case sp::OverflowY:
            return Value::ofEnum(static_cast<uint32_t>(style.overflowY));
        case sp::Visibility:
            return Value::ofEnum(static_cast<uint32_t>(style.visibility));
        case sp::PointerEvents:
            return Value::ofEnum(static_cast<uint32_t>(style.pointerEvents));
        case sp::PointerWatch:
            return Value::ofEnum(style.pointerWatch);
        case sp::FocusNav:  // T-194 (08.9).
            return Value::ofEnum(style.focusNav);
        case sp::Layer:
            return Value::ofEnum(style.layerOverlay);
        case sp::OutlineStyle:  // T-325 (05 §5.9.8).
            return Value::ofEnum(style.outlineStyle);
        case sp::TextTransform:  // T-323 (05 §5.9.8).
            return Value::ofEnum(static_cast<uint32_t>(style.textTransform));
        case sp::TextDecorationLine:  // T-324 (05 §5.9.8): kTextDecoration* bits.
            return Value::ofEnum(style.textDecorationLine);
        case sp::MixBlendMode:  // T-326 (18 §18.12).
            return Value::ofEnum(style.mixBlendMode);
        case sp::BackgroundRepeat:  // T-327 (05 §5.9.8): kBackgroundRepeat*.
            return Value::ofEnum(style.backgroundRepeat);
        case sp::ImageRendering:  // T-327: 0 auto, 1 pixelated.
            return Value::ofEnum(style.imageRendering);
        case sp::ClipPath:  // T-332 (05 §5.9.8): 0 none, else the (tagged) shape reference.
            return Value::ofEnum(style.clipShape);
        case sp::LineClamp:  // T-331 (05 §5.9.8): the line count, 0 = none.
            return Value::ofEnum(style.lineClamp);
        case sp::JustifyItems:  // T-338 (06 §6.15): AlignItems numbering.
            return Value::ofEnum(static_cast<uint32_t>(style.justifyItems));
        case sp::GridColumnStart:  // T-338: kGridLine*.
        case sp::GridColumnEnd:
        case sp::GridRowStart:
        case sp::GridRowEnd:
            return Value::ofEnum(style.gridLines[propOrStyleProp - sp::GridColumnStart]);
        case sp::FontVariantNumeric:  // T-331: 0 normal, 1 tabular-nums.
            return Value::ofEnum(style.fontVariantNumeric);
        case sp::UiScale:  // T-345 (05 §5.9.9): (mode, min, max, effective factor).
            return Value::ofColor(Color{static_cast<float>(style.uiScaleMode), style.uiScaleMin,
                                        style.uiScaleMax, style.uiScaleEff});
        case sp::MediaOverride:  // T-345: kMediaOverride*.
            return Value::ofEnum(style.mediaOverride);
        case sp::AppRegion:  // T-376 (05 §5.9.10): 0 none, 1 drag, 2 no-drag.
            return Value::ofEnum(static_cast<uint32_t>(style.appRegion));
        case sp::HitSlop:  // T-386 (05 §5.9.11): top, right, bottom, left (px; -fraction = %).
            return Value::ofColor(
                Color{style.hitSlop[0], style.hitSlop[1], style.hitSlop[2], style.hitSlop[3]});
        case sp::BorderTopStyle:  // T-333 (05 §5.9.8): kBorderStyle*.
        case sp::BorderRightStyle:
        case sp::BorderBottomStyle:
        case sp::BorderLeftStyle:
            return Value::ofEnum(style.borderStyle[propOrStyleProp - sp::BorderTopStyle]);
        case sp::ClipStart:  // T-191 (28.2).
            return Value::ofFloat(style.clipStart);
        case sp::ClipRate:
            return Value::ofFloat(style.clipRate);
        case sp::FontStyle:
            return Value::ofEnum(static_cast<uint32_t>(style.fontStyle));
        case sp::TextAlign:
            return Value::ofEnum(static_cast<uint32_t>(style.textAlign));
        case sp::WhiteSpace:
            return Value::ofEnum(static_cast<uint32_t>(style.whiteSpace));
        case sp::TextOverflow:
            return Value::ofEnum(static_cast<uint32_t>(style.textOverflow));
        case sp::Cursor:
            return Value::ofEnum(static_cast<uint32_t>(style.cursor));
        case sp::ObjectFit:
            return Value::ofEnum(static_cast<uint32_t>(style.objectFit));
        case sp::Direction:
            return Value::ofEnum(static_cast<uint32_t>(style.direction));
        case sp::WritingMode:
            return Value::ofEnum(static_cast<uint32_t>(style.writingMode));
        case sp::TextOrientation:
            return Value::ofEnum(static_cast<uint32_t>(style.textOrientation));
        default:
            return Value{};
    }
}

uint32_t ElementRef::effectCount() const {
    if (!valid()) {
        return 0;
    }
    return doc->arena().common(node).effectCount;
}

EffectInstance ElementRef::effect(uint32_t index) const {
    if (!valid()) {
        return EffectInstance{};
    }
    const NodeCommon& common = doc->arena().common(node);
    if (index >= common.effectCount) {
        return EffectInstance{};
    }
    return doc->arena().effects()[common.effectOffset + index];  // Copy (18-effects.md §18.4, T-51b).
}

void ElementRef::setEffectParam(uint32_t effectIndex, uint32_t paramIndex, const Value& value) {
    if (!acceptsMutation(*this)) {
        return;
    }
    // 18-effects.md §18.4/T-51b: `setProperty(effectParam(i, p), v, sticky = false)` +
    // `markDirty(kDirtyPaint)` -- setProperty()'s non-sticky path writes the pool slot directly
    // (xt::setProperty) without marking anything dirty itself, unlike its sticky/override path.
    setProperty(effectParam(effectIndex, paramIndex), value, /*sticky=*/false);
    doc->arena().markDirty(node, static_cast<uint8_t>(kDirtyPaint));
}

Status ElementRef::setEffects(std::span<const EffectInstance> effects, std::span<const GradientStop> stops) {
    if (!acceptsMutation(*this)) {
        return Status::kInvalidArgument;
    }
    if (effects.size() > 16) {  // 18.2: "每節點 16 個效果".
        return Status::kInvalidArgument;
    }
    NodeArena& arena = doc->arena();
    ElementData& data = arena.element(node);
    data.setStyleHash = 0;  // T-389: runtimeDecls changed outside setStyle().
    RuntimeStyleDecl out;
    out.prop = style_decode::styleprop::Effect;
    out.important = 0;
    out.value = doc->declareRuntimeEffectsFor(node, effects, stops);  // 18.4: cascades like setStyle's.
    bool replaced = false;
    for (RuntimeStyleDecl& existing : data.runtimeDecls) {
        if (existing.prop == out.prop) {
            existing = out;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        data.runtimeDecls.push_back(out);
    }
    markStyleDirtySubtree(arena, node);
    return Status::kOk;
}

Rect ElementRef::boundingBox() const {
    if (!valid()) {
        return Rect{};
    }
    // Docs/design/19-3d-space.md §19.9, Docs/tasks/T-48.md: the border box's four corners, each
    // projected through Document::nodeToScreen (so a 3D-rooted element's box is the AABB of its
    // projected quad, not its unprojected 2D bounds), then the AABB of those four points.
    const NodeArena& arena = doc->arena();
    const LayoutBox& box = arena.element(node).box;
    Vec2 corners[4] = {
        Vec2{0, 0},
        Vec2{box.width, 0},
        Vec2{box.width, box.height},
        Vec2{0, box.height},
    };
    Vec2 mapped[4];
    for (int i = 0; i < 4; ++i) {
        mapped[i] = doc->nodeToScreen(node, corners[i]);
    }
    float minX = mapped[0].x;
    float maxX = mapped[0].x;
    float minY = mapped[0].y;
    float maxY = mapped[0].y;
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, mapped[i].x);
        maxX = std::max(maxX, mapped[i].x);
        minY = std::min(minY, mapped[i].y);
        maxY = std::max(maxY, mapped[i].y);
    }
    return Rect{minX, minY, maxX - minX, maxY - minY};
}

LayoutBox ElementRef::layoutBox() const {
    if (!valid()) {
        return LayoutBox{};
    }
    return doc->arena().element(node).box;
}

Status ElementRef::focus() {
    DispatchGuard dispatch(doc != nullptr ? doc->m_context : nullptr);  // T-260a: Focus / Blur.
    if (!acceptsMutation(*this)) {
        return Status::kInvalidArgument;
    }
    // T-397 (08 §8.17): an element that is not laid out (`display: none` on it or an ancestor)
    // cannot take the focus -- the next update() would only blur it again. T-399 (08 §8.18):
    // unless this frame's pending changes (a port change not evaluated yet, a restyle of the
    // hiding element) may show it: the focus is then pending until update() has resolved style
    // and layout, and is given to the element only if it is focusable by then.
    const NodeArena& arena = doc->arena();
    bool hidden = false;
    bool mayShow = doc->m_bindings->hasDirty();
    for (NodeId n = node; n != kInvalidNode && arena.alive(n); n = arena.parent(n)) {
        if (arena.kind(n) == NodeKind::Element && arena.element(n).style.display == Display::None) {
            hidden = true;
            mayShow = mayShow || (arena.dirty(n) & static_cast<uint8_t>(kDirtyStyle)) != 0;
        }
    }
    if (hidden) {
        if (!mayShow) {
            return Status::kUnsupported;
        }
        doc->m_events->setPendingFocus(node, generation, 0);
        return Status::kOk;
    }
    doc->setFocus(*this);
    return Status::kOk;
}

NodeId ElementRef::sceneNode(std::string_view name) const {
    if (!valid()) {
        return kInvalidNode;
    }
    // Docs/design/19-3d-space.md §19.4: for a <camera> element, name == "" returns its Camera
    // child node (it has no instantiated scene).
    if (name.empty()) {
        const ElementData& data = doc->arena().element(node);
        if (data.cameraNode != kInvalidNode) {
            return data.cameraNode;
        }
    }
    const SceneInstance* instance = doc->sceneInstance(*this);
    if (instance == nullptr) {
        return kInvalidNode;
    }
    const NodeArena& arena = doc->arena();
    for (NodeId n : instance->localToArena) {
        if (n != kInvalidNode && arena.alive(n) && doc->string(arena.name(n)) == name) {
            return n;
        }
    }
    return kInvalidNode;
}

uint32_t ElementRef::defaultAnimation() const { return valid() ? doc->defaultAnimation(*this) : 0; }

ElementRef ElementRef::parent() const {
    if (!valid()) {
        return ElementRef{};
    }
    return makeRef(doc, doc->arena().parent(node));
}

ElementRef ElementRef::firstChild() const {
    if (!valid()) {
        return ElementRef{};
    }
    const NodeArena& arena = doc->arena();
    for (NodeId c = arena.firstChild(node); c != kInvalidNode; c = arena.nextSibling(c)) {
        if (arena.kind(c) == NodeKind::Element) {
            return makeRef(doc, c);
        }
    }
    return ElementRef{};
}

ElementRef ElementRef::nextSibling() const {
    if (!valid()) {
        return ElementRef{};
    }
    const NodeArena& arena = doc->arena();
    for (NodeId s = arena.nextSibling(node); s != kInvalidNode; s = arena.nextSibling(s)) {
        if (arena.kind(s) == NodeKind::Element) {
            return makeRef(doc, s);
        }
    }
    return ElementRef{};
}

// -- T-387 (13 §13.9, 08 §8.12): scroll API ---------------------------------------------------------

namespace {

/// True when `ref` has an `overflow: scroll` / `auto` axis (the scroll API's precondition).
bool hasScrollAxis(const ElementRef& ref) {
    const ElementData& data = ref.doc->arena().element(ref.node);
    auto scrollable = [](Overflow o) { return o == Overflow::Scroll || o == Overflow::Auto; };
    return scrollable(usedOverflowX(data.style)) || scrollable(usedOverflowY(data.style));
}

/// The new offset on one axis that shows [start, start + size) inside a visible window of
/// `view` px currently at `cur`, per `align`.
float alignOffset(float cur, float view, float start, float size, ScrollAlign align) {
    switch (align) {
        case ScrollAlign::Start:
            return start;
        case ScrollAlign::Center:
            return start + size * 0.5f - view * 0.5f;
        case ScrollAlign::End:
            return start + size - view;
        case ScrollAlign::Nearest:
            break;
    }
    if (start < cur || size > view) {
        return start;  // Above / left of the window, or larger than it: its start edge first.
    }
    if (start + size > cur + view) {
        return start + size - view;
    }
    return cur;
}

}  // namespace

Status ElementRef::scrollTo(float x, float y) {
    DispatchGuard dispatch(doc != nullptr ? doc->m_context : nullptr);  // T-260a: Scroll listeners.
    if (!valid()) {
        return Status::kInvalidArgument;
    }
    if (!hasScrollAxis(*this)) {
        return Status::kUnsupported;
    }
    doc->m_events->setScroll(doc->m_arena, node, x, y, /*notify=*/true);
    return Status::kOk;
}

Status ElementRef::scrollBy(float dx, float dy) {
    const Vec2 cur = scrollPos();
    return scrollTo(cur.x + dx, cur.y + dy);
}

Vec2 ElementRef::scrollPos() const {
    if (!valid()) {
        return Vec2{};
    }
    const ElementData& data = doc->arena().element(node);
    return Vec2{data.style.base[prop::ScrollX].f, data.style.base[prop::ScrollY].f};
}

Vec2 ElementRef::maxScroll() const {
    if (!valid()) {
        return Vec2{};
    }
    return EventDispatcher::maxScrollOf(doc->arena(), node);
}

Status ElementRef::scrollIntoView(ElementRef child, ScrollAlign align) {
    if (!valid() || !child.valid() || child.doc != doc) {
        return Status::kInvalidArgument;
    }
    if (!hasScrollAxis(*this)) {
        return Status::kUnsupported;
    }
    const NodeArena& arena = doc->arena();
    if (child.node == node || !arena.isAncestorOf(node, child.node)) {
        return Status::kInvalidArgument;
    }
    // The child's border box in this container's (unscrolled) content coordinates: each box is
    // placed in its parent's content box, which an intermediate scroll container offsets by its
    // own applied scroll.
    const LayoutBox& childBox = arena.element(child.node).box;
    Vec2 pos{childBox.x, childBox.y};
    for (NodeId p = arena.parent(child.node); p != kInvalidNode && p != node; p = arena.parent(p)) {
        if (arena.kind(p) != NodeKind::Element) {
            continue;
        }
        const ElementData& pd = arena.element(p);
        const Rect content = pd.box.contentBox();
        const Vec2 scrolled = appliedScroll(pd);
        pos.x += pd.box.x + content.x - scrolled.x;
        pos.y += pd.box.y + content.y - scrolled.y;
    }
    const Rect view = arena.element(node).box.contentBox();
    const Vec2 cur = scrollPos();
    const float x = alignOffset(cur.x, view.w, pos.x, childBox.width, align);
    const float y = alignOffset(cur.y, view.h, pos.y, childBox.height, align);
    return scrollTo(x, y);
}

Status ElementRef::scrollIntoView(std::string_view repeatId, uint32_t index, ScrollAlign align) {
    if (!valid()) {
        return Status::kInvalidArgument;
    }
    if (!hasScrollAxis(*this)) {
        return Status::kUnsupported;
    }
    doc->layoutIfDirty();  // T-394 (08.15): an entry inserted this frame has its place now.
    if (!valid()) {
        return Status::kInvalidArgument;
    }
    RepeatController::ItemSpan span;
    if (!doc->m_repeats->itemSpan(repeatId, index, span)) {
        return Status::kInvalidArgument;
    }
    const NodeArena& arena = doc->arena();
    if (span.element != node && !arena.isAncestorOf(node, span.element)) {
        return Status::kInvalidArgument;
    }
    if (!span.virtualized) {
        return scrollIntoView(ElementRef{doc, span.instance, arena.generation(span.instance)}, align);
    }
    // The entry's top in this container's (unscrolled) content coordinates: its offset in the
    // repeat's content box, then every box from the repeat up to this container, as above.
    float top = span.top - arena.element(span.element).box.contentBox().y;
    for (NodeId p = span.element; p != kInvalidNode && p != node; p = arena.parent(p)) {
        if (arena.kind(p) != NodeKind::Element) {
            continue;
        }
        const ElementData& pd = arena.element(p);
        top += pd.box.y + pd.box.contentBox().y - appliedScroll(pd).y;
    }
    const Rect view = arena.element(node).box.contentBox();
    const Vec2 cur = scrollPos();
    return scrollTo(cur.x, alignOffset(cur.y, view.h, top, span.height, align));
}

}  // namespace xt
