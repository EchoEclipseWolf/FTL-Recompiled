#include "run.hpp"
#include <algorithm>
#include <cmath>

namespace ftl {
void Run::setPulsarEnvironment() {
    environment_.foregroundPulsar = true;
    environment_.flashMinTime = 11000;
    environment_.flashMaxTime = 18000;
    restartEnvironmentFlashTimer();
    environment_.flareSound = false;
    eventBackground_ = "img/stars/bg_dullstars.png";
}

void Run::advancePulsarEnvironment() {
    for (const int target : spaceShipOwners_) {
        // ShipManager::PulsarDamage 0x498c40 leaves a jumping ship alone before
        // the ION_ARMOR draw.
        if (target == 0 ? jumpTicks_ > 0 : enemy_.escapeJumpElapsed >= 0) {
            continue;
        }
        auto& systems = target == 0 ? systems_ : enemy_.systems;
        auto& ship = target == 0 ? ship_ : enemy_.ship;
        auto& super = target == 0 ? superShields_ : enemy_.superShields;
        const auto shieldPosition = std::ranges::find_if(systems, [](const RunSystem& candidate) {
            return candidate.definition.id == 0;
        });
        RunSystem* shields = shieldPosition == systems.end() ? nullptr : &*shieldPosition;
        float resistance = 0;
        for (const auto& id : ship.augments) {
            if (id == "ION_ARMOR") {
                resistance += catalog_.equipment(id).value;
            }
        }
        const bool resisted = resistance > 0 && static_cast<int>(eventRandom_.next() % 10) + 1 <=
                                                    static_cast<int>(resistance * 10);
        const auto center = [&](const RunSystem& sys) {
            const auto roomPosition = std::ranges::find_if(ship.rooms, [&](const auto& candidate) {
                return candidate.id == sys.definition.room;
            });
            if (roomPosition == ship.rooms.end()) {
                throw std::runtime_error("Pulsar system room missing");
            }
            const auto& room = *roomPosition;
            return SDL_FPoint{static_cast<float>(room.x + room.w / 2),
                              static_cast<float>(room.y + room.h / 2)};
        };
        const auto effective = [](const RunSystem& sys) {
            return std::max(0, std::min(sys.power, sys.health));
        };
        const auto ion = [&](RunSystem& sys) {
            const int amount = effective(sys) / 2 + 1;
            applySystemIon(sys, amount, target == 1);
            pushCombatMessage(DamageMessage{target, amount, center(sys), tick_, false, true});
        };
        int last = -1;
        int count = 2;
        if (!resisted && shields) {
            if (super > 0) {
                const int amount = std::min(super, 3 + static_cast<int>(eventRandom_.next() % 2));
                super -= amount;
                // Shields::PulsarDamage 0x60ce80 posts the green number in the
                // shield's own list at the top of its base ellipse.
                int left = ship.rooms.front().x;
                int top = ship.rooms.front().y;
                int right = left;
                int bottom = top;
                for (const auto& room : ship.rooms) {
                    left = std::min(left, room.x);
                    top = std::min(top, room.y);
                    right = std::max(right, room.x + room.w);
                    bottom = std::max(bottom, room.y + room.h);
                }
                const SDL_FPoint shieldTop{float(left + right) / 2.f + float(ship.ellipse[2]),
                                           float(top + bottom) / 2.f + float(ship.ellipse[3]) +
                                               (target == 1 ? 110.f : 0.f) -
                                               float(ship.ellipse[1])};
                pushCombatMessage(
                    DamageMessage{target, amount, shieldTop, tick_, true, false, false, 0, true});
                continue;
            }
            if (effective(*shields) > 0) {
                ion(*shields);
                last = 0;
                count = 1;
            }
        }
        if (resisted && shields && (effective(*shields) > 0 || super > 0)) {
            pushCombatMessage(ResistMessage{target, center(*shields), tick_});
            // ShipManager::PulsarDamage 0x498c40 returns after the shield
            // resistance message. Only an unpowered/no-shield ship chooses
            // two system targets after resisting the pulse.
            continue;
        }
        if (systems.empty()) {
            continue;
        }
        // Original chooses system vector entries until consecutive IDs differ.
        // Native installed-system vector excludes the original null entries.
        for (int n = 0; n < count; ++n) {
            if (systems.size() == 1 && systems.front().definition.id == last) {
                break;
            }
            RunSystem* chosen = nullptr;
            do {
                chosen = &systems[eventRandom_.next() % systems.size()];
            } while (chosen->definition.id == last);
            last = chosen->definition.id;
            if (resisted) {
                pushCombatMessage(ResistMessage{target, center(*chosen), tick_});
            } else {
                ion(*chosen);
            }
        }
    }
    reconcilePower();
    if (enemy_.present) {
        reconcilePower(true);
    }
}

void Run::renderPulsarEnvironment() {
    const float progress =
        std::clamp(static_cast<float>(environment_.pulsarFlashTicks) / 60.f, 0.f, 1.f);
    const float loss = 1.f - static_cast<float>(hull_) / ship_.hull;
    const auto tint = [&](float opacity) {
        return SDL_Color{static_cast<Uint8>(255.f * (userPause_ ? .4f + .4f * loss : 1.f)),
                         static_cast<Uint8>(255.f * (userPause_ ? .4f : 1.f - .5f * loss * loss)),
                         static_cast<Uint8>(255.f * (userPause_ ? .4f : 1.f - .5f * loss * loss)),
                         static_cast<Uint8>(255.f * opacity)};
    };
    const auto ring = [&](const std::string& name) {
        auto& image = renderer_.image(name);
        const float scale = progress * 3;
        const float w = static_cast<float>(image.w) * scale;
        const float h = static_cast<float>(image.h) * scale;
        renderer_.sprite(image,
                         {463 - float(image.w / 2) * scale, 203 - float(image.h / 2) * scale, w, h},
                         {0, 0, float(image.w), float(image.h)},
                         0,
                         tint(1 - progress));
    };
    ring("img/effects/pulsar_backL.png");
    auto& core = renderer_.image(environment_.foregroundPulsar ? "img/effects/pulsar_black.png"
                                                               : "img/stars/planet_sun1.png");
    renderer_.sprite(core,
                     {463 - float(core.w / 2),
                      203 - float(core.h / 2),
                      static_cast<float>(core.w),
                      static_cast<float>(core.h)},
                     {0, 0, float(core.w), float(core.h)},
                     0,
                     tint(1));
    const float time = static_cast<float>(environment_.sunTicks % 421) / 60.f;
    const float glow = (3.5f - std::abs(time - 3.5f)) / 3.5f;
    auto& white = renderer_.image(environment_.foregroundPulsar ? "img/effects/pulsar_white.png"
                                                                : "img/stars/planet_sun2.png");
    renderer_.sprite(
        white,
        {463 - float(core.w / 2), 203 - float(core.h / 2), float(white.w), float(white.h)},
        {0, 0, float(white.w), float(white.h)},
        0,
        tint(glow));
    ring("img/effects/pulsar_frontL.png");
}
} // namespace ftl
