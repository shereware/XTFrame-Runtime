#include <vector>

#include "css_values.h"
#include "xt/compile/style_model.h"
#include "xts_values_internal.h"

/// Per-property value parsing (split from xts_values.cpp by T-412): background-gradient and clip-path.
/// T-413: the grammar is core's css_values (05.5c, T-201, T-332); a malformed value returns false
/// with no diagnostic and the caller words the warning.

namespace xt::compile::xts_values_detail {

bool parseClipPathValue(const std::vector<Tok>& toks, ElementClipShape& out, bool& unsupported) {
    return css_values::parseClipPathValue(toks.data(), toks.size(), kCompiler, out, unsupported);
}

bool parseGradientValue(const std::vector<Tok>& toks, GradientDecl& out) {
    ElementGradient g;
    g.kind = out.kind;
    if (!css_values::parseGradientValue(toks.data(), toks.size(), kCompiler, g)) {
        return false;
    }
    out.kind = g.kind;
    out.angle = g.angle;
    out.rx = g.rx;
    out.ry = g.ry;
    out.cx = g.cx;
    out.cy = g.cy;
    out.stops = std::move(g.stops);
    return true;
}

}  // namespace xt::compile::xts_values_detail
