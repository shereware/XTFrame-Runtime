#pragma once

#include <cstdint>

#include "xt/ir_pod.h"
#include "xt/value.h"

/// T-776: the operand counts of the timeline expression ops (xt/ir_pod.h ExprOp), shared by the TEXP
/// validator (ir_view_texp.cpp) and the evaluator (timeline_expr.cpp). Private to core.

namespace xt {

/// The fewest and most operands `op` pops; false for a byte that is no ExprOp.
inline bool exprOpOperands(uint8_t op, uint8_t& minArgs, uint8_t& maxArgs) {
    if (op > kExprOpLast) {
        return false;
    }
    switch (static_cast<ExprOp>(op)) {
        case ExprOp::PushConst:
        case ExprOp::PushValue:
        case ExprOp::PushCurve:
        case ExprOp::PushPort:
        case ExprOp::PushProp:
        case ExprOp::PushLayout:
        case ExprOp::PushPlayRandom:
        case ExprOp::PushTime:
        case ExprOp::PushDuration:
        case ExprOp::PushIndex:
            minArgs = maxArgs = 0;
            return true;
        case ExprOp::Neg:
        case ExprOp::Abs:
        case ExprOp::Round:
        case ExprOp::Floor:
        case ExprOp::Ceil:
        case ExprOp::Sqrt:
        case ExprOp::Exp:
        case ExprOp::Log:
        case ExprOp::Sin:
        case ExprOp::Cos:
        case ExprOp::Tan:
        case ExprOp::Asin:
        case ExprOp::Acos:
        case ExprOp::Atan:
        case ExprOp::Not:
        case ExprOp::Noise:
        case ExprOp::ValueAt:
        case ExprOp::RefValueAt:
        case ExprOp::PushPortDelayed:
        case ExprOp::CurveAt:          // T-777
        case ExprOp::PushPropDelayed:  // T-781
            minArgs = maxArgs = 1;
            return true;
        case ExprOp::Clamp:
        case ExprOp::Lerp:
        case ExprOp::Select:
            minArgs = maxArgs = 3;
            return true;
        case ExprOp::Linear:
        case ExprOp::Ease:
        case ExprOp::EaseIn:
        case ExprOp::EaseOut:
            minArgs = maxArgs = 5;
            return true;
        case ExprOp::Wiggle:
            minArgs = 2;
            maxArgs = 5;
            return true;
        case ExprOp::Random:
            minArgs = 2;
            maxArgs = 3;
            return true;
        case ExprOp::RandomSeeded:  // T-777
            minArgs = 3;
            maxArgs = 4;
            return true;
        default:  // The two-operand arithmetic, comparisons, And / Or, Min / Max, Pow, Atan2.
            minArgs = maxArgs = 2;
            return true;
    }
}

/// The run-time input bit (kExprInput*) an op reads, 0 for none.
inline uint16_t exprOpInput(uint8_t op) {
    switch (static_cast<ExprOp>(op)) {
        case ExprOp::PushPort:
            return kExprInputPort;
        case ExprOp::PushProp:
            return kExprInputProp;
        case ExprOp::PushLayout:
            return kExprInputLayout;
        case ExprOp::PushPlayRandom:
            return kExprInputPlayRandom;
        case ExprOp::PushPortDelayed:
            return kExprInputPortHistory;
        case ExprOp::PushPropDelayed:  // T-781
            return kExprInputPropHistory;
        default:
            return 0;
    }
}

/// The number of scalar components of a track of ValueType `type` a program may replace: Float and
/// Length 1, Vec2 2, Vec3 3, Color 4; 0 for every other type.
inline uint32_t exprTrackComponents(uint8_t type) {
    switch (static_cast<ValueType>(type)) {
        case ValueType::Float:
        case ValueType::Length:
            return 1;
        case ValueType::Vec2:
            return 2;
        case ValueType::Vec3:
            return 3;
        case ValueType::Color:
            return 4;
        default:
            return 0;
    }
}

}  // namespace xt
