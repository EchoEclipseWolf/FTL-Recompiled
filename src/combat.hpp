#pragma once
#include "native_json.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ftl {
class BlueprintCatalog;
class RandomStream;
struct ShipDefinition;
struct SystemDefinition;

struct CombatGib {
    SDL_FPoint movement{};
    SDL_FPoint offset{};
    float rotationSpeed = 0;
    float rotation = 0;
    FTL_NATIVE_JSON(CombatGib, movement, offset, rotationSpeed, rotation)
};

struct CombatExplosionEffect {
    std::string animation;
    SDL_FPoint position{};
    float elapsed = 0;
    float scale = 1;
    FTL_NATIVE_JSON(CombatExplosionEffect, animation, position, elapsed, scale)
};

struct CombatExplosion {
    bool initialized = false;
    bool finalBoom = false;
    bool jumpOut = false;
    float soundTimer = -1;
    float explosionTimer = 0;
    std::vector<CombatGib> gibs;
    std::vector<CombatExplosionEffect> effects;

    friend void to_json(nlohmann::json& data, const CombatExplosion& value) {
        data = {{"initialized", value.initialized},
                {"finalBoom", value.finalBoom},
                {"jumpOut", value.jumpOut},
                {"soundTimer", value.soundTimer},
                {"explosionTimer", value.explosionTimer},
                {"gibs", value.gibs},
                {"effects", value.effects}};
    }

    friend void from_json(const nlohmann::json& data, CombatExplosion& value) {
        nativeRead(data.at("initialized"), value.initialized);
        nativeRead(data.at("finalBoom"), value.finalBoom);
        nativeRead(data.at("soundTimer"), value.soundTimer);
        nativeRead(data.at("explosionTimer"), value.explosionTimer);
        nativeRead(data.at("gibs"), value.gibs);
        nativeRead(data.at("effects"), value.effects);
        value.jumpOut = data.contains("jumpOut") ? nativeValue<bool>(data.at("jumpOut")) : false;
    }
};

// Special-system timers are game seconds, independent of rendering and wall time.
struct CombatAbility {
    bool active = false;
    float elapsed = 0;
    float duration = 0;
    float cooldown = 0;
    std::string soundEffect;

    friend void to_json(nlohmann::json& data, const CombatAbility& value) {
        data = {{"active", value.active},
                {"elapsed", value.elapsed},
                {"duration", value.duration},
                {"cooldown", value.cooldown},
                {"soundEffect", value.soundEffect}};
    }

    friend void from_json(const nlohmann::json& data, CombatAbility& value) {
        nativeRead(data.at("active"), value.active);
        nativeRead(data.at("elapsed"), value.elapsed);
        nativeRead(data.at("duration"), value.duration);
        nativeRead(data.at("cooldown"), value.cooldown);
        value.soundEffect = data.contains("soundEffect")
                                ? nativeValue<std::string>(data.at("soundEffect"))
                                : std::string{};
    }
};

struct CombatHudWarning {
    std::string text;
    float elapsed = -1;
    bool broken = false;
    FTL_NATIVE_JSON(CombatHudWarning, text, elapsed, broken)
};

struct CombatProjectile {
    std::uint64_t id = 0;
    int owner = 0;
    int targetShip = 1;
    int room = -1;
    int space = 0;
    std::string weapon;
    std::string projectileImage;
    std::string asteroidShieldSound = "hitShield1";
    std::string asteroidHullSound = "hitHull1";
    std::string explosionImage;
    bool soundsInitialized = false;
    std::string hitShipSound;
    std::string hitShieldSound;
    std::string missSound;
    int droneTargetable = -1;
    std::uint64_t defenseTargetId = 0;
    std::uint64_t defenseDroneTargetId = 0;
    std::uint64_t defenseHackingTargetId = 0;
    std::uint64_t defenseSpaceDroneId = 0;
    bool crystalShard = false;
    // Transient: BoarderPodDrone::OnLoop 0x427820's delivery DamageTarget
    // (breach chance 10, nothing else). Never stored.
    bool podBreach = false;
    SDL_FPoint target{};
    SDL_FPoint targetEnd{};
    SDL_FPoint position{};
    SDL_FPoint previous{};
    SDL_FPoint direction{};
    float speed = 0;
    float age = 0;
    float spinAngle = 0;
    float entryAngle = 0;
    int flightFrame = -1;
    bool checkedShield = false;
    bool missed = false;
    bool resolved = false;
    bool dead = false;
    bool intercepted = false;
    bool arrived = false;
    bool bombSuperShield = false;
    bool bombBypass = false;
    float bombAnimationElapsed = 0;
    float bombDelay = .4f;
    std::uint64_t bombMissTick = 0;
    bool fake = false;
    // BeamWeapon::BeamWeapon 0x4357c0 (and BeamWeapon::LoadProjectile 0x434d40's
    // empty slot) leaves `last_collision.point` at (-2147483600,-2147483600)
    // until the first sweep, so the first ShipManager::DamageBeam 0x49e4d0 call
    // always sees a grid cell and a room that differ from the beam head's.
    // Native keeps the previous head in `previous` and mirrors that first-call
    // state with this flag.
    bool beamStepped = false;
    bool beamVisualInitialized = false;
    SDL_FPoint beamOrigin{};
    SDL_FPoint beamSubStart{};
    SDL_FPoint beamSubEnd{};
    float beamHeading = 0;
    float beamDrift = 0;
    int beamLastDamage = 0;
    bool beamPierced = true;
    float beamAnimationTimer = -1;
    SDL_FPoint beamLastSmoke{2147483600.f, 2147483600.f};
    std::vector<CombatExplosionEffect> beamContacts;
    std::vector<CombatExplosionEffect> beamSmoke;
    // BeamWeapon::CollisionCheck LABEL_54 advances last_collision even when
    // shields block DamageBeam; render interpolation has its own previous.
    SDL_FPoint beamLastCollision{-2147483600.f, -2147483600.f};
    int damageBonus = 0;
    std::vector<int> hitRooms;

    friend void to_json(nlohmann::json& nlohmann_json_j, const CombatProjectile& nlohmann_json_t) {
        nlohmann_json_j["defenseSpaceDroneId"] = nlohmann_json_t.defenseSpaceDroneId;
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 id,
                                                 owner,
                                                 targetShip,
                                                 room,
                                                 space,
                                                 weapon,
                                                 projectileImage,
                                                 asteroidShieldSound,
                                                 asteroidHullSound,
                                                 explosionImage,
                                                 droneTargetable,
                                                 defenseTargetId,
                                                 defenseDroneTargetId,
                                                 defenseHackingTargetId,
                                                 crystalShard,
                                                 target,
                                                 targetEnd,
                                                 position,
                                                 previous,
                                                 direction,
                                                 speed,
                                                 age,
                                                 spinAngle,
                                                 entryAngle,
                                                 checkedShield,
                                                 missed,
                                                 resolved,
                                                 intercepted,
                                                 arrived,
                                                 fake,
                                                 beamStepped,
                                                 damageBonus,
                                                 hitRooms,
                                                 beamLastCollision,
                                                 beamVisualInitialized,
                                                 beamOrigin,
                                                 beamSubStart,
                                                 beamSubEnd,
                                                 beamHeading,
                                                 beamDrift,
                                                 beamLastDamage,
                                                 beamPierced,
                                                 beamAnimationTimer,
                                                 beamLastSmoke,
                                                 beamContacts,
                                                 beamSmoke,
                                                 flightFrame,
                                                 soundsInitialized,
                                                 hitShipSound,
                                                 hitShieldSound,
                                                 missSound,
                                                 bombSuperShield,
                                                 bombBypass,
                                                 bombAnimationElapsed,
                                                 bombDelay,
                                                 bombMissTick,
                                                 dead))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j,
                          CombatProjectile& nlohmann_json_t) {
        nlohmann_json_t.defenseSpaceDroneId =
            nlohmann_json_j.contains("defenseSpaceDroneId")
                ? nativeValue<std::uint64_t>(nlohmann_json_j.at("defenseSpaceDroneId"))
                : 0;
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM,
                                                 id,
                                                 owner,
                                                 targetShip,
                                                 room,
                                                 space,
                                                 weapon,
                                                 projectileImage,
                                                 asteroidShieldSound,
                                                 asteroidHullSound,
                                                 explosionImage,
                                                 droneTargetable,
                                                 defenseTargetId,
                                                 defenseDroneTargetId,
                                                 defenseHackingTargetId,
                                                 crystalShard,
                                                 target,
                                                 targetEnd,
                                                 position,
                                                 previous,
                                                 direction,
                                                 speed,
                                                 age,
                                                 spinAngle,
                                                 entryAngle,
                                                 checkedShield,
                                                 missed,
                                                 resolved,
                                                 intercepted,
                                                 arrived,
                                                 fake,
                                                 beamStepped,
                                                 damageBonus,
                                                 hitRooms))
        if (nlohmann_json_j.contains("beamLastCollision")) {
            nativeRead(nlohmann_json_j.at("beamLastCollision"), nlohmann_json_t.beamLastCollision);
        } else if (nlohmann_json_t.beamStepped) {
            // Older checkpoints had only the immediately preceding simulation point.
            nlohmann_json_t.beamLastCollision = nlohmann_json_t.previous;
        } else {
            nlohmann_json_t.beamLastCollision = {-2147483600.f, -2147483600.f};
        }
        nlohmann_json_t.flightFrame = -1;
        if (nlohmann_json_j.contains("flightFrame")) {
            nativeRead(nlohmann_json_j.at("flightFrame"), nlohmann_json_t.flightFrame);
        }
        nlohmann_json_t.soundsInitialized =
            nlohmann_json_j.contains("soundsInitialized")
                ? nativeValue<bool>(nlohmann_json_j.at("soundsInitialized"))
                : false;
        nlohmann_json_t.hitShipSound =
            nlohmann_json_j.contains("hitShipSound")
                ? nativeValue<std::string>(nlohmann_json_j.at("hitShipSound"))
                : std::string{};
        nlohmann_json_t.hitShieldSound =
            nlohmann_json_j.contains("hitShieldSound")
                ? nativeValue<std::string>(nlohmann_json_j.at("hitShieldSound"))
                : std::string{};
        nlohmann_json_t.missSound = nlohmann_json_j.contains("missSound")
                                        ? nativeValue<std::string>(nlohmann_json_j.at("missSound"))
                                        : std::string{};
        const CombatProjectile nlohmann_json_default_obj{};
        nlohmann_json_t.dead = nlohmann_json_j.contains("dead")
                                   ? nativeValue<bool>(nlohmann_json_j.at("dead"))
                                   : nlohmann_json_t.resolved && nlohmann_json_t.age >= 4.f;
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 bombSuperShield,
                                                 bombBypass,
                                                 bombAnimationElapsed,
                                                 bombDelay,
                                                 bombMissTick,
                                                 beamVisualInitialized,
                                                 beamOrigin,
                                                 beamSubStart,
                                                 beamSubEnd,
                                                 beamHeading,
                                                 beamDrift,
                                                 beamLastDamage,
                                                 beamPierced,
                                                 beamAnimationTimer,
                                                 beamLastSmoke,
                                                 beamContacts,
                                                 beamSmoke))
    }
};

// Native IDs and game-time seconds. No original x86 object layout is reused.
struct CombatWeapon {
    std::vector<CombatProjectile> pendingProjectiles;
    bool animationInitialized = false;
    bool animationFiring = false;
    bool animationFireShot = false;
    bool emittedVolley = false;
    float animationElapsed = 0;
    int animationFrame = 0;
    int animationBoostLevel = -1;
    int factoryHackLevel = 0;
    bool animationPowered = false;
    float animationSlide = 0;
    int animationHackLevel = 0;
    float hackSparksElapsed = -1;
    SDL_FPoint hackSparksPosition{-1, -1};
    bool hackSparksMirror = false;
    std::string weaponExplosionImage;
    float weaponExplosionElapsed = -1;
    int bonusPower = 0;
    bool repowerPending = false;
    float charge = 0;
    float cooldown = 0;
    int targetRoom = -1;
    SDL_FPoint target{};
    std::vector<int> targetRooms;
    std::vector<SDL_FPoint> targetPoints;
    int targetGoal = 0;
    int chargeLevel = 0;
    int goalChargeLevel = 0;
    int boostLevel = 0;
    SDL_FPoint targetEnd{};
    int targetRoomEnd = -1;
    bool targetFirstPending = false;
    bool targetSelf = false;
    bool queued = false;
    bool autofire = false;
    bool firedOnce = false;
    std::uint64_t lastProjectileId = 0;
    int volleyRemaining = 0;
    float volleyTimer = 0;
    int volleyRoom = -1;
    SDL_FPoint volleyTarget{};
    SDL_FPoint volleyEnd{};
    bool volleySelf = false;
    std::vector<int> volleyRooms;
    std::vector<SDL_FPoint> volleyPoints;
    int volleyIndex = 0;
    float entryAngle = 0;

    void clearTargets(bool force = false) {
        // ClearAiming40c080 refuses to cancel the unconstructed remainder
        // once a multi-target volley has begun consuming targets.
        if (!force && volleyRemaining > 0 && targetGoal > 0 && volleyRemaining < targetGoal) {
            return;
        }
        targetRoom = targetRoomEnd = volleyRoom = -1;
        target = targetEnd = volleyTarget = volleyEnd = {};
        targetRooms.clear();
        targetPoints.clear();
        volleyRooms.clear();
        volleyPoints.clear();
        targetGoal = volleyRemaining = volleyIndex = 0;
        volleyTimer = 0;
        targetFirstPending = targetSelf = volleySelf = queued = false;
    }

    friend void to_json(nlohmann::json& j, const CombatWeapon& weapon) {
        j = nlohmann::json{
            {"bonusPower", weapon.bonusPower},
            {"repowerPending", weapon.repowerPending},
            {"charge", weapon.charge},
            {"cooldown", weapon.cooldown},
            {"targetRoom", weapon.targetRoom},
            {"target", weapon.target},
            {"targetRooms", weapon.targetRooms},
            {"targetPoints", weapon.targetPoints},
            {"targetGoal", weapon.targetGoal},
            {"chargeLevel", weapon.chargeLevel},
            {"goalChargeLevel", weapon.goalChargeLevel},
            {"boostLevel", weapon.boostLevel},
            {"targetEnd", weapon.targetEnd},
            {"targetRoomEnd", weapon.targetRoomEnd},
            {"targetFirstPending", weapon.targetFirstPending},
            {"targetSelf", weapon.targetSelf},
            {"queued", weapon.queued},
            {"autofire", weapon.autofire},
            {"volleyRemaining", weapon.volleyRemaining},
            {"volleyTimer", weapon.volleyTimer},
            {"volleyRoom", weapon.volleyRoom},
            {"volleyTarget", weapon.volleyTarget},
            {"volleyEnd", weapon.volleyEnd},
            {"volleySelf", weapon.volleySelf},
            {"volleyRooms", weapon.volleyRooms},
            {"volleyPoints", weapon.volleyPoints},
            {"volleyIndex", weapon.volleyIndex},
            {"entryAngle", weapon.entryAngle},
            {"firedOnce", weapon.firedOnce},
            {"lastProjectileId", weapon.lastProjectileId},
            {"pendingProjectiles", weapon.pendingProjectiles},
            {"animationInitialized", weapon.animationInitialized},
            {"animationFiring", weapon.animationFiring},
            {"animationFireShot", weapon.animationFireShot},
            {"emittedVolley", weapon.emittedVolley},
            {"animationElapsed", weapon.animationElapsed},
            {"animationFrame", weapon.animationFrame},
            {"animationBoostLevel", weapon.animationBoostLevel},
            {"factoryHackLevel", weapon.factoryHackLevel},
            {"animationPowered", weapon.animationPowered},
            {"animationSlide", weapon.animationSlide},
            {"animationHackLevel", weapon.animationHackLevel},
            {"hackSparksElapsed", weapon.hackSparksElapsed},
            {"hackSparksPosition", weapon.hackSparksPosition},
            {"hackSparksMirror", weapon.hackSparksMirror},
            {"weaponExplosionImage", weapon.weaponExplosionImage},
            {"weaponExplosionElapsed", weapon.weaponExplosionElapsed},
        };
    }

    friend void from_json(const nlohmann::json& j, CombatWeapon& weapon) {
        weapon.repowerPending =
            j.contains("repowerPending") && nativeValue<bool>(j.at("repowerPending"));
        weapon.pendingProjectiles =
            j.contains("pendingProjectiles")
                ? nativeValue<std::vector<CombatProjectile>>(j.at("pendingProjectiles"))
                : std::vector<CombatProjectile>{};
        weapon.animationInitialized =
            j.contains("animationInitialized") && nativeValue<bool>(j.at("animationInitialized"));
        weapon.animationFiring =
            j.contains("animationFiring") && nativeValue<bool>(j.at("animationFiring"));
        weapon.animationFireShot =
            j.contains("animationFireShot") && nativeValue<bool>(j.at("animationFireShot"));
        weapon.emittedVolley =
            j.contains("emittedVolley") && nativeValue<bool>(j.at("emittedVolley"));
        weapon.animationElapsed =
            j.contains("animationElapsed") ? nativeValue<float>(j.at("animationElapsed")) : 0;
        weapon.animationFrame =
            j.contains("animationFrame") ? nativeValue<int>(j.at("animationFrame")) : 0;
        weapon.animationBoostLevel =
            j.contains("animationBoostLevel") ? nativeValue<int>(j.at("animationBoostLevel")) : -1;
        weapon.factoryHackLevel =
            j.contains("factoryHackLevel") ? nativeValue<int>(j.at("factoryHackLevel")) : 0;
        weapon.animationPowered =
            j.contains("animationPowered") && nativeValue<bool>(j.at("animationPowered"));
        weapon.animationSlide =
            j.contains("animationSlide") ? nativeValue<float>(j.at("animationSlide")) : 0;
        weapon.animationHackLevel =
            j.contains("animationHackLevel") ? nativeValue<int>(j.at("animationHackLevel")) : 0;
        weapon.hackSparksElapsed =
            j.contains("hackSparksElapsed") ? nativeValue<float>(j.at("hackSparksElapsed")) : -1;
        weapon.hackSparksPosition = j.contains("hackSparksPosition")
                                        ? nativeValue<SDL_FPoint>(j.at("hackSparksPosition"))
                                        : SDL_FPoint{-1, -1};
        weapon.hackSparksMirror =
            j.contains("hackSparksMirror") && nativeValue<bool>(j.at("hackSparksMirror"));
        weapon.weaponExplosionImage = j.contains("weaponExplosionImage")
                                          ? nativeValue<std::string>(j.at("weaponExplosionImage"))
                                          : std::string{};
        weapon.weaponExplosionElapsed = j.contains("weaponExplosionElapsed")
                                            ? nativeValue<float>(j.at("weaponExplosionElapsed"))
                                            : -1;
        nativeRead(j.at("bonusPower"), weapon.bonusPower);
        nativeRead(j.at("charge"), weapon.charge);
        nativeRead(j.at("cooldown"), weapon.cooldown);
        nativeRead(j.at("targetRoom"), weapon.targetRoom);
        nativeRead(j.at("target"), weapon.target);
        nativeRead(j.at("targetRooms"), weapon.targetRooms);
        nativeRead(j.at("targetPoints"), weapon.targetPoints);
        nativeRead(j.at("targetGoal"), weapon.targetGoal);
        nativeRead(j.at("chargeLevel"), weapon.chargeLevel);
        nativeRead(j.at("goalChargeLevel"), weapon.goalChargeLevel);
        nativeRead(j.at("boostLevel"), weapon.boostLevel);
        nativeRead(j.at("targetEnd"), weapon.targetEnd);
        nativeRead(j.at("targetRoomEnd"), weapon.targetRoomEnd);
        nativeRead(j.at("targetFirstPending"), weapon.targetFirstPending);
        nativeRead(j.at("targetSelf"), weapon.targetSelf);
        nativeRead(j.at("queued"), weapon.queued);
        nativeRead(j.at("autofire"), weapon.autofire);
        nativeRead(j.at("volleyRemaining"), weapon.volleyRemaining);
        nativeRead(j.at("volleyTimer"), weapon.volleyTimer);
        nativeRead(j.at("volleyRoom"), weapon.volleyRoom);
        nativeRead(j.at("volleyTarget"), weapon.volleyTarget);
        nativeRead(j.at("volleyEnd"), weapon.volleyEnd);
        nativeRead(j.at("volleySelf"), weapon.volleySelf);
        nativeRead(j.at("volleyRooms"), weapon.volleyRooms);
        nativeRead(j.at("volleyPoints"), weapon.volleyPoints);
        nativeRead(j.at("volleyIndex"), weapon.volleyIndex);
        nativeRead(j.at("entryAngle"), weapon.entryAngle);
        weapon.firedOnce = false;
        if (j.contains("firedOnce")) {
            nativeRead(j.at("firedOnce"), weapon.firedOnce);
        }
        weapon.lastProjectileId = j.contains("lastProjectileId")
                                      ? nativeValue<std::uint64_t>(j.at("lastProjectileId"))
                                      : 0;
    }
};

// CrewMember::task (CrewTask 0x1a4): taskId 0 manning, 1 system repair, 2 fire,
// 3 breach, 4 idle, 5 dead, 6 healing, 7 intruders, 8 teleport. CrewMember's
// constructor and ClearTask 0x46f690 use {4, iRoomId, -1}; the original never
// saves it, so a loaded run starts from the idle task.
struct CrewTask {
    int taskId = 4;
    int room = -1;
    int system = -1;
    bool operator==(const CrewTask&) const = default;
};

// CrewAnimation::smokeEmitter: the people/firefight_cloud.png ParticleEmitter
// (ctor 0x586570: 64 particles, running, birthCounter .01) that every crew
// body carries, set up by CrewAnimation::OnInit 0x478200 (birthRate 3,
// lifespan 1, speed 1, angle 90) or the RockAnimation ctor 0x47bb80
// (birthRate FLT_MAX, lifespan .8, speed 1.3, random angle, at 15,15).
// CrewMember::OnLoop 0x4747f0 updates it every frame through OnUpdateEffects
// 0x47d360, so its births draw random32 whether or not anyone is fighting a
// fire; it is only drawn in status 4. The original saves no CrewAnimation, so
// its loads start a fresh emitter; native checkpoints keep it because those
// births share the session stream and a continue must match the uninterrupted
// run (legacy checkpoints without it still start fresh).
struct CrewSmokeParticle {
    float x = 0;
    float y = 0;
    float dx = 0;
    float dy = 0;
    float life = -1;
    bool alive = false;
};

struct CrewSmoke {
    std::array<CrewSmokeParticle, 64> particles{};
    float birthCounter = .01f;
    // Emitter origin relative to the body's 35px cell, and its centre angle.
    float x = 0;
    float y = 0;
    float angle = 90;
    bool running = true;
    bool configured = false;
    // CrewAnimation status 4 from the last OnUpdate 0x473600 and its direction
    // (0 down, 1 right, 2 up, 3 left); -1 while any other status plays.
    int fireDirection = -1;

    // Only live particles are stored, as [slot, x, y, dx, dy, life]: Birth
    // 0x586820 overwrites a free slot completely, so dead slots carry nothing.
    friend void to_json(nlohmann::json& json, const CrewSmoke& smoke) {
        auto live = nlohmann::json::array();
        for (std::size_t slot = 0; slot < smoke.particles.size(); ++slot) {
            const auto& particle = smoke.particles[slot];
            if (particle.alive) {
                live.push_back(
                    {slot, particle.x, particle.y, particle.dx, particle.dy, particle.life});
            }
        }
        json = {{"particles", live},
                {"birthCounter", smoke.birthCounter},
                {"x", smoke.x},
                {"y", smoke.y},
                {"angle", smoke.angle},
                {"running", smoke.running},
                {"configured", smoke.configured},
                {"fireDirection", smoke.fireDirection}};
    }

    friend void from_json(const nlohmann::json& json, CrewSmoke& smoke) {
        smoke = {};
        for (const auto& row : json.at("particles")) {
            const auto values = nativeValue<std::array<float, 6>>(row);
            const auto slot = std::size_t(values[0]);
            if (values[0] < 0 || float(slot) != values[0] || slot >= smoke.particles.size() ||
                smoke.particles[slot].alive) {
                throw std::out_of_range("invalid_crew_smoke_particle");
            }
            for (std::size_t index = 1; index < values.size(); ++index) {
                if (!std::isfinite(values[index]) || std::abs(values[index]) > 4096) {
                    throw std::out_of_range("invalid_crew_smoke_particle");
                }
            }
            // OnUpdate 0x586c00 retires a particle once its life falls below zero.
            if (values[5] < 0 || values[5] > 1) {
                throw std::out_of_range("invalid_crew_smoke_particle");
            }
            smoke.particles[slot] = {values[1], values[2], values[3], values[4], values[5], true};
        }
        nativeRead(json.at("birthCounter"), smoke.birthCounter);
        nativeRead(json.at("x"), smoke.x);
        nativeRead(json.at("y"), smoke.y);
        nativeRead(json.at("angle"), smoke.angle);
        nativeRead(json.at("running"), smoke.running);
        nativeRead(json.at("configured"), smoke.configured);
        nativeRead(json.at("fireDirection"), smoke.fireDirection);
        // The rock's birthRate is FLT_MAX, so its counter may hold that value.
        if (!std::isfinite(smoke.birthCounter) || smoke.birthCounter < -1 ||
            !std::isfinite(smoke.x) || !std::isfinite(smoke.y) || std::abs(smoke.x) > 64 ||
            std::abs(smoke.y) > 64 || !std::isfinite(smoke.angle) || smoke.angle < 0 ||
            smoke.angle > 360 || smoke.fireDirection < -1 || smoke.fireDirection > 3) {
            throw std::out_of_range("invalid_crew_smoke");
        }
    }
};

// Keep the separately recovered carrier, body and attack lifecycle fields
// adjacent for source review; this cold snapshot state accepts their padding.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct CombatDrone {
    std::uint64_t spaceActorId = 0;
    std::uint64_t spaceRegisteredOrder = 0;
    bool spacePendingArrival = false;
    std::uint64_t crewActorId = 0;
    std::uint64_t physicalCrewOrder = 0;
    CrewTask aiTask{};
    std::uint64_t attackTargetDroneId = 0;
    int crewPhysicalShip = -1;
    int crewRoom = -1;
    SDL_FPoint crewPosition{};
    int crewSlot = -1;
    std::string constructorExplosionImage;
    bool crewDying = false;
    bool crewDead = true;
    float crewDeathElapsed = 0;
    bool crewDeathDone = false;
    bool crewDeathLeft = true;
    std::string blueprint;
    std::string attackAnimation;
    bool bossSurge = false;
    int bonusPower = 0;
    bool deployed = false;
    bool powered = false;
    bool shotReady = false;
    bool repowerPending = false;
    bool orbitInitialized = false;
    float cooldown = 0;
    float ionStun = 0;
    float hackTime = 0;
    int controllerHackLevel = 0;
    float spaceExplosionElapsed = -1;
    int ionLastRoom = -1;
    float ionPulseCooldown = 6.f;
    float ionPulseCharge = -1.f;
    float ionPulseEffect = 0;
    float ionPulseExplosion = -1.f;
    SDL_FPoint ionPulsePosition{};
    float orbitAngle = -1000.f;
    float pause = -1.f;
    float additionalPause = 0;
    float repairBeamAge = 0;
    float shieldGlow = -1.f;
    float attackElapsed = 0;
    float attackGoal = 0;
    float shootGoal = 0;
    float shootElapsed = 0;
    float pendingDamage = 0;
    float medicalRate = 0;
    float healingElapsed = -1;
    float mindElapsed = -1;
    float stunElapsed = -1;
    float attackFlight = 0;
    float attackAnimationElapsed = 0;
    SDL_FPoint attackFrom{};
    SDL_FPoint attackTo{};
    bool attackHit = false;
    int attackTargetOwner = -1;
    int attackTargetIndex = -1;
    bool attackTargetDrone = false;
    int lifespan = -1;
    SDL_FPoint position{};
    SDL_FPoint destination{};
    SDL_FPoint basePosition{};
    SDL_FPoint velocity{};
    SDL_FPoint aimTarget{};
    SDL_FPoint repairBeamTarget{};
    float aimAngle = 0;
    bool hasAimTarget = false;
    std::uint64_t targetProjectile = 0;
    std::uint64_t targetDrone = 0;
    std::uint64_t targetSpaceDroneId = 0;
    int defenseTargetType = 5;
    std::uint64_t defenseCurrentTargetId = 0;
    std::uint64_t defenseShotTargetId = 0;
    bool defenseCurrentTargetIsDrone = false;
    bool defenseShotTargetIsDrone = false;
    SDL_FPoint defenseTargetVelocity{};
    float desiredAimAngle = -1;
    bool defenseDisrupted = false;
    float spaceMissElapsed = -1;
    SDL_FPoint spaceMissPosition{};
    // Fixture-only override of the blueprint's authored dodge, because every
    // archived drone blueprint authors dodge 0 and the powered-dodge collision
    // path cannot otherwise be reached. Negative means "use the blueprint".
    // Deliberately absent from the serializer below: the original has no such
    // per-instance state, so it must never reach a checkpoint.
    int space = 0;
    float health = 0;
    int room = -1;
    int targetRoom = -1;
    float workProgress = 0;
    float rebuildTimer = 0;
    bool delivered = false;
    float deliveryTimer = 0;
    std::vector<SDL_FPoint> crewPath{};
    std::vector<int> crewPathDoors{};
    int crewPathIndex = 0;
    int crewBlockingDoor = -1;
    int crewLastDoor = -1;
    bool crewNewGoal = true;
    int repairKind = 0;
    int repairTarget = -1;
    int repairRoom = -1;
    float heading = 0;
    float oldHeading = 0;
    float progressToDestination = 1;
    SDL_FPoint lastDestination{};
    float lastAimingAngle = 0;
    float hackAngle = 0;
    bool poweredLastFrame = false;
    bool deployedLastFrame = false;
    float spaceHackSparksElapsed = -1;
    bool spaceHackSparksMirror = false;
    std::vector<float> repairWavePhases;
    float hackingFlyingElapsed = -1;
    float hackingExtendingElapsed = -1;
    bool hackingFinishedSetup = false;
    float hackingFlashElapsed = 0;
    // SpaceDrone::beamCurrentTarget/beamFinalTarget/beamSpeed: the drone's own
    // copy of its last beam sweep, which aims the drone while additionalPause
    // runs (CombatDrone::OnLoop 0x4271d0).
    SDL_FPoint beamCurrentTarget{};
    SDL_FPoint beamFinalTarget{};
    float beamSpeed = 0;
    // DefenseDrone::currentSpeed (ctor: the blueprint speed). SuperShieldDrone
    // rewrites it after its movement, so the next frame flies at it.
    // Negative means "still the constructor value".
    float currentSpeed = -1.f;
    int crewKills = 0;
    int diagnosticDodge = -1;
    // CrewMember::iOnFire of the drone body, set by UpdateCrewmembers 0x49ee10
    // after its OnLoop and read by the next UpdateRepair and OnUpdate.
    int crewOnFire = 0;
    CrewSmoke crewSmoke{};
    // CrewMember::DirectModifyHealth 0x46eb20 / OnLoop 0x4747f0 on the drone
    // body: two game seconds of visible health after a real change below max.
    float healthChangeRemaining = 0;
    float lastPresentedHealth = -1;

    friend void to_json(nlohmann::json& nlohmann_json_j, const CombatDrone& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 blueprint,
                                                 bossSurge,
                                                 bonusPower,
                                                 deployed,
                                                 powered,
                                                 repowerPending,
                                                 orbitInitialized,
                                                 cooldown,
                                                 ionStun,
                                                 orbitAngle,
                                                 pause,
                                                 additionalPause,
                                                 repairBeamAge,
                                                 shieldGlow,
                                                 lifespan,
                                                 position,
                                                 destination,
                                                 velocity,
                                                 aimTarget,
                                                 repairBeamTarget,
                                                 aimAngle,
                                                 hasAimTarget,
                                                 targetProjectile,
                                                 targetDrone))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 space,
                                                 health,
                                                 room,
                                                 targetRoom,
                                                 workProgress,
                                                 rebuildTimer,
                                                 delivered,
                                                 deliveryTimer,
                                                 crewPath,
                                                 crewPathDoors,
                                                 crewPathIndex,
                                                 crewKills,
                                                 attackAnimation,
                                                 attackElapsed,
                                                 attackGoal,
                                                 attackFlight,
                                                 attackAnimationElapsed,
                                                 attackFrom,
                                                 attackTo,
                                                 attackHit,
                                                 attackTargetOwner,
                                                 attackTargetIndex,
                                                 attackTargetDrone,
                                                 crewBlockingDoor))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 crewLastDoor,
                                                 crewNewGoal,
                                                 ionLastRoom,
                                                 ionPulseCooldown,
                                                 ionPulseCharge,
                                                 ionPulseEffect,
                                                 ionPulseExplosion,
                                                 ionPulsePosition,
                                                 shootGoal,
                                                 shootElapsed,
                                                 pendingDamage,
                                                 medicalRate,
                                                 healingElapsed,
                                                 mindElapsed,
                                                 stunElapsed,
                                                 constructorExplosionImage,
                                                 crewDying,
                                                 crewDead,
                                                 crewDeathElapsed,
                                                 crewDeathDone,
                                                 crewDeathLeft,
                                                 shotReady))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 crewActorId,
                                                 physicalCrewOrder,
                                                 attackTargetDroneId,
                                                 hackTime,
                                                 controllerHackLevel,
                                                 spaceExplosionElapsed,
                                                 spaceActorId,
                                                 spaceRegisteredOrder,
                                                 spacePendingArrival))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(
            NLOHMANN_JSON_TO, crewPhysicalShip, crewRoom, crewPosition, crewSlot, basePosition))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 targetSpaceDroneId,
                                                 defenseTargetType,
                                                 defenseCurrentTargetId,
                                                 defenseShotTargetId,
                                                 defenseCurrentTargetIsDrone,
                                                 defenseShotTargetIsDrone,
                                                 defenseTargetVelocity,
                                                 desiredAimAngle,
                                                 defenseDisrupted,
                                                 spaceMissElapsed,
                                                 spaceMissPosition))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 repairKind,
                                                 repairTarget,
                                                 repairRoom,
                                                 heading,
                                                 oldHeading,
                                                 progressToDestination,
                                                 lastDestination,
                                                 lastAimingAngle,
                                                 hackAngle,
                                                 poweredLastFrame,
                                                 deployedLastFrame,
                                                 spaceHackSparksElapsed,
                                                 spaceHackSparksMirror,
                                                 repairWavePhases))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 hackingFlyingElapsed,
                                                 hackingExtendingElapsed,
                                                 hackingFinishedSetup,
                                                 hackingFlashElapsed,
                                                 beamCurrentTarget,
                                                 beamFinalTarget,
                                                 beamSpeed,
                                                 currentSpeed,
                                                 crewOnFire,
                                                 crewSmoke,
                                                 healthChangeRemaining,
                                                 lastPresentedHealth))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j, CombatDrone& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM,
                                                 blueprint,
                                                 bossSurge,
                                                 bonusPower,
                                                 deployed,
                                                 powered,
                                                 repowerPending,
                                                 orbitInitialized,
                                                 cooldown,
                                                 ionStun,
                                                 orbitAngle,
                                                 pause,
                                                 additionalPause,
                                                 repairBeamAge,
                                                 shieldGlow,
                                                 lifespan,
                                                 position,
                                                 destination,
                                                 velocity,
                                                 aimTarget,
                                                 repairBeamTarget,
                                                 aimAngle,
                                                 hasAimTarget,
                                                 targetProjectile,
                                                 targetDrone,
                                                 space,
                                                 health,
                                                 room,
                                                 targetRoom,
                                                 workProgress,
                                                 rebuildTimer,
                                                 delivered,
                                                 deliveryTimer,
                                                 crewPath,
                                                 crewPathDoors,
                                                 crewPathIndex,
                                                 crewKills))
        const CombatDrone nlohmann_json_default_obj{};
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 attackAnimation,
                                                 attackElapsed,
                                                 attackGoal,
                                                 attackFlight,
                                                 attackAnimationElapsed,
                                                 attackFrom,
                                                 attackTo,
                                                 attackHit,
                                                 attackTargetOwner,
                                                 attackTargetIndex,
                                                 attackTargetDrone,
                                                 crewBlockingDoor,
                                                 crewLastDoor,
                                                 crewNewGoal,
                                                 ionLastRoom,
                                                 ionPulseCooldown,
                                                 ionPulseCharge,
                                                 ionPulseEffect,
                                                 ionPulseExplosion,
                                                 ionPulsePosition,
                                                 shootGoal,
                                                 shootElapsed,
                                                 pendingDamage,
                                                 medicalRate,
                                                 healingElapsed,
                                                 mindElapsed,
                                                 stunElapsed,
                                                 constructorExplosionImage,
                                                 crewDying,
                                                 crewDead,
                                                 crewDeathElapsed,
                                                 crewDeathDone,
                                                 crewDeathLeft,
                                                 shotReady))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 crewActorId,
                                                 physicalCrewOrder,
                                                 attackTargetDroneId,
                                                 hackTime,
                                                 controllerHackLevel,
                                                 spaceExplosionElapsed,
                                                 spaceActorId,
                                                 spaceRegisteredOrder,
                                                 spacePendingArrival))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 crewPhysicalShip,
                                                 crewRoom,
                                                 crewPosition,
                                                 crewSlot,
                                                 basePosition))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 targetSpaceDroneId,
                                                 defenseTargetType,
                                                 defenseCurrentTargetId,
                                                 defenseShotTargetId,
                                                 defenseCurrentTargetIsDrone,
                                                 defenseShotTargetIsDrone,
                                                 defenseTargetVelocity,
                                                 desiredAimAngle,
                                                 defenseDisrupted,
                                                 spaceMissElapsed,
                                                 spaceMissPosition))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 repairKind,
                                                 repairTarget,
                                                 repairRoom,
                                                 heading,
                                                 oldHeading,
                                                 progressToDestination,
                                                 lastDestination,
                                                 lastAimingAngle,
                                                 hackAngle,
                                                 poweredLastFrame,
                                                 deployedLastFrame,
                                                 spaceHackSparksElapsed,
                                                 spaceHackSparksMirror,
                                                 repairWavePhases))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 hackingFlyingElapsed,
                                                 hackingExtendingElapsed,
                                                 hackingFinishedSetup,
                                                 hackingFlashElapsed,
                                                 beamCurrentTarget,
                                                 beamFinalTarget,
                                                 beamSpeed,
                                                 currentSpeed,
                                                 crewOnFire,
                                                 crewSmoke,
                                                 healthChangeRemaining,
                                                 lastPresentedHealth))
        if (!nlohmann_json_j.contains("hackingFinishedSetup") &&
            nlohmann_json_t.blueprint == "DRONE_HACKING") {
            nlohmann_json_t.hackingFinishedSetup = nlohmann_json_t.delivered;
        }
        if (!nlohmann_json_j.contains("crewDead")) {
            nlohmann_json_t.crewDead = !nlohmann_json_t.deployed || nlohmann_json_t.health <= 0;
        }
        if (nlohmann_json_t.attackTargetOwner < -1 || nlohmann_json_t.attackTargetOwner > 1 ||
            nlohmann_json_t.attackTargetIndex < -1 || nlohmann_json_t.crewBlockingDoor < -1 ||
            nlohmann_json_t.crewLastDoor < -1 || nlohmann_json_t.attackElapsed < 0 ||
            nlohmann_json_t.attackElapsed > 3 || nlohmann_json_t.attackGoal < 0 ||
            nlohmann_json_t.attackGoal >
                (nlohmann_json_t.blueprint == "BOARDER_ION" ? 1.5f : 1.3f) ||
            nlohmann_json_t.shootGoal < 0 || nlohmann_json_t.shootGoal > 1.3f ||
            nlohmann_json_t.shootElapsed < 0 || nlohmann_json_t.shootElapsed > 1.32f ||
            nlohmann_json_t.pendingDamage < 0 || nlohmann_json_t.pendingDamage > 7 ||
            nlohmann_json_t.attackFlight < 0 || nlohmann_json_t.attackFlight > 3 ||
            nlohmann_json_t.attackAnimationElapsed < 0 ||
            nlohmann_json_t.attackAnimationElapsed > 3 ||
            (!nlohmann_json_t.attackAnimation.empty() &&
             nlohmann_json_t.attackAnimation != "punch_down" &&
             nlohmann_json_t.attackAnimation != "punch_left" &&
             nlohmann_json_t.attackAnimation != "punch_right" &&
             nlohmann_json_t.attackAnimation != "punch_up" &&
             nlohmann_json_t.attackAnimation != "shoot_down" &&
             nlohmann_json_t.attackAnimation != "shoot_left" &&
             nlohmann_json_t.attackAnimation != "shoot_right" &&
             nlohmann_json_t.attackAnimation != "shoot_up") ||
            std::abs(nlohmann_json_t.attackFrom.x) > 4096 ||
            std::abs(nlohmann_json_t.attackFrom.y) > 4096 ||
            std::abs(nlohmann_json_t.attackTo.x) > 4096 ||
            std::abs(nlohmann_json_t.attackTo.y) > 4096) {
            throw std::out_of_range("invalid_drone_combat_presentation");
        }
        if (nlohmann_json_t.crewOnFire < 0 || nlohmann_json_t.crewOnFire > 256) {
            throw std::out_of_range("invalid_drone_fire_metadata");
        }
    }
};

void initializeDroneConstructor(CombatDrone& drone,
                                const BlueprintCatalog& catalog,
                                const std::string& locale,
                                RandomStream& random);
void initializeDroneLocation(CombatDrone& drone,
                             const ShipDefinition& ship,
                             int owner,
                             const BlueprintCatalog& catalog,
                             RandomStream& random);
void initializeShipExplosion(CombatExplosion& state,
                             const ShipDefinition& ship,
                             RandomStream& random);
void initializeEnemySystemStation(SystemDefinition& definition,
                                  const ShipDefinition& ship,
                                  int artilleryOrdinal,
                                  RandomStream& random);
std::vector<int> initialEnemyCrewRooms(const ShipDefinition& ship, int difficulty);

} // namespace ftl
