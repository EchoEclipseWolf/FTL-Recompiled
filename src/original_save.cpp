#include "original_save.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// windows.h must precede these.
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#endif
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace ftl::original_save {
Milli Milli::from(float value) {
    // FileHelper::writeFloat 0x58ffd0: mulss by 1000.0f, then cvttss2si.
    // cvttss2si returns the integer indefinite value INT_MIN for NaN or any
    // product outside int32 (e.g. the -FLT_MAX "unset" sentinels).
    const float scaled = value * 1000.0f;
    const bool inRange = scaled > -2147483648.0f && scaled < 2147483648.0f;
    return {inRange ? static_cast<std::int32_t>(scaled) : std::numeric_limits<std::int32_t>::min()};
}

float Milli::value() const {
    // FileHelper::readFloat 0x58f620.
    return static_cast<float>(raw) / 1000.0f;
}

namespace {
// FileHelper::writeInt 0x5902c0 / writeString 0x590140: little-endian 32-bit
// integers; strings are a 32-bit strlen followed by the bytes, no terminator.
class Writer {
    std::vector<std::uint8_t> bytes_;

    void word(std::int32_t value) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8) {
            bytes_.push_back(static_cast<std::uint8_t>(bits >> shift));
        }
    }

public:
    static constexpr bool reading = false;

    void i(const int& value) {
        word(value);
    }

    void f(const Milli& value) {
        word(value.raw);
    }

    void s(const std::string& value) {
        if (value.find('\0') != std::string::npos ||
            value.size() > std::size_t(std::numeric_limits<std::int32_t>::max())) {
            throw std::invalid_argument("original_save_invalid_string");
        }
        word(static_cast<std::int32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    // A count that the reader takes from the file and the writer from the model.
    int count(std::size_t size) {
        if (size > std::size_t(std::numeric_limits<std::int32_t>::max())) {
            throw std::invalid_argument("original_save_count_out_of_range");
        }
        const int value = static_cast<int>(size);
        word(value);
        return value;
    }

    std::vector<std::uint8_t> take() {
        return std::move(bytes_);
    }
};

class Reader {
    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_ = 0;

    std::int32_t word() {
        if (bytes_.size() - offset_ < 4) {
            throw std::runtime_error("original_save_truncated");
        }
        std::uint32_t bits = 0;
        for (int index = 0; index < 4; ++index) {
            bits |= std::uint32_t(bytes_[offset_++]) << (8 * index);
        }
        return std::bit_cast<std::int32_t>(bits);
    }

public:
    static constexpr bool reading = true;

    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    void i(int& value) {
        value = word();
    }

    void f(Milli& value) {
        value.raw = word();
    }

    void s(std::string& value) {
        const auto size = word();
        // FileHelper::readString 0x58f330 has no bound; refuse corrupt lengths.
        if (size < 0 || std::size_t(size) > bytes_.size() - offset_ || size > 65536) {
            throw std::runtime_error("original_save_invalid_string_length");
        }
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), std::size_t(size));
        offset_ += std::size_t(size);
    }

    int count(std::size_t) {
        const auto value = word();
        if (value < 0 || value > 1 << 20) {
            throw std::runtime_error("original_save_invalid_count");
        }
        return value;
    }

    bool finished() const {
        return offset_ == bytes_.size();
    }
};

template <class Io, class T, class F> void list(Io& io, T& items, F&& each) {
    const int count = io.count(items.size());
    if constexpr (Io::reading) {
        items.resize(std::size_t(count));
    }
    for (auto& item : items) {
        each(item);
    }
}

// A list whose length the file does not carry (it comes from the blueprint).
template <class Io, class T, class F>
void fixedList(Io&, T& items, std::size_t expected, F&& each) {
    if constexpr (Io::reading) {
        items.resize(expected);
    } else if (items.size() != expected) {
        throw std::invalid_argument("original_save_fixed_list_mismatch");
    }
    for (auto& item : items) {
        each(item);
    }
}

template <class Io, class A> void animation(Io& io, A& value) {
    io.i(value.running);
    io.i(value.loop);
    io.i(value.currentFrame);
    io.f(value.progress);
    io.f(value.scale);
    io.f(value.x);
    io.f(value.y);
}

template <class Io, class P> void point(Io& io, P& value) {
    io.f(value.x);
    io.f(value.y);
}

bool crystalSpecies(const std::string& species) {
    // CrystalAlien is the "crystal" race (CrewMemberFactory::CreateCrewMember).
    return species == "crystal";
}

template <class Io, class C> void crew(Io& io, C& value) {
    io.s(value.name);
    io.s(value.species);
    io.i(value.drone);
    io.i(value.health);
    io.i(value.x);
    io.i(value.y);
    io.i(value.room);
    io.i(value.slot);
    io.i(value.playerControlled);
    io.i(value.cloneReady);
    io.i(value.deathNumber);
    list(io, value.colors, [&](auto& color) {
        io.i(color);
    });
    io.i(value.mindControlled);
    io.i(value.savedSlot);
    io.i(value.savedRoom);
    for (auto& skill : value.skills) {
        io.i(skill);
    }
    io.i(value.male);
    for (auto& stat : value.stats) {
        io.i(stat);
    }
    if (value.drone) {
        return;
    }
    auto& state = value.state;
    io.f(state.stunTime);
    io.i(state.healthBoost);
    io.i(state.deathNumber);
    io.f(state.mindDamageBoost);
    io.i(state.cloneDying);
    io.i(state.deathCount);
    for (auto& earned : state.skillsEarned) {
        io.i(earned[0]);
        io.i(earned[1]);
    }
    io.i(state.teleporting);
    animation(io, state.teleportAnimation);
    io.i(state.teleportReverse);
    if (crystalSpecies(value.species)) {
        io.f(value.crystal.powerCooldownFirst);
        io.f(value.crystal.powerCooldownSecond);
        io.i(value.crystal.cloned);
    }
}

template <class Io, class R> void systemRecord(Io& io, R& value) {
    io.i(value.power);
    io.i(value.damage);
    io.i(value.lockCount);
    io.i(value.lockTimerX5000);
    io.i(value.repairOverTime);
    io.i(value.damageOverTime);
    io.i(value.batteryPower);
    io.i(value.hackLevel);
    io.i(value.hacked);
    io.i(value.tempPowerCap);
    io.i(value.tempPowerLoss);
    io.i(value.tempDividePower);
}

template <class Io, class D> void door(Io& io, D& value) {
    io.i(value.baseHealth);
    io.i(value.health);
    io.i(value.lastBase);
    io.i(value.open);
    io.i(value.fakeOpen);
    io.i(value.blast);
    io.i(value.hacked);
}

template <class Io, class S> void ship(Io& io, S& value, const Context* context) {
    io.s(value.blueprintName);
    io.s(value.shipName);
    io.s(value.imageFile);
    list(io, value.customCrew, [&](auto& member) {
        io.s(member.first);
        io.s(member.second);
    });
    io.i(value.hostile);
    io.f(value.jumpTimer);
    io.i(value.jumping);
    io.f(value.jumpAnimationTime);
    io.i(value.hull);
    io.i(value.fuel);
    io.i(value.droneCount);
    io.i(value.missileCount);
    io.i(value.scrap);
    list(io, value.crew, [&](auto& member) {
        crew(io, member);
    });
    io.i(value.reactorPower);
    ShipLayout layout;
    if (context && context->layout) {
        layout = context->layout(value.blueprintName);
    } else if constexpr (Io::reading) {
        throw std::invalid_argument("original_save_layout_required");
    } else {
        for (const auto& room : value.rooms) {
            layout.roomSquares.push_back(static_cast<int>(room.fires.size()));
        }
        layout.doors = static_cast<int>(value.doors.size());
        layout.airlocks = static_cast<int>(value.airlocks.size());
        layout.artillery = static_cast<int>(value.systems[11].size());
    }
    for (std::size_t id = 0; id < value.systems.size(); ++id) {
        auto& records = value.systems[id];
        if constexpr (Io::reading) {
            int capacity = 0;
            io.i(capacity);
            if (capacity != 0) {
                // ExportShip writes one record per ArtillerySystem, back to back;
                // ImportShip pairs them with the blueprint's artillery systems.
                records.resize(id == 11 ? std::size_t(std::max(1, layout.artillery)) : 1);
                for (auto& record : records) {
                    if (&record != &records.front()) {
                        io.i(capacity);
                    }
                    record.capacity = capacity;
                    systemRecord(io, record);
                }
            }
        } else {
            if (records.empty()) {
                const int none = 0;
                io.i(none);
                continue;
            }
            if (records.size() > 1 && (id != 11 || int(records.size()) != layout.artillery)) {
                throw std::invalid_argument("original_save_system_record_count");
            }
            for (const auto& record : records) {
                if (record.capacity <= 0) {
                    throw std::invalid_argument("original_save_installed_system_without_capacity");
                }
                io.i(record.capacity);
                systemRecord(io, record);
            }
        }
    }
    if (value.installed(13)) {
        io.f(value.clone.timeToClone);
        io.f(value.clone.timeGoal);
        io.f(value.clone.deathTime);
    }
    if (value.installed(12)) {
        io.i(value.battery.turnedOn);
        io.i(value.battery.batteryPower);
        io.i(value.battery.timerX1000);
    }
    if (value.installed(0)) {
        auto& shields = value.shields;
        io.i(shields.power);
        io.i(shields.superFirst);
        io.i(shields.superSecond);
        io.f(shields.charger);
        io.i(shields.downRunning);
        io.f(shields.downProgress);
        io.i(shields.upRunning);
        io.f(shields.upProgress);
        io.i(shields.superUpRunning);
        io.f(shields.superUpProgress);
        io.f(shields.downPointX);
        io.f(shields.downPointY);
    }
    if (value.installed(10)) {
        io.i(value.cloak.turnedOn);
        io.i(value.cloak.timerRunning);
        io.f(value.cloak.currentGoal);
        io.f(value.cloak.currentTime);
    }
    fixedList(io, value.rooms, layout.roomSquares.size(), [&](auto& room) {
        io.i(room.oxygen);
        const auto index = std::size_t(&room - value.rooms.data());
        fixedList(io, room.fires, std::size_t(layout.roomSquares[index]), [&](auto& fire) {
            io.i(fire.damage);
            io.i(fire.start);
            io.i(fire.death);
        });
        io.i(room.primarySlot);
        io.i(room.primaryDirection);
    });
    list(io, value.breaches, [&](auto& breach) {
        io.i(breach.x);
        io.i(breach.y);
        io.i(breach.damage);
    });
    // Ship::SaveState 0x483930.
    fixedList(io, value.doors, std::size_t(layout.doors), [&](auto& item) {
        door(io, item);
    });
    fixedList(io, value.airlocks, std::size_t(layout.airlocks), [&](auto& item) {
        door(io, item);
    });
    io.f(value.cloakingTracker);
    list(io, value.lockdowns, [&](auto& shard) {
        point(io, shard.position);
        point(io, shard.goal);
        io.f(shard.speed);
        io.i(shard.arrived);
        io.i(shard.done);
        io.f(shard.lifeTime);
        io.i(shard.superFreeze);
        io.i(shard.lockingRoom);
        io.i(shard.reverse);
        io.f(shard.progress);
    });
    if (value.installed(3)) {
        list(io, value.weapons, [&](auto& weapon) {
            io.s(weapon.name);
            io.i(weapon.powered);
        });
    } else {
        int none = 0;
        io.i(none);
        if (none != 0) {
            throw std::runtime_error("original_save_weapons_without_system");
        }
    }
    if (value.installed(4)) {
        list(io, value.drones, [&](auto& drone) {
            io.s(drone.name);
            io.i(drone.powered);
            io.i(drone.deployed);
            io.i(drone.x);
            io.i(drone.y);
            io.i(drone.room);
            io.i(drone.slot);
            io.i(drone.health);
        });
    } else {
        int none = 0;
        io.i(none);
        if (none != 0) {
            throw std::runtime_error("original_save_drones_without_system");
        }
    }
    list(io, value.augments, [&](auto& augment) {
        io.s(augment);
    });
}

template <class Io, class P> void projectile(Io& io, P& value) {
    io.i(value.tag);
    if (value.tag == 0) {
        return; // the loader counts the entry and drops it
    }
    if (value.tag < 0 || value.tag > 6) {
        throw std::runtime_error("original_save_unknown_projectile_tag");
    }
    point(io, value.position);
    point(io, value.lastPosition);
    io.f(value.speedMagnitude);
    point(io, value.target);
    io.f(value.heading);
    io.i(value.ownerId);
    io.i(value.selfId);
    io.i(value.damage);
    io.i(value.shieldPiercing);
    io.i(value.fireChance);
    io.i(value.breachChance);
    io.i(value.ionDamage);
    io.i(value.systemDamage);
    io.i(value.crewDamage);
    io.i(value.hullBuster);
    io.i(value.damageOwnerId);
    io.i(value.damageSelfId);
    io.i(value.lockdown);
    io.i(value.crystalShard);
    io.i(value.stunChance);
    io.i(value.stun);
    io.f(value.lifespan);
    io.i(value.destinationSpace);
    io.i(value.currentSpace);
    io.i(value.targetId);
    io.i(value.dead);
    io.s(value.deathAnimationName);
    io.s(value.flightAnimationName);
    animation(io, value.deathAnimation);
    animation(io, value.flightAnimation);
    point(io, value.speed);
    io.i(value.missed);
    io.i(value.hitTarget);
    io.s(value.hitSolidSound);
    io.s(value.hitShieldSound);
    io.s(value.missSound);
    io.i(value.entryAngle);
    io.i(value.startedDeath);
    io.i(value.passedTarget);
    io.i(value.targetType);
    io.i(value.broadcastTarget);
    switch (value.tag) {
    case 1:
        io.f(value.spinAngle);
        io.f(value.spinSpeed);
        break;
    case 4:
        io.i(value.bombMissed);
        io.f(value.explosiveDelay);
        io.i(value.superShield);
        io.i(value.superShieldBypass);
        io.i(value.flightReverse);
        break;
    case 5:
        point(io, value.subEnd);
        point(io, value.subStart);
        point(io, value.shieldEnd);
        point(io, value.finalEnd);
        point(io, value.target2);
        point(io, value.target1);
        io.f(value.beamLifespan);
        io.f(value.length);
        io.f(value.drift);
        point(io, value.lastCollision);
        io.f(value.animationTimer);
        io.i(value.lastDamage);
        io.f(value.startHeading);
        io.f(value.timer);
        io.i(value.piercedShield);
        io.i(value.oneSpace);
        io.i(value.damageSuperShield);
        io.i(value.movingTargetId);
        break;
    case 6:
        point(io, value.startPoint);
        io.i(value.pdsPassedTarget);
        io.f(value.currentScale);
        io.i(value.pdsMissed);
        animation(io, value.explosion);
        break;
    default:
        break; // Asteroid and Missile have no extra fields
    }
}

template <class Io, class S> void spaceDrone(Io& io, S& value) {
    io.f(value.destroyedTimer);
    io.i(value.currentSpace);
    io.i(value.destinationSpace);
    point(io, value.currentLocation);
    point(io, value.lastLocation);
    point(io, value.destinationLocation);
    point(io, value.pointTarget);
    point(io, value.targetLocation);
    point(io, value.targetSpeed);
    io.f(value.pause);
    io.f(value.additionalPause);
    io.f(value.weaponCooldown);
    io.f(value.currentAngle);
    io.f(value.aimingAngle);
    io.f(value.lastAimingAngle);
    io.i(value.lifespan);
    io.i(value.disrupted);
    io.i(value.poweredLastFrame);
    io.f(value.ionStun);
    io.f(value.desiredAimingAngle);
    io.i(value.deployedLastFrame);
    point(io, value.speedVector);
    animation(io, value.explosion);
}

template <class Io, class W> void weaponFactory(Io& io, W& value) {
    io.f(value.cooldownFirst);
    io.f(value.cooldownSecond);
    io.f(value.subCooldownFirst);
    io.f(value.subCooldownSecond);
    io.i(value.boostLevel);
    io.i(value.chargeLevel);
    list(io, value.targets, [&](auto& target) {
        io.i(target.x);
        io.i(target.y);
    });
    list(io, value.lastTargets, [&](auto& target) {
        io.i(target.x);
        io.i(target.y);
    });
    io.i(value.autoFiring);
    io.i(value.fireWhenReady);
    io.i(value.targetId);
    animation(io, value.visual.animation);
    io.f(value.visual.slideProgress);
    io.i(value.visual.firing);
    io.i(value.visual.fireShot);
    io.i(value.visual.boostLevel);
    animation(io, value.visual.boostAnimation);
    io.i(value.lastProjectileId);
    list(io, value.queued, [&](auto& shot) {
        projectile(io, shot);
    });
}

template <class Io, class C> void combatDrone(Io& io, C& value) {
    point(io, value.lastDestination);
    io.f(value.progressToDestination);
    io.f(value.heading);
    io.f(value.oldHeading);
}

template <class Io, class B, class S>
void battleState(Io& io, B& value, const S& owner, const Context* context) {
    if (owner.installed(4)) {
        fixedList(io, value.drones, owner.drones.size(), [&](auto& drone) {
            io.i(drone.deployed);
            io.i(drone.powered);
            const auto& name = owner.drones[std::size_t(&drone - value.drones.data())].name;
            if (!context || !context->droneType) {
                throw std::invalid_argument("original_save_drone_type_required");
            }
            const auto type = context->droneType(name);
            if (type == "REPAIR" || type == "BATTLE") {
                return; // CrewDrone::SaveState is empty.
            }
            spaceDrone(io, drone.space);
            if (type == "COMBAT" || type == "SHIP_REPAIR") {
                combatDrone(io, drone.combat);
            } else if (type == "SHIELD") {
                io.f(drone.shieldGlow);
            } else if (type == "BOARDER") {
                auto& boarder = drone.boarder;
                io.i(boarder.diedInSpace);
                io.i(boarder.deliveredDrone);
                point(io, boarder.startingPosition);
                io.i(boarder.health);
                io.i(boarder.x);
                io.i(boarder.y);
                io.i(boarder.room);
                io.i(boarder.slot);
            } else if (type != "DEFENSE") {
                throw std::runtime_error("original_save_unsupported_drone_type:" + type);
            }
        });
    }
    if (owner.installed(15)) {
        auto& hacking = value.hacking;
        io.i(hacking.currentSystem);
        io.i(hacking.dronePowered);
        io.i(hacking.droneDeployed);
        io.i(hacking.hacking);
        io.f(hacking.effectFirst);
        io.f(hacking.effectSecond);
        io.i(hacking.lockedOut);
        spaceDrone(io, hacking.drone);
        point(io, hacking.finalDestination);
        io.i(hacking.arrived);
        io.i(hacking.finishedSetup);
        animation(io, hacking.flying);
        animation(io, hacking.extending);
    }
    if (owner.installed(14)) {
        io.f(value.mind.controlFirst);
        io.f(value.mind.controlSecond);
    }
    if (owner.installed(3)) {
        list(io, value.weapons, [&](auto& weapon) {
            weaponFactory(io, weapon);
        });
    }
    if (owner.installed(11)) {
        fixedList(io, value.artillery, owner.systems[11].size(), [&](auto& weapon) {
            weaponFactory(io, weapon);
        });
    }
    list(io, value.superDrones, [&](auto& drone) {
        io.s(drone.name);
        spaceDrone(io, drone.space);
        combatDrone(io, drone.combat);
        io.i(drone.deployed);
        io.i(drone.destroyed);
        io.i(drone.powered);
    });
}

template <class Io, class G> void game(Io& io, G& save, const Context* context) {
    io.i(save.version);
    if constexpr (Io::reading) {
        if (save.version != saveVersion) {
            throw std::runtime_error("original_save_unsupported_version");
        }
    }
    io.i(save.useSystemRandom);
    io.i(save.dlc);
    // ScoreKeeper::SaveGame 0x43e5a0 and AchievementTracker::SaveGame 0x42cfa0.
    auto& score = save.score;
    io.i(score.difficulty);
    for (auto& stat : score.stats) {
        io.i(stat);
    }
    io.s(score.shipName);
    io.s(score.blueprintName);
    io.i(score.sector);
    io.i(score.flag);
    list(io, score.flags, [&](auto& flag) {
        io.s(flag.first);
        io.i(flag.second);
    });
    ship(io, save.player, context);
    // CommandGui::ExportData.
    list(io, save.cargo, [&](auto& item) {
        io.s(item);
    });
    // StarMap::SaveGame 0x5c7f30.
    auto& map = save.starMap;
    io.i(map.sectorMapSeed);
    io.i(map.currentSectorSeed);
    io.i(map.dangerX);
    io.i(map.dangerY);
    io.i(map.pursuitDelay);
    io.i(map.currentLocation);
    io.i(map.waitedLast);
    io.i(map.fuelEventSeed);
    io.s(map.lastEscapeEvent);
    io.i(map.mapRevealed);
    io.i(map.bossLevel);
    io.i(map.bossLocation);
    io.i(map.bossJumping);
    io.i(map.reversedPath);
    io.i(map.arrivedAtBase);
    list(io, map.sectorsVisited, [&](auto& visited) {
        io.i(visited);
    });
    io.i(map.worldLevel);
    io.i(map.secretSector);
    list(io, map.locations, [&](auto& location) {
        io.i(location.visited);
        if (location.visited) {
            io.s(location.spaceImage);
            io.s(location.planetImage);
            io.i(location.planetX);
            io.i(location.planetY);
            io.i(location.planetRotation);
        }
        io.i(location.known);
        int hostile = location.ship.has_value();
        io.i(hostile);
        if (hostile) {
            if constexpr (Io::reading) {
                location.ship.emplace();
            }
            io.s(location.ship->eventName);
            io.s(location.ship->autoBlueprint);
            io.i(location.ship->seed);
        }
        io.i(location.fleet);
        io.i(location.fleetChanging);
        int store = location.store.has_value();
        io.i(store);
        if (store) {
            if constexpr (Io::reading) {
                location.store.emplace();
            }
            // Store::SaveStore 0x4b3590.
            auto& shop = *location.store;
            list(io, shop.sections, [&](auto& section) {
                io.i(section.type);
                for (auto& box : section.boxes) {
                    io.i(box.count);
                    if (box.count != -1) {
                        io.s(box.blueprint);
                        io.i(box.value);
                    }
                }
            });
            for (auto& resource : shop.resources) {
                io.i(resource);
            }
        }
    });
    list(io, map.addedQuests, [&](auto& quest) {
        io.s(quest.first);
        io.i(quest.second);
    });
    list(io, map.delayedQuests, [&](auto& quest) {
        io.s(quest);
    });
    // ShipEvent::Save.
    io.i(save.shipEvent.present);
    io.i(save.shipEvent.shipSeed);
    io.s(save.shipEvent.surrender);
    io.s(save.shipEvent.escape);
    io.s(save.shipEvent.destroyed);
    io.s(save.shipEvent.deadCrew);
    io.s(save.shipEvent.gotAway);
    io.s(save.generatedEvent);
    io.i(save.lastMainTextLiteral);
    io.s(save.lastMainText);
    io.i(save.lastSelectedCrewSeed);
    list(io, save.choiceHistory, [&](auto& choice) {
        io.i(choice);
    });
    int nearby = save.enemy.has_value();
    io.i(nearby);
    if (nearby) {
        if constexpr (Io::reading) {
            save.enemy.emplace();
        }
        io.i(save.enemyBoss);
        ship(io, *save.enemy, context);
        // ShipAI::Save.
        auto& ai = save.enemyAi;
        io.i(ai.surrendered);
        io.i(ai.escaping);
        io.i(ai.destroyed);
        io.i(ai.surrenderThreshold);
        io.i(ai.escapeThreshold);
        io.f(ai.escapeTimer);
        io.i(ai.stalemateTriggered);
        io.f(ai.stalemateTimer);
        io.i(ai.timesTeleported);
        io.i(ai.crewNeeded);
    }
    // SpaceManager::SaveSpace 0x4166a0.
    auto& space = save.space;
    io.i(space.sunLevel);
    io.i(space.pulsarLevel);
    io.i(space.pds);
    io.i(space.environmentTarget);
    int asteroids = space.asteroids.has_value();
    io.i(asteroids);
    if (asteroids) {
        if constexpr (Io::reading) {
            space.asteroids.emplace();
        }
        io.i(space.asteroids->initialShields);
        io.i(space.asteroids->stateTimerX1000);
        io.i(space.asteroids->state);
        io.i(space.asteroids->timerX1000);
        io.i(space.asteroids->currentSpace);
    }
    io.i(space.flashX1000);
    io.i(space.flashTimerX1000);
    io.f(space.randomPdsTimer);
    list(io, space.projectiles, [&](auto& shot) {
        projectile(io, shot);
    });
    battleState(io, save.playerBattle, save.player, context);
    if (nearby) {
        battleState(io, save.enemyBattle, *save.enemy, context);
    }
    // CompleteShip::SaveState.
    io.i(save.playerTeleportTargetRoom);
    if (nearby) {
        io.i(save.enemyTeleportTargetRoom);
    }
    // CombatControl::SaveState and BossShip::SaveBoss.
    io.i(save.combatControl);
    io.i(save.boss.currentStage);
    io.i(save.boss.nextStage);
    io.f(save.boss.powerGoal);
    io.f(save.boss.powerTime);
    list(io, save.boss.crewCounts, [&](auto& count) {
        io.i(count);
    });
}
} // namespace

std::vector<std::uint8_t> serialize(const SaveGame& save, const Context* context) {
    Writer writer;
    game(writer, save, context);
    return writer.take();
}

SaveGame parse(const std::vector<std::uint8_t>& bytes, const Context& context) {
    Reader reader(bytes);
    SaveGame save;
    game(reader, save, &context);
    if (!reader.finished()) {
        throw std::runtime_error("original_save_trailing_bytes");
    }
    return save;
}

std::filesystem::path defaultSaveFolder() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> documents{};
    if (SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, documents.data()) != S_OK) {
        return {};
    }
    return std::filesystem::path(documents.data()) / L"My Games" / L"FasterThanLight";
#else
    return {};
#endif
}

bool originalGameRunning() {
#ifdef _WIN32
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("original_save_process_scan_failed");
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running;
         more = Process32NextW(snapshot, &entry)) {
        running = _wcsicmp(entry.szExeFile, L"FTLGame.exe") == 0;
    }
    CloseHandle(snapshot);
    return running;
#else
    return false;
#endif
}

#ifdef _WIN32
namespace {
// Steam's own library list: <SteamPath>/steamapps/libraryfolders.vdf holds one
// "path" entry per library (backslashes escaped), including SteamPath itself.
std::filesystem::path steamOriginalExecutable() {
    std::array<wchar_t, MAX_PATH> steam{};
    DWORD size = DWORD(steam.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Valve\\Steam",
                     L"SteamPath",
                     RRF_RT_REG_SZ,
                     nullptr,
                     steam.data(),
                     &size) != ERROR_SUCCESS) {
        return {};
    }
    std::vector<std::filesystem::path> libraries{std::filesystem::path(steam.data())};
    std::ifstream vdf(libraries.front() / "steamapps" / "libraryfolders.vdf");
    for (std::string line; std::getline(vdf, line);) {
        std::istringstream tokens(line);
        std::string key;
        std::string value;
        if (tokens >> std::quoted(key) >> std::quoted(value) && key == "path") {
            libraries.emplace_back(value);
        }
    }
    for (const auto& library : libraries) {
        const auto executable =
            library / "steamapps" / "common" / "FTL Faster Than Light" / "FTLGame.exe";
        std::error_code error;
        if (std::filesystem::is_regular_file(executable, error)) {
            return executable;
        }
    }
    return {};
}

// The executable of the last FTLGame.exe this process closed, so a relaunch
// after an export starts the same install.
std::filesystem::path closedOriginalExecutable;
} // namespace
#endif

void closeOriginalGame() {
#ifdef _WIN32
    // Terminate rather than close: a normal quit would write the original's own
    // run over an exported continue.sav.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("original_save_process_scan_failed");
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::vector<HANDLE> processes;
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"FTLGame.exe") != 0) {
            continue;
        }
        const HANDLE process =
            OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                        FALSE,
                        entry.th32ProcessID);
        if (process == nullptr) {
            continue;
        }
        std::array<wchar_t, 1024> image{};
        DWORD length = DWORD(image.size());
        if (QueryFullProcessImageNameW(process, 0, image.data(), &length)) {
            closedOriginalExecutable = std::filesystem::path(std::wstring(image.data(), length));
        }
        processes.push_back(process);
    }
    CloseHandle(snapshot);
    bool closed = true;
    for (const HANDLE process : processes) {
        TerminateProcess(process, 0);
        closed = WaitForSingleObject(process, 10000) == WAIT_OBJECT_0 && closed;
        CloseHandle(process);
    }
    if (!closed || originalGameRunning()) {
        throw std::runtime_error("FTLGame.exe did not close");
    }
#endif
}

void relaunchOriginalGame() {
#ifdef _WIN32
    closeOriginalGame();
    std::error_code error;
    auto executable = std::filesystem::is_regular_file(closedOriginalExecutable, error)
                          ? closedOriginalExecutable
                          : std::filesystem::path{};
    if (executable.empty()) {
        executable = steamOriginalExecutable();
    }
    if (executable.empty()) {
        // FTL's Steam app id; Steam resolves the install itself.
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                nullptr, L"open", L"steam://rungameid/212680", nullptr, nullptr, SW_SHOWNORMAL)) <=
            32) {
            throw std::runtime_error("FTLGame.exe not found");
        }
        return;
    }
    // The original reads ftl.dat from its working directory.
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    const auto directory = executable.parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION launched{};
    if (!CreateProcessW(executable.c_str(),
                        command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        directory.c_str(),
                        &startup,
                        &launched)) {
        throw std::runtime_error("FTLGame.exe could not be started");
    }
    CloseHandle(launched.hThread);
    CloseHandle(launched.hProcess);
#else
    throw std::runtime_error("original_game_launch_unsupported");
#endif
}
} // namespace ftl::original_save
