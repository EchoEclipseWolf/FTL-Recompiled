#include "menu.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ftl {
bool Menu::wipeNativeProfile() {
    Json fresh{{"version", 1},
               {"runs", 0},
               {"ships", 0},
               {"beacons", 0},
               {"scrap", 0},
               {"best_sector", 1},
               {"opened_list", 0},
               {"progression", ProgressionLedger{}.json()}};
    // Keep internal identity/counter baselines for the retained Continue save.
    // Player-visible totals, records, achievements and unlocks are still reset.
    fresh["next_run_id"] = profile_.value("next_run_id", profile_.value("started_runs", 0));
    fresh["started_runs"] = 0;
    if (hasSave_ && profile_.contains("active_run")) {
        fresh["active_run"] = profile_.at("active_run");
    }
    if (!commitProfile(std::move(fresh))) {
        return false;
    }
    run_.progression(profile_);
    statsRecentFamily_ = statsRecentLayout_ = 0;
    statsRecentTopRank_ = statsRecentShipRank_ = -1;
    supportError_.clear();
    return true;
}

bool Menu::syncHangarProfile() {
    if (hangarProfile_ != "native" || profile_.value("opened_list", 0) == hangar_.openedList()) {
        return true;
    }
    auto updated = profile_;
    updated["opened_list"] = hangar_.openedList();
    return commitProfile(std::move(updated));
}

bool Menu::commitProfile(Json updated) {
    try {
        validateStatsProfile(updated);
        const auto document = updated.dump(2);
        if (document.size() > 65536) {
            throw std::runtime_error("profile_too_large");
        }
        if (!profilePath_.empty()) {
            const std::filesystem::path destination(profilePath_);
            const std::filesystem::path temporary(profilePath_ + ".tmp");
            const std::filesystem::path backup(profilePath_ + ".bak");
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output << document;
                output.flush();
                output.close();
                if (!output) {
                    throw std::runtime_error("native_profile_write_failed");
                }
            }
            const bool previous = std::filesystem::exists(destination);
            if (previous) {
                std::filesystem::copy_file(
                    destination, backup, std::filesystem::copy_options::overwrite_existing);
            }
            bool removed = false;
            try {
                if (previous) {
                    removed = std::filesystem::remove(destination);
                }
                std::filesystem::rename(temporary, destination);
            } catch (...) {
                if (removed) {
                    std::error_code restoreError;
                    std::filesystem::copy_file(backup,
                                               destination,
                                               std::filesystem::copy_options::overwrite_existing,
                                               restoreError);
                    if (restoreError) {
                        throw std::runtime_error("native_profile_restore_failed");
                    }
                }
                throw;
            }
        }
        profile_ = std::move(updated);
        return true;
    } catch (const std::exception& error) {
        supportError_ = error.what();
        return false;
    }
}

bool Menu::syncRunProgression(bool terminal) {
    if (!run_.isOpen() || run_.tutorialRunning() || run_.terminalRecorded()) {
        return true;
    }
    if (!terminal && run_.defeated()) {
        return true;
    }
    auto updated = profile_;
    updated["progression"] = run_.progressionState();
    const auto counters = run_.runStatistics();
    auto identity = run_.profileRunId();
    auto started = updated.value("started_runs", updated.at("runs").get<int>());
    auto nextIdentity = static_cast<std::uint64_t>(updated.value("next_run_id", started));
    if (identity == 0) {
        if (nextIdentity >= static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            supportError_ = "native_run_identity_exhausted";
            return false;
        }
        identity = ++nextIdentity;
    }
    const bool sameRun = updated.contains("active_run") &&
                         updated.at("active_run").at("id").get<std::uint64_t>() == identity;
    Json active{{"id", identity}};
    Json maxima = updated.value(
        "session_records", Json{{"ships", 0}, {"beacons", 0}, {"scrap", 0}, {"crew_hired", 0}});
    for (const auto* key : {"ships", "beacons", "scrap", "crew_hired"}) {
        const int value = counters.at(key).get<int>();
        const int accounted = sameRun ? updated.at("active_run").at(key).get<int>() : 0;
        const int delta = std::max(0, value - accounted);
        const auto total = static_cast<std::int64_t>(updated.value(key, 0)) + delta;
        updated[key] =
            static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max()));
        active[key] = std::max(value, accounted);
        maxima[key] = std::max(maxima.at(key).get<int>(), value);
    }
    updated["active_run"] = std::move(active);
    updated["next_run_id"] = std::max(nextIdentity, identity);
    updated["started_runs"] =
        run_.profileRunStarted()
            ? static_cast<int>(std::min<std::int64_t>(static_cast<std::int64_t>(started) + 1,
                                                      std::numeric_limits<int>::max()))
            : started;
    updated["session_records"] = std::move(maxima);
    const auto records = run_.crewRecords();
    for (const auto& [key, record] : records.items()) {
        if (!updated.contains("crew_records") || !updated.at("crew_records").contains(key) ||
            record.at("value").get<int>() >
                updated.at("crew_records").at(key).at("value").get<int>()) {
            updated["crew_records"][key] = record;
        }
    }
    if (updated == profile_) {
        return true;
    }
    if (!commitProfile(std::move(updated))) {
        return false;
    }
    run_.profileRunId(identity);
    run_.profileRunStarted(false);
    return true;
}
} // namespace ftl
