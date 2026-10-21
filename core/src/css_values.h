#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/ir_pod.h"    // IrEasing.
#include "xt/node.h"      // EffectInstance, ElementGradient, ElementClipShape.
#include "xt/timeline.h"  // Easing, EasingPoint.
#include "xt/types.h"
#include "xt/value.h"

/// The CSS value grammar, once (T-413, docs/tasks/T-413.md): lengths, numbers, angles, times,
/// colours, easings, `transform` and the independent transform properties, origins, background
/// size, the outline / text-decoration / border-style / border-color shorthands, `effect`,
/// `background-gradient`, `clip-path`, `animation` items, font-family lists, `ui-scale`,
/// `hit-slop` and grid lines. Used by the runtime parser (core/src/mini_css.cpp, ElementRef::setStyle)
/// and by the compiler's `.xts` value parser (compile/src/xts/xts_values*.cpp, which includes this
/// private core/src header the way it already includes css_color.h and transform_compose.h).
///
/// The callers only add what is their own: the runtime its property table and the Decl it fills,
/// the compiler its diagnostics (through Diag), its property table and the `.xts`-only syntax
/// (calc(), var(), grid templates, border-image, transition, the margin/padding/... shorthands).
/// Where 05 lets the runtime accept less than the compiler, the difference is a Dialect argument.

namespace xt::css_values {

/// One value token. The kinds are the compiler lexer's (compile/src/xts/xts_lexer.h, same order)
/// plus Slash, which only the runtime tokenizer produces; the compiler's lexer has no `/` token and
/// hands it over as an Unknown "unexpected character '/'" (see isSlash()).
enum class TokKind : uint8_t {
    End,
    Unknown,
    Ident,
    Number,
    String,
    Hash,
    LBrace,
    RBrace,
    Colon,
    Semicolon,
    Comma,
    Gt,
    Dot,
    Star,
    LParen,
    RParen,
    Bang,
    At,
    LBracket,
    RBracket,
    Equals,
    Caret,
    Dollar,
    Slash,
};

/// A token whose text views storage owned by the caller (the declaration text, or the compiler's
/// token vector) for as long as the token is used.
struct Tok {
    TokKind kind = TokKind::End;
    std::string_view text{};  ///< Ident / String contents / Hash digits / Number source / Unknown message.
    float num = 0;            ///< Number only.
    std::string_view unit{};  ///< Number only: "" unitless, "%", or the unit identifier.
};

/// The `/` of `hsl(h s l / a)`, `aspect-ratio`, `clip-path: inset(... / ...)`, `grid-column`.
bool isSlash(const Tok& t);

/// A read-only walk over one value's tokens.
class Cursor {
public:
    Cursor(const Tok* toks, size_t count) : m_toks(toks), m_count(count) {}
    explicit Cursor(const std::vector<Tok>& toks) : m_toks(toks.data()), m_count(toks.size()) {}
    const Tok& peek(size_t ahead = 0) const {
        const size_t i = m_idx + ahead;
        return i < m_count ? m_toks[i] : kEnd;
    }
    const Tok& advance() {
        const Tok& t = peek();
        if (m_idx < m_count) {
            ++m_idx;
        }
        return t;
    }
    bool atEnd() const { return m_idx >= m_count; }

private:
    static const Tok kEnd;
    const Tok* m_toks;
    size_t m_count;
    size_t m_idx = 0;
};

/// Which caller reads the value. The differences are the ones the design documents give the
/// runtime (05 §5.9.3: setStyle takes no T-176 units). Keyword case is not one of them: keywords
/// match ASCII case-insensitively for both (05 §5.3, ruling 2026-10-21; T-421).
enum class Dialect : uint8_t { Compiler, Runtime };

/// ASCII case-insensitive equality.
bool iequals(std::string_view a, std::string_view b);

/// `t` is the identifier `word`, ASCII case-insensitively (both dialects; T-421).
bool isKeyword(Dialect d, const Tok& t, std::string_view word);

/// Where a parser reports what the compiler words as a diagnostic. The runtime passes nullptr.
class Diag {
public:
    virtual void warn(std::string_view message) = 0;
    virtual void error(std::string_view message) = 0;

protected:
    virtual ~Diag() = default;
};

/// CSS's 1-4 value rule (margin order): `n` values as top, right, bottom, left.
template <typename T>
void expandSides(const T* v, size_t n, T out[4]) {
    out[0] = v[0];
    out[1] = n >= 2 ? v[1] : v[0];
    out[2] = n >= 3 ? v[2] : v[0];
    out[3] = n == 4 ? v[3] : out[1];
}

/// One declaration a multi-output value writes (`transform`, origins, shorthands).
struct PropValue {
    uint16_t prop = 0;
    Value value{};
};

// ---------------------------------------------------------------------------------------------
// Single tokens.
// ---------------------------------------------------------------------------------------------

/// px / % / em / rem / auto and unitless 0 (-> 0px); the compiler also the T-176 units (vw, vh,
/// vmin, vmax, safe-*, uiscale).
bool parseLengthTok(const Tok& tok, Dialect d, Length& out);
/// A unitless number.
bool parseFloatTok(const Tok& tok, float& out);
/// `<n>deg`.
bool parseAngleTok(const Tok& tok, float& deg);
/// `s` / `ms`, in seconds.
bool parseTimeTok(const Tok& tok, float& seconds);
/// T-328: `auto` (0) | `<w> / <h>` | `<number>`, both > 0.
bool parseAspectRatio(const Tok* toks, size_t count, float& out);
/// 05.4: a font-weight number, a multiple of 100 in 100..900.
bool parseFontWeightNumber(const Tok& tok, uint32_t& out);
/// T-331: a `line-clamp` count, an integer >= 1.
bool parseLineClampNumber(const Tok& tok, uint32_t& out);
/// T-336: an `order` value, an integer.
bool parseOrderInteger(const Tok* toks, size_t count, float& out);

// ---------------------------------------------------------------------------------------------
// Colours.
// ---------------------------------------------------------------------------------------------

/// `#rgb #rgba #rrggbb #rrggbbaa` (the digits without the `#`).
bool parseHashColor(std::string_view hex, Color& out);
/// One colour at the cursor: hex, rgb()/rgba(), hsl()/hsla(), `transparent`, a named colour.
bool parseColorCursor(Cursor& cur, Color& out);
/// A whole value that is exactly one colour.
bool parseColorTokens(const Tok* toks, size_t count, Color& out);
/// T-333: `border-color` with 2-4 colours as top, right, bottom, left (false for 1 colour).
bool parseBorderColorSides(const Tok* toks, size_t count, Color out[4]);

// ---------------------------------------------------------------------------------------------
// Easing and lists.
// ---------------------------------------------------------------------------------------------

/// The cursor is on the first token of an `<easing>`.
bool isEasingStart(const Cursor& cur, Dialect d);
/// `<easing>` (05.6, T-322, T-335). `steps(n)` clamps n into [1, 65535] and sets `*stepsClamped`
/// when it had to; `linear()` appends its points to `*points` (rejected without it).
bool parseEasingCursor(Cursor& cur, Dialect d, Easing& out, bool* stepsClamped = nullptr,
                       std::vector<EasingPoint>* points = nullptr);
/// An Easing as the IR stores it; a LinearPoints easing keeps its point offset / count (into the
/// caller's point pool).
IrEasing toIrEasing(const Easing& e);
/// The compiler's wording of the steps() clamp.
constexpr const char* kStepsClampedMessage = "steps() count clamped to [1, 65535]";
/// The tokens strictly inside the parenthesis group at the cursor (`cur` on the `(`); leaves the
/// cursor after the matching `)`.
bool extractParenGroup(Cursor& cur, std::vector<Tok>& inner);
/// Splits on commas at parenthesis depth 0.
std::vector<std::vector<Tok>> splitByCommaTopLevel(const Tok* toks, size_t count);
/// `font-family: A, "B C", sans serif` (an unquoted name may be several identifiers).
bool parseFontFamilyList(const Tok* toks, size_t count, std::vector<std::string>& out);

/// One `animation` item (05.6): `<name> <duration> [<easing>] [<delay>] [<count> | infinite]
/// [<direction>] [<fill-mode>] [<play-state>]`, the parts after the duration in any order.
/// The finite iteration count is truncated; the runtime saturates it at 65535, the compiler
/// clamps it to kMaxFiniteIterations with a warning (05.x, T-258b: the IR's 0xFFFF is
/// `infinite`). `diag` also gets kStepsClampedMessage.
struct AnimationItem {
    std::string_view name{};
    float duration = 0;
    float delay = 0;
    Easing easing{};
    bool infinite = false;
    uint16_t iterations = 1;  ///< When !infinite; 0 = the item does not play (T-272).
    uint8_t direction = 0;    ///< 0 normal, 1 reverse, 2 alternate, 3 alternate-reverse.
    uint8_t fill = 0;         ///< 0 none, 1 forwards, 2 backwards, 3 both.
    uint8_t playState = 0;    ///< 0 running, 1 paused.
};
/// T-258b (D11): the largest finite count the compiler writes; 0xFFFF means `infinite`.
constexpr uint16_t kMaxFiniteIterations = 0xFFFE;
enum class AnimationItemResult : uint8_t { Ok, BadName, BadValue };
AnimationItemResult parseAnimationItem(const Tok* toks, size_t count, Dialect d, Diag* diag,
                                       AnimationItem& out, std::vector<EasingPoint>* points);

// ---------------------------------------------------------------------------------------------
// transform (05.5, 19.10, T-120, T-322, T-329).
// ---------------------------------------------------------------------------------------------

/// translate*(0), rotateX(1), rotateY(2), rotate/rotateZ(3), skew*(4), scale*(5); -1 otherwise.
int transformCategory(std::string_view fn);
/// rotate3d / scale3d / matrix3d / perspective: rejected with their own message.
bool isUnsupportedTransformFn(std::string_view fn);
/// The ten component declarations of `transform: none`.
void pushIdentityTransform(std::vector<PropValue>& out);
/// A `transform` value: the fixed-order fast path, or compose/decompose for any other order.
bool parseTransform(const Tok* toks, size_t count, Diag* diag, std::vector<PropValue>& out);
/// The fast path alone (what one independent property goes through).
bool parseTransformFixedOrder(const Tok* toks, size_t count, Diag* diag, std::vector<PropValue>& out);
/// The source text of a transform value for the compiler's messages (`transform: rotate(10)...`).
std::string transformDeclText(const Tok* toks, size_t count);

enum class IndependentTransform : uint8_t { Translate, Rotate, Scale };
/// T-329: `translate` / `rotate` / `scale` as the equivalent `transform` function tokens (views
/// into `toks` and string literals); `none` gives no tokens and sets `isNone`.
bool independentTransformFnToks(IndependentTransform which, const Tok* toks, size_t count, Dialect d,
                                Diag* diag, std::vector<Tok>& fnToks, bool& isNone);
/// One independent property on its own; `none` writes that property's components at identity.
bool parseIndependentTransform(IndependentTransform which, const Tok* toks, size_t count, Dialect d,
                               Diag* diag, std::vector<PropValue>& out);
/// The PropIds `transform` and the independent transform properties write.
bool isTransformComponentProp(uint16_t prop);

// ---------------------------------------------------------------------------------------------
// Origins, background, shorthands.
// ---------------------------------------------------------------------------------------------

/// `transform-origin` / `perspective-origin` / `background-position`: 1-2 values, keywords or
/// lengths, a missing second value is 50%.
bool parseOriginPair(const Tok* toks, size_t count, Dialect d, Length& x, Length& y);
/// T-327: `background-size: auto | cover | contain | <len|%> [<len|%>]`.
bool parseBackgroundSize(const Tok* toks, size_t count, Dialect d, Length& x, Length& y);
/// T-333: `border-style` 1-4 keywords as top, right, bottom, left (kBorderStyle*).
bool parseBorderStyleSides(const Tok* toks, size_t count, Dialect d, uint32_t out[4]);

/// T-325: `outline: [<width>] [<style>] [<color>]` in any order.
struct OutlineValue {
    Length width{0, LengthUnit::Px};
    uint32_t style = 0;
    Color color{};
    bool haveColor = false;
};
bool parseOutline(const Tok* toks, size_t count, Dialect d, OutlineValue& out);

/// T-324: `text-decoration-line`: none | a set of underline / overline / line-through bits.
bool parseTextDecorationLine(const Tok* toks, size_t count, Dialect d, uint32_t& out);
/// T-324: `auto` or a non-negative px / em length.
bool isDecorationThickness(const Length& len);
/// T-324: `text-decoration: <line…> [<color>] [<thickness>]` in any order.
struct TextDecorationValue {
    uint32_t lines = 0;
    Length thickness{0, LengthUnit::Auto};
    Color color{};
    bool haveColor = false;
};
bool parseTextDecoration(const Tok* toks, size_t count, Dialect d, TextDecorationValue& out);

/// T-345: `ui-scale: auto | none | clamp(<min>, <max>)` as (mode, min, max).
bool parseUiScale(const Tok* toks, size_t count, Dialect d, float out[3]);
/// T-386: `hit-slop: <length>{1,4}` (px / % / em, >= 0) as top, right, bottom, left.
bool parseHitSlop(const Tok* toks, size_t count, Dialect d, Length out[4]);
/// T-338: one grid line, `auto | <integer >= 1> | span <integer >= 1>` (kGridLine*).
/// `unsupported` is set for the CSS forms outside the subset (named or non-positive lines).
bool parseGridLine(const Tok* toks, size_t count, uint32_t& out, bool& unsupported);

// ---------------------------------------------------------------------------------------------
// effect (18.3-18.4), background-gradient (05.5c), clip-path (05 §5.9.8).
// ---------------------------------------------------------------------------------------------

constexpr size_t kMaxEffects = 16;            ///< 18.2: 16 effects per node.
constexpr size_t kMaxGradientMaskStops = 16;  ///< 18.2: 16 stops per GradientMask.
/// Effect lengths store plain px; em is resolved at the initial font-size (see the T-51a report).
constexpr float kEffectEmPx = 16.0f;

/// Effect lengths (18.4): px / em (em -> px); `%` sets `percentError`.
bool readEffectLen(Cursor& cur, Dialect d, float& outPx, bool& percentError);
/// The next `fn(...)` of an `effect` sequence at the cursor: its name and the tokens inside the
/// parentheses (the cursor ends after the `)`).
enum class EffectCall : uint8_t { Ok, NotAFunction, Unterminated };
EffectCall nextEffectCall(Cursor& cur, std::string_view& fn, std::vector<Tok>& inner);
enum class EffectFnResult : uint8_t { Ok, Bad, Unknown };
/// One built-in effect function `fn(inner)` (shadow, glow, blur, color-adjust, gradient-mask,
/// shine, outline, backdrop-blur; not custom()). Gradient-mask stops go to `stops`. `diag` gets
/// the gradient-mask stop errors and shine()'s clamp warning; `percentError` is set for a `%`
/// length.
EffectFnResult parseEffectFn(std::string_view fn, const std::vector<Tok>& inner, Dialect d, Diag* diag,
                             bool& percentError, EffectInstance& out, std::vector<GradientStop>& stops);

/// `none | linear-gradient(...) | radial-gradient(...) | conic-gradient(...)` (05.5c, T-201).
bool parseGradientValue(const Tok* toks, size_t count, Dialect d, ElementGradient& out);

/// `none | inset() | circle() | ellipse() | polygon()` (T-332). `unsupported` is set for `path()`
/// and `url()`.
bool parseClipPathValue(const Tok* toks, size_t count, Dialect d, ElementClipShape& out, bool& unsupported);

}  // namespace xt::css_values
