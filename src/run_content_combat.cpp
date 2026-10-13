#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace ftl {
std::map<std::string, int> Run::sectorRarities() const {
    std::map<std::string, int> rarities;
    for (const auto& node : sectorDescription().child("rarityList").children("blueprint")) {
        rarities[node.attribute("name").as_string()] = node.attribute("rarity").as_int();
    }
    return rarities;
}

void Run::crystalRetaliation(int owner, SDL_FPoint location) {
    // ShipManager::CheckCrystalAugment 0x495760: each positive hull hit
    // rolls CRYSTAL_SHARDS, emits CRYSTAL_1 with hull damage 1 and an
    // explicit shard flag. It neither spends missiles nor counts as firing.
    if (!enemy_.present) {
        return;
    }
    const auto& source = owner == 0 ? ship_ : enemy_.ship;
    const auto& destination = owner == 0 ? enemy_.ship : ship_;
    float chance = 0;
    for (const auto& id : source.augments) {
        if (id == "CRYSTAL_SHARDS") {
            chance += catalog_.equipment(id).value;
        }
    }
    if (chance <= 0 || destination.rooms.empty() ||
        float(combatRandom_.next() % 100) >= chance * 100.f) {
        return;
    }
    const auto& weapon = catalog_.equipment("CRYSTAL_1");
    const auto& target = destination.rooms[combatRandom_.next() % destination.rooms.size()];
    CombatProjectile shard;
    shard.id = nextProjectile_++;
    shard.owner = shard.space = owner;
    shard.targetShip = 1 - owner;
    shard.room = target.id;
    shard.weapon = weapon.id;
    shard.projectileImage = weapon.projectileImage;
    shard.crystalShard = true;
    shard.damageBonus = 1 - weapon.damage;
    shard.target = {float(target.x + target.w / 2), float(target.y + target.h / 2)};
    const float angle = float(combatRandom_.next() % 360) * std::numbers::pi_v<float> / 180.f;
    // CheckCrystalAugment constructs a Missile at the struck point, even
    // though CRYSTAL_1 is authored as a laser. Its carrier owner becomes -1;
    // keep the physical origin here and derive that carrier in collisions.
    shard.entryAngle = -1;
    shard.position = shard.previous = location;
    shard.direction = {std::cos(angle), std::sin(angle)};
    shard.speed = (weapon.projectileSpeed > 0 ? weapon.projectileSpeed : 35.f) * 16.f;
    initializeProjectileExplosion(shard, weapon);
    // Collision callbacks hold references into projectiles_. Append only after
    // that update completes, so retaliation cannot invalidate its source hit.
    pendingRetaliation_.push_back(std::move(shard));
    combatSound(weapon.launchSounds, tick_, true);
}

void Run::assignCrewDroneId(CombatDrone& drone) {
    if (drone.blueprint.empty() || drone.crewActorId != 0) {
        return;
    }
    const auto& type = catalog_.equipment(drone.blueprint).type;
    if (type != "REPAIR" && type != "BATTLE" && type != "BOARDER") {
        return;
    }
    if (nextCrewDroneId_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("crew_drone_identity_exhausted");
    }
    drone.crewActorId = nextCrewDroneId_++;
}

void Run::assignSpaceDroneId(CombatDrone& drone) {
    if (drone.blueprint.empty() || drone.spaceActorId != 0) {
        return;
    }
    const auto& type = catalog_.equipment(drone.blueprint).type;
    if (type != "DEFENSE" && type != "COMBAT" && type != "BOARDER" && type != "SHIP_REPAIR" &&
        type != "SHIELD" && type != "HACKING") {
        return;
    }
    if (nextSpaceDroneId_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("space_drone_identity_exhausted");
    }
    drone.spaceActorId = nextSpaceDroneId_++;
    drone.spacePendingArrival = true;
}

std::vector<CombatDrone*> Run::spaceDronesForOwner(int owner) {
    std::vector<CombatDrone*> result;
    const auto append = [&](CombatDrone& drone) {
        if (drone.spaceActorId != 0) {
            result.push_back(&drone);
        }
    };
    append(owner == 0 ? hackingDrone_ : enemy_.hackingDrone);
    for (auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
        append(drone);
    }
    if (owner == 1) {
        for (auto& drone : enemy_.droneTrash) {
            append(drone);
        }
    }
    if (owner == 0) {
        for (auto& factory : cargoFactories_) {
            append(factory.drone);
        }
        append(overflowFactory_.drone);
        for (auto& drone : retiredCrewDrones_) {
            append(drone);
        }
        for (auto& drone : retiredSpaceDrones_) {
            append(drone);
        }
    }
    std::ranges::stable_sort(result, {}, [](const auto* drone) {
        return drone->spaceActorId;
    });
    return result;
}

std::vector<const CombatDrone*> Run::spaceDronesForOwner(int owner) const {
    std::vector<const CombatDrone*> result;
    const auto append = [&](const CombatDrone& drone) {
        if (drone.spaceActorId != 0) {
            result.push_back(&drone);
        }
    };
    append(owner == 0 ? hackingDrone_ : enemy_.hackingDrone);
    for (const auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
        append(drone);
    }
    if (owner == 1) {
        for (const auto& drone : enemy_.droneTrash) {
            append(drone);
        }
    }
    if (owner == 0) {
        for (const auto& factory : cargoFactories_) {
            append(factory.drone);
        }
        append(overflowFactory_.drone);
        for (const auto& drone : retiredCrewDrones_) {
            append(drone);
        }
        for (const auto& drone : retiredSpaceDrones_) {
            append(drone);
        }
    }
    std::ranges::stable_sort(result, {}, [](const auto* drone) {
        return drone->spaceActorId;
    });
    return result;
}

std::vector<std::pair<int, CombatDrone*>> Run::registeredSpaceDrones() {
    std::vector<std::pair<int, CombatDrone*>> result;
    for (int owner : {0, 1}) {
        for (auto* drone : spaceDronesForOwner(owner)) {
            if (drone->spaceRegisteredOrder != 0) {
                result.emplace_back(owner, drone);
            }
        }
    }
    std::ranges::stable_sort(result, {}, [](const auto& actor) {
        return actor.second->spaceRegisteredOrder;
    });
    return result;
}

std::vector<std::pair<int, const CombatDrone*>> Run::registeredSpaceDrones() const {
    std::vector<std::pair<int, const CombatDrone*>> result;
    for (int owner : {0, 1}) {
        for (const auto* drone : spaceDronesForOwner(owner)) {
            if (drone->spaceRegisteredOrder != 0) {
                result.emplace_back(owner, drone);
            }
        }
    }
    std::ranges::stable_sort(result, {}, [](const auto& actor) {
        return actor.second->spaceRegisteredOrder;
    });
    return result;
}

void Run::registerSpaceDrones(int owner, bool initial) {
    if (initial && std::ranges::find(spaceShipOwners_, owner) == spaceShipOwners_.end()) {
        spaceShipOwners_.push_back(owner);
    }
    // AddShip41a380 registers the complete existing ship list and
    // GetSpaceDrones4954e0 clears its arrival queue. Later OnLoop41c940
    // consumes precisely one FIFO GetNewDroneArrivals495240 per ship.
    for (auto* drone : spaceDronesForOwner(owner)) {
        if (!initial && !drone->spacePendingArrival) {
            continue;
        }
        if (drone->spaceRegisteredOrder == 0) {
            if (nextSpaceDroneOrder_ == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("space_drone_registry_exhausted");
            }
            drone->spaceRegisteredOrder = nextSpaceDroneOrder_++;
        }
        drone->spacePendingArrival = false;
        if (!initial) {
            break;
        }
    }
}

void Run::clearSpaceDroneRegistry() {
    // SpaceManager::Clear41a000 removes pointers, not the ship's actors.
    spaceShipOwners_.clear();
    for (int owner : {0, 1}) {
        for (auto* drone : spaceDronesForOwner(owner)) {
            drone->spaceRegisteredOrder = 0;
            drone->spacePendingArrival = false;
        }
    }
}

void Run::rebaseVoyageCrewReferences(int previousEnemyCount, int newEnemyCount) {
    // Native owner1 ordinary references encode home roster followed by the
    // independent voyage visitors. Replacing the home ship must not retarget
    // those surviving physical actors merely because the new roster differs.
    const auto remap = [&](int index) {
        const int visitor = index - previousEnemyCount;
        return visitor >= 0 && std::size_t(visitor) < voyageBoarders_.size()
                   ? newEnemyCount + visitor
                   : -1;
    };
    const auto target = [&](auto& actor) {
        if (actor.attackTargetOwner != 1 || actor.attackTargetDrone) {
            return;
        }
        actor.attackTargetIndex = remap(actor.attackTargetIndex);
        if (actor.attackTargetIndex < 0) {
            actor.attackTargetOwner = -1;
            actor.attackTargetDroneId = 0;
        }
    };
    for (auto& member : crew_) {
        target(member);
    }
    for (auto& member : voyageBoarders_) {
        target(member);
    }
    for (int owner : {0, 1}) {
        for (auto* drone : physicalCrewDrones(owner)) {
            target(*drone);
        }
    }
    if (mindTargetOwner_ == 1 && mindTarget_ >= 0) {
        const int index = remap(mindTarget_);
        if (index < 0) {
            releasePlayerMind();
        } else {
            mindTarget_ = index;
        }
    }
    remapMindQueues(1, remap);
}

std::vector<CombatDrone*> Run::physicalCrewDrones(int owner) {
    std::vector<CombatDrone*> result;
    const auto append = [&](CombatDrone& drone) {
        if (drone.blueprint.empty()) {
            return;
        }
        const auto& type = catalog_.equipment(drone.blueprint).type;
        if (type == "REPAIR" || type == "BATTLE" || type == "BOARDER") {
            result.push_back(&drone);
        }
    };
    for (auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
        append(drone);
    }
    if (owner == 1) {
        for (auto& drone : enemy_.droneTrash) {
            append(drone);
        }
    }
    if (owner == 0) {
        // RemoveDrone6127e0 transfers equipment ownership without deleting
        // CrewFactory's physical body. PermanentDeath73de70 remains false.
        for (auto& factory : cargoFactories_) {
            append(factory.drone);
        }
        append(overflowFactory_.drone);
        for (auto& drone : retiredCrewDrones_) {
            append(drone);
        }
    }
    std::ranges::stable_sort(result, {}, [](const auto* drone) {
        return drone->physicalCrewOrder;
    });
    return result;
}

std::vector<const CombatDrone*> Run::physicalCrewDrones(int owner) const {
    std::vector<const CombatDrone*> result;
    const auto append = [&](const CombatDrone& drone) {
        if (drone.blueprint.empty()) {
            return;
        }
        const auto& type = catalog_.equipment(drone.blueprint).type;
        if (type == "REPAIR" || type == "BATTLE" || type == "BOARDER") {
            result.push_back(&drone);
        }
    };
    for (const auto& drone : owner == 0 ? combatDrones_ : enemy_.drones) {
        append(drone);
    }
    if (owner == 1) {
        for (const auto& drone : enemy_.droneTrash) {
            append(drone);
        }
    }
    if (owner == 0) {
        for (const auto& factory : cargoFactories_) {
            append(factory.drone);
        }
        append(overflowFactory_.drone);
        for (const auto& drone : retiredCrewDrones_) {
            append(drone);
        }
    }
    std::ranges::stable_sort(result, {}, [](const auto* drone) {
        return drone->physicalCrewOrder;
    });
    return result;
}

int Run::damageCrewDrones(int ship,
                          int room,
                          float damage,
                          const SDL_FPoint* tile,
                          float stunSeconds,
                          int stunChance,
                          int excludedOwner,
                          const CombatDrone* onlyDrone) {
    int killed = 0;
    for (const int owner : {0, 1}) {
        if (owner == excludedOwner) {
            continue;
        }
        for (auto* physical : physicalCrewDrones(owner)) {
            auto& drone = *physical;
            if (onlyDrone && onlyDrone != &drone) {
                continue;
            }
            const auto& blueprint = catalog_.equipment(drone.blueprint);
            const bool boarder = blueprint.type == "BOARDER";
            if (blueprint.type != "REPAIR" && blueprint.type != "BATTLE" && !boarder) {
                continue;
            }
            if (drone.crewRoom < 0 || drone.crewPhysicalShip != ship || drone.crewRoom != room) {
                continue;
            }
            if (tile && (int(drone.crewPosition.x) / 35 != int(tile->x) / 35 ||
                         int(drone.crewPosition.y) / 35 != int(tile->y) / 35)) {
                continue;
            }
            // CrewDrone inherits DamageCrew's guaranteed stun and unconditional
            // chance draw before its virtual ShipDamage0x437810 halves damage.
            drone.ionStun = std::max(drone.ionStun, stunSeconds);
            const int stunRoll = int(combatRandom_.next() % 10);
            if (stunChance > stunRoll && advanced_) {
                drone.ionStun = std::max(drone.ionStun, 3.f);
            }
            if (damage <= 0 || drone.health <= 0) {
                continue; // Repair weapons do not heal crew-drone bodies.
            }
            drone.health = std::max(0.f, drone.health - damage * .5f);
            killed += drone.health == 0 ? 1 : 0;
        }
    }
    return killed;
}

void Run::renderCrewDrones(
    int ship, float x, float y, bool healthOnly, const CombatDrone* onlyDrone) {
    for (const int owner : {0, 1}) {
        for (const auto* physical : physicalCrewDrones(owner)) {
            if (onlyDrone && onlyDrone != physical) {
                continue;
            }
            const auto& drone = *physical;
            const auto& blueprint = catalog_.equipment(drone.blueprint);
            const bool boarder = blueprint.type == "BOARDER";
            if (blueprint.type != "REPAIR" && blueprint.type != "BATTLE" && !boarder) {
                continue;
            }
            if (!drone.deployed || drone.crewRoom < 0 || drone.crewDead ||
                drone.crewPhysicalShip != ship) {
                continue;
            }
            if (ship == 1 && !enemyRoomVisible(drone.crewRoom)) {
                continue;
            }
            const HangarCrew appearance{.name = "",
                                        .species = blueprint.id == "BOARDER_ION" ? "boarder_ion"
                                                   : blueprint.type == "REPAIR"  ? "repair"
                                                                                 : "battle",
                                        .fullName = ""};
            RunCrew combatView;
            combatView.appearance = appearance;
            combatView.health = drone.health;
            combatView.dying = drone.crewDying;
            combatView.dead = drone.crewDead;
            combatView.deathAnimationElapsed = drone.crewDeathElapsed;
            combatView.deathAnimationDone = drone.crewDeathDone;
            combatView.x = drone.crewPosition.x;
            combatView.y = drone.crewPosition.y;
            combatView.attackAnimation = drone.attackAnimation;
            combatView.attackElapsed = drone.attackElapsed;
            combatView.attackGoal = drone.attackGoal;
            combatView.attackAnimationElapsed = drone.attackAnimationElapsed;
            combatView.attackFlight = drone.attackFlight;
            combatView.attackFrom = drone.attackFrom;
            combatView.attackTo = drone.attackTo;
            combatView.attackHit = drone.attackHit;
            if (drone.crewPathIndex < int(drone.crewPath.size())) {
                combatView.path.push_back({drone.crewPath[std::size_t(drone.crewPathIndex)], -1});
            }
            combatView.stunTime = drone.ionStun;
            combatView.healthChangeRemaining = drone.healthChangeRemaining;
            combatView.medicalRate = drone.medicalRate;
            combatView.healingElapsed = drone.healingElapsed;
            combatView.mindElapsed = drone.mindElapsed;
            combatView.stunElapsed = drone.stunElapsed;
            const float maximumHealth = blueprint.id == "BOARDER_ION" ? 125.f
                                        : blueprint.type == "REPAIR"  ? 25.f
                                                                      : 150.f;
            combatView.healthBoost = maximumHealth - crewMaximumHealth(appearance.species);
            const auto attack = crewCombatAnimation(combatView);
            const bool ionCharging = blueprint.id == "BOARDER_ION" && drone.ionPulseCharge >= 0;
            // CrewAnimation::OnUpdate 0x473600 gives a working RepairDrone
            // (RepairDrone::CanRepair 0x723e30: powered) status 4 in a burning
            // room, chosen by the crew pass, or else status 2, repair_repair.
            const auto fire = crewFireAnimation(combatView, drone.crewSmoke);
            const bool repairing = blueprint.type == "REPAIR" && drone.repairKind != 0 &&
                                   drone.powered && drone.ionStun <= 0 && drone.health > 0 &&
                                   drone.crewPathIndex >= int(drone.crewPath.size());
            const std::string animation = drone.crewDying
                                              ? (drone.crewDeathLeft ? "death_left" : "death_right")
                                          : !attack.empty() ? attack
                                          : !fire.empty()   ? fire
                                          : repairing       ? "repair"
                                                            : "walk_down";
            if (!healthOnly && !ionCharging) {
                if (animation == fire && !fire.empty()) {
                    renderCrewSmoke(drone.crewSmoke,
                                    x + drone.crewPosition.x - 17.f,
                                    y + drone.crewPosition.y - 17.f);
                }
                painter_.crewSprite(appearance,
                                    x + drone.crewPosition.x - 17.f,
                                    y + drone.crewPosition.y - 17.f,
                                    1.f,
                                    false,
                                    animation,
                                    drone.crewDying   ? std::uint64_t(drone.crewDeathElapsed * 60.f)
                                    : !attack.empty() ? crewCombatTick(combatView)
                                    : !fire.empty() || repairing ? tick_
                                    : drone.crewPath.empty()     ? 0
                                                                 : tick_);
            }
            const auto ionEffect = [&](const std::string& name,
                                       float elapsed,
                                       SDL_FPoint point,
                                       bool centered,
                                       SDL_Color tint) {
                const auto& frame = catalog_.animation(name);
                if (!frame.usable()) {
                    return;
                }
                const int index =
                    std::clamp(int(elapsed / frame.duration * frame.frames), 0, frame.frames - 1);
                renderer_.sprite(renderer_.image(frame.path),
                                 {x + point.x - (centered ? frame.width / 2.f : 17.f),
                                  y + point.y - (centered ? frame.height / 2.f : 17.f),
                                  float(frame.width),
                                  float(frame.height)},
                                 {float(frame.x + index * frame.width),
                                  float(frame.y),
                                  float(frame.width),
                                  float(frame.height)},
                                 0,
                                 tint);
            };
            if (!healthOnly && ionCharging) {
                // UpdateShooting43c500 substitutes ionAnimation into the
                // ordinary body strip; its completed frame is retained.
                ionEffect("boarder_ion_ion",
                          drone.ionPulseCharge,
                          drone.crewPosition,
                          false,
                          {255, 255, 255, 255});
                ionEffect(
                    "boarder_ion_glow",
                    0,
                    drone.crewPosition,
                    false,
                    {255, 255, 255, Uint8(std::clamp(drone.ionPulseEffect, 0.f, 1.f) * 255.f)});
            }
            if (healthOnly && blueprint.id == "BOARDER_ION" && drone.ionPulseExplosion >= 0) {
                // IonDroneAnimation::FireShot437e80 fixes the emission point;
                // subsequent actor movement must not drag the explosion.
                ionEffect("explosion_big1_ion",
                          drone.ionPulseExplosion,
                          drone.ionPulsePosition,
                          true,
                          {133, 231, 237, 255});
            }
            if (healthOnly) {
                renderCrewHealth(combatView, x, y, owner);
            }
        }
    }
}
} // namespace ftl
