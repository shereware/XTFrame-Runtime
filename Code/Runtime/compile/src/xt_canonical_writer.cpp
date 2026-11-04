#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ports_section.h"
#include "xt/compile/canonical_writer.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"

/// T-587 (Docs/design/54-source-format-v2.md §54.9): the canonical v2 writer. See
/// xt/compile/canonical_writer.h.

namespace xt::compile {

namespace {

bool isCanonSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/// Whitespace runs outside quotes -> one space; trimmed.
std::string foldCanonText(std::string_view s) {
    std::string out;
    char quote = 0;
    bool pending = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quote == 0 && isCanonSpace(c)) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out.push_back(' ');
            pending = false;
        }
        out.push_back(c);
        if (quote != 0) {
            if (c == '\\' && i + 1 < s.size()) {
                out.push_back(s[++i]);
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        }
    }
    return out;
}

/// The recursive item splitter over one CSS text.
class SheetSplitter {
public:
    explicit SheetSplitter(std::string_view css) : m_css(css) {}

    /// Items up to the end of the text (top level) or up to and including the matching '}'.
    std::vector<CanonicalItem> items(bool topLevel, std::vector<std::string>* trailing) {
        std::vector<CanonicalItem> out;
        std::vector<std::string> comments;
        for (;;) {
            while (m_pos < m_css.size() && isCanonSpace(m_css[m_pos])) {
                ++m_pos;
            }
            if (m_pos >= m_css.size()) {
                break;
            }
            if (m_css.compare(m_pos, 2, "/*") == 0) {
                comments.push_back(readComment());
                continue;
            }
            if (m_css[m_pos] == '}') {
                ++m_pos;
                if (!topLevel) {
                    break;
                }
                continue;  // A stray '}' at the top level: dropped.
            }
            CanonicalItem item;
            item.comments = std::move(comments);
            comments.clear();
            std::string text;
            const char end = readItemText(text);
            item.prelude = foldCanonText(text);
            if (end == '{') {
                item.block = true;
                item.children = items(false, nullptr);
            }
            if (!item.prelude.empty() || item.block) {
                out.push_back(std::move(item));
            }
        }
        if (trailing != nullptr) {
            *trailing = std::move(comments);
        }
        return out;
    }

private:
    std::string_view m_css;
    size_t m_pos = 0;

    std::string readComment() {
        size_t end = m_css.find("*/", m_pos + 2);
        end = end == std::string_view::npos ? m_css.size() : end + 2;
        std::string comment(m_css.substr(m_pos, end - m_pos));
        m_pos = end;
        return comment;
    }

    /// Reads an item's text up to a top-level ';' or '{' (consumed) or '}' / the end (not
    /// consumed); comments inside it are dropped. Returns the terminator (0 at '}' or the end).
    char readItemText(std::string& text) {
        int depth = 0;
        char quote = 0;
        while (m_pos < m_css.size()) {
            const char c = m_css[m_pos];
            if (quote != 0) {
                text.push_back(c);
                if (c == '\\' && m_pos + 1 < m_css.size()) {
                    text.push_back(m_css[++m_pos]);
                } else if (c == quote) {
                    quote = 0;
                }
                ++m_pos;
                continue;
            }
            if (m_css.compare(m_pos, 2, "/*") == 0) {
                readComment();
                text.push_back(' ');
                continue;
            }
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(' || c == '[') {
                ++depth;
            } else if ((c == ')' || c == ']') && depth > 0) {
                --depth;
            } else if (depth == 0 && (c == ';' || c == '{')) {
                ++m_pos;
                return c;
            } else if (depth == 0 && c == '}') {
                return 0;
            }
            text.push_back(c);
            ++m_pos;
        }
        return 0;
    }
};

/// `prelude` split at its top-level commas (a selector list), each part trimmed.
std::vector<std::string> splitSelectorList(std::string_view prelude) {
    std::vector<std::string> parts(1);
    int depth = 0;
    char quote = 0;
    for (char c : prelude) {
        if (quote != 0) {
            quote = c == quote ? 0 : quote;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if ((c == ')' || c == ']') && depth > 0) {
            --depth;
        } else if (c == ',' && depth == 0) {
            parts.emplace_back();
            continue;
        }
        parts.back().push_back(c);
    }
    for (std::string& part : parts) {
        part = foldCanonText(part);
    }
    return parts;
}

std::string escapeCanonAttr(std::string_view s, bool attr) {
    std::string out;
    for (char c : s) {
        if (c == '&') {
            out += "&amp;";
        } else if (c == '<') {
            out += "&lt;";
        } else if (c == '>' && !attr) {
            out += "&gt;";
        } else if (c == '"' && attr) {
            out += "&quot;";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// Sort key of a `<timelines>` item: a timeline's name; other blocks after them, by prelude.
std::string timelineSortKey(const CanonicalItem& item) {
    if (item.prelude.compare(0, 10, "@timeline ") == 0) {
        const size_t end = item.prelude.find(' ', 10);
        return "0" + item.prelude.substr(10, end == std::string::npos ? std::string::npos : end - 10);
    }
    return "1" + item.prelude;
}

void sortTimelineItems(std::vector<CanonicalItem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const CanonicalItem& a, const CanonicalItem& b) {
        return timelineSortKey(a) < timelineSortKey(b);
    });
    for (CanonicalItem& item : items) {
        if (item.block && item.prelude.compare(0, 10, "@timeline ") != 0) {
            sortTimelineItems(item.children);  // `symbol(X) { @timeline ... }`
        }
    }
}

/// Writes `text` with its first line at `indent` and its other lines as they are.
void writeVerbatim(std::string& out, int indent, std::string_view text) {
    out.append(static_cast<size_t>(indent), ' ');
    out.append(text);
    out.push_back('\n');
}

void writeItems(std::string& out, int indent, const std::vector<CanonicalItem>& items) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    for (const CanonicalItem& item : items) {
        for (const std::string& comment : item.comments) {
            writeVerbatim(out, indent, comment);
        }
        if (!item.block) {
            out += pad + item.prelude + ";\n";
            continue;
        }
        if (!item.prelude.empty() && item.prelude[0] != '@') {
            const std::vector<std::string> selectors = splitSelectorList(item.prelude);
            for (size_t i = 0; i < selectors.size(); ++i) {
                out += pad + selectors[i] + (i + 1 < selectors.size() ? ",\n" : " {\n");
            }
        } else {
            out += pad + item.prelude + " {\n";
        }
        writeItems(out, indent + 2, item.children);
        out += pad + "}\n";
    }
}

void writeSections(std::string& out, int indent, const char* tag,
                   const std::vector<CanonicalSection>& sections, bool sortTimelines) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    for (const CanonicalSection& section : sections) {
        if (!section.src.empty()) {
            out += pad + "<" + tag + " src=\"" + escapeCanonAttr(section.src, true) + "\"/>\n";
            continue;
        }
        out += pad + "<" + tag + ">\n";
        if (sortTimelines) {
            std::vector<CanonicalItem> items = section.items;
            sortTimelineItems(items);
            writeItems(out, indent + 2, items);
        } else {
            writeItems(out, indent + 2, section.items);
        }
        for (const std::string& comment : section.trailingComments) {
            writeVerbatim(out, indent + 2, comment);
        }
        out += pad + "</" + tag + ">\n";
    }
}

void writePorts(std::string& out, int indent, const std::vector<CanonicalPort>& ports) {
    if (ports.empty()) {
        return;
    }
    const std::string pad(static_cast<size_t>(indent), ' ');
    out += pad + "<ports>\n";
    for (const CanonicalPort& port : ports) {
        for (const std::string& comment : port.comments) {
            writeVerbatim(out, indent + 2, comment);
        }
        out += pad + "  " + port.name + ": " + port.value + (port.hidden ? " hidden" : "") + "\n";
    }
    out += pad + "</ports>\n";
}

std::vector<CanonicalPort> readCanonicalPorts(const XtPortsSection& section) {
    std::vector<PortsDecl> decls;
    DiagnosticSink scratch;
    parsePortsSection(section.text, section.lineOffset, section.colOffset, "", scratch, decls);
    std::vector<CanonicalPort> ports;
    for (PortsDecl& decl : decls) {
        ports.push_back(CanonicalPort{std::move(decl.name), std::move(decl.value), decl.hidden,
                                      std::move(decl.comments)});
    }
    return ports;
}

void readCanonicalSections(const std::vector<XtSheetSection>& sheets, const std::string& symbol,
                           std::vector<CanonicalSection>& out) {
    for (const XtSheetSection& sheet : sheets) {
        if (sheet.symbol != symbol) {
            continue;
        }
        CanonicalSection section;
        section.src = sheet.src;
        if (sheet.src.empty()) {
            section.items = splitSheetItems(sheet.text, &section.trailingComments);
        }
        out.push_back(std::move(section));
    }
}

std::string sourceOf(std::string_view text, const DomNode& node) {
    if (node.sourceEnd <= node.sourceBegin || node.sourceEnd > text.size()) {
        return {};
    }
    return std::string(text.substr(node.sourceBegin, node.sourceEnd - node.sourceBegin));
}

}  // namespace

std::vector<CanonicalItem> splitSheetItems(std::string_view css, std::vector<std::string>* trailingComments) {
    SheetSplitter splitter(css);
    return splitter.items(true, trailingComments);
}

bool readCanonicalXt(std::string_view text, std::string_view fileName, DiagnosticSink& sink,
                     CanonicalXt& out) {
    out = CanonicalXt{};
    DomTree tree;
    if (!parseXtSymbolLibrary(text, fileName, sink, tree)) {
        return false;
    }
    if (tree.formatVersion < 2) {
        sink.error(std::string(fileName),
                   "the canonical reader needs a version 2 document (<xt version=\"2\">)");
        return false;
    }
    out.title = tree.title;
    out.symbolsSrc = tree.symbolHrefs;
    for (uint32_t def : tree.symbolDefs) {
        const DomNode& node = tree.nodes[def];
        CanonicalSymbol symbol;
        for (const DomAttr& attr : node.attrs) {
            symbol.attrs.emplace_back(attr.name, attr.value);
        }
        const DomAttr* nameAttr = node.attr("name");
        const std::string name = nameAttr != nullptr ? nameAttr->value : std::string();
        for (const XtPortsSection& ports : tree.portSections) {
            if (ports.symbol == name && !name.empty()) {
                symbol.ports = readCanonicalPorts(ports);
            }
        }
        for (uint32_t child : node.children) {
            symbol.elements.push_back(sourceOf(text, tree.nodes[child]));
        }
        readCanonicalSections(tree.layoutSheets, name, symbol.layout);
        readCanonicalSections(tree.styleSheets, name, symbol.style);
        readCanonicalSections(tree.timelineSheets, name, symbol.timelines);
        out.symbols.push_back(std::move(symbol));
    }
    for (const XtPortsSection& ports : tree.portSections) {
        if (ports.symbol.empty()) {
            out.ports = readCanonicalPorts(ports);
        }
    }
    if (tree.body != 0xFFFFFFFFu) {
        out.body = sourceOf(text, tree.nodes[tree.body]);
    }
    readCanonicalSections(tree.layoutSheets, std::string(), out.layout);
    readCanonicalSections(tree.styleSheets, std::string(), out.style);
    readCanonicalSections(tree.timelineSheets, std::string(), out.timelines);
    return true;
}

std::string writeCanonicalXt(const CanonicalXt& doc) {
    std::string out = "<xt version=\"2\">\n";
    if (!doc.title.empty() || !doc.symbolsSrc.empty()) {
        out += "  <head>\n";
        if (!doc.title.empty()) {
            out += "    <title>" + escapeCanonAttr(doc.title, false) + "</title>\n";
        }
        for (const std::string& src : doc.symbolsSrc) {
            out += "    <symbols src=\"" + escapeCanonAttr(src, true) + "\"/>\n";
        }
        out += "  </head>\n";
    }
    for (const CanonicalSymbol& symbol : doc.symbols) {
        out += "\n  <symbol";
        for (const auto& [name, value] : symbol.attrs) {
            out += " " + name + "=\"" + escapeCanonAttr(value, true) + "\"";
        }
        out += ">\n";
        writePorts(out, 4, symbol.ports);
        for (const std::string& element : symbol.elements) {
            writeVerbatim(out, 4, element);
        }
        writeSections(out, 4, "layout", symbol.layout, false);
        writeSections(out, 4, "style", symbol.style, false);
        writeSections(out, 4, "timelines", symbol.timelines, true);
        out += "  </symbol>\n";
    }
    if (!doc.ports.empty()) {
        out += "\n";
        writePorts(out, 2, doc.ports);
    }
    if (!doc.body.empty()) {
        out += "\n";
        writeVerbatim(out, 2, doc.body);
    }
    if (!doc.layout.empty() || !doc.style.empty() || !doc.timelines.empty()) {
        out += "\n";
    }
    writeSections(out, 2, "layout", doc.layout, false);
    writeSections(out, 2, "style", doc.style, false);
    writeSections(out, 2, "timelines", doc.timelines, true);
    out += "</xt>\n";
    return out;
}

}  // namespace xt::compile
