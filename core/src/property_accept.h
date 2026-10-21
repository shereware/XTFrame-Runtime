#pragma once

#include "xt/node.h"
#include "xt/property.h"
#include "xt/value.h"

/// T-257b: the one table of which values setProperty() (property_access.cpp) writes, shared with
/// IrView::validate's timeline keyframe check so the two cannot drift.

namespace xt {

/// True when setProperty() writes a value of `type` (a Length in `unit`; `unit` is ignored for any
/// other type) to `prop` on a node of `kind`: a defined property's own propertyType(), or, on an
/// Element's TranslateX/TranslateY, also a Length in px or % and the Vec2 (px, %) form (T-211).
bool propertyAcceptsValue(NodeKind kind, PropId prop, ValueType type, LengthUnit unit);

/// The same for a Value.
inline bool propertyAcceptsValue(NodeKind kind, PropId prop, const Value& value) {
    return propertyAcceptsValue(kind, prop, value.type,
                                value.type == ValueType::Length ? value.len.unit : LengthUnit::Px);
}

}  // namespace xt
