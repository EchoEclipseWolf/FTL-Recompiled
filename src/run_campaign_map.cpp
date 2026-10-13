#include "run.hpp"
#include <algorithm>
#include <stdexcept>

namespace ftl {
void Run::initializeCampaign() {
    Campaign next;
    next.active = true;
    for (std::size_t i = 0; i < graph_.beacons().size(); ++i) {
        graph_.setFleet(int(i), false);
        if (graph_.beacons()[i].exit) {
            next.base = int(i);
        }
    }
    if (next.base < 0) {
        throw std::runtime_error("final_base_missing");
    }
    // GenerateMap0x5d4590: initial capture warning, then a 4..6-location
    // shortest boss path from a populated right-side grid cell to the base.
    const int changing = int(eventRandom_.next() % graph_.beacons().size());
    if (changing != next.base) {
        next.changing.push_back(changing);
    }
    const auto bossState = eventRandom_.state();
    next.path = graph_.chooseBossPath(eventRandom_, next.base);
    if (generationTrace_.contains("attempts") && !generationTrace_["attempts"].empty()) {
        auto& trace = generationTrace_["attempts"].back();
        trace["boss_state"] = bossState;
        trace["base"] = next.base;
        trace["boss_path"] = next.path;
    }
    if (next.path.empty()) {
        throw std::runtime_error("final_boss_path_generation_exhausted");
    }
    const int initial = next.path.front();
    const bool nebula = graph_.beacons()[initial].nebula;
    const std::string fleetEvent = nebula      ? "FLEET_EASY_NEBULA"
                                   : advanced_ ? "FLEET_EASY_DLC"
                                               : "FLEET_EASY";
    graph_.setEvent(initial, fleetEvent, nebula, false);
    graph_.setEventXml(initial,
                       materializeEvent(events_.event(fleetEvent),
                                        eventRandom_,
                                        advanced_,
                                        usedVoyageEvents_,
                                        [this](pugi::xml_node node, RandomStream& random) {
                                            freezeVoyageEvent(node, random);
                                        }));
    graph_.setFleet(initial, true);
    graph_.setStore(initial, false);
    graph_.setDistress(initial, false);
    campaign_ = std::move(next);
}

void Run::campaignTurn() {
    if (!campaign_.active || campaign_.victory || campaign_.path.empty()) {
        return;
    }
    auto& state = campaign_;
    const auto capture = [&](int id) {
        if (graph_.beacons()[id].fleet) {
            return;
        }
        const bool nebula = graph_.beacons()[id].nebula;
        const std::string event = nebula      ? "FLEET_EASY_NEBULA"
                                  : advanced_ ? "FLEET_EASY_DLC"
                                              : "FLEET_EASY";
        graph_.setEvent(id, event, nebula, false);
        graph_.setEventXml(id,
                           materializeEvent(events_.event(event),
                                            eventRandom_,
                                            advanced_,
                                            usedVoyageEvents_,
                                            [this](pugi::xml_node node, RandomStream& random) {
                                                freezeVoyageEvent(node, random);
                                            }));
        graph_.setFleet(id, true);
        graph_.setStore(id, false);
        graph_.setDistress(id, false);
    };
    const auto reverse = [&] {
        std::reverse(state.path.begin(), state.path.end());
        state.position = int(state.path.size()) - 1 - state.position;
        state.reversed = !state.reversed;
    };
    const bool atBase = state.path[state.position] == state.base;
    if (!atBase) {
        if (state.position == int(state.path.size()) - 1) {
            reverse();
        }
        state.baseTurns = 0;
        if (state.jumping) {
            ++state.position;
            if (state.path[state.position] == state.base) {
                state.baseTurns = 1;
            } else {
                capture(state.path[state.position]);
            }
        }
        state.jumping = !state.jumping;
    } else if (state.jumping && state.baseTurns > 0) {
        state.jumping = false;
    } else if (state.jumping) {
        ++state.position;
        state.jumping = false;
        state.baseTurns = 0;
        capture(state.path[state.position]);
    } else {
        if (state.baseTurns > 0) {
            state.baseTurns = std::min(4, state.baseTurns + 1);
        }
        state.jumping = true;
    }
    if (state.reversed && !state.jumping) {
        reverse();
    }
    if (state.position == int(state.path.size()) - 1) {
        state.jumping = false;
    }
    // UpdateBoss0x5d0b80 commits last turn's warning nodes, then samples
    // the next threats. The second sample may repeat the first in source.
    for (int id : state.changing) {
        capture(id);
    }
    state.changing.clear();
    std::vector<int> available;
    for (std::size_t i = 0; i < graph_.beacons().size(); ++i) {
        if (int(i) != state.base && !graph_.beacons()[i].fleet) {
            available.push_back(int(i));
        }
    }
    if (!available.empty()) {
        state.changing.push_back(available[eventRandom_.next() % available.size()]);
        if (available.size() > 1 && (state.position > 1 || state.reversed)) {
            const int id = available[eventRandom_.next() % available.size()];
            if (id != state.changing.front()) {
                state.changing.push_back(id);
            }
        }
    }
}

bool Run::campaignArrival() {
    if (!campaign_.active) {
        return false;
    }
    const int current = graph_.current();
    const int boss = campaign_.path.at(static_cast<std::size_t>(campaign_.position));
    if (campaign_.baseTurns == 4 && boss != current) {
        terminal(false, "gameover_fedbase");
        return true;
    }
    if (current == boss) {
        startBossStage();
        enterEvent(events_.event("BOSS_TEXT_" + std::to_string(campaign_.stage)), true);
        eventId_ = "BOSS_TEXT_" + std::to_string(campaign_.stage);
        // BossShip::GetEvent0x406ee0 appends status {limit,sensors,2,player}.
        if (auto* sensors = system(7)) {
            sensors->powerLimit = 2;
        }
        return true;
    }
    if (graph_.beacons()[current].fleet) {
        const auto& destination = graph_.beacons()[current];
        restoredEvents_.emplace_back();
        if (!restoredEvents_.back().load_string(destination.eventXml.c_str())) {
            throw std::runtime_error("invalid_campaign_fleet_event");
        }
        prepareBeaconImages(restoredEvents_.back().document_element());
        enterEvent(restoredEvents_.back().document_element(), true);
        eventId_ = destination.event;
        return true;
    }
    return false;
}

void Run::forceBossJump() {
    campaign_.jumping = true;
    if (!campaign_.reversed && campaign_.position == int(campaign_.path.size()) - 1) {
        std::reverse(campaign_.path.begin(), campaign_.path.end());
        campaign_.position = 0;
        campaign_.reversed = true;
        campaign_.baseTurns = 0;
    }
}

void Run::terminal(bool victory, const std::string& textKey) {
    if (defeated_) {
        return;
    }
    defeated_ = true;
    campaign_.victory = victory && !tutorialOwned_;
    campaign_.fighting = false;
    campaign_.savedBoss = nullptr;
    hostile_ = false;
    mapOpen_ = false;
    panel_.clear();
    cancelAim();
    event_ = true;
    openingEvent_ = false;
    eventId_ = victory ? "P10_VICTORY" : "P10_BASE_LOSS";
    eventText_ = texts_.get(textKey, locale_);
    pendingResources_ = {};
    choices_.clear();
    if (campaign_.victory) {
        progressionRun_.tutorial = tutorialOwned_;
        progression_.victory(progressionRun_);
        audio_.sound("victory");
    }
    resetEventChoiceDelay();
}
} // namespace ftl
