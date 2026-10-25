#pragma once

#include <wui/draw.hpp>
#include <wui/input.hpp>
#include <wui/layout.hpp>
#include <wui/style.hpp>
#include <wui/text.hpp>
#include <concepts>
#include <functional>
#include <cstdint>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <type_traits>

namespace wui {
namespace detail { struct FrameState; }
class Context;
class Canvas;
// role/size select the face and logical pixel size (size 0: theme size for
// the role; display defaults to the heading size). align places each line
// within the row width; a line wider than the row starts at its left edge.
struct TextOptions {
    std::optional<Color> color;
    bool wrap = false;
    FontRole role = FontRole::body;
    float size = 0;
    Align align = Align::start;
};
struct CustomItem {
    Id id;
    Rect bounds, clip;
    Behavior behavior;
    bool enabled = true; // after enclosing disabled scopes are applied
};
struct PanelOptions { std::string_view title; };
template<class T> struct Choice { T value; std::string_view label; };
enum class Orientation { horizontal, vertical };
struct WidgetOptions {
    bool enabled = true;
    Variant variant = Variant::normal;
    StyleOverride style;
    bool invalid = false;
    // text_input / numeric_input / text_area: hint painted while the draft is
    // empty; data_grid: hint painted while it has no rows.
    std::string_view placeholder;
    // button: label placement inside the padded control (other widgets ignore it).
    Align label_align = Align::center;
};
enum class SortDirection { none, ascending, descending };
struct GridColumn {
    std::string_view title;
    float width = 120;    // initial width; user resizing is retained per grid
    float min_width = 32;
    bool sortable = true;
};
// The caller sorts its rows; data_grid only reports header clicks here.
struct GridSort {
    std::size_t column = 0;
    SortDirection direction = SortDirection::none;
};
struct GridResult {
    bool selection_changed = false, sort_changed = false, activated = false;
};
// Cell text for one visible cell. A provider that builds a new string stores
// it in `scratch` and returns a view of it; the view is used before the next call.
struct GridCells {
    void* context = nullptr;
    std::string_view (*text)(void* context, std::size_t row, std::size_t column,
                             std::string& scratch) = nullptr;
};
// Placement and behaviour of Frame::popup. Non-modal popups open below the
// anchor (above when that fits better), are clamped into the viewport, and
// close on an outside press, Escape or focus loss. Modal popups are centered,
// dim the frame, swallow outside input and close only through close_popup().
struct PopupOptions {
    Rect anchor{};
    float width = 0; // 0: anchor width, at least six control heights
    bool modal = false;
};
struct TextStyle {
    FontRole role = FontRole::body;
    float size = 0; // 0: theme size for the role
    std::optional<Color> color; // default: theme text
};
struct ColorPickerOptions {
    bool alpha = true;
    float size = 0; // saturation/value square side; 0 derives it from the width
};

// A moved scope transfers closing responsibility. Scopes must close in LIFO
// order before finish(); an unfinished Frame aborts on destruction.
class PanelScope {
public:
    PanelScope(PanelScope&&) noexcept;
    PanelScope& operator=(PanelScope&&) noexcept;
    ~PanelScope();
    PanelScope(const PanelScope&) = delete;
    PanelScope& operator=(const PanelScope&) = delete;
private:
    friend class Frame;
    PanelScope(std::shared_ptr<detail::FrameState>, std::uint64_t, std::size_t) noexcept;
    void close() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
    std::size_t depth_ = 0;
};
class CellScope {
public:
    CellScope(CellScope&&) noexcept;
    CellScope& operator=(CellScope&&) noexcept;
    ~CellScope();
    CellScope(const CellScope&) = delete;
    CellScope& operator=(const CellScope&) = delete;
private:
    friend class RowScope;
    CellScope(std::shared_ptr<detail::FrameState>, std::uint64_t,
              std::size_t row_index, std::size_t depth) noexcept;
    void close() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
    std::size_t row_index_ = 0, depth_ = 0;
};

class RowScope {
public:
    RowScope(RowScope&&) noexcept;
    RowScope& operator=(RowScope&&) noexcept;
    ~RowScope();
    RowScope(const RowScope&) = delete;
    RowScope& operator=(const RowScope&) = delete;
    CellScope cell();
private:
    friend class Frame;
    RowScope(std::shared_ptr<detail::FrameState>, std::uint64_t,
             std::size_t row_index) noexcept;
    void close() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
    std::size_t row_index_ = 0;
};


class ScrollScope {
public:
    ScrollScope(ScrollScope&&) noexcept;
    ScrollScope& operator=(ScrollScope&&) noexcept;
    ~ScrollScope();
    ScrollScope(const ScrollScope&) = delete;
    ScrollScope& operator=(const ScrollScope&) = delete;
    float offset() const noexcept { return offset_; }
    Rect viewport() const noexcept { return viewport_; }
private:
    friend class Frame;
    ScrollScope(std::shared_ptr<detail::FrameState>, std::uint64_t,
                std::size_t depth, Id id, float offset, Rect viewport) noexcept;
    void close() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
    std::size_t depth_ = 0;
    Id id_{};
    float offset_ = 0;
    Rect viewport_{};
};

class StyleScope {
public:
    StyleScope(StyleScope&&) noexcept;
    StyleScope& operator=(StyleScope&&) noexcept;
    ~StyleScope();
    StyleScope(const StyleScope&) = delete;
    StyleScope& operator=(const StyleScope&) = delete;
private:
    friend class Frame;
    StyleScope(std::shared_ptr<detail::FrameState>, std::uint64_t,
               std::size_t depth, std::size_t container_depth) noexcept;
    void close() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
    std::size_t depth_ = 0, container_depth_ = 0;
};

// Region, overlay and popup scopes. An inert scope (closed popup or failed
// frame) converts to false and closes nothing.
class AreaScope {
public:
    AreaScope(AreaScope&&) noexcept;
    AreaScope& operator=(AreaScope&&) noexcept;
    ~AreaScope();
    AreaScope(const AreaScope&) = delete;
    AreaScope& operator=(const AreaScope&) = delete;
    explicit operator bool() const noexcept { return state_ != nullptr; }
    Rect bounds() const noexcept { return bounds_; }
private:
    friend class Frame;
    AreaScope() noexcept = default;
    AreaScope(std::shared_ptr<detail::FrameState>, std::uint64_t,
              std::size_t depth, Rect bounds) noexcept;
    void close() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
    std::size_t depth_ = 0;
    Rect bounds_{};
};

class Frame {
public:
    Frame(Frame&&) noexcept;
    Frame& operator=(Frame&&) noexcept;
    ~Frame();
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    PanelScope panel(std::string_view key, PanelOptions options = {});
    StyleScope style(StyleOverride override);
    RowScope row(std::string_view key, std::span<const Column> columns);
    float available_width() const;
    ScrollScope scroll(std::string_view key, float viewport_height);
    void spacer(float height);
    void text(std::string_view value);
    void text(std::string_view value, TextOptions options);
    void description(std::string_view value, TextOptions options = {});
    void tooltip(std::string_view owner_key, std::string_view message);
    void validation(std::string_view message);
    void heading(std::string_view value);
    bool button(std::string_view key, std::string_view label, WidgetOptions options = {});
    bool icon_button(std::string_view key, std::string_view accessible_label,
                     TextureHandle image, WidgetOptions options = {});
    bool checkbox(std::string_view key, std::string_view label, bool& value, WidgetOptions options = {});
    bool slider(std::string_view key, std::string_view label, float& value,
                float min, float max, WidgetOptions options = {});
    bool slider(std::string_view key, std::string_view label, int& value,
                int min, int max, WidgetOptions options = {});
    bool text_input(std::string_view key, std::string_view label, std::string& value,
                    WidgetOptions options = {});
    // Multi-line editor with word wrap and internal vertical scrolling. Enter
    // inserts '\n'; Up/Down/Page Up/Page Down move by visual line, Home/End to
    // the visual line edges (Ctrl: whole text); the pointer places the caret
    // and drag selects. Pasted CRLF/CR become '\n'. `height` is the box height
    // in logical pixels; 0 fits four body lines.
    bool text_area(std::string_view key, std::string_view label, std::string& value,
                   float height = 0, WidgetOptions options = {});
    // Table with a header row and `rows` virtualized rows in a box `height`
    // tall. Header clicks (or Enter/Space on a focused header) toggle `sort`;
    // dragging a header's right edge resizes that column. Click and
    // Up/Down/Home/End/Page Up/Page Down select a row, Enter activates it,
    // Left/Right and horizontal wheel scroll columns. `cell(row, column)` is
    // called only for visible cells and returns text (string, string_view or
    // const char*).
    GridResult data_grid(std::string_view key, std::string_view label,
                         std::span<const GridColumn> columns, std::size_t rows, GridCells cells,
                         std::optional<std::size_t>& selected, GridSort& sort, float height,
                         WidgetOptions options = {});
    template<class Cell>
    GridResult data_grid(std::string_view key, std::string_view label,
                         std::span<const GridColumn> columns, std::size_t rows, Cell&& cell,
                         std::optional<std::size_t>& selected, GridSort& sort, float height,
                         WidgetOptions options = {});
    bool numeric_input(std::string_view key, std::string_view label, double& value,
                       double min, double max, WidgetOptions options = {});
    bool numeric_input(std::string_view key, std::string_view label, float& value,
                       float min, float max, WidgetOptions options = {});
    bool numeric_input(std::string_view key, std::string_view label, int& value,
                       int min, int max, WidgetOptions options = {});
    template<class T>
    bool tabs(std::string_view key, T& selected,
              std::span<const Choice<std::type_identity_t<T>>> options,
              Orientation orientation = Orientation::horizontal,
              WidgetOptions widget_options = {});
    template<class T>
    bool radio(std::string_view key, std::string_view label, T& selected,
               std::span<const Choice<std::type_identity_t<T>>> options,
               WidgetOptions widget_options = {});
    template<class T>
    bool dropdown(std::string_view key, std::string_view label, T& selected,
                  std::span<const Choice<std::type_identity_t<T>>> options,
                  WidgetOptions widget_options = {});
    Result<CustomItem> custom(std::string_view key, std::string_view label,
                              float height, WidgetOptions options = {});
    // Explicitly placed custom item; does not advance the container flow.
    Result<CustomItem> custom_at(std::string_view key, std::string_view label,
                                 Rect bounds, WidgetOptions options = {});
    // Saturation/value square plus hue (and optional alpha) bars, all
    // pointer-draggable; left/right/home/end adjust the focused bar.
    bool color_picker(std::string_view key, std::string_view label, Color& value,
                      ColorPickerOptions options = {});
    // Child container at an explicit logical rectangle: children flow inside
    // it, are clipped to it and the parent's clip; the parent flow does not
    // advance (a positioned child window / SetCursorPos equivalent).
    AreaScope region(std::string_view key, Rect bounds);
    // Paint layer above all main content and any popup, clipped only to the
    // root viewport and bounds (custom tooltips, drag previews). It does not
    // block input beneath it.
    AreaScope overlay(std::string_view key, Rect bounds);
    // The single root popup slot (shared with dropdown lists): open_popup
    // replaces whatever is open; popup() returns a live scope while `key` is
    // open and must be called every frame it should stay open. Keys resolve in
    // the current ID scope, like widget keys. Widgets under an open popup are
    // inert; widgets inside it route normally.
    void open_popup(std::string_view key);
    AreaScope popup(std::string_view key, PopupOptions options = {});
    bool popup_open(std::string_view key);
    void close_popup();
    // True once the earlier widget `owner_key` has been hovered for the
    // tooltip delay; pairs with overlay() for custom tooltip content.
    bool tooltip_due(std::string_view owner_key);
    // Rectangle the next flow item of `height` would receive (nothing taken).
    Rect next_rect(float height = 0);
    Rect viewport() const;
    // Positioned text in logical coordinates, clipped to the current scope.
    Vec2 measure_text(std::string_view text, TextStyle style = {});
    void draw_text(Vec2 top_left, std::string_view text, TextStyle style = {});
    // Raw input routed this frame, and the latest pointer position.
    std::span<const InputEvent> events() const;
    std::optional<Vec2> pointer() const;
    // Consumes unconsumed wheel steps over `area` (x: horizontal, y:
    // vertical notches) so enclosing scroll scopes ignore them.
    Vec2 take_wheel(Rect area);
    Canvas& canvas();
    Result<DrawPacket> finish();
private:
    friend class Context;
    enum class ChoiceKind { tabs, radio, dropdown };
    class ChoiceScope {
    public:
        ChoiceScope(ChoiceScope&&) noexcept;
        ChoiceScope& operator=(ChoiceScope&&) noexcept;
        ~ChoiceScope();
        ChoiceScope(const ChoiceScope&) = delete;
        ChoiceScope& operator=(const ChoiceScope&) = delete;
        bool option(std::size_t index, std::string_view label, std::size_t value_hash);
    private:
        friend class Frame;
        ChoiceScope() noexcept = default;
        ChoiceScope(std::shared_ptr<detail::FrameState>, std::uint64_t, Id group,
                    Rect bounds, std::size_t count, std::size_t selected,
                    std::size_t first, ChoiceKind, Orientation, Theme,
                    bool enabled, bool invalid, std::string_view group_label,
                    std::string_view selected_label, Behavior trigger) noexcept;
        void close() noexcept;
        std::shared_ptr<detail::FrameState> state_;
        std::uint64_t generation_ = 0;
        Id group_{};
        Id selected_id_{};
        Rect bounds_{};
        std::size_t count_ = 0, selected_ = 0, first_ = 0, next_ = 0;
        ChoiceKind kind_ = ChoiceKind::tabs;
        Orientation orientation_ = Orientation::horizontal;
        Theme theme_{};
        bool enabled_ = true, invalid_ = false;
        std::string_view group_label_{}, selected_label_{};
        Behavior trigger_{};
    };
    ChoiceScope begin_choices(std::string_view key, std::string_view label,
                              std::size_t count, std::size_t selected_index,
                              ChoiceKind kind, Orientation orientation,
                              WidgetOptions options, std::string_view selected_label);
    void note_choice_error(std::string_view message);
    template<class T>
    bool choose(std::string_view key, std::string_view label, T& selected,
                std::span<const Choice<T>> options, ChoiceKind kind,
                Orientation orientation, WidgetOptions widget_options);
    Frame(std::shared_ptr<detail::FrameState>, std::uint64_t) noexcept;
    void abort() noexcept;
    std::shared_ptr<detail::FrameState> state_;
    std::uint64_t generation_ = 0;
};
template<class T>
bool Frame::tabs(std::string_view key, T& selected,
                 std::span<const Choice<std::type_identity_t<T>>> options,
                 Orientation orientation, WidgetOptions widget_options) {
    return choose(key, {}, selected, options, ChoiceKind::tabs, orientation, widget_options);
}

template<class T>
bool Frame::radio(std::string_view key, std::string_view label, T& selected,
                  std::span<const Choice<std::type_identity_t<T>>> options,
                  WidgetOptions widget_options) {
    return choose(key, label, selected, options, ChoiceKind::radio,
                  Orientation::vertical, widget_options);
}

template<class T>
bool Frame::dropdown(std::string_view key, std::string_view label, T& selected,
                     std::span<const Choice<std::type_identity_t<T>>> options,
                     WidgetOptions widget_options) {
    return choose(key, label, selected, options, ChoiceKind::dropdown,
                  Orientation::vertical, widget_options);
}

template<class Cell>
GridResult Frame::data_grid(std::string_view key, std::string_view label,
                            std::span<const GridColumn> columns, std::size_t rows, Cell&& cell,
                            std::optional<std::size_t>& selected, GridSort& sort, float height,
                            WidgetOptions options) {
    using Provider = std::remove_reference_t<Cell>;
    const GridCells cells{
        const_cast<void*>(static_cast<const void*>(std::addressof(cell))),
        [](void* context, std::size_t row, std::size_t column, std::string& scratch) -> std::string_view {
            auto& provider = *static_cast<Provider*>(context);
            using R = std::invoke_result_t<Provider&, std::size_t, std::size_t>;
            if constexpr (std::is_reference_v<R> || std::is_convertible_v<R, const char*> ||
                          std::is_same_v<std::remove_cv_t<R>, std::string_view>) {
                return std::string_view(provider(row, column));
            } else {
                scratch = provider(row, column);
                return scratch;
            }
        }};
    return data_grid(key, label, columns, rows, cells, selected, sort, height, options);
}

template<class T>
bool Frame::choose(std::string_view key, std::string_view label, T& selected,
                   std::span<const Choice<T>> options, ChoiceKind kind,
                   Orientation orientation, WidgetOptions widget_options) {
    static_assert(std::is_copy_assignable_v<T> && std::equality_comparable<T> &&
                  requires(const T& value) {
                      { std::hash<T>{}(value) } -> std::convertible_to<std::size_t>;
                  }, "Choice values must be copy-assignable, equality-comparable, and hashable");
    if (options.empty()) {
        note_choice_error("Choice group has no options");
        return false;
    }
    constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    std::size_t selected_index = none;
    // ponytail: O(n^2) avoids per-frame hash storage; use retained indexing if large groups become common.
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (options[i].value == selected) selected_index = i;
        const auto hash = std::hash<T>{}(options[i].value);
        for (std::size_t j = 0; j < i; ++j) {
            if (options[i].value == options[j].value ||
                hash == std::hash<T>{}(options[j].value)) {
                note_choice_error("Choice values or their IDs are not unique");
                return false;
            }
        }
    }
    if (selected_index == none) note_choice_error("Selected value is missing from choices");
    auto scope = begin_choices(key, label, options.size(), selected_index, kind,
                               orientation, widget_options,
                               selected_index == none ? std::string_view{} : options[selected_index].label);
    bool changed = false;
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (scope.option(i, options[i].label, std::hash<T>{}(options[i].value))) {
            selected = options[i].value;
            changed = true;
        }
    }
    return changed;
}
} // namespace wui
