#include "run.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ftl {
nlohmann::json Run::runStatistics() const {
    return {{"ships", scoreShips_},
            {"beacons", scoreBeacons_},
            {"scrap", scoreScrap_},
            {"crew_hired", progressionRun_.value("crew_hired")}};
}

nlohmann::json Run::crewRecords() const {
    nlohmann::json records = nlohmann::json::object();
    if (tutorialOwned_) {
        return records;
    }
    constexpr std::array<const char*, 5> keys{
        "repairs", "combat_kills", "piloted_evasions", "jumps_survived", "earned_skill_tiers"};
    std::array<int, 5> best{};
    for (const auto& member : crew_) {
        int tiers = 0;
        for (const int mask : member.earnedSkillTiers) {
            tiers += std::popcount(static_cast<unsigned>(mask));
        }
        for (std::size_t index = 0; index < keys.size(); ++index) {
            const int value = index < 4 ? member.statistics[index] : tiers;
            if (value <= best[index]) {
                continue;
            }
            best[index] = value;
            records[keys[index]] = {{"name", member.appearance.name},
                                    {"species", member.appearance.species},
                                    {"male", member.recordMale},
                                    {"appearance", member.appearance.appearance},
                                    {"value", value}};
        }
    }
    return records;
}

void Run::updateProgression() {
    if (!open_ || tutorialOwned_) {
        return;
    }
    progressionRun_.tutorial = tutorialOwned_;
    ProgressionSample sample;
    std::set<std::string> species;
    for (const auto& member : crew_) {
        if (member.health > 0) {
            species.insert(member.appearance.species);
        }
    }
    sample.distinctCrewSpecies = static_cast<int>(species.size());
    sample.installedSystems = static_cast<int>(systems_.size());
    for (const auto& sys : systems_) {
        const int id = sys.definition.id;
        if ((id < 6 || id > 8) && id != 12) {
            sample.usedPower += std::clamp(sys.power, 0, sys.health);
        }
    }
    sample.scrap = scrap_;
    sample.hull = hull_;
    sample.maxHull = ship_.hull;
    auto& minimum = progressionRun_.flags["minimum_beacon_hull"];
    if (minimum > 0) {
        minimum = std::min(minimum, hull_);
    }
    sample.minimumBeaconHull = minimum;
    if (!cloak_.active) {
        progressionRun_.flags["cloaked_incoming_damage"] = 0;
    }
    sample.cloakedIncomingDamage = progressionRun_.value("cloaked_incoming_damage");
    sample.failedDodges = progressionRun_.value("failed_dodges");
    if (const auto* engines = system(1)) {
        sample.enginesPower = std::clamp(engines->power, 0, engines->health);
    }
    sample.hackingPulse = hacking_.active;
    sample.battery = battery_.active;
    sample.mindControl = mind_.active;
    sample.poweredDrones =
        static_cast<int>(std::ranges::count_if(combatDrones_, [](const auto& drone) {
            return drone.powered && drone.deployed;
        }));
    if (graph_.current() != 0 && oxygen_ > 20 && !progressionRun_.value("higho2")) {
        progressionRun_.flag("higho2");
    }
    if (environment_.fireActive || environment_.asteroidActive || environment_.pulsarActive ||
        environment_.stormActive) {
        if (!progressionRun_.value("env_danger")) {
            progressionRun_.flag("env_danger");
        }
    }
    if (enemy_.present && hostile_) {
        if (enemy_.hull < enemy_.ship.hull && !cloak_.active) {
            progressionRun_.flags["combat_enemy_damaged_uncloaked"] = 1;
        }
        sample.enemyHostile = true;
        // UpdateEnvironment 0x497f50 reads the live bAutomated.
        sample.enemyAutomated = enemyAutomated();
        float oxygen = 0;
        for (const auto& [id, value] : enemy_.roomOxygen) {
            (void)id;
            oxygen += value;
        }
        if (!enemy_.roomOxygen.empty()) {
            sample.enemyOxygenRoundedPercent =
                static_cast<int>(std::round(oxygen / static_cast<float>(enemy_.roomOxygen.size())));
        }
        sample.enemyRooms = static_cast<int>(enemy_.ship.rooms.size());
        sample.everyEnemyTileBurning = !enemy_.ship.rooms.empty();
        sample.everyEnemyRoomVisibleWithoutSensors =
            (nebula_ || subsystemLevel(7) <= 1) && !enemy_.ship.rooms.empty();
        for (const auto& room : enemy_.ship.rooms) {
            const auto hazard = enemyHazards_.find(room.id);
            sample.everyEnemyTileBurning &=
                hazard != enemyHazards_.end() && hazard->second.fires == room.w * room.h / 1225;
            sample.everyEnemyRoomVisibleWithoutSensors &=
                std::ranges::any_of(crew_, [&](const auto& member) {
                    return member.health > 0 && member.boardRoom == room.id;
                });
            const auto beam = beamHitAt_[1].find(room.id);
            if (beam != beamHitAt_[1].end() && Time::seconds(double(tick_)) - beam->second < 5) {
                ++sample.enemyRoomsHitByBeamWithinFiveSeconds;
            }
            if (enemy_.lockdownRooms.contains(room.id)) {
                const int count =
                    static_cast<int>(std::ranges::count_if(enemy_.crew, [&](const auto& member) {
                        return member.health > 0 && member.room == room.id;
                    }));
                sample.maximumEnemyCrewInLockedRoom =
                    std::max(sample.maximumEnemyCrewInLockedRoom, count);
            }
        }
        sample.enemyIonedSystems =
            static_cast<int>(std::ranges::count_if(enemy_.systems, [](const auto& sys) {
                return sys.ion > 0;
            }));
    }
    progression_.evaluate(sample, progressionRun_);
    const auto total = [](int base, int current) {
        return static_cast<int>(std::clamp<long long>(
            static_cast<long long>(base) + current, 0, std::numeric_limits<int>::max()));
    };
    progression_.lifetime(total(lifetimeShipsBase_, scoreShips_),
                          total(lifetimeScrapBase_, scoreScrap_),
                          progressionRun_);
}

void Run::failScrapPurchase() {
    // Original purchase failures raise EventSystem event6 and powerUpFail.
    scrapFailureAnimation_ = animation_ + 1;
    combatSound({"powerUpFail"}, animation_);
}

std::vector<std::size_t> Run::livingCrew() const {
    std::vector<std::size_t> ids;
    for (std::size_t i = 0; i < crew_.size(); ++i) {
        if (crew_[i].health > 0) {
            ids.push_back(i);
        }
    }
    return ids;
}

std::vector<std::size_t> Run::crewRoster() const {
    // FactoryGetCrewPortraitList45d8d0 invokes CountForVictory720df0;
    // clonable dead actors retain their portrait and capacity slot.
    std::vector<std::size_t> ids;
    for (std::size_t i = 0; i < crew_.size(); ++i) {
        const auto& member = crew_[i];
        if ((!member.dead && !member.dying) || member.cloneReady) {
            ids.push_back(i);
        }
    }
    return ids;
}

std::array<int, 4> Run::resourceAmounts() const {
    return {scrap_, fuel_, missiles_, parts_};
}

void Run::noteResourceChanges(const std::array<int, 4>& previous) {
    const auto current = resourceAmounts();
    for (std::size_t index = 0; index < current.size(); ++index) {
        const int delta = current[index] - previous[index];
        if (delta == 0) {
            continue;
        }
        resourceDeltas_[index] = delta;
        resourceDeltaTicks_[index] = animation_;
    }
}

void Run::resetResourceFeedback() {
    resourceFeedback_.clear();
    observedResources_ = resourceAmounts();
    pendingSavedMissileFeedback_ = false;
}

void Run::updateResourceFeedback() {
    // ShipStatus::OnRender 0x53f1b0 compares displayed resource snapshots.
    // Detect during UI updates instead, keeping drawing pure and capturing
    // mutations whose callers do not explicitly report resource changes.
    std::erase_if(resourceFeedback_, [&](const auto& message) {
        return animation_ - message.start >= 60;
    });
    const auto current = resourceAmounts();
    for (const std::size_t index : {1U, 2U, 3U, 0U}) {
        const int delta = current[index] - observedResources_[index];
        if (delta != 0 || (index == 2 && pendingSavedMissileFeedback_)) {
            // ShipStatus::RenderResources 0x53f1b0 builds a DamageMessage 0x59d080,
            // whose x jitter is random32() % 21 - 10 from the one gameplay stream
            // (resourceFeedbackRandom_ is bound to it). The comparison state is
            // therefore part of the checkpoint.
            resourceFeedback_.push_back(
                {animation_, index, delta, int(resourceFeedbackRandom_.next() % 21) - 10});
        }
    }
    observedResources_ = current;
    pendingSavedMissileFeedback_ = false;
}

RunSystem* Run::system(int id) {
    auto it = std::find_if(systems_.begin(), systems_.end(), [&](const auto& s) {
        return s.definition.id == id;
    });
    return it == systems_.end() ? nullptr : &*it;
}

const RoomDefinition& Run::room(int id) const {
    auto it = std::find_if(ship_.rooms.begin(), ship_.rooms.end(), [&](const auto& r) {
        return r.id == id;
    });
    if (it == ship_.rooms.end()) {
        throw std::runtime_error("invalid_room");
    }
    return *it;
}

void Run::start(const Hangar& hangar,
                const std::string& locale,
                std::uint32_t seed,
                bool preserveSession) {
    if (!preserveSession) {
        sessionRandom_ = RandomStream(seed);
    }
    profileRunId_ = 0;
    profileRunStarted_ = true;
    tutorialOwned_ = false;
    tutorial_.stop();
    weaponSlots_.clear();
    droneSlots_.clear();
    voyageMelee_.clear();
    voyageDoorBaseHealth_.clear();
    voyageDoorLevel_.clear();
    voyageDoorLastBaseHealth_.clear();
    voyageDoorLockdownBroken_.clear();
    voyageDoorDamage_.clear();
    voyageDoorForcedTicks_.clear();
    eventBackground_.clear();
    eventPlanet_.clear();
    eventPlanetX_ = eventPlanetY_ = eventPlanetRotation_ = 0;

    sector_ = 1;
    campaign_ = {};
    secretSector_ = false;
    pendingSecretSector_ = false;
    sectorType_ = panelPage_ = distressWaits_ = 0;
    distress_ = false;
    fuelEventSeed_ = 0;
    sectorMapSeed_ = -1;
    scoreCrew_ = 0;
    waitedLast_ = false;
    lastEscapeEvent_.clear();
    mapRevealed_ = false;
    addedQuests_.clear();
    generatedEvent_.clear();
    choiceHistory_.clear();
    lastMainTextLiteral_ = true;
    lastMainText_.clear();
    encounterAutoBlueprint_.clear();
    distressWaitTicks_ = 0;
    scrapFailureAnimation_ = 0;
    inventoryDragKind_.clear();
    inventoryDragIndex_ = -1;
    panel_.clear();
    pausedShipPanel_.clear();
    dismissCrew_ = renameCrew_ = inventorySelection_ = pendingStoreOffer_ = pausedDismissCrew_ = -1;
    storeSelling_ = false;
    crewRenameText_.clear();
    crewRenameCaret_ = 0;
    crewRenameBlink_ = 0;
    pendingQuests_.clear();
    usedVoyageEvents_.clear();
    selectedVoyageCrewSeed_ = -1;
    pendingVoyageEffects_ = nlohmann::json::array();
    voyageBoarders_.clear();
    overflowEquipment_.clear();
    overflowFactory_ = {};
    overflowAugment_.clear();
    setEnvironment({});
    asteroidScroll_ = {};
    upgradePurchases_.clear();
    voyageMessage_.clear();
    cargo_.clear();
    cargoFactories_.clear();
    stores_.clear();
    restoredEvents_.clear();
    reactorPowerCap_ = 64;
    reactorPowerLoss_ = 0;
    reactorPowerDivide_ = 1;
    resetAdvanced();
    cloneQueue_.clear();
    clonePending_ = -1;
    playerIntruderCount_ = 0;
    boostFtl_ = false;
    cloneDeathTimer_ = -1;
    cloneDuration_ = 12;
    cloneTimer_ = -1;
    enemy_ = {};
    playerHazards_.clear();
    enemyHazards_.clear();
    projectiles_.clear();
    pendingRetaliation_.clear();
    beamHitAt_ = {};
    beamKilledCrew_ = {};
    shieldHits_.clear();
    resistMessages_.clear();
    damageMessages_.clear();
    systemExplosions_ = {};
    lastHullDamage_ = {};
    combatWeapons_.clear();
    cloak_ = {};
    battery_ = {};
    // WorldManager::Restart 0x5f49f0 -> PowerManager::RestartAll 0x620230 and the new
    // ships' ShipManager::OnInit 0x4a6e00 -> PowerManager::Restart 0x61ff30 zero the lend.
    batteryCapacity_ = {0, 0};
    // The new ShipManagers start their trackers over: ctor 0x495af0 clears
    // lastEngineStatus/lastJumpReady/iLastDamage, OnInit 0x4a6e00 builds fresh
    // extinguisher emitters, Ship::OnInit fresh rooms (Room ctor 0x467890: lastO2
    // 100, the current level of a new ship), fires and OuterHull strips.
    lastEngineStatus_ = lastJumpReady_ = false;
    fireOxygen_ = {};
    roomLastOxygen_ = {};
    fireExtinguishers_ = {};
    slugGelHealElapsed_ = {};
    doorHitTick_ = {};
    storeOpenPending_ = false;
    combatDrones_.clear();
    nextProjectile_ = 1;
    nextDamageMessageOrder_ = 1;
    aiming_ = -1;
    autofire_ = surrenderOffered_ = surrenderDialog_ = defeated_ = false;
    forceAutofire_ = false;
    abilityAim_.clear();
    explosion_ = 0;
    explosionDuration_ = 3.f;
    enemyExplosionActive_ = false;
    playerExplosionStartTick_ = 0;
    playerExplosionStarted_ = false;
    combatSounds_ = nlohmann::json::array();
    shieldWarningTime_ = 0;
    hudWarnings_ = {};
    hudLastHull_ = ship_.hull;
    hullWarningLevel_ = 75;
    ftlBlink_ = previousFtlBlink_ = pauseBlink_ = previousPauseBlink_ = 0;
    ftlBlinkDirection_ = pauseBlinkDirection_ = 1;
    ftlPullout_ = previousFtlPullout_ = 0;
    gameOverCommand_ = -1;
    scoreScrap_ = scoreShips_ = 0;
    scoreBeacons_ = 1;
    mapNebulas_.clear();
    choices_ = {{"Continue...", {}, true}};
    pendingResources_ = {};
    eventId_ = "START_GAME";
    openingEvent_ = true;
    mapOpen_ = hostile_ = false;
    nebula_ = false;
    oxygenLeakSound_ = false;
    hazard_.clear();
    devUiOpen_ = false;
    devUiTab_ = 0;
    devUiFocus_.clear();
    devUiStatus_.clear();
    devUiFields_.clear();
    devCrewLimit_ = 8;
    devReactorLimit_ = 25;
    ftlCharge_ = 0;
    previousCrew_.clear();
    previousDoors_.clear();
    previousJump_ = 0;
    presentationAlpha_ = 1;
    jumpTicks_ = 0;
    arrivalTicks_ = 0;
    jumpDestination_ = -1;
    encounter_.clear();
    encounterAutoBlueprint_.clear();
    encounterShipSeed_ = 0;
    ship_ = hangar.selected();
    for (auto& definition : ship_.systems) {
        definition = playerSystemDefinition(std::move(definition));
    }
    // CreateArmory4a6520 equips only the authored count and requires its system.
    const auto installed = [&](int id) {
        return std::ranges::any_of(ship_.systems, [id](const auto& station) {
            return station.id == id && station.installed;
        });
    };
    ship_.weapons.resize(installed(3) ? std::min(ship_.weapons.size(),
                                                 static_cast<std::size_t>(std::clamp(
                                                     ship_.weaponCount, 0, ship_.weaponSlots + 1)))
                                      : 0);
    ship_.drones.resize(
        installed(4)
            ? std::min(ship_.drones.size(),
                       static_cast<std::size_t>(std::clamp(ship_.droneCount, 0, ship_.droneSlots)))
            : 0);
    name_ = hangar.shipName();
    locale_ = locale;
    // World::CreateNewGame inherits the live Hangar factory's name history
    // before eagerly constructing the sector's event/crew blueprints.
    crewNameHistory_ = hangar.crewNameHistory();
    catalog_.setUsedCrewNames(crewNameHistory_);
    difficulty_ = hangar.difficulty();
    advanced_ = hangar.advanced();
    progressionRun_ = ProgressionRun{ship_.id, difficulty_, false, advanced_, {}};
    progressionRun_.flags["minimum_beacon_hull"] = ship_.hull;
    // StarMap::NewGame5d5a20 draws the sector-tree seed from the session
    // stream, then reseeds before GenerateSectorMap. ShipBuilder::Open does
    // not reseed; detached diagnostics retain their explicit initial seed.
    sectorMapSeed_ = eventRandom_.next();
    eventRandom_ = RandomStream(static_cast<std::uint32_t>(sectorMapSeed_));
    initializeSectorTree();
    graph_.generate(eventRandom_.next(), eventRandom_);
    populateSector();
    locationBeacon_ = graph_.current();
    prepareOpeningText();
    prepareBeaconImages({});
    hull_ = ship_.hull;
    scrap_ = difficulty_ == 0 ? 30 : difficulty_ == 1 ? 10 : 0;
    fuel_ = 16;
    missiles_ = ship_.missiles;
    parts_ = ship_.droneParts;
    resourceDeltas_ = {};
    resourceDeltaTicks_ = {};
    resetResourceFeedback();
    systems_.clear();
    crew_.clear();
    playerCrewAi_ = {};
    crewDeathCount_ = 0;
    nextCrewDroneId_ = 1;
    nextPhysicalCrewOrder_ = 1;
    nextSpaceDroneId_ = 1;
    nextSpaceDroneOrder_ = 1;
    spaceShipOwners_.clear();
    retiredCrewDrones_.clear();
    retiredSpaceDrones_.clear();
    doors_.assign(ship_.doors.size(), false);
    lockdownRooms_.clear();
    weaponPower_.assign(ship_.weapons.size(), false);
    weaponDesired_.assign(ship_.weapons.size(), false);
    combatWeapons_ = hangar.weaponFactories();
    combatDrones_ = hangar.droneFactories();
    hackingDrone_ = hangar.hackingFactory();
    assignSpaceDroneId(hackingDrone_);
    for (auto& drone : combatDrones_) {
        assignSpaceDroneId(drone);
        assignCrewDroneId(drone);
        const auto& type = catalog_.equipment(drone.blueprint).type;
        if (type == "REPAIR" || type == "BATTLE") {
            assignPhysicalCrewOrder(drone);
        }
    }
    for (auto drone : hangar.droneTrash()) {
        assignSpaceDroneId(drone);
        assignCrewDroneId(drone);
        const auto& type = catalog_.equipment(drone.blueprint).type;
        if (type == "REPAIR" || type == "BATTLE") {
            assignPhysicalCrewOrder(drone);
        }
        retireCrewDrone(drone);
    }
    if (combatWeapons_.size() != ship_.weapons.size() ||
        combatDrones_.size() != ship_.drones.size()) {
        throw std::runtime_error("hangar_armory_transfer_mismatch");
    }
    int available = ship_.reactor;
    std::size_t artillery = 0;
    // ShipSystem::StartingPowerUp 0x6146d0 and ctor0x61bab0, in blueprint creation order.
    for (const auto& s : ship_.systems) {
        if (s.installed) {
            bool passive = (s.id >= 6 && s.id <= 8) || s.id == 12;
            int required = s.id == 3 || s.id == 4 ? 0 : s.id == 0 ? 2 : 1;
            int p = passive ? s.power : std::min({required, s.power, available});
            if (!passive) {
                available -= p;
            }
            systems_.push_back({s, p, s.power, 0});
            if (s.id == 5) {
                systems_.back().damagingEffectTimes = hangar.medbayAnimationTimes().at(s.room);
            }
            if (s.id == 11 && !s.weapon.empty()) {
                systems_.back().artillery = hangar.artilleryFactories().at(artillery++);
            }
        }
    }
    for (const auto& c : hangar.crew()) {
        const auto& rm = room(c.room);
        RunCrew unit;
        unit.appearance = c;
        unit.shootGoal = c.shootGoal;
        unit.shootElapsed = c.shootElapsed;
        unit.health = crewMaximumHealth(c.species);
        unit.recordMale = c.species != "human" || (c.appearance & 1) == 0;
        unit.room = unit.targetRoom = unit.savedRoom = c.room;
        unit.taskRoom = c.room;
        unit.slot = unit.targetSlot = unit.savedSlot = c.slot;
        unit.x = float(rm.x + (c.slot % (rm.w / 35)) * 35) + 17.f;
        unit.y = float(rm.y + (c.slot / (rm.w / 35)) * 35) + 17.f;
        assignPhysicalCrewOrder(unit);
        crew_.push_back(unit);
    }
    const auto interiorBody = [this](const CombatDrone& drone) {
        const auto& type = catalog_.equipment(drone.blueprint).type;
        return type == "REPAIR" || type == "BATTLE";
    };
    playerCrewAi_.startingCrewCount =
        static_cast<int>(crew_.size() + std::ranges::count_if(combatDrones_, interiorBody) +
                         std::ranges::count_if(retiredCrewDrones_, interiorBody));
    // WorldManager::CreateNewGame 0x5f47e8 calls AddCrew once per CountCrew(false).
    scoreCrew_ = static_cast<int>(crew_.size());
    refreshCrewNames();
    playerExplosionState_ = hangar.explosionState();
    playerEngineAnimationTimes_ = hangar.engineAnimationTimes();
    playerRoomFireAnimationTimes_ = hangar.roomFireAnimationTimes();
    playerFireSpreaderAnimations_ = hangar.fireSpreaderAnimations();
    registerSpaceDrones(0, true);
    armamentWarnings_ = {};
    updateManning();
    resetEventChoiceDelay();
    roomOxygen_.clear();
    for (const auto& rm : ship_.rooms) {
        roomOxygen_[rm.id] = system(2) ? 100.f : 0.f;
    }
    doorFrames_.assign(doors_.size(), 0);
    doorTimers_.assign(doors_.size(), 0);
    fakeDoors_.assign(doors_.size(), false);
    shieldBubbles_ = system(0) ? system(0)->power / 2 : 0;
    superShields_ = std::find(ship_.augments.begin(), ship_.augments.end(), "ENERGY_SHIELD") !=
                            ship_.augments.end()
                        ? 5
                        : 0;
    shieldCharge_ = 0;
    shift_ = dragging_ = false;
    message_.clear();
    hoverTick_ = 0;
    doorCursorTime_ = 0;
    doorCursorDelay_ = 0;
    doorCursorReverse_ = false;
    scrapFailureAnimation_ = 0;
    open_ = event_ = true;
    userPause_ = false;
    tick_ = animation_ = 0;
    oxygen_ = system(2) ? 100.f : 0.f;
    mouseX_ = mouseY_ = -1;
    eventChoiceHoverValid_ = false;
    hudHoverSoundTarget_.clear();
    audioWeaponIds_.clear();
    audioWeaponReady_.clear();
    audioIncomingFire_ = false;
    mapHoverSoundBeacon_ = -1;
    pointerValid_ = pointerLastValid_ = pointerNewHover_ = false;
    applyStartLocation();
    startSectorMusic();
    audio_.active(true);
}

void Run::applyStartLocation() {
    // WorldManager::CreateNewGame 0x5f46b0 runs CreateLocation 0x5f2be0 on the
    // start location: ModifyEnvironment, then CheckStatusEffects 0x5eea10, with
    // the start event that GenerateNebulas 0x5d17e0 may have put in a nebula.
    pugi::xml_document document;
    if (!document.load_string(graph_.beacons()[graph_.current()].eventXml.c_str())) {
        throw std::runtime_error("invalid_prepared_start_event");
    }
    const auto event = document.document_element();
    restoreBeaconEnvironment(event);
    for (const auto status : event.children("status")) {
        nlohmann::json row{{"kind", "status"}};
        for (const auto attribute : status.attributes()) {
            row[attribute.name()] = attribute.value();
        }
        pendingVoyageEffects_.push_back(std::move(row));
    }
    (void)commitVoyageEffects();
}

void Run::prepareOpeningText() {
    // StarMap::NewGame 0x5d5a20, right after GenerateMap: GetBaseEvent
    // ("START_GAME") runs CreateEvent 0x5b8940's draws, then the ordinary PC
    // tip comes from TIPS_LIST_PC. Both precede WorldManager::CreateLocation
    // 0x5f2be0, which draws the start images.
    (void)materializeEvent(events_.event("START_GAME"), eventRandom_, advanced_, usedVoyageEvents_);
    pugi::xml_document tipDocument;
    auto tip = tipDocument.append_child("text");
    tip.append_attribute("load").set_value("TIPS_LIST_PC");
    eventText_ = texts_.get("event_START_GAME_text", locale_) + " \n \n" +
                 events_.text(tip, eventRandom_, texts_, locale_);
    // NewGame appends the tip with TextString::operator+=, making the text literal.
    lastMainTextLiteral_ = true;
    lastMainText_ = eventText_;
}

nlohmann::json Run::state() const {
    const auto equipmentExtra =
        overflowEquipment_.empty() ? Rect{} : inventoryOverflowRect(false, true);
    const auto augmentExtra = overflowAugment_.empty() ? Rect{} : inventoryOverflowRect(true, true);
    nlohmann::json crew = nlohmann::json::array();
    nlohmann::json systems = nlohmann::json::array();
    for (std::size_t i = 0; i < crew_.size(); ++i) {
        auto& c = crew_[i];
        crew.push_back({{"id", i},
                        {"name", c.appearance.name},
                        {"full_name", c.appearance.fullName},
                        {"species", c.appearance.species},
                        {"position", {c.x, c.y}},
                        {"room", c.room},
                        {"slot", c.slot},
                        {"target_room", c.targetRoom},
                        {"target_slot", c.targetSlot},
                        {"health", c.health},
                        {"stun_time", c.stunTime},
                        {"controlled", c.controlled},
                        {"weapon_skill", c.weaponSkill},
                        {"weapon_skill_progress", c.weaponSkillProgress},
                        {"combat_skill", c.combatSkill},
                        {"combat_skill_progress", c.combatSkillProgress},
                        {"combat_skill_cap", combatSkillCap(c.appearance.species)},
                        {"combat_damage_multiplier", combatSkillDamageMultiplier(c.combatSkill)},
                        {"repair_skill", c.repairSkill},
                        {"repair_skill_progress", c.repairSkillProgress},
                        {"pilot_skill", c.manningSkill[0]},
                        {"pilot_skill_progress", c.manningSkillProgress[0]},
                        {"engine_skill", c.manningSkill[1]},
                        {"engine_skill_progress", c.manningSkillProgress[1]},
                        {"shield_skill", c.manningSkill[2]},
                        {"shield_skill_progress", c.manningSkillProgress[2]},
                        {"selected", c.selected},
                        {"saved_room", c.savedRoom},
                        {"saved_slot", c.savedSlot},
                        {"path_remaining", c.path.size()},
                        {"facing", c.facing}});
    }
    for (auto& s : systems_) {
        systems.push_back(
            {{"id", s.definition.id},
             {"room", s.definition.room},
             {"station_slot", s.definition.slot},
             {"power", s.power},
             {"battery_power", s.batteryPower},
             {"bonus_power", s.bonusPower},
             {"crew_power", crewPowerForSystem(s, false).first},
             {"crew_power_permanent", crewPowerForSystem(s, false).second},
             {"capacity", s.definition.power},
             {"health", s.health},
             {"ion", s.ion},
             {"ion_timer", s.ionTimer},
             {"hack_level", s.hackLevel},
             {"hack_active", s.hackActive},
             {"manned", s.operatorActive},
             {"manning_level", s.manningLevel},
             {"occupied", s.occupied},
             {"friendlies", s.friendlies},
             {"on_fire", s.onFire},
             {"effective_level",
              s.definition.id == 7 || s.definition.id == 8 ? subsystemLevel(s.definition.id)
                                                           : s.power},
             {"repair", s.repair},
             {"fire_damage", s.fireDamage}});
    }
    auto choices = nlohmann::json::array();
    for (std::size_t index = 0; index < choices_.size(); ++index) {
        const auto preview = choiceResources(index);
        choices.push_back({{"id", index},
                           {"resources",
                            {{"scrap", preview.scrap},
                             {"fuel", preview.fuel},
                             {"missiles", preview.missiles},
                             {"drones", preview.drones}}},
                           {"text", choices_[index].text},
                           {"enabled", choices_[index].enabled},
                           {"blue_choice", choices_[index].blueChoice},
                           {"hidden_reward", choices_[index].hiddenReward},
                           {"lines", choiceLines(index)},
                           {"row_height", choiceHeight(index)}});
    }
    auto feedback = nlohmann::json::array();
    constexpr std::array<SDL_FPoint, 4> feedbackPositions{
        {{465, 70}, {180, 104}, {247, 104}, {314, 104}}};
    for (const auto& message : resourceFeedback_) {
        const float age = float(animation_ - message.start) + uiFraction_;
        feedback.push_back(
            {{"resource", message.resource},
             {"delta", message.delta},
             {"age", age / 60.f},
             {"duration", 1.f},
             {"jitter_x", message.jitter},
             {"digit_x", feedbackPositions[message.resource].x + message.jitter - 32.f},
             {"digit_y", feedbackPositions[message.resource].y - 46.f + age / 6.f},
             {"alpha", 255},
             {"visible", age < 60.f}});
    }
    return {
        {"campaign", campaign_},
        {"voyage",
         {{"sector", sector_},
          {"secret_sector", secretSector_},
          {"pending_secret_sector", pendingSecretSector_},
          {"sector_type", sectorType_},
          {"panel", panel_},
          {"pause_confirmation", panel_.starts_with("pause_confirm:") ? panel_.substr(14) : ""},
          {"pause_menu_rect",
           [&]() -> nlohmann::json {
               if (panel_ != "pause" && !panel_.starts_with("pause_confirm:")) {
                   return nullptr;
               }
               const auto rect = pauseMenuRect();
               return nlohmann::json::array({rect.x, rect.y, rect.w, rect.h});
           }()},
          {"cargo", cargo_},
          {"distress", distress_},
          {"distress_waits", distressWaits_},
          {"stores", stores_},
          {"store_selling", storeSelling_},
          {"pending_store_offer", pendingStoreOffer_},
          {"door_damage", voyageDoorDamage_},
          {"door_base_health", voyageDoorBaseHealth_},
          {"forced_open_ticks", voyageDoorForcedTicks_},
          {"melee", voyageMelee_},
          {"upgrade_cart", upgradePurchases_},
          {"dismiss_crew", dismissCrew_},
          {"rename_crew", renameCrew_},
          {"rename_text", crewRenameText_},
          {"paused_ship_panel", pausedShipPanel_},
          {"event_font_size", eventFontSize_},
          {"sector_name", sectorName_},
          {"sector_choices", sectorChoices_},
          {"pending_quests", pendingQuests_},
          {"sector_graph", sectorGraph_.nodes()},
          {"overflow", overflowEquipment_},
          {"overflow_augment", overflowAugment_},
          {"overflow_equipment_rect",
           nlohmann::json::array(
               {equipmentExtra.x, equipmentExtra.y, equipmentExtra.w, equipmentExtra.h})},
          {"overflow_augment_rect",
           nlohmann::json::array({augmentExtra.x, augmentExtra.y, augmentExtra.w, augmentExtra.h})},
          {"pending_effects", pendingVoyageEffects_},
          {"boarders", voyageBoarders_},
          {"environment", environment_},
          {"asteroid_scroll", asteroidScroll_},
          {"wait_ticks", distressWaitTicks_},
          {"message", voyageMessage_}}},
        {"advanced",
         {{"hacking_target", hackingTarget_},
          {"hacking_active", hacking_.active},
          {"hacking_cooldown", hacking_.cooldown},
          {"mind_target", mindTarget_},
          {"mind_target_owner", mindTargetOwner_},
          {"mind_active", mind_.active},
          {"mind_cooldown", mind_.cooldown},
          {"mind_queue", mindQueue_[0]},
          {"hacking_drone", hackingDrone_},
          {"hacking_drone_space", hackingDroneSpace_},
          {"hacking_drone_id", hackingDroneId_},
          {"clone_queue", cloneQueue_},
          {"clone_death_timer", cloneDeathTimer_},
          {"clone_duration", cloneDuration_},
          {"clone_timer", cloneTimer_}}},
        {"scene", "active_ship"},
        {"resource_feedback", resourceDeltas_},
        {"resource_feedback_messages", feedback},
        {"resource_feedback_observed", observedResources_},
        {"combat", combatState()},
        {"map", graph_.state()},
        {"generation_trace", generationTrace_},
        {"map_open", mapOpen_},
        {"ftl_charge", {ftlCharge_, 85}},
        {"ftl_ready", ftlCharge_ >= 85 && canFly()},
        {"jump_ticks_remaining", jumpTicks_},
        {"presentation",
         {{"jump_seconds", travelPresentationSeconds()}, {"alpha", presentationAlpha_}}},
        {"arrival_ticks_remaining", arrivalTicks_},
        {"jump_destination", jumpDestination_},
        {"event_id", eventId_},
        {"event_choices", choices},
        {"event_reward_entries", eventRewardEntries()},
        {"event_reward_height", eventRewardHeight()},
        {"event_resources",
         {{"scrap", pendingResources_.scrap},
          {"fuel", pendingResources_.fuel},
          {"missiles", pendingResources_.missiles},
          {"drones", pendingResources_.drones},
          {"pursuit", pendingResources_.pursuit}}},
        {"hostile", hostile_},
        {"nebula", nebula_},
        {"hazard", hazard_},
        {"dev_limits", {{"crew", devCrewLimit_}, {"reactor", devReactorLimit_}}},
        {"developer_ui", devUiState()},
        {"tutorial", tutorialState()},
        {"nebula_clouds", nebula_ ? nebulaScene_.state() : nlohmann::json::array()},
        {"encounter", encounter_},
        {"encounter_ship_seed", encounterShipSeed_},
        {"main_menu_open", false},
        {"hangar_open", false},
        {"active_ship_view", !event_ && !mapOpen_ && panel_.empty() && jumpTicks_ == 0},
        {"gameplay_available", true},
        {"modal",
         event_ && arrivalTicks_ == 0 && jumpTicks_ == 0
             ? nlohmann::json(openingEvent_ ? "opening_event" : "beacon_event")
         : mapOpen_ ? nlohmann::json("map")
                    : nlohmann::json(nullptr)},
        {"event_open", event_ && arrivalTicks_ == 0 && jumpTicks_ == 0},
        {"event_staged", event_ && (arrivalTicks_ > 0 || jumpTicks_ > 0)},
        {"event_text", eventText_},
        {"user_pause", userPause_},
        {"modal_pause", event_ || mapOpen_ || !panel_.empty()},
        {"simulation_tick", tick_},
        {"crew_walk_ticks", {crewWalkTick("walk_down", false), crewWalkTick("walk_down", true)}},
        {"animation_tick", animation_},
        {"ship", ship_.id},
        {"ship_name", name_},
        {"equipped_weapons", ship_.weapons},
        {"equipped_drones", ship_.drones},
        {"augments", ship_.augments},
        {"weapon_slots", ship_.weaponSlots},
        {"drone_slots", ship_.droneSlots},
        {"difficulty", difficulty_},
        {"advanced_edition", advanced_},
        {"hull", {hull_, ship_.hull}},
        {"scrap", scrap_},
        {"fuel", fuel_},
        {"missiles", missiles_},
        {"drone_parts", parts_},
        {"crew", crew},
        {"systems", systems},
        {"doors", doors_},
        {"lockdown_rooms", lockdownRooms_},
        {"logical_size", {1280, 720}},
        {"oxygen", oxygen_},
        {"room_oxygen", roomOxygen_},
        {"reactor", ship_.reactor},
        {"available_power", availablePower()},
        {"evasion", evasion()},
        {"shield_bubbles", shieldBubbles_},
        {"super_shield_bubbles", superShields_},
        {"weapon_power", weaponPower_},
        {"weapon_desired", weaponDesired_},
        {"door_frames", doorFrames_}};
}
} // namespace ftl
