#include "run.hpp"
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <map>
#include <stdexcept>

namespace ftl {
void Run::applyVoyageCrewSkills(RunCrew& member, const nlohmann::json& row) const {
    constexpr std::array<int, 6> blueprintCaps{30, 30, 110, 130, 36, 16};
    constexpr std::array<int, 6> humanCaps{26, 26, 100, 116, 32, 14};
    const auto& caps = member.appearance.species == "human" ? humanCaps : blueprintCaps;
    std::array<int, 6> progress{};
    if (row.contains("skill_progress")) {
        const auto blueprintProgress = nativeValue<std::array<int, 6>>(row.at("skill_progress"));
        for (std::size_t skill = 0; skill < progress.size(); ++skill) {
            if (blueprintProgress[skill] < 0 || blueprintProgress[skill] > blueprintCaps[skill]) {
                throw std::runtime_error("invalid_voyage_crew_skill_progress");
            }
            progress[skill] = blueprintProgress[skill] * caps[skill] / blueprintCaps[skill];
        }
    } else {
        // Legacy native event rows did not retain a blueprint skill vector.
        const int all = row.value("all_skills", 0);
        constexpr std::array names{"pilot", "engines", "shields", "weapons", "repair", "combat"};
        for (std::size_t skill = 0; skill < progress.size(); ++skill) {
            progress[skill] =
                std::clamp(std::max(all, row.value(names[skill], 0)), 0, 2) * caps[skill] / 2;
        }
    }
    for (std::size_t skill = 0; skill < 3; ++skill) {
        member.manningSkillProgress[skill] = progress[skill];
        member.manningSkill[skill] = progress[skill] * 2 / caps[skill];
    }
    member.weaponSkillProgress = progress[3];
    member.weaponSkill = progress[3] * 2 / caps[3];
    member.repairSkillProgress = progress[4];
    member.repairSkill = progress[4] * 2 / caps[4];
    member.combatSkillProgress = progress[5];
    member.combatSkill = progress[5] * 2 / caps[5];
}

void Run::stageCombatCrewReward(const std::string& requestedSpecies,
                                bool eventTemplate,
                                const std::string& explicitName) {
    // GetRandomCrew5e8fa0/RandomItems72e310 use the live rarity map,
    // including DLC disabling and the destination sector's overrides.
    struct WeightedCrew {
        std::string species;
        int weight = 0;
        int total = 0;
    };

    std::vector<WeightedCrew> heap(1);
    for (const auto& [id, weight] : catalog_.crewRewardPool(advanced_, sectorRarities())) {
        if (weight < 1 || weight > 5) {
            throw std::runtime_error("invalid_combat_reward_crew_rarity");
        }
        heap.push_back({id, weight, weight});
    }
    if (heap.size() < 2) {
        throw std::runtime_error("empty_combat_reward_crew_pool");
    }
    for (std::size_t index = heap.size() - 1; index > 1; --index) {
        heap[index / 2].total += heap[index].total;
    }

    // Prepare all archive data before advancing the staged reward stream.
    auto random = eventRandom_;
    std::size_t index = 1;
    if (requestedSpecies.empty()) {
        int choice = int(random.next() % std::uint32_t(heap[1].total)) + 1;
        while (choice > heap[index].weight) {
            choice -= heap[index].weight;
            index *= 2;
            if (choice > heap[index].total) {
                choice -= heap[index].total;
                ++index;
            }
        }
    }
    const auto& selectedSpecies = requestedSpecies.empty() ? heap[index].species : requestedSpecies;
    (void)catalog_.appearance(selectedSpecies);
    std::vector<std::string> usedNames;
    usedNames.reserve(crew_.size());
    for (const auto& member : crew_) {
        usedNames.push_back(member.appearance.fullName);
    }
    const auto drawName = [&] {
        return catalog_.unusedCrewName(locale_, usedNames, [&] {
            return random.next();
        });
    };
    // GetCrewBlueprint5e9690 consumes a base name for an explicit race.
    // CreateEvent then chooses colors and replaces that name unless authored.
    bool baseMale = true;
    if (eventTemplate && !requestedSpecies.empty()) {
        baseMale = drawName().second;
    }
    int colorAppearance = 0;
    if (eventTemplate) {
        int stride = selectedSpecies == "human" ? 2 : 1;
        for (const auto& layer : catalog_.appearance(selectedSpecies).layers) {
            if (!layer.empty()) {
                colorAppearance += static_cast<int>(random.next() % layer.size()) * stride;
                stride *= static_cast<int>(layer.size());
            }
        }
    }
    const auto selectedName = explicitName.empty() ? drawName() : std::pair{explicitName, baseMale};
    enemy_.rewardCrewSpecies = selectedSpecies;
    enemy_.rewardCrewFullName = selectedName.first;
    enemy_.rewardCrewName =
        catalog_.shortCrewName(selectedName.first, [&](const std::string& value) {
            const auto [left, right] =
                renderer_.bounds(localizedFont(6, value), value, textScale(6));
            return right - left;
        });
    enemy_.rewardCrewMale = selectedName.second;
    enemy_.rewardCrewAppearance =
        colorAppearance + (selectedSpecies == "human" && !selectedName.second ? 1 : 0);
    enemy_.rewardCrewColorsPrepared = eventTemplate;
    enemy_.rewardCrewPending = true;
    enemy_.rewardCrewOverflow = false;
    enemy_.rewardUnsupportedCrew = false;
    eventRandom_ = random;
}

bool Run::hireCrew(const std::string& species) {
    if (crewRoster().size() >= std::size_t(devCrewLimit_) || enemy_.rewardCrewPending) {
        return false;
    }
    const auto before = eventRandom_;
    const auto previousNames = catalog_.currentCrewNames();
    stageCombatCrewReward(species);
    commitCombatCrewReward();
    if (enemy_.rewardCrewPending) {
        enemy_.rewardCrewPending = false;
        enemy_.rewardCrewOverflow = false;
        eventRandom_ = before;
        catalog_.setUsedCrewNames(previousNames);
        return false;
    }
    return true;
}

void Run::commitCombatCrewReward() {
    if (!enemy_.rewardCrewPending) {
        return;
    }
    // ModifyResources5f0290 constructs event reward actors even when already
    // full. CommandGui5220b0 then opens the mandatory overfull manifest.
    RunCrew member;
    member.appearance = {enemy_.rewardCrewName,
                         enemy_.rewardCrewSpecies,
                         enemy_.rewardCrewAppearance,
                         -1,
                         0,
                         enemy_.rewardCrewFullName};
    member.recordMale = enemy_.rewardCrewMale;
    initializeCrewAnimation(member, eventRandom_);
    if (!enemy_.rewardCrewColorsPrepared) {
        int stride = member.appearance.species == "human" ? 2 : 1;
        for (const auto& layer : catalog_.appearance(member.appearance.species).layers) {
            if (layer.empty()) {
                continue;
            }
            member.appearance.appearance += int(eventRandom_.next() % layer.size()) * stride;
            stride *= static_cast<int>(layer.size());
        }
    }
    member.health = crewMaximumHealth(member.appearance.species);
    const auto* pilot = system(6);
    placeVoyageCrew(member, pilot ? pilot->definition.room : -1);
    if (member.room >= 0) {
        member.savedRoom = member.room;
        member.savedSlot = member.slot;
        // CrewMemberFactory::CreateCrewmember 0x45e770 builds the actor and
        // ShipManager::AddCrewMember 0x49c1b0 appends it to vCrewList last.
        assignPhysicalCrewOrder(member);
        crew_.push_back(std::move(member));
        refreshCrewNames();
        progressionRun_.tutorial = tutorialOwned_;
        progressionRun_.flag("crew_hired");
        ++scoreCrew_;
        previousCrew_.clear();
        enemy_.rewardCrewPending = false;
        enemy_.rewardCrewOverflow = false;
        if (crewRoster().size() > std::size_t(devCrewLimit_)) {
            panel_ = "crew";
            dismissCrew_ = renameCrew_ = -1;
        }
        updateManning();
        return;
    }
    enemy_.rewardCrewOverflow = true;
    voyageMessage_ = "Crew reward waiting: no unoccupied room slot.";
}
} // namespace ftl
