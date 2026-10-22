#pragma once

#include <array>
#include <cstddef>
#include <json.hpp>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "xt/bundle/ui_project.h"
#include "xt/types.h"

/// The design system model of a UI Project (Docs/design/35-design-system.md §35.2, §35.3, §35.5,
/// §35.6, §35.10, §35.11; Docs/tasks/T-305.md): the `design/` directory read into memory and
/// written back byte-stably, the entries and their links, the reference graph, validation, the
/// semantic diff and the upgrade of an existing project.
///
/// The design system has no version number of its own (36: its history is the project's history).
/// Every entry's `extra` keeps the keys this reader does not know (and the raw value of a known key
/// whose value has an unexpected shape, e.g. a localised `name` map or a decision's `status:
/// accepted`), and save() writes them back: known keys in a fixed order, then the unknown ones
/// alphabetically.
///
/// Paths use '/' separators. Entry references are written `<prefix>:<id>` (entryRef()).

namespace xt::bundle::design {

/// §35.3: the entry kinds and their reference prefixes (kindPrefix()).
enum class EntryKind {
    Reference,
    Text,
    Decision,
    Swatch,
    Group,
    Token,
    TypeRole,
    Convention,
    Motion,
    Symbol,
    Ui,
    Layer
};

/// §35.6: the stored status; `stale` is computed (validate()), never stored. A missing status reads
/// as Draft and Draft is not written back.
enum class EntryStatus { Draft, Approved, Deprecated };

/// One stored link (§35.3): `{ rel, to, note }`.
struct Link {
    std::string rel;
    std::string to;
    std::string note;
};

/// `source: { kind, ref, at }` (§35.3). Other keys of a `source` mapping are kept in the entry's
/// `extra["source"]`.
struct Source {
    std::string kind;
    std::string ref;
    std::string at;
};

/// `approved: { by, at }` (§35.3); only a person writes it.
struct Approval {
    std::string by;
    std::string at;
};

/// The fields every entry shares (§35.3).
struct Entry {
    EntryKind kind = EntryKind::Text;
    /// Unique within its kind; written with the kind prefix in references (`swatch:ember`).
    std::string id;
    /// Display name; empty = none.
    std::string name;
    EntryStatus status = EntryStatus::Draft;
    Source source;
    Approval approved;
    std::vector<std::string> tags;
    std::vector<Link> links;
    std::string note;
    /// Unknown keys (an object), written back after the known ones; null when there are none.
    nlohmann::json extra;
};

/// A swatch's place in a generated scale (`scale: { base, step, method }`).
struct SwatchScale {
    std::string base;
    int step = 0;
    std::string method;
};

/// One colour of `palette.yaml` (§35.3.3). `group` is the id of the group it is listed under
/// (empty for a swatch of the top-level `swatches` list).
struct Swatch {
    Entry base;
    std::string value;
    std::string role;
    std::string group;
    SwatchScale scale;
};

/// A group's scale generator (`scale: { base, steps, method }`).
struct GroupScale {
    std::string base;
    int steps = 0;
    std::string method;
};

/// One group of `palette.yaml` (§35.3.3). `name` mirrors `base.name` (load() sets both; save()
/// writes `name` when non-empty, else `base.name`). `swatches` lists the member ids in file order,
/// including the abbreviated id-only form.
struct Group {
    Entry base;
    std::string name;
    std::vector<std::string> swatches;
    GroupScale scale;
};

/// The metadata of one semantic variable (`tokens.yaml`, §35.3.4). `base.id` is the variable name
/// without `--`; the file key is `--<id>`. `from` is the short form of a `derived-from` link.
struct TokenMeta {
    Entry base;
    /// color | type | space | radius | border | shadow | motion | size | other.
    std::string category;
    std::string description;
    std::string from;
};

/// One type role of `type.yaml` (`roles:` list, §35.3.5).
struct TypeRole {
    Entry base;
    std::string family;
    std::vector<std::string> weights;
};

/// A text or decision entry (`design/text/**/*.md`, §35.3.2): YAML front matter plus Markdown.
struct TextEntry {
    Entry base;
    /// The file, relative to the design directory (`text/voice.md`).
    std::string path;
    std::string title;
    /// Everything after the closing `---` line of the front matter, byte for byte (the whole file
    /// when it has no front matter).
    std::string body;
    /// Front matter `section` (one scalar or a list).
    std::vector<std::string> sections;
};

/// A reference (`design/references/**/<file>` + `<file>.meta`, §35.3.1). `base.id` is the file's
/// path under `references/` without its extension (`moodboard/key-art-01`).
struct ReferenceMeta {
    Entry base;
    /// Meta `id`: the GUID (40 §40.2.1).
    std::string guid;
    /// The referenced file, relative to the design directory (`references/moodboard/a.png`);
    /// derived from where the `.meta` lies, not stored in it.
    std::string path;
    std::string title;
    /// own | licensed | internal-only | unknown.
    std::string license;
    /// Meta `content`: `sha256:<hex>`.
    std::string contentHash;
    /// A region note (`ref:<id>@<annotation>`), rect in fractions of the picture.
    struct Annotation {
        std::string id;
        std::array<float, 4> rect{0.0f, 0.0f, 0.0f, 0.0f};
        std::string note;
        std::vector<Link> links;
    };
    std::vector<Annotation> annotations;
    /// A cached extracted colour.
    struct Extracted {
        std::string value;
        float share = 0.0f;
    };
    std::vector<Extracted> extracted;
};

/// `design.yaml` (§35.2).
struct DesignInfo {
    std::string name;
    std::string status;
    std::vector<std::string> principles;
    std::vector<std::string> sections;
    std::vector<std::string> owners;
    std::vector<std::string> languages;
    /// Unknown keys (e.g. `version`); null when there are none.
    nlohmann::json extra;
};

/// The whole `design/` directory in memory.
struct DesignSystem {
    /// The design directory (`<project>/<shared.design>`), '/' separators; save() writes here.
    std::string rootDir;
    DesignInfo info;
    std::vector<Group> groups;
    std::vector<Swatch> swatches;
    std::vector<TokenMeta> tokens;
    std::vector<TypeRole> typeRoles;
    std::vector<TextEntry> texts;
    std::vector<TextEntry> decisions;
    std::vector<ReferenceMeta> references;
    /// `type.yaml` keys other than `roles` (scale, effects, ...); null when there are none.
    nlohmann::json typeExtra;
    /// T-305 addition: `palette.yaml` keys other than `groups`/`swatches` (`version`, `contrast`,
    /// ...), so a save keeps them; null when there are none.
    nlohmann::json paletteExtra;
    /// T-305 addition: `tokens.yaml` keys other than `tokens`; null when there are none.
    nlohmann::json tokensExtra;
};

/// The fixed `rel` vocabulary (§35.3).
enum class Rel { DerivedFrom, EvidenceFor, Illustrates, AppliesTo, Supersedes, ConflictsWith, SeeAlso };
inline constexpr std::array<std::string_view, 7> kRels = {
    "derived-from", "evidence-for", "illustrates", "applies-to", "supersedes", "conflicts-with", "see-also"};
/// The file spelling of `rel`.
std::string_view relName(Rel rel);
/// True when `text` is one of kRels; `*rel` (may be null) receives it.
bool parseRel(std::string_view text, Rel* rel = nullptr);

/// The reference prefix of a kind, without ':' (`swatch`, `ref`, `type`, ...).
std::string_view kindPrefix(EntryKind kind);
/// `<prefix>:<id>`, e.g. `swatch:ember`.
std::string entryRef(EntryKind kind, std::string_view id);
/// Splits a reference into kind and id. The id keeps an anchor: `ref:<group>/<name>@<annotation>`
/// (`@` only for ref), `text:<file>#<anchor>`, `type:<id>#<key>`, `layer:<ui>#<id>` (a layer always
/// has one). `token:--name` reads as id `name`. False for an empty text, a missing or unknown
/// prefix, an empty id or anchor, whitespace or control characters, a second `@`/`#`, or an anchor
/// on a kind that takes none. `kind`/`id` may be null.
bool parseEntryRef(std::string_view ref, EntryKind* kind, std::string* id);

/// §35.10: issue severities ("提示" = Info).
enum class Severity { Info, Warning, Error };

/// One lint finding (`design.*` ids of §35.10).
struct Issue {
    std::string id;
    Severity severity = Severity::Info;
    /// The entry reference it concerns (`swatch:ember`), or empty.
    std::string entry;
    std::string message;
};

/// The design directory of a project: `<rootDir>/<shared.design>`.
std::string designDir(const UiProject& project);

/// Reads `design.yaml`, `palette.yaml`, `tokens.yaml`, `type.yaml`, `text/**/*.md` (decisions:
/// `text/decisions/*.md`) and `references/**/*.meta`; a missing file reads as empty. kNotFound when
/// the design directory does not exist; kInvalidArgument (with `*error` naming the file) when a file
/// is malformed. `issues` is reserved for non-fatal findings (may be null).
Status load(const UiProject& project, DesignSystem& out, std::vector<Issue>* issues = nullptr,
            std::string* error = nullptr);

/// Writes the same files back into `ds.rootDir`: fixed key order, byte-stable, LF. Only files with
/// content are written (nothing is deleted); a text entry's body is written back unchanged.
/// kInvalidArgument without a rootDir, kIoError when a file cannot be written.
Status save(const DesignSystem& ds, std::string* error = nullptr);

/// §35.3.4 sync: every `:root` variable of `shared.tokens` without metadata gets a Draft TokenMeta
/// (category from its name) and an Info `design.tokenNoMeta`; metadata without a variable gets
/// `design.orphanMeta` (kept). `issues` may be null. Always kOk (a missing stylesheet has no
/// variables).
Status syncTokenMeta(DesignSystem& ds, const UiProject& project, std::vector<Issue>* issues);

/// One edge of the reference graph: `from` refers to / uses / derives from `to`.
struct Edge {
    std::string from;
    std::string to;
    /// A kRels value for stored links and `from`; "uses" for computed variable uses.
    std::string rel;
    /// True for an edge computed from the project files (variable -> UI / layer / symbol).
    bool computed = false;
};

/// The reference graph (§35.5).
struct Graph {
    /// Every node reference: the entries, their anchors (`text:x#slug`, `ref:x@a1`, `type:scale#m`),
    /// `ui:`, `layer:`, `symbol:`, `convention:` and `motion:` nodes of the project.
    std::set<std::string> nodes;
    std::vector<Edge> edges;
    /// Edge indexes by `to`.
    std::map<std::string, std::vector<size_t>> incoming;
    /// Edge indexes by `from`.
    std::map<std::string, std::vector<size_t>> outgoing;
    /// Reference id -> `sha256:<hex>` of the file as it is now (empty when it cannot be read).
    std::map<std::string, std::string> referenceHashes;

    bool has(std::string_view ref) const { return nodes.count(std::string(ref)) != 0; }
};

/// Builds the graph of `ds` in `project`: stored edges (links, `from`, annotation links,
/// `source.kind == extracted:<ref id>`) and computed ones (variable -> every UI / layer / symbol
/// whose `.xt`/`.xts`/symbol file has `var(--name`). kInvalidArgument for an unnamed project.
Status buildGraph(const UiProject& project, const DesignSystem& ds, Graph& graph,
                  std::string* error = nullptr);

/// What refers to a node, by kind of referrer (full references, sorted).
struct Usage {
    std::vector<std::string> uis;
    std::vector<std::string> layers;
    std::vector<std::string> symbols;
    std::vector<std::string> entries;
};
/// The direct referrers of `ref` (and of its anchors).
Usage usage(const Graph& graph, std::string_view ref);
/// The transitive referrers of `ref` (swatch -> variable -> UI / layer / symbol).
Usage impact(const Graph& graph, std::string_view ref);

/// §35.10 checks over the model and its graph: design.brokenLink, design.invalidRel (errors),
/// design.tokenNoDescription, design.nearDuplicateSwatch (ΔE2000 < 3), design.stale,
/// design.referenceNoLicense, design.orphan (info).
std::vector<Issue> validate(const DesignSystem& ds, const Graph& graph);

/// One semantic difference (§35.6).
struct DiffItem {
    enum class Op { Added, Removed, Changed };
    /// The entry reference (`swatch:ember`), or "design" for design.yaml.
    std::string entry;
    Op op = Op::Changed;
    /// Empty for a whole entry added/removed; else the field (`value`, `description`, `paragraph`,
    /// `link`, `content`, `status`, ...).
    std::string field;
    std::string before;
    std::string after;
    /// Swatch `value` changes: ΔE2000 between the two colours.
    float deltaE = 0.0f;
    /// Variable changes: the UIs that use it (from `graphOfB`).
    int affectedUis = 0;
};
/// The semantic diff from `a` to `b`: swatch values with ΔE2000, variable metadata with the UIs
/// using it, text bodies by paragraph (blank-line separated), references by content hash,
/// decisions by status, links added and removed.
std::vector<DiffItem> diff(const DesignSystem& a, const DesignSystem& b, const Graph* graphOfB = nullptr);

/// What upgradeDesign() did.
struct DesignUpgradeReport {
    /// The design directory was created.
    bool created = false;
    /// Files moved from the top-level `references/` (not counting `.meta` files).
    int referencesMoved = 0;
    /// Variables that got Draft metadata.
    int tokensMetaAdded = 0;
    std::vector<std::string> notes;
};

/// §35.11: creates the design directory when missing (`design.yaml`, `text/principles.md`,
/// `text/voice.md`, `references/`, a one-line `DESIGN.md` placeholder), moves the top-level
/// `references/*` into it once (a `.meta` is added to each moved file without one; `.gitkeep` and
/// the emptied directories are deleted) and syncs the variable metadata (`tokens.yaml` is written
/// only when something was added). An existing design directory is never overwritten.
/// kInvalidArgument for an unnamed project or a malformed design file, kIoError when a file cannot
/// be written or moved.
Status upgradeDesign(const UiProject& project, DesignUpgradeReport* report = nullptr,
                     std::string* error = nullptr);

}  // namespace xt::bundle::design
