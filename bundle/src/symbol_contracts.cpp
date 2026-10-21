#include "symbol_contracts.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bundle_json.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/parse_xt.h"
#include "xt/compile/utf8_path.h"

/// Symbol contract summaries (T-200; docs/design/22-bundle.md §22.10 `symbols`, 26 §26.2): read
/// from `<symbol name requires state-groups>` definitions and what their bodies declare.

namespace xt::bundle {

using compile::genericPathToUtf8;
using compile::pathFromUtf8;
using compile::pathToUtf8;

namespace {

namespace fs = std::filesystem;

std::string trim(std::string_view s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string_view::npos) {
        return std::string();
    }
    const size_t b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

/// `text` split at `sep`, each piece trimmed, empty pieces dropped.
std::vector<std::string> splitTrim(std::string_view text, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == sep) {
            std::string piece = trim(text.substr(start, i - start));
            if (!piece.empty()) {
                out.push_back(std::move(piece));
            }
            start = i + 1;
        }
    }
    return out;
}

/// The body of `def` (a `<symbol>`), nested `<symbol>` definitions excluded.
void scanBody(const compile::DomTree& tree, uint32_t node, SymbolContract& out) {
    for (uint32_t child : tree.nodes[node].children) {
        const compile::DomNode& n = tree.nodes[child];
        if (n.tag == "symbol") {
            continue;
        }
        if (n.tag == "slot") {
            if (const compile::DomAttr* name = n.attr("name"); name != nullptr && !name->value.empty()) {
                if (std::find(out.slotNames.begin(), out.slotNames.end(), name->value) ==
                    out.slotNames.end()) {
                    out.slotNames.push_back(name->value);
                }
            }
        }
        for (const compile::DomAttr& attr : n.attrs) {
            if (attr.name.rfind("port:", 0) != 0 || attr.name.size() <= 5) {
                continue;
            }
            // `<type> [in|out|call]`, as the compiler reads it (a trailing direction word).
            SymbolPortInfo port;
            port.name = attr.name.substr(5);
            std::string text = trim(attr.value);
            port.dir = "in";
            const size_t space = text.find_last_of(" \t");
            if (space != std::string::npos) {
                const std::string last = trim(std::string_view(text).substr(space));
                if (last == "in" || last == "out" || last == "call") {
                    port.dir = last;
                    text = trim(std::string_view(text).substr(0, space));
                }
            }
            port.type = text;
            out.ports.push_back(std::move(port));
        }
        scanBody(tree, child, out);
    }
}

}  // namespace

std::vector<SymbolContract> readSymbolContracts(std::string_view xtSource, std::string_view sourceName) {
    std::vector<SymbolContract> out;
    compile::DiagnosticSink sink;
    compile::DomTree tree;
    // T-319: a symbol file (a standard or a new symbol) has no `<body>`, only a `<head>` and
    // top-level definitions; the symbol library parse takes both kinds of file and collects every
    // `<symbol>` definition wherever it sits.
    if (!compile::parseXtSymbolLibrary(xtSource, sourceName, sink, tree)) {
        return out;
    }
    for (uint32_t index : tree.symbolDefs) {
        const compile::DomNode& def = tree.nodes[index];
        const compile::DomAttr* name = def.attr("name");
        if (name == nullptr || name->value.empty()) {
            continue;
        }
        SymbolContract contract;
        contract.name = name->value;
        contract.source = std::string(sourceName);
        if (const compile::DomAttr* req = def.attr("requires")) {
            for (const std::string& section : splitTrim(req->value, ';')) {
                const size_t colon = section.find(':');
                const std::string key = trim(std::string_view(section).substr(0, colon));
                const std::vector<std::string> items =
                    colon == std::string::npos ? std::vector<std::string>()
                                               : splitTrim(std::string_view(section).substr(colon + 1), ',');
                std::vector<std::string>& to =
                    key == "states" ? contract.requiredStates : contract.requiredSlots;
                if (key == "states" || key == "slots") {
                    to.insert(to.end(), items.begin(), items.end());
                }
            }
        }
        if (const compile::DomAttr* groups = def.attr("state-groups")) {
            for (const std::string& group : splitTrim(groups->value, ';')) {
                const size_t colon = group.find(':');
                SymbolStateGroup g;
                g.name = colon == std::string::npos ? std::string()
                                                    : trim(std::string_view(group).substr(0, colon));
                g.states = splitTrim(colon == std::string::npos ? std::string_view(group)
                                                                : std::string_view(group).substr(colon + 1),
                                     '|');
                contract.stateGroups.push_back(std::move(g));
            }
        }
        scanBody(tree, index, contract);
        out.push_back(std::move(contract));
    }
    return out;
}

std::string symbolContractSummary(const SymbolContract& contract) {
    auto join = [](const std::vector<std::string>& items, const char* sep) {
        std::string s;
        for (const std::string& item : items) {
            s += (s.empty() ? "" : sep) + item;
        }
        return s;
    };
    std::vector<std::string> parts;
    if (!contract.requiredStates.empty()) {
        parts.push_back("states: " + join(contract.requiredStates, ", "));
    }
    if (!contract.requiredSlots.empty()) {
        parts.push_back("slots: " + join(contract.requiredSlots, ", "));
    }
    for (const SymbolStateGroup& g : contract.stateGroups) {
        parts.push_back("group" + (g.name.empty() ? std::string() : " " + g.name) + ": " +
                        join(g.states, "|"));
    }
    if (!contract.ports.empty()) {
        std::vector<std::string> names;
        for (const SymbolPortInfo& p : contract.ports) {
            names.push_back(p.name + " " + p.type + (p.dir == "in" ? "" : " " + p.dir));
        }
        parts.push_back("ports: " + join(names, ", "));
    }
    return join(parts, "; ");
}

std::vector<SymbolContract> collectSymbolContracts(const std::string& rootDir, const std::string& symbolsDir,
                                                   const std::vector<std::string>& docs) {
    std::vector<SymbolContract> out;
    auto add = [&](const fs::path& file) {
        std::string text;
        if (!detail::readFile(pathToUtf8(file), text)) {
            return;
        }
        std::error_code ec;
        std::string rel = genericPathToUtf8(fs::relative(file, pathFromUtf8(rootDir), ec));
        if (ec || rel.empty()) {
            rel = genericPathToUtf8(file);
        }
        for (SymbolContract& c : readSymbolContracts(text, rel)) {
            const bool known = std::any_of(out.begin(), out.end(),
                                           [&c](const SymbolContract& o) { return o.name == c.name; });
            if (!known) {
                out.push_back(std::move(c));
            }
        }
    };
    // The symbol library first (sorted by file name, so the manifest does not depend on the
    // directory order), then the UI documents' own definitions.
    std::error_code ec;
    const fs::path dir = pathFromUtf8(rootDir) / pathFromUtf8(symbolsDir);
    if (!symbolsDir.empty() && fs::is_directory(dir, ec)) {
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".xt") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            add(file);
        }
    }
    for (const std::string& doc : docs) {
        add(pathFromUtf8(rootDir) / pathFromUtf8(doc));
    }
    return out;
}

}  // namespace xt::bundle
