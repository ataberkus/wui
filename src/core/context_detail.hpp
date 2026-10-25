#pragma once

#include <wui/context.hpp>
#include <wui/layout.hpp>
#include "../paint/canvas_access.hpp"
#include <charconv>
#include <cmath>
#include <memory_resource>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wui::detail {
class RetainedMemory final : public std::pmr::memory_resource {
public:
    explicit RetainedMemory(std::size_t limit) : limit_(limit) {}
    std::size_t used() const noexcept { return used_; }
    std::size_t peak() const noexcept { return peak_; }
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void*, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    std::size_t limit_, used_ = 0, peak_ = 0;
};
struct WidgetState {
    std::uint64_t seen = 0;
    std::uint64_t last_present = 0;
    std::uint64_t tooltip_seen = 0;
    Transition hover{}, selection{};
    bool initialized = false;
    bool hovered = false, enabled = false;
    Rect bounds{};
    float scroll_offset = 0;
    double tooltip_seconds = 0;
    float measured_height = 0; // last closed popup content height
    float hsv[3]{};            // color picker: hue/saturation/value kept across
    bool hsv_valid = false;    // achromatic edits, refreshed on external changes
    bool dragging = false;     // text_area: pointer captured on the previous frame
};
struct Container {
    Id id;
    Rect bounds, clip;
    Flow flow;
    PanelMark mark;
    float inset = 0;
    std::uint64_t next_text = 0;
};
struct RowState {
    Id id;
    std::size_t first, count, next, parent_depth;
    float tallest = 0;
    bool cell_open = false;
};
struct PopupState {
    Id owner{}, previous_focus{};
    // Generic popups (Frame::popup) share the single root popup slot with
    // dropdown lists; emitted marks liveness for the current frame.
    bool generic = false, emitted = false;
};
enum class AreaKind { region, overlay, popup };
struct AreaRecord {
    AreaKind kind = AreaKind::region;
    Id id{};
    CanvasLayer layer{};
    std::size_t container_depth = 0, style_depth = 0;
    Rect bounds{};
    bool modal = false, live = false, previous_layer_open = false;
};
struct EditSnapshot {
    std::pmr::string text;
    std::size_t caret = 0, anchor = 0;
};
// One visual line of a multi-line editor: byte range in the draft, excluding
// the hard line break that may follow it.
struct EditLine {
    std::size_t begin = 0, end = 0;
};
struct EditState {
    std::pmr::string draft, composition, last_published;
    std::pmr::vector<EditSnapshot> undo;
    std::pmr::vector<EditLine> lines; // text_area wrap layout of draft
    std::size_t undo_bytes = 0, caret = 0, anchor = 0;
    std::uint64_t last_present = 0;
    bool composition_active = false;
    float layout_width = -1;  // width `lines` were wrapped at; < 0 means stale
    float preferred_x = -1;   // caret column kept across vertical moves
    std::optional<Vec2> caret_point; // painted caret bottom, for IME placement
};
// data_grid: user-sized column widths and in-progress gestures.
struct GridState {
    std::pmr::vector<float> widths;
    float scroll_x = 0;
    float grab = 0;                        // pointer x minus width at resize press
    std::size_t resizing = static_cast<std::size_t>(-1);
};
enum class OverlayKind { popup_option, tooltip };
struct OverlayPaint {
    OverlayKind kind = OverlayKind::popup_option;
    std::string label;
    std::shared_ptr<const TextRun> run;
    Rect bounds{};
    Id id{};
    Behavior behavior{};
    Theme theme{};
    bool selected = false, enabled = true, invalid = false;
};
struct FrameState {
    explicit FrameState(ContextDesc);
    Theme theme;
    std::shared_ptr<IText> text;
    std::shared_ptr<IClipboard> clipboard;
    Canvas canvas;
    std::optional<Vec2> composition_caret;
    // Set once the current text_input call has written its model, so a later
    // paint failure still reports the change without copying the value.
    bool text_model_changed = false;
    Interaction interaction;
    const std::size_t max_widgets, max_retained_bytes;
    RetainedMemory memory;
    std::pmr::unordered_map<std::uint64_t, WidgetState> widgets;
    std::pmr::unordered_map<std::uint64_t, EditState> edits;
    std::pmr::unordered_map<std::uint64_t, GridState> grids;
    std::string grid_scratch; // data_grid cell text built by the provider
    std::pmr::vector<Container> stack;
    std::pmr::vector<Rect> row_rects;
    std::pmr::vector<RowState> rows;
    std::pmr::vector<std::shared_ptr<const TextRun>> wrap_lines;
    std::pmr::vector<StyleOverride> style_stack;
    std::pmr::vector<AreaRecord> areas;
    std::pmr::vector<Column> choice_columns;
    std::pmr::vector<Error> diagnostics;
    PopupState popup;
    std::pmr::vector<OverlayPaint> overlays;
    std::size_t overlay_count = 0;
    Rect popup_bounds{};
    Rect popup_anchor{};
    float popup_offset = 0, popup_max_offset = 0, popup_focus_offset = 0;
    Theme popup_theme{};
    Status error;
    Rect viewport{};
    float scale = 1, dt = 0;
    double elapsed_seconds = 0;
    std::uint64_t generation = 0;
    std::size_t count = 0, label_bytes = 0;
    std::uint64_t completed = 0;
    bool active = false, owner_alive = true;

    // report() preserves first error; contract=true asserts in Debug after
    // recording it. Subsequent calls become inert until finalization/abort.
    void report(ErrorCode, const char* message, bool contract = false) noexcept;
    void report(Error) noexcept;
    bool usable(std::uint64_t);
    bool check_canvas();
    void abort() noexcept;
    void prune() noexcept;
    WidgetState* register_widget(Id);
    bool valid_input(const FrameInput&);
    Id keyed_id(std::string_view);
    Id text_id();
    bool copy_label(std::string_view);
    void note_choice_error(std::string_view);
    Rect take(float height);
    Rect clip() const;
    // size <= 0 selects the theme size for the role (display uses heading size).
    std::shared_ptr<const TextRun> shape(std::string_view, FontRole, float size = 0);
    // Folds enclosing disabled style scopes into the caller's options.
    WidgetOptions effective(WidgetOptions options) const {
        for (const auto& scope : style_stack)
            if (scope.disabled) options.enabled = false;
        return options;
    }
    void close_area(std::uint64_t, std::size_t depth) noexcept;
    void glyphs(const TextRun&, Vec2 top_left, Color);
    void close_panel(std::uint64_t, std::size_t) noexcept;
    void close_cell(std::uint64_t, std::size_t row_index, std::size_t depth) noexcept;
    void close_row(std::uint64_t, std::size_t row_index) noexcept;
    float settle_scroll(Rect visible_viewport, float current_offset, float max_offset);
    void close_scroll(std::uint64_t, std::size_t depth, Id id,
                      Rect viewport, float current_offset) noexcept;
    void close_style(std::uint64_t, std::size_t depth,
                     std::size_t container_depth) noexcept;
    Behavior route_popup_button(Id option, Rect bounds, bool enabled);
    float route_popup_wheel(Rect list, float current_offset, float max_offset);
    void focus_popup_option(Id option);
    void close_popup(bool restore_focus);
    void record_popup_option(Id id, Rect bounds, std::string_view label,
                             std::shared_ptr<const TextRun> run, Behavior behavior,
                             bool selected, bool enabled, bool invalid);
    void queue_noninteractive_tooltip(Rect owner, std::string_view message);
    // True once the owner has been hovered for the tooltip delay; a nonempty
    // message is then queued as the standard text tooltip.
    bool tooltip_for_owner(Id owner, std::string_view message);
    EditState* edit_state(Id id);
    bool account_edit_bytes(std::size_t bytes);
    void prune_edits() noexcept;
    GridState* grid_state(Id id);
    bool input_blocked() const { return interaction.blocked(); }
    void note_diagnostic(ErrorCode code, std::string_view message);
    bool edit_text_model(Id id, std::string& value, bool focused, bool enabled);
    std::string_view edit_display(Id id);
    void paint_text_input(Id id, Rect bounds, const Theme& theme, Behavior behavior,
                          WidgetState& widget, bool enabled, bool invalid,
                          std::string_view label, std::string_view placeholder = {});
    std::optional<Vec2> edit_caret(Id id) const;
    bool popup_live_at_frame_start(Id id) const {
        return interaction.popup_owner_at_frame_start_ == id;
    }
    // Marks widgets as inside the open popup's layer; returns the previous mark.
    bool enter_layer() {
        return std::exchange(interaction.layer_open_, true);
    }
    Vec2 take_wheel_steps(Rect area) { return interaction.take_wheel_steps(area); }
};
template<class T>
bool parse_numeric(std::string_view draft, T min, T max, T& parsed) {
    T candidate{};
    const char* end = draft.data() + draft.size();
    const auto [next, ec] = std::from_chars(draft.data(), end, candidate);
    if (ec != std::errc{} || next != end || !std::isfinite(static_cast<double>(candidate)) ||
        candidate < min || candidate > max)
        return false;
    parsed = candidate;
    return true;
}
inline std::optional<Vec2> focused_text_caret(const FrameState& frame) {
    const Id focused = frame.interaction.focused();
    if (focused == Id{}) return std::nullopt;
    const auto widget = frame.widgets.find(focused.value);
    const auto editor = frame.edits.find(focused.value);
    if (widget == frame.widgets.end() || editor == frame.edits.end()) return std::nullopt;
    if (!widget->second.enabled) return std::nullopt;
    if (editor->second.caret_point) return editor->second.caret_point;
    const Rect bounds = widget->second.bounds;
    return Vec2{bounds.x + bounds.w * 0.5f, bounds.y + bounds.h};
}
// Widget source owns these operations. Caller first verifies usable(); all
// operations report failures on FrameState. No model pointer escapes the call.
void emit_text(FrameState&, std::string_view, FontRole, TextOptions = {},
               bool description = false, bool validation = false);
bool emit_button(FrameState&, std::string_view key, std::string_view label, WidgetOptions);
bool emit_icon_button(FrameState&, std::string_view key, std::string_view label,
                      TextureHandle image, WidgetOptions);
bool emit_checkbox(FrameState&, std::string_view key, std::string_view label, bool&, WidgetOptions);
bool emit_slider(FrameState&, std::string_view key, std::string_view label,
                 float&, float min, float max, WidgetOptions);
bool emit_slider(FrameState&, std::string_view key, std::string_view label,
                 int&, int min, int max, WidgetOptions);
bool emit_text_edit(FrameState&, std::string_view key, std::string_view label,
                    std::string&, WidgetOptions);
bool emit_text_area(FrameState&, std::string_view key, std::string_view label,
                    std::string&, float height, WidgetOptions);
GridResult emit_data_grid(FrameState&, std::string_view key, std::string_view label,
                          std::span<const GridColumn>, std::size_t rows, GridCells,
                          std::optional<std::size_t>& selected, GridSort&, float height,
                          WidgetOptions);
// Shared editing primitives: record an undo step (false when the retained
// byte limit refuses it) and replace the selection with `insert`.
bool snapshot_edit_undo(FrameState&, EditState&);
void replace_edit_selection(EditState&, std::string_view insert);
// Validated grapheme boundaries of `text` (0 ... size); empty after an error.
std::vector<std::size_t> edit_boundaries(FrameState&, std::string_view text);
// Applies one focused editing event to the draft; true when the draft changed.
bool process_edit_event(FrameState&, EditState&, const InputEvent&);
template<class T>
bool emit_numeric_edit(FrameState&, std::string_view key, std::string_view label,
                       T&, T min, T max, WidgetOptions);
void paint_popup(FrameState&);
Behavior begin_dropdown(FrameState&, WidgetState&, Id group, Rect trigger,
                        std::string_view group_label, std::string_view selected_label,
                        std::size_t count,
                        const Theme&, bool enabled, bool invalid);
} // namespace wui::detail
