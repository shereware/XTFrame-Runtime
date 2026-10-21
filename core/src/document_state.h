#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xt/input.h"

/// T-398 (docs/design/23-host-integration.md §23.ad.1): the runtime state of a Document that
/// Document::snapshotState() / restoreState() serialize and Context::hotReload carries over to a
/// recompiled Document. Everything is keyed by name (element ids, state names, port names), never
/// by node or index, so it applies to another load of the same (or a recompiled) IR. Internal to
/// xt_core (not installed under core/include).

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

    std::vector<Port> ports;
    std::vector<States> states;
    std::vector<Input> inputs;
    std::vector<Scroll> scrolls;
    std::vector<Timeline> timelines;
    /// Each player's focused element id ("" = none).
    std::string focus[kMaxPlayers];
};

}  // namespace xt
