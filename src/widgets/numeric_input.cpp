#include "painter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <string_view>
#include <type_traits>

namespace wui::detail {
namespace {

// Committed values are formatted into a bounded stack buffer so warmed frames
// do not allocate to compare or resynchronize the displayed number.
struct Formatted {
    std::array<char, 64> bytes{};
    std::size_t size = 0;
    std::string_view view() const { return {bytes.data(), size}; }
};

template<class T>
bool format_committed(FrameState& frame, T value, Formatted& out) {
    const auto [end, error] = std::to_chars(out.bytes.data(), out.bytes.data() + out.bytes.size(), value);
    if (error != std::errc{}) {
        frame.report(ErrorCode::capacity, "Numeric value could not be formatted");
        return false;
    }
    out.size = static_cast<std::size_t>(end - out.bytes.data());
    return true;
}

std::string_view draft_of(const EditState& editor) {
    return {editor.draft.data(), editor.draft.size()};
}

template<class T>
bool emit_numeric_impl(FrameState& frame, std::string_view key, std::string_view label,
                       T& value, T min, T max, WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    if (label.empty()) {
        frame.report(ErrorCode::invalid_argument, "Numeric input needs an accessible label", true);
        return false;
    }
    if (min > max || !std::isfinite(static_cast<double>(min)) ||
        !std::isfinite(static_cast<double>(max)) ||
        !std::isfinite(static_cast<double>(value)) || value < min || value > max) {
        frame.report(ErrorCode::invalid_argument, "Numeric bounds and value must be finite with min <= max", true);
        return false;
    }
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return false;
    const Theme theme = widget_theme(frame, options);
    if (frame.error) return false;
    const Rect row = frame.take(theme.control_height);
    if (frame.error || !frame.check_canvas()) return false;
    const Rect hit = intersect(row, frame.clip());
    initialize_animation(*state, false);
    auto* editor = frame.edit_state(id);
    if (!editor) return false;

    bool changed = false;
    // Replaces the draft with the committed number and clears selection,
    // composition and undo history.
    const auto show_committed = [&] {
        Formatted text;
        if (!format_committed(frame, value, text)) return false;
        const auto committed = text.view();
        if (!frame.account_edit_bytes(committed.size())) return false;
        editor->draft.assign(committed.data(), committed.size());
        editor->last_published.assign(committed.data(), committed.size());
        editor->caret = editor->anchor = editor->draft.size();
        editor->composition.clear();
        editor->composition_active = false;
        editor->undo.clear();
        editor->undo_bytes = 0;
        return true;
    };
    // Accepts the whole draft only when it parses as a finite in-range T.
    const auto commit_draft = [&] {
        T parsed{};
        if (!parse_numeric(draft_of(*editor), min, max, parsed)) return false;
        if (parsed != value) {
            value = parsed;
            changed = true;
        }
        return true;
    };

    Formatted committed;
    if (!format_committed(frame, value, committed)) return false;
    const std::string_view published{editor->last_published.data(), editor->last_published.size()};
    const bool pending = draft_of(*editor) != published;
    const bool focused_before = frame.interaction.focused() == id && options.enabled;
    if (!focused_before) {
        // Focus left with an uncommitted draft: keep it only when valid,
        // otherwise the settled widget shows the previous committed value.
        if (pending) commit_draft();
        if ((pending || draft_of(*editor) != committed.view()) && !show_committed()) return changed;
    } else if (!pending && published != committed.view() && !show_committed()) {
        return changed;
    }

    if (focused_before) {
        frame.interaction.edit(id, options.enabled, [&](const InputEvent& event) {
            if (frame.error) return;
            if (event.kind == EventKind::key_down && !event.repeat && event.key == Key::escape) {
                show_committed();
                return;
            }
            if (event.kind == EventKind::key_down && !event.repeat && event.key == Key::enter &&
                !event.control && !event.shift) {
                if (commit_draft()) show_committed();
                return;
            }
            process_edit_event(frame, *editor, event);
        });
        if (frame.error) return changed;
    }

    Behavior behavior;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return changed;
        behavior = frame.interaction.button(id, hit, options.enabled && visible(hit));
        // Focused editing already claimed this ID's routing; focus is persistent.
        behavior.focused = frame.interaction.focused() == id && options.enabled;
        animate(frame, *state, behavior, false);
    }
    if (!frame.check_canvas()) return changed;
    track_feedback(*state, hit, behavior, options.enabled);
    const bool focused = behavior.focused;

    const std::string_view display = frame.edit_display(id);
    T parsed{};
    const bool valid = parse_numeric(display, min, max, parsed);
    const bool invalid = options.invalid || !valid;
    const std::string_view hint = options.placeholder.empty() ? label : options.placeholder;
    const auto run = frame.shape(display.empty() ? hint : display, FontRole::body);
    if (!run) return changed;
    const float pad = std::min(theme.gap, row.w * 0.25f);
    const Rect content{row.x + pad, row.y, std::max(0.0f, row.w - pad * 2), row.h};
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return changed;
        paint_surface(frame, row, button_fill(theme, *state, behavior, options.enabled),
                      focused, theme, invalid);
        if (!frame.check_canvas()) return changed;
        WidgetClip text_clip(frame, content);
        if (!frame.check_canvas()) return changed;
        const float text_y = row.y + (row.h - run->height) * 0.5f;
        const auto from = std::min({editor->caret, editor->anchor, display.size()});
        const auto to = std::min(std::max(editor->caret, editor->anchor), display.size());
        if (focused && from != to) {
            const auto before = frame.shape(display.substr(0, from), FontRole::body);
            const auto through = frame.shape(display.substr(0, to), FontRole::body);
            if (!before || !through) return changed;
            const Rect selected{content.x + before->width, text_y,
                                through->width - before->width, run->height};
            frame.canvas.rounded_rect(selected, 0, theme.accent);
            if (!frame.check_canvas()) return changed;
        }
        frame.glyphs(*run, {content.x, text_y},
                     display.empty() || !options.enabled ? theme.secondary
                     : !valid ? invalid_ink(theme) : theme.text);
        if (!frame.check_canvas()) return changed;
        if (focused) {
            const auto caret_run = frame.shape(
                display.substr(0, std::min(editor->caret, display.size())), FontRole::body);
            if (!caret_run) return changed;
            const float caret_x = content.x + caret_run->width;
            const std::array<Vec2, 2> caret{{{caret_x, text_y}, {caret_x, text_y + run->height}}};
            frame.canvas.line(caret, 1, theme.text);
            if (!frame.check_canvas()) return changed;
        }
    }
    if (!frame.check_canvas()) return changed;
    if (frame.copy_label(display))
        frame.canvas.semantic(id, Role::numeric_input, label, row, options.enabled,
                              focused, behavior.held, display, invalid);
    frame.check_canvas();
    return changed;
}

} // namespace

template<class T>
bool emit_numeric_edit(FrameState& frame, std::string_view key, std::string_view label,
                       T& value, T min, T max, WidgetOptions options) {
    static_assert(std::is_same_v<T, double> || std::is_same_v<T, float> || std::is_same_v<T, int>,
                  "numeric input supports double, float and int");
    return emit_numeric_impl(frame, key, label, value, min, max, options);
}

template bool emit_numeric_edit<double>(FrameState&, std::string_view, std::string_view,
                                        double&, double, double, WidgetOptions);
template bool emit_numeric_edit<float>(FrameState&, std::string_view, std::string_view,
                                       float&, float, float, WidgetOptions);
template bool emit_numeric_edit<int>(FrameState&, std::string_view, std::string_view,
                                     int&, int, int, WidgetOptions);

} // namespace wui::detail
