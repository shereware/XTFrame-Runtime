#pragma once

#include <string_view>

#include "xt/document.h"
#include "xt/types.h"

/// World-layer output of one Document (Docs/design/33-world-layers.md, Docs/tasks/T-701.md): the
/// per-Document half of Context::setWorldLayerOutput / Context::pushPointerRay, for a host that
/// drives a Document without a Context's queue (and for tests). Kept out of xt/document.h.
///
/// Off (the default) the `world-layer` style property is ignored and nothing changes. On:
///   - every element with `world-layer: on` and its subtree leave the ordinary paint order and
///     become a layer of their own (at most kMaxWorldLayers per Document, the base layer included;
///     the rest stay where they are and kWarnWorldLayerLimit is reported once);
///   - Document::paint() fills CommandList::worldLayers with one self-contained command range per
///     layer (WorldLayerRange: the element's NodeId, the paint order, the layer plane's transform
///     to the root box without any camera or `perspective`, the rectangle the range covers on the
///     plane, the opacity left to the host);
///   - hit tests take a ray in root-box coordinates (logical px, x right, y down, +z toward the
///     design-time viewer): the layers' planes from the nearest intersection to the farthest,
///     coplanar ones last painted first, a `backface-visibility: hidden` layer never from behind.
///     A hit test that only has a 2D position uses the ray straight down -z through it.

namespace xt {

/// The Document::warnings() entry (once per document) when more elements have `world-layer: on`
/// than kMaxWorldLayers allows in world-layer output (33 §33.2 item 6).
inline constexpr std::string_view kWarnWorldLayerLimit =
    "world-layer: more than 64 layers; the extra elements are painted in the layer they sit in";
/// The Document::warnings() entry (once per document) when a world layer's content was wider or
/// taller than three times the root box and was cut about its centre (33 §33.4 item 2 as revised
/// by §33.13 item 1, T-703).
inline constexpr std::string_view kWarnWorldLayerClamped =
    "world-layer: a layer's content is more than three times the root box's size and was cut";

/// Turns world-layer output of `document` on or off; takes effect at its next update() / paint().
void setWorldLayerOutput(Document& document, bool on);
/// Whether `document` is in world-layer output.
bool worldLayerOutput(const Document& document);
/// The topmost Element the ray `origin` + t `direction` (t >= 0) hits in world-layer output;
/// kInvalidNode when nothing is hit or `document` is not in world-layer output.
NodeId worldLayerHitTest(const Document& document, Vec3 origin, Vec3 direction);
/// The pointer's ray for the hit tests of the input events at the 2D position `pos` that
/// `document` is handed next (Document::dispatchInput); it stays until the next call. What
/// Context::pushPointerRay does for every Document when it dispatches such an event.
void setWorldLayerPointerRay(Document& document, Vec3 origin, Vec3 direction, Vec2 pos);

}  // namespace xt
