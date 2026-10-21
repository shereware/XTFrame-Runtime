#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/document.h"

/// Strongly typed port headers (T-193; docs/design/23-host-integration.md §23.10 item 1,
/// 27-interaction.md §27.10): `xtframe-pack --gen-ports cpp|cs <out>` turns every packed UI's
/// declared-port table (the IR `PORT` block, Document::ports()) into constants a host can compile
/// against -- one namespace per UI with a `PortId` constant per port plus the struct / list entry
/// types and enums the ports declare.

namespace xt::bundle {

/// One UI's declared ports, as pack() reflected them (PackReport::portTables).
struct UiPortTable {
    /// Stable UI id.
    std::string uiId;
    /// Document::ports(), in PORT order.
    std::vector<PortRecord> ports;
};

/// Output language of generatePortHeader().
enum class PortsLanguage : uint8_t {
    Cpp = 0,     ///< `cpp`: a C++17 header, `namespace xtui::<ui>`.
    CSharp = 1,  ///< `cs`: a C# file, `namespace Xtui.<Ui>`.
};

/// `cpp` / `cs` (also `c++`, `csharp`); false for anything else.
bool parsePortsLanguage(std::string_view text, PortsLanguage& out);

/// The generated file for `tables` (LF line endings, trailing newline, deterministic: the same
/// tables always give the same bytes).
///
/// Names: a UI id becomes `lower_snake` (C++) / `Pascal` (C#) with every character outside
/// `[A-Za-z0-9_]` turned into `_`; a port constant is named after its lookup name
/// (PortRecord::qualifiedName, `.`/`/` -> `_`), a struct port's type after the port (`stats` ->
/// `Stats`), a list port's entry type after the port without a trailing `s` (`items` -> `Item`,
/// else `<Port>Item`), an enum port's type after the port. Names that clash get `_2`, `_3`, ...
/// Port type -> field type (C++ / C#): number float, string / texture std::string / string,
/// bool bool, color std::array<float, 4> / float[], vec2 / vec3 std::array<float, N> / float[],
/// entity and enum std::uint32_t / uint.
std::string generatePortHeader(PortsLanguage language, std::span<const UiPortTable> tables);

}  // namespace xt::bundle
