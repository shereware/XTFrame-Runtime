#include <algorithm>
#include <filesystem>
#include <json.hpp>
#include <set>
#include <string>
#include <vector>

#include "bundle_json.h"
#include "design_internal.h"
#include "xt/bundle/ui_project.h"
#include "xt/compile/utf8_path.h"

/// diffStandardSymbols() (Docs/tasks/T-408.md item 3, Docs/design/43-compatibility-and-versions.md
/// §43.6): the project holds copies of the standard symbols; an upgrade is offered as a diff, never
/// written over the copy.

namespace xt::bundle {

namespace fs = std::filesystem;
using compile::genericPathToUtf8;
using compile::pathFromUtf8;

namespace {

/// `text` with every CRLF read as LF (a checkout may convert line endings).
std::string lfTextSymbolSync(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out += text[i];
    }
    return out;
}

std::vector<std::string> splitLinesSymbolSync(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

/// A unified line diff of `a` -> `b` (LCS), `context` lines around each change; "" when the texts
/// are equal or either has more than kMaxLines lines.
std::string unifiedDiffSymbolSync(const std::string& a, const std::string& b, const std::string& labelA,
                                  const std::string& labelB) {
    constexpr size_t kMaxLines = 4000;
    constexpr size_t kContext = 2;
    const std::vector<std::string> x = splitLinesSymbolSync(a);
    const std::vector<std::string> y = splitLinesSymbolSync(b);
    if (a == b || x.size() > kMaxLines || y.size() > kMaxLines) {
        return std::string();
    }
    const size_t n = x.size();
    const size_t m = y.size();
    // lcs[i][j]: the LCS length of x[i..] and y[j..].
    std::vector<uint32_t> lcs((n + 1) * (m + 1), 0);
    const auto at = [&](size_t i, size_t j) -> uint32_t& { return lcs[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            at(i, j) = x[i] == y[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    // The edit script: ' ' keep, '-' x only, '+' y only, with each line's indices.
    struct Step {
        char op;
        size_t i, j;
    };
    std::vector<Step> steps;
    size_t i = 0;
    size_t j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && x[i] == y[j]) {
            steps.push_back({' ', i++, j++});
        } else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) {
            steps.push_back({'-', i++, j});  // Removals before insertions, as diff -u prints them.
        } else {
            steps.push_back({'+', i, j++});
        }
    }
    std::string out = "--- " + labelA + "\n+++ " + labelB + "\n";
    size_t k = 0;
    while (k < steps.size()) {
        if (steps[k].op == ' ') {
            ++k;
            continue;
        }
        // A hunk: from kContext keeps before the change to kContext keeps after the last change
        // that is no more than 2 * kContext keeps away.
        size_t begin = k;
        for (size_t c = 0; c < kContext && begin > 0 && steps[begin - 1].op == ' '; ++c) {
            --begin;
        }
        size_t end = k;
        size_t keeps = 0;
        for (size_t s = k; s < steps.size(); ++s) {
            if (steps[s].op == ' ') {
                if (++keeps > 2 * kContext) {
                    break;
                }
            } else {
                keeps = 0;
                end = s;
            }
        }
        size_t stop = end + 1;
        for (size_t c = 0; c < kContext && stop < steps.size() && steps[stop].op == ' '; ++c) {
            ++stop;
        }
        size_t countA = 0;
        size_t countB = 0;
        for (size_t s = begin; s < stop; ++s) {
            countA += steps[s].op != '+' ? size_t{1} : size_t{0};
            countB += steps[s].op != '-' ? size_t{1} : size_t{0};
        }
        const size_t startA = countA == 0 ? steps[begin].i : steps[begin].i + 1;
        const size_t startB = countB == 0 ? steps[begin].j : steps[begin].j + 1;
        out += "@@ -" + std::to_string(startA) + "," + std::to_string(countA) + " +" +
               std::to_string(startB) + "," + std::to_string(countB) + " @@\n";
        for (size_t s = begin; s < stop; ++s) {
            out += steps[s].op;
            out += steps[s].op == '+' ? y[steps[s].j] : x[steps[s].i];
            out += '\n';
        }
        k = stop;
    }
    return out;
}

}  // namespace

Status diffStandardSymbols(const UiProject& project, std::string_view standardSymbolDir,
                           std::vector<StandardSymbolDiff>& out) {
    out.clear();
    const fs::path library = pathFromUtf8(standardSymbolDir);
    std::string indexText;
    if (standardSymbolDir.empty() ||
        !detail::readFile(genericPathToUtf8(library / "index.json"), indexText)) {
        return Status::kNotFound;
    }
    const nlohmann::json index = nlohmann::json::parse(indexText, nullptr, false);
    if (!index.is_object() || !index.contains("symbols") || !index["symbols"].is_array()) {
        return Status::kNotFound;
    }
    if (project.shared.symbols.empty()) {
        return Status::kOk;
    }
    std::error_code ec;
    const fs::path root = fs::absolute(pathFromUtf8(project.rootDir), ec).lexically_normal();
    const fs::path symbolsDir = (root / pathFromUtf8(project.shared.symbols)).lexically_normal();
    std::set<std::string> seen;  // A copy is compared once (the root entry wins over a subset's).
    for (const nlohmann::json& entry : index["symbols"]) {
        if (!entry.is_object() || !entry.contains("file") || !entry["file"].is_string()) {
            continue;
        }
        const std::string standard = entry["file"].get<std::string>();
        const fs::path standardPath = library / pathFromUtf8(standard);
        const fs::path copyPath = symbolsDir / standardPath.filename();
        const std::string copyName = genericPathToUtf8(standardPath.filename());
        std::string copyText;
        std::string standardText;
        if (seen.count(copyName) != 0 || !fs::is_regular_file(copyPath, ec) ||
            !detail::readFile(genericPathToUtf8(copyPath), copyText) ||
            !detail::readFile(genericPathToUtf8(standardPath), standardText)) {
            continue;
        }
        seen.insert(copyName);
        copyText = lfTextSymbolSync(copyText);
        standardText = lfTextSymbolSync(standardText);
        StandardSymbolDiff row;
        row.name = entry.value("name", std::string());
        row.file = genericPathToUtf8(copyPath.lexically_relative(root));
        row.standard = standard;
        if (copyText == standardText) {
            row.status = "current";
        } else {
            const std::string hash = design::detail::contentHash(copyText);
            bool earlier = false;
            if (entry.contains("previousContent") && entry["previousContent"].is_array()) {
                for (const nlohmann::json& previous : entry["previousContent"]) {
                    earlier = earlier || (previous.is_string() && previous.get<std::string>() == hash);
                }
            }
            row.status = earlier ? "outdated" : "modified";
            row.diff = unifiedDiffSymbolSync(standardText, copyText, "standard/" + standard, row.file);
        }
        out.push_back(std::move(row));
    }
    std::sort(out.begin(), out.end(),
              [](const StandardSymbolDiff& a, const StandardSymbolDiff& b) { return a.file < b.file; });
    return Status::kOk;
}

}  // namespace xt::bundle
