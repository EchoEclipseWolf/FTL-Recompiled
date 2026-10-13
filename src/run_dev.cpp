#include "run.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace ftl {
namespace {
int integer(const nlohmann::json& request, const char* key, int minimum, int maximum) {
    if (!request.contains(key) || !request.at(key).is_number_integer()) {
        throw std::runtime_error(std::string("dev_integer_required:") + key);
    }
    const auto value = request.at(key).get<std::int64_t>();
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string("dev_integer_out_of_range:") + key);
    }
    return static_cast<int>(value);
}

bool passiveSystem(int id) {
    return (id >= 6 && id <= 8) || id == 12;
}

RunSystem* findById(std::vector<RunSystem>& systems, int id) {
    const auto found = std::ranges::find_if(systems, [id](const RunSystem& candidate) {
        return candidate.definition.id == id;
    });
    return found == systems.end() ? nullptr : &*found;
}
} // namespace

void Run::settleReactorPower() {
    // Diagnostic hazards use the same power budget as ordinary controls. Shed
    // whole weapon/drone slots before individual systems to keep slot state coherent.
    for (std::size_t index = weaponPower_.size(); index-- > 0 && availablePower() < 0;) {
        if (!weaponPower_[index]) {
            continue;
        }
        const int bars = catalog_.equipment(ship_.weapons[index]).power;
        weaponPower_[index] = weaponDesired_[index] = false;
        if (auto* weapons = system(3)) {
            weapons->power = std::max(0, weapons->power - bars);
            weapons->batteryPower = std::min(weapons->batteryPower, weapons->power);
        }
        combatWeapons_[index].clearTargets();
    }
    for (std::size_t index = combatDrones_.size(); index-- > 0 && availablePower() < 0;) {
        auto& drone = combatDrones_[index];
        if (!drone.powered) {
            continue;
        }
        const int bars = catalog_.equipment(drone.blueprint).power;
        drone.powered = false;
        drone.repowerPending = true;
        if (auto* drones = system(4)) {
            drones->power = std::max(0, drones->power - bars);
            drones->batteryPower = std::min(drones->batteryPower, drones->power);
        }
    }
    for (int pass = 0; pass < 2 && availablePower() < 0; ++pass) {
        for (auto it = systems_.rbegin(); it != systems_.rend() && availablePower() < 0; ++it) {
            auto& s = *it;
            const int id = s.definition.id;
            if (passiveSystem(id) || id == 3 || id == 4 || s.power <= 0 ||
                (pass == 0 && s.power <= s.batteryPower)) {
                continue;
            }
            const int bars = id == 0 && s.power >= 2 ? 2 : 1;
            s.power -= bars;
            s.batteryPower = std::min(s.batteryPower, s.power);
        }
    }
    if (const auto* shields = system(0)) {
        shieldBubbles_ = std::min(shieldBubbles_, shields->power / 2);
    }
    reconcilePower();
}

nlohmann::json Run::devCommand(const nlohmann::json& request) {
    if (!open_ || defeated_) {
        throw std::runtime_error("active_run_required");
    }
    const auto op = request.at("op").get<std::string>();
    if (op == "resources") {
        const auto before = resourceAmounts();
        std::array<int*, 4> targets{&scrap_, &fuel_, &missiles_, &parts_};
        constexpr std::array<const char*, 4> names{"scrap", "fuel", "missiles", "drone_parts"};
        auto next = before;
        const int nextHull =
            request.contains("hull") ? integer(request, "hull", 1, ship_.hull) : hull_;
        for (const char* section : {"set", "add"}) {
            if (!request.contains(section)) {
                continue;
            }
            if (!request.at(section).is_object()) {
                throw std::runtime_error("dev_resources_object_required");
            }
            for (auto it = request.at(section).begin(); it != request.at(section).end(); ++it) {
                if (std::ranges::none_of(names, [&](const char* name) {
                        return it.key() == name;
                    })) {
                    throw std::runtime_error("dev_unknown_resource");
                }
            }
        }
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (request.contains("set") && request.at("set").contains(names[index])) {
                next[index] = integer(request.at("set"), names[index], 0, 99999);
            }
            if (request.contains("add") && request.at("add").contains(names[index])) {
                const int delta = integer(request.at("add"), names[index], -99999, 99999);
                next[index] = std::clamp(next[index] + delta, 0, 99999);
            }
        }
        for (std::size_t index = 0; index < targets.size(); ++index) {
            *targets[index] = next[index];
        }
        hull_ = nextHull;
        noteResourceChanges(before);
    } else if (op == "grant") {
        const auto id = request.at("item").get<std::string>();
        const auto& item = catalog_.equipment(id);
        const auto destination = request.value("to", std::string("equip"));
        if (destination == "cargo") {
            if (cargo_.size() >= 4) {
                throw std::runtime_error("dev_cargo_full");
            }
            cargo_.push_back(id);
            cargoFactories_.push_back(makeInventoryFactory(id));
        } else if (destination == "equip" && item.kind == "weapon" &&
                   ship_.weapons.size() < std::size_t(ship_.weaponSlots)) {
            ship_.weapons.push_back(id);
            weaponPower_.push_back(false);
            weaponDesired_.push_back(false);
            combatWeapons_.emplace_back();
            initializeWeaponFactory(combatWeapons_.back(), item, eventRandom_);
        } else if (destination == "equip" && item.kind == "drone" &&
                   ship_.drones.size() < std::size_t(ship_.droneSlots)) {
            ship_.drones.push_back(id);
            CombatDrone drone{.blueprint = id};
            initializeCrewDroneAnimation(drone, eventRandom_);
            combatDrones_.push_back(std::move(drone));
        } else if (destination == "equip" && item.kind == "aug" && ship_.augments.size() < 3 &&
                   std::ranges::find(ship_.augments, id) == ship_.augments.end()) {
            ship_.augments.push_back(id);
        } else {
            throw std::runtime_error("dev_item_has_no_slot");
        }
    } else if (op == "crew") {
        if (crew_.size() >= std::size_t(devCrewLimit_)) {
            throw std::runtime_error("dev_crew_limit");
        }
        const auto species = request.at("species").get<std::string>();
        catalog_.appearance(species);
        const auto name = request.value("name", species);
        if (name.empty() || name.size() > 64) {
            throw std::runtime_error("dev_invalid_crew_name");
        }
        const int roomId =
            request.contains("room") ? integer(request, "room", 0, 255) : ship_.rooms.front().id;
        const auto& target = room(roomId);
        const int slots = (target.w / 35) * (target.h / 35);
        int slot = -1;
        for (int candidate = 0; candidate < slots; ++candidate) {
            if (std::ranges::none_of(crew_, [&](const RunCrew& c) {
                    return c.health > 0 && c.room == roomId && c.slot == candidate;
                })) {
                slot = candidate;
                break;
            }
        }
        if (slot < 0) {
            throw std::runtime_error("dev_room_full");
        }
        RunCrew member;
        member.appearance = {name, species, 0, roomId, slot, name};
        member.room = member.targetRoom = member.savedRoom = roomId;
        member.slot = member.targetSlot = member.savedSlot = slot;
        member.x = float(target.x + slot % (target.w / 35) * 35 + 17);
        member.y = float(target.y + slot / (target.w / 35) * 35 + 17);
        initializeCrewAnimation(member, eventRandom_);
        assignPhysicalCrewOrder(member);
        crew_.push_back(member);
        previousCrew_.clear();
        updateManning();
    } else if (op == "system") {
        const int id = integer(request, "system", 0, 15);
        const int level = integer(request, "level", 1, 16);
        auto* installed = system(id);
        auto definition = std::ranges::find_if(ship_.systems, [id](const auto& candidate) {
            return candidate.id == id;
        });
        if (definition == ship_.systems.end()) {
            throw std::runtime_error("dev_system_unavailable_on_ship");
        }
        const int maximum = definition->maxPower >= 0 ? definition->maxPower
                                                      : catalog_.system(definition->name).maxPower;
        if (level > maximum) {
            throw std::runtime_error("dev_system_level_exceeds_blueprint");
        }
        if (installed && (id == 3 || id == 4) && level < installed->power) {
            throw std::runtime_error("dev_depower_slots_before_downgrade");
        }
        if (!installed) {
            definition->installed = true;
            definition->power = level;
            *definition = playerSystemDefinition(*definition);
            systems_.push_back({*definition, passiveSystem(id) ? level : 0, level});
            if (id == 5) {
                systems_.back().damagingEffectTimes =
                    initializeMedbayAnimations(room(definition->room), eventRandom_);
            }
            if (id == 11 && !definition->weapon.empty()) {
                initializeWeaponFactory(systems_.back().artillery,
                                        catalog_.equipment(definition->weapon),
                                        eventRandom_);
            } else if (id == 15) {
                hackingDrone_ = CombatDrone{.blueprint = "DRONE_HACKING"};
                initializeCrewDroneAnimation(hackingDrone_, eventRandom_);
            }
        } else {
            const int difference = level - installed->definition.power;
            installed->definition.power = definition->power = level;
            installed->health = std::clamp(installed->health + difference, 0, level);
            installed->power = std::clamp(
                installed->power + (passiveSystem(id) ? difference : 0), 0, installed->health);
            installed->restorePower = std::min(installed->restorePower, installed->health);
        }
        settleReactorPower();
        updateManning();
    } else if (op == "reactor") {
        ship_.reactor = integer(request, "level", 0, devReactorLimit_);
        settleReactorPower();
    } else if (op == "limit") {
        const auto name = request.at("name").get<std::string>();
        const int value = integer(request, "value", 1, 999);
        if (name == "hull") {
            ship_.hull = value;
            hull_ = std::min(hull_, value);
        } else if (name == "crew" && value <= 32 && value >= int(crew_.size())) {
            devCrewLimit_ = value;
        } else if (name == "reactor" && value <= 64 && value >= ship_.reactor) {
            devReactorLimit_ = value;
        } else if (name == "weapon_slots" && value <= 4 && value >= int(ship_.weapons.size())) {
            ship_.weaponSlots = value;
        } else if (name == "drone_slots" && value <= 3 && value >= int(ship_.drones.size())) {
            ship_.droneSlots = value;
        } else {
            throw std::runtime_error("dev_invalid_limit");
        }
    } else if (op == "enemy") {
        constexpr std::array candidates{
            "REBEL_SKINNY", "REBEL_TRANSPORT", "AUTO_ASSAULT", "AUTO_BASIC"};
        auto id = request.value("ship", std::string("random"));
        if (id == "random") {
            id = candidates[combatRandom_.next() % candidates.size()];
        } else {
            catalog_.ship(id);
        }
        beginCombat(id);
        encounter_ = id;
        encounterAutoBlueprint_ = id;
        // A developer ship has the default ShipEvent seed (0).
        encounterShipSeed_ = 0;
        event_ = mapOpen_ = false;
        panel_.clear();
        userPause_ = true;
        jumpTicks_ = arrivalTicks_ = 0;
    } else if (op == "event") {
        const auto id = request.at("event").get<std::string>();
        const auto node = events_.event(id);
        const int beacon = request.contains("beacon")
                               ? integer(request, "beacon", 0, int(graph_.beacons().size()) - 1)
                               : -1;
        if (request.value("start", true)) {
            enterEvent(node);
            openingEvent_ = false;
            panel_.clear();
            mapOpen_ = false;
        }
        if (beacon >= 0) {
            graph_.setEvent(beacon, id);
        }
    } else if (op == "store") {
        if (!docked()) {
            throw std::runtime_error("dev_store_requires_safe_beacon");
        }
        std::vector<StoreOffer> offers;
        if (request.contains("offers")) {
            offers = request.at("offers").get<std::vector<StoreOffer>>();
            if (offers.size() < 4 || offers.size() > 16 || (offers.size() - 4) % 3 != 0) {
                throw std::runtime_error("dev_invalid_store_layout");
            }
            const std::array<std::string, 4> supplies{"fuel", "missiles", "parts", "repair"};
            for (std::size_t index = 0; index < offers.size(); ++index) {
                const auto& offer = offers[index];
                if (offer.cost < 0 || offer.cost > 10000 || offer.stock < 0 ||
                    offer.stock > 10000) {
                    throw std::runtime_error("dev_invalid_store_offer");
                }
                if (index < 4) {
                    if (offer.item != supplies[index]) {
                        throw std::runtime_error("dev_invalid_store_supply");
                    }
                } else if (offer.item.starts_with("crew:")) {
                    (void)catalog_.appearance(offer.item.substr(5));
                } else if (offer.item.starts_with("system:")) {
                    const int id = std::stoi(offer.item.substr(7));
                    if (id < 0 || id > 15 || id == 11) {
                        throw std::runtime_error("dev_invalid_store_system");
                    }
                } else if (offer.item != "unavailable") {
                    (void)catalog_.equipment(offer.item);
                }
            }
        }
        graph_.setStore(graph_.current(), true);
        if (request.value("regenerate", false)) {
            stores_.erase(graph_.current());
        }
        if (!offers.empty()) {
            stores_[graph_.current()] = std::move(offers);
        }
        openStore();
    } else if (op == "event_xml") {
        const auto xml = request.at("xml").get<std::string>();
        if (xml.size() > 262144) {
            throw std::runtime_error("dev_event_xml_too_large");
        }
        pugi::xml_document document;
        if (!document.load_string(xml.c_str()) ||
            std::string_view(document.document_element().name()) != "event") {
            throw std::runtime_error("dev_invalid_event_xml");
        }
        std::vector<std::string> used;
        const auto prepared = materializeEvent(document.document_element(),
                                               eventRandom_,
                                               advanced_,
                                               used,
                                               [this](pugi::xml_node event, RandomStream& random) {
                                                   freezeVoyageEvent(event, random);
                                               });
        restoredEvents_.emplace_back();
        if (!restoredEvents_.back().load_string(prepared.c_str())) {
            throw std::runtime_error("dev_event_materialization_failed");
        }
        enterEvent(restoredEvents_.back().document_element());
        openingEvent_ = false;
        panel_.clear();
        mapOpen_ = false;
    } else if (op == "hazard") {
        const auto type = request.at("type").get<std::string>();
        if (type != "ion_storm" && type != "nebula" && type != "sun" && type != "asteroid" &&
            type != "pulsar" && type != "pds" && type != "none") {
            throw std::runtime_error("dev_unknown_hazard");
        }
        if (type == "pds") {
            restoredEvents_.emplace_back();
            auto event = restoredEvents_.back().append_child("event");
            auto environment = event.append_child("environment");
            environment.append_attribute("type") = "PDS";
            const auto target = request.value("target", std::string("player"));
            if (target != "player" && target != "enemy" && target != "all") {
                throw std::runtime_error("dev_pds_target");
            }
            environment.append_attribute("target").set_value(target.c_str());
            if (request.value("fleet", false)) {
                event.append_child("fleet").text() = "rebel";
            }
            enterEvent(event);
        } else {
            setEnvironment(type == "none" ? "" : type);
        }
        if (type == "ion_storm") {
            settleReactorPower();
            enemy_.powerFixtureOverride = false;
            if (enemy_.present) {
                const auto budget = reactorCapacity(true) + batteryPower(true);
                const auto used = [&] {
                    int result = 0;
                    for (const auto& system : enemy_.systems) {
                        if (!passiveSystem(system.definition.id)) {
                            result += system.power;
                        }
                    }
                    return result;
                };
                for (std::size_t index = enemy_.powered.size(); index-- > 0 && used() > budget;) {
                    if (!enemy_.powered[index]) {
                        continue;
                    }
                    enemy_.powered[index] = false;
                    if (auto* weapons = findById(enemy_.systems, 3)) {
                        weapons->power = std::max(
                            0,
                            weapons->power - catalog_.equipment(enemy_.ship.weapons[index]).power);
                    }
                }
                for (std::size_t index = enemy_.drones.size(); index-- > 0 && used() > budget;) {
                    auto& drone = enemy_.drones[index];
                    if (!drone.powered) {
                        continue;
                    }
                    drone.powered = false;
                    if (auto* drones = findById(enemy_.systems, 4)) {
                        drones->power =
                            std::max(0, drones->power - catalog_.equipment(drone.blueprint).power);
                    }
                }
                for (auto& system : enemy_.systems) {
                    if (used() <= budget) {
                        break;
                    }
                    if (!passiveSystem(system.definition.id) && system.definition.id != 3 &&
                        system.definition.id != 4) {
                        const int bars = system.definition.id == 0 && system.power >= 2 ? 2 : 1;
                        system.power = std::max(0, system.power - bars);
                    }
                }
                if (auto* shields = findById(enemy_.systems, 0)) {
                    enemy_.shields = std::min(enemy_.shields, shields->power / 2);
                }
            }
        }
        eventId_ = type == "ion_storm" ? "DEV_ION_STORM" : "DEV_ENVIRONMENT";
        eventText_ = type == "ion_storm"
                         ? "An ion storm engulfs the beacon. Reactor output is halved."
                     : type == "nebula"   ? "The beacon lies inside a nebula."
                     : type == "sun"      ? "The beacon is dangerously close to a star."
                     : type == "asteroid" ? "The beacon lies inside an asteroid field."
                                          : "The environmental hazard has cleared.";
        choices_ = {{"Continue...", {}, true}};
        pendingResources_ = {};
        event_ = true;
        resetEventChoiceDelay();
        mapOpen_ = false;
        panel_.clear();
        openingEvent_ = false;
    } else if (op == "breach") {
        // Diagnostic: one Ship::BreachRandomHull 0x483390 draw on a named room.
        const auto side = request.at("side").get<std::string>();
        if (side != "player" && side != "enemy") {
            throw std::runtime_error("dev_invalid_side");
        }
        if (side == "enemy" && !enemy_.present) {
            throw std::runtime_error("dev_enemy_required");
        }
        const auto& layout = side == "player" ? ship_ : enemy_.ship;
        const int room = integer(request, "room", 0, 255);
        if (std::ranges::none_of(layout.rooms, [room](const auto& candidate) {
                return candidate.id == room;
            })) {
            throw std::runtime_error("dev_unknown_room");
        }
        breachRandomHull(side == "player" ? 0 : 1, room);
    } else if (op == "ion") {
        const auto side = request.at("side").get<std::string>();
        if (side != "player" && side != "enemy") {
            throw std::runtime_error("dev_invalid_side");
        }
        if (side == "enemy" && !enemy_.present) {
            throw std::runtime_error("dev_enemy_required");
        }
        const int id = integer(request, "system", 0, 15);
        const int stacks = integer(request, "stacks", 1, 5);
        auto& systems = side == "player" ? systems_ : enemy_.systems;
        const auto found = std::ranges::find_if(systems, [id](const auto& candidate) {
            return candidate.definition.id == id;
        });
        if (found == systems.end()) {
            throw std::runtime_error("dev_system_not_installed");
        }
        auto* target = &*found;
        applySystemIon(*target, stacks, side == "enemy");
        if (side == "player") {
            if (id == 0) {
                shieldBubbles_ = std::min(shieldBubbles_, target->power / 2);
            }
            settleReactorPower();
        } else {
            if (id == 0) {
                enemy_.shields = std::min(enemy_.shields, target->power / 2);
            }
            enemy_.powerFixtureOverride = false;
        }
    } else if (op == "export_original") {
        auto result = exportOriginalSave(request.value("folder", std::string()));
        result["op"] = op;
        return result;
    } else if (op == "original_save_roundtrip") {
        auto result = originalSaveRoundTrip(request.at("file").get<std::string>());
        result["op"] = op;
        return result;
    } else {
        throw std::runtime_error("dev_unknown_op");
    }
    return {{"op", op}, {"hazard", hazard_}};
}
} // namespace ftl
