#include "run.hpp"
#include <cmath>

namespace ftl {
std::pair<int, int> Run::crewPowerForSystem(const RunSystem& system, bool enemy) const {
    // ShipManager::OnLoop 0x4a05b0 counts ProvidesPower crew in each owning
    // ship's system room, then separately records crew at their final goal.
    // EnergyAlien::ProvidesPower 0x723c90 is unconditional; stun, ion and
    // movement and the dying animation do not disable room power. Retained
    // tombstones stop contributing at the physical removal boundary.
    const auto& crew = enemy ? enemy_.crew : crew_;
    int amount = 0;
    int permanent = 0;
    for (const auto& member : crew) {
        if (member.appearance.species != "energy" || member.outOfGame || member.intruder ||
            member.room != system.definition.room) {
            continue;
        }
        ++amount;
        if (member.path.empty() && member.room == member.targetRoom &&
            member.slot == member.targetSlot) {
            const auto& ship = enemy ? enemy_.ship : ship_;
            for (const auto& room : ship.rooms) {
                if (room.id == member.room) {
                    const int columns = room.w / 35;
                    const float x = float(room.x + (member.slot % columns) * 35 + 17);
                    const float y = float(room.y + (member.slot / columns) * 35 + 17);
                    // CrewMember::AtFinalGoal 0x471210 uses a half-pixel box.
                    if (std::abs(member.x - x) < .5f && std::abs(member.y - y) < .5f) {
                        ++permanent;
                    }
                    break;
                }
            }
        }
    }
    // The system's virtual SetBonusPower applies health/temporary caps and
    // armament allocation. This query deliberately returns the raw pair so
    // ordinary systems and weapons/drones can preserve their distinct rules.
    return {amount, permanent};
}

void Run::updateCrewPower(int owner) {
    for (const bool enemy : {false, true}) {
        if (owner >= 0 && int(enemy) != owner) {
            continue;
        }
        auto& systems = enemy ? enemy_.systems : systems_;
        auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
        auto& powered = enemy ? enemy_.powered : weaponPower_;
        auto& drones = enemy ? enemy_.drones : combatDrones_;
        const auto& ship = enemy ? enemy_.ship : ship_;
        for (auto& system : systems) {
            const int id = system.definition.id;
            if ((id >= 6 && id <= 8) || id == 12) {
                // SetBonusPower614f40 does nothing when bNeedsPower is false.
                continue;
            }
            const auto [roomPower, permanent] = crewPowerForSystem(system, enemy);
            const int capacity = systemCapacity(system);
            const int bonus = std::min(roomPower, capacity);
            // Native power is the effective total. Source stores raw reactor,
            // battery and the FULL controller bonus separately, including credit
            // reserved by unpowered slots. Replacing bonus preserves raw power.
            system.power += bonus - system.bonusPower;
            system.bonusPower = bonus;
            const auto decreaseRaw = [&] {
                if (system.power <= system.bonusPower) {
                    return false;
                }
                releaseBatteryPower(system, 1, enemy);
                --system.power;
                return true;
            };
            const auto debit = [&](int cost) {
                while (cost > 0 && decreaseRaw()) {
                    --cost;
                }
                return cost == 0;
            };
            if (id == 3) {
                int remaining = bonus;
                for (std::size_t index = 0; index < weapons.size() && index < powered.size() &&
                                            index < ship.weapons.size();
                     ++index) {
                    auto& weapon = weapons[index];
                    const auto& blueprint = catalog_.equipment(ship.weapons[index]);
                    const int assigned = std::min(remaining, blueprint.power);
                    remaining -= assigned;
                    const int previous = weapon.bonusPower;
                    if (powered[index] && previous != assigned) {
                        if (assigned > previous) {
                            weapon.bonusPower = assigned;
                            for (int bar = previous; bar < assigned; ++bar) {
                                (void)decreaseRaw();
                            }
                        } else {
                            if (debit(std::max(0, blueprint.power - previous))) {
                                powered[index] = false;
                            }
                            weapon.bonusPower = assigned;
                            weapon.repowerPending = true;
                        }
                        continue;
                    }
                    if (!powered[index]) {
                        // UpdateBonusPower610320 visits the tail before granting
                        // an off slot's credit; it never erases controller bonus.
                        // Its stop test is powerState.first + iBonusPower, which
                        // leaves battery bars out (DroneSystem's includes them).
                        for (std::size_t tail = powered.size();
                             tail > 0 && system.power - system.batteryPower > capacity;) {
                            --tail;
                            if (tail >= weapons.size() || tail >= ship.weapons.size() ||
                                !powered[tail]) {
                                continue;
                            }
                            const int cost = std::max(0,
                                                      catalog_.equipment(ship.weapons[tail]).power -
                                                          weapons[tail].bonusPower);
                            if (debit(cost)) {
                                powered[tail] = false;
                            }
                        }
                    }
                    weapon.bonusPower = assigned;
                    const int ammunition = enemy ? enemy_.missiles : missiles_;
                    if (!powered[index] && assigned == blueprint.power &&
                        (blueprint.missiles == 0 || ammunition > 0 ||
                         !weapon.pendingProjectiles.empty())) {
                        // PowerWeapon(..., false, true) can use entirely free
                        // credit even while locked, without changing user intent.
                        powered[index] = true;
                    }
                }
            } else if (id == 4) {
                int remaining = bonus;
                for (auto& drone : drones) {
                    if (drone.bossSurge) {
                        drone.bonusPower = 0;
                        continue;
                    }
                    const auto& blueprint = catalog_.equipment(drone.blueprint);
                    const int assigned = std::min(remaining, blueprint.power);
                    remaining -= assigned;
                    const int previous = drone.bonusPower;
                    if (drone.powered && previous != assigned) {
                        if (assigned > previous) {
                            drone.bonusPower = assigned;
                            for (int bar = previous; bar < assigned; ++bar) {
                                (void)decreaseRaw();
                            }
                        } else {
                            if (debit(std::max(0, blueprint.power - previous))) {
                                drone.powered = false;
                            }
                            drone.bonusPower = assigned;
                            drone.repowerPending = true;
                        }
                        continue;
                    }
                    if (!drone.powered) {
                        for (std::size_t tail = drones.size();
                             tail > 0 && system.power > capacity;) {
                            auto& last = drones[--tail];
                            if (!last.powered || last.bossSurge) {
                                continue;
                            }
                            const int cost = std::max(
                                0, catalog_.equipment(last.blueprint).power - last.bonusPower);
                            if (debit(cost)) {
                                last.powered = false;
                            }
                        }
                    }
                    drone.bonusPower = assigned;
                    if (!drone.powered && drone.deployed && assigned == blueprint.power) {
                        (void)powerDrone(drone, int(enemy));
                    }
                }
            } else {
                const int transient = std::max(0, bonus - permanent);
                if (system.power - transient > system.restorePower) {
                    system.restorePower = system.power;
                }
                // Bonus-loss recovery belongs to the later base OnLoop, using
                // its retained lastBonusPower; SetBonus performs only this clamp.
                checkSystemMaxPower(system, enemy);
            }
        }
    }
}
} // namespace ftl
