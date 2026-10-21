#include "xt/compile/lottie_parser.h"

#include <exception>

#include "lottie_model.h"

/// Entry point: JSON -> LottieDoc (lottie_json.cpp) -> Scene/Timeline (lottie_build.cpp). See
/// docs/tasks/T-14.md.

namespace xt::compile {

LottieResult parseLottie(std::span<const uint8_t> json, const LottieOptions& options, IrWriter& writer,
                         DiagnosticSink& sink) {
    LottieResult result;
    std::string file = options.sourceName.empty() ? std::string("<lottie>") : options.sourceName;

    try {
        LottieDoc doc;
        if (!parseLottieJson(json, file, doc, sink)) {
            return result;
        }

        SceneBuilder scene;
        TimelineBuilder timeline;
        if (!buildLottieScene(doc, options, file, writer, sink, scene, timeline)) {
            return result;
        }

        if (sink.hasErrors() || (options.strict && sink.warningCount() > 0)) {
            return result;
        }

        result.scene = writer.addScene(std::move(scene));
        result.timeline = writer.addTimeline(std::move(timeline));
        result.ok = true;
        return result;
    } catch (const std::exception& e) {
        sink.error(file, std::string("malformed lottie JSON: ") + e.what());
        return LottieResult{};
    } catch (...) {
        sink.error(file, "malformed lottie JSON: unknown error");
        return LottieResult{};
    }
}

}  // namespace xt::compile
