#include "settings.hpp"
#include "resources.hpp"
#include <cmath>
#include <fstream>
#include <json.hpp>
#include <stdexcept>

namespace ftl {
std::string Settings::keyName(int key, const TextLibrary& texts, const std::string& locale) {
    // GetHotkeyName57cd90: SDL1 keycap TextStrings, including source empty
    // entries35/37 and the duplicated36 assignment whose final value is '$'.
    if (key >= 'a' && key <= 'z') {
        return std::string(1, char(key - 'a' + 'A'));
    }
    if ((key >= 33 && key <= 64 && key != 35 && key != 37) || (key >= 91 && key <= 96)) {
        return std::string(1, char(key));
    }
    const auto translated = [&](const std::string& id) {
        const auto value = texts.get(id, locale);
        return value.empty() ? texts.get("keycap_none", locale) : value;
    };
    if (key >= 256 && key <= 265) {
        return translated("keycap_kp" + std::to_string(key - 256));
    }
    if (key >= 282 && key <= 296) {
        return translated("keycap_f" + std::to_string(key - 281));
    }
    static const std::map<int, std::string> names{
        {0, "none"},           {8, "backspace"},     {9, "tab"},
        {12, "clear"},         {13, "enter"},        {19, "pause"},
        {27, "escape"},        {32, "space"},        {127, "delete"},
        {266, "kp_decimal"},   {267, "kp_divide"},   {268, "kp_multiply"},
        {269, "kp_subtract"},  {270, "kp_add"},      {271, "kp_enter"},
        {272, "kp_equals"},    {273, "up"},          {274, "down"},
        {275, "right"},        {276, "left"},        {277, "insert"},
        {278, "home"},         {279, "end"},         {280, "pageup"},
        {281, "pagedown"},     {300, "numlock"},     {301, "capslock"},
        {302, "scrolllock"},   {303, "rightshift"},  {304, "leftshift"},
        {305, "rightcontrol"}, {306, "leftcontrol"}, {307, "rightalt"},
        {308, "leftalt"},      {309, "rightmeta"},   {310, "leftmeta"},
        {311, "leftsuper"},    {312, "rightsuper"},  {313, "altgr"},
        {314, "compose"},      {315, "help"},        {316, "printscreen"},
        {317, "sysrq"},        {318, "break"},       {319, "menu"},
        {320, "power"},        {321, "euro"},        {322, "undo"}};
    const auto found = names.find(key);
    return translated("keycap_" + (found == names.end() ? "none" : found->second));
}

// Settings::ResetHotkeys 0x5815a0, SetHotkey 0x5802a0 (Steam 1.6.9 x86).
std::map<std::string, int> Settings::defaultHotkeys() {
    return {{"crew1", 282},
            {"crew2", 283},
            {"crew3", 284},
            {"crew4", 285},
            {"crew5", 286},
            {"crew6", 287},
            {"crew7", 288},
            {"crew8", 289},
            {"crew_all", 113},
            {"loadPositions", 13},
            {"savePositions", 47},
            {"lockdown", 112},
            {"pause", 32},
            {"jump", 106},
            {"ship_info", 117},
            {"ship_inv", 105},
            {"store", 0},
            {"ship_crew", 0},
            {"options", 111},
            {"increase_font", 45},
            {"decrease_font", 61},
            {"open", 122},
            {"close", 120},
            {"activate_cloak", 99},
            {"send_tele", 116},
            {"ret_tele", 114},
            {"start_hacking", 110},
            {"mindControl", 109},
            {"activate_battery", 98},
            {"drone1", 53},
            {"drone2", 54},
            {"drone3", 55},
            {"weapon1", 49},
            {"weapon2", 50},
            {"weapon3", 51},
            {"weapon4", 52},
            {"autofire", 118},
            {"force_autofire", 306},
            {"shields", 97},
            {"engines", 115},
            {"oxygen", 102},
            {"medbay", 100},
            {"teleporter", 103},
            {"cloaking", 104},
            {"mind", 107},
            {"hacking", 108},
            {"artillery", 121},
            {"weapons", 119},
            {"drones", 101},
            {"un_shields", 0},
            {"un_engines", 0},
            {"un_oxygen", 0},
            {"un_medbay", 0},
            {"un_teleporter", 0},
            {"un_cloaking", 0},
            {"un_mind", 0},
            {"un_hacking", 0},
            {"un_artillery", 0},
            {"un_weapons", 0},
            {"un_drones", 0},
            {"power_1", 0},
            {"power_2", 0},
            {"power_3", 0},
            {"power_4", 0},
            {"power_5", 0},
            {"power_6", 0},
            {"power_7", 0},
            {"power_8", 0},
            {"un_power_1", 0},
            {"un_power_2", 0},
            {"un_power_3", 0},
            {"un_power_4", 0},
            {"un_power_5", 0},
            {"un_power_6", 0},
            {"un_power_7", 0},
            {"un_power_8", 0}};
}

const std::array<std::vector<std::string>, 4>& Settings::hotkeyTabs() {
    // Settings::ResetHotkeys0x5815a0 vector offsets0/12/24/36.
    static const std::array<std::vector<std::string>, 4> tabs{
        {{"crew1",   "crew2",         "crew3",        "crew4",         "crew5",         "crew6",
          "crew7",   "crew8",         "crew_all",     "loadPositions", "savePositions", "lockdown",
          "pause",   "jump",          "ship_info",    "ship_inv",      "store",         "ship_crew",
          "options", "increase_font", "decrease_font"},
         {"open",
          "close",
          "activate_cloak",
          "send_tele",
          "ret_tele",
          "start_hacking",
          "mindControl",
          "activate_battery",
          "drone1",
          "drone2",
          "drone3",
          "weapon1",
          "weapon2",
          "weapon3",
          "weapon4",
          "autofire",
          "force_autofire"},
         {"shields",    "engines",      "oxygen",     "medbay",        "teleporter",  "cloaking",
          "mind",       "hacking",      "artillery",  "weapons",       "drones",      "un_shields",
          "un_engines", "un_oxygen",    "un_medbay",  "un_teleporter", "un_cloaking", "un_mind",
          "un_hacking", "un_artillery", "un_weapons", "un_drones"},
         {"power_1",
          "power_2",
          "power_3",
          "power_4",
          "power_5",
          "power_6",
          "power_7",
          "power_8",
          "un_power_1",
          "un_power_2",
          "un_power_3",
          "un_power_4",
          "un_power_5",
          "un_power_6",
          "un_power_7",
          "un_power_8"}}};
    return tabs;
}

void Settings::resetHotkeys() {
    hotkeys = defaultHotkeys();
}

void Settings::setHotkey(const std::string& action, int key) {
    if (!hotkeys.contains(action) || key < 0 || key >= 323) {
        throw std::runtime_error("invalid_hotkey");
    }
    // The original clears the previous owner of a nonzero key across all tabs.
    if (key != 0) {
        bool found = false;
        for (const auto& tab : hotkeyTabs()) {
            for (const auto& name : tab) {
                if (hotkeys.at(name) == key) {
                    hotkeys.at(name) = 0;
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
    }
    hotkeys.at(action) = key;
}

void Settings::validate() const {
    if (fullscreen < 0 || fullscreen > 3 || lastFullscreen < 0 || lastFullscreen > 3 ||
        difficulty < 0 || difficulty > 2 || gameplay.dialogKeys < 0 || gameplay.dialogKeys > 2) {
        throw std::runtime_error("invalid_option_mode");
    }
    if (!supportedLanguage(language)) {
        throw std::runtime_error("unsupported_language");
    }
    if (width < 320 || height < 180 || width > 7680 || height > 4320) {
        throw std::runtime_error("invalid_window_size");
    }
    if (!std::isfinite(musicVolume) || !std::isfinite(soundVolume) || musicVolume < 0 ||
        musicVolume > 1 || soundVolume < 0 || soundVolume > 1) {
        throw std::runtime_error("invalid_audio_volume");
    }
    const auto defaults = defaultHotkeys();
    if (hotkeys.size() != defaults.size()) {
        throw std::runtime_error("invalid_hotkeys");
    }
    for (const auto& [action, key] : hotkeys) {
        if (!defaults.contains(action) || key < 0 || key >= 323) {
            throw std::runtime_error("invalid_hotkey");
        }
    }
}

Settings Settings::read(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    if (std::filesystem::file_size(path) > 16384) {
        throw std::runtime_error("settings_too_large");
    }
    std::ifstream in(path);
    nlohmann::json j;
    in >> j;
    if (!j.is_object() || !j.contains("schema_version") ||
        !j.at("schema_version").is_number_integer() || j.at("schema_version") != 1) {
        throw std::runtime_error("unsupported_settings_schema");
    }
    const auto dimension = [&](const std::string& name, int minimum, int maximum) {
        const auto& value = j.at(name);
        const bool valid =
            value.is_number_unsigned()
                ? value.get<std::uint64_t>() <= static_cast<std::uint64_t>(maximum) &&
                      value.get<std::uint64_t>() >= static_cast<std::uint64_t>(minimum)
                : value.is_number_integer() && value.get<std::int64_t>() >= minimum &&
                      value.get<std::int64_t>() <= maximum;
        if (!valid) {
            throw std::runtime_error("invalid_window_size");
        }
        return value.get<int>();
    };
    Settings result{j.at("language").get<std::string>(),
                    dimension("window_width", 320, 7680),
                    dimension("window_height", 180, 4320)};
    result.musicVolume = j.value("music_volume", 1.f);
    result.soundVolume = j.value("sound_volume", 1.f);
    result.languageSelected = j.value("language_selected", true);
    result.vsync = j.value("vsync", true);
    result.frameLimit = j.value("frame_limit", true);
    result.focusPause = j.value("focus_pause", true);
    const auto mode = [&](const std::string& name, int maximum, int fallback) {
        if (!j.contains(name)) {
            return fallback;
        }
        const auto& value = j.at(name);
        const bool valid = value.is_number_unsigned()
                               ? value.get<std::uint64_t>() <= static_cast<std::uint64_t>(maximum)
                               : value.is_number_integer() && value.get<std::int64_t>() >= 0 &&
                                     value.get<std::int64_t>() <= maximum;
        if (!valid) {
            throw std::runtime_error("invalid_option_mode");
        }
        return value.get<int>();
    };
    result.fullscreen = mode("fullscreen", 3, 0);
    result.lastFullscreen = mode("last_fullscreen", 3, 0);
    result.difficulty = mode("difficulty", 2, 1);
    result.gameplay.dialogKeys = mode("event_choice_keys", 2, 1);
    result.gameplay.lowend = j.value("lowend", false);
    result.gameplay.colorblind = j.value("colorblind", false);
    result.gameplay.showPaths = j.value("show_paths", true);
    result.gameplay.achievementPopups = j.value("achievement_popups", true);
    if (j.contains("hotkeys")) {
        if (!j.at("hotkeys").is_object()) {
            throw std::runtime_error("invalid_hotkeys");
        }
        for (const auto& [action, key] : j.at("hotkeys").items()) {
            const bool validKey = key.is_number_unsigned()
                                      ? key.get<std::uint64_t>() < 323
                                      : key.is_number_integer() && key.get<std::int64_t>() >= 0 &&
                                            key.get<std::int64_t>() < 323;
            if (!result.hotkeys.contains(action) || !validKey) {
                throw std::runtime_error("invalid_hotkey");
            }
            const int loadedKey = key.get<int>();
            // LoadSettings5834c0 accepts legacy SDLK_KP_ENTER but stores
            // SDLK_RETURN, matching the platform TranslateSILKey boundary.
            result.hotkeys.at(action) = loadedKey == 271 ? 13 : loadedKey;
        }
    }
    result.validate();
    return result;
}

void Settings::write(const std::filesystem::path& path) const {
    validate();
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream out(path, std::ios::trunc);
    out << nlohmann::json{{"schema_version", 1},
                          {"language", language},
                          {"language_selected", languageSelected},
                          {"vsync", vsync},
                          {"frame_limit", frameLimit},
                          {"focus_pause", focusPause},
                          {"fullscreen", fullscreen},
                          {"last_fullscreen", lastFullscreen},
                          {"difficulty", difficulty},
                          {"event_choice_keys", gameplay.dialogKeys},
                          {"lowend", gameplay.lowend},
                          {"colorblind", gameplay.colorblind},
                          {"show_paths", gameplay.showPaths},
                          {"achievement_popups", gameplay.achievementPopups},
                          {"window_width", width},
                          {"window_height", height},
                          {"music_volume", musicVolume},
                          {"sound_volume", soundVolume},
                          {"hotkeys", hotkeys}}
               .dump(2)
        << '\n';
    out.flush();
    if (!out) {
        throw std::runtime_error("cannot_write_settings");
    }
}
} // namespace ftl
