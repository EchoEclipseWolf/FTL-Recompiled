#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ftl {
// Original CFPS units: 16 factor units per game second at speedLevel zero.
// Fixed native updates deliberately avoid the original per-frame .5 clamp.
struct Time {
    static constexpr double tickHz = 60;
    static constexpr double stepSeconds = 1 / tickHz;
    static constexpr float step = float(stepSeconds);
    static constexpr float originalStep = 16 * step;

    static constexpr double seconds(double ticks) {
        return ticks / tickHz;
    }

    // AnimationTracker59ade0 clamps the endpoint, then restarts a looping
    // animation on the following update. A modulo would lose that frame.
    static float loopAnimation(float elapsed, float duration, float delta = step) {
        if (elapsed == duration) {
            elapsed = 0;
        }
        return std::min(duration, elapsed + delta);
    }

    static double originalFactor(double seconds, int level = 0) {
        return std::clamp(seconds * (16 + 7.0 * level), 0.0, .5);
    }
};

struct TimeState {
    double speed = 1;
    double pendingTicks = 0;
    double wallSeconds = 0;
    std::uint64_t simulationTicks = 0;
};

// Explicit owned state; static operations avoid a hidden process-wide clock.
// The callback advances exactly one original-rate logic tick. Backlog is retained.
class TimeController {
public:
    static void speed(TimeState& state, double value) {
        if (!std::isfinite(value) || value < 0 || value > 100) {
            throw std::runtime_error("simulation_speed_requires_0_to_100");
        }
        state.speed = value;
    }

    template <typename Advance>
    static void advance(TimeState& state, double wallSeconds, Advance advanceTick) {
        if (!std::isfinite(wallSeconds) || wallSeconds < 0 || wallSeconds > 3600) {
            throw std::runtime_error("wall_seconds_requires_0_to_3600");
        }
        state.wallSeconds += wallSeconds;
        state.pendingTicks += wallSeconds * state.speed * Time::tickHz;
        // Bounded work keeps input responsive after a stall; never discard debt.
        std::uint32_t count = 0;
        while (state.pendingTicks + 1e-9 >= 1 && count < 600) {
            advanceTick();
            state.pendingTicks = std::max(0.0, state.pendingTicks - 1);
            ++state.simulationTicks;
            ++count;
        }
    }

    static float alpha(const TimeState& state) {
        return float(std::clamp(state.pendingTicks, 0.0, 1.0));
    }

    static double framePeriod(double fps) {
        if (!std::isfinite(fps) || fps < 0 || fps > 10000) {
            throw std::runtime_error("render_fps_requires_0_to_10000");
        }
        return fps == 0 ? 0 : 1 / fps;
    }
};
} // namespace ftl
