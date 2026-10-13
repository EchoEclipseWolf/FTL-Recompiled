#include "run.hpp"
#include <stdexcept>

namespace ftl {
void Run::prepareBeaconImages(pugi::xml_node node) {
    // CreateLocation 0x5f2be0 caches SwitchBackground/SwitchPlanet descriptors
    // before fleet, ship generation and environment overrides.
    const auto& beacon = graph_.beacons().at(static_cast<std::size_t>(graph_.current()));
    nlohmann::json row;
    if (beacon.imageCached) {
        row = {{"kind", "img"},
               {"back", beacon.backgroundImage},
               {"planet", beacon.planetImage},
               {"x", beacon.planetX},
               {"y", beacon.planetY},
               {"rotation", beacon.planetRotation}};
    } else {
        pugi::xml_document document;
        auto event = document.append_child("event");
        auto image = event.append_child("img");
        const auto authored = node.child("img");
        // EventsParser::ProcessEvent 0x5c11f0 initializes these exact lists.
        image.append_attribute("back") = authored.attribute("back").as_string("BACKGROUND");
        image.append_attribute("planet") = authored.attribute("planet").as_string("PLANET");
        EventResources resources;
        const auto images = stageVoyageEffects(event, eventRandom_, resources);
        if (images.size() != 1) {
            throw std::runtime_error("beacon_image_selection_failed");
        }
        row = images.front();
        graph_.setImages(graph_.current(), row);
    }
    applyVoyageImage(row);
    if (node) {
        auto image = node.child("img");
        if (!image) {
            image = node.append_child("img");
        }
        const nlohmann::json frozen{{"scrap", 0},
                                    {"fuel", 0},
                                    {"missiles", 0},
                                    {"drones", 0},
                                    {"effects", nlohmann::json::array({row})}};
        if (!image.attribute("_nativeVoyage")) {
            image.append_attribute("_nativeVoyage");
        }
        image.attribute("_nativeVoyage").set_value(frozen.dump().c_str());
    }
}

void Run::restoreBeaconEnvironment(pugi::xml_node node) {
    const auto environment = node.child("environment");
    if (!environment || std::string_view(environment.attribute("type").as_string()) == "normal") {
        return;
    }
    const std::string type = environment.attribute("type").as_string();
    setEnvironment(type == "storm"    ? "ion_storm"
                   : type == "normal" ? ""
                   : type == "PDS"    ? "pds"
                                      : type);
    if (type != "PDS") {
        return;
    }
    const std::string target = environment.attribute("target").as_string();
    environment_.pdsTarget = target == "enemy" ? 1 : target == "player" ? 0 : 2;
    if (environment_.fleet == 0) {
        std::vector<std::string> planets;
        for (auto image : events_.imageList("PLANET_POPULATED").children("img")) {
            planets.emplace_back(std::string("img/") + image.child_value());
        }
        if (planets.empty()) {
            throw std::runtime_error("PDS PLANET_POPULATED image list missing");
        }
        eventPlanet_ = planets[eventRandom_.next() % planets.size()];
        eventPlanetX_ = 600;
        eventPlanetY_ = 247;
        eventPlanetRotation_ = 180 * static_cast<int>(eventRandom_.next() % 2);
    }
    environment_.pdsAmbient = 2.f + static_cast<float>(eventRandom_.next() % 200) / 100.f;
}

void Run::restoreBeaconFleet(pugi::xml_node node) {
    if (auto fleet = node.child("fleet")) {
        const std::string value = fleet.child_value();
        environment_.fleet = value == "rebel" ? 1 : value == "fed" ? 2 : value == "battle" ? 3 : 0;
        environment_.fleetShips.clear();
        for (int i = 0; environment_.fleet != 0 && i < 9; ++i) {
            const int x = static_cast<int>(eventRandom_.next() % 1280) / 3 + 1280 * (i / 3) / 3;
            const int y = static_cast<int>(eventRandom_.next() % 720) / 3 + 720 * (i % 3) / 3;
            int image = static_cast<int>(eventRandom_.next() % 4);
            const bool federation =
                environment_.fleet == 2 ||
                (environment_.fleet == 3 && (i > 5 || (i > 2 && eventRandom_.next() % 2 == 1)));
            if (federation) {
                image += 4;
            }
            environment_.fleetShips.push_back({x, y, image});
        }
    }
}
} // namespace ftl
