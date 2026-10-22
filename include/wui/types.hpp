#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace wui {

struct Vec2 {
    float x = 0;
    float y = 0;
};

struct Rect {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
};

struct Color {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;
};

enum class ErrorCode {
    invalid_argument,
    duplicate_id,
    invalid_frame,
    unbalanced_scope,
    capacity,
    invalid_packet,
    stale_texture,
    invalid_font,
    unsupported,
    backend_failure,
    device_lost,
    busy
};

struct Error {
    ErrorCode code;
    std::string message;
};

template<class T>
using Result = std::variant<T, Error>;

using Status = std::optional<Error>;

// These return-only helpers produce safe deterministic values for nonfinite input.
// Checked frame, canvas, and resource entrypoints reject nonfinite caller input with
// ErrorCode::invalid_argument through Result or Status.
Color premultiply(Color color);
Rect intersect(Rect a, Rect b);
bool contains(Rect rect, Vec2 point);

} // namespace wui
