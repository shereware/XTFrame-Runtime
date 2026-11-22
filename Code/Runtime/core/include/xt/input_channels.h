#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xt/input.h"

/// Input channels (Docs/design/60-input-channels.md §60.2, §60.5; Docs/tasks/T-873.md): named
/// quantities a host (or the shell, or the editor) feeds and a UI reads by name, whatever device
/// they come from. Three kinds: a button (0 / 1 with press / release edges and a strength 0..1), an
/// axis (a position) and a relative channel (an increment; the pushes of one frame are summed).
///
/// Names (60 §60.2; the player prefix `player(<0-7>).` is not part of the name):
///   pad.a|b|x|y|lb|rb|back|start|guide|ls|rs|up|down|left|right   button
///   pad.lx|ly|rx|ry (axis -1..1, y down positive), pad.lt|rt (axis 0..1)
///   key.<key name> (button; see keyCodeFromName(); key.Shift|Control|Alt|Meta = either side held)
///   wheel.x|y (relative, notches, down positive), pointer.dx|dy (relative, logical px)
///   pointer.left|middle|right (button)
///   midi.note(<0-127>) (button, strength = velocity), midi.cc(<0-127>) (axis 0..1, or relative
///   when its first push says so), midi.pitch (axis -1..1), midi.pressure (axis 0..1),
///   midi.ch(<1-16>).<one of the four above>
///   host.<any name>, action:<any name> (the kind of the first push)
/// A channel's state is kept per local player (kMaxPlayers).

namespace xt {

/// A channel id; 0 = none. Ids are per InputChannels table (Context, Document): a Context's ids are
/// what Context::inputChannel() / xt_input_channel() hand out.
using ChannelId = uint32_t;
inline constexpr ChannelId kInvalidChannel = 0;

/// The kind of a channel (60 §60.2).
enum class ChannelKind : uint8_t {
    Axis = 0,      ///< A position (-1..1 or 0..1).
    Button = 1,    ///< 0 / 1 with press / release edges and a strength.
    Relative = 2,  ///< An increment: a frame's pushes are summed; reads as the frame's sum.
};

/// InputEvent::channelFlags (and xt_surface_push_channel's `flags`): the kind a channel without a
/// fixed kind (host.*, action:*, midi.cc(n)) takes at its first push; no flag = an axis. Ignored for
/// a channel whose kind is fixed (every other standard name).
inline constexpr uint8_t kChannelRelative = 1u;
inline constexpr uint8_t kChannelButton = 2u;

/// A key name of `key(<name>)` / `key.<name>` (W3C KeyboardEvent.code names; `A`, `Digit1`,
/// `Left` ... spellings too) -> KeyCode. False for an unknown name. `any` is not a key name.
bool keyCodeFromName(std::string_view name, KeyCode& out);
/// The canonical name of `code` (the W3C KeyboardEvent.code name: `KeyA`, `Digit1`, `ArrowLeft`,
/// `ShiftLeft` ...), or an empty view for KeyCode::Unknown and codes without a name.
std::string_view keyCodeName(KeyCode code);

/// Parses a channel reference `[player(<0-7>).]<name>` (blanks ignored): `canonical` receives the
/// name in its canonical spelling (numbers without leading zeros, key aliases resolved), `player`
/// the player (0 without a prefix). False, with the reason in `err` when given, for a name outside
/// the grammar above.
bool parseChannelRef(std::string_view text, std::string& canonical, uint8_t& player,
                     std::string* err = nullptr);
/// The fixed kind of canonical channel `name`; false when the first push decides it (host.*,
/// action:*, midi.cc(n), midi.ch(c).cc(n)).
bool fixedChannelKind(std::string_view name, ChannelKind& out);

/// One channel table with its state (60 §60.5). Pushes between two commit() calls are one frame:
/// commit() makes them the frame's values and edges, which stay readable until the next commit().
class InputChannels {
public:
    InputChannels();

    /// The id of channel `name` (any spelling parseChannelRef() accepts, without a player prefix),
    /// added when new; kInvalidChannel for a malformed name.
    ChannelId intern(std::string_view name);
    /// The id of `name` when it is in the table, else kInvalidChannel.
    ChannelId find(std::string_view name) const;
    /// The canonical name of `id` ("" for an unknown id).
    std::string_view name(ChannelId id) const;
    /// The kind of `id` (an axis until the first push of a channel without a fixed kind).
    ChannelKind kind(ChannelId id) const;
    /// Number of channels in the table (ids 1..count()).
    uint32_t count() const { return static_cast<uint32_t>(m_channels.size()); }

    /// One value for the coming frame: an axis takes `value`, a relative channel adds it, a button is
    /// down while `value` >= 0.5 (its edges are kept even when both happen within the frame).
    /// `strength` (0..1) is a button's (and is kept for the others); `flags` see kChannelRelative.
    void push(ChannelId id, float value, float strength, uint8_t flags, uint8_t player);

    /// One derived push (applyEvent()).
    struct Push {
        ChannelId id = kInvalidChannel;
        float value = 0;
        float strength = 1;
        uint8_t flags = 0;
        uint8_t player = 0;
    };
    /// 60 §60.5: the standard channels an existing input event moves (pad.*, key.*, wheel.*,
    /// pointer.*), pushed here; each push is also appended to `out` when given. A Channel event is
    /// not derived (push() it). Other kinds move nothing.
    void applyEvent(const InputEvent& event, std::vector<Push>* out = nullptr);

    /// Ends a frame: the pushes since the last commit become the frame's values and edges.
    void commit();

    float value(ChannelId id, uint8_t player = 0) const;  ///< Button 0 / 1; relative: the frame's sum.
    float strength(ChannelId id, uint8_t player = 0) const;
    /// The change since the previous frame: a relative channel's frame sum, else value - previous.
    float delta(ChannelId id, uint8_t player = 0) const;
    bool pressed(ChannelId id, uint8_t player = 0) const;   ///< A press edge this frame.
    bool released(ChannelId id, uint8_t player = 0) const;  ///< A release edge this frame.
    /// 60 §60.3.2: the frame's change event: an axis moved, a relative channel's sum is not 0, a
    /// button changed or had an edge.
    bool changed(ChannelId id, uint8_t player = 0) const;
    /// The value read this frame differs from the previous frame's, or a button had an edge (what a
    /// binding re-evaluates on; a relative channel going back to 0 counts).
    bool moved(ChannelId id, uint8_t player = 0) const;

    /// One (channel, player) whose state commit() changed (moved() or changed()).
    struct Change {
        ChannelId id = kInvalidChannel;
        uint8_t player = 0;
    };
    /// The changes of the last commit(), in first-push order.
    const std::vector<Change>& changes() const { return m_changes; }
    /// Pushes wait for the next commit().
    bool pending() const { return m_pushes != 0; }
    /// The next commit() changes something: pushes wait, or a relative channel's frame sum, a button
    /// edge or a moved value goes back to rest.
    bool settling() const { return !m_next.empty(); }
    /// 60 §60.5 needsFrame(): `id` is away from rest for some player (an axis not 0, a button down).
    bool held(ChannelId id) const;
    /// Notes that something (a binding, a trigger) reads `id`.
    void listen(ChannelId id);
    /// 60 §60.5 needsFrame(): a listened channel is held.
    bool listenedHeld() const;

    /// Takes over `from`'s current positions and buttons (by name; no edges, no relative sums) as
    /// pushes the next commit() makes current, as a Document loaded into a running Context starts
    /// from the Context's state.
    void seedFrom(const InputChannels& from);

private:
    struct PlayerState {
        float value = 0;
        float previous = 0;
        float strength = 1;
        float pendingValue = 0;
        float pendingStrength = 1;
        bool pendingSet = false;
        bool pendingDown = false;
        bool pendingPress = false;
        bool pendingRelease = false;
        bool pressed = false;
        bool released = false;
        bool changed = false;
        bool moved = false;
        bool queued = false;  // In m_next.
    };
    struct Channel {
        std::string name;
        ChannelKind kind = ChannelKind::Axis;
        bool fixed = false;    // The kind is fixed by the name.
        bool decided = false;  // The kind is known (fixed, or set by the first push).
        std::array<PlayerState, kMaxPlayers> players{};
    };
    const PlayerState* state(ChannelId id, uint8_t player) const;
    /// Interns a standard name built from parts (no parsing).
    ChannelId standard(std::string_view name);

    std::vector<Channel> m_channels;
    std::unordered_map<std::string, ChannelId> m_byName;
    std::vector<Change> m_next;     // What the next commit looks at (each once): pushed, or settling.
    std::vector<Change> m_work;     // commit()'s copy of m_next (capacity kept).
    std::vector<Change> m_changes;  // What the last commit changed.
    uint32_t m_pushes = 0;          // Pushes since the last commit.
    std::vector<ChannelId> m_listened;  // listen().
    // applyEvent(): the standard ids looked up once.
    ChannelId m_padButtons[15] = {};
    ChannelId m_padAxes[6] = {};
    ChannelId m_wheel[2] = {};
    ChannelId m_pointerDelta[2] = {};
    ChannelId m_pointerButtons[3] = {};
    ChannelId m_modifiers[4] = {};  // key.Shift, key.Control, key.Alt, key.Meta.
    // The last pointer position per player (pointer.dx / dy), and the sided modifier keys held.
    std::array<Vec2, kMaxPlayers> m_lastPointer{};
    std::array<bool, kMaxPlayers> m_hasPointer{};
    std::array<uint8_t, kMaxPlayers>
        m_sidedHeld{};  // Bits: ShiftL, ShiftR, CtrlL, CtrlR, AltL, AltR, MetaL, MetaR.
};

}  // namespace xt
