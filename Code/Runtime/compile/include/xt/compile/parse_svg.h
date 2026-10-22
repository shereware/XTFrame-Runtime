#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/types.h"

/// Static SVG vector import: an `<img src="*.svg">` becomes a timeline-less Scene. See
/// Docs/design/12-compiler.md §12.2 step 3a and §12.2.1 (the supported subset), and
/// Docs/design/11-lottie-mapping.md §11.3.2/§11.3.3 for the node and paint structure this reuses.

namespace xt::compile {

/// Parses `text` (an SVG document) into `out`, a Scene whose `nodes[0]` is the root Group.
/// `writer` is used only to intern strings (node/scene names); the caller adds `out` to it.
/// `fileName` attributes diagnostics. Unsupported elements (`<text>`, `<image>`, `<filter>`,
/// `<mask>`, `<clipPath>`, `<pattern>`, CSS `<style>` blocks) are skipped with a warning; the rest
/// of the document still imports. Returns `kInvalidArgument` when the document is not
/// well-formed XML or has no `<svg>` root, in which case `out` is left untouched.
Status parseSvg(std::string_view text, std::string_view fileName, DiagnosticSink& sink, IrWriter& writer,
                SceneBuilder& out);

/// T-190 (11 §11.9): as above; additionally, when `ids` is non-null, appends (`id`, scene node
/// index) for every drawn `<g>`/basic shape carrying an `id` (inline `<svg>` timelines address
/// nodes by it). Also accepts `path-trim="<start> <end> [<offset>]"` on shapes (03 §3.8).
Status parseSvg(std::string_view text, std::string_view fileName, DiagnosticSink& sink, IrWriter& writer,
                SceneBuilder& out, std::vector<std::pair<std::string, uint32_t>>* ids);

/// T-243 (32 §32.4): a `bind:<target>` or `world-anchor` attribute on an element inside the SVG
/// (the `<svg>` root's own attributes are the DOM element's). The compiler turns it into a BIND
/// record on the owning `<svg>` element with IrBinding::sceneNode = `sceneNode`.
struct SvgBindAttr {
    uint32_t sceneNode = 0;      ///< Scene node index of the element.
    uint8_t kind = 0;            ///< NodeKind of that node.
    uint8_t constraintKind = 0;  ///< ConstraintKind when `kind` is Constraint.
    std::string name;            ///< `bind:<target>` or `world-anchor`.
    std::string value;           ///< The attribute value.
    int line = 0;                ///< 1-based line within the SVG text.
};

/// T-243: optional inputs and outputs of parseSvg() for rig content.
struct SvgParseExtras {
    /// Registers an `<xt-mesh href>` image (the `<img src>` registration: resolved, embedded and
    /// renamed like any image asset) and returns its ASST index; `line` is within the SVG text.
    /// When empty, the href is registered under its own name without a file lookup.
    std::function<AssetId(const std::string& href, int line)> imageAsset;
    /// Out: every `bind:` / `world-anchor` attribute on a scene node, in document order.
    std::vector<SvgBindAttr> binds;
    /// T-243b (32 §32.8): out: (`id`, 1-based line within the SVG text) of every `<xt-bone>` with
    /// an `id`, in document order (the compiler declares its `<id>.world` / `<id>.angle` ports).
    std::vector<std::pair<std::string, int>> boneIds;
};

/// T-243 (Docs/design/32-rigging-and-deformation.md §32.2): as above; additionally parses the rig
/// elements `<xt-skeleton>`, `<xt-bone>`, `<xt-constraint>`, `<xt-mesh>` (with `<xt-pins>`,
/// `<xt-tris>`, `<xt-weights>`) and `<xt-weights>` inside a `<path xt-skin>`, computing each
/// skin's inverse bind matrices from the bones' rest pose. `extras` may be null.
Status parseSvg(std::string_view text, std::string_view fileName, DiagnosticSink& sink, IrWriter& writer,
                SceneBuilder& out, std::vector<std::pair<std::string, uint32_t>>* ids,
                SvgParseExtras* extras);

/// T-265a (review H12; 32 §32.2.2): how a column of an `<xt-weights>` line is written -- items,
/// `=` (same as p) or left out / empty (same as p).
enum class WeightColumnForm : uint8_t { Items, Same, Omitted };

/// One `<xt-weights>` line, `<vertex> <p> [| <in> [| <out>]]`, as written.
struct WeightRowText {
    uint32_t vertex = 0;  ///< Set unless the line's vertex index is malformed.
    std::array<WeightColumnForm, 3> forms{WeightColumnForm::Items, WeightColumnForm::Omitted,
                                          WeightColumnForm::Omitted};
    /// (bone index, weight as written) of each `Items` column; empty for the others.
    std::array<std::vector<std::pair<uint32_t, float>>, 3> items;
};

/// Why parseWeightRow() refused a line.
enum class WeightRowError : uint8_t {
    None,
    BadVertex,       ///< The first word is not an unsigned integer.
    TooManyColumns,  ///< More than `columns` columns (a mesh vertex has only p).
    BoneRange,       ///< A bone index `>= boneCount`.
    BadWeights,      ///< Not 1 to 4 `<int>:<number >= 0>` items, or a column summing to 0.
};

/// T-265a (review H12): reads one line of `<xt-weights>` text (surrounding XML white space
/// ignored) -- the compiler's own row reading, shared with the editor so that both keep and drop
/// exactly the same lines. `columns` is 1 for an `<xt-mesh>`, 3 for a `<path>`; `boneCount` is the
/// size of the `bones` list. The vertex range is the caller's to check.
WeightRowError parseWeightRow(std::string_view row, uint32_t columns, size_t boneCount, WeightRowText& out);

}  // namespace xt::compile
