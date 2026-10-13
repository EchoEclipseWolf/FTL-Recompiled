#include "run.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace ftl {

// CrewAI 0x601710-0x6059e0 over one ship's physical crew list. CompleteShip::
// OnLoop 0x4094a0 runs it through ShipAI::OnLoop 0x6012e0 for the player
// (bAIon false, ShipAI ctor 0x600700) and for the enemy (bAIon true). The
// list is ShipManager::vCrewList: every CrewMember and CrewDrone physically
// aboard, in insertion order (native physicalCrewOrder).
class CrewAiPass {
public:
    CrewAiPass(Run& run, int ship)
        : run_(run), ship_(ship), aiOn_(ship == 1),
          state_(ship == 0 ? run.playerCrewAi_ : run.enemy_.crewAi) {}

    void onLoop();

private:
    struct Actor {
        RunCrew* crew = nullptr;
        CombatDrone* drone = nullptr;
        int owner = 0;
        std::uint64_t order = 0;
    };

    // Ship::vOuterWalls (Ship::FindOuterWalls 0x484180): one OuterHull per
    // tile, rooms in id order, each room column by column.
    struct Wall {
        int room = -1;
        int tile = -1;
        bool damaged = false;
    };

    Run& run_;
    int ship_;
    bool aiOn_;
    CrewAiState& state_;
    std::vector<Actor> crewList_;
    std::vector<std::size_t> intruderList_;
    std::vector<Wall> hullBreaches_;
    std::vector<bool> breachedRooms_;
    std::vector<CrewTask> desiredTasks_;
    std::vector<CrewTask> bonusTasks_;

    // --- ship queries ----------------------------------------------------
    const ShipDefinition& layout() const {
        return ship_ == 0 ? run_.ship_ : run_.enemy_.ship;
    }

    std::vector<RunSystem>& systems() {
        return ship_ == 0 ? run_.systems_ : run_.enemy_.systems;
    }

    const std::vector<RunSystem>& systems() const {
        return ship_ == 0 ? run_.systems_ : run_.enemy_.systems;
    }

    std::map<int, Run::RoomHazard>& hazards() {
        return ship_ == 0 ? run_.playerHazards_ : run_.enemyHazards_;
    }

    const std::map<int, Run::RoomHazard>& hazards() const {
        return ship_ == 0 ? run_.playerHazards_ : run_.enemyHazards_;
    }

    std::vector<bool>& doorStates() {
        return ship_ == 0 ? run_.doors_ : run_.enemy_.doors;
    }

    const std::map<int, int>& forcedDoors() const {
        return ship_ == 0 ? run_.voyageDoorForcedTicks_ : run_.enemy_.doorForcedTicks;
    }

    int roomCount() const {
        return int(layout().rooms.size());
    }

    const RoomDefinition* roomShape(int id) const {
        const auto& rooms = layout().rooms;
        const auto found = std::ranges::find(rooms, id, &RoomDefinition::id);
        return found == rooms.end() ? nullptr : &*found;
    }

    int doorCount(int room) const {
        // ShipGraph::DoorCount 0x459110: vDoorList doors per room (OnInit 0x45b430).
        return int(std::ranges::count_if(layout().doors, [room](const DoorDefinition& door) {
            return door.roomB != -1 && (door.roomA == room || door.roomB == room);
        }));
    }

    std::vector<int> roomAirlocks(int room) const {
        // Ship::GetRoomAirlocks 0x483c60 walks vOuterAirlocks for iRoom1 == room.
        std::vector<int> result;
        for (const auto& door : layout().doors) {
            if (door.roomB == -1 && door.roomA == room) {
                result.push_back(door.id);
            }
        }
        return result;
    }

    RunSystem* systemById(int id) {
        auto& list = systems();
        const auto found = std::ranges::find(list, id, [](const RunSystem& sys) {
            return sys.definition.id;
        });
        return found == list.end() ? nullptr : &*found;
    }

    const RunSystem* systemById(int id) const {
        const auto& list = systems();
        const auto found = std::ranges::find(list, id, [](const RunSystem& sys) {
            return sys.definition.id;
        });
        return found == list.end() ? nullptr : &*found;
    }

    RunSystem* systemInRoom(int room) {
        // ShipManager::GetSystemInRoom 0x494110: first vSystemList match.
        auto& list = systems();
        const auto found = std::ranges::find(list, room, [](const RunSystem& sys) {
            return sys.definition.room;
        });
        return found == list.end() ? nullptr : &*found;
    }

    int systemInRoomId(int room) {
        const auto* sys = systemInRoom(room);
        return sys ? sys->definition.id : -1;
    }

    int systemRoom(int id) const {
        const auto* sys = systemById(id);
        return sys ? sys->definition.room : -1;
    }

    bool functioning(const RunSystem* sys) const {
        // ShipSystem::Functioning 0x6153d0 for the systems CrewAI asks about.
        return sys && sys->power > 0;
    }

    int hackLevel(int id) const {
        // ShipManager::IsSystemHacked 0x495590 returns iHackEffect while attached.
        const auto* sys = systemById(id);
        return sys && sys->hackActive ? sys->hackLevel : 0;
    }

    bool doorsFunction() const {
        // ShipManager::DoorsFunction 0x494210.
        const auto* doors = systemById(8);
        return functioning(doors) && !run_.systemLocked(*doors);
    }

    float oxygenLevel(int room) const {
        // ShipManager::GetOxygenLevel 0x494f40 is zero without an oxygen system.
        if (!systemById(2)) {
            return 0;
        }
        const auto& oxygen = ship_ == 0 ? run_.roomOxygen_ : run_.enemy_.roomOxygen;
        const auto found = oxygen.find(room);
        return found == oxygen.end() ? 0.f : found->second;
    }

    int oxygenPercentage() const {
        // ShipManager::GetOxygenPercentage 0x494ee0: OxygenSystem::fTotalOxygen is
        // the clamped room sum over 100 per room (OxygenSystem::OnLoop 0x607670).
        if (!systemById(2) || roomCount() == 0) {
            return 0;
        }
        float total = 0;
        for (const auto& shape : layout().rooms) {
            total += std::clamp(oxygenLevel(shape.id), 0.f, 100.f);
        }
        return int(std::round(100.f * (total / float(100 * roomCount()))));
    }

    int fireCount(int room) const {
        const auto found = hazards().find(room);
        return found == hazards().end() ? 0 : found->second.fires;
    }

    bool needsRepairing(int room, const RunSystem& sys) const {
        // ShipSystem::NeedsRepairing 0x613ea0: damaged and the room is neither
        // burning (bOnFire) nor breached (bBreached).
        const auto found = hazards().find(room);
        const bool fire = found != hazards().end() && found->second.fires > 0;
        const bool breach = found != hazards().end() && found->second.breaches > 0;
        return sys.health < sys.definition.power && !fire && !breach;
    }

    bool roomLocked(int room) const {
        // Ship::RoomLocked 0x481340: a lockdown seals that room.
        const auto& locked = ship_ == 0 ? run_.lockdownRooms_ : run_.enemy_.lockdownRooms;
        return locked.contains(room);
    }

    void manipulateDoors(int room, bool open) {
        // ShipManager::ManipulateDoors 0x492380 -> Ship::ManipulateDoors 0x483ff0:
        // every interior door and airlock touching the room, when the door
        // system functions and is not locked.
        const auto* doors = systemById(8);
        if (!functioning(doors) || run_.systemLocked(*doors)) {
            return;
        }
        auto& states = doorStates();
        for (const auto& door : layout().doors) {
            if (door.roomA == room || door.roomB == room) {
                states[std::size_t(door.id)] = open;
            }
        }
    }

    static bool ioned(const RunSystem& sys, int amount) {
        // ShipSystem::Ioned 0x614ec0.
        const bool locked = sys.lockCount == -1 || sys.lockCount > 0;
        if (!locked && sys.hackLevel <= 1) {
            return false;
        }
        const int cap =
            sys.powerLimit <= 7 ? std::max(0, sys.powerLimit) : std::max(0, sys.definition.power);
        const int available = std::min(sys.health - sys.powerLoss, cap);
        const int unallocated = available - (sys.power - sys.batteryPower - sys.bonusPower);
        return unallocated >= available || amount <= unallocated;
    }

    bool roomConnected(int from, int to) const {
        // ShipGraph::IsRoomConnected 0x459300.
        if (from == to) {
            return true;
        }
        return std::ranges::any_of(layout().doors, [&](const DoorDefinition& door) {
            return door.roomB != -1 && ((door.roomA == from && door.roomB == to) ||
                                        (door.roomA == to && door.roomB == from));
        });
    }

    bool automated() const {
        return ship_ == 1 && run_.enemyAutomated();
    }

    // --- CrewMember virtuals over native actors --------------------------
    bool foreign(const Actor& a) const {
        return a.crew && a.owner == 0 && ship_ == 1;
    }

    std::string droneType(const Actor& a) const {
        return run_.catalog_.equipment(a.drone->blueprint).type;
    }

    bool ionDrone(const Actor& a) const {
        return a.drone && a.drone->blueprint == "BOARDER_ION";
    }

    std::string_view species(const Actor& a) const {
        if (a.crew) {
            return a.crew->appearance.species;
        }
        const auto type = droneType(a);
        return type == "REPAIR" ? "repair" : ionDrone(a) ? "boarder_ion" : "battle";
    }

    bool mindControlled(const Actor& a) const {
        return a.crew && a.crew->controlled;
    }

    bool dead(const Actor& a) const {
        return a.crew ? a.crew->dead : !a.drone || a.drone->crewDead;
    }

    bool dying(const Actor& a) const {
        // CrewAnimation status 3 (the death strip) before bDead is set.
        return a.crew ? a.crew->dying || (a.crew->health <= 0 && !a.crew->dead)
                      : a.drone->crewDying || (a.drone->health <= 0 && !a.drone->crewDead);
    }

    bool intruder(const Actor& a) const {
        if (a.drone) {
            // CrewDrone::GetIntruder 0x73ddd0.
            return a.owner != ship_ && a.drone->deployed && !a.drone->crewDead;
        }
        // CrewMember::GetIntruder 0x46c3e0.
        return mindControlled(a) ? a.owner == ship_ : a.owner != ship_;
    }

    bool controllable(const Actor& a) const {
        // CrewMember::GetControllable 0x720e50; CrewDrone 0x73df00 is false.
        return a.crew && a.owner == 0 && !dead(a) && !mindControlled(a);
    }

    bool functional(const Actor& a) const {
        // CrewMember::Functional 0x720b00 / CrewDrone::Functional 0x73dd70.
        return a.crew ? !dead(a) : a.drone->deployed && a.drone->powered;
    }

    bool canFight(const Actor& a) const {
        // BattleDrone/BoarderDrone fight; RepairDrone and IonDrone 0x734a00 do not.
        return a.crew || droneType(a) == "BATTLE" || (droneType(a) == "BOARDER" && !ionDrone(a));
    }

    bool canRepair(const Actor& a) const {
        if (a.drone) {
            // RepairDrone::CanRepair 0x723e30; other drones 0x73df80.
            return droneType(a) == "REPAIR" && a.drone->powered;
        }
        return !intruder(a) && !dead(a) && !dying(a);
    }

    bool canSabotage(const Actor& a) const {
        // CrewMember 0x720c60 (the intruder flag), BoarderDrone/IonDrone true.
        return a.crew ? intruder(a) : droneType(a) == "BOARDER";
    }

    bool canSuffocate(const Actor& a) const {
        return a.crew && species(a) != "anaerobic" && species(a) != "ghost";
    }

    bool canBurn(const Actor& a) const {
        return a.crew && species(a) != "rock";
    }

    bool anaerobic(const Actor& a) const {
        return a.crew && species(a) == "anaerobic";
    }

    float health(const Actor& a) const {
        return a.crew ? a.crew->health : a.drone->health;
    }

    float maxHealth(const Actor& a) const {
        if (a.crew) {
            return Run::crewMaximumHealth(a.crew->appearance.species) + a.crew->healthBoost;
        }
        return droneType(a) == "REPAIR" ? 25.f : ionDrone(a) ? 125.f : 150.f;
    }

    float healthRatio(const Actor& a) const {
        return health(a) / maxHealth(a);
    }

    bool badAtCombat(const Actor& a) const {
        // CrewMember::BadAtCombat 0x46e960.
        const auto kind = species(a);
        const float damage = kind == "mantis"   ? 1.5f
                             : kind == "engi"   ? .5f
                             : kind == "battle" ? 1.2f
                                                : 1.f;
        return damage < 1.f || maxHealth(a) < 100.f;
    }

    int roomOf(const Actor& a) const {
        if (a.drone) {
            return a.drone->crewRoom;
        }
        return foreign(a) ? a.crew->boardRoom : a.crew->room;
    }

    SDL_FPoint position(const Actor& a) const {
        if (a.drone) {
            return a.drone->crewPosition;
        }
        return foreign(a) ? SDL_FPoint{a.crew->boardX, a.crew->boardY}
                          : SDL_FPoint{a.crew->x, a.crew->y};
    }

    int slotRoom(const Actor& a) const {
        // CrewMember::currentSlot.roomId.
        if (a.drone) {
            return a.drone->targetRoom >= 0 ? a.drone->targetRoom : a.drone->crewRoom;
        }
        return foreign(a) ? a.crew->boardTargetRoom : a.crew->targetRoom;
    }

    SDL_FPoint slotPoint(const Actor& a) const {
        if (a.drone) {
            return a.drone->crewPath.empty() ? a.drone->crewPosition : a.drone->crewPath.back();
        }
        const int slot = foreign(a) ? a.crew->boardTargetSlot : a.crew->targetSlot;
        const auto* shape = roomShape(slotRoom(a));
        if (!shape || slot < 0) {
            return position(a);
        }
        const int columns = std::max(1, shape->w / 35);
        return {float(shape->x + 35 * (slot % columns) + 17),
                float(shape->y + 35 * (slot / columns) + 17)};
    }

    bool atSlot(const Actor& a) const {
        const auto point = position(a);
        const auto slot = slotPoint(a);
        return std::abs(point.x - slot.x) < .5f && std::abs(point.y - slot.y) < .5f;
    }

    bool frozen(const Actor& a) const {
        // CrewDrone::OnLoop 0x43ce20: SetFrozen(!powered || !deployed).
        return a.drone && (!a.drone->powered || !a.drone->deployed);
    }

    bool repairing(const Actor& a) const {
        return (a.crew ? a.crew->repairKind : a.drone->repairKind) != 0;
    }

    bool fighting(const Actor& a) const {
        // CrewMember::CheckFighting 0x4703b0: crewTarget is an opponent or the
        // sealed door GetNewGoal 0x471fb0 attacks.
        if (a.drone) {
            return a.drone->attackTargetOwner >= 0 || a.drone->crewBlockingDoor >= 0;
        }
        const int blocking = foreign(a) ? a.crew->boardBlockingDoor : a.crew->blockingDoor;
        return a.crew->attackTargetOwner >= 0 || blocking >= 0;
    }

    bool busy(const Actor& a) const {
        // CrewMember::IsBusy 0x46fa20.
        return repairing(a) || fighting(a) || !atSlot(a) || frozen(a);
    }

    float stun(const Actor& a) const {
        return a.crew ? a.crew->stunTime : a.drone->ionStun;
    }

    CrewTask& task(const Actor& a) const {
        if (a.crew) {
            return a.crew->aiTask;
        }
        if (!a.drone) {
            throw std::logic_error("crew_ai_actor_empty");
        }
        return a.drone->aiTask;
    }

    bool canMan(const Actor& a) const {
        // CrewMember::CanMan 0x720f70; CrewDrone 0x73df20 is false.
        return a.crew && !intruder(a) && stun(a) <= 0 && !dying(a);
    }

    int manningId(const Actor& a) {
        // UpdateCrewmembers 0x49ee10 clears iManningId, then SetCurrentSystem
        // 0x46e210 records every system in the room for a CanMan actor.
        return canMan(a) ? systemInRoomId(roomOf(a)) : -1;
    }

    bool activeManning(const Actor& a) {
        const int id = manningId(a);
        return id >= 0 && run_.crewOperator(id, ship_ == 1) == a.crew;
    }

    bool manned(const RunSystem& sys) const {
        // ShipSystem::bManned: bAutomated, or any CanMan actor in the room.
        return automated() || std::ranges::any_of(crewList_, [&](const Actor& a) {
                   return canMan(a) && roomOf(a) == sys.definition.room;
               });
    }

    int blockRoom(const Actor& a) const {
        // IonDrone::BlockRoom 0x4374a0 returns lastRoom; CrewMember -1.
        return ionDrone(a) ? a.drone->ionLastRoom : -1;
    }

    int damageType(const Actor& a) const {
        // IonDrone::IonDrone 0x43b820 sets boardingGoal.damageType = 1.
        return ionDrone(a) ? 1 : 0;
    }

    int crewCount(bool intruderSide, int room) const {
        // ShipManager::crewCounts[GetIntruder][room] from UpdateCrewmembers.
        return int(std::ranges::count_if(crewList_, [&](const Actor& a) {
            return !dead(a) && !dying(a) && intruder(a) == intruderSide && roomOf(a) == room;
        }));
    }

    int countBoarders(int room) const {
        // ShipManager::CountBoarders 0x490770.
        return int(std::ranges::count_if(crewList_, [&](const Actor& a) {
            return (!mindControlled(a) || fighting(a)) && intruder(a) && roomOf(a) == room &&
                   !dying(a);
        }));
    }

    int crewDoingTask(const CrewTask& wanted) const {
        // CrewAI::CrewDoingTask 0x602450 and the inlined counts that copy it.
        return int(std::ranges::count_if(crewList_, [&](const Actor& a) {
            return !intruder(a) && !a.drone && task(a) == wanted;
        }));
    }

    bool roomFull(int room, bool intruderSide) const {
        // Room::Full 0x46a0a0 on the occupancy side.
        return roomShape(room) == nullptr ||
               run_.crewFreeSlot(
                   nullptr, room, -1, ship_ == 1, nullptr, int(intruderSide), nullptr) < 0;
    }

    // --- movement --------------------------------------------------------
    bool moveToRoom(const Actor& a, int room);
    bool checkRoomPath(const Actor& a, int room) const;
    bool assignTask(const Actor& a, const CrewTask& next);
    void startRepair(const Actor& a, int kind, int target);

    // --- CrewAI ----------------------------------------------------------
    void checkForProblems();
    void closeAirlocks();
    bool safeBlowoutOxygen(int room);
    void selectRepair(const Actor& a);
    void updateCrewMember(std::size_t index);
    void updateDrones();
    bool attemptToAssign(const CrewTask& wanted);
    int prioritizeTask(const CrewTask& wanted, int crewId) const;
    bool dangerRating(int room, int crewId) const;
    void checkForHealing();
    void updateIntruders();
    int choosePriorityRoom(const Actor& a);
    int prioritizeIntruderRoom(const Actor& a, int room, int target) const;
};

bool CrewAiPass::moveToRoom(const Actor& a, int room) {
    // ShipManager::CommandCrewMoveRoom 0x4944d0 -> CrewMember::MoveToRoom
    // 0x472640 (slot -1, bForceMove false).
    if (stun(a) > 0 && atSlot(a)) {
        return false;
    }
    if (dead(a) || slotRoom(a) == room || room < 0) {
        return false;
    }
    if (a.crew) {
        // Native order() already reproduces the slot and path search.
        return run_.order(*a.crew, room, -1, true, ship_ == 1 && !foreign(a));
    }
    auto& drone = *a.drone;
    RunCrew proxy;
    proxy.health = std::max(1.f, drone.health);
    proxy.room = drone.crewRoom;
    proxy.targetRoom = -1;
    proxy.x = drone.crewPosition.x;
    proxy.y = drone.crewPosition.y;
    if (!run_.order(proxy, room, -1, true, ship_ == 1, nullptr, int(intruder(a)), &drone)) {
        return false;
    }
    drone.targetRoom = room;
    drone.workProgress = 0;
    drone.crewPath.clear();
    drone.crewPathDoors.clear();
    drone.crewPathIndex = 0;
    drone.crewBlockingDoor = -1;
    drone.crewNewGoal = true;
    for (const auto& waypoint : proxy.path) {
        drone.crewPath.push_back(waypoint.point);
        drone.crewPathDoors.push_back(waypoint.door);
    }
    return true;
}

bool CrewAiPass::checkRoomPath(const Actor& a, int room) const {
    // CrewMember::CheckRoomPath 0x46f4a0: FindSlot 0x46f270 (closeEnough
    // false, the actor's own slot still filled) and a FindPath distance.
    const auto* shape = roomShape(room);
    if (!shape) {
        return false;
    }
    const int slot =
        run_.crewFreeSlot(nullptr, room, -1, ship_ == 1, nullptr, int(intruder(a)), nullptr);
    if (slot < 0) {
        return false;
    }
    const int columns = std::max(1, shape->w / 35);
    const SDL_Point goal{shape->x + 35 * (slot % columns) + 17,
                         shape->y + 35 * (slot / columns) + 17};
    const auto point = position(a);
    return run_.findDoorPath(layout(), {int(point.x), int(point.y)}, goal, a.owner, ship_ == 1)
        .has_value();
}

bool CrewAiPass::assignTask(const Actor& a, const CrewTask& next) {
    // CrewAI::AssignTask 0x6020d0: the task follows a successful move order,
    // or applies in place when the actor already holds a slot in that room.
    if (dead(a) || roomLocked(task(a).room)) {
        return false;
    }
    if (moveToRoom(a, next.room) || slotRoom(a) == next.room) {
        task(a) = next;
    }
    return true;
}

void CrewAiPass::startRepair(const Actor& a, int kind, int target) {
    // CrewMember::StartRepair 0x473d10 requires the slot, a living actor and
    // CanRepair or CanSabotage, then calls the virtual UpdateRepair at once.
    if (!atSlot(a) || dead(a) || !(canRepair(a) || canSabotage(a))) {
        return;
    }
    if (ionDrone(a)) {
        // IonDrone::UpdateRepair 0x437490 is empty and its pulse owns the
        // effect; the native ion body keeps no repair task.
        return;
    }
    if (a.crew) {
        a.crew->repairKind = kind;
        a.crew->repairTarget = target;
        a.crew->repairRoom = roomOf(a);
        run_.advanceCrewRepair(*a.crew, a.owner, ship_);
        return;
    }
    auto& drone = *a.drone;
    RunCrew body;
    body.appearance.species = std::string(species(a));
    body.health = drone.health;
    body.room = drone.crewRoom;
    body.targetRoom = slotRoom(a);
    body.slot = body.targetSlot = drone.crewSlot;
    body.x = drone.crewPosition.x;
    body.y = drone.crewPosition.y;
    body.stunTime = drone.ionStun;
    body.pendingDamage = drone.pendingDamage;
    body.repairKind = kind;
    body.repairTarget = target;
    body.repairRoom = drone.crewRoom;
    body.onFire = drone.crewOnFire;
    run_.advanceCrewRepair(body, a.owner, ship_);
    drone.repairKind = body.repairKind;
    drone.repairTarget = body.repairTarget;
    drone.repairRoom = body.repairRoom;
    drone.pendingDamage = body.pendingDamage;
}

void CrewAiPass::selectRepair(const Actor& a) {
    // CrewAI::SelectRepair 0x601f00.
    if (dead(a)) {
        return;
    }
    const int room = roomOf(a);
    auto& list = systems();
    if (intruder(a)) {
        auto* sys = systemInRoom(room);
        // ShipSystem::CanBeSabatoged 0x722210.
        if (!sys || sys->health == 0) {
            return;
        }
        startRepair(a, 3, int(sys - list.data()));
        return;
    }
    // ShipManager::GetFires 0x4970c0 -> Spreader<Fire>::GetSquares 0x734ef0:
    // present fires, grid column by column.
    const auto* shape = roomShape(room);
    const int columns = shape ? std::max(1, shape->w / 35) : 1;
    const auto columnMajor = [columns](int tile) {
        return std::pair{tile % columns, tile / columns};
    };
    if (const auto found = hazards().find(room); found != hazards().end()) {
        const auto& hazard = found->second;
        int selected = -1;
        for (std::size_t index = 0; index < hazard.fireTiles.size(); ++index) {
            if (index < hazard.fireHealth.size() && hazard.fireHealth[index] <= 0) {
                continue;
            }
            const int tile = hazard.fireTiles[index];
            if (selected < 0 || columnMajor(tile) < columnMajor(selected)) {
                selected = tile;
            }
        }
        if (selected >= 0) {
            startRepair(a, 1, selected);
            return;
        }
    }
    for (const auto& wall : hullBreaches_) {
        if (wall.damaged && wall.room == room) {
            startRepair(a, 2, wall.tile);
            return;
        }
    }
    if (auto* sys = systemInRoom(room); sys && needsRepairing(room, *sys)) {
        startRepair(a, 3, int(sys - list.data()));
    }
}

void CrewAiPass::checkForProblems() {
    // CrewAI::CheckForProblems 0x603ee0.
    state_.calmShip = true;
    desiredTasks_.clear();
    bonusTasks_.clear();
    const bool anyCrew = !crewList_.empty();
    for (int room = 0; room < roomCount(); ++room) {
        if (doorCount(room) <= 0) {
            continue;
        }
        if (const int boarders = countBoarders(room); boarders > 0) {
            state_.calmShip = false;
            const CrewTask defend{7, room, systemInRoomId(room)};
            int missing = boarders;
            if (anyCrew) {
                const int assigned = crewDoingTask(defend);
                if (boarders == assigned) {
                    bonusTasks_.push_back(defend);
                    missing = 0;
                } else {
                    missing = boarders - assigned;
                }
            }
            for (int copy = 0; copy < missing; ++copy) {
                desiredTasks_.push_back(defend);
            }
        }
        if (fireCount(room) > 0) {
            state_.calmShip = false;
            if (oxygenLevel(room) >= 10.f) {
                const CrewTask fire{2, room, systemInRoomId(room)};
                const int assigned = anyCrew ? crewDoingTask(fire) : 0;
                if (assigned > 1) {
                    bonusTasks_.push_back(fire);
                }
                for (int copy = 0; copy < 2 - assigned; ++copy) {
                    desiredTasks_.push_back(fire);
                }
            }
        }
    }
    if (const int teleporter = systemRoom(9); teleporter != -1) {
        const CrewTask teleport{8, teleporter, -1};
        const int missing = state_.teleportRequest - crewDoingTask(teleport);
        for (int copy = 0; copy < missing; ++copy) {
            desiredTasks_.push_back(teleport);
        }
    }
    for (std::size_t index = 0; index < hullBreaches_.size(); ++index) {
        const auto& wall = hullBreaches_[index];
        if (doorCount(wall.room) <= 0) {
            continue;
        }
        if (!wall.damaged) {
            breachedRooms_[std::size_t(wall.room)] = false;
            continue;
        }
        state_.calmShip = false;
        breachedRooms_[std::size_t(wall.room)] = true;
        if (aiOn_) {
            manipulateDoors(wall.room, false);
        }
        // The original passes the OuterHull index, not its room, to
        // GetSystemInRoom here.
        const CrewTask breach{3, wall.room, systemInRoomId(int(index))};
        if (anyCrew && crewDoingTask(breach) != 0) {
            bonusTasks_.push_back(breach);
        } else {
            desiredTasks_.push_back(breach);
        }
    }
    for (const auto& sys : systems()) {
        const int room = sys.definition.room;
        if (doorCount(room) <= 0) {
            continue;
        }
        const CrewTask repair{1, room, sys.definition.id};
        if (needsRepairing(room, sys)) {
            state_.calmShip = false;
            if (anyCrew && crewDoingTask(repair) != 0) {
                bonusTasks_.push_back(repair);
            } else {
                desiredTasks_.push_back(repair);
            }
        }
        if (manned(sys)) {
            continue;
        }
        const CrewTask man{0, room, sys.definition.id};
        if (!anyCrew || crewDoingTask(man) == 0) {
            desiredTasks_.push_back(man);
        }
    }
    state_.breachedRooms.clear();
    for (std::size_t room = 0; room < breachedRooms_.size(); ++room) {
        if (breachedRooms_[room]) {
            state_.breachedRooms.insert(int(room));
        }
    }
}

void CrewAiPass::closeAirlocks() {
    // CrewAI::CloseAirlocks 0x6021c0. The fire test uses the airlock's index in
    // the room's list as a room id.
    auto& open = doorStates();
    for (int room = 0; room < roomCount(); ++room) {
        const auto airlocks = roomAirlocks(room);
        for (std::size_t index = 0; index < airlocks.size(); ++index) {
            if (fireCount(int(index)) != 0) {
                continue;
            }
            const int id = airlocks[index];
            if ((open[std::size_t(id)] || forcedDoors().contains(id)) && doorsFunction()) {
                open[std::size_t(id)] = false;
            }
        }
    }
}

bool CrewAiPass::safeBlowoutOxygen(int room) {
    // CrewAI::SafeBlowoutOxygen 0x6022c0 (enemy ship only). Callers pass the
    // desired-task index, not the task's room.
    if (ship_ == 0) {
        return false;
    }
    const auto airlocks = roomAirlocks(room);
    if (airlocks.empty()) {
        return false;
    }
    auto& open = doorStates();
    for (const int id : airlocks) {
        for (const auto& a : crewList_) {
            if (intruder(a) || a.drone) {
                continue;
            }
            if (task(a).room == room || roomOf(a) == room) {
                return false;
            }
        }
        if (oxygenLevel(room) < 10.f) {
            return false;
        }
        if (doorsFunction() && !open[std::size_t(id)] && !forcedDoors().contains(id)) {
            open[std::size_t(id)] = true;
        }
    }
    return true;
}

void CrewAiPass::updateCrewMember(std::size_t index) {
    // CrewAI::UpdateCrewMember 0x603410. The task and slot room are read
    // before SelectRepair.
    const auto& a = crewList_[index];
    CrewTask current = task(a);
    const int slot = slotRoom(a);
    if (!repairing(a)) {
        selectRepair(a);
    }
    if (intruder(a)) {
        return;
    }
    if (controllable(a)) {
        // Player-controllable crew only report their situation.
        CrewTask next{4, slot, systemInRoomId(slot)};
        const auto* sys = systemInRoom(slot);
        if (crewCount(true, slot) > 0) {
            next.taskId = 7;
        } else if (fireCount(slot) > 0) {
            next.taskId = 2;
        } else if ((sys && needsRepairing(slot, *sys)) || repairing(a)) {
            next.taskId = 1;
        } else if (activeManning(a)) {
            next.taskId = 0;
        }
        task(a) = next;
        return;
    }
    if (!busy(a) && current.taskId != 0) {
        if (current.taskId == 6 && double(healthRatio(a)) < .99) {
            current.taskId = hackLevel(5) <= 1 ? 6 : 4;
        } else if (current.taskId != 8 || state_.teleportRequest == 0) {
            const int manning = manningId(a);
            if (manning == -1) {
                current = {4, roomOf(a), -1};
            } else {
                if (manning != 11) {
                    current.room = systemRoom(manning);
                }
                current = crewDoingTask({0, current.room, manning}) != 0
                              ? CrewTask{4, current.room, -1}
                              : CrewTask{0, current.room, manning};
            }
        }
        task(a) = current;
    }
    const auto finish = [&] {
        if (dead(a)) {
            task(a) = {5, -1, -1};
        }
    };
    bool sameRoomTest = true;
    if (current.taskId == 7) {
        if (crewCount(true, slot) <= 0) {
            current.taskId = 4;
            task(a) = current;
        }
        if (busy(a)) {
            finish();
            return;
        }
    } else if (busy(a)) {
        sameRoomTest = false;
    }
    if (sameRoomTest && roomOf(a) != current.room) {
        task(a) = {4, roomOf(a), -1};
        finish();
        return;
    }
    if (!busy(a) && current.taskId == 0 && crewDoingTask({0, current.room, current.system}) > 1) {
        task(a) = {4, current.room, -1};
        finish();
        return;
    }
    if (current.taskId == 8 && state_.teleportRequest < crewDoingTask(current)) {
        task(a) = {4, roomOf(a), -1};
    }
    finish();
}

void CrewAiPass::updateDrones() {
    // CrewAI::UpdateDrones 0x604b10: own-ship crew drones.
    for (std::size_t index = 0; index < crewList_.size(); ++index) {
        const auto& a = crewList_[index];
        if (!a.drone || intruder(a)) {
            continue;
        }
        std::vector<CrewTask> potential = desiredTasks_;
        potential.insert(potential.end(), bonusTasks_.begin(), bonusTasks_.end());
        if (canFight(a) && functional(a)) {
            // Anti-personnel drones take the nearest intruder task whose room
            // still has a friendly slot (or is their current task room).
            const SDL_FPoint point{float(int(position(a).x)), float(int(position(a).y))};
            const int taskRoom = task(a).room;
            CrewTask boarders{4, 0, -1};
            int closest = std::numeric_limits<int>::max();
            for (const auto& candidate : potential) {
                if (candidate.taskId != 7 ||
                    (roomFull(candidate.room, false) && candidate.room != taskRoom)) {
                    continue;
                }
                const auto* shape = roomShape(candidate.room);
                if (!shape) {
                    continue;
                }
                // ShipManager::GetRoomCenter -> ShipGraph::GetRoomCenter 0x45b020.
                const SDL_FPoint center{float(shape->x + shape->w / 2),
                                        float(shape->y + shape->h / 2)};
                const int distance = int((point.y - center.y) * (point.y - center.y) +
                                         (point.x - center.x) * (point.x - center.x));
                if (distance < closest) {
                    closest = distance;
                    boarders = candidate;
                }
            }
            if (boarders.taskId != 4) {
                assignTask(a, boarders);
            }
        } else if (!busy(a) && canRepair(a)) {
            // Repair drones keep improving on the repair, fire and breach tasks.
            task(a) = {4, roomOf(a), -1};
            for (const auto& candidate : potential) {
                if (candidate.taskId >= 1 && candidate.taskId <= 3 &&
                    prioritizeTask(candidate, -1) < prioritizeTask(task(a), -1)) {
                    assignTask(a, candidate);
                }
            }
        }
        if (!busy(a)) {
            assignTask(a, {4, systemRoom(4), -1});
        }
    }
}

int CrewAiPass::prioritizeTask(const CrewTask& wanted, int crewId) const {
    // CrewAI::PrioritizeTask 0x602ab0; lower values are more urgent.
    if (crewId != -1) {
        if (wanted.taskId == 5) {
            return -1;
        }
        if (!dangerRating(wanted.room, crewId)) {
            return 100;
        }
        if (wanted.taskId == 7 || wanted.taskId == 8) {
            const auto& a = crewList_[std::size_t(crewId)];
            if (state_.multiracial && badAtCombat(a)) {
                return 100;
            }
            if (wanted.taskId == 8) {
                if (healthRatio(a) < .5f) {
                    return 100;
                }
                const auto* teleporter = systemById(9);
                // bUrgentTeleport is cleared by the CrewAI ctor and never set.
                return teleporter && !run_.systemLocked(*teleporter) && functioning(teleporter)
                           ? 32
                           : 65;
            }
        }
    }
    const auto* inRoom = [&]() -> const RunSystem* {
        const auto& list = systems();
        const auto found = std::ranges::find(list, wanted.room, [](const RunSystem& sys) {
            return sys.definition.room;
        });
        return found == list.end() ? nullptr : &*found;
    }();
    const auto rank = [&](int base, int fallback) {
        if (!inRoom) {
            return fallback;
        }
        switch (inRoom->definition.id) {
        case 0:
            return base;
        case 1:
        case 4:
        case 6:
            return base + 4;
        case 2:
            return base + 3;
        case 3:
        case 13:
            return base + 1;
        case 5:
            return base + 2 - int(state_.medbayRequested);
        case 7:
        case 12:
            return base + 6;
        case 8:
            return base + 7;
        case 9:
            return base + (state_.teleportRequest == 0 ? 3 : 1);
        case 10:
        case 14:
        case 15:
            return base + 5;
        case 11:
            return base + 2;
        default:
            return fallback;
        }
    };
    switch (wanted.taskId) {
    case 0:
        switch (wanted.system) {
        case 0:
            return run_.difficulty_ == 2 ? 67 : 65;
        case 1:
            return run_.difficulty_ == 2 ? 65 : 67;
        case 2:
            return 68;
        case 3:
        case 11:
            return 66;
        case 5:
            return 72;
        case 6:
            return 64;
        case 7:
            return 69;
        case 8:
        case 10:
            return 70;
        case 9:
            return 71;
        default:
            return 1000;
        }
    case 1:
        switch (wanted.system) {
        case 0:
            return 1;
        case 2:
            return oxygenPercentage() > 24 ? 51 : 0;
        case 1:
        case 4:
        case 6:
            return 52;
        case 3:
        case 13:
            return 49;
        case 5:
            return 50 - int(state_.medbayRequested);
        case 7:
        case 12:
            return 54;
        case 8:
            return 55;
        case 9:
            return 48 + (state_.teleportRequest == 0 ? 3 : 1);
        case 10:
        case 14:
        case 15:
            return 53;
        case 11:
            return 50;
        default:
            return 56;
        }
    case 2:
    case 7:
        return rank(16, 24);
    case 3:
        return rank(32, 40);
    case 5:
        return -1;
    case 6:
        return 0;
    case 8: {
        const auto* teleporter = systemById(9);
        return teleporter && !run_.systemLocked(*teleporter) && functioning(teleporter) ? 32 : 65;
    }
    default:
        return 1000;
    }
}

bool CrewAiPass::dangerRating(int room, int crewId) const {
    // CrewAI::DangerRating 0x602500: false means the room is dangerous.
    const auto& a = crewList_[std::size_t(crewId)];
    const float ratio = healthRatio(a);
    const int fires = fireCount(room);
    const auto& open = ship_ == 0 ? run_.doors_ : run_.enemy_.doors;
    for (const int id : roomAirlocks(room)) {
        if (open[std::size_t(id)] || forcedDoors().contains(id)) {
            return false;
        }
    }
    if (ratio < .25f && oxygenLevel(room) < 10.f && canSuffocate(a) && !anaerobic(a)) {
        return false;
    }
    if ((fires > 0 && ratio < .2f && canBurn(a)) || (fires > 3 && canBurn(a))) {
        return false;
    }
    if (room == systemRoom(5) && hackLevel(5) > 1) {
        return false;
    }
    if (room >= 0 && std::size_t(room) < breachedRooms_.size() &&
        breachedRooms_[std::size_t(room)]) {
        const auto* shape = roomShape(room);
        if (shape && fires == shape->w / 35 * (shape->h / 35)) {
            return false;
        }
    }
    return true;
}

bool CrewAiPass::attemptToAssign(const CrewTask& wanted) {
    // CrewAI::AttemptToAssign 0x602e90.
    int crewId = -1;
    int taskValue = -1;
    for (std::size_t index = 0; index < crewList_.size(); ++index) {
        const auto& a = crewList_[index];
        const CrewTask current = task(a);
        if (intruder(a) || a.drone || controllable(a)) {
            continue;
        }
        if (doorCount(roomOf(a)) <= 0 || current == wanted) {
            continue;
        }
        const int id = int(index);
        if (prioritizeTask(current, id) < prioritizeTask(wanted, id)) {
            continue;
        }
        if (crewId == -1 || prioritizeTask(current, id) > taskValue) {
            crewId = id;
            taskValue = prioritizeTask(current, id);
        }
        const bool roomTask = (current.taskId >= 1 && current.taskId <= 3) || current.taskId == 7;
        if (roomTask && current.room == wanted.room) {
            assignTask(a, wanted);
            return true;
        }
    }
    if (crewId != -1 && taskValue > prioritizeTask(wanted, crewId)) {
        assignTask(crewList_[std::size_t(crewId)], wanted);
        return true;
    }
    if (state_.multiracial && (wanted.taskId == 7 || wanted.taskId == 8)) {
        state_.multiracial = false;
        state_.overrideRace = true;
        attemptToAssign(wanted);
        return false;
    }
    return state_.multiracial;
}

void CrewAiPass::checkForHealing() {
    // CrewAI::CheckForHealing 0x602770.
    state_.hurtCrew = false;
    for (std::size_t index = 0; index < crewList_.size(); ++index) {
        const auto& a = crewList_[index];
        const int taskId = task(a).taskId;
        if (intruder(a) || a.drone) {
            continue;
        }
        const float ratio = healthRatio(a);
        if (ratio < .25f || (ratio < .99f && state_.calmShip)) {
            state_.hurtCrew = true;
            const auto* medbay = systemById(5);
            if (!medbay) {
                return;
            }
            const int room = medbay->definition.room;
            if (functioning(medbay) && taskId != 6 && dangerRating(room, int(index)) &&
                hackLevel(5) <= 1) {
                assignTask(a, {6, room, -1});
                state_.medbayRequested = false;
                continue;
            }
            state_.medbayRequested = true;
        }
        if (taskId == 6) {
            state_.hurtCrew = true;
        }
    }
}

int CrewAiPass::prioritizeIntruderRoom(const Actor& a, int room, int target) const {
    // CrewAI::PrioritizeIntruderRoom 0x601c10; lower values win.
    const int crewRoom = roomOf(a);
    const auto* doors = systemById(8);
    // ShipManager::GetSystemPower 0x493cc0 returns -1 without a door system.
    const bool doorsPowered = !doors || doors->power != 0;
    const auto& list = systems();
    const auto system = std::ranges::find(list, room, [](const RunSystem& sys) {
        return sys.definition.room;
    });
    bool systemTarget = system != list.end() && system->health != 0;
    if (damageType(a) != 0) {
        if (system == list.end()) {
            return 1000;
        }
        // ShipSystem::Ioned 0x614ec0 with amount 4.
        systemTarget = systemTarget && !ioned(*system, 4);
    }
    const int residents = crewCount(false, room);
    const auto burning = [&] {
        return fireCount(room) > 0 && canBurn(a);
    };
    if (residents > 0 && doorsPowered && room == crewRoom && canFight(a) && room != blockRoom(a)) {
        return -10;
    }
    int hazard = burning() ? 100 : 0;
    if (oxygenLevel(room) < 10.f && canSuffocate(a)) {
        hazard = burning() ? 170 : 70;
    }
    const auto* shape = roomShape(room);
    const int slots = shape ? shape->w / 35 * (shape->h / 35) : 0;
    const int size = int(intruderList_.size()) != slots ? 5 : 0;
    int location = roomConnected(crewRoom, room) ? 0 : 10;
    if (room == blockRoom(a)) {
        location += 100;
    }
    int score = location + size + hazard;
    if (target == 0 && residents <= 0) {
        score += 20;
    }
    if (target == 1 && !systemTarget) {
        score += 20;
    }
    return score;
}

int CrewAiPass::choosePriorityRoom(const Actor& a) {
    // CrewAI::ChoosePriorityRoom 0x603aa0 (boardingGoal.target is 1 for every
    // CrewMember, event boarder and boarding drone).
    constexpr int target = 1;
    const int current = task(a).room;
    const int currentScore = prioritizeIntruderRoom(a, current, target);
    int best = currentScore;
    std::vector<int> possible;
    for (int room = 0; room < roomCount(); ++room) {
        if (roomFull(room, true) || doorCount(room) <= 0 || !checkRoomPath(a, room)) {
            continue;
        }
        const int score = prioritizeIntruderRoom(a, room, target);
        if (score < best) {
            best = score;
            possible.assign(1, room);
        } else if (score == best && currentScore != score) {
            possible.push_back(room);
        }
    }
    if (possible.empty()) {
        return current;
    }
    return possible[run_.combatRandom_.next() % possible.size()];
}

void CrewAiPass::updateIntruders() {
    // CrewAI::UpdateIntruders 0x603ca0.
    if (intruderList_.empty()) {
        return;
    }
    for (const std::size_t index : intruderList_) {
        const auto& a = crewList_[index];
        const CrewTask current = task(a);
        if (atSlot(a) && current.room != roomOf(a)) {
            task(a) = {current.taskId, roomOf(a), systemInRoomId(roomOf(a))};
        }
    }
    // The first intruder with a door picks a room and the following intruders
    // join it; the first one that cannot move leads the next group.
    std::size_t leaderIndex = 0;
    while (leaderIndex < intruderList_.size()) {
        const auto& leader = crewList_[intruderList_[leaderIndex]];
        if (doorCount(roomOf(leader)) <= 0) {
            ++leaderIndex;
            continue;
        }
        const int room = choosePriorityRoom(leader);
        const CrewTask led = task(leader);
        std::size_t member = leaderIndex;
        for (; member < intruderList_.size(); ++member) {
            const auto& a = crewList_[intruderList_[member]];
            if (room == task(a).room) {
                continue;
            }
            if (!moveToRoom(a, room)) {
                break;
            }
            task(a) = {led.taskId, room, led.system};
        }
        if (member == intruderList_.size() || member == leaderIndex) {
            return;
        }
        leaderIndex = member;
    }
}

void CrewAiPass::onLoop() {
    // CrewAI::OnLoop 0x605270.
    crewList_.clear();
    const auto add = [&](RunCrew& crew, int owner) {
        crewList_.push_back({&crew, nullptr, owner, crew.physicalCrewOrder});
    };
    if (ship_ == 0) {
        for (auto& member : run_.crew_) {
            if (!member.outOfGame && !member.intruder) {
                add(member, 0);
            }
        }
        for (auto& member : run_.voyageBoarders_) {
            if (!member.outOfGame) {
                add(member, 1);
            }
        }
    } else if (run_.enemy_.present) {
        for (auto& member : run_.crew_) {
            if (!member.outOfGame && member.intruder && member.boardPositionValid) {
                add(member, 0);
            }
        }
        for (auto& member : run_.enemy_.crew) {
            if (!member.outOfGame && !member.intruder) {
                add(member, 1);
            }
        }
    }
    for (const int owner : {0, 1}) {
        for (auto* drone : run_.physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == ship_ && !drone->crewDead && drone->crewRoom >= 0) {
                crewList_.push_back({nullptr, drone, owner, drone->physicalCrewOrder});
            }
        }
    }
    std::ranges::stable_sort(crewList_, {}, &Actor::order);

    // Zoltan and Engi crews keep combat-capable races on defence while the
    // ship also has fighters, until AttemptToAssign sets bOverrideRace.
    bool fighter = false;
    bool nonFighter = false;
    for (const auto& a : crewList_) {
        const auto kind = species(a);
        (kind == "engi" || kind == "energy" ? nonFighter : fighter) = true;
    }
    state_.multiracial = fighter && nonFighter && !state_.overrideRace;

    hullBreaches_.clear();
    for (const auto& shape : layout().rooms) {
        const int columns = std::max(1, shape.w / 35);
        const int rows = std::max(1, shape.h / 35);
        const auto found = hazards().find(shape.id);
        for (int column = 0; column < columns; ++column) {
            for (int row = 0; row < rows; ++row) {
                const int tile = row * columns + column;
                bool damaged = false;
                if (found != hazards().end()) {
                    const auto& hazard = found->second;
                    for (std::size_t index = 0; index < hazard.breachTiles.size(); ++index) {
                        if (hazard.breachTiles[index] == tile &&
                            (index >= hazard.breachHealth.size() ||
                             hazard.breachHealth[index] > 0)) {
                            damaged = true;
                        }
                    }
                }
                hullBreaches_.push_back({shape.id, tile, damaged});
            }
        }
    }
    breachedRooms_.assign(std::size_t(roomCount()), false);
    intruderList_.clear();
    for (std::size_t index = 0; index < crewList_.size(); ++index) {
        const auto& a = crewList_[index];
        if (!controllable(a) && functional(a) && intruder(a)) {
            intruderList_.push_back(index);
        }
    }
    if (!crewList_.empty()) {
        checkForProblems();
    }
    const auto assignDesired = [&] {
        for (std::size_t index = 0; index < desiredTasks_.size(); ++index) {
            const auto wanted = desiredTasks_[index];
            if ((wanted.taskId == 7 || wanted.taskId == 2) && safeBlowoutOxygen(int(index))) {
                continue;
            }
            attemptToAssign(wanted);
        }
    };
    if (aiOn_) {
        closeAirlocks();
        for (std::size_t index = 0; index < crewList_.size(); ++index) {
            updateCrewMember(index);
        }
        updateDrones();
        assignDesired();
        checkForHealing();
        updateIntruders();
        return;
    }
    for (std::size_t index = 0; index < crewList_.size(); ++index) {
        updateCrewMember(index);
    }
    assignDesired();
    updateIntruders();
    updateDrones();
}

void Run::advanceCrewAi(int physicalShip) {
    if (physicalShip == 1) {
        placeBoardingCrew();
    }
    CrewAiPass(*this, physicalShip).onLoop();
    if (physicalShip == 0) {
        endVoyageBoarding();
        return;
    }
    updateEnemyTeleportRequest();
    // ShipAI::OnLoop 0x6012e0 passes its request to CrewAI::SetTeleportRequest
    // 0x601a20 after the pass, unless the player has a super shield; the value
    // is capped by TeleportSystem::iNumSlots (the room's tile count).
    if (superShields_ <= 0) {
        const auto teleporter = std::ranges::find_if(enemy_.systems, [](const RunSystem& sys) {
            return sys.definition.id == 9;
        });
        int slots = 0;
        if (teleporter != enemy_.systems.end()) {
            const auto shape = std::ranges::find(
                enemy_.ship.rooms, teleporter->definition.room, &RoomDefinition::id);
            slots = shape == enemy_.ship.rooms.end() ? 0 : shape->w / 35 * (shape->h / 35);
        }
        enemy_.crewAi.teleportRequest = std::min(slots, enemy_.teleportRequest);
    }
}

} // namespace ftl
