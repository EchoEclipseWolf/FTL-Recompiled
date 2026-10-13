#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ftl {
void Run::saveBeaconShip(int beacon) {
    if (beacon < 0) {
        beacon = graph_.current();
    }
    // ClearLocation5ef130 / SaveToBlueprint4a28a0 stores a blueprint, not live timers.
    if (!enemy_.present || !hostile_ || surrenderOffered_ || enemy_.escaping || enemy_.hull <= 0 ||
        enemyExplosionActive_ || enemy_.ship.id.starts_with("BOSS")) {
        graph_.setSavedShip(beacon, nullptr);
        return;
    }
    auto blueprint = enemy_.ship;
    blueprint.hull = enemy_.hull;
    blueprint.missiles = enemy_.missiles;
    blueprint.droneParts = enemy_.droneParts;
    // SaveToBlueprint4a28a0 derives the weapon count from its live vector,
    // but assigns the drone list count from DroneSystem.drone_count (parts).
    blueprint.weaponCount = static_cast<int>(blueprint.weapons.size());
    blueprint.droneCount = enemy_.droneParts;
    const auto& authored = catalog_.ship(blueprint.id);
    for (auto& definition : blueprint.systems) {
        const auto found = std::ranges::find_if(enemy_.systems, [&](const auto& sys) {
            return sys.definition.id == definition.id;
        });
        definition.installed = found != enemy_.systems.end();
        definition.power = definition.installed ? found->definition.power : 0;
        // The source copies myBlueprint.systemInfo: chosen physical primary
        // slots are not written back into its authored SystemTemplate.
        const auto source = std::ranges::find_if(authored.systems, [&](const auto& system) {
            return system.id == definition.id && system.room == definition.room;
        });
        if (source != authored.systems.end()) {
            definition.slot = source->slot;
            definition.direction = source->direction;
        }
    }
    nlohmann::json members = nlohmann::json::array();
    for (const auto& member : enemy_.crew) {
        if (member.dead || member.outOfGame || member.intruder) {
            continue;
        }
        members.push_back({{"appearance", member.appearance},
                           {"skills",
                            std::array<int, 6>{member.manningSkillProgress[0],
                                               member.manningSkillProgress[1],
                                               member.manningSkillProgress[2],
                                               member.weaponSkillProgress,
                                               member.repairSkillProgress,
                                               member.combatSkillProgress}}});
    }
    // WorldManager::ClearLocation 0x5ef130 saves any hostile ship that has
    // not surrendered, escaped or been destroyed; it has no crew test. A
    // crewed ship without crew aboard is not kept, but an automated one
    // (ShipGenerator::CreateShip 0x5fca40: bAutomated when no crew is
    // generated, crewCount amount 0) always is.
    if (!enemyAutomated() && members.empty()) {
        graph_.setSavedShip(beacon, nullptr);
        return;
    }
    graph_.setSavedShip(beacon,
                        {{"blueprint", blueprint},
                         {"crew", members},
                         {"encounter", encounter_},
                         {"auto_blueprint", encounterAutoBlueprint_},
                         {"ship_seed", encounterShipSeed_},
                         {"boarding_ai", enemy_.boardingAi}});
}

void Run::restoreBeaconShip() {
    const auto& saved = graph_.beacons()[graph_.current()].savedShip;
    if (saved.is_null()) {
        return;
    }
    Enemy next;
    next.ship = saved.at("blueprint").get<ShipDefinition>();
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
    if (saved.contains("boarding_ai")) {
        nativeRead(saved.at("boarding_ai"), next.boardingAi);
    } else {
        const auto document = xmlResource(archive_, next.ship.source);
        auto root = document.child("FTL");
        if (!root) {
            root = document;
        }
        for (const auto node : root.children("shipBlueprint")) {
            if (std::string_view(node.attribute("name").as_string()) == next.ship.id) {
                const std::string_view boarding = node.child_value("boardingAI");
                next.boardingAi = boarding == "invasion" ? 2 : boarding == "sabotage" ? 1 : 0;
                break;
            }
        }
    }
    next.hull = next.ship.hull;
    next.missiles = next.ship.missiles;
    next.droneParts = next.ship.droneParts;
    next.present = true;
    int artilleryOrdinal = 0;
    int reactorUsed = 0;
    for (int id = 0; id < 16; ++id) {
        for (auto& definition : next.ship.systems) {
            if (definition.id != id || !definition.installed) {
                continue;
            }
            artilleryOrdinal += definition.id == 11;
            initializeEnemySystemStation(definition, next.ship, artilleryOrdinal, eventRandom_);
            const bool needsPower = id != 6 && id != 7 && id != 8 && id != 12;
            int power = needsPower ? id == 3 || id == 4
                                         ? 0
                                         : std::min(definition.power,
                                                    std::max(0, next.ship.reactor - reactorUsed))
                                   : definition.power;
            if (needsPower) {
                reactorUsed += power;
            }
            next.systems.push_back({definition, power, definition.power});
            next.systems.back().restorePower = power;
            if (definition.id == 5) {
                const auto room = std::ranges::find_if(next.ship.rooms, [&](const auto& candidate) {
                    return candidate.id == definition.room;
                });
                next.systems.back().damagingEffectTimes =
                    initializeMedbayAnimations(*room, eventRandom_);
            } else if (definition.id == 11 && !definition.weapon.empty()) {
                initializeWeaponFactory(next.systems.back().artillery,
                                        catalog_.equipment(definition.weapon),
                                        eventRandom_);
            } else if (definition.id == 15) {
                next.hackingDrone.blueprint = "DRONE_HACKING";
                initializeCrewDroneAnimation(next.hackingDrone, eventRandom_, &next.ship, 1);
            }
            if (definition.id == 0) {
                next.shields = power / 2;
            }
        }
    }
    next.superShields = std::ranges::find(next.ship.augments, std::string("ENERGY_SHIELD")) !=
                                next.ship.augments.end()
                            ? 5
                            : 0;
    next.maxSuperShields = next.superShields;
    // Re-entering an actualBlueprint uses CreateArmory4a6520 unchanged:
    // independent authored list counts and installed controllers gate birth.
    const bool hasWeapons = std::ranges::any_of(next.systems, [](const auto& system) {
        return system.definition.id == 3;
    });
    const bool hasDrones = std::ranges::any_of(next.systems, [](const auto& system) {
        return system.definition.id == 4;
    });
    next.ship.weapons.resize(hasWeapons
                                 ? std::min({next.ship.weapons.size(),
                                             std::size_t(std::max(0, next.ship.weaponCount)),
                                             std::size_t(std::max(0, next.ship.weaponSlots) + 1)})
                                 : 0);
    auto initialDroneBirths = next.ship.drones;
    initialDroneBirths.resize(hasDrones ? std::min(initialDroneBirths.size(),
                                                   std::size_t(std::max(0, next.ship.droneCount)))
                                        : 0);
    next.ship.drones.resize(
        std::min(initialDroneBirths.size(), std::size_t(std::max(0, next.ship.droneSlots))));
    next.weapons.resize(next.ship.weapons.size());
    for (std::size_t index = 0; index < next.weapons.size(); ++index) {
        initializeWeaponFactory(
            next.weapons[index], catalog_.equipment(next.ship.weapons[index]), eventRandom_);
    }
    next.powered.assign(next.weapons.size(), false);
    for (std::size_t index = 0; index < initialDroneBirths.size(); ++index) {
        const auto& id = initialDroneBirths[index];
        CombatDrone drone{.blueprint = id};
        initializeCrewDroneAnimation(drone, eventRandom_, &next.ship, 1);
        const auto& definition = catalog_.equipment(id);
        drone.health = definition.id == "BOARDER_ION"                                ? 125.f
                       : definition.type == "REPAIR"                                 ? 25.f
                       : definition.type == "BATTLE" || definition.type == "BOARDER" ? 150.f
                                                                                     : 1.f;
        drone.space = 1;
        if (index < next.ship.drones.size()) {
            next.drones.push_back(std::move(drone));
        } else {
            next.droneTrash.push_back(std::move(drone));
        }
    }
    const std::string profile = saved.at("encounter").get<std::string>();
    for (const auto& shape : next.ship.rooms) {
        next.roomOxygen[shape.id] = 100.f;
    }
    const auto stationRooms = initialEnemyCrewRooms(next.ship, difficulty_);
    for (const auto& row : saved.at("crew")) {
        RunCrew member;
        member.appearance = row.at("appearance").get<HangarCrew>();
        const auto index = next.crew.size();
        const int preferredRoom =
            index < stationRooms.size()
                ? stationRooms[index]
                : next.ship.rooms[eventRandom_.next() % next.ship.rooms.size()].id;
        initializeCrewAnimation(member, eventRandom_);
        member.health = crewMaximumHealth(member.appearance.species);
        const auto progress = row.at("skills").get<std::array<int, 6>>();
        const std::array<int, 6> caps = member.appearance.species == "human"
                                            ? std::array<int, 6>{26, 26, 100, 116, 32, 14}
                                            : std::array<int, 6>{30, 30, 110, 130, 36, 16};
        for (std::size_t i = 0; i < 3; ++i) {
            member.manningSkillProgress[i] = progress[i];
            member.manningSkill[i] = combatSkillTier(progress[i], caps[i]);
        }
        member.weaponSkillProgress = progress[3];
        member.weaponSkill = combatSkillTier(progress[3], caps[3]);
        member.repairSkillProgress = progress[4];
        member.repairSkill = combatSkillTier(progress[4], caps[4]);
        member.combatSkillProgress = progress[5];
        member.combatSkill = combatSkillTier(progress[5], caps[5]);
        // Saved custom CrewBlueprint colors are already chosen; reconstruct
        // only the animation timer, then use the same SetRoom placement as a
        // fresh physical ship rather than the former five-system shortcut.
        placeVoyageCrew(member, preferredRoom, false, true, &next);
        assignPhysicalCrewOrder(member);
        next.crew.push_back(std::move(member));
    }

    next.fireSpreaderAnimations = initializeFireSpreaderAnimations(next.ship, eventRandom_);
    advanceEnemyConstructorClocks(next);
    next.crewAi.startingCrewCount = static_cast<int>(next.crew.size());
    for (const auto* list : {&next.drones, &next.droneTrash}) {
        for (const auto& drone : *list) {
            next.crewAi.startingCrewCount += drone.crewPhysicalShip == 1;
        }
    }
    if (environment_.stormActive) {
        next.reactorPowerDivide = 2;
        next.reactorPowerCap = (next.ship.reactor + 1) / 2;
    }
    configureCombatAiThresholds(next, events_.shipEvent(profile));
    // As in beginCombat for any ship but the returning flagship: a revisited
    // ship is never the parked boss (BossShip::ClearLocation keeps that one
    // out of the beacon cache), so actors from the boss lose their owner link
    // rather than indexing the new ship's roster.
    for (auto& member : voyageBoarders_) {
        member.enemyCrewOrigin = -1;
    }
    rebaseVoyageCrewReferences(static_cast<int>(enemy_.crew.size()),
                               static_cast<int>(next.crew.size()));
    enemy_ = std::move(next);
    // CreateShip 0x5eee10 rebuilds the saved blueprint through ShipManager::
    // OnInit; initializeCrewDroneAnimation above already numbered the
    // HackingSystem (ctor 0x6080d0) and CreateSpaceDrone 0x4a5db0 drones in
    // that order. As in beginCombat, the native hacking-drone record of a
    // ship without a HackingSystem still needs an identity before AddShip.
    assignSpaceDroneId(enemy_.hackingDrone);
    for (auto& drone : enemy_.drones) {
        assignSpaceDroneId(drone);
    }
    registerSpaceDrones(1, true);
    // A revisited ship also reaches WorldManager::AddShip 0x5ee6b0 through
    // CreateShip, which retargets the new ship's and the player's drones.
    retargetSpaceDrones(1);
    retargetSpaceDrones(0);
    retargetSpaceDrones(0);
    refreshCrewNames();
    encounter_ = saved.at("encounter").get<std::string>();
    encounterAutoBlueprint_ = saved.value("auto_blueprint", std::string{});
    // ClearLocation 0x5ef130 keeps currentShipEvent with the location's ship,
    // so a revisit's CreateShip restores the same seed. Older caches lack it.
    encounterShipSeed_ = saved.value("ship_seed", -1);
    hostile_ = true;
    enemyHazards_.clear();
    enemyExplosionActive_ = false;
    surrenderOffered_ = surrenderDialog_ = false;
}
} // namespace ftl
