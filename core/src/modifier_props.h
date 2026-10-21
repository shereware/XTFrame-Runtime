#pragma once

#include "xt/node.h"
#include "xt/property.h"
#include "xt/value.h"

/// T-225 (docs/design/31-vector-authoring.md §31.6.1): the PropId <-> ModifierNodeData /
/// ShapeNodeData::params mapping behind getProperty()/setProperty(), shared with StyleEngine's
/// scene-target base snapshot (irSceneBaseValue). Defined in property_access.cpp.

namespace xt {

/// getProperty()'s value of modifier PropId `prop` (208-231) on `m`: a Float (Vec2 for ModAnchor,
/// ModTranslate, ModCenter); 0 for a prop `m.kind` does not use.
Value modifierPropertyValue(const ModifierNodeData& m, PropId prop);

/// getProperty()'s value of shape PropId `prop` (232-245) for a shape of `kind` with `params`
/// (12 floats); 0 for a prop `kind` does not use.
float shapeParamValue(ShapeKind kind, const float* params, PropId prop);

/// T-243 (32 §32.2.3): getProperty()'s value of constraint PropId `prop` (247-255) on `c`; 0 for a
/// prop `c.kind` does not use (IkStrength on a non-Ik constraint).
float constraintPropertyValue(const ConstraintNodeData& c, PropId prop);

/// T-243: setProperty() of constraint PropId `prop` (247-255) on `c` (ignored when `c.kind` does
/// not use it).
void setConstraintPropertyValue(ConstraintNodeData& c, PropId prop, float value);

}  // namespace xt
