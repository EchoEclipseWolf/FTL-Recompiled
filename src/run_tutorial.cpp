#include "run.hpp"
#include <algorithm>
#include <cmath>

namespace ftl {
void Run::startTutorial(const Hangar& hangar,
                        const std::string& locale,
                        std::uint32_t seed,
                        bool preserveSession) {
    start(hangar, locale, seed, preserveSession);
    // ShipBuilder::QuickPlayShip4ddca0 selects the authored tutorial blueprint.
    ship_ = catalog_.ship("PLAYER_SHIP_TUTORIAL");
    // QuickPlay constructs another player ShipManager. AddSystem49ca00
    // supplies fixed primary stations when its raw template leaves slot=-1,
    // just as the ordinary Hangar ship does before entering this run.
    for (auto& definition : ship_.systems) {
        if (definition.installed) {
            definition = playerSystemDefinition(std::move(definition));
        }
    }
    name_ = ship_.name.reference ? texts_.get(ship_.name.value, locale_) : ship_.name.value;
    difficulty_ = 0;
    // ResetScrapLevel490e20 grants30 on Easy; start(hangar) sampled the prior difficulty.
    scrap_ = 30;
    advanced_ = false;
    hull_ = ship_.hull;
    fuel_ = 16;
    missiles_ = ship_.missiles;
    parts_ = ship_.droneParts;
    systems_.clear();
    int remaining = ship_.reactor;
    for (const auto& definition : ship_.systems) {
        if (!definition.installed) {
            continue;
        }
        const bool passive = (definition.id >= 6 && definition.id <= 8) || definition.id == 12;
        const int request = definition.id == 3 || definition.id == 4 ? 0
                            : definition.id == 0                     ? 2
                                                                     : 1;
        const int assigned =
            passive ? definition.power : std::min({request, definition.power, remaining});
        if (!passive) {
            remaining -= assigned;
        }
        systems_.push_back({definition, assigned, definition.power, 0});
    }
    crew_.clear();
    refreshCrewNames();
    for (const auto& group : ship_.crew) {
        for (int index = 0; index < group.count; ++index) {
            if (!hireCrew(group.species)) {
                throw std::runtime_error("tutorial_crew_creation_failed");
            }
        }
    }
    doors_.assign(ship_.doors.size(), false);
    doorFrames_.assign(doors_.size(), 0);
    doorTimers_.assign(doors_.size(), 0);
    fakeDoors_.assign(doors_.size(), false);
    weaponPower_.assign(ship_.weapons.size(), false);
    weaponDesired_.assign(ship_.weapons.size(), false);
    combatWeapons_.assign(ship_.weapons.size(), {});
    combatDrones_.clear();
    for (const auto& drone : ship_.drones) {
        combatDrones_.push_back({.blueprint = drone});
        initializeCrewDroneAnimation(combatDrones_.back(), eventRandom_);
    }
    ensureInventorySlots();
    roomOxygen_.clear();
    for (const auto& shape : ship_.rooms) {
        roomOxygen_[shape.id] = 100;
    }
    shieldBubbles_ = system(0) ? system(0)->power / 2 : 0;
    // StarMap::GenerateEvents5d2100 supplies tutorial encounters instead of normal voyage events.
    // Its tutorial branch clears currentNebulas and skips normal GenerateNebulas.
    mapNebulas_.clear();
    for (std::size_t index = 0; index < graph_.beacons().size(); ++index) {
        const int id = static_cast<int>(index);
        const bool exit = graph_.beacons()[index].exit;
        graph_.setEvent(id, "TUTORIAL_ENEMY", false, exit);
        graph_.setEventXml(id, "");
        graph_.setStore(id, false);
        graph_.setDistress(id, false);
    }
    event_ = openingEvent_ = false;
    eventId_ = "TUTORIAL_START";
    choices_.clear();
    userPause_ = false;
    tutorialOwned_ = true;
    tutorial_.start();
    tutorialEntry();
    tutorialUpdate();
    enterEvent(events_.event("TUTORIAL_START"));
    resetResourceFeedback();
}

TutorialObservation Run::tutorialObservation() const {
    TutorialObservation result;
    const auto* engines = system(1);
    const auto* pilot = system(6);
    result.enginesPowered = engines && engines->power > 0;
    result.enginesRepaired = engines && engines->health == engines->definition.power;
    for (const auto& member : crew_) {
        if (member.health <= 0 || member.intruder) {
            continue;
        }
        result.crewSelected = result.crewSelected || member.selected;
        result.crewInEngines =
            result.crewInEngines || (engines && member.room == engines->definition.room);
        result.crewInPiloting =
            result.crewInPiloting || (pilot && member.room == pilot->definition.room);
    }
    for (const auto& door : ship_.doors) {
        if ((door.roomA == 9 && door.roomB < 0) || (door.roomB == 9 && door.roomA < 0)) {
            result.roomNineAirlockOpen = result.roomNineAirlockOpen || doors_[door.id] ||
                                         voyageDoorForcedTicks_.contains(door.id);
        }
    }
    const auto hazard = playerHazards_.find(9);
    result.roomNineFireCleared = hazard == playerHazards_.end() || hazard->second.fires == 0;
    result.mapOpen = mapOpen_;
    result.paused = userPause_;
    // CheckState410e09 waits for current_target to clear, after native explosion cleanup.
    result.enemyAbsent = !enemy_.present || (!hostile_ && enemy_.rewardCommitted);
    // A tutorial MenuScreen hides, but does not close, the ship screen.
    result.upgradesOpen = visibleOrPausedPanel() == "upgrades";
    result.inventoryOpen = visibleOrPausedPanel() == "equipment";
    for (std::size_t index = 0; index < ship_.weapons.size(); ++index) {
        const auto& definition = catalog_.equipment(ship_.weapons[index]);
        const bool powered = weaponPower_[index];
        const bool fired = combatWeapons_[index].firedOnce;
        if (definition.type == "MISSILES") {
            result.missileFired = result.missileFired || fired;
        } else {
            result.nonMissilePowered = result.nonMissilePowered || powered;
            result.nonMissileFired = result.nonMissileFired || fired;
        }
        if (definition.type == "BEAM") {
            result.beamPowered = result.beamPowered || powered;
        }
    }
    return result;
}

void Run::tutorialEntry() {
    const auto& name = tutorial_.current().name;
    // TutorialManager::AdvanceState411010 owns these one-time lesson changes.
    if (name == "POWER_SYSTEM_1") {
        if (auto* engines = system(1)) {
            engines->power = std::max(0, engines->power - 2);
        }
    } else if (name == "DAMAGE_SYSTEM_1") {
        if (auto* engines = system(1)) {
            engines->health = std::max(0, engines->health - 2);
            engines->power = std::min(engines->power, engines->health);
        }
    } else if (name == "DAMAGE_SYSTEM_2") {
        for (int index = 1; index <= 2; ++index) {
            // AddCrewMember4a3ee0 still consumes GetCrewBlueprint5e9690's
            // unused base name before the supplied tutorial name replaces it.
            (void)catalog_.unusedCrewName(locale_, catalog_.currentCrewNames(), [&] {
                return eventRandom_.next();
            });
            // CrewAnimation479840 initializes its ranged timer before the
            // CrewMember47a400 constructor resolves unspecified palette layers.
            const float shotGoal = float(eventRandom_.next() % 300 + 1000) / 1000.f;
            int appearance = index == 1 ? 0 : 1;
            int stride = 2;
            for (const auto& layer : catalog_.appearance("human").layers) {
                if (!layer.empty()) {
                    appearance += int(eventRandom_.next() % layer.size()) * stride;
                    stride *= int(layer.size());
                }
            }
            RunCrew member;
            member.shootGoal = shotGoal;
            const auto crewName =
                texts_.get("tutorial_extra_crew_" + std::to_string(index), locale_);
            member.appearance = {crewName, "human", appearance, -1, 0, crewName};
            member.recordMale = index == 1;
            member.health = crewMaximumHealth("human");
            // AdvanceState411572/41167b explicitly requests room1, beside the pilot.
            placeVoyageCrew(member, 1);
            assignPhysicalCrewOrder(member);
            member.savedRoom = member.room;
            member.savedSlot = member.slot;
            crew_.push_back(std::move(member));
            refreshCrewNames();
        }
        previousCrew_.clear();
        updateManning();
    } else if (name == "FIRE_PC_1") {
        (void)room(9);
        // AdvanceState 0x411010 calls ShipManager::StartFire 0x497080,
        // which draws two random tile coordinates, including duplicates.
        startRoomFire(0, 9, 2);
    } else if (name == "COMBAT_4") {
        // AdvanceState4117ae triggers GetEvent40f7c0 only on COMBAT_4.
        userPause_ = false;
        enterEvent(events_.event("TUTORIAL_MISSILE"));
    }
}

void Run::tutorialUpdate() {
    if (!tutorialOwned_ || !open_ || event_ || jumpTicks_ > 0 || arrivalTicks_ > 0) {
        return;
    }
    // TutorialManager::OnLoop 0x411950 keeps the missile lesson recoverable
    // after the player spends the last missile.
    if (missiles_ <= 0) {
        missiles_ += 10;
    }
    auto& shipPanel = pausedShipPanel_.empty() ? panel_ : pausedShipPanel_;
    if (tutorial_.current().name == "INVENTORY_1" &&
        (shipPanel == "crew" || shipPanel == "upgrades")) {
        // CheckState 0x410160 observes any open ship-info tab, then selects
        // equipment. It does not close a panel on entering this lesson.
        shipPanel = "equipment";
    }
    scoreScrap_ = scoreShips_ = scoreBeacons_ = 0;
    if (tutorial_.update(tutorialObservation())) {
        tutorialEntry();
        (void)tutorial_.update(tutorialObservation());
    }
}

std::vector<std::string> Run::tutorialLines() const {
    return wrappedText(12, texts_.get(tutorial_.current().textId, locale_), 308);
}

Rect Run::tutorialContinueRect() const {
    const auto overlay = tutorial_.overlay();
    const auto lineHeight = textLineHeight(12, texts_.get(tutorial_.current().textId, locale_));
    const auto height =
        std::max(212.f, float(int(float(tutorialLines().size()) * lineHeight + 22)));
    return {
        static_cast<float>(overlay.x + 83), static_cast<float>(overlay.y) + height + 3, 170, 32};
}

bool Run::tutorialClick(float x, float y, bool right) {
    if (!tutorialOwned_ || event_ || jumpTicks_ > 0 || arrivalTicks_ > 0 || right ||
        !tutorialContinueRect().contains(x, y)) {
        return false;
    }
    if (tutorial_.continueStep(tutorialObservation())) {
        if (!tutorial_.running()) {
            // TutorialManager::MouseClick411f00 sets quit; menu handles the owned run.
            gameOverCommand_ = 3;
        } else {
            tutorialEntry();
            (void)tutorial_.update(tutorialObservation());
            tutorialUpdate();
        }
    }
    return true;
}

void Run::tutorialRender() {
    if (!tutorialOwned_ || !tutorial_.running() || event_ || jumpTicks_ > 0 || arrivalTicks_ > 0) {
        return;
    }
    const auto& lesson = tutorial_.current().name;
    if (lesson == "COMBAT_1" || lesson == "COMBAT_5") {
        // OnLoop411950 chooses one-based slots1/3; ArmamentBox4f9600 pulses
        // only their required power bars red over a0.75s unscaled UI loop.
        const int slot = lesson == "COMBAT_1" ? 0 : 2;
        const int dense = logicalArmamentIndex("weapon", slot);
        if (dense >= 0) {
            float weaponX = 90;
            for (int id : {0, 1, 5, 2, 9, 10, 11, 13, 14, 15}) {
                if (system(id)) {
                    weaponX += 36;
                }
            }
            const float phase =
                std::fmod(static_cast<float>(animation_ % 45) + uiFraction_, 45.f) / 45.f;
            const Uint8 alpha = static_cast<Uint8>((1.f - std::abs(2.f * phase - 1.f)) * 255.f);
            const auto& definition = catalog_.equipment(ship_.weapons[dense]);
            for (int bar = 0; bar < definition.power; ++bar) {
                renderer_.fill({weaponX + 28 + 97 * slot, 651.f - 8 * bar, 16, 7},
                               {255, 0, 0, alpha});
            }
        }
    }
    const auto overlay = tutorial_.overlay();
    const auto button = tutorialContinueRect();
    // WindowFrame578c90 expands its logical rectangle by seven pixels per side.
    renderer_.windowFrame({static_cast<float>(overlay.x),
                           static_cast<float>(overlay.y - 7),
                           static_cast<float>(overlay.frameWidth + 14),
                           button.y - overlay.y - 3 + 14});
    float y = static_cast<float>(overlay.y + overlay.textOffsetY);
    const float lineHeight = textLineHeight(12, texts_.get(tutorial_.current().textId, locale_));
    for (const auto& line : tutorialLines()) {
        text(line, static_cast<float>(overlay.x + overlay.textOffsetX), y, 12);
        y += lineHeight;
    }
    if (tutorial_.canContinue(tutorialObservation())) {
        renderer_.draw("img/tutorial/button_continue_base.png", button.x - 23, button.y - 7);
        // TextButton::InitPrimitives577040 uses the authored four-pixel corner inset.
        const SDL_Color face = button.contains(mouseX_, mouseY_) ? SDL_Color{255, 255, 50, 255}
                                                                 : SDL_Color{255, 255, 255, 255};
        for (int row = 0; row < 4; ++row) {
            const float trim = static_cast<float>(4 - row);
            renderer_.fill({button.x + trim, button.y + row, button.w - 2 * trim, 1}, face);
            renderer_.fill({button.x + trim, button.y + button.h - 1 - row, button.w - 2 * trim, 1},
                           face);
        }
        renderer_.fill({button.x, button.y + 4, button.w, button.h - 8}, face);
        // TextButton577750/SetFont575db0: font63 has textYOffset4 and
        // centers its measured line height inside the 32-pixel face.
        const auto buttonLineHeight = textLineHeight(63, texts_.get("tutorial_continue", locale_));
        text(texts_.get("tutorial_continue", locale_),
             button.x + button.w / 2,
             button.y + button.h / 2 + 4 - buttonLineHeight / 2,
             63,
             true,
             {25, 49, 51, 255});
    }
    const bool beamEquipped = std::ranges::any_of(ship_.weapons, [&](const auto& weapon) {
        return catalog_.equipment(weapon).type == "BEAM";
    });
    for (const auto& arrow : tutorial_.current().arrows) {
        // OnLoop 0x411950 clears these arrows once the required beam exists.
        if (lesson == "INVENTORY_2" && beamEquipped) {
            break;
        }
        SDL_FPoint offset{};
        if (arrow.relative == "upgrades") {
            offset = {managementPanelX(), 75};
        } else if (arrow.relative == "enemy") {
            offset = {enemyOriginX(), 170};
        }
        auto& image = renderer_.image("img/tutorial/arrow.png");
        renderer_.sprite(image,
                         {offset.x + arrow.x, offset.y + arrow.y, 164, 38},
                         {0, 0, static_cast<float>(image.w), static_cast<float>(image.h)},
                         arrow.angle);
        // TutorialManager40f510/414de0: a second arrow fades over a 0.75s UI loop.
        // animation_ is unscaled presentation time and advances while gameplay is paused.
        const float phase =
            std::fmod(static_cast<float>(animation_ % 45) + uiFraction_, 45.f) / 45.f;
        const Uint8 alpha = static_cast<Uint8>((1.f - std::abs(2.f * phase - 1.f)) * 255.f);
        auto& pulse = renderer_.image("img/tutorial/arrow2.png");
        renderer_.sprite(pulse,
                         {offset.x + arrow.x, offset.y + arrow.y, 164, 38},
                         {0, 0, static_cast<float>(pulse.w), static_cast<float>(pulse.h)},
                         arrow.angle,
                         {255, 255, 255, alpha});
    }
}

nlohmann::json Run::tutorialState() const {
    if (!tutorialOwned_) {
        return {{"running", false}};
    }
    const auto button = tutorialContinueRect();
    return {{"running", true},
            {"state", tutorial_.current().name},
            {"index", tutorial_.index()},
            {"count", tutorial_.steps().size()},
            {"can_continue", tutorial_.canContinue(tutorialObservation())},
            {"text_id", tutorial_.current().textId},
            {"text", texts_.get(tutorial_.current().textId, locale_)},
            {"continue_rect", {button.x, button.y, button.w, button.h}},
            {"quit_requested", !tutorial_.running()}};
}
} // namespace ftl
