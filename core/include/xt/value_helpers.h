#pragma once

#include <cstdint>
#include <string_view>

#include "xt/types.h"
#include "xt/value.h"

/// Value construction helpers and the runtime CSS value parser of docs/design/13-api.md §13.4.
/// See docs/tasks/T-35.md.

namespace xt {

/// Wraps a float in a Value.
inline Value makeFloat(float v) { return Value::ofFloat(v); }
/// Wraps a 2D vector in a Value.
inline Value makeVec2(float x, float y) { return Value::ofVec2(Vec2{x, y}); }
/// Wraps an RGBA color (each channel 0-1) in a Value.
inline Value makeColor(float r, float g, float b, float a) { return Value::ofColor(Color{r, g, b, a}); }
/// Wraps a length with its unit in a Value.
inline Value makeLength(float v, LengthUnit unit) { return Value::ofLength(v, unit); }
/// Wraps a raw enum ordinal in a Value.
inline Value makeEnum(uint32_t v) { return Value::ofEnum(v); }

/// Parses one CSS value ("12px", "#ff0000", "0.5") for `prop` -- an animatable PropId (0-105) or a
/// non-animatable StyleProp number (160-186) -- using the same mini parser ElementRef::setStyle()
/// uses (core/src/mini_css.h). Errors:
///   kInvalidArgument  unknown prop, or the text is not a valid value for it;
///   kUnsupported      `transition`/`animation` (compile-time only, 05.6), `transform`/
///                     `transform-origin` (they expand to several props, so there is no single
///                     Value to return) and the string-valued props `font-family`/
///                     `background-image` (interning their string needs a Document).
Result<Value> parseValue(uint16_t prop, std::string_view css);

}  // namespace xt
