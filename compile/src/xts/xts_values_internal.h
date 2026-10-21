#pragma once

// T-412: declarations shared by the xts_values*.cpp files (split from one file along its section
// comments; docs/tasks/T-412.md). Internal to xt_compile's style parser: only those files include it.
// T-413: the value grammar itself is core's (core/src/css_values.h, shared with the runtime's
// mini_css); these files read `.xts` values through it on css_values tokens and add only the
// compiler's diagnostics and the `.xts`-only syntax.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "css_values.h"  // core/src: the CSS value grammar (T-413).
#include "xt/charconv.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_value.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/num_guard.h"
#include "xt/property.h"
#include "xts_lexer.h"
#include "xts_props.h"

namespace xt::compile {

// Declared in xts_parser.cpp; defined here. They take the lexer's tokens and convert them once.
bool parseAndExpandDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName,
                        int line, DiagnosticSink& sink, std::vector<StyleDecl>& outDecls,
                        std::vector<EasingPoint>* easingPoints = nullptr);
bool parseEasingValue(const std::vector<Token>& tokens, Easing& out, bool* stepsClamped = nullptr,
                      std::vector<EasingPoint>* easingPoints = nullptr);
bool containsVarFunction(const std::vector<Token>& tokens);
// T-329: also used by xts_parser.cpp to combine `translate`/`rotate`/`scale` with `transform`.
bool independentTransformFnTokens(std::string_view name, const std::vector<Token>& toks,
                                  std::string_view fileName, int line, DiagnosticSink& sink,
                                  std::vector<Token>& fnToks, bool& isNone);
bool isCalcFunctionValue(const std::vector<Token>& tokens);

namespace xts_values_detail {

using css_values::Cursor;
using css_values::kMaxFiniteIterations;
using css_values::kStepsClampedMessage;
using css_values::Tok;
using css_values::TokKind;

constexpr css_values::Dialect kCompiler = css_values::Dialect::Compiler;

/// The lexer's tokens as css_values tokens (views into `toks`, which must outlive the result).
std::vector<Tok> toValueTokens(const std::vector<Token>& toks);
/// A css_values token back as a lexer token (its text copied).
Token toLexerToken(const Tok& t);

/// The compiler's side of css_values::Diag: one declaration's file and line on `sink`.
class SinkDiag final : public css_values::Diag {
public:
    SinkDiag(DiagnosticSink& sink, std::string_view fileName, int line)
        : m_sink(sink), m_fileName(fileName), m_line(line) {}
    void warn(std::string_view message) override {
        m_sink.warn(std::string(m_fileName), std::string(message), m_line);
    }
    void error(std::string_view message) override {
        m_sink.error(std::string(m_fileName), std::string(message), m_line);
    }

private:
    DiagnosticSink& m_sink;
    std::string_view m_fileName;
    int m_line;
};

// The css_values grammar in the compiler's dialect.
inline bool parseLengthTok(const Tok& tok, Length& out) {
    return css_values::parseLengthTok(tok, kCompiler, out);
}
using css_values::isSlash;
using css_values::parseAngleTok;
using css_values::parseColorCursor;
using css_values::parseFloatTok;
using css_values::parseTimeTok;
inline bool parseColorTokens(const std::vector<Tok>& toks, Color& out) {
    return css_values::parseColorTokens(toks.data(), toks.size(), out);
}
inline bool isEasingStart(const Cursor& cur) { return css_values::isEasingStart(cur, kCompiler); }
inline bool parseEasingCursor(Cursor& cur, Easing& out, bool* stepsClamped = nullptr,
                              std::vector<EasingPoint>* easingPoints = nullptr) {
    return css_values::parseEasingCursor(cur, kCompiler, out, stepsClamped, easingPoints);
}
inline std::vector<std::vector<Tok>> splitByCommaTopLevel(const std::vector<Tok>& toks) {
    return css_values::splitByCommaTopLevel(toks.data(), toks.size());
}

bool parseAndExpandDecl(const PropInfo* info, const std::vector<Tok>& tokens, std::string_view fileName,
                        int line, DiagnosticSink& sink, std::vector<StyleDecl>& outDecls,
                        std::vector<EasingPoint>* easingPoints);
bool containsVarFunction(const std::vector<Tok>& tokens);
bool isCalcFunctionValue(const std::vector<Tok>& tokens);
bool parseCalcValue(const std::vector<Tok>& tokens, std::vector<CalcOpDecl>& out, std::string& error);
bool parseFontFamilyList(const std::vector<Tok>& toks, std::vector<std::string>& out);
StyleDecl enumDecl(uint16_t prop, uint32_t e, int line);
bool parseBorderImageSlice(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls);
bool parseUiScale(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls);
bool parseHitSlop(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls);
bool parseBorderImageShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& decls);
bool parseBackgroundImage(const std::vector<Tok>& toks, std::string& out);
bool parseTransitionList(const std::vector<Tok>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<TransitionSpec>& out,
                         std::vector<EasingPoint>* easingPoints);
bool parseAnimationList(const std::vector<Tok>& toks, std::string_view fileName, int line,
                        DiagnosticSink& sink, std::vector<AnimationSpec>& out,
                        std::vector<EasingPoint>* easingPoints);
bool parseTextEffectList(const std::vector<Tok>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<EffectDeclEntry>& out);
bool parseEffectList(const std::vector<Tok>& toks, std::string_view fileName, int line, DiagnosticSink& sink,
                     std::vector<EffectDeclEntry>& out);
bool parseClipPathValue(const std::vector<Tok>& toks, ElementClipShape& out, bool& unsupported);
bool parseGradientValue(const std::vector<Tok>& toks, GradientDecl& out);
bool parseTransformShorthand(const std::vector<Tok>& toks, std::string_view fileName, int line,
                             DiagnosticSink& sink, std::vector<StyleDecl>& out);
bool parseIndependentTransform(std::string_view name, const std::vector<Tok>& toks, std::string_view fileName,
                               int line, DiagnosticSink& sink, std::vector<StyleDecl>& out);
bool parseTransformOrigin(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line);
bool parsePerspectiveOrigin(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line);
bool parseBackgroundPosition(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line);
bool parseBackgroundSize(const std::vector<Tok>& toks, std::vector<StyleDecl>& out, int line);
bool expand1to4(const PropId order[4], const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseBorderShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseBorderColorSides(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseBorderStyleShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseOutlineShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseTextDecorationLine(const std::vector<Tok>& toks, uint32_t& out);
using css_values::isDecorationThickness;
bool parseTextDecorationShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseFlexShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseGapShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseParticlePairShorthand(const std::vector<Tok>& toks, xt::PropId first, xt::PropId second, int line,
                                std::vector<StyleDecl>& out);
bool parseParticleColorShorthand(const std::vector<Tok>& toks, int line, std::vector<StyleDecl>& out);
bool parseAspectRatio(const std::vector<Tok>& toks, float& out);

/// css_values::PropValue results (`transform`, origins) as StyleDecls of `line`.
void appendPropValues(const std::vector<css_values::PropValue>& values, int line,
                      std::vector<StyleDecl>& out);

/// Why a grid value was rejected, for the warning: Unsupported names a CSS grid feature outside
/// the subset (`minmax()`, `auto-fill`, named lines, ...), NotRectangle a `grid-template-areas`
/// area that is not a rectangle; Invalid is any other malformed value.
enum class GridReject : uint8_t { Invalid, Unsupported, NotRectangle };

bool parseGridTrackList(const std::vector<Tok>& toks, std::vector<GridTrack>& out, GridReject& why);
bool parseGridAreasValue(const std::vector<Tok>& toks, StyleValue& out, GridReject& why);
bool parseGridLine(const std::vector<Tok>& toks, size_t begin, size_t end, uint32_t& out, GridReject& why);
bool parseGridLineShorthand(const std::vector<Tok>& toks, uint16_t startProp, int line,
                            std::vector<StyleDecl>& out, GridReject& why);

}  // namespace xts_values_detail

}  // namespace xt::compile
