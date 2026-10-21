#pragma once

// T-412: declarations shared by the xts_values*.cpp files (split from one file along its section
// comments; docs/tasks/T-412.md). Internal to xt_compile's style parser: only those files include it.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

#include "css_color.h"          // core/src: named colours and hsl() (T-322).
#include "transform_compose.h"  // core/src: 05.5's compose/decompose path (T-120).
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

// Declared in xts_parser.cpp; defined here.
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
bool parseCalcValue(const std::vector<Token>& tokens, std::vector<CalcOpDecl>& out, std::string& error);

namespace xts_values_detail {

class Cursor {
public:
    explicit Cursor(const std::vector<Token>& toks) : m_toks(&toks) {}
    const Token& peek(size_t ahead = 0) const {
        size_t i = m_idx + ahead;
        return i < m_toks->size() ? (*m_toks)[i] : kEndToken;
    }
    const Token& advance() {
        const Token& t = peek();
        if (m_idx < m_toks->size()) {
            ++m_idx;
        }
        return t;
    }
    bool atEnd() const { return m_idx >= m_toks->size(); }

private:
    static const Token kEndToken;
    const std::vector<Token>* m_toks;
    size_t m_idx = 0;
};
bool parseLengthTok(const Token& tok, Length& out);
bool parseFloatTok(const Token& tok, float& out);
bool isSlashTok(const Token& t);
bool parseColorCursor(Cursor& cur, Color& out);
bool parseColorTokens(const std::vector<Token>& toks, Color& out);
constexpr const char* kStepsClampedMessage = "steps() count clamped to [1, 65535]";
/// T-258b (D11): the largest finite `animation` iteration count; 0xFFFF means `infinite`.
constexpr uint16_t kMaxFiniteIterations = 0xFFFE;

bool parseEasingCursor(Cursor& cur, Easing& out, bool* stepsClamped = nullptr,
                       std::vector<EasingPoint>* easingPoints = nullptr);
bool isEasingStart(const Cursor& cur);
bool parseFontFamilyList(const std::vector<Token>& toks, std::vector<std::string>& out);
StyleDecl enumDecl(uint16_t prop, uint32_t e, int line);
bool parseBorderImageSlice(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls);
bool parseUiScale(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls);
bool parseHitSlop(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls);
bool parseBorderImageShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& decls);
bool parseBackgroundImage(const std::vector<Token>& toks, std::string& out);
std::vector<std::vector<Token>> splitByCommaTopLevel(const std::vector<Token>& toks);
bool parseTimeTok(const Token& tok, float& seconds);
bool parseTransitionList(const std::vector<Token>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<TransitionSpec>& out,
                         std::vector<EasingPoint>* easingPoints);
bool parseAnimationList(const std::vector<Token>& toks, std::string_view fileName, int line,
                        DiagnosticSink& sink, std::vector<AnimationSpec>& out,
                        std::vector<EasingPoint>* easingPoints);
bool extractParenGroup(Cursor& cur, std::vector<Token>& inner);
bool parseTextEffectList(const std::vector<Token>& toks, std::string_view fileName, int line,
                         DiagnosticSink& sink, std::vector<EffectDeclEntry>& out);
bool parseEffectList(const std::vector<Token>& toks, std::string_view fileName, int line,
                     DiagnosticSink& sink, std::vector<EffectDeclEntry>& out);
bool parseClipPathValue(const std::vector<Token>& toks, ElementClipShape& out, bool& unsupported);
bool parseGradientValue(const std::vector<Token>& toks, GradientDecl& out);
bool parseAngleTok(const Token& tok, float& deg);
bool parseTransformShorthand(const std::vector<Token>& toks, std::string_view fileName, int line,
                             DiagnosticSink& sink, std::vector<StyleDecl>& out);
bool parseIndependentTransform(std::string_view name, const std::vector<Token>& toks,
                               std::string_view fileName, int line, DiagnosticSink& sink,
                               std::vector<StyleDecl>& out);
bool parseTransformOrigin(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line);
bool parsePerspectiveOrigin(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line);
bool parseBackgroundPosition(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line);
bool parseBackgroundSize(const std::vector<Token>& toks, std::vector<StyleDecl>& out, int line);
bool expand1to4(const PropId order[4], const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseBorderShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseBorderColorSides(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseBorderStyleShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseOutlineShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseTextDecorationLine(const std::vector<Token>& toks, uint32_t& out);
bool isDecorationThickness(const Length& len);
bool parseTextDecorationShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseFlexShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseGapShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseParticlePairShorthand(const std::vector<Token>& toks, xt::PropId first, xt::PropId second, int line,
                                std::vector<StyleDecl>& out);
bool parseParticleColorShorthand(const std::vector<Token>& toks, int line, std::vector<StyleDecl>& out);
bool parseAspectRatio(const std::vector<Token>& toks, float& out);

/// Why a grid value was rejected, for the warning: Unsupported names a CSS grid feature outside
/// the subset (`minmax()`, `auto-fill`, named lines, ...), NotRectangle a `grid-template-areas`
/// area that is not a rectangle; Invalid is any other malformed value.
enum class GridReject : uint8_t { Invalid, Unsupported, NotRectangle };

bool parseGridTrackList(const std::vector<Token>& toks, std::vector<GridTrack>& out, GridReject& why);
bool parseGridAreasValue(const std::vector<Token>& toks, StyleValue& out, GridReject& why);
bool parseGridLine(const std::vector<Token>& toks, size_t begin, size_t end, uint32_t& out, GridReject& why);
bool parseGridLineShorthand(const std::vector<Token>& toks, uint16_t startProp, int line,
                            std::vector<StyleDecl>& out, GridReject& why);

}  // namespace xts_values_detail

}  // namespace xt::compile
