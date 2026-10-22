#include "gallery.hpp"

#include <wui/canvas.hpp>
#include <wui/layout.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <string_view>
#include <variant>

namespace {

constexpr wui::Color rgb(int r, int g, int b) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
}

constexpr std::array accents{rgb(0x9c, 0x8c, 0xff), rgb(0x2d, 0xc5, 0xb4),
                             rgb(0xf2, 0xb1, 0x4c), rgb(0xf2, 0x6d, 0x8f)};

constexpr std::array sections{wui::Choice<int>{0, "Appearance"},
                              wui::Choice<int>{1, "Controls"},
                              wui::Choice<int>{2, "Diagnostics"}};
constexpr std::array densities{wui::Choice<int>{0, "Comfortable"},
                               wui::Choice<int>{1, "Compact"}};
constexpr std::array accent_names{wui::Choice<int>{0, "Violet"}, wui::Choice<int>{1, "Teal"},
                                  wui::Choice<int>{2, "Amber"}, wui::Choice<int>{3, "Rose"}};
constexpr std::array views{wui::Choice<int>{0, "List"}, wui::Choice<int>{1, "Grid"},
                           wui::Choice<int>{2, "Board"}};
// The radio and dropdown present the same values in different orders.
constexpr std::array quality_radio{wui::Choice<int>{2, "High"}, wui::Choice<int>{0, "Low"},
                                   wui::Choice<int>{1, "Balanced"}};
constexpr std::array quality_menu{wui::Choice<int>{0, "Low"}, wui::Choice<int>{1, "Balanced"},
                                  wui::Choice<int>{2, "High"}};
constexpr std::array halves{wui::Column{wui::ColumnKind::share, 1},
                            wui::Column{wui::ColumnKind::share, 1}};
constexpr std::array thirds{wui::Column{wui::ColumnKind::share, 1},
                            wui::Column{wui::ColumnKind::share, 1},
                            wui::Column{wui::ColumnKind::share, 1}};
constexpr std::array icon_columns{wui::Column{wui::ColumnKind::fixed, 44},
                                  wui::Column{wui::ColumnKind::fixed, 44},
                                  wui::Column{wui::ColumnKind::share, 1}};

wui::WidgetOptions with(wui::WidgetOptions options, wui::Variant variant) {
    options.variant = variant;
    return options;
}

wui::WidgetOptions invalid_if(wui::WidgetOptions options, bool invalid) {
    options.invalid = invalid;
    return options;
}

template<class... Args>
std::string_view format(std::span<char> buffer, const char* pattern, Args... args) {
    const int written = std::snprintf(buffer.data(), buffer.size(), pattern, args...);
    if (written <= 0) return {};
    return {buffer.data(), std::min(buffer.size() - 1, static_cast<std::size_t>(written))};
}

void build_appearance(wui::Frame& ui, GalleryState& state, wui::WidgetOptions options) {
    ui.heading("Appearance");
    ui.description("İstanbul — reusable C++ controls drawn by the same source on every backend.",
                   {.color = rgb(0xb7, 0xbd, 0xcc)});
    {
        auto row = ui.row("theme", halves);
        {
            auto cell = row.cell();
            ui.radio("density", "Density", state.density, std::span(densities), options);
        }
        auto cell = row.cell();
        ui.dropdown("accent", "Accent", state.accent_choice, std::span(accent_names), options);
        ui.checkbox("reduced_motion", "Reduce motion", state.reduced_motion, options);
    }
    ui.slider("scale", "UI scale", state.scale, 0.75f, 2.0f, options);
    ui.description("UI scale is a model value; the host owns the render scale.",
                   {.color = rgb(0x8f, 0xd6, 0xc9)});
    {
        auto row = ui.row("variants", thirds);
        {
            auto cell = row.cell();
            ui.button("prominent", "Prominent", with(options, wui::Variant::prominent));
        }
        {
            auto cell = row.cell();
            ui.button("quiet", "Quiet", with(options, wui::Variant::quiet));
        }
        auto cell = row.cell();
        auto disabled = options;
        disabled.enabled = false;
        ui.button("disabled", "Disabled", disabled);
    }
    {
        auto row = ui.row("icons", icon_columns);
        {
            auto cell = row.cell();
            ui.icon_button("add", "Add item", state.icon, options);
        }
        {
            auto cell = row.cell();
            ui.icon_button("pin", "Pin item", state.icon, with(options, wui::Variant::quiet));
        }
        auto cell = row.cell();
        auto scope = ui.style({.accent = accents[3]});
        ui.checkbox("scoped_accent", "Scoped accent", state.scoped_accent, options);
    }
    auto painted = ui.custom("paint", "Paint samples", 72, options);
    const auto* item = std::get_if<wui::CustomItem>(&painted);
    // Four samples need positive interiors; very narrow layouts keep only the semantic.
    if (item && (item->bounds.w - 24) / 4 > 16 && item->bounds.h > 24) {
        auto& canvas = ui.canvas();
        const auto b = item->bounds;
        const float w = (b.w - 24) / 4;
        canvas.push_clip(item->clip);
        canvas.gradient({b.x, b.y + 4, w, b.h - 8}, accents[0], accents[1]);
        canvas.border({b.x + w + 8, b.y + 4, w, b.h - 8}, 8, 2, accents[2]);
        canvas.shadow({b.x + 2 * (w + 8) + 6, b.y + 10, w - 12, b.h - 20}, 8, 4,
                      {0, 0, 0, 0.6f});
        canvas.rounded_rect({b.x + 2 * (w + 8) + 6, b.y + 10, w - 12, b.h - 20}, 8,
                            rgb(0x23, 0x27, 0x35));
        const float side = std::min(w, b.h - 8);
        canvas.image({b.x + 3 * (w + 8), b.y + 4, side, side}, state.icon, {0, 0, 1, 1},
                     {1, 1, 1, 1});
        canvas.pop_clip();
    }
}

void build_controls(wui::Frame& ui, GalleryState& state, wui::WidgetOptions options) {
    ui.checkbox("notifications", "Notifications", state.notifications, options);
    if (ui.button("restore", "Restore defaults", options)) {
        state.notifications = true;
        ++state.restore_count;
    }
    ui.tooltip("restore", "Turns notifications back on");
    {
        auto row = ui.row("choices", halves);
        {
            auto cell = row.cell();
            ui.tabs("view", state.view, std::span(views), wui::Orientation::vertical, options);
        }
        auto cell = row.cell();
        ui.radio("quality", "Quality", state.quality, std::span(quality_radio), options);
    }
    {
        auto row = ui.row("menu", halves);
        {
            auto cell = row.cell();
            ui.text("Quality menu (edge anchored)");
        }
        auto cell = row.cell();
        ui.dropdown("quality_menu", "Quality menu", state.quality, std::span(quality_menu),
                    options);
    }
    ui.slider("volume", "Volume", state.volume, 0.0f, 1.0f, options);
    const bool too_many = state.quantity > 8;
    ui.slider("quantity", "Quantity", state.quantity, 0, 10, invalid_if(options, too_many));
    if (too_many) ui.validation("Quantities above 8 need approval");
    const bool unnamed = state.name.empty();
    ui.text_input("name", "Name", state.name, invalid_if(options, unnamed));
    if (unnamed) ui.validation("Name is required");
    ui.numeric_input("amount", "Amount", state.amount, 0.0, 1000.0, options);
    ui.description("Amounts commit on Enter or when focus leaves; Escape cancels.");
}

void build_diagnostics(wui::Frame& ui, GalleryState& state, wui::WidgetOptions options) {
    std::array<char, 160> line{};
    ui.text(format(line, "Focused: %s", state.focused_id ? state.focused_label.c_str() : "none"));
    const auto& f = state.focused_bounds;
    ui.text(format(line, "Focus bounds: %.0f, %.0f  %.0f x %.0f", f.x, f.y, f.w, f.h));
    ui.text(format(line, "Last packet: %zu vertices, %zu indices, %zu batches",
                   state.last_vertices, state.last_indices, state.last_batches));
    ui.text(format(line, "Textures: %zu  Semantics: %zu  Diagnostics: %zu",
                   state.last_textures, state.last_semantics, state.last_diagnostics));
    if (!state.last_diagnostic.empty())
        ui.description(state.last_diagnostic, {.color = accents[2]});

    auto theme = gallery_theme(state);
    if (!std::holds_alternative<wui::Theme>(theme)) return;
    const auto& metrics = std::get<wui::Theme>(theme);
    ui.text(format(line, "Virtual list: %zu persistent rows", state.row_ids.size()));
    auto list = ui.scroll("rows", 220);
    // Rows are fixed-height buttons; the pitch includes the flow gap.
    const float pitch = metrics.control_height + metrics.gap;
    auto range = wui::visible_rows(state.row_ids.size(), pitch, list.offset(), list.viewport().h);
    if (!std::holds_alternative<wui::VisibleRows>(range)) return;
    const auto [first, last] = std::get<wui::VisibleRows>(range);
    if (first > 0) ui.spacer(static_cast<float>(first) * pitch - metrics.gap);
    std::array<char, 24> key{};
    for (std::size_t i = first; i < last; ++i) {
        const auto [end, error] = std::to_chars(key.data(), key.data() + key.size(),
                                                state.row_ids[i], 16);
        if (error != std::errc{}) return;
        ui.button({key.data(), static_cast<std::size_t>(end - key.data())},
                  format(line, "Row %zu", i + 1), with(options, wui::Variant::quiet));
    }
    if (last < state.row_ids.size())
        ui.spacer(static_cast<float>(state.row_ids.size() - last) * pitch - metrics.gap);
}

} // namespace

void prepare_gallery_model(GalleryState& state) {
    if (!state.row_ids.empty()) return;
    state.row_ids.resize(10'000);
    for (std::size_t i = 0; i < state.row_ids.size(); ++i)
        state.row_ids[i] = 0x100'000u + i * 2; // persistent model value, not viewport index
    for (std::size_t i = 0; i < state.grid_ids.size(); ++i)
        state.grid_ids[i] = 0x200'000u + i * 2;
}

wui::Result<wui::TextureHandle> make_gallery_icon(wui::TextureStore& store) {
    std::array<std::byte, 8 * 8 * 4> rgba{};
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const bool ink = x == 3 || x == 4 || y == 3 || y == 4;
            const std::size_t at = (y * 8 + x) * 4;
            rgba[at] = rgba[at + 1] = rgba[at + 2] = std::byte{0xe0};
            rgba[at + 3] = ink ? std::byte{0xff} : std::byte{0};
        }
    }
    return store.create(8, 8, rgba, wui::AlphaMode::straight);
}

wui::Result<wui::Theme> gallery_theme(const GalleryState& state) {
    auto theme = wui::Theme::dark();
    if (state.accent_choice >= 0 && static_cast<std::size_t>(state.accent_choice) < accents.size())
        theme.accent = accents[static_cast<std::size_t>(state.accent_choice)];
    theme.reduced_motion = state.reduced_motion;
    return wui::resolve_theme(theme, state.density == 1 ? wui::Density::compact
                                                        : wui::Density::comfortable);
}

void observe_gallery_frame(GalleryState& state, const wui::PacketData& packet,
                           std::span<const wui::Error> diagnostics) {
    state.last_vertices = packet.vertices.size();
    state.last_indices = packet.indices.size();
    state.last_batches = packet.batches.size();
    state.last_textures = packet.textures.size();
    state.last_semantics = packet.semantics.size();
    state.last_diagnostics = diagnostics.size();
    if (diagnostics.empty()) state.last_diagnostic.clear();
    else state.last_diagnostic = diagnostics.back().message;
    state.focused_id = 0;
    state.focused_bounds = {};
    for (const auto& item : packet.semantics) {
        if (!item.focused) continue;
        state.focused_id = item.id.value;
        state.focused_label = item.label;
        state.focused_bounds = item.bounds;
        return;
    }
    state.focused_label.clear();
}

wui::Result<wui::Theme> stress_theme(const GalleryState& state) {
    auto theme = wui::Theme::dark();
    theme.reduced_motion = state.reduced_motion;
    auto resolved = wui::resolve_theme(theme, wui::Density::compact);
    if (auto* value = std::get_if<wui::Theme>(&resolved)) value->gap = 0;
    return resolved;
}

void build_stress_gallery(wui::Frame& ui, GalleryState& state, StressWorkload workload) {
    std::array<char, 24> key{};
    std::array<char, 32> label{};
    const auto key_of = [&](std::uint64_t id) -> std::string_view {
        const auto [end, error] = std::to_chars(key.data(), key.data() + key.size(), id, 16);
        if (error != std::errc{}) return {};
        return {key.data(), static_cast<std::size_t>(end - key.data())};
    };
    switch (workload) {
    case StressWorkload::controls_100: {
        constexpr std::size_t columns = 10, rows = 10;
        static constexpr auto shares = [] {
            std::array<wui::Column, columns> result{};
            result.fill({wui::ColumnKind::share, 1.0f});
            return result;
        }();
        for (std::size_t r = 0; r < rows; ++r) {
            auto row = ui.row(key_of(r), shares);
            for (std::size_t c = 0; c < columns; ++c) {
                auto cell = row.cell();
                const std::uint64_t id = state.grid_ids[r * columns + c];
                const auto name = format(label, "Button %zu", r * columns + c + 1);
                ui.button(key_of(id), name);
            }
        }
        break;
    }
    case StressWorkload::controls_1000: {
        constexpr std::size_t columns = 40, rows = 25;
        static constexpr auto shares = [] {
            std::array<wui::Column, columns> result{};
            result.fill({wui::ColumnKind::share, 1.0f});
            return result;
        }();
        for (std::size_t r = 0; r < rows; ++r) {
            auto row = ui.row(key_of(r), shares);
            for (std::size_t c = 0; c < columns; ++c) {
                auto cell = row.cell();
                const std::uint64_t id = state.grid_ids[r * columns + c];
                const auto name = format(label, "Item %zu", r * columns + c + 1);
                ui.icon_button(key_of(id), name, state.icon);
            }
        }
        break;
    }
    case StressWorkload::virtual_10000: {
        auto theme = stress_theme(state);
        if (!std::holds_alternative<wui::Theme>(theme)) return;
        const float pitch = std::get<wui::Theme>(theme).control_height;
        auto list = ui.scroll("stress_rows", state.viewport_height);
        auto range = wui::visible_rows(state.row_ids.size(), pitch, list.offset(),
                                       list.viewport().h);
        if (!std::holds_alternative<wui::VisibleRows>(range)) return;
        const auto [first, last] = std::get<wui::VisibleRows>(range);
        if (first > 0) ui.spacer(static_cast<float>(first) * pitch);
        for (std::size_t i = first; i < last; ++i)
            ui.button(key_of(state.row_ids[i]), format(label, "Row %zu", i + 1),
                      with({}, wui::Variant::quiet));
        if (last < state.row_ids.size())
            ui.spacer(static_cast<float>(state.row_ids.size() - last) * pitch);
        break;
    }
    }
}

void build_gallery(wui::Frame& ui, GalleryState& state) {
    build_gallery(ui, state, wui::WidgetOptions{});
}

void build_gallery(wui::Frame& ui, GalleryState& state, wui::WidgetOptions options) {
    auto page = ui.scroll("page", state.viewport_height);
    auto panel = ui.panel("gallery", {.title = "Widget gallery"});
    ui.tabs("section", state.section, std::span(sections), wui::Orientation::horizontal, options);
    switch (state.section) {
    case 0: build_appearance(ui, state, options); break;
    case 1: build_controls(ui, state, options); break;
    case 2: build_diagnostics(ui, state, options); break;
    default: break;
    }
}
