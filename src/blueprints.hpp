#pragma once
#include "native_json.hpp"
#include "resources.hpp"
#include <array>
#include <functional>
#include <json.hpp>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftl {
class RandomStream;
class BlueprintCatalog;

struct LocalizedText {
    std::string value;
    bool reference = false;
    FTL_NATIVE_JSON(LocalizedText, value, reference)
};

struct RoomDefinition {
    int id = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    FTL_NATIVE_JSON(RoomDefinition, id, x, y, w, h)
};

struct FireAnimationState {
    float elapsed = 0;
    bool smokeMirrored = false;
    FTL_NATIVE_JSON(FireAnimationState, elapsed, smokeMirrored)
};

struct DoorDefinition {
    int id = 0;
    int x = 0;
    int y = 0;
    int roomA = -1;
    int roomB = -1;
    bool vertical = false;
    FTL_NATIVE_JSON(DoorDefinition, id, x, y, roomA, roomB, vertical)
};

struct SystemDefinition {
    std::string name;
    std::string image;
    std::string weapon;
    std::string direction;
    int id = -1;
    int room = -1;
    int power = 0;
    int maxPower = -1;
    int bp = 0;
    int slot = -1;
    bool installed = false;
    std::string glowName;
    std::string glowDirection;
    int glowX = 0;
    int glowY = 0;

    friend void to_json(nlohmann::json& nlohmann_json_j, const SystemDefinition& nlohmann_json_t) {
        nlohmann_json_j = {{"bp", nlohmann_json_t.bp}};
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 name,
                                                 image,
                                                 weapon,
                                                 direction,
                                                 id,
                                                 room,
                                                 power,
                                                 maxPower,
                                                 slot,
                                                 installed,
                                                 glowName,
                                                 glowDirection,
                                                 glowX,
                                                 glowY))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j,
                          SystemDefinition& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM,
                                                 name,
                                                 image,
                                                 weapon,
                                                 direction,
                                                 id,
                                                 room,
                                                 power,
                                                 maxPower,
                                                 slot,
                                                 installed,
                                                 glowName,
                                                 glowDirection,
                                                 glowX,
                                                 glowY))
        nlohmann_json_t.bp = 0;
        if (nlohmann_json_j.contains("bp")) {
            nativeRead(nlohmann_json_j.at("bp"), nlohmann_json_t.bp);
        }
    }
};

// AddSystem49ca00 converts only an absent player slot (-1). The raw
// blueprint retains that sentinel; physical ships carry the returned values.
SystemDefinition playerSystemDefinition(SystemDefinition definition);

struct CrewDefinition {
    std::string species;
    int count = 0;
    FTL_NATIVE_JSON(CrewDefinition, species, count)
};

struct WeaponMount {
    int x = 0;
    int y = 0;
    bool rotate = false;
    bool mirror = false;
    std::string slide = "none";
    int gib = -1;
    FTL_NATIVE_JSON(WeaponMount, x, y, rotate, mirror, slide, gib)
};

struct ShipGib {
    int x = 0;
    int y = 0;
    float velocityMin = 0;
    float velocityMax = 0;
    float directionMin = 0;
    float directionMax = 0;
    float angularMin = 0;
    float angularMax = 0;
    FTL_NATIVE_JSON(
        ShipGib, x, y, velocityMin, velocityMax, directionMin, directionMax, angularMin, angularMax)
};

struct ShipDefinition {
    std::string id;
    std::string layout;
    std::string image;
    std::string weaponLoad;
    std::string droneLoad;
    std::string floorImage;
    std::string shieldImage;
    std::string cloakImage;
    std::string source;
    LocalizedText name;
    LocalizedText shipClass;
    LocalizedText description;
    LocalizedText unlock;
    int minSector = 0;
    int maxSector = std::numeric_limits<int>::max();
    int hull = 0;
    int reactor = 0;
    int weaponSlots = 0;
    int droneSlots = 0;
    int missiles = 0;
    int droneParts = 0;
    int maxCrew = 8;
    int weaponCount = 0;
    int droneCount = 0;
    int offsetX = 0;
    int offsetY = 0;
    int horizontalShift = 0;
    int verticalShift = 0;
    std::array<int, 4> imageRect{};
    std::array<int, 4> ellipse{};
    std::array<int, 2> floorOffset{};
    std::array<int, 2> cloakOffset{};
    std::vector<RoomDefinition> rooms;
    std::vector<DoorDefinition> doors;
    std::vector<SystemDefinition> systems;
    std::vector<CrewDefinition> crew;
    std::vector<WeaponMount> mounts;
    std::vector<ShipGib> gibs;
    std::vector<std::string> weapons;
    std::vector<std::string> drones;
    std::vector<std::string> augments;
    nlohmann::json snapshot() const;
    FTL_NATIVE_JSON_DEFAULT(ShipDefinition,
                            id,
                            layout,
                            image,
                            weaponLoad,
                            droneLoad,
                            floorImage,
                            shieldImage,
                            cloakImage,
                            source,
                            name,
                            shipClass,
                            description,
                            unlock,
                            minSector,
                            maxSector,
                            hull,
                            reactor,
                            weaponSlots,
                            droneSlots,
                            missiles,
                            droneParts,
                            maxCrew,
                            weaponCount,
                            droneCount,
                            offsetX,
                            offsetY,
                            horizontalShift,
                            verticalShift,
                            imageRect,
                            ellipse,
                            floorOffset,
                            cloakOffset,
                            rooms,
                            doors,
                            systems,
                            crew,
                            mounts,
                            gibs,
                            weapons,
                            drones,
                            augments)
};

struct EquipmentDefinition {
    struct MiniProjectile {
        std::string image;
        int count = 0;
        bool fake = false;
    };

    std::string id;
    std::string kind;
    std::string type;
    std::string art;
    std::string iconImage;
    std::string source;
    LocalizedText title;
    LocalizedText shortTitle;
    LocalizedText description;
    int power = 0;
    LocalizedText tooltip;
    LocalizedText tip;
    // Some archived weapon blueprints have multiple ordered <tip> children.
    // ProcessDescription0x5de180 keeps the final entry for current call sites,
    // while exposing the
    // complete archive record to presentation code and diagnostics.
    std::vector<LocalizedText> tips;
    LocalizedText flavorType;
    bool locked = false;
    bool stacking = false;
    int cost = 0;
    int rarity = 0;
    int damage = 0;
    int shots = 0;
    int missiles = 0;
    float cooldown = 0;
    float value = 0;
    int piercing = 0;
    int fireChance = 0;
    int breachChance = 0;
    int stunChance = 0;
    int ion = 0;
    int personnel = 0;
    int system = 0;
    int stun = 0;
    int length = -1;
    int chargeLevels = 1;
    int miniCount = 0;
    int radius = 0;
    int spin = 0;
    int droneTargetable = -1;
    std::vector<MiniProjectile> miniProjectiles;
    bool hullBuster = false;
    bool lockdown = false;
    int boostCount = 0;
    float boostAmount = 0;
    std::string boostType;
    std::string imagePath;
    int frameWidth = 0;
    int frameHeight = 0;
    int frameX = 0;
    int frameY = 0;
    int chargedFrame = 0;
    int fireFrame = 0;
    int animationFrames = 1;
    float weaponAnimationTime = 1;
    std::string chargeImage;
    std::string boostAnimation;
    float delayChargeAnimation = 0;
    // WeaponAnimation's registry constructor retains this concrete random
    // animation independently of a projectile blueprint's explosion.
    std::string weaponExplosionAnimation;
    int fireX = 0;
    int fireY = 0;
    int chargeOffset = 0;
    std::string projectileImage;
    std::string explosionAnimation;
    std::string droneTarget = "MISSILES";
    std::string droneWeapon;
    float droneSpeed = 0;
    int droneDodge = 0;
    std::array<int, 3> beamColor{227, 39, 39};
    std::vector<std::string> launchSounds;
    std::vector<std::string> hitShipSounds;
    std::vector<std::string> hitShieldSounds;
    std::vector<std::string> missSounds;
    float projectileSpeed = 0;
    int mountX = 0;
    int mountY = 0;
    std::string descriptionText(const TextLibrary& texts,
                                const std::string& locale,
                                bool advanced,
                                bool tooltip = false) const;
};

struct AnimationFrame {
    std::string path;
    int x = 0;
    int y = 0;
    int width = 35;
    int height = 35;
    int sheetHeight = 35;
    int frames = 1;
    float duration = 1;

    bool usable() const {
        return !path.empty() && width > 0 && height > 0 && frames > 0 && duration > 0;
    }
};

// Room467890 creates every tile's retained fire clock during layout import,
// before ship gibs and engines. Medbay61bab0 uses column-major tile clocks.
std::map<int, std::vector<float>> initializeRoomFireAnimations(const ShipDefinition& ship,
                                                               const BlueprintCatalog& catalog,
                                                               RandomStream& random);
std::vector<float> initializeMedbayAnimations(const RoomDefinition& room, RandomStream& random);
std::map<int, std::vector<FireAnimationState>>
initializeFireSpreaderAnimations(const ShipDefinition& ship, RandomStream& random);

struct CrewAppearance {
    std::string source;
    int cost = 0;
    int rarity = 0;
    LocalizedText shortTitle;
    std::vector<std::vector<std::array<float, 4>>> layers;
    LocalizedText title;
    LocalizedText description;
    std::vector<LocalizedText> powers;
};

struct AchievementDefinition {
    LocalizedText title;
    LocalizedText description;
    bool multiDifficulty = false;
};

struct SystemBlueprint {
    int cost = 0;
    int startPower = 0;
    LocalizedText title;
    LocalizedText description;
    int maxPower = 0;
    std::vector<int> costs;
};

struct ItemDefinition {
    std::string id;
    std::string source;
    int type = -1;
    int cost = 0;
    int rarity = 0;
    int bp = 0;
    LocalizedText title;
    LocalizedText shortTitle;
    LocalizedText description;
    LocalizedText tooltip;
    LocalizedText tip;
};

class BlueprintCatalog {
    const Archive& archive_;
    std::map<std::string, ShipDefinition> ships_;
    std::map<std::string, EquipmentDefinition> equipment_;
    std::map<std::string, std::vector<std::string>> lists_;
    std::map<std::string, AnimationFrame> animations_;
    std::map<std::string, std::string> weaponAnimationExplosions_;
    std::map<std::string, CrewAppearance> appearances_;
    std::map<std::string, SystemBlueprint> systems_;
    std::map<std::string, ItemDefinition> items_;
    std::map<std::string, std::vector<AchievementDefinition>> achievements_;
    std::map<std::string, std::map<std::string, bool>> crewNames_;
    std::map<std::string, std::string> crewShortNames_;
    mutable std::vector<std::string> currentCrewNames_;
    void load(const std::string& path);
    void layout(ShipDefinition& ship);

public:
    // ScoreKeeper::GetShipBlueprint0x43e4d0; live SHIP_BLUEPRINTS_5.
    static constexpr std::array<const char*, 10> families = {"PLAYER_SHIP_HARD",
                                                             "PLAYER_SHIP_STEALTH",
                                                             "PLAYER_SHIP_MANTIS",
                                                             "PLAYER_SHIP_CIRCLE",
                                                             "PLAYER_SHIP_FED",
                                                             "PLAYER_SHIP_JELLY",
                                                             "PLAYER_SHIP_ROCK",
                                                             "PLAYER_SHIP_ENERGY",
                                                             "PLAYER_SHIP_CRYSTAL",
                                                             "PLAYER_SHIP_ANAEROBIC"};
    explicit BlueprintCatalog(const Archive& archive);
    void weaponAnimationExplosions(const std::map<std::string, std::string>& registry);

    const std::map<std::string, std::string>& weaponAnimationExplosions() const {
        return weaponAnimationExplosions_;
    }

    const ShipDefinition* ship(int family, int type) const;
    const ShipDefinition& ship(const std::string& id) const;
    const ShipDefinition& selectShip(const std::string& id, int sector, RandomStream& random) const;
    const EquipmentDefinition& equipment(const std::string& id) const;

    const ItemDefinition& item(const std::string& id) const {
        // GetItemBlueprint0x5e6c20 returns a default dummy for unknown IDs.
        static const ItemDefinition dummy;
        const auto found = items_.find(id);
        return found == items_.end() ? dummy : found->second;
    }

    // Complete authored catalogs, including enemy-only and non-store entries.
    std::vector<std::string> shipIds() const;
    std::vector<std::string> crewIds() const;

    void setUsedCrewNames(const std::vector<std::string>& names) const {
        currentCrewNames_ = names;
    }

    const std::vector<std::string>& currentCrewNames() const {
        return currentCrewNames_;
    }

    std::pair<std::string, bool> unusedCrewName(const std::string& locale,
                                                const std::vector<std::string>& used,
                                                const std::function<std::uint32_t()>& random) const;
    std::string shortCrewName(const std::string& name,
                              const std::function<float(const std::string&)>& measure) const;

    bool containsEquipment(const std::string& id) const {
        return equipment_.contains(id);
    }

    const std::vector<std::string>& list(const std::string& id) const;

    const AnimationFrame& animation(const std::string& id) const {
        // GetAnimation587230 returns an inert, zero-sized Animation for an
        // unknown alias. Its (-1,-1) object position is not a strip offset.
        static const AnimationFrame empty{"", 0, 0, 0, 0, 0, 0, 0};
        const auto found = animations_.find(id);
        return found == animations_.end() ? empty : found->second;
    }

    bool hasAppearance(const std::string& species) const {
        return appearances_.contains(species);
    }

    const CrewAppearance& appearance(const std::string& species) const {
        if (!appearances_.contains(species)) {
            throw std::runtime_error("unknown_crew_appearance: " + species);
        }
        return appearances_.at(species);
    }

    const SystemBlueprint& system(const std::string& name) const {
        if (!systems_.contains(name)) {
            throw std::runtime_error("unknown_system_blueprint: " + name);
        }
        return systems_.at(name);
    }

    const std::vector<AchievementDefinition>& achievements(const std::string& ship) const {
        return achievements_.at(ship);
    }

    std::vector<std::string>
    equipmentIds(bool advanced = true,
                 const std::map<std::string, int>& sectorRarities = {}) const {
        std::vector<std::string> ids;
        for (const auto& [id, definition] : equipment_) {
            if (definition.cost > 0 && effectiveRarity(id, advanced, sectorRarities) > 0 &&
                !definition.locked) {
                ids.push_back(id);
            }
        }
        return ids;
    }

    // Original RandomItems<T> uses the full sorted blueprint map, including
    // locked and zero-cost entries, filtered only by nonzero rarity.
    // ResetRarities 0x5e72f0 suppresses the authored DLC_ITEMS list when AE
    // is off; source file names and locked flags do not define this filter.
    int effectiveRarity(const std::string& id,
                        bool advanced,
                        const std::map<std::string, int>& sectorRarities = {}) const;
    std::vector<std::pair<std::string, int>>
    rewardEquipmentPool(std::string_view kind,
                        bool advanced = true,
                        const std::map<std::string, int>& sectorRarities = {}) const;
    std::vector<std::pair<std::string, int>>
    crewRewardPool(bool advanced = true,
                   const std::map<std::string, int>& sectorRarities = {}) const;

    nlohmann::json snapshot() const;
    static int systemId(const std::string& name);
};

std::map<std::string, std::string> initializeWeaponAnimationExplosions(const Archive& archive,
                                                                       RandomStream& random);

// CrewBlueprint::RandomSkills0x5e5290 returns blueprint progress, before the
// physical actor converts the six source caps to its species-specific caps.
std::array<int, 6>
randomCrewSkills(int worldLevel, RandomStream& random, std::array<int, 6> progress = {});
} // namespace ftl
