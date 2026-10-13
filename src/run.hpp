#pragma once
#include "campaign.hpp"
#include "combat.hpp"
#include "combat_skills.hpp"
#include "events.hpp"
#include "hangar.hpp"
#include "log.hpp"
#include "original_save.hpp"
#include "profile.hpp"
#include "settings.hpp"
#include "space.hpp"
#include "time.hpp"
#include "travel.hpp"
#include "tutorial.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftl {
// WarningMessage/AnimationTracker source state. The flash tracker keeps running
// after the main warning stops; looping trackers clamp before restarting.
struct HudWarning {
    float elapsed = 0;
    float flash = 0;
    bool running = false;
    bool flashRunning = false;
    bool loop = false;
    FTL_NATIVE_JSON(HudWarning, elapsed, flash, running, flashRunning, loop)
};

struct RunSystem {
    SystemDefinition definition;
    int power = 0;
    int health = 0;
    int batteryPower = 0;
    int bonusPower = 0;
    float ion = 0;
    float ionTimer = 0;
    // ShipSystem owns the shared ion and ability lock, including its -1 sentinel.
    int lockCount = 0;
    float lockElapsed = 0;
    int hackLevel = 0;
    bool hackActive = false;
    bool operatorActive = false;
    int manningLevel = 0;
    bool occupied = false;
    bool friendlies = false;
    bool onFire = false;
    bool damagedLastFrame = false;
    bool repairedLastFrame = false;
    bool exploded = false;
    float repair = 0;
    float fireDamage = 0;
    int restorePower = 0;
    CombatWeapon artillery{};
    int powerLimit = 8;
    int powerLoss = 0;
    int powerDivide = 1;
    std::vector<float> damagingEffectTimes;
    // ShipSystem::OnLoop compares this previous callback's full bonus allocation.
    int lastBonusPower = 0;

    friend void to_json(nlohmann::json& nlohmann_json_j, const RunSystem& nlohmann_json_t) {
        nlohmann_json_j["damagingEffectTimes"] = nlohmann_json_t.damagingEffectTimes;
        nlohmann_json_j["manningLevel"] = nlohmann_json_t.manningLevel;
        nlohmann_json_j["occupied"] = nlohmann_json_t.occupied;
        nlohmann_json_j["friendlies"] = nlohmann_json_t.friendlies;
        nlohmann_json_j["onFire"] = nlohmann_json_t.onFire;
        nlohmann_json_j["damagedLastFrame"] = nlohmann_json_t.damagedLastFrame;
        nlohmann_json_j["repairedLastFrame"] = nlohmann_json_t.repairedLastFrame;
        nlohmann_json_j["exploded"] = nlohmann_json_t.exploded;
        nlohmann_json_j["lockCount"] = nlohmann_json_t.lockCount;
        nlohmann_json_j["lockElapsed"] = nlohmann_json_t.lockElapsed;
        nlohmann_json_j["lastBonusPower"] = nlohmann_json_t.lastBonusPower;
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 definition,
                                                 power,
                                                 health,
                                                 batteryPower,
                                                 bonusPower,
                                                 ion,
                                                 ionTimer,
                                                 hackLevel,
                                                 hackActive,
                                                 operatorActive,
                                                 repair,
                                                 fireDamage,
                                                 restorePower,
                                                 artillery,
                                                 powerLimit,
                                                 powerLoss,
                                                 powerDivide))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j, RunSystem& nlohmann_json_t) {
        const RunSystem nlohmann_json_default_obj{};
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 damagingEffectTimes,
                                                 damagedLastFrame,
                                                 repairedLastFrame,
                                                 exploded,
                                                 occupied,
                                                 friendlies,
                                                 onFire))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM,
                                                 definition,
                                                 power,
                                                 health,
                                                 batteryPower,
                                                 bonusPower,
                                                 ion,
                                                 ionTimer,
                                                 hackLevel,
                                                 hackActive,
                                                 operatorActive,
                                                 repair,
                                                 fireDamage,
                                                 restorePower,
                                                 artillery,
                                                 powerLimit,
                                                 powerLoss))
        // Historical snapshots omitted the prior callback's bonus. Preserve the
        // current allocation without inventing a bonus-loss repower on loading.
        nlohmann_json_t.lastBonusPower =
            nlohmann_json_j.contains("lastBonusPower")
                ? nativeValue<int>(nlohmann_json_j.at("lastBonusPower"))
                : nlohmann_json_t.bonusPower;
        nlohmann_json_t.powerDivide = nlohmann_json_j.contains("powerDivide")
                                          ? nativeValue<int>(nlohmann_json_j.at("powerDivide"))
                                          : 1;
        if (nlohmann_json_j.contains("lockCount") != nlohmann_json_j.contains("lockElapsed")) {
            throw std::out_of_range("incomplete_system_lock");
        }
        if (nlohmann_json_j.contains("lockCount")) {
            nlohmann_json_t.lockCount = nativeValue<int>(nlohmann_json_j.at("lockCount"));
            nlohmann_json_t.lockElapsed = nativeValue<float>(nlohmann_json_j.at("lockElapsed"));
        } else {
            // Detached legacy systems retained only ion stacks. Whole-run loading
            // also reconstructs the separate old ability cooldown when available.
            const double oldCount = nlohmann_json_t.ion;
            if (!std::isfinite(oldCount) || oldCount < 0 ||
                oldCount > std::numeric_limits<int>::max() || std::floor(oldCount) != oldCount) {
                throw std::out_of_range("invalid_legacy_system_lock");
            }
            nlohmann_json_t.lockCount = static_cast<int>(oldCount);
            nlohmann_json_t.lockElapsed = oldCount > 0 ? 5.f - nlohmann_json_t.ionTimer : 0.f;
        }
        if (nlohmann_json_t.lockCount < -1 || nlohmann_json_t.lockElapsed < 0 ||
            nlohmann_json_t.lockElapsed > 5 || !std::isfinite(nlohmann_json_t.lockElapsed)) {
            throw std::out_of_range("invalid_system_lock");
        }
        // Older checkpoints retained only the active boolean, not its skill tier.
        nlohmann_json_t.manningLevel = nlohmann_json_j.contains("manningLevel")
                                           ? nativeValue<int>(nlohmann_json_j.at("manningLevel"))
                                           : int(nlohmann_json_t.operatorActive);
        if (nlohmann_json_t.manningLevel < 0 || nlohmann_json_t.manningLevel > 3) {
            throw std::out_of_range("invalid_system_manning_level");
        }
    }
};

struct CrewWaypoint {
    SDL_FPoint point;
    int door = -1;
    FTL_NATIVE_JSON(CrewWaypoint, point, door)
};

struct CrewAiState {
    bool medbayRequested = false;
    bool hurtCrew = false;
    bool calmShip = false;
    bool multiracial = false;
    bool overrideRace = false;
    int teleportRequest = 0;
    bool urgentTeleport = false;
    int startingCrewCount = 0;
    std::set<int> breachedRooms;
    FTL_NATIVE_JSON_DEFAULT(CrewAiState,
                            medbayRequested,
                            hurtCrew,
                            calmShip,
                            multiracial,
                            overrideRace,
                            teleportRequest,
                            urgentTeleport,
                            startingCrewCount,
                            breachedRooms)
};

struct RunCrew {
    HangarCrew appearance;
    std::string attackAnimation;
    float x = 0;
    float y = 0;
    float health = 100;
    float lastPresentedHealth = -1;
    float healthChangeRemaining = 0;
    float healthFlashTime = 0;
    float attackElapsed = 0;
    float attackGoal = 0;
    float shootGoal = 0;
    float shootElapsed = 0;
    float pendingDamage = 0;
    // HP depletion starts status 3; bDead and bOutOfGame are later phases.
    float deathAnimationElapsed = 0;
    std::uint64_t deathNumber = 0;
    std::uint64_t fireWorkTick = std::numeric_limits<std::uint64_t>::max();
    float medicalRate = 0;
    int onFire = 0;
    int repairKind = 0;
    // Presentation tracker for the rock's retained extinguishing strip.
    float fireWorkElapsed = 0;
    // CrewAnimation::smokeEmitter and status 4 (native checkpoints only; see CrewSmoke).
    CrewSmoke smoke{};
    int repairTarget = -1;
    int repairRoom = -1;
    int taskId = 4;
    int taskRoom = -1;
    int taskSystem = -1;
    float healingElapsed = -1;
    float mindElapsed = -1;
    float stunElapsed = -1;
    float attackFlight = 0;
    float attackAnimationElapsed = 0;
    SDL_FPoint attackFrom{};
    SDL_FPoint attackTo{};
    std::array<int, 4> statistics{};
    std::array<int, 6> earnedSkillTiers{};
    int attackTargetOwner = -1;
    int attackTargetIndex = -1;
    std::uint64_t attackTargetDroneId = 0;
    std::uint64_t physicalCrewOrder = 0;
    float crystalPowerElapsed = 0;
    float stunTime = 0;
    // CrewMember::GetIntruder 0x46c3e0 is true while the member is aboard a ship other
    // than its own (`currentShipId != iShipId`). A boarding party therefore lives in
    // the player's own crew list, which is the list CrewMemberFactory counts for
    // CompleteShip::DeadCrew 0x409930, and records the enemy-ship room it occupies in
    // `boardRoom`.
    int boardRoom = -1;
    // Keep home coordinates stable while the member navigates the opposing ship.
    float boardX = 0;
    float boardY = 0;
    int boardSlot = 0;
    int boardTargetRoom = -1;
    int boardTargetSlot = 0;
    std::deque<CrewWaypoint> boardPath;
    int boardLastDoor = -1;
    int boardBlockingDoor = -1;
    // Stable home slot for an actual hostile crew actor aboard the player ship.
    int enemyCrewOrigin = -1;
    // CrewMember::SetHealthBoost 0x470030 stores the applied mind-control health boost
    // so MindSystem::ReleaseCrew 0x609d40 can remove exactly that delta.
    float healthBoost = 0;
    int weaponSkill = 0;
    int weaponSkillProgress = 0;
    int repairSkill = 0;
    int repairSkillProgress = 0;
    std::array<int, 3> manningSkill{};
    std::array<int, 3> manningSkillProgress{};
    int combatSkill = 0;
    int combatSkillProgress = 0;
    int room = -1;
    int slot = 0;
    int targetRoom = -1;
    int targetSlot = 0;
    int savedRoom = -1;
    int savedSlot = 0;
    std::uint64_t lowHealthTick = 0;
    std::uint64_t deathTick = 0;
    std::string facing = "down";
    std::deque<CrewWaypoint> path;
    int lastDoor = -1;
    int blockingDoor = -1;
    // Group native flags to avoid per-field alignment gaps; persistence uses named fields.
    bool dying = false;
    bool deathAnimationDone = false;
    bool dead = false;
    bool outOfGame = false;
    bool cloneReady = false;
    // CrewMember::bCloned, set by Clone 0x470650: a kill of a cloned actor
    // grants no combat skill or kill statistic (CrewMember::OnLoop 0x4747f0).
    bool cloned = false;
    CrewTask aiTask{};
    bool suffocating = false;
    bool fireWorkLanded = false;
    bool recordMale = true;
    bool attackHit = false;
    bool attackTargetDrone = false;
    bool controlled = false;
    bool intruder = false;
    bool boardPositionValid = false;
    bool boardNewGoal = false;
    bool selected = false;
    bool newGoal = false;
    // CrewMember::skillUp 0x47a400: two rows of six 1-second trackers
    // (gains 0-5, clone losses 6-11). Presentation only; not persisted.
    std::array<float, 12> skillUpRemaining{};
    // CrewMember::StartTeleportArrive 0x472db0: remaining seconds of the
    // reversed status-6 teleport strip after arriving on a ship.
    float teleportArrive = 0;
    // CrewMember::StartTeleport 0x472d40: remaining forward strip before the
    // transfer, the pending command (0 none, see advanceTeleportDepartures) and
    // its destination room.
    float teleportLeave = 0;
    int teleportLeaveKind = 0;
    int teleportLeaveRoom = -1;

    friend void to_json(nlohmann::json& j, const RunCrew& crew) {
        j = nlohmann::json{{"appearance", crew.appearance},
                           {"x", crew.x},
                           {"y", crew.y},
                           {"health", crew.health},
                           {"lastPresentedHealth", crew.lastPresentedHealth},
                           {"healthChangeRemaining", crew.healthChangeRemaining},
                           {"healthFlashTime", crew.healthFlashTime},
                           {"attackElapsed", crew.attackElapsed},
                           {"attackGoal", crew.attackGoal},
                           {"shootGoal", crew.shootGoal},
                           {"shootElapsed", crew.shootElapsed},
                           {"pendingDamage", crew.pendingDamage},
                           {"dying", crew.dying},
                           {"deathAnimationElapsed", crew.deathAnimationElapsed},
                           {"deathAnimationDone", crew.deathAnimationDone},
                           {"dead", crew.dead},
                           {"outOfGame", crew.outOfGame},
                           {"cloneReady", crew.cloneReady},
                           {"cloned", crew.cloned},
                           {"deathNumber", crew.deathNumber},
                           {"medicalRate", crew.medicalRate},
                           {"onFire", crew.onFire},
                           {"suffocating", crew.suffocating},
                           {"repairKind", crew.repairKind},
                           {"repairTarget", crew.repairTarget},
                           {"repairRoom", crew.repairRoom},
                           {"taskId", crew.taskId},
                           {"taskRoom", crew.taskRoom},
                           {"taskSystem", crew.taskSystem},
                           {"healingElapsed", crew.healingElapsed},
                           {"mindElapsed", crew.mindElapsed},
                           {"stunElapsed", crew.stunElapsed},
                           {"attackFlight", crew.attackFlight},
                           {"attackAnimationElapsed", crew.attackAnimationElapsed},
                           {"attackHit", crew.attackHit},
                           {"attackTargetOwner", crew.attackTargetOwner},
                           {"attackTargetIndex", crew.attackTargetIndex},
                           {"attackTargetDrone", crew.attackTargetDrone},
                           {"attackTargetDroneId", crew.attackTargetDroneId},
                           {"physicalCrewOrder", crew.physicalCrewOrder},
                           {"attackAnimation", crew.attackAnimation},
                           {"attackFrom", {crew.attackFrom.x, crew.attackFrom.y}},
                           {"attackTo", {crew.attackTo.x, crew.attackTo.y}},
                           {"statistics", crew.statistics},
                           {"earnedSkillTiers", crew.earnedSkillTiers},
                           {"recordMale", crew.recordMale},
                           {"crystalPowerElapsed", crew.crystalPowerElapsed},
                           {"stunTime", crew.stunTime},
                           {"controlled", crew.controlled},
                           {"intruder", crew.intruder},
                           {"boardRoom", crew.boardRoom},
                           {"boardPositionValid", crew.boardPositionValid},
                           {"boardX", crew.boardX},
                           {"boardY", crew.boardY},
                           {"boardSlot", crew.boardSlot},
                           {"boardTargetRoom", crew.boardTargetRoom},
                           {"boardTargetSlot", crew.boardTargetSlot},
                           {"boardNewGoal", crew.boardNewGoal},
                           {"boardPath", crew.boardPath},
                           {"boardLastDoor", crew.boardLastDoor},
                           {"enemyCrewOrigin", crew.enemyCrewOrigin},
                           {"healthBoost", crew.healthBoost},
                           {"weaponSkill", crew.weaponSkill},
                           {"weaponSkillProgress", crew.weaponSkillProgress},
                           {"repairSkill", crew.repairSkill},
                           {"repairSkillProgress", crew.repairSkillProgress},
                           {"manningSkill", crew.manningSkill},
                           {"manningSkillProgress", crew.manningSkillProgress},
                           {"combatSkill", crew.combatSkill},
                           {"combatSkillProgress", crew.combatSkillProgress},
                           {"room", crew.room},
                           {"slot", crew.slot},
                           {"targetRoom", crew.targetRoom},
                           {"targetSlot", crew.targetSlot},
                           {"savedRoom", crew.savedRoom},
                           {"savedSlot", crew.savedSlot},
                           {"selected", crew.selected},
                           {"newGoal", crew.newGoal},
                           {"lowHealthTick", crew.lowHealthTick},
                           {"deathTick", crew.deathTick},
                           {"facing", crew.facing},
                           {"path", crew.path},
                           {"lastDoor", crew.lastDoor},
                           {"blockingDoor", crew.blockingDoor},
                           {"boardBlockingDoor", crew.boardBlockingDoor},
                           {"teleportArrive", crew.teleportArrive},
                           {"teleportLeave", crew.teleportLeave},
                           {"teleportLeaveKind", crew.teleportLeaveKind},
                           {"teleportLeaveRoom", crew.teleportLeaveRoom},
                           {"smoke", crew.smoke},
                           {"fireWorkElapsed", crew.fireWorkElapsed},
                           {"fireWorkLanded", crew.fireWorkLanded}};
    }

    friend void from_json(const nlohmann::json& j, RunCrew& crew) {
        nativeRead(j.at("appearance"), crew.appearance);
        nativeRead(j.at("x"), crew.x);
        nativeRead(j.at("y"), crew.y);
        nativeRead(j.at("health"), crew.health);
        crew.lastPresentedHealth = j.contains("lastPresentedHealth")
                                       ? nativeValue<float>(j.at("lastPresentedHealth"))
                                       : crew.health;
        crew.healthChangeRemaining = j.contains("healthChangeRemaining")
                                         ? nativeValue<float>(j.at("healthChangeRemaining"))
                                         : 0.f;
        crew.healthFlashTime =
            j.contains("healthFlashTime") ? nativeValue<float>(j.at("healthFlashTime")) : 0.f;
        crew.attackElapsed =
            j.contains("attackElapsed") ? nativeValue<float>(j.at("attackElapsed")) : 0.f;
        crew.attackGoal = j.contains("attackGoal") ? nativeValue<float>(j.at("attackGoal")) : 0.f;
        crew.shootGoal = j.contains("shootGoal") ? nativeValue<float>(j.at("shootGoal")) : 0.f;
        crew.shootElapsed =
            j.contains("shootElapsed") ? nativeValue<float>(j.at("shootElapsed")) : 0.f;
        crew.pendingDamage =
            j.contains("pendingDamage") ? nativeValue<float>(j.at("pendingDamage")) : 0.f;
        crew.dying = j.contains("dying") && nativeValue<bool>(j.at("dying"));
        crew.deathAnimationElapsed = j.contains("deathAnimationElapsed")
                                         ? nativeValue<float>(j.at("deathAnimationElapsed"))
                                         : 0.f;
        crew.deathAnimationDone =
            j.contains("deathAnimationDone") && nativeValue<bool>(j.at("deathAnimationDone"));
        crew.dead = j.contains("dead") ? nativeValue<bool>(j.at("dead")) : crew.health <= 0;
        crew.outOfGame =
            j.contains("outOfGame") ? nativeValue<bool>(j.at("outOfGame")) : crew.health <= 0;
        crew.cloneReady = j.contains("cloneReady") && nativeValue<bool>(j.at("cloneReady"));
        crew.cloned = j.contains("cloned") && nativeValue<bool>(j.at("cloned"));
        // The smoke emitter and the rock's stomp tracker draw the session stream
        // (ParticleEmitter Birth/Seed); older checkpoints start them fresh.
        crew.smoke = j.contains("smoke") ? nativeValue<CrewSmoke>(j.at("smoke")) : CrewSmoke{};
        crew.fireWorkElapsed =
            j.contains("fireWorkElapsed") ? nativeValue<float>(j.at("fireWorkElapsed")) : 0.f;
        crew.fireWorkLanded =
            j.contains("fireWorkLanded") && nativeValue<bool>(j.at("fireWorkLanded"));
        if (!std::isfinite(crew.fireWorkElapsed) || crew.fireWorkElapsed < 0 ||
            crew.fireWorkElapsed > 1) {
            throw std::out_of_range("invalid_crew_fire_work");
        }
        crew.deathNumber =
            j.contains("deathNumber") ? nativeValue<std::uint64_t>(j.at("deathNumber")) : 0;
        if (crew.deathAnimationElapsed < 0 || crew.deathAnimationElapsed > 60) {
            throw std::out_of_range("invalid_crew_death_animation");
        }
        crew.medicalRate =
            j.contains("medicalRate") ? nativeValue<float>(j.at("medicalRate")) : 0.f;
        crew.onFire = j.contains("onFire") ? nativeValue<int>(j.at("onFire")) : 0;
        crew.suffocating = j.contains("suffocating") && nativeValue<bool>(j.at("suffocating"));
        crew.repairKind = j.contains("repairKind") ? nativeValue<int>(j.at("repairKind")) : 0;
        crew.repairTarget =
            j.contains("repairTarget") ? nativeValue<int>(j.at("repairTarget")) : -1;
        crew.repairRoom = j.contains("repairRoom") ? nativeValue<int>(j.at("repairRoom")) : -1;
        crew.taskId = j.contains("taskId") ? nativeValue<int>(j.at("taskId")) : 4;
        crew.taskRoom = j.contains("taskRoom") ? nativeValue<int>(j.at("taskRoom")) : -1;
        crew.taskSystem = j.contains("taskSystem") ? nativeValue<int>(j.at("taskSystem")) : -1;
        if (crew.taskId < -1 || crew.taskId > 8 || crew.taskRoom < -1 || crew.taskRoom > 255 ||
            crew.taskSystem < -1 || crew.taskSystem > 15) {
            throw std::out_of_range("invalid_crew_task");
        }
        if (crew.repairKind < 0 || crew.repairKind > 3 || crew.repairTarget < -1 ||
            crew.repairTarget > 255 || crew.repairRoom < -1 || crew.repairRoom > 255) {
            throw std::out_of_range("invalid_crew_repair_metadata");
        }
        if (crew.onFire < 0 || crew.onFire > 256) {
            throw std::out_of_range("invalid_crew_fire_metadata");
        }
        crew.healingElapsed =
            j.contains("healingElapsed") ? nativeValue<float>(j.at("healingElapsed")) : -1.f;
        crew.mindElapsed =
            j.contains("mindElapsed") ? nativeValue<float>(j.at("mindElapsed")) : -1.f;
        crew.stunElapsed =
            j.contains("stunElapsed") ? nativeValue<float>(j.at("stunElapsed")) : -1.f;
        crew.attackFlight =
            j.contains("attackFlight") ? nativeValue<float>(j.at("attackFlight")) : 0.f;
        crew.attackAnimationElapsed = j.contains("attackAnimationElapsed")
                                          ? nativeValue<float>(j.at("attackAnimationElapsed"))
                                          : 0.f;
        crew.attackHit = j.contains("attackHit") && nativeValue<bool>(j.at("attackHit"));
        crew.attackTargetOwner =
            j.contains("attackTargetOwner") ? nativeValue<int>(j.at("attackTargetOwner")) : -1;
        crew.attackTargetIndex =
            j.contains("attackTargetIndex") ? nativeValue<int>(j.at("attackTargetIndex")) : -1;
        crew.attackTargetDrone =
            j.contains("attackTargetDrone") && nativeValue<bool>(j.at("attackTargetDrone"));
        crew.attackTargetDroneId = j.contains("attackTargetDroneId")
                                       ? nativeValue<std::uint64_t>(j.at("attackTargetDroneId"))
                                       : 0;
        crew.physicalCrewOrder = j.contains("physicalCrewOrder")
                                     ? nativeValue<std::uint64_t>(j.at("physicalCrewOrder"))
                                     : 0;
        crew.attackAnimation =
            j.contains("attackAnimation") ? nativeValue<std::string>(j.at("attackAnimation")) : "";
        if (j.contains("attackFrom")) {
            const auto point = nativeValue<std::array<float, 2>>(j.at("attackFrom"));
            crew.attackFrom = {point[0], point[1]};
        }
        if (j.contains("attackTo")) {
            const auto point = nativeValue<std::array<float, 2>>(j.at("attackTo"));
            crew.attackTo = {point[0], point[1]};
        }
        if (crew.lastPresentedHealth < -1 || crew.lastPresentedHealth > 230 ||
            crew.healthChangeRemaining < 0 || crew.healthChangeRemaining > 2 ||
            crew.healthFlashTime < 0 || crew.healthFlashTime >= 1 || crew.attackElapsed < 0 ||
            crew.attackElapsed > 3 || crew.attackGoal < 0 || crew.attackGoal > 1.3f ||
            crew.shootGoal < 0 || crew.shootGoal > 1.3f ||
            (crew.shootGoal > 0 && crew.shootGoal < 1.f) || crew.shootElapsed < 0 ||
            crew.shootElapsed > 1.31f || crew.pendingDamage < 0 || crew.pendingDamage > 7.f ||
            std::abs(crew.medicalRate) > 1000.f || crew.healingElapsed < -1 ||
            crew.healingElapsed > 1.02f || crew.mindElapsed < -1 || crew.mindElapsed > 1.02f ||
            crew.stunElapsed < -1 || crew.stunElapsed > 1.02f || crew.attackFlight < 0 ||
            crew.attackFlight > 3 || crew.attackAnimationElapsed < 0 ||
            crew.attackAnimationElapsed > 3 ||
            (!crew.attackAnimation.empty() && crew.attackAnimation != "punch_down" &&
             crew.attackAnimation != "punch_left" && crew.attackAnimation != "punch_right" &&
             crew.attackAnimation != "punch_up" && crew.attackAnimation != "shoot_down" &&
             crew.attackAnimation != "shoot_left" && crew.attackAnimation != "shoot_right" &&
             crew.attackAnimation != "shoot_up") ||
            std::abs(crew.attackFrom.x) > 4096 || std::abs(crew.attackFrom.y) > 4096 ||
            std::abs(crew.attackTo.x) > 4096 || std::abs(crew.attackTo.y) > 4096) {
            throw std::out_of_range("invalid_crew_combat_presentation");
        }
        crew.statistics = j.contains("statistics")
                              ? nativeValue<std::array<int, 4>>(j.at("statistics"))
                              : std::array<int, 4>{};
        crew.earnedSkillTiers = j.contains("earnedSkillTiers")
                                    ? nativeValue<std::array<int, 6>>(j.at("earnedSkillTiers"))
                                    : std::array<int, 6>{};
        for (const int count : crew.statistics) {
            if (count < 0) {
                throw std::out_of_range("invalid_crew_statistics");
            }
        }
        for (const int mask : crew.earnedSkillTiers) {
            if (mask < 0 || mask > 3) {
                throw std::out_of_range("invalid_earned_skill_tiers");
            }
        }
        crew.recordMale = j.contains("recordMale") ? nativeValue<bool>(j.at("recordMale"))
                                                   : crew.appearance.species != "human" ||
                                                         (crew.appearance.appearance % 2) == 0;
        crew.crystalPowerElapsed = j.contains("crystalPowerElapsed")
                                       ? nativeValue<float>(j.at("crystalPowerElapsed"))
                                       : 0.f;
        if (crew.crystalPowerElapsed < 0 || crew.crystalPowerElapsed > 50) {
            throw std::out_of_range("invalid_crystal_power_elapsed");
        }
        nativeRead(j.at("stunTime"), crew.stunTime);
        nativeRead(j.at("controlled"), crew.controlled);
        // Version-1 checkpoints predate the boarding party and mind-control boost.
        crew.intruder = j.contains("intruder") ? nativeValue<bool>(j.at("intruder")) : false;
        const int boardRoom = j.contains("boardRoom") ? nativeValue<int>(j.at("boardRoom")) : -1;
        if (boardRoom < -1 || boardRoom > 63) {
            throw std::out_of_range("invalid_crew_board_room");
        }
        crew.boardRoom = boardRoom;
        crew.boardPositionValid =
            j.contains("boardPositionValid") && nativeValue<bool>(j.at("boardPositionValid"));
        crew.boardX = j.contains("boardX") ? nativeValue<float>(j.at("boardX")) : 0.f;
        crew.boardY = j.contains("boardY") ? nativeValue<float>(j.at("boardY")) : 0.f;
        crew.boardSlot = j.contains("boardSlot") ? nativeValue<int>(j.at("boardSlot")) : 0;
        crew.boardTargetRoom =
            j.contains("boardTargetRoom") ? nativeValue<int>(j.at("boardTargetRoom")) : -1;
        crew.boardTargetSlot =
            j.contains("boardTargetSlot") ? nativeValue<int>(j.at("boardTargetSlot")) : 0;
        crew.boardNewGoal = j.contains("boardNewGoal") && nativeValue<bool>(j.at("boardNewGoal"));
        if (j.contains("boardPath")) {
            nativeRead(j.at("boardPath"), crew.boardPath);
        }
        crew.enemyCrewOrigin =
            j.contains("enemyCrewOrigin") ? nativeValue<int>(j.at("enemyCrewOrigin")) : -1;
        if (crew.enemyCrewOrigin < -1 || crew.enemyCrewOrigin > 63) {
            throw std::out_of_range("invalid_enemy_crew_origin");
        }
        const float healthBoost =
            j.contains("healthBoost") ? nativeValue<float>(j.at("healthBoost")) : 0.f;
        // HEALTH_BOOST 0x837d60 tops out at 30; a non-positive or NaN sample stores none.
        if (healthBoost < 0 || healthBoost > 30) {
            throw std::out_of_range("invalid_crew_health_boost");
        }
        crew.healthBoost = healthBoost > 0 ? healthBoost : 0.f;
        nativeRead(j.at("weaponSkill"), crew.weaponSkill);
        nativeRead(j.at("weaponSkillProgress"), crew.weaponSkillProgress);
        nativeRead(j.at("repairSkill"), crew.repairSkill);
        nativeRead(j.at("repairSkillProgress"), crew.repairSkillProgress);
        nativeRead(j.at("manningSkill"), crew.manningSkill);
        nativeRead(j.at("manningSkillProgress"), crew.manningSkillProgress);
        nativeRead(j.at("room"), crew.room);
        nativeRead(j.at("slot"), crew.slot);
        nativeRead(j.at("targetRoom"), crew.targetRoom);
        nativeRead(j.at("targetSlot"), crew.targetSlot);
        nativeRead(j.at("savedRoom"), crew.savedRoom);
        nativeRead(j.at("savedSlot"), crew.savedSlot);
        nativeRead(j.at("selected"), crew.selected);
        nativeRead(j.at("newGoal"), crew.newGoal);
        nativeRead(j.at("lowHealthTick"), crew.lowHealthTick);
        nativeRead(j.at("deathTick"), crew.deathTick);
        nativeRead(j.at("facing"), crew.facing);
        nativeRead(j.at("path"), crew.path);
        crew.lastDoor = j.contains("lastDoor") ? nativeValue<int>(j.at("lastDoor")) : -1;
        crew.boardLastDoor =
            j.contains("boardLastDoor") ? nativeValue<int>(j.at("boardLastDoor")) : -1;
        crew.blockingDoor =
            j.contains("blockingDoor") ? nativeValue<int>(j.at("blockingDoor")) : -1;
        crew.boardBlockingDoor =
            j.contains("boardBlockingDoor") ? nativeValue<int>(j.at("boardBlockingDoor")) : -1;
        // Teleport strips (StartTeleport 0x472d40 / StartTeleportArrive 0x472db0);
        // older checkpoints predate them and hold no actor mid-strip.
        crew.teleportArrive =
            j.contains("teleportArrive") ? nativeValue<float>(j.at("teleportArrive")) : 0.f;
        crew.teleportLeave =
            j.contains("teleportLeave") ? nativeValue<float>(j.at("teleportLeave")) : 0.f;
        crew.teleportLeaveKind =
            j.contains("teleportLeaveKind") ? nativeValue<int>(j.at("teleportLeaveKind")) : 0;
        crew.teleportLeaveRoom =
            j.contains("teleportLeaveRoom") ? nativeValue<int>(j.at("teleportLeaveRoom")) : -1;
        if (crew.teleportArrive < 0 || crew.teleportArrive > 10 || crew.teleportLeave < 0 ||
            crew.teleportLeave > 10 || crew.teleportLeaveKind < 0 || crew.teleportLeaveKind > 5 ||
            crew.teleportLeaveRoom < -1 || crew.teleportLeaveRoom > 63) {
            throw std::out_of_range("invalid_crew_teleport_strip");
        }

        const int cap = combatSkillCap(crew.appearance.species);
        if (j.contains("combatSkillProgress")) {
            const int progress = nativeValue<int>(j.at("combatSkillProgress"));
            if (progress < 0 || progress > cap) {
                throw std::out_of_range("invalid_combat_skill_progress");
            }
            crew.combatSkillProgress = progress;
            crew.combatSkill = combatSkillTier(progress, cap);
            if (j.contains("combatSkill") &&
                nativeValue<int>(j.at("combatSkill")) != crew.combatSkill) {
                throw std::out_of_range("inconsistent_combat_skill_tier");
            }
        } else if (j.contains("combatSkill")) {
            const int tier = nativeValue<int>(j.at("combatSkill"));
            if (tier < 0 || tier > 2) {
                throw std::out_of_range("invalid_combat_skill_tier");
            }
            crew.combatSkill = tier;
            crew.combatSkillProgress = tier == 2 ? cap : tier == 1 ? cap / 2 : 0;
        } else {
            // Version-1 checkpoints predate this skill and defaulted it to zero.
            crew.combatSkill = 0;
            crew.combatSkillProgress = 0;
        }
    }
};

struct EventResources {
    int scrap = 0;
    int fuel = 0;
    int missiles = 0;
    int drones = 0;
    int pursuit = 0;

    bool visible() const {
        return scrap || fuel || missiles || drones;
    }
    FTL_NATIVE_JSON(EventResources, scrap, fuel, missiles, drones, pursuit)
};

// ChoiceBox::MeasureResources 0x4ebaf0 / ResourceBoxDesc: positioned reward
// entries plus the box size and, for a wrapped choice preview, each row's width.
struct RewardLayout {
    nlohmann::json entries = nlohmann::json::array();
    int width = 0;
    int height = 0;
    int row1Width = 0;
    int row1Height = 0;
    int row2Width = 0;
};

struct RunChoice {
    std::string text;
    pugi::xml_node event;
    bool enabled = true;
    bool raceChoice = false;
    bool blueChoice = false;
    bool hiddenReward = false;
    // Index of the <choice> in the event's full list (WorldManager::PauseLoop
    // 0x5f62b0 actual_choice); -1 for native-only dialogs.
    int authoredIndex = -1;
};

// Preserve ownership groups and declaration order: constructors consume the
// shared random stream. Padding in this single application owner is deliberate.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
class Run {
    Renderer& renderer_;
    const Archive& archive_;
    Audio& audio_;

    struct InventoryFactory {
        CombatWeapon weapon;
        CombatDrone drone;
        FTL_NATIVE_JSON(InventoryFactory, weapon, drone)
    };

    InventoryFactory makeInventoryFactory(const std::string& id);

    struct MapNebula {
        int x;
        int y;
        int variant;
        FTL_NATIVE_JSON(MapNebula, x, y, variant)
    };

    bool canFly() const;
    bool canOpenMap() const;
    Rect nextSectorButtonRect() const;
    void openMap();
    void jump(int destination);
    void advanceTravel();
    void convertFleetLocation(int index);
    void advanceTick();
    void travelMap();
    int hoveredBeacon() const;
    int sectorPotentialAt(float x, float y) const;
    float targetBoxTime() const;
    void enterEvent(pugi::xml_node node, bool newLocation = false);
    void clearLocation();
    void clearPlayerLocationState();
    void clearEnemyLocationCrew();
    void rebaseVoyageCrewReferences(int previousEnemyCount, int newEnemyCount);
    void choose(int index);
    bool canAfford(pugi::xml_node node) const;
    float choiceTop() const;
    nlohmann::json eventRewardEntries() const;
    RewardLayout
    rewardLayout(const EventResources& resources, nlohmann::json entries, bool choice) const;
    RewardLayout choiceRewardLayout(std::size_t index) const;
    void drawRewardEntries(const nlohmann::json& entries, float left, float top, SDL_Color color);
    float eventRewardHeight() const;

    float choiceRow(std::size_t index) const;
    float choiceHeight(std::size_t index) const;
    std::vector<std::string> choiceLines(std::size_t index) const;
    std::vector<std::string> dialogLines(const std::string& value, float width = 565) const;
    EventResources choiceResources(std::size_t index) const;
    float choiceTextWidth(std::size_t index) const;
    Font& localizedFont(int size, const std::string& value) const;
    float textScale(int size) const;
    float textLineHeight(int size, const std::string& value) const;
    std::vector<std::string> wrappedText(int size, const std::string& value, float width) const;

    std::vector<std::string> eventLines() const;
    EventResources eventResources(pugi::xml_node node, RandomStream& random) const;
    void applyResources(const EventResources& resources);
    nlohmann::json stageVoyageEffects(pugi::xml_node node,
                                      RandomStream& random,
                                      EventResources& resources,
                                      bool includeImages = true);
    std::string commitVoyageEffects(std::string* locationText = nullptr);
    pugi::xml_node resourceFollowupEvent(const std::string& id, pugi::xml_node next);
    pugi::xml_node createShipOutcomeEvent(pugi::xml_node outcome);
    void reseedShipOutcome();
    static float crewMaximumHealth(std::string_view species);
    void freezeVoyageEvent(pugi::xml_node node, RandomStream& random);
    std::string
    materializeEvent(pugi::xml_node node,
                     RandomStream& random,
                     bool advanced,
                     std::vector<std::string>& used,
                     const std::function<void(pugi::xml_node, RandomStream&)>& freeze = {},
                     bool ignoreUnique = false,
                     int forceRandomIndex = 0) const;
    void applyVoyageCrewSkills(RunCrew& member, const nlohmann::json& row) const;
    void applyVoyageImage(const nlohmann::json& row);
    void prepareBeaconImages(pugi::xml_node node);
    void restoreBeaconEnvironment(pugi::xml_node node);
    void restoreBeaconFleet(pugi::xml_node node);
    bool eventRequirement(const std::string& requirement, int minimum, int maximum) const;
    void endVoyageBoarding();
    static int systemCapacity(const RunSystem& sys);
    void resourceBox(const EventResources& resources,
                     float x,
                     float y,
                     bool choice = false,
                     SDL_Color color = {235, 245, 229, 255},
                     bool frame = true);
    static int resourceBoxWidth(const EventResources& resources);
    std::array<int, 4> resourceAmounts() const;
    void noteResourceChanges(const std::array<int, 4>& previous);
    void resetResourceFeedback();
    void updateResourceFeedback();

    struct VoyageButton {
        std::string target;
        std::string label;
        bool enabled = true;
    };

    struct StoreOffer {
        std::string item;
        int cost = 0;
        int stock = 0;
        std::string bonus{};
        int category = -1;
        FTL_NATIVE_JSON_DEFAULT(StoreOffer, item, cost, stock, bonus, category)
    };

    std::vector<VoyageButton> voyageButtons() const;

    struct PauseMenuLayout {
        Rect menu;
        int difficultyWidth = 0;
        int dlcWidth = 0;
        int achievementExtra = 0;
        int achievementWidth = 0;
    };

    PauseMenuLayout pauseMenuLayout() const;
    Rect pauseMenuRect() const;
    void renderPauseMenuStatus();
    void renderPauseTooltip();
    float managementPanelX() const;
    mutable std::string managementPanelLocale_;
    mutable float managementPanelCachedX_ = 340;
    std::string systemLevelDescription(int systemId, int level) const;
    void voyageRender();
    bool voyageAction(const std::string& target, const std::string& action);
    bool resumePausedShipScreen();
    const std::string& visibleOrPausedPanel() const;
    void voyageClick(float x, float y, bool right = false);
    bool docked() const;
    bool hostileEnvironment() const;
    bool shipButtonActive() const;
    bool hudStoreAvailable() const;
    Rect hudStoreRect() const;
    Rect hudOptionsRect() const;
    void populateSector();
    void prepareOpeningText();
    void applyStartLocation();
    pugi::xml_node sectorDescription() const;
    void startSectorMusic();
    void startSecretSector();
    std::map<std::string, int> sectorRarities() const;
    void nextSector(int choice);
    void initializeSectorTree();
    bool canWaitAtBeacon() const;
    void waitForFuel();
    void advanceVoyageUi();
    bool addQuest(const std::string& event, bool nextSector);
    void openStore();
    void createStore();
    bool installSystem(int id);
    void removeEquippedItem(const std::string& id);
    bool inventoryMove(const std::string& kind, int index, bool equip);
    void ensureInventorySlots();
    int logicalArmamentIndex(const std::string& kind, int slot) const;
    int logicalArmamentSlot(const std::string& kind, int denseIndex) const;
    void eraseInventorySlot(const std::string& kind, int denseIndex);
    bool inventoryCanHold(const std::string& id) const;
    bool inventoryCompletelyFull(const std::string& kind) const;
    bool inventoryAdd(const std::string& id,
                      bool overflow = false,
                      const InventoryFactory* existing = nullptr);
    std::string rewardEquipment(const std::string& id);
    void detachInventoryWeapon(int index);
    void detachInventoryDrone(int index);
    bool inventorySwap(int cargoIndex, const std::string& kind, int index);
    std::vector<VoyageButton> storeButtons(bool renderStock = false) const;
    bool storeAction(const std::string& target, const std::string& action);
    void storeRender();
    void storeClick(float x, float y);
    Rect inventoryOverflowRect(bool augment = false, bool itemOnly = false) const;
    std::string inventoryDraggedItem() const;
    std::string inventoryHoveredItem() const;
    void inventoryRender();
    void renderEquipmentInfo(const EquipmentDefinition& bp, float infoX, float infoY);
    void renderCrewInfo(const RunCrew& member, float infoX, float infoY);
    void renderSystemInfo(const RunSystem& system,
                          int displayedLevel,
                          float infoX,
                          float infoY,
                          int forceWidth = 0,
                          bool detailed = true);
    void inventoryClick(float x, float y);
    bool inventoryPointerDown(float x, float y);
    void inventoryPointerUp(float x, float y);
    bool hireCrew(const std::string& species);
    int pendingUpgrade(int id) const;
    bool tutorialBlocksPanelAccept() const;
    void finishUpgrades(bool accept);
    void crewTextInput(std::string_view value);
    void crewRenameKey(const std::string& key);
    void setEnvironment(const std::string& type);
    void advanceEnvironment();
    void restartEnvironmentFlashTimer();
    void advanceAsteroidEnvironment();
    void extractEnvironmentProjectiles();
    void renderEnvironment();
    void renderEnvironmentFlash();
    void renderEnvironmentWarnings();
    void advanceAsteroidScroll();
    void renderAsteroidScroll(int layers = 3);
    std::array<float, 3> asteroidScroll_{};
    void saveBeaconShip(int beacon = -1);
    void restoreBeaconShip();
    void renderSunEnvironment();
    void setPulsarEnvironment();
    void advancePulsarEnvironment();
    void renderPulsarEnvironment();

    struct Environment {
        bool fireActive = false;
        bool pulsarActive = false;
        bool pdsActive = false;
        bool asteroidActive = false;
        bool stormActive = false;
        int flashMinTime = 28000;
        int flashMaxTime = 34000;
        float flashTime = 0;
        float flashGoal = 0;
        float flashAnimationTime = -1;
        bool foregroundPulsar = false;
        std::uint64_t warningUiStart = 0;
        bool warningActive = false;
        int pulsarFlashTicks = 60;
        int sunTicks = 0;
        int flareTicks = 0;
        int flareGoalTicks = 0;
        int flashTicks = 0;
        bool flareSound = false;
        int asteroidState = 0;
        int asteroidStateTicks = 0;
        int asteroidSpawnTicks = 0;
        int asteroidTarget = 0;
        int asteroidBand = 0;
        // RandomAmount::SelectAmount 0x5a9360 raises its stored max by one on
        // every draw, so each state length and spawn rate widens as the
        // generator keeps sampling it (reset by AsteroidGenerator::Initialize).
        std::array<int, 3> asteroidStateDraws{};
        std::array<int, 3> asteroidRateDraws{};
        bool asteroidDefense = false;
        int fleet = 0;
        std::vector<std::array<int, 3>> fleetShips;
        int pdsTarget = 0;
        float pdsAmbient = 0;
        float pdsTime = 0;
        float pdsGoal = 0;
        std::vector<CombatProjectile> pdsQueue;
        std::vector<CombatProjectile> asteroidQueue;
        std::vector<std::array<float, 3>> pdsSmoke;
        FTL_NATIVE_JSON_DEFAULT(Environment,
                                fireActive,
                                pulsarActive,
                                pdsActive,
                                asteroidActive,
                                stormActive,
                                flashMinTime,
                                flashMaxTime,
                                flashTime,
                                flashGoal,
                                flashAnimationTime,
                                foregroundPulsar,
                                warningUiStart,
                                warningActive,
                                pulsarFlashTicks,
                                sunTicks,
                                flareTicks,
                                flareGoalTicks,
                                flashTicks,
                                flareSound,
                                asteroidState,
                                asteroidStateTicks,
                                asteroidSpawnTicks,
                                asteroidTarget,
                                asteroidBand,
                                asteroidStateDraws,
                                asteroidRateDraws,
                                asteroidDefense,
                                fleet,
                                fleetShips,
                                pdsTarget,
                                pdsAmbient,
                                pdsTime,
                                pdsGoal,
                                pdsQueue,
                                asteroidQueue,
                                pdsSmoke)
    };

    void advanceAdvanced();
    void advanceEnemyTeleporter();
    void releaseEnemyMind();
    void releasePlayerMind();
    void initiateMindControl(int owner);
    void raiseMindResists();
    void remapMindQueues(int owner, const std::function<int(int)>& remap);
    void reindexCrewReferences(int owner, int removed);
    void reindexDroneReferences(int owner, int changed, bool removed = false);
    void refreshCrewNames() const;
    void updateCrewAudio();
    void advanceTeleporter();
    void healTeleportCrew(RunCrew& member, bool enemy = false);
    float crewTeleportDuration(const RunCrew& member) const;
    void startTeleportDeparture(RunCrew& member, int kind, int room);
    void startTeleportArrival(RunCrew& member);
    void advanceTeleportDepartures();
    std::uint64_t crewTeleportArriveTick(const RunCrew& member) const;
    void placeBoardingCrew();
    void advanceCrewCombat();
    void advanceCrewLifecycle();
    void advanceCrewLifecycle(RunCrew& member, int creationOwner);
    void updateCrewHealthPresentation(RunCrew& member);
    void advanceCrewHealth(RunCrew& member, int creationOwner, bool drone = false);
    void finishCrewDeath(RunCrew& member);
    void killCrew(RunCrew& member, bool noClone);
    void resetCrewDroneDeployment(CombatDrone& drone);
    void undeployDrone(CombatDrone& drone);
    bool droneCanBeDeployed(const CombatDrone& drone, int owner) const;
    bool droneHasMovementTarget(const CombatDrone& drone, int owner) const;
    bool depowerDrone(CombatDrone& drone, RunSystem& controller, int owner);
    void retargetSpaceDrones(int owner);
    void initializeCrewAnimation(RunCrew& member, RandomStream& random);
    void initializeCrewDroneAnimation(CombatDrone& drone,
                                      RandomStream& random,
                                      const ShipDefinition* owningShip = nullptr,
                                      int owner = 0);
    void assignCrewDroneId(CombatDrone& drone);
    void retireCrewDrone(CombatDrone& drone);
    void detachCrewDroneShip(int ship);
    void blowUpDrone(CombatDrone& drone, bool silent = false);
    void assignPhysicalCrewOrder(RunCrew& member, bool reinsert = false);
    void assignPhysicalCrewOrder(CombatDrone& drone, bool reinsert = false);
    void resetCrewForClone(RunCrew& member);
    // `ship`/`owner` place a drone's temporary crew body, which belongs to no
    // crew list; ordinary crew derive both from their list.
    bool crewHasOpponent(const RunCrew& member, int ship = -1, int owner = -1) const;
    void updateCrewHealthPresentation();
    SDL_FPoint crewCombatPoint(const RunCrew& member) const;
    SDL_FPoint crewPresentationPoint(const RunCrew& member) const;
    std::string crewCombatAnimation(const RunCrew& member) const;
    std::uint64_t crewCombatTick(const RunCrew& member) const;
    std::uint64_t crewWalkTick(const std::string& animation, bool enemyOwner) const;
    float crewHealthAlpha(const RunCrew& member) const;
    void renderCrewShots(const RunCrew& member, float x, float y);
    void renderCrewHealth(const RunCrew& member, float x, float y, int ownerOverride = -1);
    int mindLevel() const;
    void resetAdvanced();
    bool advancedAction(const std::string& target, const std::string& action);
    bool activateAbility(int id, bool enemy = false);
    void stopAbility(int id, bool enemy = false);
    void advanceAbilities();
    int batteryPower(bool enemy = false) const;
    int reactorCapacity(bool enemy = false) const;
    void settleReactorPower();
    void devUiRender();
    void devUiClick(float x, float y);
    void devUiKey(const std::string& key);
    void devUiText(const std::string& value);
    void devUiAction(const std::string& action);
    nlohmann::json devUiState() const;
    void assignBatteryPower(RunSystem& system, int amount, bool enemy = false);
    void releaseBatteryPower(RunSystem& system, int amount, bool enemy = false);
    bool cloaked(int owner) const;
    void reconcilePower(bool enemy = false);
    bool systemLocked(const RunSystem& system) const;
    void setSystemLock(RunSystem& system, int count, bool enemy = false);
    void addSystemLock(RunSystem& system, int count, bool enemy = false);
    void applySystemIon(RunSystem& system, int amount, bool enemy = false);
    void syncSystemLock(RunSystem& system, bool enemy = false);
    void checkSystemRepower(RunSystem& system, bool enemy = false);
    void checkSystemMaxPower(RunSystem& system, bool enemy = false);
    bool forceIncreaseSystemPower(RunSystem& system, bool enemy = false);
    // Virtual ShipSystem::AddDamage 0x614e50 and its Shields/Weapon/Drone overrides.
    void addSystemDamage(RunSystem& system, int amount, bool enemy = false);
    void clampArmamentPower(RunSystem& system, bool enemy);
    // ShipSystem::RemoveBatteryPower 0x614b50 and its weapon/drone overrides.
    void removeBatteryPower(RunSystem& system, bool enemy = false);
    // WeaponSystem::PowerWeapon 0x6101c0 without user intent.
    bool powerWeaponSlot(RunSystem& system, std::size_t index, bool enemy, bool force);
    void cancelAim();
    int armamentAt(float x, float y, bool drone) const;
    bool swapArmaments(int first, int second, bool drone);
    void abilityHud(bool front = false);

    // Source subsystem/actor groups remain adjacent for lifecycle review; one
    // current encounter does not warrant changing construction order for padding.
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
    struct Enemy {
        ShipDefinition ship;
        std::vector<RunSystem> systems;
        std::vector<RunCrew> crew;
        std::vector<CombatWeapon> weapons;
        std::vector<CombatDrone> drones;
        std::vector<CombatDrone> droneTrash;
        std::vector<bool> powered;
        std::vector<bool> doors;
        std::vector<bool> fakeDoors;
        std::vector<float> doorFrames;
        std::vector<float> doorTimers;
        std::map<int, int> doorBaseHealth;
        std::map<int, float> doorDamage;
        std::map<int, int> doorForcedTicks;
        std::map<int, int> doorLevel;
        std::map<int, int> doorLastBaseHealth;
        std::set<int> doorLockdownBroken;
        std::map<int, float> roomOxygen;
        std::map<int, float> lockdownRooms;
        int hull = 0;
        int missiles = 0;
        int droneParts = 0;
        int shields = 0;
        int superShields = 0;
        int maxSuperShields = 0;
        std::array<float, 2> engineAnimationTimes{};
        std::map<int, std::vector<float>> roomFireAnimationTimes;
        std::map<int, std::vector<FireAnimationState>> fireSpreaderAnimations;
        float shieldCharge = 0;
        bool shieldExcessChargeHack = false;
        bool present = false;
        // The player's ShipManager::current_target. CombatControl::AddEnemyShip
        // sets it; WorldManager::OnLoop 0x5f6520 clears it (SetTarget(nullptr))
        // once the ship is destroyed, its crew dies or it escapes. A surrender
        // only drops the ship's hostile flag.
        bool playerTarget = true;
        bool powerFixtureOverride = false;
        bool fireWhileCloaked = false;
        // Historical reference captures use an explicitly requested diagnostic
        // reward; ordinary encounters always generate their authored outcome.
        bool pinnedRewardFixture = false;
        int rewardVariant = -1;
        int rewardTier = -1;
        std::string rewardType;
        bool rewardUnsupportedItem = false;
        bool rewardUnsupportedCrew = false;
        std::string rewardItem;
        bool rewardCommitted = false;
        std::string rewardCrewSpecies;
        std::string rewardCrewName;
        std::string rewardCrewFullName;
        bool rewardCrewMale = false;
        int rewardCrewAppearance = 0;
        bool rewardCrewColorsPrepared = true;
        bool rewardCrewPending = false;
        bool rewardCrewOverflow = false;
        bool hasSurrenderThreshold = false;
        int surrenderThresholdMin = 0;
        int surrenderThresholdMax = 0;
        float surrenderChanceNone = -1.f;
        int surrenderThresholdSelected = 0;
        int surrenderWorldLevelOffset = 0;
        int surrenderThreshold = 0;
        int escapeThreshold = 0;
        float escapeTimer = 15;
        bool escaping = false;
        float escapeWarningTime = 0;
        float escapeJumpElapsed = -1;
        bool stalemateTrigger = false;
        float stalemateTimer = 0;
        int lastEscapeHealth = 100;
        int intruderCount = 0;
        CrewAiState crewAi;
        std::vector<int> cloneQueue;
        int clonePending = -1;
        float cloneTimer = -1;
        float cloneDeathTimer = -1;
        float cloneDuration = 12;
        CombatAbility mind;
        int mindTarget = -1;
        int mindTargetOwner = 0;
        int reactorPowerCap = 64;
        int reactorPowerLoss = 0;
        int reactorPowerDivide = 1;
        CombatAbility hacking;
        int boardingAi = 0;
        int teleportStartingCrew = 0;
        int teleportWaves = 0;
        float teleportCharge = 0;
        int teleportRequest = 0;
        std::vector<int> teleportParty;
        CombatDrone hackingDrone{.blueprint = "DRONE_HACKING"};
        int hackingTarget = -1;
        int hackingDroneSpace = 1;
        std::uint64_t hackingDroneId = 0;
        CombatAbility cloak;
        CombatAbility battery;
        CombatExplosion explosionState;

        // Split the field expansion below the JSON macro argument limit.
        friend void to_json(nlohmann::json& nlohmann_json_j, const Enemy& nlohmann_json_t) {
            nlohmann_json_j["intruderCount"] = nlohmann_json_t.intruderCount;
            nlohmann_json_j["crewAi"] = nlohmann_json_t.crewAi;
            nlohmann_json_j["clonePending"] = nlohmann_json_t.clonePending;
            nlohmann_json_j["droneTrash"] = nlohmann_json_t.droneTrash;
            nlohmann_json_j["rewardCrewColorsPrepared"] = nlohmann_json_t.rewardCrewColorsPrepared;
            nlohmann_json_j["maxSuperShields"] = nlohmann_json_t.maxSuperShields;
            nlohmann_json_j["engineAnimationTimes"] = nlohmann_json_t.engineAnimationTimes;
            nlohmann_json_j["roomFireAnimationTimes"] = nlohmann_json_t.roomFireAnimationTimes;
            nlohmann_json_j["fireSpreaderAnimations"] = nlohmann_json_t.fireSpreaderAnimations;
            nlohmann_json_j["escapeWarningTime"] = nlohmann_json_t.escapeWarningTime;
            nlohmann_json_j["playerTarget"] = nlohmann_json_t.playerTarget;
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                     ship,
                                                     systems,
                                                     crew,
                                                     weapons,
                                                     drones,
                                                     powered,
                                                     doors,
                                                     fakeDoors,
                                                     doorFrames,
                                                     doorTimers,
                                                     roomOxygen,
                                                     lockdownRooms,
                                                     hull,
                                                     missiles,
                                                     droneParts,
                                                     shields,
                                                     superShields,
                                                     shieldCharge,
                                                     shieldExcessChargeHack,
                                                     present,
                                                     powerFixtureOverride,
                                                     fireWhileCloaked,
                                                     pinnedRewardFixture,
                                                     rewardVariant))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                     rewardTier,
                                                     rewardType,
                                                     rewardUnsupportedItem,
                                                     rewardUnsupportedCrew,
                                                     rewardItem,
                                                     rewardCommitted,
                                                     rewardCrewSpecies,
                                                     rewardCrewName,
                                                     rewardCrewFullName,
                                                     rewardCrewMale,
                                                     rewardCrewAppearance,
                                                     rewardCrewPending,
                                                     rewardCrewOverflow,
                                                     hasSurrenderThreshold,
                                                     surrenderThresholdMin,
                                                     surrenderThresholdMax,
                                                     surrenderChanceNone,
                                                     surrenderThresholdSelected,
                                                     surrenderWorldLevelOffset,
                                                     surrenderThreshold,
                                                     cloneQueue,
                                                     cloneTimer,
                                                     cloneDeathTimer,
                                                     cloneDuration))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                     mind,
                                                     mindTarget,
                                                     mindTargetOwner,
                                                     reactorPowerCap,
                                                     reactorPowerLoss,
                                                     reactorPowerDivide,
                                                     hacking,
                                                     boardingAi,
                                                     teleportStartingCrew,
                                                     teleportWaves,
                                                     teleportCharge,
                                                     teleportRequest,
                                                     teleportParty,
                                                     hackingDrone,
                                                     hackingTarget,
                                                     hackingDroneSpace,
                                                     hackingDroneId,
                                                     cloak,
                                                     battery,
                                                     explosionState))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                     escapeThreshold,
                                                     escapeTimer,
                                                     escaping,
                                                     escapeJumpElapsed,
                                                     stalemateTrigger,
                                                     stalemateTimer,
                                                     lastEscapeHealth,
                                                     doorBaseHealth,
                                                     doorDamage,
                                                     doorForcedTicks,
                                                     doorLevel,
                                                     doorLastBaseHealth,
                                                     doorLockdownBroken))
        }

        friend void from_json(const nlohmann::json& nlohmann_json_j, Enemy& nlohmann_json_t) {
            const Enemy nlohmann_json_default_obj{};
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, intruderCount))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, crewAi))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, clonePending))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, playerTarget))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, droneTrash))
            NLOHMANN_JSON_EXPAND(
                NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, rewardCrewColorsPrepared))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                     maxSuperShields,
                                                     engineAnimationTimes,
                                                     escapeWarningTime,
                                                     roomFireAnimationTimes,
                                                     fireSpreaderAnimations))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                     ship,
                                                     systems,
                                                     crew,
                                                     weapons,
                                                     drones,
                                                     powered,
                                                     doors,
                                                     fakeDoors,
                                                     doorFrames,
                                                     doorTimers,
                                                     roomOxygen,
                                                     lockdownRooms,
                                                     hull,
                                                     missiles,
                                                     droneParts,
                                                     shields,
                                                     superShields,
                                                     shieldCharge,
                                                     shieldExcessChargeHack,
                                                     present,
                                                     powerFixtureOverride,
                                                     fireWhileCloaked,
                                                     pinnedRewardFixture,
                                                     rewardVariant))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                     rewardTier,
                                                     rewardType,
                                                     rewardUnsupportedItem,
                                                     rewardUnsupportedCrew,
                                                     rewardItem,
                                                     rewardCommitted,
                                                     rewardCrewSpecies,
                                                     rewardCrewName,
                                                     rewardCrewFullName,
                                                     rewardCrewMale,
                                                     rewardCrewAppearance,
                                                     rewardCrewPending,
                                                     rewardCrewOverflow,
                                                     hasSurrenderThreshold,
                                                     surrenderThresholdMin,
                                                     surrenderThresholdMax,
                                                     surrenderChanceNone,
                                                     surrenderThresholdSelected,
                                                     surrenderWorldLevelOffset,
                                                     surrenderThreshold,
                                                     cloneQueue,
                                                     cloneTimer,
                                                     cloneDeathTimer,
                                                     cloneDuration))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                     mind,
                                                     mindTarget,
                                                     mindTargetOwner,
                                                     reactorPowerCap,
                                                     reactorPowerLoss,
                                                     reactorPowerDivide,
                                                     hacking,
                                                     boardingAi,
                                                     teleportStartingCrew,
                                                     teleportWaves,
                                                     teleportCharge,
                                                     teleportRequest,
                                                     teleportParty,
                                                     hackingDrone,
                                                     hackingTarget,
                                                     hackingDroneSpace,
                                                     hackingDroneId,
                                                     cloak,
                                                     battery,
                                                     explosionState))
            NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                     escapeThreshold,
                                                     escapeTimer,
                                                     escaping,
                                                     escapeJumpElapsed,
                                                     stalemateTrigger,
                                                     stalemateTimer,
                                                     lastEscapeHealth,
                                                     doorBaseHealth,
                                                     doorDamage,
                                                     doorForcedTicks,
                                                     doorLevel,
                                                     doorLastBaseHealth,
                                                     doorLockdownBroken))
        }
    } enemy_;

    void configureCombatAiThresholds(Enemy& next, pugi::xml_node combatDefinition);
    void beginEnemyJump();

    struct FireSmoke {
        int tile = 0;
        float elapsed = 0;
        FTL_NATIVE_JSON(FireSmoke, tile, elapsed)
    };

    struct RoomHazard {
        int fires = 0;
        int breaches = 0;
        std::vector<int> breachTiles;
        std::vector<float> breachHealth;
        float fireRepair = 0;
        float fireSpreadSeconds = 0;
        float breachRepair = 0;
        std::vector<float> fireDeathTimers{};
        std::vector<int> fireTiles{};
        std::vector<float> fireHealth{};
        std::map<int, float> fireStartTimers{};
        std::vector<FireSmoke> fireSmoke{};

        void syncFireTiles() {
            fireTiles.resize(std::min(fireTiles.size(), static_cast<std::size_t>(fires)));
            while (fireTiles.size() < static_cast<std::size_t>(fires)) {
                int tile = 0;
                while (std::find(fireTiles.begin(), fireTiles.end(), tile) != fireTiles.end()) {
                    ++tile;
                }
                fireTiles.push_back(tile);
            }
            fireHealth.resize(static_cast<std::size_t>(fires), 100.f);
        }

        void extinguishFire(std::size_t index) {
            syncFireTiles();
            const int tile = fireTiles.at(index);
            std::erase_if(fireSmoke, [tile](const FireSmoke& smoke) {
                return smoke.tile == tile;
            });
            fireSmoke.push_back({tile, 0});
            fireTiles.erase(fireTiles.begin() + static_cast<std::ptrdiff_t>(index));
            fireHealth.erase(fireHealth.begin() + static_cast<std::ptrdiff_t>(index));
            --fires;
        }

        friend void to_json(nlohmann::json& data, const RoomHazard& hazard) {
            data = {{"fires", hazard.fires},
                    {"breaches", hazard.breaches},
                    {"breachTiles", hazard.breachTiles},
                    {"breachHealth", hazard.breachHealth},
                    {"fireRepair", hazard.fireRepair},
                    {"fireSpreadSeconds", hazard.fireSpreadSeconds},
                    {"breachRepair", hazard.breachRepair},
                    {"fireDeathTimers", hazard.fireDeathTimers},
                    {"fireTiles", hazard.fireTiles},
                    {"fireHealth", hazard.fireHealth},
                    {"fireStartTimers", hazard.fireStartTimers},
                    {"fireSmoke", hazard.fireSmoke}};
        }

        friend void from_json(const nlohmann::json& data, RoomHazard& hazard) {
            nativeRead(data.at("fires"), hazard.fires);
            nativeRead(data.at("breaches"), hazard.breaches);
            nativeRead(data.at("breachTiles"), hazard.breachTiles);
            nativeRead(data.at("fireRepair"), hazard.fireRepair);
            nativeRead(data.at("fireSpreadSeconds"), hazard.fireSpreadSeconds);
            nativeRead(data.at("breachRepair"), hazard.breachRepair);
            if (data.contains("breachHealth")) {
                nativeRead(data.at("breachHealth"), hazard.breachHealth);
            } else {
                hazard.breachHealth.assign(hazard.breachTiles.size(), 100.f);
                if (!hazard.breachHealth.empty()) {
                    hazard.breachHealth.back() = std::max(0.f, 100.f - hazard.breachRepair);
                }
            }
            if (hazard.breachHealth.size() != hazard.breachTiles.size()) {
                throw std::out_of_range("invalid_breach_health_count");
            }
            for (const auto health : hazard.breachHealth) {
                if (!std::isfinite(health) || health < 0 || health > 100) {
                    throw std::out_of_range("invalid_breach_health");
                }
            }
            // Older version-one saves did not retain individual suffocation timers.
            if (data.contains("fireDeathTimers")) {
                nativeRead(data.at("fireDeathTimers"), hazard.fireDeathTimers);
            } else {
                hazard.fireDeathTimers.clear();
            }
            hazard.fireTiles.clear();
            hazard.fireSmoke.clear();
            if (data.contains("fireTiles")) {
                nativeRead(data.at("fireTiles"), hazard.fireTiles);
            }
            if (data.contains("fireSmoke")) {
                nativeRead(data.at("fireSmoke"), hazard.fireSmoke);
            }
            if (data.contains("fireHealth")) {
                nativeRead(data.at("fireHealth"), hazard.fireHealth);
            } else {
                hazard.fireHealth.assign(static_cast<std::size_t>(std::max(0, hazard.fires)),
                                         100.f);
                if (!hazard.fireHealth.empty()) {
                    hazard.fireHealth.back() = std::max(0.f, 100.f - hazard.fireRepair);
                }
            }
            hazard.fireStartTimers.clear();
            if (data.contains("fireStartTimers")) {
                nativeRead(data.at("fireStartTimers"), hazard.fireStartTimers);
            }
        }
    };

    struct ShieldHit {
        int ship = 0;
        SDL_FPoint point{};
        std::uint64_t tick = 0;
        std::uint64_t projectileId = 0;
        float currentSize = 0;
        float currentThickness = 0;
        int cachedDamage = 0;
        FTL_NATIVE_JSON_DEFAULT(
            ShieldHit, ship, point, tick, projectileId, currentSize, currentThickness, cachedDamage)
    };

    struct ResistMessage {
        int ship = 0;
        SDL_FPoint point{};
        std::uint64_t tick = 0;
        bool ion = true;
        float elapsed = 0;
        bool shieldOwned = false;
        bool superShield = false;
        std::uint64_t messageOrder = 0;
        // ShipManager::OnRender 0x49abc0 colours a telepathic crew's RESIST (1, 0, 1, 1).
        bool mind = false;

        friend void to_json(nlohmann::json& data, const ResistMessage& message) {
            data = {{"ship", message.ship},
                    {"point", message.point},
                    {"tick", message.tick},
                    {"ion", message.ion},
                    {"elapsed", message.elapsed},
                    {"shieldOwned", message.shieldOwned},
                    {"superShield", message.superShield},
                    {"messageOrder", message.messageOrder},
                    {"mind", message.mind}};
        }

        friend void from_json(const nlohmann::json& data, ResistMessage& message) {
            nativeRead(data.at("ship"), message.ship);
            nativeRead(data.at("point"), message.point);
            nativeRead(data.at("tick"), message.tick);
            message.ion = data.contains("ion") ? nativeValue<bool>(data.at("ion")) : true;
            message.elapsed =
                data.contains("elapsed") ? nativeValue<float>(data.at("elapsed")) : 0.f;
            message.shieldOwned =
                data.contains("shieldOwned") && nativeValue<bool>(data.at("shieldOwned"));
            message.superShield =
                data.contains("superShield") && nativeValue<bool>(data.at("superShield"));
            message.messageOrder = data.contains("messageOrder")
                                       ? nativeValue<std::uint64_t>(data.at("messageOrder"))
                                       : 0;
            message.mind = data.contains("mind") && nativeValue<bool>(data.at("mind"));
        }
    };

    // DamageMessage::DamageMessage(float, int, Pointf, bool) 0x59d080 builds a white
    // (`color = 1,1,1,1`) floating number from `numbers/Text_*.png`.
    // `SpaceDrone::CollisionMoving 0x41e400` raises one with `damage = 0` when a
    // powered drone dodges a shot, and `Shields::CollisionReal 0x60d980` raises one
    // with the absorbed damage and `color = (0.157, 0.941, 0.157)` when a super
    // shield layer takes the hit. Positive room hull damage also raises a white numeric message.
    struct DamageMessage {
        int ship = 0;
        int amount = 0;
        SDL_FPoint point{};
        std::uint64_t tick = 0;
        bool superShield = false;
        bool ion = false;
        bool miss = false;
        float elapsed = 0;
        bool shieldOwned = false;
        std::uint64_t messageOrder = 0;
        // DamageSystem 0x4991d0 colours a system-damage number (1, .39, .39).
        bool systemDamage = false;
        FTL_NATIVE_JSON_DEFAULT(DamageMessage,
                                ship,
                                amount,
                                point,
                                tick,
                                superShield,
                                ion,
                                miss,
                                elapsed,
                                shieldOwned,
                                messageOrder,
                                systemDamage)
    };

    void combatSound(const std::vector<std::string>& aliases,
                     std::uint64_t key,
                     bool weaponEffect = false);
    int ionAfterResistance(int targetShip, int amount, SDL_FPoint point, bool shieldDomain = false);
    bool touchShieldHit(int ship, SDL_FPoint point, std::uint64_t id, int damage);
    void pushCombatMessage(DamageMessage message);
    void pushCombatMessage(ResistMessage message);
    void ionizeShieldCollision(int targetShip, int amount, int stun, int stunChance);
    bool resistAugmentation(int targetShip, std::string_view augmentation);
    void crewDeath(RunCrew& crew);
    void beginCombat(std::string_view enemyShipId = "REBEL_SKINNY",
                     std::string_view enemyDroneChoice = {},
                     std::string_view shipEventId = {},
                     std::uint32_t shipSeed = 0);
    void advanceCombat();
    void advanceCombatSystem(int owner, std::size_t systemIndex);
    void advanceCombatSystemState(int owner, std::size_t systemIndex, bool jumping = false);
    void advanceCombatAi(int owner);
    void advanceCombatOutcomes();
    void advanceCombatShipState(int owner);
    void advanceCombatShipLocks(int owner);
    void advanceCrewSystem(int owner, std::size_t systemIndex);
    void advanceAdvancedSystem(int owner, std::size_t systemIndex);
    void advanceAdvancedOwner(int owner, int systemId, bool ai);
    void advanceAdvancedAi(int owner);
    void advanceDroneSystem(int owner, std::size_t systemIndex);
    void advanceDroneAi(int owner);
    void advanceOwnerEnvironment(int owner);
    void advancePhysicalCrew(int physicalShip);
    void advanceCloneReady(int owner);
    void advanceSpreadDamage(int owner);
    void advanceCrewRepair(RunCrew& member, int owner, int physicalShip = -1);
    // CrewAnimation status 4 and its smoke emitter (see CrewSmoke).
    int crewFireDirection(const RunCrew& member, int physicalShip) const;
    static void updateCrewFiring(CrewSmoke& smoke, int direction);
    void birthCrewSmoke(CrewSmoke& smoke, std::string_view species, float startingLife);
    void seedCrewSmoke(CrewSmoke& smoke, std::string_view species, int amount);
    void advanceCrewSmoke(CrewSmoke& smoke, std::string_view species);
    static std::string crewFireAnimation(const RunCrew& member, const CrewSmoke& smoke);
    void renderCrewSmoke(const CrewSmoke& smoke, float x, float y);
    void combatExperience(RunCrew& crew);
    void advanceCrewAi(int physicalShip);
    bool damageSystemOverTime(RunSystem& system, float amount, bool enemy = false);
    bool
    partialRepairSystem(RunSystem& system, float speed, bool automated = false, bool enemy = false);
    void advanceCompleteShip(int owner, bool jumping);
    void advanceShipManager(int owner, bool jumping);
    void advanceStatusEffectShips();
    void advanceShipState(int owner, bool jumping);
    void advanceEnemyConstructorClocks(Enemy& enemy);
    void advanceArtillery();
    void advanceDrones();
    void advanceDroneSystems();
    void waitSystems();
    void jumpWeaponFactories(bool enemy = false);
    void advanceSpace();
    void advanceDroneBase(CombatDrone& drone, int owner, int controllerId = 4);
    // Authored drone dodge, or the fixture override when one is set.
    int effectiveDroneDodge(const CombatDrone& drone) const;
    void advanceCombatDrone(CombatDrone& drone, int owner);
    void advanceOrbitFlight(CombatDrone& drone, int owner);
    void offerDefenseProjectile(CombatDrone& drone, int owner, const CombatProjectile& projectile);
    void
    offerDefenseDrone(CombatDrone& drone, int owner, const CombatDrone& target, int targetOwner);
    void advanceDefenseDroneAim(CombatDrone& drone);
    void advanceSpaceProjectiles(std::uint64_t firstNewProjectile);
    bool projectileDead(CombatProjectile& projectile);
    void advanceCrewDrone(CombatDrone& drone, int owner, bool carrierOnly = false);
    void advanceHackingDroneSpace(CombatDrone& drone, int owner);
    std::vector<CombatDrone*> physicalCrewDrones(int owner);
    std::vector<const CombatDrone*> physicalCrewDrones(int owner) const;
    void assignSpaceDroneId(CombatDrone& drone);
    void registerSpaceDrones(int owner, bool initial = false);
    void clearSpaceDroneRegistry();
    std::vector<CombatDrone*> spaceDronesForOwner(int owner);
    std::vector<const CombatDrone*> spaceDronesForOwner(int owner) const;
    std::vector<std::pair<int, CombatDrone*>> registeredSpaceDrones();
    std::vector<std::pair<int, const CombatDrone*>> registeredSpaceDrones() const;
    void advanceHazards(int owner = -1);
    void combatRender();
    bool enemyContainsPlayerCrew() const;
    bool enemyRoomVisible(int room) const;
    bool playerLifeSight() const;
    bool playerRoomVisible(int room) const;
    std::vector<bool> doorsHit(bool enemy, std::size_t count) const;
    void doorRenderLevels(bool enemy, ShipPaintState& view) const;
    bool weaponCursorValid() const;
    void renderEnemySystemHud();
    void startArmamentWarning(int index, const std::string& key, bool broken = false);
    void stopArmamentWarning(int index);
    void advanceArmamentWarnings(float seconds);
    void renderArmamentWarnings();
    void renderSpaceProjectiles(int ship, int layer);
    void renderSpaceDrones(int ship, int layer);
    void renderShipMessages(int ship);
    void renderShieldHits(int ship);
    void renderWeaponAiming(int targetShip);
    void renderHazards(const ShipDefinition& ship,
                       const std::map<int, RoomHazard>& hazards,
                       float x,
                       float y);
    void
    renderLockdown(const ShipDefinition& ship, const std::map<int, float>& rooms, float x, float y);
    void gameOverRender();
    bool checkDefeat();
    static Rect gameOverButton(int index);
    std::vector<int> combatWeaponFrames(const ShipDefinition& ship,
                                        const std::vector<CombatWeapon>& weapons) const;
    std::vector<int> combatArtilleryFrames(const std::vector<RunSystem>& systems) const;
    void selectWeapon(int index, bool remove = false);
    bool combatClick(float x, float y, bool right);
    void targetWeapon(int id, bool self = false, const SDL_FPoint* aim = nullptr);
    void setAutofire(bool enabled);
    std::string applyCombatRewardPickups();
    std::string commitCombatEquipmentReward();
    EventResources surrenderReward();
    EventResources generateCombatReward(std::string_view type, int tier);
    void stageCombatCrewReward(const std::string& species = {},
                               bool eventTemplate = false,
                               const std::string& explicitName = {});
    void commitCombatCrewReward();
    void stopCombatHostilities();
    void initializeExplosion(CombatExplosion& state, const ShipDefinition& ship);
    void
    advanceExplosion(CombatExplosion& state, const ShipDefinition& ship, float age, float step);
    void
    renderExplosionEffects(const std::vector<CombatExplosionEffect>& effects, float x, float y);
    void startRoomFire(int targetShip, int roomId, int count = 1);
    // Ship::BreachRandomHull 0x483390 / BreachSpecificHull 0x483120: open a
    // breach on a room hull tile (native row-major tile index) with its sounds.
    bool breachRandomHull(int targetShip, int roomId);
    bool breachHullTile(int targetShip, int roomId, int tile, bool specific);
    void sweepProjectileSpaceContacts(CombatProjectile& projectile);
    void combatResult(bool surrender);
    void fireProjectile(int owner, int index, bool artillery = false);
    void advanceWeaponAnimation(CombatWeapon& weapon,
                                const EquipmentDefinition& blueprint,
                                bool powered,
                                int hacking);
    void advanceWeaponSlides(float seconds);
    void extractWeaponProjectiles();
    void extractDroneProjectiles();
    void initializeWeaponFactory(CombatWeapon& weapon,
                                 const EquipmentDefinition& blueprint,
                                 RandomStream& random);
    void initializeProjectileExplosion(CombatProjectile& projectile,
                                       const EquipmentDefinition& blueprint);
    void initializeBeamProjectile(CombatProjectile& projectile,
                                  SDL_FPoint emission,
                                  float heading,
                                  bool oneSpace,
                                  float authoredSpeed);
    void crystalRetaliation(int owner, SDL_FPoint location);
    bool projectileDodged(CombatProjectile& projectile);
    int damageCrewDrones(int ship,
                         int room,
                         float damage,
                         const SDL_FPoint* tile = nullptr,
                         float stunSeconds = 0,
                         int stunChance = 0,
                         int excludedOwner = -1,
                         const CombatDrone* onlyDrone = nullptr);
    bool toggleDrone(int index);
    bool powerDrone(CombatDrone& active, int owner = 0);
    void impact(CombatProjectile& projectile,
                bool shieldOnly = false,
                const bool* beamResistance = nullptr,
                bool directRoom = false);
    // ShipManager::DamageBeam 0x49e4d0's tail: the fire and breach rolls and the
    // crew strike run only on steps where ShipGraph::TranslateToGrid(current)
    // differs from ShipGraph::TranslateToGrid(last), i.e. once per 35-pixel tile
    // the beam head enters.
    void beamTileImpact(CombatProjectile& projectile, bool rockArmor);
    void repairSystems(std::vector<RunSystem>& systems, std::vector<RunCrew>& crew, bool enemy);
    nlohmann::json combatState() const;
    void loadCombatSnapshot(const nlohmann::json& values);

    float playerX() const {
        return enemy_.present ? 200.f : 350.f;
    }

    float enemyOriginX() const;
    float enemyOriginY() const;

    void text(const std::string& s,
              float x,
              float y,
              int size = 12,
              bool center = false,
              SDL_Color color = {235, 245, 229, 255});
    void hud(bool front);
    void shipStatusHud();
    void resourceHud();
    void crewHud();
    int systemHud(bool front = false);
    int subsystemHudSpacing() const;
    SDL_FPoint systemHudPosition(int id) const;
    Rect abilityButtonRect(int id) const;
    void renderSystemHudBox(const RunSystem& source,
                            SDL_FPoint position,
                            bool enemy = false,
                            bool showPower = true);
    Rect autofireRect() const;
    enum class ArmamentPass : std::uint8_t { Body, Labels, Dragging };
    void weaponHud(int x, ArmamentPass pass = ArmamentPass::Body);
    void droneHud(ArmamentPass pass = ArmamentPass::Body);
    void armamentLabels(const EquipmentDefinition& blueprint,
                        int key,
                        SDL_FPoint position,
                        bool drone,
                        SDL_Color color);
    bool armamentDragging(bool drone, int dense) const;
    std::array<int, 8> weaponKeyLabels_{1, 2, 3, 4, -1, -1, -1, -1};
    std::array<int, 8> droneKeyLabels_{5, 6, 7, 8, -1, -1, -1, -1};
    void renderCrewDrones(int ship,
                          float x,
                          float y,
                          bool healthOnly = false,
                          const CombatDrone* onlyDrone = nullptr);
    void subsystemHud(bool front = false);
    void eventDialog();
    // CommandGui's extra ChoiceBox is application UI, separate from events
    // and checkpoints. A successful save clears only its repeat guard.
    bool writeErrorLatched_ = false;
    bool writeErrorOpen_ = false;
    bool writeErrorHovered_ = false;
    std::uint64_t writeErrorOpenedUiTick_ = 0;
    std::string writeErrorText_;
    std::string writeErrorChoiceText_;
    RunSystem* system(int id);
    const RoomDefinition& room(int id) const;
    const RunSystem* system(int id) const;
    int availablePower() const;
    std::pair<int, int> crewPowerForSystem(const RunSystem&, bool enemy) const;
    void updateCrewPower(int owner = -1);
    void updateManning(int owner = -1);
    void advanceStationHandoff(RunCrew& member, int physicalShip, int creationOwner);
    const RunCrew* crewOperator(int id, bool enemy = false) const;
    bool hasCrewManning(int id, bool enemy = false) const;
    bool enemyAutomated() const;
    void advanceStationHandoffs(bool enemy = false);
    bool manned(int id) const;
    int subsystemLevel(int id) const;
    int evasion(int owner = 0) const;
    std::vector<std::size_t> livingCrew() const;
    std::vector<std::size_t> crewRoster() const;
    int roomAt(float x, float y) const;
    int primarySlot(int id) const;
    SDL_FPoint slotPoint(int id, int slot) const;
    // The target slot is reserved immediately; physical movement waits for simulation ticks.
    bool order(RunCrew& c,
               int destination,
               int requested = -1,
               bool force = false,
               bool enemy = false,
               const RunCrew* boardingSource = nullptr,
               int occupancySide = -1,
               const CombatDrone* movingDrone = nullptr);
    void advanceCrew(RunCrew& c, bool enemy = false, int owner = -1);
    void setCrewControlled(RunCrew& member, bool controlled);
    void environment();
    std::map<int, int> connectedRoomDepths(int start) const;
    bool doorLocked(int id, bool enemy = false) const;
    // Door::iHacked from Ship::SetHackLevel 0x4813a0 (ShipManager::OnLoop).
    int doorHackLevel(const DoorDefinition& door, bool enemy) const;
    int doorAt(float x, float y) const;
    int selectableDoorAt(float x, float y) const;
    int doorCursorMode() const;
    bool power(int id, bool add);
    bool weaponPower(int index, bool add);
    void stations(bool save);
    void tooltip();
    void renderCrewRosterSkills();
    void renderDroneIcon(const EquipmentDefinition& bp, float centerX, float centerY);
    // CrewBox::GetSelected 0x501bc0 for the roster; returns the pointed box
    // (selectable or not) and keeps crewBoxHover_ for the skill panel.
    int updateCrewBoxHover();
    bool crewPointerHit(const RunCrew& member) const;
    static std::pair<int, int> crewSkillProgress(const RunCrew& member, int skill);
    std::string crewSkillTooltip(const RunCrew& member, int skill, bool infoScreen) const;
    void renderCrewSkill(
        const RunCrew& member, int skill, float x, float y, float length = 40, float height = 8);
    void mapTooltip();
    void doorControl(bool open);
    void updateDoors(bool enemy = false);
    bool crewDoorSealed(int id, int owner, bool controlled, bool enemy = false) const;

    struct DoorPath {
        int startRoom = -1;
        std::vector<int> doors;
    };

    int crewFreeSlot(const RunCrew* self,
                     int destination,
                     int requested,
                     bool enemy,
                     const RunCrew* boardingSource,
                     int side,
                     const CombatDrone* movingDrone) const;
    int shipIntruderCount(int ship) const;
    void updateEnemyTeleportRequest();
    std::optional<DoorPath> findDoorPath(
        const ShipDefinition& ship, SDL_Point start, SDL_Point goal, int shipId, bool enemy) const;
    void damageDoor(int id, bool enemy = false);
    void lockdownDoors(int roomId, bool enemy = false);
    bool forceDecreaseSystemPower(RunSystem& system, int amount, bool enemy = false);
    void updateReactor(bool enemy);

    int systemMaximum(const RunSystem& station) const {
        return station.definition.maxPower >= 0 ? station.definition.maxPower
                                                : catalog_.system(station.definition.name).maxPower;
    }

    // Persistent state is grouped by alignment to avoid padding in this large run object.
    BlueprintCatalog catalog_;
    TextLibrary texts_;
    ShipPainter painter_;
    EventCatalog events_;
    RandomStream sessionRandom_{0};
    RandomStream eventRandom_{0};
    std::uint64_t tick_ = 0;
    std::uint64_t animation_ = 0;
    std::uint64_t eventChoiceOpenedUiTick_ = 0;
    std::uint64_t hoverTick_ = 0;
    // Store boxes keep the hover state of the last Store::MouseMove 0x4b28c0;
    // CommandGui stops forwarding mouse moves while the purchase confirm is open.
    float storeMouseX_ = 0;
    float storeMouseY_ = 0;
    // Door::ApplyDamage 0x463f30 starts the 0.5 s gotHit tracker (UI ticks;
    // Door::PauseLoop keeps advancing it while paused). [0] player, [1] enemy.
    std::array<std::map<int, std::uint64_t>, 2> doorHitTick_;
    // StarMap::targetBoxTimer (ctor 0x5cc737: 1 s, looping), shared by the
    // beacon and sector maps and restarted when the potential choice changes.
    std::uint64_t targetBoxTick_ = 0;
    bool targetBoxRunning_ = false;
    // StarMap::potentialSectorChoice (sector index, -1 none).
    int sectorPotential_ = -1;
    std::uint64_t scrapFailureAnimation_ = 0;
    std::uint64_t messageTick_ = 0;
    RandomStream combatRandom_{0};
    RandomStream resourceFeedbackRandom_{0};
    std::uint64_t nextProjectile_ = 1;
    std::uint64_t nextDamageMessageOrder_ = 1;
    // Process state, like the original's static: not a checkpoint field.
    int beamSide_ = 0;
    std::uint64_t hackingDroneId_ = 0;
    std::vector<CombatProjectile> pendingRetaliation_;
    nlohmann::json combatSounds_ = nlohmann::json::array();
    std::map<int, float> roomOxygen_;
    // Room::lastO2, copied by Ship::OnLoop 0x48a4b0 from the oxygen levels each
    // frame (zero without an oxygen system). Transient like the original.
    std::array<std::map<int, float>, 2> roomLastOxygen_;
    // Fire::fOxygen per room (UpdateEnvironment stores the truncated level).
    std::array<std::map<int, int>, 2> fireOxygen_;
    // OuterHull::heal phase (people/healing_strip11, 11 frames, 1s loop).
    std::array<float, 2> slugGelHealElapsed_{};
    // PowerManager::batteryPower.second as last set by BatterySystem::OnLoop
    // 0x608ed0 or SetTurnedOn 0x608b80; -1 = not yet set since load.
    std::array<int, 2> batteryCapacity_{-1, -1};
    // ShipManager::iLastDamage per ship (constructor 0; never saved).
    std::array<int, 2> lastHullDamage_{};
    friend class CrewAiPass;

    // FIRE_EXTINGUISHERS ParticleEmitter per room (ShipManager::OnInit 0x4a6e00:
    // people/firefight_cloud.png 24x24, birthRate 3, lifespan 0.8, speed 1.3,
    // random angle, room centre). Cosmetic; never saved.
    struct ExtinguisherParticle {
        float x = 0;
        float y = 0;
        float dx = 0;
        float dy = 0;
        float life = -1;
        bool alive = false;
        FTL_NATIVE_JSON(ExtinguisherParticle, x, y, dx, dy, life, alive)
    };

    struct ExtinguisherEmitter {
        std::array<ExtinguisherParticle, 64> particles{};
        float birthCounter = .01f;
        FTL_NATIVE_JSON(ExtinguisherEmitter, particles, birthCounter)
    };

    std::array<std::map<int, ExtinguisherEmitter>, 2> fireExtinguishers_;
    void advanceFireExtinguishers(int owner);
    std::map<int, float> lockdownRooms_;
    std::map<int, std::vector<StoreOffer>> stores_;
    std::map<int, RoomHazard> playerHazards_;
    std::map<int, RoomHazard> enemyHazards_;
    std::vector<MapNebula> mapNebulas_;
    // Automation-only record of the latest sector generation (RNG states
    // before its exit, boss, nebula and event-placement draws); never saved.
    nlohmann::json generationTrace_ = nlohmann::json::object();
    std::vector<RunChoice> choices_;
    std::vector<SDL_FPoint> previousCrew_;
    std::vector<float> previousDoors_;
    std::vector<RunSystem> systems_;
    std::vector<RunCrew> crew_;
    CrewAiState playerCrewAi_;
    std::uint64_t crewDeathCount_ = 0;
    std::uint64_t nextCrewDroneId_ = 1;
    std::uint64_t nextPhysicalCrewOrder_ = 1;
    std::uint64_t nextSpaceDroneId_ = 1;
    std::uint64_t nextSpaceDroneOrder_ = 1;
    std::vector<int> spaceShipOwners_;
    std::vector<RunCrew> voyageBoarders_;
    nlohmann::json pendingVoyageEffects_ = nlohmann::json::array();
    std::array<std::uint64_t, 4> resourceDeltaTicks_{};

    struct ResourceFeedback {
        std::uint64_t start = 0;
        std::size_t resource = 0;
        int delta = 0;
        int jitter = 0;
        FTL_NATIVE_JSON(ResourceFeedback, start, resource, delta, jitter)
    };

    std::vector<ResourceFeedback> resourceFeedback_;
    std::array<int, 4> observedResources_{};
    std::vector<float> doorFrames_;
    std::vector<float> doorTimers_;
    std::vector<std::string> cargo_;
    std::vector<InventoryFactory> cargoFactories_;
    std::vector<std::pair<int, int>> upgradePurchases_;
    std::vector<int> cloneQueue_;
    std::vector<CombatWeapon> combatWeapons_;
    std::vector<CombatDrone> combatDrones_;
    std::vector<CombatDrone> retiredCrewDrones_;
    std::vector<CombatDrone> retiredSpaceDrones_;
    std::vector<CombatProjectile> projectiles_;
    // ShipManager::hitByBeam (written by ShipManager::DamageBeam 0x49e4d0 from
    // CFPS::GetUnpausedTime 0x59a690) is a per-room timestamp of the last beam
    // hit; ShipManager::killedByBeam is a per-beam counter keyed by the beam's
    // selfId. Both are indexed by target ship and are only read by the
    // achievement check that shares this ship state.
    std::array<std::map<int, float>, 2> beamHitAt_;
    std::array<std::map<int, int>, 2> beamKilledCrew_;
    std::vector<ShieldHit> shieldHits_;
    std::vector<ResistMessage> resistMessages_;
    std::vector<DamageMessage> damageMessages_;
    // ShipManager::explosions: transient explosion1 effects of destroyed systems
    // (OnLoop 0x4a05b0 GetExploded). The original never saves them.
    std::array<std::vector<CombatExplosionEffect>, 2> systemExplosions_;
    std::vector<pugi::xml_document> restoredEvents_;
    std::string name_;
    std::string locale_;
    std::string eventText_;
    std::string eventId_;
    std::vector<std::string> crewNameHistory_;
    std::string encounter_;
    // WorldManager::currentShipEvent.shipSeed: CreateEvent 0x5b8940 draws it
    // for an event ship, CreateShip 0x5eee10 copies it and ShipEvent::Save
    // 0x5ebb60 keeps it. The ShipEvent default is 0; -1 (GetBaseEvent's
    // no-reseed value) marks a native save that predates it.
    int encounterShipSeed_ = 0;
    NebulaScene nebulaScene_;
    Environment environment_;
    Campaign campaign_;
    CombatDrone hackingDrone_{.blueprint = "DRONE_HACKING"};
    std::string message_;
    std::string panel_;
    std::string voyageMessage_;
    std::string crewRenameText_;
    std::size_t crewRenameCaret_ = 0;
    float crewRenameBlink_ = 0;
    std::string sectorName_;
    std::string overflowEquipment_;
    InventoryFactory overflowFactory_;
    std::string overflowAugment_;
    std::string inventoryDragKind_;
    std::vector<std::string> sectorChoices_;
    std::vector<std::string> pendingQuests_;
    std::vector<std::string> usedVoyageEvents_;
    std::int64_t selectedVoyageCrewSeed_ = -1;
    void applyCloneSkillLoss(RunCrew& crew);
    void failScrapPurchase();
    bool placeVoyageCrew(RunCrew& member,
                         int preferredRoom,
                         bool intruder = false,
                         bool enemy = false,
                         Enemy* stagedEnemy = nullptr);
    std::vector<bool> doors_;
    std::vector<bool> weaponPower_;
    std::vector<bool> weaponDesired_;
    std::vector<bool> fakeDoors_;
    TravelGraph graph_;
    std::vector<int> weaponSlots_, droneSlots_;
    std::map<int, std::array<float, 3>> voyageMelee_;
    std::map<int, int> voyageDoorBaseHealth_;
    std::map<int, int> voyageDoorLevel_;
    std::map<int, int> voyageDoorLastBaseHealth_;
    std::set<int> voyageDoorLockdownBroken_;
    std::map<int, float> voyageDoorDamage_;
    std::map<int, int> voyageDoorForcedTicks_;
    std::string eventBackground_, eventPlanet_;
    int eventPlanetX_ = 0, eventPlanetY_ = 0, eventPlanetRotation_ = 0;
    SectorGraph sectorGraph_;
    ShipDefinition ship_;
    float previousJump_ = 0;
    float presentationAlpha_ = 1;
    float uiFraction_ = 0;
    float uiAlpha_ = 1;
    float ftlCharge_ = 0;
    // ShipManager::lastJumpReady / lastEngineStatus: the previous OnLoop's FTL
    // readiness and GetDodgeFactor > 0. Both start false (ShipManager ctor
    // 0x495af0) and are never saved, so the cues may replay after a load.
    bool lastJumpReady_ = false;
    bool lastEngineStatus_ = false;
    int jumpTicks_ = 0;
    int arrivalTicks_ = 0;
    int jumpDestination_ = -1;
    int locationBeacon_ = -1;
    RunOptions options_;
    int difficulty_ = 0;
    int hull_ = 0;
    int scrap_ = 0;
    int fuel_ = 16;
    int missiles_ = 0;
    int parts_ = 0;
    int devCrewLimit_ = 8;
    int devReactorLimit_ = 25;
    float mouseX_ = -1;
    float mouseY_ = -1;
    bool eventChoiceHoverValid_ = false;
    // MouseControl 0x8c76c0: valid (+0x1d) and newHover (+0x1e) are cleared
    // by Reset 0x4d7cf0 each CApp::OnLoop; lastValid (+0x1c) persists so
    // OnRender 0x4d6e00 can beep on the invalid-to-valid edge.
    bool pointerValid_ = false;
    bool pointerLastValid_ = false;
    bool pointerNewHover_ = false;
    // CrewBox::mouseHover (roster index) and the box CrewControl last pointed at.
    int crewBoxHover_ = -1;
    // WorldManager::CreateStore 0x5edda0 / CommandGui::CreateNewStore 0x51fe60
    // open the store TabbedWindow on arrival; RenderStatic 0x520a70 draws only
    // the first open focus window, so it appears once the arrival event closes.
    bool storeOpenPending_ = false;
    int crewBoxPointed_ = -1;
    std::string hudHoverSoundTarget_;
    std::vector<std::string> audioWeaponIds_;
    std::vector<bool> audioWeaponReady_;
    bool audioIncomingFire_ = false;
    int mapHoverSoundBeacon_ = -1;
    float shieldWarningTime_ = 0;
    std::array<HudWarning, 4> hudWarnings_{};
    int hudLastHull_ = 0;
    int hullWarningLevel_ = 75;
    float ftlBlink_ = 0;
    float previousFtlBlink_ = 0;
    float ftlBlinkDirection_ = 1;
    float pauseBlink_ = 0;
    float previousPauseBlink_ = 0;
    float pauseBlinkDirection_ = 1;
    std::string pauseKeyName_ = "Space";
    std::array<std::string, 8> crewKeyNames_{"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8"};
    std::string resourceFollowupText_;
    bool externalModal_ = false;
    bool fullFocusLatch_ = false;
    bool fullFocusForFrame_ = false;
    float ftlPullout_ = 0;
    float previousFtlPullout_ = 0;
    void advanceHudUi(std::uint32_t ticks);
    float oxygen_ = 100;
    bool oxygenLeakSound_ = false;
    int shieldBubbles_ = 0;
    int superShields_ = 0;
    float shieldCharge_ = 0;
    bool shieldExcessChargeHack_ = false;
    int sector_ = 1;
    int sectorType_ = 0;
    int panelPage_ = 0;
    int dismissCrew_ = -1;
    int renameCrew_ = -1;
    // Tutorial MenuScreen opened over a still-open ship screen (CommandGui::
    // KeyDown 0x5201f0); restored when the menu closes.
    std::string pausedShipPanel_;
    int pausedDismissCrew_ = -1;
    int inventorySelection_ = -1;
    int inventoryDragIndex_ = -1;
    int pendingStoreOffer_ = -1;
    bool storeSelling_ = false;
    int distressWaits_ = 0;
    std::uint32_t fuelEventSeed_ = 0;
    // StarMap::sectorMapSeed: the value StarMap::NewGame 0x5d5a20 drew to seed
    // GenerateSectorMap. -1 marks a checkpoint from before it was recorded.
    std::int64_t sectorMapSeed_ = -1;
    // ScoreKeeper current "crew hired" (+128), counted by AddCrew 0x4417b0 for
    // each starting crew member and every later hire.
    int scoreCrew_ = 0;
    // StarMap::waitedLast / lastEscapeEvent (SetupNoFuel 0x5d0250; a jump clears
    // waitedLast), bMapRevealed (reveal_map) and addedQuests (AddQuest 0x5d4100).
    bool waitedLast_ = false;
    std::string lastEscapeEvent_;
    bool mapRevealed_ = false;
    std::vector<std::pair<std::string, int>> addedQuests_;
    // WorldManager::generatedEvent, choiceHistory and lastMainText: the dialog
    // state LoadGame replays (voyage.md section 7.2). generatedEvent holds the
    // original game's event name.
    std::string generatedEvent_;
    std::vector<int> choiceHistory_;
    bool lastMainTextLiteral_ = true;
    std::string lastMainText_;
    // ShipEvent::auto_blueprint of the current encounter.
    std::string encounterAutoBlueprint_;
    int distressWaitTicks_ = 0;
    int hackingDroneSpace_ = 0;
    int hackingTarget_ = -1;
    int mindTarget_ = -1;
    int mindTargetOwner_ = 1;

    // MindSystem::queuedCrew: QueueMindControl 0x60a040 (CombatControl::SelectTarget
    // for the player's click, CombatAI::UpdateMindControl 0x605bd0 after the ShipAI
    // pass) stores the room's crew; the source's next MindSystem::OnLoop 0x60a990
    // hands them to InitiateMindControl 0x60a280. Indexed by the source ship.
    struct MindQueuedCrew {
        int owner = 0;
        int index = -1;
        FTL_NATIVE_JSON(MindQueuedCrew, owner, index)
    };

    std::array<std::vector<MindQueuedCrew>, 2> mindQueue_;
    // CrewMember::SetResisted from InitiateMindControl, read back by the physical
    // ShipManager::OnRender 0x49abc0 (GetResisted 0x46e860) into a RESIST message.
    std::vector<MindQueuedCrew> mindResisted_;
    int reactorPowerCap_ = 64;
    int reactorPowerLoss_ = 0;
    int reactorPowerDivide_ = 1;
    // TeleportSystem::chargeLevel 0x620ee0, iArmed and CompleteShip::teleTargetRoom
    // 0x4090a0. The charge ring is presentation (`TeleportSystem::Charged 0x620920`
    // gates on the lock, not the charge); the arm command lives for one tick before
    // CompleteShip::OnLoop 0x4094a0 consumes it.
    float teleportCharge_ = 0;
    int teleportArmed_ = 0;
    int teleportTarget_ = -1;
    int clonePending_ = -1;
    float cloneTimer_ = -1;
    float cloneDeathTimer_ = -1;
    float cloneDuration_ = 12;
    int playerIntruderCount_ = 0;
    bool boostFtl_ = false;
    int aiming_ = -1;
    int gameOverCommand_ = -1;

    void addScore(int& counter, int amount) {
        if (!tutorialOwned_) {
            const auto total = static_cast<std::int64_t>(counter) + amount;
            counter =
                static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max()));
        }
    }

    int scoreScrap_ = 0;
    int scoreShips_ = 0;
    int scoreBeacons_ = 1;
    // Enemy destruction is a countdown because its visuals use the remaining
    // duration; player hull loss tracks elapsed time independently so the
    // Game Over transition follows Ship::DestroyedDone without reusing enemy
    // burst state.
    CombatExplosion playerExplosionState_;
    std::array<float, 2> playerEngineAnimationTimes_{};
    std::map<int, std::vector<float>> playerRoomFireAnimationTimes_;
    std::map<int, std::vector<FireAnimationState>> playerFireSpreaderAnimations_;
    std::array<CombatHudWarning, 5> armamentWarnings_;
    float explosion_ = 0;
    float explosionDuration_ = 3.f;
    // MouseControl presentation state; advanced only by unscaled UI steps.
    float doorCursorTime_ = 0;
    float doorCursorDelay_ = 0;
    std::uint64_t playerExplosionStartTick_ = 0;
    SDL_FPoint dragStart_{};
    std::array<int, 4> resourceDeltas_{};
    CombatAbility hacking_;
    CombatAbility mind_;
    CombatAbility cloak_;
    CombatAbility battery_;
    EventResources pendingResources_;
    bool secretSector_ = false;
    bool pendingSavedMissileFeedback_ = false;
    bool pendingSecretSector_ = false;
    bool openingEvent_ = true;
    bool mapOpen_ = false;
    bool hostile_ = false;
    bool nebula_ = false;
    std::string hazard_;
    std::map<std::string, std::string> devUiFields_;
    std::string nativeDataDirectory_;
    original_save::Context originalSaveContext() const;
    original_save::SaveGame originalSave() const;
    original_save::Ship originalShip(bool enemy) const;
    original_save::Crew originalCrew(const RunCrew& member, bool owned, bool aboardEnemy) const;
    void originalStarMap(original_save::SaveGame& save) const;
    void originalEncounter(original_save::SaveGame& save) const;
    original_save::Projectile originalProjectile(const CombatProjectile& projectile,
                                                 int selfId) const;
    original_save::SpaceDroneState originalSpaceDrone(const CombatDrone& drone) const;
    original_save::WeaponFactory originalWeapon(const CombatWeapon& weapon,
                                                const std::string& blueprint,
                                                int owner,
                                                const std::map<std::uint64_t, int>& ids) const;
    original_save::BattleState originalBattle(bool enemy,
                                              const std::map<std::uint64_t, int>& ids) const;
    // Native event/ship-event keys to the names the original assigns them.
    std::string originalEventName(const std::string& key) const;
    std::string originalShipEventName(const std::string& key) const;
    std::string devUiFocus_;
    std::string devUiStatus_;
    int devUiTab_ = 0;
    bool devUiOpen_ = false;
    bool revisiting_ = false;
    bool advanced_ = false;
    bool open_ = false;
    bool event_ = false;
    bool userPause_ = false;
    bool shift_ = false;
    bool dragging_ = false;
    bool distress_ = false;
    bool forceAutofire_ = false;
    int eventFontSize_ = 12;
    std::deque<ProgressionPopup> achievementPopupQueue_;
    std::uint32_t achievementPopupTicks_ = 0;
    bool achievementPopupRunning_ = false;
    void renderAchievementPopup();
    std::string abilityAim_;
    bool autofire_ = false;
    bool surrenderOffered_ = false;
    bool surrenderDialog_ = false;
    bool defeated_ = false;
    void initializeCampaign();
    void campaignTurn();
    void forceBossJump();
    bool campaignArrival();
    void startBossStage();
    void rememberBoss();
    void finishBossStage();
    void advanceBoss();
    void terminal(bool victory, const std::string& textKey);
    bool enemyExplosionActive_ = false;
    bool playerExplosionStarted_ = false;
    bool doorCursorReverse_ = false;

    bool profileRunStarted_ = false;
    std::uint64_t profileRunId_ = 0;
    int lifetimeShipsBase_ = 0;
    int lifetimeScrapBase_ = 0;
    ProgressionLedger progression_;
    ProgressionRun progressionRun_;
    void updateProgression();

    void progressionBaseline(const nlohmann::json& profile) {
        lifetimeShipsBase_ = profile.value("ships", 0);
        lifetimeScrapBase_ = profile.value("scrap", 0);
        if (profileRunId_ != 0 && profile.contains("active_run") &&
            profile.at("active_run").at("id").get<std::uint64_t>() == profileRunId_) {
            lifetimeShipsBase_ -= profile.at("active_run").at("ships").get<int>();
            lifetimeScrapBase_ -= profile.at("active_run").at("scrap").get<int>();
        }
    }

    Tutorial tutorial_{archive_};
    bool tutorialOwned_ = false;
    TutorialObservation tutorialObservation() const;
    void tutorialEntry();
    void tutorialUpdate();
    bool tutorialClick(float x, float y, bool right);
    void tutorialRender();
    Rect tutorialContinueRect() const;
    std::vector<std::string> tutorialLines() const;
    void checkpoint(nlohmann::json& data, bool loading);
    void validateCheckpoint() const;

public:
    void initializeApplicationSpace();

    bool campaignVictory() const {
        return campaign_.victory;
    }

    bool terminalRecorded() const {
        return campaign_.recorded;
    }

    void markTerminalRecorded() {
        campaign_.recorded = true;
    }

    void progression(const nlohmann::json& profile) {
        auto ledger = ProgressionLedger::read(profile);
        progression_ = std::move(ledger);
        progressionBaseline(profile);
    }

    ProgressionLedger progressionLedger() const {
        return progression_;
    }

    void progressionLedger(ProgressionLedger ledger, const nlohmann::json& profile) {
        progression_ = std::move(ledger);
        progressionBaseline(profile);
    }

    void mergeProgression(const nlohmann::json& profile) {
        progression_.merge(ProgressionLedger::read(profile));
        progressionBaseline(profile);
    }

    std::vector<ProgressionPopup> progressionPopups() {
        return progression_.takePopups();
    }

    bool profileRunStarted() const {
        return profileRunStarted_;
    }

    void profileRunStarted(bool pending) {
        profileRunStarted_ = pending;
    }

    std::uint64_t profileRunId() const {
        return profileRunId_;
    }

    void profileRunId(std::uint64_t id) {
        profileRunId_ = id;
    }

    nlohmann::json controlState() const {
        nlohmann::json front = nullptr;
        if (!achievementPopupQueue_.empty()) {
            const auto& popup = achievementPopupQueue_.front();
            front = {{"id", popup.id},
                     {"family", popup.family},
                     {"layout", popup.layout},
                     {"difficulty", popup.difficulty}};
        }
        return {{"force_autofire", forceAutofire_},
                {"ability_aim", abilityAim_},
                {"pointer_valid", pointerValid_},
                {"crew_box_hover", crewBoxHover_},
                {"event_font_size", eventFontSize_},
                {"achievement_popup",
                 {{"queue_size", achievementPopupQueue_.size()},
                  {"ticks", achievementPopupTicks_},
                  {"running", achievementPopupRunning_},
                  {"front", front}}}};
    }

    nlohmann::json runStatistics() const;
    nlohmann::json crewRecords() const;

    nlohmann::json progressionState() const {
        return progression_.json();
    }

    void options(const RunOptions& options);

    void pauseKeyName(std::string name) {
        pauseKeyName_ = std::move(name);
    }

    void crewKeyNames(std::array<std::string, 8> names) {
        crewKeyNames_ = std::move(names);
    }

    void armamentKeyLabels(std::array<int, 8> weapons, std::array<int, 8> drones) {
        weaponKeyLabels_ = weapons;
        droneKeyLabels_ = drones;
    }

    const RunOptions& options() const {
        return options_;
    }

    void resetEventChoiceDelay() {
        eventChoiceOpenedUiTick_ = animation_;
    }

    void notify(const std::string& value) {
        voyageMessage_ = value;
    }

    void save(const std::string& path);
    void load(const std::string& path);

    // Folder of the native checkpoint; original-save export backups live here.
    void nativeDataDirectory(std::string path) {
        nativeDataDirectory_ = std::move(path);
    }

    // Writes this run as the original game's continue.sav in `folder` (empty:
    // the original's own save folder), backing up any file it replaces.
    // Into the original's own folder, `closeOriginal` terminates a running
    // FTLGame.exe first (the developer button); otherwise it refuses the export.
    nlohmann::json exportOriginalSave(const std::string& folder, bool closeOriginal = false);
    // Parses an original continue.sav with this build's blueprints and checks
    // that it serializes back to the same bytes.
    nlohmann::json originalSaveRoundTrip(const std::string& file) const;

    Run(Renderer& r, const Archive& a, Audio& audio, RandomStream* sessionRandom = nullptr)
        : renderer_(r), archive_(a), audio_(audio), catalog_(a), texts_(a),
          painter_(r, a, catalog_), events_(a) {
        if (sessionRandom) {
            sessionRandom_.bind(*sessionRandom);
        }
        eventRandom_.bind(sessionRandom_);
        combatRandom_.bind(sessionRandom_);
        resourceFeedbackRandom_.bind(sessionRandom_);
    }

    void start(const Hangar& hangar,
               const std::string& locale,
               std::uint32_t seed = 0,
               bool preserveSession = false);
    void startTutorial(const Hangar& hangar,
                       const std::string& locale,
                       std::uint32_t seed = 0,
                       bool preserveSession = false);

    bool tutorialRunning() const {
        return tutorialOwned_;
    }

    // CommandGui::KeyDown 0x5201f0 returns while jumping, and its focus-window
    // dispatch (GameOver, MenuScreen, options, StarMap, event ChoiceBox,
    // InputBox, Store, ship screens) consumes the key before the options
    // hotkey.
    bool optionsHotkeyReachable() const {
        return panel_.empty() && (!event_ || arrivalTicks_ != 0) && !mapOpen_ && !defeated_ &&
               jumpTicks_ == 0;
    }

    nlohmann::json tutorialState() const;

    void close() {
        tutorialOwned_ = false;
        tutorial_.stop();
        open_ = false;
        eventChoiceHoverValid_ = false;
        audio_.pauseLoops(true);
        audio_.music("audio/music/bp_MUS_TitleScreen.ogg");
    }

    bool isOpen() const {
        return open_;
    }

    bool defeated() const {
        return defeated_;
    }

    std::uint64_t gameplayTicks() const {
        return tick_;
    }

    bool gameplayClockActive() const {
        return open_ && !defeated_ && panel_.empty() && !devUiOpen_ && !externalModal_ &&
               !userPause_ && !mapOpen_ && (!event_ || jumpTicks_ > 0 || arrivalTicks_ > 0);
    }

    void updateAudio();

    int takeGameOverCommand() {
        const int command = gameOverCommand_;
        gameOverCommand_ = -1;
        return command;
    }

    void move(float x, float y) {
        if (mouseX_ != x || mouseY_ != y) {
            hoverTick_ = animation_;
        }
        mouseX_ = x;
        mouseY_ = y;
        if (pendingStoreOffer_ < 0) {
            storeMouseX_ = x;
            storeMouseY_ = y;
        }
        // ChoiceBox::MouseMove 0x4e82b0 marks enabled choices as a valid
        // pointer; MouseControl::OnRender 0x4d6e00 emits hoverBeep only on
        // the invalid-to-valid edge. A direct click also calls move first.
        bool validChoice = false;
        if (event_ && jumpTicks_ == 0 && arrivalTicks_ == 0) {
            for (std::size_t index = 0; index < choices_.size(); ++index) {
                if (choices_[index].enabled && Rect{enemy_.present ? 202.f : 352.f,
                                                    choiceRow(index) - 10,
                                                    565,
                                                    choiceHeight(index)}
                                                   .contains(x, y)) {
                    validChoice = true;
                    break;
                }
            }
        }
        eventChoiceHoverValid_ = validChoice;
        std::string hudTarget;
        if (!panel_.empty() && !event_ && !mapOpen_ && !defeated_ && !writeErrorOpen_ &&
            !devUiOpen_) {
            hudTarget = panelHoverSoundTarget(x, y);
        }
        if (panel_.empty() && !event_ && !mapOpen_ && jumpTicks_ == 0 && arrivalTicks_ == 0 &&
            !defeated_ && !writeErrorOpen_ && !devUiOpen_) {
            if (canOpenMap() && Rect{536, 35, 74, 29}.contains(x, y)) {
                hudTarget = "jump";
            } else if (shipButtonActive() && Rect{625, 22, 74, 55}.contains(x, y)) {
                hudTarget = "ship";
            } else if (hudOptionsRect().contains(x, y)) {
                hudTarget = "options";
            } else if (hudStoreAvailable() && hudStoreRect().contains(x, y)) {
                hudTarget = "store";
            } else {
                // CrewControl's save/return station GenericButtons.
                const float stationY = 154.f + float(crewRoster().size()) * 30;
                if (Rect{23, stationY + 8, 20, 20}.contains(x, y)) {
                    hudTarget = "stations:save";
                } else if (Rect{64, stationY + 8, 20, 20}.contains(x, y)) {
                    hudTarget = "stations:return";
                }
            }
        }
        if (mapOpen_ && distressWaitTicks_ == 0 && !writeErrorOpen_ && !devUiOpen_) {
            if (fuel_ > 0 && graph_.beacons()[graph_.current()].exit && sector_ < 8 &&
                nextSectorButtonRect().contains(x, y)) {
                hudTarget = "map:next_sector";
            } else if (!tutorialOwned_ && fuel_ > 0 && Rect{937, 624, 124, 30}.contains(x, y)) {
                hudTarget = "map:cancel";
            } else if (canWaitAtBeacon() && Rect{730, 624, 100, 30}.contains(x, y)) {
                hudTarget = "map:wait";
            } else if (fuel_ == 0 && Rect{956, 624, 116, 30}.contains(x, y)) {
                hudTarget = "map:distress";
            }
        }
        // GenericButton::MouseMove 0x573850 calls SetValid(1, !bHover): the
        // entry edge sets newHover, so OnRender beeps even between buttons.
        if (!hudTarget.empty() && hudTarget != hudHoverSoundTarget_) {
            pointerNewHover_ = true;
        }
        hudHoverSoundTarget_ = std::move(hudTarget);
        int mapTarget = -1;
        if (mapOpen_ && distressWaitTicks_ == 0 && !writeErrorOpen_ && !devUiOpen_) {
            const int beacon = hoveredBeacon();
            const auto& neighbors = graph_.beacons().at(graph_.current()).neighbors;
            // StarMap::OnLoop 0x5d13a0 marks only potential destinations valid.
            if (beacon >= 0 && beacon != graph_.current() &&
                (fuel_ == 0 || std::ranges::find(neighbors, beacon) != neighbors.end())) {
                mapTarget = beacon;
            }
        }
        // StarMap::MouseMove 0x5d6590 restarts targetBoxTimer on a new
        // potentialLoc or potentialSectorChoice.
        if (mapTarget != -1 && mapTarget != mapHoverSoundBeacon_) {
            targetBoxTick_ = animation_;
            targetBoxRunning_ = true;
        }
        mapHoverSoundBeacon_ = mapTarget;
        if (panel_ == "sector") {
            const int potential = sectorPotentialAt(x, y);
            if (potential != -1 && potential != sectorPotential_) {
                targetBoxTick_ = animation_;
                targetBoxRunning_ = true;
            }
            sectorPotential_ = potential;
        }
        if (writeErrorOpen_) {
            writeErrorMove(x, y);
        }
    }

    void click(float x, float y, bool right = false);
    // Once per presented frame: mirrors MouseControl::Reset, the GUI's
    // SetValid/SetSilentValid calls and OnRender's hoverBeep edge.
    void updatePointer();

    std::string panelHoverSoundTarget(float x, float y) const;
    std::string storeHoverSoundTarget(float x, float y) const;
    void noteCrewHealthChange(const RunCrew& member, float previous);
    void key(const std::string& key);

    bool repeatTextKey(const std::string& key) {
        // Escape is not a text-only key: its key-down reaches CommandGui::KeyDown
        // 0x5201f0 before TEXT_CANCEL (see Run::key).
        if (renameCrew_ != -1 && !writeErrorOpen_ &&
            (key == "Backspace" || key == "Delete" || key == "Left" || key == "Right" ||
             key == "Home" || key == "End" || key == "Clear" || key == "Enter")) {
            crewRenameKey(key);
            return true;
        }
        return false;
    }

    bool userPaused() const {
        return userPause_;
    }

    void userPaused(bool paused) {
        userPause_ = paused;
    }

    void semantic(const std::string& target, const std::string& action);

    void inputText(const std::string& value) {
        if (writeErrorOpen_) {
            return;
        }
        if (devUiOpen_) {
            devUiText(value);
        } else if (renameCrew_ != -1) {
            crewTextInput(value);
        }
    }

    void modifier(bool shift) {
        shift_ = shift;
    }

    void dragBegin(float x, float y) {
        if (writeErrorOpen_) {
            logging::verbose("pointer", "drag begin ignored: write-error dialog open");
            return;
        }
        if (inventoryPointerDown(x, y)) {
            logging::verbose(
                "pointer", "drag begin taken by inventory drag '{}'", inventoryDragKind_);
            return;
        }
        dragStart_ = {x, y};
        dragging_ = true;
    }

    void cancelPointer() {
        dragging_ = false;
        inventoryDragKind_.clear();
        inventoryDragIndex_ = -1;
    }

    void dragEnd(float x, float y);
    void diagnosticFixture(const std::string& name, const nlohmann::json& values = {});
    nlohmann::json devCommand(const nlohmann::json& request);
    void step(std::uint32_t ticks, bool legacy = true);
    void uiStep(std::uint32_t ticks);
    void applicationUiStep(std::uint32_t ticks);

    void presentation(float alpha, float uiFraction) {
        presentationAlpha_ = alpha;
        uiFraction_ = uiFraction;
        uiAlpha_ = uiFraction;
    }

    float travelPresentationSeconds() const;
    void render();
    void renderOuter();
    void renderApplicationPopup();
    void showWriteError();
    void clearWriteError();

    bool writeErrorOpen() const {
        return writeErrorOpen_;
    }

    Rect writeErrorChoiceRect() const;
    void writeErrorMove(float x, float y);
    void writeErrorClick(float x, float y);
    bool writeErrorKey(const std::string& key);

    void dismissWriteError() {
        writeErrorOpen_ = false;
        writeErrorHovered_ = false;
    }

    void renderWriteError();

    void applicationLanguage(std::string locale) {
        locale_ = std::move(locale);
    }

    void weaponAnimationExplosions(const std::map<std::string, std::string>& registry) {
        catalog_.weaponAnimationExplosions(registry);
    }

    void externalModal(bool active) {
        externalModal_ = active;
    }

    void renderBoundary();

    void pointerPosition(float x, float y) {
        mouseX_ = x;
        mouseY_ = y;
    }

    nlohmann::json state() const;
};
} // namespace ftl
