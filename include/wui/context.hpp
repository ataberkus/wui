#pragma once

#include <wui/frame.hpp>
#include <wui/input.hpp>
#include <wui/style.hpp>
#include <wui/text.hpp>
#include <cstddef>
#include <memory>
#include <span>

namespace wui {
struct ContextDesc {
    Theme theme = Theme::dark();
    std::shared_ptr<TextureStore> textures;
    std::shared_ptr<IText> text;
    std::shared_ptr<IClipboard> clipboard;
    std::size_t max_widgets = 4096, max_retained_bytes = 16 * 1024 * 1024;
    std::size_t max_packet_slots = 4;
};

// Allocation-free snapshot. Retained bytes count the Context's bounded PMR
// storage (ID/animation state, scope containers); peak never decreases.
struct ContextStats {
    std::size_t retained_bytes = 0, peak_retained_bytes = 0;
    Id focused, captured;
};

// Single externally synchronized producer; completed packets may outlive it.
// The text service must publish into this TextureStore. No model pointers are
// retained. Theme changes apply between frames (resolve_theme selects density).
// max_widgets includes panels and text per frame. max_retained_bytes bounds
// retained ID/animation and scope-container allocations; input and copied labels
// each have the same per-frame byte ceiling. Packet pixels/geometry and IText
// have their own separate resource limits. Absent ID state expires after 120
// absent completed frames. Packet readers prevent slot reuse (busy at begin).
class Context {
public:
    static Result<std::unique_ptr<Context>> create(ContextDesc desc);
    Result<Frame> begin_frame(const FrameInput& input);
    std::span<const Error> diagnostics() const;
    std::optional<Vec2> composition_caret() const;
    Status set_theme(const Theme& theme);
    ContextStats statistics() const;
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
private:
    explicit Context(std::shared_ptr<detail::FrameState>) noexcept;
    std::shared_ptr<detail::FrameState> state_;
};
} // namespace wui
