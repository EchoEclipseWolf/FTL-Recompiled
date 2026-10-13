#include "menu.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ftl {
const std::array<std::vector<std::string>, 4>& Menu::controlTabs() {
    return Settings::hotkeyTabs();
}

std::vector<std::pair<std::string, Rect>> Menu::controlRows() const {
    const auto& names = controlTabs().at(static_cast<std::size_t>(controlPage_));
    const int columnSize = (static_cast<int>(names.size()) + 1) / 2 - (controlPage_ == 1 ? 1 : 0);
    std::vector<std::pair<std::string, Rect>> rows;
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        const bool right = i >= columnSize;
        rows.emplace_back(names[static_cast<std::size_t>(i)],
                          Rect{480.f + (right ? 440.f : 0.f),
                               150.f + 40.f * (right ? i - columnSize : i),
                               105,
                               25});
    }
    return rows;
}

void Menu::supportText(
    const std::string& value, float x, float y, int size, bool center, SDL_Color color) {
    renderer_.text(localizedFont(size, value),
                   value,
                   x,
                   y,
                   Renderer::sourceFontScale(size, locale_ == "zh-Hans"),
                   color,
                   center);
}

Rect Menu::supportButtonRect(const std::string& value, float right, float y, float minimum) {
    const auto [left, rightEdge] =
        renderer_.bounds(localizedFont(63, value), value, locale_ == "zh-Hans" ? 1.f : 3.f);
    const float width = std::max(minimum, std::round(rightEdge - left) + 6.f);
    return {right - width, y, width, 32};
}

void Menu::supportButton(Rect rect, const std::string& value, const std::string& base) {
    auto& image = renderer_.image(base);
    const float half = std::floor(float(image.w) / 2);
    const float extra = rect.w - (base == "img/optionsUI/defaults_base.png" ? 165.f : 103.f);
    const float baseX = rect.x - 23;
    renderer_.sprite(
        image, {baseX, rect.y - 7, half, float(image.h)}, {0, 0, half, float(image.h)});
    if (extra > 0) {
        renderer_.sprite(
            image, {baseX + half, rect.y - 7, extra, float(image.h)}, {half, 0, 1, float(image.h)});
    }
    renderer_.sprite(image,
                     {baseX + half + extra, rect.y - 7, float(image.w) - half, float(image.h)},
                     {half, 0, float(image.w) - half, float(image.h)});
    const bool hover = rect.contains(mouseX_, mouseY_);
    for (int row = 0; row < int(rect.h); ++row) {
        const float inset = float(std::max(0, 4 - std::min(row, int(rect.h) - row - 1)));
        renderer_.fill({rect.x + inset, rect.y + row, rect.w - 2 * inset, 1},
                       hover ? SDL_Color{255, 238, 128, 255} : SDL_Color{235, 245, 229, 255});
    }
    supportText(value, rect.x + rect.w / 2, rect.y + 1, 63, true, {25, 49, 51, 255});
}

const std::map<std::string, std::string>& Menu::supportedHotkeys() {
    static const std::map<std::string, std::string> native = [] {
        std::map<std::string, std::string> result{{"crew1", "F1"},
                                                  {"crew2", "F2"},
                                                  {"crew3", "F3"},
                                                  {"crew4", "F4"},
                                                  {"crew5", "F5"},
                                                  {"crew6", "F6"},
                                                  {"crew7", "F7"},
                                                  {"crew8", "F8"},
                                                  {"crew_all", "Q"},
                                                  {"loadPositions", "Enter"},
                                                  {"savePositions", "Slash"},
                                                  {"pause", "Space"},
                                                  {"jump", "J"},
                                                  {"ship_info", "U"},
                                                  {"activate_cloak", "C"},
                                                  {"activate_battery", "B"},
                                                  {"autofire", "V"},
                                                  {"open", "Z"},
                                                  {"close", "X"},
                                                  {"shields", "A"},
                                                  {"engines", "S"},
                                                  {"medbay", "D"},
                                                  {"oxygen", "F"},
                                                  {"weapon1", "1"},
                                                  {"weapon2", "2"},
                                                  {"weapon3", "3"},
                                                  {"weapon4", "4"},
                                                  {"un_shields", "Shift+A"},
                                                  {"un_engines", "Shift+S"},
                                                  {"un_medbay", "Shift+D"},
                                                  {"un_oxygen", "Shift+F"}};
        for (const auto& action :
             {"ship_inv",  "ship_crew",  "store",        "drone1",        "drone2",
              "drone3",    "teleporter", "cloaking",     "mind",          "hacking",
              "artillery", "weapons",    "drones",       "un_teleporter", "un_cloaking",
              "un_mind",   "un_hacking", "un_artillery", "un_weapons",    "un_drones"}) {
            result.emplace(action, "Hotkey:" + std::string(action));
        }
        result.emplace("options", "options");
        result.emplace("force_autofire", "Hotkey:force_autofire");
        result.emplace("increase_font", "Hotkey:increase_font");
        result.emplace("decrease_font", "Hotkey:decrease_font");
        result.emplace("lockdown", "Hotkey:lockdown");
        for (const auto& action : {"send_tele", "ret_tele", "start_hacking", "mindControl"}) {
            result.emplace(action, "Hotkey:" + std::string(action));
        }
        for (int slot = 1; slot <= 8; ++slot) {
            for (const auto& prefix : {"power_", "un_power_"}) {
                const auto action = std::string(prefix) + std::to_string(slot);
                result.emplace(action, "Hotkey:" + action);
            }
        }
        return result;
    }();
    return native;
}

int Menu::originalKey(const std::string& key) {
    const auto plain = key.starts_with("Shift+") ? key.substr(6) : key;
    if (plain.size() == 1) {
        const auto c = plain[0];
        // Source keymap entries42/43 both contain '$'; entry44 ('%')
        // remains zero. Preserve these original table assignments.
        if (c == '#') {
            return '$';
        }
        if (c == '%') {
            return 0;
        }
        return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : static_cast<unsigned char>(c);
    }
    // TranslateSILKey5780f0 and LoadSettings5834c0 normalize keypad Enter
    // and digits. SDL3's remaining platform names map explicitly to SDL1.
    static const std::map<std::string, int> names{
        {"None", 0},         {"Backspace", 8},    {"Tab", 9},           {"Clear", 12},
        {"Enter", 13},       {"Return", 13},      {"Keypad Enter", 13}, {"Pause", 19},
        {"Escape", 27},      {"Space", 32},       {"Delete", 127},      {"Slash", 47},
        {"Keypad .", 266},   {"Keypad /", 267},   {"Keypad *", 268},    {"Keypad -", 269},
        {"Keypad +", 270},   {"Keypad =", 272},   {"Up", 273},          {"Down", 274},
        {"Right", 275},      {"Left", 276},       {"Insert", 277},      {"Home", 278},
        {"End", 279},        {"PageUp", 280},     {"PageDown", 281},    {"Numlock", 300},
        {"NumLock", 300},    {"CapsLock", 301},   {"ScrollLock", 302},  {"Right Shift", 303},
        {"Left Shift", 304}, {"Right Ctrl", 305}, {"Left Ctrl", 306},   {"Right Alt", 307},
        {"Left Alt", 308},   {"Right Meta", 309}, {"Left Meta", 310},   {"Left GUI", 311},
        {"Right GUI", 312},  {"Left Super", 311}, {"Right Super", 312}, {"ModeSwitch", 313},
        {"Compose", 314},    {"Help", 315},       {"PrintScreen", 316}, {"SysReq", 317},
        {"Break", 318},      {"Menu", 319},       {"Application", 319}, {"Power", 320},
        {"Euro", 321},       {"Undo", 322}};
    if (const auto found = names.find(plain); found != names.end()) {
        return found->second;
    }
    if (plain.size() == 8 && plain.starts_with("Keypad ") && plain[7] >= '0' && plain[7] <= '9') {
        return plain[7];
    }
    for (int i = 1; i <= 15; ++i) {
        if (plain == "F" + std::to_string(i)) {
            return 281 + i;
        }
    }
    return -1;
}

bool Menu::controlKey(const std::string& key) {
    if (modal_ != "controls") {
        return false;
    }
    if (originalKey(key) == 27) {
        if (controlReset_) {
            controlReset_ = false;
        } else if (!controlEditing_.empty()) {
            controlEditing_.clear();
        } else {
            modal_ = "options";
        }
        return true;
    }
    if (!controlEditing_.empty() && !controlReset_) {
        const auto code = originalKey(key);
        if (code >= 0 && code != 303 && code != 304 && code != 290 && code != 291 && code != 293) {
            controls_.setHotkey(controlEditing_, code);
            syncOptions();
            controlEditing_.clear();
        }
    }
    return true;
}

std::string Menu::gameplayKey(const std::string& key) const {
    // Translate supported native actions deliberately; unsupported original actions stay inert.
    const int code = originalKey(key);
    if (code <= 0) {
        return {};
    }
    const auto& native = supportedHotkeys();
    for (const auto& [action, binding] : controls_.hotkeys) {
        if (binding == code && native.contains(action)) {
            const auto& mapped = native.at(action);
            return key.starts_with("Shift+") && !mapped.starts_with("Shift+") ? "Shift+" + mapped
                                                                              : mapped;
        }
    }
    return {};
}

bool Menu::recordRun() {
    const auto state = run_.state();
    if (run_.tutorialRunning() || run_.terminalRecorded() ||
        !state.at("combat").at("defeated").get<bool>()) {
        return true;
    }
    if (!savePath_.empty() && profilePath_.empty()) {
        supportError_ = "native_profile_unavailable";
        return false;
    }
    const auto previousProfile = profile_;
    const auto previousIdentity = run_.profileRunId();
    const auto previousStarted = run_.profileRunStarted();
    const auto destination = profilePath_;
    // Stage lifetime accounting without a disk commit. Terminal accounting and
    // high scores become visible in one profile transaction.
    const bool duplicate =
        previousIdentity != 0 &&
        previousIdentity <= profile_.value("last_terminal_run", std::uint64_t{0});
    profilePath_.clear();
    const bool staged = duplicate || syncRunProgression(true);
    profilePath_ = destination;
    if (!staged) {
        return false;
    }
    auto updated = profile_;
    profile_ = previousProfile;
    const auto rollback = [&] {
        run_.profileRunId(previousIdentity);
        run_.profileRunStarted(previousStarted);
    };
    const auto identity = run_.profileRunId();
    int recentTopRank = -1;
    int recentShipRank = -1;
    if (!duplicate) {
        const auto add = [&](const std::string& key, int value) {
            const auto total = static_cast<std::int64_t>(updated.at(key).get<int>()) + value;
            updated[key] =
                static_cast<int>(std::min<std::int64_t>(total, std::numeric_limits<int>::max()));
        };
        add("runs", 1);
        updated["best_sector"] = std::max(profile_.at("best_sector").get<int>(),
                                          state.at("voyage").at("sector").get<int>());
        const auto& inputs = state.at("combat").at("score_inputs");
        const auto baseScore = static_cast<std::int64_t>(inputs.at("scrap").get<int>()) +
                               10 * static_cast<std::int64_t>(inputs.at("beacons").get<int>()) +
                               20 * static_cast<std::int64_t>(inputs.at("ships").get<int>());
        const int difficulty = state.at("difficulty").get<int>();
        const float multiplier = difficulty == 1 ? 1.25f : difficulty == 2 ? 1.5f : 1.f;
        const double scaled = multiplier * static_cast<float>(baseScore);
        const int score =
            static_cast<int>(std::clamp(scaled, 0., double(std::numeric_limits<int>::max())));
        if (score > 0) {
            const Json row{{"name", state.at("ship_name")},
                           {"blueprint", state.at("ship")},
                           {"score", score},
                           {"sector", state.at("voyage").at("sector")},
                           {"victory", run_.campaignVictory()},
                           {"difficulty", difficulty},
                           {"advanced", state.at("advanced_edition")}};
            const auto insert = [&](Json& rows, std::size_t limit) {
                if (rows.is_null()) {
                    rows = Json::array();
                }
                auto position = rows.begin();
                while (position != rows.end() && position->at("score").get<int>() >= score) {
                    ++position;
                }
                const auto rank = static_cast<int>(position - rows.begin());
                rows.insert(position, row);
                if (rows.size() > limit) {
                    rows.erase(rows.begin() + static_cast<Json::difference_type>(limit),
                               rows.end());
                }
                // The common original insert retains five entries before
                // the per-ship caller trims to four.
                return rank < 5 ? rank : -1;
            };
            recentTopRank = insert(updated["top_scores"], 5);
            recentShipRank = insert(updated["ship_scores"][state.at("ship").get<std::string>()], 4);
            if (!updated.contains("best_run") ||
                updated.at("best_run").at("score").get<int>() < score) {
                updated["best_run"] = row;
            }
        }
        updated["progression"] = run_.progressionState();
        updated.erase("active_run");
    }
    updated["last_terminal_run"] =
        std::max(identity, updated.value("last_terminal_run", std::uint64_t{0}));
    // Move Continue out of the active slot before committing. A failed profile
    // write restores the exact checkpoint and run-accounting state.
    const std::filesystem::path checkpoint(savePath_);
    const std::filesystem::path retired(savePath_ + ".terminal-pending");
    bool moved = false;
    try {
        if (!savePath_.empty() && std::filesystem::exists(checkpoint)) {
            if (std::filesystem::exists(retired)) {
                throw std::runtime_error("native_terminal_backup_exists");
            }
            std::filesystem::rename(checkpoint, retired);
            moved = true;
        }
        if (!commitProfile(std::move(updated))) {
            throw std::runtime_error(supportError_);
        }
    } catch (const std::exception& error) {
        supportError_ = error.what();
        if (moved) {
            std::error_code restore;
            std::filesystem::rename(retired, checkpoint, restore);
            if (restore) {
                supportError_ = "native_terminal_checkpoint_restore_failed";
            }
        }
        rollback();
        return false;
    }
    run_.markTerminalRecorded();
    if (!duplicate) {
        statsRecentTopRank_ = recentTopRank;
        statsRecentShipRank_ = recentShipRank;
        if (recentShipRank != -1) {
            const auto blueprint = state.at("ship").get<std::string>();
            for (int family = 0; family < 10; ++family) {
                for (int layout = 0; layout < 3; ++layout) {
                    const auto name = std::string(BlueprintCatalog::families[family]) +
                                      (layout ? "_" + std::to_string(layout + 1) : "");
                    if (blueprint == name) {
                        statsRecentFamily_ = family;
                        statsRecentLayout_ = layout;
                    }
                }
            }
        }
    }
    hasSave_ = false;
    buttons_[0].active = false;
    if (moved) {
        std::error_code cleanup;
        std::filesystem::remove(retired, cleanup);
    }
    supportError_.clear();
    return true;
}

void Menu::options(const Settings& settings) {
    controls_.vsync = settings.vsync;
    controls_.frameLimit = settings.frameLimit;
    controls_.focusPause = settings.focusPause;
    controls_.fullscreen = settings.fullscreen;
    controls_.lastFullscreen = settings.lastFullscreen;
    controls_.difficulty = settings.difficulty;
    controls_.gameplay = settings.gameplay;
    renderer_.setVsync(settings.vsync);
    if (!renderer_.setFullscreen(settings.fullscreen)) {
        const std::string failure = SDL_GetError();
        // CApp::OnInit403430 tries desktop Stretch when Native is unavailable,
        // then windowed mode. Keep desired/applied synchronized after fallback.
        if (settings.fullscreen == 3 && renderer_.setFullscreen(1)) {
            supportError_ = texts_.get("error_native_fullscreen", locale_);
        } else {
            renderer_.setFullscreen(0);
            supportError_ = failure;
        }
        controls_.fullscreen = renderer_.fullscreen();
    }
    syncOptions();
}

void Menu::syncOptions() {
    run_.options(controls_.gameplay);
    run_.applicationLanguage(locale_);
    run_.pauseKeyName(Settings::keyName(controls_.hotkeys.at("pause"), texts_, locale_));
    std::array<std::string, 8> crewKeys;
    for (std::size_t i = 0; i < crewKeys.size(); ++i) {
        crewKeys[i] = Settings::keyName(
            controls_.hotkeys.at("crew" + std::to_string(i + 1)), texts_, locale_);
    }
    run_.crewKeyNames(std::move(crewKeys));
    std::array<int, 8> weaponLabels;
    std::array<int, 8> droneLabels;
    weaponLabels.fill(-1);
    droneLabels.fill(-1);
    // Armament boxes show a numeric label only for the matching number key.
    // Settings' original SDL1 key values use ASCII for these bindings.
    for (std::size_t i = 0; i < weaponLabels.size(); ++i) {
        const auto key = controls_.hotkeys.find("weapon" + std::to_string(i + 1));
        if (key != controls_.hotkeys.end() && key->second == 49 + int(i)) {
            weaponLabels[i] = int(i) + 1;
        }
    }
    for (std::size_t i = 0; i < droneLabels.size(); ++i) {
        const auto key = controls_.hotkeys.find("drone" + std::to_string(i + 1));
        if (key != controls_.hotkeys.end() && key->second == 53 + int(i)) {
            droneLabels[i] = int(i) + 5;
        }
    }
    run_.armamentKeyLabels(weaponLabels, droneLabels);
}

Json Menu::optionsState() const {
    return {{"music", musicVolume_},
            {"sound", soundVolume_},
            {"vsync_desired", controls_.vsync},
            {"vsync_applied", renderer_.vsync()},
            {"vsync_available", renderer_.vsyncAvailable()},
            {"frame_limit", controls_.frameLimit},
            {"focus_pause", controls_.focusPause},
            {"fullscreen_desired", controls_.fullscreen},
            {"last_fullscreen", controls_.lastFullscreen},
            {"fullscreen_applied", renderer_.fullscreen()},
            {"restart_required", (controls_.fullscreen == 3) != (renderer_.fullscreen() == 3)},
            {"lowend", controls_.gameplay.lowend},
            {"colorblind", controls_.gameplay.colorblind},
            {"event_choice_keys", controls_.gameplay.dialogKeys},
            {"show_paths", controls_.gameplay.showPaths},
            {"achievement_popups", controls_.gameplay.achievementPopups},
            {"render_cap", renderCap_},
            {"cap_from_option", capFromOption_},
            {"focused", focused_},
            {"focus_pause_owned", focusPauseOwned_},
            {"focus_previous_pause", focusPreviousPause_}};
}

void Menu::focus(bool focused) {
    // CApp::OnInputBlur401630 / OnInputFocus401590, windowed branch.
    // Native platform recovery: preserve Equipment::MouseUp525350's cleared
    // held-state invariant when focus changes prevent delivery of mouse-up.
    if (!focused) {
        cancelPointer();
    }
    if (focused == focused_) {
        return;
    }
    focused_ = focused;
    if (!focused) {
        run_.key("Release:Hotkey:force_autofire");
        if (run_.isOpen() && (controls_.focusPause || renderer_.fullscreen() != 0)) {
            focusPreviousPause_ = run_.userPaused();
            focusPauseOwned_ = true;
            run_.userPaused(true);
        }
    } else if (focusPauseOwned_) {
        if (run_.isOpen()) {
            run_.userPaused(focusPreviousPause_);
        }
        focusPauseOwned_ = false;
    }
}

bool Menu::supportAction(const std::string& target) {
    logging::verbose("action", "menu support '{}' (modal '{}')", target, modal_);
    if (modal_ == "options_delete") {
        if (target == "option:delete_cancel" || target == "support:close" ||
            (target == "option:delete_confirm" && wipeNativeProfile())) {
            modal_ = "options";
        }
        return true;
    }
    if (modal_ == "options_restart") {
        if (target == "option:restart_continue") {
            modal_.clear();
        }
        return true;
    }
    if (modal_ == "stats") {
        if (statsShipAction(target)) {
            return true;
        }
        if (statsShipList_) {
            return true;
        }
        if (target == "stats:top_scores" || target == "stats:ship_best") {
            statsShipBest_ = target == "stats:ship_best";
            return true;
        }
        if (target == "stats:totals" || target == "stats:achievements") {
            statsAchievements_ = target == "stats:achievements";
            return true;
        }
    }
    if (modal_ == "controls") {
        if (target.starts_with("control:bind:")) {
            const auto action = target.substr(13);
            if (!controlReset_ && supportedHotkeys().contains(action)) {
                controlEditing_ = action;
            }
            return true;
        }
        if (target == "control:reset") {
            controlEditing_.clear();
            controlReset_ = true;
            return true;
        }
        if (target == "control:confirm" || target == "control:cancel") {
            if (controlReset_ && target == "control:confirm") {
                controls_.resetHotkeys();
                syncOptions();
            }
            controlReset_ = false;
            return true;
        }
        if (target == "control:next" || target == "control:previous") {
            if (!controlReset_) {
                controlPage_ = std::clamp(controlPage_ + (target == "control:next" ? 1 : -1), 0, 3);
                controlEditing_.clear();
            }
            return true;
        }
        if (target.starts_with("control:tab:") && !controlReset_) {
            for (int tab = 0; tab < 4; ++tab) {
                if (target == "control:tab:" + std::to_string(tab)) {
                    controlPage_ = tab;
                    controlEditing_.clear();
                }
            }
            return true;
        }
    }
    if (target == "support:close" && !modal_.empty() && modal_ != "language_selection" &&
        modal_ != "new_game_confirmation") {
        if (modal_ == "controls") {
            controlEditing_.clear();
            controlReset_ = false;
            modal_ = pauseControlsDirect_ ? "" : "options";
            pauseControlsDirect_ = false;
        } else {
            if (modal_ == "options" &&
                (controls_.fullscreen == 3) == (renderer_.fullscreen() == 3) &&
                !renderer_.setFullscreen(controls_.fullscreen)) {
                supportError_ = SDL_GetError();
                controls_.fullscreen = renderer_.fullscreen();
                return true;
            }
            modal_ =
                modal_ == "options" && (controls_.fullscreen == 3) != (renderer_.fullscreen() == 3)
                    ? "options_restart"
                    : "";
        }
        return true;
    }
    if (modal_ == "options") {
        if (target == "option:delete_profile") {
            if (run_.isOpen()) {
                return true;
            }
            modal_ = "options_delete";
            return true;
        }
        if (target == "option:fullscreen") {
            controls_.fullscreen = (controls_.fullscreen + 1) % 4;
            return true;
        }
        if (target == "option:dynamic_background" || target == "option:colorblind" ||
            target == "option:show_paths" || target == "option:achievement_popup" ||
            target == "option:event_choice") {
            auto& flags = controls_.gameplay;
            if (target == "option:dynamic_background") {
                flags.lowend = !flags.lowend;
            } else if (target == "option:colorblind") {
                flags.colorblind = !flags.colorblind;
            } else if (target == "option:show_paths") {
                flags.showPaths = !flags.showPaths;
            } else if (target == "option:achievement_popup") {
                flags.achievementPopups = !flags.achievementPopups;
            } else {
                flags.dialogKeys = (flags.dialogKeys + 1) % 3;
            }
            syncOptions();
            return true;
        }
        if (target == "option:frame_limit") {
            controls_.frameLimit = !controls_.frameLimit;
            renderCap_ = controls_.frameLimit ? 60 : 0;
            capFromOption_ = true;
            return true;
        }
        if (target == "option:focus_pause") {
            controls_.focusPause = !controls_.focusPause;
            return true;
        }
        if (target == "option:vsync") {
            if (renderer_.vsyncAvailable()) {
                const bool desired = !controls_.vsync;
                if (renderer_.setVsync(desired)) {
                    controls_.vsync = desired;
                } else {
                    supportError_ = "VSync unavailable: " + std::string(SDL_GetError());
                }
            }
            return true;
        }
        if (target == "option:music_down") {
            volumes(std::max(0.f, musicVolume_ - .1f), soundVolume_);
            return true;
        }
        if (target == "option:music_up") {
            volumes(std::min(1.f, musicVolume_ + .1f), soundVolume_);
            return true;
        }
        if (target == "option:sound_down") {
            volumes(musicVolume_, std::max(0.f, soundVolume_ - .1f));
            return true;
        }
        if (target == "option:sound_up") {
            volumes(musicVolume_, std::min(1.f, soundVolume_ + .1f));
            return true;
        }
        if (target == "option:language") {
            optionsLanguage_ = true;
            modal_ = "language_selection";
            return true;
        }
        if (target == "option:controls") {
            modal_ = "controls";
            controlEditing_.clear();
            controlReset_ = false;
            return true;
        }
    }
    return false;
}

void Menu::supportMove(float x, float y) {
    if (modal_ == "options" && supportSlider_ >= 0) {
        const float marker =
            std::clamp(supportSliderMarker_ + x - supportSliderMouse_, 562.f, 737.f);
        const float volume = (float(int(marker)) - 562.f) / 175.f;
        volumes(supportSlider_ == 1 ? volume : musicVolume_,
                supportSlider_ == 0 ? volume : soundVolume_);
    }
    (void)y;
}

void Menu::supportClick(float x, float y) {
    if (modal_ == "options_delete") {
        const auto layout = controlConfirmation();
        if (layout.yes.contains(x, y)) {
            supportAction("option:delete_confirm");
        } else if (layout.no.contains(x, y)) {
            supportAction("option:delete_cancel");
        }
        return;
    }
    if (modal_ == "options_restart") {
        if (Rect{352, 240, 565, 30}.contains(x, y)) {
            supportAction("option:restart_continue");
        }
        return;
    }
    if (modal_ == "stats") {
        // ScoreKeeper::OnInit449110 and MouseClick4455b0.
        for (const auto& [action, rect] : statsShipControls()) {
            if (rect.contains(x, y)) {
                supportAction(action);
                return;
            }
        }
        if (statsShipList_) {
            return;
        }
        const auto closeLabel = texts_.get("button_close", locale_);
        const auto [closeLeft, closeRight] = renderer_.bounds(
            localizedFont(64, closeLabel), closeLabel, locale_ == "zh-Hans" ? 1.f : 4.f);
        const float closeWidth = std::max(138.f, std::round(closeRight - closeLeft) + 8);
        const std::array<std::pair<Rect, const char*>, 5> buttons{
            {{{43, 38, 302, 53}, "stats:top_scores"},
             {{338, 38, 291, 53}, "stats:ship_best"},
             {{651, 38, 193, 53}, "stats:totals"},
             {{838, 38, 387, 53}, "stats:achievements"},
             {{1155.f - float(int(closeWidth) / 2), 627, closeWidth, 46}, "support:close"}}};
        for (const auto& [rect, action] : buttons) {
            if (rect.contains(x, y)) {
                supportAction(action);
                return;
            }
        }
        return;
    }
    if (modal_ == "controls" || modal_ == "options") {
        const auto close = supportButtonRect(texts_.get("button_close", locale_),
                                             modal_ == "controls" ? 1190.f : 1019.f,
                                             modal_ == "controls" ? 619.f : 441.f,
                                             103);
        if (close.contains(x, y)) {
            supportAction("support:close");
            return;
        }
    } else if (Rect{930, 125, 80, 35}.contains(x, y)) {
        supportAction("support:close");
        return;
    }
    if (modal_ == "controls") {
        if (controlReset_) {
            const auto layout = controlConfirmation();
            if (layout.yes.contains(x, y)) {
                supportAction("control:confirm");
            } else if (layout.no.contains(x, y)) {
                supportAction("control:cancel");
            }
            return;
        }
        for (int tab = 0; tab < 4; ++tab) {
            if (Rect{944.f + 66.f * tab, 83, 64, 29}.contains(x, y)) {
                supportAction("control:tab:" + std::to_string(tab));
                return;
            }
        }
        for (const auto& [action, box] : controlRows()) {
            if (box.contains(x, y)) {
                supportAction("control:bind:" + action);
                return;
            }
        }
        const auto defaults =
            supportButtonRect(texts_.get("button_defaults", locale_), 325, 619, 165);
        if (defaults.contains(x, y)) {
            supportAction("control:reset");
        }
    } else if (modal_ == "options") {
        float width = 131;
        std::istringstream lines(texts_.get("button_delete_profile", locale_));
        std::string line;
        while (std::getline(lines, line)) {
            const auto [left, right] =
                renderer_.bounds(localizedFont(63, line), line, locale_ == "zh-Hans" ? 1.f : 3.f);
            width = std::max(width, std::round(right - left) + 8);
        }
        if (!run_.isOpen() &&
            Rect{251.f - float(int(width - 131) / 2), 474, width, 60}.contains(x, y)) {
            supportAction("option:delete_profile");
            return;
        }

        if (Rect{557, 476, 200, 30}.contains(x, y) || Rect{557, 526, 200, 30}.contains(x, y)) {
            supportSlider_ = y < 516 ? 0 : 1;
            supportSliderMouse_ = x;
            const float volume = supportSlider_ == 0 ? soundVolume_ : musicVolume_;
            const float marker = float(int(562.f + volume * 175.f));
            supportSliderMarker_ =
                Rect{marker, supportSlider_ == 0 ? 476.f : 526.f, 15, 30}.contains(x, y) ? marker
                                                                                         : x - 7;
            supportMove(x, y);
            return;
        }
        if (Rect{262, 217, 370, 34}.contains(x, y)) {
            supportAction("option:fullscreen");
        } else if (Rect{262, 319, 370, 34}.contains(x, y)) {
            supportAction("option:dynamic_background");
        } else if (Rect{262, 353, 370, 34}.contains(x, y)) {
            supportAction("option:colorblind");
        } else if (Rect{662, 217, 370, 34}.contains(x, y)) {
            supportAction("option:event_choice");
        } else if (Rect{662, 251, 370, 34}.contains(x, y)) {
            supportAction("option:show_paths");
        } else if (Rect{662, 285, 370, 34}.contains(x, y)) {
            supportAction("option:achievement_popup");
        } else if (Rect{262, 387, 370, 34}.contains(x, y)) {
            supportAction("option:language");
        } else if (Rect{262, 251, 370, 34}.contains(x, y)) {
            supportAction("option:vsync");
        } else if (Rect{262, 285, 370, 34}.contains(x, y)) {
            supportAction("option:frame_limit");
        } else if (controls_.fullscreen == 0 && Rect{662, 319, 370, 34}.contains(x, y)) {
            supportAction("option:focus_pause");
        } else if (Rect{662, controls_.fullscreen == 0 ? 353.f : 319.f, 370, 34}.contains(x, y)) {
            supportAction("option:controls");
        }
    }
}

void Menu::optionsRender() {
    // ChoiceBox::OnRender0x4ec490 two-column option frame (start327,140 minus100).
    renderer_.draw("img/optionsUI_new.png", 227, 140);
    constexpr SDL_Color ink{25, 49, 51, 255};
    supportText(texts_.get("options_tab", locale_), 353, 151, 63, true, ink);
    supportText(texts_.get("options_frame_video", locale_), 252, 187, 62, false, ink);
    supportText(texts_.get("options_frame_gameplay", locale_), 650, 187, 62, false, ink);
    supportText(texts_.get("options_frame_audio", locale_), 413, 434, 62, false, ink);
    const auto formatted = [&](const std::string& key, const std::string& setting) {
        auto label = texts_.get(key, locale_);
        const auto position = label.find("\\1");
        if (position != std::string::npos) {
            label.replace(position, 2, setting);
        }
        return label;
    };
    if ((controls_.fullscreen == 3) != (renderer_.fullscreen() == 3)) {
        supportText(
            texts_.get("restart_required", locale_), 472, 170, 13, false, {255, 255, 0, 255});
    }
    const std::array<std::string, 6> video{
        formatted("fullscreen",
                  texts_.get("fullscreen_" + std::to_string(controls_.fullscreen), locale_)),
        formatted("v_sync", texts_.get(renderer_.vsync() ? "on" : "off", locale_)),
        formatted("frame_limit", texts_.get(controls_.frameLimit ? "on" : "off", locale_)),
        formatted("dynamic_back", texts_.get(controls_.gameplay.lowend ? "off" : "on", locale_)),
        formatted("colorblind",
                  texts_.get(controls_.gameplay.colorblind ? "enabled" : "disabled", locale_)),
        texts_.get("change_language", locale_)};
    std::vector<std::string> gameplay{
        formatted("event_choice",
                  texts_.get(controls_.gameplay.dialogKeys == 0   ? "event_choice_disable_hotkeys"
                             : controls_.gameplay.dialogKeys == 1 ? "event_choice_no_delay"
                                                                  : "event_choice_brief_delay",
                             locale_)),
        formatted("show_paths",
                  texts_.get(controls_.gameplay.showPaths ? "enabled" : "disabled", locale_)),
        formatted(
            "achievement_popup",
            texts_.get(controls_.gameplay.achievementPopups ? "enabled" : "disabled", locale_)),
        formatted("window_focus", texts_.get(controls_.focusPause ? "on" : "off", locale_)),
        texts_.get("configure_controls", locale_)};
    if (controls_.fullscreen != 0) {
        gameplay.erase(gameplay.begin() + 3);
    }
    for (int column = 0; column < 2; ++column) {
        const int count = column == 0 ? 6 : static_cast<int>(gameplay.size());
        for (int row = 0; row < count; ++row) {
            const float x = 262.f + 400.f * column;
            // Font13: bitmap19-4 plus7 =22 lineHeight; choices add12 =>34.
            const float y = 227.f + 34.f * row;
            const bool active = column != 0 || row != 1 || renderer_.vsyncAvailable();
            const auto color = !active ? SDL_Color{150, 150, 150, 255}
                               : Rect{x, y - 10, 370, 34}.contains(mouseX_, mouseY_)
                                   ? SDL_Color{243, 255, 80, 255}
                                   : SDL_Color{255, 255, 255, 255};
            const auto& label = column == 0 ? video[static_cast<std::size_t>(row)]
                                            : gameplay[static_cast<std::size_t>(row)];
            supportText(label, x, y, 13, false, color);
        }
    }
    supportText(texts_.get("sound_volume", locale_), 483, 484, 13, true);
    supportText(texts_.get("music_volume", locale_), 483, 534, 13, true);
    for (int i = 0; i < 2; ++i) {
        const float y = 481.f + 50.f * i;
        renderer_.fill({557, y, 200, 20}, {0, 0, 0, 128});
        const SDL_Color white{255, 255, 255, 255};
        renderer_.fill({557, y, 200, 2}, white);
        renderer_.fill({557, y + 18, 200, 2}, white);
        renderer_.fill({557, y, 2, 20}, white);
        renderer_.fill({755, y, 2, 20}, white);
        const float volume = i == 0 ? soundVolume_ : musicVolume_;
        const float marker = float(int(562.f + volume * 175.f));
        const bool hovered = Rect{marker, y - 5, 15, 30}.contains(mouseX_, mouseY_);
        renderer_.fill({marker, y - 5, 15, 30}, hovered ? SDL_Color{255, 255, 0, 255} : white);
    }
    const auto closeText = texts_.get("button_close", locale_);
    // OptionsScreen571ad0/5726c0: Delete Profile exists only from main menu.
    if (!run_.isOpen()) {
        // OptionsScreen::OnInit570240:131x60,inset8, centered auto-width margin4;
        // TextButton::OnRender577750: lineHeight26,textYOffset-2. Native profile
        // deletion confirms before committing only the native profile.
        std::vector<std::string> deleteLines;
        std::istringstream deleteText(texts_.get("button_delete_profile", locale_));
        std::string deleteLine;
        float deleteWidth = 131;
        const float deleteScale = locale_ == "zh-Hans" ? 1.f : 3.f;
        while (std::getline(deleteText, deleteLine)) {
            const auto [left, right] =
                renderer_.bounds(localizedFont(63, deleteLine), deleteLine, deleteScale);
            deleteWidth = std::max(deleteWidth, std::round(right - left) + 8);
            deleteLines.push_back(deleteLine);
        }
        const float deleteX = 251.f - float(int(deleteWidth - 131) / 2);
        auto& deleteBase = renderer_.image("img/optionsUI/button_deleteprofile_base.png");
        const float half = std::floor(float(deleteBase.w) / 2);
        const float extra = deleteWidth - 131;
        renderer_.sprite(deleteBase, {deleteX - 15, 459, half, 90}, {0, 0, half, 90});
        if (extra > 0) {
            renderer_.sprite(deleteBase, {deleteX - 15 + half, 459, extra, 90}, {half, 0, 1, 90});
        }
        renderer_.sprite(deleteBase,
                         {deleteX - 15 + half + extra, 459, float(deleteBase.w) - half, 90},
                         {half, 0, float(deleteBase.w) - half, 90});
        const SDL_Color deleteColor = Rect{deleteX, 474, deleteWidth, 60}.contains(mouseX_, mouseY_)
                                          ? SDL_Color{255, 255, 50, 255}
                                          : SDL_Color{235, 245, 229, 255};
        for (int row = 0; row < 60; ++row) {
            const float inset = float(std::max(0, 8 - std::min(row, 59 - row)));
            renderer_.fill({deleteX + inset, 474.f + float(row), deleteWidth - 2 * inset, 1},
                           deleteColor);
        }
        float deleteY = 502.f - 26.f * float(deleteLines.size()) / 2;
        for (const auto& line : deleteLines) {
            supportText(line, deleteX + float(int(deleteWidth) / 2), deleteY, 63, true, ink);
            deleteY += 26;
        }
    }
    supportButton(supportButtonRect(closeText, 1019, 441, 103),
                  closeText,
                  "img/storeUI/store_close_base.png");
    if (!supportError_.empty()) {
        supportText(supportError_, 640, 603, 10, true);
    }
}

Menu::SupportConfirmation Menu::controlConfirmation() {
    // ConfirmWindow544870/545ed0, ControlsScreen::SetText(minimum300).
    SupportConfirmation result;
    const auto yesText = texts_.get("confirm_yes", locale_);
    const auto noText = texts_.get("confirm_no", locale_);
    const float scale = locale_ == "zh-Hans" ? 1.f : 2.f;
    const auto yesBounds = renderer_.bounds(localizedFont(62, yesText), yesText, scale);
    const auto noBounds = renderer_.bounds(localizedFont(62, noText), noText, scale);
    result.yes.w = std::round(yesBounds.second - yesBounds.first) + 6;
    result.no.w = std::round(noBounds.second - noBounds.first) + 6;
    result.yes.h = result.no.h = 24;
    result.body.w =
        std::max(modal_ == "options_delete" ? 240.f : 300.f, result.yes.w + result.no.w + 100);
    const auto message = texts_.get(
        modal_ == "options_delete" ? "confirm_wipe_profile" : "confirm_reset_keys", locale_);
    auto& bodyFont = localizedFont(52, message);
    result.lines =
        renderer_.wrappedLines(bodyFont, message, result.body.w - 40, scale, locale_ == "zh-Hans");
    result.body.h =
        static_cast<float>(result.lines.size()) * (Renderer::sourceLineHeight(52, bodyFont));
    result.body.x = float(int(1280 - result.body.w) / 2);
    const auto& frame = renderer_.image("img/confirm_window.png");
    result.body.y = float(int(720 - (float(frame.h) + result.body.h - 22)) / 2);
    result.yes.x =
        result.body.x + float(int(result.body.w) / 2 - int(result.yes.w + result.no.w + 14) / 2);
    result.no.x = result.yes.x + result.yes.w + 14;
    result.yes.y = result.no.y = result.body.y + result.body.h + 28;
    return result;
}

void Menu::controlConfirmationRender() {
    const auto layout = controlConfirmation();
    const auto& [x, y, w, h] = layout.body;
    auto& frame = renderer_.image("img/confirm_window.png");
    const std::array<float, 3> sx{0, 70, 71};
    const std::array<float, 3> sw{70, 1, 69};
    const std::array<float, 3> dx{x, x + 70, x + w - 69};
    const std::array<float, 3> dw{70, w - 139, 69};
    for (std::size_t i = 0; i < 3; ++i) {
        const Rect top{sx[i], 0, sw[i], 26};
        const Rect middle{sx[i], 26, sw[i], 1};
        const Rect bottom{sx[i], 27, sw[i], 17};
        renderer_.draw(frame, {dx[i], y, dw[i], 26}, &top);
        renderer_.draw(frame, {dx[i], y + 26, dw[i], h - 21}, &middle);
        renderer_.draw(frame, {dx[i], y + h + 5, dw[i], 17}, &bottom);
    }
    const float end = layout.no.x + layout.no.w;
    const std::array<float, 9> sourceX{0, 20, 21, 52, 53, 87, 88, 119, 120};
    const std::array<float, 9> sourceWidth{20, 1, 31, 1, 34, 1, 31, 1, 20};
    const std::array<float, 9> destinationX{x,
                                            x + 20,
                                            layout.yes.x - 21,
                                            layout.yes.x + 10,
                                            layout.yes.x + layout.yes.w - 10,
                                            layout.no.x + 10,
                                            end - 10,
                                            end + 21,
                                            x + w - 20};
    const std::array<float, 9> destinationWidth{20,
                                                layout.yes.x - x - 41,
                                                31,
                                                layout.yes.w - 20,
                                                34,
                                                layout.no.w - 20,
                                                31,
                                                x + w - 20 - (end + 21),
                                                20};
    for (std::size_t i = 0; i < 9; ++i) {
        const Rect source{sourceX[i], 44, sourceWidth[i], 46};
        renderer_.draw(frame, {destinationX[i], y + h + 22, destinationWidth[i], 46}, &source);
    }
    float lineY = y + 17;
    for (const auto& line : layout.lines) {
        auto& font = localizedFont(52, line);
        renderer_.text(font, line, x + w / 2, lineY, fontScale(), {235, 245, 229, 255}, true);
        lineY += Renderer::sourceLineHeight(52, font);
    }
    textButton(
        layout.yes, texts_.get("confirm_yes", locale_), layout.yes.contains(mouseX_, mouseY_));
    textButton(layout.no, texts_.get("confirm_no", locale_), layout.no.contains(mouseX_, mouseY_));
}

void Menu::controlsRender() {
    // ControlsScreen::OnInit0x56f490 and OnRender0x570d70.
    if (controlReset_) {
        renderer_.setTint(.5f);
    }
    renderer_.windowFrame({130, 112, 1089, 500});
    const auto title = texts_.get("configure_controls_tab", locale_);
    const float scale = locale_ == "zh-Hans" ? 1.f : 3.f;
    const float width = std::round(renderer_.measure(localizedFont(63, title), title, scale));
    auto& strip = renderer_.image("img/box_options_configure_tab.png");
    const Rect left{0, 0, 39, float(strip.h)};
    const Rect middle{39, 0, 1, float(strip.h)};
    const Rect right{40, 0, 51, float(strip.h)};
    renderer_.draw(strip, {123, 70, 39, float(strip.h)}, &left);
    renderer_.draw(strip, {162, 70, width - 16, float(strip.h)}, &middle);
    renderer_.draw(strip, {width + 146, 70, 51, float(strip.h)}, &right);
    supportText(title, 154, 81, 63, false, {25, 49, 51, 255});
    for (const auto& [action, box] : controlRows()) {
        const bool active = supportedHotkeys().contains(action);
        const bool selected = controlEditing_ == action;
        const bool hover = active && box.contains(mouseX_, mouseY_);
        const auto color = !active             ? SDL_Color{150, 150, 150, 255}
                           : selected || hover ? SDL_Color{255, 255, 50, 255}
                                               : SDL_Color{255, 255, 255, 255};
        renderer_.fill(box, selected ? SDL_Color{125, 125, 125, 128} : SDL_Color{50, 50, 50, 128});
        renderer_.fill({box.x, box.y, box.w, 2}, color);
        renderer_.fill({box.x, box.y + box.h - 2, box.w, 2}, color);
        renderer_.fill({box.x, box.y, 2, box.h}, color);
        renderer_.fill({box.x + box.w - 2, box.y, 2, box.h}, color);
        auto label = texts_.get("hotkey_" + action, locale_);
        const float descriptionWidth =
            float(int(renderer_.measure(localizedFont(13, label), label)));
        supportText(label, box.x - descriptionWidth - 5, box.y + 6, 13, false, color);
        auto keyName = Settings::keyName(controls_.hotkeys.at(action), texts_, locale_);
        if (!active) {
            keyName = "Inactive";
        } else if (selected) {
            keyName = texts_.get("press_key", locale_);
        }
        supportText(keyName, box.x + box.w / 2 + 1, box.y + 6, 12, true, color);
    }
    const auto defaults = texts_.get("button_defaults", locale_);
    supportButton(
        supportButtonRect(defaults, 325, 619, 165), defaults, "img/optionsUI/defaults_base.png");
    for (int tab = 0; tab < 4; ++tab) {
        const bool hover = Rect{944.f + 66.f * tab, 83, 64, 29}.contains(mouseX_, mouseY_);
        const auto asset = tab == controlPage_ ? "img/optionsUI/tab_page_on.png"
                           : hover             ? "img/optionsUI/tab_page_select2.png"
                                               : "img/optionsUI/tab_page_off.png";
        renderer_.draw(asset, 939.f + 66.f * tab, 83);
        supportText(std::to_string(tab + 1), 976.f + 66.f * tab, 90, 62, true, {25, 49, 51, 255});
    }
    if (controlPage_ > 1) {
        supportText(texts_.get("power_note", locale_), 467, 625, 12);
    }
    if (controlReset_) {
        renderer_.setTint(1);
        controlConfirmationRender();
    }
    // OptionsScreen renders its shared close button after ControlsScreen.
    const auto close = texts_.get("button_close", locale_);
    supportButton(
        supportButtonRect(close, 1190, 619, 103), close, "img/storeUI/store_close_base.png");
}

void Menu::supportRender() {
    if (modal_ != "options" && modal_ != "controls" && modal_ != "stats" &&
        modal_ != "support_error" && modal_ != "options_restart" && modal_ != "options_delete") {
        return;
    }
    if (modal_ == "options_delete") {
        renderer_.setTint(.25f);
        optionsRender();
        renderer_.setTint(1);
        controlConfirmationRender();
        return;
    }
    if (modal_ == "options_restart") {
        renderer_.setTint(.25f);
        optionsRender();
        renderer_.setTint(1);
        // ChoiceBox::OnRender4ec490: start327,125 +frame7,22,602,377.
        renderer_.windowFrame({334, 147, 602, 377});
        auto& font = localizedFont(12, texts_.get("restart_required_dialog", locale_));
        float y = 170;
        const auto lines = renderer_.wrappedLines(
            font, texts_.get("restart_required_dialog", locale_), 565, 1, locale_ == "zh-Hans");
        for (const auto& line : lines) {
            supportText(line, 352, y, 12);
            y += Renderer::sourceLineHeight(12, font);
        }
        const auto color = Rect{352, 240, 565, 30}.contains(mouseX_, mouseY_)
                               ? SDL_Color{243, 255, 80, 255}
                               : SDL_Color{255, 255, 255, 255};
        supportText("1. " + texts_.get("continue", locale_), 352, 250, 12, false, color);
        return;
    }
    if (modal_ == "options") {
        optionsRender();
        return;
    }
    if (modal_ == "controls") {
        controlsRender();
        return;
    }
    if (modal_ == "stats") {
        // ScoreKeeper::OnInit449110/OnRender4459d0: independently switched
        renderer_.setTint(statsShipList_ ? .25f : 1.f);
        // left/right panels and localized labels. Unrecorded high scores and
        // achievements remain empty instead of inventing original profile data.
        renderer_.draw(statsShipBest_ ? "img/scoreUI/score_main_left_2.png"
                                      : "img/scoreUI/score_main_left_1.png",
                       0,
                       0);
        renderer_.draw(statsAchievements_ ? "img/scoreUI/score_main_right_2.png"
                                          : "img/scoreUI/score_main_right_1.png",
                       645,
                       0);
        const auto inactiveTab = [&](const std::string& base, Rect rect) {
            renderer_.draw(base + (rect.contains(mouseX_, mouseY_) ? "_select2.png" : "_off.png"),
                           rect.x,
                           rect.y);
        };
        inactiveTab(statsShipBest_ ? "img/scoreUI/tab_topscores" : "img/scoreUI/tab_shipbest",
                    statsShipBest_ ? Rect{43, 38, 302, 53} : Rect{338, 38, 291, 53});
        inactiveTab(statsAchievements_ ? "img/scoreUI/tab_stats" : "img/scoreUI/tab_achieve",
                    statsAchievements_ ? Rect{651, 38, 193, 53} : Rect{838, 38, 387, 53});
        constexpr SDL_Color ink{25, 49, 51, 255};
        const std::array<std::pair<const char*, float>, 4> labels{{{"stats_topscores", 188.f},
                                                                   {"stats_shipbest", 477.f},
                                                                   {"stats_stats", 753.f},
                                                                   {"stats_achievements", 1023.f}}};
        const std::array<float, 4> maximumWidths{254, 218, 134, 324};
        int labelFont = 64;
        float labelY = 46;
        for (; labelFont > 62; --labelFont, labelY += 4) {
            bool fits = true;
            for (std::size_t i = 0; i < labels.size(); ++i) {
                const auto label = texts_.get(labels[i].first, locale_);
                const float scale = locale_ == "zh-Hans" ? 1.f : float(labelFont - 60);
                const auto [left, right] =
                    renderer_.bounds(localizedFont(labelFont, label), label, scale);
                fits = fits && right - left <= maximumWidths[i];
            }
            if (fits) {
                break;
            }
        }
        const auto statsLabel = texts_.get("stats_stats", locale_);
        const auto [statsLeft, statsRight] =
            renderer_.bounds(localizedFont(labelFont, statsLabel),
                             statsLabel,
                             locale_ == "zh-Hans" ? 1.f : float(labelFont - 60));
        if (statsRight - statsLeft > 134) {
            labelY += 3;
        }
        for (const auto& [key, x] : labels) {
            supportText(texts_.get(key, locale_), x, labelY, labelFont, true, ink);
        }
        statsScoresRender();
        if (!statsAchievements_) {
            statsRecordsRender();
        }
        if (statsAchievements_) {
            statsAchievementsRender();
        }
        const auto closeLabel = texts_.get("button_close", locale_);
        const auto [closeLeft, closeRight] = renderer_.bounds(
            localizedFont(64, closeLabel), closeLabel, locale_ == "zh-Hans" ? 1.f : 4.f);
        const float closeWidth = std::max(138.f, std::round(closeRight - closeLeft) + 8);
        const Rect close{1155.f - float(int(closeWidth) / 2), 627, closeWidth, 46};
        // ScoreKeeper449110: TextButton inset8/font64, base offset(-15,-15).
        auto& closeBase = renderer_.image("img/scoreUI/score_close_box.png");
        const float half = std::floor(float(closeBase.w) / 2);
        const float extra = closeWidth - 138;
        renderer_.sprite(closeBase,
                         {close.x - 15, 612, half, float(closeBase.h)},
                         {0, 0, half, float(closeBase.h)});
        if (extra > 0) {
            renderer_.sprite(closeBase,
                             {close.x - 15 + half, 612, extra, float(closeBase.h)},
                             {half, 0, 1, float(closeBase.h)});
        }
        renderer_.sprite(
            closeBase,
            {close.x - 15 + half + extra, 612, float(closeBase.w) - half, float(closeBase.h)},
            {half, 0, float(closeBase.w) - half, float(closeBase.h)});
        const bool hover = close.contains(mouseX_, mouseY_);
        for (int row = 0; row < 46; ++row) {
            const float inset = float(std::max(0, 8 - std::min(row, 45 - row)));
            renderer_.fill({close.x + inset, 627.f + float(row), closeWidth - 2 * inset, 1},
                           hover ? SDL_Color{255, 238, 128, 255} : SDL_Color{235, 245, 229, 255});
        }
        // Font64 lineHeight52/offset6:627+46/2+6-52/2=630.
        supportText(closeLabel, close.x + float(int(closeWidth) / 2), 630, 64, true, ink);
        renderer_.setTint(1);
        statsTooltipRender();
        return;
    }
    renderer_.fill({280, 110, 750, 500}, {20, 35, 40, 255});
    auto& font = localizedFont(12, modal_);
    const auto line = [&](const std::string& value, float y) {
        renderer_.text(font, value, 330, y, 1, {235, 245, 229, 255});
    };
    line(modal_, 140);
    renderer_.text(font, "Close", 930, 135, 1, {235, 245, 229, 255});
    line(supportError_, 235);
    line("Escape returns to the main menu.", 570);
}
} // namespace ftl
