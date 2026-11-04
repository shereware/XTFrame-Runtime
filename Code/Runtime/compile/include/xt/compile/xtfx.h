#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/style_model.h"  // EffectDeclEntry (the header's `fallback:`).
#include "xt/effect_utils.h"         // EffectSlotName / effectSlotParamIndex (T-534).

/// `.xtfx` custom effect assets (Docs/design/18-effects.md §18.7, Docs/tasks/T-203.md): the
/// header (`name:`, `param`, `fallback:`, `expand:`, `---`), the HLSL-subset body parser and the
/// GLSL 330 translation. Everything here is compile-time: `xtframe-compile` runs it when a
/// stylesheet uses `custom("<asset>")`, the editor runs the header part to build the effect
/// panel's parameter UI.

namespace xt::compile {

/// `param <name> <float|vec2|color> <default> [<min>, <max>]`.
struct XtfxParam {
    std::string name;
    /// 0 Float, 1 Vec2, 2 Color (ShaderParamDesc::type, Docs/design/18-effects.md §18.5).
    uint8_t type = 0;
    /// Default: [0] for float, [0..1] for vec2, RGBA for color.
    float def[4] = {0, 0, 0, 0};
    /// `[min, max]`; both 0 with `hasRange == false` when the header gives none.
    float min = 0;
    float max = 0;
    bool hasRange = false;
    /// Where the value lives in the effect's EffectInstance (xt::customParamSlots(),
    /// command_list.h): `params[slot]` (Float; Vec2 also `params[slot + 1]`) or `colors[slot]`.
    uint8_t slot = 0;
};

/// A parse/translation error with its 1-based position in the `.xtfx` text.
struct XtfxError {
    std::string message;
    int line = 0;
    int col = 0;
};

/// The header block above `---`.
struct XtfxHeader {
    /// `name:` (may be empty; the compiler then uses the asset file's stem).
    std::string name;
    std::vector<XtfxParam> params;
    /// `fallback:` parsed as one 18.4 effect function; `kind == None` for `none` or when the
    /// line is absent.
    EffectDeclEntry fallback;
    /// `expand:` in local px (0 when absent).
    float expandPx = 0;
    /// T-532 (18.7): `blurb:`, the effect catalog's one-line description (trimmed, UTF-8; empty
    /// when absent; the last line wins). Read by the editor only: not in the IR or `ShaderRef`.
    std::string blurb;
    /// Non-fatal header problems (a repeated `blurb:`, reported once), in line order.
    std::vector<XtfxError> warnings;
};

/// `compileXtfx` output.
struct XtfxResult {
    bool ok = false;
    XtfxHeader header;
    /// The HLSL body exactly as written (everything after the `---` line), no trailing NUL.
    std::string source;
    /// The translated GLSL 330 module: the parameter uniforms (same names, `float`/`vec2`/`vec4`),
    /// `uniform sampler2D uLayer`, `uniform float xtTime`, `uniform vec2 uTexelSize`/`uLayerSize`,
    /// `struct XtInput`, the `xtSample`/`xtFmod` helpers and `vec4 xtMain(XtInput i)`. No
    /// `#version`, no varyings and no `main()`: the GL backend wraps it and defines
    /// `vec2 xtTexCoord(vec2 uv)` (Code/Runtime/gl/src/gl_effects.cpp).
    std::string glsl;
    XtfxError error;
};

/// Parses the header lines up to and including `---`. On success `out` is filled, `body` holds
/// the rest of the text and `bodyLine` its first 1-based line number. Every problem (unknown key,
/// bad param line, duplicate param, missing `---`, a `fallback:` that is not one built-in effect
/// function or `none`, a `custom()` fallback) is an error with the offending line.
bool parseXtfxHeader(std::string_view text, XtfxHeader& out, std::string& body, int& bodyLine,
                     XtfxError& err);

/// Parses `body` against Docs/design/18-effects.md §18.7's HLSL subset and writes the GLSL
/// module (see `XtfxResult::glsl`) to `glsl`. `params` become read-only globals; the body must
/// define exactly `float4 xtMain(XtInput <name>)` and nothing else. `firstLine` offsets the error
/// positions to the enclosing file. Rejected (each with line and column): types outside
/// `float, float2, float3, float4, int, bool`; `struct`; arrays; `Texture2D`/`SamplerState`/
/// `.Sample`; `discard`; `while`/`do`/`switch`/`break`/`continue`; the ternary operator; any
/// function other than `xtMain` (so no recursion); global declarations (no mutable global state);
/// assignment to a parameter or `xtTime`; a `for` whose bounds are not integer constants or
/// which runs more than 64 times; unknown identifiers and functions; type mismatches.
bool translateHlslSubset(std::string_view body, int firstLine, std::span<const XtfxParam> params,
                         std::string& glsl, XtfxError& err);

/// `parseXtfxHeader` + `translateHlslSubset` over a whole `.xtfx` text.
XtfxResult compileXtfx(std::string_view text);

/// The `effectParam()` paramIndex (Docs/design/18-effects.md §18.2, T-534 encoding) that
/// addresses parameter `name` of a custom effect (`effect.<fx>.<name>`, 18.13): a Float ->
/// `kEffectParamFloatBase + slot`, a Color -> `kEffectParamColorBase + slot`, a Vec2 as a whole ->
/// `kEffectParamVec2Base + slot` (a Vec2 track, any slot). `<name>.x` / `<name>.y` -> the Float
/// `kEffectParamFloatBase + slot` / `+ slot + 1` of a Vec2. `enabled` -> 0. False for an unknown
/// name or a component on a Float / Color. Slot names (`p<n>` etc.) are not handled here: see
/// effectSlotParamIndex().
bool xtfxParamIndex(std::span<const XtfxParam> params, std::string_view name, uint32_t& paramIndex);

/// T-534 (18.13): slot addressing lives in xt_core (xt/effect_utils.h) so the flow runtime shares
/// it (cloud review 2026-10-31 §2.7).
using xt::EffectSlotName;
using xt::effectSlotParamIndex;

/// Writes the header defaults of `params` into an effect's `params[16]`/`colors[4]` (RGBA floats,
/// `colors` as 4 floats each) per their slots.
void applyXtfxDefaults(std::span<const XtfxParam> params, float outParams[16], float outColors[4][4]);

}  // namespace xt::compile
