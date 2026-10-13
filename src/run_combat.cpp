#include "run.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <numbers>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>

namespace ftl {
namespace {
struct PhysicalDamageActor {
    RunCrew* crew = nullptr;
    CombatDrone* drone = nullptr;
    int owner = 0;
    std::uint64_t order = 0;
};

bool defenseAcceptsType(const EquipmentDefinition& blueprint, int type) {
    const int wanted = blueprint.droneTarget == "DRONES"     ? 3
                       : blueprint.droneTarget == "MISSILES" ? 1
                                                             : 4;
    return type == wanted || (type == 2 && (wanted == 1 || wanted == 4)) ||
           ((type == 1 || type == 6) && wanted == 4) || (type == 6 && (wanted == 1 || wanted == 3));
}

void acceptDefenseTarget(CombatDrone& drone,
                         const EquipmentDefinition& blueprint,
                         int type,
                         std::uint64_t id,
                         bool spaceDrone,
                         SDL_FPoint position,
                         SDL_FPoint velocity) {
    // DefenseDrone::SetWeaponTarget420e90 accepts later equal-priority
    // candidates. Missile1 and asteroid2 alone prevent lower-priority replacement.
    if ((id == drone.defenseShotTargetId && spaceDrone == drone.defenseShotTargetIsDrone &&
         blueprint.droneTarget != "DRONES") ||
        (drone.defenseTargetType == 1 && type != 1) ||
        (drone.defenseTargetType == 2 && type != 1 && type != 2)) {
        return;
    }
    if (id != drone.defenseCurrentTargetId || spaceDrone != drone.defenseCurrentTargetIsDrone) {
        if (drone.aimAngle > 180.f) {
            drone.aimAngle -= 360.f;
        } else if (drone.aimAngle < -180.f) {
            drone.aimAngle += 360.f;
        }
    }
    drone.aimTarget = position;
    drone.defenseTargetVelocity = velocity;
    drone.defenseCurrentTargetId = id;
    drone.defenseCurrentTargetIsDrone = spaceDrone;
    drone.defenseTargetType = type;
    drone.hasAimTarget = true;
}

SDL_FPoint defenseOrbitPoint(const ShipDefinition& ship, float angle) {
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
    const float centerY = float(top + bottom) / 2.f + ship.ellipse[3] + 110.f;
    const float radians = angle * std::numbers::pi_v<float> / 180.f;
    return {centerX + float(ship.ellipse[0]) * std::cos(radians),
            centerY + float(ship.ellipse[1]) * std::sin(radians)};
}

void placeDefenseDrone(CombatDrone& drone, const ShipDefinition& ship) {
    // DefenseDrone::PickDestination 0x421090 starts at angle zero on its
    // protected ship's ellipse, then advances in one-degree destinations.
    drone.orbitAngle = 0.f;
    drone.position = defenseOrbitPoint(ship, 0.f);
    drone.destination = defenseOrbitPoint(ship, 1.f);
    drone.velocity = {};
    drone.cooldown = 1.f;
    drone.orbitInitialized = true;
    drone.hasAimTarget = false;
}

RunSystem* findSystem(std::vector<RunSystem>& systems, int id) {
    for (auto& s : systems) {
        if (s.definition.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const RunSystem* findSystem(const std::vector<RunSystem>& systems, int id) {
    for (const auto& station : systems) {
        if (station.definition.id == id) {
            return &station;
        }
    }
    return nullptr;
}

const RoomDefinition& findRoom(const ShipDefinition& ship, int id) {
    for (const auto& r : ship.rooms) {
        if (r.id == id) {
            return r;
        }
    }
    throw std::runtime_error("invalid_combat_room");
}

std::vector<std::pair<int, std::string>> stationWalls(const ShipDefinition& ship,
                                                      const RoomDefinition& room) {
    std::vector<std::pair<int, std::string>> walls;
    const int columns = room.w / 35;
    const int rows = room.h / 35;
    const auto hasDoor = [&](int x, int y, bool vertical) {
        return std::any_of(ship.doors.begin(), ship.doors.end(), [&](const auto& door) {
            return door.vertical == vertical && door.x == x && door.y == y &&
                   (door.roomA == room.id || door.roomB == room.id);
        });
    };
    // Ship::ChoosePrimarySlot 0x487070 visits x, then y, and tests
    // down/up/right/left for a solid wall with no connected doorway.
    for (int col = 0; col < columns; ++col) {
        for (int row = 0; row < rows; ++row) {
            const int x = room.x + col * 35;
            const int y = room.y + row * 35;
            const int slot = row * columns + col;
            if (row == rows - 1 && !hasDoor(x, y + 35, false)) {
                walls.emplace_back(slot, "DOWN");
            }
            if (row == 0 && !hasDoor(x, y, false)) {
                walls.emplace_back(slot, "UP");
            }
            if (col == columns - 1 && !hasDoor(x + 35, y, true)) {
                walls.emplace_back(slot, "RIGHT");
            }
            if (col == 0 && !hasDoor(x, y, true)) {
                walls.emplace_back(slot, "LEFT");
            }
        }
    }
    return walls;
}

RunSystem& artilleryAt(std::vector<RunSystem>& systems, int index) {
    for (auto& system : systems) {
        if (system.definition.id == 11 && index-- == 0) {
            return system;
        }
    }
    throw std::runtime_error("invalid_artillery_index");
}

int crewSkillCap(const RunCrew& crew, int skill) {
    // CrewMember::CrewMember 0x47a400 lowers all six human skill caps.
    constexpr std::array<int, 6> human{26, 26, 100, 116, 32, 14};
    constexpr std::array<int, 6> generic{30, 30, 110, 130, 36, 16};
    return (crew.appearance.species == "human" ? human : generic).at(std::size_t(skill));
}

int skillTier(int progress, int cap) {
    return progress >= cap ? 2 : progress >= cap / 2 ? 1 : 0;
}

int weaponSkillCap(const RunCrew& crew) {
    return crewSkillCap(crew, 3);
}

int weaponSkillTier(const RunCrew& crew) {
    return skillTier(crew.weaponSkillProgress, weaponSkillCap(crew));
}

int repairSkillCap(const RunCrew& crew) {
    return crewSkillCap(crew, 4);
}

int repairSkillTier(const RunCrew& crew) {
    return skillTier(crew.repairSkillProgress, repairSkillCap(crew));
}

void grantManningSkill(std::vector<RunCrew>& crew, const RunCrew* actor, int skill, Audio& audio) {
    if (!actor) {
        return;
    }
    for (auto& member : crew) {
        if (&member != actor) {
            continue;
        }
        const auto index = std::size_t(skill);
        const int previousTier = member.manningSkill[index];
        // CrewMember::IncreaseSkill 0x46e3f0 starts skillUp[0][skill] for
        // every point gained below the maximum.
        if (member.manningSkillProgress[index] < crewSkillCap(member, skill)) {
            member.skillUpRemaining[index] = 1.f;
        }
        member.manningSkillProgress[index] =
            std::min(crewSkillCap(member, skill), member.manningSkillProgress[index] + 1);
        member.manningSkill[index] =
            skillTier(member.manningSkillProgress[index], crewSkillCap(member, skill));
        recordEarnedSkill(member.earnedSkillTiers, skill, previousTier, member.manningSkill[index]);
        if (member.manningSkill[index] > previousTier) {
            audio.sound("levelup");
        }
        break;
    }
}

SDL_FPoint center(const RoomDefinition& room) {
    // ShipGraph::GetRoomCenter 0x45b020 halves the integer rectangle.
    return {float(room.x + room.w / 2), float(room.y + room.h / 2)};
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
    const float x = (point.x - (left + right) / 2.f - ship.ellipse[2]) / ship.ellipse[0];
    // Ship::GetBaseEllipse0x480be0 offsets the enemy graph center by 110 pixels.
    const float y =
        (point.y - (top + bottom) / 2 - ship.ellipse[3] - (enemy ? 110 : 0)) / ship.ellipse[1];
    return x * x + y * y < 1.f;
}

std::optional<SDL_FPoint>
beamShieldEntry(const ShipDefinition& ship, SDL_FPoint start, SDL_FPoint finish, bool enemy) {
    // CollisionMoving499990 raytraces in five-pixel increments and rejects
    // the first increment whose distance to finish is greater than the prior.
    // CollisionShield498830 admits only an outside-to-inside transition.
    if (insideShield(ship, start, enemy)) {
        return std::nullopt;
    }
    float distance = std::hypot(finish.x - start.x, finish.y - start.y);
    if (distance == 0) {
        return std::nullopt;
    }
    const SDL_FPoint direction{(finish.x - start.x) * 5.f / distance,
                               (finish.y - start.y) * 5.f / distance};
    auto current = start;
    for (;;) {
        current.x += direction.x;
        current.y += direction.y;
        const float remaining = std::hypot(finish.x - current.x, finish.y - current.y);
        if (remaining > distance) {
            return std::nullopt;
        }
        distance = remaining;
        if (insideShield(ship, current, enemy)) {
            return current;
        }
    }
}
} // namespace

void Run::combatExperience(RunCrew& crew) {
    // CrewMember::IncreaseSkill 0x46e3f0 returns for a drone or for any member
    // whose iShipId is not the player's: enemy crew never gain skill points.
    const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& member) {
        return &member == &crew;
    });
    if (!playerOwned) {
        return;
    }
    const int cap = combatSkillCap(crew.appearance.species);
    const int previousTier = crew.combatSkill;
    if (crew.combatSkillProgress < cap) {
        crew.skillUpRemaining[5] = 1.f;
    }
    crew.combatSkillProgress = std::min(cap, crew.combatSkillProgress + 1);
    crew.combatSkill = combatSkillTier(crew.combatSkillProgress, cap);
    recordEarnedSkill(crew.earnedSkillTiers, 5, previousTier, crew.combatSkill);
    if (crew.combatSkill > previousTier) {
        audio_.sound("levelup");
    }
}

void Run::noteCrewHealthChange(const RunCrew& member, float previous) {
    // DirectModifyHealth46eb20: integer threshold crossing for player-owned
    // non-drone crew, including damage that kills them in the same operation.
    if (int(previous) > 25 && int(member.health) <= 25 &&
        std::ranges::any_of(crew_, [&](const auto& owned) {
            return &owned == &member;
        })) {
        audio_.sound("lowCrewHealth");
    }
    // DirectModifyHealth46eb20 raises ACH_ROCK_FIRE for any health change that
    // kills a burning (iOnFire) enemy-owned, non-drone member aboard its own ship
    // while it is fighting (bFighting) a crew target owned by the player. The
    // cause of death and the killer's species are not checked; SetAchievement
    // applies the Rock-cruiser ship gate.
    if (previous > 0 && member.health <= 0 && member.onFire > 0 && !member.intruder &&
        !member.attackAnimation.empty() && member.attackTargetOwner == 0 &&
        member.appearance.species != "battle" && member.appearance.species != "repair" &&
        member.appearance.species != "boarder_ion" &&
        std::ranges::any_of(enemy_.crew, [&](const auto& owned) {
            return &owned == &member;
        })) {
        progressionRun_.tutorial = tutorialOwned_;
        progression_.condition("ACH_ROCK_FIRE", true, progressionRun_);
    }
}

void initializeEnemySystemStation(SystemDefinition& definition,
                                  const ShipDefinition& ship,
                                  int artilleryOrdinal,
                                  RandomStream& random) {
    // AddSystem49ca00 chooses a primary slot only for a boostable system
    // whose template omits its slot. Explicit slots consume no selection draw.
    const bool boostable = definition.id == 0 || definition.id == 1 || definition.id == 3 ||
                           definition.id == 6 || definition.id == 7 || definition.id == 8 ||
                           definition.id == 11;
    if (!boostable) {
        // Nonboostable medical fixtures retain their authored reservation
        // slot even though AddSystem skips SetPrimarySlot.
        definition.direction = "NONE";
    } else if (definition.slot == -1) {
        if (definition.id == 6 || definition.id == 11) {
            definition.slot = definition.id == 11 && artilleryOrdinal == 3 ? 1 : 0;
            definition.direction = "UP";
        } else {
            const auto walls = stationWalls(ship, findRoom(ship, definition.room));
            if (!walls.empty()) {
                const auto& chosen = walls[random.next() % walls.size()];
                definition.slot = chosen.first;
                definition.direction = chosen.second;
            }
        }
    }
}

std::vector<int> initialEnemyCrewRooms(const ShipDefinition& ship, int difficulty) {
    // Enemy AddInitialCrew4a3610 inserts two artillery priorities after
    // piloting, then one at the end. Each consumes the next artillery room;
    // an exhausted artillery list gives SetRoom its default room (-1).
    const std::array<int, 15> order =
        difficulty == 2 ? std::array<int, 15>{6, 11, 11, 1, 3, 0, 2, 7, 8, 10, 5, 4, 15, 9, 11}
                        : std::array<int, 15>{6, 11, 11, 0, 3, 1, 2, 7, 8, 10, 5, 4, 15, 9, 11};
    std::vector<int> artilleryRooms;
    for (const auto& system : ship.systems) {
        if (system.id == 11 && system.installed) {
            artilleryRooms.push_back(system.room);
        }
    }
    std::vector<int> rooms;
    std::size_t artillery = 0;
    for (const int id : order) {
        const auto system = std::ranges::find_if(ship.systems, [&](const auto& definition) {
            return definition.id == id && definition.installed;
        });
        if (system == ship.systems.end()) {
            continue;
        }
        rooms.push_back(id == 11
                            ? artillery < artilleryRooms.size() ? artilleryRooms[artillery++] : -1
                            : system->room);
    }
    return rooms;
}

float Run::enemyOriginX() const {
    int left = enemy_.ship.rooms.front().x;
    int right = left;
    for (const auto& room : enemy_.ship.rooms) {
        left = std::min(left, room.x);
        right = std::max(right, room.x + room.w);
    }
    // CombatControl::AddEnemyShip 0x5638a0 centers the room-bound shipBox
    // horizontally in the target panel, using integer width division.
    const bool boss =
        campaign_.fighting || enemy_.ship.id == "BOSS_SPECIAL" || enemy_.ship.id == "BOSS_DEMO";
    return float((boss ? 1016 : 1075) - (right - left) / 2);
}

float Run::enemyOriginY() const {
    // The same function adds the layout's VERTICAL offset to the enemy
    // translation. REBEL_SKINNY is -25, AUTO_ASSAULT is +100.
    const bool boss =
        campaign_.fighting || enemy_.ship.id == "BOSS_SPECIAL" || enemy_.ship.id == "BOSS_DEMO";
    return boss ? 127.f : float(130 + enemy_.ship.verticalShift);
}

void Run::loadCombatSnapshot(const nlohmann::json& values) {
    if (!enemy_.present || !values.is_object()) {
        throw std::runtime_error("combat_fixture_required");
    }
    const auto fields = [](const auto& object, std::initializer_list<std::string_view> allowed) {
        if (!object.is_object()) {
            throw std::runtime_error("invalid_combat_object");
        }
        for (const auto& [key, value] : object.items()) {
            if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
                throw std::runtime_error("unknown_combat_field");
            }
            if (value.is_number() && key != "repair" && key != "health" && key != "value" &&
                key != "ion_timer" && key != "stun_time" && !value.is_number_integer()) {
                throw std::runtime_error("invalid_combat_integer");
            }
        }
    };
    fields(values,
           {"hull",
            "missiles",
            "shields",
            "super_shields",
            "enemy_hull",
            "enemy_missiles",
            "enemy_shields",
            "enemy_super_shields",
            "systems",
            "enemy_systems",
            "crew",
            "enemy_crew",
            "room_oxygen",
            "weapon_ids",
            "drone_ids",
            "drone_ion_stun",
            "drone_dodge",
            "enemy_drone_ids",
            "enemy_weapon_ids",
            "weapon_power",
            "weapon_desired",
            "augments",
            "enemy_augments",
            "cloak",
            "battery",
            "enemy_cloak",
            "enemy_battery"});
    auto ship = ship_;
    auto playerSystems = systems_;
    auto playerCrew = crew_;
    auto roomOxygen = roomOxygen_;
    auto enemy = enemy_;
    auto playerWeaponPower = weaponPower_;
    auto playerWeaponDesired = weaponDesired_;
    auto playerDrones = combatDrones_;
    auto playerFactories = combatWeapons_;
    auto cloak = cloak_;
    auto battery = battery_;
    const auto bounded = [](int value, int minimum, int maximum) {
        if (value < minimum || value > maximum) {
            throw std::runtime_error("invalid_combat_snapshot");
        }
        return value;
    };
    if (values.contains("weapon_ids")) {
        const auto& ids = values.at("weapon_ids");
        if (!ids.is_array() || ids.size() != ship.weapons.size() ||
            std::any_of(weaponPower_.begin(),
                        weaponPower_.end(),
                        [](bool on) {
                            return on;
                        }) ||
            !projectiles_.empty()) {
            throw std::runtime_error("invalid_combat_weapon_fixture");
        }
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (!ids[index].is_string()) {
                throw std::runtime_error("invalid_combat_weapon_fixture");
            }
            const std::string id = ids[index].get<std::string>();
            const auto& blueprint = catalog_.equipment(id);
            if (blueprint.kind != "weapon") {
                throw std::runtime_error("invalid_combat_weapon_fixture");
            }
            ship.weapons[index] = id;
            // The slot now holds a newly built ProjectileFactory (ctor 0x40b380) whose
            // WeaponAnimation starts over; the fixture keeps the old goal charge (clamped
            // to the new levels) rather than repeating the constructor's draw.
            auto& factory = playerFactories.at(index);
            const int goal = factory.goalChargeLevel;
            factory = CombatWeapon{};
            factory.goalChargeLevel = std::clamp(goal, 1, std::max(1, blueprint.chargeLevels));
            factory.weaponExplosionImage = blueprint.weaponExplosionAnimation;
        }
    }
    if (values.contains("drone_ids")) {
        const auto& ids = values.at("drone_ids");
        if (!ids.is_array() || ids.size() != ship.drones.size() ||
            ids.size() != playerDrones.size() || !projectiles_.empty() ||
            std::any_of(playerDrones.begin(), playerDrones.end(), [](const auto& drone) {
                return drone.deployed || drone.powered;
            })) {
            throw std::runtime_error("invalid_combat_drone_fixture");
        }
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (!ids[index].is_string()) {
                throw std::runtime_error("invalid_combat_drone_fixture");
            }
            const std::string id = ids[index].get<std::string>();
            if (catalog_.equipment(id).kind != "drone") {
                throw std::runtime_error("invalid_combat_drone_fixture");
            }
            ship.drones[index] = id;
            auto& drone = playerDrones[index];
            drone.blueprint = id;
            // The swapped slot keeps its identities only where the new type
            // uses them: crew bodies and boarding pods carry a crew actor id.
            const auto& type = catalog_.equipment(id).type;
            if (type != "REPAIR" && type != "BATTLE" && type != "BOARDER") {
                drone.crewActorId = 0;
                drone.physicalCrewOrder = 0;
                drone.crewPhysicalShip = drone.crewRoom = drone.crewSlot = -1;
                drone.crewDying = drone.crewDeathDone = false;
                drone.crewDeathElapsed = 0;
            }
            // ... and a space actor identity only for a space drone, so a
            // crew body leaves the SpaceManager registry.
            if (type != "DEFENSE" && type != "COMBAT" && type != "BOARDER" &&
                type != "SHIP_REPAIR" && type != "SHIELD" && type != "HACKING") {
                drone.spaceActorId = drone.spaceRegisteredOrder = 0;
                drone.spacePendingArrival = false;
            }
            assignCrewDroneId(drone);
            assignSpaceDroneId(drone);
            if (type == "REPAIR" || type == "BATTLE") {
                // As initializeCrewDroneAnimation: an interior body belongs to
                // its owner's ship, unplaced until the first deployment.
                assignPhysicalCrewOrder(drone);
                drone.crewPhysicalShip = 0;
                drone.crewRoom = drone.crewSlot = -1;
                drone.crewPosition = {};
            } else if (type == "BOARDER") {
                // A pod launches from its controller room (no random draw).
                initializeDroneLocation(drone, ship, 0, catalog_, combatRandom_);
            }
            // The slot now holds a newly built drone of that type: the CrewDrone ctor
            // 0x438130 gives crew bodies 25 (repair), 125 (ion boarder) or 150 health
            // with bDead set; space drones receive theirs when deployed.
            const bool crewBody = type == "REPAIR" || type == "BATTLE" || type == "BOARDER";
            drone.health = type == "REPAIR"      ? 25.f
                           : id == "BOARDER_ION" ? 125.f
                           : crewBody            ? 150.f
                                                 : 0.f;
            if (crewBody) {
                drone.crewDead = true;
            }
        }
    }
    if (values.contains("drone_ion_stun")) {
        const auto& durations = values.at("drone_ion_stun");
        if (!durations.is_array() || durations.size() != playerDrones.size()) {
            throw std::runtime_error("invalid_combat_drone_stun_fixture");
        }
        for (std::size_t index = 0; index < playerDrones.size(); ++index) {
            if (!durations[index].is_number()) {
                throw std::runtime_error("invalid_combat_drone_stun_fixture");
            }
            const float duration = durations[index].get<float>();
            if (!std::isfinite(duration) || duration < 0.f || duration > 5.f) {
                throw std::runtime_error("invalid_combat_drone_stun_fixture");
            }
            playerDrones[index].ionStun = duration;
        }
    }
    if (values.contains("drone_dodge")) {
        // No archived drone blueprint authors a non-zero dodge, so the
        // powered-dodge collision path in SpaceDrone::CollisionMoving 0x41e400
        // needs an authored fixture value. This override is diagnostic state and
        // is deliberately not serialized.
        const auto& dodges = values.at("drone_dodge");
        if (!dodges.is_array() || dodges.size() != playerDrones.size()) {
            throw std::runtime_error("invalid_combat_drone_dodge_fixture");
        }
        for (std::size_t index = 0; index < playerDrones.size(); ++index) {
            if (!dodges[index].is_number_integer()) {
                throw std::runtime_error("invalid_combat_drone_dodge_fixture");
            }
            const int dodge = dodges[index].get<int>();
            if (dodge < 0 || dodge > 10) {
                throw std::runtime_error("invalid_combat_drone_dodge_fixture");
            }
            playerDrones[index].diagnosticDodge = dodge;
        }
    }
    if (values.contains("enemy_drone_ids")) {
        const auto& ids = values.at("enemy_drone_ids");
        const auto* droneSystem = findSystem(enemy.systems, 4);
        const bool replace = !enemy.drones.empty();
        if (!ids.is_array() || ids.size() > 2 || (replace && ids.size() != enemy.drones.size()) ||
            !projectiles_.empty() || !droneSystem || droneSystem->health == 0) {
            throw std::runtime_error("invalid_combat_enemy_drone_fixture");
        }
        int requiredPower = 0;
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (!ids[index].is_string()) {
                throw std::runtime_error("invalid_combat_enemy_drone_fixture");
            }
            const auto id = ids[index].get<std::string>();
            const auto& blueprint = catalog_.equipment(id);
            if (blueprint.kind != "drone" ||
                (blueprint.type != "COMBAT" && blueprint.type != "DEFENSE" &&
                 blueprint.type != "REPAIR" && blueprint.type != "BATTLE" &&
                 blueprint.type != "BOARDER" && blueprint.type != "SHIELD" &&
                 blueprint.type != "SHIP_REPAIR")) {
                throw std::runtime_error("invalid_combat_enemy_drone_fixture");
            }
            requiredPower += blueprint.power;
            if (requiredPower > droneSystem->power) {
                throw std::runtime_error("invalid_combat_enemy_drone_power");
            }
            if (replace) {
                enemy.drones[index].blueprint = id;
                initializeCrewDroneAnimation(enemy.drones[index], combatRandom_, &enemy.ship, 1);
                enemy.drones[index].space = blueprint.type == "COMBAT" ? 0 : 1;
                enemy.drones[index].health =
                    blueprint.id == "BOARDER_ION"                                 ? 125.f
                    : blueprint.type == "REPAIR"                                  ? 25.f
                    : (blueprint.type == "BATTLE" || blueprint.type == "BOARDER") ? 150.f
                                                                                  : 1.f;
                enemy.drones[index].room = enemy.drones[index].targetRoom = -1;
                enemy.drones[index].delivered = false;
                enemy.ship.drones[index] = id;
                if (blueprint.type == "DEFENSE") {
                    placeDefenseDrone(enemy.drones[index], enemy.ship);
                }
            } else {
                CombatDrone drone;
                drone.blueprint = id;
                initializeCrewDroneAnimation(drone, combatRandom_, &enemy.ship, 1);
                drone.space = blueprint.type == "COMBAT" ? 0 : 1;
                drone.health = blueprint.id == "BOARDER_ION"                                 ? 125.f
                               : blueprint.type == "REPAIR"                                  ? 25.f
                               : (blueprint.type == "BATTLE" || blueprint.type == "BOARDER") ? 150.f
                                                                                             : 1.f;
                drone.deployed = drone.powered = true;
                drone.crewDead = false;
                if (blueprint.type == "DEFENSE") {
                    placeDefenseDrone(drone, enemy.ship);
                } else {
                    drone.position = {
                        float(ship.imageRect[0] + ship.imageRect[2] + 75 + index * 25),
                        float(ship.imageRect[1] + ship.imageRect[3] / 2)};
                }
                enemy.drones.push_back(std::move(drone));
                enemy.ship.drones.push_back(id);
            }
        }
    }
    if (values.contains("enemy_weapon_ids")) {
        const auto& ids = values.at("enemy_weapon_ids");
        if (!ids.is_array() || ids.size() != enemy.ship.weapons.size() || !projectiles_.empty()) {
            throw std::runtime_error("invalid_combat_enemy_weapon_fixture");
        }
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (!ids[index].is_string()) {
                throw std::runtime_error("invalid_combat_enemy_weapon_fixture");
            }
            const std::string id = ids[index].get<std::string>();
            if (catalog_.equipment(id).kind != "weapon") {
                throw std::runtime_error("invalid_combat_enemy_weapon_fixture");
            }
            enemy.ship.weapons[index] = id;
        }
    }
    if (values.contains("augments")) {
        const auto& ids = values.at("augments");
        if (!ids.is_array() || ids.size() > 3) {
            throw std::runtime_error("invalid_combat_augments");
        }
        ship.augments.clear();
        for (const auto& id : ids) {
            if (!id.is_string()) {
                throw std::runtime_error("invalid_combat_augments");
            }
            const auto name = id.get<std::string>();
            if (catalog_.equipment(name).kind != "aug") {
                throw std::runtime_error("invalid_combat_augments");
            }
            ship.augments.push_back(name);
        }
    }
    if (values.contains("enemy_augments")) {
        const auto& ids = values.at("enemy_augments");
        if (!ids.is_array() || ids.size() > 3) {
            throw std::runtime_error("invalid_combat_augments");
        }
        enemy.ship.augments.clear();
        for (const auto& id : ids) {
            if (!id.is_string()) {
                throw std::runtime_error("invalid_combat_augments");
            }
            const auto name = id.get<std::string>();
            if (catalog_.equipment(name).kind != "aug") {
                throw std::runtime_error("invalid_combat_augments");
            }
            enemy.ship.augments.push_back(name);
        }
    }
    for (const auto* key : {"weapon_power", "weapon_desired"}) {
        if (!values.contains(key)) {
            continue;
        }
        const auto& entries = values.at(key);
        if (!entries.is_array() || entries.size() != playerWeaponPower.size()) {
            throw std::runtime_error("invalid_combat_weapon_power");
        }
        auto& target =
            std::string_view(key) == "weapon_power" ? playerWeaponPower : playerWeaponDesired;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].is_boolean()) {
                throw std::runtime_error("invalid_combat_weapon_power");
            }
            target[i] = entries[i].get<bool>();
        }
    }
    const int hull = bounded(values.value("hull", hull_), 0, ship_.hull);
    const int missiles = bounded(values.value("missiles", missiles_), 0, 999);
    const int shields = bounded(values.value("shields", shieldBubbles_), 0, 4);
    const int superShields = bounded(values.value("super_shields", superShields_), 0, 5);
    enemy.hull = bounded(values.value("enemy_hull", enemy.hull), 0, enemy.ship.hull);
    if (values.contains("enemy_missiles")) {
        enemy.missiles = bounded(values.at("enemy_missiles").get<int>(), 0, 999);
    }
    enemy.shields = bounded(values.value("enemy_shields", enemy.shields), 0, 4);
    enemy.superShields = bounded(values.value("enemy_super_shields", enemy.superShields),
                                 0,
                                 campaign_.fighting && campaign_.stage == 3 ? 12 : 5);
    // An enemy reaches super layers only through ENERGY_SHIELD or a shield
    // drone (maximum 5, SuperShieldDrone::OnLoop 0x426db0) or the flagship's
    // third stage (12, Shields::PowerSuperShields 0x60bc70); the fixture
    // takes the matching maximum rather than inventing its own.
    if (enemy.superShields > enemy.maxSuperShields) {
        enemy.maxSuperShields = enemy.superShields > 5 ? 12 : 5;
    }
    auto systems = [&](const char* key, auto& destination, ShipDefinition& owner) {
        if (!values.contains(key)) {
            return;
        }
        const auto& rows = values.at(key);
        if (!rows.is_array() || rows.size() > destination.size()) {
            throw std::runtime_error("invalid_combat_systems");
        }
        std::set<int> ids;
        for (const auto& row : rows) {
            fields(row,
                   {"id",
                    "capacity",
                    "health",
                    "power",
                    "battery_power",
                    "restore_power",
                    "repair",
                    "ion",
                    "ion_timer",
                    "lock_count",
                    "lock_elapsed",
                    "hack_level",
                    "hack_active"});
            if (!row.at("id").is_number_integer() ||
                (row.contains("health") && !row.at("health").is_number_integer()) ||
                !ids.insert(row.at("id").template get<int>()).second) {
                throw std::runtime_error("invalid_combat_system_id_or_health");
            }
            auto* s = findSystem(destination, row.at("id").template get<int>());
            if (!s) {
                throw std::runtime_error("invalid_combat_system");
            }
            if (row.contains("capacity")) {
                if ((s->definition.id != 0 && s->definition.id != 3 && s->definition.id != 4) ||
                    !row.at("capacity").is_number_integer()) {
                    throw std::runtime_error("invalid_combat_system_capacity");
                }
                const int capacity =
                    bounded(row.at("capacity").get<int>(), s->definition.id == 0 ? 2 : 1, 8);
                s->definition.power = capacity;
                auto definition = std::find_if(
                    owner.systems.begin(), owner.systems.end(), [&](const auto& entry) {
                        return entry.id == s->definition.id;
                    });
                if (definition != owner.systems.end()) {
                    definition->power = capacity;
                }
            }
            s->health = bounded(row.value("health", s->health), 0, s->definition.power);
            s->power = bounded(row.value("power", std::min(s->power, s->health)), 0, s->health);
            s->batteryPower = bounded(
                row.value("battery_power", std::min(s->batteryPower, s->power)), 0, s->power);
            s->restorePower =
                bounded(row.value("restore_power", s->restorePower), 0, s->definition.power);
            s->repair = row.value("repair", s->repair);
            const float previousIon = s->ion;
            s->ion = row.value("ion", s->ion);
            if (row.contains("ion_timer")) {
                s->ionTimer = row.at("ion_timer").get<float>();
            } else if (row.contains("ion")) {
                s->ionTimer = s->ion == 0 ? 0.f : previousIon == 0 ? 5.f : s->ionTimer;
            }
            s->hackLevel = bounded(row.value("hack_level", s->hackLevel), 0, 3);
            if (row.contains("hack_active") && !row.at("hack_active").is_boolean()) {
                throw std::runtime_error("invalid_combat_hack");
            }
            s->hackActive = row.value("hack_active", s->hackActive);
            if (!std::isfinite(s->repair) || s->repair < 0 || s->repair >= 100) {
                throw std::runtime_error("invalid_repair_progress");
            }
            if (!std::isfinite(s->ion) || s->ion < 0 ||
                double(s->ion) > std::numeric_limits<int>::max() || std::floor(s->ion) != s->ion ||
                !std::isfinite(s->ionTimer) || s->ionTimer < 0 || s->ionTimer > 5 ||
                (s->ion == 0 && s->ionTimer != 0) || (s->ion > 0 && s->ionTimer < 0)) {
                throw std::runtime_error("invalid_ion_lock");
            }
            if (row.contains("lock_count") != row.contains("lock_elapsed")) {
                throw std::runtime_error("invalid_shared_system_lock");
            }
            if (row.contains("lock_count")) {
                nativeRead(row.at("lock_count"), s->lockCount);
                nativeRead(row.at("lock_elapsed"), s->lockElapsed);
            } else if (row.contains("ion") || row.contains("ion_timer")) {
                s->lockCount = int(s->ion);
                s->lockElapsed = s->ion > 0 ? 5.f - s->ionTimer : 0.f;
            }
            if (s->lockCount < -1 || !std::isfinite(s->lockElapsed) || s->lockElapsed < 0 ||
                s->lockElapsed > 5) {
                throw std::runtime_error("invalid_shared_system_lock");
            }
            s->ion = float(std::max(0, s->lockCount));
            s->ionTimer = s->lockCount > 0 ? 5.f - s->lockElapsed : 0.f;
        }
    };
    systems("systems", playerSystems, ship);
    systems("enemy_systems", enemy.systems, enemy.ship);
    if (values.contains("enemy_systems")) {
        enemy.powerFixtureOverride =
            enemy.powerFixtureOverride ||
            std::ranges::any_of(values.at("enemy_systems"), [](const auto& row) {
                return row.contains("power");
            });
    }
    auto crew = [&](const char* key, auto& destination, const ShipDefinition& ship) {
        if (!values.contains(key)) {
            return;
        }
        const auto& rows = values.at(key);
        if (!rows.is_array() || rows.size() > destination.size()) {
            throw std::runtime_error("invalid_combat_crew");
        }
        std::set<std::size_t> ids;
        for (const auto& row : rows) {
            fields(row,
                   {"id",
                    "health",
                    "room",
                    "slot",
                    "stun_time",
                    "species",
                    "weapon_skill",
                    "weapon_skill_progress",
                    "combat_skill",
                    "combat_skill_progress",
                    "repair_skill",
                    "repair_skill_progress",
                    "pilot_skill",
                    "pilot_skill_progress",
                    "engine_skill",
                    "engine_skill_progress",
                    "shield_skill",
                    "shield_skill_progress"});
            if (!row.at("id").is_number_integer() ||
                !ids.insert(row.at("id").template get<std::size_t>()).second) {
                throw std::runtime_error("invalid_combat_crew_id");
            }
            auto& c = destination.at(row.at("id").template get<std::size_t>());
            c.health = row.value("health", c.health);
            c.stunTime = row.value("stun_time", c.stunTime);
            if (!std::isfinite(c.stunTime) || c.stunTime < 0 || c.stunTime > 60) {
                throw std::runtime_error("invalid_combat_stun");
            }
            if (row.contains("species")) {
                const auto species = row.at("species").get<std::string>();
                if (species != "human" && species != "engi" && species != "mantis" &&
                    species != "rock" && species != "crystal" && species != "energy" &&
                    species != "anaerobic" && species != "slug") {
                    throw std::runtime_error("invalid_combat_species");
                }
                c.appearance.species = species;
            }
            if (!std::isfinite(c.health) || c.health < 0 ||
                c.health > crewMaximumHealth(c.appearance.species) + c.healthBoost) {
                throw std::runtime_error("invalid_combat_health");
            }
            if (row.contains("weapon_skill_progress")) {
                c.weaponSkillProgress =
                    bounded(row.at("weapon_skill_progress").get<int>(), 0, weaponSkillCap(c));
                c.weaponSkill = weaponSkillTier(c);
            } else if (row.contains("weapon_skill")) {
                c.weaponSkill = bounded(row.at("weapon_skill").get<int>(), 0, 2);
                c.weaponSkillProgress = c.weaponSkill == 2   ? weaponSkillCap(c)
                                        : c.weaponSkill == 1 ? weaponSkillCap(c) / 2
                                                             : 0;
            } else if (row.contains("species")) {
                c.weaponSkillProgress = std::min(c.weaponSkillProgress, weaponSkillCap(c));
                c.weaponSkill = weaponSkillTier(c);
            }
            if (row.contains("repair_skill_progress")) {
                c.repairSkillProgress =
                    bounded(row.at("repair_skill_progress").get<int>(), 0, repairSkillCap(c));
                c.repairSkill = repairSkillTier(c);
            } else if (row.contains("repair_skill")) {
                c.repairSkill = bounded(row.at("repair_skill").get<int>(), 0, 2);
                c.repairSkillProgress = c.repairSkill == 2   ? repairSkillCap(c)
                                        : c.repairSkill == 1 ? repairSkillCap(c) / 2
                                                             : 0;
            } else if (row.contains("species")) {
                c.repairSkillProgress = std::min(c.repairSkillProgress, repairSkillCap(c));
                c.repairSkill = repairSkillTier(c);
            }
            constexpr std::array<std::string_view, 3> skillNames{"pilot", "engine", "shield"};
            for (int skill = 0; skill < 3; ++skill) {
                const auto index = std::size_t(skill);
                const std::string levelKey = std::string(skillNames[index]) + "_skill";
                const std::string progressKey = levelKey + "_progress";
                if (row.contains(progressKey)) {
                    c.manningSkillProgress[index] =
                        bounded(row.at(progressKey).get<int>(), 0, crewSkillCap(c, skill));
                    c.manningSkill[index] =
                        skillTier(c.manningSkillProgress[index], crewSkillCap(c, skill));
                } else if (row.contains(levelKey)) {
                    c.manningSkill[index] = bounded(row.at(levelKey).get<int>(), 0, 2);
                    c.manningSkillProgress[index] =
                        c.manningSkill[index] == 2   ? crewSkillCap(c, skill)
                        : c.manningSkill[index] == 1 ? crewSkillCap(c, skill) / 2
                                                     : 0;
                } else if (row.contains("species")) {
                    c.manningSkillProgress[index] =
                        std::min(c.manningSkillProgress[index], crewSkillCap(c, skill));
                    c.manningSkill[index] =
                        skillTier(c.manningSkillProgress[index], crewSkillCap(c, skill));
                }
            }
            const int combatCap = crewSkillCap(c, 5);
            if (row.contains("combat_skill_progress")) {
                c.combatSkillProgress =
                    bounded(row.at("combat_skill_progress").get<int>(), 0, combatCap);
                c.combatSkill = combatSkillTier(c.combatSkillProgress, combatCap);
            } else if (row.contains("combat_skill")) {
                c.combatSkill = bounded(row.at("combat_skill").get<int>(), 0, 2);
                c.combatSkillProgress = c.combatSkill == 2   ? combatCap
                                        : c.combatSkill == 1 ? combatCap / 2
                                                             : 0;
            } else if (row.contains("species")) {
                c.combatSkillProgress = std::min(c.combatSkillProgress, combatCap);
                c.combatSkill = combatSkillTier(c.combatSkillProgress, combatCap);
            }
            const int previousRoom = c.room;
            c.room = c.targetRoom = row.value("room", c.room);
            const auto& r = findRoom(ship, c.room);
            // A fixture that moves a member without naming a slot takes the
            // new room's first slot rather than the old room's slot index.
            const int defaultSlot = c.room == previousRoom ? c.slot : 0;
            c.slot = c.targetSlot =
                bounded(row.value("slot", defaultSlot), 0, r.w / 35 * (r.h / 35) - 1);
            c.x = float(r.x + c.slot % (r.w / 35) * 35 + 17);
            c.y = float(r.y + c.slot / (r.w / 35) * 35 + 17);
            c.path.clear();
            c.selected = false;
        }
    };
    crew("crew", playerCrew, ship);
    crew("enemy_crew", enemy.crew, enemy.ship);
    if (values.contains("room_oxygen")) {
        const auto& rows = values.at("room_oxygen");
        if (!rows.is_array() || rows.size() > roomOxygen.size()) {
            throw std::runtime_error("invalid_combat_oxygen");
        }
        std::set<int> ids;
        for (const auto& row : rows) {
            fields(row, {"id", "value"});
            const int id = row.at("id").get<int>();
            const float value = row.at("value").get<float>();
            if (!ids.insert(id).second || !roomOxygen.contains(id) || !std::isfinite(value) ||
                value > 100) {
                // UpdateEnvironment leaves rooms below zero until the next
                // OxygenSystem::OnLoop clamp, so a mid-combat state may hold them.
                throw std::runtime_error("invalid_combat_oxygen");
            }
            roomOxygen[id] = value;
        }
    }
    const auto* playerShields = findSystem(playerSystems, 0);
    const auto* enemyShields = findSystem(enemy.systems, 0);
    bounded(shields, 0, playerShields ? playerShields->power / 2 : 0);
    bounded(enemy.shields, 0, enemyShields ? enemyShields->power / 2 : 0);
    const auto reactor = [&](const auto& entries, int capacity) {
        int used = 0;
        for (const auto& s : entries) {
            const int id = s.definition.id;
            if ((id < 6 || id > 8) && id != 12) {
                used += std::max(0, s.power - s.bonusPower);
            }
        }
        bounded(used, 0, capacity);
    };
    const auto loadAbility = [&](const char* name,
                                 CombatAbility& ability,
                                 std::vector<RunSystem>& entries,
                                 int id) {
        if (values.contains(name)) {
            const auto& row = values.at(name);
            if (!row.is_object()) {
                throw std::runtime_error("invalid_combat_ability");
            }
            for (const auto& [key, ignored] : row.items()) {
                (void)ignored;
                if (key != "active" && key != "elapsed" && key != "cooldown") {
                    throw std::runtime_error("unknown_combat_ability_field");
                }
            }
            ability.active = row.value("active", ability.active);
            ability.elapsed = row.value("elapsed", ability.elapsed);
            ability.cooldown = row.value("cooldown", ability.cooldown);
        }
        auto* system = findSystem(entries, id);
        const int effective = system ? std::clamp(system->power, 0, system->health) : 0;
        ability.duration = id == 10 ? 5.f * effective : 30.f;
        if (!std::isfinite(ability.elapsed) || !std::isfinite(ability.cooldown) ||
            ability.elapsed < 0 || ability.elapsed > 30 || ability.cooldown < 0 ||
            ability.cooldown > 60 ||
            (ability.active && (!system || effective == 0 || ability.elapsed > ability.duration ||
                                (system->hackActive && system->hackLevel > 1)))) {
            throw std::runtime_error("invalid_combat_ability");
        }
        if (system && values.contains(name)) {
            if (ability.active && system->lockCount == 0) {
                system->lockCount = -1;
            } else if (!ability.active && ability.cooldown > 0 && system->lockCount == 0) {
                system->lockCount = int(std::ceil(ability.cooldown / 5.f));
                system->lockElapsed = float(system->lockCount) * 5.f - ability.cooldown;
            }
            system->ion = float(std::max(0, system->lockCount));
            system->ionTimer = system->lockCount > 0 ? 5.f - system->lockElapsed : 0.f;
        }
        if (system) {
            ability.cooldown =
                system->lockCount > 0 ? float(system->lockCount) * 5.f - system->lockElapsed : 0.f;
        }
    };
    loadAbility("cloak", cloak, playerSystems, 10);
    loadAbility("battery", battery, playerSystems, 12);
    loadAbility("enemy_cloak", enemy.cloak, enemy.systems, 10);
    loadAbility("enemy_battery", enemy.battery, enemy.systems, 12);
    const auto bonus = [](const CombatAbility& ability, std::vector<RunSystem>& entries) {
        const auto* system = findSystem(entries, 12);
        return ability.active && system ? 2 * std::clamp(system->power, 0, system->health) : 0;
    };
    reactor(playerSystems, ship_.reactor + bonus(battery, playerSystems));
    reactor(enemy.systems, enemy.ship.reactor + bonus(enemy.battery, enemy.systems));
    const auto validateBattery = [&](const auto& entries, int reactorPower, int batteryCapacity) {
        int allocated = 0;
        int used = 0;
        for (const auto& s : entries) {
            const int id = s.definition.id;
            if ((id < 6 || id > 8) && id != 12) {
                allocated += s.batteryPower;
                used += std::max(0, s.power - s.bonusPower);
            } else if (s.batteryPower != 0) {
                throw std::runtime_error("invalid_battery_allocation");
            }
        }
        if (allocated > batteryCapacity || used - allocated > reactorPower) {
            throw std::runtime_error("invalid_battery_allocation");
        }
    };
    validateBattery(playerSystems, ship.reactor, bonus(battery, playerSystems));
    validateBattery(enemy.systems, enemy.ship.reactor, bonus(enemy.battery, enemy.systems));
    int weaponPower = 0;
    for (std::size_t i = 0; i < playerWeaponPower.size(); ++i) {
        if (playerWeaponPower[i]) {
            weaponPower += catalog_.equipment(ship.weapons[i]).power;
        }
    }
    const auto* playerWeapons = findSystem(playerSystems, 3);
    if (weaponPower != (playerWeapons ? playerWeapons->power : 0)) {
        throw std::runtime_error("incoherent_weapon_power");
    }
    if (const auto* enemyWeapons = findSystem(enemy.systems, 3)) {
        bool matched = false;
        for (std::size_t mask = 0; mask < (std::size_t{1} << enemy.powered.size()); ++mask) {
            int used = 0;
            for (std::size_t i = 0; i < enemy.powered.size(); ++i) {
                if ((mask & (std::size_t{1} << i)) != 0) {
                    used += catalog_.equipment(enemy.ship.weapons[i]).power;
                }
            }
            if (used == enemyWeapons->power) {
                for (std::size_t i = 0; i < enemy.powered.size(); ++i) {
                    enemy.powered[i] = (mask & (std::size_t{1} << i)) != 0;
                }
                matched = true;
                break;
            }
        }
        if (!matched) {
            throw std::runtime_error("incoherent_enemy_weapon_power");
        }
    }
    // A fixture that lowers a drone controller below its powered drones stands
    // in for damage: DroneSystem::CheckMaxPower 0x613dd0 unpowers drones from
    // the back until the raw bars cover them, and each DePowerDrone takes its
    // bars with it, leaving the controller with what its drones still use.
    const auto settleDrones = [&](auto& drones, auto& entries) {
        auto* controller = findSystem(entries, 4);
        const int available = controller ? controller->power - controller->bonusPower : 0;
        int used = 0;
        for (const auto& drone : drones) {
            if (drone.powered) {
                used += std::max(0, catalog_.equipment(drone.blueprint).power - drone.bonusPower);
            }
        }
        bool unpowered = false;
        for (std::size_t index = drones.size(); index-- > 0 && used > available;) {
            auto& drone = drones[index];
            if (drone.powered) {
                used -= std::max(0, catalog_.equipment(drone.blueprint).power - drone.bonusPower);
                drone.powered = false;
                unpowered = true;
            }
        }
        if (controller && unpowered) {
            controller->power = controller->bonusPower + used;
            controller->batteryPower = std::min(controller->batteryPower, used);
        }
    };
    settleDrones(playerDrones, playerSystems);
    settleDrones(enemy.drones, enemy.systems);
    // Validate all values before committing; snapshots never embed native/original pointers.
    cloak_ = cloak;
    battery_ = battery;
    systems_ = std::move(playerSystems);
    weaponPower_ = std::move(playerWeaponPower);
    weaponDesired_ = std::move(playerWeaponDesired);
    ship_ = std::move(ship);
    combatDrones_ = std::move(playerDrones);
    combatWeapons_ = std::move(playerFactories);
    crew_ = std::move(playerCrew);
    roomOxygen_ = std::move(roomOxygen);
    enemy_ = std::move(enemy);
    // The snapshot may set the battery on/off or its power directly, bypassing
    // SetTurnedOn 0x608b80 / OnLoop 0x608ed0, which set PowerManager::SetBatteryPower.
    // Commit the capacity the allocation checks above validated against.
    batteryCapacity_ = {bonus(battery_, systems_), bonus(enemy_.battery, enemy_.systems)};
    for (int owner : {0, 1}) {
        for (auto& source : owner == 0 ? systems_ : enemy_.systems) {
            syncSystemLock(source, owner == 1);
        }
        assignSpaceDroneId(owner == 0 ? hackingDrone_ : enemy_.hackingDrone);
        for (auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
            assignSpaceDroneId(drone);
        }
        registerSpaceDrones(owner, true);
    }
    hull_ = hull;
    missiles_ = missiles;
    shieldBubbles_ = shields;
    superShields_ = superShields;
    shieldHits_.clear();
    resistMessages_.clear();
    updateManning();
    previousCrew_.clear();
    for (const auto& c : crew_) {
        previousCrew_.push_back({c.x, c.y});
    }
    resetResourceFeedback();
}

void Run::advanceEnemyConstructorClocks(Enemy& enemy) {
    // ShipManager::OnInit4a6e00 executes one immediate OnLoop4a05b0.
    // Runtime births use one fixed native step; startup Hangar owns its
    // separate zero-delta pass. Initially dormant effects still advance.
    float augmentation = 0;
    for (const auto& id : enemy.ship.augments) {
        if (id == "AUTO_COOLDOWN") {
            augmentation += catalog_.equipment(id).value;
        }
    }
    for (auto& system : enemy.systems) {
        const int id = system.definition.id;
        if ((id < 6 || id > 8) && id != 12) {
            const int roomPower =
                static_cast<int>(std::ranges::count_if(enemy.crew, [&](const auto& member) {
                    return !member.outOfGame && !member.intruder &&
                           member.appearance.species == "energy" &&
                           member.room == system.definition.room;
                }));
            const int bonus = std::min(roomPower, systemCapacity(system));
            system.power += bonus - system.bonusPower;
            system.bonusPower = bonus;
            if (id != 3 && id != 4) {
                // Initial SetRoom actors are at their final goal, so their
                // supplied bonus is permanent for SetBonus614f40's intent.
                system.restorePower = std::max(system.restorePower, system.power);
            }
            int remaining = bonus;
            if (id == 3) {
                for (std::size_t index = 0; index < enemy.weapons.size(); ++index) {
                    auto& weapon = enemy.weapons[index];
                    weapon.bonusPower =
                        std::min(remaining, catalog_.equipment(enemy.ship.weapons[index]).power);
                    remaining -= weapon.bonusPower;
                }
            } else if (id == 4) {
                for (auto& drone : enemy.drones) {
                    drone.bonusPower =
                        std::min(remaining, catalog_.equipment(drone.blueprint).power);
                    remaining -= drone.bonusPower;
                }
            }
            // Initially unpowered armaments need no reverse slot shedding.
            // Other systems apply base CheckMaxPower without consulting the
            // prior enemy object that is still live during staged construction.
            system.power =
                std::max(system.bonusPower, std::min(system.power, systemCapacity(system)));
        } else {
            system.power = systemCapacity(system);
        }
        system.lastBonusPower = system.bonusPower;
        if (id == 0) {
            system.restorePower -= system.restorePower & 1;
            const int reactorRaw = system.power - system.bonusPower - system.batteryPower;
            if ((system.power & 1) && (reactorRaw & 1) && !systemLocked(system)) {
                --system.power;
            }
            enemy.shields = std::min(enemy.shields, system.power / 2);
            if (enemy.shields < system.power / 2) {
                enemy.shieldCharge += 2.5f * Time::step;
            }
        }
        for (auto& elapsed : system.damagingEffectTimes) {
            elapsed = Time::loopAnimation(elapsed, 1.f);
        }
        if (system.definition.id == 11 && !system.definition.weapon.empty()) {
            const auto& blueprint = catalog_.equipment(system.definition.weapon);
            auto& weapon = system.artillery;
            weapon.cooldown = std::max(.1f, blueprint.cooldown * (1.5f - system.power * .25f));
            if (system.power > 0) {
                weapon.charge =
                    std::min(weapon.cooldown, weapon.charge + Time::step * (1.f + augmentation));
            }
            advanceWeaponAnimation(weapon, blueprint, system.power > 0, 0);
        }
    }
    for (std::size_t index = 0; index < enemy.weapons.size(); ++index) {
        // AddWeapon creates unpowered factories. AI allocation follows
        // CompleteShip construction, after this immediate manager update.
        advanceWeaponAnimation(
            enemy.weapons[index], catalog_.equipment(enemy.ship.weapons[index]), false, 0);
    }
    for (auto& elapsed : enemy.engineAnimationTimes) {
        elapsed = Time::loopAnimation(elapsed, .5f);
    }
    for (auto& [room, times] : enemy.roomFireAnimationTimes) {
        (void)room;
        for (auto& elapsed : times) {
            elapsed = Time::loopAnimation(elapsed, 1.f);
        }
    }
    for (auto& [room, animations] : enemy.fireSpreaderAnimations) {
        (void)room;
        for (auto& animation : animations) {
            animation.elapsed = Time::loopAnimation(animation.elapsed, 1.f);
        }
    }
}

void Run::beginCombat(std::string_view enemyShipId,
                      std::string_view enemyDroneChoice,
                      std::string_view shipEventId,
                      std::uint32_t shipSeed) {
    if (shipSeed != 0) {
        eventRandom_ = RandomStream(shipSeed);
    }
    const std::string resolvedShip =
        shipEventId.empty() ? std::string(enemyShipId)
                            : catalog_
                                  .selectShip(std::string(enemyShipId),
                                              std::max(0, sector_ - 1 - (difficulty_ == 0 ? 1 : 0)),
                                              eventRandom_)
                                  .id;
    enemyShipId = resolvedShip;
    // Explicit diagnostics retain the measured P06 fixture. Authored encounters
    // use ShipGenerator::CreateShip (original x86 0x5fca40).
    Enemy next;
    next.ship = catalog_.ship(std::string(enemyShipId));
    // ShipAI::ShipAI 0x600700 samples both defaults before constructing Ship.
    next.surrenderThreshold = int(combatRandom_.next() % 5) + 1;
    next.escapeThreshold = int(combatRandom_.next() % 5) + 1;
    if (next.escapeThreshold == next.surrenderThreshold) {
        --next.escapeThreshold;
    }
    next.roomFireAnimationTimes = initializeRoomFireAnimations(next.ship, catalog_, eventRandom_);
    initializeExplosion(next.explosionState, next.ship);
    for (auto& elapsed : next.engineAnimationTimes) {
        elapsed = float(eventRandom_.next() % 4) * .125f;
    }
    next.doors.assign(next.ship.doors.size(), false);
    next.fakeDoors.assign(next.ship.doors.size(), false);
    next.doorFrames.assign(next.ship.doors.size(), 0);
    next.doorTimers.assign(next.ship.doors.size(), 0);
    const bool referenceRebel = shipEventId.empty() && enemyShipId == "REBEL_SKINNY";
    pugi::xml_document generatorDocument;
    pugi::xml_node generatorBlueprint;
    if (!next.ship.source.empty()) {
        generatorDocument = xmlResource(archive_, next.ship.source);
        auto root = generatorDocument.child("FTL");
        if (!root) {
            root = generatorDocument;
        }
        for (auto node : root.children("shipBlueprint")) {
            if (std::string_view(node.attribute("name").as_string()) == enemyShipId) {
                generatorBlueprint = node;
                break;
            }
        }
    }
    if (generatorBlueprint) {
        const std::string_view boarding = generatorBlueprint.child_value("boardingAI");
        next.boardingAi = boarding == "invasion" ? 2 : boarding == "sabotage" ? 1 : 0;
    } else if (next.ship.id == "BOSS_3") {
        next.boardingAi = 2;
    }
    const std::string_view surrenderProfile =
        !shipEventId.empty() ? shipEventId : (referenceRebel ? "REBEL" : enemyShipId);
    const auto combatDefinition = events_.shipEvent(std::string(surrenderProfile));
    const bool authoredGenerator = generatorBlueprint && !shipEventId.empty();
    const bool initialBlueprintArmory = authoredGenerator || next.ship.id.starts_with("BOSS");
    std::vector<int> authoredSystemPower;
    authoredSystemPower.reserve(next.ship.systems.size());
    for (const auto& definition : next.ship.systems) {
        authoredSystemPower.push_back(definition.power);
    }
    int generatorAttackFilter = 3;
    std::vector<bool> preparedSystems(next.ship.systems.size(), false);
    std::vector<std::size_t> constructedSystemOrder;
    std::map<std::size_t, CombatWeapon> constructedArtillery;
    std::map<std::size_t, std::vector<float>> constructedMedbay;
    std::map<std::size_t, int> constructedPower;
    std::map<std::size_t, int> constructedIntent;
    int constructorPowerUsed = 0;
    std::vector<CombatDrone> constructedDrones;
    const auto prepareSystem = [&](SystemDefinition& definition) {
        const auto index = std::size_t(&definition - next.ship.systems.data());
        if (preparedSystems[index]) {
            return;
        }
        preparedSystems[index] = true;
        // ShipManager::AddSystem 0x49ca00 caps the starting level at the
        // SystemBlueprint maxPower (LONG_ELITE_* author weapons power 9).
        definition.power = std::min(definition.power, catalog_.system(definition.name).maxPower);
        constructedSystemOrder.push_back(index);
        const bool needsPower =
            definition.id != 6 && definition.id != 7 && definition.id != 8 && definition.id != 12;
        int power = needsPower
                        ? definition.id == 3 || definition.id == 4
                              ? 0
                              : std::min(definition.power,
                                         std::max(0, next.ship.reactor - constructorPowerUsed))
                        : definition.power;
        constructedPower[index] = power;
        constructedIntent[index] = power;
        if (needsPower) {
            constructorPowerUsed += power;
        }
        int artilleryOrdinal = 0;
        for (const auto prepared : constructedSystemOrder) {
            artilleryOrdinal += next.ship.systems[prepared].id == 11;
        }
        initializeEnemySystemStation(definition, next.ship, artilleryOrdinal, eventRandom_);
        if (definition.id == 5) {
            constructedMedbay[index] =
                initializeMedbayAnimations(findRoom(next.ship, definition.room), eventRandom_);
        } else if (definition.id == 11 && !definition.weapon.empty()) {
            initializeWeaponFactory(
                constructedArtillery[index], catalog_.equipment(definition.weapon), eventRandom_);
        } else if (definition.id == 15) {
            next.hackingDrone.blueprint = "DRONE_HACKING";
            initializeCrewDroneAnimation(next.hackingDrone, eventRandom_, &next.ship, 1);
        }
    };
    const auto constructDrone = [&](const std::string& id, bool installed = true) {
        CombatDrone drone{.blueprint = id};
        initializeCrewDroneAnimation(drone, eventRandom_, &next.ship, 1);
        if (installed) {
            constructedDrones.push_back(std::move(drone));
        } else {
            next.droneTrash.push_back(std::move(drone));
        }
    };
    if (referenceRebel) {
        next.ship.weapons = {"MISSILES_2", "LASER_BURST_1"};
        next.ship.reactor = 10;
    }
    if (initialBlueprintArmory) {
        // CreateArmory0x4a6520 creates only the authored count of explicit
        // equipment, and only when its system exists during OnInit. Later
        // procedural upgrades do not revisit that initial list.
        const auto hasInitialSystem = [&](int id) {
            return std::ranges::any_of(next.ship.systems, [&](const auto& system) {
                return system.id == id && system.installed;
            });
        };
        next.ship.weapons.resize(
            hasInitialSystem(3) ? std::min({next.ship.weapons.size(),
                                            std::size_t(std::max(0, next.ship.weaponCount)),
                                            std::size_t(std::max(0, next.ship.weaponSlots) + 1)})
                                : 0);
        auto initialDroneBirths = next.ship.drones;
        initialDroneBirths.resize(hasInitialSystem(4)
                                      ? std::min(initialDroneBirths.size(),
                                                 std::size_t(std::max(0, next.ship.droneCount)))
                                      : 0);
        next.ship.drones.resize(
            std::min(initialDroneBirths.size(), std::size_t(std::max(0, next.ship.droneSlots))));
        // OnInit/CreateSystems0x4a6e00 constructs installed systems in ID
        // order, then CreateArmory0x4a6520, before generator upgrades/maxes.
        for (int id = 0; id < 16; ++id) {
            for (auto& definition : next.ship.systems) {
                if (definition.id == id && definition.installed) {
                    // AddSystem 0x49ca00 constructs an installed zero-power
                    // template with one physical bar. Keep its authored zero
                    // separately for GenerateSystemMaxes 0x5fbe90 below.
                    definition.power = std::max(1, definition.power);
                    prepareSystem(definition);
                }
            }
        }
        for (const auto& id : next.ship.weapons) {
            next.weapons.emplace_back();
            initializeWeaponFactory(next.weapons.back(), catalog_.equipment(id), eventRandom_);
        }
        for (std::size_t index = 0; index < initialDroneBirths.size(); ++index) {
            constructDrone(initialDroneBirths[index], index < next.ship.drones.size());
        }
        // CreateShip5fca40 clears defaultCrew before ShipManager::OnInit.
        // Its Fire spreader is constructed by OnInit before procedural crew
        // selection and before GenerateSystemMaxes.
        next.fireSpreaderAnimations = initializeFireSpreaderAnimations(next.ship, eventRandom_);
        // Base StartingPowerUp6146d0 allocates before the Shields vtable is
        // installed. Odd shield raw power remains occupied until its first
        // OnLoop60e310, after all other initial system constructors.
        for (const auto index : constructedSystemOrder) {
            if (next.ship.systems[index].id == 0) {
                const int removed = constructedPower[index] & 1;
                constructedPower[index] -= removed;
                constructorPowerUsed -= removed;
                constructedIntent[index] -= constructedIntent[index] & 1;
            }
        }
        for (auto& [index, times] : constructedMedbay) {
            (void)index;
            for (auto& time : times) {
                time = Time::loopAnimation(time, 1.f);
            }
        }
        // Only initially constructed objects participate in OnInit's
        // immediate manager loop; procedural factories do not exist yet.
        for (auto& [index, weapon] : constructedArtillery) {
            const auto& definition = next.ship.systems[index];
            const auto& blueprint = catalog_.equipment(definition.weapon);
            weapon.cooldown = std::max(.1f, blueprint.cooldown * (1.5f - definition.power * .25f));
            float augmentation = 0;
            for (const auto& id : next.ship.augments) {
                if (id == "AUTO_COOLDOWN") {
                    augmentation += catalog_.equipment(id).value;
                }
            }
            const int power = constructedPower[index];
            weapon.charge =
                power > 0 ? std::min(weapon.cooldown, Time::step * (1.f + augmentation)) : 0.f;
            advanceWeaponAnimation(weapon, blueprint, power > 0, 0);
        }
        advanceEnemyConstructorClocks(next);
    }
    if (authoredGenerator) {
        const int level = sector_ - 1 - (difficulty_ == 0 ? 1 : 0);
        const int nonnegativeLevel = std::max(0, level);
        next.ship.hull = std::min(20, next.ship.hull + level);
        if (difficulty_ == 0 && level == -1) {
            std::erase(next.ship.augments, "ENERGY_SHIELD");
        }
        // GenerateSystemMaxes 0x5fbe90 uses authored initial/max bars,
        // difficulty-dependent integer jitter and the unclamped effective sector.
        std::array<int, 16> maxes{};
        for (int id = 0; id < 16; ++id) {
            const auto entry = std::ranges::find(next.ship.systems, id, &SystemDefinition::id);
            if (entry == next.ship.systems.end()) {
                continue;
            }
            const auto& system = *entry;
            const int authoredPower =
                authoredSystemPower[std::size_t(entry - next.ship.systems.begin())];
            // GenerateSystemMaxes5fbe90 reads template.maxPower directly.
            // The physical system's fallback maxLevel is a separate AddSystem
            // rule; an absent template maximum remains its authored -1 here.
            const int authoredMaximum = system.maxPower;
            const int width = level < 0 ? 1 : difficulty_ == 0 ? 2 : 2 + (level > 1 ? 1 : 0);
            const int expected =
                static_cast<int>(float(authoredPower) +
                                 float(level) * .125f * float(authoredMaximum - authoredPower));
            maxes[std::size_t(system.id)] =
                std::min(authoredMaximum,
                         std::max(authoredPower,
                                  expected + static_cast<int>(eventRandom_.next() %
                                                              static_cast<unsigned>(width))));
        }
        auto upgrade = [&](SystemDefinition& system) {
            if (!system.installed) {
                system.installed = true;
                // ShipManager::AddSystem 0x49ca00 starts a newly installed
                // zero-level template at one bar.
                system.power = std::max(1, system.power);
                prepareSystem(system);
                if (system.id != 6 && system.id != 7 && system.id != 8 && system.id != 12) {
                    next.ship.reactor += system.power;
                }
            } else {
                ++system.power;
                if (system.id != 6 && system.id != 7 && system.id != 8 && system.id != 12) {
                    ++next.ship.reactor;
                }
            }
        };
        const auto upgradeId = [&](int id) {
            if (id == 11) {
                // UpgradeSystem0x5f92b0 creates the whole artillery list, then
                // draws an eligible member even when only one remains.
                std::vector<std::size_t> choices;
                for (std::size_t i = 0; i < next.ship.systems.size(); ++i) {
                    auto& system = next.ship.systems[i];
                    if (system.id != id) {
                        continue;
                    }
                    if (!system.installed) {
                        upgrade(system);
                    }
                    if (system.power < maxes[std::size_t(id)]) {
                        choices.push_back(i);
                    }
                }
                if (!choices.empty()) {
                    upgrade(next.ship.systems[choices[eventRandom_.next() % choices.size()]]);
                }
                return;
            }
            const auto system = std::ranges::find(next.ship.systems, id, &SystemDefinition::id);
            if (system != next.ship.systems.end()) {
                upgrade(*system);
            }
        };
        int primary = nonnegativeLevel + 1;
        int secondary = nonnegativeLevel / 3 + 1 + (difficulty_ == 2 ? 1 : 0);
        int defense = level + 2;
        for (int id = 0; id < 16; ++id) {
            const auto system = std::ranges::find(next.ship.systems, id, &SystemDefinition::id);
            if (system != next.ship.systems.end() && !system->installed &&
                maxes[std::size_t(id)] > 0 &&
                static_cast<int>(eventRandom_.next() % 10) < nonnegativeLevel + 1 + difficulty_) {
                upgradeId(id);
                if (id == 3 || id == 4 || id == 9) {
                    --primary;
                } else {
                    secondary -= difficulty_ == 2 ? 1 : 2;
                }
            }
        }
        auto spendUpgrades = [&](int count, int type) {
            while (count > 0) {
                std::vector<std::size_t> choices;
                for (int id = 0; id < 16; ++id) {
                    const bool group = type == 0 ||
                                       (type == 1 && (id == 0 || id == 1 || id == 10)) ||
                                       (type == 2 && (id == 3 || id == 4 || id == 9 || id == 11));
                    for (std::size_t i = 0; i < next.ship.systems.size(); ++i) {
                        const auto& system = next.ship.systems[i];
                        if (group && system.id == id && system.installed &&
                            system.power < maxes[std::size_t(id)]) {
                            choices.push_back(i);
                        }
                    }
                }
                if (choices.empty()) {
                    break;
                }
                upgradeId(next.ship.systems[choices[eventRandom_.next() % choices.size()]].id);
                --count;
            }
            return count;
        };
        primary = spendUpgrades(primary, 2);
        defense = spendUpgrades(defense, 1);
        (void)spendUpgrades(primary + defense + secondary, 0);
        // GetPossibleWeaponList 0x5fb240: remaining-power quarter floor,
        // slot cap, no initial one-bar weapon on a >2-bar weapon system,
        // and damage/shield penetration composition mask.
        const auto templateNode = events_.shipEvent(std::string(shipEventId));
        auto equipmentOverride = [&](const char* tag,
                                     int systemId,
                                     std::vector<std::string>& inventory) {
            const auto overrideNode = templateNode.child(tag);
            if (!overrideNode) {
                return;
            }
            std::vector<std::string> choices;
            for (auto child : overrideNode.children()) {
                choices.emplace_back(child.child_value());
            }
            const int count = overrideNode.attribute("count").as_int(1);
            int required = 0;
            const auto system =
                std::ranges::find(next.ship.systems, systemId, &SystemDefinition::id);
            for (int index = 0; index < count && !choices.empty(); ++index) {
                const auto chosen = eventRandom_.next() % choices.size();
                required += catalog_.equipment(choices[chosen]).power;
                // CreateShip adds each override before it ensures that the
                // system exists; AddWeapon/AddDrone ignore absent systems.
                if (system != next.ship.systems.end() && system->installed) {
                    const auto slots = systemId == 3 ? next.ship.weaponSlots : next.ship.droneSlots;
                    // WeaponSystem610ce0's append guard allows currentCount
                    // ==slot_count; DroneSystem613380 rejects that case.
                    const int limit = std::max(0, slots) + (systemId == 3 ? 1 : 0);
                    if (inventory.size() < std::size_t(limit)) {
                        inventory.push_back(choices[chosen]);
                        if (systemId == 3) {
                            next.weapons.emplace_back();
                            initializeWeaponFactory(next.weapons.back(),
                                                    catalog_.equipment(choices[chosen]),
                                                    eventRandom_);
                        } else {
                            constructDrone(choices[chosen]);
                        }
                    } else if (systemId == 4) {
                        // AddDrone constructs before DroneSystem's capacity
                        // check; a refused object still consumed its ctor draws.
                        CombatDrone refused{.blueprint = choices[chosen]};
                        initializeCrewDroneAnimation(refused, eventRandom_, &next.ship, 1);
                        next.droneTrash.push_back(std::move(refused));
                    }
                }
                choices.erase(choices.begin() + static_cast<std::ptrdiff_t>(chosen));
            }
            if (required > 0 && system != next.ship.systems.end()) {
                if (!system->installed) {
                    upgrade(*system);
                }
                while (system->power < required) {
                    upgrade(*system);
                }
            }
        };
        equipmentOverride("weaponOverride", 3, next.ship.weapons);
        equipmentOverride("droneOverride", 4, next.ship.drones);
        const std::string load =
            generatorBlueprint.child("weaponList").attribute("load").as_string();
        if (!load.empty()) {
            const auto system = std::ranges::find_if(next.ship.systems, [](const auto& item) {
                return item.id == 3 && item.installed;
            });
            if (system != next.ship.systems.end()) {
                int remaining = system->power;
                int filter = 3;
                for (const auto& id : next.ship.weapons) {
                    const auto& weapon = catalog_.equipment(id);
                    remaining -= weapon.power;
                }
                while (next.ship.weapons.size() < static_cast<std::size_t>(next.ship.weaponSlots)) {
                    std::vector<std::pair<std::string, int>> choices;
                    for (const auto& id : catalog_.list(load)) {
                        // GetWeaponBlueprint (0x5e6580) returns a zero-power dummy for
                        // obsolete list entries; GetPossibleWeaponList rejects it.
                        if (!catalog_.containsEquipment(id)) {
                            continue;
                        }
                        const auto& weapon = catalog_.equipment(id);
                        const int penetration = weapon.type == "LASER"                          ? 2
                                                : weapon.type == "BEAM" && weapon.piercing <= 3 ? 2
                                                                                                : 0;
                        const int flags = penetration + (weapon.damage != 0 ? 1 : 0);
                        if (weapon.power > remaining / 4 && weapon.power <= remaining &&
                            (system->power <= 1 || weapon.power != system->power) &&
                            (!next.ship.weapons.empty() || system->power <= 2 ||
                             weapon.power != 1) &&
                            (filter == 0 || (flags & filter) != 0)) {
                            choices.emplace_back(id, flags);
                        }
                    }
                    if (choices.empty()) {
                        break;
                    }
                    const auto& chosen = choices[eventRandom_.next() % choices.size()];
                    next.ship.weapons.push_back(chosen.first);
                    next.weapons.emplace_back();
                    initializeWeaponFactory(
                        next.weapons.back(), catalog_.equipment(chosen.first), eventRandom_);
                    remaining -= catalog_.equipment(chosen.first).power;
                    filter &= ~chosen.second;
                }
                generatorAttackFilter = filter;
            }
        }
        int missileWeapons = 0;
        for (const auto& id : next.ship.weapons) {
            if (catalog_.equipment(id).missiles > 0) {
                ++missileWeapons;
            }
        }
        next.ship.missiles = std::max(next.ship.missiles, 2 * missileWeapons);
    }
    next.hull = next.ship.hull;
    next.missiles = next.ship.missiles;
    next.droneParts = next.ship.droneParts;
    next.shields = std::ranges::any_of(next.ship.systems,
                                       [](const auto& definition) {
                                           return definition.id == 0 && definition.installed &&
                                                  definition.power >= 2;
                                       })
                       ? 1
                       : 0;
    if (authoredGenerator) {
        for (const auto& system : next.ship.systems) {
            if (system.id == 0 && system.installed) {
                next.shields = system.power / 2;
            }
        }
    }
    next.superShields =
        std::find(next.ship.augments.begin(), next.ship.augments.end(), "ENERGY_SHIELD") !=
                next.ship.augments.end()
            ? 5
            : 0;
    next.maxSuperShields = next.superShields;
    next.present = true;
    for (auto& definition : next.ship.systems) {
        if (!definition.installed) {
            continue;
        }
        prepareSystem(definition);
    }
    if (authoredGenerator) {
        // Generator return powers/charges shields after reactor upgrades.
        // Ordinary UpgradeSystem615b80 never allocates its new bars.
        for (const auto index : constructedSystemOrder) {
            const auto& definition = next.ship.systems[index];
            if (definition.id != 0) {
                continue;
            }
            const int addition = std::min(definition.power - constructedPower[index],
                                          std::max(0, next.ship.reactor - constructorPowerUsed)) /
                                 2 * 2;
            constructedPower[index] += addition;
            constructorPowerUsed += addition;
        }
    }
    // Keep the physical system-vector order: initial CreateSystems is ID
    // ordered, while systems installed by generation append at installation.
    for (const auto index : constructedSystemOrder) {
        auto& definition = next.ship.systems[index];
        if (referenceRebel) {
            definition.power = definition.id == 6                         ? 1
                               : definition.id == 0 || definition.id == 3 ? 3
                                                                          : 2;
        }
        if (!initialBlueprintArmory && definition.installed) {
            // ShipManager::AddSystem 0x49ca00 gives every installed system at
            // least one bar, as the generator path above and Run::load do.
            definition.power = std::max(1, definition.power);
        }
        const int power = initialBlueprintArmory ? constructedPower[index]
                          : definition.id == 0   ? 2
                          : definition.id == 3   ? 0
                          : definition.id == 2   ? 1
                                                 : definition.power;
        if (definition.id == 0) {
            next.shields = power / 2;
        }
        next.systems.push_back({definition, power, definition.power});
        next.systems.back().restorePower =
            initialBlueprintArmory ? constructedIntent[index] : power;
        const auto constructed =
            constructedArtillery.find(std::size_t(&definition - next.ship.systems.data()));
        if (constructed != constructedArtillery.end()) {
            next.systems.back().artillery = constructed->second;
        }
        if (const auto medbay = constructedMedbay.find(index); medbay != constructedMedbay.end()) {
            next.systems.back().damagingEffectTimes = medbay->second;
        }
    }
    if (shipEventId.empty() && !referenceRebel && !next.ship.id.starts_with("BOSS") &&
        next.crew.empty()) {
        // Diagnostic encounters skip ShipGenerator::CreateShip, whose crew
        // the original always adds; stand the authored crewCount at the
        // initial stations so a crewed ship is not left crewless.
        const auto stationRooms = initialEnemyCrewRooms(next.ship, difficulty_);
        for (const auto& authored : next.ship.crew) {
            for (int count = 0; count < authored.count && next.crew.size() < 10; ++count) {
                const std::size_t crewIndex = next.crew.size();
                const int station = crewIndex < stationRooms.size() ? stationRooms[crewIndex] : -1;
                const int roomId =
                    station >= 0 ? station : next.ship.rooms[crewIndex % next.ship.rooms.size()].id;
                const auto& r = findRoom(next.ship, roomId);
                const int columns = std::max(1, r.w / 35);
                const int slots = columns * std::max(1, r.h / 35);
                int slot = 0;
                while (slot + 1 < slots &&
                       std::ranges::any_of(next.crew, [&](const RunCrew& other) {
                           return other.room == roomId && other.slot == slot;
                       })) {
                    ++slot;
                }
                RunCrew crew;
                crew.appearance = {"Rebel", authored.species, 0, roomId, slot, "Rebel"};
                crew.room = crew.targetRoom = roomId;
                crew.slot = crew.targetSlot = slot;
                crew.x = float(r.x + slot % columns * 35 + 17);
                crew.y = float(r.y + slot / columns * 35 + 17);
                crew.health = crewMaximumHealth(authored.species);
                assignPhysicalCrewOrder(crew);
                next.crew.push_back(crew);
            }
        }
    }
    if (referenceRebel) {
        for (auto [roomId, systemId] : {std::pair{0, 6}, {6, 0}, {4, 3}}) {
            const auto& r = findRoom(next.ship, roomId);
            const auto station = std::find_if(
                next.ship.systems.begin(), next.ship.systems.end(), [&](const auto& system) {
                    return system.id == systemId;
                });
            const int slot =
                station != next.ship.systems.end() && station->slot >= 0 ? station->slot : 0;
            RunCrew crew;
            crew.appearance = {"Rebel", "human", 0, roomId, slot, "Rebel"};
            crew.room = crew.targetRoom = roomId;
            crew.slot = crew.targetSlot = slot;
            crew.x = float(r.x + slot % (r.w / 35) * 35 + 17);
            crew.y = float(r.y + slot / (r.w / 35) * 35 + 17);
            assignPhysicalCrewOrder(crew);
            next.crew.push_back(crew);
        }
    }
    while (next.weapons.size() < next.ship.weapons.size()) {
        const auto index = next.weapons.size();
        next.weapons.emplace_back();
        initializeWeaponFactory(
            next.weapons.back(), catalog_.equipment(next.ship.weapons[index]), eventRandom_);
    }
    next.powered.assign(next.weapons.size(), false);
    if (initialBlueprintArmory && (!next.ship.droneLoad.empty() || !next.ship.drones.empty())) {
        const auto* droneSystem = findSystem(next.systems, 4);
        int remaining = droneSystem ? droneSystem->definition.power : 0;
        for (const auto& id : next.ship.drones) {
            remaining -= catalog_.equipment(id).power;
        }
        int filter = generatorAttackFilter;
        bool desperate = false;
        bool generatedDrone = false;
        while (authoredGenerator &&
               next.ship.drones.size() < static_cast<std::size_t>(next.ship.droneSlots) &&
               remaining > 0) {
            std::vector<std::string> choices;
            for (const auto& id : next.ship.droneLoad.empty()
                                      ? std::vector<std::string>{}
                                      : catalog_.list(next.ship.droneLoad)) {
                const auto& drone = catalog_.equipment(id);
                if ((filter == 0 || desperate || drone.type == "COMBAT") &&
                    (desperate ||
                     std::ranges::find(next.ship.drones, id) == next.ship.drones.end()) &&
                    drone.power > 0 && drone.power <= remaining &&
                    (droneSystem->definition.power <= 3 ||
                     drone.power != droneSystem->definition.power)) {
                    choices.push_back(id);
                }
            }
            if (choices.empty()) {
                if (!desperate && generatedDrone) {
                    desperate = true;
                    continue;
                }
                break;
            }
            const auto selected = choices[eventRandom_.next() % choices.size()];
            if (catalog_.equipment(selected).type == "COMBAT") {
                filter = 0;
            }
            desperate = false;
            generatedDrone = true;
            remaining -= catalog_.equipment(selected).power;
            next.ship.drones.push_back(selected);
            constructDrone(selected);
        }
        for (std::size_t index = 0; index < next.ship.drones.size(); ++index) {
            const auto& selected = next.ship.drones[index];
            CombatDrone drone = std::move(constructedDrones[index]);
            const auto& droneBlueprint = catalog_.equipment(selected);
            drone.health = droneBlueprint.id == "BOARDER_ION" ? 125.f
                           : droneBlueprint.type == "REPAIR"  ? 25.f
                           : (droneBlueprint.type == "BATTLE" || droneBlueprint.type == "BOARDER")
                               ? 150.f
                               : 1.f;
            drone.space = droneBlueprint.type == "COMBAT" ? 0 : 1;
            next.drones.push_back(std::move(drone));
        }
    } else if (!next.ship.droneLoad.empty()) {
        const auto* droneSystem = findSystem(next.systems, 4);
        std::vector<std::string> candidates;
        if (droneSystem && droneSystem->power > 0 && next.droneParts > 0) {
            for (const auto& id : catalog_.list(next.ship.droneLoad)) {
                const auto& blueprint = catalog_.equipment(id);
                if (blueprint.kind == "drone" &&
                    (blueprint.type == "COMBAT" || blueprint.type == "DEFENSE" ||
                     blueprint.type == "REPAIR" || blueprint.type == "BATTLE" ||
                     blueprint.type == "BOARDER" || blueprint.type == "SHIP_REPAIR" ||
                     blueprint.type == "SHIELD") &&
                    blueprint.power > 0 && blueprint.power <= droneSystem->power) {
                    candidates.push_back(id);
                }
            }
        }
        if (!candidates.empty()) {
            const auto selected =
                enemyDroneChoice.empty()
                    ? candidates[(referenceRebel ? combatRandom_.next() : eventRandom_.next()) %
                                 candidates.size()]
                    : std::string(enemyDroneChoice);
            if (std::ranges::find(candidates, selected) == candidates.end()) {
                throw std::runtime_error("invalid_combat_enemy_drone_choice");
            }
            CombatDrone drone;
            drone.blueprint = selected;
            initializeCrewDroneAnimation(drone, eventRandom_, &next.ship, 1);
            drone.deployed = drone.powered = true;
            drone.crewDead = false;
            const auto& droneBlueprint = catalog_.equipment(selected);
            drone.health = droneBlueprint.id == "BOARDER_ION" ? 125.f
                           : droneBlueprint.type == "REPAIR"  ? 25.f
                           : (droneBlueprint.type == "BATTLE" || droneBlueprint.type == "BOARDER")
                               ? 150.f
                               : 1.f;
            drone.space = droneBlueprint.type == "COMBAT" ? 0 : 1;
            if (droneBlueprint.type == "SHIP_REPAIR") {
                drone.lifespan = int(combatRandom_.next() % 3) + 3;
            }
            if (catalog_.equipment(selected).type == "DEFENSE") {
                placeDefenseDrone(drone, next.ship);
            } else {
                drone.position = {float(ship_.imageRect[0] + ship_.imageRect[2] + 75),
                                  float(ship_.imageRect[1] + ship_.imageRect[3] / 2)};
            }
            if (droneBlueprint.type == "BOARDER") {
                drone.position = {float(next.ship.imageRect[0] + next.ship.imageRect[2] + 20),
                                  float(next.ship.imageRect[1] + next.ship.imageRect[3] / 2)};
                drone.destination = {800.f, drone.position.y};
            }
            next.drones.push_back(std::move(drone));
            next.ship.drones.push_back(selected);
            --next.droneParts;
        } else if (!enemyDroneChoice.empty()) {
            throw std::runtime_error("invalid_combat_enemy_drone_choice");
        }
    }
    if (authoredGenerator) {
        const auto authoredCrew = generatorBlueprint.child("crewCount");
        const int level = std::max(0, sector_ - 1 - (difficulty_ == 0 ? 1 : 0));
        const int initial = authoredCrew.attribute("amount").as_int();
        const int count =
            initial <= 0
                ? 0
                : static_cast<int>(float(initial) + std::min(1.f, float(level) * .125f) *
                                                        float(next.ship.maxCrew - initial));
        const std::string defaultSpecies = authoredCrew.attribute("class").as_string("human");
        std::vector<std::pair<std::string, int>> batches;
        const auto crewOverride = events_.shipEvent(std::string(shipEventId)).child("crew");
        if (crewOverride) {
            for (auto row : crewOverride.children("crewMember")) {
                const float proportion = row.attribute("prop").as_float();
                const int amount = proportion > 0.f
                                       ? std::max(1, static_cast<int>(proportion * float(count)))
                                       : row.attribute("amount").as_int();
                batches.emplace_back(row.attribute("type").as_string("human"), amount);
            }
        } else if (count > 0) {
            batches.emplace_back(defaultSpecies, count);
        }
        // GetPossibleCrewList5f9640 constructs every explicit candidate's name,
        // then CreateShip chooses one and rebuilds all candidates after every
        // selection, including the final unused rebuild. Factory creation is
        // deferred until the entire override batch has been selected.
        const auto possibleCrew = [&](const std::string& type) {
            std::vector<HangarCrew> candidates;
            if (next.crew.size() >= 10) {
                return candidates; // IsCrewFull491310 uses enemy capacity10.
            }
            if (type == "random") {
                struct WeightedCrew {
                    std::string species;
                    int weight = 0;
                    int total = 0;
                };
                std::vector<WeightedCrew> heap(1);
                for (const auto& [id, weight] :
                     catalog_.crewRewardPool(advanced_, sectorRarities())) {
                    heap.push_back({id, weight, weight});
                }
                if (heap.size() < 2) {
                    return candidates;
                }
                for (std::size_t index = heap.size() - 1; index > 1; --index) {
                    heap[index / 2].total += heap[index].total;
                }
                int choice = int(eventRandom_.next() % std::uint32_t(heap[1].total)) + 1;
                std::size_t index = 1;
                while (choice > heap[index].weight) {
                    choice -= heap[index].weight;
                    index *= 2;
                    if (choice > heap[index].total) {
                        choice -= heap[index].total;
                        ++index;
                    }
                }
                // GetRandomCrew returns the unnamed canonical blueprint;
                // AddCrewMember chooses its name later, during creation.
                candidates.push_back({"", heap[index].species, 0, 0, 0, ""});
                return candidates;
            }
            std::vector<std::string> species;
            try {
                species = catalog_.list(type);
            } catch (const std::runtime_error&) { /* Individual crew blueprint. */
            }
            if (species.empty()) {
                species.push_back(type);
            }
            for (const auto& id : species) {
                stageCombatCrewReward(id);
                candidates.push_back({enemy_.rewardCrewName,
                                      enemy_.rewardCrewSpecies,
                                      enemy_.rewardCrewAppearance,
                                      0,
                                      0,
                                      enemy_.rewardCrewFullName});
            }
            return candidates;
        };
        const auto stationRooms = initialEnemyCrewRooms(next.ship, difficulty_);
        const auto previousEnemy = enemy_;
        for (const auto& [type, amount] : batches) {
            auto candidates = possibleCrew(type);
            std::vector<HangarCrew> selectedCrew;
            for (int index = 0; index < amount && !candidates.empty(); ++index) {
                selectedCrew.push_back(candidates[eventRandom_.next() % candidates.size()]);
                candidates = possibleCrew(type);
            }
            for (std::size_t index = 0; index < selectedCrew.size(); ++index) {
                RunCrew member;
                member.appearance = selectedCrew[index];
                const auto crewIndex = next.crew.size();
                const int preferredRoom =
                    crewIndex < stationRooms.size()
                        ? stationRooms[crewIndex]
                        : next.ship.rooms[eventRandom_.next() % next.ship.rooms.size()].id;
                if (member.appearance.fullName.empty()) {
                    stageCombatCrewReward(member.appearance.species);
                    member.appearance.name = enemy_.rewardCrewName;
                    member.appearance.fullName = enemy_.rewardCrewFullName;
                    member.appearance.appearance = enemy_.rewardCrewAppearance;
                }
                // CrewAnimation479840 starts its1000..1300ms shoot timer before
                // CrewMember47a400 chooses color layers.
                initializeCrewAnimation(member, eventRandom_);
                int stride = member.appearance.species == "human" ? 2 : 1;
                for (const auto& layer : catalog_.appearance(member.appearance.species).layers) {
                    if (!layer.empty()) {
                        member.appearance.appearance +=
                            static_cast<int>(eventRandom_.next() % layer.size()) * stride;
                        stride *= static_cast<int>(layer.size());
                    }
                }
                // Ship generation uses canonical zero skill blueprints. RandomSkills
                // is called by event/store creation, never this constructor tree.
                member.health = crewMaximumHealth(member.appearance.species);
                // AddInitialCrew4a3610 supplies the room, not its station
                // slot. SetRoom46f800 resolves primary/free/connected/nearest
                // availability against this newly constructed physical ship.
                placeVoyageCrew(member, preferredRoom, false, true, &next);
                assignPhysicalCrewOrder(member);
                next.crew.push_back(std::move(member));
                refreshCrewNames(); // Factory::OnLoop replaces interim candidate names.
            }
        }
        enemy_ = previousEnemy;
    }
    for (const auto& room : next.ship.rooms) {
        next.roomOxygen[room.id] = 100.f;
    }
    if (authoredGenerator && next.droneParts <= 0 &&
        (findSystem(next.systems, 4) || findSystem(next.systems, 15))) {
        next.droneParts = 5;
    }
    if (!initialBlueprintArmory) {
        next.fireSpreaderAnimations = initializeFireSpreaderAnimations(next.ship, eventRandom_);
        advanceEnemyConstructorClocks(next);
    }
    // CrewAI::OnInit6017d0 receives the complete physical list only after
    // procedural crew construction. Interior drone bodies are list members
    // even while dead-born and unplaced; flying BOARDER bodies are not.
    next.crewAi.startingCrewCount = static_cast<int>(next.crew.size());
    for (const auto* list : {&next.drones, &next.droneTrash}) {
        for (const auto& drone : *list) {
            next.crewAi.startingCrewCount += drone.crewPhysicalShip == 1;
        }
    }
    if (enemy_.rewardCrewPending) {
        next.rewardCrewSpecies = enemy_.rewardCrewSpecies;
        next.rewardCrewName = enemy_.rewardCrewName;
        next.rewardCrewFullName = enemy_.rewardCrewFullName;
        next.rewardCrewMale = enemy_.rewardCrewMale;
        next.rewardCrewAppearance = enemy_.rewardCrewAppearance;
        next.rewardCrewColorsPrepared = enemy_.rewardCrewColorsPrepared;
        next.rewardCrewPending = true;
        next.rewardCrewOverflow = enemy_.rewardCrewOverflow;
    }
    const bool restoringBoss = campaign_.active && campaign_.stage == campaign_.nextStage &&
                               campaign_.savedBoss.is_object() &&
                               campaign_.savedBoss.at("enemy").at("ship").at("id") == next.ship.id;
    if (!restoringBoss) {
        for (auto& member : voyageBoarders_) {
            member.enemyCrewOrigin = -1;
        }
    }
    configureCombatAiThresholds(next, combatDefinition);
    if (environment_.stormActive) {
        // CreateEvent appends GetStormEffect 0x5ebaf0 after authored statuses.
        // A ship created inside an already active storm must receive it too.
        next.reactorPowerDivide = 2;
        next.reactorPowerCap = (next.ship.reactor + 1) / 2;
    }
    rebaseVoyageCrewReferences(static_cast<int>(enemy_.crew.size()),
                               static_cast<int>(next.crew.size()));
    enemy_ = std::move(next);
    // The new enemy ShipManager::OnInit 0x4a6e00 runs PowerManager::Restart 0x61ff30
    // and, with its ctor 0x495af0 and Ship::OnInit, starts the per-ship trackers over.
    batteryCapacity_[1] = 0;
    lastHullDamage_[1] = 0;
    fireOxygen_[1].clear();
    roomLastOxygen_[1].clear();
    fireExtinguishers_[1].clear();
    slugGelHealElapsed_[1] = 0;
    doorHitTick_[1].clear();
    systemExplosions_[1].clear();
    // MindSystem ctor 0x609b40: the new enemy source has no queued crew.
    mindQueue_[1].clear();
    assignSpaceDroneId(enemy_.hackingDrone);
    for (auto& drone : enemy_.drones) {
        assignSpaceDroneId(drone);
    }
    registerSpaceDrones(1, true);
    // WorldManager::AddShip 0x5ee6b0: the new ship targets the player
    // (ShipAI::SetEnemyShip -> CombatAI::SetTarget), then CombatControl::
    // AddEnemyShip and the player's own CombatAI::SetTarget each retarget the
    // player's drones through ShipManager::SetTarget 0x49c230.
    retargetSpaceDrones(1);
    retargetSpaceDrones(0);
    retargetSpaceDrones(0);
    refreshCrewNames();
    progressionRun_.tutorial = tutorialOwned_;
    if (!tutorialOwned_) {
        std::erase_if(progressionRun_.flags, [](const auto& entry) {
            return entry.first.starts_with("combat_");
        });
        progressionRun_.flag("encounter_serial");
        progressionRun_.flags["combat_start_hull"] = hull_;
        progressionRun_.flags["combat_start_crew"] =
            static_cast<int>(std::ranges::count_if(crew_, [](const auto& member) {
                return member.health > 0;
            }));
        progressionRun_.flags["enemy_automated"] = enemy_.crew.empty() ? 1 : 0;
    }
    enemyHazards_.clear();
    // A new ShipManager carries empty hitByBeam and killedByBeam state.
    beamHitAt_ = {};
    beamKilledCrew_ = {};
    // ShipManager0x49abc0 renders its own damMessages. Replacing the enemy
    // must discard that manager's messages, including a frozen final MISS.
    std::erase_if(damageMessages_, [](const DamageMessage& message) {
        return message.ship == 1;
    });
    shieldHits_.clear();
    resistMessages_.clear();
    aiming_ = -1;
    surrenderOffered_ = surrenderDialog_ = false;
    explosion_ = 0;
    explosionDuration_ = 3.f;
    enemyExplosionActive_ = false;
    playerExplosionStartTick_ = 0;
    playerExplosionStarted_ = false;
    combatSounds_ = nlohmann::json::array();
    hostile_ = true;
    ftlCharge_ = 0;
}

void Run::configureCombatAiThresholds(Enemy& next, pugi::xml_node combatDefinition) {
    // WorldManager::CreateShip 0x5eee10 selects escape then surrender AFTER
    // creating systems/crew/loadouts; both include worldLevel/2.
    const auto selectAmount = [&](pugi::xml_node node, int& minimum, int& maximum, float& chance) {
        minimum = node.attribute("min").as_int();
        maximum = node.attribute("max").as_int();
        if (minimum > maximum) {
            std::swap(minimum, maximum);
        }
        chance = node.attribute("chance").as_float(-1.f);
        if (chance > 0 && float(eventRandom_.next() % 100) < chance * 100.f) {
            return 0;
        }
        return maximum == 0 ? minimum
                            : minimum + int(eventRandom_.next() % unsigned(maximum + 1 - minimum));
    };
    int escapeMinimum = 0;
    int escapeMaximum = 0;
    float escapeChance = -1;
    const auto escape = combatDefinition.child("escape");
    next.escapeThreshold = selectAmount(escape, escapeMinimum, escapeMaximum, escapeChance) +
                           std::max(0, sector_ - 1) / 2;
    if (next.escapeThreshold == next.surrenderThreshold) {
        --next.escapeThreshold;
    }
    next.escapeTimer = float(escape.attribute("timer").as_int(15));
    if (next.escapeTimer <= 0) {
        next.escapeTimer = 15;
    }
    const auto surrender = combatDefinition.child("surrender");
    next.hasSurrenderThreshold = bool(surrender);
    next.surrenderThresholdSelected = selectAmount(surrender,
                                                   next.surrenderThresholdMin,
                                                   next.surrenderThresholdMax,
                                                   next.surrenderChanceNone);
    next.surrenderWorldLevelOffset = std::max(0, sector_ - 1) / 2;
    next.surrenderThreshold = next.surrenderThresholdSelected + next.surrenderWorldLevelOffset;
    if (next.escapeThreshold == next.surrenderThreshold) {
        --next.escapeThreshold;
    }
    next.lastEscapeHealth = next.hull;
}

void Run::combatSound(const std::vector<std::string>& aliases,
                      std::uint64_t key,
                      bool weaponEffect) {
    if (aliases.empty()) {
        return;
    }
    // EffectsBlueprint::RandomSoundChoice 0x5dedd0 consumes random32 for
    // every nonempty weapon sound group, including a one-alias group.
    // Direct system/crew event aliases remain separate from that path.
    const auto& alias = aliases[(weaponEffect ? combatRandom_.next() : key) % aliases.size()];
    if (alias.empty()) {
        return;
    }
    audio_.sound(alias);
    combatSounds_.push_back({{"tick", tick_}, {"alias", alias}});
}

void Run::pushCombatMessage(DamageMessage message) {
    if (nextDamageMessageOrder_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Combat message order exhausted");
    }
    message.messageOrder = nextDamageMessageOrder_++;
    damageMessages_.push_back(message);
}

void Run::pushCombatMessage(ResistMessage message) {
    if (nextDamageMessageOrder_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Combat message order exhausted");
    }
    message.messageOrder = nextDamageMessageOrder_++;
    resistMessages_.push_back(message);
}

bool Run::touchShieldHit(int ship, SDL_FPoint point, std::uint64_t id, int damage) {
    // CollisionReal60d980 reuses an arc by Damage.selfId. Repeated queries
    // move/refresh it and return its saved shield level without another hit.
    const auto found = std::ranges::find_if(shieldHits_, [&](const ShieldHit& hit) {
        return hit.ship == ship && hit.projectileId == id;
    });
    if (found != shieldHits_.end()) {
        found->point = point;
        found->currentSize = 40.f - 20.f * Time::originalStep;
        return false;
    }
    shieldHits_.push_back({ship, point, tick_, id, 0, 0, damage});
    return true;
}

int Run::ionAfterResistance(int targetShip, int amount, SDL_FPoint point, bool shieldDomain) {
    if (amount <= 0) {
        return amount;
    }
    const auto& ship = targetShip == 1 ? enemy_.ship : ship_;
    float resistance = 0;
    for (const auto& augmentation : ship.augments) {
        if (augmentation == "ION_ARMOR") {
            resistance += catalog_.equipment(augmentation).value;
        }
    }
    if (resistance > 0 &&
        int(combatRandom_.next() % 10) + 1 <= std::clamp(int(resistance * 10.f), 0, 10)) {
        pushCombatMessage(
            ResistMessage{targetShip, point, tick_, true, 0, shieldDomain, shieldDomain});
        return 0;
    }
    return amount;
}

void Run::ionizeShieldCollision(int targetShip, int amount, int stun, int stunChance) {
    if (amount <= 0) {
        return;
    }
    auto& systems = targetShip == 1 ? enemy_.systems : systems_;
    auto* shields = findSystem(systems, 0);
    if (!shields) {
        return;
    }
    const auto& ship = targetShip == 1 ? enemy_.ship : ship_;
    const int ion =
        ionAfterResistance(targetShip, amount, center(findRoom(ship, shields->definition.room)));
    if (ion > 0) {
        applySystemIon(*shields, ion, targetShip == 1);
    }
    auto& bubbles = targetShip == 1 ? enemy_.shields : shieldBubbles_;
    bubbles = std::min(bubbles, shields->power / 2);
    // CollisionMoving499990 invokes DamageCrew after DamageSystem even when
    // IonArmor resisted. The copied damage has zero hull/personnel payload.
    std::vector<PhysicalDamageActor> bodies;
    for (int owner : {0, 1}) {
        for (auto& member : owner == 0 ? crew_ : enemy_.crew) {
            const int physicalShip = member.intruder ? 1 - owner : owner;
            const int room = member.intruder ? member.boardRoom : member.room;
            if (!member.outOfGame && physicalShip == targetShip &&
                room == shields->definition.room) {
                bodies.push_back({&member, nullptr, owner, member.physicalCrewOrder});
            }
        }
        for (auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == targetShip &&
                drone->crewRoom == shields->definition.room) {
                bodies.push_back({nullptr, drone, owner, drone->physicalCrewOrder});
            }
        }
    }
    if (targetShip == 0) {
        for (auto& member : voyageBoarders_) {
            if (!member.outOfGame && member.room == shields->definition.room) {
                bodies.push_back({&member, nullptr, 1, member.physicalCrewOrder});
            }
        }
    }
    std::ranges::stable_sort(bodies, {}, &PhysicalDamageActor::order);
    for (const auto& body : bodies) {
        if (body.drone) {
            damageCrewDrones(targetShip,
                             shields->definition.room,
                             0,
                             nullptr,
                             float(stun),
                             stunChance,
                             -1,
                             body.drone);
        } else {
            body.crew->stunTime = std::max(body.crew->stunTime, float(stun));
            const int roll = int(combatRandom_.next() % 10);
            if (advanced_ && stunChance > roll) {
                body.crew->stunTime = std::max(body.crew->stunTime, 3.f);
            }
        }
    }
}

// ShipManager::ResistDamage 0x491940. Its second parameter is the augmentation id as
// a std::string, and it returns false without consuming a draw when the ship does not
// carry that augmentation; otherwise it scales the augmentation value to a percentage
// and compares it against a full random32() reduced modulo 100 plus one.
bool Run::resistAugmentation(int targetShip, std::string_view augmentation) {
    const auto& ship = targetShip == 1 ? enemy_.ship : ship_;
    if (std::find(ship.augments.begin(), ship.augments.end(), augmentation) ==
        ship.augments.end()) {
        return false;
    }
    const float value = catalog_.equipment(std::string(augmentation)).value;
    return int(value * 100.f) >= int(combatRandom_.next() % 100) + 1;
}

void Run::crewDeath(RunCrew& crew) {
    // HP depletion is only the start of CrewAnimation status3 (473600).
    // Physical removal and clone eligibility follow Dead472fc0, then
    // ShipManager::UpdateCrewmembers49ee10's next removal pass.
    if (crew.health > 0 || crew.dying || crew.outOfGame) {
        return;
    }
    crew.dying = true;
    crew.dead = false;
    crew.deathAnimationElapsed = 0;
    crew.deathAnimationDone = false;
    crew.deathTick = tick_;
    crew.path.clear();
    crew.boardPath.clear();
    crew.selected = false;
    crew.attackTargetOwner = crew.attackTargetIndex = -1;
    crew.attackTargetDrone = false;
    const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& member) {
        return &member == &crew;
    });
    progressionRun_.tutorial = tutorialOwned_;
    progressionRun_.flag(playerOwned ? "lost_crew" : "combat_enemy_crew_killed");
    // CrewAnimation473600 plays death sounds for player creation ownership;
    // enemy non-drone actors enter their death strip without a sound draw.
    if (playerOwned) {
        const auto& species = crew.appearance.species;
        if (species == "rock" || species == "mantis" || species == "crystal" || species == "engi" ||
            species == "energy" || species == "anaerobic") {
            combatSound({species == "anaerobic" ? "shrikeDeath" : species + "Death"}, tick_);
        } else if (crew.recordMale) {
            combatSound({"death1", "death2"}, tick_);
        } else {
            combatSound({"femaleDeath1", "femaleDeath2", "femaleDeath3"}, tick_);
        }
    }
}

void Run::selectWeapon(int index, bool remove) {
    if (index < 0 || index >= static_cast<int>(weaponPower_.size()) || defeated_) {
        return;
    }
    cancelAim();
    if (remove || !weaponPower_[index]) {
        const bool wasPowered = weaponPower_[index];
        if (!remove) {
            const auto& blueprint = catalog_.equipment(ship_.weapons[std::size_t(index)]);
            auto* weapons = system(3);
            const int required = std::max(0, blueprint.power - combatWeapons_[index].bonusPower);
            // WeaponControl::SelectArmament5511f0 tests whether this weapon
            // uses ammunition, before either system lock or power allocation.
            if (blueprint.missiles > 0 && missiles_ == 0) {
                startArmamentWarning(0, "warning_no_missiles");
                stopArmamentWarning(1);
            } else if (weapons && !systemLocked(*weapons)) {
                const int effective = std::min(systemCapacity(*weapons),
                                               weapons->power - weapons->bonusPower +
                                                   crewPowerForSystem(*weapons, false).first);
                if (systemCapacity(*weapons) - effective < required) {
                    const bool broken = weapons->definition.power - effective >= required &&
                                        weapons->health < weapons->definition.power;
                    startArmamentWarning(1,
                                         weapons->definition.power < required
                                             ? "warning_needs_upgrade"
                                         : broken ? "warning_system_broken"
                                                  : "warning_no_system_power",
                                         broken);
                    stopArmamentWarning(0);
                }
            }
        }
        if (weaponPower(index, !remove)) {
            // WeaponControl::SelectArmament 0x5511f0 and DeselectArmament
            // 0x5510e0 play the matching system sound after user toggles.
            audio_.sound(remove ? "powerDownSystem" : "powerUpSystem");
        } else if (!remove || wasPowered) {
            // SelectArmament5511f0 / DeselectArmament5510e0: failed user
            // activation or a blocked powered-slot removal requests failure audio.
            audio_.sound("powerUpFail");
        }
        return;
    }
    auto& weapon = combatWeapons_[index];
    if (const auto* weapons = system(3); weapons && weapons->hackLevel > 1) {
        return;
    }
    // SelectArmament5511f0 calls ClearAiming40c080; only a partly
    // constructed multi-target volley prevents cancellation.
    weapon.clearTargets();
    aiming_ = index;
}

void Run::startArmamentWarning(int index, const std::string& key, bool broken) {
    auto& warning = armamentWarnings_.at(std::size_t(index));
    // SetText5b41f0 stops a changed message, while Start5b43c0 refuses to
    // extend an already running message. Its sound is empty for these boxes.
    if (warning.text != key) {
        warning.text = key;
        warning.broken = broken;
        warning.elapsed = -1;
    }
    if (warning.elapsed < 0) {
        warning.elapsed = 0;
    }
}

void Run::stopArmamentWarning(int index) {
    armamentWarnings_.at(std::size_t(index)).elapsed = -1;
}

void Run::advanceArmamentWarnings(float seconds) {
    for (std::size_t index = 0; index < armamentWarnings_.size(); ++index) {
        // Control::OnLoop returns before its warning clocks when its
        // corresponding physical system is absent.
        if (!system(index < 2 ? 3 : 4)) {
            continue;
        }
        auto& warning = armamentWarnings_[index];
        if (warning.elapsed >= 0) {
            warning.elapsed += seconds;
            if (warning.elapsed > 2.f) {
                warning.elapsed = -1;
            }
        }
    }
}

void Run::targetWeapon(int id, bool self, const SDL_FPoint* aim) {
    if (aiming_ < 0 || (!self && (!hostile_ || !enemy_.present))) {
        return;
    }
    auto& weapon = combatWeapons_.at(aiming_);
    const auto& blueprint = catalog_.equipment(ship_.weapons.at(aiming_));
    if (self && blueprint.type != "BOMB") {
        return;
    }
    const auto& targetShip = self ? ship_ : enemy_.ship;
    const auto point = aim && blueprint.type == "BEAM"
                           ? SDL_FPoint{float(int(aim->x)), float(int(aim->y))}
                           : center(findRoom(targetShip, id));
    if (weapon.volleyRemaining > 0 && weapon.targetGoal > 0 &&
        weapon.volleyRemaining < weapon.targetGoal) {
        // Fire0x40cb80 rejects a replacement while the target list is partly consumed.
        aiming_ = -1;
        return;
    }
    if (blueprint.type == "BEAM" && !weapon.targetFirstPending) {
        weapon.target = point;
        weapon.targetRoom = id;
        weapon.targetFirstPending = true;
        return;
    }
    if (blueprint.type == "BEAM") {
        const float dx = point.x - weapon.target.x;
        const float dy = point.y - weapon.target.y;
        const float distance = std::hypot(dx, dy);
        // UpdateAiming564050 accepts the direction only beyond ten pixels,
        // then sets the endpoint at the full authored beam length.
        if (distance <= 10.f) {
            return;
        }
        const float scale = float(blueprint.length) / distance;
        weapon.targetEnd = {weapon.target.x + dx * scale, weapon.target.y + dy * scale};
        weapon.targetRoomEnd = id;
        weapon.targetFirstPending = false;
    } else {
        weapon.target = point;
        weapon.targetRoom = id;
        // CombatControl::SelectTarget566280 repeats the single selected
        // point until NumTargetsRequired40c7d0 is satisfied. Charged
        // weapons select their whole current charge with one click.
        weapon.targetGoal = blueprint.chargeLevels > 1 ? std::max(1, weapon.chargeLevel)
                                                       : std::max(1, blueprint.shots);
        weapon.targetRooms.assign(static_cast<std::size_t>(weapon.targetGoal), id);
        weapon.targetPoints.assign(static_cast<std::size_t>(weapon.targetGoal), point);
    }
    weapon.targetSelf = self;
    weapon.queued = true;
    weapon.autofire = autofire_ != forceAutofire_;
    // WeaponControl::Fire553950 submits even while uncharged. The factory
    // samples its entry angle at submission, then waits for charge in Update.
    weapon.volleyRoom = weapon.targetRoom;
    weapon.volleyTarget = weapon.target;
    weapon.volleyEnd = weapon.targetEnd;
    weapon.volleySelf = weapon.targetSelf;
    weapon.volleyRooms = weapon.targetRooms;
    weapon.volleyPoints = weapon.targetPoints;
    weapon.volleyIndex = 0;
    weapon.volleyRemaining = blueprint.chargeLevels > 1 ? std::max(1, weapon.targetGoal)
                             : blueprint.type == "BEAM" ? 1
                                                        : std::max(1, blueprint.shots);
    weapon.targetGoal = weapon.volleyRemaining;
    weapon.volleyTimer = 0;
    weapon.emittedVolley = false;
    weapon.entryAngle = (combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
    aiming_ = -1;
}

void Run::setAutofire(bool enabled) {
    // WeaponControl::SetAutofiring 0x553640 plays the selected toggle cue.
    if (enabled != autofire_) {
        audio_.sound(enabled ? "autofireOn" : "autofireOff");
    }
    for (auto& weapon : combatWeapons_) {
        if (!enabled && weapon.autofire && weapon.targetRoom >= 0) {
            weapon.queued = true;
        }
        weapon.autofire = enabled;
    }
    autofire_ = enabled;
}

bool Run::enemyContainsPlayerCrew() const {
    // CrewMember::ProvidesVision720dd0 remains true for owner0 or a
    // controlled enemy until physical removal; CrewDrone73de80 also
    // requires deployment.
    return std::ranges::any_of(crew_,
                               [](const auto& member) {
                                   return member.intruder && !member.outOfGame;
                               }) ||
           std::ranges::any_of(enemy_.crew,
                               [](const auto& member) {
                                   return member.controlled && !member.intruder &&
                                          !member.outOfGame;
                               }) ||
           std::ranges::any_of(physicalCrewDrones(0), [&](const auto* drone) {
               return drone->deployed && drone->crewPhysicalShip == 1 && drone->crewRoom >= 0;
           });
}

bool Run::weaponCursorValid() const {
    if (aiming_ < 0 || aiming_ >= int(combatWeapons_.size())) {
        return false;
    }
    const auto& blueprint = catalog_.equipment(ship_.weapons[std::size_t(aiming_)]);
    const auto roomUnderMouse = [&](const ShipDefinition& ship, float x, float y) {
        return std::ranges::any_of(ship.rooms, [&](const auto& room) {
            return Rect{float(room.x), float(room.y), float(room.w), float(room.h)}.contains(
                float(int(mouseX_ - x)), float(int(mouseY_ - y)));
        });
    };
    // UpdateAiming564050 resolves a self-targeted bomb before inspecting the
    // enemy pointer. CanTargetSelf563e30 does not require a hostile encounter.
    if (blueprint.type == "BOMB" &&
        roomUnderMouse(ship_, playerX() + ship_.horizontalShift, 170.f + ship_.verticalShift)) {
        return true;
    }
    if (!enemy_.present || !hostile_) {
        return false;
    }
    const auto& weapon = combatWeapons_[std::size_t(aiming_)];
    if (blueprint.type == "BEAM" && weapon.targetFirstPending) {
        const float x = float(int(mouseX_ - enemyOriginX()));
        const float y = float(int(mouseY_ - enemyOriginY()));
        // Once a first point exists, the second point can leave the hull and
        // UpdateAiming ignores selected-room/cloak state, requiring distance>10.
        return std::hypot(x - weapon.target.x, y - weapon.target.y) > 10.f;
    }
    return !enemyExplosionActive_ && (!enemy_.cloak.active || enemyContainsPlayerCrew()) &&
           roomUnderMouse(enemy_.ship, enemyOriginX(), enemyOriginY());
}

bool Run::combatClick(float x, float y, bool right) {
    if (right) {
        // CombatMouseRClick568060 snapshots armed state before DisarmAll,
        // always runs both armament handlers, then returns the old state so
        // CommandGui cannot also issue a crew order on this cancelling click.
        const bool wasArmed = aiming_ >= 0 || !abilityAim_.empty();
        cancelAim();
        const int weaponIndex = armamentAt(x, y, false);
        if (weaponIndex >= 0) {
            selectWeapon(weaponIndex, true);
            return true;
        }
        const int index = armamentAt(x, y, true);
        if (index >= 0) {
            if (combatDrones_[index].powered) {
                toggleDrone(index);
            } else {
                combatDrones_[index].repowerPending = false;
            }
            return true;
        }
        return wasArmed;
    }
    for (int id : {10, 12}) {
        if (system(id) && abilityButtonRect(id).contains(x, y)) {
            activateAbility(id);
            return true;
        }
    }
    if (!tutorialOwned_ && autofireRect().contains(x, y)) {
        setAutofire(!autofire_);
        return true;
    }
    for (int slot = 0; slot < ship_.droneSlots; ++slot) {
        if (Rect{systemHudPosition(4).x + 48.f + 97.f * slot, 623, 95, 39}.contains(x, y)) {
            const int index = logicalArmamentIndex("drone", slot);
            if (index >= 0) {
                toggleDrone(index);
            }
            return true;
        }
    }
    if (aiming_ >= 0) {
        if (catalog_.equipment(ship_.weapons.at(aiming_)).type == "BOMB") {
            for (const auto& r : ship_.rooms) {
                if (Rect{playerX() + ship_.horizontalShift + r.x,
                         170.f + ship_.verticalShift + r.y,
                         float(r.w),
                         float(r.h)}
                        .contains(x, y)) {
                    targetWeapon(r.id, true);
                    return true;
                }
            }
        }
        if (!enemy_.present || !hostile_ || enemyExplosionActive_) {
            return false;
        }
        auto& weapon = combatWeapons_.at(aiming_);
        if (catalog_.equipment(ship_.weapons.at(aiming_)).type == "BEAM" &&
            weapon.targetFirstPending) {
            // The first beam point requires a room; its direction can be
            // selected outside the hull once the first point is stored.
            const SDL_FPoint point{x - enemyOriginX(), y - enemyOriginY()};
            targetWeapon(weapon.targetRoom, false, &point);
            return true;
        }
        if (enemy_.cloak.active && !enemyContainsPlayerCrew()) {
            return true;
        }
        for (const auto& r : enemy_.ship.rooms) {
            if (Rect{enemyOriginX() + r.x, enemyOriginY() + r.y, float(r.w), float(r.h)}.contains(
                    x, y)) {
                const SDL_FPoint point{x - enemyOriginX(), y - enemyOriginY()};
                targetWeapon(r.id, false, &point);
                return true;
            }
        }
    }
    return false;
}

void Run::startRoomFire(int targetShip, int roomId, int count) {
    // Spreader<Fire>::StartInRoom0x735470 chooses x then y for every attempt,
    // including a tile already burning, and resets that tile's damage to100.
    const auto& ship = targetShip == 0 ? ship_ : enemy_.ship;
    const auto& room = findRoom(ship, roomId);
    const int columns = room.w / 35;
    const int rows = room.h / 35;
    auto& hazard = (targetShip == 0 ? playerHazards_ : enemyHazards_)[roomId];
    hazard.syncFireTiles();
    for (int index = 0; index < std::min(count, columns * rows); ++index) {
        const int column = static_cast<int>(combatRandom_.next() % static_cast<unsigned>(columns));
        const int row = static_cast<int>(combatRandom_.next() % static_cast<unsigned>(rows));
        const int tile = row * columns + column;
        const auto found = std::ranges::find(hazard.fireTiles, tile);
        if (found == hazard.fireTiles.end()) {
            hazard.fireTiles.push_back(tile);
            hazard.fireHealth.push_back(100);
            hazard.fireDeathTimers.push_back(-1);
            hazard.fires = static_cast<int>(hazard.fireTiles.size());
        } else {
            hazard.fireHealth[static_cast<std::size_t>(found - hazard.fireTiles.begin())] = 100;
        }
        std::erase_if(hazard.fireSmoke, [tile](const FireSmoke& smoke) {
            return smoke.tile == tile;
        });
    }
    hazard.fireRepair = 0;
}

bool Run::breachHullTile(int targetShip, int roomId, int tile, bool specific) {
    auto& hazard = (targetShip == 0 ? playerHazards_ : enemyHazards_)[roomId];
    hazard.breachHealth.resize(hazard.breachTiles.size(), 100.f);
    const auto found = std::ranges::find(hazard.breachTiles, tile);
    // BreachRandomHull 0x483390 leaves an already breached tile alone;
    // BreachSpecificHull 0x483120 re-breaches it, restarting its repair.
    if (found != hazard.breachTiles.end() && !specific) {
        return false;
    }
    // Both play airLoss when the room held oxygen on the last Ship::OnLoop
    // (Room::lastO2) and the tile was intact, then hullBreach.
    if (found == hazard.breachTiles.end()) {
        const auto& last = roomLastOxygen_[std::size_t(targetShip == 1)];
        const auto lastO2 = last.find(roomId);
        const auto& oxygen = targetShip == 1 ? enemy_.roomOxygen : roomOxygen_;
        const auto current = oxygen.find(roomId);
        const float level = lastO2 != last.end()      ? lastO2->second
                            : current != oxygen.end() ? current->second
                                                      : 0.f;
        if (level > 0.f) {
            combatSound({"airLoss"}, tick_);
        }
    }
    combatSound({"hullBreach"}, tick_);
    if (found == hazard.breachTiles.end()) {
        hazard.breachTiles.push_back(tile);
        hazard.breachHealth.push_back(100.f);
    } else {
        hazard.breachHealth[std::size_t(found - hazard.breachTiles.begin())] = 100.f;
    }
    hazard.breaches = int(hazard.breachTiles.size());
    return true;
}

bool Run::breachRandomHull(int targetShip, int roomId) {
    const auto& room = findRoom(targetShip == 0 ? ship_ : enemy_.ship, roomId);
    const int columns = room.w / 35;
    const int rows = room.h / 35;
    if (columns <= 0 || rows <= 0) {
        return false;
    }
    // Ship::FindOuterWalls 0x484180 builds each room's OuterHull list column by
    // column (x outer, y inner); BreachRandomHull draws an index into that list.
    const int hull = int(combatRandom_.next() % std::uint32_t(columns * rows));
    return breachHullTile(targetShip, roomId, (hull % rows) * columns + hull / rows, false);
}

namespace {
struct CrewSmokeShape {
    float birthRate;
    float lifespan;
    float speed;
    bool randomAngle;
};

CrewSmokeShape crewSmokeShape(std::string_view species) {
    // CrewAnimation::OnInit 0x478200; the RockAnimation ctor 0x47bb80 then
    // overrides it so the rock only ever births once on its own.
    return species == "rock" ? CrewSmokeShape{std::numeric_limits<float>::max(), .8f, 1.3f, true}
                             : CrewSmokeShape{3.f, 1.f, 1.f, false};
}

void configureCrewSmoke(CrewSmoke& smoke, std::string_view species) {
    if (smoke.configured) {
        return;
    }
    smoke.configured = true;
    if (species == "rock") {
        smoke.x = smoke.y = 15.f;
    }
}
} // namespace

int Run::crewFireDirection(const RunCrew& member, int physicalShip) const {
    // CrewMember::OnLoop 0x4747f0 turns a non-intruder with iOnFire towards
    // its repair: CrewAnimation::SetDirection 0x473c50 from ShipGraph::
    // TranslateToGrid 0x45b1f0 of its own point to Repairable::GetGridLocation
    // 0x466f10. Fires and breaches sit on their tile; a system is taken at its
    // room's first tile (ShipSystem pLoc not traced; it only matters while a
    // system repair outlives the fire check, which this port never does).
    const auto& layout = physicalShip == 0 ? ship_ : enemy_.ship;
    const auto room = std::ranges::find(layout.rooms, member.repairRoom, &RoomDefinition::id);
    if (room == layout.rooms.end()) {
        return 0;
    }
    const int columns = std::max(1, room->w / 35);
    const int tile = member.repairKind == 3 ? 0 : std::max(0, member.repairTarget);
    const float goalX = float(room->x / 35 + tile % columns);
    const float goalY = float(room->y / 35 + tile / columns);
    const auto point = crewCombatPoint(member);
    const float dx = goalX - float(int(point.x) / 35);
    const float dy = goalY - float(int(point.y) / 35);
    if (std::abs(dx) <= std::abs(dy)) {
        return dy < 0 ? 2 : 0;
    }
    return dx <= 0 ? 3 : 1;
}

void Run::updateCrewFiring(CrewSmoke& smoke, int direction) {
    // CrewAnimation::UpdateFiring 0x46c570 (status 4, never while stunned):
    // running = !bFrozen && !bStunned, and the nozzle point and spray angle of
    // the direction. RockAnimation::UpdateFiring 0x475fd0 replaces all of it.
    smoke.running = true;
    switch (direction) {
    case 0:
        smoke.x = 7.f;
        smoke.y = 17.f;
        smoke.angle = 90.f;
        break;
    case 1:
        smoke.x = 15.f;
        smoke.y = 15.f;
        smoke.angle = 0.f;
        break;
    case 2:
        smoke.x = 14.f;
        smoke.y = 4.f;
        smoke.angle = 270.f;
        break;
    default:
        smoke.x = 4.f;
        smoke.y = 10.f;
        smoke.angle = 180.f;
        break;
    }
}

void Run::birthCrewSmoke(CrewSmoke& smoke, std::string_view species, float startingLife) {
    // ParticleEmitter::Birth 0x586820: the first free particle, or nothing
    // and no draw. Each axis jitters by twice (r % 100) / 50 - .5 and then
    // draws its zero min..max range; the angle draw spreads 90 degrees around
    // emitAngle unless randAngle replaces it with another draw.
    configureCrewSmoke(smoke, species);
    const auto free = std::ranges::find_if(smoke.particles, [](const CrewSmokeParticle& particle) {
        return !particle.alive;
    });
    if (free == smoke.particles.end()) {
        return;
    }
    const auto shape = crewSmokeShape(species);
    const auto jitter = [](std::uint32_t value) {
        const float offset = float(value % 100) / 50.f - .5f;
        return offset + offset;
    };
    const float x = jitter(combatRandom_.next()) + smoke.x;
    (void)combatRandom_.next();
    const float y = jitter(combatRandom_.next()) + smoke.y;
    (void)combatRandom_.next();
    float degrees = float(combatRandom_.next() % 90) + (smoke.angle - 45.f);
    if (degrees < 0) {
        degrees += 360.f;
    }
    if (shape.randomAngle) {
        degrees = float(combatRandom_.next() % 360);
    }
    const float radians = degrees * 3.1415927f / 180.f;
    free->x = x;
    free->y = y;
    free->dx = std::cos(radians) * shape.speed;
    free->dy = std::sin(radians) * shape.speed;
    free->life = startingLife * shape.lifespan;
    free->alive = true;
}

void Run::seedCrewSmoke(CrewSmoke& smoke, std::string_view species, int amount) {
    // ParticleEmitter::Seed 0x586b70 draws each starting life even when the
    // following Birth finds no free particle.
    for (int index = 0; index < amount; ++index) {
        birthCrewSmoke(smoke, species, float(combatRandom_.next() % 1001) / 1000.f);
    }
}

void Run::advanceCrewSmoke(CrewSmoke& smoke, std::string_view species) {
    // ParticleEmitter::OnUpdate 0x586c00: move and age the live particles,
    // then birth one whenever the factor-unit counter runs out.
    configureCrewSmoke(smoke, species);
    for (auto& particle : smoke.particles) {
        if (!particle.alive) {
            continue;
        }
        particle.x += Time::originalStep * particle.dx;
        particle.y += Time::originalStep * particle.dy;
        const float life = particle.life - Time::originalStep * .0625f;
        if (life < 0) {
            particle.life = -1;
            particle.alive = false;
        } else {
            particle.life = life;
        }
    }
    if (!smoke.running) {
        return;
    }
    smoke.birthCounter -= Time::originalStep;
    if (smoke.birthCounter <= 0) {
        smoke.birthCounter = crewSmokeShape(species).birthRate;
        birthCrewSmoke(smoke, species, 1.f);
    }
}

std::string Run::crewFireAnimation(const RunCrew& member, const CrewSmoke& smoke) {
    // CrewAnimation::OnInit 0x478200 fills anims[direction][4] with the
    // fire_<direction> strips; RockAnimation forces direction 0.
    static constexpr std::array<std::string_view, 4> strips{
        "fire_down", "fire_right", "fire_up", "fire_left"};
    if (smoke.fireDirection < 0 || smoke.fireDirection > 3 || member.health <= 0) {
        return {};
    }
    return std::string(strips[std::size_t(smoke.fireDirection)]);
}

void Run::renderCrewSmoke(const CrewSmoke& smoke, float x, float y) {
    // CrewAnimation::OnRender 0x46c7f0 draws the emitter in status 4 (not on
    // the outline pass) before the body, from the body's 35px cell corner;
    // ParticleEmitter::OnRender 0x586730 centres the 24px cloud and uses
    // alpha 4 * lifespan.
    auto& cloud = renderer_.image("img/people/firefight_cloud.png");
    for (const auto& particle : smoke.particles) {
        if (!particle.alive) {
            continue;
        }
        const auto alpha = std::uint8_t(std::clamp(4.f * particle.life, 0.f, 1.f) * 255.f);
        renderer_.sprite(cloud,
                         {x + particle.x - 12.f, y + particle.y - 12.f, 24, 24},
                         {0, 0, 24, 24},
                         0,
                         {255, 255, 255, alpha});
    }
}

void Run::advanceCrewRepair(RunCrew& member, int owner, int physicalShip) {
    if (member.repairKind == 0) {
        return;
    }
    const auto clear = [&] {
        member.repairKind = 0;
        member.repairTarget = member.repairRoom = -1;
    };
    const auto& species = member.appearance.species;
    const bool drone = species == "repair" || species == "battle" || species == "boarder_ion";
    const bool foreign = !drone && member.intruder;
    if (physicalShip < 0) {
        physicalShip = owner ^ int(member.intruder);
    }
    const int room = foreign ? member.boardRoom : member.room;
    const auto& layout = physicalShip == 0 ? ship_ : enemy_.ship;
    const auto& path = foreign ? member.boardPath : member.path;
    if (member.dead || member.dying || member.outOfGame || !path.empty() ||
        room != member.repairRoom) {
        clear();
        return;
    }
    const int targetRoom = foreign ? member.boardTargetRoom : member.targetRoom;
    const int targetSlot = foreign ? member.boardTargetSlot : member.targetSlot;
    const auto shape = std::ranges::find(layout.rooms, targetRoom, &RoomDefinition::id);
    if (shape == layout.rooms.end() || targetSlot < 0) {
        clear();
        return;
    }
    const int columns = std::max(1, shape->w / 35);
    const SDL_FPoint point = foreign && member.boardPositionValid
                                 ? SDL_FPoint{member.boardX, member.boardY}
                                 : SDL_FPoint{member.x, member.y};
    const SDL_FPoint goal{shape->x + (targetSlot % columns) * 35.f + 17.f,
                          shape->y + (targetSlot / columns) * 35.f + 17.f};
    if (std::abs(point.x - goal.x) >= .5f || std::abs(point.y - goal.y) >= .5f) {
        clear();
        return;
    }
    // CrewMember::UpdateRepair 0x473f60 does nothing while bFighting, which
    // OnLoop 0x4747f0 sets at the goal with a live opponent for any actor that
    // CanFight (BoarderDrone/BattleDrone 0x725510/0x722c40 true, IonDrone
    // 0x734a00 false). A drone body is in no crew list, so place it explicitly.
    const bool fighting = drone ? species != "boarder_ion" && species != "repair" &&
                                      crewHasOpponent(member, physicalShip, owner)
                                : crewHasOpponent(member);
    if (member.stunTime > 0 || fighting) {
        return;
    }
    const bool sabotage = (owner ^ int(member.controlled)) != physicalShip;
    auto& systems = physicalShip == 0 ? systems_ : enemy_.systems;
    auto& hazards = physicalShip == 0 ? playerHazards_ : enemyHazards_;
    bool completed = false;
    if (sabotage) {
        if (member.repairKind != 3 || species == "repair" || species == "boarder_ion") {
            clear();
            return;
        }
        if (member.repairTarget < 0 || std::size_t(member.repairTarget) >= systems.size()) {
            clear();
            return;
        }
        auto& system = systems[std::size_t(member.repairTarget)];
        if (system.definition.room != room || system.health <= 0) {
            clear();
            return;
        }
        if (damageSystemOverTime(system, .5f, physicalShip == 1) && !drone) {
            combatExperience(member);
        }
        if (system.health == 0) {
            clear();
        }
        return;
    }
    if (species == "battle" || species == "boarder_ion") {
        clear();
        return;
    }
    float speed = species == "engi" || species == "repair" ? 2.f : species == "mantis" ? .5f : 1.f;
    constexpr std::array<float, 3> skillSpeed{1.f, 1.1f, 1.2f};
    if (!drone) {
        speed *= skillSpeed[std::size_t(member.repairSkill)];
    }
    if (member.onFire > 0) {
        speed *= species == "rock" ? 2.f : species == "crystal" ? 1.f : 1.2f;
    }
    if (member.repairKind == 3) {
        if (member.repairTarget < 0 || std::size_t(member.repairTarget) >= systems.size()) {
            clear();
            return;
        }
        auto& system = systems[std::size_t(member.repairTarget)];
        // CrewMember::UpdateRepair 0x473f60 stops once ShipSystem::NeedsRepairing
        // 0x613ea0 fails (repaired, bOnFire from the last CheckSpreadDamage, or
        // bBreached from this frame's UpdateEnvironment) or, for anything but a
        // fire, while its room burns (iOnFire). SelectRepair 0x601f00 then
        // takes the room's fire, then its breach.
        const auto hazard = hazards.find(room);
        const bool breached = hazard != hazards.end() && hazard->second.breaches > 0;
        if (system.definition.room != room || system.health >= system.definition.power ||
            system.onFire || breached || member.onFire > 0) {
            clear();
            return;
        }
        completed = partialRepairSystem(system, speed, false, physicalShip == 1);
    } else {
        auto found = hazards.find(room);
        if (found == hazards.end()) {
            clear();
            return;
        }
        auto& hazard = found->second;
        hazard.syncFireTiles();
        if (member.repairKind == 1) {
            const auto tile = std::ranges::find(hazard.fireTiles, member.repairTarget);
            if (tile == hazard.fireTiles.end()) {
                clear();
                return;
            }
            if (species == "rock" && member.fireWorkTick != tick_) {
                member.fireWorkTick = tick_;
                // RockAnimation475fd0 inspects frame nine before updating its
                // ten-frame, one-second fire_down strip. The tracker retains
                // its phase while other tasks or stun prevent fire work.
                const int frame = std::min(9, int(member.fireWorkElapsed * 10.f));
                if (frame <= 8) {
                    member.fireWorkLanded = false;
                } else if (!member.fireWorkLanded) {
                    if (owner == 0) {
                        audio_.sound("rockStomp" + std::to_string(combatRandom_.next() % 2 + 1));
                    }
                    member.fireWorkLanded = true;
                    // ...then ParticleEmitter::Seed(smokeEmitter, 50) for every rock.
                    seedCrewSmoke(member.smoke, species, 50);
                }
                // AnimationTracker59ade0 retains the terminal frame for one
                // step, then loops before advancing again.
                member.fireWorkElapsed = Time::loopAnimation(member.fireWorkElapsed, 1.f);
            }
            const auto index = std::size_t(tile - hazard.fireTiles.begin());
            hazard.fireHealth[index] -= speed * .5f * Time::originalStep;
            // Repairable466f30 reports completion only on exact zero; an
            // overshoot still ends the fire but does not award skill.
            // UpdateRepair 0x473f60 keeps currentRepair on the spent fire: the
            // next frame's NeedsRepairing (fDamage > 0) releases the member, so
            // CrewAI sees it busy for this frame (the missing tile, below).
            completed = hazard.fireHealth[index] == 0;
            if (hazard.fireHealth[index] <= 0) {
                hazard.extinguishFire(index);
                if (index < hazard.fireDeathTimers.size()) {
                    hazard.fireDeathTimers.erase(hazard.fireDeathTimers.begin() +
                                                 std::ptrdiff_t(index));
                }
            }
            hazard.fireRepair = hazard.fireHealth.empty() ? 0 : 100 - hazard.fireHealth.front();
        } else if (member.repairKind == 2) {
            const auto tile = std::ranges::find(hazard.breachTiles, member.repairTarget);
            if (tile == hazard.breachTiles.end() || member.onFire > 0) {
                clear();
                return;
            }
            const auto index = std::size_t(tile - hazard.breachTiles.begin());
            hazard.breachHealth.resize(hazard.breachTiles.size(), 100.f);
            hazard.breachHealth[index] -= speed * .5f * Time::originalStep;
            // The sealed OuterHull likewise stays the currentRepair until the
            // next frame's NeedsRepairing.
            completed = hazard.breachHealth[index] == 0;
            if (hazard.breachHealth[index] <= 0) {
                hazard.breachTiles.erase(tile);
                hazard.breachHealth.erase(hazard.breachHealth.begin() + std::ptrdiff_t(index));
                hazard.breaches = int(hazard.breachTiles.size());
            }
            hazard.breachRepair =
                hazard.breachHealth.empty() ? 0 : 100 - hazard.breachHealth.front();
        }
    }
    if (completed && owner == 0 && !drone) {
        const int previousTier = member.repairSkill;
        if (member.repairSkillProgress < repairSkillCap(member)) {
            member.skillUpRemaining[4] = 1.f;
        }
        member.repairSkillProgress =
            std::min(repairSkillCap(member), member.repairSkillProgress + 1);
        member.repairSkill = repairSkillTier(member);
        recordEarnedSkill(member.earnedSkillTiers, 4, previousTier, member.repairSkill);
        if (member.repairSkill > previousTier) {
            audio_.sound("levelup");
        }
        incrementNativeCounter(member.statistics[0]);
    }
}

void Run::repairSystems([[maybe_unused]] std::vector<RunSystem>& systems,
                        std::vector<RunCrew>& crew,
                        bool enemy) {
    for (auto& member : crew) {
        advanceCrewRepair(member, enemy ? 1 : 0);
    }
}

void Run::updateAudio() {
    if (!open_) {
        // CommandGui::OnCleanup leaves managed loop counts intact; returning
        // through a focused window retains their paused state until a new run.
        audio_.pauseLoops(true);
        audio_.updateLoop("distress", 0.f);
        audioWeaponIds_.clear();
        audioWeaponReady_.clear();
        audioIncomingFire_ = false;
        return;
    }
    // BossShip::IncomingFire405c60 uses the final five seconds of the
    // phase-two/three power timer; WarningMessage::Start5b43c0 sounds once.
    const bool incomingFire = campaign_.fighting && enemy_.present && campaign_.stage > 1 &&
                              campaign_.surgeGoal - campaign_.surgeElapsed < 5.f;
    if (incomingFire && !audioIncomingFire_) {
        audio_.sound("surgeWarning");
    }
    audioIncomingFire_ = incomingFire;
    // WeaponBox::SetWeapon4f7eb0 primes wasCharged; OnLoop4f88b0 sounds only
    // a readiness edge without a queued shot (FireNextShot40c140).
    const bool rebound =
        audioWeaponIds_ != ship_.weapons || audioWeaponReady_.size() != combatWeapons_.size();
    if (rebound) {
        audioWeaponIds_ = ship_.weapons;
        audioWeaponReady_.assign(combatWeapons_.size(), false);
    }
    for (std::size_t i = 0; i < combatWeapons_.size() && i < ship_.weapons.size(); ++i) {
        const auto& weapon = combatWeapons_[i];
        const auto& blueprint = catalog_.equipment(ship_.weapons[i]);
        const bool ready = i < weaponPower_.size() && weaponPower_[i] && weapon.chargeLevel > 0 &&
                           missiles_ >= blueprint.missiles;
        const bool firing = ready && (weapon.queued || weapon.autofire) &&
                            (weapon.targetRoom >= 0 || !weapon.targetPoints.empty());
        if (!rebound && ready && !audioWeaponReady_[i] && !firing) {
            audio_.sound("weaponReady");
        }
        audioWeaponReady_[i] = ready;
    }
    int count = 0;
    if (open_ && !defeated_) {
        for (const auto& [room, hazard] : playerHazards_) {
            (void)room;
            count += hazard.fires;
        }
    }
    // Spreader<Fire>::UpdateSpread0x7355b0 updates audio only for shipId0.
    // UpdateSoundLoop0x5afc00 scales the archive loop gain by fire count/max4.
    audio_.updateLoop("fire", static_cast<float>(count));
    // SpaceManager::OnLoop41c940 and StarMap::OnLoop5d13a0 manage these
    // ambient loops; StarMap::Close stops distress even while it remains armed.
    audio_.updateLoop("nebula", nebula_ && !options_.lowend ? 1.f : 0.f);
    audio_.updateLoop("distress", mapOpen_ && fuel_ == 0 && distress_ ? 1.f : 0.f);
    updateCrewAudio();
    // WorldManager::CheckMusic0x5ee100 switches combat tracks for dangerous
    // space, physical intruders, controlled enemies or a live hostile ship.
    const bool dangerous = environment_.fireActive || environment_.asteroidActive ||
                           environment_.pulsarActive ||
                           (environment_.pdsActive && environment_.pdsTarget != 1);
    const bool intruders = std::ranges::any_of(voyageBoarders_,
                                               [](const auto& crew) {
                                                   return crew.health > 0;
                                               }) ||
                           std::ranges::any_of(enemy_.crew, [](const auto& crew) {
                               return crew.intruder && crew.health > 0;
                           });
    const bool controlledEnemies = std::ranges::any_of(voyageBoarders_,
                                                       [](const auto& crew) {
                                                           return crew.controlled;
                                                       }) ||
                                   std::ranges::any_of(enemy_.crew, [](const auto& crew) {
                                       return crew.intruder && crew.controlled;
                                   });
    audio_.combatMusic(
        open_ && (dangerous || intruders || controlledEnemies ||
                  (enemy_.present && hostile_ && (enemy_.hull > 0 || enemyExplosionActive_))));
    audio_.pauseLoops(!open_ || defeated_ || event_ || userPause_ || externalModal_ || mapOpen_ ||
                      !panel_.empty() || devUiOpen_);
}

void Run::advanceFireExtinguishers(int owner) {
    const auto& ship = owner == 0 ? ship_ : enemy_.ship;
    auto& hazards = owner == 0 ? playerHazards_ : enemyHazards_;
    auto& emitters = fireExtinguishers_[std::size_t(owner == 1)];
    // ShipManager::OnLoop 0x4a05b0 tests HasEquipment("FIRE_EXTINGUISHERS")
    // first. For every room with fire it runs ParticleEmitter::OnUpdate
    // 0x586c00 and gives each burning tile PartialRepair(augment value, false).
    if (std::ranges::find(ship.augments, std::string("FIRE_EXTINGUISHERS")) ==
        ship.augments.end()) {
        return;
    }
    const float strength = catalog_.equipment("FIRE_EXTINGUISHERS").value;
    std::vector<int> roomIds;
    roomIds.reserve(ship.rooms.size());
    for (const auto& room : ship.rooms) {
        roomIds.push_back(room.id);
    }
    std::ranges::sort(roomIds);
    for (const int roomId : roomIds) {
        const auto found = hazards.find(roomId);
        if (found == hazards.end() || found->second.fires <= 0) {
            continue;
        }
        const auto& room = findRoom(ship, roomId);
        auto& emitter = emitters[roomId];
        // OnUpdate moves and ages live particles (lifespan in seconds), then
        // births one particle each time the factor-unit counter runs out.
        for (auto& particle : emitter.particles) {
            if (!particle.alive) {
                continue;
            }
            particle.x += Time::originalStep * particle.dx;
            particle.y += Time::originalStep * particle.dy;
            particle.life -= Time::step;
            if (particle.life < 0) {
                particle.life = -1;
                particle.alive = false;
            }
        }
        emitter.birthCounter -= Time::originalStep;
        if (emitter.birthCounter <= 0) {
            emitter.birthCounter = 3.f;
            // Birth 0x586820: first free slot; x/y jitter (r%100)/50-0.5 twice
            // around the integer room centre (dx/dy ranges are zero, each still
            // drawing), then a 90-degree draw replaced by a random angle.
            const auto free = std::ranges::find_if(emitter.particles, [](const auto& particle) {
                return !particle.alive;
            });
            if (free != emitter.particles.end()) {
                const float centreX = float(room.x + room.w / 2);
                const float centreY = float(room.y + room.h / 2);
                const float jitterX = float(combatRandom_.next() % 100) / 50.f - .5f;
                (void)combatRandom_.next();
                const float jitterY = float(combatRandom_.next() % 100) / 50.f - .5f;
                (void)combatRandom_.next();
                (void)combatRandom_.next();
                const float degrees = float(combatRandom_.next() % 360);
                const float radians = degrees * 3.1415927f / 180.f;
                free->x = centreX + (jitterX + jitterX);
                free->y = centreY + (jitterY + jitterY);
                free->dx = std::cos(radians) * 1.3f;
                free->dy = std::sin(radians) * 1.3f;
                free->life = .8f;
                free->alive = true;
            }
        }
        auto& hazard = found->second;
        hazard.syncFireTiles();
        hazard.fireDeathTimers.resize(hazard.fireTiles.size(), -1.f);
        for (std::size_t index = hazard.fireTiles.size(); index-- > 0;) {
            // Repairable::PartialRepair 0x466f30: damage -= speed * 0.5 per unit.
            hazard.fireHealth[index] -= strength * .5f * Time::originalStep;
            if (hazard.fireHealth[index] <= 0) {
                const int tile = hazard.fireTiles[index];
                hazard.extinguishFire(index);
                hazard.fireDeathTimers.erase(hazard.fireDeathTimers.begin() +
                                             std::ptrdiff_t(index));
                hazard.fireStartTimers.erase(tile);
            }
        }
        hazard.fireRepair = hazard.fireHealth.empty() ? 0 : 100 - hazard.fireHealth.front();
    }
}

void Run::advanceHazards(int owner) {
    const auto spread = [&](bool player) {
        auto& hazards = player ? playerHazards_ : enemyHazards_;
        const auto& ship = player ? ship_ : enemy_.ship;
        const auto& oxygen = player ? roomOxygen_ : enemy_.roomOxygen;
        const auto& fireOxygen = fireOxygen_[std::size_t(!player)];
        // Fire::fOxygen is the integer room level the previous environment
        // pass stored after that room's fire consumption (UpdateEnvironment
        // 0x497f50); a fresh ship falls back to its current level.
        const auto tileOxygen = [&](int roomId) {
            const auto stored = fireOxygen.find(roomId);
            if (stored != fireOxygen.end()) {
                return float(stored->second);
            }
            const auto current = oxygen.find(roomId);
            return current != oxygen.end() ? float(int(current->second)) : 0.f;
        };
        const auto& doorOpen = player ? doors_ : enemy_.doors;
        const auto& doorForced = player ? voyageDoorForcedTicks_ : enemy_.doorForcedTicks;
        const auto& doorSystems = player ? systems_ : enemy_.systems;
        const auto* doorSystem = findSystem(doorSystems, 8);
        int doorLevel = player       ? subsystemLevel(8)
                        : doorSystem ? std::clamp(std::min(doorSystem->power, doorSystem->health) +
                                                      int(doorSystem->operatorActive),
                                                  0,
                                                  4)
                                     : 0;
        std::map<std::pair<int, int>, std::pair<int, int>> squares;
        for (const auto& room : ship.rooms) {
            const int columns = room.w / 35;
            for (int column = 0; column < columns; ++column) {
                for (int row = 0; row < room.h / 35; ++row) {
                    squares[{room.x / 35 + column, room.y / 35 + row}] = {room.id,
                                                                          row * columns + column};
                }
            }
        }
        // Spreader<Fire>::UpdateSpread0x7355b0 scans in authored room order,
        // x-major/y-minor. Newly ignited neighbors affect later tiles this pass.
        for (const auto& room : ship.rooms) {
            const int columns = room.w / 35;
            for (int column = 0; column < columns; ++column) {
                for (int row = 0; row < room.h / 35; ++row) {
                    const std::pair<int, int> point{room.x / 35 + column, room.y / 35 + row};
                    const int tile = row * columns + column;
                    auto& hazard = hazards[room.id];
                    hazard.syncFireTiles();
                    int connected = 0;
                    for (const auto direction :
                         std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
                        const std::pair<int, int> neighborPoint{point.first + direction.first,
                                                                point.second + direction.second};
                        const auto neighbor = squares.find(neighborPoint);
                        if (neighbor == squares.end()) {
                            continue;
                        }
                        const auto [neighborRoom, neighborTile] = neighbor->second;
                        const auto foundHazard = hazards.find(neighborRoom);
                        if (foundHazard == hazards.end() ||
                            std::ranges::find(foundHazard->second.fireTiles, neighborTile) ==
                                foundHazard->second.fireTiles.end()) {
                            continue;
                        }
                        float rate = .1f;
                        if (neighborRoom != room.id) {
                            const bool vertical = direction.first != 0;
                            const int x = (vertical ? std::max(point.first, neighborPoint.first)
                                                    : point.first) *
                                          35;
                            const int y =
                                (vertical ? point.second
                                          : std::max(point.second, neighborPoint.second)) *
                                35;
                            const auto door =
                                std::ranges::find_if(ship.doors, [&](const auto& entry) {
                                    return entry.vertical == vertical && entry.x == x &&
                                           entry.y == y;
                                });
                            if (door == ship.doors.end()) {
                                continue;
                            }
                            // ConnectedGridSquares 0x459760 treats bOpen or a
                            // running forcedOpen (a door broken open) as open.
                            const bool open =
                                door->id >= 0 &&
                                ((static_cast<std::size_t>(door->id) < doorOpen.size() &&
                                  doorOpen[static_cast<std::size_t>(door->id)]) ||
                                 doorForced.contains(door->id));
                            // A closed hacked door (ConnectedGridSquares 6) spreads
                            // at the open rate.
                            if (!open && doorHackLevel(*door, !player) <= 0) {
                                rate = doorLevel >= 2 ? .01f : .057142857f;
                            }
                        }
                        ++connected;
                        if (std::ranges::find(hazard.fireTiles, tile) != hazard.fireTiles.end()) {
                            continue;
                        }
                        auto& timer = hazard.fireStartTimers[tile];
                        if (timer == 0) {
                            timer = static_cast<float>(combatRandom_.next() % 40 + 10);
                        }
                        timer -= Time::originalStep * rate;
                        if (timer < 0) {
                            timer = 0;
                            hazard.fireTiles.push_back(tile);
                            hazard.fireHealth.push_back(100);
                            hazard.fireDeathTimers.push_back(-1);
                            hazard.fires = static_cast<int>(hazard.fireTiles.size());
                            std::erase_if(hazard.fireSmoke, [tile](const FireSmoke& smoke) {
                                return smoke.tile == tile;
                            });
                        }
                    }
                    const auto burning = std::ranges::find(hazard.fireTiles, tile);
                    if (burning == hazard.fireTiles.end()) {
                        if (connected == 0) {
                            hazard.fireStartTimers.erase(tile);
                        }
                        continue;
                    }
                    const auto index = static_cast<std::size_t>(burning - hazard.fireTiles.begin());
                    hazard.fireDeathTimers.resize(hazard.fireTiles.size(), -1.f);
                    auto& timer = hazard.fireDeathTimers[index];
                    if (timer >= 0) {
                        timer = std::max(0.f,
                                         timer - Time::originalStep *
                                                     static_cast<float>(15 - 3 * connected) * .01f);
                    }
                    if (tileOxygen(room.id) <= 10 && timer == 0) {
                        hazard.extinguishFire(index);
                        hazard.fireDeathTimers.erase(hazard.fireDeathTimers.begin() +
                                                     static_cast<std::ptrdiff_t>(index));
                        hazard.fireStartTimers.erase(tile);
                    } else if (tileOxygen(room.id) <= 10 && timer < 0) {
                        timer = static_cast<float>(combatRandom_.next() % 10 + 5);
                    }
                }
            }
        }
    };
    for (int shipOwner : {0, 1}) {
        if ((owner >= 0 && owner != shipOwner) || (shipOwner == 1 && !enemy_.present)) {
            continue;
        }
        const bool player = shipOwner == 0;
        spread(player);
        auto& hazards = player ? playerHazards_ : enemyHazards_;
        // Spreader7355b0 invokes Fire48e970 for every valid tile, including
        // dormant ones; OnInit48ed10 gives each largeFire its own 1s phase.
        auto& animations = player ? playerFireSpreaderAnimations_ : enemy_.fireSpreaderAnimations;
        for (auto& [room, states] : animations) {
            (void)room;
            for (auto& state : states) {
                state.elapsed = Time::loopAnimation(state.elapsed, 1.f);
            }
        }
        for (auto& [room, hazard] : hazards) {
            // Fire::OnLoop 0x48e970 advances the one-second extinction smoke
            // through environment/game time, retaining effects after damage reaches zero.
            for (auto& smoke : hazard.fireSmoke) {
                smoke.elapsed += Time::step;
            }
            std::erase_if(hazard.fireSmoke, [](const FireSmoke& smoke) {
                return smoke.elapsed > 1.f;
            });
            hazard.syncFireTiles();
        }
    }
}

bool Run::damageSystemOverTime(RunSystem& system, float amount, bool enemy) {
    // ShipSystem::DamageOverTime617640 drops threshold overshoot and latches
    // hull explosion for the next ordered system callback.
    system.fireDamage += amount * Time::originalStep;
    bool completed = false;
    if (system.fireDamage >= 100.f) {
        system.fireDamage = 0;
        const bool wasDestroyed = system.health == 0;
        addSystemDamage(system, 1, enemy);
        system.exploded = !wasDestroyed && system.health == 0;
        completed = true;
    }
    system.damagedLastFrame = true;
    return completed;
}

bool Run::partialRepairSystem(RunSystem& system, float speed, bool automated, bool enemy) {
    // PartialRepair614a80 marks repaired even below the completion threshold.
    if (system.health == system.definition.power) {
        return false;
    }
    float amount = speed * .5f * Time::originalStep;
    if (automated) {
        amount /= 3.f;
    }
    if (system.hackActive) {
        amount *= .5f;
    }
    system.repair += amount;
    system.repairedLastFrame = true;
    if (system.repair < 100.f) {
        return false;
    }
    system.repair = 0;
    system.health = std::min(system.definition.power, system.health + 1);
    checkSystemRepower(system, enemy);
    return true;
}

void Run::advanceSpreadDamage(int owner) {
    if (owner == 1 && !enemy_.present) {
        return;
    }
    const auto& hazards = owner == 0 ? playerHazards_ : enemyHazards_;
    auto& systems = owner == 0 ? systems_ : enemy_.systems;
    // CheckSpreadDamage4939b0 visits systems in physical vector order, then
    // every room tile column-first. Each burning tile is a separate pulse.
    for (auto& system : systems) {
        system.onFire = false;
        const auto found = hazards.find(system.definition.room);
        if (found == hazards.end()) {
            continue;
        }
        const auto& room = findRoom(owner == 0 ? ship_ : enemy_.ship, system.definition.room);
        const int columns = room.w / 35;
        for (int column = 0; column < columns; ++column) {
            for (int row = 0; row < room.h / 35; ++row) {
                if (std::ranges::find(found->second.fireTiles, row * columns + column) !=
                    found->second.fireTiles.end()) {
                    system.onFire = true;
                    damageSystemOverTime(system, .5f, owner == 1);
                }
            }
        }
    }
}

void Run::initializeWeaponFactory(CombatWeapon& weapon,
                                  const EquipmentDefinition& blueprint,
                                  RandomStream& random) {
    // ProjectileFactory::ProjectileFactory0x40b380 ends with this draw even
    // when chargeLevels==1. WeaponAnimation::OnInit0x456940 consumes none.
    weapon.goalChargeLevel = int(random.next() % std::max(1, blueprint.chargeLevels)) + 1;
    // GetWeapon returns the animation registry's retained constructor state;
    // selecting a new charge goal never draws another explosion alias.
    if (weapon.weaponExplosionImage.empty()) {
        weapon.weaponExplosionImage = blueprint.weaponExplosionAnimation;
    }
}

void Run::fireProjectile(int owner, int index, bool artillery) {
    const auto& ship = owner == 0 ? ship_ : enemy_.ship;
    auto& weapon = artillery ? artilleryAt(owner == 0 ? systems_ : enemy_.systems, index).artillery
                   : owner == 0 ? combatWeapons_.at(index)
                                : enemy_.weapons.at(index);
    const auto& blueprint = catalog_.equipment(
        artillery ? artilleryAt(owner == 0 ? systems_ : enemy_.systems, index).definition.weapon
                  : ship.weapons.at(index));
    // Missing mounts must not crash a valid authored/diagnostic weapon slot.
    // Native fallback is the hull center; exact original fallback remains open.
    const int mountedSlot = owner == 0 && !artillery ? logicalArmamentSlot("weapon", index) : index;
    // RenderWeapons49a960 pads the ordinary factory list to four entries
    // before SetMount, which also supplies GetFireLocation4580f0.
    const auto mountIndex =
        std::size_t(artillery ? std::max(4, int(ship.weapons.size())) + index : mountedSlot);
    const WeaponMount fallback{.x = ship.imageRect[2] / 2, .y = ship.imageRect[3] / 2};
    const auto& mount = mountIndex < ship.mounts.size() ? ship.mounts[mountIndex] : fallback;
    // WeaponAnimation::GetFireLocation0x4580f0 and Render0x458140.
    const int fireX = (blueprint.fireX - blueprint.mountX) * (mount.mirror ? -1 : 1);
    const int fireY = blueprint.fireY - blueprint.mountY;
    CombatProjectile base;
    base.owner = base.space = owner;
    base.targetShip = weapon.volleySelf ? owner : 1 - owner;
    base.room = weapon.volleyRoom;
    base.target = weapon.volleyTarget;
    base.targetEnd = weapon.volleyEnd;
    base.entryAngle = weapon.entryAngle;
    base.weapon = blueprint.id;
    base.projectileImage = blueprint.projectileImage;
    base.droneTargetable = blueprint.droneTargetable;
    if (blueprint.boostType == "damage" && weapon.boostLevel > 0) {
        base.damageBonus = int(float(weapon.boostLevel - 1) * blueprint.boostAmount);
    }
    // ShipGraph::GetShipCorner481f70 contributes the minimum room point;
    // imageRect remains the raw authored offset from that graph corner.
    const int graphX =
        ship.rooms.empty() ? 0 : std::ranges::min(ship.rooms, {}, &RoomDefinition::x).x;
    const int graphY =
        ship.rooms.empty() ? 0 : std::ranges::min(ship.rooms, {}, &RoomDefinition::y).y;
    base.position = {float(graphX + ship.imageRect[0] + mount.x + (mount.rotate ? -fireY : fireX)),
                     float(graphY + ship.imageRect[1] + mount.y + (mount.rotate ? fireX : fireY))};
    if (blueprint.chargeLevels > 1) {
        const float offset =
            float(blueprint.chargeOffset * std::max(0, weapon.volleyRemaining - 1)) *
            (mount.mirror ? 1.f : -1.f);
        if (mount.rotate) {
            base.position.y += offset;
        } else {
            base.position.x += offset;
        }
    }
    // ProjectileFactory::Update0x40d420 offsets a missile's muzzle16 pixels
    // along its owner's fixed firing angle before constructing the projectile.
    if (blueprint.type == "MISSILES") {
        if (owner == 0) {
            base.position.x += 16;
        } else {
            base.position.y -= 16;
        }
    }
    base.previous = base.position;
    base.direction = owner == 0 ? SDL_FPoint{1, 0} : SDL_FPoint{0, -1};
    base.speed = (blueprint.projectileSpeed > 0  ? blueprint.projectileSpeed
                  : blueprint.type == "MISSILES" ? 35.f
                                                 : 60.f) *
                 16.f;
    if (blueprint.type == "BOMB") {
        base.speed = 0;
        base.bombBypass =
            std::ranges::find(ship.augments, std::string("ZOLTAN_BYPASS")) != ship.augments.end();
    } else if (blueprint.type == "BEAM") {
        // BeamWeapon::BeamWeapon 0x4357c0 starts at target1 and sweeps at
        // speed_magnitude 5 in original CFPS units (16 units/second).
        initializeBeamProjectile(base,
                                 base.position,
                                 owner == 0 ? 0.f : 270.f,
                                 base.targetShip == owner,
                                 blueprint.projectileSpeed);
    }
    if (blueprint.type == "BURST") {
        const auto& destination = owner == 0 ? enemy_.ship : ship_;
        std::vector<SDL_FPoint> scatterTargets;
        scatterTargets.reserve(
            static_cast<std::size_t>(std::accumulate(blueprint.miniProjectiles.begin(),
                                                     blueprint.miniProjectiles.end(),
                                                     0,
                                                     [](int count, const auto& mini) {
                                                         return count + mini.count;
                                                     })));
        // ProjectileFactory::Update 0x40cccf builds every scattered target
        // before it constructs the mini-projectiles and seeds their spin.
        std::size_t miniIndex = 0;
        for (const auto& mini : blueprint.miniProjectiles) {
            for (int shard = 0; shard < mini.count; ++shard) {
                // ProjectileFactory::Update 0x40ccd0 pops the artillery centre from the
                // *back* of the target list before the scatter loop and then pops one more
                // per record inside it, so record `i` scatters around
                // `targets[size - 1 - i]`. That covers record 0, whose centre is not the
                // list front a non-artillery volley would aim at.
                const SDL_FPoint centerTarget =
                    artillery && miniIndex < weapon.volleyPoints.size()
                        ? weapon.volleyPoints[weapon.volleyPoints.size() - 1 - miniIndex]
                        : base.target;
                // ProcessWeaponBlueprint5e41d0 expands count into distinct original
                // records. Native retains aggregate XML records, so consume a distinct
                // scatter pair for each shard, including fake shards.
                // The angle is a full random32() reduced modulo 360, and the distance
                // scales the full random32() result before sqrt rather than reducing it
                // modulo 10001, which would change seeded patterns.
                const float angle =
                    float(combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
                const float distance =
                    std::sqrt(float(combatRandom_.next()) / 2147483600.f) * blueprint.radius;
                const SDL_FPoint scatter{centerTarget.x + std::cos(angle) * distance,
                                         centerTarget.y + std::sin(angle) * distance};
                scatterTargets.push_back(scatter);
                ++miniIndex;
            }
        }
        std::size_t scatterIndex = 0;
        for (const auto& mini : blueprint.miniProjectiles) {
            for (int shot = 0; shot < mini.count; ++shot) {
                CombatProjectile pellet = base;
                pellet.id = nextProjectile_++;
                pellet.projectileImage = mini.image;
                pellet.fake = mini.fake;
                pellet.target = scatterTargets.at(scatterIndex++);
                initializeProjectileExplosion(pellet, blueprint);
                // 0x40ccd0 randomises the authored mini-flight frame and then
                // stops its tracker; spin remains an independent angle.
                const auto& miniAnimation = catalog_.animation(mini.image);
                if (miniAnimation.usable()) {
                    pellet.flightFrame = int(combatRandom_.next() % unsigned(miniAnimation.frames));
                }
                if (pellet.fake) {
                    pellet.damageBonus = 0;
                }
                pellet.room = -1;
                for (const auto& room : destination.rooms) {
                    if (pellet.target.x >= room.x && pellet.target.x < room.x + room.w &&
                        pellet.target.y >= room.y && pellet.target.y < room.y + room.h) {
                        pellet.room = room.id;
                        break;
                    }
                }
                weapon.pendingProjectiles.push_back(std::move(pellet));
            }
        }
    } else {
        base.id = nextProjectile_++;
        if (blueprint.type == "BEAM") {
            weapon.lastProjectileId = base.id;
        }
        initializeProjectileExplosion(base, blueprint);
        weapon.pendingProjectiles.push_back(std::move(base));
    }
    // Bomb launch audio belongs to construction, unlike other weapon types.
    if (blueprint.type == "BOMB") {
        combatSound(blueprint.launchSounds, 0, true);
    }
}

void Run::advanceWeaponAnimation(CombatWeapon& weapon,
                                 const EquipmentDefinition& blueprint,
                                 bool powered,
                                 int hacking) {
    const int frames = std::max(1, blueprint.animationFrames);
    const int charged = std::clamp(blueprint.chargedFrame, 0, frames - 1);
    const float duration =
        weaponFireDuration(blueprint) * float(frames) / float(std::max(1, frames - charged));
    const float actualCharge = weapon.cooldown > 0 ? weapon.charge / weapon.cooldown : 0.f;
    const float visualCharge = std::max((actualCharge - blueprint.delayChargeAnimation) /
                                            (1.f - blueprint.delayChargeAnimation),
                                        0.f);
    const int chargedFrame =
        actualCharge >= 1.f ? charged : std::clamp(int(charged * visualCharge), 0, charged);
    if (!weapon.animationInitialized) {
        weapon.animationInitialized = true;
        weapon.animationFrame = chargedFrame;
    }
    // SetChargedLevel457370 sets the idle strip before Update4574a0.
    if (!weapon.animationFiring) {
        weapon.animationFrame = chargedFrame;
        weapon.animationElapsed = float(weapon.animationFrame) / frames * duration;
    }
    const int previous = weapon.animationFrame;
    if (weapon.animationFiring) {
        weapon.animationElapsed += Time::step;
        weapon.animationFrame =
            std::clamp(int(weapon.animationElapsed / duration * frames), 0, frames - 1);
        if (blueprint.chargeLevels > 1 && blueprint.type != "BEAM") {
            const int segment = std::max(1, (frames - charged) / blueprint.chargeLevels);
            if (weapon.animationFrame > charged + segment * (weapon.animationBoostLevel + 1) ||
                weapon.animationElapsed > duration) {
                weapon.animationFiring = false;
                weapon.animationFrame = 0;
                weapon.animationElapsed = 0;
            }
        } else if (weapon.animationElapsed > duration) {
            weapon.animationFiring = false;
        }
    }
    if (blueprint.chargeLevels > 1 && blueprint.type != "BEAM") {
        const int segment = std::max(1, (frames - charged) / blueprint.chargeLevels);
        if (weapon.animationFrame != charged &&
            ((segment == 1 && weapon.animationFrame != previous) ||
             ((weapon.animationFrame - charged) % segment == blueprint.fireFrame - charged &&
              (previous - charged) % segment != blueprint.fireFrame - charged))) {
            weapon.animationFireShot = true;
        }
    } else if (weapon.animationFrame == blueprint.fireFrame && blueprint.fireFrame > previous) {
        weapon.animationFireShot = true;
    }
    weapon.animationHackLevel = hacking;
    if (weapon.weaponExplosionElapsed >= 0 && !weapon.weaponExplosionImage.empty()) {
        weapon.weaponExplosionElapsed =
            std::min(catalog_.animation(weapon.weaponExplosionImage).duration,
                     weapon.weaponExplosionElapsed + Time::step);
    }
    // Update4574a0 uses the previous visual powered state; SetPowered follows
    // it in ProjectileFactory::Update40ccd0. SetHacked457870 stops sparks at0.
    const auto& sparks = catalog_.animation("stun_spark_big");
    if (hacking <= 1 || !weapon.animationPowered || !sparks.usable()) {
        weapon.hackSparksElapsed = -1;
    } else {
        if (weapon.hackSparksElapsed < 0) {
            weapon.hackSparksElapsed = 0;
        }
        weapon.hackSparksElapsed += Time::step;
        if (weapon.hackSparksElapsed > sparks.duration) {
            const int verticalRange = blueprint.frameHeight - blueprint.frameWidth;
            if (verticalRange > 0) {
                weapon.hackSparksPosition = {
                    0, -float(combatRandom_.next() % unsigned(verticalRange))};
            }
            weapon.hackSparksMirror = (combatRandom_.next() & 1U) != 0;
            weapon.hackSparksElapsed = 0;
        }
    }
    // SetPowered457200 cancels the active firing strip only on the falling
    // edge. Its half-second slide retains its position across both edges.
    if (weapon.animationPowered && !powered) {
        weapon.animationFiring = false;
        weapon.animationFrame = 0;
        weapon.animationElapsed = 0;
    }
    weapon.animationPowered = powered;
    // StartFire457420 follows Update; it refuses a second start while either
    // the strip or its unconsumed fire-frame signal remains active.
    if (!weapon.pendingProjectiles.empty() && !weapon.animationFiring &&
        !weapon.animationFireShot) {
        weapon.animationFiring = true;
        weapon.animationFrame = charged;
        weapon.animationElapsed = float(charged) / frames * duration;
    }
    if (!weapon.animationFiring) {
        weapon.animationBoostLevel =
            blueprint.chargeLevels > 1 ? weapon.chargeLevel - 1 : weapon.boostLevel - 1;
    }
}

void Run::advanceWeaponSlides(float seconds) {
    // OnRender458140 advanced this cosmetic tracker. The accepted native
    // clock contract advances it explicitly, keeping drawing pure.
    const auto advance = [seconds](CombatWeapon& weapon) {
        weapon.animationSlide = std::clamp(
            weapon.animationSlide + (weapon.animationPowered ? seconds : -seconds), 0.f, .5f);
    };
    for (auto& weapon : combatWeapons_) {
        advance(weapon);
    }
    for (auto& weapon : enemy_.weapons) {
        advance(weapon);
    }
    for (auto* systems : {&systems_, &enemy_.systems}) {
        for (auto& system : *systems) {
            if (system.definition.id == 11) {
                advance(system.artillery);
            }
        }
    }
}

void Run::jumpWeaponFactories(bool enemy) {
    auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
    const auto& powered = enemy ? enemy_.powered : weaponPower_;
    const auto& ship = enemy ? enemy_.ship : ship_;
    const bool preigniter =
        std::ranges::find(ship.augments, "WEAPON_PREIGNITE") != ship.augments.end();
    for (std::size_t index = 0; index < weapons.size() && index < ship.weapons.size(); ++index) {
        auto& weapon = weapons[index];
        const auto& blueprint = catalog_.equipment(ship.weapons[index]);
        // WeaponSystem::Jump60f7e0 does not call ShipSystem::Jump or
        // SetPowered: the existing firing strip and power allocation survive.
        if (!preigniter) {
            weapon.charge = 0;
            weapon.chargeLevel = 0;
        } else if (index < powered.size() && powered[index]) {
            weapon.chargeLevel = 1;
            weapon.charge = blueprint.chargeLevels > 1 ? 0.f
                            : weapon.cooldown > 0      ? weapon.cooldown
                                                       : blueprint.cooldown;
            if (blueprint.chargeLevels > 1) {
                weapon.animationBoostLevel = 0;
            }
        }
        // bFiredOnce is only ever set (GetProjectile 0x40c2e0); Jump keeps it.
        weapon.clearTargets(blueprint.chargeLevels > 1 && weapon.chargeLevel == 0);
        weapon.pendingProjectiles.clear(); // ClearProjectiles40c0f0 only empties this queue.
    }
}

void Run::extractWeaponProjectiles() {
    const auto extract = [&](CombatWeapon& weapon,
                             const EquipmentDefinition& blueprint,
                             int owner,
                             int index,
                             bool artillery) {
        const int burstCount = std::accumulate(blueprint.miniProjectiles.begin(),
                                               blueprint.miniProjectiles.end(),
                                               0,
                                               [](int count, const auto& mini) {
                                                   return count + mini.count;
                                               });
        while (!weapon.pendingProjectiles.empty()) {
            // FireShot4578c0 consumes its signal even when the queue is empty;
            // this loop models repeated GetNextShot calls while a shot exists.
            const bool fireShot = std::exchange(weapon.animationFireShot, false);
            if (!fireShot && (blueprint.type != "BURST" ||
                              int(weapon.pendingProjectiles.size()) >= burstCount)) {
                break;
            }
            const int queued = int(weapon.pendingProjectiles.size());
            const int shots = blueprint.chargeLevels > 1 ? std::max(1, weapon.chargeLevel)
                                                         : std::max(1, blueprint.shots);
            // Normal burst/laser launch sounds occur at the firing signal;
            // subsequent shards and bombs skip this GetProjectile sound path.
            if (fireShot && blueprint.type != "BOMB" &&
                (blueprint.type != "BURST" ||
                 int(weapon.pendingProjectiles.size()) >= burstCount)) {
                combatSound(blueprint.launchSounds, 0, true);
            }
            auto& cloak = owner == 0 ? cloak_ : enemy_.cloak;
            const auto& ship = owner == 0 ? ship_ : enemy_.ship;
            if (queued == shots && cloak.active &&
                std::ranges::find(ship.augments, std::string("CLOAK_FIRE")) ==
                    ship.augments.end()) {
                cloak.elapsed += cloak.duration / 5.f;
            }
            if (!weapon.emittedVolley && !tutorialOwned_) {
                progressionRun_.tutorial = false;
                const std::string prefix = owner == 0 ? "combat_player_" : "combat_enemy_";
                progressionRun_.flag(prefix + (artillery ? "artillery_shots" : "weapon_shots"));
                if (owner == 0 && !artillery) {
                    const auto key = "combat_volley_" + std::to_string(index);
                    progressionRun_.flag(key);
                    progressionRun_.flags["combat_max_volley"] = std::max(
                        progressionRun_.value("combat_max_volley"), progressionRun_.value(key));
                    if (blueprint.type != "MISSILES" && blueprint.id != "MISSILE_CHARGEGUN") {
                        progressionRun_.flag("combat_non_missile");
                    }
                }
                weapon.emittedVolley = true;
            }
            weapon.firedOnce = true;
            if (owner == 0) {
                progressionRun_.flag("fired_shot");
            }
            projectiles_.push_back(std::move(weapon.pendingProjectiles.back()));
            weapon.pendingProjectiles.pop_back();
        }
        // GetNextShot polls every factory, including factories with no shots.
        weapon.animationFireShot = false;
    };
    for (std::size_t shipIndex = 0; shipIndex < spaceShipOwners_.size();) {
        const int owner = spaceShipOwners_[shipIndex];
        const bool destructionDone =
            owner == 0 ? playerExplosionStarted_ &&
                             Time::seconds(double(tick_ - playerExplosionStartTick_)) > 6
                       : enemyExplosionActive_ && explosion_ < 0;
        if (destructionDone) {
            spaceShipOwners_.erase(spaceShipOwners_.begin() + std::ptrdiff_t(shipIndex));
            continue;
        }
        ++shipIndex;
        if (owner == 1 && !enemy_.present) {
            continue;
        }
        registerSpaceDrones(owner);
        auto& weapons = owner == 0 ? combatWeapons_ : enemy_.weapons;
        const auto& ship = owner == 0 ? ship_ : enemy_.ship;
        for (std::size_t index = 0; index < weapons.size(); ++index) {
            extract(
                weapons[index], catalog_.equipment(ship.weapons[index]), owner, int(index), false);
        }
        int index = 0;
        for (auto& system : owner == 0 ? systems_ : enemy_.systems) {
            if (system.definition.id == 11) {
                extract(system.artillery,
                        catalog_.equipment(system.definition.weapon),
                        owner,
                        index++,
                        true);
            }
        }
    }
}

void Run::initializeBeamProjectile(CombatProjectile& projectile,
                                   SDL_FPoint emission,
                                   float heading,
                                   bool oneSpace,
                                   float authoredSpeed) {
    // BeamWeapon::BeamWeapon0x4357c0: constructor draws precede
    // Projectile::Initialize's sound/animation choices. Heading is degrees.
    projectile.beamVisualInitialized = true;
    projectile.beamOrigin = emission;
    projectile.beamSubEnd = emission;
    projectile.beamHeading = heading;
    projectile.beamDrift = (combatRandom_.next() & 1U) != 0 ? .125f : -.125f;
    if (heading == 0) {
        projectile.beamSubEnd.x += 2000;
    } else {
        projectile.beamSubEnd.y -= 2000;
    }
    if (oneSpace) {
        projectile.beamSubStart = emission;
        projectile.beamHeading =
            std::atan2(projectile.target.y - emission.y, projectile.target.x - emission.x) * 180.f /
            3.141592653589793f;
    } else {
        // Projectile::RandomSidePoint0x4541f0 with BeamWeapon::BeamWeapon's
        // function-static side: zero at process start, carried through new
        // games and loads, never saved.
        const int coordinate = int(combatRandom_.next() % 800);
        switch (beamSide_) {
        case 0:
            projectile.beamSubStart = {-400.f, float(coordinate)};
            break;
        case 1:
            projectile.beamSubStart = {800.f, float(coordinate / 2)};
            break;
        case 2:
            projectile.beamSubStart = {float(coordinate), -400.f};
            break;
        default:
            projectile.beamSubStart = {float(coordinate), 800.f};
            break;
        }
        beamSide_ = (beamSide_ + 1) % 4;
    }
    projectile.position = projectile.previous = projectile.target;
    // BeamWeapon::BeamWeapon sets speed_magnitude 5; Projectile::Initialize
    // 0x452130 then replaces it with the blueprint <speed> when that is > 0
    // (ARTILLERY_FED, BEAM_LONG and BEAM_BIO author 13, BEAM_1 3).
    projectile.speed = (authoredSpeed > 0 ? authoredSpeed : 5.f) * 16.f;
    projectile.arrived = true;
}

void Run::initializeProjectileExplosion(CombatProjectile& projectile,
                                        const EquipmentDefinition& blueprint) {
    // Missile::Missile 0x44b520 resolves its default explosion_random before
    // Projectile::Initialize 0x452130 applies an optional authored override.
    // Preserve both draws when that override itself is explosion_random.
    if (blueprint.type == "MISSILES" || blueprint.type == "BOMB" || projectile.crystalShard) {
        projectile.explosionImage = "explosion_big" + std::to_string(2 + combatRandom_.next() % 3);
    }
    const auto select = [&](const auto& sounds) {
        return sounds.empty() ? std::string{} : sounds[combatRandom_.next() % sounds.size()];
    };
    // Projectile::Initialize0x452130 chooses all three collision sounds
    // before its animation overrides, including aliases never actually used.
    projectile.hitShipSound = select(blueprint.hitShipSounds);
    projectile.hitShieldSound = select(blueprint.hitShieldSounds);
    projectile.missSound = select(blueprint.missSounds);
    projectile.soundsInitialized = true;
    if (blueprint.explosionAnimation == "explosion_random") {
        projectile.explosionImage = "explosion_big" + std::to_string(2 + combatRandom_.next() % 3);
    } else if (!blueprint.explosionAnimation.empty()) {
        projectile.explosionImage = blueprint.explosionAnimation;
    }
    if (blueprint.spin > 0 && !projectile.crystalShard && blueprint.type != "MISSILES" &&
        blueprint.type != "BEAM" && blueprint.type != "BOMB") {
        // Initialize's virtual SetSpin0x44ed40 follows the death animation.
        projectile.spinAngle = float(combatRandom_.next() % 360);
    }
}

bool Run::projectileDodged(CombatProjectile& p) {
    if (p.checkedShield) {
        return p.missed;
    }
    const auto& damage = catalog_.equipment(p.weapon);
    const bool selfOrdinary = p.targetShip == p.owner && !p.crystalShard && damage.type != "BOMB";
    const int dodge = selfOrdinary ? 0 : evasion(p.targetShip);
    const bool automaticMiss = p.targetShip == 1 && (!hostile_ || enemy_.escapeJumpElapsed >= 0);
    p.missed = automaticMiss || (dodge > 0 && static_cast<int>(combatRandom_.next() % 100) < dodge);
    p.checkedShield = true;
    progressionRun_.tutorial = tutorialOwned_;
    if (p.targetShip == 0 && p.owner != 0) {
        if (p.missed) {
            progressionRun_.flags["failed_dodges"] = 0;
            if (cloak_.active) {
                progressionRun_.flag("cloaked_incoming_damage",
                                     p.fake ? 0 : std::max(0, damage.damage));
            }
        } else if (const auto* engines = system(1);
                   engines && std::clamp(engines->power, 0, engines->health) > 7 && evasion() > 0) {
            progressionRun_.flag("failed_dodges");
        }
    }
    const bool intruders = std::ranges::any_of(voyageBoarders_,
                                               [](const auto& member) {
                                                   return member.health > 0;
                                               }) ||
                           std::ranges::any_of(enemy_.crew, [](const auto& member) {
                               return member.intruder && member.health > 0;
                           });
    if (p.missed && p.targetShip == 0 && !cloak_.active && (enemy_.present || intruders)) {
        // GetDodged0x492060 emits event7 before BombProjectile decides whether
        // an own-ship repair bomb should ignore the returned dodge result.
        for (bool enemy : {false, true}) {
            const auto* pilot = crewOperator(6, enemy);
            for (auto& member : crew_) {
                if (&member != pilot) {
                    continue;
                }
                incrementNativeCounter(member.statistics[2]);
                if (member.manningSkillProgress[0] < crewSkillCap(member, 0)) {
                    incrementNativeCounter(member.statistics[2]);
                }
                break;
            }
            grantManningSkill(crew_, pilot, 0, audio_);
            grantManningSkill(crew_, crewOperator(1, enemy), 1, audio_);
        }
    }
    return p.missed;
}

void Run::impact(CombatProjectile& p,
                 bool shieldOnly,
                 const bool* beamResistance,
                 bool directRoom) {
    auto damage = catalog_.equipment(p.weapon);
    if (directRoom && p.weapon == "BOARDER_ION") {
        // IonDrone::GetRoomDamage0x4374b0 passes a Damage value directly to
        // DamageArea; it does not construct or collide a Bomb projectile.
        damage.type = "BOMB";
        damage.damage = damage.system = damage.personnel = damage.piercing = damage.stunChance =
            damage.fireChance = damage.breachChance = 0;
        damage.ion = 3;
        damage.stun = 6;
        damage.hullBuster = damage.lockdown = false;
    }
    if (directRoom && p.podBreach) {
        // BoarderPodDrone::OnLoop 0x427820 lands with DamageTarget(position,
        // Damage{breachChance 10}) straight into DamageArea.
        damage.type = "BOMB";
        damage.damage = damage.system = damage.personnel = damage.piercing = damage.stunChance =
            damage.fireChance = damage.ion = damage.stun = 0;
        damage.breachChance = 10;
        damage.hullBuster = damage.lockdown = false;
    }
    if (p.fake) {
        // Factory0x40ccd0 zeroes fake mini-projectile payloads after Initialize.
        // Their ordinary collision callbacks, draws and quarter-scale FX remain.
        damage.damage = damage.piercing = damage.fireChance = damage.breachChance =
            damage.stunChance = damage.ion = damage.system = damage.personnel = damage.stun = 0;
        damage.hullBuster = damage.lockdown = false;
    }
    const auto collisionSound = [&](const std::string& selected, const auto& group) {
        if (directRoom) {
            return;
        }
        // A legacy checkpoint cannot recover the creation-time selection.
        // Its stable first-alias fallback consumes no fabricated RNG draws.
        const std::string alias = p.soundsInitialized ? selected
                                  : group.empty()     ? std::string{}
                                                      : group.front();
        if (!alias.empty()) {
            combatSound({alias}, p.id);
        }
    };
    auto& systems = p.targetShip == 1 ? enemy_.systems : systems_;
    auto& crew = p.targetShip == 1 ? enemy_.crew : crew_;
    const auto& targetShipDef = p.targetShip == 1 ? enemy_.ship : ship_;
    int& hull = p.targetShip == 1 ? enemy_.hull : hull_;
    int& shields = p.targetShip == 1 ? enemy_.shields : shieldBubbles_;
    int& superShields = p.targetShip == 1 ? enemy_.superShields : superShields_;
    if (!directRoom) {
        projectileDodged(p);
    }
    p.resolved = true;
    if (p.missed) {
        const bool targetJumping =
            p.targetShip == 1 ? enemy_.escapeJumpElapsed >= 0 : jumpTicks_ > 0 || arrivalTicks_ > 0;
        if (damage.type != "BOMB" && (shieldOnly || !targetJumping)) {
            pushCombatMessage(DamageMessage{
                p.targetShip, 0, shieldOnly ? p.position : p.target, tick_, false, false, true});
        }
        // CollisionCheck0x4505a0 marks a miss without starting the death animation;
        // Projectile::OnUpdate0x44eed0 keeps that projectile moving past its target.
        const float dx = p.target.x - p.previous.x;
        const float dy = p.target.y - p.previous.y;
        const float distance = std::hypot(dx, dy);
        if (distance > 0) {
            p.direction = {dx / distance, dy / distance};
        }
        collisionSound(p.missSound, damage.missSounds);
        return;
    }
    if (p.room < 0) {
        p.missed = true;
        collisionSound(p.missSound, damage.missSounds);
        return;
    }
    if (!directRoom && (damage.type == "BOMB" ? p.bombSuperShield : superShields > 0) &&
        p.weapon != "PDS_SHOT") {
        // Shields::CollisionReal 0x60d980: a super shield takes hull damage
        // plus twice the ion payload before normal bubbles are considered.
        // BombProjectile::CollisionCheck 0x44a5c0 also adds its system payload.
        if (!touchShieldHit(p.targetShip, p.position, p.id, shields)) {
            p.intercepted = true;
            return;
        }
        const int ionPayload = ionAfterResistance(
            p.targetShip,
            damage.ion + (damage.damage <= 0 && damage.ion > 0 ? p.damageBonus : 0),
            p.position,
            true);
        const int hullPayload = damage.damage + (damage.damage > 0 ? p.damageBonus : 0);
        const int bombPayload = damage.type == "BOMB" ? damage.system : 0;
        const int absorbed = std::max(0, hullPayload + bombPayload + 2 * ionPayload);
        // CollisionReal adds the arc even when a zero payload leaves every
        // super layer intact; normal bubbles still determine interception.
        if (absorbed > 0) {
            superShields = std::max(0, superShields - absorbed);
            // Shields::CollisionReal 0x60d980 reports the absorbed amount as a green
            // floating number. It constructs the message from the raw absorbed value
            // (`first_8`), not the amount clamped to the remaining super layers that
            // it stores in the collision response's superDamage field.
            pushCombatMessage(DamageMessage{
                p.targetShip, absorbed, p.position, tick_, true, false, false, 0, true});
            p.intercepted = true;
            if (p.weapon == "ENV_ASTEROID") {
                combatSound({p.asteroidShieldSound}, p.id);
            } else {
                collisionSound(damage.type == "BOMB" ? p.hitShipSound : p.hitShieldSound,
                               damage.type == "BOMB" ? damage.hitShipSounds
                                                     : damage.hitShieldSounds);
            }
            // CollisionReal 0x60d980 plays shieldsDown after every new shield
            // hit that leaves zero normal bubbles, super-layer hits included.
            if (shields == 0) {
                combatSound({"shieldsDown"}, p.id);
            }
            return;
        }
        if (shields > damage.piercing && damage.type != "BOMB" && damage.type != "BEAM") {
            p.intercepted = true;
            collisionSound(p.hitShieldSound, damage.hitShieldSounds);
            return;
        }
        if (damage.type == "BOMB") {
            // Bomb's teleport-time barrier decision persists through the fuse;
            // a zero payload still starts its death animation without room damage.
            p.intercepted = true;
            collisionSound(p.hitShipSound, damage.hitShipSounds);
            return;
        }
    }
    if (!directRoom && p.weapon != "PDS_SHOT" && damage.type != "BOMB" && damage.type != "BEAM" &&
        shields > damage.piercing) {
        const int previousShields = shields;
        if (!touchShieldHit(p.targetShip, p.position, p.id, previousShields)) {
            p.intercepted = true;
            return;
        }
        if (damage.damage > 0) {
            --shields;
        }
        if (p.targetShip == 0 && damage.damage > 0 && previousShields > 0 && enemy_.present) {
            // ShipManager::CollisionShield emits event 8 only for a
            // damaging shot against the player's normal shield.
            grantManningSkill(crew_, crewOperator(0, false), 2, audio_);
            grantManningSkill(crew_, crewOperator(0, true), 2, audio_);
        }
        // ShipManager::CollisionShield 0x499990 ionizes the shield system
        // after a normal-bubble collision, even though the shot never reaches
        // its aimed room. Super-shield collisions take a separate path.
        ionizeShieldCollision(p.targetShip,
                              damage.ion +
                                  (damage.damage <= 0 && damage.ion > 0 ? p.damageBonus : 0),
                              damage.stun,
                              damage.stunChance);
        p.intercepted = true;
        if (p.weapon == "ENV_ASTEROID") {
            combatSound({p.asteroidShieldSound}, p.id);
        } else {
            collisionSound(p.hitShieldSound, damage.hitShieldSounds);
        }
        if (previousShields > 0 && shields == 0) {
            combatSound({"shieldsDown"}, p.id);
        }
        return;
    }
    if (shieldOnly) {
        if (shields > 0) {
            touchShieldHit(p.targetShip, p.position, p.id, shields);
        }
        p.resolved = false;
        return;
    }
    const bool emptyRoom = std::none_of(systems.begin(), systems.end(), [&](const auto& system) {
        return system.definition.room == p.room;
    });
    // ShipManager::DamageArea 0x499fb0 doubles hull damage for a hull-buster
    // only when the struck room has no system; system and crew damage stay normal.
    const int baseDamage =
        p.crystalShard ? 1 : damage.damage + (damage.damage > 0 ? p.damageBonus : 0);
    const int systemBonus =
        damage.damage <= 0 && damage.ion <= 0 && damage.personnel <= 0 ? p.damageBonus : 0;
    const int personnelBonus =
        damage.damage <= 0 && damage.ion <= 0 && damage.personnel > 0 ? p.damageBonus : 0;
    const bool beamPayload = damage.type == "BEAM";
    // DamageArea calls DamageCrew at 0x49a14c before hull/hazard resistance.
    std::vector<PhysicalDamageActor> affectedCrew;
    for (auto& member : crew) {
        if (!member.intruder && member.room == p.room) {
            affectedCrew.push_back({&member, nullptr, p.targetShip, member.physicalCrewOrder});
        }
    }
    auto& visitors = p.targetShip == 1 ? crew_ : enemy_.crew;
    for (auto& member : visitors) {
        if (member.intruder && member.boardRoom == p.room) {
            affectedCrew.push_back({&member, nullptr, 1 - p.targetShip, member.physicalCrewOrder});
        }
    }
    if (p.targetShip == 0) {
        for (auto& member : voyageBoarders_) {
            if (member.room == p.room) {
                affectedCrew.push_back({&member, nullptr, 1, member.physicalCrewOrder});
            }
        }
    }
    for (const int owner : {0, 1}) {
        for (auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == p.targetShip && drone->crewRoom == p.room) {
                affectedCrew.push_back({nullptr, drone, owner, drone->physicalCrewOrder});
            }
        }
    }
    std::ranges::stable_sort(affectedCrew, {}, &PhysicalDamageActor::order);
    for (const auto& actor : affectedCrew) {
        const int owner = actor.owner;
        if (directRoom && p.weapon == "BOARDER_ION" && owner == p.owner) {
            // DamageCrew0x4925a0 uses creation iShipId, before any stun/RNG;
            // mind control changes allegiance but does not change that owner.
            continue;
        }
        if (actor.drone) {
            if (!beamPayload) {
                damageCrewDrones(
                    p.targetShip,
                    p.room,
                    15.f * float(std::max(0, baseDamage) + damage.personnel + personnelBonus),
                    nullptr,
                    float(damage.stun),
                    damage.stunChance,
                    -1,
                    actor.drone);
            }
            continue;
        }
        auto& c = *actor.crew;
        if (!beamPayload && !c.outOfGame) {
            // ShipManager::DamageCrew 0x4925a0 first applies guaranteed stun,
            // then gives AE chance payloads a minimum three-second duration.
            c.stunTime = std::max(c.stunTime, float(damage.stun));
            // DamageCrew 0x492604 draws before testing chance and AE, including
            // zero-chance payloads and a disabled DLC setting.
            const int stunRoll = int(combatRandom_.next() % 10);
            if (damage.stunChance > stunRoll && advanced_) {
                c.stunTime = std::max(c.stunTime, 3.f);
            }
            if (c.health <= 0) {
                continue; // Physical dying body still drew stun; health refuses edits.
            }
            const int crewPayload = std::max(0, baseDamage) + damage.personnel + personnelBonus;
            if (crewPayload < 0 && owner != p.owner) {
                // DamageCrew 0x492648 rejects hostile healing only after stun.
                continue;
            }
            // ShipManager::DamageCrew0x4925a0, normal human impact damage.
            const float previousHealth = c.health;
            c.health = std::clamp(c.health - 15.f * crewPayload,
                                  0.f,
                                  crewMaximumHealth(c.appearance.species) + c.healthBoost);
            noteCrewHealthChange(c, previousHealth);
            if (c.health == 0) {
                crewDeath(c);
            }
        }
    }
    // ShipManager::DamageArea 0x499fb0 rolls ShipManager::ResistDamage 0x491940 with the
    // ROCK_ARMOR augmentation id before its fire and breach rolls. A resisted hit still
    // consumes both hazard rolls, but it is not struck on the hull at all and cannot
    // breach. See docs/research/P06-damage-resistance-source.md.
    const bool rockArmor =
        beamResistance ? *beamResistance : resistAugmentation(p.targetShip, "ROCK_ARMOR");
    bool hullResisted = rockArmor && baseDamage > 0;
    // DamageArea keeps the ROCK_ARMOR result itself (cleared by an admitted
    // breach) and strikes the hull only when it failed or the payload heals.
    bool rockResisted = rockArmor;
    if (p.weapon == "ENV_ASTEROID") {
        combatSound({p.asteroidHullSound}, p.id);
    } else if (!beamPayload) {
        collisionSound(p.hitShipSound, damage.hitShipSounds);
    }
    if (damage.lockdown && p.room >= 0) {
        // ShipManager::DamageArea 0x49a4ac calls Ship::LockdownRoom;
        // Door::SetLockdown 0x465af0 seals every adjacent door for 12 seconds.
        auto& locked = p.targetShip == 1 ? enemy_.lockdownRooms : lockdownRooms_;
        locked[p.room] = 12.f;
        lockdownDoors(p.room, p.targetShip == 1);
        if (p.targetShip == 0) {
            for (const auto& door : ship_.doors) {
                if (door.roomA == p.room || door.roomB == p.room) {
                    doors_[door.id] = false;
                    fakeDoors_[door.id] = false;
                }
            }
        }
    }
    // ShipManager::DamageArea 0x49a1c1/0x49a765 attempts two fire tiles
    // first; its breach chance is evaluated only when no fire starts.
    // ShipManager::DamageBeam 0x49e4d0 does neither: it rolls one fire per grid
    // tile the beam head enters, rolls the breach unconditionally (even when a
    // fire started) and strikes only the crew standing on that tile. The beam's
    // hazard and crew payload therefore lives in beamTileImpact() and this block
    // stays with DamageArea.
    bool startedFire = false;
    if (!beamPayload) {
        // ShipManager::DamageArea 0x499fb0 always consumes the fire roll on a
        // valid room hit, including a zero-chance payload.
        const int fireRoll = int(combatRandom_.next() % 10);
        if (damage.fireChance > fireRoll) {
            startRoomFire(p.targetShip, p.room, 2);
            startedFire = true;
        }
        if (!startedFire) {
            // The original also draws for breach at zero chance whenever the
            // fire roll fails, before applying room/system damage.
            const int breachRoll = int(combatRandom_.next() % 10);
            if (damage.breachChance > breachRoll) {
                const auto& room = findRoom(targetShipDef, p.room);
                const int tiles = room.w / 35 * (room.h / 35);
                // Ship::BreachRandomHull 0x483390 selects one OuterHull tile in
                // this room; striking an already breached tile adds no breach.
                // A resisted hit still consumes this roll but never reaches the call.
                if (tiles > 0 && (!rockArmor || damage.breachChance == 10)) {
                    // DamageArea 0x49a24e clears the shared resistance after
                    // an admitted breach, including guaranteed breach payloads.
                    hullResisted = false;
                    rockResisted = false;
                    breachRandomHull(p.targetShip, p.room);
                }
            }
        }
    }
    // ShipManager::DamageArea 0x499fb0 rolls ShipManager::ResistDamage 0x491940 with the
    // SYSTEM_CASING augmentation id when the struck room holds a system that is not
    // completely destroyed, and on success zeroes both the base damage component and the
    // system payload of the copy it passes to DamageSystem. The draw is consumed either
    // way, because the original's no-system arm rolls it and discards the result.
    const bool casingRoll = resistAugmentation(p.targetShip, "SYSTEM_CASING");
    const auto roomSystem = std::find_if(systems.begin(), systems.end(), [&](const auto& s) {
        return s.definition.room == p.room;
    });
    const bool systemCasing = casingRoll && roomSystem != systems.end() && roomSystem->health > 0 &&
                              (baseDamage > 0 || damage.system + systemBonus > 0);
    const int systemPayload =
        systemCasing ? 0 : std::max(0, baseDamage) + damage.system + systemBonus;
    // DamageArea calls DamageSystem at 0x49a48e after SYSTEM_CASING. That
    // function rolls ION_ARMOR at 0x49929b before applying the system payload.
    // DamageSystem places its Ion Armor RESIST and both payload numbers at
    // Ship::GetRoomCenter of the struck room.
    const SDL_FPoint struckCenter = center(findRoom(targetShipDef, p.room));
    const int ionDamage =
        ionAfterResistance(p.targetShip,
                           damage.ion + (damage.damage <= 0 && damage.ion > 0 ? p.damageBonus : 0),
                           struckCenter);
    const int systemNumber = systemCasing ? 0 : damage.system + systemBonus;
    for (auto& s : systems) {
        if (s.definition.room == p.room) {
            // ShipSystem::AddDamage 0x614e50 clamps negative system damage
            // (the repair bomb) to the installed system's maximum health.
            // The virtual AddDamage leaves base reactor/battery bars for the next
            // CheckMaxPower; armaments shed slots and shields an odd bar now.
            addSystemDamage(s, systemPayload, p.targetShip == 1);
            if (ionDamage > 0) {
                applySystemIon(s, ionDamage, p.targetShip == 1);
            }
            // DamageSystem 0x4991d0: a blue ion number and a light-red system
            // number for each payload that reaches the room's system.
            if (ionDamage > 0) {
                pushCombatMessage(
                    DamageMessage{p.targetShip, ionDamage, struckCenter, tick_, false, true});
            }
            if (systemNumber > 0) {
                DamageMessage message{p.targetShip, systemNumber, struckCenter, tick_};
                message.systemDamage = true;
                pushCombatMessage(message);
            }
        }
    }
    const int hullDamage = hullResisted ? 0 : baseDamage * (damage.hullBuster && emptyRoom ? 2 : 1);
    const int previousHull = hull;
    hull = std::clamp(hull - hullDamage, 0, targetShipDef.hull);
    progression_.crystalShardDamage(
        p.targetShip == 1, previousHull > 0, hull == 0, p.crystalShard, progressionRun_);
    if (hullDamage > 0) {
        crystalRetaliation(p.targetShip, p.position);
    }
    progressionRun_.tutorial = tutorialOwned_;
    if (p.targetShip == 1 && hullDamage > 0 && !cloak_.active) {
        progressionRun_.flag("combat_enemy_damaged_uncloaked");
    }
    // ShipManager::iLastDamage: DamageBeam always records the room entry
    // (0 when resisted); DamageArea records only a struck hull, so a resisted
    // zero-hull payload repeats the previous number (0x49a58f).
    auto& lastHull = lastHullDamage_[std::size_t(p.targetShip)];
    if (beamPayload) {
        lastHull = hullResisted ? 0 : hullDamage;
    } else if (!rockResisted || baseDamage < 0) {
        lastHull = hullDamage;
    }
    if (hullResisted || systemCasing) {
        // DamageArea 0x49a532 and DamageBeam 0x49e824 choose RESIST
        // instead of the ordinary hull number when either positive payload resists.
        pushCombatMessage(ResistMessage{p.targetShip, p.position, tick_, false});
    } else if (lastHull > 0) {
        // DamageArea places the number at the projectile's target point;
        // DamageBeam 0x49ecd6 at `current`, the beam head entering this room.
        pushCombatMessage(
            DamageMessage{p.targetShip, lastHull, beamPayload ? p.position : p.target, tick_});
    }
}

void Run::beamTileImpact(CombatProjectile& p, bool rockArmor) {
    const auto& damage = catalog_.equipment(p.weapon);
    const auto& destination = p.targetShip == 1 ? enemy_.ship : ship_;
    auto& hazards = p.targetShip == 1 ? enemyHazards_ : playerHazards_;
    auto& crew = p.targetShip == 1 ? enemy_.crew : crew_;
    // ShipGraph::TranslateToGrid 0x45b150 is Point((int)x / 35, (int)y / 35), so
    // the beam's position quantises to a 35-pixel tile of the *absolute* ship
    // space. ShipManager::DamageBeam 0x49e4d0 indexes the fire spreader and
    // Ship::BreachSpecificHull with those absolute grid coordinates.
    const int gridX = int(p.position.x) / 35;
    const int gridY = int(p.position.y) / 35;
    const SDL_FPoint tileCentre{float(gridX * 35 + 17), float(gridY * 35 + 17)};
    int roomId = -1;
    for (const auto& candidate : destination.rooms) {
        if (tileCentre.x >= candidate.x && tileCentre.x < candidate.x + candidate.w &&
            tileCentre.y >= candidate.y && tileCentre.y < candidate.y + candidate.h) {
            roomId = candidate.id;
            break;
        }
    }
    // 0x49e4d0 returns before any roll when ShipGraph::Valid(p) is false or the
    // head is outside every room.
    if (roomId < 0) {
        return;
    }
    const auto& room = findRoom(destination, roomId);
    const int columns = room.w / 35;
    const int rows = room.h / 35;
    const int tiles = columns * rows;
    auto& hazard = hazards[roomId];
    const int column = (gridX * 35 - room.x) / 35;
    const int row = (gridY * 35 - room.y) / 35;
    const int tileIndex = row * columns + column;
    // 0x49e4d0 rolls fire first, and Fire::Spread 0x732be0 is a thunk to
    // Repairable::SetDamage 0x466e90 with100.0f: the entered tile is ignited on
    // its own, not through ShipManager::DamageArea 0x499fb0's two-tile attempt.
    const int fireRoll = int(combatRandom_.next() % 10);
    if (damage.fireChance > fireRoll && tiles > 0 && column >= 0 && column < columns && row >= 0 &&
        row < rows) {
        hazard.syncFireTiles();
        if (std::ranges::find(hazard.fireTiles, tileIndex) == hazard.fireTiles.end()) {
            hazard.fireTiles.push_back(tileIndex);
            hazard.fireHealth.push_back(100);
            hazard.fireDeathTimers.push_back(-1);
            hazard.fires = int(hazard.fireTiles.size());
        } else {
            const auto found = std::ranges::find(hazard.fireTiles, tileIndex);
            hazard.fireHealth[static_cast<std::size_t>(found - hazard.fireTiles.begin())] = 100;
        }
        std::erase_if(hazard.fireSmoke, [tileIndex](const FireSmoke& smoke) {
            return smoke.tile == tileIndex;
        });
    }
    // The breach roll follows even when the fire roll succeeded, unlike
    // DamageArea. ROCK_ARMOR resistance suppresses it only below a guaranteed
    // chance: 0x49e4d0 tests `resist <= (breachChance == 10)`. This is the
    // same resistance result as this invocation's room hull payload.
    const int breachRoll = int(combatRandom_.next() % 10);
    if (damage.breachChance > breachRoll && (!rockArmor || damage.breachChance == 10)) {
        // Ship::BreachSpecificHull 0x483120 opens the breach on the tile the
        // beam head stands on rather than on a random tile of the room, so the
        // native tile index is that tile's index inside the room.
        if (tiles > 0 && column >= 0 && column < columns && row >= 0 && row < rows) {
            breachHullTile(p.targetShip, roomId, tileIndex, true);
        }
    }
    // 0x49e4d0 hands DamageCrew0x4925a0 to each actor on this tile. A killed
    // actor leaves the inner scan to update killedByBeam, then LABEL39 advances
    // to the next actor; it does not stop this tile's full damage pass.
    std::vector<PhysicalDamageActor> affectedCrew;
    for (auto& member : crew) {
        if (!member.intruder) {
            affectedCrew.push_back({&member, nullptr, p.targetShip, member.physicalCrewOrder});
        }
    }
    auto& visitors = p.targetShip == 1 ? crew_ : enemy_.crew;
    for (auto& member : visitors) {
        if (member.intruder) {
            affectedCrew.push_back({&member, nullptr, 1 - p.targetShip, member.physicalCrewOrder});
        }
    }
    if (p.targetShip == 0) {
        for (auto& member : voyageBoarders_) {
            affectedCrew.push_back({&member, nullptr, 1, member.physicalCrewOrder});
        }
    }
    for (const int owner : {0, 1}) {
        for (auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == p.targetShip && drone->crewRoom == roomId) {
                affectedCrew.push_back({nullptr, drone, owner, drone->physicalCrewOrder});
            }
        }
    }
    std::ranges::stable_sort(affectedCrew, {}, &PhysicalDamageActor::order);
    for (const auto& actor : affectedCrew) {
        if (actor.drone) {
            const int killed = damageCrewDrones(
                p.targetShip,
                roomId,
                15.f * float(std::max(0, damage.damage + p.damageBonus) + damage.personnel),
                &tileCentre,
                float(damage.stun),
                damage.stunChance,
                -1,
                actor.drone);
            if (killed > 0) {
                beamKilledCrew_[std::size_t(p.targetShip)][int(p.id)] += killed;
                progressionRun_.tutorial = tutorialOwned_;
                progression_.condition("ACH_SLUG_BIO",
                                       p.targetShip == 1 && damage.damage == 0 &&
                                           damage.personnel > 0 &&
                                           beamKilledCrew_[1][int(p.id)] > 2,
                                       progressionRun_);
            }
            continue;
        }
        auto& c = *actor.crew;

        const auto physical = crewCombatPoint(c);
        if (c.outOfGame || int(physical.x) / 35 != gridX || int(physical.y) / 35 != gridY) {
            continue;
        }
        c.stunTime = std::max(c.stunTime, float(damage.stun));
        // DamageBeam forwards the complete payload to DamageCrew 0x4925a0,
        // including its unconditional chance draw and nonnegative hull component.
        const int stunRoll = int(combatRandom_.next() % 10);
        if (damage.stunChance > stunRoll && advanced_) {
            c.stunTime = std::max(c.stunTime, 3.f);
        }
        if (c.health <= 0) {
            continue;
        }
        const int crewPayload = std::max(0, damage.damage + p.damageBonus) + damage.personnel;
        const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& member) {
            return &member == &c;
        });
        if (crewPayload < 0 && p.owner != (playerOwned ? 0 : 1)) {
            continue;
        }
        const float previousHealth = c.health;
        c.health = std::clamp(c.health - 15.f * float(crewPayload),
                              0.f,
                              crewMaximumHealth(c.appearance.species) + c.healthBoost);
        noteCrewHealthChange(c, previousHealth);
        if (c.health == 0) {
            crewDeath(c);
            // 0x49e4d0 raises ACH_SLUG_BIO once one beam has killed three crew
            // with no hull damage and a positive personnel payload; native models
            // the counter and leaves the achievement tracker to the hangar.
            ++beamKilledCrew_[std::size_t(p.targetShip)][int(p.id)];
            progressionRun_.tutorial = tutorialOwned_;
            progression_.condition("ACH_SLUG_BIO",
                                   p.targetShip == 1 && damage.damage == 0 &&
                                       damage.personnel > 0 && beamKilledCrew_[1][int(p.id)] > 2,
                                   progressionRun_);
        }
    }
}

EventResources Run::surrenderReward() {
    enemy_.rewardVariant = -1;
    enemy_.rewardUnsupportedCrew = false;
    // PIRATE_SURRENDER's accepted child requests RANDOM/stuff.
    return generateCombatReward("stuff", 3);
}

EventResources Run::generateCombatReward(std::string_view type, int tier) {
    // GenerateReward 0x5bbae0 / GetValue 0x5bb820. The native event stream
    // reproduces the source draw structure for represented currency branches;
    // it is not the original process-global RNG stream.
    constexpr std::array<std::array<int, 2>, 3> fuelValues{{{1, 3}, {2, 4}, {3, 6}}};
    constexpr std::array<std::array<int, 2>, 3> missileValues{{{1, 2}, {2, 4}, {4, 8}}};
    constexpr std::array<std::array<int, 2>, 3> droneValues{{{1, 1}, {1, 1}, {1, 2}}};
    constexpr std::array<std::array<int, 2>, 3> scrapThousandths{
        {{500, 700}, {800, 1300}, {1300, 1550}}};

    const auto drawInclusive = [this](int minimum, int maximum) {
        const auto width = static_cast<std::uint32_t>(maximum - minimum + 1);
        return minimum + static_cast<int>(eventRandom_.next() % width);
    };
    const int rewardLevel = tier == 3 ? static_cast<int>(eventRandom_.next() % 3) : tier;
    if (rewardLevel < 0 || rewardLevel > 2 ||
        (type != "standard" && type != "stuff" && type != "fuel" && type != "scrap_only" &&
         type != "weapon" && type != "drone" && type != "augment" && type != "fuel_only" &&
         type != "missiles" && type != "missiles_only" && type != "droneparts" &&
         type != "droneparts_only")) {
        throw std::runtime_error("unsupported_combat_reward");
    }
    enemy_.rewardTier = rewardLevel;
    enemy_.rewardType = type;
    enemy_.rewardUnsupportedItem = false;
    enemy_.rewardItem.clear();
    enemy_.rewardCommitted = false;
    const auto selectItem = [&](std::string_view kind) {
        // RandomItems<T> 0x72ec50: sorted map becomes a one-based weighted
        // heap. Its traversal order determines the seed-to-blueprint mapping.
        const auto pool = catalog_.rewardEquipmentPool(kind, advanced_, sectorRarities());
        if (pool.empty()) {
            throw std::runtime_error("empty_combat_reward_pool");
        }
        std::vector<int> totals(pool.size() + 1, 0);
        for (std::size_t i = 1; i <= pool.size(); ++i) {
            if (pool[i - 1].second <= 0) {
                throw std::runtime_error("invalid_combat_reward_weight");
            }
            totals[i] = pool[i - 1].second;
        }
        for (std::size_t i = pool.size(); i > 1; --i) {
            totals[i / 2] += totals[i];
        }
        int choice =
            1 + static_cast<int>(eventRandom_.next() % static_cast<std::uint32_t>(totals[1]));
        std::size_t index = 1;
        while (choice > pool[index - 1].second) {
            choice -= pool[index - 1].second;
            index *= 2;
            if (choice > totals[index]) {
                choice -= totals[index];
                ++index;
            }
        }
        enemy_.rewardItem = pool[index - 1].first;
    };
    // A named weapon outcome selects the item before its scrap draw.
    if (type == "weapon" || type == "drone" || type == "augment") {
        selectItem(type == "augment" ? "aug" : type);
    }
    const int worldLevel = std::max(0, sector_ - 1);
    const int rewardScale =
        std::max(0, worldLevel + (difficulty_ == 0 ? 1 : 0) - (difficulty_ == 2 ? 1 : 0));

    EventResources reward;
    // STANDARD uses the requested scrap tier but LOW resource quantities.
    // STUFF uses LOW scrap but requested resource quantities. Single-resource
    // and scrap-only outcomes use their requested tier (0x5bca20/0x5bcfdd).
    const int scrapTier = type == "stuff" ? 0 : rewardLevel;
    const int scrapRate =
        type.ends_with("_only") && type != "scrap_only"
            ? 0
            : drawInclusive(scrapThousandths[scrapTier][0], scrapThousandths[scrapTier][1]);
    reward.scrap = static_cast<int>((static_cast<float>(scrapRate) / 1000.f) *
                                    static_cast<float>(6 * rewardScale + 15));

    const int resourceTier = type == "standard" ? 0 : rewardLevel;
    const auto giveResource = [&](std::size_t kind) {
        if (kind == 0) {
            reward.fuel += drawInclusive(fuelValues[resourceTier][0], fuelValues[resourceTier][1]);
        } else if (kind == 1) {
            reward.missiles +=
                drawInclusive(missileValues[resourceTier][0], missileValues[resourceTier][1]);
        } else {
            reward.drones +=
                drawInclusive(droneValues[resourceTier][0], droneValues[resourceTier][1]);
        }
    };
    if (type == "standard" || type == "stuff") {
        // GetResource 0x5bba20 selects the two kinds without replacement before
        // their quantity draws. Even a one-element amount range consumes a draw.
        const auto firstResource = static_cast<std::size_t>(eventRandom_.next() % 3);
        std::size_t secondResource = static_cast<std::size_t>(eventRandom_.next() % 3);
        while (secondResource == firstResource) {
            secondResource = static_cast<std::size_t>(eventRandom_.next() % 3);
        }
        giveResource(firstResource);
        giveResource(secondResource);
        // STANDARD's inclusive <=2 gate is 3%; STUFF's <=5 gate is 6%.
        if (eventRandom_.next() % 100 <= (type == "standard" ? 2U : 5U)) {
            // itemStrings startup0x82311d: weapon, drone, augment.
            constexpr std::array<std::string_view, 3> kinds{"weapon", "drone", "aug"};
            selectItem(kinds[eventRandom_.next() % kinds.size()]);
            // GenerateReward's item branch calls GetValue(scrap) again, replacing
            // the earlier STANDARD/STUFF scrap at the requested tier.
            const int rate =
                drawInclusive(scrapThousandths[rewardLevel][0], scrapThousandths[rewardLevel][1]);
            reward.scrap = static_cast<int>((static_cast<float>(rate) / 1000.f) *
                                            static_cast<float>(6 * rewardScale + 15));
        }
    } else if (type == "fuel" || type == "fuel_only") {
        giveResource(0);
    } else if (type == "missiles" || type == "missiles_only") {
        giveResource(1);
    } else if (type == "droneparts" || type == "droneparts_only") {
        giveResource(2);
    }
    return reward;
}

void Run::stopCombatHostilities() {
    hostile_ = false;
    aiming_ = -1;
    // ShipAI::OnLoop 0x6012e0 enters its nonhostile branch and calls
    // CombatAI::DeactiveWeapons 0x605f40, then drains super shields.
    for (auto& system : enemy_.systems) {
        if (system.definition.id == 3 || system.definition.id == 4 || system.definition.id == 11) {
            forceDecreaseSystemPower(system, system.power + 1, true);
        }
    }
    enemy_.superShields = 0;
    stopAbility(10, true);
    stopAbility(12, true);
    cancelAim();
    for (auto& w : combatWeapons_) {
        w.clearTargets();
        // WeaponControl clears the target at fight end. SetCurrentShip
        // 0x40be30 drops chain progress when its target becomes null.
        w.boostLevel = 0;
    }
    for (auto& system : systems_) {
        if (system.definition.id == 11) {
            system.artillery.clearTargets();
        }
    }
    projectiles_.clear();
}

void Run::combatResult(bool surrender) {
    // Read before stopCombatHostilities: WorldManager::OnLoop 0x5f6520 tests the
    // ship's hostile flag at LABEL_45 after building the outcome event.
    const bool wasHostile = hostile_;
    if (campaign_.fighting) {
        if (!surrender) {
            finishBossStage();
        }
        return;
    }
    progressionRun_.tutorial = tutorialOwned_;
    if (!surrender && !tutorialOwned_) {
        ProgressionCombatResult result;
        result.encounterSerial = progressionRun_.value("encounter_serial");
        result.destroyed = enemy_.hull == 0;
        result.crewKilled = !result.destroyed && !enemy_.crew.empty() &&
                            std::ranges::none_of(enemy_.crew, [](const auto& member) {
                                return (!member.dead && !member.dying) || member.cloneReady;
                            });
        result.sector = sector_ - 1;
        result.playerWeaponShots = progressionRun_.value("combat_player_weapon_shots");
        result.maximumWeaponVolleys = progressionRun_.value("combat_max_volley");
        result.playerArtilleryShots = progressionRun_.value("combat_player_artillery_shots");
        result.enemyWeaponShots = progressionRun_.value("combat_enemy_weapon_shots");
        result.offensiveDroneActivated = progressionRun_.value("combat_offensive_drone") != 0;
        result.nonMissileWeaponFired = progressionRun_.value("combat_non_missile") != 0;
        result.preIgniter = std::ranges::find(ship_.augments, std::string{"WEAPON_PREIGNITE"}) !=
                            ship_.augments.end();
        result.enemyDamagedUncloaked =
            progressionRun_.value("combat_enemy_damaged_uncloaked") != 0 ||
            (!cloak_.active && enemy_.hull < enemy_.ship.hull);
        result.playerSuperShields = superShields_;
        for (const auto& drone : enemy_.drones) {
            const auto& equipment = catalog_.equipment(drone.blueprint);
            result.enemyDefenseDrone |=
                equipment.type == "DEFENSE" && equipment.droneTarget != "DRONES";
        }
        result.rockShip = enemy_.ship.id.starts_with("ROCK_SCOUT") ||
                          enemy_.ship.id.starts_with("ROCK_ASSAULT") ||
                          enemy_.ship.id.starts_with("ROCK_FIGHT");
        for (const auto& member : crew_) {
            if (member.health > 0) {
                ++(member.boardRoom >= 0 ? result.playerCrewOnEnemyShip
                                         : result.playerCrewOnPlayerShip);
            }
        }
        result.playerHullUnchanged = hull_ == progressionRun_.value("combat_start_hull");
        result.playerCrewCountUnchanged =
            result.playerCrewOnPlayerShip + result.playerCrewOnEnemyShip ==
            progressionRun_.value("combat_start_crew");
        result.enemyCrewKilledThisFight = progressionRun_.value("combat_enemy_crew_killed");
        progression_.combat(result, progressionRun_);
        progression_.condition("ACH_INVADE_SHIP",
                               (result.destroyed || result.crewKilled) &&
                                   result.playerCrewOnPlayerShip <= 0,
                               progressionRun_);
    }
    if (!surrender) {
        addScore(scoreShips_, 1);
        // WorldManager::OnLoop 0x5f6520 calls ShipManager::SetTarget(nullptr)
        // for the destroyed and deadCrew outcomes; a surrender keeps it.
        // SetTarget(nullptr) clears every player drone's bDisrupted.
        for (auto& drone : combatDrones_) {
            drone.defenseDisrupted = false;
        }
        enemy_.playerTarget = false;
        if (enemy_.hull == 0) {
            enemy_.present = false;
        }
    }
    stopCombatHostilities();
    enemy_.rewardUnsupportedCrew = false;
    if (!surrender && (tutorialOwned_ || !enemy_.pinnedRewardFixture)) {
        // WorldManager::OnLoop 0x5f6520 dispatches the encounter's actual
        // destroyed/deadCrew event. Its authored chain may contain quests,
        // crew/equipment rewards or ship unlocks in addition to currency.
        reseedShipOutcome();
        const auto definition = events_.shipEvent(encounter_);
        const auto outcome = definition.child(enemy_.hull > 0 ? "deadCrew" : "destroyed");
        if (outcome) {
            selectedVoyageCrewSeed_ = -1;
            const auto prepared = createShipOutcomeEvent(outcome);
            if (!wasHostile) {
                // A ship that is no longer hostile (a surrender accepted) still
                // counts as defeated (AddDefeatedShips at LABEL_155) but LABEL_45
                // skips UpdateLocation: the destroyed event is generated and
                // dropped, so there is no reward.
                surrenderDialog_ = false;
                return;
            }
            enterEvent(prepared);
            if (tutorialOwned_) {
                eventId_ = "P06_VICTORY_REWARD";
            }
            openingEvent_ = false;
            surrenderDialog_ = false;
            return;
        }
        if (!tutorialOwned_) {
            // GetBaseEvent 0x5bb240 returns null for an absent outcome name and
            // OnLoop then skips UpdateLocation (its !BaseEvent test before
            // LABEL_27): no event and no reward. Only the tutorial substitutes
            // TutorialManager::GetEvent there.
            surrenderDialog_ = false;
            return;
        }
    }
    if (surrender) {
        pendingResources_ = surrenderReward();
        eventText_ = texts_.get("text_PIRATE_SURRENDER_TEXT_7", locale_);
    } else if (enemy_.pinnedRewardFixture) {
        // Only an explicit diagnostic fixture reproduces the historical pixels.
        pendingResources_ = {14, 1, 2, 0, 0};
        enemy_.rewardVariant = enemy_.rewardTier = -1;
        enemy_.rewardType = "reference";
        enemy_.rewardUnsupportedItem = false;
        eventText_ = texts_.get(enemy_.hull > 0 ? "event_DEAD_CREW_DEFAULT_1_text"
                                                : "event_DESTROYED_DEFAULT_1_text",
                                locale_);
    } else {
        const std::string family = enemy_.hull > 0 ? "DEAD_CREW_DEFAULT" : "DESTROYED_DEFAULT";
        const auto list = events_.event(family);
        const auto outcome = events_.resolve(list, eventRandom_);
        enemy_.rewardVariant = 0;
        for (const auto candidate : list.children("event")) {
            if (candidate == outcome) {
                break;
            }
            ++enemy_.rewardVariant;
        }
        const auto reward = outcome.child("autoReward");
        const std::string_view tier = reward.attribute("level").as_string();
        const int level = tier == "LOW" ? 0 : tier == "MED" ? 1 : tier == "HIGH" ? 2 : 3;
        enemy_.rewardUnsupportedCrew = bool(outcome.child("crewMember"));
        pendingResources_ = generateCombatReward(reward.child_value(), level);
        if (enemy_.rewardUnsupportedCrew) {
            stageCombatCrewReward();
        }
        eventText_ = events_.text(outcome.child("text"), eventRandom_, texts_, locale_);
    }
    choices_ = {{"Continue...", {}, true}};
    resetEventChoiceDelay();
    eventId_ = surrender ? "P06_SURRENDER_REWARD" : "P06_VICTORY_REWARD";
    openingEvent_ = false;
    event_ = true;
    surrenderDialog_ = false;
}

void Run::reseedShipOutcome() {
    // WorldManager::OnLoop 0x5f6520 passes currentShipEvent.shipSeed to
    // EventGenerator::GetBaseEvent 0x5bb240 for surrender, deadCrew, gotaway,
    // destroyed and escape. GetBaseEvent calls srandom32 0x62f000 (useSysRand
    // is cleared by CreateNewGame 0x5f46b0) unless the seed is -1, before it
    // looks the (possibly empty) event name up.
    if (encounterShipSeed_ != -1) {
        eventRandom_ = RandomStream(static_cast<std::uint32_t>(encounterShipSeed_));
    }
}

pugi::xml_node Run::createShipOutcomeEvent(pugi::xml_node outcome) {
    // ShipEvent keeps only the surrender/escape/gotaway/destroyed/deadCrew
    // event names (EventGenerator::GetShipEvent 0x5b73f0). The event is built
    // when it triggers, and CreateEvent generates every nested choice reward
    // then; CreateChoiceBox 0x5f0f70 shows the rewards of non-hidden choices.
    const auto freeze = [this](pugi::xml_node event, RandomStream& stream) {
        freezeVoyageEvent(event, stream);
    };
    const auto xml = materializeEvent(outcome, eventRandom_, advanced_, usedVoyageEvents_, freeze);
    restoredEvents_.emplace_back();
    auto& document = restoredEvents_.back();
    if (!document.load_string(xml.c_str())) {
        throw std::runtime_error("invalid_ship_outcome_event");
    }
    // WorldManager::OnLoop 0x5f6690 names the outcome in generatedEvent and
    // restarts choiceHistory; native keys map to original names on export.
    generatedEvent_ = outcome.attribute("load") ? outcome.attribute("load").value()
                                                : outcome.attribute("_nativeEventTemplate").value();
    choiceHistory_.clear();
    return document.document_element();
}

void Run::advanceArtillery() {
    for (int owner : {0, 1}) {
        const auto& systems = owner == 0 ? systems_ : enemy_.systems;
        for (std::size_t index = 0; index < systems.size(); ++index) {
            if (systems[index].definition.id == 11) {
                advanceCombatSystem(owner, index);
            }
        }
    }
}

bool Run::cloaked(int owner) const {
    return owner == 0 ? cloak_.active : enemy_.cloak.active;
}

int Run::batteryPower(bool enemy) const {
    const auto& systems = enemy ? enemy_.systems : systems_;
    if (std::ranges::none_of(systems, [](const auto& s) {
            return s.definition.id == 12;
        })) {
        return 0;
    }
    // The PowerManager keeps the capacity set by the battery's last OnLoop, so
    // the frame whose 30-second timer expires still lends twice the power.
    if (const int snapshot = batteryCapacity_[std::size_t(enemy)]; snapshot >= 0) {
        return snapshot;
    }
    const auto& ability = enemy ? enemy_.battery : battery_;
    if (!ability.active) {
        return 0;
    }
    for (const auto& s : systems) {
        if (s.definition.id == 12 && (!s.hackActive || s.hackLevel <= 1)) {
            return 2 * std::clamp(s.power, 0, s.health);
        }
    }
    return 0;
}

int Run::reactorCapacity(bool enemy) const {
    const auto& systems = enemy ? enemy_.systems : systems_;
    const auto battery = std::ranges::find_if(systems, [](const auto& s) {
        return s.definition.id == 12;
    });
    const int hacked =
        battery != systems.end() && battery->hackActive && battery->hackLevel > 1 ? 2 : 0;
    // BatterySystem::OnLoop 0x608ed0 passes its strong-hack state to
    // PowerManager::SetHacked 0x620140, reducing usable reactor capacity by two.
    const int reactor = enemy ? enemy_.ship.reactor : ship_.reactor;
    const int divisor = enemy ? enemy_.reactorPowerDivide : reactorPowerDivide_;
    const int cap = divisor != 1 ? (reactor + divisor - 1) / divisor
                    : enemy      ? enemy_.reactorPowerCap
                                 : reactorPowerCap_;
    const int loss = enemy ? enemy_.reactorPowerLoss : reactorPowerLoss_;
    // PowerManager::GetMaxPower0x620100 applies the current cap after loss/hack;
    // SetDividePower0x6201e0 overwrites that cap using the full reactor amount.
    return std::max(0, std::min(reactor - loss - hacked, cap));
}

void Run::assignBatteryPower(RunSystem& system, int amount, bool enemy) {
    // PowerManager::IncreasePower 0x620040 uses reactor bars before battery
    // bars. The caller has already increased this system's total power.
    int reactorUsedBefore = -amount;
    for (const auto& entry : enemy ? enemy_.systems : systems_) {
        const int id = entry.definition.id;
        if ((id < 6 || id > 8) && id != 12) {
            reactorUsedBefore += entry.power - entry.batteryPower - entry.bonusPower;
        }
    }
    const int freeReactor = std::max(0, reactorCapacity(enemy) - reactorUsedBefore);
    system.batteryPower += std::max(0, amount - freeReactor);
}

void Run::releaseBatteryPower(RunSystem& system, int amount, [[maybe_unused]] bool enemy) {
    // PowerManager::DecreasePower 0x61ffa0 returns battery bars first.
    system.batteryPower = std::max(0, system.batteryPower - amount);
}

bool Run::systemLocked(const RunSystem& system) const {
    // GetLocked615930 tests the shared indefinite/positive lock count and
    // strong hacking independently of the attached-hack presentation flag.
    return system.lockCount > 0 || system.lockCount == -1 || system.hackLevel > 1;
}

void Run::syncSystemLock(RunSystem& system, bool enemy) {
    system.ion = float(std::max(0, system.lockCount));
    system.ionTimer = system.lockCount > 0 ? 5.f - system.lockElapsed : 0.f;
    const float remaining =
        system.lockCount > 0 ? float(system.lockCount) * 5.f - system.lockElapsed : 0.f;
    switch (system.definition.id) {
    case 10:
        (enemy ? enemy_.cloak : cloak_).cooldown = remaining;
        break;
    case 12:
        (enemy ? enemy_.battery : battery_).cooldown = remaining;
        break;
    case 14:
        (enemy ? enemy_.mind : mind_).cooldown = remaining;
        break;
    case 15:
        (enemy ? enemy_.hacking : hacking_).cooldown = remaining;
        break;
    default:
        break;
    }
}

void Run::checkSystemRepower(RunSystem& system, bool enemy) {
    // Every caller of CheckForRepower 0x614890 (OnLoop, UpdateLock, LockSystem,
    // AddLock, PartialRepair, RemoveBatteryPower, ClearStatus, SetPowerCap/Loss/
    // DividePower) skips iSystemType 3 and 4: weapons and drones never regain
    // power after ion, repair or a lifted cap.
    if (system.definition.id == 3 || system.definition.id == 4 || systemLocked(system)) {
        return;
    }
    const bool subsystem =
        (system.definition.id >= 6 && system.definition.id <= 8) || system.definition.id == 12;
    // CheckForRepower614890 retries the virtual increase one allocation at a
    // time. A failed allocation replaces intent with reactor-only power;
    // retaining an unattainable target would retry it forever in native code.
    const auto reactorRaw = [&] {
        return system.power - system.bonusPower - system.batteryPower;
    };
    while (system.power < system.restorePower && reactorRaw() < systemCapacity(system)) {
        const int amount = system.definition.id == 0 ? 2 - (system.power & 1) : 1;
        if (system.definition.id == 0 && systemCapacity(system) - system.power < amount) {
            break;
        }
        if (subsystem) {
            system.restorePower = reactorRaw();
            return;
        }
        if (!forceIncreaseSystemPower(system, enemy)) {
            system.restorePower = reactorRaw();
            return;
        }
    }
}

bool Run::forceIncreaseSystemPower(RunSystem& system, bool enemy) {
    const int id = system.definition.id;
    int budget = reactorCapacity(enemy) + batteryPower(enemy);
    for (const auto& allocated : enemy ? enemy_.systems : systems_) {
        const int allocatedId = allocated.definition.id;
        if ((allocatedId < 6 || allocatedId > 8) && allocatedId != 12) {
            budget -= allocated.power - allocated.bonusPower;
        }
    }
    const auto increaseRaw = [&](int amount) {
        if (system.power + amount > systemCapacity(system) || amount > budget) {
            return false;
        }
        system.power += amount;
        assignBatteryPower(system, amount, enemy);
        return true;
    };
    if (id == 3) {
        // WeaponSystem::ForceIncrease60fa20 picks the first eligible dormant
        // factory. Its ammunition condition tests availability, not the full
        // per-volley spend tested separately by ReadyToFire40c1b0.
        if (systemLocked(system)) {
            return false;
        }
        auto& powered = enemy ? enemy_.powered : weaponPower_;
        const auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
        const auto& ship = enemy ? enemy_.ship : ship_;
        const int ammunition = enemy ? enemy_.missiles : missiles_;
        for (std::size_t index = 0; index < powered.size(); ++index) {
            if (powered[index]) {
                continue;
            }
            const auto& blueprint = catalog_.equipment(ship.weapons[index]);
            if (blueprint.missiles != 0 && ammunition <= 0 &&
                weapons[index].pendingProjectiles.empty()) {
                continue;
            }
            const int cost = std::max(0, blueprint.power - weapons[index].bonusPower);
            if (increaseRaw(cost)) {
                powered[index] = true;
                return true;
            }
        }
        return false;
    }
    if (id == 4) {
        // ForceIncrease6122e0 tries dormant installed objects in order, using
        // the internal non-user activation and its derived deployment gate.
        for (auto& drone : enemy ? enemy_.drones : combatDrones_) {
            if (!drone.powered && !drone.bossSurge && powerDrone(drone, int(enemy))) {
                return true;
            }
        }
        return false;
    }
    if ((id >= 6 && id <= 8) || id == 12) {
        return false;
    }
    return increaseRaw(id == 0 ? 2 - (system.power & 1) : 1);
}

void Run::clampArmamentPower(RunSystem& system, bool enemy) {
    // WeaponSystem/DroneSystem::AddDamage 0x60f920/0x6118e0 and their
    // CheckMaxPower 0x611460/0x613dd0 clamp the full controller credit to
    // GetMaxPower, then visit powered slots backwards while battery, raw and
    // bonus power together exceed it. RawDecreasePower removes one bar each.
    const auto decreaseRaw = [&] {
        if (system.power <= system.bonusPower) {
            return false;
        }
        releaseBatteryPower(system, 1, enemy);
        --system.power;
        return true;
    };
    const int maximum = systemCapacity(system);
    const int bonus = std::min(system.bonusPower, maximum);
    system.power -= system.bonusPower - bonus;
    system.bonusPower = bonus;
    if (system.definition.id == 3) {
        auto& powered = enemy ? enemy_.powered : weaponPower_;
        const auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
        const auto& ship = enemy ? enemy_.ship : ship_;
        for (std::size_t index = powered.size(); index-- > 0 && system.power > maximum;) {
            if (!powered[index]) {
                continue;
            }
            int cost = std::max(
                0, catalog_.equipment(ship.weapons[index]).power - weapons[index].bonusPower);
            while (cost > 0 && decreaseRaw()) {
                --cost;
            }
            if (cost == 0) {
                powered[index] = false;
            }
        }
    } else if (system.definition.id == 4) {
        auto& drones = enemy ? enemy_.drones : combatDrones_;
        for (std::size_t index = drones.size(); index-- > 0 && system.power > maximum;) {
            auto& drone = drones[index];
            if (!drone.powered) {
                continue;
            }
            int cost = std::max(0, catalog_.equipment(drone.blueprint).power - drone.bonusPower);
            while (cost > 0 && decreaseRaw()) {
                --cost;
            }
            if (cost == 0) {
                drone.powered = false;
            }
        }
    }
}

void Run::addSystemDamage(RunSystem& system, int amount, bool enemy) {
    const int id = system.definition.id;
    if (id == 3 || id == 4) {
        // The armament overrides clamp health only at zero (OnLoop applies the
        // upper clamp) and shed slots immediately.
        system.health = std::max(0, system.health - amount);
        clampArmamentPower(system, enemy);
        return;
    }
    // ShipSystem::AddDamage 0x614e50 clamps health to [0, max] and the bonus to
    // the remaining capacity; it leaves raw and battery bars for CheckMaxPower.
    system.health = std::clamp(system.health - amount, 0, system.definition.power);
    const int bonus = std::min(system.bonusPower, systemCapacity(system));
    system.power -= system.bonusPower - bonus;
    system.bonusPower = bonus;
    // Shields::AddDamage 0x60e1f0 then drops one raw bar when the effective
    // power and powerState.first (battery excluded) are both odd and unlocked.
    if (id == 0 && (system.power & 1) &&
        ((system.power - system.bonusPower - system.batteryPower) & 1) && !systemLocked(system) &&
        system.power > system.bonusPower) {
        releaseBatteryPower(system, 1, enemy);
        --system.power;
    }
}

void Run::checkSystemMaxPower(RunSystem& system, bool enemy) {
    const int maximum = systemCapacity(system);
    const int id = system.definition.id;
    if (id == 3 || id == 4) {
        clampArmamentPower(system, enemy);
    }
    if ((id >= 6 && id <= 8) || id == 12) {
        // Passive systems own reactor-free raw state; an active lock prevents
        // upward restoration while still permitting the downward cap clamp.
        system.power = systemLocked(system) ? std::min(system.power, maximum) : maximum;
    } else {
        // RawDecreasePower617230 / PowerManager61ffa0 remove exactly ONE bar,
        // battery first, even for Shields (whose requiredPower is two).
        while (system.power > maximum && system.power > system.bonusPower) {
            releaseBatteryPower(system, 1, enemy);
            --system.power;
        }
    }
}

void Run::setSystemLock(RunSystem& system, int count, bool enemy) {
    // LockSystem6159d0 replaces the count. Only a positive replacement starts
    // a fresh five-second timer; clearing/indefinite locks retain its phase.
    const bool released = count == 0 && system.lockCount != 0;
    system.lockCount = count;
    if (count > 0) {
        system.lockElapsed = 0;
    }
    syncSystemLock(system, enemy);
    if (released) {
        checkSystemRepower(system, enemy);
    }
}

void Run::addSystemLock(RunSystem& system, int count, bool enemy) {
    // AddLock615aa0 replaces inactive/indefinite/strong-hacked locks, while
    // adding to an ordinary positive lock preserves the current timer phase.
    if (system.lockCount <= 0 || system.hackLevel > 1) {
        setSystemLock(system, count, enemy);
    } else {
        system.lockCount = int(std::min(std::int64_t(5), std::int64_t(system.lockCount) + count));
        syncSystemLock(system, enemy);
    }
}

void Run::applySystemIon(RunSystem& system, int amount, bool enemy) {
    if (amount <= 0) {
        return;
    }
    addSystemLock(system, amount, enemy);
    forceDecreaseSystemPower(system, amount, enemy);
}

bool Run::activateAbility(int id, bool enemy) {
    if ((id != 10 && id != 12) || defeated_ || (!enemy && (event_ || mapOpen_ || jumpTicks_ > 0))) {
        return false;
    }
    auto* s = findSystem(enemy ? enemy_.systems : systems_, id);
    auto& ability =
        id == 10 ? (enemy ? enemy_.cloak : cloak_) : (enemy ? enemy_.battery : battery_);
    if (!s || s->health <= 0 || s->power <= 0 || systemLocked(*s) || ability.active) {
        return false;
    }
    // CloakingSystem::SetTurnedOn 0x609530; BatterySystem::SetTurnedOn 0x608b80.
    ability.active = true;
    ability.elapsed = 0;
    ability.duration = id == 10 ? 5.f * std::min(s->power, s->health) : 30.f;
    setSystemLock(*s, -1, enemy);
    if (id == 10) {
        ability.soundEffect = "cloak";
    } else {
        combatSound({"batteryStart"}, tick_);
        // SetTurnedOn 0x608b80 sets PowerManager::SetBatteryPower(2 x effective).
        batteryCapacity_[std::size_t(enemy)] = 2 * std::clamp(s->power, 0, s->health);
    }
    return true;
}

void Run::stopAbility(int id, bool enemy) {
    auto& ability =
        id == 10 ? (enemy ? enemy_.cloak : cloak_) : (enemy ? enemy_.battery : battery_);
    if (!ability.active) {
        return;
    }
    ability.active = false;
    if (id == 12) {
        // The battery's next OnLoop (Jump runs it at once) clears the lend; the
        // timer-expiry branch of advanceCombatSystem restores this frame's lend.
        batteryCapacity_[std::size_t(enemy)] = 0;
    }
    auto* source = findSystem(enemy ? enemy_.systems : systems_, id);
    int addedLocks = 4;
    if (id == 12) {
        const auto& ship = enemy ? enemy_.ship : ship_;
        float modifier = 0;
        for (const auto& augment : ship.augments) {
            if (augment == "BATTERY_BOOSTER") {
                modifier += catalog_.equipment(augment).value;
            }
        }
        if (modifier > 0) {
            addedLocks = int(4.f * modifier);
        }
    }
    if (source) {
        addSystemLock(*source, addedLocks, enemy);
    }
    ability.soundEffect = id == 10 ? "decloak" : "batteryStop";
}

bool Run::forceDecreaseSystemPower(RunSystem& system, int amount, bool enemy) {
    const auto raw = [&] {
        if (system.power <= system.bonusPower) {
            return false;
        }
        releaseBatteryPower(system, 1, enemy);
        --system.power;
        return true;
    };
    const int id = system.definition.id;
    if (id == 3) {
        auto& powered = enemy ? enemy_.powered : weaponPower_;
        auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
        const auto& ship = enemy ? enemy_.ship : ship_;
        for (std::size_t i = powered.size(); i-- > 0 && amount > 0;) {
            if (!powered[i]) {
                continue;
            }
            const int bonus = weapons[i].bonusPower;
            const int required = std::max(0, catalog_.equipment(ship.weapons[i]).power - bonus);
            amount -= required;
            int remaining = required;
            while (remaining > 0 && raw()) {
                --remaining;
            }
            if (remaining == 0) {
                powered[i] = false; // SetPowered40c800 preserves factory charge/aim.
            }
        }
        return !powered.empty() && amount <= 0;
    } else if (id == 4) {
        auto& drones = enemy ? enemy_.drones : combatDrones_;
        for (std::size_t i = drones.size(); i-- > 0 && amount > 0;) {
            auto& drone = drones[i];
            if (!drone.powered) {
                continue;
            }
            const auto& blueprint = catalog_.equipment(drone.blueprint);
            const int required = std::max(0, blueprint.power - drone.bonusPower);
            amount -= required;
            int remaining = required;
            while (remaining > 0 && raw()) {
                --remaining;
            }
            if (remaining == 0) {
                drone.powered = false;
            }
        }
        return !drones.empty() && amount <= 0;
    } else if ((id >= 6 && id <= 8) || id == 12) {
        const bool result = system.power > 0;
        system.power = std::max(0, system.power - amount);
        return result;
    } else {
        bool result = true;
        while (amount > 0 && result) {
            result = raw();
            --amount;
        }
        // Shields::ForceDecreasePower0x60b030 restores an even pair only when
        // unlocked and both effective and assigned power are odd.
        if (id == 0 && (system.power & 1) && ((system.power - system.bonusPower) & 1) &&
            !systemLocked(system)) {
            return raw();
        }
        return result;
    }
}

bool Run::powerWeaponSlot(RunSystem& system, std::size_t index, bool enemy, bool force) {
    // WeaponSystem::PowerWeapon 0x6101c0: GetLocked blocks unless forced, an
    // already powered factory succeeds, and missile factories need ammunition
    // or queued shots before GetMaxPower and RawIncreasePower 0x617180 apply.
    auto& powered = enemy ? enemy_.powered : weaponPower_;
    const auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
    const auto& ship = enemy ? enemy_.ship : ship_;
    if (index >= powered.size() || index >= weapons.size() || index >= ship.weapons.size() ||
        (systemLocked(system) && !force)) {
        return false;
    }
    if (powered[index]) {
        return true;
    }
    const auto& blueprint = catalog_.equipment(ship.weapons[index]);
    const int ammunition = enemy ? enemy_.missiles : missiles_;
    if (blueprint.missiles != 0 && ammunition <= 0 && weapons[index].pendingProjectiles.empty()) {
        return false;
    }
    const int cost = std::max(0, blueprint.power - weapons[index].bonusPower);
    const int reactorRaw = system.power - system.bonusPower - system.batteryPower;
    if (systemCapacity(system) - reactorRaw < cost) {
        return false;
    }
    if (cost > 0) {
        int budget = reactorCapacity(enemy) + batteryPower(enemy);
        for (const auto& allocated : enemy ? enemy_.systems : systems_) {
            const int id = allocated.definition.id;
            if ((id < 6 || id > 8) && id != 12) {
                budget -= allocated.power - allocated.bonusPower;
            }
        }
        if (system.power + cost > systemCapacity(system) || cost > budget) {
            return false;
        }
        system.power += cost;
        assignBatteryPower(system, cost, enemy);
    }
    powered[index] = true;
    return true;
}

void Run::removeBatteryPower(RunSystem& system, bool enemy) {
    const int id = system.definition.id;
    if (id == 3) {
        // WeaponSystem::RemoveBatteryPower 0x6105d0 records the powered
        // factories, sheds one bar through ForceDecreasePower, then retries
        // PowerWeapon(false, false) from the back for every factory it lost.
        auto& powered = enemy ? enemy_.powered : weaponPower_;
        const auto before = powered;
        forceDecreaseSystemPower(system, 1, enemy);
        for (std::size_t index = std::min(before.size(), powered.size()); index-- > 0;) {
            if (before[index] && !powered[index]) {
                (void)powerWeaponSlot(system, index, enemy, false);
            }
        }
        return;
    }
    if (id == 4) {
        // DroneSystem::RemoveBatteryPower 0x612090 does the same with the
        // reactor-only PowerDrone clone 0x611c90 for deployable or deployed drones.
        auto& drones = enemy ? enemy_.drones : combatDrones_;
        std::vector<bool> before;
        before.reserve(drones.size());
        for (const auto& drone : drones) {
            before.push_back(drone.powered);
        }
        forceDecreaseSystemPower(system, 1, enemy);
        for (std::size_t index = std::min(before.size(), drones.size()); index-- > 0;) {
            if (before[index] && !drones[index].powered) {
                (void)powerDrone(drones[index], int(enemy));
            }
        }
        return;
    }
    // ShipSystem::RemoveBatteryPower 0x614b50: one virtual ForceDecreasePower
    // bar (Shields keeps its pair), then CheckForRepower refills from free
    // reactor power up to the last user allocation.
    forceDecreaseSystemPower(system, 1, enemy);
    checkSystemRepower(system, enemy);
}

void Run::updateReactor(bool enemy) {
    // ShipManager::UpdateReactor 0x4918a0: while used reactor power exceeds
    // PowerManager::GetMaxPower 0x620100 (e.g. an ion storm's divided cap),
    // a random system loses one bar through its ForceDecreasePower.
    auto& systems = enemy ? enemy_.systems : systems_;
    if (systems.empty()) {
        return;
    }
    const auto used = [&] {
        int total = 0;
        for (const auto& entry : systems) {
            const int id = entry.definition.id;
            if ((id < 6 || id > 8) && id != 12) {
                total += entry.power - entry.batteryPower - entry.bonusPower;
            }
        }
        return total;
    };
    const int maximum = reactorCapacity(enemy);
    // The original loops until it fits; the guard only prevents a hang if no
    // system can shed a reactor bar.
    for (int attempt = 0; attempt < 10000 && used() > maximum; ++attempt) {
        auto& chosen = systems[combatRandom_.next() % systems.size()];
        forceDecreaseSystemPower(chosen, 1, enemy);
    }
}

void Run::reconcilePower(bool enemy) {
    auto& systems = enemy ? enemy_.systems : systems_;
    int allocated = 0;
    for (const auto& s : systems) {
        allocated += s.batteryPower;
    }
    int excess = allocated - batteryPower(enemy);
    int remaining = allocated;
    const bool capacityReduced = excess > 0;
    // ShipManager::CheckBatteryPower 0x4913a0 visits systems in installation
    // order and draws once per allocated bar, including bars visited after the
    // shortfall has been filled. A selected shield pair loses two.
    for (auto& s : systems) {
        int candidates = s.batteryPower;
        while (candidates > 0 && remaining > 0 && capacityReduced) {
            const bool remove = int(combatRandom_.next() % unsigned(remaining)) < excess;
            if (!remove) {
                --candidates;
                --remaining;
                continue;
            }
            const int previous = s.batteryPower;
            const int id = s.definition.id;
            // CheckBatteryPower calls the virtual RemoveBatteryPower (slot 27)
            // regardless of locks, then advances its original candidate
            // counters by one (two for a shield pair), even when a virtual
            // weapon/drone callback removes a whole slot.
            removeBatteryPower(s, enemy);
            const int removed = id == 0 && previous > 1 ? 2 : 1;
            remaining = std::max(0, remaining - removed);
            excess = std::max(0, excess - removed);
            candidates -= removed;
        }
    }
}

void Run::advanceAbilities() {
    for (bool enemy : {false, true}) {
        if (enemy && !enemy_.present) {
            continue;
        }
        for (int id : {10, 12}) {
            auto& ability =
                id == 10 ? (enemy ? enemy_.cloak : cloak_) : (enemy ? enemy_.battery : battery_);
            auto* s = findSystem(enemy ? enemy_.systems : systems_, id);
            if (!ability.active) {
                if (!ability.soundEffect.empty()) {
                    combatSound({ability.soundEffect}, tick_);
                    ability.soundEffect.clear();
                }
                continue;
            }
            // OnLoop 0x609930/0x608ed0 recalculates effective power and stops
            // an active system on complete damage, ion loss or a strong hack.
            const int effective = s ? std::clamp(s->power, 0, s->health) : 0;
            const float duration = id == 10 ? 5.f * effective : 30.f;
            if (ability.duration > 0 && ability.duration != duration) {
                // TimerHelper::SetMaxTime5a95b0 preserves the elapsed fraction.
                ability.elapsed = ability.elapsed / ability.duration * duration;
            }
            ability.duration = duration;
            ability.elapsed += Time::step;
            if (effective == 0 || (s && s->hackActive && s->hackLevel > 1) ||
                ability.elapsed > ability.duration) {
                stopAbility(id, enemy);
            }
            if (!ability.soundEffect.empty()) {
                combatSound({ability.soundEffect}, tick_);
                ability.soundEffect.clear();
            }
        }
    }
    reconcilePower();
}

void Run::offerDefenseProjectile(CombatDrone& drone,
                                 int owner,
                                 const CombatProjectile& projectile) {
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    if (blueprint.type != "DEFENSE" || !drone.powered ||
        (projectile.resolved && !projectile.missed) ||
        (projectile.crystalShard ? -1 : projectile.owner) == owner ||
        projectile.space != drone.space) {
        return;
    }
    const auto& weapon = catalog_.equipment(projectile.weapon);
    // Projectile::Initialize452130 overrides the runtime Targetable category.
    const int type = projectile.droneTargetable != -1 ? projectile.droneTargetable == 0 ? 4 : 2
                     : weapon.type == "MISSILES" || projectile.crystalShard ? 1
                     : projectile.weapon == "ENV_ASTEROID"                  ? 2
                     : weapon.type == "LASER" || weapon.type == "BURST"     ? 4
                                                                            : 5;
    if (defenseAcceptsType(blueprint, type)) {
        acceptDefenseTarget(drone,
                            blueprint,
                            type,
                            projectile.id,
                            false,
                            projectile.position,
                            {projectile.position.x - projectile.previous.x,
                             projectile.position.y - projectile.previous.y});
    }
}

void Run::offerDefenseDrone(CombatDrone& drone,
                            int owner,
                            const CombatDrone& target,
                            int targetOwner) {
    const auto& blueprint = catalog_.equipment(drone.blueprint);
    if (blueprint.type != "DEFENSE" || !drone.powered || owner == targetOwner || !target.powered ||
        !target.deployed || target.ionStun > 0 || target.controllerHackLevel > 1) {
        return;
    }
    const auto& candidate = catalog_.equipment(target.blueprint);
    const bool pod = candidate.type == "BOARDER" || candidate.type == "HACKING";
    const bool destroyed =
        candidate.type == "BOARDER" ? target.delivered && target.crewDead : target.health <= 0;
    const int targetSpace = candidate.type == "HACKING"
                                ? targetOwner == 0 ? hackingDroneSpace_ : enemy_.hackingDroneSpace
                                : target.space;
    if (destroyed || targetSpace != drone.space) {
        return;
    }
    const int type = pod ? target.delivered || cloaked(1 - targetOwner) ? 5 : 6 : 3;
    if (defenseAcceptsType(blueprint, type)) {
        acceptDefenseTarget(
            drone, blueprint, type, target.spaceActorId, true, target.position, target.velocity);
    }
}

void Run::advanceDefenseDroneAim(CombatDrone& drone) {
    // DefenseDrone::PickTarget422540 is called by every GetNextProjectile
    // attempt, even while the firing timer is not ready.
    if (drone.defenseDisrupted && drone.powered) {
        if (drone.desiredAimAngle == drone.aimAngle || drone.desiredAimAngle < 0) {
            drone.desiredAimAngle = float(combatRandom_.next() % 360);
        }
    } else {
        if (!drone.hasAimTarget || drone.defenseDisrupted) {
            return;
        }
        const auto& weapon = catalog_.equipment(catalog_.equipment(drone.blueprint).droneWeapon);
        const SDL_FPoint delta{drone.aimTarget.x - drone.position.x,
                               drone.aimTarget.y - drone.position.y};
        const auto velocity = drone.defenseTargetVelocity;
        const float muzzle =
            (weapon.projectileSpeed > 0 ? weapon.projectileSpeed : 60.f) * Time::originalStep;
        const float distance = delta.x * delta.x + delta.y * delta.y;
        const float b = 2.f * (velocity.x * delta.x + velocity.y * delta.y);
        const float discriminant =
            b * b -
            4.f * (velocity.x * velocity.x + velocity.y * velocity.y - muzzle * muzzle) * distance;
        if (discriminant > 0) {
            const float lead = 2.f * distance / (std::sqrt(discriminant) - b);
            if (lead > 0) {
                drone.aimTarget.x += lead * velocity.x;
                drone.aimTarget.y += lead * velocity.y;
            }
        }
        drone.desiredAimAngle =
            std::atan2(drone.aimTarget.y - drone.position.y, drone.aimTarget.x - drone.position.x) *
            180.f / std::numbers::pi_v<float>;
    }
    if (drone.desiredAimAngle < 0) {
        drone.desiredAimAngle += 360.f;
    }
    const float difference = drone.desiredAimAngle - drone.aimAngle;
    const float direction =
        (difference < 0 && difference > -180.f) || difference > 180.f ? -1.f : 1.f;
    drone.aimAngle += direction * 30.f * Time::originalStep;
    if (drone.aimAngle > 360.f) {
        drone.aimAngle -= 360.f;
    } else if (drone.aimAngle < 0) {
        drone.aimAngle += 360.f;
    }
    const float remaining = drone.aimAngle - drone.desiredAimAngle;
    if ((direction > 0 && remaining > 0 && remaining < 180.f) ||
        (direction < 0 && remaining < 0 && remaining > -180.f)) {
        drone.aimAngle = drone.desiredAimAngle;
    }
    if (drone.defenseDisrupted) {
        return;
    }
    if (drone.aimTarget.x > -150 && drone.aimTarget.x < 450 && drone.aimTarget.y > -150 &&
        drone.aimTarget.y < 450 && drone.aimAngle == drone.desiredAimAngle) {
        drone.defenseShotTargetId = drone.defenseCurrentTargetId;
        drone.defenseShotTargetIsDrone = drone.defenseCurrentTargetIsDrone;
    } else {
        drone.hasAimTarget = false;
    }
}

void Run::sweepProjectileSpaceContacts(CombatProjectile& projectile) {
    const auto& blueprint = catalog_.equipment(projectile.weapon);
    if ((projectile.resolved && !projectile.missed) || blueprint.type == "BEAM" ||
        blueprint.type == "BOMB" || projectile.weapon == "PDS_SHOT") {
        return;
    }
    const int carrier = projectile.crystalShard ? -1 : projectile.owner;
    const int hull = projectile.fake ? 0
                     : projectile.crystalShard
                         ? 1
                         : blueprint.damage + (blueprint.damage > 0 ? projectile.damageBonus : 0);
    const int ion = projectile.fake ? 0
                                    : blueprint.ion + (blueprint.damage <= 0 && blueprint.ion > 0
                                                           ? projectile.damageBonus
                                                           : 0);
    const auto finishCollision = [&] {
        projectile.resolved = projectile.intercepted = true;
        projectile.missed = false;
        projectile.age = 0;
        combatSound({projectile.soundsInitialized      ? projectile.hitShipSound
                     : blueprint.hitShipSounds.empty() ? std::string{}
                                                       : blueprint.hitShipSounds.front()},
                    projectile.id);
    };
    // SpaceManager::UpdateProjectile0x41a190 checks all opposing space actors,
    // including incidental collisions by ordinary and already-missed shots.
    for (const auto& [owner, registered] : registeredSpaceDrones()) {
        offerDefenseProjectile(*registered, owner, projectile);
        if (owner == carrier) {
            continue;
        }
        auto& drone = *registered;
        const auto& definition = catalog_.equipment(drone.blueprint);
        const bool attachedHacking = definition.type == "HACKING" && drone.delivered;
        const bool carrierDead =
            definition.type == "BOARDER" ? drone.delivered && drone.crewDead : drone.health <= 0;
        const int space = definition.type == "HACKING"
                              ? owner == 0 ? hackingDroneSpace_ : enemy_.hackingDroneSpace
                              : drone.space;
        if (attachedHacking || !drone.deployed || carrierDead || drone.spaceExplosionElapsed >= 0 ||
            space != projectile.space) {
            continue;
        }
        const float dx = projectile.position.x - drone.position.x;
        const float dy = projectile.position.y - drone.position.y;
        if (dx * dx + dy * dy >= 100) {
            continue;
        }
        const bool dodged =
            static_cast<int>(combatRandom_.next() % 10) < effectiveDroneDodge(drone) &&
            drone.powered;
        if (dodged) {
            drone.spaceMissElapsed = 0;
            drone.spaceMissPosition = drone.position;
            continue;
        }
        if (ion > 0) {
            // HackingDrone44e2d0 invokes Ionize after its base response,
            // including an unpowered contact; ordinary SpaceDrone does not.
            if (drone.powered || definition.type == "HACKING") {
                drone.ionStun = static_cast<float>(5 * ion);
            }
        } else if (hull > 0) {
            // CollisionMoving41e400 dispatches virtual BlowUp, which starts
            // the retained explosion. Derived OnLoop owns later destruction.
            blowUpDrone(drone);
        }
        finishCollision();
        return;
    }
    for (auto& other : projectiles_) {
        if (other.id == projectile.id || (other.crystalShard ? -1 : other.owner) == carrier ||
            other.space != projectile.space || (other.resolved && !other.missed) ||
            catalog_.equipment(other.weapon).type == "BOMB" ||
            catalog_.equipment(other.weapon).type == "BEAM" || other.weapon == "PDS_SHOT") {
            continue;
        }
        const float dx = projectile.position.x - other.position.x;
        const float dy = projectile.position.y - other.position.y;
        if (dx * dx + dy * dy < 100) {
            other.resolved = other.intercepted = true;
            other.missed = false;
            other.age = 0;
            finishCollision();
            return;
        }
    }
}

void initializeShipExplosion(CombatExplosion& state,
                             const ShipDefinition& ship,
                             RandomStream& random) {
    state = {};
    state.initialized = true;
    // ExplosionAnimation::OnInit 0x487f10 samples angular, velocity, then
    // direction once per authored gib when constructing the ship.
    const auto thousandths = [&](float minimum, float maximum) {
        const int low = int(minimum * 1000.f);
        const int high = int(maximum * 1000.f);
        return float(low + int(random.next() % unsigned(high + 1 - low))) / 1000.f;
    };
    for (const auto& definition : ship.gibs) {
        CombatGib gib;
        gib.rotationSpeed = thousandths(definition.angularMin, definition.angularMax) * .5f;
        const float velocity = thousandths(definition.velocityMin, definition.velocityMax) * 2.5f;
        const int low = int(270.f - definition.directionMax);
        const int high = int(270.f - definition.directionMin);
        const float direction = float(low + int(random.next() % unsigned(high + 1 - low))) *
                                std::numbers::pi_v<float> / 180.f;
        gib.movement = {std::cos(direction) * velocity, std::sin(direction) * velocity};
        state.gibs.push_back(gib);
    }
}

void Run::initializeExplosion(CombatExplosion& state, const ShipDefinition& ship) {
    initializeShipExplosion(state, ship, combatRandom_);
}

void Run::advanceExplosion(CombatExplosion& state,
                           const ShipDefinition& ship,
                           float age,
                           float step) {
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
    const int width = right - left;
    const int height = bottom - top;
    const auto randomEffect = [&](SDL_FPoint point, float scale = 1.f) {
        // AnimationControl::GetAnimation 0x587230 resolves explosion_random.
        const auto name = "explosion_big" + std::to_string(combatRandom_.next() % 3 + 2);
        state.effects.push_back({name, point, 0, scale});
    };
    const auto sound = [&](const std::string& alias, float gain) {
        audio_.sound(alias, false, gain);
        combatSounds_.push_back({{"tick", tick_}, {"alias", alias}, {"volume", gain}});
    };
    // ExplosionAnimation::Update 0x489930 starts the final burst strictly after
    // 1.5 seconds, and does not move pieces on that first final-burst update.
    if (age > 1.5f) {
        if (!state.finalBoom) {
            SDL_FPoint previous{-2147483600.f, -2147483600.f};
            const int count = int(combatRandom_.next() % 2) + 2;
            for (int i = 0; i < count; ++i) {
                SDL_FPoint point;
                do {
                    // Original draws y before x, and uses the room-bound box.
                    point.y = float(top + int(combatRandom_.next() % unsigned(height)));
                    point.x = float(left +
                                    int(combatRandom_.next() % unsigned(std::max(1, width - 128))));
                } while (i > 0 &&
                         int(std::hypot(point.x - previous.x, point.y - previous.y)) <= 63);
                previous = point;
                randomEffect(point, 2.f);
            }
            sound("explosion1", 1.f);
            state.finalBoom = true;
        } else {
            const float factor = step * 16.f;
            for (auto& gib : state.gibs) {
                gib.offset.x += gib.movement.x * factor;
                gib.offset.y += gib.movement.y * factor;
                gib.rotation += gib.rotationSpeed * factor;
            }
        }
    } else if (age < 1.5f) {
        if (state.soundTimer < 0) {
            sound("explosion" + std::to_string(combatRandom_.next() % 4 + 1), .25f);
            state.soundTimer = float(combatRandom_.next() % 3) * .3f + .3f;
        } else {
            state.soundTimer -= step;
        }
        if (state.explosionTimer < 0) {
            if (state.effects.size() <= 24) {
                const int type = int(combatRandom_.next() % 3);
                SDL_FPoint point;
                point.y = float(top + int(combatRandom_.next() % unsigned(height)));
                point.x =
                    float(left + int(combatRandom_.next() % unsigned(std::max(1, width - 32))));
                if (type == 0) {
                    randomEffect(point);
                }
                // Case0 adds two explosion1 effects, case1 two, case2 one.
                const int ordinary = type == 2 ? 1 : 2;
                for (int i = 0; i < ordinary; ++i) {
                    state.effects.push_back({"explosion1", point, 0, 1});
                }
            }
            state.explosionTimer = float(combatRandom_.next() % 10) * .05f;
        }
        state.explosionTimer -= step;
    }
    for (auto& effect : state.effects) {
        effect.elapsed += step;
    }
    std::erase_if(state.effects, [&](const auto& effect) {
        return effect.elapsed > catalog_.animation(effect.animation).duration;
    });
}

bool Run::projectileDead(CombatProjectile& projectile) {
    const auto& blueprint = catalog_.equipment(projectile.weapon);
    if (projectile.dead || (blueprint.type == "BEAM" && projectile.resolved)) {
        if (blueprint.type == "BEAM") {
            const auto stop = [&](CombatWeapon& weapon) {
                if (weapon.lastProjectileId == projectile.id) {
                    weapon.animationFiring = false;
                }
            };
            for (auto& weapon : combatWeapons_) {
                stop(weapon);
            }
            for (auto& weapon : enemy_.weapons) {
                stop(weapon);
            }
            for (auto* stations : {&systems_, &enemy_.systems}) {
                for (auto& station : *stations) {
                    stop(station.artillery);
                }
            }
            for (auto& factory : cargoFactories_) {
                stop(factory.weapon);
            }
            stop(overflowFactory_.weapon);
        }
        return true;
    }
    if (!projectile.resolved || (projectile.missed && blueprint.type != "BOMB")) {
        return false;
    }
    // Projectile::Dead44f020 updates the running death strip before
    // OnUpdate; its tracker completes strictly after the authored time.
    const std::string effect =
        !projectile.explosionImage.empty()                        ? projectile.explosionImage
        : !blueprint.explosionAnimation.empty()                   ? blueprint.explosionAnimation
        : blueprint.type == "MISSILES" || projectile.crystalShard ? "explosion_big2"
        : blueprint.missiles > 0                                  ? "explosion_missile1"
                                                                  : "explosion1";
    projectile.age += Time::step;
    return projectile.age > catalog_.animation(effect).duration;
}

void Run::advanceSpaceProjectiles(std::uint64_t firstNewProjectile) {
    for (std::size_t index = 0; index < projectiles_.size(); ++index) {
        auto& p = projectiles_[index];
        if (p.id >= firstNewProjectile) {
            continue;
        }
        if (projectileDead(p)) {
            projectiles_.erase(projectiles_.begin() + std::ptrdiff_t(index));
            continue; // Shifted successor is deliberately skipped this pass.
        }
        p.previous = p.position;
        const auto& blueprint = catalog_.equipment(p.weapon);
        if (!p.resolved || (p.missed && blueprint.type != "BOMB")) {
            p.age += Time::step;
        }
        // LaserBlast450b00 updates its flight strip and spin even while the
        // death strip is running. CFPS originalStep is16 game units/second;
        // spinSpeed*.0625 therefore advances by spinSpeed game degrees/second.
        if (blueprint.spin > 0 && !p.crystalShard && blueprint.type != "MISSILES" &&
            blueprint.type != "BEAM" && blueprint.type != "BOMB") {
            if (p.weapon == "ENV_ASTEROID") {
                // Asteroid450950 immediately clears rotation while dying.
                if (p.resolved && !p.missed) {
                    p.spinAngle = 0;
                } else {
                    p.spinAngle += 160.f * Time::step;
                    if (p.spinAngle > 360.f) {
                        p.spinAngle = 0;
                    }
                }
            } else {
                p.spinAngle += float(blueprint.spin) * .0625f * Time::originalStep;
            }
        }
        // SpaceManager41c940 checks bounds before virtual OnUpdate, so a
        // crossing made this pass remains in its old space until the next.
        // Beam's carrier position is its emission point; native position is
        // the swept head and cannot be substituted in this base-space check.
        const auto carrierPosition = blueprint.type == "BEAM" ? p.beamOrigin : p.position;
        if (std::abs(carrierPosition.x) > 800 || std::abs(carrierPosition.y) > 800) {
            if (p.space == p.targetShip) {
                p.dead = true; // Kill still precedes this pass's OnUpdate.
            } else if (blueprint.type != "BEAM") {
                // BeamWeapon::EnterDestinationSpace71f8e0 is an empty override.
                if (p.entryAngle == -1) {
                    p.entryAngle =
                        float(combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
                }
                p.space = p.targetShip;
                p.position = p.previous = {200 + 600 * std::cos(p.entryAngle),
                                           200 + 600 * std::sin(p.entryAngle)};
                const float distance =
                    std::hypot(p.target.x - p.position.x, p.target.y - p.position.y);
                p.direction = distance > 0 ? SDL_FPoint{(p.target.x - p.position.x) / distance,
                                                        (p.target.y - p.position.y) / distance}
                                           : SDL_FPoint{1, 0};
            }
        }
        if (p.weapon == "PDS_SHOT") {
            if (!p.resolved) {
                // PDSFire::OnUpdate moves first, then updates scale and speed.
                p.position.x += p.direction.x * p.speed * Time::step;
                p.position.y += p.direction.y * p.speed * Time::step;
                const float total =
                    std::hypot(p.target.x - p.targetEnd.x, p.target.y - p.targetEnd.y);
                const float traveled =
                    std::hypot(p.position.x - p.targetEnd.x, p.position.y - p.targetEnd.y);
                p.speed = (20.f + 3.f * (1.f + (total > 0 ? traveled / total : 0.f) * 7.f)) * 16.f;
                const float remaining =
                    std::hypot(p.target.x - p.position.x, p.target.y - p.position.y);
                if (p.space == p.targetShip && !p.fake && !p.missed && !p.arrived &&
                    remaining <= p.speed * Time::step) {
                    const auto flightPosition = p.position;
                    p.position = p.target;
                    impact(p);
                    p.position = flightPosition;
                    p.resolved = false;
                    p.arrived = true;
                    p.spinAngle = p.age; // Source passedTarget and explosion start time.
                }
            }
            continue;
        }
        // A missed defense shot keeps the same flight: Projectile::OnUpdate
        // 0x450440 ignores `missed` and still steps onto the aim point.
        if ((p.defenseSpaceDroneId != 0 || p.defenseHackingTargetId != 0 ||
             p.defenseDroneTargetId != 0 || p.defenseTargetId != 0) &&
            (!p.resolved || (p.missed && !p.intercepted))) {
            // GetNextProjectile0x428cb0 fixes the launch target point. The
            // Projectile0x450440 travels to it then passes along its last heading;
            // target IDs are selection provenance, never a homing instruction.
            if (!p.arrived) {
                const float dx = p.target.x - p.position.x;
                const float dy = p.target.y - p.position.y;
                const float distance = std::hypot(dx, dy);
                if (distance > 0) {
                    p.direction = {dx / distance, dy / distance};
                }
                const float fraction =
                    distance > 0 ? std::min(1.f, p.speed * Time::step / distance) : 1.f;
                p.position.x += dx * fraction;
                p.position.y += dy * fraction;
                p.arrived = fraction == 1;
            } else {
                p.position.x += p.direction.x * p.speed * Time::step;
                p.position.y += p.direction.y * p.speed * Time::step;
            }
            sweepProjectileSpaceContacts(p);
            // Projectile::CollisionCheck 0x4505a0, after each object's
            // response, misses a shot left within one step of its aim point
            // (destination space, not yet missed): DamageArea and
            // ValidTargetLocation are the Collideable defaults for drones and
            // projectiles, so it plays missSound and sets missed. UpdateProjectile
            // 0x41a190 runs it after the move, so this is the frame before the
            // shot lands, and only when some opposing object shares the space.
            const float remaining =
                std::hypot(p.target.x - p.position.x, p.target.y - p.position.y);
            const bool contact =
                std::ranges::any_of(registeredSpaceDrones(),
                                    [&](const auto& entry) {
                                        return entry.first != p.owner &&
                                               entry.second->space == p.space;
                                    }) ||
                std::ranges::any_of(projectiles_, [&](const CombatProjectile& other) {
                    return other.id != p.id && !other.dead && other.space == p.space &&
                           (other.crystalShard ? -1 : other.owner) != p.owner;
                });
            if (!p.resolved && (p.arrived || remaining <= p.speed * Time::step) && contact) {
                combatSound({p.soundsInitialized            ? p.missSound
                             : blueprint.missSounds.empty() ? std::string{}
                                                            : blueprint.missSounds.front()},
                            p.id);
                p.resolved = p.missed = true;
                p.age = 0;
            }
            continue;
        }
        if (p.resolved) {
            if (p.missed) {
                p.position.x += p.direction.x * p.speed * Time::step;
                p.position.y += p.direction.y * p.speed * Time::step;
                sweepProjectileSpaceContacts(p);
            }
            continue;
        }
        if (blueprint.type == "BEAM") {
            const bool advancedBeam = p.beamStepped;
            if (p.beamStepped) {
                const auto advanceEffects = [&](auto& effects) {
                    // The original increments its vector index even after erase,
                    // so a shifted successor waits until the following update.
                    for (std::size_t index = 0; index < effects.size(); ++index) {
                        auto& effect = effects[index];
                        effect.elapsed += Time::step;
                        const float duration = effect.animation == "fire_smoke"
                                                   ? .5f
                                                   : catalog_.animation(effect.animation).duration;
                        if (effect.elapsed > duration) {
                            effects.erase(effects.begin() + std::ptrdiff_t(index));
                        }
                    }
                };
                advanceEffects(p.beamContacts);
                advanceEffects(p.beamSmoke);
                p.beamAnimationTimer -= Time::step;
                p.beamHeading += p.beamDrift * 16.f * Time::step;
                const auto& weapons = p.owner == 0 ? combatWeapons_ : enemy_.weapons;
                const auto& powered = p.owner == 0 ? weaponPower_ : enemy_.powered;
                const auto& stations = p.owner == 0 ? systems_ : enemy_.systems;
                bool depowered = false;
                for (std::size_t slot = 0; slot < weapons.size(); ++slot) {
                    depowered |= weapons[slot].lastProjectileId == p.id && !powered[slot];
                }
                if (depowered || std::ranges::any_of(stations, [&](const RunSystem& station) {
                        return station.definition.id == 11 &&
                               station.artillery.lastProjectileId == p.id && station.power <= 0;
                    })) {
                    p.resolved = true;
                    p.age = 0;
                    continue;
                }
            }
            const float dx = p.targetEnd.x - p.target.x;
            const float dy = p.targetEnd.y - p.target.y;
            const float distance = std::hypot(dx, dy);
            const float limit = std::min(distance, float(blueprint.length));
            const float previousProgress =
                std::hypot(p.position.x - p.target.x, p.position.y - p.target.y);
            // OnUpdate435c90 runs only after the first CollisionCheck. It moves
            // toward target2, then tests authored length strictly; length does
            // not clamp the motion at its boundary.
            const float progress =
                std::min(distance, previousProgress + (p.beamStepped ? p.speed * Time::step : 0.f));
            if (distance > 0) {
                // Keep the preceding simulation position for render interpolation;
                // otherwise a zero-alpha capture draws a zero-length beam.
                p.previous = p.position;
                p.position = {p.target.x + dx * progress / distance,
                              p.target.y + dy * progress / distance};
            }
            const auto& destination = p.targetShip == 1 ? enemy_.ship : ship_;
            // BeamWeapon::OnUpdate 0x435c90 raises bDamageSuperShield on TWO
            // threshold crossings: 33% for every beam and 80% when its authored
            // length exceeds 20. CollisionCheck clears the flag at 0x436978.
            // Derive the one-tick pulse from saved geometry, not a persistent
            // intercepted flag or room-entry event, so checkpoints need no new field.
            const bool damageSuperShield =
                limit > 0 && ((previousProgress < .33f * limit && progress >= .33f * limit) ||
                              (blueprint.length > 20 && previousProgress < .8f * limit &&
                               progress >= .8f * limit));
            int& superShields = p.targetShip == 1 ? enemy_.superShields : superShields_;
            int& bubbles = p.targetShip == 1 ? enemy_.shields : shieldBubbles_;
            const auto shieldPoint =
                (bubbles > 0 || superShields > 0)
                    ? beamShieldEntry(destination, p.beamSubStart, p.position, p.targetShip == 1)
                    : std::nullopt;
            // Raytrace returns CollisionTest's sampled level; CollisionReal's
            // repeated-id cached response is used only to avoid a second hit.
            const int shields = shieldPoint ? bubbles : 0;
            const bool superShieldStopped = shieldPoint && superShields > 0;
            if (shieldPoint) {
                const bool fresh = touchShieldHit(p.targetShip, *shieldPoint, p.id, shields);
                if (fresh && superShields > 0 && blueprint.ion > 0) {
                    const int ion =
                        ionAfterResistance(p.targetShip, blueprint.ion, *shieldPoint, true);
                    if (ion > 0) {
                        superShields = std::max(0, superShields - 2 * ion);
                        pushCombatMessage(DamageMessage{p.targetShip,
                                                        2 * ion,
                                                        *shieldPoint,
                                                        tick_,
                                                        true,
                                                        false,
                                                        false,
                                                        0,
                                                        true});
                    }
                }
                if (!superShieldStopped) {
                    ionizeShieldCollision(
                        p.targetShip, blueprint.ion, blueprint.stun, blueprint.stunChance);
                }
            }
            if (superShieldStopped && damageSuperShield) {
                // Shields::CollisionReal 0x60d980's super arm spends the full
                // nonnegative hull + boost + twice-ion payload once per pulse.
                // Beam CollisionCheck436040 assigns a NEW SpaceId to each pulse,
                // so the 33% and 80% pulses are distinct despite arc deduplication.
                const auto pulseId = nextProjectile_++;
                if ((superShields > 0 || bubbles > 0) &&
                    touchShieldHit(p.targetShip, *shieldPoint, pulseId, bubbles)) {
                    if (superShields > 0) {
                        // As for projectiles: max(0, iDamage + 2 x ion), so a
                        // zero-payload beam (BEAM_BIO) spends no layer and
                        // raises no message.
                        const int ionPayload =
                            ionAfterResistance(p.targetShip, blueprint.ion, *shieldPoint, true);
                        const int hullPayload =
                            blueprint.damage + (blueprint.damage > 0 ? p.damageBonus : 0);
                        const int absorbed = std::max(0, hullPayload + 2 * ionPayload);
                        if (absorbed > 0) {
                            superShields = std::max(0, superShields - absorbed);
                            pushCombatMessage(DamageMessage{p.targetShip,
                                                            absorbed,
                                                            *shieldPoint,
                                                            tick_,
                                                            true,
                                                            false,
                                                            false,
                                                            0,
                                                            true});
                        }
                    } else if (bubbles > blueprint.piercing) {
                        --bubbles;
                    }
                }
            }
            // Sweep the whole fixed-step segment, not just its end point. Fast
            // beams must not skip a narrow crossed room. Resolve in travel order.
            // BeamWeapon::CollisionCheck 0x436040 hands the beam to
            // ShipManager::DamageBeam 0x49e4d0 once per swept step with `current`
            // at the head and `last` at BeamWeapon::last_collision.point, and
            // returns at LABEL_54 without reaching it whenever the shield stopped
            // the beam this step (0x436944 `goto LABEL_54` when piercedShield is
            // clear), so a stopped step rolls no beam hazard and no crew hit.
            const int shieldAbsorbed = std::max(0, shields - blueprint.piercing);
            const int beamDamage = std::max(0, blueprint.damage + p.damageBonus - shieldAbsorbed);
            // CollisionCheck computes piercedShield before DamageShield runs;
            // the pulse that removes the final layer still stops this step.
            // The next collision invocation can reach the room payload.
            const bool beamStopped =
                superShieldStopped ||
                (shields > 0 && shields - blueprint.piercing >= blueprint.damage + p.damageBonus);
            p.beamLastDamage = beamDamage;
            p.beamPierced = !beamStopped;
            if (beamStopped) {
                p.intercepted = true;
            }
            const bool validBeamHead =
                std::ranges::any_of(destination.rooms, [&](const auto& room) {
                    return Rect{float(room.x), float(room.y), float(room.w), float(room.h)}
                        .contains(p.position.x, p.position.y);
                });
            // DamageBeam 0x49e621 consumes its resistance check on every
            // valid invocation, including steps that enter no new room/tile.
            if (!beamStopped && p.beamVisualInitialized) {
                if (p.beamAnimationTimer < 0) {
                    p.beamContacts.push_back({p.projectileImage, {}, 0, 1});
                    p.beamAnimationTimer = float(combatRandom_.next() % 10 + 25) / 100.f;
                }
                if (std::hypot(p.beamLastSmoke.x - p.beamLastCollision.x,
                               p.beamLastSmoke.y - p.beamLastCollision.y) > 5.f) {
                    CombatExplosionEffect smoke;
                    smoke.animation = beamDamage > 2 ? "explosion1" : "fire_smoke";
                    smoke.scale = beamDamage > 2 ? .75f : beamDamage == 2 ? .5f : .2f;
                    if (beamDamage > 2) {
                        const auto& animation = catalog_.animation("explosion1");
                        if (animation.usable()) {
                            smoke.elapsed = 4.f / animation.frames * animation.duration;
                        }
                    }
                    // x86 cvttss2si returns INT_MIN on float overflow; the untouched
                    // ctor sentinels deliberately produce an offscreen first smoke.
                    const auto truncatedCoordinate = [](float value) {
                        return value >= 2147483648.f || value < -2147483648.f
                                   ? float(std::numeric_limits<int>::min())
                                   : float(int(value));
                    };
                    const int jitterX = int(combatRandom_.next() % 7) - 3;
                    const int jitterY = int(combatRandom_.next() % 7) - 3;
                    smoke.position = {
                        truncatedCoordinate(p.beamLastSmoke.x - 17.f * smoke.scale + jitterX),
                        truncatedCoordinate(p.beamLastSmoke.y - 17.f * smoke.scale + jitterY)};
                    p.beamSmoke.push_back(std::move(smoke));
                    p.beamLastSmoke = p.beamLastCollision;
                }
            }
            const bool beamResistance = !beamStopped && validBeamHead
                                            ? resistAugmentation(p.targetShip, "ROCK_ARMOR")
                                            : false;
            // BeamWeapon::CollisionCheck 0x436a6b calls DamageBeam once with
            // the current head and the preceding collision point. LABEL_54
            // updates that point even after a blocked step. Native's fixed80px/sec beam step
            // is narrower than a room/tile, so the original point transitions do
            // not require a second synthetic segment collision.
            if (!beamStopped) {
                const auto roomAt = [&](SDL_FPoint point) {
                    for (const auto& room : destination.rooms) {
                        if (Rect{float(room.x), float(room.y), float(room.w), float(room.h)}
                                .contains(point.x, point.y)) {
                            return room.id;
                        }
                    }
                    return -1;
                };
                const int roomId = roomAt(p.position);
                const int lastRoomId = p.beamStepped ? roomAt(p.beamLastCollision) : -1;
                if (roomId >= 0) {
                    CombatProjectile hit = p;
                    hit.room = roomId;
                    hit.checkedShield = true;
                    hit.damageBonus = beamDamage - blueprint.damage;
                    if (roomId != lastRoomId) {
                        if (std::ranges::find(p.hitRooms, roomId) == p.hitRooms.end()) {
                            p.hitRooms.push_back(roomId);
                        }
                        impact(hit, false, &beamResistance);
                        beamHitAt_[std::size_t(p.targetShip)][roomId] =
                            float(Time::seconds(double(tick_)));
                    }
                    const int tileX = int(p.position.x) / 35;
                    const int tileY = int(p.position.y) / 35;
                    if (!p.beamStepped || tileX != int(p.beamLastCollision.x) / 35 ||
                        tileY != int(p.beamLastCollision.y) / 35) {
                        beamTileImpact(hit, beamResistance);
                    }
                }
            }
            p.beamLastCollision = p.position;
            p.beamStepped = true;
            if (advancedBeam && (progress >= distance || progress > float(blueprint.length))) {
                p.resolved = true;
                p.age = 0;
            }
            continue;
        }
        if (blueprint.type == "BOMB") {
            // CollisionCheck0x44a5c0 teleports/reverses on its first update.
            // OnUpdate0x44a290 then waits for the authored reverse strip to
            // finish and counts down the constructor's .4-second fuse.
            if (!p.arrived) {
                p.space = p.targetShip;
                p.position = p.previous = p.target;
                p.arrived = true;
                p.bombAnimationElapsed = 0;
                projectileDodged(p);
                if (p.targetShip == p.owner) {
                    p.missed = false; // The own-ship roll is consumed but ignored.
                }
                const auto& destination = p.targetShip == 1 ? enemy_.ship : ship_;
                if (p.missed) {
                    const int direction = int(combatRandom_.next() % 4);
                    const auto inRoom = [&] {
                        return std::ranges::any_of(destination.rooms, [&](const auto& room) {
                            return Rect{float(room.x), float(room.y), float(room.w), float(room.h)}
                                .contains(p.position.x, p.position.y);
                        });
                    };
                    while (inRoom()) {
                        p.position.x += direction == 2 ? -117.f : direction == 3 ? 117.f : 0;
                        p.position.y += direction == 0 ? -117.f : direction == 1 ? 117.f : 0;
                    }
                    p.bombMissTick = tick_;
                } else {
                    const int barrier = p.targetShip == 1 ? enemy_.superShields : superShields_;
                    if (barrier > 0) {
                        // GetSuperShield0x4900d0 samples a point even when the
                        // subsequent own-ship/bypass test ignores that point.
                        auto point =
                            defenseOrbitPoint(destination, float(combatRandom_.next() % 360));
                        if (p.targetShip == 0) {
                            point.y -= 110.f;
                        }
                        if (!p.bombBypass && p.targetShip != p.owner) {
                            p.bombSuperShield = true;
                            p.position = point;
                        }
                    }
                }
                p.previous = p.position;
            } else {
                const float duration = catalog_.animation(p.projectileImage).duration;
                p.bombAnimationElapsed = std::min(duration, p.bombAnimationElapsed + Time::step);
                if (p.bombAnimationElapsed >= duration) {
                    p.bombDelay -= Time::step;
                }
                if (p.bombDelay < 0) {
                    if (!p.missed) {
                        impact(p);
                    } else {
                        p.resolved = true;
                        combatSound({p.soundsInitialized ? p.hitShipSound
                                     : blueprint.hitShipSounds.empty()
                                         ? std::string{}
                                         : blueprint.hitShipSounds.front()},
                                    p.id);
                    }
                    p.age = 0;
                }
            }
            continue;
        }
        const float amount = p.speed * Time::step;
        if (p.space == p.owner) {
            p.position.x += p.direction.x * amount;
            p.position.y += p.direction.y * amount;
            // Carrier -1 permits a retaliatory shard to collide with its origin
            // ship on an outside-to-inside crossing (0x4505a0/0x498830).
            if (p.crystalShard && !p.checkedShield) {
                const auto& origin = p.owner == 0 ? ship_ : enemy_.ship;
                const int bubbles = p.owner == 0 ? shieldBubbles_ : enemy_.shields;
                const int super = p.owner == 0 ? superShields_ : enemy_.superShields;
                if ((bubbles > 0 || super > 0) && insideShield(origin, p.position, p.owner == 1) &&
                    !insideShield(origin, p.previous, p.owner == 1)) {
                    const int destination = p.targetShip;
                    p.targetShip = p.owner;
                    impact(p, true);
                    p.targetShip = destination;
                    if (p.resolved) {
                        p.age = 0;
                        continue;
                    }
                }
            }
        } else {
            const float dx = p.target.x - p.position.x;
            const float dy = p.target.y - p.position.y;
            const float distance = std::hypot(dx, dy);
            const float fraction = distance > 0 ? std::min(1.f, amount / distance) : 1.f;
            p.position.x += dx * fraction;
            p.position.y += dy * fraction;
            const auto& destination = p.targetShip == 1 ? enemy_.ship : ship_;
            const int shields = p.targetShip == 1 ? enemy_.shields : shieldBubbles_;
            const int superShields = p.targetShip == 1 ? enemy_.superShields : superShields_;
            if (!p.checkedShield && (shields > 0 || superShields > 0) &&
                insideShield(destination, p.position, p.targetShip == 1) &&
                !insideShield(destination, p.previous, p.targetShip == 1)) {
                impact(p, true);
                if (p.resolved) {
                    p.age = 0;
                }
            }
            // Projectile::CollisionCheck4505a0 tests the post-motion distance
            // against one further logic-step displacement, then passes the
            // fixed target point to DamageArea. Waiting for exact arrival
            // delayed ordinary area damage by one logic step.
            if (std::hypot(p.target.x - p.position.x, p.target.y - p.position.y) <= amount &&
                !p.resolved) {
                impact(p);
                p.age = 0;
            }
        }
        sweepProjectileSpaceContacts(p);
    }
    projectiles_.insert(projectiles_.end(),
                        std::make_move_iterator(pendingRetaliation_.begin()),
                        std::make_move_iterator(pendingRetaliation_.end()));
    pendingRetaliation_.clear();
}

void Run::advanceSpace() {
    // SpaceManager41c940 produces PDS shots or applies the sun/pulsar pulse
    // before its nebula/drone/projectile passes. New PDS shots move this pass.
    advanceEnvironment();
    if (nebula_) {
        nebulaScene_.advance(
            Time::originalStep,
            [&](int alias, float volume) {
                audio_.sound("lightning" + std::to_string(alias), false, volume);
            },
            &eventRandom_);
    }
    // SpaceManager::OnLoop 0x41c940 updates existing projectiles before draining
    // ShipManager::GetNextShot and SpaceDrone::GetNextProjectile. Shots created
    // by this pass begin moving and colliding on the following space pass.
    const auto firstNewProjectile = nextProjectile_;
    advanceDrones();
    advanceAsteroidEnvironment();
    advanceSpaceProjectiles(firstNewProjectile);
    extractWeaponProjectiles();
    extractEnvironmentProjectiles();
    extractDroneProjectiles();
    advanceAsteroidScroll();
}

void Run::advanceCombatSystem(int owner, std::size_t systemIndex) {
    if ((owner != 0 && owner != 1) || (owner == 1 && !enemy_.present)) {
        return;
    }
    auto& systems = owner == 0 ? systems_ : enemy_.systems;
    if (systemIndex >= systems.size()) {
        return;
    }
    auto& s = systems[systemIndex];
    // ShipSystem::OnLoop614c00 runs before this system's derived callback.
    if (s.lockCount > 0) {
        s.lockElapsed += Time::step;
        if (s.lockElapsed > 5.f) {
            s.lockElapsed = 0;
            --s.lockCount;
            if (s.lockCount == 0) {
                checkSystemRepower(s, owner == 1);
            }
        }
    }
    syncSystemLock(s, owner == 1);
    for (auto& elapsed : s.damagingEffectTimes) {
        elapsed = Time::loopAnimation(elapsed, 1.f);
    }
    if (s.powerDivide != 1) {
        s.powerLimit = s.definition.power / 2;
    }
    // ShipSystem consumes metadata left by the preceding physical-crew pass.
    if (!s.damagedLastFrame && !s.occupied) {
        s.fireDamage = 0;
    }
    if (!s.repairedLastFrame && !s.friendlies) {
        s.repair = 0;
    }
    s.damagedLastFrame = s.repairedLastFrame = false;
    checkSystemMaxPower(s, owner == 1);
    if (s.bonusPower < s.lastBonusPower && s.definition.id != 3 && s.definition.id != 4) {
        checkSystemRepower(s, owner == 1);
    }
    s.lastBonusPower = s.bonusPower;
    s.health = std::clamp(s.health, 0, s.definition.power);
    if (s.definition.id == 0) {
        std::erase_if(resistMessages_, [&](ResistMessage& message) {
            if (message.ship != owner || !message.shieldOwned) {
                return false;
            }
            message.elapsed += Time::step;
            return message.elapsed > 1.f;
        });
        std::erase_if(damageMessages_, [&](DamageMessage& message) {
            if (message.ship != owner || !message.shieldOwned) {
                return false;
            }
            message.elapsed += Time::step;
            return message.elapsed > 1.f;
        });
        std::erase_if(shieldHits_, [&](ShieldHit& hit) {
            if (hit.ship != owner) {
                return false;
            }
            if (hit.currentSize >= 40.f) {
                return true;
            }
            hit.currentSize += 10.f * Time::originalStep;
            hit.currentThickness = std::min(75.f, hit.currentThickness + 60.f * Time::originalStep);
            return false;
        });
        // Shields::OnLoop 0x60e310 keeps lastUserPower even, then sheds one bar
        // through the virtual ForceDecreasePower while the effective power and
        // powerState.first (battery excluded) are both odd and unlocked; the
        // base CheckMaxPower above can leave such an unusable half bubble.
        if (s.restorePower % 2 == 1) {
            --s.restorePower;
        }
        if ((s.power & 1) && ((s.power - s.bonusPower - s.batteryPower) & 1) && !systemLocked(s)) {
            forceDecreaseSystemPower(s, 1, owner == 1);
        }
        auto* enemyShields = &s;
        auto& bubbles = owner == 0 ? shieldBubbles_ : enemy_.shields;
        auto& charge = owner == 0 ? shieldCharge_ : enemy_.shieldCharge;
        auto& excess = owner == 0 ? shieldExcessChargeHack_ : enemy_.shieldExcessChargeHack;
        const auto& shieldShip = owner == 0 ? ship_ : enemy_.ship;
        const int maximum = enemyShields ? enemyShields->power / 2 : 0;
        // The later clamp to the new capacity plays shieldsDown when the layer
        // still held bubbles and the capacity fell to zero.
        if (maximum == 0 && bubbles > 0) {
            combatSound({"shieldsDown"}, tick_);
        }
        bubbles = std::min(bubbles, maximum);
        if (enemyShields && enemyShields->hackActive && enemyShields->hackLevel > 1) {
            // Shields::OnLoop 0x60e310 drains its two-second recharge clock.
            // Native represents that clock on a 0..5 scale, hence 2.5 units/second.
            if (bubbles > 0 || excess) {
                if (charge == 0) {
                    charge = 5.f;
                }
                charge -= 2.5f * Time::step;
                if (charge <= 0) {
                    if (!excess) {
                        bubbles = std::max(0, bubbles - 1);
                    }
                    excess = false;
                    charge = 0;
                }
            } else {
                charge = 0;
            }
        } else if (bubbles < maximum) {
            constexpr std::array<float, 4> shieldSkill{1.f, 1.1f, 1.2f, 1.3f};
            const int activeManning = s.health == s.definition.power ? s.manningLevel : 0;
            const float shieldSkillModifier = shieldSkill[std::size_t(activeManning)];
            float augmentation = 1.f;
            for (const auto& id : shieldShip.augments) {
                if (id == "SHIELD_RECHARGE") {
                    augmentation += catalog_.equipment(id).value;
                }
            }
            charge += 2.5f * Time::step * augmentation * shieldSkillModifier *
                      (1.f + std::max(0, bubbles - 1) / 6.f);
            if (charge > 5.f) {
                if (bubbles == 0) {
                    combatSound({"shieldsUp"}, tick_);
                }
                ++bubbles;
                charge = 0;
            }
        } else {
            charge = 0;
        }
    } else if (s.definition.id == 10 || s.definition.id == 12) {
        const int id = s.definition.id;
        auto& ability = id == 10 ? (owner == 0 ? cloak_ : enemy_.cloak)
                                 : (owner == 0 ? battery_ : enemy_.battery);
        // BatterySystem::OnLoop 0x608ed0 lends 2 x effective power whenever the
        // battery is still on after its Functioning and hack checks, including
        // the frame on which its timer runs out; otherwise it clears the lend.
        const int batteryEffective = std::clamp(s.power, 0, s.health);
        const bool batteryLending = id == 12 && ability.active && batteryEffective > 0 &&
                                    (!s.hackActive || s.hackLevel <= 1);
        if (ability.active) {
            const int effective = std::clamp(s.power, 0, s.health);
            const float duration = id == 10 ? 5.f * effective : 30.f;
            if (ability.duration > 0 && ability.duration != duration) {
                ability.elapsed = ability.elapsed / ability.duration * duration;
            }
            ability.duration = duration;
            ability.elapsed += Time::step;
            if (effective == 0 || (s.hackActive && s.hackLevel > 1) ||
                ability.elapsed > ability.duration) {
                stopAbility(id, owner == 1);
            }
        }
        if (id == 12) {
            batteryCapacity_[std::size_t(owner == 1)] = batteryLending ? 2 * batteryEffective : 0;
        }
        // Derived OnLoop plays the retained queue after activation/cancellation
        // and timer processing; direct Battery start is the separate immediate sound.
        if (!ability.soundEffect.empty()) {
            combatSound({ability.soundEffect}, tick_);
            ability.soundEffect.clear();
        }
    } else if (s.definition.id == 3) {
        auto& weapons = owner == 0 ? combatWeapons_ : enemy_.weapons;
        const auto& ship = owner == 0 ? ship_ : enemy_.ship;
        float cooldownAugment = 0;
        for (const auto& augmentation : ship.augments) {
            if (augmentation == "AUTO_COOLDOWN") {
                cooldownAugment += catalog_.equipment(augmentation).value;
            }
        }
        auto& powered = owner == 0 ? weaponPower_ : enemy_.powered;
        int& ammunition = owner == 0 ? missiles_ : enemy_.missiles;
        // WeaponSystem::OnLoop 0x6110f0 clamps at the beginning of the
        // update; concurrent queued volleys may overspend until the next step.
        ammunition = std::max(0, ammunition);
        const int manningLevel = s.health == s.definition.power ? s.manningLevel : 0;
        constexpr std::array<float, 4> skillCooldown{1.f, .9f, .85f, .8f};
        for (std::size_t index = 0; index < weapons.size(); ++index) {
            auto& w = weapons[index];
            const auto& e = catalog_.equipment(ship.weapons[index]);
            // UpdateBonusPower610320 schedules one retry when a powered
            // factory loses bonus credit. OnLoop consumes the bit even when
            // locked or the attempted activation fails.
            if (w.repowerPending && !powered[index] && !systemLocked(s)) {
                (void)powerWeaponSlot(s, index, owner == 1, false);
            }
            w.repowerPending = false;
            int spendMissiles = 0;
            const auto finishFactory = [&] {
                advanceWeaponAnimation(w, e, powered[index], s.hackActive ? s.hackLevel : 0);
                // WeaponSystem6110f0 tests current ammunition AFTER factory
                // Update and BEFORE SpendMissiles. Its final shot stays powered
                // until the following pass, while queued objects keep it alive.
                if (e.missiles != 0 && ammunition < 1 && w.pendingProjectiles.empty() &&
                    powered[index]) {
                    int cost = std::max(0, e.power - w.bonusPower);
                    while (cost > 0 && s.power > s.bonusPower) {
                        releaseBatteryPower(s, 1, owner == 1);
                        --s.power;
                        --cost;
                    }
                    if (cost == 0) {
                        powered[index] = false;
                    }
                }
                if (spendMissiles > 0) {
                    bool saved = false;
                    if (std::ranges::find(ship.augments, std::string("EXPLOSIVE_REPLICATOR")) !=
                        ship.augments.end()) {
                        saved = (combatRandom_.next() & 1U) == 0;
                    }
                    if (saved && owner == 0) {
                        pendingSavedMissileFeedback_ = true;
                    }
                    if (!saved) {
                        const auto previousResources = resourceAmounts();
                        ammunition -= spendMissiles;
                        if (owner == 0) {
                            progressionRun_.flag("used_missile");
                            noteResourceChanges(previousResources);
                        }
                    }
                }
            };
            const float boostedCooldown = std::max(
                .1f, e.cooldown - (e.boostType == "cooldown" ? w.boostLevel * e.boostAmount : 0.f));
            const float cooldown = boostedCooldown * skillCooldown[std::size_t(manningLevel)];
            if (w.cooldown > 0 && w.cooldown != cooldown) {
                w.charge *= cooldown / w.cooldown;
            }
            w.cooldown = cooldown;
            if (e.chargeLevels <= 1 && w.charge >= cooldown && w.chargeLevel == 0) {
                // Native legacy snapshots and pre-igniter transfers represented
                // an ordinary full charge only through its cooldown fraction.
                w.chargeLevel = 1;
            }
            if (!w.animationInitialized && owner == 0 && w.queued && w.targetRoom >= 0 &&
                w.volleyRemaining == 0) {
                // Legacy target orders predate the separate factory queue.
                w.volleyRoom = w.targetRoom;
                w.volleyTarget = w.target;
                w.volleyEnd = w.targetEnd;
                w.volleySelf = w.targetSelf;
                w.volleyRooms = w.targetRooms;
                w.volleyPoints = w.targetPoints;
                w.volleyIndex = 0;
                w.volleyRemaining = e.chargeLevels > 1 ? std::max(1, w.targetGoal)
                                    : e.type == "BEAM" ? 1
                                                       : std::max(1, e.shots);
                w.targetGoal = w.volleyRemaining;
                w.volleyTimer = 0;
            }
            if (!powered[index]) {
                if (w.charge <= 0) {
                    w.charge = 0;
                    w.boostLevel = 0;
                    w.chargeLevel = 0;
                } else {
                    if (w.charge >= cooldown && w.chargeLevel > 0) {
                        --w.chargeLevel;
                    }
                    w.charge = std::max(0.f, w.charge - 6.f * Time::step);
                }
                w.clearTargets(true);
                w.pendingProjectiles.clear();
                if (owner == 0 && aiming_ == int(index)) {
                    cancelAim();
                }
                finishFactory();
                continue;
            }
            if (w.volleyRemaining > 0 && w.chargeLevel > 0 && ammunition >= e.missiles) {
                const int initialTargets = e.chargeLevels > 1 ? std::max(1, w.targetGoal)
                                           : e.type == "BEAM" ? 1
                                                              : std::max(1, e.shots);
                if (w.volleyRemaining == initialTargets && e.boostAmount > 0 && e.boostCount > 0) {
                    w.boostLevel = std::min(w.boostLevel + 1,
                                            e.boostCount + (e.boostType == "damage" ? 1 : 0));
                }
                if (!w.volleyPoints.empty()) {
                    w.volleyRoom = w.volleyRooms.at(std::size_t(w.volleyIndex));
                    w.volleyTarget = w.volleyPoints.at(std::size_t(w.volleyIndex));
                    ++w.volleyIndex;
                }
                fireProjectile(owner, static_cast<int>(index));
                if (e.missiles > 0 && w.volleyRemaining == 1) {
                    spendMissiles = e.missiles;
                }
                --w.volleyRemaining;
                if (w.volleyRemaining == 0) {
                    if (owner == 0) {
                        for (bool physicalEnemy : {false, true}) {
                            const auto* actor = crewOperator(3, physicalEnemy);
                            auto operatorCrew = std::ranges::find_if(crew_, [&](const auto& owned) {
                                return &owned == actor;
                            });
                            if (operatorCrew == crew_.end()) {
                                continue;
                            }
                            // FireNextShot 0x40c2c0 posts event 9 once
                            // when the volley ends. CheckSkills 0x46e5f0
                            // awards it to the active weapons operator.
                            const int previousTier = operatorCrew->weaponSkill;
                            if (operatorCrew->weaponSkillProgress < weaponSkillCap(*operatorCrew)) {
                                operatorCrew->skillUpRemaining[3] = 1.f;
                            }
                            operatorCrew->weaponSkillProgress =
                                std::min(weaponSkillCap(*operatorCrew),
                                         operatorCrew->weaponSkillProgress + 1);
                            operatorCrew->weaponSkill = weaponSkillTier(*operatorCrew);
                            recordEarnedSkill(operatorCrew->earnedSkillTiers,
                                              3,
                                              previousTier,
                                              operatorCrew->weaponSkill);
                            if (operatorCrew->weaponSkill > previousTier) {
                                audio_.sound("levelup");
                            }
                        }
                    }
                    w.charge = 0;
                    w.chargeLevel = 0;
                    w.queued = false;
                    if (w.autofire) {
                        // ProjectileFactory::Update 0x40e669 refills from lastTargets,
                        // which Fire 0x40cb80 stored reversed, and trims a
                        // multi-charge weapon to its first entry: the last pick.
                        if (e.chargeLevels > 1 && w.targetPoints.size() > 1) {
                            const auto discard = std::ptrdiff_t(w.targetPoints.size() - 1);
                            w.targetPoints.erase(w.targetPoints.begin(),
                                                 w.targetPoints.begin() + discard);
                            w.targetRooms.erase(w.targetRooms.begin(),
                                                w.targetRooms.begin() + discard);
                        }
                        w.volleyPoints = w.targetPoints;
                        w.volleyRooms = w.targetRooms;
                        w.volleyIndex = 0;
                        w.volleyRemaining = e.chargeLevels > 1
                                                ? std::max(1, int(w.volleyPoints.size()))
                                            : e.type == "BEAM" ? 1
                                                               : std::max(1, e.shots);
                        w.targetGoal = w.volleyRemaining;
                        w.emittedVolley = false;
                    } else {
                        w.targetRoom = -1;
                        w.targetRooms.clear();
                        w.targetPoints.clear();
                        w.targetGoal = 0;
                    }
                }
            }
            const auto* weaponSystem = findSystem(owner == 0 ? systems_ : enemy_.systems, 3);
            if (weaponSystem && weaponSystem->hackActive && weaponSystem->hackLevel > 1) {
                // ProjectileFactory::Update 0x40cd5d drains one game-time
                // second per second while the hacking pulse is active.
                if (w.charge >= cooldown && w.chargeLevel > 0) {
                    --w.chargeLevel;
                }
                w.charge = std::max(0.f, w.charge - Time::step);
                finishFactory();
                continue;
            }
            // ProjectileFactory::Update 0x40cfab freezes charge against a
            // cloaked target. Already launched projectiles remain independent.
            if ((w.targetSelf || enemy_.present) && cloaked(w.targetSelf ? owner : 1 - owner)) {
                finishFactory();
                continue;
            }
            const float previousCharge = w.charge;
            w.charge = std::min(cooldown, w.charge + (1.f + cooldownAugment) * Time::step);
            if (w.charge >= cooldown && previousCharge < cooldown) {
                ++w.chargeLevel;
                if (w.chargeLevel < e.chargeLevels) {
                    w.charge = 0;
                }
            }
            finishFactory();
        }
        // WeaponSystem6110f0's final reverse pass compares reactor RAW plus
        // controller bonus (excluding battery), after all factory updates.
        for (std::size_t index = powered.size();
             index-- > 0 && s.power - s.batteryPower > systemCapacity(s);) {
            if (!powered[index]) {
                continue;
            }
            int cost = std::max(
                0, catalog_.equipment(ship.weapons[index]).power - weapons[index].bonusPower);
            while (cost > 0 && s.power > s.bonusPower) {
                releaseBatteryPower(s, 1, owner == 1);
                --s.power;
                --cost;
            }
            if (cost == 0) {
                powered[index] = false;
            }
        }
    } else if (s.definition.id == 11) {
        const auto& ship = owner == 0 ? ship_ : enemy_.ship;
        const auto& targetShip = owner == 0 ? enemy_.ship : ship_;
        float augmentation = 0;
        for (const auto& id : ship.augments) {
            if (id == "AUTO_COOLDOWN") {
                augmentation += catalog_.equipment(id).value;
            }
        }
        int index = 0;
        for (std::size_t prior = 0; prior < systemIndex; ++prior) {
            index += systems[prior].definition.id == 11;
        }
        for (auto* systemPointer : {&s}) {
            auto& system = *systemPointer;
            auto& weapon = system.artillery;
            const auto& blueprint = catalog_.equipment(system.definition.weapon);
            const int power = std::clamp(system.power, 0, system.health);
            const float cooldown = std::max(.1f, blueprint.cooldown * (1.5f - power * .25f));
            if (weapon.cooldown > 0 && weapon.cooldown != cooldown) {
                weapon.charge *= cooldown / weapon.cooldown;
            }
            weapon.cooldown = cooldown;
            if (power <= 0) {
                if (weapon.charge <= 0) {
                    weapon.chargeLevel = weapon.boostLevel = 0;
                } else if (weapon.charge >= cooldown && weapon.chargeLevel > 0) {
                    --weapon.chargeLevel;
                }
                weapon.charge = std::max(0.f, weapon.charge - 6.f * Time::step);
                weapon.clearTargets(true);
                weapon.pendingProjectiles.clear();
            } else {
                if (weapon.volleyRemaining > 0 && weapon.chargeLevel > 0 &&
                    blueprint.missiles == 0) {
                    if (blueprint.type != "BURST" && !weapon.volleyPoints.empty()) {
                        weapon.volleyRoom = weapon.volleyRooms.at(std::size_t(weapon.volleyIndex));
                        weapon.volleyTarget =
                            weapon.volleyPoints.at(std::size_t(weapon.volleyIndex++));
                    }
                    fireProjectile(owner, index, true);
                    --weapon.volleyRemaining;
                    if (weapon.volleyRemaining == 0) {
                        weapon.charge = 0;
                        weapon.chargeLevel = 0;
                        weapon.queued = false;
                        weapon.volleyPoints.clear();
                        weapon.volleyRooms.clear();
                        weapon.volleyIndex = 0;
                    }
                }
                if (weapon.factoryHackLevel > 1) {
                    if (weapon.charge >= cooldown && weapon.chargeLevel > 0) {
                        --weapon.chargeLevel;
                    }
                    weapon.charge = std::max(0.f, weapon.charge - Time::step);
                } else if (!enemy_.present || !cloaked(1 - owner)) {
                    const float previous = weapon.charge;
                    weapon.charge =
                        std::min(cooldown, weapon.charge + (1.f + augmentation) * Time::step);
                    if (weapon.charge >= cooldown && previous < cooldown) {
                        ++weapon.chargeLevel;
                        if (weapon.chargeLevel < blueprint.chargeLevels) {
                            weapon.charge = 0;
                        }
                    }
                }
            }
            advanceWeaponAnimation(weapon, blueprint, power > 0, weapon.factoryHackLevel);
            weapon.factoryHackLevel = system.hackActive ? system.hackLevel : 0;
            if (power <= 0 || !hostile_ || cloaked(1 - owner) || weapon.chargeLevel <= 0 ||
                blueprint.missiles > 0 || weapon.volleyRemaining > 0 || targetShip.rooms.empty()) {
                ++index;
                continue;
            }
            weapon.volleyPoints.clear();
            weapon.volleyRooms.clear();
            weapon.volleyIndex = 0;
            weapon.emittedVolley = false;
            if (blueprint.type == "BEAM") {
                const auto& start =
                    targetShip.rooms[combatRandom_.next() % targetShip.rooms.size()];
                weapon.volleyRoom = start.id;
                weapon.volleyTarget = center(start);
                const auto& farthest =
                    *std::max_element(targetShip.rooms.begin(),
                                      targetShip.rooms.end(),
                                      [&](const auto& a, const auto& b) {
                                          const auto distance = [&](const auto& room) {
                                              const auto point = center(room);
                                              return std::hypot(point.x - weapon.volleyTarget.x,
                                                                point.y - weapon.volleyTarget.y);
                                          };
                                          return distance(a) < distance(b);
                                      });
                weapon.volleyEnd = center(farthest);
            } else if (blueprint.type == "BURST") {
                for (const auto& mini : blueprint.miniProjectiles) {
                    for (int shard = 0; shard < mini.count; ++shard) {
                        const auto& room =
                            targetShip.rooms[combatRandom_.next() % targetShip.rooms.size()];
                        weapon.volleyPoints.push_back(center(room));
                    }
                }
                if (!weapon.volleyPoints.empty()) {
                    weapon.volleyTarget = weapon.volleyPoints.front();
                }
                weapon.volleyRoom = -1;
            } else {
                const int count = blueprint.chargeLevels > 1 ? std::max(1, weapon.chargeLevel)
                                                             : std::max(1, blueprint.shots);
                while (int(weapon.volleyRooms.size()) < count) {
                    const auto& room =
                        targetShip.rooms[combatRandom_.next() % targetShip.rooms.size()];
                    const auto duplicate = std::ranges::find(weapon.volleyRooms, room.id);
                    if (duplicate != weapon.volleyRooms.end()) {
                        const auto at = duplicate - weapon.volleyRooms.begin();
                        weapon.volleyRooms.erase(duplicate);
                        weapon.volleyPoints.erase(weapon.volleyPoints.begin() + at);
                    }
                    weapon.volleyRooms.push_back(room.id);
                    weapon.volleyPoints.push_back(center(room));
                }
                weapon.volleyRoom = weapon.volleyRooms.front();
                weapon.volleyTarget = weapon.volleyPoints.front();
            }
            if (blueprint.type != "BEAM") {
                weapon.volleyEnd = weapon.volleyTarget;
            }
            weapon.targetGoal = blueprint.chargeLevels > 1 ? std::max(1, weapon.chargeLevel)
                                                           : std::max(1, blueprint.shots);
            weapon.volleyRemaining =
                blueprint.type == "BEAM" || blueprint.type == "BURST" ? 1 : weapon.targetGoal;
            weapon.volleyTimer = 0;
            weapon.queued = true;
            weapon.entryAngle = (combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
            ++index;
        }
    }
}

void Run::advanceCombatSystemState(int owner, std::size_t systemIndex, bool jumping) {
    if ((owner != 0 && owner != 1) || (owner == 1 && !enemy_.present)) {
        return;
    }
    auto& systems = owner == 0 ? systems_ : enemy_.systems;
    if (systemIndex >= systems.size()) {
        return;
    }
    auto& s = systems[systemIndex];
    // ShipManager4a05b0 consumes GetExploded even when jumping skips OnLoop.
    if (s.exploded) {
        s.exploded = false;
        // DamageHull(1, false) 0x490820 returns without damage while bJumping.
        if (!jumping) {
            auto& hull = owner == 0 ? hull_ : enemy_.hull;
            const int previous = hull;
            hull = std::max(0, hull - 1);
            lastHullDamage_[std::size_t(owner)] = previous - hull;
        }
        const SDL_FPoint roomCenter =
            center(findRoom(owner == 0 ? ship_ : enemy_.ship, s.definition.room));
        pushCombatMessage(DamageMessage{owner, 1, roomCenter, tick_, false, false});
        // The same branch appends an explosion1 Animation at the room centre
        // offset by (-16, -16) (integer Point) and plays sysExplosion.
        systemExplosions_[std::size_t(owner)].push_back(
            {"explosion1",
             {float(int(roomCenter.x - 16.f)), float(int(roomCenter.y - 16.f))},
             0,
             1});
        combatSound({"sysExplosion"}, tick_);
    }
}

void Run::advanceCombatShipState(int owner) {
    // Destroyed ShipManager::OnLoop4a2830 delegates only to Ship::OnLoop.
    // Its manager messages and derived Shields callback remain frozen.
    const bool destroyed = owner == 0 ? playerExplosionStarted_ : enemyExplosionActive_;
    if (!destroyed) {
        std::erase_if(resistMessages_, [&](ResistMessage& message) {
            if (message.ship != owner || message.shieldOwned) {
                return false;
            }
            message.elapsed += Time::step;
            return message.elapsed > 1.f;
        });
        // ShipManager::OnLoop updates its explosions after the damage messages
        // and erases each Animation once Done.
        std::erase_if(systemExplosions_[std::size_t(owner)], [&](CombatExplosionEffect& effect) {
            effect.elapsed += Time::step;
            return effect.elapsed >= catalog_.animation(effect.animation).duration;
        });
        // The original's DamageMessage tracker owns its own lifetime; native uses the
        // same one-second window as the resist indicator it already models.
        std::erase_if(damageMessages_, [&](DamageMessage& message) {
            if (message.ship != owner || message.shieldOwned) {
                return false;
            }
            message.elapsed += Time::step;
            return message.elapsed > 1.f;
        });
    }
    if (owner == 0 && playerExplosionStarted_) {
        const float elapsed = float(Time::seconds(double(tick_ - playerExplosionStartTick_)));
        if (elapsed <= 6.f + Time::step) {
            advanceExplosion(playerExplosionState_, ship_, elapsed, Time::step);
        }
    }
    if (owner == 1 && enemyExplosionActive_) {
        explosion_ -= Time::step;
        const float age = explosionDuration_ - explosion_;
        if (explosionDuration_ == 4.f && age - Time::step < 3.f && age >= 3.f) {
            combatSound({"jumpLeave"}, tick_);
        }
        if (age <= explosionDuration_) {
            advanceExplosion(enemy_.explosionState, enemy_.ship, age, Time::step);
        }
    }
    if (owner == 1 && enemy_.escapeJumpElapsed >= 0) {
        enemy_.escapeJumpElapsed += Time::step;
    }
}

void Run::advanceCombatShipLocks(int owner) {
    // Ship::OnLoop48a4b0 updates lockdown after engines, doors and Room clocks.
    auto& rooms = owner == 0 ? lockdownRooms_ : enemy_.lockdownRooms;
    for (auto it = rooms.begin(); it != rooms.end();) {
        it->second -= Time::step;
        it = it->second <= 0 ? rooms.erase(it) : std::next(it);
    }
}

// ShipManager::iIntruderCount (UpdateCrewmembers 0x49ee10): physically present
// actors for which GetIntruder and Functional hold, including mind-controlled
// residents and powered boarding drones.
int Run::shipIntruderCount(int ship) const {
    int count = 0;
    const auto alive = [](const RunCrew& member) {
        return !member.dead && !member.outOfGame;
    };
    for (const auto& member : crew_) {
        const bool aboard =
            ship == 1 ? member.intruder && member.boardPositionValid : !member.intruder;
        if (aboard && alive(member) && member.controlled == (ship == 0)) {
            ++count;
        }
    }
    if (ship == 0) {
        count += int(std::ranges::count_if(voyageBoarders_, [&](const RunCrew& member) {
            return alive(member) && !member.controlled;
        }));
    } else if (enemy_.present) {
        count += int(std::ranges::count_if(enemy_.crew, [&](const RunCrew& member) {
            return alive(member) && !member.intruder && member.controlled;
        }));
    }
    for (const int owner : {0, 1}) {
        if (owner == ship) {
            continue;
        }
        for (const auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == ship && !drone->crewDead && drone->deployed &&
                drone->powered) {
                ++count;
            }
        }
    }
    return count;
}

void Run::advanceCombatAi(int owner) {
    if (owner == 0) {
        // ShipAI6012e0's player path still reaches UpdateMindControl after
        // CrewAI, including the strongly hacked player system's auto-cast.
        advanceAdvancedOwner(0, 14, true);
        return;
    }
    if (owner != 1 || !enemy_.present) {
        return;
    }
    if (!hostile_ || enemy_.hull <= 0) {
        // DeactiveWeapons605f40 force-depowers drones, weapons and every
        // artillery pod; it preserves controller bonus and user intent.
        for (int id : {4, 3, 11}) {
            for (auto& station : enemy_.systems) {
                if (station.definition.id == id) {
                    forceDecreaseSystemPower(station, station.power + 1, true);
                }
            }
        }
        if (findSystem(enemy_.systems, 0)) {
            enemy_.superShields = 0;
        }
        enemy_.teleportCharge = 100.f;
        if (findSystem(enemy_.systems, 15)) {
            // HackingSystem::StopHacking608230 clears the target's base hack
            // and silently starts its retained carrier explosion.
            if (auto* target = system(enemy_.hackingTarget)) {
                target->hackActive = false;
                target->hackLevel = 0;
            }
            enemy_.hackingTarget = -1;
            enemy_.hacking.active = false;
            blowUpDrone(enemy_.hackingDrone, true);
        }
        advanceAdvancedOwner(1, 14, true);
        return;
    }
    // WorldManager latches SetStalemate before the enemy CompleteShip pass.
    // ShipAI6012e0 processes that flag before the battery and power rebuild.
    if (enemy_.stalemateTrigger) {
        enemy_.escapeThreshold = 300;
        enemy_.stalemateTimer += Time::step;
        if (!enemy_.escaping) {
            enemy_.escaping = true;
            enemy_.escapeTimer = std::max(90.f, enemy_.escapeTimer);
        }
        if (enemy_.hull != enemy_.lastEscapeHealth) {
            enemy_.stalemateTimer = std::min(enemy_.stalemateTimer, 60.f);
        }
        if (campaign_.fighting && enemy_.escapeJumpElapsed < 0 && enemy_.stalemateTimer >= 120.f) {
            enemy_.escapeJumpElapsed = 0;
            beginEnemyJump();
            combatSound({"jumpLeave"}, tick_);
        }
    }
    // ShipAI6012e0 activates the battery before rebuilding system power.
    activateAbility(12, true);
    {
        auto* weapons = findSystem(enemy_.systems, 3);
        if (!hostile_) {
            if (weapons) {
                std::fill(enemy_.powered.begin(), enemy_.powered.end(), false);
                weapons->power = 0;
            }
        } else {
            std::vector<std::size_t> priority(enemy_.powered.size());
            std::vector<bool> selected(priority.size(), false);
            for (auto& next : priority) {
                int bestCharge = -1;
                for (std::size_t i = 0; i < selected.size(); ++i) {
                    if (selected[i]) {
                        continue;
                    }
                    const auto& weapon = enemy_.weapons[i];
                    // CheckPowerLevels 0x601080 prefers a factory whose
                    // WeaponAnimation::bFiring strip is still running.
                    if (weapon.animationFiring) {
                        next = i;
                        break;
                    }
                    // CheckPowerLevels 0x601080 stores the chosen floating
                    // cooldown in an integer before comparing the next slot.
                    if (weapon.charge > float(bestCharge)) {
                        bestCharge = int(weapon.charge);
                        next = i;
                    }
                }
                selected[next] = true;
            }
            if (enemy_.powerFixtureOverride) {
                // A deterministic snapshot with explicit system power holds
                // that setup while testing weapons and projectile effects.
                int available = reactorCapacity(true) + batteryPower(true);
                for (const auto& s : enemy_.systems) {
                    const int id = s.definition.id;
                    if (id != 3 && (id < 6 || id > 8) && id != 12) {
                        available -= std::max(0, s.power - s.bonusPower);
                    }
                }
                if (weapons && !systemLocked(*weapons)) {
                    const int capacity = std::min(systemCapacity(*weapons),
                                                  std::max(0, available) + weapons->bonusPower);
                    int remaining = std::max(0, capacity - weapons->bonusPower);
                    const int initial = remaining;
                    releaseBatteryPower(*weapons, weapons->batteryPower, true);
                    weapons->power = weapons->bonusPower;
                    std::fill(enemy_.powered.begin(), enemy_.powered.end(), false);
                    for (const auto i : priority) {
                        const auto& e = catalog_.equipment(enemy_.ship.weapons[i]);
                        const int cost = std::max(0, e.power - enemy_.weapons[i].bonusPower);
                        enemy_.powered[i] =
                            remaining >= cost && (e.missiles == 0 || enemy_.missiles > 0 ||
                                                  !enemy_.weapons[i].pendingProjectiles.empty());
                        if (enemy_.powered[i]) {
                            remaining -= cost;
                        }
                    }
                    weapons->power += initial - remaining;
                    assignBatteryPower(*weapons, initial - remaining, true);
                }
            } else {
                // ShutdownAllPower4955e0 visits the physical system vector,
                // repeatedly invoking unlocked non-user DecreasePower. Full
                // controller bonus remains present after every slot is off.
                for (auto& station : enemy_.systems) {
                    const int id = station.definition.id;
                    if (systemLocked(station) || (id >= 6 && id <= 8) || id == 12) {
                        continue;
                    }
                    while (station.power > station.bonusPower ||
                           (id == 3 && std::ranges::any_of(enemy_.powered,
                                                           [](bool on) {
                                                               return on;
                                                           })) ||
                           (id == 4 && std::ranges::any_of(enemy_.drones, [](const auto& drone) {
                                return drone.powered;
                            }))) {
                        const int previous = station.power;
                        const auto previousWeapons = enemy_.powered;
                        const int previousDrones = static_cast<int>(
                            std::ranges::count_if(enemy_.drones, [](const auto& drone) {
                                return drone.powered;
                            }));
                        forceDecreaseSystemPower(station, 1, true);
                        if (station.power == previous && previousWeapons == enemy_.powered &&
                            previousDrones ==
                                std::ranges::count_if(enemy_.drones, [](const auto& drone) {
                                    return drone.powered;
                                })) {
                            break;
                        }
                    }
                }
                const auto increase = [&](int id) {
                    auto* station = findSystem(enemy_.systems, id);
                    return station && !systemLocked(*station) &&
                           forceIncreaseSystemPower(*station, true);
                };
                const auto powerWeapon = [&] {
                    if (!weapons || systemLocked(*weapons)) {
                        return;
                    }
                    int bestCharge = -1;
                    std::size_t selected = enemy_.weapons.size();
                    for (std::size_t index = 0; index < enemy_.weapons.size(); ++index) {
                        if (enemy_.powered[index]) {
                            continue;
                        }
                        const auto& factory = enemy_.weapons[index];
                        // CheckPowerLevels 0x601080 prefers a factory whose
                        // WeaponAnimation::bFiring strip is still running.
                        if (factory.animationFiring) {
                            selected = index;
                            break;
                        }
                        if (factory.charge > float(bestCharge)) {
                            bestCharge = int(factory.charge);
                            selected = index;
                        }
                    }
                    if (selected == enemy_.weapons.size()) {
                        return;
                    }
                    const auto& blueprint = catalog_.equipment(enemy_.ship.weapons[selected]);
                    const auto& factory = enemy_.weapons[selected];
                    if (blueprint.missiles != 0 && enemy_.missiles <= 0 &&
                        factory.pendingProjectiles.empty()) {
                        return;
                    }
                    int available = reactorCapacity(true) + batteryPower(true);
                    for (const auto& allocated : enemy_.systems) {
                        const int id = allocated.definition.id;
                        if ((id < 6 || id > 8) && id != 12) {
                            available -= allocated.power - allocated.bonusPower;
                        }
                    }
                    const int required = std::max(0, blueprint.power - factory.bonusPower);
                    if (required <= available &&
                        weapons->power + required <= systemCapacity(*weapons)) {
                        weapons->power += required;
                        assignBatteryPower(*weapons, required, true);
                        enemy_.powered[selected] = true;
                    }
                };
                increase(2);
                increase(9);
                increase(13);
                if (enemy_.crewAi.hurtCrew) {
                    increase(5);
                }
                bool gained = false;
                do {
                    bool drone = false;
                    if (!enemy_.escaping) {
                        powerWeapon();
                        drone = increase(4);
                    }
                    const bool shield = increase(0);
                    const bool cloak = increase(10);
                    const bool engine = increase(1);
                    gained = drone || shield || cloak || engine;
                } while (gained);
                for (auto& station : enemy_.systems) {
                    while (!systemLocked(station) && forceIncreaseSystemPower(station, true)) {
                    }
                }
            }
        }
    }
    // CheckPowerLevels601080 invokes drone allocation before CombatAI weapons.
    advanceDroneAi(owner);
    if (hostile_ && (!cloaked(1) || enemy_.fireWhileCloaked)) {
        auto& weapons = enemy_.weapons;
        const auto& ship = enemy_.ship;
        const auto& powered = enemy_.powered;
        const int ammunition = enemy_.missiles;
        for (std::size_t index = 0; index < weapons.size(); ++index) {
            auto& w = weapons[index];
            const auto& e = catalog_.equipment(ship.weapons[index]);
            const auto chooseEnemyAim = [&]() -> const RoomDefinition& {
                // CombatAI::PrioritizeSystem 0x606260: Easy/Normal make a
                // 20/33-percent attempt from the fixed system target list.
                int preferredRoom = -1;
                if (difficulty_ < 2) {
                    if (combatRandom_.next() % 100 < (difficulty_ == 0 ? 20U : 33U)) {
                        constexpr std::array<int, 5> targets{0, 1, 3, 2, 4};
                        const int systemId = targets[combatRandom_.next() % targets.size()];
                        if (const auto* system = findSystem(systems_, systemId)) {
                            preferredRoom = system->definition.room;
                        }
                    }
                } else {
                    // Hard uses active combat conditions to build its key
                    // system list, then rolls key/any/random at 25/25/50.
                    std::vector<int> keySystems;
                    for (int id = 0; id <= 15; ++id) {
                        const auto* system = findSystem(systems_, id);
                        if (!system) {
                            continue;
                        }
                        // Functioning 0x6153d0 tests effective power, which can
                        // remain positive during a partial ion lock.
                        const bool active = system->power > 0;
                        bool key = false;
                        switch (id) {
                        case 0:
                            // Functioning calls Shields::Powered 0x60aec0: a
                            // whole bubble (effective power above one).
                            key = system->power > 1;
                            break;
                        case 3:
                        case 4:
                            key = active;
                            break;
                        case 1:
                        case 6:
                            // ShipManager::GetDodgeFactor 0x491e20 excludes the
                            // cloak bonus GetDodged adds separately.
                            key = evasion() - (cloaked(0) ? 60 : 0) > 25;
                            break;
                        case 2:
                            key = oxygen_ <= 49;
                            break;
                        case 8:
                            key = shipIntruderCount(0) > 0 ||
                                  std::ranges::any_of(playerHazards_, [](const auto& room) {
                                      return room.second.fires > 0;
                                  });
                            break;
                        case 9:
                            key = shipIntruderCount(1) > 0;
                            break;
                        case 10:
                            key = !systemLocked(*system) && !cloak_.active;
                            break;
                        case 11:
                            key = system->artillery.charge < 4;
                            break;
                        case 12:
                            key = battery_.active;
                            break;
                        case 13:
                            key = !cloneQueue_.empty() && system->power > 0;
                            break;
                        case 14:
                            key = mind_.active;
                            break;
                        case 15:
                            key = hacking_.active;
                            break;
                        default:
                            break;
                        }
                        if (key) {
                            keySystems.push_back(id);
                        }
                    }
                    const auto roll = combatRandom_.next() % 100;
                    int systemId = -1;
                    if (roll < 25 && !keySystems.empty()) {
                        systemId = keySystems[combatRandom_.next() % keySystems.size()];
                    } else if (roll < 50 && !systems_.empty()) {
                        systemId = systems_[combatRandom_.next() % systems_.size()].definition.id;
                    }
                    if (const auto* system = findSystem(systems_, systemId)) {
                        preferredRoom = system->definition.room;
                    }
                }
                // UpdateWeapons 0x606800 draws a fallback room even after
                // PrioritizeSystem selected a present system.
                const auto& randomRoom = ship_.rooms[combatRandom_.next() % ship_.rooms.size()];
                if (preferredRoom >= 0) {
                    return findRoom(ship_, preferredRoom);
                }
                return randomRoom;
            };
            // UpdateWeapons 0x606800 tests ReadyToFire, IsChargedGoal and the
            // target's cloak before the partial-charge draw.
            bool charged = powered[index] && w.chargeLevel > 0 && ammunition >= e.missiles &&
                           w.chargeLevel >= std::max(1, w.goalChargeLevel) && !cloaked(0);
            if (charged && e.chargeLevels > 1 && w.chargeLevel < e.chargeLevels) {
                charged = combatRandom_.next() % (e.chargeLevels - w.chargeLevel + 1) == 0;
            }
            if (!charged) {
                continue;
            }
            // CombatAI606800 always builds candidates before Fire40cb80 tests
            // whether a partly consumed volley can be replaced.
            std::vector<int> candidateRooms;
            std::vector<SDL_FPoint> candidatePoints;
            const int required = e.chargeLevels > 1 ? std::max(1, w.chargeLevel)
                                 : e.type == "BEAM" ? 2
                                                    : std::max(1, e.shots);
            while (int(candidatePoints.size()) < required) {
                const auto& aimed = chooseEnemyAim();
                const auto duplicate = std::ranges::find(candidateRooms, aimed.id);
                if (duplicate != candidateRooms.end()) {
                    const auto at = duplicate - candidateRooms.begin();
                    candidateRooms.erase(duplicate);
                    candidatePoints.erase(candidatePoints.begin() + at);
                }
                candidateRooms.push_back(aimed.id);
                candidatePoints.push_back(center(aimed));
            }
            const int remaining = e.type == "BEAM" && w.volleyRemaining > 0 ? 2 : w.volleyRemaining;
            if (remaining == 0 || remaining >= required) {
                w.targetRooms = candidateRooms;
                w.targetPoints = candidatePoints;
                w.targetRoom = candidateRooms.front();
                w.target = candidatePoints.front();
                w.targetRoomEnd = e.type == "BEAM" ? candidateRooms.back() : -1;
                w.targetEnd = e.type == "BEAM" ? candidatePoints.back() : w.target;
                w.volleyRoom = w.targetRoom;
                w.volleyTarget = w.target;
                w.volleyEnd = w.targetEnd;
                w.volleySelf = w.targetSelf;
                w.volleyRooms = e.type == "BEAM" ? std::vector<int>{} : candidateRooms;
                w.volleyPoints = e.type == "BEAM" ? std::vector<SDL_FPoint>{} : candidatePoints;
                w.volleyIndex = 0;
                w.emittedVolley = false;
                w.entryAngle = (combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
                w.volleyRemaining = e.type == "BEAM" ? 1 : required;
                w.targetGoal = w.volleyRemaining;
                w.volleyTimer = 0;
                w.queued = true;
            }
            // SelectChargeGoal follows every attempted Fire, including refusal.
            initializeWeaponFactory(w, e, combatRandom_);
        }
    }
    if (hostile_ && enemy_.hull > 0) {
        if (activateAbility(10, true)) {
            enemy_.fireWhileCloaked = (combatRandom_.next() & 1U) == 0;
        }
    } else if (!hostile_) {
        enemy_.superShields = enemy_.maxSuperShields = 0;
    }
    advanceAdvancedOwner(owner, 15, true);
    if (hostile_ && enemy_.hull > 0) {
        const auto* engines = findSystem(enemy_.systems, 1);
        const auto* pilot = findSystem(enemy_.systems, 6);
        // ShipManager::ShipFlying 0x491d70 / ShipSystem::Functioning 0x6153d0:
        // the piloting system also needs bManned (a CanMan actor at the
        // station, or the automated ship's bAutomated manning).
        const bool pilotManned = enemyAutomated() || hasCrewManning(6, true);
        const bool flying = engines && pilot && engines->power > 0 && pilot->power > 0 &&
                            pilotManned && (!engines->hackActive || engines->hackLevel <= 1) &&
                            (!pilot->hackActive || pilot->hackLevel <= 1);
        if (enemy_.escaping && flying) {
            const bool jammer = std::ranges::find(ship_.augments, std::string("FTL_JAMMER")) !=
                                ship_.augments.end();
            enemy_.escapeTimer -= Time::step * (jammer ? .5f : 1.f);
        }
        if (enemy_.escapeJumpElapsed < 0 && enemy_.escaping && enemy_.escapeTimer < 0 && flying) {
            enemy_.escapeJumpElapsed = 0;
            beginEnemyJump();
            combatSound({"jumpLeave"}, tick_);
        }
        enemy_.lastEscapeHealth = enemy_.hull;
    }
    advanceAdvancedOwner(owner, 14, true);
}

void Run::advanceCombatOutcomes() {
    // WorldManager5f6520 still updates the enemy CompleteShip while the player
    // jumps, then skips its outcome/event branches until the player arrives.
    if (jumpTicks_ > 0 || arrivalTicks_ > 0) {
        return;
    }
    if (playerExplosionStarted_) {
        if (checkDefeat()) {
            return;
        }
        const double elapsed = Time::seconds(double(tick_ - playerExplosionStartTick_));
        if (elapsed > 6.0) {
            checkDefeat();
        }
        return;
    }
    if (enemyExplosionActive_) {
        const float age = explosionDuration_ - explosion_;
        // AnimationTracker::Update 0x59ade0 marks done only when elapsed is
        // strictly greater than its duration, so an exact-duration sample is
        // still a destruction animation frame.
        if (age > explosionDuration_) {
            enemyExplosionActive_ = false;
            explosion_ = 0.f;
            // WorldManager::OnLoop / ShipAI::Destroyed 0x600da0 kills occupants at DestroyedDone.
            for (auto& member : crew_) {
                if (member.intruder && !member.outOfGame) {
                    killCrew(member, false);
                }
            }
            if (!campaign_.fighting) {
                for (auto& member : enemy_.crew) {
                    if (!member.intruder && !member.outOfGame) {
                        killCrew(member, false);
                    }
                }
            }
            if (!checkDefeat()) {
                combatResult(false);
            }
        }
        return;
    }
    if (enemy_.present && hostile_ && enemy_.hull > 0 && enemy_.escapeJumpElapsed >= 0) {
        if (enemy_.escapeJumpElapsed > 2.f) {
            // ShipAI::Escaped0x600d40 and WorldManager0x5f6520: leaving
            // selects gotaway rather than a destroyed/deadCrew reward. OnLoop
            // builds it (GetBaseEvent 0x5bb240, after its reseed) before
            // KillEveryone 0x4a0210 destroys the physical occupants without
            // clone eligibility, and applies it (UpdateLocation) afterwards.
            pugi::xml_node gotaway;
            if (!campaign_.fighting) {
                reseedShipOutcome();
                if (const auto outcome = events_.shipEvent(encounter_).child("gotaway")) {
                    selectedVoyageCrewSeed_ = -1;
                    gotaway = createShipOutcomeEvent(outcome);
                }
            }
            for (std::size_t i = 0; i < crew_.size(); ++i) {
                auto& member = crew_[i];
                if (member.intruder && !member.outOfGame) {
                    killCrew(member, true);
                }
            }
            for (auto& member : enemy_.crew) {
                if (!campaign_.fighting && !member.intruder && !member.outOfGame) {
                    killCrew(member, true);
                }
            }
            enemy_.cloneQueue.clear();
            if (campaign_.fighting) {
                finishBossStage();
            } else {
                addScore(scoreShips_, 1);
                // SetTarget(nullptr) clears every player drone's bDisrupted.
                for (auto& drone : combatDrones_) {
                    drone.defenseDisrupted = false;
                }
                enemy_.playerTarget = false;
                stopCombatHostilities();
                enemy_.present = false;
                if (gotaway) {
                    enterEvent(gotaway);
                    openingEvent_ = false;
                }
            }
            refreshCrewNames();
            checkDefeat();
            return;
        }
    }
    advanceBoss();
    if (checkDefeat() || playerExplosionStarted_) {
        return;
    }
    const auto startEnemyExplosion = [&] {
        if (!enemyExplosionActive_) {
            releaseEnemyMind();
            enemyExplosionActive_ = true;
            // Boss stage1/2 is invincible and SetDestroyed(true,true) uses
            // four-second jump-out destruction; stage3 uses the six-second
            // boss_3 initialization. The stage outcome waits DestroyedDone.
            enemy_.explosionState.jumpOut = campaign_.fighting && campaign_.stage < 3;
            explosionDuration_ = enemy_.explosionState.jumpOut   ? 4.f
                                 : enemy_.ship.image == "boss_3" ? 6.f
                                                                 : 3.f;
            explosion_ = explosionDuration_;
            aiming_ = -1;
        }
    };
    if (!enemy_.present) {
        return;
    }
    if (!hostile_) {
        // ShipManager hull damage still destroys a ship that is no longer
        // hostile (fire, breaches, hazards after a surrender is accepted);
        // combatResult then withholds the destroyed reward.
        if (enemy_.hull <= 0) {
            startEnemyExplosion();
        }
        return;
    }
    // WorldManager0x5f6520 tests surrender, stalemate and escape before
    // crew victory. ShipAI latches surrender even without an authored event.
    if (!campaign_.fighting && !surrenderOffered_ && enemy_.hull > 0 &&
        enemy_.hull <= enemy_.surrenderThreshold && enemy_.escapeJumpElapsed < 0) {
        surrenderOffered_ = true;
        if (!enemy_.pinnedRewardFixture) {
            reseedShipOutcome();
            const auto surrender = events_.shipEvent(encounter_).child("surrender");
            if (surrender) {
                selectedVoyageCrewSeed_ = -1;
                enterEvent(createShipOutcomeEvent(surrender));
                surrenderDialog_ = true;
                openingEvent_ = false;
            }
        } else {
            surrenderDialog_ = true;
            event_ = true;
            openingEvent_ = false;
            eventId_ = "P06_SURRENDER";
            eventText_ = texts_.get("text_PIRATE_SURRENDER_TEXT_7", locale_);
            pendingResources_ = {};
            choices_ = {{"Accept their offer.", {}, true},
                        {"We will not accept surrender!", {}, true}};
            resetEventChoiceDelay();
        }
        return;
    }
    if (!campaign_.fighting && enemy_.hull > 0 && enemy_.stalemateTrigger &&
        enemy_.stalemateTimer > 120.f && enemy_.escapeJumpElapsed < 0) {
        enemy_.stalemateTimer = 0;
        selectedVoyageCrewSeed_ = -1;
        generatedEvent_ = "STALEMATE_SURRENDER";
        choiceHistory_.clear();
        enterEvent(events_.event("STALEMATE_SURRENDER"));
        openingEvent_ = false;
        return;
    }
    if (!campaign_.fighting && enemy_.hull > 0 && !enemy_.escaping && !surrenderOffered_ &&
        enemy_.hull <= enemy_.escapeThreshold) {
        enemy_.escaping = true;
        reseedShipOutcome();
        const auto escape = events_.shipEvent(encounter_).child("escape");
        if (escape) {
            selectedVoyageCrewSeed_ = -1;
            enterEvent(createShipOutcomeEvent(escape));
            openingEvent_ = false;
        } else {
            // CancelEscape0x601060 prevents retrying an absent escape event.
            enemy_.escaping = false;
            enemy_.escapeThreshold = -100;
        }
        return;
    }
    if (enemy_.hull <= 0) {
        startEnemyExplosion();
    } else if (!enemyAutomated() && enemy_.cloneQueue.empty() &&
               // CompleteShip::DeadCrew 0x409930: no crew left, unless bAutomated.
               std::none_of(enemy_.crew.begin(),
                            enemy_.crew.end(),
                            [](const auto& c) {
                                return (!c.dead && !c.dying) || c.cloneReady;
                            }) &&
               std::ranges::none_of(voyageBoarders_, [](const auto& member) {
                   return member.enemyCrewOrigin >= 0 &&
                          ((!member.dead && !member.dying) || member.cloneReady);
               })) {
        if (campaign_.fighting) {
            // BossShip::GetSubEvent 0x406200 sets bAutomated and raises the event.
            campaign_.automated = true;
            progressionRun_.flags["enemy_automated"] = 1;
            selectedVoyageCrewSeed_ = -1;
            enterEvent(events_.event("BOSS_AUTOMATED"));
            eventId_ = "BOSS_AUTOMATED";
        } else {
            combatResult(false);
        }
    }
}

void Run::advanceCombat() {
    // Compatibility wrapper until the owner dispatcher replaces the old call.
    for (int owner : {0, 1}) {
        if (owner == 1 && !enemy_.present) {
            continue;
        }
        const auto& systems = owner == 0 ? systems_ : enemy_.systems;
        for (std::size_t index = 0; index < systems.size(); ++index) {
            advanceCombatSystem(owner, index);
            advanceCombatSystemState(owner, index);
        }
        advanceCombatShipState(owner);
        advanceCombatShipLocks(owner);
    }
    advanceHazards();
    advanceSpreadDamage(0);
    if (enemy_.present) {
        advanceSpreadDamage(1);
    }
    repairSystems(systems_, crew_, false);
    repairSystems(enemy_.systems, enemy_.crew, true);
    advanceCombatAi(1);
    advanceCombatOutcomes();
}

bool Run::checkDefeat() {
    // CommandGui::SetBossDead 0x518c30 sets the guard read by CheckGameOver
    // while the final Flagship destruction animation is still running.
    if (campaign_.fighting && campaign_.stage == 3 && enemy_.hull <= 0) {
        playerExplosionStarted_ = false;
        return false;
    }
    const bool deadCrew =
        cloneQueue_.empty() && std::none_of(crew_.begin(), crew_.end(), [](const auto& c) {
            return (!c.dead && !c.dying) || c.cloneReady;
        });
    if (!defeated_ && hull_ <= 0 && !playerExplosionStarted_ && !deadCrew) {
        playerExplosionStarted_ = true;
        playerExplosionStartTick_ = tick_;
        aiming_ = -1;
        return false;
    }
    if (!defeated_ &&
        (deadCrew || (hull_ <= 0 && playerExplosionStarted_ &&
                      Time::seconds(double(tick_ - playerExplosionStartTick_)) > 6.0))) {
        defeated_ = true;
        hostile_ = false;
        aiming_ = -1;
        event_ = true;
        openingEvent_ = false;
        eventId_ = "P06_DEFEAT";
        eventText_ = texts_.get(deadCrew ? "gameover_crew" : "gameover_blowup", locale_);
        pendingResources_ = {};
        choices_ = {{"Return to main menu", {}, true}};
        resetEventChoiceDelay();
    }
    return defeated_;
}

nlohmann::json Run::combatState() const {
    const auto abilityState = [](const CombatAbility& ability) {
        return nlohmann::json{
            {"active", ability.active},
            {"elapsed", ability.elapsed},
            {"duration", ability.duration},
            {"remaining", ability.active ? std::max(0.f, ability.duration - ability.elapsed) : 0.f},
            {"cooldown", ability.cooldown}};
    };
    auto weaponStates = [](const auto& weapons) {
        auto result = nlohmann::json::array();
        for (std::size_t i = 0; i < weapons.size(); ++i) {
            const auto& w = weapons[i];
            const auto points = [](const std::vector<SDL_FPoint>& values) {
                auto rows = nlohmann::json::array();
                for (const auto& point : values) {
                    rows.push_back({point.x, point.y});
                }
                return rows;
            };
            result.push_back({{"id", i},
                              {"fired_once", w.firedOnce},
                              {"charge", w.charge},
                              {"charge_level", w.chargeLevel},
                              {"charge_goal", w.goalChargeLevel},
                              {"boost_level", w.boostLevel},
                              {"cooldown", w.cooldown},
                              {"target_room", w.targetRoom},
                              {"target_self", w.targetSelf},
                              {"target_rooms", w.targetRooms},
                              {"target_points", points(w.targetPoints)},
                              {"volley_points", points(w.volleyPoints)},
                              {"target_goal", w.targetGoal},
                              {"target", {w.target.x, w.target.y}},
                              {"queued", w.queued},
                              {"autofire", w.autofire},
                              {"volley_remaining", w.volleyRemaining},
                              {"volley_timer", w.volleyTimer},
                              {"target_room_end", w.targetRoomEnd},
                              {"target_end", {w.targetEnd.x, w.targetEnd.y}},
                              {"target_first_pending", w.targetFirstPending},
                              {"volley_room", w.volleyRoom},
                              {"volley_target", {w.volleyTarget.x, w.volleyTarget.y}},
                              {"volley_end", {w.volleyEnd.x, w.volleyEnd.y}},
                              {"volley_self", w.volleySelf},
                              {"volley_index", w.volleyIndex},
                              {"volley_rooms", w.volleyRooms},
                              {"entry_angle", w.entryAngle}});
        }
        return result;
    };
    auto artilleryStates = [](const auto& systems) {
        auto result = nlohmann::json::array();
        for (const auto& system : systems) {
            if (system.definition.id == 11) {
                const auto& weapon = system.artillery;
                result.push_back({{"weapon", system.definition.weapon},
                                  {"room", system.definition.room},
                                  {"power", system.power},
                                  {"health", system.health},
                                  {"charge", weapon.charge},
                                  {"cooldown", weapon.cooldown},
                                  {"volley_remaining", weapon.volleyRemaining}});
            }
        }
        return result;
    };
    // Drone targets are stable space-drone ids; report them as the opposing
    // drone list slot plus one (0 for none), as the earlier slot fields did.
    const auto droneSlot = [](const std::vector<CombatDrone>& candidates, std::uint64_t id) {
        for (std::size_t index = 0; id != 0 && index < candidates.size(); ++index) {
            if (candidates[index].spaceActorId == id) {
                return index + 1;
            }
        }
        return std::size_t{0};
    };
    auto projectiles = nlohmann::json::array();
    for (const auto& p : projectiles_) {
        projectiles.push_back(
            {{"id", p.id},
             {"owner", p.owner},
             {"space", p.space},
             {"target_ship", p.targetShip},
             {"target_room", p.room},
             {"target", {p.target.x, p.target.y}},
             {"target_end", {p.targetEnd.x, p.targetEnd.y}},
             {"hit_rooms", p.hitRooms},
             {"weapon", p.weapon},
             {"projectile_image", p.projectileImage},
             {"explosion_image", p.explosionImage},
             {"drone_targetable", p.droneTargetable},
             {"defense_target_id", p.defenseTargetId},
             {"defense_drone_target_id",
              droneSlot(p.owner == 0 ? enemy_.drones : combatDrones_, p.defenseSpaceDroneId)},
             {"defense_space_drone_id", p.defenseSpaceDroneId},
             {"fake", p.fake},
             {"damage_bonus", p.damageBonus},
             {"crystal_shard", p.crystalShard},
             {"defense_hacking_target_id", p.defenseHackingTargetId},
             {"position", {p.position.x, p.position.y}},
             {"spin_angle", p.spinAngle},
             {"flight_frame", p.flightFrame},
             {"previous", {p.previous.x, p.previous.y}},
             {"direction", {p.direction.x, p.direction.y}},
             {"speed", p.speed},
             {"age", p.age},
             {"entry_angle", p.entryAngle},
             {"checked_shield", p.checkedShield},
             {"arrived", p.arrived},
             {"sounds_initialized", p.soundsInitialized},
             {"hit_ship_sound", p.hitShipSound},
             {"hit_shield_sound", p.hitShieldSound},
             {"miss_sound", p.missSound},
             {"bomb_super_shield", p.bombSuperShield},
             {"bomb_bypass", p.bombBypass},
             {"bomb_animation_elapsed", p.bombAnimationElapsed},
             {"bomb_delay", p.bombDelay},
             {"bomb_miss_tick", p.bombMissTick},
             {"missed", p.missed},
             {"resolved", p.resolved},
             {"beam_stepped", p.beamStepped},
             {"beam_last_collision", p.beamLastCollision},
             {"intercepted", p.intercepted}});
    }
    auto shieldHits = nlohmann::json::array();
    // Space-pass internals the drone audit tests read each frame (state only).
    const auto spaceFlight = [](nlohmann::json& row, const CombatDrone& drone) {
        row["shot_ready"] = drone.shotReady;
        row["has_aim_target"] = drone.hasAimTarget;
        row["current_speed"] = drone.currentSpeed;
        row["last_aim_angle"] = drone.lastAimingAngle;
        row["progress_to_destination"] = drone.progressToDestination;
        row["last_destination"] = {drone.lastDestination.x, drone.lastDestination.y};
        row["space_explosion_elapsed"] = drone.spaceExplosionElapsed;
        row["beam_current_target"] = {drone.beamCurrentTarget.x, drone.beamCurrentTarget.y};
        row["beam_final_target"] = {drone.beamFinalTarget.x, drone.beamFinalTarget.y};
        row["beam_speed"] = drone.beamSpeed;
        row["controller_hack_level"] = drone.controllerHackLevel;
    };
    auto drones = nlohmann::json::array();
    for (std::size_t i = 0; i < combatDrones_.size(); ++i) {
        const auto& drone = combatDrones_[i];
        drones.push_back(
            {{"id", i},
             {"blueprint", drone.blueprint},
             {"boss_surge", drone.bossSurge},
             {"deployed", drone.deployed},
             {"powered", drone.powered},
             {"repower_pending", drone.repowerPending},
             {"bonus_power", drone.bonusPower},
             {"space", drone.space},
             {"health", drone.health},
             {"room", drone.room},
             {"target_room", drone.targetRoom},
             {"delivered", drone.delivered},
             {"rebuild_timer", drone.rebuildTimer},
             {"work_progress", drone.workProgress},
             {"crew_kills", drone.crewKills},
             {"crew_path", drone.crewPath},
             {"crew_path_doors", drone.crewPathDoors},
             {"crew_path_index", drone.crewPathIndex},
             {"crew_position", {drone.crewPosition.x, drone.crewPosition.y}},
             {"crew_room", drone.crewRoom},
             {"crew_ship", drone.crewPhysicalShip},
             {"orbit_initialized", drone.orbitInitialized},
             {"orbit_angle", drone.orbitAngle},
             {"pause", drone.pause},
             {"additional_pause", drone.additionalPause},
             {"velocity", {drone.velocity.x, drone.velocity.y}},
             {"destination", {drone.destination.x, drone.destination.y}},
             {"cooldown", drone.cooldown},
             {"lifespan", drone.lifespan},
             {"repair_beam_age", drone.repairBeamAge},
             {"shield_glow", drone.shieldGlow},
             {"aim_angle", drone.aimAngle},
             {"aim_target", {drone.aimTarget.x, drone.aimTarget.y}},
             {"ion_stun", drone.ionStun},
             {"dodge", effectiveDroneDodge(drone)},
             {"position", {drone.position.x, drone.position.y}},
             {"velocity", {drone.velocity.x, drone.velocity.y}},
             {"target_projectile", drone.targetProjectile},
             {"target_drone", droneSlot(enemy_.drones, drone.targetSpaceDroneId)},
             {"space_actor_id", drone.spaceActorId},
             {"miss_elapsed", drone.spaceMissElapsed},
             {"miss_position", {drone.spaceMissPosition.x, drone.spaceMissPosition.y}}});
        spaceFlight(drones.back(), drone);
    }
    auto enemyDrones = nlohmann::json::array();
    for (std::size_t i = 0; i < enemy_.drones.size(); ++i) {
        const auto& drone = enemy_.drones[i];
        enemyDrones.push_back(
            {{"id", i + 1},
             {"blueprint", drone.blueprint},
             {"boss_surge", drone.bossSurge},
             {"lifespan", drone.lifespan},
             {"deployed", drone.deployed},
             {"powered", drone.powered},
             {"repower_pending", drone.repowerPending},
             {"bonus_power", drone.bonusPower},
             {"space", drone.space},
             {"health", drone.health},
             {"room", drone.room},
             {"target_room", drone.targetRoom},
             {"delivered", drone.delivered},
             {"rebuild_timer", drone.rebuildTimer},
             {"work_progress", drone.workProgress},
             {"crew_kills", drone.crewKills},
             {"crew_path", drone.crewPath},
             {"crew_path_doors", drone.crewPathDoors},
             {"crew_path_index", drone.crewPathIndex},
             {"crew_position", {drone.crewPosition.x, drone.crewPosition.y}},
             {"crew_room", drone.crewRoom},
             {"crew_ship", drone.crewPhysicalShip},
             {"orbit_initialized", drone.orbitInitialized},
             {"orbit_angle", drone.orbitAngle},
             {"pause", drone.pause},
             {"additional_pause", drone.additionalPause},
             {"velocity", {drone.velocity.x, drone.velocity.y}},
             {"destination", {drone.destination.x, drone.destination.y}},
             {"ion_stun", drone.ionStun},
             {"dodge", effectiveDroneDodge(drone)},
             {"aim_angle", drone.aimAngle},
             {"aim_target", {drone.aimTarget.x, drone.aimTarget.y}},
             {"position", {drone.position.x, drone.position.y}},
             {"target_drone", droneSlot(combatDrones_, drone.targetSpaceDroneId)},
             {"space_actor_id", drone.spaceActorId},
             {"cooldown", drone.cooldown},
             {"miss_elapsed", drone.spaceMissElapsed},
             {"miss_position", {drone.spaceMissPosition.x, drone.spaceMissPosition.y}}});
        spaceFlight(enemyDrones.back(), drone);
    }
    for (const auto& hit : shieldHits_) {
        const auto age = tick_ - hit.tick;
        shieldHits.push_back({{"ship", hit.ship},
                              {"position", {hit.point.x, hit.point.y}},
                              {"age_ticks", age},
                              {"projectile_id", hit.projectileId},
                              {"cached_damage", hit.cachedDamage},
                              {"current_size", hit.currentSize},
                              {"current_thickness", hit.currentThickness},
                              {"end_size", 40.f},
                              {"end_thickness", 75.f}});
    }
    auto resistMessages = nlohmann::json::array();
    for (const auto& message : resistMessages_) {
        resistMessages.push_back({{"ship", message.ship},
                                  {"position", {message.point.x, message.point.y}},
                                  {"age_ticks", tick_ - message.tick},
                                  {"elapsed_seconds", message.elapsed},
                                  {"shield_owned", message.shieldOwned},
                                  {"super_shield", message.superShield},
                                  {"message_order", message.messageOrder},
                                  {"ion", message.ion},
                                  {"mind", message.mind}});
    }
    auto damageMessages = nlohmann::json::array();
    for (const auto& message : damageMessages_) {
        damageMessages.push_back({{"ship", message.ship},
                                  {"amount", message.amount},
                                  {"position", {message.point.x, message.point.y}},
                                  {"age_ticks", tick_ - message.tick},
                                  {"elapsed_seconds", message.elapsed},
                                  {"shield_owned", message.shieldOwned},
                                  {"message_order", message.messageOrder},
                                  {"super_shield", message.superShield},
                                  {"ion", message.ion},
                                  {"miss", message.miss},
                                  {"system_damage", message.systemDamage}});
    }
    auto systems = nlohmann::json::array();
    for (const auto& s : enemy_.systems) {
        systems.push_back({{"id", s.definition.id},
                           {"room", s.definition.room},
                           {"station_slot", s.definition.slot},
                           {"power", s.power},
                           {"battery_power", s.batteryPower},
                           {"bonus_power", s.bonusPower},
                           {"crew_power", crewPowerForSystem(s, true).first},
                           {"crew_power_permanent", crewPowerForSystem(s, true).second},
                           {"health", s.health},
                           {"capacity", s.definition.power},
                           {"repair", s.repair},
                           {"fire_damage", s.fireDamage},
                           {"ion", s.ion},
                           {"ion_timer", s.ionTimer},
                           {"lock_count", s.lockCount},
                           {"lock_elapsed", s.lockElapsed},
                           {"hack_level", s.hackLevel},
                           {"hack_active", s.hackActive}});
    }
    auto crew = nlohmann::json::array();
    for (std::size_t i = 0; i < enemy_.crew.size(); ++i) {
        const auto& c = enemy_.crew[i];
        crew.push_back({{"id", i},
                        {"health", c.health},
                        {"stun_time", c.stunTime},
                        {"weapon_skill", c.weaponSkill},
                        {"weapon_skill_progress", c.weaponSkillProgress},
                        {"combat_skill", c.combatSkill},
                        {"combat_skill_progress", c.combatSkillProgress},
                        {"combat_skill_cap", crewSkillCap(c, 5)},
                        {"combat_damage_multiplier", combatSkillDamageMultiplier(c.combatSkill)},
                        {"repair_skill", c.repairSkill},
                        {"repair_skill_progress", c.repairSkillProgress},
                        {"pilot_skill", c.manningSkill[0]},
                        {"pilot_skill_progress", c.manningSkillProgress[0]},
                        {"engine_skill", c.manningSkill[1]},
                        {"engine_skill_progress", c.manningSkillProgress[1]},
                        {"shield_skill", c.manningSkill[2]},
                        {"shield_skill_progress", c.manningSkillProgress[2]},
                        {"room", c.room},
                        {"slot", c.slot},
                        {"position", {c.x, c.y}}});
    }
    // ShipManager::hitByBeam / killedByBeam, both only read by the ship's
    // achievement check. Ages are game seconds so nothing here depends on wall
    // time.
    const auto beamHitState = [&](const std::array<std::map<int, float>, 2>& hits) {
        auto result = nlohmann::json::array();
        for (std::size_t ship = 0; ship < hits.size(); ++ship) {
            auto rooms = nlohmann::json::object();
            for (const auto& [room, at] : hits[ship]) {
                rooms[std::to_string(room)] = Time::seconds(double(tick_)) - double(at);
            }
            result.push_back(std::move(rooms));
        }
        return result;
    };
    const auto beamKillState = [](const std::array<std::map<int, int>, 2>& kills) {
        auto result = nlohmann::json::array();
        for (const auto& ship : kills) {
            auto beams = nlohmann::json::object();
            for (const auto& [id, count] : ship) {
                beams[std::to_string(id)] = count;
            }
            result.push_back(std::move(beams));
        }
        return result;
    };
    auto hazardState = [](const auto& hazards) {
        auto result = nlohmann::json::array();
        for (const auto& [room, hazard] : hazards) {
            if (hazard.fires > 0 || hazard.breaches > 0 || !hazard.fireSmoke.empty()) {
                result.push_back({{"room", room},
                                  {"fires", hazard.fires},
                                  {"breaches", hazard.breaches},
                                  {"breach_tiles", hazard.breachTiles},
                                  {"fire_repair", hazard.fireRepair},
                                  {"fire_tiles", hazard.fireTiles},
                                  {"fire_health", hazard.fireHealth},
                                  {"fire_start_timers", hazard.fireStartTimers},
                                  {"fire_smoke", hazard.fireSmoke},
                                  {"fire_spread_seconds", hazard.fireSpreadSeconds},
                                  {"breach_repair", hazard.breachRepair}});
            }
        }
        return result;
    };
    // Diagnostic: rooms not blacked out (playerRoomVisible / enemyRoomVisible).
    const auto visibleRooms = [this](const ShipDefinition& layout, bool enemy) {
        auto result = nlohmann::json::array();
        for (const auto& room : layout.rooms) {
            if (enemy ? enemyRoomVisible(room.id) : playerRoomVisible(room.id)) {
                result.push_back(room.id);
            }
        }
        return result;
    };
    auto lockedDoors = nlohmann::json::array();
    for (const auto& door : ship_.doors) {
        if (doorLocked(door.id)) {
            lockedDoors.push_back(door.id);
        }
    }
    return {
        {"aiming_weapon", aiming_},
        {"cloak", abilityState(cloak_)},
        {"battery", abilityState(battery_)},
        {"battery_power", batteryPower()},
        {"weapon_ids", ship_.weapons},
        {"weapon_power_intent", weaponDesired_},
        {"rng_state", combatRandom_.state()},
        {"score_inputs",
         {{"scrap", scoreScrap_}, {"beacons", scoreBeacons_}, {"ships", scoreShips_}}},
        {"autofire", autofire_},
        {"player_shields", shieldBubbles_},
        {"player_evasion", evasion()},
        {"player_shield_charge", shieldCharge_},
        {"player_super_shields", superShields_},
        {"weapons", weaponStates(combatWeapons_)},
        {"artillery", artilleryStates(systems_)},
        {"drones", drones},
        {"projectiles", projectiles},
        {"beam_hit_at", beamHitState(beamHitAt_)},
        {"beam_killed_crew", beamKillState(beamKilledCrew_)},
        {"shield_hits", shieldHits},
        {"resist_messages", resistMessages},
        {"damage_messages", damageMessages},
        {"last_hull_damage", lastHullDamage_},
        {"player_hazards", hazardState(playerHazards_)},
        {"player_system_explosions", systemExplosions_[0]},
        {"player_visible_rooms", visibleRooms(ship_, false)},
        {"player_lockdown_rooms", lockdownRooms_},
        {"locked_doors", lockedDoors},
        {"sound_events", combatSounds_},
        {"defeated", defeated_},
        {"enemy_explosion_active", enemyExplosionActive_},
        {"enemy_explosion_duration", explosionDuration_},
        {"enemy_explosion_age", enemyExplosionActive_ ? explosionDuration_ - explosion_ : 0.f},
        {"explosion_seconds", std::max(0.f, explosion_)},
        {"player_explosion_started", playerExplosionStarted_},
        {"player_explosion_elapsed",
         playerExplosionStarted_ ? Time::seconds(double(tick_ - playerExplosionStartTick_)) : 0.0},
        {"destruction_stage",
         playerExplosionStarted_ && !defeated_     ? "player-explosion"
         : !enemyExplosionActive_                  ? "none"
         : explosionDuration_ - explosion_ <= 1.5f ? "bursts"
                                                   : "gibs"},
        {"surrender_offered", surrenderOffered_},
        {"reward",
         {{"pinned_fixture", enemy_.pinnedRewardFixture},
          {"variant", enemy_.rewardVariant},
          {"tier", enemy_.rewardTier},
          {"type", enemy_.rewardType},
          {"unsupported_item", enemy_.rewardUnsupportedItem},
          {"unsupported_crew", enemy_.rewardUnsupportedCrew},
          {"item", enemy_.rewardItem},
          {"committed", enemy_.rewardCommitted},
          {"crew_pending", enemy_.rewardCrewPending},
          {"crew_species", enemy_.rewardCrewSpecies},
          {"crew_name", enemy_.rewardCrewName},
          {"crew_full_name", enemy_.rewardCrewFullName},
          {"crew_male", enemy_.rewardCrewMale},
          {"crew_appearance", enemy_.rewardCrewAppearance},
          {"crew_overflow", enemy_.rewardCrewOverflow}}},
        {"enemy",
         {{"present", enemy_.present},
          {"surrender_threshold",
           {{"configured", enemy_.hasSurrenderThreshold},
            {"min", enemy_.surrenderThresholdMin},
            {"max", enemy_.surrenderThresholdMax},
            {"chance_none", enemy_.surrenderChanceNone},
            {"selected_amount", enemy_.surrenderThresholdSelected},
            {"world_level_offset", enemy_.surrenderWorldLevelOffset},
            {"effective", enemy_.surrenderThreshold}}},
          {"cloak", abilityState(enemy_.cloak)},
          {"battery", abilityState(enemy_.battery)},
          {"battery_power", batteryPower(true)},
          {"fire_while_cloaked", enemy_.fireWhileCloaked},
          {"ship", enemy_.ship.id},
          {"evasion", evasion(1)},
          {"hull", enemy_.hull},
          {"missiles", enemy_.missiles},
          {"drone_parts", enemy_.droneParts},
          {"escape_threshold", enemy_.escapeThreshold},
          {"escape_timer", enemy_.escapeTimer},
          {"escaping", enemy_.escaping},
          {"escape_jump_elapsed", enemy_.escapeJumpElapsed},
          {"stalemate_trigger", enemy_.stalemateTrigger},
          {"stalemate_timer", enemy_.stalemateTimer},
          {"last_escape_health", enemy_.lastEscapeHealth},
          {"mind", enemy_.mind},
          {"mind_target", enemy_.mindTarget},
          {"mind_queue", mindQueue_[1]},
          {"hacking", enemy_.hacking},
          {"hacking_target", enemy_.hackingTarget},
          {"hacking_drone", enemy_.hackingDrone},
          {"hacking_drone_space", enemy_.hackingDroneSpace},
          {"hacking_drone_id", enemy_.hackingDroneId},
          {"clone_queue", enemy_.cloneQueue},
          {"clone_timer", enemy_.cloneTimer},
          {"clone_death_timer", enemy_.cloneDeathTimer},
          {"clone_duration", enemy_.cloneDuration},
          {"shields", enemy_.shields},
          {"shield_charge", enemy_.shieldCharge},
          {"super_shields", enemy_.superShields},
          {"systems", systems},
          {"crew", crew},
          {"drones", enemyDrones},
          {"hazards", hazardState(enemyHazards_)},
          {"system_explosions", systemExplosions_[1]},
          {"visible_rooms", visibleRooms(enemy_.ship, true)},
          {"lockdown_rooms", enemy_.lockdownRooms},
          {"room_oxygen", enemy_.roomOxygen},
          {"weapons", weaponStates(enemy_.weapons)},
          {"artillery", artilleryStates(enemy_.systems)},
          {"weapon_power", enemy_.powered}}}};
}
} // namespace ftl
