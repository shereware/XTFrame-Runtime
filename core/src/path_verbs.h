#pragma once

#include <cstddef>
#include <cstdint>

#include "xt/node.h"

namespace xt {

/// T-255 / T-255b: a Path value's own verbs (PathVerb codes stored as floats, as in a Path keyframe,
/// a `d` declaration or a Value handed to setProperty) are exact codes whose operands add up to
/// `pointCount` floats, the rule scene paths follow. The one copy of this rule: IrView::validate
/// and property_access.cpp both call it.
inline bool floatVerbsMatchPoints(const float* verbs, size_t verbCount, uint64_t pointCount) {
    uint64_t expected = 0;
    for (size_t i = 0; i < verbCount; ++i) {
        const float v = verbs[i];
        if (!(v >= 0.0f && v <= 3.0f) || v != static_cast<float>(static_cast<int>(v))) {
            return false;
        }
        const PathVerb verb = static_cast<PathVerb>(static_cast<uint8_t>(v));
        expected += verb == PathVerb::Cubic ? 6u : (verb == PathVerb::Close ? 0u : 2u);
    }
    return expected == pointCount;
}

}  // namespace xt
