#pragma once
#include "native_json.hpp"
#include <json.hpp>
#include <vector>

namespace ftl {
// Native campaign state. Original x86 BossShip/StarMap records are provenance,
// never ABI layouts. Map turns are committed jumps, not elapsed wall time.
struct Campaign {
    bool active = false;
    int base = -1;
    std::vector<int> path;
    int position = 0;
    bool jumping = false;
    bool reversed = false;
    int baseTurns = 0;
    std::vector<int> changing;
    int stage = 0;
    int nextStage = 1;
    bool fighting = false;
    bool automated = false;
    std::vector<int> crewCounts;
    float surgeElapsed = 0;
    float surgeGoal = 0;
    int surgeCount = 0;
    nlohmann::json savedBoss;
    bool victory = false;
    bool recorded = false;
    FTL_NATIVE_JSON(Campaign,
                    active,
                    base,
                    path,
                    position,
                    jumping,
                    reversed,
                    baseTurns,
                    changing,
                    stage,
                    nextStage,
                    fighting,
                    automated,
                    crewCounts,
                    surgeElapsed,
                    surgeGoal,
                    surgeCount,
                    savedBoss,
                    victory,
                    recorded)
};
} // namespace ftl
