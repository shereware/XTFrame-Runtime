#include "xt/node.h"
#include "xt/text_shaper.h"

/// NullTextShaper: the no-op TextShaper used when no real text backend is wired in. See
/// Docs/design/07-text.md §7.2, Docs/tasks/T-28.md.

namespace xt {

FontId NullTextShaper::match(const FontRequest&) { return kInvalidFont; }

FontMetrics NullTextShaper::metrics(FontId) { return FontMetrics{}; }

void NullTextShaper::shape(std::string_view, const ShapeParams&, ShapeResult& out) {
    // Every ShapeParams field is ignored, ShapeParams::features (T-331) included.
    out.runs.clear();
    out.glyphs.clear();
    out.breakAfter.clear();
    out.graphemeStart.clear();
}

bool NullTextShaper::glyphOutline(FontId, uint32_t, std::vector<PathVerb>&, std::vector<float>&) {
    return false;
}

}  // namespace xt
