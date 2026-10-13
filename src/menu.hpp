#pragma once
#include "audio.hpp"
#include "render.hpp"
#include "resources.hpp"
#include "run.hpp"
#include "settings.hpp"
#include "time.hpp"
#include <array>
#include <json.hpp>
#include <memory>

namespace ftl {
using Json = nlohmann::json;

struct MenuButton {
    std::string id;
    std::string asset;
    Rect rect;
    bool active = true;
    bool hover = false;
};

class Menu {
    struct StatsAchievement {
        std::string id;
        std::string image;
        std::string name;
        std::string description;
        bool multiDifficulty = false;
    };

    struct StatsShip {
        std::string image;
        LocalizedText shipClass;
        std::vector<StatsAchievement> achievements;
    };

    Renderer& renderer_;
    Audio& audio_;
    double renderCap_ = 240;
    double creditScroll_ = -70;
    double creditPause_ = 0;
    bool endingPresented_ = false;
    std::string creditShip_;
    std::string creditCrew_;
    double uiRemaining_ = 0;
    std::uint64_t frame_ = 0;
    std::uint64_t animationTick_ = 0;
    std::uint64_t hoverTick_ = 0;
    Json profile_ = {{"version", 1},
                     {"runs", 0},
                     {"ships", 0},
                     {"beacons", 0},
                     {"scrap", 0},
                     {"best_sector", 1}};
    std::unique_ptr<BlueprintCatalog> statsCatalog_;
    const Archive* statsArchive_ = nullptr;
    std::map<std::string, StatsShip> statsShips_;
    std::vector<std::string> confirmationLines_;
    std::vector<StatsAchievement> statsAchievementsCatalog_;
    std::vector<std::string> creditNames_;
    TimeState time_;
    std::string hangarProfile_ = "native";
    std::string hoveredSoundTarget_;
    std::string locale_ = "en";
    std::string modal_;
    std::string boundary_;
    std::string confirmationText_;
    std::string noSaveText_;
    std::string yesText_;
    std::string noText_;
    std::string versionText_;
    std::string savePath_;
    std::string supportError_;
    std::string profilePath_;
    std::string controlEditing_;
    std::string openingError_;
    std::string startupNoticeText_;
    std::vector<std::string> startupNoticeChoices_;
    std::uint64_t startupNoticeOpenedTick_ = 0;
    int startupNoticeHover_ = -1;
    bool startupNoticePending_ = false;
    bool startupWelcome_ = false;
    bool startupScreenChanged_ = false;
    bool startupProcessed_ = false;
    TextLibrary texts_;
    Settings controls_;
    std::array<std::string, 9> languageLabels_;
    std::array<MenuButton, 7> buttons_;
    RandomStream sessionRandom_{0};
    Hangar hangar_;
    Run run_;
    int statsFamily_ = 0;
    int statsLayout_ = 0;
    int statsListLayout_ = 0;
    int statsRecentFamily_ = 0;
    int statsRecentLayout_ = 0;
    int statsRecentTopRank_ = -1;
    int statsRecentShipRank_ = -1;
    int languageHover_ = -1;
    float musicVolume_ = 1;
    float soundVolume_ = 1;
    int controlPage_ = 0;
    int supportSlider_ = -1;
    float supportSliderMouse_ = 0;
    float supportSliderMarker_ = 0;
    float mouseX_ = -1;
    float mouseY_ = -1;
    float uiFraction_ = 0;
    std::uint32_t seed_ = 0;
    Rect confirmationRect_{450, 278, 380, 96};
    Rect yes_{543, 402, 98, 24};
    Rect no_{655, 402, 83, 24};
    bool languageSelected_ = true;
    bool hasSave_ = false;
    bool statsLastPlaythrough_ = false;
    bool statsShipList_ = false;
    bool statsShipBest_ = false;
    bool statsAchievements_ = false;
    bool controlReset_ = false;
    bool supportPointerOwned_ = false;
    bool supportPointerHeld_ = false;
    bool optionsLanguage_ = false;
    bool capFromOption_ = false;
    bool focused_ = true;
    bool pauseControlsDirect_ = false;
    bool focusPauseOwned_ = false;
    bool focusPreviousPause_ = false;
    bool quit_ = false;
    bool liveSeeds_ = false;
    bool yesHover_ = false;
    bool noHover_ = false;
    void syncRunModal();
    void openRunOptions();
    void openHangar();
    void launch();
    Font& localizedFont(int size, const std::string& text);

    float fontScale() const {
        return locale_ == "zh-Hans" ? 1.f : 2.f;
    }

    void layoutConfirmation();

    void validateStatsProfile(const Json& profile) const;
    void openStats(bool lastPlaythrough);
    void statsRecordsRender();
    void statsScoresRender();
    void initializeStats(const Archive& archive);
    void statsAchievementsRender();
    void statsTooltipRender(bool outer = false);
    bool statsShipAction(const std::string& id);
    bool statsShipUnlocked(int family, int layout) const;
    std::string statsSelectedShip() const;
    std::vector<std::pair<std::string, Rect>> statsShipControls() const;
    Json statsShipState() const;
    Json statsShipAchievementsState() const;
    void statsShipListRender();
    void
    statsButtonRender(Rect rect, const std::string& label, int font, bool enabled, bool selected);
    Json statsAchievementsState() const;
    static const std::array<std::vector<std::string>, 4>& controlTabs();
    std::vector<std::pair<std::string, Rect>> controlRows() const;
    void supportText(const std::string& value,
                     float x,
                     float y,
                     int size = 12,
                     bool center = false,
                     SDL_Color color = {255, 255, 255, 255});
    Rect supportButtonRect(const std::string& value, float right, float y, float minimum);
    void supportButton(Rect rect, const std::string& value, const std::string& base);
    void creditsStep(std::uint32_t ticks);
    void creditsRender();
    void optionsRender();
    void syncOptions();
    void initializeWeaponAnimations();
    void openStartupNotice();
    void startupNoticeStep();
    void startupNoticeRender();
    void startupNoticeMove(float x, float y);
    void selectStartupNotice(int choice);
    std::vector<Rect> startupNoticeRects();
    std::vector<std::string> startupNoticeLines(const std::string& value);
    Json startupNoticeState();
    void controlsRender();

    struct SupportConfirmation {
        Rect body;
        Rect yes;
        Rect no;
        std::vector<std::string> lines;
    };

    SupportConfirmation controlConfirmation();
    void controlConfirmationRender();
    void supportMove(float x, float y);
    static const std::map<std::string, std::string>& supportedHotkeys();
    static int originalKey(const std::string& key);
    bool controlKey(const std::string& key);
    std::string gameplayKey(const std::string& key) const;
    bool supportAction(const std::string& target);
    void supportRender();
    void supportClick(float x, float y);
    bool wipeNativeProfile();
    bool commitProfile(Json updated);
    bool syncRunProgression(bool terminal = false);
    bool syncHangarProfile();
    bool recordRun();
    void persistenceAction(const std::string& path, bool saving);
    void confirmation();
    void languagePicker(bool pointer = true);
    void textButton(Rect rect, const std::string& label, bool hover);
    void activate(MenuButton& button);
    void routeGameOver();
    void terminalStep();
    std::string asset(const std::string& base) const;

public:
    int fullscreenDesired() const {
        return controls_.fullscreen;
    }

    int lastFullscreen() const {
        return controls_.lastFullscreen;
    }

    int difficultyDesired() const {
        return controls_.difficulty;
    }

    const RunOptions& gameplayOptions() const {
        return controls_.gameplay;
    }

    Menu(Renderer& renderer, const Archive& archive, Audio& audio, std::uint32_t startupSeed = 0);

    void liveSeeds(bool enabled) {
        liveSeeds_ = enabled;
    }

    void startCombat();
    void persistence(const std::string& path);

    void hotkeys(const std::map<std::string, int>& keys) {
        controls_.hotkeys = keys;
        controls_.validate();
    }

    const std::map<std::string, int>& hotkeys() const {
        return controls_.hotkeys;
    }

    void volumes(float music, float sound) {
        audio_.volumes(music, sound);
        musicVolume_ = music;
        soundVolume_ = sound;
    }

    void options(const Settings& settings);
    Json optionsState() const;
    void focus(bool focused);

    bool frameLimit() const {
        return controls_.frameLimit;
    }

    bool focusPause() const {
        return controls_.focusPause;
    }

    bool vsyncDesired() const {
        return controls_.vsync;
    }

    double renderCap() const {
        return renderCap_;
    }

    void renderCap(double cap) {
        renderCap_ = cap;
        capFromOption_ = false;
    }

    float musicVolume() const {
        return musicVolume_;
    }

    float soundVolume() const {
        return soundVolume_;
    }

    void saveRun(const std::string& path = {});
    void continueRun(const std::string& path = {});
    bool requestExit();
    void
    fixture(const std::string& name, std::uint32_t seed = 0, const nlohmann::json& values = {});
    void move(float x, float y);
    void click(float x, float y);

    void modifier(bool shift) {
        run_.modifier(shift);
    }

    void pointerDown(float x, float y) {
        if (supportPointerHeld_) {
            logging::verbose("pointer", "press while a press is already held; previous replaced");
        }
        supportPointerHeld_ = true;
        supportPointerOwned_ = !modal_.empty();
        if (run_.isOpen() && !supportPointerOwned_) {
            logging::verbose("pointer", "down ({:.1f},{:.1f}) -> run drag begin", x, y);
            run_.dragBegin(x, y);
        } else {
            logging::verbose(
                "pointer", "down ({:.1f},{:.1f}) -> immediate click, modal '{}'", x, y, modal_);
            click(x, y);
        }
    }

    void pointerUp(float x, float y) {
        if (supportPointerHeld_ && run_.isOpen() && !supportPointerOwned_) {
            logging::verbose("pointer", "up ({:.1f},{:.1f}) -> run drag end", x, y);
            run_.dragEnd(x, y);
            routeGameOver();
            syncRunProgression();
        } else if (!supportPointerHeld_) {
            logging::verbose("pointer",
                             "up ({:.1f},{:.1f}) ignored: no held press (cancelled or never "
                             "received)",
                             x,
                             y);
        } else {
            logging::verbose(
                "pointer", "up ({:.1f},{:.1f}) -> no action, press already clicked on down", x, y);
        }
        supportPointerHeld_ = false;
        supportPointerOwned_ = false;
        supportSlider_ = -1;
    }

    void cancelPointer() {
        if (run_.isOpen()) {
            run_.cancelPointer();
        }
        supportPointerHeld_ = false;
        supportPointerOwned_ = false;
        supportSlider_ = -1;
    }

    bool reconcilePointer(bool leftHeld) {
        if (supportPointerHeld_ && !leftHeld) {
            logging::verbose("pointer",
                             "reconcile cancel: owned by {}, modal '{}'",
                             supportPointerOwned_ ? "modal" : "run",
                             modal_);
            cancelPointer();
            return true;
        }
        return false;
    }

    void rightClick(float x, float y) {
        if (run_.isOpen() && modal_.empty()) {
            run_.click(x, y, true);
        } else {
            logging::verbose("pointer",
                             "right click ({:.1f},{:.1f}) ignored: {}",
                             x,
                             y,
                             run_.isOpen() ? "modal '" + modal_ + "' open" : "no run");
        }
    }

    void middleClick() {
        if (run_.isOpen() && modal_.empty()) {
            logging::verbose("pointer", "middle click -> pause toggle");
            run_.key("Space");
        } else {
            logging::verbose("pointer", "middle click ignored");
        }
    }

    void key(const std::string& key);
    void keyUp(const std::string& key);
    bool repeatTextKey(const std::string& key);

    void inputText(const std::string& value) {
        if (run_.isOpen()) {
            run_.inputText(value);
        } else if (hangar_.isOpen()) {
            hangar_.inputText(value);
        }
    }

    Json devCommand(const Json& request) {
        auto result = run_.devCommand(request);
        syncRunProgression();
        return result;
    }

    void step(std::uint32_t ticks);
    void advance(double seconds);

    std::uint64_t gameplayTicks() const {
        return run_.gameplayTicks();
    }

    bool gameplayClockActive() const {
        return run_.gameplayClockActive();
    }

    Json timing() const;

    void simulationSpeed(double speed) {
        TimeController::speed(time_, speed);
    }

    void renderBoundary();
    void render();
    void semantic(const std::string& target, const std::string& action);
    void language(const std::string& value);
    Json state();

    bool quitting() const {
        return quit_;
    }

    bool languageSelected() const {
        return languageSelected_;
    }

    const std::string& locale() const {
        return locale_;
    }
};
} // namespace ftl
