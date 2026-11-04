#include "xt/text/missing_fonts.h"

#include <algorithm>
#include <cstdint>
#include <span>

#include "font_name_table.h"
#include "xt/document.h"
#include "xt/ir.h"
#include "xt/node_arena.h"

namespace xt::text {

namespace {

std::string lowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

}  // namespace

std::vector<std::string> missingCjkFamilies(const Document& doc, const TextShaper& shaper) {
    std::vector<std::string> missing;
    std::vector<FontDesc> fonts;
    shaper.fonts(fonts);
    if (fonts.empty()) return missing;
    // Lowercased family -> covers CJK.
    std::vector<std::pair<std::string, bool>> installed;
    for (const FontDesc& font : fonts) {
        const std::string lower = lowerAscii(font.family);
        auto it =
            std::find_if(installed.begin(), installed.end(), [&](const auto& e) { return e.first == lower; });
        if (it == installed.end()) {
            installed.emplace_back(lower, font.cjk);
        } else {
            it->second = it->second || font.cjk;
        }
    }
    const auto presence = [&](std::string_view family) {
        const std::string lower = lowerAscii(family);
        for (const auto& entry : installed) {
            if (entry.first == lower)
                return entry.second ? FamilyPresence::kInstalledCjk : FamilyPresence::kInstalled;
        }
        return FamilyPresence::kAbsent;
    };
    const IrView& view = doc.ir();
    const std::span<const uint32_t> pool = view.style().fontFamilies();
    const NodeArena& arena = doc.arena();
    for (NodeId node = 0; node < arena.capacity(); ++node) {
        if (!arena.alive(node) || arena.kind(node) != NodeKind::Text) continue;
        const std::string_view text = doc.string(arena.text(node).text);
        const bool visible = std::any_of(text.begin(), text.end(),
                                         [](char c) { return static_cast<unsigned char>(c) > 0x20; });
        const NodeId parent = arena.parent(node);
        if (!visible || parent == kInvalidNode || arena.kind(parent) != NodeKind::Element) continue;
        const ComputedStyle& style = arena.element(parent).style;
        std::vector<std::string> stack;
        for (uint32_t i = 0; i < style.fontFamilyCount; ++i) {
            const uint32_t index = style.fontFamilyOffset + i;
            if (index < pool.size()) stack.emplace_back(view.string(pool[index]));
        }
        std::string family = firstMissingCjkFamily(stack, presence);
        if (!family.empty() && std::find(missing.begin(), missing.end(), family) == missing.end()) {
            missing.push_back(std::move(family));
        }
    }
    return missing;
}

}  // namespace xt::text
