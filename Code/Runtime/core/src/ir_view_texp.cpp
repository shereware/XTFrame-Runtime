#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "expr_op_info.h"
#include "xt/ir.h"
#include "xt/property.h"

/// T-776: the optional `TEXP` chunk -- its parser and its cross-reference validation (Docs/design/
/// 58-value-functions-and-drivers.md §58.5, xt/ir_pod.h). Split out of ir_view.cpp, which is past the
/// file-length limit.

namespace xt {

namespace {

/// Reads `u32 count` then `count` packed records of T as a span of the payload.
template <class T>
bool readCounted(const uint8_t* payload, size_t size, size_t& pos, std::span<const T>& out) {
    uint32_t count = 0;
    if (size - pos < sizeof(count)) {
        return false;
    }
    std::memcpy(&count, payload + pos, sizeof(count));
    pos += sizeof(count);
    const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(T);
    if (bytes > size - pos) {
        return false;
    }
    out = std::span<const T>(reinterpret_cast<const T*>(payload + pos), count);
    pos += static_cast<size_t>(bytes);
    return true;
}

bool inRange(uint64_t offset, uint64_t count, uint64_t poolSize) { return offset + count <= poolSize; }

bool isIntegral(float v) { return std::isfinite(v) && std::floor(v) == v; }

/// One failed rule: records its text and returns kInvalidIr.
Status reject(std::string* detail, const char* what, size_t expr) {
    if (detail != nullptr) {
        *detail = "expression " + std::to_string(expr) + ": " + what;
    }
    return Status::kInvalidIr;
}

/// The checks of one op that do not need the stack (its operand count and its references).
const char* checkOp(const IrView& view, const IrTimelineExpr& e, const IrExprOp& op) {
    uint8_t minArgs = 0;
    uint8_t maxArgs = 0;
    if (!exprOpOperands(op.op, minArgs, maxArgs)) {
        return "unknown op";
    }
    if (op.argc < minArgs || op.argc > maxArgs) {
        return "wrong operand count";
    }
    const TimelineExprChunk& chunk = view.timelineExprs();
    const uint32_t portCount = static_cast<uint32_t>(view.ports().ports().size());
    const uint32_t elementCount = static_cast<uint32_t>(view.doc().elements().size());
    switch (static_cast<ExprOp>(op.op)) {
        case ExprOp::PushConst:
            return std::isfinite(op.imm) ? nullptr : "PushConst is not finite";
        case ExprOp::PushCurve:
            return op.ref < e.curveCount ? nullptr : "PushCurve names no curve of the expression";
        case ExprOp::CurveAt:  // T-777
            return op.ref < e.curveCount ? nullptr : "CurveAt names no curve of the expression";
        case ExprOp::RandomSeeded:  // T-777
            return op.imm == 0.0f || op.imm == 1.0f ? nullptr : "RandomSeeded's mode is neither 0 nor 1";
        case ExprOp::PushPort:
        case ExprOp::PushPortDelayed:
            return op.ref < portCount ? nullptr : "names no port";
        case ExprOp::PushProp:
            if (op.ref >= elementCount) {
                return "PushProp names no element";
            }
            return isIntegral(op.imm) && op.imm >= 0.0f && op.imm < 65536.0f * 4.0f
                       ? nullptr
                       : "PushProp's property code is invalid";
        case ExprOp::PushPropDelayed:  // T-781: the property must be one this runtime knows.
            if (op.ref >= elementCount) {
                return "PushPropDelayed names no element";
            }
            return isIntegral(op.imm) && op.imm >= 0.0f && op.imm < 65536.0f * 4.0f &&
                           propertyDefined(static_cast<PropId>(static_cast<uint32_t>(op.imm) >> 2))
                       ? nullptr
                       : "PushPropDelayed's property code is invalid";
        case ExprOp::PushLayout:
            if (op.ref >= elementCount) {
                return "PushLayout names no element";
            }
            return isIntegral(op.imm) && op.imm >= 0.0f && op.imm <= 3.0f
                       ? nullptr
                       : "PushLayout's box value is invalid";
        case ExprOp::Random:
            return op.imm == 0.0f || op.imm == 1.0f ? nullptr : "Random's mode is neither 0 nor 1";
        case ExprOp::RefValueAt: {
            const IrTimelineGroup& g = view.timelineGroups().groups()[e.group];
            if ((op.ref & kExprRefRow) == 0) {
                if (op.ref >= chunk.exprs().size() || chunk.exprs()[op.ref].group != e.group) {
                    return "RefValueAt names no expression of the same group";
                }
                return nullptr;
            }
            const uint32_t member = (op.ref >> 16) & 0x7FFFu;
            const uint32_t track = (op.ref >> 4) & 0xFFFu;
            const uint32_t component = op.ref & 0xFu;
            if (member >= g.memberCount) {
                return "RefValueAt names no member of the group";
            }
            const IrTimelineGroupMember& m = view.timelineGroups().members()[g.memberOffset + member];
            const std::span<const IrTrack> tracks = view.timeline(m.timeline).tracks();
            if (track >= tracks.size() || component >= exprTrackComponents(tracks[track].type)) {
                return "RefValueAt names no number of the member's timeline";
            }
            return nullptr;
        }
        default:
            return nullptr;
    }
}

}  // namespace

bool IrView::parseTimelineExprs(const uint8_t* payload, size_t payloadSize, TimelineExprChunk& out) {
    size_t pos = 0;
    if (!readCounted(payload, payloadSize, pos, out.m_exprs) ||
        !readCounted(payload, payloadSize, pos, out.m_ops) ||
        !readCounted(payload, payloadSize, pos, out.m_curves) ||
        !readCounted(payload, payloadSize, pos, out.m_keys)) {
        return false;
    }
    out.m_present = true;
    return true;
}

Status IrView::validateTimelineExprs(const IrView& view, std::string* detail) {
    const TimelineExprChunk& chunk = view.timelineExprs();
    if (!chunk.present()) {
        return Status::kOk;
    }
    // The curve pool: every curve is a non-empty run of finite keys in time order.
    for (size_t c = 0; c < chunk.curves().size(); ++c) {
        const IrExprCurve& curve = chunk.curves()[c];
        if (curve.keyCount == 0 || !inRange(curve.keyOffset, curve.keyCount, chunk.keys().size())) {
            return reject(detail, "a curve's key range is empty or outside the key pool", c);
        }
        for (uint32_t k = 0; k < curve.keyCount; ++k) {
            const IrExprKey& key = chunk.keys()[curve.keyOffset + k];
            if (!std::isfinite(key.time) || !std::isfinite(key.value) || key.interp > 1) {
                return reject(detail, "a curve key is not finite or has an unknown interpolation", c);
            }
            if (k > 0 && key.time < chunk.keys()[curve.keyOffset + k - 1].time) {
                return reject(detail, "a curve's keys are not in time order", c);
            }
        }
    }
    const TimelineGroupChunk& groups = view.timelineGroups();
    struct Row {
        uint32_t group, member, trackComponent;
        bool operator<(const Row& o) const {
            return group != o.group
                       ? group < o.group
                       : (member != o.member ? member < o.member : trackComponent < o.trackComponent);
        }
        bool operator==(const Row& o) const = default;
    };
    std::vector<Row> rows;  // The (group, member, track, component) of every expression.
    rows.reserve(chunk.exprs().size());
    for (size_t i = 0; i < chunk.exprs().size(); ++i) {
        const IrTimelineExpr& e = chunk.exprs()[i];
        if (!groups.present() || e.group >= groups.groups().size()) {
            return reject(detail, "names no timeline group", i);
        }
        const IrTimelineGroup& g = groups.groups()[e.group];
        if (e.member >= g.memberCount) {
            return reject(detail, "names no member of its group", i);
        }
        const IrTimelineGroupMember& m = groups.members()[g.memberOffset + e.member];
        const std::span<const IrTrack> tracks = view.timeline(m.timeline).tracks();
        if (e.track >= tracks.size()) {
            return reject(detail, "names no track of the member's timeline", i);
        }
        if (e.component >= exprTrackComponents(tracks[e.track].type)) {
            return reject(detail, "the track has no such number component", i);
        }
        if (e.opCount == 0 || e.opCount > kExprMaxOps ||
            !inRange(e.opOffset, e.opCount, chunk.ops().size())) {
            return reject(detail, "the program is empty, too long or outside the op pool", i);
        }
        if (!inRange(e.curveOffset, e.curveCount, chunk.curves().size())) {
            return reject(detail, "the curve range is outside the curve pool", i);
        }
        if (!std::isfinite(e.portHistorySeconds) || e.portHistorySeconds < 0.0f || !std::isfinite(e.index)) {
            return reject(detail, "the port history length or the index is invalid", i);
        }
        // The ops, with the stack simulated: no underflow, the deepest point equals maxStack (at
        // most kExprMaxStack), exactly one value at the end.
        uint32_t depth = 0;
        uint32_t deepest = 0;
        for (uint32_t k = 0; k < e.opCount; ++k) {
            const IrExprOp& op = chunk.ops()[e.opOffset + k];
            if (const char* why = checkOp(view, e, op)) {
                return reject(detail, why, i);
            }
            if (depth < op.argc) {
                return reject(detail, "the program pops an empty stack", i);
            }
            depth = depth - op.argc + 1;
            deepest = std::max(deepest, depth);
        }
        if (deepest > kExprMaxStack || deepest != e.maxStack) {
            return reject(detail, "maxStack does not match the program or is too deep", i);
        }
        if (depth != 1) {
            return reject(detail, "the program does not end with exactly one value", i);
        }
        rows.push_back(Row{e.group, e.member, (static_cast<uint32_t>(e.track) << 8) | e.component});
    }
    // One program per row component.
    std::sort(rows.begin(), rows.end());
    if (std::adjacent_find(rows.begin(), rows.end()) != rows.end()) {
        if (detail != nullptr) {
            *detail = "two programs write the same component";
        }
        return Status::kInvalidIr;
    }
    return Status::kOk;
}

}  // namespace xt
