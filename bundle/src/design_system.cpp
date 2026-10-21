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

namespace {

// --- SHA-256 (FIPS 180-4) ---

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void sha256Block(uint32_t h[8], const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) | (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(p[4 * i + 2]) << 8) | static_cast<uint32_t>(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // namespace

std::string contentHash(std::string_view bytes) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t size = bytes.size();
    size_t offset = 0;
    for (; offset + 64 <= size; offset += 64) {
        sha256Block(h, data + offset);
    }
    unsigned char tail[128] = {};
    const size_t rest = size - offset;
    for (size_t i = 0; i < rest; ++i) {
        tail[i] = data[offset + i];
    }
    tail[rest] = 0x80;
    const size_t tailSize = rest + 9 <= 64 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(size) * 8u;
    for (int i = 0; i < 8; ++i) {
        tail[tailSize - 1 - static_cast<size_t>(i)] = static_cast<unsigned char>((bits >> (8 * i)) & 0xFFu);
    }
    sha256Block(h, tail);
    if (tailSize == 128) {
        sha256Block(h, tail + 64);
    }
    std::string out = "sha256:";
    char buffer[9];
    for (uint32_t word : h) {
        std::snprintf(buffer, sizeof(buffer), "%08x", static_cast<unsigned int>(word));
        out += buffer;
    }
    return out;
}

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
