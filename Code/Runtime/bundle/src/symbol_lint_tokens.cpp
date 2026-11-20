#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "style_internal.h"
#include "symbol_internal.h"
#include "xt/bundle/project_style.h"
#include "xt/bundle/symbol_lint.h"
#include "xt/compile/utf8_path.h"

/// T-743 (Docs/design/47-symbol-visual-style.md "共同詞彙清單" rules 1-3; 56 §56.2 item 4): the token
/// vocabulary findings of the symbol lint -- `token.core` and `token.role`.

namespace xt::bundle::detail::lint {

namespace fs = std::filesystem;
using compile::pathFromUtf8;

namespace {

/// A colour value written as a literal or a colour function (not through var()).
bool looksLikeColourLint(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    if (value[0] == '#') {
        return true;
    }
    for (const char* fn : {"rgb(", "rgba(", "hsl(", "hsla(", "color-mix("}) {
        if (value.rfind(fn, 0) == 0) {
            return true;
        }
    }
    return false;
}

/// The first family of a family list, unquoted.
std::string firstFamilyLint(const std::string& value) {
    std::string item = trimmed(std::string_view(value).substr(0, value.find(',')));
    if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'') && item.back() == item.front()) {
        item = item.substr(1, item.size() - 2);
    }
    return item;
}

}  // namespace

std::vector<LintIssue> tokenVocabularyIssues(const TokenSheet& base, const std::vector<TokenSheet>& themes,
                                             const std::string& presetGroupDir) {
    std::vector<LintIssue> out;
    if (!base.ok) {
        return out;  // group.tokens says so.
    }
    // Rule 1: every core token, of the right type.
    for (const CoreToken& core : coreTokens()) {
        const auto it = base.values.find(core.name);
        if (it == base.values.end()) {
            out.push_back(
                LintIssue{LintLevel::Error, "", "token.core",
                          std::string("missing core token ") + core.name + " (47, core vocabulary)"});
        } else if (!coreTokenValueFits(core.type, it->second)) {
            out.push_back(LintIssue{
                LintLevel::Error, "", "token.core",
                std::string(core.name) + ": '" + it->second + "' is not a " + coreTokenTypeName(core.type)});
        }
    }
    // Rule 3: an extension texture or font token the preset group also has, naming another file / family.
    TokenSheet preset;
    if (!presetGroupDir.empty()) {
        SymbolGroup group;
        fs::path file;
        if (loadGroup(pathFromUtf8(presetGroupDir), group) == Status::kOk && !group.tokensFile.empty() &&
            resolveInside(pathFromUtf8(presetGroupDir), group.tokensFile, file)) {
            preset = readTokenSheet(file);
        }
    }
    for (const auto& [name, value] : base.values) {
        if (findCoreToken(name) != nullptr) {
            continue;
        }
        if (preset.ok && (name.rfind("--tex-", 0) == 0 || name.rfind("--font-", 0) == 0)) {
            const auto theirs = preset.values.find(name);
            if (theirs != preset.values.end()) {
                const bool texture = name.rfind("--tex-", 0) == 0;
                const std::string mine = texture ? fileNameOf(style::urlPath(value)) : firstFamilyLint(value);
                const std::string other =
                    texture ? fileNameOf(style::urlPath(theirs->second)) : firstFamilyLint(theirs->second);
                if (mine != other) {
                    out.push_back(LintIssue{
                        LintLevel::Warning, "", "token.role",
                        name + " names '" + mine + "' but the preset group's " + name + " names '" + other +
                            "': a different role under the same name (47 rule 3; prefix it "
                            "with the library's short name)"});
                }
            }
        }
        // Rule 2: an extension colour follows a style change only through core tokens or the themes.
        if (name.rfind("--color-", 0) != 0 && !looksLikeColourLint(value)) {
            continue;
        }
        const std::vector<std::string> refs = style::varRefs(value);
        const bool viaCore =
            !refs.empty() && std::all_of(refs.begin(), refs.end(), [](const std::string& ref) {
                return findCoreToken(ref) != nullptr;
            });
        const bool everyTheme =
            !themes.empty() && std::all_of(themes.begin(), themes.end(), [&](const TokenSheet& t) {
                return t.ok && t.values.count(name) != 0;
            });
        if (!viaCore && !everyTheme) {
            out.push_back(LintIssue{LintLevel::Warning, "", "token.role",
                                    name +
                                        ": an extension colour that is neither written with core tokens "
                                        "(var(--color-...)) nor set in every theme file; it will not follow "
                                        "a style change (47 rule 2)"});
        }
    }
    return out;
}

}  // namespace xt::bundle::detail::lint
