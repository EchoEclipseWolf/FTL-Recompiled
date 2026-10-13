#include "run.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>

namespace ftl {
namespace {
const RoomDefinition& findRoom(const ShipDefinition& ship, int id) {
    const auto found = std::ranges::find(ship.rooms, id, &RoomDefinition::id);
    if (found == ship.rooms.end()) {
        throw std::runtime_error("invalid_crew_room");
    }
    return *found;
}

RunSystem* findSystem(std::vector<RunSystem>& systems, int id) {
    const auto found = std::ranges::find_if(systems, [&](const RunSystem& system) {
        return system.definition.id == id;
    });
    return found == systems.end() ? nullptr : &*found;
}

const RunSystem* findSystem(const std::vector<RunSystem>& systems, int id) {
    const auto found = std::ranges::find_if(systems, [id](const auto& sys) {
        return sys.definition.id == id;
    });
    return found == systems.end() ? nullptr : &*found;
}

void clearCrewCombat(RunCrew& member) {
    member.attackTargetOwner = member.attackTargetIndex = -1;
    member.attackTargetDrone = false;
    member.attackTargetDroneId = 0;
    member.attackAnimation.clear();
    member.attackElapsed = member.attackGoal = member.attackFlight = 0;
    member.attackAnimationElapsed = 0;
    member.attackHit = false;
}

void clearForeignPosition(RunCrew& member) {
    member.boardPositionValid = false;
    member.boardTargetRoom = -1;
    member.boardNewGoal = false;
    member.boardPath.clear();
    member.lastDoor = member.boardLastDoor = -1;
    member.blockingDoor = member.boardBlockingDoor = -1;
}

// MindSystem tables from .rdata 0x837d50-0x837da0, indexed by the mind system's
// power level (`MindSystem::GetMindPowerLevel 0x609d10`).
constexpr std::array<float, 4> mindControlTimer{14.f, 14.f, 20.f, 28.f}; // 0x837d90
constexpr std::array<float, 4> mindDamageBoost{1.f, 1.f, 1.25f, 2.f};    // 0x837d50
constexpr std::array<int, 4> mindHealthBoost{0, 0, 15, 30};              // 0x837d60
// TeleportSystem::OnLoop 0x620ee0 advances `chargeLevel` by
// `SpeedFactor * _CSWTCH.368[effectivePower - 1]` (0x838d38 = .2/.35/.5) and clamps it
// at 100 (0x838d50); TeleportSystem::InitiateTeleport 0x620e10 locks
// `_CSWTCH.282[effectivePower - 1]` (0x838d44 = 4/3/2) five-second lock intervals.
constexpr std::array<int, 3> teleportLock{4, 3, 2};

template <class Members>
auto mindMember(Members& player, Members& enemy, Members& foreign, int owner, int index) {
    using Pointer = decltype(&player[0]);
    if (index < 0) {
        return Pointer{};
    }
    if (owner == 0) {
        return std::size_t(index) < player.size() ? &player[std::size_t(index)] : Pointer{};
    }
    if (std::size_t(index) < enemy.size()) {
        const auto away = std::ranges::find_if(foreign, [&](const auto& member) {
            return member.enemyCrewOrigin == index && !member.outOfGame;
        });
        return away != foreign.end() ? &*away : &enemy[std::size_t(index)];
    }
    const auto extended = std::size_t(index) - enemy.size();
    return extended < foreign.size() && foreign[extended].enemyCrewOrigin < 0 ? &foreign[extended]
                                                                              : Pointer{};
}

// The ShipManager whose crew list holds the actor (CrewMember::currentShipId): player
// crew stand on 0 unless boarding, resident enemy crew on 1, voyage boarders on 0.
template <class Members>
int mindPhysicalShip(const Members& player, const Members& enemy, const RunCrew& member) {
    const auto contains = [&](const Members& members) {
        return std::ranges::any_of(members, [&](const auto& actor) {
            return &actor == &member;
        });
    };
    if (contains(player)) {
        return member.intruder ? 1 : 0;
    }
    if (contains(enemy)) {
        return member.intruder ? 0 : 1;
    }
    return 0;
}
} // namespace

void Run::initializeCrewAnimation(RunCrew& member, RandomStream& random) {
    // CrewAnimation479840 constructs the looping ranged timer before palette
    // choices. Restart46c670 does not reset this timer after death or cloning.
    member.shootGoal = float(random.next() % 300 + 1000) / 1000.f;
    member.shootElapsed = 0;
    member.pendingDamage = 0;
}

void Run::renderEnemySystemHud() {
    if (!enemy_.present) {
        return;
    }
    // CombatControl566ab0 builds one generic SystemBox per physical system,
    // retaining ShipManager's vector order rather than selecting five IDs.
    const bool boss =
        campaign_.fighting || enemy_.ship.id == "BOSS_SPECIAL" || enemy_.ship.id == "BOSS_DEMO";
    SDL_FPoint position{boss ? 832.f : 912.f, boss ? 542.f : 515.f};
    if (enemy_.systems.size() > 12) {
        position.x -= 45.f;
    }
    const auto* sensors = system(7);
    // DoSensorsProvide491530 deliberately maps request3 to effective4.
    const bool showPower = !nebula_ && sensors && sensors->hackLevel <= 1 &&
                           subsystemLevel(7) > 3 && !enemy_.cloak.active;
    for (const auto& station : enemy_.systems) {
        if (station.definition.power <= 0) {
            continue;
        }
        renderSystemHudBox(station, position, true, showPower);
        position.x += 30.f;
    }
}

void Run::resetCrewForClone(RunCrew& member) {
    // Clone470650/Restart46fd90 reuse the original actor, preserving stations,
    // statistics, control metadata and ranged timer/projectile effects.
    const float flight = member.attackFlight;
    clearCrewCombat(member);
    member.attackFlight = flight;
    clearForeignPosition(member);
    member.intruder = false;
    member.boardRoom = -1;
    member.health = crewMaximumHealth(member.appearance.species);
    member.stunTime = 0;
    member.deathTick = 0;
    member.dying = member.dead = member.outOfGame = false;
    member.deathAnimationElapsed = 0;
    member.deathAnimationDone = false;
    member.path.clear();
    member.cloned = true;
    applyCloneSkillLoss(member);
}

SDL_FPoint Run::crewCombatPoint(const RunCrew& member) const {
    if (member.intruder && member.boardPositionValid) {
        return {member.boardX, member.boardY};
    }
    for (const auto& player : crew_) {
        if (&player != &member || !member.intruder) {
            continue;
        }
        const auto shape =
            std::ranges::find(enemy_.ship.rooms, member.boardRoom, &RoomDefinition::id);
        if (shape == enemy_.ship.rooms.end()) {
            return {member.x, member.y};
        }
        int tile = 0;
        for (const auto& preceding : crew_) {
            if (&preceding == &member) {
                break;
            }
            if (preceding.intruder && preceding.boardRoom == member.boardRoom) {
                ++tile;
            }
        }
        const int columns = std::max(1, shape->w / 35);
        tile %= columns * std::max(1, shape->h / 35);
        return {shape->x + 35.f * (tile % columns) + 17.f,
                shape->y + 35.f * (tile / columns) + 17.f};
    }
    return {member.x, member.y};
}

std::string Run::crewCombatAnimation(const RunCrew& member) const {
    const bool idle = member.intruder ? member.boardPath.empty() : member.path.empty();
    return member.health > 0 && member.stunTime <= 0 && idle ? member.attackAnimation : "";
}

SDL_FPoint Run::crewPresentationPoint(const RunCrew& member) const {
    auto point = crewCombatPoint(member);
    float shift = 3.f;
    if (member.attackAnimation.starts_with("punch_") && !crewCombatAnimation(member).empty()) {
        const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& player) {
            return &player == &member;
        });
        const bool away = playerOwned
                              ? member.intruder
                              : std::ranges::any_of(voyageBoarders_, [&](const auto& boarder) {
                                    return &boarder == &member;
                                });
        shift = away != member.controlled ? 10.f : -4.f;
    }
    point.y += shift;
    return point;
}

// CrewAnimation::OnUpdate 0x473600 ends by forcing the walk strips of all four
// directions to tracker.time 0.7 when the body's ShipObject (its owning ship)
// has CREW_STIMS, 1.0 otherwise. The method is not virtual, so crew drones on a
// stimmed ship also cycle faster even though their move speed is unchanged.
std::uint64_t Run::crewWalkTick(const std::string& animation, bool enemyOwner) const {
    if (!animation.starts_with("walk_")) {
        return tick_;
    }
    const auto& augments = enemyOwner ? enemy_.ship.augments : ship_.augments;
    return std::ranges::find(augments, "CREW_STIMS") != augments.end()
               ? static_cast<std::uint64_t>(static_cast<double>(tick_) / .7)
               : tick_;
}

std::uint64_t Run::crewCombatTick(const RunCrew& member) const {
    if (member.attackAnimation.empty() || member.attackGoal <= 0) {
        return 0;
    }
    const auto& animation =
        catalog_.animation(member.appearance.species + "_" + member.attackAnimation);
    const float duration =
        member.attackGoal * (member.attackAnimation.starts_with("punch_") ? 2.f : 1.f);
    return static_cast<std::uint64_t>(member.attackAnimationElapsed / duration *
                                      animation.duration * 60.f);
}

bool Run::crewHasOpponent(const RunCrew& member, int ship, int owner) const {
    const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& player) {
        return &player == &member;
    });
    const bool foreign = std::ranges::any_of(voyageBoarders_, [&](const auto& boarder) {
        return &boarder == &member;
    });
    const bool body = ship >= 0 && owner >= 0;
    const int currentShip = body ? ship : playerOwned ? int(member.intruder) : foreign ? 0 : 1;
    const int currentRoom = playerOwned && member.intruder ? member.boardRoom : member.room;
    const bool hostile = body ? owner == 1 : playerOwned ? member.controlled : !member.controlled;
    const auto matches = [&](const RunCrew& other, int ship, int room, bool opposing) {
        return &other != &member && other.health > 0 && ship == currentShip &&
               room == currentRoom && opposing != hostile;
    };
    const auto droneOpponent = [&](const auto& drones, int owner) {
        return std::ranges::any_of(drones, [&](const auto& drone) {
            const auto& type = catalog_.equipment(drone.blueprint).type;
            if ((type != "BOARDER" && type != "BATTLE" && type != "REPAIR") ||
                drone.crewPhysicalShip < 0 || drone.crewDead || drone.health <= 0) {
                return false;
            }
            return drone.crewPhysicalShip == currentShip && drone.crewRoom == currentRoom &&
                   (owner == 1) != hostile;
        });
    };
    return std::ranges::any_of(crew_,
                               [&](const auto& other) {
                                   return matches(other,
                                                  int(other.intruder),
                                                  other.intruder ? other.boardRoom : other.room,
                                                  other.controlled);
                               }) ||
           std::ranges::any_of(enemy_.crew,
                               [&](const auto& other) {
                                   return enemy_.present && !other.intruder &&
                                          matches(other, 1, other.room, !other.controlled);
                               }) ||
           std::ranges::any_of(voyageBoarders_,
                               [&](const auto& other) {
                                   return matches(other, 0, other.room, !other.controlled);
                               }) ||
           droneOpponent(combatDrones_, 0) || droneOpponent(enemy_.drones, 1);
}

void Run::assignPhysicalCrewOrder(RunCrew& member, bool reinsert) {
    if (!reinsert && member.physicalCrewOrder != 0) {
        return;
    }
    if (nextPhysicalCrewOrder_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("crew_physical_order_exhausted");
    }
    member.physicalCrewOrder = nextPhysicalCrewOrder_++;
}

void Run::assignPhysicalCrewOrder(CombatDrone& drone, bool reinsert) {
    if (!reinsert && drone.physicalCrewOrder != 0) {
        return;
    }
    if (nextPhysicalCrewOrder_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("crew_physical_order_exhausted");
    }
    drone.physicalCrewOrder = nextPhysicalCrewOrder_++;
}

void Run::advancePhysicalCrew(int physicalShip) {
    auto& intruderCount = physicalShip == 0 ? playerIntruderCount_ : enemy_.intruderCount;
    intruderCount = 0;
    // UpdateCrewmembers49ee10 queries GetIntruder and Functional BEFORE
    // the actor callback. Ordinary Functional720b00 tests bDead only;
    // CrewDrone GetIntruder73ddd0 excludes bDead and undeployed actors;
    // Functional73dd70 additionally tests virtual GetPowered. HP0 alone
    // still does not exclude a body whose death animation has not finished.
    for (int owner : {0, 1}) {
        for (const auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == physicalShip && owner != physicalShip &&
                !drone->crewDead && drone->deployed && drone->powered) {
                ++intruderCount;
            }
        }
    }
    if (!enemy_.present || !hostile_) {
        for (int owner : {0, 1}) {
            for (auto* drone : physicalCrewDrones(owner)) {
                if (drone->crewPhysicalShip == physicalShip && physicalShip != owner &&
                    !drone->crewDead && drone->health > 0) {
                    drone->health = 0;
                }
            }
        }
    }

    struct Actor {
        RunCrew* member;
        int ship;
        int room;
        bool hostile;
        CombatDrone* drone = nullptr;
        int owner = 0;
        int index = -1;
    };

    std::vector<Actor> actors;
    actors.reserve(crew_.size() + enemy_.crew.size() + voyageBoarders_.size() +
                   combatDrones_.size() + enemy_.drones.size());
    for (auto& member : crew_) {
        if (member.outOfGame) {
            continue;
        }
        actors.push_back({&member,
                          member.intruder ? 1 : 0,
                          member.intruder ? member.boardRoom : member.room,
                          member.controlled,
                          nullptr,
                          0,
                          int(&member - crew_.data())});
    }
    for (auto& member : enemy_.crew) {
        if (!member.outOfGame && (enemy_.present || member.intruder)) {
            actors.push_back({&member,
                              member.intruder ? 0 : 1,
                              member.intruder ? member.boardRoom : member.room,
                              !member.controlled,
                              nullptr,
                              1,
                              int(&member - enemy_.crew.data())});
        }
    }
    for (auto& member : voyageBoarders_) {
        if (member.outOfGame) {
            continue;
        }
        const int index =
            member.enemyCrewOrigin >= 0
                ? member.enemyCrewOrigin
                : int(enemy_.crew.size() + std::size_t(&member - voyageBoarders_.data()));
        actors.push_back({&member, 0, member.room, !member.controlled, nullptr, 1, index});
    }
    // CrewDrone::OnLoop 0x43ce20 enters the same CrewMember/animation attack
    // lifecycle. Adapt native drone bodies to that domain without adding them
    // to the ordinary roster, manning, cloning or teleport lists.
    std::vector<RunCrew> bodies;
    bodies.reserve(physicalCrewDrones(0).size() + physicalCrewDrones(1).size());
    const auto addDrones = [&](int owner) {
        for (auto* physical : physicalCrewDrones(owner)) {
            auto& drone = *physical;
            const auto& type = catalog_.equipment(drone.blueprint).type;
            if ((type != "BOARDER" && type != "BATTLE" && type != "REPAIR") ||
                drone.crewPhysicalShip < 0 || drone.crewDead) {
                continue;
            }
            bodies.emplace_back();
            auto& body = bodies.back();
            if (drone.health <= 0 && !drone.crewDying) {
                drone.crewDeathLeft = drone.attackAnimation.ends_with("_left");
                if (drone.attackAnimation.empty()) {
                    drone.crewDeathLeft = drone.velocity.x < 0;
                }
                drone.crewDying = true;
                drone.crewDeathElapsed = 0;
                drone.crewDeathDone = false;
            }
            body.appearance.species = type == "REPAIR"                   ? "repair"
                                      : drone.blueprint == "BOARDER_ION" ? "boarder_ion"
                                                                         : "battle";
            body.health = drone.health;
            body.dying = drone.crewDying;
            body.dead = drone.crewDead;
            const float maximum = type == "REPAIR"                   ? 25.f
                                  : drone.blueprint == "BOARDER_ION" ? 125.f
                                                                     : 150.f;
            body.healthBoost = maximum - crewMaximumHealth(body.appearance.species);
            body.repairKind = drone.repairKind;
            body.repairTarget = drone.repairTarget;
            body.repairRoom = drone.repairRoom;
            // UpdateRepair and OnUpdate read the iOnFire of the last pass.
            body.onFire = drone.crewOnFire;
            body.room = drone.crewRoom;
            body.x = drone.crewPosition.x;
            body.y = drone.crewPosition.y;
            const int physicalShip = drone.crewPhysicalShip;
            const auto& layout = physicalShip == 0 ? ship_ : enemy_.ship;
            const auto shape = std::ranges::find(layout.rooms, drone.crewRoom, &RoomDefinition::id);
            if (shape != layout.rooms.end()) {
                const int columns = std::max(1, shape->w / 35);
                const int column = std::clamp(int((body.x - shape->x) / 35), 0, columns - 1);
                const int row =
                    std::clamp(int((body.y - shape->y) / 35), 0, std::max(1, shape->h / 35) - 1);
                body.slot = body.targetSlot = row * columns + column;
                body.targetRoom = body.room;
            }
            body.stunTime = drone.ionStun;
            body.medicalRate = drone.medicalRate;
            body.healingElapsed = drone.healingElapsed;
            body.mindElapsed = drone.mindElapsed;
            body.stunElapsed = drone.stunElapsed;
            const auto* controller = findSystem(owner == 0 ? systems_ : enemy_.systems, 4);
            if (!drone.powered ||
                (controller && controller->hackActive && controller->hackLevel > 1)) {
                body.stunTime = std::max(Time::step, body.stunTime);
            }
            if (drone.crewPathIndex < int(drone.crewPath.size())) {
                body.path.push_back({drone.crewPath[std::size_t(drone.crewPathIndex)], -1});
            }
            body.facing = drone.attackAnimation.ends_with("_left")    ? "left"
                          : drone.attackAnimation.ends_with("_right") ? "right"
                          : drone.attackAnimation.ends_with("_up")    ? "up"
                                                                      : "down";
            body.attackAnimation = drone.attackAnimation;
            body.attackElapsed = drone.attackElapsed;
            body.attackGoal = drone.attackGoal;
            body.shootGoal = drone.shootGoal;
            body.shootElapsed = drone.shootElapsed;
            body.pendingDamage = drone.pendingDamage;
            body.attackAnimationElapsed = drone.attackAnimationElapsed;
            body.attackHit = drone.attackHit;
            body.attackFlight = drone.attackFlight;
            body.attackFrom = drone.attackFrom;
            body.attackTo = drone.attackTo;
            body.attackTargetOwner = drone.attackTargetOwner;
            body.attackTargetIndex = drone.attackTargetIndex;
            body.attackTargetDrone = drone.attackTargetDrone;
            body.attackTargetDroneId = drone.attackTargetDroneId;
            body.blockingDoor = drone.crewBlockingDoor;
            actors.push_back({&body, physicalShip, drone.crewRoom, owner == 1, &drone, owner, -1});
        }
    };
    addDrones(0);
    addDrones(1);
    // ShipManager49ee10 iterates the physical insertion list. Constructor
    // identity is separate: teleports append the same actor to a new ship.
    std::stable_sort(actors.begin(), actors.end(), [](const auto& first, const auto& second) {
        if (first.ship != second.ship) {
            return first.ship < second.ship;
        }
        const auto order = [](const auto& actor) {
            return actor.drone ? actor.drone->physicalCrewOrder : actor.member->physicalCrewOrder;
        };
        return order(first) < order(second);
    });

    const auto slot = [](const Actor& actor) {
        return actor.member->intruder ? actor.member->boardSlot : actor.member->slot;
    };
    const auto atFinalGoal = [&](const Actor& actor) {
        const auto& member = *actor.member;
        const bool aboardForeign = member.intruder && !actor.drone;
        if (aboardForeign ? !member.boardPath.empty() : !member.path.empty()) {
            return false;
        }
        const int targetRoom = aboardForeign ? member.boardTargetRoom : member.targetRoom;
        const int targetSlot = aboardForeign ? member.boardTargetSlot : member.targetSlot;
        const auto& layout = actor.ship == 0 ? ship_ : enemy_.ship;
        const auto shape = std::ranges::find(layout.rooms, targetRoom, &RoomDefinition::id);
        if (shape == layout.rooms.end() || targetSlot < 0) {
            return false;
        }
        const int columns = std::max(1, shape->w / 35);
        const auto position = crewCombatPoint(member);
        const float x = float(shape->x + 35 * (targetSlot % columns) + 17);
        const float y = float(shape->y + 35 * (targetSlot / columns) + 17);
        return std::abs(position.x - x) < .5f && std::abs(position.y - y) < .5f;
    };
    std::map<int, int> intrudersByRoom;
    for (auto& actor : actors) {
        if (actor.ship != physicalShip) {
            continue;
        }
        auto& member = *actor.member;
        auto& smoke = actor.drone ? actor.drone->crewSmoke : member.smoke;
        // Status 4 is decided again by this pass's OnUpdate; a body that
        // returns before it is drawn by its death or idle strip instead.
        smoke.fireDirection = -1;
        if (!actor.drone && !member.dead && ((actor.owner != actor.ship) != member.controlled)) {
            ++intruderCount;
        }
        if (!actor.drone && member.dead) {
            finishCrewDeath(member);
            continue;
        }
        if (member.outOfGame) {
            continue;
        }
        if (!actor.drone) {
            member.stunTime = std::max(0.f, member.stunTime - Time::step);
            member.cloneReady =
                findSystem(actor.owner == 0 ? systems_ : enemy_.systems, 13) != nullptr;
        } else if (actor.drone->deployed && actor.drone->crewSlot < 0 &&
                   catalog_.equipment(actor.drone->blueprint).type != "BOARDER") {
            // CrewDrone43ce20 performs its derived SetRoom(droneRoom)
            // before CrewMember4747f0 can return for an unassigned slot.
            // This actor already belongs to the physical list from birth;
            // acquiring its first reservation does not reinsert or tick it.
            auto& drone = *actor.drone;
            const auto* controller = findSystem(actor.owner == 0 ? systems_ : enemy_.systems, 4);
            const bool dead = drone.crewDead;
            drone.crewDead = true;
            const bool placed = placeVoyageCrew(
                member, controller ? controller->definition.room : -1, false, actor.ship == 1);
            drone.crewDead = dead;
            if (placed) {
                drone.crewRoom = actor.room = member.room;
                drone.crewSlot = member.slot;
                drone.crewPosition = {member.x, member.y};
                drone.targetRoom = member.targetRoom;
                drone.delivered = true;
            }
        }

        struct MetadataUpdate {
            std::function<void()> apply;

            ~MetadataUpdate() {
                apply();
            }
        } updateMetadata{[&] {
            // CrystalAlien475b00 advances after the base virtual call even
            // when that base returned for an unassigned current slot.
            if (!actor.drone && member.appearance.species == "crystal") {
                member.crystalPowerElapsed =
                    std::min(50.f, member.crystalPowerElapsed + Time::step);
            }
            actor.room = actor.drone       ? actor.drone->crewRoom
                         : member.intruder ? member.boardRoom
                                           : member.room;
            const auto& oxygen = actor.ship == 0 ? roomOxygen_ : enemy_.roomOxygen;
            const auto& hazards = actor.ship == 0 ? playerHazards_ : enemyHazards_;
            const auto& systems = actor.ship == 0 ? systems_ : enemy_.systems;
            member.medicalRate = 0;
            const auto roomOxygen = oxygen.find(actor.room);
            member.suffocating =
                !actor.drone && roomOxygen != oxygen.end() && roomOxygen->second <= 5;
            const auto hazard = hazards.find(actor.room);
            member.onFire = hazard == hazards.end() ? 0 : hazard->second.fires;
            if (member.dead || member.outOfGame || member.health <= 0) {
                return;
            }
            const bool intruder = (actor.owner != actor.ship) != member.controlled;
            if (!intruder) {
                if (actor.drone) {
                    const auto* controller = findSystem(systems, 4);
                    // UpdateCrewmembers 0x49ee10 needs only the drone system's
                    // Functioning (effective power, hacking ignored) and no
                    // intruder counted in the room so far; SetMedbay 0x472c80
                    // then keeps the rate only when CanHeal 0x46d480 allows it.
                    if (controller && controller->definition.room == actor.room &&
                        controller->power > 0 && controller->health > 0 &&
                        intrudersByRoom[actor.room] == 0 &&
                        (member.health <
                             crewMaximumHealth(member.appearance.species) + member.healthBoost ||
                         member.onFire > 0 || member.attackTargetOwner >= 0)) {
                        member.medicalRate = 1;
                    }
                } else {
                    const auto* medbay = findSystem(systems, 5);
                    const bool hacked = medbay && medbay->hackActive && medbay->hackLevel > 1;
                    if (medbay && (hacked || (medbay->power > 0 && medbay->health > 0))) {
                        if (medbay->definition.room == actor.room) {
                            member.medicalRate =
                                hacked ? -2.f : float(std::min(medbay->power, medbay->health));
                        } else if (!hacked) {
                            for (const auto& augment :
                                 (actor.ship == 0 ? ship_ : enemy_.ship).augments) {
                                if (augment == "NANO_MEDBAY") {
                                    member.medicalRate += catalog_.equipment(augment).value;
                                }
                            }
                        }
                    }
                    // CanHeal 0x46d480 ignores an anaerobic crew's suffocation flag.
                    const bool canHeal =
                        member.health <
                            crewMaximumHealth(member.appearance.species) + member.healthBoost ||
                        member.onFire > 0 ||
                        (member.suffocating && member.appearance.species != "anaerobic") ||
                        member.attackTargetOwner >= 0;
                    if (member.medicalRate > 0 && !canHeal) {
                        member.medicalRate = 0;
                    }
                }
            }
            if (intruder) {
                ++intrudersByRoom[actor.room];
            }
        }};

        // CrewMember4747f0 returns before targets, movement or animation
        // when its current physical slot has not been assigned. The manager
        // still refreshes room metadata after that virtual call.
        if (slot(actor) < 0 || actor.room < 0) {
            continue;
        }
        RunCrew* target = nullptr;
        CombatDrone* targetDrone = nullptr;
        bool melee = false;
        int doorTarget =
            member.intruder && !actor.drone ? member.boardBlockingDoor : member.blockingDoor;
        const auto prior = std::ranges::find_if(actors, [&](const Actor& candidate) {
            return candidate.ship == actor.ship && candidate.room == actor.room &&
                   candidate.hostile != actor.hostile && !candidate.member->dead &&
                   !candidate.member->dying && !candidate.member->outOfGame &&
                   candidate.owner == member.attackTargetOwner &&
                   (candidate.drone != nullptr) == member.attackTargetDrone &&
                   (candidate.drone ? candidate.drone->crewActorId == member.attackTargetDroneId
                                    : candidate.index == member.attackTargetIndex);
        });
        if (prior != actors.end()) {
            target = prior->member;
            targetDrone = prior->drone;
            melee = slot(actor) == slot(*prior) && atFinalGoal(actor) && atFinalGoal(*prior);
        }
        const bool nonFighter =
            member.appearance.species == "repair" || member.appearance.species == "boarder_ion";
        if (nonFighter && doorTarget < 0) {
            // SetCurrentTarget471e80 still follows the room-selection RNG,
            // but a non-fighting CrewDrone cannot retain a combat target.
            clearCrewCombat(member);
            target = nullptr;
            targetDrone = nullptr;
        }
        if (doorTarget >= 0 &&
            crewDoorSealed(doorTarget, actor.owner, member.controlled, actor.ship == 1) &&
            (!target || nonFighter)) {
            target = nullptr;
            targetDrone = nullptr;
            member.attackTargetOwner = member.attackTargetIndex = -1;
            member.attackTargetDrone = false;
            member.attackTargetDroneId = 0;
            melee = true;
        } else {
            doorTarget = -1;
        }
        const auto hit = [&](float rawDamage) {
            if (doorTarget >= 0) {
                // Door::ApplyDamage463f30 ignores the numerical amount.
                damageDoor(doorTarget, actor.ship == 1);
                return;
            }
            if (!target) {
                return;
            }
            const float race = member.appearance.species == "mantis"   ? 1.5f
                               : member.appearance.species == "engi"   ? .5f
                               : member.appearance.species == "battle" ? 1.2f
                                                                       : 1.f;
            int level = mindLevel();
            const auto* playerTarget =
                mindMember(crew_, enemy_.crew, voyageBoarders_, mindTargetOwner_, mindTarget_);
            if (member.controlled && (!mind_.active || playerTarget != &member)) {
                const auto sys = std::ranges::find_if(enemy_.systems, [](const auto& entry) {
                    return entry.definition.id == 14;
                });
                level = sys == enemy_.systems.end()             ? 0
                        : sys->hackActive && sys->hackLevel > 1 ? sys->health
                                                                : std::min(sys->power, sys->health);
            }
            const float boost =
                member.controlled ? mindDamageBoost[std::size_t(std::clamp(level, 0, 3))] : 1.f;
            // CrewAnimation::GetDamageDone 0x473570 consumes each 3..7 hit once.
            if (target->health <= 0) {
                return;
            }
            const float previousHealth = target->health;
            target->health = std::max(
                0.f,
                target->health -
                    rawDamage * race * combatSkillDamageMultiplier(member.combatSkill) * boost);
            if (!targetDrone) {
                noteCrewHealthChange(*target, previousHealth);
            }
            if (targetDrone) {
                targetDrone->health = target->health;
            }
            if (target->health <= 0) {
                if (actor.drone) {
                    if (actor.owner == 0 && actor.ship == 1) {
                        progressionRun_.tutorial = tutorialOwned_;
                        progression_.boardingDroneKill(
                            actor.drone->crewKills, true, progressionRun_);
                    }
                    incrementNativeCounter(actor.drone->crewKills);
                } else if (!target->cloned) {
                    combatExperience(member);
                    incrementNativeCounter(member.statistics[1]);
                }
                // CrewMember::OnLoop 0x4747f0: a player-owned killer (crew or
                // drone) aboard the enemy ship that kills a burning non-drone
                // target also raises ACH_ROCK_FIRE, whatever its own species.
                if (actor.owner == 0 && actor.ship == 1 && !targetDrone && target->onFire > 0) {
                    progressionRun_.tutorial = tutorialOwned_;
                    progression_.condition("ACH_ROCK_FIRE", true, progressionRun_);
                }
                if (!targetDrone) {
                    crewDeath(*target);
                }
            }
        };
        // CrewMemberOnLoop4747f0 consumes the previous animation damage
        // before animation/effects update. The current target receives it.
        if (member.pendingDamage > 0 && (target || doorTarget >= 0)) {
            hit(member.pendingDamage);
            member.pendingDamage = 0;
        }
        if (!actor.drone) {
            advanceStationHandoff(member, actor.ship, actor.owner);
        }
        if (actor.drone) {
            auto& drone = *actor.drone;
            if (catalog_.equipment(drone.blueprint).type != "BOARDER") {
                advanceDroneBase(drone, actor.owner);
            }
            drone.ionStun = std::max(0.f, drone.ionStun - Time::step);
            advanceCrewDrone(drone, actor.owner);
            member.room = actor.room = drone.crewRoom;
            member.x = drone.crewPosition.x;
            member.y = drone.crewPosition.y;
            member.slot = drone.crewSlot;
            member.targetRoom = drone.targetRoom;
            member.targetSlot = drone.crewSlot;
            member.stunTime = drone.ionStun;
            const auto* controller = findSystem(actor.owner == 0 ? systems_ : enemy_.systems, 4);
            if (!drone.powered ||
                (controller && controller->hackActive && controller->hackLevel > 1)) {
                member.stunTime = std::max(Time::step, member.stunTime);
            }
            member.blockingDoor = drone.crewBlockingDoor;
            member.path.clear();
            for (std::size_t i = std::size_t(drone.crewPathIndex); i < drone.crewPath.size(); ++i) {
                member.path.push_back({drone.crewPath[i], drone.crewPathDoors.at(i)});
            }
        } else if (!member.intruder) {
            advanceCrew(member, actor.ship == 1, actor.owner);
        } else if (member.boardPositionValid) {
            auto foreign = member;
            foreign.intruder = false;
            foreign.x = member.boardX;
            foreign.y = member.boardY;
            foreign.room = member.boardRoom;
            foreign.slot = member.boardSlot;
            foreign.targetRoom = member.boardTargetRoom;
            foreign.targetSlot = member.boardTargetSlot;
            foreign.path = member.boardPath;
            foreign.newGoal = member.boardNewGoal;
            foreign.lastDoor = member.boardLastDoor;
            foreign.blockingDoor = member.boardBlockingDoor;
            advanceCrew(foreign, actor.ship == 1, actor.owner);
            member.boardX = foreign.x;
            member.boardY = foreign.y;
            member.boardRoom = actor.room = foreign.room;
            member.boardSlot = foreign.slot;
            member.boardPath = std::move(foreign.path);
            member.boardNewGoal = foreign.newGoal;
            member.boardLastDoor = foreign.lastDoor;
            member.boardBlockingDoor = foreign.blockingDoor;
        }
        if ((actor.drone ? member.slot : member.intruder ? member.boardSlot : member.slot) >= 0) {
            // The prior animation Dead flag is observed before UpdateHealth.
            // Its physical removal remains the next manager pass.
            if (!actor.drone && member.health <= 0 && member.deathAnimationDone) {
                advanceCrewLifecycle(member, actor.owner);
                continue;
            }
            if (actor.drone && actor.drone->crewDeathDone) {
                actor.drone->crewDead = member.dead = true;
                actor.drone->ionStun = 0;
                continue;
            }
            advanceCrewHealth(member, actor.owner, actor.drone != nullptr);
            if (!actor.drone) {
                advanceCrewLifecycle(member, actor.owner);
            }
            if (member.health > 0 && member.stunTime <= 0 &&
                (!actor.drone || actor.drone->powered)) {
                advanceCrewRepair(member, actor.owner, actor.ship);
            }
            updateCrewHealthPresentation(member);
        }
        if (actor.drone) {
            actor.drone->health = member.health;
        }
        if (actor.drone && member.health <= 0) {
            auto& drone = *actor.drone;
            if (!drone.crewDying) {
                drone.crewDying = member.dying = true;
                drone.crewDeathElapsed = 0;
                drone.crewDeathDone = false;
                drone.crewDeathLeft = member.facing == "left";
            }
            if (drone.crewDeathDone) {
                drone.crewDead = member.dead = true;
                drone.ionStun = 0;
            } else {
                const auto& death =
                    catalog_.animation(member.appearance.species +
                                       (drone.crewDeathLeft ? "_death_left" : "_death_right"));
                const float elapsed = drone.crewDeathElapsed + Time::step;
                drone.crewDeathDone = elapsed > death.duration;
                drone.crewDeathElapsed = std::min(death.duration, elapsed);
            }
        }
        // CrewAnimation::OnUpdate 0x473600 follows UpdateRepair. Status 4, the
        // extinguishing strip, needs repairing (CrewMember::OnLoop 0x474d62:
        // currentRepair && CanRepair, so never an intruder) in a room whose
        // iOnFire is set, and a body that is not dying, teleporting, stunned,
        // walking or fighting; UpdateFiring then aims the smoke emitter.
        const bool repairDrone = actor.drone && member.appearance.species == "repair";
        const bool canRepair = actor.drone ? repairDrone && actor.drone->powered
                                           : (actor.owner != actor.ship) == member.controlled;
        const auto& walkPath = member.intruder && !actor.drone ? member.boardPath : member.path;
        if (canRepair && member.repairKind != 0 && member.onFire > 0 && member.health > 0 &&
            !member.dead && member.teleportArrive <= 0 && member.teleportLeaveKind == 0 &&
            member.stunTime <= 0 && walkPath.empty() && (actor.drone || !crewHasOpponent(member))) {
            if (member.appearance.species == "rock") {
                // RockAnimation::UpdateFiring 0x475fd0 forces fire_down and
                // stomps in advanceCrewRepair.
                smoke.fireDirection = 0;
            } else {
                smoke.fireDirection = crewFireDirection(member, actor.ship);
                updateCrewFiring(smoke, smoke.fireDirection);
            }
        }
        // CrewAnimation::OnUpdateEffects 0x47d360 then runs after OnUpdate's
        // punch and shooting draws below: the smoke emitter first, then the
        // shots, whose StartedDeath draws the next hit even without a target.
        // IonDroneAnimation::OnUpdateEffects 0x4379d0 has no smoke emitter.
        bool shotDied = false;

        struct EffectsUpdate {
            std::function<void()> apply;

            ~EffectsUpdate() {
                apply();
            }
        } updateEffects{[&] {
            if (member.appearance.species != "boarder_ion" && !member.dead) {
                advanceCrewSmoke(smoke, member.appearance.species);
            }
            if (shotDied) {
                member.pendingDamage = float(combatRandom_.next() % 4001 + 3000) / 1000.f;
            }
        }};

        const auto currentOrigin = crewCombatPoint(member);
        if (member.attackFlight > 0) {
            member.attackFlight = std::max(0.f, member.attackFlight - Time::step);
            if (member.attackFlight <= Time::step) {
                member.attackFlight = 0;
                shotDied = true;
            }
        }
        const bool moving = member.intruder ? !member.boardPath.empty() : !member.path.empty();
        const auto& path = member.intruder && !actor.drone ? member.boardPath : member.path;
        const bool atDoor = doorTarget >= 0 && !path.empty() &&
                            std::abs(currentOrigin.x - path.front().point.x) < .5f &&
                            std::abs(currentOrigin.y - path.front().point.y) < .5f;
        if ((!target && doorTarget < 0) || member.health <= 0 || member.stunTime > 0 ||
            member.teleportArrive > 0 || member.teleportLeaveKind != 0 ||
            (!atDoor && (moving || !atFinalGoal(actor))) || (nonFighter && doorTarget < 0)) {
            member.attackAnimation.clear();
            member.attackGoal = 0;
            member.attackElapsed = 0;
            member.attackAnimationElapsed = 0;
            continue;
        }
        const auto destination = doorTarget >= 0 ? [&] {
            const auto& door =
                (actor.ship == 0 ? ship_ : enemy_.ship).doors[std::size_t(doorTarget)];
            return SDL_FPoint{float(door.x + (door.vertical ? 0 : 17)),
                              float(door.y + (door.vertical ? 17 : 0))};
        }()
                                                 : crewCombatPoint(*target);
        const float dx = destination.x - currentOrigin.x;
        const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& player) {
            return &player == &member;
        });
        const bool away = actor.drone   ? actor.ship != actor.owner
                          : playerOwned ? member.intruder
                                        : actor.ship == 0;
        const float dy = melee && doorTarget < 0 ? (away != member.controlled ? -14.f : 14.f)
                                                 : destination.y - currentOrigin.y;
        const std::string direction =
            std::abs(dx) > std::abs(dy) ? (dx < 0 ? "left" : "right") : (dy < 0 ? "up" : "down");
        if (member.appearance.species == "boarder_ion" && doorTarget >= 0) {
            // IonDroneAnimation::UpdateShooting43c500 substitutes the
            // authored directional door strip. Frame zero emits one damage
            // draw each cycle; the door still removes one health per hit.
            member.attackAnimation = "shoot_" + direction;
            member.facing = direction;
            member.attackGoal =
                catalog_.animation("boarder_ion_" + member.attackAnimation).duration;
            if (!member.attackHit) {
                member.pendingDamage = float(combatRandom_.next() % 4001 + 3000) / 1000.f;
                member.attackHit = true;
            }
            member.attackElapsed += Time::step;
            member.attackAnimationElapsed = member.attackElapsed;
            if (member.attackElapsed > member.attackGoal) {
                member.attackElapsed = member.attackAnimationElapsed = 0;
                member.attackHit = false;
            }
            continue;
        }
        // CrewAnimationOnUpdate473600 selects status1 only for ordinary
        // shared-spot actors; every CrewDrone uses its ranged virtual path.
        const bool punching = melee && !actor.drone;
        const std::string animation = (punching ? "punch_" : "shoot_") + direction;
        const bool changed = member.attackAnimation != animation;
        if (punching) {
            // OnUpdate checks animation completion first, then timer Done
            // BEFORE Update. The strict timer threshold emits damage once.
            if (changed || member.attackGoal <= 0 ||
                member.attackAnimationElapsed >= member.attackGoal * 2.f) {
                member.attackGoal = float(combatRandom_.next() % 150 + 500) / 1000.f;
                member.attackElapsed = member.attackAnimationElapsed = 0;
                member.attackHit = false;
            } else if (member.attackElapsed > member.attackGoal && !member.attackHit) {
                member.pendingDamage = float(combatRandom_.next() % 4001 + 3000) / 1000.f;
                member.attackHit = true;
            }
            member.attackElapsed += Time::step;
            member.attackAnimationElapsed += Time::step;
        } else {
            if (member.shootGoal == 0) {
                // Legacy checkpoints did not retain this constructor timer.
                initializeCrewAnimation(member, combatRandom_);
            }
            member.attackGoal =
                catalog_.animation(member.appearance.species + "_" + animation).duration;
            if (changed) {
                member.attackAnimationElapsed = 0;
            }
            member.attackAnimationElapsed += Time::step;
            if (member.attackAnimationElapsed > member.attackGoal) {
                member.attackAnimationElapsed = 0;
            }
            member.shootElapsed += Time::step;
            if (member.shootElapsed > member.shootGoal) {
                // TimerHelperDone5a9640 rerolls the loop before the shot's
                // aim jitter and randomized sound alias are selected.
                member.shootElapsed = 0;
                member.shootGoal = float(combatRandom_.next() % 300 + 1000) / 1000.f;
                member.attackFrom = {float(int(currentOrigin.x)), float(int(currentOrigin.y))};
                member.attackTo = destination;
                if (doorTarget < 0) {
                    // Ordinary CrewMemberExactTarget720c70 is false; doors
                    // are exact targets and do not consume jitter draws.
                    member.attackTo.x += float(int(combatRandom_.next() % 17) - 8);
                    member.attackTo.y += float(int(combatRandom_.next() % 17) - 8);
                }
                member.attackFlight = std::max(Time::step,
                                               std::hypot(member.attackTo.x - member.attackFrom.x,
                                                          member.attackTo.y - member.attackFrom.y) /
                                                   160.f);
                combatSound({"crewLaser" + std::to_string(combatRandom_.next() % 3 + 1)}, tick_);
            }
        }
        member.attackAnimation = animation;
        member.facing = direction;
    }
    // UpdateCrewmembers49ee10 assigns new targets only AFTER the whole
    // physical first pass. Even non-fighters/nonhostile rooms consume the draw.
    for (auto& actor : actors) {
        if (actor.ship != physicalShip || actor.member->dead || actor.member->outOfGame ||
            actor.room < 0) {
            continue;
        }
        auto& member = *actor.member;
        std::vector<const Actor*> candidates;
        for (const auto& other : actors) {
            if (other.ship != actor.ship || other.room != actor.room ||
                other.hostile == actor.hostile || other.member->dead || other.member->dying ||
                other.member->outOfGame || member.dead) {
                continue;
            }
            candidates.push_back(&other);
        }
        if (!candidates.empty()) {
            // UpdateCrewmembers 0x49ee10 draws once for each live member
            // with room opponents, including stunned/moving/non-fighting
            // actors. A shared final slot then overrides the random choice.
            const auto* selected = candidates[combatRandom_.next() % candidates.size()];
            const auto previous = std::ranges::find_if(candidates, [&](const Actor* candidate) {
                return candidate->owner == member.attackTargetOwner &&
                       (candidate->drone != nullptr) == member.attackTargetDrone &&
                       (candidate->drone
                            ? candidate->drone->crewActorId == member.attackTargetDroneId
                            : candidate->index == member.attackTargetIndex);
            });
            // SetCurrentTarget 0x471e80 retains its existing equal-priority
            // target. CrewMember::GetPriority 0x720c80 returns 100.
            if (previous != candidates.end()) {
                selected = *previous;
            }
            for (const auto* candidate : candidates) {
                if (slot(actor) == slot(*candidate) && atFinalGoal(actor) &&
                    atFinalGoal(*candidate)) {
                    selected = candidate;
                }
            }
            member.attackTargetOwner = selected->owner;
            member.attackTargetIndex = selected->index;
            member.attackTargetDrone = selected->drone != nullptr;
            member.attackTargetDroneId = selected->drone ? selected->drone->crewActorId : 0;
        } else {
            member.attackTargetOwner = member.attackTargetIndex = -1;
            member.attackTargetDrone = false;
            member.attackTargetDroneId = 0;
        }
        // Player-manager hostility stays enabled even in boarder-only encounters.
        if ((physicalShip == 1 && !hostile_) || member.appearance.species == "repair" ||
            member.appearance.species == "boarder_ion") {
            member.attackTargetOwner = member.attackTargetIndex = -1;
            member.attackTargetDrone = false;
            member.attackTargetDroneId = 0;
        }
    }
    for (const auto& actor : actors) {
        if (!actor.drone) {
            continue;
        }
        auto& drone = *actor.drone;
        const auto& body = *actor.member;
        drone.health = body.health;
        drone.repairKind = body.repairKind;
        drone.repairTarget = body.repairTarget;
        drone.repairRoom = body.repairRoom;
        drone.crewOnFire = body.onFire;
        drone.medicalRate = body.medicalRate;
        drone.healingElapsed = body.healingElapsed;
        drone.mindElapsed = body.mindElapsed;
        drone.stunElapsed = body.stunElapsed;
        drone.attackAnimation = body.attackAnimation;
        drone.attackElapsed = body.attackElapsed;
        drone.attackGoal = body.attackGoal;
        drone.shootGoal = body.shootGoal;
        drone.shootElapsed = body.shootElapsed;
        drone.pendingDamage = body.pendingDamage;
        drone.attackAnimationElapsed = body.attackAnimationElapsed;
        drone.attackHit = body.attackHit;
        drone.attackFlight = body.attackFlight;
        drone.attackFrom = body.attackFrom;
        drone.attackTo = body.attackTo;
        drone.attackTargetOwner = body.attackTargetOwner;
        drone.attackTargetIndex = body.attackTargetIndex;
        drone.attackTargetDrone = body.attackTargetDrone;
        drone.attackTargetDroneId = body.attackTargetDroneId;
    }
}

void Run::advanceCrewCombat() {
    advancePhysicalCrew(0);
    if (enemy_.present) {
        advancePhysicalCrew(1);
    }
}

void Run::finishCrewDeath(RunCrew& member) {
    if (member.outOfGame) {
        return;
    }
    const auto player = std::ranges::find_if(crew_, [&](const auto& entry) {
        return &entry == &member;
    });
    const auto enemy = std::ranges::find_if(enemy_.crew, [&](const auto& entry) {
        return &entry == &member;
    });
    const int owner = player != crew_.end() ? 0 : 1;
    const bool foreign = player == crew_.end() && enemy == enemy_.crew.end();
    const int physicalShip = foreign ? 0 : member.intruder ? 1 - owner : owner;
    const int door =
        member.intruder && member.boardPositionValid ? member.boardLastDoor : member.lastDoor;
    auto& fake = physicalShip == 0 ? fakeDoors_ : enemy_.fakeDoors;
    auto& timers = physicalShip == 0 ? doorTimers_ : enemy_.doorTimers;
    if (door >= 0 && std::size_t(door) < fake.size()) {
        fake[std::size_t(door)] = false;
        timers[std::size_t(door)] = 0;
    }
    // SetOutOfGame46c410 assigns the global death order on physical removal.
    member.dead = member.outOfGame = true;
    member.controlled = false;
    member.stunTime = 0;
    member.selected = false;
    member.deathNumber = ++crewDeathCount_;
    member.path.clear();
    member.boardPath.clear();
    member.lastDoor = member.boardLastDoor = -1;
    member.blockingDoor = member.boardBlockingDoor = -1;
    const int origin = member.enemyCrewOrigin;
    int index = owner == 0                   ? int(player - crew_.begin())
                : enemy != enemy_.crew.end() ? int(enemy - enemy_.crew.begin())
                                             : origin;
    if (origin >= 0) {
        auto record = member;
        record.enemyCrewOrigin = -1;
        record.intruder = false;
        record.boardRoom = -1;
        clearForeignPosition(record);
        if (std::size_t(origin) < enemy_.crew.size()) {
            const auto& home = enemy_.crew[std::size_t(origin)];
            record.room = home.room;
            record.slot = home.slot;
            record.x = home.x;
            record.y = home.y;
            record.targetRoom = home.targetRoom;
            record.targetSlot = home.targetSlot;
            enemy_.crew[std::size_t(origin)] = record;
        }
        if (!campaign_.fighting && campaign_.savedBoss.is_object()) {
            auto& roster = campaign_.savedBoss.at("enemy").at("crew");
            if (std::size_t(origin) < roster.size()) {
                roster.at(origin) = record;
            }
        }
        member.enemyCrewOrigin = -1;
    }
    if (member.cloneReady && index >= 0) {
        auto& pending = owner == 0 ? cloneQueue_ : enemy_.cloneQueue;
        const auto& roster = owner == 0 ? crew_ : enemy_.crew;
        if (std::size_t(index) < roster.size() &&
            std::ranges::find(pending, index) == pending.end()) {
            pending.push_back(index);
            std::stable_sort(pending.begin(), pending.end(), [&](int first, int second) {
                return roster.at(std::size_t(first)).deathNumber <
                       roster.at(std::size_t(second)).deathNumber;
            });
        }
    }
    if (foreign) {
        member.cloneReady = false;
    }
    member.intruder = false;
    member.boardRoom = -1;
    clearForeignPosition(member);
}

void Run::killCrew(RunCrew& member, bool noClone) {
    // Kill4701c0 bypasses the death strip; KillEveryone4a0210 immediately
    // updates the physical list, preserving DNA unless noClone is requested.
    member.health = 0;
    member.dead = true;
    member.dying = false;
    member.stunTime = 0;
    member.controlled = false;
    member.attackTargetOwner = member.attackTargetIndex = -1;
    member.attackTargetDrone = false;
    member.attackTargetDroneId = 0;
    if (noClone) {
        member.cloneReady = false;
        const auto erase = [&](auto& roster, auto& queue) {
            for (std::size_t i = 0; i < roster.size(); ++i) {
                if (&roster[i] == &member) {
                    std::erase(queue, int(i));
                }
            }
        };
        erase(crew_, cloneQueue_);
        erase(enemy_.crew, enemy_.cloneQueue);
    }
    finishCrewDeath(member);
}

void Run::advanceCrewHealth(RunCrew& member, int owner, bool drone) {
    if (member.dead || member.outOfGame || member.health <= 0) {
        return;
    }
    const float maximum = crewMaximumHealth(member.appearance.species) + member.healthBoost;
    const auto modify = [&](float amount) {
        if (member.health <= 0) {
            return;
        }
        const float previous = member.health;
        member.health = std::clamp(member.health + amount, 0.f, maximum);
        if (!drone) {
            noteCrewHealthChange(member, previous);
        }
        if (int(previous) > 25 && int(member.health) <= 25) {
            member.lowHealthTick = animation_;
        }
    };
    // UpdateHealth470a20 uses LAST UpdateCrewmembers metadata, in this order.
    // CrewDrone cannot burn/suffocate and scales its SetMedbay rate by .2.
    if (!drone && member.appearance.species != "rock") {
        modify(-float(member.onFire) * .133f * Time::originalStep);
    }
    if (!drone && member.suffocating && member.appearance.species != "anaerobic") {
        float resistance = member.appearance.species == "crystal" ? .5f : 1.f;
        float masks = 0;
        bool masked = false;
        for (const auto& augment : owner == 0 ? ship_.augments : enemy_.ship.augments) {
            if (augment == "O2_MASKS" && (owner == 0 || enemy_.present)) {
                masked = true;
                masks += catalog_.equipment(augment).value;
            }
        }
        resistance *= masked ? masks : 1.f;
        modify(-.4f * resistance * Time::originalStep);
    }
    const float rate =
        member.medicalRate > 1.f ? (member.medicalRate - 1.f) * 1.5f : member.medicalRate;
    modify(.4f * rate * (drone ? .2f : 1.f) * Time::originalStep);
    if (member.health <= 0 && !drone) {
        crewDeath(member);
    }
}

void Run::advanceCrewLifecycle(RunCrew& member, int owner) {
    if (member.outOfGame) {
        return;
    }
    // UpdateCrewmembers49ee10 removes prior bDead before calling OnLoop.
    if (member.dead) {
        finishCrewDeath(member);
        return;
    }
    member.cloneReady = findSystem(owner == 0 ? systems_ : enemy_.systems, 13) != nullptr;
    if (member.health > 0) {
        return;
    }
    if (!member.dying) {
        crewDeath(member);
    }
    // CrewMember4747f0 observes CrewAnimation::Dead BEFORE OnUpdate.
    if (member.deathAnimationDone) {
        member.dead = true;
        member.stunTime = 0;
        member.controlled = false;
        member.selected = false;
        const bool foreign = member.enemyCrewOrigin >= 0 ||
                             std::ranges::any_of(voyageBoarders_, [&](const auto& entry) {
                                 return &entry == &member;
                             });
        const int ship = foreign ? 0 : member.intruder ? 1 - owner : owner;
        const int door =
            member.intruder && member.boardPositionValid ? member.boardLastDoor : member.lastDoor;
        auto& fake = ship == 0 ? fakeDoors_ : enemy_.fakeDoors;
        auto& timers = ship == 0 ? doorTimers_ : enemy_.doorTimers;
        if (door >= 0 && std::size_t(door) < fake.size()) {
            fake[std::size_t(door)] = false;
            timers[std::size_t(door)] = 0;
        }
        member.lastDoor = member.boardLastDoor = -1;
        member.blockingDoor = member.boardBlockingDoor = -1;
        return;
    }
    const auto& animation = catalog_.animation(
        member.appearance.species + (member.facing == "left" ? "_death_left" : "_death_right"));
    const float advanced = member.deathAnimationElapsed + Time::step;
    member.deathAnimationDone = advanced > animation.duration;
    member.deathAnimationElapsed = std::min(animation.duration, advanced);
}

void Run::advanceCrewLifecycle() {
    for (auto& member : crew_) {
        advanceCrewLifecycle(member, 0);
    }
    for (auto& member : enemy_.crew) {
        if (enemy_.present || member.intruder) {
            advanceCrewLifecycle(member, 1);
        }
    }
    for (auto& member : voyageBoarders_) {
        advanceCrewLifecycle(member, 1);
    }
    for (int owner : {0, 1}) {
        for (auto* physical : physicalCrewDrones(owner)) {
            auto& drone = *physical;
            const auto& blueprint = catalog_.equipment(drone.blueprint);
            if ((blueprint.type != "REPAIR" && blueprint.type != "BATTLE" &&
                 blueprint.type != "BOARDER") ||
                drone.crewPhysicalShip < 0 || drone.crewRoom < 0 || drone.health > 0 ||
                drone.crewDead) {
                continue;
            }
            if (!drone.crewDying) {
                drone.crewDying = true;
                drone.crewDeathElapsed = 0;
                drone.crewDeathDone = false;
                drone.crewDeathLeft = drone.attackAnimation.ends_with("_left");
                if (drone.attackAnimation.empty()) {
                    drone.crewDeathLeft = drone.velocity.x < 0;
                }
            }
            if (drone.crewDeathDone) {
                // CrewMember4747f0 observes the prior animation Dead result.
                // Next CrewDrone43ce20 pass performs SetDestroyed, not this one.
                drone.crewDead = true;
                drone.ionStun = 0;
                continue;
            }
            const auto species = blueprint.type == "REPAIR"      ? std::string("repair")
                                 : blueprint.id == "BOARDER_ION" ? std::string("boarder_ion")
                                                                 : std::string("battle");
            const auto& animation = catalog_.animation(
                species + (drone.crewDeathLeft ? "_death_left" : "_death_right"));
            const float advanced = drone.crewDeathElapsed + Time::step;
            drone.crewDeathDone = advanced > animation.duration;
            drone.crewDeathElapsed = std::min(animation.duration, advanced);
        }
    }
}

void Run::updateCrewHealthPresentation(RunCrew& member) {
    // CrewMember4747f0 advances these independent looping animations
    // after health updates. AnimationTracker59ade0 retains the final
    // frame for a step before wrapping; Stop(false) resets its time.
    const auto overlay = [](float& elapsed, bool active) {
        if (!active) {
            elapsed = -1;
        } else {
            if (elapsed < 0 || elapsed == 1.f) {
                elapsed = 0;
            }
            elapsed = std::min(1.f, elapsed + Time::step);
        }
    };
    member.healthChangeRemaining = std::max(0.f, member.healthChangeRemaining - Time::step);
    const float maximum = crewMaximumHealth(member.appearance.species) + member.healthBoost;
    // DirectModifyHealth 0x46eb20 and OnLoop 0x4747f0: two game seconds,
    // refreshed only by a real change whose result is below maximum.
    if (member.health != member.lastPresentedHealth && member.health < maximum) {
        member.healthChangeRemaining = 2.f;
    }
    member.lastPresentedHealth = member.health;
    overlay(member.healingElapsed, member.health > 0 && member.medicalRate > 0);
    overlay(member.stunElapsed, member.health > 0 && member.stunTime > 0);
    overlay(member.mindElapsed, member.health > 0 && member.controlled);
}

void Run::updateCrewHealthPresentation() {
    for (auto& member : crew_) {
        updateCrewHealthPresentation(member);
    }
    for (auto& member : enemy_.crew) {
        updateCrewHealthPresentation(member);
    }
    for (auto& member : voyageBoarders_) {
        updateCrewHealthPresentation(member);
    }
    const auto overlay = [](float& elapsed, bool active) {
        if (!active) {
            elapsed = -1;
        } else {
            if (elapsed < 0 || elapsed == 1.f) {
                elapsed = 0;
            }
            elapsed = std::min(1.f, elapsed + Time::step);
        }
    };
    for (int owner : {0, 1}) {
        for (auto* physical : physicalCrewDrones(owner)) {
            auto& drone = *physical;
            if (drone.crewDead) {
                continue;
            }
            const bool alive = drone.crewPhysicalShip >= 0 && !drone.crewDead && drone.health > 0;
            // Same rule as updateCrewHealthPresentation for the drone's crew body.
            drone.healthChangeRemaining = std::max(0.f, drone.healthChangeRemaining - Time::step);
            const auto& type = catalog_.equipment(drone.blueprint);
            const float maximum = type.id == "BOARDER_ION" ? 125.f
                                  : type.type == "REPAIR"  ? 25.f
                                                           : 150.f;
            if (drone.lastPresentedHealth >= 0 && drone.health != drone.lastPresentedHealth &&
                drone.health < maximum) {
                drone.healthChangeRemaining = 2.f;
            }
            drone.lastPresentedHealth = drone.health;
            overlay(drone.healingElapsed, alive && drone.medicalRate > 0);
            overlay(drone.stunElapsed, alive && drone.ionStun > 0);
            overlay(drone.mindElapsed, false);
        }
    }
}

float Run::crewHealthAlpha(const RunCrew& member) const {
    if (member.health <= 0) {
        return 0;
    }
    const float maximum = crewMaximumHealth(member.appearance.species) + member.healthBoost;
    // OnRenderHealth 0x471270: selection, recent change, or below 55%.
    if (!member.selected && member.healthChangeRemaining <= 0 && member.health >= .55f * maximum) {
        return 0;
    }
    const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& player) {
        return &player == &member;
    });
    return member.selected || !playerOwned ? 1.f : .7f;
}

void Run::renderCrewShots(const RunCrew& member, float x, float y) {
    if (member.attackFlight <= 0) {
        return;
    }
    const float distance = std::hypot(member.attackTo.x - member.attackFrom.x,
                                      member.attackTo.y - member.attackFrom.y);
    const float progress =
        distance > 0 ? std::clamp(1.f - member.attackFlight * 160.f / distance, 0.f, 1.f) : 1.f;
    const float px = x + std::lerp(member.attackFrom.x, member.attackTo.x, progress);
    const float py = y + std::lerp(member.attackFrom.y, member.attackTo.y, progress);
    const auto& frame = catalog_.animation("crewLaser");
    if (!frame.usable()) {
        return;
    }
    const float angle = std::atan2(member.attackTo.y - member.attackFrom.y,
                                   member.attackTo.x - member.attackFrom.x) *
                            180.f / 3.14159265f +
                        90.f;
    // CrewAnimation::SetupStrips 0x477d91: human, anaerobic, engi/energy and
    // slug shots use COLOR_GREEN (100,255,100) for the player's crew
    // (bPlayer) and COLOR_RED (255,50,50) otherwise; other races COLOR_WHITE.
    // CrewAnimation::OnRenderProps 0x46c710 tints every shot with it.
    const auto& race = member.appearance.species;
    const bool tinted = race == "human" || race == "anaerobic" || race == "engi" ||
                        race == "energy" || race == "slug";
    const bool playerOwned = std::ranges::any_of(crew_, [&](const auto& player) {
        return &player == &member;
    });
    const SDL_Color color = !tinted       ? SDL_Color{255, 255, 255, 255}
                            : playerOwned ? SDL_Color{100, 255, 100, 255}
                                          : SDL_Color{255, 50, 50, 255};
    // CrewAnimation::SetupStrips 0x477d91 loads "crewLaser" (0x82c6d4) and then
    // SetAnimationId(projectile, baseStrip): the shot is that frame of the
    // race's own base sheet (female humans use female_base), not the human one.
    const auto& species = member.appearance.species;
    const std::string sheet = species == "human" && (member.appearance.appearance % 2) != 0
                                  ? std::string("female")
                                  : species;
    // Animation desc rows count from the bottom of the sheet, so keep the
    // frame's distance from the bottom when moving it onto the race sheet.
    auto& human = renderer_.image(frame.path);
    auto& raceSheet = renderer_.image("img/people/" + sheet + "_base.png");
    const float fromBottom = float(human.h) - frame.y - float(frame.height);
    renderer_.sprite(
        raceSheet,
        {px - frame.width / 2.f, py - frame.height / 2.f, float(frame.width), float(frame.height)},
        {float(frame.x),
         float(raceSheet.h) - fromBottom - float(frame.height),
         float(frame.width),
         float(frame.height)},
        angle,
        color);
}

void Run::renderCrewHealth(const RunCrew& member, float x, float y, int ownerOverride) {
    // CrewMember::OnRenderHealth471270 calls animation props before the
    // position-shifted healing, health and stun/mind overlays. All bodies have
    // already been drawn by ShipManager49abc0's preceding traversal.
    if (member.health <= 0) {
        return;
    }
    renderCrewShots(member, x, y);
    const auto point = crewPresentationPoint(member);
    const float px = x + std::trunc(point.x);
    const float py = y + std::trunc(point.y);
    const auto strip = [&](const std::string& path,
                           int frames,
                           int size,
                           float elapsed,
                           float left,
                           float top,
                           SDL_Color color) {
        if (elapsed < 0) {
            return;
        }
        const int frame = std::min(frames - 1, int(elapsed * frames));
        renderer_.sprite(renderer_.image(path),
                         {left, top, float(size), float(size)},
                         {float(frame * size), 0, float(size), float(size)},
                         0,
                         color);
    };
    strip("img/people/healing_strip11.png",
          11,
          32,
          member.healingElapsed,
          x + point.x - 17,
          y + point.y - 20,
          {255, 255, 255, 255});
    const bool playerOwned = ownerOverride >= 0
                                 ? ownerOverride == 0
                                 : std::ranges::any_of(crew_, [&](const auto& player) {
                                       return &player == &member;
                                   });
    const SDL_Color allegiance = playerOwned           ? SDL_Color{0, 255, 0, 255}
                                 : options_.colorblind ? SDL_Color{0, 147, 224, 255}
                                                       : SDL_Color{255, 0, 0, 255};
    const bool hovered = panel_.empty() && !event_ && !mapOpen_ && jumpTicks_ == 0 &&
                         arrivalTicks_ == 0 &&
                         Rect{px - 17, py - 17, 35, 35}.contains(mouseX_, mouseY_);
    // Hover selection exposes even a full health bar, as in the supplied
    // original CrewMember::OnRenderHealth reference.
    const float alpha = hovered ? 1.f
                        : ownerOverride == 0 && !member.selected && crewHealthAlpha(member) > 0
                            ? .7f
                            : crewHealthAlpha(member);
    if (alpha > 0) {
        renderer_.draw(member.health <= 25 && member.healthFlashTime < .5f
                           ? "img/people/health_box_red.png"
                           : "img/people/health_box.png",
                       px - 15,
                       py - 20,
                       1.f,
                       alpha);
        const float maximum = crewMaximumHealth(member.appearance.species) + member.healthBoost;
        renderer_.fill(
            {px - 10, py - 17, std::round(25 * std::clamp(member.health / maximum, 0.f, 1.f)), 3},
            allegiance);
    }
    if (member.stunTime > 0) {
        strip("img/people/stun_strip5.png",
              5,
              14,
              member.stunElapsed,
              px - 27,
              py - 20,
              {255, 255, 50, 255});
    } else if (member.controlled) {
        strip("img/people/mindcontrol_strip8.png",
              8,
              14,
              member.mindElapsed,
              px - 27,
              py - 20,
              allegiance);
    }
}

void Run::resetAdvanced() {
    for (auto& sys : systems_) {
        if (sys.definition.id == enemy_.hackingTarget) {
            sys.hackActive = false;
            sys.hackLevel = 0;
        }
    }
    enemy_.hacking = {};
    enemy_.hackingTarget = -1;
    resetCrewDroneDeployment(enemy_.hackingDrone);
    enemy_.hackingDroneId = 0;
    enemy_.hackingDroneSpace = 1;
    for (auto& sys : enemy_.systems) {
        if (sys.definition.id == hackingTarget_) {
            sys.hackActive = false;
            sys.hackLevel = 0;
        }
    }
    for (auto& crew : enemy_.crew) {
        clearCrewCombat(crew);
        crew.controlled = false;
        if (crew.healthBoost > 0) {
            crew.health = std::min(crew.health, crewMaximumHealth(crew.appearance.species));
            crew.healthBoost = 0;
        }
    }
    // A boarding party stays in the player's own crew list because the original's
    // CrewMemberFactory counts it for CompleteShip::DeadCrew 0x409930: an empty
    // player ship with a live party aboard the enemy is not a defeat. Clearing the
    // intruder flag (CrewMember::GetIntruder 0x46c3e0) disembarks the party at the
    // teleporter-room position this representation always keeps valid.
    for (auto& crew : crew_) {
        clearCrewCombat(crew);
        crew.intruder = false;
        crew.boardRoom = -1;
        crew.boardPositionValid = false;
        crew.lastDoor = crew.boardLastDoor = -1;
        crew.blockingDoor = crew.boardBlockingDoor = -1;
        crew.boardTargetRoom = -1;
        crew.boardPath.clear();
        crew.controlled = false;
        if (crew.healthBoost > 0) {
            crew.health = std::min(crew.health, crewMaximumHealth(crew.appearance.species));
            crew.healthBoost = 0;
        }
    }
    for (auto& member : voyageBoarders_) {
        clearCrewCombat(member);
        member.controlled = false;
        if (member.healthBoost > 0) {
            member.health = std::min(member.health, crewMaximumHealth(member.appearance.species));
            member.healthBoost = 0;
        }
    }
    enemy_.mind = {};
    enemy_.mindTarget = -1;
    enemy_.mindTargetOwner = 0;
    teleportCharge_ = 0;
    teleportArmed_ = 0;
    teleportTarget_ = -1;
    hackingTarget_ = mindTarget_ = -1;
    mindTargetOwner_ = 1;
    hacking_ = mind_ = {};
    // The jump drops the old location's crew; a queue still pointing at them (the
    // original keeps raw pointers) is not carried to the next beacon.
    mindQueue_ = {};
    mindResisted_.clear();
    resetCrewDroneDeployment(hackingDrone_);
    hackingDroneSpace_ = 0;
    hackingDroneId_ = 0;
}

void Run::updateCrewAudio() {
    // UpdateCrewmembers49ee10 issues these counts only for the physical
    // player ShipManager. GetUniqueRepairing varies by race, not system.
    float repair = 0;
    float shrikeRepair = 0;
    float extinguish = 0;
    float healing = 0;
    float brawling = 0;
    if (open_ && !defeated_) {
        const auto working = [&](int room, bool ready, bool opponent, const std::string& species) {
            if (!ready || opponent) {
                return;
            }
            const auto hazard = playerHazards_.find(room);
            const bool burning = hazard != playerHazards_.end() && hazard->second.fires > 0;
            if (burning) {
                // Rock73ef90 and Crystal726160 suppress the work loop.
                if (species != "rock" && species != "crystal") {
                    ++extinguish;
                }
                return;
            }
            const bool damaged =
                (hazard != playerHazards_.end() && hazard->second.breaches > 0) ||
                std::ranges::any_of(systems_, [&](const RunSystem& sys) {
                    return sys.definition.room == room && sys.health < sys.definition.power;
                });
            if (!damaged) {
                return;
            }
            if (species == "anaerobic") {
                ++shrikeRepair; // GetUniqueRepairing72bdd0.
            } else if (species != "rock" && species != "crystal") {
                ++repair;
            }
        };
        const auto member = [&](const RunCrew& crew, bool foreign) {
            if (crew.health <= 0) {
                return;
            }
            brawling += !crew.attackAnimation.empty() ? 1.f : 0.f;
            const bool atHome = foreign || !crew.intruder;
            const bool friendly = foreign ? crew.controlled : !crew.controlled;
            if (!atHome || !friendly) {
                return;
            }
            const bool opponent = crewHasOpponent(crew);
            const bool validGoal =
                crew.targetRoom >= 0 && crew.targetSlot >= 0 &&
                std::ranges::any_of(ship_.rooms, [&](const RoomDefinition& room) {
                    return room.id == crew.targetRoom;
                });
            const auto point =
                validGoal ? slotPoint(crew.targetRoom, crew.targetSlot) : SDL_FPoint{};
            const bool ready = validGoal && crew.path.empty() && crew.room == crew.targetRoom &&
                               std::abs(crew.x - point.x) < .5f &&
                               std::abs(crew.y - point.y) < .5f && crew.stunTime <= 0;
            working(crew.room, ready, opponent, crew.appearance.species);
            const auto* med = system(5);
            if (!med || med->health <= 0 || med->power <= 0 ||
                (med->hackActive && med->hackLevel > 1)) {
                return;
            }
            const auto hazard = playerHazards_.find(crew.room);
            const bool fire = hazard != playerHazards_.end() && hazard->second.fires > 0;
            const auto oxygen = roomOxygen_.find(crew.room);
            const bool suffocating = crew.appearance.species != "anaerobic" &&
                                     (oxygen == roomOxygen_.end() || oxygen->second <= 5);
            const bool canHeal =
                crew.health < crewMaximumHealth(crew.appearance.species) + crew.healthBoost ||
                suffocating || fire || !crew.attackAnimation.empty();
            float nano = 0;
            for (const auto& augment : ship_.augments) {
                if (augment == "NANO_MEDBAY") {
                    nano += catalog_.equipment(augment).value;
                }
            }
            healing += canHeal && (med->definition.room == crew.room || nano > 0) ? 1.f : 0.f;
        };
        for (const auto& crew : crew_) {
            member(crew, false);
        }
        for (const auto& crew : voyageBoarders_) {
            member(crew, true);
        }
        for (const auto& crew : enemy_.crew) {
            if (enemy_.present && crew.health > 0) {
                brawling += !crew.attackAnimation.empty() ? 1.f : 0.f;
            }
        }
        const auto drones = [&](int owner) {
            for (const auto* actor : physicalCrewDrones(owner)) {
                const auto& drone = *actor;
                const auto& type = catalog_.equipment(drone.blueprint).type;
                if ((type != "BATTLE" && type != "REPAIR" && type != "BOARDER") ||
                    drone.crewPhysicalShip < 0 || drone.crewDead || drone.health <= 0) {
                    continue;
                }
                brawling += !drone.attackAnimation.empty() ? 1.f : 0.f;
                if (owner != drone.crewPhysicalShip || drone.crewPhysicalShip != 0 ||
                    !drone.powered || drone.ionStun > 0) {
                    continue;
                }
                const auto* controller = system(4);
                if (controller && controller->hackActive && controller->hackLevel > 1) {
                    continue;
                }
                const bool opponent =
                    std::ranges::any_of(voyageBoarders_,
                                        [&](const RunCrew& crew) {
                                            return crew.health > 0 && !crew.controlled &&
                                                   crew.room == drone.crewRoom;
                                        }) ||
                    std::ranges::any_of(crew_,
                                        [&](const RunCrew& crew) {
                                            return crew.health > 0 && !crew.intruder &&
                                                   crew.controlled && crew.room == drone.crewRoom;
                                        }) ||
                    std::ranges::any_of(physicalCrewDrones(1), [&](const CombatDrone* other) {
                        return !other->crewDead && other->health > 0 &&
                               other->crewPhysicalShip == 0 && other->crewRoom == drone.crewRoom;
                    });
                if (type == "REPAIR") {
                    working(drone.crewRoom, drone.crewPath.empty(), opponent, "repair");
                }
            }
        };
        drones(0);
        drones(1);
    } else {
        audio_.updateLoop("airLeak", 0.f);
    }
    audio_.updateLoop("repair", repair);
    audio_.updateLoop("shrikeRepair", shrikeRepair);
    audio_.updateLoop("extinguish", extinguish);
    audio_.updateLoop("healing", healing);
    audio_.updateLoop("brawling", brawling);
}

int Run::mindLevel() const {
    // MindSystem::GetMindPowerLevel 0x609d10 returns the system's effective power,
    // or its current health while `iHackEffect > 1 && bUnderAttack`; the same
    // substitution indexes MIND_CONTROL_COUNT/TIMER/HEALTH_BOOST/DAMAGE_BOOST in
    // MindSystem::OnLoop 0x60a990 and MindSystem::GetControlLimit 0x609ee0.
    const auto* source = system(14);
    if (!source || source->health <= 0) {
        return 0;
    }
    if (source->hackActive && source->hackLevel > 1) {
        return std::clamp(source->health, 0, 3);
    }
    return std::clamp(std::min(source->power, source->health), 0, 3);
}

bool Run::advancedAction(const std::string& target, const std::string& action) {
    logging::verbose("action", "advanced '{}' {}", target, action);
    const auto controlRoom = [&](int physicalShip, int roomId) {
        const bool lifeScanner =
            std::ranges::find(ship_.augments, std::string("LIFE_SCANNER")) != ship_.augments.end();
        const bool telepath = std::ranges::any_of(crew_, [](const auto& member) {
            return (!member.outOfGame || member.cloneReady) && member.appearance.species == "slug";
        });
        // UpdateTarget564700 accepts otherwise-blackout rooms for slug
        // crew equipment or LIFE_SCANNER; control itself still filters actors.
        if (physicalShip == 1 && !enemyRoomVisible(roomId) && !lifeScanner && !telepath) {
            return;
        }
        // CombatControl::SelectTarget takes the target ShipManager's crew list
        // (GetCrewList), keeps the crew standing in the room and, when any remain,
        // calls MindSystem::QueueMindControl 0x60a040. Nothing is controlled yet: the
        // next MindSystem::OnLoop 0x60a990 filters and draws (InitiateMindControl).
        std::vector<MindQueuedCrew> queued;
        const auto add = [&](const RunCrew& member, int owner, int index, int ship, int room) {
            if (ship == physicalShip && room == roomId && !member.outOfGame) {
                queued.push_back({owner, index});
            }
        };
        for (std::size_t index = 0; index < crew_.size(); ++index) {
            const auto& member = crew_[index];
            add(member,
                0,
                int(index),
                int(member.intruder),
                member.intruder ? member.boardRoom : member.room);
        }
        for (std::size_t index = 0; index < enemy_.crew.size(); ++index) {
            const auto& member = enemy_.crew[index];
            if (!member.intruder) {
                add(member, 1, int(index), 1, member.room);
            }
        }
        for (std::size_t index = 0; index < voyageBoarders_.size(); ++index) {
            const auto& member = voyageBoarders_[index];
            add(member,
                1,
                member.enemyCrewOrigin >= 0 ? member.enemyCrewOrigin
                                            : int(enemy_.crew.size() + index),
                0,
                member.room);
        }
        if (!queued.empty()) {
            mindQueue_[0] = std::move(queued);
        }
    };
    if (target.starts_with("mind:player:") || target.starts_with("mind:player-room:")) {
        const auto* source = system(14);
        if (action != "click" || !open_ || defeated_ || event_ || mapOpen_ || jumpTicks_ > 0 ||
            !source || source->health <= 0 || source->power <= 0 || systemLocked(*source)) {
            return true;
        }
        const bool roomTarget = target.starts_with("mind:player-room:");
        const auto value = std::string_view(target).substr(roomTarget ? 17 : 12);
        int id = -1;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || id < 0 ||
            (!roomTarget && id >= int(crew_.size()))) {
            return true;
        }
        if (roomTarget) {
            if (std::ranges::any_of(ship_.rooms, [&](const auto& shape) {
                    return shape.id == id;
                })) {
                controlRoom(0, id);
                panel_.clear();
            }
            return true;
        }
        auto& crew = crew_[std::size_t(id)];
        if (!crew.controlled || crew.health <= 0 || crew.appearance.species == "slug") {
            return true;
        }
        controlRoom(int(crew.intruder), crew.intruder ? crew.boardRoom : crew.room);
        panel_.clear();
        return true;
    }
    const bool unavailable = action != "click" || !open_ || defeated_ || event_ || mapOpen_ ||
                             jumpTicks_ > 0 || !enemy_.present || enemyExplosionActive_ ||
                             (cloaked(1) && !enemyContainsPlayerCrew());
    if (target.starts_with("teleport:")) {
        if (unavailable) {
            return true;
        }
        const std::string_view value = std::string_view(target).substr(9);
        const bool send = value.starts_with("send:");
        const bool receive = value.starts_with("return:");
        const std::string_view roomText = send      ? value.substr(5)
                                          : receive ? value.substr(7)
                                                    : std::string_view{};
        if (roomText.empty()) {
            return true;
        }
        int room = -1;
        const auto parsed =
            std::from_chars(roomText.data(), roomText.data() + roomText.size(), room);
        if (parsed.ec != std::errc{} || parsed.ptr != roomText.data() + roomText.size() ||
            room < 0) {
            return true;
        }
        const auto* teleporter = system(9);
        // ShipManager::SetArmed/CombatControl::ArmTeleporter 0x565cf0 route through
        // TeleportSystem::SetArmed 0x620a90, which needs ShipSystem::Functioning
        // 0x6153d0 and a lock-free system (ShipSystem::GetLocked 0x615930), plus a
        // prepared crew for a send (TeleportSystem::CanSend 0x620c80) and the
        // ZOLTAN_BYPASS augmentation whenever the receiving ship still has super
        // shields.
        const bool functioning = teleporter && teleporter->health > 0 && teleporter->power > 0;
        const bool locked = !teleporter || systemLocked(*teleporter);
        if (!functioning || locked) {
            return true;
        }
        const bool bypass =
            std::find(ship_.augments.begin(), ship_.augments.end(), "ZOLTAN_BYPASS") !=
            ship_.augments.end();
        const bool known = std::ranges::any_of(enemy_.ship.rooms, [room](const auto& candidate) {
            return candidate.id == room;
        });
        if (!known || (enemy_.superShields > 0 && !bypass) || (send && !hostile_)) {
            return true;
        }
        if (send) {
            // ShipManager::UpdateCrewmembers 0x49ee10 clears and rebuilds the
            // teleporter's prepared-crew bits from crew standing at its final goal in
            // the teleporter room (TeleportSystem::UpdateCrewLocation 0x620a40).
            const auto prepared = std::ranges::count_if(crew_, [&](const RunCrew& crew) {
                return crew.health > 0 && !crew.intruder && !crew.controlled && crew.path.empty() &&
                       crew.room == crew.targetRoom && crew.slot == crew.targetSlot &&
                       crew.room == teleporter->definition.room;
            });
            if (prepared == 0) {
                return true;
            }
        }
        // CombatControl::GetTeleportationCommand 0x565f50 returns the armed command
        // and room, which CompleteShip::OnLoop 0x4094a0 consumes on the next tick.
        teleportArmed_ = send ? 1 : 2;
        teleportTarget_ = room;
        panel_.clear();
        return true;
    }
    if (!target.starts_with("hack:") && !target.starts_with("mind:")) {
        return false;
    }
    if (unavailable || !hostile_) {
        return true;
    }
    const auto value = std::string_view(target).substr(5);
    int id = -1;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return true;
    }
    const bool hack = target.starts_with("hack:");
    auto* source = system(hack ? 15 : 14);
    auto& ability = hack ? hacking_ : mind_;
    if (!source || source->power <= 0 || source->health <= 0 || systemLocked(*source)) {
        return true;
    }
    const int level = std::clamp(std::min(source->power, source->health), 1, 3);
    const auto previousResources = resourceAmounts();
    if (hack) {
        if (hackingDrone_.rebuildTimer > 0) {
            return true;
        }
        auto it = std::ranges::find_if(enemy_.systems, [id](const auto& sys) {
            return sys.definition.id == id;
        });
        if (it == enemy_.systems.end() || it->health <= 0 ||
            (hackingTarget_ < 0 && enemy_.superShields > 0) ||
            (hackingTarget_ >= 0 && hackingTarget_ != id) || (hackingTarget_ < 0 && parts_ <= 0)) {
            return true;
        }
        if (hackingTarget_ < 0) {
            --parts_;
            hackingTarget_ = id;
            resetCrewDroneDeployment(hackingDrone_);
            hackingDrone_.deployed = hackingDrone_.powered = true;
            hackingDrone_.hackingFlyingElapsed = hackingDrone_.hackingExtendingElapsed = -1;
            hackingDrone_.hackingFinishedSetup = false;
            hackingDrone_.health = 1;
            hackingDrone_.room = it->definition.room;
            hackingDrone_.position = {0.f, 0.f};
            hackingDrone_.destination = {800.f, 0.f};
            hackingDroneSpace_ = 0;
            hackingDroneId_ = nextProjectile_++;
            combatSound({"droneLaunch"}, tick_);
            noteResourceChanges(previousResources);
            panel_.clear();
            return true;
        }
        if (!hackingDrone_.deployed || !hackingDrone_.delivered) {
            return true;
        }
        hackingTarget_ = id;
        // InitiatePulse607e20 changes the source lock; OnLoop6083b0
        // applies the target's strong hacking level at its next callback.
        ability = {true, 0, float(1 + 3 * level), 0};
        setSystemLock(*source, -1);
        combatSound({"hackStart"}, tick_);
    } else {
        if (id < 0 || id >= static_cast<int>(enemy_.crew.size())) {
            return true;
        }
        auto& crew = enemy_.crew[id];
        if (crew.intruder || crew.dead || crew.dying || crew.outOfGame) {
            return true;
        }
        controlRoom(1, crew.room);
    }
    noteResourceChanges(previousResources);
    panel_.clear();
    return true;
}

void Run::setCrewControlled(RunCrew& member, bool controlled) {
    // SetMindControl 0x4728c0 frees the old allegiance's slot and finds a
    // new slot on the current physical ship before forcing a movement order.
    const bool changed = member.controlled != controlled;
    member.controlled = controlled;
    // SetMindControl4728c0 always clears the medical controller and station
    // registration before the next physical ShipManager crew pass.
    member.medicalRate = 0;
    if (controlled) {
        member.selected = false;
    }
    if (!changed || member.health <= 0) {
        updateManning();
        return;
    }
    const bool player = std::ranges::any_of(crew_, [&](const auto& actor) {
        return &actor == &member;
    });
    const bool residentEnemy = std::ranges::any_of(enemy_.crew, [&](const auto& actor) {
        return &actor == &member;
    });
    const bool physicalEnemy = player ? member.intruder : residentEnemy;
    const int currentRoom = player && member.intruder ? member.boardRoom : member.room;
    const bool side = (player ? member.intruder : !residentEnemy) != controlled;
    // The player-away wrapper preserves owning-ship bookkeeping and updates
    // the separate physical position/path representation.
    if (!order(member, currentRoom, -1, true, player ? false : physicalEnemy)) {
        if (player && physicalEnemy) {
            const auto home = member;
            const bool placed = placeVoyageCrew(member, currentRoom, side, true);
            if (placed) {
                member.boardRoom = member.boardTargetRoom = member.room;
                member.boardSlot = member.boardTargetSlot = member.slot;
                member.boardX = member.x;
                member.boardY = member.y;
                member.boardPositionValid = true;
                member.boardNewGoal = false;
                member.boardPath.clear();
                member.boardLastDoor = member.boardBlockingDoor = -1;
            }
            member.room = home.room;
            member.targetRoom = home.targetRoom;
            member.slot = home.slot;
            member.targetSlot = home.targetSlot;
            member.appearance.room = home.appearance.room;
            member.appearance.slot = home.appearance.slot;
            member.x = home.x;
            member.y = home.y;
            member.path = home.path;
            member.newGoal = home.newGoal;
            member.lastDoor = home.lastDoor;
            member.blockingDoor = home.blockingDoor;
        } else {
            placeVoyageCrew(member, currentRoom, side, physicalEnemy);
        }
    }
    updateManning();
}

void Run::releaseEnemyMind() {
    // ReleaseCrew 0x609d40, ShipDestroyed 0x609e60 and Jump 0x60ae20.
    if (auto* target = mindMember(
            crew_, enemy_.crew, voyageBoarders_, enemy_.mindTargetOwner, enemy_.mindTarget)) {
        auto& member = *target;
        setCrewControlled(member, false);
        member.health = std::min(member.health, crewMaximumHealth(member.appearance.species));
        member.healthBoost = 0;
    }
    if (enemy_.mindTarget >= 0) {
        combatSound({"mindControlEnd"}, tick_);
    }
    if (auto* source = findSystem(enemy_.systems, 14)) {
        setSystemLock(*source, 4, true);
    }
    enemy_.mind.elapsed = enemy_.mind.duration;
    enemy_.mind.active = false;
    enemy_.mindTarget = -1;
    enemy_.mindTargetOwner = 0;
}

void Run::releasePlayerMind() {
    if (auto* target =
            mindMember(crew_, enemy_.crew, voyageBoarders_, mindTargetOwner_, mindTarget_)) {
        setCrewControlled(*target, false);
        target->health = std::min(target->health, crewMaximumHealth(target->appearance.species));
        target->healthBoost = 0;
    }
    if (mindTarget_ >= 0) {
        combatSound({"mindControlEnd"}, tick_);
    }
    if (auto* source = system(14)) {
        setSystemLock(*source, 4);
    }
    mind_.elapsed = mind_.duration;
    mind_.active = false;
    mindTarget_ = -1;
    mindTargetOwner_ = 1;
}

void Run::initiateMindControl(int owner) {
    // MindSystem::InitiateMindControl 0x60a280 empties the queue first, then drops
    // the crew it cannot affect: a source frees its own controlled crew and takes
    // foreign uncontrolled crew (a strong hack, iHackEffect > 1 && bUnderAttack,
    // reverses both), never a dead or dying (status 3) actor, nor one aboard a
    // super-shielded opponent without ZOLTAN_BYPASS.
    const auto queued = std::exchange(mindQueue_[std::size_t(owner)], {});
    auto* source = findSystem(owner == 0 ? systems_ : enemy_.systems, 14);
    if (!source) {
        return;
    }
    const bool strong = source->hackActive && source->hackLevel > 1;
    const int level =
        std::clamp(strong ? source->health : std::min(source->power, source->health), 0, 3);
    const auto& sourceShip = owner == 0 ? ship_ : enemy_.ship;
    const bool bypass = std::ranges::find(sourceShip.augments, std::string("ZOLTAN_BYPASS")) !=
                        sourceShip.augments.end();

    struct Candidate {
        RunCrew* member;
        MindQueuedCrew reference;
    };

    std::vector<Candidate> admitted;
    bool validTarget = false;
    for (const auto& reference : queued) {
        auto* member =
            mindMember(crew_, enemy_.crew, voyageBoarders_, reference.owner, reference.index);
        if (!member) {
            continue;
        }
        const bool own = reference.owner == owner;
        if ((own == member->controlled) == strong || member->dead || member->dying ||
            member->outOfGame) {
            continue;
        }
        const int ship = mindPhysicalShip(crew_, enemy_.crew, *member);
        if (ship != owner && (ship == 0 ? superShields_ : enemy_.superShields) > 0 && !bypass) {
            continue;
        }
        // SlugAlien::IsTelepathic 0x740340 answers true, CrewMember 0x720d00 false.
        validTarget = validTarget || member->appearance.species != "slug";
        admitted.push_back({member, reference});
    }
    // Without a non-telepathic target the source stays unused (timer full, no lock)
    // and nobody resists.
    if (!validTarget) {
        return;
    }
    std::vector<Candidate> eligible;
    for (const auto& candidate : admitted) {
        if (candidate.member->appearance.species == "slug") {
            mindResisted_.push_back(candidate.reference);
        } else {
            eligible.push_back(candidate);
        }
    }
    setSystemLock(*source, -1, owner == 1);
    (owner == 0 ? mind_ : enemy_.mind) = {true, 0, mindControlTimer[std::size_t(level)], 0};
    // MIND_CONTROL_COUNT 0x837d70 is {0,1,1,1}; even a sole candidate consumes
    // random32() % count. Actor insertion order remains explicit.
    if (level == 0) {
        return;
    }
    const auto& selected = eligible[combatRandom_.next() % eligible.size()];
    auto& member = *selected.member;
    auto& targetIndex = owner == 0 ? mindTarget_ : enemy_.mindTarget;
    auto& targetOwner = owner == 0 ? mindTargetOwner_ : enemy_.mindTargetOwner;
    if (member.controlled) {
        // CrewMember::SetMindControl(false) 0x4728c0 frees the actor; the other
        // source drops it from its controlled list on its own next OnLoop.
        setCrewControlled(member, false);
        member.health = std::min(member.health, crewMaximumHealth(member.appearance.species));
        member.healthBoost = 0;
        targetIndex = -1;
        targetOwner = 1 - owner;
        auto& otherIndex = owner == 0 ? enemy_.mindTarget : mindTarget_;
        auto& otherOwner = owner == 0 ? enemy_.mindTargetOwner : mindTargetOwner_;
        if (otherOwner == selected.reference.owner && otherIndex == selected.reference.index) {
            otherIndex = -1;
            otherOwner = owner;
        }
        combatSound({"mindControlEnd"}, tick_);
    } else {
        targetIndex = selected.reference.index;
        targetOwner = selected.reference.owner;
        setCrewControlled(member, true);
        combatSound({"mindControl"}, tick_);
    }
}

void Run::raiseMindResists() {
    // ShipManager::OnRender 0x49abc0 reads each crew's flag with GetResisted 0x46e860
    // (which clears it) and pushes a RESIST DamageMessage at GetPosition - (20, 0),
    // coloured (1, 0, 1, 1). Native raises it once the tick's ship loops have run.
    for (const auto& reference : std::exchange(mindResisted_, {})) {
        const auto* member =
            mindMember(crew_, enemy_.crew, voyageBoarders_, reference.owner, reference.index);
        if (!member || member->outOfGame) {
            continue;
        }
        const int ship = mindPhysicalShip(crew_, enemy_.crew, *member);
        if (ship == 1 && !enemy_.present) {
            continue;
        }
        auto point = crewCombatPoint(*member);
        point.x -= 20.f;
        ResistMessage message{ship, point, tick_, false};
        message.mind = true;
        pushCombatMessage(message);
    }
}

void Run::remapMindQueues(int owner, const std::function<int(int)>& remap) {
    for (auto* crewList : {&mindQueue_[0], &mindQueue_[1], &mindResisted_}) {
        std::erase_if(*crewList, [&](MindQueuedCrew& queued) {
            if (queued.owner != owner) {
                return false;
            }
            queued.index = remap(queued.index);
            return queued.index < 0;
        });
    }
}

void Run::beginEnemyJump() {
    // ShipManager::JumpLeave491b70 dispatches virtual Jump for every
    // installed system except weapons. Preserve derived overrides: cloak,
    // battery, artillery and drones do not call the base StopHacking6140d0.
    for (auto& sys : enemy_.systems) {
        const int id = sys.definition.id;
        if (id == 3) {
            continue;
        }
        if (id != 4 && id != 10 && id != 11 && id != 12) {
            sys.hackActive = false;
            sys.hackLevel = 0;
            setSystemLock(sys, 0, true);
        }
        if (id == 0) {
            std::erase_if(shieldHits_, [](const ShieldHit& hit) {
                return hit.ship == 1;
            });
            enemy_.superShields =
                std::ranges::find(enemy_.ship.augments, std::string("ENERGY_SHIELD")) !=
                        enemy_.ship.augments.end()
                    ? 5
                    : 0;
            enemy_.shieldExcessChargeHack = false;
        } else if (id == 9) {
            enemy_.teleportCharge = 0;
        } else if (id == 10) {
            stopAbility(10, true);
        } else if (id == 11) {
            sys.artillery.charge = 0;
            sys.artillery.chargeLevel = 0;
        } else if (id == 12) {
            stopAbility(12, true);
            setSystemLock(sys, 0, true);
        } else if (id == 14) {
            releaseEnemyMind();
            setSystemLock(sys, 0, true);
        } else if (id == 15) {
            if (auto* target = system(enemy_.hackingTarget)) {
                if (target->definition.id == 14 && target->hackLevel > 1) {
                    releasePlayerMind();
                }
                target->hackActive = false;
                target->hackLevel = 0;
                if (target->definition.id == 0) {
                    shieldExcessChargeHack_ = false;
                }
            }
            enemy_.hacking = {};
            enemy_.hackingTarget = -1;
            enemy_.hackingDrone.health = 0;
            enemy_.hackingDrone.deployed = enemy_.hackingDrone.delivered = false;
            enemy_.hackingDrone.rebuildTimer = 10;
            setSystemLock(sys, 0, true);
        }
    }
    if (auto* controller = findSystem(enemy_.systems, 4)) {
        for (auto& drone : enemy_.drones) {
            const auto& blueprint = catalog_.equipment(drone.blueprint);
            if (blueprint.type == "REPAIR" || blueprint.type == "BATTLE") {
                continue;
            }
            // DroneSystemJump611ae0 depowers recalled enemy space drones
            // without calling SetDeployed(false); delivered enemy boarder
            // bodies therefore remain physical but frozen without power.
            if (drone.powered) {
                releaseBatteryPower(
                    *controller, std::max(0, blueprint.power - drone.bonusPower), true);
                controller->power = std::max(0, controller->power - blueprint.power);
                controller->bonusPower = std::max(0, controller->bonusPower - drone.bonusPower);
            }
            drone.powered = drone.repowerPending = false;
        }
    }
    const bool intruders =
        std::ranges::any_of(crew_,
                            [](const auto& member) {
                                return member.health > 0 && member.intruder && !member.controlled;
                            }) ||
        std::ranges::any_of(enemy_.crew,
                            [](const auto& member) {
                                return member.health > 0 && !member.intruder && member.controlled;
                            }) ||
        std::ranges::any_of(physicalCrewDrones(0), [&](const auto* drone) {
            return !drone->crewDead && drone->health > 0 && drone->crewPhysicalShip == 1;
        });
    if (!intruders) {
        // JumpLeave491b70 resets current door health only after the ship's
        // effective intruder count has fallen to zero.
        enemy_.doorDamage.clear();
    }
    reconcilePower(true);
}

void Run::waitSystems() {
    // ShipManager::Wait492d70 is a selective reset before World recreates
    // the no-fuel location. It does not dispatch every system's Jump.
    jumpWeaponFactories();
    if (auto* shields = findSystem(systems_, 0)) {
        shields->hackActive = false;
        shields->hackLevel = 0;
        shieldExcessChargeHack_ = false;
        std::erase_if(shieldHits_, [](const ShieldHit& hit) {
            return hit.ship == 0;
        });
        superShields_ =
            std::ranges::find(ship_.augments, std::string("ENERGY_SHIELD")) != ship_.augments.end()
                ? 5
                : 0;
    }
    for (auto& sys : systems_) {
        if (sys.definition.id == 11) {
            // Artillery60ecb0 leaves its constructed projectile queue alone.
            sys.artillery.charge = 0;
            sys.artillery.chargeLevel = 0;
        }
    }
    if (auto* hack = findSystem(systems_, 15)) {
        hack->hackActive = false;
        hack->hackLevel = 0;
        if (hackingTarget_ >= 0) {
            if (auto* target = findSystem(enemy_.systems, hackingTarget_)) {
                target->hackActive = false;
                target->hackLevel = 0;
                if (target->definition.id == 0) {
                    enemy_.shieldExcessChargeHack = false;
                }
            }
            hackingTarget_ = -1;
        }
        hacking_.active = false;
        hacking_.elapsed = hacking_.duration = hacking_.cooldown = 0;
        blowUpDrone(hackingDrone_, true);
        setSystemLock(*hack, 0);
    }
    // CountCrew491030 counts non-dead ordinary effective intruders, including
    // a dying actor until IsDead becomes true; crew drones are excluded.
    const bool intruders = std::ranges::any_of(crew_,
                                               [](const auto& member) {
                                                   return !member.dead && !member.outOfGame &&
                                                          !member.intruder && member.controlled;
                                               }) ||
                           std::ranges::any_of(enemy_.crew,
                                               [](const auto& member) {
                                                   return !member.dead && !member.outOfGame &&
                                                          member.intruder && !member.controlled;
                                               }) ||
                           std::ranges::any_of(voyageBoarders_, [](const auto& member) {
                               return !member.dead && !member.outOfGame && !member.controlled;
                           });
    if (!intruders) {
        voyageDoorDamage_.clear();
    }
    if (const auto* bay = system(13); bay && bay->health > 0) {
        constexpr std::array<float, 4> cloneHealth{0.f, 8.f, 16.f, 25.f};
        const float heal = cloneHealth[std::size_t(std::clamp(systemCapacity(*bay), 0, 3))];
        for (auto& member : crew_) {
            if (!member.intruder && !member.outOfGame && member.health > 0) {
                member.health =
                    std::min(member.health + heal,
                             crewMaximumHealth(member.appearance.species) + member.healthBoost);
            }
        }
    }
    for (auto* drone : physicalCrewDrones(1)) {
        if (drone->crewPhysicalShip == 0 && drone->health > 0) {
            drone->health = std::max(0.f, drone->health - 1000.f);
        }
    }
    if (auto* mind = findSystem(systems_, 14)) {
        // ForceReady60a000 unlocks without releasing controlled actors.
        setSystemLock(*mind, 0);
    }
    for (const int id : {0, 1, 3, 6}) {
        if (auto* sys = findSystem(systems_, id)) {
            setSystemLock(*sys, 0);
        }
    }
}

void Run::clearPlayerLocationState() {
    // World::ClearLocation5ef130 calls these independently of the departure
    // ShipManager::Jump. CompleteShip::Jump4098b0 only clears transfer lists;
    // the native player transfer is currently immediate, with no such lists.
    for (auto& sys : systems_) {
        sys.powerLimit = 8;
        sys.powerLoss = 0;
        sys.powerDivide = 1;
    }
    reactorPowerCap_ = 64;
    reactorPowerLoss_ = 0;
    reactorPowerDivide_ = 1;
}

void Run::clearEnemyLocationCrew() {
    // ClearLocation5ef130 kills the old enemy's PHYSICAL crew list. Actors
    // originating there but already aboard the player remain in the factory.
    releaseEnemyMind();
    const int oldCount = static_cast<int>(enemy_.crew.size());
    std::map<int, int> survivors;
    for (std::size_t index = 0; index < voyageBoarders_.size(); ++index) {
        const auto& member = voyageBoarders_[index];
        if (member.enemyCrewOrigin >= 0 && !member.outOfGame) {
            survivors[member.enemyCrewOrigin] = static_cast<int>(index);
        }
    }
    for (int index = 0; index < oldCount; ++index) {
        auto& member = enemy_.crew[std::size_t(index)];
        if (!member.outOfGame && member.intruder) {
            if (!survivors.contains(index)) {
                survivors[index] = static_cast<int>(voyageBoarders_.size());
                auto survivor = member;
                // The voyage list represents owner1 actors physically on0.
                if (member.boardPositionValid) {
                    survivor.room = member.boardRoom;
                    survivor.slot = member.boardSlot;
                    survivor.x = member.boardX;
                    survivor.y = member.boardY;
                    survivor.targetRoom =
                        member.boardTargetRoom >= 0 ? member.boardTargetRoom : member.boardRoom;
                    survivor.targetSlot = member.boardTargetSlot;
                    survivor.path = member.boardPath;
                    survivor.newGoal = member.boardNewGoal;
                    survivor.lastDoor = member.boardLastDoor;
                    survivor.blockingDoor = member.boardBlockingDoor;
                }
                survivor.intruder = false;
                survivor.enemyCrewOrigin = -1;
                const int lastDoor = survivor.lastDoor;
                const int blockingDoor = survivor.blockingDoor;
                clearForeignPosition(survivor);
                survivor.lastDoor = lastDoor;
                survivor.blockingDoor = blockingDoor;
                voyageBoarders_.push_back(std::move(survivor));
            }
        } else if (!member.outOfGame) {
            killCrew(member, true);
        }
    }
    for (auto& member : crew_) {
        if (!member.outOfGame && member.intruder) {
            killCrew(member, true);
        }
    }
    // After enemy_ is disposed, owner1 ordinary target indices are voyage
    // indices. A later enemy birth adds its roster size to these references.
    const auto remap = [&](int index) {
        if (index >= oldCount) {
            return index - oldCount;
        }
        const auto found = survivors.find(index);
        return found == survivors.end() ? -1 : found->second;
    };
    const auto targets = [&](auto& actor) {
        if (actor.attackTargetOwner != 1 || actor.attackTargetDrone) {
            return;
        }
        const int index = remap(actor.attackTargetIndex);
        if (index >= 0 && std::size_t(index) < voyageBoarders_.size()) {
            actor.attackTargetIndex = index;
        } else {
            actor.attackTargetOwner = actor.attackTargetIndex = -1;
            actor.attackTargetDrone = false;
            actor.attackTargetDroneId = 0;
        }
    };
    for (auto& member : crew_) {
        targets(member);
    }
    // ClearLocation 0x5ef130 keeps a departing flagship (BossShip::ClearLocation)
    // rather than deleting it, so its crew aboard the player keep their owner
    // while it is parked; beginCombat keeps the same link when the boss returns.
    const bool parkedBoss = campaign_.active && campaign_.savedBoss.is_object() &&
                            campaign_.savedBoss.at("enemy").at("ship").at("id") == enemy_.ship.id;
    for (auto& member : voyageBoarders_) {
        targets(member);
        if (!parkedBoss) {
            member.enemyCrewOrigin = -1;
            member.cloneReady = false;
        }
    }
    for (const int owner : {0, 1}) {
        for (auto* drone : physicalCrewDrones(owner)) {
            targets(*drone);
        }
    }
    if (mindTarget_ >= 0 && mindTargetOwner_ == 1) {
        const int target = remap(mindTarget_);
        if (target >= 0 && std::size_t(target) < voyageBoarders_.size()) {
            mindTarget_ = target;
        } else {
            releasePlayerMind();
        }
    }
    // The disposed enemy MindSystem takes its queue with it.
    mindQueue_[1].clear();
    remapMindQueues(1, [&](int index) {
        const int target = remap(index);
        return target >= 0 && std::size_t(target) < voyageBoarders_.size() ? target : -1;
    });
}

float Run::crewTeleportDuration(const RunCrew& member) const {
    const auto& strip = catalog_.animation(member.appearance.species + "_teleport");
    return strip.usable() ? strip.duration : .5f;
}

std::uint64_t Run::crewTeleportArriveTick(const RunCrew& member) const {
    // Elapsed frames of whichever teleport strip is running.
    const float remaining =
        member.teleportLeaveKind != 0 ? member.teleportLeave : member.teleportArrive;
    return std::uint64_t(std::max(0.f, crewTeleportDuration(member) - remaining) * 60.f);
}

void Run::healTeleportCrew(RunCrew& member, bool enemy) {
    // CompleteShip::InitiateTeleport4090a0 heals both leaving and arriving
    // parties using the casting ship's augmentation and each actor's HP cap.
    double value = 0;
    for (const auto& augmentation : enemy ? enemy_.ship.augments : ship_.augments) {
        if (augmentation == "TELEPORT_HEAL") {
            value += catalog_.equipment(augmentation).value;
        }
    }
    if (value > 0 && member.health > 0) {
        const float maximum = crewMaximumHealth(member.appearance.species) + member.healthBoost;
        member.health = std::min(maximum, member.health + float(value) * maximum);
    }
}

void Run::advanceCloneReady(int owner) {
    // World5f6520 consumes CloneReady after both CompleteShip passes,
    // enemy first. The system callback advances the timer only.
    auto& queue = owner == 0 ? cloneQueue_ : enemy_.cloneQueue;
    auto& pending = owner == 0 ? clonePending_ : enemy_.clonePending;
    const auto* bay = findSystem(owner == 0 ? systems_ : enemy_.systems, 13);
    if (!bay || pending < 0) {
        return;
    }
    const int index = pending;
    pending = -1;
    auto& member = (owner == 0 ? crew_ : enemy_.crew).at(std::size_t(index));
    resetCrewForClone(member);
    placeVoyageCrew(member, bay->definition.room, member.controlled, owner == 1);
    assignPhysicalCrewOrder(member, true);
    audio_.sound("cloneArrive");
    std::erase(queue, index);
    refreshCrewNames();
}

void Run::advanceEnemyTeleporter() {
    // ShipAI::OnInit 0x600ac0 and OnLoop 0x6012e0: invasion keeps
    // one quarter of its initial crew home; real actors gather before sending.
    if (!enemy_.present || enemy_.hull <= 0 || enemy_.escapeJumpElapsed >= 0) {
        return;
    }
    auto* teleporter = findSystem(enemy_.systems, 9);
    if (!teleporter) {
        return;
    }
    // CompleteShip::InitiateTeleport 0x4090a0 waits while a party is leaving.
    const auto leaving = [](const RunCrew& member) {
        return member.teleportLeaveKind >= 3;
    };
    if (std::ranges::any_of(enemy_.crew, leaving) || std::ranges::any_of(crew_, leaving) ||
        std::ranges::any_of(voyageBoarders_, leaving)) {
        return;
    }
    if (enemy_.teleportStartingCrew == 0) {
        // Older live checkpoints preserve stable dead and living home slots.
        enemy_.teleportStartingCrew = static_cast<int>(enemy_.crew.size());
    }
    const int effective = std::clamp(std::min(teleporter->power, teleporter->health), 0, 3);
    // RequiredEvac 0x600ba0 also evacuates sabotage crews while escaping,
    // after three systems break, and after surrender. ReadyForEvac 0x601ab0
    // otherwise uses BoardingGoal's .25 health limit (invasion uses -100).
    const bool strongHack = teleporter->hackActive && teleporter->hackLevel > 1;
    const int broken =
        static_cast<int>(std::ranges::count_if(enemy_.systems, [](const RunSystem& sys) {
            return sys.health <= 0;
        }));
    const bool forceEvac =
        strongHack || !hostile_ ||
        (enemy_.boardingAi == 1 && ((enemy_.escaping && enemy_.escapeTimer < 15.f) || broken > 2));
    int returnRoom = -1;
    for (const auto& member : crew_) {
        if (member.health > 0 && !member.intruder && member.controlled &&
            (forceEvac || member.health <= .25f * (crewMaximumHealth(member.appearance.species) +
                                                   member.healthBoost))) {
            returnRoom = member.room;
            break;
        }
    }
    if (returnRoom < 0) {
        for (const auto& boarder : voyageBoarders_) {
            if (boarder.enemyCrewOrigin >= 0 && boarder.health > 0 && !boarder.controlled &&
                (forceEvac ||
                 (enemy_.boardingAi == 1 &&
                  boarder.health <= .25f * (crewMaximumHealth(boarder.appearance.species) +
                                            boarder.healthBoost)))) {
                returnRoom = boarder.room;
                break;
            }
        }
    }
    if (returnRoom >= 0) {
        // SetEvacRequest 0x600df0 checks the target's barrier directly,
        // without the sending command's ZOLTAN_BYPASS exception. A surrendered
        // enemy may receive even if its teleporter no longer functions.
        if (cloaked(0) || superShields_ > 0 ||
            (hostile_ && (effective == 0 || (systemLocked(*teleporter) && !strongHack)))) {
            return;
        }
        int returned = 0;
        // Effective intruders can include controlled player-owned crew on
        // their home ship. Teleportation changes physical ship, not ownership.
        for (auto& member : crew_) {
            if (member.health <= 0 || member.intruder || !member.controlled ||
                member.room != returnRoom || returned >= 4) {
                continue;
            }
            ++returned;
            healTeleportCrew(member, true);
            startTeleportDeparture(member, 4, teleporter->definition.room);
        }
        for (auto& boarder : voyageBoarders_) {
            const int origin = boarder.enemyCrewOrigin;
            if (origin < 0 || boarder.health <= 0 || boarder.controlled ||
                std::size_t(origin) >= enemy_.crew.size()) {
                continue;
            }
            if (boarder.room != returnRoom || returned >= 4) {
                continue;
            }
            ++returned;
            healTeleportCrew(boarder, true);
            startTeleportDeparture(boarder, 5, teleporter->definition.room);
        }
        enemy_.teleportParty.clear();
        enemy_.teleportRequest = 0;
        if (returned > 0) {
            enemy_.teleportCharge = 0;
            setSystemLock(
                *teleporter, effective > 0 ? teleportLock[std::size_t(effective - 1)] : 0, true);
            combatSound({"teleport"}, tick_);
        }
        return;
    }
    if (forceEvac || !hostile_) {
        enemy_.teleportParty.clear();
        enemy_.teleportRequest = 0;
        return;
    }
    // ShipAI::GetTeleportCommand 0x600ea0 sends once CrewAI::ReadyForTeleport
    // 0x601a00 (CrewAI's capped request at most TeleportSystem::iPreparedCrew)
    // and TeleportSystem::CanSend 0x620c80 hold. CrewAI task 8 walks the crew
    // there; iPreparedCrew counts the distinct slots of non-intruders standing
    // at their goal in the teleporter room (UpdateCrewmembers 0x49ee10 ->
    // UpdateCrewLocation 0x620a40).
    enemy_.teleportParty.clear();
    const int teleporterRoom = teleporter->definition.room;
    const auto& shape = findRoom(enemy_.ship, teleporterRoom);
    const int columns = std::max(1, shape.w / 35);
    const auto atGoal = [&](const RunCrew& member) {
        return member.targetSlot >= 0 &&
               std::abs(member.x - float(shape.x + 35 * (member.targetSlot % columns) + 17)) <
                   .5f &&
               std::abs(member.y - float(shape.y + 35 * (member.targetSlot / columns) + 17)) < .5f;
    };
    std::vector<RunCrew*> present;
    for (auto& member : enemy_.crew) {
        if (!member.outOfGame && !member.dead && !member.intruder && !member.controlled &&
            member.room == teleporterRoom && member.targetRoom == teleporterRoom) {
            present.push_back(&member);
        }
    }
    std::ranges::stable_sort(present, {}, &RunCrew::physicalCrewOrder);
    std::set<int> preparedSlots;
    for (const auto* member : present) {
        if (member->health > 0 && !member->dying && atGoal(*member)) {
            preparedSlots.insert(member->targetSlot);
        }
    }
    const int prepared = int(preparedSlots.size());
    const bool bypass = std::ranges::find(enemy_.ship.augments, std::string("ZOLTAN_BYPASS")) !=
                        enemy_.ship.augments.end();
    if (enemy_.teleportRequest <= 0 || enemy_.crewAi.teleportRequest > prepared || prepared == 0 ||
        effective == 0 || systemLocked(*teleporter) || cloaked(0) ||
        (superShields_ > 0 && !bypass)) {
        return;
    }
    // ShipManager::TeleportCrew 0x49be30 takes the first four non-intruders in
    // the room that CrewMember::CanTeleport 0x46d9d0 allows (not dying; an enemy
    // actor otherwise always may).
    const auto& destination = ship_.rooms[combatRandom_.next() % ship_.rooms.size()];
    int sent = 0;
    for (auto* member : present) {
        if (member->health <= 0 || member->dying || sent > 3) {
            continue;
        }
        ++sent;
        healTeleportCrew(*member, true);
        startTeleportDeparture(*member, 3, destination.id);
    }
    const bool exact = enemy_.teleportRequest == prepared;
    enemy_.teleportRequest -= prepared;
    if (enemy_.teleportRequest < 0 || exact) {
        ++enemy_.teleportWaves;
        enemy_.teleportRequest = 0;
    }
    enemy_.teleportCharge = 0;
    setSystemLock(*teleporter, teleportLock[std::size_t(effective - 1)], true);
    combatSound({"teleport"}, tick_);
}

// ShipAI::OnLoop 0x6012e0 recomputes its teleport request after the CrewAI
// pass (GetTeleportCommand therefore reads the previous frame's value):
// ShipManager::CountCrew(false) minus iCrewNeeded from ShipAI::OnInit 0x600ac0
// (invasion keeps a quarter of the starting crew home, sabotage half), while
// iTimesTeleported is at most one.
void Run::updateEnemyTeleportRequest() {
    int home = int(std::ranges::count_if(enemy_.crew, [](const RunCrew& member) {
        return !member.outOfGame && !member.dead && !member.intruder && !member.controlled;
    }));
    home += int(std::ranges::count_if(crew_, [](const RunCrew& member) {
        return !member.outOfGame && !member.dead && member.intruder && member.controlled &&
               member.boardPositionValid;
    }));
    const int sendCount = enemy_.boardingAi == 2   ? 3 * enemy_.teleportStartingCrew / 4
                          : enemy_.boardingAi == 1 ? enemy_.teleportStartingCrew / 2
                                                   : 0;
    const int needed = enemy_.teleportStartingCrew - sendCount;
    enemy_.teleportRequest = enemy_.teleportWaves <= 1 ? std::max(0, home - needed) : 0;
}

void Run::startTeleportDeparture(RunCrew& member, int kind, int room) {
    // CrewMember::StartTeleport 0x472d40 (via ShipManager::TeleportCrew
    // 0x49be30): status-6 strip played forward on the sending ship.
    member.teleportLeave = crewTeleportDuration(member);
    member.teleportLeaveKind = kind;
    member.teleportLeaveRoom = room;
}

void Run::startTeleportArrival(RunCrew& member) {
    // ShipManager::AddCrewMember on the receiving ship starts the reversed
    // strip (CrewMember::StartTeleportArrive 0x472db0).
    member.teleportArrive = crewTeleportDuration(member);
}

void Run::advanceTeleportDepartures() {
    // CrewMember::CheckForTeleport 0x471f10 ends each forward strip;
    // CompleteShip::OnLoop 0x4094a0 then moves the actor to the other ship.
    // Kinds: 1 player send, 2 player return, 3 enemy send, 4 enemy recall of
    // controlled player crew, 5 enemy recall of boarders.
    for (auto& crew : crew_) {
        if (crew.teleportLeaveKind == 0 || crew.teleportLeave > 0) {
            continue;
        }
        const int kind = crew.teleportLeaveKind;
        const int room = crew.teleportLeaveRoom;
        crew.teleportLeaveKind = 0;
        if (crew.health <= 0 || !enemy_.present) {
            continue;
        }
        if (kind == 1) {
            if (std::ranges::none_of(enemy_.ship.rooms, [&](const auto& candidate) {
                    return candidate.id == room;
                })) {
                continue;
            }
            crew.intruder = true;
            crew.boardRoom = room;
            clearCrewCombat(crew);
            crew.boardPositionValid = false;
            crew.lastDoor = crew.boardLastDoor = -1;
            crew.boardPath.clear();
            // CompleteShip4094a0 appends the leaving actor to the receiving
            // ship before its manager pass, rather than waiting for CrewAI.
            const auto home = crew;
            const bool placed = placeVoyageCrew(crew, room, !crew.controlled, true);
            assignPhysicalCrewOrder(crew, true);
            if (placed) {
                crew.boardRoom = crew.boardTargetRoom = crew.room;
                crew.boardSlot = crew.boardTargetSlot = crew.slot;
                crew.boardX = crew.x;
                crew.boardY = crew.y;
                crew.boardPositionValid = true;
                crew.boardNewGoal = false;
            }
            crew.room = home.room;
            crew.targetRoom = home.targetRoom;
            crew.slot = home.slot;
            crew.targetSlot = home.targetSlot;
            crew.appearance.room = home.appearance.room;
            crew.appearance.slot = home.appearance.slot;
            crew.x = home.x;
            crew.y = home.y;
            crew.path = home.path;
            crew.newGoal = home.newGoal;
            crew.lastDoor = home.lastDoor;
            crew.blockingDoor = home.blockingDoor;
            startTeleportArrival(crew);
        } else if (kind == 2) {
            crew.intruder = false;
            crew.boardRoom = -1;
            clearCrewCombat(crew);
            crew.boardPositionValid = false;
            crew.lastDoor = crew.boardLastDoor = -1;
            crew.boardTargetRoom = -1;
            crew.boardPath.clear();
            placeVoyageCrew(crew, room);
            assignPhysicalCrewOrder(crew, true);
            startTeleportArrival(crew);
        } else if (kind == 4) {
            auto& member = crew;
            clearCrewCombat(member);
            clearForeignPosition(member);
            member.intruder = true;
            member.boardRoom = room;
            member.selected = false;
            member.path.clear();
            startTeleportArrival(member);
        }
    }
    for (auto& boarder : voyageBoarders_) {
        if (boarder.teleportLeaveKind == 0 || boarder.teleportLeave > 0) {
            continue;
        }
        const int room = boarder.teleportLeaveRoom;
        boarder.teleportLeaveKind = 0;
        const int origin = boarder.enemyCrewOrigin;
        if (boarder.health <= 0 || !enemy_.present || origin < 0 ||
            std::size_t(origin) >= enemy_.crew.size()) {
            continue;
        }
        auto member = std::move(boarder);
        boarder = member;
        boarder.health = 0;
        boarder.dead = boarder.outOfGame = true;
        boarder.cloneReady = false;
        boarder.dying = false;
        boarder.enemyCrewOrigin = -1;
        boarder.path.clear();
        member.enemyCrewOrigin = -1;
        member.path.clear();
        clearCrewCombat(member);
        clearForeignPosition(member);
        member.intruder = false;
        member.boardRoom = -1;
        member.room = member.targetRoom = room;
        member.slot = member.targetSlot = 0;
        const auto& shape = findRoom(enemy_.ship, member.room);
        member.x = float(shape.x + 17);
        member.y = float(shape.y + 17);
        enemy_.crew[std::size_t(origin)] = std::move(member);
        auto& arriving = enemy_.crew[std::size_t(origin)];
        placeVoyageCrew(arriving, room, arriving.controlled, true);
        assignPhysicalCrewOrder(arriving, true);
        startTeleportArrival(arriving);
    }
    for (std::size_t index = 0; index < enemy_.crew.size(); ++index) {
        auto& leaving = enemy_.crew[index];
        if (leaving.teleportLeaveKind == 0 || leaving.teleportLeave > 0) {
            continue;
        }
        const int room = leaving.teleportLeaveRoom;
        leaving.teleportLeaveKind = 0;
        const auto found = std::ranges::find(ship_.rooms, room, &RoomDefinition::id);
        if (leaving.health <= 0 || found == ship_.rooms.end()) {
            continue;
        }
        const auto& destination = *found;
        auto member = std::move(enemy_.crew[std::size_t(index)]);
        enemy_.crew[std::size_t(index)] = member;
        enemy_.crew[std::size_t(index)].health = 0;
        enemy_.crew[std::size_t(index)].dead = enemy_.crew[std::size_t(index)].outOfGame = true;
        enemy_.crew[std::size_t(index)].cloneReady = false;
        enemy_.crew[std::size_t(index)].dying = false;
        enemy_.crew[std::size_t(index)].path.clear();
        member.enemyCrewOrigin = int(index);
        clearCrewCombat(member);
        clearForeignPosition(member);
        member.path.clear();
        member.room = member.targetRoom = destination.id;
        member.slot = member.targetSlot = 0;
        member.x = float(destination.x + member.slot % (destination.w / 35) * 35 + 17);
        member.y = float(destination.y + member.slot / (destination.w / 35) * 35 + 17);
        const auto vacant = std::ranges::find_if(voyageBoarders_, [](const auto& boarder) {
            return boarder.outOfGame && !boarder.cloneReady && boarder.enemyCrewOrigin < 0;
        });
        std::size_t arrivedIndex = 0;
        if (vacant == voyageBoarders_.end()) {
            arrivedIndex = voyageBoarders_.size();
            voyageBoarders_.push_back(std::move(member));
        } else {
            const int key = static_cast<int>(vacant - voyageBoarders_.begin());
            arrivedIndex = std::size_t(key);
            voyageMelee_.erase(key);
            *vacant = std::move(member);
        }
        auto& arrived = voyageBoarders_[arrivedIndex];
        placeVoyageCrew(arrived, destination.id, !arrived.controlled);
        assignPhysicalCrewOrder(arrived, true);
        startTeleportArrival(arrived);
    }
}

void Run::advanceTeleporter() {
    auto* teleporter = system(9);
    if (!teleporter) {
        teleportCharge_ = 0;
        teleportArmed_ = 0;
        teleportTarget_ = -1;
        return;
    }
    // ShipSystem::Functioning 0x6153d0 sums battery and bonus bars with the allocated
    // power; ShipSystem::GetLocked 0x615930 is an ion lock or a strong hack.
    // TeleportSystem::Charged 0x620920 requires both and ignores `chargeLevel`, so
    // the ring this advances is presentation while the lock is the usable gate.
    const bool functioning = teleporter->health > 0 && teleporter->power > 0;
    const int effective = std::clamp(std::min(teleporter->power, teleporter->health), 0, 3);
    // Readiness is reset by the common World SetSafe tail after enemies.
    if (teleportArmed_ == 0) {
        return;
    }
    // CompleteShip::InitiateTeleport 0x4090a0 waits while a party is leaving.
    if (std::ranges::any_of(crew_, [](const RunCrew& member) {
            return member.teleportLeaveKind == 1 || member.teleportLeaveKind == 2;
        })) {
        return;
    }
    const bool locked = systemLocked(*teleporter);
    const bool bypass =
        std::ranges::find(ship_.augments, std::string("ZOLTAN_BYPASS")) != ship_.augments.end();
    const bool knownRoom = std::ranges::any_of(enemy_.ship.rooms, [&](const auto& candidate) {
        return candidate.id == teleportTarget_;
    });
    if (!functioning || effective <= 0 || locked || !knownRoom ||
        (enemy_.superShields > 0 && !bypass) || !enemy_.present || enemy_.escapeJumpElapsed >= 0 ||
        (teleportArmed_ == 1 && !hostile_)) {
        // TeleportSystem::OnLoop 0x620ee0 clears the armed flag once the system stops
        // functioning. CompleteShip::InitiateTeleport 0x4090a0 checks that both
        // ships exist and neither is jumping; it does not require hostility,
        // allowing survivors to return after surrender or during destruction.
        teleportArmed_ = 0;
        return;
    }
    const int room = teleportTarget_;
    bool moved = false;
    if (teleportArmed_ == 1) {
        // ShipManager::TeleportCrew 0x49be30 sweeps the sending ship's own crew list
        // for crew in the teleporter room whose CrewMember::GetIntruder 0x46c3e0 is
        // false and that CrewMember::CanTeleport 0x46d9d0 admits; CompleteShip::OnLoop
        // 0x4094a0 then adds each to the enemy ship's list in `teleTargetRoom` through
        // ShipManager::AddCrewMember.
        for (auto& crew : crew_) {
            if (crew.intruder || crew.health <= 0 || crew.controlled ||
                crew.room != crew.targetRoom || crew.slot != crew.targetSlot ||
                !crew.path.empty() || crew.room != teleporter->definition.room) {
                continue;
            }
            healTeleportCrew(crew);
            startTeleportDeparture(crew, 1, room);
            moved = true;
        }
    } else {
        // The return command takes the intruders of the armed room; CompleteShip::
        // OnLoop 0x4094a0's arriving branch returns them to
        // ShipManager::GetSystemRoom(shipManager, 9), the receiving teleporter room.
        for (auto& crew : crew_) {
            if (!crew.intruder || crew.health <= 0 || crew.controlled || crew.boardRoom != room) {
                continue;
            }
            healTeleportCrew(crew);
            startTeleportDeparture(crew, 2, teleporter->definition.room);
            moved = true;
        }
    }
    teleportArmed_ = 0;
    if (!moved) {
        // CompleteShip::InitiateTeleport 0x4090a0 tests both party lists before it
        // calls TeleportSystem::InitiateTeleport, so an empty party costs no cooldown.
        return;
    }
    // TeleportSystem::InitiateTeleport 0x620e10 locks CSWTCH_282[effectivePower - 1]
    // and resets the charge; CompleteShip::InitiateTeleport 0x4090a0 plays its
    // "teleport" alias for the same move.
    progressionRun_.tutorial = tutorialOwned_;
    progressionRun_.flag("teleported");
    teleportCharge_ = 0.f;
    setSystemLock(*teleporter, teleportLock[std::size_t(effective - 1)]);
    combatSound({"teleport"}, tick_);
}

// CompleteShip::OnLoop 0x4094a0 adds an arriving party to the receiving ship (ShipManager::
// AddCrewMember) before ShipAI::OnLoop, so CrewAI sees placed boarders.
void Run::placeBoardingCrew() {
    for (auto& crew : crew_) {
        if (crew.intruder && enemy_.present) {
            if (!crew.boardPositionValid) {
                const auto home = crew;
                const bool placed = placeVoyageCrew(crew, crew.boardRoom, !crew.controlled, true);
                if (placed) {
                    assignPhysicalCrewOrder(crew, true);
                    crew.boardRoom = crew.boardTargetRoom = crew.room;
                    crew.boardSlot = crew.boardTargetSlot = crew.slot;
                    crew.boardX = crew.x;
                    crew.boardY = crew.y;
                    crew.boardPositionValid = true;
                    crew.boardNewGoal = false;
                    crew.boardPath.clear();
                }
                // Foreign actor geometry is separate from retained ownership
                // fields; physical placement must not replace the home room.
                crew.room = home.room;
                crew.targetRoom = home.targetRoom;
                crew.slot = home.slot;
                crew.targetSlot = home.targetSlot;
                crew.appearance.room = home.appearance.room;
                crew.appearance.slot = home.appearance.slot;
                crew.x = home.x;
                crew.y = home.y;
                crew.path = home.path;
                crew.newGoal = home.newGoal;
                crew.lastDoor = home.lastDoor;
                crew.blockingDoor = home.blockingDoor;
            }
        }
    }
}

void Run::advanceAdvancedOwner(int shipOwner, int systemId, bool ai) {
    // Source lifecycle: HackingSystem::InitiatePulse 0x607e20 / OnLoop 0x6083b0,
    // MindSystem::OnLoop 0x60a990 / ReleaseCrew 0x609d40, TeleportSystem::OnLoop
    // 0x620ee0 / InitiateTeleport 0x620e10. The native state remains distinct from the
    // original x86 object; drone flight/attachment are explicit native state.
    const auto releaseControl = [&](int owner, bool enteringStrong) {
        if (owner == 0) {
            releasePlayerMind();
        } else {
            releaseEnemyMind();
        }
        if (enteringStrong) {
            if (auto* source = findSystem(owner == 0 ? systems_ : enemy_.systems, 14)) {
                setSystemLock(*source, 0, owner == 1);
            }
        }
    };
    // CombatAI::OnLoop 0x606c80 selects an installed system with repeated
    // random32()%16 draws, and retries InitiatePulse while attached. Enemy
    // hacking owns a separate drone, target and game-time lock from the player.
    const auto advanceEnemyHack = [&]() {
        auto& ability = enemy_.hacking;
        auto& drone = enemy_.hackingDrone;
        auto source = std::ranges::find_if(enemy_.systems, [](const auto& sys) {
            return sys.definition.id == 15;
        });
        const bool valid = hostile_ && enemy_.present && source != enemy_.systems.end() &&
                           source->power > 0 && source->health > 0 &&
                           (!source->hackActive || source->hackLevel <= 1);
        if (ai) {
            if (valid && !systemLocked(*source) && enemy_.hackingTarget < 0 &&
                drone.rebuildTimer == 0 && enemy_.droneParts > 0 && superShields_ == 0 &&
                std::ranges::any_of(systems_, [](const auto& sys) {
                    return sys.definition.id >= 0 && sys.definition.id < 16;
                })) {
                int id = -1;
                do {
                    id = int(combatRandom_.next() % 16);
                } while (!system(id));
                const auto* target = system(id);
                enemy_.hackingTarget = id;
                --enemy_.droneParts;
                resetCrewDroneDeployment(drone);
                drone.deployed = drone.powered = true;
                drone.hackingFlyingElapsed = drone.hackingExtendingElapsed = -1;
                drone.hackingFinishedSetup = false;
                drone.health = 1;
                drone.room = target->definition.room;
                drone.position = {0, 0};
                drone.destination = {800, 0};
                enemy_.hackingDroneSpace = 1;
                enemy_.hackingDroneId = nextProjectile_++;
                combatSound({"droneLaunch"}, tick_);
            }
            auto* target = system(enemy_.hackingTarget);
            const bool attached = valid && drone.deployed && drone.delivered && drone.health > 0;
            if (attached && target && target->health > 0 && !systemLocked(*source)) {
                const int level = std::clamp(std::min(source->power, source->health), 0, 3);
                ability = {true, 0, std::array<float, 4>{10, 4, 7, 10}[std::size_t(level)], 0};
                setSystemLock(*source, -1, true);
                combatSound({"hackStart"}, tick_);
            }
            return;
        }
        drone.rebuildTimer = std::max(0.f, drone.rebuildTimer - Time::step);
        if (enemy_.hackingTarget >= 0 &&
            (!hostile_ || !enemy_.present || !drone.deployed || drone.health <= 0)) {
            if (auto* target = system(enemy_.hackingTarget)) {
                if (target->definition.id == 14 && target->hackLevel > 1) {
                    releaseControl(0, false);
                }
                target->hackActive = false;
                target->hackLevel = 0;
            }
            enemy_.hackingTarget = -1;
            const float rebuild = drone.rebuildTimer;
            resetCrewDroneDeployment(drone);
            drone.rebuildTimer = rebuild;
            enemy_.hackingDroneId = 0;
            ability.active = false;
        }
        drone.controllerHackLevel =
            source != enemy_.systems.end() && source->hackActive ? source->hackLevel : 0;
        // HackingSystem::OnLoop 0x6083b0: a deployed drone is powered whenever
        // the source has effective power, even while strongly hacked itself.
        drone.powered = drone.deployed && source != enemy_.systems.end() && source->power > 0 &&
                        source->health > 0;
        auto* target = system(enemy_.hackingTarget);
        if (!target) {
            return;
        }
        const bool attached = valid && drone.deployed && drone.delivered && drone.health > 0;
        bool pulse = source != enemy_.systems.end() && source->lockCount == -1 && attached;
        if (source != enemy_.systems.end() && source->lockCount == -1) {
            if (!attached) {
                ability.active = false;
                setSystemLock(*source, 4, true);
                ability.elapsed = 0;
                pulse = false;
            } else {
                const float previousDuration = ability.duration;
                ability.elapsed += Time::step;
                if (ability.elapsed > previousDuration) {
                    ability.active = false;
                    setSystemLock(*source, 4, true);
                }
                const int level = std::clamp(std::min(source->power, source->health), 0, 3);
                const float duration = std::array<float, 4>{10, 4, 7, 10}[std::size_t(level)];
                if (duration != previousDuration && previousDuration > 0) {
                    ability.elapsed = ability.elapsed / previousDuration * duration;
                    ability.duration = duration;
                }
            }
        }
        if (target->definition.id == 0 && pulse && target->hackLevel <= 1) {
            shieldExcessChargeHack_ = shieldBubbles_ < target->power / 2 && shieldCharge_ > 0;
        }
        const int nextLevel = attached ? (pulse ? 2 : 1) : 0;
        const bool enteringStrong = target->hackLevel <= 1 && nextLevel > 1;
        if (target->definition.id == 14 && (target->hackLevel > 1) != (nextLevel > 1)) {
            releaseControl(0, nextLevel > 1);
        }
        target->hackActive = attached;
        target->hackLevel = nextLevel;
        ability.active = source != enemy_.systems.end() && source->lockCount == -1;
        if (!pulse) {
            return;
        }
        if (target->definition.id == 9 && enteringStrong) {
            teleportCharge_ = 100;
            setSystemLock(*target, 0);
        }
    };
    if (shipOwner == 1 && systemId == 15) {
        advanceEnemyHack();
    }
    if (shipOwner == 0 && systemId == 15 && !ai) {
        hackingDrone_.rebuildTimer = std::max(0.f, hackingDrone_.rebuildTimer - Time::step);
        auto* hackingSource = system(15);
        const bool hackingSourceValid =
            hackingSource && hackingSource->power > 0 && hackingSource->health > 0 &&
            (!hackingSource->hackActive || hackingSource->hackLevel <= 1);
        if (hackingTarget_ >= 0 && (!enemy_.present || !hostile_ || !hackingDrone_.deployed ||
                                    hackingDrone_.health <= 0)) {
            for (auto& sys : enemy_.systems) {
                if (sys.definition.id == hackingTarget_) {
                    sys.hackActive = false;
                    sys.hackLevel = 0;
                }
            }
            hackingTarget_ = -1;
            const float rebuild = hackingDrone_.rebuildTimer;
            resetCrewDroneDeployment(hackingDrone_);
            hackingDrone_.rebuildTimer = rebuild;
            hackingDroneId_ = 0;
            if (hackingSource && hackingSource->lockCount == -1) {
                hacking_.active = false;
                setSystemLock(*hackingSource, 4);
                hacking_.elapsed = 0;
            }
        }
        hackingDrone_.controllerHackLevel =
            hackingSource && hackingSource->hackActive ? hackingSource->hackLevel : 0;
        hackingDrone_.powered = hackingDrone_.deployed && hackingSource &&
                                hackingSource->power > 0 && hackingSource->health > 0;
        bool hackPulse = false;
        if (hackingSource && hackingSource->lockCount == -1) {
            if (!hostile_ || !enemy_.present || !hackingSourceValid) {
                hacking_.active = false;
                setSystemLock(*hackingSource, 4);
                hacking_.elapsed = 0;
            } else {
                // HACKING_EFFECT_TIMER[effective power] at 0x6083b0 is {10,4,7,10}.
                // The source advances the timer before testing strict `first > second`.
                // It leaves level 2 set for the crossing update that starts LockSystem(4).
                const float previousDuration = hacking_.duration;
                hacking_.elapsed += Time::step;
                hackPulse = true;
                if (hacking_.elapsed > previousDuration) {
                    hacking_.active = false;
                    setSystemLock(*hackingSource, 4);
                }
                const int effectivePower =
                    std::clamp(std::min(hackingSource->power, hackingSource->health), 0, 3);
                const float duration =
                    std::array<float, 4>{10.f, 4.f, 7.f, 10.f}[std::size_t(effectivePower)];
                if (duration != previousDuration && previousDuration > 0) {
                    hacking_.elapsed = hacking_.elapsed / previousDuration * duration;
                    hacking_.duration = duration;
                }
            }
        }
        for (auto& sys : enemy_.systems) {
            if (sys.definition.id != hackingTarget_) {
                continue;
            }
            // HackingSystem::OnLoop keeps the target under attack and at level 1
            // throughout the attached weak-lock period. A click can start another
            // pulse after that lock expires; the source does not pulse automatically.
            const bool attached = hostile_ && enemy_.present && hackingSourceValid &&
                                  hackingDrone_.deployed && hackingDrone_.delivered;
            if (sys.definition.id == 0 && hackPulse && sys.hackLevel <= 1) {
                enemy_.shieldExcessChargeHack =
                    enemy_.shields < sys.power / 2 && enemy_.shieldCharge > 0;
            }
            const int nextLevel = attached ? (hackPulse ? 2 : 1) : 0;
            const bool enteringStrong = sys.hackLevel <= 1 && nextLevel > 1;
            if (sys.definition.id == 14 && (sys.hackLevel > 1) != (nextLevel > 1)) {
                releaseControl(1, nextLevel > 1);
            }
            sys.hackActive = attached;
            sys.hackLevel = nextLevel;
            if (!attached || !hackPulse) {
                continue;
            }
            if (sys.definition.id == 9 && enteringStrong) {
                // TeleportSystem::SetHackingLevel 0x6203c0 readies and unlocks the
                // teleporter the moment a strong hack lands on it (`chargeLevel = 100`,
                // ShipSystem::LockSystem(this, 0)).
                enemy_.teleportCharge = 100;
                setSystemLock(sys, 0, true);
            }
        }
        hacking_.active = hackingSource && hackingSource->lockCount == -1;
    }
    // MindSystem::OnLoop 0x60a990 and CombatAI::UpdateMindControl 0x605bd0
    // share both ownership lists. A strong hack reverses the source's target
    // ownership and uses its health, including while its ordinary power is zero.
    for (const int owner : {shipOwner}) {
        if (systemId != 14) {
            continue;
        }
        auto* source = findSystem(owner == 0 ? systems_ : enemy_.systems, 14);
        const bool strong = source && source->hackActive && source->hackLevel > 1;
        const int level =
            !source ? 0
                    : std::clamp(
                          strong ? source->health : std::min(source->power, source->health), 0, 3);
        auto& ability = owner == 0 ? mind_ : enemy_.mind;
        auto& targetIndex = owner == 0 ? mindTarget_ : enemy_.mindTarget;
        auto& targetOwner = owner == 0 ? mindTargetOwner_ : enemy_.mindTargetOwner;
        auto* target = mindMember(crew_, enemy_.crew, voyageBoarders_, targetOwner, targetIndex);
        if (!ai) {
            // MindSystem::OnLoop 0x60a990 runs InitiateMindControl 0x60a280 for crew
            // queued since its last pass, before advancing the control timer.
            if (!mindQueue_[std::size_t(owner)].empty()) {
                initiateMindControl(owner);
                target = mindMember(crew_, enemy_.crew, voyageBoarders_, targetOwner, targetIndex);
            }
            if (ability.active) {
                if (ability.elapsed < ability.duration) {
                    ability.elapsed += Time::step;
                }
                if (ability.elapsed >= ability.duration || (level == 0 && target)) {
                    // Power-zero MIND_CONTROL_COUNT removes the target before the
                    // empty-list ReleaseCrew starts the canonical four-interval lock.
                    releaseControl(owner, false);
                    target = nullptr;
                } else if (!target || target->outOfGame || target->dead || target->dying ||
                           !target->controlled) {
                    // Pruned pointers are absent when ReleaseCrew emits its sounds.
                    targetIndex = -1;
                    releaseControl(owner, false);
                    target = nullptr;
                }
            }
            const float duration = mindControlTimer[std::size_t(level)];
            if (ability.duration != duration) {
                if (ability.elapsed == ability.duration) {
                    ability.elapsed = duration;
                }
                ability.duration = duration;
            }
        }
        if (ai && owner == 1 && !hostile_) {
            // ShipAI::OnLoop 0x6012e0's nonhostile branch calls UpdateMindControl(false)
            // 0x605bd0, which releases a source still in use (ReleaseCrew 0x609d40).
            if (ability.active) {
                releaseControl(owner, false);
            }
            continue;
        }
        if (!ability.active && hostile_ && enemy_.present && level > 0 && ai &&
            (owner == 1 || strong) && (!systemLocked(*source) || strong)) {
            struct Candidate {
                const RunCrew* member;
                int owner;
                int index;
                int ship;
                int room;
            };

            std::vector<Candidate> actors;
            for (std::size_t i = 0; i < crew_.size(); ++i) {
                const auto& member = crew_[i];
                if (member.health > 0) {
                    actors.push_back({&member,
                                      0,
                                      int(i),
                                      int(member.intruder),
                                      member.intruder ? member.boardRoom : member.room});
                }
            }
            for (std::size_t i = 0; i < enemy_.crew.size(); ++i) {
                const auto& member = enemy_.crew[i];
                if (member.health > 0 && !member.intruder) {
                    actors.push_back({&member, 1, int(i), 1, member.room});
                }
            }
            for (std::size_t i = 0; i < voyageBoarders_.size(); ++i) {
                const auto& member = voyageBoarders_[i];
                if (member.health > 0) {
                    const int index = member.enemyCrewOrigin >= 0 ? member.enemyCrewOrigin
                                                                  : int(enemy_.crew.size() + i);
                    actors.push_back({&member, 1, index, 0, member.room});
                }
            }
            const int desiredOwner = strong ? owner : 1 - owner;
            std::vector<const Candidate*> owned;
            for (const auto& actor : actors) {
                if (actor.owner == desiredOwner) {
                    owned.push_back(&actor);
                }
            }
            if (!owned.empty()) {
                // UpdateMindControl first picks an owned crew room (including slug
                // rooms), keeps that ship's non-telepathic crew standing there and
                // queues them (QueueMindControl 0x60a040). This runs after the ship's
                // ShipManager::OnLoop, so InitiateMindControl filters and draws on the
                // next frame's MindSystem::OnLoop 0x60a990.
                const auto& chosen = *owned[combatRandom_.next() % owned.size()];
                std::vector<MindQueuedCrew> queued;
                for (const auto& actor : actors) {
                    if (actor.ship != chosen.ship || actor.room != chosen.room ||
                        actor.member->appearance.species == "slug" ||
                        (actor.ship != owner && cloaked(actor.ship))) {
                        continue;
                    }
                    queued.push_back({actor.owner, actor.index});
                }
                if (!queued.empty()) {
                    mindQueue_[std::size_t(owner)] = std::move(queued);
                }
            }
        }
        if (!ability.active || !target) {
            continue;
        }
        const float boost = float(mindHealthBoost[std::size_t(level)]);
        if (boost > target->healthBoost) {
            target->health += boost - target->healthBoost;
        } else {
            target->health =
                std::min(target->health, crewMaximumHealth(target->appearance.species) + boost);
        }
        target->healthBoost = boost;
    }
}

void Run::advanceAdvancedSystem(int owner, std::size_t systemIndex) {
    const auto& systems = owner == 0 ? systems_ : enemy_.systems;
    if (systemIndex >= systems.size()) {
        return;
    }
    const int id = systems[systemIndex].definition.id;
    if (id == 14 || id == 15) {
        advanceAdvancedOwner(owner, id, false);
    }
}

void Run::advanceAdvancedAi(int owner) {
    advanceAdvancedOwner(owner, 15, true);
    advanceAdvancedOwner(owner, 14, true);
}

void Run::advanceAdvanced() {
    for (int owner : {0, 1}) {
        const auto& systems = owner == 0 ? systems_ : enemy_.systems;
        for (std::size_t i = 0; i < systems.size(); ++i) {
            advanceAdvancedSystem(owner, i);
        }
        advanceAdvancedAi(owner);
    }
}

} // namespace ftl
