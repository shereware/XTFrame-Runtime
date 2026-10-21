#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "repeat_emit.h"
#include "symbol_expand.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/parse_svg.h"

/// Declared ports and data bindings -> the `PORT` and `BIND` chunks (docs/design/27-interaction.md
/// §27.3, 03 §3.12.3, 05 §5.9.1, 12 §12.7, docs/tasks/T-175.md).

namespace xt::compile {

/// The compile-time port table: every `port:` declaration plus the port scope of each element
/// (the innermost symbol instance root strictly above it, or kNoPortScope). Shared with
/// trigger_emit, which resolves `port(<name>)` events, `emit(<port>)` and `if` arguments with it.
struct PortScopes {
    /// Index-aligned with the PORT chunk.
    std::vector<IrPort> ports;
    /// Declared names, index-aligned with `ports`.
    std::vector<std::string> names;
    /// Per DOM node: its scope (an instance root's DOM index) or kNoPortScope.
    std::vector<uint32_t> scopeOf;
    /// `<element id>.<port>` and `<instance id>.<port>` (exposed ports) -> port index.
    std::map<std::string, uint32_t, std::less<>> qualified;

    /// Resolves `name` as seen from DOM node `element`: a qualified `[#]<id>.<port>` name, else
    /// the element's own scope, then each enclosing scope, then the document level. -1 when no
    /// port matches.
    int resolve(std::string_view name, uint32_t element) const;
    /// The index of `<port>_max` in port `port`'s own scope, or -1.
    int maxCompanion(uint32_t port) const;
};

/// Compiles every `port:<name>="<type> [in|out|call]"` attribute (and `expose="false"`) into
/// `out` and fills `scopes`. Invalid types, names or directions and a name declared twice in one
/// scope are errors. Returns false if any error was reported.
/// T-243b: `boneIds` (may be null; emitInlineSvgs()' InlineSvgRig::boneIds) adds the implicit
/// bone ports after the system ports (declareBonePorts()).
bool emitPorts(const DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances, IrWriter& writer,
               std::string_view fileName, DiagnosticSink& sink, PortBuilder& out, PortScopes& scopes,
               const std::vector<std::pair<std::string, int>>* boneIds = nullptr);

/// T-385 (27 §27.3.2): renames every `bind:state.Disabled` attribute to `bind:disabled` (the
/// built-in state, so no custom state of that name is registered). Call before emitPorts().
/// T-408: every binding alias of xt/compile/aliases.h is renamed this way, each use reported
/// through DiagnosticSink::deprecated() at `fileName` and the element's position.
void normalizeBindAliases(DomTree& dom, std::string_view fileName, DiagnosticSink& sink);

/// T-385 (27 §27.3.2): the literal port values on `<use>`: each ExpandedSymbolInstance::useAttrs
/// entry naming a port the instance declares directly (scope = the instance root) becomes that
/// port's default in `out.defaults` (replacing a declared one), parsed by the port's type (String
/// as written; List a JSON array, Struct a JSON object; Texture / Entity rejected), and is removed
/// from the instance root's attributes. Naming an out / call / event port, a value that does not
/// parse, or a `bind:<same name>` on the same `<use>` is an error. With `strict`, an attribute
/// that is neither a port nor a standard element attribute warns `unknown attribute "<name>" on
/// <use symbol="<X>">: not a port of <X>`. Call right after emitPorts(). Returns false on an error.
bool applyUseLiterals(DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances,
                      const PortScopes& scopes, IrWriter& writer, std::string_view fileName,
                      DiagnosticSink& sink, PortBuilder& out, bool strict);

/// T-200 (27.12, 23.10 item 4): declares, after the `port:` ones, each implicit document-level
/// system port a `bind:` or `on:` value of `dom` names as a whole word: `system.loading`,
/// `system.paused`, `system.resumed`, `system.disconnected` (event) and `system.language`,
/// `system.platform`, `system.inputDevice` (string), then `system.back` (event, T-240) and
/// `system.reducedMotion` (string, T-330), in that order. Each is an `in` port on DOM
/// element 0 with no scope, resolvable by its full name. Called by emitPorts().
void declareSystemPorts(const DomTree& dom, IrWriter& writer, PortBuilder& out, PortScopes& scopes);

/// T-243b (32 §32.8): declares, after the system ports, the implicit out ports of every named
/// bone in `boneIds` (id, `.xt` line), in order: `<id>.world` (vec2) then `<id>.angle` (number),
/// each on DOM element 0 with no scope, resolvable by its full name (so a `bind:` can read them;
/// the run time publishes them). A name already declared (an earlier bone with the same id, or
/// another port) warns and is skipped: only the first is declared. Called by emitPorts().
void declareBonePorts(const std::vector<std::pair<std::string, int>>& boneIds, IrWriter& writer,
                      std::string_view fileName, DiagnosticSink& sink, PortBuilder& out, PortScopes& scopes);

/// Compiles every `bind:<target>="<expr>"` attribute into `out`. `states` is the finished custom
/// state table (trigger_emit registers every `bind:state.<Name>` name). Unknown targets,
/// converters, ports and malformed expressions are errors. Returns false if any error was
/// reported. T-177: inside a `<repeat>` template (`repeats`, may be null) an expression may start
/// at the repeat entry -- `item(<field>)`, `index`, `count` (kBindRepeatItem) -- and those three
/// may also be converter arguments (ConverterArg Item / Index / Count). T-243 (32 §32.4):
/// `svgBinds` (may be null; emitInlineSvgs()' InlineSvgRig::binds) adds the `bind:` /
/// `world-anchor` attributes of inline `<svg>` content, each a record on its `<svg>` element with
/// IrBinding::sceneNode set. There a `bind:` target can only be a property the scene node takes
/// (appliesToSvgNode()); anything else is an error.
bool emitBindings(const DomTree& dom, const std::vector<ExpandedSymbolInstance>& instances,
                  const PortScopes& scopes, const CustomStateBuilder& states, IrWriter& writer,
                  std::string_view fileName, DiagnosticSink& sink, BindingBuilder& out,
                  const RepeatScopes* repeats = nullptr,
                  const std::map<uint32_t, std::vector<SvgBindAttr>>* svgBinds = nullptr);

/// T-201: compiles the converter segments of a `set(<port>, event.value | <converters>)` value
/// (`converters` is the text after the first `|`, e.g. `range(0, 1, 0, 100) | round`) as seen from
/// DOM node `element` (port arguments resolve in its scope). `tween` and `delay` keep state a
/// trigger does not have and are rejected. False with `err` on anything invalid.
bool compileConverterChain(const DomTree& dom, const PortScopes& scopes, IrWriter& writer, uint32_t element,
                           std::string_view converters, std::vector<IrConverterStep>& steps,
                           std::string& err);

/// T-411 (27 §27.2.3): a trigger action value that starts at a bind source, compiled by
/// compileActionValue(). `steps` are the converter steps (for an Item head they begin with the
/// step that reads the entry: `item(<field>)`, `index` or `count`, as a kBindRepeatItem binding).
struct ActionValue {
    enum class Head : uint8_t {
        Number,  ///< A constant number (`true` / `false` are 1 / 0) followed by converters.
        String,  ///< A quoted constant followed by converters.
        Port,    ///< `port(<name>)`: that port's value when the action runs.
        Item,    ///< `item(<field>)`, `item()`, `repeat.item(...)`, `repeat.index`, `repeat.count`.
    };
    Head head = Head::Number;
    float number = 0;
    std::string text;  ///< Head::String: the unquoted constant.
    int port = -1;     ///< Head::Port: the PORT index.
    std::vector<IrConverterStep> steps;
};

/// T-411 (27 §27.2.3): compiles a `set(<port>, <value>)` value that is a bind-source expression,
/// as seen from DOM node `element`: a head -- `port(<name>)` (an in or out port of the scope; not
/// a call or event port), a repeat term inside a `<repeat>` template (`item(<field>)`, `item()`,
/// `repeat.item(...)`, `repeat.index`, `repeat.count`; `repeats` may be null) or a constant
/// (number, `true` / `false`, quoted string) -- optionally followed by `| <converter> ...` (27
/// §27.3.3, the bind converter parser; `tween` / `delay` rejected). Converter arguments may also
/// name a port as `port(<name>)`. `outer.*` is not accepted. False with `err` on anything invalid.
bool compileActionValue(const DomTree& dom, const PortScopes& scopes, const RepeatScopes* repeats,
                        IrWriter& writer, uint32_t element, std::string_view text, ActionValue& out,
                        std::string& err);

}  // namespace xt::compile
