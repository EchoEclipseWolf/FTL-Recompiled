#include "run.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace ftl {
namespace {
constexpr std::array<int, 12> upgradeOrder{0, 1, 5, 13, 2, 3, 4, 9, 10, 14, 15, 11};

std::string nebulaStartEvent(const std::string& prepared) {
    // StarMap::GenerateNebulas 0x5d17e0 on a location that already has its
    // event: environment = 3 (nebula), then the GetNebulaEffect 0x5eba80
    // status (sensors limited to zero on both ships) appended last.
    pugi::xml_document document;
    if (!document.load_string(prepared.c_str())) {
        throw std::runtime_error("invalid_prepared_start_event");
    }
    auto event = document.document_element();
    while (event.remove_child("environment")) {
    }
    event.append_child("environment").append_attribute("type").set_value("nebula");
    auto status = event.append_child("status");
    status.append_attribute("type").set_value("limit");
    status.append_attribute("system").set_value("sensors");
    status.append_attribute("amount").set_value(0);
    status.append_attribute("target").set_value("all");
    std::ostringstream serialized;
    event.print(serialized, "", pugi::format_raw);
    return serialized.str();
}

Rect sectorCancelRect(Renderer& renderer, const std::string& label, bool chinese) {
    // TextButton575660/577750: base minimum103, margin3, right aligned.
    const float width =
        std::max(103.f,
                 std::round(renderer.measure(renderer.sourceFont(63, label, chinese),
                                             label,
                                             Renderer::sourceFontScale(63, chinese))) +
                     6.f);
    return {903.f - width, 552, width, 32};
}

void roundedButtonFace(Renderer& renderer, Rect rect, int inset, SDL_Color color) {
    // TextButton::InitPrimitives0x577040 creates solid scanline strips.
    for (int row = 0; row < inset; ++row) {
        const float trim = static_cast<float>(inset - row);
        renderer.fill({rect.x + trim, rect.y + row, rect.w - 2 * trim, 1}, color);
        renderer.fill({rect.x + trim, rect.y + rect.h - row - 1, rect.w - 2 * trim, 1}, color);
    }
    renderer.fill({rect.x, rect.y + inset, rect.w, rect.h - 2 * inset}, color);
}

struct CrewConfirmationLayout {
    float width = 300;
    float height = 0;
    float yesWidth = 0;
    float noWidth = 0;
    float yesX = 0;
    float noX = 0;
    std::vector<std::string> lines;
};

CrewConfirmationLayout crewConfirmationLayout(Renderer& renderer,
                                              const std::string& message,
                                              const std::string& yes,
                                              const std::string& no,
                                              bool chinese) {
    CrewConfirmationLayout result;
    const auto labelWidth = [&](const std::string& label) {
        const auto bounds = renderer.bounds(
            renderer.sourceFont(62, label, chinese), label, Renderer::sourceFontScale(62, chinese));
        return std::round(bounds.second - bounds.first) + 6.f;
    };
    result.yesWidth = labelWidth(yes);
    result.noWidth = labelWidth(no);
    result.width = std::max(300.f, result.yesWidth + result.noWidth + 100);
    auto& font = renderer.sourceFont(52, message, chinese);
    result.lines = renderer.wrappedLines(
        font, message, result.width - 40, Renderer::sourceFontScale(52, chinese), chinese);
    result.height =
        std::round(static_cast<float>(result.lines.size()) * Renderer::sourceLineHeight(52, font));
    result.yesX = static_cast<float>(static_cast<int>(result.width) / 2 -
                                     static_cast<int>(result.yesWidth + result.noWidth + 14) / 2);
    result.noX = result.yesX + result.yesWidth + 14;
    return result;
}

int reactorUpgradeCost(int bars) {
    // ReactorButton::GetReactorCost 0x532a20, with no pending upgrades.
    return bars > 4 ? 5 * (bars / 5) + 15 : 30;
}

int indexOf(std::string_view value) {
    int result = -1;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : -1;
}

Rect sectorNameRect(Renderer& renderer, const SectorNode& node, std::size_t choice, bool chinese) {
    // RenderSectorName 0x5cee20: numbered font7 boxes sit above the first
    // neighboring node and below the second, rather than across graph edges.
    const auto label = std::to_string(choice + 1) + ". " + node.shortTitle;
    auto& font = renderer.sourceFont(7, label, chinese);
    const float measured =
        5.f + renderer.measure(font, label, Renderer::sourceFontScale(7, chinese));
    return {415.f + node.level * 66.f - static_cast<int>(measured - 35.f),
            300.f + node.row - (choice == 0 ? 40.f : -20.f),
            measured + 7.f,
            Renderer::sourceLineHeight(7, font) + 6.f};
}
} // namespace

std::string Run::panelHoverSoundTarget(float x, float y) const {
    // GenericButton573850: enabled button entry requests hoverBeep. TabbedWindow505e10
    // skips its current tab; Upgrades531270 includes upgrade boxes, Undo and reactor.
    if (panel_ == "store") {
        return storeHoverSoundTarget(x, y);
    }
    if (panel_.starts_with("pause_confirm:")) {
        const auto layout = crewConfirmationLayout(renderer_,
                                                   texts_.get("confirm_end_game", locale_),
                                                   texts_.get("confirm_yes", locale_),
                                                   texts_.get("confirm_no", locale_),
                                                   locale_ == "zh-Hans");
        const float fx = 640 + (tutorialOwned_ ? 150 : 0) - layout.width / 2;
        const float fy = std::floor(
            (720 - (renderer_.image("img/confirm_window.png").h + layout.height - 22)) / 2);
        const float by = fy + layout.height + 28;
        if (Rect{fx + layout.yesX, by, layout.yesWidth, 24}.contains(x, y)) {
            return "pause:yes";
        }
        if (Rect{fx + layout.noX, by, layout.noWidth, 24}.contains(x, y)) {
            return "pause:no";
        }
        return {};
    }
    if (panel_ == "pause") {
        const auto rect = pauseMenuRect();
        const auto buttons = voyageButtons();
        constexpr std::array<int, 7> offsets{18, 69, 106, 143, 180, 217, 254};
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            if (buttons[i].enabled &&
                Rect{rect.x + 17, rect.y + offsets[i], rect.w - 34, 24}.contains(x, y)) {
                return buttons[i].target;
            }
        }
        return {};
    }
    if (panel_ != "upgrades" && panel_ != "crew" && panel_ != "equipment") {
        return {};
    }
    if (panel_ == "crew" && dismissCrew_ != -1) {
        const auto living = crewRoster();
        const int display =
            dismissCrew_ == -2
                ? static_cast<int>(living.size())
                : static_cast<int>(
                      std::ranges::find(living, static_cast<std::size_t>(dismissCrew_)) -
                      living.begin());
        const float popupX =
            managementPanelX() + 28.f + static_cast<float>(display % 3) * 170.f +
            (display >= 6 && !enemy_.rewardCrewPending && living.size() <= 8 ? 86.f : 0.f);
        const float popupY = 266.f + static_cast<float>(display / 3) * 133.f;
        const auto layout = crewConfirmationLayout(renderer_,
                                                   texts_.get("confirm_dismiss", locale_),
                                                   texts_.get("confirm_yes", locale_),
                                                   texts_.get("confirm_no", locale_),
                                                   locale_ == "zh-Hans");
        const float by = popupY - 7 + layout.height + 28;
        if (Rect{popupX - 7 + layout.yesX, by, layout.yesWidth, 24}.contains(x, y)) {
            return "dismiss:confirm";
        }
        if (Rect{popupX - 7 + layout.noX, by, layout.noWidth, 24}.contains(x, y)) {
            return "dismiss:cancel";
        }
        return {};
    }
    if (renameCrew_ != -1) {
        return {};
    }
    const float paneX = managementPanelX();
    if (!tutorialBlocksPanelAccept() &&
        (panel_ != "crew" || crewRoster().size() <= std::size_t(devCrewLimit_)) &&
        Rect{paneX + 436, 556, 132, 32}.contains(x, y)) {
        return "panel:close";
    }
    if (!tutorialBlocksPanelAccept()) {
        const std::array<std::pair<std::string_view, Rect>, 3> tabs{
            std::pair{"upgrades", Rect{paneX + 5, 85, 89, 38}},
            std::pair{"crew", Rect{paneX + 97, 85, 97, 38}},
            std::pair{"equipment", Rect{paneX + 197, 85, 103, 38}}};
        for (const auto& [name, rect] : tabs) {
            if (panel_ != name && rect.contains(x, y)) {
                return "panel:" + std::string(name);
            }
        }
    }
    if (panel_ == "crew") {
        const auto living = crewRoster();
        for (std::size_t i = 0; i < living.size() + (enemy_.rewardCrewPending ? 1U : 0U) && i < 9;
             ++i) {
            const bool pending = i == living.size();
            const float cx =
                paneX + 75.f + static_cast<float>(i % 3) * 170.f +
                (i >= 6 && !enemy_.rewardCrewPending && living.size() <= 8 ? 86.f : 0.f);
            const float cy = 173.f + static_cast<float>(i / 3) * 133.f;
            if (Rect{cx, cy + 67, 100, 23}.contains(x, y)) {
                return pending ? "dismiss:new" : "dismiss:" + std::to_string(living[i]);
            }
            if (Rect{cx - 25, cy + 43, 150, 23}.contains(x, y)) {
                return pending ? "rename:new" : "rename:" + std::to_string(living[i]);
            }
        }
        return {};
    }
    if (panel_ != "upgrades") {
        return {};
    }
    if (!upgradePurchases_.empty() && Rect{paneX + 33, 556, 97, 32}.contains(x, y)) {
        return "upgrade:undo";
    }
    const auto upgrade = [&](int id, Rect rect) -> std::string {
        const auto* sys = system(id);
        return sys && sys->definition.power + pendingUpgrade(id) < systemMaximum(*sys) &&
                       rect.contains(x, y)
                   ? "upgrade:" + std::to_string(id)
                   : std::string{};
    };
    int slot = 0;
    for (int id : upgradeOrder) {
        if (!system(id)) {
            continue;
        }
        const auto target = upgrade(id, {paneX + 39.f + slot * 66.f, 200, 66, 150});
        if (!target.empty()) {
            return target;
        }
        if (++slot == 8) {
            break;
        }
    }
    for (int id : {6, 7, 8, 12}) {
        const auto target =
            upgrade(id, {paneX + 16.f + (id == 12 ? 3 : id - 6) * 66.f, 415, 66, 113});
        if (!target.empty()) {
            return target;
        }
    }
    if (ship_.reactor + pendingUpgrade(-1) < std::min(25, devReactorLimit_) &&
        Rect{paneX + 305, 412, 272, 113}.contains(x, y)) {
        return "upgrade:reactor";
    }
    return {};
}

int Run::pendingUpgrade(int id) const {
    return static_cast<int>(std::ranges::count_if(upgradePurchases_, [id](const auto& entry) {
        return entry.first == id;
    }));
}

bool Run::tutorialBlocksPanelAccept() const {
    if (!tutorialOwned_) {
        return false;
    }
    // TutorialManager::OnLoop 0x411950 controls TabbedWindow::bBlockClose.
    const auto& lesson = tutorial_.current().name;
    return lesson == "UPGRADES_1" || (lesson == "UPGRADES_1_2" && pendingUpgrade(3) <= 0) ||
           (lesson == "INVENTORY_2" &&
            std::none_of(ship_.weapons.begin(), ship_.weapons.end(), [&](const auto& weapon) {
                return catalog_.equipment(weapon).type == "BEAM";
            }));
}

void Run::finishUpgrades(bool accept) {
    // Upgrades::Close 0x531340 commits staged levels; Undo refunds the charged cart.
    for (const auto& [id, cost] : upgradePurchases_) {
        if (!accept) {
            scrap_ += cost;
        } else if (id == -1) {
            ++ship_.reactor;
            progressionRun_.tutorial = tutorialOwned_;
            progressionRun_.flag("reactor_upgrade");
        } else if (auto* sys = system(id)) {
            ++sys->definition.power;
            progressionRun_.tutorial = tutorialOwned_;
            progressionRun_.flag("system_upgrade");
            if (id == 3) {
                progressionRun_.flag("weapon_upgrade");
            }
            // ShipSystem::UpgradeSystem 0x615b80 leaves fully destroyed systems broken.
            if (sys->health > 0) {
                ++sys->health;
            }
            if ((id >= 6 && id <= 8) || id == 12) {
                ++sys->power;
            }
        }
    }
    upgradePurchases_.clear();
    reconcilePower();
}

void Run::crewTextInput(std::string_view value) {
    if (renameCrew_ == -1) {
        return;
    }
    // TextInput4ef6d0 inserts allowed language codepoints at its caret.
    auto characters = std::ranges::count_if(crewRenameText_, [](unsigned char byte) {
        return (byte & 0xc0) != 0x80;
    });
    crewRenameCaret_ = std::min(crewRenameCaret_, crewRenameText_.size());
    for (std::size_t offset = 0; offset < value.size() && characters < 15;) {
        const auto first = static_cast<unsigned char>(value[offset]);
        const std::size_t length = first < 128                    ? 1
                                   : first >= 194 && first <= 223 ? 2
                                   : first >= 224 && first <= 239 ? 3
                                   : first >= 240 && first <= 244 ? 4
                                                                  : 0;
        if (length == 0 || length > value.size() - offset) {
            ++offset;
            continue;
        }
        std::uint32_t codepoint = first & (length == 1 ? 127U : (1U << (7U - length)) - 1U);
        bool valid = true;
        for (std::size_t continuationIndex = 1; continuationIndex < length; ++continuationIndex) {
            const auto continuation = static_cast<unsigned char>(value[offset + continuationIndex]);
            if ((continuation & 192U) != 128U) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 63U);
        }
        const std::uint32_t minimum = length == 1   ? 0
                                      : length == 2 ? 128
                                      : length == 3 ? 2048
                                                    : 65536;
        if (!valid || codepoint < minimum || codepoint > 1114111 ||
            (codepoint >= 55296 && codepoint <= 57343)) {
            ++offset;
            continue;
        }
        if (sourceNameCharacterAllowed(codepoint, locale_)) {
            crewRenameText_.insert(crewRenameCaret_, value.substr(offset, length));
            crewRenameCaret_ += length;
            crewRenameBlink_ = 0;
            ++characters;
        }
        offset += length;
    }
}

void Run::crewRenameKey(const std::string& key) {
    if (renameCrew_ == -1) {
        return;
    }
    crewRenameCaret_ = std::min(crewRenameCaret_, crewRenameText_.size());
    const auto previous = [&] {
        auto offset = crewRenameCaret_;
        if (offset > 0) {
            --offset;
            while (offset > 0 &&
                   (static_cast<unsigned char>(crewRenameText_[offset]) & 0xc0) == 0x80) {
                --offset;
            }
        }
        return offset;
    };
    const auto next = [&] {
        auto offset = crewRenameCaret_;
        if (offset < crewRenameText_.size()) {
            ++offset;
            while (offset < crewRenameText_.size() &&
                   (static_cast<unsigned char>(crewRenameText_[offset]) & 0xc0) == 0x80) {
                ++offset;
            }
        }
        return offset;
    };
    const auto oldCaret = crewRenameCaret_;
    if (key == "Backspace" && crewRenameCaret_ > 0) {
        const auto start = previous();
        crewRenameText_.erase(start, crewRenameCaret_ - start);
        crewRenameCaret_ = start;
    } else if (key == "Delete" && crewRenameCaret_ < crewRenameText_.size()) {
        crewRenameText_.erase(crewRenameCaret_, next() - crewRenameCaret_);
    } else if (key == "Left") {
        crewRenameCaret_ = previous();
    } else if (key == "Right") {
        crewRenameCaret_ = next();
    } else if (key == "Home" || key == "Clear") {
        crewRenameCaret_ = 0;
        if (key == "Clear") {
            crewRenameText_.clear();
        }
    } else if (key == "End") {
        crewRenameCaret_ = crewRenameText_.size();
    } else if (key == "Enter" || key == "Escape") {
        // TEXT_DONE, or TEXT_CANCEL while the tutorial keeps the ship screen
        // open (CommandGui::KeyDown 0x5201f0 toggles the menu instead), only
        // deactivates input; CrewEquipBox52f410 commits the text, even empty.
        const auto shortName =
            catalog_.shortCrewName(crewRenameText_, [&](const std::string& value) {
                const auto [left, right] =
                    renderer_.bounds(localizedFont(6, value), value, textScale(6));
                return right - left;
            });
        if (renameCrew_ == -2 && enemy_.rewardCrewPending) {
            const auto oldName = enemy_.rewardCrewName;
            enemy_.rewardCrewName = shortName;
            enemy_.rewardCrewFullName = crewRenameText_;
            for (auto& row : pendingVoyageEffects_) {
                if (row.value("kind", std::string{}) == "crewSkills" &&
                    row.value("name", std::string{}) == oldName) {
                    row["name"] = shortName;
                }
            }
        } else if (renameCrew_ >= 0 && std::size_t(renameCrew_) < crew_.size()) {
            crew_[renameCrew_].appearance.name = shortName;
            crew_[renameCrew_].appearance.fullName = crewRenameText_;
            refreshCrewNames();
        }
        renameCrew_ = -1;
    }
    if (crewRenameCaret_ != oldCaret) {
        // Native UI boundary substitutes for the source next OnLoop caret latch.
        crewRenameBlink_ = 0;
    }
}

bool Run::docked() const {
    return open_ && !defeated_ && !hostile_ && !event_ && !mapOpen_ && jumpTicks_ == 0 &&
           arrivalTicks_ == 0;
}

bool Run::hostileEnvironment() const {
    // WorldManager::HostileEnvironment 0x5ee1f0 with SpaceManager::
    // DangerousEnvironment 0x417690: a PDS aimed at the enemy (target 1)
    // makes the whole environment safe; otherwise sun, asteroids, pulsar or
    // PDS, then intruders or a mind-controlled enemy aboard.
    const bool pds = environment_.pdsActive;
    const bool dangerous = (!pds || environment_.pdsTarget != 1) &&
                           (environment_.fireActive || environment_.asteroidActive ||
                            environment_.pulsarActive || pds);
    if (dangerous || playerIntruderCount_ > 0) {
        return true;
    }
    return std::ranges::any_of(enemy_.crew,
                               [](const auto& member) {
                                   return member.intruder && member.controlled && !member.outOfGame;
                               }) ||
           std::ranges::any_of(voyageBoarders_, [](const auto& member) {
               return member.controlled && !member.outOfGame;
           });
}

bool Run::shipButtonActive() const {
    // CommandGui::OnLoop 0x5221bc: the ship button is inactive while the
    // danger flag (+0x3FDE, WorldManager::OnLoop: hostile ship or
    // HostileEnvironment) is set, else TutorialManager::AllowUpgrades.
    return docked() && !hostileEnvironment() &&
           (!tutorialOwned_ || tutorial_.current().name == "COMBAT_5" ||
            tutorial_.current().name == "INVENTORY_1");
}

bool Run::hudStoreAvailable() const {
    return docked() && graph_.beacons().at(graph_.current()).store;
}

Rect Run::hudStoreRect() const {
    const auto label = texts_.get("button_store", locale_);
    const float width = std::max(
        78.f, std::round(renderer_.measure(localizedFont(62, label), label, textScale(62))) + 6.f);
    return {708, 25, width + 12, 49};
}

Rect Run::hudOptionsRect() const {
    return {hudStoreAvailable() ? 714.f + hudStoreRect().w - 12.f + 15.f : 702.f, 22, 55, 55};
}

void Run::initializeSectorTree() {
    std::vector<std::string> used;
    std::map<std::string, std::vector<pugi::xml_node>> names;
    sectorGraph_.generate(eventRandom_, [&](SectorNode& node) {
        node.name = events_.selectSector(node.type, node.level, advanced_, used, eventRandom_);
        if (!names.contains(node.name)) {
            for (auto name : events_.sector(node.name).child("nameList").children("name")) {
                names[node.name].push_back(name);
            }
        }
        auto& available = names[node.name];
        if (available.empty()) {
            throw std::runtime_error("sector_names_empty");
        }
        const auto choice = eventRandom_.next() % available.size();
        const auto selected = available[choice];
        node.title = selected.attribute("id")
                         ? texts_.get(selected.attribute("id").as_string(), locale_)
                         : selected.text().as_string();
        node.shortTitle = selected.attribute("short")
                              ? texts_.get(selected.attribute("short").as_string(), locale_)
                              : node.title;
        if (available.size() > 1) {
            available.erase(available.begin() + static_cast<std::ptrdiff_t>(choice));
        }
    });
    // GetSpecificSector 0x5b6ae0 uses the first authored name for fixed
    // start/final descriptions and consumes no random name draw.
    for (auto& node : sectorGraph_.nodes()) {
        if (node.level != 0 && node.name != "FINAL") {
            continue;
        }
        const auto fixed = events_.sector(node.name).child("nameList").child("name");
        node.title = fixed.attribute("id") ? texts_.get(fixed.attribute("id").as_string(), locale_)
                                           : fixed.text().as_string();
        node.shortTitle = fixed.attribute("short")
                              ? texts_.get(fixed.attribute("short").as_string(), locale_)
                              : node.title;
    }
    sectorName_ = "CIVILIAN_SECTOR";
    sectorChoices_.clear();
    for (int id : sectorGraph_.current().neighbors) {
        sectorChoices_.push_back(sectorGraph_.nodes()[id].name);
    }
    pendingQuests_.clear();
}

pugi::xml_node Run::sectorDescription() const {
    // StarMap::GenerateSectorMap 0x5cb6f0 gives the starting sector
    // GetSpecificSector("STANDARD_SPACE"), then renames it CIVILIAN_SECTOR
    // (type, name and short name only): its event counts, rarities, tracks
    // and start event stay STANDARD_SPACE's.
    const bool start = !secretSector_ && sectorName_ == "CIVILIAN_SECTOR" &&
                       !sectorGraph_.nodes().empty() && sectorGraph_.current().level == 0;
    return events_.sector(start ? "STANDARD_SPACE" : sectorName_);
}

void Run::startSectorMusic() {
    // StarMap::GenerateMap0x5d4590 preserves the sector's authored track order.
    std::vector<std::string> tracks;
    for (const auto track : sectorDescription().child("trackList").children("track")) {
        tracks.emplace_back(track.child_value());
    }
    if (tracks.empty()) {
        tracks.emplace_back("civilian");
    }
    audio_.playlist(tracks);
}

void Run::populateSector() {
    locationBeacon_ = graph_.current();
    const bool final = sectorName_ == "FINAL";
    const auto initialQuests = pendingQuests_;
    generationTrace_ = {{"attempts", nlohmann::json::array()}};
    for (int generationAttempt = 0; generationAttempt < TravelGraph::finalGenerationAttempts;
         ++generationAttempt) {
        generationTrace_["attempts"].push_back(nlohmann::json::object());
        auto& trace = generationTrace_["attempts"].back();
        // Harness only: the RNG state before each traced draw.
        const auto traced = [this](nlohmann::json& states) {
            states.push_back(eventRandom_.state());
            return eventRandom_.next();
        };
        // DeleteMap empties StarMap::addedQuests before the new map's AddQuest calls.
        addedQuests_.clear();
        try {
            // GenerateEvents 0x5d2100 uses each archived sector event count, assigns
            // without replacement, and fills spare locations with NEUTRAL.
            const auto definition = sectorDescription();
            graph_.setNebulaMap(sectorType_ == 2);
            auto& used = usedVoyageEvents_;
            used.clear();
            const auto freeze = [this](pugi::xml_node node, RandomStream& random) {
                freezeVoyageEvent(node, random);
            };
            const std::string start =
                final ? "LAST_STAND_START"
                      : definition.child("startEvent").text().as_string("START_BEACON");
            auto startXml =
                materializeEvent(events_.event(start), eventRandom_, advanced_, used, freeze);
            trace["exit_state"] = eventRandom_.state();
            int exit = graph_.chooseExit(eventRandom_, final ? difficulty_ : -1);
            for (int retry = 0; exit < 0 && retry < TravelGraph::finalExitRegenerationAttempts;
                 ++retry) {
                // GenerateMap 0x5d4590 rebuilds geometry after sixteen invalid exits.
                graph_.regenerateGeometry(eventRandom_);
                graph_.setNebulaMap(sectorType_ == 2);
                used.clear();
                startXml =
                    materializeEvent(events_.event(start), eventRandom_, advanced_, used, freeze);
                trace["exit_state"] = eventRandom_.state();
                exit = graph_.chooseExit(eventRandom_, final ? difficulty_ : -1);
            }
            if (exit < 0) {
                throw std::runtime_error("exit_generation_exhausted");
            }
            trace["current"] = graph_.current();
            trace["exit"] = exit;
            trace["geometry"] = nlohmann::json::array();
            for (const auto& beacon : graph_.beacons()) {
                trace["geometry"].push_back({beacon.row, beacon.column, beacon.neighbors});
            }
            std::vector<int> available;
            for (std::size_t index = 0; index < graph_.beacons().size(); ++index) {
                graph_.setEvent(static_cast<int>(index), "", false, false);
                graph_.setStore(static_cast<int>(index), false);
                if (static_cast<int>(index) != graph_.current() &&
                    static_cast<int>(index) != exit) {
                    available.push_back(static_cast<int>(index));
                }
            }
            graph_.setEvent(graph_.current(), start);
            graph_.setEventXml(graph_.current(), std::move(startXml));
            auto prepare = [&](int id) {
                graph_.setEventXml(id,
                                   materializeEvent(events_.event(graph_.beacons()[id].event),
                                                    eventRandom_,
                                                    advanced_,
                                                    used,
                                                    freeze));
                pugi::xml_document document;
                if (!document.load_string(graph_.beacons()[id].eventXml.c_str())) {
                    throw std::runtime_error("invalid_prepared_beacon_event");
                }
                const auto event = document.document_element();
                graph_.setStore(id, static_cast<bool>(event.child("store")));
                graph_.setDistress(id, static_cast<bool>(event.child("distressBeacon")));
            };
            graph_.setEvent(exit, final ? "FEDERATION_BASE" : "FINISH_BEACON", false, true);
            prepare(exit);
            std::vector<std::string> nebulaEvents;
            auto amount = [&](pugi::xml_node entry) {
                const int minimum = entry.attribute("min").as_int();
                const int maximum = entry.attribute("max").as_int(minimum);
                if (minimum < 0 || maximum < minimum || maximum > 100) {
                    throw std::runtime_error("invalid_sector_event_count");
                }
                const float chance = entry.attribute("chance").as_float();
                if (chance > 0 && static_cast<float>(eventRandom_.next() % 100) < chance * 100) {
                    return 0;
                }
                return maximum
                           ? minimum +
                                 static_cast<int>(eventRandom_.next() %
                                                  static_cast<std::uint32_t>(maximum - minimum + 1))
                           : minimum;
            };
            for (auto entry : definition.children("event")) {
                const std::string name = entry.attribute("name").as_string();
                if (name.find("NEBULA") != std::string::npos) {
                    const int count = amount(entry);
                    for (int index = 0; index < count; ++index) {
                        nebulaEvents.push_back(name);
                    }
                }
            }
            // GenerateNebulas 0x5d17e0 packs touching archived image rectangles,
            // marks every enclosed beacon and consumes the requested cloud events.
            mapNebulas_.clear();
            if (!nebulaEvents.empty()) {
                const bool large = nebulaEvents.size() > 5;
                const int variants = large ? 3 : 4;
                auto texture = [&](int variant) -> Texture& {
                    return renderer_.image(std::string("img/map/nebula_") +
                                           (large ? "large_" : "") + std::to_string(variant) +
                                           ".png");
                };
                auto& nebulaTrace = trace["nebula"];
                nebulaTrace = {{"names", nebulaEvents},
                               {"large", large},
                               {"sizes", nlohmann::json::array()},
                               {"draws", nlohmann::json::array()}};
                for (int index = 0; index < variants; ++index) {
                    nebulaTrace["sizes"].push_back({texture(index).w, texture(index).h});
                }
                const auto draw = [&] {
                    return traced(nebulaTrace["draws"]);
                };
                int variant = static_cast<int>(draw() % variants);
                // Every location not yet in a nebula, in location order.
                std::vector<int> cloudFree;
                for (int index = 0; index < static_cast<int>(graph_.beacons().size()); ++index) {
                    if (!graph_.beacons()[static_cast<std::size_t>(index)].nebula) {
                        cloudFree.push_back(index);
                    }
                }
                // Drop random nebula events until more than three free locations
                // remain beyond them (an unsigned comparison in the original).
                while (!nebulaEvents.empty() &&
                       static_cast<unsigned>(cloudFree.size() - nebulaEvents.size()) <= 3U) {
                    const auto choice = draw() % nebulaEvents.size();
                    nebulaEvents.erase(nebulaEvents.begin() + static_cast<std::ptrdiff_t>(choice));
                }
                const int center = cloudFree[draw() % cloudFree.size()];
                int x = graph_.beacons()[center].x - texture(variant).w / 2;
                int y = graph_.beacons()[center].y - texture(variant).h / 2;
                int failed = 0;
                for (int attempt = 0; attempt < 10000; ++attempt) {
                    const auto& image = texture(variant);
                    bool hit = false;
                    for (std::size_t index = 0; index < cloudFree.size();) {
                        const int id = cloudFree[index];
                        const auto beacon = graph_.beacons()[id];
                        if (beacon.x > x + 5 && beacon.x < x + image.w - 5 && beacon.y > y + 5 &&
                            beacon.y < y + image.h - 5) {
                            std::string name = beacon.event;
                            if (name.empty()) {
                                // Each hit builds its name from "NEBULA" (0x5d1e03)
                                // and assigns a drawn event over it (ecx = &name at
                                // 0x5d1e70) only while some remain.
                                name = "NEBULA";
                                if (!nebulaEvents.empty()) {
                                    const auto choice = draw() % nebulaEvents.size();
                                    name = nebulaEvents[choice];
                                    nebulaEvents.erase(nebulaEvents.begin() +
                                                       static_cast<std::ptrdiff_t>(choice));
                                }
                            } else if (beacon.exit) {
                                name = "FINISH_BEACON_NEBULA";
                            }
                            const auto prepared = beacon.eventXml;
                            const bool starting = id == graph_.current();
                            graph_.setEvent(id, name, true, beacon.exit);
                            if (starting) {
                                // The start event survives (StarMap::NewGame
                                // 0x5d5a20 replaces only its text), and
                                // GenerateNebulas sets its environment to 3
                                // and appends GetNebulaEffect 0x5eba80.
                                graph_.setEventXml(id, nebulaStartEvent(prepared));
                            } else {
                                prepare(id);
                            }
                            available.erase(std::remove(available.begin(), available.end(), id),
                                            available.end());
                            cloudFree.erase(cloudFree.begin() + static_cast<std::ptrdiff_t>(index));
                            hit = true;
                        } else {
                            ++index;
                        }
                    }
                    if (hit) {
                        mapNebulas_.push_back({x, y, variant + (large ? 4 : 0)});
                    } else {
                        ++failed;
                    }
                    // The next cloud's draws precede the exhaustion test, even
                    // when the last cloud took every free location.
                    if (failed > 20 || mapNebulas_.empty()) {
                        if (cloudFree.empty()) {
                            // The original divides by the empty free list here.
                            break;
                        }
                        failed = 0;
                        const auto& beacon = graph_.beacons()[cloudFree[draw() % cloudFree.size()]];
                        x = beacon.x - image.w / 2;
                        y = beacon.y - image.h / 2;
                    } else {
                        const auto& previous = mapNebulas_[draw() % mapNebulas_.size()];
                        const auto& parent = texture(previous.variant - (large ? 4 : 0));
                        variant = static_cast<int>(draw() % variants);
                        const auto& next = texture(variant);
                        x = previous.x + static_cast<int>(draw() % (next.w + parent.w)) - next.w;
                        y = previous.y + static_cast<int>(draw() % (next.h + parent.h)) - next.h;
                    }
                    if (nebulaEvents.empty()) {
                        break;
                    }
                }
                if (!nebulaEvents.empty()) {
                    throw std::runtime_error("nebula_generation_exhausted");
                }
            }
            // GenerateEvents 0x5d2100 draws from every location in order
            // (tempLocations), removing each pick; a location that already has
            // an event (start, exit, nebula) is spent without counting. An
            // exhausted pool ends the remaining entries.
            std::vector<int> pool;
            pool.reserve(graph_.beacons().size());
            auto& placementTrace = trace["events"];
            placementTrace = {{"before", nlohmann::json::array()},
                              {"entries", nlohmann::json::array()},
                              {"draws", nlohmann::json::array()}};
            for (int index = 0; index < static_cast<int>(graph_.beacons().size()); ++index) {
                pool.push_back(index);
                placementTrace["before"].push_back(
                    graph_.beacons()[static_cast<std::size_t>(index)].event);
            }
            for (auto entry : definition.children("event")) {
                const std::string name = entry.attribute("name").as_string();
                if (name.find("NEBULA") != std::string::npos) {
                    continue;
                }
                const int count = amount(entry);
                placementTrace["entries"].push_back({name, count});
                for (int placed = 0; placed < count && !pool.empty();) {
                    const auto choice = traced(placementTrace["draws"]) % pool.size();
                    const int beacon = pool[choice];
                    pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(choice));
                    if (!graph_.beacons()[static_cast<std::size_t>(beacon)].event.empty()) {
                        continue;
                    }
                    graph_.setEvent(beacon, name);
                    prepare(beacon);
                    available.erase(std::remove(available.begin(), available.end(), beacon),
                                    available.end());
                    ++placed;
                }
                if (pool.empty()) {
                    break;
                }
            }
            for (int beacon : available) {
                graph_.setEvent(beacon, "NEUTRAL");
                prepare(beacon);
            }
            auto deferred = std::move(pendingQuests_);
            pendingQuests_.clear();
            for (const auto& quest : deferred) {
                auto xml =
                    materializeEvent(events_.event(quest), eventRandom_, advanced_, used, freeze);
                const int id = graph_.addQuest(quest, true, eventRandom_);
                if (id < 0 && sector_ < 7) {
                    pendingQuests_.push_back(quest);
                } else if (id >= 0) {
                    graph_.setEventXml(id, std::move(xml));
                    addedQuests_.emplace_back(quest, id);
                }
            }
            if (final) {
                initializeCampaign();
            }
            return;
        } catch (const std::runtime_error& error) {
            if (!final ||
                std::string_view(error.what()) != "final_boss_path_generation_exhausted" ||
                generationAttempt + 1 == TravelGraph::finalGenerationAttempts) {
                throw;
            }
            // Preserve the authored base and Flagship path constraints rather
            // than accepting an unreachable route on a particular geometry.
            pendingQuests_ = initialQuests;
            graph_.regenerateGeometry(eventRandom_);
        }
    }
}

bool Run::addQuest(const std::string& event, bool nextSector) {
    events_.event(event);
    auto& used = usedVoyageEvents_;
    const auto freeze = [this](pugi::xml_node node, RandomStream& random) {
        freezeVoyageEvent(node, random);
    };
    auto xml = materializeEvent(events_.event(event), eventRandom_, advanced_, used, freeze);
    const int id = nextSector ? -1 : graph_.addQuest(event, false, eventRandom_);
    if (id >= 0) {
        graph_.setEventXml(id, std::move(xml));
        addedQuests_.emplace_back(event, id);
    } else if (sector_ < 7) {
        pendingQuests_.push_back(event);
    }
    return id >= 0;
}

bool Run::canWaitAtBeacon() const {
    return fuel_ == 0 || (campaign_.active && !(enemy_.present && hostile_));
}

void Run::waitForFuel() {
    if (!open_ || defeated_ || hostile_ || event_ || !canWaitAtBeacon() || distressWaitTicks_ > 0 ||
        jumpTicks_ > 0 || arrivalTicks_ > 0) {
        return;
    }
    // The two-second map waiting animation uses unscaled modal UI time.
    graph_.beginWait();
    // AnimationTracker::Update completes only after elapsed > two seconds.
    distressWaitTicks_ = 121;
    panel_.clear();
    mapOpen_ = true;
}

void Run::advanceVoyageUi() {
    if (distressWaitTicks_ <= 0) {
        return;
    }
    --distressWaitTicks_;
    graph_.waitProgress(static_cast<float>(121 - distressWaitTicks_) / 120.f,
                        distressWaitTicks_ > 0 && !campaign_.active,
                        [this](int index) {
                            convertFleetLocation(index);
                        });
    if (distressWaitTicks_ > 0) {
        return;
    }
    nlohmann::json backup;
    checkpoint(backup, false);
    const auto oldFeedback = resourceDeltas_;
    const auto oldFeedbackTicks = resourceDeltaTicks_;
    try {
        if (campaign_.active) {
            campaignTurn();
        }
        auto currentBeacon = graph_.beacons()[graph_.current()];
        pugi::xml_document oldLocation;
        if (!currentBeacon.eventXml.empty() &&
            !oldLocation.load_string(currentBeacon.eventXml.c_str())) {
            throw std::runtime_error("invalid_wait_location_event");
        }
        auto oldEvent = oldLocation.document_element();
        if (!oldEvent) {
            oldEvent = oldLocation.append_child("event");
        }
        if (campaign_.active && !currentBeacon.fleet) {
            auto beacon = oldEvent.attribute("_nativeBeacon");
            if (!beacon) {
                beacon = oldEvent.append_attribute("_nativeBeacon");
            }
            beacon.set_value(currentBeacon.exit);
            events_.clearLocationEvent(oldEvent, true, !currentBeacon.savedShip.is_null());
            std::ostringstream cleared;
            oldEvent.print(cleared, "", pugi::format_raw);
            graph_.setEventXml(graph_.current(), cleared.str());
            // A non-beacon force clear destroys its cached Store. Ordinary
            // ClearLocation separately retains an existing Store pointer.
            if (!currentBeacon.exit) {
                stores_.erase(graph_.current());
                graph_.setStore(graph_.current(), false);
                graph_.setDistress(graph_.current(), false);
            }
            currentBeacon = graph_.beacons()[graph_.current()];
        }
        if (fuel_ > 0) {
            waitSystems();
            clearLocation();
            mapOpen_ = false;
            if (!campaignArrival()) {
                restoredEvents_.emplace_back();
                if (!restoredEvents_.back().load_string(
                        graph_.beacons()[graph_.current()].eventXml.c_str())) {
                    throw std::runtime_error("invalid_cleared_boss_wait_event");
                }
                const auto active = restoredEvents_.back().document_element();
                prepareBeaconImages(active);
                enterEvent(active, true);
            }
            registerSpaceDrones(0, true);
            reconcilePower();
            return;
        }
        const std::string oldEnvironment =
            oldEvent.child("environment").attribute("type").as_string();
        const std::string oldFleet = currentBeacon.fleet ? "rebel" : oldEvent.child_value("fleet");
        const bool fleet = oldFleet == "rebel";
        // UpdateDangerZone5d0f30 calls SetupNoFuel(-1) on completed waiting;
        // SetupNoFuel5d0250 draws and reseeds before either base event.
        fuelEventSeed_ = eventRandom_.next();
        eventRandom_ = RandomStream(fuelEventSeed_);
        const auto name = fleet       ? (advanced_ ? "NO_FUEL_FLEET_DLC" : "NO_FUEL_FLEET")
                          : distress_ ? "NO_FUEL_DISTRESS"
                                      : "NO_FUEL";
        std::string escape;
        if (oldEnvironment == "asteroid") {
            escape = "FUEL_ESCAPE_ASTEROIDS";
        } else if (oldEnvironment == "sun") {
            escape = "FUEL_ESCAPE_SUN";
        } else if (oldEnvironment == "pulsar") {
            escape = "FUEL_ESCAPE_PULSAR";
        } else if (!fleet && (oldEnvironment == "PDS" || oldEnvironment == "pds")) {
            escape = "FUEL_ESCAPE_PDS";
        } else if (!fleet && (oldEnvironment == "storm" || oldEnvironment == "ion_storm")) {
            escape = "FUEL_ESCAPE_STORM";
        }
        auto& used = usedVoyageEvents_;
        const auto freeze = [this](pugi::xml_node node, RandomStream& random) {
            freezeVoyageEvent(node, random);
        };
        // SetupNoFuel 0x5d0250 constructs the hazard escape base event first,
        // then the complete waiting result. Continue never rerolls its tree.
        std::string escapeXml;
        if (!escape.empty()) {
            // SetupNoFuel constructs its escape blueprint at worldLevel zero.
            const int savedSector = sector_;
            sector_ = 1;
            try {
                escapeXml =
                    materializeEvent(events_.event(escape), eventRandom_, advanced_, used, freeze);
            } catch (...) {
                sector_ = savedSector;
                throw;
            }
            sector_ = savedSector;
        }
        const auto resultXml =
            materializeEvent(events_.event(name), eventRandom_, advanced_, used, freeze);
        restoredEvents_.emplace_back();
        if (!restoredEvents_.back().load_string(resultXml.c_str())) {
            throw std::runtime_error("invalid_prepared_wait_event");
        }
        auto result = restoredEvents_.back().document_element();
        // Preserve location descriptors; environmental escape and its result
        // are separate generated events in SetupNoFuel 0x5d0250.
        result.remove_child("fleet");
        if (!oldFleet.empty()) {
            result.append_child("fleet").text().set(oldFleet.c_str());
        }
        if (!fleet) {
            result.remove_child("environment");
            const bool keepNebula = oldEnvironment == "nebula" || oldEnvironment == "storm" ||
                                    oldEnvironment == "ion_storm";
            result.append_child("environment").append_attribute("type") =
                keepNebula ? "nebula" : "normal";
            if (keepNebula) {
                auto status = result.append_child("status");
                status.append_attribute("type") = "limit";
                status.append_attribute("system") = "sensors";
                status.append_attribute("amount") = 0;
                status.append_attribute("target") = "all";
            }
        }
        auto active = result;
        if (!escapeXml.empty()) {
            restoredEvents_.emplace_back();
            if (!restoredEvents_.back().load_string(escapeXml.c_str())) {
                throw std::runtime_error("invalid_prepared_escape_event");
            }
            active = restoredEvents_.back().document_element();
            active.remove_child("environment");
            if (result.child("environment")) {
                active.append_copy(result.child("environment"));
            }
            while (active.remove_child("choice")) {
            }
            auto choice = active.append_child("choice");
            choice.append_child("text").text().set(texts_.get("continue", locale_).c_str());
            choice.append_copy(result);
        }
        waitSystems();
        clearLocation();
        mapOpen_ = false;
        const std::string activeName = escapeXml.empty() ? name : escape;
        const bool activeNebula =
            std::string_view(active.child("environment").attribute("type").as_string()) == "nebula";
        graph_.setEvent(graph_.current(), activeName, activeNebula, currentBeacon.exit);
        prepareBeaconImages(active);
        std::ostringstream activeXml;
        active.print(activeXml, "", pugi::format_raw);
        graph_.setEventXml(graph_.current(), activeXml.str());
        // The cached Store object remains attached to this physical beacon.
        graph_.setStore(graph_.current(), currentBeacon.store);
        // CreateLocation checks the boss-location override after SetupNoFuel
        // has constructed and cached its complete result/escape tree.
        if (!campaignArrival()) {
            enterEvent(active, true);
        }
        registerSpaceDrones(0, true);
        // UpdateDangerZone constructs SetupNoFuel before consuming pursuit
        // delay and scanning the final danger circle for fleet conversions.
        if (!campaign_.active) {
            graph_.wait([this](int index) {
                convertFleetLocation(index);
            });
        }
        ++distressWaits_;
        // SetupNoFuel 0x5d0250 sets waitedLast and lastEscapeEvent.
        waitedLast_ = true;
        lastEscapeEvent_ = escape;
        reconcilePower();
    } catch (...) {
        checkpoint(backup, true);
        catalog_.setUsedCrewNames(crewNameHistory_);
        graph_.waitProgress(0.f, false);
        resourceDeltas_ = oldFeedback;
        resourceDeltaTicks_ = oldFeedbackTicks;
        throw;
    }
}

void Run::reindexCrewReferences(int owner, int removed) {
    auto& pending = owner == 0 ? clonePending_ : enemy_.clonePending;
    if (pending == removed) {
        pending = -1;
    } else if (pending > removed) {
        --pending;
    }
    if (mindTargetOwner_ == owner && mindTarget_ == removed) {
        releasePlayerMind();
    } else if (mindTargetOwner_ == owner && mindTarget_ > removed) {
        --mindTarget_;
    }
    if (enemy_.mindTargetOwner == owner && enemy_.mindTarget == removed) {
        releaseEnemyMind();
    } else if (enemy_.mindTargetOwner == owner && enemy_.mindTarget > removed) {
        --enemy_.mindTarget;
    }
    remapMindQueues(owner, [removed](int index) {
        return index == removed ? -1 : index > removed ? index - 1 : index;
    });
    const auto update = [&](auto& actors) {
        for (auto& actor : actors) {
            if (actor.attackTargetDrone || actor.attackTargetOwner != owner) {
                continue;
            }
            if (actor.attackTargetIndex == removed) {
                actor.attackTargetOwner = actor.attackTargetIndex = -1;
                actor.attackTargetDrone = false;
            } else if (actor.attackTargetIndex > removed) {
                --actor.attackTargetIndex;
            }
        }
    };
    update(crew_);
    update(enemy_.crew);
    update(voyageBoarders_);
    // Drone actors use the same stable crew identity when crew melee targets one.
    update(combatDrones_);
    update(enemy_.drones);
}

void Run::reindexDroneReferences(int owner, int changed, bool removed) {
    const auto update = [&](auto& actors) {
        for (auto& actor : actors) {
            if (!actor.attackTargetDrone || actor.attackTargetOwner != owner ||
                actor.attackTargetDroneId != 0) {
                continue;
            }
            if (actor.attackTargetIndex == changed) {
                actor.attackTargetOwner = actor.attackTargetIndex = -1;
                actor.attackTargetDrone = false;
                actor.attackTargetDroneId = 0;
            } else if (removed && actor.attackTargetIndex > changed) {
                --actor.attackTargetIndex;
            }
        }
    };
    update(crew_);
    update(enemy_.crew);
    update(voyageBoarders_);
    update(combatDrones_);
    update(enemy_.drones);
}

void Run::refreshCrewNames() const {
    std::vector<std::string> names;
    for (std::size_t index = 0; index < crew_.size(); ++index) {
        const auto& member = crew_[index];
        if ((!member.dead && !member.dying) || member.cloneReady) {
            names.push_back(member.appearance.fullName);
        }
    }
    catalog_.setUsedCrewNames(names);
}

void Run::startSecretSector() {
    if (!pendingSecretSector_ || secretSector_ || event_ || jumpTicks_ > 0 || defeated_) {
        return;
    }
    nextSector(-1);
}

float Run::targetBoxTime() const {
    // AnimationTracker current_time of the looping one-second targetBoxTimer.
    if (!targetBoxRunning_) {
        return 0.f;
    }
    return (static_cast<float>((animation_ - targetBoxTick_) % 60) + uiFraction_) / 60.f;
}

int Run::sectorPotentialAt(float x, float y) const {
    // StarMap::MouseMove 0x5d6590: the nearest sector whose Point::Distance
    // 0x585e90 (truncated) is under 10, overridden by a sector-name hit box.
    const auto& nodes = sectorGraph_.nodes();
    int best = -1;
    int bestDistance = 10;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const float dx = 415.f + nodes[index].level * 66.f - x;
        const float dy = 300.f + nodes[index].row - y;
        const int distance = static_cast<int>(std::sqrt(dx * dx + dy * dy));
        if (distance < bestDistance) {
            best = static_cast<int>(index);
            bestDistance = distance;
        }
    }
    const auto& choices = sectorGraph_.current().neighbors;
    for (std::size_t choice = 0; choice < choices.size(); ++choice) {
        if (sectorNameRect(renderer_, nodes[choices[choice]], choice, locale_ == "zh-Hans")
                .contains(x, y)) {
            best = choices[choice];
        }
    }
    return best;
}

void Run::nextSector(int choice) {
    const bool hidden = choice == -1 && pendingSecretSector_ && !secretSector_;
    if (!hidden && (!docked() || !graph_.beacons().at(graph_.current()).exit || choice < 0 ||
                    choice >= static_cast<int>(sectorGraph_.current().neighbors.size()) ||
                    sector_ >= 8 || fuel_ <= 0 || !canFly())) {
        return;
    }
    // Prepare the destination graph before charging fuel or clearing encounter state.
    auto nextTree = sectorGraph_;
    if (!hidden && !nextTree.select(choice)) {
        return;
    }
    auto destination = nextTree.current();
    if (hidden) {
        // StartSecretSector 0x5c7a90 switches currentSector to the prebuilt
        // hidden sector. It does not increment its level or advance the tree.
        destination.name = "CRYSTAL_HOME";
        destination.type = 0;
    }
    events_.sector(destination.name);
    const auto previousGraph = graph_;
    const auto previousCampaign = campaign_;
    const auto previousRandom = eventRandom_;
    const auto previousClouds = mapNebulas_;
    const auto previousQuests = pendingQuests_;
    const auto previousAddedQuests = addedQuests_;
    const auto previousUsed = usedVoyageEvents_;
    const auto previousName = sectorName_;
    const int previousType = sectorType_;
    try {
        if (!hidden) {
            ++sector_;
        }
        sectorName_ = destination.name;
        sectorType_ = destination.type;
        graph_.generate(eventRandom_.next(), eventRandom_);
        populateSector();
    } catch (...) {
        if (!hidden) {
            --sector_;
        }
        graph_ = previousGraph;
        campaign_ = previousCampaign;
        eventRandom_ = previousRandom;
        mapNebulas_ = previousClouds;
        pendingQuests_ = previousQuests;
        addedQuests_ = previousAddedQuests;
        usedVoyageEvents_ = previousUsed;
        sectorName_ = previousName;
        sectorType_ = previousType;
        throw;
    }
    auto nextGraph = graph_;
    auto nextCampaign = campaign_;
    auto nextRandom = eventRandom_;
    auto nextClouds = mapNebulas_;
    auto nextQuests = pendingQuests_;
    auto nextAddedQuests = addedQuests_;
    auto nextUsed = usedVoyageEvents_;
    if (!hidden) {
        --sector_;
    }
    graph_ = previousGraph;
    campaign_ = previousCampaign;
    eventRandom_ = previousRandom;
    mapNebulas_ = previousClouds;
    pendingQuests_ = previousQuests;
    addedQuests_ = previousAddedQuests;
    usedVoyageEvents_ = previousUsed;
    sectorName_ = previousName;
    sectorType_ = previousType;
    const auto previousResources = resourceAmounts();
    retireCrewDrone(overflowFactory_.drone);
    overflowEquipment_.clear();
    overflowFactory_ = {};
    overflowAugment_.clear();
    // Preserve the departure environment until CheckForNewLocation completes.
    for (auto& sys : systems_) {
        sys.powerLimit = 8;
        sys.powerLoss = 0;
        sys.powerDivide = 1;
    }
    reactorPowerCap_ = 64;
    reactorPowerLoss_ = 0;
    reactorPowerDivide_ = 1;
    // WorldManager::ClearLocation 0x5ef130 kills the enemy ship's entire
    // onboard list with noClone=true, including an abandoned friendly party.
    for (std::size_t i = 0; i < crew_.size(); ++i) {
        auto& member = crew_[i];
        if (!member.intruder) {
            continue;
        }
        if (member.health > 0) {
            killCrew(member, true);
        }
        std::erase(cloneQueue_, int(i));
        member.controlled = false;
        member.healthBoost = 0;
    }
    // The sector jump's JumpLeave also runs MindSystem::Jump 0x60ae20.
    if (auto* mind = system(14)) {
        releasePlayerMind();
        setSystemLock(*mind, 0);
    }
    resetAdvanced();
    stopAbility(10);
    stopAbility(12);
    reconcilePower();
    battery_.cooldown = 0.f;
    const bool recover =
        std::find(ship_.augments.begin(), ship_.augments.end(), "DRONE_RECOVERY") !=
        ship_.augments.end();
    for (auto& drone : combatDrones_) {
        const auto& definition = catalog_.equipment(drone.blueprint);
        const bool spaceDrone = definition.type == "COMBAT" || definition.type == "DEFENSE" ||
                                definition.type == "SHIP_REPAIR" || definition.type == "SHIELD";
        if (!spaceDrone && definition.type != "BOARDER") {
            continue;
        }
        // DroneSystem::Jump 0x611ae0: CanBeRecovered excludes boarder pods and
        // Destroyed (bDead) bodies are not refunded; DePowerDrone removes only
        // the drone's raw bars, keeping the bonus credit.
        if (recover && drone.deployed && drone.health > 0 && definition.type != "BOARDER") {
            ++parts_;
        }
        if (drone.powered) {
            if (auto* drones = system(4)) {
                (void)depowerDrone(drone, *drones, 0);
            }
        }
        reindexDroneReferences(0, static_cast<int>(&drone - combatDrones_.data()));
        resetCrewDroneDeployment(drone);
    }
    const bool preignited =
        std::find(ship_.augments.begin(), ship_.augments.end(), "WEAPON_PREIGNITE") !=
        ship_.augments.end();
    for (std::size_t index = 0; index < combatWeapons_.size(); ++index) {
        auto& weapon = combatWeapons_[index];
        weapon.clearTargets();
        weapon.boostLevel = 0;
        weapon.goalChargeLevel = 0;
        weapon.charge = 0;
        weapon.chargeLevel = 0;
        if (preignited && weaponPower_[index]) {
            const auto& definition = catalog_.equipment(ship_.weapons[index]);
            weapon.cooldown = definition.cooldown;
            weapon.chargeLevel = 1;
            weapon.charge = definition.chargeLevels > 1 ? 0.f : weapon.cooldown;
        }
    }
    for (auto& sys : systems_) {
        if (sys.definition.id == 11) {
            sys.artillery = {};
        }
    }
    superShields_ = std::find(ship_.augments.begin(), ship_.augments.end(), "ENERGY_SHIELD") !=
                            ship_.augments.end()
                        ? 5
                        : 0;
    if (!hidden) {
        ++sector_;
    }
    sectorGraph_ = std::move(nextTree);
    progressionRun_.tutorial = tutorialOwned_;
    if (!hidden) {
        progression_.sector(sector_ - 1, progressionRun_);
    }
    if (!hidden && sector_ == 5 && ProgressionLedger::family(ship_.id) == 0) {
        progression_.unlock(3, 0, progressionRun_);
    }
    sectorType_ = destination.type;
    sectorName_ = destination.name;
    sectorChoices_.clear();
    for (int id : sectorGraph_.current().neighbors) {
        sectorChoices_.push_back(sectorGraph_.nodes()[id].name);
    }
    // CommandGui's secret dummy-sector dispatch still calls JumpLeave
    // 0x491b70, which spends one fuel and clamps at zero. The authored
    // ROCK_CRYSTAL_BEACON reward supplies that departure fuel.
    fuel_ = std::max(0, fuel_ - 1);
    graph_ = std::move(nextGraph);
    campaign_ = std::move(nextCampaign);
    eventRandom_ = nextRandom;
    mapNebulas_ = std::move(nextClouds);
    pendingQuests_ = std::move(nextQuests);
    addedQuests_ = std::move(nextAddedQuests);
    // GenerateMap 0x5d4590 clears waitedLast and bMapRevealed.
    waitedLast_ = false;
    mapRevealed_ = false;
    usedVoyageEvents_ = std::move(nextUsed);
    stores_.clear();
    detachCrewDroneShip(1);
    clearSpaceDroneRegistry();
    enemy_ = {};
    registerSpaceDrones(0, true);
    enemyHazards_.clear();
    projectiles_.clear();
    beamHitAt_ = {};
    beamKilledCrew_ = {};
    eventId_ = "NOTHING";
    encounter_.clear();
    encounterAutoBlueprint_.clear();
    ftlCharge_ = 0;
    arrivalTicks_ = 0;
    jumpTicks_ = 120;
    // Sector jumps also leave through ShipManager::JumpLeave 0x491b70,
    // which plays jumpLeave with the fade animation.
    audio_.sound("jumpLeave");
    previousJump_ = 0;
    jumpDestination_ = graph_.current();
    revisiting_ = false;
    panel_.clear();
    mapOpen_ = false;
    openingEvent_ = false;
    aiming_ = -1;
    distressWaitTicks_ = 0;
    noteResourceChanges(previousResources);
    distress_ = false;
    distressWaits_ = 0;
    secretSector_ = hidden;
    pendingSecretSector_ = false;
    if (hidden) {
        // Award at authoritative entry, after generated content is installed.
        progression_.secretSectorEntered(progressionRun_);
    }
    startSectorMusic();
    addScore(scoreBeacons_, 1);
    for (auto& weapon : combatWeapons_) {
        weapon.clearTargets();
    }
}

Run::PauseMenuLayout Run::pauseMenuLayout() const {
    auto& image = renderer_.image("img/optionsUI/escape_main.png");
    float width = static_cast<float>(image.w);
    for (const auto& key : {"escape_continue",
                            "escape_mainmenu",
                            "escape_hangar",
                            "escape_reset",
                            "escape_options",
                            "escape_controls",
                            "escape_save"}) {
        const auto label = texts_.get(key, locale_);
        width = std::max(
            width,
            std::round(renderer_.measure(localizedFont(62, label), label, textScale(62))) + 40);
    }
    const auto statusWidth = [&](const char* imageName,
                                 const char* valueKey,
                                 const char* labelKey) {
        const auto value = texts_.get(valueKey, locale_);
        const auto label = texts_.get(labelKey, locale_);
        return std::max({renderer_.image(imageName).w,
                         static_cast<int>(std::round(
                             renderer_.measure(localizedFont(62, value), value, textScale(62)))) +
                             30,
                         static_cast<int>(std::round(
                             renderer_.measure(localizedFont(13, label), label, textScale(13)))) +
                             34});
    };
    PauseMenuLayout result;
    result.difficultyWidth = statusWidth("img/optionsUI/info_difficulty.png",
                                         difficulty_ == 0   ? "easy_button"
                                         : difficulty_ == 1 ? "normal_button"
                                                            : "hard_button",
                                         "menu_status_difficulty");
    result.dlcWidth = statusWidth("img/optionsUI/info_dlc.png",
                                  advanced_ ? "advanced_on_button" : "advanced_off_button",
                                  "menu_status_ae_content");
    const int base = renderer_.image("img/optionsUI/info_achievements.png").w;
    result.achievementExtra = result.difficultyWidth + result.dlcWidth + 3 - base;
    const int remainder = result.achievementExtra - 10 * (result.achievementExtra / 10);
    if (remainder != 0) {
        const int added = 10 - remainder;
        result.achievementExtra += added;
        result.difficultyWidth += added / 2;
        result.dlcWidth += added - added / 2;
    }
    result.achievementWidth = base + result.achievementExtra;
    result.menu = {static_cast<float>(660 -
                                      (result.achievementWidth + static_cast<int>(width) + 38) / 2 +
                                      (tutorialOwned_ ? 150 : 0)),
                   static_cast<float>(360 - image.h / 2),
                   width,
                   static_cast<float>(image.h)};
    return result;
}

Rect Run::pauseMenuRect() const {
    return pauseMenuLayout().menu;
}

void Run::renderPauseMenuStatus() {
    // MenuScreen4feaf0: all backgrounds, labels/values, then achievements and InfoBox.
    if (tutorialOwned_) {
        return;
    }
    const auto layout = pauseMenuLayout();
    const float x = layout.menu.x + layout.menu.w + 38;
    const float y = layout.menu.y + 30;
    const auto statusImage = [&](const char* name, float px, int width) {
        renderer_.draw(renderer_.image(name), {px, y, static_cast<float>(width), 72});
    };
    statusImage("img/optionsUI/info_difficulty.png", x, layout.difficultyWidth);
    statusImage("img/optionsUI/info_dlc.png", x + layout.difficultyWidth + 3, layout.dlcWidth);
    auto& achievementFrame = renderer_.image("img/optionsUI/info_achievements.png");
    const float height = static_cast<float>(achievementFrame.h);
    renderer_.sprite(achievementFrame, {x, y + 74, 132, height}, {0, 0, 132, height});
    float tileX = x + 132;
    for (int tiled = 0; tiled < layout.achievementExtra + 10; tiled += 10) {
        renderer_.sprite(achievementFrame, {tileX, y + 74, 10, height}, {132, 0, 10, height});
        tileX += 10;
    }
    const float tail = static_cast<float>(achievementFrame.w - 142);
    renderer_.sprite(achievementFrame, {tileX, y + 74, tail, height}, {142, 0, tail, height});
    const float difficultyX = x + (layout.difficultyWidth + 1) / 2;
    const float dlcX = x + layout.difficultyWidth + (layout.dlcWidth + 1) / 2 + 3;
    text(texts_.get("menu_status_difficulty", locale_), difficultyX, y + 16, 13, true);
    text(texts_.get("menu_status_ae_content", locale_), dlcX, y + 16, 13, true);
    text(texts_.get("menu_status_achievements", locale_),
         x + (layout.achievementWidth + 1) / 2,
         y + 90,
         13,
         true);
    text(texts_.get(difficulty_ == 0   ? "easy_button"
                    : difficulty_ == 1 ? "normal_button"
                                       : "hard_button",
                    locale_),
         difficultyX,
         y + 40,
         62,
         true,
         {25, 49, 51, 255});
    text(texts_.get(advanced_ ? "advanced_on_button" : "advanced_off_button", locale_),
         dlcX,
         y + 40,
         62,
         true,
         {25, 49, 51, 255});
    const int family = ProgressionLedger::family(ship_.id);
    if (family < 0) {
        return;
    }
    const std::string base = BlueprintCatalog::families[family];
    auto document = xmlResource(archive_, "data/achievements.xml");
    std::vector<std::string> ids;
    for (const auto row : document.child("FTL").children("achievement")) {
        if (std::string_view(row.child("ship").child_value()) == base && ids.size() < 3) {
            ids.emplace_back(row.attribute("id").as_string());
        }
    }
    ids.push_back(base + "_VICTORY");
    if (family != 0 && family != 3 && family != 9) {
        ids.push_back(base + "_QUEST");
    }
    float iconX = x + (layout.achievementExtra + 3) / 6 + 22;
    float iconY = y + 119;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto* award = progression_.achievement(ids[index]);
        const auto badge = index < 3 ? std::to_string(index + 1) : index == 3 ? "V" : "Q";
        AchievementDrawing drawing;
        drawing.miniOn = "img/achievements/S_" + badge + "_on.png";
        drawing.miniOff = "img/achievements/S_" + badge + "_off.png";
        drawing.dimension = index < 3 ? 64 : 32;
        drawing.unlocked = award && award->unlocked;
        drawing.fresh = award && award->fresh;
        drawing.victory = index == 3;
        drawing.twoLayouts = family >= 8;
        if (award) {
            drawing.layouts = award->layouts;
        }
        drawing.image =
            index < 3 ? "img/achievements/" + catalog_.ship(family, 0)->image + "_" + badge + ".png"
            : drawing.unlocked ? drawing.miniOn
                               : drawing.miniOff;
        const bool selected = panel_ == "pause" && mouseX_ > iconX &&
                              mouseX_ < iconX + drawing.dimension && mouseY_ > iconY &&
                              mouseY_ < iconY + drawing.dimension;
        renderer_.achievement(drawing,
                              {iconX, iconY},
                              index < 3  ? int(selected)
                              : selected ? 2
                                         : 3,
                              texts_.get("new_achievement", locale_),
                              locale_ == "zh-Hans");
        if (index < 2) {
            iconX += (layout.achievementExtra + 1) / 3 + 71;
        } else if (index == 2) {
            iconX = x + 245 + layout.achievementExtra;
            iconY += 34;
        } else {
            iconY -= 34;
        }
    }
}

void Run::renderPauseTooltip() {
    // MenuScreen4ffb50 queues this instant tooltip; CApp403070 renders it
    // after the global achievement popup, immediately before MouseControl.
    const auto layout = pauseMenuLayout();
    if (tutorialOwned_) {
        if (Rect{layout.menu.x + 17, layout.menu.y + 254, layout.menu.w - 34, 24}.contains(
                mouseX_, mouseY_)) {
            renderer_.tooltip({},
                              texts_.get("no_save_tutorial", locale_),
                              {mouseX_, mouseY_},
                              0,
                              locale_ == "zh-Hans");
        }
        return;
    }
    const int family = ProgressionLedger::family(ship_.id);
    if (family < 0) {
        return;
    }
    const std::string base = BlueprintCatalog::families[family];
    auto document = xmlResource(archive_, "data/achievements.xml");
    std::vector<pugi::xml_node> rows;
    for (const auto row : document.child("FTL").children("achievement")) {
        if (std::string_view(row.child("ship").child_value()) == base && rows.size() < 3) {
            rows.push_back(row);
        }
    }
    const float statusX = layout.menu.x + layout.menu.w + 38;
    float x = statusX + (layout.achievementExtra + 3) / 6 + 22;
    float y = layout.menu.y + 149;
    const std::size_t count = family == 0 || family == 3 || family == 9 ? 4 : 5;
    for (std::size_t index = 0; index < count; ++index) {
        const int dimension = index < 3 ? 64 : 32;
        if (mouseX_ > x && mouseX_ < x + dimension && mouseY_ > y && mouseY_ < y + dimension) {
            const auto localized = [&](pugi::xml_node node) {
                return node.attribute("id") ? texts_.get(node.attribute("id").as_string(), locale_)
                                            : std::string(node.child_value());
            };
            const auto replace =
                [](std::string& value, const std::string& token, const std::string& text) {
                    std::size_t offset = 0;
                    while ((offset = value.find(token, offset)) != std::string::npos) {
                        value.replace(offset, token.size(), text);
                        offset += text.size();
                    }
                };
            const auto id = index < 3 ? std::string(rows.at(index).attribute("id").as_string())
                                      : base + (index == 3 ? "_VICTORY" : "_QUEST");
            const auto* award = progression_.achievement(id);
            std::string title;
            std::string body;
            if (index < 3) {
                title = localized(rows.at(index).child("name"));
                body = localized(rows.at(index).child("desc"));
                if (rows.at(index).child("multiDifficulty").text().as_int() != 0 && award &&
                    award->unlocked && award->difficulty >= 0) {
                    constexpr std::array<const char*, 3> keys{"ach_easy", "ach_normal", "ach_hard"};
                    body +=
                        " \n\n" +
                        texts_.get(keys.at(static_cast<std::size_t>(award->difficulty)), locale_);
                }
            } else {
                title = texts_.get(index == 3 ? "victory_ach_title" : "quest_ach_title", locale_);
                body = texts_.get(index == 3 ? "victory_ach" : "quest_ach", locale_);
                const auto& shipClass = catalog_.ship(family, 0)->shipClass;
                const auto name =
                    shipClass.reference ? texts_.get(shipClass.value, locale_) : shipClass.value;
                replace(title, "\\1", name);
                replace(body, "\\1", name);
                if (index == 3 && award) {
                    constexpr std::array<const char*, 3> difficulty{"easy", "normal", "hard"};
                    std::string achieved;
                    for (std::size_t type = 0; type < award->layouts.size(); ++type) {
                        if (award->layouts[type] < 0) {
                            continue;
                        }
                        auto line = texts_.get("achieved_with", locale_);
                        replace(line,
                                "\\1",
                                texts_.get("type_" + std::string(1, char('a' + type)), locale_));
                        replace(line,
                                "\\2",
                                texts_.get(
                                    difficulty.at(static_cast<std::size_t>(award->layouts[type])),
                                    locale_));
                        achieved += (achieved.empty() ? "" : "\n") + line;
                    }
                    if (!achieved.empty()) {
                        body += " \n\n" + achieved;
                    }
                }
            }
            renderer_.tooltip(title, body, {mouseX_, mouseY_}, 350, locale_ == "zh-Hans");
            return;
        }
        if (index < 2) {
            x += (layout.achievementExtra + 1) / 3 + 71;
        } else if (index == 2) {
            x = statusX + 245 + layout.achievementExtra;
            y += 34;
        } else {
            y -= 34;
        }
    }
}

std::vector<Run::VoyageButton> Run::voyageButtons() const {
    std::vector<VoyageButton> result;
    if (panel_ == "pause") {
        result = {{"pause:continue", texts_.get("escape_continue", locale_)},
                  {"pause:main_menu", texts_.get("escape_mainmenu", locale_)},
                  {"pause:hangar", texts_.get("escape_hangar", locale_)},
                  {"pause:restart", texts_.get("escape_reset", locale_)},
                  {"pause:options", texts_.get("escape_options", locale_)},
                  {"pause:controls", texts_.get("escape_controls", locale_)},
                  {"pause:save_quit", texts_.get("escape_save", locale_), !tutorialOwned_}};
    } else if (panel_.starts_with("pause_confirm:")) {
        result = {{"pause:yes", texts_.get("confirm_yes", locale_)},
                  {"pause:no", texts_.get("confirm_no", locale_)}};
    } else if (panel_ == "ship") {
        result = {
            {"panel:abilities", "Advanced systems", hostile_ && enemy_.present},
            {"panel:upgrades", "Ship upgrades", docked()},
            {"panel:equipment", "Equipment and cargo", docked()},
            {"panel:crew", "Crew manifest", docked()},
            {"panel:store", "Visit store", docked() && graph_.beacons()[graph_.current()].store},
            {"panel:sector",
             "Choose next sector",
             docked() && graph_.beacons()[graph_.current()].exit && sector_ < 8},
            {"distress:toggle",
             distress_ ? "Disable distress beacon" : "Enable distress beacon",
             docked() && fuel_ == 0},
            {"distress:wait", "Wait for assistance", docked() && canWaitAtBeacon()},
            {"save_menu", "Save and return to main menu"}};
    } else if (panel_ == "abilities") {
        if (system(15)) {
            for (const auto& sys : enemy_.systems) {
                result.push_back({"hack:" + std::to_string(sys.definition.id),
                                  "Hack " + sys.definition.name,
                                  !hacking_.active && hacking_.cooldown <= 0});
            }
        }
        if (system(14)) {
            for (std::size_t i = 0; i < enemy_.crew.size(); ++i) {
                result.push_back(
                    {"mind:" + std::to_string(i),
                     "Mind control " + enemy_.crew[i].appearance.name,
                     enemy_.crew[i].health > 0 && !mind_.active && mind_.cooldown <= 0});
            }
        }
    } else if (panel_ == "upgrades") {
        for (const auto& sys : systems_) {
            const auto& bp = catalog_.system(sys.definition.name);
            const int level = sys.definition.power + pendingUpgrade(sys.definition.id);
            const int maximum = systemMaximum(sys);
            const int cost =
                level > 0 && level < maximum && std::size_t(level - 1) < bp.costs.size()
                    ? bp.costs[level - 1]
                    : -1;
            result.push_back({"upgrade:" + std::to_string(sys.definition.id),
                              sys.definition.name + " " + std::to_string(level) + " -> " +
                                  std::to_string(level + 1) +
                                  (cost < 0 ? " (MAX)" : " / " + std::to_string(cost) + " scrap"),
                              docked() && cost >= 0 && scrap_ >= cost});
        }
        const int reactor = ship_.reactor + pendingUpgrade(-1);
        const int cost = reactorUpgradeCost(reactor);
        result.push_back(
            {"upgrade:reactor",
             "Reactor " + std::to_string(reactor) + " / " + std::to_string(cost) + " scrap",
             docked() && reactor < std::min(25, devReactorLimit_) && scrap_ >= cost});
        result.push_back({"upgrade:undo", "Undo upgrades", !upgradePurchases_.empty()});
        result.push_back({"upgrade:accept", "Accept upgrades", !tutorialBlocksPanelAccept()});
        result.push_back({"upgrade:cancel", "Cancel upgrades", !tutorialBlocksPanelAccept()});
    } else if (panel_ == "equipment") {
        for (const auto& kind : {"weapon", "drone", "augment"}) {
            const auto& items = std::string_view(kind) == "weapon"  ? ship_.weapons
                                : std::string_view(kind) == "drone" ? ship_.drones
                                                                    : ship_.augments;
            for (std::size_t i = 0; i < items.size(); ++i) {
                result.push_back(
                    {"unequip:" + std::string(kind) + ":" + std::to_string(i),
                     "Move to cargo: " + items[i],
                     docked() && std::string_view(kind) != "augment" && cargo_.size() < 4});
            }
        }
        for (std::size_t i = 0; i < cargo_.size(); ++i) {
            result.push_back(
                {"equip:" + std::to_string(i),
                 "Equip: " + cargo_[i],
                 docked() &&
                     (catalog_.equipment(cargo_[i]).kind == "weapon"
                          ? system(3) && ship_.weapons.size() < std::size_t(ship_.weaponSlots)
                      : catalog_.equipment(cargo_[i]).kind == "drone"
                          ? system(4) && ship_.drones.size() < std::size_t(ship_.droneSlots)
                          : inventoryCanHold(cargo_[i]))});
        }
    } else if (panel_ == "store") {
        return storeButtons();
    } else if (panel_ == "crew") {
        for (std::size_t i = 0; i < crew_.size(); ++i) {
            result.push_back(
                {"dismiss:" + std::to_string(i),
                 "Dismiss " + crew_[i].appearance.name + " (" + crew_[i].appearance.species + ")",
                 (docked() || enemy_.rewardCrewPending ||
                  crewRoster().size() > std::size_t(devCrewLimit_)) &&
                     crewRoster().size() > 1 &&
                     ((!crew_[i].dead && !crew_[i].dying) || crew_[i].cloneReady)});
            result.push_back({"rename:" + std::to_string(i),
                              "Rename " + crew_[i].appearance.name,
                              (docked() || enemy_.rewardCrewPending ||
                               crewRoster().size() > std::size_t(devCrewLimit_)) &&
                                  ((!crew_[i].dead && !crew_[i].dying) || crew_[i].cloneReady)});
        }
        if (enemy_.rewardCrewPending) {
            result.push_back({"rename:new", "Rename " + enemy_.rewardCrewName, dismissCrew_ == -1});
            result.push_back({"dismiss:new", "Dismiss new crewmember", dismissCrew_ == -1});
        }
        if (dismissCrew_ != -1) {
            result.push_back({"dismiss:confirm", "Confirm dismissal", true});
            result.push_back({"dismiss:cancel", "Cancel dismissal", true});
        }
        result.push_back({"stations:save", "Save crew stations", docked()});
        result.push_back({"stations:return", "Return crew to stations", docked()});
    } else if (panel_ == "sector") {
        const auto& choices = sectorGraph_.current().neighbors;
        for (std::size_t index = 0; index < choices.size(); ++index) {
            const auto& destination = sectorGraph_.nodes()[choices[index]];
            const std::string label = destination.title;
            result.push_back({"sector:" + std::to_string(index),
                              label,
                              docked() && sector_ < 8 && fuel_ > 0 && canFly()});
        }
    }
    return result;
}

bool Run::resumePausedShipScreen() {
    if (panel_ != "pause" || pausedShipPanel_.empty()) {
        return false;
    }
    // FocusWindow::Close on the MenuScreen leaves the ship screen's
    // TabbedWindow, tab and CrewManifest delete dialog as they were.
    panel_ = std::exchange(pausedShipPanel_, {});
    dismissCrew_ = std::exchange(pausedDismissCrew_, -1);
    return true;
}

const std::string& Run::visibleOrPausedPanel() const {
    return (panel_ == "pause" || panel_.starts_with("pause_confirm:")) && !pausedShipPanel_.empty()
               ? pausedShipPanel_
               : panel_;
}

bool Run::voyageAction(const std::string& target, const std::string& action) {
    logging::verbose("action", "voyage '{}' {} (panel '{}')", target, action, panel_);
    if (target.starts_with("pause:")) {
        if (action != "click" || !open_ || defeated_) {
            return true;
        }
        if (panel_.starts_with("pause_confirm:")) {
            if (target == "pause:no") {
                panel_ = "pause";
            } else if (target == "pause:yes") {
                gameOverCommand_ = panel_ == "pause_confirm:hangar" ? 2 : 1;
            }
            return true;
        }
        if (panel_ != "pause") {
            return true;
        }
        if (target == "pause:continue") {
            if (!resumePausedShipScreen()) {
                panel_.clear();
            }
        } else if (target == "pause:hangar" || target == "pause:restart") {
            panel_ = "pause_confirm:" + target.substr(6);
        } else if (target == "pause:main_menu") {
            gameOverCommand_ = 5;
        } else if (target == "pause:options") {
            gameOverCommand_ = 7;
        } else if (target == "pause:controls") {
            gameOverCommand_ = 8;
        } else if (target == "pause:save_quit" && !tutorialOwned_) {
            gameOverCommand_ = 9;
        }
        return true;
    }
    if (target == "panel:close" && panel_.starts_with("pause_confirm:")) {
        if (action == "click") {
            panel_ = "pause";
        }
        return true;
    }
    if (storeAction(target, action)) {
        return true;
    }
    if (pendingStoreOffer_ >= 0) {
        return true;
    }
    if (target.starts_with("rename:")) {
        const int i = indexOf(std::string_view(target).substr(7));
        const auto characters = std::ranges::count_if(action, [](unsigned char byte) {
            return (byte & 0xc0) != 0x80;
        });
        if (target == "rename:new" && enemy_.rewardCrewPending && panel_ == "crew" &&
            dismissCrew_ == -1) {
            if (action == "click") {
                renameCrew_ = -2;
                crewRenameText_ = enemy_.rewardCrewFullName;
                crewRenameCaret_ = crewRenameText_.size();
            } else if (!action.empty() && characters <= 15 &&
                       action.find_first_of("\r\n\t") == std::string::npos) {
                const auto previous = enemy_.rewardCrewName;
                enemy_.rewardCrewName = enemy_.rewardCrewFullName = action;
                for (auto& row : pendingVoyageEffects_) {
                    if (row.value("kind", std::string{}) == "crewSkills" &&
                        row.value("name", std::string{}) == previous) {
                        row["name"] = action;
                    }
                }
            }
        } else if (panel_ == "crew" &&
                   (docked() || enemy_.rewardCrewPending ||
                    crewRoster().size() > std::size_t(devCrewLimit_)) &&
                   dismissCrew_ == -1 && i >= 0 && i < static_cast<int>(crew_.size()) &&
                   ((!crew_[i].dead && !crew_[i].dying) || crew_[i].cloneReady) &&
                   action == "click") {
            renameCrew_ = i;
            crewRenameText_ = crew_[i].appearance.fullName;
            crewRenameCaret_ = crewRenameText_.size();
        } else if ((docked() ||
                    (panel_ == "crew" && (enemy_.rewardCrewPending ||
                                          crewRoster().size() > std::size_t(devCrewLimit_)))) &&
                   dismissCrew_ == -1 && i >= 0 && i < static_cast<int>(crew_.size()) &&
                   !action.empty() && characters <= 15 &&
                   action.find_first_of("\r\n\t") == std::string::npos) {
            crew_[i].appearance.name = action;
            crew_[i].appearance.fullName = action;
        }
        return true;
    }
    if (target.starts_with("panel:")) {
        if (panel_ == "crew" &&
            (dismissCrew_ != -1 || crewRoster().size() > std::size_t(devCrewLimit_))) {
            return true;
        }
        if (action != "click" || defeated_ || (event_ && target != "panel:close") || mapOpen_ ||
            jumpTicks_ > 0) {
            return true;
        }
        const auto panel = target.substr(6);
        // Closing a tutorial MenuScreen reveals the ship screen it covered.
        if (panel == "close" && resumePausedShipScreen()) {
            return true;
        }
        const auto lesson = tutorialOwned_ ? tutorial_.current().name : std::string{};
        if (tutorialBlocksPanelAccept() && panel != panel_) {
            return true;
        }
        if (panel_.empty() && tutorialOwned_ &&
            (panel == "ship" || panel == "upgrades" || panel == "equipment" || panel == "crew") &&
            lesson != "COMBAT_5" && lesson != "INVENTORY_1") {
            return true;
        }
        if (panel == "close") {
            const bool returnToMap = panel_ == "sector";
            finishUpgrades(true);
            dismissCrew_ = renameCrew_ = pausedDismissCrew_ = -1;
            pausedShipPanel_.clear();
            panel_.clear();
            mapOpen_ = returnToMap;
            return true;
        }
        if (panel == "store") {
            openStore();
            return true;
        }
        // CommandGui::KeyDown 0x520495 and the inactive ship GenericButton
        // ignore requests while the danger flag is set.
        if (panel == "ship" && panel_.empty() && !shipButtonActive()) {
            return true;
        }
        if (panel == "pause" || panel == "ship" || panel == "abilities" ||
            (panel == "crew" && enemy_.rewardCrewPending) ||
            (docked() && (panel == "upgrades" || panel == "equipment" || panel == "crew" ||
                          panel == "sector"))) {
            if (panel_ == "upgrades" && panel != "upgrades" && panel != "ship") {
                finishUpgrades(true);
            }
            dismissCrew_ = renameCrew_ = pausedDismissCrew_ = -1;
            pausedShipPanel_.clear();
            panel_ = tutorialOwned_ && lesson == "INVENTORY_1" ? "equipment"
                     : panel == "ship"                         ? "upgrades"
                                                               : panel;
            panelPage_ = 0;
            cancelAim();
        }
        return true;
    }
    if (target == "panel_next" || target == "panel_previous") {
        if (action == "click") {
            const int pages = std::max(1, (static_cast<int>(voyageButtons().size()) + 9) / 10);
            panelPage_ = std::clamp(panelPage_ + (target == "panel_next" ? 1 : -1), 0, pages - 1);
        }
        return true;
    }
    const bool recognized = target == "save_menu" || target.starts_with("upgrade:") ||
                            target.starts_with("equip:") || target.starts_with("unequip:") ||
                            target.starts_with("dismiss:") || target.starts_with("sector:") ||
                            target.starts_with("distress:") || target.starts_with("stations:");
    if (!recognized) {
        return false;
    }
    if ((action != "click" && !(target.starts_with("upgrade:") && action == "right")) || !open_ ||
        defeated_) {
        return true;
    }
    if (target == "save_menu") {
        gameOverCommand_ = 5;
        return true;
    }
    if (target.starts_with("distress:")) {
        if (!hostile_ && !event_ && distressWaitTicks_ == 0) {
            if (target == "distress:toggle" && fuel_ == 0) {
                distress_ = !distress_;
            } else if (target == "distress:wait" && canWaitAtBeacon()) {
                waitForFuel();
            }
        }
        return true;
    }
    const bool pendingCrewAction =
        panel_ == "crew" &&
        (enemy_.rewardCrewPending || crewRoster().size() > std::size_t(devCrewLimit_)) &&
        target.starts_with("dismiss:");
    if (!docked() && !pendingCrewAction) {
        return true;
    }
    const int index = indexOf(std::string_view(target).substr(target.find(':') + 1));
    const auto previousResources = resourceAmounts();
    if (dismissCrew_ != -1 && target != "dismiss:confirm" && target != "dismiss:cancel") {
        return true;
    }
    if (target.starts_with("upgrade:") && action == "right" && panel_ == "upgrades") {
        const int id = target == "upgrade:reactor" ? -1 : index;
        const auto found = std::find_if(
            upgradePurchases_.rbegin(), upgradePurchases_.rend(), [id](const auto& entry) {
                return entry.first == id;
            });
        if (found != upgradePurchases_.rend()) {
            scrap_ += found->second;
            upgradePurchases_.erase(std::next(found).base());
            // ReactorButton531a80 / UpgradeBox532fd0 refund one staged level.
            audio_.sound("downgradeSystem");
        }
    } else if (target == "upgrade:accept" || target == "upgrade:cancel") {
        if (panel_ == "upgrades") {
            if (tutorialBlocksPanelAccept()) {
                return true;
            }
            finishUpgrades(target == "upgrade:accept");
            panel_.clear();
        }
    } else if (target == "upgrade:undo") {
        if (panel_ == "upgrades" && !upgradePurchases_.empty()) {
            // Upgrades::MouseClick532dc0 emits one cue for the whole cart.
            audio_.sound("undoUpgrades");
            finishUpgrades(false);
        }
    } else if (target == "upgrade:reactor") {
        const int reactor = ship_.reactor + pendingUpgrade(-1);
        const int cost = reactorUpgradeCost(reactor);
        if (panel_ == "upgrades" && reactor < std::min(25, devReactorLimit_) && scrap_ >= cost) {
            scrap_ -= cost;
            upgradePurchases_.emplace_back(-1, cost);
            // ReactorButton::OnClick531ce0, after a successful purchase.
            audio_.sound("upgradeSystem");
        } else if (panel_ == "upgrades" && reactor < std::min(25, devReactorLimit_) &&
                   scrap_ < cost) {
            failScrapPurchase();
        }
    } else if (target.starts_with("upgrade:")) {
        if (auto* sys = system(index); sys && panel_ == "upgrades") {
            const auto& bp = catalog_.system(sys->definition.name);
            const int level = sys->definition.power + pendingUpgrade(index);
            const int maximum = systemMaximum(*sys);
            if (level < maximum && level > 0 && std::size_t(level - 1) < bp.costs.size() &&
                scrap_ >= bp.costs[level - 1]) {
                const int cost = bp.costs[level - 1];
                scrap_ -= cost;
                upgradePurchases_.emplace_back(index, cost);
                // UpgradeBox::MouseClick532c60 shares the reactor purchase cue.
                audio_.sound("upgradeSystem");
            } else if (level < maximum && level > 0 &&
                       static_cast<std::size_t>(level - 1) < bp.costs.size() &&
                       scrap_ < bp.costs[level - 1]) {
                failScrapPurchase();
            }
        }
    } else if (target.starts_with("equip:")) {
        inventoryMove({}, index, true);
    } else if (target.starts_with("unequip:")) {
        const auto split = target.find(':', 8);
        if (split != std::string::npos) {
            inventoryMove(target.substr(8, split - 8),
                          indexOf(std::string_view(target).substr(split + 1)),
                          false);
        }
    } else if (target == "dismiss:new" && enemy_.rewardCrewPending && panel_ == "crew") {
        dismissCrew_ = -2;
        renameCrew_ = -1;
    } else if (target == "dismiss:cancel") {
        dismissCrew_ = -1;
    } else if (target == "dismiss:confirm") {
        const int selected = dismissCrew_;
        dismissCrew_ = -1;
        if (panel_ == "crew" && selected == -2 && enemy_.rewardCrewPending) {
            const auto name = enemy_.rewardCrewName;
            nlohmann::json remaining = nlohmann::json::array();
            for (const auto& row : pendingVoyageEffects_) {
                if (!(row.value("kind", std::string{}) == "crewSkills" &&
                      row.value("name", std::string{}) == name)) {
                    remaining.push_back(row);
                }
            }
            pendingVoyageEffects_ = std::move(remaining);
            enemy_.rewardCrewPending = enemy_.rewardCrewOverflow = false;
            renameCrew_ = -1;
            commitVoyageEffects();
        } else if (panel_ == "crew" && selected >= 0 && selected < static_cast<int>(crew_.size()) &&
                   ((!crew_[selected].dead && !crew_[selected].dying) ||
                    crew_[selected].cloneReady) &&
                   crewRoster().size() > 1) {
            killCrew(crew_[selected], true);
            reindexCrewReferences(0, selected);
            crew_.erase(crew_.begin() + selected);
            refreshCrewNames();
            previousCrew_.clear();
            renameCrew_ = -1;
            updateManning();
            commitCombatCrewReward();
            commitVoyageEffects();
        }
    } else if (target.starts_with("dismiss:") && panel_ == "crew" && index >= 0 &&
               index < static_cast<int>(crew_.size()) &&
               ((!crew_[index].dead && !crew_[index].dying) || crew_[index].cloneReady) &&
               crewRoster().size() > 1) {
        renameCrew_ = -1;
        dismissCrew_ = index;
    } else if (target.starts_with("sector:")) {
        nextSector(index);
    } else if (target == "distress:toggle" && fuel_ == 0 && distressWaitTicks_ == 0) {
        distress_ = !distress_;
    } else if (target == "distress:wait" && canWaitAtBeacon()) {
        waitForFuel();
    } else if (target == "stations:save") {
        stations(true);
    } else if (target == "stations:return") {
        stations(false);
    }
    noteResourceChanges(previousResources);
    return true;
}

void Run::voyageClick(float x, float y, bool right) {
    if (panel_ == "pause") {
        if (right) {
            return;
        }
        const auto rect = pauseMenuRect();
        const auto buttons = voyageButtons();
        constexpr std::array<int, 7> offsets{18, 69, 106, 143, 180, 217, 254};
        for (std::size_t index = 0; index < buttons.size(); ++index) {
            if (buttons[index].enabled &&
                Rect{rect.x + 17, rect.y + offsets[index], rect.w - 34, 24}.contains(x, y)) {
                voyageAction(buttons[index].target, "click");
                break;
            }
        }
        return;
    }
    if (panel_.starts_with("pause_confirm:")) {
        const auto layout = crewConfirmationLayout(renderer_,
                                                   texts_.get("confirm_end_game", locale_),
                                                   texts_.get("confirm_yes", locale_),
                                                   texts_.get("confirm_no", locale_),
                                                   locale_ == "zh-Hans");
        const float fx = 640 + (tutorialOwned_ ? 150 : 0) - layout.width / 2;
        const float fy = std::floor(
            (720 - (renderer_.image("img/confirm_window.png").h + layout.height - 22)) / 2);
        const float by = fy + layout.height + 28;
        if (!right && Rect{fx + layout.yesX, by, layout.yesWidth, 24}.contains(x, y)) {
            voyageAction("pause:yes", "click");
        } else if (!right && Rect{fx + layout.noX, by, layout.noWidth, 24}.contains(x, y)) {
            voyageAction("pause:no", "click");
        }
        return;
    }
    if (panel_ == "store") {
        if (!right) {
            storeClick(x, y);
        }
        return;
    }
    if (panel_ == "sector") {
        if (right ||
            sectorCancelRect(renderer_, texts_.get("button_cancel", locale_), locale_ == "zh-Hans")
                .contains(x, y)) {
            panel_.clear();
            mapOpen_ = true;
            return;
        }
        // StarMap::MouseClick 0x5ca380 commits potentialSectorChoice; OnLoop
        // 0x5d13a0 accepts it only when it neighbours the current sector.
        const auto& choices = sectorGraph_.current().neighbors;
        const auto chosen = std::ranges::find(choices, sectorPotentialAt(x, y));
        if (chosen != choices.end()) {
            voyageAction("sector:" + std::to_string(chosen - choices.begin()), "click");
        }
        return;
    }

    if (panel_ == "crew" && dismissCrew_ != -1) {
        const auto living = crewRoster();
        const int display =
            dismissCrew_ == -2
                ? static_cast<int>(living.size())
                : static_cast<int>(
                      std::ranges::find(living, static_cast<std::size_t>(dismissCrew_)) -
                      living.begin());
        const float popupX =
            managementPanelX() + 28.f + static_cast<float>(display % 3) * 170.f +
            (display >= 6 && !enemy_.rewardCrewPending && living.size() <= 8 ? 86.f : 0.f);
        const float popupY = 266.f + static_cast<float>(display / 3) * 133.f;
        const auto layout = crewConfirmationLayout(renderer_,
                                                   texts_.get("confirm_dismiss", locale_),
                                                   texts_.get("confirm_yes", locale_),
                                                   texts_.get("confirm_no", locale_),
                                                   locale_ == "zh-Hans");
        const float buttonY = popupY - 7 + layout.height + 28;
        if (Rect{popupX - 7 + layout.yesX, buttonY, layout.yesWidth, 24}.contains(x, y)) {
            voyageAction("dismiss:confirm", "click");
        } else if (Rect{popupX - 7 + layout.noX, buttonY, layout.noWidth, 24}.contains(x, y)) {
            voyageAction("dismiss:cancel", "click");
        }
        return;
    }
    const bool shipPanel = panel_ == "upgrades" || panel_ == "crew" || panel_ == "equipment";
    if (shipPanel) {
        const float paneX = managementPanelX();
        if (Rect{paneX + 436, 556, 132, 32}.contains(x, y)) {
            voyageAction("panel:close", "click");
            return;
        }
        if (y >= 85 && y < 123) {
            if (x >= paneX + 5 && x < paneX + 94) {
                voyageAction("panel:upgrades", "click");
            } else if (x >= paneX + 97 && x < paneX + 194) {
                voyageAction("panel:crew", "click");
            } else if (x >= paneX + 197 && x < paneX + 300) {
                voyageAction("panel:equipment", "click");
            }
            return;
        }
        if (panel_ == "upgrades") {
            if (Rect{paneX + 33, 556, 97, 32}.contains(x, y)) {
                voyageAction("upgrade:undo", "click");
                return;
            }
            int slot = 0;
            for (int id : upgradeOrder) {
                if (!system(id)) {
                    continue;
                }
                if (Rect{paneX + 39.f + slot * 66.f, 200, 66, 150}.contains(x, y)) {
                    voyageAction("upgrade:" + std::to_string(id), right ? "right" : "click");
                    return;
                }
                if (++slot == 8) {
                    break;
                }
            }
            for (int id : {6, 7, 8, 12}) {
                if (system(id) &&
                    Rect{paneX + 16.f + (id == 12 ? 3 : id - 6) * 66.f, 415, 66, 113}.contains(x,
                                                                                               y)) {
                    voyageAction("upgrade:" + std::to_string(id), right ? "right" : "click");
                    return;
                }
            }
            if (Rect{paneX + 305, 412, 272, 113}.contains(x, y)) {
                voyageAction("upgrade:reactor", right ? "right" : "click");
            }
            return;
        }
        if (panel_ == "crew") {
            if (renameCrew_ != -1) {
                crewRenameKey("Enter");
                return;
            }
            const auto living = crewRoster();
            for (std::size_t i = 0; i < living.size() + (enemy_.rewardCrewPending ? 1 : 0) && i < 9;
                 ++i) {
                const bool pending = i == living.size();
                const auto id = pending ? 0 : living[i];
                const float cx =
                    paneX + 75.f + static_cast<float>(i % 3) * 170.f +
                    (i >= 6 && !enemy_.rewardCrewPending && living.size() <= 8 ? 86.f : 0.f);
                const float cy = 173.f + static_cast<float>(i / 3) * 133.f;
                if (Rect{cx, cy + 67, 100, 23}.contains(x, y)) {
                    voyageAction(pending ? "dismiss:new" : "dismiss:" + std::to_string(id),
                                 "click");
                    return;
                }
                if (Rect{cx - 25, cy + 43, 150, 23}.contains(x, y)) {
                    voyageAction(pending ? "rename:new" : "rename:" + std::to_string(id), "click");
                    return;
                }
            }
            return;
        }
        if (panel_ == "equipment") {
            inventoryClick(x, y);
            return;
        }
        const auto buttons = voyageButtons();
        for (int row = 0; row < 10; ++row) {
            const auto index =
                static_cast<std::size_t>(panelPage_) * 10 + static_cast<std::size_t>(row);
            if (index < buttons.size() && buttons[index].enabled &&
                Rect{360, 205.f + row * 33.f, 550, 30}.contains(x, y)) {
                voyageAction(buttons[index].target, "click");
                return;
            }
        }
        return;
    }
    if (Rect{1010, 90, 65, 35}.contains(x, y)) {
        panel_.clear();
        return;
    }
    if (Rect{270, 570, 120, 35}.contains(x, y)) {
        voyageAction("panel_previous", "click");
        return;
    }
    if (Rect{850, 570, 120, 35}.contains(x, y)) {
        voyageAction("panel_next", "click");
        return;
    }
    const auto buttons = voyageButtons();
    for (int row = 0; row < 10; ++row) {
        const auto index =
            static_cast<std::size_t>(panelPage_) * 10 + static_cast<std::size_t>(row);
        if (index < buttons.size() && buttons[index].enabled &&
            Rect{260, 160.f + 38.f * row, 760, 34}.contains(x, y)) {
            if (!voyageAction(buttons[index].target, "click")) {
                advancedAction(buttons[index].target, "click");
            }
            return;
        }
    }
}

std::string Run::systemLevelDescription(int systemId, int level) const {
    std::string key;
    std::string first;
    std::string second;
    switch (systemId) {
    case 0:
        if (level % 2 == 0) {
            return {};
        }
        key = "shields_" + std::to_string(level / 2);
        break;
    case 1: {
        constexpr std::array<int, 8> evade{5, 10, 15, 20, 25, 28, 31, 35};
        key = "engine";
        first = std::to_string(evade[std::size_t(std::clamp(level, 0, 7))]);
        std::ostringstream value;
        value << 1.f + float(level) * .25f;
        second = value.str();
        break;
    }
    case 2:
        key = "oxygen_on";
        first = std::to_string(level == 0 ? 1 : 3 * level);
        break;
    case 3:
    case 4:
        key = "system_power";
        break;
    case 5:
        key = "medbay_healing";
        first = level == 0 ? "1" : level == 1 ? "1.5" : "3";
        break;
    case 6:
        key = "pilot_" + std::to_string(level + 1);
        break;
    case 7:
        key = "sensor_" + std::to_string(level + 1);
        break;
    case 8:
        key = "door_" + std::to_string(level + 1);
        break;
    case 9:
        key = "teleporter_on";
        first = std::to_string(level == 1 ? 15 : level == 2 ? 10 : 20);
        break;
    case 10:
        key = "cloak";
        first = std::to_string(5 * level + 5);
        break;
    case 11:
        key = "artillery_" + std::to_string(level + 1);
        break;
    case 12:
        key = "battery_power";
        first = std::to_string(2 * level + 2);
        break;
    case 13:
        key = "clone_full";
        first = std::to_string(level == 0 ? 12 : level == 1 ? 9 : 7);
        second = std::to_string(level == 0 ? 8 : level == 1 ? 16 : 25);
        break;
    case 14:
        key = "mind_" + std::to_string(level + 1);
        break;
    case 15:
        key = "hacking_duration";
        first = std::to_string(4 + 3 * level);
        break;
    default:
        return {};
    }
    auto result = texts_.get(key, locale_);
    const auto replace = [&](const std::string& marker, const std::string& value) {
        std::size_t offset = 0;
        while ((offset = result.find(marker, offset)) != std::string::npos) {
            result.replace(offset, marker.size(), value);
            offset += value.size();
        }
    };
    replace("\\1", first);
    replace("\\2", second);
    return result;
}

float Run::managementPanelX() const {
    if (managementPanelLocale_ == locale_) {
        return managementPanelCachedX_;
    }
    // InfoBox::CalcCenterCountForSystem/CalcWidthForSystem and
    // CommandGui::OnLanguageChange (0x546de0, 0x5474d0, 0x523520).
    const int left = renderer_.image("img/upgradeUI/details_base_A.png").w;
    const int center = renderer_.image("img/upgradeUI/details_base_B.png").w;
    const int right = renderer_.image("img/upgradeUI/details_base_C.png").w;
    int maximum = 0;
    for (int id = 0; id < 16; ++id) {
        const auto& blueprint =
            catalog_.system(std::array<std::string, 16>{"shields",
                                                        "engines",
                                                        "oxygen",
                                                        "weapons",
                                                        "drones",
                                                        "medbay",
                                                        "pilot",
                                                        "sensors",
                                                        "doors",
                                                        "teleporter",
                                                        "cloaking",
                                                        "artillery",
                                                        "battery",
                                                        "clonebay",
                                                        "mind",
                                                        "hacking"}[std::size_t(id)]);
        const int count = renderer_.systemInfoCenterCount(
            id,
            blueprint.maxPower,
            [&](int level) {
                return systemLevelDescription(id, level);
            },
            locale_ == "zh-Hans");
        maximum = std::max(maximum, left + center * count + right);
    }
    managementPanelLocale_ = locale_;
    managementPanelCachedX_ = static_cast<float>(std::clamp(682 - maximum, 150, 340));
    return managementPanelCachedX_;
}

void Run::voyageRender() {
    if (panel_ == "store") {
        storeRender();
        return;
    }
    if (panel_.empty()) {
        return;
    }
    if (panel_ == "pause" || panel_.starts_with("pause_confirm:")) {
        renderer_.setTint(panel_ == "pause" ? 1.f : .5f);
        {
            const auto rect = pauseMenuRect();
            auto& image = renderer_.image("img/optionsUI/escape_main.png");
            renderer_.sprite(image, {rect.x, rect.y, 81, rect.h}, {0, 0, 81, rect.h});
            renderer_.sprite(
                image, {rect.x + 81, rect.y, rect.w - 161, rect.h}, {81, 0, 1, rect.h});
            renderer_.sprite(
                image, {rect.x + rect.w - 80, rect.y, 80, rect.h}, {82, 0, 80, rect.h});
            constexpr std::array<const char*, 7> labels{"escape_continue",
                                                        "escape_mainmenu",
                                                        "escape_hangar",
                                                        "escape_reset",
                                                        "escape_options",
                                                        "escape_controls",
                                                        "escape_save"};
            constexpr std::array<int, 7> offsets{18, 69, 106, 143, 180, 217, 254};
            for (std::size_t index = 0; index < labels.size(); ++index) {
                const Rect button{rect.x + 17, rect.y + offsets[index], rect.w - 34, 24};
                const bool enabled = index != 6 || !tutorialOwned_;
                const bool selected = (panel_ == "pause_confirm:hangar" && index == 2) ||
                                      (panel_ == "pause_confirm:restart" && index == 3);
                roundedButtonFace(renderer_,
                                  button,
                                  4,
                                  !enabled ? SDL_Color{164, 171, 160, 255}
                                  : selected || button.contains(mouseX_, mouseY_)
                                      ? SDL_Color{255, 230, 94, 255}
                                      : SDL_Color{235, 245, 229, 255});
                text(texts_.get(labels[index], locale_),
                     button.x + button.w / 2,
                     button.y + button.h / 2 + 2 -
                         textLineHeight(62, texts_.get(labels[index], locale_)) / 2,
                     62,
                     true,
                     {25, 49, 51, 255});
            }
            renderPauseMenuStatus();
        }
        if (panel_.starts_with("pause_confirm:")) {
            renderer_.setTint(1);
            const auto layout = crewConfirmationLayout(renderer_,
                                                       texts_.get("confirm_end_game", locale_),
                                                       texts_.get("confirm_yes", locale_),
                                                       texts_.get("confirm_no", locale_),
                                                       locale_ == "zh-Hans");
            const float confirmationCenter = 640.f + (tutorialOwned_ ? 150.f : 0.f);
            const float fx = confirmationCenter - layout.width / 2;
            const float fy = std::floor(
                (720 - (renderer_.image("img/confirm_window.png").h + layout.height - 22)) / 2);
            auto& frame = renderer_.image("img/confirm_window.png");
            const std::array<float, 3> sx{0, 70, 71};
            const std::array<float, 3> sw{70, 1, 69};
            const std::array<float, 3> dx{fx, fx + 70, fx + layout.width - 69};
            const std::array<float, 3> dw{70, layout.width - 139, 69};
            for (std::size_t index = 0; index < sx.size(); ++index) {
                renderer_.sprite(
                    frame, {dx[index], fy, dw[index], 26}, {sx[index], 0, sw[index], 26});
                renderer_.sprite(frame,
                                 {dx[index], fy + 26, dw[index], layout.height - 21},
                                 {sx[index], 26, sw[index], 1});
                renderer_.sprite(frame,
                                 {dx[index], fy + layout.height + 5, dw[index], 17},
                                 {sx[index], 27, sw[index], 17});
            }
            float y = fy + 17;
            for (const auto& line : layout.lines) {
                text(line, confirmationCenter, y, 52, true);
                y += textLineHeight(52, texts_.get("confirm_end_game", locale_));
            }
            const float by = fy + layout.height + 28;
            const float yesX = fx + layout.yesX;
            const float noX = fx + layout.noX;
            const float endX = noX + layout.noWidth;
            const auto wellStrip = [&](float x, float width, float sourceX, float sourceWidth) {
                renderer_.sprite(
                    frame, {x, fy + layout.height + 22, width, 46}, {sourceX, 44, sourceWidth, 46});
            };
            wellStrip(fx, 20, 0, 20);
            wellStrip(fx + 20, layout.yesX - 41, 20, 1);
            wellStrip(yesX - 21, 31, 21, 31);
            wellStrip(yesX + 10, layout.yesWidth - 20, 52, 1);
            wellStrip(yesX + layout.yesWidth - 10, 34, 53, 34);
            wellStrip(noX + 10, layout.noWidth - 20, 87, 1);
            wellStrip(endX - 10, 31, 88, 31);
            wellStrip(endX + 21, layout.width - 20 - (layout.noX + layout.noWidth + 21), 119, 1);
            wellStrip(fx + layout.width - 20, 20, 120, 20);
            for (bool yes : {true, false}) {
                const Rect button{fx + (yes ? layout.yesX : layout.noX),
                                  by,
                                  yes ? layout.yesWidth : layout.noWidth,
                                  24};
                roundedButtonFace(renderer_,
                                  button,
                                  4,
                                  button.contains(mouseX_, mouseY_)
                                      ? SDL_Color{255, 230, 94, 255}
                                      : SDL_Color{235, 245, 229, 255});
                text(texts_.get(yes ? "confirm_yes" : "confirm_no", locale_),
                     button.x + button.w / 2,
                     button.y + button.h / 2 + 2 -
                         textLineHeight(62,
                                        texts_.get(yes ? "confirm_yes" : "confirm_no", locale_)) /
                             2,
                     62,
                     true,
                     {25, 49, 51, 255});
            }
        }
        return;
    }
    if (panel_ == "sector") {
        // OnRender 0x5d9ab0: archived frame at358,211 and sector graph
        // translated to415,300; RenderSectorMap0x5cf7d0 has radius7/6 nodes.
        renderer_.draw("img/map/sector_box.png", 358, 211);
        text(texts_.get("sector_title", locale_), 378, 219, 63, false, {25, 49, 51, 255});
        const auto& nodes = sectorGraph_.nodes();
        const auto& current = sectorGraph_.current();
        auto circle = [&](float x, float y, float radius, SDL_Color color) {
            std::vector<SDL_FPoint> points;
            for (int edge = 0; edge < 36; ++edge) {
                const float angle = static_cast<float>(edge) * 6.283185307f / 36.f;
                points.push_back({x + std::cos(angle) * radius, y + std::sin(angle) * radius});
            }
            renderer_.polygon(points, color);
        };
        std::vector<bool> reachable(nodes.size(), false);
        std::vector<int> pending = current.neighbors;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const int id = pending[index];
            if (reachable[id]) {
                continue;
            }
            reachable[id] = true;
            pending.insert(pending.end(), nodes[id].neighbors.begin(), nodes[id].neighbors.end());
        }
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const auto& node = nodes[index];
            const float x = 415.f + node.level * 66.f;
            const float y = 300.f + node.row;
            for (int destination : node.neighbors) {
                const auto& next = nodes[destination];
                const float nx = 415.f + next.level * 66.f;
                const float ny = 300.f + next.row;
                const float length = std::sqrt((nx - x) * (nx - x) + (ny - y) * (ny - y));
                const float dx = (ny - y) / length;
                const float dy = (nx - x) / length;
                const SDL_Color color =
                    node.visited && next.visited                  ? SDL_Color{255, 255, 50, 255}
                    : node.level == current.level && node.visited ? SDL_Color{100, 255, 100, 255}
                    : reachable[index] && reachable[destination]  ? SDL_Color{255, 255, 255, 255}
                                                                  : SDL_Color{125, 125, 125, 255};
                renderer_.polygon(
                    {{x - dx, y + dy}, {nx - dx, ny + dy}, {nx + dx, ny - dy}, {x + dx, y - dy}},
                    color);
            }
            const bool selectable = std::find(current.neighbors.begin(),
                                              current.neighbors.end(),
                                              static_cast<int>(index)) != current.neighbors.end();
            const bool potentialNeighbor =
                std::ranges::find(current.neighbors, sectorPotential_) != current.neighbors.end();
            const bool potential = sectorPotential_ == static_cast<int>(index);
            const SDL_Color border = potential && selectable ? SDL_Color{100, 255, 100, 255}
                                     : node.visited          ? SDL_Color{255, 255, 50, 255}
                                     : reachable[index]      ? SDL_Color{255, 255, 255, 255}
                                                             : SDL_Color{125, 125, 125, 255};
            circle(x, y, 7, border);
            auto color = node.type == 1   ? SDL_Color{214, 50, 50, 255}
                         : node.type == 2 ? SDL_Color{128, 51, 210, 255}
                                          : SDL_Color{135, 199, 74, 255};
            if (!reachable[index] && !node.visited) {
                color.r /= 4;
                color.g /= 4;
                color.b /= 4;
            }
            circle(x, y, 6, color);
            if (node.visited && node.level == current.level) {
                auto& image = renderer_.image("img/map/map_icon_ship.png");
                const SDL_FPoint pivot{10, 32};
                renderer_.sprite(image,
                                 {x - 10, y - 32, 64, 64},
                                 {0, 0, 64, 64},
                                 -(static_cast<double>(animation_ % 1200) + uiFraction_) * .3,
                                 {255, 255, 255, 255},
                                 false,
                                 &pivot);
            }
            if (selectable) {
                const auto choice = static_cast<std::size_t>(std::find(current.neighbors.begin(),
                                                                       current.neighbors.end(),
                                                                       static_cast<int>(index)) -
                                                             current.neighbors.begin());
                const auto box = sectorNameRect(renderer_, node, choice, locale_ == "zh-Hans");
                // RenderSectorMap: yellow for the potential neighbour, grey for
                // the others while one is chosen, otherwise white.
                const SDL_Color labelColor = !potentialNeighbor ? SDL_Color{255, 255, 255, 255}
                                             : potential        ? SDL_Color{255, 255, 50, 255}
                                                                : SDL_Color{125, 125, 125, 255};
                renderer_.fill(box, {0, 0, 0, 204});
                renderer_.fill({box.x, box.y, box.w, 1}, labelColor);
                renderer_.fill({box.x, box.y + box.h - 1, box.w, 1}, labelColor);
                renderer_.fill({box.x, box.y, 1, box.h}, labelColor);
                renderer_.fill({box.x + box.w - 1, box.y, 1, box.h}, labelColor);
                text(std::to_string(choice + 1) + ". " + node.shortTitle,
                     box.x + 5,
                     box.y + 1,
                     7,
                     false,
                     labelColor);
            }
            // RenderSectorMap 0x5cf7d0: four mirrored target-box corners pulse
            // round((1 - cos(2*pi*t)) / 2 * 5) px around each choice while no
            // neighbour is potential, else only around the potential sector
            // (any sector); green for the potential one, yellow otherwise.
            if ((selectable && !potentialNeighbor) || potential) {
                auto& corner = renderer_.image(potential ? "img/map/map_targetbox_sector_g.png"
                                                         : "img/map/map_targetbox_sector_y.png");
                const float pulse =
                    std::round((1.f - std::cos(6.2831855f * targetBoxTime())) * .5f * 5.f);
                const float offset = pulse - 20.f;
                const float w = static_cast<float>(corner.w);
                const float h = static_cast<float>(corner.h);
                for (int horizontal : {0, 1}) {
                    for (int vertical : {0, 1}) {
                        renderer_.sprite(corner,
                                         {horizontal ? x - offset - w : x + offset,
                                          vertical ? y - offset - h : y + offset,
                                          w,
                                          h},
                                         {0, 0, w, h},
                                         vertical ? 180 : 0,
                                         {255, 255, 255, 255},
                                         horizontal != vertical);
                    }
                }
            }
        }
        const auto civilian = texts_.get("sector_legend_civilian", locale_);
        const auto hostile = texts_.get("sector_legend_hostile", locale_);
        const auto nebula = texts_.get("sector_legend_nebula", locale_);
        const auto legendWidth = [&](const std::string& label) {
            return std::round(renderer_.measure(localizedFont(52, label), label, textScale(52)));
        };
        const float hostileX = 375.f + legendWidth(civilian) + 36.f;
        const float nebulaX = hostileX + legendWidth(hostile) + 36.f;
        // OnRender 0x5d9ab0: three backing rectangles and beveled end strips.
        const float legendEnd = nebulaX + legendWidth(nebula) + 30.f;
        const SDL_Color legendBack{31, 19, 19, 255};
        renderer_.fill({378, 517, hostileX - 378, 23}, legendBack);
        renderer_.fill({hostileX, 517, nebulaX - hostileX, 23}, legendBack);
        renderer_.fill({nebulaX, 517, legendEnd - nebulaX, 23}, legendBack);
        for (int edge = 0; edge < 3; ++edge) {
            renderer_.fill({375.f + edge, 520.f - edge, 1, 17.f + 2 * edge}, legendBack);
            renderer_.fill({legendEnd - edge, 520.f - edge, 1, 17.f + 2 * edge}, legendBack);
        }
        renderer_.draw("img/map/sector_legend_civilian.png", 381, 521);
        renderer_.draw("img/map/sector_legend_hostile.png", hostileX + 6, 521);
        renderer_.draw("img/map/sector_legend_nebula.png", nebulaX + 6, 521);
        text(civilian, 402, 519, 52, false, {235, 245, 229, 255});
        text(hostile, hostileX + 27, 519, 52, false, {235, 245, 229, 255});
        text(nebula, nebulaX + 27, 519, 52, false, {235, 245, 229, 255});
        const auto cancelLabel = texts_.get("button_cancel", locale_);
        const auto cancelRect = sectorCancelRect(renderer_, cancelLabel, locale_ == "zh-Hans");
        auto& cancelBase = renderer_.image("img/storeUI/store_close_base.png");
        const float baseX = cancelRect.x - 23;
        const float baseWidth = static_cast<float>(cancelBase.w);
        const float baseHeight = static_cast<float>(cancelBase.h);
        const float half = std::floor(baseWidth / 2);
        const float extra = cancelRect.w - 103;
        // InitPrimitives577040 stretches the central texel, retaining both ends.
        renderer_.sprite(cancelBase, {baseX, 545, half, baseHeight}, {0, 0, half, baseHeight});
        if (extra > 0) {
            renderer_.sprite(
                cancelBase, {baseX + half, 545, extra, baseHeight}, {half, 0, 1, baseHeight});
        }
        renderer_.sprite(cancelBase,
                         {baseX + half + extra, 545, baseWidth - half, baseHeight},
                         {half, 0, baseWidth - half, baseHeight});
        roundedButtonFace(renderer_, cancelRect, 4, {235, 245, 229, 255});
        // SetFont575db0/OnRender577750:552+32/2+4-38/2=553.
        text(cancelLabel, cancelRect.x + cancelRect.w / 2, 553, 63, true, {25, 49, 51, 255});
        return;
    }
    if (panel_ == "upgrades" || panel_ == "crew" || panel_ == "equipment") {
        const float paneX = managementPanelX();
        const std::string root = "img/upgradeUI/Equipment/";
        const bool confirmingCrew = panel_ == "crew" && dismissCrew_ != -1;
        renderer_.setTint(confirmingCrew ? .25f : 1.f);
        // TabbedWindow::OnRender 0x505c40 draws every tab but the current one
        // (Button::OnRender, image base "<current>_<tab>" from SetTab 0x504d60)
        // before the current window; MouseMove 0x505e10 hovers those buttons
        // only while no priority popup is open.
        const std::string hoverTarget = panelHoverSoundTarget(mouseX_, mouseY_);
        const auto tab = [&](std::string_view name, float x) {
            const std::string target = "panel:" + std::string(name);
            renderer_.draw(root + "tabButtons/" + panel_ + "_" + std::string(name) +
                               (hoverTarget == target ? "_select2.png" : "_on.png"),
                           paneX + x,
                           78);
        };
        if (panel_ != "upgrades") {
            tab("upgrades", 0);
        }
        if (panel_ != "crew") {
            tab("crew", 82);
        }
        if (panel_ != "equipment") {
            tab("equipment", 182);
        }
        renderer_.draw(root + (panel_ == "upgrades" ? "upgrades_main.png"
                               : panel_ == "crew"   ? "crew_main.png"
                                                    : "equipment_main.png"),
                       paneX,
                       78);
        if (panel_ == "upgrades") {
            text(name_, paneX + 302, 124, 24, true);
        }
        const auto buttonFace = [&](float x, float y, float width, SDL_Color color) {
            for (int inset = 4; inset > 0; --inset) {
                const float side = static_cast<float>(inset);
                renderer_.fill({x + side, y + 4 - side, width - 2 * side, 1}, color);
                renderer_.fill({x + side, y + 27 + side, width - 2 * side, 1}, color);
            }
            renderer_.fill({x, y + 4, width, 24}, color);
        };
        if (dismissCrew_ == -1) {
            renderer_.draw("img/upgradeUI/buttons_accept_base.png", paneX + 413, 549);
            // TabbedWindow::OnLoop 0x5060b0 disables its doneButton whenever
            // bBlockClose is set; its face must agree with the input guard.
            const bool acceptEnabled =
                !tutorialBlocksPanelAccept() &&
                (panel_ != "crew" || crewRoster().size() <= std::size_t(devCrewLimit_));
            const bool acceptHovered =
                acceptEnabled && Rect{paneX + 436, 556, 132, 32}.contains(mouseX_, mouseY_);
            // TextButton::OnRender 0x577750 selects its third primitive on
            // enabled hover; original COLOR_BUTTON_SELECT is (255,230,94).
            buttonFace(paneX + 436,
                       556,
                       132,
                       !acceptEnabled  ? SDL_Color{164, 171, 160, 255}
                       : acceptHovered ? SDL_Color{255, 230, 94, 255}
                                       : SDL_Color{235, 245, 229, 255});
            text(texts_.get("button_accept", locale_),
                 paneX + 502,
                 557,
                 63,
                 true,
                 {25, 49, 51, 255});
        }
        if (panel_ == "upgrades") {
            constexpr SDL_Color heading{25, 49, 51, 255};
            text(texts_.get("upgrade_frame_systems", locale_), paneX + 18, 151, 62, false, heading);
            text(texts_.get("upgrade_frame_subsystems", locale_),
                 paneX + 18,
                 375,
                 62,
                 false,
                 heading);
            text(
                texts_.get("upgrade_frame_reactor", locale_), paneX + 296, 375, 62, false, heading);
            renderer_.draw("img/upgradeUI/buttons_undo_base.png", paneX + 10, 549);
            buttonFace(paneX + 33,
                       556,
                       97,
                       upgradePurchases_.empty() ? SDL_Color{164, 171, 160, 255}
                                                 : SDL_Color{235, 245, 229, 255});
            text(texts_.get("button_undo", locale_), paneX + 81, 557, 63, true, {25, 49, 51, 255});
            int slot = 0;
            const auto drawUpgrade = [&](const RunSystem& sys, float x, float y, bool subsystem) {
                const auto& blueprint = catalog_.system(sys.definition.name);
                const int maximum = systemMaximum(sys);
                const int level = sys.definition.power + pendingUpgrade(sys.definition.id);
                const bool hovered =
                    Rect{x, y, 66, subsystem ? 110.f : 150.f}.contains(mouseX_, mouseY_);
                if (hovered) {
                    renderSystemInfo(sys, level, paneX + 600, 85);
                }
                const int cost =
                    level > 0 && level < maximum && std::size_t(level - 1) < blueprint.costs.size()
                        ? blueprint.costs[level - 1]
                        : -1;
                renderer_.draw("img/upgradeUI/upgrade_" +
                                   std::string(subsystem ? "subsystem" : "system") +
                                   (level >= maximum ? "_bar_max" : "_bar") +
                                   (hovered ? "_select2.png" : "_on.png"),
                               x,
                               y);
                for (int bar = 0; bar < std::min(maximum, subsystem ? 4 : 8); ++bar) {
                    renderer_.fill({x + 24, y + (subsystem ? 38.f : 74.f) - bar * 8.f, 15, 6},
                                   bar < sys.definition.power ? SDL_Color{80, 255, 85, 255}
                                   : bar < level              ? SDL_Color{255, 230, 80, 255}
                                                              : SDL_Color{104, 99, 65, 255});
                }
                const auto icon = "img/icons/s_" + sys.definition.name + "_green1.png";
                if (archive_.contains(icon)) {
                    renderer_.draw(icon, x, y + (subsystem ? 30.f : 65.f));
                }
                text(cost < 0 ? texts_.get("upgrade_max", locale_) : std::to_string(cost),
                     x + 33,
                     y + (subsystem ? 88.f : 125.f),
                     52,
                     true,
                     hovered ? SDL_Color{245, 239, 163, 255} : SDL_Color{235, 245, 229, 255});
            };
            for (int id : upgradeOrder) {
                const auto* sys = system(id);
                if (!sys) {
                    continue;
                }
                drawUpgrade(*sys, paneX + 39.f + slot * 66.f, 200, false);
                if (++slot == 8) {
                    break;
                }
            }
            for (; slot < 8; ++slot) {
                renderer_.draw(
                    "img/upgradeUI/upgrade_system_bar_none.png", paneX + 39.f + slot * 66.f, 200);
            }
            for (int id : {6, 7, 8, 12}) {
                const int subsystemSlot = id == 12 ? 3 : id - 6;
                if (const auto* sys = system(id)) {
                    drawUpgrade(*sys, paneX + 16.f + subsystemSlot * 66.f, 415, true);
                } else {
                    renderer_.draw("img/upgradeUI/upgrade_subsystem_bar_none.png",
                                   paneX + 16.f + subsystemSlot * 66.f,
                                   415);
                }
            }
            const bool reactorHovered = Rect{paneX + 305, 412, 272, 113}.contains(mouseX_, mouseY_);
            if (reactorHovered) {
                renderer_.windowFrame({paneX + 600, 152, 337, 135});
                text(texts_.get("upgrade_reactor", locale_), paneX + 618, 165, 16);
                std::istringstream words(texts_.get("reactor_desc", locale_));
                std::string word;
                std::string line;
                float y = 200;
                while (words >> word) {
                    auto next = line;
                    if (!next.empty()) {
                        next += ' ';
                    }
                    next += word;
                    if (!line.empty() &&
                        renderer_.measure(renderer_.font("fonts/JustinFont10.font"), next) > 303) {
                        text(line, paneX + 618, y, 10);
                        y += 17;
                        line = word;
                    } else {
                        line = next;
                    }
                }
                if (!line.empty()) {
                    text(line, paneX + 618, y, 10);
                }
            }
            renderer_.draw(root + (reactorHovered ? "equipment_reactor_select2.png"
                                                  : "equipment_reactor_on.png"),
                           paneX + 305,
                           412);
            for (int bar = 0; bar < 25; ++bar) {
                renderer_.fill(
                    {paneX + 335.f + (bar / 5) * 44.f, 426.f + (4 - bar % 5) * 12.f, 32, 8},
                    bar < ship_.reactor                        ? SDL_Color{80, 255, 85, 255}
                    : bar < ship_.reactor + pendingUpgrade(-1) ? SDL_Color{255, 230, 80, 255}
                                                               : SDL_Color{104, 99, 65, 255});
            }
            auto powerLabel = texts_.get("upgrade_reactor_power", locale_);
            const auto powerMarker = powerLabel.find("\\1");
            if (powerMarker != std::string::npos) {
                // ReactorButton::OnRender 0x5338f0 reserves two numeric glyphs,
                // then prints the localized suffix separately in font52.
                const auto suffix = powerLabel.substr(powerMarker + 2);
                auto& numberFont = renderer_.font("fonts/num_font.font");
                const float numberWidth = std::round(renderer_.measure(numberFont, "88"));
                const float suffixWidth =
                    std::round(renderer_.measure(localizedFont(52, suffix), suffix, textScale(52)));
                const float right =
                    paneX + 408 - int((numberWidth + suffixWidth) / 2) + numberWidth;
                const auto number = std::to_string(ship_.reactor + pendingUpgrade(-1));
                text(number, right - renderer_.measure(numberFont, number), 505, 0);
                text(suffix, right, 501, 52);
            } else {
                text(powerLabel, paneX + 408, 501, 52, true);
            }
            if (ship_.reactor + pendingUpgrade(-1) < 25) {
                text(std::to_string(reactorUpgradeCost(ship_.reactor + pendingUpgrade(-1))),
                     paneX + 540,
                     505,
                     0);
            } else {
                if (pendingUpgrade(-1) > 0) {
                    text("--", paneX + 538, 500, 52);
                } else {
                    text(texts_.get("upgrade_max", locale_), paneX + 543, 500, 52, true);
                }
            }
        } else if (panel_ == "crew") {
            const auto living = crewRoster();
            text(texts_.get("rename_note", locale_), paneX + 25, 140, 10);
            if (enemy_.rewardCrewPending || living.size() > 8) {
                // CrewManifest4da710 places its explanatory DropBox at(-275,100).
                const float x = paneX - 275;
                constexpr float y = 185;
                const auto title = texts_.get("overcapacity_crew_title", locale_);
                const auto body = texts_.get("overcapacity_crew_text", locale_);
                auto& art = renderer_.image("img/dropbox_on.png");
                const float width = static_cast<float>(art.w);
                const float titleInsert = title.find('\n') == std::string::npos ? 0.f : 24.f;
                const auto lines = wrappedText(52, body, width - 37);
                const float lineHeight = textLineHeight(52, body);
                const float insert = std::round(lineHeight * static_cast<float>(lines.size())) - 20;
                renderer_.sprite(art, {x, y, width, 32}, {0, 0, width, 32});
                renderer_.sprite(art, {x, y + 32, width, titleInsert + 1}, {0, 32, width, 1});
                renderer_.sprite(art, {x, y + 33 + titleInsert, width, 31}, {0, 33, width, 31});
                renderer_.sprite(
                    art, {x, y + 64 + titleInsert, width, insert + 1}, {0, 64, width, 1});
                renderer_.sprite(
                    art,
                    {x, y + 65 + titleInsert + insert, width, static_cast<float>(art.h) - 65},
                    {0, 65, width, static_cast<float>(art.h) - 65});
                const auto split = title.find('\n');
                if (split == std::string::npos) {
                    text(title, x + width / 2, y + 12, 63, true, {28, 21, 21, 255});
                } else {
                    text(
                        title.substr(0, split), x + width / 2, y + 10, 63, true, {28, 21, 21, 255});
                    text(title.substr(split + 1),
                         x + width / 2,
                         y + 38,
                         63,
                         true,
                         {28, 21, 21, 255});
                }
                float bodyY = y + titleInsert + 57;
                for (const auto& line : lines) {
                    text(line, x + width / 2, bodyY, 52, true);
                    bodyY += lineHeight;
                }
            }
            std::optional<RunCrew> hoveredCrew;
            // CrewManifest::OnRender4db730 performs all EquipmentBox bodies,
            // then all RenderLabels, then InfoBox and the full-bright dialog.
            for (int pass = 0; pass < 2; ++pass) {
                for (std::size_t i = 0;
                     i < (enemy_.rewardCrewPending || living.size() > 8 ? 9U : 8U);
                     ++i) {
                    const float cx =
                        paneX + 75.f + static_cast<float>(i % 3) * 170.f +
                        (i >= 6 && !enemy_.rewardCrewPending && living.size() <= 8 ? 86.f : 0.f);
                    const float cy = 173.f + static_cast<float>(i / 3) * 133.f;
                    const bool pending = enemy_.rewardCrewPending && i == living.size();
                    const int displayMember = pending             ? -2
                                              : i < living.size() ? static_cast<int>(living[i])
                                                                  : -1;
                    const bool cardSelected =
                        dismissCrew_ == -1 && (i < living.size() || pending) &&
                        (Rect{cx - 4, cy - 8, 104, 86}.contains(mouseX_, mouseY_) ||
                         renameCrew_ == displayMember);
                    RunCrew incoming;
                    incoming.appearance = {enemy_.rewardCrewName,
                                           enemy_.rewardCrewSpecies,
                                           enemy_.rewardCrewAppearance,
                                           -1,
                                           0,
                                           enemy_.rewardCrewFullName};
                    incoming.health = pending ? crewMaximumHealth(enemy_.rewardCrewSpecies) : 0;
                    if (pending) {
                        for (const auto& row : pendingVoyageEffects_) {
                            if (row.value("kind", std::string{}) != "crewSkills" ||
                                row.value("name", std::string{}) != incoming.appearance.name) {
                                continue;
                            }
                            applyVoyageCrewSkills(incoming, row);
                        }
                    }
                    const auto& member =
                        pending || i >= living.size() ? incoming : crew_[living[i]];
                    const int memberIndex = displayMember;
                    renderer_.setTint(confirmingCrew && dismissCrew_ != memberIndex ? .25f : 1.f);
                    if (pass == 0) {
                        renderer_.draw(root + "box_crew_" +
                                           (cardSelected                   ? "selected.png"
                                            : i < living.size() || pending ? "on.png"
                                                                           : "off.png"),
                                       cx,
                                       cy);
                        if (i >= living.size() && !pending) {
                            continue;
                        }
                        const SDL_Color dismissFace =
                            Rect{cx, cy + 67, 100, 20}.contains(mouseX_, mouseY_)
                                ? SDL_Color{255, 230, 80, 255}
                                : SDL_Color{235, 245, 229, 255};
                        renderer_.draw(std::string("img/customizeUI/box_crewcustom_") +
                                           (cardSelected ? "selected.png" : "on.png"),
                                       cx,
                                       cy + 64);
                        roundedButtonFace(renderer_, {cx + 4, cy + 72, 92, 12}, 2, dismissFace);
                        text(texts_.get("crewbox_dismiss", locale_),
                             cx + 50,
                             cy + 73,
                             51,
                             true,
                             pending || living.size() > 1 ? SDL_Color{25, 49, 51, 255}
                                                          : SDL_Color{130, 130, 130, 255});
                        const auto& portrait =
                            catalog_.animation(member.appearance.species + "_portrait");
                        painter_.crewSprite(member.appearance,
                                            cx + 50 - portrait.width,
                                            cy + 23 - portrait.height,
                                            2,
                                            false);
                        continue;
                    }
                    if (i >= living.size() && !pending) {
                        continue;
                    }
                    if (dismissCrew_ == -1 &&
                        (Rect{cx - 4, cy - 8, 104, 86}.contains(mouseX_, mouseY_) ||
                         renameCrew_ == memberIndex)) {
                        hoveredCrew = member;
                    }
                    if (renameCrew_ == memberIndex) {
                        renderer_.fill({cx - 25, cy + 44, 150, 23}, {0, 0, 0, 255});
                        renderer_.fill({cx - 25, cy + 44, 150, 1}, {255, 255, 255, 255});
                        renderer_.fill({cx - 25, cy + 66, 150, 1}, {255, 255, 255, 255});
                        renderer_.fill({cx - 25, cy + 44, 1, 23}, {255, 255, 255, 255});
                        renderer_.fill({cx + 124, cy + 44, 1, 23}, {255, 255, 255, 255});
                    }
                    const auto label =
                        renameCrew_ == memberIndex ? crewRenameText_ : member.appearance.name;
                    const bool ignoreLanguage = std::ranges::all_of(label, [](unsigned char byte) {
                        return byte <= 0xD1;
                    });
                    const int fontSize = renameCrew_ == memberIndex ? 10 : 6;
                    const bool chinese = locale_ == "zh-Hans" && !ignoreLanguage;
                    if (renameCrew_ == memberIndex) {
                        renderer_.textInput(label,
                                            crewRenameCaret_,
                                            fontSize,
                                            {cx + 50, cy + 49},
                                            crewRenameBlink_ < .5f,
                                            chinese);
                    } else {
                        auto& font = renderer_.sourceFont(fontSize, label, chinese);
                        renderer_.text(font,
                                       label,
                                       cx + 50,
                                       cy + 49,
                                       Renderer::sourceFontScale(fontSize, chinese),
                                       {235, 245, 229, 255},
                                       true);
                    }
                }
            }
            renderer_.setTint(confirmingCrew ? .25f : 1.f);
            if (hoveredCrew) {
                renderCrewInfo(*hoveredCrew, paneX + 600, 85);
            }
            renderer_.setTint(1);
            if (dismissCrew_ != -1 &&
                (dismissCrew_ == -2 || dismissCrew_ < static_cast<int>(crew_.size()))) {
                const int display =
                    dismissCrew_ == -2
                        ? static_cast<int>(living.size())
                        : static_cast<int>(
                              std::ranges::find(living, static_cast<std::size_t>(dismissCrew_)) -
                              living.begin());
                const float px =
                    paneX + 28.f + static_cast<float>(display % 3) * 170.f +
                    (display >= 6 && !enemy_.rewardCrewPending && living.size() <= 8 ? 86.f : 0.f);
                const float py = 266.f + static_cast<float>(display / 3) * 133.f;

                auto& frame = renderer_.image("img/confirm_window.png");
                const float fx = px - 7;
                const float fy = py - 7;
                const auto layout = crewConfirmationLayout(renderer_,
                                                           texts_.get("confirm_dismiss", locale_),
                                                           texts_.get("confirm_yes", locale_),
                                                           texts_.get("confirm_no", locale_),
                                                           locale_ == "zh-Hans");
                const std::array<float, 4> dx{
                    fx, fx + 70, fx + layout.width - 69, fx + layout.width};
                const std::array<float, 4> dy{
                    fy, fy + 26, fy + layout.height + 5, fy + layout.height + 22};
                const std::array<float, 4> sx{0, 70, 71, 140};
                const std::array<float, 4> sy{0, 26, 27, 44};
                for (std::size_t row = 0; row < 3; ++row) {
                    for (std::size_t col = 0; col < 3; ++col) {
                        renderer_.sprite(
                            frame,
                            {dx[col], dy[row], dx[col + 1] - dx[col], dy[row + 1] - dy[row]},
                            {sx[col], sy[row], sx[col + 1] - sx[col], sy[row + 1] - sy[row]});
                    }
                }
                const float wellY = fy + layout.height + 22;
                const float yesX = fx + layout.yesX;
                const float noX = fx + layout.noX;
                const float endX = noX + layout.noWidth;
                const auto wellStrip = [&](float x, float width, float sourceX, float sourceWidth) {
                    renderer_.sprite(frame, {x, wellY, width, 46}, {sourceX, 44, sourceWidth, 46});
                };
                // ConfirmWindow::OnRender0x544e30 stretches each button well
                // independently around the localized TextButton widths.
                wellStrip(fx, 20, 0, 20);
                wellStrip(fx + 20, layout.yesX - 41, 20, 1);
                wellStrip(yesX - 21, 31, 21, 31);
                wellStrip(yesX + 10, layout.yesWidth - 20, 52, 1);
                wellStrip(yesX + layout.yesWidth - 10, 34, 53, 34);
                wellStrip(noX + 10, layout.noWidth - 20, 87, 1);
                wellStrip(endX - 10, 31, 88, 31);
                wellStrip(
                    endX + 21, layout.width - 20 - (layout.noX + layout.noWidth + 21), 119, 1);
                wellStrip(fx + layout.width - 20, 20, 120, 20);
                float confirmationY = fy + 17;
                for (const auto& confirmationLine : layout.lines) {
                    text(confirmationLine, fx + layout.width / 2, confirmationY, 52, true);
                    confirmationY += textLineHeight(52, texts_.get("confirm_dismiss", locale_));
                }
                const float buttonY = fy + layout.height + 28;
                const auto confirmFace = [&](float bx, float width) {
                    const Rect rect{bx, buttonY, width, 24};
                    roundedButtonFace(renderer_,
                                      rect,
                                      4,
                                      rect.contains(mouseX_, mouseY_)
                                          ? SDL_Color{245, 239, 163, 255}
                                          : SDL_Color{235, 245, 229, 255});
                };
                confirmFace(yesX, layout.yesWidth);
                confirmFace(noX, layout.noWidth);
                text(texts_.get("confirm_yes", locale_),
                     yesX + layout.yesWidth / 2,
                     buttonY + 4,
                     62,
                     true,
                     {25, 49, 51, 255});
                text(texts_.get("confirm_no", locale_),
                     noX + layout.noWidth / 2,
                     buttonY + 4,
                     62,
                     true,
                     {25, 49, 51, 255});
            }
        } else if (panel_ == "equipment") {
            inventoryRender();
        } else {
            text(panel_ == "crew" ? "CREW" : "EQUIPMENT", 354, 187, 62);
            const auto buttons = voyageButtons();
            for (int row = 0; row < 10; ++row) {
                const auto index =
                    static_cast<std::size_t>(panelPage_) * 10 + static_cast<std::size_t>(row);
                if (index >= buttons.size()) {
                    break;
                }
                text(buttons[index].label,
                     365,
                     211.f + row * 33.f,
                     10,
                     false,
                     buttons[index].enabled ? SDL_Color{235, 245, 229, 255}
                                            : SDL_Color{130, 130, 130, 255});
            }
        }
        renderer_.setTint(1);
        return;
    }
    renderer_.fill({230, 80, 850, 550}, {18, 30, 35, 255});
    text(panel_ + " | Sector " + std::to_string(sector_) + " | Scrap " + std::to_string(scrap_),
         260,
         106,
         14);
    text("Close", 1010, 102, 10);
    const auto buttons = voyageButtons();
    for (int row = 0; row < 10; ++row) {
        const auto index =
            static_cast<std::size_t>(panelPage_) * 10 + static_cast<std::size_t>(row);
        if (index >= buttons.size()) {
            break;
        }
        const auto& button = buttons[index];
        const Rect rect{260, 160.f + 38.f * row, 760, 34};
        renderer_.fill(rect,
                       button.enabled ? SDL_Color{43, 70, 76, 255} : SDL_Color{35, 39, 40, 255});
        text(button.label,
             rect.x + 10,
             rect.y + 8,
             10,
             false,
             button.enabled ? SDL_Color{235, 245, 229, 255} : SDL_Color{125, 130, 125, 255});
    }
    text("Previous", 270, 577, 10);
    text("Next", 850, 577, 10);
    text(voyageMessage_, 260, 609, 9);
}
} // namespace ftl
