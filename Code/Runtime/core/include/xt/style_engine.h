#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xt/animator.h"
#include "xt/asset_resolver.h"
#include "xt/ir.h"
#include "xt/layout.h"
#include "xt/node_arena.h"
#include "xt/scene_instance.h"
#include "xt/types.h"

/// Runtime style resolution: selector matching, cascade, inheritance, ComputedStyle, transition
/// registration and CSS `animation` play/stop. See Docs/design/05-style-and-css.md §5.4, §5.7-5.8,
/// Docs/design/04-timeline.md §4.6, Docs/tasks/T-24.md.

namespace xt {

class Document;
class TextShaper;

/// One design-token value held outside the IR (Docs/design/05-style-and-css.md §5.9.2,
/// 13-api.md §13.8.2, T-170): a theme's `--name: value` or a Context::setToken() override.
struct TokenValue {
    /// The typed value (Float, Length, Color or Enum); meaningful only when `hasValue` is true.
    Value value{};
    /// True when `value` holds a typed value.
    bool hasValue = false;
    /// The value's source text (a theme file's), re-parsed per property when the typed value does
    /// not fit it (e.g. an enum keyword). Empty for a setToken() value.
    std::string text;
};

/// Token name (with its leading `--`) -> value.
using TokenMap = std::map<std::string, TokenValue, std::less<>>;

/// T-218 (Docs/design/31-vector-authoring.md §31.5.1): one declaration of a scene-target rule
/// (`#Icon/Blade { fill: ... }`) matched for an element, already decoded to the node property's
/// Value (var()/calc()/em resolved). An element's SceneOverrideSet is in cascade order (later
/// wins per (node, prop)).
struct SceneOverrideDecl {
    /// IrSceneTarget code: 0 name, 1 tag, 2 any.
    uint8_t targetKind = 0;
    /// The node name (Name) or SVG tag name (Tag) StringId; kInvalidString for Any.
    StringId target = kInvalidString;
    /// PropId written on the matching scene nodes.
    PropId prop = 0;
    /// The decoded value (a Path points into the IR's STYL floatPool).
    Value value{};
    /// The rule's specificity and source order (the cascade key the set is sorted by).
    uint32_t specificity = 0;
    uint32_t order = 0;
    /// `!important`.
    bool important = false;
};

/// T-218: the scene-target declarations matched for one element, in cascade order.
using SceneOverrideSet = std::vector<SceneOverrideDecl>;

/// T-218: one (scene node, prop) value StyleEngine wrote for an element's scene-target rules.
struct SceneOverrideWrite {
    NodeId node = kInvalidNode;  ///< Scene node written.
    PropId prop = 0;             ///< Property written.
    uint32_t local = 0;          ///< The node's local index in the IR scene.
    Value value{};               ///< The value written.
};

/// T-218: the IR value of a (scene node, prop) before its first override, restored when no rule
/// overrides it any more. `verbs` owns a Path base's verbs (as floats); `value.pathVerbs` is
/// re-pointed at it when restoring.
struct SceneOverrideBase {
    NodeId node = kInvalidNode;
    PropId prop = 0;
    Value value{};
    std::vector<float> verbs;
};

/// T-218: per-element scene-override state: the instance it applies to (a changed instance root
/// drops the state without restoring anything), the base snapshot and the last resolve's writes.
struct SceneOverrideState {
    NodeId sceneRoot = kInvalidNode;
    SceneId scene = kInvalidScene;
    std::vector<SceneOverrideBase> base;
    std::vector<SceneOverrideWrite> applied;
};

/// Resolves CSS-subset styles for element nodes: selector matching, cascade, inheritance,
/// ComputedStyle, and driving Animator for transitions and `animation` declarations.
class StyleEngine {
public:
    /// Borrows `ir`/`animator`/`shaper`; the caller (Document) guarantees all three outlive the
    /// StyleEngine. `shaper` may be null, in which case every ComputedStyle::resolvedFont stays
    /// kInvalidFont (Docs/tasks/T-30.md goal 1) and Text nodes lay out as 0-size.
    void init(const IrView& ir, Animator& animator, TextShaper* shaper = nullptr);

    /// Binds Document's runtime effect declaration pool (Docs/design/18-effects.md §18.4,
    /// Docs/tasks/T-51b.md) so resolve() can dereference a ComputedStyle::effectOffset whose high
    /// bit (kRuntimeEffectOffsetBit, xt/node.h) marks it as pointing there instead of the IR's
    /// STYL `effects[]`/`effectStops[]`. Mirrors LayoutEngine::setRuntimeStrings' pointer-binding
    /// pattern (xt/layout.h); called once from document_load.cpp. Both pointers must outlive this
    /// StyleEngine; unset (nullptr, the default) means a runtime `effect` declaration is treated
    /// as empty -- only relevant to tests that use StyleEngine without a Document.
    void setRuntimeEffects(const std::vector<EffectInstance>* effects,
                           const std::vector<GradientStop>* stops) {
        m_runtimeEffects = effects;
        m_runtimeEffectStops = stops;
    }

    /// Binds Document's runtime `background-gradient` declaration pool (05.5c, T-115), the
    /// gradient counterpart of setRuntimeEffects() above: resolve() dereferences a
    /// ComputedStyle::gradientOffset carrying kRuntimeEffectOffsetBit into it. The pointer must
    /// outlive this StyleEngine; unset (nullptr, the default) means a runtime
    /// `background-gradient` declaration resolves to `none`.
    void setRuntimeGradients(const std::vector<ElementGradient>* gradients) {
        m_runtimeGradients = gradients;
    }

    /// Binds Document's runtime `clip-path` shape pool (05 §5.9.8, T-332), the clip-path
    /// counterpart of setRuntimeGradients() above: resolve() dereferences a
    /// ComputedStyle::clipShape carrying kRuntimeEffectOffsetBit into it. The pointer must outlive
    /// this StyleEngine; unset (nullptr, the default) means a runtime `clip-path` declaration
    /// resolves to `none`.
    void setRuntimeClipShapes(const std::vector<ElementClipShape>* shapes) { m_runtimeClipShapes = shapes; }

    /// Binds Document's runtime `animation` declaration pool (17.6, T-123), the animation
    /// counterpart of setRuntimeEffects() above: resolve()'s §5.7 step 6 `animation` diff
    /// dereferences a ComputedStyle::animationOffset carrying kRuntimeEffectOffsetBit into it.
    /// The pointer must outlive this StyleEngine; unset (nullptr, the default) means a runtime
    /// `animation` declaration plays nothing.
    void setRuntimeAnimations(const std::vector<IrAnimationSpec>* animations) {
        m_runtimeAnimations = animations;
    }

    /// T-199 (26 §26.3.4): binds the Document's AssetResolver so resolve() can read an image's
    /// atlas cell and manifest slice (AssetResolver::imageMeta) into ElementData::backgroundSubUv,
    /// borderImageSubUv, borderImageSize and borderImageSlicePx. Unset (nullptr, the default): the
    /// whole image, `auto` slices resolve to 0.
    void setAssetResolver(AssetResolver* assets) { m_assets = assets; }

    /// T-218 (31 §31.5.1): binds Document's scene instances (indexed by
    /// ElementData::sceneInstanceIndex) so scene-target rules can reach an element's inline
    /// `<svg>` nodes. Unset (nullptr, the default): such rules are matched but write nothing.
    void setSceneInstances(const std::vector<SceneInstance>* instances) { m_sceneInstances = instances; }

    /// T-218: the scene-override state of `element` (null when no scene-target rule applies to it
    /// right now). For tests and tools.
    const SceneOverrideState* sceneOverrides(NodeId element) const {
        auto it = m_sceneOverrides.find(element);
        return it != m_sceneOverrides.end() ? &it->second : nullptr;
    }

    /// Recomputes ComputedStyle for every Element node in `arena` whose dirty bits include
    /// kDirtyStyle. Elements are visited in increasing NodeId order, which processes parents
    /// before children given this codebase's node-creation invariant (a node's parent is always
    /// created, hence assigned a smaller NodeId, before the node itself -- see
    /// Code/Runtime/core/src/document_load.cpp and NodeArena::create()); a future dynamic-reparenting API that
    /// breaks that invariant would need a real topological/preorder walk instead (uncovered case,
    /// see Docs/tasks/T-24 report). Returns the number of Elements processed.
    uint32_t resolve(NodeArena& arena, float clock);

    /// T-170 (13.8.2): binds the token layers a `var(--name)` declaration is looked up in, most
    /// specific first: `overrides` (Context::setToken), then `theme` (Context::setTheme), then the
    /// IR's own `VARS` table. Either pointer may be null; both must outlive this StyleEngine or be
    /// rebound. Does not mark anything dirty (see markTokenUsersDirty()).
    void setTokenLayers(const TokenMap* theme, const TokenMap* overrides) {
        m_themeTokens = theme;
        m_overrideTokens = overrides;
    }

    /// T-170: Document's runtime string pool, so a runtime `var()` declaration's interned name
    /// (a kRuntimeStringBase-tagged StringId) can be read back. Same contract as
    /// LayoutEngine::setRuntimeStrings().
    void setRuntimeStrings(const std::deque<std::string>* strings) { m_runtimeStrings = strings; }
    /// T-270d: the owning Document, through whose string() every text value the cascade reads
    /// resolves -- IR strings, the runtime string pool and the node-owned runtime text slots
    /// (kRuntimeTextBase: setStyle's text values, a `<repeat>` instance id) alike. nullptr (the
    /// default, a Document-less test StyleEngine) = the IR plus setRuntimeStrings()'s pool.
    void setDocument(const Document* doc) { m_doc = doc; }

    /// T-179 (13.8.2): the document language `:lang()` matches before the elements' own `lang`
    /// attributes; empty = attributes only. Does not mark anything dirty (Document does that).
    void setDocumentLanguage(std::string_view lang) { m_documentLang.assign(lang); }

    /// T-170 (13.8.2): marks kDirtyStyle every element whose last resolve() matched a `var()`
    /// declaration, and its whole subtree (inherited properties follow it). Returns the number of
    /// such elements.
    uint32_t markTokenUsersDirty(NodeArena& arena);

    /// T-395 (08 §8.16): the Color value of token `name` (`--placeholder-color`) through the same
    /// layers a `var()` declaration on `color` reads (overrides, theme, the IR's `VARS`). False when
    /// no layer defines it as a color.
    bool tokenColor(std::string_view name, Color& out) const;

    /// T-176 (05.9.3, 12.7): evaluates every `@media` block of the IR against `env`'s viewport and
    /// `device`, and for each STYL rule whose active state changed marks kDirtyStyle every element
    /// the rule's selector matches right now, plus its subtree. The first call only records the
    /// states (every element is dirty after load anyway). Returns the number of rules that
    /// changed state.
    uint32_t updateMedia(NodeArena& arena, const LengthEnv& env, const DeviceInfo& device);

    /// T-176: whether STYL rule `rule` currently applies (false only for a rule inside an `@media`
    /// that does not hold).
    bool ruleActive(uint32_t rule) const { return rule >= m_ruleActive.size() || m_ruleActive[rule] != 0; }

    /// T-345 (05 §5.9.9): the Document's uiscale and DeviceInfo::dpr, from which resolve() derives
    /// each element's ComputedStyle::uiScaleEff (`auto` = uiScale, `none` = dpr, `clamp` = uiScale
    /// clamped). Returns true when either changed; the caller then marks every element
    /// kDirtyStyle (every px length is rescaled).
    bool setScaleInputs(float uiScale, float dpr);

    /// T-176: marks kDirtyStyle every element whose last resolve() turned a viewport/safe-area/
    /// uiscale unit or `calc()` into px at style time (`font-size`, translate), plus its subtree.
    /// Returns the number of such elements.
    uint32_t markDeviceUsersDirty(NodeArena& arena);

    /// T-176: the runtime `calc()` pool (programs whose `em`/`var()` terms were substituted by
    /// resolve(); LengthUnit::Calc index kRuntimeCalcBase + i). Append-only; Document binds it into
    /// its LengthEnv.
    const std::vector<IrCalc>& runtimeCalcs() const { return m_runtimeCalcs; }
    const std::vector<IrCalcOp>& runtimeCalcOps() const { return m_runtimeCalcOps; }

    /// Called (if set) once per CSS `animation` instance created by resolve()'s §5.7 step 6 diff,
    /// right after Animator::play() returns its instance id. Document uses this to record
    /// instanceId -> element for AnimationEnd/Iteration/Marker bubbling (Docs/tasks/T-32.md "動畫
    /// 事件"); unused (no-op) when unset, e.g. in tests that use StyleEngine without a Document.
    std::function<void(uint32_t instanceId, NodeId node)> onAnimationPlay;

    /// T-173 (05.9.2): true when the IR's selectors use `:nth-child()`, `:first-child` or
    /// `:last-child`, so adding or removing a child must re-resolve its siblings (Document does).
    bool usesStructuralSelectors() const { return m_usesStructural; }

private:
    /// T-199: see setAssetResolver().
    void resolveImageMeta(const IrView& ir, const ComputedStyle& next, ElementData& data) const;

    const IrView* m_ir = nullptr;
    Animator* m_animator = nullptr;
    TextShaper* m_shaper = nullptr;
    // T-199: the Document's resolver, bound by setAssetResolver().
    AssetResolver* m_assets = nullptr;
    // T-218: scene-target rules -- the Document's instances, per-element state, and resolve()'s
    // scratch for the current element's set and its per-(node, prop) winners.
    const std::vector<SceneInstance>* m_sceneInstances = nullptr;
    std::unordered_map<NodeId, SceneOverrideState> m_sceneOverrides;
    SceneOverrideSet m_sceneSet;
    std::vector<SceneOverrideWrite> m_sceneWinners;

    /// T-218: decodes one scene-target declaration (IR `decl` or resolved runtime `runtime`) to
    /// `prop`'s node Value: lengths to px (em against `fontPx`, calc() instantiated), a `d`
    /// ListRef to a Path. False when the value does not fit the property.
    bool decodeSceneValue(const IrDecl* decl, const RuntimeStyleDecl* runtime, PropId prop, float fontPx,
                          NodeId element, Value& out);
    /// T-218: writes m_sceneSet to `element`'s scene nodes (restoring the ones no rule overrides
    /// any more), with transitions per `next`'s `transition` list.
    void applySceneOverrides(NodeArena& arena, NodeId element, const ElementData& data,
                             const ComputedStyle& next, float clock, bool firstResolve);
    // Scratch for the FontRequest family list (string_views into the IR string table), refilled
    // once per element; resolve() is single-threaded, so reuse across elements is safe.
    std::vector<std::string_view> m_familyScratch;
    // T-51b: Document's runtime effect declaration pool, bound by setRuntimeEffects(); see there.
    const std::vector<EffectInstance>* m_runtimeEffects = nullptr;
    const std::vector<GradientStop>* m_runtimeEffectStops = nullptr;
    // T-115: Document's runtime `background-gradient` pool, bound by setRuntimeGradients().
    const std::vector<ElementGradient>* m_runtimeGradients = nullptr;
    // T-332: Document's runtime `clip-path` shape pool, bound by setRuntimeClipShapes().
    const std::vector<ElementClipShape>* m_runtimeClipShapes = nullptr;
    // T-123: Document's runtime `animation` pool, bound by setRuntimeAnimations().
    const std::vector<IrAnimationSpec>* m_runtimeAnimations = nullptr;
    // T-170: token layers (setTokenLayers), Document's runtime strings (setRuntimeStrings), the
    // per-NodeId "matched a var() declaration last resolve" flags, and resolve()'s scratch for the
    // declarations a var() resolved to (reserved up front so Entry pointers into it stay valid).
    const TokenMap* m_themeTokens = nullptr;
    const TokenMap* m_overrideTokens = nullptr;
    const std::deque<std::string>* m_runtimeStrings = nullptr;
    const Document* m_doc = nullptr;  // T-270d: setDocument().
    // T-179: setDocumentLanguage().
    std::string m_documentLang;
    std::vector<uint8_t> m_tokenUsers;
    std::vector<RuntimeStyleDecl> m_varScratch;
    // T-173: selector features the IR uses (init()), resolve()'s per-NodeId Element-child index
    // (`:nth-child`) and `:focus-within` flags (element or a descendant focused, as of the last
    // resolve), and whether `:focus-within` appears in a non-subject compound (then a change
    // re-resolves whole subtrees, not just the changed elements).
    bool m_usesStructural = false;
    bool m_usesFocusWithin = false;
    bool m_focusWithinDeep = false;
    std::vector<uint32_t> m_childIndex;
    std::vector<uint8_t> m_focusWithin;
    std::vector<uint8_t> m_focusWithinNext;

    void updateFocusWithin(NodeArena& arena);

    // T-176: per-rule `@media` state (empty = every rule active), the per-NodeId "used a device
    // unit at style time" flags, the runtime calc pool and its build scratch.
    std::vector<uint8_t> m_ruleActive;
    bool m_mediaEvaluated = false;
    // T-345: per STYL rule, 1 = inside an `@media` block (what `media-override` skips); the
    // setScaleInputs() values; and resolve()'s current element's factor for instantiateCalc()
    // (`m_calcAuto`: the factor is the plain uiscale, `ui-scale: auto`).
    std::vector<uint8_t> m_ruleIsMedia;
    float m_uiScale = 1.0f;
    float m_dpr = 1.0f;
    float m_calcScale = 1.0f;
    bool m_calcAuto = true;
    std::vector<uint8_t> m_deviceUsers;
    std::vector<IrCalc> m_runtimeCalcs;
    std::vector<IrCalcOp> m_runtimeCalcOps;
    std::vector<IrCalcOp> m_calcBuild;
    /// T-335: the root element's resolved font size in px (what `rem` multiplies) for the tree
    /// resolve() is walking; 16 until that root has been resolved once. T-345: unscaled (before
    /// the root's `ui-scale` factor).
    float m_rootFontPx = 16.0f;

    std::string_view stringOf(StringId id) const;
    bool lookupToken(std::string_view name, uint16_t prop, Value& out) const;
    /// T-176: a token's typed number or length for a `calc()` term.
    bool lookupCalcToken(std::string_view name, Value& out) const;
    /// T-176: IR program `index` with its `em` terms resolved against `emPx` and its `var()` terms
    /// substituted: the IR index itself when it has neither, else a (deduplicated) runtime pool
    /// entry. False when a `var()` has neither a value nor a fallback. `usedToken` is set when a
    /// `var()` was looked up.
    bool instantiateCalc(uint32_t index, float emPx, Length& out, bool& usedToken);
};

}  // namespace xt
