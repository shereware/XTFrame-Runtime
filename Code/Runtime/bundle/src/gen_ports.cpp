#include "xt/bundle/gen_ports.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "design_internal.h"
#include "xt/ir.h"
#include "xt/ir_pod.h"

/// generatePortHeader() (T-193, `xtframe-pack --gen-ports`). See the header for the naming rules.

namespace xt::bundle {

namespace {

/// Every character outside `[A-Za-z0-9_]` becomes `_`; a leading digit (or an empty name) gets a
/// leading `_`.
std::string identifier(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 1);
    for (char c : text) {
        out += std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ? c : '_';
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])) != 0) {
        out.insert(out.begin(), '_');
    }
    return out;
}

/// `snake_or-kebab.name` -> `SnakeOrKebabName`.
std::string pascalCase(std::string_view text) {
    std::string out;
    bool upper = true;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0) {
            upper = true;
            continue;
        }
        out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upper = false;
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])) != 0) {
        out.insert(out.begin(), '_');
    }
    return out;
}

/// C++ keywords a port or field name could plausibly collide with.
bool isCppKeyword(const std::string& name) {
    static const std::set<std::string> kKeywords = {
        "alignas",   "alignof", "and",     "asm",      "auto",     "bool",    "break",     "case",
        "catch",     "char",    "class",   "const",    "continue", "default", "delete",    "do",
        "double",    "else",    "enum",    "explicit", "export",   "extern",  "false",     "float",
        "for",       "friend",  "goto",    "if",       "inline",   "int",     "long",      "mutable",
        "namespace", "new",     "not",     "operator", "or",       "private", "protected", "public",
        "register",  "return",  "short",   "signed",   "sizeof",   "static",  "struct",    "switch",
        "template",  "this",    "throw",   "true",     "try",      "typedef", "typename",  "union",
        "unsigned",  "using",   "virtual", "void",     "volatile", "while",   "xor"};
    return kKeywords.count(name) != 0;
}

std::string cppName(std::string_view text) {
    std::string name = identifier(text);
    if (isCppKeyword(name)) {
        name += '_';
    }
    return name;
}

/// Makes `name` unique within `used` (`name`, `name_2`, `name_3`, ...).
std::string unique(std::string name, std::set<std::string>& used) {
    std::string candidate = name;
    for (int n = 2; used.count(candidate) != 0; ++n) {
        candidate = name + "_" + std::to_string(n);
    }
    used.insert(candidate);
    return candidate;
}

const char* typeName(PortValueType type) {
    switch (type) {
        case PortValueType::Number:
            return "number";
        case PortValueType::String:
            return "string";
        case PortValueType::Bool:
            return "bool";
        case PortValueType::Color:
            return "color";
        case PortValueType::Vec2:
            return "vec2";
        case PortValueType::Vec3:
            return "vec3";
        case PortValueType::Entity:
            return "entity";
        case PortValueType::Enum:
            return "enum";
        case PortValueType::Struct:
            return "struct";
        case PortValueType::List:
            return "list";
        case PortValueType::Texture:
            return "texture";
        case PortValueType::Event:
            return "event";
    }
    return "unknown";
}

const char* dirName(PortDir dir) {
    switch (dir) {
        case PortDir::In:
            return "in";
        case PortDir::Out:
            return "out";
        case PortDir::Call:
            return "call";
    }
    return "in";
}

/// A struct / list entry field's C++ declaration (with its default initializer).
std::string cppField(PortValueType type, const std::string& name) {
    switch (type) {
        case PortValueType::Number:
            return "float " + name + " = 0;";
        case PortValueType::Bool:
            return "bool " + name + " = false;";
        case PortValueType::Color:
            return "std::array<float, 4> " + name + "{};";
        case PortValueType::Vec2:
            return "std::array<float, 2> " + name + "{};";
        case PortValueType::Vec3:
            return "std::array<float, 3> " + name + "{};";
        case PortValueType::Entity:
        case PortValueType::Enum:
            return "std::uint32_t " + name + " = 0;";
        default:
            return "std::string " + name + ";";
    }
}

std::string csField(PortValueType type, const std::string& name) {
    switch (type) {
        case PortValueType::Number:
            return "public float " + name + ";";
        case PortValueType::Bool:
            return "public bool " + name + ";";
        case PortValueType::Color:
        case PortValueType::Vec2:
        case PortValueType::Vec3:
            return "public float[] " + name + ";";
        case PortValueType::Entity:
        case PortValueType::Enum:
            return "public uint " + name + ";";
        default:
            return "public string " + name + ";";
    }
}

/// The record type a struct / list port declares, named per the header's rules.
std::string recordTypeName(const PortRecord& port) {
    const std::string base = pascalCase(port.name);
    if (port.type != PortValueType::List) {
        return base;
    }
    if (base.size() > 1 && base.back() == 's') {
        return base.substr(0, base.size() - 1);
    }
    return base + "Item";
}

/// T-391: a primitive list entry's C++ / C# element type.
std::string cppElement(PortValueType type) {
    switch (type) {
        case PortValueType::Number:
            return "float";
        case PortValueType::Bool:
            return "bool";
        case PortValueType::Color:
            return "std::array<float, 4>";
        case PortValueType::Vec2:
            return "std::array<float, 2>";
        case PortValueType::Vec3:
            return "std::array<float, 3>";
        case PortValueType::Entity:
        case PortValueType::Enum:
            return "std::uint32_t";
        default:
            return "std::string";
    }
}

std::string csElement(PortValueType type) {
    switch (type) {
        case PortValueType::Number:
            return "float";
        case PortValueType::Bool:
            return "bool";
        case PortValueType::Color:
        case PortValueType::Vec2:
        case PortValueType::Vec3:
            return "float[]";
        case PortValueType::Entity:
        case PortValueType::Enum:
            return "uint";
        default:
            return "string";
    }
}

/// T-391 (27 §27.3.4): true when some port has a struct field of type list<T>.
bool hasNestedLists(std::span<const UiPortTable> tables) {
    for (const UiPortTable& table : tables) {
        for (const PortRecord& port : table.ports) {
            if (!port.fieldLists.empty()) {
                return true;
            }
        }
    }
    return false;
}

/// Writes the C++ record type of struct / list<struct> `rec` (named `name`) to `out`, after the
/// record types of its list<struct> fields (T-391: `std::vector<Row> rows;`).
void cppRecord(const PortRecord& rec, const std::string& name, std::set<std::string>& used,
               std::string& out) {
    std::vector<std::string> listTypes;
    size_t list = 0;
    for (const auto& [fieldName, fieldType] : rec.fields) {
        if (fieldType != PortValueType::List) {
            continue;
        }
        if (list >= rec.fieldLists.size()) {
            listTypes.emplace_back("std::string");
            continue;
        }
        const PortRecord& sub = rec.fieldLists[list++];
        if (sub.entryType != PortValueType{}) {
            listTypes.push_back(cppElement(sub.entryType));
            continue;
        }
        const std::string subName = unique(recordTypeName(sub), used);
        cppRecord(sub, subName, used, out);
        listTypes.push_back(subName);
    }
    out += "\nstruct " + name + " {\n";
    std::set<std::string> fields;
    list = 0;
    for (const auto& [fieldName, fieldType] : rec.fields) {
        const std::string field = unique(cppName(fieldName), fields);
        if (fieldType == PortValueType::List) {
            out += "    std::vector<" + listTypes[list++] + "> " + field + ";\n";
        } else {
            out += "    " + cppField(fieldType, field) + "\n";
        }
    }
    out += "};\n";
}

/// The C# counterpart of cppRecord() (`public Row[] rows;`).
void csRecord(const PortRecord& rec, const std::string& name, std::set<std::string>& used, std::string& out) {
    std::vector<std::string> listTypes;
    size_t list = 0;
    for (const auto& [fieldName, fieldType] : rec.fields) {
        if (fieldType != PortValueType::List) {
            continue;
        }
        if (list >= rec.fieldLists.size()) {
            listTypes.emplace_back("string");
            continue;
        }
        const PortRecord& sub = rec.fieldLists[list++];
        if (sub.entryType != PortValueType{}) {
            listTypes.push_back(csElement(sub.entryType));
            continue;
        }
        const std::string subName = unique(recordTypeName(sub), used);
        csRecord(sub, subName, used, out);
        listTypes.push_back(subName);
    }
    out += "    public struct " + name + "\n    {\n";
    std::set<std::string> fields;
    list = 0;
    for (const auto& [fieldName, fieldType] : rec.fields) {
        const std::string field = unique(pascalCase(fieldName), fields);
        if (fieldType == PortValueType::List) {
            out += "        public " + listTypes[list++] + "[] " + field + ";\n";
        } else {
            out += "        " + csField(fieldType, field) + "\n";
        }
    }
    out += "    }\n\n";
}

/// The comment after a port constant: `<type>, <dir>` plus the record / enum type it uses.
std::string portComment(const PortRecord& port, const std::string& typeRef) {
    std::string text = std::string(typeName(port.type)) + ", " + dirName(port.dir);
    if (!typeRef.empty()) {
        text += " (" + typeRef + ")";
    }
    if (!port.exposedName.empty()) {
        text += ", exposed as " + port.exposedName;
    }
    return text;
}

void generateCpp(std::span<const UiPortTable> tables, std::string& out) {
    out += "// Generated by `xtframe-pack --gen-ports cpp` from the bundle's declared ports. Do not edit.\n";
    out += "// The PortId values belong to this build's .xtir files; Document::lookupPort() resolves the\n";
    out += "// same names at run time.\n";
    out += "#pragma once\n\n#include <array>\n#include <cstdint>\n#include <string>\n";
    out += hasNestedLists(tables) ? "#include <vector>\n\n" : "\n";  // T-391
    out += "namespace xtui {\n\nusing PortId = std::uint32_t;\n";
    std::set<std::string> namespaces;
    for (const UiPortTable& table : tables) {
        const std::string ns = unique(cppName(table.uiId), namespaces);
        out += "\nnamespace " + ns + " {\n";
        std::set<std::string> used = {"kContractHash"};  // T-408: reserved for the constant below.
        if (!table.contract.empty()) {
            out += "\n// The UI's contract hash (manifest uis[].contract, T-408).\n";
            out += "inline constexpr char kContractHash[] = \"" + table.contract + "\";\n";
        }
        std::vector<std::string> typeRefs(table.ports.size());
        bool wroteType = false;
        for (size_t i = 0; i < table.ports.size(); ++i) {
            const PortRecord& port = table.ports[i];
            if (port.type == PortValueType::Enum && !port.enumValues.empty()) {
                const std::string name = unique(pascalCase(port.name), used);
                typeRefs[i] = name;
                out += "\nenum class " + name + " : std::uint32_t {";
                std::set<std::string> values;
                for (size_t v = 0; v < port.enumValues.size(); ++v) {
                    out += (v == 0 ? " " : ", ") + unique(cppName(port.enumValues[v]), values) + " = " +
                           std::to_string(v);
                }
                out += " };\n";
                wroteType = true;
            } else if (port.type == PortValueType::List && port.entryType != PortValueType{}) {
                // T-381: a list<number|string|...> declares no record type.
                typeRefs[i] = std::string("list<") + typeName(port.entryType) + ">";
            } else if (port.type == PortValueType::Struct || port.type == PortValueType::List) {
                const std::string name = unique(recordTypeName(port), used);
                typeRefs[i] = port.type == PortValueType::List ? "list<" + name + ">" : name;
                cppRecord(port, name, used, out);  // T-391: nested list<struct> fields first.
                wroteType = true;
            }
        }
        out += wroteType ? "\n" : "";
        for (size_t i = 0; i < table.ports.size(); ++i) {
            const PortRecord& port = table.ports[i];
            const std::string name =
                unique(cppName(port.qualifiedName.empty() ? port.name : port.qualifiedName), used);
            out += "constexpr PortId " + name + " = " + std::to_string(port.id) + ";  // " +
                   portComment(port, typeRefs[i]) + "\n";
        }
        out += "\n}  // namespace " + ns + "\n";
    }
    out += "\n}  // namespace xtui\n";
}

void generateCs(std::span<const UiPortTable> tables, std::string& out) {
    out += "// <auto-generated>\n";
    out += "// Generated by `xtframe-pack --gen-ports cs` from the bundle's declared ports. Do not edit.\n";
    out += "// The port ids belong to this build's .xtir files; Document::lookupPort() resolves the same\n";
    out += "// names at run time.\n";
    out += "// </auto-generated>\n";
    std::set<std::string> namespaces;
    for (const UiPortTable& table : tables) {
        const std::string ns = unique(pascalCase(table.uiId), namespaces);
        out += "\nnamespace Xtui." + ns + "\n{\n";
        std::set<std::string> used = {"Ports"};
        std::vector<std::string> typeRefs(table.ports.size());
        for (size_t i = 0; i < table.ports.size(); ++i) {
            const PortRecord& port = table.ports[i];
            if (port.type == PortValueType::Enum && !port.enumValues.empty()) {
                const std::string name = unique(pascalCase(port.name), used);
                typeRefs[i] = name;
                out += "    public enum " + name + " : uint\n    {\n";
                std::set<std::string> values;
                for (size_t v = 0; v < port.enumValues.size(); ++v) {
                    out += "        " + unique(pascalCase(port.enumValues[v]), values) + " = " +
                           std::to_string(v) + ",\n";
                }
                out += "    }\n\n";
            } else if (port.type == PortValueType::List && port.entryType != PortValueType{}) {
                // T-381: a list<number|string|...> declares no record type.
                typeRefs[i] = std::string("list<") + typeName(port.entryType) + ">";
            } else if (port.type == PortValueType::Struct || port.type == PortValueType::List) {
                const std::string name = unique(recordTypeName(port), used);
                typeRefs[i] = port.type == PortValueType::List ? name + "[]" : name;
                csRecord(port, name, used, out);  // T-391: nested list<struct> fields first.
            }
        }
        out += "    public static class Ports\n    {\n";
        std::set<std::string> constants = {"ContractHash"};  // T-408: reserved for the constant below.
        if (!table.contract.empty()) {
            out += "        /// <summary>The UI's contract hash (manifest uis[].contract).</summary>\n";
            out += "        public const string ContractHash = \"" + table.contract + "\";\n";
        }
        for (size_t i = 0; i < table.ports.size(); ++i) {
            const PortRecord& port = table.ports[i];
            const std::string name =
                unique(pascalCase(port.qualifiedName.empty() ? port.name : port.qualifiedName), constants);
            out += "        /// <summary>" + portComment(port, typeRefs[i]) + "</summary>\n";
            out += "        public const uint " + name + " = " + std::to_string(port.id) + ";\n";
        }
        out += "    }\n}\n";
    }
}

/// T-408: `text` as a JSON string literal (quotes included).
std::string contractQuote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::string contractType(const PortRecord& rec);

/// T-408: `struct{<field>:<type>,...}` of a struct / list<struct> record (list fields through
/// `fieldLists`, T-391).
std::string contractStruct(const PortRecord& rec) {
    std::string out = "struct{";
    size_t list = 0;
    for (size_t i = 0; i < rec.fields.size(); ++i) {
        const auto& [fieldName, fieldType] = rec.fields[i];
        out += (i == 0 ? "" : ",") + fieldName + ":";
        if (fieldType == PortValueType::List && list < rec.fieldLists.size()) {
            out += contractType(rec.fieldLists[list++]);
        } else {
            out += typeName(fieldType);
        }
    }
    return out + "}";
}

/// T-408: `enum(a|b|...)`.
std::string contractEnum(const PortRecord& rec) {
    std::string out = "enum(";
    for (size_t i = 0; i < rec.enumValues.size(); ++i) {
        out += (i == 0 ? "" : "|") + rec.enumValues[i];
    }
    return out + ")";
}

/// T-408: the full type spelling contractJson() writes.
std::string contractType(const PortRecord& rec) {
    switch (rec.type) {
        case PortValueType::Enum:
            return contractEnum(rec);
        case PortValueType::Struct:
            return contractStruct(rec);
        case PortValueType::List:
            if (rec.entryType == PortValueType::Enum) {
                return "list<" + contractEnum(rec) + ">";
            }
            if (rec.entryType != PortValueType{}) {
                return std::string("list<") + typeName(rec.entryType) + ">";
            }
            return "list<" + contractStruct(rec) + ">";
        default:
            return typeName(rec.type);
    }
}

/// T-408: `["a","b"]` of `names`, sorted and deduplicated.
std::string contractNames(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::string out = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        out += (i == 0 ? "" : ",") + contractQuote(names[i]);
    }
    return out + "]";
}

}  // namespace

std::string contractJson(const Document& doc) {
    std::vector<PortRecord> ports = doc.ports();
    const auto lookupName = [](const PortRecord& port) -> const std::string& {
        return port.qualifiedName.empty() ? port.name : port.qualifiedName;
    };
    std::stable_sort(ports.begin(), ports.end(),
                     [&](const PortRecord& a, const PortRecord& b) { return lookupName(a) < lookupName(b); });
    std::string out = "{\"ports\":[";
    for (size_t i = 0; i < ports.size(); ++i) {
        const PortRecord& port = ports[i];
        out += i == 0 ? "{" : ",{";
        out += "\"name\":" + contractQuote(lookupName(port));
        out += ",\"type\":" + contractQuote(contractType(port));
        out += ",\"dir\":" + contractQuote(dirName(port.dir));
        out += ",\"default\":" + contractQuote(doc.portText(port.id)) + "}";
    }
    const IrView& ir = doc.ir();
    std::vector<std::string> timelines;
    for (const IrTimelineGroup& group : ir.timelineGroups().groups()) {
        timelines.emplace_back(ir.string(group.name));
    }
    std::vector<std::string> states;
    for (const uint32_t name : ir.customStates().names()) {
        states.emplace_back(ir.string(name));
    }
    out += "],\"timelines\":" + contractNames(std::move(timelines));
    out += ",\"states\":" + contractNames(std::move(states)) + "}";
    return out;
}

std::string contractHash(const Document& doc) {
    // design::detail::contentHash() is `sha256:<64 hex>`; the contract keeps the first 16 digits.
    return design::detail::contentHash(contractJson(doc)).substr(7, 16);
}

bool parsePortsLanguage(std::string_view text, PortsLanguage& out) {
    if (text == "cpp" || text == "c++") {
        out = PortsLanguage::Cpp;
        return true;
    }
    if (text == "cs" || text == "csharp") {
        out = PortsLanguage::CSharp;
        return true;
    }
    return false;
}

std::string generatePortHeader(PortsLanguage language, std::span<const UiPortTable> tables) {
    std::string out;
    if (language == PortsLanguage::Cpp) {
        generateCpp(tables, out);
    } else {
        generateCs(tables, out);
    }
    return out;
}

}  // namespace xt::bundle
