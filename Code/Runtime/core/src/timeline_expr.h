#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/ir.h"
#include "xt/types.h"
#include "xt/value.h"

/// T-776: the run-time half of timeline expressions (Docs/design/58-value-functions-and-drivers.md
/// §58.5): the stack evaluator of the IR's `TEXP` programs, the Document's set of them with the
/// inputs they read (ports, another element's property or layout, the per-play random number, the
/// port history; T-781: the property history) and the rules for when a program must run again. Private to
/// core; the Animator runs the programs of its timeline-group members (animator.cpp), Document owns the set
/// (TimelineGroupSet::exprs, timeline_group.h). Allocates only in load(); never throws.

namespace xt {

class Animator;
class BindingRuntime;
class NodeArena;
struct TimelineGroupRuntime;

/// The run-time inputs a program reads (58 §58.5). The generations move when the value behind them
/// may have changed; a program whose inputs and time did not move is not run again.
class ExprInputs {
public:
    /// PORT `port`'s number now (Number, Bool, Enum, Entity; 0 for another type or no port).
    virtual float portNumber(uint32_t port) const = 0;
    /// Moves whenever a numeric port changes (BindingRuntime::numberGeneration()).
    virtual uint64_t portGeneration() const = 0;
    /// DOCT element `element`'s property `prop`, component `component`, now (0 when it has none).
    virtual float propValue(uint32_t element, PropId prop, uint8_t component) const = 0;
    /// DOCT element `element`'s last layout: `which` 0 left, 1 top, 2 width, 3 height.
    virtual float layoutValue(uint32_t element, uint8_t which) const = 0;
    /// Moves whenever a layout changed a box some program reads.
    virtual uint64_t layoutGeneration() const = 0;
    /// PORT `port`'s number `seconds` ago, from the port history (58 §58.5).
    virtual float portDelayed(uint32_t port, float seconds) const = 0;
    /// T-781: DOCT element `element`'s property (`code` = prop x 4 + component) `seconds` ago, from
    /// the property history (kept like the port history).
    virtual float propDelayed(uint32_t element, uint32_t code, float seconds) const = 0;
    /// Moves whenever the port or the property history records.
    virtual uint64_t portHistoryGeneration() const = 0;

protected:
    ~ExprInputs() = default;
};

class TimelineExprSet;

/// One program and what it owns in the TEXP pools.
struct ExprProgram {
    std::span<const IrExprOp> ops;
    std::span<const IrExprCurve> curves;  ///< The expression's hidden curves.
    std::span<const IrExprKey> keys;      ///< The chunk's whole key pool (curves index into it).
    uint32_t seed = 0;
    float index = 0;
};

/// What one run of a program reads besides its ops.
struct ExprEnv {
    float value = 0;     ///< The row's interpolated component now (PushValue; the fallback).
    float time = 0;      ///< Group time, seconds.
    float duration = 0;  ///< Group duration, seconds.
    uint32_t playSeed = 0;
    const ExprInputs* inputs = nullptr;  ///< null: every input reads 0.
    /// ValueAt / RefValueAt (expression `self` of `set`); null: ValueAt reads `value`, RefValueAt 0.
    const TimelineExprSet* set = nullptr;
    uint32_t self = 0;
    uint32_t depth = 0;  ///< RefValueAt nesting of this run.
};

/// Runs `program` (validated by IrView::open). A result that is not finite (a division by zero, the
/// log of a negative...) gives `env.value` and sets `*fellBack`. No allocation, no exception.
float evalTimelineExpr(const ExprProgram& program, const ExprEnv& env, bool* fellBack = nullptr);

/// A hidden curve (a run of IrExprKey, times ascending) at `t`: the first key's value before it, the
/// last's after it, linear or held between keys.
float sampleExprCurve(std::span<const IrExprKey> keys, float t);

/// Component `c` of a scalar Value (Float, Length, Vec2, Vec3, Color); 0 for anything else.
float exprValueComponent(const Value& v, uint8_t c);
/// Writes component `c` of a scalar Value; no-op for anything else.
void setExprValueComponent(Value& v, uint8_t c, float x);

/// The Document's TEXP programs and the inputs they read.
class TimelineExprSet final : public ExprInputs {
public:
    /// Rebuilds from `ir` (empty without TEXP): the programs, the inputs each one reads (its own ops
    /// and the programs RefValueAt runs), the boxes to watch and the port history rings.
    void load(const IrView& ir);
    /// The Document state the inputs read (pointers kept; all outlive the set's use).
    void bind(const NodeArena* arena, const std::vector<NodeId>* docElementNodes,
              const BindingRuntime* bindings, const Animator* animator);
    bool empty() const { return m_exprs.empty(); }
    /// The expressions of member `member` of group `group`.
    std::span<const uint32_t> exprsOf(uint32_t group, uint32_t member) const;
    /// Expression `expr`'s record.
    const IrTimelineExpr& record(uint32_t expr) const { return m_exprs[expr]; }
    /// The kExprInput* bits expression `expr` depends on.
    uint16_t inputMask(uint32_t expr) const { return m_masks[expr]; }
    /// Notes that member `member` of group `group` runs in seconds (an inline `<svg>`'s scene).
    void setMemberScene(uint32_t group, uint32_t member, bool scene);

    /// Runs expression `expr` at group time `t`, `value` being its row's value at `t`.
    float evaluate(uint32_t expr, float t, float value, uint32_t playSeed, uint32_t depth = 0,
                   bool* fellBack = nullptr) const;
    /// Expression `expr`'s own row (its keys) at group time `t`.
    float rowValueAt(uint32_t expr, float t) const;
    /// RefValueAt `ref` of expression `expr` at group time `t`.
    float refValueAt(uint32_t expr, uint32_t ref, float t, uint32_t playSeed, uint32_t depth) const;

    /// The next per-play seed of group `group` (a Document-level count hashed with the group).
    uint32_t drawPlaySeed(uint32_t group, StringId name);
    /// Test entry: restarts the per-play seeds from `base`.
    void setSeedBase(uint32_t base) {
        m_seedBase = base;
        m_playCount = 0;
    }
    /// One update: records the history of every port (T-781: and every property) a playing group
    /// reads with a delay.
    void recordPortHistory(float clock, std::span<const TimelineGroupRuntime> groups);
    /// After a layout: moves layoutGeneration() when a box some program reads changed.
    void checkLayout();
    /// The records held for PORT `port` (tests); 0 when it has no history.
    uint32_t portHistoryCount(uint32_t port) const;
    /// T-781: the records held for DOCT element `element`'s property `code` (tests); 0 when none.
    uint32_t propHistoryCount(uint32_t element, uint32_t code) const;

    // ExprInputs.
    float portNumber(uint32_t port) const override;
    uint64_t portGeneration() const override;
    float propValue(uint32_t element, PropId prop, uint8_t component) const override;
    float layoutValue(uint32_t element, uint8_t which) const override;
    uint64_t layoutGeneration() const override { return m_layoutGeneration; }
    float portDelayed(uint32_t port, float seconds) const override;
    float propDelayed(uint32_t element, uint32_t code, float seconds) const override;
    uint64_t portHistoryGeneration() const override { return m_historyGeneration; }

private:
    /// One port's (T-781: or one property component's) history: `capacity` (time, value) records in
    /// a ring.
    struct Ring {
        bool prop = false;    // T-781: a property (PushPropDelayed), else a port (PushPortDelayed).
        uint32_t ref = 0;     // PORT index, or DOCT element.
        uint32_t code = 0;    // T-781: prop x 4 + component (property rings).
        uint32_t offset = 0;  // Into m_historyTimes / m_historyValues.
        uint32_t capacity = 0;
        uint32_t head = 0;             // The next slot written.
        uint32_t count = 0;            // Records held.
        std::vector<uint32_t> groups;  // The groups with a program reading it delayed.
    };
    struct Watched {
        uint32_t element = 0;
        float box[4] = {};
    };
    float rowKeysAt(uint32_t group, uint32_t member, uint32_t track, uint8_t component, float t) const;
    ExprProgram program(uint32_t expr) const;
    const Ring* ringOf(bool prop, uint32_t ref, uint32_t code) const;
    /// The value of ring `r` `seconds` before the last record time (`now` when it has none).
    float delayed(const Ring* r, float seconds, float now) const;

    std::span<const IrTimelineExpr> m_exprs;
    std::span<const IrExprOp> m_ops;
    std::span<const IrExprCurve> m_curves;
    std::span<const IrExprKey> m_keys;
    std::span<const IrTimelineGroup> m_groups;
    std::span<const IrTimelineGroupMember> m_members;
    std::vector<uint16_t> m_masks;       // Per expression.
    std::vector<uint32_t> m_byMember;    // Expression indices sorted by (group, member).
    std::vector<uint8_t> m_memberScene;  // Per TGRP member (absolute index).
    std::vector<Ring> m_rings;
    std::vector<float> m_historyTimes;
    std::vector<float> m_historyValues;
    std::vector<Watched> m_watched;
    const NodeArena* m_arena = nullptr;
    const std::vector<NodeId>* m_docNodes = nullptr;
    const BindingRuntime* m_bindings = nullptr;
    const Animator* m_animator = nullptr;
    float m_now = 0;
    uint64_t m_layoutGeneration = 0;
    uint64_t m_historyGeneration = 0;
    uint32_t m_seedBase = 0x2545F491u;
    uint32_t m_playCount = 0;
};

}  // namespace xt
