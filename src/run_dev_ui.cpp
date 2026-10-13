#include "run.hpp"
#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace ftl {
namespace {
struct Field {
    const char* key;
    const char* label;
    const char* initial;
};

struct Button {
    const char* action;
    const char* label;
};

constexpr std::array tabs{"Resources",
                          "Equipment",
                          "Crew",
                          "Systems",
                          "Limits",
                          "Encounters",
                          "Events",
                          "Hazards",
                          "Save Export"};
constexpr std::size_t tabColumns = 5;
constexpr std::array systemNames{"Shields",
                                 "Engines",
                                 "Oxygen",
                                 "Weapons",
                                 "Drones",
                                 "Medbay",
                                 "Piloting",
                                 "Sensors",
                                 "Doors",
                                 "Teleporter",
                                 "Cloaking",
                                 "Artillery",
                                 "Battery",
                                 "Clone Bay",
                                 "Mind Control",
                                 "Hacking"};
const std::array<std::vector<Field>, tabs.size()> fields{{
    {{{"resource", "Resource", "scrap"},
      {"amount", "Amount", "10"},
      {"hull", "Hull (current)", "30"}}},
    {{{"item", "Blueprint ID", "LASER_BURST_2"}, {"destination", "Destination", "equip"}}},
    {{{"species", "Species", "human"}, {"name", "Name", "Developer"}, {"room", "Room ID", "0"}}},
    {{{"system", "System ID", "0"},
      {"level", "System level", "3"},
      {"reactor", "Reactor bars", "12"}}},
    {{{"limit", "Limit", "hull"}, {"value", "Value", "30"}}},
    {{{"ship", "Enemy ship ID", "REBEL_SKINNY"}}},
    {{{"event", "Archived event ID", "REFUEL_STATION"}, {"beacon", "Beacon ID", "0"}}},
    {{{"side", "Ion target", "player"},
      {"ion_system", "System ID", "1"},
      {"stacks", "Ion stacks (1-5)", "1"}}},
    // The default folder is resolved at run time (original_save::defaultSaveFolder).
    {{{"original_folder", "Original save folder", ""}}},
}};
const std::array<std::vector<Button>, tabs.size()> buttons{{
    {{{"resource:next", "NEXT RESOURCE"},
      {"resource:add", "ADD AMOUNT"},
      {"resource:set", "SET AMOUNT"},
      {"hull:set", "SET HULL"}}},
    {{{"item:next", "NEXT ITEM"},
      {"destination:toggle", "EQUIP / CARGO"},
      {"item:grant", "GRANT ITEM"}}},
    {{{"species:next", "NEXT SPECIES"}, {"crew:spawn", "SPAWN CREW"}}},
    {{{"system:next", "NEXT SYSTEM"},
      {"system:apply", "INSTALL / SET SYSTEM"},
      {"reactor:apply", "SET REACTOR"}}},
    {{{"limit:next", "NEXT LIMIT"}, {"limit:apply", "APPLY LIMIT"}}},
    {{{"enemy:random", "RANDOM ENEMY"},
      {"enemy:next", "NEXT TESTED SHIP"},
      {"enemy:spawn", "SPAWN SHIP ID"}}},
    {{{"event:next", "NEXT ARCHIVED EVENT"},
      {"event:start", "START EVENT"},
      {"event:assign", "ASSIGN BEACON"},
      {"event:both", "START + ASSIGN"}}},
    {{{"hazard:ion_storm", "ION STORM"},
      {"hazard:nebula", "NEBULA"},
      {"hazard:sun", "SUN"},
      {"hazard:asteroid", "ASTEROIDS"},
      {"hazard:none", "CLEAR HAZARD"},
      {"side:toggle", "PLAYER / ENEMY"},
      {"ion_system:next", "NEXT SYSTEM"},
      {"ion:apply", "APPLY ION"}}},
    {{{"original:export", "EXPORT TO ORIGINAL"},
      {"original:default", "DEFAULT FOLDER"},
      {"original:relaunch", "CLOSE AND RELAUNCH ORIG"}}},
}};

std::string value(const std::map<std::string, std::string>& values, std::string_view key) {
    const auto found = values.find(std::string(key));
    if (found != values.end()) {
        return found->second;
    }
    if (key == "original_folder") {
        return original_save::defaultSaveFolder().string();
    }
    for (const auto& tab : fields) {
        for (const auto& field : tab) {
            if (field.key == key) {
                return field.initial;
            }
        }
    }
    return {};
}

int number(const std::map<std::string, std::string>& values, std::string_view key) {
    const auto raw = value(values, key);
    std::size_t consumed = 0;
    const int result = std::stoi(raw, &consumed);
    if (consumed != raw.size()) {
        throw std::runtime_error("Enter a whole number for " + std::string(key));
    }
    return result;
}

template <std::size_t N>
void cycle(std::map<std::string, std::string>& values,
           const char* key,
           const std::array<const char*, N>& choices) {
    const auto current = value(values, key);
    const auto found = std::ranges::find_if(choices, [&](const char* choice) {
        return choice == current;
    });
    values[key] =
        found == choices.end() || found + 1 == choices.end() ? choices.front() : *(found + 1);
}
} // namespace

nlohmann::json Run::devUiState() const {
    nlohmann::json values = nlohmann::json::object();
    for (const auto& tab : fields) {
        for (const auto& field : tab) {
            values[field.key] = value(devUiFields_, field.key);
        }
    }
    return {{"open", devUiOpen_},
            {"tab", tabs[std::size_t(devUiTab_)]},
            {"focus", devUiFocus_},
            {"status", devUiStatus_},
            {"fields", values}};
}

void Run::devUiAction(const std::string& action) {
    if (action == "toggle") {
        devUiOpen_ = !devUiOpen_;
        devUiFocus_.clear();
        dragging_ = false;
        return;
    }
    if (!devUiOpen_) {
        throw std::runtime_error("developer_ui_closed");
    }
    if (action == "close") {
        devUiOpen_ = false;
        devUiFocus_.clear();
        return;
    }
    if (action.starts_with("tab:")) {
        const auto found = std::ranges::find(tabs, action.substr(4));
        if (found == tabs.end()) {
            throw std::runtime_error("developer_ui_unknown_tab");
        }
        devUiTab_ = static_cast<int>(found - tabs.begin());
        devUiFocus_.clear();
        devUiStatus_.clear();
        return;
    }
    if (action.starts_with("field:")) {
        const auto key = action.substr(6);
        const auto& active = fields[std::size_t(devUiTab_)];
        if (std::ranges::none_of(active, [&](const Field& field) {
                return key == field.key;
            })) {
            throw std::runtime_error("developer_ui_unknown_field");
        }
        devUiFocus_ = key;
        devUiFields_.try_emplace(key, value(devUiFields_, key));
        return;
    }
    if (!action.starts_with("action:")) {
        throw std::runtime_error("developer_ui_unknown_action");
    }
    const auto name = action.substr(7);
    const auto& active = buttons[std::size_t(devUiTab_)];
    if (std::ranges::none_of(active, [&](const Button& button) {
            return name == button.action;
        })) {
        throw std::runtime_error("developer_ui_unknown_action");
    }
    devUiFocus_.clear();
    devUiStatus_.clear();
    try {
        nlohmann::json request;
        if (name == "resource:next") {
            cycle(devUiFields_, "resource", std::array{"scrap", "fuel", "missiles", "drone_parts"});
            return;
        }
        if (name == "item:next") {
            const auto ids = catalog_.equipmentIds();
            if (ids.empty()) {
                throw std::runtime_error("No equipment blueprints available");
            }
            const auto found = std::ranges::find(ids, value(devUiFields_, "item"));
            devUiFields_["item"] =
                found == ids.end() || found + 1 == ids.end() ? ids.front() : *(found + 1);
            return;
        }
        if (name == "species:next") {
            cycle(devUiFields_,
                  "species",
                  std::array{
                      "human", "engi", "mantis", "rock", "slug", "energy", "anaerobic", "crystal"});
            return;
        }
        if (name == "enemy:next") {
            cycle(devUiFields_,
                  "ship",
                  std::array{"REBEL_SKINNY", "REBEL_TRANSPORT", "AUTO_ASSAULT", "AUTO_BASIC"});
            return;
        }
        if (name == "event:next") {
            const auto ids = events_.eventIds();
            if (ids.empty()) {
                throw std::runtime_error("No archived events available");
            }
            const auto found = std::ranges::find(ids, value(devUiFields_, "event"));
            devUiFields_["event"] =
                found == ids.end() || found + 1 == ids.end() ? ids.front() : *(found + 1);
            return;
        }
        if (name == "system:next" || name == "ion_system:next") {
            const auto key = name == "system:next" ? "system" : "ion_system";
            int next = 0;
            try {
                next = (number(devUiFields_, key) + 1) % int(systemNames.size());
            } catch (const std::exception&) {
            }
            devUiFields_[key] = std::to_string(next);
            return;
        }
        if (name == "limit:next") {
            cycle(devUiFields_,
                  "limit",
                  std::array{"hull", "crew", "reactor", "weapon_slots", "drone_slots"});
            return;
        }
        if (name == "original:default") {
            devUiFields_["original_folder"] = original_save::defaultSaveFolder().string();
            return;
        }
        if (name == "original:export") {
            const auto result = exportOriginalSave(value(devUiFields_, "original_folder"), true);
            devUiStatus_ = "Exported " + result.at("path").get<std::string>();
            return;
        }
        if (name == "original:relaunch") {
            original_save::relaunchOriginalGame();
            return;
        }
        if (name == "destination:toggle") {
            devUiFields_["destination"] =
                value(devUiFields_, "destination") == "equip" ? "cargo" : "equip";
            return;
        }
        if (name == "side:toggle") {
            devUiFields_["side"] = value(devUiFields_, "side") == "player" ? "enemy" : "player";
            return;
        }
        if (name == "resource:add" || name == "resource:set") {
            const auto key = value(devUiFields_, "resource");
            request = {
                {"op", "resources"},
                {name == "resource:add" ? "add" : "set", {{key, number(devUiFields_, "amount")}}}};
        } else if (name == "hull:set") {
            request = {{"op", "resources"}, {"hull", number(devUiFields_, "hull")}};
        } else if (name == "item:grant") {
            request = {{"op", "grant"},
                       {"item", value(devUiFields_, "item")},
                       {"to", value(devUiFields_, "destination")}};
        } else if (name == "crew:spawn") {
            request = {{"op", "crew"},
                       {"species", value(devUiFields_, "species")},
                       {"name", value(devUiFields_, "name")},
                       {"room", number(devUiFields_, "room")}};
        } else if (name == "system:apply") {
            request = {{"op", "system"},
                       {"system", number(devUiFields_, "system")},
                       {"level", number(devUiFields_, "level")}};
        } else if (name == "reactor:apply") {
            request = {{"op", "reactor"}, {"level", number(devUiFields_, "reactor")}};
        } else if (name == "limit:apply") {
            request = {{"op", "limit"},
                       {"name", value(devUiFields_, "limit")},
                       {"value", number(devUiFields_, "value")}};
        } else if (name == "enemy:random" || name == "enemy:spawn") {
            request = {{"op", "enemy"},
                       {"ship", name == "enemy:random" ? "random" : value(devUiFields_, "ship")}};
        } else if (name == "event:start" || name == "event:assign" || name == "event:both") {
            request = {{"op", "event"},
                       {"event", value(devUiFields_, "event")},
                       {"start", name != "event:assign"}};
            if (name != "event:start") {
                request["beacon"] = number(devUiFields_, "beacon");
            }
        } else if (name.starts_with("hazard:")) {
            request = {{"op", "hazard"}, {"type", name.substr(7)}};
        } else if (name == "ion:apply") {
            request = {{"op", "ion"},
                       {"side", value(devUiFields_, "side")},
                       {"system", number(devUiFields_, "ion_system")},
                       {"stacks", number(devUiFields_, "stacks")}};
        }
        devCommand(request);
        devUiStatus_ = "Applied " + name;
    } catch (const std::exception& error) {
        devUiStatus_ = std::string("Error: ") + error.what();
    }
}

void Run::devUiClick(float x, float y) {
    if (Rect{1030, 76, 88, 36}.contains(x, y)) {
        devUiAction("close");
        return;
    }
    for (std::size_t index = 0; index < tabs.size(); ++index) {
        const float bx = 166.f + float(index % tabColumns) * 190;
        const float by = 124.f + float(index / tabColumns) * 39;
        if (Rect{bx, by, 180, 33}.contains(x, y)) {
            devUiAction("tab:" + std::string(tabs[index]));
            return;
        }
    }
    const auto& activeFields = fields[std::size_t(devUiTab_)];
    for (std::size_t index = 0; index < activeFields.size(); ++index) {
        const float bx = 172.f + float(index % 2) * 478;
        const float by = 252.f + float(index / 2) * 73;
        if (Rect{bx, by, 450, 35}.contains(x, y)) {
            devUiAction("field:" + std::string(activeFields[index].key));
            return;
        }
    }
    const auto& activeButtons = buttons[std::size_t(devUiTab_)];
    for (std::size_t index = 0; index < activeButtons.size(); ++index) {
        const float bx = 172.f + float(index % 3) * 318;
        const float by = 464.f + float(index / 3) * 52;
        if (Rect{bx, by, 302, 40}.contains(x, y)) {
            devUiAction("action:" + std::string(activeButtons[index].action));
            return;
        }
    }
    devUiFocus_.clear();
}

void Run::devUiKey(const std::string& key) {
    if (key == "Escape") {
        devUiAction("close");
    } else if (key == "Backspace" && !devUiFocus_.empty()) {
        auto& current = devUiFields_[devUiFocus_];
        if (!current.empty()) {
            current.pop_back();
        }
    } else if (key == "Delete" && !devUiFocus_.empty()) {
        devUiFields_[devUiFocus_].clear();
    } else if (key == "Enter" && !devUiFocus_.empty()) {
        devUiFocus_.clear();
    }
}

void Run::devUiText(const std::string& value) {
    if (devUiFocus_.empty()) {
        return;
    }
    auto& current = devUiFields_[devUiFocus_];
    for (const unsigned char character : value) {
        const std::size_t limit = devUiFocus_ == "original_folder" ? 260 : 64;
        if (character >= 32 && character != 127 && current.size() < limit) {
            current += static_cast<char>(character);
        }
    }
}

void Run::devUiRender() {
    constexpr SDL_Color border{183, 230, 215, 255};
    constexpr SDL_Color ink{236, 246, 232, 255};
    renderer_.fill({0, 0, 1280, 720}, {0, 0, 0, 185});
    renderer_.fill({148, 63, 984, 585}, border);
    renderer_.fill({152, 67, 976, 577}, {20, 34, 38, 250});
    text("DEVELOPER CONTROLS", 172, 82, 14, false, ink);
    text("F12 / ESC", 1075, 84, 9, true, ink);
    renderer_.fill({1030, 76, 88, 36}, {70, 45, 45, 255});
    text("CLOSE", 1074, 84, 10, true, ink);
    for (std::size_t index = 0; index < tabs.size(); ++index) {
        const float x = 166.f + float(index % tabColumns) * 190;
        const float y = 124.f + float(index / tabColumns) * 39;
        renderer_.fill({x, y, 180, 33},
                       int(index) == devUiTab_ ? SDL_Color{65, 115, 108, 255}
                                               : SDL_Color{37, 60, 63, 255});
        text(tabs[index], x + 90, y + 5, 10, true, ink);
    }
    const auto& activeFields = fields[std::size_t(devUiTab_)];
    for (std::size_t index = 0; index < activeFields.size(); ++index) {
        const auto& field = activeFields[index];
        const float x = 172.f + float(index % 2) * 478;
        const float y = 252.f + float(index / 2) * 73;
        std::string label = field.label;
        if (std::string_view(field.key) == "system" ||
            std::string_view(field.key) == "ion_system") {
            try {
                const int id = number(devUiFields_, field.key);
                if (id >= 0 && id < int(systemNames.size())) {
                    label += " (" + std::string(systemNames[std::size_t(id)]) + ")";
                }
            } catch (const std::exception&) {
            }
        }
        text(label, x + 2, y - 25, 10, false, ink);
        renderer_.fill({x, y, 450, 35},
                       devUiFocus_ == field.key ? SDL_Color{111, 170, 151, 255}
                                                : SDL_Color{92, 129, 127, 255});
        renderer_.fill({x + 2, y + 2, 446, 31}, {8, 20, 25, 255});
        auto displayed = value(devUiFields_, field.key);
        if (displayed.size() > 40) {
            displayed = displayed.substr(displayed.size() - 40);
        }
        text(displayed + (devUiFocus_ == field.key ? "|" : ""), x + 10, y + 6, 10, false, ink);
    }
    const auto& activeButtons = buttons[std::size_t(devUiTab_)];
    for (std::size_t index = 0; index < activeButtons.size(); ++index) {
        const float x = 172.f + float(index % 3) * 318;
        const float y = 464.f + float(index / 3) * 52;
        renderer_.fill({x, y, 302, 40}, {80, 133, 120, 255});
        renderer_.fill({x + 2, y + 2, 298, 36}, {37, 65, 65, 255});
        text(activeButtons[index].label, x + 151, y + 11, 10, true, ink);
    }
    text("Click a field to edit. Buttons apply immediately to this run.", 172, 576, 10, false, ink);
    if (!devUiStatus_.empty()) {
        const SDL_Color statusColor = devUiStatus_.starts_with("Error:")
                                          ? SDL_Color{255, 148, 138, 255}
                                          : SDL_Color{135, 255, 161, 255};
        text(devUiStatus_.substr(0, 95), 172, 605, 10, false, statusColor);
    }
}
} // namespace ftl
