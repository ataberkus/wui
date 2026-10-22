#include <wui/input.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace wui {
namespace {
constexpr float wheel_pixels_per_notch = 32.0f;
}

void Interaction::clear_press() {
    keyboard_owner_ = {};
    keyboard_key_ = Key::none;
}

void Interaction::traverse(bool backwards) {
    clear_press();
    if (previous_order_.empty()) {
        focus_ = {};
        return;
    }
    const auto found = std::find(previous_order_.begin(), previous_order_.end(), focus_);
    if (found == previous_order_.end()) {
        focus_ = backwards ? previous_order_.back() : previous_order_.front();
        return;
    }
    const auto index = static_cast<std::size_t>(found - previous_order_.begin());
    const auto count = previous_order_.size();
    focus_ = previous_order_[(index + (backwards ? count - 1 : 1)) % count];
}

void Interaction::prepare_events(const FrameInput& input) {
    pending_records_.clear();
    pending_records_.reserve(deferred_.size() + input.events.size());
    for (auto& record : deferred_) pending_records_.push_back(std::move(record));
    deferred_.clear();
    for (const auto& event : input.events) pending_records_.push_back({event});
    events_.clear();
    bool pointer_down = false;
    bool defer = false;
    for (auto& event : pending_records_) {
        // A native focus-loss barrier must not wait behind deferred input.
        // Discard the incomplete suffix, retain the completed prefix, and let
        // subsequent fresh gestures run after this cancellation barrier.
        if (event.event.kind == EventKind::focus_lost) {
            deferred_.clear();
            defer = false;
            pointer_down = false;
            events_.push_back(std::move(event));
            events_.back().wheel_remaining = events_.back().event.wheel;
            continue;
        }
        if (event.consumed) {
            events_.push_back(std::move(event));
            continue;
        }
        if (pointer_down && (event.event.kind == EventKind::key_down ||
                             event.event.kind == EventKind::key_up ||
                             event.event.kind == EventKind::text ||
                             event.event.kind == EventKind::composition)) {
            defer = true;
        }
        if (defer) {
            deferred_.push_back(std::move(event));
        } else {
            pointer_down = pointer_down || event.event.kind == EventKind::pointer_down;
            events_.push_back(std::move(event));
            events_.back().wheel_remaining = events_.back().event.wheel;
        }
    }
    raw_.clear();
    raw_.reserve(events_.size());
    for (const auto& record : events_) raw_.push_back(record.event);
}

void Interaction::begin(const FrameInput& input, Id popup_owner_at_frame_start) {
    popup_owner_at_frame_start_ = popup_owner_at_frame_start;
    popup_list_ = {};
    popup_dismissed_ = false;
    popup_escape_ = false;
    popup_navigated_ = false;
    drag_actions_.clear();
    prepare_events(input);
    current_order_.clear();
    current_popup_order_.clear();
    seen_.clear();
    initial_capture_ = capture_;
    initial_secondary_ = secondary_;
    focus_event_.reset();
    capture_event_.reset();
    secondary_event_.reset();
    layer_open_ = false;
    active_ = true;

    // Keyboard input precedes the first pointer down in each routed segment.
    // Its targets can therefore be determined from persistent focus and the
    // previous eligible order before any current widget geometry is known.
    for (std::size_t i = 0; i < events_.size(); ++i) {
        auto& record = events_[i];
        const auto& event = record.event;
        switch (event.kind) {
        case EventKind::pointer_move:
        case EventKind::wheel:
            pointer_ = event.position;
            has_pointer_ = true;
            break;
        case EventKind::pointer_down:
            pointer_ = event.position;
            has_pointer_ = true;
            if (event.button == PointerButton::right) {
                secondary_ = {};
                secondary_event_ = i;
                break;
            }
            if (event.button != PointerButton::left) break;
            focus_ = {};
            capture_ = {};
            clear_press();
            focus_event_ = i;
            capture_event_ = i;
            break;
        case EventKind::pointer_up:
            pointer_ = event.position;
            has_pointer_ = true;
            if (event.button == PointerButton::right) secondary_ = {};
            if (event.button != PointerButton::left) break;
            capture_ = {};
            capture_event_.reset();
            break;
        case EventKind::focus_lost:
            focus_ = {};
            capture_ = {};
            secondary_ = {};
            clear_press();
            focus_event_.reset();
            capture_event_.reset();
            secondary_event_.reset();
            has_pointer_ = false;
            break;
        case EventKind::key_down:
            if (event.key == Key::tab) {
                if (popup_owner_at_frame_start_ == Id{}) traverse(event.shift);
                record.consumed = true;
            } else if (popup_owner_at_frame_start_ != Id{} &&
                       (event.key == Key::up || event.key == Key::down)) {
                if (!previous_popup_order_.empty()) {
                    const auto found = std::find(previous_popup_order_.begin(),
                                                 previous_popup_order_.end(), focus_);
                    const auto size = previous_popup_order_.size();
                    const auto index = found == previous_popup_order_.end()
                        ? (event.key == Key::up ? size - 1 : 0)
                        : (static_cast<std::size_t>(found - previous_popup_order_.begin()) +
                           (event.key == Key::up ? size - 1 : 1)) % size;
                    focus_ = previous_popup_order_[index];
                    popup_navigated_ = true;
                    clear_press();
                }
                record.consumed = true;
            } else if (((event.key == Key::left || event.key == Key::right ||
                         event.key == Key::home || event.key == Key::end ||
                         event.key == Key::backspace || event.key == Key::del ||
                         event.key == Key::a || event.key == Key::z) ||
                        (event.control &&
                         (event.key == Key::c || event.key == Key::x || event.key == Key::v))) &&
                       !record.consumed) {
                record.activation = focus_;
            } else if (event.key == Key::enter && !event.control && !event.shift &&
                       !event.repeat && focus_ != Id{} && keyboard_owner_ == Id{}) {
                // Focused editors commit plain Enter through edit(); keep the press
                // armed but leave the key for that focused traversal.
                keyboard_owner_ = focus_;
                keyboard_key_ = event.key;
                record.activation = focus_;
            } else if (event.key == Key::space && !event.repeat && focus_ != Id{} &&
                       keyboard_owner_ == Id{}) {
                keyboard_owner_ = focus_;
                keyboard_key_ = event.key;
                record.consumed = true;
            }
            break;
        case EventKind::key_up:
            if (event.key == keyboard_key_ && keyboard_owner_ != Id{}) {
                if (keyboard_owner_ == focus_) record.activation = focus_;
                clear_press();
            }
            break;
        case EventKind::text:
        case EventKind::composition:
            record.activation = focus_;
            break;
        }
    }
}

Behavior Interaction::button(Id id, Rect bounds, bool enabled) {
    if (!active_ || id == Id{} ||
        std::find(seen_.begin(), seen_.end(), id) != seen_.end()) return {};
    seen_.push_back(id);
    if (!enabled) {
        if (focus_ == id) focus_ = {};
        if (capture_ == id) capture_ = {};
        if (keyboard_owner_ == id) clear_press();
        return {};
    }
    current_order_.push_back(id);
    Behavior behavior;
    bool owns_capture = initial_capture_ == id;
    bool owns_secondary = initial_secondary_ == id;
    for (std::size_t i = 0; i < events_.size(); ++i) {
        auto& record = events_[i];
        const auto& event = record.event;
        if (blocked() &&
            (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
             event.kind == EventKind::key_up))
            continue;
        switch (event.kind) {
        case EventKind::pointer_down:
            if (event.button == PointerButton::right) {
                owns_secondary = false;
                if (!record.consumed && contains(bounds, event.position)) {
                    record.consumed = true;
                    owns_secondary = true;
                    if (secondary_event_ == i) secondary_ = id;
                }
                break;
            }
            if (event.button != PointerButton::left) break;
            owns_capture = false;
            if (!record.consumed && contains(bounds, event.position)) {
                record.consumed = true;
                owns_capture = true;
                if (focus_event_ == i) focus_ = id;
                if (capture_event_ == i) capture_ = id;
            }
            break;
        case EventKind::pointer_up:
            if (event.button == PointerButton::right) {
                if (owns_secondary && !record.consumed) {
                    behavior.secondary_activated = behavior.secondary_activated ||
                                                   contains(bounds, event.position);
                    record.consumed = true;
                }
                owns_secondary = false;
                break;
            }
            if (event.button != PointerButton::left) break;
            if (owns_capture && !record.consumed) {
                behavior.activated = behavior.activated || contains(bounds, event.position);
                record.consumed = true;
            }
            owns_capture = false;
            break;
        case EventKind::focus_lost:
            owns_capture = false;
            owns_secondary = false;
            break;
        case EventKind::key_up:
            if (!record.consumed && record.activation == id) {
                behavior.activated = true;
                record.consumed = true;
            }
            break;
        default:
            break;
        }
    }
    behavior.hovered = !blocked() && has_pointer_ && contains(bounds, pointer_);
    behavior.held = !blocked() && (capture_ == id || keyboard_owner_ == id);
    behavior.focused = !blocked() && focus_ == id;
    return behavior;
}
DragResult Interaction::drag(Id id, Rect bounds, bool enabled) {
    drag_actions_.clear();
    DragResult result;
    if (!active_ || id == Id{} ||
        std::find(seen_.begin(), seen_.end(), id) != seen_.end()) return result;
    seen_.push_back(id);
    if (!enabled) {
        if (focus_ == id) focus_ = {};
        if (capture_ == id) capture_ = {};
        if (keyboard_owner_ == id) clear_press();
        return result;
    }
    current_order_.push_back(id);
    bool owns_capture = initial_capture_ == id;
    const auto accept = [&](Vec2 position) {
        drag_actions_.push_back({Key::none, position.x, position.y});
        result.moved = true;
        result.position_x = position.x;
        result.position_y = position.y;
    };
    for (std::size_t i = 0; i < events_.size(); ++i) {
        auto& record = events_[i];
        const auto& event = record.event;
        if (blocked() &&
            (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_move ||
             event.kind == EventKind::pointer_up || event.kind == EventKind::key_down))
            continue;
        if ((event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up) &&
            event.button != PointerButton::left)
            continue;
        switch (event.kind) {
        case EventKind::pointer_down:
            owns_capture = false;
            if (!record.consumed && contains(bounds, event.position)) {
                record.consumed = true;
                owns_capture = true;
                if (focus_event_ == i) focus_ = id;
                if (capture_event_ == i) capture_ = id;
                accept(event.position);
            }
            break;
        case EventKind::pointer_move:
            if (owns_capture && !record.consumed) accept(event.position);
            break;
        case EventKind::pointer_up:
            if (owns_capture && !record.consumed) {
                record.consumed = true;
                accept(event.position);
            }
            owns_capture = false;
            break;
        case EventKind::key_down:
            if (!record.consumed && record.activation == id &&
                (event.key == Key::left || event.key == Key::right ||
                 event.key == Key::home || event.key == Key::end)) {
                record.consumed = true;
                drag_actions_.push_back({event.key, 0, 0});
            }
            break;
        case EventKind::focus_lost:
            owns_capture = false;
            break;
        default:
            break;
        }
    }
    result.behavior.hovered = !blocked() && has_pointer_ && contains(bounds, pointer_);
    result.behavior.held = !blocked() && (capture_ == id || keyboard_owner_ == id);
    result.behavior.focused = !blocked() && focus_ == id;
    result.actions = drag_actions_;
    return result;
}


Behavior Interaction::popup_button(Id id, Rect bounds, bool enabled) {
    if (!active_ || popup_dismissed_ || id == Id{} ||
        std::find(seen_.begin(), seen_.end(), id) != seen_.end())
        return {};
    seen_.push_back(id);
    if (!enabled) {
        if (focus_ == id) focus_ = {};
        if (capture_ == id) capture_ = {};
        if (keyboard_owner_ == id) clear_press();
        return {};
    }
    current_order_.push_back(id);
    Behavior behavior;
    current_popup_order_.push_back(id);
    if (popup_owner_at_frame_start_ == Id{})
        return {.focused = focus_ == id};
    bool owns_capture = initial_capture_ == id;
    for (std::size_t i = 0; i < events_.size(); ++i) {
        auto& record = events_[i];
        const auto& event = record.event;
        if (popup_dismissed_) {
            if (event.kind == EventKind::pointer_down || event.kind == EventKind::pointer_up ||
                event.kind == EventKind::key_up) record.consumed = true;
            continue;
        }
        if (event.kind == EventKind::focus_lost ||
            (event.kind == EventKind::key_down && event.key == Key::escape)) {
            if (event.kind == EventKind::key_down) popup_escape_ = true;
            record.consumed = true;
            popup_dismissed_ = true;
            owns_capture = false;
        } else if (event.kind == EventKind::pointer_down ||
                   event.kind == EventKind::pointer_up) {
            if (!contains(popup_list_, event.position)) {
                record.consumed = true;
                popup_dismissed_ = true;
                owns_capture = false;
            } else if (event.button != PointerButton::left) {
                record.consumed = true;
            } else if (event.kind == EventKind::pointer_down) {
                owns_capture = false;
                if (!record.consumed && contains(bounds, event.position)) {
                    record.consumed = true;
                    owns_capture = true;
                    if (focus_event_ == i) focus_ = id;
                    if (capture_event_ == i) capture_ = id;
                }
            } else {
                if (owns_capture && !record.consumed) {
                    behavior.activated = behavior.activated || contains(bounds, event.position);
                    record.consumed = true;
                }
                owns_capture = false;
            }
        } else if (event.kind == EventKind::key_up &&
                   !record.consumed && record.activation == id) {
            behavior.activated = true;
            record.consumed = true;
        }
    }
    behavior.hovered = !popup_dismissed_ && has_pointer_ && contains(bounds, pointer_);
    behavior.held = !popup_dismissed_ && (capture_ == id || keyboard_owner_ == id);
    behavior.focused = !popup_dismissed_ && focus_ == id;
    return behavior;
}

void Interaction::popup_focus(Id id) {
    if (active_ &&
        std::find(current_popup_order_.begin(), current_popup_order_.end(), id) !=
            current_popup_order_.end()) {
        clear_press();
        focus_ = id;
    }
}

float Interaction::popup_wheel(Rect list, Id live_owner,
                                float current_offset, float max_offset) {
    float offset = std::clamp(current_offset, 0.0f, max_offset);
    if (popup_owner_at_frame_start_ == Id{}) return offset;
    for (auto& record : events_) {
        if (record.event.kind != EventKind::wheel) continue;
        if (live_owner == popup_owner_at_frame_start_ &&
            contains(list, record.event.position)) {
            offset = static_cast<float>(std::clamp(
                static_cast<double>(offset) - static_cast<double>(record.wheel_remaining) *
                    wheel_pixels_per_notch,
                0.0, static_cast<double>(max_offset)));
        }
        // Modal overflow and out-of-list wheel steps never reach the parent.
        record.wheel_remaining = 0.0f;
    }
    return offset;
}

float Interaction::take_wheel(Rect viewport, float current_offset, float max_offset) {
    if (blocked())
        return std::clamp(current_offset, 0.0f, max_offset);
    float offset = std::clamp(current_offset, 0.0f, max_offset);
    for (auto& record : events_) {
        if (record.event.kind != EventKind::wheel || record.consumed ||
            record.wheel_remaining == 0 || !contains(viewport, record.event.position))
            continue;
        const double requested = -static_cast<double>(record.wheel_remaining) *
                                 wheel_pixels_per_notch;
        const double applied = std::clamp(static_cast<double>(offset) + requested,
                                          0.0, static_cast<double>(max_offset)) - offset;
        offset = static_cast<float>(static_cast<double>(offset) + applied);
        record.wheel_remaining = applied == requested ? 0.0f :
            static_cast<float>(record.wheel_remaining + applied / wheel_pixels_per_notch);
    }
    return offset;
}

Vec2 Interaction::take_wheel_steps(Rect area) {
    Vec2 steps{};
    if (blocked()) return steps;
    for (auto& record : events_) {
        if (record.event.kind != EventKind::wheel || record.consumed ||
            !contains(area, record.event.position))
            continue;
        steps.x += record.event.wheel_x;
        steps.y += record.wheel_remaining;
        record.wheel_remaining = 0;
        record.consumed = true;
    }
    return steps;
}

bool Interaction::finish_layer(Rect bounds, bool modal) {
    bool dismiss = false;
    for (auto& record : events_) {
        const auto& event = record.event;
        switch (event.kind) {
        case EventKind::pointer_down:
            if (!record.consumed && !contains(bounds, event.position)) {
                record.consumed = true;
                dismiss = dismiss || !modal;
            }
            break;
        case EventKind::pointer_up:
            if (!record.consumed && !contains(bounds, event.position)) record.consumed = true;
            break;
        case EventKind::wheel:
            // Wheel steps never scroll content beneath a popup.
            record.wheel_remaining = 0;
            record.consumed = true;
            break;
        case EventKind::key_down:
            if (!record.consumed && event.key == Key::escape && !modal) {
                record.consumed = true;
                dismiss = true;
            }
            break;
        case EventKind::focus_lost:
            dismiss = dismiss || !modal;
            break;
        default:
            break;
        }
    }
    return dismiss;
}

std::optional<Vec2> Interaction::pointer() const {
    if (!has_pointer_) return std::nullopt;
    return pointer_;
}

void Interaction::end() {
    if (!active_) return;
    const auto eligible = [this](Id id) {
        return std::find(current_order_.begin(), current_order_.end(), id) != current_order_.end();
    };
    if (!eligible(focus_)) focus_ = {};
    if (!eligible(capture_)) capture_ = {};
    if (!eligible(secondary_)) secondary_ = {};
    if (!eligible(keyboard_owner_)) clear_press();
    previous_order_.swap(current_order_);
    previous_popup_order_.swap(current_popup_order_);
    active_ = false;
}

Id Interaction::focused() const { return focus_; }
Id Interaction::captured() const { return capture_; }

} // namespace wui
