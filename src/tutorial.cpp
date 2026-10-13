#include "tutorial.hpp"
#include <pugixml.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ftl {
namespace {
// These checks change a false XML continue flag into a manual Continue button,
// rather than automatically skipping the explanation (CheckState410160).
bool manualGate(std::string_view name) {
    return name == "POWER_SYSTEM_2" || name == "DAMAGE_SYSTEM_4" || name == "FIRE_END" ||
           name == "DAMAGE_SYSTEM_6";
}
} // namespace

Tutorial::Tutorial(const Archive& archive) {
    const auto bytes = archive.read("data/tutorial.xml");
    pugi::xml_document document;
    if (!document.load_buffer(bytes.data(), bytes.size())) {
        throw std::runtime_error("invalid_tutorial_script");
    }
    const auto root = document.child("FTL");
    const auto order = root.find_child_by_attribute("order", "type", "PC");
    for (const auto row : order.children("step")) {
        TutorialStep step;
        step.name = row.text().as_string();
        const auto definition = root.find_child_by_attribute("step", "name", step.name.c_str());
        auto text = definition.child("PC");
        if (!text) {
            text = definition.child("default");
        }
        if (!definition || !text || !text.attribute("id")) {
            throw std::runtime_error("incomplete_tutorial_script");
        }
        step.textId = text.attribute("id").as_string();
        step.continuation = definition.child("continue").text().as_bool();
        for (const auto arrow : definition.children("arrow")) {
            const std::string_view platform = arrow.attribute("type").as_string();
            if (platform.empty() || platform == "PC") {
                step.arrows.push_back({arrow.child("x").text().as_int(),
                                       arrow.child("y").text().as_int(),
                                       arrow.child("angle").text().as_int(),
                                       arrow.attribute("relative").as_string()});
            }
        }
        steps_.push_back(std::move(step));
    }
    if (steps_.empty()) {
        throw std::runtime_error("missing_tutorial_order");
    }
}

void Tutorial::start() {
    index_ = 0;
    running_ = true;
    continuationUnlocked_ = false;
    entering_ = true;
}

void Tutorial::stop() {
    running_ = false;
}

const TutorialStep& Tutorial::current() const {
    return steps_.at(index_);
}

TutorialOverlay Tutorial::overlay() const {
    // TutorialManager::OnRender414de0 positions the same original frame around
    // the map, combat target and ship-management panels.
    TutorialOverlay result;
    const auto& name = current().name;
    if (name == "STAR_MAP_1") {
        result.x = 50;
        result.y = 200;
    } else if (name == "UPGRADES_1" || name == "UPGRADES_1_2") {
        result.x = 0;
        result.y = 230;
    } else if (name == "INVENTORY_2") {
        result.x = 0;
        result.y = 185;
    } else {
        bool afterMap = false;
        for (const auto& step : steps_) {
            if (step.name == "STAR_MAP_1") {
                afterMap = true;
            }
            if (step.name == "INVENTORY_1") {
                break;
            }
            if (step.name == name && afterMap) {
                result.x = 520;
                result.y = 3;
                break;
            }
        }
    }
    return result;
}

bool Tutorial::ready(const TutorialObservation& o) const {
    const auto& name = current().name;
    if (name == "POWER_SYSTEM_2") {
        return o.enginesPowered;
    }
    if (name == "DAMAGE_SYSTEM_2") {
        return o.crewSelected;
    }
    if (name == "DAMAGE_SYSTEM_3") {
        return o.crewInEngines;
    }
    if (name == "DAMAGE_SYSTEM_4") {
        return o.enginesRepaired;
    }
    if (name == "FIRE_PC_1") {
        return o.roomNineAirlockOpen;
    }
    if (name == "FIRE_END") {
        return o.roomNineFireCleared;
    }
    if (name == "DAMAGE_SYSTEM_6") {
        return o.crewInPiloting;
    }
    if (name == "DAMAGE_SYSTEM_7") {
        return o.mapOpen;
    }
    if (name == "STAR_MAP_1") {
        return !o.mapOpen;
    }
    if (name == "COMBAT_0") {
        return o.paused;
    }
    if (name == "COMBAT_1") {
        return o.nonMissilePowered;
    }
    if (name == "COMBAT_2") {
        return o.nonMissileFired;
    }
    if (name == "COMBAT_4") {
        return o.missileFired;
    }
    if (name == "COMBAT_4_6") {
        return o.enemyAbsent;
    }
    if (name == "COMBAT_5") {
        return o.upgradesOpen;
    }
    if (name == "INVENTORY_1") {
        return o.inventoryOpen;
    }
    if (name == "INVENTORY_2") {
        return !o.inventoryOpen;
    }
    if (name == "UPGRADES_1_2") {
        return !o.upgradesOpen;
    }
    if (name == "UPGRADES_2") {
        return o.beamPowered;
    }
    // Manual XML states are advanced only by Continue. No invented fallback predicate.
    return false;
}

bool Tutorial::canContinue(const TutorialObservation& observation) const {
    return running_ && (current().continuation || continuationUnlocked_ ||
                        (manualGate(current().name) && ready(observation)));
}

bool Tutorial::advance() {
    continuationUnlocked_ = false;
    entering_ = true;
    if (index_ + 1 >= steps_.size()) {
        stop();
    } else {
        ++index_;
    }
    return true;
}

bool Tutorial::continueStep(const TutorialObservation& observation) {
    return canContinue(observation) && advance();
}

bool Tutorial::update(const TutorialObservation& observation) {
    if (!running_) {
        return false;
    }
    // AdvanceState411010 makes an already-satisfied new state manual, so its
    // instructions remain visible instead of being skipped on the next loop.
    if (entering_) {
        entering_ = false;
        continuationUnlocked_ = ready(observation);
    }
    if (continuationUnlocked_) {
        return false;
    }
    if (running_ && manualGate(current().name)) {
        continuationUnlocked_ = continuationUnlocked_ || ready(observation);
        return false;
    }
    return running_ && !current().continuation && !manualGate(current().name) &&
           ready(observation) && advance();
}
} // namespace ftl
