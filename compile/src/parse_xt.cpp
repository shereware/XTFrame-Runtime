#include "xt/compile/parse_xt.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

/// Hand-written recursive-descent `.xt` parser. See docs/design/05-style-and-css.md §5.1-5.2,
/// docs/tasks/T-21.md.

namespace xt::compile {

namespace {

bool isAsciiAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
bool isAsciiHexDigit(char c) { return isAsciiDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool isNameChar(char c) { return isAsciiAlpha(c) || isAsciiDigit(c) || c == '_' || c == '-'; }
bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

char toLowerChar(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

std::string toLowerCopy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(toLowerChar(c));
    }
    return out;
}

bool iequalsAscii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (toLowerChar(a[i]) != toLowerChar(b[i])) {
            return false;
        }
    }
    return true;
}

/// Collapses runs of whitespace (space/tab/CR/LF) to a single space and trims both ends.
std::string foldWhitespace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    bool any = false;
    for (char c : s) {
        if (isWs(c)) {
            if (any) {
                pendingSpace = true;
            }
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        out.push_back(c);
        any = true;
    }
    return out;
}

/// Encodes a Unicode code point as UTF-8.
std::string utf8Encode(uint32_t cp) {
    std::string out;
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
    return out;
}

/// Tag names recognized by 5.2 (elements) plus head/link/style/title (recognized but produce no
/// DomNode), plus T-171's symbol/use/slot (26.2.1; expanded away by symbol_expand.cpp). Anything
/// else triggers the "unknown tag" rewrite-to-div warning. T-177 adds `repeat` (27.3.4).
constexpr std::array<std::string_view, 40> kKnownTags = {
    "xt", "head", "body", "div", "p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "li", "section", "span",
    "label", "b", "i", "img", "anim", "button", "input", "scroll", "link", "style", "title", "camera",
    "symbol", "use", "slot", "a", "glyph",      // T-178: inline markup of rich text (07.8).
    "repeat", "video", "sequence", "external",  // T-191: external content (25.3.5b).
    "svg",                                      // T-190: inline vector content (12 §12.2.1
                                                // subset), kept as raw source.
    "particles",                                // T-202: particle emitter (03 §3.12.5).
    "u", "s"};                                  // T-324: underline / line-through run markup.

/// T-178: U+FFFC OBJECT REPLACEMENT CHARACTER, the text of an inline object's run.
constexpr std::string_view kObjectReplacement = "\xEF\xBF\xBC";

/// T-178: whitespace folding (foldWhitespace's rule) that also maps every source byte offset to
/// its folded offset: `map[i]` is where byte `i` (or, for whitespace, the next kept byte) lands,
/// `map[s.size()]` the folded length.
std::string foldWhitespaceMapped(std::string_view s, std::vector<uint32_t>& map) {
    std::string out;
    out.reserve(s.size());
    map.assign(s.size() + 1, 0);
    bool pendingSpace = false;
    bool any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (isWs(c)) {
            if (any) {
                pendingSpace = true;
            }
            map[i] = static_cast<uint32_t>(out.size());
            continue;
        }
        if (pendingSpace) {
            out.push_back(' ');
            pendingSpace = false;
        }
        map[i] = static_cast<uint32_t>(out.size());
        out.push_back(c);
        any = true;
    }
    map[s.size()] = static_cast<uint32_t>(out.size());
    for (uint32_t& m : map) {
        if (m > out.size()) {
            m = static_cast<uint32_t>(out.size());
        }
    }
    return out;
}

/// T-178: the tags that may appear inside rich text (`img` only with an `inline` attribute).
bool isInlineMarkupTag(std::string_view tag) {
    return tag == "span" || tag == "b" || tag == "i" || tag == "a" || tag == "u" || tag == "s";
}

/// T-171: attribute names may carry a `prefix:` (26.2.1's `slot:`/`prop:`/`attr:`/`variant:`).
/// Past the ':' the name may also contain '.' and '/' (`prop:Ic/Img.color`), and its case is kept
/// (it names internal ids and variant properties); only the prefix is lower-cased.
bool isPrefixedAttrNameChar(char c) { return isNameChar(c) || c == '.' || c == '/'; }

bool isKnownTag(std::string_view lower) {
    for (std::string_view t : kKnownTags) {
        if (t == lower) {
            return true;
        }
    }
    return false;
}

/// Decodes a self-contained string (used for <title> content, which has no live cursor). Unknown
/// entities warn at a single approximate position (the element's start), which is an accepted
/// simplification since <title> content is short, plain text in practice.
std::string decodeEntitiesStatic(std::string_view raw, const std::string& file, int line, int col,
                                 DiagnosticSink& sink) {
    std::string out;
    out.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            ++i;
            continue;
        }
        size_t p = i + 1;
        bool numeric = false;
        bool hex = false;
        if (p < raw.size() && raw[p] == '#') {
            numeric = true;
            ++p;
            if (p < raw.size() && (raw[p] == 'x' || raw[p] == 'X')) {
                hex = true;
                ++p;
            }
        }
        size_t bodyBegin = p;
        while (p < raw.size() && (p - bodyBegin) < 16) {
            char c = raw[p];
            bool ok = numeric ? (hex ? isAsciiHexDigit(c) : isAsciiDigit(c)) : isAsciiAlpha(c);
            if (!ok) {
                break;
            }
            ++p;
        }
        if (p < raw.size() && raw[p] == ';' && p > bodyBegin) {
            std::string_view body = raw.substr(bodyBegin, p - bodyBegin);
            bool known = true;
            std::string decoded;
            if (numeric) {
                uint32_t val = 0;
                bool overflow = false;
                for (char c : body) {
                    uint32_t digit = isAsciiDigit(c) ? static_cast<uint32_t>(c - '0')
                                                     : static_cast<uint32_t>(toLowerChar(c) - 'a' + 10);
                    uint32_t base = hex ? 16u : 10u;
                    if (val > (0xFFFFFFFFu - digit) / base) {
                        overflow = true;
                    }
                    val = val * base + digit;
                }
                if (overflow || val == 0 || val > 0x10FFFFu || (val >= 0xD800u && val <= 0xDFFFu)) {
                    known = false;
                } else {
                    decoded = utf8Encode(val);
                }
            } else {
                if (body == "lt") {
                    decoded = "<";
                } else if (body == "gt") {
                    decoded = ">";
                } else if (body == "amp") {
                    decoded = "&";
                } else if (body == "quot") {
                    decoded = "\"";
                } else if (body == "apos") {
                    decoded = "'";
                } else {
                    known = false;
                }
            }
            if (known) {
                out += decoded;
            } else {
                sink.warn(file, "unknown entity &" + std::string(body) + ";", line, col);
                out.push_back('&');
                out += body;
                out.push_back(';');
            }
            i = p + 1;
            continue;
        }
        // Not a recognizable entity attempt: treat '&' as a literal character.
        out.push_back('&');
        ++i;
    }
    return out;
}

/// Recursive-descent parser over one `.xt` document.
class Parser {
public:
    Parser(std::string_view text, std::string_view fileName, DiagnosticSink& sink, DomTree& tree,
           bool requireBody)
        : m_text(text), m_file(fileName), m_sink(sink), m_tree(tree), m_requireBody(requireBody) {}

    bool parse() {
        skipBom();
        skipMisc();
        if (eof()) {
            error("unexpected end of file, expected root element <xt>", m_line, m_col);
            return !m_hadError;
        }
        if (peek() != '<' || !isAsciiAlpha(peek(1))) {
            error("expected root element <xt>", m_line, m_col);
            return !m_hadError;
        }
        int rootLine = m_line;
        int rootCol = m_col;
        advance();  // consume '<'
        std::string name;
        std::vector<DomAttr> attrs;
        bool selfClosed = false;
        if (!parseStartTagRest(name, attrs, selfClosed, rootLine, rootCol)) {
            return !m_hadError;
        }
        std::string lower = toLowerCopy(name);
        if (lower != "xt") {
            error("root element must be <xt>", rootLine, rootCol);
            return !m_hadError;
        }
        // T-406 (43 §43.2): `<xt version>`, missing = 1, newer than kXtFormatVersion refused.
        for (const DomAttr& attr : attrs) {
            if (toLowerCopy(attr.name) != "version") {
                continue;
            }
            const std::string& text = attr.value;
            bool digits = !text.empty() && text.size() <= 7;
            for (char c : text) {
                digits = digits && c >= '0' && c <= '9';
            }
            const long version = digits ? std::strtol(text.c_str(), nullptr, 10) : 0;
            if (version < 1) {
                error("<xt version=\"" + text + "\"> is not a positive integer", attr.line, attr.col);
                return !m_hadError;
            }
            if (version > kXtFormatVersion) {
                error(".xt format version " + text + " is newer than this XTFrame supports (" +
                          std::to_string(kXtFormatVersion) + "); a newer XTFrame is needed",
                      attr.line, attr.col);
                return !m_hadError;
            }
        }
        DomNode root;
        root.tag = "xt";
        root.attrs = std::move(attrs);
        root.line = rootLine;
        root.col = rootCol;
        m_tree.nodes.push_back(std::move(root));
        m_depth = 1;
        if (!selfClosed) {
            parseGenericContent(0, "xt", rootLine, rootCol, /*isRoot=*/true);
        }
        if (m_requireBody && !m_aborted && m_tree.body == 0xFFFFFFFFu) {
            error("missing <body> element", rootLine, rootCol);
        }
        return !m_hadError;
    }

private:
    std::string_view m_text;
    size_t m_pos = 0;
    int m_line = 1;
    int m_col = 1;
    std::string m_file;
    DiagnosticSink& m_sink;
    DomTree& m_tree;
    int m_depth = 0;
    bool m_aborted = false;  // T-257: the nesting limit was hit; stop parsing.
    bool m_hadError = false;
    bool m_requireBody = true;

    bool eof() const { return m_pos >= m_text.size(); }

    char peek(size_t off = 0) const {
        size_t p = m_pos + off;
        return p < m_text.size() ? m_text[p] : '\0';
    }

    void advance() {
        if (eof()) {
            return;
        }
        char c = m_text[m_pos];
        ++m_pos;
        if (c == '\n') {
            ++m_line;
            m_col = 1;
        } else {
            ++m_col;
        }
    }

    void error(const std::string& msg, int line, int col) {
        m_hadError = true;
        m_sink.error(m_file, msg, line, col);
    }

    void warn(const std::string& msg, int line, int col) { m_sink.warn(m_file, msg, line, col); }

    /// Returns false (after one error) once the nesting passes kMaxXtDepth: the parse is then
    /// aborted, and every content loop stops without descending further (T-257).
    bool checkDepth(int line, int col) {
        if (m_depth > kMaxXtDepth) {
            if (!m_aborted) {
                m_aborted = true;
                error("maximum nesting depth (" + std::to_string(kMaxXtDepth) + ") exceeded", line, col);
            }
            return false;
        }
        return !m_aborted;
    }

    void skipBom() {
        if (m_text.size() >= 3 && static_cast<unsigned char>(m_text[0]) == 0xEFu &&
            static_cast<unsigned char>(m_text[1]) == 0xBBu &&
            static_cast<unsigned char>(m_text[2]) == 0xBFu) {
            advance();
            advance();
            advance();
        }
    }

    bool startsWith(std::string_view lit) const {
        if (m_pos + lit.size() > m_text.size()) {
            return false;
        }
        return m_text.compare(m_pos, lit.size(), lit) == 0;
    }

    void consumeLiteral(std::string_view lit) {
        for (size_t i = 0; i < lit.size(); ++i) {
            advance();
        }
    }

    /// Consumes "<!--" ... "-->"; comments may span multiple lines and contain '>'.
    void skipComment() {
        int startLine = m_line;
        int startCol = m_col;
        consumeLiteral("<!--");
        while (!startsWith("-->")) {
            if (eof()) {
                error("unexpected end of file in comment", startLine, startCol);
                return;
            }
            advance();
        }
        consumeLiteral("-->");
    }

    /// Consumes a `<!DOCTYPE ...>` or `<?xml ...?>` declaration and warns that it was ignored.
    void skipDeclaration() {
        int startLine = m_line;
        int startCol = m_col;
        bool question = peek(1) == '?';
        advance();  // '<'
        advance();  // '!' or '?'
        std::string_view terminator = question ? std::string_view("?>") : std::string_view(">");
        while (!startsWith(terminator)) {
            if (eof()) {
                break;
            }
            advance();
        }
        if (startsWith(terminator)) {
            consumeLiteral(terminator);
        }
        warn("ignored declaration", startLine, startCol);
    }

    /// Skips whitespace/comments/declarations before the root start tag.
    void skipMisc() {
        for (;;) {
            if (eof()) {
                return;
            }
            char c = peek();
            if (isWs(c)) {
                advance();
                continue;
            }
            if (startsWith("<!--")) {
                skipComment();
                continue;
            }
            if (c == '<' && (peek(1) == '!' || peek(1) == '?')) {
                skipDeclaration();
                continue;
            }
            return;
        }
    }

    bool readTagName(std::string& out) {
        if (eof() || !isAsciiAlpha(peek())) {
            return false;
        }
        out.clear();
        while (!eof() && isNameChar(peek())) {
            out.push_back(peek());
            advance();
        }
        return true;
    }

    void skipInlineWs() {
        while (!eof() && isWs(peek())) {
            advance();
        }
    }

    /// Decodes one entity at the current position (which must be '&'), advancing past it. Falls
    /// back to a literal '&' if the text at the cursor is not a well-formed entity reference.
    std::string consumeEntity() {
        int startLine = m_line;
        int startCol = m_col;
        size_t p = m_pos + 1;
        bool numeric = false;
        bool hex = false;
        if (p < m_text.size() && m_text[p] == '#') {
            numeric = true;
            ++p;
            if (p < m_text.size() && (m_text[p] == 'x' || m_text[p] == 'X')) {
                hex = true;
                ++p;
            }
        }
        size_t bodyBegin = p;
        while (p < m_text.size() && (p - bodyBegin) < 16) {
            char c = m_text[p];
            bool ok = numeric ? (hex ? isAsciiHexDigit(c) : isAsciiDigit(c)) : isAsciiAlpha(c);
            if (!ok) {
                break;
            }
            ++p;
        }
        if (p < m_text.size() && m_text[p] == ';' && p > bodyBegin) {
            std::string_view body = m_text.substr(bodyBegin, p - bodyBegin);
            size_t entityLen = p + 1 - m_pos;
            bool known = true;
            std::string decoded;
            if (numeric) {
                uint32_t val = 0;
                bool overflow = false;
                for (char c : body) {
                    uint32_t digit = isAsciiDigit(c) ? static_cast<uint32_t>(c - '0')
                                                     : static_cast<uint32_t>(toLowerChar(c) - 'a' + 10);
                    uint32_t base = hex ? 16u : 10u;
                    if (val > (0xFFFFFFFFu - digit) / base) {
                        overflow = true;
                    }
                    val = val * base + digit;
                }
                if (overflow || val == 0 || val > 0x10FFFFu || (val >= 0xD800u && val <= 0xDFFFu)) {
                    known = false;
                } else {
                    decoded = utf8Encode(val);
                }
            } else {
                if (body == "lt") {
                    decoded = "<";
                } else if (body == "gt") {
                    decoded = ">";
                } else if (body == "amp") {
                    decoded = "&";
                } else if (body == "quot") {
                    decoded = "\"";
                } else if (body == "apos") {
                    decoded = "'";
                } else {
                    known = false;
                }
            }
            for (size_t i = 0; i < entityLen; ++i) {
                advance();
            }
            if (!known) {
                warn("unknown entity &" + std::string(body) + ";", startLine, startCol);
                return "&" + std::string(body) + ";";
            }
            return decoded;
        }
        advance();  // consume just '&'
        return "&";
    }

    /// Reads the rest of a start tag (name already consumed at [nameLine,nameCol], cursor at the
    /// first name character) up to and including '>' or "/>".
    bool parseStartTagRest(std::string& name, std::vector<DomAttr>& attrs, bool& selfClosed, int nameLine,
                           int nameCol) {
        if (!readTagName(name)) {
            error("invalid tag name", nameLine, nameCol);
            return false;
        }
        for (;;) {
            skipInlineWs();
            if (eof()) {
                error("unexpected end of file inside tag <" + name + ">", nameLine, nameCol);
                return false;
            }
            char c = peek();
            if (c == '/') {
                advance();
                if (!eof() && peek() == '>') {
                    advance();
                    selfClosed = true;
                    return true;
                }
                error("malformed tag <" + name + ">", nameLine, nameCol);
                return false;
            }
            if (c == '>') {
                advance();
                selfClosed = false;
                return true;
            }
            int attrLine = m_line;
            int attrCol = m_col;
            std::string attrName;
            if (!readTagName(attrName)) {
                error("invalid attribute name in tag <" + name + ">", attrLine, attrCol);
                return false;
            }
            attrName = toLowerCopy(attrName);
            if (!eof() && peek() == ':') {
                // T-174: an `on:` event name may carry `(<arg>)` (`on:key(Enter)`,
                // `on:timeline(intro).end`, `on:state(Low).enter`).
                const bool isTrigger = attrName == "on";
                attrName.push_back(':');
                advance();
                while (!eof() &&
                       (isPrefixedAttrNameChar(peek()) || (isTrigger && (peek() == '(' || peek() == ')')))) {
                    attrName.push_back(peek());
                    advance();
                }
            }
            skipInlineWs();
            if ((attrName == "inline" || attrName == "virtualize" || attrName == "loop" ||
                 attrName == "autoplay" || attrName == "sync" || attrName == "draggable" ||
                 attrName == "drop-target" || attrName == "autofocus" || attrName == "focus-trap" ||
                 attrName == "editor-only") &&
                (eof() || peek() != '=')) {
                // T-191 (5.9.1): `<video loop autoplay sync>` / `<sequence loop sync>` flags too.
                // T-178 (5.9.1 `<img inline>`) and T-177 (`<repeat virtualize>`): bare attributes.
                // T-194 (5.9.1): `draggable`, `drop-target`, `autofocus`; T-201: `focus-trap`.
                // T-342 (03 §3.13): `editor-only`.
                attrs.push_back(DomAttr{attrName, std::string(), attrLine, attrCol});
                continue;
            }
            if ((attrName == "expose:false" || attrName == "expose:true") && (eof() || peek() != '=')) {
                // T-175 (27.3.1): the valueless `expose:false` is `expose="false"`.
                attrs.push_back(DomAttr{"expose", attrName.substr(7), attrLine, attrCol});
                continue;
            }
            if (eof() || peek() != '=') {
                error("expected '=' after attribute \"" + attrName + "\"", attrLine, attrCol);
                return false;
            }
            advance();
            skipInlineWs();
            if (eof() || (peek() != '"' && peek() != '\'')) {
                error("attribute value must be quoted", attrLine, attrCol);
                return false;
            }
            char quote = peek();
            advance();
            std::string value;
            for (;;) {
                if (eof()) {
                    error("unexpected end of file in attribute value", attrLine, attrCol);
                    return false;
                }
                char vc = peek();
                if (vc == quote) {
                    advance();
                    break;
                }
                if (vc == '&') {
                    value += consumeEntity();
                    continue;
                }
                value.push_back(vc);
                advance();
            }
            bool dup = false;
            for (DomAttr& a : attrs) {
                if (a.name == attrName) {
                    warn("duplicate attribute \"" + attrName + "\"", attrLine, attrCol);
                    a.value = value;
                    a.line = attrLine;
                    a.col = attrCol;
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                attrs.push_back(DomAttr{attrName, value, attrLine, attrCol});
            }
        }
    }

    /// Scans raw (undecoded) content up to a literal case-insensitive `</tagLower>` (whitespace
    /// allowed before '>'), consuming the closing tag. Used for <style>/<link>/<title> content,
    /// which must not be re-tokenized as markup (e.g. CSS child combinator '>').
    std::string rawScanUntilClosingTag(std::string_view tagLower, int startLine, int startCol) {
        std::string content;
        for (;;) {
            if (eof()) {
                error("unexpected end of file, unclosed tag <" + std::string(tagLower) + ">", startLine,
                      startCol);
                return content;
            }
            if (peek() == '<' && peek(1) == '/') {
                size_t save = m_pos;
                int saveLine = m_line;
                int saveCol = m_col;
                advance();
                advance();
                std::string name;
                readTagName(name);
                skipInlineWs();
                if (!eof() && peek() == '>' && iequalsAscii(name, tagLower)) {
                    advance();
                    return content;
                }
                m_pos = save;
                m_line = saveLine;
                m_col = saveCol;
            }
            content.push_back(peek());
            advance();
        }
    }

    /// Skips (discards) an already-opened element's content, still tracking nesting/comments/
    /// declarations and reporting a mismatched-closing-tag error if one occurs. Used for tags
    /// that are structurally illegal in <head> (their content is not represented in the tree).
    void skipElementContent(const std::string& tagLower, int startLine, int startCol) {
        for (;;) {
            if (m_aborted) {
                return;
            }
            if (eof()) {
                error("unexpected end of file, unclosed tag <" + tagLower + ">", startLine, startCol);
                return;
            }
            if (peek() == '<') {
                if (startsWith("<!--")) {
                    skipComment();
                    continue;
                }
                if (peek(1) == '!' || peek(1) == '?') {
                    skipDeclaration();
                    continue;
                }
                if (peek(1) == '/') {
                    int closeLine = m_line;
                    int closeCol = m_col;
                    advance();
                    advance();
                    std::string closeName;
                    readTagName(closeName);
                    skipInlineWs();
                    if (!eof() && peek() == '>') {
                        advance();
                    }
                    std::string closeLower = toLowerCopy(closeName);
                    if (closeLower != tagLower) {
                        error("mismatched closing tag </" + closeLower + ">, expected </" + tagLower + ">",
                              closeLine, closeCol);
                    }
                    return;
                }
                int childLine = m_line;
                int childCol = m_col;
                advance();  // '<'
                std::string name;
                std::vector<DomAttr> attrs;
                bool selfClosed = false;
                if (!parseStartTagRest(name, attrs, selfClosed, childLine, childCol)) {
                    return;
                }
                if (!selfClosed) {
                    ++m_depth;
                    if (checkDepth(childLine, childCol)) {
                        skipElementContent(toLowerCopy(name), childLine, childCol);
                    }
                    --m_depth;
                }
            } else {
                advance();
            }
        }
    }

    void handleLink(std::vector<DomAttr>&& attrs, bool selfClosed, int line, int col) {
        if (!selfClosed) {
            rawScanUntilClosingTag("link", line, col);
        }
        const DomAttr* href = nullptr;
        for (const DomAttr& a : attrs) {
            if (a.name == "href") {
                href = &a;
                break;
            }
        }
        const DomAttr* rel = nullptr;
        for (const DomAttr& a : attrs) {
            if (a.name == "rel") {
                rel = &a;
                break;
            }
        }
        if (href != nullptr && rel != nullptr && iequalsAscii(rel->value, "symbols")) {
            m_tree.symbolHrefs.push_back(href->value);  // T-171: a symbol library, not a stylesheet.
        } else if (href != nullptr) {
            m_tree.linkHrefs.push_back(href->value);
        } else {
            warn("<link> missing href attribute, ignored", line, col);
        }
    }

    void handleStyle(bool selfClosed, int line, int col) {
        int contentLine = m_line;
        if (selfClosed) {
            m_tree.inlineStyles.push_back({std::string(), contentLine});
            return;
        }
        std::string content = rawScanUntilClosingTag("style", line, col);
        m_tree.inlineStyles.push_back({std::move(content), contentLine});
    }

    void handleTitle(bool selfClosed, int line, int col) {
        if (selfClosed) {
            m_tree.title.clear();
            return;
        }
        std::string raw = rawScanUntilClosingTag("title", line, col);
        std::string decoded = decodeEntitiesStatic(raw, m_file, line, col, m_sink);
        m_tree.title = foldWhitespace(decoded);
    }

    void parseHeadContent(int headLine, int headCol) {
        ++m_depth;
        checkDepth(headLine, headCol);
        for (;;) {
            if (m_aborted) {
                break;
            }
            if (eof()) {
                error("unexpected end of file, unclosed tag <head>", headLine, headCol);
                break;
            }
            if (peek() == '<') {
                if (startsWith("<!--")) {
                    skipComment();
                    continue;
                }
                if (peek(1) == '!' || peek(1) == '?') {
                    skipDeclaration();
                    continue;
                }
                if (peek(1) == '/') {
                    int closeLine = m_line;
                    int closeCol = m_col;
                    advance();
                    advance();
                    std::string closeName;
                    readTagName(closeName);
                    skipInlineWs();
                    if (!eof() && peek() == '>') {
                        advance();
                    }
                    std::string closeLower = toLowerCopy(closeName);
                    if (closeLower != "head") {
                        error("mismatched closing tag </" + closeLower + ">, expected </head>", closeLine,
                              closeCol);
                    }
                    break;
                }
                int line = m_line;
                int col = m_col;
                advance();  // '<'
                std::string name;
                std::vector<DomAttr> attrs;
                bool selfClosed = false;
                if (!parseStartTagRest(name, attrs, selfClosed, line, col)) {
                    break;
                }
                std::string lower = toLowerCopy(name);
                if (lower == "link") {
                    handleLink(std::move(attrs), selfClosed, line, col);
                } else if (lower == "style") {
                    handleStyle(selfClosed, line, col);
                } else if (lower == "title") {
                    handleTitle(selfClosed, line, col);
                } else if (lower == "symbol") {
                    // T-171: a <symbol> definition in <head> is kept (detached, no parent) for
                    // symbol_expand.cpp; it never becomes a document element itself.
                    uint32_t index = parseElement(name, std::move(attrs), selfClosed, line, col);
                    m_tree.symbolDefs.push_back(index);
                } else {
                    warn("illegal tag <" + lower + "> in <head>, ignored", line, col);
                    if (!selfClosed) {
                        ++m_depth;
                        if (checkDepth(line, col)) {
                            skipElementContent(lower, line, col);
                        }
                        --m_depth;
                    }
                }
            } else {
                // Stray text directly inside <head> is discarded; <head> never carries text.
                advance();
            }
        }
        --m_depth;
    }

    /// T-190 (11 §11.9, 12 §12.2.1): an inline `<svg>` is one element whose content is SVG, not
    /// markup: its source is kept verbatim for parseSvg(). The XML start tag gets every attribute
    /// except the element-level id/class/style/sync. Returns the new node's index.
    uint32_t parseInlineSvg(DomNode&& node, bool selfClosed, int line, int col) {
        uint32_t index = static_cast<uint32_t>(m_tree.nodes.size());
        std::string source = "<svg";
        for (const DomAttr& a : node.attrs) {
            if (a.name == "id" || a.name == "class" || a.name == "style" || a.name == "sync") {
                continue;
            }
            // Attribute names were lower-cased by the .xt tokenizer; restore the camelCase SVG
            // attributes parseSvg looks up by exact name.
            std::string_view svgName = a.name;
            if (svgName == "viewbox") {
                svgName = "viewBox";
            } else if (svgName == "preserveaspectratio") {
                svgName = "preserveAspectRatio";
            }
            source += " ";
            source += svgName;
            source += "=\"";
            for (char c : a.value) {
                if (c == '&') {
                    source += "&amp;";
                } else if (c == '"') {
                    source += "&quot;";
                } else if (c == '<') {
                    source += "&lt;";
                } else {
                    source.push_back(c);
                }
            }
            source += "\"";
        }
        source += ">";
        if (!selfClosed) {
            ++m_depth;
            if (checkDepth(line, col)) {
                source += rawScanUntilClosingTag("svg", line, col);
            }
            --m_depth;
        }
        source += "</svg>";
        node.svgSource = std::move(source);
        node.svgLine = line;
        m_tree.nodes.push_back(std::move(node));
        return index;
    }

    /// Parses one element (start tag already consumed as name/attrs/selfClosed at [line,col]),
    /// applying the unknown-tag-to-div rewrite, pushes it into the tree and (if not self-closed)
    /// recurses into its content. Returns the new node's index.
    uint32_t parseElement(const std::string& originalName, std::vector<DomAttr>&& attrs, bool selfClosed,
                          int line, int col) {
        std::string lower = toLowerCopy(originalName);
        std::string finalTag = lower;
        if (!isKnownTag(lower)) {
            warn("unknown tag <" + lower + ">, treated as div", line, col);
            attrs.push_back(DomAttr{"data-xt-tag", originalName, line, col});
            finalTag = "div";
        }
        DomNode node;
        node.tag = finalTag;
        node.attrs = std::move(attrs);
        node.line = line;
        node.col = col;
        uint32_t index = static_cast<uint32_t>(m_tree.nodes.size());
        if (finalTag == "svg") {
            // Kept out of line: this function recurses per nesting level (T-190).
            return parseInlineSvg(std::move(node), selfClosed, line, col);
        }
        m_tree.nodes.push_back(std::move(node));
        if (!selfClosed) {
            ++m_depth;
            if (checkDepth(line, col)) {
                parseGenericContent(index, lower, line, col, /*isRoot=*/false);
            }
            --m_depth;
        }
        if (finalTag == "glyph") {
            // T-178 (5.9.1 "行內或獨立"): a `<glyph>` element's own text is its inline glyph, so
            // a standalone one lays out and draws exactly like one inside rich text.
            DomNode& glyph = m_tree.nodes[index];
            if (!glyph.children.empty() || glyph.hasText) {
                warn("<glyph> content ignored", line, col);
                glyph.children.clear();
                glyph.rawChildPos.clear();
                truncateNodes(static_cast<size_t>(index) + 1);
            }
            const DomAttr* key = glyph.attr("key");
            if (key == nullptr || key->value.empty()) {
                warn("<glyph> without key", line, col);
            }
            DomTextRun run;
            run.start = 0;
            run.end = static_cast<uint32_t>(kObjectReplacement.size());
            run.inlineKind = DomInlineKind::Glyph;
            run.inlineRef = key != nullptr ? key->value : std::string();
            if (const DomAttr* h = glyph.attr("height")) {
                run.inlineHeight = h->value;
            }
            run.line = line;
            run.col = col;
            glyph.text = std::string(kObjectReplacement);
            glyph.hasText = true;
            glyph.textRuns.assign(1, run);
        }
        return index;
    }

    /// T-178: true when every child of `nodeIndex` (recursively) is inline markup: `<span>`,
    /// `<b>`, `<i>`, `<a>`, `<glyph>` or `<img inline>`.
    bool allChildrenInline(uint32_t nodeIndex) const {
        for (uint32_t child : m_tree.nodes[nodeIndex].children) {
            const DomNode& c = m_tree.nodes[child];
            if (c.tag == "glyph") {
                continue;
            }
            if (c.tag == "img") {
                if (c.attr("inline") == nullptr || !c.children.empty()) {
                    return false;
                }
                continue;
            }
            if (!isInlineMarkupTag(c.tag) || !allChildrenInline(child)) {
                return false;
            }
        }
        return true;
    }

    /// T-178: the inherited context of a text segment while flattening inline markup.
    struct RunContext {
        std::string style;
        std::vector<std::string> classes;
        int32_t link = -1;
        int line = 0;
        int col = 0;
        bool styled() const { return !style.empty() || !classes.empty() || link >= 0; }
    };

    /// T-178: appends `text` under `ctx` to the flattened raw string (a run when styled).
    static void appendSegment(std::string_view text, const RunContext& ctx, std::string& raw,
                              std::vector<DomTextRun>& runs) {
        if (text.empty()) {
            return;
        }
        if (ctx.styled()) {
            DomTextRun run;
            run.start = static_cast<uint32_t>(raw.size());
            run.end = static_cast<uint32_t>(raw.size() + text.size());
            run.style = ctx.style;
            run.classes = ctx.classes;
            run.link = ctx.link;
            run.line = ctx.line;
            run.col = ctx.col;
            runs.push_back(std::move(run));
        }
        raw.append(text);
    }

    /// T-178: the context `child` (an inline element) gives its content.
    RunContext childContext(const RunContext& parent, const DomNode& child,
                            std::vector<std::string>& links) const {
        RunContext ctx = parent;
        ctx.line = child.line;
        ctx.col = child.col;
        if (child.tag == "b") {
            ctx.style += "font-weight: bold;";
        } else if (child.tag == "i") {
            ctx.style += "font-style: italic;";
        } else if (child.tag == "u") {
            ctx.style += "text-decoration-line: underline;";  // T-324 (05 §5.9.8).
        } else if (child.tag == "s") {
            ctx.style += "text-decoration-line: line-through;";
        } else if (child.tag == "a") {
            const DomAttr* onClick = child.attr("on:click");
            links.push_back(onClick != nullptr ? onClick->value : std::string());
            ctx.link = static_cast<int32_t>(links.size() - 1);
        }
        if (const DomAttr* style = child.attr("style")) {
            ctx.style += style->value;
            ctx.style += ";";
        }
        if (const DomAttr* cls = child.attr("class")) {
            size_t i = 0;
            const std::string& v = cls->value;
            while (i < v.size()) {
                while (i < v.size() && isWs(v[i])) {
                    ++i;
                }
                size_t s = i;
                while (i < v.size() && !isWs(v[i])) {
                    ++i;
                }
                if (i > s) {
                    ctx.classes.push_back(v.substr(s, i - s));
                }
            }
        }
        return ctx;
    }

    /// T-178: flattens `nodeIndex`'s content (raw text + inline children) into `raw`/`runs`.
    void flattenInline(uint32_t nodeIndex, const RunContext& ctx, std::string& raw,
                       std::vector<DomTextRun>& runs, std::vector<std::string>& links) const {
        const DomNode& node = m_tree.nodes[nodeIndex];
        if (!node.textRuns.empty()) {
            // Already rich (a nested inline element that mixed text and markup itself): its
            // folded text and runs are merged under `ctx`.
            uint32_t cursor = 0;
            const uint32_t linkBase = static_cast<uint32_t>(links.size());
            links.insert(links.end(), node.textLinks.begin(), node.textLinks.end());
            for (const DomTextRun& inner : node.textRuns) {
                appendSegment(std::string_view(node.text).substr(cursor, inner.start - cursor), ctx, raw,
                              runs);
                DomTextRun run = inner;
                const uint32_t length = inner.end - inner.start;
                run.start = static_cast<uint32_t>(raw.size());
                run.end = run.start + length;
                run.style = ctx.style + inner.style;
                run.classes = ctx.classes;
                run.classes.insert(run.classes.end(), inner.classes.begin(), inner.classes.end());
                run.link = inner.link >= 0 ? static_cast<int32_t>(linkBase) + inner.link : ctx.link;
                raw.append(std::string_view(node.text).substr(inner.start, length));
                runs.push_back(std::move(run));
                cursor = inner.end;
            }
            appendSegment(std::string_view(node.text).substr(cursor), ctx, raw, runs);
            return;
        }
        uint32_t pos = 0;
        for (size_t k = 0; k < node.children.size(); ++k) {
            const uint32_t at = k < node.rawChildPos.size() ? node.rawChildPos[k] : pos;
            appendSegment(std::string_view(node.rawText).substr(pos, at - pos), ctx, raw, runs);
            pos = at;
            const uint32_t childIndex = node.children[k];
            const DomNode& child = m_tree.nodes[childIndex];
            if (child.tag == "glyph" || child.tag == "img") {
                RunContext objectCtx = childContext(ctx, child, links);
                DomTextRun run;
                run.start = static_cast<uint32_t>(raw.size());
                run.end = run.start + static_cast<uint32_t>(kObjectReplacement.size());
                run.style = objectCtx.style;
                run.classes = objectCtx.classes;
                run.link = objectCtx.link;
                run.inlineKind = child.tag == "glyph" ? DomInlineKind::Glyph : DomInlineKind::Image;
                const DomAttr* ref = child.attr(child.tag == "glyph" ? "key" : "src");
                run.inlineRef = ref != nullptr ? ref->value : std::string();
                if (const DomAttr* h = child.attr("height")) {
                    run.inlineHeight = h->value;
                }
                run.line = child.line;
                run.col = child.col;
                raw.append(kObjectReplacement);
                runs.push_back(std::move(run));
                continue;
            }
            flattenInline(childIndex, childContext(ctx, child, links), raw, runs, links);
        }
        appendSegment(std::string_view(node.rawText).substr(std::min<size_t>(pos, node.rawText.size())), ctx,
                      raw, runs);
    }

    /// T-178 (07.8): turns `nodeIndex`'s text + inline children into one folded text with runs.
    /// Its inline children (always the last nodes of the tree: they were parsed inside it) are
    /// removed from the tree, so DomTree::nodes stays DOCT's element order.
    void buildRichText(uint32_t nodeIndex) {
        std::string raw;
        std::vector<DomTextRun> rawRuns;
        std::vector<std::string> links;
        flattenInline(nodeIndex, RunContext{}, raw, rawRuns, links);
        std::vector<uint32_t> map;
        std::string folded = foldWhitespaceMapped(raw, map);
        std::vector<DomTextRun> runs;
        for (DomTextRun& run : rawRuns) {
            run.start = map[run.start];
            run.end = map[run.end];
            if (run.end > run.start) {
                runs.push_back(std::move(run));
            }
        }
        DomNode& node = m_tree.nodes[nodeIndex];
        node.text = std::move(folded);
        node.hasText = true;
        node.textRuns = std::move(runs);
        node.textLinks = std::move(links);
        node.children.clear();
        node.rawChildPos.clear();
        truncateNodes(static_cast<size_t>(nodeIndex) + 1);
    }

    /// Drops the tree's nodes from `newSize` on, together with every `symbolDefs` entry that
    /// pointed at one of them (T-257: no stale index survives a truncation).
    void truncateNodes(size_t newSize) {
        m_tree.nodes.resize(newSize);
        std::erase_if(m_tree.symbolDefs, [newSize](uint32_t idx) { return idx >= newSize; });
    }

    /// Parses the content of an already-open element (nodeIndex; or, for the root, index 0) up to
    /// its matching closing tag, handling text accumulation/entity decoding, child elements, and
    /// the text/child-elements mutual exclusion rule. `isRoot` additionally special-cases <head>
    /// (no node produced) and <body> (registered as DomTree::body) among the root's children.
    void parseGenericContent(uint32_t nodeIndex, std::string_view tagLowerForClosing, int startLine,
                             int startCol, bool isRoot) {
        std::string textBuf;
        for (;;) {
            if (m_aborted) {
                return;  // T-257: the nesting limit was hit below; leave the partial tree as is.
            }
            if (eof()) {
                error("unexpected end of file, unclosed tag <" + std::string(tagLowerForClosing) + ">",
                      startLine, startCol);
                break;
            }
            char c = peek();
            if (c == '<') {
                if (startsWith("<!--")) {
                    skipComment();
                    continue;
                }
                if (peek(1) == '!' || peek(1) == '?') {
                    skipDeclaration();
                    continue;
                }
                if (peek(1) == '/') {
                    int closeLine = m_line;
                    int closeCol = m_col;
                    advance();
                    advance();
                    std::string closeName;
                    readTagName(closeName);
                    skipInlineWs();
                    if (!eof() && peek() == '>') {
                        advance();
                    }
                    std::string closeLower = toLowerCopy(closeName);
                    if (closeLower != tagLowerForClosing) {
                        error("mismatched closing tag </" + closeLower + ">, expected </" +
                                  std::string(tagLowerForClosing) + ">",
                              closeLine, closeCol);
                    }
                    break;
                }
                int childLine = m_line;
                int childCol = m_col;
                advance();  // '<'
                std::string name;
                std::vector<DomAttr> attrs;
                bool selfClosed = false;
                if (!parseStartTagRest(name, attrs, selfClosed, childLine, childCol)) {
                    break;
                }
                std::string lower = toLowerCopy(name);
                if (isRoot && lower == "head") {
                    if (!selfClosed) {
                        parseHeadContent(childLine, childCol);
                    }
                    continue;
                }
                uint32_t childIndex = parseElement(name, std::move(attrs), selfClosed, childLine, childCol);
                m_tree.nodes[nodeIndex].children.push_back(childIndex);
                m_tree.nodes[nodeIndex].rawChildPos.push_back(static_cast<uint32_t>(textBuf.size()));
                m_tree.nodes[childIndex].parent = nodeIndex;
                if (lower == "symbol") {
                    m_tree.symbolDefs.push_back(childIndex);
                }
                if (isRoot && lower == "body") {
                    if (m_tree.body != 0xFFFFFFFFu) {
                        error("duplicate <body> element", childLine, childCol);
                    } else {
                        m_tree.body = childIndex;
                    }
                }
            } else if (c == '&') {
                textBuf += consumeEntity();
            } else {
                textBuf.push_back(c);
                advance();
            }
        }
        std::string folded = foldWhitespace(textBuf);
        bool hasChildren = !m_tree.nodes[nodeIndex].children.empty();
        m_tree.nodes[nodeIndex].rawText = textBuf;
        if (!folded.empty()) {
            if (hasChildren && allChildrenInline(nodeIndex)) {
                buildRichText(nodeIndex);  // T-178 (07.8): text with inline markup.
            } else if (hasChildren) {
                error("element <" + m_tree.nodes[nodeIndex].tag + "> mixes text and child elements",
                      startLine, startCol);
            } else {
                m_tree.nodes[nodeIndex].text = folded;
                m_tree.nodes[nodeIndex].hasText = true;
            }
        }
    }
};

}  // namespace

bool parseXt(std::string_view text, std::string_view fileName, DiagnosticSink& sink, DomTree& out) {
    out = DomTree{};
    Parser parser(text, fileName, sink, out, /*requireBody=*/true);
    return parser.parse();
}

bool parseXtSymbolLibrary(std::string_view text, std::string_view fileName, DiagnosticSink& sink,
                          DomTree& out) {
    out = DomTree{};
    Parser parser(text, fileName, sink, out, /*requireBody=*/false);
    return parser.parse();
}

}  // namespace xt::compile
