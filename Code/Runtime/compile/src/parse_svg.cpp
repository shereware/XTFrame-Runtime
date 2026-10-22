#include "xt/compile/parse_svg.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "css_color.h"        // Code/Runtime/core/src: named colours (T-322).
#include "delaunay.h"         // Code/Runtime/core/src (T-243).
#include "shape_modifiers.h"  // Code/Runtime/core/src: kMaxGeneratedPoints (T-258b).
#include "xt/charconv.h"
#include "xt/compile/svg_path.h"
#include "xt/ir_pod.h"
#include "xt/node.h"
#include "xt/num_guard.h"
#include "xt/util/hex.h"

/// Static SVG vector import (Docs/design/12-compiler.md §12.2.1, Docs/tasks/T-110.md): a hand
/// written XML subset parser plus the element/paint mapping onto the same Scene node structure
/// Lottie uses (Docs/design/11-lottie-mapping.md §11.3.2/§11.3.3). No third-party dependency.

namespace xt::compile {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
/// Guard against pathological nesting; deeper subtrees are skipped with a warning.
constexpr int kMaxDepth = 64;

// ---------------------------------------------------------------------------------------------
// XML subset
// ---------------------------------------------------------------------------------------------

struct XmlAttr {
    std::string name;
    std::string value;
};

struct XmlNode {
    std::string tag;
    std::vector<XmlAttr> attrs;
    std::vector<uint32_t> children;
    int line = 1;
    /// T-243: the element's character data (entity-decoded, every text run concatenated); only
    /// `<xt-pins>`, `<xt-tris>` and `<xt-weights>` read it.
    std::string text;
};

/// Drops a namespace prefix (`svg:rect` -> `rect`, `xlink:href` -> `href`); the subset treats all
/// namespaces alike.
std::string_view stripPrefix(std::string_view name) {
    size_t colon = name.rfind(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

/// T-243 (32 §32.4): an attribute name with its namespace prefix dropped, except `bind:` and `on:`
/// (XTFrame's binding attributes, which keep their whole name).
std::string_view attrNameOf(std::string_view name) {
    if (name.rfind("bind:", 0) == 0 || name.rfind("on:", 0) == 0) {
        return name;
    }
    return stripPrefix(name);
}

bool isNameCharParseSvg(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-' || c == ':' || c == '.';
}

bool isXmlSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// Expands the five predefined entities plus numeric character references; anything else is kept
/// verbatim (the subset never needs a DTD).
std::string decodeEntities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != '&') {
            out.push_back(raw[i++]);
            continue;
        }
        size_t semi = raw.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out.push_back(raw[i++]);
            continue;
        }
        std::string_view name = raw.substr(i + 1, semi - i - 1);
        if (name == "amp") {
            out.push_back('&');
        } else if (name == "lt") {
            out.push_back('<');
        } else if (name == "gt") {
            out.push_back('>');
        } else if (name == "quot") {
            out.push_back('"');
        } else if (name == "apos") {
            out.push_back('\'');
        } else if (!name.empty() && name[0] == '#') {
            uint32_t code = 0;
            std::string_view digits = name.substr(1);
            int base = 10;
            if (!digits.empty() && (digits[0] == 'x' || digits[0] == 'X')) {
                base = 16;
                digits = digits.substr(1);
            }
            std::from_chars_result r =
                std::from_chars(digits.data(), digits.data() + digits.size(), code, base);
            if (r.ec != std::errc{} || r.ptr != digits.data() + digits.size()) {
                out.append(raw.substr(i, semi - i + 1));
            } else if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else if (code < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (code >> 18)));
                out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
        } else {
            out.append(raw.substr(i, semi - i + 1));
            i = semi + 1;
            continue;
        }
        i = semi + 1;
    }
    return out;
}

/// Parses the XML subset 12.2.1 needs: elements, attributes, self-closing tags, comments, CDATA
/// and processing instructions/DOCTYPE (all skipped). Character data is kept in XmlNode::text
/// (T-243: only the rig elements read it). Returns the root index, or `0xFFFFFFFF` on a syntax
/// error.
uint32_t parseXml(std::string_view text, std::vector<XmlNode>& nodes) {
    std::vector<uint32_t> stack;
    uint32_t root = 0xFFFFFFFFu;
    int line = 1;
    size_t i = 0;

    auto advance = [&](size_t to) {
        for (; i < to && i < text.size(); ++i) {
            if (text[i] == '\n') {
                ++line;
            }
        }
    };

    while (i < text.size()) {
        size_t lt = text.find('<', i);
        if (lt == std::string_view::npos) {
            break;
        }
        if (!stack.empty() && lt > i) {
            nodes[stack.back()].text += decodeEntities(text.substr(i, lt - i));  // T-243
        }
        advance(lt);
        ++i;
        if (i >= text.size()) {
            return 0xFFFFFFFFu;
        }
        if (text.compare(i, 3, "!--") == 0) {
            size_t end = text.find("-->", i + 3);
            if (end == std::string_view::npos) {
                return 0xFFFFFFFFu;
            }
            advance(end + 3);
            continue;
        }
        if (text.compare(i, 8, "![CDATA[") == 0) {
            size_t end = text.find("]]>", i + 8);
            if (end == std::string_view::npos) {
                return 0xFFFFFFFFu;
            }
            advance(end + 3);
            continue;
        }
        if (text[i] == '?' || text[i] == '!') {
            size_t end = text.find('>', i);
            if (end == std::string_view::npos) {
                return 0xFFFFFFFFu;
            }
            advance(end + 1);
            continue;
        }
        if (text[i] == '/') {
            size_t end = text.find('>', i);
            if (end == std::string_view::npos || stack.empty()) {
                return 0xFFFFFFFFu;
            }
            std::string_view name = text.substr(i + 1, end - i - 1);
            while (!name.empty() && isXmlSpace(name.back())) {
                name.remove_suffix(1);
            }
            if (stripPrefix(name) != nodes[stack.back()].tag) {
                return 0xFFFFFFFFu;
            }
            stack.pop_back();
            advance(end + 1);
            continue;
        }

        // An element start tag.
        size_t nameBegin = i;
        while (i < text.size() && isNameCharParseSvg(text[i])) {
            ++i;
        }
        if (i == nameBegin) {
            return 0xFFFFFFFFu;
        }
        XmlNode node;
        node.tag = std::string(stripPrefix(text.substr(nameBegin, i - nameBegin)));
        node.line = line;

        bool selfClosing = false;
        bool closed = false;
        while (i < text.size()) {
            while (i < text.size() && isXmlSpace(text[i])) {
                if (text[i] == '\n') {
                    ++line;
                }
                ++i;
            }
            if (i >= text.size()) {
                return 0xFFFFFFFFu;
            }
            if (text[i] == '>') {
                ++i;
                closed = true;
                break;
            }
            if (text[i] == '/') {
                ++i;
                if (i >= text.size() || text[i] != '>') {
                    return 0xFFFFFFFFu;
                }
                ++i;
                selfClosing = true;
                closed = true;
                break;
            }
            size_t attrBegin = i;
            while (i < text.size() && isNameCharParseSvg(text[i])) {
                ++i;
            }
            if (i == attrBegin) {
                return 0xFFFFFFFFu;
            }
            std::string_view attrName = attrNameOf(text.substr(attrBegin, i - attrBegin));
            while (i < text.size() && isXmlSpace(text[i])) {
                ++i;
            }
            if (i >= text.size() || text[i] != '=') {
                return 0xFFFFFFFFu;
            }
            ++i;
            while (i < text.size() && isXmlSpace(text[i])) {
                ++i;
            }
            if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) {
                return 0xFFFFFFFFu;  // Unquoted attribute values are not part of the subset.
            }
            char quote = text[i++];
            size_t valueBegin = i;
            size_t valueEnd = text.find(quote, i);
            if (valueEnd == std::string_view::npos) {
                return 0xFFFFFFFFu;
            }
            node.attrs.push_back(XmlAttr{std::string(attrName),
                                         decodeEntities(text.substr(valueBegin, valueEnd - valueBegin))});
            advance(valueEnd);
            ++i;
        }
        if (!closed) {
            return 0xFFFFFFFFu;
        }

        uint32_t index = static_cast<uint32_t>(nodes.size());
        nodes.push_back(std::move(node));
        if (!stack.empty()) {
            nodes[stack.back()].children.push_back(index);
        } else if (root == 0xFFFFFFFFu) {
            root = index;
        } else {
            return 0xFFFFFFFFu;  // A second root element.
        }
        if (!selfClosing) {
            stack.push_back(index);
        }
    }
    return stack.empty() ? root : 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------------------------
// Value parsing
// ---------------------------------------------------------------------------------------------

std::string_view trimParseSvg(std::string_view s) {
    while (!s.empty() && isXmlSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isXmlSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool parseNumberParseSvg(std::string_view s, float& out) {
    s = trimParseSvg(s);
    if (s.empty()) {
        return false;
    }
    if (s.front() == '+') {
        s.remove_prefix(1);
    }
    float value = 0;
    std::from_chars_result r = xt::fromChars(s.data(), s.data() + s.size(), value);
    // T-258b (D10): "inf" / "nan" (which xt::fromChars accepts) are not numbers here.
    if (r.ec != std::errc{} || !std::isfinite(value)) {
        return false;
    }
    // A trailing CSS unit is accepted and ignored: the subset has no notion of physical units, so
    // every length is a user-space number (see report).
    out = value;
    return true;
}

/// T-258b (D3): true when `s` reads as an infinity or a NaN (parseNumber() refuses those).
bool isNonFiniteNumber(std::string_view s) {
    s = trimParseSvg(s);
    if (!s.empty() && s.front() == '+') {
        s.remove_prefix(1);
    }
    float value = 0;
    std::from_chars_result r = xt::fromChars(s.data(), s.data() + s.size(), value);
    return r.ec == std::errc{} && !std::isfinite(value);
}

/// Parses a length that may be a percentage. `out` is the raw number; `outIsPercent` says whether
/// it still has to be scaled by a reference length.
bool parseLengthOrPercent(std::string_view s, float& out, bool& outIsPercent) {
    s = trimParseSvg(s);
    outIsPercent = !s.empty() && s.back() == '%';
    if (outIsPercent) {
        s.remove_suffix(1);
    }
    if (!parseNumberParseSvg(s, out)) {
        return false;
    }
    if (outIsPercent) {
        out *= 0.01f;
    }
    return true;
}

/// Reads a whitespace/comma separated list of numbers (`points`, `stroke-dasharray`, ...).
std::vector<float> parseNumberList(std::string_view s) {
    std::vector<float> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (isXmlSpace(s[i]) || s[i] == ',')) {
            ++i;
        }
        size_t begin = i;
        while (i < s.size() && !isXmlSpace(s[i]) && s[i] != ',') {
            ++i;
        }
        if (i == begin) {
            break;
        }
        float value = 0;
        if (!parseNumberParseSvg(s.substr(begin, i - begin), value)) {
            out.clear();
            return out;
        }
        out.push_back(value);
    }
    return out;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

/// `#rgb`, `#rrggbb`, `#rrggbbaa`, `rgb()`, `rgba()` and the named colors. `none`/`transparent`
/// are handled by the paint parser, not here.
bool parseColor(std::string_view raw, Color& out) {
    std::string s = toLower(std::string(trimParseSvg(raw)));
    if (s.empty()) {
        return false;
    }
    if (s[0] == '#') {
        std::string_view hex(s.c_str() + 1, s.size() - 1);
        int v[8] = {};
        for (size_t k = 0; k < hex.size(); ++k) {
            int d = util::hexDigit(hex[k]);
            if (d < 0 || k >= 8) {
                return false;
            }
            v[k] = d;
        }
        if (hex.size() == 3 || hex.size() == 4) {
            out = Color{static_cast<float>(v[0] * 17) / 255.0f, static_cast<float>(v[1] * 17) / 255.0f,
                        static_cast<float>(v[2] * 17) / 255.0f,
                        hex.size() == 4 ? static_cast<float>(v[3] * 17) / 255.0f : 1.0f};
            return true;
        }
        if (hex.size() == 6 || hex.size() == 8) {
            out = Color{static_cast<float>(v[0] * 16 + v[1]) / 255.0f,
                        static_cast<float>(v[2] * 16 + v[3]) / 255.0f,
                        static_cast<float>(v[4] * 16 + v[5]) / 255.0f,
                        hex.size() == 8 ? static_cast<float>(v[6] * 16 + v[7]) / 255.0f : 1.0f};
            return true;
        }
        return false;
    }
    if (s.rfind("rgb(", 0) == 0 || s.rfind("rgba(", 0) == 0) {
        size_t open = s.find('(');
        size_t close = s.rfind(')');
        if (close == std::string::npos || close < open) {
            return false;
        }
        std::string body = s.substr(open + 1, close - open - 1);
        for (char& c : body) {
            if (c == '/') {
                c = ' ';  // The CSS Color 4 `rgb(r g b / a)` form.
            }
        }
        // Percentages are resolved against 255 for the color components, 1 for alpha.
        std::vector<std::string_view> parts;
        size_t p = 0;
        while (p < body.size()) {
            while (p < body.size() && (isXmlSpace(body[p]) || body[p] == ',')) {
                ++p;
            }
            size_t begin = p;
            while (p < body.size() && !isXmlSpace(body[p]) && body[p] != ',') {
                ++p;
            }
            if (p > begin) {
                parts.push_back(std::string_view(body).substr(begin, p - begin));
            }
        }
        if (parts.size() < 3) {
            return false;
        }
        float c[4] = {0, 0, 0, 1};
        for (size_t k = 0; k < parts.size() && k < 4; ++k) {
            float value = 0;
            bool percent = false;
            if (!parseLengthOrPercent(parts[k], value, percent)) {
                return false;
            }
            c[k] = (k < 3 && !percent) ? value / 255.0f : value;
        }
        out = Color{c[0], c[1], c[2], c[3]};
        return true;
    }
    // T-322: the CSS Color Level 4 named colours, shared with the .xts parser (Code/Runtime/core/src/css_color.h).
    return css_color::findNamedColor(s, out);
}

// ---------------------------------------------------------------------------------------------
// Transforms
// ---------------------------------------------------------------------------------------------

/// Parses an SVG `transform` list (matrix/translate/scale/rotate/skewX/skewY, chained left to
/// right). Returns false on a malformed function; `out` then holds the part parsed so far.
bool parseTransform(std::string_view s, Mat2x3& out) {
    out = Mat2x3{};
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (isXmlSpace(s[i]) || s[i] == ',')) {
            ++i;
        }
        if (i >= s.size()) {
            break;
        }
        size_t nameBegin = i;
        while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
            ++i;
        }
        std::string_view name = s.substr(nameBegin, i - nameBegin);
        while (i < s.size() && isXmlSpace(s[i])) {
            ++i;
        }
        if (name.empty() || i >= s.size() || s[i] != '(') {
            return false;
        }
        size_t close = s.find(')', i);
        if (close == std::string_view::npos) {
            return false;
        }
        std::vector<float> a = parseNumberList(s.substr(i + 1, close - i - 1));
        i = close + 1;

        Mat2x3 m;
        if (name == "matrix" && a.size() == 6) {
            m = Mat2x3{a[0], a[1], a[2], a[3], a[4], a[5]};
        } else if (name == "translate" && (a.size() == 1 || a.size() == 2)) {
            m = translation(Vec2{a[0], a.size() > 1 ? a[1] : 0.0f});
        } else if (name == "scale" && (a.size() == 1 || a.size() == 2)) {
            m = scaling(Vec2{a[0], a.size() > 1 ? a[1] : a[0]});
        } else if (name == "rotate" && (a.size() == 1 || a.size() == 3)) {
            m = rotationDeg(a[0]);
            if (a.size() == 3) {
                m = mul(translation(Vec2{a[1], a[2]}), mul(m, translation(Vec2{-a[1], -a[2]})));
            }
        } else if (name == "skewX" && a.size() == 1) {
            m = Mat2x3{1, 0, std::tan(a[0] * kDegToRad), 1, 0, 0};
        } else if (name == "skewY" && a.size() == 1) {
            m = Mat2x3{1, std::tan(a[0] * kDegToRad), 0, 1, 0, 0};
        } else {
            return false;
        }
        out = mul(out, m);
    }
    return true;
}

/// Decomposes `m` into the translate/rotate/skew/scale fields an `IrSceneNode` carries, such that
/// `composeTRSS(node...) == m` (`composeTRSS` builds `T * R * ShearX * S`, so this is the matching
/// QR-style factorization). Anchor stays at the origin.
void applyTransform(IrSceneNode& node, const Mat2x3& m) {
    float rot = std::atan2(m.b, m.a);
    float scaleX = std::sqrt(m.a * m.a + m.b * m.b);
    float cosR = std::cos(rot);
    float sinR = std::sin(rot);
    float shear = cosR * m.c + sinR * m.d;
    float scaleY = -sinR * m.c + cosR * m.d;
    float skewX = scaleY != 0.0f ? std::atan(shear / scaleY) : 0.0f;

    node.translate = Vec2{m.e, m.f};
    node.rotation = rot / kDegToRad;
    node.scale = Vec2{scaleX, scaleY};
    node.skew = Vec2{skewX / kDegToRad, 0};
    node.anchor = Vec2{0, 0};
}

// ---------------------------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------------------------

struct SvgPaintSpec {
    uint8_t kind = static_cast<uint8_t>(PaintKind::Solid);
    Color color{0, 0, 0, 1};
    std::string gradientId;
};

/// The inherited presentation state. `opacity` is deliberately absent: it applies to the element
/// itself (as the scene node's opacity) and is not inherited.
struct SvgStyle {
    SvgPaintSpec fill;
    SvgPaintSpec stroke{static_cast<uint8_t>(PaintKind::None), Color{0, 0, 0, 1}, {}};
    float fillOpacity = 1;
    float strokeOpacity = 1;
    uint8_t fillRule = static_cast<uint8_t>(FillRule::NonZero);
    float strokeWidth = 1;
    uint8_t cap = static_cast<uint8_t>(LineCap::Butt);
    uint8_t join = static_cast<uint8_t>(LineJoin::Miter);
    float miterLimit = 4;
    std::vector<float> dash;
    float dashPhase = 0;
    /// T-225b (31 §31.5.3): `clip-rule`, the fill rule of `<clipPath>` content (inherited).
    uint8_t clipRule = static_cast<uint8_t>(FillRule::NonZero);
};

// ---------------------------------------------------------------------------------------------
// Build
// ---------------------------------------------------------------------------------------------

struct GradientDef {
    bool radial = false;
    bool userSpace = false;
    bool hasTransform = false;
    Mat2x3 transform;
    // Linear: x1/y1/x2/y2. Radial: cx/cy and r (kept in `p1.x` as the edge offset).
    Vec2 p0{0, 0};
    Vec2 p1{1, 0};
    std::vector<IrGradientStop> stops;
    /// T-119: a local `href`/`xlink:href` to another gradient. Only the referenced gradient's
    /// stops are inherited (the geometry attributes are not), resolved once the whole document
    /// has been collected so the target may appear anywhere.
    std::string hrefStops;
};

struct BuildCtxParseSvg {
    SceneBuilder& scene;
    IrWriter& writer;
    DiagnosticSink& sink;
    std::string file;
    std::vector<XmlNode>& nodes;
    std::unordered_map<std::string, uint32_t> byId;
    std::unordered_map<std::string, GradientDef> gradients;
    bool inUse = false;
    /// T-190: when non-null, receives (`id`, scene node index) for every drawn `<g>`/shape with an
    /// `id` (not those reached through `<use>`, `clip-path` or `mask`).
    std::vector<std::pair<std::string, uint32_t>>* ids = nullptr;
    /// T-225b (31 §31.5.3): building the content of a referenced `<clipPath>`/`<mask>` (its ids
    /// are not recorded, and its elements' own `clip-path`/`mask` are ignored).
    bool inDef = false;
    /// T-225b: building `<clipPath>` content: only the geometry counts (opaque fill by
    /// `clip-rule`, no stroke, no opacity).
    bool clipContent = false;
    /// T-243: the caller's rig inputs / outputs (may be null).
    SvgParseExtras* extras = nullptr;
    /// T-243: (`id`, scene node index) of every element recordId() names, kept even when `ids` is
    /// null (constraint targets, `xt-skin` and `<xt-weights bones>` resolve through it).
    std::vector<std::pair<std::string, uint32_t>> allIds;
    /// T-243: a Path / Mesh whose skin is built once every node exists (finishRig()).
    struct PendingSkin {
        uint32_t owner = 0;        ///< Scene node index of the Path / Mesh.
        uint32_t xmlNode = 0;      ///< Its XmlNode.
        uint32_t vertexCount = 0;  ///< Path: on-curve vertices (T-243c); Mesh: one per pin.
        /// T-243c: a Path's weight slot (one per non-Close verb, the IR's p / in / out triple) ->
        /// its `<xt-weights>` vertex (weightVertexOrder()); empty for a Mesh (slot = vertex).
        std::vector<uint32_t> slotVertex;
    };
    std::vector<PendingSkin> pendingSkins;
    /// T-243: (constraints[] index, target id, line) resolved by finishRig().
    struct PendingTarget {
        uint32_t constraint = 0;
        std::string target;
        int line = 0;
    };
    std::vector<PendingTarget> pendingTargets;
    /// T-249 (32 §32.2.1): per bone (`bones[]` index) the bind pose: the local transform built
    /// from `rest-*` (absent = the posed value) and the rest length a child bone's rest translate
    /// takes. Compile time only; the IR keeps the pose.
    struct BoneRest {
        Mat2x3 local;
        float length = 0;
    };
    std::vector<BoneRest> boneRests;
};

/// T-190: records `node`'s `id` as naming scene node `index` (see BuildCtx::ids) and returns
/// the interned name for the scene node (kInvalidString when it has no id).
StringId recordId(BuildCtxParseSvg& ctx, const XmlNode& node, uint32_t index) {
    const std::string* id = nullptr;
    for (const XmlAttr& a : node.attrs) {
        if (a.name == "id") {
            id = &a.value;
        }
    }
    // T-243 (32 §32.4): `bind:` / `world-anchor` on a drawn element bind its scene node; `on:` is
    // not supported on <svg> content yet.
    if (!ctx.inUse && !ctx.inDef) {
        for (const XmlAttr& a : node.attrs) {
            if (a.name.rfind("bind:", 0) == 0 || a.name == "world-anchor") {
                if (ctx.extras != nullptr) {
                    SvgBindAttr b;
                    b.sceneNode = index;
                    b.name = a.name;
                    b.value = a.value;
                    b.line = node.line;
                    ctx.extras->binds.push_back(std::move(b));
                }
            } else if (a.name.rfind("on:", 0) == 0) {
                ctx.sink.warn(ctx.file, a.name + " on <svg> content is not supported, ignored", node.line, 0);
            }
        }
    }
    if (id == nullptr || id->empty()) {
        return kInvalidString;
    }
    if (!ctx.inUse && !ctx.inDef) {
        ctx.allIds.emplace_back(*id, index);  // T-243
    }
    // T-230: clip-path / mask content (inDef) is addressable too -- its first copy, so an animated
    // mask path (a Lottie mask) can carry `@timeline` keys.
    bool known = false;
    if (ctx.ids != nullptr && ctx.inDef) {
        for (const auto& entry : *ctx.ids) {
            known = known || entry.first == *id;
        }
    }
    if (ctx.ids != nullptr && !ctx.inUse && !known) {
        ctx.ids->emplace_back(*id, index);
    }
    return ctx.writer.intern(*id);
}

const std::string* attrOf(const XmlNode& node, std::string_view name) {
    for (const XmlAttr& a : node.attrs) {
        if (a.name == name) {
            return &a.value;
        }
    }
    return nullptr;
}

/// Looks a presentation property up in the element's `style="..."` declarations first (they win
/// over presentation attributes), then in the attributes themselves.
bool propertyOf(const XmlNode& node, std::string_view name, std::string& out) {
    const std::string* styleAttr = attrOf(node, "style");
    if (styleAttr != nullptr) {
        std::string_view s(*styleAttr);
        size_t i = 0;
        while (i < s.size()) {
            size_t semi = s.find(';', i);
            std::string_view decl = s.substr(i, semi == std::string_view::npos ? s.size() - i : semi - i);
            size_t colon = decl.find(':');
            if (colon != std::string_view::npos && trimParseSvg(decl.substr(0, colon)) == name) {
                out.assign(trimParseSvg(decl.substr(colon + 1)));
                return true;
            }
            if (semi == std::string_view::npos) {
                break;
            }
            i = semi + 1;
        }
    }
    const std::string* a = attrOf(node, name);
    if (a != nullptr) {
        out = *a;
        return true;
    }
    return false;
}

/// Parses a `fill`/`stroke` value. `inherit` (and any unrecognized value) leaves `out` untouched
/// and returns false.
bool parsePaintSpec(std::string_view raw, SvgPaintSpec& out) {
    std::string s = toLower(std::string(trimParseSvg(raw)));
    if (s.empty() || s == "inherit") {
        return false;
    }
    if (s == "none") {
        out.kind = static_cast<uint8_t>(PaintKind::None);
        out.gradientId.clear();
        return true;
    }
    if (s == "transparent") {
        out.kind = static_cast<uint8_t>(PaintKind::Solid);
        out.color = Color{0, 0, 0, 0};
        out.gradientId.clear();
        return true;
    }
    if (s.rfind("url(", 0) == 0) {
        // T-236: ids are case-sensitive (SVG), so the reference comes from the original text; only
        // the keywords above are matched lower-cased. `s` and `original` have the same length.
        const std::string_view original = trimParseSvg(raw);
        size_t close = original.find(')');
        if (close == std::string::npos) {
            return false;
        }
        std::string ref = std::string(trimParseSvg(original.substr(4, close - 4)));
        if (!ref.empty() && (ref.front() == '"' || ref.front() == '\'')) {
            ref = ref.substr(1, ref.size() >= 2 ? ref.size() - 2 : 0);
        }
        if (ref.empty() || ref[0] != '#') {
            return false;
        }
        out.gradientId = ref.substr(1);
        out.kind = static_cast<uint8_t>(PaintKind::Linear);  // Refined when the gradient resolves.
        return true;
    }
    Color c;
    if (!parseColor(s, c)) {
        return false;
    }
    out.kind = static_cast<uint8_t>(PaintKind::Solid);
    out.color = c;
    out.gradientId.clear();
    return true;
}

void applyStyle(BuildCtxParseSvg& ctx, const XmlNode& node, SvgStyle& st) {
    std::string v;
    if (propertyOf(node, "fill", v) && !parsePaintSpec(v, st.fill)) {
        ctx.sink.warn(ctx.file, "unsupported fill value \"" + v + "\", ignored", node.line, 0);
    }
    if (propertyOf(node, "stroke", v) && !parsePaintSpec(v, st.stroke)) {
        ctx.sink.warn(ctx.file, "unsupported stroke value \"" + v + "\", ignored", node.line, 0);
    }
    float f = 0;
    bool percent = false;
    if (propertyOf(node, "fill-opacity", v) && parseLengthOrPercent(v, f, percent)) {
        st.fillOpacity = std::clamp(xt::finiteOr(f, 1.0f), 0.0f, 1.0f);
    }
    if (propertyOf(node, "stroke-opacity", v) && parseLengthOrPercent(v, f, percent)) {
        st.strokeOpacity = std::clamp(xt::finiteOr(f, 1.0f), 0.0f, 1.0f);
    }
    if (propertyOf(node, "fill-rule", v)) {
        st.fillRule = static_cast<uint8_t>(toLower(v) == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero);
    }
    if (propertyOf(node, "stroke-width", v) && parseNumberParseSvg(v, f)) {
        st.strokeWidth = f;
    }
    if (propertyOf(node, "stroke-linecap", v)) {
        std::string lc = toLower(v);
        st.cap = static_cast<uint8_t>(lc == "round" ? LineCap::Round
                                                    : (lc == "square" ? LineCap::Square : LineCap::Butt));
    }
    if (propertyOf(node, "stroke-linejoin", v)) {
        std::string lj = toLower(v);
        st.join = static_cast<uint8_t>(lj == "round" ? LineJoin::Round
                                                     : (lj == "bevel" ? LineJoin::Bevel : LineJoin::Miter));
    }
    if (propertyOf(node, "stroke-miterlimit", v) && parseNumberParseSvg(v, f)) {
        st.miterLimit = f;
    }
    if (propertyOf(node, "stroke-dasharray", v)) {
        if (toLower(trimParseSvg(v)) == "none") {
            st.dash.clear();
        } else {
            std::vector<float> dash = parseNumberList(v);
            bool positive = false;
            for (float d : dash) {
                positive = positive || d > 0.0f;
            }
            st.dash = positive ? dash : std::vector<float>{};
        }
    }
    if (propertyOf(node, "stroke-dashoffset", v) && parseNumberParseSvg(v, f)) {
        st.dashPhase = f;
    }
    if (propertyOf(node, "clip-rule", v)) {
        st.clipRule = static_cast<uint8_t>(toLower(v) == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero);
    }
}

/// The element's own (non-inherited) `opacity`.
float opacityOf(const XmlNode& node) {
    std::string v;
    float f = 1;
    bool percent = false;
    if (propertyOf(node, "opacity", v) && parseLengthOrPercent(v, f, percent)) {
        return std::clamp(xt::finiteOr(f, 1.0f), 0.0f, 1.0f);  // T-258b: NaN passes std::clamp.
    }
    return 1.0f;
}

/// T-230 (31 §31.5.2): `blend` or `mix-blend-mode` (attribute or style) as a BlendMode code; false
/// when absent. Names are the CSS ones plus `add` (Lottie's additive mode); an unknown name warns.
bool blendOf(BuildCtxParseSvg& ctx, const XmlNode& node, uint8_t& out) {
    std::string v;
    if (!propertyOf(node, "blend", v) && !propertyOf(node, "mix-blend-mode", v)) {
        return false;
    }
    static const char* kNames[] = {"normal",     "multiply",    "screen",     "overlay",    "darken",
                                   "lighten",    "color-dodge", "color-burn", "hard-light", "soft-light",
                                   "difference", "exclusion",   "hue",        "saturation", "color",
                                   "luminosity", "add"};
    const std::string name = toLower(trimParseSvg(v));
    for (size_t i = 0; i < std::size(kNames); ++i) {
        if (name == kNames[i]) {
            out = static_cast<uint8_t>(i);
            return true;
        }
    }
    ctx.sink.warn(ctx.file, "unknown blend mode \"" + v + "\", ignored", node.line, 0);
    return false;
}

void collectGradient(BuildCtxParseSvg& ctx, const XmlNode& node, bool radial) {
    const std::string* id = attrOf(node, "id");
    if (id == nullptr || id->empty()) {
        return;
    }
    GradientDef def;
    def.radial = radial;
    std::string v;
    if (propertyOf(node, "gradientUnits", v)) {
        def.userSpace = toLower(trimParseSvg(v)) == "userspaceonuse";
    }
    if (propertyOf(node, "gradientTransform", v)) {
        def.hasTransform = parseTransform(v, def.transform);
    }

    auto coord = [&](const char* name, float fallback) {
        const std::string* a = attrOf(node, name);
        float value = fallback;
        bool percent = false;
        if (a != nullptr && parseLengthOrPercent(*a, value, percent)) {
            return value;
        }
        return fallback;
    };
    if (radial) {
        def.p0 = Vec2{coord("cx", 0.5f), coord("cy", 0.5f)};
        def.p1 = Vec2{coord("r", 0.5f), 0};
    } else {
        def.p0 = Vec2{coord("x1", 0.0f), coord("y1", 0.0f)};
        def.p1 = Vec2{coord("x2", 1.0f), coord("y2", 0.0f)};
    }

    for (uint32_t childIdx : node.children) {
        const XmlNode& child = ctx.nodes[childIdx];
        if (child.tag != "stop") {
            continue;
        }
        IrGradientStop stop{};
        stop.offset = 0;
        float f = 0;
        bool percent = false;
        if (propertyOf(child, "offset", v) && parseLengthOrPercent(v, f, percent)) {
            stop.offset = std::clamp(f, 0.0f, 1.0f);
        }
        Color c{0, 0, 0, 1};
        if (propertyOf(child, "stop-color", v)) {
            std::string lower = toLower(trimParseSvg(v));
            if (lower == "none" || lower == "transparent") {
                c = Color{0, 0, 0, 0};
            } else if (!parseColor(v, c)) {
                ctx.sink.warn(ctx.file, "unsupported stop-color \"" + v + "\", using black", child.line, 0);
                c = Color{0, 0, 0, 1};
            }
        }
        if (propertyOf(child, "stop-opacity", v) && parseLengthOrPercent(v, f, percent)) {
            c.a *= std::clamp(f, 0.0f, 1.0f);
        }
        stop.color = c;
        def.stops.push_back(stop);
    }
    const std::string* href = attrOf(node, "href");
    if (def.stops.empty() && href != nullptr && href->size() > 1 && (*href)[0] == '#') {
        def.hrefStops = href->substr(1);
    }
    ctx.gradients.emplace(*id, std::move(def));
}

/// Copies stops along the `href` chains collectGradient() recorded (T-119). Runs after the whole
/// document has been collected, so a reference may point forward, backward or into a `<defs>`.
/// A chain longer than kMaxDepth links (or a cycle) leaves the gradient stop-less, which
/// resolvePaint() then reports as "not found".
void resolveGradientHrefs(BuildCtxParseSvg& ctx) {
    for (auto& entry : ctx.gradients) {
        if (!entry.second.stops.empty()) {
            continue;
        }
        std::string next = entry.second.hrefStops;
        for (int hops = 0; hops < kMaxDepth && !next.empty(); ++hops) {
            auto it = ctx.gradients.find(next);
            if (it == ctx.gradients.end() || &it->second == &entry.second) {
                break;
            }
            if (!it->second.stops.empty()) {
                entry.second.stops = it->second.stops;
                break;
            }
            next = it->second.hrefStops;
        }
    }
}

/// Indexes every `id` and every gradient in the document, so `url(#g)` and `<use href="#r">` can
/// be resolved regardless of document order.
void collectDefsParseSvg(BuildCtxParseSvg& ctx, uint32_t nodeIdx, int depth) {
    if (depth > kMaxDepth) {
        return;
    }
    const XmlNode& node = ctx.nodes[nodeIdx];
    const std::string* id = attrOf(node, "id");
    if (id != nullptr && !id->empty()) {
        ctx.byId.emplace(*id, nodeIdx);
    }
    if (node.tag == "linearGradient" || node.tag == "radialGradient") {
        collectGradient(ctx, node, node.tag == "radialGradient");
    }
    for (uint32_t child : node.children) {
        collectDefsParseSvg(ctx, child, depth + 1);
    }
}

struct Bounds {
    float minX = 0, minY = 0, maxX = 0, maxY = 0;
};

Bounds boundsOf(const SvgPath& path) {
    Bounds b;
    if (path.points.empty()) {
        return b;
    }
    b.minX = b.maxX = path.points[0];
    b.minY = b.maxY = path.points[1];
    for (size_t i = 0; i + 1 < path.points.size(); i += 2) {
        b.minX = std::min(b.minX, path.points[i]);
        b.maxX = std::max(b.maxX, path.points[i]);
        b.minY = std::min(b.minY, path.points[i + 1]);
        b.maxY = std::max(b.maxY, path.points[i + 1]);
    }
    return b;
}

/// Resolves a paint onto `IrPathData`. `alpha` is the fill/stroke-opacity multiplier, `bounds` the
/// shape's bounding box for `objectBoundingBox` gradient units.
void resolvePaint(BuildCtxParseSvg& ctx, const SvgPaintSpec& spec, float alpha, const Bounds& bounds,
                  int line, uint8_t& outKind, Color& outColor, Vec2& outP0, Vec2& outP1,
                  uint32_t& outStopOffset, uint32_t& outStopCount) {
    outKind = static_cast<uint8_t>(PaintKind::None);
    outColor = Color{0, 0, 0, 1};
    outP0 = Vec2{0, 0};
    outP1 = Vec2{0, 0};
    outStopOffset = 0;
    outStopCount = 0;

    if (spec.kind == static_cast<uint8_t>(PaintKind::None)) {
        return;
    }
    if (spec.gradientId.empty()) {
        outKind = static_cast<uint8_t>(PaintKind::Solid);
        outColor = spec.color;
        outColor.a *= alpha;
        return;
    }
    auto it = ctx.gradients.find(spec.gradientId);
    if (it == ctx.gradients.end() || it->second.stops.empty()) {
        ctx.sink.warn(ctx.file, "gradient \"#" + spec.gradientId + "\" not found, painted black", line, 0);
        outKind = static_cast<uint8_t>(PaintKind::Solid);
        outColor = Color{0, 0, 0, alpha};
        return;
    }
    const GradientDef& def = it->second;

    Vec2 p0 = def.p0;
    Vec2 p1 = def.p1;
    if (def.radial) {
        float r = def.p1.x;
        if (!def.userSpace) {
            float w = bounds.maxX - bounds.minX;
            float h = bounds.maxY - bounds.minY;
            p0 = Vec2{bounds.minX + p0.x * w, bounds.minY + p0.y * h};
            // objectBoundingBox radii are relative to the box's diagonal-normalized size; the
            // renderer's radial paint only takes one radius, so use the box's mean extent.
            r *= (w + h) * 0.5f;
        }
        p1 = Vec2{p0.x + r, p0.y};
    } else if (!def.userSpace) {
        float w = bounds.maxX - bounds.minX;
        float h = bounds.maxY - bounds.minY;
        p0 = Vec2{bounds.minX + p0.x * w, bounds.minY + p0.y * h};
        p1 = Vec2{bounds.minX + p1.x * w, bounds.minY + p1.y * h};
    }
    if (def.hasTransform) {
        p0 = apply(def.transform, p0);
        p1 = apply(def.transform, p1);
    }

    outKind = static_cast<uint8_t>(def.radial ? PaintKind::Radial : PaintKind::Linear);
    outColor = def.stops.front().color;
    outP0 = p0;
    outP1 = p1;
    outStopOffset = static_cast<uint32_t>(ctx.scene.stopPool.size());
    for (IrGradientStop stop : def.stops) {
        stop.color.a *= alpha;
        ctx.scene.stopPool.push_back(stop);
    }
    outStopCount = static_cast<uint32_t>(def.stops.size());
}

/// Fills the paint fields `IrPathData` and `IrShapeData` share (T-225: same names) from `style`;
/// `bounds` is the shape's box for `objectBoundingBox` gradients.
template <class PaintData>
void resolvePaintFields(BuildCtxParseSvg& ctx, const SvgStyle& style, const Bounds& bounds, int line,
                        PaintData& data) {
    data.fillRule = style.fillRule;
    resolvePaint(ctx, style.fill, style.fillOpacity, bounds, line, data.fillKind, data.fillColor, data.fillP0,
                 data.fillP1, data.fillStopOffset, data.fillStopCount);
    resolvePaint(ctx, style.stroke, style.strokeOpacity, bounds, line, data.strokeKind, data.strokeColor,
                 data.strokeP0, data.strokeP1, data.strokeStopOffset, data.strokeStopCount);
    data.cap = style.cap;
    data.join = style.join;
    data.strokeWidth = style.strokeWidth;
    data.miterLimit = style.miterLimit;
    data.trimStart = 0;
    data.trimEnd = 1;
    data.trimOffset = 0;
    if (!style.dash.empty() && data.strokeKind != static_cast<uint8_t>(PaintKind::None)) {
        data.dashOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
        ctx.scene.floatPool.insert(ctx.scene.floatPool.end(), style.dash.begin(), style.dash.end());
        data.dashCount = static_cast<uint32_t>(style.dash.size());
        data.dashPhase = style.dashPhase;
    }
}

/// Appends a Path node carrying `path`'s geometry and `style`'s paints.
void emitPath(BuildCtxParseSvg& ctx, const SvgPath& path, const SvgStyle& style, uint32_t parent,
              const Mat2x3& local, float opacity, StringId name, int line) {
    IrPathData data{};
    data.verbOffset = static_cast<uint32_t>(ctx.scene.verbPool.size());
    ctx.scene.verbPool.insert(ctx.scene.verbPool.end(), path.verbs.begin(), path.verbs.end());
    data.verbCount = static_cast<uint32_t>(path.verbs.size());
    data.pointOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
    ctx.scene.floatPool.insert(ctx.scene.floatPool.end(), path.points.begin(), path.points.end());
    data.pointCount = static_cast<uint32_t>(path.points.size());
    resolvePaintFields(ctx, style, boundsOf(path), line, data);

    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Path);
    node.visible = 1;
    node.parent = parent;
    node.kindIndex = static_cast<uint32_t>(ctx.scene.paths.size());
    node.opacity = opacity;
    node.scale = Vec2{1, 1};
    node.name = name;
    applyTransform(node, local);
    ctx.scene.paths.push_back(data);
    ctx.scene.nodes.push_back(node);
}

uint32_t emitGroup(BuildCtxParseSvg& ctx, uint32_t parent, const Mat2x3& local, float opacity,
                   StringId name) {
    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Group);
    node.visible = 1;
    node.parent = parent;
    node.kindIndex = 0;
    node.opacity = opacity;
    node.scale = Vec2{1, 1};
    node.name = name;
    applyTransform(node, local);
    uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(node);
    return index;
}

/// T-225b (31 §31.6.1): appends a Shape node (a parametric `<rect>`/`<circle>`/`<ellipse>`/
/// `<xt-star>`) with `params` (ShapeNodeData's layout) and `style`'s paints. `bounds` is the box
/// of the shape's geometry at these parameters, for `objectBoundingBox` gradients.
void emitShape(BuildCtxParseSvg& ctx, ShapeKind kind, const float (&params)[12], const Bounds& bounds,
               const SvgStyle& style, uint32_t parent, const Mat2x3& local, float opacity, StringId name,
               int line) {
    IrShapeData data{};
    data.kind = static_cast<uint8_t>(kind);
    std::copy(std::begin(params), std::end(params), std::begin(data.params));
    resolvePaintFields(ctx, style, bounds, line, data);

    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Shape);
    node.visible = 1;
    node.parent = parent;
    node.kindIndex = static_cast<uint32_t>(ctx.scene.shapes.size());
    node.opacity = opacity;
    node.scale = Vec2{1, 1};
    node.name = name;
    applyTransform(node, local);
    ctx.scene.shapes.push_back(data);
    ctx.scene.nodes.push_back(node);
}

/// T-190 (03 §3.8/§3.12.6): `path-trim="<start> <end> [<offset>]"`, the attribute form of the
/// `path-trim` property (not inherited), onto an IrPathData or (T-225b) IrShapeData.
template <class PaintData>
void applyPathTrimAttr(BuildCtxParseSvg& ctx, const XmlNode& node, PaintData& data) {
    std::string trimValue;
    if (propertyOf(node, "path-trim", trimValue)) {
        std::vector<float> t = parseNumberList(trimValue);
        if (t.size() == 2 || t.size() == 3) {
            data.trimStart = t[0];
            data.trimEnd = t[1];
            data.trimOffset = t.size() == 3 ? t[2] : 0.0f;
        } else {
            ctx.sink.warn(ctx.file, "malformed path-trim \"" + trimValue + "\", ignored", node.line, 0);
        }
    }
}

/// T-233 (31 §31.4.7): `xt-taper="<start> <end>"` (fractions of each contour's arc length) and
/// `xt-width-profile="t:w t:w ..."` (whitespace or comma separated; sorted by t, t clamped to 0..1,
/// w to >= 0), the attribute forms of the variable-width stroke (not inherited), onto an IrPathData
/// or IrShapeData; the profile goes into the scene floatPool as (t, w) pairs. `closed`: the
/// geometry has a closed contour, where the taper is ignored (warned).
template <class PaintData>
void applyStrokeTaperAttrs(BuildCtxParseSvg& ctx, const XmlNode& node, bool closed, PaintData& data) {
    std::string value;
    if (propertyOf(node, "xt-taper", value)) {
        std::vector<float> t = parseNumberList(value);
        if (t.size() == 2) {
            data.taperStart = t[0];
            data.taperEnd = t[1];
            if (closed && (t[0] > 0.0f || t[1] > 0.0f)) {
                ctx.sink.warn(ctx.file, "xt-taper is ignored on closed contours", node.line, 0);
            }
        } else {
            ctx.sink.warn(ctx.file, "malformed xt-taper \"" + value + "\", ignored", node.line, 0);
        }
    }
    if (propertyOf(node, "xt-width-profile", value)) {
        std::vector<WidthPoint> profile;
        bool malformed = false;
        std::string_view s(value);
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && (isXmlSpace(s[i]) || s[i] == ',')) {
                ++i;
            }
            size_t begin = i;
            while (i < s.size() && !isXmlSpace(s[i]) && s[i] != ',') {
                ++i;
            }
            if (i == begin) {
                break;
            }
            std::string_view tok = s.substr(begin, i - begin);
            size_t colon = tok.find(':');
            WidthPoint p;
            if (colon == std::string_view::npos || !parseNumberParseSvg(tok.substr(0, colon), p.t) ||
                !parseNumberParseSvg(tok.substr(colon + 1), p.w)) {
                malformed = true;
                break;
            }
            p.t = std::clamp(p.t, 0.0f, 1.0f);
            p.w = std::max(p.w, 0.0f);
            profile.push_back(p);
        }
        if (malformed || profile.empty()) {
            ctx.sink.warn(ctx.file, "malformed xt-width-profile \"" + value + "\", ignored", node.line, 0);
        } else {
            std::stable_sort(profile.begin(), profile.end(),
                             [](const WidthPoint& a, const WidthPoint& b) { return a.t < b.t; });
            data.profileOffset = static_cast<uint32_t>(ctx.scene.floatPool.size());
            for (const WidthPoint& p : profile) {
                ctx.scene.floatPool.push_back(p.t);
                ctx.scene.floatPool.push_back(p.w);
            }
            data.profileCount = static_cast<uint32_t>(profile.size());
        }
    }
}

/// T-225b: the parametric-shape kind of an SVG element name; false for any other element.
bool shapeKindOf(std::string_view tag, ShapeKind& out) {
    if (tag == "rect") {
        out = ShapeKind::Rect;
    } else if (tag == "circle" || tag == "ellipse") {
        out = ShapeKind::Ellipse;
    } else if (tag == "xt-star") {
        out = ShapeKind::Star;
    } else {
        return false;
    }
    return true;
}

/// T-225b (31 §31.6.1): ShapeNodeData::params of a `<rect>` [x, y, w, h, rx, ry] (rx/ry resolved
/// as buildShape() does: a missing one takes the other's value; the clamping to half the size stays
/// with the renderer), `<circle>`/`<ellipse>` [cx, cy, rx, ry] or `<xt-star>` [cx, cy, points, r,
/// r2, rotation, round, round2] (round/round2 clamped to 0..1, as buildStar()).
void shapeParamsOf(const XmlNode& node, ShapeKind kind, float (&params)[12]) {
    auto num = [&](const char* name, float fallback) {
        const std::string* a = attrOf(node, name);
        float value = fallback;
        bool percent = false;
        if (a != nullptr && parseLengthOrPercent(*a, value, percent)) {
            return value;
        }
        return fallback;
    };
    std::fill(std::begin(params), std::end(params), 0.0f);
    switch (kind) {
        case ShapeKind::Rect: {
            params[0] = num("x", 0);
            params[1] = num("y", 0);
            params[2] = num("width", 0);
            params[3] = num("height", 0);
            const bool hasRx = attrOf(node, "rx") != nullptr;
            const bool hasRy = attrOf(node, "ry") != nullptr;
            const float rx = hasRx ? num("rx", 0) : (hasRy ? num("ry", 0) : 0.0f);
            params[4] = rx;
            params[5] = hasRy ? num("ry", 0) : rx;
            break;
        }
        case ShapeKind::Ellipse:
            params[0] = num("cx", 0);
            params[1] = num("cy", 0);
            if (node.tag == "circle") {
                params[2] = params[3] = num("r", 0);
            } else {
                params[2] = num("rx", 0);
                params[3] = num("ry", 0);
            }
            break;
        case ShapeKind::Star:
            params[0] = num("cx", 0);
            params[1] = num("cy", 0);
            // T-258b (D3): clamped as buildStar() does (which warns).
            params[2] = std::min(num("points", 0), static_cast<float>(modifiers::kMaxGeneratedPoints));
            params[3] = num("r", 0);
            params[4] = num("r2", 0);
            params[5] = num("rotation", 0);
            params[6] = std::clamp(num("round", 0), 0.0f, 1.0f);
            params[7] = std::clamp(num("round2", 0), 0.0f, 1.0f);
            break;
    }
}

/// T-225b (31 §31.6.1): the modifier kind of an `<xt-*>` modifier element name; false otherwise.
bool modifierKindOf(std::string_view tag, ModifierKind& out) {
    constexpr std::pair<std::string_view, ModifierKind> kTags[] = {
        {"xt-trim", ModifierKind::Trim},     {"xt-repeat", ModifierKind::Repeat},
        {"xt-round", ModifierKind::Round},   {"xt-pucker", ModifierKind::Pucker},
        {"xt-zigzag", ModifierKind::ZigZag}, {"xt-twist", ModifierKind::Twist},
        {"xt-wiggle", ModifierKind::Wiggle}, {"xt-offset", ModifierKind::Offset},
        {"xt-merge", ModifierKind::Merge},
    };
    for (const auto& [name, kind] : kTags) {
        if (tag == name) {
            out = kind;
            return true;
        }
    }
    return false;
}

/// T-225b (31 §31.6.1): the IrModifierData of an `<xt-*>` modifier element. Attribute names are
/// the `.xts` names (`trim-start`, `copies`, `repeat-rotation`, ...) plus the element-table
/// spellings of §31.6.1 (`start`/`end`/`offset`, `anchor`/`translate`/`rotate`/`scale`, `center`);
/// numbers may be percentages (`50%` = 0.5). A missing required parameter warns (an error under
/// --strict) and keeps its default. `<xt-offset>`/`<xt-merge>` are editor-baked: `enabled` is 0,
/// and without `baked="1"` they also warn `vector.unbakedModifier`.
IrModifierData modifierDataOf(BuildCtxParseSvg& ctx, const XmlNode& node, ModifierKind kind) {
    IrModifierData m{};
    m.kind = static_cast<uint8_t>(kind);
    m.enabled = 1;
    m.opacityA = 1;
    m.opacityB = 1;
    const std::string& tag = node.tag;

    // The first present attribute of `names`; nullptr if none.
    auto find = [&](std::initializer_list<const char*> names) -> const std::string* {
        for (const char* n : names) {
            if (const std::string* a = attrOf(node, n)) {
                return a;
            }
        }
        return nullptr;
    };
    auto num = [&](std::initializer_list<const char*> names, float& value) {
        const std::string* a = find(names);
        if (a == nullptr) {
            return false;
        }
        bool percent = false;
        float v = 0;
        if (!parseLengthOrPercent(*a, v, percent)) {
            ctx.sink.warn(ctx.file, "<" + tag + ">: malformed number \"" + *a + "\", ignored", node.line, 0);
            return false;
        }
        value = v;
        return true;
    };
    // A "x y" pair (one number = both); percentages allowed.
    auto numbers = [&](const std::string& text, std::vector<float>& out) {
        out.clear();
        std::string_view rest = text;
        while (true) {
            rest = trimParseSvg(rest);
            if (rest.empty()) {
                return true;
            }
            size_t end = rest.find_first_of(" \t\n\r,");
            std::string_view token = rest.substr(0, end);
            float f = 0;
            bool percent = false;
            if (token.empty() || !parseLengthOrPercent(token, f, percent)) {
                return false;
            }
            out.push_back(f);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        }
    };
    auto pair = [&](std::initializer_list<const char*> names, float& x, float& y) {
        const std::string* a = find(names);
        if (a == nullptr) {
            return false;
        }
        std::vector<float> v;
        if (!numbers(*a, v) || v.empty() || v.size() > 2) {
            ctx.sink.warn(ctx.file, "<" + tag + ">: malformed \"x y\" pair \"" + *a + "\", ignored",
                          node.line, 0);
            return false;
        }
        x = v[0];
        y = v.size() == 2 ? v[1] : v[0];
        return true;
    };
    auto required = [&](bool present, const char* name) {
        if (!present) {
            ctx.sink.warn(ctx.file, "<" + tag + "> needs " + name, node.line, 0);
        }
    };
    auto keyword = [&](const char* attr, std::initializer_list<std::pair<const char*, uint8_t>> table,
                       uint8_t& out) {
        const std::string* a = attrOf(node, attr);
        if (a == nullptr) {
            return;
        }
        const std::string v = toLower(trimParseSvg(*a));
        for (const auto& [name, code] : table) {
            if (v == name) {
                out = code;
                return;
            }
        }
        ctx.sink.warn(ctx.file, "<" + tag + ">: unknown " + attr + " \"" + *a + "\", ignored", node.line, 0);
    };

    float* p = m.params;
    switch (kind) {
        case ModifierKind::Trim:
            p[1] = 1;
            num({"trim-start", "start"}, p[0]);
            num({"trim-end", "end"}, p[1]);
            num({"trim-offset", "offset"}, p[2]);
            keyword("mode", {{"parallel", 0}, {"simultaneous", 0}, {"serial", 1}, {"individual", 1}}, m.mode);
            break;
        case ModifierKind::Repeat: {
            p[0] = 1;
            p[7] = 1;
            required(num({"copies"}, p[0]), "copies");
            num({"repeat-offset", "offset"}, p[1]);
            pair({"repeat-anchor", "anchor"}, p[2], p[3]);
            pair({"repeat-translate", "translate"}, p[4], p[5]);
            num({"repeat-rotation", "rotate", "rotation"}, p[6]);
            float sx = 1, sy = 1;
            if (pair({"repeat-scale", "scale"}, sx, sy)) {
                p[7] = sx;
                if (sy != sx) {
                    ctx.sink.warn(ctx.file, "<xt-repeat>: non-uniform scale is not supported, x used",
                                  node.line, 0);
                }
            }
            num({"opacity-start"}, m.opacityA);
            num({"opacity-end"}, m.opacityB);
            keyword("order", {{"below", 0}, {"above", 1}}, m.mode);
            break;
        }
        case ModifierKind::Round:
            required(num({"radius"}, p[0]), "radius");
            break;
        case ModifierKind::Pucker:
            required(num({"amount"}, p[0]), "amount");
            break;
        case ModifierKind::ZigZag:
            required(num({"size"}, p[0]), "size");
            required(num({"ridges"}, p[1]), "ridges");
            keyword("points", {{"corner", 0}, {"smooth", 1}}, m.mode);
            break;
        case ModifierKind::Twist:
            required(num({"angle"}, p[0]), "angle");
            pair({"center"}, p[1], p[2]);
            break;
        case ModifierKind::Wiggle:
            // Unset optional parameters take After Effects' "Wiggle Paths" defaults.
            p[1] = 3;
            p[3] = 2;
            p[4] = 0.5f;
            required(num({"size"}, p[0]), "size");
            num({"detail"}, p[1]);
            num({"seed"}, p[2]);
            num({"speed"}, p[3]);
            num({"correlation"}, p[4]);
            num({"temporal-phase"}, p[5]);
            num({"spatial-phase"}, p[6]);
            keyword("points", {{"corner", 0}, {"smooth", 1}}, m.mode);
            break;
        case ModifierKind::Offset:
            p[1] = 4;
            required(num({"amount"}, p[0]), "amount");
            num({"miter"}, p[1]);
            keyword("join", {{"miter", 0}, {"round", 1}, {"bevel", 2}}, m.mode);
            break;
        case ModifierKind::Merge:
            keyword("mode", {{"merge", 0}, {"add", 1}, {"subtract", 2}, {"intersect", 3}, {"exclude", 4}},
                    m.mode);
            break;
    }
    float enabled = 1;
    if (num({"enabled"}, enabled)) {
        m.enabled = enabled != 0.0f ? 1 : 0;
    }
    if (kind == ModifierKind::Offset || kind == ModifierKind::Merge) {
        const std::string* baked = attrOf(node, "baked");
        if (baked == nullptr || trimParseSvg(*baked) != "1") {
            ctx.sink.warn(ctx.file,
                          "vector.unbakedModifier: <" + tag +
                              "> is only applied by the editor (bake it there); it draws nothing here",
                          node.line, 0);
        }
        m.enabled = 0;
    }
    return m;
}

/// T-225b: appends a Modifier node carrying `data` (no transform, opacity 1).
void emitModifier(BuildCtxParseSvg& ctx, const IrModifierData& data, uint32_t parent, StringId name) {
    IrSceneNode node{};
    node.kind = static_cast<uint8_t>(NodeKind::Modifier);
    node.visible = 1;
    node.parent = parent;
    node.kindIndex = static_cast<uint32_t>(ctx.scene.modifiers.size());
    node.opacity = 1;
    node.scale = Vec2{1, 1};
    node.name = name;
    node.svgTag = static_cast<uint8_t>(IrSvgTag::Modifier);
    ctx.scene.modifiers.push_back(data);
    ctx.scene.nodes.push_back(node);
}

/// T-236 (31 §31.6.1): an enabled parallel `<xt-trim>` whose scope (the earlier direct children
/// of the same group) is exactly one Path / Shape, with no modifier before it, is the same as that
/// path's own trim (the Painter trims a collected path before any modifier runs). It is written
/// into the path's per-path trim fields (offset: degrees -> turns) and produces no Modifier node;
/// its `id` names the path instead, so an `@timeline` `trim-*` track on it drives the path's trim.
/// Not demoted when the path already has a per-path trim of its own. Returns true when demoted.
bool demoteSinglePathTrim(BuildCtxParseSvg& ctx, const XmlNode& node, const IrModifierData& data,
                          uint32_t parent) {
    if (data.enabled == 0 || data.mode != 0) {
        return false;
    }
    uint32_t target = kInvalidNode;
    for (uint32_t i = parent + 1; i < ctx.scene.nodes.size(); ++i) {
        const IrSceneNode& sn = ctx.scene.nodes[i];
        if (sn.parent != parent) {
            continue;
        }
        const auto kind = static_cast<NodeKind>(sn.kind);
        if (kind == NodeKind::Modifier) {
            return false;  // Another modifier before it: the trim sees its output.
        }
        if (kind == NodeKind::Path || kind == NodeKind::Shape) {
            if (target != kInvalidNode) {
                return false;  // More than one path in scope.
            }
            target = i;
        }
    }
    if (target == kInvalidNode) {
        return false;
    }
    const IrSceneNode& sn = ctx.scene.nodes[target];
    float* start = nullptr;
    float* end = nullptr;
    float* offset = nullptr;
    if (static_cast<NodeKind>(sn.kind) == NodeKind::Path) {
        IrPathData& pd = ctx.scene.paths[sn.kindIndex];
        start = &pd.trimStart;
        end = &pd.trimEnd;
        offset = &pd.trimOffset;
    } else {
        IrShapeData& sd = ctx.scene.shapes[sn.kindIndex];
        start = &sd.trimStart;
        end = &sd.trimEnd;
        offset = &sd.trimOffset;
    }
    if (*start != 0.0f || *end != 1.0f || *offset != 0.0f) {
        return false;
    }
    *start = data.params[0];
    *end = data.params[1];
    *offset = data.params[2] / 360.0f;
    recordId(ctx, node, target);
    return true;
}

/// T-223 (31 §31.4.3, M24 transition): expands `<xt-star cx cy points r r2 rotation round round2>`
/// into a closed path. `r2` missing or 0 = a regular polygon with `points` corners; otherwise the
/// corners alternate outer (`r`) and inner (`r2`). The first outer corner points up (-90 deg) and
/// `rotation` (degrees) turns it clockwise. `round`/`round2` (0..1) round the outer/inner corners
/// with handles tangent to the corner's circle, `round` x one third of the adjacent edge long.
/// A missing `points`/`r` (or `points` < 3, `r` <= 0) warns (a failure under --strict) and draws
/// nothing. T-258b (D3): a non-finite `points` is an error (nothing drawn); more than
/// modifiers::kMaxGeneratedPoints (the run time's limit) is clamped to it with a warning.
bool buildStar(BuildCtxParseSvg& ctx, const XmlNode& node, SvgPath& out) {
    auto num = [&](const char* name, float& value) {
        const std::string* a = attrOf(node, name);
        bool percent = false;
        return a != nullptr && parseLengthOrPercent(*a, value, percent);
    };
    const std::string* pointsAttr = attrOf(node, "points");
    if (pointsAttr != nullptr && isNonFiniteNumber(*pointsAttr)) {
        ctx.sink.error(ctx.file, "<xt-star> points=\"" + *pointsAttr + "\" is not a finite number", node.line,
                       0);
        return false;
    }
    float points = 0, r = 0;
    if (!num("points", points) || !num("r", r)) {
        ctx.sink.warn(ctx.file, "<xt-star> needs points and r, skipped", node.line, 0);
        return false;
    }
    constexpr float kMaxPoints = static_cast<float>(modifiers::kMaxGeneratedPoints);
    if (points > kMaxPoints) {
        ctx.sink.warn(ctx.file,
                      "<xt-star> points clamped to " + std::to_string(modifiers::kMaxGeneratedPoints),
                      node.line, 0);
        points = kMaxPoints;
    }
    int n = static_cast<int>(std::lround(std::max(points, 0.0f)));
    if (n < 3 || r <= 0.0f) {
        ctx.sink.warn(ctx.file, "<xt-star> needs points >= 3 and r > 0, skipped", node.line, 0);
        return false;
    }
    float cx = 0, cy = 0, r2 = 0, rotation = 0, round = 0, round2 = 0;
    num("cx", cx);
    num("cy", cy);
    num("r2", r2);
    num("rotation", rotation);
    num("round", round);
    num("round2", round2);
    round = std::clamp(round, 0.0f, 1.0f);
    round2 = std::clamp(round2, 0.0f, 1.0f);
    const bool star = r2 > 0.0f;
    const int count = star ? n * 2 : n;
    const float step = 2.0f * kPi / static_cast<float>(count);
    const float start = (rotation - 90.0f) * kPi / 180.0f;

    // Rounded corners (same as `<xt-round>`): each corner with round > 0 is cut and replaced by two
    // anchors, one on each adjacent edge at d = round * min(adjacent edge lengths) / 2 from the corner,
    // joined by a cubic whose handles point back at the corner with length kKappa * d.
    constexpr float kKappa = 0.5523f;
    std::vector<Vec2> pts(static_cast<size_t>(count));
    std::vector<float> rounds(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        bool inner = star && (i % 2) == 1;
        float radius = inner ? r2 : r;
        float a = start + step * static_cast<float>(i);
        pts[static_cast<size_t>(i)] = Vec2{cx + radius * std::cos(a), cy + radius * std::sin(a)};
        rounds[static_cast<size_t>(i)] = inner ? round2 : round;
    }
    for (int i = 0; i < count; ++i) {
        const Vec2 p = pts[static_cast<size_t>(i)];
        const Vec2 prev = pts[static_cast<size_t>((i + count - 1) % count)];
        const Vec2 next = pts[static_cast<size_t>((i + 1) % count)];
        const float rd = rounds[static_cast<size_t>(i)];
        const float lp = std::sqrt((prev.x - p.x) * (prev.x - p.x) + (prev.y - p.y) * (prev.y - p.y));
        const float ln = std::sqrt((next.x - p.x) * (next.x - p.x) + (next.y - p.y) * (next.y - p.y));
        const float d = rd * std::min(lp, ln) / 2.0f;
        if (rd <= 0.0f || d <= 0.0f || lp <= 0.0f || ln <= 0.0f) {
            if (i == 0) {
                out.moveTo(p);
            } else {
                out.lineTo(p);
            }
            continue;
        }
        const Vec2 up{(prev.x - p.x) / lp, (prev.y - p.y) / lp};
        const Vec2 un{(next.x - p.x) / ln, (next.y - p.y) / ln};
        const Vec2 a{p.x + up.x * d, p.y + up.y * d};
        const Vec2 b{p.x + un.x * d, p.y + un.y * d};
        const float h = kKappa * d;
        if (i == 0) {
            out.moveTo(a);
        } else {
            out.lineTo(a);
        }
        out.cubicTo(Vec2{a.x - up.x * h, a.y - up.y * h}, Vec2{b.x - un.x * h, b.y - un.y * h}, b);
    }
    out.close();
    return true;
}

/// Builds the geometry of one basic shape. Returns false when the element is not a basic shape or
/// its geometry attributes are unusable.
bool buildShape(BuildCtxParseSvg& ctx, const XmlNode& node, SvgPath& out) {
    auto num = [&](const char* name, float fallback) {
        const std::string* a = attrOf(node, name);
        float value = fallback;
        bool percent = false;
        if (a != nullptr && parseLengthOrPercent(*a, value, percent)) {
            return value;
        }
        return fallback;
    };

    if (node.tag == "path") {
        const std::string* d = attrOf(node, "d");
        if (d == nullptr) {
            return false;
        }
        if (!parseSvgPathData(*d, out)) {
            ctx.sink.warn(ctx.file, "malformed path data, truncated", node.line, 0);
        }
        return !out.empty();
    }
    if (node.tag == "rect") {
        float x = num("x", 0), y = num("y", 0), w = num("width", 0), h = num("height", 0);
        if (w <= 0.0f || h <= 0.0f) {
            return false;
        }
        const std::string* rxAttr = attrOf(node, "rx");
        const std::string* ryAttr = attrOf(node, "ry");
        float rx = rxAttr != nullptr ? num("rx", 0) : (ryAttr != nullptr ? num("ry", 0) : 0.0f);
        float ry = ryAttr != nullptr ? num("ry", 0) : rx;
        rx = std::clamp(rx, 0.0f, w * 0.5f);
        ry = std::clamp(ry, 0.0f, h * 0.5f);
        if (rx <= 0.0f || ry <= 0.0f) {
            out.moveTo(Vec2{x, y});
            out.lineTo(Vec2{x + w, y});
            out.lineTo(Vec2{x + w, y + h});
            out.lineTo(Vec2{x, y + h});
            out.close();
            return true;
        }
        const float k = 0.5522847498f;  // Circular-arc Bezier constant (11.3.2).
        float ox = rx * k;
        float oy = ry * k;
        out.moveTo(Vec2{x + rx, y});
        out.lineTo(Vec2{x + w - rx, y});
        out.cubicTo(Vec2{x + w - rx + ox, y}, Vec2{x + w, y + ry - oy}, Vec2{x + w, y + ry});
        out.lineTo(Vec2{x + w, y + h - ry});
        out.cubicTo(Vec2{x + w, y + h - ry + oy}, Vec2{x + w - rx + ox, y + h}, Vec2{x + w - rx, y + h});
        out.lineTo(Vec2{x + rx, y + h});
        out.cubicTo(Vec2{x + rx - ox, y + h}, Vec2{x, y + h - ry + oy}, Vec2{x, y + h - ry});
        out.lineTo(Vec2{x, y + ry});
        out.cubicTo(Vec2{x, y + ry - oy}, Vec2{x + rx - ox, y}, Vec2{x + rx, y});
        out.close();
        return true;
    }
    if (node.tag == "circle" || node.tag == "ellipse") {
        float cx = num("cx", 0), cy = num("cy", 0);
        float rx, ry;
        if (node.tag == "circle") {
            rx = ry = num("r", 0);
        } else {
            rx = num("rx", 0);
            ry = num("ry", 0);
        }
        if (rx <= 0.0f || ry <= 0.0f) {
            return false;
        }
        const float k = 0.5522847498f;
        float ox = rx * k;
        float oy = ry * k;
        out.moveTo(Vec2{cx + rx, cy});
        out.cubicTo(Vec2{cx + rx, cy + oy}, Vec2{cx + ox, cy + ry}, Vec2{cx, cy + ry});
        out.cubicTo(Vec2{cx - ox, cy + ry}, Vec2{cx - rx, cy + oy}, Vec2{cx - rx, cy});
        out.cubicTo(Vec2{cx - rx, cy - oy}, Vec2{cx - ox, cy - ry}, Vec2{cx, cy - ry});
        out.cubicTo(Vec2{cx + ox, cy - ry}, Vec2{cx + rx, cy - oy}, Vec2{cx + rx, cy});
        out.close();
        return true;
    }
    if (node.tag == "line") {
        out.moveTo(Vec2{num("x1", 0), num("y1", 0)});
        out.lineTo(Vec2{num("x2", 0), num("y2", 0)});
        return true;
    }
    if (node.tag == "polyline" || node.tag == "polygon") {
        const std::string* pts = attrOf(node, "points");
        if (pts == nullptr) {
            return false;
        }
        std::vector<float> values = parseNumberList(*pts);
        if (values.size() < 4) {
            return false;
        }
        out.moveTo(Vec2{values[0], values[1]});
        for (size_t i = 2; i + 1 < values.size(); i += 2) {
            out.lineTo(Vec2{values[i], values[i + 1]});
        }
        if (node.tag == "polygon") {
            out.close();
        }
        return true;
    }
    if (node.tag == "xt-star") {
        return buildStar(ctx, node, out);
    }
    return false;
}

bool isUnsupportedElement(std::string_view tag) {
    return tag == "text" || tag == "tspan" || tag == "textPath" || tag == "image" || tag == "filter" ||
           tag == "pattern" || tag == "style" || tag == "switch" || tag == "foreignObject" || tag == "marker";
}

bool isIgnoredElement(std::string_view tag) {
    return tag == "title" || tag == "desc" || tag == "metadata" || tag == "linearGradient" ||
           tag == "radialGradient" || tag == "stop" || tag == "symbol" || tag == "animate" ||
           tag == "animateTransform" || tag == "animateMotion" || tag == "set" ||
           // T-225b (31 §31.5.3): drawn only through `clip-path="url(#id)"` / `mask="url(#id)"`.
           tag == "clipPath" || tag == "mask";
}

void buildElement(BuildCtxParseSvg& ctx, uint32_t nodeIdx, uint32_t parent, SvgStyle style, int depth);

void buildChildren(BuildCtxParseSvg& ctx, const XmlNode& node, uint32_t parent, const SvgStyle& style,
                   int depth) {
    for (uint32_t child : node.children) {
        buildElement(ctx, child, parent, style, depth + 1);
    }
}

/// T-225b (31 §31.5.3): the `<clipPath>`/`<mask>` (`wantTag`) that property `name`
/// (`clip-path`/`mask`) of `node` references as `url(#id)`; false (with a warning when it is
/// malformed or dangling) when there is none.
bool maskRefOf(BuildCtxParseSvg& ctx, const XmlNode& node, std::string_view name, std::string_view wantTag,
               uint32_t& out) {
    std::string v;
    if (!propertyOf(node, name, v)) {
        return false;
    }
    std::string_view s = trimParseSvg(v);
    if (s.empty() || s == "none") {
        return false;
    }
    std::string id;
    if (s.rfind("url(", 0) == 0 && s.back() == ')') {
        std::string_view inner = trimParseSvg(s.substr(4, s.size() - 5));
        if (inner.size() >= 2 && (inner.front() == '"' || inner.front() == '\'') &&
            inner.back() == inner.front()) {
            inner = inner.substr(1, inner.size() - 2);
        }
        if (!inner.empty() && inner.front() == '#') {
            id.assign(inner.substr(1));
        }
    }
    const std::string what(name);
    if (id.empty()) {
        ctx.sink.warn(ctx.file, what + " \"" + v + "\" is not a local url(#id), ignored", node.line, 0);
        return false;
    }
    auto it = ctx.byId.find(id);
    if (it == ctx.byId.end() || ctx.nodes[it->second].tag != wantTag) {
        ctx.sink.warn(ctx.file,
                      what + " target \"#" + id + "\" is not a <" + std::string(wantTag) + ">, ignored",
                      node.line, 0);
        return false;
    }
    out = it->second;
    return true;
}

/// T-225b (31 §31.5.3): wraps what follows in a Group whose first child is a Mask node holding a
/// copy of `<clipPath>`/`<mask>` `defIdx`'s content (MaskMode::Intersect for a clip path; Luma, or
/// Alpha with `mask-type="alpha"`, for a mask): the Painter's mask run then masks the Group's
/// later children. The Mask node takes `local` (the referencing element's own transform: the
/// content is in its user space) times a clip path's own `transform`. Returns the Group.
uint32_t emitMaskLayer(BuildCtxParseSvg& ctx, uint32_t parent, uint32_t defIdx, bool clip,
                       const Mat2x3& local, int depth) {
    const XmlNode& def = ctx.nodes[defIdx];
    const uint32_t group = emitGroup(ctx, parent, Mat2x3{}, 1.0f, kInvalidString);

    Mat2x3 m = local;
    MaskMode mode = MaskMode::Intersect;
    std::string v;
    if (clip) {
        if (propertyOf(def, "clipPathUnits", v) && trimParseSvg(v) == "objectBoundingBox") {
            ctx.sink.warn(ctx.file,
                          "clipPathUnits=\"objectBoundingBox\" is not supported, userSpaceOnUse used",
                          def.line, 0);
        }
        Mat2x3 own;
        if (propertyOf(def, "transform", v)) {
            if (!parseTransform(v, own)) {
                ctx.sink.warn(ctx.file, "malformed transform \"" + v + "\", partially applied", def.line, 0);
            }
            m = mul(local, own);
        }
    } else {
        mode = MaskMode::Luma;
        if (propertyOf(def, "mask-type", v) && toLower(trimParseSvg(v)) == "alpha") {
            mode = MaskMode::Alpha;
        }
        if (propertyOf(def, "maskContentUnits", v) && trimParseSvg(v) == "objectBoundingBox") {
            ctx.sink.warn(ctx.file,
                          "maskContentUnits=\"objectBoundingBox\" is not supported, userSpaceOnUse used",
                          def.line, 0);
        }
    }

    IrSceneNode mask{};
    mask.kind = static_cast<uint8_t>(NodeKind::Mask);
    mask.visible = 1;
    mask.parent = group;
    mask.opacity = 1;
    mask.scale = Vec2{1, 1};
    mask.name = kInvalidString;
    mask.maskMode = static_cast<uint8_t>(mode);
    applyTransform(mask, m);
    const uint32_t maskIndex = static_cast<uint32_t>(ctx.scene.nodes.size());
    ctx.scene.nodes.push_back(mask);

    SvgStyle style;
    applyStyle(ctx, def, style);
    const bool savedInDef = ctx.inDef;
    const bool savedClip = ctx.clipContent;
    ctx.inDef = true;
    ctx.clipContent = clip;
    buildChildren(ctx, def, maskIndex, style, depth);
    ctx.inDef = savedInDef;
    ctx.clipContent = savedClip;
    return group;
}

/// T-225b (31 §31.5.3): applies `node`'s `clip-path` and `mask` references: each one wraps the
/// element in one more Group layer (clip outside, mask inside). Returns the parent the element
/// itself goes under (`parent` when it has neither). Inside clip/mask content they are ignored.
uint32_t wrapClipAndMask(BuildCtxParseSvg& ctx, const XmlNode& node, uint32_t parent, const Mat2x3& local,
                         int depth) {
    std::string v;
    if (ctx.inDef) {
        if (propertyOf(node, "clip-path", v) || propertyOf(node, "mask", v)) {
            ctx.sink.warn(ctx.file,
                          "clip-path/mask inside <clipPath>/<mask> content is not supported, ignored",
                          node.line, 0);
        }
        return parent;
    }
    uint32_t def = 0;
    if (maskRefOf(ctx, node, "clip-path", "clipPath", def)) {
        parent = emitMaskLayer(ctx, parent, def, /*clip=*/true, local, depth);
    }
    if (maskRefOf(ctx, node, "mask", "mask", def)) {
        parent = emitMaskLayer(ctx, parent, def, /*clip=*/false, local, depth);
    }
    return parent;
}

// ---------------------------------------------------------------------------------------------
// T-243: rig elements (Docs/design/32-rigging-and-deformation.md §32.2)
// ---------------------------------------------------------------------------------------------

/// The child XmlNode of `node` with tag `tag`, or 0xFFFFFFFF.
uint32_t childByTag(const BuildCtxParseSvg& ctx, const XmlNode& node, std::string_view tag) {
    for (uint32_t child : node.children) {
        if (ctx.nodes[child].tag == tag) {
            return child;
        }
    }
    return 0xFFFFFFFFu;
}

/// `name` as a number, `def` when absent; a malformed value warns and keeps `def`.
float rigNumber(BuildCtxParseSvg& ctx, const XmlNode& node, std::string_view name, float def,
                bool* present = nullptr) {
    const std::string* a = attrOf(node, name);
    if (present != nullptr) {
        *present = a != nullptr;
    }
    if (a == nullptr) {
        return def;
    }
    std::string_view v = trimParseSvg(*a);
    if (v == "true") {
        return 1.0f;
    }
    if (v == "false") {
        return 0.0f;
    }
    float out = 0;
    if (!parseNumberParseSvg(v, out)) {
        ctx.sink.warn(ctx.file,
                      "<" + node.tag + "> " + std::string(name) + "=\"" + *a + "\" is not a number, ignored",
                      node.line, 0);
        return def;
    }
    return out;
}

/// The scene node index an `id` names (recordId()), or 0xFFFFFFFF. A leading `#` is dropped.
uint32_t rigNodeById(const BuildCtxParseSvg& ctx, std::string_view id) {
    if (!id.empty() && id.front() == '#') {
        id.remove_prefix(1);
    }
    for (const auto& [name, index] : ctx.allIds) {
        if (name == id) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}

/// The local transform instantiateScene() gives scene node `n`.
Mat2x3 nodeLocal(const IrSceneNode& n) {
    return composeTRSS(n.translate, n.rotation, n.skew, n.scale, n.anchor);
}

/// `<xt-bone length rotate translate scale id>` (32 §32.2.1) under an `<xt-skeleton>` or another
/// bone. A child bone's translate x is its parent's length (`skin.boneOffset` warning when the
/// file says otherwise). Its children may only be bones and constraints.
void buildBone(BuildCtxParseSvg& ctx, uint32_t nodeIdx, uint32_t parent, const SvgStyle& style, int depth) {
    const XmlNode& node = ctx.nodes[nodeIdx];
    const IrSceneNode parentNode = ctx.scene.nodes[parent];
    const bool underBone = parentNode.kind == static_cast<uint8_t>(NodeKind::Bone);
    if (!underBone && parentNode.svgTag != static_cast<uint8_t>(IrSvgTag::XtSkeleton)) {
        ctx.sink.warn(ctx.file, "<xt-bone> outside <xt-skeleton>, skipped", node.line, 0);
        return;
    }
    IrBoneData bone{};
    bone.length = rigNumber(ctx, node, "length", 0.0f);
    Vec2 translate{0, 0};
    Vec2 scale{1, 1};
    bool hasTranslate = false;
    if (const std::string* t = attrOf(node, "translate")) {
        std::vector<float> v = parseNumberList(*t);
        if (v.empty() || v.size() > 2) {
            ctx.sink.warn(ctx.file, "<xt-bone> translate=\"" + *t + "\" is malformed, ignored", node.line, 0);
        } else {
            translate = Vec2{v[0], v.size() == 2 ? v[1] : 0.0f};
            hasTranslate = true;
        }
    }
    if (const std::string* s = attrOf(node, "scale")) {
        std::vector<float> v = parseNumberList(*s);
        if (v.empty() || v.size() > 2) {
            ctx.sink.warn(ctx.file, "<xt-bone> scale=\"" + *s + "\" is malformed, ignored", node.line, 0);
        } else {
            scale = Vec2{v[0], v.size() == 2 ? v[1] : v[0]};
        }
    }
    if (underBone) {
        const float parentLength = ctx.scene.bones[parentNode.kindIndex].length;
        // T-244: y too -- the run time places a child bone at (parent length, 0) whatever the file
        // says.
        if (hasTranslate && (translate.x != parentLength || translate.y != 0.0f)) {
            ctx.sink.warn(ctx.file,
                          "skin.boneOffset: a child bone sits at its parent's tip; translate " +
                              std::to_string(translate.x) + " " + std::to_string(translate.y) +
                              " is ignored (x becomes the parent length " + std::to_string(parentLength) +
                              ", y 0 at run time)",
                          node.line, 0);
        }
        translate.x = parentLength;
        translate.y = 0.0f;  // T-243b: the same (parent length, 0) the run time uses.
    }

    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Bone);
    n.visible = 1;
    n.parent = parent;
    n.kindIndex = static_cast<uint32_t>(ctx.scene.bones.size());
    n.opacity = 1;
    n.translate = translate;
    n.rotation = rigNumber(ctx, node, "rotate", 0.0f);
    n.scale = scale;

    // T-249 (32 §32.2.1): the bind pose. Each `rest-*` defaults to its posed counterpart, so a
    // file without them binds at the pose it shows (the old meaning).
    BuildCtxParseSvg::BoneRest rest;
    rest.length = rigNumber(ctx, node, "rest-length", bone.length);
    Vec2 restTranslate = translate;
    if (const std::string* t = attrOf(node, "rest-translate")) {
        std::vector<float> v = parseNumberList(*t);
        if (v.empty() || v.size() > 2) {
            ctx.sink.warn(ctx.file, "<xt-bone> rest-translate=\"" + *t + "\" is malformed, ignored",
                          node.line, 0);
        } else if (!underBone) {
            restTranslate = Vec2{v[0], v.size() == 2 ? v[1] : 0.0f};
        } else {
            const float parentRestLength = ctx.boneRests[parentNode.kindIndex].length;
            if (v[0] != parentRestLength || (v.size() == 2 && v[1] != 0.0f)) {
                ctx.sink.warn(ctx.file,
                              "skin.boneOffset: a child bone sits at its parent's tip; rest-translate \"" +
                                  *t + "\" is ignored (x becomes the parent rest length " +
                                  std::to_string(parentRestLength) + ", y 0)",
                              node.line, 0);
            }
        }
    }
    if (underBone) {
        restTranslate = Vec2{ctx.boneRests[parentNode.kindIndex].length, 0.0f};
    }
    Vec2 restScale = scale;
    if (const std::string* s = attrOf(node, "rest-scale")) {
        std::vector<float> v = parseNumberList(*s);
        if (v.empty() || v.size() > 2) {
            ctx.sink.warn(ctx.file, "<xt-bone> rest-scale=\"" + *s + "\" is malformed, ignored", node.line,
                          0);
        } else {
            restScale = Vec2{v[0], v.size() == 2 ? v[1] : v[0]};
        }
    }
    const float restRotate = rigNumber(ctx, node, "rest-rotate", n.rotation);
    rest.local = composeTRSS(restTranslate, restRotate, Vec2{0, 0}, restScale, Vec2{0, 0});
    if (ctx.boneRests.size() <= n.kindIndex) {
        ctx.boneRests.resize(n.kindIndex + 1);
    }
    ctx.boneRests[n.kindIndex] = rest;
    n.svgTag = static_cast<uint8_t>(IrSvgTag::XtBone);
    const uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
    n.name = recordId(ctx, node, index);
    if (ctx.extras != nullptr && n.name != kInvalidString && !ctx.inUse && !ctx.inDef) {
        ctx.extras->boneIds.emplace_back(*attrOf(node, "id"), node.line);  // T-243b
    }
    ctx.scene.bones.push_back(bone);
    ctx.scene.nodes.push_back(n);

    for (uint32_t child : node.children) {
        const std::string& tag = ctx.nodes[child].tag;
        if (tag == "xt-bone" || tag == "xt-constraint") {
            buildElement(ctx, child, index, style, depth + 1);
        } else {
            ctx.sink.warn(ctx.file, "<" + tag + "> inside <xt-bone> is not supported, skipped",
                          ctx.nodes[child].line, 0);
        }
    }
}

/// `<xt-constraint kind target strength invert chain mix space inertia damping gravity enabled>`
/// (32 §32.2.3). An unknown kind, or an ik / transform / rotation constraint without `target`,
/// is an error; the target is resolved by finishRig().
void buildConstraint(BuildCtxParseSvg& ctx, const XmlNode& node, uint32_t parent) {
    const std::string* kindAttr = attrOf(node, "kind");
    const std::string kindName = kindAttr != nullptr ? toLower(trimParseSvg(*kindAttr)) : std::string();
    ConstraintKind kind{};
    if (kindName == "ik") {
        kind = ConstraintKind::Ik;
    } else if (kindName == "transform") {
        kind = ConstraintKind::Transform;
    } else if (kindName == "rotation") {
        kind = ConstraintKind::Rotation;
    } else if (kindName == "spring") {
        kind = ConstraintKind::Spring;
    } else {
        ctx.sink.error(ctx.file,
                       "<xt-constraint> kind=\"" + kindName + "\" is not ik, transform, rotation or spring",
                       node.line, 0);
        return;
    }
    IrConstraintData c{};
    c.kind = static_cast<uint8_t>(kind);
    c.enabled = rigNumber(ctx, node, "enabled", 1.0f) >= 0.5f ? 1 : 0;
    c.target = kIrNoTarget;
    c.mix = rigNumber(ctx, node, "mix", 1.0f);
    if (const std::string* space = attrOf(node, "space")) {
        const std::string s = toLower(trimParseSvg(*space));
        if (s == "world") {
            c.space = 1;
        } else if (s != "local") {
            ctx.sink.warn(ctx.file,
                          "<xt-constraint> space=\"" + *space + "\" is not local or world, local used",
                          node.line, 0);
        }
    }
    if (kind == ConstraintKind::Ik) {
        c.params[0] = rigNumber(ctx, node, "strength", 1.0f);
        c.params[1] = rigNumber(ctx, node, "invert", 0.0f);
        c.params[2] = rigNumber(ctx, node, "chain", 2.0f);
    } else if (kind == ConstraintKind::Spring) {
        // T-244: 32 §32.2.3's defaults (inertia 1, strength 60, damping 8, gravity 0).
        c.params[0] = rigNumber(ctx, node, "inertia", 1.0f);
        c.params[1] = rigNumber(ctx, node, "strength", 60.0f);
        c.params[2] = rigNumber(ctx, node, "damping", 8.0f);
        c.params[3] = rigNumber(ctx, node, "gravity", 0.0f);
    }
    const std::string* target = attrOf(node, "target");
    if (kind != ConstraintKind::Spring && (target == nullptr || trimParseSvg(*target).empty())) {
        ctx.sink.error(ctx.file, "<xt-constraint kind=\"" + kindName + "\"> needs a target", node.line, 0);
        return;
    }

    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Constraint);
    n.visible = 1;
    n.parent = parent;
    n.kindIndex = static_cast<uint32_t>(ctx.scene.constraints.size());
    n.opacity = 1;
    n.scale = Vec2{1, 1};
    n.svgTag = static_cast<uint8_t>(IrSvgTag::XtConstraint);
    const uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
    n.name = recordId(ctx, node, index);
    if (kind != ConstraintKind::Spring) {
        ctx.pendingTargets.push_back(
            BuildCtxParseSvg::PendingTarget{n.kindIndex, std::string(trimParseSvg(*target)), node.line});
    }
    ctx.scene.constraints.push_back(c);
    ctx.scene.nodes.push_back(n);
}

/// `<xt-pins>`: `x,y` pairs separated by whitespace. False on a malformed pair.
bool parsePins(std::string_view text, std::vector<Vec2>& out) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isXmlSpace(text[i])) {
            ++i;
        }
        size_t begin = i;
        while (i < text.size() && !isXmlSpace(text[i])) {
            ++i;
        }
        if (i == begin) {
            break;
        }
        std::string_view pair = text.substr(begin, i - begin);
        size_t comma = pair.find(',');
        float x = 0, y = 0;
        if (comma == std::string_view::npos || !parseNumberParseSvg(pair.substr(0, comma), x) ||
            !parseNumberParseSvg(pair.substr(comma + 1), y)) {
            return false;
        }
        out.push_back(Vec2{x, y});
    }
    return true;
}

/// `<xt-tris>`: whitespace separated vertex indices. False on a malformed index.
bool parseTris(std::string_view text, std::vector<uint32_t>& out) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (isXmlSpace(text[i]) || text[i] == ',')) {
            ++i;
        }
        size_t begin = i;
        while (i < text.size() && !isXmlSpace(text[i]) && text[i] != ',') {
            ++i;
        }
        if (i == begin) {
            break;
        }
        uint32_t v = 0;
        std::from_chars_result r = std::from_chars(text.data() + begin, text.data() + i, v);
        if (r.ec != std::errc{} || r.ptr != text.data() + i) {
            return false;
        }
        out.push_back(v);
    }
    return true;
}

/// The ASST index of an `<xt-mesh href>`: the caller's image registration, else an entry under
/// the href's own name (shared with any image of that name).
AssetId meshAsset(BuildCtxParseSvg& ctx, const std::string& href, int line) {
    if (ctx.extras != nullptr && ctx.extras->imageAsset) {
        return ctx.extras->imageAsset(href, line);
    }
    const std::vector<AssetBuilder>& existing = ctx.writer.assets();
    for (size_t i = 0; i < existing.size(); ++i) {
        if (existing[i].kind == 0 &&
            (existing[i].sourceName == href ||
             (existing[i].sourceName.empty() && ctx.writer.stringAt(existing[i].name) == href))) {
            return static_cast<AssetId>(i);
        }
    }
    AssetBuilder ab;
    ab.kind = 0;
    ab.name = ctx.writer.intern(href);
    ab.sourceName = href;
    return ctx.writer.addAsset(std::move(ab));
}

/// `<xt-mesh href x y width height opacity smooth id transform>` with `<xt-pins>` (default: the
/// four corners of the image rectangle; fewer than 3 is an error), `<xt-tris>` (default: Delaunay
/// of the pins) and `<xt-weights>` (32 §32.2.4). The pins are in the mesh's local space (its
/// `transform` is the node's, like any shape); T-249: the uvs are (pin - (x, y)) over (width,
/// height), clamped to 0..1 (`x` / `y` default 0).
void buildMesh(BuildCtxParseSvg& ctx, uint32_t nodeIdx, uint32_t parent, const Mat2x3& local) {
    const XmlNode& node = ctx.nodes[nodeIdx];
    const std::string* href = attrOf(node, "href");
    if (href == nullptr || trimParseSvg(*href).empty()) {
        ctx.sink.error(ctx.file, "<xt-mesh> needs an href", node.line, 0);
        return;
    }
    const float width = rigNumber(ctx, node, "width", 0.0f);
    const float height = rigNumber(ctx, node, "height", 0.0f);
    if (!(width > 0.0f) || !(height > 0.0f)) {
        ctx.sink.error(ctx.file, "<xt-mesh> needs a positive width and height", node.line, 0);
        return;
    }
    const float originX = rigNumber(ctx, node, "x", 0.0f);  // T-249: the image rectangle's origin.
    const float originY = rigNumber(ctx, node, "y", 0.0f);
    std::vector<Vec2> pins;
    const uint32_t pinsNode = childByTag(ctx, node, "xt-pins");
    if (pinsNode == 0xFFFFFFFFu) {
        pins = {Vec2{originX, originY}, Vec2{originX + width, originY},
                Vec2{originX + width, originY + height}, Vec2{originX, originY + height}};
    } else if (!parsePins(ctx.nodes[pinsNode].text, pins)) {
        ctx.sink.error(ctx.file, "<xt-pins> is not a list of x,y pairs", ctx.nodes[pinsNode].line, 0);
        return;
    } else if (pins.size() < 3) {
        ctx.sink.error(ctx.file, "<xt-pins> needs at least 3 pins", ctx.nodes[pinsNode].line, 0);
        return;
    } else if (pins.size() > 65536) {
        ctx.sink.error(ctx.file, "<xt-pins> has more than 65536 pins", ctx.nodes[pinsNode].line, 0);
        return;
    }
    std::vector<uint16_t> indices;
    const uint32_t trisNode = childByTag(ctx, node, "xt-tris");
    if (trisNode == 0xFFFFFFFFu) {
        if (!triangulate(pins, indices)) {
            ctx.sink.error(ctx.file, "mesh.degenerate: <xt-mesh> pins are collinear, no triangle", node.line,
                           0);
            return;
        }
    } else {
        std::vector<uint32_t> tris;
        const int line = ctx.nodes[trisNode].line;
        if (!parseTris(ctx.nodes[trisNode].text, tris) || tris.empty() || tris.size() % 3 != 0) {
            ctx.sink.error(ctx.file, "<xt-tris> is not a list of index triples", line, 0);
            return;
        }
        for (uint32_t v : tris) {
            if (v >= pins.size()) {
                ctx.sink.error(ctx.file, "<xt-tris> index " + std::to_string(v) + " is not a pin", line, 0);
                return;
            }
            indices.push_back(static_cast<uint16_t>(v));
        }
    }
    for (uint32_t child : node.children) {
        const std::string& tag = ctx.nodes[child].tag;
        if (tag != "xt-pins" && tag != "xt-tris" && tag != "xt-weights") {
            ctx.sink.warn(ctx.file, "<" + tag + "> inside <xt-mesh> is not supported, skipped",
                          ctx.nodes[child].line, 0);
        }
    }

    IrMeshData m{};
    m.asset = meshAsset(ctx, std::string(trimParseSvg(*href)), node.line);
    m.posOffset = static_cast<uint32_t>(ctx.scene.meshVec2Pool.size());
    m.vertexCount = static_cast<uint32_t>(pins.size());
    for (const Vec2& pin : pins) {
        ctx.scene.meshVec2Pool.push_back(pin);
    }
    for (const Vec2& pin : pins) {
        // T-258b (D10): width / height are finite and positive, but a far pin can still overflow.
        ctx.scene.meshVec2Pool.push_back(
            Vec2{std::clamp(xt::finiteOr((pin.x - originX) / width, 0.0f), 0.0f, 1.0f),
                 std::clamp(xt::finiteOr((pin.y - originY) / height, 0.0f), 0.0f, 1.0f)});
    }
    m.indexOffset = static_cast<uint32_t>(ctx.scene.meshIndexPool.size());
    m.indexCount = static_cast<uint32_t>(indices.size());
    ctx.scene.meshIndexPool.insert(ctx.scene.meshIndexPool.end(), indices.begin(), indices.end());
    m.opacity = rigNumber(ctx, node, "opacity", 1.0f);
    m.smooth = rigNumber(ctx, node, "smooth", 1.0f) >= 0.5f ? 1 : 0;
    m.skinIndex = kIrNoSkin;

    IrSceneNode n{};
    n.kind = static_cast<uint8_t>(NodeKind::Mesh);
    n.visible = 1;
    n.parent = parent;
    n.kindIndex = static_cast<uint32_t>(ctx.scene.meshes.size());
    n.opacity = 1;  // The `opacity` attribute is the mesh's own (IrMeshData::opacity).
    n.scale = Vec2{1, 1};
    applyTransform(n, local);
    n.svgTag = static_cast<uint8_t>(IrSvgTag::XtMesh);
    const uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
    n.name = recordId(ctx, node, index);
    ctx.scene.meshes.push_back(m);
    ctx.scene.nodes.push_back(n);
    if (!ctx.inUse && !ctx.inDef &&
        (attrOf(node, "xt-skin") != nullptr || childByTag(ctx, node, "xt-weights") != 0xFFFFFFFFu)) {
        ctx.pendingSkins.push_back(BuildCtxParseSvg::PendingSkin{index, nodeIdx, m.vertexCount, {}});
    }
}

/// One column of an `<xt-weights>` line: up to 4 `<bone>:<weight>` items. False when malformed;
/// `boneRange` is set when a bone index is out of range.
bool parseWeightColumn(std::string_view text, size_t boneCount, std::vector<std::pair<uint32_t, float>>& out,
                       bool& boneRange) {
    out.clear();
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isXmlSpace(text[i])) {
            ++i;
        }
        size_t begin = i;
        while (i < text.size() && !isXmlSpace(text[i])) {
            ++i;
        }
        if (i == begin) {
            break;
        }
        std::string_view item = text.substr(begin, i - begin);
        size_t colon = item.find(':');
        uint32_t bone = 0;
        float w = 0;
        if (colon == std::string_view::npos) {
            return false;
        }
        std::from_chars_result r = std::from_chars(item.data(), item.data() + colon, bone);
        if (r.ec != std::errc{} || r.ptr != item.data() + colon ||
            !parseNumberParseSvg(item.substr(colon + 1), w) || !(w >= 0.0f)) {
            return false;
        }
        if (bone >= boneCount) {
            boneRange = true;
            return false;
        }
        out.emplace_back(bone, w);
    }
    return !out.empty() && out.size() <= 4;
}

/// A normalized PointWeight of `items` (sum > 0): weights to 8 bits, the rounding remainder on the
/// heaviest item so they sum to 255.
IrPointWeight packWeights(const std::vector<std::pair<uint32_t, float>>& items) {
    float sum = 0;
    for (const auto& it : items) {
        sum += it.second;
    }
    IrPointWeight pw{};
    int q[4] = {0, 0, 0, 0};
    int total = 0;
    size_t heaviest = 0;
    for (size_t k = 0; k < items.size(); ++k) {
        q[k] = static_cast<int>(std::lround(items[k].second / sum * 255.0f));
        total += q[k];
        if (items[k].second > items[heaviest].second) {
            heaviest = k;
        }
    }
    q[heaviest] += 255 - total;
    for (size_t k = 0; k < items.size(); ++k) {
        pw.idx4 |= (items[k].first & 0xFFu) << (8 * k);
        pw.w4 |= (static_cast<uint32_t>(std::clamp(q[k], 0, 255)) & 0xFFu) << (8 * k);
    }
    return pw;
}

/// T-243c (32 §32.2.2): the `<xt-weights>` vertex order of `path` -- each contour's on-curve
/// vertices in turn (its start point is vertex 0, every Line / Cubic end the next one), contours
/// continuing the count. A closed contour's last segment ending on its start point adds no vertex
/// (it is vertex 0), with the editor's merge rule (vector_geometry `mergeClosing`: the contour has
/// at least 3 on-curve points counting that end, and the end is within 1e-3 of the start). A
/// segment after `Z` with no `M` starts a contour at the previous start point, which takes a
/// vertex of its own (as in the editor) but has no slot. `slotVertex` gets one entry per non-Close
/// verb (the IR's weight slots): that slot's vertex. Returns the number of vertices.
uint32_t weightVertexOrder(const SvgPath& path, std::vector<uint32_t>& slotVertex) {
    slotVertex.clear();
    uint32_t vertices = 0;
    size_t pi = 0;                  // Float index into points.
    bool open = false;              // A contour is in progress.
    uint32_t first = 0;             // The current contour's vertex 0.
    uint32_t onCurve = 0;           // The current contour's on-curve points so far.
    Vec2 start{};                   // The current contour's start point.
    Vec2 end{};                     // The last on-curve point.
    size_t lastSegment = SIZE_MAX;  // Slot of the contour's last Line / Cubic, SIZE_MAX if none.
    auto pointAt = [&](size_t index) {
        return index + 1 < path.points.size() ? Vec2{path.points[index], path.points[index + 1]} : Vec2{};
    };
    for (uint8_t verb : path.verbs) {
        switch (static_cast<PathVerb>(verb)) {
            case PathVerb::Move:
                start = end = pointAt(pi);
                pi += 2;
                first = vertices++;
                onCurve = 1;
                open = true;
                lastSegment = SIZE_MAX;
                slotVertex.push_back(first);
                break;
            case PathVerb::Line:
            case PathVerb::Cubic: {
                if (!open) {
                    first = vertices++;
                    onCurve = 1;
                    open = true;
                    lastSegment = SIZE_MAX;
                }
                pi += verb == static_cast<uint8_t>(PathVerb::Cubic) ? 4 : 0;
                end = pointAt(pi);
                pi += 2;
                lastSegment = slotVertex.size();
                slotVertex.push_back(vertices++);
                ++onCurve;
                break;
            }
            case PathVerb::Close:
                if (open && lastSegment != SIZE_MAX && onCurve >= 3) {
                    const float dx = end.x - start.x;
                    const float dy = end.y - start.y;
                    if (std::sqrt(dx * dx + dy * dy) < 1e-3f) {
                        slotVertex[lastSegment] = first;  // The closing end is vertex 0.
                        --vertices;
                    }
                }
                open = false;
                end = start;
                break;
        }
    }
    return vertices;
}

/// Builds the skin of pending Path / Mesh `ps` (32 §32.2.2): `xt-skin` names the skeleton,
/// `<xt-weights bones version>` the bones (one tendon each) and the per-vertex weights; each
/// tendon's inverse bind is inverse(bone rest world) x the shape's own transform (T-243b), the
/// rest world being the product of the file's local transforms from the bone up to, not
/// including, the skeleton group (the run time's bone world is relative to that group).
void buildSkin(BuildCtxParseSvg& ctx, const BuildCtxParseSvg::PendingSkin& ps) {
    const XmlNode& node = ctx.nodes[ps.xmlNode];
    const bool mesh = ctx.scene.nodes[ps.owner].kind == static_cast<uint8_t>(NodeKind::Mesh);
    const std::string* skinRef = attrOf(node, "xt-skin");
    const uint32_t weightsIdx = childByTag(ctx, node, "xt-weights");
    if (skinRef == nullptr) {
        ctx.sink.warn(ctx.file, "<xt-weights> without xt-skin on its <" + node.tag + ">, not skinned",
                      node.line, 0);
        return;
    }
    if (weightsIdx == 0xFFFFFFFFu) {
        ctx.sink.warn(ctx.file, "xt-skin without <xt-weights>, not skinned", node.line, 0);
        return;
    }
    const XmlNode& weights = ctx.nodes[weightsIdx];
    const uint32_t skeleton = rigNodeById(ctx, trimParseSvg(*skinRef));
    if (skeleton == 0xFFFFFFFFu ||
        ctx.scene.nodes[skeleton].svgTag != static_cast<uint8_t>(IrSvgTag::XtSkeleton)) {
        ctx.sink.error(ctx.file, "xt-skin=\"" + *skinRef + "\" is not an <xt-skeleton>", node.line, 0);
        return;
    }
    if (const std::string* version = attrOf(weights, "version");
        version != nullptr && trimParseSvg(*version) != "1") {
        ctx.sink.warn(ctx.file, "<xt-weights> version=\"" + *version + "\" is not supported, read as 1",
                      weights.line, 0);
    }
    // The bones, one tendon each, in `bones` order.
    std::vector<uint32_t> bones;
    if (const std::string* list = attrOf(weights, "bones")) {
        std::string_view s = *list;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && isXmlSpace(s[i])) {
                ++i;
            }
            size_t begin = i;
            while (i < s.size() && !isXmlSpace(s[i])) {
                ++i;
            }
            if (i == begin) {
                break;
            }
            const std::string_view id = s.substr(begin, i - begin);
            const uint32_t bone = rigNodeById(ctx, id);
            bool inSkeleton = false;
            for (uint32_t p = bone; p != 0xFFFFFFFFu && p != 0 && !inSkeleton;
                 p = ctx.scene.nodes[p].parent) {
                inSkeleton = ctx.scene.nodes[p].parent == skeleton;
            }
            if (bone == 0xFFFFFFFFu || ctx.scene.nodes[bone].kind != static_cast<uint8_t>(NodeKind::Bone) ||
                !inSkeleton) {
                ctx.sink.error(
                    ctx.file,
                    "<xt-weights> bone \"" + std::string(id) + "\" is not a bone of \"" + *skinRef + "\"",
                    weights.line, 0);
                return;
            }
            if (std::find(bones.begin(), bones.end(), bone) != bones.end()) {
                ctx.sink.error(ctx.file, "<xt-weights> lists bone \"" + std::string(id) + "\" twice",
                               weights.line, 0);
                return;
            }
            bones.push_back(bone);
        }
    }
    if (bones.empty() || bones.size() > 256) {
        ctx.sink.error(ctx.file, "<xt-weights> needs 1 to 256 bones", weights.line, 0);
        return;
    }

    IrSkinData skin{};
    skin.skeleton = skeleton;
    skin.tendonOffset = static_cast<uint32_t>(ctx.scene.tendonPool.size());
    skin.tendonCount = static_cast<uint32_t>(bones.size());
    const Mat2x3 own = nodeLocal(ctx.scene.nodes[ps.owner]);
    std::vector<IrTendon> tendons;
    for (uint32_t bone : bones) {
        // T-249: the rest chain (`rest-*`, absent = pose) up to the skeleton group.
        Mat2x3 rest = ctx.boneRests[ctx.scene.nodes[bone].kindIndex].local;
        for (uint32_t p = ctx.scene.nodes[bone].parent; p != 0xFFFFFFFFu && p != 0 && p != skeleton;
             p = ctx.scene.nodes[p].parent) {
            rest = mul(ctx.boneRests[ctx.scene.nodes[p].kindIndex].local, rest);
        }
        Mat2x3 restInv;
        if (!invert(rest, restInv)) {
            ctx.sink.error(ctx.file, "a bone's rest transform is singular, cannot bind", weights.line, 0);
            return;
        }
        const Mat2x3 inv = mul(restInv, own);
        IrTendon t{};
        t.bone = bone;
        t.inverseBind[0] = inv.a;
        t.inverseBind[1] = inv.b;
        t.inverseBind[2] = inv.c;
        t.inverseBind[3] = inv.d;
        t.inverseBind[4] = inv.e;
        t.inverseBind[5] = inv.f;
        tendons.push_back(t);
    }

    // Per-vertex columns: p, then (paths) in and out; unlisted vertices go wholly to bones[0].
    const uint32_t columns = mesh ? 1u : 3u;
    IrPointWeight whole{};
    whole.w4 = 255;
    std::vector<IrPointWeight> out(static_cast<size_t>(ps.vertexCount) * columns, whole);
    std::vector<uint8_t> seen(ps.vertexCount, 0);
    std::string_view text = weights.text;
    int line = weights.line;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view raw =
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        const int rawLine = line;
        pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++line;
        std::string_view row = trimParseSvg(raw);
        if (row.empty()) {
            continue;
        }
        auto drop = [&](const std::string& why) {
            ctx.sink.warn(ctx.file, "skin.weights: " + why + ", line dropped", rawLine, 0);
        };
        // T-265a (review H12): the row reading is parseWeightRow(), shared with the editor.
        WeightRowText parsed;
        const WeightRowError error = parseWeightRow(row, columns, bones.size(), parsed);
        if (error == WeightRowError::BadVertex) {
            size_t sp = 0;
            while (sp < row.size() && !isXmlSpace(row[sp])) {
                ++sp;
            }
            drop("malformed vertex index \"" + std::string(row.substr(0, sp)) + "\"");
            continue;
        }
        const uint32_t vertex = parsed.vertex;
        if (vertex >= ps.vertexCount) {
            drop("vertex " + std::to_string(vertex) + " is out of range (" + std::to_string(ps.vertexCount) +
                 " vertices)");
            continue;
        }
        if (error == WeightRowError::TooManyColumns) {
            drop(mesh ? "a mesh vertex has only the p column" : "more than three columns");
            continue;
        }
        if (error != WeightRowError::None) {
            drop(error == WeightRowError::BoneRange
                     ? "a bone index is out of range (" + std::to_string(bones.size()) + " bones)"
                     : "malformed weights (up to 4 <bone>:<weight> items with a positive sum per column)");
            continue;
        }
        if (seen[vertex] != 0) {
            ctx.sink.warn(ctx.file,
                          "skin.weights: vertex " + std::to_string(vertex) + " listed twice, the last wins",
                          rawLine, 0);
        }
        seen[vertex] = 1;
        for (uint32_t c = 0; c < columns; ++c) {
            const auto& items =
                parsed.forms[c] == WeightColumnForm::Items ? parsed.items[c] : parsed.items[0];
            out[static_cast<size_t>(vertex) * columns + c] = packWeights(items);
        }
    }

    // T-243c: a path's rows are numbered by on-curve vertex; the IR keeps one p / in / out triple
    // per non-Close verb, so expand (a closing segment's end takes vertex 0's whole triple).
    if (!mesh) {
        std::vector<IrPointWeight> slots(ps.slotVertex.size() * 3, whole);
        for (size_t s = 0; s < ps.slotVertex.size(); ++s) {
            const size_t v = ps.slotVertex[s];
            for (size_t c = 0; c < 3; ++c) {
                slots[s * 3 + c] = out[v * 3 + c];
            }
        }
        out = std::move(slots);
    }

    skin.weightOffset = static_cast<uint32_t>(ctx.scene.weightPool.size());
    skin.weightCount = static_cast<uint32_t>(out.size());
    ctx.scene.tendonPool.insert(ctx.scene.tendonPool.end(), tendons.begin(), tendons.end());
    ctx.scene.weightPool.insert(ctx.scene.weightPool.end(), out.begin(), out.end());
    const uint32_t skinIndex = static_cast<uint32_t>(ctx.scene.skins.size());
    ctx.scene.skins.push_back(skin);
    const IrSceneNode& owner = ctx.scene.nodes[ps.owner];
    if (mesh) {
        ctx.scene.meshes[owner.kindIndex].skinIndex = skinIndex;
    } else {
        ctx.scene.paths[owner.kindIndex].skinIndex = skinIndex;
    }
}

/// T-243: what needs every node: constraint targets, then skins, then the kinds of the bound scene
/// nodes (SvgBindAttr::kind).
void finishRig(BuildCtxParseSvg& ctx) {
    for (const BuildCtxParseSvg::PendingTarget& pt : ctx.pendingTargets) {
        const uint32_t target = rigNodeById(ctx, pt.target);
        if (target == 0xFFFFFFFFu) {
            ctx.sink.error(ctx.file,
                           "<xt-constraint> target \"" + pt.target + "\" is not a node of this <svg>",
                           pt.line, 0);
            continue;
        }
        ctx.scene.constraints[pt.constraint].target = target;
    }
    for (const BuildCtxParseSvg::PendingSkin& ps : ctx.pendingSkins) {
        buildSkin(ctx, ps);
    }
    if (ctx.extras != nullptr) {
        for (SvgBindAttr& b : ctx.extras->binds) {
            if (b.sceneNode < ctx.scene.nodes.size()) {
                const IrSceneNode& n = ctx.scene.nodes[b.sceneNode];
                b.kind = n.kind;
                if (n.kind == static_cast<uint8_t>(NodeKind::Constraint) &&
                    n.kindIndex < ctx.scene.constraints.size()) {
                    b.constraintKind = ctx.scene.constraints[n.kindIndex].kind;
                }
            }
        }
    }
}

void buildElement(BuildCtxParseSvg& ctx, uint32_t nodeIdx, uint32_t parent, SvgStyle style, int depth) {
    if (depth > kMaxDepth) {
        ctx.sink.warn(ctx.file, "SVG nesting too deep, subtree skipped", ctx.nodes[nodeIdx].line, 0);
        return;
    }
    const XmlNode& node = ctx.nodes[nodeIdx];
    const std::string& tag = node.tag;

    if (isUnsupportedElement(tag)) {
        ctx.sink.warn(ctx.file, "<" + tag + "> is not supported, skipped", node.line, 0);
        return;
    }
    if (isIgnoredElement(tag)) {
        return;  // Definition-only or decorative: never drawn.
    }
    if (tag == "defs") {
        return;  // Its contents are only reachable through `<use>`/`url(#id)`.
    }

    // T-225b (31 §31.6.1): a modifier element: no paint, transform or opacity of its own.
    ModifierKind modKind{};
    if (modifierKindOf(tag, modKind)) {
        const IrModifierData data = modifierDataOf(ctx, node, modKind);
        if (modKind == ModifierKind::Trim && demoteSinglePathTrim(ctx, node, data, parent)) {
            return;  // T-236: written into the one path's own trim.
        }
        const uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
        emitModifier(ctx, data, parent, kInvalidString);
        ctx.scene.nodes[index].name = recordId(ctx, node, index);
        return;
    }

    // T-243 (32 §32.2): bones and constraints have no paint, transform attribute or opacity.
    if (tag == "xt-bone") {
        buildBone(ctx, nodeIdx, parent, style, depth);
        return;
    }
    if (tag == "xt-constraint") {
        buildConstraint(ctx, node, parent);
        return;
    }
    if (tag == "xt-pins" || tag == "xt-tris" || tag == "xt-weights") {
        ctx.sink.warn(ctx.file, "<" + tag + "> outside <xt-mesh> / <path> is not supported, skipped",
                      node.line, 0);
        return;
    }

    applyStyle(ctx, node, style);
    float opacity = opacityOf(node);
    if (ctx.clipContent) {
        // T-225b (31 §31.5.3): clip-path content contributes its geometry only.
        style.fill = SvgPaintSpec{};
        style.stroke.kind = static_cast<uint8_t>(PaintKind::None);
        style.fillOpacity = 1;
        style.fillRule = style.clipRule;
        style.dash.clear();
        opacity = 1;
    }

    Mat2x3 local;
    std::string transformValue;
    if (propertyOf(node, "transform", transformValue) && !parseTransform(transformValue, local)) {
        ctx.sink.warn(ctx.file, "malformed transform \"" + transformValue + "\", partially applied",
                      node.line, 0);
    }

    if (tag == "xt-mesh") {
        buildMesh(ctx, nodeIdx, parent, local);  // T-243 (32 §32.2.4)
        return;
    }

    // T-243b (32 §32.2.1): the run time's bone world is relative to the skeleton group without
    // the group's own transform, so the group takes none.
    if (tag == "xt-skeleton" && (attrOf(node, "transform") != nullptr || !transformValue.empty() ||
                                 attrOf(node, "translate") != nullptr || attrOf(node, "rotate") != nullptr ||
                                 attrOf(node, "scale") != nullptr)) {
        ctx.sink.warn(ctx.file,
                      "skin.skeletonTransform: <xt-skeleton> takes no transform (its bones are in the "
                      "<svg> group's space), ignored",
                      node.line, 0);
        local = Mat2x3{};
    }
    if (tag == "g" || tag == "a" || tag == "svg" || tag == "xt-skeleton") {
        // A nested <svg> is treated as a plain group: its own viewBox is out of the subset.
        if (tag == "svg" && depth > 0) {
            ctx.sink.warn(ctx.file, "nested <svg> viewBox is not supported, treated as a group", node.line,
                          0);
        }
        parent = wrapClipAndMask(ctx, node, parent, local, depth);
        uint32_t group = emitGroup(ctx, parent, local, opacity, kInvalidString);
        ctx.scene.nodes[group].name = recordId(ctx, node, group);
        // T-218: `g` only; T-243: and `xt-skeleton`.
        ctx.scene.nodes[group].svgTag = static_cast<uint8_t>(irSvgTagCode(tag));
        uint8_t blend = 0;
        if (blendOf(ctx, node, blend)) {
            ctx.scene.nodes[group].blend = blend;
        }
        buildChildren(ctx, node, group, style, depth);
        return;
    }

    if (tag == "use") {
        const std::string* href = attrOf(node, "href");
        if (href == nullptr || href->empty() || (*href)[0] != '#') {
            ctx.sink.warn(ctx.file, "<use> without a local href, skipped", node.line, 0);
            return;
        }
        if (ctx.inUse) {
            ctx.sink.warn(ctx.file, "nested <use> is expanded only once, skipped", node.line, 0);
            return;
        }
        auto target = ctx.byId.find(href->substr(1));
        if (target == ctx.byId.end()) {
            ctx.sink.warn(ctx.file, "<use> target \"" + *href + "\" not found, skipped", node.line, 0);
            return;
        }
        float x = 0, y = 0;
        bool percent = false;
        const std::string* xAttr = attrOf(node, "x");
        const std::string* yAttr = attrOf(node, "y");
        if (xAttr != nullptr) {
            parseLengthOrPercent(*xAttr, x, percent);
        }
        if (yAttr != nullptr) {
            parseLengthOrPercent(*yAttr, y, percent);
        }
        Mat2x3 useXf = mul(local, translation(Vec2{x, y}));
        parent = wrapClipAndMask(ctx, node, parent, useXf, depth);
        uint32_t group = emitGroup(ctx, parent, useXf, opacity, kInvalidString);
        ctx.inUse = true;
        buildElement(ctx, target->second, group, style, depth + 1);
        ctx.inUse = false;
        return;
    }

    SvgPath path;
    ShapeKind shapeKind{};
    if (shapeKindOf(tag, shapeKind)) {
        // T-225b (31 §31.6.1): `<rect>`, `<circle>`/`<ellipse>` and `<xt-star>` become Shape nodes
        // (the renderer rebuilds the same geometry from the parameters, so they can be animated).
        // The geometry is still built here for the gradient bounding box; a star without `points`
        // / `r` warns and is skipped, a degenerate rect/ellipse is kept (its parameters may animate).
        const bool built = buildShape(ctx, node, path);
        if (!built && shapeKind == ShapeKind::Star) {
            return;
        }
        float params[12];
        shapeParamsOf(node, shapeKind, params);
        parent = wrapClipAndMask(ctx, node, parent, local, depth);
        uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
        StringId name = recordId(ctx, node, index);
        emitShape(ctx, shapeKind, params, built ? boundsOf(path) : Bounds{}, style, parent, local, opacity,
                  name, node.line);
        ctx.scene.nodes[index].svgTag = static_cast<uint8_t>(irSvgTagCode(tag));
        applyPathTrimAttr(ctx, node, ctx.scene.shapes.back());
        applyStrokeTaperAttrs(ctx, node, /*closed=*/true, ctx.scene.shapes.back());  // T-233
        if (attrOf(node, "xt-skin") != nullptr) {
            ctx.sink.warn(ctx.file,
                          "skin.shape: <" + tag + "> is a parametric shape and is not skinned; use a <path>",
                          node.line, 0);
        }
        return;
    }
    if (buildShape(ctx, node, path)) {
        parent = wrapClipAndMask(ctx, node, parent, local, depth);
        uint32_t index = static_cast<uint32_t>(ctx.scene.nodes.size());
        StringId name = recordId(ctx, node, index);
        emitPath(ctx, path, style, parent, local, opacity, name, node.line);
        if (index < ctx.scene.nodes.size()) {
            ctx.scene.nodes[index].svgTag = static_cast<uint8_t>(irSvgTagCode(tag));  // T-218.
        }
        applyPathTrimAttr(ctx, node, ctx.scene.paths.back());
        const bool closed = std::find(path.verbs.begin(), path.verbs.end(),
                                      static_cast<uint8_t>(PathVerb::Close)) != path.verbs.end();
        applyStrokeTaperAttrs(ctx, node, closed, ctx.scene.paths.back());  // T-233
        // T-243 (32 §32.2.2): a skinned path (built once every node exists, finishRig()).
        if (!ctx.inUse && !ctx.inDef &&
            (attrOf(node, "xt-skin") != nullptr || childByTag(ctx, node, "xt-weights") != 0xFFFFFFFFu)) {
            BuildCtxParseSvg::PendingSkin ps{index, nodeIdx, 0, {}};
            ps.vertexCount = weightVertexOrder(path, ps.slotVertex);
            ctx.pendingSkins.push_back(std::move(ps));
        }
        return;
    }
    if (tag == "path" || tag == "rect" || tag == "circle" || tag == "ellipse" || tag == "line" ||
        tag == "polyline" || tag == "polygon" || tag == "xt-star") {
        return;  // A degenerate/empty shape: nothing to draw, and not worth a diagnostic.
    }
    ctx.sink.warn(ctx.file, "<" + tag + "> is not supported, skipped", node.line, 0);
}

}  // namespace

WeightRowError parseWeightRow(std::string_view row, uint32_t columns, size_t boneCount, WeightRowText& out) {
    out = WeightRowText{};
    row = trimParseSvg(row);
    size_t sp = 0;
    while (sp < row.size() && !isXmlSpace(row[sp])) {
        ++sp;
    }
    const std::from_chars_result r = std::from_chars(row.data(), row.data() + sp, out.vertex);
    if (sp == 0 || r.ec != std::errc{} || r.ptr != row.data() + sp) {
        out.vertex = 0;
        return WeightRowError::BadVertex;
    }
    std::vector<std::string_view> cols;
    std::string_view rest = row.substr(sp);
    for (size_t bar = rest.find('|'); bar != std::string_view::npos; bar = rest.find('|')) {
        cols.push_back(trimParseSvg(rest.substr(0, bar)));
        rest = rest.substr(bar + 1);
    }
    cols.push_back(trimParseSvg(rest));
    if (cols.size() > columns) {
        return WeightRowError::TooManyColumns;
    }
    for (uint32_t c = 0; c < columns && c < 3; ++c) {
        if (c > 0 && (c >= cols.size() || cols[c].empty())) {
            out.forms[c] = WeightColumnForm::Omitted;
            continue;
        }
        if (c > 0 && cols[c] == "=") {
            out.forms[c] = WeightColumnForm::Same;
            continue;
        }
        out.forms[c] = WeightColumnForm::Items;
        bool boneRange = false;
        bool ok = parseWeightColumn(cols[c], boneCount, out.items[c], boneRange);
        float sum = 0;
        for (const auto& it : out.items[c]) {
            sum += it.second;
        }
        ok = ok && sum > 0.0f;
        if (!ok) {
            return boneRange ? WeightRowError::BoneRange : WeightRowError::BadWeights;
        }
    }
    return WeightRowError::None;
}

Status parseSvg(std::string_view text, std::string_view fileName, DiagnosticSink& sink, IrWriter& writer,
                SceneBuilder& out) {
    return parseSvg(text, fileName, sink, writer, out, nullptr);
}

Status parseSvg(std::string_view text, std::string_view fileName, DiagnosticSink& sink, IrWriter& writer,
                SceneBuilder& out, std::vector<std::pair<std::string, uint32_t>>* ids) {
    return parseSvg(text, fileName, sink, writer, out, ids, nullptr);
}

Status parseSvg(std::string_view text, std::string_view fileName, DiagnosticSink& sink, IrWriter& writer,
                SceneBuilder& out, std::vector<std::pair<std::string, uint32_t>>* ids,
                SvgParseExtras* extras) {
    std::string file(fileName);
    std::vector<XmlNode> nodes;
    uint32_t root = parseXml(text, nodes);
    if (root == 0xFFFFFFFFu) {
        sink.error(file, "malformed SVG/XML document");
        return Status::kInvalidArgument;
    }
    if (nodes[root].tag != "svg") {
        sink.error(file, "root element is <" + nodes[root].tag + ">, expected <svg>");
        return Status::kInvalidArgument;
    }

    const XmlNode& svg = nodes[root];
    float viewX = 0, viewY = 0, width = 0, height = 0;
    bool hasViewBox = false;
    const std::string* viewBox = attrOf(svg, "viewBox");
    if (viewBox != nullptr) {
        std::vector<float> v = parseNumberList(*viewBox);
        if (v.size() == 4 && v[2] > 0.0f && v[3] > 0.0f) {
            viewX = v[0];
            viewY = v[1];
            width = v[2];
            height = v[3];
            hasViewBox = true;
        } else {
            sink.warn(file, "malformed viewBox \"" + *viewBox + "\", ignored", svg.line, 0);
        }
    }
    if (!hasViewBox) {
        // 12.2.1: no viewBox -> the width/height attributes; neither -> the CSS default replaced
        // element size, with a warning.
        float w = 0, h = 0;
        bool percent = false;
        const std::string* wAttr = attrOf(svg, "width");
        const std::string* hAttr = attrOf(svg, "height");
        bool haveW = wAttr != nullptr && parseLengthOrPercent(*wAttr, w, percent) && !percent && w > 0.0f;
        bool haveH = hAttr != nullptr && parseLengthOrPercent(*hAttr, h, percent) && !percent && h > 0.0f;
        if (haveW && haveH) {
            width = w;
            height = h;
        } else {
            sink.warn(file, "no viewBox and no width/height, defaulting to 300x150", svg.line, 0);
            width = 300;
            height = 150;
        }
    }

    out.name = writer.intern(file);
    out.width = width;
    out.height = height;

    IrSceneNode rootNode{};
    rootNode.kind = static_cast<uint8_t>(NodeKind::Group);
    rootNode.visible = 1;
    rootNode.parent = kInvalidNode;
    rootNode.opacity = 1;
    rootNode.scale = Vec2{1, 1};
    rootNode.name = kInvalidString;
    // The viewBox origin becomes the scene's origin; the design size is the viewBox size, so no
    // scale is needed here (object-fit does the fitting, 06.10).
    rootNode.translate = Vec2{-viewX, -viewY};
    out.nodes.push_back(rootNode);

    BuildCtxParseSvg ctx{out, writer, sink,  file,   nodes, {}, {}, false,
                         ids, false,  false, extras, {},    {}, {}, {}};
    collectDefsParseSvg(ctx, root, 0);
    resolveGradientHrefs(ctx);

    SvgStyle style;
    applyStyle(ctx, svg, style);
    buildChildren(ctx, svg, 0, style, 0);
    finishRig(ctx);  // T-243
    return Status::kOk;
}

}  // namespace xt::compile
