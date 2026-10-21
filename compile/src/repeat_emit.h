#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"

/// `<repeat>` list templates -> the `RPT ` chunk (docs/design/27-interaction.md §27.3.4, 03
/// §3.12.4, 12 §12.7, docs/tasks/T-177.md).

namespace xt::compile {

struct PortScopes;

/// T-201 (27.2.3, 03 §3.12.3 SpawnPool): lowers every `spawn(<symbol>, ...)` action written in an
/// `on:` attribute of `tree` to a spawn pool: a `<repeat spawn="<symbol>" id="spawn-<n>">` child of
/// that element (absolutely positioned at its containing block's origin, 0 x 0, `pointer-events:
/// none`) whose template is `<use symbol="<symbol>" id="spawn-<n>-<symbol>"/>`; one pool per
/// element and symbol, in the order the symbols are first named. Run on the document and on every
/// symbol library before symbol expansion, so a spawn inside a symbol gets a pool per instance.
/// `counter` numbers the pools across calls.
void lowerSpawnPools(DomTree& tree, uint32_t& counter);

/// One `<repeat>` found by prepareRepeats().
struct RepeatDecl {
    /// DOM index of the `<repeat>` element.
    uint32_t element = 0;
    /// DOM index of its template root (its only child element).
    uint32_t templateRoot = 0;
    /// The `port` attribute (the list port's name as seen from the repeat element).
    std::string port;
    /// The `viewport` attribute without a leading `#` (empty = none).
    std::string viewport;
    /// `virtualize` (bare or `="true"`).
    bool virtualize = false;
    /// T-201: a spawn pool (`spawn="<symbol>"`, see lowerSpawnPools()): no list port.
    bool spawn = false;
    /// Source position of the `<repeat>` start tag.
    int line = 0;
    int col = 0;  ///< 1-based source column.
};

/// Per DOM node: the index into the RepeatDecl list of the `<repeat>` whose template contains it
/// (the innermost one, T-391), or kNoRepeat; per repeat: its list port (PORT index) and that port's
/// entry field names.
struct RepeatScopes {
    static constexpr uint32_t kNoRepeat = 0xFFFFFFFFu;
    /// Per DOM node.
    std::vector<uint32_t> repeatOf;
    /// Per repeat: PORT index of the list port, or -1 when it did not resolve (or, T-391, when the
    /// repeat reads a list field of the enclosing repeat's entry: see itemField).
    std::vector<int> listPort;
    /// Per repeat: the list entry's field names.
    std::vector<std::vector<std::string>> fields;
    /// T-391 (27 §27.3.4): per repeat: the enclosing repeat (whose template holds this `<repeat>`),
    /// or kNoRepeat.
    std::vector<uint32_t> parent;
    /// T-391: per repeat: the index of the list field among the enclosing repeat's entry fields
    /// for `port="item(<field>)"`, else -1.
    std::vector<int> itemField;
    /// T-391: per repeat: the entry type (IrPort::entryType: 0 = struct) and the range of the entry
    /// fields in PortBuilder::fields (valid once resolved()).
    std::vector<uint8_t> entryType;
    std::vector<uint32_t> fieldOffset;

    /// The repeat whose template contains DOM node `node`, or kNoRepeat.
    uint32_t repeatAt(uint32_t node) const { return node < repeatOf.size() ? repeatOf[node] : kNoRepeat; }
    /// T-391: true when repeat `r`'s list (a port or a list field) resolved.
    bool resolved(uint32_t r) const {
        return r < listPort.size() && (listPort[r] >= 0 || (r < itemField.size() && itemField[r] >= 0));
    }
};

/// Validates every `<repeat>` of `dom` (after symbol expansion, before ports and triggers) and
/// moves its `item-enter` / `item-exit` attributes onto the template root as `on:item-enter` /
/// `on:item-exit` (compiled by emitTriggers in the template's scope). A `<repeat>` needs `port`
/// and `id` and exactly one child element (the template); ports declared directly inside a
/// template and `on:item-enter/exit` anywhere but on a template root are errors. T-391: a repeat
/// inside another repeat's template is allowed (not virtualized, no spawn pool). T-380
/// (27 §27.3.4): a port declared by a symbol instance inside the template (the declaring element
/// lies strictly below an instance root of `instanceRoots` that is the template root or inside it)
/// is the symbol's and allowed; every repeat instance gets its own copy at run time. Fills `decls`
/// and `scopes.repeatOf`. Returns false if any error was reported.
bool prepareRepeats(DomTree& dom, std::string_view fileName, DiagnosticSink& sink,
                    std::vector<RepeatDecl>& decls, RepeatScopes& scopes,
                    const std::vector<uint32_t>& instanceRoots = {});

/// Resolves each repeat's `port` (it must be a declared `list<struct{...}>` port or, T-381, a
/// `list<number|string|...>` port, whose entries have no fields) into `scopes.listPort` /
/// `scopes.fields`. T-391: inside another repeat's template, `item(<field>)` names a list field of
/// the enclosing repeat's entry (`scopes.itemField`). Returns false if any error was reported.
bool resolveRepeatPorts(const std::vector<RepeatDecl>& decls, const PortScopes& ports,
                        const PortBuilder& portTable, const IrWriter& writer, std::string_view fileName,
                        DiagnosticSink& sink, RepeatScopes& scopes);

/// Builds the `RPT ` rows: the template root's `item-enter` / `item-exit` triggers (from the
/// finished TRIG tables) and the `viewport` element. Returns false if any error was reported.
bool emitRepeats(const DomTree& dom, const std::vector<RepeatDecl>& decls, const RepeatScopes& scopes,
                 const TriggerBuilder& triggers, std::string_view fileName, DiagnosticSink& sink,
                 std::vector<IrRepeat>& out);

}  // namespace xt::compile
