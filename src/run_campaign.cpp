#include "run.hpp"
#include <algorithm>
#include <array>

namespace ftl {
void Run::startBossStage() {
    if (!campaign_.active || campaign_.victory || defeated_) {
        return;
    }
    const int stage = campaign_.nextStage;
    const std::array<std::string_view, 3> difficulty{"_EASY", "_NORMAL", "_HARD"};
    const std::string blueprint = "BOSS_" + std::to_string(stage) +
                                  std::string(difficulty.at(std::size_t(difficulty_))) +
                                  (advanced_ ? "_DLC" : "");
    beginCombat(blueprint);
    const bool restored = campaign_.stage == stage && campaign_.savedBoss.is_object();
    if (restored) {
        enemy_ = campaign_.savedBoss.at("enemy").get<Enemy>();
        enemyHazards_ = campaign_.savedBoss.at("hazards").get<std::map<int, RoomHazard>>();
        enemy_.present = true;
        if (stage == 3 && enemy_.teleportStartingCrew == 0) {
            // Pre-dependency checkpoints retain every stable home crew slot,
            // including dead records, so the stage's initial roster is recoverable.
            enemy_.teleportStartingCrew = static_cast<int>(enemy_.crew.size());
        }
    } else {
        // BossShip::StartStage 0x406490 constructs the exact authored blueprint,
        // then creates humans from per-room survival counts, never ShipGenerator.
        if (stage == 1) {
            campaign_.crewCounts.assign(enemy_.ship.rooms.size(), 0);
            for (const int room : {0, 1, 2, 3, 4, 5, 6, 9, 10, 14, 18}) {
                campaign_.crewCounts.at(std::size_t(room)) = 1;
            }
        }
        campaign_.crewCounts.resize(enemy_.ship.rooms.size());
        enemy_.crew.clear();
        for (const auto& room : enemy_.ship.rooms) {
            for (int slot = 0; slot < campaign_.crewCounts.at(std::size_t(room.id)); ++slot) {
                RunCrew member;
                // AddCrewMember4a3ee0 first gets the race blueprint's unused
                // base name, then replaces its empty explicit name with a new
                // unused name. Both draws precede animation and palette.
                (void)catalog_.unusedCrewName(locale_, {}, [&] {
                    return eventRandom_.next();
                });
                const auto name = catalog_.unusedCrewName(locale_, {}, [&] {
                    return eventRandom_.next();
                });
                const auto shortName =
                    catalog_.shortCrewName(name.first, [&](const std::string& value) {
                        const auto [left, right] =
                            renderer_.bounds(localizedFont(6, value), value, textScale(6));
                        return right - left;
                    });
                member.appearance = {shortName, "human", name.second ? 0 : 1, -1, 0, name.first};
                member.recordMale = name.second;
                initializeCrewAnimation(member, eventRandom_);
                int stride = 2;
                for (const auto& layer : catalog_.appearance("human").layers) {
                    if (layer.empty()) {
                        continue;
                    }
                    member.appearance.appearance +=
                        int(eventRandom_.next() % layer.size()) * stride;
                    stride *= static_cast<int>(layer.size());
                }
                member.health = crewMaximumHealth("human");
                placeVoyageCrew(member, room.id, false, true);
                assignPhysicalCrewOrder(member);
                member.savedRoom = member.room;
                member.savedSlot = member.slot;
                enemy_.crew.push_back(std::move(member));
            }
        }
        enemy_.droneParts = 10;
        // OnInit already constructed the authored armory. BossShip406490
        // changes its part count; it never appends a second deployed armory.
        campaign_.surgeElapsed = 0;
        campaign_.surgeGoal = float(combatRandom_.next() % 10000 + 20000) / 1000.f;
        campaign_.surgeCount = 0;
        if (stage == 3) {
            enemy_.superShields = 12;
            enemy_.maxSuperShields = 12;
            enemy_.teleportStartingCrew = static_cast<int>(enemy_.crew.size());
        }
    }
    campaign_.stage = stage;
    enemy_.boardingAi = stage == 3 ? 2 : 0;
    campaign_.fighting = true;
    campaign_.savedBoss = nullptr;
    if (!restored) {
        // StartStage 0x406490 clears bAutomated on the new stage's ship; a
        // crewless stage raises BOSS_AUTOMATED again through GetSubEvent.
        // A revisited stage keeps the same BossShip and its bAutomated.
        campaign_.automated = false;
    }
    progressionRun_.flags["enemy_automated"] = campaign_.automated ? 1 : 0;
    hostile_ = true;
    enemy_.hasSurrenderThreshold = false;
    encounter_.clear();
    encounterAutoBlueprint_.clear();
}

void Run::rememberBoss() {
    if (!campaign_.fighting || !enemy_.present) {
        return;
    }
    std::fill(campaign_.crewCounts.begin(), campaign_.crewCounts.end(), 0);
    for (const auto& crew : enemy_.crew) {
        if (crew.health > 0 && !crew.intruder && crew.room >= 0 &&
            std::size_t(crew.room) < campaign_.crewCounts.size()) {
            ++campaign_.crewCounts[std::size_t(crew.room)];
        }
    }
    campaign_.savedBoss = {{"enemy", enemy_}, {"hazards", enemyHazards_}};
    campaign_.fighting = false;
}

void Run::finishBossStage() {
    if (!campaign_.fighting) {
        return;
    }
    // MindSystem::ShipDestroyed 0x609e60 calls ReleaseCrew 0x609d40
    // before the destroyed Flagship shell can lose its target ownership.
    const bool stalemate = enemy_.escaping;
    releaseEnemyMind();
    rememberBoss();
    for (auto& member : enemy_.crew) {
        member.health = 0;
    }
    enemy_.cloneQueue.clear();
    for (std::size_t index = 0; index < crew_.size(); ++index) {
        auto& member = crew_[index];
        if (member.intruder) {
            if (member.health > 0) {
                member.health = 0;
                crewDeath(member);
            }
            std::erase(cloneQueue_, static_cast<int>(index));
        }
    }
    refreshCrewNames();
    hostile_ = false;
    if (campaign_.stage == 3 && !stalemate) {
        terminal(true, "gameover_win");
        return;
    }
    campaign_.nextStage = stalemate ? campaign_.stage : campaign_.stage + 1;
    campaign_.savedBoss = nullptr;
    forceBossJump();
    enemy_.present = false;
    for (auto& system : systems_) {
        if (system.definition.id == enemy_.hackingTarget) {
            system.hackActive = false;
            system.hackLevel = 0;
        }
    }
    enemy_.hacking = {};
    enemy_.hackingTarget = -1;
    resetCrewDroneDeployment(enemy_.hackingDrone);
    enemy_.hackingDroneId = 0;
    if (hacking_.active) {
        hacking_.cooldown = 20;
        hacking_.active = false;
    }
    hackingTarget_ = -1;
    resetCrewDroneDeployment(hackingDrone_);
    hackingDroneId_ = 0;
    enemyExplosionActive_ = false;
    explosion_ = 0;
    projectiles_.clear();
    cancelAim();
    for (auto& weapon : combatWeapons_) {
        weapon.clearTargets();
    }
    for (auto& system : systems_) {
        system.artillery.clearTargets();
    }
    eventId_ = stalemate ? "BOSS_STALEMATE" : "BOSS_ESCAPED";
    // World5f6520 starts a generated outcome chain with a fresh crew selector.
    selectedVoyageCrewSeed_ = -1;
    enterEvent(events_.event(eventId_));
}

void Run::advanceBoss() {
    if (!campaign_.fighting || !enemy_.present || !hostile_ || enemy_.hull <= 0) {
        return;
    }
    // BossShip::OnLoop 0x407510 updates its game-time timer after checking Done.
    if (campaign_.surgeElapsed >= campaign_.surgeGoal) {
        if (campaign_.stage == 2) {
            if (std::ranges::none_of(enemy_.drones, [](const auto& drone) {
                    return drone.bossSurge;
                })) {
                for (int i = 0; i < 7; ++i) {
                    CombatDrone drone;
                    drone.blueprint = combatRandom_.next() % 2 == 1 ? "COMBAT_1" : "COMBAT_BEAM";
                    drone.bossSurge = true;
                    drone.health = 150;
                    enemy_.drones.push_back(std::move(drone));
                }
            }
            const int active = difficulty_ == 0 ? 4 : difficulty_ == 1 ? 6 : 7;
            int index = 0;
            for (auto& drone : enemy_.drones) {
                if (!drone.bossSurge) {
                    continue;
                }
                drone.powered = drone.deployed = index++ < active;
                drone.lifespan = 2;
                drone.health = 150;
                drone.space = 0;
                drone.orbitInitialized = false;
                drone.pause = 0;
                drone.additionalPause = 0;
            }
        } else if (campaign_.stage == 3) {
            if (campaign_.surgeCount % 4 == 3) {
                // Shields::PowerSuperShields 0x60bc70 restores twelve layers.
                enemy_.superShields = 12;
                enemy_.maxSuperShields = 12;
            } else {
                const auto& weapon = catalog_.equipment("LASER_HEAVY_1");
                for (int i = 0; i < 7; ++i) {
                    const auto& room = ship_.rooms[combatRandom_.next() % ship_.rooms.size()];
                    CombatProjectile shot;
                    shot.id = nextProjectile_++;
                    shot.owner = shot.space = 1;
                    shot.targetShip = 0;
                    shot.room = room.id;
                    shot.weapon = weapon.id;
                    shot.projectileImage = weapon.projectileImage;
                    shot.damageBonus = 1 - weapon.damage;
                    shot.target = {float(room.x + room.w / 2), float(room.y + room.h / 2)};
                    shot.position = shot.previous = {
                        float(enemy_.ship.imageRect[0] + enemy_.ship.imageRect[2] / 2),
                        float(enemy_.ship.imageRect[1] + enemy_.ship.imageRect[3] / 2)};
                    shot.direction = {0, -1};
                    shot.entryAngle = float(combatRandom_.next() % 360);
                    shot.speed =
                        (weapon.projectileSpeed > 0 ? weapon.projectileSpeed : 60.f) * 16.f;
                    projectiles_.push_back(std::move(shot));
                    combatSound(weapon.launchSounds, 0, true);
                }
            }
            ++campaign_.surgeCount;
        }
        campaign_.surgeElapsed = 0;
        campaign_.surgeGoal = float(combatRandom_.next() % 10000 + 20000) / 1000.f;
    }
    campaign_.surgeElapsed += Time::step;
}
} // namespace ftl
