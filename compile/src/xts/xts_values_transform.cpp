#include <string>
#include <vector>

#include "css_values.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/style_model.h"
#include "xt/property.h"
#include "xts_lexer.h"
#include "xts_values_internal.h"

/// Per-property value parsing (split from xts_values.cpp by T-412): transform, the independent transform
/// properties and the origin / background position values. T-413: the grammar is core's
/// css_values (05.5's fixed-order and compose paths, T-329's independent properties); this file
/// adds the declaration's diagnostics and turns the results into StyleDecls.

namespace xt::compile {

using namespace xts_values_detail;  // T-412: helpers shared between the split files

namespace {

css_values::IndependentTransform independentKind(std::string_view name) {
    return name == "translate" ? css_values::IndependentTransform::Translate
           : name == "rotate"  ? css_values::IndependentTransform::Rotate
                               : css_values::IndependentTransform::Scale;
}

using OriginParser = bool (*)(const Tok*, size_t, css_values::Dialect, Length&, Length&);

bool parseLengthPair(OriginParser parse, const std::vector<Tok>& toks, PropId xProp, PropId yProp,
                     std::vector<StyleDecl>& out, int line) {
    Length x, y;
    if (!parse(toks.data(), toks.size(), kCompiler, x, y)) {
        return false;
    }
    appendPropValues({{xProp, Value::ofLength(x.value, x.unit)}, {yProp, Value::ofLength(y.value, y.unit)}},
                     line, out);
    return true;
}

}  // namespace

bool independentTransformFnTokens(std::string_view name, const std::vector<Token>& toks,
                                  std::string_view fileName, int line, DiagnosticSink& sink,
                                  std::vector<Token>& fnToks, bool& isNone) {
    fnToks.clear();
    const std::vector<Tok> values = toValueTokens(toks);
    std::vector<Tok> fn;
    SinkDiag diag(sink, fileName, line);
    if (!css_values::independentTransformFnToks(independentKind(name), values.data(), values.size(),
                                                kCompiler, &diag, fn, isNone)) {
        return false;
    }
    for (const Tok& t : fn) {
        fnToks.push_back(toLexerToken(t));
    }
    return true;
}

namespace xts_values_detail {

bool parseTransformShorthand(const std::vector<Tok>& toks, std::string_view fileName, int line,
                             DiagnosticSink& sink, std::vector<StyleDecl>& out) {
    SinkDiag diag(sink, fileName, line);
    std::vector<css_values::PropValue> values;
    if (!css_values::parseTransform(toks.data(), toks.size(), &diag, values)) {
        return false;
    }
    appendPropValues(values, line, out);
    return true;
}

bool parseIndependentTransform(std::string_view name, const std::vector<Tok>& toks, std::string_view fileName,
                               int line, DiagnosticSink& sink, std::vector<StyleDecl>& out) {
    SinkDiag diag(sink, fileName, line);
    std::vector<css_values::PropValue> values;
    if (!css_values::parseIndependentTransform(independentKind(name), toks.data(), toks.size(), kCompiler,
                                               &diag, values)) {
        return false;
    }
    appendPropValues(values, line, out);
    return true;
}

bool parseTransformOrigin(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line) {
    return parseLengthPair(css_values::parseOriginPair, toks, xt::prop::AnchorX, xt::prop::AnchorY, out,
                           line);
}

bool parsePerspectiveOrigin(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line) {
    return parseLengthPair(css_values::parseOriginPair, toks, xt::prop::PerspectiveOriginX,
                           xt::prop::PerspectiveOriginY, out, line);
}

/// T-327 (05 §5.9.8): the origin grammar into BackgroundPositionX/Y.
bool parseBackgroundPosition(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line) {
    return parseLengthPair(css_values::parseOriginPair, toks, xt::prop::BackgroundPositionX,
                           xt::prop::BackgroundPositionY, out, line);
}

/// T-327 (05 §5.9.8): `background-size` into BackgroundSizeX/Y.
bool parseBackgroundSize(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line) {
    return parseLengthPair(css_values::parseBackgroundSize, toks, xt::prop::BackgroundSizeX,
                           xt::prop::BackgroundSizeY, out, line);
}

}  // namespace xts_values_detail

}  // namespace xt::compile
