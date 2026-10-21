#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/aliases.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/node.h"  // ElementState bits (kStateHover etc.), reused for SelectorPart::stateMask.
#include "xts_lexer.h"
#include "xts_props.h"

/// Rule / at-rule grammar. See docs/design/05-style-and-css.md §5.3, docs/tasks/T-22.md.

namespace xt::compile {

// Implemented in xts_values.cpp: parses one declaration's value tokens, expanding shorthands into
// possibly several StyleDecls. `info` must be non-null (the caller already resolved the name via
// findProp). Returns false if the value failed to parse (a warning/error was already reported
// into `sink`; nothing is appended to `outDecls` in that case).
bool parseAndExpandDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName,
                        int line, DiagnosticSink& sink, std::vector<StyleDecl>& outDecls,
                        std::vector<EasingPoint>* easingPoints = nullptr);

// Implemented in xts_values.cpp: parses an `animation-timing-function` value (used only inside
// @keyframes stops). Returns false on failure (no diagnostic emitted; caller decides). T-258b:
// `stepsClamped` (optional) is set when a `steps(n)` count was clamped to [1, 65535].
bool parseEasingValue(const std::vector<Token>& tokens, Easing& out, bool* stepsClamped = nullptr,
                      std::vector<EasingPoint>* easingPoints = nullptr);

// Implemented in xts_values.cpp (T-172): an `@timeline` time point (s/ms, bare 0) and an
// `effect.<effect>.<param>` value. Both return false on failure without reporting.
bool parseTimelineTime(const Token& tok, float& seconds);
bool parseEffectParamValue(const std::vector<Token>& tokens, bool isColor, StyleValue& out);
// Implemented in xts_values.cpp (T-170, 05.9.2): whether a value holds `var(`; the tokens
// re-serialized as `.xts` text; and a declaration whose value holds `var()` -- kept as a VarRef
// when it is a plain longhand's whole value, else deferred as a VarSubst.
bool containsVarFunction(const std::vector<Token>& tokens);
bool isCalcFunctionValue(const std::vector<Token>& tokens);
std::string tokensToText(const std::vector<Token>& tokens);
// Implemented in xts_values.cpp (T-329, 05 §5.9.8): `translate`/`rotate`/`scale` as the
// equivalent `transform` function tokens; `none` gives an empty list and sets isNone.
bool independentTransformFnTokens(std::string_view name, const std::vector<Token>& toks,
                                  std::string_view fileName, int line, DiagnosticSink& sink,
                                  std::vector<Token>& fnToks, bool& isNone);
bool parseVarDecl(const PropInfo* info, const std::vector<Token>& tokens, std::string_view fileName, int line,
                  DiagnosticSink& sink, std::vector<StyleDecl>& outDecls);

namespace {

// -------------------------------------------------------------------------------------------
// T-322: `transition-property/-duration/-timing-function/-delay`, merged per declaration block
// into the `transition` shorthand's representation (05.6).
// -------------------------------------------------------------------------------------------

/// The four longhands, in the order TransitionLists below uses.
constexpr const char* kTransitionLonghands[4] = {"transition-property", "transition-duration",
                                                 "transition-timing-function", "transition-delay"};

/// Index of a `transition-*` longhand in kTransitionLonghands, or -1.
int transitionLonghandIndex(std::string_view name) {
    for (int i = 0; i < 4; ++i) {
        if (name == kTransitionLonghands[i]) {
            return i;
        }
    }
    return -1;
}

/// One comma-separated list per longhand; an empty item means "not given" (the shorthand's own
/// default applies when it is re-parsed).
using TransitionLists = std::vector<std::vector<Token>>[4];

/// Splits `toks` on top-level commas (a comma inside `cubic-bezier(...)` does not split).
std::vector<std::vector<Token>> splitTopLevelCommas(const std::vector<Token>& toks) {
    std::vector<std::vector<Token>> items(1);
    int depth = 0;
    for (const Token& t : toks) {
        if (t.kind == TokenKind::LParen) {
            ++depth;
        } else if (t.kind == TokenKind::RParen) {
            --depth;
        }
        if (t.kind == TokenKind::Comma && depth == 0) {
            items.emplace_back();
        } else {
            items.back().push_back(t);
        }
    }
    return items;
}

/// Splits a `transition` shorthand value (`<prop> <duration> [<easing>] [<delay>]`, comma
/// separated; the grammar xts_values.cpp's parseTransitionItem() reads) into the four longhand
/// lists. False when an item does not have that shape.
bool splitTransitionShorthand(const std::vector<Token>& toks, TransitionLists& out) {
    for (auto& list : out) {
        list.clear();
    }
    for (const std::vector<Token>& item : splitTopLevelCommas(toks)) {
        if (item.size() < 2 || item[0].kind != TokenKind::Ident || item[1].kind != TokenKind::Number) {
            return false;
        }
        std::vector<Token> easing;
        std::vector<Token> delay;
        size_t i = 2;
        while (i < item.size()) {
            if (item[i].kind == TokenKind::Ident && easing.empty()) {
                easing.push_back(item[i++]);
                if (i < item.size() && item[i].kind == TokenKind::LParen) {
                    while (i < item.size() && item[i].kind != TokenKind::RParen) {
                        easing.push_back(item[i++]);
                    }
                    if (i >= item.size()) {
                        return false;
                    }
                    easing.push_back(item[i++]);  // ')'
                }
            } else if (item[i].kind == TokenKind::Number && delay.empty()) {
                delay.push_back(item[i++]);
            } else {
                return false;
            }
        }
        out[0].push_back(std::vector<Token>{item[0]});
        out[1].push_back(std::vector<Token>{item[1]});
        out[2].push_back(std::move(easing));
        out[3].push_back(std::move(delay));
    }
    return true;
}

/// T-329 (05 §5.9.8): 0 for `transform`, 1/2/3 for `translate`/`rotate`/`scale`, else -1. The
/// order is CSS Transforms 2's: translate * rotate * scale * transform.
int transformPartKind(std::string_view name) {
    if (name == "transform") {
        return 0;
    }
    if (name == "translate") {
        return 1;
    }
    if (name == "rotate") {
        return 2;
    }
    if (name == "scale") {
        return 3;
    }
    return -1;
}

/// The PropIds `transform` and the independent transform properties write.
bool isTransformComponentProp(uint16_t prop) {
    return prop == xt::prop::TranslateX || prop == xt::prop::TranslateY || prop == xt::prop::TranslateZ ||
           prop == xt::prop::RotationX || prop == xt::prop::RotationY || prop == xt::prop::Rotation ||
           prop == xt::prop::SkewX || prop == xt::prop::SkewY || prop == xt::prop::ScaleX ||
           prop == xt::prop::ScaleY;
}

bool pseudoStateBit(std::string_view name, uint16_t& bit) {
    if (name == "hover") {
        bit = kStateHover;
    } else if (name == "active") {
        bit = kStateActive;
    } else if (name == "focus") {
        bit = kStateFocus;
    } else if (name == "disabled") {
        bit = kStateDisabled;
    } else if (name == "checked") {
        bit = kStateChecked;
    } else {
        return false;
    }
    return true;
}

// T-173: `an+b`, `odd`, `even` (whitespace already removed). Returns false when malformed.
bool parseInteger(std::string_view text, bool allowSign, int32_t& out) {
    size_t i = 0;
    bool negative = false;
    if (allowSign && i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i == text.size() || text.size() - i > 6) {
        return false;
    }
    int32_t value = 0;
    for (; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
        value = value * 10 + (text[i] - '0');
    }
    out = negative ? -value : value;
    return true;
}

bool parseAnPlusB(std::string text, int32_t& a, int32_t& b) {
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (text == "odd") {
        a = 2;
        b = 1;
        return true;
    }
    if (text == "even") {
        a = 2;
        b = 0;
        return true;
    }
    size_t n = text.find('n');
    if (n == std::string::npos) {
        a = 0;
        return parseInteger(text, true, b);
    }
    std::string_view head = std::string_view(text).substr(0, n);
    std::string_view tail = std::string_view(text).substr(n + 1);
    if (head.empty() || head == "+") {
        a = 1;
    } else if (head == "-") {
        a = -1;
    } else if (!parseInteger(head, true, a)) {
        return false;
    }
    if (tail.empty()) {
        b = 0;
        return true;
    }
    if (tail[0] != '+' && tail[0] != '-') {
        return false;
    }
    return parseInteger(tail, true, b);
}

// One simple selector's packed specificity (T-173's `:not()` arguments).
uint32_t condSpecificity(const SelectorCond& cond) {
    switch (cond.kind) {
        case SelectorCondKind::Tag:
            return 1;
        case SelectorCondKind::Id:
            return 1u << 16;
        case SelectorCondKind::Any:
            return 0;
        default:
            return 1u << 8;  // Class, state, attribute and the other pseudo-classes.
    }
}

}  // namespace

uint32_t selectorSpecificity(const Selector& sel) {
    uint32_t ids = 0, classes = 0, tags = 0;
    uint32_t packedNot = 0;
    for (const SelectorPart& part : sel.parts) {
        if (!part.id.empty()) {
            ++ids;
        }
        classes += static_cast<uint32_t>(part.classes.size());
        // Each set pseudo-class bit counts as a class for specificity (05.3).
        for (uint16_t bit = 1; bit != 0; bit = static_cast<uint16_t>(bit << 1)) {
            if (part.stateMask & bit) {
                ++classes;
            }
        }
        if (!part.tag.empty()) {
            ++tags;
        }
        // T-173: attribute selectors and pseudo-classes count as classes; a `:not()` counts as
        // its most specific argument (its arguments are consecutive and share a notGroup).
        uint16_t group = 0;
        uint32_t groupMax = 0;
        for (const SelectorCond& cond : part.conds) {
            if (!cond.negated) {
                ++classes;
                continue;
            }
            if (cond.notGroup != group) {
                packedNot += groupMax;
                group = cond.notGroup;
                groupMax = 0;
            }
            groupMax = std::max(groupMax, condSpecificity(cond));
        }
        packedNot += groupMax;
        // T-218: `#Icon/Blade` weighs like `#Icon #Blade`, `#Icon/path` like `#Icon path`.
        if (part.sceneKind == SceneTargetKind::Name) {
            ++ids;
        } else if (part.sceneKind == SceneTargetKind::Tag) {
            ++tags;
        }
    }
    return ((ids << 16) | (classes << 8) | tags) + packedNot;
}

namespace {
/// T-174: joins a trigger declaration's value tokens back into text for trigger_emit.cpp's own
/// parser. Characters the stylesheet lexer does not know (`=` in `from=<marker>`, `/` in a path
/// id) come back as Unknown tokens and are restored verbatim; any other Unknown token (an
/// unterminated string) makes this return false.
bool triggerTokensToText(const std::vector<Token>& tokens, std::string& out) {
    static constexpr std::string_view kUnexpected = "unexpected character '";
    out.clear();
    for (const Token& t : tokens) {
        std::string piece;
        if (t.kind == TokenKind::Unknown) {
            if (t.text.size() != kUnexpected.size() + 2 ||
                t.text.compare(0, kUnexpected.size(), kUnexpected) != 0) {
                return false;
            }
            piece.assign(1, t.text[kUnexpected.size()]);
        } else if (t.kind == TokenKind::String) {
            piece = '"' + t.text + '"';
        } else if (t.kind == TokenKind::Equals) {
            piece = "=";  // `from=<marker>` (T-173 made `=` a token of its own).
        } else {
            piece = tokensToText(std::vector<Token>{t});
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += piece;
    }
    return true;
}

class Parser {
public:
    Parser(std::string_view text, std::string_view fileName, int lineOffset, DiagnosticSink& sink)
        : m_lexer(text), m_fileName(fileName), m_lineOffset(lineOffset), m_sink(sink) {
        advance();
    }

    /// T-335: where parseDeclarationList() puts `linear()` easing points (null: rejected).
    void setEasingPoints(std::vector<EasingPoint>* points) { m_easingPoints = points; }

    bool parseStylesheet(RuleSet& out) {
        m_tokensOut = &out.tokens;
        m_easingPoints = &out.easingPoints;  // T-335.
        while (m_cur.kind != TokenKind::End) {
            if (m_cur.kind == TokenKind::At) {
                parseAtRule(out);
            } else if (atSymbolBlock()) {
                parseSymbolBlock(out);
            } else {
                parseRule(out);
            }
        }
        return !m_hadError;
    }

    bool parseDeclarationList(std::vector<StyleDecl>& out) {
        bool sawEnd = false;
        parseDecls(out, /*insideBraces=*/false, sawEnd);
        return !m_hadError;
    }

    // T-172: parses the whole input as the body of one `@timeline` key (a declaration list, no
    // braces) into `key` -- compile_document.cpp feeds `.xtanim.json` keys through the same rules.
    bool parseTimelineKeyBody(TimelineKey& key) {
        bool sawEnd = false;
        parseTimelineKeyDecls(key, /*insideBraces=*/false, sawEnd);
        return !m_hadError;
    }

private:
    Token m_cur;
    Lexer m_lexer;
    std::string m_fileName;
    int m_lineOffset;
    DiagnosticSink& m_sink;
    bool m_hadError = false;
    // T-170: where `--name: value` declarations go (parseXts only), whether parseDecls is inside a
    // rule body right now, and the `:root` bookkeeping of parseCompound/parseSelectorList.
    std::vector<TokenDecl>* m_tokensOut = nullptr;
    // T-335: where `linear()` easing points go (null: `linear()` is rejected).
    std::vector<EasingPoint>* m_easingPoints = nullptr;
    // T-174: where the current rule's `on-<event>` / `state-enter|exit` declarations go (null
    // outside a stylesheet rule, e.g. in a `style` attribute).
    std::vector<TriggerDecl>* m_triggersOut = nullptr;
    bool m_inRule = false;
    bool m_sawRoot = false;
    bool m_listHadRoot = false;
    uint16_t m_notGroups = 0;  // T-173: `:not()` groups numbered so far in the current selector.
    // T-176: inside a `symbol()` block / an `@media` block right now.
    bool m_inSymbol = false;
    bool m_inMedia = false;
    // T-322: the `transition` / `transition-*` declarations of the block parseDecls() is reading,
    // in source order. `declIndex` is decls.size() when the declaration was read; `emitted` is how
    // many StyleDecls a shorthand produced there (longhands produce none until the merge).
    struct TransitionPart {
        int longhand = -1;  ///< transitionLonghandIndex(), or -1 for the shorthand.
        std::vector<Token> value;
        Token nameTok;
        bool important = false;
        size_t declIndex = 0;
        size_t emitted = 0;
    };
    std::vector<TransitionPart>* m_transitionParts = nullptr;
    // T-329: the successfully parsed `transform` / `translate` / `rotate` / `scale` declarations of
    // the block being read, in source order (see mergeIndependentTransforms()).
    struct TransformPart {
        int kind = 0;  ///< transformPartKind().
        std::vector<Token> value;
        Token nameTok;
        bool important = false;
    };
    std::vector<TransformPart>* m_transformParts = nullptr;
    // T-218: some selector of the current rule may address inline `<svg>` content (`#Icon/*`, or a
    // path id `#Icon/Blade` that resolveSceneTargets() may turn into a scene target once the DOM
    // is known), so vector properties are kept for that pass to sort out.
    bool m_sceneRule = false;

    void advance() { m_cur = m_lexer.next(); }
    int reportLine(int lexLine) const { return m_lineOffset + (lexLine - 1); }

    void error(const Token& tok, const std::string& msg) {
        m_sink.error(m_fileName, msg, reportLine(tok.line), tok.col);
        m_hadError = true;
    }
    void warn(const Token& tok, const std::string& msg) {
        m_sink.warn(m_fileName, msg, reportLine(tok.line), tok.col);
    }

    // xts_values.cpp's parseAndExpandDecl() reports its own diagnostics (including transform's
    // 05.5 ordering error, which is error-level, not warning-level) straight into m_sink; sync
    // m_hadError afterwards so parseXts()'s return value reflects it too.
    void syncErrorFlag(size_t sinkSizeBefore) {
        const std::vector<Diagnostic>& all = m_sink.all();
        for (size_t i = sinkSizeBefore; i < all.size(); ++i) {
            if (all[i].level == Diagnostic::Level::Error) {
                m_hadError = true;
            }
        }
    }

    // Skips tokens (no brace-nesting awareness -- the .xts grammar has no nested `{}` other than
    // at-rule bodies, which are handled separately) until the next '}' or end of input, consuming
    // the '}' if found. See docs/tasks/T-22.md "缺 `:` 或 `}` -> error ... 跳到下一個 `}` 繼續".
    void recoverToNextRBrace() {
        while (m_cur.kind != TokenKind::RBrace && m_cur.kind != TokenKind::End) {
            advance();
        }
        if (m_cur.kind == TokenKind::RBrace) {
            advance();
        }
    }

    // Skips a `{ ... }` block, tracking nesting depth (used for unknown at-rules, which may
    // contain nested blocks, e.g. a stray rule inside an unrecognized at-rule).
    void skipBracedBlock() {
        if (m_cur.kind != TokenKind::LBrace) {
            return;
        }
        int depth = 0;
        do {
            if (m_cur.kind == TokenKind::LBrace) {
                ++depth;
            } else if (m_cur.kind == TokenKind::RBrace) {
                --depth;
            } else if (m_cur.kind == TokenKind::End) {
                break;
            }
            advance();
        } while (depth > 0);
    }

    // Reads declarations up to (and consuming) a '}' when insideBraces, or up to End otherwise.
    // On a structural error (missing ':', unexpected token, unterminated block), reports and
    // recovers per recoverToNextRBrace(), then returns with sawEnd left false (caller should treat
    // the whole enclosing rule as errored, per spec).
    void parseDecls(std::vector<StyleDecl>& decls, bool insideBraces, bool& sawEnd) {
        std::vector<TransitionPart> parts;
        std::vector<TransitionPart>* saved = m_transitionParts;
        m_transitionParts = &parts;
        std::vector<TransformPart> xforms;
        std::vector<TransformPart>* savedXforms = m_transformParts;
        m_transformParts = &xforms;
        parseDeclsBody(decls, insideBraces, sawEnd);
        m_transitionParts = saved;
        m_transformParts = savedXforms;
        mergeTransitionLonghands(parts, decls);
        mergeIndependentTransforms(xforms, decls);
    }

    // T-329: remembers a parsed transform-family declaration for mergeIndependentTransforms().
    void recordTransformPart(const Token& nameTok, const std::vector<Token>& valueToks, bool important,
                             bool parsed) {
        const int kind = transformPartKind(nameTok.text);
        if (m_transformParts != nullptr && parsed && kind >= 0) {
            m_transformParts->push_back(TransformPart{kind, valueToks, nameTok, important});
        }
    }

    // T-329 (05 §5.9.8): when a block holds `transform` and at least one of `translate` /
    // `rotate` / `scale`, the final matrix is translate * rotate * scale * transform (CSS
    // Transforms 2 §6) around the same `transform-origin`. The last declaration of each wins; the
    // four are re-parsed as one `transform` sequence (05.5's fast path when the result keeps the
    // fixed order, otherwise compose/decompose), and every StyleDecl they produced is replaced by
    // that one expansion. Alone, an independent property keeps its straight-through PropIds.
    void mergeIndependentTransforms(const std::vector<TransformPart>& parts, std::vector<StyleDecl>& decls) {
        const TransformPart* last[4] = {nullptr, nullptr, nullptr, nullptr};
        for (const TransformPart& part : parts) {
            last[part.kind] = &part;
        }
        if (last[0] == nullptr || (last[1] == nullptr && last[2] == nullptr && last[3] == nullptr)) {
            return;
        }
        const TransformPart* latest = nullptr;
        for (const TransformPart* part : last) {
            if (part == nullptr) {
                continue;
            }
            if (containsVarFunction(part->value)) {
                warn(part->nameTok, "\"" + part->nameTok.text +
                                        "\" with var() is not combined with the other transform properties "
                                        "of this block; each applies on its own");
                return;
            }
            if (latest == nullptr || part > latest) {
                latest = part;  // `parts` is in source order, so a later element is a later declaration.
            }
        }
        std::vector<Token> combined;
        for (int k = 1; k < 4; ++k) {
            if (last[k] == nullptr) {
                continue;
            }
            std::vector<Token> fnToks;
            bool isNone = false;
            // Already parsed successfully once, so this reports nothing.
            independentTransformFnTokens(last[k]->nameTok.text, last[k]->value, m_fileName,
                                         reportLine(last[k]->nameTok.line), m_sink, fnToks, isNone);
            combined.insert(combined.end(), fnToks.begin(), fnToks.end());
        }
        const std::vector<Token>& xform = last[0]->value;
        const bool xformNone =
            xform.size() == 1 && xform[0].kind == TokenKind::Ident && xform[0].text == "none";
        if (!xformNone) {
            combined.insert(combined.end(), xform.begin(), xform.end());
        }
        if (combined.empty()) {
            Token none;
            none.kind = TokenKind::Ident;
            none.text = "none";
            combined.push_back(none);
        }
        std::vector<StyleDecl> merged;
        const size_t sinkSizeBefore = m_sink.all().size();
        const bool ok = parseAndExpandDecl(findProp("transform"), combined, m_fileName,
                                           reportLine(latest->nameTok.line), m_sink, merged, m_easingPoints);
        syncErrorFlag(sinkSizeBefore);
        if (!ok) {
            return;  // Diagnostic already emitted; the declarations stay as parsed one by one.
        }
        decls.erase(std::remove_if(decls.begin(), decls.end(),
                                   [](const StyleDecl& d) { return isTransformComponentProp(d.prop); }),
                    decls.end());
        for (StyleDecl& d : merged) {
            d.important = latest->important;
            decls.push_back(std::move(d));
        }
    }

    // T-322 (05.6): when a block holds `transition-*` longhands, the block's `transition`
    // declarations (shorthand and longhands, applied in source order, the later one winning) are
    // merged into one `transition` StyleDecl, placed where the last of them was. A block with only
    // the shorthand is left exactly as parseDeclsBody() produced it.
    void mergeTransitionLonghands(const std::vector<TransitionPart>& parts, std::vector<StyleDecl>& decls) {
        bool anyLonghand = false;
        for (const TransitionPart& part : parts) {
            anyLonghand = anyLonghand || part.longhand >= 0;
        }
        if (!anyLonghand) {
            return;
        }
        // CSS initial values: `all`, `0s`; the easing and delay stay "not given" (the shorthand's
        // own defaults).
        Token allTok;
        allTok.kind = TokenKind::Ident;
        allTok.text = "all";
        Token zeroTok;
        zeroTok.kind = TokenKind::Number;
        zeroTok.text = "0";
        zeroTok.unit = "s";
        TransitionLists lists;
        lists[0].push_back(std::vector<Token>{allTok});
        lists[1].push_back(std::vector<Token>{zeroTok});
        lists[2].emplace_back();
        lists[3].emplace_back();
        for (const TransitionPart& part : parts) {
            if (part.longhand < 0) {
                TransitionLists split;
                if (!splitTransitionShorthand(part.value, split)) {
                    warn(part.nameTok,
                         "\"transition\" value cannot be combined with transition-* longhands; longhands "
                         "ignored");
                    return;
                }
                for (int k = 0; k < 4; ++k) {
                    lists[k] = std::move(split[k]);
                }
            } else {
                lists[part.longhand] = splitTopLevelCommas(part.value);
            }
        }
        size_t insertAt = parts.back().declIndex;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            insertAt -= parts[i].emitted;
        }
        for (size_t i = parts.size(); i-- > 0;) {
            if (parts[i].emitted > 0) {
                decls.erase(
                    decls.begin() + static_cast<std::ptrdiff_t>(parts[i].declIndex),
                    decls.begin() + static_cast<std::ptrdiff_t>(parts[i].declIndex + parts[i].emitted));
            }
        }
        const TransitionPart& last = parts.back();
        const int line = reportLine(last.nameTok.line);
        std::vector<StyleDecl> merged;
        if (lists[0].size() == 1 && lists[0][0].size() == 1 && lists[0][0][0].kind == TokenKind::Ident &&
            lists[0][0][0].text == "none") {
            StyleDecl d;  // `transition-property: none`: no transitions.
            d.prop = styleprop::Transition;
            d.value.kind = StyleValueKind::TransitionList;
            d.line = line;
            merged.push_back(std::move(d));
        } else {
            std::vector<Token> toks;
            for (size_t i = 0; i < lists[0].size(); ++i) {
                if (i > 0) {
                    Token comma;
                    comma.kind = TokenKind::Comma;
                    toks.push_back(comma);
                }
                toks.insert(toks.end(), lists[0][i].begin(), lists[0][i].end());
                for (int k = 1; k < 4; ++k) {
                    const std::vector<Token>& item = lists[k][i % lists[k].size()];
                    if (k == 1 && item.empty()) {
                        toks.push_back(zeroTok);
                    } else {
                        toks.insert(toks.end(), item.begin(), item.end());
                    }
                }
            }
            const size_t sinkSizeBefore = m_sink.all().size();
            parseAndExpandDecl(findProp("transition"), toks, m_fileName, line, m_sink, merged,
                               m_easingPoints);
            syncErrorFlag(sinkSizeBefore);
        }
        for (StyleDecl& d : merged) {
            d.important = last.important;
        }
        decls.insert(decls.begin() + static_cast<std::ptrdiff_t>(insertAt), merged.begin(), merged.end());
    }

    void parseDeclsBody(std::vector<StyleDecl>& decls, bool insideBraces, bool& sawEnd) {
        for (;;) {
            if (insideBraces && m_cur.kind == TokenKind::RBrace) {
                advance();
                sawEnd = true;
                return;
            }
            if (m_cur.kind == TokenKind::End) {
                if (insideBraces) {
                    error(m_cur, "unterminated block, expected '}'");
                }
                sawEnd = true;
                return;
            }
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected property name");
                if (insideBraces) {
                    recoverToNextRBrace();
                }
                return;
            }
            Token nameTok = m_cur;
            advance();
            // T-174: `on-<event>` may carry `(<arg>)` and `.end`/`.marker(<m>)` parts before ':'.
            const bool isTrigger = nameTok.text.size() > 3 && nameTok.text.compare(0, 3, "on-") == 0;
            std::string triggerEvent = isTrigger ? nameTok.text.substr(3) : std::string();
            if (isTrigger) {
                while (m_cur.kind == TokenKind::LParen || m_cur.kind == TokenKind::RParen ||
                       m_cur.kind == TokenKind::Dot || m_cur.kind == TokenKind::Ident ||
                       m_cur.kind == TokenKind::Number) {
                    if (m_cur.kind == TokenKind::Number) {
                        triggerEvent += tokensToText(std::vector<Token>{m_cur});
                    } else if (m_cur.kind == TokenKind::Ident) {
                        triggerEvent += m_cur.text;
                    } else {
                        triggerEvent += m_cur.kind == TokenKind::LParen   ? '('
                                        : m_cur.kind == TokenKind::RParen ? ')'
                                                                          : '.';
                    }
                    advance();
                }
            }
            if (m_cur.kind != TokenKind::Colon) {
                error(nameTok, "expected ':' after property name \"" + nameTok.text + "\"");
                if (insideBraces) {
                    recoverToNextRBrace();
                }
                return;
            }
            advance();  // ':'

            std::vector<Token> valueToks;
            while (m_cur.kind != TokenKind::Semicolon && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::Bang && m_cur.kind != TokenKind::End) {
                valueToks.push_back(m_cur);
                advance();
            }
            bool important = false;
            if (m_cur.kind == TokenKind::Bang) {
                advance();
                if (m_cur.kind == TokenKind::Ident && m_cur.text == "important") {
                    important = true;
                    advance();
                } else {
                    warn(m_cur, "expected 'important' after '!'");
                }
            }
            if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            } else if (m_cur.kind == TokenKind::RBrace) {
                if (!insideBraces) {
                    error(nameTok, "unexpected '}'");
                    return;
                }
                // Leave it for the top of the loop to consume and set sawEnd.
            } else if (m_cur.kind == TokenKind::End) {
                if (insideBraces) {
                    error(nameTok, "unterminated block, expected '}'");
                    return;
                }
                // Bare declaration list (parseDeclarations) reaching End with no trailing ';' is fine;
                // the top of the loop will see End next and return normally.
            } else {
                error(nameTok, "expected ';' or '}' after declaration");
                if (insideBraces) {
                    recoverToNextRBrace();
                }
                return;
            }

            if (nameTok.text.size() > 2 && nameTok.text.compare(0, 2, "--") == 0) {
                // T-170 (05.9.2): a design token. Document-wide whatever the selector; no
                // element-level cascade, so a `style` attribute cannot declare one.
                if (m_inMedia) {
                    // T-176: tokens are document-wide; a conditional one would be misleading.
                    error(nameTok, "token \"" + nameTok.text + "\" cannot be declared inside @media");
                } else if (m_tokensOut == nullptr || !m_inRule) {
                    warn(nameTok,
                         "token \"" + nameTok.text + "\" can only be declared in a stylesheet rule; ignored");
                } else if (valueToks.empty()) {
                    warn(nameTok, "token \"" + nameTok.text + "\" has no value; ignored");
                } else {
                    m_tokensOut->push_back(TokenDecl{nameTok.text, tokensToText(valueToks), m_fileName,
                                                     reportLine(nameTok.line)});
                }
                continue;
            }
            if (isTrigger || nameTok.text == "state-enter" || nameTok.text == "state-exit") {
                addTriggerDecl(nameTok, std::move(triggerEvent), valueToks);
                continue;
            }
            // T-322: `transition` / `transition-*` are recorded for mergeTransitionLonghands(); the
            // shorthand is still parsed here as before.
            const int longhand = transitionLonghandIndex(nameTok.text);
            if ((longhand >= 0 || nameTok.text == "transition") && m_transitionParts != nullptr) {
                m_transitionParts->push_back(
                    TransitionPart{longhand, valueToks, nameTok, important, decls.size(), 0});
                if (longhand >= 0) {
                    continue;
                }
            }
            const PropInfo* info = findProp(nameTok.text);
            if (info == nullptr) {
                // T-408 (43 §43.6): a registered property alias parses as its replacement.
                if (const NameAlias* alias = findNameAlias(AliasKind::Property, nameTok.text)) {
                    m_sink.deprecated(m_fileName, deprecatedMessage(*alias), reportLine(nameTok.line),
                                      nameTok.col);
                    info = findProp(alias->replacement);
                }
            }
            if (info == nullptr) {
                warn(nameTok, "unknown property \"" + nameTok.text + "\"");
                continue;
            }
            if (isVectorPropInfo(*info) && !m_sceneRule) {
                warn(nameTok, "\"" + nameTok.text +
                                  "\" only applies to inline <svg> content in @timeline keys; ignored");
                continue;
            }
            if (m_sceneRule && (info->prop == xt::prop::FillColor || info->prop == xt::prop::StrokeColor) &&
                !valueToks.empty() && valueToks[0].kind == TokenKind::Ident && valueToks[0].text == "url") {
                // T-218 (31 §31.5.1): a paint server reference only works inside the <svg> itself.
                warn(nameTok, "\"" + nameTok.text + ": url()\" is not supported in style rules; ignored");
                continue;
            }
            std::vector<StyleDecl> expanded;
            size_t sinkSizeBefore = m_sink.all().size();
            // T-176: a whole `calc()` keeps its var() terms for runtime lookup.
            const bool parsed =
                containsVarFunction(valueToks) && !isCalcFunctionValue(valueToks)
                    ? parseVarDecl(info, valueToks, m_fileName, reportLine(nameTok.line), m_sink, expanded)
                    : parseAndExpandDecl(info, valueToks, m_fileName, reportLine(nameTok.line), m_sink,
                                         expanded, m_easingPoints);
            if (parsed) {
                for (StyleDecl& d : expanded) {
                    d.important = important;
                    decls.push_back(std::move(d));
                }
            }
            recordTransformPart(nameTok, valueToks, important, parsed);
            if (nameTok.text == "transition" && m_transitionParts != nullptr) {
                m_transitionParts->back().emitted = decls.size() - m_transitionParts->back().declIndex;
            }
            syncErrorFlag(sinkSizeBefore);
        }
    }

    // T-174: records one `on-<event>: <actions>` (event already without `on-`) or
    // `state-enter|state-exit: <Name> <actions>` declaration into the current rule.
    void addTriggerDecl(const Token& nameTok, std::string event, const std::vector<Token>& valueToks) {
        if (m_triggersOut == nullptr) {
            warn(nameTok,
                 "\"" + nameTok.text +
                     "\" can only be declared in a stylesheet rule (use on:<event> on the element); ignored");
            return;
        }
        std::vector<Token> actionToks = valueToks;
        if (event.empty()) {
            // state-enter / state-exit: the first token names the state.
            if (valueToks.empty() || valueToks[0].kind != TokenKind::Ident) {
                error(nameTok, "\"" + nameTok.text + "\" expects \"<State> <actions>\"");
                return;
            }
            event = "state(" + valueToks[0].text + ")." + (nameTok.text == "state-enter" ? "enter" : "exit");
            actionToks.erase(actionToks.begin());
        }
        std::string text;
        if (actionToks.size() == 1 && actionToks[0].kind == TokenKind::String) {
            text = actionToks[0].text;  // A quoted action list may use ';' between actions.
        } else if (!triggerTokensToText(actionToks, text)) {
            error(nameTok, "invalid action list for \"" + nameTok.text + "\"");
            return;
        }
        if (text.empty()) {
            error(nameTok, "\"" + nameTok.text + "\" has no actions");
            return;
        }
        m_triggersOut->push_back(TriggerDecl{std::move(event), std::move(text), reportLine(nameTok.line)});
    }

    void parseRule(RuleSet& out) {
        int ruleLine = reportLine(m_cur.line);
        std::vector<Selector> selectors;
        m_listHadRoot = false;
        if (!parseSelectorList(selectors)) {
            // Selector-level syntax error: recover the same way as a broken decl block would, by
            // skipping to the next '}' if we're positioned before one (best effort).
            if (m_cur.kind == TokenKind::LBrace) {
                advance();
            }
            recoverToNextRBrace();
            return;
        }
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after selector list");
            recoverToNextRBrace();
            return;
        }
        Token braceTok = m_cur;
        advance();  // '{'
        StyleRule rule;
        rule.selectors = std::move(selectors);
        rule.line = ruleLine;
        rule.file = m_fileName;
        bool sawEnd = false;
        m_inRule = true;
        m_triggersOut = &rule.triggers;
        m_sceneRule = false;
        for (const Selector& sel : rule.selectors) {
            m_sceneRule = m_sceneRule || sel.parts[0].sceneKind != SceneTargetKind::None ||
                          sel.parts[0].id.find('/') != std::string::npos;
        }
        parseDecls(rule.decls, /*insideBraces=*/true, sawEnd);
        m_sceneRule = false;
        m_triggersOut = nullptr;
        m_inRule = false;
        if (sawEnd && rule.selectors.empty()) {
            // T-170: a `:root`-only rule only declares tokens (collected by parseDecls).
            if (!rule.decls.empty() || !rule.triggers.empty()) {
                warn(braceTok,
                     "a \":root\" rule can only declare tokens (--name); other declarations ignored");
            }
            return;
        }
        if (!sawEnd) {
            // A structural error (missing ':' or unterminated block) invalidates the whole rule
            // (05.3's recovery rule); parseDecls already reported it and recovered. A per-decl
            // semantic error (e.g. an invalid value, or transform's 05.5 ordering error) doesn't
            // reach here: parseDecls sets sawEnd and keeps going, just dropping that one decl.
            return;
        }
        out.rules.push_back(std::move(rule));
    }

    // Parses `compound (combinator compound)*`. Returns false on a syntax/semantic error (no
    // selector-starting token, or an unknown pseudo-class).
    bool parseSelectorList(std::vector<Selector>& out) {
        for (;;) {
            Selector sel;
            m_sawRoot = false;
            if (!parseSelector(sel)) {
                return false;
            }
            if (m_sawRoot) {
                // T-170: `:root` is accepted for token declarations only, and only on its own.
                if (sel.parts.size() != 1 || !sel.parts[0].tag.empty() || !sel.parts[0].id.empty() ||
                    !sel.parts[0].classes.empty() || sel.parts[0].stateMask != 0 ||
                    !sel.parts[0].conds.empty()) {
                    error(m_cur, "\":root\" is only supported as a selector of its own");
                    return false;
                }
                m_listHadRoot = true;
            } else {
                sel.specificity = selectorSpecificity(sel);
                out.push_back(std::move(sel));
            }
            if (m_cur.kind == TokenKind::Comma) {
                advance();
                continue;
            }
            return true;
        }
    }

    // T-173: a token that can start a compound selector.
    static bool startsCompound(const Token& t) {
        return t.kind == TokenKind::Ident || t.kind == TokenKind::Star || t.kind == TokenKind::Dot ||
               t.kind == TokenKind::Hash || t.kind == TokenKind::Colon || t.kind == TokenKind::LBracket;
    }

    // `compound (combinator compound)*`. Whitespace between two compounds is the descendant
    // combinator (T-173); `>` (with or without whitespace around it) is the child combinator.
    bool parseSelector(Selector& out) {
        struct RawCompound {
            SelectorPart part;
            uint8_t combAfter = 0;  // Combinator following this compound (0 = none/last).
        };
        std::vector<RawCompound> raw;
        m_notGroups = 0;
        for (;;) {
            RawCompound rc;
            if (!parseCompound(rc.part)) {
                return false;
            }
            if (m_cur.kind == TokenKind::Gt) {
                advance();
                rc.combAfter = 1;  // child
                raw.push_back(std::move(rc));
                continue;
            }
            if (startsCompound(m_cur) && m_cur.wsBefore) {
                rc.combAfter = 0;  // descendant
                raw.push_back(std::move(rc));
                continue;
            }
            raw.push_back(std::move(rc));
            break;
        }
        out.parts.reserve(raw.size());
        for (size_t i = raw.size(); i-- > 0;) {
            SelectorPart part = std::move(raw[i].part);
            part.combinator = (i + 1 < raw.size()) ? raw[i].combAfter : 0;
            out.parts.push_back(std::move(part));
        }
        // T-218: `#Icon/*` only names the subject (the rightmost compound).
        for (size_t i = 1; i < out.parts.size(); ++i) {
            if (out.parts[i].sceneKind != SceneTargetKind::None) {
                error(m_cur, "an <svg> content target (#<svg id>/*) must be the selector's last compound");
                return false;
            }
        }
        return true;
    }

    // One compound: an optional tag or `*`, then `.class`, `#id`, `:pseudo` and `[attr]` simple
    // selectors with no whitespace between them (T-173).
    bool parseCompound(SelectorPart& part) {
        // True until the first simple selector of the compound is consumed: only that one may be
        // preceded by whitespace.
        bool atStart = true;
        if (m_cur.kind == TokenKind::Star) {
            advance();
            atStart = false;
        } else if (m_cur.kind == TokenKind::Ident) {
            part.tag = m_cur.text;
            advance();
            atStart = false;
        } else if (!(m_cur.kind == TokenKind::Dot || m_cur.kind == TokenKind::Hash ||
                     m_cur.kind == TokenKind::Colon || m_cur.kind == TokenKind::LBracket)) {
            error(m_cur, "expected a selector");
            return false;
        }
        for (;;) {
            if (!atStart && m_cur.wsBefore) {
                break;  // Whitespace ends the compound (descendant combinator, T-173).
            }
            atStart = false;
            if (m_cur.kind == TokenKind::Dot) {
                advance();
                if (m_cur.kind != TokenKind::Ident || m_cur.wsBefore) {
                    error(m_cur, "expected class name after '.'");
                    return false;
                }
                part.classes.push_back(m_cur.text);
                advance();
            } else if (m_cur.kind == TokenKind::Hash) {
                part.id = m_cur.text;
                advance();
                // T-218 (31 §31.5.1): `#Icon/*` -- every node of the element's inline `<svg>`.
                // (`#Icon/Blade` and `#Icon/path` stay one path id, T-201; resolveSceneTargets()
                // tells them from element path ids once the DOM is known.)
                if (part.id.size() > 2 && part.id.compare(part.id.size() - 2, 2, "/*") == 0) {
                    part.id.resize(part.id.size() - 2);
                    part.sceneKind = SceneTargetKind::Any;
                }
            } else if (m_cur.kind == TokenKind::LBracket) {
                SelectorCond cond;
                if (!parseAttributeSelector(cond)) {
                    return false;
                }
                part.conds.push_back(std::move(cond));
            } else if (m_cur.kind == TokenKind::Colon) {
                advance();
                if (m_cur.kind != TokenKind::Ident || m_cur.wsBefore) {
                    error(m_cur, "expected pseudo-class name after ':'");
                    return false;
                }
                if (m_cur.text == "root") {
                    m_sawRoot = true;  // T-170; validated by parseSelectorList.
                    advance();
                    continue;
                }
                if (m_cur.text == "not" && nextIsCallParen()) {
                    if (!parseNot(part)) {
                        return false;
                    }
                    continue;
                }
                SelectorCond cond;
                uint16_t bit = 0;
                if (!parsePseudoCondition(cond, bit)) {
                    return false;
                }
                if (bit != 0) {
                    part.stateMask |= bit;
                } else {
                    part.conds.push_back(std::move(cond));
                }
            } else {
                break;
            }
        }
        return true;
    }

    // True when the token after the current Ident is a `(` glued to it (a function call).
    bool nextIsCallParen() const {
        Lexer ahead = m_lexer;
        Token t = ahead.next();
        return t.kind == TokenKind::LParen && !t.wsBefore;
    }

    // `[name]`, `[name=v]`, `[name^=v]`, `[name$=v]`, `[name*=v]` (T-173); the current token is
    // `[`. `v` is an identifier, a string or a number.
    bool parseAttributeSelector(SelectorCond& cond) {
        advance();  // '['
        if (m_cur.kind != TokenKind::Ident) {
            error(m_cur, "expected attribute name after '['");
            return false;
        }
        cond.name = m_cur.text;
        if (cond.name == "class") {
            error(m_cur, "attribute selector on \"class\" is not supported; use .class");
            return false;
        }
        advance();
        if (m_cur.kind == TokenKind::RBracket) {
            cond.kind = SelectorCondKind::AttrExists;
            advance();
            return true;
        }
        if (m_cur.kind == TokenKind::Equals) {
            cond.kind = SelectorCondKind::AttrEquals;
        } else if (m_cur.kind == TokenKind::Caret || m_cur.kind == TokenKind::Dollar ||
                   m_cur.kind == TokenKind::Star) {
            cond.kind = m_cur.kind == TokenKind::Caret    ? SelectorCondKind::AttrPrefix
                        : m_cur.kind == TokenKind::Dollar ? SelectorCondKind::AttrSuffix
                                                          : SelectorCondKind::AttrSubstring;
            advance();
            if (m_cur.kind != TokenKind::Equals || m_cur.wsBefore) {
                error(m_cur, "expected '=' in attribute selector");
                return false;
            }
        } else {
            error(m_cur, "unsupported attribute selector operator");
            return false;
        }
        advance();  // '='
        if (m_cur.kind == TokenKind::Ident || m_cur.kind == TokenKind::String) {
            cond.value = m_cur.text;
        } else if (m_cur.kind == TokenKind::Number) {
            cond.value = m_cur.text + m_cur.unit;
        } else {
            error(m_cur, "expected attribute value");
            return false;
        }
        advance();
        if (m_cur.kind != TokenKind::RBracket) {
            error(m_cur, "expected ']' after attribute selector");
            return false;
        }
        advance();
        return true;
    }

    // One pseudo-class other than `:root`/`:not()`; the current token is its name. A state
    // pseudo-class sets `bit`; everything else fills `cond` (T-173).
    bool parsePseudoCondition(SelectorCond& cond, uint16_t& bit) {
        const Token nameTok = m_cur;
        const std::string name = m_cur.text;
        if (pseudoStateBit(name, bit)) {
            advance();
            return true;
        }
        bit = 0;
        if (name == "first-child" || name == "last-child" || name == "focus-within") {
            cond.kind = name == "first-child"  ? SelectorCondKind::FirstChild
                        : name == "last-child" ? SelectorCondKind::LastChild
                                               : SelectorCondKind::FocusWithin;
            advance();
            return true;
        }
        if (name == "state" && nextIsCallParen()) {
            // T-174 (27.1): `:state(<Name>)`, a custom state (the name follows the id rules).
            advance();  // name
            advance();  // '('
            if (m_cur.kind != TokenKind::Ident || m_cur.text.find('-') != std::string::npos) {
                error(m_cur, "expected a state name in \":state()\"");
                return false;
            }
            if (const uint16_t builtin = builtinStateAlias(m_cur.text); builtin != 0) {
                // T-390 / T-392 (27.3.2, 27.3.5): `:state(Disabled)` is the built-in `:disabled`,
                // `:state(Focused|Hover|Active|Checked)` `:focus` / `:hover` / `:active` / `:checked`.
                bit = builtin;
            } else {
                cond.kind = SelectorCondKind::CustomState;
                cond.name = m_cur.text;
            }
            advance();
            if (m_cur.kind != TokenKind::RParen) {
                error(m_cur, "expected ')' after the state name in \":state()\"");
                return false;
            }
            advance();
            return true;
        }
        if (name == "focus-player" && nextIsCallParen()) {
            // T-194 (08.9): `:focus-player(<n>)`, n = 0..7 (InputEvent::player).
            advance();  // name
            advance();  // '('
            if (m_cur.kind != TokenKind::Number || !m_cur.unit.empty() ||
                m_cur.text.find('.') != std::string::npos || std::atoi(m_cur.text.c_str()) < 0 ||
                std::atoi(m_cur.text.c_str()) > 7) {
                error(m_cur, "expected a player index 0-7 in \":focus-player()\"");
                return false;
            }
            cond.kind = SelectorCondKind::FocusPlayer;
            cond.a = std::atoi(m_cur.text.c_str());
            advance();
            if (m_cur.kind != TokenKind::RParen) {
                error(m_cur, "expected ')' after the player index in \":focus-player()\"");
                return false;
            }
            advance();
            return true;
        }
        if ((name == "nth-child" || name == "lang") && nextIsCallParen()) {
            advance();  // name
            advance();  // '('
            std::string arg;
            while (m_cur.kind != TokenKind::RParen) {
                if (m_cur.kind == TokenKind::End || m_cur.kind == TokenKind::LBrace ||
                    m_cur.kind == TokenKind::Semicolon) {
                    error(m_cur, "expected ')' after :" + name + "() argument");
                    return false;
                }
                arg += argumentText(m_cur);
                advance();
            }
            advance();  // ')'
            if (name == "lang") {
                if (arg.empty()) {
                    error(nameTok, "expected a language tag in :lang()");
                    return false;
                }
                for (char ch : arg) {
                    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-')) {
                        error(nameTok, "invalid :lang() argument \"" + arg + "\"");
                        return false;
                    }
                }
                cond.kind = SelectorCondKind::Lang;
                cond.name = arg;
                return true;
            }
            cond.kind = SelectorCondKind::NthChild;
            if (!parseAnPlusB(arg, cond.a, cond.b)) {
                error(nameTok, "invalid :nth-child() argument \"" + arg + "\"");
                return false;
            }
            return true;
        }
        error(nameTok, "unknown pseudo-class \":" + name + "\"");
        return false;
    }

    // The source text of one token inside `:nth-child()`/`:lang()` (whitespace dropped). A lone
    // `+` has no token kind of its own and arrives as Unknown.
    static std::string argumentText(const Token& t) {
        switch (t.kind) {
            case TokenKind::Ident:
            case TokenKind::String:
                return t.text;
            case TokenKind::Number:
                return t.text + t.unit;
            case TokenKind::Unknown:
                return t.text == "unexpected character '+'" ? std::string("+") : std::string("?");
            default:
                return std::string("?");
        }
    }

    // `:not(<simple>, <simple>, ...)` (T-173): every argument becomes one negated condition, all
    // sharing a new `notGroup`. The current token is `not`.
    bool parseNot(SelectorPart& part) {
        advance();  // 'not'
        advance();  // '('
        const uint16_t group = ++m_notGroups;
        for (;;) {
            SelectorCond cond;
            cond.negated = true;
            cond.notGroup = group;
            if (m_cur.kind == TokenKind::Star) {
                cond.kind = SelectorCondKind::Any;
                advance();
            } else if (m_cur.kind == TokenKind::Ident) {
                cond.kind = SelectorCondKind::Tag;
                cond.name = m_cur.text;
                advance();
            } else if (m_cur.kind == TokenKind::Dot) {
                advance();
                if (m_cur.kind != TokenKind::Ident || m_cur.wsBefore) {
                    error(m_cur, "expected class name after '.'");
                    return false;
                }
                cond.kind = SelectorCondKind::Class;
                cond.name = m_cur.text;
                advance();
            } else if (m_cur.kind == TokenKind::Hash) {
                cond.kind = SelectorCondKind::Id;
                cond.name = m_cur.text;
                advance();
            } else if (m_cur.kind == TokenKind::LBracket) {
                if (!parseAttributeSelector(cond)) {
                    return false;
                }
            } else if (m_cur.kind == TokenKind::Colon) {
                advance();
                if (m_cur.kind != TokenKind::Ident || m_cur.wsBefore) {
                    error(m_cur, "expected pseudo-class name after ':'");
                    return false;
                }
                if (m_cur.text == "not" || m_cur.text == "root") {
                    error(m_cur, "\":" + m_cur.text + "\" is not allowed inside :not()");
                    return false;
                }
                uint16_t bit = 0;
                if (!parsePseudoCondition(cond, bit)) {
                    return false;
                }
                if (bit != 0) {
                    cond.kind = SelectorCondKind::State;
                    cond.state = bit;
                }
            } else {
                error(m_cur, "expected a simple selector inside :not()");
                return false;
            }
            part.conds.push_back(std::move(cond));
            if (m_cur.kind == TokenKind::Comma) {
                advance();
                continue;
            }
            if (m_cur.kind == TokenKind::RParen) {
                advance();
                return true;
            }
            error(m_cur, ":not() accepts a list of simple selectors only");
            return false;
        }
    }

    // T-171: `symbol(<Name>) {` starts a symbol-scoped block (docs/design/26-layers-symbols-
    // library.md §26.2.1). Looks one token past the current `symbol` ident on a copy of the lexer.
    bool atSymbolBlock() const {
        if (m_cur.kind != TokenKind::Ident || m_cur.text != "symbol") {
            return false;
        }
        Lexer ahead = m_lexer;
        return ahead.next().kind == TokenKind::LParen;
    }

    // `symbol(<Name>) { <rule>* }`: every rule inside is tagged with StyleRule::symbol = Name
    // (rewritten per instance later, symbol_expand.cpp). At-rules inside the block are parsed as
    // ordinary document-wide at-rules.
    void parseSymbolBlock(RuleSet& out) {
        advance();  // 'symbol'
        advance();  // '('
        if (m_cur.kind != TokenKind::Ident) {
            error(m_cur, "expected symbol name in symbol()");
            recoverToNextRBrace();
            return;
        }
        std::string name = m_cur.text;
        advance();
        if (m_cur.kind != TokenKind::RParen) {
            error(m_cur, "expected ')' after symbol name");
            recoverToNextRBrace();
            return;
        }
        advance();
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after symbol(" + name + ")");
            recoverToNextRBrace();
            return;
        }
        advance();
        m_inSymbol = true;
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated symbol(" + name + ") block");
                m_inSymbol = false;
                return;
            }
            if (m_cur.kind == TokenKind::At) {
                // T-190 (26.2.1, 28 §28.4): an `@timeline` inside `symbol()` is symbol-scoped
                // (rewritten per instance by symbol_expand.cpp); other at-rules stay document-wide.
                size_t timelinesBefore = out.timelines.size();
                parseAtRule(out);
                for (size_t i = timelinesBefore; i < out.timelines.size(); ++i) {
                    out.timelines[i].symbol = name;
                }
                continue;
            }
            size_t before = out.rules.size();
            parseRule(out);
            for (size_t i = before; i < out.rules.size(); ++i) {
                out.rules[i].symbol = name;
            }
        }
        m_inSymbol = false;
        advance();  // '}'
    }

    void parseAtRule(RuleSet& out) {
        Token atTok = m_cur;
        advance();  // '@'
        if (m_cur.kind != TokenKind::Ident) {
            error(atTok, "expected at-rule name after '@'");
            recoverToNextRBrace();
            return;
        }
        std::string name = m_cur.text;
        advance();
        if (name == "keyframes") {
            parseKeyframes(out);
        } else if (name == "font-face") {
            parseFontFace(out);
        } else if (name == "font-fallback") {
            parseFontFallback(atTok, out);
        } else if (name == "timeline") {
            parseTimeline(atTok, out);
        } else if (name == "media") {
            parseMedia(atTok, out);
        } else {
            warn(atTok, "unknown at-rule \"@" + name + "\", ignored");
            while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::End &&
                   m_cur.kind != TokenKind::Semicolon) {
                advance();
            }
            if (m_cur.kind == TokenKind::LBrace) {
                skipBracedBlock();
            } else if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            }
        }
    }

    // T-176 (05.9.2-5.9.3): the operator character of a lexer-less `<` or `/` (Unknown
    // "unexpected character" token), or 0.
    static char unknownChar(const Token& t) {
        const std::string_view prefix = "unexpected character '";
        if (t.kind == TokenKind::Unknown && t.text.size() == prefix.size() + 2 &&
            t.text.compare(0, prefix.size(), prefix) == 0) {
            return t.text[prefix.size()];
        }
        return 0;
    }

    // One `( ... )` of an `@media` query; m_cur is on the `(`. Returns false (after an error) on
    // anything 05.9.2 does not list.
    // T-176: a query this dialect does not support (05.9.3's list) is a warning and the whole
    // block is skipped, like any unknown at-rule before T-176 -- web CSS pasted by an importer
    // keeps compiling. Structural misuse inside a supported block stays an error.
    void mediaWarn(const Token& tok, const std::string& msg) { warn(tok, msg + "; @media block ignored"); }

    bool parseMediaCondition(MediaCondDecl& cond) {
        Token open = m_cur;
        advance();  // '('
        if (m_cur.kind != TokenKind::Ident) {
            mediaWarn(m_cur, "expected a media feature name");
            return false;
        }
        const std::string feature = m_cur.text;
        Token featureTok = m_cur;
        advance();
        if (feature == "aspect-ratio") {
            // `>=` lexes as Gt + '=', `<=` as '<' + '=' (no lexer tokens for them).
            bool ge = false;
            if (m_cur.kind == TokenKind::Gt) {
                ge = true;
            } else if (unknownChar(m_cur) != '<') {
                mediaWarn(featureTok, "@media (aspect-ratio) needs >= or <= (e.g. (aspect-ratio >= 21/9))");
                return false;
            }
            advance();
            if (m_cur.kind != TokenKind::Equals && unknownChar(m_cur) != '=') {
                mediaWarn(featureTok, "@media (aspect-ratio) needs >= or <= (e.g. (aspect-ratio >= 21/9))");
                return false;
            }
            advance();
            if (m_cur.kind != TokenKind::Number || !m_cur.unit.empty() || !(m_cur.num > 0.0f)) {
                mediaWarn(m_cur, "expected a ratio such as 16/9 or 1.78");
                return false;
            }
            float ratio = m_cur.num;
            advance();
            if (unknownChar(m_cur) == '/') {
                advance();
                if (m_cur.kind != TokenKind::Number || !m_cur.unit.empty() || !(m_cur.num > 0.0f)) {
                    mediaWarn(m_cur, "expected a ratio such as 16/9 or 1.78");
                    return false;
                }
                ratio /= m_cur.num;
                advance();
            }
            cond.feature = ge ? MediaFeature::MinAspect : MediaFeature::MaxAspect;
            cond.value = ratio;
        } else {
            if (m_cur.kind != TokenKind::Colon) {
                mediaWarn(featureTok, "expected ':' after media feature \"" + feature + "\"");
                return false;
            }
            advance();
            if (feature == "min-width" || feature == "max-width" || feature == "min-height" ||
                feature == "max-height") {
                if (m_cur.kind != TokenKind::Number ||
                    !(m_cur.unit == "px" || (m_cur.unit.empty() && m_cur.num == 0))) {
                    mediaWarn(m_cur, "@media (" + feature + ") takes a px length");
                    return false;
                }
                cond.feature = feature == "min-width"    ? MediaFeature::MinWidth
                               : feature == "max-width"  ? MediaFeature::MaxWidth
                               : feature == "min-height" ? MediaFeature::MinHeight
                                                         : MediaFeature::MaxHeight;
                cond.value = m_cur.num;
                advance();
            } else if (feature == "resolution" || feature == "min-resolution" ||
                       feature == "max-resolution") {
                // T-330: against DeviceInfo::dpr; `dppx` and `x` as written, `dpi` / 96.
                if (m_cur.kind != TokenKind::Number || !(m_cur.num > 0.0f) ||
                    !(m_cur.unit == "dppx" || m_cur.unit == "x" || m_cur.unit == "dpi")) {
                    mediaWarn(m_cur, "@media (" + feature + ") takes a positive dppx, x or dpi value");
                    return false;
                }
                cond.feature = feature == "min-resolution"   ? MediaFeature::MinResolution
                               : feature == "max-resolution" ? MediaFeature::MaxResolution
                                                             : MediaFeature::Resolution;
                cond.value = m_cur.unit == "dpi" ? m_cur.num / 96.0f : m_cur.num;
                advance();
            } else {
                static constexpr struct {
                    const char* feature;
                    MediaFeature id;
                    const char* keywords[3];
                } kKeywordFeatures[] = {
                    {"orientation", MediaFeature::Orientation, {"landscape", "portrait", nullptr}},
                    {"platform", MediaFeature::Platform, {"desktop", "console", "handheld"}},
                    {"input", MediaFeature::Input, {"mouse", "gamepad", "touch"}},
                    // T-330: value 0 = no-preference, 1 = reduce (DeviceInfo::reducedMotion).
                    {"prefers-reduced-motion",
                     MediaFeature::ReducedMotion,
                     {"no-preference", "reduce", nullptr}},
                };
                bool known = false;
                for (const auto& entry : kKeywordFeatures) {
                    if (feature != entry.feature) {
                        continue;
                    }
                    known = true;
                    int found = -1;
                    for (int k = 0; k < 3 && entry.keywords[k] != nullptr; ++k) {
                        if (m_cur.kind == TokenKind::Ident && m_cur.text == entry.keywords[k]) {
                            found = k;
                        }
                    }
                    if (found < 0) {
                        mediaWarn(m_cur, "unknown value for @media (" + feature + ")");
                        return false;
                    }
                    cond.feature = entry.id;
                    cond.value = static_cast<float>(found);
                    advance();
                }
                if (!known) {
                    mediaWarn(featureTok, "unsupported @media feature \"" + feature + "\"");
                    return false;
                }
            }
        }
        if (m_cur.kind != TokenKind::RParen) {
            mediaWarn(open, "expected ')' to close the media condition");
            return false;
        }
        advance();
        return true;
    }

    // `@media <query> [, <query>]* { <rule>* }` (05.9.2-5.9.3, T-176). A query is `(cond)
    // [and (cond)]*`. Rules inside are tagged with the block's index; at-rules, symbol() blocks,
    // nested @media and token declarations inside are errors.
    void parseMedia(const Token& atTok, RuleSet& out) {
        if (m_inMedia || m_inSymbol) {
            error(atTok, m_inMedia ? "@media cannot be nested" : "@media is not allowed inside symbol()");
            while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::End) {
                advance();
            }
            skipBracedBlock();
            return;
        }
        MediaDef def;
        def.file = m_fileName;
        def.line = reportLine(atTok.line);
        bool ok = true;
        for (;;) {
            std::vector<MediaCondDecl> query;
            for (;;) {
                if (m_cur.kind != TokenKind::LParen) {
                    mediaWarn(m_cur, "expected '(' to start a media condition");
                    ok = false;
                    break;
                }
                MediaCondDecl cond;
                if (!parseMediaCondition(cond)) {
                    ok = false;
                    break;
                }
                query.push_back(cond);
                if (m_cur.kind == TokenKind::Ident && m_cur.text == "and") {
                    advance();
                    continue;
                }
                break;
            }
            if (!ok) {
                break;
            }
            def.queries.push_back(std::move(query));
            if (m_cur.kind == TokenKind::Comma) {
                advance();
                continue;
            }
            break;
        }
        if (ok && m_cur.kind != TokenKind::LBrace) {
            mediaWarn(m_cur, "expected '{' after the @media query list");
            ok = false;
        }
        if (!ok) {
            while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::End) {
                advance();
            }
            skipBracedBlock();
            return;
        }
        advance();  // '{'
        const int index = static_cast<int>(out.media.size());
        out.media.push_back(std::move(def));
        m_inMedia = true;
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated @media block");
                m_inMedia = false;
                return;
            }
            if (m_cur.kind == TokenKind::At) {
                Token inner = m_cur;
                advance();  // '@'
                error(inner, m_cur.kind == TokenKind::Ident && m_cur.text == "media"
                                 ? "@media cannot be nested"
                                 : "at-rules are not allowed inside @media");
                while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::End &&
                       m_cur.kind != TokenKind::Semicolon) {
                    advance();
                }
                if (m_cur.kind == TokenKind::LBrace) {
                    skipBracedBlock();
                } else if (m_cur.kind == TokenKind::Semicolon) {
                    advance();
                }
                continue;
            }
            if (atSymbolBlock()) {
                error(m_cur, "symbol() blocks are not allowed inside @media");
                while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::End) {
                    advance();
                }
                skipBracedBlock();
                continue;
            }
            size_t before = out.rules.size();
            parseRule(out);
            for (size_t i = before; i < out.rules.size(); ++i) {
                out.rules[i].media = index;
            }
        }
        m_inMedia = false;
        advance();  // '}'
    }

    void parseKeyframes(RuleSet& out) {
        if (m_cur.kind != TokenKind::Ident) {
            error(m_cur, "expected @keyframes name");
            recoverToNextRBrace();
            return;
        }
        KeyframesDef def;
        def.name = m_cur.text;
        advance();
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after @keyframes name");
            recoverToNextRBrace();
            return;
        }
        advance();
        while (m_cur.kind != TokenKind::RBrace && m_cur.kind != TokenKind::End) {
            std::vector<float> offsets;
            for (;;) {
                if (m_cur.kind == TokenKind::Ident && (m_cur.text == "from" || m_cur.text == "to")) {
                    offsets.push_back(m_cur.text == "from" ? 0.0f : 100.0f);
                    advance();
                } else if (m_cur.kind == TokenKind::Number && m_cur.unit == "%") {
                    offsets.push_back(m_cur.num);
                    advance();
                } else {
                    error(m_cur, "expected 'from', 'to' or a percentage");
                    recoverToNextRBrace();  // skips the rest of @keyframes body too (best effort).
                    return;
                }
                if (m_cur.kind == TokenKind::Comma) {
                    advance();
                    continue;
                }
                break;
            }
            if (m_cur.kind != TokenKind::LBrace) {
                error(m_cur, "expected '{' after keyframe selector");
                recoverToNextRBrace();
                return;
            }
            advance();
            std::vector<StyleDecl> decls;
            Easing timing;
            bool hasTiming = false;
            bool sawEnd = false;
            std::vector<TransformPart> xforms;  // T-329: combined per stop, like a rule block.
            std::vector<TransformPart>* savedXforms = m_transformParts;
            m_transformParts = &xforms;
            parseKeyframeStopDecls(decls, timing, hasTiming, sawEnd);
            m_transformParts = savedXforms;
            mergeIndependentTransforms(xforms, decls);
            if (!sawEnd) {
                return;  // Error already reported; recovery already skipped forward.
            }
            for (float off : offsets) {
                KeyframeStop stop;
                stop.offset = off;
                stop.decls = decls;
                stop.timing = timing;
                stop.hasTiming = hasTiming;
                def.stops.push_back(std::move(stop));
            }
        }
        if (m_cur.kind == TokenKind::RBrace) {
            advance();
        } else {
            error(m_cur, "unterminated @keyframes block");
        }
        out.keyframes.push_back(std::move(def));
    }

    // Like parseDecls(insideBraces=true) but intercepts "animation-timing-function" as this
    // stop's `timing` instead of a regular decl (05.3's @keyframes rule).
    void parseKeyframeStopDecls(std::vector<StyleDecl>& decls, Easing& timing, bool& hasTiming,
                                bool& sawEnd) {
        for (;;) {
            if (m_cur.kind == TokenKind::RBrace) {
                advance();
                sawEnd = true;
                return;
            }
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated block, expected '}'");
                sawEnd = false;
                return;
            }
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected property name");
                recoverToNextRBrace();
                sawEnd = false;
                return;
            }
            Token nameTok = m_cur;
            advance();
            if (m_cur.kind != TokenKind::Colon) {
                error(nameTok, "expected ':' after property name \"" + nameTok.text + "\"");
                recoverToNextRBrace();
                sawEnd = false;
                return;
            }
            advance();
            std::vector<Token> valueToks;
            while (m_cur.kind != TokenKind::Semicolon && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::End) {
                valueToks.push_back(m_cur);
                advance();
            }
            if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            } else if (m_cur.kind != TokenKind::RBrace && m_cur.kind != TokenKind::End) {
                error(nameTok, "expected ';' or '}' after declaration");
                recoverToNextRBrace();
                sawEnd = false;
                return;
            }
            if (nameTok.text == "animation-timing-function") {
                Easing e;
                bool stepsClamped = false;
                if (parseEasingValue(valueToks, e, &stepsClamped, m_easingPoints)) {
                    timing = e;
                    hasTiming = true;
                    if (stepsClamped) {
                        warn(nameTok, "steps() count clamped to [1, 65535]");  // T-258b (D11)
                    }
                } else {
                    warn(nameTok, "invalid value for \"animation-timing-function\"");
                }
                continue;
            }
            const PropInfo* info = findProp(nameTok.text);
            if (info == nullptr) {
                warn(nameTok, "unknown property \"" + nameTok.text + "\"");
                continue;
            }
            if (isVectorPropInfo(*info)) {
                warn(nameTok, "\"" + nameTok.text +
                                  "\" only applies to inline <svg> content in @timeline keys; ignored");
                continue;
            }
            std::vector<StyleDecl> expanded;
            size_t sinkSizeBefore = m_sink.all().size();
            const bool parsed =
                containsVarFunction(valueToks)  // T-170: flattened later by resolveRuleSetVars().
                    ? parseVarDecl(info, valueToks, m_fileName, reportLine(nameTok.line), m_sink, expanded)
                    : parseAndExpandDecl(info, valueToks, m_fileName, reportLine(nameTok.line), m_sink,
                                         expanded, m_easingPoints);
            if (parsed) {
                for (StyleDecl& d : expanded) {
                    decls.push_back(std::move(d));
                }
            }
            recordTransformPart(nameTok, valueToks, false, parsed);
            syncErrorFlag(sinkSizeBefore);
        }
    }

    // ---------------------------------------------------------------------------------------
    // @timeline (docs/design/28-animation-editing.md §28.1.2, 05-style-and-css.md §5.9.2-5.9.3,
    // docs/tasks/T-172.md).
    // ---------------------------------------------------------------------------------------

    static bool isSlashToken(const Token& t) {
        return t.kind == TokenKind::Unknown && t.text == "unexpected character '/'";
    }

    // Skips to just past the `}` closing the block whose `{` was already consumed.
    void skipToBlockEnd() {
        int depth = 1;
        while (m_cur.kind != TokenKind::End) {
            if (m_cur.kind == TokenKind::LBrace) {
                ++depth;
            } else if (m_cur.kind == TokenKind::RBrace) {
                --depth;
                if (depth == 0) {
                    advance();
                    return;
                }
            }
            advance();
        }
    }

    bool readTime(float& seconds) {
        if (!parseTimelineTime(m_cur, seconds)) {
            error(m_cur, "expected a time in s or ms");
            return false;
        }
        advance();
        return true;
    }

    void parseTimeline(const Token& atTok, RuleSet& out) {
        if (m_cur.kind != TokenKind::Ident) {
            error(m_cur, "expected @timeline name");
            recoverToNextRBrace();
            return;
        }
        TimelineDef def;
        def.name = m_cur.text;
        def.fileName = m_fileName;
        def.line = reportLine(atTok.line);
        advance();
        if (m_cur.kind == TokenKind::Ident && m_cur.text == "from") {
            advance();
            if (m_cur.kind != TokenKind::String) {
                error(m_cur, "expected \"<file>#<name>\" after 'from'");
                recoverToNextRBrace();
                return;
            }
            const std::string& ref = m_cur.text;
            size_t hash = ref.rfind('#');
            if (hash == std::string::npos || hash == 0 || hash + 1 == ref.size()) {
                error(m_cur, "@timeline from reference must be \"<file>#<name>\"");
                advance();
                return;
            }
            def.fromFile = ref.substr(0, hash);
            def.fromName = ref.substr(hash + 1);
            advance();
            if (m_cur.kind != TokenKind::LBrace) {
                // `@timeline <name> from "<file>#<name>"` with no body; an optional ';' ends it.
                if (m_cur.kind == TokenKind::Semicolon) {
                    advance();
                }
                out.timelines.push_back(std::move(def));
                return;
            }
        }
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after @timeline name");
            recoverToNextRBrace();
            return;
        }
        advance();  // '{'
        bool ok = true;
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated @timeline block");
                return;
            }
            if (m_cur.kind == TokenKind::Hash) {
                ok = parseTimelineTrack(def) && ok;
                continue;
            }
            if (m_cur.kind == TokenKind::Ident && m_cur.text == "sound") {
                ok = parseTimelineSound(def) && ok;
                continue;
            }
            if (m_cur.kind == TokenKind::Ident && (m_cur.text == "duration" || m_cur.text == "marker")) {
                ok = parseTimelineProperty(def) && ok;
                continue;
            }
            if (m_cur.kind == TokenKind::Ident || m_cur.kind == TokenKind::Dot ||
                m_cur.kind == TokenKind::Star) {
                error(m_cur, "@timeline track selector must be an #id");  // 05.9.2
            } else {
                error(m_cur, "unexpected token in @timeline");
            }
            ok = false;
            while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::Semicolon &&
                   m_cur.kind != TokenKind::RBrace && m_cur.kind != TokenKind::End) {
                advance();
            }
            if (m_cur.kind == TokenKind::LBrace) {
                advance();
                skipToBlockEnd();
            } else if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            }
        }
        advance();  // '}'
        if (ok) {
            out.timelines.push_back(std::move(def));
        }
    }

    // `duration: <t>;` or `marker: <name> <t>, ...;` (the ';' may be omitted before '}').
    bool parseTimelineProperty(TimelineDef& def) {
        Token nameTok = m_cur;
        advance();
        if (m_cur.kind != TokenKind::Colon) {
            error(nameTok, "expected ':' after \"" + nameTok.text + "\"");
            recoverToNextRBrace();
            return false;
        }
        advance();
        bool ok = true;
        if (nameTok.text == "duration") {
            float t = 0;
            ok = readTime(t);
            if (ok) {
                def.duration = t;
            }
        } else {
            for (;;) {
                if (m_cur.kind != TokenKind::Ident) {
                    error(m_cur, "expected a marker name");
                    ok = false;
                    break;
                }
                std::string markerName = m_cur.text;
                advance();
                float t = 0;
                if (!readTime(t)) {
                    ok = false;
                    break;
                }
                def.markers.emplace_back(std::move(markerName), t);
                if (m_cur.kind != TokenKind::Comma) {
                    break;
                }
                advance();
            }
        }
        if (!ok) {
            while (m_cur.kind != TokenKind::Semicolon && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::End) {
                advance();
            }
        }
        if (m_cur.kind == TokenKind::Semicolon) {
            advance();
        } else if (ok && m_cur.kind != TokenKind::RBrace) {
            error(m_cur, "expected ';' after \"" + nameTok.text + "\"");
            return false;
        }
        return ok;
    }

    // `sound { <t> <cue>; ... }` (28.1.2, 28.6). A cue is an identifier or a string.
    bool parseTimelineSound(TimelineDef& def) {
        advance();  // 'sound'
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after 'sound'");
            recoverToNextRBrace();
            return false;
        }
        advance();
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated sound block");
                return false;
            }
            float t = 0;
            if (!readTime(t)) {
                skipToBlockEnd();
                return false;
            }
            if (m_cur.kind != TokenKind::Ident && m_cur.kind != TokenKind::String) {
                error(m_cur, "expected a sound cue name");
                skipToBlockEnd();
                return false;
            }
            def.sounds.emplace_back(t, m_cur.text);
            advance();
            if (m_cur.kind == TokenKind::Semicolon || m_cur.kind == TokenKind::Comma) {
                advance();
            }
        }
        advance();  // '}'
        return true;
    }

    // `#<id>[/<id>...] { <t> [<easing>] { decls } ... }`.
    bool parseTimelineTrack(TimelineDef& def) {
        TimelineTrackDef track;
        track.line = reportLine(m_cur.line);
        track.path = m_cur.text;
        advance();
        // A symbol path id (`#Play/Bg`, 12.7): the lexer has no '/' token, so each '/' arrives as
        // an Unknown "unexpected character" token between identifiers.
        while (isSlashToken(m_cur)) {
            advance();
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected an id after '/' in a path id");
                recoverToNextRBrace();
                return false;
            }
            track.path += "/" + m_cur.text;
            advance();
        }
        if (track.path.empty()) {
            error(m_cur, "expected an element id after '#'");
            recoverToNextRBrace();
            return false;
        }
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after @timeline track selector");
            recoverToNextRBrace();
            return false;
        }
        advance();
        bool ok = true;
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated @timeline track");
                return false;
            }
            TimelineKey key;
            key.line = reportLine(m_cur.line);
            if (!readTime(key.time)) {
                skipToBlockEnd();
                return false;
            }
            std::vector<Token> easingToks;
            while (m_cur.kind != TokenKind::LBrace && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::End && m_cur.kind != TokenKind::Semicolon) {
                easingToks.push_back(m_cur);
                advance();
            }
            if (m_cur.kind != TokenKind::LBrace) {
                error(m_cur, "expected '{' after a @timeline key time");
                skipToBlockEnd();
                return false;
            }
            if (!easingToks.empty()) {
                bool stepsClamped = false;
                if (parseEasingValue(easingToks, key.easing, &stepsClamped, m_easingPoints)) {
                    key.hasEasing = true;
                    if (stepsClamped) {
                        warn(easingToks.front(), "steps() count clamped to [1, 65535]");  // T-258b (D11)
                    }
                } else {
                    error(easingToks.front(), "invalid easing in @timeline key");
                    ok = false;
                }
            }
            advance();  // '{'
            bool sawEnd = false;
            parseTimelineKeyDecls(key, /*insideBraces=*/true, sawEnd);
            if (!sawEnd) {
                // Structural error inside the key: recoverToNextRBrace() consumed the key's '}' at
                // best; drop the rest of the track.
                skipToBlockEnd();
                return false;
            }
            track.keys.push_back(std::move(key));
        }
        advance();  // '}'
        def.tracks.push_back(std::move(track));
        return ok;
    }

    // The declarations of one `@timeline` key: regular properties (shorthands such as `transform`
    // expanded as usual, 05.5) plus `effect.<effect>.<param>` (18.12). Same structure and recovery
    // as parseDecls(); a value that fails to parse is an error for an effect parameter and the
    // usual per-property diagnostic otherwise.
    void parseTimelineKeyDecls(TimelineKey& key, bool insideBraces, bool& sawEnd) {
        std::vector<TransformPart> xforms;  // T-329: combined per key, like a rule block.
        std::vector<TransformPart>* savedXforms = m_transformParts;
        m_transformParts = &xforms;
        parseTimelineKeyDeclsBody(key, insideBraces, sawEnd);
        m_transformParts = savedXforms;
        mergeIndependentTransforms(xforms, key.decls);
    }

    void parseTimelineKeyDeclsBody(TimelineKey& key, bool insideBraces, bool& sawEnd) {
        for (;;) {
            if (insideBraces && m_cur.kind == TokenKind::RBrace) {
                advance();
                sawEnd = true;
                return;
            }
            if (m_cur.kind == TokenKind::End) {
                if (insideBraces) {
                    error(m_cur, "unterminated block, expected '}'");
                    sawEnd = false;
                    return;
                }
                sawEnd = true;
                return;
            }
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected property name");
                if (insideBraces) {
                    recoverToNextRBrace();
                }
                sawEnd = false;
                return;
            }
            Token nameTok = m_cur;
            advance();
            bool isEffect = false;
            TimelineEffectDecl fx;
            if (nameTok.text == "effect" &&
                (m_cur.kind == TokenKind::Dot || m_cur.kind == TokenKind::Number)) {
                isEffect = true;
                if (!parseEffectAddress(fx)) {
                    if (insideBraces) {
                        recoverToNextRBrace();
                    }
                    sawEnd = false;
                    return;
                }
            }
            if (m_cur.kind != TokenKind::Colon) {
                error(nameTok, "expected ':' after property name \"" + nameTok.text + "\"");
                if (insideBraces) {
                    recoverToNextRBrace();
                }
                sawEnd = false;
                return;
            }
            advance();
            std::vector<Token> valueToks;
            while (m_cur.kind != TokenKind::Semicolon && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::End) {
                valueToks.push_back(m_cur);
                advance();
            }
            if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            } else if (m_cur.kind == TokenKind::End && insideBraces) {
                error(nameTok, "unterminated block, expected '}'");
                sawEnd = false;
                return;
            }
            int line = reportLine(nameTok.line);
            if (isEffect) {
                fx.line = line;
                if (!parseEffectParamValue(valueToks, fx.param == "color", fx.value)) {
                    error(nameTok, "invalid value for \"effect." + fx.effect + "." + fx.param + "\"");
                    continue;
                }
                key.effectDecls.push_back(std::move(fx));
                continue;
            }
            if (nameTok.text == "motion-in" || nameTok.text == "motion-out") {
                // T-228 (31 §31.7.2): key-only spatial handles, two px lengths (or bare 0).
                Vec2 v{};
                bool good = valueToks.size() == 2;
                for (size_t i = 0; good && i < 2; ++i) {
                    const Token& t = valueToks[i];
                    good =
                        t.kind == TokenKind::Number && (t.unit == "px" || (t.unit.empty() && t.num == 0.0f));
                    (i == 0 ? v.x : v.y) = t.num;
                }
                if (!good) {
                    error(nameTok, "\"" + nameTok.text + "\" takes two px lengths");
                    continue;
                }
                key.hasMotion = true;
                (nameTok.text == "motion-in" ? key.motionIn : key.motionOut) = v;
                continue;
            }
            const PropInfo* info = findProp(nameTok.text);
            if (info == nullptr) {
                warn(nameTok, "unknown property \"" + nameTok.text + "\"");
                continue;
            }
            std::vector<StyleDecl> expanded;
            size_t sinkSizeBefore = m_sink.all().size();
            const bool parsed =
                parseAndExpandDecl(info, valueToks, m_fileName, line, m_sink, expanded, m_easingPoints);
            if (parsed) {
                for (StyleDecl& d : expanded) {
                    key.decls.push_back(std::move(d));
                }
            }
            recordTransformPart(nameTok, valueToks, false, parsed);
            syncErrorFlag(sinkSizeBefore);
        }
    }

    // `.<effect>.<param>` after `effect` (18.12); `<effect>` is an effect name or a stack index.
    // The lexer reads `.3` as the number 0.3, so an index is recovered from its digit count (the
    // token's column span up to the following '.').
    bool parseEffectAddress(TimelineEffectDecl& fx) {
        if (m_cur.kind == TokenKind::Number) {
            Token numTok = m_cur;
            advance();
            if (!numTok.unit.empty() || m_cur.kind != TokenKind::Dot || m_cur.line != numTok.line ||
                m_cur.col <= numTok.col + 1) {
                error(numTok, "expected effect.<name|index>.<param>");
                return false;
            }
            int digits = m_cur.col - numTok.col - 1;
            float scaled = numTok.num;
            for (int i = 0; i < digits; ++i) {
                scaled *= 10.0f;
            }
            fx.effect = std::to_string(static_cast<int>(scaled + 0.5f));
        } else {
            advance();  // '.'
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected effect.<name|index>.<param>");
                return false;
            }
            fx.effect = m_cur.text;
            advance();
            if (m_cur.kind != TokenKind::Dot) {
                error(m_cur, "expected effect.<name|index>.<param>");
                return false;
            }
        }
        advance();  // '.'
        if (m_cur.kind != TokenKind::Ident) {
            error(m_cur, "expected effect.<name|index>.<param>");
            return false;
        }
        fx.param = m_cur.text;
        advance();
        return true;
    }

    // T-179 (07.8): `@font-fallback <lang> { font-family: A, "B C", D E; }`. The language is an
    // identifier or a string; `font-family` is the only property; a family is a string or a run of
    // identifiers joined by single spaces (CSS's unquoted family names). Every block is kept in
    // source order; the emitter lets a later block for the same language replace an earlier one.
    void parseFontFallback(const Token& atTok, RuleSet& out) {
        if (m_cur.kind != TokenKind::Ident && m_cur.kind != TokenKind::String) {
            error(atTok, "expected a language after @font-fallback");
            recoverToNextRBrace();
            return;
        }
        FontFallbackDef def;
        def.lang = m_cur.text;
        advance();
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after @font-fallback " + def.lang);
            recoverToNextRBrace();
            return;
        }
        advance();
        bool sawFamily = false;
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated @font-fallback block");
                return;
            }
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected property name");
                recoverToNextRBrace();
                return;
            }
            Token nameTok = m_cur;
            advance();
            if (m_cur.kind != TokenKind::Colon) {
                error(nameTok, "expected ':' after property name \"" + nameTok.text + "\"");
                recoverToNextRBrace();
                return;
            }
            advance();
            std::vector<std::string> families;
            std::string current;
            bool valid = true;
            bool trailingComma = false;
            while (m_cur.kind != TokenKind::Semicolon && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::End) {
                if (m_cur.kind == TokenKind::Comma) {
                    if (current.empty()) {
                        valid = false;
                    }
                    families.push_back(std::move(current));
                    current.clear();
                    trailingComma = true;
                } else if (m_cur.kind == TokenKind::String && current.empty()) {
                    current = m_cur.text;
                    trailingComma = false;
                } else if (m_cur.kind == TokenKind::Ident) {
                    if (!current.empty()) {
                        current.push_back(' ');
                    }
                    current += m_cur.text;
                    trailingComma = false;
                } else {
                    valid = false;
                }
                advance();
            }
            if (!current.empty()) {
                families.push_back(std::move(current));
            } else if (trailingComma || families.empty()) {
                valid = false;
            }
            if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            } else if (m_cur.kind != TokenKind::RBrace) {
                error(nameTok, "expected ';' or '}' after declaration");
                recoverToNextRBrace();
                return;
            }
            if (nameTok.text != "font-family") {
                warn(nameTok, "unknown property \"" + nameTok.text + "\" in @font-fallback");
                continue;
            }
            if (!valid) {
                error(nameTok, "invalid value for \"font-family\" in @font-fallback");
                continue;
            }
            def.families = std::move(families);
            sawFamily = true;
        }
        advance();  // '}'
        if (!sawFamily) {
            error(atTok, "@font-fallback " + def.lang + " missing \"font-family\"");
            return;
        }
        out.fontFallbacks.push_back(std::move(def));
    }

    void parseFontFace(RuleSet& out) {
        if (m_cur.kind != TokenKind::LBrace) {
            error(m_cur, "expected '{' after @font-face");
            recoverToNextRBrace();
            return;
        }
        advance();
        FontFaceDef def;
        bool sawFamily = false, sawSrc = false;
        Token startTok = m_cur;
        while (m_cur.kind != TokenKind::RBrace) {
            if (m_cur.kind == TokenKind::End) {
                error(m_cur, "unterminated @font-face block");
                return;
            }
            if (m_cur.kind != TokenKind::Ident) {
                error(m_cur, "expected property name");
                recoverToNextRBrace();
                return;
            }
            Token nameTok = m_cur;
            advance();
            if (m_cur.kind != TokenKind::Colon) {
                error(nameTok, "expected ':' after property name \"" + nameTok.text + "\"");
                recoverToNextRBrace();
                return;
            }
            advance();
            std::vector<Token> valueToks;
            while (m_cur.kind != TokenKind::Semicolon && m_cur.kind != TokenKind::RBrace &&
                   m_cur.kind != TokenKind::End) {
                valueToks.push_back(m_cur);
                advance();
            }
            if (m_cur.kind == TokenKind::Semicolon) {
                advance();
            } else if (m_cur.kind != TokenKind::RBrace) {
                error(nameTok, "expected ';' or '}' after declaration");
                recoverToNextRBrace();
                return;
            }
            if (nameTok.text == "font-family") {
                if (valueToks.size() == 1 && valueToks[0].kind == TokenKind::String) {
                    def.family = valueToks[0].text;
                    sawFamily = true;
                } else if (valueToks.size() == 1 && valueToks[0].kind == TokenKind::Ident) {
                    def.family = valueToks[0].text;
                    sawFamily = true;
                } else {
                    warn(nameTok, "invalid value for \"font-family\"");
                }
            } else if (nameTok.text == "src") {
                if (valueToks.size() == 1 &&
                    (valueToks[0].kind == TokenKind::String || valueToks[0].kind == TokenKind::Ident)) {
                    def.src = valueToks[0].text;
                    sawSrc = true;
                } else {
                    warn(nameTok, "invalid value for \"src\"");
                }
            } else if (nameTok.text == "font-weight") {
                if (valueToks.size() == 1 && valueToks[0].kind == TokenKind::Number &&
                    valueToks[0].unit.empty()) {
                    def.weight = static_cast<uint16_t>(valueToks[0].num);
                } else if (valueToks.size() == 1 && valueToks[0].kind == TokenKind::Ident &&
                           valueToks[0].text == "normal") {
                    def.weight = 400;
                } else if (valueToks.size() == 1 && valueToks[0].kind == TokenKind::Ident &&
                           valueToks[0].text == "bold") {
                    def.weight = 700;
                } else {
                    warn(nameTok, "invalid value for \"font-weight\"");
                }
            } else if (nameTok.text == "font-style") {
                if (valueToks.size() == 1 && valueToks[0].kind == TokenKind::Ident) {
                    if (valueToks[0].text == "italic") {
                        def.italic = true;
                    } else if (valueToks[0].text == "normal") {
                        def.italic = false;
                    } else {
                        warn(nameTok, "invalid value for \"font-style\"");
                    }
                } else {
                    warn(nameTok, "invalid value for \"font-style\"");
                }
            } else {
                warn(nameTok, "unknown property \"" + nameTok.text + "\"");
            }
        }
        advance();  // '}'
        if (!sawFamily) {
            error(startTok, "@font-face missing required \"font-family\"");
            return;
        }
        if (!sawSrc) {
            error(startTok, "@font-face missing required \"src\"");
            return;
        }
        out.fontFaces.push_back(std::move(def));
    }
};

}  // namespace

bool parseXts(std::string_view text, std::string_view fileName, int lineOffset, DiagnosticSink& sink,
              RuleSet& out) {
    Parser parser(text, fileName, lineOffset, sink);
    return parser.parseStylesheet(out);
}

bool parseDeclarations(std::string_view text, std::string_view fileName, int line, DiagnosticSink& sink,
                       std::vector<StyleDecl>& out, std::vector<EasingPoint>* easingPoints) {
    Parser parser(text, fileName, line, sink);
    parser.setEasingPoints(easingPoints);
    return parser.parseDeclarationList(out);
}

// T-172: one `.xtanim.json` key ("<prop>: <value>") parsed with the same rules as an inline
// `@timeline` key. Declared where it is used (compile_document.cpp), like parseAndExpandDecl.
bool parseTimelineKeyDeclarations(std::string_view text, std::string_view fileName, int line,
                                  DiagnosticSink& sink, TimelineKey& key) {
    Parser parser(text, fileName, line, sink);
    return parser.parseTimelineKeyBody(key);
}

// T-172: an easing given as text (a `.xtanim.json` key's third element). T-335: `linear()` needs
// `easingPoints`.
bool parseEasingText(std::string_view text, Easing& out, std::vector<EasingPoint>* easingPoints) {
    Lexer lexer(text);
    std::vector<Token> toks;
    for (Token t = lexer.next(); t.kind != TokenKind::End; t = lexer.next()) {
        toks.push_back(t);
    }
    return !toks.empty() && parseEasingValue(toks, out, nullptr, easingPoints);
}

}  // namespace xt::compile
