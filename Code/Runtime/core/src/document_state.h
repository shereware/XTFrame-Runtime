#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xt/input.h"

/// T-398 (Docs/design/23-host-integration.md §23.ad.1): the runtime state of a Document that
/// Document::snapshotState() / restoreState() serialize and Context::hotReload carries over to a
/// recompiled Document. Everything is keyed by name (element ids, state names, port names), never
/// by node or index, so it applies to another load of the same (or a recompiled) IR. Internal to
/// xt_core (not installed under Code/Runtime/core/include).

namespace xt {

struct DocumentState {
    /// One port's value as setPortText() takes it, by qualified name.
    struct Port {
        std::string name;
        std::string text;
    };
    /// One element's custom states, by name.
    struct States {
        std::string id;
        std::vector<std::string> names;
    };
    /// One editable `input`'s value and selection (UTF-8 byte offsets).
    struct Input {
        std::string id;
        std::string value;
        uint32_t cursor = 0;
        uint32_t anchor = 0;
    };
    /// One scrolled element's offset.
    struct Scroll {
        std::string id;
        float x = 0;
        float y = 0;
    };
    /// One active `@timeline` group.
    struct Timeline {
        std::string scope;  // "" = the document root.
        std::string name;
        bool playing = false;
        bool reverse = false;
        bool loop = false;
        bool hold = false;
        float t = 0;
        float rate = 1;
    };

    /// T-616 (04 §4.13): one CSS `animation` item of an element with an id, matched by its position
    /// in the element's `animation` list and its `@keyframes` name.
    struct Animation {
        std::string id;
        uint32_t position = 0;
        std::string name;
        double elapsed = 0;  // Animator::elapsedSeconds().
        bool paused = false;
        bool ended = false;     // Finished and dropped (no forwards fill): the new one is stopped.
        uint8_t direction = 0;  // T-651: IrAnimationSpec::direction, written for reading only.
    };

    /// T-650 (23 XF-097 version 2): one id'd element's runtime classes against its IR classes.
    struct Classes {
        std::string id;
        std::vector<std::string> add;     // Runtime classes the IR element does not have, sorted.
        std::vector<std::string> remove;  // IR classes the element no longer has, sorted.
    };

    std::vector<Port> ports;
    std::vector<States> states;
    std::vector<Input> inputs;
    std::vector<Scroll> scrolls;
    std::vector<Timeline> timelines;
    std::vector<Animation> animations;  // T-616; in the snapshotState() JSON since version 3 (T-651).
    /// Each player's focused element id ("" = none).
    std::string focus[kMaxPlayers];
    // T-650 (23 XF-097 version 2). A version 1 snapshot has none of them (has* false, shown -1).
    std::vector<Classes> classes;  // Only elements whose classes differ from the IR's.
    bool hasClasses = false;       // The snapshot lists classes (applied by restoreState() only).
    bool hasClock = false;
    double clockSeconds = 0;  // The document clock (a float, written with %.17g).
    uint64_t clockFrame = 0;  // Document::update() calls.
    int shown = -1;           // The initial show was done: 1, not yet: 0, not in the snapshot: -1.
};

}  // namespace xt
