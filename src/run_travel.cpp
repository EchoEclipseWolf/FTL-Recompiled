#include "run.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ftl {
bool Run::canFly() const {
    const auto* engines = system(1);
    const auto* pilot = system(6);
    return !enemy_.rewardCrewPending && engines && engines->power > 0 && engines->health > 0 &&
           (!engines->hackActive || engines->hackLevel <= 1) && pilot && pilot->power > 0 &&
           pilot->health > 0 && (!pilot->hackActive || pilot->hackLevel <= 1) && hasCrewManning(6);
}

bool Run::canOpenMap() const {
    return (!tutorialOwned_ || mapOpen_ || tutorial_.current().name == "DAMAGE_SYSTEM_7") &&
           panel_.empty() && !event_ && jumpTicks_ == 0 && arrivalTicks_ == 0 &&
           (fuel_ > 0 || !(enemy_.present && hostile_)) && canFly() && ftlCharge_ >= 85.f;
}

Rect Run::nextSectorButtonRect() const {
    // StarMap5cc560: local514,12, size226x36. TextButton575690 keeps
    // the right edge fixed while accommodating localized text and margin4.
    const auto label = texts_.get("button_nextsector", locale_);
    const float width = std::max(
        226.f, std::round(renderer_.measure(localizedFont(63, label), label, textScale(63))) + 8.f);
    return {1080 - width, 95, width, 36};
}

void Run::openMap() {
    if (canOpenMap() && (!tutorialOwned_ || tutorial_.current().name == "DAMAGE_SYSTEM_7")) {
        mapOpen_ = true;
    }
}

int Run::hoveredBeacon() const {
    for (std::size_t index = 0; index < graph_.beacons().size(); ++index) {
        const auto& beacon = graph_.beacons()[index];
        const float dx = mouseX_ - (385.f + beacon.x);
        const float dy = mouseY_ - (123.f + beacon.y);
        if (dx * dx + dy * dy < 100) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void Run::convertFleetLocation(int index) {
    const auto& beacon = graph_.beacons().at(static_cast<std::size_t>(index));
    // TurnIntoFleetLocation5d0940 creates the replacement event at capture,
    // including locations which will never be visited, in location order.
    std::string name = beacon.exit && !campaign_.active ? "FLEET_EASY_BEACON"
                       : beacon.nebula                  ? "FLEET_EASY_NEBULA"
                                                        : "FLEET_EASY";
    if ((beacon.exit && !campaign_.active && difficulty_ != 0) ||
        ((!beacon.exit || campaign_.active) && !beacon.nebula && advanced_)) {
        name += "_DLC";
    }
    auto xml = materializeEvent(events_.event(name),
                                eventRandom_,
                                advanced_,
                                usedVoyageEvents_,
                                [this](pugi::xml_node node, RandomStream& random) {
                                    freezeVoyageEvent(node, random);
                                });
    if (beacon.exit && !campaign_.active && beacon.nebula) {
        pugi::xml_document document;
        if (!document.load_string(xml.c_str())) {
            throw std::runtime_error("invalid_prepared_fleet_event");
        }
        auto event = document.document_element();
        event.remove_child("environment");
        event.append_child("environment").append_attribute("type").set_value("nebula");
        std::ostringstream serialized;
        document.save(serialized, "", pugi::format_raw);
        xml = serialized.str();
    }
    graph_.setEvent(index, name, beacon.nebula, beacon.exit);
    graph_.setEventXml(index, std::move(xml));
    graph_.setSavedShip(index, nullptr);
    graph_.setStore(index, false);
    graph_.setDistress(index, false);
}

void Run::jump(int destination) {
    if (!mapOpen_ || !canOpenMap() || fuel_ <= 0 || !graph_.reachable(destination)) {
        return;
    }
    const auto& beacon = graph_.beacons()[destination];
    if (beacon.event.empty()) {
        throw std::runtime_error("beacon_event_unassigned");
    }
    // Validate the definition before committing fuel/graph mutations.
    events_.event(beacon.event);
    revisiting_ = beacon.visited > 0;
    progressionRun_.tutorial = tutorialOwned_;
    progressionRun_.flags["minimum_beacon_hull"] = hull_;
    if (beacon.nebula) {
        progressionRun_.flag("nebula");
        progression_.condition(
            "ACH_SLUG_NEBULA", progressionRun_.value("nebula") > 29, progressionRun_);
    }
    for (auto& member : crew_) {
        if (member.health > 0) {
            incrementNativeCounter(member.statistics[3]);
            if (member.appearance.species == "crystal") {
                member.crystalPowerElapsed = 50;
            }
        }
    }
    // ShipManager::CloneHealing 0x490000 uses GetMaxPower 0x615cc0
    // (health/cap/loss-adjusted capacity), even when unpowered. A completely
    // destroyed bay supplies no healing.
    if (const auto* bay = system(13); bay && bay->health > 0) {
        constexpr std::array<float, 4> cloneHealth{0.f, 8.f, 16.f, 25.f};
        const float heal = cloneHealth[std::size_t(std::clamp(systemCapacity(*bay), 0, 3))];
        for (auto& member : crew_) {
            if (member.health > 0 && !member.intruder) {
                member.health =
                    std::min(member.health + heal,
                             crewMaximumHealth(member.appearance.species) + member.healthBoost);
            }
        }
    }
    releaseEnemyMind();
    rememberBoss();
    saveBeaconShip();
    if (!graph_.visit(destination, !campaign_.active, [this](int index) {
            convertFleetLocation(index);
        })) {
        return;
    }
    // StarMap::GetNewLocation 0x5d1329 clears waitedLast.
    waitedLast_ = false;
    if (campaign_.active) {
        campaignTurn();
    }
    const auto previousResources = resourceAmounts();
    --fuel_;
    retireCrewDrone(overflowFactory_.drone);
    overflowEquipment_.clear();
    overflowFactory_ = {};
    overflowAugment_.clear();
    // SpaceManager survives departure; ClearLocation runs when jump completes.
    for (auto& sys : systems_) {
        sys.powerLimit = 8;
        sys.powerLoss = 0;
        sys.powerDivide = 1;
    }
    reactorPowerCap_ = 64;
    reactorPowerLoss_ = 0;
    reactorPowerDivide_ = 1;
    // ClearLocation 0x5ef130 calls KillEveryone(noClone=true) on the
    // departing enemy ship. A party still aboard is lost, not recalled.
    for (std::size_t i = 0; i < crew_.size(); ++i) {
        auto& member = crew_[i];
        if (!member.intruder) {
            continue;
        }
        if (member.health > 0) {
            member.health = 0;
            crewDeath(member);
        }
        std::erase(cloneQueue_, int(i));
        member.controlled = false;
        member.healthBoost = 0;
    }
    // ShipManager::JumpLeave 0x491b70 -> MindSystem::Jump 0x60ae20: ReleaseCrew
    // 0x609d40 (mindControlEnd, MIND_CONTROL_LOCK), then LockSystem(0).
    if (auto* mind = system(14)) {
        releasePlayerMind();
        setSystemLock(*mind, 0);
    }
    resetAdvanced();
    stopAbility(10);
    stopAbility(12);
    reconcilePower();
    // BatterySystem::Jump 0x608940 clears its lock after ending an active
    // cycle. A jump restores immediate availability even during cooldown.
    if (auto* source = system(12)) {
        setSystemLock(*source, 0);
    }
    // DroneSystem::Jump 0x611ae0 recalls space drones. Recover only bodies
    // that still exist; repeated jumps must never refund a second part.
    const bool recoverDrones =
        std::find(ship_.augments.begin(), ship_.augments.end(), "DRONE_RECOVERY") !=
        ship_.augments.end();
    for (auto& drone : combatDrones_) {
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        if (blueprint.type != "COMBAT" && blueprint.type != "DEFENSE" &&
            blueprint.type != "SHIP_REPAIR" && blueprint.type != "SHIELD" &&
            blueprint.type != "BOARDER") {
            continue;
        }
        // CanBeRecovered excludes boarder pods; Destroyed (bDead) bodies
        // are not refunded.
        if (drone.deployed && drone.health > 0 && recoverDrones && blueprint.type != "BOARDER") {
            ++parts_;
        }
        // DePowerDrone(userDriven=false) removes only the drone's raw bars;
        // the controller and the drone keep their bonus credit.
        if (drone.powered) {
            if (auto* drones = system(4)) {
                (void)depowerDrone(drone, *drones, 0);
            }
        }
        reindexDroneReferences(0, static_cast<int>(&drone - combatDrones_.data()));
        resetCrewDroneDeployment(drone);
    }
    // Shields::Jump 0x60e130 restores ENERGY_SHIELD's super layer.
    superShields_ = std::find(ship_.augments.begin(), ship_.augments.end(), "ENERGY_SHIELD") !=
                            ship_.augments.end()
                        ? 5
                        : 0;
    if (!revisiting_) {
        addScore(scoreBeacons_, 1);
    }
    jumpDestination_ = destination;
    previousJump_ = 0;
    jumpTicks_ = 120; // ShipManager ctor0x495af0: jumpAnimation.time = 2 seconds.
    // ShipManager::Jump 0x491b70 starts jumpLeave with the fade animation.
    audio_.sound("jumpLeave");
    // Leaving the map clears its retained user pause. Keeping it blocks the
    // jump timer and prevents the prepared destination event from opening.
    userPause_ = false;
    mapOpen_ = false;
    noteResourceChanges(previousResources);
    openingEvent_ = false;
    aiming_ = -1;
    const bool preignited =
        std::find(ship_.augments.begin(), ship_.augments.end(), "WEAPON_PREIGNITE") !=
        ship_.augments.end();
    for (std::size_t index = 0; index < combatWeapons_.size(); ++index) {
        auto& weapon = combatWeapons_[index];
        weapon.clearTargets();
        weapon.boostLevel = 0;
        weapon.goalChargeLevel = 0;
        if (!preignited) {
            // WeaponSystem::Jump 0x60f7e0 clears ordinary weapon charge.
            weapon.charge = 0;
            weapon.chargeLevel = 0;
        } else if (weaponPower_[index]) {
            // ForceCoolup 0x40bec0 charges a powered weapon. Charge weapons
            // gain their first stored level and restart the next charge bar.
            const auto& definition = catalog_.equipment(ship_.weapons[index]);
            if (weapon.cooldown <= 0) {
                weapon.cooldown = definition.cooldown;
            }
            weapon.chargeLevel = 1;
            weapon.charge = definition.chargeLevels > 1 ? 0.f : weapon.cooldown;
        }
    }
    for (auto& system : systems_) {
        if (system.definition.id == 11) {
            system.artillery.clearTargets();
            // ArtillerySystem::Jump 0x60ecb0 zeroes cooldown.first and
            // chargeLevel; a stored level would fire at the next enemy.
            system.artillery.charge = 0;
            system.artillery.chargeLevel = 0;
        }
    }
}

void Run::advanceTravel() {
    // lastJumpReady is the previous OnLoop's result, so the cue also fires
    // when WorldManager's SetSafe 0x4903c0 filled the timer after that pass.
    const bool wasReady = lastJumpReady_;
    // Manager recharge precedes the authoritative World SetSafe/boost tail.
    if (fuel_ == 0) {
        // ShipManager::OnLoop 0x4a05b0 permits the waiting map immediately
        // when no fuel remains, independently of ordinary recharge progress.
        ftlCharge_ = 85.f;
    } else if (canFly()) {
        const auto* engines = system(1);
        float manning = 1.f;
        if (engines->health == engines->definition.power) {
            constexpr std::array<float, 4> bonuses{1.f, 1.1f, 1.17f, 1.25f};
            manning = bonuses[std::size_t(engines->manningLevel)];
        }
        float booster = 1.f;
        for (const auto& augment : ship_.augments) {
            if (augment == "FTL_BOOSTER") {
                booster += catalog_.equipment(augment).value;
            }
        }
        // GetEngineSpeed 0x618d00 includes engine skill, FTL_BOOSTER and the
        // peaceful 3x boost, then caps at 1.2 original factor units per tick.
        const float rate = std::min(19.2f,
                                    (1.25f + .35f * (engines->power - 1)) * manning * booster *
                                        (boostFtl_ ? 3.f : 1.f));
        ftlCharge_ = std::min(85.f, ftlCharge_ + rate / 60.f);
    }
    // ShipManager::OnLoop4a1ce0 tests charge completion, including the
    // no-fuel shortcut, independently of current ShipFlying validity.
    if (!wasReady && ftlCharge_ >= 85.f) {
        audio_.sound("jumpReady");
    }
    lastJumpReady_ = ftlCharge_ >= 85.f;
}

bool Run::canAfford(pugi::xml_node node) const {
    // CheckRequirements 0x5ede50 bypasses resource requirements for theft.
    if (node.child("item_modify").attribute("steal").as_bool()) {
        return true;
    }
    for (auto item : node.child("item_modify").children("item")) {
        const std::string type = item.attribute("type").as_string();
        const int amount = std::min(item.attribute("min").as_int(), item.attribute("max").as_int());
        const int available = type == "scrap"      ? scrap_
                              : type == "fuel"     ? fuel_
                              : type == "missiles" ? missiles_
                              : type == "drones"   ? parts_
                                                   : 0;
        if (available + amount < 0) {
            return false;
        }
    }
    // Repair-station choices hide overly large repairs when little hull is
    // missing; the name prefix is verified at original .rdata 0x83745b.
    if (std::string_view(node.attribute("name").as_string()).starts_with("REPAIR_STATION")) {
        const int missing = ship_.hull - hull_;
        int repair = 0;
        for (const auto damage : node.children("damage")) {
            repair += std::abs(damage.attribute("amount").as_int());
        }
        if (missing <= 10 && (missing <= 5 || repair > 19)) {
            return (missing > 0 && repair <= 9) || repair == 0;
        }
    }
    return true;
}

EventResources Run::eventResources(pugi::xml_node node, RandomStream& random) const {
    EventResources result;
    result.pursuit = node.child("modifyPursuit").attribute("amount").as_int();
    if (std::abs(result.pursuit) > 100) {
        throw std::runtime_error("invalid_pursuit_modifier");
    }
    for (auto item : node.child("item_modify").children("item")) {
        const std::string type = item.attribute("type").as_string();
        const int minimum = item.attribute("min").as_int();
        const int maximum = item.attribute("max").as_int(minimum);
        if (maximum < minimum || minimum < -10000 || maximum > 10000) {
            throw std::runtime_error("invalid_event_resource_range");
        }
        int* target = type == "scrap"      ? &result.scrap
                      : type == "fuel"     ? &result.fuel
                      : type == "missiles" ? &result.missiles
                      : type == "drones"   ? &result.drones
                                           : nullptr;
        if (!target) {
            throw std::runtime_error("unsupported_event_resource: " + type);
        }
        const int amount =
            minimum + (maximum == minimum
                           ? 0
                           : static_cast<int>(random.next() %
                                              static_cast<std::uint32_t>(maximum - minimum + 1)));
        *target += amount;
        if (std::abs(*target) > 10000) {
            throw std::runtime_error("invalid_event_resource_total");
        }
    }
    return result;
}

void Run::applyResources(const EventResources& resources) {
    // WorldManager::ModifyResources0x5f0290 clamps each inventory to zero.
    const auto previousResources = resourceAmounts();
    graph_.modifyPursuit(resources.pursuit);
    int scrapIncome = resources.scrap;
    if (scrapIncome > 0) {
        // ShipManager::ModifyScrapCount4908a0 applies all income augments at
        // receipt; event preview and scrap-collected statistics remain authored.
        float modifier = 0;
        bool repairArm = false;
        for (const auto& id : ship_.augments) {
            if (id == "SCRAP_COLLECTOR") {
                modifier += catalog_.equipment(id).value;
            }
            if (id == "REPAIR_ARM") {
                repairArm = true;
                if (hull_ != ship_.hull) {
                    modifier -= catalog_.equipment(id).value;
                }
            }
        }
        scrapIncome = static_cast<int>(modifier * static_cast<float>(scrapIncome) +
                                       static_cast<float>(scrapIncome));
        // REPAIR_ARM runs DamageHull(-2, false): nothing while jumping, and
        // the applied (negative) amount becomes iLastDamage.
        if (repairArm && jumpTicks_ <= 0) {
            const int previousHull = hull_;
            hull_ = std::min(ship_.hull, hull_ + 2);
            lastHullDamage_[0] = previousHull - hull_;
        }
    }
    scrap_ = std::max(0, scrap_ + scrapIncome);
    addScore(scoreScrap_, std::max(0, resources.scrap));
    fuel_ = std::max(0, fuel_ + resources.fuel);
    missiles_ = std::max(0, missiles_ + resources.missiles);
    parts_ = std::max(0, parts_ + resources.drones);
    noteResourceChanges(previousResources);
}

void Run::clearLocation() {
    // WorldManager::ClearLocation 0x5ef130 also runs for a completed wait
    // at the same beacon. Space is cleared before unlinking the old ship.
    setEnvironment("");
    clearSpaceDroneRegistry();
    projectiles_.clear();
    beamHitAt_ = {};
    beamKilledCrew_ = {};
    clearPlayerLocationState();
    const int physicalBeacon = locationBeacon_ >= 0 ? locationBeacon_ : graph_.current();
    saveBeaconShip(physicalBeacon);
    const auto oldBeacon = graph_.beacons().at(static_cast<std::size_t>(physicalBeacon));
    if (!oldBeacon.eventXml.empty()) {
        pugi::xml_document previous;
        if (!previous.load_string(oldBeacon.eventXml.c_str())) {
            throw std::runtime_error("invalid_departing_location_event");
        }
        auto base = previous.document_element();
        const bool cachedShip = !graph_.beacons()[physicalBeacon].savedShip.is_null();
        auto beacon = base.attribute("_nativeBeacon");
        if (!beacon) {
            beacon = base.append_attribute("_nativeBeacon");
        }
        beacon.set_value(oldBeacon.exit);
        if (!oldBeacon.fleet) {
            if (auto ship = base.child("ship")) {
                auto present = ship.attribute("_nativeShipPresent");
                if (!present) {
                    present = ship.append_attribute("_nativeShipPresent");
                }
                present.set_value(false);
                auto hostile = ship.attribute("hostile");
                if (!hostile) {
                    hostile = ship.append_attribute("hostile");
                }
                hostile.set_value(cachedShip);
            }
        }
        events_.clearLocationEvent(base, false, cachedShip);
        std::ostringstream cleared;
        base.print(cleared, "", pugi::format_raw);
        graph_.setEventXml(physicalBeacon, cleared.str());
    }
    clearEnemyLocationCrew();
    detachCrewDroneShip(1);
    // ClearLocation0x5ef130 destroys the departing CompleteShip. Its
    // ShipManager-owned floating messages cannot belong to the next enemy.
    std::erase_if(damageMessages_, [](const DamageMessage& message) {
        return message.ship == 1;
    });
    systemExplosions_[1].clear();
    lastHullDamage_[1] = 0;
    roomLastOxygen_[1].clear();
    fireOxygen_[1].clear();
    fireExtinguishers_[1].clear();
    batteryCapacity_[1] = -1;
    enemy_ = {};
    enemyHazards_.clear();
    hostile_ = false;
    encounter_.clear();
    encounterAutoBlueprint_.clear();
    enemyExplosionActive_ = false;
    explosion_ = 0;
    cancelAim();
    locationBeacon_ = graph_.current();
}

void Run::enterEvent(pugi::xml_node node, bool newLocation) {
    // Prepare all fallible data before replacing the visible dialog or random stream.
    auto random = eventRandom_;
    node = events_.resolve(node, random);
    for (auto child : node.children()) {
        const std::string kind = child.name();
        if (kind != "text" && kind != "choice" && kind != "item_modify" && kind != "ship" &&
            kind != "environment" && kind != "fleet" && kind != "modifyPursuit" &&
            kind != "autoReward" && kind != "weapon" && kind != "drone" && kind != "augment" &&
            kind != "system" && kind != "crewMember" && kind != "damage" && kind != "upgrade" &&
            kind != "status" && kind != "quest" && kind != "store" && kind != "reveal_map" &&
            kind != "distressBeacon" && kind != "removeCrew" && kind != "removeItem" &&
            kind != "boarders" && kind != "img" && kind != "unlockShip" && kind != "secretSector" &&
            kind != "repair") {
            throw std::runtime_error("unsupported_event_effect: " + kind);
        }
    }
    auto resources = eventResources(node, random);
    // SwitchImages and SetDangerZone belong to CreateLocation, whereas
    // UpdateLocation applies nested choices without selecting images again.
    auto voyageEffects = stageVoyageEffects(node, random, resources, false);
    auto environment = node.child("environment");
    const std::string environmentType = environment.attribute("type").as_string();
    if (environment && environmentType != "nebula" && environmentType != "storm" &&
        environmentType != "sun" && environmentType != "asteroid" && environmentType != "PDS" &&
        environmentType != "pulsar" && environmentType != "normal") {
        throw std::runtime_error("unsupported_voyage_environment: " + environmentType);
    }
    int unlockFamily = -1;
    std::string unlockText;
    if (auto unlock = node.child("unlockShip")) {
        const std::string identifier = unlock.attribute("id").as_string();
        if (identifier.size() != 1 || identifier[0] < '0' || identifier[0] > '9') {
            throw std::runtime_error("invalid_event_unlock_ship");
        }
        unlockFamily = identifier[0] - '0';
        unlockText = unlock.text().as_string();
        if (unlockText.empty()) {
            unlockText = texts_.get("unlock_ship_" + identifier, locale_);
        }
    }
    const auto textSource = events_.textSource(node.child("text"), random);
    auto text = textSource.literal ? textSource.value : texts_.get(textSource.value, locale_);
    const auto authoredText = text;
    std::string id = node.attribute("name").as_string(eventId_.c_str());
    // UpdateLocation0x5f57d0 applies physical location changes before filtering
    // the dialog choices. Only ResourceEvent rewards/costs wait for a choice.
    eventRandom_ = random;
    if (newLocation) {
        restoreBeaconFleet(node);
    }
    if (newLocation && !enemy_.present && !graph_.beacons()[graph_.current()].savedShip.is_null()) {
        restoreBeaconShip();
    }
    if (auto ship = node.child("ship")) {
        const bool hostile = ship.attribute("hostile").as_bool(false);
        if (!enemy_.present && (!ship.attribute("_nativeShipPresent") ||
                                ship.attribute("_nativeShipPresent").as_bool())) {
            encounter_ = ship.attribute("_nativeInlineShipTemplate")
                             ? ship.attribute("_nativeInlineShipTemplate").value()
                             : ship.attribute("load").as_string();
            const auto shipSeed = ship.attribute("_nativeShipSeed").as_uint();
            // WorldManager::CreateShip 0x5eee10 copies this ShipEvent, seed
            // included, into currentShipEvent; random32 seeds are below 2^31.
            encounterShipSeed_ = static_cast<int>(shipSeed);
            // WorldManager presents the blueprint under this seed; CreateShip
            // reseeds again before generation. The resulting stream remains global.
            if (shipSeed != 0) {
                eventRandom_ = RandomStream(shipSeed);
            }
            const auto description = events_.shipEvent(encounter_);
            std::string blueprint =
                ship.attribute("auto_blueprint")
                    .as_string(description.attribute("auto_blueprint")
                                   .as_string(description.attribute("blueprint").as_string()));
            encounterAutoBlueprint_ = blueprint;
            if (!blueprint.empty()) {
                const std::string& generatorBlueprint = blueprint;
                (void)catalog_.selectShip(blueprint, sector_ - 1, eventRandom_);
                beginCombat(generatorBlueprint, {}, encounter_, shipSeed);
            } else if (hostile && encounter_ == "REBEL") {
                beginCombat("REBEL_SKINNY", {}, encounter_, shipSeed);
            }
        }
        const bool wasHostile = hostile_;
        hostile_ = hostile;
        if (wasHostile && !hostile_) {
            stopCombatHostilities();
        }
    }
    auto deferredEffects = std::move(pendingVoyageEffects_);
    pendingVoyageEffects_ = nlohmann::json::array();
    const auto immediate = [&](std::initializer_list<const char*> kinds) {
        for (const auto* kind : kinds) {
            for (const auto& effect : voyageEffects) {
                if (effect.at("kind") == kind) {
                    pendingVoyageEffects_.push_back(effect);
                }
            }
        }
        (void)commitVoyageEffects(&text);
    };
    for (const auto& effect : voyageEffects) {
        if (effect.at("kind") == "img") {
            applyVoyageImage(effect);
        }
    }
    if (!newLocation) {
        // UpdateLocation 0x5f57d0: CheckStatusEffects, then CreateStore.
        immediate({"status"});
        advanceStatusEffectShips();
        immediate({"store"});
    }
    immediate({"damage", "reveal_map", "distressBeacon", "repair"});
    graph_.modifyPursuit(resources.pursuit);
    resources.pursuit = 0;
    const auto unlockShip = [&] {
        if (unlockFamily < 0 || tutorialOwned_) {
            return;
        }
        if (!progression_.unlocked(unlockFamily, 0)) {
            text += "\n\n" + unlockText;
        }
        progressionRun_.tutorial = tutorialOwned_;
        progression_.unlock(unlockFamily, 0, progressionRun_);
        if (unlockFamily != 0 && unlockFamily != 3 && unlockFamily != 9) {
            progression_.award(ProgressionLedger::blueprint(unlockFamily) + "_QUEST",
                               progressionRun_);
        }
    };
    if (newLocation) {
        // CreateLocation5f2be0 orders damage/pursuit/quest/unlock before
        // store/environment/status, unlike nested UpdateLocation5f57d0.
        immediate({"quest"});
        unlockShip();
        immediate({"store"});
        storeOpenPending_ = false;
        if (stores_.contains(graph_.current())) {
            graph_.setStore(graph_.current(), true);
            createStore();
            storeOpenPending_ = true;
        }
    }
    restoreBeaconEnvironment(node);
    if (newLocation) {
        // CreateLocation 0x5f2be0 checks status effects after the
        // environment, before the boarders.
        immediate({"status"});
        advanceStatusEffectShips();
    }
    immediate({"boarders"});
    if (!newLocation) {
        immediate({"quest"});
        unlockShip();
    }
    immediate({"secretSector"});
    pendingVoyageEffects_ = std::move(deferredEffects);
    for (const auto& effect : voyageEffects) {
        const auto kind = effect.at("kind").get<std::string>();
        if (kind == "equipment" || kind == "crew" || kind == "crewSkills" || kind == "removeCrew" ||
            kind == "upgrade" || kind == "system" || kind == "removeItem") {
            pendingVoyageEffects_.push_back(effect);
        }
    }
    const bool resourcesApplied = text.empty();
    if (resourcesApplied) {
        applyResources(resources);
        const auto followup = commitVoyageEffects();
        resources = {};
        if (!followup.empty()) {
            if (newLocation) {
                selectedVoyageCrewSeed_ = -1;
                generatedEvent_.clear();
                choiceHistory_.clear();
            }
            enterEvent(resourceFollowupEvent(followup, {}));
            return;
        }
    }
    // CreateLocation5f2be0 clears this retained selector after its physical
    // UpdateLocation and before CreateChoiceBox. Ordinary nested choices keep
    // it; generated World outcomes clear it at their own caller boundary.
    if (newLocation) {
        selectedVoyageCrewSeed_ = -1;
        // CreateLocation 0x5f30a7 clears generatedEvent and choiceHistory too.
        generatedEvent_.clear();
        choiceHistory_.clear();
    }
    random = eventRandom_;
    std::vector<RunChoice> choices;
    std::vector<pugi::xml_node> visibleChoices;
    std::map<int, pugi::xml_node> groups;
    std::map<pugi::xml_node, int> authoredIndex;
    for (auto choice : node.children("choice")) {
        authoredIndex.emplace(choice, static_cast<int>(authoredIndex.size()));
        if (choice.attribute("req")) {
            const std::string requirement = choice.attribute("req").as_string();
            if (!requirement.empty() && !eventRequirement(requirement,
                                                          choice.attribute("lvl").as_int(1),
                                                          choice.attribute("max_lvl").as_int(
                                                              std::numeric_limits<int>::max()))) {
                continue;
            }
            const int group = choice.attribute("max_group").as_int(-1);
            if (!requirement.empty() && group != -1) {
                const auto found = groups.find(group);
                if (found == groups.end() ||
                    choice.attribute("lvl").as_int(1) > found->second.attribute("lvl").as_int(1)) {
                    groups[group] = choice;
                }
                continue;
            }
        }
        visibleChoices.push_back(choice);
    }
    // CreateChoiceBox 0x5f0f70 appends the highest eligible minimum-level
    // choice from each max_group in group-number order, after ordinary choices.
    for (const auto& [group, choice] : groups) {
        (void)group;
        visibleChoices.push_back(choice);
    }
    for (auto choice : visibleChoices) {
        auto next = choice.child("event");
        const bool enabled = canAfford(next);
        const std::string requirement = choice.attribute("req").as_string();
        // CrewMemberFactory::IsRace0x45ce90 intentionally excludes crystal.
        const bool race = requirement == "human" || requirement == "engi" ||
                          requirement == "ghost" || requirement == "energy" ||
                          requirement == "rock" || requirement == "slug" ||
                          requirement == "mantis" || requirement == "anaerobic";
        choices.push_back({events_.text(choice.child("text"), random, texts_, locale_),
                           next,
                           enabled,
                           race,
                           !requirement.empty() && choice.attribute("blue").as_bool(true),
                           choice.attribute("hidden").as_bool(),
                           authoredIndex.at(choice)});
    }
    const bool open = !text.empty() || !choices.empty();
    if (open && choices.empty()) {
        // EventGenerator gives a choice-less event one "continue" choice.
        choices.push_back({texts_.get("continue", locale_), {}, true, false, false, false, 0});
    }
    pendingResources_ = resources;
    eventRandom_ = random;
    progressionRun_.tutorial = tutorialOwned_;
    // CreateChoiceBox 0x5f0fa5: lastMainText keeps the text id unless an append
    // (TextString::operator+=) made it literal.
    if (open) {
        lastMainTextLiteral_ = textSource.literal || text != authoredText;
        lastMainText_ = lastMainTextLiteral_ ? text : textSource.value;
    }
    eventText_ = std::move(text);
    eventId_ = std::move(id);
    choices_ = std::move(choices);
    event_ = open;
    if (event_) {
        resetEventChoiceDelay();
    }
    if (!event_ && !resourcesApplied) {
        applyResources(pendingResources_);
        const auto followup = commitVoyageEffects();
        pendingResources_ = {};
        if (!followup.empty()) {
            enterEvent(resourceFollowupEvent(followup, {}));
        }
    }
}

std::string Run::commitCombatEquipmentReward() {
    if (enemy_.rewardItem.empty()) {
        return {};
    }
    return rewardEquipment(std::exchange(enemy_.rewardItem, {}));
}

pugi::xml_node Run::resourceFollowupEvent(const std::string& id, pugi::xml_node next) {
    // ModifyResources0x5f0290 / PauseLoop0x5f6070 show the authored extra event,
    // then continue the player's selected child with its reward preview hidden.
    const auto freeze = [this](pugi::xml_node event, RandomStream& stream) {
        freezeVoyageEvent(event, stream);
    };
    const auto xml =
        materializeEvent(events_.event(id), eventRandom_, advanced_, usedVoyageEvents_, freeze);
    restoredEvents_.emplace_back();
    auto& document = restoredEvents_.back();
    if (!document.load_string(xml.c_str())) {
        throw std::runtime_error("invalid_resource_followup");
    }
    auto event = document.document_element();
    // ModifyResources 0x5f0290 records these follow-ups as choiceHistory markers.
    choiceHistory_.push_back(id == "AUGMENT_FULL" ? 101 : id == "CLONED" ? 102 : 103);
    if (id == "EQUIP_FULL" || id == "AUGMENT_FULL") {
        auto item = event.append_child("item_modify").append_child("item");
        item.append_attribute("type") = "scrap";
        item.append_attribute("min") = 25;
        item.append_attribute("max") = 25;
    }
    if (id == "CLONED") {
        while (const auto authoredText = event.child("text")) {
            event.remove_child(authoredText);
        }
        event.append_child("text").text().set(resourceFollowupText_.c_str());
        resourceFollowupText_.clear();
    }
    while (const auto authoredChoice = event.child("choice")) {
        event.remove_child(authoredChoice);
    }
    auto choice = event.append_child("choice");
    choice.append_attribute("hidden") = true;
    choice.append_child("text").append_attribute("id") = "continue";
    if (next) {
        choice.append_copy(next);
    } else {
        choice.append_child("event");
    }
    return event;
}

std::string Run::applyCombatRewardPickups() {
    if ((eventId_ != "P06_VICTORY_REWARD" && eventId_ != "P06_SURRENDER_REWARD") ||
        enemy_.rewardCommitted) {
        return {};
    }
    const auto followup = commitCombatEquipmentReward();
    commitCombatCrewReward();
    if (enemy_.rewardCrewPending) {
        panel_ = "crew";
    }
    enemy_.rewardCommitted = true;
    return followup;
}

void Run::choose(int index) {
    if (!event_ || arrivalTicks_ > 0 || jumpTicks_ > 0 || index < 0 ||
        index >= static_cast<int>(choices_.size()) || !choices_[index].enabled) {
        return;
    }
    // Playtest requires audible choice activation as well as pointer entry.
    // Reuse MouseControl::OnRender 0x4d6e00's original select_light1 cue.
    audio_.hover();
    if (defeated_) {
        close();
        return;
    }
    // PauseLoop 0x5f62b0 records the authored index of every click.
    choiceHistory_.push_back(choices_[index].authoredIndex >= 0 ? choices_[index].authoredIndex
                                                                : index);
    if (surrenderDialog_ && !choices_[index].event) {
        surrenderDialog_ = false;
        if (index == 0) {
            // PIRATE_SURRENDER choice 0 requests ship hostile=false and its
            // RANDOM/stuff reward; choice 1 is an empty event. Keep the offer
            // latched after either branch, matching ShipAI::Surrender's
            // one-shot `surrendered` flag at 0x600cb0.
            combatResult(true);
            applyResources(pendingResources_);
            const auto followup = applyCombatRewardPickups();
            pendingResources_ = {};
            event_ = false;
            choices_.clear();
            if (!followup.empty()) {
                enterEvent(resourceFollowupEvent(followup, {}));
            }
        } else {
            event_ = false;
            choices_.clear();
        }
        if (!event_) {
            eventChoiceHoverValid_ = false;
        }
        return;
    }
    surrenderDialog_ = false;
    const auto next = choices_[index].event;
    const bool raceChoice = choices_[index].raceChoice;
    nlohmann::json transactionBackup;
    checkpoint(transactionBackup, false);
    const auto oldFeedback = resourceDeltas_;
    const auto oldFeedbackTicks = resourceDeltaTicks_;
    const auto oldProgression = progression_;
    try {
        progressionRun_.tutorial = tutorialOwned_;
        if (raceChoice) {
            progression_.blueCrewChoice(sector_ - 1, progressionRun_);
        }
        applyResources(pendingResources_);
        auto followup = applyCombatRewardPickups();
        const auto authoredFollowup = commitVoyageEffects();
        if (!authoredFollowup.empty()) {
            followup = authoredFollowup;
        }
        if (!followup.empty()) {
            enterEvent(resourceFollowupEvent(followup, next));
        } else if (next) {
            enterEvent(next);
        } else {
            event_ = false;
            choices_.clear();
            pendingResources_ = {};
        }
        if (!event_) {
            eventChoiceHoverValid_ = false;
        }
    } catch (...) {
        checkpoint(transactionBackup, true);
        catalog_.setUsedCrewNames(crewNameHistory_);
        progression_ = oldProgression;
        resourceDeltas_ = oldFeedback;
        resourceDeltaTicks_ = oldFeedbackTicks;
        throw;
    }
}

std::vector<std::string> Run::dialogLines(const std::string& value, float width) const {
    return renderer_.wrappedLines(localizedFont(eventFontSize_, value),
                                  value,
                                  width,
                                  textScale(eventFontSize_),
                                  locale_ == "zh-Hans");
}

std::vector<std::string> Run::eventLines() const {
    return dialogLines(eventText_);
}

std::vector<std::string> Run::choiceLines(std::size_t index) const {
    return dialogLines(std::to_string(index + 1) + ". " + choices_[index].text,
                       choiceTextWidth(index));
}

EventResources Run::choiceResources(std::size_t index) const {
    const auto& choice = choices_.at(index);
    if (choice.hiddenReward) {
        return {};
    }
    if (!choice.event) {
        return surrenderDialog_ && index == 0 ? EventResources{9, 0, 2, 1, 0} : EventResources{};
    }
    EventResources result;
    // CreateChoiceBox copies the already-generated child reward. Read its frozen
    // values without staging side effects or advancing any gameplay stream.
    for (const auto item : choice.event.child("item_modify").children("item")) {
        const std::string_view type = item.attribute("type").as_string();
        const int amount = item.attribute("min").as_int();
        if (type == "scrap") {
            result.scrap += amount;
        } else if (type == "fuel") {
            result.fuel += amount;
        } else if (type == "missiles") {
            result.missiles += amount;
        } else if (type == "drones") {
            result.drones += amount;
        }
    }
    for (const auto child : choice.event.children()) {
        if (child.attribute("_nativeVoyage")) {
            const auto frozen = nlohmann::json::parse(child.attribute("_nativeVoyage").value());
            result.scrap += frozen.at("scrap").get<int>();
            result.fuel += frozen.at("fuel").get<int>();
            result.missiles += frozen.at("missiles").get<int>();
            result.drones += frozen.at("drones").get<int>();
        }
    }
    return result;
}

RewardLayout Run::choiceRewardLayout(std::size_t index) const {
    // ChoiceBox::OnRender 0x4ec490 measures each choice's frozen child reward
    // with MeasureResources(choice = true): the resources plus the weapon,
    // drone, augment and crew CreateEvent already generated for it.
    const auto& choice = choices_.at(index);
    auto entries = nlohmann::json::array();
    if (choice.event && !choice.hiddenReward) {
        for (const auto child : choice.event.children()) {
            if (!child.attribute("_nativeVoyage")) {
                continue;
            }
            const auto frozen = nlohmann::json::parse(child.attribute("_nativeVoyage").value());
            for (const auto& effect : frozen.at("effects")) {
                const auto kind = effect.value("kind", std::string{});
                if (kind != "equipment" && kind != "crew") {
                    continue;
                }
                if (kind == "equipment" &&
                    child.attribute("_nativeClearedBasicResources").as_bool()) {
                    const auto& item = catalog_.equipment(effect.at("id").get<std::string>());
                    if (item.kind == "weapon" || item.kind == "drone") {
                        continue;
                    }
                }
                entries.push_back(effect);
            }
        }
    }
    return rewardLayout(choiceResources(index), std::move(entries), true);
}

float Run::choiceTextWidth(std::size_t index) const {
    const float resourcesWidth = static_cast<float>(choiceRewardLayout(index).width);
    if (resourcesWidth == 0) {
        return 565;
    }
    const std::string label = std::to_string(index + 1) + ". " + choices_[index].text;
    auto& font = localizedFont(eventFontSize_, label);
    float width = 0;
    for (const auto& line : dialogLines(label)) {
        const auto [left, right] = renderer_.bounds(font, line, textScale(eventFontSize_));
        width = std::max(width, right - left);
    }
    return resourcesWidth + width > 565 ? std::max(1.f, 545.f - resourcesWidth) : width + 10.f;
}

float Run::choiceHeight(std::size_t index) const {
    // OnRender 0x4ec490 advances lastChoice by 10, then by max(reward box
    // height, text height + 12); the native text term already carries the 10.
    const float box = static_cast<float>(choiceRewardLayout(index).height);
    return std::max({surrenderDialog_ ? 36.f : 32.f,
                     box > 0 ? box + 10.f : 0.f,
                     textLineHeight(eventFontSize_, choices_[index].text) *
                             static_cast<float>(choiceLines(index).size()) +
                         12.f});
}

float Run::choiceRow(std::size_t index) const {
    float top = choiceTop() + (surrenderDialog_ ? 6.f : 0.f);
    for (std::size_t previous = 0; previous < index; ++previous) {
        top += choiceHeight(previous);
    }
    return top;
}

nlohmann::json Run::eventRewardEntries() const {
    // ChoiceBox::SetEventReward/RenderResources retain the generated reward;
    // presenting it must neither regenerate it nor commit it to the inventory.
    auto entries = nlohmann::json::array();
    for (const auto& effect : pendingVoyageEffects_) {
        const auto kind = effect.value("kind", std::string{});
        if (kind == "equipment" || kind == "crew") {
            entries.push_back(effect);
        }
    }
    if (!enemy_.rewardCommitted &&
        (eventId_ == "P06_VICTORY_REWARD" || eventId_ == "P06_SURRENDER_REWARD")) {
        if (!enemy_.rewardItem.empty()) {
            entries.push_back({{"kind", "equipment"}, {"id", enemy_.rewardItem}});
        }
        if (enemy_.rewardCrewPending) {
            entries.push_back({{"kind", "crew"},
                               {"species", enemy_.rewardCrewSpecies},
                               {"name", enemy_.rewardCrewName},
                               {"full_name", enemy_.rewardCrewFullName},
                               {"appearance", enemy_.rewardCrewAppearance}});
        }
    }
    return rewardLayout(pendingResources_, std::move(entries), false).entries;
}

RewardLayout
Run::rewardLayout(const EventResources& resources, nlohmann::json entries, bool choice) const {
    // ChoiceBox::RenderResources 0x4e92e0 lays the rewards out in fixed order
    // weapon, drone, augment, crew; MeasureResources 0x4ebaf0 runs the same
    // code without drawing. In an event result (choice false) everything sits on
    // one row. In a choice preview, a drawn resource row ends row 1 at (n5, 32)
    // and the items restart at n5 = 10 on row 2, 32 px lower; each row is then
    // centred in the box by PrintResourceBox 0x4ebde0.
    const auto order = [this](const nlohmann::json& entry) {
        if (entry.at("kind") != "equipment") {
            return 3;
        }
        const auto& kind = catalog_.equipment(entry.at("id").get<std::string>()).kind;
        return kind == "weapon" ? 0 : kind == "drone" ? 1 : 2;
    };
    std::stable_sort(
        entries.begin(), entries.end(), [&](const nlohmann::json& a, const nlohmann::json& b) {
            return order(a) < order(b);
        });
    const auto measure = [&](const std::string& value, int size) {
        return renderer_.measure(localizedFont(size, value), value, textScale(size));
    };
    RewardLayout layout;
    // n5 starts at 5; RenderResource 0x4e8c60 returns 45/50 (+5 if negative) per
    // resource, then 0x4e9eea adds 20 when any resource was drawn (result) or
    // wraps to row 2 (choice).
    const int resourcesWidth = resourceBoxWidth(resources);
    int x = 5 + resourcesWidth;
    int rowY = 0;
    int rowHeight = 32;
    if (resourcesWidth > 0) {
        if (choice) {
            layout.row1Width = x;
            layout.row1Height = 32;
            x = 10;
            rowY = 32;
            rowHeight = 0;
        } else {
            x += 20;
        }
    }
    bool drawn = false;
    for (auto& entry : entries) {
        std::string label;
        std::string prefix;
        int iconX = 0;
        int labelX = 0;
        int prefixX = 0;
        int iconWidth = 0;
        int iconHeight = 0;
        int height = 32;
        int start = x;
        const auto kind = entry.at("kind").get<std::string>();
        if (kind == "equipment") {
            const auto& bp = catalog_.equipment(entry.at("id").get<std::string>());
            label = bp.title.reference ? texts_.get(bp.title.value, locale_) : bp.title.value;
            if (bp.kind == "weapon") {
                // WeaponBlueprint::GetDimensions 0x5df4d0 swaps the frame axes.
                // 0x4e9a10..0x4e9bac: icon centred at n5 + w / 2, label at
                // n5 + w + 10, n5 += label + w + 20, height max(32, h + 9). A
                // choice preview first lowers the running row by (h - 32) >> 1
                // when h > 32.
                iconWidth = bp.frameHeight;
                iconHeight = bp.frameWidth;
                if (choice && iconHeight > 32) {
                    rowY += (iconHeight - 32) >> 1;
                }
                iconX = x + iconWidth / 2;
                labelX = x + iconWidth + 10;
                x = static_cast<int>(static_cast<float>(x) + measure(label, 10)) + iconWidth + 20;
                height = std::max(32, iconHeight + 9);
            } else if (bp.kind == "drone") {
                // DroneBlueprint::GetDimensions 0x5e0600; type ids from
                // DroneSystem::StringToDrone 0x612620 (REPAIR 2, BATTLE 3,
                // BOARDER 4 are the crew-sprite drones).
                const bool crewDrone =
                    bp.type == "REPAIR" || bp.type == "BATTLE" || bp.type == "BOARDER";
                if (crewDrone) {
                    // Blueprint name + "_walk_down" animation frame size.
                    const auto& strip = catalog_.animation(bp.id + "_walk_down");
                    iconWidth = strip.usable() ? strip.width : 0;
                    iconHeight = strip.usable() ? strip.height : 0;
                } else {
                    // "ship/drones/<image>_charged.png" size, 1 x 1 when missing;
                    // DEFENSE and COMBAT_BEAM_2 add 8 to y, SHIELD adds 16 x 22.
                    iconWidth = 1;
                    iconHeight = 1;
                    const auto image = "img/ship/drones/" + bp.art + "_charged.png";
                    if (archive_.contains(image)) {
                        auto& art = renderer_.image(image);
                        iconWidth = art.w;
                        iconHeight = art.h;
                    }
                    if (bp.type == "DEFENSE" || bp.id == "COMBAT_BEAM_2") {
                        iconHeight += 8;
                    } else if (bp.type == "SHIELD") {
                        iconWidth += 16;
                        iconHeight += 22;
                    }
                }
                // 0x4e9585: non-crew drones start 16 px left; 0x4e9644 puts the
                // label at n5 + w + 15 (crew) or n5 + w - 10; 0x4e9739/0x4ea3f0
                // advance by label + w + 20 (height 32) or label + w - 4
                // (height max(32, h - 16)). A choice preview adds 10 before a
                // crew drone, and lowers the row by (h - 48) / 2 - 10 (height
                // then from h - 10) when a non-crew drone's h - 16 exceeds 32.
                if (!crewDrone) {
                    x -= 16;
                    if (choice && iconHeight - 16 > 32) {
                        rowY += (iconHeight - 48) / 2 - 10;
                        iconHeight -= 10;
                    }
                } else if (choice) {
                    x += 10;
                }
                start = x;
                iconX = x + iconWidth / 2;
                labelX = x + iconWidth + (crewDrone ? 15 : -10);
                x = static_cast<int>(static_cast<float>(x) + measure(label, 10)) + iconWidth +
                    (crewDrone ? 20 : -4);
                height = crewDrone ? 32 : std::max(32, iconHeight - 16);
            } else {
                // 0x4e97aa..0x4e9910: "augment" in font 12 at n5, the title in
                // font 10 five pixels after it, then five pixels of spacing.
                prefix = texts_.get("augment", locale_);
                prefixX = x;
                labelX = static_cast<int>(static_cast<float>(x) + (measure(prefix, 12) + 5.f));
                x = static_cast<int>((measure(label, 10) + 5.f) + static_cast<float>(labelX));
            }
        } else {
            // 0x4e9bd2..0x4e9e2c: n5 += 20 unless only resources precede it
            // (result: n5 already carries their 20; choice: the wrapped row
            // starts the icon at 15 and the name at 30). With nothing before it
            // a choice preview puts the icon at 23 and the name at 38. Otherwise
            // icon at n5, name at n5 + 15, then v = name end + 15. Up to two
            // skills with progress > 0, in skill order, get a
            // CrewBlueprint::RenderSkillIcon 0x5df7a0 at v (the first after
            // v -= 10), v += 23 each; n5 = v + 5 after icons, else v - 7.
            label = entry.value("full_name", entry.value("name", std::string{}));
            const bool resourcesOnly = resourcesWidth > 0 && !drawn;
            if (!resourcesOnly) {
                x += 20;
            }
            if (choice && resourcesOnly) {
                x = 15;
                labelX = 30;
            } else if (choice && !drawn) {
                labelX = x + 13;
                x -= 2;
            } else {
                labelX = x + 15;
            }
            start = x;
            iconX = x;
            int next = static_cast<int>((measure(label, 10) + 15.f) + static_cast<float>(labelX));
            auto icons = nlohmann::json::array();
            if (entry.contains("skill_progress")) {
                constexpr std::array<int, 6> caps{30, 30, 110, 130, 36, 16};
                const auto progress = entry.at("skill_progress").get<std::array<int, 6>>();
                for (std::size_t skill = 0; skill < progress.size() && icons.size() < 2; ++skill) {
                    if (progress[skill] <= 0) {
                        continue;
                    }
                    if (icons.empty()) {
                        next -= 10;
                    }
                    icons.push_back({{"skill", skill},
                                     {"x", next},
                                     {"fraction", float(progress[skill]) / float(caps[skill])}});
                    next += 23;
                }
            }
            entry["skill_icons"] = icons;
            x = icons.empty() ? next - 7 : next + 5;
        }
        drawn = true;
        rowHeight = std::max(rowHeight, height);
        entry["label"] = label;
        entry["prefix"] = prefix;
        entry["prefix_x"] = prefixX;
        entry["x"] = start;
        entry["y"] = rowY;
        entry["icon_x"] = iconX;
        entry["label_x"] = labelX;
        entry["end"] = x;
        entry["icon_width"] = iconWidth;
        entry["icon_height"] = iconHeight;
        entry["height"] = height;
    }
    // LABEL_93..LABEL_94: row 2 is (n5, n32); the box is max(row1, row2) wide
    // and row1 + row2 tall. With nothing after a wrapped resource row, row 2
    // is (10, 0).
    if (choice && resourcesWidth > 0) {
        layout.row2Width = x;
        layout.width = std::max(layout.row1Width, x);
        layout.height = layout.row1Height + rowHeight;
    } else if (drawn || resourcesWidth > 0) {
        layout.row2Width = x;
        layout.width = x;
        layout.height = rowHeight;
    }
    layout.entries = std::move(entries);
    return layout;
}

float Run::eventRewardHeight() const {
    // RenderResources 0x4e92e0 ends with h = max of the item heights, at least 32.
    const auto entries = eventRewardEntries();
    if (entries.empty() && !pendingResources_.visible()) {
        return 0;
    }
    int height = 32;
    for (const auto& entry : entries) {
        height = std::max(height, entry.at("height").get<int>());
    }
    return static_cast<float>(height);
}

float Run::choiceTop() const {
    // ChoiceBox::OnRender 0x4ec490: lastChoice.y = text end + n30 (30, set by
    // the CommandGui constructor at 0x523ecf), plus a fixed 50 when the reward
    // box is shown whatever its height; choice text sits 10 px lower.
    return openingEvent_ ? 370.f
                         : 210.f +
                               textLineHeight(eventFontSize_, eventText_) *
                                   static_cast<float>(eventLines().size()) +
                               (eventRewardHeight() > 0 ? 50.f : 0.f);
}

void Run::travelMap() {
    renderer_.fill({511, 0, 769, 720}, {0, 0, 0, 190});
    renderer_.fill({0, 140, 511, 580}, {0, 0, 0, 190});
    renderer_.draw("img/map/zone_1.png", 340, 83, .75f);
    // StarMap::OnRender 0x5d9ab0 draws the nebula clouds inside the
    // WindowFrame::DrawMask stencil (0x5dabcc..0x5dbb82), the map rectangle.
    renderer_.setClip(Rect{340, 83, 752, 534});
    for (const auto& nebula : mapNebulas_) {
        renderer_.draw(
            std::string("img/map/nebula_") + (nebula.variant >= 4 ? "large_" : "") +
                std::to_string(nebula.variant >= 4 ? nebula.variant - 4 : nebula.variant) + ".png",
            385.f + nebula.x,
            123.f + nebula.y);
    }
    renderer_.setClip(std::nullopt);
    const int hovered = hoveredBeacon();
    const auto changing = [&](int id) {
        return campaign_.active &&
               std::ranges::find(campaign_.changing, id) != campaign_.changing.end();
    };
    const auto mapText =
        [&](const std::string& value, float x, float y, int size, bool center, SDL_Color color) {
            text(value, x, y, size, center, color);
        };
    auto connection = [&](int first, int second, SDL_Color color) {
        const auto& a = graph_.beacons()[first];
        const auto& b = graph_.beacons()[second];
        renderer_.texturedLine(renderer_.image("img/map/dotted_line.png"),
                               {385.f + a.x, 123.f + a.y},
                               {385.f + b.x, 123.f + b.y},
                               color);
    };
    if (options_.showPaths && hovered >= 0 && hovered != graph_.current()) {
        for (int neighbor : graph_.beacons()[hovered].neighbors) {
            if (neighbor != graph_.current()) {
                connection(hovered, neighbor, {255, 255, 50, 150});
            }
        }
    }
    for (int neighbor : graph_.beacons()[graph_.current()].neighbors) {
        connection(graph_.current(), neighbor, {120, 255, 120, 255});
    }
    // StarMap::OnRender0x5d9ab0: dotted forecast, solid nine-pixel
    // connection when the Flagship will jump on the next committed turn.
    if (campaign_.active && campaign_.position + 1 < static_cast<int>(campaign_.path.size())) {
        const auto& from = graph_.beacons()[campaign_.path[campaign_.position]];
        const auto& to = graph_.beacons()[campaign_.path[campaign_.position + 1]];
        if (campaign_.jumping) {
            const float dx = static_cast<float>(to.x - from.x);
            const float dy = static_cast<float>(to.y - from.y);
            const float distance = std::hypot(dx, dy);
            if (distance > 0) {
                const float nx = -dy / distance * 4.5f;
                const float ny = dx / distance * 4.5f;
                renderer_.polygon({{385.f + from.x + nx, 123.f + from.y + ny},
                                   {385.f + to.x + nx, 123.f + to.y + ny},
                                   {385.f + to.x - nx, 123.f + to.y - ny},
                                   {385.f + from.x - nx, 123.f + from.y - ny}},
                                  {230, 80, 80, 200});
            }
        } else {
            connection(campaign_.path[campaign_.position],
                       campaign_.path[campaign_.position + 1],
                       {230, 80, 80, 200});
        }
    }
    for (std::size_t index = 0; index < graph_.beacons().size(); ++index) {
        const auto& beacon = graph_.beacons()[index];
        const float x = 385.f + beacon.x;
        const float y = 123.f + beacon.y;
        if (beacon.nebula) {
            std::vector<SDL_FPoint> circle;
            for (int angle = 0; angle < 36; ++angle) {
                const float radians = angle * 3.14159265358979323846f / 18;
                circle.push_back({x + 1 + std::cos(radians) * 10, y + 1 + std::sin(radians) * 10});
            }
            // Match the original GL polygon's pixel-center coverage. SDL's
            // software triangle fan rounds these small translucent edges early.
            for (int row = -9; row <= 10; ++row) {
                const float sampleY = y + row + .5f;
                float left = x + 20;
                float right = x - 20;
                for (std::size_t edge = 0; edge < circle.size(); ++edge) {
                    const auto a = circle[edge];
                    const auto b = circle[(edge + 1) % circle.size()];
                    if ((a.y <= sampleY && b.y > sampleY) || (b.y <= sampleY && a.y > sampleY)) {
                        const float intersection =
                            a.x + (sampleY - a.y) * (b.x - a.x) / (b.y - a.y);
                        left = std::min(left, intersection);
                        right = std::max(right, intersection);
                    }
                }
                left = std::ceil(left - .5f);
                right = std::ceil(right - .5f);
                if (right > left) {
                    renderer_.fill({left, y + row, right - left, 1}, {50, 50, 255, 128});
                }
            }
        }
        if (changing(static_cast<int>(index))) {
            const float phase =
                std::fmod((static_cast<float>(animation_ % 120) + uiFraction_) / 60, 2.f);
            renderer_.draw("img/map/map_icon_diamond_circle.png",
                           x - 16,
                           y - 16,
                           1,
                           .25f + .75f * (1 - std::abs(phase - 1)));
        }
        renderer_.draw(beacon.fleet ? "img/map/map_icon_triangle_shadow.png"
                                    : "img/map/map_icon_diamond_shadow.png",
                       x - 16,
                       y - 16);
        // StarMap::OnRender0x5d9ab0 draws the potential destination three times.
        const int passes = static_cast<int>(index) == hovered && graph_.reachable(hovered) ? 3 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            renderer_.draw(beacon.fleet     ? "img/map/map_icon_triangle_red.png"
                           : beacon.visited ? "img/map/map_icon_diamond_blue.png"
                                            : "img/map/map_icon_diamond_yellow.png",
                           x - 16,
                           y - 16);
        }
        if (static_cast<int>(index) == graph_.current()) {
            auto& ship = renderer_.image("img/map/map_icon_ship.png");
            const SDL_FPoint pivot{10, 32};
            renderer_.sprite(ship,
                             {x - 10, y - 32, 64, 64},
                             {0, 0, 64, 64},
                             -(static_cast<double>(animation_ % 1200) + uiFraction_) * .3,
                             {255, 255, 255, 255},
                             false,
                             &pivot);
        }
        if (beacon.exit && !beacon.fleet) {
            // StarMap::RenderLabels0x5c9070: three-piece box, HL1 at scale1.
            const auto label =
                texts_.get(campaign_.active ? "map_icon_base" : "map_icon_exit", locale_);
            const std::string boxColor = campaign_.active ? "purple" : "green";
            const float width =
                std::round(renderer_.measure(localizedFont(51, label), label, textScale(51)));
            const float phase = std::fmod(
                beacon.flashOffset + (static_cast<float>(animation_ % 120) + uiFraction_) / 60,
                2.f);
            const float alpha = .5f + .5f * (1 - std::abs(phase - 1));
            renderer_.draw("img/map/map_box_" + boxColor + "_1.png", x - 1, y - 33, 1, alpha);
            auto& middle = renderer_.image("img/map/map_box_" + boxColor + "_2.png");
            renderer_.sprite(middle,
                             {x + 18, y - 33, width - 8, 32},
                             {0, 0, 1, 32},
                             0,
                             {255, 255, 255, static_cast<Uint8>(255 * alpha)});
            renderer_.draw(
                "img/map/map_box_" + boxColor + "_3.png", x + width + 10, y - 33, 1, alpha);
            const SDL_Color ink = campaign_.active || options_.colorblind
                                      ? SDL_Color{235, 245, 229, static_cast<Uint8>(255 * alpha)}
                                      : SDL_Color{46, 252, 51, static_cast<Uint8>(255 * alpha)};
            mapText(label, x + 14, y - 25, 51, false, ink);
        }
        const bool repair = beacon.repair;
        const bool cachedStore = stores_.contains(static_cast<int>(index));
        const bool bossHere = campaign_.active && !campaign_.path.empty() &&
                              campaign_.path[campaign_.position] == static_cast<int>(index);
        if (!beacon.exit && !beacon.fleet && !changing(static_cast<int>(index)) && !bossHere &&
            (repair || beacon.quest || cachedStore ||
             (beacon.known && (beacon.store || beacon.distress)))) {
            // RenderLabels5c9070 gives quest/purple precedence, then repair,
            // cached Store, known Store and known distress in white boxes.
            const auto label = texts_.get(beacon.quest                  ? "map_icon_quest"
                                          : repair                      ? "map_icon_repair"
                                          : cachedStore || beacon.store ? "map_icon_store"
                                                                        : "map_icon_distress",
                                          locale_);
            const float width =
                std::round(renderer_.measure(localizedFont(51, label), label, textScale(51)));
            const float phase = std::fmod(
                beacon.flashOffset + (static_cast<float>(animation_ % 120) + uiFraction_) / 60.f,
                2.f);
            const float alpha = .5f + .5f * (1.f - std::abs(phase - 1.f));
            const std::string boxColor = beacon.quest ? "purple" : "white";
            renderer_.draw("img/map/map_box_" + boxColor + "_1.png", x - 1, y - 33, 1, alpha);
            auto& middle = renderer_.image("img/map/map_box_" + boxColor + "_2.png");
            renderer_.sprite(middle,
                             {x + 18, y - 33, width - 8, 32},
                             {0, 0, 1, 32},
                             0,
                             {255, 255, 255, static_cast<Uint8>(255 * alpha)});
            renderer_.draw(
                "img/map/map_box_" + boxColor + "_3.png", x + width + 10, y - 33, 1, alpha);
            mapText(
                label, x + 14, y - 25, 51, false, {235, 245, 229, static_cast<Uint8>(255 * alpha)});
        }
        if (static_cast<int>(index) == hovered && graph_.reachable(hovered)) {
            const float phase = targetBoxTime();
            const float inset = std::round((1 - std::cos(phase * 6.283185307f)) * 2.5f) - 21;
            auto& corner = renderer_.image("img/map/map_targetbox.png");
            for (int horizontal : {0, 1}) {
                for (int vertical : {0, 1}) {
                    renderer_.sprite(corner,
                                     {horizontal ? x - inset - 32 : x + inset,
                                      vertical ? y - inset - 32 : y + inset,
                                      32,
                                      32},
                                     {0, 0, 32, 32},
                                     vertical ? 180 : 0,
                                     {255, 255, 255, 255},
                                     horizontal != vertical);
                }
            }
        }
    }
    if (!campaign_.active) {
        // Fleet images are clipped to the map's original stencil rectangle.
        auto fleetImage = [&](const std::string& path, float x, float y) {
            auto& image = renderer_.image(path);
            const float left = std::max(340.f, x);
            const float top = std::max(83.f, y);
            const float right = std::min(1092.f, x + image.w);
            const float bottom = std::min(617.f, y + image.h);
            if (right > left && bottom > top) {
                renderer_.sprite(image,
                                 {left, top, right - left, bottom - top},
                                 {left - x, top - y, right - left, bottom - top});
            }
        };
        const float fleetX = 971.f + graph_.dangerX();
        const float fleetY = graph_.dangerY() - 375.f;
        if (graph_.nextFleetMove() > 0) {
            const float predictedX = fleetX + graph_.nextFleetMove();
            if (predictedX > 340) {
                renderer_.fill({340, 83, std::min(752.f, predictedX - 340), 534},
                               {246, 128, 125, 21});
            }
            fleetImage("img/map/map_warningcircle_point.png", predictedX, fleetY);
        }
        fleetImage("img/map/map_warningcircle.png", fleetX, fleetY);
        for (int x = graph_.dangerX() + 821; x + 150 > 340; x -= 150) {
            for (int y = graph_.dangerY() - 375; y < 617; y += 150) {
                fleetImage("img/map/map_warningcircle_tile.png",
                           static_cast<float>(x),
                           static_cast<float>(y));
            }
        }
    }
    if (campaign_.active && !campaign_.path.empty()) {
        const auto& beacon = graph_.beacons()[campaign_.path[campaign_.position]];
        auto& icon = renderer_.image("img/map/map_icon_boss.png");
        float x = 385.f + beacon.x;
        float y = 123.f + beacon.y;
        double angle = -(static_cast<double>(animation_ % 1200) + uiFraction_) * .3;
        float alpha = 1;
        SDL_FPoint pivot{10, 32};
        if (campaign_.jumping && campaign_.position + 1 < static_cast<int>(campaign_.path.size())) {
            const auto& next = graph_.beacons()[campaign_.path[campaign_.position + 1]];
            const float dx = static_cast<float>(next.x - beacon.x);
            const float dy = static_cast<float>(next.y - beacon.y);
            const float distance = std::hypot(dx, dy);
            if (distance > 0) {
                // Source drift is six pixels/second and loops after twenty.
                // Derive it from UI time so repeated rendering never changes state.
                const float drift =
                    std::fmod((static_cast<float>(animation_ % 600) + uiFraction_) / 10,
                              std::min(20.f, distance));
                x += dx / distance * (20 + drift);
                y += dy / distance * (20 + drift);
                angle = std::atan2(dy, dx) * 180.f / 3.14159265358979323846f + 90.f;
                pivot.x = 31;
                alpha = drift <= 10 ? 1.f : std::pow(1.f - (drift - 10.f) / 10.f, 2.f);
            }
        }
        renderer_.sprite(icon,
                         {x - 10, y - 32, 64, 64},
                         {0, 0, 64, 64},
                         angle,
                         {255, 255, 255, static_cast<Uint8>(255 * alpha)},
                         false,
                         &pivot);
        if (campaign_.baseTurns > 0) {
            const auto label = texts_.get("boss_jumps", locale_);
            float width = 200;
            auto lines = wrappedText(52, label, width + 12);
            // OnRender5d9ab0 grows until the measured block fits fifty pixels.
            while (static_cast<float>(lines.size()) * textLineHeight(52, label) > 50.f) {
                width += 10;
                lines = wrappedText(52, label, width + 12);
            }
            auto& box = renderer_.image("img/map/boss_jumps_box.png");
            renderer_.sprite(box, {962 - width, 0, 26, 78}, {0, 0, 26, 78});
            renderer_.sprite(box, {988 - width, 0, width, 78}, {26, 0, 1, 78});
            renderer_.sprite(box,
                             {988, 0, static_cast<float>(box.w - 27), 78},
                             {27, 0, static_cast<float>(box.w - 27), 78});
            for (std::size_t i = 0; i < lines.size(); ++i) {
                mapText(lines[i],
                        988 - width / 2,
                        18 + textLineHeight(52, label) * static_cast<float>(i),
                        52,
                        true,
                        {235, 245, 229, 255});
            }
            mapText(
                std::to_string(4 - campaign_.baseTurns), 1029, 13, 24, false, {255, 255, 255, 255});
        } else if (campaign_.jumping) {
            mapText(
                texts_.get("warning_boss_jump", locale_), 780, 8, 52, true, {255, 100, 100, 255});
        }
    }
    const auto mapTitle = texts_.get("map_title", locale_);
    const float titleWidth =
        std::round(renderer_.measure(localizedFont(63, mapTitle), mapTitle, textScale(63)));
    const auto sectorLabel = texts_.get("map_sector", locale_);
    const int numberX = static_cast<int>(std::round(renderer_.measure(
                            localizedFont(63, sectorLabel), sectorLabel, textScale(63)))) +
                        18;
    const int sectorX = numberX + 49;
    const auto& sectorNode = sectorGraph_.current();
    const auto sectorTitle = campaign_.active           ? texts_.get("sectorname_FINAL", locale_)
                             : sectorNode.title.empty() ? sectorName_
                                                        : sectorNode.title;
    const int sectorWidth =
        std::max(459 - sectorX,
                 static_cast<int>(std::round(renderer_.measure(
                     localizedFont(14, sectorTitle), sectorTitle, textScale(14)))) +
                     8);
    const float sectorExtra = static_cast<float>(sectorWidth - (459 - sectorX));
    auto& sectorBase = renderer_.image("img/map/side_sector.png");
    renderer_.windowFrame({332, 75, 768, 547},
                          false,
                          {{1, 1, titleWidth + 59, 45},
                           {1, 522, float(sectorBase.w) + sectorExtra, float(sectorBase.h)}});
    // InitMapTitle5c6f00 and OnRender5d9ab0 stretch their authored center columns.
    auto& title = renderer_.image("img/map/side_beaconmap.png");
    renderer_.sprite(title, {333, 76, 27, 45}, {0, 0, 27, 45});
    renderer_.sprite(title, {360, 76, titleWidth - 12, 45}, {27, 0, 1, 45});
    renderer_.sprite(title, {348 + titleWidth, 76, 44, 45}, {28, 0, 44, 45});
    text(mapTitle, 354, 84, 63, false, {25, 49, 51, 255});
    renderer_.sprite(
        sectorBase, {333, 597, 100, float(sectorBase.h)}, {0, 0, 100, float(sectorBase.h)});
    if (sectorExtra > 0) {
        renderer_.sprite(sectorBase,
                         {433, 597, sectorExtra, float(sectorBase.h)},
                         {100, 0, 1, float(sectorBase.h)});
    }
    renderer_.sprite(sectorBase,
                     {433 + sectorExtra, 597, float(sectorBase.w - 100), float(sectorBase.h)},
                     {100, 0, float(sectorBase.w - 100), float(sectorBase.h)});
    text(sectorLabel, 346, 614, 63, false, {25, 49, 51, 255});
    auto inset = [&](Rect rect) {
        renderer_.polygon({{rect.x + 3, rect.y},
                           {rect.x + rect.w - 3, rect.y},
                           {rect.x + rect.w, rect.y + 3},
                           {rect.x + rect.w, rect.y + rect.h - 3},
                           {rect.x + rect.w - 3, rect.y + rect.h},
                           {rect.x + 3, rect.y + rect.h},
                           {rect.x, rect.y + rect.h - 3},
                           {rect.x, rect.y + 3}},
                          {53, 75, 89, 255});
    };
    inset({340.f + numberX, 616, 38, 26});
    inset({340.f + sectorX, 616, float(sectorWidth), 26});
    text(secretSector_ ? "?" : std::to_string(sector_), 359.f + numberX, 612, 14, true);
    text(sectorTitle, 340.f + sectorX + sectorWidth / 2, 612, 14, true);
    if (fuel_ == 0) {
        renderer_.draw("img/map/map_fuel_text_" +
                           std::string(distressWaitTicks_ > 0 ? "waiting" : "nofuel") + ".png",
                       895,
                       525);
        renderer_.draw("img/map/side_wait_distress.png", 711, 616);
        const auto buttonColor =
            distressWaitTicks_ > 0 ? SDL_Color{125, 130, 125, 255} : SDL_Color{235, 245, 229, 255};
        renderer_.fill({730, 624, 100, 30}, buttonColor);
        renderer_.fill({956, 624, 116, 30}, buttonColor);
        text(texts_.get("button_wait", locale_), 780, 624, 63, true, {25, 49, 51, 255});
        text(texts_.get("map_distress_beacon", locale_), 846, 628, 52, false);
        text(distress_ ? "ON" : "OFF", 1014, 624, 63, true, {25, 49, 51, 255});
        if (distress_ && (animation_ % 60) < 30) {
            renderer_.draw("img/map/side_distressbeacon_redlight.png", 939, 624);
        }
    } else {
        if (canWaitAtBeacon()) {
            const SDL_Color waitColor = distressWaitTicks_ > 0 ? SDL_Color{125, 130, 125, 255}
                                                               : SDL_Color{235, 245, 229, 255};
            renderer_.fill({730, 624, 100, 30}, waitColor);
            text(texts_.get("button_wait", locale_), 780, 624, 63, true, {25, 49, 51, 255});
        }
        renderer_.draw("img/main_menus/button_cancel_base.png", 913, 616);
        const SDL_Color buttonColor = Rect{937, 624, 124, 30}.contains(mouseX_, mouseY_)
                                          ? SDL_Color{255, 230, 94, 255}
                                          : SDL_Color{235, 245, 229, 255};
        for (int row = 0; row < 30; ++row) {
            const int insetWidth = std::max(0, 3 - std::min(row, 29 - row));
            renderer_.fill({937.f + insetWidth, 624.f + row, 124.f - 2 * insetWidth, 1},
                           buttonColor);
        }
        text(texts_.get("button_cancel", locale_), 999, 624, 63, true, {25, 49, 51, 255});
        if (graph_.beacons()[graph_.current()].exit && sector_ < 8) {
            const auto button = nextSectorButtonRect();
            auto& base = renderer_.image("img/map/side_nextsector.png");
            const float half = std::floor(base.w / 2.f);
            const float extra = button.w - 226;
            const float baseX = button.x - 19;
            const float baseY = button.y - 8;
            renderer_.sprite(
                base, {baseX, baseY, half, float(base.h)}, {0, 0, half, float(base.h)});
            if (extra > 0) {
                renderer_.sprite(
                    base, {baseX + half, baseY, extra, float(base.h)}, {half, 0, 1, float(base.h)});
            }
            renderer_.sprite(base,
                             {baseX + half + extra, baseY, float(base.w) - half, float(base.h)},
                             {half, 0, float(base.w) - half, float(base.h)});
            const bool nextHovered = button.contains(mouseX_, mouseY_);
            const SDL_Color face =
                nextHovered ? SDL_Color{255, 230, 94, 255} : SDL_Color{235, 245, 229, 255};
            // StarMap ctor 0x5cc560 sets endButton.bFlashing, but StarMap::OnLoop
            // 0x5d13a0 never runs its GenericButton::OnLoop 0x573840, so the flash
            // tracker stays at time 0 and GetAlphaLevel 0x59c490 returns 0: the
            // TextButton 0x577750 overlay is invisible and the face stays solid.
            for (int row = 0; row < 36; ++row) {
                const float cornerTrim = float(std::max(0, 3 - std::min(row, 35 - row)));
                const Rect strip{
                    button.x + cornerTrim, button.y + row, button.w - 2 * cornerTrim, 1};
                renderer_.fill(strip, face);
            }
            text(texts_.get("button_nextsector", locale_),
                 button.x + button.w / 2,
                 button.y + 3,
                 63,
                 true,
                 {25, 49, 51, 255});
        }
    }
    mapTooltip();
}

void Run::mapTooltip() {
    const int hovered = hoveredBeacon();
    if (hovered < 0) {
        return;
    }
    const auto& beacon = graph_.beacons()[hovered];
    // StarMap::GetLocationText0x5c9910 / OnLoop0x5d13a0: immediate map tooltip.
    const bool bossHere = campaign_.active && !campaign_.path.empty() &&
                          campaign_.path[campaign_.position] == hovered && !campaign_.jumping;
    const bool changing = campaign_.active && std::ranges::find(campaign_.changing, hovered) !=
                                                  campaign_.changing.end();
    const bool revealed = beacon.known;
    const bool scanned =
        beacon.known && std::ranges::find(ship_.augments, "ADV_SCANNERS") != ship_.augments.end();
    pugi::xml_document event;
    event.load_string(beacon.eventXml.c_str());
    const auto node = event.document_element();
    const auto key = hovered == graph_.current() ? "map_current_loc"
                     : bossHere                  ? "map_boss_loc"
                     : beacon.exit   ? (campaign_.active ? "map_base_loc" : "map_exit_loc")
                     : changing      ? "map_rebels_loc"
                     : beacon.fleet  ? "map_fleet_loc"
                     : beacon.repair ? "map_repair_loc"
                     : revealed && beacon.store
                         ? (beacon.visited ? "map_store_loc" : "map_merchant_loc")
                     : beacon.visited                ? "map_nothing_loc"
                     : revealed && beacon.distress   ? "map_distress_loc"
                     : scanned && node.child("ship") ? "map_ship_loc"
                     : beacon.quest                  ? "map_quest_loc"
                                                     : "map_unvisited_loc";
    auto value = texts_.get(key, locale_);
    // GetLocationText 0x5c9910 appends " \n" (0x835c3b) and map_nebula_fleet_loc
    // when StarMap::bNebulaMap (a nebula sector) is set, map_nebula_loc otherwise.
    if (beacon.nebula) {
        value += " \n" + texts_.get(graph_.nebulaMap() ? "map_nebula_fleet_loc" : "map_nebula_loc",
                                    locale_);
    }
    if (scanned && !beacon.eventXml.empty()) {
        if (node) {
            const std::string_view environment =
                node.child("environment").attribute("type").as_string();
            const auto hazard = environment == "asteroid" ? "map_asteroid_loc"
                                : environment == "sun"    ? "map_sun_loc"
                                : environment == "storm"  ? "map_ion_loc"
                                : environment == "pulsar" ? "map_pulsar_loc"
                                : environment == "PDS"
                                    ? (beacon.fleet ? "map_pds_fleet" : "map_pds_loc")
                                    : "";
            if (*hazard) {
                value += " \n" + texts_.get(hazard, locale_);
            }
        }
    }
    auto& font = localizedFont(12, value);
    const auto lines = wrappedText(12, value, 275);
    float measured = 0;
    for (const auto& line : lines) {
        const auto bounds = renderer_.bounds(font, line, textScale(12));
        measured = std::max(measured, bounds.second - bounds.first);
    }
    const float lineHeight = textLineHeight(12, value);
    const float width = measured + 27;
    const float height = 18.f + static_cast<float>(lines.size()) * lineHeight;
    const float x = std::min(mouseX_, 1240 - width);
    float y = std::min(mouseY_ + 22, 686 - height);
    renderer_.fill({x, y, width, height}, {0, 0, 0, 242});
    for (const auto edge : std::array<Rect, 4>{{{x, y, width, 2},
                                                {x, y + height - 2, width, 2},
                                                {x, y, 2, height},
                                                {x + width - 2, y, 2, height}}}) {
        renderer_.fill(edge, {255, 255, 255, 255});
    }
    for (const auto& line : lines) {
        text(line, x + 15, y + 11, 12);
        y += lineHeight;
    }
}
} // namespace ftl
