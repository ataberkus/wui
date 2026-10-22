#include <wui/style.hpp>

#include <algorithm>

namespace wui {

float Transition::value() const {
    const float progress = duration <= 0.0f
                               ? 1.0f
                               : std::clamp(elapsed / duration, 0.0f, 1.0f);
    return from + (target - from) * progress;
}

void Transition::retarget(float next) {
    if (next == target) {
        return;
    }
    from = value();
    target = next;
    elapsed = 0.0f;
}

void Transition::tick(float dt, bool reduced_motion) {
    elapsed = reduced_motion
                  ? duration
                  : std::min(duration, elapsed + std::clamp(dt, 0.0f, 0.05f));
}

} // namespace wui
