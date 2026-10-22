#pragma once

#include <wui/draw.hpp>
#include <wui/frame.hpp>
#include <wui/resources.hpp>
#include <wui/style.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Visible-control stress workloads measured beside the fixed gallery. They are
// performance fixtures, not the human-readable gallery.
enum class StressWorkload { controls_100, controls_1000, virtual_10000 };

// Caller-owned gallery model. Tabs, scroll offsets, focus, popups and edit
// drafts live in the Context; only values the application owns live here.
struct GalleryState {
    bool notifications = true;
    unsigned restore_count = 0;
    int section = 1; // 0 Appearance, 1 Controls (startup), 2 Diagnostics
    int density = 0; // 0 comfortable, 1 compact
    int accent_choice = 0;
    bool reduced_motion = false;
    bool scoped_accent = true;
    // A model value only; render scale stays owned by the host.
    float scale = 1.0f;
    int view = 0;
    int quality = 1;
    float volume = 0.6f;
    int quantity = 3;
    std::string name = "İstanbul";
    double amount = 12.5;
    wui::TextureHandle icon{};
    std::array<std::uint64_t, 1000> grid_ids{};
    std::vector<std::uint64_t> row_ids;

    // Host-updated between frames; build_gallery only reads these.
    float viewport_height = 400.0f;
    std::size_t last_vertices = 0, last_indices = 0, last_batches = 0, last_textures = 0,
                last_semantics = 0, last_diagnostics = 0;
    std::uint64_t focused_id = 0;
    std::string focused_label;
    wui::Rect focused_bounds{};
    std::string last_diagnostic;
};

// Fills persistent grid/list IDs once; scrolling never regenerates them.
void prepare_gallery_model(GalleryState& state);
// Registers the one shared 8x8 straight-alpha icon used by every backend.
wui::Result<wui::TextureHandle> make_gallery_icon(wui::TextureStore& store);
// Theme selected by the model; hosts apply it with Context::set_theme between frames.
wui::Result<wui::Theme> gallery_theme(const GalleryState& state);
// Records last-frame packet and focus facts shown by the Diagnostics section.
void observe_gallery_frame(GalleryState& state, const wui::PacketData& packet,
                           std::span<const wui::Error> diagnostics);

// Compact theme with zero gap: 28px controls on a 28px pitch.
wui::Result<wui::Theme> stress_theme(const GalleryState& state);
// controls_100: 10x10 labeled buttons; controls_1000: 40x25 icon buttons;
// virtual_10000: visible_rows of 10,000 persistent rows in a viewport-high scroll.
void build_stress_gallery(wui::Frame& ui, GalleryState& state, StressWorkload workload);

void build_gallery(wui::Frame& ui, GalleryState& state);
void build_gallery(wui::Frame& ui, GalleryState& state, wui::WidgetOptions options);
