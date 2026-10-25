#include "painter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>
#include <variant>

namespace wui::detail {

std::vector<std::size_t> edit_boundaries(FrameState& frame, std::string_view text) {
    auto result = frame.text->grapheme_offsets(text);
    if (auto* failed = std::get_if<Error>(&result)) {
        frame.report(std::move(*failed));
        return {};
    }
    auto offsets = std::get<std::vector<std::size_t>>(std::move(result));
    if (offsets.empty() || offsets.front() != 0 || offsets.back() != text.size()) {
        frame.report(ErrorCode::invalid_font, "Text service returned invalid grapheme offsets", true);
        return {};
    }
    for (std::size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] <= offsets[i - 1]) {
            frame.report(ErrorCode::invalid_font, "Text service returned unordered grapheme offsets", true);
            return {};
        }
    }
    return offsets;
}

bool snapshot_edit_undo(FrameState& frame, EditState& editor) {
    const std::size_t bytes = editor.draft.size();
    if (!frame.account_edit_bytes(bytes)) return false;
    if (editor.undo.size() == 64 || editor.undo_bytes + bytes > 64 * 1024) {
        frame.label_bytes -= editor.undo.front().text.size();
        editor.undo.erase(editor.undo.begin());
    }
    editor.undo.push_back(EditSnapshot{
        std::pmr::string(editor.draft.data(), editor.draft.size(), &frame.memory),
        editor.caret, editor.anchor});
    editor.undo_bytes += bytes;
    return true;
}

void replace_edit_selection(EditState& editor, std::string_view insert) {
    const auto from = std::min(editor.caret, editor.anchor);
    const auto to = std::max(editor.caret, editor.anchor);
    editor.draft.replace(from, to - from, insert.data(), insert.size());
    editor.caret = editor.anchor = from + insert.size();
}

namespace {

std::size_t step_before(const std::vector<std::size_t>& offsets, std::size_t caret) {
    const auto it = std::lower_bound(offsets.begin(), offsets.end(), caret);
    if (it == offsets.begin()) return 0;
    return *(it - 1);
}

std::size_t step_after(const std::vector<std::size_t>& offsets, std::size_t caret) {
    const auto it = std::upper_bound(offsets.begin(), offsets.end(), caret);
    return it == offsets.end() ? offsets.back() : *it;
}
} // namespace

bool process_edit_event(FrameState& frame, EditState& editor, const wui::InputEvent& event) {
    if (event.kind == wui::EventKind::text) {
        if (!snapshot_edit_undo(frame, editor)) return false;
        replace_edit_selection(editor, event.text);
        return true;
    }
    if (event.kind == wui::EventKind::composition) {
        editor.composition.assign(event.text.data(), event.text.size());
        editor.composition_active = !event.text.empty();
        return false;
    }
    if (event.kind != wui::EventKind::key_down) return false;
    if (event.control && !event.repeat &&
        (event.key == Key::c || event.key == Key::x || event.key == Key::v)) {
        const auto from = std::min(editor.caret, editor.anchor);
        const auto to = std::max(editor.caret, editor.anchor);
        const std::string_view selection{editor.draft.data() + from, to - from};
        if (event.key == Key::v) {
            if (!frame.clipboard) {
                frame.note_diagnostic(ErrorCode::unsupported, "Clipboard service is unavailable");
                return false;
            }
            auto pasted = frame.clipboard->clipboard();
            if (auto* failed = std::get_if<Error>(&pasted)) {
                frame.note_diagnostic(failed->code, failed->message.c_str());
                return false;
            }
            if (!snapshot_edit_undo(frame, editor)) return false;
            const auto& text = std::get<std::string>(pasted);
            replace_edit_selection(editor, {text.data(), text.size()});
            return true;
        }
        if (from == to) return false;
        if (!frame.clipboard) {
            frame.note_diagnostic(ErrorCode::unsupported, "Clipboard service is unavailable");
            return false;
        }
        if (auto failed = frame.clipboard->set_clipboard(selection)) {
            frame.note_diagnostic(failed->code, failed->message.c_str());
            return false;
        }
        if (event.key == Key::x) {
            if (!snapshot_edit_undo(frame, editor)) return false;
            replace_edit_selection(editor, {});
            return true;
        }
        return false;
    }
    const auto move = [&](std::size_t next) {
        editor.caret = next;
        if (!event.shift) editor.anchor = next;
    };
    switch (event.key) {
    case Key::left: {
        auto offsets = edit_boundaries(frame, {editor.draft.data(), editor.draft.size()});
        if (frame.error) return false;
        move(step_before(offsets, editor.caret));
        return false;
    }
    case Key::right: {
        auto offsets = edit_boundaries(frame, {editor.draft.data(), editor.draft.size()});
        if (frame.error) return false;
        move(step_after(offsets, editor.caret));
        return false;
    }
    case Key::home: move(0); return false;
    case Key::end: move(editor.draft.size()); return false;
    case Key::backspace:
    case Key::del: {
        if (editor.caret != editor.anchor) {
            if (!snapshot_edit_undo(frame, editor)) return false;
            replace_edit_selection(editor, {});
            return true;
        }
        auto offsets = edit_boundaries(frame, {editor.draft.data(), editor.draft.size()});
        if (frame.error) return false;
        const std::size_t next = event.key == Key::backspace
            ? step_before(offsets, editor.caret) : step_after(offsets, editor.caret);
        if (next == editor.caret) return false;
        if (!snapshot_edit_undo(frame, editor)) return false;
        const auto from = std::min(next, editor.caret);
        editor.draft.erase(from, std::max(next, editor.caret) - from);
        editor.caret = editor.anchor = from;
        return true;
    }
    case Key::a:
        if (event.control && !event.repeat) {
            editor.anchor = 0;
            editor.caret = editor.draft.size();
        }
        return false;
    case Key::z: {
        if (!event.control || event.repeat || editor.undo.empty()) return false;
        auto snapshot = std::move(editor.undo.back());
        editor.undo.pop_back();
        editor.undo_bytes -= snapshot.text.size();
        frame.label_bytes -= snapshot.text.size();
        editor.draft = std::move(snapshot.text);
        editor.caret = snapshot.caret;
        editor.anchor = snapshot.anchor;
        return true;
    }
    default: return false;
    }
}

bool FrameState::edit_text_model(Id id, std::string& value, bool focused, bool enabled) {
    auto* editor = edit_state(id);
    if (!editor) return false;
    if (value.size() > max_retained_bytes) {
        report(ErrorCode::capacity, "Text model exceeds retained byte limit");
        return false;
    }
    const std::string_view published{editor->last_published.data(), editor->last_published.size()};
    if (published != value) {
        if (!account_edit_bytes(value.size())) return false;
        editor->draft.assign(value.data(), value.size());
        editor->last_published.assign(value.data(), value.size());
        editor->caret = editor->anchor = editor->draft.size();
        editor->composition.clear();
        editor->composition_active = false;
        editor->undo.clear();
        editor->undo_bytes = 0;
    }
    bool changed = false;
    if (focused && enabled) {
        interaction.edit(id, enabled, [&](const InputEvent& event) {
            if (process_edit_event(*this, *editor, event)) changed = true;
        });
        if (error) return false;
    } else if (!focused) {
        editor->composition.clear();
        editor->composition_active = false;
    }
    const std::string_view updated{editor->draft.data(), editor->draft.size()};
    if (updated != value) {
        value.assign(updated.data(), updated.size());
        text_model_changed = true;
        editor->last_published.assign(value.data(), value.size());
        changed = true;
    }
    return changed;
}
void FrameState::paint_text_input(Id id, Rect bounds, const Theme& theme, Behavior behavior,
                                  WidgetState& widget, bool enabled, bool invalid,
                                  std::string_view label, std::string_view placeholder) {
    const bool focused = behavior.focused && enabled;
    auto* editor = edit_state(id);
    if (!editor) return;
    const std::string_view display = edit_display(id);
    const std::string_view hint = placeholder.empty() ? label : placeholder;
    const auto run = shape(display.empty() ? hint : display, FontRole::body);
    if (!run) return;
    const float pad = std::min(theme.gap, bounds.w * 0.25f);
    const Rect content{bounds.x + pad, bounds.y, std::max(0.0f, bounds.w - pad * 2), bounds.h};
    {
        WidgetClip clip(*this, intersect(bounds, this->clip()));
        if (!check_canvas()) return;
        paint_surface(*this, bounds, button_fill(theme, widget, behavior, enabled),
                      behavior.focused, theme, invalid);
        if (!check_canvas()) return;
        WidgetClip text_clip(*this, content);
        if (!check_canvas()) return;
        const float text_y = bounds.y + (bounds.h - run->height) * 0.5f;
        if (editor->caret != editor->anchor) {
            const auto from = shape(display.substr(0, std::min(editor->caret, editor->anchor)), FontRole::body);
            const auto to = shape(display.substr(0, std::max(editor->caret, editor->anchor)), FontRole::body);
            if (!from || !to) return;
            const Rect selected{content.x + from->width, text_y, to->width - from->width, run->height};
            canvas.rounded_rect(selected, 0, theme.accent);
            if (!check_canvas()) return;
        }
        glyphs(*run, {content.x, text_y}, display.empty() || !enabled ? theme.secondary : theme.text);
        if (!check_canvas()) return;
        if (focused) {
            const auto caret_run = shape(display.substr(0, std::min(editor->caret, display.size())), FontRole::body);
            if (!caret_run) return;
            const float caret_x = content.x + caret_run->width;
            const std::array<Vec2, 2> caret{{{caret_x, text_y}, {caret_x, text_y + run->height}}};
            canvas.line(caret, 1, theme.text);
            if (!check_canvas()) return;
        }
    }
    if (!check_canvas()) return;
    if (copy_label(display))
        canvas.semantic(id, Role::text_input, label, bounds, enabled, behavior.focused,
                        behavior.held, display, invalid);
    check_canvas();
}

bool emit_text_edit(FrameState& frame, std::string_view key, std::string_view label,
                    std::string& value, WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    if (label.empty()) {
        frame.report(ErrorCode::invalid_argument, "Text input needs an accessible label", true);
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
    // The edit model runs exactly once per frame: a second unfocused pass
    // would discard the IME composition that call just received.
    const bool changed = frame.edit_text_model(id, value, frame.interaction.focused() == id,
                                               options.enabled);
    Behavior behavior;
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return changed;
        behavior = frame.interaction.button(id, hit, options.enabled && visible(hit));
        // Focused editing already claimed this ID's routing, so button()
        // reports no state for it; focus itself is persistent.
        behavior.focused = frame.interaction.focused() == id && options.enabled;
        animate(frame, *state, behavior, false);
    }
    if (!frame.check_canvas()) return changed;
    track_feedback(*state, hit, behavior, options.enabled);
    frame.paint_text_input(id, row, theme, behavior, *state, options.enabled, options.invalid,
                           label, options.placeholder);
    return changed;
}

} // namespace wui::detail
