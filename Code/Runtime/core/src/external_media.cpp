#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>

#include "dispatch_guard.h"
#include "event_dispatcher.h"
#include "trigger_runtime.h"
#include "xt/asset_resolver.h"
#include "xt/charconv.h"
#include "xt/document.h"
#include "xt/node_arena.h"
#include "xt/num_guard.h"
#include "xt/text_shaper.h"

/// External content layers: `<video>`, `<sequence>` and `<external>` (Docs/design/25-editor-
/// blueprint.md §25.3.5b, 03 §3.12.1, 05 §5.9.1, 13 §13.8.3, 04 §4.10.2, Docs/tasks/T-191.md).
///
/// Every such element gets a NodeKind::External child at load time (loadExternal) holding its
/// ExternalNodeData; the element itself draws the content as its image, so object-fit, radius,
/// masks and effects apply as for an `<img>`:
///   - video / scene view / engine / host texture: the element's `engine://` ASST entry, bound by
///     the host (IExternalTexture::bindTexture, or bindImage for a software renderer). A video is
///     decoded and played by the host only; the core keeps its state and sends ExternalRequests.
///   - sequence: the core plays it -- `imageAsset` switches to the current frame's asset (or
///     `imageSubUv` to the current atlas cell) and the next setSequencePreload() frames are
///     decoded ahead through the AssetResolver.

namespace xt {

namespace {

constexpr uint32_t bit(ExternalSourceKind kind) { return 1u << static_cast<uint32_t>(kind); }
constexpr uint32_t kMediaMask = bit(ExternalSourceKind::Video) | bit(ExternalSourceKind::Sequence);
constexpr uint32_t kBindMask = bit(ExternalSourceKind::Video) | bit(ExternalSourceKind::SceneView) |
                               bit(ExternalSourceKind::Engine) | bit(ExternalSourceKind::Host);
std::string_view attribute(const IrView& ir, const ElementData& data, std::string_view name, bool& present) {
    for (const auto& entry : data.attributes) {
        if (ir.string(entry.first) == name) {
            present = true;
            return ir.string(entry.second);
        }
    }
    present = false;
    return {};
}

std::string_view attribute(const IrView& ir, const ElementData& data, std::string_view name) {
    bool present = false;
    return attribute(ir, data, name, present);
}

/// A boolean attribute: present and not "false".
bool flagAttribute(const IrView& ir, const ElementData& data, std::string_view name) {
    bool present = false;
    std::string_view value = attribute(ir, data, name, present);
    return present && value != "false";
}

float positiveOr(std::string_view text, float fallback) {
    float value = 0;
    if (text.empty() || xt::fromChars(text.data(), text.data() + text.size(), value).ec != std::errc{} ||
        !(value > 0.0f)) {
        return fallback;
    }
    return value;
}

/// `m:ss`.
std::string formatDuration(float seconds) {
    const int total = static_cast<int>(std::lround(seconds));
    const int s = total % 60;
    return std::to_string(total / 60) + (s < 10 ? ":0" : ":") + std::to_string(s);
}

}  // namespace

void Document::loadExternal(NodeId element) {
    const ElementData& data = m_arena.element(element);
    const std::string_view tag = string(data.tag);
    ExternalNodeData x;
    x.element = element;
    x.loop = flagAttribute(m_ir, data, "loop") ? 1 : 0;
    x.autoplay = flagAttribute(m_ir, data, "autoplay") ? 1 : 0;
    x.pendingAutoplay = x.autoplay;
    x.sync = flagAttribute(m_ir, data, "sync") ? 1 : 0;
    x.rate = positiveOr(attribute(m_ir, data, "rate"), 1.0f);
    x.asset = data.imageAsset;
    AssetId firstFrame = kInvalidAsset;
    if (tag == "video") {
        x.source = ExternalSourceKind::Video;
    } else if (tag == "sequence") {
        x.source = ExternalSourceKind::Sequence;
        x.fps = positiveOr(attribute(m_ir, data, "fps"), 24.0f);
        const std::string_view list = attribute(m_ir, data, "data-xt-frames");
        size_t pos = 0;
        while (pos < list.size()) {
            size_t end = list.find(',', pos);
            if (end == std::string_view::npos) {
                end = list.size();
            }
            uint32_t id = 0;
            if (std::from_chars(list.data() + pos, list.data() + end, id).ec == std::errc{}) {
                x.frames.push_back(id);
            }
            pos = end + 1;
        }
        if (!x.frames.empty()) {
            x.asset = kInvalidAsset;
            firstFrame = x.frames.front();
            x.duration = static_cast<float>(x.frames.size()) / x.fps;
        } else {
            // An atlas: `frame-size="WxH"`, `frames` (default: every whole cell, known once the
            // atlas size is -- see stepExternalMedia()).
            const std::string_view size = attribute(m_ir, data, "frame-size");
            const size_t cross = size.find('x');
            if (cross != std::string_view::npos) {
                x.frameSize =
                    Vec2{positiveOr(size.substr(0, cross), 0.0f), positiveOr(size.substr(cross + 1), 0.0f)};
            }
            x.atlasFrames = toUint32Sat(positiveOr(attribute(m_ir, data, "frames"), 0.0f));
            if (x.atlasFrames > 0) {
                x.duration = static_cast<float>(x.atlasFrames) / x.fps;
            }
        }
    } else {
        const std::string_view source = attribute(m_ir, data, "source");
        x.source = source.rfind("scene-view:", 0) == 0 ? ExternalSourceKind::SceneView
                   : source.rfind("host:", 0) == 0     ? ExternalSourceKind::Host
                                                       : ExternalSourceKind::Engine;
    }
    NodeId ext = m_arena.create(NodeKind::External);
    m_arena.external(ext) = std::move(x);
    m_arena.appendChild(element, ext);
    ElementData& owner = m_arena.element(element);
    owner.externalNode = ext;
    if (firstFrame != kInvalidAsset) {
        owner.imageAsset = firstFrame;
    }
    m_externalElements.push_back(element);
}

NodeId Document::externalOf(NodeId element) const {
    if (!m_arena.alive(element)) {
        return kInvalidNode;
    }
    if (m_arena.kind(element) == NodeKind::External) {
        return element;
    }
    if (m_arena.kind(element) != NodeKind::Element) {
        return kInvalidNode;
    }
    return m_arena.element(element).externalNode;
}

const ExternalNodeData* Document::externalData(NodeId node) const {
    NodeId ext = externalOf(node);
    return ext == kInvalidNode ? nullptr : &m_arena.external(ext);
}

NodeId Document::externalFor(NodeId node, uint32_t mask) const {
    NodeId ext = externalOf(node);
    if (ext == kInvalidNode || (bit(m_arena.external(ext).source) & mask) == 0) {
        return kInvalidNode;
    }
    return ext;
}

void Document::setExternalRequestListener(ExternalRequestListener listener) {
    m_externalRequestListener = std::move(listener);
}

void Document::setExternalPlaceholders(bool on) {
    if (m_externalPlaceholders == on) {
        return;
    }
    m_externalPlaceholders = on;
    m_layoutEngine.setExternalPlaceholders(on);
    // The intrinsic size of an unbound element changes (0 x 0 <-> the placeholder size).
    for (NodeId element : m_externalElements) {
        if (m_arena.alive(element)) {
            m_arena.markDirty(element, static_cast<uint8_t>(kDirtyLayout));
        }
    }
}

void Document::sendExternalRequest(NodeId ext, ExternalRequestKind kind, float value) {
    if (!m_externalRequestListener || m_unloading) {  // T-260a: none from an unloading Document.
        return;
    }
    const ExternalNodeData& x = m_arena.external(ext);
    if (!m_arena.alive(x.element)) {
        return;
    }
    const ElementData& data = m_arena.element(x.element);
    ExternalRequest request;
    request.kind = kind;
    request.element = x.element;
    request.elementId = data.id == kInvalidString ? std::string_view{} : string(data.id);
    request.src = attribute(m_ir, data, "src");
    request.value = value;
    request.instanceId = m_instanceId;
    m_externalRequestListener(request);
}

// --- IExternalTexture ------------------------------------------------------------------------

Status Document::bindTexture(NodeId node, uint32_t textureHandle, int width, int height) {
    NodeId ext = externalFor(node, kBindMask);
    if (ext == kInvalidNode || width < 0 || height < 0) {
        return Status::kInvalidArgument;
    }
    if (m_assets == nullptr) {
        return Status::kUnsupported;
    }
    const ExternalNodeData& x = m_arena.external(ext);
    if (!isExternalAsset(x.asset, m_ir)) {
        return Status::kInvalidArgument;
    }
    const std::string_view name = externalAssetName(m_ir.string(m_ir.assets().assets()[x.asset].name));
    const AssetResolver::ExternalTexture* prev = m_assets->externalTexture(name);
    const bool resized = prev == nullptr || prev->width != width || prev->height != height;
    m_assets->setExternalTexture(name, textureHandle, width, height);
    if (resized) {
        m_arena.markDirty(x.element, static_cast<uint8_t>(kDirtyLayout));
    }
    m_externalDamaged.push_back(x.element);
    return Status::kOk;
}

Status Document::bindImage(NodeId node, const ImageData* pixels) {
    NodeId ext = externalFor(node, kBindMask);
    if (ext == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    if (m_assets == nullptr) {
        return Status::kUnsupported;
    }
    const ExternalNodeData& x = m_arena.external(ext);
    if (!isExternalAsset(x.asset, m_ir)) {
        return Status::kInvalidArgument;
    }
    const std::string_view name = externalAssetName(m_ir.string(m_ir.assets().assets()[x.asset].name));
    const AssetResolver::ExternalTexture* prev = m_assets->externalTexture(name);
    const bool resized = pixels != nullptr &&
                         (prev == nullptr || prev->width != pixels->width || prev->height != pixels->height);
    m_assets->setExternalPixels(name, pixels);
    if (resized) {
        m_arena.markDirty(x.element, static_cast<uint8_t>(kDirtyLayout));
    }
    m_externalDamaged.push_back(x.element);
    return Status::kOk;
}

Status Document::unbindExternal(NodeId node) {
    NodeId ext = externalFor(node, kBindMask);
    if (ext == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    if (m_assets == nullptr) {
        return Status::kUnsupported;
    }
    const ExternalNodeData& x = m_arena.external(ext);
    if (!isExternalAsset(x.asset, m_ir)) {
        return Status::kInvalidArgument;
    }
    m_assets->clearExternalTexture(externalAssetName(m_ir.string(m_ir.assets().assets()[x.asset].name)));
    m_arena.markDirty(x.element, static_cast<uint8_t>(kDirtyLayout));
    m_externalDamaged.push_back(x.element);
    return Status::kOk;
}

Status Document::setPlayback(NodeId node, MediaState state, float time) {
    NodeId ext = externalFor(node, bit(ExternalSourceKind::Video));
    if (ext == kInvalidNode || !std::isfinite(time)) {
        return Status::kInvalidArgument;
    }
    ExternalNodeData& x = m_arena.external(ext);
    x.time = std::max(0.0f, time);
    if (state == MediaState::Ended) {
        endMedia(ext);
    } else {
        x.state = state;
    }
    return Status::kOk;
}

Status Document::setMediaDuration(NodeId node, float seconds) {
    NodeId ext = externalFor(node, bit(ExternalSourceKind::Video));
    if (ext == kInvalidNode || !std::isfinite(seconds)) {
        return Status::kInvalidArgument;
    }
    m_arena.external(ext).duration = seconds < 0.0f ? -1.0f : seconds;
    return Status::kOk;
}

void Document::takeExternalDamage(std::vector<Rect>& out) {
    for (NodeId element : m_externalDamaged) {
        if (!m_arena.alive(element) || m_arena.kind(element) != NodeKind::Element) {
            continue;
        }
        const LayoutBox& box = m_arena.element(element).box;
        const Vec2 corners[4] = {nodeToScreen(element, Vec2{0, 0}), nodeToScreen(element, Vec2{box.width, 0}),
                                 nodeToScreen(element, Vec2{box.width, box.height}),
                                 nodeToScreen(element, Vec2{0, box.height})};
        float x0 = corners[0].x, y0 = corners[0].y, x1 = corners[0].x, y1 = corners[0].y;
        for (const Vec2& c : corners) {
            x0 = std::min(x0, c.x);
            y0 = std::min(y0, c.y);
            x1 = std::max(x1, c.x);
            y1 = std::max(y1, c.y);
        }
        out.push_back(Rect{x0, y0, x1 - x0, y1 - y0});
    }
    m_externalDamaged.clear();
}

// --- Media control -----------------------------------------------------------------------------

void Document::endMedia(NodeId ext) {
    ExternalNodeData& x = m_arena.external(ext);
    if (x.state == MediaState::Ended) {
        return;
    }
    x.state = MediaState::Ended;
    x.syncRunning = 0;
    m_pendingMediaEnds.push_back(x.element);
}

void Document::setSequenceTime(NodeId ext, float time, bool preload) {
    ExternalNodeData& x = m_arena.external(ext);
    x.time = std::max(0.0f, time);
    const uint32_t count = x.frames.empty() ? x.atlasFrames : static_cast<uint32_t>(x.frames.size());
    if (count == 0 || !m_arena.alive(x.element)) {
        return;
    }
    // A tiny epsilon so t = n / fps lands on frame n despite float rounding.
    const double index = std::floor(static_cast<double>(x.time) * static_cast<double>(x.fps) + 1e-4);
    const uint32_t frame = index <= 0 ? 0u : static_cast<uint32_t>(std::min<double>(index, count - 1));
    x.frame = frame;
    ElementData& data = m_arena.element(x.element);
    if (!x.frames.empty()) {
        data.imageAsset = x.frames[frame];
        if (preload && m_assets != nullptr && x.preloadedFrom != frame) {
            x.preloadedFrom = frame;
            for (uint32_t k = 0; k < m_sequencePreload; ++k) {
                uint32_t i = frame + k;
                if (i >= count) {
                    if (x.loop == 0) {
                        break;
                    }
                    i %= count;
                }
                m_assets->imageView(x.frames[i], m_ir);  // Decoded (and cached) ahead of use.
            }
        }
        return;
    }
    // Atlas: row-major cells of frameSize.
    if (m_assets == nullptr || !(x.frameSize.x > 0.0f) || !(x.frameSize.y > 0.0f)) {
        return;
    }
    const Vec2 size = m_assets->imageSize(x.asset, m_ir);
    const uint32_t cols = size.x > 0.0f ? toUint32Sat(size.x / x.frameSize.x) : 0u;
    if (cols == 0 || !(size.y > 0.0f)) {
        return;
    }
    const float col = static_cast<float>(frame % cols);
    const float row = static_cast<float>(frame / cols);
    data.imageAsset = x.asset;
    data.imageSubUv = Rect{col * x.frameSize.x / size.x, row * x.frameSize.y / size.y, x.frameSize.x / size.x,
                           x.frameSize.y / size.y};
}

// T-260a: every media call below that sends a video request (ContextListener::onExternalRequest)
// holds a DispatchGuard (its first local).
Status Document::playMedia(NodeId node, bool loop) {
    DispatchGuard dispatch(m_context);
    NodeId ext = externalFor(node, kMediaMask);
    if (ext == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    ExternalNodeData& x = m_arena.external(ext);
    if (loop && x.loop == 0) {
        x.loop = 1;
        if (x.source == ExternalSourceKind::Video) {
            sendExternalRequest(ext, ExternalRequestKind::Loop, 1.0f);
        }
    }
    if (x.state == MediaState::Ended || (x.duration > 0.0f && x.time >= x.duration)) {
        x.time = 0;
    }
    x.state = MediaState::Playing;
    if (x.source == ExternalSourceKind::Video) {
        sendExternalRequest(ext, ExternalRequestKind::Play, x.time);
    } else {
        setSequenceTime(ext, x.time, true);
    }
    return Status::kOk;
}

Status Document::pauseMedia(NodeId node) {
    DispatchGuard dispatch(m_context);
    NodeId ext = externalFor(node, kMediaMask);
    if (ext == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    ExternalNodeData& x = m_arena.external(ext);
    if (x.state == MediaState::Playing) {
        x.state = MediaState::Paused;
    }
    if (x.source == ExternalSourceKind::Video) {
        sendExternalRequest(ext, ExternalRequestKind::Pause, x.time);
    }
    return Status::kOk;
}

Status Document::stopMedia(NodeId node) {
    DispatchGuard dispatch(m_context);
    NodeId ext = externalFor(node, kMediaMask);
    if (ext == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    ExternalNodeData& x = m_arena.external(ext);
    x.state = MediaState::Stopped;
    x.syncRunning = 0;
    if (x.source == ExternalSourceKind::Video) {
        x.time = 0;
        sendExternalRequest(ext, ExternalRequestKind::Pause, 0.0f);
        sendExternalRequest(ext, ExternalRequestKind::Seek, 0.0f);
    } else {
        setSequenceTime(ext, 0.0f, true);
    }
    return Status::kOk;
}

Status Document::seekMedia(NodeId node, float seconds) {
    DispatchGuard dispatch(m_context);
    NodeId ext = externalFor(node, kMediaMask);
    if (ext == kInvalidNode || !std::isfinite(seconds)) {
        return Status::kInvalidArgument;
    }
    ExternalNodeData& x = m_arena.external(ext);
    float t = std::max(0.0f, seconds);
    if (x.duration > 0.0f) {
        t = std::min(t, x.duration);
    }
    if (x.state == MediaState::Ended && (x.duration <= 0.0f || t < x.duration)) {
        x.state = MediaState::Paused;
    }
    if (x.source == ExternalSourceKind::Video) {
        x.time = t;
        sendExternalRequest(ext, ExternalRequestKind::Seek, t);
    } else {
        setSequenceTime(ext, t, true);
    }
    return Status::kOk;
}

Status Document::setMediaRate(NodeId node, float rate) {
    DispatchGuard dispatch(m_context);
    NodeId ext = externalFor(node, kMediaMask);
    if (ext == kInvalidNode || !(rate > 0.0f) || !std::isfinite(rate)) {
        return Status::kInvalidArgument;
    }
    m_arena.external(ext).rate = rate;
    if (m_arena.external(ext).source == ExternalSourceKind::Video) {
        sendExternalRequest(ext, ExternalRequestKind::Rate, rate);
    }
    return Status::kOk;
}

Status Document::setMediaLoop(NodeId node, bool loop) {
    DispatchGuard dispatch(m_context);
    NodeId ext = externalFor(node, kMediaMask);
    if (ext == kInvalidNode) {
        return Status::kInvalidArgument;
    }
    m_arena.external(ext).loop = loop ? 1 : 0;
    if (m_arena.external(ext).source == ExternalSourceKind::Video) {
        sendExternalRequest(ext, ExternalRequestKind::Loop, loop ? 1.0f : 0.0f);
    }
    return Status::kOk;
}

// --- Per frame -------------------------------------------------------------------------------

void Document::stepExternalMedia(float clock) {
    const float dt = m_mediaClockValid ? std::max(0.0f, clock - m_mediaClock) : 0.0f;
    m_mediaClock = clock;
    m_mediaClockValid = true;
    for (NodeId element : m_externalElements) {
        if (!m_arena.alive(element) || m_arena.kind(element) != NodeKind::Element) {
            continue;
        }
        const NodeId ext = m_arena.element(element).externalNode;
        if (ext == kInvalidNode) {
            continue;
        }
        ExternalNodeData& x = m_arena.external(ext);
        if (x.source == ExternalSourceKind::Sequence && x.frames.empty() && x.atlasFrames == 0 &&
            m_assets != nullptr && x.frameSize.x > 0.0f && x.frameSize.y > 0.0f) {
            // An atlas without `frames`: every whole cell, once its size is known.
            const Vec2 size = m_assets->imageSize(x.asset, m_ir);
            // T-258a: saturating casts, and a product past uint32 saturates too.
            const uint64_t cellsWide = static_cast<uint64_t>(toUint32Sat(size.x / x.frameSize.x)) *
                                       static_cast<uint64_t>(toUint32Sat(size.y / x.frameSize.y));
            const auto cells = static_cast<uint32_t>(std::min<uint64_t>(cellsWide, UINT32_MAX));
            if (cells > 0) {
                x.atlasFrames = cells;
                x.duration = static_cast<float>(cells) / x.fps;
                setSequenceTime(ext, x.time, false);
            }
        }
        if (x.pendingAutoplay != 0) {
            x.pendingAutoplay = 0;
            if (x.source == ExternalSourceKind::Sequence || x.source == ExternalSourceKind::Video) {
                playMedia(element, false);
            }
            continue;  // Starts at time 0 this frame.
        }
        if (x.source != ExternalSourceKind::Sequence || x.state != MediaState::Playing) {
            continue;
        }
        float t = x.time + dt * x.rate;
        if (x.duration > 0.0f && t >= x.duration) {
            if (x.loop != 0) {
                t = std::fmod(t, x.duration);
            } else {
                setSequenceTime(ext, x.duration, false);
                endMedia(ext);
                continue;
            }
        }
        setSequenceTime(ext, t, true);
    }
}

void Document::refreshExternalPlaceholders() {
    for (NodeId element : m_externalElements) {
        if (!m_arena.alive(element) || m_arena.kind(element) != NodeKind::Element) {
            continue;
        }
        const NodeId ext = m_arena.element(element).externalNode;
        if (ext == kInvalidNode) {
            continue;
        }
        ExternalNodeData& x = m_arena.external(ext);
        if (x.source == ExternalSourceKind::Sequence) {
            continue;
        }
        const AssetResolver::ExternalTexture* binding = m_assets != nullptr && isExternalAsset(x.asset, m_ir)
                                                            ? m_assets->externalTexture(x.asset, m_ir)
                                                            : nullptr;
        x.bound = binding != nullptr && (binding->texture != 0 || binding->pixels != nullptr) ? 1 : 0;
        x.showPlaceholder = m_externalPlaceholders && x.bound == 0 ? 1 : 0;
        if (x.showPlaceholder == 0) {
            x.label.clear();
            x.labelText.clear();
            continue;
        }
        const ElementData& data = m_arena.element(element);
        const Rect content = data.box.contentBox();
        std::string text;
        if (data.id != kInvalidString) {
            text = std::string(string(data.id));
        } else {
            text =
                std::string(attribute(m_ir, data, x.source == ExternalSourceKind::Video ? "src" : "source"));
        }
        if (x.source == ExternalSourceKind::Video && x.duration >= 0.0f) {
            text += "  " + formatDuration(x.duration);  // Once the host reported it.
        }
        const float size = 12.0f;
        if (m_shaper == nullptr || data.style.resolvedFont == kInvalidFont || content.h < size * 2.0f) {
            x.label.clear();
            x.labelText.clear();
            continue;
        }
        const Vec2 origin{content.x + 8.0f, content.y + 8.0f};
        if (text == x.labelText && size == x.labelSize && origin.x == x.labelOrigin.x &&
            origin.y == x.labelOrigin.y) {
            continue;
        }
        x.labelText = text;
        x.labelSize = size;
        x.labelOrigin = origin;
        x.labelColor = Color{0.86f, 0.86f, 0.88f, 1.0f};
        x.label.clear();
        // The element's own font families, like its text would use.
        std::vector<std::string_view> families;
        const std::span<const uint32_t> pool = m_ir.style().fontFamilies();
        for (uint32_t i = 0; i < data.style.fontFamilyCount; ++i) {
            const uint32_t index = data.style.fontFamilyOffset + i;
            if (index < pool.size()) {
                families.push_back(m_ir.string(pool[index]));
            }
        }
        ShapeParams params;
        params.font.families = std::span<const std::string_view>(families);
        params.size = size;
        ShapeResult shaped;
        m_shaper->shape(text, params, shaped);
        const float ascent = m_shaper->metrics(data.style.resolvedFont).ascent * size;
        float pen = origin.x;
        const float baseline = origin.y + (ascent > 0.0f ? ascent : size * 0.8f);
        for (const ShapedRun& run : shaped.runs) {
            GlyphRun out;
            out.font = run.font;
            out.fontSize = size;
            for (uint32_t g = run.glyphStart; g < run.glyphStart + run.glyphCount && g < shaped.glyphs.size();
                 ++g) {
                const ShapedGlyph& glyph = shaped.glyphs[g];
                out.glyphs.push_back(glyph.glyphId);
                out.positions.push_back(Vec2{pen + glyph.xOffset, baseline + glyph.yOffset});
                pen += glyph.xAdvance;
            }
            if (!out.glyphs.empty()) {
                x.label.push_back(std::move(out));
            }
        }
    }
}

void Document::flushMediaEvents() {
    if (m_pendingMediaEnds.empty()) {
        return;
    }
    std::vector<NodeId> batch;
    batch.swap(m_pendingMediaEnds);
    for (NodeId element : batch) {
        if (!m_arena.alive(element) || m_arena.kind(element) != NodeKind::Element) {
            continue;
        }
        Event event;
        event.type = EventType::MediaEnd;
        event.target = element;
        m_events->dispatch(m_arena, event);
        const StringId id = m_arena.element(element).id;
        if (id != kInvalidString) {
            m_triggers->onMediaEnd(id);  // `media(<id>).end`, `play(<id>) then ...`.
        }
    }
}

// --- `@timeline` sync (04 §4.10.2) ------------------------------------------------------------

void Document::syncExternalClip(NodeId ext, float t, bool groupPlaying) {
    ExternalNodeData& x = m_arena.external(ext);
    if (m_arena.alive(x.element)) {
        const ComputedStyle& style = m_arena.element(x.element).style;
        x.clipStart = style.clipStart;
        x.clipRate = style.clipRate > 0.0f ? style.clipRate : 1.0f;
    }
    float local = (t - x.clipStart) * x.clipRate;
    if (local < 0.0f) {
        local = 0.0f;
    }
    if (x.duration > 0.0f) {
        local = x.loop != 0 ? std::fmod(local, x.duration) : std::min(local, x.duration);
    }
    if (x.source == ExternalSourceKind::Sequence) {
        // The group is the clock: the sequence shows exactly the frame of its local time.
        x.state = MediaState::Paused;
        setSequenceTime(ext, local, true);
        return;
    }
    // A video is decoded by the host: while the group plays and the clip has started, ask the host
    // to play once from the local time; a paused group (a seek) pauses and seeks it instead.
    const bool started = t >= x.clipStart;
    x.time = local;
    if (groupPlaying) {
        if (started && x.syncRunning == 0) {
            x.syncRunning = 1;
            x.state = MediaState::Playing;
            if (x.clipRate != x.rate) {
                x.rate = x.clipRate;
                sendExternalRequest(ext, ExternalRequestKind::Rate, x.rate);
            }
            sendExternalRequest(ext, ExternalRequestKind::Seek, local);
            sendExternalRequest(ext, ExternalRequestKind::Play, local);
        } else if (!started && x.syncRunning != 0) {
            // A looping group wrapped back before the clip: hold it at its start.
            x.syncRunning = 0;
            x.state = MediaState::Paused;
            sendExternalRequest(ext, ExternalRequestKind::Pause, local);
            sendExternalRequest(ext, ExternalRequestKind::Seek, local);
        }
        return;
    }
    if (x.syncRunning != 0 || x.state == MediaState::Playing) {
        sendExternalRequest(ext, ExternalRequestKind::Pause, local);
    }
    x.syncRunning = 0;
    if (x.state != MediaState::Ended) {
        x.state = MediaState::Paused;
    }
    sendExternalRequest(ext, ExternalRequestKind::Seek, local);
}

void Document::releaseExternalClip(NodeId ext) {
    ExternalNodeData& x = m_arena.external(ext);
    if (x.source == ExternalSourceKind::Video && x.syncRunning != 0) {
        sendExternalRequest(ext, ExternalRequestKind::Pause, x.time);
        if (x.state == MediaState::Playing) {
            x.state = MediaState::Paused;
        }
    }
    x.syncRunning = 0;
}

}  // namespace xt
