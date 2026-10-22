#pragma once

#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/ir.h"
#include "xt/node_arena.h"
#include "xt/text_layout.h"
#include "xt/types.h"
#include "xt/value.h"

/// LayoutEngine: length resolution, block flow, absolute/relative positioning, min/max clamping,
/// `img`/`anim` intrinsic sizes with object-fit, post-layout transform integration, and a JSON-ish
/// layout snapshot (test-only). See Docs/design/06-layout.md §6.1-6.7, §6.10-6.11, Docs/tasks/T-25.md.
///
/// Text (§6.8) is out of scope for this task: Text nodes are treated as 0x0. Flex (§6.9,
/// T-26) is implemented in layout_flex.cpp: `layoutFlex` handles single- and multi-line flex,
/// `flex-basis`/grow/shrink with min/max freezing, wrap, justify-content, align-items/self,
/// align-content, main-axis auto margins, gap, and `*-reverse`.

namespace xt {

class AssetResolver;
class TextShaper;
struct ComputedStyle;
struct EditState;         // Code/Runtime/core/src/text_edit.h (private); only ever used through a pointer here.
struct CompositionState;  // Code/Runtime/core/src/composition.h (private); likewise.

/// Safe-area insets (Docs/design/13-api.md §13.8.2, T-176). In DeviceInfo they are FRACTIONS of
/// the viewport -- top/bottom of its height, left/right of its width (e.g. 0.05 = 5 %) -- so the
/// area follows a resize; LengthEnv holds them converted to logical pixels.
struct Insets {
    float top = 0;
    float right = 0;
    float bottom = 0;
    float left = 0;
};

/// `@media (platform: ...)` (T-176).
enum class DeviceKind : uint8_t { Desktop = 0, Console = 1, Handheld = 2 };

/// `@media (input: ...)` (T-176).
enum class InputKind : uint8_t { Mouse = 0, Gamepad = 1, Touch = 2 };

/// What the host tells the core about the screen it renders to (13.8.2 `setDevice`, 25.4,
/// 23.10 item 12; T-176). Context::setDevice() hands it to every Document: `@media` is
/// re-evaluated and the safe-area units follow `safeArea`.
struct DeviceInfo {
    /// Screen aspect ratio (width / height) for `@media (aspect-ratio)` and `(orientation)`;
    /// 0 (the default) uses the viewport's own width / height.
    float aspect = 0;
    /// Safe-area insets as fractions of the viewport (see Insets).
    Insets safeArea{};
    /// Device pixel ratio as reported by the host; `@media (resolution)` (T-330) reads it. The
    /// render DPR still comes from Context::setViewport().
    float dpr = 1;
    /// `@media (platform: ...)`.
    DeviceKind platform = DeviceKind::Desktop;
    /// `@media (input: ...)`.
    InputKind input = InputKind::Mouse;
    /// T-330: `@media (prefers-reduced-motion: reduce)` (host flag `system.reducedMotion`).
    bool reducedMotion = false;
};

/// Everything the device-relative units and `calc()` resolve against (Docs/design/06-layout.md
/// §6.4, T-176). A Document owns one, refreshed on setViewport()/setDevice()/setUiScale(), and
/// makes it current (ScopedLengthEnv) while its style, layout and paint passes run.
struct LengthEnv {
    /// Viewport logical size (`vw`/`vh`/`vmin`/`vmax`).
    float viewportW = 0;
    float viewportH = 0;
    /// Safe-area insets in logical px (`safe-*`).
    Insets safePx{};
    /// `1uiscale` in px (Context::setUiScale()).
    float uiScale = 1;
    /// The IR's `CALC` programs.
    std::span<const IrCalc> calcs;
    std::span<const IrCalcOp> calcOps;
    /// StyleEngine's runtime calc pool (index kRuntimeCalcBase + i); may be null.
    const std::vector<IrCalc>* runtimeCalcs = nullptr;
    const std::vector<IrCalcOp>* runtimeCalcOps = nullptr;
};

/// The LengthEnv resolveLength() and friends read on this thread; a default LengthEnv (zero
/// viewport, no safe area, uiscale 1, no programs) when none is current.
const LengthEnv& currentLengthEnv();

/// Makes `env` current for its lifetime (restoring the previous one afterwards).
class ScopedLengthEnv {
public:
    explicit ScopedLengthEnv(const LengthEnv* env);
    ~ScopedLengthEnv();
    ScopedLengthEnv(const ScopedLengthEnv&) = delete;
    ScopedLengthEnv& operator=(const ScopedLengthEnv&) = delete;

private:
    const LengthEnv* m_previous;
};

/// T-176: `len` in px against `env`: Px, Percent (of `percentRef`), the viewport/safe-area/
/// uiscale units and Calc. False for Auto, Em, a Percent (or a `calc()` using one) against a
/// negative `percentRef`, and a `calc()` that cannot be evaluated (unknown program, a `var()` or
/// `em` term that was not substituted, a type error, a division by zero).
bool lengthToPxEnv(const LengthEnv& env, Length len, float percentRef, float& outPx);

/// T-176: evaluates `calc()` program `index` (IR index, or kRuntimeCalcBase + runtime index) in
/// px; see lengthToPxEnv() for when it fails. `emPx` >= 0 lets an `em` term resolve (style-time
/// evaluation of `font-size`); otherwise one makes the program fail.
bool evalCalc(const LengthEnv& env, uint32_t index, float percentRef, float emPx, float& outPx);

/// A reference < 0 means "undefined" (Docs/design/06-layout.md §6.4: a Percent length with an
/// undefined reference resolves to `fallback`, same as Auto).
inline constexpr float kUndefinedRef = -1.0f;

/// Resolves a single Length to px. `len.unit == LengthUnit::Em` should never reach here (style
/// resolution -- 05.7 -- converts Em to Px before writing ComputedStyle/ElementData, and
/// IrView::validate is the gate for the IR); one that does resolves to `fallback` in every build.
float resolveLength(Length len, float reference, float fallback);

/// Docs/design/06-layout.md §6.4 (T-74): resolves a `height`-family length against a containing
/// block content height that may be undefined. Returns true and writes the px value when the length
/// is *definite* -- Px always, Percent only when `refH >= 0`. Returns false (leaving `outPx`
/// untouched) for Auto (and an Em that escaped style resolution), and for a Percent against an undefined
/// reference: such a `height` behaves as `auto` and such a `min/max-height` as "no bound" (CSS 2.1 §10.5).
bool resolveHeightLength(Length len, float refH, float& outPx);

/// Docs/design/06-layout.md §6.4 (T-74): clamps `value` to [`min-height`, `max-height`], ignoring
/// either bound that is not definite against `refH` (see resolveHeightLength). The width-side
/// equivalent is a plain clamp against the containing block content width, `clampMinMax` in
/// Code/Runtime/core/src/layout_common.h (shared by the layout_*.cpp passes).
float clampHeightMinMax(float value, Length minLen, Length maxLen, float refH);

/// "No max" sentinel for FlexBasisInput::maxMain (Docs/design/06-layout.md §6.9 step 2: a `max-*`
/// of `auto` means unbounded). Large but finite so ordinary arithmetic (target vs. max compares,
/// clamping) never produces inf/NaN.
inline constexpr float kFlexNoMax = 1.0e30f;

/// Docs/design/06-layout.md §6.16 (T-328): resolved min/max bounds for resolveAspect(), in px and
/// in the same box as the sizes passed to it; a max of kFlexNoMax means "no bound".
struct AspectMinMax {
    float minW = 0;
    float maxW = kFlexNoMax;
    float minH = 0;
    float maxH = kFlexNoMax;
};

/// §6.16: `min/max-width` (against `refW`) and `min/max-height` (against `refH`, an indefinite
/// one leaving a Percent bound unset, as clampHeightMinMax does) of `style`, as an AspectMinMax.
AspectMinMax aspectMinMax(const ComputedStyle& style, float refW, float refH);

/// §6.16 (T-328): `aspect-ratio`'s one multiply/divide. Exactly one of `w`/`h` is negative (e.g.
/// kUndefinedRef) -- the axis to derive from the other, already min/max-clamped, definite one
/// through `ratio` (width / height) -- and is returned derived and clamped to its own bounds. When
/// that clamp changes it and `definiteIsAutomatic` (the definite axis is itself an automatic size:
/// a fill/stretch or natural width, not an author's length), the definite axis is recomputed once
/// from the clamped one and clamped to its own bounds (CSS 2.1 §10.4 / Sizing 4 §5.2's transfer,
/// single pass, no iteration). `ratio <= 0`, or both or neither axis negative, returns {w, h}.
Vec2 resolveAspect(float w, float h, float ratio, const AspectMinMax& minmax,
                   bool definiteIsAutomatic = false);

/// T-336 (06 §6.5): stores a laid-out element's scroll size (LayoutBox::scrollWidth/Height) -- its
/// in-flow content's extent in its own content-box coordinates: the largest right / bottom
/// margin-box edge of its in-flow Element children (absolute and `display: none` ones are not
/// counted) and the size of its Text children, never less than `contentW` / `minHeight`.
/// maxScrollX/Y (xt/node.h) subtract the content box. A separate call keeps the recursive layout
/// frames small (the 256-level chain must fit a 512 KiB stack in Debug, T-255b).
void storeScrollSize(NodeArena& arena, NodeId node, float contentW, float minHeight);

/// A single flex item's CSS Flexbox §9.7 "Resolving Flexible Lengths" inputs, for one flex line.
/// `basis` is the item's flex base size clamped to [minMain, maxMain] (Docs/design/06-layout.md
/// §6.9 step 2's `hypotheticalMain`); `minMain`/`maxMain` are the same resolved px bounds, used
/// again during the freezing loop below (an item pushed by grow/shrink past its own basis-time
/// clamp still needs re-clamping). `maxMain == kFlexNoMax` means unbounded.
struct FlexBasisInput {
    /// Flex base size, clamped to [minMain, maxMain], in px.
    float basis = 0;
    /// `flex-grow` weight.
    float grow = 0;
    /// `flex-shrink` weight.
    float shrink = 1;
    /// Resolved `min-*` bound on the main axis, in px.
    float minMain = 0;
    /// Resolved `max-*` bound on the main axis, in px, or `kFlexNoMax` for unbounded.
    float maxMain = kFlexNoMax;
};

/// Docs/design/06-layout.md §6.9 step 4 / Docs/tasks/T-26.md's "resolveFlexibleLengths": resolves
/// one flex line's main sizes given each item's basis/grow/shrink/min/max and the line's free
/// space (`mainAvail - Sum(outerMain) - gaps`, 0 when the container's main axis is auto). Grows
/// (`free > 0`, weighted by flex-grow) or shrinks (`free < 0`, weighted by flex-shrink * basis)
/// every item in lockstep, clamping to [minMain, maxMain] and "freezing" (excluding from further
/// redistribution) any item that would violate its bound, then redistributing the remaining free
/// space among the still-unfrozen items; repeats until no item freezes in a pass (bounded by
/// `items.size()` passes, per the spec). `free == 0`, an empty `items`, or every relevant weight
/// (grow when growing, shrink*basis when shrinking) being 0 leaves every item at its `basis`.
/// Writes `items.size()` sizes to `outSizes` (resized), in `items` order.
void resolveFlexibleLengths(const std::vector<FlexBasisInput>& items, float free,
                            std::vector<float>& outSizes);

/// 06 §6.15 (T-338): one grid item's resolved placement -- 0-based track ranges -- as
/// LayoutEngine::placeGridItems produces it (layout_grid.cpp).
struct GridItemPlacement {
    NodeId node = kInvalidNode;
    int rowStart = -1;
    int rowSpan = 1;
    int colStart = -1;
    int colSpan = 1;
    bool rowDefinite = false;  ///< The row came from `grid-area` / `grid-row` lines.
    bool colDefinite = false;  ///< Likewise the column.
    bool placed = false;
};

/// LayoutEngine::measureTextChildren's result (Docs/tasks/T-30.md "intrinsicWidth").
struct TextIntrinsic {
    /// Intrinsic main-axis content size (shrink-to-fit / flex-basis size), in px.
    float main = 0;
    /// Text node box width, in px.
    float width = 0;
    /// Text node box height, in px.
    float height = 0;
};

/// Computes layout boxes for a NodeArena's Element/Text subtree. See Docs/design/06-layout.md.
class LayoutEngine {
public:
    /// Borrows `ir`/`assets`/`shaper`; the caller (Document) guarantees all three outlive the
    /// LayoutEngine. `assets` may be nullptr (img intrinsic size then resolves to 0x0); `shaper`
    /// may be nullptr or have no font (Text nodes are then laid out as 0x0, Docs/design/07-text.md
    /// §7.2). `shaper` defaults to nullptr so a snapshot-only LayoutEngine (tests) needs no shaper.
    void init(const IrView& ir, AssetResolver* assets, TextShaper* shaper = nullptr);

    /// Borrows the Document's EditState pool (T-33): a Text node whose TextNodeData::editIndex is
    /// set takes its string from that pool entry's runtime value instead of the IR string table.
    /// nullptr (the default) means "no editable elements", which is what every Document-less test
    /// LayoutEngine wants.
    void setEditStates(const std::vector<EditState>* editStates) { m_editStates = editStates; }
    /// Borrows the Document's CompositionState (T-33b): while it is active, the Text node of
    /// the composing element lays out its display string (value prefix + composition + value
    /// suffix) instead of the bare value. nullptr (the default) means "never composing".
    void setComposition(const CompositionState* composition) { m_composition = composition; }

    /// Borrows the Document's runtime string pool (T-35), so a StringId at or above
    /// kRuntimeStringBase -- e.g. the text ElementRef::setText() stored, or a dynamically created
    /// element's tag -- resolves to its text instead of an empty view. nullptr (the default) means
    /// "IR strings only", which is what every Document-less test LayoutEngine wants.
    void setRuntimeStrings(const std::deque<std::string>* strings) { m_runtimeStrings = strings; }
    /// T-270a: borrows the Document's node-owned runtime text table, so a Text node's StringId at
    /// or above kRuntimeTextBase (the text ElementRef::setText() stored) resolves too. nullptr
    /// (the default) resolves such an id to an empty view.
    void setRuntimeTexts(const std::deque<std::string>* texts) { m_runtimeTexts = texts; }

    /// T-179 (07.8): the IR's `STRS` chunk, whose `@font-fallback` chains text shaping appends to
    /// an element's own `font-family` list. nullptr (the default) = no chains. Borrowed.
    void setLocalization(const LocalizationChunk* localization) { m_localization = localization; }
    /// T-179: the document language that picks the chain; empty = each element's `lang`
    /// attribute (inherited). Does not invalidate any shaping cache (Document does that).
    void setLanguage(std::string_view lang) { m_language.assign(lang); }
    /// T-397 (08 §8.16): borrows the Document's sorted list of `placeholder` values that are an
    /// unresolved `@key` (no string table, or the key is missing from it); such a placeholder is
    /// not laid out, so nothing is drawn. nullptr (the default) = every placeholder is drawn.
    void setUnresolvedPlaceholders(const std::vector<StringId>* ids) { m_unresolvedPlaceholders = ids; }
    /// T-191: unbound external content measures the placeholder size (320 x 180) instead of 0 x 0.
    void setExternalPlaceholders(bool on) { m_externalPlaceholders = on; }
    /// T-191: `img`, `anim` and the external content tags are replaced elements (06.10).
    static bool isReplacedTag(std::string_view tag);

    /// Lays out `root`'s subtree (root's own box = `viewport`, border-box, no margin/padding/
    /// border) into `viewport`'s coordinate system. No-op if `root` does not carry kDirtyLayout
    /// (see Docs/design/06-layout.md §6.3: first version relayouts the whole tree whenever any
    /// element is dirty, detected via kDirtyLayout's propagation up to `root`).
    void layout(NodeArena& arena, NodeId root, Rect viewport);

    /// Docs/design/21-perf-tracing.md §21.1's `laidOutElements`: how many elements the most recent
    /// layout() call sized (the root plus every layoutChild() visit, so an element sized twice --
    /// once in flow, once in the absolute pass -- counts twice). 0 when that call returned early
    /// because nothing was dirty.
    uint32_t lastLaidOutElements() const { return m_laidOutElements; }

    /// 06 §6.15 (T-338): true once any layout() placed a grid item past its grid's explicit tracks
    /// (clamped to the last track); Document reports kWarnGridPlacementClamped once.
    bool gridPlacementClamped() const { return m_gridPlacementClamped; }

    /// Preorder JSON-ish snapshot of every Element under (and including) `root`, one line each:
    /// `{"tag":"div","id":"a","x":0,"y":0,"w":100,"h":20}` (`id` omitted when the element has none).
    /// Test-only (Docs/tasks/T-25.md "快照格式").
    std::string snapshot(const NodeArena& arena, NodeId root) const;

private:
    // Dispatch by `display` (§6.5-6.9): Block -> layoutBlock; Flex -> layoutFlex; None -> zero box,
    // visible = 0, no recursion.
    //
    // The three share one parameter contract (§6.4, T-74):
    //   availW   the containing block's content *width*: the reference every Percent width, margin,
    //            padding, border and left/right resolves against. kUndefinedRef means "not known",
    //            i.e. a shrink-to-fit measurement.
    //   availH   the containing block's *definite* content height: the reference every Percent
    //            height and top/bottom resolves against. kUndefinedRef means the containing block's
    //            own height is content-based, so a Percent `height` behaves as `auto` and a Percent
    //            `min/max-height` as "no bound".
    //   imposedH a used border-box height forced on this element from the outside (a flex item's
    //            resolved main size in a column container, a stretched cross size, an absolutely
    //            positioned box whose height top/bottom or `height` already fixed). kUndefinedRef
    //            normally. It only applies when the element's own `height` is not definite, and it
    //            makes the element's content height definite for its descendants.
    void layoutChild(NodeArena& arena, NodeId node, float availW, float availH, float imposedH, int depth);
    void layoutBlock(NodeArena& arena, NodeId node, float availW, float availH, float imposedH, int depth);
    void layoutFlex(NodeArena& arena, NodeId node, float availW, float availH, float imposedH, int depth);
    /// 06 §6.15 (T-338, layout_grid.cpp): a `display: grid` container, sized like layoutFlex.
    void layoutGrid(NodeArena& arena, NodeId node, float availW, float availH, float imposedH, int depth);

    /// T-338: places and sizes `node`'s in-flow grid items inside a content box of `contentW` x
    /// `contentH` (either may be auto: the tracks' own size), writing each item's box relative to
    /// the content box. Returns the final content size. Also used by the absolute pass.
    Vec2 layoutGridChildren(NodeArena& arena, NodeId node, float contentW, float contentH, bool widthAuto,
                            bool heightAuto, int depth);
    /// T-338: §6.15 placement of `node`'s in-flow children (grid-area / lines first, then
    /// row-major auto-placement), clamping past the explicit tracks; fills the track counts.
    void placeGridItems(NodeArena& arena, NodeId node, std::vector<GridItemPlacement>& items, int& numCols,
                        int& numRows);
    /// T-338: a grid container's max-content content width -- its columns sized against an
    /// indefinite width, plus the column gaps (maxContentWidth's grid case).
    float gridContentWidth(NodeArena& arena, NodeId node, int depth);
    /// T-338: `item`'s max-content width plus its non-auto horizontal margins.
    float gridOuterMaxContentWidth(NodeArena& arena, NodeId item, int depth);

    /// §6.5 steps 3-4: stacks `node`'s in-flow (non-absolute, display != none) children into
    /// `contentW`, writing each child's box (including x/y, relative to `node`'s content box) via
    /// layoutChild + margin/centering + (for position: relative) the §6.6 offset. Returns the
    /// accumulated content height (cursorY), used by the two callers (layoutBlock's own step 5, and
    /// layoutAbsoluteChildren's fixed-height case) differently. Does not touch `node`'s own
    /// box.width/height/margin/padding/border (the caller owns those).
    /// `contentH` is `node`'s *own* definite content height, kUndefinedRef when it is content-based:
    /// it is both the available main size of a vertical writing-mode Text child (§6.8) and the
    /// `availH` handed to every element child (§6.4, T-74).
    float layoutBlockChildren(NodeArena& arena, NodeId node, float contentW, float contentH, int depth);

    /// §6.9 steps 2-11: flex-arranges `node`'s in-flow (non-absolute, display != none) children
    /// within a content box of `contentMain` x `contentCross` (main/cross per `node`'s
    /// flex-direction), writing each child's full box (via layoutChild, so a child's own
    /// margin/padding/border/nested layout are resolved too) with x/y relative to `node`'s content
    /// box, same convention as layoutBlockChildren. `mainAxisAuto`/`crossAxisAuto` mark an axis
    /// "undefined" per §6.9 step 1 (the corresponding `content*` input is then just a placeholder;
    /// the real size is computed from children, step 10, and returned). `refW`/`refH` are `node`'s
    /// own content width / definite content height, the two references every child length resolves
    /// against (§6.4: width, margin, padding, border and flex-basis-along-a-row percentages
    /// reference `refW`; height and flex-basis-along-a-column percentages reference `refH`). Pass
    /// `kUndefinedRef` for `refW` when `node`'s own content width isn't known yet (row direction
    /// with `mainAxisAuto`, or column direction with `crossAxisAuto`), and for `refH` when its
    /// height is content-based (T-74). Returns the final content
    /// {main, cross} size (unchanged from the input on an axis that wasn't auto). Does not touch
    /// `node`'s own box.width/height/margin/padding/border (the caller -- layoutFlex, or
    /// resolveAbsoluteBox for an absolutely positioned flex container's shrink-to-fit case -- owns
    /// those; see layout_flex.cpp / layout_absolute.cpp).
    Vec2 layoutFlexChildren(NodeArena& arena, NodeId node, float contentMain, float contentCross,
                            bool mainAxisAuto, bool crossAxisAuto, float refW, float refH, int depth);

    /// §6.6: single global top-down pass (invoked once from `layout()`, after the in-flow tree is
    /// fully sized) that resolves every position: absolute element's box, tracking the nearest
    /// position != static ancestor's padding box (falling back to `root`) as it descends. Doing
    /// this as a separate pass (rather than inline in layoutBlock's own step 8) avoids resolving an
    /// ancestor's containing block against a still-provisional auto height (see the T-25 report's
    /// "uncovered cases").
    void layoutAbsolutePass(NodeArena& arena, NodeId root);
    void layoutAbsoluteChildren(NodeArena& arena, NodeId node, NodeId cbNode, Rect cbPaddingAbs,
                                float parentContentAbsX, float parentContentAbsY, int depth);
    void resolveAbsoluteBox(NodeArena& arena, NodeId node, Rect cbPaddingAbs, int depth);

    /// §6.8 (layout_text.cpp): shapes `textNode`'s text (reusing TextNodeData's shaping cache
    /// whenever the key -- text/font/size/letter-spacing/direction/writing-mode/orientation/lang --
    /// is unchanged) and lays it out into an available main-axis size of `availMain` (< 0 means
    /// "unbounded": no soft wrapping), writing TextNodeData::runs/lines/width/height/intrinsicMain.
    /// `style` is the parent Element's ComputedStyle (text inherits everything it needs). Writes
    /// zeros when there is no shaper, no font or no text.
    void layoutTextNode(NodeArena& arena, NodeId textNode, const ComputedStyle& style, float availMain);

    /// Unbounded ("no soft wrapping") measurement of every Text child of `element`, componentwise
    /// max, all zero when it has none: `main` is `TextLayoutResult::intrinsicMain` (§6.8's
    /// shrink-to-fit / flex-basis content size), `width`/`height` the text node's own box. Fills the
    /// shaping cache, so the real layout pass that follows does not reshape.
    TextIntrinsic measureTextChildren(NodeArena& arena, NodeId element);

    /// T-201 (§6.5/§6.6/§6.9): `node`'s max-content border-box width -- its own definite `width`,
    /// else its content's widest unwrapped extent: a flex row sums its in-flow children's outer
    /// max-content widths plus gaps, anything else takes the widest in-flow child or its text's
    /// unwrapped width; min/max-width clamp the content width as in layoutBlock. Percentages have
    /// no reference here and count as 0 / auto. The shrink-to-fit width of a `width: auto` block
    /// measured without an available width (a flex row item's content basis, a flex column item's
    /// cross measurement, an absolutely positioned box).
    float maxContentWidth(NodeArena& arena, NodeId node, int depth);

    /// T-178 (07 §7.8): rebuilds `data.shaped`'s runs/glyphs segment by segment from
    /// `data.styleRuns` (each segment shaped with its run's style, inline objects as one
    /// placeholder glyph or, for an unresolved `<glyph>`, the text `[key]`) and fills
    /// `data.shapedRunInfo`. `data.shaped` must already hold the whole text shaped with
    /// `baseParams` (its breakAfter/graphemeStart are kept).
    void shapeRichText(TextNodeData& data, std::string_view text, const ShapeParams& baseParams,
                       const ComputedStyle& style, float baseLineHeight);
    /// T-178: sets every TextInline::box from the laid-out placeholder runs.
    void placeInlineObjects(TextNodeData& data, WritingMode writingMode);

    /// §6.10: img -> AssetResolver::imageSize via the `data-xt-asset` attribute (decimal AssetId),
    /// or, for an `engine://` asset, the size bound with AssetResolver::setExternalTexture (T-150);
    /// anim -> the referenced scene's design size (IrView::scene(id).width/height). (0, 0) when
    /// unavailable (no resolver, no/invalid attribute, asset not found or texture not bound).
    /// T-345 (05 §5.9.9): in the element's `ui-scale` factor (an intrinsic size is design px).
    Vec2 intrinsicSize(NodeArena& arena, NodeId node) const;
    /// intrinsicSize() before the `ui-scale` factor.
    Vec2 intrinsicSizeUnscaled(NodeArena& arena, NodeId node) const;

    /// §6.7 (every Element: anchor/translate from transform-origin + cssTranslate, folding in the
    /// parent's padding offset and (overflow: scroll only) scroll) and §6.10's anim object-fit
    /// (sets the scene root Group's scale/translate from the now-final content box). Single preorder
    /// pass over `root`'s subtree, run last (after both the in-flow layout and the absolute pass).
    void applyTransforms(NodeArena& arena, NodeId root);

    /// applyTransforms()'s per-Element body: foldElementTranslate(), perspective px (§19.4) and
    /// the anim scene root's object-fit (§6.10), from the element's current box and css values.
    void applyElementTransform(NodeArena& arena, NodeId node);

    /// §6.7's fold for one Element: NodeCommon::anchor from transform-origin, NodeCommon::translate
    /// from the box, the parent's padding/scroll offset and cssTranslate.
    void foldElementTranslate(NodeArena& arena, NodeId node);

    /// T-160: the frame had no relayout, but an Element's §6.7 inputs (cssTranslate from an
    /// animation or setProperty, transform-origin) may still have changed -- they only reach
    /// NodeCommon through the fold. Re-folds every Element carrying kDirtyTransform (its box and
    /// its parent's padding/scroll cannot change without a relayout).
    void refreshDirtyTransforms(NodeArena& arena, NodeId root);

    const IrView* m_ir = nullptr;
    AssetResolver* m_assets = nullptr;
    TextShaper* m_shaper = nullptr;
    const std::vector<EditState>* m_editStates = nullptr;
    const CompositionState* m_composition = nullptr;
    const std::deque<std::string>* m_runtimeStrings = nullptr;
    const std::deque<std::string>* m_runtimeTexts = nullptr;  // T-270a: setRuntimeTexts().
    // T-179: setLocalization()/setLanguage().
    const LocalizationChunk* m_localization = nullptr;
    std::string m_language;
    const std::vector<StringId>* m_unresolvedPlaceholders = nullptr;  // T-397.
    bool m_externalPlaceholders = false;                              // T-191: see setExternalPlaceholders().

    /// StringId -> text, dispatching on kRuntimeStringBase (see setRuntimeStrings()).
    std::string_view string(StringId id) const;
    // Scratch for layoutTextNode(): the ShapeParams family list (string_views into the IR string
    // table) and layoutText()'s output. Each layoutTextNode() call fully consumes both before
    // returning, and layout is single-threaded, so reuse across calls is safe.
    std::vector<std::string_view> m_familyScratch;
    TextLayoutResult m_textScratch;
    // T-323: the `text-transform`ed copy of the text being shaped (layoutTextNode(), cache miss).
    std::string m_caseScratch;
    // 21.1's laidOutElements, reset by every layout() call; see lastLaidOutElements().
    uint32_t m_laidOutElements = 0;
    // §6.16 (T-328): the flex item currently being laid out stretched with an auto cross size, whose
    // own `aspect-ratio` layoutBlock / layoutFlex must ignore (stretch wins); kInvalidNode otherwise.
    // Set and restored around each such layoutChild call by layoutFlexChildren.
    NodeId m_aspectSuppressed = kInvalidNode;
    // T-338: see gridPlacementClamped().
    bool m_gridPlacementClamped = false;
};

}  // namespace xt
