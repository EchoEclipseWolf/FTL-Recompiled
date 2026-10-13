#include "menu.hpp"
#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ftl {
namespace {
constexpr SDL_Color on{235, 245, 229, 255}, ink{25, 49, 51, 255}, selected{255, 238, 128, 255};

} // namespace

Menu::Menu(Renderer& r, const Archive& a, Audio& audio, std::uint32_t startupSeed)
    : renderer_(r), audio_(audio), texts_(a), sessionRandom_(startupSeed),
      hangar_(r, a, audio, &sessionRandom_), run_(r, a, audio, &sessionRandom_) {
    audio_.sessionRandom(sessionRandom_);
    const std::array<std::string, 7> ids{
        "continue", "new_game", "tutorial", "stats", "options", "credits", "quit"};
    const std::array<std::string, 7> names{
        "continue", "start", "tutorial", "stats", "options", "credits", "quit"};
    for (std::size_t i = 0; i < 7; ++i) {
        buttons_[i] = {ids[i], names[i], {0, float(275 + 58 * i), 0, 58}};
    }
    for (std::size_t i = 0; i < languages.size(); ++i) {
        languageLabels_[i] = texts_.get("language_name", std::string(languages[i]));
    }
    const auto credits = a.read("data/credits.txt");
    std::istringstream creditLines(std::string(credits.begin(), credits.end()));
    std::string name;
    while (std::getline(creditLines, name)) {
        if (!name.empty() && name.back() == '\r') {
            name.pop_back();
        }
        creditNames_.push_back(name);
    }
    initializeStats(a);
    initializeWeaponAnimations();
    run_.initializeApplicationSpace();
    language("en");
}

void Menu::initializeWeaponAnimations() {
    const auto weaponAnimations =
        initializeWeaponAnimationExplosions(*statsArchive_, sessionRandom_);
    hangar_.weaponAnimationExplosions(weaponAnimations);
    run_.weaponAnimationExplosions(weaponAnimations);
    statsCatalog_->weaponAnimationExplosions(weaponAnimations);
}

std::vector<std::string> Menu::startupNoticeLines(const std::string& value) {
    auto& font = localizedFont(12, value);
    return renderer_.wrappedLines(font,
                                  value,
                                  565,
                                  Renderer::sourceFontScale(12, locale_ == "zh-Hans"),
                                  locale_ == "zh-Hans");
}

void Menu::openStartupNotice() {
    if (!startupNoticePending_ || !languageSelected_ || !modal_.empty()) {
        return;
    }
    startupNoticePending_ = false;
    startupScreenChanged_ = renderer_.fullscreen() == 3 && controls_.lastFullscreen != 3;
    startupNoticeChoices_.clear();
    if (startupScreenChanged_) {
        startupNoticeChoices_ = {texts_.get("keep_screen", locale_),
                                 texts_.get("revert_screen", locale_)};
    } else if (startupWelcome_) {
        startupNoticeText_ = texts_.get("welcome", locale_);
        startupNoticeChoices_ = {texts_.get("continue", locale_)};
        // MainMenu::Open4c1ab0 saves the newly initialized profile before
        // showing its welcome ChoiceBox. Native and original files stay separate.
        if (!commitProfile(profile_)) {
            openingError_ = supportError_;
        }
    } else if (!openingError_.empty()) {
        startupNoticeText_ = openingError_;
        startupNoticeChoices_ = {texts_.get("continue", locale_)};
    } else {
        startupWelcome_ = false;
        return;
    }
    if (!startupScreenChanged_ && startupWelcome_ && !openingError_.empty()) {
        startupNoticeText_ += "\n\n" + openingError_;
    }
    startupWelcome_ = false;
    startupNoticeOpenedTick_ = animationTick_;
    startupNoticeHover_ = -1;
    modal_ = "startup_notice";
    startupNoticeStep();
}

void Menu::startupNoticeStep() {
    openStartupNotice();
    if (modal_ != "startup_notice" || !startupScreenChanged_) {
        return;
    }
    const auto elapsed = animationTick_ - startupNoticeOpenedTick_;
    // MainMenu::OnLoop4c50b0 uses a strict negative remaining-time test.
    if (elapsed > 600) {
        selectStartupNotice(1);
        return;
    }
    const int remaining = int((600 - elapsed + 59) / 60);
    auto time =
        texts_.get(remaining <= 1 ? "screen_changed_1_second" : "screen_changed_seconds", locale_);
    const auto replace = [](std::string& text, const std::string& value) {
        std::size_t position = 0;
        while ((position = text.find("\\1", position)) != std::string::npos) {
            text.replace(position, 2, value);
            position += value.size();
        }
    };
    replace(time, std::to_string(remaining));
    startupNoticeText_ = texts_.get("screen_changed", locale_);
    replace(startupNoticeText_, time);
    if (!openingError_.empty()) {
        startupNoticeText_ += "\n\n" + openingError_;
    }
    startupNoticeMove(mouseX_, mouseY_);
}

std::vector<Rect> Menu::startupNoticeRects() {
    auto& bodyFont = localizedFont(12, startupNoticeText_);
    float y = 200 + float(startupNoticeLines(startupNoticeText_).size()) *
                        Renderer::sourceLineHeight(12, bodyFont);
    std::vector<Rect> rectangles;
    for (std::size_t i = 0; i < startupNoticeChoices_.size(); ++i) {
        const auto choice = std::to_string(i + 1) + ". " + startupNoticeChoices_[i];
        const auto lines = startupNoticeLines(choice);
        auto& font = localizedFont(12, choice);
        float width = 0;
        for (const auto& line : lines) {
            width = std::max(
                width,
                renderer_.measure(font, line, Renderer::sourceFontScale(12, locale_ == "zh-Hans")));
        }
        const float height =
            float(int(float(lines.size()) * Renderer::sourceLineHeight(12, font) + 12));
        rectangles.push_back({352, y, float(int(width)), height});
        y += height;
    }
    return rectangles;
}

void Menu::startupNoticeMove(float x, float y) {
    startupNoticeHover_ = -1;
    const auto rectangles = startupNoticeRects();
    for (std::size_t i = 0; i < rectangles.size(); ++i) {
        const auto& rect = rectangles[i];
        if (x >= rect.x && x <= rect.x + rect.w && y >= rect.y && y <= rect.y + rect.h) {
            startupNoticeHover_ = int(i);
            break;
        }
    }
}

void Menu::selectStartupNotice(int choice) {
    if (modal_ != "startup_notice" || choice < 0 || choice >= int(startupNoticeChoices_.size())) {
        return;
    }
    if (startupScreenChanged_) {
        if (choice == 0) {
            controls_.lastFullscreen = renderer_.fullscreen();
        } else {
            controls_.fullscreen = controls_.lastFullscreen;
            // Native SDL3 can apply the reverted display mode at this boundary;
            // original CApp restarts its application initialization (command2).
            if (!renderer_.setFullscreen(controls_.fullscreen)) {
                renderer_.setFullscreen(0);
                controls_.fullscreen = renderer_.fullscreen();
                supportError_ = SDL_GetError();
            }
        }
    }
    startupScreenChanged_ = false;
    modal_.clear();
    startupNoticeHover_ = -1;
    move(mouseX_, mouseY_);
}

void Menu::startupNoticeRender() {
    renderer_.setTint(1);
    renderer_.windowFrame({327, 140, 616, 391});
    float y = 170;
    auto& font = localizedFont(12, startupNoticeText_);
    for (const auto& line : startupNoticeLines(startupNoticeText_)) {
        supportText(line, 352, y);
        y += Renderer::sourceLineHeight(12, font);
    }
    const auto rectangles = startupNoticeRects();
    for (std::size_t i = 0; i < startupNoticeChoices_.size(); ++i) {
        const auto choice = std::to_string(i + 1) + ". " + startupNoticeChoices_[i];
        auto& choiceFont = localizedFont(12, choice);
        y = rectangles[i].y + 10;
        for (const auto& line : startupNoticeLines(choice)) {
            supportText(line,
                        352,
                        y,
                        12,
                        false,
                        startupNoticeHover_ == int(i) ? SDL_Color{243, 255, 80, 255}
                                                      : SDL_Color{255, 255, 255, 255});
            y += Renderer::sourceLineHeight(12, choiceFont);
        }
    }
}

Json Menu::startupNoticeState() {
    Json choices = Json::array();
    const auto rectangles = startupNoticeRects();
    for (std::size_t i = 0; i < startupNoticeChoices_.size(); ++i) {
        const auto& rect = rectangles[i];
        choices.push_back(
            {{"text", startupNoticeChoices_[i]}, {"hitbox", {rect.x, rect.y, rect.w, rect.h}}});
    }
    return {{"open", modal_ == "startup_notice"},
            {"pending", startupNoticePending_},
            {"screen_changed", startupScreenChanged_},
            {"text", startupNoticeText_},
            {"hover", startupNoticeHover_},
            {"elapsed_ticks",
             modal_ == "startup_notice" ? animationTick_ - startupNoticeOpenedTick_ : 0},
            {"choices", choices}};
}

std::string Menu::asset(const std::string& base) const {
    return "img/main_menus/" + base;
}

void Menu::language(const std::string& value) {
    if (!supportedLanguage(value)) {
        throw std::runtime_error("unsupported_language");
    }
    locale_ = value;
    renderer_.imageLanguage(value);
    confirmationText_ = texts_.get("confirm_new_game", value);
    noSaveText_ = texts_.get("no_load", value);
    yesText_ = texts_.get("button_confirm", value);
    noText_ = texts_.get("button_cancel", value);
    versionText_ = "Steam " + texts_.get("version", value);
    if (auto p = versionText_.find("\\1"); p != std::string::npos) {
        versionText_.replace(p, 2, "1.6.9");
    }
    for (auto& b : buttons_) {
        auto& t = renderer_.image(asset(b.asset + "_on.png"));
        b.rect.x = 1200.f - t.w;
        b.rect.w = float(t.w);
        b.rect.h = float(t.h - 4);
    }
    layoutConfirmation();
    syncOptions();
}

Font& Menu::localizedFont(int size, const std::string& text) {
    return renderer_.sourceFont(size, text, locale_ == "zh-Hans");
}

void Menu::layoutConfirmation() {
    auto width = [&](Font& font, const std::string& s) {
        auto [l, r] = renderer_.bounds(font, s, fontScale());
        return r - l;
    };
    yes_.w = std::round(width(localizedFont(62, yesText_), yesText_)) + 6;
    no_.w = std::round(width(localizedFont(62, noText_), noText_)) + 6;
    confirmationRect_.w = std::max(380.f, yes_.w + no_.w + 100);
    auto& font = localizedFont(52, confirmationText_);
    confirmationLines_ = renderer_.wrappedLines(
        font, confirmationText_, confirmationRect_.w - 40, fontScale(), locale_ == "zh-Hans");
    confirmationRect_.h = float(confirmationLines_.size()) * (Renderer::sourceLineHeight(52, font));
    confirmationRect_.x = float(int(1280 - confirmationRect_.w) / 2);
    confirmationRect_.y = float(int(720 - (90 + confirmationRect_.h - 22)) / 2);
    yes_.x = confirmationRect_.x + int(confirmationRect_.w) / 2 - int(yes_.w + no_.w + 14) / 2;
    no_.x = yes_.x + yes_.w + 14;
    yes_.y = no_.y = confirmationRect_.y + confirmationRect_.h + 28;
}

void Menu::syncRunModal() {
    // CommandGui::OnLoop5220b0 derives bMenuPaused from focus windows;
    // opening options does not change the independent manual bPaused flag.
    run_.externalModal(!modal_.empty());
}

void Menu::openRunOptions() {
    pauseControlsDirect_ = false;
    modal_ = "options";
    syncRunModal();
}

void Menu::openHangar() {
    hangar_.openedList(hangarProfile_ == "native"  ? profile_.value("opened_list", 0)
                       : hangarProfile_ == "fresh" ? 0
                                                   : 2);
    hangar_.dialogKeys(controls_.gameplay.dialogKeys);
    hangar_.colorblind(controls_.gameplay.colorblind);
    hangar_.open(locale_, seed_, hangarProfile_);
    // ShipBuilder::Open 0x4e2b50 selects the difficulty button from the
    // persistent Settings value instead of resetting each new hangar visit.
    hangar_.difficulty(controls_.difficulty);
    if (hangarProfile_ == "native") {
        hangar_.progression(ProgressionLedger::read(profile_));
    }
}

void Menu::launch() {
    if (!syncHangarProfile()) {
        return;
    }
    if (hangar_.isOpen() && hangar_.launchRequested()) {
        run_.progression(profile_);
        run_.start(hangar_, locale_, seed_, true);
        endingPresented_ = false;
        syncRunProgression();
        hangar_.close();
    }
}

void Menu::startCombat() {
    // Developer entry uses the same hangar launch and run fixture as combat replay.
    fixture("hangar", 0);
    hangar_.semantic("start", "click");
    launch();
    run_.diagnosticFixture("combat_reference", nlohmann::json::object());
}

void Menu::fixture(const std::string& name, std::uint32_t seed, const nlohmann::json& values) {
    if (name.starts_with("live_") || name.starts_with("travel_") || name.starts_with("combat_") ||
        name == "campaign_final") {
        run_.diagnosticFixture(name, values);
        return;
    }
    if (name != "fresh" && name != "continue" && name != "first_launch" && name != "hangar" &&
        name != "hangar_all" && name != "hangar_fresh") {
        throw std::runtime_error("unknown_fixture");
    }
    run_.close();
    endingPresented_ = false;
    run_.externalModal(false);
    audio_.music("audio/music/bp_MUS_TitleScreen.ogg");
    audio_.combatMusic(false);
    hangar_.close();
    hangarProfile_ = name == "hangar_all"     ? "all_unlocked"
                     : name == "hangar_fresh" ? "fresh"
                     : name == "hangar"       ? "reference"
                                              : "native";
    hasSave_ = name == "continue";
    seed_ = seed;
    sessionRandom_ = RandomStream(seed);
    // A fixture recreates the application seed boundary, including the
    // registry constructors which precede the first menu/actor loop.
    initializeWeaponAnimations();
    run_.initializeApplicationSpace();
    modal_.clear();
    startupNoticePending_ = false;
    startupScreenChanged_ = false;
    startupWelcome_ = false;
    startupProcessed_ = false;
    startupNoticeHover_ = -1;
    startupNoticeChoices_.clear();
    optionsLanguage_ = false;
    supportSlider_ = -1;
    supportPointerHeld_ = false;
    boundary_.clear();
    quit_ = false;
    animationTick_ = frame_ = hoverTick_ = 0;
    const auto speed = time_.speed;
    time_ = {};
    time_.speed = speed;
    uiRemaining_ = 0;
    uiFraction_ = 0;
    mouseX_ = mouseY_ = -1;
    yesHover_ = noHover_ = false;
    hoveredSoundTarget_.clear();
    languageSelected_ = name != "first_launch";
    languageHover_ = -1;
    if (!languageSelected_) {
        modal_ = "language_selection";
    }
    audio_.active(languageSelected_);
    for (auto& b : buttons_) {
        b.active = b.id != "continue" || hasSave_;
        b.hover = false;
    }
    if (name.starts_with("hangar")) {
        openHangar();
    }
}

void Menu::move(float x, float y) {
    if (modal_ == "startup_notice") {
        mouseX_ = x;
        mouseY_ = y;
        startupNoticeMove(x, y);
        return;
    }
    if (run_.isOpen() && run_.writeErrorOpen()) {
        mouseX_ = x;
        mouseY_ = y;
        run_.pointerPosition(x, y);
        run_.writeErrorMove(x, y);
        return;
    }
    if (run_.isOpen() && !modal_.empty()) {
        mouseX_ = x;
        mouseY_ = y;
        run_.pointerPosition(x, y);
        supportMove(x, y);
        if (modal_ == "language_selection") {
            languageHover_ = -1;
            for (int i = 0; i < int(languages.size()); ++i) {
                if (Rect{513, float(211 + 37 * i), 256, 26}.contains(x, y)) {
                    languageHover_ = i;
                }
            }
        }
        return;
    }
    if (run_.isOpen()) {
        run_.move(x, y);
        return;
    }
    if (hangar_.isOpen()) {
        hangar_.move(x, y);
        return;
    }
    if (x != mouseX_ || y != mouseY_) {
        hoverTick_ = animationTick_;
    }
    mouseX_ = x;
    mouseY_ = y;
    supportMove(x, y);
    std::string target;
    if (modal_ == "language_selection") {
        languageHover_ = -1;
        for (int i = 0; i < int(languages.size()); ++i) {
            if (Rect{513, float(211 + 37 * i), 256, 26}.contains(x, y)) {
                languageHover_ = i;
                target = "language:" + std::string(languages[i]);
            }
        }
    } else if (modal_ == "new_game_confirmation") {
        yesHover_ = yes_.contains(x, y);
        noHover_ = no_.contains(x, y);
        if (yesHover_) {
            target = "confirm";
        } else if (noHover_) {
            target = "cancel";
        }
    } else if (modal_.empty()) {
        for (auto& b : buttons_) {
            auto hit = b.rect;
            hit.y += 2;
            b.hover = hit.contains(x, y);
            if (b.hover && b.active) {
                target = b.id;
            }
        }
    }
    if (!target.empty() && target != hoveredSoundTarget_) {
        audio_.hover();
    }
    hoveredSoundTarget_ = std::move(target);
}

void Menu::persistence(const std::string& path) {
    const bool startup = !startupProcessed_ && !run_.isOpen() && !hangar_.isOpen();
    savePath_ = path;
    run_.nativeDataDirectory(std::filesystem::path(path).parent_path().string());
    hasSave_ = std::filesystem::is_regular_file(savePath_);
    buttons_[0].active = hasSave_;
    const auto profilePath =
        (std::filesystem::path(path).parent_path() / "recomp-profile-v1.json").string();
    if (profilePath != profilePath_) {
        statsRecentFamily_ = statsRecentLayout_ = 0;
        statsRecentTopRank_ = statsRecentShipRank_ = -1;
        profile_ = {{"version", 1},
                    {"runs", 0},
                    {"ships", 0},
                    {"beacons", 0},
                    {"scrap", 0},
                    {"best_sector", 1}};
    }
    profilePath_ = profilePath;
    const bool profileExists = std::filesystem::is_regular_file(profilePath_);
    if (profileExists) {
        try {
            if (std::filesystem::file_size(profilePath_) > 65536) {
                throw std::runtime_error("profile_too_large");
            }
            std::ifstream input(profilePath_);
            const auto profile = Json::parse(input);
            if (profile.at("version") != 1) {
                throw std::runtime_error("unsupported_native_profile");
            }
            for (const auto& key : {"runs", "ships", "beacons", "scrap", "best_sector"}) {
                const auto& value = profile.at(key);
                const bool valid =
                    value.is_number_unsigned()
                        ? value.get<std::uint64_t>() <= std::numeric_limits<int>::max()
                        : value.is_number_integer() && value.get<std::int64_t>() >= 0 &&
                              value.get<std::int64_t>() <= std::numeric_limits<int>::max();
                if (!valid) {
                    throw std::runtime_error("invalid_native_profile");
                }
            }
            validateStatsProfile(profile);
            profile_ = profile;
        } catch (const std::exception& error) {
            supportError_ = error.what();
            profilePath_.clear(); // Preserve an unreadable profile instead of overwriting it.
        }
    }
    if (startup) {
        startupProcessed_ = true;
        startupWelcome_ = !profileExists;
        openingError_ = supportError_;
        startupNoticePending_ = true;
        openStartupNotice();
    }
}

void Menu::persistenceAction(const std::string& path, bool saving) {
    const auto previousSavePath = savePath_;
    const auto previousProfilePath = profilePath_;
    const auto previousProfile = profile_;
    const auto previousProgression = run_.progressionLedger();
    const auto previousIdentity = run_.profileRunId();
    const bool previousStarted = run_.profileRunStarted();
    const auto previousError = supportError_;
    const bool previousHasSave = hasSave_;
    const bool previousContinueActive = buttons_[0].active;
    // The first path after a fixture emulates the startup profile load; a
    // rejected action must not leave that startup notice staged either.
    const auto previousModal = modal_;
    const auto previousOpeningError = openingError_;
    const auto previousNoticeText = startupNoticeText_;
    const auto previousNoticeChoices = startupNoticeChoices_;
    const auto previousNoticeOpenedTick = startupNoticeOpenedTick_;
    const int previousNoticeHover = startupNoticeHover_;
    const bool previousNoticePending = startupNoticePending_;
    const bool previousWelcome = startupWelcome_;
    const bool previousScreenChanged = startupScreenChanged_;
    const bool previousStartupProcessed = startupProcessed_;
    try {
        persistence(path);
        if (profilePath_.empty() && !supportError_.empty()) {
            throw std::runtime_error(supportError_);
        }
        if (saving) {
            run_.mergeProgression(profile_);
            saveRun();
        } else {
            if (!hasSave_) {
                throw std::runtime_error("native_checkpoint_not_found");
            }
            continueRun();
        }
    } catch (...) {
        // Run::load stages gameplay; preserve its menu/profile context on failure too.
        savePath_ = previousSavePath;
        profilePath_ = previousProfilePath;
        profile_ = previousProfile;
        run_.progressionLedger(previousProgression, previousProfile);
        run_.profileRunId(previousIdentity);
        run_.profileRunStarted(previousStarted);
        supportError_ = previousError;
        hasSave_ = previousHasSave;
        buttons_[0].active = previousContinueActive;
        modal_ = previousModal;
        openingError_ = previousOpeningError;
        startupNoticeText_ = previousNoticeText;
        startupNoticeChoices_ = previousNoticeChoices;
        startupNoticeOpenedTick_ = previousNoticeOpenedTick;
        startupNoticeHover_ = previousNoticeHover;
        startupNoticePending_ = previousNoticePending;
        startupWelcome_ = previousWelcome;
        startupScreenChanged_ = previousScreenChanged;
        startupProcessed_ = previousStartupProcessed;
        throw;
    }
}

void Menu::saveRun(const std::string& path) {
    if (run_.isOpen() && run_.defeated()) {
        throw std::runtime_error("terminal_checkpoint_unavailable");
    }
    if (run_.tutorialRunning()) {
        throw std::runtime_error("tutorial_checkpoint_unavailable");
    }
    if (!path.empty()) {
        persistenceAction(path, true);
        return;
    }
    if (savePath_.empty()) {
        throw std::runtime_error("native_persistence_not_configured");
    }
    const auto previousProfile = profile_;
    const auto previousProgression = run_.progressionLedger();
    const auto previousIdentity = run_.profileRunId();
    const bool previousStarted = run_.profileRunStarted();
    const auto destination = std::filesystem::path(savePath_);
    const auto backup = std::filesystem::path(savePath_ + ".profile-save-backup");
    const auto profileDestination = profilePath_;
    bool backedUp = false;
    bool checkpointWritten = false;
    try {
        if (std::filesystem::exists(destination)) {
            std::filesystem::copy_file(destination, backup);
            backedUp = true;
        }
        // Stage identity/accounting in memory so the checkpoint contains the
        // same identity as its profile. No profile file changes before save.
        profilePath_.clear();
        const bool staged = syncRunProgression();
        profilePath_ = profileDestination;
        if (!staged) {
            throw std::runtime_error(supportError_);
        }
        run_.save(savePath_);
        checkpointWritten = true;
        if (!commitProfile(profile_)) {
            throw std::runtime_error(supportError_);
        }
    } catch (...) {
        const auto failure = std::current_exception();
        run_.showWriteError();
        profilePath_ = profileDestination;
        profile_ = previousProfile;
        run_.progressionLedger(previousProgression, previousProfile);
        run_.profileRunId(previousIdentity);
        run_.profileRunStarted(previousStarted);
        try {
            if (checkpointWritten) {
                std::filesystem::remove(destination);
                if (backedUp) {
                    std::filesystem::rename(backup, destination);
                }
            } else if (backedUp) {
                std::filesystem::remove(backup);
            }
        } catch (...) {
            throw std::runtime_error("native_checkpoint_restore_failed");
        }
        std::rethrow_exception(failure);
    }
    if (backedUp) {
        std::error_code cleanup;
        std::filesystem::remove(backup, cleanup);
    }
    run_.clearWriteError();
    hasSave_ = true;
    buttons_[0].active = true;
}

void Menu::continueRun(const std::string& path) {
    if (!path.empty()) {
        persistenceAction(path, false);
        return;
    }
    if (savePath_.empty() || !hasSave_) {
        return;
    }
    run_.load(savePath_);
    run_.progression(profile_);
    hangar_.close();
    modal_.clear();
}

bool Menu::requestExit() {
    // CApp::OnRequestExit0x401820 saves an active non-tutorial run before
    // OnExit. SDL window-close must use the same checkpoint lifecycle.
    try {
        if (run_.isOpen() && !run_.tutorialRunning()) {
            if (run_.defeated()) {
                if (!recordRun()) {
                    throw std::runtime_error(supportError_);
                }
            } else if (!savePath_.empty()) {
                // Automation without --settings has no checkpoint destination.
                saveRun();
            }
        }
        quit_ = true;
        return true;
    } catch (const std::exception& error) {
        supportError_ = error.what();
        if (!run_.isOpen()) {
            modal_ = "support_error";
        }
        return false;
    }
}

void Menu::activate(MenuButton& b) {
    if (!b.active) {
        return;
    }
    if (b.id == "continue" && !savePath_.empty()) {
        try {
            continueRun();
        } catch (const std::exception& error) {
            supportError_ = error.what();
            modal_ = "support_error";
        }
        return;
    }
    if (b.id == "tutorial") {
        openHangar();
        run_.startTutorial(hangar_, locale_, seed_, true);
        hangar_.close();
        modal_.clear();
        return;
    }
    if (b.id == "options" || b.id == "stats" || b.id == "credits") {
        modal_ = b.id;
        if (modal_ == "stats") {
            openStats(false);
        }
        if (modal_ == "credits") {
            creditShip_.clear();
            creditCrew_.clear();
            creditScroll_ = -70;
            creditPause_ = 0;
        }
        return;
    }
    if (b.id == "quit") {
        quit_ = true;
        return;
    }
    if (b.id == "new_game") {
        if (hasSave_) {
            modal_ = "new_game_confirmation";
            yesHover_ = noHover_ = false;
        } else {
            openHangar();
        }
        return;
    }
    boundary_ = b.id;
    modal_ = "phase_boundary";
}

void Menu::terminalStep() {
    if (!run_.isOpen() || run_.tutorialRunning() || !run_.defeated()) {
        return;
    }
    if (!recordRun()) {
        run_.notify(std::string("Profile save failed: ") + supportError_);
        return;
    }
    if (!endingPresented_) {
        endingPresented_ = true;
        if (run_.campaignVictory()) {
            const auto state = run_.state();
            creditShip_ = state.at("ship_name").get<std::string>();
            std::vector<std::string> survivors;
            for (const auto& crew : state.at("crew")) {
                if (crew.at("health").get<double>() > 0) {
                    survivors.push_back(crew.at("name").get<std::string>());
                }
            }
            creditCrew_.clear();
            for (std::size_t i = 0; i < survivors.size(); ++i) {
                if (i != 0) {
                    creditCrew_ += texts_.get(
                        i + 1 == survivors.size() ? "crewlist_and" : "crewlist_comma", locale_);
                }
                creditCrew_ += survivors[i];
            }
            modal_ = "credits";
            creditScroll_ = 0;
            creditPause_ = 5;
        }
    }
}

void Menu::routeGameOver() {
    const int command = run_.takeGameOverCommand();
    if (command < 0) {
        return;
    }
    if (command == 7 || command == 8) {
        openRunOptions();
        if (command == 8) {
            modal_ = "controls";
            pauseControlsDirect_ = true;
            controlEditing_.clear();
            controlReset_ = false;
        }
        return;
    }
    if (command == 9) {
        if (run_.tutorialRunning()) {
            return;
        }
        try {
            saveRun();
        } catch (const std::exception& error) {
            supportError_ = error.what();
            return;
        }
        run_.close();
        quit_ = true;
        return;
    }
    if (command == 5 && !run_.defeated()) {
        // CApp::OnLoop0x4022c0 case5 skips the ordinary save during a tutorial.
        if (run_.tutorialRunning()) {
            run_.close();
            return;
        }
        try {
            saveRun();
            run_.close();
        } catch (const std::exception& error) {
            supportError_ = error.what();
            return;
        }
        return;
    }
    if (run_.tutorialRunning()) {
        if (command == 1) {
            run_.startTutorial(hangar_, locale_, seed_, true);
            audio_.restartLoops();
        } else {
            run_.close();
            if (command == 2) {
                openHangar();
            } else if (command == 4) {
                quit_ = true;
            }
        }
        return;
    }
    if (!recordRun()) {
        run_.notify(std::string("Profile save failed: ") + supportError_);
        return;
    }
    hasSave_ = false;
    buttons_[0].active = false;
    if (!savePath_.empty()) {
        std::error_code error;
        std::filesystem::remove(savePath_, error);
    }
    if (command == 1) {
        run_.progression(profile_);
        run_.start(hangar_, locale_, seed_, true);
        audio_.restartLoops();
        endingPresented_ = false;
        return;
    }
    run_.close();
    if (command == 0) {
        openStats(true);
    } else if (command == 2) {
        openHangar();
    } else if (command == 4) {
        quit_ = true;
    }
}

void Menu::click(float x, float y) {
    logging::ClickTrace trace("menu", x, y, false);
    if (modal_ == "startup_notice") {
        trace.hit(startupNoticeHover_ >= 0
                      ? "startup notice choice " + std::to_string(startupNoticeHover_)
                      : "startup notice (no choice under pointer)");
        move(x, y);
        if (startupNoticeHover_ >= 0 && (controls_.gameplay.dialogKeys != 2 ||
                                         animationTick_ - startupNoticeOpenedTick_ >= 18)) {
            selectStartupNotice(startupNoticeHover_);
        }
        return;
    }
    if (run_.isOpen() && run_.writeErrorOpen()) {
        trace.hit("run write-error dialog");
        move(x, y);
        run_.writeErrorClick(x, y);
        return;
    }
    if (run_.isOpen() && !modal_.empty()) {
        trace.hit("in-run modal '" + modal_ + "'");
        move(x, y);
        if (modal_ == "language_selection" && languageHover_ >= 0) {
            language(std::string(languages[languageHover_]));
            languageSelected_ = true;
            modal_ = "options";
            optionsLanguage_ = false;
        } else {
            supportClick(x, y);
        }
        syncRunModal();
        return;
    }
    if (modal_ == "credits") {
        trace.hit("credits (ignored)");
        return;
    }
    if (run_.isOpen()) {
        trace.hit("run");
        run_.click(x, y);
        routeGameOver();
        syncRunProgression();
        return;
    }
    if (hangar_.isOpen()) {
        trace.hit("hangar");
        hangar_.click(x, y);
        controls_.difficulty = hangar_.difficulty();
        launch();
        return;
    }
    move(x, y);
    if (modal_ == "language_selection") {
        trace.hit(languageHover_ >= 0 ? "language " + std::to_string(languageHover_)
                                      : "language selection (no language under pointer)");
        if (languageHover_ >= 0) {
            language(std::string(languages[languageHover_]));
            languageSelected_ = true;
            modal_ = optionsLanguage_ ? "options" : "";
            optionsLanguage_ = false;
            hoveredSoundTarget_.clear();
            animationTick_ = 0;
            audio_.active(true);
            openStartupNotice();
            move(x, y);
        }
        return;
    }
    if (modal_ == "new_game_confirmation") {
        trace.hit(noHover_    ? "new game confirmation: no"
                  : yesHover_ ? "new game confirmation: yes"
                              : "new game confirmation (no button under pointer)");
        if (noHover_) {
            modal_.clear();
            yesHover_ = noHover_ = false;
            move(mouseX_, mouseY_);
        } else if (yesHover_) {
            try {
                // MainMenu::MouseClick4c6580 deletes existing save files when
                // confirmation succeeds, before opening the hangar.
                if (!savePath_.empty()) {
                    std::filesystem::remove(savePath_);
                }
            } catch (const std::exception& error) {
                supportError_ = error.what();
                modal_ = "support_error";
                return;
            }
            hasSave_ = false;
            buttons_[0].active = false;
            modal_.clear();
            openHangar();
        }
        return;
    }
    if (!modal_.empty()) {
        trace.hit("menu modal '" + modal_ + "'");
        supportClick(x, y);
        if (!supportPointerHeld_) {
            supportSlider_ = -1;
        }
        return;
    }
    for (auto& b : buttons_) {
        if (b.hover) {
            trace.hit("main menu button '" + b.id + "'" + (b.active ? "" : " (inactive)"));
            activate(b);
            return;
        }
    }
}

void Menu::keyUp(const std::string& key) {
    const auto modifier = controls_.hotkeys.find("force_autofire");
    if (run_.isOpen() && modifier != controls_.hotkeys.end() && modifier->second > 0 &&
        originalKey(key) == modifier->second) {
        run_.key("Release:Hotkey:force_autofire");
    }
}

bool Menu::repeatTextKey(const std::string& key) {
    if (!modal_.empty()) {
        return false;
    }
    // SIL text-control events are separate from key-down/modifier dispatch.
    const auto plain = key.starts_with("Shift+") ? key.substr(6) : key;
    if (run_.isOpen()) {
        return run_.repeatTextKey(plain);
    }
    return hangar_.isOpen() && hangar_.repeatTextKey(plain);
}

void Menu::key(const std::string& key) {
    if (repeatTextKey(key)) {
        return;
    }
    if (modal_ == "startup_notice") {
        if (controls_.gameplay.dialogKeys != 0 &&
            (controls_.gameplay.dialogKeys != 2 ||
             animationTick_ - startupNoticeOpenedTick_ >= 18) &&
            key.size() == 1 && key[0] >= '1' && key[0] <= '9') {
            selectStartupNotice(key[0] - '1');
        }
        return;
    }
    if (run_.isOpen() && run_.writeErrorOpen() && gameplayKey(key) == "Space") {
        // CApp::OnKeyDown 0x401f80 runs CommandGui::SpaceBar for the pause
        // hotkey before CommandGui::KeyDown hands keys to the write-error box.
        run_.key("Hotkey:pause");
        return;
    }
    if (run_.isOpen() && run_.writeErrorKey(key)) {
        return;
    }
    if (modal_ == "credits") {
        modal_.clear();
        return;
    }
    const auto forceBinding = controls_.hotkeys.find("force_autofire");
    if (run_.isOpen() && forceBinding != controls_.hotkeys.end() && forceBinding->second > 0 &&
        originalKey(key) == forceBinding->second) {
        run_.key("Hotkey:force_autofire");
    }
    // CommandGui::KeyDown 0x5201f0 reaches the options hotkey only after no
    // open focus window took the key, so typing 'o' in a crew name (or any
    // key over a ship screen, store, map, event or menu) never opens it.
    if (run_.isOpen() && modal_.empty() && gameplayKey(key) == "options" &&
        run_.optionsHotkeyReachable()) {
        openRunOptions();
        return;
    }
    if (run_.isOpen() && !modal_.empty()) {
        if (modal_ == "options_restart" && (key == "1" || key == "Enter" || key == "Escape")) {
            supportAction("option:restart_continue");
        } else if (modal_ == "options_delete" && key == "Escape") {
            supportAction("option:delete_cancel");
        } else if (!controlKey(key) && key == "Escape") {
            if (modal_ == "language_selection" && optionsLanguage_) {
                modal_ = "options";
                optionsLanguage_ = false;
            } else {
                supportAction("support:close");
            }
        }
        syncRunModal();
        return;
    }
    if (modal_ == "options_restart" && (key == "1" || key == "Enter" || key == "Escape")) {
        supportAction("option:restart_continue");
        return;
    }
    if (modal_ == "options_delete" && key == "Escape") {
        supportAction("option:delete_cancel");
        return;
    }
    if (modal_ == "credits") {
        modal_.clear();
        return;
    }
    if (run_.isOpen()) {
        const auto state = run_.state();
        const bool eventChoice =
            state.at("event_open").get<bool>() && key.size() == 1 && key[0] >= '1' && key[0] <= '9';
        const bool contextInput = run_.writeErrorOpen() || key == "Escape" || key == "F12" ||
                                  eventChoice ||
                                  !state.at("voyage").at("panel").get<std::string>().empty() ||
                                  state.at("developer_ui").value("open", false);
        const auto hotkey = gameplayKey(key);
        // CApp::OnKeyDown 0x401f80 calls CommandGui::SpaceBar for the pause
        // hotkey, and CommandGui::KeyDown applies the font hotkeys, before
        // any open window receives the key.
        const auto mapped = !contextInput       ? hotkey
                            : hotkey == "Space" ? std::string("Hotkey:pause")
                            : hotkey == "Hotkey:increase_font" || hotkey == "Hotkey:decrease_font"
                                ? hotkey
                                : key;
        if (!mapped.empty()) {
            run_.key(mapped);
        }
        routeGameOver();
        syncRunProgression();
        return;
    }
    if (hangar_.isOpen()) {
        hangar_.key(key);
        controls_.difficulty = hangar_.difficulty();
        syncHangarProfile();
        return;
    }
    if (controlKey(key)) {
        return;
    }
    if (key == "Escape") {
        if (modal_ == "stats" && statsShipList_) {
            statsShipList_ = false;
            return;
        }
        if (modal_ == "options") {
            supportAction("support:close");
            return;
        }
        if (modal_ == "language_selection") {
            if (optionsLanguage_) {
                modal_ = "options";
                optionsLanguage_ = false;
            }
            return;
        }
        modal_.clear();
        boundary_.clear();
        yesHover_ = noHover_ = false;
        move(mouseX_, mouseY_);
    }
}

void Menu::step(std::uint32_t ticks) {
    if (ticks > 1000000) {
        throw std::runtime_error("invalid_step_count");
    }
    if (ticks) {
        hangar_.applicationClockInitialized();
    }
    uiFraction_ = 0;
    audio_.advance(double(ticks) / Time::tickHz);
    animationTick_ += ticks;
    startupNoticeStep();
    if (languageSelected_) {
        run_.applicationUiStep(ticks);
    }
    if (run_.isOpen()) {
        run_.step(ticks);
        run_.updatePointer();
        run_.updateAudio();
        terminalStep();
        syncRunProgression();
    }
    if (hangar_.isOpen()) {
        hangar_.step(ticks);
        syncHangarProfile();
    }
}

Json Menu::timing() const {
    return {{"wall_seconds", time_.wallSeconds},
            {"simulation_speed", time_.speed},
            {"simulation_ticks", time_.simulationTicks},
            {"pending_ticks", time_.pendingTicks},
            {"interpolation_alpha", TimeController::alpha(time_)},
            {"tick_hz", Time::tickHz}};
}

void Menu::advance(double seconds) {
    // Validation precedes any scene or UI mutation.
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 3600) {
        throw std::runtime_error("wall_seconds_requires_0_to_3600");
    }
    hangar_.applicationClockInitialized();
    audio_.advance(seconds);
    uiRemaining_ += seconds * Time::tickHz;
    const auto uiTicks = std::uint32_t(uiRemaining_ + 1e-9);
    uiRemaining_ = std::max(0.0, uiRemaining_ - uiTicks);
    uiFraction_ = float(uiRemaining_);
    animationTick_ += uiTicks;
    startupNoticeStep();
    if (languageSelected_) {
        run_.applicationUiStep(uiTicks);
    }
    creditsStep(uiTicks);
    if (hangar_.isOpen()) {
        hangar_.step(uiTicks);
        hangar_.presentation(uiFraction_);
        syncHangarProfile();
    }
    if (run_.isOpen()) {
        run_.uiStep(uiTicks);
    }
    TimeController::advance(time_, seconds, [&] {
        if (run_.isOpen()) {
            run_.step(1, false);
        }
    });
    run_.presentation(time_.speed == 0 ? 1 : TimeController::alpha(time_), uiFraction_);
    if (run_.isOpen()) {
        run_.updatePointer();
    }
    run_.updateAudio();
    terminalStep();
    syncRunProgression();
}

void Menu::semantic(const std::string& target, const std::string& action) {
    if (modal_ == "startup_notice") {
        const auto rectangles = startupNoticeRects();
        for (std::size_t i = 0; i < rectangles.size(); ++i) {
            const std::string id = startupScreenChanged_
                                       ? (i == 0 ? "startup:keep_screen" : "startup:revert_screen")
                                       : "startup:continue";
            if (target == id && (action == "hover" || action == "click")) {
                const auto& rect = rectangles[i];
                if (action == "hover") {
                    move(rect.x + rect.w / 2, rect.y + rect.h / 2);
                } else {
                    click(rect.x + rect.w / 2, rect.y + rect.h / 2);
                }
                return;
            }
        }
        throw std::runtime_error("target_not_available");
    }
    if (run_.isOpen() && run_.writeErrorOpen()) {
        if (target != "write_error:continue") {
            throw std::runtime_error("target_not_available");
        }
        const auto rect = run_.writeErrorChoiceRect();
        const float x = rect.x + rect.w / 2;
        const float y = rect.y + rect.h / 2;
        if (action == "hover") {
            move(x, y);
        } else if (action == "click") {
            click(x, y);
        } else {
            throw std::runtime_error("unsupported_input_action");
        }
        return;
    }
    if (run_.isOpen() && action == "click" && target == "options" && modal_.empty()) {
        openRunOptions();
        return;
    }
    if (run_.isOpen() && !modal_.empty()) {
        if (modal_ == "language_selection") {
            for (int i = 0; i < int(languages.size()); ++i) {
                if (target == "language:" + std::string(languages[i])) {
                    if (action == "hover") {
                        move(641, float(224 + 37 * i));
                    } else if (action == "click") {
                        click(641, float(224 + 37 * i));
                    }
                    return;
                }
            }
        }
        if (action == "click") {
            supportAction(target);
            syncRunModal();
        }
        return;
    }
    if (!run_.isOpen() && !hangar_.isOpen() && action == "click" && supportAction(target)) {
        return;
    }
    if (run_.isOpen()) {
        run_.semantic(target, action);
        routeGameOver();
        if (action != "hover") {
            syncRunProgression();
        }
        return;
    }
    if (hangar_.isOpen()) {
        hangar_.semantic(target, action);
        controls_.difficulty = hangar_.difficulty();
        launch();
        return;
    }
    if (action != "hover" && action != "click") {
        throw std::runtime_error("unsupported_input_action");
    }
    Rect rect{};
    bool found = false;
    if (modal_ == "language_selection") {
        for (int i = 0; i < int(languages.size()); ++i) {
            if (target == "language:" + std::string(languages[i])) {
                rect = {513, float(211 + 37 * i), 256, 26};
                found = true;
                break;
            }
        }
    } else if (modal_ == "new_game_confirmation" && (target == "confirm" || target == "cancel")) {
        rect = target == "confirm" ? yes_ : no_;
        found = true;
    } else if (modal_.empty()) {
        for (const auto& b : buttons_) {
            if (b.id == target) {
                rect = b.rect;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        throw std::runtime_error("target_not_available");
    }
    const auto x = rect.x + rect.w / 2;
    const auto y = rect.y + rect.h / 2;
    if (action == "hover") {
        move(x, y);
    } else {
        click(x, y);
    }
}

void Menu::textButton(Rect rect, const std::string& label, bool hover) {
    // TextButton::InitPrimitives uses one-pixel stepped corners, no outline.
    for (int row = 0; row < int(rect.h); ++row) {
        float inset = float(std::max(0, 4 - std::min(row, int(rect.h) - row - 1)));
        renderer_.fill({rect.x + inset, rect.y + row, rect.w - 2 * inset, 1},
                       hover ? selected : on);
    }
    auto& f = localizedFont(62, label);
    // Font 62 overrides HL2's bitmap baseline from 18 to 16 at 2x.
    const int lineHeight = int(Renderer::sourceLineHeight(62, f));
    const auto lines = renderer_.wrappedLines(f, label, 0, fontScale(), locale_ == "zh-Hans");
    float y = rect.y + int(rect.h) / 2 + 2 - lineHeight * int(lines.size()) / 2;
    for (const auto& line : lines) {
        renderer_.text(f, line, rect.x + int(rect.w) / 2, y, fontScale(), ink, true);
        y += float(lineHeight);
    }
}

void Menu::languagePicker(bool pointer) {
    renderer_.text(renderer_.font("fonts/JustinFont11Bold.font"),
                   texts_.get("choose_language"),
                   640,
                   165,
                   1,
                   {255, 255, 255, 255},
                   true);
    float y = 194;
    for (int i = 0; i < int(languages.size()); ++i) {
        renderer_.draw("img/optionsUI/language_" +
                           std::string(i == 0   ? "top"
                                       : i == 8 ? "bottom"
                                                : "mid") +
                           ".png",
                       496,
                       y);
        Rect rect{513, float(211 + 37 * i), 256, 26};
        if (i == 8) {
            for (int row = 0; row < int(rect.h); ++row) {
                float inset = float(std::max(0, 4 - std::min(row, int(rect.h) - row - 1)));
                renderer_.fill({rect.x + inset, rect.y + row, rect.w - 2 * inset, 1},
                               i == languageHover_ ? selected : on);
            }
            auto& font = renderer_.chineseFont(16, true, languageLabels_[i]);
            renderer_.text(font, languageLabels_[i], 641, rect.y + 3, 1, ink, true);
        } else {
            // The chooser uses each label's own language, independently of
            // the last selected menu locale (important for replay fixtures).
            auto locale = locale_;
            locale_ = "en";
            textButton(rect, languageLabels_[i], i == languageHover_);
            locale_ = locale;
        }
        y += i == 0 ? 46 : 37;
    }
    if (pointer && mouseX_ >= 0 && mouseY_ >= 0) {
        renderer_.draw(languageHover_ >= 0 ? "img/mouse/pointerValid.png"
                                           : "img/mouse/pointerInvalid.png",
                       mouseX_,
                       mouseY_);
    }
}

void Menu::confirmation() {
    // ConfirmWindow::OnRender 0x544e30: preserve the original 3-column top
    // and nine-piece lower tab strip, rather than stretch its border corners.
    auto& t = renderer_.image("img/confirm_window.png");
    const auto [x, y, w, h] = confirmationRect_;
    auto part = [&](Rect src, Rect dst) {
        renderer_.draw(t, dst, &src);
    };
    const std::array<float, 3> sx{0, 70, 71};
    const std::array<float, 3> sw{70, 1, 69};
    const std::array<float, 3> dx{x, x + 70, x + w - 69};
    const std::array<float, 3> dw{70, w - 139, 69};
    for (int i = 0; i < 3; ++i) {
        part({sx[i], 0, sw[i], 26}, {dx[i], y, dw[i], 26});
        part({sx[i], 26, sw[i], 1}, {dx[i], y + 26, dw[i], h - 21});
        part({sx[i], 27, sw[i], 17}, {dx[i], y + h + 5, dw[i], 17});
    }
    const float ex = no_.x + no_.w;
    const std::array<float, 9> bsx{0, 20, 21, 52, 53, 87, 88, 119, 120};
    const std::array<float, 9> bsw{20, 1, 31, 1, 34, 1, 31, 1, 20};
    const std::array<float, 9> bdx{x,
                                   x + 20,
                                   yes_.x - 21,
                                   yes_.x + 10,
                                   yes_.x + yes_.w - 10,
                                   no_.x + 10,
                                   ex - 10,
                                   ex + 21,
                                   x + w - 20};
    const std::array<float, 9> bdw{
        20, yes_.x - x - 41, 31, yes_.w - 20, 34, no_.w - 20, 31, x + w - 20 - (ex + 21), 20};
    for (int i = 0; i < 9; ++i) {
        part({bsx[i], 44, bsw[i], 46}, {bdx[i], y + h + 22, bdw[i], 46});
    }
    auto& f = localizedFont(52, confirmationText_);
    float ty = y + 17;
    for (const auto& line : confirmationLines_) {
        renderer_.text(f, line, 640, ty, fontScale(), on, true);
        ty += Renderer::sourceLineHeight(52, f);
    }
    textButton(yes_, yesText_, yesHover_);
    textButton(no_, noText_, noHover_);
}

void Menu::renderBoundary() {
    if (run_.isOpen() && modal_ != "credits") {
        run_.externalModal(!modal_.empty());
        run_.renderBoundary();
    }
}

void Menu::render() {
    if (modal_ == "credits") {
        renderer_.clear();
        creditsRender();
        run_.renderApplicationPopup();
        // CreditScreen5597a0 resets MouseControl rather than hiding it.
        if (mouseX_ >= 0 && mouseY_ >= 0) {
            renderer_.draw("img/mouse/pointerInvalid.png", mouseX_, mouseY_);
        }
        ++frame_;
        return;
    }
    if (run_.isOpen()) {
        run_.render();
        if (!modal_.empty()) {
            if (modal_ == "language_selection") {
                if (optionsLanguage_) {
                    renderer_.setTint(.25f);
                    optionsRender();
                    renderer_.setTint(1);
                }
                languagePicker(false);
            } else {
                supportRender();
            }
        }
        run_.renderWriteError();
        run_.renderApplicationPopup();
        run_.renderOuter();
        ++frame_;
        return;
    }
    if (hangar_.isOpen()) {
        hangar_.render();
        run_.renderApplicationPopup();
        hangar_.renderOuter();
        ++frame_;
        return;
    }
    if (modal_ == "language_selection" && !optionsLanguage_) {
        renderer_.clear();
        languagePicker(false);
        run_.renderApplicationPopup();
        if (mouseX_ >= 0 && mouseY_ >= 0) {
            renderer_.draw(languageHover_ >= 0 ? "img/mouse/pointerValid.png"
                                               : "img/mouse/pointerInvalid.png",
                           mouseX_,
                           mouseY_);
        }
        ++frame_;
        return;
    }
    renderer_.clear();
    float tint = modal_.empty() ? 1.f : 0.15f;
    renderer_.draw("img/main_menus/main_base2.png", 0, 0, tint);
    float time = (float(animationTick_ % 180) + uiFraction_) * Time::step;
    renderer_.draw(
        "img/main_menus/main_FTL2.png", 820, 60, tint, (1.5f - std::abs(time - 1.5f)) / 1.5f);
    for (const auto& b : buttons_) {
        std::string suffix = !b.active ? "_off.png" : b.hover ? "_select2.png" : "_on.png";
        renderer_.draw(asset(b.asset + suffix), b.rect.x, b.rect.y, tint);
    }
    renderer_.draw("img/itb_banner_ftl_on.png", 0, 595, tint);
    auto& mech = renderer_.image("img/itb_mech_punch_a.png");
    Rect src{float((animationTick_ % 72) / 18) * 80, 0, 80, 68};
    renderer_.draw(mech, {25, 610, 80, 68}, &src, tint);
    auto& small = localizedFont(10, versionText_);
    const auto& version = versionText_;
    auto [vl, vr] = renderer_.bounds(small, version);
    auto c = Uint8(tint * 255);
    renderer_.text(small, version, float(1265 - int(vr - vl)), 700, 1, {c, c, c, 255});
    if (modal_ == "language_selection") {
        renderer_.setTint(.25f);
        optionsRender();
        renderer_.setTint(1);
        languagePicker(false);
        run_.renderApplicationPopup();
        if (mouseX_ >= 0 && mouseY_ >= 0) {
            renderer_.draw(languageHover_ >= 0 ? "img/mouse/pointerValid.png"
                                               : "img/mouse/pointerInvalid.png",
                           mouseX_,
                           mouseY_);
        }
        ++frame_;
        return;
    }
    if (modal_ == "new_game_confirmation") {
        confirmation();
    } else if (modal_ == "startup_notice") {
        startupNoticeRender();
    } else if (modal_ == "phase_boundary") {
        renderer_.fill({340, 280, 600, 150}, {25, 35, 45, 255});
        renderer_.text(small, "Reconstruction in progress", 640, 305, 1, on, true);
        renderer_.text(small,
                       boundary_ == "hangar" ? "Ship selection is the next phase (P02)."
                                             : "This menu branch is scheduled for P08.",
                       640,
                       342,
                       1,
                       on,
                       true);
        renderer_.text(small, "Press Escape to return to the main menu.", 640, 385, 1, on, true);
    }
    supportRender();
    run_.renderApplicationPopup();
    if (modal_ == "stats") {
        statsTooltipRender(true);
    }
    // CFPS speed16 * 0.0625 gives seconds; strict >0.15 at 60 Hz is tick10.
    if (modal_.empty() && !hasSave_ && buttons_[0].hover && animationTick_ - hoverTick_ >= 10) {
        renderer_.tooltip("", noSaveText_, {mouseX_, mouseY_}, 0, locale_ == "zh-Hans");
    }
    if (mouseX_ >= 0 && mouseY_ >= 0) {
        bool valid = modal_ == "startup_notice" ? startupNoticeHover_ >= 0
                     : modal_ == "new_game_confirmation"
                         ? (yesHover_ || noHover_)
                         : modal_.empty() &&
                               std::any_of(buttons_.begin(), buttons_.end(), [](const auto& b) {
                                   return b.hover && b.active;
                               });
        renderer_.draw(valid ? "img/mouse/pointerValid.png" : "img/mouse/pointerInvalid.png",
                       mouseX_,
                       mouseY_);
    }
    ++frame_;
}

Json Menu::state() {
    if (run_.isOpen()) {
        auto result = run_.state();
        result["continue_available"] = hasSave_;
        result["credits"] = {{"scroll", creditScroll_},
                             {"pause", creditPause_},
                             {"ship", creditShip_},
                             {"crew", creditCrew_}};
        result["support_modal"] = modal_.empty() ? Json(nullptr) : Json(modal_);
        result["options"] = optionsState();
        result["native_profile"] = profile_;
        result["crew_records"] = run_.crewRecords();
        result["run_statistics"] = run_.runStatistics();
        result["control_state"] = run_.controlState();
        result["support_error"] = supportError_;
        result["options_delete_confirmation"] = modal_ == "options_delete";
        return result;
    }
    if (hangar_.isOpen()) {
        return hangar_.state();
    }
    Json resetGeometry = nullptr;
    if (modal_ == "controls" && controlReset_) {
        const auto layout = controlConfirmation();
        const auto rect = [](const Rect& r) {
            return Json::array({r.x, r.y, r.w, r.h});
        };
        resetGeometry = {{"body", rect(layout.body)},
                         {"yes", rect(layout.yes)},
                         {"no", rect(layout.no)},
                         {"lines", layout.lines}};
    }
    Json languageButtons = Json::array();
    for (int i = 0; i < int(languages.size()); ++i) {
        languageButtons.push_back({{"language", languages[i]},
                                   {"label", languageLabels_[i]},
                                   {"hitbox", {513, 211 + 37 * i, 256, 26}}});
    }
    Json buttons = Json::array();
    for (const auto& b : buttons_) {
        buttons.push_back({{"id", b.id},
                           {"position", {b.rect.x, b.rect.y}},
                           {"hitbox", {b.rect.x, b.rect.y + 2, b.rect.w, b.rect.h}},
                           {"enabled", b.active},
                           {"hover", b.hover}});
    }
    Json animation = {
        {"glow_seconds", double(animationTick_ % 180) / 60},
        {"glow_period", 3.0},
        {"banner_frame", (animationTick_ % 72) / 18},
        {"banner_period", 1.2},
        {"hover_seconds", double(animationTick_ - hoverTick_) / 60},
        {"tooltip_visible",
         modal_.empty() && !hasSave_ && buttons_[0].hover && animationTick_ - hoverTick_ >= 10}};
    return {{"scene", languageSelected_ ? "main_menu" : "language_selection"},
            {"language_selected", languageSelected_},
            {"language_hover", languageHover_},
            {"language_buttons", languageButtons},
            {"main_menu_open", languageSelected_},
            {"hangar_open", false},
            {"modal", modal_.empty() ? Json(nullptr) : Json(modal_)},
            {"requested_scene", boundary_.empty() ? Json(nullptr) : Json(boundary_)},
            {"continue_available", hasSave_},
            {"native_profile", profile_},
            {"credits",
             {{"scroll", creditScroll_}, {"pause", creditPause_}, {"names", creditNames_.size()}}},
            {"stats_last_playthrough", statsLastPlaythrough_},
            {"stats_tabs", {{"ship_best", statsShipBest_}, {"achievements", statsAchievements_}}},
            {"stats_achievements", statsAchievementsState()},
            {"stats_ship", statsShipState()},
            {"stats_ship_achievements", statsShipAchievementsState()},
            {"options_delete_confirmation", modal_ == "options_delete"},
            {"support_error", supportError_},
            {"startup_notice", startupNoticeState()},
            {"options", optionsState()},
            {"controls",
             {{"hotkeys", controls_.hotkeys},
              {"editing", controlEditing_},
              {"page", controlPage_},
              {"reset_confirmation", controlReset_},
              {"reset_geometry", resetGeometry},
              {"implemented", supportedHotkeys()}}},
            {"user_pause", false},
            {"modal_pause", !modal_.empty()},
            {"simulation_tick", 0},
            {"animation_tick", animationTick_},
            {"animation", animation},
            {"frame_count", frame_},
            {"seed", seed_},
            {"language", locale_},
            {"logical_size", {1280, 720}},
            {"buttons", buttons},
            {"quitting", quit_},
            {"gameplay_available", false},
            {"confirmation",
             {{"position", {confirmationRect_.x, confirmationRect_.y}},
              {"width", confirmationRect_.w},
              {"text_height", confirmationRect_.h},
              {"confirm_hitbox", {yes_.x, yes_.y, yes_.w, yes_.h}},
              {"cancel_hitbox", {no_.x, no_.y, no_.w, no_.h}}}}};
}
} // namespace ftl
