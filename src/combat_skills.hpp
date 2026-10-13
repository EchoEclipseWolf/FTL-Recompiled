#pragma once

#include "blueprints.hpp"
#include <algorithm>
#include <string_view>

namespace ftl {
// ProjectileFactory::ProjectileFactory0x40b380 and SetFireTime0x456f30.
// This is the time from the charged frame through the end of the strip.
inline float weaponFireDuration(const EquipmentDefinition& weapon) noexcept {
    if (weapon.type == "BEAM") {
        return weapon.weaponAnimationTime *
               float(std::max(1, weapon.animationFrames - weapon.chargedFrame)) /
               float(std::max(1, weapon.animationFrames));
    }
    return weapon.type == "BOMB" || weapon.chargeLevels > 1 ? 1.f : .25f;
}

inline float weaponShotInterval(const EquipmentDefinition& weapon) noexcept {
    if (weapon.chargeLevels > 1 && weapon.type != "BEAM") {
        const int frames = std::max(1, weapon.animationFrames - weapon.chargedFrame);
        return weaponFireDuration(weapon) * float(std::max(1, frames / weapon.chargeLevels)) /
               float(frames);
    }
    return weaponFireDuration(weapon);
}

// CrewMember::CrewMember (0x47a400) uses cap 14 for human skill 5 and the
// generic skill cap 16. The six skill rows are ordered pilot, engines,
// shields, weapons, repair, combat.
constexpr int combatSkillCap(std::string_view species) noexcept {
    return species == "human" ? 14 : 16;
}

constexpr int combatSkillTier(int progress, int cap) noexcept {
    return progress >= cap ? 2 : progress >= cap / 2 ? 1 : 0;
}

constexpr float combatSkillDamageMultiplier(int tier) noexcept {
    switch (std::clamp(tier, 0, 2)) {
    case 0:
        return 1.f;
    case 1:
        return 1.1f;
    default:
        return 1.2f;
    }
}
} // namespace ftl
