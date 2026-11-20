#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "bind_emit.h"  // PortScopes
#include "expressions_section.h"
#include "style_emit.h"  // staticEffectSequence(), resolveEffectAddress()
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/style_model.h"
#include "xt/property.h"

/// T-777 (Docs/design/58-value-functions-and-drivers.md §58.5, 02 §2.v): the `TEXP` chunk -- the
/// run-time programs applyExpressions() compiled, with their rows, ports and references resolved to
/// TGRP / PORT / DOCT indices once the timeline groups and the ports exist. See
/// expressions_section.h.

namespace xt::compile {

namespace {

/// A row of a timeline group: member index, track index in the member's timeline, component.
struct RowIndex {
    uint32_t member = 0;
    uint16_t track = 0;
    uint8_t component = 0;
};

uint32_t trackComponents(uint8_t type) {
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

class TexpEmitter {
public:
    TexpEmitter(const RuleSet& ruleSet, const DomTree& dom, const std::vector<uint32_t>& groupOfTimeline,
                const TimelineGroupBuilder& groups, const PortScopes& ports, IrWriter& writer,
                DiagnosticSink& sink, std::vector<ExpressionOutcome>* outcomes)
        : m_rules(ruleSet),
          m_dom(dom),
          m_groupOf(groupOfTimeline),
          m_groups(groups),
          m_ports(ports),
          m_writer(writer),
          m_sink(sink),
          m_outcomes(outcomes) {}

    void run(const std::vector<RuntimeExpression>& list) {
        // Each entry is resolved after the entries it references (the lowering finished those first,
        // so they come earlier): a reference to a skipped one reads that row's keys instead.
        m_index.assign(list.size(), UINT32_MAX);
        m_depth.assign(list.size(), 0);
        m_rows.assign(list.size(), Resolved{});
        TimelineExprBuilder out;
        for (size_t i = 0; i < list.size(); ++i) {
            emitOne(list, i, out);
        }
        if (!out.exprs.empty()) {
            m_writer.setTimelineExprs(std::move(out));
        }
    }

private:
    struct Resolved {
        bool ok = false;
        uint32_t group = 0;
        RowIndex row;
    };

    void skip(const RuntimeExpression& r, const char* code, const std::string& message) {
        const std::string text = r.label + ": " + message + "; the row keeps its keyframes";
        m_sink.note(r.file, text, r.line, r.col, code);
        if (m_outcomes != nullptr && r.outcome < m_outcomes->size()) {
            ExpressionOutcome& o = (*m_outcomes)[r.outcome];
            o.status = ExpressionOutcome::Status::Skipped;
            o.diagnostics.push_back(
                Diagnostic{Diagnostic::Level::Warning, r.file, r.line, r.col, text, code});
        }
    }

    uint32_t groupOf(const TimelineDef* def) const {
        if (def == nullptr || m_rules.timelines.empty() || def < m_rules.timelines.data() ||
            def >= m_rules.timelines.data() + m_rules.timelines.size()) {
            return UINT32_MAX;
        }
        const size_t i = static_cast<size_t>(def - m_rules.timelines.data());
        return i < m_groupOf.size() ? m_groupOf[i] : UINT32_MAX;
    }

    /// T-781: the member and track of the inline `<svg>` row `path` / `prop` (on `<svg>` `element`) in
    /// group `group`: the scene member of that `<svg>` and the row's track in its scene Timeline.
    bool sceneRowIndex(uint32_t group, const TimelineDef& def, uint32_t element, const std::string& path,
                       PropId prop, RowIndex& out) const {
        const IrTimelineGroup& g = m_groups.groups[group];
        for (const TimelineDef::SceneRow& row : def.sceneRows) {
            if (row.path != path || row.prop != prop) {
                continue;
            }
            for (uint32_t m = 0; m < g.memberCount; ++m) {
                const IrTimelineGroupMember& mem = m_groups.members[g.memberOffset + m];
                const TimelineBuilder* tl = m_writer.timeline(mem.timeline);
                if (mem.element != element || mem.timeline != row.timeline || tl == nullptr ||
                    row.track >= tl->tracks.size() || trackComponents(tl->tracks[row.track].type) == 0) {
                    continue;
                }
                out.member = m;
                out.track = static_cast<uint16_t>(row.track);
                out.component = 0;
                return row.track < 0x1000 && m < 0x8000;
            }
        }
        return false;
    }

    /// The member, track and component of `element`'s row `row` in group `group` (T-781: `svgPath`
    /// set: of that shape inside the inline `<svg>` `element`).
    bool rowIndex(uint32_t group, const TimelineDef& def, uint32_t element, const RuntimeRowAddr& row,
                  RowIndex& out, const std::string& svgPath = std::string()) {
        if (group >= m_groups.groups.size()) {
            return false;
        }
        if (!svgPath.empty()) {
            return row.prop != 0xFFFF && sceneRowIndex(group, def, element, svgPath, row.prop, out);
        }
        const IrTimelineGroup& g = m_groups.groups[group];
        PropId prop = row.prop;
        uint8_t component = 0;
        if (row.prop == 0xFFFF) {
            std::vector<StyleDecl> scratch;
            const std::vector<EffectDeclEntry>* seq =
                staticEffectSequence(m_rules, m_dom, element, def.fileName, scratch);
            std::string error;
            ValueType type = ValueType::Float;
            if (!resolveEffectAddress(m_writer, row.effect, row.param, seq, error, prop, type)) {
                return false;
            }
            if (type == ValueType::Vec2 && row.param.size() > 2 &&
                row.param.compare(row.param.size() - 2, 2, ".y") == 0) {
                component = 1;
            }
        }
        for (uint32_t m = 0; m < g.memberCount; ++m) {
            const IrTimelineGroupMember& mem = m_groups.members[g.memberOffset + m];
            if (mem.element != element) {
                continue;
            }
            const bool scene = std::any_of(def.sceneMembers.begin(), def.sceneMembers.end(),
                                           [&](const auto& s) { return s.second == mem.timeline; });
            const TimelineBuilder* tl = scene ? nullptr : m_writer.timeline(mem.timeline);
            if (tl == nullptr) {
                continue;
            }
            for (size_t t = 0; t < tl->tracks.size(); ++t) {
                if (tl->tracks[t].prop == prop && component < trackComponents(tl->tracks[t].type)) {
                    out.member = m;
                    out.track = static_cast<uint16_t>(t);
                    out.component = component;
                    return t < 0x1000 && m < 0x8000;
                }
            }
        }
        return false;
    }

    static uint32_t rowRef(const RowIndex& r) {
        return kExprRefRow | (r.member << 16) | (static_cast<uint32_t>(r.track) << 4) | r.component;
    }

    void emitOne(const std::vector<RuntimeExpression>& list, size_t i, TimelineExprBuilder& out) {
        const RuntimeExpression& r = list[i];
        Resolved& self = m_rows[i];
        self.group = groupOf(r.def);
        if (self.group == UINT32_MAX) {
            return;  // The timeline was dropped (an error is reported for it).
        }
        if (!rowIndex(self.group, *r.def, r.element, r.row, self.row, r.svgPath)) {
            skip(r, "expr.limit", "the row has no track to run a program on");
            return;
        }
        self.ok = true;  // A reference to it can read its keys even when its program is skipped.
        std::vector<IrExprOp> ops;
        ops.reserve(r.program.ops.size());
        uint32_t depth = 0;
        for (const expr::RuntimeOp& op : r.program.ops) {
            IrExprOp ir{};
            ir.op = static_cast<uint8_t>(op.op);
            ir.argc = op.argc;
            ir.imm = op.imm;
            ir.ref = op.ref;
            if (op.symbol >= 0) {
                const expr::RuntimeSymbol& sym = r.program.symbols[static_cast<size_t>(op.symbol)];
                const RuntimeExpression::Target& target = r.targets[static_cast<size_t>(op.symbol)];
                switch (sym.kind) {
                    case expr::RuntimeSymbol::Kind::Port: {
                        const int port = m_ports.resolve(sym.name, r.element);
                        if (port < 0) {
                            skip(r, "expr.port",
                                 "port(\"" + sym.name +
                                     "\") is not declared; declare it as a number port (<ports> hp: number "
                                     "in)");
                            return;
                        }
                        if (m_ports.ports[static_cast<size_t>(port)].type !=
                            static_cast<uint8_t>(PortValueType::Number)) {
                            skip(r, "expr.port", "port(\"" + sym.name + "\") is not a number port");
                            return;
                        }
                        ir.ref = static_cast<uint32_t>(port);
                        break;
                    }
                    case expr::RuntimeSymbol::Kind::Layout:
                        ir.ref = target.element;
                        break;
                    case expr::RuntimeSymbol::Kind::LayerProp: {
                        PropId prop = target.row.prop;
                        uint8_t component = 0;
                        if (prop == 0xFFFF) {
                            std::vector<StyleDecl> scratch;
                            const std::vector<EffectDeclEntry>* seq = staticEffectSequence(
                                m_rules, m_dom, target.element, r.def->fileName, scratch);
                            std::string error;
                            ValueType type = ValueType::Float;
                            if (!resolveEffectAddress(m_writer, target.row.effect, target.row.param, seq,
                                                      error, prop, type)) {
                                skip(r, "expr.ref",
                                     "effect." + target.row.effect + "." + target.row.param + ": " + error);
                                return;
                            }
                            if (type == ValueType::Vec2 && target.row.param.size() > 2 &&
                                target.row.param.compare(target.row.param.size() - 2, 2, ".y") == 0) {
                                component = 1;
                            }
                        }
                        ir.ref = target.element;
                        ir.imm = static_cast<float>(static_cast<uint32_t>(prop) * 4u + component);
                        break;
                    }
                    case expr::RuntimeSymbol::Kind::OtherRow: {
                        RowIndex row;
                        if (!rowIndex(self.group, *r.def, target.element, target.row, row)) {
                            skip(r, "expr.ref", "layer(...)." + sym.name + " has no track in this timeline");
                            return;
                        }
                        ir.ref = rowRef(row);
                        break;
                    }
                    case expr::RuntimeSymbol::Kind::RuntimeRow: {
                        const size_t j = target.runtimeExpr;
                        if (j >= i || !m_rows[j].ok || m_rows[j].group != self.group) {
                            skip(r, "expr.ref", "layer(...)." + sym.name + " has no track in this timeline");
                            return;
                        }
                        if (m_index[j] != UINT32_MAX) {
                            ir.ref = m_index[j];  // Its program runs (RefValueAt in expression form).
                            depth = std::max(depth, m_depth[j] + 1);
                        } else {
                            ir.ref = rowRef(m_rows[j].row);  // Its program was skipped: its keys.
                        }
                        break;
                    }
                }
            }
            ops.push_back(ir);
        }
        if (depth > kExprMaxRefDepth) {
            skip(r, "expr.limit",
                 "it reads rows whose expressions read other rows more than " +
                     std::to_string(kExprMaxRefDepth) + " levels deep at run time");
            return;
        }
        IrTimelineExpr e{};
        e.group = self.group;
        e.member = self.row.member;
        e.track = self.row.track;
        e.component = self.row.component;
        e.flags = 0;
        e.opOffset = static_cast<uint32_t>(out.ops.size());
        e.opCount = static_cast<uint16_t>(ops.size());
        e.maxStack = r.program.maxStack;
        e.curveOffset = static_cast<uint32_t>(out.curves.size());
        e.curveCount = static_cast<uint16_t>(r.program.curves.size());
        e.inputMask = r.program.inputMask;
        e.portHistorySeconds = r.program.portHistorySeconds;
        e.seed = r.seed;
        e.index = r.index;
        out.ops.insert(out.ops.end(), ops.begin(), ops.end());
        for (const std::vector<IrExprKey>& keys : r.program.curves) {
            IrExprCurve c{};
            c.keyOffset = static_cast<uint32_t>(out.keys.size());
            c.keyCount = static_cast<uint32_t>(keys.size());
            out.curves.push_back(c);
            out.keys.insert(out.keys.end(), keys.begin(), keys.end());
        }
        m_index[i] = static_cast<uint32_t>(out.exprs.size());
        m_depth[i] = depth;
        out.exprs.push_back(e);
    }

    const RuleSet& m_rules;
    const DomTree& m_dom;
    const std::vector<uint32_t>& m_groupOf;
    const TimelineGroupBuilder& m_groups;
    const PortScopes& m_ports;
    IrWriter& m_writer;
    DiagnosticSink& m_sink;
    std::vector<ExpressionOutcome>* m_outcomes;
    std::vector<uint32_t> m_index;  // Per RuntimeExpression: its TEXP index, or UINT32_MAX.
    std::vector<uint32_t> m_depth;  // Its RefValueAt nesting.
    std::vector<Resolved> m_rows;
};

}  // namespace

void emitTimelineExprs(const std::vector<RuntimeExpression>& runtime, const RuleSet& ruleSet,
                       const DomTree& dom, const std::vector<uint32_t>& groupOfTimeline,
                       const TimelineGroupBuilder& groups, const PortScopes& ports, IrWriter& writer,
                       DiagnosticSink& sink, std::vector<ExpressionOutcome>* outcomes) {
    if (runtime.empty()) {
        return;
    }
    TexpEmitter emitter(ruleSet, dom, groupOfTimeline, groups, ports, writer, sink, outcomes);
    emitter.run(runtime);
}

}  // namespace xt::compile
