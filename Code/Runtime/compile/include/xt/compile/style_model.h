#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/ir_pod.h"  // CalcOpKind, MediaFeature (T-176).
#include "xt/node.h"    // EffectKind (Docs/design/18-effects.md §18.2) and GradientKind (05.5c).
#include "xt/timeline.h"
#include "xt/types.h"
#include "xt/value.h"

/// Intermediate model produced by parsing a `.xts` stylesheet (or a single `style="..."`
/// declaration list): RuleSet (rules, `@keyframes`, `@font-face`), plus the parse entry points.
/// See Docs/design/05-style-and-css.md §5.3-5.6, Docs/tasks/T-22.md.

namespace xt::compile {

/// Shape of a parsed StyleValue. Mirrors the value categories used by the PropId/StyleProp value
/// types (Docs/design/05-style-and-css.md §5.4) plus the list-shaped `transition`/`animation`/
/// `font-family` values. `Shorthand` is not a value shape produced in a StyleDecl; it only appears
/// as PropInfo::kind to mark a CSS name that expands into several StyleDecls (see xts_props.h).
enum class StyleValueKind : uint8_t {
    Float = 0,           ///< Plain float; value stored in `StyleValue::f`.
    Length = 1,          ///< Length value; value stored in `StyleValue::len`.
    Color = 2,           ///< Color value; value stored in `StyleValue::color`.
    Enum = 3,            ///< Resolved enum value; value stored in `StyleValue::e`.
    String = 4,          ///< String value; value stored in `StyleValue::str`.
    TransitionList = 5,  ///< `transition` list; value stored in `StyleValue::transitions`.
    AnimationList = 6,   ///< `animation` list; value stored in `StyleValue::animations`.
    FontFamilyList = 7,  ///< `font-family` list; value stored in `StyleValue::families`.
    Neutral = 8,         ///< No stored value (e.g. `initial`/`inherit`-style keywords).
    Shorthand = 9,       ///< Marks a CSS name that expands into several StyleDecls; never
                         ///< produced as an actual StyleValue's kind.
    EffectList = 10,     ///< `effect` sequence (Docs/design/18-effects.md §18.4); value stored in
                         ///< `StyleValue::effects`.
    Gradient = 11,       ///< `background-gradient` (Docs/design/05-style-and-css.md §5.5c); value
                         ///< stored in `StyleValue::gradient`.
    VarRef = 12,         ///< T-170 (05.9.2): `var(--name[, fallback])` as a plain-valued longhand's
                         ///< whole value, kept as a reference into the `VARS` table; name in
                         ///< `StyleValue::varName`, optional fallback in `StyleValue::varFallback`.
    VarSubst = 13,       ///< T-170: a declaration with `var()` nested in its value (or on a
                         ///< list-valued or shorthand property); substituted at compile time once
                         ///< every token is known. `StyleValue::str` holds the whole declaration
                         ///< text, `name: value`, and `StyleDecl::prop` may be kShorthandProp.
    Calc = 14,           ///< T-176 (05.9.2, 06.4): `calc(<expr>)` on a Length longhand; the RPN
                         ///< program is in `StyleValue::calc`.
    FloatArray = 15,     ///< T-236: `stroke-dasharray` (px or plain numbers; `none` = empty);
                         ///< value stored in `StyleValue::floats`.
    Vec2 = 16,           ///< T-236: an `<x> <y>` modifier parameter (`repeat-anchor`,
                         ///< `repeat-translate`, `center`); value stored in `StyleValue::floats[0..1]`.
    ClipShape = 17,      ///< T-332 (05 §5.9.8): `clip-path`; value stored in `StyleValue::clipShape`.
    GridTracks = 18,     ///< T-338 (06 §6.15): `grid-template-columns` / `-rows`; value stored in
                         ///< `StyleValue::gridTracks` (empty = `none`).
    GridAreas = 19,      ///< T-338: `grid-template-areas`; value stored in `StyleValue::gridAreas`
                         ///< and `gridAreaRows` / `gridAreaCols` (0 rows = `none`).
};

/// One named area of a parsed `grid-template-areas` (T-338): 0-based lines, end exclusive.
struct GridAreaDecl {
    std::string name;
    uint16_t rowStart = 0;
    uint16_t rowEnd = 0;
    uint16_t colStart = 0;
    uint16_t colEnd = 0;
};

/// One RPN instruction of a parsed `calc()` (T-176); calc_emit.cpp writes it as an IrCalcOp.
struct CalcOpDecl {
    /// What the instruction does.
    CalcOpKind op = CalcOpKind::PushNumber;
    /// PushLength: the operand's unit; PushVar: the fallback's unit (when it is a length).
    LengthUnit unit = LengthUnit::Px;
    /// PushLength/PushNumber: the operand; PushVar: the fallback's value.
    float value = 0;
    /// PushVar: the token name, with its leading `--`.
    std::string varName;
    /// PushVar: whether `value`/`unit` hold a fallback, and whether that fallback is a number.
    bool hasFallback = false;
    bool fallbackIsNumber = false;
};

/// One `(feature ...)` of an `@media` query (T-176); value encoding per MediaFeature (ir_pod.h).
struct MediaCondDecl {
    MediaFeature feature = MediaFeature::MinWidth;
    float value = 0;
};

/// One `@media <query>, <query> { ... }` block (05.9.2-5.9.3, T-176): the rules inside carry its
/// index in RuleSet::media as StyleRule::media.
struct MediaDef {
    /// Comma-separated queries (any holds); each is `and`-joined conditions (all hold).
    std::vector<std::vector<MediaCondDecl>> queries;
    /// Source file name and 1-based line of the `@media`, for diagnostics.
    std::string file;
    int line = 0;
};

/// A parsed `background-gradient` value (05.5c, T-115). `kind == GradientKind::None` is `none`
/// (no gradient), and then every other field is unused. Angles are already normalized to degrees
/// by the parser; lengths keep their unit (`%` is resolved against the element's box at paint
/// time, so the compiler cannot fold it).
struct GradientDecl {
    /// Linear, Radial, or None for `background-gradient: none`.
    GradientKind kind = GradientKind::None;
    /// Linear: gradient-line angle in degrees (0 = towards the top, clockwise). Conic (T-201): the
    /// start angle.
    float angle = 0;
    /// Radial: rx, ry, cx, cy (05.5c's `<len> <len> at <len> <len>`). Conic: cx, cy.
    Length rx, ry, cx, cy;
    /// The color stops, in source order, with omitted positions already distributed.
    std::vector<GradientStop> stops;
};

/// One `<ident> <value>` override inside `custom()` (T-203, Docs/design/18-effects.md §18.4):
/// `value[]` holds one number (float), two numbers (vec2) or an RGBA color; `count` says which
/// (1, 2 or 4). The parameter's declared type is checked against it at emit time.
struct CustomParamOverride {
    std::string name;
    float value[4] = {0, 0, 0, 0};
    uint8_t count = 0;
    /// True when the value was written as a color (`#hex`).
    bool isColor = false;
    int line = 0;
};

/// One `effect:` sequence entry, as parsed from one `<effect-fn>` (Docs/design/18-effects.md
/// §18.3-18.4, Docs/tasks/T-51a.md). On-disk layout (`IrEffect`/`EffectInstance`) additionally
/// carries `enabled`/`shader`, which this compiler-internal shape doesn't need (`enabled` is
/// always 1 for a parsed `effect:` entry per T-51a; `shader` is resolved by style_emit.cpp from
/// `customAsset` for `EffectKind::Custom`, T-203).
struct EffectDeclEntry {
    /// Which built-in effect this entry is.
    EffectKind kind = EffectKind::None;
    /// paramIndex 1..8 (18.2); meaning depends on `kind` (18.3's parameter table). Lengths and
    /// angles are already resolved to plain numbers (px, degrees) by the parser.
    float params[8] = {};
    /// paramIndex 16/17 (18.2).
    Color colors[2]{};
    /// `GradientMask`'s color stops; empty for every other kind.
    std::vector<GradientStop> stops;
    /// T-203 (18.4 `custom("<asset>" [, <ident> <value>]*)`): the `.xtfx` asset name as written
    /// and the parameter overrides, in source order. Empty for every other kind. `params`/`colors`
    /// are filled by style_emit.cpp from the asset's header (defaults, then these overrides).
    std::string customAsset;
    std::vector<CustomParamOverride> customParams;
    /// T-203: the `.xtfx` asset's ASST index, set by resolveCustomEffects() (style_emit.h);
    /// kInvalidAsset until then and for every other kind.
    AssetId shaderAsset = kInvalidAsset;
};

/// One item of a `transition` list. `prop == 0xFFFF` means `all`.
struct TransitionSpec {
    /// Property this transition applies to, or `0xFFFF` for `all`.
    PropId prop = 0xFFFF;
    /// Transition duration, in seconds.
    float duration = 0;
    /// Delay before the transition starts, in seconds.
    float delay = 0;
    /// Timing function.
    Easing easing;
};

/// One item of an `animation` list. `direction`/`fill`/`playState` are small enums encoded as in
/// xts_props.h (AnimDirection/AnimFill/AnimPlayState).
struct AnimationSpec {
    /// `@keyframes` name; resolved to a `TimelineId` at compile time.
    std::string name;
    /// Animation duration, in seconds.
    float duration = 0;
    /// Delay before the animation starts, in seconds.
    float delay = 0;
    /// Timing function.
    Easing easing;
    /// Iteration count; `0xFFFF` means infinite.
    uint16_t iterations = 1;
    /// AnimDirection (xts_props.h): 0 normal, 1 reverse, 2 alternate, 3 alternate-reverse.
    uint8_t direction = 0;
    /// AnimFill (xts_props.h).
    uint8_t fill = 0;
    /// AnimPlayState (xts_props.h): 0 running, 1 paused.
    uint8_t playState = 0;
};

/// A single parsed value. Which member is meaningful is given by `kind`.
struct StyleValue {
    /// Which member below holds the value.
    StyleValueKind kind = StyleValueKind::Neutral;
    /// Valid when `kind == Float`.
    float f = 0;
    /// Valid when `kind == Length`.
    Length len;
    /// Valid when `kind == Color`.
    Color color;
    /// Valid when `kind == Enum`; the resolved enum value.
    uint32_t e = 0;
    /// Valid when `kind == String`.
    std::string str;
    /// Valid when `kind == TransitionList`.
    std::vector<TransitionSpec> transitions;
    /// Valid when `kind == AnimationList`.
    std::vector<AnimationSpec> animations;
    /// Valid when `kind == FontFamilyList`.
    std::vector<std::string> families;
    /// Valid when `kind == EffectList`.
    std::vector<EffectDeclEntry> effects;
    /// Valid when `kind == Gradient`.
    GradientDecl gradient;
    /// Valid when `kind == ClipShape` (T-332): the parsed `clip-path` basic shape (`kind == None`
    /// for `clip-path: none`); emitted into the IR's `CLIP` chunk.
    ElementClipShape clipShape;
    /// Valid when `kind == GridTracks` (T-338): the tracks, `repeat()` expanded; empty = `none`.
    std::vector<GridTrack> gridTracks;
    /// Valid when `kind == GridAreas` (T-338): the named areas and the matrix size (0 x 0 = `none`).
    std::vector<GridAreaDecl> gridAreas;
    uint16_t gridAreaRows = 0;
    uint16_t gridAreaCols = 0;
    /// Valid when `kind == VarRef`: the token name, including its leading `--`.
    std::string varName;
    /// Valid when `kind == VarRef`: empty, or exactly one parsed fallback value.
    std::vector<StyleValue> varFallback;
    /// Valid when `kind == Calc`: the program, in RPN order.
    std::vector<CalcOpDecl> calc;
    /// Valid when `kind == FloatArray` (any count) or `kind == Vec2` (exactly two).
    std::vector<float> floats;
};

/// One declaration: `prop = PropId 或 StyleProp（05.4 編號）`.
struct StyleDecl {
    /// PropId or StyleProp number (§5.4).
    uint16_t prop = 0xFFFF;
    /// Parsed value.
    StyleValue value;
    /// True if the declaration carried `!important`.
    bool important = false;
    /// 1-based source line.
    int line = 0;
};

/// T-173 (05.9.2): kind of an extra per-compound selector condition (SelectorCond). The numeric
/// values are the IR's condition codes (xt/ir_pod.h's IrSelPart with kIrSelCondition), so they
/// must only ever be appended to.
enum class SelectorCondKind : uint8_t {
    Tag = 1,             ///< `:not(tag)` only (a plain tag lives in SelectorPart::tag); `name`.
    Id = 2,              ///< `:not(#id)` only; `name`.
    Class = 3,           ///< `:not(.class)` only; `name`.
    State = 4,           ///< `:not(:hover)` etc. only; `state` holds the ElementState bit.
    Any = 5,             ///< `:not(*)` only (never matches).
    AttrExists = 6,      ///< `[name]`.
    AttrEquals = 7,      ///< `[name=value]`.
    AttrPrefix = 8,      ///< `[name^=value]`.
    AttrSuffix = 9,      ///< `[name$=value]`.
    AttrSubstring = 10,  ///< `[name*=value]`.
    NthChild = 11,       ///< `:nth-child(an+b)`; `a`, `b`.
    FirstChild = 12,     ///< `:first-child`.
    LastChild = 13,      ///< `:last-child`.
    FocusWithin = 14,    ///< `:focus-within` (the element or a descendant has focus).
    Lang = 15,           ///< `:lang(prefix)`; `name` is the BCP 47 prefix.
    CustomState = 16,    ///< T-174 (27.1): `:state(<Name>)`; `name` is the state name.
    FocusPlayer = 17,    ///< T-194 (08.9): `:focus-player(<n>)`; `a` is the player index (0-7).
};

/// T-173: one extra condition on a compound selector beyond tag/id/classes/state bits: an
/// attribute selector, a structural or `:focus-within`/`:lang()` pseudo-class, or one simple
/// selector of a `:not(...)` list (`negated`; every simple selector of one `:not()` shares the same
/// nonzero `notGroup`, which only specificity uses).
struct SelectorCond {
    /// What is tested.
    SelectorCondKind kind = SelectorCondKind::Any;
    /// True for a `:not()` argument: the condition must NOT hold.
    bool negated = false;
    /// Which `:not()` of the selector this argument belongs to (1-based); 0 when not negated.
    uint16_t notGroup = 0;
    /// Tag, id, class, attribute name or `:lang()` prefix, per `kind`.
    std::string name;
    /// Attribute value for AttrEquals/Prefix/Suffix/Substring.
    std::string value;
    /// ElementState bit for State.
    uint16_t state = 0;
    /// `an+b` of NthChild.
    int32_t a = 0;
    int32_t b = 0;
};

/// T-218 (31 §31.5.1): what a `#<svgId>/<target>` subject addresses inside the element's inline
/// `<svg>` scene. The IR code (IrSceneTarget) is the value minus one.
enum class SceneTargetKind : uint8_t {
    None = 0,  ///< The element itself (an ordinary selector).
    Name = 1,  ///< `#Icon/Blade`: the scene nodes named `Blade`.
    Tag =
        2,  ///< `#Icon/path`: every node of that SVG tag (path|rect|circle|ellipse|line|polygon|polyline|g).
    Any = 3,  ///< `#Icon/*`: every node but the scene root.
};

/// One compound selector, right-to-left: parts[0] is the subject, later parts are ancestors.
/// `combinator` describes how THIS part relates to the part before it in `parts` (i.e. the part
/// closer to the subject): 0 = descendant (whitespace), 1 = child (`>`). parts[0]'s combinator is
/// unused (always 0). Whitespace between two compounds is the descendant combinator (T-173); a
/// compound has no internal whitespace.
struct SelectorPart {
    /// Combinator to the previous (closer-to-subject) part: 0 = descendant, 1 = child.
    uint8_t combinator = 0;
    /// Tag name, or empty for `*`/no tag requirement.
    std::string tag;
    /// `#id` requirement, or empty if none.
    std::string id;
    /// `.class` requirements (all must match).
    std::vector<std::string> classes;
    /// Bitmask of required `ElementState` bits (from `:hover`/`:active`/etc pseudo-classes).
    uint16_t stateMask = 0;
    /// T-173: extra conditions (attribute selectors, structural/`:focus-within`/`:lang()`
    /// pseudo-classes, `:not()` arguments), all of which must hold, in source order.
    std::vector<SelectorCond> conds;
    /// T-218: a `/<name|tag|*>` after this compound's `#id` (only ever on the subject, parts[0]):
    /// the rule's declarations go to those nodes of the element's inline `<svg>` scene.
    SceneTargetKind sceneKind = SceneTargetKind::None;
    /// T-218: the node name or tag name for SceneTargetKind::Name / Tag; empty otherwise.
    std::string sceneTarget;
};

/// A full selector: `parts[0]` is the subject, `parts.back()` the outermost ancestor.
struct Selector {
    /// Compound-selector parts, subject-first (see `SelectorPart::combinator`).
    std::vector<SelectorPart> parts;
    /// `(idCount << 16) | ((classCount + pseudoCount) << 8) | tagCount`, summed across `parts`
    /// (selectorSpecificity(), parse_xts.h): attribute selectors count as classes, and a `:not()`
    /// counts as its most specific argument.
    uint32_t specificity = 0;
};

/// T-174 (Docs/design/27-interaction.md §27.2.1): one `.xts` trigger declaration, `on-<event>:
/// <actions>` or `state-enter|state-exit: <Name> <actions>` (stored as event `state(<Name>).enter`
/// / `.exit`). The event and action texts are parsed by trigger_emit.cpp.
struct TriggerDecl {
    /// Event text without the `on-` prefix, e.g. `hover`, `key(Enter)`, `timeline(intro).end`.
    std::string event;
    /// Action list text, e.g. `play(card-hover); state(Selected, toggle)`.
    std::string actions;
    /// 1-based source line.
    int line = 0;
};

/// A rule; a grouped selector list (`a, b { ... }`) keeps all selectors in one StyleRule (T-23
/// expands it into one rule per selector).
struct StyleRule {
    /// Selectors this rule applies to (all sharing `decls`).
    std::vector<Selector> selectors;
    /// Declarations in source order.
    std::vector<StyleDecl> decls;
    /// 1-based source line of the rule's opening selector.
    int line = 0;
    /// T-174: `on-<event>` / `state-enter` / `state-exit` declarations in source order. They are
    /// compiled into the `TRIG` chunk (not STYL) against the elements the rule matches statically.
    std::vector<TriggerDecl> triggers;
    /// Source file name of the rule (for trigger diagnostics); empty when unknown.
    std::string file;
    /// T-171: the `<Name>` of the enclosing `symbol(<Name>) { ... }` block (Docs/design/26-layers-
    /// symbols-library.md §26.2.1), or empty for an ordinary document-wide rule. A scoped rule's
    /// selectors are relative to the symbol; symbol_expand.cpp rewrites them per instance.
    std::string symbol;
    /// T-176: index into RuleSet::media of the enclosing `@media`, or -1 (always applies).
    int media = -1;
    /// T-235: for a `symbol()` rule rewritten per instance, where that instance is
    /// (`<file>:<line> <use id="...">`), appended to the rule's diagnostics; empty otherwise.
    std::string instanceSite;
};

/// One `from`/`to`/`N%` stop inside an `@keyframes` block.
struct KeyframeStop {
    /// Stop position, 0..100 (percent).
    float offset = 0;
    /// Declarations to apply at this offset.
    std::vector<StyleDecl> decls;
    /// This stop's timing function, valid only when `hasTiming` is true.
    Easing timing;
    /// True if this stop specified its own `animation-timing-function`-style easing.
    bool hasTiming = false;
    /// T-228 (31 §31.7.2): the spatial handles of a synthesized `@timeline` translate stop
    /// (`motion-in` / `motion-out`, px, relative to the stop's translate); see TimelineKey.
    bool hasMotion = false;
    Vec2 motionIn{};   ///< Arriving handle.
    Vec2 motionOut{};  ///< Leaving handle.
};

/// One `@keyframes name { ... }` block.
struct KeyframesDef {
    /// Keyframes name, referenced by `animation` declarations.
    std::string name;
    /// Stops in source order.
    std::vector<KeyframeStop> stops;
};

/// One `@font-face { ... }` block.
struct FontFaceDef {
    /// `font-family` value.
    std::string family;
    /// `src` value (font file path).
    std::string src;
    /// `font-weight`, 100..900.
    uint16_t weight = 400;
    /// True if `font-style: italic`.
    bool italic = false;
};

/// T-179 (07.8): one `@font-fallback <lang> { font-family: A, B, C; }` block -- the families text
/// shaping tries, in order, for a codepoint the element's own `font-family` list lacks while `lang`
/// is the document language.
struct FontFallbackDef {
    /// BCP 47 language tag, as written.
    std::string lang;
    /// Family names, most preferred first.
    std::vector<std::string> families;
};

/// One `effect.<effect>.<param>: <value>` declaration inside an `@timeline` key (Docs/design/
/// 28-animation-editing.md §28.1.4, 18-effects.md §18.12). The address stays symbolic here: the
/// effect index can only be resolved against the target element's effect sequence, which the
/// emitter looks up (style_emit.cpp's emitTimelineGroups).
struct TimelineEffectDecl {
    /// `<effect>`: an effect name (`glow`, `shadow`, ...) or a stack index written as digits.
    std::string effect;
    /// `<param>`: the 18.3 parameter name (`intensity`, `radius`, `color`, ...).
    std::string param;
    /// Parsed value: Float for a numeric parameter, Color for `color`.
    StyleValue value;
    /// 1-based source line.
    int line = 0;
};

/// One `<t> [<easing>] { decls }` key inside an `@timeline` track (28.1.2). `easing` applies to
/// the segment arriving at this key (previous key -> this key).
struct TimelineKey {
    /// Absolute time in seconds from the timeline's start.
    float time = 0;
    /// Arrival easing, valid only when `hasEasing`.
    Easing easing;
    /// True if the key named an easing.
    bool hasEasing = false;
    /// Regular declarations (shorthands such as `transform` already expanded).
    std::vector<StyleDecl> decls;
    /// `effect.<effect>.<param>` declarations.
    std::vector<TimelineEffectDecl> effectDecls;
    /// 1-based source line.
    int line = 0;
    /// T-228 (31 §31.7.2): the key-only `motion-in: <x> <y>` / `motion-out: <x> <y>` (px, relative
    /// to this key's translate): the spatial-bezier handles of the translate motion path arriving
    /// at / leaving this key (04 §4.2's tanIn / tanOut).
    bool hasMotion = false;
    Vec2 motionIn{};   ///< `motion-in`.
    Vec2 motionOut{};  ///< `motion-out`.
};

/// One `#id { keys }` track of an `@timeline` (28.1.2). `path` is the element id (or a symbol
/// path id such as `Play/Bg`).
struct TimelineTrackDef {
    /// Target element id or path id, without the leading `#`.
    std::string path;
    /// Keys in source order.
    std::vector<TimelineKey> keys;
    /// 1-based source line.
    int line = 0;
};

/// One `@timeline <name> { ... }` block, or `@timeline <name> from "<file>#<name>"` (28.1.2,
/// 05.9.2). `fromFile`/`fromName` are resolved and merged by compile_document.cpp before emission.
struct TimelineDef {
    /// Timeline name.
    std::string name;
    /// Explicit `duration` in seconds, or a negative value when omitted (the emitter then uses
    /// the last key's time).
    float duration = -1;
    /// `marker: <name> <t>, ...` in source order (name, seconds).
    std::vector<std::pair<std::string, float>> markers;
    /// Tracks in source order (the same path may appear more than once; the emitter merges).
    std::vector<TimelineTrackDef> tracks;
    /// `sound { <t> <cue>; ... }` entries in source order (seconds, cue name).
    std::vector<std::pair<float, std::string>> sounds;
    /// `from "<file>#<name>"`: the `.xtanim.json` file (empty when there is no `from`).
    std::string fromFile;
    /// `from "<file>#<name>"`: the timeline name inside that file.
    std::string fromName;
    /// Source file name, for diagnostics.
    std::string fileName;
    /// 1-based source line of the `@timeline`.
    int line = 0;
    /// T-190 (26.2.1, 04 §4.10.1): the `<Name>` of the enclosing `symbol(<Name>) { ... }` block, or
    /// empty for a document-level timeline. symbol_expand.cpp rewrites a symbol timeline into one
    /// copy per instance (track paths prefixed, `scopePath` set) and drops the original.
    std::string symbol;
    /// T-190: the instance path id this (rewritten) timeline is scoped to; empty = the root.
    std::string scopePath;
    /// T-190 (11 §11.9): inline `<svg>` scenes this timeline animates, filled by
    /// compile_document.cpp after it moved the tracks addressing `<svg id>/<inner id>` out of
    /// `tracks` into the scene's own Timeline: (DomTree node index of the `<svg>`, TimelineId).
    std::vector<std::pair<uint32_t, uint32_t>> sceneMembers;
    /// T-190: the latest key time among the tracks moved into `sceneMembers` (for the implicit
    /// duration), or 0.
    float sceneLastKey = 0;
};

/// One `--name: value` design-token declaration (05.9.2, T-170). Tokens are document-wide: the
/// selector of the rule that declared one is irrelevant, and a later declaration of the same name
/// wins (with a warning).
struct TokenDecl {
    /// Token name, including its leading `--`.
    std::string name;
    /// The value's text, as re-serialized from its tokens (may itself contain `var()`).
    std::string value;
    /// Source file and 1-based line, for diagnostics.
    std::string file;
    int line = 0;
};

/// Result of parsing one `.xts` stylesheet (or `<style>` block): all rules, `@keyframes` and
/// `@font-face` definitions it contains.
struct RuleSet {
    /// Style rules in source order.
    std::vector<StyleRule> rules;
    /// `@keyframes` blocks in source order.
    std::vector<KeyframesDef> keyframes;
    /// `@font-face` blocks in source order.
    std::vector<FontFaceDef> fontFaces;
    /// `@timeline` blocks in source order (28.1.2).
    std::vector<TimelineDef> timelines;
    /// T-170: `--name: value` declarations in source order, from every rule.
    std::vector<TokenDecl> tokens;
    /// T-176: `@media` blocks in source order (StyleRule::media indexes it).
    std::vector<MediaDef> media;
    /// T-179: `@font-fallback` blocks in source order.
    std::vector<FontFallbackDef> fontFallbacks;
    /// T-335: the points of every `linear()` easing parsed into this RuleSet; a LinearPoints
    /// Easing's pointOffset / pointCount index it. compileDocument() copies it into
    /// StyleBuilder::easingPoints before emitting, so the same offsets index the IR pool.
    std::vector<EasingPoint> easingPoints;
};

}  // namespace xt::compile
