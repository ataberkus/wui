#include "painter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>

namespace wui::detail {
namespace {

constexpr std::size_t none = static_cast<std::size_t>(-1);
constexpr float edge_grab = 4; // header edge half-width that starts a resize

Id indexed_id(Id parent, std::string_view kind, std::size_t index) {
    char digits[24];
    const auto end = std::to_chars(digits, digits + sizeof digits, index).ptr;
    return child_id(child_id(parent, kind), {digits, static_cast<std::size_t>(end - digits)});
}

bool valid_columns(std::span<const GridColumn> columns) {
    return std::all_of(columns.begin(), columns.end(), [](const GridColumn& column) {
        return std::isfinite(column.width) && column.width > 0 && std::isfinite(column.min_width) &&
               column.min_width >= 0;
    });
}

} // namespace

GridResult emit_data_grid(FrameState& frame, std::string_view key, std::string_view label,
                          std::span<const GridColumn> columns, std::size_t rows, GridCells cells,
                          std::optional<std::size_t>& selected, GridSort& sort, float height,
                          WidgetOptions options) {
    GridResult result;
    if (!frame.check_canvas()) return result;
    if (label.empty() || columns.empty() || columns.size() > 256 || !cells.text || !valid_columns(columns) ||
        !std::isfinite(height) || height <= 0 ||
        (sort.direction != SortDirection::none && sort.column >= columns.size())) {
        frame.report(ErrorCode::invalid_argument,
                     "Data grid needs a label, 1..256 valid columns, a cell provider, a positive height "
                     "and a sort column in range", true);
        return result;
    }
    if (selected && *selected >= rows) {
        selected.reset();
        result.selection_changed = true;
    }
    const Id id = frame.keyed_id(key);
    auto* state = frame.register_widget(id);
    if (!state || !frame.copy_label(label)) return result;
    auto* grid = frame.grid_state(id);
    if (!grid) return result;
    const Theme theme = widget_theme(frame, options);
    if (frame.error) return result;
    const auto probe = frame.shape(label, FontRole::body);
    if (!probe) return result;
    const float row_height = std::round(probe->height + theme.gap);
    const float pad = theme.gap;
    const Rect box = frame.take(height);
    if (frame.error || !frame.check_canvas()) return result;
    const Rect hit = intersect(box, frame.clip());
    initialize_animation(*state, false);
    const bool enabled = options.enabled && visible(hit);

    if (grid->widths.size() != columns.size()) {
        grid->widths.clear();
        for (const auto& column : columns) grid->widths.push_back(std::max(column.width, column.min_width));
        grid->resizing = none;
    }
    const float header_height = std::min(row_height, box.h);
    const Rect header{box.x, box.y, box.w, header_height};
    const Rect body{box.x, box.y + header_height, box.w, box.h - header_height};
    const Rect body_hit = intersect(body, hit);
    const auto total_width = [&] {
        float sum = 0;
        for (const float width : grid->widths) sum += width;
        return sum;
    };
    const auto column_left = [&](std::size_t column) {
        float x = box.x - grid->scroll_x;
        for (std::size_t i = 0; i < column; ++i) x += grid->widths[i];
        return x;
    };

    // Column resize: a press within edge_grab of a header's right edge, kept
    // while that drag holds capture. Idle, the handle is disabled, so it is
    // never a Tab stop.
    const Id resize_id = child_id(id, "resize");
    std::size_t edge = grid->resizing;
    if (edge == none && enabled && !frame.input_blocked()) {
        for (const auto& event : frame.interaction.events()) {
            if (event.kind != EventKind::pointer_down || event.button != PointerButton::left ||
                !contains(intersect(header, hit), event.position)) continue;
            for (std::size_t i = 0; i < columns.size() && edge == none; ++i) {
                if (std::abs(event.position.x - (column_left(i) + grid->widths[i])) <= edge_grab) edge = i;
            }
            break;
        }
    }
    {
        const Rect handle = edge == none ? Rect{}
            : intersect({column_left(edge) + grid->widths[edge] - edge_grab, box.y, edge_grab * 2,
                         header_height}, hit);
        const auto drag = frame.interaction.drag(resize_id, handle, enabled && edge != none);
        for (const auto& action : drag.actions) {
            if (action.key != Key::none) continue;
            if (grid->resizing == none) {
                grid->resizing = edge;
                grid->grab = action.position_x - grid->widths[edge];
            } else {
                grid->widths[edge] = std::max(columns[edge].min_width, action.position_x - grid->grab);
            }
        }
        if (frame.interaction.captured() != resize_id) grid->resizing = none;
    }

    // Header buttons toggle sorting: a new column starts ascending.
    std::size_t header_focus = none;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!columns[i].sortable) continue;
        const Rect cell = intersect({column_left(i), box.y, grid->widths[i], header_height}, intersect(header, hit));
        const auto behavior = frame.interaction.button(indexed_id(id, "h", i), cell,
                                                       enabled && visible(cell));
        if (behavior.focused) header_focus = i;
        if (!behavior.activated) continue;
        if (sort.column == i && sort.direction == SortDirection::ascending) {
            sort.direction = SortDirection::descending;
        } else {
            sort.column = i;
            sort.direction = SortDirection::ascending;
        }
        result.sort_changed = true;
    }

    // Body: keyboard selection first, then pointer selection under the same ID.
    const float view = std::max(0.0f, body.h);
    const auto page = static_cast<std::size_t>(std::max(1.0f, std::floor(view / row_height) - 1));
    float offset = state->scroll_offset;
    bool reveal = false;
    const auto choose = [&](std::size_t row) {
        if (row >= rows) return;
        if (selected != row) result.selection_changed = true;
        selected = row;
        reveal = true;
    };
    if (enabled) {
        frame.interaction.keyboard(id, [&](const InputEvent& event) {
            if (event.kind != EventKind::key_down || rows == 0) return;
            const std::size_t current = selected.value_or(none);
            switch (event.key) {
            case Key::up: choose(current == none ? 0 : current - std::min<std::size_t>(current, 1)); break;
            case Key::down: choose(current == none ? 0 : std::min(rows - 1, current + 1)); break;
            case Key::page_up: choose(current == none ? 0 : current - std::min(current, page)); break;
            case Key::page_down: choose(current == none ? 0 : std::min(rows - 1, current + page)); break;
            case Key::home: choose(0); break;
            case Key::end: choose(rows - 1); break;
            case Key::enter: result.activated = result.activated || selected.has_value(); break;
            case Key::left: grid->scroll_x -= row_height * 2; break;
            case Key::right: grid->scroll_x += row_height * 2; break;
            default: break;
            }
        });
    }
    const auto drag = frame.interaction.drag(id, body_hit, enabled);
    for (const auto& action : drag.actions) {
        if (action.key != Key::none) continue;
        const float y = action.position_y - body.y + offset;
        if (y >= 0) choose(static_cast<std::size_t>(y / row_height));
    }
    if (frame.error) return result;

    // Scrolling: reveal a newly chosen row, then apply the wheel.
    const float max_offset = std::max(0.0f, static_cast<float>(rows) * row_height - view);
    offset = std::clamp(offset, 0.0f, max_offset);
    if (reveal && selected) {
        const float top = static_cast<float>(*selected) * row_height;
        if (top < offset) offset = top;
        if (top + row_height > offset + view) offset = top + row_height - view;
        offset = std::clamp(offset, 0.0f, max_offset);
    }
    offset = frame.settle_scroll(body_hit, offset, max_offset);
    state->scroll_offset = offset;
    if (!frame.input_blocked()) {
        for (const auto& event : frame.interaction.events())
            if (event.kind == EventKind::wheel && contains(hit, event.position))
                grid->scroll_x += event.wheel_x * row_height * 2;
    }
    grid->scroll_x = std::clamp(grid->scroll_x, 0.0f, std::max(0.0f, total_width() - box.w));

    Behavior behavior = drag.behavior;
    behavior.focused = frame.interaction.focused() == id && options.enabled;
    track_feedback(*state, hit, behavior, options.enabled);
    const Color line_ink = mix(theme.control, theme.text, 0.18f);
    const Color ink = options.enabled ? theme.text : theme.secondary;
    const std::size_t first = static_cast<std::size_t>(std::floor(offset / row_height));
    const std::size_t last = std::min(rows, static_cast<std::size_t>(std::ceil((offset + view) / row_height)));
    {
        WidgetClip clip(frame, hit);
        if (!frame.check_canvas()) return result;
        paint_surface(frame, box, theme.control, behavior.focused, theme, options.invalid);
        if (!frame.check_canvas()) return result;

        // Header: hover/press tint, title, sort triangle, column separator.
        for (std::size_t i = 0; i < columns.size() && frame.check_canvas(); ++i) {
            const Rect cell{column_left(i), box.y, grid->widths[i], header_height};
            if (cell.x >= box.x + box.w || cell.x + cell.w <= box.x) continue;
            const bool sorted = sort.direction != SortDirection::none && sort.column == i;
            if (i == header_focus) frame.canvas.border(inset(cell, 2), theme.control_radius * 0.5f, 2, theme.accent);
            const auto title = columns[i].title.empty() ? nullptr : frame.shape(columns[i].title, FontRole::body);
            if (title) {
                const float room = std::max(0.0f, cell.w - pad * 2 - (sorted ? 12.0f : 0.0f));
                WidgetClip text_clip(frame, intersect({cell.x + pad, cell.y, room, cell.h}, hit));
                if (frame.check_canvas())
                    frame.glyphs(*title, {cell.x + pad, cell.y + (cell.h - title->height) * 0.5f}, theme.secondary);
            }
            if (sorted) {
                const float x = cell.x + cell.w - pad - 8, y = cell.y + cell.h * 0.5f;
                const bool up = sort.direction == SortDirection::ascending;
                const std::array<Vec2, 3> arrow = up
                    ? std::array<Vec2, 3>{{{x, y + 3}, {x + 4, y - 3}, {x + 8, y + 3}}}
                    : std::array<Vec2, 3>{{{x, y - 3}, {x + 8, y - 3}, {x + 4, y + 3}}};
                frame.canvas.convex(arrow, theme.secondary);
            }
            const float right = cell.x + cell.w - 0.5f;
            const std::array<Vec2, 2> separator{{{right, cell.y + cell.h * 0.25f}, {right, cell.y + cell.h * 0.75f}}};
            frame.canvas.line(separator, 1, line_ink);
        }
        const std::array<Vec2, 2> rule{{{box.x, box.y + header_height}, {box.x + box.w, box.y + header_height}}};
        frame.canvas.line(rule, 1, line_ink);
        if (!frame.check_canvas()) return result;

        WidgetClip body_clip(frame, body_hit);
        if (!frame.check_canvas()) return result;
        if (rows == 0 && !options.placeholder.empty()) {
            if (const auto hint = frame.shape(options.placeholder, FontRole::body))
                frame.glyphs(*hint, {body.x + pad, body.y + (row_height - hint->height) * 0.5f}, theme.secondary);
        }
        const auto pointer = frame.interaction.pointer();
        for (std::size_t row = first; row < last && frame.check_canvas(); ++row) {
            const float y = body.y - offset + static_cast<float>(row) * row_height;
            const Rect band{box.x, y, box.w, row_height};
            const bool chosen = selected == row;
            Color row_ink = ink;
            if (chosen) {
                frame.canvas.rounded_rect(band, 0, theme.accent);
                row_ink = check_ink(theme, theme.accent);
            } else if (enabled && pointer && contains(body_hit, *pointer) && contains(band, *pointer)) {
                frame.canvas.rounded_rect(band, 0, mix(theme.control, theme.accent, 0.14f));
            }
            for (std::size_t column = 0; column < columns.size(); ++column) {
                const Rect cell{column_left(column), y, grid->widths[column], row_height};
                if (cell.x >= box.x + box.w || cell.x + cell.w <= box.x) continue;
                const auto text = cells.text(cells.context, row, column, frame.grid_scratch);
                if (text.empty()) continue;
                const auto run = frame.shape(text, FontRole::body);
                if (!run) return result;
                WidgetClip text_clip(frame, intersect({cell.x + pad, y, std::max(0.0f, cell.w - pad * 2), row_height},
                                                      body_hit));
                if (!frame.check_canvas()) return result;
                frame.glyphs(*run, {cell.x + pad, y + (row_height - run->height) * 0.5f}, row_ink);
            }
        }
        // shortcut: the scroll indicator is not draggable; wheel and keys scroll. Make it a
        // drag target if pointer-only scrolling of long grids is needed.
        if (max_offset > 0 && frame.check_canvas()) {
            const float thumb = std::max(row_height, view * view / (view + max_offset));
            const float y = body.y + (view - thumb) * (offset / max_offset);
            frame.canvas.rounded_rect({box.x + box.w - 5, y, 3, thumb}, 1.5f, mix(theme.control, theme.text, 0.35f));
        }
    }
    if (!frame.check_canvas()) return result;

    // Semantics: the grid, its visible headers and rows (row label: first cell).
    char digits[24];
    const auto end = selected ? std::to_chars(digits, digits + sizeof digits, *selected).ptr : digits;
    const std::string_view value{digits, static_cast<std::size_t>(end - digits)};
    if (!frame.copy_label(value)) return result;
    frame.canvas.semantic(id, Role::grid, label, box, options.enabled, behavior.focused, false, value,
                          options.invalid);
    for (std::size_t i = 0; i < columns.size() && frame.check_canvas(); ++i) {
        const Rect cell = intersect({column_left(i), box.y, grid->widths[i], header_height}, hit);
        if (!visible(cell)) continue;
        const bool sorted = sort.direction != SortDirection::none && sort.column == i;
        const std::string_view order = !sorted ? std::string_view{}
            : sort.direction == SortDirection::ascending ? "ascending" : "descending";
        if (!frame.copy_label(columns[i].title) || !frame.copy_label(order)) return result;
        frame.canvas.semantic(indexed_id(id, "h", i), Role::grid_header, columns[i].title, cell,
                              options.enabled && columns[i].sortable, i == header_focus, sorted, order);
    }
    for (std::size_t row = first; row < last && frame.check_canvas(); ++row) {
        const Rect band = intersect({box.x, body.y - offset + static_cast<float>(row) * row_height, box.w,
                                     row_height}, body_hit);
        if (!visible(band)) continue;
        const auto text = cells.text(cells.context, row, 0, frame.grid_scratch);
        if (!frame.copy_label(text)) return result;
        frame.canvas.semantic(indexed_id(id, "r", row), Role::grid_row, text, band, options.enabled, false,
                              selected == row);
    }
    frame.check_canvas();
    return result;
}

} // namespace wui::detail
