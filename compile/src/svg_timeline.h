#pragma once

#include <map>
#include <string>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_svg.h"
#include "xt/compile/style_model.h"
#include "xt/types.h"

/// Inline `<svg>` elements (docs/design/11-lottie-mapping.md §11.9, 12-compiler.md §12.2.1/§12.7,
/// docs/tasks/T-190.md): each one's source becomes a scene (parseSvg), and the `@timeline` tracks
/// addressing its nodes (`#<svg id>/<inner id>`) become that scene's own Timeline, played in the
/// owning timeline group's time (04 §4.10.2).

namespace xt::compile {

/// The `sceneBySrc` key doc_emit.cpp looks an inline `<svg>` (DomTree node `domIndex`) up by.
std::string inlineSvgSceneKey(uint32_t domIndex);

/// Step 3b of compileDocument (after `<anim>`/`<img src=*.svg>`, before `@keyframes`, so the
/// SceneId == TimelineId identity holds): parses every inline `<svg>` of `dom` into a scene and
/// moves every `@timeline` track whose path is `<svg id>/<inner id>` out of `ruleSet.timelines`
/// into that scene's Timeline (keys in the timeline's seconds, arrival easings moved onto the
/// preceding key). The owning TimelineDef gets the (node, TimelineId) pair in `sceneMembers` and
/// its latest key in `sceneLastKey`. Errors: an unknown inner id, a property that does not apply
/// to the node (vector properties need a Path node; only `opacity`, `transform` components,
/// `transform-origin` px and the vector properties apply at all), a `d` that does not parse, two
/// timelines animating the same `<svg>`, the same property twice at one time.
/// T-218: when `innerIds` is non-null it receives, per inline `<svg>` with an `id`, the ids of its
/// drawn nodes (for warnUnknownSceneTargets()).
/// T-243 (32 §32.2/§32.4): the rig side of emitInlineSvgs(). `imageAsset` registers an
/// `<xt-mesh href>` (as SvgParseExtras::imageAsset, but `line` is the `.xt` file's); `binds`
/// receives, per inline `<svg>` DOM index, its content's `bind:` / `world-anchor` attributes with
/// their lines made `.xt` file lines (for emitBindings()).
struct InlineSvgRig {
    std::function<AssetId(const std::string& href, int line)> imageAsset;
    std::map<uint32_t, std::vector<SvgBindAttr>> binds;
    /// T-243b (32 §32.8): (`id`, `.xt` file line) of every named `<xt-bone>` of every inline
    /// `<svg>`, in document order (for emitPorts()' bone ports).
    std::vector<std::pair<std::string, int>> boneIds;
};

void emitInlineSvgs(const DomTree& dom, RuleSet& ruleSet, IrWriter& writer, const std::string& fileName,
                    DiagnosticSink& sink, std::map<std::string, SceneId>& sceneBySrc,
                    std::map<std::string, std::vector<std::string>>* innerIds = nullptr,
                    InlineSvgRig* rig = nullptr);

/// T-218 (31 §31.5.1): turns every selector subject `#<id>/<name>` (one path-id token, T-201)
/// into a scene target when no element has that whole id but `<id>` is an `<svg>`, `<anim>` or
/// `<img>` element (`<name>` an SVG tag name -> SceneTargetKind::Tag, else Name), recomputing its
/// specificity. T-236: when `innerIds` (emitInlineSvgs()'s) lists `<name>` for that inline `<svg>`,
/// it is a Name even when it is also a tag name (the id wins). Then, per rule: when every selector is a scene
/// target, a declaration an `<svg>` node does not take is dropped with a warning ("only vector properties
/// apply to <svg> content"; `transform`'s identity 3D components drop silently); when none is, a vector
/// property is dropped with the usual "only applies to inline <svg> content" warning. A mixed
/// group keeps both (emitStyle filters per selector). Runs after symbol expansion.
void resolveSceneTargets(RuleSet& ruleSet, const DomTree& dom, const std::string& fileName,
                         DiagnosticSink& sink,
                         const std::map<std::string, std::vector<std::string>>* innerIds = nullptr);

/// T-218 (31 §31.5.1): warns `vector.unknownTarget` for every scene-target rule `#<svg id>/<name>`
/// whose `<svg id>` is an inline `<svg>` of this document without a node of that name (a warning,
/// not an error: the node may only arrive with a later hotReload). Tag and `*` targets, and ids
/// that are not an inline `<svg>` (an `<anim>`, an `<img src=*.svg>`), are not checked.
void warnUnknownSceneTargets(const RuleSet& ruleSet,
                             const std::map<std::string, std::vector<std::string>>& innerIds,
                             const std::string& fileName, DiagnosticSink& sink);

}  // namespace xt::compile
