#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>

namespace ftl {
namespace {
bool passive(int id) {
    return (id >= 6 && id <= 8) || id == 12;
}

float distance(SDL_FPoint a, SDL_FPoint b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}
} // namespace

const RunSystem* Run::system(int id) const {
    for (const auto& s : systems_) {
        if (s.definition.id == id) {
            return &s;
        }
    }
    return nullptr;
}

int Run::availablePower() const {
    int used = 0;
    for (const auto& s : systems_) {
        if (!passive(s.definition.id)) {
            used += std::max(0, s.power - s.bonusPower);
        }
    }
    return reactorCapacity() + batteryPower() - used;
}

int Run::roomAt(float x, float y) const {
    for (const auto& r : ship_.rooms) {
        if (Rect{float(r.x), float(r.y), float(r.w), float(r.h)}.contains(x, y)) {
            return r.id;
        }
    }
    return -1;
}

SDL_FPoint Run::slotPoint(int id, int slot) const {
    const auto& r = room(id);
    return {float(r.x + 35 * (slot % (r.w / 35)) + 17), float(r.y + 35 * (slot / (r.w / 35)) + 17)};
}

int Run::primarySlot(int id) const {
    for (const auto& s : systems_) {
        if (s.definition.room == id && s.definition.id != 5 && s.definition.id != 13 &&
            s.definition.slot >= 0) {
            return s.definition.slot;
        }
    }
    return -1;
}

void Run::advanceStationHandoff(RunCrew& member, int physicalShip, int creationOwner) {
    // CrewMember4747f0 LABEL100: after prior-target damage, before movement.
    // CanMan720f70 tests effective allegiance, stun and death-animation status;
    // system damage and room hazards do not independently suppress this branch.
    const bool foreign = member.intruder;
    const bool intruder = (creationOwner != physicalShip) != member.controlled;
    if (intruder || member.dead || member.dying || member.outOfGame || member.health <= 0 ||
        member.stunTime > 0 || member.repairKind != 0 || member.attackTargetOwner >= 0 ||
        !(foreign ? member.boardPath : member.path).empty()) {
        return;
    }
    const int roomId = foreign ? member.boardRoom : member.room;
    const auto& stations = physicalShip == 0 ? systems_ : enemy_.systems;
    const auto station = std::ranges::find_if(stations, [roomId](const RunSystem& entry) {
        return entry.definition.room == roomId && entry.definition.id != 5 &&
               entry.definition.id != 13 && entry.definition.slot >= 0;
    });
    if (station == stations.end()) {
        return;
    }
    if ((foreign ? member.boardTargetSlot : member.targetSlot) == station->definition.slot &&
        (foreign ? member.boardTargetRoom : member.targetRoom) == roomId) {
        return;
    }
    RunCrew proxy = member;
    if (foreign) {
        proxy.intruder = false;
        proxy.room = member.boardRoom;
        proxy.slot = member.boardSlot;
        proxy.x = member.boardX;
        proxy.y = member.boardY;
        proxy.targetRoom = member.boardTargetRoom;
        proxy.targetSlot = member.boardTargetSlot;
        proxy.path = member.boardPath;
        proxy.newGoal = member.boardNewGoal;
        proxy.lastDoor = member.boardLastDoor;
        proxy.blockingDoor = member.boardBlockingDoor;
    }
    if (!order(proxy, roomId, station->definition.slot, true, physicalShip == 1, &member, 0) ||
        proxy.targetSlot != station->definition.slot) {
        return;
    }
    if (foreign) {
        member.boardTargetRoom = proxy.targetRoom;
        member.boardTargetSlot = proxy.targetSlot;
        member.boardPath = std::move(proxy.path);
        member.boardNewGoal = proxy.newGoal;
        member.boardLastDoor = proxy.lastDoor;
        member.boardBlockingDoor = proxy.blockingDoor;
    } else {
        member.targetRoom = proxy.targetRoom;
        member.targetSlot = proxy.targetSlot;
        member.path = std::move(proxy.path);
        member.newGoal = proxy.newGoal;
        member.lastDoor = proxy.lastDoor;
        member.blockingDoor = proxy.blockingDoor;
    }
}

void Run::advanceStationHandoffs(bool enemy) {
    for (auto& member : crew_) {
        if (member.intruder == enemy) {
            advanceStationHandoff(member, int(enemy), 0);
        }
    }
    for (auto& member : enemy_.crew) {
        if (!member.intruder == enemy) {
            advanceStationHandoff(member, int(enemy), 1);
        }
    }
    if (!enemy) {
        for (auto& member : voyageBoarders_) {
            advanceStationHandoff(member, 0, 1);
        }
    }
}

bool Run::enemyAutomated() const {
    // ShipManager::bAutomated. ShipGenerator::CreateShip 0x5fca40 and
    // AddInitialCrew 0x4a3610 set it when the ship is built (or imported)
    // without any crew; enemy crew records are never removed, so an empty
    // roster is exactly that case. The flagship's BossShip::StartStage
    // 0x406490 clears it and GetSubEvent 0x406200 sets it on DeadCrew.
    return campaign_.fighting ? campaign_.automated : enemy_.crew.empty();
}

bool Run::hasCrewManning(int id, bool enemy) const {
    const auto& systems = enemy ? enemy_.systems : systems_;
    const auto station = std::ranges::find_if(systems, [&](const auto& sys) {
        return sys.definition.id == id;
    });
    if (station == systems.end()) {
        return false;
    }
    const auto eligible = [&](const RunCrew& actor, bool player) {
        const bool physical =
            player ? actor.intruder : std::ranges::any_of(enemy_.crew, [&](const auto& member) {
                return &member == &actor;
            });
        const bool intruder = (player ? actor.intruder : !physical) != actor.controlled;
        const int roomId = player && actor.intruder ? actor.boardRoom : actor.room;
        return physical == enemy && !intruder && actor.health > 0 && actor.stunTime <= 0 &&
               roomId == station->definition.room;
    };
    // SetCurrentSystem46e210 sets bManned for any CanMan actor in the room;
    // movement, fighting and damage only inhibit the separate active boost.
    return std::ranges::any_of(crew_,
                               [&](const auto& actor) {
                                   return eligible(actor, true);
                               }) ||
           std::ranges::any_of(enemy_.crew,
                               [&](const auto& actor) {
                                   return eligible(actor, false);
                               }) ||
           std::ranges::any_of(voyageBoarders_, [&](const auto& actor) {
               return eligible(actor, false);
           });
}

const RunCrew* Run::crewOperator(int id, bool enemy) const {
    const auto& systems = enemy ? enemy_.systems : systems_;
    const auto station = std::ranges::find_if(systems, [&](const auto& sys) {
        return sys.definition.id == id;
    });
    // SetCurrentSystem 0x46e210 needs bBoostable. The ShipSystem ctor 0x61bab0
    // clears it for oxygen, medbay, drones, teleporter, cloaking, battery,
    // clone bay, mind control and hacking, and sets it on artillery only for
    // the enemy; ShipManager::OnLoop 0x4a05b0 copies the engines' Functioning
    // (effective power > 0) into piloting's flag every frame.
    const auto engines = std::ranges::find_if(systems, [](const auto& sys) {
        return sys.definition.id == 1;
    });
    const bool boostable = id == 0 || id == 1 || id == 3 || id == 7 || id == 8 ||
                           (id == 11 && enemy) ||
                           (id == 6 && engines != systems.end() && engines->power > 0);
    // BlockedBoosted 0x6155f0 calls the virtual Powered; Shields 0x60aec0
    // needs a whole bubble (effective power above one).
    if (station == systems.end() || !boostable || station->power <= (id == 0 ? 1 : 0) ||
        station->health != station->definition.power || systemLocked(*station) ||
        (station->hackActive && station->hackLevel > 0) || station->powerLoss > 0 ||
        station->powerDivide > 1 ||
        station->powerLimit < station->definition.power + int(id == 7 || id == 8)) {
        return nullptr;
    }
    const auto& hazards = enemy ? enemyHazards_ : playerHazards_;
    const auto hazard = hazards.find(station->definition.room);
    if (station->occupied || (hazard != hazards.end() && hazard->second.fires > 0)) {
        return nullptr;
    }
    const RunCrew* result = nullptr;
    const auto inspect = [&](const auto& actors, bool player, bool physical) {
        for (const auto& actor : actors) {
            const bool aboard = player ? actor.intruder : physical;
            const bool intruder = (player ? actor.intruder : !physical) != actor.controlled;
            const bool foreign = player && actor.intruder;
            const int roomId = foreign ? actor.boardRoom : actor.room;
            const int slot = foreign ? actor.boardTargetSlot : actor.targetSlot;
            const int targetRoom = foreign ? actor.boardTargetRoom : actor.targetRoom;
            const auto& path = foreign ? actor.boardPath : actor.path;
            if (aboard != enemy || intruder || actor.health <= 0 || actor.stunTime > 0 ||
                !path.empty() || roomId != station->definition.room || targetRoom != roomId ||
                slot != station->definition.slot || crewHasOpponent(actor)) {
                continue;
            }
            const auto& layout = enemy ? enemy_.ship : ship_;
            const auto shape = std::ranges::find(layout.rooms, roomId, &RoomDefinition::id);
            if (shape == layout.rooms.end() || slot < 0) {
                continue;
            }
            const int columns = std::max(1, shape->w / 35);
            const auto point = crewCombatPoint(actor);
            if (std::abs(point.x - float(shape->x + slot % columns * 35 + 17)) < .5f &&
                std::abs(point.y - float(shape->y + slot / columns * 35 + 17)) < .5f) {
                result = &actor;
            }
        }
    };
    inspect(crew_, true, false);
    inspect(enemy_.crew, false, true);
    inspect(voyageBoarders_, false, false);
    return result;
}

void Run::updateManning(int owner) {
    for (const bool enemy : {false, true}) {
        if (owner >= 0 && int(enemy) != owner) {
            continue;
        }
        for (auto& sys : enemy ? enemy_.systems : systems_) {
            sys.occupied = sys.friendlies = false;
            const auto inspect =
                [&](const RunCrew& member, int creationOwner, int physicalShip, int room) {
                    if (member.outOfGame || member.dead || physicalShip != int(enemy) ||
                        room != sys.definition.room) {
                        return;
                    }
                    const int allegiance = member.controlled ? 1 - creationOwner : creationOwner;
                    (allegiance == int(enemy) ? sys.friendlies : sys.occupied) = true;
                };
            for (const auto& member : crew_) {
                inspect(member,
                        0,
                        member.intruder ? 1 : 0,
                        member.intruder ? member.boardRoom : member.room);
            }
            for (const auto& member : enemy_.crew) {
                inspect(member,
                        1,
                        member.intruder ? 0 : 1,
                        member.intruder ? member.boardRoom : member.room);
            }
            for (const auto& member : voyageBoarders_) {
                inspect(member, 1, 0, member.room);
            }
            for (int droneOwner : {0, 1}) {
                for (const auto* drone : physicalCrewDrones(droneOwner)) {
                    if (!drone->crewDead && drone->crewPhysicalShip == int(enemy) &&
                        drone->crewRoom == sys.definition.room) {
                        (droneOwner == int(enemy) ? sys.friendlies : sys.occupied) = true;
                    }
                }
            }
            const auto* member = crewOperator(sys.definition.id, enemy);
            const int id = sys.definition.id;
            sys.manningLevel = !member   ? 0
                               : id == 3 ? member->weaponSkill + 1
                               : id == 0 || id == 1 || id == 6
                                   ? member->manningSkill[std::size_t(id == 0   ? 2
                                                                      : id == 1 ? 1
                                                                                : 0)] +
                                         1
                                   : 1;
            // UpdateCrewmembers49ee10 initializes active manning to bAutomated.
            if (!member && enemy && enemyAutomated()) {
                sys.manningLevel = 1;
            }
            sys.operatorActive = sys.manningLevel > 0;
        }
    }
}

bool Run::manned(int id) const {
    auto* s = system(id);
    return s && s->operatorActive;
}

int Run::subsystemLevel(int id) const {
    const auto* s = system(id);
    if (!s || s->health <= 0 || s->power <= 0) {
        return 0;
    }
    const int level = std::min(s->power, s->health) + int(s->operatorActive);
    return std::min(level, id == 7 || id == 8 ? 4 : s->definition.power);
}

int Run::evasion(int owner) const {
    const auto& systems = owner == 0 ? systems_ : enemy_.systems;
    const auto station = [&](int id) -> const RunSystem* {
        for (const auto& candidate : systems) {
            if (candidate.definition.id == id) {
                return &candidate;
            }
        }
        return nullptr;
    };
    const int cloakBonus = cloaked(owner) ? 60 : 0;
    const auto* engines = station(1);
    const auto* pilot = station(6);
    const auto effective = [](const RunSystem* s) {
        return s && (!s->hackActive || s->hackLevel <= 1) ? std::clamp(s->power, 0, s->health) : 0;
    };
    if (effective(engines) == 0 || effective(pilot) == 0) {
        return cloakBonus;
    }
    // ShipManager::GetDodgeFactor 0x491e20: shared engine/skill/autopilot
    // calculation for both sides, including hack and effective pilot level.
    const auto operatorLevel = [&](int id) {
        const auto* s = station(id);
        // IsMannedBoost 0x615900 reads iActiveManned, which an automated
        // ship's UpdateCrewmembers seeds with bAutomated (manningLevel 1).
        if (!s || s->health != s->definition.power) {
            return 0;
        }
        return s->manningLevel;
    };
    constexpr std::array<int, 9> base{0, 5, 10, 15, 20, 25, 28, 31, 35};
    constexpr std::array<int, 4> skill{0, 5, 7, 10};
    const int pilotLevel = operatorLevel(6);
    const int value = base[std::size_t(std::clamp(effective(engines), 0, 8))] +
                      skill[std::size_t(operatorLevel(1))] + skill[std::size_t(pilotLevel)];
    // ShipFlying 0x491d70: the pilot is bManned, which UpdateCrewmembers
    // seeds with bAutomated before any crew mans it.
    const bool automated = owner == 1 && enemyAutomated();
    if (hasCrewManning(6, owner == 1) || automated) {
        return cloakBonus + value;
    }
    return cloakBonus +
           (effective(pilot) <= 1 ? 0 : int(value * (effective(pilot) == 2 ? .5f : .8f)));
}

bool Run::power(int id, bool add) {
    auto* s = system(id);
    if (!s || passive(id) || systemLocked(*s)) {
        return false;
    }
    // Increase617280/Decrease6173e0 dispatch the virtual family operation.
    // Decrease requests one bar even for shields; the derived callback alone
    // decides whether its unlocked odd pair requires one additional debit.
    if (!(add ? forceIncreaseSystemPower(*s) : forceDecreaseSystemPower(*s, 1))) {
        return false;
    }
    // ShipSystem::IncreasePower 0x617280 / DecreasePower 0x6173e0 update
    // lastUserPower; ion recovery must respect the player's latest allocation.
    s->restorePower = s->power;
    // SystemBox::AdjustPowerTo 0x4cd470 and OnTouch 0x4cd5e0 sound only
    // after a successful user power change.
    audio_.sound(add ? "powerUpSystem" : "powerDownSystem");
    return true;
}

bool Run::weaponPower(int index, bool add) {
    if (index < 0 || index >= int(weaponPower_.size())) {
        return false;
    }
    if (!add && !weaponPower_[index]) {
        // WeaponSystem::DePowerWeapon 0x610830 returns at once for an unpowered
        // weapon, so its userPowered bit (weaponDesired_) survives.
        return false;
    }
    auto* s = system(3);
    if (!s || systemLocked(*s)) {
        return false;
    }
    const auto& e = catalog_.equipment(ship_.weapons[index]);
    const int required = std::max(0, e.power - combatWeapons_[index].bonusPower);
    if (weaponPower_[index] == add) {
        return false;
    }
    if (add) {
        if (e.missiles != 0 && missiles_ <= 0 && combatWeapons_[index].pendingProjectiles.empty()) {
            return false;
        }
        if (availablePower() < required || s->power + required > systemCapacity(*s)) {
            return false;
        }
        s->power += required;
        assignBatteryPower(*s, required);
    } else {
        s->power -= required;
        releaseBatteryPower(*s, required);
    }
    weaponPower_[index] = add;
    weaponDesired_[index] = add;
    return true;
}

// ShipGraph::FindPath 0x45a7d0 and ShipGraph::Dijkstra 0x459ba0. The graph is
// Ship::vDoorList: Ship::Import 0x486942 sends a layout door whose second room
// is -1 to vOuterAirlocks, and every other door keeps its layout order. Point
// distances are Point::Distance 0x585e90 integers, so the running minimum of
// PopClosestDoor is the first smallest door left in id order. The search ends
// at the first popped door that enters the goal room.
std::optional<Run::DoorPath> Run::findDoorPath(
    const ShipDefinition& ship, SDL_Point start, SDL_Point goal, int shipId, bool enemy) const {
    const auto roomAt = [&](SDL_Point p) {
        // Room::ContainsPoint 0x46af30 with walls; the first listed room wins.
        for (const auto& room : ship.rooms) {
            if (room.x <= p.x && p.x <= room.x + room.w && room.y <= p.y &&
                p.y <= room.y + room.h) {
                return room.id;
            }
        }
        return -1;
    };
    const int from = roomAt(start);
    const int to = roomAt(goal);
    if (from < 0 || to < 0) {
        return std::nullopt;
    }
    if (from == to) {
        return DoorPath{from, {}};
    }
    std::vector<int> graph;
    for (std::size_t index = 0; index < ship.doors.size(); ++index) {
        if (ship.doors[index].roomB != -1) {
            graph.push_back(int(index));
        }
    }
    const auto pointDistance = [](SDL_Point a, SDL_Point b) {
        const int squared = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y);
        return int(std::sqrt(double(float(squared))));
    };
    const auto center = [&](int node) {
        const auto& door = ship.doors[std::size_t(graph[std::size_t(node)])];
        return SDL_Point{door.x + (door.vertical ? 0 : 17), door.y + (door.vertical ? 17 : 0)};
    };
    const auto connecting = [&](int node, int room) {
        const auto& door = ship.doors[std::size_t(graph[std::size_t(node)])];
        return door.roomA == room ? door.roomB : door.roomB == room ? door.roomA : -1;
    };
    const int count = int(graph.size());
    std::vector<float> dist(std::size_t(count), 1000000.f);
    std::vector<int> prev(std::size_t(count), -1);
    std::vector<int> next(std::size_t(count), -1);
    const auto doorsOf = [&](int room, auto&& visit) {
        // ShipGraph::GetDoors 0x459140 uses Door::ConnectsRooms(room, -1).
        for (int node = 0; node < count; ++node) {
            const auto& door = ship.doors[std::size_t(graph[std::size_t(node)])];
            if (door.roomA == room || door.roomB == room) {
                visit(node);
            }
        }
    };
    doorsOf(from, [&](int node) {
        dist[std::size_t(node)] = float(pointDistance(start, center(node)));
        next[std::size_t(node)] = connecting(node, from);
    });
    std::vector<int> queue(static_cast<std::size_t>(count));
    std::iota(queue.begin(), queue.end(), 0);
    while (!queue.empty()) {
        std::size_t pick = 0;
        int best = int(dist[std::size_t(queue[0])]);
        for (std::size_t index = 1; index < queue.size(); ++index) {
            if (float(best) > dist[std::size_t(queue[index])]) {
                best = int(dist[std::size_t(queue[index])]);
                pick = index;
            }
        }
        const int id = queue[pick];
        queue.erase(queue.begin() + std::ptrdiff_t(pick));
        const int room = next[std::size_t(id)];
        if (room == to) {
            DoorPath path{from, {}};
            for (int node = id; node != -1; node = prev[std::size_t(node)]) {
                path.doors.push_back(graph[std::size_t(node)]);
            }
            std::ranges::reverse(path.doors);
            return path;
        }
        doorsOf(room, [&](int node) {
            const float sealed =
                shipId != -1 && crewDoorSealed(graph[std::size_t(node)], shipId, false, enemy)
                    ? 1000.f
                    : 0.f;
            const float cost =
                float(pointDistance(center(id), center(node))) + dist[std::size_t(id)] + sealed;
            if (dist[std::size_t(node)] > cost) {
                dist[std::size_t(node)] = cost;
                prev[std::size_t(node)] = id;
                next[std::size_t(node)] = connecting(node, room);
            }
        });
    }
    return std::nullopt;
}

// The slot half of CrewMember::FindSlot 0x46f270 for MoveToRoom (closeEnough
// false): Room::GetEmptySlot 0x46a0f0 on the actor's occupancy side, or -1 when
// that side of the room is full.
int Run::crewFreeSlot(const RunCrew* self,
                      int destination,
                      int requested,
                      bool enemy,
                      const RunCrew* boardingSource,
                      int side,
                      const CombatDrone* movingDrone) const {
    const auto& ship = enemy ? enemy_.ship : ship_;
    const auto found = std::ranges::find(ship.rooms, destination, &RoomDefinition::id);
    if (found == ship.rooms.end()) {
        throw std::runtime_error("invalid_crew_room");
    }
    const auto& target = *found;
    int count = target.w / 35 * (target.h / 35);
    auto free = [&](int slot) {
        // AddSystem 0x49ca00 reserves medical fixtures for both occupancy sides.
        const bool blocked =
            std::ranges::any_of(enemy ? enemy_.systems : systems_, [&](const RunSystem& sys) {
                const auto& definition = sys.definition;
                return (definition.id == 5 || definition.id == 13) &&
                       definition.room == destination && definition.slot != -2 &&
                       slot == (definition.slot < 0 ? 1 : definition.slot);
            });
        // Room slot vectors in UpdateCrewmembers 0x49ee10 include every
        // physically present CrewMember, including CrewDrone, and keep separate
        // friendly/intruder occupancy. Native ownership lists do not define it.
        const bool playerReserved = std::ranges::any_of(crew_, [&](const RunCrew& other) {
            if (&other == self || &other == boardingSource || other.dead || other.outOfGame ||
                other.intruder != enemy || int(other.intruder != other.controlled) != side) {
                return false;
            }
            return other.intruder
                       ? other.boardPositionValid && other.boardTargetRoom == destination &&
                             other.boardTargetSlot == slot
                       : other.targetRoom == destination && other.targetSlot == slot;
        });
        const bool enemyReserved =
            enemy && std::ranges::any_of(enemy_.crew, [&](const RunCrew& other) {
                return &other != self && &other != boardingSource && !other.dead &&
                       !other.outOfGame && !other.intruder && int(other.controlled) == side &&
                       other.targetRoom == destination && other.targetSlot == slot;
            });
        const bool boarderReserved =
            !enemy && std::ranges::any_of(voyageBoarders_, [&](const RunCrew& other) {
                return &other != self && &other != boardingSource && !other.dead &&
                       !other.outOfGame && int(!other.controlled) == side &&
                       other.targetRoom == destination && other.targetSlot == slot;
            });
        const auto droneReserved = [&](const auto& drones, int owner) {
            return std::ranges::any_of(drones, [&](const CombatDrone& drone) {
                const auto& type = catalog_.equipment(drone.blueprint).type;
                if (&drone == movingDrone || drone.crewPhysicalShip < 0 || drone.crewDead ||
                    (type != "REPAIR" && type != "BATTLE" && type != "BOARDER")) {
                    return false;
                }
                const int physicalShip = drone.crewPhysicalShip;
                if (physicalShip != int(enemy) || int(owner != physicalShip) != side ||
                    drone.targetRoom != destination) {
                    return false;
                }
                const auto point =
                    drone.crewPath.empty() ? drone.crewPosition : drone.crewPath.back();
                const int columns = std::max(1, target.w / 35);
                const int column = std::clamp(int((point.x - target.x) / 35), 0, columns - 1);
                const int row =
                    std::clamp(int((point.y - target.y) / 35), 0, std::max(1, target.h / 35) - 1);
                return row * columns + column == slot;
            });
        };
        return !blocked && !playerReserved && !enemyReserved && !boarderReserved &&
               !droneReserved(combatDrones_, 0) && !droneReserved(enemy_.drones, 1) && slot >= 0 &&
               slot < count;
    };
    int primary = 0;
    if (side == 0) {
        if (!enemy) {
            primary = primarySlot(destination);
        } else {
            for (const auto& sys : enemy_.systems) {
                if (sys.definition.room == destination && sys.definition.slot >= 0 &&
                    sys.definition.id != 5 && sys.definition.id != 13) {
                    primary = sys.definition.slot;
                    break;
                }
            }
        }
    }
    // Room::GetEmptySlot 0x46a0f0 offers the station's primary slot only to
    // the friendly occupancy side, including controlled enemy residents.
    int slot = free(requested) ? requested : primary;
    if (!free(slot)) {
        slot = 0;
        while (slot < count && !free(slot)) {
            ++slot;
        }
    }
    return slot == count ? -1 : slot;
}

bool Run::order(RunCrew& c,
                int destination,
                int requested,
                bool force,
                bool enemy,
                const RunCrew* boardingSource,
                int occupancySide,
                const CombatDrone* movingDrone) {
    if (!enemy && c.intruder && std::ranges::any_of(crew_, [&](const auto& member) {
            return &member == &c;
        })) {
        if (!enemy_.present || !c.boardPositionValid) {
            return false;
        }
        RunCrew foreign = c;
        foreign.intruder = false;
        foreign.x = c.boardX;
        foreign.y = c.boardY;
        foreign.room = c.boardRoom;
        foreign.slot = c.boardSlot;
        foreign.targetRoom = c.boardTargetRoom;
        foreign.targetSlot = c.boardTargetSlot;
        foreign.path = c.boardPath;
        foreign.newGoal = c.boardNewGoal;
        foreign.lastDoor = c.boardLastDoor;
        foreign.blockingDoor = c.boardBlockingDoor;
        if (!order(foreign, destination, requested, force, true, &c, occupancySide, movingDrone)) {
            return false;
        }
        c.boardTargetRoom = foreign.targetRoom;
        c.boardTargetSlot = foreign.targetSlot;
        c.boardPath = std::move(foreign.path);
        c.boardNewGoal = foreign.newGoal;
        c.boardLastDoor = foreign.lastDoor;
        c.boardBlockingDoor = foreign.blockingDoor;
        return true;
    }
    const auto& ship = enemy ? enemy_.ship : ship_;
    const bool voyageMember =
        !enemy && std::ranges::any_of(voyageBoarders_, [&](const RunCrew& member) {
            return &member == &c;
        });
    const int side = occupancySide >= 0
                         ? occupancySide
                         : int((boardingSource != nullptr || voyageMember) != c.controlled);
    const auto getRoom = [&](int id) -> const RoomDefinition& {
        for (const auto& r : ship.rooms) {
            if (r.id == id) {
                return r;
            }
        }
        throw std::runtime_error("invalid_crew_room");
    };
    if ((!force && c.controlled) || c.health <= 0 || destination < 0 ||
        (!force && destination == c.targetRoom)) {
        return false;
    }
    const auto& target = getRoom(destination);
    const int slot =
        crewFreeSlot(&c, destination, requested, enemy, boardingSource, side, movingDrone);
    if (slot < 0) {
        return false;
    }
    // CrewMember::MoveToRoom 0x472640 paths with the actor's own iShipId, so a
    // door sealed against its owner costs 1000 in ShipGraph::Dijkstra.
    int owner = 1;
    if (movingDrone) {
        owner = std::ranges::any_of(combatDrones_,
                                    [&](const CombatDrone& drone) {
                                        return &drone == movingDrone;
                                    })
                    ? 0
                    : 1;
    } else if (boardingSource || std::ranges::any_of(crew_, [&](const RunCrew& member) {
                   return &member == &c;
               })) {
        owner = 0;
    }
    const SDL_Point goalPoint{target.x + 35 * (slot % (target.w / 35)) + 17,
                              target.y + 35 * (slot / (target.w / 35)) + 17};
    const auto found = findDoorPath(ship, {int(c.x), int(c.y)}, goalPoint, owner, enemy);
    if (!found) {
        return false;
    }
    auto center = [&](int d) {
        const auto& door = ship.doors[d];
        return SDL_FPoint{float(door.x + (door.vertical ? 0 : 17)),
                          float(door.y + (door.vertical ? 17 : 0))};
    };
    SDL_FPoint start{c.x, c.y};
    // Enemy orders use the enemy layout. slotPoint() resolves player rooms.
    SDL_FPoint finish{float(goalPoint.x), float(goalPoint.y)};
    const std::vector<int>& route = found->doors;
    std::deque<CrewWaypoint> path;
    SDL_FPoint position = start;
    int current = found->startRoom;
    auto push = [&](SDL_FPoint p, int door = -1) {
        if (distance(position, p) > .05f) {
            path.push_back({p, door});
            position = p;
        }
    };
    for (int id : route) {
        const auto& d = ship.doors[id];
        auto mid = center(id);
        const auto& r = getRoom(current);
        bool negative = d.vertical ? r.x < mid.x : r.y < mid.y;
        SDL_FPoint entry = mid;
        if (d.vertical) {
            entry.x += negative ? -17 : 17;
        } else {
            entry.y += negative ? -18 : 17;
        }
        // GetNewGoal0x471fb0: align within entry tile, then cross by34/35 pixels.
        if (int(position.x / 35) != int(entry.x / 35) ||
            int(position.y / 35) != int(entry.y / 35)) {
            push(entry);
        }
        if (d.vertical) {
            push({position.x, entry.y});
        } else {
            push({entry.x, position.y});
        }
        SDL_FPoint exit = entry;
        if (d.vertical) {
            exit.x += negative ? 34 : -35;
        } else {
            exit.y += negative ? 34 : -35;
        }
        push(exit, id);
        current = d.roomA == current ? d.roomB : d.roomA;
        const auto& next = getRoom(current);
        if (exit.x - next.x <= 15 || next.x + next.w - exit.x <= 15 || exit.y - next.y <= 15 ||
            next.y + next.h - exit.y <= 15) {
            int px = int(exit.x) + (next.x + next.w / 2 > exit.x ? 1 : -1);
            int py = int(exit.y) + (next.y + next.h / 2 > exit.y ? 1 : -1);
            push({float(px / 35 * 35 + 17), float(py / 35 * 35 + 17)});
        }
    }
    push(finish);
    c.targetRoom = destination;
    c.targetSlot = slot;
    c.path = std::move(path);
    c.newGoal = true;
    c.blockingDoor = -1;
    return true;
}

void Run::advanceCrew(RunCrew& c, bool enemy, int owner) {
    if (c.intruder || c.health <= 0) {
        return;
    }
    // OnLoop 0x4747f0 refreshes the last crossed door until the final
    // destination is reached, including while movement is stunned.
    auto& fake = enemy ? enemy_.fakeDoors : fakeDoors_;
    auto& timers = enemy ? enemy_.doorTimers : doorTimers_;
    if (!c.path.empty() && c.lastDoor >= 0 && std::size_t(c.lastDoor) < fake.size()) {
        fake[std::size_t(c.lastDoor)] = true;
        timers[std::size_t(c.lastDoor)] = 1;
    }
    // A teleporting actor holds still until its strip finishes.
    if (c.path.empty() || c.stunTime > 0 || c.teleportArrive > 0 || c.teleportLeaveKind != 0) {
        return;
    }
    const int originalOwner = owner < 0 ? int(enemy) : owner;
    if (c.blockingDoor >= 0 &&
        !crewDoorSealed(c.blockingDoor, originalOwner, c.controlled, enemy)) {
        c.blockingDoor = -1;
        c.path.pop_front();
        c.newGoal = true;
        if (c.path.empty()) {
            return;
        }
    }
    if (c.blockingDoor < 0 && c.path.front().door >= 0 &&
        crewDoorSealed(c.path.front().door, originalOwner, c.controlled, enemy)) {
        const int id = c.path.front().door;
        const auto& door = (enemy ? enemy_.ship : ship_).doors[std::size_t(id)];
        const SDL_FPoint center{float(door.x + (door.vertical ? 0 : 17)),
                                float(door.y + (door.vertical ? 17 : 0))};
        SDL_FPoint approach = center;
        // GetNewGoal471fb0 makes one perpendicular draw in [-8,8] and
        // stands eleven pixels from the blocking door while attacking it.
        const float jitter = float(int(combatRandom_.next() % 17) - 8);
        if (door.vertical) {
            approach.x += c.x < center.x ? -11.f : 11.f;
            approach.y += jitter;
        } else {
            approach.y += c.y < center.y ? -11.f : 11.f;
            approach.x += jitter;
        }
        c.blockingDoor = id;
        c.path.push_front({approach, -1});
        c.newGoal = true;
    }
    auto& next = c.path.front();
    if (c.newGoal) {
        // GetNewGoal 0x471fb0 closes the previous transient opening before
        // selecting each next subgoal. Real openness is independent.
        if (c.lastDoor >= 0 && std::size_t(c.lastDoor) < fake.size()) {
            fake[std::size_t(c.lastDoor)] = false;
        }
        c.lastDoor = -1;
        c.newGoal = false;
        auto dx = next.point.x - c.x;
        auto dy = next.point.y - c.y;
        c.facing =
            std::abs(dx) > std::abs(dy) ? (dx > 0 ? "right" : "left") : (dy > 0 ? "down" : "up");
        if (next.door >= 0) {
            const auto& open = enemy ? enemy_.doors : doors_;
            const auto& forced = enemy ? enemy_.doorForcedTicks : voyageDoorForcedTicks_;
            if (!open[std::size_t(next.door)] && !forced.contains(next.door)) {
                c.lastDoor = next.door;
                fake[std::size_t(next.door)] = true;
                timers[std::size_t(next.door)] = 1;
            }
        }
        return;
    }
    float remaining = distance({c.x, c.y}, next.point);
    // CrewMember::UpdateMovement 0x4745b0 snaps once both axes are within half
    // a pixel of the goal.
    if (std::abs(next.point.x - c.x) < .5f && std::abs(next.point.y - c.y) < .5f) {
        c.x = next.point.x;
        c.y = next.point.y;
        if (c.blockingDoor >= 0) {
            return;
        }
        c.path.pop_front();
        c.newGoal = true;
        if (c.path.empty()) {
            c.room = c.targetRoom;
            c.slot = c.targetSlot;
        } else {
            advanceCrew(c, enemy, owner);
        }
        return;
    }
    // CFPS::OnLoop0x59a4e0: dt*16, CrewMember::GetMoveSpeed0x46fb60: factor*5.
    // Every CrewDrone (battle, repair, boarding and ion boarding bodies) uses
    // CrewDrone::GetMoveSpeedMultipler 0x437570 (0.5) and cannot be stimmed.
    const bool drone = c.appearance.species == "battle" || c.appearance.species == "repair" ||
                       c.appearance.species == "boarder_ion";
    float speed = (drone ? 40.f : 80.f) * Time::step;
    const bool enemyOwner = owner < 0 ? enemy : owner == 1;
    // CrewMember::GetMoveSpeed 0x46fb60 uses the actor's owning ship, even
    // while aboard its opponent. Crew drones cannot receive CREW_STIMS.
    for (const auto& augment : enemyOwner ? enemy_.ship.augments : ship_.augments) {
        if (augment == (drone ? "DRONE_SPEED" : "CREW_STIMS")) {
            speed += (drone ? 40.f : 80.f) * Time::step * float(catalog_.equipment(augment).value);
        }
    }
    if (c.appearance.species == "mantis") {
        speed *= 1.2f;
    } else if (c.appearance.species == "rock") {
        speed *= .5f;
    } else if (c.appearance.species == "crystal") {
        speed *= .8f; // CrystalAlien::GetMoveSpeedMultipler 0x726140.
    } else if (c.appearance.species == "anaerobic") {
        const auto& oxygen = enemy ? enemy_.roomOxygen : roomOxygen_;
        const auto sample = oxygen.find(c.room);
        // AnaerobicAlien::GetMoveSpeedMultipler 0x46c450: normal speed
        // in oxygen, .85 while not exposed to oxygen (bSuffocating false).
        speed *= sample != oxygen.end() && sample->second > 5.f ? 1.f : .85f;
    }
    float amount = std::min(speed, remaining);
    c.x += (next.point.x - c.x) / remaining * amount;
    c.y += (next.point.y - c.y) / remaining * amount;
    int id = -1;
    for (const auto& r : (enemy ? enemy_.ship : ship_).rooms) {
        if (c.x >= r.x && c.x < r.x + r.w && c.y >= r.y && c.y < r.y + r.h) {
            id = r.id;
            break;
        }
    }
    if (id >= 0) {
        c.room = id;
    }
}

std::map<int, int> Run::connectedRoomDepths(int start) const {
    std::map<int, int> depth;
    std::queue<int> pending;
    depth[start] = 0;
    pending.push(start);
    while (!pending.empty()) {
        const int id = pending.front();
        pending.pop();
        for (const auto& door : ship_.doors) {
            if (!doors_[door.id] || door.roomA < 0 || door.roomB < 0) {
                continue;
            }
            const int other = door.roomA == id ? door.roomB : door.roomB == id ? door.roomA : -1;
            if (other >= 0 && !depth.contains(other)) {
                depth[other] = depth[id] + 1;
                pending.push(other);
            }
        }
    }
    return depth;
}

void Run::updateDoors(bool enemy) {
    const auto& layout = enemy ? enemy_.ship : ship_;
    const auto& systems = enemy ? enemy_.systems : systems_;
    auto& open = enemy ? enemy_.doors : doors_;
    auto& base = enemy ? enemy_.doorBaseHealth : voyageDoorBaseHealth_;
    auto& damage = enemy ? enemy_.doorDamage : voyageDoorDamage_;
    auto& forced = enemy ? enemy_.doorForcedTicks : voyageDoorForcedTicks_;
    auto& levels = enemy ? enemy_.doorLevel : voyageDoorLevel_;
    auto& last = enemy ? enemy_.doorLastBaseHealth : voyageDoorLastBaseHealth_;
    auto& broken = enemy ? enemy_.doorLockdownBroken : voyageDoorLockdownBroken_;
    const auto& locked = enemy ? enemy_.lockdownRooms : lockdownRooms_;
    const auto doors = std::ranges::find_if(systems, [](const auto& sys) {
        return sys.definition.id == 8;
    });
    const int power =
        doors == systems.end()
            ? 0
            : std::clamp(std::min(doors->power, doors->health) + int(doors->operatorActive), 0, 4);
    constexpr std::array<std::array<int, 3>, 3> health{{{12, 8, 6}, {16, 12, 10}, {20, 18, 15}}};
    for (const auto& door : layout.doors) {
        const int id = door.id;
        const bool lockdown = (door.roomA >= 0 && locked.contains(door.roomA)) ||
                              (door.roomB >= 0 && locked.contains(door.roomB));
        auto& previous = base[id];
        if (previous <= 0) {
            previous = 4;
            last[id] = 4;
        }
        if (!lockdown) {
            broken.erase(id);
        }
        if (lockdown && !broken.contains(id)) {
            if (previous != 50) {
                last[id] = previous;
                previous = 50;
                damage[id] = 0;
            }
        } else {
            if (previous == 50) {
                previous = std::max(1, last[id]);
                damage[id] = 0;
            }
            int hack = 0;
            for (const auto& sys : systems) {
                if (sys.hackActive && sys.hackLevel > 0 &&
                    ((sys.definition.id == 8 && sys.hackLevel > 1) ||
                     sys.definition.room == door.roomA || sys.definition.room == door.roomB)) {
                    hack = std::max(hack, sys.hackLevel);
                }
            }
            const int level = hack > 0 ? hack + 2 : power;
            const int desired = hack > 0    ? health[1][std::size_t(std::clamp(difficulty_, 0, 2))]
                                : power < 2 ? 4
                                            : health[std::size_t(std::clamp(power - 2, 0, 2))]
                                                    [std::size_t(std::clamp(difficulty_, 0, 2))];
            last[id] = previous;
            if (!levels.contains(id) || levels[id] != level) {
                const float ratio = std::min((previous - damage[id]) / float(previous), 1.f);
                damage[id] = float(desired - int(ratio * desired));
            }
            previous = desired;
            levels[id] = level;
            if (hack > 0) {
                open[std::size_t(id)] = false;
            }
        }
    }
    for (auto it = forced.begin(); it != forced.end();) {
        // Door's seven-second AnimationTracker ends on a strict > boundary.
        if (--it->second < 0) {
            it = forced.erase(it);
        } else {
            ++it;
        }
    }
}

int Run::doorHackLevel(const DoorDefinition& door, bool enemy) const {
    // ShipManager::OnLoop 0x4a05b0 clears every room's doors, then marks the
    // doors of each room whose system carries a hacking drone with
    // iHackEffect > 0, and every door while the door system is strongly hacked.
    int hack = 0;
    for (const auto& sys : enemy ? enemy_.systems : systems_) {
        if (sys.hackActive && sys.hackLevel > 0 &&
            ((sys.definition.id == 8 && sys.hackLevel > 1) || sys.definition.room == door.roomA ||
             sys.definition.room == door.roomB)) {
            hack = std::max(hack, sys.hackLevel);
        }
    }
    return hack;
}

bool Run::crewDoorSealed(int id, int owner, bool controlled, bool enemy) const {
    const auto& layout = enemy ? enemy_.ship : ship_;
    const auto& door = layout.doors.at(std::size_t(id));
    const auto& locked = enemy ? enemy_.lockdownRooms : lockdownRooms_;
    const auto& broken = enemy ? enemy_.doorLockdownBroken : voyageDoorLockdownBroken_;
    if (!broken.contains(id) && ((door.roomA >= 0 && locked.contains(door.roomA)) ||
                                 (door.roomB >= 0 && locked.contains(door.roomB)))) {
        return true;
    }
    if (controlled) {
        return false;
    }
    const auto& forced = enemy ? enemy_.doorForcedTicks : voyageDoorForcedTicks_;
    const auto& open = enemy ? enemy_.doors : doors_;
    if (forced.contains(id) || open.at(std::size_t(id))) {
        return false;
    }
    const auto& systems = enemy ? enemy_.systems : systems_;
    bool hacked = false;
    int level = 0;
    for (const auto& sys : systems) {
        if (sys.definition.id == 8) {
            level = std::min(sys.power, sys.health) + int(sys.operatorActive);
        }
        hacked =
            hacked || (sys.hackActive && sys.hackLevel > 0 &&
                       ((sys.definition.id == 8 && sys.hackLevel > 1) ||
                        sys.definition.room == door.roomA || sys.definition.room == door.roomB));
    }
    // IsSealed465a90 flips the door's owner while hacked. Ordinary doors
    // without a blast upgrade never impede an intruder.
    return (hacked || level >= 2) && owner != (hacked ? int(!enemy) : int(enemy));
}

void Run::damageDoor(int id, bool enemy) {
    auto& forced = enemy ? enemy_.doorForcedTicks : voyageDoorForcedTicks_;
    if (forced.contains(id)) {
        return;
    }
    auto& damage = enemy ? enemy_.doorDamage : voyageDoorDamage_;
    auto& base = enemy ? enemy_.doorBaseHealth : voyageDoorBaseHealth_;
    auto& last = enemy ? enemy_.doorLastBaseHealth : voyageDoorLastBaseHealth_;
    auto& broken = enemy ? enemy_.doorLockdownBroken : voyageDoorLockdownBroken_;
    auto& open = enemy ? enemy_.doors : doors_;
    doorHitTick_[enemy ? 1 : 0][id] = animation_;
    // ApplyDamage463f30 ignores its float amount and subtracts one per hit.
    damage[id] += 1;
    if (damage[id] >= std::max(1, base[id])) {
        damage[id] = 0;
        forced[id] = 420;
        if (base[id] == 50) {
            broken.insert(id);
        }
        base[id] = std::max(1, last[id]);
        open.at(std::size_t(id)) = true;
    }
}

void Run::lockdownDoors(int roomId, bool enemy) {
    auto& base = enemy ? enemy_.doorBaseHealth : voyageDoorBaseHealth_;
    auto& damage = enemy ? enemy_.doorDamage : voyageDoorDamage_;
    auto& last = enemy ? enemy_.doorLastBaseHealth : voyageDoorLastBaseHealth_;
    auto& broken = enemy ? enemy_.doorLockdownBroken : voyageDoorLockdownBroken_;
    for (const auto& door : (enemy ? enemy_.ship : ship_).doors) {
        if (door.roomA != roomId && door.roomB != roomId) {
            continue;
        }
        broken.erase(door.id);
        if (base[door.id] != 50) {
            last[door.id] = std::max(4, base[door.id]);
        }
        base[door.id] = 50;
        damage[door.id] = 0;
    }
}

bool Run::doorLocked(int id, bool enemy) const {
    if ((enemy ? enemy_.doorForcedTicks : voyageDoorForcedTicks_).contains(id)) {
        return false;
    }
    const auto& ship = enemy ? enemy_.ship : ship_;
    const auto& locked = enemy ? enemy_.lockdownRooms : lockdownRooms_;
    const auto& door = ship.doors.at(std::size_t(id));
    if (enemy && hackingTarget_ >= 0) {
        for (const auto& sys : enemy_.systems) {
            if (sys.definition.id == hackingTarget_ && sys.hackLevel > 0 &&
                ((sys.definition.id == 8 && sys.hackLevel > 1) ||
                 door.roomA == sys.definition.room || door.roomB == sys.definition.room)) {
                return true;
            }
        }
    }
    if (!enemy && enemy_.hackingTarget >= 0) {
        const auto* target = system(enemy_.hackingTarget);
        if (target && target->hackActive && target->hackLevel > 0 &&
            ((target->definition.id == 8 && target->hackLevel > 1) ||
             door.roomA == target->definition.room || door.roomB == target->definition.room)) {
            return true;
        }
    }
    return !(enemy ? enemy_.doorLockdownBroken : voyageDoorLockdownBroken_).contains(id) &&
           ((door.roomA >= 0 && locked.contains(door.roomA)) ||
            (door.roomB >= 0 && locked.contains(door.roomB)));
}

int Run::doorAt(float x, float y) const {
    // Door::Door0x4643d0 stores6x17 vertical/17x6 horizontal dimensions.
    // ContainsPoint0x465e60 expands them by20x10/10x20, inclusive edges.
    const float localX = x - playerX() - ship_.horizontalShift;
    const float localY = y - 170 - ship_.verticalShift;
    for (const auto& door : ship_.doors) {
        const float centerX = static_cast<float>(door.x) + (door.vertical ? 0.f : 17.f);
        const float centerY = static_cast<float>(door.y) + (door.vertical ? 17.f : 0.f);
        const float left = centerX - 13;
        const float top = centerY - 13;
        if (localX >= left && localX <= left + (door.vertical ? 26.f : 27.f) && localY >= top &&
            localY <= top + (door.vertical ? 27.f : 26.f)) {
            return door.id;
        }
    }
    return -1;
}

int Run::selectableDoorAt(float x, float y) const {
    // CrewControl::MouseMove0x4f4380 gives armed weapons/selected crew priority
    // and rejects locked-down, hacked and forced-open doors. Modal UI owns input.
    if (!panel_.empty() || event_ || mapOpen_ || defeated_ || devUiOpen_ || jumpTicks_ > 0 ||
        arrivalTicks_ > 0 || aiming_ >= 0 || dragging_ ||
        std::ranges::any_of(crew_, [](const auto& crew) {
            return crew.selected && crew.health > 0;
        })) {
        return -1;
    }
    const int id = doorAt(x, y);
    if (id < 0 || doorLocked(id) || voyageDoorForcedTicks_.contains(id)) {
        return -1;
    }
    const auto& door = ship_.doors.at(static_cast<std::size_t>(id));
    if (std::ranges::any_of(systems_, [&door](const auto& sys) {
            return sys.hackLevel > 0 &&
                   (sys.definition.room == door.roomA || sys.definition.room == door.roomB);
        })) {
        return -1;
    }
    return id;
}

int Run::doorCursorMode() const {
    const int id = mouseX_ >= 0 ? selectableDoorAt(mouseX_, mouseY_) : -1;
    if (id < 0) {
        return 0;
    }
    // CrewControl::OnLoop0x4f26a0: closed=>SetDoor1(forward), open=>2(reverse).
    return doors_.at(static_cast<std::size_t>(id)) ? 2 : 1;
}

void Run::updatePointer() {
    // CApp::OnLoop 0x4022c0 calls MouseControl::Reset 0x4d7cf0, which clears
    // valid; CommandGui then re-runs its mouse checks at the held position.
    pointerValid_ = false;
    if (mouseX_ < 0) {
        pointerNewHover_ = false;
        crewBoxPointed_ = crewBoxHover_ = -1;
        return;
    }
    move(mouseX_, mouseY_);
    const bool crewControl = panel_.empty() && !mapOpen_ && !defeated_ && !externalModal_ &&
                             !devUiOpen_ && !writeErrorOpen_ && jumpTicks_ == 0 &&
                             (!event_ || arrivalTicks_ > 0);
    if (crewControl) {
        crewBoxPointed_ = updateCrewBoxHover();
    } else {
        crewBoxPointed_ = crewBoxHover_ = -1;
    }
    const bool shipView = panel_.empty() && !mapOpen_ && !defeated_ && !externalModal_ &&
                          !devUiOpen_ && !writeErrorOpen_ &&
                          (!event_ || jumpTicks_ > 0 || arrivalTicks_ > 0);
    // SetSilentValid 0x4d8780 sets valid and lastValid, so it never beeps.
    const bool silent = [&] {
        if (devUiOpen_ || externalModal_ || defeated_) {
            // Native developer UI, hosted options and game-over screen keep
            // the earlier always-valid arrow until their buttons are ported.
            return true;
        }
        if (!shipView) {
            return false;
        }
        if (aiming_ >= 0) {
            // CombatControl::SetMouseCursor 0x55ec00: armed weapon over a target.
            return weaponCursorValid();
        }
        if (!abilityAim_.empty()) {
            // SetMouseCursor: teleport/mind/hack armed with a target room.
            const auto overRoom = [&](const ShipDefinition& ship, float ox, float oy) {
                return std::ranges::any_of(ship.rooms, [&](const auto& room) {
                    return Rect{ox + room.x, oy + room.y, float(room.w), float(room.h)}.contains(
                        mouseX_, mouseY_);
                });
            };
            return (abilityAim_ == "mind" && overRoom(ship_, playerX(), 170.f)) ||
                   (enemy_.present && overRoom(enemy_.ship, enemyOriginX(), enemyOriginY()));
        }
        // CrewControl::OnLoop 0x4f26a0: potential crew (hovered on the ship or
        // in a CrewBox, CrewControl::MouseMove 0x4f4380), or a door while crew
        // are selected.
        bool potential = crewBoxPointed_ >= 0;
        for (const auto& member : crew_) {
            potential = potential || crewPointerHit(member);
        }
        const bool anySelected = std::ranges::any_of(crew_, [](const RunCrew& member) {
            return member.selected;
        });
        return potential || (anySelected && doorCursorMode() > 0);
    }();
    const bool valid = [&] {
        if (writeErrorOpen_) {
            // The save-error ChoiceBox::MouseMove 0x4e82b0 marks its choices.
            return writeErrorHovered_;
        }
        if (devUiOpen_ || externalModal_ || defeated_) {
            return false;
        }
        if (event_ && jumpTicks_ == 0 && arrivalTicks_ == 0) {
            // ChoiceBox::MouseMove 0x4e82b0: a hovered choice whose type != 1.
            return eventChoiceHoverValid_;
        }
        if (mapOpen_) {
            // StarMap::OnLoop 0x5d13a0: potentialLoc or the active end button;
            // its other buttons are GenericButtons.
            return mapHoverSoundBeacon_ >= 0 || !hudHoverSoundTarget_.empty();
        }
        if (!panel_.empty() || !hudHoverSoundTarget_.empty()) {
            // GenericButton::MouseMove 0x573850 and FTLButton::OnRender 0x5428b0.
            return !hudHoverSoundTarget_.empty();
        }
        if (aiming_ >= 0 || !abilityAim_.empty()) {
            return false;
        }
        // SystemBox::OnLoop 0x4d3ea0 / WeaponSystemBox::OnLoop 0x4d40d0 pass
        // ShipSystem::Clickable 0x7222e0, which returns bNeedsPower.
        for (int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15, 3, 4, 6, 7, 8, 12}) {
            if (const auto* source = system(id)) {
                const auto position = systemHudPosition(id);
                if (Rect{position.x + 24,
                         position.y + 8 - 8.f * float(source->definition.power),
                         16,
                         8.f * float(source->definition.power) + 55}
                        .contains(mouseX_, mouseY_)) {
                    return !passive(id);
                }
            }
        }
        // ArmamentControl::OnLoop 0x5573f0: a hovered, non-empty box while
        // nothing is being dragged.
        if (!dragging_ &&
            (armamentAt(mouseX_, mouseY_, false) >= 0 || armamentAt(mouseX_, mouseY_, true) >= 0)) {
            return true;
        }
        // WeaponControl::OnLoop 0x553da0: the active, hovered autofire button.
        return !tutorialOwned_ && ship_.weaponSlots > 2 &&
               autofireRect().contains(mouseX_, mouseY_);
    }();
    // MouseControl::OnRender 0x4d6e00: hoverBeep when valid and either the
    // previous frame was invalid or a button reported a new hover.
    if (valid && ((!pointerLastValid_ && !silent) || pointerNewHover_)) {
        audio_.hover();
    }
    pointerValid_ = valid || silent;
    pointerLastValid_ = pointerValid_;
    pointerNewHover_ = false;
}

void Run::environment() {
    advanceOwnerEnvironment(0);
    if (enemy_.present) {
        advanceOwnerEnvironment(1);
    }
}

void Run::advanceCrewSystem(int owner, std::size_t systemIndex) {
    auto& systems = owner == 0 ? systems_ : enemy_.systems;
    if (systemIndex >= systems.size()) {
        return;
    }
    auto& current = systems[systemIndex];
    const int id = current.definition.id;
    if (id == 2) {
        auto& oxygen = owner == 0 ? roomOxygen_ : enemy_.roomOxygen;
        const auto& ship = owner == 0 ? ship_ : enemy_.ship;
        const auto& doors = owner == 0 ? doors_ : enemy_.doors;
        const auto& forced = owner == 0 ? voyageDoorForcedTicks_ : enemy_.doorForcedTicks;
        const int power = std::clamp(current.power, 0, current.health);
        const float refill = current.hackActive && current.hackLevel > 1 ? -.375f
                             : power > 0 ? .075f * (1 + 3 * (power - 1))
                                         : -.075f;
        float total = 0;
        for (auto& [room, value] : oxygen) {
            (void)room;
            value = std::clamp(value + refill * Time::originalStep, 0.f, 100.f);
            total += value;
        }
        // Oxygen607670 snapshots total BEFORE redistribution and later leaks.
        if (owner == 0) {
            oxygen_ = oxygen.empty() ? 0.f : total / float(oxygen.size());
        }
        std::set<int> visited;
        for (const auto& [start, value] : oxygen) {
            (void)value;
            if (visited.contains(start)) {
                continue;
            }
            std::set<int> group{start};
            std::queue<int> pending;
            pending.push(start);
            while (!pending.empty()) {
                const int room = pending.front();
                pending.pop();
                for (const auto& door : ship.doors) {
                    if (door.roomA < 0 || door.roomB < 0 || door.id < 0 ||
                        std::size_t(door.id) >= doors.size() ||
                        (!doors[std::size_t(door.id)] && !forced.contains(door.id))) {
                        continue;
                    }
                    const int other = door.roomA == room   ? door.roomB
                                      : door.roomB == room ? door.roomA
                                                           : -1;
                    if (other >= 0 && oxygen.contains(other) && group.insert(other).second) {
                        pending.push(other);
                    }
                }
            }
            float mean = 0;
            for (int room : group) {
                visited.insert(room);
                mean += oxygen.at(room);
            }
            mean /= float(group.size());
            for (int room : group) {
                oxygen.at(room) += .005f * Time::originalStep * (mean - oxygen.at(room));
            }
        }
        if (owner == 0) {
            audio_.updateLoop("airLeak", oxygenLeakSound_ ? 1.f : 0.f);
            oxygenLeakSound_ = false;
        }
    } else if (id == 9) {
        auto& charge = owner == 0 ? teleportCharge_ : enemy_.teleportCharge;
        const int power = std::clamp(std::min(current.power, current.health), 0, 3);
        if (power > 0) {
            charge =
                std::min(100.f,
                         charge + std::array<float, 3>{.2f, .35f, .5f}[std::size_t(power - 1)] *
                                      Time::originalStep);
        }
        // Teleport620ee0's non-functioning branch still only tests the target
        // barrier; it does not cancel an armed command merely for zero power.
        if (owner == 0 && teleportArmed_ != 0 && enemy_.superShields > 0 &&
            std::ranges::find(ship_.augments, std::string("ZOLTAN_BYPASS")) ==
                ship_.augments.end()) {
            teleportArmed_ = 0;
        }
    } else if (id == 13) {
        auto& queue = owner == 0 ? cloneQueue_ : enemy_.cloneQueue;
        auto& roster = owner == 0 ? crew_ : enemy_.crew;
        auto& timer = owner == 0 ? cloneTimer_ : enemy_.cloneTimer;
        auto& death = owner == 0 ? cloneDeathTimer_ : enemy_.cloneDeathTimer;
        auto& duration = owner == 0 ? cloneDuration_ : enemy_.cloneDuration;
        auto& pending = owner == 0 ? clonePending_ : enemy_.clonePending;
        const auto& ship = owner == 0 ? ship_ : enemy_.ship;
        const bool backup =
            std::ranges::find(ship.augments, std::string("BACKUP_DNA")) != ship.augments.end();
        const int power = current.hackActive && current.hackLevel > 1
                              ? 0
                              : std::clamp(std::min(current.power, current.health), 0, 3);
        if (queue.empty()) {
            timer = death = -1;
        } else if (power == 0) {
            timer = 0;
            if (death < 0) {
                death = 0;
            }
            if (!backup) {
                death += Time::step;
                if (death > 3.f) {
                    roster.at(std::size_t(queue.back())).cloneReady = false;
                    queue.pop_back();
                    death = -1;
                }
            }
        } else {
            death = -1;
            const float nextDuration = std::array<float, 3>{12, 9, 7}[std::size_t(power - 1)];
            if (duration != nextDuration) {
                timer = duration > 0 ? timer / duration * nextDuration : 0;
                duration = nextDuration;
            }
            timer = std::max(0.f, timer) + Time::step;
            if (timer >= duration) {
                pending = queue.front();
                timer = -1;
            }
        }
        if (owner == 0) {
            audio_.updateLoop("cloneBroken", !queue.empty() && power == 0 && !backup ? 1.f : 0.f);
        }
    }
}

void Run::advanceOwnerEnvironment(int owner) {
    const bool enemy = owner == 1;
    if (enemy && !enemy_.present) {
        return;
    }
    // ShipManager::UpdateEnvironment 0x497f50 starts with Spreader<Fire>::
    // UpdateSpread, whose extinction test reads each tile's Fire::fOxygen as
    // left by the previous pass.
    advanceHazards(owner);
    const auto& ship = enemy ? enemy_.ship : ship_;
    // Both physical ships own their dynamic doors independently.
    const auto doorOpen = [&](int id) {
        const auto& open = enemy ? enemy_.doors : doors_;
        return id >= 0 && std::size_t(id) < open.size() &&
               (open[std::size_t(id)] ||
                (enemy ? enemy_.doorForcedTicks : voyageDoorForcedTicks_).contains(id));
    };
    auto& oxygen = enemy ? enemy_.roomOxygen : roomOxygen_;
    auto& fireOxygen = fireOxygen_[std::size_t(enemy)];
    const auto& hazards = enemy ? enemyHazards_ : playerHazards_;
    const bool hasOxygen =
        std::ranges::any_of(enemy ? enemy_.systems : systems_, [](const auto& sys) {
            return sys.definition.id == 2;
        });
    const auto depths = [&](int start) {
        std::map<int, int> result{{start, 0}};
        std::queue<int> pending;
        pending.push(start);
        while (!pending.empty()) {
            const int id = pending.front();
            pending.pop();
            for (const auto& door : ship.doors) {
                if (!doorOpen(door.id) || door.roomA < 0 || door.roomB < 0) {
                    continue;
                }
                const int other = door.roomA == id   ? door.roomB
                                  : door.roomB == id ? door.roomA
                                                     : -1;
                if (other >= 0 && !result.contains(other)) {
                    result[other] = result.at(id) + 1;
                    pending.push(other);
                }
            }
        }
        return result;
    };
    // OxygenSystem::ComputeAirLoss 0x6070a0: rooms already at or below zero
    // are cut from the loss (their neighbours keep their depth); every other
    // connected room loses base * 0.75^depth. A non-silent loss from a room
    // above one unit raises the airLeak loop for the next OxygenSystem pass.
    const auto airLoss = [&](int start, float base, bool silent) {
        for (const auto& [id, depth] : depths(start)) {
            auto& level = oxygen[id];
            if (level <= 0.f) {
                continue;
            }
            if (level > 1.f && !silent && !enemy) {
                oxygenLeakSound_ = true;
            }
            level -= std::pow(.75f, float(depth)) * (base * Time::originalStep);
        }
    };
    const auto lanius = [&](int roomId) {
        int count = 0;
        const auto inspect = [&](const auto& crew, bool player) {
            for (const auto& member : crew) {
                const bool aboardEnemy = player ? member.intruder : !member.intruder;
                const int room = player && member.intruder ? member.boardRoom : member.room;
                if (member.health > 0 && member.appearance.species == "anaerobic" &&
                    aboardEnemy == enemy && room == roomId) {
                    ++count;
                }
            }
        };
        inspect(crew_, true);
        inspect(enemy_.crew, false);
        if (!enemy) {
            for (const auto& member : voyageBoarders_) {
                if (member.health > 0 && member.appearance.species == "anaerobic" &&
                    member.room == roomId) {
                    ++count;
                }
            }
        }
        return count;
    };
    std::vector<int> roomIds;
    roomIds.reserve(ship.rooms.size());
    for (const auto& room : ship.rooms) {
        roomIds.push_back(room.id);
    }
    std::ranges::sort(roomIds);
    // Rooms are visited in index order; each room finishes its fire, airlock
    // and breach steps before the next room starts.
    for (const int roomId : roomIds) {
        const auto hazard = hazards.find(roomId);
        const int fires = hazard != hazards.end() ? hazard->second.fires : 0;
        const int breaches = hazard != hazards.end() ? hazard->second.breaches : 0;
        if (!hasOxygen) {
            fireOxygen[roomId] = 0;
            continue;
        }
        // ModifyRoomOxygen does not clamp; OxygenSystem::OnLoop clamps later.
        oxygen[roomId] -= float(fires) * (Time::originalStep * .06f);
        fireOxygen[roomId] = int(oxygen[roomId]);
        int airlocks = 0;
        for (const auto& door : ship.doors) {
            if (doorOpen(door.id) && (door.roomA < 0 || door.roomB < 0) &&
                std::max(door.roomA, door.roomB) == roomId) {
                ++airlocks;
            }
        }
        // OxygenSystem::UpdateAirlock 0x607270 empties the source room (with
        // airLoss) while it holds more than ten units, then drains its neighbours.
        if (airlocks > 0) {
            if (oxygen[roomId] > 10.f) {
                oxygen[roomId] = 0;
                combatSound({"airLoss"}, tick_);
            }
            airLoss(roomId, float(airlocks), false);
        }
        // UpdateBreach 0x607210: half a unit per breach and per Lanius, silent
        // when the room has no hull breach.
        const int leaks = breaches + lanius(roomId);
        if (leaks > 0) {
            airLoss(roomId, float(leaks) * .5f, breaches == 0);
        }
    }
    if (!hasOxygen) {
        for (auto& [id, value] : oxygen) {
            (void)id;
            value = 0;
        }
    }
}

void Run::stations(bool save) {
    for (auto& c : crew_) {
        c.selected = false;
    }
    const auto roster = crewRoster();
    if (save) {
        // SaveCrewPositions490440 saves the reserved currentSlot, including
        // while walking. Away members have their saved position cleared.
        for (const auto index : roster) {
            auto& c = crew_[index];
            if (c.intruder) {
                c.savedRoom = c.savedSlot = -1;
                continue;
            }
            c.savedRoom = c.targetRoom;
            c.savedSlot = c.targetSlot;
        }
    } else {
        struct PriorSlot {
            std::size_t index;
            int room;
            int slot;
        };

        std::vector<PriorSlot> unsaved;
        // RestoreCrewPositions497540 removes controlled/away members before
        // emptying every eligible reservation, so return orders do not compete
        // with their own old reservations. Unsaved actors reoccupy that slot
        // after the saved actors have received their orders.
        for (const auto index : roster) {
            auto& c = crew_[index];
            if (c.intruder || c.controlled) {
                continue;
            }
            if (c.savedRoom < 0 || c.savedSlot < 0) {
                unsaved.push_back({index, c.targetRoom, c.targetSlot});
            }
            c.targetRoom = -1;
            c.targetSlot = -1;
        }
        for (const auto index : roster) {
            auto& c = crew_[index];
            if (c.intruder || c.controlled || c.savedRoom < 0 || c.savedSlot < 0) {
                continue;
            }
            order(c, c.savedRoom, c.savedSlot, true);
        }
        for (const auto& prior : unsaved) {
            order(crew_[prior.index], prior.room, prior.slot, true);
        }
    }
    message_ = texts_.get(save ? "stations_saved" : "return_stations", locale_);
    messageTick_ = animation_;
}

void Run::doorControl(bool open) {
    auto* s = system(8);
    if (!s || s->health <= 0 || s->power <= 0) {
        return;
    }
    const auto hacked = [&](const auto& door) {
        return std::ranges::any_of(systems_, [&](const auto& sys) {
            return sys.hackActive && sys.hackLevel > 0 &&
                   ((sys.definition.id == 8 && sys.hackLevel > 1) ||
                    sys.definition.room == door.roomA || sys.definition.room == door.roomB);
        });
    };
    const bool allInternal =
        std::all_of(ship_.doors.begin(), ship_.doors.end(), [&](const auto& d) {
            return d.roomA < 0 || d.roomB < 0 || hacked(d) || doors_[d.id];
        });
    int changed = 0;
    for (const auto& d : ship_.doors) {
        const bool internal = d.roomA >= 0 && d.roomB >= 0;
        if ((!internal || !hacked(d)) && (!open || allInternal || internal)) {
            changed += doors_[d.id] != open;
            doors_[d.id] = open;
        }
    }
    // ManipulateAllDoors483dd0 suppresses the individual transition sounds,
    // then plays doorOpen once per changed door, capped at eight, even closing.
    for (int count = 0; count < std::min(changed, 8); ++count) {
        audio_.sound("doorOpen");
    }
}

void Run::step(std::uint32_t ticks, bool legacy) {
    presentationAlpha_ = 1;
    uiFraction_ = 0;
    uiAlpha_ = 1;
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        if (legacy) {
            uiStep(1);
        }
        previousDoors_ = doorFrames_;
        previousCrew_.clear();
        for (const auto& c : crew_) {
            previousCrew_.push_back({c.x, c.y});
        }
        previousJump_ = jumpTicks_ > 0 ? 2.f - jumpTicks_ * Time::step : arrivalTicks_ * Time::step;
        advanceTick();
        tutorialUpdate();
    }
}

void Run::applicationUiStep(std::uint32_t ticks) {
    for (auto& popup : progressionPopups()) {
        // UnlockShip42ef50 ignores the ordinary achievement-popup setting.
        if (popup.id.empty() || options_.achievementPopups) {
            achievementPopupQueue_.push_back(std::move(popup));
        }
    }
    // AchievementTracker42e630 starts the next popup only at the next UI
    // update after the previous one stops. Preserve that boundary in batches.
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        if (!achievementPopupRunning_ && !achievementPopupQueue_.empty()) {
            combatSound({achievementPopupQueue_.front().id.empty() ? "unlock" : "achievement"},
                        animation_);
            achievementPopupTicks_ = 0;
            achievementPopupRunning_ = true;
        }
        if (achievementPopupRunning_ && ++achievementPopupTicks_ > 180) {
            achievementPopupQueue_.pop_front();
            achievementPopupTicks_ = 0;
            achievementPopupRunning_ = false;
        }
    }
}

void Run::uiStep(std::uint32_t ticks) {
    animation_ += ticks;
    // CommandGui::OnLoop5220b0 skips its ordinary UI children while the
    // extra save-error ChoiceBox is open. Its own key delay still advances.
    if (writeErrorOpen_) {
        return;
    }
    if (renameCrew_ != -1) {
        for (std::uint32_t tick = 0; tick < ticks; ++tick) {
            crewRenameBlink_ += Time::step;
            if (crewRenameBlink_ > 1.f) {
                crewRenameBlink_ = 0;
            }
        }
    }
    if (storeOpenPending_ && docked() && panel_.empty()) {
        storeOpenPending_ = false;
        openStore();
    }
    advanceWeaponSlides(static_cast<float>(ticks) * Time::step);
    // WeaponControl::OnLoop 0x553da0 (every CommandGui frame): without a
    // hostile, non-dying target, each non-empty box calls ClearAiming 0x40c080.
    if (!enemy_.present || !hostile_ || enemy_.hull <= 0 || enemyExplosionActive_) {
        for (auto& weapon : combatWeapons_) {
            if (weapon.targetRoom >= 0) {
                weapon.clearTargets();
            }
        }
    }
    // CrewBox::OnRenderSkillLevel 0x5003e0 updates the skill-up trackers on
    // unscaled frame time, so they also run while paused.
    for (auto& member : crew_) {
        for (auto& remaining : member.skillUpRemaining) {
            remaining = std::max(0.f, remaining - static_cast<float>(ticks) * Time::step);
        }
    }
    advanceArmamentWarnings(static_cast<float>(ticks) * Time::step);
    if (enemy_.escaping && hostile_ && enemy_.hull > 0 && !enemyExplosionActive_) {
        for (std::uint32_t tick = 0; tick < ticks; ++tick) {
            if (enemy_.escapeWarningTime == .66f) {
                enemy_.escapeWarningTime = 0;
            }
            enemy_.escapeWarningTime = std::min(.66f, enemy_.escapeWarningTime + Time::step);
        }
    } else {
        enemy_.escapeWarningTime = 0;
    }
    const auto healthFlash = [&](auto& members) {
        for (auto& member : members) {
            if (crewHealthAlpha(member) > 0) {
                // OnRenderHealth 0x471270 advances a looping one-second tracker.
                // Keep that presentation update out of the native draw path.
                member.healthFlashTime =
                    std::fmod(member.healthFlashTime + ticks * Time::step, 1.f);
            }
        }
    };
    healthFlash(crew_);
    healthFlash(enemy_.crew);
    healthFlash(voyageBoarders_);
    updateResourceFeedback();
    // MouseControl::SetDoor/AnimationTracker::Update source, moved out of drawing.
    // The cursor continues during game pause and ignores gameplay speed.
    const int cursorMode = doorCursorMode();
    if (cursorMode > 0) {
        const bool reverse = cursorMode == 2;
        if (reverse != doorCursorReverse_) {
            doorCursorReverse_ = reverse;
            doorCursorTime_ = reverse ? .65f : 0.f;
        }
        for (std::uint32_t tick = 0; tick < ticks; ++tick) {
            if (doorCursorDelay_ > 0) {
                doorCursorDelay_ -= Time::step;
                continue;
            }
            if ((reverse && doorCursorTime_ == 0) || (!reverse && doorCursorTime_ == .65f)) {
                doorCursorTime_ = reverse ? .65f : 0.f;
                doorCursorDelay_ = .5f;
            }
            doorCursorTime_ += reverse ? -Time::step : Time::step;
            if ((reverse && doorCursorTime_ <= 0) || (!reverse && doorCursorTime_ > .65f)) {
                doorCursorTime_ = reverse ? 0.f : .65f;
                doorCursorDelay_ = .5f;
            }
        }
    }

    // CommandGui::OnLoop 0x5220b0 advances SpaceStatus outside gameplay pause.
    const bool imminent = environment_.pdsActive
                              ? environment_.pdsGoal - environment_.pdsTime < 5.f
                              : (environment_.fireActive || environment_.pulsarActive) &&
                                    environment_.flashGoal - environment_.flashTime < 5.f;
    if (imminent && !environment_.warningActive) {
        environment_.warningUiStart = animation_;
        combatSound({"environWarning"}, animation_);
    }
    environment_.warningActive = imminent;
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        advanceVoyageUi();
    }
    advanceHudUi(ticks);
    if (event_ || userPause_ || mapOpen_ || devUiOpen_) {
        for (std::uint32_t tick = 0; tick < ticks; ++tick) {
            if (nebula_) {
                nebulaScene_.advance(0, {}, &eventRandom_);
            }
            for (std::size_t d = 0; d < doors_.size(); ++d) {
                const float target = (doors_[d] || fakeDoors_[d]) ? 4.f : 0.f;
                doorFrames_[d] += std::clamp(target - doorFrames_[d], -1.f / 3, 1.f / 3);
            }
            for (std::size_t d = 0; d < enemy_.doors.size(); ++d) {
                const float target = (enemy_.doors[d] || enemy_.fakeDoors[d]) ? 4.f : 0.f;
                enemy_.doorFrames[d] +=
                    std::clamp(target - enemy_.doorFrames[d], -1.f / 3, 1.f / 3);
            }
        }
    }
}

void Run::advanceShipState(int owner, bool) {
    auto& engines = owner == 0 ? playerEngineAnimationTimes_ : enemy_.engineAnimationTimes;
    auto& fires = owner == 0 ? playerRoomFireAnimationTimes_ : enemy_.roomFireAnimationTimes;
    auto& doors = owner == 0 ? doors_ : enemy_.doors;
    auto& fake = owner == 0 ? fakeDoors_ : enemy_.fakeDoors;
    auto& timers = owner == 0 ? doorTimers_ : enemy_.doorTimers;
    auto& frames = owner == 0 ? doorFrames_ : enemy_.doorFrames;
    // Ship48a4b0 owns these clocks even in the destroyed manager wrapper.
    for (auto& elapsed : engines) {
        elapsed = Time::loopAnimation(elapsed, .5f);
    }
    for (auto& [room, phases] : fires) {
        (void)room;
        for (auto& elapsed : phases) {
            elapsed = Time::loopAnimation(elapsed, catalog_.animation("fire_small").duration);
        }
    }
    const auto& ship = owner == 0 ? ship_ : enemy_.ship;
    // Ship48a4b0 traverses its internal doors before its separate airlock list.
    for (const bool airlocks : {false, true}) {
        for (std::size_t index = 0; index < doors.size(); ++index) {
            const auto shape = std::ranges::find(ship.doors, int(index), &DoorDefinition::id);
            const bool airlock =
                shape != ship.doors.end() && (shape->roomA < 0 || shape->roomB < 0);
            if (airlock != airlocks) {
                continue;
            }
            if (fake[index]) {
                timers[index] -= Time::step;
                if (timers[index] < 0) {
                    fake[index] = false;
                }
            }
            const float target = doors[index] || fake[index] ? 4.f : 0.f;
            frames[index] += std::clamp(target - frames[index], -1.f / 3, 1.f / 3);
        }
    }
    // Ship48a4b0 then gives every Room its lastO2 from the manager's copy of
    // the oxygen levels; a destroyed manager or a ship without an oxygen
    // system passes an empty vector, leaving zero.
    const bool destroyed = owner == 0 ? playerExplosionStarted_ : enemyExplosionActive_;
    const bool hasOxygen =
        std::ranges::any_of(owner == 0 ? systems_ : enemy_.systems, [](const auto& sys) {
            return sys.definition.id == 2;
        });
    auto& lastOxygen = roomLastOxygen_[std::size_t(owner == 1)];
    lastOxygen.clear();
    for (const auto& [room, level] : owner == 0 ? roomOxygen_ : enemy_.roomOxygen) {
        lastOxygen[room] = !destroyed && hasOxygen ? level : 0.f;
    }
    advanceCombatShipLocks(owner);
    // Ship48a4b0 ends with OuterHull::OnLoop 0x618c40 for every hull tile: with
    // SLUG_GEL each breach takes PartialRepair(0.75, false) (Repairable
    // 0x466f30: damage -= 0.75 * 0.5 per factor unit), and every tile's looping
    // heal Animation advances.
    auto& healElapsed = slugGelHealElapsed_[std::size_t(owner == 1)];
    healElapsed = Time::loopAnimation(healElapsed, 1.f);
    if (std::ranges::find(ship.augments, std::string("SLUG_GEL")) != ship.augments.end()) {
        for (auto& [room, hazard] : owner == 0 ? playerHazards_ : enemyHazards_) {
            (void)room;
            hazard.breachHealth.resize(hazard.breachTiles.size(), 100.f);
            for (std::size_t index = hazard.breachTiles.size(); index-- > 0;) {
                hazard.breachHealth[index] -= .75f * .5f * Time::originalStep;
                if (hazard.breachHealth[index] <= 0) {
                    hazard.breachTiles.erase(hazard.breachTiles.begin() + std::ptrdiff_t(index));
                    hazard.breachHealth.erase(hazard.breachHealth.begin() + std::ptrdiff_t(index));
                }
            }
            hazard.breaches = int(hazard.breachTiles.size());
            hazard.breachRepair =
                hazard.breachHealth.empty() ? 0 : 100 - hazard.breachHealth.front();
        }
    }
}

void Run::advanceCompleteShip(int owner, bool jumping) {
    if (owner == 1 && !enemy_.present) {
        return;
    }
    advanceShipManager(owner, jumping);
    // CompleteShip4094a0 consumes transfers before the next AI decisions.
    if (owner == 0) {
        advanceTeleportDepartures();
        advanceTeleporter();
    } else {
        advanceEnemyTeleporter();
    }
    if ((owner == 0 || hostile_) && (owner == 0 ? hull_ : enemy_.hull) > 0) {
        advanceCrewAi(owner);
    }
    advanceCombatAi(owner);
}

void Run::advanceStatusEffectShips() {
    // WorldManager::CheckStatusEffects 0x5eea10 ends with one extra
    // ShipManager::OnLoop for every ship, enemies first, then the player.
    if (defeated_) {
        return;
    }
    if (enemy_.present) {
        advanceShipManager(1, enemy_.escapeJumpElapsed >= 0);
    }
    advanceShipManager(0, jumpTicks_ > 0 || arrivalTicks_ > 0);
}

void Run::advanceShipManager(int owner, bool jumping) {
    // ShipManager::OnLoop 0x4a05b0: the ship's own systems, crew and hazards,
    // without CompleteShip's transfers and AI.
    if (owner == 1 && !enemy_.present) {
        return;
    }
    const bool destroyed = owner == 0 ? playerExplosionStarted_ : enemyExplosionActive_;
    if (!destroyed) {
        // ShipManager::OnLoop 0x4a05b0 runs the fire suppression pass first.
        advanceFireExtinguishers(owner);
        // Its system pass gives every crew in a room whose system is strongly
        // hacked by a ship owning HACKING_STUN at least 0.2 s of stun.
        const auto& attacker = owner == 0 ? enemy_.ship : ship_;
        if (enemy_.present && std::ranges::find(attacker.augments, std::string("HACKING_STUN")) !=
                                  attacker.augments.end()) {
            for (const auto& sys : owner == 0 ? systems_ : enemy_.systems) {
                if (!sys.hackActive || sys.hackLevel <= 1) {
                    continue;
                }
                const int room = sys.definition.room;
                const auto stun = [&](auto& members, bool player) {
                    for (auto& member : members) {
                        const bool aboard = player ? member.intruder == (owner == 1)
                                                   : member.intruder == (owner == 0);
                        const int at = member.intruder ? member.boardRoom : member.room;
                        if (aboard && !member.outOfGame && at == room) {
                            member.stunTime = std::max(member.stunTime, .2f);
                        }
                    }
                };
                stun(crew_, true);
                stun(enemy_.crew, false);
                if (owner == 0) {
                    for (auto& member : voyageBoarders_) {
                        if (!member.outOfGame && member.room == room) {
                            member.stunTime = std::max(member.stunTime, .2f);
                        }
                    }
                }
            }
        }
        updateReactor(owner == 1);
        updateDoors(owner == 1);
        updateCrewPower(owner);
        auto& systems = owner == 0 ? systems_ : enemy_.systems;
        for (std::size_t index = 0; index < systems.size(); ++index) {
            if (!jumping) {
                advanceCombatSystem(owner, index);
                advanceCrewSystem(owner, index);
                advanceDroneSystem(owner, index);
                advanceAdvancedSystem(owner, index);
            }
            // The same pass (even while jumping) lets a bAutomated ship fix a
            // system that NeedsRepairing 0x613ea0 (damaged, room neither
            // burning nor breached) with PartialRepair(1.0, true) 0x614a80.
            if (owner == 1 && enemyAutomated()) {
                auto& sys = systems[index];
                const auto hazard = enemyHazards_.find(sys.definition.room);
                const bool breached = hazard != enemyHazards_.end() && hazard->second.breaches > 0;
                if (sys.health < sys.definition.power && !sys.onFire && !breached) {
                    (void)partialRepairSystem(sys, 1.f, true, true);
                }
            }
            advanceCombatSystemState(owner, index, jumping);
        }
        if (owner == 0) {
            // ShipManager::OnLoop 0x4a05b0 plays enginesOff when the player's
            // GetDodgeFactor 0x491e20 (without the cloak bonus) falls to zero.
            const int dodge = evasion(0) - (cloaked(0) ? 60 : 0);
            if (lastEngineStatus_ != (dodge > 0) && dodge == 0) {
                audio_.sound("enginesOff");
            }
            lastEngineStatus_ = dodge > 0;
        }
    }
    advanceCombatShipState(owner);
    advanceShipState(owner, jumping);
    if (!destroyed) {
        advanceOwnerEnvironment(owner);
        if (owner == 0) {
            advanceTravel();
            // The status-6 strip advances with CrewMember::OnLoop game time.
            for (auto* group : {&crew_, &enemy_.crew, &voyageBoarders_}) {
                for (auto& member : *group) {
                    member.teleportArrive = std::max(0.f, member.teleportArrive - Time::step);
                    member.teleportLeave = std::max(0.f, member.teleportLeave - Time::step);
                }
            }
        }
        advancePhysicalCrew(owner);
        updateManning(owner);
        advanceSpreadDamage(owner);
        reconcilePower(owner == 1);
    }
}

void Run::advanceTick() {
    if (defeated_) {
        return;
    }
    if (!panel_.empty() || devUiOpen_) {
        return;
    }
    // Arrival presentation continues for a staged ChoiceBox, while that modal
    // already gates the destination's combat and environment simulation.
    const bool animateShips = !externalModal_ && !userPause_ && !mapOpen_ &&
                              (!event_ || jumpTicks_ > 0 || arrivalTicks_ > 0);
    const auto advanceWorld = [&] {
        ++tick_;
        advanceSpace();
        advanceCompleteShip(0, jumpTicks_ > 0 || arrivalTicks_ > 0);
        // Factory45e090 snapshots global names/counts and never ticks actors.
        refreshCrewNames();
        refreshCrewNames();
        if (enemy_.present) {
            const auto cap = [](const RunSystem* sys) {
                return !sys ? 0
                            : std::max(
                                  0, sys->powerLimit > 7 ? sys->definition.power : sys->powerLimit);
            };
            // CountCrew491030 excludes drones and tests bDead, not raw health.
            const bool intruders = std::ranges::any_of(crew_,
                                                       [](const auto& member) {
                                                           return member.intruder &&
                                                                  !member.controlled &&
                                                                  !member.dead && !member.outOfGame;
                                                       }) ||
                                   std::ranges::any_of(enemy_.crew, [](const auto& member) {
                                       return !member.intruder && member.controlled &&
                                              !member.dead && !member.outOfGame;
                                   });
            const bool stalemate =
                (cap(system(1)) <= 0 || cap(system(6)) <= 0 || fuel_ <= 0) && !intruders;
            if (!stalemate || !enemy_.stalemateTrigger) {
                enemy_.stalemateTimer = 0;
            }
            enemy_.stalemateTrigger = stalemate;
            advanceCompleteShip(1, enemy_.escapeJumpElapsed >= 0);
        }
        advanceCombatOutcomes();
        {
            // World5f6520 owns SetSafe after enemy outcomes, before clone births.
            boostFtl_ = !(enemy_.present && hostile_);
            if (boostFtl_) {
                // WorldManager::OnLoop 0x5f704d: SetSafe unless HostileEnvironment.
                if (!hostileEnvironment()) {
                    ftlCharge_ = 85.f;
                    if (auto* teleport = system(9)) {
                        teleportCharge_ = 100.f;
                        setSystemLock(*teleport, 0);
                    }
                    for (int id : {10, 12}) {
                        auto* source = system(id);
                        if (source && source->lockCount > 0) {
                            setSystemLock(*source, 0);
                        }
                    }
                }
                // ClearStatus493d60 takes system IDs, not status-effect types.
                for (int id : {2, 6, 1}) {
                    if (auto* source = system(id)) {
                        source->powerLimit = 8;
                        source->powerLoss = 0;
                        source->powerDivide = 1;
                    }
                }
            }
        }
        if (jumpTicks_ > 0 || arrivalTicks_ > 0) {
            // WorldManager::OnLoop 0x5f6ec1 also restarts choiceHistory and
            // blanks lastMainText when the jump starts.
            selectedVoyageCrewSeed_ = -1;
            choiceHistory_.clear();
            lastMainTextLiteral_ = true;
            lastMainText_.clear();
        }
        advanceCloneReady(1);
        advanceCloneReady(0);
        refreshCrewNames();
        if (!defeated_) {
            updateProgression();
        }
        // The frame's ShipManager::OnRender 0x49abc0 turns resisted crew into RESIST
        // messages after every ship loop, so they are first aged next tick.
        raiseMindResists();
    };
    if (pendingSecretSector_ && !event_ && jumpTicks_ == 0) {
        startSecretSector();
    }
    if (arrivalTicks_ > 0) {
        if (!animateShips) {
            return;
        }
        if (!event_) {
            advanceWorld();
        }
        --arrivalTicks_;
        if (arrivalTicks_ == 0 && event_) {
            resetEventChoiceDelay();
        }
        // The destination is already prepared, but ChoiceBox is presented
        // only once the ship has finished its reverse jump animation.
        return;
    }
    if (jumpTicks_ > 0) {
        if (!animateShips) {
            return;
        }
        --jumpTicks_;
        if (jumpTicks_ == 0) {
            // WorldManager::CheckForNewLocation 0x5f4fa0 clears SpaceManager
            // only after CommandGui::IsJumpComplete, before CreateLocation.
            clearLocation();
            arrivalTicks_ = 120;
            // ShipManager::JumpArrive 0x492ef0 starts the reverse animation
            // and plays the archived jumpArrive cue at this transition.
            audio_.sound("jumpArrive");
            ftlCharge_ = 0;
            nebula_ = graph_.beacons()[graph_.current()].nebula;
            if (nebula_) {
                nebulaScene_.reset(0, environment_.stormActive, &eventRandom_);
            }
            if (campaignArrival()) {
                registerSpaceDrones(0, true);
                return;
            }
            const auto& destination = graph_.beacons()[graph_.current()];
            eventId_ = destination.event;
            if (destination.fleet && destination.eventXml.empty()) {
                // StarMap::TurnIntoFleetLocation 0x5d0940 uses difficulty for
                // exit DLC encounters, but advanced-content mode elsewhere.
                eventId_ = destination.exit     ? "FLEET_EASY_BEACON"
                           : destination.nebula ? "FLEET_EASY_NEBULA"
                                                : "FLEET_EASY";
                if ((destination.exit && difficulty_ != 0) ||
                    (!destination.exit && !destination.nebula && advanced_)) {
                    eventId_ += "_DLC";
                }
                graph_.setEvent(graph_.current(), eventId_, destination.nebula, destination.exit);
                graph_.setStore(graph_.current(), false);
                graph_.setDistress(graph_.current(), false);
            }
            if (destination.eventXml.empty()) {
                graph_.setEventXml(
                    graph_.current(),
                    materializeEvent(events_.event(eventId_),
                                     eventRandom_,
                                     advanced_,
                                     usedVoyageEvents_,
                                     [this](pugi::xml_node node, RandomStream& random) {
                                         freezeVoyageEvent(node, random);
                                     }));
            }
            restoredEvents_.emplace_back();
            if (!restoredEvents_.back().load_string(destination.eventXml.c_str())) {
                throw std::runtime_error("invalid_prepared_beacon_event");
            }
            prepareBeaconImages(restoredEvents_.back().document_element());
            enterEvent(restoredEvents_.back().document_element(), true);
            registerSpaceDrones(0, true);
        } else {
            advanceWorld();
        }
        return;
    }
    if (!animateShips) {
        return;
    }
    advanceWorld();
}

void Run::cancelAim() {
    if (aiming_ >= 0 && aiming_ < int(combatWeapons_.size())) {
        auto& weapon = combatWeapons_[aiming_];
        if (weapon.targetFirstPending ||
            (!weapon.targetPoints.empty() && int(weapon.targetPoints.size()) < weapon.targetGoal)) {
            weapon.targetRoom = -1;
            weapon.targetRoomEnd = -1;
            weapon.targetRooms.clear();
            weapon.targetPoints.clear();
            weapon.targetGoal = 0;
            weapon.queued = false;
        }
        weapon.targetFirstPending = false;
    }
    aiming_ = -1;
}

int Run::armamentAt(float x, float y, bool drone) const {
    const int start = int(systemHudPosition(drone ? 4 : 3).x + 32);
    const int count = drone ? ship_.droneSlots : ship_.weaponSlots;
    for (int index = 0; index < count; ++index) {
        if (Rect{float(start + 16 + 97 * index), 623, 95, 39}.contains(x, y)) {
            return logicalArmamentIndex(drone ? "drone" : "weapon", index);
        }
    }
    return -1;
}

bool Run::swapArmaments(int first, int second, bool drone) {
    const int count = int(drone ? combatDrones_.size() : combatWeapons_.size());
    if (defeated_ || event_ || mapOpen_ || jumpTicks_ > 0 || first < 0 || second < 0 ||
        first >= count || second >= count || first == second) {
        return false;
    }
    // SwapWeapons 0x6100e0 and SwapDrones 0x6132c0 swap complete owned objects.
    // Native parallel slot arrays must move together; in-flight weapons own IDs.
    if (drone) {
        const auto remapAttack = [&](auto& member) {
            if (member.attackTargetOwner == 0 && member.attackTargetDrone &&
                member.attackTargetDroneId == 0) {
                if (member.attackTargetIndex == first) {
                    member.attackTargetIndex = second;
                } else if (member.attackTargetIndex == second) {
                    member.attackTargetIndex = first;
                }
            }
        };
        for (auto& member : crew_) {
            remapAttack(member);
        }
        for (auto& member : enemy_.crew) {
            remapAttack(member);
        }
        for (auto& member : voyageBoarders_) {
            remapAttack(member);
        }
        for (auto& member : combatDrones_) {
            remapAttack(member);
        }
        for (auto& member : enemy_.drones) {
            remapAttack(member);
        }
        std::swap(combatDrones_[first], combatDrones_[second]);
        std::swap(ship_.drones[first], ship_.drones[second]);
        const auto remap = [&](std::uint64_t& target) {
            if (target == std::uint64_t(first) + 1) {
                target = std::uint64_t(second) + 1;
            } else if (target == std::uint64_t(second) + 1) {
                target = std::uint64_t(first) + 1;
            }
        };
        for (auto& projectile : projectiles_) {
            if (projectile.owner == 1) {
                remap(projectile.defenseDroneTargetId);
            }
        }
        for (auto& defender : enemy_.drones) {
            remap(defender.targetDrone);
        }
    } else {
        std::swap(combatWeapons_[first], combatWeapons_[second]);
        std::swap(ship_.weapons[first], ship_.weapons[second]);
        const bool powered = weaponPower_[first];
        weaponPower_[first] = weaponPower_[second];
        weaponPower_[second] = powered;
        const bool desired = weaponDesired_[first];
        weaponDesired_[first] = weaponDesired_[second];
        weaponDesired_[second] = desired;
        if (aiming_ == first) {
            aiming_ = second;
        } else if (aiming_ == second) {
            aiming_ = first;
        }
    }
    return true;
}

void Run::dragEnd(float x, float y) {
    if (devUiOpen_) {
        logging::verbose("pointer", "drag end -> developer UI");
        dragging_ = false;
        devUiClick(x, y);
        return;
    }
    if (!panel_.empty()) {
        dragging_ = false;
        if (!inventoryDragKind_.empty()) {
            logging::verbose("pointer",
                             "drag end -> inventory drop '{}' #{}",
                             inventoryDragKind_,
                             inventoryDragIndex_);
            inventoryPointerUp(x, y);
            return;
        }
        // CommandGui::LButtonDown 0x521430 gives the tutorial overlay first
        // refusal before dispatching an open ship panel.
        click(x, y);
        return;
    }
    if (!dragging_) {
        logging::verbose("pointer", "drag end ignored: no drag in progress");
        return;
    }
    dragging_ = false;
    if (event_ || mapOpen_ || jumpTicks_ > 0) {
        click(x, y);
        return;
    }
    const bool armamentDrag = armamentAt(dragStart_.x, dragStart_.y, false) >= 0 ||
                              armamentAt(dragStart_.x, dragStart_.y, true) >= 0;
    // Armament558970 uses integer Point::Distance >6; CrewControl4f3e00
    // uses squared RelativeDistance >49 for the selection rectangle.
    const float travelled = distance(dragStart_, {x, y});
    if (armamentDrag ? int(travelled) <= 6 : travelled <= 7) {
        click(x, y);
        return;
    }
    logging::verbose("pointer",
                     "drag end: moved {:.1f}px from ({:.1f},{:.1f}), over the {}px limit; treated "
                     "as {} instead of a click",
                     travelled,
                     dragStart_.x,
                     dragStart_.y,
                     armamentDrag ? 6 : 7,
                     armamentDrag ? "armament drag" : "crew selection box");
    for (bool drone : {true, false}) {
        const int source = armamentAt(dragStart_.x, dragStart_.y, drone);
        if (source >= 0) {
            ensureInventorySlots();
            const std::string kind = drone ? "drone" : "weapon";
            const int sourceSlot = logicalArmamentSlot(kind, source);
            const int start = int(systemHudPosition(drone ? 4 : 3).x + 32);
            const int count = drone ? ship_.droneSlots : ship_.weaponSlots;
            for (int slot = 0; slot < count; ++slot) {
                if (Rect{float(start + 16 + 97 * slot), 623, 95, 39}.contains(x, y) &&
                    sourceSlot >= 0) {
                    const int target = logicalArmamentIndex(kind, slot);
                    if (target >= 0) {
                        swapArmaments(source, target, drone);
                    }
                    ensureInventorySlots();
                    break;
                }
            }
            return;
        }
    }
    Rect selection{std::min(x, dragStart_.x),
                   std::min(y, dragStart_.y),
                   std::abs(x - dragStart_.x),
                   std::abs(y - dragStart_.y)};
    for (auto& c : crew_) {
        const auto point = c.intruder ? crewCombatPoint(c) : SDL_FPoint{c.x, c.y};
        const float originX = c.intruder ? enemyOriginX() : playerX() + ship_.horizontalShift;
        const float originY = c.intruder ? enemyOriginY() : 170.f + ship_.verticalShift;
        c.selected =
            c.health > 0 && !c.controlled &&
            (!c.intruder || (enemy_.present && c.boardPositionValid)) &&
            ((shift_ && c.selected) || selection.contains(point.x + originX, point.y + originY));
    }
}

void Run::click(float x, float y, bool right) {
    logging::ClickTrace trace("run", x, y, right);
    if (writeErrorOpen()) {
        trace.hit("write-error dialog");
        if (!right) {
            writeErrorClick(x, y);
        }
        return;
    }
    if (tutorialClick(x, y, right)) {
        trace.hit("tutorial continue");
        return;
    }
    if (devUiOpen_) {
        trace.hit("developer UI");
        if (!right) {
            devUiClick(x, y);
        }
        return;
    }
    if (!panel_.empty()) {
        const auto target = panelHoverSoundTarget(x, y);
        trace.hit("panel '" + panel_ + "'" +
                  (target.empty() ? std::string(" (no button under pointer)")
                                  : " button '" + target + "'") +
                  (!right || panel_ == "upgrades" ? "" : " (right click ignored)"));
        if (!right || panel_ == "upgrades") {
            voyageClick(x, y, right);
        }
        return;
    }
    if (!event_ && !mapOpen_ && jumpTicks_ == 0 && !defeated_ &&
        (Rect{625, 22, 74, 55}.contains(x, y) || hudOptionsRect().contains(x, y) ||
         (hudStoreAvailable() && hudStoreRect().contains(x, y)))) {
        trace.hit("HUD panel button");
        voyageAction(Rect{625, 22, 74, 55}.contains(x, y) ? "panel:ship"
                     : hudOptionsRect().contains(x, y)    ? "panel:pause"
                                                          : "panel:store",
                     "click");
        return;
    }
    move(x, y);
    if (defeated_) {
        trace.hit("game over screen (no button under pointer)");
        if (!right) {
            for (int i = 0; i < 5; ++i) {
                if (gameOverButton(i).contains(x, y)) {
                    trace.hit("game over button " + std::to_string(i));
                    gameOverCommand_ = i;
                    return;
                }
            }
        }
        return;
    }
    if (jumpTicks_ > 0 || arrivalTicks_ > 0) {
        trace.hit(std::format("blocked by jump/arrival animation (jump {} arrival {} ticks)",
                              jumpTicks_,
                              arrivalTicks_));
        return;
    }
    if (mapOpen_) {
        if (distressWaitTicks_ > 0) {
            trace.hit("map blocked by distress wait");
            return;
        }
        trace.hit(
            right ? std::string("map (right click ignored)")
                  : std::format("map beacon {} (current {})", hoveredBeacon(), graph_.current()));
        if (!right) {
            if (canWaitAtBeacon() && Rect{730, 624, 100, 30}.contains(x, y)) {
                voyageAction("distress:wait", "click");
            } else if (fuel_ == 0 && Rect{956, 624, 116, 30}.contains(x, y)) {
                voyageAction("distress:toggle", "click");
            } else if (fuel_ > 0 && graph_.beacons()[graph_.current()].exit && sector_ < 8 &&
                       nextSectorButtonRect().contains(x, y)) {
                trace.hit("map next sector");
                mapOpen_ = false;
                panel_ = "sector";
                // StarMap::MouseClick 0x5ca380 clears potentialSectorChoice.
                sectorPotential_ = -1;
            } else if (Rect{932, 619, 130, 36}.contains(x, y)) {
                trace.hit(tutorialOwned_ ? "map cancel (disabled in tutorial)" : "map cancel");
                // StarMap::MouseClick5ca380 disables Cancel on tutorial-generated maps.
                if (!tutorialOwned_) {
                    mapOpen_ = false;
                }
            } else {
                jump(hoveredBeacon());
            }
        }
        return;
    }
    if (event_) {
        trace.hit("event dialog (no choice under pointer)");
        if (!right) {
            for (std::size_t index = 0; index < choices_.size(); ++index) {
                if (Rect{enemy_.present ? 202.f : 352.f,
                         choiceRow(index) - 10,
                         565,
                         choiceHeight(index)}
                        .contains(x, y)) {
                    trace.hit(std::format(
                        "event choice {}{}", index, choices_[index].enabled ? "" : " (disabled)"));
                    choose(static_cast<int>(index));
                    return;
                }
            }
        }
        return;
    }
    // CrewControl::MouseMove0x4f4380 determines selectedDoor before input.
    // Preserve armed priority even when right-click cancels aiming below.
    if (!abilityAim_.empty()) {
        trace.hit("ability aim '" + abilityAim_ + "' (no target room under pointer)");
        if (right) {
            trace.hit("ability aim '" + abilityAim_ + "' cancelled");
            abilityAim_.clear();
            // CombatControl::MouseRClick568060 disarms first, then processes
            // both armament controls while consuming the prior armed click.
            (void)combatClick(x, y, true);
            return;
        }
        if (abilityAim_ == "mind") {
            for (const auto& shape : ship_.rooms) {
                if (Rect{playerX() + shape.x, 170.f + shape.y, float(shape.w), float(shape.h)}
                        .contains(x, y)) {
                    trace.hit("mind control player room " + std::to_string(shape.id));
                    advancedAction("mind:player-room:" + std::to_string(shape.id), "click");
                    abilityAim_.clear();
                    return;
                }
            }
        }
        for (const auto& room : enemy_.ship.rooms) {
            if (!Rect{
                    enemyOriginX() + room.x, enemyOriginY() + room.y, float(room.w), float(room.h)}
                     .contains(x, y)) {
                continue;
            }
            trace.hit("ability '" + abilityAim_ + "' enemy room " + std::to_string(room.id));
            if (abilityAim_ == "teleport_send" || abilityAim_ == "teleport_return") {
                advancedAction(std::string(abilityAim_ == "teleport_send" ? "teleport:send:"
                                                                          : "teleport:return:") +
                                   std::to_string(room.id),
                               "click");
            } else if (abilityAim_ == "hack") {
                for (const auto& source : enemy_.systems) {
                    if (source.definition.room == room.id) {
                        advancedAction("hack:" + std::to_string(source.definition.id), "click");
                        break;
                    }
                }
            } else {
                for (std::size_t index = 0; index < enemy_.crew.size(); ++index) {
                    const auto& crew = enemy_.crew[index];
                    if (crew.room == room.id && crew.health > 0 &&
                        crew.appearance.species != "slug") {
                        advancedAction("mind:" + std::to_string(index), "click");
                        break;
                    }
                }
            }
            abilityAim_.clear();
            updateProgression();
            return;
        }
        return;
    }
    const int selectedDoor = selectableDoorAt(x, y);
    const int aimBefore = aiming_;
    if (combatClick(x, y, right)) {
        trace.hit(std::format("combat control (weapon aim {} -> {})", aimBefore, aiming_));
        return;
    }
    if (!right && Rect{524, 22, 99, 50}.contains(x, y)) {
        trace.hit("jump button (open map)");
        openMap();
        return;
    }
    if (!right) {
        const auto living = crewRoster();
        for (std::size_t i = 0; i < living.size(); ++i) {
            if (Rect{10, 155.f + i * 30, 86, 27}.contains(x, y)) {
                const auto& listed = crew_[living[i]];
                trace.hit(std::format(
                    "crew list #{} '{}'{}",
                    i,
                    listed.appearance.name,
                    listed.controlled || listed.dead || listed.dying ? " (unselectable)" : ""));
                // SelectPotentialCrew 0x4f2b90 rejects mind-controlled crew.
                if (crew_[living[i]].controlled || crew_[living[i]].dead ||
                    crew_[living[i]].dying) {
                    return;
                }
                bool prior = crew_[living[i]].selected;
                if (!shift_) {
                    for (auto& c : crew_) {
                        c.selected = false;
                    }
                }
                crew_[living[i]].selected = shift_ ? !prior : true;
                return;
            }
        }
        float stationY = 154.f + living.size() * 30;
        if (Rect{11, stationY - 4, 40, 40}.contains(x, y)) {
            trace.hit("save stations");
            stations(true);
            return;
        }
        if (Rect{52, stationY - 4, 40, 40}.contains(x, y)) {
            trace.hit("return to stations");
            stations(false);
            return;
        }
    }
    for (int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15, 3, 4}) {
        if (const auto* source = system(id)) {
            const auto position = systemHudPosition(id);
            const Rect hit{position.x + 24,
                           position.y + 8 - 8.f * source->definition.power,
                           16,
                           8.f * source->definition.power + 55};
            if (hit.contains(x, y)) {
                trace.hit(std::format("system {} power {}", id, right ? "down" : "up"));
                power(id, !right);
                return;
            }
        }
    }
    const int sx = int(systemHudPosition(3).x + 32);
    if (!right) {
        for (int id : {9, 14, 15}) {
            const auto* source = system(id);
            if (!source) {
                continue;
            }
            const auto position = systemHudPosition(id);
            if (id == 9) {
                const SDL_FPoint button{position.x + 35, position.y - 36};
                const bool send = Rect{button.x + 10, button.y + 10, 20, 20}.contains(x, y);
                const bool receive = Rect{button.x + 10, button.y + 33, 20, 20}.contains(x, y);
                if (send || receive) {
                    trace.hit(send ? "teleporter send" : "teleporter return");
                    const bool locked = systemLocked(*source);
                    if (!locked && source->health > 0 && source->power > 0 && enemy_.present &&
                        !enemyExplosionActive_ && (!cloaked(1) || enemyContainsPlayerCrew()) &&
                        (!send || hostile_)) {
                        abilityAim_ = send ? "teleport_send" : "teleport_return";
                    }
                    return;
                }
            } else if (abilityButtonRect(id).contains(x, y)) {
                trace.hit(std::format("ability button system {}", id));
                if (id == 15 && hackingTarget_ >= 0) {
                    advancedAction("hack:" + std::to_string(hackingTarget_), "click");
                } else if (source->health > 0 && source->power > 0 && !systemLocked(*source) &&
                           enemy_.present && !enemyExplosionActive_ &&
                           (!cloaked(1) || enemyContainsPlayerCrew())) {
                    abilityAim_ = id == 14 ? "mind" : "hack";
                }
                return;
            }
        }
    }
    for (int i = 0; i < ship_.weaponSlots; ++i) {
        if (Rect{float(sx + 16 + 97 * i), 623, 95, 39}.contains(x, y)) {
            const int dense = logicalArmamentIndex("weapon", i);
            trace.hit(std::format("weapon slot {} (weapon {})", i, dense));
            if (dense >= 0) {
                selectWeapon(dense, right);
            }
            return;
        }
    }
    if (system(8)) {
        const auto position = systemHudPosition(8);
        const SDL_FPoint button{position.x + 35, position.y - 36};
        if (Rect{button.x + 10, button.y + 10, 20, 20}.contains(x, y)) {
            trace.hit("open all doors");
            if (!right) {
                doorControl(false);
            }
            return;
        }
        if (Rect{button.x + 10, button.y + 33, 20, 20}.contains(x, y)) {
            trace.hit("close all doors");
            if (!right) {
                doorControl(true);
            }
            return;
        }
    }
    // The passive subsystem backing owns its input extent after localization.
    if (Rect{1022, 655, 170.f + 3 * subsystemHudSpacing(), 44}.contains(x, y)) {
        trace.hit("subsystem HUD backing (absorbs click)");
        return;
    }
    float localX = x - playerX() - ship_.horizontalShift;
    float localY = y - 170 - ship_.verticalShift;
    const int hoveredDoor = selectedDoor;
    if (hoveredDoor >= 0) {
        trace.hit("door " + std::to_string(hoveredDoor));
        auto* doors = system(8);
        if (doors && doors->health > 0 && doors->power > 0 && !doorLocked(hoveredDoor) &&
            !voyageDoorForcedTicks_.contains(hoveredDoor)) {
            doors_[static_cast<std::size_t>(hoveredDoor)] =
                !doors_[static_cast<std::size_t>(hoveredDoor)];
        }
        return;
    }
    if (enemy_.present) {
        const float foreignX = x - enemyOriginX();
        const float foreignY = y - enemyOriginY();
        const auto target = std::ranges::find_if(enemy_.ship.rooms, [&](const auto& shape) {
            return Rect{float(shape.x), float(shape.y), float(shape.w), float(shape.h)}.contains(
                foreignX, foreignY);
        });
        if (target != enemy_.ship.rooms.end()) {
            trace.hit("enemy room " + std::to_string(target->id));
            if (right) {
                const int slot = int((foreignY - target->y) / 35) * (target->w / 35) +
                                 int((foreignX - target->x) / 35);
                for (auto& member : crew_) {
                    if (member.selected && member.intruder) {
                        order(member, target->id, slot, true);
                    }
                }
            } else {
                RunCrew* selected = nullptr;
                for (auto& member : crew_) {
                    const auto point = crewCombatPoint(member);
                    if (member.intruder && member.boardPositionValid && member.health > 0 &&
                        !member.controlled && std::abs(point.x - foreignX) < 14 &&
                        std::abs(point.y - foreignY) < 14) {
                        selected = &member;
                        break;
                    }
                }
                const bool prior = selected && selected->selected;
                if (!shift_) {
                    for (auto& member : crew_) {
                        member.selected = false;
                    }
                }
                if (selected) {
                    selected->selected = shift_ ? !prior : true;
                }
            }
            return;
        }
    }
    if (right) {
        int destination = roomAt(localX, localY);
        trace.hit(destination >= 0 ? "move crew to room " + std::to_string(destination)
                                   : std::string("right click outside rooms"));
        if (destination >= 0) {
            for (auto& c : crew_) {
                if (c.selected && !c.intruder) {
                    order(c, destination);
                }
            }
        }
        return;
    }
    int selected = -1;
    for (int i = 0; i < int(crew_.size()); ++i) {
        if (crew_[i].health > 0 && !crew_[i].controlled && !crew_[i].intruder &&
            std::abs(crew_[i].x - localX) < 14 && std::abs(crew_[i].y - localY) < 14) {
            selected = i;
            break;
        }
    }
    bool prior = selected >= 0 && crew_[selected].selected;
    trace.hit(selected >= 0 ? "crew '" + crew_[selected].appearance.name + "' on ship"
                            : std::string("empty space (crew deselected)"));
    if (!shift_) {
        for (auto& c : crew_) {
            c.selected = false;
        }
    }
    if (selected >= 0) {
        crew_[selected].selected = shift_ ? !prior : true;
    }
}

void Run::options(const RunOptions& options) {
    options_ = options;
}

void Run::key(const std::string& key) {
    if (key == "Release:Hotkey:force_autofire") {
        forceAutofire_ = false;
        return;
    }
    if (key == "Hotkey:force_autofire" || key == "Shift+Hotkey:force_autofire") {
        forceAutofire_ = true;
        return;
    }
    if (key == "F12") {
        devUiOpen_ = !devUiOpen_;
        devUiFocus_.clear();
        dragging_ = false;
        return;
    }
    if (devUiOpen_) {
        devUiKey(key);
        return;
    }
    const bool shiftedHotkey = key.starts_with("Shift+Hotkey:");
    const std::string action = shiftedHotkey                ? key.substr(13)
                               : key.starts_with("Hotkey:") ? key.substr(7)
                                                            : "";
    if (action == "pause") {
        // CApp::OnKeyDown 0x401f80 runs CommandGui::SpaceBar 0x518c40 for
        // the pause hotkey before CommandGui::KeyDown, so it also toggles
        // over open windows, including the write-error box and a crew name
        // being edited. KeyDown then still offers the key to that box.
        if (!defeated_ && jumpTicks_ == 0 && arrivalTicks_ == 0) {
            userPause_ = !userPause_;
        }
        (void)writeErrorKey(key);
        return;
    }
    if (writeErrorKey(key)) {
        return;
    }
    if (!action.empty()) {
        if (action == "increase_font" || action == "decrease_font") {
            const bool increase = action == "increase_font";
            eventFontSize_ = std::clamp(increase ? (eventFontSize_ == 10 ? 12 : eventFontSize_ + 1)
                                                 : (eventFontSize_ == 12 ? 10 : eventFontSize_ - 1),
                                        9,
                                        13);
            return;
        }
        if (action == "ship_inv" || action == "ship_crew" || action == "store") {
            voyageAction(action == "ship_inv"    ? "panel:equipment"
                         : action == "ship_crew" ? "panel:crew"
                                                 : "panel:store",
                         "click");
            return;
        }
        if (!panel_.empty() || event_ || mapOpen_ || jumpTicks_ > 0 || defeated_) {
            return;
        }
        if (action == "send_tele" || action == "ret_tele" || action == "start_hacking" ||
            action == "mindControl") {
            const int id = action == "send_tele" || action == "ret_tele" ? 9
                           : action == "start_hacking"                   ? 15
                                                                         : 14;
            const auto* source = system(id);
            if (!source || source->health <= 0 || source->power <= 0 || systemLocked(*source) ||
                !enemy_.present || enemyExplosionActive_ ||
                (cloaked(1) && !enemyContainsPlayerCrew()) ||
                (action == "send_tele" && !hostile_)) {
                return;
            }
            if (id == 15 && hackingTarget_ >= 0) {
                advancedAction("hack:" + std::to_string(hackingTarget_), "click");
                updateProgression();
                return;
            }
            abilityAim_ = action == "send_tele"       ? "teleport_send"
                          : action == "ret_tele"      ? "teleport_return"
                          : action == "start_hacking" ? "hack"
                                                      : "mind";
            cancelAim();
            return;
        }
        if (action == "lockdown") {
            for (auto& crew : crew_) {
                if (crew.selected && crew.health > 0 && !crew.controlled &&
                    crew.appearance.species == "crystal" && crew.crystalPowerElapsed == 50) {
                    crew.crystalPowerElapsed = 0;
                    auto& rooms = crew.boardRoom >= 0 ? enemy_.lockdownRooms : lockdownRooms_;
                    const int room = crew.boardRoom >= 0 ? crew.boardRoom : crew.room;
                    if (room >= 0) {
                        rooms[room] = 12;
                        lockdownDoors(room, crew.boardRoom >= 0);
                    }
                    combatSound({"lockdown1", "lockdown2"}, tick_);
                }
            }
            updateProgression();
            return;
        }
        if (action.size() == 6 && action.starts_with("drone") && action.back() >= '1' &&
            action.back() <= '3') {
            const int dense = logicalArmamentIndex("drone", action.back() - '1');
            if (dense >= 0) {
                toggleDrone(dense);
            }
            return;
        }
        const bool explicitUnpower = action.starts_with("un_");
        const auto name = explicitUnpower ? action.substr(3) : action;
        const bool increase = !explicitUnpower && !shiftedHotkey;
        if (name.starts_with("power_") && name.size() == 7 && name.back() >= '1' &&
            name.back() <= '8') {
            int slot = 0;
            // SystemControl4b9020 indexes rendered system boxes, skipping absent systems.
            for (int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15, 3, 4}) {
                if (system(id) && ++slot == name.back() - '0') {
                    power(id, increase);
                    break;
                }
            }
            return;
        }
        for (const auto& [label, id] : std::map<std::string, int>{{"teleporter", 9},
                                                                  {"cloaking", 10},
                                                                  {"mind", 14},
                                                                  {"hacking", 15},
                                                                  {"artillery", 11},
                                                                  {"weapons", 3},
                                                                  {"drones", 4}}) {
            if (name == label) {
                power(id, increase);
                return;
            }
        }
        return;
    }
    if (!panel_.empty()) {
        if (key == "Escape" && tutorialOwned_ &&
            (panel_ == "upgrades" || panel_ == "crew" || panel_ == "equipment")) {
            // CommandGui::KeyDown 0x5201f0 toggles the MenuScreen during the
            // tutorial and leaves the ship screen (and its delete dialog)
            // open; RenderStatic 0x520a70 draws only the first open focus
            // window, the menu. A rename's TEXT_CANCEL still deactivates the
            // input, so OnRender 0x52f410 commits it.
            if (renameCrew_ != -1) {
                crewRenameKey(key);
            }
            pausedShipPanel_ = panel_;
            pausedDismissCrew_ = std::exchange(dismissCrew_, -1);
            panel_ = "pause";
            cancelAim();
            return;
        }
        if (renameCrew_ != -1) {
            // windows_handle_input_message 0x680d60 sends Escape's key-down
            // before TEXT_CANCEL. Outside the tutorial CommandGui::KeyDown
            // 0x5201f0 closes the ship screen, and CrewManifest::Close 0x4db3a0
            // -> CrewEquipBox::CloseRename 0x52ce30 drops the edit before the
            // OnRender 0x52f410 commit. A locked manifest stays open.
            if (key == "Escape") {
                renameCrew_ = -1;
                voyageAction("panel:close", "click");
                return;
            }
            crewRenameKey(key);
            return;
        }
        if (key == "Escape") {
            if (dismissCrew_ != -1) {
                // CommandGui's own ConfirmWindow (+16372) is the leave-crew
                // jump prompt, so Escape closes the ship screen and
                // CrewManifest::Close 0x4db3a0 closes its delete dialog.
                dismissCrew_ = -1;
                voyageAction("panel:close", "click");
            } else if (pendingStoreOffer_ >= 0) {
                // CommandGui::KeyDown 0x5201f0 closes the store's TabbedWindow;
                // Store::Close 0x4b2860 drops confirmBuy along with it.
                pendingStoreOffer_ = -1;
                voyageAction("panel:close", "click");
            } else {
                voyageAction("panel:close", "click");
            }
        }
        return;
    }
    if (!event_ && !mapOpen_ && jumpTicks_ == 0 && !defeated_ &&
        (key == "U" || (key == "Escape" && aiming_ < 0 && abilityAim_.empty()))) {
        voyageAction(key == "U" ? "panel:ship" : "panel:pause", "click");
        return;
    }
    if (defeated_) {
        return;
    }
    if (key == "Space") {
        if (jumpTicks_ == 0 && arrivalTicks_ == 0) {
            userPause_ = !userPause_;
        }
        return;
    }
    if (jumpTicks_ > 0 || arrivalTicks_ > 0) {
        return;
    }
    if (mapOpen_) {
        if (distressWaitTicks_ > 0) {
            return;
        }
        if (key == "Escape" && !tutorialOwned_) {
            // CommandGui::KeyDown5201f0 keeps the tutorial map open until a jump.
            mapOpen_ = false;
        }
        return;
    }
    if (event_) {
        // ChoiceBox::KeyDown4e85e0: mode2 waits .3s, mode1 is immediate.
        if (options_.dialogKeys != 0 &&
            (options_.dialogKeys != 2 || animation_ - eventChoiceOpenedUiTick_ >= 18) &&
            key.size() == 1 && key[0] >= '1' && key[0] <= '9') {
            choose(key[0] - '1');
        }
        return;
    }
    if (key == "Escape" && !abilityAim_.empty()) {
        abilityAim_.clear();
        return;
    }
    if (key == "Escape" && aiming_ >= 0) {
        cancelAim();
        return;
    }
    if (key == "C" || key == "B") {
        activateAbility(key == "C" ? 10 : 12);
        return;
    }
    if (key == "V") {
        setAutofire(!autofire_);
        return;
    }
    if (key == "J") {
        openMap();
        return;
    }
    bool shifted = key.starts_with("Shift+");
    std::string k = shifted ? key.substr(6) : key;
    if (k.size() == 2 && k[0] == 'F' && k[1] >= '1' && k[1] <= '8') {
        int i = k[1] - '1';
        const auto living = crewRoster();
        if (i < int(living.size())) {
            if (crew_[living[i]].controlled || crew_[living[i]].dead || crew_[living[i]].dying) {
                return;
            }
            bool previous = crew_[living[i]].selected;
            if (!shifted) {
                for (auto& c : crew_) {
                    c.selected = false;
                }
            }
            crew_[living[i]].selected = shifted ? !previous : true;
        }
        return;
    }
    if (k == "Q") {
        for (auto& c : crew_) {
            c.selected = c.health > 0 && !c.controlled && !c.dead && !c.dying;
        }
        return;
    }
    if (k == "Enter") {
        stations(false);
        return;
    }
    if (k == "Slash") {
        stations(true);
        return;
    }
    if (k == "Z") {
        doorControl(true);
        return;
    }
    if (k == "X") {
        doorControl(false);
        return;
    }
    // Read-only original hotkeys-decoded.json: A/S/D/F.
    for (auto [name, id] : {std::pair{"S", 1}, {"A", 0}, {"D", 5}, {"F", 2}}) {
        if (k == name) {
            // SystemBox::KeyDown0x4cdf50 aliases clonebay to medbay.
            power(id == 5 && !system(5) ? 13 : id, !shifted);
            return;
        }
    }
    if (k.size() == 1 && k[0] >= '1' && k[0] <= '4') {
        const int i = logicalArmamentIndex("weapon", k[0] - '1');
        if (i >= 0 && i < int(weaponPower_.size())) {
            selectWeapon(i, shifted);
        }
    }
}

void Run::semantic(const std::string& target, const std::string& action) {
    if (writeErrorOpen()) {
        if (target == "save_error:continue" && action == "click") {
            const auto choice = writeErrorChoiceRect();
            writeErrorClick(choice.x + choice.w / 2, choice.y + choice.h / 2);
        }
        return;
    }
    if (target == "tutorial:continue" && action == "click" && tutorialOwned_) {
        const auto button = tutorialContinueRect();
        tutorialClick(button.x + button.w / 2, button.y + button.h / 2, false);
        return;
    }
    if (target.starts_with("dev:")) {
        if (action != "click") {
            throw std::runtime_error("unsupported_input_action");
        }
        devUiAction(target.substr(4));
        return;
    }
    if (voyageAction(target, action) || advancedAction(target, action)) {
        return;
    }
    if (!panel_.empty()) {
        return;
    }
    if ((target == "cloak" || target == "battery") && action == "click") {
        activateAbility(target == "cloak" ? 10 : 12);
        return;
    }
    if ((target.starts_with("weapon:") || target.starts_with("drone:")) &&
        action.starts_with("swap:")) {
        const auto colon = target.find(':');
        if (!swapArmaments(std::stoi(target.substr(colon + 1)),
                           std::stoi(action.substr(5)),
                           target.starts_with("drone:"))) {
            throw std::runtime_error("invalid_armament_swap");
        }
        return;
    }
    if (defeated_) {
        const std::array<std::string, 5> targets{"gameover_stats",
                                                 "gameover_restart",
                                                 "gameover_hangar",
                                                 "gameover_menu",
                                                 "gameover_quit"};
        for (int i = 0; i < static_cast<int>(targets.size()); ++i) {
            if (target == targets[i] || (i == 3 && target == "choice:0")) {
                const auto rect = gameOverButton(i);
                if (action == "hover") {
                    move(rect.x + rect.w / 2, rect.y + rect.h / 2);
                } else if (action == "click") {
                    click(rect.x + rect.w / 2, rect.y + rect.h / 2);
                } else {
                    throw std::runtime_error("unsupported_input_action");
                }
                return;
            }
        }
        throw std::runtime_error("target_not_available");
    }
    if (target.starts_with("enemy_room:") && action == "click" && enemy_.present) {
        const int id = std::stoi(target.substr(11));
        for (const auto& r : enemy_.ship.rooms) {
            if (r.id == id) {
                click(enemyOriginX() + r.x + r.w / 2.f, enemyOriginY() + r.y + r.h / 2.f);
                return;
            }
        }
        throw std::runtime_error("invalid_combat_room");
    }
    if (target.starts_with("player_room:") && (action == "click" || action == "right_click")) {
        const int id = std::stoi(target.substr(12));
        for (const auto& r : ship_.rooms) {
            if (r.id == id) {
                click(playerX() + ship_.horizontalShift + r.x + r.w / 2.f,
                      170.f + ship_.verticalShift + r.y + r.h / 2.f,
                      action == "right_click");
                return;
            }
        }
        throw std::runtime_error("invalid_combat_room");
    }
    if (target == "autofire" && action == "click") {
        click(585, 682);
        return;
    }
    if (target.starts_with("drone:") && action == "click") {
        const int index = std::stoi(target.substr(6));
        if (index < 0 || index >= int(combatDrones_.size())) {
            throw std::runtime_error("invalid_drone_slot");
        }
        // Centre of the box armamentAt hit-tests; the drone bar follows the
        // ship's system boxes.
        click(systemHudPosition(4).x + 32.f + 16.f + 97.f * logicalArmamentSlot("drone", index) +
                  47.f,
              642.f);
        return;
    }
    if (target == "jump" && action == "click") {
        click(573, 45);
        return;
    }
    if (target == "sector_next" && action == "click" && mapOpen_ &&
        graph_.beacons()[graph_.current()].exit && sector_ < 8 && fuel_ > 0) {
        const auto button = nextSectorButtonRect();
        click(button.x + button.w / 2, button.y + button.h / 2);
        return;
    }
    if (target == "map_cancel" && action == "click" && mapOpen_) {
        key("Escape");
        return;
    }
    if (target.starts_with("beacon:") && mapOpen_) {
        const auto index = static_cast<std::size_t>(std::stoul(target.substr(7)));
        const auto& beacon = graph_.beacons().at(index);
        if (action == "hover") {
            move(385.f + beacon.x, 123.f + beacon.y);
        } else if (action == "click") {
            click(385.f + beacon.x, 123.f + beacon.y);
        } else {
            throw std::runtime_error("unsupported_input_action");
        }
        return;
    }
    if (target.starts_with("choice:") && event_) {
        const auto index = static_cast<std::size_t>(std::stoul(target.substr(7)));
        if (index >= choices_.size()) {
            throw std::runtime_error("choice_out_of_range");
        }
        const float y = choiceRow(index);
        if (action == "hover") {
            move(405, y);
        } else if (action == "click") {
            click(405, y);
        } else {
            throw std::runtime_error("unsupported_input_action");
        }
        return;
    }
    if (target == "continue" && event_) {
        if (action == "hover") {
            move(405, choiceTop() + 6);
        } else if (action == "click") {
            click(405, choiceTop() + 6);
        } else {
            throw std::runtime_error("unsupported_input_action");
        }
        return;
    }
    if (target == "pause" && action == "click") {
        key("Space");
        return;
    }
    if (target == "save_stations" && action == "click") {
        stations(true);
        return;
    }
    if (target == "return_stations" && action == "click") {
        stations(false);
        return;
    }
    throw std::runtime_error("target_not_available");
}

void Run::diagnosticFixture(const std::string& name, const nlohmann::json& values) {
    if (!open_) {
        throw std::runtime_error("active_run_required");
    }
    if (name == "campaign_final") {
        const int difficulty =
            values.contains("difficulty") ? nativeValue<int>(values.at("difficulty")) : difficulty_;
        const bool advanced =
            values.contains("advanced") ? nativeValue<bool>(values.at("advanced")) : advanced_;
        if (difficulty < 0 || difficulty > 2 || tutorialOwned_ ||
            values.size() > std::size_t(values.contains("difficulty")) +
                                std::size_t(values.contains("advanced"))) {
            throw std::runtime_error("invalid_campaign_fixture");
        }
        nlohmann::json backup;
        checkpoint(backup, false);
        try {
            difficulty_ = difficulty;
            advanced_ = advanced;
            progressionRun_.difficulty = difficulty;
            progressionRun_.advanced = advanced;
            sector_ = 8;
            sectorName_ = "FINAL";
            sectorType_ = 0;
            secretSector_ = pendingSecretSector_ = false;
            campaign_ = {};
            detachCrewDroneShip(1);
            clearSpaceDroneRegistry();
            enemy_ = {};
            registerSpaceDrones(0, true);
            enemyHazards_.clear();
            projectiles_.clear();
            hostile_ = false;
            graph_.generate(eventRandom_.next(), eventRandom_);
            populateSector();
            mapOpen_ = false;
            panel_.clear();
            jumpTicks_ = arrivalTicks_ = 0;
            ftlCharge_ = 85;
            userPause_ = true;
            enterEvent(events_.event("LAST_STAND_START"), true);
            openingEvent_ = false;
        } catch (...) {
            checkpoint(backup, true);
            catalog_.setUsedCrewNames(crewNameHistory_);
            throw;
        }
    } else if (name == "combat_snapshot") {
        loadCombatSnapshot(values);
    } else if (name == "combat_reference") {
        const auto enemyShipId = values.value("enemy_ship_id", std::string("REBEL_SKINNY"));
        const auto enemyDroneChoice = values.value("enemy_drone_choice", std::string{});
        const int sensorLevel = values.value("sensor_level", 2);
        if (values.size() > (values.contains("enemy_ship_id") ? 1U : 0U) +
                                (values.contains("enemy_drone_choice") ? 1U : 0U) +
                                (values.contains("sensor_level") ? 1U : 0U) +
                                (values.contains("pinned_reward") ? 1U : 0U) ||
            sensorLevel < 1 || sensorLevel > 2 ||
            (enemyShipId != "REBEL_SKINNY" && enemyShipId != "REBEL_TRANSPORT" &&
             enemyShipId != "AUTO_ASSAULT" && enemyShipId != "AUTO_BASIC")) {
            throw std::runtime_error("invalid_combat_reference_ship");
        }
        encounter_ = "REBEL";
        // A fixture ship has the default ShipEvent seed (0).
        encounterShipSeed_ = 0;
        beginCombat(enemyShipId, enemyDroneChoice);
        // Pinned references reproduce the measured oracle reward only here.
        // Ordinary encounters use the authored reward distribution.
        enemy_.pinnedRewardFixture = values.value("pinned_reward", true);
        // The dedicated combat capture/launcher fixture upgrades sensors so
        // enemy room and effect states can be inspected throughout P06.
        if (auto* sensors = system(7)) {
            sensors->definition.power = sensorLevel;
            sensors->health = sensorLevel;
            sensors->power = sensorLevel;
            sensors->restorePower = sensorLevel;
        }
        for (auto& definition : ship_.systems) {
            if (definition.id == 7 && definition.installed) {
                definition.power = sensorLevel;
            }
        }
        event_ = mapOpen_ = false;
        userPause_ = true;
        jumpTicks_ = arrivalTicks_ = 0;
        fuel_ = 14;
        scrap_ = 10;
        missiles_ = 8;
        const std::array names{"Liam", "Fleischy", "Nazia"};
        for (std::size_t i = 0; i < std::min(crew_.size(), names.size()); ++i) {
            crew_[i].appearance.name = names[i];
        }
    } else if (name == "travel_nebula_snapshot") {
        const int hull = values.at("hull").get<int>();
        if (hull < 1 || hull > ship_.hull) {
            throw std::runtime_error("invalid_fixture_hull");
        }
        nebulaScene_.load(values.at("clouds"));
        nebula_ = true;
        hull_ = hull;
        userPause_ = true;
        event_ = mapOpen_ = false;
        jumpTicks_ = arrivalTicks_ = 0;
    } else if (name == "travel_event") {
        enterEvent(events_.event(values.at("event").get<std::string>()));
        openingEvent_ = false;
    } else if (name == "travel_reward_snapshot") {
        EventResources resources;
        resources.scrap = values.at("scrap").get<int>();
        resources.fuel = values.at("fuel").get<int>();
        resources.missiles = values.at("missiles").get<int>();
        resources.drones = values.at("drones").get<int>();
        for (int amount : {resources.scrap, resources.fuel, resources.missiles, resources.drones}) {
            if (amount < -100 || amount > 100) {
                throw std::runtime_error("invalid_reward_fixture");
            }
        }
        const auto text = values.at("text").get<std::string>();
        if (text.empty() || text.size() > 4096) {
            throw std::runtime_error("invalid_reward_fixture_text");
        }
        eventText_ = text;
        pendingResources_ = resources;
        choices_ = {{"Continue...", {}, true}};
        event_ = true;
        resetEventChoiceDelay();
        openingEvent_ = mapOpen_ = false;
    } else if (name == "travel_return_map") {
        diagnosticFixture("travel_reference");
        graph_.visit(5);
        graph_.visit(9);
        graph_.modifyPursuit(1);
        graph_.visit(5);
        event_ = false;
        mapOpen_ = true;
        userPause_ = true;
    } else if (name == "travel_hostile_charge") {
        // WorldManager::OnLoop 0x5f6520 SetSafe refills the drive unless a
        // hostile enemy ship is present. Use an unarmed automated ship so
        // nothing can damage the engines or piloting being measured.
        encounter_ = "AUTO_BASIC";
        encounterShipSeed_ = 0;
        beginCombat("AUTO_BASIC");
        for (auto& enemySystem : enemy_.systems) {
            if (enemySystem.definition.id == 3 || enemySystem.definition.id == 4) {
                enemySystem.health = 0;
                enemySystem.power = 0;
                enemySystem.restorePower = 0;
            }
        }
        hostile_ = true;
        ftlCharge_ = 0;
        event_ = mapOpen_ = userPause_ = false;
        jumpTicks_ = arrivalTicks_ = 0;
    } else if (name == "travel_no_fuel") {
        fuel_ = 0;
    } else if (name == "travel_sector_seed") {
        // Replays an original capture's StarMap currentSectorSeed for a fresh
        // first sector. StarMap::GenerateMap 0x5d4590 reseeds the shared
        // stream with it after StarMap::NewGame 0x5d5a20 drew it, so the map,
        // its events and the opening text/images that follow depend on the
        // seed alone. Only valid before the first jump.
        if (values.size() != 1 || !values.contains("seed") || sector_ != 1 || scoreBeacons_ != 1 ||
            jumpTicks_ != 0 || arrivalTicks_ != 0 || tutorialOwned_) {
            throw std::runtime_error("invalid_sector_seed_fixture");
        }
        graph_.generate(nativeValue<std::uint32_t>(values.at("seed")), eventRandom_);
        populateSector();
        locationBeacon_ = graph_.current();
        prepareOpeningText();
        prepareBeaconImages({});
        // Replace the discarded start location's environment and statuses
        // with the replayed one's, as CreateLocation would have applied it.
        setEnvironment("");
        pendingVoyageEffects_.push_back(
            {{"kind", "status"}, {"type", "clear"}, {"system", "all"}, {"target", "all"}});
        (void)commitVoyageEffects();
        applyStartLocation();
    } else if (name == "travel_reference") {
        graph_.generate(1027768635);
        // Measured native fixture metadata: reference/P05/012-nebulas.json.
        mapNebulas_ = {{67, 27, 0}, {11, -25, 1}, {90, 72, 1}, {32, 87, 1}};
        for (std::size_t index = 0; index < graph_.beacons().size(); ++index) {
            graph_.setEvent(static_cast<int>(index), "NOTHING");
        }
        graph_.setEvent(5, "NEBULA_PIRATE_SMUGGLE", true);
        graph_.setEvent(3, "REFUEL_STATION");
        for (int index : {0, 1, 4}) {
            graph_.setEvent(index, "NOTHING", true);
        }
        graph_.setEvent(19, "FINISH_BEACON", false, true);
        // CreateEvent 0x5b8940 moves a unique template into usedEvents when a
        // sector creates it, and GenerateMap 0x5d4590 empties that list first
        // (EventGenerator::Clear 0x5b70d0). These placed events are created on
        // arrival, so the replaced sector must not have used them already.
        for (const auto* placed : {"NEBULA_PIRATE_SMUGGLE", "REFUEL_STATION", "FINISH_BEACON"}) {
            std::erase(usedVoyageEvents_, std::string(placed));
        }
    } else if (name == "live_damage") {
        auto* s = system(1);
        if (!s) {
            throw std::runtime_error("engines_required");
        }
        s->health = 0;
        s->power = 0;
    } else if (name == "live_ion") {
        auto* s = system(0);
        if (!s) {
            throw std::runtime_error("shields_required");
        }
        setSystemLock(*s, 1);
        s->power = 0;
        s->restorePower = 0;
    } else {
        throw std::runtime_error("unknown_fixture");
    }
    resetResourceFeedback();
}

std::pair<int, int> Run::crewSkillProgress(const RunCrew& member, int skill) {
    const std::array<int, 6> caps = member.appearance.species == "human"
                                        ? std::array<int, 6>{26, 26, 100, 116, 32, 14}
                                        : std::array<int, 6>{30, 30, 110, 130, 36, 16};
    const int progress = skill < 3    ? member.manningSkillProgress[std::size_t(skill)]
                         : skill == 3 ? member.weaponSkillProgress
                         : skill == 4 ? member.repairSkillProgress
                                      : member.combatSkillProgress;
    return {progress, caps[std::size_t(skill)]};
}

std::string Run::crewSkillTooltip(const RunCrew& member, int skill, bool infoScreen) const {
    const auto [progress, cap] = crewSkillProgress(member, skill);
    // CrewMember::GetSkillLevel 0x46e890: 1 below half, 2 from half, 3 full.
    const int level = progress == cap ? 3 : progress >= cap / 2 ? 2 : 1;
    const auto format = [&](const std::string& key, const std::string& value) {
        auto result = texts_.get(key, locale_);
        if (const auto marker = result.find("\\1"); marker != std::string::npos) {
            result.replace(marker, 2, value);
        }
        return result;
    };
    static constexpr std::array<const char*, 6> names{
        "pilot", "engines", "shields", "weapons", "repair", "combat"};
    const std::string name = names[std::size_t(skill)];
    // ShipSystem::GetMannedDescription 0x6190e0 with getSkillBonus 0x5df310;
    // repair and combat use repair_/combat_ text in GetSkillTooltip 0x4768f0.
    std::string description;
    if (skill < 2) {
        description = format(skill == 0 ? "pilot_skill" : "engine_skill",
                             std::to_string(std::array{0, 5, 7, 10}[std::size_t(level)]));
    } else if (skill == 2) {
        description =
            format("shield_skill", std::to_string(std::array{0, 10, 20, 30}[std::size_t(level)]));
    } else if (skill == 3) {
        description =
            format("weapon_skill", std::to_string(std::array{0, 10, 15, 20}[std::size_t(level)]));
    } else {
        description = level <= 1 ? texts_.get(name + "_0", locale_)
                                 : format(name + "_skilled", level == 2 ? "10" : "20");
    }
    if (infoScreen) {
        return format(name + "_skill_infobox", description);
    }
    auto next = texts_.get("next_level", locale_);
    if (const auto marker = next.find("\\1"); marker != std::string::npos) {
        next.replace(marker, 2, std::to_string(progress));
    }
    if (const auto marker = next.find("\\2"); marker != std::string::npos) {
        next.replace(marker, 2, std::to_string(cap));
    }
    return format(name + "_skill_tooltip", description) + "\n" + next;
}

void Run::renderCrewSkill(
    const RunCrew& member, int skill, float x, float y, float length, float height) {
    // CrewBlueprint::RenderSkill 0x5df920 (CrewBox: 40 x 8, InfoBox: 110 x 9) after
    // RenderSkillIcon 0x5df7a0. Colours from the static initializer 0x823430.
    constexpr SDL_Color green{100, 255, 100, 255};
    const SDL_Color yellow =
        options_.colorblind ? SDL_Color{0, 147, 224, 255} : SDL_Color{255, 255, 50, 255};
    const auto [progress, cap] = crewSkillProgress(member, skill);
    const float fraction = float(progress) / float(cap);
    static constexpr std::array<const char*, 6> icons{
        "pilot", "engines", "shields", "weapons", "repair", "combat"};
    auto& icon = renderer_.image("img/people/skill_" + std::string(icons[std::size_t(skill)]) +
                                 "_white.png");
    renderer_.sprite(icon,
                     {x, y, 26, 26},
                     {0, 0, float(icon.w), float(icon.h)},
                     0,
                     fraction >= 1     ? yellow
                     : fraction >= .5f ? green
                                       : SDL_Color{243, 255, 238, 255});
    const float barX = x + 30;
    const float barY = y + (26 - height) / 2;
    float scale = fraction;
    SDL_Color fill = yellow;
    if (fraction < 1) {
        if (fraction >= .5f) {
            renderer_.fill({barX, barY, length, height}, green);
            scale = (fraction - .5f) + (fraction - .5f);
        } else {
            fill = green;
            scale = fraction + fraction;
        }
    }
    // The 1px white mark sits at the yellow fill edge only while the
    // second half fills; there is no fixed divider.
    const float width = float(int(length * scale));
    renderer_.fill({barX, barY, width, height}, fill);
    if (fraction >= .5f && fraction < 1) {
        renderer_.fill({barX + width, barY, 1, height}, {255, 255, 255, 255});
    }
    for (const auto edge : std::array<Rect, 4>{{{barX, barY, length, 1},
                                                {barX, barY + height - 1, length, 1},
                                                {barX, barY, 1, height},
                                                {barX + length - 1, barY, 1, height}}}) {
        renderer_.fill(edge, {255, 255, 255, 255});
    }
}

bool Run::crewPointerHit(const RunCrew& member) const {
    if (member.health <= 0 || member.dead || member.dying) {
        return false;
    }
    const auto point = crewPresentationPoint(member);
    const float ox = member.intruder ? enemyOriginX() : playerX() + float(ship_.horizontalShift);
    const float oy = member.intruder ? enemyOriginY() : 170.f + float(ship_.verticalShift);
    return Rect{point.x + ox - 17, point.y + oy - 17, 35, 35}.contains(mouseX_, mouseY_);
}

int Run::updateCrewBoxHover() {
    // CrewControl::MouseMove 0x4f4380 asks each CrewBox in order. A box that
    // was hovered keeps the pointer across its skill panel (skillBox:
    // box.x + box.w - 2, box.y, 85 x 146); bounds are exclusive.
    const auto living = crewRoster();
    int pointed = -1;
    int hovered = -1;
    for (std::size_t i = 0; i < living.size(); ++i) {
        const float y = 155.f + float(i) * 30;
        const bool inBox = mouseX_ > 10 && mouseX_ < 96 && mouseY_ > y && mouseY_ < y + 27;
        const bool inSkills = crewBoxHover_ == int(i) && mouseX_ > 94 && mouseX_ < 179 &&
                              mouseY_ > y && mouseY_ < y + 146;
        if (inBox || inSkills) {
            pointed = int(i);
            // CrewBox::OnLoop 0x501a40 clears mouseHover while cloning/dead.
            const auto& member = crew_[living[i]];
            if (!member.dead && !member.dying) {
                hovered = int(i);
            }
            break;
        }
    }
    crewBoxHover_ = hovered;
    return pointed;
}

void Run::renderCrewRosterSkills() {
    if (crewBoxHover_ < 0 || !panel_.empty() || event_ || mapOpen_ || jumpTicks_ > 0 || defeated_) {
        return;
    }
    const auto living = crewRoster();
    if (std::size_t(crewBoxHover_) >= living.size()) {
        return;
    }
    // CrewBox::OnRender 0x504240: each skill at (box.x + box.w + 7,
    // box.y + 24 * i); the skill-box background and outline are in crewHud.
    const float y = 155.f + float(crewBoxHover_) * 30;
    for (int skill = 0; skill < 6; ++skill) {
        renderCrewSkill(
            crew_[living[std::size_t(crewBoxHover_)]], skill, 103, y + 24.f * float(skill));
    }
}

void Run::tooltip() {
    if (doorCursorMode() > 0) {
        return;
    }
    const auto living = crewRoster();
    std::string hudTooltip;
    if (panel_.empty() && !event_ && !mapOpen_ && jumpTicks_ == 0 && !defeated_) {
        // ShipStatus::OnMouseMove 0x53c400 uses inclusive HUD rectangles;
        // ShipStatus::OnLoop forces these informational tooltips immediately.
        for (const auto& entry : std::array<std::pair<Rect, const char*>, 9>{
                 {{{6, 8, 368, 37}, "hull"},
                  {{121, 51, 65, 24}, "fuelCount"},
                  {{191, 51, 62, 24}, "missileCount"},
                  {{261, 51, 59, 24}, "droneCount"},
                  {{383, 12, 121, 33}, "scrapCount"},
                  {{7, 51, 121, 25}, "shieldDisplay"},
                  {{8, 96, 90, 20}, "evadeDisplay"},
                  {{8, 118, 90, 20}, "oxygenDisplay"},
                  {{30, 77, 98, 11}, "energyShieldDisplay"}}}) {
            const auto& bounds = entry.first;
            if (mouseX_ >= bounds.x && mouseX_ <= bounds.x + bounds.w - 1 && mouseY_ >= bounds.y &&
                mouseY_ <= bounds.y + bounds.h - 1 &&
                (std::string_view(entry.second) != "energyShieldDisplay" || superShields_ > 0)) {
                hudTooltip = "tooltip_" + std::string(entry.second);
                break;
            }
        }
    }
    // CrewBox::OnRender queues InstantTooltip for its box and skill panel.
    bool rosterHover = Rect{10, 155, 86, float(living.size() * 30)}.contains(mouseX_, mouseY_) ||
                       crewBoxHover_ >= 0;
    const bool hudButtonHover =
        panel_.empty() && !event_ && !mapOpen_ &&
        (Rect{625, 22, 74, 55}.contains(mouseX_, mouseY_) ||
         hudOptionsRect().contains(mouseX_, mouseY_) ||
         (hudStoreAvailable() && hudStoreRect().contains(mouseX_, mouseY_)));
    if (hudButtonHover && animation_ - hoverTick_ <= 9) {
        // MouseControl::OnRender: tooltipTimer > 0.15000001 seconds.
        return;
    }
    if (panel_ != "sector" && !rosterHover && hudTooltip.empty() && animation_ - hoverTick_ < 9) {
        return;
    }
    std::string value;
    if (panel_ == "sector") {
        // GetPotentialSectorChoiceName 0x5c8ec0 exposes only the category
        // for unidentified non-neighbors. Neighbor names are boxed labels.
        const auto& current = sectorGraph_.current();
        for (std::size_t index = 0; index < sectorGraph_.nodes().size(); ++index) {
            const auto& node = sectorGraph_.nodes()[index];
            const float dx = mouseX_ - (415.f + node.level * 66.f);
            const float dy = mouseY_ - (300.f + node.row);
            if (dx * dx + dy * dy < 100.f && !node.visited &&
                std::find(current.neighbors.begin(),
                          current.neighbors.end(),
                          static_cast<int>(index)) == current.neighbors.end()) {
                value = texts_.get(node.type == 1   ? "sector_hostile"
                                   : node.type == 2 ? "sector_nebula"
                                                    : "sector_civilian",
                                   locale_);
                break;
            }
        }
        if (value.empty()) {
            return;
        }
    }
    float x = mouseX_;
    float y = mouseY_ + 22;
    float width = 320;
    float wrap = 275;
    auto tr = [&](const std::string& k) {
        return texts_.get(k, locale_);
    };
    auto format = [&](const std::string& k, const std::string& a, const std::string& b = "") {
        auto v = tr(k);
        if (auto p = v.find("\\1"); p != std::string::npos) {
            v.replace(p, 2, a);
        }
        if (auto p = v.find("\\2"); p != std::string::npos) {
            v.replace(p, 2, b);
        }
        return v;
    };
    for (std::size_t i = 0; i < living.size(); ++i) {
        if (Rect{10, 155.f + i * 30, 86, 27}.contains(mouseX_, mouseY_)) {
            if (crew_[living[i]].dead || crew_[living[i]].dying) {
                value = texts_.get("cloning", locale_);
                break;
            }
            const auto& member = crew_[living[i]];
            value = format("health",
                           std::to_string(std::max(1, int(member.health))),
                           std::to_string(int(crewMaximumHealth(member.appearance.species) +
                                              member.healthBoost))) +
                    "\n" + format("hotkey", i < crewKeyNames_.size() ? crewKeyNames_[i] : "");
            x = 191;
            y = 155.f + i * 30;
            break;
        }
    }
    if (crewBoxHover_ >= 0 && std::size_t(crewBoxHover_) < living.size() && value.empty()) {
        // CrewBox::GetSelected 0x501bc0: inside the hovered box's skillBox,
        // row n (1..6) spans (box.y + 24n - 21, box.y + 24n - 3) exclusive and
        // shows GetSkillTooltip as a static tooltip at (box.x + box.w + 95,
        // box.y); between rows the box tooltip stays empty.
        const float boxY = 155.f + float(crewBoxHover_) * 30;
        for (int row = 1; row <= 6; ++row) {
            if (mouseY_ > boxY + 24.f * row - 21 && mouseY_ < boxY + 24.f * row - 3) {
                value = crewSkillTooltip(crew_[living[std::size_t(crewBoxHover_)]], row - 1, false);
                break;
            }
        }
        if (value.empty()) {
            return;
        }
        x = 191;
        y = boxY;
    }
    if (!hudTooltip.empty()) {
        value = tr(hudTooltip);
    } else if (panel_.empty() && !event_ && !mapOpen_ && jumpTicks_ == 0 && !defeated_) {
        if (Rect{625, 22, 74, 55}.contains(mouseX_, mouseY_)) {
            value = format("ship_info", "U");
        } else if (hudOptionsRect().contains(mouseX_, mouseY_)) {
            value = tr("open_options");
        } else if (hudStoreAvailable() && hudStoreRect().contains(mouseX_, mouseY_)) {
            value = tr("open_store");
            const auto newline = std::min(value.find("\\n"), value.find('\n'));
            if (newline != std::string::npos) {
                value.resize(newline);
            }
        }
    }
    auto systemTip = [&](const RunSystem& s) {
        int id = s.definition.id;
        int n = s.power;
        wrap = 300;
        value = tr("tooltip_" + s.definition.name) + "\n\n";
        std::string level;
        if (!n) {
            level = tr((id == 1 ? "engine" : s.definition.name) + "_off");
        } else {
            switch (id) {
            case 0:
                level = tr("shields_" + std::to_string(n / 2 - 1));
                break;
            case 1: {
                std::ostringstream rate;
                rate << 1 + (n - 1) * .25f;
                level = format("engine",
                               std::to_string(std::array{0, 5, 10, 15, 20, 25, 28, 31, 35}.at(n)),
                               rate.str());
                break;
            }
            case 2:
                level = format("oxygen_on", std::to_string(n == 1 ? 1 : (n - 1) * 3));
                break;
            case 5:
                level =
                    format("medbay_healing", std::array<std::string, 3>{"1", "1.5", "3"}.at(n - 1));
                break;
            case 6:
                level = tr("pilot_" + std::to_string(n));
                break;
            case 7:
                level = tr("sensor_" + std::to_string(n));
                break;
            case 8:
                level = tr("door_" + std::to_string(n));
                break;
            default:
                level = tr("system_power");
                break;
            }
            level = "Level " + std::to_string(n) + ": " + level;
        }
        value += level;
        if (manned(id) && (id == 0 || id == 1 || id == 3 || id == 7 || id == 8)) {
            value += "\n\nManned: " + (id == 7 || id == 8
                                           ? "Effective level " + std::to_string(subsystemLevel(id))
                                           : std::string(id == 1   ? "Evasion +5"
                                                         : id == 0 ? "Recharge +10%"
                                                                   : "Charge +10%"));
        }
        value += "\n\n" + tr("status") + "\n-" +
                 tr(passive(id)                    ? "full_functional"
                    : s.power == 0                 ? "unpowered"
                    : s.power < s.definition.power ? "partial_powered"
                                                   : "full_powered");
        if (s.health < s.definition.power) {
            value += "\n-" + tr(s.health ? "damaged" : "destroyed");
        }
        if (s.ion > 0) {
            value += "\n-" + tr("ioned");
        }
        std::string k = id == 0 ? "A" : id == 1 ? "S" : id == 5 ? "D" : id == 2 ? "F" : "";
        if (!k.empty()) {
            value +=
                "\n\n" + format("add_power", k) + "\n" + format("remove_power", "Shift + " + k);
        }
    };
    for (int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15, 3, 4, 6, 7, 8, 12}) {
        if (const auto* source = system(id)) {
            const auto position = systemHudPosition(id);
            if (Rect{position.x + 24,
                     position.y + 8 - 8.f * source->definition.power,
                     16,
                     8.f * source->definition.power + 55}
                    .contains(mouseX_, mouseY_)) {
                systemTip(*source);
                break;
            }
        }
    }
    float sy = 154.f + living.size() * 30;
    if (Rect{11, sy - 4, 40, 40}.contains(mouseX_, mouseY_)) {
        value = format("save_stations_tooltip", "/");
    }
    if (Rect{52, sy - 4, 40, 40}.contains(mouseX_, mouseY_)) {
        value = format("ret_stations_tooltip", "ENTER");
    }
    bool staticTooltip = x == 191;
    for (bool drone : {false, true}) {
        const int index = armamentAt(mouseX_, mouseY_, drone);
        if (index < 0) {
            continue;
        }
        const auto& e =
            catalog_.equipment(drone ? combatDrones_[index].blueprint : ship_.weapons[index]);
        value = e.descriptionText(texts_, locale_, advanced_, true);
        staticTooltip = false;
        break;
    }
    if (value.empty() && panel_.empty() && !event_ && !mapOpen_ && jumpTicks_ == 0) {
        const auto crewTip = [&](const RunCrew& member, float originX, float originY) {
            const auto point = crewPresentationPoint(member);
            if (member.health <= 0 ||
                !Rect{point.x + originX - 17, point.y + originY - 17, 35, 35}.contains(mouseX_,
                                                                                       mouseY_)) {
                return;
            }
            value = member.appearance.name +
                    "\nHealth: " + std::to_string(std::max(1, int(member.health))) + "/" +
                    std::to_string(
                        int(crewMaximumHealth(member.appearance.species) + member.healthBoost));
        };
        for (const auto& member : crew_) {
            if (!member.intruder) {
                crewTip(member, playerX() + ship_.horizontalShift, 170.f + ship_.verticalShift);
            }
            if (!value.empty()) {
                break;
            }
        }
        // Hidden actors are not drawn in blacked-out rooms, so they offer no
        // crew tooltip either (ShipManager::OnRender 0x49b435 GetRoomBlackedOut).
        if (value.empty()) {
            for (const auto& member : voyageBoarders_) {
                if (!playerRoomVisible(member.room)) {
                    continue;
                }
                crewTip(member, playerX(), 170.f);
                if (!value.empty()) {
                    break;
                }
            }
        }
        if (value.empty() && enemy_.present && !enemyExplosionActive_) {
            for (const auto& member : enemy_.crew) {
                if (!member.intruder && enemyRoomVisible(member.room)) {
                    crewTip(member, enemyOriginX(), enemyOriginY());
                }
                if (!value.empty()) {
                    break;
                }
            }
            if (value.empty()) {
                for (const auto& member : crew_) {
                    if (member.intruder) {
                        crewTip(member, enemyOriginX(), enemyOriginY());
                    }
                    if (!value.empty()) {
                        break;
                    }
                }
            }
        }
    }
    if (value.empty()) {
        return;
    }
    const auto lines = wrappedText(12, value, wrap);
    float measured = 0;
    for (const auto& line : lines) {
        measured =
            std::max(measured, renderer_.measure(localizedFont(12, line), line, textScale(12)));
    }
    const float lineHeight = textLineHeight(12, value);
    width = float(int(measured)) + 27;
    const float height = float(int(lines.size() * lineHeight)) + 18;
    if (!staticTooltip) {
        x = mouseX_;
        y = mouseY_ + 22;
        if (mouseY_ + height > 670) {
            x += 16;
            y = std::trunc(y - (mouseY_ + height - 670));
            if (y + height > mouseY_) {
                y = mouseY_ - height - 10;
            }
        }
        if (mouseX_ + width > 1260) {
            x = std::trunc(x - (mouseX_ + width - 1240));
        }
    }
    renderer_.fill({x, y, width, height}, {0, 0, 0, 242});
    for (auto edge : std::array<Rect, 4>{{{x, y, width, 2},
                                          {x, y + height - 2, width, 2},
                                          {x, y, 2, height},
                                          {x + width - 2, y, 2, height}}}) {
        renderer_.fill(edge, {255, 255, 255, 255});
    }
    for (const auto& line : lines) {
        text(line, x + 15, y + 11, 12, false, {235, 245, 229, 255});
        y += lineHeight;
    }
}
} // namespace ftl
