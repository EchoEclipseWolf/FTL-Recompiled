#pragma once
#include <array>
#include <json.hpp>
#include <map>
#include <string>
#include <vector>

namespace ftl {
void recordEarnedSkill(std::array<int, 6>& masks, int skill, int previousTier, int tier);
void incrementNativeCounter(int& counter);

struct ProgressionAchievement {
    bool unlocked = false;
    int difficulty = -1;
    std::array<int, 3> layouts{-1, -1, -1};
    bool fresh = false;
};

struct ProgressionPopup {
    std::string id;
    int family = -1;
    int layout = -1;
    int difficulty = -1;
};

// Native checkpoint state; independent of the original x86 flag-map layout.
struct ProgressionRun {
    std::string ship;
    int difficulty = 0;
    bool tutorial = false;
    bool advanced = true;
    std::map<std::string, int> flags;
    int value(const std::string& name) const;
    void flag(const std::string& name, int amount = 1);
    nlohmann::json json() const;
    static ProgressionRun read(const nlohmann::json& input);
};

// Facts sampled at the gameplay boundary. Event-specific awards use condition()
// only where their source call-site predicate has actually been satisfied.
struct ProgressionSample {
    int distinctCrewSpecies = 0;
    int installedSystems = 0;
    int usedPower = 0;
    int scrap = 0;
    int hull = 0;
    int maxHull = 0;
    int minimumBeaconHull = 0;
    int cloakedIncomingDamage = 0;
    int enemyIonedSystems = 0;
    int enginesPower = 0;
    int failedDodges = 0;
    int poweredDrones = 0;
    int enemyOxygenRoundedPercent = 100;
    int enemyRooms = 0;
    int enemyRoomsHitByBeamWithinFiveSeconds = 0;
    int maximumEnemyCrewInLockedRoom = 0;
    bool enemyHostile = false;
    bool enemyAutomated = false;
    bool everyEnemyTileBurning = false;
    bool everyEnemyRoomVisibleWithoutSensors = false;
    bool hackingPulse = false;
    bool battery = false;
    bool mindControl = false;
};

struct ProgressionCombatResult {
    int encounterSerial = 0;
    bool destroyed = false;
    bool crewKilled = false;
    int sector = 0;
    int playerWeaponShots = 0;
    int maximumWeaponVolleys = 0;
    int playerArtilleryShots = 0;
    int enemyWeaponShots = 0;
    int playerCrewOnPlayerShip = 0;
    int playerCrewOnEnemyShip = 0;
    int playerSuperShields = 0;
    bool offensiveDroneActivated = false;
    bool nonMissileWeaponFired = false;
    bool enemyDefenseDrone = false;
    bool preIgniter = false;
    bool enemyDamagedUncloaked = true;
    bool rockShip = false;
    bool playerHullUnchanged = false;
    bool playerCrewCountUnchanged = false;
    int enemyCrewKilledThisFight = 0;
};

class ProgressionLedger {
public:
    static ProgressionLedger read(const nlohmann::json& nativeProfile);
    nlohmann::json json() const;
    void merge(const ProgressionLedger& other);
    bool unlocked(int index, int type) const;
    const ProgressionAchievement* achievement(const std::string& id) const;
    static int family(const std::string& ship);
    static int layout(const std::string& ship);
    static const std::string& blueprint(int index);
    bool award(const std::string& id, const ProgressionRun& run, bool noPopup = false);
    bool unlock(int index, int type, const ProgressionRun& run, bool noPopup = false);
    void evaluate(const ProgressionSample& sample, const ProgressionRun& run);
    void sector(int zeroBasedSector, const ProgressionRun& run);
    void victory(const ProgressionRun& run);
    void lifetime(int defeatedShips, int collectedScrap, const ProgressionRun& run);
    void combat(const ProgressionCombatResult& result, ProgressionRun& run);
    void blueCrewChoice(int zeroBasedSector, ProgressionRun& run);
    void boardingDroneKill(int previousKills, bool friendlyIntruder, const ProgressionRun& run);
    void crystalShardDamage(bool enemyShip,
                            bool previouslyAlive,
                            bool destroyed,
                            bool crystalShard,
                            const ProgressionRun& run);
    void secretSectorEntered(const ProgressionRun& run);
    // Explicit source event predicates (boarding deaths, beam rooms, blue choices,
    // combat outcomes). Unknown/noncatalogue IDs are rejected, never synthesized.
    bool condition(const std::string& id, bool satisfied, const ProgressionRun& run);
    std::vector<ProgressionPopup> takePopups();
    void clearFresh();

private:
    std::array<int, 10> masks_{1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::map<std::string, ProgressionAchievement> achievements_;
    std::vector<ProgressionPopup> popups_;
    void checkShip(int index, const ProgressionRun& run, bool noPopup);
};
} // namespace ftl
