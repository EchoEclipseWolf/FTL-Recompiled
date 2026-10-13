#pragma once
#include "audio.hpp"
#include "combat.hpp"
#include "profile.hpp"
#include "render.hpp"
#include "ship_render.hpp"
#include "time.hpp"
#include "travel.hpp"

namespace ftl {

class Hangar {
    Renderer& renderer_;
    const Archive& archive_;
    Audio& audio_;
    TextLibrary texts_;
    BlueprintCatalog catalog_;
    std::string locale_ = "en";
    std::string name_;
    std::string hover_;
    std::string editing_;
    std::vector<HangarCrew> crew_;
    ShipDefinition selectedShip_;
    std::vector<CombatWeapon> weaponFactories_;
    std::vector<CombatWeapon> artilleryFactories_;
    std::vector<CombatDrone> droneFactories_;
    std::vector<CombatDrone> droneTrash_;
    CombatDrone hackingFactory_{.blueprint = "DRONE_HACKING"};
    CombatExplosion explosionState_;
    std::array<float, 2> engineAnimationTimes_{};
    std::map<int, std::vector<float>> roomFireAnimationTimes_;
    std::map<int, std::vector<float>> medbayAnimationTimes_;
    std::map<int, std::vector<FireAnimationState>> fireSpreaderAnimations_;
    float constructorLoopDelta_ = 0;
    bool customizeMode_ = false;
    int family_ = 0;
    int type_ = 0;
    int listType_ = 0;
    int difficulty_ = 1;
    int customCrew_ = -1;
    bool open_ = false;
    bool list_ = false;
    bool rooms_ = true;
    bool advanced_ = false;
    bool colorblind_ = false;
    bool start_ = false;
    bool nativeNames_ = false;
    int openedList_ = 0;
    int listTutorialPage_ = -1;
    int dialogKeys_ = 1;
    bool tutorialChoicePending_ = false;
    bool encourageShipList_ = false;
    double tutorialOpenSeconds_ = 0;
    double listPulseSeconds_ = 0;
    std::array<std::array<bool, 3>, 10> unlocked_{};
    std::array<std::array<bool, 5>, 10> achievements_{};
    std::array<std::array<bool, 5>, 10> freshAchievements_{};
    std::array<std::array<int, 5>, 10> achievementDifficulties_{};
    std::array<std::array<int, 3>, 10> victoryLayouts_{};
    RandomStream random_{0};
    bool sessionRandomBound_ = false;
    float mx_ = -1;
    float my_ = -1;
    float fraction_ = 0;
    std::uint64_t tick_ = 0;
    std::uint64_t hoverTick_ = 0;
    std::uint64_t previewTick_ = 0;
    float sceneAnimationTime_ = 0;
    std::array<int, 4> sceneAnimationFrames_{};
    float walkingManX_ = 1245;
    float walkingManY_ = 136;
    std::size_t caret_ = 0;

    struct NameInputBlink {
        std::size_t caret = 0;
        float elapsed = 0;
        float renderElapsed = 0;
    };

    NameInputBlink shipNameBlink_;
    std::vector<NameInputBlink> crewNameBlinks_;

    struct Button {
        std::string id, label;
        Rect rect;
        int font = 62;
        bool enabled = true, selected = false;

        bool contains(float x, float y) const {
            return id == "list_tutorial_continue"
                       ? x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h
                       : rect.contains(x, y);
        }
    };

    std::vector<Button> buttons(bool includeTutorial = true);
    std::vector<std::string> listTutorialLines();
    Rect listTutorialChoice();
    void listTutorial();
    std::string label(const LocalizedText& value);
    Font& localizedFont(int font, const std::string& value);
    std::string shortCrewName(const std::string& value);
    void choose(int family, int type);
    void advanceShipAnimations(float seconds, bool systemAnimations);
    void activate(const std::string& id);
    void stopEditing();
    void button(const Button& source);
    void text(const std::string& value,
              float x,
              float y,
              int font = 62,
              bool center = false,
              SDL_Color color = {235, 245, 229, 255});
    void crewSprite(const HangarCrew& crew, float x, float y, float scale);
    void shipPreview();
    void sceneAnimations();
    void equipment(bool labels);
    void systems();
    void shipList();
    void infoPanel(const std::string& title,
                   const std::string& body,
                   float height = 135,
                   float x = 945,
                   float y = 97,
                   float width = 337);
    std::string equipmentDescription(const EquipmentDefinition& equipment);
    std::string systemLevelDescription(int system, int level);
    void systemPanel(const SystemDefinition& system);
    void achievementTooltip(int family, int index);
    void achievementTooltips();
    AchievementDrawing achievementDrawing(int family, int index) const;
    void tooltip();

public:
    Hangar(Renderer& r, const Archive& a, Audio& audio, RandomStream* sessionRandom = nullptr)
        : renderer_(r), archive_(a), audio_(audio), texts_(a), catalog_(a) {
        if (sessionRandom) {
            random_.bind(*sessionRandom);
            sessionRandomBound_ = true;
        }
    }

    void
    open(const std::string& locale, std::uint32_t seed, const std::string& profile = "reference");

    void progression(const ProgressionLedger& ledger);

    void weaponAnimationExplosions(const std::map<std::string, std::string>& registry) {
        catalog_.weaponAnimationExplosions(registry);
    }

    void openedList(int value) {
        openedList_ = value;
    }

    int openedList() const {
        return openedList_;
    }

    void dialogKeys(int value) {
        dialogKeys_ = value;
    }

    void colorblind(bool value) {
        colorblind_ = value;
    }

    void applicationClockInitialized() {
        constructorLoopDelta_ = Time::step;
    }

    void close() {
        open_ = false;
    }

    bool isOpen() const {
        return open_;
    }

    bool repeatTextKey(const std::string& value) {
        if (editing_.empty() || (value != "Backspace" && value != "Delete" && value != "Left" &&
                                 value != "Right" && value != "Home" && value != "End" &&
                                 value != "Clear" && value != "Enter" && value != "Escape")) {
            return false;
        }
        key(value);
        return true;
    }

    bool launchRequested() const {
        return start_;
    }

    void move(float x, float y);
    void click(float x, float y);
    void key(const std::string& key);
    void inputText(const std::string& value);
    void semantic(const std::string& target, const std::string& action);

    void step(std::uint32_t ticks);

    void presentation(float fraction) {
        fraction_ = fraction;
    }

    void render();
    void renderOuter();
    nlohmann::json state();

    const std::vector<HangarCrew>& crew() const {
        return crew_;
    }

    const std::vector<CombatWeapon>& weaponFactories() const {
        return weaponFactories_;
    }

    const std::vector<CombatWeapon>& artilleryFactories() const {
        return artilleryFactories_;
    }

    const std::vector<CombatDrone>& droneFactories() const {
        return droneFactories_;
    }

    const std::vector<CombatDrone>& droneTrash() const {
        return droneTrash_;
    }

    const CombatDrone& hackingFactory() const {
        return hackingFactory_;
    }

    const CombatExplosion& explosionState() const {
        return explosionState_;
    }

    const std::array<float, 2>& engineAnimationTimes() const {
        return engineAnimationTimes_;
    }

    const std::map<int, std::vector<float>>& roomFireAnimationTimes() const {
        return roomFireAnimationTimes_;
    }

    const std::map<int, std::vector<float>>& medbayAnimationTimes() const {
        return medbayAnimationTimes_;
    }

    const std::map<int, std::vector<FireAnimationState>>& fireSpreaderAnimations() const {
        return fireSpreaderAnimations_;
    }

    const std::string& shipName() const {
        return name_;
    }

    int difficulty() const {
        return difficulty_;
    }

    void difficulty(int value) {
        difficulty_ = value;
    }

    bool advanced() const {
        return advanced_;
    }

    const ShipDefinition& selected() const {
        return selectedShip_;
    }

    const std::vector<std::string>& crewNameHistory() const {
        return catalog_.currentCrewNames();
    }
};
} // namespace ftl
