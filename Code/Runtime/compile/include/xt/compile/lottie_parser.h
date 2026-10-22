#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "xt/compile/diagnostics.h"
#include "xt/compile/ir_writer.h"
#include "xt/types.h"

/// Lottie (bodymovin) JSON -> SceneBuilder + TimelineBuilder parser, stage one: document-level
/// fields, layer transforms/keyframes/timing, layer nesting. See Docs/design/11-lottie-mapping.md
/// §11.1-11.3.1, §11.4, §11.5, §11.7, Docs/design/12-compiler.md §12.1, Docs/tasks/T-14.md.
/// Shape contents: T-15. Masks/track matte/image assets: T-16 (see Docs/tasks/T-16.md).

namespace xt::compile {

/// Options controlling a `parseLottie()` call.
struct LottieOptions {
    /// Any warning is treated as a parse failure.
    bool strict = false;
    /// Name used to attribute diagnostics (typically the input file's path).
    std::string sourceName;
    /// Base directory external image assets (`u + p`) are resolved against, for the existence
    /// check and (with `embedImages`) for actually reading the bytes. Empty = no check.
    std::string rootDir;
    /// Read external image asset bytes (`rootDir / (u+p)`) and embed them at parse time instead
    /// of leaving the ASST entry external. A read failure warns and leaves it external.
    bool embedImages = false;
};

/// Outcome of a `parseLottie()` call.
struct LottieResult {
    /// Id of the newly added scene, or `kInvalidScene` on failure.
    SceneId scene = kInvalidScene;
    /// Id of the newly added timeline, or `kInvalidTimeline` on failure.
    TimelineId timeline = kInvalidTimeline;
    /// True if parsing succeeded.
    bool ok = false;
};

/// Parses `json` and writes one new Scene and one new Timeline into `writer`. On a JSON syntax
/// error or a missing required top-level field, returns `ok = false`, records an error in `sink`,
/// and leaves `writer` unmodified. Never throws (nlohmann/json exceptions are caught internally).
LottieResult parseLottie(std::span<const uint8_t> json, const LottieOptions& options, IrWriter& writer,
                         DiagnosticSink& sink);

}  // namespace xt::compile
