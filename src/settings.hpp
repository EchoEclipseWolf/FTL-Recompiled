#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ftl {
class TextLibrary;

struct RunOptions {
    bool lowend = false;
    bool colorblind = false;
    bool showPaths = true;
    bool achievementPopups = true;
    int dialogKeys = 1;
};

// Native reconstruction settings. This is not the original settings.ini format.
struct Settings {
    std::string language = "en";
    int width = 1280;
    int height = 720;
    float musicVolume = 1;
    float soundVolume = 1;
    bool languageSelected = false;
    bool vsync = true;
    bool frameLimit = true;
    bool focusPause = true;
    int difficulty = 1;
    int fullscreen = 0;
    int lastFullscreen = 0;
    RunOptions gameplay{};
    // Original SDL 1 key numbers; never cast these to SDL 3 keycodes.
    static std::map<std::string, int> defaultHotkeys();
    static const std::array<std::vector<std::string>, 4>& hotkeyTabs();
    static std::string keyName(int key, const TextLibrary& texts, const std::string& locale);
    std::map<std::string, int> hotkeys = defaultHotkeys();
    void setHotkey(const std::string& action, int key);
    void resetHotkeys();
    void validate() const;
    static Settings read(const std::filesystem::path& path);
    void write(const std::filesystem::path& path) const;
};
} // namespace ftl
