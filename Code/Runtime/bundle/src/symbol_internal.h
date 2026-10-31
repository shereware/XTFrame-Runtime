#pragma once

#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "bundle_json.h"
#include "xt/bundle/symbol_library.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/style_model.h"
#include "xt/compile/utf8_path.h"
#include "xt/types.h"

/// T-499: the helpers symbol_library.cpp, symbol_lint.cpp and symbol_set.cpp share (Docs/design/
/// 49-symbol-sets.md). Internal to xt_bundle. They were file-local in symbol_library.cpp (T-498).

namespace xt::bundle::detail {

/// 14 §14.3: the last line of defence around a public entry point. Runs `body`; anything it
/// throws becomes kInternal.
template <typename Body>
Status catchAll(Body&& body) {
    try {
        return body();
    } catch (const std::exception&) {
    } catch (...) {
    }
    return Status::kInternal;
}

/// A group directory name: a stable id (`[a-z0-9-]+`) or a kept copy `<id>@<version>[-<seconds>]`
/// (reloadPreset with keepOld), so `.` and `@` are allowed after the id. Never a path: no
/// separator, no `..`, nothing starting with `.`.
inline bool isGroupId(std::string_view id) {
    if (id.empty() || id.size() > 128 || id.front() == '.' || id.front() == '-') {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '@';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// Joins `untrusted`, a relative UTF-8 path read from a group file, under `root` into `out`
/// (the plain join). False when it is empty, not valid UTF-8, absolute, has a root name or a
/// `..` segment, or leaves `root` once normalised (14 §14.9 rule 5; the import layer's
/// resolveInside(), which the runtime cannot include).
inline bool resolveInside(const std::filesystem::path& root, std::string_view untrusted,
                          std::filesystem::path& out) {
    namespace fs = std::filesystem;
    if (untrusted.empty() || untrusted.size() > 1024) {
        return false;
    }
    fs::path relative;
    try {
        relative = compile::pathFromUtf8(untrusted);
    } catch (const std::exception&) {
        return false;
    }
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    for (const fs::path& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    const fs::path joined = root / relative;
    fs::path base = root.lexically_normal();
    if (!base.has_filename() && base.has_relative_path()) {
        base = base.parent_path();
    }
    const fs::path inside = joined.lexically_normal().lexically_relative(base);
    if (inside.empty() || inside == "." || *inside.begin() == "..") {
        return false;
    }
    out = joined;
    return true;
}

/// `text` with CRLF read as LF (a checkout may convert line endings), for content hashes.
inline std::string lfText(const std::string& text) {
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

/// The last '/' separated segment of `relative`.
inline std::string fileNameOf(std::string_view relative) {
    const size_t slash = relative.find_last_of('/');
    return std::string(slash == std::string_view::npos ? relative : relative.substr(slash + 1));
}

/// `text` without leading and trailing space, tab, CR and LF.
inline std::string trimmed(std::string_view text) {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

/// A symbol name usable as a file stem (the useSymbols() rule): no separator, `..`, `.` or `:`
/// (the `xt-standard:` rule).
inline bool isPlainSymbolName(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == '.' || c == ':' || c < 0x20) {
            return false;
        }
    }
    return true;
}

/// Seconds since the epoch, for `.trash/<id>-<seconds>/`, kept-copy suffixes and staging names.
inline std::string nowSeconds() {
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    return std::to_string(seconds.count());
}

/// The group `id` of `library`, or null.
inline const SymbolGroup* groupOf(const SymbolLibrary& library, std::string_view id) {
    for (const SymbolGroup& group : library.groups) {
        if (group.id == id) {
            return &group;
        }
    }
    return nullptr;
}

/// `<root>/<relative>` as a project-relative '/' path (what UseReport lists).
inline std::string relativeText(const std::filesystem::path& root, const std::filesystem::path& path) {
    return compile::genericPathToUtf8(path.lexically_normal().lexically_relative(root.lexically_normal()));
}

/// The tokens and `@font-face` blocks of one `.xts` token file.
struct TokenSheet {
    /// False when the file could not be read.
    bool ok = false;
    /// `--name` -> value text; the first declaration of a name wins.
    std::map<std::string, std::string> values;
    /// font family -> `src` files (relative to the file), in file order.
    std::map<std::string, std::vector<std::string>> faces;
};

/// Parses `file` as a stylesheet without diagnostics.
inline TokenSheet readTokenSheet(const std::filesystem::path& file) {
    TokenSheet sheet;
    std::string text;
    const std::string utf8 = compile::genericPathToUtf8(file);
    if (!readFile(utf8, text)) {
        return sheet;
    }
    compile::RuleSet rules;
    compile::DiagnosticSink sink;
    compile::parseXts(text, utf8, 1, sink, rules);
    sheet.ok = true;
    for (const compile::TokenDecl& token : rules.tokens) {
        sheet.values.emplace(token.name, trimmed(token.value));
    }
    for (const compile::FontFaceDef& face : rules.fontFaces) {
        sheet.faces[face.family].push_back(face.src);
    }
    return sheet;
}

}  // namespace xt::bundle::detail

/// T-501: two helpers of symbol_lint.cpp that symbol_group_ops.cpp (saveAsGroup) reuses, so the
/// index it writes lists exactly what the lint checks.
namespace xt::bundle::detail::lint {

/// The sorted set of `--name` custom properties `text` reads through `var(--name)`, without the
/// dashes.
std::vector<std::string> varNames(const std::string& text);

/// The fonts and textures a symbol reading the tokens `names` (without `--`) needs: for every sheet
/// in order, the `@font-face src` of each family a `--font-*` token names and the `url()` of each
/// `--tex-*` token (paths as the sheets write them).
void dependencies(const std::vector<std::string>& names, const std::vector<TokenSheet>& sheets,
                  std::vector<std::string>& fonts, std::vector<std::string>& textures);

}  // namespace xt::bundle::detail::lint
