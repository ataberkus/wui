#include "context_detail.hpp"
#include "../style/theme_detail.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui {
namespace {
Error missing_frame() {
#ifndef NDEBUG
    std::fputs("wui contract: Frame handle has been moved\n", stderr);
    std::fflush(stderr);
    assert(false && "wui programmer contract violation");
#endif
    return {ErrorCode::invalid_frame, "Frame handle has been moved"};
}
}
Frame::Frame(std::shared_ptr<detail::FrameState> state, std::uint64_t generation) noexcept
    : state_(std::move(state)), generation_(generation) {}
Frame::Frame(Frame&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_) {}
Frame& Frame::operator=(Frame&& other) noexcept {
    if (this != &other) {
        abort();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
    }
    return *this;
}
Frame::~Frame() { abort(); }
void Frame::abort() noexcept {
    if (state_ && state_->active && state_->generation == generation_) state_->abort();
}
StyleScope Frame::style(StyleOverride override) {
    if (!state_) { (void)missing_frame(); return StyleScope({}, 0, 0, 0); }
    auto& frame = *state_;
    try {
        if (!frame.usable(generation_)) return StyleScope({}, 0, 0, 0);
        auto resolved = frame.theme;
        detail::apply_style(resolved, override);
        if (auto error = detail::validate_theme(resolved)) {
            frame.report(error->code, error->message.c_str(), true);
            return StyleScope({}, 0, 0, 0);
        }
        frame.style_stack.push_back(override);
        return StyleScope(state_, generation_, frame.style_stack.size(), frame.stack.size());
    } catch (const std::bad_alloc&) {
        frame.report(ErrorCode::capacity, "Style scope allocation failed");
    } catch (const std::length_error&) {
        frame.report(ErrorCode::capacity, "Style scope exceeds container limits");
    }
    return StyleScope({}, 0, 0, 0);
}
void Frame::text(std::string_view value) {
    if (!state_) { (void)missing_frame(); return; }
    try {
        if (state_->usable(generation_)) detail::emit_text(*state_, value, FontRole::body);
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Text emission allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Text emission exceeds container limits"); }
}
void Frame::text(std::string_view value, TextOptions options) {
    if (!state_) { (void)missing_frame(); return; }
    try {
        if (state_->usable(generation_)) detail::emit_text(*state_, value, options.role, options);
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Text emission allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Text emission exceeds container limits"); }
}
void Frame::description(std::string_view value, TextOptions options) {
    if (!state_) { (void)missing_frame(); return; }
    options.wrap = true;
    try {
        if (state_->usable(generation_))
            detail::emit_text(*state_, value, FontRole::body, options, true);
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Description allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Description exceeds container limits"); }
}
void Frame::tooltip(std::string_view owner_key, std::string_view message) {
    if (!state_) { (void)missing_frame(); return; }
    try {
        if (state_->usable(generation_))
            state_->tooltip_for_owner(state_->keyed_id(owner_key), message);
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Tooltip allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Tooltip exceeds container limits"); }
}
void Frame::validation(std::string_view message) {
    if (!state_) { (void)missing_frame(); return; }
    try {
        if (state_->usable(generation_))
            detail::emit_text(*state_, message, FontRole::body, {.wrap = true}, false, true);
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Validation allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Validation exceeds container limits"); }
}
Canvas& Frame::canvas() {
    if (!state_) {
        (void)missing_frame();
        throw std::logic_error("Frame handle has been moved");
    }
    if (!state_->usable(generation_))
        throw std::logic_error("Canvas requires an active, valid Frame");
    return state_->canvas;
}
void Frame::heading(std::string_view value) {
    if (!state_) { (void)missing_frame(); return; }
    try {
        if (state_->usable(generation_)) detail::emit_text(*state_, value, FontRole::heading);
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Heading emission allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Heading emission exceeds container limits"); }
}
bool Frame::button(std::string_view key, std::string_view label, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    try {
        return state_->usable(generation_) &&
               detail::emit_button(*state_, key, label, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Button emission allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Button emission exceeds container limits"); }
    return false;
}
bool Frame::icon_button(std::string_view key, std::string_view label,
                        TextureHandle image, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    try {
        return state_->usable(generation_) &&
               detail::emit_icon_button(*state_, key, label, image, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Icon button allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Icon button exceeds container limits"); }
    return false;
}
bool Frame::checkbox(std::string_view key, std::string_view label, bool& value, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    const bool previous = value;
    try {
        return state_->usable(generation_) &&
               detail::emit_checkbox(*state_, key, label, value, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Checkbox emission allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Checkbox emission exceeds container limits"); }
    // Painting can fail after synchronous activation. The return value still
    // reports the model change; finish() separately reports the invalid packet.
    return value != previous;
}
bool Frame::slider(std::string_view key, std::string_view label, float& value,
                   float min, float max, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    const float previous = value;
    try {
        return state_->usable(generation_) &&
               detail::emit_slider(*state_, key, label, value, min, max, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Float slider allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Float slider exceeds container limits"); }
    return std::isfinite(previous) && value != previous;
}
bool Frame::slider(std::string_view key, std::string_view label, int& value,
                   int min, int max, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    const int previous = value;
    try {
        return state_->usable(generation_) &&
               detail::emit_slider(*state_, key, label, value, min, max, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Integer slider allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Integer slider exceeds container limits"); }
    return value != previous;
}
bool Frame::text_input(std::string_view key, std::string_view label, std::string& value,
                       WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    // No per-frame copy of the model: the editor records whether it wrote it.
    state_->text_model_changed = false;
    try {
        return state_->usable(generation_) &&
               detail::emit_text_edit(*state_, key, label, value, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Text input allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Text input exceeds container limits"); }
    return state_->text_model_changed;
}
bool Frame::text_area(std::string_view key, std::string_view label, std::string& value,
                      float height, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    state_->text_model_changed = false;
    try {
        return state_->usable(generation_) &&
               detail::emit_text_area(*state_, key, label, value, height, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Text area allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Text area exceeds container limits"); }
    return state_->text_model_changed;
}
GridResult Frame::data_grid(std::string_view key, std::string_view label,
                            std::span<const GridColumn> columns, std::size_t rows, GridCells cells,
                            std::optional<std::size_t>& selected, GridSort& sort, float height,
                            WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return {}; }
    try {
        if (state_->usable(generation_))
            return detail::emit_data_grid(*state_, key, label, columns, rows, cells, selected, sort, height,
                                          state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Data grid allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Data grid exceeds container limits"); }
    return {};
}
bool Frame::numeric_input(std::string_view key, std::string_view label, double& value,
                          double min, double max, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    const double previous = value;
    try {
        return state_->usable(generation_) &&
               detail::emit_numeric_edit(*state_, key, label, value, min, max, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Numeric input allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Numeric input exceeds container limits"); }
    return value != previous;
}
bool Frame::numeric_input(std::string_view key, std::string_view label, float& value,
                          float min, float max, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    const float previous = value;
    try {
        return state_->usable(generation_) &&
               detail::emit_numeric_edit(*state_, key, label, value, min, max, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Numeric input allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Numeric input exceeds container limits"); }
    return value != previous;
}
bool Frame::numeric_input(std::string_view key, std::string_view label, int& value,
                          int min, int max, WidgetOptions options) {
    if (!state_) { (void)missing_frame(); return false; }
    const int previous = value;
    try {
        return state_->usable(generation_) &&
               detail::emit_numeric_edit(*state_, key, label, value, min, max, state_->effective(options));
    } catch (const std::bad_alloc&) { state_->report(ErrorCode::capacity, "Numeric input allocation failed"); }
      catch (const std::length_error&) { state_->report(ErrorCode::capacity, "Numeric input exceeds container limits"); }
    return value != previous;
}
void Frame::note_choice_error(std::string_view message) {
    if (!state_) { (void)missing_frame(); return; }
    try {
        if (state_->usable(generation_)) state_->note_choice_error(message);
    } catch (const std::bad_alloc&) {
        state_->report(ErrorCode::capacity, "Choice diagnostic allocation failed");
    } catch (const std::length_error&) {
        state_->report(ErrorCode::capacity, "Choice diagnostic exceeds container limits");
    }
}
Result<DrawPacket> Frame::finish() {
    if (!state_) return missing_frame();
    auto& s = *state_;
    try {
        if (!s.usable(generation_)) {
            auto error = *s.error;
            if (s.active && s.generation == generation_) s.abort();
            return error;
        }
        if (s.stack.size() != 1 || !s.rows.empty() || !s.style_stack.empty()) {
            s.report(ErrorCode::unbalanced_scope, "Frame finished with open scopes", true);
        }
        // A generic popup that was not emitted this frame has no owner left.
        if (!s.error && s.popup.generic && !s.popup.emitted) s.close_popup(false);
        // Popup and overlay layers paint above the main content; dropdown
        // lists and text tooltips paint above those.
        if (!s.error) {
            detail::CanvasAccess::flush_layers(s.canvas);
            s.check_canvas();
        }
        if (!s.error) detail::paint_popup(s);
        if (!s.error) {
            if (auto error = s.text->publish()) s.report(std::move(*error));
        }
        if (s.error) {
            auto error = std::move(*s.error);
            s.abort();
            return error;
        }
        s.interaction.end();
        detail::CanvasAccess::sync_focus(s.canvas, s.interaction.focused());
        s.composition_caret = detail::focused_text_caret(s);
        auto packet = s.canvas.finish();
        if (std::holds_alternative<Error>(packet)) {
            s.abort();
            return packet;
        }
        ++s.completed;
        for (auto& [id, widget] : s.widgets) {
            if (widget.seen == s.generation) widget.last_present = s.completed;
        }
        s.prune();
        s.active = false;
        s.stack.clear();
        s.style_stack.clear();
        return packet;
    } catch (const std::bad_alloc&) {
        s.abort();
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        s.abort();
        return Error{ErrorCode::capacity, {}};
    }
}
PanelScope::PanelScope(std::shared_ptr<detail::FrameState> state,
                       std::uint64_t generation, std::size_t depth) noexcept
    : state_(std::move(state)), generation_(generation), depth_(depth) {}
PanelScope::PanelScope(PanelScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_), depth_(other.depth_) {}
PanelScope& PanelScope::operator=(PanelScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        depth_ = other.depth_;
    }
    return *this;
}
PanelScope::~PanelScope() { close(); }
void PanelScope::close() noexcept {
    if (state_) {
        state_->close_panel(generation_, depth_);
        state_.reset();
    }
}
RowScope::RowScope(std::shared_ptr<detail::FrameState> state,
                   std::uint64_t generation, std::size_t row_index) noexcept
    : state_(std::move(state)), generation_(generation), row_index_(row_index) {}
RowScope::RowScope(RowScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_),
      row_index_(other.row_index_) {}
RowScope& RowScope::operator=(RowScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        row_index_ = other.row_index_;
    }
    return *this;
}
RowScope::~RowScope() { close(); }
void RowScope::close() noexcept {
    if (state_) {
        state_->close_row(generation_, row_index_);
        state_.reset();
    }
}
CellScope::CellScope(std::shared_ptr<detail::FrameState> state,
                     std::uint64_t generation, std::size_t row_index,
                     std::size_t depth) noexcept
    : state_(std::move(state)), generation_(generation),
      row_index_(row_index), depth_(depth) {}
CellScope::CellScope(CellScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_),
      row_index_(other.row_index_), depth_(other.depth_) {}
CellScope& CellScope::operator=(CellScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        row_index_ = other.row_index_;
        depth_ = other.depth_;
    }
    return *this;
}
CellScope::~CellScope() { close(); }
void CellScope::close() noexcept {
    if (state_) {
        state_->close_cell(generation_, row_index_, depth_);
        state_.reset();
    }
}
ScrollScope::ScrollScope(std::shared_ptr<detail::FrameState> state,
                         std::uint64_t generation, std::size_t depth,
                         Id id, float offset, Rect viewport) noexcept
    : state_(std::move(state)), generation_(generation), depth_(depth),
      id_(id), offset_(offset), viewport_(viewport) {}
ScrollScope::ScrollScope(ScrollScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_),
      depth_(other.depth_), id_(other.id_), offset_(other.offset_),
      viewport_(other.viewport_) {}
ScrollScope& ScrollScope::operator=(ScrollScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        depth_ = other.depth_;
        id_ = other.id_;
        offset_ = other.offset_;
        viewport_ = other.viewport_;
    }
    return *this;
}
ScrollScope::~ScrollScope() { close(); }
void ScrollScope::close() noexcept {
    if (state_) {
        state_->close_scroll(generation_, depth_, id_, viewport_, offset_);
        state_.reset();
    }
}
void detail::FrameState::close_style(std::uint64_t expected, std::size_t depth,
                                     std::size_t container_depth) noexcept {
    if (!active || generation != expected || !owner_alive || error) return;
    if (style_stack.size() != depth || stack.size() != container_depth) {
        report(ErrorCode::unbalanced_scope, "Style scopes must close in reverse opening order", true);
        return;
    }
    style_stack.pop_back();
}
StyleScope::StyleScope(std::shared_ptr<detail::FrameState> state,
                       std::uint64_t generation, std::size_t depth,
                       std::size_t container_depth) noexcept
    : state_(std::move(state)), generation_(generation),
      depth_(depth), container_depth_(container_depth) {}
StyleScope::StyleScope(StyleScope&& other) noexcept
    : state_(std::move(other.state_)), generation_(other.generation_),
      depth_(other.depth_), container_depth_(other.container_depth_) {}
StyleScope& StyleScope::operator=(StyleScope&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
        generation_ = other.generation_;
        depth_ = other.depth_;
        container_depth_ = other.container_depth_;
    }
    return *this;
}
StyleScope::~StyleScope() { close(); }
void StyleScope::close() noexcept {
    if (state_) {
        state_->close_style(generation_, depth_, container_depth_);
        state_.reset();
    }
}
} // namespace wui
