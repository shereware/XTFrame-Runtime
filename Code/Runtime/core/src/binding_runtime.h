#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/document.h"
#include "xt/ir_pod.h"
#include "xt/timeline.h"
#include "xt/types.h"
#include "xt/value.h"

/// Port values and data bindings (Docs/design/27-interaction.md §27.3, 03 §3.12.3, 04 §4.10.4,
/// 13 §13.8.1, Docs/tasks/T-175.md). Private to core; Document owns one BindingRuntime behind a
/// unique_ptr and forwards its public port API here. T-389 (27 §27.4.1): a port change marks the
/// bindings that read it dirty, and Document::update() evaluates each dirty binding once (however
/// often the port was set in between); port listeners and `port(<name>)` triggers still run at
/// once, and get() reads the stored value at once. `tween` / `delay` steps keep their bindings
/// ticking from Document::update(); `#Other.<prop>` bindings are evaluated again after layout.

namespace xt {

class Document;

class BindingRuntime {
public:
    /// No port index (PORT indices are 0-based; PortId = index + 1).
    static constexpr uint32_t kNoPort = 0xFFFFFFFFu;

    explicit BindingRuntime(Document& doc);
    ~BindingRuntime();
    BindingRuntime(const BindingRuntime&) = delete;
    BindingRuntime& operator=(const BindingRuntime&) = delete;

    /// Builds the port table (PORT) and the bindings (BIND), indexes the `port(<name>)` triggers
    /// (TRIG), then evaluates every binding once. Called by Document::loadInternal after the
    /// element tree and TriggerRuntime::load().
    void load();
    /// Evaluates the bindings a port change marked dirty (T-389), then advances the ticking ones
    /// (`tween`, `delay`). Called at the start of Document::update.
    void tick();
    /// T-389: evaluates every binding marked dirty since the last call, once each.
    void flushDirty();
    /// T-399: true while port changes wait for the next flushDirty() (ElementRef::focus() then
    /// accepts a `display: none` element as a pending focus).
    bool hasDirty() const { return !m_dirty.empty(); }
    /// Evaluates the `#Other.<prop>` bindings against the fresh layout. True when one of them
    /// applied a change (Document::update then resolves and lays out once more).
    bool afterLayout();
    /// T-399 (08 §8.18): re-evaluates and re-applies (even when the result did not change) the
    /// `bind:value` bindings of `node` (instance copies included). False when it has none.
    bool reapplyValue(NodeId node);
    /// T-399: re-applies every `bind:value` binding (Document::restoreState).
    void reapplyAllValues();
    /// T-399: after a style pass, re-applies the `bind:value` bindings whose element went from
    /// `display: none` (on it or an ancestor) to displayed since the last call.
    void reapplyShownValues();

    /// The localization provider or language changed: re-evaluates the bindings with a
    /// `localize` step.
    void relocalize();
    /// A TRIG `if` condition: `value` is the event value; for a `port(<name>)` event `port` is its
    /// PORT index and the port's current value is compared instead.
    bool evalCondition(std::span<const IrConverterStep> steps, float value, uint32_t port);
    /// An `emit(<port>)` action on `node` (HostEvent, port listeners, `call` handlers).
    void emit(NodeId node, const IrTriggerAction& action);
    /// T-201: a `set(<port>, <value>)` action on `node`; `value` / `port` are the event's (as for
    /// evalCondition). An `in` port is set like Document::setPort; an `out` port stores the value
    /// and emits it (as `emit(<port>)`).
    void setAction(NodeId node, const IrTriggerAction& action, float value, uint32_t port);

    // Document's public port API (see xt/document.h).
    PortId lookup(std::string_view name) const;
    std::vector<PortRecord> ports() const;
    PortRecord info(PortId port) const;
    Status set(PortId port, const Value& value);
    Status setText(PortId port, std::string_view text);
    Value get(PortId port);
    std::string text(PortId port) const;
    Result<Value> call(PortId port, std::span<const Value> args);
    Status setCallHandler(PortId port, PortCallHandler handler);
    Status listSet(PortId port, std::span<const Value> entries);
    Status listInsert(PortId port, uint32_t index, const Value& entry);
    Status listRemove(PortId port, uint32_t index);
    Status listUpdate(PortId port, uint32_t index, const Value& entry);
    Result<uint32_t> registerConverter(std::string_view name, ConverterFn fn);
    /// T-244 (32 §32.8): the implicit output ports of bone `bone` (`<id>.world`, `<id>.angle`):
    /// stores `screen` / `angleDeg` and notifies (bindings, listeners, triggers) only when a value
    /// moved by more than 1e-3. No-op for a bone without ports.
    void publishBone(NodeId bone, Vec2 screen, float angleDeg);
    /// T-200 (13.8.3): Document::setWorldAnchor.
    Status setWorldAnchor(PortId port, float x, float y, float depth, bool onScreen);
    /// T-200 (Context::hotReload): appends `port <qualified name>` to `missing` for every
    /// non-event port of `from` (another Document's runtime) this one lacks or declares with a
    /// different type, or whose enum value it does not have.
    void missingPorts(const BindingRuntime& from, std::vector<std::string>& missing) const;
    /// T-200: takes over every port value of `from` (matched as missingPorts() does; struct fields
    /// and list entries by field name, strings re-interned), rebuilds `<repeat>` lists, then
    /// evaluates every binding as load() does. No port listener or trigger fires.
    void adoptPorts(const BindingRuntime& from);
    uint32_t addListener(PortListener listener);
    void removeListener(uint32_t id);

    // T-177 (27.3.4): `<repeat>` instances (RepeatController). A template element's BIND records
    // stay inert; every instance gets its own copy, bound to the copied node and to one entry of
    // the repeat's list port.
    /// No list entry (a pooled or exiting instance: its bindings are not evaluated).
    static constexpr uint32_t kNoItem = 0xFFFFFFFFu;
    /// Copies BIND record `irBinding` onto `node` for entry `item` of list port `listPort` (a PORT
    /// index) and registers it with its source ports; returns the binding index. Not evaluated.
    /// T-380: `portMap` (template port slot -> the instance's copy, see addInstancePort()) redirects
    /// every port the copy reads or targets.
    uint32_t addInstanceBinding(uint32_t irBinding, NodeId node, uint32_t listPort, uint32_t item,
                                std::span<const std::pair<uint32_t, uint32_t>> portMap = {});
    /// T-380 (27 §27.3.4): a repeat instance's own copy of template port `templatePort` (a PORT
    /// index of a port declared inside a `<repeat>` template by a symbol): a new slot holding the
    /// port's declared default, declared on `element` (the copy of the declaring element) and
    /// exposed through `scope` (the copy of the symbol instance root, kInvalidNode = none). Its
    /// names follow the copies' current path ids (`Item/3/Cell/T.title`, `Item/3/Cell.title`), so
    /// lookup() finds it after a rename. Returns the slot index (PortId = index + 1); a released
    /// slot's index is reused.
    uint32_t addInstancePort(uint32_t templatePort, NodeId element, NodeId scope);
    /// T-380: points the `<name>_max` companion of every instance copy in `portMap` at the same
    /// instance's copy of the template's companion.
    void linkInstancePorts(std::span<const std::pair<uint32_t, uint32_t>> portMap);
    /// T-380: a pooled instance's port copy goes back to the template's declared default (no
    /// notification).
    void resetInstancePort(uint32_t slot);
    /// T-380: releases an instance's port copy (its PortId stops resolving; the index may be reused).
    void removeInstancePort(uint32_t slot);
    /// Points an instance binding at another entry (kNoItem = none); the next evaluation re-applies.
    void setInstanceItem(uint32_t binding, uint32_t item);
    /// Unregisters an instance binding; its index may be reused.
    void removeBinding(uint32_t binding);
    /// T-260b (tests): the instance bindings currently registered (added and not removed).
    size_t liveInstanceBindings() const {
        size_t n = 0;
        for (const Binding& b : m_bindings) {
            n += b.instance && !b.free ? 1 : 0;
        }
        return n;
    }
    /// Evaluates one binding now.
    void evaluateBinding(uint32_t binding, bool initial) { evaluate(binding, initial); }
    /// Registers `node`'s `port(<name>)` triggers (its NodeCommon::triggerTable). T-380: a port in
    /// `portMap` (a repeat instance's template port -> copy) registers on the copy.
    void bindTriggerNode(NodeId node, std::span<const std::pair<uint32_t, uint32_t>> portMap = {});
    /// Drops `node` from every port's trigger list.
    void forgetTriggerNode(NodeId node);
    /// Entry `item` of list port `listPort` (a Struct Value into the port's storage), or a Float 0.
    Value entryValue(uint32_t listPort, uint32_t item) const;

    // T-391 (27 §27.3.4): nested lists. A struct field may be a list<T>; a `<repeat
    // port="item(<field>)">` inside another repeat's template expands, per instance of the
    // enclosing repeat, a nested list slot: a List slot holding a copy of that field of the
    // enclosing entry, named `<enclosing repeat id>/<i>.<field>` for lookup(), whose list API
    // writes back into the enclosing entry.
    /// A new nested list slot for field `field` (an index among the entry fields) of entry `item`
    /// of list slot `parent`; `repeatNode` is the enclosing `<repeat>` element (its id names the
    /// slot). Filled from the entry without notification. kNoPort when `parent` is no
    /// list<struct> or the field is no list.
    uint32_t addNestedList(uint32_t parent, uint32_t field, uint32_t item, NodeId repeatNode);
    /// T-391: the enclosing entry of nested slot `slot` moved to `item` (kNoItem: none, e.g. pooled).
    void setNestedItem(uint32_t slot, uint32_t item);
    /// T-391: copies the enclosing entry's field into nested slot `slot` again, without
    /// notification; true when its content changed.
    bool refreshNestedList(uint32_t slot);
    /// T-391: releases a nested list slot (its index may be reused).
    void removeNestedList(uint32_t slot);
    /// T-391: the enclosing repeat's list slot and entry of an instance binding (`outer.item` /
    /// `outer.index`); kNoPort / kNoItem = none.
    void setInstanceOuter(uint32_t binding, uint32_t outerPort, uint32_t outerItem);
    /// T-391: true when instance binding `binding` reads `outer.item(...)` / `outer.index`.
    bool usesOuter(uint32_t binding) const {
        return binding < m_bindings.size() && !m_bindings[binding].free && m_bindings[binding].usesOuter;
    }
    /// The entry count of list port `listPort`.
    uint32_t entryCount(uint32_t listPort) const;
    /// T-270d: the largest number of distinct new runtime strings any one binding, port or
    /// condition has interned so far (what the kMaxDistinctStrings guard counts; FrameStats).
    uint32_t maxDistinctStrings() const { return m_maxDistinctStrings; }

    /// A value flowing through a converter chain.
    struct BVal {
        enum class Kind : uint8_t { None, Num, Str, Color, Len, Vec2, Vec3, Struct, List };
        Kind kind = Kind::None;
        bool isBool = false;  ///< Num only: a comparison / bool port result.
        double num = 0;
        std::string str;
        Value v{};  ///< Color / Len / Vec2 / Vec3 / Struct / List.
    };

private:
    /// T-270c: new runtime strings one source interned (values that need a StringId); one warning
    /// past kMaxDistinctStrings.
    struct StringGuard {
        uint32_t distinct = 0;
        bool warned = false;
    };
    static constexpr uint32_t kMaxDistinctStrings = 1024;
    /// T-391 (27 §27.3.4): the entry type of a list<T> struct field (recursively).
    struct ListSchema {
        PortValueType entryType = static_cast<PortValueType>(0);  // 0 = struct.
        std::vector<StringId> fieldNames;
        std::vector<PortValueType> fieldTypes;
        std::vector<std::string> enumValues;                  // An Enum entry's value names.
        std::vector<std::shared_ptr<const ListSchema>> subs;  // Parallel to fieldNames (List fields).
    };
    /// T-391: owned, immutable storage behind one nested List Value (the item arrays it and its
    /// entries point to; struct field names point into the ListSchema).
    struct ValueTree {
        std::deque<std::vector<Value>> arrays;
    };
    using TreePtr = std::shared_ptr<const ValueTree>;
    struct Slot {
        IrPort ir{};
        StringGuard strings;  // T-270c: publicValue() and setText()'s struct / list fields.
        std::string name;
        std::string qualifiedName;
        std::string exposedName;
        std::string elementId;
        NodeId element = kInvalidNode;
        // The current value, by type.
        double num = 0;    // Number, Bool (0/1), Entity, Enum ordinal.
        std::string text;  // String, Texture.
        Value value{};     // Color, Vec2, Vec3.
        std::vector<StringId> fieldNames;
        std::vector<PortValueType> fieldTypes;
        std::vector<std::string> enumValues;
        std::vector<Value> fields;                // Struct.
        std::vector<std::vector<Value>> entries;  // List: each entry's field values.
        std::vector<Value> entryValues;           // List: the Struct Values over `entries`.
        // T-391: the entry types of the List fields (parallel to fieldNames, null elsewhere); the
        // storage of a Struct port's List fields (parallel to fields) and of each list entry's List
        // fields (parallel to entries; each empty or parallel to fieldNames).
        std::vector<std::shared_ptr<const ListSchema>> fieldSubs;
        std::vector<TreePtr> fieldTrees;
        std::vector<std::vector<TreePtr>> entryTrees;
        // T-391: a nested list slot (addNestedList()): field `nestedField` of entry `nestedItem` of
        // list slot `nestedParent`, named after the enclosing `<repeat>` element `nestedRepeat`.
        bool nested = false;
        uint32_t nestedParent = kNoPort;
        uint32_t nestedField = 0;
        uint32_t nestedItem = 0xFFFFFFFFu;
        NodeId nestedRepeat = kInvalidNode;
        std::vector<uint32_t> bindings;    // Bindings that read this port.
        std::vector<NodeId> triggerNodes;  // Elements with `port(<this>)` triggers.
        int maxPort = -1;                  // `<name>_max` in the same scope.
        // T-381: List only: 0 for list<struct>, else the primitive entry type; each entry is then
        // one Value (`entries[i][0]`, also `entryValues[i]`): Number / Bool Float, String
        // StringRef, Color / Vec2 / Vec3 as such, Enum the value name as a StringRef.
        PortValueType entryType = static_cast<PortValueType>(0);
        PortCallHandler callHandler;
        NodeId bone = kInvalidNode;  // T-244: an implicit bone output port's bone.
        // T-380 (27 §27.3.4): a port declared inside a `<repeat>` template (by a symbol) is never
        // set; each repeat instance gets a copy (`instance`), released (`free`) with the instance.
        bool templateOnly = false;
        bool instance = false;
        bool free = false;
        uint32_t templatePort = kNoPort;  // Instance: the template slot it copies.
        NodeId scopeNode = kInvalidNode;  // Instance: the copy of its symbol instance root.
    };
    struct StepState {
        bool init = false;
        BVal from;  // tween: the start of the running interpolation.
        BVal to;    // tween: its target; delay: the last input seen.
        BVal out;   // delay: the value passed through so far.
        float start = 0;
        std::deque<std::pair<float, BVal>> queue;  // delay: (due clock, value).
        NodeId elem = kInvalidNode;                // elemProp: the referenced element.
        uint32_t elemGeneration = 0;
    };
    struct Binding {
        const IrBinding* ir = nullptr;
        NodeId node = kInvalidNode;
        uint32_t generation = 0;
        std::span<const IrConverterStep> steps;
        std::span<const uint32_t> sources;
        int primary = -1;
        int maxPort = -1;
        // A final `tween` on an animatable property runs as an Animator transition (04 §4.6).
        bool transitionTween = false;
        float tweenDuration = 0;
        Easing tweenEasing{};
        bool usesCustom = false;
        bool usesLocalize = false;
        bool applied = false;
        BVal last;
        std::vector<StepState> state;  // Per step.
        bool ticking = false;
        // T-177: a `<repeat>` template's record (never evaluated) or an instance's copy of one.
        bool templateOnly = false;
        bool instance = false;
        bool free = false;            // A removed instance binding (index reusable).
        uint32_t listPort = kNoPort;  // Instance: the repeat's list port.
        uint32_t item = kNoItem;      // Instance: the entry index.
        StringGuard strings;          // T-270c: class names, host converter arguments.
        // T-380: instance copy only: template port slot -> the instance's copy, and the remapped
        // sources (`sources` then points here).
        std::vector<std::pair<uint32_t, uint32_t>> portMap;
        std::vector<uint32_t> ownSources;
        // T-391: `outer.item(...)` / `outer.index`: the enclosing repeat's list slot and entry.
        bool usesOuter = false;
        uint32_t outerPort = kNoPort;
        uint32_t outerItem = kNoItem;
        // T-399: `bind:value` only: the element was `display: none` (itself or an ancestor) at the
        // last reapplyShownValues().
        bool valueHidden = false;
    };
    struct Arg {
        ConverterArg kind = ConverterArg::Number;
        float f = 0;
        uint32_t u = 0;
    };
    struct EvalCtx {
        Binding* binding = nullptr;  // Null for an `if` condition (no tween / delay state).
        int maxPort = -1;            // The `_max` companion `N%` compares against.
        bool ticking = false;        // Set when a tween / delay still has work to do.
        // T-411: a trigger set()'s element: in a `<repeat>` instance a template port argument
        // reads that instance's copy (kInvalidNode = no such mapping).
        NodeId node = kInvalidNode;
    };

    /// The host-visible slot of `port` (null for none, a template-only or a released one).
    Slot* slot(PortId port);
    const Slot* slot(PortId port) const;
    PortRecord infoOf(uint32_t index) const;
    /// T-380: `port` as binding `b` reads it (its instance copy when `b` is a repeat instance's).
    static uint32_t mapPort(std::span<const std::pair<uint32_t, uint32_t>> portMap, uint32_t port);
    /// T-380: the current id of element `node` ("" when none / dead).
    std::string_view nodeIdText(NodeId node) const;
    /// T-380: true when `name` is `<id of node>.<port>`.
    bool pathNameIs(NodeId node, std::string_view port, std::string_view name) const;
    /// T-380: a BindTarget::Port binding's result into slot `index`; `initial` (the binding's first
    /// application) re-evaluates the port's readers as initial and notifies no listener / trigger.
    void writePort(uint32_t index, const BVal& r, bool initial);

    BVal portValue(uint32_t index) const;
    Value publicValue(uint32_t index);
    BVal valueToB(const Value& v) const;
    /// `guard`: whose distinct-string count a Str interned here goes to (T-270c).
    Value bToValue(const BVal& b, StringGuard& guard, std::string_view source);
    /// T-270c: interns `s` for a value that needs a StringId (a host-facing Value, a class name),
    /// counting the runtime strings it adds against `guard`; past kMaxDistinctStrings, warns once
    /// `binding "<source>" produces unbounded distinct strings; bind it to text or a String port
    /// instead of a class / converter argument` (T-270d). Behaviour is otherwise unchanged.
    StringId internCounted(std::string_view s, StringGuard& guard, std::string_view source);
    /// internCounted()'s count alone, for a string interned elsewhere (ElementRef::addClass).
    void countString(std::string_view s, StringGuard& guard, std::string_view source);
    /// Converts and stores `value` into port `index`. `changed` reports whether it differs.
    Status assign(uint32_t index, const Value& value, bool& changed);
    /// T-270c: assign() of a String / Texture / Enum port from text, without interning it.
    Status assignText(uint32_t index, std::string_view text, bool& changed);
    /// A port changed: bindings, listeners, `port(<name>)` triggers.
    void notify(uint32_t index, PortEventKind kind, uint32_t listIndex);
    void notifyListeners(PortEventKind kind, uint32_t index, uint32_t listIndex, uint32_t itemIndex = kNoItem,
                         const Value* item = nullptr);
    void fireTriggers(uint32_t index);
    void rebuildEntries(Slot& s);
    /// `trees` (T-391), when given, receives the storage of the List fields converted (sized to
    /// the field count on the first one); without it a List field is refused.
    bool convertEntry(const Slot& s, const Value& entry, std::vector<Value>& out,
                      std::vector<TreePtr>* trees = nullptr) const;
    /// T-391: converts list Value `list` (whose strings and field names belong to `src`) to schema
    /// `sc` into `tree`; `out` is the List Value over it. False on a type mismatch.
    bool convertNestedList(const ListSchema& sc, const Value& list, const Document& src, ValueTree& tree,
                           Value& out) const;
    /// T-391: one struct field / primitive entry value of type `t` (not List) from `v` into `out`;
    /// `enums` = the value names of an Enum entry (stored as its name).
    bool convertScalar(PortValueType t, const std::vector<std::string>& enums, const Value& v,
                       const Document& src, Value& out) const;
    /// T-391: a nested slot's list changed through the list API: copies it into the enclosing
    /// entry (and on up) and notifies the enclosing port (no repeat re-evaluation).
    void writeBackNested(uint32_t slot);
    /// T-391: the entry type of List field `field` (an index into the PORT chunk's fields).
    std::shared_ptr<const ListSchema> schemaFromIr(uint32_t field, int depth = 0) const;
    /// T-391: makes `s.entryTrees` parallel to `s.entries` (empty storage for new entries).
    static void fitTrees(Slot& s) {
        if (s.entryTrees.size() != s.entries.size()) {
            s.entryTrees.resize(s.entries.size());
        }
    }
    /// T-391: `<id of s.nestedRepeat>/<s.nestedItem>.<s.name>` ("" while it has none).
    std::string nestedName(const Slot& s) const;
    /// T-381: a new entry of list slot `s` before convertEntry(): every field at its zero value
    /// (one Value for a primitive entry type).
    static std::vector<Value> blankEntry(const Slot& s);
    /// T-381: entry value `v` of list slot `s` as a binding value (a Bool entry as a boolean).
    BVal entryToB(const Slot& s, const Value& v) const;
    /// T-383 (27 §27.3.4): the empty value of an `item(<n>)` past the end of list slot `s`: a
    /// primitive entry type's zero value (0, "", false, transparent, the first enum value, a zero
    /// vector); for a list<struct>, the zero value of field `field` (BVal{} when unknown).
    BVal emptyEntryValue(const Slot& s, std::string_view field) const;
    /// T-383: load() is applying a list port's declared default (listSet() stays silent).
    bool m_quietList = false;
    // T-394 (08.15): a trigger `set()` or a binding is writing a port (PortEvent::fromDocument);
    // the host API (set / setText) clears it for its own writes.
    bool m_documentWrite = false;

    void evaluate(uint32_t binding, bool initial);
    BVal run(std::span<const IrConverterStep> steps, BVal v, EvalCtx& ctx);
    size_t gather(std::span<const IrConverterStep> steps, size_t i, std::vector<Arg>& out) const;
    BVal argValue(const Arg& a, bool relativePercent, const EvalCtx& ctx) const;
    BVal applyStep(ConverterOp op, const std::vector<Arg>& args, size_t stepIndex, BVal v, EvalCtx& ctx);
    BVal elemProp(const std::vector<Arg>& args, StepState* state);
    void apply(Binding& b, const BVal& r, bool initial);
    /// T-200: a WorldAnchor binding's run-time translate from the anchor `r` (Vec2 / Vec3).
    void applyWorldAnchor(NodeId node, const BVal& r);
    /// T-244 (32 §32.8): a WorldAnchor binding on a bone: the screen point `r` mapped into the
    /// skeleton group's space, then rig::applyBoneAnchor().
    void applyBoneWorldAnchor(NodeId bone, const BVal& r);
    /// T-244: appends the implicit `<id>.world` (vec2) / `<id>.angle` (number) out ports of every
    /// named bone of the scene instances (load()). T-243b: a port the compiler already declared
    /// (PORT: same name, out, no scope, same type) is taken over by the first bone with the id;
    /// older files without the declarations still get them here.
    void addBonePorts();
    /// T-243 (32 §32.4): the arena node of local scene node `sceneNode` of the scene `element`
    /// owns (its SceneInstance's localToArena table); kInvalidNode when there is none.
    NodeId sceneNodeOf(NodeId element, uint32_t sceneNode) const;
    /// T-243: resolves a scene-node binding's node at load (or for a `<repeat>` copy): only a Prop
    /// target whose PropId applies to the node's kind is kept; anything else is reported
    /// (kInvalidArgument) and the binding stays inert (node kInvalidNode).
    NodeId resolveSceneBinding(const IrBinding& ir, NodeId element);
    /// T-243: a Prop binding on a scene node: setProperty() with `r` converted to the PropId's type.
    void applySceneProp(const Binding& b, const BVal& r);

    Document& m_doc;
    std::vector<Slot> m_slots;
    std::deque<Binding> m_bindings;        // A deque: instance bindings are added while others evaluate.
    std::vector<uint32_t> m_freeBindings;  // T-177: removed instance bindings.
    std::vector<uint32_t> m_freePorts;     // T-380: released instance port slots.
    std::vector<uint32_t> m_afterLayout;
    std::vector<uint32_t> m_bonePorts;  // T-244: the implicit bone output ports' slot indices.
    std::vector<std::pair<std::string, ConverterFn>> m_converters;
    std::vector<std::pair<uint32_t, PortListener>> m_listeners;
    uint32_t m_nextListener = 1;
    std::vector<std::string> m_warnedConverters;
    // callPort(): the last emitted value while a call runs.
    bool m_capturing = false;
    bool m_captured = false;
    Value m_capture{};
    int m_depth = 0;                    // Re-entrancy guard for binding evaluation.
    bool m_anchorMoved = false;         // T-200: afterLayout() moved a world anchor.
    std::string m_styleScratch;         // T-270c: apply()'s "<prop>: <value>" for setStyle (capacity kept).
    StringGuard m_conditionStrings;     // T-270c: host converters inside an `if` condition (no binding).
    uint32_t m_maxDistinctStrings = 0;  // T-270d: the largest StringGuard::distinct so far.
    // T-389: bindings a port change marked, in marking order, and their flags (by binding index).
    std::vector<uint32_t> m_dirty;
    std::vector<uint8_t> m_dirtyFlag;
    /// T-389: marks binding `b` for flushDirty().
    void markDirty(uint32_t b);
};

}  // namespace xt
