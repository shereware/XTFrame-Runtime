#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <json.hpp>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "bundle_json.h"
#include "color_math.h"
#include "design_internal.h"
#include "xt/bundle/design_system.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"
#include "yaml_lite.h"

/// The reference graph, usage / impact queries and validation (docs/design/35-design-system.md
/// §35.5, §35.6, §35.10; docs/tasks/T-305.md).
///
/// Computed edges (variable -> UI / layer / symbol) come from a text scan of `var(--name` in:
///   - every UI document (its `<style>` blocks and element `style=` attributes) and the `.xts`
///     stylesheets it `<link>`s: an edge `ui:<id>` -> `token:<name>` always, plus
///     `layer:<ui>#<element id>` -> `token:<name>` when the use sits in a rule block whose selector
///     (innermost first) names `#<id>` of an element of that UI (the last `#id` of each
///     comma-separated selector), or in that element's `style=`;
///   - every symbol file of `shared.symbols` and every symbol file a UI links: `symbol:<name>` ->
///     `token:<name>` for each `<symbol name>` the file defines. A UI that links a symbol file also
///     gets `ui:<id>` -> `token:<name>` for the file's uses, because the symbol's styles are compiled
///     into that UI (this is how a UI built only from symbols, like the starter's, reaches a colour
///     variable).

namespace xt::bundle::design {

using nlohmann::json;
namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace {

bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || static_cast<unsigned char>(c) >= 0x80;
}

/// The variable names (without `--`) of every `var(--name` in `text`.
std::vector<std::string> varNames(std::string_view text) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = text.find("var(", pos)) != std::string_view::npos) {
        size_t i = pos + 4;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
            ++i;
        }
        pos = i;
        if (i + 2 > text.size() || text[i] != '-' || text[i + 1] != '-') {
            continue;
        }
        size_t j = i + 2;
        while (j < text.size() && isNameChar(text[j])) {
            ++j;
        }
        if (j > i + 2) {
            out.emplace_back(text.substr(i + 2, j - i - 2));
        }
        pos = j;
    }
    return out;
}

/// The last `#id` of each comma-separated selector of a rule prelude.
std::vector<std::string> preludeIds(std::string_view prelude) {
    std::vector<std::string> out;
    int depth = 0;
    std::string last;
    const auto flush = [&]() {
        if (!last.empty()) {
            out.push_back(last);
        }
        last.clear();
    };
    for (size_t i = 0; i < prelude.size(); ++i) {
        const char c = prelude[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        } else if (c == ',' && depth == 0) {
            flush();
        } else if (c == '#' && depth == 0) {
            size_t j = i + 1;
            while (j < prelude.size() && isNameChar(prelude[j])) {
                ++j;
            }
            if (j > i + 1) {
                last = std::string(prelude.substr(i + 1, j - i - 1));
            }
            i = j - 1;
        }
    }
    flush();
    return out;
}

/// One `var(--name` use in a stylesheet: the variable and the `#id`s of the innermost enclosing rule
/// whose selector names one (empty when none does).
struct StyleUse {
    std::string token;
    std::vector<std::string> ids;
};

std::vector<StyleUse> scanStylesheet(std::string_view css) {
    std::vector<StyleUse> out;
    std::vector<std::string> stack;
    std::string prelude;
    size_t i = 0;
    while (i < css.size()) {
        const char c = css[i];
        if (c == '/' && i + 1 < css.size() && css[i + 1] == '*') {
            const size_t end = css.find("*/", i + 2);
            i = end == std::string_view::npos ? css.size() : end + 2;
            continue;
        }
        if (c == '{') {
            stack.push_back(prelude);
            prelude.clear();
        } else if (c == '}') {
            if (!stack.empty()) {
                stack.pop_back();
            }
            prelude.clear();
        } else if (c == ';') {
            prelude.clear();
        } else if (c == 'v' && css.compare(i, 4, "var(") == 0) {
            const size_t close = css.find_first_of(";{}", i);
            const std::string_view chunk =
                css.substr(i, close == std::string_view::npos ? css.size() - i : close - i);
            const std::vector<std::string> names =
                varNames(chunk.substr(0, std::min<size_t>(chunk.size(), 260)));
            if (!names.empty()) {
                std::vector<std::string> ids;
                for (auto it = stack.rbegin(); it != stack.rend() && ids.empty(); ++it) {
                    ids = preludeIds(*it);
                }
                out.push_back(StyleUse{names.front(), ids});
            }
            prelude += c;
            ++i;
            continue;
        } else {
            prelude += c;
        }
        ++i;
    }
    return out;
}

/// What one symbol file defines and uses.
struct SymbolFileInfo {
    std::vector<std::string> names;
    std::set<std::string> tokens;
};

class GraphBuilder {
public:
    GraphBuilder(const UiProject& project, const DesignSystem& ds, Graph& graph)
        : m_project(project), m_ds(ds), m_graph(graph) {}

    void build() {
        m_root = pathFromUtf8(m_project.rootDir);
        addEntries();
        addProjectNodes();
        scanSymbolDir();
        for (const ProjectUi& ui : m_project.uis) {
            scanUi(ui);
        }
        for (size_t i = 0; i < m_graph.edges.size(); ++i) {
            m_graph.incoming[m_graph.edges[i].to].push_back(i);
            m_graph.outgoing[m_graph.edges[i].from].push_back(i);
        }
    }

private:
    void node(const std::string& ref) { m_graph.nodes.insert(ref); }

    void edge(const std::string& from, const std::string& to, const std::string& rel, bool computed) {
        if (!m_edgeKeys.insert(from + '\n' + to + '\n' + rel).second) {
            return;
        }
        m_graph.edges.push_back(Edge{from, to, rel, computed});
    }

    void storedLinks(const std::string& from, const std::vector<Link>& links) {
        for (const Link& link : links) {
            edge(from, detail::normalizeRef(link.to), link.rel, false);
        }
    }

    void entryEdges(const Entry& e) {
        const std::string self = entryRef(e.kind, e.id);
        storedLinks(self, e.links);
        if (e.source.kind.rfind("extracted:", 0) == 0 && e.source.kind.size() > 10) {
            edge(self, "ref:" + e.source.kind.substr(10), "derived-from", false);
        }
    }

    void addEntries() {
        for (const Group& g : m_ds.groups) {
            node(entryRef(EntryKind::Group, g.base.id));
            entryEdges(g.base);
        }
        for (const Swatch& s : m_ds.swatches) {
            node(entryRef(EntryKind::Swatch, s.base.id));
            entryEdges(s.base);
        }
        for (const TokenMeta& t : m_ds.tokens) {
            const std::string self = entryRef(EntryKind::Token, t.base.id);
            node(self);
            entryEdges(t.base);
            if (!t.from.empty()) {
                edge(self, detail::normalizeRef(t.from), "derived-from", false);
            }
        }
        for (const std::string& name : detail::sharedTokenNames(m_project)) {
            node(entryRef(EntryKind::Token, name.substr(2)));
        }
        for (const TypeRole& r : m_ds.typeRoles) {
            node(entryRef(EntryKind::TypeRole, r.base.id));
            entryEdges(r.base);
        }
        if (m_ds.typeExtra.is_object()) {
            for (auto it = m_ds.typeExtra.begin(); it != m_ds.typeExtra.end(); ++it) {
                const std::string self = entryRef(EntryKind::TypeRole, it.key());
                node(self);
                if (it->is_object()) {
                    for (auto sub = it->begin(); sub != it->end(); ++sub) {
                        node(self + "#" + sub.key());
                    }
                }
            }
        }
        for (const TextEntry& t : m_ds.texts) {
            const std::string self = entryRef(EntryKind::Text, t.base.id);
            node(self);
            for (const std::string& slug : detail::headingSlugs(t.body)) {
                node(self + "#" + slug);
            }
            entryEdges(t.base);
        }
        for (const TextEntry& t : m_ds.decisions) {
            node(entryRef(EntryKind::Decision, t.base.id));
            entryEdges(t.base);
        }
        const fs::path designRoot = pathFromUtf8(m_ds.rootDir);
        for (const ReferenceMeta& r : m_ds.references) {
            const std::string self = entryRef(EntryKind::Reference, r.base.id);
            node(self);
            entryEdges(r.base);
            for (const ReferenceMeta::Annotation& a : r.annotations) {
                const std::string anchor = self + "@" + a.id;
                node(anchor);
                storedLinks(anchor, a.links);
            }
            std::string bytes;
            const bool read = !m_ds.rootDir.empty() &&
                              bundle::detail::readFile(pathToUtf8(designRoot / pathFromUtf8(r.path)), bytes);
            m_graph.referenceHashes[r.base.id] = read ? detail::contentHash(bytes) : std::string();
        }
    }

    void addMotionKeys(const json& value, const std::string& prefix) {
        if (!value.is_object()) {
            return;
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string path = prefix.empty() ? it.key() : prefix + "." + it.key();
            node("motion:" + path);
            addMotionKeys(it.value(), path);
        }
    }

    void addProjectNodes() {
        std::error_code ec;
        const fs::path conventions = m_root / "conventions";
        if (fs::is_directory(conventions, ec)) {
            for (fs::recursive_directory_iterator it(conventions, ec), end; it != end; it.increment(ec)) {
                if (ec) {
                    break;
                }
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const std::string relative = genericPathToUtf8(
                    it->path().lexically_normal().lexically_relative(conventions.lexically_normal()));
                const std::string stem = pathToUtf8(it->path().stem());
                node("convention:" + stem);
                const size_t dot = relative.rfind('.');
                node("convention:" + (dot == std::string::npos ? relative : relative.substr(0, dot)));
            }
        }
        std::string motion;
        if (bundle::detail::readFile(pathToUtf8(m_root / "contracts" / "motion-spec.yaml"), motion)) {
            json spec;
            if (bundle::detail::parseYamlLite(motion, spec, nullptr)) {
                addMotionKeys(spec, std::string());
            }
        }
    }

    const SymbolFileInfo& symbolFile(const fs::path& path) {
        const std::string key = genericPathToUtf8(path.lexically_normal());
        const auto found = m_symbolFiles.find(key);
        if (found != m_symbolFiles.end()) {
            return found->second;
        }
        SymbolFileInfo& info = m_symbolFiles[key];
        std::string text;
        if (!bundle::detail::readFile(pathToUtf8(path), text)) {
            return info;
        }
        compile::DiagnosticSink sink;
        compile::DomTree dom;
        if (!compile::parseXtSymbolLibrary(text, key, sink, dom)) {
            return info;
        }
        for (uint32_t index : dom.symbolDefs) {
            if (index < dom.nodes.size()) {
                if (const compile::DomAttr* name = dom.nodes[index].attr("name")) {
                    info.names.push_back(name->value);
                }
            }
        }
        for (const auto& [css, line] : dom.inlineStyles) {
            (void)line;
            for (const StyleUse& use : scanStylesheet(css)) {
                info.tokens.insert(use.token);
            }
        }
        for (const compile::DomNode& n : dom.nodes) {
            if (const compile::DomAttr* style = n.attr("style")) {
                for (const std::string& name : varNames(style->value)) {
                    info.tokens.insert(name);
                }
            }
        }
        for (const std::string& name : info.names) {
            const std::string self = entryRef(EntryKind::Symbol, name);
            node(self);
            for (const std::string& token : info.tokens) {
                edge(self, entryRef(EntryKind::Token, token), "uses", true);
            }
        }
        return info;
    }

    void scanSymbolDir() {
        if (m_project.shared.symbols.empty()) {
            return;
        }
        const fs::path dir = m_root / pathFromUtf8(m_project.shared.symbols);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            return;
        }
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".xt") {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            symbolFile(file);
        }
    }

    void useInUi(const std::string& uiId, const std::set<std::string>& elementIds, const StyleUse& use) {
        const std::string token = entryRef(EntryKind::Token, use.token);
        edge(entryRef(EntryKind::Ui, uiId), token, "uses", true);
        for (const std::string& id : use.ids) {
            if (elementIds.count(id) != 0) {
                edge("layer:" + uiId + "#" + id, token, "uses", true);
            }
        }
    }

    void scanUi(const ProjectUi& ui) {
        const std::string uiRef = entryRef(EntryKind::Ui, ui.id);
        node(uiRef);
        const fs::path docPath = (m_root / pathFromUtf8(ui.doc)).lexically_normal();
        std::string text;
        if (!bundle::detail::readFile(pathToUtf8(docPath), text)) {
            return;
        }
        compile::DiagnosticSink sink;
        compile::DomTree dom;
        if (!compile::parseXt(text, pathToUtf8(docPath), sink, dom)) {
            return;
        }
        // The layers: every element id under <body>; `style=` uses belong to that element.
        std::set<std::string> elementIds;
        std::vector<std::pair<std::string, std::string>> styleAttrs;  // (element id or "", style)
        if (dom.body < dom.nodes.size()) {
            std::vector<uint32_t> stack{dom.body};
            while (!stack.empty()) {
                const uint32_t index = stack.back();
                stack.pop_back();
                const compile::DomNode& n = dom.nodes[index];
                const compile::DomAttr* id = n.attr("id");
                if (id != nullptr && !id->value.empty()) {
                    elementIds.insert(id->value);
                    node("layer:" + ui.id + "#" + id->value);
                }
                if (const compile::DomAttr* style = n.attr("style")) {
                    styleAttrs.emplace_back(id != nullptr ? id->value : std::string(), style->value);
                }
                if (n.tag == "use") {
                    if (const compile::DomAttr* symbol = n.attr("symbol")) {
                        node(entryRef(EntryKind::Symbol, symbol->value));
                    }
                }
                for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
                    if (*it < dom.nodes.size()) {
                        stack.push_back(*it);
                    }
                }
            }
        }
        for (const auto& [id, style] : styleAttrs) {
            for (const std::string& name : varNames(style)) {
                StyleUse use{name, {}};
                if (!id.empty()) {
                    use.ids.push_back(id);
                }
                useInUi(ui.id, elementIds, use);
            }
        }
        for (const auto& [css, line] : dom.inlineStyles) {
            (void)line;
            for (const StyleUse& use : scanStylesheet(css)) {
                useInUi(ui.id, elementIds, use);
            }
        }
        const fs::path docDir = docPath.parent_path();
        for (const std::string& href : dom.linkHrefs) {
            const fs::path sheet = (docDir / pathFromUtf8(href)).lexically_normal();
            if (sheet.extension() != ".xts") {
                continue;
            }
            std::string css;
            if (!bundle::detail::readFile(pathToUtf8(sheet), css)) {
                continue;
            }
            for (const StyleUse& use : scanStylesheet(css)) {
                useInUi(ui.id, elementIds, use);
            }
        }
        for (const std::string& href : dom.symbolHrefs) {
            if (href.rfind("xt-standard:", 0) == 0) {
                continue;
            }
            const fs::path file = (docDir / pathFromUtf8(href)).lexically_normal();
            const SymbolFileInfo& info = symbolFile(file);
            for (const std::string& token : info.tokens) {
                edge(uiRef, entryRef(EntryKind::Token, token), "uses", true);
            }
            if (info.names.empty()) {
                node(entryRef(EntryKind::Symbol, pathToUtf8(file.stem())));  // not pulled in yet
            }
        }
    }

    const UiProject& m_project;
    const DesignSystem& m_ds;
    Graph& m_graph;
    fs::path m_root;
    std::set<std::string> m_edgeKeys;
    std::map<std::string, SymbolFileInfo> m_symbolFiles;
};

/// `ref` and every anchor under it (`ref#...`, `ref@...`) present in the graph.
std::vector<std::string> withAnchors(const Graph& graph, const std::string& ref) {
    std::vector<std::string> out{ref};
    for (const char sep : {'#', '@'}) {
        const std::string prefix = ref + sep;
        for (auto it = graph.nodes.lower_bound(prefix); it != graph.nodes.end() && it->rfind(prefix, 0) == 0;
             ++it) {
            out.push_back(*it);
        }
    }
    return out;
}

void classify(const std::string& ref, Usage& out) {
    if (ref.rfind("ui:", 0) == 0) {
        out.uis.push_back(ref);
    } else if (ref.rfind("layer:", 0) == 0) {
        out.layers.push_back(ref);
    } else if (ref.rfind("symbol:", 0) == 0) {
        out.symbols.push_back(ref);
    } else {
        out.entries.push_back(ref);
    }
}

Usage collect(const std::set<std::string>& refs) {
    Usage out;
    for (const std::string& ref : refs) {  // sorted
        classify(ref, out);
    }
    return out;
}

void addIssue(std::vector<Issue>& out, const char* id, Severity severity, std::string entry,
              std::string message) {
    out.push_back(Issue{id, severity, std::move(entry), std::move(message)});
}

bool hasEdges(const Graph& graph, const std::string& ref) {
    for (const std::string& r : withAnchors(graph, ref)) {
        const auto in = graph.incoming.find(r);
        const auto outIt = graph.outgoing.find(r);
        if ((in != graph.incoming.end() && !in->second.empty()) ||
            (outIt != graph.outgoing.end() && !outIt->second.empty())) {
            return true;
        }
    }
    return false;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

}  // namespace

Status buildGraph(const UiProject& project, const DesignSystem& ds, Graph& graph, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (project.isUnnamed()) {
        if (error != nullptr) {
            *error = "an unnamed UI Project has no reference graph";
        }
        return Status::kInvalidArgument;
    }
    Graph built;
    GraphBuilder builder(project, ds, built);
    builder.build();
    graph = std::move(built);
    return Status::kOk;
}

Usage usage(const Graph& graph, std::string_view ref) {
    const std::string self = detail::normalizeRef(ref);
    const std::vector<std::string> targets = withAnchors(graph, self);
    const std::set<std::string> targetSet(targets.begin(), targets.end());
    std::set<std::string> found;
    for (const std::string& target : targets) {
        const auto it = graph.incoming.find(target);
        if (it == graph.incoming.end()) {
            continue;
        }
        for (size_t index : it->second) {
            const std::string& from = graph.edges[index].from;
            if (targetSet.count(from) == 0) {
                found.insert(from);
            }
        }
    }
    return collect(found);
}

Usage impact(const Graph& graph, std::string_view ref) {
    const std::string self = detail::normalizeRef(ref);
    const std::vector<std::string> targets = withAnchors(graph, self);
    std::set<std::string> visited(targets.begin(), targets.end());
    std::deque<std::string> queue(targets.begin(), targets.end());
    std::set<std::string> found;
    while (!queue.empty()) {
        const std::string current = queue.front();
        queue.pop_front();
        const auto it = graph.incoming.find(current);
        if (it == graph.incoming.end()) {
            continue;
        }
        for (size_t index : it->second) {
            const std::string& from = graph.edges[index].from;
            for (const std::string& next : withAnchors(graph, from)) {
                if (visited.insert(next).second) {
                    queue.push_back(next);
                    found.insert(next);
                }
            }
        }
    }
    return collect(found);
}

std::vector<Issue> validate(const DesignSystem& ds, const Graph& graph) {
    std::vector<Issue> out;
    // design.invalidRel, design.brokenLink: every stored edge.
    for (const Edge& e : graph.edges) {
        if (e.computed) {
            continue;
        }
        if (!parseRel(e.rel)) {
            addIssue(out, "design.invalidRel", Severity::Error, e.from,
                     "link rel '" + e.rel +
                         "' is not one of derived-from, evidence-for, illustrates, applies-to, "
                         "supersedes, conflicts-with, see-also");
        }
        if (!parseEntryRef(e.to, nullptr, nullptr)) {
            addIssue(out, "design.brokenLink", Severity::Error, e.from,
                     "'" + e.to + "' is not a valid reference");
        } else if (!graph.has(e.to)) {
            addIssue(out, "design.brokenLink", Severity::Error, e.from, "'" + e.to + "' does not exist");
        }
    }
    // design.tokenNoDescription.
    for (const TokenMeta& t : ds.tokens) {
        if (t.description.empty()) {
            addIssue(out, "design.tokenNoDescription", Severity::Info, entryRef(EntryKind::Token, t.base.id),
                     "'--" + t.base.id + "' has no description");
        }
    }
    // design.nearDuplicateSwatch: ΔE2000 < 3.
    for (size_t i = 0; i < ds.swatches.size(); ++i) {
        for (size_t j = i + 1; j < ds.swatches.size(); ++j) {
            double dE = 0.0;
            if (!bundle::detail::hexDeltaE2000(ds.swatches[i].value, ds.swatches[j].value, &dE) ||
                dE >= 3.0) {
                continue;
            }
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.2f", dE);
            addIssue(out, "design.nearDuplicateSwatch", Severity::Info,
                     entryRef(EntryKind::Swatch, ds.swatches[i].base.id),
                     "ΔE2000 " + std::string(buffer) + " to swatch:" + ds.swatches[j].base.id);
        }
    }
    // design.stale: a derived-from reference whose file changed; a `from` swatch whose value changed.
    std::map<std::string, const ReferenceMeta*> references;
    for (const ReferenceMeta& r : ds.references) {
        references[r.base.id] = &r;
    }
    std::set<std::string> staleReported;
    for (const Edge& e : graph.edges) {
        if (e.computed || e.rel != "derived-from" || e.to.rfind("ref:", 0) != 0) {
            continue;
        }
        std::string id = e.to.substr(4);
        const size_t at = id.find('@');
        if (at != std::string::npos) {
            id.resize(at);
        }
        const auto ref = references.find(id);
        const auto now = graph.referenceHashes.find(id);
        if (ref == references.end() || now == graph.referenceHashes.end() || now->second.empty() ||
            ref->second->contentHash.empty() || ref->second->contentHash == now->second) {
            continue;
        }
        if (staleReported.insert(e.from + "\n" + id).second) {
            addIssue(out, "design.stale", Severity::Info, e.from,
                     "derived from ref:" + id + ", whose content changed since it was recorded");
        }
    }
    std::map<std::string, const Swatch*> swatches;
    for (const Swatch& s : ds.swatches) {
        swatches[s.base.id] = &s;
    }
    for (const TokenMeta& t : ds.tokens) {
        const std::string from = detail::normalizeRef(t.from);
        if (from.rfind("swatch:", 0) != 0 || !t.base.extra.is_object()) {
            continue;
        }
        const auto recorded = t.base.extra.find("fromValue");
        const auto swatch = swatches.find(from.substr(7));
        if (recorded == t.base.extra.end() || !recorded->is_string() || swatch == swatches.end() ||
            lowerAscii(recorded->get<std::string>()) == lowerAscii(swatch->second->value)) {
            continue;
        }
        addIssue(out, "design.stale", Severity::Info, entryRef(EntryKind::Token, t.base.id),
                 "derived from " + from + " at " + recorded->get<std::string>() + "; it is now " +
                     swatch->second->value);
    }
    // design.referenceNoLicense.
    for (const ReferenceMeta& r : ds.references) {
        if (r.license.empty() || r.license == "unknown") {
            addIssue(out, "design.referenceNoLicense", Severity::Info,
                     entryRef(EntryKind::Reference, r.base.id), "reference license is unknown");
        }
    }
    // design.orphan: a swatch or reference with no link either way and no use.
    for (const Swatch& s : ds.swatches) {
        const std::string self = entryRef(EntryKind::Swatch, s.base.id);
        if (!hasEdges(graph, self)) {
            addIssue(out, "design.orphan", Severity::Info, self, "swatch has no links and is not used");
        }
    }
    for (const ReferenceMeta& r : ds.references) {
        const std::string self = entryRef(EntryKind::Reference, r.base.id);
        if (!hasEdges(graph, self)) {
            addIssue(out, "design.orphan", Severity::Info, self, "reference has no links and is not used");
        }
    }
    return out;
}

}  // namespace xt::bundle::design
