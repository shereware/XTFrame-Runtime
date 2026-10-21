#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Context-free syntax checks for the editor (docs/design/17-editor.md §17.2.1 "觸發、綁定、port 以屬性
/// 原文讀寫；用 xt_compile 的解析器驗證", docs/tasks/T-180.md). These run the same lexers/parsers the
/// compiler uses (trigger_emit.cpp, bind_emit.cpp) but without a document: names that can only be
/// resolved against a compiled document (timelines, markers, ports, focus targets, custom-state
/// limits) are not checked here -- a full compile reports those. Each returns true when the text
/// is well-formed, else false with a one-line English reason in `error`.

namespace xt::compile {

/// `<event>` of an `on:<event>` attribute / `.xts` `on-<event>` declaration (27.2.2), e.g.
/// `hover`, `key(Enter)`, `port(hp)`, `timeline(intro).end`, `state(Low).enter`.
bool validateTriggerEvent(std::string_view event, std::string& error);

/// The action list of a trigger (27.2.3): `;`/`,`-separated statements, `then` sequences,
/// `if <cmp>(...) ... [else ...]`, with each action's name and argument shape checked.
bool validateTriggerActions(std::string_view actions, std::string& error);

/// `<target>` of a `bind:<target>` attribute (27.3.2): `text`, `src`, `visible`, `checked` (T-204;
/// a full compile also requires an `<input type="checkbox">`), `state.<Name>`, `class.<name>`,
/// `variant.<Prop>` or a known non-shorthand CSS property.
bool validateBindingTarget(std::string_view target, std::string& error);

/// T-382 (27 §27.3.4, the T-380 ruling): the same for a `bind:` on a `<use>`, whose symbol declares
/// the in-ports `instanceInPorts` (symbolInPorts()): a target naming one of them is a port target
/// and wins over a property of the same name; anything else is checked as above.
bool validateBindingTarget(std::string_view target, std::span<const std::string> instanceInPorts,
                           std::string& error);

/// T-382: the ports an instance of `symbol` takes as `bind:<port>` targets -- the `port:` attributes
/// declared inside the `<symbol name="<symbol>">` definition of `xtText` (a document or a symbol
/// library), outside nested `<use>` elements, whose direction is `in` (the default) and whose type
/// is not `event`. Returns false (and leaves `ports` empty) when `xtText` does not parse or defines
/// no such symbol. `symbolHrefs`, when given, receives the `<link rel="symbols" href>` values of
/// `xtText` (whether or not the symbol was found), so the caller can look in those libraries next.
bool symbolInPorts(std::string_view xtText, std::string_view symbol, std::vector<std::string>& ports,
                   std::vector<std::string>* symbolHrefs = nullptr);

/// The expression of a `bind:` attribute (27.3.2-27.3.3): a head (port, `#id.prop`, `format()`,
/// `select()`, `step()`) followed by `|`-chained converters from the fixed catalog or `host.<name>()`.
bool validateBindingExpression(std::string_view expression, std::string& error);

/// The value of a `port:<name>` attribute (27.3.1): `<type> [in|out|call]`.
bool validatePortDeclaration(std::string_view declaration, std::string& error);

/// A port name (the `<name>` of `port:<name>`): `[A-Za-z_][A-Za-z0-9_]*`.
bool validatePortName(std::string_view name, std::string& error);

/// T-355 (29 §29.3): true when the `.xts` parser knows the declaration name `name` -- a row of the
/// compiler's property table (05), a `transition-*` longhand or a trigger declaration (`on-<event>`,
/// `state-enter`, `state-exit`). Custom properties (`--name`) are not included.
bool isKnownStyleProperty(std::string_view name);

}  // namespace xt::compile
