#pragma once
#include "archive.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace ftl {
struct TutorialArrow {
    int x = 0;
    int y = 0;
    int angle = 0;
    std::string relative;
};

struct TutorialStep {
    std::string name;
    std::string textId;
    bool continuation = false;
    std::vector<TutorialArrow> arrows;
};

struct TutorialOverlay {
    int x = 900;
    int y = 50;
    int frameWidth = 323;
    int textWidth = 308;
    int textOffsetX = 18;
    int textOffsetY = 15;
    int font = 12;
};

// Native observations of the predicates in TutorialManager::CheckState410160.
// The caller owns all gameplay changes and samples these after logic updates.
struct TutorialObservation {
    bool crewSelected = false;
    bool crewInEngines = false;
    bool enginesRepaired = false;
    bool enginesPowered = false;
    bool roomNineAirlockOpen = false;
    bool roomNineFireCleared = false;
    bool crewInPiloting = false;
    bool mapOpen = false;
    bool paused = false;
    bool nonMissilePowered = false;
    bool nonMissileFired = false;
    bool missileFired = false;
    bool beamPowered = false;
    bool enemyAbsent = false;
    bool upgradesOpen = false;
    bool inventoryOpen = false;
};

// Source-driven desktop script. This helper has no rendering, RNG or clock side effects.
class Tutorial {
    std::vector<TutorialStep> steps_;
    std::size_t index_ = 0;
    bool running_ = false;
    bool continuationUnlocked_ = false;
    bool entering_ = true;
    bool ready(const TutorialObservation& observation) const;
    bool advance();

public:
    explicit Tutorial(const Archive& archive);
    void start();
    void stop();

    bool running() const {
        return running_;
    }

    std::size_t index() const {
        return index_;
    }

    const std::vector<TutorialStep>& steps() const {
        return steps_;
    }

    const TutorialStep& current() const;
    TutorialOverlay overlay() const;
    bool canContinue(const TutorialObservation& observation) const;
    // Returns true only on a state transition. The caller applies entry hooks once.
    bool continueStep(const TutorialObservation& observation);
    bool update(const TutorialObservation& observation);
};
} // namespace ftl
