#include "combat_skills.hpp"
#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace ftl {
std::string Run::materializeEvent(pugi::xml_node node,
                                  RandomStream& random,
                                  bool advanced,
                                  std::vector<std::string>& used,
                                  const std::function<void(pugi::xml_node, RandomStream&)>& freeze,
                                  bool ignoreUnique,
                                  int forceRandomIndex) const {
    return events_.materialize(node,
                               random,
                               advanced,
                               used,
                               freeze,
                               ignoreUnique,
                               forceRandomIndex,
                               [this](pugi::xml_node textNode) {
                                   return textNode.attribute("id")
                                              ? texts_.get(textNode.attribute("id").as_string(),
                                                           locale_)
                                              : std::string(textNode.child_value());
                               });
}

int Run::systemCapacity(const RunSystem& sys) {
    // ShipSystem::GetMaxPower 0x615cc0: temporary cap/loss never damage capacity.
    return std::max(0,
                    std::min(sys.health - sys.powerLoss,
                             sys.powerDivide != 1 ? sys.definition.power / 2
                             : sys.powerLimit > 7 ? sys.definition.power
                                                  : sys.powerLimit));
}

bool Run::eventRequirement(const std::string& requirement, int minimum, int maximum) const {
    // ShipObject::HasEquipment 0x45f840 returns list presence or an individual level.
    try {
        const auto& alternatives = catalog_.list(requirement);
        if (!alternatives.empty()) {
            int total = 0;
            for (const auto& id : alternatives) {
                if (id == requirement) {
                    continue;
                }
                for (const auto& sys : systems_) {
                    if (sys.definition.name == id) {
                        total += sys.definition.power;
                    }
                }
                total += static_cast<int>(std::ranges::count(ship_.augments, id));
                total += static_cast<int>(std::ranges::count(ship_.weapons, id));
                total += static_cast<int>(std::ranges::count(ship_.drones, id));
                total += static_cast<int>(std::ranges::count(cargo_, id));
                for (const auto& member : crew_) {
                    if (member.health > 0 && member.appearance.species == id) {
                        ++total;
                    }
                }
            }
            // HasEquipment 0x45f840 returns presence (0/1) for blueprint lists,
            // even when several alternatives or multiple copies are carried.
            const int present = total > 0 ? 1 : 0;
            return present >= minimum && present <= maximum;
        }
    } catch (const std::runtime_error&) {
        // An authored individual blueprint is not a blueprintList.
    }
    int level = 0;
    for (const auto& sys : systems_) {
        if (sys.definition.name == requirement) {
            level = sys.definition.power;
        }
    }
    for (const auto& member : crew_) {
        if (member.health > 0 && member.appearance.species == requirement) {
            ++level;
        }
    }
    level += static_cast<int>(std::ranges::count(ship_.augments, requirement));
    level += static_cast<int>(std::ranges::count(ship_.weapons, requirement));
    level += static_cast<int>(std::ranges::count(ship_.drones, requirement));
    level += static_cast<int>(std::ranges::count(cargo_, requirement));
    if (requirement == "scrap") {
        level = scrap_;
    }
    if (requirement == "fuel") {
        level = fuel_;
    }
    if (requirement == "missiles") {
        level = missiles_;
    }
    if (requirement == "drones") {
        level = parts_;
    }
    if (requirement == "reactor") {
        level = ship_.reactor;
    }
    return level >= minimum && level <= maximum;
}

float Run::crewMaximumHealth(std::string_view species) {
    // CrewMember720cf0, EnergyAlien723c80, CrystalAlien7260c0, RockAlien73ef50;
    // crew drones: BattleDrone 0x722c10 / BoarderDrone 0x7254e0 (150),
    // RepairDrone 0x723e10 (25), IonDrone 0x437560 (125).
    return species == "rock" || species == "battle"           ? 150.f
           : species == "crystal" || species == "boarder_ion" ? 125.f
           : species == "energy"                              ? 70.f
           : species == "repair"                              ? 25.f
                                                              : 100.f;
}

void Run::applyCloneSkillLoss(RunCrew& crew) {
    // CrewMember::Clone0x470650 removes10% of each full skill cap, at
    // least one point, bounded by current progress; all six skills participate.
    const bool human = crew.appearance.species == "human";
    const std::array<int, 6> caps = human ? std::array<int, 6>{26, 26, 100, 116, 32, 14}
                                          : std::array<int, 6>{30, 30, 110, 130, 36, 16};
    // Clone also starts skillUp[1][skill] for each skill that had progress.
    auto reduce = [&](int& progress, int cap, std::size_t skill) {
        if (progress > 0) {
            crew.skillUpRemaining[6 + skill] = 1.f;
        }
        progress = std::max(0, progress - std::max(1, cap / 10));
    };
    for (std::size_t index = 0; index < 3; ++index) {
        reduce(crew.manningSkillProgress[index], caps[index], index);
        crew.manningSkill[index] = combatSkillTier(crew.manningSkillProgress[index], caps[index]);
    }
    reduce(crew.weaponSkillProgress, caps[3], 3);
    crew.weaponSkill = combatSkillTier(crew.weaponSkillProgress, caps[3]);
    reduce(crew.repairSkillProgress, caps[4], 4);
    crew.repairSkill = combatSkillTier(crew.repairSkillProgress, caps[4]);
    reduce(crew.combatSkillProgress, caps[5], 5);
    crew.combatSkill = combatSkillTier(crew.combatSkillProgress, caps[5]);
}

bool Run::placeVoyageCrew(
    RunCrew& member, int preferredRoom, bool intruder, bool enemy, Enemy* stagedEnemy) {
    // SetRoom46f800 empties the previous faction reservation before choosing
    // GetAvailableRoom485bb0/GetAvailableRoomSlot485860/GetEmptySlot46a0f0.
    const auto& physicalEnemy = stagedEnemy ? *stagedEnemy : enemy_;
    const auto& layout = enemy ? physicalEnemy.ship : ship_;
    const auto& physicalSystems = enemy ? physicalEnemy.systems : systems_;
    const int physicalShip = enemy ? 1 : 0;
    const auto shapeFor = [&](int id) -> const RoomDefinition* {
        const auto found = std::ranges::find(layout.rooms, id, &RoomDefinition::id);
        return found == layout.rooms.end() ? nullptr : &*found;
    };
    const auto emptySlot = [&](int id) {
        const auto* shape = shapeFor(id);
        if (!shape) {
            return -1;
        }
        const int columns = std::max(1, shape->w / 35);
        const int tiles = columns * std::max(1, shape->h / 35);
        const auto occupied = [&](int slot) {
            const bool blocked = std::ranges::any_of(physicalSystems, [&](const auto& system) {
                const auto& definition = system.definition;
                return definition.room == id && (definition.id == 5 || definition.id == 13) &&
                       definition.slot != -2 && slot == (definition.slot < 0 ? 1 : definition.slot);
            });
            const auto crewOccupied = [&](const RunCrew& other, int ship, bool side, bool foreign) {
                return &other != &member && !other.dead && !other.outOfGame &&
                       ship == physicalShip && side == intruder &&
                       (foreign ? other.boardTargetRoom : other.targetRoom) == id &&
                       (foreign ? other.boardTargetSlot : other.targetSlot) == slot;
            };
            const bool playerOccupied =
                !stagedEnemy && std::ranges::any_of(crew_, [&](const auto& other) {
                    return crewOccupied(other,
                                        int(other.intruder),
                                        other.intruder != other.controlled,
                                        other.intruder);
                });
            const bool enemyOccupied =
                std::ranges::any_of(physicalEnemy.crew, [&](const auto& other) {
                    return !other.intruder && (stagedEnemy || physicalEnemy.present) &&
                           crewOccupied(other, 1, other.controlled, false);
                });
            const bool boarderOccupied =
                !stagedEnemy && std::ranges::any_of(voyageBoarders_, [&](const auto& other) {
                    return crewOccupied(other, 0, !other.controlled, false);
                });
            const auto droneOccupied = [&](const auto& drones, int owner) {
                return std::ranges::any_of(drones, [&](const CombatDrone& drone) {
                    const auto& type = catalog_.equipment(drone.blueprint).type;
                    const int ship = drone.crewPhysicalShip;
                    if ((type != "BOARDER" && type != "BATTLE" && type != "REPAIR") ||
                        drone.crewRoom < 0 || drone.crewDead || ship != physicalShip ||
                        bool(owner != ship) != intruder || drone.targetRoom != id) {
                        return false;
                    }
                    const auto point =
                        drone.crewPath.empty() ? drone.crewPosition : drone.crewPath.back();
                    const int column = std::clamp(int((point.x - shape->x) / 35), 0, columns - 1);
                    const int row = std::clamp(
                        int((point.y - shape->y) / 35), 0, std::max(1, shape->h / 35) - 1);
                    return row * columns + column == slot;
                });
            };
            return blocked || playerOccupied || enemyOccupied || boarderOccupied ||
                   (!stagedEnemy && droneOccupied(combatDrones_, 0)) ||
                   droneOccupied(physicalEnemy.drones, 1);
        };
        const auto station = std::ranges::find_if(physicalSystems, [&](const auto& system) {
            const auto& definition = system.definition;
            return definition.room == id && definition.id != 5 && definition.id != 13 &&
                   definition.slot >= 0;
        });
        if (!intruder && station != physicalSystems.end() && station->definition.slot < tiles &&
            !occupied(station->definition.slot)) {
            return station->definition.slot;
        }
        for (int slot = 0; slot < tiles; ++slot) {
            if (!occupied(slot)) {
                return slot;
            }
        }
        return -1;
    };
    std::vector<int> candidates;
    std::vector<const RoomDefinition*> orderedRooms;
    orderedRooms.reserve(layout.rooms.size());
    for (const auto& shape : layout.rooms) {
        orderedRooms.push_back(&shape);
    }
    std::ranges::sort(orderedRooms, {}, &RoomDefinition::id);
    const auto* origin = shapeFor(preferredRoom);
    if (origin) {
        candidates.push_back(preferredRoom);
        // GetConnectedRooms45bb10 preserves the authored connecting-door order.
        for (const auto& door : layout.doors) {
            const int adjacent = door.roomA == preferredRoom   ? door.roomB
                                 : door.roomB == preferredRoom ? door.roomA
                                                               : -1;
            if (adjacent >= 0) {
                candidates.push_back(adjacent);
            }
        }
        int nearest = -1;
        int nearestDistance = std::numeric_limits<int>::max();
        for (const auto* shape : orderedRooms) {
            if (emptySlot(shape->id) < 0) {
                continue;
            }
            const float distance =
                std::hypot(float(shape->x + shape->w / 2 - origin->x - origin->w / 2),
                           float(shape->y + shape->h / 2 - origin->y - origin->h / 2));
            // The original stores the winning distance in an integer.
            if (nearest < 0 || float(nearestDistance) > distance) {
                nearest = shape->id;
                nearestDistance = int(distance);
            }
        }
        if (nearest >= 0) {
            candidates.push_back(nearest);
        }
    } else {
        // Preferred=-1 tries the rooms connected to outside before ID order.
        if (preferredRoom == -1) {
            for (const auto& door : layout.doors) {
                const int adjacent = door.roomA == -1   ? door.roomB
                                     : door.roomB == -1 ? door.roomA
                                                        : -1;
                if (adjacent >= 0) {
                    candidates.push_back(adjacent);
                }
            }
        }
        for (const auto* shape : orderedRooms) {
            candidates.push_back(shape->id);
        }
    }
    for (int id : candidates) {
        const int slot = emptySlot(id);
        if (slot < 0) {
            continue;
        }
        const auto* shape = shapeFor(id);
        member.room = member.targetRoom = id;
        member.slot = member.targetSlot = slot;
        member.appearance.room = id;
        member.appearance.slot = slot;
        member.x = float(shape->x + 35 * (slot % std::max(1, shape->w / 35)) + 17);
        member.y = float(shape->y + 35 * (slot / std::max(1, shape->w / 35)) + 17);
        member.path.clear();
        member.newGoal = false;
        member.lastDoor = member.blockingDoor = -1;
        return true;
    }
    return false;
}

void Run::freezeVoyageEvent(pugi::xml_node node, RandomStream& random) {
    // Native staging record for eagerly generated EventGenerator::CreateEvent
    // rewards. Keep runtime-dependent removal victims out of this record.
    // CreateEvent generates explicit crew/equipment before autoReward.
    for (const auto* kindName : {"crewMember", "weapon", "drone", "augment", "autoReward"}) {
        for (auto child : node.children(kindName)) {
            const std::string kind = child.name();
            const bool crew = kind == "crewMember" && child.attribute("amount").as_int(1) > 0;
            if (kind != "autoReward" && kind != "weapon" && kind != "drone" && kind != "augment" &&
                !crew) {
                continue;
            }
            if (child.attribute("_nativeVoyage")) {
                continue;
            }
            pugi::xml_document document;
            auto event = document.append_child("event");
            event.append_copy(child);
            EventResources resources;
            const auto effects = stageVoyageEffects(event, random, resources);
            const nlohmann::json frozen{{"scrap", resources.scrap},
                                        {"fuel", resources.fuel},
                                        {"missiles", resources.missiles},
                                        {"drones", resources.drones},
                                        {"effects", effects}};
            child.append_attribute("_nativeVoyage").set_value(frozen.dump().c_str());
        }
    }
}

nlohmann::json Run::stageVoyageEffects(pugi::xml_node node,
                                       RandomStream& random,
                                       EventResources& resources,
                                       bool includeImages) {
    auto effects = nlohmann::json::array();
    const auto draw = [&](int low, int high) {
        if (low > high || low < -10000 || high > 10000) {
            throw std::runtime_error("invalid_voyage_effect_range");
        }
        return low + int(random.next() % std::uint32_t(high - low + 1));
    };
    const auto selectEquipment = [&](const std::string& kind) {
        const auto pool = catalog_.rewardEquipmentPool(kind, advanced_, sectorRarities());
        if (pool.empty()) {
            throw std::runtime_error("empty_voyage_equipment_pool");
        }
        std::vector<int> totals(pool.size() + 1);
        for (std::size_t i = 1; i <= pool.size(); ++i) {
            totals[i] = pool[i - 1].second;
        }
        for (std::size_t i = pool.size(); i > 1; --i) {
            totals[i / 2] += totals[i];
        }
        int value = int(random.next() % std::uint32_t(totals[1])) + 1;
        std::size_t i = 1;
        while (value > pool[i - 1].second) {
            value -= pool[i - 1].second;
            i *= 2;
            if (value > totals[i]) {
                value -= totals[i];
                ++i;
            }
        }
        return pool[i - 1].first;
    };
    for (auto child : node.children()) {
        const std::string kind = child.name();
        if (kind == "img" && !includeImages) {
            continue;
        }
        if (child.attribute("_nativeVoyage")) {
            const auto frozen = nlohmann::json::parse(child.attribute("_nativeVoyage").value());
            resources.scrap += frozen.at("scrap").get<int>();
            resources.fuel += frozen.at("fuel").get<int>();
            resources.missiles += frozen.at("missiles").get<int>();
            resources.drones += frozen.at("drones").get<int>();
            for (const auto& effect : frozen.at("effects")) {
                if (child.attribute("_nativeClearedBasicResources").as_bool() &&
                    effect.at("kind") == "equipment") {
                    const auto& item = catalog_.equipment(effect.at("id").get<std::string>());
                    if (item.kind == "weapon" || item.kind == "drone") {
                        continue;
                    }
                }
                effects.push_back(effect);
            }
            continue;
        }
        if (kind == "autoReward") {
            const std::string tier = child.attribute("level").as_string("LOW");
            const int level = tier == "LOW" ? 0 : tier == "MED" ? 1 : tier == "HIGH" ? 2 : 3;
            const auto oldEnemy = enemy_;
            const auto oldRandom = eventRandom_;
            const bool sharedRandom = eventRandom_.sharesStateWith(random);
            eventRandom_ = random;
            EventResources reward;
            std::string item;
            try {
                reward = generateCombatReward(child.child_value(), level);
                item = enemy_.rewardItem;
                random = eventRandom_;
            } catch (...) {
                enemy_ = oldEnemy;
                eventRandom_ = oldRandom;
                throw;
            }
            enemy_ = oldEnemy;
            if (!sharedRandom) {
                eventRandom_ = oldRandom;
            }
            resources.scrap += reward.scrap;
            resources.fuel += reward.fuel;
            resources.missiles += reward.missiles;
            resources.drones += reward.drones;
            if (!item.empty()) {
                effects.push_back({{"kind", "equipment"}, {"id", item}});
            }
        } else if (kind == "weapon" || kind == "drone" || kind == "augment") {
            std::string id = child.attribute("name").as_string();
            if (id == "RANDOM") {
                id = selectEquipment(kind == "augment" ? "aug" : kind);
            }
            // GetWeaponBlueprint0x5e6580 samples named lists (WEAPONS_CRYSTAL).
            // Native nested-list support extends that with a bounded traversal;
            // duplicate entries retain their authored selection weight.
            for (int depth = 0; !catalog_.containsEquipment(id); ++depth) {
                if (depth >= 64) {
                    throw std::runtime_error("recursive_equipment_blueprint_list");
                }
                const auto& entries = catalog_.list(id);
                if (entries.empty()) {
                    throw std::runtime_error("empty_equipment_blueprint_list");
                }
                id = entries[random.next() % entries.size()];
            }
            if (catalog_.equipment(id).kind != (kind == "augment" ? "aug" : kind)) {
                throw std::runtime_error("event_equipment_kind_mismatch: " + id);
            }
            effects.push_back({{"kind", "equipment"}, {"id", id}});
        } else if (kind == "crewMember" || kind == "removeCrew") {
            const int amount = kind == "removeCrew" ? -1 : child.attribute("amount").as_int(1);
            if (amount < 0) {
                const bool cloneable = child.attribute("_nativeCloneable")
                                           ? child.attribute("_nativeCloneable").as_bool()
                                       : kind == "removeCrew"
                                           ? std::string_view(child.child_value("clone")) == "true"
                                           : child.attribute("cloneable").as_bool(false);
                const auto cloneTextNode = child.child("_nativeCloneText")
                                               ? child.child("_nativeCloneText")
                                           : kind == "removeCrew" ? child.child("text")
                                                                  : pugi::xml_node{};
                const std::string cloneText =
                    cloneTextNode.attribute("id")
                        ? texts_.get(cloneTextNode.attribute("id").value(), locale_)
                        : cloneTextNode.child_value();
                // GetCrewList45ddc0(creation owner0, includeNonVictory=false)
                // and SelectRandomCrew498520 retain clone-ready dead actors.
                // This creation-owner roster excludes drones; health alone
                // would wrongly omit crew waiting for the clone bay.
                auto living = crewRoster();
                const std::string species = child.attribute("class").as_string();
                auto preferred = living;
                std::erase_if(preferred, [&](std::size_t index) {
                    return !species.empty() && species != "random" &&
                           crew_[index].appearance.species != species;
                });
                if (!preferred.empty()) {
                    living = std::move(preferred);
                }
                for (int i = 0; i < -amount && !living.empty(); ++i) {
                    if (selectedVoyageCrewSeed_ < 0) {
                        selectedVoyageCrewSeed_ = random.next();
                    }
                    RandomStream selector(static_cast<std::uint32_t>(selectedVoyageCrewSeed_));
                    const auto selected = selector.next() % living.size();
                    random = selector;
                    effects.push_back({{"kind", "removeCrew"},
                                       {"id", living[selected]},
                                       {"cloneable", cloneable},
                                       {"clone_text", cloneText},
                                       {"traitor", child.attribute("traitor").as_bool(false)}});
                    living.erase(living.begin() + static_cast<std::ptrdiff_t>(selected));
                    if (cloneable && system(13)) {
                        break;
                    }
                }
            } else {
                std::string authoredName;
                if (child.attribute("id")) {
                    authoredName = texts_.get(child.attribute("id").as_string(), locale_);
                } else if (std::string_view(child.child_value()).find_first_not_of(" \r\n\t") !=
                           std::string_view::npos) {
                    authoredName = child.child_value();
                }
                // One generated crew blueprint is copied for the authored amount.
                nlohmann::json generated;
                for (int i = 0; i < amount; ++i) {
                    if (i > 0) {
                        effects.push_back(generated);
                        continue;
                    }
                    const auto oldEnemy = enemy_;
                    const auto oldRandom = eventRandom_;
                    const bool sharedRandom = eventRandom_.sharesStateWith(random);
                    eventRandom_ = random;
                    const std::string species = child.attribute("class").as_string();
                    try {
                        stageCombatCrewReward(
                            species == "random" ? std::string{} : species, true, authoredName);
                    } catch (...) {
                        enemy_ = oldEnemy;
                        eventRandom_ = oldRandom;
                        throw;
                    }
                    random = eventRandom_;
                    nlohmann::json row{{"kind", "crew"},
                                       {"species", enemy_.rewardCrewSpecies},
                                       {"name", enemy_.rewardCrewName},
                                       {"full_name", enemy_.rewardCrewFullName},
                                       {"male", enemy_.rewardCrewMale},
                                       {"appearance", enemy_.rewardCrewAppearance}};
                    constexpr std::array names{
                        "pilot", "engines", "shields", "weapons", "repair", "combat"};
                    constexpr std::array<int, 6> caps{30, 30, 110, 130, 36, 16};
                    std::array<int, 6> progress{};
                    bool authoredSkills = false;
                    for (std::size_t skill = 0; skill < names.size(); ++skill) {
                        const auto attribute = child.attribute("all_skills")
                                                   ? child.attribute("all_skills")
                                                   : child.attribute(names[skill]);
                        if (attribute) {
                            const int level = attribute.as_int();
                            if (level < 0 || level > 2) {
                                throw std::runtime_error("unsupported_authored_crew_skill_level");
                            }
                            progress[skill] = level * caps[skill] / 2;
                            authoredSkills = true;
                        }
                    }
                    if (!authoredSkills) {
                        progress = randomCrewSkills(sector_ - 1, random);
                    }
                    row["skill_progress"] = progress;
                    generated = row;
                    effects.push_back(std::move(row));
                    enemy_ = oldEnemy;
                    if (!sharedRandom) {
                        eventRandom_ = oldRandom;
                    }
                }
            }
        } else if (kind == "img") {
            nlohmann::json row{{"kind", "img"}};
            for (const auto* attribute : {"back", "planet"}) {
                if (!child.attribute(attribute)) {
                    continue;
                }
                const std::string name = child.attribute(attribute).as_string();
                if (name == "NONE") {
                    row[attribute] = "";
                    continue;
                }
                std::vector<std::string> images;
                for (auto image : events_.imageList(name).children("img")) {
                    images.emplace_back(std::string("img/") + image.child_value());
                }
                if (images.empty()) {
                    const std::string direct = name.starts_with("img/") ? name : "img/" + name;
                    if (archive_.contains(direct)) {
                        images.push_back(direct);
                    }
                }
                if (images.empty()) {
                    row[attribute] = "";
                    continue;
                }
                const auto& current =
                    std::string_view(attribute) == "back" ? eventBackground_ : eventPlanet_;
                std::string selected;
                do {
                    selected = images[random.next() % images.size()];
                } while (selected == current && images.size() > 1);
                row[attribute] = selected;
                if (std::string_view(attribute) == "planet") {
                    const auto image = archive_.read(selected);
                    const int width =
                        image.size() >= 24 ? static_cast<int>(be32(image.data() + 16)) : 0;
                    const int height =
                        image.size() >= 24 ? static_cast<int>(be32(image.data() + 20)) : 0;
                    row["x"] = static_cast<int>(random.next() %
                                                static_cast<unsigned>(std::max(1, 1280 - width)));
                    row["y"] = static_cast<int>(random.next() %
                                                static_cast<unsigned>(std::max(1, 720 - height)));
                    row["rotation"] = 180 * static_cast<int>(random.next() % 2);
                }
            }
            effects.push_back(std::move(row));
        } else if (kind == "damage") {
            const std::string systemName = child.attribute("system").as_string();
            // CreateEvent copies EventDamage descriptors verbatim. Its room
            // draw belongs to PossibleDamage after enemy/store construction.
            effects.push_back({{"kind", kind},
                               {"amount", child.attribute("amount").as_int()},
                               {"system", systemName},
                               {"effect", child.attribute("effect").as_string()}});
        } else if (kind == "upgrade" || kind == "system" || kind == "status" || kind == "quest" ||
                   kind == "store" || kind == "reveal_map" || kind == "distressBeacon" ||
                   kind == "repair" || kind == "removeItem" || kind == "remove" ||
                   kind == "secretSector") {
            nlohmann::json row{{"kind", kind == "remove" ? "removeItem" : kind}};
            for (auto attr : child.attributes()) {
                row[attr.name()] = attr.value();
            }
            effects.push_back(std::move(row));
        } else if (kind == "boarders") {
            const int amount =
                child.attribute("amount") && child.attribute("amount").as_int(-1) >= 0
                    ? child.attribute("amount").as_int()
                    : draw(child.attribute("min").as_int(1), child.attribute("max").as_int(1));
            const std::string species = child.attribute("class").as_string("random");
            if (amount > 0 && species != "random") {
                (void)catalog_.appearance(species);
            }
            effects.push_back({{"kind", kind},
                               {"amount", amount},
                               {"species", species},
                               {"breach", child.attribute("breach").as_bool(false)}});
        }
    }
    return effects;
}

std::string Run::commitVoyageEffects(std::string* locationText) {
    std::string followup;
    int damageTotal = 0;
    resourceFollowupText_.clear();
    std::vector<nlohmann::json> effects;
    for (auto& effect : pendingVoyageEffects_) {
        effects.push_back(std::move(effect));
    }
    pendingVoyageEffects_ = nlohmann::json::array();
    // ModifyResources5f0290 applies ResourceEvent fields in a fixed order,
    // independent of the order of authored XML tags.
    const auto resourceOrder = [&](const nlohmann::json& effect) {
        const auto kind = effect.at("kind").get<std::string>();
        if (kind == "equipment") {
            const auto& item = catalog_.equipment(effect.at("id").get<std::string>());
            return item.kind == "weapon" ? 0 : item.kind == "aug" ? 1 : 6;
        }
        if (kind == "removeItem") {
            return 2;
        }
        if (kind == "upgrade") {
            return 3;
        }
        if (kind == "system") {
            return 4;
        }
        if (kind == "crew" || kind == "crewSkills" || kind == "removeCrew") {
            return 5;
        }
        return 7;
    };
    std::stable_sort(effects.begin(), effects.end(), [&](const auto& left, const auto& right) {
        return resourceOrder(left) < resourceOrder(right);
    });
    for (const auto& row : effects) {
        const auto kind = row.at("kind").get<std::string>();
        if (kind == "equipment") {
            if (auto full = rewardEquipment(row.at("id").get<std::string>()); !full.empty()) {
                followup = std::move(full);
            }
        } else if (kind == "crew" || kind == "crewSkills") {
            if (kind == "crewSkills") {
                if (enemy_.rewardCrewPending) {
                    pendingVoyageEffects_.push_back(row);
                    continue;
                }
                const auto found = std::ranges::find_if(crew_, [&](const RunCrew& member) {
                    return member.appearance.name == row.at("name").get<std::string>() &&
                           member.appearance.species == row.at("species").get<std::string>();
                });
                if (found == crew_.end()) {
                    continue;
                }
            }
            if (enemy_.rewardCrewPending) {
                pendingVoyageEffects_.push_back(row);
                continue;
            }
            if (kind == "crew") {
                enemy_.rewardCrewSpecies = row.at("species");
                enemy_.rewardCrewName = row.at("name");
                enemy_.rewardCrewFullName = row.at("full_name");
                enemy_.rewardCrewAppearance = row.at("appearance");
                enemy_.rewardCrewColorsPrepared = true;
                enemy_.rewardCrewMale = row.value("male", true);
                enemy_.rewardCrewPending = true;
            }
            const auto oldSize = crew_.size();
            commitCombatCrewReward();
            if (enemy_.rewardCrewPending) {
                panel_ = "crew";
                auto skills = row;
                skills["kind"] = "crewSkills";
                pendingVoyageEffects_.push_back(std::move(skills));
            }
            if (crew_.size() > oldSize || kind == "crewSkills") {
                const auto found = std::ranges::find_if(crew_, [&](const RunCrew& candidate) {
                    return candidate.appearance.name == row.at("name").get<std::string>() &&
                           candidate.appearance.species == row.at("species").get<std::string>();
                });
                if (kind == "crewSkills" && found == crew_.end()) {
                    continue;
                }
                auto& member = kind == "crew" ? crew_.back() : *found;
                applyVoyageCrewSkills(member, row);
            }
        } else if (kind == "removeCrew") {
            const auto id = row.at("id").get<std::size_t>();
            if (id < crew_.size()) {
                auto& member = crew_[id];
                // ModifyResources5f0290 creates the clone-bay explanation
                // before checking whether the selected victim is cloneable.
                if (system(13) && !row.value("clone_text", std::string{}).empty()) {
                    followup = "CLONED";
                    resourceFollowupText_ = row.at("clone_text");
                }
                if (row.value("cloneable", false) && system(13)) {
                    resetCrewForClone(member);
                    member.intruder = false;
                    member.boardRoom = -1;
                    placeVoyageCrew(member, system(13)->definition.room, member.controlled);
                    combatSound({"cloneArrive"}, tick_);
                } else {
                    killCrew(member, true);
                    // Event KillCrew without cloneable is a permanent loss, unlike combat death.
                    std::erase(cloneQueue_, static_cast<int>(id));
                    if (row.value("traitor", false)) {
                        // CompleteShip::AddCrewMember constructs a new actor
                        // from the retained blueprint, with fresh body state.
                        RunCrew traitor;
                        traitor.appearance = member.appearance;
                        traitor.recordMale = member.recordMale;
                        traitor.manningSkill = member.manningSkill;
                        traitor.manningSkillProgress = member.manningSkillProgress;
                        traitor.weaponSkill = member.weaponSkill;
                        traitor.weaponSkillProgress = member.weaponSkillProgress;
                        traitor.repairSkill = member.repairSkill;
                        traitor.repairSkillProgress = member.repairSkillProgress;
                        traitor.combatSkill = member.combatSkill;
                        traitor.combatSkillProgress = member.combatSkillProgress;
                        initializeCrewAnimation(traitor, combatRandom_);
                        traitor.health = crewMaximumHealth(traitor.appearance.species);
                        placeVoyageCrew(traitor, member.room, true);
                        if (traitor.room >= 0) {
                            assignPhysicalCrewOrder(traitor);
                            voyageBoarders_.push_back(std::move(traitor));
                        }
                    }
                }
            }
        } else if (kind == "damage") {
            const int amount = row.at("amount");
            const int previousHull = hull_;
            hull_ = std::clamp(hull_ - amount, 0, ship_.hull);
            damageTotal += previousHull - hull_;
            // WorldManager::PossibleDamage -> ShipManager::DamageHull 0x490820.
            lastHullDamage_[0] = previousHull - hull_;
            int roomId = row.value("room", -1);
            if (row.contains("system")) {
                const auto systemName = row.at("system").get<std::string>();
                if (systemName == "random" && !systems_.empty()) {
                    roomId = systems_[combatRandom_.next() % systems_.size()].definition.room;
                } else if (systemName == "room" && !ship_.rooms.empty()) {
                    roomId = ship_.rooms[combatRandom_.next() % ship_.rooms.size()].id;
                } else {
                    for (const auto& sys : systems_) {
                        if (sys.definition.name == systemName) {
                            roomId = sys.definition.room;
                        }
                    }
                }
            }
            // PossibleDamage 0x5edb00 calls DamageSystem(room, Damage{
            // iSystemDamage amount}): each system there runs AddDamage(amount)
            // and a positive amount posts the light-red number at the room
            // centre.
            for (auto& sys : systems_) {
                if (sys.definition.room == roomId) {
                    addSystemDamage(sys, amount);
                    if (amount > 0) {
                        const auto& shape = room(roomId);
                        DamageMessage message{
                            0,
                            amount,
                            {float(shape.x + shape.w / 2), float(shape.y + shape.h / 2)},
                            tick_};
                        message.systemDamage = true;
                        pushCombatMessage(message);
                    }
                }
            }
            const std::string effect = row.at("effect");
            if (roomId >= 0) {
                const auto& shape = room(roomId);
                const int tiles = shape.w / 35 * (shape.h / 35);
                bool fire = effect == "fire" || effect == "all";
                bool breach = effect == "breach" || effect == "all";
                if (effect == "random") {
                    fire = (combatRandom_.next() & 1) != 0;
                    breach = !fire;
                }
                if (fire) {
                    startRoomFire(0, roomId, 2);
                }
                if (breach && tiles > 0) {
                    breachRandomHull(0, roomId);
                }
            }
        } else if (kind == "img") {
            applyVoyageImage(row);
        } else if (kind == "upgrade") {
            const auto name = row.value("system", std::string{});
            const int amount = std::stoi(row.value("amount", std::string("1")));
            if (amount <= 0) {
                continue;
            }
            if (name == "reactor") {
                ship_.reactor = std::clamp(ship_.reactor + amount, 0, 25);
            }
            for (auto& sys : systems_) {
                if (sys.definition.name == name || name == "all") {
                    const int level =
                        std::clamp(sys.definition.power + amount, 1, systemMaximum(sys));
                    if (sys.health > 0) {
                        sys.health += level - sys.definition.power;
                    }
                    sys.health = std::clamp(sys.health, 0, level);
                    sys.definition.power = level;
                }
            }
        } else if (kind == "system") {
            const auto name = row.value("name", std::string{});
            (void)installSystem(BlueprintCatalog::systemId(name));
        } else if (kind == "status") {
            const auto name = row.value("system", std::string{});
            const auto type = row.value("type", std::string{});
            const int amount = std::stoi(row.value("amount", std::string("0")));
            const auto target = row.value("target", std::string("player"));
            for (int shipIndex = 0; shipIndex < 2; ++shipIndex) {
                if ((shipIndex == 0 && target == "enemy") ||
                    (shipIndex == 1 && target == "player")) {
                    continue;
                }
                auto& entries = shipIndex == 0 ? systems_ : enemy_.systems;
                if (name == "reactor") {
                    auto& cap = shipIndex == 0 ? reactorPowerCap_ : enemy_.reactorPowerCap;
                    auto& loss = shipIndex == 0 ? reactorPowerLoss_ : enemy_.reactorPowerLoss;
                    auto& divisor =
                        shipIndex == 0 ? reactorPowerDivide_ : enemy_.reactorPowerDivide;
                    if (type == "limit") {
                        cap = amount;
                    } else if (type == "loss") {
                        loss = amount;
                    } else if (type == "divide") {
                        divisor = std::max(1, amount);
                        const int reactor = shipIndex == 0 ? ship_.reactor : enemy_.ship.reactor;
                        cap = (reactor + divisor - 1) / divisor;
                    } else if (type == "clear") {
                        cap = 64;
                        loss = 0;
                        divisor = 1;
                    }
                }
                const bool enemy = shipIndex == 1;
                for (auto& sys : entries) {
                    if (sys.definition.name == name || name == "all") {
                        if (type == "clear") {
                            // ShipSystem::ClearStatus 0x616dc0 repowers only when a
                            // cap below eight, a loss or a divider was in force.
                            const bool restricted =
                                sys.powerLimit <= 7 || sys.powerLoss > 0 || sys.powerDivide > 1;
                            // Native keeps 8 for the unrestricted 1000 (every cap test
                            // treats values above seven as the full level).
                            sys.powerLimit = 8;
                            sys.powerLoss = 0;
                            sys.powerDivide = 1;
                            if (restricted) {
                                checkSystemRepower(sys, enemy);
                            }
                            continue;
                        }
                        if (type == "limit") {
                            sys.powerLimit = amount;
                        } else if (type == "divide") {
                            sys.powerDivide = amount;
                            if (amount != 1) {
                                sys.powerLimit = sys.definition.power / 2;
                            }
                        } else if (type == "loss") {
                            sys.powerLoss = amount;
                        } else {
                            continue;
                        }
                        // SetPowerCap/SetPowerLoss/SetDividePower 0x616e40/0x616f30/
                        // 0x617020: virtual AddDamage(0), virtual CheckMaxPower, then
                        // CheckForRepower for everything but weapons and drones.
                        addSystemDamage(sys, 0, enemy);
                        checkSystemMaxPower(sys, enemy);
                        checkSystemRepower(sys, enemy);
                    }
                }
            }
        } else if (kind == "quest") {
            const bool added =
                addQuest(row.at("event"), row.value("nextSector", std::string{}) == "true");
            if (locationText) {
                *locationText += "\n\n" + texts_.get(added         ? "added_quest"
                                                     : sector_ > 6 ? "no_time"
                                                                   : "added_quest_sector",
                                                     locale_);
            }
        } else if (kind == "secretSector") {
            // CommandGui::SetSecretSector 0x51ce30 latches a request; OnLoop
            // consumes it only after event dispatch. Recruitment alone never
            // enters the hidden sector or awards its achievement.
            if (!secretSector_) {
                pendingSecretSector_ = true;
            }
        } else if (kind == "store") {
            graph_.setStore(graph_.current(), true);
            createStore();
            // CreateLocation 0x5f2be0 and UpdateLocation 0x5f57d0 both reach
            // WorldManager::CreateStore 0x5edda0, which opens the store window.
            storeOpenPending_ = true;
        } else if (kind == "reveal_map") {
            graph_.revealAll();
            mapRevealed_ = true;
        } else if (kind == "distressBeacon") {
            graph_.setDistress(graph_.current(), true);
        } else if (kind == "repair") {
            // Event.repair is a map label, not a hull or Store operation.
            graph_.setRepair(graph_.current(), true);
        } else if (kind == "removeItem") {
            removeEquippedItem(row.value("name", std::string{}));
        } else if (kind == "boarders") {
            // WorldManager::AddBoarders5ee030 gates the entire group once;
            // CompleteShip407a60 generates actors at application, not event construction.
            const int amount = row.value("amount", 1); // migrate an already-staged legacy actor
            // The gate reads ShipManager::iIntruderCount, which also counts
            // mind-controlled residents and powered boarding drones.
            if (amount <= 0 || shipIntruderCount(0) > 14) {
                continue;
            }
            int roomId = row.contains("room")
                             ? nativeValue<int>(row.at("room"))
                             : ship_.rooms[eventRandom_.next() % ship_.rooms.size()].id;
            for (int actor = 0; actor < amount; ++actor) {
                const bool legacy = row.contains("name");
                const auto name = legacy ? std::pair{row.at("name").get<std::string>(), true}
                                         : catalog_.unusedCrewName(locale_, {}, [&] {
                                               return eventRandom_.next();
                                           });
                const auto oldEnemy = enemy_;
                std::string species = row.at("species");
                int appearance = row.value("appearance", 0);
                std::string shortName = name.first;
                if (!legacy) {
                    if (species != "random") {
                        (void)catalog_.unusedCrewName(locale_, {}, [&] {
                            return eventRandom_.next();
                        });
                    }
                    stageCombatCrewReward(
                        species == "random" ? std::string{} : species, false, name.first);
                    species = enemy_.rewardCrewSpecies;
                    shortName = enemy_.rewardCrewName;
                    int stride = species == "human" ? 2 : 1;
                    for (const auto& layer : catalog_.appearance(species).layers) {
                        if (!layer.empty()) {
                            appearance +=
                                static_cast<int>(eventRandom_.next() % layer.size()) * stride;
                            stride *= static_cast<int>(layer.size());
                        }
                    }
                    if (species == "human" && !name.second) {
                        ++appearance;
                    }
                }
                enemy_ = oldEnemy;
                RunCrew boarder;
                boarder.appearance = {shortName, species, appearance, -1, 0, name.first};
                boarder.recordMale = name.second;
                boarder.health = crewMaximumHealth(species);
                placeVoyageCrew(boarder, roomId, true);
                if (boarder.room < 0) {
                    continue;
                }
                roomId = boarder.room;
                boarder.savedRoom = roomId;
                boarder.savedSlot = boarder.slot;
                // CompleteShip::AddBoarders 0x407a60 teleports a non-breach
                // party in (CrewMember::StartTeleportArrive).
                if (!row.value("breach", false)) {
                    startTeleportArrival(boarder);
                }
                assignPhysicalCrewOrder(boarder);
                voyageBoarders_.push_back(std::move(boarder));
                refreshCrewNames();
            }
            if (row.value("breach", false)) {
                breachRandomHull(0, roomId);
                // DamageHull(1, false) does nothing while the ship is jumping.
                if (jumpTicks_ <= 0) {
                    const int previousHull = hull_;
                    hull_ = std::max(0, hull_ - 1);
                    lastHullDamage_[0] = previousHull - hull_;
                }
            }
            if (locationText && superShields_ > 0) {
                *locationText += "\n\n" + texts_.get("intruder_energy", locale_);
            }
        }
    }
    reconcilePower();
    if (enemy_.present) {
        reconcilePower(true);
    }
    updateManning();
    if (damageTotal > 0) {
        audio_.sound("eventDamage");
    }
    return followup;
}

void Run::applyVoyageImage(const nlohmann::json& row) {
    if (row.contains("back")) {
        eventBackground_ = row.at("back");
    }
    if (row.contains("planet")) {
        eventPlanet_ = row.at("planet");
        eventPlanetX_ = row.value("x", 0);
        eventPlanetY_ = row.value("y", 0);
        eventPlanetRotation_ = row.value("rotation", 0);
    }
}

void Run::endVoyageBoarding() {
    const auto alive = [](const RunCrew& member) {
        return member.health > 0;
    };
    if (!voyageBoarders_.empty() && std::ranges::none_of(voyageBoarders_, alive) &&
        !enemy_.present) {
        hostile_ = false;
    }
    reconcilePower();
}
} // namespace ftl
