#pragma once
#include "blueprints.hpp"
#include "native_json.hpp"
#include "render.hpp"
#include <array>
#include <functional>

namespace ftl {
struct CombatExplosion;
struct CombatWeapon;

struct HangarCrew {
    std::string name;
    std::string species;
    int appearance = 0;
    int room = -1;
    int slot = 0;
    std::string fullName;
    float shootGoal = 0;
    float shootElapsed = 0;

    friend void to_json(nlohmann::json& j, const HangarCrew& member) {
        j = {{"name", member.name},
             {"species", member.species},
             {"appearance", member.appearance},
             {"room", member.room},
             {"slot", member.slot},
             {"fullName", member.fullName},
             {"shootGoal", member.shootGoal},
             {"shootElapsed", member.shootElapsed}};
    }

    friend void from_json(const nlohmann::json& j, HangarCrew& member) {
        nativeRead(j.at("name"), member.name);
        nativeRead(j.at("species"), member.species);
        nativeRead(j.at("appearance"), member.appearance);
        nativeRead(j.at("room"), member.room);
        nativeRead(j.at("slot"), member.slot);
        nativeRead(j.at("fullName"), member.fullName);
        member.shootGoal = j.contains("shootGoal") ? nativeValue<float>(j.at("shootGoal")) : 0.f;
        member.shootElapsed =
            j.contains("shootElapsed") ? nativeValue<float>(j.at("shootElapsed")) : 0.f;
        if ((member.shootGoal != 0 && (member.shootGoal < 1 || member.shootGoal > 1.3f)) ||
            member.shootElapsed < 0 || member.shootElapsed > 1.32f) {
            throw std::out_of_range("invalid_hangar_crew_shoot_timer");
        }
    }
};

struct ShipCrewRenderActor {
    std::uint64_t physicalOrder = 0;
    std::function<void()> body;
    std::function<void()> health;
};

struct ShipPaintState {
    std::uint64_t tick = 0;
    const std::vector<CombatWeapon>* weaponStates = nullptr;
    const CombatExplosion* explosion = nullptr;
    std::map<int, float> roomOxygen;
    std::map<int, int> computerLevels;
    std::map<int, int> systemHealth;
    // ShipSystem::healthState.second: the current (upgraded) system level.
    std::map<int, int> systemCapacity;
    // ShipSystem iLockCount/iHackEffect inputs to the room icon colour.
    std::map<int, bool> systemLocked;
    std::map<int, int> systemHackEffect;
    // ShipSystem::flashTracker current_time (0.75 s loop).
    float systemFlash = 0;
    // Strongly hacked medbay effects are per-tile, in column-major order.
    std::map<int, std::vector<float>> damagingEffectTimes;
    // Logical mounted slot -> dense weapon index; empty for hangar/legacy fixtures.
    std::vector<int> weaponSlotIndices;
    std::vector<SDL_FPoint> crewPositions;
    std::vector<std::string> crewAnimations;
    std::vector<std::uint64_t> crewTicks;
    std::vector<float> crewHealth;
    std::vector<float> crewHealthAlpha;
    std::vector<std::uint64_t> crewPhysicalOrders;
    std::vector<std::function<void()>> crewHealthRenderers;
    // Drawn under each body at its 35px cell corner (the status-4 smoke).
    std::vector<std::function<void(float, float)>> crewUnderlays;
    std::vector<ShipCrewRenderActor> visitingCrew;
    std::vector<SDL_FPoint> targets;
    std::vector<float> doorFrames;
    // Doors whose gotHit tracker is running (Door::OnRender 0x466bb0 red tint).
    std::vector<bool> doorHit;
    // Per door: Door::doorLevel (Ship::SetBlastDoors 0x485790 from the door
    // system's effective power, 0 when absent or broken; iHacked+2 while
    // hacked) and Door::iHacked. Empty or -1 falls back to doorLevel/doorsHacked.
    std::vector<int> doorLevels;
    std::vector<int> doorHacks;
    std::vector<int> visionRooms;
    std::vector<int> highlightRooms;
    std::vector<int> weaponFrames;
    std::vector<int> artilleryFrames;
    std::vector<const CombatWeapon*> artilleryStates;
    std::vector<bool> selected;
    std::vector<bool> hostileCrew;
    std::vector<bool> stunnedCrew;
    // ShipManager::OnRender: crew in blacked-out rooms seen through slug crew
    // or LIFE_SCANNER, drawn by CrewAnimation::OnRender's outline-only pass.
    std::vector<bool> silhouetteCrew;
    std::vector<bool> doors;
    std::vector<bool> crewHealthFlash;
    float x = 350;
    float y = 170;
    float deployment = 0;
    float jumpProgress = -1;
    // -1 uses the blueprint level for hangar previews; live ships supply effective manning.
    int doorLevel = -1;
    int hoverRoom = -1;
    int hoverDoor = -1;
    int hoverSystem = -1;
    float destructionAge = -1.f;
    std::array<float, 2> engineAnimationTimes{};
    bool rooms = true;
    bool cloaked = false;
    bool hostile = false;
    bool containsPlayerCrew = false;
    // Live physical lists interleave ordinary crew and retained drone bodies.
    // Hangar previews retain the existing blueprint-only drawing path.
    bool orderedPhysicalCrew = false;
    Uint8 crewHighlightAlpha = 0;
    bool doorsHacked = false;
    bool enginesOn = true;
    bool sensorsWorking = true;
    // Physical rooms revealed by ProvidesVision actors when sensors cannot see.
    bool explicitVision = false;
    bool active = false;
    bool colorblind = false;
};

class ShipPainter {
    Renderer& renderer_;
    const Archive& archive_;
    const BlueprintCatalog& catalog_;

public:
    void drawDestructionGibs(const ShipDefinition&, const ShipPaintState&, float x, float y);
    void drawMountedWeapon(const EquipmentDefinition&,
                           const CombatWeapon*,
                           int frame,
                           const WeaponMount&,
                           SDL_FPoint base,
                           float deployment,
                           bool cloaked,
                           float rotation = 0,
                           const SDL_FPoint* rotationCenter = nullptr);

    ShipPainter(Renderer& r, const Archive& a, const BlueprintCatalog& c)
        : renderer_(r), archive_(a), catalog_(c) {}

    void draw(const ShipDefinition&,
              const std::vector<HangarCrew>&,
              const ShipPaintState&,
              const std::function<void()>& floorEffects = {},
              const std::function<void()>& wallEffects = {},
              const std::function<void()>& crewBodies = {},
              const std::function<void()>& crewHealth = {});
    void drawDestructionExplosions(const ShipDefinition&, const ShipPaintState&);
    void crewSprite(const HangarCrew&,
                    float x,
                    float y,
                    float scale,
                    bool selected = false,
                    const std::string& animation = "walk_down",
                    std::uint64_t tick = 0,
                    bool hostile = false,
                    bool colorblind = false,
                    bool stunned = false,
                    bool outline = true,
                    bool scaledPortrait = true,
                    bool silhouette = false);
};
} // namespace ftl
