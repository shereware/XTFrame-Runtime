#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/value.h"

/// T-477 (Docs/design/27-interaction.md §27.4.2 item 4): a setStyle() declaration text whose
/// numbers are holes -- `transform: translate(<n>px, <n>px)`, `width: <n>%`, `opacity: <n>` --
/// compiled once into the declarations it parses to, so that a caller writing the completed text
/// every frame (a flow `format -> setStyle` edge) can hand it to ElementRef::setStyleTemplate(),
/// which takes the numbers out of the text and writes the declarations without the CSS parse.
/// The result is the one ElementRef::setStyle() would produce for the same text; a text that does
/// not have the template's shape is left to setStyle().

namespace xt {

/// A compiled template (compileStyleTemplate()). Opaque to callers: they keep it and pass it back.
struct StyleTemplate {
    /// One parsed declaration, its hole fields still holding the probe value.
    struct Decl {
        uint16_t prop = 0;
        uint8_t important = 0;
        Value value{};
    };
    /// Where hole `hole`'s number goes: float `field` of decls[decl].value (Float: f; Length:
    /// len.value; Vec2 / Vec3: x, y, z; Color: r, g, b, a).
    struct Target {
        uint16_t decl = 0;
        uint8_t field = 0;
        uint8_t hole = 0;
    };
    /// Hole numbers that keep the parse shape besides positive ones (kHoleNegative, kHoleZero).
    static constexpr uint8_t kHoleNegative = 1;
    static constexpr uint8_t kHoleZero = 2;
    /// The most holes a template has.
    static constexpr size_t kMaxHoles = 16;

    /// The literal text around the holes: literals[i] comes before hole i, the last one after the
    /// last hole (holes + 1 entries).
    std::vector<std::string> literals;
    std::vector<Decl> decls;
    std::vector<Target> targets;
    /// Per hole: kHoleNegative / kHoleZero when such a number parses like a positive one.
    std::vector<uint8_t> holeDomain;

    size_t holeCount() const { return literals.empty() ? 0 : literals.size() - 1; }
    /// False for a default-constructed or failed template.
    bool valid() const { return !literals.empty() && !decls.empty(); }
};

/// Compiles the declaration text `literals[0] <n> literals[1] <n> ... literals[k]` (k holes,
/// k <= StyleTemplate::kMaxHoles, each hole one CSS number). Succeeds only when every completion
/// with numbers parses (Code/Runtime/core/src/mini_css) without an error into the same plain
/// declarations -- no `var()`, no names to intern, no `effect` / `background-gradient` /
/// `clip-path` / `animation` -- each hole's number landing unchanged in one or more float fields
/// (a length keeps its unit: `px` is still multiplied by the UI scale when the style resolves,
/// 05 §5.9.9), and the characters around each hole cannot merge with a number. A hole whose
/// negative or zero numbers change the parse is restricted to positive ones (holeDomain). On
/// failure `out` is cleared and false is returned: the caller keeps using setStyle().
bool compileStyleTemplate(std::span<const std::string_view> literals, StyleTemplate& out);

}  // namespace xt
