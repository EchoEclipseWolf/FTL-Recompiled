#include "run.hpp"
#include <algorithm>
#include <cmath>

namespace ftl {
void Run::renderSunEnvironment() {
    // SpaceManager::OnRenderForeground0x418310, AnimationTracker0x59c490.
    const auto alpha = [&](int seconds) {
        const float duration = static_cast<float>(seconds);
        const float current = static_cast<float>(environment_.sunTicks % (seconds * 60 + 1)) / 60.f;
        return (duration * .5f - std::abs(current - duration * .5f)) / (duration * .5f);
    };
    const float loss = 1.f - static_cast<float>(hull_) / ship_.hull;
    const auto tint = [&](float opacity) {
        return SDL_Color{static_cast<Uint8>(255.f * (userPause_ ? .4f + .4f * loss : 1.f)),
                         static_cast<Uint8>(255.f * (userPause_ ? .4f : 1.f - .5f * loss * loss)),
                         static_cast<Uint8>(255.f * (userPause_ ? .4f : 1.f - .5f * loss * loss)),
                         static_cast<Uint8>(255.f * opacity)};
    };
    auto& glow = renderer_.image(environment_.foregroundPulsar ? "img/effects/pulsar_white.png"
                                                               : "img/stars/planet_sun2.png");
    const Rect source{0, 0, static_cast<float>(glow.w), static_cast<float>(glow.h)};
    renderer_.sprite(glow, {0, 0, 1000, 1000}, source, 0, tint(.1f + .9f * alpha(7)));
    auto& sun = renderer_.image(environment_.foregroundPulsar ? "img/effects/pulsar_black.png"
                                                              : "img/stars/planet_sun1.png");
    renderer_.sprite(sun,
                     {100, 109, static_cast<float>(sun.w), static_cast<float>(sun.h)},
                     {0, 0, static_cast<float>(sun.w), static_cast<float>(sun.h)},
                     132,
                     tint(.7f + .3f * alpha(5)));
    renderer_.sprite(glow, {0, 0, 1000, 1000}, source, 0, tint(.05f + .35f * alpha(10)));
}
} // namespace ftl
