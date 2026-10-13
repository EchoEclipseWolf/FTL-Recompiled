#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ftl {
namespace {
constexpr std::array<std::array<int, 6>, 4> asteroidRanges{{{5, 10, 8, 16, 5, 8},
                                                            {5, 10, 10, 20, 5, 10},
                                                            {12, 15, 16, 28, 8, 13},
                                                            {12, 15, 16, 30, 8, 11}}};
}

void Run::initializeApplicationSpace() {
    // SpaceManager416110 owns these clocks for the lifetime of the World,
    // after animation registry import. TimerHelper5a94f0's upper bound is
    // exclusive. The fixed sun trackers7/5/10 begin together at time zero.
    environment_ = {};
    environment_.flashMinTime = 16000;
    environment_.flashMaxTime = 32000;
    restartEnvironmentFlashTimer();
}

void Run::restartEnvironmentFlashTimer() {
    // TimerHelper5a94f0/5a9640 uses an exclusive upper bound and draws
    // the NEXT looping goal before any hazard damage consumes random values.
    const int range = environment_.flashMaxTime - environment_.flashMinTime;
    const int millis =
        environment_.flashMinTime +
        (range > 0 ? static_cast<int>(eventRandom_.next() % static_cast<unsigned>(range)) : 0);
    environment_.flashTime = 0;
    environment_.flashGoal = float(millis) / 1000.f;
    environment_.flareTicks = 0;
    environment_.flareGoalTicks = static_cast<int>(std::ceil(environment_.flashGoal * 60.f));
    environment_.pdsTime = 0;
    environment_.pdsGoal = environment_.flashGoal;
}

void Run::setEnvironment(const std::string& type) {
    if (type.empty()) {
        // SpaceManager::Clear41a000 clears all flags together. An ordinary
        // ModifyEnvironment5ee240 setter changes only its named active flag.
        // Clear41a000 leaves generator state, sun clocks and the shared
        // flash timer untouched. Only hazard/ship flags and flash playback
        // are reset; subsequent setters can replace the timer explicitly.
        environment_.fireActive = environment_.pulsarActive = environment_.pdsActive = false;
        environment_.asteroidActive = environment_.stormActive = false;
        environment_.pdsTarget = 2;
        environment_.flashAnimationTime = -1;
        environment_.flashTicks = 0;
        hazard_.clear();
        nebula_ = false;
        nebulaScene_.setStorm(false);
        return;
    }
    hazard_ = type;
    if (type == "nebula" || type == "ion_storm") {
        nebula_ = true;
    }
    if (type == "ion_storm") {
        environment_.stormActive = true;
        nebulaScene_.setStorm(true);
        reactorPowerDivide_ = enemy_.reactorPowerDivide = 2;
        reactorPowerCap_ = (ship_.reactor + 1) / 2;
        enemy_.reactorPowerCap = (enemy_.ship.reactor + 1) / 2;
    } else if (type == "sun") {
        environment_.fireActive = true;
        environment_.foregroundPulsar = false;
        environment_.flashMinTime = 28000;
        environment_.flashMaxTime = 34000;
        restartEnvironmentFlashTimer();
        environment_.flareSound = false;
    } else if (type == "pulsar") {
        environment_.pulsarActive = true;
        setPulsarEnvironment();
    } else if (type == "pds") {
        environment_.pdsActive = true;
        environment_.flashMinTime = 20000;
        environment_.flashMaxTime = 25000;
        restartEnvironmentFlashTimer();
        // SetPlanetaryDefense417170 uses200 for initial ambient delay;
        // UpdatePDS41b920 uses300 after a complete queue is extracted.
        environment_.pdsAmbient = 2.f + float(eventRandom_.next() % 200) / 100.f;
    } else if (type == "asteroid") {
        environment_.asteroidActive = true;
        environment_.asteroidSpawnTicks = 0;
        environment_.asteroidTarget = 0;
        environment_.asteroidState = 2 - static_cast<int>(eventRandom_.next() & 1);
        const auto* shields = system(0);
        const int band = std::clamp((shields ? shields->definition.power / 2 : 0) - 1, 0, 3);
        environment_.asteroidBand = band;
        // WorldModifyEnvironment passes defense=false, independently of
        // installed drone inventory, to StartAsteroids416f10.
        environment_.asteroidDefense = false;
        environment_.asteroidStateDraws = {};
        environment_.asteroidRateDraws = {};
        const auto& ranges = asteroidRanges[band];
        const int state = environment_.asteroidState;
        const int index = state * 2;
        environment_.asteroidStateTicks =
            60 * (ranges[index] +
                  static_cast<int>(eventRandom_.next() %
                                   static_cast<unsigned>(ranges[index + 1] - ranges[index] + 1 +
                                                         environment_.asteroidStateDraws[state])));
        ++environment_.asteroidStateDraws[state];
    }
    reconcilePower();
}

void Run::advanceEnvironment() {
    if (defeated_) {
        return;
    }
    ++environment_.sunTicks;
    const bool pdsPass = environment_.pdsActive;
    if (!environment_.pdsActive) {
        environment_.pdsSmoke.clear();
    }
    for (auto& smoke : environment_.pdsSmoke) {
        smoke[2] += Time::step;
    }
    std::erase_if(environment_.pdsSmoke, [](const auto& smoke) {
        return smoke[2] > 1.f;
    });
    if (environment_.pdsActive && environment_.pdsTarget == 1 &&
        (spaceShipOwners_.size() == 1 || (spaceShipOwners_.size() > 1 && spaceShipOwners_[1] == 1 &&
                                          enemy_.hull <= 0 && explosion_ <= 0))) {
        environment_.pdsActive = false;
    }
    if (environment_.pdsActive) {
        environment_.flashTime += Time::step;
        environment_.pdsTime = environment_.flashTime;
        environment_.flareTicks = static_cast<int>(environment_.flashTime * 60.f);
        environment_.pdsAmbient -= Time::step;
        if (environment_.pdsAmbient < 0) {
            if (environment_.pdsQueue.empty()) {
                const bool aimed = environment_.pdsTime > environment_.pdsGoal;
                if (aimed) {
                    restartEnvironmentFlashTimer();
                }
                for (const int target : spaceShipOwners_) {
                    if (environment_.pdsTarget != 2 && environment_.pdsTarget != target) {
                        continue;
                    }
                    const auto& ship = target == 0 ? ship_ : enemy_.ship;
                    if (ship.rooms.empty()) {
                        continue;
                    }
                    const bool heavy = aimed && !cloaked(target);
                    CombatProjectile p;
                    p.id = nextProjectile_++;
                    p.owner = -1;
                    p.targetShip = target;
                    p.space = 0;
                    p.weapon = "PDS_SHOT";
                    p.droneTargetable = 0;
                    p.projectileImage = "pds";
                    p.checkedShield = true;
                    p.fake = !heavy;
                    p.missed = aimed && !heavy;
                    const float shipX =
                        p.space == 0 ? playerX() + ship_.horizontalShift : enemyOriginX();
                    const float shipY = p.space == 0 ? 170.f + ship_.verticalShift : enemyOriginY();
                    if (environment_.fleet != 0) {
                        auto& image = renderer_.image("img/ship/fleet/fleet_big.png");
                        p.position = {550.f + (2.f * image.w - 100.f) / 2.f - shipX,
                                      447.f + (image.h + 25.f) / 2.f - shipY};
                    } else if (!eventPlanet_.empty()) {
                        auto& image = renderer_.image(eventPlanet_);
                        p.position = {static_cast<float>(eventPlanetX_) + image.w / 2.f - shipX,
                                      static_cast<float>(eventPlanetY_) + image.h / 2.f - shipY};
                    } else {
                        throw std::runtime_error("PDS requires a source planet or fleet origin");
                    }
                    if (heavy) {
                        const auto& room = ship.rooms[combatRandom_.next() % ship.rooms.size()];
                        p.room = room.id;
                        p.target = {static_cast<float>(room.x + room.w / 2),
                                    static_cast<float>(room.y + room.h / 2)};
                    } else {
                        p.target = {-400.f, static_cast<float>(combatRandom_.next() % 800)};
                    }
                    // Constructor draws a heading even when ComputeHeading replaces it.
                    const float heading = static_cast<float>(combatRandom_.next() % 90 + 135) *
                                          3.141592653589793f / 180.f;
                    p.explosionImage =
                        "explosion_big" + std::to_string(2 + combatRandom_.next() % 3);
                    p.entryAngle = -1;
                    p.previous = p.targetEnd = p.position;
                    const float length =
                        std::hypot(p.target.x - p.position.x, p.target.y - p.position.y);
                    p.direction = length > 0 ? SDL_FPoint{(p.target.x - p.position.x) / length,
                                                          (p.target.y - p.position.y) / length}
                                             : SDL_FPoint{1, 0};
                    if (heavy && target != 0) {
                        p.direction = {std::cos(heading), std::sin(heading)};
                    }
                    p.speed = 60.f * 16.f;
                    combatSound({heavy ? "fleetFireHeavy" : "fleetFireLight"}, p.id);
                    environment_.pdsQueue.push_back(std::move(p));
                }
            }
            if (!environment_.pdsQueue.empty()) {
                auto p = std::move(environment_.pdsQueue.back());
                environment_.pdsQueue.pop_back();
                const float x = p.position.x +
                                (p.space == 0 ? playerX() + ship_.horizontalShift : enemyOriginX());
                const float y =
                    p.position.y + (p.space == 0 ? 170.f + ship_.verticalShift : enemyOriginY());
                environment_.pdsSmoke.push_back({x, y, 0});
                projectiles_.push_back(std::move(p));
            }
            environment_.pdsAmbient =
                environment_.pdsQueue.empty()
                    ? 2.f + static_cast<float>(eventRandom_.next() % 300) / 100.f
                    : .5f;
        }
    }
    if (!pdsPass && (environment_.fireActive || environment_.pulsarActive)) {
        environment_.flashTime += Time::step;
        environment_.flareTicks = static_cast<int>(environment_.flashTime * 60.f);
        environment_.pdsTime = environment_.flashTime;
        const float remaining = environment_.flashGoal - environment_.flashTime;
        if (remaining < 1.2f && !environment_.flareSound) {
            combatSound({environment_.fireActive ? "solarFlare" : "pulsar"}, nextProjectile_++);
            environment_.flareSound = true;
        }
        if (remaining < .5f && environment_.flashAnimationTime < 0) {
            environment_.flashAnimationTime = 0;
        }
        if (environment_.flashAnimationTime >= 0) {
            environment_.flashAnimationTime += Time::step;
            if (environment_.flashAnimationTime > 1.f) {
                environment_.flashAnimationTime = -1;
            }
        }
        environment_.pulsarFlashTicks =
            environment_.flashAnimationTime < 0
                ? 60
                : static_cast<int>(environment_.flashAnimationTime * 60.f);
        environment_.flashTicks =
            environment_.flashAnimationTime < 0 ? 0 : 60 - environment_.pulsarFlashTicks;
        if (remaining < 0) {
            restartEnvironmentFlashTimer();
            environment_.flareSound = false;
            if (environment_.pulsarActive) {
                advancePulsarEnvironment();
                return;
            }
            // ShipManager::SunDamage 0x4995a0. Shields reduce fire count, never absorb flare.
            for (const int target : spaceShipOwners_) {
                // A jumping ship is left alone before any draw.
                if (target == 0 ? jumpTicks_ > 0 : enemy_.escapeJumpElapsed >= 0) {
                    continue;
                }
                const auto& ship = target == 0 ? ship_ : enemy_.ship;
                auto& systems = target == 0 ? systems_ : enemy_.systems;
                int& hull = target == 0 ? hull_ : enemy_.hull;
                const int normalShields = target == 0 ? shieldBubbles_ : enemy_.shields;
                const int superShields = target == 0 ? superShields_ : enemy_.superShields;
                const auto firstRoll = combatRandom_.next();
                if (ship.rooms.empty()) {
                    continue;
                }
                const int count = normalShields > 0 || superShields > 0
                                      ? static_cast<int>(combatRandom_.next() % 2) + 1
                                      : static_cast<int>(firstRoll % 4) + 3;
                std::vector<int> selected(ship.rooms.size());
                for (int added = 0; added < count;) {
                    const int amount =
                        added != count - 1 ? static_cast<int>(combatRandom_.next() % 2) + 1 : 1;
                    const auto roomIndex = combatRandom_.next() % ship.rooms.size();
                    const auto& room = ship.rooms[roomIndex];
                    selected[roomIndex] += amount;
                    startRoomFire(target, room.id, amount);
                    added += amount;
                }
                for (std::size_t i = 0; i < ship.rooms.size(); ++i) {
                    if (static_cast<int>(combatRandom_.next() % 100) >= 33 * selected[i]) {
                        continue;
                    }
                    const auto& room = ship.rooms[i];
                    // DamageSystem(room, Damage{iDamage 1}) runs each system's
                    // virtual AddDamage(1); Ship::ProjectileStrike takes one
                    // hull point and iLastDamage becomes 1.
                    for (auto& sys : systems) {
                        if (sys.definition.room == room.id) {
                            addSystemDamage(sys, 1, target == 1);
                        }
                    }
                    hull = std::max(0, hull - 1);
                    lastHullDamage_[std::size_t(target)] = 1;
                    pushCombatMessage(DamageMessage{target,
                                                    1,
                                                    {static_cast<float>(room.x + room.w / 2),
                                                     static_cast<float>(room.y + room.h / 2)},
                                                    tick_});
                }
            }
            reconcilePower();
            if (enemy_.present) {
                reconcilePower(true);
            }
            checkDefeat();
        }
    }
}

void Run::advanceAsteroidEnvironment() {
    if (!environment_.asteroidActive || defeated_) {
        return;
    }
    const int band = environment_.asteroidBand;
    if (--environment_.asteroidStateTicks < 0) {
        environment_.asteroidState =
            environment_.asteroidState == 0 ? 2 - static_cast<int>(eventRandom_.next() & 1) : 0;
        const auto& ranges = asteroidRanges[band];
        const int state = environment_.asteroidState;
        const int index = state * 2;
        environment_.asteroidStateTicks =
            60 * (ranges[index] +
                  static_cast<int>(eventRandom_.next() %
                                   static_cast<unsigned>(ranges[index + 1] - ranges[index] + 1 +
                                                         environment_.asteroidStateDraws[state])));
        ++environment_.asteroidStateDraws[state];
    }
    if (environment_.asteroidState != 0 && environment_.asteroidSpawnTicks <= 0) {
        constexpr std::array<std::array<int, 4>, 4> rates{{{1800, 2200, 1400, 1600},
                                                           {1000, 1800, 900, 1300},
                                                           {900, 1400, 900, 1300},
                                                           {720, 1350, 540, 950}}};
        const int offset = (environment_.asteroidState - 1) * 2;
        const int reduction =
            environment_.asteroidDefense ? std::array<int, 4>{200, 250, 100, 50}[band] : 0;
        auto& rateDraws = environment_.asteroidRateDraws[std::size_t(environment_.asteroidState)];
        const int millis =
            rates[band][offset] - reduction +
            static_cast<int>(eventRandom_.next() %
                             static_cast<unsigned>(rates[band][offset + 1] - rates[band][offset] +
                                                   1 + rateDraws));
        ++rateDraws;
        environment_.asteroidSpawnTicks = std::max(1, (millis * 60 + 999) / 1000);
        const int target = environment_.asteroidTarget;
        // AsteroidGenerator40acd0 uses the SpaceManager ship-list index,
        // not the creation owner. SetNumShips40a080 retains that cursor.
        const int count = int(spaceShipOwners_.size());
        environment_.asteroidTarget = target == count - 1 ? 0 : target + 1;
        if (count > 0) {
            // Index zero draws its side; subsequent spaces use side1.
            const int side = target == 0 ? static_cast<int>(combatRandom_.next() % 4) : 1;
            const int sideRoll = static_cast<int>(combatRandom_.next() % 800);
            SDL_FPoint origin = side == 0   ? SDL_FPoint{-400.f, static_cast<float>(sideRoll)}
                                : side == 1 ? SDL_FPoint{800.f, static_cast<float>(sideRoll / 2)}
                                : side == 2 ? SDL_FPoint{static_cast<float>(sideRoll), -400.f}
                                            : SDL_FPoint{static_cast<float>(sideRoll), 800.f};
            // Asteroid453290 chooses the ten-image variant and spin in
            // its constructor. GetNext40a090 supplies sound choices later.
            const auto image = combatRandom_.next() % 10;
            const float spin = static_cast<float>(combatRandom_.next() % 360);
            CombatProjectile asteroid;
            asteroid.id = nextProjectile_++;
            asteroid.owner = -1;
            asteroid.targetShip = asteroid.space = target;
            asteroid.weapon = "ENV_ASTEROID";
            asteroid.explosionImage = "explosion1";
            asteroid.projectileImage = "img/asteroids/asteroid" + std::to_string(image) + ".png";
            asteroid.spinAngle = spin;
            asteroid.droneTargetable = 1;
            asteroid.target = {-1, -1};
            asteroid.position = asteroid.previous = origin;
            const float dx = asteroid.target.x - origin.x;
            const float dy = asteroid.target.y - origin.y;
            const float length = std::hypot(dx, dy);
            asteroid.direction =
                length > 0 ? SDL_FPoint{dx / length, dy / length} : SDL_FPoint{1, 0};
            asteroid.speed = 35.f * 16.f;
            environment_.asteroidQueue.push_back(std::move(asteroid));
        }
    } else if (environment_.asteroidState != 0) {
        --environment_.asteroidSpawnTicks;
    }
}

void Run::advanceAsteroidScroll() {
    // Scroller::OnLoop0x579fa0 runs for all three layers even off hazard.
    constexpr std::array<float, 3> widths{546, 388, 476};
    constexpr std::array<float, 3> speeds{4, 5, 6};
    for (std::size_t layer = 0; layer < asteroidScroll_.size(); ++layer) {
        asteroidScroll_[layer] -= .5f * speeds[layer] * Time::originalStep;
        if (asteroidScroll_[layer] < 0) {
            asteroidScroll_[layer] = widths[layer];
        }
    }
}

void Run::extractEnvironmentProjectiles() {
    if (environment_.asteroidQueue.empty()) {
        return;
    }
    // 40a090 pops exactly one FIFO asteroid and assigns the two impact sounds.
    // 41c940 then finds its destination ship and calls GetRandomRoomCenter.
    auto asteroid = std::move(environment_.asteroidQueue.front());
    environment_.asteroidQueue.erase(environment_.asteroidQueue.begin());
    asteroid.asteroidShieldSound = "hitShield" + std::to_string(1 + combatRandom_.next() % 3);
    asteroid.asteroidHullSound = "hitHull" + std::to_string(1 + combatRandom_.next() % 3);
    if (asteroid.targetShip < 0 || std::size_t(asteroid.targetShip) >= spaceShipOwners_.size()) {
        return;
    }
    asteroid.targetShip = asteroid.space = spaceShipOwners_[std::size_t(asteroid.targetShip)];
    if (asteroid.targetShip == 1 && !enemy_.present) {
        return;
    }
    const auto& ship = asteroid.targetShip == 0 ? ship_ : enemy_.ship;
    if (ship.rooms.empty()) {
        return;
    }
    const auto& room = ship.rooms[combatRandom_.next() % ship.rooms.size()];
    asteroid.room = room.id;
    asteroid.target = {static_cast<float>(room.x + room.w / 2),
                       static_cast<float>(room.y + room.h / 2)};
    const float dx = asteroid.target.x - asteroid.position.x;
    const float dy = asteroid.target.y - asteroid.position.y;
    const float length = std::hypot(dx, dy);
    asteroid.direction = length > 0 ? SDL_FPoint{dx / length, dy / length} : SDL_FPoint{1, 0};
    projectiles_.push_back(std::move(asteroid));
}

void Run::renderAsteroidScroll(int layers) {
    if (!environment_.asteroidActive) {
        return;
    }
    constexpr std::array<float, 3> widths{546, 388, 476};
    constexpr std::array<float, 3> heights{309, 446, 439};
    const float health = static_cast<float>(hull_) / ship_.hull;
    const float loss = 1 - health;
    // CommandGui520a70 supplies SpaceManager bGamePaused from its manual
    // pause flag; focus-window simulation pause is a separate field.
    const bool paused = userPause_;
    const SDL_Color color{static_cast<Uint8>(255 * (paused ? .4f + .4f * loss : 1.f)),
                          static_cast<Uint8>(255 * (paused ? .4f : 1 - .5f * loss * loss)),
                          static_cast<Uint8>(255 * (paused ? .4f : 1 - .5f * loss * loss)),
                          255};
    // SpaceManager0x418c80 translates a3840x3600 tiled field(-1280,-720).
    for (int layer = std::max(0, 3 - layers); layer < 3; ++layer) {
        auto& image =
            renderer_.image("img/asteroids/asteroid_back" + std::to_string(layer + 1) + ".png");
        const float width = widths[static_cast<std::size_t>(layer)];
        const float height = heights[static_cast<std::size_t>(layer)];
        for (int x = 0; x < static_cast<int>(3840 / width + .5f) + 2; ++x) {
            const float px = static_cast<float>(x) * width +
                             asteroidScroll_[static_cast<std::size_t>(layer)] - width - 1280;
            if (px + width < 0 || px > 1280) {
                continue;
            }
            for (int y = -1; y < static_cast<int>(3600 / height + .5f) + 2; ++y) {
                const float py = static_cast<float>(y) * height - 720;
                if (py + height < 0 || py > 720) {
                    continue;
                }
                renderer_.sprite(image,
                                 {px, py, width, height},
                                 {0, 0, static_cast<float>(image.w), static_cast<float>(image.h)},
                                 0,
                                 color);
            }
        }
    }
}

void Run::renderEnvironment() {
    constexpr std::array<const char*, 8> fleetImages{{"img/ship/fleet/fleet_1_med.png",
                                                      "img/ship/fleet/fleet_2_med.png",
                                                      "img/ship/fleet/fleet_1_small.png",
                                                      "img/ship/fleet/fleet_2_small.png",
                                                      "img/ship/fleet/fleetfed_1_med.png",
                                                      "img/ship/fleet/fleetfed_2_med.png",
                                                      "img/ship/fleet/fleetfed_1_small.png",
                                                      "img/ship/fleet/fleetfed_2_small.png"}};
    for (const auto& ship : environment_.fleetShips) {
        renderer_.draw(fleetImages[std::clamp(ship[2], 0, 7)],
                       static_cast<float>(ship[0] - 150),
                       static_cast<float>(ship[1] - 150),
                       userPause_ ? .4f : 1.f);
    }
    if (environment_.fleet != 0 && environment_.pdsActive) {
        renderer_.draw("img/ship/fleet/fleet_big.png", 550, 447);
    }
    for (const auto& smoke : environment_.pdsSmoke) {
        const int frame = std::min(6, static_cast<int>(smoke[2] * 7.f));
        renderer_.sprite(renderer_.image("img/effects/fire_smoke.png"),
                         {smoke[0] - 8.5f, smoke[1] - 8.5f, 17, 17},
                         {static_cast<float>(frame * 34), 0, 34, 34});
    }
    if (!nebula_) {
        if (environment_.fireActive) {
            renderSunEnvironment();
        } else if (environment_.pulsarActive) {
            renderPulsarEnvironment();
        }
    }
}

void Run::renderEnvironmentFlash() {
    // SpaceGetFlashOpacity417860 cubes the triangular alpha of a nonlooping
    // one-second tracker. CommandGui520a70 renders it after ShipStatus.
    if (environment_.flashAnimationTime < 0) {
        return;
    }
    const float alpha = 1.f - std::abs(2.f * environment_.flashAnimationTime - 1.f);
    const Uint8 opacity = static_cast<Uint8>(std::clamp(alpha * alpha * alpha, 0.f, 1.f) * 255.f);
    renderer_.fill({0, 0, 1280, 720},
                   environment_.pulsarActive ? SDL_Color{133, 231, 237, opacity}
                                             : SDL_Color{249, 129, 33, opacity});
}

void Run::renderEnvironmentWarnings() {
    // SpaceStatus 0x4f0bf0/0x4f1450: precedence, second storm icon and shared DANGER.
    const char* icon = nullptr;
    bool storm = false;
    if (environment_.asteroidActive) {
        icon = "danger_asteroids.png";
    } else if (environment_.fireActive) {
        icon = "danger_star.png";
    } else if (environment_.pulsarActive) {
        icon = "danger_pulsar.png";
    } else if (environment_.pdsActive) {
        icon = environment_.pdsTarget == 1 ? "danger_pds_green.png" : "danger_pds.png";
    } else if (nebula_) {
        icon = "danger_nebula.png";
        storm = environment_.stormActive;
    }
    if (!icon) {
        return;
    }
    std::string image = "img/warnings/";
    image.append(icon);
    renderer_.draw(image, 660, 72);
    if (storm) {
        renderer_.draw("img/warnings/danger_storm.png", 732, 72);
    }
    const bool allies = environment_.pdsActive && environment_.pdsTarget == 1;
    const auto label =
        texts_.get(allies ? "warning_pds_allies" : "warning_environment_danger", locale_);
    auto& font = renderer_.font("fonts/HL1.font");
    const float width = std::round(renderer_.measure(font, label, 2));
    const float center = storm ? 726.f : 690.f;
    renderer_.draw(renderer_.image(allies ? "img/warnings/backglow_warning_green.png"
                                          : "img/warnings/backglow_warning_red.png"),
                   {center - width / 2, 122, width, static_cast<float>(font.height * 2)});
    text(label,
         center,
         123,
         52,
         true,
         allies ? SDL_Color{120, 255, 120, 255} : SDL_Color{253, 84, 70, 255});
    const char* imminent = nullptr;
    if (!environment_.asteroidActive && (environment_.fireActive || environment_.pulsarActive) &&
        environment_.flashGoal - environment_.flashTime < 5.f) {
        imminent = environment_.fireActive ? "warning_solar_flare" : "warning_ion_pulse";
    } else if (!environment_.asteroidActive && !environment_.fireActive &&
               !environment_.pulsarActive && environment_.pdsActive &&
               environment_.flashGoal - environment_.flashTime < 5.f) {
        imminent = "warning_pds_locked";
    }
    if (imminent) {
        const auto elapsed = animation_ - environment_.warningUiStart;
        const Uint8 flash = elapsed % 41 <= 20 ? 230 : 102;
        // SpaceStatus::OnInit 0x4efab0 places the warning at (690+135, 72-42).
        // WarningMessage::RenderWithAlpha uses DrawTextWithGlow font 59.
        renderer_.textWithGlow(texts_.get(imminent, locale_),
                               825.f,
                               30.f,
                               locale_ == "zh-Hans",
                               {253, 84, 70, 255},
                               float(flash) / 255.f,
                               true);
    }
}
} // namespace ftl
