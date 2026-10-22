#pragma once

#include <wui/draw.hpp>

#include <cstdint>

namespace wui::test {

// Deterministic font/widget-independent reference packet for backend
// conformance. Viewport is {0,0,128,128}. See packet_fixture.cpp for the
// exact draw order. Revision 2 changes one checker texel without changing
// its handle or dimensions. Throws std::invalid_argument for any revision
// other than 1 or 2 (test-only helper; the A9 signature returns DrawPacket
// directly and cannot carry a Status).
DrawPacket reference_packet(std::uint64_t texture_revision, float scale = 1);

} // namespace wui::test
