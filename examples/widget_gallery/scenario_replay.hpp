#pragma once

#include <wui/draw.hpp>
#include <wui/resources.hpp>
#include <wui/text.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Renderer-neutral replay of the gallery acceptance scenarios. It drives
// Context input directly (no window, no graphics API) through the same
// semantic-bound targeting as the native D3D11 validator and asserts the same
// model outcomes. Every backend's conformance run renders its checkpoints.
//
// Native-only and deliberately absent here: WM_DPICHANGED and real monitor
// moves, minimize, TME_LEAVE/real capture, IME composition (covered by the
// wui-win32 test), the interactive clipboard, and device recreation (a
// conformance lifetime case per backend instead).

struct ReplayServices {
    std::shared_ptr<wui::TextureStore> textures;
    std::shared_ptr<wui::IText> text;
};

struct ReplayCheckpoint {
    std::string name;            // e.g. "scenario-03-dropdown-edges/popup-bottom-edge"
    std::uint32_t width = 0, height = 0; // physical pixels
    float scale = 1;
    // Owned copy of the frame's packet: holding it never pins a Context slot.
    wui::DrawPacket packet;
};

struct ReplayResult {
    std::vector<ReplayCheckpoint> checkpoints;
    std::string log;             // one PASS line per scenario plus observations
};

// Runs the scale set (4 simulated scales), scenarios 1-10 and the portable
// extra cases. Returns the first failed expectation as an Error.
wui::Result<ReplayResult> replay_gallery(const ReplayServices& services);
