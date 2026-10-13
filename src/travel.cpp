#include "travel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <pugixml.hpp>
#include <stdexcept>

namespace ftl {
std::uint32_t RandomStream::next() {
    auto& state = owner().state_;
    state = state * UINT64_C(0x5851f42d4c957f2d) + 1;
    return static_cast<std::uint32_t>(state >> 33);
}

void TravelGraph::connect() {
    for (std::size_t first = 0; first < beacons_.size(); ++first) {
        for (std::size_t second = first + 1; second < beacons_.size(); ++second) {
            auto& a = beacons_[first];
            auto& b = beacons_[second];
            const int dx = a.x - b.x;
            const int dy = a.y - b.y;
            if (std::abs(a.column - b.column) <= 1 && std::abs(a.row - b.row) <= 1 &&
                dx * dx + dy * dy <= 165 * 165) {
                a.neighbors.push_back(static_cast<int>(second));
                b.neighbors.push_back(static_cast<int>(first));
            }
        }
    }
}

bool TravelGraph::connected() const {
    if (beacons_.empty()) {
        return false;
    }
    std::vector<bool> seen(beacons_.size(), false);
    std::vector<int> pending{0};
    seen[0] = true;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        for (int neighbor : beacons_[pending[index]].neighbors) {
            if (!seen[neighbor]) {
                seen[neighbor] = true;
                pending.push_back(neighbor);
            }
        }
    }
    return pending.size() == beacons_.size();
}

void TravelGraph::generate(std::uint32_t seed) {
    RandomStream random(seed);
    generate(seed, random);
}

void TravelGraph::generate(std::uint32_t seed, RandomStream& random) {
    // GenerateMap 0x5d4590 resets the original global stream to the sector
    // seed; subsequent geometry, content and voyage draws share its state.
    random = RandomStream(seed);
    seed_ = seed;
    dangerX_ = -959;
    pursuitDelay_ = 0;
    nebulaMap_ = false;
    dangerY_ = static_cast<int>(random.next() % 250) + 50;
    regenerateGeometry(random);
}

void TravelGraph::regenerateGeometry(RandomStream& random) {
    // Exit/disconnected retries consume the same stream without reseeding or
    // redrawing danger-zone Y (GenerateMap LABEL_9).
    for (int attempt = 0; attempt < 256; ++attempt) {
        beacons_.clear();
        int populated = 0;
        int empty = 0;
        for (int column = 0; column < 6; ++column) {
            for (int row = 0; row < 4; ++row) {
                const bool skip = random.next() % 5 == 0 && populated / ++empty > 4;
                ++populated;
                if (skip) {
                    continue;
                }
                Beacon beacon;
                beacon.flashOffset = static_cast<float>(random.next() % 2001) / 1000;
                beacon.column = column;
                beacon.row = row;
                beacon.x = column * 110 + static_cast<int>(random.next() % 90) + 10;
                beacon.y = std::min(415, row * 110 + static_cast<int>(random.next() % 90) + 10);
                if (column > 3 && row == 0) {
                    beacon.y = std::max(30, beacon.y);
                }
                beacons_.push_back(std::move(beacon));
            }
        }
        connect();
        if (connected()) {
            const int startRow = static_cast<int>(random.next() % 4);
            auto start = std::find_if(beacons_.begin(), beacons_.end(), [startRow](const auto& b) {
                return b.column == 0 && b.row == startRow;
            });
            current_ = start == beacons_.end() ? 0 : static_cast<int>(start - beacons_.begin());
            beacons_[current_].visited = 1;
            revealCurrent();
            return;
        }
    }
    throw std::runtime_error("map_generation_exhausted");
}

bool TravelGraph::reachable(int destination) const {
    if (destination < 0 || destination >= static_cast<int>(beacons_.size()) ||
        destination == current_) {
        return false;
    }
    const auto& neighbors = beacons_[current_].neighbors;
    return std::find(neighbors.begin(), neighbors.end(), destination) != neighbors.end();
}

void TravelGraph::revealCurrent() {
    beacons_[current_].known = true;
    for (int neighbor : beacons_[current_].neighbors) {
        beacons_[neighbor].known = true;
    }
}

bool TravelGraph::visit(int destination,
                        bool advancePursuit,
                        const std::function<void(int)>& convert) {
    if (!reachable(destination)) {
        return false;
    }
    // 0x5d0f30 advances pursuit using the departing location, before changing currentLoc.
    if (advancePursuit) {
        advanceFleet(convert);
    }
    current_ = destination;
    // GetNewLocation 0x5d1310 / TravelToLocation 0x5c8760 clear questLoc on arrival.
    beacons_[current_].quest = false;
    ++beacons_[current_].visited;
    revealCurrent();
    return true;
}

int TravelGraph::nextFleetMove() const {
    if (dangerX_ >= 60 || pursuitDelay_ < 0) {
        return 0;
    }
    // GetNextDangerMove 0x5c85a0: a nebula sector's clouds slow pursuit
    // to 51 pixels, compared with 32 in an ordinary sector's clouds.
    return (beacons_.at(current_).nebula ? (nebulaMap_ ? 51 : 32) : 64) *
           (pursuitDelay_ > 0 ? 2 : 1);
}

void TravelGraph::advanceFleet(const std::function<void(int)>& convert) {
    dangerX_ = std::min(60, dangerX_ + nextFleetMove());
    pursuitDelay_ += pursuitDelay_ < 0 ? 1 : pursuitDelay_ > 0 ? -1 : 0;
    convertFleetLocations(convert);
}

void TravelGraph::convertFleetLocations(const std::function<void(int)>& convert) {
    // UpdateDangerZone 0x5d0f30 uses the circle's squared radius 767^2.
    for (std::size_t index = 0; index < beacons_.size(); ++index) {
        auto& beacon = beacons_[index];
        const auto dx = static_cast<std::int64_t>(beacon.x) - dangerX_;
        const auto dy = static_cast<std::int64_t>(beacon.y) - dangerY_;
        if (!beacon.fleet && dx * dx + dy * dy < 588289) {
            beacon.fleet = true;
            if (convert) {
                convert(static_cast<int>(index));
            }
        }
    }
}

void TravelGraph::wait(const std::function<void(int)>& convert) {
    // The completed waiting animation already assigned dangerZone.x. Source
    // goes directly to delay consumption/final scan, without moving it again.
    pursuitDelay_ += pursuitDelay_ < 0 ? 1 : pursuitDelay_ > 0 ? -1 : 0;
    convertFleetLocations(convert);
    waitMove_ = 0;
}

void TravelGraph::beginWait() {
    waitStart_ = dangerX_;
    waitMove_ = nextFleetMove();
}

void TravelGraph::waitProgress(float progress, bool scan, const std::function<void(int)>& convert) {
    // UpdateDangerZone5d0f30 recomputes from the prior dangerZone.x each time.
    // Its waiting branch does not clamp to60, including the original endpoint
    // fallback when the preceding animation sample passed that threshold.
    const int move = dangerX_ <= 59 ? nextFleetMove() : 0;
    dangerX_ =
        static_cast<int>(static_cast<float>(waitStart_) + std::clamp(progress, 0.f, 1.f) * move);
    if (scan) {
        convertFleetLocations(convert);
    }
}

void TravelGraph::setStore(int id, bool value) {
    beacons_.at(static_cast<std::size_t>(id)).store = value;
}

void TravelGraph::setDistress(int id, bool value) {
    beacons_.at(static_cast<std::size_t>(id)).distress = value;
}

void TravelGraph::setRepair(int id, bool value) {
    beacons_.at(static_cast<std::size_t>(id)).repair = value;
}

void TravelGraph::revealAll() {
    for (auto& beacon : beacons_) {
        beacon.known = true;
    }
}

int TravelGraph::addQuest(const std::string& event, bool force, RandomStream& random) {
    // AddQuest 0x5d4100 excludes visited, nebula, exit, pursuit, existing
    // quest and store locations; prefer a reachable destination before pursuit.
    // Dijkstra 0x5d3920 assigns every edge cost one and includes all locations,
    // so BFS yields exactly the path length used here, including captured
    // intermediate locations. Its returned vector includes start and finish.
    std::vector<int> distance(beacons_.size(), -1);
    std::vector<int> pending{current_};
    distance[current_] = 0;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        for (int neighbor : beacons_[pending[index]].neighbors) {
            if (distance[neighbor] < 0) {
                distance[neighbor] = distance[pending[index]] + 1;
                pending.push_back(neighbor);
            }
        }
    }
    std::vector<int> choices;
    for (std::size_t index = 0; index < beacons_.size(); ++index) {
        const auto& beacon = beacons_[index];
        if (beacon.visited || beacon.nebula || beacon.exit || beacon.fleet || beacon.quest ||
            beacon.store || beacon.distress || static_cast<int>(index) == current_) {
            continue;
        }
        const float dx = static_cast<float>(beacon.x - dangerX_);
        const float dy = static_cast<float>(beacon.y - dangerY_);
        const int safeJumps = static_cast<int>((std::sqrt(dx * dx + dy * dy) - 767.f) / 64.f - 1.f);
        if ((distance[index] > 0 && force) ||
            (distance[index] > 0 && safeJumps > distance[index] + 1)) {
            choices.push_back(static_cast<int>(index));
        }
    }
    if (choices.empty()) {
        return -1;
    }
    const int choice = choices[random.next() % choices.size()];
    auto& beacon = beacons_[choice];
    beacon.event = event;
    beacon.eventXml.clear();
    beacon.savedShip = nullptr;
    beacon.quest = true;
    beacon.known = true;
    return choice;
}

int TravelGraph::chooseExit(RandomStream& random, int finalDifficulty) const {
    std::vector<int> distance(beacons_.size(), -1);
    std::vector<int> pending{current_};
    distance[current_] = 0;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        for (int neighbor : beacons_[pending[index]].neighbors) {
            if (distance[neighbor] < 0) {
                distance[neighbor] = distance[pending[index]] + 1;
                pending.push_back(neighbor);
            }
        }
    }
    // GenerateMap 0x5d4590 draws row 0..3 and column 4..5, requiring
    // a path with more than four locations (including the starting beacon).
    int evaluated = 0;
    generationEvaluations_ = 0;
    for (int sample = 0; sample < 4096 && evaluated < 16; ++sample) {
        const int row = static_cast<int>(random.next() % 4);
        int column = static_cast<int>(random.next() % 2) + 4;
        if (finalDifficulty >= 0) {
            column = static_cast<int>(random.next() % 2) + (finalDifficulty == 2 ? 3 : 2);
        }
        for (std::size_t index = 0; index < beacons_.size(); ++index) {
            if (beacons_[index].row == row && beacons_[index].column == column) {
                // Empty grid draws retry without increasing the source count.
                ++evaluated;
                // The path size (start included) must lie strictly between
                // minDistance and maxDistance: 4/100 for an ordinary sector,
                // 3/5 for the final sector, 4/7 on hard (unsigned compares).
                const int count = distance[index] + 1;
                const int minimum = finalDifficulty < 0 ? 4 : finalDifficulty == 2 ? 4 : 3;
                const int maximum = finalDifficulty < 0 ? 100 : finalDifficulty == 2 ? 7 : 5;
                if (distance[index] >= 0 && count > minimum && count < maximum) {
                    generationEvaluations_ = evaluated;
                    return static_cast<int>(index);
                }
                break;
            }
        }
    }
    return -1;
}

std::vector<int> TravelGraph::chooseBossPath(RandomStream& random, int base) const {
    if (base < 0 || base >= int(beacons_.size())) {
        throw std::runtime_error("invalid_boss_base");
    }
    // GenerateMap0x5d4590 samples populated right-side grid cells. Empty
    // draws do not count. The counter continues from the exit search; a
    // failed candidate once it exceeds fifteen regenerates the whole map.
    int evaluated = generationEvaluations_;
    for (int attempt = 0; attempt < 4096; ++attempt) {
        const int row = int(random.next() % 4);
        const int column = int(random.next() % 2) + 4;
        for (std::size_t index = 0; index < beacons_.size(); ++index) {
            const auto& beacon = beacons_[index];
            if (beacon.row == row && beacon.column == column) {
                ++evaluated;
                auto path = shortestPath(int(index), base);
                if (path.size() >= 4 && path.size() <= 6) {
                    return path;
                }
                if (evaluated > 15) {
                    return {};
                }
                break;
            }
        }
    }
    return {};
}

std::vector<int> TravelGraph::shortestPath(int start, int finish) const {
    std::vector<int> previous(beacons_.size(), -1);
    std::vector<int> queue{start};
    previous.at(static_cast<std::size_t>(start)) = start;
    for (std::size_t index = 0; index < queue.size(); ++index) {
        for (int neighbor : beacons_.at(static_cast<std::size_t>(queue[index])).neighbors) {
            if (previous.at(static_cast<std::size_t>(neighbor)) < 0) {
                previous[neighbor] = queue[index];
                queue.push_back(neighbor);
            }
        }
    }
    if (previous.at(static_cast<std::size_t>(finish)) < 0) {
        return {};
    }
    std::vector<int> path{finish};
    while (path.back() != start) {
        path.push_back(previous[path.back()]);
    }
    std::reverse(path.begin(), path.end());
    return path;
}

void TravelGraph::modifyPursuit(int amount) {
    if (amount < -100 || amount > 100) {
        throw std::runtime_error("invalid_pursuit_modifier");
    }
    pursuitDelay_ = std::clamp(pursuitDelay_ + amount, -100, 100);
}

void TravelGraph::setImages(int id, const nlohmann::json& row) {
    auto& beacon = beacons_.at(static_cast<std::size_t>(id));
    beacon.imageCached = true;
    beacon.backgroundImage = row.value("back", std::string{});
    beacon.planetImage = row.value("planet", std::string{});
    beacon.planetX = row.value("x", 0);
    beacon.planetY = row.value("y", 0);
    beacon.planetRotation = row.value("rotation", 0);
}

void TravelGraph::setEventXml(int id, std::string xml) {
    auto& beacon = beacons_.at(static_cast<std::size_t>(id));
    pugi::xml_document event;
    if (!xml.empty() && !event.load_string(xml.c_str())) {
        throw std::runtime_error("invalid_prepared_beacon_event");
    }
    beacon.repair = static_cast<bool>(event.document_element().child("repair"));
    beacon.eventXml = std::move(xml);
}

void TravelGraph::setEvent(int id, const std::string& event, bool nebula, bool exit) {
    auto& beacon = beacons_.at(static_cast<std::size_t>(id));
    beacon.event = event;
    beacon.eventXml.clear();
    beacon.repair = false;
    beacon.savedShip = nullptr;
    beacon.nebula = nebula;
    beacon.exit = exit;
}

nlohmann::json TravelGraph::state() const {
    auto beacons = nlohmann::json::array();
    for (std::size_t index = 0; index < beacons_.size(); ++index) {
        const auto& beacon = beacons_[index];
        beacons.push_back(
            {{"id", index},
             {"position", {beacon.x, beacon.y}},
             {"neighbors", beacon.neighbors},
             {"visited", beacon.visited},
             {"known", beacon.known},
             {"nebula", beacon.nebula},
             {"exit", beacon.exit},
             {"store", beacon.store},
             {"quest", beacon.quest},
             {"fleet", beacon.fleet},
             {"distress", beacon.distress},
             {"repair", beacon.repair},
             {"event", beacon.event},
             {"event_prepared", !beacon.eventXml.empty()},
             {"image_cached", beacon.imageCached},
             {"background_image", beacon.backgroundImage},
             {"planet_image", beacon.planetImage},
             {"planet_position", {beacon.planetX, beacon.planetY, beacon.planetRotation}}});
    }
    return {{"seed", seed_},
            {"current", current_},
            {"danger_y", dangerY_},
            {"danger_x", dangerX_},
            {"pursuit_delay", pursuitDelay_},
            {"nebula_sector", nebulaMap_},
            {"next_fleet_move", nextFleetMove()},
            {"beacons", beacons}};
}

void SectorGraph::generate(RandomStream& random,
                           const std::function<void(SectorNode&)>& selectName) {
    nodes_.clear();
    current_ = 0;
    auto drawType = [&random] {
        const auto value = random.next() % 10;
        return value < 2 ? 2 : value > 5 ? 1 : 0;
    };
    // The constructor draws a category for the start, subsequently replaced
    // by CIVILIAN_SECTOR in GenerateSectorMap.
    drawType();
    nodes_.push_back({.level = 0,
                      .row = 75,
                      .type = 0,
                      .visited = true,
                      .name = "CIVILIAN_SECTOR",
                      .title = "sectorname_CIVILIAN_SECTOR",
                      .shortTitle = "sectorname_short_CIVILIAN_SECTOR",
                      .neighbors = {}});
    std::vector<int> previous{0};
    for (int level = 1; level < 7; ++level) {
        const auto lastCount = static_cast<int>(previous.size());
        int count = 0;
        do {
            count = static_cast<int>(random.next() % 3) + 2;
        } while (count == lastCount);
        const int initialRow = lastCount == 1 ? 50 : 25 * (4 - count);
        if (lastCount == 1) {
            count = 2;
        }
        std::vector<int> column;
        for (int row = 0; row < count; ++row) {
            column.push_back(static_cast<int>(nodes_.size()));
            nodes_.push_back({.level = level,
                              .row = initialRow + row * 50,
                              .type = drawType(),
                              .visited = false,
                              .name = {},
                              .title = {},
                              .shortTitle = {},
                              .neighbors = {}});
        }
        if (lastCount == 4 && count == 2) {
            for (int row = 0; row < 4; ++row) {
                nodes_[previous[row]].neighbors.push_back(column[row / 2]);
            }
        } else if (lastCount == 2 && count == 4) {
            for (int row = 0; row < 4; ++row) {
                nodes_[previous[row / 2]].neighbors.push_back(column[row]);
            }
        } else if (count > lastCount) {
            // Splitting adds a second child to the first parent, then each
            // later parent shares its preceding parent's last child.
            nodes_[previous[0]].neighbors = {column[0], column[1]};
            for (int row = 1; row < lastCount; ++row) {
                nodes_[previous[row]].neighbors = {column[row], column[row + 1]};
            }
        } else {
            for (int row = 0; row < lastCount; ++row) {
                if (row > 0) {
                    nodes_[previous[row]].neighbors.push_back(column[row - 1]);
                }
                if (row < count) {
                    nodes_[previous[row]].neighbors.push_back(column[row]);
                }
            }
        }
        for (int id : column) {
            auto& node = nodes_[id];
            selectName(node);
        }
        previous = std::move(column);
    }
    drawType();
    const int final = static_cast<int>(nodes_.size());
    nodes_.push_back({.level = 7,
                      .row = 75,
                      .type = 1,
                      .visited = false,
                      .name = "FINAL",
                      .title = "sectorname_FINAL",
                      .shortTitle = "sectorname_short_FINAL",
                      .neighbors = {}});
    for (int parent : previous) {
        nodes_[parent].neighbors.push_back(final);
    }
}

bool SectorGraph::select(int choice) {
    const auto& choices = current().neighbors;
    if (choice < 0 || choice >= static_cast<int>(choices.size())) {
        return false;
    }
    current_ = choices[choice];
    nodes_[current_].visited = true;
    return true;
}
} // namespace ftl
