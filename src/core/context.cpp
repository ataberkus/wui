#include "context_detail.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace wui::detail {
void* RetainedMemory::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes > limit_ - used_) throw std::bad_alloc{};
    void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
    used_ += bytes;
    peak_ = std::max(peak_, used_);
    return result;
}
void RetainedMemory::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    used_ -= bytes;
}
FrameState::FrameState(ContextDesc desc)
    : theme(desc.theme), text(std::move(desc.text)), clipboard(std::move(desc.clipboard)),
      canvas(std::move(desc.textures), desc.max_packet_slots),
      max_widgets(desc.max_widgets), max_retained_bytes(desc.max_retained_bytes),
      memory(max_retained_bytes), widgets(&memory), stack(&memory),
      row_rects(&memory), rows(&memory), wrap_lines(&memory), style_stack(&memory), areas(&memory),
      choice_columns(&memory), diagnostics(&memory), overlays(&memory), edits(&memory),
      grids(&memory) {}

void FrameState::report(ErrorCode code, const char* message, bool contract) noexcept {
    if (error) return;
    error.emplace(Error{code, {}});
    try { error->message = message; } catch (...) {}
#ifndef NDEBUG
    if (contract) {
        std::fprintf(stderr, "wui contract: %s\n", message);
        std::fflush(stderr);
        assert(false && "wui programmer contract violation");
    }
#else
    (void)contract;
#endif
}
void FrameState::report(Error value) noexcept {
    if (!error) error = std::move(value);
}
bool FrameState::usable(std::uint64_t expected) {
    if (!owner_alive || !active || generation != expected) {
        report(ErrorCode::invalid_frame, "Frame no longer belongs to an active Context", true);
        return false;
    }
    return !error && check_canvas();
}
bool FrameState::check_canvas() {
    if (error) return false;
    if (auto failed = CanvasAccess::status(canvas)) report(std::move(*failed));
    return !error;
}
void FrameState::abort() noexcept {
    CanvasAccess::abort(canvas);
    // An aborted emission order must not become next frame's focus order or
    // leave half-consumed gestures able to activate a later caller model.
    interaction = Interaction{};
    popup = {};
    overlay_count = 0;
    stack.clear();
    style_stack.clear();
    areas.clear();
    active = false;
}
void FrameState::prune() noexcept {
    for (auto it = widgets.begin(); it != widgets.end();) {
        if (completed >= it->second.last_present &&
            completed - it->second.last_present >= 120) it = widgets.erase(it);
        else ++it;
    }
    prune_edits();
}
void FrameState::prune_edits() noexcept {
    for (auto it = edits.begin(); it != edits.end();) {
        const auto owner = widgets.find(it->first);
        if (owner == widgets.end() || completed < owner->second.last_present ||
            completed - owner->second.last_present < 120) {
            ++it;
            continue;
        }
        it = edits.erase(it);
    }
    // Grid state of a widget pruned above goes with it.
    for (auto it = grids.begin(); it != grids.end();) {
        if (widgets.contains(it->first)) ++it;
        else it = grids.erase(it);
    }
}
GridState* FrameState::grid_state(Id id) {
    if (error) return nullptr;
    auto found = grids.find(id.value);
    if (found == grids.end())
        found = grids.try_emplace(id.value, GridState{std::pmr::vector<float>{&memory}}).first;
    return &found->second;
}
WidgetState* FrameState::register_widget(Id id) {
    if (error) return nullptr;
    if (id == Id{}) {
        report(ErrorCode::invalid_argument, "Widget ID must be nonzero", true);
        return nullptr;
    }
    auto found = widgets.find(id.value);
    if (found != widgets.end() && found->second.seen == generation) {
        report(ErrorCode::duplicate_id, "Duplicate widget key in the same ID scope", true);
        return nullptr;
    }
    if (count == max_widgets) {
        report(ErrorCode::capacity, "Per-frame widget limit exceeded");
        return nullptr;
    }
    if (found == widgets.end()) found = widgets.try_emplace(id.value).first;
    if (found->second.last_present != completed) found->second.tooltip_seconds = 0;
    found->second.hovered = found->second.enabled = false;
    found->second.bounds = {};
    found->second.seen = generation;
    ++count;
    const auto editor = edits.find(id.value);
    if (editor != edits.end()) editor->second.last_present = completed;
    return &found->second;
}
bool FrameState::account_edit_bytes(std::size_t bytes) {
    if (error || bytes > max_retained_bytes) return false;
    if (label_bytes > max_retained_bytes - bytes) {
        report(ErrorCode::capacity, "Editor buffer exceeds retained byte limit");
        return false;
    }
    label_bytes += bytes;
    return true;
}
EditState* FrameState::edit_state(Id id) {
    if (error) return nullptr;
    auto found = edits.find(id.value);
    if (found == edits.end()) {
        if (count == max_widgets) {
            report(ErrorCode::capacity, "Per-frame widget limit exceeded");
            return nullptr;
        }
        EditState state{std::pmr::string{&memory}, std::pmr::string{&memory},
                        std::pmr::string{&memory}, std::pmr::vector<EditSnapshot>{&memory},
                        std::pmr::vector<EditLine>{&memory}};
        found = edits.try_emplace(id.value, std::move(state)).first;
    }
    return &found->second;
}
std::string_view FrameState::edit_display(Id id) {
    const auto found = edits.find(id.value);
    if (found == edits.end()) return {};
    const auto& draft = found->second.draft;
    return {draft.data(), draft.size()};
}
Id FrameState::keyed_id(std::string_view key) {
    if (key.empty()) {
        report(ErrorCode::invalid_argument, "Widget key must not be empty", true);
        return {};
    }
    if (key.size() > max_retained_bytes) {
        report(ErrorCode::capacity, "Widget key exceeds byte limit");
        return {};
    }
    // Explicit keys and positional text occupy separate ID namespaces.
    return child_id(child_id(stack.back().id, "key"), key);
}
Id FrameState::text_id() {
    auto& container = stack.back();
    const auto ordinal = container.next_text++;
    return child_id(child_id(container.id, "text"),
                    {reinterpret_cast<const char*>(&ordinal), sizeof(ordinal)});
}
bool FrameState::copy_label(std::string_view label) {
    if (error) return false;
    if (label.size() > max_retained_bytes - label_bytes) {
        report(ErrorCode::capacity, "Frame labels exceed byte limit");
        return false;
    }
    label_bytes += label.size();
    return true;
}
void FrameState::note_choice_error(std::string_view message) {
    note_diagnostic(ErrorCode::invalid_argument, message);
}
void FrameState::note_diagnostic(ErrorCode code, std::string_view message) {
    if (error || diagnostics.size() == 64) return;
    diagnostics.push_back({code, std::string(message)});
}
Rect FrameState::take(float height) {
    if (error) return {};
    auto result = stack.back().flow.take(height);
    if (auto* failed = std::get_if<Error>(&result)) {
        report(failed->code, failed->message.c_str(), true);
        return {};
    }
    return std::get<Rect>(result);
}
Rect FrameState::clip() const { return stack.back().clip; }
std::shared_ptr<const TextRun> FrameState::shape(std::string_view value, FontRole role, float size) {
    if (error) return {};
    // Display text defaults to the heading size; Theme has no separate token.
    if (size <= 0) size = role == FontRole::body ? theme.body_size : theme.heading_size;
    auto result = text->shape({value, role, size, scale});
    if (auto* failed = std::get_if<Error>(&result)) {
        report(std::move(*failed));
        return {};
    }
    auto run = std::get<std::shared_ptr<const TextRun>>(std::move(result));
    if (!run || !std::isfinite(run->width) || run->width < 0 ||
        !std::isfinite(run->height) || run->height < 0 || !std::isfinite(run->baseline)) {
        report(ErrorCode::invalid_font, "Text service returned invalid run metrics");
        return {};
    }
    return run;
}
void FrameState::glyphs(const TextRun& run, Vec2 top_left, Color color) {
    for (const auto& glyph : run.glyphs) {
        Rect bounds = glyph.bounds;
        // A6 already includes its baseline in each glyph's y-down bounds.
        bounds.x += top_left.x;
        bounds.y += top_left.y;
        canvas.image(bounds, glyph.texture, glyph.uv, color);
        if (!check_canvas()) return;
    }
}
bool FrameState::valid_input(const FrameInput& input) {
    const auto r = input.viewport;
    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.w) ||
        !std::isfinite(r.h) || r.w < 0 || r.h < 0 ||
        !std::isfinite(r.x + r.w) || !std::isfinite(r.y + r.h) ||
        !std::isfinite(input.scale) || input.scale <= 0 ||
        !std::isfinite(input.dt_seconds) || input.dt_seconds < 0) {
        report(ErrorCode::invalid_argument, "Frame geometry, scale and elapsed time must be finite and valid");
        return false;
    }
    std::size_t bytes = 0;
    const auto account = [&](const InputEvent& event) {
        if (sizeof(InputEvent) > max_retained_bytes - bytes) return false;
        bytes += sizeof(InputEvent);
        if (event.text.size() > max_retained_bytes - bytes) return false;
        bytes += event.text.size();
        return true;
    };
    for (const auto& record : interaction.deferred_) {
        if (!account(record.event)) {
            report(ErrorCode::capacity, "Deferred input exceeds frame byte limit");
            return false;
        }
    }
    for (const auto& event : input.events) {
        if (event.kind < EventKind::pointer_move || event.kind > EventKind::composition ||
            event.key < Key::none || event.key > Key::right_super ||
            event.button < PointerButton::left || event.button > PointerButton::x2 ||
            !std::isfinite(event.position.x) || !std::isfinite(event.position.y) ||
            !std::isfinite(event.wheel) || !std::isfinite(event.wheel_x)) {
            report(ErrorCode::invalid_argument, "Input event contains invalid values");
            return false;
        }
        if (!account(event)) {
            report(ErrorCode::capacity, "Queued and fresh input exceed frame byte limit");
            return false;
        }
    }
    return true;
}
} // namespace wui::detail

namespace wui {
Context::Context(std::shared_ptr<detail::FrameState> state) noexcept : state_(std::move(state)) {}
std::span<const Error> Context::diagnostics() const { return state_->diagnostics; }
std::optional<Vec2> Context::composition_caret() const { return state_->composition_caret; }
ContextStats Context::statistics() const {
    return {state_->memory.used(), state_->memory.peak(), state_->interaction.focused(),
            state_->interaction.captured()};
}
Context::~Context() {
    state_->owner_alive = false;
    state_->abort();
}
Result<std::unique_ptr<Context>> Context::create(ContextDesc desc) {
    try {
        if (!desc.text || !desc.textures || desc.max_widgets == 0 ||
            desc.max_retained_bytes == 0 || desc.max_packet_slots == 0 || desc.max_packet_slots > 4) {
            return Error{ErrorCode::invalid_argument, "Context requires text/store services and positive limits (at most four packet slots)"};
        }
        auto validated = resolve_theme(desc.theme);
        if (auto* error = std::get_if<Error>(&validated)) return std::move(*error);
        auto state = std::make_shared<detail::FrameState>(std::move(desc));
        state->canvas.begin({}, 1);
        auto failure = detail::CanvasAccess::status(state->canvas);
        detail::CanvasAccess::abort(state->canvas);
        if (failure) return std::move(*failure);
        return std::unique_ptr<Context>(new Context(std::move(state)));
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        return Error{ErrorCode::capacity, {}};
    }
}
Result<Frame> Context::begin_frame(const FrameInput& input) {
    auto& s = *state_;
    try {
        if (s.active) {
            s.report(ErrorCode::invalid_frame, "Context already has an active frame", true);
            return *s.error;
        }
        s.error.reset();
        s.diagnostics.clear();
        if (!s.valid_input(input)) return *s.error;
        if (s.generation == std::numeric_limits<std::uint64_t>::max() ||
            s.completed == std::numeric_limits<std::uint64_t>::max()) {
            return Error{ErrorCode::capacity, "Context frame generation exhausted"};
        }
        s.canvas.begin(input.viewport, input.scale);
        if (!s.check_canvas()) {
            auto failure = std::move(*s.error);
            detail::CanvasAccess::abort(s.canvas);
            return failure;
        }
        s.viewport = input.viewport;
        s.scale = input.scale;
        s.dt = static_cast<float>(std::min(input.dt_seconds, 0.05));
        s.elapsed_seconds = input.dt_seconds;
        s.count = s.label_bytes = 0;
        s.stack.clear();
        s.row_rects.clear();
        s.rows.clear();
        s.wrap_lines.clear();
        s.style_stack.clear();
        s.areas.clear();
        s.choice_columns.clear();
        s.overlay_count = 0;
        s.popup.emitted = false;
        s.stack.push_back({Id{1}, input.viewport, input.viewport,
                          Flow(input.viewport, s.theme.gap), {}, 0, 0});
        s.interaction.begin(input, s.popup.owner);
        ++s.generation;
        s.active = true;
        // A fully transparent canvas token leaves the target untouched, so an
        // overlay context can draw over existing content (e.g. a game frame).
        if (s.theme.canvas.a > 0) s.canvas.rounded_rect(input.viewport, 0, s.theme.canvas);
        if (!s.check_canvas()) {
            auto failure = std::move(*s.error);
            s.abort();
            return failure;
        }
        return Frame(state_, s.generation);
    } catch (const std::bad_alloc&) {
        s.abort();
        return Error{ErrorCode::capacity, {}};
    } catch (const std::length_error&) {
        s.abort();
        return Error{ErrorCode::capacity, {}};
    }
}
Status Context::set_theme(const Theme& theme) {
    try {
        if (state_->active) {
            state_->report(ErrorCode::invalid_frame, "Theme changes require a closed frame", true);
            return state_->error;
        }
        auto result = resolve_theme(theme);
        if (auto* failure = std::get_if<Error>(&result)) return std::move(*failure);
        state_->theme = std::get<Theme>(result);
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::capacity, {}};
    }
}
} // namespace wui
