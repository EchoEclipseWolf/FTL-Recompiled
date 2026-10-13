#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace ftl {
void Run::checkpoint(nlohmann::json& data, bool loading) {
    if (loading) {
        // Legacy native saves did not distinguish physical location from a
        // selected jump destination. Their unavailable old identity cannot
        // be recovered by sampling RNG or guessing a previously visited node.
        if (!data.contains("locationBeacon_")) {
            data["locationBeacon_"] = data.at("graph_").at("current_");
        }
        // Old saves lack the preceding manager's count/boost snapshots.
        if (!data.contains("playerIntruderCount_")) {
            data["playerIntruderCount_"] = 0;
        }
        if (!data.contains("boostFtl_")) {
            data["boostFtl_"] = !nativeValue<bool>(data.at("hostile_"));
        }
        if (!data.contains("playerCrewAi_")) {
            data["playerCrewAi_"] = CrewAiState{};
        }
        if (!data.contains("damageMessages_")) {
            data["damageMessages_"] = nlohmann::json::array();
        }
        // Older saves derived the lend from the battery state (-1), as the
        // ShipManager::OnLoop that WorldManager::LoadGame runs would set it.
        if (!data.contains("batteryCapacity_")) {
            data["batteryCapacity_"] = std::array<int, 2>{-1, -1};
        }
        // They also reset the resource HUD comparison to the loaded amounts.
        if (!data.contains("resourceFeedback_")) {
            data["resourceFeedback_"] = nlohmann::json::array();
        }
        if (!data.contains("observedResources_")) {
            data["observedResources_"] = nlohmann::json::array(
                {data.at("scrap_"), data.at("fuel_"), data.at("missiles_"), data.at("parts_")});
        }
        if (!data.contains("pendingSavedMissileFeedback_")) {
            data["pendingSavedMissileFeedback_"] = false;
        }
        // Older saves predate the per-ship trackers: use what a freshly built
        // ShipManager (ctor 0x495af0, OnInit 0x4a6e00) holds before its first loop.
        const auto pair = [](const nlohmann::json& value) {
            return nlohmann::json::array({value, value});
        };
        for (const auto& [key, value] :
             std::initializer_list<std::pair<const char*, nlohmann::json>>{
                 {"lastEngineStatus_", false},
                 {"lastJumpReady_", false},
                 {"lastHullDamage_", pair(0)},
                 {"fireOxygen_", pair(nlohmann::json::array())},
                 {"roomLastOxygen_", pair(nlohmann::json::array())},
                 {"fireExtinguishers_", pair(nlohmann::json::array())},
                 {"slugGelHealElapsed_", pair(0.0)},
                 {"doorHitTick_", pair(nlohmann::json::array())},
                 {"systemExplosions_", pair(nlohmann::json::array())},
                 {"storeOpenPending_", false},
                 // MindSystem ctor 0x609b40: an empty queue and no resisted crew.
                 {"mindQueue_", pair(nlohmann::json::array())},
                 {"mindResisted_", nlohmann::json::array()}}) {
            if (!data.contains(key)) {
                data[key] = value;
            }
        }
        const auto legacyAge = [&](const nlohmann::json& object) {
            const auto now = nativeValue<std::uint64_t>(data.at("tick_"));
            const auto born = nativeValue<std::uint64_t>(object.at("tick"));
            if (born > now) {
                throw std::out_of_range("invalid_message_creation_tick");
            }
            return static_cast<double>(now - born) * Time::step;
        };
        for (auto& hit : data.at("shieldHits_")) {
            const double age = legacyAge(hit);
            if (!hit.contains("currentSize")) {
                hit["currentSize"] = std::min(40.0, age * 160);
            }
            if (!hit.contains("currentThickness")) {
                hit["currentThickness"] = std::min(75.0, age * 960);
            }
        }
        for (const char* name : {"resistMessages_", "damageMessages_"}) {
            for (auto& message : data.at(name)) {
                if (!message.contains("elapsed")) {
                    message["elapsed"] = std::min(1.0, legacyAge(message));
                }
                if (!message.contains("shieldOwned")) {
                    message["shieldOwned"] = message.contains("superShield") &&
                                             nativeValue<bool>(message.at("superShield"));
                }
            }
        }
        if (!data.contains("nextDamageMessageOrder_")) {
            // Historical mixed message insertion order was not saved. Retain
            // deterministic old vector traversal without inventing that order.
            std::uint64_t nextOrder = 1;
            for (const char* name : {"resistMessages_", "damageMessages_"}) {
                for (auto& message : data.at(name)) {
                    if (message.contains("messageOrder")) {
                        throw std::out_of_range("incomplete_damage_message_order");
                    }
                    message["messageOrder"] = nextOrder++;
                }
            }
            data["nextDamageMessageOrder_"] = nextOrder;
        }
        for (auto& beacon : data.at("graph_").at("beacons_")) {
            if (beacon.contains("repair")) {
                continue;
            }
            pugi::xml_document event;
            const auto xml = nativeValue<std::string>(beacon.at("eventXml"));
            if (!xml.empty() && !event.load_string(xml.c_str())) {
                throw std::runtime_error("invalid_prepared_beacon_event");
            }
            beacon["repair"] = static_cast<bool>(event.document_element().child("repair"));
        }
        const auto migrateClonePending = [](nlohmann::json& object,
                                            const char* pending,
                                            const char* timer,
                                            const char* duration,
                                            const char* queue) {
            if (object.contains(pending)) {
                return;
            }
            object[pending] = -1;
            if (!object.contains(timer) || !object.contains(duration) || !object.contains(queue)) {
                return;
            }
            if (nativeValue<float>(object.at(timer)) >= nativeValue<float>(object.at(duration)) &&
                !object.at(queue).empty()) {
                object[pending] = nativeValue<int>(object.at(queue).front());
                object[timer] = -1.f;
            }
        };
        migrateClonePending(data, "clonePending_", "cloneTimer_", "cloneDuration_", "cloneQueue_");
        migrateClonePending(
            data.at("enemy_"), "clonePending", "cloneTimer", "cloneDuration", "cloneQueue");
        if (data.at("campaign_").at("savedBoss").is_object()) {
            migrateClonePending(data.at("campaign_").at("savedBoss").at("enemy"),
                                "clonePending",
                                "cloneTimer",
                                "cloneDuration",
                                "cloneQueue");
        }
        if (!data.contains("retiredCrewDrones_")) {
            data["retiredCrewDrones_"] = nlohmann::json::array();
        }
        if (!data.contains("retiredSpaceDrones_")) {
            data["retiredSpaceDrones_"] = nlohmann::json::array();
        }
        const auto migrateEnemyContainers = [](nlohmann::json& enemy) {
            if (!enemy.contains("droneTrash")) {
                enemy["droneTrash"] = nlohmann::json::array();
            }
            if (!enemy.contains("hackingDrone")) {
                enemy["hackingDrone"] = CombatDrone{.blueprint = "DRONE_HACKING"};
            }
        };
        migrateEnemyContainers(data.at("enemy_"));
        if (data.at("campaign_").at("savedBoss").is_object()) {
            migrateEnemyContainers(data.at("campaign_").at("savedBoss").at("enemy"));
        }
        const auto migrateSystemLocks = [](nlohmann::json& owner, bool player) {
            for (auto& sys : owner.at(player ? "systems_" : "systems")) {
                if (sys.contains("lockCount") || sys.contains("lockElapsed")) {
                    continue;
                }
                const double ion = nativeValue<float>(sys.at("ion"));
                const float ionTimer = nativeValue<float>(sys.at("ionTimer"));
                if (!std::isfinite(ion) || ion < 0 || ion > std::numeric_limits<int>::max() ||
                    std::floor(ion) != ion || ionTimer < 0 || ionTimer > 5 ||
                    (ion == 0 ? ionTimer != 0 : ionTimer <= 0)) {
                    throw std::out_of_range("invalid_legacy_system_lock");
                }
                int count = static_cast<int>(ion);
                float elapsed = count > 0 ? 5.f - ionTimer : 0.f;
                const int id = nativeValue<int>(sys.at("definition").at("id"));
                const char* abilityName = id == 10   ? (player ? "cloak_" : "cloak")
                                          : id == 12 ? (player ? "battery_" : "battery")
                                          : id == 14 ? (player ? "mind_" : "mind")
                                          : id == 15 ? (player ? "hacking_" : "hacking")
                                                     : nullptr;
                if (abilityName && owner.contains(abilityName)) {
                    auto& ability = owner.at(abilityName);
                    const double cooldown = nativeValue<float>(ability.at("cooldown"));
                    if (!std::isfinite(cooldown) || cooldown < 0 ||
                        cooldown > static_cast<double>(std::numeric_limits<int>::max()) * 5) {
                        throw std::out_of_range("invalid_legacy_ability_lock");
                    }
                    // Older saves allowed two independent locks. Positive ion
                    // wins; otherwise recover the active sentinel or cooldown.
                    // Their omitted historical shared phase cannot be recovered.
                    if (count == 0) {
                        if (nativeValue<bool>(ability.at("active"))) {
                            count = -1;
                        } else if (cooldown > 0) {
                            count = static_cast<int>(std::ceil(cooldown / 5));
                            elapsed = static_cast<float>(static_cast<double>(count) * 5 - cooldown);
                        }
                    }
                    ability["cooldown"] = count > 0 ? static_cast<double>(count) * 5 - elapsed : 0;
                }
                sys["lockCount"] = count;
                sys["lockElapsed"] = elapsed;
                sys["ion"] = static_cast<float>(std::max(count, 0));
                sys["ionTimer"] = count > 0 ? 5.f - elapsed : 0.f;
            }
        };
        migrateSystemLocks(data, true);
        migrateSystemLocks(data.at("enemy_"), false);
        if (data.at("campaign_").at("savedBoss").is_object()) {
            migrateSystemLocks(data.at("campaign_").at("savedBoss").at("enemy"), false);
        }
        const auto legacyFire = [](const nlohmann::json& ship) {
            std::map<int, std::vector<float>> result;
            if (!ship.at("rooms").is_array() || ship.at("rooms").size() > 256) {
                throw std::runtime_error("invalid_checkpoint_rooms");
            }
            for (const auto& row : ship.at("rooms")) {
                const auto shape = nativeValue<RoomDefinition>(row);
                if (shape.w < 35 || shape.h < 35 || shape.w > 4096 || shape.h > 4096 ||
                    shape.w % 35 != 0 || shape.h % 35 != 0) {
                    throw std::runtime_error("invalid_checkpoint_room_geometry");
                }
                result[shape.id].resize(static_cast<std::size_t>(shape.w / 35) *
                                        static_cast<std::size_t>(shape.h / 35));
            }
            return result;
        };
        if (!data.contains("playerRoomFireAnimationTimes_")) {
            data["playerRoomFireAnimationTimes_"] = legacyFire(data.at("ship_"));
        }
        const auto legacySpreader = [&](const nlohmann::json& ship) {
            std::map<int, std::vector<FireAnimationState>> result;
            for (const auto& [id, phases] : legacyFire(ship)) {
                result[id].resize(phases.size());
            }
            return result;
        };
        if (!data.contains("playerFireSpreaderAnimations_")) {
            data["playerFireSpreaderAnimations_"] = legacySpreader(data.at("ship_"));
        }
        if (!data.contains("armamentWarnings_")) {
            data["armamentWarnings_"] = std::array<CombatHudWarning, 5>{};
        }
        const auto migrateShipAnimations = [&](nlohmann::json& shipState, bool player) {
            const auto& ship = shipState.at(player ? "ship_" : "ship");
            if (!player && !shipState.contains("roomFireAnimationTimes")) {
                shipState["roomFireAnimationTimes"] = legacyFire(ship);
            }
            if (!player && !shipState.contains("fireSpreaderAnimations")) {
                shipState["fireSpreaderAnimations"] = legacySpreader(ship);
            }
            if (!player && !shipState.contains("maxSuperShields")) {
                const auto augments = nativeValue<std::vector<std::string>>(ship.at("augments"));
                const bool shield =
                    std::ranges::find(augments, "ENERGY_SHIELD") != augments.end() ||
                    nativeValue<int>(shipState.at("superShields")) > 0;
                const auto id = ship.at("id").get<std::string>();
                shipState["maxSuperShields"] = id.starts_with("BOSS_3_") ? 12 : shield ? 5 : 0;
            }
            for (auto& sys : shipState.at(player ? "systems_" : "systems")) {
                if (!sys.contains("damagingEffectTimes")) {
                    std::vector<float> phases;
                    if (nativeValue<int>(sys.at("definition").at("id")) == 5) {
                        const int roomId = nativeValue<int>(sys.at("definition").at("room"));
                        for (const auto& row : ship.at("rooms")) {
                            const auto shape = nativeValue<RoomDefinition>(row);
                            if (shape.id == roomId) {
                                phases.resize(static_cast<std::size_t>(shape.w / 35) *
                                              static_cast<std::size_t>(shape.h / 35));
                                break;
                            }
                        }
                    }
                    sys["damagingEffectTimes"] = phases;
                }
            }
        };
        // Earlier native saves omitted these constructor clocks. Deterministic
        // zero phases preserve loading without fabricating historical RNG.
        migrateShipAnimations(data, true);
        migrateShipAnimations(data["enemy_"], false);
        if (data.at("campaign_").at("savedBoss").is_object()) {
            migrateShipAnimations(data["campaign_"]["savedBoss"]["enemy"], false);
        }
    }
    if (loading && !data.contains("playerEngineAnimationTimes_")) {
        data["playerEngineAnimationTimes_"] = std::array<float, 2>{};
    }
    if (loading) {
        const auto legacyFactory = [&](const std::string& id) {
            InventoryFactory factory;
            if (!id.empty()) {
                const auto& blueprint = catalog_.equipment(id);
                if (blueprint.kind == "weapon") {
                    factory.weapon.goalChargeLevel = 1;
                }
                if (blueprint.kind == "drone") {
                    factory.drone.blueprint = id;
                    const bool interior = blueprint.type == "REPAIR" ||
                                          blueprint.type == "BATTLE" || blueprint.type == "BOARDER";
                    if (interior) {
                        factory.drone.shootGoal = 1.f;
                    }
                    if (blueprint.type != "REPAIR" && blueprint.type != "BATTLE") {
                        factory.drone.orbitAngle = 0;
                    }
                }
            }
            // Earlier saves discarded cargo factories. Their historical
            // constructor draws cannot be recreated; migrate without RNG.
            return factory;
        };
        if (!data.contains("cargoFactories_")) {
            data["cargoFactories_"] = nlohmann::json::array();
            for (const auto& id : data.at("cargo_")) {
                data["cargoFactories_"].push_back(legacyFactory(id.get<std::string>()));
            }
        }
        if (!data.contains("overflowFactory_")) {
            data["overflowFactory_"] =
                legacyFactory(data.value("overflowEquipment_", std::string{}));
        }
    }
    if (loading && !data.contains("nextCrewDroneId_")) {
        // Historical checkpoints addressed bodies by installed equipment index.
        // Assign deterministic identities without replaying constructor RNG.
        std::uint64_t nextId = 1;
        const auto identify = [&](nlohmann::json& drone) {
            const auto name = drone.value("blueprint", std::string{});
            if (name.empty()) {
                drone["crewActorId"] = 0;
                return;
            }
            const auto& type = catalog_.equipment(name).type;
            drone["crewActorId"] =
                type == "REPAIR" || type == "BATTLE" || type == "BOARDER" ? nextId++ : 0;
        };
        for (auto& drone : data.at("combatDrones_")) {
            identify(drone);
        }
        for (auto& factory : data.at("cargoFactories_")) {
            identify(factory.at("drone"));
        }
        identify(data.at("overflowFactory_").at("drone"));
        for (auto& drone : data.at("enemy_").at("drones")) {
            identify(drone);
        }
        for (auto& drone : data.at("enemy_").at("droneTrash")) {
            identify(drone);
        }
        for (auto& drone : data.at("retiredCrewDrones_")) {
            identify(drone);
        }
        for (auto& drone : data.at("retiredSpaceDrones_")) {
            identify(drone);
        }
        const auto migrateTargets = [&](nlohmann::json& actors, const nlohmann::json& enemyDrones) {
            for (auto& actor : actors) {
                actor["attackTargetDroneId"] = 0;
                if (!actor.value("attackTargetDrone", false)) {
                    continue;
                }
                const int owner = nativeValue<int>(actor.at("attackTargetOwner"));
                const int index = nativeValue<int>(actor.at("attackTargetIndex"));
                if (owner < 0 || owner > 1) {
                    throw std::runtime_error("invalid_checkpoint_drone_target");
                }
                const auto& drones = owner == 0 ? data.at("combatDrones_") : enemyDrones;
                if (index < 0 || static_cast<std::size_t>(index) >= drones.size()) {
                    throw std::runtime_error("invalid_checkpoint_drone_target");
                }
                actor["attackTargetDroneId"] = drones.at(index).at("crewActorId");
                actor["attackTargetIndex"] = -1;
            }
        };
        const auto& enemyDrones = data.at("enemy_").at("drones");
        for (auto* actors : {&data.at("crew_"),
                             &data.at("voyageBoarders_"),
                             &data.at("combatDrones_"),
                             &data.at("enemy_").at("crew"),
                             &data.at("enemy_").at("drones"),
                             &data.at("enemy_").at("droneTrash"),
                             &data.at("retiredCrewDrones_"),
                             &data.at("retiredSpaceDrones_")}) {
            migrateTargets(*actors, enemyDrones);
        }
        for (auto& factory : data.at("cargoFactories_")) {
            auto actors = nlohmann::json::array({factory.at("drone")});
            migrateTargets(actors, enemyDrones);
            factory["drone"] = std::move(actors.front());
        }
        auto overflow = nlohmann::json::array({data.at("overflowFactory_").at("drone")});
        migrateTargets(overflow, enemyDrones);
        data.at("overflowFactory_")["drone"] = std::move(overflow.front());
        auto& savedBoss = data.at("campaign_").at("savedBoss");
        if (savedBoss.is_object()) {
            auto& boss = savedBoss.at("enemy");
            for (auto& drone : boss.at("drones")) {
                identify(drone);
            }
            for (auto& drone : boss.at("droneTrash")) {
                identify(drone);
            }
            migrateTargets(boss.at("crew"), boss.at("drones"));
            migrateTargets(boss.at("drones"), boss.at("drones"));
            migrateTargets(boss.at("droneTrash"), boss.at("drones"));
        }
        data["nextCrewDroneId_"] = nextId;
    }
    if (loading && !data.contains("nextPhysicalCrewOrder_")) {
        // Old native saves did not retain physical insertion history. Preserve
        // their deterministic traversal without asserting original birth order.
        std::uint64_t nextOrder = 1;
        for (auto& member : data.at("crew_")) {
            member["physicalCrewOrder"] = nextOrder++;
        }
        for (auto& member : data.at("enemy_").at("crew")) {
            member["physicalCrewOrder"] = nextOrder++;
        }
        for (auto& member : data.at("voyageBoarders_")) {
            const int origin = member.value("enemyCrewOrigin", -1);
            if (origin >= 0 &&
                static_cast<std::size_t>(origin) < data.at("enemy_").at("crew").size()) {
                member["physicalCrewOrder"] =
                    data.at("enemy_").at("crew").at(origin).at("physicalCrewOrder");
            } else {
                member["physicalCrewOrder"] = nextOrder++;
            }
        }
        const auto migrateDroneOrder = [&](nlohmann::json& drone) {
            const auto name = drone.value("blueprint", std::string{});
            drone["physicalCrewOrder"] = 0;
            if (name.empty()) {
                return;
            }
            const auto& type = catalog_.equipment(name).type;
            if (type == "REPAIR" || type == "BATTLE" ||
                (type == "BOARDER" && drone.value("delivered", false))) {
                drone["physicalCrewOrder"] = nextOrder++;
            }
        };
        for (auto& drone : data.at("combatDrones_")) {
            migrateDroneOrder(drone);
        }
        for (auto& drone : data.at("enemy_").at("drones")) {
            migrateDroneOrder(drone);
        }
        for (auto& drone : data.at("enemy_").at("droneTrash")) {
            migrateDroneOrder(drone);
        }
        for (auto& drone : data.at("retiredCrewDrones_")) {
            migrateDroneOrder(drone);
        }
        for (auto& drone : data.at("retiredSpaceDrones_")) {
            migrateDroneOrder(drone);
        }
        for (auto& factory : data.at("cargoFactories_")) {
            migrateDroneOrder(factory.at("drone"));
        }
        migrateDroneOrder(data.at("overflowFactory_").at("drone"));
        auto& savedBoss = data.at("campaign_").at("savedBoss");
        if (savedBoss.is_object()) {
            for (auto& member : savedBoss.at("enemy").at("crew")) {
                member["physicalCrewOrder"] = nextOrder++;
            }
            for (auto& drone : savedBoss.at("enemy").at("drones")) {
                migrateDroneOrder(drone);
            }
            for (auto& drone : savedBoss.at("enemy").at("droneTrash")) {
                migrateDroneOrder(drone);
            }
        }
        data["nextPhysicalCrewOrder_"] = nextOrder;
    }
    if (loading && !data.contains("nextSpaceDroneId_")) {
        // Old checkpoints retained neither the space registry nor historical
        // arrivals. Preserve available actors without manufacturing RNG.
        std::uint64_t nextId = 1;
        std::uint64_t nextOrder = 1;
        const auto migrateSpaceDrone = [&](nlohmann::json& drone, bool registered) {
            drone["spaceActorId"] = 0;
            drone["spaceRegisteredOrder"] = 0;
            drone["spacePendingArrival"] = false;
            const auto name = drone.value("blueprint", std::string{});
            if (name.empty()) {
                return;
            }
            const auto& type = catalog_.equipment(name).type;
            if (type != "DEFENSE" && type != "COMBAT" && type != "BOARDER" &&
                type != "SHIP_REPAIR" && type != "SHIELD" && type != "HACKING") {
                return;
            }
            drone["spaceActorId"] = nextId++;
            if (registered) {
                drone["spaceRegisteredOrder"] = nextOrder++;
            }
        };
        migrateSpaceDrone(data.at("hackingDrone_"), true);
        for (auto& drone : data.at("combatDrones_")) {
            migrateSpaceDrone(drone, true);
        }
        for (auto& factory : data.at("cargoFactories_")) {
            migrateSpaceDrone(factory.at("drone"), true);
        }
        migrateSpaceDrone(data.at("overflowFactory_").at("drone"), true);
        for (auto& drone : data.at("retiredCrewDrones_")) {
            migrateSpaceDrone(drone, true);
        }
        for (auto& drone : data.at("retiredSpaceDrones_")) {
            migrateSpaceDrone(drone, true);
        }
        migrateSpaceDrone(data.at("enemy_").at("hackingDrone"), true);
        for (auto& drone : data.at("enemy_").at("drones")) {
            migrateSpaceDrone(drone, true);
        }
        for (auto& drone : data.at("enemy_").at("droneTrash")) {
            migrateSpaceDrone(drone, true);
        }
        auto& savedBoss = data.at("campaign_").at("savedBoss");
        if (savedBoss.is_object()) {
            migrateSpaceDrone(savedBoss.at("enemy").at("hackingDrone"), false);
            for (auto& drone : savedBoss.at("enemy").at("drones")) {
                migrateSpaceDrone(drone, false);
            }
            for (auto& drone : savedBoss.at("enemy").at("droneTrash")) {
                migrateSpaceDrone(drone, false);
            }
        }
        data["nextSpaceDroneId_"] = nextId;
        data["nextSpaceDroneOrder_"] = nextOrder;
    }
    if (loading && !data.contains("spaceShipOwners_")) {
        // Historical checkpoints did not retain AddShip order. Preserve the
        // available ships without claiming to recover their original order.
        data["spaceShipOwners_"] = nlohmann::json::array({0});
        if (data.at("enemy_").value("present", false)) {
            data["spaceShipOwners_"].push_back(1);
        }
    }
    if (loading) {
        const auto migrateCrewBody = [&](nlohmann::json& drone,
                                         int owner,
                                         const nlohmann::json& foreignShip) {
            if (drone.contains("crewPhysicalShip")) {
                return;
            }
            drone["crewPhysicalShip"] = -1;
            drone["crewRoom"] = -1;
            drone["crewPosition"] = SDL_FPoint{};
            drone["crewSlot"] = -1;
            const auto name = drone.value("blueprint", std::string{});
            if (name.empty()) {
                return;
            }
            const auto& type = catalog_.equipment(name).type;
            if (type != "REPAIR" && type != "BATTLE" && type != "BOARDER") {
                return;
            }
            const int roomId = drone.value("room", -1);
            if (roomId < 0 || (type == "BOARDER" && !drone.value("delivered", false))) {
                return;
            }
            const int physicalShip = type == "BOARDER" ? 1 - owner : owner;
            const auto& layout = physicalShip == 0 ? data.at("ship_") : foreignShip;
            const auto point = nativeValue<SDL_FPoint>(drone.at("position"));
            for (const auto& row : layout.at("rooms")) {
                const auto shape = nativeValue<RoomDefinition>(row);
                if (shape.id != roomId) {
                    continue;
                }
                drone["crewPhysicalShip"] = physicalShip;
                drone["crewRoom"] = roomId;
                drone["crewPosition"] = point;
                const int column = static_cast<int>((point.x - static_cast<float>(shape.x)) / 35.f);
                const int line = static_cast<int>((point.y - static_cast<float>(shape.y)) / 35.f);
                if (column >= 0 && column < shape.w / 35 && line >= 0 && line < shape.h / 35) {
                    drone["crewSlot"] = line * (shape.w / 35) + column;
                }
                return;
            }
        };
        const auto& foreignShip = data.at("enemy_").at("ship");
        for (auto& drone : data.at("combatDrones_")) {
            migrateCrewBody(drone, 0, foreignShip);
        }
        for (auto& factory : data.at("cargoFactories_")) {
            migrateCrewBody(factory.at("drone"), 0, foreignShip);
        }
        migrateCrewBody(data.at("overflowFactory_").at("drone"), 0, foreignShip);
        for (auto& drone : data.at("retiredCrewDrones_")) {
            migrateCrewBody(drone, 0, foreignShip);
        }
        for (auto& drone : data.at("enemy_").at("drones")) {
            migrateCrewBody(drone, 1, foreignShip);
        }
        for (auto& drone : data.at("enemy_").at("droneTrash")) {
            migrateCrewBody(drone, 1, foreignShip);
        }
        auto& savedBoss = data.at("campaign_").at("savedBoss");
        if (savedBoss.is_object()) {
            for (auto& drone : savedBoss.at("enemy").at("drones")) {
                migrateCrewBody(drone, 1, savedBoss.at("enemy").at("ship"));
            }
            for (auto& drone : savedBoss.at("enemy").at("droneTrash")) {
                migrateCrewBody(drone, 1, savedBoss.at("enemy").at("ship"));
            }
        }
    }
    if (loading && !data.contains("fuelEventSeed_")) {
        data["fuelEventSeed_"] = 0;
    }
    // The sector-tree seed was not kept before original-save export; it cannot be
    // recovered from the generated tree. Starting crew came from the ship blueprint.
    if (loading && !data.contains("sectorMapSeed_")) {
        data["sectorMapSeed_"] = -1;
    }
    // Original-save state added 7 October 2026; older checkpoints hold what a
    // fresh StarMap/WorldManager holds (no wait, quest list or dialog history).
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, nlohmann::json>>{
             {"waitedLast_", false},
             {"lastEscapeEvent_", ""},
             {"mapRevealed_", false},
             {"addedQuests_", nlohmann::json::array()},
             {"generatedEvent_", ""},
             {"choiceHistory_", nlohmann::json::array()},
             {"lastMainTextLiteral_", true},
             {"lastMainText_", ""},
             {"encounterAutoBlueprint_", ""}}) {
        if (loading && !data.contains(key)) {
            data[key] = value;
        }
    }
    if (loading && !data.contains("scoreCrew_")) {
        int starting = 0;
        for (const auto& group : data.at("ship_").at("crew")) {
            starting += nativeValue<int>(group.at("count"));
        }
        int hired = 0;
        if (data.contains("progression_run") &&
            data.at("progression_run").at("flags").contains("crew_hired")) {
            hired = nativeValue<int>(data.at("progression_run").at("flags").at("crew_hired"));
        }
        data["scoreCrew_"] = starting + hired;
    }
    if (loading && !data.at("environment_").contains("fireActive")) {
        auto& environment = data.at("environment_");
        const auto hazard = data.value("hazard_", std::string{});
        const bool pulsar = hazard == "pulsar";
        const bool pds = hazard == "pds";
        environment["fireActive"] = hazard == "sun";
        environment["pulsarActive"] = pulsar;
        environment["pdsActive"] = hazard == "pds";
        environment["asteroidActive"] = hazard == "asteroid";
        environment["stormActive"] = hazard == "ion_storm";
        environment["flashMinTime"] = pulsar ? 11000 : pds ? 20000 : 28000;
        environment["flashMaxTime"] = pulsar ? 18000 : pds ? 25000 : 34000;
        environment["flashTime"] =
            pds ? environment.value("pdsTime", 0.f) : environment.value("flareTicks", 0) / 60.f;
        environment["flashGoal"] =
            pds ? environment.value("pdsGoal", 0.f) : environment.value("flareGoalTicks", 0) / 60.f;
        const int elapsed = pulsar ? environment.value("pulsarFlashTicks", 60)
                                   : 60 - environment.value("flashTicks", 0);
        environment["flashAnimationTime"] = elapsed < 60 ? elapsed / 60.f : -1.f;
        environment["foregroundPulsar"] = pulsar;
    }
    if (loading) {
        const auto legacySpaceId = [](const nlohmann::json& drones, std::uint64_t slot) {
            return slot > 0 && slot <= drones.size() ? drones.at(static_cast<std::size_t>(slot - 1))
                                                           .value("spaceActorId", std::uint64_t{})
                                                     : std::uint64_t{};
        };
        const auto migrateDefense = [&](nlohmann::json& drone, const nlohmann::json& opposing) {
            if (!drone.contains("targetSpaceDroneId")) {
                drone["targetSpaceDroneId"] =
                    legacySpaceId(opposing, drone.value("targetDrone", std::uint64_t{}));
                drone["targetDrone"] = 0;
            }
            if (!drone.contains("defenseCurrentTargetId")) {
                const auto targetDrone = drone.value("targetSpaceDroneId", std::uint64_t{});
                drone["defenseCurrentTargetIsDrone"] = targetDrone != 0;
                drone["defenseCurrentTargetId"] =
                    targetDrone != 0 ? targetDrone
                                     : drone.value("targetProjectile", std::uint64_t{});
                drone["defenseShotTargetId"] = 0;
                drone["defenseShotTargetIsDrone"] = false;
            }
        };
        const auto& enemyDrones = data.at("enemy_").at("drones");
        const auto& playerDrones = data.at("combatDrones_");
        for (auto* rows : {&data.at("combatDrones_"),
                           &data.at("retiredCrewDrones_"),
                           &data.at("retiredSpaceDrones_")}) {
            for (auto& drone : *rows) {
                migrateDefense(drone, enemyDrones);
            }
        }
        for (auto& factory : data.at("cargoFactories_")) {
            migrateDefense(factory.at("drone"), enemyDrones);
        }
        migrateDefense(data.at("overflowFactory_").at("drone"), enemyDrones);
        migrateDefense(data.at("hackingDrone_"), enemyDrones);
        for (auto* rows : {&data.at("enemy_").at("drones"), &data.at("enemy_").at("droneTrash")}) {
            for (auto& drone : *rows) {
                migrateDefense(drone, playerDrones);
            }
        }
        migrateDefense(data.at("enemy_").at("hackingDrone"), playerDrones);
        auto& savedBoss = data.at("campaign_").at("savedBoss");
        if (savedBoss.is_object()) {
            for (auto* rows :
                 {&savedBoss.at("enemy").at("drones"), &savedBoss.at("enemy").at("droneTrash")}) {
                for (auto& drone : *rows) {
                    migrateDefense(drone, playerDrones);
                }
            }
            migrateDefense(savedBoss.at("enemy").at("hackingDrone"), playerDrones);
        }
        for (auto& projectile : data.at("projectiles_")) {
            if (projectile.contains("defenseSpaceDroneId")) {
                continue;
            }
            const auto owner = projectile.value("owner", -1);
            const auto slot = projectile.value("defenseDroneTargetId", std::uint64_t{});
            projectile["defenseSpaceDroneId"] =
                owner < 0 ? std::uint64_t{}
                          : legacySpaceId(owner == 0 ? enemyDrones : playerDrones, slot);
            projectile["defenseDroneTargetId"] = 0;
        }
    }
    if (loading) {
        std::uint64_t deathCount = 0;
        const auto migrateCorpses = [&](nlohmann::json& actors, const nlohmann::json& queue) {
            for (const auto& actor : actors) {
                if (actor.contains("deathNumber")) {
                    deathCount =
                        std::max(deathCount, nativeValue<std::uint64_t>(actor.at("deathNumber")));
                }
            }
            for (const auto& value : queue) {
                const int index = nativeValue<int>(value);
                if (index < 0 || static_cast<std::size_t>(index) >= actors.size()) {
                    throw std::runtime_error("invalid_checkpoint_clone_queue");
                }
                auto& actor = actors.at(static_cast<std::size_t>(index));
                if (!actor.contains("cloneReady")) {
                    actor["cloneReady"] = true;
                }
            }
        };
        migrateCorpses(data.at("crew_"), data.at("cloneQueue_"));
        const auto enemyQueue = data.at("enemy_").contains("cloneQueue")
                                    ? data.at("enemy_").at("cloneQueue")
                                    : nlohmann::json::array();
        migrateCorpses(data.at("enemy_").at("crew"), enemyQueue);
        if (data.contains("campaign_") && data.at("campaign_").contains("savedBoss") &&
            data.at("campaign_").at("savedBoss").is_object()) {
            auto& boss = data["campaign_"]["savedBoss"]["enemy"];
            const auto bossQueue =
                boss.contains("cloneQueue") ? boss.at("cloneQueue") : nlohmann::json::array();
            migrateCorpses(boss.at("crew"), bossQueue);
        }
        for (const auto& actor : data.at("voyageBoarders_")) {
            if (actor.contains("deathNumber")) {
                deathCount =
                    std::max(deathCount, nativeValue<std::uint64_t>(actor.at("deathNumber")));
            }
        }
        if (!data.contains("crewDeathCount_")) {
            data["crewDeathCount_"] = deathCount;
        }
    }
    // Earlier native saves also kept beamSide_. The original's side is a
    // function-static of BeamWeapon::BeamWeapon 0x4357c0 that no save holds,
    // so loading ignores the key and keeps the live process value.
    if (loading && !data.contains("encounterShipSeed_")) {
        // Earlier native saves did not keep currentShipEvent.shipSeed; their
        // outcome events keep drawing from the live stream.
        data["encounterShipSeed_"] = -1;
    }
    if (loading && !data.contains("sessionRandom_")) {
        data["sessionRandom_"] = data.at("eventRandom_");
    }
    if (loading && !data.contains("hudWarnings_")) {
        data["hudWarnings_"] = std::array<HudWarning, 4>{};
        data["hudLastHull_"] = data.at("hull_");
        data["hullWarningLevel_"] = 75;
    }
    for (const auto* key :
         {"ftlBlink_", "previousFtlBlink_", "pauseBlink_", "previousPauseBlink_"}) {
        if (loading && !data.contains(key)) {
            data[key] = 0.f;
        }
    }
    for (const auto* key : {"ftlBlinkDirection_", "pauseBlinkDirection_"}) {
        if (loading && !data.contains(key)) {
            data[key] = 1.f;
        }
    }
    for (const auto* key : {"ftlPullout_", "previousFtlPullout_"}) {
        if (loading && !data.contains(key)) {
            data[key] = 0.f;
        }
    }
    if (loading && !data.contains("oxygenLeakSound_")) {
        data["oxygenLeakSound_"] = false;
    }
    for (const auto* key : {"voyageDoorLevel_", "voyageDoorLastBaseHealth_"}) {
        if (loading && !data.contains(key)) {
            data[key] = std::map<int, int>{};
        }
    }
    if (loading && !data.contains("voyageDoorLockdownBroken_")) {
        data["voyageDoorLockdownBroken_"] = std::set<int>{};
    }
    if (!loading) {
        crewNameHistory_ = catalog_.currentCrewNames();
    } else if (!data.contains("crewNameHistory_")) {
        data["crewNameHistory_"] = nlohmann::json::array();
        for (std::size_t index = 0; index < data.at("crew_").size(); ++index) {
            const auto& member = data.at("crew_").at(index);
            const auto clones = nativeValue<std::vector<int>>(data.at("cloneQueue_"));
            if (nativeValue<float>(member.at("health")) > 0 ||
                std::ranges::find(clones, static_cast<int>(index)) != clones.end()) {
                data["crewNameHistory_"].push_back(member.at("appearance").at("fullName"));
            }
        }
    }
#define FTL_RUN_FIELD(member)                                                                      \
    if (loading) {                                                                                 \
        nativeRead(data.at(#member), member);                                                      \
    } else {                                                                                       \
        data[#member] = member;                                                                    \
    }
#include "run_checkpoint_fields.hpp"
#undef FTL_RUN_FIELD
    if (!loading) {
        data["voyage_melee_version"] = 2;
    }
    if (loading) {
        profileRunStarted_ = data.contains("profile_run_started")
                                 ? nativeValue<bool>(data.at("profile_run_started"))
                                 : false;
        profileRunId_ = data.contains("profile_run_id")
                            ? nativeValue<std::uint64_t>(data.at("profile_run_id"))
                            : 0;
        if (profileRunId_ > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("invalid_native_profile_run_id");
        }
        if (data.contains("progression_run")) {
            progressionRun_ = ProgressionRun::read(data.at("progression_run"));
            if (progressionRun_.ship != ship_.id || progressionRun_.difficulty != difficulty_ ||
                progressionRun_.advanced != advanced_ || progressionRun_.tutorial) {
                throw std::runtime_error("invalid_native_progression_checkpoint");
            }
        } else {
            progressionRun_ = {ship_.id, difficulty_, false, advanced_, {}};
            progressionRun_.flag("challenge_history_unavailable");
            // Legacy checkpoints cannot prove the absence of earlier actions.
            for (const auto* flag : {"repair",
                                     "store_repair",
                                     "system_upgrade",
                                     "teleported",
                                     "offensive_drone",
                                     "fired_shot",
                                     "reactor_upgrade",
                                     "weapon_upgrade",
                                     "higho2",
                                     "used_missile",
                                     "used_drone",
                                     "store_purchase",
                                     "lost_crew",
                                     "env_danger"}) {
                progressionRun_.flag(flag);
            }
        }
    } else {
        data["progression_run"] = progressionRun_.json();
        data["profile_run_id"] = profileRunId_;
        data["profile_run_started"] = profileRunStarted_;
    }
    if (loading) {
        choices_.clear();
        restoredEvents_.clear();
        for (const auto& row : data.at("choices")) {
            const auto xml = row.at("event").get<std::string>();
            pugi::xml_node event;
            if (!xml.empty()) {
                restoredEvents_.emplace_back();
                if (!restoredEvents_.back().load_string(xml.c_str())) {
                    throw std::runtime_error("invalid_checkpoint_event");
                }
                event = restoredEvents_.back().document_element();
            }
            choices_.push_back(
                {row.at("text").get<std::string>(),
                 event,
                 row.at("enabled").get<bool>(),
                 row.contains("race_choice") ? nativeValue<bool>(row.at("race_choice")) : false,
                 row.contains("blue_choice") ? nativeValue<bool>(row.at("blue_choice")) : false,
                 row.contains("hidden_reward") ? nativeValue<bool>(row.at("hidden_reward")) : false,
                 row.contains("authored_index") ? nativeValue<int>(row.at("authored_index")) : -1});
        }
    } else {
        data["choices"] = nlohmann::json::array();
        for (const auto& choice : choices_) {
            std::ostringstream xml;
            if (choice.event) {
                choice.event.print(xml);
            }
            data["choices"].push_back({{"text", choice.text},
                                       {"event", xml.str()},
                                       {"enabled", choice.enabled},
                                       {"race_choice", choice.raceChoice},
                                       {"blue_choice", choice.blueChoice},
                                       {"hidden_reward", choice.hiddenReward},
                                       {"authored_index", choice.authoredIndex}});
        }
    }
}

void Run::validateCheckpoint() const {
    const auto require = [](bool valid,
                            std::source_location where = std::source_location::current()) {
        if (!valid) {
            throw std::runtime_error("invalid_native_checkpoint:" + std::to_string(where.line()));
        }
    };
    require(encounterShipSeed_ >= -1);
    require(open_ && !defeated_ && sector_ >= 1 && sector_ <= 8 && difficulty_ >= 0 &&
            difficulty_ <= 2);
    require(!campaign_.victory && !campaign_.recorded && campaign_.stage >= 0 &&
            campaign_.stage <= 3 && campaign_.nextStage >= 1 && campaign_.nextStage <= 3 &&
            campaign_.baseTurns >= 0 && campaign_.baseTurns <= 4 &&
            std::isfinite(campaign_.surgeElapsed) && campaign_.surgeElapsed >= 0 &&
            std::isfinite(campaign_.surgeGoal) && campaign_.surgeGoal >= 0 &&
            campaign_.surgeGoal <= 30 && campaign_.surgeCount >= 0);
    require(campaign_.active || nlohmann::json(campaign_) == nlohmann::json(Campaign{}));
    const auto bossFamily =
        "BOSS_1" +
        std::array<std::string, 3>{"_EASY", "_NORMAL", "_HARD"}[std::size_t(difficulty_)] +
        (advanced_ ? "_DLC" : "");
    require(campaign_.crewCounts.size() <= catalog_.ship(bossFamily).rooms.size());
    if (campaign_.active) {
        require(sectorName_ == "FINAL" && campaign_.base >= 0 &&
                campaign_.base < int(graph_.beacons().size()) && !campaign_.path.empty() &&
                campaign_.path.size() <= graph_.beacons().size() && campaign_.position >= 0 &&
                campaign_.position < int(campaign_.path.size()));
        std::unordered_set<int> route;
        for (int beacon : campaign_.path) {
            require(beacon >= 0 && beacon < int(graph_.beacons().size()) &&
                    route.insert(beacon).second);
        }
        for (std::size_t i = 1; i < campaign_.path.size(); ++i) {
            const auto& links = graph_.beacons()[campaign_.path[i - 1]].neighbors;
            require(std::ranges::find(links, campaign_.path[i]) != links.end());
        }
        require(!campaign_.fighting ||
                (enemy_.present && hostile_ && campaign_.stage >= 1 &&
                 enemy_.ship.id == "BOSS_" + std::to_string(campaign_.stage) +
                                       std::array<std::string, 3>{
                                           "_EASY", "_NORMAL", "_HARD"}[std::size_t(difficulty_)] +
                                       (advanced_ ? "_DLC" : "")));
        require(campaign_.path.front() == campaign_.base ||
                campaign_.path.back() == campaign_.base);
        require(campaign_.changing.size() <= 2);
        for (int beacon : campaign_.changing) {
            require(beacon >= 0 && beacon < int(graph_.beacons().size()) &&
                    beacon != campaign_.base);
        }
    }
    for (int count : campaign_.crewCounts) {
        require(count >= 0 && count <= 64);
    }
    require(hull_ > 0 && hull_ <= ship_.hull && scrap_ >= 0 && fuel_ >= 0 && missiles_ >= 0 &&
            parts_ >= 0);
    require(devReactorLimit_ >= 1 && devReactorLimit_ <= 64 && devCrewLimit_ >= 1 &&
            devCrewLimit_ <= 32 &&
            (hazard_.empty() || hazard_ == "nebula" || hazard_ == "ion_storm" || hazard_ == "sun" ||
             hazard_ == "asteroid" || hazard_ == "pulsar" || hazard_ == "pds"));
    require(ship_.reactor >= 0 && ship_.reactor <= devReactorLimit_ && cargo_.size() <= 4 &&
            ship_.augments.size() <= 3);
    const auto& canonical = catalog_.ship(ship_.id);
    require(ship_.rooms.size() == canonical.rooms.size() &&
            ship_.doors.size() == canonical.doors.size());
    for (std::size_t i = 0; i < ship_.rooms.size(); ++i) {
        require(nlohmann::json(ship_.rooms[i]) == nlohmann::json(canonical.rooms[i]));
    }
    for (std::size_t i = 0; i < ship_.doors.size(); ++i) {
        require(nlohmann::json(ship_.doors[i]) == nlohmann::json(canonical.doors[i]));
    }
    require(doors_.size() == ship_.doors.size() && fakeDoors_.size() == doors_.size() &&
            doorFrames_.size() == doors_.size() && doorTimers_.size() == doors_.size());
    require(combatWeapons_.size() == ship_.weapons.size() &&
            weaponPower_.size() == ship_.weapons.size() &&
            weaponDesired_.size() == ship_.weapons.size() &&
            combatDrones_.size() == ship_.drones.size());
    const auto validateShipAnimations = [&](const ShipDefinition& ship,
                                            const auto& engines,
                                            const auto& fires,
                                            const auto& systems) {
        for (float elapsed : engines) {
            require(std::isfinite(elapsed) && elapsed >= 0 && elapsed <= .5f);
        }
        require(fires.size() == ship.rooms.size());
        for (const auto& shape : ship.rooms) {
            const auto phases = fires.find(shape.id);
            require(phases != fires.end() &&
                    phases->second.size() == static_cast<std::size_t>(shape.w / 35) *
                                                 static_cast<std::size_t>(shape.h / 35));
            for (float elapsed : phases->second) {
                require(std::isfinite(elapsed) && elapsed >= 0 &&
                        elapsed <= catalog_.animation("fire_small").duration);
            }
        }
        for (const auto& sys : systems) {
            if (sys.definition.id != 5) {
                require(sys.damagingEffectTimes.empty());
                continue;
            }
            const auto shape = std::ranges::find_if(ship.rooms, [&](const auto& candidate) {
                return candidate.id == sys.definition.room;
            });
            require(shape != ship.rooms.end() &&
                    sys.damagingEffectTimes.size() == static_cast<std::size_t>(shape->w / 35) *
                                                          static_cast<std::size_t>(shape->h / 35));
            for (float elapsed : sys.damagingEffectTimes) {
                require(std::isfinite(elapsed) && elapsed >= 0 && elapsed <= 1);
            }
        }
    };
    validateShipAnimations(
        ship_, playerEngineAnimationTimes_, playerRoomFireAnimationTimes_, systems_);
    validateShipAnimations(
        enemy_.ship, enemy_.engineAnimationTimes, enemy_.roomFireAnimationTimes, enemy_.systems);
    const auto validateFireAnimations = [&](const ShipDefinition& ship, const auto& fires) {
        require(fires.size() == ship.rooms.size());
        for (const auto& shape : ship.rooms) {
            const auto phases = fires.find(shape.id);
            require(phases != fires.end() &&
                    phases->second.size() == static_cast<std::size_t>(shape.w / 35) *
                                                 static_cast<std::size_t>(shape.h / 35));
            for (const auto& phase : phases->second) {
                require(std::isfinite(phase.elapsed) && phase.elapsed >= 0 && phase.elapsed <= 1.f);
            }
        }
    };
    validateFireAnimations(ship_, playerFireSpreaderAnimations_);
    validateFireAnimations(enemy_.ship, enemy_.fireSpreaderAnimations);
    for (const auto& warning : armamentWarnings_) {
        require(warning.text.size() <= 4096 && std::isfinite(warning.elapsed) &&
                (warning.elapsed == -1 || (warning.elapsed >= 0 && warning.elapsed <= 2)));
    }
    require((enemy_.maxSuperShields == 0 || enemy_.maxSuperShields == 5 ||
             enemy_.maxSuperShields == 12) &&
            enemy_.superShields >= 0 && enemy_.superShields <= enemy_.maxSuperShields &&
            std::isfinite(enemy_.escapeWarningTime) && enemy_.escapeWarningTime >= 0 &&
            enemy_.escapeWarningTime <= .66f);
    require(ship_.hull >= 1 && ship_.hull <= 999 && ship_.weaponSlots >= 1 &&
            ship_.weaponSlots <= 8 && ship_.droneSlots >= 1 && ship_.droneSlots <= 8);
    require(ship_.weapons.size() <= std::size_t(ship_.weaponSlots) + 1 &&
            ship_.drones.size() <= std::size_t(ship_.droneSlots));
    for (const auto& id : ship_.weapons) {
        require(catalog_.equipment(id).kind == "weapon");
    }
    for (const auto& id : ship_.drones) {
        require(catalog_.equipment(id).kind == "drone");
    }
    for (const auto& id : ship_.augments) {
        require(catalog_.equipment(id).kind == "aug");
    }
    for (const auto& id : cargo_) {
        (void)catalog_.equipment(id);
    }
    require(crew_.size() <= 64 && systems_.size() <= 32 && projectiles_.size() <= 4096 &&
            choices_.size() <= 64);
    const auto validateBoardPosition = [&](const RunCrew& member) {
        require(std::isfinite(member.boardX) && std::isfinite(member.boardY) &&
                member.boardSlot >= 0 && member.boardTargetSlot >= 0 &&
                member.boardPath.size() <= 4096);
        if (member.boardPositionValid) {
            require(member.intruder && enemy_.present);
            const auto shape = std::ranges::find_if(enemy_.ship.rooms, [&](const auto& candidate) {
                return candidate.id == member.boardRoom;
            });
            require(shape != enemy_.ship.rooms.end());
            require(member.boardSlot < shape->w / 35 * (shape->h / 35));
            if (member.boardTargetRoom >= 0) {
                const auto target =
                    std::ranges::find_if(enemy_.ship.rooms, [&](const auto& candidate) {
                        return candidate.id == member.boardTargetRoom;
                    });
                require(target != enemy_.ship.rooms.end());
                require(member.boardTargetSlot < target->w / 35 * (target->h / 35));
            }
        } else {
            require(member.boardPath.empty());
        }
        for (const auto& waypoint : member.boardPath) {
            require(waypoint.door >= -1 &&
                    waypoint.door < static_cast<int>(enemy_.ship.doors.size()) &&
                    std::isfinite(waypoint.point.x) && std::isfinite(waypoint.point.y));
        }
    };
    for (const auto& crew : crew_) {
        validateBoardPosition(crew);
        require(crew.lastDoor >= -1 && crew.lastDoor < static_cast<int>(ship_.doors.size()) &&
                crew.blockingDoor >= -1 &&
                crew.blockingDoor < static_cast<int>(ship_.doors.size()) &&
                crew.boardBlockingDoor >= -1 &&
                crew.boardBlockingDoor < static_cast<int>(enemy_.ship.doors.size()) &&
                crew.boardLastDoor >= -1 &&
                crew.boardLastDoor < static_cast<int>(enemy_.ship.doors.size()));
        require(crew.enemyCrewOrigin == -1);
        const auto& shape = room(crew.room);
        require(!crew.intruder ||
                (enemy_.present &&
                 std::ranges::any_of(enemy_.ship.rooms, [&](const RoomDefinition& candidate) {
                     return candidate.id == crew.boardRoom;
                 })));
        require(crew.slot >= 0 && crew.slot < shape.w / 35 * (shape.h / 35) &&
                std::isfinite(crew.health) && crew.health >= 0 && crew.health <= 200 &&
                std::isfinite(crew.x) && std::isfinite(crew.y));
        require(crew.weaponSkill >= 0 && crew.weaponSkill <= 2 && crew.repairSkill >= 0 &&
                crew.repairSkill <= 2);
        const int combatCap = combatSkillCap(crew.appearance.species);
        require(crew.combatSkillProgress >= 0 && crew.combatSkillProgress <= combatCap &&
                crew.combatSkill == combatSkillTier(crew.combatSkillProgress, combatCap));
        for (int skill : crew.manningSkill) {
            require(skill >= 0 && skill <= 2);
        }
        (void)room(crew.targetRoom);
        for (const auto& waypoint : crew.path) {
            require(waypoint.door >= -1 && waypoint.door < static_cast<int>(doors_.size()) &&
                    std::isfinite(waypoint.point.x) && std::isfinite(waypoint.point.y));
        }
    }
    const auto validIonLock = [](const RunSystem& sys) {
        return sys.lockCount >= -1 && std::isfinite(sys.lockElapsed) && sys.lockElapsed >= 0 &&
               sys.lockElapsed <= 5 && std::isfinite(sys.ion) &&
               sys.ion == static_cast<float>(std::max(sys.lockCount, 0)) &&
               std::isfinite(sys.ionTimer) &&
               sys.ionTimer == (sys.lockCount > 0 ? 5.f - sys.lockElapsed : 0.f);
    };
    const auto validSystemPower = [&](const RunSystem& sys) {
        // AddDamage614e50 retains assigned reactor/battery bars until the base
        // callback. A saved total may therefore exceed current health/capacity.
        // ShipManager::AddSystem 0x49ca00 caps the starting level at the
        // SystemBlueprint maxPower but sets maxLevel from the template's max,
        // so an authored level may exceed maxLevel (JELLY_TRUFFLE_P_DLC pilot
        // power 1, max 0); UpgradeSystem only raises a level up to maxLevel.
        const auto maximum =
            std::max(systemMaximum(sys), catalog_.system(sys.definition.name).maxPower);
        const auto raw = static_cast<std::int64_t>(sys.power) - sys.batteryPower - sys.bonusPower;
        // The native diagnostic system ceiling is64; battery allocation may
        // retain up to twice that ceiling after the battery has stopped.
        return maximum >= 0 && maximum <= 64 && sys.definition.power >= 0 &&
               sys.definition.power <= maximum && sys.health >= 0 &&
               sys.health <= sys.definition.power && sys.power >= 0 && raw >= 0 && raw <= maximum &&
               sys.batteryPower >= 0 && sys.batteryPower <= 128 && sys.bonusPower >= 0 &&
               sys.bonusPower <= sys.definition.power && sys.lastBonusPower >= 0 &&
               sys.lastBonusPower <= maximum && sys.restorePower >= 0 &&
               sys.restorePower <= 2 * maximum + 128;
    };
    const auto validateCrewAi = [&](const CrewAiState& state, const ShipDefinition& ship) {
        require(state.teleportRequest >= 0 && state.teleportRequest <= 65536 &&
                state.startingCrewCount >= 0 && state.startingCrewCount <= 65536);
        for (const int id : state.breachedRooms) {
            require(std::ranges::any_of(ship.rooms, [id](const RoomDefinition& candidate) {
                return candidate.id == id;
            }));
        }
    };
    validateCrewAi(playerCrewAi_, ship_);
    validateCrewAi(enemy_.crewAi, enemy_.ship);
    for (const auto& sys : systems_) {
        (void)room(sys.definition.room);
        require(validSystemPower(sys) && sys.definition.maxPower >= -1 && sys.powerDivide >= 0 &&
                sys.powerDivide <= 64 && validIonLock(sys));
    }
    require(std::isfinite(cloneTimer_) && cloneTimer_ >= -1 && cloneTimer_ <= 12.1f &&
            std::isfinite(cloneDeathTimer_) && cloneDeathTimer_ >= -1 && cloneDeathTimer_ <= 3.1f &&
            std::isfinite(cloneDuration_) && cloneDuration_ >= 3 && cloneDuration_ <= 12);
    require(playerIntruderCount_ >= 0 && playerIntruderCount_ <= 65536 &&
            enemy_.intruderCount >= 0 && enemy_.intruderCount <= 65536);
    std::unordered_set<int> cloneIds;
    require(clonePending_ >= -1 && clonePending_ < static_cast<int>(crew_.size()));
    if (clonePending_ >= 0) {
        require(crew_[std::size_t(clonePending_)].health == 0 &&
                crew_[std::size_t(clonePending_)].cloneReady);
    }
    for (int id : cloneQueue_) {
        require(id >= 0 && id < static_cast<int>(crew_.size()) && crew_[id].health == 0 &&
                crew_[id].outOfGame && crew_[id].cloneReady && cloneIds.insert(id).second);
    }
    for (const auto* actors : {&crew_, &enemy_.crew, &voyageBoarders_}) {
        for (const auto& member : *actors) {
            const auto& deathAnimation =
                catalog_.animation(member.appearance.species + "_death_" +
                                   (member.facing == "left" ? "left" : "right"));
            require(std::isfinite(member.deathAnimationElapsed) &&
                    member.deathAnimationElapsed >= 0 &&
                    member.deathAnimationElapsed <= deathAnimation.duration &&
                    (!member.deathAnimationDone ||
                     member.deathAnimationElapsed == deathAnimation.duration) &&
                    (!member.dead || member.health == 0) && (!member.dying || member.health == 0) &&
                    (!member.outOfGame || (member.dead && !member.controlled)) &&
                    member.deathNumber <= crewDeathCount_);
        }
    }
    require(reactorPowerCap_ >= 0 && reactorPowerCap_ <= 64 && reactorPowerLoss_ >= 0 &&
            reactorPowerLoss_ <= 64 && reactorPowerDivide_ >= 1 && reactorPowerDivide_ <= 64 &&
            enemy_.reactorPowerCap >= 0 && enemy_.reactorPowerCap <= 64 &&
            enemy_.reactorPowerLoss >= 0 && enemy_.reactorPowerLoss <= 64 &&
            enemy_.reactorPowerDivide >= 1 && enemy_.reactorPowerDivide <= 64);
    const auto validMindTarget = [&](int owner, int target) {
        if (owner < 0 || owner > 1 || target < -1) {
            return false;
        }
        if (target == -1) {
            return true;
        }
        if (owner == 0) {
            return target < static_cast<int>(crew_.size());
        }
        if (target < static_cast<int>(enemy_.crew.size())) {
            return true;
        }
        const auto foreignIndex = static_cast<std::size_t>(target) - enemy_.crew.size();
        return foreignIndex < voyageBoarders_.size() &&
               voyageBoarders_[foreignIndex].enemyCrewOrigin == -1;
    };
    require(nextCrewDroneId_ > 0 && nextPhysicalCrewOrder_ > 0 &&
            retiredCrewDrones_.size() <= 65536 && nextSpaceDroneId_ > 0 &&
            nextSpaceDroneOrder_ > 0 && retiredSpaceDrones_.size() <= 65536);
    require(spaceShipOwners_.size() <= 2);
    std::unordered_set<int> registeredShipOwners;
    for (const int owner : spaceShipOwners_) {
        require(owner >= 0 && owner <= 1 && registeredShipOwners.insert(owner).second);
    }
    std::unordered_set<std::uint64_t> spaceIdentities;
    std::unordered_set<std::uint64_t> spaceOrders;
    for (int owner : {0, 1}) {
        for (const auto* drone : spaceDronesForOwner(owner)) {
            require(drone->spaceActorId < nextSpaceDroneId_ &&
                    spaceIdentities.insert(drone->spaceActorId).second &&
                    drone->spaceRegisteredOrder < nextSpaceDroneOrder_ &&
                    (!drone->spacePendingArrival || drone->spaceRegisteredOrder == 0));
            if (drone->spaceRegisteredOrder != 0) {
                require(spaceOrders.insert(drone->spaceRegisteredOrder).second);
            }
        }
    }
    std::unordered_set<std::uint64_t> droneIdentities;
    for (const int owner : {0, 1}) {
        for (const auto* drone : physicalCrewDrones(owner)) {
            require(drone->crewActorId > 0 && drone->crewActorId < nextCrewDroneId_ &&
                    droneIdentities.insert(drone->crewActorId).second);
            require(drone->physicalCrewOrder < nextPhysicalCrewOrder_);
        }
    }
    for (const auto* actors : {&crew_, &enemy_.crew, &voyageBoarders_}) {
        for (const auto& actor : *actors) {
            require(actor.physicalCrewOrder > 0 &&
                    actor.physicalCrewOrder < nextPhysicalCrewOrder_);
        }
    }
    const auto validAttackTarget = [&](const auto& actor) {
        const int owner = actor.attackTargetOwner;
        const int index = actor.attackTargetIndex;
        if (actor.attackTargetDrone) {
            if (owner < 0 || owner > 1 || index != -1 || actor.attackTargetDroneId == 0) {
                return false;
            }
            const auto bodies = physicalCrewDrones(owner);
            return std::ranges::any_of(bodies, [&](const CombatDrone* body) {
                return body->crewActorId == actor.attackTargetDroneId;
            });
        }
        if (actor.attackTargetDroneId != 0) {
            return false;
        }
        if (owner == -1 || index == -1) {
            return owner == -1 && index == -1;
        }
        return owner >= 0 && owner <= 1 && index >= 0 && validMindTarget(owner, index);
    };
    for (const auto& actor : crew_) {
        require(validAttackTarget(actor));
    }
    for (const auto& actor : enemy_.crew) {
        require(validAttackTarget(actor));
    }
    for (const auto& actor : voyageBoarders_) {
        require(validAttackTarget(actor));
    }
    for (const auto& actor : combatDrones_) {
        require(validAttackTarget(actor));
    }
    for (const auto& actor : enemy_.drones) {
        require(validAttackTarget(actor));
    }
    for (const auto& factory : cargoFactories_) {
        require(validAttackTarget(factory.drone));
    }
    require(validAttackTarget(overflowFactory_.drone));
    for (const auto& actor : retiredCrewDrones_) {
        require(validAttackTarget(actor));
    }
    require(validMindTarget(mindTargetOwner_, mindTarget_));
    for (const auto* crewList : {&mindQueue_[0], &mindQueue_[1], &mindResisted_}) {
        require(crewList->size() <= 256);
        for (const auto& queued : *crewList) {
            require(queued.index >= 0 && validMindTarget(queued.owner, queued.index));
        }
    }
    const auto finiteAbility = [&](const CombatAbility& ability, int id) {
        require(std::isfinite(ability.elapsed) && std::isfinite(ability.duration) &&
                std::isfinite(ability.cooldown) && ability.elapsed >= 0 && ability.duration >= 0 &&
                ability.cooldown >= 0);
        require(
            ability.soundEffect.empty() ||
            (id == 10 && (ability.soundEffect == "cloak" || ability.soundEffect == "decloak")) ||
            (id == 12 && ability.soundEffect == "batteryStop"));
    };
    finiteAbility(cloak_, 10);
    finiteAbility(battery_, 12);
    finiteAbility(hacking_, 15);
    finiteAbility(enemy_.cloak, 10);
    finiteAbility(enemy_.battery, 12);
    // SetBatteryPower takes 2 x effective power (at most eight bars), or -1 to derive.
    for (const int capacity : batteryCapacity_) {
        require(capacity >= -1 && capacity <= 16);
    }
    // DamageMessage 0x59d080 jitters by random32() % 21 - 10; messages live 60 frames.
    require(resourceFeedback_.size() <= 1024);
    for (const auto& message : resourceFeedback_) {
        require(message.resource < 4 && message.jitter >= -10 && message.jitter <= 10 &&
                message.start <= animation_);
    }
    for (std::size_t owner = 0; owner < 2; ++owner) {
        require(std::isfinite(slugGelHealElapsed_[owner]) && slugGelHealElapsed_[owner] >= 0 &&
                slugGelHealElapsed_[owner] <= 1 && lastHullDamage_[owner] >= 0);
        require(fireOxygen_[owner].size() <= 256 && roomLastOxygen_[owner].size() <= 256 &&
                fireExtinguishers_[owner].size() <= 256 && doorHitTick_[owner].size() <= 256 &&
                systemExplosions_[owner].size() <= 256);
        for (const auto& [room, level] : roomLastOxygen_[owner]) {
            require(room >= 0 && std::isfinite(level));
        }
        for (const auto& [door, tick] : doorHitTick_[owner]) {
            require(door >= 0 && tick <= animation_);
        }
        for (const auto& [room, emitter] : fireExtinguishers_[owner]) {
            require(room >= 0 && std::isfinite(emitter.birthCounter));
            for (const auto& particle : emitter.particles) {
                require(std::isfinite(particle.x) && std::isfinite(particle.y) &&
                        std::isfinite(particle.dx) && std::isfinite(particle.dy) &&
                        std::isfinite(particle.life));
            }
        }
        for (const auto& effect : systemExplosions_[owner]) {
            require(!effect.animation.empty() && effect.animation.size() <= 64 &&
                    std::isfinite(effect.elapsed) && effect.elapsed >= 0 &&
                    std::isfinite(effect.position.x) && std::isfinite(effect.position.y));
        }
    }
    // TeleportSystem charges to 100 and locks whole five-second intervals; the armed
    // room is either unset (-1) or one of the target ship's rooms.
    require(std::isfinite(teleportCharge_) && teleportCharge_ >= 0 && teleportCharge_ <= 100 &&
            (teleportArmed_ >= 0 && teleportArmed_ <= 2) && teleportTarget_ >= -1 &&
            (teleportTarget_ == -1 ||
             std::ranges::any_of(enemy_.ship.rooms, [&](const RoomDefinition& candidate) {
                 return candidate.id == teleportTarget_;
             })));
    finiteAbility(mind_, 14);
    finiteAbility(enemy_.mind, 14);
    finiteAbility(enemy_.hacking, 15);
    require(std::isfinite(enemy_.cloneTimer) && enemy_.cloneTimer >= -1 &&
            enemy_.cloneTimer <= 12.1f && std::isfinite(enemy_.cloneDeathTimer) &&
            enemy_.cloneDeathTimer >= -1 && enemy_.cloneDeathTimer <= 3.1f &&
            std::isfinite(enemy_.cloneDuration) && enemy_.cloneDuration >= 3 &&
            enemy_.cloneDuration <= 12);
    std::unordered_set<int> enemyCloneIds;
    require(enemy_.clonePending >= -1 &&
            enemy_.clonePending < static_cast<int>(enemy_.crew.size()));
    if (enemy_.clonePending >= 0) {
        require(enemy_.crew[std::size_t(enemy_.clonePending)].health == 0 &&
                enemy_.crew[std::size_t(enemy_.clonePending)].cloneReady);
    }
    for (const int id : enemy_.cloneQueue) {
        require(id >= 0 && id < int(enemy_.crew.size()) &&
                enemy_.crew[std::size_t(id)].health == 0 &&
                enemy_.crew[std::size_t(id)].outOfGame && enemy_.crew[std::size_t(id)].cloneReady &&
                enemyCloneIds.insert(id).second);
    }

    require(validMindTarget(enemy_.mindTargetOwner, enemy_.mindTarget));
    require(aiming_ >= -1 && aiming_ < static_cast<int>(combatWeapons_.size()));
    require(panelPage_ >= 0 && panelPage_ < 100 && sectorType_ >= 0 && sectorType_ <= 2);
    require(std::isfinite(ftlPullout_) && ftlPullout_ >= -40 && ftlPullout_ <= 0 &&
            std::isfinite(previousFtlPullout_) && previousFtlPullout_ >= -40 &&
            previousFtlPullout_ <= 0);
    require(hudLastHull_ >= 0 && hudLastHull_ <= ship_.hull &&
            (hullWarningLevel_ == 25 || hullWarningLevel_ == 50 || hullWarningLevel_ == 75));
    for (std::size_t index = 0; index < hudWarnings_.size(); ++index) {
        const auto& warning = hudWarnings_[index];
        require(std::isfinite(warning.elapsed) && warning.elapsed >= 0 &&
                warning.elapsed <= (index == 3 ? 3.f : 6.f) && std::isfinite(warning.flash) &&
                warning.flash >= 0 && warning.flash <= .66000003f);
        require(!warning.running || warning.flashRunning);
    }
    for (const float value : {ftlBlink_, previousFtlBlink_, pauseBlink_, previousPauseBlink_}) {
        require(std::isfinite(value) && value >= 0 && value <= .75f);
    }
    require((ftlBlinkDirection_ == -1 || ftlBlinkDirection_ == 1) &&
            (pauseBlinkDirection_ == -1 || pauseBlinkDirection_ == 1));
    require(std::isfinite(ftlCharge_) && std::isfinite(oxygen_) && jumpTicks_ >= 0 &&
            jumpTicks_ <= 120);
    for (std::size_t i = 0; i < combatDrones_.size(); ++i) {
        require(combatDrones_[i].blueprint == ship_.drones[i]);
    }
    const auto validateDrone = [&](const CombatDrone& drone, int owner) {
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        const bool crewDrone =
            blueprint.type == "REPAIR" || blueprint.type == "BATTLE" || blueprint.type == "BOARDER";
        const bool aboardEnemy = blueprint.type == "BOARDER" ? owner == 0 : owner == 1;
        const auto& ship = aboardEnemy ? enemy_.ship : ship_;
        require(drone.space >= 0 && drone.space <= 1 && drone.bonusPower >= 0 &&
                drone.bonusPower <= blueprint.power && std::isfinite(drone.health) &&
                drone.health >= 0 &&
                drone.health <= (blueprint.type == "REPAIR"         ? 25.f
                                 : drone.blueprint == "BOARDER_ION" ? 125.f
                                                                    : 150.f) &&
                std::isfinite(drone.workProgress) && drone.workProgress >= 0 &&
                drone.workProgress <= 100 && std::isfinite(drone.rebuildTimer) &&
                drone.rebuildTimer >= 0 && drone.rebuildTimer <= 10 &&
                std::isfinite(drone.deliveryTimer) && drone.deliveryTimer >= 0 &&
                drone.crewKills >= 0 && drone.crewPath.size() <= 4096 &&
                drone.crewPath.size() == drone.crewPathDoors.size() && drone.crewPathIndex >= 0 &&
                std::size_t(drone.crewPathIndex) <= drone.crewPath.size());
        const auto validRoom = [&](int id) {
            return id == -1 || std::ranges::any_of(ship.rooms, [id](const auto& room) {
                       return room.id == id;
                   });
        };
        if (crewDrone) {
            require(validRoom(drone.room) && validRoom(drone.targetRoom));
        }
        for (std::size_t i = 0; i < drone.crewPath.size(); ++i) {
            require(std::isfinite(drone.crewPath[i].x) && std::isfinite(drone.crewPath[i].y));
            const int door = drone.crewPathDoors[i];
            require(door == -1 || std::ranges::any_of(ship.doors, [door](const auto& candidate) {
                        return candidate.id == door;
                    }));
        }
    };
    for (std::size_t i = 0; i < combatWeapons_.size(); ++i) {
        require(combatWeapons_[i].bonusPower >= 0 &&
                combatWeapons_[i].bonusPower <= catalog_.equipment(ship_.weapons[i]).power);
    }
    for (std::size_t i = 0; i < enemy_.weapons.size(); ++i) {
        require(enemy_.weapons[i].bonusPower >= 0 &&
                enemy_.weapons[i].bonusPower <= catalog_.equipment(enemy_.ship.weapons[i]).power);
    }
    for (const auto& drone : combatDrones_) {
        require(!drone.bossSurge);
        validateDrone(drone, 0);
    }
    for (const auto& drone : enemy_.drones) {
        validateDrone(drone, 1);
    }
    require(hackingDrone_.blueprint == "DRONE_HACKING" && hackingDroneSpace_ >= 0 &&
            hackingDroneSpace_ <= 1 &&
            (!hackingDrone_.deployed ||
             (enemy_.present && hackingTarget_ >= 0 && hackingDroneId_ > 0)) &&
            std::isfinite(hackingDrone_.position.x) && std::isfinite(hackingDrone_.position.y) &&
            std::isfinite(hackingDrone_.destination.x) &&
            std::isfinite(hackingDrone_.destination.y) && std::isfinite(hackingDrone_.ionStun) &&
            hackingDrone_.ionStun >= 0);
    require(enemy_.hackingDrone.blueprint == "DRONE_HACKING" && enemy_.hackingDroneSpace >= 0 &&
            enemy_.hackingDroneSpace <= 1 && enemy_.hackingTarget >= -1 &&
            enemy_.hackingTarget < 16 &&
            (enemy_.hackingTarget == -1 || system(enemy_.hackingTarget)) &&
            (!enemy_.hackingDrone.deployed ||
             (enemy_.present && enemy_.hackingTarget >= 0 && enemy_.hackingDroneId > 0 &&
              enemy_.hackingDroneId < nextProjectile_)) &&
            (!enemy_.hacking.active ||
             (enemy_.hackingDrone.deployed && enemy_.hackingDrone.delivered)) &&
            std::isfinite(enemy_.hackingDrone.position.x) &&
            std::isfinite(enemy_.hackingDrone.position.y) &&
            std::isfinite(enemy_.hackingDrone.destination.x) &&
            std::isfinite(enemy_.hackingDrone.destination.y) &&
            std::isfinite(enemy_.hackingDrone.ionStun) && enemy_.hackingDrone.ionStun >= 0 &&
            std::isfinite(enemy_.hackingDrone.health) && enemy_.hackingDrone.health >= 0 &&
            enemy_.hackingDrone.health <= 1 && std::isfinite(enemy_.hackingDrone.rebuildTimer) &&
            enemy_.hackingDrone.rebuildTimer >= 0 && enemy_.hackingDrone.rebuildTimer <= 10);
    require(enemy_.teleportStartingCrew >= 0 && enemy_.teleportStartingCrew <= 64 &&
            enemy_.teleportWaves >= 0 && enemy_.teleportWaves <= 2 &&
            std::isfinite(enemy_.teleportCharge) && enemy_.teleportCharge >= 0 &&
            enemy_.teleportCharge <= 100 && enemy_.teleportRequest >= 0 &&
            enemy_.teleportRequest <= 64 && enemy_.teleportParty.size() <= 4);
    std::unordered_set<int> teleportParty;
    for (int index : enemy_.teleportParty) {
        require(index >= 0 && std::size_t(index) < enemy_.crew.size() &&
                teleportParty.insert(index).second);
    }
    require(enemy_.rewardVariant >= -1 && enemy_.rewardVariant <= 8 && enemy_.rewardTier >= -1 &&
            enemy_.rewardTier <= 2 &&
            (enemy_.rewardType.empty() || enemy_.rewardType == "reference" ||
             enemy_.rewardType == "standard" || enemy_.rewardType == "stuff" ||
             enemy_.rewardType == "fuel" || enemy_.rewardType == "scrap_only" ||
             enemy_.rewardType == "weapon"));
    if (!enemy_.rewardItem.empty()) {
        const auto& item = catalog_.equipment(enemy_.rewardItem);
        require(item.kind == "weapon" || item.kind == "drone" || item.kind == "aug");
    }
    const auto validAppearance = [&](const std::string& species, int appearance) {
        if (species.empty()) {
            return appearance == 0;
        }
        int count = species == "human" ? 2 : 1;
        for (const auto& layer : catalog_.appearance(species).layers) {
            count *= std::max(1, static_cast<int>(layer.size()));
        }
        return appearance >= 0 && appearance < count;
    };
    require(validAppearance(enemy_.rewardCrewSpecies, enemy_.rewardCrewAppearance));
    require(enemy_.rewardCrewName.size() <= 256 && enemy_.rewardCrewFullName.size() <= 256 &&
            enemy_.rewardCrewAppearance >= 0);
    if (!enemy_.rewardCrewSpecies.empty()) {
        (void)catalog_.appearance(enemy_.rewardCrewSpecies);
    }
    if (enemy_.rewardCrewPending) {
        require(!enemy_.rewardCrewSpecies.empty() && !enemy_.rewardCrewName.empty() &&
                !enemy_.rewardCrewFullName.empty() &&
                (enemy_.rewardCrewSpecies != "human" ||
                 (enemy_.rewardCrewAppearance & 1) == (enemy_.rewardCrewMale ? 0 : 1)));
    }
    require(!enemy_.rewardCrewOverflow || enemy_.rewardCrewPending);
    require(enemy_.boardingAi >= 0 && enemy_.boardingAi <= 2);
    if (enemy_.present) {
        require(enemy_.escapeThreshold >= -100 && enemy_.escapeThreshold <= 300 &&
                std::isfinite(enemy_.escapeTimer) && enemy_.escapeTimer >= -1 &&
                enemy_.escapeTimer <= 3600 && std::isfinite(enemy_.escapeJumpElapsed) &&
                enemy_.escapeJumpElapsed >= -1 && enemy_.escapeJumpElapsed <= 2.1f &&
                std::isfinite(enemy_.stalemateTimer) && enemy_.stalemateTimer >= 0 &&
                enemy_.stalemateTimer <= 3600 && enemy_.lastEscapeHealth >= 0 &&
                enemy_.lastEscapeHealth <= enemy_.ship.hull);
        const auto& definition = catalog_.ship(enemy_.ship.id);
        require(nlohmann::json(enemy_.ship.rooms) == nlohmann::json(definition.rooms) &&
                nlohmann::json(enemy_.ship.doors) == nlohmann::json(definition.doors));
        require(enemy_.doors.size() == definition.doors.size() &&
                enemy_.fakeDoors.size() == definition.doors.size() &&
                enemy_.doorFrames.size() == definition.doors.size() &&
                enemy_.doorTimers.size() == definition.doors.size());
        // Door::OnLoop 0x466760 ends a fake opening once its timer passes one
        // second; the native countdown then rests at most one step below zero.
        for (std::size_t i = 0; i < definition.doors.size(); ++i) {
            require(std::isfinite(enemy_.doorFrames[i]) && enemy_.doorFrames[i] >= 0 &&
                    enemy_.doorFrames[i] <= 4 && std::isfinite(enemy_.doorTimers[i]) &&
                    enemy_.doorTimers[i] >= -Time::step && enemy_.doorTimers[i] <= 1);
        }
        require(enemy_.weapons.size() == enemy_.ship.weapons.size() &&
                enemy_.powered.size() == enemy_.weapons.size());
        require(enemy_.crew.size() <= 64 && enemy_.systems.size() <= 32 &&
                enemy_.drones.size() <= 16);
        for (const auto& sys : enemy_.systems) {
            require(validSystemPower(sys) && sys.powerDivide >= 0 && sys.powerDivide <= 64 &&
                    validIonLock(sys));
        }
        for (const auto& id : enemy_.ship.weapons) {
            require(catalog_.equipment(id).kind == "weapon");
        }
        for (const auto& drone : enemy_.drones) {
            require(catalog_.equipment(drone.blueprint).kind == "drone");
        }
        for (const auto& crew : enemy_.crew) {
            require(crew.enemyCrewOrigin == -1 && crew.blockingDoor >= -1 &&
                    crew.blockingDoor < static_cast<int>(definition.doors.size()) &&
                    crew.lastDoor >= -1 &&
                    crew.lastDoor < static_cast<int>(definition.doors.size()));
            require(std::ranges::any_of(definition.rooms, [&](const auto& room) {
                return room.id == crew.room;
            }));
            require(std::isfinite(crew.health) && std::isfinite(crew.x) && std::isfinite(crew.y));
            require(crew.weaponSkill >= 0 && crew.weaponSkill <= 2 && crew.repairSkill >= 0 &&
                    crew.repairSkill <= 2);
            const int combatCap = combatSkillCap(crew.appearance.species);
            require(crew.combatSkillProgress >= 0 && crew.combatSkillProgress <= combatCap &&
                    crew.combatSkill == combatSkillTier(crew.combatSkillProgress, combatCap));
            for (int skill : crew.manningSkill) {
                require(skill >= 0 && skill <= 2);
            }
            for (const auto& waypoint : crew.path) {
                require(waypoint.door >= -1 &&
                        waypoint.door < static_cast<int>(definition.doors.size()));
            }
        }
    }
    const auto finitePoint = [](SDL_FPoint point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    const auto validateProjectile = [&](const CombatProjectile& projectile) {
        require((projectile.asteroidShieldSound == "hitShield1" ||
                 projectile.asteroidShieldSound == "hitShield2" ||
                 projectile.asteroidShieldSound == "hitShield3") &&
                (projectile.asteroidHullSound == "hitHull1" ||
                 projectile.asteroidHullSound == "hitHull2" ||
                 projectile.asteroidHullSound == "hitHull3"));
        require(((projectile.owner >= 0 && projectile.owner <= 1) ||
                 (projectile.owner == -1 &&
                  (projectile.weapon == "PDS_SHOT" || projectile.weapon == "ENV_ASTEROID"))) &&
                projectile.targetShip >= 0 && projectile.targetShip <= 1);
        require(projectile.id != 0 && projectile.room >= -1 && projectile.space >= 0 &&
                projectile.space <= 1 && std::isfinite(projectile.speed) && projectile.speed >= 0 &&
                std::isfinite(projectile.age) && projectile.age >= 0 &&
                std::isfinite(projectile.spinAngle) && std::isfinite(projectile.entryAngle));
        require(finitePoint(projectile.target) && finitePoint(projectile.targetEnd) &&
                finitePoint(projectile.position) && finitePoint(projectile.previous) &&
                finitePoint(projectile.direction) && finitePoint(projectile.beamLastCollision));
        const auto& targetRooms = projectile.targetShip == 0 ? ship_.rooms : enemy_.ship.rooms;
        // Beam room bookkeeping. BeamWeapon::CollisionCheck 0x436040 records each
        // crossed room once, and the beam sweep is the only path that appends to
        // hitRooms, so a repeat, an unknown room or contacts on another weapon
        // type describe corrupt state rather than a live sweep.
        require(projectile.hitRooms.size() <= 128 &&
                projectile.hitRooms.size() <= targetRooms.size());
        std::unordered_set<int> hitRooms;
        for (int hitRoom : projectile.hitRooms) {
            require(std::ranges::any_of(targetRooms, [hitRoom](const RoomDefinition& candidate) {
                return candidate.id == hitRoom;
            }));
            require(hitRooms.insert(hitRoom).second);
        }
        require(projectile.room == -1 ||
                std::ranges::any_of(targetRooms, [&](const RoomDefinition& candidate) {
                    return candidate.id == projectile.room;
                }));
        const auto& equipment = catalog_.equipment(projectile.weapon);
        const auto boundedPoint = [&](SDL_FPoint point, float bound) {
            return finitePoint(point) && std::abs(point.x) <= bound && std::abs(point.y) <= bound;
        };
        require(
            boundedPoint(projectile.beamOrigin, 4096) &&
            boundedPoint(projectile.beamSubStart, 4096) &&
            boundedPoint(projectile.beamSubEnd, 4096) && std::isfinite(projectile.beamHeading) &&
            std::abs(projectile.beamHeading) <= 10000 && std::isfinite(projectile.beamDrift) &&
            std::abs(projectile.beamDrift) <= .125f && projectile.beamLastDamage >= 0 &&
            projectile.beamLastDamage <= std::max(0, equipment.damage + projectile.damageBonus) &&
            std::isfinite(projectile.beamAnimationTimer) &&
            projectile.beamAnimationTimer >= -1 - projectile.age - Time::step &&
            projectile.beamAnimationTimer <= .34f);
        require((projectile.beamLastSmoke.x == 2147483600.f &&
                 projectile.beamLastSmoke.y == 2147483600.f) ||
                (projectile.beamLastSmoke.x == -2147483600.f &&
                 projectile.beamLastSmoke.y == -2147483600.f) ||
                boundedPoint(projectile.beamLastSmoke, 4096));
        const auto validateBeamEffects = [&](const auto& effects, bool smoke) {
            require(effects.size() <= 4096);
            for (const auto& effect : effects) {
                const float duration = smoke && effect.animation == "fire_smoke"
                                           ? .5f
                                           : catalog_.animation(effect.animation).duration;
                require(std::isfinite(effect.elapsed) && effect.elapsed >= 0 &&
                        effect.elapsed <= duration + Time::step && std::isfinite(effect.scale));
                if (smoke) {
                    // The original creates its first smoke from the untouched
                    // INT_MAX-float sentinel before updating the contact point.
                    require(
                        (effect.animation == "fire_smoke" || effect.animation == "explosion1") &&
                        boundedPoint(effect.position, 2147483648.f + 1024.f) &&
                        (effect.animation == "fire_smoke"
                             ? effect.scale == .2f || effect.scale == .5f
                             : effect.scale == .75f));
                } else {
                    require(effect.animation == projectile.projectileImage &&
                            boundedPoint(effect.position, 4096) && effect.scale == 1);
                }
            }
        };
        validateBeamEffects(projectile.beamContacts, false);
        validateBeamEffects(projectile.beamSmoke, true);
        require(equipment.type == "BEAM" ||
                (!projectile.beamVisualInitialized && projectile.beamContacts.empty() &&
                 projectile.beamSmoke.empty() && projectile.beamAnimationTimer == -1 &&
                 projectile.beamLastDamage == 0));
        const auto validSound = [](const std::string& alias,
                                   const std::vector<std::string>& choices) {
            return alias.size() <= 256 &&
                   (choices.empty() ? alias.empty()
                                    : std::ranges::find(choices, alias) != choices.end());
        };
        if (projectile.soundsInitialized) {
            require(validSound(projectile.hitShipSound, equipment.hitShipSounds) &&
                    validSound(projectile.hitShieldSound, equipment.hitShieldSounds) &&
                    validSound(projectile.missSound, equipment.missSounds));
        } else {
            require(projectile.hitShipSound.empty() && projectile.hitShieldSound.empty() &&
                    projectile.missSound.empty());
        }
        require(std::isfinite(projectile.bombAnimationElapsed) &&
                projectile.bombAnimationElapsed >= 0 && std::isfinite(projectile.bombDelay) &&
                projectile.bombDelay >= -static_cast<float>(Time::stepSeconds) &&
                projectile.bombDelay <= .4f);
        if (equipment.type == "BOMB") {
            const float animationDuration =
                projectile.projectileImage.empty()
                    ? 0.f
                    : catalog_.animation(projectile.projectileImage).duration;
            require(projectile.bombAnimationElapsed <=
                    animationDuration + static_cast<float>(Time::stepSeconds));
        } else {
            require(projectile.bombAnimationElapsed == 0 && projectile.bombDelay == .4f &&
                    !projectile.bombSuperShield && !projectile.bombBypass);
        }
        require(projectile.bombMissTick <= tick_);
        require(projectile.hitRooms.empty() || equipment.type == "BEAM");
        require(projectile.flightFrame >= -1 &&
                (projectile.flightFrame == -1 ||
                 (!projectile.projectileImage.empty() &&
                  projectile.flightFrame < catalog_.animation(projectile.projectileImage).frames)));
        // BombProjectile::CollisionCheck 0x44a5c0 teleports the bomb into its target
        // space on its first collision update; before that update it stays in its
        // owner's space and the two fields have to agree.
        if (equipment.type == "BOMB") {
            require(projectile.space ==
                    (projectile.arrived ? projectile.targetShip : projectile.owner));
        }
    };
    for (const auto& projectile : pendingRetaliation_) {
        validateProjectile(projectile);
    }
    for (const auto& projectile : projectiles_) {
        validateProjectile(projectile);
    }
    for (const auto& projectile : environment_.pdsQueue) {
        require(projectile.weapon == "PDS_SHOT" && projectile.owner == -1 &&
                projectile.defenseTargetId == 0 && projectile.defenseDroneTargetId == 0);
        validateProjectile(projectile);
    }
    require(environment_.asteroidQueue.size() <= 4096);
    for (const auto& projectile : environment_.asteroidQueue) {
        validateProjectile(projectile);
        require(projectile.weapon == "ENV_ASTEROID" && projectile.owner == -1 &&
                projectile.room == -1 && projectile.space == projectile.targetShip &&
                projectile.target.x == -1 && projectile.target.y == -1 && projectile.age == 0 &&
                !projectile.resolved && !projectile.arrived && projectile.defenseTargetId == 0 &&
                projectile.defenseDroneTargetId == 0);
    }

    // Combat snapshots are native diagnostics with stable IDs. Validate the
    // modeled transient graph before accepting it so corrupted IDs, timers or
    // drone target references cannot silently bind to a different object.
    std::unordered_set<std::uint64_t> projectileIds;
    std::uint64_t largestProjectileId = 0;
    for (const auto& projectile : pendingRetaliation_) {
        require(projectileIds.insert(projectile.id).second);
        largestProjectileId = std::max(largestProjectileId, projectile.id);
    }
    for (const auto& projectile : projectiles_) {
        require(projectileIds.insert(projectile.id).second);
        largestProjectileId = std::max(largestProjectileId, projectile.id);
        require(projectile.defenseTargetId == 0 || projectile.defenseTargetId != projectile.id);
        require(projectile.defenseTargetId < nextProjectile_ &&
                projectile.defenseSpaceDroneId < nextSpaceDroneId_ &&
                (projectile.defenseTargetId == 0 || projectile.defenseSpaceDroneId == 0));
    }
    require(nextProjectile_ != 0 && largestProjectileId < nextProjectile_);
    auto allProjectileIds = projectileIds;
    for (const auto& projectile : environment_.pdsQueue) {
        require(allProjectileIds.insert(projectile.id).second && projectile.id < nextProjectile_);
    }
    for (const auto& projectile : environment_.asteroidQueue) {
        require(allProjectileIds.insert(projectile.id).second && projectile.id < nextProjectile_);
    }
    // A fired defense projectile retains target provenance after that target
    // leaves Space. IDs belong to their historical namespace, not a live slot.

    const auto roomExists = [](const ShipDefinition& ship, int id) {
        return std::ranges::any_of(ship.rooms, [id](const RoomDefinition& candidate) {
            return candidate.id == id;
        });
    };
    const auto validateWeaponState = [&](const CombatWeapon& weapon,
                                         int owner,
                                         const EquipmentDefinition& blueprint) {
        const int frames = std::max(1, blueprint.animationFrames);
        const int charged = std::clamp(blueprint.chargedFrame, 0, frames - 1);
        const float duration = weaponFireDuration(blueprint) * static_cast<float>(frames) /
                               static_cast<float>(std::max(1, frames - charged));
        require(std::isfinite(weapon.animationElapsed) && weapon.animationElapsed >= 0 &&
                weapon.animationElapsed <= duration + Time::step && weapon.animationFrame >= 0 &&
                weapon.animationFrame < frames && weapon.animationBoostLevel >= -1 &&
                weapon.animationBoostLevel <=
                    std::max(blueprint.chargeLevels - 1, blueprint.boostCount) &&
                weapon.factoryHackLevel >= 0 && weapon.factoryHackLevel <= 3 &&
                weapon.pendingProjectiles.size() <= 4096);
        require(std::isfinite(weapon.animationSlide) && weapon.animationSlide >= 0 &&
                weapon.animationSlide <= .5f && weapon.animationHackLevel >= 0 &&
                weapon.animationHackLevel <= 3 && std::isfinite(weapon.hackSparksElapsed) &&
                (weapon.hackSparksElapsed == -1 ||
                 (weapon.hackSparksElapsed >= 0 &&
                  weapon.hackSparksElapsed <=
                      catalog_.animation("stun_spark_big").duration + Time::step)) &&
                finitePoint(weapon.hackSparksPosition));
        const int sparkRange = blueprint.frameHeight - blueprint.frameWidth;
        require((weapon.hackSparksPosition.x == -1 && weapon.hackSparksPosition.y == -1) ||
                (weapon.hackSparksPosition.x == 0 && sparkRange > 0 &&
                 weapon.hackSparksPosition.y <= 0 && weapon.hackSparksPosition.y > -sparkRange));
        require(weapon.weaponExplosionImage.empty() ||
                weapon.weaponExplosionImage == "explosion_big2" ||
                weapon.weaponExplosionImage == "explosion_big3" ||
                weapon.weaponExplosionImage == "explosion_big4");
        require(std::isfinite(weapon.weaponExplosionElapsed) &&
                (weapon.weaponExplosionElapsed == -1 ||
                 (!weapon.weaponExplosionImage.empty() && weapon.weaponExplosionElapsed >= 0 &&
                  weapon.weaponExplosionElapsed <=
                      catalog_.animation(weapon.weaponExplosionImage).duration)));
        for (const auto& projectile : weapon.pendingProjectiles) {
            validateProjectile(projectile);
            require(allProjectileIds.insert(projectile.id).second &&
                    projectile.id < nextProjectile_ && projectile.owner == owner &&
                    projectile.weapon == blueprint.id && !projectile.resolved && !projectile.dead &&
                    !projectile.checkedShield && projectile.age == 0 && !projectile.beamStepped &&
                    projectile.hitRooms.empty());
        }
        require(weapon.lastProjectileId == 0 || weapon.lastProjectileId < nextProjectile_);
        if (weapon.lastProjectileId != 0) {
            const auto active = std::ranges::find_if(projectiles_, [&](const auto& projectile) {
                return projectile.id == weapon.lastProjectileId;
            });
            require(active == projectiles_.end() ||
                    (active->owner == owner && catalog_.equipment(active->weapon).type == "BEAM"));
            const auto pending =
                std::ranges::find_if(weapon.pendingProjectiles, [&](const auto& projectile) {
                    return projectile.id == weapon.lastProjectileId;
                });
            require(pending == weapon.pendingProjectiles.end() || blueprint.type == "BEAM");
        }
        require(std::isfinite(weapon.charge) && weapon.charge >= 0 &&
                std::isfinite(weapon.cooldown) && weapon.cooldown >= 0 &&
                std::isfinite(weapon.volleyTimer) && weapon.volleyTimer >= 0 &&
                std::isfinite(weapon.entryAngle));
        require(finitePoint(weapon.target) && finitePoint(weapon.targetEnd) &&
                finitePoint(weapon.volleyTarget) && finitePoint(weapon.volleyEnd));
        require(weapon.targetRooms.size() <= 128 && weapon.targetPoints.size() <= 128 &&
                weapon.volleyRooms.size() <= 128 && weapon.volleyPoints.size() <= 128 &&
                weapon.targetGoal >= 0 && weapon.targetGoal <= 128 && weapon.chargeLevel >= 0 &&
                weapon.chargeLevel <= 128 && weapon.goalChargeLevel >= 0 &&
                weapon.goalChargeLevel <= 128 && weapon.boostLevel >= 0 &&
                weapon.boostLevel <= 128 && weapon.volleyRemaining >= 0 &&
                weapon.volleyRemaining <= 128 && weapon.volleyIndex >= 0 &&
                weapon.volleyIndex <= 128);
        const auto validateTarget = [&](int id, bool self) {
            if (id < 0) {
                require(id == -1);
                return;
            }
            const int targetShip = self ? owner : 1 - owner;
            require(roomExists(targetShip == 0 ? ship_ : enemy_.ship, id));
        };
        validateTarget(weapon.targetRoom, weapon.targetSelf);
        validateTarget(weapon.targetRoomEnd, weapon.targetSelf);
        validateTarget(weapon.volleyRoom, weapon.volleySelf);
        for (int id : weapon.targetRooms) {
            validateTarget(id, weapon.targetSelf);
        }
        for (int id : weapon.volleyRooms) {
            validateTarget(id, weapon.volleySelf);
        }
        for (SDL_FPoint point : weapon.targetPoints) {
            require(finitePoint(point));
        }
        for (SDL_FPoint point : weapon.volleyPoints) {
            require(finitePoint(point));
        }
    };
    for (std::size_t i = 0; i < combatWeapons_.size(); ++i) {
        validateWeaponState(combatWeapons_[i], 0, catalog_.equipment(ship_.weapons[i]));
    }
    for (std::size_t i = 0; i < enemy_.weapons.size(); ++i) {
        validateWeaponState(enemy_.weapons[i], 1, catalog_.equipment(enemy_.ship.weapons.at(i)));
    }
    for (const auto& sys : systems_) {
        if (sys.definition.id == 11) {
            validateWeaponState(sys.artillery, 0, catalog_.equipment(sys.definition.weapon));
        }
    }
    for (const auto& sys : enemy_.systems) {
        if (sys.definition.id == 11) {
            validateWeaponState(sys.artillery, 1, catalog_.equipment(sys.definition.weapon));
        }
    }

    const auto validateDroneState = [&](const CombatDrone& drone, int owner) {
        if (drone.blueprint.empty()) {
            require(nlohmann::json(drone) == nlohmann::json(CombatDrone{}));
            return;
        }
        require(drone.constructorExplosionImage.empty() ||
                drone.constructorExplosionImage == "explosion_big2" ||
                drone.constructorExplosionImage == "explosion_big3" ||
                drone.constructorExplosionImage == "explosion_big4");
        require(std::isfinite(drone.medicalRate) && std::abs(drone.medicalRate) <= 1000.f &&
                std::isfinite(drone.healingElapsed) && drone.healingElapsed >= -1 &&
                drone.healingElapsed <= 1.02f && std::isfinite(drone.mindElapsed) &&
                drone.mindElapsed >= -1 && drone.mindElapsed <= 1.02f &&
                std::isfinite(drone.stunElapsed) && drone.stunElapsed >= -1 &&
                drone.stunElapsed <= 1.02f);
        require(drone.repairKind >= 0 && drone.repairKind <= 3 && drone.repairTarget >= -1 &&
                drone.repairTarget <= 255 && drone.repairRoom >= -1 && drone.repairRoom <= 255 &&
                std::isfinite(drone.heading) && std::abs(drone.heading) <= 10000.f &&
                std::isfinite(drone.oldHeading) && std::abs(drone.oldHeading) <= 10000.f &&
                std::isfinite(drone.lastAimingAngle) &&
                std::abs(drone.lastAimingAngle) <= 10000.f && std::isfinite(drone.hackAngle) &&
                std::abs(drone.hackAngle) <= 10000.f &&
                // CombatDrone::OnLoop 0x4271d0 divides the distance from
                // lastDestination by the path length without a clamp; a
                // drone that drifted powered down (UpdatePoweredDown 0x426670)
                // resumes off its line, so the ratio can exceed one.
                std::isfinite(drone.progressToDestination) && drone.progressToDestination >= 0 &&
                finitePoint(drone.lastDestination) && std::isfinite(drone.spaceHackSparksElapsed) &&
                drone.spaceHackSparksElapsed >= -1 && drone.spaceHackSparksElapsed <= 1.02f &&
                drone.repairWavePhases.size() <= 4096);
        for (float phase : drone.repairWavePhases) {
            require(std::isfinite(phase) && phase >= 0 && phase <= 1.f);
        }
        // pause is a countdown that also carries the -1 sentinel advanceCombat
        // and advanceDrones write when a drone fires, is ion-stunned or is
        // hacked, so it is legitimately negative in a running fight.
        // DefenseDrone::OnLoop 0x426a50 parks an expired weapon timer at -1
        // with bFire latched until the drone has a shot. CombatDrone::OnLoop
        // 0x4271d0 counts a beam's additionalPause down by one step and
        // leaves it just below zero (at most one step) until the next beam.
        require(std::isfinite(drone.cooldown) && drone.cooldown >= -1 &&
                std::isfinite(drone.ionStun) && drone.ionStun >= 0 &&
                std::isfinite(drone.orbitAngle) && std::isfinite(drone.pause) &&
                drone.pause >= -1 && std::isfinite(drone.additionalPause) &&
                drone.additionalPause >= -Time::step && std::isfinite(drone.repairBeamAge) &&
                drone.repairBeamAge >= 0 && std::isfinite(drone.shieldGlow) &&
                std::isfinite(drone.aimAngle) && drone.lifespan >= -1);
        require(finitePoint(drone.position) && finitePoint(drone.destination) &&
                finitePoint(drone.velocity) && finitePoint(drone.aimTarget) &&
                finitePoint(drone.repairBeamTarget) && finitePoint(drone.basePosition) &&
                finitePoint(drone.defenseTargetVelocity));
        require(std::isfinite(drone.spaceMissElapsed) && drone.spaceMissElapsed >= -1 &&
                drone.spaceMissElapsed <= 1.f + Time::step && finitePoint(drone.spaceMissPosition));
        require(drone.targetProjectile == 0 || drone.targetProjectile < nextProjectile_);
        require(drone.targetSpaceDroneId < nextSpaceDroneId_ && drone.defenseTargetType >= 1 &&
                drone.defenseTargetType <= 6 && std::isfinite(drone.desiredAimAngle) &&
                std::abs(drone.desiredAimAngle) <= 360.f);
        require(drone.defenseCurrentTargetId <
                    (drone.defenseCurrentTargetIsDrone ? nextSpaceDroneId_ : nextProjectile_) &&
                drone.defenseShotTargetId <
                    (drone.defenseShotTargetIsDrone ? nextSpaceDroneId_ : nextProjectile_));
        require(std::isfinite(drone.ionPulseCooldown) && drone.ionPulseCooldown >= -Time::step &&
                drone.ionPulseCooldown <= 6 && std::isfinite(drone.ionPulseCharge) &&
                drone.ionPulseCharge >= -1 && drone.ionPulseCharge <= 1 + Time::step &&
                std::isfinite(drone.ionPulseEffect) && drone.ionPulseEffect >= 0 &&
                drone.ionPulseEffect <= 1.25f + Time::step &&
                std::isfinite(drone.ionPulseExplosion) && drone.ionPulseExplosion >= -1 &&
                drone.ionPulseExplosion <= .5f + Time::step);
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        const bool interior =
            blueprint.type == "REPAIR" || blueprint.type == "BATTLE" || blueprint.type == "BOARDER";
        require(interior || (drone.crewActorId == 0 && drone.physicalCrewOrder == 0));
        require(std::isfinite(drone.hackTime) && drone.hackTime >= 0 &&
                drone.controllerHackLevel >= 0 && drone.controllerHackLevel <= 3);
        require(std::isfinite(drone.hackingFlyingElapsed) && drone.hackingFlyingElapsed >= -1 &&
                drone.hackingFlyingElapsed <=
                    catalog_.animation("drone_hack_land").duration + Time::step &&
                std::isfinite(drone.hackingExtendingElapsed) &&
                drone.hackingExtendingElapsed >= -1 &&
                drone.hackingExtendingElapsed <=
                    catalog_.animation("drone_hack_extend").duration + Time::step &&
                std::isfinite(drone.hackingFlashElapsed) && drone.hackingFlashElapsed >= 0 &&
                drone.hackingFlashElapsed <= 1.f);
        require(drone.spaceExplosionElapsed == -1 ||
                (std::isfinite(drone.spaceExplosionElapsed) && drone.spaceExplosionElapsed >= 0 &&
                 (drone.constructorExplosionImage.empty()
                      ? blueprint.type == "HACKING" && drone.spaceExplosionElapsed == 0
                      : drone.spaceExplosionElapsed <=
                            catalog_.animation(drone.constructorExplosionImage).duration +
                                Time::step)));
        require(std::isfinite(drone.crewDeathElapsed) && drone.crewDeathElapsed >= 0);
        if (interior) {
            const auto species = blueprint.type == "REPAIR"      ? std::string("repair")
                                 : blueprint.id == "BOARDER_ION" ? std::string("boarder_ion")
                                                                 : std::string("battle");
            const auto& death = catalog_.animation(
                species + (drone.crewDeathLeft ? "_death_left" : "_death_right"));
            // A boarding pod in flight carries its body as built (CrewDrone ctor 0x438130:
            // bDead with full health) until delivery brings it aboard.
            const bool podInFlight = blueprint.type == "BOARDER" && !drone.delivered;
            require(drone.crewDeathElapsed <= death.duration &&
                    (!drone.crewDeathDone || drone.crewDeathElapsed == death.duration) &&
                    (!drone.crewDying || drone.health == 0) &&
                    (!(drone.crewDead && drone.deployed) || drone.health == 0 || podInFlight));
        } else {
            require(!drone.crewDying && !drone.crewDeathDone && drone.crewDeathElapsed == 0);
        }
        require(drone.crewPhysicalShip >= -1 && drone.crewPhysicalShip <= 1 &&
                drone.crewRoom >= -1 && drone.crewSlot >= -1 && finitePoint(drone.crewPosition));
        if (!interior) {
            require(drone.crewPhysicalShip == -1 && drone.crewRoom == -1 && drone.crewSlot == -1);
        } else if (drone.crewPhysicalShip == -1 || drone.crewRoom == -1) {
            require(drone.crewRoom == -1 && drone.crewSlot == -1);
        } else {
            const auto& bodyShip = drone.crewPhysicalShip == 0 ? ship_ : enemy_.ship;
            require(roomExists(bodyShip, drone.crewRoom));
            const auto bodyRoom = std::ranges::find_if(bodyShip.rooms, [&](const auto& shape) {
                return shape.id == drone.crewRoom;
            });
            require(drone.crewSlot < bodyRoom->w / 35 * (bodyRoom->h / 35));
        }
        const bool spaceActor = blueprint.type == "DEFENSE" || blueprint.type == "COMBAT" ||
                                blueprint.type == "BOARDER" || blueprint.type == "SHIP_REPAIR" ||
                                blueprint.type == "SHIELD" || blueprint.type == "HACKING";
        const bool inactiveEnemyHackingFactory =
            owner == 1 && !enemy_.present && &drone == &enemy_.hackingDrone &&
            drone.spaceActorId == 0 && drone.spaceRegisteredOrder == 0 &&
            !drone.spacePendingArrival;
        require(spaceActor ? inactiveEnemyHackingFactory ||
                                 (drone.spaceActorId > 0 && drone.spaceActorId < nextSpaceDroneId_)
                           : drone.spaceActorId == 0 && drone.spaceRegisteredOrder == 0 &&
                                 !drone.spacePendingArrival);
        const int physicalShip =
            interior && drone.crewPhysicalShip >= 0 ? drone.crewPhysicalShip : owner;
        const auto& physicalShipDefinition = physicalShip == 0 ? ship_ : enemy_.ship;
        const auto& physicalDoors = physicalShipDefinition.doors;
        require(drone.ionLastRoom == -1 || roomExists(physicalShipDefinition, drone.ionLastRoom));
        if (drone.blueprint != "BOARDER_ION") {
            require(drone.ionLastRoom == -1 && drone.ionPulseCooldown == 6 &&
                    drone.ionPulseCharge == -1 && drone.ionPulseEffect == 0 &&
                    drone.ionPulseExplosion == -1);
        }
        require(drone.crewBlockingDoor >= -1 &&
                drone.crewBlockingDoor < static_cast<int>(physicalDoors.size()) &&
                drone.crewLastDoor >= -1 &&
                drone.crewLastDoor < static_cast<int>(physicalDoors.size()));
        const auto& opposingDrones = owner == 0 ? enemy_.drones : combatDrones_;
        require(drone.targetDrone == 0 || drone.targetDrone <= opposingDrones.size());
    };
    for (const auto& drone : combatDrones_) {
        validateDroneState(drone, 0);
    }
    for (const auto& drone : enemy_.drones) {
        validateDroneState(drone, 1);
    }
    require(enemy_.droneTrash.size() <= 65536);
    for (const auto& drone : enemy_.droneTrash) {
        validateDroneState(drone, 1);
    }
    for (const auto& drone : retiredCrewDrones_) {
        const auto& type = catalog_.equipment(drone.blueprint).type;
        require(type == "REPAIR" || type == "BATTLE" || type == "BOARDER");
        validateDroneState(drone, 0);
    }
    for (const auto& drone : retiredSpaceDrones_) {
        const auto& type = catalog_.equipment(drone.blueprint).type;
        require(type == "DEFENSE" || type == "COMBAT" || type == "SHIP_REPAIR" ||
                type == "SHIELD" || type == "HACKING");
        validateDroneState(drone, 0);
    }
    validateDroneState(hackingDrone_, 0);
    validateDroneState(enemy_.hackingDrone, 1);
    require(cargoFactories_.size() == cargo_.size());
    const auto validateInventoryFactory = [&](const InventoryFactory& factory,
                                              const std::string& id) {
        if (id.empty()) {
            require(nlohmann::json(factory) == nlohmann::json(InventoryFactory{}));
            return;
        }
        const auto& blueprint = catalog_.equipment(id);
        if (blueprint.kind == "weapon") {
            validateWeaponState(factory.weapon, 0, blueprint);
            require(nlohmann::json(factory.drone) == nlohmann::json(CombatDrone{}));
        } else if (blueprint.kind == "drone") {
            require(factory.drone.blueprint == id && !factory.drone.powered &&
                    !factory.drone.deployed && factory.drone.diagnosticDodge == -1);
            validateDroneState(factory.drone, 0);
            require(nlohmann::json(factory.weapon) == nlohmann::json(CombatWeapon{}));
        } else {
            require(blueprint.kind == "aug" &&
                    nlohmann::json(factory) == nlohmann::json(InventoryFactory{}));
        }
    };
    for (std::size_t i = 0; i < cargo_.size(); ++i) {
        validateInventoryFactory(cargoFactories_[i], cargo_[i]);
    }
    validateInventoryFactory(overflowFactory_, overflowEquipment_);

    const auto validateHazards = [&](const std::map<int, RoomHazard>& hazards,
                                     const ShipDefinition& ship) {
        for (const auto& [roomId, hazard] : hazards) {
            const auto shape =
                std::ranges::find_if(ship.rooms, [roomId](const RoomDefinition& row) {
                    return row.id == roomId;
                });
            require(shape != ship.rooms.end());
            const int tileCount = shape->w / 35 * (shape->h / 35);
            require(tileCount > 0 && hazard.fires >= 0 && hazard.fires <= tileCount &&
                    hazard.breaches >= 0 && hazard.breaches <= tileCount &&
                    hazard.breachTiles.size() <= static_cast<std::size_t>(tileCount) &&
                    (hazard.breachTiles.empty() ||
                     hazard.breachTiles.size() == static_cast<std::size_t>(hazard.breaches)));
            require(std::isfinite(hazard.fireRepair) && hazard.fireRepair >= 0 &&
                    std::isfinite(hazard.fireSpreadSeconds) && hazard.fireSpreadSeconds >= 0 &&
                    std::isfinite(hazard.breachRepair) && hazard.breachRepair >= 0);
            require(hazard.fireDeathTimers.size() <= static_cast<std::size_t>(tileCount));
            for (float timer : hazard.fireDeathTimers) {
                require(std::isfinite(timer) && timer >= -1 && timer <= 14);
            }
            require(hazard.fireTiles.empty() ||
                    hazard.fireTiles.size() == static_cast<std::size_t>(hazard.fires));
            require(hazard.fireHealth.size() == static_cast<std::size_t>(hazard.fires));
            for (float health : hazard.fireHealth) {
                require(std::isfinite(health) && health >= 0 && health <= 100);
            }
            require(hazard.fireStartTimers.size() <= static_cast<std::size_t>(tileCount));
            for (const auto& [tile, timer] : hazard.fireStartTimers) {
                require(tile >= 0 && tile < tileCount && std::isfinite(timer) && timer >= 0 &&
                        timer <= 49);
            }
            std::unordered_set<int> fireTiles;
            for (int tile : hazard.fireTiles) {
                require(tile >= 0 && tile < tileCount && fireTiles.insert(tile).second);
            }
            require(hazard.fireSmoke.size() <= static_cast<std::size_t>(tileCount));
            std::unordered_set<int> smokeTiles;
            for (const auto& smoke : hazard.fireSmoke) {
                require(smoke.tile >= 0 && smoke.tile < tileCount &&
                        smokeTiles.insert(smoke.tile).second && std::isfinite(smoke.elapsed) &&
                        smoke.elapsed >= 0 && smoke.elapsed <= 1);
            }
            std::unordered_set<int> uniqueTiles;
            for (int tile : hazard.breachTiles) {
                require(tile >= 0 && tile < tileCount && uniqueTiles.insert(tile).second);
            }
        }
    };
    validateHazards(playerHazards_, ship_);
    validateHazards(enemyHazards_, enemy_.ship);
    // The beam history is native stable state, with references to this encounter.
    for (std::size_t owner = 0; owner < beamHitAt_.size(); ++owner) {
        const auto& target = owner == 0 ? ship_ : enemy_.ship;
        require(beamHitAt_[owner].size() <= target.rooms.size());
        for (const auto& [roomId, when] : beamHitAt_[owner]) {
            require(roomExists(target, roomId) && std::isfinite(when) && when >= 0 &&
                    when <= float(Time::seconds(double(tick_))));
        }
        require(beamKilledCrew_[owner].size() <= 4096);
        for (const auto& [projectileId, killed] : beamKilledCrew_[owner]) {
            require(projectileId > 0 && std::uint64_t(projectileId) < nextProjectile_ &&
                    killed > 0 && killed <= 64);
        }
    }
    for (const auto& hit : shieldHits_) {
        require(hit.ship >= 0 && hit.ship <= 1 && hit.tick <= tick_ && finitePoint(hit.point) &&
                hit.projectileId < nextProjectile_ && std::isfinite(hit.currentSize) &&
                hit.currentSize >= 0 && hit.currentSize <= 40.f + 160.f * Time::step &&
                std::isfinite(hit.currentThickness) && hit.currentThickness >= 0 &&
                hit.currentThickness <= 75 && hit.cachedDamage >= 0);
    }
    require(nextDamageMessageOrder_ > 0);
    std::unordered_set<std::uint64_t> messageOrders;
    for (const auto& message : resistMessages_) {
        require(message.ship >= 0 && message.ship <= 1 && message.tick <= tick_ &&
                finitePoint(message.point) && std::isfinite(message.elapsed) &&
                message.elapsed >= 0 && message.elapsed <= 1 && message.messageOrder > 0 &&
                message.messageOrder < nextDamageMessageOrder_ &&
                messageOrders.insert(message.messageOrder).second);
    }
    for (const auto& message : damageMessages_) {
        require(message.ship >= 0 && message.ship <= 1 && message.tick <= tick_ &&
                finitePoint(message.point) && message.amount >= -999 && message.amount <= 999 &&
                std::isfinite(message.elapsed) && message.elapsed >= 0 && message.elapsed <= 1 &&
                message.messageOrder > 0 && message.messageOrder < nextDamageMessageOrder_ &&
                messageOrders.insert(message.messageOrder).second &&
                (!message.miss || (message.amount == 0 && !message.superShield && !message.ion)));
    }
    const auto validateExplosion = [&](const CombatExplosion& state, const ShipDefinition& ship) {
        if (!state.initialized) {
            require(state.gibs.empty() && state.effects.empty() && !state.finalBoom &&
                    state.soundTimer == -1 && state.explosionTimer == 0);
            return;
        }
        require(
            state.gibs.size() == ship.gibs.size() && state.effects.size() <= 30 &&
            std::isfinite(state.soundTimer) &&
            (state.soundTimer == -1 || (state.soundTimer >= -.1f && state.soundTimer <= .91f)) &&
            std::isfinite(state.explosionTimer) && state.explosionTimer >= -.1f &&
            state.explosionTimer <= .46f);
        for (const auto& gib : state.gibs) {
            require(finitePoint(gib.movement) && finitePoint(gib.offset) &&
                    std::isfinite(gib.rotationSpeed) && std::isfinite(gib.rotation) &&
                    std::abs(gib.movement.x) <= 4096 && std::abs(gib.movement.y) <= 4096 &&
                    std::abs(gib.offset.x) <= 4096 && std::abs(gib.offset.y) <= 4096 &&
                    std::abs(gib.rotationSpeed) <= 4096 && std::abs(gib.rotation) <= 4096);
        }
        int left = 10000;
        int top = 10000;
        int right = 0;
        int bottom = 0;
        for (const auto& shape : ship.rooms) {
            left = std::min(left, shape.x);
            top = std::min(top, shape.y);
            right = std::max(right, shape.x + shape.w);
            bottom = std::max(bottom, shape.y + shape.h);
        }
        for (const auto& effect : state.effects) {
            require((effect.animation == "explosion1" || effect.animation == "explosion_big2" ||
                     effect.animation == "explosion_big3" ||
                     effect.animation == "explosion_big4") &&
                    finitePoint(effect.position) && std::isfinite(effect.elapsed) &&
                    effect.elapsed >= 0 &&
                    effect.elapsed <= catalog_.animation(effect.animation).duration + Time::step &&
                    (effect.scale == 1 || effect.scale == 2) && effect.position.x >= left &&
                    effect.position.x <=
                        left + std::max(1, right - left - (effect.scale == 2 ? 128 : 32)) &&
                    effect.position.y >= top && effect.position.y <= bottom);
        }
    };
    validateExplosion(playerExplosionState_, ship_);
    validateExplosion(enemy_.explosionState, enemy_.ship);
    require(std::isfinite(explosion_) && std::isfinite(explosionDuration_) &&
            (explosionDuration_ == 3.f || explosionDuration_ == 4.f || explosionDuration_ == 6.f) &&
            explosion_ >= 0 && explosion_ <= explosionDuration_ &&
            (!enemyExplosionActive_ || enemy_.hull <= 0));
    require(!playerExplosionStarted_ && playerExplosionStartTick_ <= tick_);

    const auto& beacons = graph_.beacons();
    require(!beacons.empty() && beacons.size() < 256 && graph_.current() >= 0 &&
            graph_.current() < static_cast<int>(beacons.size()));
    // Waiting uses the source unclamped interpolation:59+128 is legal.
    // These ranges protect native checkpoint geometry without narrowing that
    // original near-threshold behavior to the ordinary60-pixel endpoint.
    const nlohmann::json graphData = graph_;
    require(graph_.dangerX() >= -959 && graph_.dangerX() <= 187 && graph_.dangerY() >= 50 &&
            graph_.dangerY() <= 299 && nativeValue<int>(graphData.at("pursuitDelay_")) >= -100 &&
            nativeValue<int>(graphData.at("pursuitDelay_")) <= 100 &&
            nativeValue<int>(graphData.at("waitStart_")) >= -959 &&
            nativeValue<int>(graphData.at("waitStart_")) <= 187 &&
            nativeValue<int>(graphData.at("waitMove_")) >= 0 &&
            nativeValue<int>(graphData.at("waitMove_")) <= 128);
    require(jumpDestination_ >= -1 && jumpDestination_ < static_cast<int>(beacons.size()));
    require(locationBeacon_ >= -1 && locationBeacon_ < static_cast<int>(beacons.size()));
    const auto validateSlots = [&](const std::vector<int>& slots, std::size_t count, int capacity) {
        require(slots.size() == static_cast<std::size_t>(capacity));
        std::unordered_set<int> used;
        for (int index : slots) {
            require(index >= -1 && (index == -1 || static_cast<std::size_t>(index) < count));
            if (index >= 0) {
                require(used.insert(index).second);
            }
        }
        require(used.size() == std::min(count, static_cast<std::size_t>(capacity)));
    };
    validateSlots(weaponSlots_, ship_.weapons.size(), ship_.weaponSlots);
    validateSlots(droneSlots_, ship_.drones.size(), ship_.droneSlots);
    require(upgradePurchases_.size() <= 128 && distressWaitTicks_ >= 0 &&
            distressWaitTicks_ <= 121);
    for (const auto& [id, cost] : upgradePurchases_) {
        require(cost >= 0 && (id == -1 || system(id)));
        if (id == -1) {
            require(ship_.reactor + pendingUpgrade(-1) <= devReactorLimit_);
        } else {
            const auto* sys = system(id);
            require(sys->definition.power + pendingUpgrade(id) <= systemMaximum(*sys));
        }
    }
    if (!overflowEquipment_.empty()) {
        require(catalog_.equipment(overflowEquipment_).kind != "aug");
    }
    if (!overflowAugment_.empty()) {
        require(catalog_.equipment(overflowAugment_).kind == "aug");
    }
    for (const auto& cloud : mapNebulas_) {
        require(cloud.variant >= 0 && cloud.variant <= 6);
    }
    require(selectedVoyageCrewSeed_ >= -1 && selectedVoyageCrewSeed_ <= 4294967295LL);
    require(usedVoyageEvents_.size() <= 4096 && crewNameHistory_.size() <= 65536);
    for (const auto& name : crewNameHistory_) {
        require(!name.empty() && name.size() <= 4096);
    }
    for (const auto& eventName : usedVoyageEvents_) {
        require(!eventName.empty() && eventName.size() <= 256);
    }
    require(sectorGraph_.nodes().size() <= 64 && !sectorGraph_.nodes().empty());
    (void)sectorGraph_.current();
    for (const auto& node : sectorGraph_.nodes()) {
        require(node.level >= 0 && node.level < 8 && node.type >= 0 && node.type <= 2);
        for (int neighbor : node.neighbors) {
            require(neighbor >= 0 && neighbor < static_cast<int>(sectorGraph_.nodes().size()));
        }
    }
    require(pendingVoyageEffects_.is_array() && pendingVoyageEffects_.size() <= 512 &&
            voyageBoarders_.size() <= 64);
    // Deferred event rows intentionally retain their XML-derived JSON schema.
    // Their numeric fields are native int counters, except removeCrew's vector index.
    for (const auto& effect : pendingVoyageEffects_) {
        require(effect.is_object());
        const auto kind = effect.at("kind").get<std::string>();
        if (effect.contains("skill_progress")) {
            require(kind == "crew" || kind == "crewSkills");
            const auto progress = nativeValue<std::array<int, 6>>(effect.at("skill_progress"));
            constexpr std::array<int, 6> caps{30, 30, 110, 130, 36, 16};
            for (std::size_t skill = 0; skill < progress.size(); ++skill) {
                require(progress[skill] >= 0 && progress[skill] <= caps[skill]);
            }
        }
        for (const auto& [key, value] : effect.items()) {
            if (value.is_number()) {
                if (kind == "removeCrew" && key == "id") {
                    (void)nativeValue<std::size_t>(value);
                } else {
                    (void)nativeValue<int>(value);
                }
            }
        }
    }
    std::unordered_set<int> foreignOrigins;
    for (const auto& member : voyageBoarders_) {
        if (member.enemyCrewOrigin >= 0) {
            const auto origin = std::size_t(member.enemyCrewOrigin);
            // A dying actor keeps its owner until its physical removal records
            // the death on the owner's roster.
            require((member.health > 0 || (member.dying && !member.outOfGame)) &&
                    foreignOrigins.insert(member.enemyCrewOrigin).second);
            const auto owner = enemy_.present ? nlohmann::json(enemy_.crew)
                               : campaign_.savedBoss.is_object()
                                   ? campaign_.savedBoss.at("enemy").at("crew")
                                   : nlohmann::json::array();
            require(origin < owner.size());
            const auto home = owner.at(origin).get<RunCrew>();
            require(home.health == 0 &&
                    home.appearance.appearance == member.appearance.appearance &&
                    home.appearance.species == member.appearance.species &&
                    home.appearance.name == member.appearance.name);
        }

        require(member.blockingDoor >= -1 &&
                member.blockingDoor < static_cast<int>(ship_.doors.size()) &&
                member.lastDoor >= -1 && member.lastDoor < static_cast<int>(ship_.doors.size()));
        (void)room(member.room);
        (void)room(member.targetRoom);
        require(std::isfinite(member.health) && member.health >= 0 && member.health <= 200 &&
                std::isfinite(member.x) && std::isfinite(member.y) &&
                std::isfinite(member.stunTime) && member.stunTime >= 0 && member.stunTime <= 60);
        for (const auto& waypoint : member.path) {
            require(waypoint.door >= -1 && waypoint.door < static_cast<int>(doors_.size()) &&
                    std::isfinite(waypoint.point.x) && std::isfinite(waypoint.point.y));
        }
        (void)catalog_.appearance(member.appearance.species);
    }
    for (const auto* path : {&eventBackground_, &eventPlanet_}) {
        require(path->empty() || archive_.contains(*path));
    }
    require(eventPlanetRotation_ == 0 || eventPlanetRotation_ == 180);
    require(environment_.flareTicks >= 0 && environment_.flareTicks <= 3600 &&
            environment_.flareGoalTicks >= 0 && environment_.flareGoalTicks <= 3600 &&
            environment_.flashTicks >= 0 && environment_.flashTicks <= 60 &&
            environment_.asteroidState >= 0 && environment_.asteroidState <= 2 &&
            environment_.asteroidStateTicks >= 0 &&
            environment_.asteroidStateTicks <=
                60 * (31 + std::ranges::max(environment_.asteroidStateDraws)) &&
            std::ranges::min(environment_.asteroidStateDraws) >= 0 &&
            std::ranges::max(environment_.asteroidStateDraws) <= 1000000 &&
            std::ranges::min(environment_.asteroidRateDraws) >= 0 &&
            std::ranges::max(environment_.asteroidRateDraws) <= 1000000 &&
            environment_.asteroidSpawnTicks >= 0 && environment_.asteroidSpawnTicks <= 3600 &&
            environment_.asteroidTarget >= 0 && environment_.asteroidTarget <= 1 &&
            environment_.asteroidBand >= 0 && environment_.asteroidBand <= 3);
    require((environment_.flashMinTime == 11000 && environment_.flashMaxTime == 18000) ||
            (environment_.flashMinTime == 16000 && environment_.flashMaxTime == 32000) ||
            (environment_.flashMinTime == 20000 && environment_.flashMaxTime == 25000) ||
            (environment_.flashMinTime == 28000 && environment_.flashMaxTime == 34000));
    require(std::isfinite(environment_.flashTime) && environment_.flashTime >= 0 &&
            environment_.flashTime <= 45 && std::isfinite(environment_.flashGoal) &&
            environment_.flashGoal >= 0 && environment_.flashGoal <= 34 &&
            std::isfinite(environment_.flashAnimationTime) &&
            environment_.flashAnimationTime >= -1 && environment_.flashAnimationTime <= 1.02f);
    for (std::size_t layer = 0; layer < asteroidScroll_.size(); ++layer) {
        require(std::isfinite(asteroidScroll_[layer]) && asteroidScroll_[layer] >= 0 &&
                asteroidScroll_[layer] <= std::array<float, 3>{546, 388, 476}[layer]);
    }
    require(environment_.sunTicks >= 0 && environment_.pdsTarget >= 0 &&
            environment_.pdsTarget <= 2 && std::isfinite(environment_.pdsTime) &&
            environment_.pdsTime >= 0 && environment_.pdsTime <= 45 &&
            std::isfinite(environment_.pdsGoal) && environment_.pdsGoal >= 0 &&
            environment_.pdsGoal <= 34 && std::isfinite(environment_.pdsAmbient) &&
            environment_.pdsAmbient >= -1 && environment_.pdsAmbient <= 5 &&
            environment_.pdsQueue.size() <= 32 && environment_.pdsSmoke.size() <= 256 &&
            environment_.fleetShips.size() <= 256);
    for (const auto& smoke : environment_.pdsSmoke) {
        require(std::isfinite(smoke[0]) && std::isfinite(smoke[1]) && std::isfinite(smoke[2]) &&
                smoke[2] >= 0 && smoke[2] <= 1);
    }
    const auto validateDoors = [&](const ShipDefinition& shape,
                                   const auto& bases,
                                   const auto& damage,
                                   const auto& forced,
                                   const auto& levels,
                                   const auto& lastBases,
                                   const auto& broken) {
        const auto validId = [&](int id) {
            return id >= 0 && id < static_cast<int>(shape.doors.size());
        };
        for (const auto* values : {&bases, &lastBases}) {
            for (const auto& [id, base] : *values) {
                require(validId(id) && base >= 4 && base <= 50);
            }
        }
        for (const auto& [id, value] : damage) {
            require(validId(id) && std::isfinite(value) && value >= 0 && value <= 50);
        }
        for (const auto& [id, ticks] : forced) {
            require(validId(id) && ticks >= 0 && ticks <= 420);
        }
        for (const auto& [id, level] : levels) {
            require(validId(id) && level >= 0 && level <= 4);
        }
        for (int id : broken) {
            require(validId(id));
        }
    };
    validateDoors(ship_,
                  voyageDoorBaseHealth_,
                  voyageDoorDamage_,
                  voyageDoorForcedTicks_,
                  voyageDoorLevel_,
                  voyageDoorLastBaseHealth_,
                  voyageDoorLockdownBroken_);
    validateDoors(enemy_.ship,
                  enemy_.doorBaseHealth,
                  enemy_.doorDamage,
                  enemy_.doorForcedTicks,
                  enemy_.doorLevel,
                  enemy_.doorLastBaseHealth,
                  enemy_.doorLockdownBroken);
    for (const auto& [key, state] : voyageMelee_) {
        require(key >= 0 && key < 128 && std::isfinite(state[0]) && state[0] >= 0 &&
                state[0] <= 2 && std::isfinite(state[1]) && state[1] >= 0 && state[1] <= .65f &&
                (state[2] == 0 || state[2] == 1));
    }
    for (const auto& [beaconId, offers] : stores_) {
        require(beaconId >= 0 && beaconId < static_cast<int>(beacons.size()) && offers.size() < 64);
        for (const auto& offer : offers) {
            require(offer.cost >= 0 && offer.stock >= 0 && offer.category >= -1 &&
                    offer.category <= 4);
            if (offer.item.starts_with("system:")) {
                const int id = std::stoi(offer.item.substr(7));
                require(id >= 0 && id <= 15);
                if (!offer.bonus.empty()) {
                    require(catalog_.equipment(offer.bonus).kind == "drone");
                }
            } else if (offer.item.starts_with("crew:")) {
                (void)catalog_.appearance(offer.item.substr(5));
                if (!offer.bonus.empty()) {
                    require(offer.bonus.size() <= 4096);
                    const auto metadata = nlohmann::json::parse(offer.bonus);
                    for (const auto* field : {"name", "full_name"}) {
                        const auto name = metadata.at(field).get<std::string>();
                        require(!name.empty() && name.size() <= 256);
                    }
                    (void)metadata.at("male").get<bool>();
                    require(nativeValue<int>(metadata.at("appearance")) >= 0);
                    const auto skills = nativeValue<std::array<int, 6>>(metadata.at("skills"));
                    for (int tier : skills) {
                        require(tier >= 0 && tier <= 2);
                    }
                }
            } else if (offer.item != "fuel" && offer.item != "missiles" && offer.item != "parts" &&
                       offer.item != "repair" && offer.item != "unavailable") {
                (void)catalog_.equipment(offer.item);
            }
        }
    }
    for (const auto& beacon : beacons) {
        if (!beacon.savedShip.is_null()) {
            require(beacon.savedShip.is_object() &&
                    beacon.savedShip.size() ==
                        3U + (beacon.savedShip.contains("boarding_ai") ? 1U : 0U) +
                            (beacon.savedShip.contains("ship_seed") ? 1U : 0U) +
                            (beacon.savedShip.contains("auto_blueprint") ? 1U : 0U));
            if (beacon.savedShip.contains("auto_blueprint")) {
                // ShipEvent autoBlueprint (original-save export); empty when authored.
                require(beacon.savedShip.at("auto_blueprint").get<std::string>().size() <= 256);
            }
            if (beacon.savedShip.contains("boarding_ai")) {
                const int boarding = nativeValue<int>(beacon.savedShip.at("boarding_ai"));
                require(boarding >= 0 && boarding <= 2);
            }
            if (beacon.savedShip.contains("ship_seed")) {
                require(nativeValue<int>(beacon.savedShip.at("ship_seed")) >= -1);
            }
            const auto saved = beacon.savedShip.at("blueprint").get<ShipDefinition>();
            const auto& sourceShip = catalog_.ship(saved.id);
            nlohmann::json savedGeometry = saved;
            nlohmann::json sourceGeometry = sourceShip;
            // saveBeaconShip rewrites the list counts from live state, as
            // SaveToBlueprint 0x4a28a0 does; only static geometry must match.
            for (const auto* key : {"hull",
                                    "reactor",
                                    "missiles",
                                    "droneParts",
                                    "weapons",
                                    "drones",
                                    "weaponCount",
                                    "droneCount",
                                    "augments",
                                    "systems"}) {
                savedGeometry.erase(key);
                sourceGeometry.erase(key);
            }
            require(savedGeometry == sourceGeometry);
            require(saved.weaponCount >= 0 && saved.weaponCount <= 8 && saved.droneCount >= 0 &&
                    saved.droneCount <= 999);
            require(saved.systems.size() == sourceShip.systems.size());
            std::set<std::pair<int, int>> systemRooms;
            for (const auto& definition : saved.systems) {
                require(systemRooms.emplace(definition.id, definition.room).second);
                const auto original =
                    std::ranges::find_if(sourceShip.systems, [&](const auto& candidate) {
                        return candidate.id == definition.id && candidate.room == definition.room;
                    });
                require(original != sourceShip.systems.end());
                nlohmann::json savedSystem = definition;
                nlohmann::json originalSystem = *original;
                for (const auto* key : {"power", "installed", "slot", "direction"}) {
                    savedSystem.erase(key);
                    originalSystem.erase(key);
                }
                require(savedSystem == originalSystem);
                const auto shape = std::ranges::find_if(saved.rooms, [&](const auto& candidate) {
                    return candidate.id == definition.room;
                });
                require(shape != saved.rooms.end() && shape->w >= 35 && shape->h >= 35 &&
                        shape->w % 35 == 0 && shape->h % 35 == 0);
                require(definition.slot >= -2 && definition.slot < shape->w / 35 * (shape->h / 35));
                require(definition.direction.size() <= 16);
            }
            require(saved.hull > 0 && saved.hull <= 100 && saved.reactor >= 0 &&
                    saved.reactor <= 50);
            require(saved.weapons.size() <= static_cast<std::size_t>(saved.weaponSlots) + 1 &&
                    saved.drones.size() <= static_cast<std::size_t>(saved.droneSlots) &&
                    saved.augments.size() <= 8);
            require(saved.missiles >= 0 && saved.missiles <= 999 && saved.droneParts >= 0 &&
                    saved.droneParts <= 999);
            for (const auto& id : saved.weapons) {
                require(catalog_.equipment(id).kind == "weapon");
            }
            for (const auto& id : saved.drones) {
                require(catalog_.equipment(id).kind == "drone");
            }
            for (const auto& id : saved.augments) {
                require(catalog_.equipment(id).kind == "aug");
            }
            for (const auto& definition : saved.systems) {
                require(definition.maxPower >= -1 && definition.power >= 0 &&
                        definition.power <= (definition.maxPower >= 0
                                                 ? definition.maxPower
                                                 : catalog_.system(definition.name).maxPower));
            }
            require(beacon.savedShip.at("crew").is_array() &&
                    beacon.savedShip.at("crew").size() <= 16);
            for (const auto& row : beacon.savedShip.at("crew")) {
                const auto appearance = row.at("appearance").get<HangarCrew>();
                (void)catalog_.appearance(appearance.species);
                require(appearance.name.size() <= 256 && appearance.appearance >= 0);
                const auto skills = nativeValue<std::array<int, 6>>(row.at("skills"));
                const std::array<int, 6> caps = appearance.species == "human"
                                                    ? std::array<int, 6>{26, 26, 100, 116, 32, 14}
                                                    : std::array<int, 6>{30, 30, 110, 130, 36, 16};
                for (std::size_t i = 0; i < skills.size(); ++i) {
                    require(skills[i] >= 0 && skills[i] <= caps[i]);
                }
            }
            require(beacon.savedShip.at("encounter").get<std::string>().size() <= 256);
        }
        require(beacon.backgroundImage.size() < 512 && beacon.planetImage.size() < 512 &&
                beacon.planetX >= -1280 && beacon.planetX <= 2560 && beacon.planetY >= -720 &&
                beacon.planetY <= 1440 &&
                (beacon.planetRotation == 0 || beacon.planetRotation == 180));
        for (int id : beacon.neighbors) {
            require(id >= 0 && id < static_cast<int>(beacons.size()));
        }
        (void)events_.event(beacon.event);
    }
}

void Run::save(const std::string& path) {
    if (!open_ || defeated_) {
        throw std::runtime_error("no_live_run_to_save");
    }
    // Do not write a checkpoint which the loader would reject. This also
    // blocks saving during the hull-loss animation, when a living crew keeps
    // the Game Over event pending but the defeated ship is not resumable.
    ensureInventorySlots();
    validateCheckpoint();
    const std::filesystem::path destination(path);
    if (destination.filename() != "recomp-run-v1.json") {
        throw std::runtime_error("native_save_filename_required");
    }
    nlohmann::json data;
    checkpoint(data, false);
    data["format"] = "FTLRecomp-native-run";
    data["version"] = 1;
    const auto serialized = data.dump();
    if (serialized.size() > 16 * 1024 * 1024) {
        throw std::runtime_error("native_save_too_large");
    }
    const auto temporary = std::filesystem::path(path + ".tmp");
    const auto backup = std::filesystem::path(path + ".bak");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << serialized;
        output.flush();
        if (!output) {
            throw std::runtime_error("native_save_write_failed");
        }
    }
    // Keep the last good checkpoint across a failed replacement, including on Windows.
    if (std::filesystem::exists(destination)) {
        std::filesystem::copy_file(
            destination, backup, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(destination);
    }
    try {
        std::filesystem::rename(temporary, destination);
    } catch (...) {
        if (std::filesystem::exists(backup)) {
            std::filesystem::copy_file(
                backup, destination, std::filesystem::copy_options::overwrite_existing);
        }
        throw;
    }
}

void Run::load(const std::string& path) {
    if (std::filesystem::path(path).filename() != "recomp-run-v1.json" ||
        std::filesystem::file_size(path) > 16 * 1024 * 1024) {
        throw std::runtime_error("invalid_native_save_file");
    }
    std::ifstream input(path, std::ios::binary);
    auto data = nlohmann::json::parse(input);
    if (data.value("format", "") != "FTLRecomp-native-run" ||
        (!data.contains("version") || nativeValue<int>(data.at("version")) != 1)) {
        throw std::runtime_error("unsupported_native_save_version");
    }
    // Earlier native saves advanced independent streams. Their historical
    // interleaving cannot be recovered; continue from the saved voyage state.
    if (!data.contains("sessionRandom_")) {
        data["sessionRandom_"] = data.at("eventRandom_");
    }
    const auto migrateSystem = [&](nlohmann::json& definition,
                                   const ShipDefinition& authored,
                                   bool player) {
        if (definition.contains("bp")) {
            // Earlier native defaults reversed the original vertical directions.
            // Authored station directions and customized slots remain authoritative.
            if (player) {
                const int id = nativeValue<int>(definition.at("id"));
                const auto source = std::ranges::find_if(authored.systems, [&](const auto& row) {
                    return row.id == id && row.slot == -1;
                });
                if (source != authored.systems.end()) {
                    const auto physical = playerSystemDefinition(*source);
                    const auto oldDirection = id == 1 ? "UP" : "DOWN";
                    if ((id == 1 || id == 3 || id == 7 || id == 8) &&
                        nativeValue<int>(definition.at("slot")) == physical.slot &&
                        nativeValue<std::string>(definition.at("direction")) == oldDirection) {
                        definition["direction"] = physical.direction;
                    }
                }
            }
            return;
        }
        const int id = nativeValue<int>(definition.at("id"));
        const int roomId = nativeValue<int>(definition.at("room"));
        auto source = std::ranges::find_if(authored.systems, [&](const auto& candidate) {
            return candidate.id == id && candidate.room == roomId;
        });
        if (source == authored.systems.end()) {
            source = std::ranges::find_if(authored.systems, [&](const auto& candidate) {
                return candidate.id == id;
            });
        }
        if (source == authored.systems.end()) {
            throw std::runtime_error("invalid_legacy_system_definition");
        }
        definition["bp"] = source->bp;
        definition["maxPower"] = source->maxPower;
        const auto physical = player ? playerSystemDefinition(*source) : *source;
        definition["slot"] = physical.slot;
        definition["direction"] = physical.direction;
    };
    const auto migrateShip = [&](nlohmann::json& definition, bool player) {
        const auto& authored = catalog_.ship(nativeValue<std::string>(definition.at("id")));
        auto legacyImage = authored.imageRect;
        legacyImage[0] += authored.offsetX * 35;
        legacyImage[1] += authored.offsetY * 35;
        if (nativeValue<std::array<int, 4>>(definition.at("imageRect")) == legacyImage) {
            definition["imageRect"] = authored.imageRect;
        }
        for (const auto& [key, value] : std::initializer_list<std::pair<const char*, int>>{
                 {"maxCrew", authored.maxCrew},
                 {"weaponCount", authored.weaponCount},
                 {"droneCount", authored.droneCount}}) {
            if (!definition.contains(key)) {
                definition[key] = value;
            }
        }
        for (auto& system : definition.at("systems")) {
            migrateSystem(system, authored, player);
        }
    };
    migrateShip(data.at("ship_"), true);
    if (!data.at("enemy_").at("ship").at("id").get<std::string>().empty()) {
        migrateShip(data.at("enemy_").at("ship"), false);
    }
    if (data.contains("campaign_") && data.at("campaign_").contains("savedBoss") &&
        data.at("campaign_").at("savedBoss").is_object()) {
        migrateShip(data["campaign_"]["savedBoss"]["enemy"]["ship"], false);
    }
    for (auto& beacon : data.at("graph_").at("beacons_")) {
        if (beacon.contains("savedShip") && beacon.at("savedShip").is_object()) {
            migrateShip(beacon["savedShip"]["blueprint"], false);
        }
    }
    const auto migrateRuntimeSystems = [&](nlohmann::json& enemy, bool player) {
        auto& ship = player ? data.at("ship_") : enemy.at("ship");
        const auto id = nativeValue<std::string>(ship.at("id"));
        if (id.empty()) {
            return;
        }
        const auto& authored = catalog_.ship(id);
        auto& rows = player ? data.at("systems_") : enemy.at("systems");
        for (auto& row : rows) {
            migrateSystem(row.at("definition"), authored, player);
        }
    };
    migrateRuntimeSystems(data.at("enemy_"), true);
    migrateRuntimeSystems(data.at("enemy_"), false);
    if (data.contains("campaign_") && data.at("campaign_").contains("savedBoss") &&
        data.at("campaign_").at("savedBoss").is_object()) {
        migrateRuntimeSystems(data["campaign_"]["savedBoss"]["enemy"], false);
    }
    // Older native enemy constructors retained authored zero as a physical
    // system level. Original AddSystem 0x49ca00 constructs an installed
    // template with at least one bar, even when its authored level is zero.
    const auto restoreEnemyStartingBars = [](nlohmann::json& enemy) {
        for (auto& definition : enemy.at("ship").at("systems")) {
            if (nativeValue<bool>(definition.at("installed")) &&
                nativeValue<int>(definition.at("power")) == 0) {
                definition["power"] = 1;
            }
        }
        for (auto& row : enemy.at("systems")) {
            auto& definition = row.at("definition");
            if (nativeValue<bool>(definition.at("installed")) &&
                nativeValue<int>(definition.at("power")) == 0) {
                definition["power"] = 1;
                row["health"] = 1;
            }
        }
    };
    restoreEnemyStartingBars(data.at("enemy_"));
    if (data.contains("campaign_") && data.at("campaign_").contains("savedBoss") &&
        data.at("campaign_").at("savedBoss").is_object()) {
        restoreEnemyStartingBars(data["campaign_"]["savedBoss"]["enemy"]);
    }
    // Version-one saves made before per-system battery accounting have no
    // allocation field. Assign the minimum missing bars to keep their current
    // power legal; the original allocation order cannot be recovered.
    const auto restoreBatteryAllocation = [](nlohmann::json& rows, int reactor, bool active) {
        bool legacy = false;
        std::int64_t used = 0;
        for (auto& row : rows) {
            if (!row.contains("batteryPower")) {
                row["batteryPower"] = 0;
                legacy = true;
            }
            const int id = nativeValue<int>(row.at("definition").at("id"));
            if ((id < 6 || id > 8) && id != 12) {
                used += nativeValue<int>(row.at("power"));
            }
        }
        if (!legacy || !active) {
            return;
        }
        std::int64_t missing = std::max<std::int64_t>(0, used - reactor);
        for (auto it = rows.rbegin(); it != rows.rend() && missing > 0; ++it) {
            auto& row = *it;
            const int id = nativeValue<int>(row.at("definition").at("id"));
            if ((id >= 6 && id <= 8) || id == 12) {
                continue;
            }
            const auto bars = std::min<std::int64_t>(missing, nativeValue<int>(row.at("power")));
            row["batteryPower"] = bars;
            missing -= bars;
        }
    };
    restoreBatteryAllocation(data.at("systems_"),
                             nativeValue<int>(data.at("ship_").at("reactor")),
                             data.at("battery_").at("active").get<bool>());
    if (!data.contains("playerExplosionState_")) {
        // Historical gib RNG draws cannot be reconstructed from old checkpoints.
        // Keep an uninitialized state; the presentation migration is explicit.
        data["playerExplosionState_"] = CombatExplosion{};
    }
    auto& enemyData = data.at("enemy_");
    if (!enemyData.contains("lastEscapeHealth")) {
        enemyData["lastEscapeHealth"] = enemyData.at("hull");
    }
    const auto enemyDoorCount = enemyData.at("ship").at("doors").size();
    for (const auto* key : {"doors", "fakeDoors"}) {
        if (!enemyData.contains(key)) {
            enemyData[key] = std::vector<bool>(enemyDoorCount, false);
        }
    }
    for (const auto* key : {"doorFrames", "doorTimers"}) {
        if (!enemyData.contains(key)) {
            enemyData[key] = std::vector<float>(enemyDoorCount, 0.f);
        }
    }
    if (!data.contains("mindTargetOwner_")) {
        data["mindTargetOwner_"] = 1;
    }
    if (!enemyData.contains("boardingAi")) {
        const auto id = nativeValue<std::string>(enemyData.at("ship").at("id"));
        int boarding = id.starts_with("BOSS_3_") ? 2 : 0;
        if (enemyData.at("ship").contains("source")) {
            const auto source = nativeValue<std::string>(enemyData.at("ship").at("source"));
            if (!source.empty()) {
                const auto document = xmlResource(archive_, source);
                auto root = document.child("FTL");
                if (!root) {
                    root = document;
                }
                for (const auto ship : root.children("shipBlueprint")) {
                    if (std::string_view(ship.attribute("name").as_string()) == id) {
                        const std::string_view mode = ship.child_value("boardingAI");
                        boarding = mode == "invasion" ? 2 : mode == "sabotage" ? 1 : boarding;
                        break;
                    }
                }
            }
        }
        enemyData["boardingAi"] = boarding;
    }
    restoreBatteryAllocation(enemyData.at("systems"),
                             nativeValue<int>(enemyData.at("ship").at("reactor")),
                             enemyData.at("battery").at("active").get<bool>());
    if (!data.contains("devCrewLimit_")) {
        data["devCrewLimit_"] = 8;
    }
    if (!data.contains("devReactorLimit_")) {
        data["devReactorLimit_"] = 25;
    }
    if (!data.contains("hazard_")) {
        data["hazard_"] = "";
    }
    if (data.contains("nebulaScene_") && !data.at("nebulaScene_").contains("storm_")) {
        data["nebulaScene_"]["storm_"] =
            nativeValue<std::string>(data.at("hazard_")) == "ion_storm";
    }
    if (!data.contains("shieldExcessChargeHack_")) {
        data["shieldExcessChargeHack_"] = false;
    }
    // Version-one checkpoints saved before the drone dodge indicator existed carry
    // no damage messages; an empty list is the correct historical value.
    if (!data.contains("damageMessages_")) {
        data["damageMessages_"] = nlohmann::json::array();
    }
    // Version-one checkpoints from before destruction lifecycle state was
    // explicit stored only the enemy's remaining three-second timer.
    if (!data.contains("explosionDuration_")) {
        data["explosionDuration_"] = 3.f;
    }
    if (!data.contains("enemyExplosionActive_")) {
        data["enemyExplosionActive_"] =
            enemyData.value("hull", 1) <= 0 && data.value("explosion_", 0.f) > 0.f;
    }
    if (!data.contains("playerExplosionStartTick_")) {
        data["playerExplosionStartTick_"] = 0;
    }
    if (!data.contains("playerExplosionStarted_")) {
        data["playerExplosionStarted_"] = false;
    }
    // Additive v1 fields retain the historical empty/default state.
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, nlohmann::json>>{
             {"reactorPowerCap_", 64},
             {"reactorPowerLoss_", 0},
             {"reactorPowerDivide_", 1},
             {"teleportCharge_", 0.f},
             {"teleportArmed_", 0},
             {"teleportTarget_", -1},
             {"beamHitAt_",
              nlohmann::json::array({nlohmann::json::array(), nlohmann::json::array()})},
             {"beamKilledCrew_",
              nlohmann::json::array({nlohmann::json::array(), nlohmann::json::array()})}}) {
        if (!data.contains(key)) {
            data[key] = value;
        }
    }
    for (auto& projectile : data.at("projectiles_")) {
        if (!projectile.contains("crystalShard")) {
            projectile["crystalShard"] = false;
        }
        if (!projectile.contains("defenseHackingTargetId")) {
            projectile["defenseHackingTargetId"] = 0;
        }
        if (!projectile.contains("beamStepped")) {
            projectile["beamStepped"] = false;
        }
    }
    const auto migrateBombs = [&](nlohmann::json& rows) {
        for (auto& projectile : rows) {
            if (catalog_.equipment(nativeValue<std::string>(projectile.at("weapon"))).type !=
                "BOMB") {
                continue;
            }
            // Old native saves did not retain teleport-time barrier, dodge or
            // strip state. This timing conversion is an explicit approximation.
            const auto image = nativeValue<std::string>(projectile.at("projectileImage"));
            const float duration = image.empty() ? 0.f : catalog_.animation(image).duration;
            const float elapsed = std::max(0.f, nativeValue<float>(projectile.at("age")) - .35f);
            if (!projectile.contains("bombAnimationElapsed")) {
                projectile["bombAnimationElapsed"] = std::clamp(elapsed, 0.f, duration);
            }
            if (!projectile.contains("bombDelay")) {
                projectile["bombDelay"] =
                    std::clamp(.4f - std::max(0.f, elapsed - duration), -Time::step, .4f);
            }
            const int owner = nativeValue<int>(projectile.at("owner"));
            const int target = nativeValue<int>(projectile.at("targetShip"));
            const auto& source = owner == 0 ? data.at("ship_") : enemyData.at("ship");
            const auto augments = nativeValue<std::vector<std::string>>(source.at("augments"));
            const bool bypass = std::ranges::find(augments, "ZOLTAN_BYPASS") != augments.end();
            if (!projectile.contains("bombBypass")) {
                projectile["bombBypass"] = bypass;
            }
            if (!projectile.contains("bombSuperShield")) {
                const int barrier = nativeValue<int>(target == 0 ? data.at("superShields_")
                                                                 : enemyData.at("superShields"));
                projectile["bombSuperShield"] = owner != target && !bypass && barrier > 0;
            }
            if (!projectile.contains("bombMissTick")) {
                const auto now = nativeValue<std::uint64_t>(data.at("tick_"));
                const auto ageTicks = static_cast<std::uint64_t>(std::min(
                    static_cast<double>(now), static_cast<double>(elapsed) * Time::tickHz));
                projectile["bombMissTick"] =
                    nativeValue<bool>(projectile.at("missed")) ? now - ageTicks : 0;
            }
        }
    };
    migrateBombs(data.at("projectiles_"));
    if (data.contains("pendingRetaliation_")) {
        migrateBombs(data.at("pendingRetaliation_"));
    }
    const auto restoreAsteroidSounds = [](nlohmann::json& rows) {
        for (auto& projectile : rows) {
            if (!projectile.contains("asteroidShieldSound")) {
                projectile["asteroidShieldSound"] = "hitShield1";
            }
            if (!projectile.contains("asteroidHullSound")) {
                projectile["asteroidHullSound"] = "hitHull1";
            }
        }
    };
    restoreAsteroidSounds(data.at("projectiles_"));
    if (data.contains("environment_") && data.at("environment_").contains("pdsQueue")) {
        restoreAsteroidSounds(data.at("environment_").at("pdsQueue"));
    }
    for (auto* rows : {&data.at("systems_"), &data.at("enemy_").at("systems")}) {
        for (auto& row : *rows) {
            if (!row.contains("bonusPower")) {
                row["bonusPower"] = 0;
            }
            if (!row.contains("powerLimit")) {
                row["powerLimit"] = 8;
            }
            if (!row.contains("powerLoss")) {
                row["powerLoss"] = 0;
            }
        }
    }
    if (!data.at("graph_").contains("waitStart_")) {
        data["graph_"]["waitStart_"] = data["graph_"]["dangerX_"];
    }
    if (!data.at("graph_").contains("waitMove_")) {
        data["graph_"]["waitMove_"] = 0;
    }
    if (!data.at("graph_").contains("nebulaMap_")) {
        data["graph_"]["nebulaMap_"] = false;
    }
    for (auto& row : data.at("graph_").at("beacons_")) {
        if (!row.contains("savedShip")) {
            row["savedShip"] = nullptr;
        }
        if (!row.contains("eventXml")) {
            row["eventXml"] = "";
        }
        if (!row.contains("imageCached")) {
            row["imageCached"] = false;
        }
        for (const auto* key : {"backgroundImage", "planetImage"}) {
            if (!row.contains(key)) {
                row[key] = "";
            }
        }
        for (const auto* key : {"planetX", "planetY", "planetRotation"}) {
            if (!row.contains(key)) {
                row[key] = 0;
            }
        }
        for (const auto* key : {"store", "quest", "fleet", "distress"}) {
            if (!row.contains(key)) {
                row[key] = false;
            }
        }
    }
    SectorGraph legacySectors;
    RandomStream legacyRandom{0};
    nativeRead(data.at("sessionRandom_"), legacyRandom);
    legacySectors.generate(legacyRandom, [](SectorNode& node) {
        node.name = node.type == 2 ? "NEBULA_SECTOR" : "CIVILIAN_SECTOR";
    });
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, nlohmann::json>>{
             {"selectedVoyageCrewSeed_", -1},
             {"asteroidScroll_", std::array<float, 3>{}},
             {"usedVoyageEvents_", nlohmann::json::array()},
             {"upgradePurchases_", nlohmann::json::array()},
             {"sectorGraph_", legacySectors},
             {"sectorName_", "CIVILIAN_SECTOR"},
             {"sectorChoices_", nlohmann::json::array()},
             {"pendingQuests_", nlohmann::json::array()},
             {"pendingVoyageEffects_", nlohmann::json::array()},
             {"voyageBoarders_", nlohmann::json::array()},
             {"overflowEquipment_", ""},
             {"overflowAugment_", ""},
             {"environment_", Environment{}},
             {"distressWaitTicks_", 0},
             {"weaponSlots_", nlohmann::json::array()},
             {"droneSlots_", nlohmann::json::array()},
             {"eventBackground_", ""},
             {"eventPlanet_", ""},
             {"eventPlanetX_", 0},
             {"eventPlanetY_", 0},
             {"eventPlanetRotation_", 0},
             {"voyageDoorBaseHealth_", std::map<int, int>{}},
             {"voyageMelee_", std::map<int, std::array<float, 3>>{}},
             {"voyageDoorDamage_", std::map<int, float>{}},
             {"voyageDoorForcedTicks_", std::map<int, int>{}}}) {
        if (!data.contains(key)) {
            data[key] = value;
        }
    }
    // P10 separates hostile slots 0..63 from defender slots 64..127.
    // Older saves used 16 + defender index; preserve their attack cadence.
    const int meleeVersion = data.contains("voyage_melee_version")
                                 ? nativeValue<int>(data.at("voyage_melee_version"))
                             : std::ranges::any_of(data.at("crew_"),
                                                   [](const auto& row) {
                                                       return row.contains("enemyCrewOrigin");
                                                   })
                                 ? 2
                                 : 1;
    if (meleeVersion != 1 && meleeVersion != 2) {
        throw std::runtime_error("invalid_voyage_melee_version");
    }
    if (meleeVersion == 1) {
        for (auto& row : data.at("voyageMelee_")) {
            const int key = nativeValue<int>(row.at(0));
            if (key >= 16 && key < 80) {
                row.at(0) = key + 48;
            }
        }
    }
    data["voyage_melee_version"] = 2;
    // Additive P09 fields: preserve legacy attachment/clone state explicitly.
    if (!data.contains("campaign_")) {
        data["campaign_"] = Campaign{};
    }
    if (!data.contains("pendingSecretSector_")) {
        data["pendingSecretSector_"] = false;
    }
    if (!data.contains("secretSector_")) {
        data["secretSector_"] = false;
    }
    if (!data.contains("pendingRetaliation_")) {
        data["pendingRetaliation_"] = nlohmann::json::array();
    }
    if (!data.contains("cloneDeathTimer_")) {
        data["cloneDeathTimer_"] = 0.f;
    }
    if (!data.contains("cloneDuration_")) {
        data["cloneDuration_"] = 12.f;
    }
    if (data.at("enemyExplosionActive_").get<bool>() &&
        data.at("campaign_").at("fighting").get<bool>() &&
        nativeValue<int>(data.at("campaign_").at("stage")) < 3 &&
        nativeValue<int>(enemyData.at("hull")) <= 0) {
        data["explosionDuration_"] = 4.f;
        if (enemyData.contains("explosionState") &&
            enemyData.at("explosionState").at("initialized").get<bool>()) {
            enemyData["explosionState"]["jumpOut"] = true;
        }
    }
    const nlohmann::json droneDefaults = {{"bossSurge", false},
                                          {"bonusPower", 0},
                                          {"space", 0},
                                          {"health", 0.f},
                                          {"room", -1},
                                          {"targetRoom", -1},
                                          {"workProgress", 0.f},
                                          {"rebuildTimer", 0.f},
                                          {"delivered", false},
                                          {"deliveryTimer", 0.f},
                                          {"crewPath", nlohmann::json::array()},
                                          {"crewPathDoors", nlohmann::json::array()},
                                          {"crewPathIndex", 0},
                                          {"crewKills", 0}};
    const auto migrateDrone = [&droneDefaults](nlohmann::json& row) {
        // Earlier native Ion Intruders used ordinary boarder health and had no pulse state.
        if (nativeValue<std::string>(row.at("blueprint")) == "BOARDER_ION" &&
            !row.contains("ionPulseCooldown") && row.contains("health")) {
            row["health"] = std::min(nativeValue<float>(row.at("health")), 125.f);
        }
        for (const auto& [key, value] : droneDefaults.items()) {
            if (!row.contains(key)) {
                row[key] = value;
            }
        }
    };
    for (auto* rows : {&data.at("combatDrones_"), &data.at("enemy_").at("drones")}) {
        for (auto& row : *rows) {
            migrateDrone(row);
        }
    }
    if (data.contains("campaign_") && data.at("campaign_").contains("savedBoss") &&
        data.at("campaign_").at("savedBoss").is_object()) {
        for (auto& row : data["campaign_"]["savedBoss"]["enemy"]["drones"]) {
            migrateDrone(row);
        }
    }
    if (!data.contains("hackingDrone_")) {
        CombatDrone legacy{.blueprint = "DRONE_HACKING"};
        if (nativeValue<int>(data.at("hackingTarget_")) >= 0) {
            legacy.deployed = legacy.delivered = true;
            legacy.powered = true;
        }
        data["hackingDrone_"] = legacy;
    }
    migrateDrone(data.at("hackingDrone_"));
    if (!data.contains("hackingDroneSpace_")) {
        data["hackingDroneSpace_"] = data.at("hackingDrone_").at("deployed").get<bool>() ? 1 : 0;
    }
    if (!data.contains("hackingDroneId_")) {
        if (data.at("hackingDrone_").at("deployed").get<bool>()) {
            const auto next = nativeValue<std::uint64_t>(data.at("nextProjectile_"));
            if (next == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("invalid_checkpoint_projectile_id");
            }
            data["hackingDroneId_"] = next;
            data["nextProjectile_"] = next + 1;
        } else {
            data["hackingDroneId_"] = 0;
        }
    }
    for (auto* rows : {&data.at("combatWeapons_"), &data.at("enemy_").at("weapons")}) {
        for (auto& weapon : *rows) {
            if (!weapon.contains("bonusPower")) {
                weapon["bonusPower"] = 0;
            }
        }
    }
    for (auto* rows : {&data.at("systems_"), &data.at("enemy_").at("systems")}) {
        for (auto& sys : *rows) {
            if (sys.contains("artillery") && !sys.at("artillery").contains("bonusPower")) {
                sys["artillery"]["bonusPower"] = 0;
            }
        }
    }
    Run staged(renderer_, archive_, audio_);
    staged.weaponAnimationExplosions(catalog_.weaponAnimationExplosions());
    staged.checkpoint(data, true);
    if (!staged.overflowEquipment_.empty() &&
        catalog_.equipment(staged.overflowEquipment_).kind == "aug") {
        staged.overflowAugment_ = std::move(staged.overflowEquipment_);
        staged.overflowEquipment_.clear();
        staged.overflowFactory_ = {};
    }
    staged.ensureInventorySlots();
    staged.validateCheckpoint();
    if (!staged.campaign_.savedBoss.is_null()) {
        const auto& saved = staged.campaign_.savedBoss;
        if (!saved.is_object() || saved.size() != 2 || !saved.contains("enemy") ||
            !saved.contains("hazards")) {
            throw std::runtime_error("invalid_campaign_boss_snapshot");
        }
        auto bossData = data;
        bossData["enemy_"] = saved.at("enemy");
        if (!bossData["enemy_"].contains("lastEscapeHealth")) {
            bossData["enemy_"]["lastEscapeHealth"] = bossData["enemy_"].at("hull");
        }
        bossData["enemyHazards_"] = saved.at("hazards");
        bossData["campaign_"]["savedBoss"] = nullptr;
        bossData["campaign_"]["fighting"] = true;
        bossData["hostile_"] = true;
        bossData["jumpTicks_"] = 0;
        Run bossStaged(renderer_, archive_, audio_);
        bossStaged.weaponAnimationExplosions(catalog_.weaponAnimationExplosions());
        bossStaged.checkpoint(bossData, true);
        bossStaged.validateCheckpoint();
    }
    // Validate in isolation before changing the live run or its XML-node owners.
#define FTL_RUN_FIELD(member) std::swap(member, staged.member);
#include "run_checkpoint_fields.hpp"
#undef FTL_RUN_FIELD
    std::swap(progressionRun_, staged.progressionRun_);
    std::swap(profileRunId_, staged.profileRunId_);
    std::swap(profileRunStarted_, staged.profileRunStarted_);
    catalog_.setUsedCrewNames(crewNameHistory_);
    // WorldManager::LoadGame always runs BossShip::LoadBoss 0x405f30,
    // which clears the flagship's bAutomated; a crewless flagship then raises
    // BOSS_AUTOMATED again through GetSubEvent 0x406200.
    if (campaign_.active) {
        campaign_.automated = false;
    }
    // Newly restored actors/weapon boxes rebind their transient presentation
    // state; loading an already-charged weapon must not replay weaponReady.
    audioWeaponIds_.clear();
    audioWeaponReady_.clear();
    audioIncomingFire_ = false;
    resetEventChoiceDelay();
    choices_.swap(staged.choices_);
    restoredEvents_.swap(staged.restoredEvents_);
    finishUpgrades(true);
    panel_ = overflowEquipment_.empty() && overflowAugment_.empty() ? "" : "equipment";
    pausedShipPanel_.clear();
    dismissCrew_ = renameCrew_ = inventorySelection_ = pendingStoreOffer_ = pausedDismissCrew_ = -1;
    inventoryDragKind_.clear();
    inventoryDragIndex_ = -1;
    crewRenameText_.clear();
    crewRenameCaret_ = 0;
    crewRenameBlink_ = 0;
    gameOverCommand_ = -1;
    dragging_ = shift_ = forceAutofire_ = false;
    abilityAim_.clear();
    // A continued run opens paused, except mid-jump or mid-arrival:
    // CommandGui::KeyDown 0x5201f0 ignores the pause key while jumping, so a
    // paused jump could never be resumed.
    userPause_ = jumpTicks_ == 0 && arrivalTicks_ == 0;
    resourceDeltas_ = {};
    resourceDeltaTicks_ = {};
    // The ShipStatus resource messages and their pending comparison are
    // checkpoint fields: their next jitter draws from the shared stream.
    startSectorMusic();
    audio_.active(true);
}
} // namespace ftl
