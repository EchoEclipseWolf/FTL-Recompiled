#include "run.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace ftl {
namespace {
float superShieldDroneCooldown(int bubbles) {
    // SuperShieldDrone::GetWeaponCooldown 0x41e8d0 indexes the original
    // SUPER_SHIELD_TIME table by the protected ship's current shield count.
    constexpr std::array<float, 5> time{8.f, 10.f, 13.f, 16.f, 20.f};
    return time[std::size_t(std::clamp(bubbles, 0, 4))];
}

SDL_FPoint defenseOrbitPoint(const ShipDefinition& ship, float angle, bool enemy = true) {
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
    const float centerX = float(left + right) / 2.f + ship.ellipse[2];
    // CombatControl::AddEnemyShip 0x5638a0 shifts the hostile shield/space
    // ellipse down 110 pixels inside the target panel.
    const float centerY = float(top + bottom) / 2.f + ship.ellipse[3] + (enemy ? 110.f : 0.f);
    const float radians = angle * std::numbers::pi_v<float> / 180.f;
    return {centerX + float(ship.ellipse[0]) * std::cos(radians),
            centerY + float(ship.ellipse[1]) * std::sin(radians)};
}

SDL_FPoint center(const RoomDefinition& room) {
    // ShipGraph::GetRoomCenter 0x45b020 halves the integer rectangle.
    return {float(room.x + room.w / 2), float(room.y + room.h / 2)};
}

// Globals::GetNextPoint 0x5a88b0: one `speed` step along the line toward
// `dest`, or `dest` itself once the squared distance is below speed squared.
SDL_FPoint nextPoint(SDL_FPoint current, float speed, SDL_FPoint dest) {
    const float dx = current.x - dest.x;
    const float dy = current.y - dest.y;
    if (speed * speed > dx * dx + dy * dy) {
        return dest;
    }
    float stepX = speed;
    float stepY = 0;
    const float sign = current.y > dest.y ? -1.f : 1.f;
    if (current.y != dest.y) {
        if (current.x == dest.x) {
            stepX = 0;
            stepY = speed * sign;
        } else {
            const float slope = std::fabs(dy / dx);
            stepX = std::sqrt(speed * speed / (slope * slope + 1.f));
            stepY = slope * stepX * sign;
        }
    }
    return {current.x + stepX * (current.x <= dest.x ? 1.f : -1.f), current.y + stepY};
}

// SpaceDrone::UpdateAimingAngle 0x4261b0. Without a target (and no forced
// angle) the aim is kept. A non-negative percentage interpolates from
// PickTarget's lastAimingAngle toward the angle from `location` to the
// target; a negative one turns 30 degrees per CFPS unit and stops on it.
float updateAimingAngle(CombatDrone& drone,
                        bool hasTarget,
                        SDL_FPoint target,
                        SDL_FPoint location,
                        float percentage,
                        float forceDesired = -1.f) {
    if (!hasTarget && forceDesired < 0) {
        return drone.aimAngle;
    }
    float desired = forceDesired;
    if (forceDesired <= 0) {
        const float dx = target.x - location.x;
        const float dy = target.y - location.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        const float x = length > 0 ? dx / length : 0.f;
        const float y = length > 0 ? dy / length : 0.f;
        desired = std::acos(x) * 180.f / std::numbers::pi_v<float>;
        if (y < 0) {
            desired = -desired;
        }
    }
    drone.desiredAimAngle = desired;
    if (percentage < 0) {
        if (desired < 0) {
            desired += 360.f;
            drone.desiredAimAngle = desired;
        }
        const float aim = drone.aimAngle;
        const bool up = (aim <= desired || aim - desired >= 180.f) &&
                        (desired <= aim || desired - aim <= 180.f);
        float next = aim + Time::originalStep * (up ? 30.f : -30.f);
        if (next > 360.f) {
            next -= 360.f;
        } else if (next < 0) {
            next += 360.f;
        }
        drone.aimAngle = next;
        if ((next > desired && up && next - desired < 180.f) ||
            (desired > next && !up && desired - next < 180.f)) {
            drone.aimAngle = desired;
        }
        return desired;
    }
    if (std::fabs(drone.aimAngle - desired) > 180.f) {
        desired += desired < 0 ? 360.f : -360.f;
        drone.desiredAimAngle = desired;
    }
    drone.aimAngle = percentage * (desired - drone.lastAimingAngle) + drone.lastAimingAngle;
    return desired;
}

// CombatDrone::PickDestination 0x421260 (ShipRepairDrone inherits it): an
// angle at least 90 degrees from the previous one, 1.15 radii out on the
// movement target's shield ellipse.
void pickOrbitDestination(CombatDrone& drone,
                          const ShipDefinition& ship,
                          float centerOffsetY,
                          RandomStream& random) {
    int angle = 0;
    do {
        angle = int(random.next() % 360);
    } while (std::abs(float(angle) - drone.orbitAngle) < 90.f);
    drone.orbitAngle = float(angle);
    drone.lastDestination = drone.destination;
    drone.oldHeading = drone.heading;
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
    const float radians = drone.orbitAngle * std::numbers::pi_v<float> / 180.f;
    drone.destination = {float(left + right) / 2.f + ship.ellipse[2] +
                             1.15f * float(ship.ellipse[0]) * std::cos(radians),
                         float(top + bottom) / 2.f + ship.ellipse[3] + centerOffsetY +
                             1.15f * float(ship.ellipse[1]) * std::sin(radians)};
    drone.heading = std::atan2(drone.destination.y - drone.lastDestination.y,
                               drone.destination.x - drone.lastDestination.x) *
                    180.f / std::numbers::pi_v<float>;
    drone.orbitInitialized = true;
}

bool insideShield(const ShipDefinition& ship, SDL_FPoint point, bool enemy) {
    int left = 10000;
    int top = 10000;
    int right = 0;
    int bottom = 0;
    for (const auto& room : ship.rooms) {
        left = std::min(left, room.x);
        top = std::min(top, room.y);
        right = std::max(right, room.x + room.w);
        bottom = std::max(bottom, room.y + room.h);
    }
    const float x =
        (point.x - float(left + right) / 2.f - float(ship.ellipse[2])) / float(ship.ellipse[0]);
    const float y =
        (point.y - float(top + bottom) / 2.f - float(ship.ellipse[3]) - (enemy ? 110.f : 0.f)) /
        float(ship.ellipse[1]);
    return x * x + y * y < 1.f;
}

RunSystem* findSystem(std::vector<RunSystem>& systems, int id) {
    for (auto& s : systems) {
        if (s.definition.id == id) {
            return &s;
        }
    }
    return nullptr;
}

// HackingDrone::PickDestination 0x44dbf0: four discarded RandomSidePoint
// draws, then sides chosen at random without replacement. Each side scans grid
// axes across the ship box (ShipGraph::ComputeCenter 0x45aca0) from its top or
// left; HackingDrone::CheckAxis 0x44daa0 samples the full line in 50 steps for
// the first point inside a room (GetSelectedRoom with walls, room order). The
// first axis whose sample lies in the wanted room wins, otherwise the sample
// nearest that room's centre. The drone then flies the whole line; its landing
// probe stops it at the hull.
std::pair<SDL_FPoint, SDL_FPoint>
hackingApproach(const ShipDefinition& ship, int targetRoom, RandomStream& random) {
    const auto target = std::ranges::find_if(ship.rooms, [targetRoom](const auto& room) {
        return room.id == targetRoom;
    });
    if (target == ship.rooms.end()) {
        return {};
    }
    const SDL_FPoint centre{float(target->x + target->w / 2), float(target->y + target->h / 2)};
    int left = 1280;
    int top = 720;
    int right = -1;
    int bottom = -1;
    for (const auto& room : ship.rooms) {
        left = std::min(left, room.x);
        top = std::min(top, room.y);
        right = std::max(right, room.x + room.w);
        bottom = std::max(bottom, room.y + room.h);
    }
    const auto selected = [&](float px, float py) {
        const int x = int(px);
        const int y = int(py);
        for (const auto& room : ship.rooms) {
            if (room.x <= x && x <= room.x + room.w && room.y <= y && y <= room.y + room.h) {
                return room.id;
            }
        }
        return -1;
    };
    constexpr float miss = 2147483600.f;
    const auto checkAxis = [&](SDL_FPoint from, SDL_FPoint to) {
        const float stepX = (to.x - from.x) / 50.f;
        const float stepY = (to.y - from.y) / 50.f;
        SDL_FPoint point = from;
        for (int sample = 0; sample < 50; ++sample) {
            if (selected(point.x, point.y) != -1) {
                return point;
            }
            point.x += stepX;
            point.y += stepY;
        }
        return SDL_FPoint{miss, miss};
    };
    std::vector<int> sides{2, 3, 0, 1};
    for (int i = 0; i < 4; ++i) {
        (void)random.next();
    }
    float best = miss;
    std::pair<SDL_FPoint, SDL_FPoint> result{};
    while (!sides.empty()) {
        const auto chosen = random.next() % std::uint32_t(sides.size());
        const int side = sides[chosen];
        sides.erase(sides.begin() + std::ptrdiff_t(chosen));
        const bool horizontal = side < 2;
        const int begin = horizontal ? top : left;
        const int extent = horizontal ? bottom - top : right - left;
        for (int axis = begin + 17; axis < begin + extent; axis += 35) {
            const float near = side == 0 || side == 2 ? -400.f : 800.f;
            const float far = side == 0 || side == 2 ? 800.f : -400.f;
            const SDL_FPoint start =
                horizontal ? SDL_FPoint{near, float(axis)} : SDL_FPoint{float(axis), near};
            const SDL_FPoint finish =
                horizontal ? SDL_FPoint{far, float(axis)} : SDL_FPoint{float(axis), far};
            const auto sample = checkAxis(start, finish);
            if (selected(sample.x, sample.y) == targetRoom) {
                return {start, finish};
            }
            const float distance = std::hypot(sample.x - centre.x, sample.y - centre.y);
            if (best > distance) {
                best = distance;
                result = {start, finish};
            }
        }
    }
    return result;
}

void advancePoweredDownDrone(CombatDrone& drone,
                             const BlueprintCatalog& catalog,
                             RandomStream& random,
                             const ShipDefinition* movementShip) {
    // SpaceDrone426670: retained stun sparks, angular spin and inertial drift.
    const bool stunned = drone.ionStun > 0 || drone.controllerHackLevel > 1;
    if (stunned) {
        drone.hackAngle += 30.f * Time::originalStep;
        if (drone.hackAngle > 360.f) {
            drone.hackAngle = 0;
        }
        const auto& sparks = catalog.animation("stun_spark_big");
        if (drone.powered && sparks.usable()) {
            if (drone.spaceHackSparksElapsed < 0) {
                drone.spaceHackSparksElapsed = 0;
            }
            const float elapsed = drone.spaceHackSparksElapsed + Time::step;
            if (elapsed > sparks.duration) {
                drone.spaceHackSparksMirror = (random.next() & 1) != 0;
                drone.spaceHackSparksElapsed = 0;
            } else {
                drone.spaceHackSparksElapsed = elapsed;
            }
        } else {
            drone.spaceHackSparksElapsed = -1;
        }
    } else {
        drone.spaceHackSparksElapsed = -1;
    }
    if ((!drone.deployed && drone.pause <= 0) || drone.pause > 0) {
        if (drone.pause > 0) {
            drone.velocity = {};
        }
        return;
    }
    drone.position.x += drone.velocity.x;
    drone.position.y += drone.velocity.y;
    drone.velocity.x -= .1f * Time::originalStep * drone.velocity.x;
    drone.velocity.y -= .1f * Time::originalStep * drone.velocity.y;
    if (std::hypot(drone.velocity.x, drone.velocity.y) < Time::originalStep) {
        drone.velocity = {};
    }
    if (movementShip && !movementShip->rooms.empty()) {
        const auto first = defenseOrbitPoint(*movementShip, 0.f, false);
        const auto opposite = defenseOrbitPoint(*movementShip, 180.f, false);
        const SDL_FPoint centre{(first.x + opposite.x) / 2.f, (first.y + opposite.y) / 2.f};
        if (std::hypot(drone.position.x - centre.x, drone.position.y - centre.y) > 400.f) {
            drone.velocity = {};
        }
    }
}

void advanceRepairWaves(CombatDrone& drone) {
    // RenderDrone4288b0 mutates cosmetics; the accepted pure-render model
    // projects that update into the source Space pass without consuming RNG.
    if (drone.pause <= 0) {
        return;
    }
    if (drone.repairWavePhases.empty()) {
        drone.repairWavePhases = {.75f, .25f, 0.f};
    }
    for (std::size_t index = 0; index < drone.repairWavePhases.size();) {
        drone.repairWavePhases[index] += .6f * Time::step;
        if (drone.repairWavePhases[index] > 1.f) {
            drone.repairWavePhases.erase(drone.repairWavePhases.begin() + std::ptrdiff_t(index));
            continue;
        }
        if (index + 1 == drone.repairWavePhases.size() && drone.repairWavePhases[index] > .25f) {
            drone.repairWavePhases.push_back(0);
        }
        ++index;
    }
}

} // namespace

void initializeDroneConstructor(CombatDrone& drone,
                                const BlueprintCatalog& catalog,
                                const std::string& locale,
                                RandomStream& random) {
    const auto& blueprint = catalog.equipment(drone.blueprint);
    const bool space = blueprint.type == "COMBAT" || blueprint.type == "DEFENSE" ||
                       blueprint.type == "BOARDER" || blueprint.type == "SHIELD" ||
                       blueprint.type == "SHIP_REPAIR" || blueprint.type == "HACKING";
    if (space) {
        // SpaceDrone422d00 chooses angle, then resolves explosion_random.
        drone.orbitAngle = float(random.next() % 360);
        drone.constructorExplosionImage = "explosion_big" + std::to_string(2 + random.next() % 3);
    }
    if (blueprint.type == "HACKING") {
        drone.constructorExplosionImage.clear();
    }
    if (blueprint.type == "SHIP_REPAIR") {
        // Derived ctor4246c0 draws lifespan; SetDeployed421160 draws again.
        drone.lifespan = int(random.next() % 3) + 3;
    }
    if (blueprint.type != "REPAIR" && blueprint.type != "BATTLE" && blueprint.type != "BOARDER") {
        return;
    }
    // Factory45d5d0/43b820 supply animation before CrewDrone438130's
    // GetCrewBlueprint name draw; Battle45db80/ordinary boarder45dee0 after.
    const bool supplied = blueprint.type == "REPAIR" || blueprint.id == "BOARDER_ION";
    const auto animation = [&] {
        drone.shootGoal = float(random.next() % 300 + 1000) / 1000.f;
        drone.shootElapsed = drone.pendingDamage = 0;
    };
    if (supplied) {
        animation();
    }
    (void)catalog.unusedCrewName(locale, {}, [&] {
        return random.next();
    });
    // Battle/repair authored dummy crew blueprints have no palette layers.
    if (!supplied) {
        animation();
    }
    drone.health = blueprint.type == "REPAIR"      ? 25.f
                   : blueprint.id == "BOARDER_ION" ? 125.f
                                                   : 150.f;
    // CrewMember47a400 starts with x=y=0 and room/slot=-1; the derived
    // CrewDrone438130 constructor explicitly sets its inherited bDead=true.
    // CreateCrewDrone49bd20 appends it without placing or deploying it.
    drone.crewDead = true;
}

void initializeDroneLocation(CombatDrone& drone,
                             const ShipDefinition& ship,
                             int owner,
                             const BlueprintCatalog& catalog,
                             RandomStream& random) {
    const auto& blueprint = catalog.equipment(drone.blueprint);
    if (blueprint.type == "BOARDER") {
        const auto controller = std::ranges::find_if(ship.systems, [](const auto& sys) {
            return sys.id == 4;
        });
        if (controller != ship.systems.end()) {
            const auto room = std::ranges::find(ship.rooms, controller->room, &RoomDefinition::id);
            if (room != ship.rooms.end()) {
                // CreateSpaceDrone4a5db0 calls SetCurrentLocation41da60 at
                // birth; later SetDeployed420060 returns to this base.
                drone.position = drone.basePosition = center(*room);
            }
        }
        return;
    }
    if (ship.rooms.empty() || (blueprint.type != "DEFENSE" && blueprint.type != "SHIELD" &&
                               blueprint.type != "SHIP_REPAIR")) {
        return;
    }
    drone.space = owner;
    const auto pick = [&]() {
        if (blueprint.type == "SHIP_REPAIR") {
            int angle = 0;
            do {
                angle = int(random.next() % 360);
            } while (std::abs(float(angle) - drone.orbitAngle) < 90.f);
            drone.orbitAngle = float(angle);
            const auto point = defenseOrbitPoint(ship, drone.orbitAngle, false);
            const auto origin = defenseOrbitPoint(ship, drone.orbitAngle + 180.f, false);
            const SDL_FPoint midpoint{(point.x + origin.x) / 2.f, (point.y + origin.y) / 2.f};
            return SDL_FPoint{midpoint.x + 1.15f * (point.x - midpoint.x),
                              midpoint.y + 1.15f * (point.y - midpoint.y)};
        }
        const auto point = defenseOrbitPoint(ship, drone.orbitAngle, false);
        drone.orbitAngle += 1.f;
        return point;
    };
    // SetMovementTarget421870/4217f0/4218f0 picks twice at BIRTH, even
    // undeployed. The enemy UI's later ellipse offset is not present here.
    drone.position = pick();
    drone.destination = pick();
    drone.orbitInitialized = true;
    drone.pause = 0;
    if (blueprint.type == "SHIELD") {
        drone.cooldown = 8.f;
    }
}

void Run::initializeCrewDroneAnimation(CombatDrone& drone,
                                       RandomStream& random,
                                       const ShipDefinition* owningShip,
                                       int owner) {
    initializeDroneConstructor(drone, catalog_, locale_, random);
    initializeDroneLocation(drone, owningShip ? *owningShip : ship_, owner, catalog_, random);
    assignSpaceDroneId(drone);
    assignCrewDroneId(drone);
    const auto& type = catalog_.equipment(drone.blueprint).type;
    if (type == "REPAIR" || type == "BATTLE") {
        assignPhysicalCrewOrder(drone);
        drone.crewPhysicalShip = owner;
        drone.crewRoom = drone.crewSlot = -1;
        drone.crewPosition = {};
    }
    crewNameHistory_ = catalog_.currentCrewNames();
}

void Run::resetCrewDroneDeployment(CombatDrone& drone) {
    // DroneSystem::Jump611ae0 retains the object. Apply the player virtual
    // SetDeployed(false) falling edge without reconstructing its factory,
    // target, movement, constructor timers or active explosion.
    undeployDrone(drone);
    drone.powered = drone.repowerPending = false;
}

void Run::undeployDrone(CombatDrone& drone) {
    // The virtual Drone::SetDeployed(false) of each drone class; power is the
    // caller's (DroneSystem::DePowerDrone) concern.
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    if (blueprint.type == "REPAIR" || blueprint.type == "BATTLE" || blueprint.type == "BOARDER") {
        drone.crewDead = true;
    }
    if (blueprint.type != "REPAIR" && blueprint.type != "BATTLE" && blueprint.type != "BOARDER") {
        drone.ionStun = 0;
    }
    if (blueprint.type == "DEFENSE" || blueprint.type == "SHIELD") {
        const auto enemies = spaceDronesForOwner(1);
        const bool enemy = std::ranges::find(enemies, &drone) != enemies.end();
        drone.cooldown = blueprint.type == "SHIELD"
                             ? superShieldDroneCooldown(enemy ? enemy_.superShields : superShields_)
                             : blueprint.cooldown;
    }
    if (blueprint.type == "SHIELD") {
        drone.shieldGlow = -1.f;
    }
    if (blueprint.type == "BOARDER" && !drone.deployed) {
        const auto enemies = spaceDronesForOwner(1);
        drone.space = std::ranges::find(enemies, &drone) != enemies.end() ? 1 : 0;
        drone.position = drone.basePosition;
        drone.destination = {800.f, drone.position.y};
        drone.delivered = false;
    }
    drone.deployed = false;
}

bool Run::droneCanBeDeployed(const CombatDrone& drone, int owner) const {
    const auto& type = catalog_.equipment(drone.blueprint).type;
    if (type == "COMBAT") {
        // CombatDrone::CanBeDeployed 0x41d900 is HasTarget 0x41d910: a weapon
        // target whose Targetable::hostile is set. ShipManager::OnInit marks
        // every ship hostile; only the enemy's flag later drops.
        return owner == 0 ? enemy_.present && enemy_.playerTarget && hostile_ : enemy_.present;
    }
    if (type == "BOARDER" || type == "HACKING") {
        // BoarderPodDrone/HackingDrone::CanBeDeployed 0x41e860/0x44bbf0: a
        // movement target, and for the enemy no player super shield.
        return owner == 0 ? enemy_.present && enemy_.playerTarget
                          : enemy_.present && superShields_ <= 0;
    }
    return true;
}

bool Run::droneHasMovementTarget(const CombatDrone& drone, int owner) const {
    // ShipManager::SetTarget 0x49c230 points offensive drones at the target
    // ship; defensive and repair drones keep their own ship (their
    // SetMovementTarget overrides accept only the first target).
    const auto& type = catalog_.equipment(drone.blueprint).type;
    if (type == "COMBAT" || type == "BOARDER" || type == "HACKING") {
        return owner == 0 ? enemy_.present && enemy_.playerTarget : enemy_.present;
    }
    return true;
}

bool Run::depowerDrone(CombatDrone& drone, RunSystem& controller, int owner) {
    // DroneSystem::DePowerDrone 0x611630: one RawDecreasePower (battery
    // first) per required bar. A bar that cannot be removed leaves it powered.
    int remaining = std::max(0, catalog_.equipment(drone.blueprint).power - drone.bonusPower);
    while (remaining > 0) {
        if (controller.power <= controller.bonusPower) {
            return false;
        }
        releaseBatteryPower(controller, 1, owner == 1);
        --controller.power;
        --remaining;
    }
    drone.powered = false;
    return true;
}

void Run::retargetSpaceDrones(int owner) {
    // ShipManager::SetTarget 0x49c230: SpaceDrone::SetMovementTarget 0x41d940
    // picks a destination, starts there and picks again. Only combat drones
    // take a new target; the other classes keep their first one. A combat
    // drone without a hostile target also adopts it as weapon target.
    const auto& target = owner == 0 ? enemy_.ship : ship_;
    // ShipManager::SetTarget 0x49c230 also sets every space drone's
    // bDisrupted from target->HasEquipment("DEFENSE_SCRAMBLER"); only
    // DefenseDrone::HasTarget 0x41e810 / PickTarget 0x422540 read it.
    const bool scrambled =
        std::ranges::find(target.augments, "DEFENSE_SCRAMBLER") != target.augments.end();
    for (auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
        drone.defenseDisrupted = scrambled;
    }
    if (target.rooms.empty()) {
        return;
    }
    for (auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
        if (catalog_.equipment(drone.blueprint).type != "COMBAT" || drone.bossSurge) {
            continue;
        }
        pickOrbitDestination(drone, target, 0.f, combatRandom_);
        drone.position = drone.destination;
        pickOrbitDestination(drone, target, 0.f, combatRandom_);
        drone.hasAimTarget = false;
    }
}

void Run::detachInventoryDrone(int index) {
    if (index < 0 || index >= int(combatDrones_.size())) {
        return;
    }
    auto& drone = combatDrones_[std::size_t(index)];
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    // RemoveDrone6127e0 uses DePowerDrone611630(userDriven=false), then
    // SetDeployed(false) twice. Ion/hacking locks do not gate removal.
    if (drone.powered) {
        if (auto* controller = findSystem(systems_, 4)) {
            const int required = std::max(0, blueprint.power - drone.bonusPower);
            const int released =
                std::min(required, std::max(0, controller->power - controller->bonusPower));
            releaseBatteryPower(*controller, released);
            controller->power -= released;
        }
        drone.powered = false;
    }
    drone.repowerPending = false;
    const bool crewBody =
        blueprint.type == "REPAIR" || blueprint.type == "BATTLE" || blueprint.type == "BOARDER";
    if (crewBody) {
        // CrewDrone437770 empties its reservation and sets bDead, retaining
        // physical room/position, health, constructor timers and shot effects.
        drone.crewDead = true;
    }
    if (blueprint.type != "REPAIR" && blueprint.type != "BATTLE") {
        // SpaceDrone41ebe0 clears ion stun on every falling edge.
        drone.ionStun = 0;
    }
    if (blueprint.type == "DEFENSE" || blueprint.type == "SHIELD") {
        // DefenseDrone421a10 resets its weapon cooldown even while disabled.
        drone.cooldown = blueprint.type == "SHIELD" ? superShieldDroneCooldown(superShields_)
                                                    : blueprint.cooldown;
    }
    if (blueprint.type == "SHIELD") {
        drone.shieldGlow = -1.f;
    }
    drone.deployed = false;
    if (blueprint.type == "BOARDER") {
        // BoarderPod420060's second false call resets the carrier to its own
        // space. The embedded actor remains the same object.
        drone.delivered = false;
        drone.space = 0;
        drone.orbitInitialized = false;
    }
}

bool Run::powerDrone(CombatDrone& active, int owner) {
    if (active.powered) {
        return true;
    }
    auto& systems = owner == 0 ? systems_ : enemy_.systems;
    auto* drones = findSystem(systems, 4);
    if (!drones || active.rebuildTimer > 0) {
        return false;
    }
    const auto& blueprint = catalog_.equipment(active.blueprint);
    // DroneSystem::PowerDrone 0x612b10 refuses an undeployed drone whose
    // virtual CanBeDeployed fails.
    const bool canDeploy = droneCanBeDeployed(active, owner);
    auto& parts = owner == 0 ? parts_ : enemy_.droneParts;
    if ((!canDeploy && !active.deployed) || (!active.deployed && parts <= 0)) {
        return false;
    }
    const int required = std::max(0, blueprint.power - active.bonusPower);
    int available = reactorCapacity(owner == 1) + batteryPower(owner == 1);
    for (const auto& allocated : systems) {
        const int id = allocated.definition.id;
        if ((id < 6 || id > 8) && id != 12) {
            available -= allocated.power - allocated.bonusPower;
        }
    }
    if (drones->power + required > systemCapacity(*drones) || available < required) {
        return false;
    }
    // RawIncreasePower617180 has no ion/hack lock predicate. Human Control
    // checks it before PowerDrone; free bonus and forced activation use this
    // internal source recipe without mutating the human warning/intent state.
    drones->power += required;
    assignBatteryPower(*drones, required, owner == 1);
    active.powered = true;
    if (owner == 0 && (blueprint.type == "COMBAT" || blueprint.type == "BOARDER")) {
        progressionRun_.tutorial = tutorialOwned_;
        progressionRun_.flag("offensive_drone");
        progressionRun_.flag("combat_offensive_drone");
    }
    active.repowerPending = false;
    // A deployed defense drone restarts its weapon cooldown in its own OnLoop
    // (advanceDrones), not here.
    if (!active.deployed) {
        // DroneSystem::PowerDrone 0x612b10 spends a part only on first deploy.
        --parts;
        if (owner == 0) {
            progressionRun_.tutorial = tutorialOwned_;
            progressionRun_.flag("used_drone");
            const auto previousResources = std::array{scrap_, fuel_, missiles_, parts_ + 1};
            noteResourceChanges(previousResources);
        }
        active.deployed = true;
        active.space = blueprint.type == "COMBAT" ? 1 - owner : owner;
        // A boarding pod keeps its body until delivery, but BoarderPodDrone::SetDeployed
        // 0x420060 clears diedInSpace on every deploy: a pod redeployed after its body
        // died flies again (native keeps the pod alive through this health field).
        if (blueprint.type != "BOARDER" || active.health <= 0) {
            active.health = blueprint.type == "REPAIR"                                  ? 25.f
                            : blueprint.id == "BOARDER_ION"                             ? 125.f
                            : blueprint.type == "BATTLE" || blueprint.type == "BOARDER" ? 150.f
                                                                                        : 1.f;
        }
        active.crewDying = active.crewDeathDone = false;
        active.crewDead = blueprint.type == "BOARDER";
        active.crewDeathElapsed = 0;
        active.room = active.targetRoom = -1;
        active.delivered = false;
        active.crewPath.clear();
        active.crewPathDoors.clear();
        active.crewPathIndex = 0;
        active.crewKills = 0;
        active.ionLastRoom = -1;
        active.ionPulseCooldown = 6.f;
        active.ionPulseCharge = active.ionPulseExplosion = -1.f;
        active.ionPulseEffect = 0;
        active.attackAnimation.clear();
        active.attackElapsed = active.attackGoal = active.attackAnimationElapsed = 0;
        active.attackHit = false;
        active.attackTargetOwner = active.attackTargetIndex = -1;
        active.attackTargetDrone = false;
        active.attackTargetDroneId = 0;
        // SpaceDrone::SetDeployed 0x41ebe0 keeps the retained location: its
        // RandomizeStartingPosition 0x41eb90 test can never pass.
        if (blueprint.type == "SHIP_REPAIR") {
            // ShipRepairDrone::SetDeployed 0x421160 grants 3-5 repairs.
            active.lifespan = int(combatRandom_.next() % 3) + 3;
        }
        // DefenseDrone::SetDeployed 0x421a10 starts ordinary defense drones
        // at one second; later shots use the authored cooldown.
        active.cooldown =
            blueprint.type == "SHIELD"
                ? superShieldDroneCooldown(owner == 0 ? superShields_ : enemy_.superShields)
                : 1.f;
        if (blueprint.type == "DEFENSE" || blueprint.type == "SHIELD") {
            // SetDeployed421a10 picks twice using the retained constructor/
            // movement-target angle. It never restarts at angle zero.
            active.position =
                defenseOrbitPoint(owner == 0 ? ship_ : enemy_.ship, active.orbitAngle, owner == 1);
            active.orbitAngle += 1.f;
            active.destination =
                defenseOrbitPoint(owner == 0 ? ship_ : enemy_.ship, active.orbitAngle, owner == 1);
            active.orbitAngle += 1.f;
            active.orbitInitialized = true;
        }
        if (blueprint.type == "BOARDER") {
            active.position = active.basePosition;
            active.destination = {800.f, active.position.y};
        }
        combatSound({"droneLaunch"}, tick_);
    }
    return true;
}

bool Run::toggleDrone(int index) {
    if (index < 0 || index >= int(combatDrones_.size()) || defeated_) {
        return false;
    }
    auto* drones = findSystem(systems_, 4);
    if (!drones) {
        return false;
    }
    auto& active = combatDrones_[std::size_t(index)];
    const auto& blueprint = catalog_.equipment(active.blueprint);
    if (active.powered) {
        if (systemLocked(*drones)) {
            return false;
        }
        int remaining = std::max(0, blueprint.power - active.bonusPower);
        while (remaining > 0 && drones->power > drones->bonusPower) {
            releaseBatteryPower(*drones, 1);
            --drones->power;
            --remaining;
        }
        if (remaining > 0) {
            return false;
        }
        active.powered = false;
        active.repowerPending = false;
        if (blueprint.type == "SHIELD") {
            active.shieldGlow = -1.f;
        }
        return true;
    }
    const auto fail = [&] {
        combatSound({"powerUpFail"}, tick_);
        return false;
    };
    // DroneControl5382d0 checks parts before system lock and preserves
    // unrelated warning timers on its other failure paths.
    if (!active.deployed && parts_ <= 0) {
        startArmamentWarning(2, "warning_no_drones");
        stopArmamentWarning(4);
        stopArmamentWarning(3);
        return fail();
    }
    if (systemLocked(*drones)) {
        return fail();
    }
    if (!active.deployed && (blueprint.type == "BOARDER" || blueprint.type == "COMBAT") &&
        (!enemy_.present || !hostile_)) {
        startArmamentWarning(4, "warning_no_enemy_ship");
        stopArmamentWarning(2);
        stopArmamentWarning(3);
        return fail();
    }
    if (active.rebuildTimer > 0) {
        return fail();
    }
    const int required = std::max(0, blueprint.power - active.bonusPower);
    const int effective = drones->power;
    if (systemCapacity(*drones) - effective < required) {
        const bool upgrade = drones->definition.power < required;
        const bool broken = !upgrade && drones->definition.power - effective >= required &&
                            drones->health < drones->definition.power;
        startArmamentWarning(3,
                             upgrade  ? "warning_needs_upgrade"
                             : broken ? "warning_system_broken"
                                      : "warning_no_system_power",
                             broken);
        stopArmamentWarning(2);
        // The original emits this failure sound, then still calls PowerDrone.
        combatSound({"powerUpFail"}, tick_);
    }
    if ((blueprint.type != "DEFENSE" && blueprint.type != "SHIP_REPAIR" &&
         blueprint.type != "SHIELD" && blueprint.type != "COMBAT" && blueprint.type != "REPAIR" &&
         blueprint.type != "BATTLE" && blueprint.type != "BOARDER") ||
        systemLocked(*drones) || active.rebuildTimer > 0 ||
        (blueprint.type == "BOARDER" && (!enemy_.present || !hostile_)) ||
        drones->power + required > systemCapacity(*drones) ||
        availablePower() < std::max(0, blueprint.power - active.bonusPower) ||
        (!active.deployed && parts_ <= 0) ||
        (blueprint.type == "COMBAT" && blueprint.droneWeapon.empty())) {
        return fail();
    }
    if (!powerDrone(active, 0)) {
        return fail();
    }
    combatSound({"powerUpSystem"}, tick_);
    return true;
}

void Run::advanceCombatDrone(CombatDrone& drone, int owner) {
    const auto& targetShip = owner == 0 ? enemy_.ship : ship_;
    if (targetShip.rooms.empty()) {
        drone.shotReady = false;
        return;
    }
    if (drone.bossSurge && drone.lifespan <= 0) {
        drone.powered = drone.deployed = drone.shotReady = false;
        return;
    }
    // Combat drones orbit in the target ship's local coordinates. Repair old
    // checkpoints that stored the owner's space before rendering or firing.
    drone.space = 1 - owner;
    advanceOrbitFlight(drone, owner);
}

void Run::advanceOrbitFlight(CombatDrone& drone, int owner) {
    // CombatDrone::OnLoop 0x4271d0 and ShipRepairDrone::OnLoop 0x427f50 share
    // this powered, unstunned flight. Globals::GetNextPoint lands exactly on
    // the destination; the next frame picks another and pauses half a second,
    // coasting at pause/1.5 of the last step, then latches bFire when the pause
    // reaches exactly zero. A beam's additionalPause holds the drone still
    // while it aims along its own copy of the sweep.
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    const bool repair = blueprint.type == "SHIP_REPAIR";
    const auto& orbitShip =
        repair ? (owner == 0 ? ship_ : enemy_.ship) : (owner == 0 ? enemy_.ship : ship_);
    const float centerOffsetY = repair && owner == 1 ? 110.f : 0.f;
    // Ordinary combat drones carry lifespan INT_MAX, which is never counted.
    const bool finiteLife = repair || drone.bossSurge;
    if (!drone.orbitInitialized) {
        pickOrbitDestination(drone, orbitShip, centerOffsetY, combatRandom_);
    }
    const SDL_FPoint previous = drone.position;
    bool moving = false;
    if (drone.position.x == drone.destination.x && drone.position.y == drone.destination.y) {
        pickOrbitDestination(drone, orbitShip, centerOffsetY, combatRandom_);
        drone.pause = .5f;
    } else {
        moving = drone.pause <= 0 && drone.additionalPause <= 0;
    }
    drone.hackAngle = drone.aimAngle;
    if (moving) {
        const float speed = float(std::max(orbitShip.ellipse[0], orbitShip.ellipse[1])) / 350.f *
                            float(blueprint.droneSpeed) * Time::originalStep;
        drone.position = nextPoint(drone.position, speed, drone.destination);
        drone.velocity = {drone.position.x - previous.x, drone.position.y - previous.y};
        const float travelled = std::hypot(drone.position.x - drone.lastDestination.x,
                                           drone.position.y - drone.lastDestination.y);
        const float pathLength = std::hypot(drone.destination.x - drone.lastDestination.x,
                                            drone.destination.y - drone.lastDestination.y);
        drone.progressToDestination = pathLength > 0 ? travelled / pathLength : 1.f;
        updateAimingAngle(drone,
                          drone.hasAimTarget,
                          drone.aimTarget,
                          drone.destination,
                          drone.progressToDestination);
    } else if (drone.additionalPause <= 0) {
        drone.pause -= Time::step;
        float coast = 0;
        if (drone.pause <= 0) {
            drone.pause = 0;
        } else {
            coast = drone.pause / 1.5f;
        }
        drone.position.x += coast * drone.velocity.x;
        drone.position.y += coast * drone.velocity.y;
    } else {
        drone.additionalPause -= Time::step;
        if (drone.additionalPause < 0 && finiteLife) {
            --drone.lifespan;
        }
        drone.velocity = {};
        if (drone.beamFinalTarget.x != drone.beamCurrentTarget.x ||
            drone.beamFinalTarget.y != drone.beamCurrentTarget.y) {
            drone.beamCurrentTarget = nextPoint(drone.beamCurrentTarget,
                                                Time::originalStep * drone.beamSpeed,
                                                drone.beamFinalTarget);
            updateAimingAngle(drone, true, drone.beamCurrentTarget, drone.position, 1.f);
        }
    }
    if (drone.pause == 0) {
        // Only bFire is latched here. SpaceManager41c940 constructs/extracts
        // the projectile after existing-projectile updates.
        drone.shotReady = true;
        drone.pause = -1.f;
        if (finiteLife) {
            --drone.lifespan;
        }
    }
}

void Run::advanceCrewDrone(CombatDrone& drone, int owner, bool carrierOnly) {
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    const bool boarder = blueprint.type == "BOARDER";
    const bool ionBoarder = blueprint.id == "BOARDER_ION";
    if (boarder && (carrierOnly ? !drone.deployed || drone.delivered : !drone.delivered)) {
        return;
    }
    if (drone.crewDead && !drone.deployed) {
        if (drone.controllerHackLevel > 1) {
            drone.ionStun = .5f;
        }
        return;
    }
    if (!boarder && drone.deployed && !drone.delivered) {
        const auto* controller = findSystem(owner == 0 ? systems_ : enemy_.systems, 4);
        RunCrew body;
        body.appearance.species = blueprint.type == "REPAIR" ? "repair" : "battle";
        body.health = drone.health;
        // SetDeployed437770 empties the old reservation; the following
        // CrewDroneOnLoop43ce20 SetRoom reacquires a controller-room tile.
        const bool dead = drone.crewDead;
        drone.crewDead = true;
        if (placeVoyageCrew(
                body, controller ? controller->definition.room : -1, false, owner == 1)) {
            drone.crewPhysicalShip = owner;
            drone.crewRoom = body.room;
            drone.crewPosition = {body.x, body.y};
            drone.crewSlot = body.slot;
            drone.delivered = true;
        }
        drone.crewDead = dead;
    }

    struct BodyProjection {
        CombatDrone& drone;
        SDL_FPoint carrierPosition;
        int carrierRoom;
        bool active;
        bool carrier;

        ~BodyProjection() {
            if (!active) {
                return;
            }
            drone.crewPosition = drone.position;
            drone.crewRoom = drone.room;
            if (carrier) {
                drone.position = carrierPosition;
                drone.room = carrierRoom;
            }
        }
    } projection{drone,
                 drone.position,
                 drone.room,
                 drone.crewPhysicalShip >= 0 && (!boarder || !drone.deployed || drone.delivered),
                 boarder};

    if (projection.active) {
        drone.position = drone.crewPosition;
        drone.room = drone.crewRoom;
    }
    const bool enemySpace = projection.active ? drone.crewPhysicalShip == 1
                            : boarder         ? owner == 0
                                              : owner == 1;
    auto& ship = enemySpace ? enemy_.ship : ship_;
    auto& systems = enemySpace ? enemy_.systems : systems_;
    // A pod flies while it has a movement target; a surrender keeps it.
    if (ship.rooms.empty() || (boarder && !drone.delivered &&
                               (!droneHasMovementTarget(drone, owner) || cloaked(1 - owner)))) {
        return;
    }
    const auto center = [](const RoomDefinition& shape) {
        return SDL_FPoint{float(shape.x + 17), float(shape.y + 17)};
    };
    const auto destroy = [&]() {
        // Drone::SetDestroyed 0x458ba0 starts a ten-second rebuild lock.
        drone.health = 0;
        drone.rebuildTimer = 10.f;
        drone.deployed = false;
        // PermanentDeath73de70 is false: an interior corpse stays in the
        // physical crew list, even after SetDeployed(false), until reused.
        // An intercepted pod has not installed a physical body.
        if (drone.room < 0) {
            drone.delivered = false;
        }
        drone.crewDead = true;
        drone.crewPath.clear();
        drone.crewPathDoors.clear();
        drone.crewPathIndex = 0;
        drone.crewBlockingDoor = drone.crewLastDoor = -1;
        drone.crewNewGoal = true;
        if (drone.powered) {
            auto* controller = findSystem(owner == 0 ? systems_ : enemy_.systems, 4);
            if (controller) {
                controller->power =
                    std::max(controller->bonusPower,
                             controller->power - std::max(0, blueprint.power - drone.bonusPower));
                releaseBatteryPower(
                    *controller, std::max(0, blueprint.power - drone.bonusPower), owner == 1);
            }
        }
        drone.powered = drone.repowerPending = false;
    };
    if (drone.crewDead && drone.deployed && drone.delivered) {
        destroy();
        return;
    }
    if (drone.crewDead && (!boarder || !drone.deployed || drone.delivered)) {
        // CrewDrone43ce20 still visits undeployed retained factory bodies.
        // Its stored hack effect may refresh stun before CrewMember4747f0's
        // bDead early return; no movement/health/animation task is performed.
        if (drone.controllerHackLevel > 1) {
            drone.ionStun = .5f;
        }
        return;
    }
    if (drone.health <= 0 && (!boarder || drone.delivered)) {
        return;
    }
    if (drone.room < 0) {
        if (!drone.powered || drone.ionStun > 0) {
            return;
        }
        if (boarder) {
            // BoarderPodDrone::SetMovementTarget 0x41dab0 flies to x800
            // in its owner space. PickDestination 0x41e2f0 then transfers to
            // the target space, chooses a valuable system room, and spawns
            // on the original 600-radius circle around (200,200).
            const int targetSpace = 1 - owner;
            const float speed = float(blueprint.droneSpeed) * Time::originalStep;
            if (drone.space != targetSpace) {
                // BoarderPodDrone::OnLoop 0x427820 picks its next destination
                // only once it rests on the x800 point, then steps on that
                // same frame (Globals::GetNextPoint 0x5a88b0).
                if (drone.position.x != drone.destination.x ||
                    drone.position.y != drone.destination.y) {
                    const SDL_FPoint previous = drone.position;
                    drone.position = nextPoint(drone.position, speed, drone.destination);
                    if (drone.position.x != drone.destination.x ||
                        drone.position.y != drone.destination.y) {
                        drone.velocity = {drone.position.x - previous.x,
                                          drone.position.y - previous.y};
                    }
                    return;
                }
                drone.space = targetSpace;
                drone.pause = .5f;
                drone.targetRoom =
                    systems.empty()
                        ? ship.rooms.front().id
                        : systems[combatRandom_.next() % systems.size()].definition.room;
                const auto shape = std::ranges::find_if(ship.rooms, [&](const auto& r) {
                    return r.id == drone.targetRoom;
                });
                if (shape == ship.rooms.end()) {
                    return;
                }
                drone.destination = {float(shape->x + shape->w / 2),
                                     float(shape->y + shape->h / 2)};
                const float angle =
                    float(combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
                drone.position = {200.f + 600.f * std::cos(angle), 200.f + 600.f * std::sin(angle)};
            }
            const SDL_FPoint previous = drone.position;
            drone.position = nextPoint(drone.position, speed, drone.destination);
            const bool arrived =
                drone.position.x == drone.destination.x && drone.position.y == drone.destination.y;
            // Landing exactly keeps the previous speedVector.
            if (!arrived) {
                drone.velocity = {drone.position.x - previous.x, drone.position.y - previous.y};
            }
            // An outside-to-inside crossing of the super-shield ellipse calls
            // BoarderPodDrone::BlowUp 0x421db0: the pod explodes in space and
            // its OnLoop starts the rebuild when the explosion ends.
            if ((enemySpace ? enemy_.superShields : superShields_) > 0 &&
                !insideShield(ship, previous, enemySpace) &&
                insideShield(ship, drone.position, enemySpace)) {
                blowUpDrone(drone);
                return;
            }
            if (!arrived) {
                return;
            }
            drone.room = drone.targetRoom;
            drone.delivered = true;
            drone.crewDead = drone.crewDying = drone.crewDeathDone = false;
            drone.crewDeathElapsed = 0;
            drone.health = ionBoarder ? 125.f : 150.f;
            drone.crewPhysicalShip = int(enemySpace);
            drone.crewRoom = drone.room;
            drone.crewPosition = drone.position;
            projection.active = true;
            projection.carrierPosition = drone.position;
            projection.carrierRoom = drone.room;
            assignPhysicalCrewOrder(drone, true);
            const auto shape = std::ranges::find_if(ship.rooms, [&](const auto& r) {
                return r.id == drone.room;
            });
            const int columns = std::max(1, shape->w / 35);
            const int tile =
                std::clamp(int(drone.position.x - float(shape->x)) / 35, 0, columns - 1) +
                columns * std::clamp(int(drone.position.y - float(shape->y)) / 35,
                                     0,
                                     std::max(0, shape->h / 35 - 1));
            drone.crewSlot = tile;
            combatSound({"smallExplosion"}, tick_);
            // The landing DamageTarget runs DamageArea's draws and breaches a
            // random outer-hull tile of the room (Ship::BreachRandomHull).
            CombatProjectile landing;
            landing.weapon = drone.blueprint;
            landing.owner = owner;
            landing.targetShip = int(enemySpace);
            landing.room = drone.room;
            landing.position = landing.target = drone.position;
            landing.soundsInitialized = true;
            landing.podBreach = true;
            impact(landing, false, nullptr, true);
        } else {
            const auto* controller = findSystem(systems, 4);
            drone.room = controller ? controller->definition.room : ship.rooms.front().id;
            const auto shape = std::ranges::find_if(ship.rooms, [&](const auto& r) {
                return r.id == drone.room;
            });
            if (shape == ship.rooms.end()) {
                return;
            }
            drone.position = center(*shape);
            drone.targetRoom = drone.room;
            drone.delivered = true;
            assignPhysicalCrewOrder(drone);
        }
    }
    if (carrierOnly) {
        return;
    }
    const auto* controller = findSystem(owner == 0 ? systems_ : enemy_.systems, 4);
    if (controller && controller->hackActive && controller->hackLevel > 1) {
        // CrewDrone::OnLoop43ce20 refreshes the inherited half-second stun.
        drone.ionStun = .5f;
    }
    if (ionBoarder) {
        // ShipManager::UpdateCrewmembers49ee10 asks GetRoomDamage4374b0
        // before the crew animation's next update. A completed ion charge
        // therefore emits once, including the source's own-body stun exclusion.
        if (drone.ionPulseCharge >= 0 && drone.ionPulseEffect >= 1.25f) {
            CombatProjectile pulse;
            pulse.weapon = drone.blueprint;
            pulse.owner = owner;
            pulse.targetShip = int(enemySpace);
            pulse.room = drone.room;
            pulse.position = pulse.target = drone.position;
            pulse.soundsInitialized = true;
            impact(pulse, false, nullptr, true);
            drone.ionLastRoom = drone.room;
            drone.ionPulseCooldown = 6.f;
            drone.ionPulseCharge = -1.f;
            drone.ionPulseEffect = 0;
            drone.ionPulseExplosion = 0;
            drone.ionPulsePosition = drone.position;
            audio_.sound("ionHit1");
        }
        if (drone.ionPulseExplosion >= 0) {
            drone.ionPulseExplosion += Time::step;
            if (drone.ionPulseExplosion > catalog_.animation("explosion_big1_ion").duration) {
                drone.ionPulseExplosion = -1.f;
            }
        }
        if (drone.ionPulseCooldown < 0) {
            // TimerHelper::Done5a9640 uses strictly greater than its fixed
            // six-second goal; fixed min/max consumes no RNG in Start5a94f0.
            drone.ionPulseCooldown = 6.f;
            drone.ionPulseCharge = 0;
            drone.ionPulseEffect = 0;
            audio_.sound("ionBoarderCharge");
        }
        const auto currentSystem = std::ranges::find(systems, drone.room, [](const auto& entry) {
            return entry.definition.room;
        });
        const bool sabotaging =
            drone.powered && drone.ionStun <= 0 && (!controller || controller->hackLevel <= 1) &&
            drone.crewPathIndex >= int(drone.crewPath.size()) && drone.crewBlockingDoor < 0 &&
            currentSystem != systems.end() && currentSystem->health > 0;
        if (drone.ionPulseCharge >= 0) {
            if (!sabotaging) {
                // UpdateShooting43c500 keeps the charge active but restarts
                // its animation/glow whenever status is not sabotage7.
                drone.ionPulseCharge = 0;
                drone.ionPulseEffect = 0;
            } else {
                const float duration = catalog_.animation("boarder_ion_ion").duration;
                const float advanced = drone.ionPulseCharge + Time::step;
                if (advanced > duration) {
                    drone.ionPulseEffect += Time::step;
                }
                drone.ionPulseCharge = std::min(duration + Time::step, advanced);
            }
        } else {
            drone.ionPulseCooldown -= Time::step;
        }
    }
    if (!drone.powered) {
        drone.crewPath.clear();
        drone.crewPathDoors.clear();
        drone.crewPathIndex = 0;
        drone.targetRoom = drone.room;
        return;
    }
    if (drone.ionStun > 0 || (controller && controller->hackActive && controller->hackLevel > 1)) {
        return;
    }
    if (drone.crewPath.empty()) {
        const auto shape = std::ranges::find(ship.rooms, drone.room, &RoomDefinition::id);
        if (shape != ship.rooms.end()) {
            const int columns = std::max(1, shape->w / 35);
            const int column = std::clamp(int((drone.position.x - shape->x) / 35), 0, columns - 1);
            const int row = std::clamp(
                int((drone.position.y - shape->y) / 35), 0, std::max(1, shape->h / 35) - 1);
            const SDL_FPoint goal{float(shape->x + 35 * column + 17),
                                  float(shape->y + 35 * row + 17)};
            if (std::abs(drone.position.x - goal.x) >= .5f ||
                std::abs(drone.position.y - goal.y) >= .5f) {
                // CrewDrone::OnLoop 0x43ce20 acquires a physical faction slot
                // through SetRoom/MoveToRoom before it can stand and fight.
                RunCrew proxy;
                proxy.health = drone.health;
                proxy.room = drone.room;
                proxy.x = drone.position.x;
                proxy.y = drone.position.y;
                if (!order(proxy,
                           drone.room,
                           row * columns + column,
                           true,
                           enemySpace,
                           nullptr,
                           int(owner != int(enemySpace)),
                           &drone)) {
                    return;
                }
                drone.targetRoom = proxy.targetRoom;
                drone.crewPathIndex = 0;
                drone.crewNewGoal = true;
                drone.crewBlockingDoor = -1;
                for (const auto& waypoint : proxy.path) {
                    drone.crewPath.push_back(waypoint.point);
                    drone.crewPathDoors.push_back(waypoint.door);
                }
            }
        }
    }
    if (drone.crewPathIndex < int(drone.crewPath.size())) {
        RunCrew body;
        body.appearance.species = blueprint.type == "REPAIR" ? "repair" : "battle";
        body.health = drone.health;
        body.room = drone.room;
        body.slot = drone.crewSlot;
        body.targetRoom = drone.targetRoom;
        body.x = drone.position.x;
        body.y = drone.position.y;
        body.blockingDoor = drone.crewBlockingDoor;
        body.lastDoor = drone.crewLastDoor;
        body.newGoal = drone.crewNewGoal;
        const auto& target = *std::ranges::find(ship.rooms, drone.targetRoom, &RoomDefinition::id);
        const int columns = std::max(1, target.w / 35);
        const auto finish = drone.crewPath.back();
        body.targetSlot =
            std::clamp(int((finish.x - target.x) / 35), 0, columns - 1) +
            columns *
                std::clamp(int((finish.y - target.y) / 35), 0, std::max(1, target.h / 35) - 1);
        for (std::size_t i = std::size_t(drone.crewPathIndex); i < drone.crewPath.size(); ++i) {
            body.path.push_back({drone.crewPath[i], drone.crewPathDoors.at(i)});
        }
        advanceCrew(body, enemySpace, owner);
        drone.room = body.room;
        drone.position = {body.x, body.y};
        drone.crewSlot = body.slot;
        drone.crewBlockingDoor = body.blockingDoor;
        drone.crewLastDoor = body.lastDoor;
        drone.crewNewGoal = body.newGoal;
        drone.crewPath.clear();
        drone.crewPathDoors.clear();
        drone.crewPathIndex = 0;
        for (const auto& waypoint : body.path) {
            drone.crewPath.push_back(waypoint.point);
            drone.crewPathDoors.push_back(waypoint.door);
        }
        return;
    }
}

int Run::effectiveDroneDodge(const CombatDrone& drone) const {
    // SpaceDrone::CollisionMoving 0x41e400 reads the dodge straight from the
    // drone's blueprint. diagnosticDodge exists only so a fixture can author the
    // non-zero dodge no archived blueprint carries.
    return drone.diagnosticDodge >= 0 ? drone.diagnosticDodge
                                      : catalog_.equipment(drone.blueprint).droneDodge;
}

void Run::blowUpDrone(CombatDrone& drone, bool silent) {
    const auto& type = catalog_.equipment(drone.blueprint).type;
    if (type == "REPAIR" || type == "BATTLE") {
        // CrewDrone::BlowUp4376c0 enters normal crew death through HP.
        if (drone.health > 0) {
            drone.health = std::max(0.f, drone.health - 1000.f);
        }
    } else if (type == "BOARDER" && drone.delivered) {
        // BoarderPod421db0 kills the embedded delivered actor directly.
        drone.health = 0;
        drone.crewDead = true;
        drone.crewDying = false;
    } else if (drone.deployed && drone.spaceExplosionElapsed < 0) {
        // SpaceDrone::BlowUp422480 starts the retained explosion; derived
        // OnLoop observes completion before SetDestroyed/rebuild accounting.
        drone.spaceExplosionElapsed = 0;
        drone.shotReady = false;
        if (!silent) {
            combatSound({"smallExplosion"}, tick_);
        }
    }
}

void Run::detachCrewDroneShip(int ship) {
    for (int owner : {0, 1}) {
        for (auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip != ship) {
                continue;
            }
            if (!drone->crewDead) {
                drone->health = 0;
            }
            drone->crewDead = true;
            drone->crewDying = false;
            drone->crewPhysicalShip = drone->crewRoom = drone->crewSlot = -1;
            drone->crewPath.clear();
            drone->crewPathDoors.clear();
            drone->crewPathIndex = 0;
            drone->crewBlockingDoor = drone->crewLastDoor = -1;
            drone->crewNewGoal = true;
            drone->targetRoom = -1;
            drone->ionLastRoom = -1;
            drone->attackTargetOwner = drone->attackTargetIndex = -1;
            drone->attackTargetDrone = false;
            drone->attackTargetDroneId = 0;
            drone->attackAnimation.clear();
            drone->attackElapsed = drone->attackGoal = drone->attackAnimationElapsed = 0;
            drone->attackHit = false;
        }
    }
}

void Run::advanceDroneBase(CombatDrone& drone, int owner, int controllerId) {
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    // Defense426a50/Combat4271d0/Repair427f50 update DamageMessage before
    // power/explosion gates. Boarder427820/Hacking44e4b0 do not update it.
    if ((blueprint.type == "DEFENSE" || blueprint.type == "SHIELD" || blueprint.type == "COMBAT" ||
         blueprint.type == "SHIP_REPAIR") &&
        drone.spaceMissElapsed >= 0) {
        drone.spaceMissElapsed += Time::step;
        if (drone.spaceMissElapsed > 1.f) {
            drone.spaceMissElapsed = -1.f;
        }
    }
    const bool interior = blueprint.type == "REPAIR" || blueprint.type == "BATTLE";
    const bool destroyed = interior                      ? drone.crewDead
                           : blueprint.type == "BOARDER" ? drone.delivered && drone.crewDead
                                                         : drone.health <= 0;
    // DroneOnLoop458cb0 rolls at integer-second crossings strictly after two
    // seconds of its virtual stun predicate. HackingDrone44e4b0 also runs it.
    const bool stunned = drone.controllerHackLevel > 1 || (!interior && drone.ionStun > 0);
    if (drone.deployed && !destroyed && stunned) {
        const float previous = std::floor(drone.hackTime);
        drone.hackTime += Time::step;
        if (std::floor(drone.hackTime) > previous && drone.hackTime > 2.f &&
            combatRandom_.next() % 20 <= 2) {
            blowUpDrone(drone);
        }
    } else {
        drone.hackTime = 0;
    }
    if (drone.spaceExplosionElapsed < 0) {
        return;
    }
    const auto& explosion = catalog_.animation(drone.constructorExplosionImage);
    const float advanced = drone.spaceExplosionElapsed + Time::step;
    drone.spaceExplosionElapsed = std::min(explosion.duration, advanced);
    drone.shotReady = false;
    if (advanced <= explosion.duration) {
        return;
    }
    drone.spaceExplosionElapsed = -1;
    // Drone::SetDestroyed(true, true) starts the rebuild only while bDead is
    // clear. A hull repair drone that ran out or lost power mid-explosion has
    // already set it without a rebuild.
    const bool alreadyDead = blueprint.type != "BOARDER" && drone.health <= 0;
    if (blueprint.type != "BOARDER") {
        drone.health = 0;
    }
    if (controllerId == 4 && !alreadyDead) {
        drone.rebuildTimer = 10.f;
    }
    if (controllerId == 4 && drone.powered) {
        if (auto* controller = findSystem(owner == 0 ? systems_ : enemy_.systems, controllerId)) {
            const int amount = std::max(0, blueprint.power - drone.bonusPower);
            releaseBatteryPower(*controller, amount, owner == 1);
            controller->power = std::max(controller->bonusPower, controller->power - amount);
        }
    }
    drone.powered = drone.deployed = drone.repowerPending = false;
    if (blueprint.type == "BOARDER") {
        drone.delivered = false;
        drone.crewDead = true;
    }
}

void Run::advanceDroneSystem(int owner, std::size_t systemIndex) {
    auto& systems = owner == 0 ? systems_ : enemy_.systems;
    if (systemIndex >= systems.size() || systems[systemIndex].definition.id != 4) {
        return;
    }
    auto& controller = systems[systemIndex];
    auto& drones = owner == 0 ? combatDrones_ : enemy_.drones;
    const auto& augments = (owner == 0 ? ship_ : enemy_.ship).augments;
    const bool recovery =
        std::ranges::find(augments, std::string("DRONE_RECOVERY")) != augments.end();
    auto& parts = owner == 0 ? parts_ : enemy_.droneParts;
    for (auto& drone : drones) {
        // DroneSystem613780 attempts each retained bonus-loss request once,
        // BEFORE rebuilding/death handling, and clears it even when locked or
        // allocation fails. A normal clamp/depower never invents that request.
        if (drone.repowerPending && !drone.powered && !systemLocked(controller) &&
            !drone.bossSurge && (droneCanBeDeployed(drone, owner) || drone.deployed)) {
            (void)powerDrone(drone, owner);
        }
        drone.repowerPending = false;
        drone.controllerHackLevel = controller.hackActive ? controller.hackLevel : 0;
        drone.rebuildTimer = std::max(0.f, drone.rebuildTimer - Time::step);
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        const bool crewBody = blueprint.type == "REPAIR" || blueprint.type == "BATTLE";
        const bool deliveredBoarder = blueprint.type == "BOARDER" && drone.delivered;
        const bool destroyed = crewBody || deliveredBoarder ? drone.crewDead : drone.health <= 0;
        if (destroyed && drone.deployed) {
            // SetDestroyed(true, true), SetDeployed(false), SetDestroyed(false,
            // false), DePowerDrone, SetPowered(false). Only a crew body or a
            // delivered boarder still had Drone::bDead clear, so only they
            // start the rebuild here; a space drone set (or skipped) it itself.
            if (crewBody || deliveredBoarder) {
                drone.rebuildTimer = 10;
            }
            undeployDrone(drone);
            if (blueprint.type == "BOARDER") {
                drone.delivered = false;
            }
            if (drone.powered) {
                (void)depowerDrone(drone, controller, owner);
            }
            drone.powered = false;
        }
        if (!droneCanBeDeployed(drone, owner)) {
            // Every frame an undeployable drone is recalled (the player's) and
            // depowered; DRONE_RECOVERY refunds a living deployed space drone
            // (CanBeRecovered: SpaceDrone true, boarder pods false).
            const bool recoverable = blueprint.type == "COMBAT" || blueprint.type == "DEFENSE" ||
                                     blueprint.type == "SHIP_REPAIR" || blueprint.type == "SHIELD";
            if (recoverable && drone.deployed && !destroyed && recovery) {
                ++parts;
            }
            if (owner == 0) {
                undeployDrone(drone);
            }
            if (drone.powered) {
                (void)depowerDrone(drone, controller, owner);
            }
        }
    }
    checkSystemMaxPower(controller, owner == 1);
}

void Run::advanceDroneAi(int owner) {
    if (owner != 1 || !hostile_) {
        return;
    }
    auto* controller = findSystem(enemy_.systems, 4);
    if (!controller || systemLocked(*controller)) {
        return;
    }
    for (auto& drone : enemy_.drones) {
        if (!drone.powered && !drone.bossSurge) {
            (void)powerDrone(drone, owner);
        }
    }
}

void Run::advanceDroneSystems() {
    for (int owner : {0, 1}) {
        auto& systems = owner == 0 ? systems_ : enemy_.systems;
        for (std::size_t i = 0; i < systems.size(); ++i) {
            advanceDroneSystem(owner, i);
        }
    }
}

void Run::advanceHackingDroneSpace(CombatDrone& drone, int owner) {
    // HackingSystem6083b0 sets power/hacking state during the manager pass;
    // HackingDrone44e4b0 performs flight only in registered SpaceManager order.
    advanceDroneBase(drone, owner, 15);
    drone.ionStun = std::max(0.f, drone.ionStun - Time::step);
    drone.hackingFlashElapsed = Time::loopAnimation(drone.hackingFlashElapsed, 1.f);
    if (!drone.deployed || !drone.powered || drone.ionStun > 0 || drone.controllerHackLevel > 1 ||
        drone.spaceExplosionElapsed >= 0 || !enemy_.present || cloaked(1 - owner) ||
        (owner == 0 ? enemy_.escapeJumpElapsed >= 0 : jumpTicks_ > 0)) {
        if (drone.spaceExplosionElapsed < 0) {
            advancePoweredDownDrone(drone,
                                    catalog_,
                                    combatRandom_,
                                    enemy_.present ? &(owner == 0 ? enemy_.ship : ship_) : nullptr);
        }
        return;
    }
    if (drone.delivered) {
        // Hacking44e4b0 advances the land strip before the extend strip.
        // Arrival admits gameplay hacking immediately; setup is presentation.
        if (drone.hackingFlyingElapsed >= 0) {
            const auto& land = catalog_.animation("drone_hack_land");
            drone.hackingFlyingElapsed += Time::step;
            if (drone.hackingFlyingElapsed > land.duration) {
                drone.hackingFlyingElapsed = -1;
                drone.hackingExtendingElapsed = 0;
            }
        } else if (drone.hackingExtendingElapsed >= 0) {
            const auto& extend = catalog_.animation("drone_hack_extend");
            drone.hackingExtendingElapsed += Time::step;
            if (drone.hackingExtendingElapsed > extend.duration) {
                drone.hackingExtendingElapsed = -1;
                drone.hackingFinishedSetup = true;
            }
        }
        return;
    }
    auto& space = owner == 0 ? hackingDroneSpace_ : enemy_.hackingDroneSpace;
    const auto& targetShip = owner == 0 ? enemy_.ship : ship_;
    // SpaceDrone::UpdateLocation 0x428600 runs before the step: a drone resting
    // on its destination in its own space crosses into the target's space
    // (HackingDrone::PickDestination 0x44dbf0) and moves on the same frame.
    if (space != 1 - owner && drone.position.x == drone.destination.x &&
        drone.position.y == drone.destination.y) {
        const auto approach = hackingApproach(targetShip, drone.room, combatRandom_);
        drone.position = approach.first;
        drone.destination = approach.second;
        space = 1 - owner;
    }
    const float speed = catalog_.equipment(drone.blueprint).droneSpeed * Time::originalStep;
    const SDL_FPoint previous = drone.position;
    drone.position = nextPoint(drone.position, speed, drone.destination);
    // Landing exactly on the destination keeps the previous speedVector.
    if (drone.position.x != drone.destination.x || drone.position.y != drone.destination.y) {
        drone.velocity = {drone.position.x - previous.x, drone.position.y - previous.y};
    }
    drone.lastAimingAngle = drone.aimAngle;
    if (drone.velocity.x != 0 || drone.velocity.y != 0) {
        drone.aimAngle =
            std::atan2(drone.velocity.y, drone.velocity.x) * 180.f / std::numbers::pi_v<float>;
        if (drone.aimAngle < 0) {
            drone.aimAngle += 360.f;
        }
    }
    if (space == 1 - owner) {
        // The source attaches when its twenty-pixel probe along the normalised
        // speedVector reaches a room, retaining the carrier outside the hull.
        const float length = std::hypot(drone.velocity.x, drone.velocity.y);
        const SDL_FPoint future{
            drone.position.x + (length > 0 ? 20.f * drone.velocity.x / length : 0.f),
            drone.position.y + (length > 0 ? 20.f * drone.velocity.y / length : 0.f)};
        const int x = int(future.x);
        const int y = int(future.y);
        if (std::ranges::any_of(targetShip.rooms, [&](const auto& room) {
                return x >= room.x && x <= room.x + room.w && y >= room.y && y <= room.y + room.h;
            })) {
            drone.delivered = true;
            drone.hackingFlyingElapsed = 0;
            drone.hackingExtendingElapsed = -1;
            drone.hackingFinishedSetup = false;
            drone.destination = drone.position;
            drone.velocity = {};
            combatSound({"hackLand"}, tick_);
            return;
        }
        const int shields = owner == 0 ? enemy_.superShields : superShields_;
        if (shields > 0 && insideShield(targetShip, drone.position, owner == 0) &&
            !insideShield(targetShip, previous, owner == 0)) {
            blowUpDrone(drone);
            return;
        }
    }
}

void Run::advanceDrones() {
    // SpaceManager41c940 visits every registered pointer in insertion order,
    // including cargo and retired actors. Registration does not imply power.
    for (const auto& [owner, actor] : registeredSpaceDrones()) {
        auto& drone = *actor;
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        if (blueprint.type == "HACKING") {
            advanceHackingDroneSpace(drone, owner);
            continue;
        }
        drone.repairBeamAge = std::max(0.f, drone.repairBeamAge - Time::step);
        if (blueprint.type == "DEFENSE") {
            drone.defenseTargetType = 5;
            drone.pause = 0;
        }
        advanceDroneBase(drone, owner);
        drone.ionStun = std::max(0.f, drone.ionStun - Time::step);
        const bool combat = blueprint.type == "COMBAT";
        const bool repair = blueprint.type == "SHIP_REPAIR";
        if (drone.spaceExplosionElapsed >= 0) {
            // ShipRepairDrone::OnLoop 0x427f50 still kills an exploding drone
            // with no repairs left or no power (SetDestroyed(true, false)).
            if (repair && (drone.lifespan <= 0 || !drone.powered)) {
                drone.health = 0;
            }
            continue;
        }
        if (blueprint.type == "BOARDER") {
            drone.poweredLastFrame = drone.powered;
            drone.lastAimingAngle = drone.aimAngle;
            if (!drone.powered || drone.ionStun > 0 || drone.controllerHackLevel > 1 ||
                !enemy_.present || cloaked(1 - owner) ||
                (owner == 0 ? enemy_.escapeJumpElapsed >= 0 : jumpTicks_ > 0)) {
                advancePoweredDownDrone(drone,
                                        catalog_,
                                        combatRandom_,
                                        enemy_.present ? &(owner == 0 ? enemy_.ship : ship_)
                                                       : nullptr);
                continue;
            }
            advanceCrewDrone(drone, owner, true);
            continue;
        }
        const bool stunned =
            (drone.controllerHackLevel > 1 && !drone.bossSurge) || drone.ionStun > 0;
        if (combat || repair) {
            // CombatDrone::OnLoop 0x4271d0 / ShipRepairDrone::OnLoop 0x427f50:
            // nothing moves without a movement target; a powered-down,
            // destroyed or stunned drone drifts, drops its pause and bFire.
            if (repair && drone.lifespan <= 0) {
                drone.health = 0;
            }
            const auto& movementShip =
                combat ? (owner == 0 ? enemy_.ship : ship_) : (owner == 0 ? ship_ : enemy_.ship);
            if (droneHasMovementTarget(drone, owner) && !movementShip.rooms.empty()) {
                if (!drone.deployed || !drone.powered || drone.health <= 0 || stunned) {
                    advancePoweredDownDrone(drone, catalog_, combatRandom_, &movementShip);
                    drone.pause = -1.f;
                    drone.shotReady = false;
                } else if (combat) {
                    advanceCombatDrone(drone, owner);
                } else {
                    advanceOrbitFlight(drone, owner);
                    advanceRepairWaves(drone);
                }
            }
            // An unpowered hull repair drone dies without a rebuild, so the
            // DroneSystem pass recalls it and redeploying costs a part.
            if (repair && (drone.lifespan <= 0 || !drone.powered)) {
                drone.health = 0;
            }
            continue;
        }
        if ((blueprint.type != "DEFENSE" && blueprint.type != "SHIELD") ||
            (blueprint.type == "DEFENSE" && blueprint.droneWeapon.empty())) {
            continue;
        }
        const auto& protectedShip = owner == 0 ? ship_ : enemy_.ship;
        if (protectedShip.rooms.empty()) {
            continue;
        }
        auto& shields = owner == 0 ? superShields_ : enemy_.superShields;
        const bool shield = blueprint.type == "SHIELD";
        // DefenseDrone::OnLoop 0x426a50 restarts the weapon cooldown
        // (virtual GetWeaponCooldown) on the first powered frame of a drone
        // that was deployed the frame before. A power toggle between two
        // space passes, such as the enemy AI's reallocation, is not seen.
        if (drone.powered && !drone.poweredLastFrame && drone.deployedLastFrame) {
            drone.cooldown = shield ? superShieldDroneCooldown(shields) : blueprint.cooldown;
        }
        drone.poweredLastFrame = drone.powered;
        drone.deployedLastFrame = drone.deployed;
        const bool active = drone.deployed && drone.powered;
        if (!active || stunned) {
            // DefenseDrone::OnLoop 0x426a50 keeps a latched bFire while it
            // drifts powered down or stunned.
            advancePoweredDownDrone(drone, catalog_, combatRandom_, &protectedShip);
        } else {
            if (!drone.orbitInitialized) {
                drone.position = defenseOrbitPoint(protectedShip, 0.f, owner == 1);
                drone.destination = defenseOrbitPoint(protectedShip, 1.f, owner == 1);
                drone.orbitAngle = 1.f;
                drone.orbitInitialized = true;
                drone.cooldown = blueprint.type == "SHIELD" ? superShieldDroneCooldown(shields)
                                                            : blueprint.cooldown;
            }
            // DefenseDrone::OnLoop: arriving picks the next one-degree point
            // before this frame's step (DefenseDrone::PickDestination 0x421090).
            if (drone.position.x == drone.destination.x &&
                drone.position.y == drone.destination.y) {
                drone.destination = defenseOrbitPoint(protectedShip, drone.orbitAngle, owner == 1);
                drone.orbitAngle += 1.f;
            }
            drone.hackAngle = drone.aimAngle;
            drone.cooldown -= Time::step;
            const float speed =
                drone.currentSpeed >= 0 ? drone.currentSpeed : float(blueprint.droneSpeed);
            const SDL_FPoint previous = drone.position;
            drone.position =
                nextPoint(drone.position, speed * Time::originalStep, drone.destination);
            drone.velocity = {drone.position.x - previous.x, drone.position.y - previous.y};
            if (drone.cooldown < 0) {
                drone.shotReady = true;
                drone.cooldown = -1.f;
            }
        }
        if (!shield) {
            continue;
        }
        // SuperShieldDrone::OnLoop 0x426db0 after its DefenseDrone base. Its
        // speed takes effect on the next frame's step.
        if (!active) {
            drone.cooldown = superShieldDroneCooldown(shields);
            drone.shieldGlow = -1.f;
            continue;
        }
        drone.currentSpeed = float(blueprint.droneSpeed);
        if (drone.cooldown < 1.5f && drone.shieldGlow <= 0.f) {
            drone.shieldGlow = 3.f;
            combatSound({"shieldDroneCharge"}, tick_);
        }
        if (drone.cooldown < 2.5f) {
            drone.currentSpeed *= std::max(drone.cooldown - 1.5f, 0.f);
        }
        if (drone.shieldGlow > 0.f) {
            drone.currentSpeed = 0.f;
        }
        drone.shieldGlow -= Time::step;
        if (drone.shieldGlow < 0.f) {
            drone.shieldGlow = -1.f;
        }
        if (drone.shotReady) {
            // Shields::AddSuperShield 0x60bbf0 caps at five layers.
            drone.shotReady = false;
            if (owner == 1 && enemy_.maxSuperShields == 0) {
                enemy_.maxSuperShields = 5;
            }
            shields = std::min(5, shields + 1);
            combatSound({"shieldDroneActivate"}, tick_);
            drone.cooldown = superShieldDroneCooldown(shields);
        }
    }
}

void Run::extractDroneProjectiles() {
    // SpaceManager41c940 drains each ship's weapons, then the asteroid queue,
    // then each space drone. GetNextProjectile428cb0 picks a target even when
    // bFire is false; projectile constructor and launch-sound RNG follow here.
    const auto registry = registeredSpaceDrones();
    for (const auto& [owner, actor] : registry) {
        auto& drone = *actor;
        const auto extract = [&]() -> bool {
            const auto& blueprint = catalog_.equipment(drone.blueprint);
            const bool repair = blueprint.type == "SHIP_REPAIR";
            const bool combat = blueprint.type == "COMBAT";
            const bool defense = blueprint.type == "DEFENSE";
            if (!repair && !combat && !defense) {
                return false;
            }
            if (defense) {
                advanceDefenseDroneAim(drone);
            }
            const auto& targetShip =
                repair ? (owner == 0 ? ship_ : enemy_.ship) : (owner == 0 ? enemy_.ship : ship_);
            // CombatDrone::HasTarget 0x41d910 equals its CanBeDeployed; a hull
            // repair drone always targets its own ship.
            const bool hasShipTarget = repair || droneCanBeDeployed(drone, owner);
            if ((repair || combat) && hasShipTarget && !targetShip.rooms.empty() &&
                !drone.hasAimTarget) {
                // CombatDrone::PickTarget 0x41f7d0 snapshots the normalised aim
                // that UpdateAimingAngle interpolates from.
                if (drone.aimAngle > 180.f) {
                    drone.aimAngle -= 360.f;
                } else if (drone.aimAngle < -180.f) {
                    drone.aimAngle += 360.f;
                }
                drone.lastAimingAngle = drone.aimAngle;
                drone.aimTarget =
                    center(targetShip.rooms[combatRandom_.next() % targetShip.rooms.size()]);
                drone.hasAimTarget = true;
            }
            if (!drone.powered || drone.controllerHackLevel > 1 || !drone.shotReady) {
                return false;
            }
            if (repair) {
                if (targetShip.rooms.empty()) {
                    return false;
                }
                // 41ec80 calls PickTarget first, then a second random point for
                //  DamageTarget(-1). The first retained target supplies the ray.
                (void)combatRandom_.next();
                drone.repairBeamTarget = drone.aimTarget;
                drone.repairBeamAge = .5f;
                auto& hull = owner == 0 ? hull_ : enemy_.hull;
                hull = std::min(targetShip.hull, hull + 1);
                drone.hasAimTarget = drone.shotReady = false;
                return false;
            }
            if (blueprint.droneWeapon.empty()) {
                return false;
            }
            const auto& weapon = catalog_.equipment(blueprint.droneWeapon);
            CombatProjectile shot;
            shot.owner = owner;
            shot.space = shot.targetShip = defense ? owner : 1 - owner;
            shot.weapon = weapon.id;
            shot.projectileImage = weapon.projectileImage;
            shot.position =
                shot.previous = {std::trunc(drone.position.x), std::trunc(drone.position.y)};
            if (defense) {
                if (!drone.hasAimTarget || drone.defenseDisrupted) {
                    return false;
                }
                shot.target = drone.aimTarget;
                if (drone.defenseCurrentTargetIsDrone) {
                    shot.defenseSpaceDroneId = drone.defenseCurrentTargetId;
                } else {
                    shot.defenseTargetId = drone.defenseCurrentTargetId;
                }
            } else {
                const bool dying = owner == 0 ? enemy_.hull <= 0 : hull_ <= 0;
                const bool jumping = owner == 0 ? enemy_.escapeJumpElapsed >= 0 : jumpTicks_ > 0;
                if (!hasShipTarget || !drone.hasAimTarget || cloaked(1 - owner) || dying ||
                    jumping) {
                    drone.hasAimTarget = drone.shotReady = false;
                    drone.cooldown = blueprint.cooldown;
                    return false;
                }
                shot.target = drone.aimTarget;
                const auto room = std::ranges::find_if(targetShip.rooms, [&](const auto& shape) {
                    return center(shape).x == shot.target.x && center(shape).y == shot.target.y;
                });
                shot.room = room == targetShip.rooms.end() ? -1 : room->id;
                if (weapon.type == "BEAM") {
                    shot.targetEnd = shot.target;
                    if (targetShip.rooms.size() > 1) {
                        do {
                            shot.targetEnd = center(
                                targetShip.rooms[combatRandom_.next() % targetShip.rooms.size()]);
                        } while (shot.targetEnd.x == shot.target.x &&
                                 shot.targetEnd.y == shot.target.y);
                    }
                    drone.additionalPause = .5f;
                    if (drone.bossSurge) {
                        ++drone.lifespan;
                    }
                }
            }
            shot.id = nextProjectile_++;
            const float dx = shot.target.x - shot.position.x;
            const float dy = shot.target.y - shot.position.y;
            const float distance = std::hypot(dx, dy);
            shot.direction =
                distance > 0 ? SDL_FPoint{dx / distance, dy / distance} : SDL_FPoint{1, 0};
            drone.aimTarget = shot.target;
            drone.targetProjectile = shot.defenseTargetId;
            drone.targetDrone = 0;
            drone.targetSpaceDroneId = shot.defenseSpaceDroneId;
            if (weapon.type == "BEAM") {
                const float shotAngle = std::atan2(dy, dx) * 180.f / std::numbers::pi_v<float>;
                initializeBeamProjectile(
                    shot, shot.position, shotAngle, true, weapon.projectileSpeed);
                // GetNextProjectile 0x428cb0 keeps the sweep and its
                // speed_magnitude for the drone's own aim.
                drone.beamCurrentTarget = shot.target;
                drone.beamFinalTarget = shot.targetEnd;
                drone.beamSpeed = shot.speed / 16.f;
            } else {
                shot.speed = (weapon.projectileSpeed > 0 ? weapon.projectileSpeed : 60.f) * 16.f;
            }
            initializeProjectileExplosion(shot, weapon);
            combatSound(weapon.launchSounds, 0, true);
            drone.cooldown = blueprint.cooldown;
            drone.hasAimTarget = drone.shotReady = false;
            projectiles_.push_back(std::move(shot));
            return true;
        };
        // SpaceManager41c940 keeps calling GetNextProjectile until it returns
        // null, so a drone that fired runs PickTarget again on the same frame.
        while (extract()) {
        }
        // Source41c940 offers every other registry actor after EACH extraction
        // attempt, including attempts producing no shot, for the next pass.
        for (const auto& [targetOwner, target] : registry) {
            if (target != actor) {
                offerDefenseDrone(drone, owner, *target, targetOwner);
            }
        }
    }
}

} // namespace ftl
