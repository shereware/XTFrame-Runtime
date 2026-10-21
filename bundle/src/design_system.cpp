#include "xt/bundle/design_system.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <utility>

#include "bundle_json.h"
#include "design_internal.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/parse_xts.h"
#include "xt/compile/utf8_path.h"
#include "xt/util/sha256.h"

/// Entry references, the rel vocabulary and the helpers shared by the design system sources
/// (docs/design/35-design-system.md §35.3, docs/tasks/T-305.md).

namespace xt::bundle::design {

namespace {

constexpr std::array<std::string_view, 12> kPrefixes = {"ref",    "text",   "decision", "swatch",
                                                        "group",  "token",  "type",     "convention",
                                                        "motion", "symbol", "ui",       "layer"};

bool isControlOrSpace(char c) {
    const auto u = static_cast<unsigned char>(c);
    return u <= 0x20 || u == 0x7F;
}

}  // namespace

std::string_view relName(Rel rel) { return kRels[static_cast<size_t>(rel)]; }

bool parseRel(std::string_view text, Rel* rel) {
    for (size_t i = 0; i < kRels.size(); ++i) {
        if (kRels[i] == text) {
            if (rel != nullptr) {
                *rel = static_cast<Rel>(i);
            }
            return true;
        }
    }
    return false;
}

std::string_view kindPrefix(EntryKind kind) { return kPrefixes[static_cast<size_t>(kind)]; }

std::string entryRef(EntryKind kind, std::string_view id) {
    std::string out(kindPrefix(kind));
    out += ':';
    out += id;
    return out;
}

bool parseEntryRef(std::string_view ref, EntryKind* kind, std::string* id) {
    const size_t colon = ref.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    const std::string_view prefix = ref.substr(0, colon);
    size_t index = kPrefixes.size();
    for (size_t i = 0; i < kPrefixes.size(); ++i) {
        if (kPrefixes[i] == prefix) {
            index = i;
            break;
        }
    }
    if (index == kPrefixes.size()) {
        return false;
    }
    const auto k = static_cast<EntryKind>(index);
    std::string_view rest = ref.substr(colon + 1);
    if (k == EntryKind::Token && rest.size() >= 2 && rest.substr(0, 2) == "--") {
        rest.remove_prefix(2);
    }
    if (rest.empty()) {
        return false;
    }
    for (char c : rest) {
        if (isControlOrSpace(c)) {
            return false;
        }
    }
    const size_t at = rest.find('@');
    const size_t hash = rest.find('#');
    if (at != std::string_view::npos) {
        // `ref:<group>/<name>@<annotation>` only.
        if (k != EntryKind::Reference || at == 0 || at + 1 == rest.size() ||
            rest.find('@', at + 1) != std::string_view::npos || hash != std::string_view::npos) {
            return false;
        }
    }
    const bool takesAnchor = k == EntryKind::Text || k == EntryKind::Layer || k == EntryKind::TypeRole;
    if (hash != std::string_view::npos) {
        if (!takesAnchor || hash == 0 || hash + 1 == rest.size() ||
            rest.find('#', hash + 1) != std::string_view::npos) {
            return false;
        }
    } else if (k == EntryKind::Layer) {
        return false;  // `layer:<ui>#<id>` always names the element.
    }
    if (rest.front() == '/' || rest.back() == '/' || rest.find("//") != std::string_view::npos) {
        return false;
    }
    if (kind != nullptr) {
        *kind = k;
    }
    if (id != nullptr) {
        *id = std::string(rest);
    }
    return true;
}

std::string designDir(const UiProject& project) {
    const std::string sub = project.shared.design.empty() ? std::string("design") : project.shared.design;
    return bundle::detail::joinPath(project.rootDir, sub);
}

namespace detail {

std::string contentHash(std::string_view bytes) { return "sha256:" + util::sha256Hex(bytes); }

std::string newGuid() {
    std::random_device device;
    std::mt19937_64 engine((static_cast<uint64_t>(device()) << 32) ^ static_cast<uint64_t>(device()));
    unsigned char bytes[16];
    for (int i = 0; i < 16; i += 8) {
        const uint64_t v = engine();
        for (int j = 0; j < 8; ++j) {
            bytes[i + j] = static_cast<unsigned char>((v >> (8 * j)) & 0xFFu);
        }
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u);
    std::string out;
    char buffer[3];
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out += '-';
        }
        std::snprintf(buffer, sizeof(buffer), "%02x", static_cast<unsigned int>(bytes[i]));
        out += buffer;
    }
    return out;
}

std::string headingSlug(std::string_view heading) {
    std::string out;
    bool dash = false;
    for (const char c : heading) {
        const auto u = static_cast<unsigned char>(c);
        if (c >= 'A' && c <= 'Z') {
            if (dash && !out.empty()) {
                out += '-';
            }
            dash = false;
            out += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || u >= 0x80) {
            if (dash && !out.empty()) {
                out += '-';
            }
            dash = false;
            out += c;
        } else {
            dash = true;
        }
    }
    return out;
}

std::vector<std::string> headingSlugs(std::string_view body) {
    std::vector<std::string> out;
    bool fenced = false;
    size_t start = 0;
    while (start < body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        std::string_view line = body.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.substr(0, 3) == "```" || line.substr(0, 3) == "~~~") {
            fenced = !fenced;
            continue;
        }
        if (fenced || line.empty() || line[0] != '#') {
            continue;
        }
        size_t level = 0;
        while (level < line.size() && line[level] == '#') {
            ++level;
        }
        if (level > 6 || level >= line.size() || line[level] != ' ') {
            continue;
        }
        std::string_view text = line.substr(level + 1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '#')) {
            text.remove_suffix(1);
        }
        const std::string slug = headingSlug(text);
        if (!slug.empty()) {
            out.push_back(slug);
        }
    }
    return out;
}

std::string statusText(const Entry& entry) {
    if (entry.extra.is_object()) {
        const auto it = entry.extra.find("status");
        if (it != entry.extra.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    switch (entry.status) {
        case EntryStatus::Approved:
            return "approved";
        case EntryStatus::Deprecated:
            return "deprecated";
        case EntryStatus::Draft:
            break;
    }
    return "draft";
}

std::string normalizeRef(std::string_view ref) {
    if (ref.substr(0, 8) == "token:--") {
        return "token:" + std::string(ref.substr(8));
    }
    return std::string(ref);
}

std::vector<std::string> sharedTokenNames(const UiProject& project) {
    std::vector<std::string> names;
    if (project.shared.tokens.empty()) {
        return names;
    }
    std::string text;
    if (!bundle::detail::readFile(bundle::detail::joinPath(project.rootDir, project.shared.tokens), text)) {
        return names;
    }
    compile::DiagnosticSink sink;
    compile::RuleSet rules;
    compile::parseXts(text, project.shared.tokens, 1, sink, rules);
    std::set<std::string> seen;
    for (const compile::TokenDecl& token : rules.tokens) {
        if (seen.insert(token.name).second) {
            names.push_back(token.name);
        }
    }
    return names;
}

}  // namespace detail

}  // namespace xt::bundle::design
