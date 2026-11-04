#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "migrate_v2.h"
#include "xt/compile/aliases.h"
#include "xt/compile/canonical_writer.h"
#include "xts/xts_sections.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.10 steps 3, 4, 5 and 7) on CSS text: a version 1
/// sheet split by the section table into layout / style / timelines items, its rule triggers
/// collected, and the anchor formulas rewritten into the shorthands. See migrate_v2.h.

namespace xt::compile::migrate {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

/// The value of a declaration prelude (after the first ':'), trimmed.
std::string_view declarationValue(std::string_view prelude) {
    const size_t colon = prelude.find(':');
    return colon == std::string_view::npos ? std::string_view() : trim(prelude.substr(colon + 1));
}

/// A trigger declaration: `on-<event>: <actions>` / `state-enter|state-exit: <State> <actions>`.
bool triggerOf(std::string_view prelude, std::string& event, std::string& actions) {
    const std::string name = declarationName(prelude);
    std::string_view value = declarationValue(prelude);
    if (startsWith(name, "on-") && name.size() > 3) {
        event = std::string(trim(prelude.substr(0, prelude.find(':')))).substr(3);
    } else if (name == "state-enter" || name == "state-exit") {
        size_t end = 0;
        while (end < value.size() && !isSpace(value[end])) {
            ++end;
        }
        event =
            "state(" + std::string(value.substr(0, end)) + ")." + (name == "state-enter" ? "enter" : "exit");
        value = trim(value.substr(end));
    } else {
        return false;
    }
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front() &&
        value.substr(1, value.size() - 2).find(value.front()) == std::string_view::npos) {
        value = value.substr(1, value.size() - 2);  // A quoted action list (may use ';').
    }
    actions = std::string(value);
    return true;
}

struct Splitter {
    bool anchors = false;
    SplitSheet* out = nullptr;

    void split(const std::vector<CanonicalItem>& in, const std::string& symbol, bool inMedia,
               std::vector<CanonicalItem>& layout, std::vector<CanonicalItem>& style,
               std::vector<CanonicalItem>& timelines) {
        for (const CanonicalItem& item : in) {
            const std::string_view prelude = item.prelude;
            if (startsWith(prelude, "@timeline")) {
                timelines.push_back(item);  // Step 3 (`from` statements included).
                continue;
            }
            if (!item.block) {
                style.push_back(item);
                continue;
            }
            const bool media = startsWith(prelude, "@media");
            const bool symbolBlock = startsWith(prelude, "symbol(");
            if (media || symbolBlock) {
                std::string inner = symbol;
                if (symbolBlock) {
                    const size_t close = prelude.find(')');
                    inner =
                        std::string(trim(prelude.substr(7, close == std::string_view::npos ? 0 : close - 7)));
                }
                CanonicalItem l{{}, item.prelude, true, {}};
                CanonicalItem s{item.comments, item.prelude, true, {}};
                CanonicalItem t{{}, item.prelude, true, {}};
                split(item.children, inner, inMedia || media, l.children, s.children, t.children);
                if (!l.children.empty()) {
                    if (s.children.empty() && t.children.empty()) {
                        l.comments = item.comments;
                    }
                    layout.push_back(std::move(l));
                }
                if (!s.children.empty() || (l.children.empty() && t.children.empty())) {
                    style.push_back(std::move(s));
                }
                if (!t.children.empty()) {
                    if (media) {
                        // `@timeline` inside `@media` is not version 1 syntax either; keep the blocks.
                        timelines.insert(timelines.end(), t.children.begin(), t.children.end());
                    } else {
                        timelines.push_back(std::move(t));
                    }
                }
                continue;
            }
            if (prelude.empty() || prelude.front() == '@') {
                style.push_back(item);  // @keyframes, @font-face, @font-fallback, ...
                continue;
            }
            splitRule(item, symbol, inMedia, layout, style);
        }
    }

    void splitRule(const CanonicalItem& rule, const std::string& symbol, bool inMedia,
                   std::vector<CanonicalItem>& layout, std::vector<CanonicalItem>& style) {
        CanonicalItem l{{}, rule.prelude, true, {}};
        CanonicalItem s{{}, rule.prelude, true, {}};
        bool hadTrigger = false;
        for (const CanonicalItem& decl : rule.children) {
            std::string event;
            std::string actions;
            if (!decl.block && triggerOf(decl.prelude, event, actions)) {
                hadTrigger = true;
                for (const std::string& selector : splitSelectors(rule.prelude)) {
                    out->triggers.push_back(TriggerRule{symbol, selector, event, actions, inMedia});
                }
                continue;
            }
            if (!decl.block && isLayoutDeclaration(decl.prelude)) {
                l.children.push_back(decl);
            } else {
                s.children.push_back(decl);
            }
        }
        if (!l.children.empty() && anchors && reverseAnchors(l.children)) {
            ++out->anchors;
        }
        const bool keepStyle = !s.children.empty() || (l.children.empty() && !hadTrigger);
        if (!l.children.empty() && keepStyle) {
            ++out->rulesSplit;
        }
        if (keepStyle) {
            s.comments = rule.comments;
        } else {
            l.comments = rule.comments;
        }
        if (!l.children.empty()) {
            layout.push_back(std::move(l));
        }
        if (keepStyle) {
            style.push_back(std::move(s));
        }
    }
};

// ---- step 7 --------------------------------------------------------------------------------

/// `calc(<inner>)` -> `<inner>`; empty when `value` is not one calc().
std::string_view calcInner(std::string_view value) {
    if (!startsWith(value, "calc(") || value.back() != ')') {
        return {};
    }
    return trim(value.substr(5, value.size() - 6));
}

/// `1safe-<edge>` -> "0", `calc(1safe-<edge> + <x>)` -> `<x>`; false for anything else.
bool safeEdgeOffset(std::string_view value, std::string_view edge, std::string& offset) {
    const std::string bare = "1safe-" + std::string(edge);
    if (value == bare) {
        offset = "0";
        return true;
    }
    const std::string_view inner = calcInner(value);
    const std::string head = bare + " + ";
    if (!startsWith(inner, head) || inner.size() == head.size()) {
        return false;
    }
    const std::string_view rest = inner.substr(head.size());
    int depth = 0;
    for (char c : rest) {  // One term only (the expansion writes exactly `<edge> + <offset>`).
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        if (depth == 0 && (c == ' ')) {
            return false;
        }
    }
    offset = std::string(rest);
    return true;
}

/// `calc(50% - <size> / 2)` or `calc(50% - <size> / 2 + <x>)` -> offset ("0" or `<x>`).
bool centerOffset(std::string_view value, std::string_view size, std::string& offset) {
    const std::string_view inner = calcInner(value);
    const std::string head = "50% - " + std::string(size) + " / 2";
    if (!startsWith(inner, head)) {
        return false;
    }
    const std::string_view rest = inner.substr(head.size());
    if (rest.empty()) {
        offset = "0";
        return true;
    }
    if (!startsWith(rest, " + ") || rest.size() <= 3 || rest.substr(3).find(' ') != std::string_view::npos) {
        return false;
    }
    offset = std::string(rest.substr(3));
    return true;
}

enum class AxisKind { None, Start, End, Center, Stretch };

struct Axis {
    AxisKind kind = AxisKind::None;
    std::string offset = "0";
    std::string offsetEnd = "0";
    bool usesSize = false;  ///< Center: the size declaration is consumed.
};

/// One axis from its start / end / size declarations (empty string: absent).
Axis readAxis(const std::string* start, const std::string* end, const std::string* size,
              std::string_view startEdge, std::string_view endEdge) {
    Axis axis;
    std::string a;
    std::string b;
    if (start != nullptr && end != nullptr) {
        if (size == nullptr && safeEdgeOffset(*start, startEdge, a) && safeEdgeOffset(*end, endEdge, b)) {
            axis.kind = AxisKind::Stretch;
            axis.offset = a;
            axis.offsetEnd = b;
        }
    } else if (start != nullptr) {
        if (safeEdgeOffset(*start, startEdge, a)) {
            axis.kind = AxisKind::Start;
            axis.offset = a;
        } else if (size != nullptr && centerOffset(*start, *size, a)) {
            axis.kind = AxisKind::Center;
            axis.offset = a;
            axis.usesSize = true;
        }
    } else if (end != nullptr && safeEdgeOffset(*end, endEdge, a)) {
        axis.kind = AxisKind::End;
        axis.offset = a;
    }
    return axis;
}

const char* axisWord(AxisKind kind, bool horizontal) {
    switch (kind) {
        case AxisKind::Start:
            return horizontal ? "left" : "top";
        case AxisKind::End:
            return horizontal ? "right" : "bottom";
        case AxisKind::Center:
            return "center";
        default:
            return "stretch";
    }
}

}  // namespace

std::string declarationName(std::string_view prelude) {
    const size_t colon = prelude.find(':');
    std::string name(trim(colon == std::string_view::npos ? prelude : prelude.substr(0, colon)));
    std::transform(name.begin(), name.end(), name.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; });
    return name;
}

bool isLayoutDeclaration(std::string_view prelude) {
    std::string name = declarationName(prelude);
    if (const NameAlias* alias = findNameAlias(AliasKind::Property, name)) {
        name = alias->replacement;
    }
    return isLayoutSectionProperty(name);
}

SplitSheet splitV1Sheet(std::string_view css, bool anchors) {
    SplitSheet sheet;
    const std::vector<CanonicalItem> items = splitSheetItems(css, &sheet.trailingComments);
    Splitter splitter{anchors, &sheet};
    splitter.split(items, std::string(), false, sheet.layout, sheet.style, sheet.timelines);
    return sheet;
}

bool reverseAnchors(std::vector<CanonicalItem>& decls) {
    // Every declaration once, none !important, none a block.
    std::map<std::string, std::string> values;
    for (const CanonicalItem& decl : decls) {
        if (decl.block || decl.prelude.find("!important") != std::string::npos) {
            return false;
        }
        if (!values.emplace(declarationName(decl.prelude), std::string(declarationValue(decl.prelude)))
                 .second) {
            return false;
        }
    }
    auto value = [&](const char* name) -> const std::string* {
        auto it = values.find(name);
        return it == values.end() ? nullptr : &it->second;
    };
    const std::string* position = value("position");
    if (position == nullptr || *position != "absolute") {
        return false;
    }
    const std::string* width = value("width");
    const std::string* height = value("height");
    const Axis x = readAxis(value("left"), value("right"), width, "left", "right");
    const Axis y = readAxis(value("top"), value("bottom"), height, "top", "bottom");
    if (x.kind == AxisKind::None || y.kind == AxisKind::None) {
        return false;
    }
    // `size` takes both components; a component it would add (`auto`) changes the cascade, so it
    // is written only when both sizes are there and neither axis stretches.
    const bool bothSizes = width != nullptr && height != nullptr;
    const bool stretch = x.kind == AxisKind::Stretch || y.kind == AxisKind::Stretch;
    const bool writeSize = bothSizes && !stretch;
    if ((x.usesSize || y.usesSize) && !writeSize) {
        return false;
    }
    std::vector<CanonicalItem> next;
    bool placed = false;
    for (size_t i = 0; i < decls.size(); ++i) {
        const std::string name = declarationName(decls[i].prelude);
        const bool edge =
            name == "position" || name == "left" || name == "right" || name == "top" || name == "bottom";
        const bool sizeDecl = writeSize && (name == "width" || name == "height");
        if (!edge && !sizeDecl) {
            next.push_back(decls[i]);
            continue;
        }
        if (placed) {
            continue;
        }
        placed = true;
        next.push_back(
            CanonicalItem{decls[i].comments,
                          std::string("anchor: ") + axisWord(x.kind, true) + " " + axisWord(y.kind, false),
                          false,
                          {}});
        next.push_back(CanonicalItem{{}, "offset: " + x.offset + " " + y.offset, false, {}});
        if (stretch) {
            next.push_back(
                CanonicalItem{{},
                              "offset-end: " + (x.kind == AxisKind::Stretch ? x.offsetEnd : x.offset) + " " +
                                  (y.kind == AxisKind::Stretch ? y.offsetEnd : y.offset),
                              false,
                              {}});
        }
        if (writeSize) {
            next.push_back(CanonicalItem{{}, "size: " + *width + " " + *height, false, {}});
        }
    }
    decls = std::move(next);
    return true;
}

std::vector<std::string> splitSelectors(std::string_view list) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i <= list.size(); ++i) {
        const char c = i < list.size() ? list[i] : ',';
        depth += (c == '(' || c == '[') ? 1 : (c == ')' || c == ']') ? -1 : 0;
        if (c == ',' && depth <= 0) {
            const std::string_view one = trim(list.substr(start, i - start));
            if (!one.empty()) {
                out.emplace_back(one);
            }
            start = i + 1;
        }
    }
    return out;
}

std::vector<std::string> splitDeclarations(std::string_view style) {
    std::vector<std::string> out;
    int depth = 0;
    char quote = 0;
    size_t start = 0;
    for (size_t i = 0; i <= style.size(); ++i) {
        const char c = i < style.size() ? style[i] : ';';
        if (quote != 0) {
            quote = c == quote ? 0 : quote;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        if (c == ';' && depth <= 0) {
            const std::string_view one = trim(style.substr(start, i - start));
            if (!one.empty()) {
                out.emplace_back(one);
            }
            start = i + 1;
        }
    }
    return out;
}

namespace {

void writeFragmentItems(std::string& out, int indent, const std::vector<CanonicalItem>& items) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    for (const CanonicalItem& item : items) {
        for (const std::string& comment : item.comments) {
            out += pad + comment + "\n";
        }
        if (!item.block) {
            out += pad + item.prelude + ";\n";
            continue;
        }
        out += pad + item.prelude + " {\n";
        writeFragmentItems(out, indent + 2, item.children);
        out += pad + "}\n";
    }
}

}  // namespace

std::string writeFragment(const std::vector<CanonicalItem>& items, const std::vector<std::string>& trailing) {
    std::string out;
    writeFragmentItems(out, 0, items);
    for (const std::string& comment : trailing) {
        out += comment + "\n";
    }
    return out;
}

}  // namespace xt::compile::migrate
