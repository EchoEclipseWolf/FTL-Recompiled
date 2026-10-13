#include "profile.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ftl {
void recordEarnedSkill(std::array<int, 6>& masks, int skill, int previousTier, int tier) {
    if (skill < 0 || skill >= 6 || previousTier < 0 || tier > 2 || tier < previousTier) {
        throw std::runtime_error("invalid_native_earned_skill");
    }
    for (int crossed = previousTier + 1; crossed <= tier; ++crossed) {
        masks[std::size_t(skill)] |= 1 << (crossed - 1);
    }
}

void incrementNativeCounter(int& counter) {
    if (counter < std::numeric_limits<int>::max()) {
        ++counter;
    }
}

namespace {
struct Definition {
    int family;
    bool multi;
};

const std::array<std::string, 10> families{"PLAYER_SHIP_HARD",
                                           "PLAYER_SHIP_STEALTH",
                                           "PLAYER_SHIP_MANTIS",
                                           "PLAYER_SHIP_CIRCLE",
                                           "PLAYER_SHIP_FED",
                                           "PLAYER_SHIP_JELLY",
                                           "PLAYER_SHIP_ROCK",
                                           "PLAYER_SHIP_ENERGY",
                                           "PLAYER_SHIP_CRYSTAL",
                                           "PLAYER_SHIP_ANAEROBIC"};
// Catalogue order and family restrictions: achievements.xml; generated victory
// and quest records: AchievementTracker::LoadAchievementDescriptions 0x4302b0.
const std::map<std::string, Definition> definitions{
    {"ACH_SECTOR_5", {-1, true}},
    {"ACH_SECTOR_8", {-1, true}},
    {"ACH_WIN_EASY", {-1, false}},
    {"ACH_WIN_NORMAL", {-1, false}},
    {"ACH_UNLOCK_ALL", {-1, false}},
    {"ACH_SCRAP", {-1, false}},
    {"ACH_SHIPS", {-1, false}},
    {"ACH_NO_UPGRADES", {-1, true}},
    {"ACH_PACIFIST", {-1, true}},
    {"ACH_NO_REPAIR", {-1, true}},
    {"ACH_NO_MISSILES", {-1, true}},
    {"ACH_NO_DRONES", {-1, true}},
    {"ACH_NO_BUYING", {-1, true}},
    {"ACH_NO_DEATH", {-1, true}},
    {"ACH_BURNING", {-1, true}},
    {"ACH_BAD_DODGING", {-1, true}},
    {"ACH_ONE_VOLLEY", {-1, true}},
    {"ACH_BOARDING_DRONE", {-1, true}},
    {"ACH_INVADE_SHIP", {-1, true}},
    {"ACH_SLICE_DICE", {-1, true}},
    {"ACH_SUFFOCATE", {-1, true}},
    {"ACH_UNITED_FEDERATION", {0, true}},
    {"ACH_FULL_ARSENAL", {0, true}},
    {"ACH_TOUGH_SHIP", {0, true}},
    {"ACH_ENERGY_SHIELDS", {7, true}},
    {"ACH_ENERGY_POWER", {7, true}},
    {"ACH_ENERGY_MANPOWER", {7, true}},
    {"ACH_STEALTH_DESTROY", {1, true}},
    {"ACH_STEALTH_AVOID", {1, true}},
    {"ACH_STEALTH_TACTICAL", {1, true}},
    {"ACH_ROBOTIC", {3, true}},
    {"ACH_ONLY_DRONES", {3, true}},
    {"ACH_IONED", {3, true}},
    {"ACH_ROCK_FIRE", {6, true}},
    {"ACH_ROCK_MISSILES", {6, true}},
    {"ACH_ROCK_CRYSTAL", {6, true}},
    {"ACH_MANTIS_CREW_DEAD", {2, true}},
    {"ACH_MANTIS_SLAUGHTER", {2, true}},
    {"ACH_MANTIS_SURVIVOR", {2, true}},
    {"ACH_SLUG_VISION", {5, true}},
    {"ACH_SLUG_NEBULA", {5, true}},
    {"ACH_SLUG_BIO", {5, true}},
    {"ACH_FED_PATIENCE", {4, true}},
    {"ACH_FED_DIPLOMACY", {4, true}},
    {"ACH_FED_UPGRADE", {4, true}},
    {"ACH_CRYSTAL_SHARD", {8, true}},
    {"ACH_CRYSTAL_LOCKDOWN", {8, true}},
    {"ACH_CRYSTAL_CLASH", {8, true}},
    {"ACH_LANIUS_ADVANCED", {9, true}},
    {"ACH_LANIUS_SCRAP", {9, true}},
    {"ACH_LANIUS_OXYGEN", {9, true}},
    {"PLAYER_SHIP_HARD_VICTORY", {0, true}},
    {"PLAYER_SHIP_STEALTH_VICTORY", {1, true}},
    {"PLAYER_SHIP_STEALTH_QUEST", {-1, false}},
    {"PLAYER_SHIP_MANTIS_VICTORY", {2, true}},
    {"PLAYER_SHIP_MANTIS_QUEST", {-1, false}},
    {"PLAYER_SHIP_CIRCLE_VICTORY", {3, true}},
    {"PLAYER_SHIP_FED_VICTORY", {4, true}},
    {"PLAYER_SHIP_FED_QUEST", {-1, false}},
    {"PLAYER_SHIP_JELLY_VICTORY", {5, true}},
    {"PLAYER_SHIP_JELLY_QUEST", {-1, false}},
    {"PLAYER_SHIP_ROCK_VICTORY", {6, true}},
    {"PLAYER_SHIP_ROCK_QUEST", {-1, false}},
    {"PLAYER_SHIP_ENERGY_VICTORY", {7, true}},
    {"PLAYER_SHIP_ENERGY_QUEST", {-1, false}},
    {"PLAYER_SHIP_CRYSTAL_VICTORY", {8, true}},
    {"PLAYER_SHIP_CRYSTAL_QUEST", {-1, false}},
    {"PLAYER_SHIP_ANAEROBIC_VICTORY", {9, true}},
};

int integer(const nlohmann::json& value, int minimum, int maximum) {
    if (!value.is_number_integer()) {
        throw std::runtime_error("invalid_native_progression_integer");
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(maximum)) {
            throw std::runtime_error("invalid_native_progression_integer");
        }
    } else {
        const auto number = value.get<std::int64_t>();
        if (number < minimum || number > maximum) {
            throw std::runtime_error("invalid_native_progression_integer");
        }
    }
    const int number = value.get<int>();
    if (number < minimum) {
        throw std::runtime_error("invalid_native_progression_integer");
    }
    return number;
}

bool boolean(const nlohmann::json& value) {
    if (!value.is_boolean()) {
        throw std::runtime_error("invalid_native_progression_boolean");
    }
    return value.get<bool>();
}

void validateRun(const ProgressionRun& run) {
    if (ProgressionLedger::family(run.ship) < 0 || ProgressionLedger::layout(run.ship) < 0 ||
        run.difficulty < 0 || run.difficulty > 2) {
        throw std::runtime_error("invalid_native_progression_run");
    }
}
} // namespace

int ProgressionRun::value(const std::string& name) const {
    const auto found = flags.find(name);
    return found == flags.end() ? 0 : found->second;
}

void ProgressionRun::flag(const std::string& name, int amount) {
    if (tutorial) {
        return;
    }
    if (name.empty() || name.size() > 80 || amount < 0) {
        throw std::runtime_error("invalid_native_progression_flag");
    }
    auto& count = flags[name];
    count = amount > std::numeric_limits<int>::max() - count ? std::numeric_limits<int>::max()
                                                             : count + amount;
}

nlohmann::json ProgressionRun::json() const {
    validateRun(*this);
    return {{"ship", ship},
            {"difficulty", difficulty},
            {"tutorial", tutorial},
            {"advanced", advanced},
            {"flags", flags}};
}

ProgressionRun ProgressionRun::read(const nlohmann::json& input) {
    if (!input.is_object() || !input.at("ship").is_string() || !input.at("flags").is_object()) {
        throw std::runtime_error("invalid_native_progression_run");
    }
    ProgressionRun run;
    run.ship = input.at("ship").get<std::string>();
    run.difficulty = integer(input.at("difficulty"), 0, 2);
    run.tutorial = boolean(input.at("tutorial"));
    run.advanced = boolean(input.at("advanced"));
    if (input.at("flags").size() > 128) {
        throw std::runtime_error("invalid_native_progression_flags");
    }
    for (const auto& [name, value] : input.at("flags").items()) {
        if (name.empty() || name.size() > 80) {
            throw std::runtime_error("invalid_native_progression_flag");
        }
        run.flags[name] = integer(value, 0, std::numeric_limits<int>::max());
    }
    validateRun(run);
    return run;
}

int ProgressionLedger::family(const std::string& ship) {
    for (std::size_t i = 0; i < families.size(); ++i) {
        if (ship == families[i] || ship == families[i] + "_2" || ship == families[i] + "_3") {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ProgressionLedger::layout(const std::string& ship) {
    const int index = family(ship);
    if (index < 0) {
        return -1;
    }
    const auto& base = families.at(static_cast<std::size_t>(index));
    if (ship == base) {
        return 0;
    }
    const int result = ship == base + "_2" ? 1 : 2;
    return index >= 8 && result == 2 ? -1 : result;
}

const std::string& ProgressionLedger::blueprint(int index) {
    if (index < 0 || index >= 10) {
        throw std::runtime_error("invalid_native_progression_family");
    }
    return families.at(static_cast<std::size_t>(index));
}

void ProgressionLedger::merge(const ProgressionLedger& other) {
    for (std::size_t index = 0; index < masks_.size(); ++index) {
        masks_[index] |= other.masks_[index];
    }
    for (const auto& [id, incoming] : other.achievements_) {
        auto& current = achievements_[id];
        current.unlocked |= incoming.unlocked;
        current.fresh |= incoming.fresh;
        current.difficulty = std::max(current.difficulty, incoming.difficulty);
        for (std::size_t index = 0; index < current.layouts.size(); ++index) {
            current.layouts[index] = std::max(current.layouts[index], incoming.layouts[index]);
        }
    }
}

bool ProgressionLedger::unlocked(int index, int type) const {
    return index >= 0 && index < 10 && type >= 0 && type < 3 && (index < 8 || type != 2) &&
           (masks_.at(static_cast<std::size_t>(index)) & (1 << type)) != 0;
}

const ProgressionAchievement* ProgressionLedger::achievement(const std::string& id) const {
    const auto found = achievements_.find(id);
    return found == achievements_.end() ? nullptr : &found->second;
}

ProgressionLedger ProgressionLedger::read(const nlohmann::json& nativeProfile) {
    if (!nativeProfile.is_object()) {
        throw std::runtime_error("invalid_native_progression");
    }
    ProgressionLedger result;
    if (!nativeProfile.contains("progression")) {
        return result;
    }
    const auto& input = nativeProfile.at("progression");
    if (!input.is_object() || integer(input.at("version"), 1, 1) != 1 ||
        !input.at("masks").is_array() || input.at("masks").size() != 10 ||
        !input.at("achievements").is_object()) {
        throw std::runtime_error("invalid_native_progression");
    }
    for (std::size_t i = 0; i < 10; ++i) {
        result.masks_[i] = integer(input.at("masks").at(i), 0, i >= 8 ? 3 : 7);
        if ((result.masks_[i] & 6) != 0 && (result.masks_[i] & 1) == 0) {
            throw std::runtime_error("invalid_native_progression_mask");
        }
    }
    if (!result.unlocked(0, 0)) {
        throw std::runtime_error("invalid_native_progression_mask");
    }
    for (const auto& [id, row] : input.at("achievements").items()) {
        if (!definitions.contains(id) || !row.is_object()) {
            throw std::runtime_error("invalid_native_progression_achievement");
        }
        ProgressionAchievement item;
        item.unlocked = boolean(row.at("unlocked"));
        item.difficulty = integer(row.at("difficulty"), -1, 2);
        item.fresh = boolean(row.at("fresh"));
        if (!row.at("layouts").is_array() || row.at("layouts").size() != 3) {
            throw std::runtime_error("invalid_native_progression_layouts");
        }
        for (std::size_t i = 0; i < 3; ++i) {
            item.layouts[i] = integer(row.at("layouts").at(i), -1, 2);
        }
        if (item.unlocked != (item.difficulty >= 0) || (!item.unlocked && item.fresh)) {
            throw std::runtime_error("invalid_native_progression_achievement");
        }
        const auto& definition = definitions.at(id);
        const bool victory = id.ends_with("_VICTORY");
        if (!victory && item.layouts != std::array<int, 3>{-1, -1, -1}) {
            throw std::runtime_error("invalid_native_progression_layouts");
        }
        if (victory && ((definition.family >= 8 && item.layouts[2] != -1) ||
                        item.unlocked != (std::ranges::max(item.layouts) >= 0))) {
            throw std::runtime_error("invalid_native_progression_layouts");
        }
        result.achievements_[id] = item;
    }
    return result;
}

nlohmann::json ProgressionLedger::json() const {
    nlohmann::json records = nlohmann::json::object();
    for (const auto& [id, item] : achievements_) {
        records[id] = {{"unlocked", item.unlocked},
                       {"difficulty", item.difficulty},
                       {"layouts", item.layouts},
                       {"fresh", item.fresh}};
    }
    return {{"version", 1}, {"masks", masks_}, {"achievements", records}};
}

bool ProgressionLedger::award(const std::string& id, const ProgressionRun& run, bool noPopup) {
    validateRun(run);
    const auto found = definitions.find(id);
    if (found == definitions.end()) {
        throw std::runtime_error("unknown_native_achievement");
    }
    if (run.tutorial || (found->second.family >= 0 && found->second.family != family(run.ship))) {
        return false;
    }
    const bool victory = id.ends_with("_VICTORY");
    const int type = layout(run.ship);
    auto& item = achievements_[id];
    const bool improved =
        !item.unlocked || (found->second.multi && run.difficulty > item.difficulty) ||
        (victory && run.difficulty > item.layouts.at(static_cast<std::size_t>(type)));
    if (!improved) {
        return false;
    }
    item.unlocked = true;
    // SetAchievement0x42fbc0 stores this award's difficulty even when a
    // newly completed victory layout is easier than an earlier layout.
    item.difficulty = run.difficulty;
    item.fresh = true;
    if (victory) {
        item.layouts.at(static_cast<std::size_t>(type)) = run.difficulty;
    }
    if (!noPopup) {
        popups_.push_back({id, found->second.family, victory ? type : -1, run.difficulty});
    }
    if (found->second.family >= 0) {
        checkShip(found->second.family, run, noPopup);
    }
    return true;
}

bool ProgressionLedger::unlock(int index, int type, const ProgressionRun& run, bool noPopup) {
    validateRun(run);
    if (index < 0 || index >= 10 || type < 0 || type >= 3 || (index >= 8 && type == 2)) {
        throw std::runtime_error("invalid_native_progression_unlock");
    }
    if (run.tutorial || unlocked(index, type)) {
        return false;
    }
    if (type > 0 && !unlocked(index, 0)) {
        return false;
    }
    masks_.at(static_cast<std::size_t>(index)) |= 1 << type;
    const int count = static_cast<int>(std::ranges::count_if(masks_, [](int mask) {
        return (mask & 1) != 0;
    }));
    if (std::ranges::all_of(masks_, [](int mask) {
            return (mask & 1) != 0;
        })) {
        award("ACH_UNLOCK_ALL", run, noPopup);
    }
    // UnlockShip443590 checks the all-family achievement and recursively
    // unlocks Lanius before queuing the triggering ship's own popup.
    if (count == 5 && index != 9) {
        unlock(9, 0, run, noPopup);
    }
    if (!noPopup) {
        popups_.push_back({"", index, type, run.difficulty});
    }
    return true;
}

void ProgressionLedger::checkShip(int index, const ProgressionRun& run, bool noPopup) {
    int count = 0;
    for (const auto& [id, definition] : definitions) {
        if (definition.family == index && !id.ends_with("_VICTORY") && !id.ends_with("_QUEST")) {
            const auto* item = achievement(id);
            if (item && item->unlocked) {
                ++count;
            }
        }
    }
    if (count >= 2) {
        unlock(index, 1, run, noPopup);
    }
    const auto* won = achievement(blueprint(index) + "_VICTORY");
    // CheckShipAchievements 0x42f360 exact alternate-victory unlock chain.
    constexpr std::array<int, 10> next{-1, -1, 5, 4, 7, 6, 1, 2, -1, -1};
    if (won && won->unlocked && next.at(static_cast<std::size_t>(index)) >= 0) {
        unlock(next.at(static_cast<std::size_t>(index)), 0, run, noPopup);
    }
}

void ProgressionLedger::evaluate(const ProgressionSample& sample, const ProgressionRun& run) {
    // Source call sites: CrewMemberFactory0x45e090, ShipManager0x4a05b0,
    // GetDodged0x492060. Family gating belongs to SetAchievement0x42fbc0.
    condition("ACH_UNITED_FEDERATION", sample.distinctCrewSpecies > 5, run);
    condition("ACH_FULL_ARSENAL", sample.installedSystems > 10, run);
    condition("ACH_ENERGY_POWER", sample.usedPower > 28, run);
    condition("ACH_LANIUS_SCRAP", sample.scrap > 599, run);
    condition("ACH_TOUGH_SHIP",
              sample.hull > 0 && sample.hull == sample.maxHull && sample.minimumBeaconHull == 1,
              run);
    condition("ACH_STEALTH_AVOID", sample.cloakedIncomingDamage > 8, run);
    condition("ACH_IONED", sample.enemyIonedSystems > 3, run);
    condition("ACH_BAD_DODGING", sample.enginesPower > 7 && sample.failedDodges > 4, run);
    condition(
        "ACH_LANIUS_ADVANCED", sample.hackingPulse && sample.battery && sample.mindControl, run);
    condition("ACH_ROBOTIC", sample.poweredDrones > 2, run);
    condition("ACH_SUFFOCATE",
              sample.enemyHostile && !sample.enemyAutomated &&
                  sample.enemyOxygenRoundedPercent <= 4,
              run);
    condition("ACH_BURNING", sample.enemyRooms > 0 && sample.everyEnemyTileBurning, run);
    condition("ACH_SLUG_VISION",
              sample.enemyRooms > 0 && sample.everyEnemyRoomVisibleWithoutSensors,
              run);
    condition("ACH_SLICE_DICE",
              sample.enemyRooms > 0 &&
                  sample.enemyRoomsHitByBeamWithinFiveSeconds == sample.enemyRooms,
              run);
    condition("ACH_CRYSTAL_LOCKDOWN", sample.maximumEnemyCrewInLockedRoom > 3, run);
}

void ProgressionLedger::sector(int sector, const ProgressionRun& run) {
    validateRun(run);
    if (sector < 0 || sector > 7) {
        throw std::runtime_error("invalid_native_progression_sector");
    }
    if (run.tutorial || sector <= 3) {
        return;
    }
    award("ACH_SECTOR_5", run);
    const bool knownHistory = !run.value("challenge_history_unavailable");
    if (knownHistory) {
        if (!run.value("store_repair")) {
            award("ACH_NO_REPAIR", run);
        }
        if (!run.value("system_upgrade")) {
            award("ACH_NO_UPGRADES", run);
        }
        if (!run.value("teleported") && !run.value("offensive_drone") && !run.value("fired_shot")) {
            award("ACH_PACIFIST", run);
        }
        if (!run.value("reactor_upgrade")) {
            award("ACH_ENERGY_MANPOWER", run);
        }
        if (!run.value("weapon_upgrade")) {
            award("ACH_FED_UPGRADE", run);
        }
    }
    if (sector <= 6) {
        return;
    }
    award("ACH_SECTOR_8", run);
    for (const auto& [flag, id] : std::array<std::pair<const char*, const char*>, 6>{
             {{"higho2", "ACH_LANIUS_OXYGEN"},
              {"used_missile", "ACH_NO_MISSILES"},
              {"used_drone", "ACH_NO_DRONES"},
              {"store_purchase", "ACH_NO_BUYING"},
              {"lost_crew", "ACH_NO_DEATH"},
              {"env_danger", "ACH_STEALTH_TACTICAL"}}}) {
        if (knownHistory && !run.value(flag)) {
            award(id, run);
        }
    }
    const int index = family(run.ship);
    if (run.advanced && index < 8 && layout(run.ship) == 1) {
        unlock(index, 2, run);
    }
}

void ProgressionLedger::victory(const ProgressionRun& run) {
    validateRun(run);
    if (run.tutorial) {
        return;
    }
    award("ACH_WIN_EASY", run);
    if (run.difficulty > 0) {
        award("ACH_WIN_NORMAL", run);
    }
    award(blueprint(family(run.ship)) + "_VICTORY", run);
    // BossShip::OnLoop0x407510 calls ScoreKeeper::CheckCrystalUnlock0x443830
    // after the family victory. CheckCrystalUnlock0x42e520 requires both A/B
    // victory markers for the eight non-Crystal, non-Lanius families.
    bool crystalEligible = true;
    for (int index = 0; index < 8; ++index) {
        const auto* won = achievement(blueprint(index) + "_VICTORY");
        if (!won || won->layouts[0] < 0 || won->layouts[1] < 0) {
            crystalEligible = false;
            break;
        }
    }
    if (crystalEligible) {
        unlock(8, 0, run);
    }
}

void ProgressionLedger::lifetime(int ships, int scrap, const ProgressionRun& run) {
    if (ships < 0 || scrap < 0) {
        throw std::runtime_error("invalid_native_progression_lifetime");
    }
    condition("ACH_SHIPS", ships > 999, run);
    condition("ACH_SCRAP", scrap > 9999, run);
}

void ProgressionLedger::combat(const ProgressionCombatResult& result, ProgressionRun& run) {
    validateRun(run);
    if (run.tutorial) {
        return;
    }
    if (result.encounterSerial <= 0) {
        throw std::runtime_error("invalid_native_progression_encounter");
    }
    if (result.encounterSerial <= run.value("last_combat_result")) {
        return;
    }
    if (!result.destroyed && !result.crewKilled) {
        return;
    }
    run.flags["last_combat_result"] = result.encounterSerial;
    // WorldManager::OnLoop0x5f6520, at real enemy outcome (not surrender).
    if (result.crewKilled) {
        run.flag("dead_crew");
        condition("ACH_MANTIS_CREW_DEAD", run.value("dead_crew") > 19 && result.sector <= 5, run);
        condition("ACH_MANTIS_SURVIVOR",
                  result.playerCrewOnPlayerShip <= 0 && result.playerCrewOnEnemyShip == 1,
                  run);
    }
    if (result.destroyed) {
        condition("ACH_ONE_VOLLEY",
                  result.maximumWeaponVolleys < 2 && result.enemyWeaponShots <= 0 &&
                      result.preIgniter && result.playerArtilleryShots <= 1,
                  run);
        condition("ACH_FED_PATIENCE",
                  !result.offensiveDroneActivated && result.playerWeaponShots == 0 &&
                      result.playerArtilleryShots > 0,
                  run);
        condition("ACH_ONLY_DRONES",
                  result.playerWeaponShots == 0 && result.playerArtilleryShots == 0,
                  run);
        condition("ACH_STEALTH_DESTROY", !result.enemyDamagedUncloaked, run);
        condition("ACH_ENERGY_SHIELDS", result.playerSuperShields > 0, run);
        condition("ACH_ROCK_MISSILES",
                  !result.nonMissileWeaponFired && !result.offensiveDroneActivated &&
                      result.enemyDefenseDrone,
                  run);
        if (result.rockShip) {
            run.flag("destroyed_rock");
            condition("ACH_CRYSTAL_CLASH", run.value("destroyed_rock") > 9, run);
        }
    }
    if (result.destroyed || result.crewKilled) {
        condition("ACH_MANTIS_SLAUGHTER",
                  result.playerHullUnchanged && result.playerCrewCountUnchanged &&
                      result.enemyCrewKilledThisFight > 4,
                  run);
    }
}

void ProgressionLedger::blueCrewChoice(int sector, ProgressionRun& run) {
    run.flag("blue_alien");
    // WorldManager::PauseLoop0x5f6070: fourth race-specific blue choice,
    // source worldLevel<=4, separate from generic equipment blue choices.
    condition("ACH_FED_DIPLOMACY", run.value("blue_alien") > 3 && sector <= 4, run);
}

void ProgressionLedger::boardingDroneKill(int previousKills,
                                          bool friendlyIntruder,
                                          const ProgressionRun& run) {
    // CrewMember::OnLoop0x4747f0 checks prior targetsDestroyed>3 before increment.
    condition("ACH_BOARDING_DRONE", friendlyIntruder && previousKills > 3, run);
}

void ProgressionLedger::crystalShardDamage(bool enemyShip,
                                           bool previouslyAlive,
                                           bool destroyed,
                                           bool crystalShard,
                                           const ProgressionRun& run) {
    // ShipManager::DamageArea0x499fb0 requires a real living-to-dead hull transition
    // carrying the source crystalShard collision flag, not merely a weapon name.
    condition("ACH_CRYSTAL_SHARD", enemyShip && previouslyAlive && destroyed && crystalShard, run);
}

void ProgressionLedger::secretSectorEntered(const ProgressionRun& run) {
    // StarMap::StartSecretSector0x5c7a90, never crew recruitment.
    award("ACH_ROCK_CRYSTAL", run);
}

bool ProgressionLedger::condition(const std::string& id,
                                  bool satisfied,
                                  const ProgressionRun& run) {
    return satisfied && award(id, run);
}

std::vector<ProgressionPopup> ProgressionLedger::takePopups() {
    return std::exchange(popups_, {});
}

void ProgressionLedger::clearFresh() {
    for (auto& [id, item] : achievements_) {
        (void)id;
        item.fresh = false;
    }
}
} // namespace ftl
