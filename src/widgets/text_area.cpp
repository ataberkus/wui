#include "painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace wui::detail {
namespace {

std::string_view line_text(const EditState& editor, const EditLine& line) {
    return {editor.draft.data() + line.begin, line.end - line.begin};
}

// True when line `index` continues on the next line without a hard break.
bool soft_wrapped(const EditState& editor, std::size_t index) {
    return index + 1 < editor.lines.size() && editor.lines[index + 1].begin == editor.lines[index].end;
}

// Wraps the draft at `width` into byte-range lines. A line breaks after the
// last space that fits (spaces hang at the line end), else between
// graphemes; every byte except '\n' belongs to exactly one line.
bool wrap_draft(FrameState& frame, EditState& editor, float width) {
    editor.lines.clear();
    const std::string_view text{editor.draft.data(), editor.draft.size()};
    for (std::size_t start = 0;;) {
        const auto newline = text.find('\n', start);
        const std::size_t stop = newline == std::string_view::npos ? text.size() : newline;
        const auto paragraph = text.substr(start, stop - start);
        if (paragraph.empty()) {
            editor.lines.push_back({start, start});
        } else {
            const auto offsets = edit_boundaries(frame, paragraph);
            if (frame.error) return false;
            const std::size_t last = offsets.size() - 1;
            for (std::size_t first = 0; first < last;) {
                std::size_t low = first + 1, high = last, fit = first + 1;
                while (low <= high) {
                    const std::size_t mid = low + (high - low) / 2;
                    const auto run = frame.shape(
                        paragraph.substr(offsets[first], offsets[mid] - offsets[first]), FontRole::body);
                    if (!run) return false;
                    if (run->width <= width) {
                        fit = mid;
                        low = mid + 1;
                    } else {
                        high = mid - 1;
                    }
                }
                std::size_t end = fit;
                if (end < last && paragraph[offsets[end]] == ' ') {
                    while (end < last && paragraph[offsets[end]] == ' ') ++end;
                } else if (end < last) {
                    const auto space = paragraph.rfind(' ', offsets[end] - 1);
                    if (space != std::string_view::npos && space >= offsets[first]) {
                        const auto from = offsets.begin() + static_cast<std::ptrdiff_t>(first + 1);
                        const auto to = offsets.begin() + static_cast<std::ptrdiff_t>(end + 1);
                        const auto after = std::lower_bound(from, to, space + 1);
                        if (after != to && *after == space + 1)
                            end = static_cast<std::size_t>(after - offsets.begin());
                    }
                }
                editor.lines.push_back({start + offsets[first], start + offsets[end]});
                first = end;
            }
        }
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    editor.layout_width = width;
    return true;
}

std::size_t line_of(const EditState& editor, std::size_t offset) {
    const auto found = std::upper_bound(editor.lines.begin(), editor.lines.end(), offset,
        [](std::size_t value, const EditLine& line) { return value < line.begin; });
    return found == editor.lines.begin() ? 0
                                         : static_cast<std::size_t>(found - editor.lines.begin()) - 1;
}

float prefix_width(FrameState& frame, const EditState& editor, std::size_t index, std::size_t offset) {
    const auto& line = editor.lines[index];
    const std::size_t end = std::clamp(offset, line.begin, line.end);
    if (end == line.begin) return 0;
    const auto run = frame.shape(line_text(editor, line).substr(0, end - line.begin), FontRole::body);
    return run ? run->width : 0;
}

// Last caret position that still paints on line `index`: before the final
// grapheme of a soft-wrapped line, whose end is the next line's start.
std::size_t line_end(FrameState& frame, const EditState& editor, std::size_t index) {
    const auto& line = editor.lines[index];
    if (!soft_wrapped(editor, index)) return line.end;
    const auto offsets = edit_boundaries(frame, line_text(editor, line));
    if (frame.error || offsets.size() < 2) return line.begin;
    return line.begin + offsets[offsets.size() - 2];
}

// Grapheme boundary on line `index` nearest to `x` (content coordinates).
std::size_t offset_at(FrameState& frame, const EditState& editor, std::size_t index, float x) {
    const auto& line = editor.lines[index];
    const auto text = line_text(editor, line);
    if (text.empty() || x <= 0) return line.begin;
    const auto offsets = edit_boundaries(frame, text);
    if (frame.error) return line.begin;
    std::size_t last = offsets.size() - 1;
    if (soft_wrapped(editor, index) && last > 0) --last;
    const auto width = [&](std::size_t i) {
        return prefix_width(frame, editor, index, line.begin + offsets[i]);
    };
    std::size_t low = 0, high = last;
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        if (width(mid) < x) low = mid + 1;
        else high = mid;
    }
    if (low > 0 && x - width(low - 1) < width(low) - x) --low;
    return line.begin + offsets[low];
}

// Multi-line keys first; everything else is the shared single-line editor.
bool area_event(FrameState& frame, EditState& editor, const InputEvent& event, std::size_t page) {
    if (event.kind == EventKind::key_down) {
        const auto place = [&](std::size_t caret) {
            editor.caret = caret;
            if (!event.shift) editor.anchor = caret;
        };
        const auto vertical = [&](std::ptrdiff_t lines) {
            const std::size_t current = line_of(editor, editor.caret);
            if (editor.preferred_x < 0) editor.preferred_x = prefix_width(frame, editor, current, editor.caret);
            const auto target = static_cast<std::ptrdiff_t>(current) + lines;
            if (target < 0) return place(0);
            if (target >= static_cast<std::ptrdiff_t>(editor.lines.size())) return place(editor.draft.size());
            place(offset_at(frame, editor, static_cast<std::size_t>(target), editor.preferred_x));
        };
        const auto step = static_cast<std::ptrdiff_t>(page);
        switch (event.key) {
        case Key::up: vertical(-1); return false;
        case Key::down: vertical(1); return false;
        case Key::page_up: vertical(-step); return false;
        case Key::page_down: vertical(step); return false;
        default: break;
        }
        editor.preferred_x = -1;
        switch (event.key) {
        case Key::home:
            place(event.control ? 0 : editor.lines[line_of(editor, editor.caret)].begin);
            return false;
        case Key::end:
            place(event.control ? editor.draft.size()
                                : line_end(frame, editor, line_of(editor, editor.caret)));
            return false;
        case Key::enter:
            if (!snapshot_edit_undo(frame, editor)) return false;
            replace_edit_selection(editor, "\n");
            return true;
        case Key::v: {
            if (!event.control || event.repeat) break;
            if (!frame.clipboard) {
                frame.note_diagnostic(ErrorCode::unsupported, "Clipboard service is unavailable");
                return false;
            }
            auto pasted = frame.clipboard->clipboard();
            if (auto* failed = std::get_if<Error>(&pasted)) {
                frame.note_diagnostic(failed->code, failed->message.c_str());
                return false;
            }
            // Platform line breaks (CRLF, CR) become the draft's '\n'.
            const auto& raw = std::get<std::string>(pasted);
            std::string text;
            text.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] != '\r') {
                    text += raw[i];
                    continue;
                }
                text += '\n';
                if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
            }
            if (!snapshot_edit_undo(frame, editor)) return false;
            replace_edit_selection(editor, text);
            return true;
        }
        default: break;
        }
    } else if (event.kind == EventKind::text) {
        editor.preferred_x = -1;
    }
    return process_edit_event(frame, editor, event);
}

} // namespace

bool emit_text_area(FrameState& frame, std::string_view key, std::string_view label,
                    std::string& value, float height, WidgetOptions options) {
    if (!frame.check_canvas()) return false;
    if (label.empty()) {
        frame.report(ErrorCode::invalid_argument, "Text area needs an accessible label", true);
        return false;
    }
    if (!std::isfinite(height) || height < 0) {
        frame.report(ErrorCode::invalid_argument, "Text area height must be finite and nonnegative", true);
        return false;
    }
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return false;
    const Theme theme = widget_theme(frame, options);
    if (frame.error) return false;
    const auto probe = frame.shape(label, FontRole::body);
    if (!probe) return false;
    const float line_height = probe->height;
    const Rect row = frame.take(height > 0 ? height : line_height * 4 + theme.gap * 2);
    if (frame.error || !frame.check_canvas()) return false;
    const Rect hit = intersect(row, frame.clip());
    initialize_animation(*state, false);
    auto* editor = frame.edit_state(id);
    if (!editor) return false;

    // An application write replaces the draft, like text_input.
    if (value.size() > frame.max_retained_bytes) {
        frame.report(ErrorCode::capacity, "Text model exceeds retained byte limit");
        return false;
    }
    if (std::string_view{editor->last_published.data(), editor->last_published.size()} != value) {
        if (!frame.account_edit_bytes(value.size())) return false;
        editor->draft.assign(value.data(), value.size());
        editor->last_published.assign(value.data(), value.size());
        editor->caret = editor->anchor = editor->draft.size();
        editor->composition.clear();
        editor->composition_active = false;
        editor->undo.clear();
        editor->undo_bytes = 0;
        editor->layout_width = -1;
        editor->preferred_x = -1;
    }

    constexpr float gutter = 6; // scroll indicator lane, reserved so text never reflows
    const Rect content = inset(row, theme.gap);
    const float wrap_width = std::max(1.0f, content.w - gutter);
    if (editor->layout_width != wrap_width && !wrap_draft(frame, *editor, wrap_width)) return false;
    const std::size_t caret_before = editor->caret, anchor_before = editor->anchor;
    const auto page = static_cast<std::size_t>(std::max(1.0f, std::floor(content.h / line_height) - 1));
    bool changed = false;
    if (options.enabled) {
        frame.interaction.keyboard(id, [&](const InputEvent& event) {
            if (frame.error) return;
            if (area_event(frame, *editor, event, page)) {
                changed = true;
                wrap_draft(frame, *editor, wrap_width);
            }
        });
        if (frame.error) return false;
    }
    if (frame.interaction.focused() != id) {
        editor->composition.clear();
        editor->composition_active = false;
    }

    // Pointer: a press places the caret, a captured drag extends the selection.
    const auto drag = frame.interaction.drag(id, hit, options.enabled && visible(hit));
    float offset = state->scroll_offset;
    bool fresh = !state->dragging;
    for (const auto& action : drag.actions) {
        if (action.key != Key::none) continue;
        const float y = action.position_y - content.y + offset;
        const auto index = static_cast<std::size_t>(std::clamp(
            std::floor(y / line_height), 0.0f, static_cast<float>(editor->lines.size() - 1)));
        editor->caret = offset_at(frame, *editor, index, action.position_x - content.x);
        if (fresh) editor->anchor = editor->caret;
        fresh = false;
        editor->preferred_x = -1;
    }
    if (frame.error) return false;
    state->dragging = frame.interaction.captured() == id;

    const std::string_view draft{editor->draft.data(), editor->draft.size()};
    if (draft != value) {
        value.assign(draft.data(), draft.size());
        frame.text_model_changed = true;
        editor->last_published.assign(draft.data(), draft.size());
        changed = true;
    }

    // Keep the caret line visible after it moved, then apply the wheel.
    const float content_height = static_cast<float>(editor->lines.size()) * line_height;
    const float max_offset = std::max(0.0f, content_height - content.h);
    offset = std::clamp(offset, 0.0f, max_offset);
    if (changed || editor->caret != caret_before || editor->anchor != anchor_before) {
        const float caret_y = static_cast<float>(line_of(*editor, editor->caret)) * line_height;
        if (caret_y < offset) offset = caret_y;
        if (caret_y + line_height > offset + content.h) offset = caret_y + line_height - content.h;
        offset = std::clamp(offset, 0.0f, max_offset);
    }
    state->scroll_offset = frame.settle_scroll(hit, offset, max_offset);
    offset = state->scroll_offset;

    Behavior behavior = drag.behavior;
    behavior.focused = frame.interaction.focused() == id && options.enabled;
    animate(frame, *state, behavior, false);
    track_feedback(*state, hit, behavior, options.enabled);
    editor->caret_point.reset();
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return changed;
        paint_surface(frame, row, button_fill(theme, *state, behavior, options.enabled),
                      behavior.focused, theme, options.invalid);
        if (!frame.check_canvas()) return changed;
        WidgetClip text_clip(frame, intersect(content, hit));
        if (!frame.check_canvas()) return changed;
        const float top = content.y - offset;
        const auto first = static_cast<std::size_t>(std::floor(offset / line_height));
        const auto last = std::min(editor->lines.size(),
            static_cast<std::size_t>(std::ceil((offset + content.h) / line_height)));
        if (draft.empty()) {
            const auto hint = frame.shape(options.placeholder.empty() ? label : options.placeholder,
                                          FontRole::body);
            if (!hint) return changed;
            frame.glyphs(*hint, {content.x, content.y}, theme.secondary);
        }
        const std::size_t from = std::min(editor->caret, editor->anchor);
        const std::size_t to = std::max(editor->caret, editor->anchor);
        const Color ink = options.enabled ? theme.text : theme.secondary;
        for (std::size_t i = first; i < last && frame.check_canvas(); ++i) {
            const auto& line = editor->lines[i];
            const float y = top + static_cast<float>(i) * line_height;
            if (from != to && from <= line.end && to > line.begin) {
                const float left = prefix_width(frame, *editor, i, std::max(from, line.begin));
                // A selected hard break shows as a short stub past the text.
                float right = prefix_width(frame, *editor, i, std::min(to, line.end));
                if (to > line.end && !soft_wrapped(*editor, i)) right += theme.gap * 0.5f;
                frame.canvas.rounded_rect({content.x + left, y, right - left, line_height}, 0, theme.accent);
            }
            if (line.end > line.begin) {
                const auto run = frame.shape(line_text(*editor, line), FontRole::body);
                if (!run) return changed;
                frame.glyphs(*run, {content.x, y}, ink);
            }
        }
        if (!frame.check_canvas()) return changed;
        if (behavior.focused) {
            const std::size_t index = line_of(*editor, editor->caret);
            const float x = content.x + prefix_width(frame, *editor, index, editor->caret);
            const float y = top + static_cast<float>(index) * line_height;
            const std::array<Vec2, 2> caret{{{x, y}, {x, y + line_height}}};
            frame.canvas.line(caret, 1, theme.text);
            if (!frame.check_canvas()) return changed;
            editor->caret_point = Vec2{x, y + line_height};
        }
        if (max_offset > 0) {
            const float thumb = std::max(line_height, content.h * content.h / content_height);
            const float y = content.y + (content.h - thumb) * (offset / max_offset);
            frame.canvas.rounded_rect({content.x + content.w - 3, y, 3, thumb}, 1.5f,
                                      mix(theme.control, theme.text, 0.35f));
        }
    }
    if (!frame.check_canvas()) return changed;
    if (frame.copy_label(draft))
        frame.canvas.semantic(id, Role::text_area, label, row, options.enabled, behavior.focused,
                              behavior.held, draft, options.invalid);
    frame.check_canvas();
    return changed;
}

} // namespace wui::detail
