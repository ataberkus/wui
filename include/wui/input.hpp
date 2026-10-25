#pragma once

#include <wui/id.hpp>
#include <wui/types.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <utility>

namespace wui {
namespace detail { struct FrameState; }

enum class EventKind { pointer_move, pointer_down, pointer_up, wheel,
                       key_down, key_up, focus_lost, text, composition };
// Earlier enumerators keep their values; later ones were appended.
enum class Key { none, tab, enter, space, escape, left, right, home, end,
                 backspace, del, up, down, a, c, v, x, z,
                 b, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, w, y,
                 digit0, digit1, digit2, digit3, digit4, digit5, digit6, digit7, digit8, digit9,
                 f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
                 insert, page_up, page_down,
                 left_shift, right_shift, left_control, right_control,
                 left_alt, right_alt, left_super, right_super };
// Left is the primary activation button. Right press and release on the same
// enabled hit target reports Behavior::secondary_activated; middle and the X
// buttons are delivered as raw events only (Frame::events()).
enum class PointerButton : unsigned char { left, right, middle, x1, x2 };
struct InputEvent {
    EventKind kind;
    Vec2 position{};
    Key key = Key::none;
    float wheel = 0;    // vertical notches; positive scrolls content up
    bool shift = false, control = false, repeat = false;
    std::string text;
    PointerButton button = PointerButton::left; // pointer_down / pointer_up only
    float wheel_x = 0;  // horizontal notches; positive moves right
    bool alt = false, super = false;
    // Platform key code for key events (Win32: the virtual-key code), so hosts
    // can bind keys that Key does not name. Zero when not supplied.
    std::uint32_t native_key = 0;
};
struct FrameInput {
    Rect viewport;
    float scale = 1;
    double dt_seconds = 1.0 / 60.0;
    std::vector<InputEvent> events;
};
struct Behavior {
    bool hovered = false, held = false, focused = false, activated = false;
    bool secondary_activated = false;
};
struct DragAction {
    Key key = Key::none; // none denotes an accepted pointer position.
    float position_x = 0;
    float position_y = 0;
};
struct DragResult {
    Behavior behavior;
    bool moved = false;
    float position_x = 0;
    float position_y = 0;
    std::span<const DragAction> actions; // valid until the next drag() or begin().
};

// Each pointer_down clears focus, capture and pending keyboard activation;
// the first enabled current-frame hit claims that gesture. Blank presses clear
// focus. Keyboard events following a pointer_down, and their entire ordered
// suffix, wait until the next begin and precede newly supplied events. This
// gives immediate widgets current geometry without guessing future hit targets.
// A suffix can wait again if another pointer_down precedes keyboard input.
// focus_lost cancels capture and pending input immediately, even in a deferred
// suffix; completed activations preceding the barrier remain valid.
// Only the left button takes part in focus, capture and activation. A right
// press claims the first enabled hit target without changing focus or
// capture; its release on that target reports secondary_activated. Other
// buttons and horizontal wheel steps are only visible through events().
// Input events are owned copies; no caller model pointers are retained.
class Interaction {
public:
    void begin(const FrameInput& input, Id popup_owner_at_frame_start);
    Behavior button(Id id, Rect bounds, bool enabled);
    void end();
    DragResult drag(Id id, Rect bounds, bool enabled);
    template<class F> void edit(Id id, bool enabled, F&& on_event);
    // Delivers the focused widget's keyboard, text and composition events as
    // edit() does, for a widget that routes its pointer through drag() under
    // the same id. Call it before drag(); it neither claims the id nor joins
    // the focus order (drag() does both).
    template<class F> void keyboard(Id id, F&& on_event);
    Id focused() const;
    Id captured() const;
    // Every event routed this frame, in order (deferred keyboard input
    // included once it is routed), independent of consumption.
    std::span<const InputEvent> events() const { return raw_; }
    std::optional<Vec2> pointer() const;

private:
    friend struct detail::FrameState;
    struct EventRecord {
        InputEvent event;
        Id activation{};
        bool consumed = false;
        float wheel_remaining = 0;
    };
    void clear_press();
    void traverse(bool backwards);
    void prepare_events(const FrameInput& input);
    // Widgets under an open popup are inert unless they are emitted inside
    // the popup's own layer.
    bool blocked() const { return popup_owner_at_frame_start_ != Id{} && !layer_open_; }
    float take_wheel(Rect viewport, float current_offset, float max_offset);
    Vec2 take_wheel_steps(Rect area);
    // Closes a popup layer: consumes presses and releases outside `bounds`
    // (and Escape / focus loss unless modal). True when a non-modal popup
    // must be dismissed.
    bool finish_layer(Rect bounds, bool modal);
    Behavior popup_button(Id id, Rect bounds, bool enabled);
    float popup_wheel(Rect list, Id live_owner, float current_offset, float max_offset);
    void popup_focus(Id id);

    std::vector<EventRecord> events_;
    std::vector<EventRecord> deferred_;
    std::vector<EventRecord> pending_records_;
    std::vector<InputEvent> pending_;
    std::vector<InputEvent> raw_;
    std::vector<Id> previous_order_, current_order_, seen_;
    std::vector<DragAction> drag_actions_;
    Id focus_{}, capture_{}, initial_capture_{}, keyboard_owner_{};
    Id secondary_{}, initial_secondary_{};
    bool layer_open_ = false;
    std::vector<Id> previous_popup_order_, current_popup_order_;
    Id popup_owner_at_frame_start_{};
    Rect popup_list_{};
    bool popup_dismissed_ = false;
    bool popup_escape_ = false;
    bool popup_navigated_ = false;
    Key keyboard_key_ = Key::none;
    Vec2 pointer_{};
    bool has_pointer_ = false;
    bool active_ = false;
    std::optional<std::size_t> focus_event_, capture_event_, secondary_event_;
};
template<class F> void Interaction::edit(Id id, bool enabled, F&& on_event) {
    if (!active_ || id == Id{} ||
        std::find(seen_.begin(), seen_.end(), id) != seen_.end()) return;
    seen_.push_back(id);
    if (!enabled || blocked()) {
        if (!enabled) {
            if (focus_ == id) focus_ = {};
            if (capture_ == id) capture_ = {};
            if (keyboard_owner_ == id) clear_press();
        }
        return;
    }
    current_order_.push_back(id);
    keyboard(id, std::forward<F>(on_event));
}
template<class F> void Interaction::keyboard(Id id, F&& on_event) {
    if (!active_ || id == Id{} || blocked() || focus_ != id) return;
    for (auto& record : events_) {
        if (record.consumed) continue;
        const auto kind = record.event.kind;
        if (kind != EventKind::key_down && kind != EventKind::key_up &&
            kind != EventKind::text && kind != EventKind::composition) continue;
        if (record.activation == Id{} || record.activation == focus_) {
            on_event(record.event);
            record.consumed = true;
            if (kind == EventKind::key_down &&
                (record.event.key == Key::enter || record.event.key == Key::escape))
                clear_press();
        }
    }
}

} // namespace wui
