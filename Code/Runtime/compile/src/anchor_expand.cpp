#include "anchor_expand.h"

#include <algorithm>
#include <string>
#include <utility>

#include "style_emit.h"  // selectorMatchesStatically(), staticWinningDecl().
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xts.h"
#include "xt/node.h"  // Display, Position, FlexDirection.
#include "xts/xts_props.h"

/// T-588: the anchor shorthands of 54 §54.5.1 -- see anchor_expand.h.

namespace xt::compile {

namespace {

/// The tokens of `text` (a fixed snippet of the expansion table).
std::vector<Token> lexTokens(std::string_view text) {
    std::vector<Token> out;
    Lexer lexer(text);
    for (Token t = lexer.next(); t.kind != TokenKind::End; t = lexer.next()) {
        out.push_back(std::move(t));
    }
    return out;
}

void append(std::vector<Token>& out, const std::vector<Token>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

bool isWord(const std::vector<Token>& value, std::string_view word) {
    return value.size() == 1 && value[0].kind == TokenKind::Ident && value[0].text == word;
}

/// A single zero number (`0`, `0px`, `0%`): the short forms of the table drop it.
bool isZero(const std::vector<Token>& value) {
    return value.size() == 1 && value[0].kind == TokenKind::Number && value[0].num == 0.0f;
}

/// One axis of the shorthands.
struct Axis {
    const char* start;    ///< `left` / `top`.
    const char* end;      ///< `right` / `bottom`.
    const char* size;     ///< `width` / `height`.
    const char* name;     ///< `x` / `y`.
    const char* sizeArg;  ///< `w` / `h` (diagnostics).
    uint8_t fillBit;
};
constexpr Axis kAxes[2] = {{"left", "right", "width", "x", "w", kAnchorFillX},
                           {"top", "bottom", "height", "y", "h", kAnchorFillY}};

/// `1safe-<edge>` / `calc(1safe-<edge> + <o>)` (safe) or `0` / `<o>` (screen).
std::vector<Token> edgeValue(bool safe, std::string_view edge, const std::vector<Token>& offset) {
    if (!safe) {
        return isZero(offset) ? lexTokens("0") : offset;
    }
    if (isZero(offset)) {
        return lexTokens("1safe-" + std::string(edge));
    }
    std::vector<Token> out = lexTokens("calc(1safe-" + std::string(edge) + " + ");
    append(out, offset);
    append(out, lexTokens(")"));
    return out;
}

/// `calc(50% - <size> / 2)` / `calc(50% - <size> / 2 + <o>)`.
std::vector<Token> centerValue(const std::vector<Token>& size, const std::vector<Token>& offset) {
    std::vector<Token> out = lexTokens("calc(50% - ");
    append(out, size);
    append(out, lexTokens(" / 2"));
    if (!isZero(offset)) {
        append(out, lexTokens(" + "));
        append(out, offset);
    }
    append(out, lexTokens(")"));
    return out;
}

const AnchorShorthandDecl* lastNamed(const std::vector<AnchorShorthandDecl>& decls, std::string_view name) {
    const AnchorShorthandDecl* found = nullptr;
    for (const AnchorShorthandDecl& d : decls) {
        found = d.name == name ? &d : found;
    }
    return found;
}

class Expander {
public:
    Expander(std::string_view fileName, DiagnosticSink& sink) : m_file(fileName), m_sink(sink) {}

    bool run(const std::vector<AnchorShorthandDecl>& decls, std::string_view conflict,
             std::vector<AnchorLonghand>& out) {
        const AnchorShorthandDecl* anchor = lastNamed(decls, "anchor");
        const AnchorShorthandDecl* offset = lastNamed(decls, "offset");
        const AnchorShorthandDecl* offsetEnd = lastNamed(decls, "offset-end");
        const AnchorShorthandDecl* size = lastNamed(decls, "size");
        std::string keys[2];
        bool safe = true;
        if (anchor != nullptr) {
            readAnchor(*anchor, keys, safe);
        }
        checkPair(offset, "offset: <x> <y>, e.g. offset: 16px 0");
        checkPair(offsetEnd, "offset-end: <x> <y>, e.g. offset-end: 16px 0");
        checkPair(size, "size: <w> <h> with a length, auto or fill each, e.g. size: 200px auto");
        if (anchor == nullptr) {
            for (const AnchorShorthandDecl* d : {offset, offsetEnd}) {
                if (d != nullptr) {
                    error(*d, "\"" + d->name + "\" only applies together with \"anchor\" in the same rule",
                          "",
                          "add anchor: <x> <y> (e.g. anchor: left top) to this rule, or write left/top "
                          "directly");
                }
            }
        } else if (!conflict.empty()) {
            error(
                *anchor,
                "\"anchor\" cannot be combined with \"" + std::string(conflict) + "\" in the same rule",
                "layout.anchorConflict",
                "remove \"" + std::string(conflict) +
                    "\": anchor implies position: absolute and sets the edges; move the element with offset");
        }
        if (anchor != nullptr && m_ok) {
            for (int a = 0; a < 2; ++a) {
                checkAxis(*anchor, size, kAxes[a], keys[a], a);
            }
        }
        if (!m_ok) {
            return false;
        }
        if (anchor != nullptr) {
            emit(out, *anchor, "position", lexTokens("absolute"), 0);
            for (int a = 0; a < 2; ++a) {
                const std::vector<Token> zero = lexTokens("0");
                const std::vector<Token>& o =
                    offset != nullptr ? offset->parts[static_cast<size_t>(a)] : zero;
                const std::vector<Token>& oe =
                    offsetEnd != nullptr ? offsetEnd->parts[static_cast<size_t>(a)] : o;
                const Axis& axis = kAxes[a];
                if (keys[a] == axis.start || keys[a] == "stretch") {
                    emit(out, *anchor, axis.start, edgeValue(safe, axis.start, o), 0);
                }
                if (keys[a] == "center") {
                    emit(out, *anchor, axis.start, centerValue(size->parts[static_cast<size_t>(a)], o), 0);
                }
                if (keys[a] == axis.end) {
                    emit(out, *anchor, axis.end, edgeValue(safe, axis.end, o), 0);
                }
                if (keys[a] == "stretch") {
                    emit(out, *anchor, axis.end, edgeValue(safe, axis.end, oe), 0);
                }
            }
        }
        if (size != nullptr) {
            for (int a = 0; a < 2; ++a) {
                const std::vector<Token>& v = size->parts[static_cast<size_t>(a)];
                if (isWord(v, "fill")) {
                    const uint8_t anchored = anchor != nullptr ? kAnchorFillAnchored : 0;
                    emit(out, *size, kAxes[a].size, lexTokens("100%"),
                         static_cast<uint8_t>(kAxes[a].fillBit | anchored));
                } else {
                    emit(out, *size, kAxes[a].size, v, 0);
                }
            }
        }
        return true;
    }

private:
    std::string_view m_file;
    DiagnosticSink& m_sink;
    bool m_ok = true;

    void error(const AnchorShorthandDecl& at, const std::string& message, const char* code,
               const std::string& fix) {
        m_sink.error(std::string(m_file), message, at.line, at.col, code, fix);
        m_ok = false;
    }

    static void emit(std::vector<AnchorLonghand>& out, const AnchorShorthandDecl& from, const char* name,
                     std::vector<Token> value, uint8_t fill) {
        out.push_back(AnchorLonghand{name, std::move(value), from.important, from.line, fill});
    }

    void readAnchor(const AnchorShorthandDecl& anchor, std::string (&keys)[2], bool& safe) {
        const auto keyword = [&](size_t i) {
            return anchor.parts[i].size() == 1 && anchor.parts[i][0].kind == TokenKind::Ident
                       ? anchor.parts[i][0].text
                       : std::string();
        };
        bool ok = anchor.parts.size() == 2 || anchor.parts.size() == 3;
        if (ok) {
            keys[0] = keyword(0);
            keys[1] = keyword(1);
            ok = (keys[0] == "left" || keys[0] == "center" || keys[0] == "right" || keys[0] == "stretch") &&
                 (keys[1] == "top" || keys[1] == "center" || keys[1] == "bottom" || keys[1] == "stretch");
        }
        if (ok && anchor.parts.size() == 3) {
            const std::string edge = keyword(2);
            ok = edge == "safe" || edge == "screen";
            safe = edge != "screen";
        }
        if (!ok) {
            error(anchor,
                  "\"anchor\" expects \"<left|center|right|stretch> <top|center|bottom|stretch> "
                  "[safe|screen]\"",
                  "", "write e.g. anchor: left top, anchor: center bottom or anchor: stretch stretch screen");
        }
    }

    void checkPair(const AnchorShorthandDecl* d, const std::string& example) {
        if (d != nullptr && d->parts.size() != 2) {
            error(*d, "\"" + d->name + "\" expects two values (" + (d->name == "size" ? "w h" : "x y") + ")",
                  "", "write " + example);
        }
    }

    // 54 §54.5.1: `center` needs a length size on its axis; `stretch` takes no size there.
    void checkAxis(const AnchorShorthandDecl& anchor, const AnchorShorthandDecl* size, const Axis& axis,
                   const std::string& key, int index) {
        const std::vector<Token>* v = size != nullptr ? &size->parts[static_cast<size_t>(index)] : nullptr;
        const bool length = v != nullptr && !isWord(*v, "auto") && !isWord(*v, "fill");
        if (key == "center" && !length) {
            error(anchor,
                  "\"anchor: center\" on the " + std::string(axis.name) + " axis needs a length " +
                      axis.size + " in \"size\"",
                  "layout.centerNeedsSize",
                  "give size a length " + std::string(axis.sizeArg) + " (e.g. size: " +
                      (index == 0 ? "200px auto" : "auto 48px") + "), or anchor the axis to an edge");
        }
        if (key == "stretch" && v != nullptr && !isWord(*v, "auto")) {
            error(*size,
                  "a stretched " + std::string(axis.name) + " axis takes its " + axis.size +
                      " from its two edges; \"size\" must be auto or omitted there",
                  "layout.anchorConflict",
                  "write auto for " + std::string(axis.sizeArg) + " in size, or anchor the axis to one edge");
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Stage 2: `size: fill` for what the element is.
// ---------------------------------------------------------------------------------------------

enum class FillContext : uint8_t { Free, Root, StackRow, StackColumn };

const char* contextName(FillContext c) {
    switch (c) {
        case FillContext::Root:
            return "a UI root";
        case FillContext::StackRow:
        case FillContext::StackColumn:
            return "a stack child";
        default:
            return "a free child";
    }
}

bool enumIs(const StyleDecl* d, uint32_t value) {
    return d != nullptr && d->value.kind == StyleValueKind::Enum && d->value.e == value;
}

FillContext fillContextOf(const RuleSet& ruleSet, const DomTree& dom, uint32_t element, bool anchored,
                          std::string_view fileName, std::vector<StyleDecl>& scratch) {
    const uint32_t parent = dom.nodes[element].parent;
    if (parent == dom.body) {
        return FillContext::Root;
    }
    if (anchored || parent >= dom.nodes.size() ||
        enumIs(staticWinningDecl(ruleSet, dom, element, styleprop::Position, fileName, scratch, true),
               static_cast<uint32_t>(Position::Absolute))) {
        return FillContext::Free;
    }
    if (!enumIs(staticWinningDecl(ruleSet, dom, parent, styleprop::Display, fileName, scratch, true),
                static_cast<uint32_t>(Display::Flex))) {
        return FillContext::Free;
    }
    const StyleDecl* dir =
        staticWinningDecl(ruleSet, dom, parent, styleprop::FlexDirection, fileName, scratch, true);
    return enumIs(dir, static_cast<uint32_t>(FlexDirection::Column)) ||
                   enumIs(dir, static_cast<uint32_t>(FlexDirection::ColumnReverse))
               ? FillContext::StackColumn
               : FillContext::StackRow;
}

/// The declarations `text` parses to, with `like`'s importance and line.
void appendParsed(std::string_view text, const StyleDecl& like, std::string_view fileName,
                  std::vector<StyleDecl>& out) {
    DiagnosticSink quiet;  // Fixed, valid snippets.
    std::vector<StyleDecl> parsed;
    parseDeclarations(text, fileName, like.line, quiet, parsed);
    for (StyleDecl& d : parsed) {
        d.important = like.important;
        d.line = like.line;
        out.push_back(std::move(d));
    }
}

}  // namespace

std::vector<std::vector<Token>> splitAnchorComponents(const std::vector<Token>& value) {
    std::vector<std::vector<Token>> parts;
    int depth = 0;
    for (const Token& t : value) {
        if (parts.empty() || (depth == 0 && t.wsBefore)) {
            parts.emplace_back();
        }
        depth += t.kind == TokenKind::LParen ? 1 : (t.kind == TokenKind::RParen && depth > 0 ? -1 : 0);
        parts.back().push_back(t);
    }
    return parts;
}

bool expandAnchorShorthands(const std::vector<AnchorShorthandDecl>& decls, std::string_view conflict,
                            std::string_view fileName, DiagnosticSink& sink,
                            std::vector<AnchorLonghand>& out) {
    out.clear();
    Expander expander(fileName, sink);
    if (!expander.run(decls, conflict, out)) {
        out.clear();
        return false;
    }
    return true;
}

void resolveAnchorFill(RuleSet& ruleSet, const DomTree& dom, std::string_view fileName,
                       DiagnosticSink& sink) {
    std::vector<uint32_t> elements;  // The elements under <body>, found on the first marked rule.
    bool collected = false;
    std::vector<StyleDecl> scratch;
    for (StyleRule& rule : ruleSet.rules) {
        const auto marked = std::find_if(rule.decls.begin(), rule.decls.end(),
                                         [](const StyleDecl& d) { return d.anchorFill != 0; });
        if (marked == rule.decls.end()) {
            continue;
        }
        if (!collected) {
            collected = true;
            std::vector<uint8_t> inBody(dom.nodes.size(), 0);
            for (uint32_t i = 0; i < dom.nodes.size(); ++i) {  // Preorder: a parent comes first.
                const uint32_t parent = dom.nodes[i].parent;
                inBody[i] = (parent == dom.body || (parent < i && inBody[parent] != 0)) ? 1 : 0;
                if (inBody[i] != 0) {
                    elements.push_back(i);
                }
            }
        }
        const bool anchored = (marked->anchorFill & kAnchorFillAnchored) != 0;
        bool found = false;
        bool mixed = false;
        FillContext context = FillContext::Free;
        FillContext other = FillContext::Free;
        for (uint32_t element : elements) {
            const bool matches = std::any_of(
                rule.selectors.begin(), rule.selectors.end(),
                [&](const Selector& sel) { return selectorMatchesStatically(sel, dom, element); });
            if (!matches) {
                continue;
            }
            const FillContext c = fillContextOf(ruleSet, dom, element, anchored, fileName, scratch);
            if (!found) {
                context = c;
                found = true;
            } else if (c != context && !mixed) {
                mixed = true;
                other = c;
            }
        }
        if (mixed) {
            sink.error(
                rule.file.empty() ? std::string(fileName) : rule.file,
                std::string("\"size: fill\" means different things for the elements this rule matches (") +
                    contextName(context) + " and " + contextName(other) + ")" +
                    (rule.instanceSite.empty() ? "" : " (instance " + rule.instanceSite + ")"),
                marked->line, 0, "",
                "give each element its own #id rule, or write width/height (flex-grow, align-self) directly");
            context = FillContext::Free;
        }
        std::vector<StyleDecl> decls;
        decls.reserve(rule.decls.size() + 2);
        for (StyleDecl& d : rule.decls) {
            const uint8_t fill = d.anchorFill;
            d.anchorFill = 0;
            if (fill == 0 || context == FillContext::Free) {
                decls.push_back(std::move(d));
                continue;
            }
            const bool x = (fill & kAnchorFillX) != 0;
            const std::string_view rel = rule.file.empty() ? fileName : std::string_view(rule.file);
            if (context == FillContext::Root) {
                appendParsed(x ? "width: 100vw" : "height: 100vh", d, rel, decls);
            } else if (x == (context == FillContext::StackRow)) {
                appendParsed("flex-grow: 1; flex-basis: 0", d, rel, decls);  // Main axis.
            } else {
                appendParsed("align-self: stretch", d, rel, decls);  // Cross axis.
            }
        }
        rule.decls = std::move(decls);
    }
}

}  // namespace xt::compile
