#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// T-892 (Docs/design/18-effects.md §18.7): the one list of the identifiers a back end's wrapper
/// puts around a translated `.xtfx` module -- the names the translation itself emits
/// (Code/Runtime/compile/src/xtfx/glsl_emit.cpp), the GL wrapper's (Code/Runtime/gl/src/gl_effects.cpp
/// `wrapCustomModule` and the uniforms of gl_shaders.h `kQuadVert`, which links with it) and the
/// wgpu wrapper's (Code/Runtime/wgpu/src/wgpu_custom.cpp `wrapCustomModule`). A `.xtfx` parameter or
/// local variable spelled like one of them is a compile error ("... is reserved in the translation"),
/// so the module keeps working on every back end. It lives in xt_core because xt_compile checks
/// against it and the back ends' tests hold their wrapper text against it; xt_compile does not
/// depend on a GPU back end.
///
/// Rule for a wrapper: every identifier it declares in the fragment module that starts with `xt` or
/// `Xt` must be in kXtfxWrapperNames (Tests/gl and Tests/wgpu check this with
/// xtfxUnlistedWrapperNames); a name without that prefix goes in too when it is declared at the
/// module's global scope or in its `main`. A host's own wrapper (HLSL, Docs/design/23 upgrade notes)
/// should use names from this list or names that start with `xt` and are added here.

namespace xt {

/// The wrappers' and the translation's own names. The first group is also what the author may use
/// but not redefine (kXtfxBuiltinNames).
inline constexpr std::string_view kXtfxWrapperNames[] = {
    // Built in, visible to the author (§18.7 fixed interface).
    "xtTime",
    "xtSample",
    "xtMain",
    "XtInput",
    // The translation's module (glsl_emit.cpp).
    "uLayer",
    "uTexelSize",
    "uLayerSize",
    "xtFmod",
    "xtTexCoord",
    // The GL wrapper (gl_effects.cpp) and the vertex shader it links with (gl_shaders.h kQuadVert).
    "vUV",
    "fragColor",
    "uNdc",
    "xtIn",
    "xtPx",
    "xtLocalRowX",  // T-818
    "xtLocalRowY",
    // The wgpu wrapper (wgpu_custom.cpp): the std140 block, the texture and sampler behind the
    // `uLayer` macro, and the Vulkan-style GLSL type names it uses.
    "XtCustomBlock",
    "xtU",
    "xtLayerTex",
    "xtLayerSmp",
    "texture2D",
    "sampler",
};

/// The names of §18.7's fixed interface: usable in a body, never declared by it (a `.xtfx` header
/// `param` with one of these names is the header error "is reserved").
inline constexpr std::string_view kXtfxBuiltinNames[] = {"xtTime", "xtSample", "xtMain", "XtInput"};

/// True when `name` is in kXtfxWrapperNames.
inline bool isXtfxWrapperName(std::string_view name) {
    for (std::string_view n : kXtfxWrapperNames) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

/// True when `name` is in kXtfxBuiltinNames.
inline bool isXtfxBuiltinName(std::string_view name) {
    for (std::string_view n : kXtfxBuiltinNames) {
        if (n == name) {
            return true;
        }
    }
    return false;
}

/// The identifiers of the GLSL text `code` that start with `xt` or `Xt` and are not in
/// kXtfxWrapperNames, each once, in order of first appearance (`//` comments are skipped). What a
/// back end's test runs over its wrapper's output: an empty result means the list covers it.
inline std::vector<std::string> xtfxUnlistedWrapperNames(std::string_view code) {
    std::vector<std::string> out;
    const auto isStart = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    const auto isPart = [&](char c) { return isStart(c) || (c >= '0' && c <= '9'); };
    size_t i = 0;
    while (i < code.size()) {
        const char c = code[i];
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            while (i < code.size() && code[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (isStart(c)) {
            const size_t start = i;
            while (i < code.size() && isPart(code[i])) {
                ++i;
            }
            const std::string_view word = code.substr(start, i - start);
            const bool prefixed = word.size() >= 2 && (word[0] == 'x' || word[0] == 'X') && word[1] == 't';
            if (prefixed && !isXtfxWrapperName(word)) {
                bool seen = false;
                for (const std::string& s : out) {
                    seen = seen || s == word;
                }
                if (!seen) {
                    out.emplace_back(word);
                }
            }
            continue;
        }
        if (c >= '0' && c <= '9') {
            while (i < code.size() && isPart(code[i])) {
                ++i;  // a number with its suffix, e.g. 1e5 or 0x1F
            }
            continue;
        }
        ++i;
    }
    return out;
}

}  // namespace xt
