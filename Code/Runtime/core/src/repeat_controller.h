#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "xt/document.h"
#include "xt/ir_pod.h"
#include "xt/timeline.h"
#include "xt/types.h"

/// `<repeat>` list templates at run time (Docs/design/27-interaction.md §27.3.4, 03 §3.12.4,
/// Docs/tasks/T-177.md). Private to core; Document owns one RepeatController behind a unique_ptr.
/// The template subtree of every `RPT ` row is kept out of the element tree; each list entry gets
/// a copy of it (an instance) under the `<repeat>` element, whose bindings read that entry. List
/// changes (BindingRuntime) create, re-point, move or remove instances; `item-enter` /
/// `item-exit` run as the template root's triggers; a virtualized repeat only instantiates the
/// entries around its viewport's visible range and keeps the scroll length with two spacers.

namespace xt {

class Document;

/// What changed in a list port (BindingRuntime -> RepeatController).
enum class RepeatListOp : uint8_t { Set, Insert, Remove, Update };

/// RepeatController::play()'s outcome.
enum class RepeatPlay : uint8_t {
    NotInstance,  ///< Not inside an instance, or the timeline animates no template element.
    Played,       ///< Started (or restarted) for the instance.
    KeptPlaying,  ///< `hold` and already playing: left alone.
    Failed,       ///< A `from=` marker the timeline does not have.
};

class RepeatController {
public:
    /// No list entry.
    static constexpr uint32_t kNoItem = 0xFFFFFFFFu;

    explicit RepeatController(Document& doc) : m_doc(doc) {}
    RepeatController(const RepeatController&) = delete;
    RepeatController& operator=(const RepeatController&) = delete;

    /// Reads `RPT `, marks every template element kTemplate and detaches each template root from
    /// its `<repeat>`. Called by Document::loadInternal after the element tree exists and before
    /// TriggerRuntime::load / BindingRuntime::load.
    void prepare();
    /// Clears the template elements' dirty bits, adds the virtualization spacers and instantiates
    /// the current lists. Called last by Document::loadInternal.
    void load();
    /// True when DOCT element `docIndex` belongs to a template.
    bool isTemplateElement(uint32_t docIndex) const {
        return docIndex < m_templateOf.size() && m_templateOf[docIndex] != kNoItem;
    }

    /// A list port (PORT index) changed; `index` is the entry the operation touched.
    void onListChanged(uint32_t port, RepeatListOp op, uint32_t index);
    /// T-260b: Document::remove() is about to destroy the subtree at `root` (still intact). Live
    /// and exiting instances whose root is in it are dropped (their bindings unregistered, their
    /// `port()` trigger nodes forgotten); a `<repeat>` element in it makes that repeat dead (its
    /// pooled instances are destroyed, later list changes are ignored).
    void onSubtreeRemoved(NodeId root);

    /// Advances the instance timelines (before the Animator evaluates the frame).
    void step(float clock);
    /// Sends the instance timelines' End events to the trigger runtime.
    void flushEvents();
    /// Virtualization against the fresh layout; true when instances or spacers changed (the
    /// Document then resolves and lays out once more).
    bool afterLayout();

    /// `play(<name>)` fired on `node`. When `node` is inside an instance and the timeline animates
    /// template elements, plays that timeline's copy for the instance (`scope` = its root).
    RepeatPlay play(NodeId node, StringId name, const PlayOptions& options, bool keepPlaying, NodeId& scope);
    /// `stop(<name>)` on an instance's copy; false when `node` has none (`scope` = the instance root).
    bool stop(NodeId node, StringId name, NodeId& scope);
    /// `seek(<name>, t)` on an instance's copy; false when `node` has none.
    bool seek(NodeId node, StringId name, float t);
    /// `remove`: destroys `node`'s instance when it is exiting.
    void removeAction(NodeId node);

    /// Most live instances of one spawn pool; a further spawn retires the oldest (03 §3.12.3
    /// maxInstances).
    static constexpr uint32_t kMaxSpawnInstances = 32;
    /// T-201 (27.2.3, 03 §3.12.3): `spawn()` fired on `node` for its `pool`-th spawn pool child: a
    /// new (or pooled) copy of the pool's template, its root absolutely positioned with its
    /// top-left at `at` (viewport px) and `pointer-events: none` (08.9), its bindings evaluated
    /// once and then frozen, `timeline` (kInvalidString = none) played on it; it is recycled
    /// `lifetime` seconds later. False when `node` has no such pool.
    bool spawn(NodeId node, uint32_t pool, Vec2 at, float lifetime, StringId timeline);

    /// An instance element with path id `id`, or an invalid ref.
    ElementRef byId(std::string_view id) const;
    /// Document::repeatInstances().
    std::span<const ElementRef> instances(std::string_view repeatId);
    /// T-387: Document::repeatIndexAt().
    uint32_t indexAt(std::string_view repeatId, Vec2 rootPos) const;
    /// T-394 (08.15): where entry `index` of the repeat `repeatId` is, from the last layout.
    struct ItemSpan {
        NodeId element = kInvalidNode;   ///< The `<repeat>` element.
        NodeId instance = kInvalidNode;  ///< The entry's live instance root (kInvalidNode when a
                                         ///< virtualized entry is not instantiated).
        bool virtualized = false;        ///< `top` / `height` hold the entry's span.
        float top = 0;                   ///< Virtualized: the entry's top in `element`'s border box.
        float height = 0;                ///< Virtualized: the measured entry height (margins included).
    };
    /// Read only. False for an unknown repeat, `index` past its list, a virtualized repeat whose
    /// entry height is not measured yet, or a non-virtualized entry with no live instance.
    bool itemSpan(std::string_view repeatId, uint32_t index, ItemSpan& out) const;
    /// The entry index of the live instance containing `node` (kNoItem when none); `listPort`, if
    /// given, receives its repeat's list port. T-391: inside nested repeats that is the innermost
    /// instance; with `outermost`, the instance of the outermost repeat around `node` instead.
    uint32_t itemIndexOf(NodeId node, uint32_t* listPort, bool outermost = false) const;
    /// T-397 (08 §8.17): Document::repeatChain(). Read only.
    void chainOf(NodeId node, std::vector<RepeatRef>& out) const;
    /// T-380: template port `port` (a PORT index) as seen from `node`: the copy of the live or
    /// exiting instance containing `node` (T-391: or of an enclosing one), else `port` itself.
    uint32_t instancePortOf(NodeId node, uint32_t port) const;

private:
    struct Instance {
        uint32_t index = kNoItem;
        std::vector<NodeId> nodes;       // Parallel to Repeat::tpl; nodes[0] is the root.
        std::vector<uint32_t> bindings;  // BindingRuntime indices.
        // T-380: (template port slot, this instance's copy), parallel to Repeat::tplPorts.
        std::vector<std::pair<uint32_t, uint32_t>> ports;
        // T-391 (27 §27.3.4): the m_repeats indices of the nested repeats inside this instance.
        std::vector<uint32_t> inner;
    };
    struct Repeat {
        const IrRepeat* ir = nullptr;
        NodeId element = kInvalidNode;
        StringId id = kInvalidString;
        // T-391 (27 §27.3.4): the list slot (a PORT index, or a nested list slot of
        // BindingRuntime::addNestedList()); kRepeatNoListPort for a spawn pool.
        uint32_t listPort = kRepeatNoListPort;
        uint32_t self = 0;      // T-391: this record's index in m_repeats.
        uint32_t tplIndex = 0;  // T-391: the RPT row (m_templateOf value) whose template it copies.
        // T-391: a `<repeat>` inside another repeat's template: its RPT row only describes it
        // (`proto`, never live); every instance of the enclosing repeat gets a live copy whose
        // `parentRepeat` is the enclosing record and `outerPort` / `outerItem` its list and entry.
        bool proto = false;
        bool free = false;        // T-391: a released copy (index reusable).
        bool hiddenList = false;  // T-391: `listPort` is this copy's own nested list slot.
        bool synced = false;      // T-391: a copy's first instances were made.
        bool frozen = false;      // T-391: inside an exiting instance: no list changes, no id lookups.
        uint32_t parentRepeat = kNoItem;
        uint32_t outerPort = kNoItem;
        uint32_t outerItem = kNoItem;
        std::string stripPrefix;            // T-378: the symbol instance prefix template ids drop.
        std::vector<uint32_t> tpl;          // Template DOCT element indices, ascending.
        std::vector<uint32_t> tplBindings;  // BIND record indices of template elements.
        std::vector<uint32_t> tplPorts;     // T-380: PORT indices declared on template elements.
        bool virtualize = false;
        bool spawn = false;             // T-201: a spawn pool (kRepeatSpawn), no list port.
        bool exitRemoves = false;       // The item-exit actions contain `remove`.
        bool dead = false;              // T-260b: the `<repeat>` element was removed by the host.
        std::vector<Instance> live;     // Ascending index.
        std::vector<Instance> exiting;  // Playing item-exit.
        std::vector<Instance> pool;     // Virtualized: recycled, detached.
        // Virtualization.
        NodeId spacerBefore = kInvalidNode;
        NodeId spacerAfter = kInvalidNode;
        float spacerBeforeH = -1;
        float spacerAfterH = -1;
        float itemHeight = 0;
        uint32_t windowBegin = 0;
        uint32_t windowEnd = 1;  // Before the first measurement: entry 0 only.
        uint32_t poolCap = 3;
        bool viewportChecked = false;  // T-392 (27 §27.3.2): the zero-height viewport check ran.
        std::vector<ElementRef> refs;  // instances() scratch.
    };
    struct Playback {
        NodeId scope = kInvalidNode;
        uint32_t scopeGeneration = 0;
        size_t group = 0;
        StringId name = kInvalidString;
        float duration = 0;
        float t = 0;
        float lastClock = 0;
        float rate = 1;
        bool reverse = false;
        bool loop = false;
        bool hold = false;
        bool playing = false;
        std::vector<uint32_t> ids;  // Animator instances.
        std::vector<NodeId> nodes;  // Their elements.
        std::vector<IrTimelineGroupMember> members;
    };

    Repeat* repeatOfElement(NodeId element);
    /// The repeat and instance whose root is `node` or an ancestor of it.
    bool findInstance(NodeId node, Repeat*& repeat, Instance*& instance, bool& exiting);
    Instance createInstance(Repeat& r, uint32_t index);
    void attachInstance(Repeat& r, Instance& inst);
    void pointInstance(Repeat& r, Instance& inst, uint32_t index);
    /// A pooled instance being reused: clears its subtree's kPooled bit (T-270f) and resets its
    /// elements from the template.
    void resetFromTemplate(Repeat& r, Instance& inst);
    void recycleInstance(Repeat& r, Instance inst);
    void destroyInstance(Repeat& r, Instance& inst);
    /// T-260b: unregisters `inst`'s bindings and forgets its nodes' `port()` triggers (the part of
    /// destroyInstance() that does not touch the nodes themselves).
    void releaseInstance(Instance& inst);
    /// Evaluates `inst`'s bindings. T-260b: their ids are copied to m_evalScratch first (used as a
    /// stack), so a binding that changes the list (re-entrancy) cannot pull them from under it.
    void evaluateInstance(const Instance& inst, bool initial);
    /// T-391: the live copies of the nested repeats inside `inst` (records only, no instances).
    void createInnerRepeats(Repeat& r, Instance& inst);
    /// T-391: the nested repeats inside `inst` follow it (after evaluateInstance()): a copy of a
    /// list field is refreshed (Set), the first time the instances are made, otherwise the
    /// instance bindings reading `outer.*` are evaluated again.
    void refreshInner(uint32_t repeat);
    /// T-391: releases the nested repeat copy `repeat` (its instances, recursively, and its nested
    /// list slot); `destroyNodes` also removes its instances' nodes (the enclosing instance is
    /// pooled, not destroyed).
    void releaseRepeat(uint32_t repeat, bool destroyNodes);
    /// T-391: `inst`'s entry index is now `index`: its nested repeats follow (ids, nested list
    /// slot, `outer.*` bindings).
    void repointInner(Instance& inst, uint32_t index);
    /// T-391: re-reads the ids of `inst`'s nested repeats and renames their instances (recursively).
    void refreshInnerIds(Instance& inst);
    /// T-391: `inst` starts its item-exit: its nested repeats (recursively) stop following lists and
    /// answer to no path id (the live instance that took its index does).
    void freezeInner(Instance& inst);
    /// T-260b: the index in `r.live` of the instance whose root is `root`, or r.live.size().
    size_t liveIndexOf(const Repeat& r, NodeId root) const;
    void renameInstance(Repeat& r, Instance& inst);
    NodeId anchorFor(const Repeat& r, uint32_t index) const;
    /// Makes `r.live` exactly the entries [begin, end) (clamped to the list).
    bool sync(Repeat& r, uint32_t begin, uint32_t end);
    bool setSpacers(Repeat& r);
    NodeId viewportOf(const Repeat& r) const;
    void stopPlaybacks(NodeId scope);
    void releasePlayback(Playback& pb);
    void applyPlayback(Playback& pb);
    uint32_t count(const Repeat& r) const;

    /// T-201: a live spawned instance and when it expires.
    struct Spawned {
        NodeId root = kInvalidNode;
        uint32_t generation = 0;
        float expires = 0;
        uint32_t repeat = 0;
    };
    void expireSpawned(float clock);

    Document& m_doc;
    std::deque<Repeat> m_repeats;  // T-391: a deque, nested copies are added while others run.
    std::vector<Spawned> m_spawned;
    uint32_t m_spawnSerial = 0;
    std::vector<uint32_t> m_templateOf;  // Per DOCT element: repeat index or kNoItem.
    std::vector<uint32_t> m_tplPos;      // Per DOCT element: position in its repeat's `tpl`.
    std::vector<Playback> m_playbacks;
    std::vector<std::pair<NodeId, StringId>> m_pendingEnds;
    // T-260b: evaluateInstance()'s binding ids. A stack: a nested evaluation appends its own ids
    // and shrinks back to where it started, so the outer one reads its part by index.
    std::vector<uint32_t> m_evalScratch;
};

}  // namespace xt
