#pragma once

#include <cstdint>
#include <type_traits>

#include "xt/types.h"

/// Runtime property value representation (Value), its value-type enum and interpolation.
/// See docs/design/03-scene-and-properties.md §3.10, docs/tasks/T-05.md
/// value.h depends only on xt/types.h (kept dependency-light so xt/node.h can include it).

namespace xt {

/// See docs/design/03-scene-and-properties.md §3.10.
enum class ValueType : uint8_t {
    Float = 0,       ///< A single float, stored in Value::f.
    Vec2 = 1,        ///< A 2D vector, stored in Value::v2.
    Color = 2,       ///< An RGBA color, stored in Value::c.
    Length = 3,      ///< A CSS-style length, stored in Value::len.
    Path = 4,        ///< Path point data, stored as an external float span (floats/floatCount).
    Stops = 5,       ///< Gradient stop data, stored as an external span (stops/stopCount).
    FloatArray = 6,  ///< An arbitrary float array, stored as an external span (floats/floatCount).
    Enum = 7,        ///< An enum ordinal, stored in Value::e.
    StringRef = 8,   ///< A StringId reference, stored in Value::sid.
    ListRef = 9,     ///< A list/id reference, stored in Value::sid.
    Vec3 = 10,       ///< A 3D vector, stored in Value::v3.
    /// T-175 (docs/design/13-api.md §13.8.1, 27 §27.3.1): a port struct -- `itemCount` fields,
    /// names in `fieldNames` (StringIds of the owning Document), values in `items`. Non-owning.
    Struct = 11,
    /// T-175: a port list -- `itemCount` entries in `items`, each a Struct Value. Non-owning.
    List = 12,
};

/// Unit of a Length value.
enum class LengthUnit : uint8_t {
    Px = 0,       ///< Absolute pixels.
    Percent = 1,  ///< Percentage of a caller-provided reference size.
    Em = 2,       ///< Multiple of the current font size.
    Auto = 3,     ///< CSS `auto`; `Length::value` is ignored.
    // T-176 (docs/design/03-scene-and-properties.md §3.12.6, 25-editor-blueprint.md §25.4): resolved
    // against the Document's LengthEnv (xt/layout.h) when the length is used.
    Vw = 4,           ///< Percent of the viewport's logical width.
    Vh = 5,           ///< Percent of the viewport's logical height.
    Vmin = 6,         ///< Percent of the smaller viewport side.
    Vmax = 7,         ///< Percent of the larger viewport side.
    SafeTop = 8,      ///< Multiple of the safe area's top inset (`1safe-top` = the inset itself).
    SafeRight = 9,    ///< Multiple of the safe area's right inset.
    SafeBottom = 10,  ///< Multiple of the safe area's bottom inset.
    SafeLeft = 11,    ///< Multiple of the safe area's left inset.
    UiScale = 12,     ///< Multiple of 1px x Context::uiScale().
    /// A `calc()` expression: `Length::value` holds the program index -- an index into the IR's
    /// `CALC` chunk, or kRuntimeCalcBase + an index into the StyleEngine's runtime calc pool.
    Calc = 13,
    /// T-335 (docs/design/05-style-and-css.md §5.9.3, docs/tasks/T-335.md item 2): a multiple of
    /// the root element's font size (16px when the root declares none). Like `Em`, StyleEngine
    /// turns it into px when the style is resolved, so layout never sees it.
    Rem = 14,
};

/// T-176: a LengthUnit::Calc index at or above this refers to the runtime calc pool (a program
/// whose `em`/`var()` terms were substituted at style-resolve time), not the IR's `CALC` chunk.
/// Small enough that every index stays exact in a float.
inline constexpr uint32_t kRuntimeCalcBase = 1u << 22;

/// T-255: a LengthUnit::Calc length's `value` as a program index, or 0xFFFFFFFF (no program) when
/// it is negative, NaN or too large, so a malformed value never reaches a float-to-int cast.
inline uint32_t calcProgramIndex(float value) {
    return value >= 0.0f && value < 4294967296.0f ? static_cast<uint32_t>(value) : 0xFFFFFFFFu;
}

/// A CSS-style length value: a numeric value plus a unit (or Auto, which ignores value).
struct Length {
    /// Magnitude, interpreted per `unit` (ignored when `unit == LengthUnit::Auto`).
    float value = 0;
    /// Unit `value` is interpreted in.
    LengthUnit unit = LengthUnit::Px;
};

/// A single gradient color stop. See docs/design/03-scene-and-properties.md §3.4.
struct GradientStop {
    /// Position along the gradient, in [0, 1], stops given in increasing order.
    float offset = 0;
    /// Stop color.
    Color color{};
};

/// A single animatable/settable property value. Path/Stops/FloatArray data lives in an external
/// pool owned by the caller; Value only stores a non-owning span into it.
/// See docs/design/03-scene-and-properties.md §3.10-3.11, docs/tasks/T-05.md.
struct Value {
    /// Which union/span member is active.
    ValueType type = ValueType::Float;
    // Vec2/Color/Length carry default member initializers, which makes the anonymous union's
    // implicit default constructor deleted on GCC/Clang; provide one that picks the float member.
    constexpr Value() : f(0.0f) {}
    union {
        float f;       ///< Active when type == ValueType::Float.
        Vec2 v2;       ///< Active when type == ValueType::Vec2.
        Vec3 v3;       ///< Active when type == ValueType::Vec3.
        Color c;       ///< Active when type == ValueType::Color.
        Length len;    ///< Active when type == ValueType::Length.
        uint32_t e;    ///< Active when type == ValueType::Enum.
        uint32_t sid;  ///< Active when type == ValueType::StringRef or ValueType::ListRef.
    };
    // Path (points) / FloatArray: floatCount is the number of floats (point count x 2 for Path).
    // T-175: Struct/List reuse the same slots (the Value's size is unchanged).
    union {
        /// Non-owning pointer to Path/FloatArray float data (nullptr otherwise).
        const float* floats = nullptr;
        /// Struct: the field values; List: the entries (each a Struct). Non-owning.
        const Value* items;
    };
    union {
        /// Number of floats at `floats`.
        uint32_t floatCount = 0;
        /// Struct: field count; List: entry count.
        uint32_t itemCount;
    };
    // Stops.
    union {
        /// Non-owning pointer to Stops gradient-stop data (nullptr otherwise).
        const GradientStop* stops = nullptr;
        /// Struct only: the field names (`itemCount` StringIds). Non-owning.
        const StringId* fieldNames;
        /// T-190 (docs/design/03-scene-and-properties.md §3.12.6): Path only, optional -- the
        /// path's own verbs (PathVerb codes stored as floats, `stopCount` of them), for a Path
        /// keyframe whose topology differs from the node's. nullptr = "the node's own verbs".
        const float* pathVerbs;
    };
    /// Number of entries at `stops` (Path: number of entries at `pathVerbs`).
    uint32_t stopCount = 0;

    /// Builds a ValueType::Float value.
    static Value ofFloat(float v);
    /// Builds a ValueType::Vec2 value.
    static Value ofVec2(Vec2 v);
    /// Builds a ValueType::Vec3 value.
    static Value ofVec3(Vec3 v);
    /// Builds a ValueType::Color value.
    static Value ofColor(Color v);
    /// Builds a ValueType::Length value.
    static Value ofLength(float v, LengthUnit unit);
    /// Builds a ValueType::Enum value.
    static Value ofEnum(uint32_t v);
    /// Builds a ValueType::StringRef value.
    static Value ofStringId(StringId v);
    /// count is the number of floats (point count x 2), not the point count.
    static Value ofPath(const float* pts, uint32_t count);
    /// T-190: a Path value that also carries its own verbs (`verbCount` PathVerb codes stored as
    /// floats), so a keyframe may change the path's topology (docs/design/03 §3.12.6).
    static Value ofPathWithVerbs(const float* pts, uint32_t count, const float* verbs, uint32_t verbCount);
    /// Builds a ValueType::FloatArray value. count is the number of floats at data.
    static Value ofFloatArray(const float* data, uint32_t count);
    /// Builds a ValueType::Stops value. count is the number of GradientStop entries at data.
    static Value ofStops(const GradientStop* data, uint32_t count);
    /// T-175: builds a ValueType::Struct value over `count` field names and values (non-owning).
    static Value ofStruct(const StringId* names, const Value* values, uint32_t count);
    /// T-175: builds a ValueType::List value over `count` Struct entries (non-owning).
    static Value ofList(const Value* entries, uint32_t count);

    /// Neutral marker (see docs/design/12-*.md §12.3): type == Enum, e == 0xFFFFFFFF.
    static Value neutral();
    /// True if this Value is the neutral() marker.
    bool isNeutral() const;
};

static_assert(std::is_trivially_copyable_v<Value>, "Value must be trivially copyable");

inline Value Value::ofFloat(float v) {
    Value out;
    out.type = ValueType::Float;
    out.f = v;
    return out;
}

inline Value Value::ofVec2(Vec2 v) {
    Value out;
    out.type = ValueType::Vec2;
    out.v2 = v;
    return out;
}

inline Value Value::ofVec3(Vec3 v) {
    Value out;
    out.type = ValueType::Vec3;
    out.v3 = v;
    return out;
}

inline Value Value::ofColor(Color v) {
    Value out;
    out.type = ValueType::Color;
    out.c = v;
    return out;
}

inline Value Value::ofLength(float v, LengthUnit unit) {
    Value out;
    out.type = ValueType::Length;
    out.len = Length{v, unit};
    return out;
}

inline Value Value::ofEnum(uint32_t v) {
    Value out;
    out.type = ValueType::Enum;
    out.e = v;
    return out;
}

inline Value Value::ofStringId(StringId v) {
    Value out;
    out.type = ValueType::StringRef;
    out.sid = v;
    return out;
}

inline Value Value::ofPath(const float* pts, uint32_t count) {
    Value out;
    out.type = ValueType::Path;
    out.floats = pts;
    out.floatCount = count;
    return out;
}

inline Value Value::ofPathWithVerbs(const float* pts, uint32_t count, const float* verbs,
                                    uint32_t verbCount) {
    Value out = ofPath(pts, count);
    out.pathVerbs = verbCount != 0 ? verbs : nullptr;
    out.stopCount = verbCount != 0 ? verbCount : 0;
    return out;
}

/// T-190: true when two Path values share a topology: the same point count and, when both carry
/// verbs, the same verbs (docs/design/03-scene-and-properties.md §3.12.6: "頂點數與閉合相同才插值").
/// A value without verbs uses its node's verbs, which are assumed to match.
inline bool pathTopologyEquals(const Value& a, const Value& b) {
    if (a.floatCount != b.floatCount) {
        return false;
    }
    if (a.pathVerbs == nullptr || b.pathVerbs == nullptr) {
        return true;
    }
    if (a.stopCount != b.stopCount) {
        return false;
    }
    for (uint32_t i = 0; i < a.stopCount; ++i) {
        if (a.pathVerbs[i] != b.pathVerbs[i]) {
            return false;
        }
    }
    return true;
}

inline Value Value::ofFloatArray(const float* data, uint32_t count) {
    Value out;
    out.type = ValueType::FloatArray;
    out.floats = data;
    out.floatCount = count;
    return out;
}

inline Value Value::ofStops(const GradientStop* data, uint32_t count) {
    Value out;
    out.type = ValueType::Stops;
    out.stops = data;
    out.stopCount = count;
    return out;
}

inline Value Value::ofStruct(const StringId* names, const Value* values, uint32_t count) {
    Value out;
    out.type = ValueType::Struct;
    out.fieldNames = names;
    out.items = values;
    out.itemCount = count;
    return out;
}

inline Value Value::ofList(const Value* entries, uint32_t count) {
    Value out;
    out.type = ValueType::List;
    out.items = entries;
    out.itemCount = count;
    return out;
}

inline Value Value::neutral() {
    Value out;
    out.type = ValueType::Enum;
    out.e = 0xFFFFFFFFu;
    return out;
}

inline bool Value::isNeutral() const { return type == ValueType::Enum && e == 0xFFFFFFFFu; }

/// Exact, same-type comparison. External data (Path/FloatArray/Stops) compared element-wise.
/// Different types are always unequal.
inline bool valueEquals(const Value& a, const Value& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case ValueType::Float:
            return a.f == b.f;
        case ValueType::Vec2:
            return a.v2 == b.v2;
        case ValueType::Vec3:
            return a.v3.x == b.v3.x && a.v3.y == b.v3.y && a.v3.z == b.v3.z;
        case ValueType::Color:
            return a.c == b.c;
        case ValueType::Length:
            return a.len.unit == b.len.unit && a.len.value == b.len.value;
        case ValueType::Path:
        case ValueType::FloatArray:
            if (a.floatCount != b.floatCount) {
                return false;
            }
            if (a.type == ValueType::Path && !pathTopologyEquals(a, b)) {
                return false;
            }
            for (uint32_t i = 0; i < a.floatCount; ++i) {
                if (a.floats[i] != b.floats[i]) {
                    return false;
                }
            }
            return true;
        case ValueType::Stops:
            if (a.stopCount != b.stopCount) {
                return false;
            }
            for (uint32_t i = 0; i < a.stopCount; ++i) {
                if (a.stops[i].offset != b.stops[i].offset || a.stops[i].color != b.stops[i].color) {
                    return false;
                }
            }
            return true;
        case ValueType::Enum:
            return a.e == b.e;
        case ValueType::StringRef:
        case ValueType::ListRef:
            return a.sid == b.sid;
        case ValueType::Struct:
        case ValueType::List:
            if (a.itemCount != b.itemCount) {
                return false;
            }
            for (uint32_t i = 0; i < a.itemCount; ++i) {
                if ((a.type == ValueType::Struct && a.fieldNames[i] != b.fieldNames[i]) ||
                    !valueEquals(a.items[i], b.items[i])) {
                    return false;
                }
            }
            return true;
    }
    return false;
}

/// Interpolation per docs/design/03-scene-and-properties.md §3.10. a and b must share a.type ==
/// b.type. out's external data (Path/FloatArray/Stops) is written into scratchFloats/scratchStops,
/// which the caller must guarantee are large enough (>= a's length); the returned Value's
/// floats/stops pointers then point into scratch. When interpolation is discrete, the returned
/// Value is a copy of a or b unchanged (still pointing at the caller's original data, not scratch).
inline Value lerpValue(const Value& a, const Value& b, float t, float* scratchFloats,
                       GradientStop* scratchStops) {
    XT_ASSERT(a.type == b.type);
    switch (a.type) {
        case ValueType::Float:
            return Value::ofFloat(a.f + (b.f - a.f) * t);
        case ValueType::Vec2:
            return Value::ofVec2(a.v2 + (b.v2 - a.v2) * t);
        case ValueType::Vec3:
            return Value::ofVec3(Vec3{a.v3.x + (b.v3.x - a.v3.x) * t, a.v3.y + (b.v3.y - a.v3.y) * t,
                                      a.v3.z + (b.v3.z - a.v3.z) * t});
        case ValueType::Color: {
            Color c{a.c.r + (b.c.r - a.c.r) * t, a.c.g + (b.c.g - a.c.g) * t, a.c.b + (b.c.b - a.c.b) * t,
                    a.c.a + (b.c.a - a.c.a) * t};
            return Value::ofColor(c);
        }
        case ValueType::Length:
            // A Calc length's value is a program index, never interpolated (T-176).
            if (a.len.unit == b.len.unit && a.len.unit != LengthUnit::Auto &&
                a.len.unit != LengthUnit::Calc) {
                return Value::ofLength(a.len.value + (b.len.value - a.len.value) * t, a.len.unit);
            }
            return t < 0.5f ? a : b;
        case ValueType::Path:
        case ValueType::FloatArray: {
            // T-190 (03 §3.12.6): a Path interpolates only between equal topologies (point count
            // and verbs); anything else is a discrete jump like the other non-interpolable types.
            if (a.floatCount == b.floatCount && (a.type != ValueType::Path || pathTopologyEquals(a, b))) {
                for (uint32_t i = 0; i < a.floatCount; ++i) {
                    scratchFloats[i] = a.floats[i] + (b.floats[i] - a.floats[i]) * t;
                }
                Value out = a;
                out.floats = scratchFloats;
                out.floatCount = a.floatCount;
                return out;
            }
            return t < 0.5f ? a : b;
        }
        case ValueType::Stops: {
            if (a.stopCount == b.stopCount) {
                for (uint32_t i = 0; i < a.stopCount; ++i) {
                    scratchStops[i].offset = a.stops[i].offset + (b.stops[i].offset - a.stops[i].offset) * t;
                    const Color& ca = a.stops[i].color;
                    const Color& cb = b.stops[i].color;
                    scratchStops[i].color = Color{ca.r + (cb.r - ca.r) * t, ca.g + (cb.g - ca.g) * t,
                                                  ca.b + (cb.b - ca.b) * t, ca.a + (cb.a - ca.a) * t};
                }
                Value out = a;
                out.stops = scratchStops;
                out.stopCount = a.stopCount;
                return out;
            }
            return t < 0.5f ? a : b;
        }
        case ValueType::Enum:
        case ValueType::StringRef:
        case ValueType::ListRef:
        case ValueType::Struct:
        case ValueType::List:
            // Not covered by the 03.10 table for StringRef/ListRef; treated conservatively as
            // discrete, same as Enum.
            return t < 0.5f ? a : b;
    }
    return a;
}

}  // namespace xt
