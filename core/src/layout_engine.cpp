#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>

#include "xt/assert.h"
#include "xt/asset_resolver.h"
#include "xt/layout.h"
#include "xt/node.h"

/// LayoutEngine entry point (layout()), display dispatch (layoutChild; layoutFlex itself lives in
/// layout_flex.cpp), img/anim intrinsic size lookup, and the post-layout transform integration
/// pass (applyTransforms, §6.7 + §6.10's anim object-fit). See docs/tasks/T-25.md,
/// docs/design/06-layout.md §6.3-6.4, §6.7, §6.10.

namespace xt {

namespace {

/// Parses `text` as an unsigned decimal AssetId; returns kInvalidAsset on empty/malformed input
/// (never throws: core has no exceptions). Mirrors document_load.cpp's parseFloatOr.
AssetId parseAssetIdOr(std::string_view text) {
    if (text.empty()) {
        return kInvalidAsset;
    }
    uint32_t value = 0;
    std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return kInvalidAsset;
    }
    return value;
}

}  // namespace

namespace {

// T-176: the LengthEnv made current by ScopedLengthEnv (Document: style, layout and paint).
thread_local const LengthEnv* t_lengthEnv = nullptr;

/// One non-Calc, non-Auto unit in px; `emPx` < 0 makes Em fail.
bool unitToPx(const LengthEnv& env, LengthUnit unit, float v, float percentRef, float emPx, float& out) {
    switch (unit) {
        case LengthUnit::Px:
            out = v;
            return true;
        case LengthUnit::Percent:
            if (percentRef < 0.0f) {
                return false;
            }
            out = percentRef * v / 100.0f;
            return true;
        case LengthUnit::Em:
            if (emPx < 0.0f) {
                return false;
            }
            out = v * emPx;
            return true;
        case LengthUnit::Vw:
            out = v * env.viewportW / 100.0f;
            return true;
        case LengthUnit::Vh:
            out = v * env.viewportH / 100.0f;
            return true;
        case LengthUnit::Vmin:
            out = v * std::min(env.viewportW, env.viewportH) / 100.0f;
            return true;
        case LengthUnit::Vmax:
            out = v * std::max(env.viewportW, env.viewportH) / 100.0f;
            return true;
        case LengthUnit::SafeTop:
            out = v * env.safePx.top;
            return true;
        case LengthUnit::SafeRight:
            out = v * env.safePx.right;
            return true;
        case LengthUnit::SafeBottom:
            out = v * env.safePx.bottom;
            return true;
        case LengthUnit::SafeLeft:
            out = v * env.safePx.left;
            return true;
        case LengthUnit::UiScale:
            out = v * env.uiScale;
            return true;
        case LengthUnit::Auto:
        case LengthUnit::Calc:
        case LengthUnit::Rem:  // T-335: style resolution turns it into px, like Em.
            return false;
    }
    return false;
}

}  // namespace

const LengthEnv& currentLengthEnv() {
    static const LengthEnv kDefault{};
    return t_lengthEnv != nullptr ? *t_lengthEnv : kDefault;
}

ScopedLengthEnv::ScopedLengthEnv(const LengthEnv* env) : m_previous(t_lengthEnv) { t_lengthEnv = env; }

ScopedLengthEnv::~ScopedLengthEnv() { t_lengthEnv = m_previous; }

bool evalCalc(const LengthEnv& env, uint32_t index, float percentRef, float emPx, float& outPx) {
    const IrCalcOp* ops = nullptr;
    uint32_t count = 0;
    if (index >= kRuntimeCalcBase) {
        const uint32_t i = index - kRuntimeCalcBase;
        if (env.runtimeCalcs == nullptr || env.runtimeCalcOps == nullptr || i >= env.runtimeCalcs->size()) {
            return false;
        }
        const IrCalc& calc = (*env.runtimeCalcs)[i];
        if (static_cast<uint64_t>(calc.opOffset) + calc.opCount > env.runtimeCalcOps->size()) {
            return false;
        }
        ops = env.runtimeCalcOps->data() + calc.opOffset;
        count = calc.opCount;
    } else {
        if (index >= env.calcs.size()) {
            return false;
        }
        const IrCalc& calc = env.calcs[index];
        if (static_cast<uint64_t>(calc.opOffset) + calc.opCount > env.calcOps.size()) {
            return false;
        }
        ops = env.calcOps.data() + calc.opOffset;
        count = calc.opCount;
    }
    // A typed RPN stack: a length (already in px) or a plain number.
    struct Slot {
        float v;
        bool isLength;
    };
    constexpr int kMaxDepth = 32;
    Slot stack[kMaxDepth];
    int sp = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const IrCalcOp& op = ops[i];
        switch (static_cast<CalcOpKind>(op.op)) {
            case CalcOpKind::PushLength: {
                float px = 0;
                if (sp == kMaxDepth ||
                    !unitToPx(env, static_cast<LengthUnit>(op.unit), op.value, percentRef, emPx, px)) {
                    return false;
                }
                stack[sp++] = Slot{px, true};
                break;
            }
            case CalcOpKind::PushNumber:
                if (sp == kMaxDepth) {
                    return false;
                }
                stack[sp++] = Slot{op.value, false};
                break;
            case CalcOpKind::PushVar:
                return false;  // Substituted at style-resolve time; never evaluated as such.
            case CalcOpKind::Add:
            case CalcOpKind::Sub:
            case CalcOpKind::Mul:
            case CalcOpKind::Div:
            case CalcOpKind::Min:    // T-330.
            case CalcOpKind::Max: {  // T-330.
                if (sp < 2) {
                    return false;
                }
                const Slot b = stack[--sp];
                Slot& a = stack[sp - 1];
                const auto kind = static_cast<CalcOpKind>(op.op);
                if (kind == CalcOpKind::Add || kind == CalcOpKind::Sub) {
                    if (a.isLength != b.isLength) {
                        return false;
                    }
                    a.v = kind == CalcOpKind::Add ? a.v + b.v : a.v - b.v;
                } else if (kind == CalcOpKind::Min || kind == CalcOpKind::Max) {
                    if (a.isLength != b.isLength) {
                        return false;
                    }
                    a.v = kind == CalcOpKind::Min ? (b.v < a.v ? b.v : a.v) : (b.v > a.v ? b.v : a.v);
                } else if (kind == CalcOpKind::Mul) {
                    if (a.isLength && b.isLength) {
                        return false;
                    }
                    a.v *= b.v;
                    a.isLength = a.isLength || b.isLength;
                } else {
                    if (b.isLength || b.v == 0.0f) {
                        return false;
                    }
                    a.v /= b.v;
                }
                break;
            }
            default:
                return false;
        }
    }
    if (sp != 1 || !stack[0].isLength) {
        return false;
    }
    outPx = stack[0].v;
    return true;
}

bool lengthToPxEnv(const LengthEnv& env, Length len, float percentRef, float& outPx) {
    if (len.unit == LengthUnit::Calc) {
        return evalCalc(env, calcProgramIndex(len.value), percentRef, -1.0f, outPx);
    }
    return unitToPx(env, len.unit, len.value, percentRef, -1.0f, outPx);
}

float resolveLength(Length len, float reference, float fallback) {
    switch (len.unit) {
        case LengthUnit::Px:
            return len.value;
        case LengthUnit::Em:
            // Style resolution (05.7) converts Em to Px before it reaches ElementData /
            // ComputedStyle, and IrView::validate rejects the IR that could smuggle one past it
            // (T-257b); an Em that still gets here resolves to `fallback`, the same in every build.
        case LengthUnit::Auto:
            return fallback;
        default:
            break;
    }
    // Percent (undefined reference -> fallback), the T-176 device units and `calc()`.
    float px = 0;
    return lengthToPxEnv(currentLengthEnv(), len, reference, px) ? px : fallback;
}

bool resolveHeightLength(Length len, float refH, float& outPx) {
    switch (len.unit) {
        case LengthUnit::Px:
            outPx = len.value;
            return true;
        case LengthUnit::Em:
            // Same upstream invariant as resolveLength: an Em that reaches layout is not definite.
        case LengthUnit::Auto:
            return false;
        default:
            break;
    }
    // §6.4: a Percent height (or a `calc()` using one) against an indefinite height behaves as
    // auto -- lengthToPxEnv() fails for it.
    float px = 0;
    if (!lengthToPxEnv(currentLengthEnv(), len, refH, px)) {
        return false;
    }
    outPx = px;
    return true;
}

float clampHeightMinMax(float value, Length minLen, Length maxLen, float refH) {
    float clamped = value;
    float bound = 0;
    if (resolveHeightLength(maxLen, refH, bound) && clamped > bound) {
        clamped = bound;
    }
    if (resolveHeightLength(minLen, refH, bound) && clamped < bound) {
        clamped = bound;
    }
    return clamped;
}

AspectMinMax aspectMinMax(const ComputedStyle& style, float refW, float refH) {
    AspectMinMax m;
    const Length maxW = style.base[prop::MaxWidth].len;
    if (maxW.unit != LengthUnit::Auto) {
        m.maxW = resolveLength(maxW, refW, 0);
    }
    m.minW = resolveLength(style.base[prop::MinWidth].len, refW, 0);
    float bound = 0;
    if (resolveHeightLength(style.base[prop::MaxHeight].len, refH, bound)) {
        m.maxH = bound;
    }
    if (resolveHeightLength(style.base[prop::MinHeight].len, refH, bound)) {
        m.minH = bound;
    }
    return m;
}

Vec2 resolveAspect(float w, float h, float ratio, const AspectMinMax& minmax, bool definiteIsAutomatic) {
    const bool deriveH = h < 0.0f;
    if (!(ratio > 0.0f) || deriveH == (w < 0.0f)) {
        return Vec2{w, h};
    }
    // CSS order: max first, then min (a min larger than the max wins).
    const auto clamp = [](float v, float lo, float hi) { return std::max(lo, std::min(v, hi)); };
    const float derived = deriveH ? w / ratio : h * ratio;
    const float clamped =
        deriveH ? clamp(derived, minmax.minH, minmax.maxH) : clamp(derived, minmax.minW, minmax.maxW);
    float definite = deriveH ? w : h;
    if (clamped != derived && definiteIsAutomatic) {
        definite = deriveH ? clamp(clamped * ratio, minmax.minW, minmax.maxW)
                           : clamp(clamped / ratio, minmax.minH, minmax.maxH);
    }
    return deriveH ? Vec2{definite, clamped} : Vec2{clamped, definite};
}

std::string_view LayoutEngine::string(StringId id) const {
    if (id == kInvalidString) {
        return std::string_view{};
    }
    if (id < kRuntimeStringBase) {
        return m_ir->string(id);
    }
    if (id >= kRuntimeTextBase) {
        uint32_t slot = id - kRuntimeTextBase;
        if (m_runtimeTexts == nullptr || slot >= m_runtimeTexts->size()) {
            return std::string_view{};
        }
        return std::string_view((*m_runtimeTexts)[slot]);
    }
    uint32_t index = id - kRuntimeStringBase;
    if (m_runtimeStrings == nullptr || index >= m_runtimeStrings->size()) {
        return std::string_view{};
    }
    return std::string_view((*m_runtimeStrings)[index]);
}

void LayoutEngine::init(const IrView& ir, AssetResolver* assets, TextShaper* shaper) {
    m_ir = &ir;
    m_assets = assets;
    m_shaper = shaper;
}

void LayoutEngine::layout(NodeArena& arena, NodeId root, Rect viewport) {
    XT_ASSERT(m_ir != nullptr);
    // docs/design/21-perf-tracing.md §21.1's laidOutElements; stays 0 on either early return below.
    m_laidOutElements = 0;
    if (root == kInvalidNode || !arena.alive(root) || arena.kind(root) != NodeKind::Element) {
        return;
    }
    // First version (§6.3): relayout the whole tree whenever any element is dirty. kDirtyLayout
    // always propagates up to root (NodeArena::markDirty), so checking root's bit detects "any
    // element dirty" without a full-tree scan.
    if ((arena.dirty(root) & static_cast<uint8_t>(kDirtyLayout)) == 0) {
        refreshDirtyTransforms(arena, root);
        return;
    }

    // The `xt` root element's own box = viewport, no margin/padding/border (docs/tasks/T-25.md
    // "根（body 的父 xt 元素...）").
    ElementData& rootData = arena.element(root);
    ++m_laidOutElements;  // The root itself; every other element is counted by layoutChild().
    rootData.box = LayoutBox{};
    rootData.box.width = viewport.w;
    rootData.box.height = viewport.h;
    arena.common(root).visible = 1;

    // `body`'s width auto -> viewport.w (passed as availW); height auto -> content height (§6.3: an
    // auto-height body does *not* fill the viewport). The viewport height is still the definite
    // content height of body's containing block, so `body { height: 100% }` fills it (§6.4, T-74).
    float cursorY = layoutBlockChildren(arena, root, viewport.w, viewport.h, 0);
    rootData.box.scrollWidth = viewport.w;
    rootData.box.scrollHeight = cursorY > viewport.h ? cursorY : viewport.h;

    layoutAbsolutePass(arena, root);
    applyTransforms(arena, root);
}

void LayoutEngine::layoutChild(NodeArena& arena, NodeId node, float availW, float availH, float imposedH,
                               int depth) {
    ++m_laidOutElements;  // 21.1's laidOutElements.
    ElementData& data = arena.element(node);
    // docs/design/19-3d-space.md §19.4: a <camera> element is treated as display:none. T-177: so is
    // a `<repeat>` template element (defensive: the runtime keeps templates out of the tree).
    if (data.cameraNode != kInvalidNode || (arena.common(node).flags2 & kTemplate) != 0) {
        data.box = LayoutBox{};
        arena.common(node).visible = 0;
        return;
    }
    switch (data.style.display) {
        case Display::None:
            data.box = LayoutBox{};
            arena.common(node).visible = 0;
            return;
        case Display::Flex:
            layoutFlex(arena, node, availW, availH, imposedH, depth);
            return;
        case Display::Grid:  // 06 §6.15 (T-338).
            layoutGrid(arena, node, availW, availH, imposedH, depth);
            return;
        case Display::Block:
        default:
            layoutBlock(arena, node, availW, availH, imposedH, depth);
            return;
    }
}

bool LayoutEngine::isReplacedTag(std::string_view tag) {
    // T-191: external content lays out like an `<img>` (25.3.5b).
    return tag == "img" || tag == "anim" || tag == "video" || tag == "sequence" || tag == "external" ||
           tag == "svg";  // T-190: inline <svg> (11.9).
}

Vec2 LayoutEngine::intrinsicSize(NodeArena& arena, NodeId node) const {
    const Vec2 size = intrinsicSizeUnscaled(arena, node);
    const float s = arena.element(node).style.uiScaleEff;
    return s == 1.0f ? size : Vec2{size.x * s, size.y * s};
}

Vec2 LayoutEngine::intrinsicSizeUnscaled(NodeArena& arena, NodeId node) const {
    ElementData& data = arena.element(node);
    std::string_view tag = string(data.tag);
    if (data.externalNode != kInvalidNode) {
        // T-191: the size the host reported with its binding (video, scene view, engine/host
        // texture), or the sequence's first frame / atlas cell; an unbound one measures 0 x 0, or
        // the placeholder size while placeholders are on.
        const ExternalNodeData& ext = arena.external(data.externalNode);
        if (ext.source == ExternalSourceKind::Sequence) {
            if (ext.frameSize.x > 0.0f && ext.frameSize.y > 0.0f) {
                return ext.frameSize;
            }
            if (m_assets == nullptr || ext.frames.empty()) {
                return Vec2{0, 0};
            }
            return m_assets->imageSize(ext.frames.front(), *m_ir);
        }
        const AssetResolver::ExternalTexture* bound = m_assets != nullptr && isExternalAsset(ext.asset, *m_ir)
                                                          ? m_assets->externalTexture(ext.asset, *m_ir)
                                                          : nullptr;
        if (bound != nullptr && bound->width > 0 && bound->height > 0) {
            return Vec2{static_cast<float>(bound->width), static_cast<float>(bound->height)};
        }
        return m_externalPlaceholders ? Vec2{320, 180} : Vec2{0, 0};
    }
    if (tag == "img" || tag == "anim" || tag == "svg") {  // T-190: inline <svg> (11.9).
        // An `<anim>`, or an `<img src="*.svg">` the compiler turned into a static scene (T-110),
        // takes its intrinsic size from the scene's design size (12.2.1: the SVG viewBox).
        if (data.scene != kInvalidScene) {
            const SceneChunk& scene = m_ir->scene(data.scene);
            return Vec2{scene.width(), scene.height()};
        }
    }
    if (tag == "img") {
        if (m_assets == nullptr) {
            return Vec2{0, 0};
        }
        AssetId asset = kInvalidAsset;
        for (const auto& attr : data.attributes) {
            if (m_ir->string(attr.first) == "data-xt-asset") {
                asset = parseAssetIdOr(m_ir->string(attr.second));
                break;
            }
        }
        if (asset == kInvalidAsset) {
            return Vec2{0, 0};
        }
        if (isExternalAsset(asset, *m_ir)) {
            // T-150: an `engine://` image measures what the host bound, never a decoded file.
            const AssetResolver::ExternalTexture* ext = m_assets->externalTexture(asset, *m_ir);
            if (ext == nullptr) {
                return Vec2{0, 0};
            }
            return Vec2{static_cast<float>(ext->width), static_cast<float>(ext->height)};
        }
        return m_assets->imageSize(asset, *m_ir);
    }
    return Vec2{0, 0};
}

namespace {

/// §6.10: scale/translate for the anim element's scene root Group so the scene's design rect fits
/// `content` per `fit`. (1, 1) scale / content's top-left translate when the design size is
/// degenerate (either dimension <= 0).
void computeObjectFit(ObjectFit fit, Rect content, float designW, float designH, Vec2& outScale,
                      Vec2& outTranslate) {
    if (designW <= 0.0f || designH <= 0.0f) {
        outScale = Vec2{1, 1};
        outTranslate = Vec2{content.x, content.y};
        return;
    }
    float scaleX, scaleY;
    switch (fit) {
        case ObjectFit::Fill:
            scaleX = content.w / designW;
            scaleY = content.h / designH;
            break;
        case ObjectFit::Cover: {
            float s = std::max(content.w / designW, content.h / designH);
            scaleX = scaleY = s;
            break;
        }
        case ObjectFit::Contain:
        default: {
            float s = std::min(content.w / designW, content.h / designH);
            scaleX = scaleY = s;
            break;
        }
    }
    outScale = Vec2{scaleX, scaleY};
    outTranslate = Vec2{content.x + (content.w - designW * scaleX) * 0.5f,
                        content.y + (content.h - designH * scaleY) * 0.5f};
}

}  // namespace

void LayoutEngine::applyTransforms(NodeArena& arena, NodeId root) {
    // NOTE: this traversal only writes NodeCommon fields; it must not call arena.markDirty(...,
    // kDirtyTransform) here (that call reuses the same NodeArena scratch stack traversePreorder
    // drives its own iteration from -- see StyleEngine::resolve's identical caveat). Every touched
    // node is instead marked in one shot after the traversal, below.
    arena.traversePreorder(root, [&](NodeId node) {
        if (arena.kind(node) == NodeKind::Text) {
            // docs/tasks/T-30.md: a Text node sits at its parent element's content-box top-left
            // (relative to that element's border box); GlyphRun positions are relative to the text
            // node itself.
            NodeId textParent = arena.parent(node);
            Vec2 origin{0, 0};
            if (textParent != kInvalidNode && arena.kind(textParent) == NodeKind::Element) {
                Rect content = arena.element(textParent).box.contentBox();
                origin = Vec2{content.x, content.y};
            }
            NodeCommon& textCommon = arena.common(node);
            textCommon.anchor = Vec2{0, 0};
            textCommon.translate = origin;
            return true;
        }
        if (arena.kind(node) != NodeKind::Element) {
            return true;  // Group/Path: not touched by §6.7 (only Elements are).
        }
        applyElementTransform(arena, node);
        return true;
    });
    // One flood-fill markDirty covers root and every descendant (Elements, scene roots and their
    // subtrees, Text nodes), which is exactly the set of NodeCommon fields this pass may have
    // touched.
    arena.markDirty(root, static_cast<uint8_t>(kDirtyTransform));
}

void LayoutEngine::foldElementTranslate(NodeArena& arena, NodeId node) {
    const ElementData& data = arena.element(node);
    Length originX = data.originX;
    Length originY = data.originY;
    Vec2 anchorPx{resolveLength(originX, data.box.width, 0), resolveLength(originY, data.box.height, 0)};

    NodeId parent = arena.parent(node);
    float parentPadLeft = 0, parentPadTop = 0, parentScrollX = 0, parentScrollY = 0;
    if (parent != kInvalidNode && arena.kind(parent) == NodeKind::Element) {
        const ElementData& pdata = arena.element(parent);
        Rect pContent = pdata.box.contentBox();
        parentPadLeft = pContent.x;
        parentPadTop = pContent.y;
        // T-336: per axis, only a live scroll axis (`scroll`, or `auto` that overflows) offsets.
        const Vec2 scroll = appliedScroll(pdata);
        parentScrollX = scroll.x;
        parentScrollY = scroll.y;
    }

    NodeCommon& common = arena.common(node);
    common.anchor = anchorPx;
    // T-211: a percent translate resolves against the element's own border box, like
    // transform-origin above; the px and % parts are summed (CSS calc semantics).
    const float translateX = data.cssTranslate.x + data.cssTranslatePct.x * 0.01f * data.box.width;
    const float translateY = data.cssTranslate.y + data.cssTranslatePct.y * 0.01f * data.box.height;
    common.translate = Vec2{data.box.x + parentPadLeft - parentScrollX + translateX + anchorPx.x,
                            data.box.y + parentPadTop - parentScrollY + translateY + anchorPx.y};
}

void LayoutEngine::applyElementTransform(NodeArena& arena, NodeId node) {
    foldElementTranslate(arena, node);
    ElementData& data = arena.element(node);

    // §19.4/19.10 (T-47a): perspective/perspective-origin resolved to px, both relative to
    // this element's own border-box (06.10's revision). `perspective: none` (Auto) resolves to
    // 0 via resolveLength's Auto -> fallback.
    data.perspectivePx = resolveLength(data.style.base[prop::Perspective].len, data.box.width, 0);
    data.perspectiveOriginPx =
        Vec2{resolveLength(data.style.base[prop::PerspectiveOriginX].len, data.box.width, 0),
             resolveLength(data.style.base[prop::PerspectiveOriginY].len, data.box.height, 0)};

    if (data.scene != kInvalidScene && data.sceneRoot != kInvalidNode && arena.alive(data.sceneRoot)) {
        const SceneChunk& scene = m_ir->scene(data.scene);
        Rect content = data.box.contentBox();
        Vec2 scale, translate;
        computeObjectFit(data.style.objectFit, content, scene.width(), scene.height(), scale, translate);
        NodeCommon& groupCommon = arena.common(data.sceneRoot);
        groupCommon.scale = scale;
        groupCommon.translate = translate;
    }
}

void LayoutEngine::refreshDirtyTransforms(NodeArena& arena, NodeId root) {
    if (!arena.anyDirty(static_cast<uint8_t>(kDirtyTransform))) {
        return;
    }
    arena.traversePreorder(root, [&](NodeId node) {
        if (arena.kind(node) == NodeKind::Element &&
            (arena.dirty(node) & static_cast<uint8_t>(kDirtyTransform)) != 0) {
            foldElementTranslate(arena, node);
        }
        return true;
    });
}

}  // namespace xt
