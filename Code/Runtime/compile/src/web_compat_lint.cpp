#include "web_compat_lint.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <json.hpp>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/migrate.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"
#include "xt/property.h"

/// T-594: see web_compat_lint.h. Each message is one sentence stating XTFrame's rule, ending with
/// the not-like-web.md item it comes from; `fix` says what to write instead.
///
/// Not covered by the task sheet (choices made here, listed in the report):
///   - not-like-web.md items that are no web habit, or that existing diagnostics already report,
///     get no `web.*` code: ports and their direction (`bind.outPortSource`), port names `index` /
///     `count` / `item` / `time` and ancestor classes in symbol styles (compiler warnings), `@media`
///     limits (dropped with a warning), text mixed with block children (the parser's error).
///   - `web.textWithChildren`, `web.percentHeightOnAuto` and `web.vwVhVsUiscale` point at their own
///     not-like-web.md items under "Layout and painting" (added by T-611).
///   - The computed-style checks read the cascade without any `@media` condition or state
///     (dumpComputedStyle()'s base combination); inherited values are followed up the tree here.

namespace xt::compile {

namespace {

using Json = nlohmann::json;
using Props = std::map<std::string, std::string>;

std::string lowerTrim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    std::string out(text.substr(begin, end - begin));
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// The pointer at the end of every message.
std::string item(std::string_view lead) { return " (not-like-web.md: \"" + std::string(lead) + "\")"; }

constexpr std::string_view kDisplayItem = "display is block | flex | grid | none";
constexpr std::string_view kTextItem = "An element holds either text or child elements";
constexpr std::string_view kPercentHeightItem =
    "`height: N%` on a child of an `auto`-height parent is nothing";
constexpr std::string_view kViewportItem = "`vw`/`vh` do not follow `uiscale`";

/// True when `value` holds a number directly followed by `vw`, `vh`, `vmin` or `vmax`.
bool hasViewportUnit(const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != 'v' || i == 0 ||
            (std::isdigit(static_cast<unsigned char>(value[i - 1])) == 0 && value[i - 1] != '.')) {
            continue;
        }
        for (const std::string_view unit : {"vmin", "vmax", "vw", "vh"}) {
            if (value.compare(i, unit.size(), unit) == 0) {
                const size_t after = i + unit.size();
                if (after >= value.size() || std::isalnum(static_cast<unsigned char>(value[after])) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool isTableTag(std::string_view tag) {
    return tag == "table" || tag == "thead" || tag == "tbody" || tag == "tfoot" || tag == "tr" ||
           tag == "td" || tag == "th";
}

/// The tag as written: parse_xt keeps an unknown one (`<table>`, read as a div) in `data-xt-tag`.
std::string writtenTag(const DomNode& node) {
    const DomAttr* original = node.attr("data-xt-tag");
    return original != nullptr ? lowerTrim(original->value) : node.tag;
}

bool hasDroppedAttr(const DomNode& node) {
    return std::any_of(node.attrs.begin(), node.attrs.end(), [](const DomAttr& a) {
        return a.name == "id" || a.name.rfind("bind:", 0) == 0 || a.name.rfind("on:", 0) == 0;
    });
}

void richTextChildren(const DomTree& tree, uint32_t nodeIndex, const DomNode& owner, const std::string& file,
                      DiagnosticSink& sink) {
    for (const uint32_t child : tree.nodes[nodeIndex].children) {
        const DomNode& node = tree.nodes[child];
        if (hasDroppedAttr(node)) {
            const DomAttr* id = node.attr("id");
            const std::string name =
                "<" + node.tag + (id != nullptr ? " id=\"" + id->value + "\"" : "") + ">";
            sink.web(file,
                     name + " sits in the text of <" + owner.tag +
                         ">, so it becomes a run of that text, not an element: its id, bind: and on: do "
                         "nothing (an XTFrame element holds either text or child elements)" +
                         item(kTextItem),
                     node.line, node.col, "web.textWithChildren",
                     "put the text beside " + name + " in its own <span>, so " + name + " stays an element");
        }
        richTextChildren(tree, child, owner, file, sink);
    }
}

// ---- the computed-style checks -----------------------------------------------------------------

/// One DOCT element with its base cascade.
struct Element {
    uint32_t parent = 0xFFFFFFFFu;
    std::string tag;
    std::string id;
    bool text = false;         ///< Its own non-blank text.
    bool interactive = false;  ///< Pointer triggers, a control tag or a drag attribute.
    int line = 0;
    int col = 0;
    Props props;
    std::vector<uint32_t> children;
};

class DocumentLint {
public:
    DocumentLint(std::vector<Element> elements, const std::string& file, DiagnosticSink& sink)
        : m_el(std::move(elements)), m_file(file), m_sink(sink) {}

    void run() {
        for (uint32_t e = 0; e < m_el.size(); ++e) {
            zIndex(e);
            pointerEvents(e);
            flexText(e);
            percentHeight(e);
            sortGroups(e);
        }
    }

private:
    std::string prop(uint32_t e, const char* name) const {
        const auto found = m_el[e].props.find(name);
        return found == m_el[e].props.end() ? std::string() : found->second;
    }

    bool has(uint32_t e, const char* name) const { return m_el[e].props.count(name) != 0; }

    std::string name(uint32_t e) const {
        return m_el[e].id.empty() ? "<" + m_el[e].tag + ">" : "#" + m_el[e].id;
    }

    /// The selector a fix writes for `e` (`#id`, or the tag when it has no id).
    std::string selector(uint32_t e) const { return m_el[e].id.empty() ? m_el[e].tag : "#" + m_el[e].id; }

    bool valid(uint32_t e) const { return e < m_el.size(); }

    void report(uint32_t e, const std::string& message, const char* code, const std::string& fix) {
        m_sink.web(m_file, message, m_el[e].line, m_el[e].col, code, fix);
    }

    void zIndex(uint32_t e) {
        const std::string z = prop(e, "z-index");
        if (z.empty() || std::strtof(z.c_str(), nullptr) == 0.0f || prop(e, "layer") == "overlay" ||
            !valid(m_el[e].parent)) {
            return;
        }
        std::string why;
        if (m_el[m_el[e].parent].children.size() == 1) {
            why = "it has no sibling to reorder against";
        }
        for (uint32_t a = m_el[e].parent; why.empty() && valid(a); a = m_el[a].parent) {
            for (const char* clip : {"overflow-x", "overflow-y", "overflow"}) {
                const std::string v = prop(a, clip);
                if (v == "hidden" || v == "clip" || v == "scroll" || v == "auto") {
                    why = "it stays clipped by " + name(a) + " (overflow: " + v + ")";
                    break;
                }
            }
        }
        if (!why.empty()) {
            report(e,
                   name(e) + " has z-index " + z + ", but XTFrame's z-index only reorders siblings, so " +
                       why + "; layer: overlay draws a subtree above everything" +
                       item("No global stacking."),
                   "web.zIndexAcrossLayers", selector(e) + " { layer: overlay; }");
        }
    }

    void pointerEvents(uint32_t e) {
        if (!m_el[e].interactive || has(e, "pointer-events")) {
            return;
        }
        for (uint32_t a = m_el[e].parent; valid(a); a = m_el[a].parent) {
            const std::string v = prop(a, "pointer-events");
            if (v.empty()) {
                continue;
            }
            if (v == "none") {
                report(e,
                       name(e) + " takes no clicks: pointer-events is inherited in XTFrame and " + name(a) +
                           " sets none, so " + name(e) + " needs its own pointer-events: auto" +
                           item("`pointer-events` is inherited."),
                       "web.pointerEventsInherit", selector(e) + " { pointer-events: auto; }");
            }
            return;
        }
    }

    void flexText(uint32_t e) {
        const uint32_t p = m_el[e].parent;
        if (!m_el[e].text || !valid(p) || prop(p, "display") != "flex" ||
            prop(p, "flex-direction").rfind("column", 0) == 0) {
            return;
        }
        const std::string shrink = prop(e, "flex-shrink");
        const std::string whiteSpace = prop(e, "white-space");
        if (has(e, "min-width") || (!shrink.empty() && std::strtof(shrink.c_str(), nullptr) == 0.0f) ||
            whiteSpace == "nowrap" || whiteSpace == "pre") {
            return;
        }
        report(e,
               name(e) +
                   " is a flex item with text and no min-width: XTFrame's flex has no min-content "
                   "minimum, so it can shrink narrower than its longest word" +
                   item("Flex items have no min-content minimum"),
               "web.flexTextNoMinWidth", selector(e) + " { min-width: <width>; } (or flex-shrink: 0)");
    }

    void percentHeight(uint32_t e) {
        const std::string h = prop(e, "height");
        const uint32_t p = m_el[e].parent;
        if (h.empty() || h.back() != '%' || !valid(p) || m_el[p].tag == "body" || m_el[p].tag == "xt") {
            return;
        }
        const std::string ph = prop(p, "height");
        const bool stretched = prop(p, "position") == "absolute" && has(p, "top") && has(p, "bottom");
        if ((!ph.empty() && ph.find("auto") == std::string::npos) || stretched) {  // Auto reads "0auto".
            return;
        }
        report(e,
               name(e) + " has height " + h + " of " + name(p) +
                   ", whose height is auto (it comes from its content): give the parent a height, or size " +
                   name(e) + " with flex" + item(kPercentHeightItem),
               "web.percentHeightOnAuto", selector(p) + " { height: <height>; }");
    }

    /// A 3D layer: a non-zero translateZ, rotateX / rotateY, `translate3d` z or orientation. The dump
    /// names these transform components `prop<PropId>` (they have no CSS name of their own).
    bool is3d(uint32_t e) const {
        const auto nonZero = [](const std::string& v) {
            for (const char* p = v.c_str(); *p != 0;) {
                char* end = nullptr;
                const float f = std::strtof(p, &end);
                if (end == p) {
                    ++p;
                    continue;
                }
                if (f != 0.0f) {
                    return true;
                }
                p = end;
            }
            return false;
        };
        for (const PropId id : {prop::TranslateZ, prop::RotationX, prop::RotationY, prop::Orientation}) {
            if (nonZero(prop(e, ("prop" + std::to_string(id)).c_str()))) {
                return true;
            }
        }
        const std::string t3 = prop(e, ("prop" + std::to_string(prop::Translate3)).c_str());
        const size_t z = t3.find_last_of(' ');
        return z != std::string::npos && nonZero(t3.substr(z + 1));
    }

    /// A 2D child between two 3D siblings (reported once, on the 2D one).
    void sortGroups(uint32_t e) {
        const std::vector<uint32_t>& kids = m_el[e].children;
        for (size_t i = 1; i + 1 < kids.size(); ++i) {
            if (is3d(kids[i])) {
                continue;
            }
            const auto before = std::find_if(kids.rbegin() + static_cast<std::ptrdiff_t>(kids.size() - i),
                                             kids.rend(), [this](uint32_t k) { return is3d(k); });
            const auto after = std::find_if(kids.begin() + static_cast<std::ptrdiff_t>(i + 1), kids.end(),
                                            [this](uint32_t k) { return is3d(k); });
            if (before == kids.rend() || after == kids.end()) {
                continue;
            }
            report(kids[i],
                   name(kids[i]) + " is a 2D layer between the 3D siblings " + name(*before) + " and " +
                       name(*after) +
                       ": XTFrame depth-sorts whole layers, and a 2D sibling splits them into separate "
                       "sort groups" +
                       item("`translateZ` sorts whole layers, not pixels"),
                   "web.translateZSort",
                   "move " + name(kids[i]) + " before or after the 3D siblings, or give it a translateZ");
        }
    }

    std::vector<Element> m_el;
    const std::string& m_file;
    DiagnosticSink& m_sink;
};

bool isPointerTrigger(uint8_t event) {
    switch (static_cast<TriggerEvent>(event)) {
        case TriggerEvent::Hover:
        case TriggerEvent::Leave:
        case TriggerEvent::Press:
        case TriggerEvent::Release:
        case TriggerEvent::Click:
        case TriggerEvent::DoubleClick:
        case TriggerEvent::PointerMove:
        case TriggerEvent::PointerDrag:
        case TriggerEvent::DragStart:
        case TriggerEvent::DragOver:
        case TriggerEvent::Drop:
            return true;
        default:
            return false;
    }
}

}  // namespace

void lintWebDeclaration(std::string_view prop, std::string_view value, const std::string& file, int line,
                        int col, DiagnosticSink& sink) {
    if (!sink.webCompat()) {
        return;
    }
    const std::string p = lowerTrim(prop);
    const std::string v = lowerTrim(value);
    const std::string decl = p + ": " + v;
    if (p == "display" && (v.rfind("table", 0) == 0 || v == "inline-table")) {
        sink.web(file,
                 decl +
                     " does not exist: XTFrame has no tables, so lay rows out with flex or grid and share "
                     "column widths through document tokens" +
                     item("Shared column widths: document tokens."),
                 line, col, "web.floatOrTable",
                 "display: grid (or flex rows), with widths as :root tokens (width: var(--col-x))");
    } else if (p == "display" &&
               (v == "inline" || v == "inline-block" || v == "inline-flex" || v == "inline-grid" ||
                v == "contents" || v == "flow-root" || v == "list-item")) {
        sink.web(file,
                 decl +
                     " does not exist: XTFrame's display is block, flex, grid or none, and a row of boxes "
                     "is a flex parent" +
                     item(kDisplayItem),
                 line, col, "web.inlineBlock",
                 v == "inline-flex" || v == "inline-grid"
                     ? "display: " + v.substr(7)
                     : "display: flex on the parent (a row), display: block here");
    } else if ((p == "float" || p == "clear") && v != "none") {
        sink.web(file,
                 decl +
                     " does not exist: XTFrame has no floats, so place side-by-side boxes with flex (or "
                     "position: absolute)" +
                     item(kDisplayItem),
                 line, col, "web.floatOrTable", "display: flex on the parent instead of " + p);
    } else if (p == "position" && (v == "fixed" || v == "sticky")) {
        sink.web(file,
                 decl +
                     " does not exist: XTFrame's position is static, relative or absolute, so put the "
                     "element at document level with position: absolute" +
                     item("`position` is `static | relative | absolute`."),
                 line, col, "web.positionFixed",
                 "position: absolute on a direct child of <body> (layer: overlay to draw above everything)");
    } else if (p == "box-sizing" && v != "border-box") {
        sink.web(file,
                 decl +
                     " does nothing: XTFrame is always border-box, so width and height include padding "
                     "and border" +
                     item("Always border-box."),
                 line, col, "web.boxSizing",
                 "remove box-sizing and add the padding and border to width/height");
    }
    if (hasViewportUnit(v) && v.find("uiscale") == std::string::npos) {
        sink.web(file,
                 decl +
                     " follows the viewport, not the UI scale: XTFrame multiplies px by uiscale but not "
                     "vw / vh" +
                     item(kViewportItem),
                 line, col, "web.vwVhVsUiscale", "write the size in px, unless it must follow the viewport");
    }
}

void lintWebRichText(const DomTree& tree, uint32_t nodeIndex, const std::string& file, DiagnosticSink& sink) {
    if (sink.webCompat() && nodeIndex < tree.nodes.size()) {
        richTextChildren(tree, nodeIndex, tree.nodes[nodeIndex], file, sink);
    }
}

void lintWebDom(const DomTree& dom, const std::string& file, DiagnosticSink& sink) {
    if (!sink.webCompat()) {
        return;
    }
    for (const DomNode& node : dom.nodes) {
        const std::string tag = writtenTag(node);
        const bool topOfTable = isTableTag(tag) && (node.parent >= dom.nodes.size() ||
                                                    !isTableTag(writtenTag(dom.nodes[node.parent])));
        if (topOfTable) {
            sink.web(file,
                     "<" + tag +
                         "> is no table: XTFrame has no table layout, so lay rows out with flex or "
                         "grid and share column widths through document tokens" +
                         item("Shared column widths: document tokens."),
                     node.line, node.col, "web.floatOrTable",
                     "<div> rows with display: flex, widths as :root tokens (width: var(--col-x))");
        }
        const DomAttr* layer = node.attr("layer");
        if (layer != nullptr && node.tag != "use") {
            const DomAttr* id = node.attr("id");
            const std::string selector = id != nullptr ? "#" + id->value : node.tag;
            sink.web(file,
                     "<" + node.tag + " layer=\"" + layer->value +
                         "\"> does nothing: layer is a style property in XTFrame, never an attribute" +
                         item("No global stacking."),
                     node.line, node.col, "web.layerAttribute",
                     selector + " { layer: " + layer->value + "; }");
        }
    }
}

void lintWebDocument(std::span<const uint8_t> ir, const std::string& file, DiagnosticSink& sink) {
    if (!sink.webCompat()) {
        return;
    }
    std::string text;
    std::string error;
    Result<IrView> opened = IrView::open(IrBlob{ir.data(), ir.size()});
    if (!opened.ok() || !dumpComputedStyle(ir, text, error)) {
        return;
    }
    const IrView& view = opened.value();
    const Json dump = Json::parse(text, nullptr, false);
    if (dump.is_discarded() || !dump.contains("elements") || !dump["elements"].is_array()) {
        return;
    }
    const DocChunk& doc = view.doc();
    const auto irElements = doc.elements();
    std::vector<Element> elements(irElements.size());
    for (uint32_t e = 0; e < irElements.size(); ++e) {
        const IrDocElement& src = irElements[e];
        Element& out = elements[e];
        out.parent = src.parent;
        out.tag = std::string(view.string(src.tag));
        out.id = src.id == kInvalidString ? std::string() : std::string(view.string(src.id));
        if (src.firstTextChild < doc.texts().size()) {
            const std::string_view t = view.string(doc.texts()[src.firstTextChild].text);
            out.text = std::any_of(t.begin(), t.end(),
                                   [](char c) { return !std::isspace(static_cast<unsigned char>(c)); });
        }
        out.interactive =
            out.tag == "button" || out.tag == "input" || out.tag == "textarea" || out.tag == "select";
        for (uint32_t a = 0; a < src.attrCount && src.attrOffset + a < doc.attrPool().size(); ++a) {
            const std::string_view attr = view.string(doc.attrPool()[src.attrOffset + a].name);
            out.interactive = out.interactive || attr == "draggable" || attr == "drop-target";
        }
        if (view.hasSourceMap() && e < view.sourceMap().entries().size()) {
            out.line = static_cast<int>(view.sourceMap().entries()[e].line);
            out.col = static_cast<int>(view.sourceMap().entries()[e].col);
        }
        if (src.parent < irElements.size()) {
            elements[src.parent].children.push_back(e);
        }
    }
    for (const IrTriggerTable& table : view.triggers().tables()) {
        for (uint32_t t = 0; t < table.triggerCount && table.element < elements.size(); ++t) {
            const size_t index = static_cast<size_t>(table.triggerOffset) + t;
            if (index < view.triggers().triggers().size() &&
                isPointerTrigger(view.triggers().triggers()[index].event)) {
                elements[table.element].interactive = true;
            }
        }
    }
    const Json& list = dump["elements"];
    for (size_t i = 0; i < list.size() && i < elements.size(); ++i) {
        const Json* base = nullptr;
        if (list[i].contains("states") && list[i]["states"].contains("") &&
            list[i]["states"][""].contains("base")) {
            base = &list[i]["states"][""]["base"];
        }
        if (base == nullptr || !base->is_object()) {
            continue;
        }
        for (const auto& [key, value] : base->items()) {
            if (value.is_string()) {
                elements[i].props.emplace(key, lowerTrim(value.get<std::string>()));
            }
        }
    }
    DocumentLint(std::move(elements), file, sink).run();
}

}  // namespace xt::compile
