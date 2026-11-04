#pragma once

#include <string>
#include <string_view>
#include <vector>

/// T-587 (Docs/design/54-source-format-v2.md §54.4): the `<ports>` section grammar, one
/// declaration per entry: `<name>: <type> [in|out|call] [= <default>] [hidden]`. The type,
/// direction and default are exactly the text of a v1 `port:<name>` attribute value (bind_emit's
/// parsePortType() reads it); `/* */` and `//` comments are dropped; a declaration may span lines
/// (a new one starts at a line whose first word, outside any bracket, is `<name>:`).

namespace xt::compile {

class DiagnosticSink;

/// One `<ports>` declaration.
struct PortsDecl {
    /// The port name (before the ':').
    std::string name;
    /// `<type> [in|out|call] [= <default>]` with `hidden` removed and whitespace runs folded to
    /// one space: the value of the equivalent v1 `port:<name>` attribute.
    std::string value;
    /// The trailing `hidden` word was present (= v1 `expose:false`).
    bool hidden = false;
    /// The comment lines directly above the declaration, verbatim (one per line, `//` or `/* */`).
    std::vector<std::string> comments;
    /// 1-based file line and column of the name.
    int line = 0;
    int col = 0;
};

/// Splits a `<ports>` section's raw `text` into declarations. `line`/`col` are the file position
/// of the text's first character (columns of later lines are file columns already). Reports an
/// error (into `sink`, file `fileName`) for text that is no `<name>:` declaration and returns
/// false then; the declarations that did parse are still in `out`.
bool parsePortsSection(std::string_view text, int line, int col, std::string_view fileName,
                       DiagnosticSink& sink, std::vector<PortsDecl>& out);

/// Splits a declaration value (PortsDecl::value) into its type, direction (`in`/`out`/`call`, or
/// "" when not written) and default text (after `=`, or "" with `hasDefault` false), the way
/// emitPorts() reads a `port:` attribute.
void splitPortsValue(std::string_view value, std::string& type, std::string& dir, std::string& defaultText,
                     bool& hasDefault);

}  // namespace xt::compile
