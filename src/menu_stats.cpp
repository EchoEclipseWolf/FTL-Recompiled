#include "menu.hpp"
#include "profile.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ftl {
namespace {
constexpr std::array<const char*, 21> achievementIds{
    "ACH_SECTOR_5",    "ACH_SECTOR_8",       "ACH_WIN_EASY",   "ACH_WIN_NORMAL",  "ACH_UNLOCK_ALL",
    "ACH_SCRAP",       "ACH_SHIPS",          "ACH_PACIFIST",   "ACH_NO_UPGRADES", "ACH_NO_REPAIR",
    "ACH_NO_MISSILES", "ACH_NO_DRONES",      "ACH_NO_BUYING",  "ACH_NO_DEATH",    "ACH_BURNING",
    "ACH_BAD_DODGING", "ACH_BOARDING_DRONE", "ACH_ONE_VOLLEY", "ACH_INVADE_SHIP", "ACH_SLICE_DICE",
    "ACH_SUFFOCATE"};

Rect achievementRect(std::size_t index) {
    // ScoreKeeper::Open0x444a60: three source-defined rows of seven64px icons.
    return {704.f + 70.f * static_cast<float>(index % 7),
            163.f + 170.f * static_cast<float>(index / 7),
            64,
            64};
}
} // namespace

void Menu::initializeStats(const Archive& archive) {
    statsArchive_ = &archive;
    statsCatalog_ = std::make_unique<BlueprintCatalog>(archive);
    const auto& catalog = *statsCatalog_;
    for (int family = 0; family < 10; ++family) {
        for (int layout = 0; layout < 3; ++layout) {
            const auto* ship = catalog.ship(family, layout);
            if (ship) {
                const auto image = "img/customizeUI/miniship_" + ship->image + ".png";
                if (archive.contains(image)) {
                    statsShips_.emplace(ship->id, StatsShip{image, ship->shipClass, {}});
                }
            }
        }
    }
    auto document = xmlResource(archive, "data/achievements.xml");
    const auto root = document.child("FTL");
    for (const auto* id : achievementIds) {
        const auto row = root.find_child_by_attribute("achievement", "id", id);
        if (!row) {
            throw std::runtime_error("missing_statistics_achievement");
        }
        const std::string image = "img/" + std::string(row.child("img").child_value());
        if (!archive.contains(image)) {
            throw std::runtime_error("missing_statistics_achievement_image");
        }
        statsAchievementsCatalog_.push_back({id,
                                             image,
                                             row.child("name").attribute("id").as_string(),
                                             row.child("desc").attribute("id").as_string(),
                                             row.child("multiDifficulty").text().as_int() != 0});
    }
    for (int family = 0; family < 10; ++family) {
        std::vector<StatsAchievement> achievements;
        for (const auto row : root.children("achievement")) {
            if (std::string(row.child("ship").child_value()) !=
                BlueprintCatalog::families[family]) {
                continue;
            }
            const std::string image = "img/" + std::string(row.child("img").child_value());
            if (!archive.contains(image)) {
                throw std::runtime_error("missing_ship_statistics_achievement_image");
            }
            achievements.push_back({row.attribute("id").as_string(),
                                    image,
                                    row.child("name").attribute("id").as_string(),
                                    row.child("desc").attribute("id").as_string(),
                                    row.child("multiDifficulty").text().as_int() != 0});
            if (achievements.size() == 3) {
                break;
            }
        }
        if (achievements.size() != 3) {
            throw std::runtime_error("missing_ship_statistics_achievements");
        }
        for (int layout = 0; layout < 3; ++layout) {
            if (const auto* ship = catalog.ship(family, layout);
                ship && statsShips_.contains(ship->id)) {
                statsShips_.at(ship->id).achievements = achievements;
            }
        }
    }
}

void Menu::openStats(bool lastPlaythrough) {
    // ScoreKeeper::Open444a60 selects the latest accepted score's family and
    // switches to Ship Best only when no global Top Score was accepted.
    statsLastPlaythrough_ = lastPlaythrough;
    statsShipBest_ = statsRecentTopRank_ == -1 && statsRecentShipRank_ != -1;
    statsAchievements_ = false;
    const bool accepted = statsRecentTopRank_ != -1 || statsRecentShipRank_ != -1;
    statsFamily_ = accepted ? statsRecentFamily_ : 0;
    statsLayout_ = accepted ? statsRecentLayout_ : 0;
    statsListLayout_ = statsLayout_;
    statsShipList_ = false;
    modal_ = "stats";
}

void Menu::validateStatsProfile(const Json& profile) const {
    static_cast<void>(ProgressionLedger::read(profile));
    const auto integer = [](const Json& value, int minimum, int maximum) {
        if (!value.is_number_integer() ||
            (value.is_number_unsigned() &&
             value.get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))) {
            throw std::runtime_error("invalid_native_score_history");
        }
        const auto number = value.get<std::int64_t>();
        if (number < minimum || number > maximum) {
            throw std::runtime_error("invalid_native_score_history");
        }
    };
    if (profile.contains("opened_list")) {
        integer(profile.at("opened_list"), 0, 2);
    }
    if (profile.contains("session_records")) {
        const auto& records = profile.at("session_records");
        if (!records.is_object() || records.size() != 4) {
            throw std::runtime_error("invalid_native_session_records");
        }
        for (const auto* key : {"ships", "beacons", "scrap", "crew_hired"}) {
            integer(records.at(key), 0, std::numeric_limits<int>::max());
        }
    }
    for (const auto* key : {"crew_hired", "victories", "started_runs", "next_run_id"}) {
        if (profile.contains(key)) {
            integer(profile.at(key), 0, std::numeric_limits<int>::max());
        }
    }
    if (profile.contains("active_run")) {
        const auto& active = profile.at("active_run");
        if (!active.is_object() || active.size() != 5 ||
            (!profile.contains("next_run_id") && !profile.contains("started_runs"))) {
            throw std::runtime_error("invalid_native_active_run");
        }
        integer(active.at("id"), 1, std::numeric_limits<int>::max());
        if (active.at("id").get<int>() >
            profile.at(profile.contains("next_run_id") ? "next_run_id" : "started_runs")
                .get<int>()) {
            throw std::runtime_error("invalid_native_active_run");
        }
        for (const auto* key : {"ships", "beacons", "scrap", "crew_hired"}) {
            integer(active.at(key), 0, std::numeric_limits<int>::max());
        }
    }
    if (profile.contains("last_terminal_run")) {
        integer(profile.at("last_terminal_run"), 1, std::numeric_limits<int>::max());
        if (!profile.contains("next_run_id") ||
            profile.at("last_terminal_run").get<int>() > profile.at("next_run_id").get<int>()) {
            throw std::runtime_error("invalid_native_terminal_run");
        }
    }
    if (profile.contains("crew_records")) {
        const auto& records = profile.at("crew_records");
        constexpr std::array<const char*, 5> keys{
            "repairs", "combat_kills", "piloted_evasions", "jumps_survived", "earned_skill_tiers"};
        if (!records.is_object() || records.size() > keys.size()) {
            throw std::runtime_error("invalid_native_crew_records");
        }
        for (const auto& [key, row] : records.items()) {
            if (std::ranges::find(keys, key) == keys.end() || !row.is_object() || row.size() != 5 ||
                !row.at("name").is_string() ||
                row.at("name").get_ref<const std::string&>().empty() ||
                row.at("name").get_ref<const std::string&>().size() > 256 ||
                !row.at("species").is_string() || !row.at("male").is_boolean()) {
                throw std::runtime_error("invalid_native_crew_records");
            }
            static_cast<void>(statsCatalog_->appearance(row.at("species").get<std::string>()));
            integer(row.at("appearance"), 0, std::numeric_limits<int>::max());
            integer(row.at("value"),
                    1,
                    key == "earned_skill_tiers" ? 12 : std::numeric_limits<int>::max());
        }
    }
    const auto entry = [&](const Json& row, const std::string& ship) {
        if (!row.is_object() || row.size() != 7 || !row.at("name").is_string() ||
            row.at("name").get_ref<const std::string&>().size() > 256 ||
            !row.at("blueprint").is_string() ||
            !statsShips_.contains(row.at("blueprint").get<std::string>()) ||
            (!ship.empty() && row.at("blueprint") != ship) || !row.at("victory").is_boolean() ||
            !row.at("advanced").is_boolean()) {
            throw std::runtime_error("invalid_native_score_history");
        }
        integer(row.at("score"), 1, std::numeric_limits<int>::max());
        integer(row.at("sector"), 1, 8);
        integer(row.at("difficulty"), 0, 2);
    };
    const auto list = [&](const Json& rows, std::size_t maximum, const std::string& ship) {
        if (!rows.is_array() || rows.size() > maximum) {
            throw std::runtime_error("invalid_native_score_history");
        }
        int previous = std::numeric_limits<int>::max();
        for (const auto& row : rows) {
            entry(row, ship);
            const int score = row.at("score").get<int>();
            if (score > previous) {
                throw std::runtime_error("invalid_native_score_history_order");
            }
            previous = score;
        }
    };
    if (profile.contains("best_run")) {
        entry(profile.at("best_run"), "");
    }
    if (profile.contains("top_scores")) {
        list(profile.at("top_scores"), 5, "");
    }
    if (profile.contains("ship_scores")) {
        const auto& ships = profile.at("ship_scores");
        if (!ships.is_object() || ships.size() > statsShips_.size()) {
            throw std::runtime_error("invalid_native_score_history");
        }
        for (const auto& [ship, rows] : ships.items()) {
            if (!statsShips_.contains(ship)) {
                throw std::runtime_error("invalid_native_score_history_blueprint");
            }
            list(rows, 4, ship);
        }
    }
}

void Menu::statsScoresRender() {
    // RenderTopScores0x43ea40 uses the source bitmap font numbers, including
    // HL1 at3x for rank53 and c&cnew at2x for body14. The generic menu helper
    // does not map these sizes; actual original004-statistics.png confirms them.
    const auto text = [&](const std::string& value,
                          float x,
                          float y,
                          int font,
                          bool center,
                          SDL_Color color = SDL_Color{255, 255, 255, 255}) {
        if (locale_ == "zh-Hans") {
            supportText(value, x, y, font, center, color);
            return;
        }
        const auto* path = font >= 50                 ? "fonts/HL1.font"
                           : font == 24 || font == 14 ? "fonts/c&cnew.font"
                                                      : "fonts/JustinFont11Bold.font";
        const float scale = font >= 50   ? static_cast<float>(font - 50)
                            : font == 24 ? 3.f
                            : font == 14 ? 2.f
                                         : 1.f;
        renderer_.text(renderer_.font(path), value, x, y, scale, color, center);
    };

    const Json* rows = nullptr;
    float originY = 88;
    if (statsShipBest_) {
        const auto shipId = statsSelectedShip();
        const auto& ship = statsShips_.at(shipId);
        const auto title = ship.shipClass.reference ? texts_.get(ship.shipClass.value, locale_)
                                                    : ship.shipClass.value;
        text(title, 217, 91, 24, true);

        const auto& achievements = ship.achievements;
        for (std::size_t i = 0; i < achievements.size(); ++i) {
            const Rect rect{410.f + 70.f * static_cast<float>(i), 118, 64, 64};
            const auto ledger = ProgressionLedger::read(profile_);
            const auto* award = ledger.achievement(achievements[i].id);
            AchievementDrawing drawing;
            drawing.image = achievements[i].image;
            drawing.unlocked = award && award->unlocked;
            drawing.fresh = award && award->fresh;
            renderer_.achievement(drawing,
                                  {rect.x, rect.y},
                                  rect.contains(mouseX_, mouseY_) ? 1 : 0,
                                  texts_.get("new_achievement", locale_),
                                  locale_ == "zh-Hans");
        }
        for (const auto& [id, rect] : statsShipControls()) {
            if (id == "stats_ship_list") {
                statsButtonRender(rect, texts_.get("ship_list_button", locale_), 62, true, false);
            } else if (id == "stats_ship_prev" || id == "stats_ship_next") {
                auto& arrow = renderer_.image(
                    "img/customizeUI/button_arrow_" +
                    std::string(rect.contains(mouseX_, mouseY_) ? "select2" : "on") + ".png");
                renderer_.sprite(arrow,
                                 rect,
                                 {0, 0, static_cast<float>(arrow.w), static_cast<float>(arrow.h)},
                                 0,
                                 {255, 255, 255, 255},
                                 id == "stats_ship_next");
            } else if (id.starts_with("stats_ship_type:")) {
                const int layout = std::stoi(id.substr(16));
                statsButtonRender(rect,
                                  std::string(1, static_cast<char>('A' + layout)),
                                  63,
                                  statsShipUnlocked(statsFamily_, layout),
                                  layout == statsLayout_);
            }
        }
        originY = 204;
        if (profile_.contains("ship_scores") && profile_.at("ship_scores").contains(shipId)) {
            rows = &profile_.at("ship_scores").at(shipId);
        }
    } else if (profile_.contains("top_scores")) {
        rows = &profile_.at("top_scores");
    }
    if (!rows) {
        return;
    }
    const auto substitute = [](std::string label, int value) {
        if (const auto position = label.find("\\1"); position != std::string::npos) {
            label.replace(position, 2, std::to_string(value));
        }
        return label;
    };
    for (std::size_t i = 0; i < rows->size(); ++i) {
        const auto& row = rows->at(i);
        const float y = originY + 116.f * static_cast<float>(i);
        renderer_.draw(statsShips_.at(row.at("blueprint").get<std::string>()).image, 112, y);
        text(substitute(texts_.get("stats_ranking", locale_), static_cast<int>(i + 1)),
             61,
             y + 45,
             53,
             false);
        text(row.at("name").get<std::string>(), 462, y + 1, 24, true);
        text(substitute(texts_.get("scorelist_score", locale_), row.at("score").get<int>()),
             462,
             y + 38,
             14,
             true);
        text(substitute(texts_.get("scorelist_sector", locale_), row.at("sector").get<int>()),
             462,
             y + 62,
             14,
             true);
        const auto difficulty = row.at("difficulty").get<int>();
        auto label = texts_.get(difficulty == 0   ? "easy"
                                : difficulty == 1 ? "normal"
                                                  : "hard",
                                locale_);
        text(label, 462, y + 85, 14, true);
        constexpr SDL_Color badge{255, 255, 50, 255};
        const int recentRank = statsShipBest_ ? (statsFamily_ == statsRecentFamily_ &&
                                                         statsLayout_ == statsRecentLayout_
                                                     ? statsRecentShipRank_
                                                     : -1)
                                              : statsRecentTopRank_;
        if (recentRank == static_cast<int>(i)) {
            text(texts_.get("new_score", locale_), 312, y + 48, 12, false, badge);
        }
        if (row.at("victory").get<bool>()) {
            text(texts_.get("victory", locale_), 317, y + 97, 12, false, badge);
        }
        if (row.at("advanced").get<bool>()) {
            text(texts_.get("advanced", locale_), 542, y + 97, 12, false, badge);
        }
    }
}

Json Menu::statsAchievementsState() const {
    Json result = Json::array();
    for (std::size_t i = 0; i < statsAchievementsCatalog_.size(); ++i) {
        const auto& entry = statsAchievementsCatalog_[i];
        const auto rect = achievementRect(i);
        const bool hovered =
            modal_ == "stats" && statsAchievements_ && rect.contains(mouseX_, mouseY_);
        const auto ledger = ProgressionLedger::read(profile_);
        const auto* award = ledger.achievement(entry.id);
        result.push_back({{"id", entry.id},
                          {"image", entry.image},
                          {"locked", !award || !award->unlocked},
                          {"name", texts_.get(entry.name, locale_)},
                          {"description", texts_.get(entry.description, locale_)},
                          {"rect", {rect.x, rect.y, rect.w, rect.h}},
                          {"hovered", hovered}});
    }
    return result;
}

void Menu::statsAchievementsRender() {
    // OnRender0x4459d0: headings and CAchievement::OnRender0x429fe0.
    constexpr std::array<const char*, 3> headings{
        "progression_heading", "distance_heading", "skill_heading"};
    for (std::size_t row = 0; row < headings.size(); ++row) {
        supportText(texts_.get(headings[row], locale_),
                    705,
                    row == 2 ? 430.f : 100.f + 160.f * static_cast<float>(row),
                    24,
                    false);
    }
    for (std::size_t i = 0; i < statsAchievementsCatalog_.size(); ++i) {
        const auto& entry = statsAchievementsCatalog_[i];
        const auto rect = achievementRect(i);
        const bool hover = rect.contains(mouseX_, mouseY_);
        const auto ledger = ProgressionLedger::read(profile_);
        const auto* award = ledger.achievement(entry.id);
        AchievementDrawing drawing;
        drawing.image = entry.image;
        drawing.unlocked = award && award->unlocked;
        drawing.fresh = award && award->fresh;
        renderer_.achievement(drawing,
                              {rect.x, rect.y},
                              hover ? 1 : 0,
                              texts_.get("new_achievement", locale_),
                              locale_ == "zh-Hans");
    }
}

void Menu::statsTooltipRender(bool outer) {
    if (!outer && !statsShipList_) {
        return;
    }
    const auto ledger = ProgressionLedger::read(profile_);
    const auto label = [&](const LocalizedText& value) {
        return value.reference ? texts_.get(value.value, locale_) : value.value;
    };
    const auto replace =
        [](std::string value, const std::string& token, const std::string& replacement) {
            std::size_t position = 0;
            while ((position = value.find(token, position)) != std::string::npos) {
                value.replace(position, token.size(), replacement);
                position += replacement.size();
            }
            return value;
        };
    const auto format = [&](const std::string& key, const std::string& first) {
        return replace(texts_.get(key, locale_), "\\1", first);
    };
    const auto strictInside = [&](Rect rect) {
        return mouseX_ > rect.x && mouseX_ < rect.x + rect.w && mouseY_ > rect.y &&
               mouseY_ < rect.y + rect.h;
    };
    const StatsAchievement* tooltip = nullptr;
    std::string title;
    std::string description;
    if (statsShipList_) {
        if (!outer) {
            statsShipListRender();
        }
        for (const auto& [id, rect] : statsShipControls()) {
            if (!id.starts_with("stats_list_ship:") || !strictInside(rect)) {
                continue;
            }
            const int family = std::stoi(id.substr(16));
            const auto* ship = statsCatalog_->ship(family, statsListLayout_);
            if (!ship) {
                continue;
            }
            const bool locked = !ledger.unlocked(family, statsListLayout_);
            const auto infoTitle =
                family == 8 && locked
                    ? texts_.get("unidentified", locale_)
                    : label(ship->shipClass) +
                          format("shipselect_type_suffix",
                                 texts_.get(
                                     "type_" +
                                         std::string(1, static_cast<char>('a' + statsListLayout_)),
                                     locale_));
            auto body = label(locked ? ship->unlock : ship->description);
            if (locked && statsListLayout_ == 0) {
                if (family == 8) {
                    body +=
                        " \n\n" + format("alt_unlock", texts_.get("alt_unlock_crystal", locale_));
                } else if (family != 0 && family != 3 && family != 9) {
                    constexpr std::array<int, 10> previous{0, 6, 7, 0, 3, 2, 5, 4, 0, 0};
                    const int before = previous[static_cast<std::size_t>(family)];
                    body += " \n\n" +
                            format("alt_unlock",
                                   format(ledger.unlocked(before, 0) ? "alt_unlock_victory"
                                                                     : "alt_unlock_victory_locked",
                                          label(statsCatalog_->ship(before, 0)->shipClass)));
                }
            }
            if (!outer) {
                // InfoBox54bef0/54c710/54add0: width360+14border, location460+67.
                auto& headingFont = locale_ == "zh-Hans"
                                        ? renderer_.sourceChineseFont(16, infoTitle)
                                        : renderer_.font("fonts/c&cnew.font");
                auto& descriptionFont = locale_ == "zh-Hans"
                                            ? renderer_.sourceChineseFont(10, body)
                                            : renderer_.font("fonts/JustinFont10.font");
                const float headingScale = locale_ == "zh-Hans" ? 1.f : 2.f;
                const auto headings = renderer_.wrappedLines(
                    headingFont, infoTitle, 342, headingScale, locale_ == "zh-Hans");
                const auto lines =
                    renderer_.wrappedLines(descriptionFont, body, 342, 1, locale_ == "zh-Hans");
                const float bodyLineHeight = Renderer::sourceLineHeight(10, descriptionFont);
                const float headingLineHeight = Renderer::sourceLineHeight(16, headingFont);
                const float height =
                    std::max(locked && statsListLayout_ == 0 ? 176.f : 135.f,
                             63.f + bodyLineHeight * static_cast<float>(lines.size()));
                renderer_.windowFrame({453, 527, 374, height});
                for (std::size_t i = 0; i < headings.size(); ++i) {
                    renderer_.text(headingFont,
                                   headings[i],
                                   471,
                                   540.f + headingLineHeight * static_cast<float>(i),
                                   headingScale);
                }
                const float bodyY =
                    540.f + headingLineHeight * static_cast<float>(headings.size()) + 15.f;
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    renderer_.text(descriptionFont,
                                   lines[i],
                                   471,
                                   bodyY + bodyLineHeight * static_cast<float>(i),
                                   1);
                }
            }
            if (!ledger.unlocked(family, 0)) {
                continue;
            }
            const auto& achievements = statsShips_.at(ship->id).achievements;
            const int count = family == 0 || family == 3 || family == 9 ? 4 : 5;
            for (int index = 0; index < count; ++index) {
                if (!strictInside(
                        {rect.x + 6 + 37.f * static_cast<float>(index), rect.y + 123, 32, 32})) {
                    continue;
                }
                if (index < 3) {
                    tooltip = &achievements[static_cast<std::size_t>(index)];
                } else {
                    const auto cls = label(statsCatalog_->ship(family, 0)->shipClass);
                    const auto achievementId = std::string(BlueprintCatalog::families[family]) +
                                               (index == 3 ? "_VICTORY" : "_QUEST");
                    title = format(index == 3 ? "victory_ach_title" : "quest_ach_title", cls);
                    description = format(index == 3 ? "victory_ach" : "quest_ach", cls);
                    if (const auto* award = ledger.achievement(achievementId);
                        award && award->unlocked && index == 3) {
                        std::string awarded;
                        constexpr std::array<const char*, 3> difficulties{"easy", "normal", "hard"};
                        for (std::size_t layout = 0; layout < award->layouts.size(); ++layout) {
                            if (award->layouts[layout] < 0) {
                                continue;
                            }
                            auto line =
                                format("achieved_with",
                                       texts_.get("type_" + std::string(
                                                                1, static_cast<char>('a' + layout)),
                                                  locale_));
                            line = replace(
                                line,
                                "\\2",
                                texts_.get(
                                    difficulties[static_cast<std::size_t>(award->layouts[layout])],
                                    locale_));
                            awarded += (awarded.empty() ? "" : "\n") + line;
                        }
                        if (!awarded.empty()) {
                            description += " \n\n" + awarded;
                        }
                    }
                }
            }
        }
        constexpr std::array<int, 8> order{0, 3, 4, 7, 2, 5, 6, 1};
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (ledger.unlocked(order[i + 1], 0)) {
                continue;
            }
            const bool right = i < 3;
            const bool down = i == 3;
            const float x = right  ? 136.f + 205.f * static_cast<float>(i) + 191.f
                            : down ? 838.f
                                   : 751.f - 205.f * static_cast<float>(i - 4) - 43.f;
            const float y = right  ? 227.f
                            : down ? 321.f - (ledger.unlocked(order[i], 0) ? 0.f : 20.f)
                                   : 404.f;
            if (!strictInside({x, y, down ? 30.f : 43.f, down ? 85.f : 28.f})) {
                continue;
            }
            title.clear();
            description = format(i == 0 ? "ship_unlock_sector5" : "ship_unlock_victory",
                                 label(statsCatalog_->ship(order[i], 0)->shipClass));
            description =
                replace(description, "\\2", label(statsCatalog_->ship(order[i + 1], 0)->shipClass));
        }
    } else {
        if (statsShipBest_) {
            const auto& achievements = statsShips_.at(statsSelectedShip()).achievements;
            for (std::size_t i = 0; i < achievements.size(); ++i) {
                if (strictInside({410.f + 70.f * static_cast<float>(i), 118, 64, 64})) {
                    tooltip = &achievements[i];
                }
            }
        }
        if (statsAchievements_) {
            for (std::size_t i = 0; i < statsAchievementsCatalog_.size(); ++i) {
                if (strictInside(achievementRect(i))) {
                    tooltip = &statsAchievementsCatalog_[i];
                }
            }
        }
    }
    if (!outer) {
        // ShipSelect518240 draws Type tabs after its specialized InfoBox.
        for (const auto& [id, rect] : statsShipControls()) {
            if (!id.starts_with("stats_list_type:")) {
                continue;
            }
            const int layout = std::stoi(id.substr(16));
            statsButtonRender(
                rect,
                texts_.get("typebutton_type_" + std::string(1, static_cast<char>('a' + layout)),
                           locale_),
                62,
                true,
                layout == statsListLayout_);
        }
        return;
    }
    if (tooltip) {
        title = texts_.get(tooltip->name, locale_);
        description = texts_.get(tooltip->description, locale_);
        if (const auto* award = ledger.achievement(tooltip->id);
            tooltip->multiDifficulty && award && award->unlocked && award->difficulty >= 0) {
            constexpr std::array<const char*, 3> difficultyKeys{
                "ach_easy", "ach_normal", "ach_hard"};
            description +=
                " \n\n" +
                texts_.get(difficultyKeys[static_cast<std::size_t>(award->difficulty)], locale_);
        }
    }
    if (description.empty()) {
        return;
    }
    renderer_.tooltip(title, description, {mouseX_, mouseY_}, 0, locale_ == "zh-Hans");
}

std::string Menu::statsSelectedShip() const {
    return std::string(BlueprintCatalog::families[statsFamily_]) +
           (statsLayout_ ? "_" + std::to_string(statsLayout_ + 1) : "");
}

bool Menu::statsShipUnlocked(int family, int layout) const {
    return ProgressionLedger::read(profile_).unlocked(family, layout);
}

std::vector<std::pair<std::string, Rect>> Menu::statsShipControls() const {
    std::vector<std::pair<std::string, Rect>> controls;
    if (!statsShipBest_ && !statsShipList_) {
        return controls;
    }
    if (statsShipList_) {
        for (int i = 0; i < 3; ++i) {
            controls.emplace_back("stats_list_type:" + std::to_string(i),
                                  Rect{685.f + 87.f * static_cast<float>(i), 122, 83, 22});
        }
        constexpr std::array<int, 10> order{0, 3, 4, 7, 9, 1, 6, 5, 2, 8};
        for (std::size_t i = 0; i < order.size(); ++i) {
            const bool unlockedA = statsShipUnlocked(order[i], 0);
            const bool exists = statsListLayout_ != 2 || order[i] < 8;
            controls.emplace_back(
                "stats_list_ship:" + std::to_string(order[i]),
                Rect{i % 5 == 4 ? 976.f : 136.f + 205.f * static_cast<float>(i % 5),
                     (i < 5 ? 161.f : 338.f) + (unlockedA && exists ? 0.f : 20.f),
                     191,
                     unlockedA ? 161.f : 121.f});
        }
        return controls;
    }
    controls.emplace_back("stats_ship_prev", Rect{79, 157, 32, 28});
    controls.emplace_back("stats_ship_list", Rect{116, 157, 51, 28});
    controls.emplace_back("stats_ship_next", Rect{172, 157, 32, 28});
    for (int i = 0; i < 3; ++i) {
        if (i == 2 && statsFamily_ >= 8) {
            continue;
        }
        controls.emplace_back(
            "stats_ship_type:" + std::to_string(i),
            Rect{250.f + 45.f * static_cast<float>(i) + (statsFamily_ >= 8 ? 22.f : 0.f),
                 155,
                 36,
                 34});
    }
    return controls;
}

bool Menu::statsShipAction(const std::string& id) {
    if ((id.starts_with("stats_ship_type:") || id.starts_with("stats_list_type:") ||
         id.starts_with("stats_list_ship:")) &&
        (id.size() != 17 || id.back() < '0' || id.back() > '9')) {
        return false;
    }

    if (id == "stats_ship_list") {
        statsShipList_ = true;
        statsListLayout_ = statsLayout_;
        return true;
    }
    if (id == "stats_ship_prev" || id == "stats_ship_next") {
        const int direction = id == "stats_ship_prev" ? -1 : 1;
        for (int step = 1; step <= 10; ++step) {
            const int family = (statsFamily_ + direction * step + 10) % 10;
            if (statsShipUnlocked(family, statsLayout_)) {
                statsFamily_ = family;
                break;
            }
        }
        return true;
    }
    if (id.starts_with("stats_ship_type:")) {
        const int layout = std::stoi(id.substr(16));
        if (layout >= 0 && layout < 3 && statsShipUnlocked(statsFamily_, layout)) {
            statsLayout_ = layout;
        }
        return true;
    }
    if (id.starts_with("stats_list_type:")) {
        const int layout = std::stoi(id.substr(16));
        if (layout >= 0 && layout < 3) {
            statsListLayout_ = layout;
        }
        return true;
    }
    if (id.starts_with("stats_list_ship:")) {
        const int family = std::stoi(id.substr(16));
        if (family >= 0 && family < 10 && statsShipUnlocked(family, statsListLayout_)) {
            statsFamily_ = family;
            statsLayout_ = statsListLayout_;
            statsShipList_ = false;
        }
        return true;
    }
    return false;
}

Json Menu::statsShipState() const {
    Json controls = Json::array();
    for (const auto& [id, rect] : statsShipControls()) {
        controls.push_back({{"id", id}, {"rect", {rect.x, rect.y, rect.w, rect.h}}});
    }
    int hoveredFamily = -1;
    int hoveredArrow = -1;
    std::string hoveredAchievement;
    const auto strictInside = [&](Rect rect) {
        return mouseX_ > rect.x && mouseX_ < rect.x + rect.w && mouseY_ > rect.y &&
               mouseY_ < rect.y + rect.h;
    };
    if (statsShipList_) {
        for (const auto& [id, rect] : statsShipControls()) {
            if (!id.starts_with("stats_list_ship:") || !strictInside(rect)) {
                continue;
            }
            const int family = std::stoi(id.substr(16));
            if (!statsCatalog_->ship(family, statsListLayout_)) {
                continue;
            }
            hoveredFamily = family;
            if (!statsShipUnlocked(family, 0)) {
                continue;
            }
            const auto* ship = statsCatalog_->ship(family, statsListLayout_);
            const auto& achievements = statsShips_.at(ship->id).achievements;
            for (int index = 0; index < (family == 0 || family == 3 || family == 9 ? 4 : 5);
                 ++index) {
                if (strictInside(
                        {rect.x + 6 + 37.f * static_cast<float>(index), rect.y + 123, 32, 32})) {
                    hoveredAchievement = index < 3
                                             ? achievements[static_cast<std::size_t>(index)].id
                                             : std::string(BlueprintCatalog::families[family]) +
                                                   (index == 3 ? "_VICTORY" : "_QUEST");
                }
            }
        }
        constexpr std::array<int, 8> order{0, 3, 4, 7, 2, 5, 6, 1};
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (statsShipUnlocked(order[i + 1], 0)) {
                continue;
            }
            const bool right = i < 3;
            const bool down = i == 3;
            const float x = right  ? 136.f + 205.f * static_cast<float>(i) + 191.f
                            : down ? 838.f
                                   : 751.f - 205.f * static_cast<float>(i - 4) - 43.f;
            const float y = right  ? 227.f
                            : down ? 321.f - (statsShipUnlocked(order[i], 0) ? 0.f : 20.f)
                                   : 404.f;
            if (strictInside({x, y, down ? 30.f : 43.f, down ? 85.f : 28.f})) {
                hoveredArrow = static_cast<int>(i);
            }
        }
    }
    return {{"family", statsFamily_},
            {"layout", statsLayout_},
            {"blueprint", statsSelectedShip()},
            {"list_open", statsShipList_},
            {"list_layout", statsListLayout_},
            {"controls", controls},
            {"hovered_family", hoveredFamily},
            {"hovered_achievement", hoveredAchievement},
            {"hovered_arrow", hoveredArrow}};
}

Json Menu::statsShipAchievementsState() const {
    Json rows = Json::array();
    const auto& achievements = statsShips_.at(statsSelectedShip()).achievements;
    for (std::size_t i = 0; i < achievements.size(); ++i) {
        const auto& achievement = achievements[i];
        const Rect rect{410.f + 70.f * static_cast<float>(i), 118, 64, 64};
        const auto ledger = ProgressionLedger::read(profile_);
        const auto* award = ledger.achievement(achievement.id);
        rows.push_back(
            {{"id", achievement.id},
             {"title", texts_.get(achievement.name, locale_)},
             {"description", texts_.get(achievement.description, locale_)},
             {"unlocked", award && award->unlocked},
             {"rect", {rect.x, rect.y, rect.w, rect.h}},
             {"hover", statsShipBest_ && !statsShipList_ && rect.contains(mouseX_, mouseY_)}});
    }
    return rows;
}

void Menu::statsShipListRender() {
    const auto ledger = ProgressionLedger::read(profile_);
    renderer_.draw("img/customizeUI/ship_list_main_2.png", 112, 110);
    supportText(texts_.get("shiplist_title", locale_), 359, 118, 63);
    auto& typeFrame = renderer_.image("img/customizeUI/type_button_frame.png");
    constexpr std::array<float, 7> sourceX{0, 12, 13, 12, 13, 37, 38};
    constexpr std::array<float, 7> sourceWidth{12, 1, 24, 1, 24, 1, 12};
    constexpr std::array<float, 7> destinationWidth{12, 64, 24, 63, 24, 64, 12};
    float frameX = 682;
    for (std::size_t i = 0; i < sourceX.size(); ++i) {
        renderer_.sprite(
            typeFrame, {frameX, 119, destinationWidth[i], 28}, {sourceX[i], 0, sourceWidth[i], 28});
        frameX += destinationWidth[i];
    }
    for (const auto& [id, rect] : statsShipControls()) {
        if (id.starts_with("stats_list_type:")) {
            continue;
        }
        const int family = std::stoi(id.substr(16));
        const std::string blueprint =
            std::string(BlueprintCatalog::families[family]) +
            (statsListLayout_ ? "_" + std::to_string(statsListLayout_ + 1) : "");
        const bool available = statsShipUnlocked(family, statsListLayout_);
        const bool exists = statsShips_.contains(blueprint);
        if (statsShipUnlocked(family, 0) && exists) {
            renderer_.draw("img/customizeUI/ship_list_button_lower_" +
                               std::string(rect.contains(mouseX_, mouseY_) ? "select2" : "on") +
                               ".png",
                           rect.x,
                           rect.y + 107);
            for (int badge = 0; badge < (family == 0 || family == 3 || family == 9 ? 4 : 5);
                 ++badge) {
                const auto name = badge < 3 ? std::to_string(badge + 1) : badge == 3 ? "V" : "Q";
                const auto achievementId =
                    badge < 3
                        ? statsShips_.at(blueprint).achievements[static_cast<std::size_t>(badge)].id
                        : std::string(BlueprintCatalog::families[family]) +
                              (badge == 3 ? "_VICTORY" : "_QUEST");
                const auto* award = ledger.achievement(achievementId);
                AchievementDrawing drawing;
                drawing.miniOn = "img/achievements/S_" + name + "_on.png";
                drawing.miniOff = "img/achievements/S_" + name + "_off.png";
                drawing.unlocked = award && award->unlocked;
                drawing.victory = badge == 3;
                drawing.twoLayouts = family >= 8;
                if (award) {
                    drawing.layouts = award->layouts;
                }
                const Rect icon{
                    rect.x + 6 + 37.f * static_cast<float>(badge), rect.y + 123, 32, 32};
                renderer_.achievement(
                    drawing, {icon.x, icon.y}, icon.contains(mouseX_, mouseY_) ? 2 : 3);
            }
        }
        renderer_.draw("img/customizeUI/ship_list_button_" +
                           std::string(!exists                           ? "off"
                                       : rect.contains(mouseX_, mouseY_) ? "select2"
                                                                         : "on") +
                           ".png",
                       rect.x,
                       rect.y);
        if (exists) {
            auto& image = renderer_.image(statsShips_.at(blueprint).image);
            renderer_.draw(
                image, {rect.x, rect.y, static_cast<float>(image.w), static_cast<float>(image.h)});
            if (!available) {
                renderer_.draw(
                    image,
                    {rect.x, rect.y, static_cast<float>(image.w), static_cast<float>(image.h)},
                    nullptr,
                    0.f);
                renderer_.draw(rect.contains(mouseX_, mouseY_)
                                   ? "img/customizeUI/box_lock_selected.png"
                                   : "img/customizeUI/box_lock_on.png",
                               rect.x + 90,
                               rect.y + 50);
            }
        } else {
            supportText(texts_.get("no_type_c", locale_), rect.x + 95, rect.y + 50, 12, true);
        }
    }
    // ShipSelect::Open515930; UnlockArrow512130/512460.
    constexpr std::array<int, 8> unlockOrder{0, 3, 4, 7, 2, 5, 6, 1};
    for (std::size_t i = 0; i + 1 < unlockOrder.size(); ++i) {
        const int from = unlockOrder[i];
        if (ledger.unlocked(unlockOrder[i + 1], 0)) {
            continue;
        }
        const int status = ledger.unlocked(from, 0) ? (from == 0 ? 2 : 1) : 0;
        const bool down = i == 3;
        const bool right = i < 3;
        const auto asset = status == 2 ? std::string("arrow_5_right")
                                       : std::string("arrow_") + (status ? "v_" : "lock_") +
                                             (down ? "down" : "left");
        const float x = right  ? 136.f + 205.f * static_cast<float>(i) + 191.f
                        : down ? 838.f
                               : 751.f - 205.f * static_cast<float>(i - 4) - 43.f;
        const float y = right  ? 227.f
                        : down ? 321.f - (ledger.unlocked(from, 0) ? 0.f : 20.f)
                               : 404.f;
        auto& image = renderer_.image("img/customizeUI/" + asset + ".png");
        renderer_.sprite(image,
                         {x, y, static_cast<float>(image.w), static_cast<float>(image.h)},
                         {0, 0, static_cast<float>(image.w), static_cast<float>(image.h)},
                         0,
                         {255, 255, 255, 255},
                         right && status != 2);
    }
}

void Menu::statsButtonRender(
    Rect rect, const std::string& label, int font, bool enabled, bool selected) {
    const auto color = !enabled ? SDL_Color{164, 171, 160, 255}
                       : selected || rect.contains(mouseX_, mouseY_)
                           ? SDL_Color{255, 230, 94, 255}
                           : SDL_Color{235, 245, 229, 255};
    const int corner = font == 63 ? 6 : 3;
    for (int row = 0; row < static_cast<int>(rect.h); ++row) {
        const float inset = static_cast<float>(
            std::max(0, corner - std::min(row, static_cast<int>(rect.h) - row - 1)));
        renderer_.fill({rect.x + inset, rect.y + static_cast<float>(row), rect.w - 2 * inset, 1},
                       color);
    }
    const float textHeight = font == 63 ? 38.f : 24.f;
    const float offset = font == 63 ? 4.f : 2.f;
    supportText(label,
                rect.x + static_cast<float>(static_cast<int>(rect.w) / 2),
                rect.y + static_cast<float>(static_cast<int>(rect.h) / 2) - textHeight / 2 + offset,
                font,
                true,
                {25, 49, 51, 255});
}

void Menu::statsRecordsRender() {
    // ScoreKeeper::OnRender4459d0; MainMenu4bfdf0 passesfalse, GameOver4c7360 passestrue.
    const auto counter = [&](const Json& values, const char* key, float y, bool zeroDefault) {
        const auto value = values.contains(key) ? std::to_string(values.at(key).get<int>())
                           : zeroDefault        ? "0"
                                                : "--";
        supportText(value, 1165, y, 13, true);
    };
    const Json empty = Json::object();
    const auto& records =
        profile_.contains("session_records") ? profile_.at("session_records") : empty;
    const bool fresh = profile_.at("runs").get<int>() == 0;
    supportText(texts_.get("records_heading", locale_), 695, 100, 24);
    constexpr std::array<const char*, 4> keys{"ships", "beacons", "scrap", "crew_hired"};
    constexpr std::array<const char*, 4> labels{
        "defeated_stat", "beacons_stat", "scrap_stat", "crew_stat"};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const float y = 150.f + 20.f * static_cast<float>(i);
        supportText(texts_.get("most_" + std::string(labels[i]), locale_), 700, y, 13);
        counter(records, keys[i], y, fresh);
    }
    supportText(texts_.get("crew_heading", locale_), 695, 240, 24);
    constexpr std::array<const char*, 5> crewKeys{
        "repairs", "combat_kills", "piloted_evasions", "jumps_survived", "earned_skill_tiers"};
    constexpr std::array<const char*, 5> crewLabels{
        "crew_repairs", "crew_kills", "crew_evasions", "crew_jumps", "crew_masteries"};
    const auto& crewRecords =
        profile_.contains("crew_records") ? profile_.at("crew_records") : empty;
    for (std::size_t i = 0; i < crewKeys.size(); ++i) {
        const float y = 280.f + 20.f * static_cast<float>(i);
        supportText(texts_.get("most_" + std::string(crewLabels[i]), locale_), 700, y, 13);
        if (crewRecords.contains(crewKeys[i])) {
            const auto& record = crewRecords.at(crewKeys[i]);
            // CrewBlueprint::RenderIcon5e0f00: walk_down frame0, source-centered base/layers.
            const auto species = record.at("species").get<std::string>();
            const auto& frame = statsCatalog_->animation(species + "_walk_down");
            const auto textureSpecies =
                species == "human" && !record.at("male").get<bool>() ? "female" : species;
            const auto textureBase = "img/people/" + textureSpecies;
            const Rect source{static_cast<float>(frame.x),
                              static_cast<float>(frame.y),
                              static_cast<float>(frame.width),
                              static_cast<float>(frame.height)};
            const Rect destination{1035.f + std::trunc(-static_cast<float>(frame.width) / 2.f),
                                   y + 8.f + std::trunc(-static_cast<float>(frame.height) / 2.f),
                                   source.w,
                                   source.h};
            if (statsArchive_->contains(textureBase + "_base.png")) {
                renderer_.sprite(renderer_.image(textureBase + "_base.png"), destination, source);
            }
            const auto& layers = statsCatalog_->appearance(species).layers;
            int choice =
                0; // Source record reconstructs default blueprint colors; only sex is retained.
            for (std::size_t layer = 0; layer < layers.size(); ++layer) {
                const auto& colors = layers[layer];
                const auto& color = colors[static_cast<std::size_t>(choice) % colors.size()];
                choice /= static_cast<int>(colors.size());
                const auto path = textureBase + "_layer" + std::to_string(layer + 1) + ".png";
                if (statsArchive_->contains(path)) {
                    renderer_.sprite(renderer_.image(path),
                                     destination,
                                     source,
                                     0,
                                     {static_cast<Uint8>(color[0]),
                                      static_cast<Uint8>(color[1]),
                                      static_cast<Uint8>(color[2]),
                                      static_cast<Uint8>(color[3] * 255)});
                }
            }
            supportText(record.at("name").get<std::string>(), 1065, y, 13, true);
            supportText(std::to_string(record.at("value").get<int>()), 1165, y, 13, true);
        } else {
            supportText(fresh ? "0" : "--", 1165, y, 13, true);
        }
    }
    const auto runState = statsLastPlaythrough_ ? run_.state() : empty;
    const auto current = statsLastPlaythrough_ ? run_.runStatistics() : empty;
    supportText(texts_.get(statsLastPlaythrough_ ? "stats_heading" : "total_heading", locale_),
                695,
                395,
                24);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const float y = 445.f + 20.f * static_cast<float>(i);
        supportText(
            texts_.get((statsLastPlaythrough_ ? std::string() : "total_") + labels[i], locale_),
            700,
            y,
            13);
        counter(statsLastPlaythrough_ ? current : profile_, keys[i], y, fresh);
    }
    if (statsLastPlaythrough_) {
        supportText(texts_.get("final_score", locale_), 700, 545, 13);
        const auto baseScore = static_cast<std::int64_t>(current.at("scrap").get<int>()) +
                               10 * static_cast<std::int64_t>(current.at("beacons").get<int>()) +
                               20 * static_cast<std::int64_t>(current.at("ships").get<int>());
        const int difficulty = runState.at("difficulty").get<int>();
        const float multiplier = difficulty == 1 ? 1.25f : difficulty == 2 ? 1.5f : 1.f;
        const double scaled = multiplier * static_cast<float>(baseScore);
        const int score =
            static_cast<int>(std::clamp(scaled, 0., double(std::numeric_limits<int>::max())));
        supportText(std::to_string(score), 1165, 545, 13, true);
    } else {
        supportText(texts_.get("total_games", locale_), 700, 545, 13);
        counter(profile_, profile_.contains("started_runs") ? "started_runs" : "runs", 545, true);
        supportText(texts_.get("total_victories", locale_), 700, 565, 13);
        counter(profile_, "victories", 565, fresh);
    }
}
} // namespace ftl
