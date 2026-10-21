#include "calc_emit.h"

#include <utility>

namespace xt::compile {

uint32_t emitCalc(const std::vector<CalcOpDecl>& ops, IrWriter& writer) {
    std::vector<IrCalcOp> out;
    out.reserve(ops.size());
    for (const CalcOpDecl& op : ops) {
        IrCalcOp ir{};
        ir.op = static_cast<uint8_t>(op.op);
        ir.name = kInvalidString;
        switch (op.op) {
            case CalcOpKind::PushLength:
                ir.unit = static_cast<uint8_t>(op.unit);
                ir.value = op.value;
                break;
            case CalcOpKind::PushNumber:
                ir.value = op.value;
                break;
            case CalcOpKind::PushVar:
                ir.name = writer.intern(op.varName);
                if (op.hasFallback) {
                    ir.hasFallback = 1;
                    ir.unit = op.fallbackIsNumber ? kCalcUnitNumber : static_cast<uint8_t>(op.unit);
                    ir.value = op.value;
                }
                break;
            default:
                break;
        }
        out.push_back(ir);
    }
    return writer.addCalc(out);
}

bool foldCalcToPx(const std::vector<CalcOpDecl>& ops, float& px) {
    bool hasMinMax = false;
    for (const CalcOpDecl& op : ops) {
        if (op.op == CalcOpKind::PushVar || (op.op == CalcOpKind::PushLength && op.unit != LengthUnit::Px)) {
            return false;
        }
        hasMinMax = hasMinMax || op.op == CalcOpKind::Min || op.op == CalcOpKind::Max;
    }
    if (!hasMinMax) {
        return false;
    }
    // The parser type-checked the program; this mirrors the runtime evaluator (layout_engine.cpp).
    struct Slot {
        float v;
        bool isLength;
    };
    std::vector<Slot> stack;
    for (const CalcOpDecl& op : ops) {
        if (op.op == CalcOpKind::PushLength || op.op == CalcOpKind::PushNumber) {
            stack.push_back(Slot{op.value, op.op == CalcOpKind::PushLength});
            continue;
        }
        if (stack.size() < 2) {
            return false;
        }
        const Slot b = stack.back();
        stack.pop_back();
        Slot& a = stack.back();
        switch (op.op) {
            case CalcOpKind::Add:
                a.v += b.v;
                break;
            case CalcOpKind::Sub:
                a.v -= b.v;
                break;
            case CalcOpKind::Mul:
                a.v *= b.v;
                a.isLength = a.isLength || b.isLength;
                break;
            case CalcOpKind::Div:
                if (b.v == 0.0f) {
                    return false;
                }
                a.v /= b.v;
                break;
            case CalcOpKind::Min:
                a.v = b.v < a.v ? b.v : a.v;
                break;
            case CalcOpKind::Max:
                a.v = b.v > a.v ? b.v : a.v;
                break;
            default:
                return false;
        }
    }
    if (stack.size() != 1 || !stack.back().isLength) {
        return false;
    }
    px = stack.back().v;
    return true;
}

void emitMedia(const std::vector<MediaDef>& media, const std::vector<IrMediaRule>& ruleMap,
               IrWriter& writer) {
    if (ruleMap.empty()) {
        return;
    }
    MediaBuilder builder;
    for (const MediaDef& def : media) {
        IrMedia block{};
        block.queryOffset = static_cast<uint32_t>(builder.queries.size());
        block.queryCount = static_cast<uint32_t>(def.queries.size());
        for (const std::vector<MediaCondDecl>& query : def.queries) {
            IrMediaQuery q{};
            q.condOffset = static_cast<uint32_t>(builder.conds.size());
            q.condCount = static_cast<uint32_t>(query.size());
            for (const MediaCondDecl& cond : query) {
                IrMediaCond c{};
                c.feature = static_cast<uint8_t>(cond.feature);
                c.value = cond.value;
                builder.conds.push_back(c);
            }
            builder.queries.push_back(q);
        }
        builder.media.push_back(block);
    }
    builder.rules = ruleMap;
    writer.setMedia(std::move(builder));
}

}  // namespace xt::compile
