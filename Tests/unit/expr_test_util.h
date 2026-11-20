#pragma once

#include <string>
#include <vector>

#include "doctest.h"
#include "expr/expr.h"
#include "xt/timeline.h"

/// Helpers shared by the timeline expression tests (T-766, Docs/tasks/T-766.md).

namespace expr_test {

namespace xe = xt::compile::expr;

inline xe::ExprKey key(double t, double v, xt::Easing e = xt::Easing::linear()) {
    return xe::ExprKey{t, v, e};
}

/// A context over `keys` (which must outlive it).
inline xe::ExprContext context(const std::vector<xe::ExprKey>& keys, double duration, int index = 1,
                               uint32_t seed = 1234) {
    xe::ExprContext ctx;
    ctx.row = xe::ExprRow{keys, {}};
    ctx.duration = duration;
    ctx.index = index;
    ctx.seed = seed;
    return ctx;
}

/// Parses `text`, failing the test on a diagnostic.
inline xe::Program parse(const std::string& text) {
    xe::ParseResult r = xe::parseExpression(text);
    INFO("expression: " << text);
    if (!r.diagnostics.empty()) {
        INFO("diagnostic: " << r.diagnostics.front().message);
        CHECK(r.diagnostics.empty());
    }
    REQUIRE(r.ok());
    return std::move(r.program);
}

/// The first diagnostic's code / message of a parse that must fail.
inline xt::compile::Diagnostic parseError(const std::string& text) {
    xe::ParseResult r = xe::parseExpression(text);
    INFO("expression: " << text);
    REQUIRE_FALSE(r.ok());
    REQUIRE_FALSE(r.diagnostics.empty());
    return r.diagnostics.front();
}

/// Evaluates `text` at `t` for component ctx.component; fails the test on an error.
inline double eval(const std::string& text, const xe::ExprContext& ctx, double t) {
    const xe::Program p = parse(text);
    xe::EvalResult r = xe::evaluateExpression(p, ctx, t);
    INFO("expression: " << text << " at t = " << t);
    if (!r.diagnostics.empty()) {
        INFO("diagnostic: " << r.diagnostics.front().message);
        CHECK(r.diagnostics.empty());
    }
    REQUIRE(r.status == xe::ExprStatus::Ok);
    double out = 0;
    std::vector<xt::compile::Diagnostic> d;
    REQUIRE(xe::componentOf(r.value, ctx.component, out, d));
    return out;
}

/// The diagnostic of an evaluation that must fail.
inline xt::compile::Diagnostic evalError(const std::string& text, const xe::ExprContext& ctx, double t) {
    const xe::Program p = parse(text);
    xe::EvalResult r = xe::evaluateExpression(p, ctx, t);
    INFO("expression: " << text);
    REQUIRE(r.status == xe::ExprStatus::Error);
    REQUIRE_FALSE(r.diagnostics.empty());
    return r.diagnostics.front();
}

/// Plays lowered keys the way the runtime does: an xt::Timeline with one Float track, xt::evalTrack.
struct LoweredTrack {
    xt::Timeline timeline;
    std::vector<xt::EasingPoint> points;

    explicit LoweredTrack(const std::vector<xe::LoweredKey>& keys, std::vector<xt::EasingPoint> pool = {})
        : points(std::move(pool)) {
        for (const xe::LoweredKey& k : keys) {
            xt::Keyframe kf;
            kf.time = static_cast<float>(k.time);
            kf.value = xt::Value::ofFloat(static_cast<float>(k.value));
            kf.easing = k.interp == xe::LoweredInterp::Linear
                            ? xt::Easing::linear()
                            : (k.interp == xe::LoweredInterp::Hold ? xt::Easing::hold() : k.easing);
            timeline.keyframes.push_back(kf);
        }
        xt::Track track;
        track.type = xt::ValueType::Float;
        track.kfCount = static_cast<uint32_t>(timeline.keyframes.size());
        timeline.tracks.push_back(track);
    }

    double at(double t) const {
        return static_cast<double>(xt::evalTrack(timeline, timeline.tracks[0], static_cast<float>(t), nullptr,
                                                 nullptr, nullptr, points)
                                       .f);
    }
};

/// Lowers `text` (which must parse).
inline xe::LowerResult lower(const std::string& text, const xe::ExprContext& ctx) {
    const xe::Program p = parse(text);
    return xe::lowerExpression(p, ctx);
}

}  // namespace expr_test
