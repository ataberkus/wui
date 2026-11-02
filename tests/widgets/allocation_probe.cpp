#include <wui/wui.hpp>

#include "../../src/paint/canvas_access.hpp"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace {
bool* watched_model = nullptr;
bool fail_after_mutation = false;
bool injected = false;
bool counting_semantic = false;
std::size_t semantic_alloc_calls = 0;
// When nonzero, the next allocation larger than this many bytes fails once.
std::size_t fail_next_larger_than = 0;

// Ignore tiny debug-STL iterator proxies only for fault injection: those can
// allocate inside noexcept constructors. Measurement counts every request.
void before_allocation(std::size_t bytes) {
    if (counting_semantic) ++semantic_alloc_calls;
    if (fail_next_larger_than != 0 && bytes > fail_next_larger_than) {
        fail_next_larger_than = 0;
        injected = true;
        throw std::bad_alloc{};
    }
    if (bytes >= 3 * sizeof(wui::Vertex) &&
        fail_after_mutation && watched_model && *watched_model) {
        injected = true;
        fail_after_mutation = false;
        throw std::bad_alloc{};
    }
}

void* allocate(std::size_t bytes) {
    before_allocation(bytes);
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc{};
}

void* allocate_aligned(std::size_t bytes, std::align_val_t alignment) {
    before_allocation(bytes);
    const auto align = static_cast<std::size_t>(alignment);
#ifdef _WIN32
    void* result = _aligned_malloc(bytes ? bytes : 1, align);
#else
    void* result = nullptr;
    if (::posix_memalign(&result, align < sizeof(void*) ? sizeof(void*) : align,
            bytes ? bytes : 1) != 0) {
        throw std::bad_alloc{};
    }
#endif
    if (!result) throw std::bad_alloc{};
    return result;
}

void free_aligned(void* pointer) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}

// Keep these replacements isolated from the other test executables.
void* operator new(std::size_t bytes) { return allocate(bytes); }
void* operator new[](std::size_t bytes) { return allocate(bytes); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return allocate(bytes); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocate_aligned(bytes, alignment);
}
void* operator new(std::size_t bytes, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
    try { return allocate_aligned(bytes, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { free_aligned(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
    free_aligned(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    free_aligned(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    free_aligned(pointer);
}

namespace {
int allocation_contracts() {
    constexpr auto alignment = std::align_val_t{256};
    constexpr std::size_t large = 3 * sizeof(wui::Vertex);
    for (const bool array : {false, true}) {
        for (const bool aligned : {false, true}) {
            for (const bool nothrow : {false, true}) {
                const auto request = [&](std::size_t bytes) -> void* {
                    if (aligned) {
                        if (nothrow) return array
                            ? ::operator new[](bytes, alignment, std::nothrow)
                            : ::operator new(bytes, alignment, std::nothrow);
                        return array ? ::operator new[](bytes, alignment)
                                     : ::operator new(bytes, alignment);
                    }
                    if (nothrow) return array ? ::operator new[](bytes, std::nothrow)
                                             : ::operator new(bytes, std::nothrow);
                    return array ? ::operator new[](bytes) : ::operator new(bytes);
                };
                const auto release = [&](void* pointer) {
                    if (aligned) {
                        if (array) ::operator delete[](pointer, alignment);
                        else ::operator delete(pointer, alignment);
                    } else {
                        if (array) ::operator delete[](pointer);
                        else ::operator delete(pointer);
                    }
                };
                bool model = false;
                watched_model = &model;
                fail_after_mutation = true;
                injected = false;
                semantic_alloc_calls = 0;
                counting_semantic = true;
                bool valid = true;
                for (const std::size_t bytes : {std::size_t{0}, std::size_t{1}, large}) {
                    void* pointer = request(bytes);
                    valid = valid && pointer && (!aligned ||
                        reinterpret_cast<std::uintptr_t>(pointer) % 256 == 0);
                    release(pointer);
                }
                valid = valid && !injected && fail_after_mutation;
                model = true;
                bool threw = false;
                void* failed = nullptr;
                try { failed = request(large); }
                catch (const std::bad_alloc&) { threw = true; }
                valid = valid && !failed && (threw != nothrow) &&
                    injected && !fail_after_mutation;
                release(failed);
                void* recovered = request(large);
                valid = valid && recovered && semantic_alloc_calls == 5;
                release(recovered);
                counting_semantic = false;
                fail_after_mutation = false;
                watched_model = nullptr;
                if (!valid) {
                    std::fprintf(stderr,
                        "allocation contract failed: array=%d aligned=%d nothrow=%d calls=%zu\n",
                        array, aligned, nothrow, semantic_alloc_calls);
                    return 1;
                }
            }
        }
    }
    std::puts("Scalar/array allocation alignment, counting and mutation-failure contracts passed.");
    return 0;
}

class Text final : public wui::IText {
public:
    Text() : run_(std::make_shared<wui::TextRun>()) {
        run_->width = 42; run_->height = 18; run_->baseline = 14;
        run_->display = "Istanbul";
        for (std::size_t i = 0; i < run_->display.size(); ++i)
            run_->grapheme_starts.push_back(i);
    }
    wui::Result<std::shared_ptr<const wui::TextRun>> shape(const wui::ShapeRequest&) override {
        return std::shared_ptr<const wui::TextRun>(run_);
    }
    wui::Status publish() override { return std::nullopt; }
    wui::Result<std::vector<std::size_t>> grapheme_offsets(std::string_view source) override {
        std::vector<std::size_t> offsets{0};
        for (std::size_t i = 0; i < source.size(); ++i) offsets.push_back(i + 1);
        return offsets;
    }
private:
    std::shared_ptr<wui::TextRun> run_;
};
int frame_reuse() {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    std::array<std::byte, 16> pixels{};
    pixels.fill(std::byte{255});
    auto image = textures->create(2, 2, pixels, wui::AlphaMode::straight);
    if (!std::holds_alternative<wui::TextureHandle>(image)) return 6;
    const auto icon = std::get<wui::TextureHandle>(image);
    auto created = wui::Context::create({wui::Theme::dark(), textures, std::make_shared<Text>()});
    if (!std::holds_alternative<std::unique_ptr<wui::Context>>(created)) return 2;
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    wui::FrameInput input{{0, 0, 640, 400}};
    input.dt_seconds = 0.1;
    bool value = true;
    const std::array<wui::Choice<int>, 2> choices{{{10, "First"}, {20, "Second"}}};
    int selected = 20;
    float scale = 1.0f;
    int copies = 3;
    // Longer than any small-string buffer: a per-frame copy would allocate.
    std::string name = "A display name longer than the small-string buffer";
    double amount = 12.5;
    wui::Vec2 popup_point{};
    for (int i = 0; i < 308; ++i) {
        if (i == 1) {
            input.events = {{wui::EventKind::pointer_down, popup_point},
                            {wui::EventKind::pointer_up, popup_point}};
        } else if (i == 2) {
            input.events.clear();
        }
        if (i == 8) semantic_alloc_calls = 0;
        if (i >= 8) counting_semantic = true;
        auto begun = context->begin_frame(input);
        if (!std::holds_alternative<wui::Frame>(begun)) return 3;
        auto frame = std::get<wui::Frame>(std::move(begun));
        {
            auto panel = frame.panel("preferences", {.title = "Preferences"});
            frame.heading("Widget foundation");
            frame.tabs("navigation", selected, choices);
            frame.radio("choice", "Mode", selected, choices);
            frame.dropdown("quality", "Quality", selected, choices);
            frame.tooltip("quality", "Quality help");
            frame.text("Istanbul");
            frame.description("Istanbul");
            auto custom = frame.custom("custom", "Custom", 24);
            if (!std::holds_alternative<wui::CustomItem>(custom)) return 5;
            const auto item = std::get<wui::CustomItem>(custom);
            frame.canvas().push_clip(item.clip);
            frame.canvas().gradient(item.bounds, {1, 0, 0, 1}, {0, 0, 1, 1});
            frame.canvas().border(item.bounds, 4, 1, {1, 1, 1, 1});
            frame.canvas().pop_clip();
            frame.checkbox("notifications", "Notifications", value);
            frame.slider("scale", "UI scale", scale, 0.5f, 2.0f);
            frame.slider("copies", "Copies", copies, 0, 10);
            frame.text_input("name", "Name", name);
            frame.numeric_input("amount", "Amount", amount, 0.0, 100.0);
            frame.validation("Enter a valid value");
            {
                auto style = frame.style({.accent = wui::Color{0.8f, 0.2f, 0.2f, 1}});
                frame.button("restore", "Restore defaults",
                             {.variant = wui::Variant::prominent, .invalid = true});
                frame.icon_button("help", "Help", icon);
            }
        }
        auto finished = frame.finish();
        counting_semantic = false;
        if (!std::holds_alternative<wui::DrawPacket>(finished)) return 4;
        if (i == 0) {
            const auto& semantics = std::get<wui::DrawPacket>(finished)->semantics;
            for (const auto& entry : semantics) {
                if (entry.role == wui::Role::dropdown && entry.label == "Quality") {
                    popup_point = {entry.bounds.x + 8, entry.bounds.y + 8};
                    break;
                }
            }
            if (popup_point.x == 0 && popup_point.y == 0) return 7;
        }
        if (i >= 8) {
            bool open = false, tooltip = false, validation = false;
            int sliders = 0;
            for (const auto& entry : std::get<wui::DrawPacket>(finished)->semantics) {
                open |= entry.role == wui::Role::dropdown && entry.label == "Second";
                tooltip |= entry.role == wui::Role::tooltip && entry.label == "Quality help";
                validation |= entry.role == wui::Role::validation &&
                              entry.label == "Enter a valid value";
                sliders += entry.role == wui::Role::slider;
            }
            if (!open || !tooltip || !validation || sliders != 2) return 8;
        }
    }
    if (semantic_alloc_calls != 0) {
        std::fprintf(stderr, "frame reuse allocated %zu times\n", semantic_alloc_calls);
        return 1;
    }
    std::puts("Warmed widget frame with popup, tooltip, validation, sliders, text/numeric inputs and scoped paint performs zero allocations across 300 frames.");
    return 0;
}

// A batched glyph run makes room for all of its glyphs at once, empty and
// invalid ones included. When that allocation fails it must paint and fail
// exactly like one Canvas::image per glyph on the same canvas, which here
// allocates only for its first quad (and gets that allocation).
int glyph_run_allocation_failure() {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    const std::array<std::byte, 4> pixel{std::byte{255}, std::byte{255}, std::byte{255},
                                         std::byte{255}};
    auto image = textures->create(1, 1, pixel, wui::AlphaMode::straight);
    if (!std::holds_alternative<wui::TextureHandle>(image)) return 20;
    const auto texture = std::get<wui::TextureHandle>(image);
    const wui::GlyphQuad empty{texture, {0, 0, 0, 9}, {0, 0, 1, 1}};
    const wui::GlyphQuad quad{texture, {1, 2, 6, 9}, {0, 0, 0.5f, 0.5f}};
    const wui::GlyphQuad invalid{texture, {0, 0, -1, 9}, {0, 0, 1, 1}};
    const wui::Vec2 offset{3, 4};
    const wui::Color tint{1, 0.5f, 0.25f, 0.5f};
    const std::array<std::vector<wui::GlyphQuad>, 2> runs{
        std::vector<wui::GlyphQuad>{empty, empty, empty, empty, empty, empty, empty, quad},
        std::vector<wui::GlyphQuad>{quad, invalid, quad, quad, quad, quad, quad, quad}};
    for (const auto& glyphs : runs) {
        wui::Canvas expected(textures, 1);
        expected.begin({0, 0, 100, 100}, 1);
        for (const auto& glyph : glyphs) {
            expected.image({glyph.bounds.x + offset.x, glyph.bounds.y + offset.y,
                            glyph.bounds.w, glyph.bounds.h},
                           glyph.texture, glyph.uv, tint);
        }
        const auto expected_status = wui::detail::CanvasAccess::status(expected);
        auto reference = expected.finish();

        wui::Canvas actual(textures, 1);
        actual.begin({0, 0, 100, 100}, 1);
        bool armed = true; // fails the next allocation of at least three vertices
        watched_model = &armed;
        fail_after_mutation = true;
        injected = false;
        bool threw = false;
        try {
            wui::detail::CanvasAccess::glyph_run(actual, glyphs, offset, tint);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        const bool hit = injected;
        fail_after_mutation = false;
        watched_model = nullptr;
        const auto actual_status = wui::detail::CanvasAccess::status(actual);
        auto result = actual.finish();
        if (threw || !hit) {
            std::fprintf(stderr, "glyph run allocation failure: threw=%d injected=%d\n", threw,
                         hit);
            return 21;
        }
        if (actual_status.has_value() != expected_status.has_value() ||
            (actual_status && (actual_status->code != expected_status->code ||
                               actual_status->message != expected_status->message))) {
            return 22;
        }
        if (std::holds_alternative<wui::DrawPacket>(result) !=
            std::holds_alternative<wui::DrawPacket>(reference)) {
            return 23;
        }
        if (std::holds_alternative<wui::DrawPacket>(result)) {
            const auto& a = *std::get<wui::DrawPacket>(result);
            const auto& b = *std::get<wui::DrawPacket>(reference);
            if (a.vertices.size() != 4 || a.vertices.size() != b.vertices.size() ||
                std::memcmp(a.vertices.data(), b.vertices.data(),
                            a.vertices.size() * sizeof(wui::Vertex)) != 0 ||
                a.indices != b.indices || a.batches.size() != b.batches.size()) {
                return 24;
            }
        }
    }
    std::puts("A glyph run whose room cannot be allocated paints and fails like one image per glyph.");
    return 0;
}

// Scope closers are noexcept, yet closing a row cell or a scroll area can
// allocate: reporting a failed canvas copies its error message, and popping
// the scope's clip off an unbalanced canvas clip stack builds an error, as
// does a refused row allocation when a row closes. When that allocation
// fails, the frame must be rejected rather than the process terminated.
int closer_allocation_failure() {
    auto created = wui::Context::create({wui::Theme::dark(),
        std::make_shared<wui::TextureStore>(1024 * 1024), std::make_shared<Text>()});
    if (!std::holds_alternative<std::unique_ptr<wui::Context>>(created)) return 30;
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    const std::array<wui::Column, 1> columns{{{wui::ColumnKind::share, 1}}};
    // Debug-STL proxies fit in this many bytes; every error message involved
    // needs a larger buffer.
    constexpr std::size_t message_bytes = 16;
    enum class Failure { canvas, clip, row };
    const auto run = [&](bool cell, Failure failure, wui::ErrorCode expected) {
        wui::FrameInput input{{0, 0, 400, 300}};
        // The row case: a viewport far above the origin lets a cell hold
        // content whose height overflows the enclosing flow when the row
        // closes, although the cell's own flow accepted it.
        if (failure == Failure::row) input.viewport.y = -3e38f;
        auto begun = context->begin_frame(input);
        if (!std::holds_alternative<wui::Frame>(begun)) return 31;
        auto frame = std::get<wui::Frame>(std::move(begun));
        if (failure == Failure::row) frame.spacer(3e38f);
        const auto fail_inside = [&] {
            if (failure == Failure::canvas) {
                // The canvas keeps this error; the frame learns of it at close.
                frame.canvas().rounded_rect(
                    {0, 0, std::numeric_limits<float>::quiet_NaN(), 10}, 0, {1, 1, 1, 1});
            } else if (failure == Failure::clip) {
                frame.canvas().pop_clip(); // the scope's own clip
            } else {
                frame.spacer(3e38f);
            }
            injected = false;
            fail_next_larger_than = message_bytes;
        };
        if (cell) {
            auto row = frame.row("row", columns);
            auto inner = row.cell();
            fail_inside();
        } else {
            auto scroll = frame.scroll("scroll", 100);
            fail_inside();
        }
        fail_next_larger_than = 0;
        const bool hit = injected;
        injected = false;
        auto finished = frame.finish();
        if (!hit) {
            std::fprintf(stderr, "closer allocation failure: cell=%d failure=%d not injected\n",
                         cell, static_cast<int>(failure));
            return 32;
        }
        if (!std::holds_alternative<wui::Error>(finished) ||
            std::get<wui::Error>(finished).code != expected) {
            std::fprintf(stderr, "closer allocation failure: cell=%d failure=%d not rejected\n",
                         cell, static_cast<int>(failure));
            return 33;
        }
        // The context recovers: the same scope closes normally next frame.
        auto again = context->begin_frame(wui::FrameInput{{0, 0, 400, 300}});
        if (!std::holds_alternative<wui::Frame>(again)) return 34;
        auto next = std::get<wui::Frame>(std::move(again));
        if (cell) {
            auto row = next.row("row", columns);
            auto inner = row.cell();
            next.text("Istanbul");
        } else {
            auto scroll = next.scroll("scroll", 100);
            next.text("Istanbul");
        }
        if (!std::holds_alternative<wui::DrawPacket>(next.finish())) return 35;
        return 0;
    };
    for (const bool cell : {true, false}) {
        if (const int failed = run(cell, Failure::canvas, wui::ErrorCode::capacity)) return failed;
        if (const int failed = run(cell, Failure::clip, wui::ErrorCode::capacity)) return failed;
    }
#ifdef NDEBUG
    // A refused row allocation is a programmer contract violation, which
    // asserts in Debug builds; Release builds report it as an error. Only
    // the message copy can fail here, so the error keeps its code.
    if (const int failed = run(true, Failure::row, wui::ErrorCode::invalid_argument)) return failed;
#endif
    std::puts("Failed allocations while closing row cells and scroll areas reject the frame.");
    return 0;
}

// Generic popups (anchored, then modal) paint a raised sheet: its shadow comes
// from the canvas shadow cache, so warmed frames still allocate nothing.
int generic_popup_reuse() {
    auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
    auto created = wui::Context::create({wui::Theme::dark(), textures, std::make_shared<Text>()});
    if (!std::holds_alternative<std::unique_ptr<wui::Context>>(created)) return 12;
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    bool value = true;
    semantic_alloc_calls = 0;
    for (const bool modal : {false, true}) {
        for (int i = 0; i < 158; ++i) {
            wui::FrameInput input{{0, 0, 640, 400}, modal ? 1.5f : 1.0f};
            input.dt_seconds = 0.1;
            if (i >= 8) counting_semantic = true;
            auto begun = context->begin_frame(input);
            if (!std::holds_alternative<wui::Frame>(begun)) return 13;
            auto frame = std::get<wui::Frame>(std::move(begun));
            {
                auto panel = frame.panel("root");
                const auto anchor = frame.next_rect(36);
                frame.button("anchor", "Options");
                frame.checkbox("below", "Below", value);
                if (i == 0) frame.open_popup(modal ? "dialog" : "menu");
                if (auto popup = frame.popup(modal ? "dialog" : "menu", {anchor, 0, modal})) {
                    frame.text("Istanbul");
                    frame.description("Istanbul");
                    frame.button("inside", "Inside");
                }
            }
            auto finished = frame.finish();
            counting_semantic = false;
            if (!std::holds_alternative<wui::DrawPacket>(finished)) return 14;
            if (i >= 8) {
                // The untitled root panel is one untitled panel; the open
                // popup's sheet must be a second one.
                int untitled_panels = 0;
                for (const auto& entry : std::get<wui::DrawPacket>(finished)->semantics)
                    if (entry.role == wui::Role::panel && entry.label.empty()) ++untitled_panels;
                if (untitled_panels < 2) return 15;
            }
        }
    }
    if (semantic_alloc_calls != 0) {
        std::fprintf(stderr, "generic popup reuse allocated %zu times\n", semantic_alloc_calls);
        return 1;
    }
    std::puts("Warmed anchored and modal generic popups perform zero allocations across 300 frames.");
    return 0;
}
}

int main(int argc, char** argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    if (argc == 2 && std::string(argv[1]) == "--allocation-contracts") {
        return allocation_contracts();
    }
    if (argc == 2 && std::string(argv[1]) == "--semantic-reuse") {
        auto textures = std::make_shared<wui::TextureStore>(1024 * 1024);
        wui::Canvas canvas(textures, 1);
        const std::string label =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
        canvas.begin({0, 0, 100, 100}, 1);
        canvas.semantic({42}, wui::Role::button, label, {10, 20, 30, 40}, true, false, false);
        {
            auto finished = canvas.finish();
            if (!std::holds_alternative<wui::DrawPacket>(finished)) return 2;
        }
        semantic_alloc_calls = 0;
        for (int i = 0; i < 300; ++i) {
            canvas.begin({0, 0, 100, 100}, 1);
            counting_semantic = true;
            canvas.semantic({42}, wui::Role::button, label, {10, 20, 30, 40}, true, false, false);
            counting_semantic = false;
            auto finished = canvas.finish();
            if (!std::holds_alternative<wui::DrawPacket>(finished)) return 3;
        }
        if (semantic_alloc_calls != 0) {
            std::fprintf(stderr, "semantic reuse allocated %zu times\n", semantic_alloc_calls);
            return 1;
        }
        std::puts("Warmed Canvas::semantic performs zero allocations across 300 frames.");
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--frame-reuse") {
        if (const int result = frame_reuse()) return result;
        return generic_popup_reuse();
    }
    if (argc != 1) {
        std::fprintf(stderr,
            "usage: wui_allocation_probe [--semantic-reuse|--allocation-contracts|--frame-reuse]\n");
        return 2;
    }
    if (const int failed = glyph_run_allocation_failure()) return failed;
    if (const int failed = closer_allocation_failure()) return failed;
    auto created = wui::Context::create({wui::Theme::dark(),
        std::make_shared<wui::TextureStore>(1024 * 1024), std::make_shared<Text>()});
    if (!std::holds_alternative<std::unique_ptr<wui::Context>>(created)) return 2;
    auto context = std::get<std::unique_ptr<wui::Context>>(std::move(created));
    wui::FrameInput input{{0, 0, 400, 300}};
    input.events = {{wui::EventKind::pointer_down, {30, 10}},
                    {wui::EventKind::pointer_up, {30, 10}}};
    auto begun = context->begin_frame(input);
    if (!std::holds_alternative<wui::Frame>(begun)) return 3;
    auto frame = std::get<wui::Frame>(std::move(begun));
    bool value = false;
    watched_model = &value;
    fail_after_mutation = true;
    const bool changed = frame.checkbox("choice", "Choice", value);
    fail_after_mutation = false;
    watched_model = nullptr;
    if (!injected || !value || !changed) {
        std::fprintf(stderr, "injected=%d value=%d changed=%d\n", injected, value, changed);
        return 1;
    }
    bool later = false;
    if (frame.checkbox("later", "Later", later) || later) return 4;
    auto finished = frame.finish();
    if (!std::holds_alternative<wui::Error>(finished) ||
        std::get<wui::Error>(finished).code != wui::ErrorCode::capacity) return 5;
    std::puts("Post-mutation allocation failure preserves changed=true and rejects finalization.");
}
