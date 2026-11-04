#include "migrate_v2.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/canonical_writer.h"
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/migrate.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"
#include "xt/ir_pod.h"

/// T-589 (Docs/design/54-source-format-v2.md §54.10, §54.11): migrateToV2(), the driver of the
/// eight steps (see xt/compile/migrate.h) and the self-check. CSS work is in migrate_sheet.cpp,
/// markup work in migrate_markup.cpp, the report in migrate_report.cpp.
///
/// Not covered by the design (choices made here):
///   - The section order of a migrated file follows version 1's cascade order: the linked sheets
///     first (each as `<layout src>`, `<style src>`, `<timelines src>`), then each inline
///     `<style>` as inline sections, then the `#id` rules of step 6 as the last `<layout>` (inline
///     declarations won over every rule in version 1; last is the closest a rule gets).
///   - A linked sheet keeps its file name for its style part even when nothing is left in it, so
///     no other file's reference breaks.
///   - The self-check compiles both versions from memory (compileDocumentFromStrings(), the
///     version 1 texts and the migrated texts under the same absolute paths), so both see the same
///     pipeline; a document whose version 1 does not compile is not checked (reported).

namespace xt::compile {

namespace {

namespace fs = std::filesystem;
using migrate::SplitSheet;
using migrate::TagAttr;
using migrate::TextEdit;
using migrate::TriggerRule;

constexpr uint32_t kNone = 0xFFFFFFFFu;

std::string normalPath(const fs::path& path) {
    std::error_code ec;
    return genericPathToUtf8(fs::absolute(path, ec).lexically_normal());
}

bool readText(const std::string& path, std::string& out) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.clear();
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') {
            continue;
        }
        out.push_back(raw[i]);
    }
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);  // A UTF-8 byte order mark.
    }
    return true;
}

bool isStandardHref(std::string_view href) { return href.rfind("xt-standard:", 0) == 0; }

/// `x.xts` -> `x.<part>.xts`.
std::string partHref(const std::string& href, const char* part) {
    const std::string ext = ".xts";
    if (href.size() > ext.size() && href.compare(href.size() - ext.size(), ext.size(), ext) == 0) {
        return href.substr(0, href.size() - ext.size()) + "." + part + ext;
    }
    return href + "." + part + ext;
}

std::string firstError(const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& d : diagnostics) {
        if (d.level == Diagnostic::Level::Error) {
            return d.file + ":" + std::to_string(d.line) + ": " + d.message;
        }
    }
    return "unknown error";
}

/// The chunks of an `.xtir` (type and payload) without `SRCM`, whose lines move with the text.
std::vector<std::string> chunksWithoutSourceMap(const std::vector<uint8_t>& ir) {
    std::vector<std::string> chunks;
    size_t pos = sizeof(IrHeader);
    while (pos + sizeof(IrChunkHeader) <= ir.size()) {
        IrChunkHeader header{};
        std::memcpy(&header, ir.data() + pos, sizeof(header));
        const size_t begin = pos + sizeof(header);
        if (begin + header.payloadLength > ir.size()) {
            break;
        }
        if (std::memcmp(header.type, "SRCM", 4) != 0) {
            chunks.emplace_back(reinterpret_cast<const char*>(ir.data() + pos), sizeof(header.type));
            chunks.back().append(reinterpret_cast<const char*>(ir.data() + begin), header.payloadLength);
        }
        pos = begin + header.payloadLength;
    }
    return chunks;
}

/// A linked sheet, migrated once however many documents link it.
struct SheetEntry {
    bool read = false;
    SplitSheet split;
};

/// One file of the migration before writing: its original and new text.
struct FileText {
    std::string original;
    std::string migrated;
    bool created = false;
    bool hasMigrated = false;
};

class Migrator {
public:
    Migrator(const MigrateOptions& options, MigrateReport& report) : m_options(options), m_report(report) {}

    void run(const std::vector<std::string>& inputs) {
        for (const std::string& path : inputs) {
            m_queue.push_back(path);
        }
        for (size_t i = 0; i < m_queue.size(); ++i) {
            const std::string path = m_queue[i];  // A copy: migrateXt() appends to m_queue.
            migrateXt(path);
        }
        if (m_options.selfCheck) {
            for (const std::string& path : m_uiDocs) {
                selfCheck(path);
            }
        }
        for (const std::string& path : m_order) {
            m_report.files.push_back(std::move(m_entries[path]));
        }
    }

    void collect(std::vector<MigratedFile>& out) const {
        for (const auto& [path, file] : m_files) {
            if (file.hasMigrated) {
                out.push_back(MigratedFile{path, file.migrated, file.created});
            }
        }
    }

private:
    MigrateFileReport& reportFor(const std::string& path, const char* kind) {
        auto it = m_entries.find(path);
        if (it != m_entries.end()) {
            return it->second;
        }
        m_order.push_back(path);
        MigrateFileReport& entry = m_entries[path];
        entry.path = path;
        entry.kind = kind;
        entry.status = "migrated";
        return entry;
    }

    void setOutput(const std::string& path, std::string text, bool created) {
        FileText& f = m_files[path];
        f.migrated = std::move(text);
        f.created = created;
        f.hasMigrated = true;
    }

    // ---- sheets ------------------------------------------------------------------------------

    SheetEntry& sheet(const std::string& path) {
        auto it = m_sheets.find(path);
        if (it != m_sheets.end()) {
            return it->second;
        }
        SheetEntry& entry = m_sheets[path];
        std::string text;
        if (!readText(path, text)) {
            return entry;
        }
        entry.read = true;
        entry.split = migrate::splitV1Sheet(text, m_options.anchors);
        m_files[path].original = text;
        MigrateFileReport& report = reportFor(path, "fragment");
        report.rulesSplit = entry.split.rulesSplit;
        report.anchors = entry.split.anchors;
        setOutput(path, migrate::writeFragment(entry.split.style, entry.split.trailingComments), false);
        if (!entry.split.layout.empty()) {
            const std::string layoutPath = normalPath(pathFromUtf8(partHref(path, "layout")));
            setOutput(layoutPath, migrate::writeFragment(entry.split.layout, {}), true);
            report.notes.push_back("layout part -> " + layoutPath);
        }
        if (!entry.split.timelines.empty()) {
            const std::string timelinesPath = normalPath(pathFromUtf8(partHref(path, "timelines")));
            setOutput(timelinesPath, migrate::writeFragment(entry.split.timelines, {}), true);
            report.notes.push_back("timelines part -> " + timelinesPath);
        }
        return entry;
    }

    // ---- one .xt -----------------------------------------------------------------------------

    /// The state of the `.xt` being migrated.
    struct Doc {
        std::string path;
        fs::path dir;
        std::string text;
        DomTree tree;
        std::vector<TextEdit> edits;
        std::vector<uint8_t> inSymbol;   ///< Per node: inside a `<symbol>` definition.
        std::vector<uint32_t> symbolOf;  ///< Per node: the definition it belongs to, or kNone.
        std::set<std::string> usedIds;
        std::map<uint32_t, std::string> assignedIds;
        std::map<uint32_t, std::vector<std::pair<std::string, std::string>>> newAttrs;
        std::map<uint32_t, std::vector<CanonicalItem>> symbolLayout;   ///< Step 6, per definition.
        std::vector<CanonicalItem> docLayout;                          ///< Step 6, document level.
        std::map<uint32_t, std::vector<CanonicalPort>> symbolPorts;    ///< Step 2, per definition.
        std::vector<std::pair<std::string, std::string>> flowRenames;  ///< Step 2: flow endpoints.
        MigrateFileReport* report = nullptr;
    };

    void migrateXt(const std::string& path) {
        if (!m_seen.insert(path).second) {
            return;
        }
        Doc doc;
        doc.path = path;
        doc.dir = pathFromUtf8(path).parent_path();
        if (!readText(path, doc.text)) {
            MigrateFileReport& r = reportFor(path, m_libraries.count(path) != 0 ? "library" : "ui");
            r.status = "skipped";
            r.reason = "cannot read the file";
            std::string standard;
            if (m_libraries.count(path) != 0 && standardCopy(path, standard)) {
                // A UI Project's standard symbols are not checked in (T-263f) and pack() copies them
                // from the standard library; the self-check reads that copy, unmigrated, both times.
                r.reason += "; the self-check uses the standard library's copy, as pack copies it";
                m_files[path].original = std::move(standard);
            }
            return;
        }
        DiagnosticSink sink;
        const bool parsed = parseXtSymbolLibrary(doc.text, path, sink, doc.tree);
        const bool isUi = doc.tree.body != kNone;
        MigrateFileReport& report = reportFor(path, isUi ? "ui" : "library");
        doc.report = &report;
        if (!parsed) {
            report.status = "skipped";
            report.reason = "does not parse: " + firstError(sink.all());
            return;
        }
        if (doc.tree.formatVersion >= 2) {
            report.status = "skipped";
            report.reason = "already version 2";
            return;
        }
        m_files[path].original = doc.text;
        prepare(doc);
        CanonicalXt out;
        out.title = doc.tree.title;
        std::vector<TriggerRule> triggers;
        linkSheets(doc, isUi, out, triggers);
        movePorts(doc, out);
        placeTriggers(doc, triggers);
        moveInlineGeometry(doc);
        assemble(doc, out);
        if (!report.errors.empty()) {
            report.status = "error";
            return;
        }
        setOutput(path, writeCanonicalXt(out), false);
        rewriteFlow(doc);
        if (isUi) {
            m_uiDocs.push_back(path);
        }
    }

    /// The standard library file of the same name as `path` (its root, `mobile/`, `list/`).
    bool standardCopy(const std::string& path, std::string& text) const {
        if (m_options.standardSymbolDir.empty()) {
            return false;
        }
        const fs::path dir = pathFromUtf8(m_options.standardSymbolDir);
        const fs::path name = pathFromUtf8(path).filename();
        for (const fs::path& d : {dir, dir / "mobile", dir / "list"}) {
            if (readText(genericPathToUtf8(d / name), text)) {
                return true;
            }
        }
        return false;
    }

    /// T-604 ruling: the document's sibling `<stem>.xtflow` (the importer's convention, T-135) with
    /// every `"#<root id>.<port>"` endpoint of a moved port written as `"#document.<port>"`.
    void rewriteFlow(Doc& doc) {
        if (doc.flowRenames.empty()) {
            return;
        }
        const std::string flowPath = normalPath(pathFromUtf8(doc.path).replace_extension(".xtflow"));
        std::string text;
        if (!readText(flowPath, text)) {
            return;
        }
        const std::string original = text;
        uint32_t count = 0;
        for (const auto& [from, to] : doc.flowRenames) {
            const std::string quoted = "\"" + from + "\"";
            for (size_t at = text.find(quoted); at != std::string::npos;
                 at = text.find(quoted, at + to.size() + 2)) {
                text.replace(at, quoted.size(), "\"" + to + "\"");
                ++count;
            }
        }
        if (count == 0) {
            return;
        }
        m_files[flowPath].original = original;
        setOutput(flowPath, std::move(text), false);
        MigrateFileReport& report = reportFor(flowPath, "flow");
        report.notes.push_back(std::to_string(count) + " endpoint(s) #<root id>.<port> -> #document.<port>");
    }

    void prepare(Doc& doc) {
        const size_t n = doc.tree.nodes.size();
        doc.inSymbol.assign(n, 0);
        doc.symbolOf.assign(n, kNone);
        for (uint32_t def : doc.tree.symbolDefs) {
            std::vector<uint32_t> stack{def};
            while (!stack.empty()) {
                const uint32_t i = stack.back();
                stack.pop_back();
                doc.inSymbol[i] = 1;
                doc.symbolOf[i] = def;
                for (uint32_t c : doc.tree.nodes[i].children) {
                    stack.push_back(c);
                }
            }
        }
        for (const DomNode& node : doc.tree.nodes) {
            if (const DomAttr* id = node.attr("id"); id != nullptr && !id->value.empty()) {
                doc.usedIds.insert(id->value);
            }
        }
    }

    /// Step 1 (links, symbol libraries, inline `<style>`), 3 and 5 by way of splitV1Sheet().
    void linkSheets(Doc& doc, bool isUi, CanonicalXt& out, std::vector<TriggerRule>& triggers) {
        MigrateFileReport& report = *doc.report;
        for (const std::string& href : doc.tree.symbolHrefs) {
            if (!isUi) {
                report.notes.push_back("dropped <link rel=\"symbols\" href=\"" + href +
                                       "\"> (version 1 never loaded a library's own libraries)");
                continue;
            }
            out.symbolsSrc.push_back(href);
            if (!isStandardHref(href)) {
                m_queue.push_back(normalPath(doc.dir / pathFromUtf8(href)));
                m_libraries.insert(m_queue.back());
            }
        }
        for (const std::string& href : doc.tree.linkHrefs) {
            if (!isUi) {
                report.notes.push_back("dropped <link href=\"" + href +
                                       "\"> (version 1 never loaded a library's own stylesheets, XF-017)");
                continue;
            }
            const std::string sheetPath = normalPath(doc.dir / pathFromUtf8(href));
            SheetEntry& entry = sheet(sheetPath);
            if (!entry.read) {
                // Version 1 does not compile either; the reference is kept as it is.
                report.notes.push_back("the linked sheet \"" + href +
                                       "\" cannot be read; kept as <style src>");
                out.style.push_back(CanonicalSection{href, {}, {}});
                continue;
            }
            if (!entry.split.layout.empty()) {
                out.layout.push_back(CanonicalSection{partHref(href, "layout"), {}, {}});
            }
            out.style.push_back(CanonicalSection{href, {}, {}});
            if (!entry.split.timelines.empty()) {
                out.timelines.push_back(CanonicalSection{partHref(href, "timelines"), {}, {}});
            }
            triggers.insert(triggers.end(), entry.split.triggers.begin(), entry.split.triggers.end());
        }
        for (const auto& [text, line] : doc.tree.inlineStyles) {
            (void)line;
            SplitSheet split = migrate::splitV1Sheet(text, m_options.anchors);
            report.rulesSplit += split.rulesSplit;
            report.anchors += split.anchors;
            if (!split.layout.empty()) {
                out.layout.push_back(CanonicalSection{{}, std::move(split.layout), {}});
            }
            if (!split.style.empty() || !split.trailingComments.empty()) {
                out.style.push_back(
                    CanonicalSection{{}, std::move(split.style), std::move(split.trailingComments)});
            }
            if (!split.timelines.empty()) {
                out.timelines.push_back(CanonicalSection{{}, std::move(split.timelines), {}});
            }
            triggers.insert(triggers.end(), split.triggers.begin(), split.triggers.end());
        }
    }

    /// The attributes of node `i`'s start tag in the source.
    bool tagOf(const Doc& doc, uint32_t i, std::vector<TagAttr>& attrs, size_t& nameEnd,
               size_t& close) const {
        const DomNode& node = doc.tree.nodes[i];
        if (node.sourceEnd <= node.sourceBegin) {
            return false;
        }
        return migrate::scanStartTag(doc.text, node.sourceBegin, attrs, nameEnd, close);
    }

    void removeAttr(Doc& doc, uint32_t i, const std::string& name) {
        std::vector<TagAttr> attrs;
        size_t nameEnd = 0;
        size_t close = 0;
        if (!tagOf(doc, i, attrs, nameEnd, close)) {
            return;
        }
        for (const TagAttr& a : attrs) {
            if (a.name == name) {
                doc.edits.push_back(TextEdit{a.begin, a.end - a.begin, std::string()});
                return;
            }
        }
    }

    /// Step 2.
    void movePorts(Doc& doc, CanonicalXt& out) {
        const DomTree& tree = doc.tree;
        std::map<uint32_t, std::vector<CanonicalPort>> symbolPorts;
        for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
            const DomNode& node = tree.nodes[i];
            for (const DomAttr& attr : node.attrs) {
                if (attr.name.size() <= 5 || attr.name.compare(0, 5, "port:") != 0) {
                    continue;
                }
                const uint32_t def = doc.symbolOf[i];
                // T-604 ruling (53, "v2 document port addressing"): a port on the UI root element
                // (a direct element child of <body>) moves into the document's <ports> too; its
                // `#<root id>.<port>` flow endpoints become `#document.<port>` (rewriteFlow()).
                const bool uiRoot = def == kNone && tree.body != kNone && node.parent == tree.body;
                const bool root =
                    def == kNone ? (i == tree.body || uiRoot)
                                 : (!tree.nodes[def].children.empty() && tree.nodes[def].children[0] == i);
                CanonicalPort port{attr.name.substr(5), attr.value, false, {}};
                if (uiRoot) {
                    const DomAttr* id = node.attr("id");
                    if (id != nullptr && !id->value.empty()) {
                        doc.flowRenames.emplace_back("#" + id->value + "." + port.name,
                                                     "#document." + port.name);
                    }
                    doc.report->notes.push_back("port:" + port.name + " moved from the UI root <" + node.tag +
                                                "> line " + std::to_string(node.line) + " to <ports>");
                }
                if (!root) {
                    // 54 §54.4 has a `<ports>` for `<body>` (and, T-604, the UI root) and for a
                    // symbol's root only; a port declared on another element is addressed as `#<id>.<port>`
                    // (flows, the manifest's flow interface), which moving it would change.
                    const DomAttr* id = node.attr("id");
                    doc.report->errors.push_back(
                        "port:" + port.name + " is declared on <" + node.tag +
                        (id != nullptr ? " id=\"" + id->value + "\"" : std::string()) + "> line " +
                        std::to_string(node.line) + ", not on " +
                        (def == kNone ? "<body>" : "the symbol's root") +
                        "; source format 2 has no place for it (moving it would change its name #" +
                        (id != nullptr ? id->value : std::string("<id>")) + "." + port.name + ")");
                    continue;
                }
                if (def == kNone) {
                    out.ports.push_back(std::move(port));
                } else {
                    symbolPorts[def].push_back(std::move(port));
                }
                ++doc.report->portsMoved;
                removeAttr(doc, i, attr.name);
            }
        }
        doc.symbolPorts = std::move(symbolPorts);
    }

    std::string idFor(Doc& doc, uint32_t i) {
        if (const DomAttr* id = doc.tree.nodes[i].attr("id"); id != nullptr && !id->value.empty()) {
            return id->value;
        }
        auto it = doc.assignedIds.find(i);
        if (it != doc.assignedIds.end()) {
            return it->second;
        }
        const std::string id = migrate::autoElementId(doc.tree, i, doc.usedIds);
        doc.usedIds.insert(id);
        doc.assignedIds.emplace(i, id);
        doc.report->idsAssigned.push_back("<" + doc.tree.nodes[i].tag + "> line " +
                                          std::to_string(doc.tree.nodes[i].line) + " -> " + id);
        return id;
    }

    bool hasOwnAttr(const Doc& doc, uint32_t i, const std::string& name) const {
        return doc.tree.nodes[i].attr(name) != nullptr;
    }

    void addTriggerAttr(Doc& doc, uint32_t i, const std::string& name, const std::string& actions) {
        for (const auto& [existing, value] : doc.newAttrs[i]) {
            if (existing == name) {
                doc.report->errors.push_back("two rule triggers write " + name + " on <" +
                                             doc.tree.nodes[i].tag + "> line " +
                                             std::to_string(doc.tree.nodes[i].line));
                return;
            }
        }
        doc.newAttrs[i].emplace_back(name, actions);
        ++doc.report->triggersMoved;
    }

    /// Step 4.
    void placeTriggers(Doc& doc, const std::vector<TriggerRule>& triggers) {
        const DomTree& tree = doc.tree;
        for (const TriggerRule& t : triggers) {
            const std::string where = "trigger on-" + t.event + " of \"" + t.selector + "\"";
            if (t.inMedia) {
                doc.report->errors.push_back(where + " is inside @media; write it on the element by hand");
                continue;
            }
            if (t.symbol.empty() && t.selector.size() > 1 && t.selector[0] == '#' &&
                t.selector.find('/') != std::string::npos) {
                const std::string useId = t.selector.substr(1, t.selector.find('/') - 1);
                const std::string path = t.selector.substr(t.selector.find('/') + 1);
                bool found = false;
                for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
                    const DomAttr* id = tree.nodes[i].attr("id");
                    if (doc.inSymbol[i] == 0 && tree.nodes[i].tag == "use" && id != nullptr &&
                        id->value == useId) {
                        addTriggerAttr(doc, i, "on:" + t.event + "@" + path, t.actions);
                        found = true;
                    }
                }
                if (!found) {
                    doc.report->errors.push_back(where + ": no <use id=\"" + useId + "\"> in the document");
                }
                continue;
            }
            if (t.symbol.empty() && tree.body == kNone) {
                // A library's own document-wide trigger rule applied to the documents using it.
                doc.report->errors.push_back(where +
                                             " in a symbol library is not inside symbol(X); move it by hand");
                continue;
            }
            uint32_t def = kNone;
            if (!t.symbol.empty()) {
                for (uint32_t s : tree.symbolDefs) {
                    const DomAttr* name = tree.nodes[s].attr("name");
                    def = name != nullptr && name->value == t.symbol ? s : def;
                }
                if (def == kNone) {
                    placeLibraryTrigger(doc, t, where);
                    continue;
                }
            }
            bool ok = true;
            uint32_t matched = 0;
            for (uint32_t i = 0; i < tree.nodes.size() && ok; ++i) {
                const bool inScope = def == kNone ? (doc.inSymbol[i] == 0 && inBody(doc, i))
                                                  : (doc.symbolOf[i] == def && i != def);
                if (!inScope || !migrate::matchSimpleSelector(tree, i, t.selector, ok)) {
                    continue;
                }
                ++matched;
                if (hasOwnAttr(doc, i, "on:" + t.event)) {
                    doc.report->notes.push_back(where + ": <" + tree.nodes[i].tag + "> line " +
                                                std::to_string(tree.nodes[i].line) +
                                                " has its own on:" + t.event + ", which version 1 kept");
                    continue;
                }
                addTriggerAttr(doc, i, "on:" + t.event, t.actions);
            }
            if (!ok) {
                doc.report->errors.push_back(where + ": the selector is not a plain tag/#id/.class selector");
            } else if (matched == 0) {
                doc.report->notes.push_back(where +
                                            " matched no element; dropped (it did nothing in version 1)");
            }
        }
    }

    /// `symbol(X) { #<path> { on-... } }` for a symbol of a library: on every `<use symbol="X">`.
    void placeLibraryTrigger(Doc& doc, const TriggerRule& t, const std::string& where) {
        if (t.selector.size() < 2 || t.selector[0] != '#' ||
            t.selector.find_first_of(" >.:[") != std::string::npos) {
            doc.report->errors.push_back(where + " inside symbol(" + t.symbol +
                                         "): only an #id selector can move onto the instances");
            return;
        }
        for (uint32_t i = 0; i < doc.tree.nodes.size(); ++i) {
            const DomAttr* symbol = doc.tree.nodes[i].attr("symbol");
            if (doc.inSymbol[i] == 0 && doc.tree.nodes[i].tag == "use" && symbol != nullptr &&
                symbol->value == t.symbol) {
                idFor(doc, i);
                addTriggerAttr(doc, i, "on:" + t.event + "@" + t.selector.substr(1), t.actions);
            }
        }
    }

    bool inBody(const Doc& doc, uint32_t i) const {
        for (uint32_t n = i; n != kNone; n = doc.tree.nodes[n].parent) {
            if (n == doc.tree.body) {
                return true;
            }
        }
        return false;
    }

    /// Step 6.
    void moveInlineGeometry(Doc& doc) {
        const DomTree& tree = doc.tree;
        for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
            const DomAttr* style = tree.nodes[i].attr("style");
            if (style == nullptr || (doc.inSymbol[i] != 0 && doc.symbolOf[i] == i) ||
                (!inBody(doc, i) && doc.inSymbol[i] == 0)) {
                continue;
            }
            std::vector<CanonicalItem> layout;
            std::string rest;
            for (const std::string& decl : migrate::splitDeclarations(style->value)) {
                if (migrate::isLayoutDeclaration(decl)) {
                    layout.push_back(CanonicalItem{{}, foldSpaces(decl), false, {}});
                } else {
                    rest += (rest.empty() ? "" : "; ") + decl;
                }
            }
            if (layout.empty()) {
                continue;
            }
            const std::string id = idFor(doc, i);
            if (m_options.anchors && migrate::reverseAnchors(layout)) {
                ++doc.report->anchors;
            }
            CanonicalItem rule{{}, "#" + id, true, std::move(layout)};
            if (doc.inSymbol[i] != 0) {
                doc.symbolLayout[doc.symbolOf[i]].push_back(std::move(rule));
            } else {
                doc.docLayout.push_back(std::move(rule));
            }
            ++doc.report->inlineMoved;
            std::vector<TagAttr> attrs;
            size_t nameEnd = 0;
            size_t close = 0;
            if (!tagOf(doc, i, attrs, nameEnd, close)) {
                continue;
            }
            for (const TagAttr& a : attrs) {
                if (a.name == "style") {
                    doc.edits.push_back(TextEdit{
                        a.begin, a.end - a.begin,
                        rest.empty() ? std::string() : " style=\"" + migrate::escapeAttr(rest) + "\""});
                }
            }
        }
    }

    static std::string foldSpaces(const std::string& s) {
        std::string out;
        bool space = false;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\n') {
                space = !out.empty();
                continue;
            }
            if (space) {
                out.push_back(' ');
                space = false;
            }
            out.push_back(c);
        }
        return out;
    }

    /// The id / trigger attribute insertions, the nested `<symbol>` removals, then step 8.
    void assemble(Doc& doc, CanonicalXt& out) {
        const DomTree& tree = doc.tree;
        std::set<uint32_t> touched;
        for (const auto& [i, id] : doc.assignedIds) {
            touched.insert(i);
        }
        for (const auto& [i, attrs] : doc.newAttrs) {
            touched.insert(i);
        }
        for (uint32_t i : touched) {
            std::vector<TagAttr> attrs;
            size_t nameEnd = 0;
            size_t close = 0;
            if (!tagOf(doc, i, attrs, nameEnd, close)) {
                doc.report->errors.push_back("cannot edit <" + tree.nodes[i].tag + "> line " +
                                             std::to_string(tree.nodes[i].line));
                continue;
            }
            if (auto it = doc.assignedIds.find(i); it != doc.assignedIds.end()) {
                doc.edits.push_back(TextEdit{nameEnd, 0, " id=\"" + migrate::escapeAttr(it->second) + "\""});
            }
            if (auto it = doc.newAttrs.find(i); it != doc.newAttrs.end()) {
                size_t at = close;
                while (at > nameEnd &&
                       (doc.text[at - 1] == ' ' || doc.text[at - 1] == '\t' || doc.text[at - 1] == '\n')) {
                    --at;
                }
                for (const TagAttr& a : attrs) {
                    if (a.name.rfind("on:", 0) == 0) {
                        at = a.begin;
                        break;
                    }
                }
                std::string text;
                for (const auto& [name, value] : it->second) {
                    text += " " + name + "=\"" + migrate::escapeAttr(value) + "\"";
                }
                doc.edits.push_back(TextEdit{at, 0, text});
            }
        }
        for (uint32_t def : tree.symbolDefs) {
            const DomNode& node = tree.nodes[def];
            if (node.parent == kNone || node.sourceEnd <= node.sourceBegin) {
                continue;  // A `<head>` definition is not in the body text.
            }
            size_t begin = node.sourceBegin;
            while (begin > 0 && (doc.text[begin - 1] == ' ' || doc.text[begin - 1] == '\t')) {
                --begin;
            }
            if (begin > 0 && doc.text[begin - 1] == '\n') {
                --begin;
            }
            doc.edits.push_back(TextEdit{begin, node.sourceEnd - begin, std::string()});
        }
        for (uint32_t def : tree.symbolDefs) {
            const DomNode& node = tree.nodes[def];
            CanonicalSymbol symbol;
            for (const DomAttr& a : node.attrs) {
                symbol.attrs.emplace_back(a.name, a.value);
            }
            if (auto it = doc.symbolPorts.find(def); it != doc.symbolPorts.end()) {
                symbol.ports = it->second;
            }
            for (uint32_t child : node.children) {
                const DomNode& c = tree.nodes[child];
                const std::string markup =
                    migrate::applyEdits(doc.text, c.sourceBegin, c.sourceEnd, doc.edits);
                symbol.elements.push_back(
                    migrate::reindent(markup, 4 - migrate::indentAt(doc.text, c.sourceBegin)));
            }
            if (auto it = doc.symbolLayout.find(def); it != doc.symbolLayout.end()) {
                symbol.layout.push_back(CanonicalSection{{}, it->second, {}});
            }
            out.symbols.push_back(std::move(symbol));
        }
        if (tree.body != kNone) {
            const DomNode& body = tree.nodes[tree.body];
            const std::string markup =
                migrate::applyEdits(doc.text, body.sourceBegin, body.sourceEnd, doc.edits);
            out.body = migrate::reindent(markup, 2 - migrate::indentAt(doc.text, body.sourceBegin));
        }
        if (!doc.docLayout.empty()) {
            out.layout.push_back(CanonicalSection{{}, std::move(doc.docLayout), {}});
        }
    }

    // ---- self-check (54 §54.11) --------------------------------------------------------------

    void selfCheck(const std::string& path) {
        MigrateFileReport& report = reportFor(path, "ui");
        std::vector<SourceFile> before;
        std::vector<SourceFile> after;
        before.push_back(SourceFile{path, m_files[path].original});
        after.push_back(SourceFile{path, m_files[path].migrated});
        for (const auto& [name, file] : m_files) {
            if (name == path) {
                continue;
            }
            if (!file.created && !file.original.empty()) {
                before.push_back(SourceFile{name, file.original});
            }
            if (file.hasMigrated) {
                after.push_back(SourceFile{name, file.migrated});
            } else if (!file.original.empty()) {
                after.push_back(SourceFile{name, file.original});
            }
        }
        CompileOptions options = m_options.compileOptions ? m_options.compileOptions(path) : CompileOptions{};
        options.emitMeta = false;
        options.xtDir = genericPathToUtf8(pathFromUtf8(path).parent_path());
        if (options.rootDir.empty()) {
            options.rootDir = options.xtDir;
        }
        if (options.standardSymbolDir.empty()) {
            options.standardSymbolDir = m_options.standardSymbolDir;
        }
        const CompileResult v1 = compileDocumentFromStrings(before, path, options);
        if (!v1.ok()) {
            report.checkDetail = "version 1 does not compile: " + firstError(v1.diagnostics);
            return;
        }
        const CompileResult v2 = compileDocumentFromStrings(after, path, options);
        if (!v2.ok()) {
            report.status = "error";
            report.errors.push_back("the migrated document does not compile: " + firstError(v2.diagnostics));
            report.layerTwo = MigrateCheck::Differ;
            return;
        }
        report.layerOne = v1.ir == v2.ir ? MigrateCheck::Equal : MigrateCheck::Differ;
        report.layerOneWithoutSourceMap = chunksWithoutSourceMap(v1.ir) == chunksWithoutSourceMap(v2.ir)
                                              ? MigrateCheck::Equal
                                              : MigrateCheck::Differ;
        std::string dump1;
        std::string dump2;
        std::string error;
        if (!dumpComputedStyle(v1.ir, dump1, error) || !dumpComputedStyle(v2.ir, dump2, error)) {
            report.checkDetail = "computed-style dump failed: " + error;
            report.layerTwo = MigrateCheck::Differ;
            return;
        }
        if (dump1 == dump2) {
            report.layerTwo = MigrateCheck::Equal;
            return;
        }
        report.layerTwo = MigrateCheck::Differ;
        size_t a = 0;
        size_t b = 0;
        for (;;) {
            const size_t ea = dump1.find('\n', a);
            const size_t eb = dump2.find('\n', b);
            const std::string_view la =
                std::string_view(dump1).substr(a, ea == std::string::npos ? std::string::npos : ea - a);
            const std::string_view lb =
                std::string_view(dump2).substr(b, eb == std::string::npos ? std::string::npos : eb - b);
            if (la != lb || ea == std::string::npos || eb == std::string::npos) {
                report.checkDetail =
                    "v1: " + std::string(la.substr(0, 600)) + " | v2: " + std::string(lb.substr(0, 600));
                break;
            }
            a = ea + 1;
            b = eb + 1;
        }
    }

    const MigrateOptions& m_options;
    MigrateReport& m_report;
    std::vector<std::string> m_queue;
    std::set<std::string> m_seen;
    std::set<std::string> m_libraries;  ///< Paths reached through `<link rel="symbols">`.
    std::vector<std::string> m_uiDocs;
    std::map<std::string, MigrateFileReport> m_entries;  ///< Stable references while migrating.
    std::vector<std::string> m_order;
    std::map<std::string, SheetEntry> m_sheets;
    std::map<std::string, FileText> m_files;
};

bool skippedDirectory(const fs::path& dir) {
    const std::string name = pathToUtf8(dir.filename());
    return (!name.empty() && name[0] == '.') || name == "build" || name == "dist" || name == "bin" ||
           name == "node_modules";
}

/// The deepest directory that contains every path of `paths` (generic, absolute).
std::string commonRoot(const std::vector<std::string>& paths) {
    if (paths.empty()) {
        return std::string();
    }
    fs::path root = pathFromUtf8(paths.front()).parent_path();
    for (const std::string& p : paths) {
        const fs::path dir = pathFromUtf8(p).parent_path();
        while (!root.empty()) {
            const fs::path rel = dir.lexically_relative(root);
            if (!rel.empty() && *rel.begin() != "..") {
                break;
            }
            const fs::path up = root.parent_path();
            if (up == root) {
                break;
            }
            root = up;
        }
    }
    return genericPathToUtf8(root);
}

}  // namespace

bool migrateToV2(const std::string& input, const MigrateOptions& options, std::vector<MigratedFile>& out,
                 MigrateReport& report) {
    out.clear();
    report = MigrateReport{};
    const fs::path in = pathFromUtf8(input);
    std::error_code ec;
    std::vector<std::string> inputs;
    std::string inputRoot;
    if (fs::is_directory(in, ec)) {
        inputRoot = normalPath(in);
        for (fs::recursive_directory_iterator it(in, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) {
                if (skippedDirectory(it->path())) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (it->path().extension() == ".xt") {
                inputs.push_back(normalPath(it->path()));
            }
        }
        std::sort(inputs.begin(), inputs.end());
    } else {
        inputs.push_back(normalPath(in));
        inputRoot = genericPathToUtf8(pathFromUtf8(inputs.front()).parent_path());
    }
    Migrator migrator(options, report);
    migrator.run(inputs);
    migrator.collect(out);
    std::vector<std::string> paths{inputRoot + "/x"};
    for (const MigrateFileReport& f : report.files) {
        paths.push_back(f.path);
    }
    for (const MigratedFile& f : out) {
        paths.push_back(f.path);
    }
    report.root = commonRoot(paths);
    return report.ok();
}

}  // namespace xt::compile
