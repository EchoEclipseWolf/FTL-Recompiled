#pragma once
#include "native_json.hpp"
#include <cstdint>
#include <functional>
#include <json.hpp>
#include <string>
#include <vector>

namespace ftl {
// Original RNG 0x62ede0/0x62f000. Unsigned overflow is intentional and portable.
class RandomStream {
    std::uint64_t state_;
    RandomStream* source_ = nullptr;

    RandomStream& owner() {
        return source_ ? source_->owner() : *this;
    }

    const RandomStream& owner() const {
        return source_ ? source_->owner() : *this;
    }

public:
    explicit RandomStream(std::uint32_t seed) : state_(seed) {}

    // Copies are independent snapshots. Assignment commits a state while
    // retaining the destination's explicit session binding, including swaps.
    RandomStream(const RandomStream& other) : state_(other.state()) {}

    RandomStream(RandomStream&& other) noexcept : state_(other.state()) {}

    RandomStream& operator=(const RandomStream& other) {
        owner().state_ = other.state();
        return *this;
    }

    RandomStream& operator=(RandomStream&& other) noexcept {
        owner().state_ = other.state();
        return *this;
    }

    void bind(RandomStream& source) {
        if (&source.owner() == this) {
            throw std::runtime_error("cyclic_random_stream_binding");
        }
        source_ = &source;
    }

    bool sharesStateWith(const RandomStream& other) const {
        return &owner() == &other.owner();
    }

    std::uint32_t next();

    std::uint64_t state() const {
        return owner().state_;
    }

    friend void to_json(nlohmann::json& json, const RandomStream& random) {
        json = {{"state_", random.state()}};
    }

    friend void from_json(const nlohmann::json& json, RandomStream& random) {
        nativeRead(json.at("state_"), random.owner().state_);
    }
};

struct Beacon {
    int x = 0;
    int y = 0;
    int column = 0;
    int row = 0;
    int visited = 0;
    float flashOffset = 0;
    bool known = false;
    bool nebula = false;
    bool exit = false;
    bool store = false;
    bool quest = false;
    bool fleet = false;
    bool distress = false;
    bool repair = false;
    std::string event;
    std::string eventXml;
    nlohmann::json savedShip;
    bool imageCached = false;
    std::string backgroundImage, planetImage;
    int planetX = 0, planetY = 0, planetRotation = 0;
    std::vector<int> neighbors;

    friend void to_json(nlohmann::json& nlohmann_json_j, const Beacon& nlohmann_json_t) {
        nlohmann_json_j["repair"] = nlohmann_json_t.repair;
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 x,
                                                 y,
                                                 column,
                                                 row,
                                                 visited,
                                                 flashOffset,
                                                 known,
                                                 nebula,
                                                 exit,
                                                 store,
                                                 quest,
                                                 fleet,
                                                 distress,
                                                 event,
                                                 eventXml,
                                                 savedShip,
                                                 imageCached,
                                                 backgroundImage,
                                                 planetImage,
                                                 planetX,
                                                 planetY,
                                                 planetRotation,
                                                 neighbors))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j, Beacon& nlohmann_json_t) {
        const Beacon nlohmann_json_default_obj{};
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, repair))
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM,
                                                 x,
                                                 y,
                                                 column,
                                                 row,
                                                 visited,
                                                 flashOffset,
                                                 known,
                                                 nebula,
                                                 exit,
                                                 store,
                                                 quest,
                                                 fleet,
                                                 distress,
                                                 event,
                                                 eventXml,
                                                 savedShip,
                                                 imageCached,
                                                 backgroundImage,
                                                 planetImage,
                                                 planetX,
                                                 planetY,
                                                 planetRotation,
                                                 neighbors))
    }
};

class TravelGraph {
    std::vector<Beacon> beacons_;
    std::uint32_t seed_ = 0;
    int current_ = 0;
    int dangerY_ = 0;
    int dangerX_ = -959;
    int pursuitDelay_ = 0;
    // GenerateMap 0x5d4590 counts exit candidates and final-sector boss path
    // candidates with one shared counter; transient, generation only.
    mutable int generationEvaluations_ = 0;
    int waitStart_ = -959;
    int waitMove_ = 0;
    bool nebulaMap_ = false;
    void convertFleetLocations(const std::function<void(int)>& convert);
    void connect();
    bool connected() const;

public:
    static constexpr int finalGenerationAttempts = 64;
    static constexpr int finalExitRegenerationAttempts = 256;
    // Geometry follows 0x5d3510/0x5d30b0, including Location's animation RNG draw.
    // Sector/event population and exit selection are separate from geometry.
    void generate(std::uint32_t seed);
    void generate(std::uint32_t seed, RandomStream& random);
    void regenerateGeometry(RandomStream& random);
    bool reachable(int destination) const;
    bool visit(int destination,
               bool advancePursuit = true,
               const std::function<void(int)>& convert = {});
    void revealCurrent();
    void setEvent(int id, const std::string& event, bool nebula = false, bool exit = false);
    void setEventXml(int id, std::string xml);
    void setImages(int id, const nlohmann::json& row);
    int nextFleetMove() const;
    void modifyPursuit(int amount);
    void wait(const std::function<void(int)>& convert = {});
    void beginWait();
    void
    waitProgress(float progress, bool scan = true, const std::function<void(int)>& convert = {});

    void setNebulaMap(bool value) {
        nebulaMap_ = value;
    }

    [[nodiscard]] bool nebulaMap() const {
        return nebulaMap_;
    }

    void setStore(int id, bool value = true);
    void setDistress(int id, bool value = true);
    void setRepair(int id, bool value = true);
    void revealAll();
    int addQuest(const std::string& event, bool force, RandomStream& random);
    int chooseExit(RandomStream& random, int finalDifficulty = -1) const;
    std::vector<int> chooseBossPath(RandomStream& random, int base) const;
    std::vector<int> shortestPath(int start, int finish) const;

    void setFleet(int id, bool value) {
        beacons_.at(static_cast<std::size_t>(id)).fleet = value;
    }

    void advanceFleet(const std::function<void(int)>& convert = {});

    int dangerX() const {
        return dangerX_;
    }

    int dangerY() const {
        return dangerY_;
    }

    // StarMap::currentSectorSeed, pursuitDelay and dangerWaitStart.
    std::uint32_t seed() const {
        return seed_;
    }

    int pursuitDelay() const {
        return pursuitDelay_;
    }

    int waitStart() const {
        return waitStart_;
    }

    void setSavedShip(int id, nlohmann::json value) {
        beacons_.at(static_cast<std::size_t>(id)).savedShip = std::move(value);
    }

    const std::vector<Beacon>& beacons() const {
        return beacons_;
    }

    int current() const {
        return current_;
    }

    nlohmann::json state() const;
    FTL_NATIVE_JSON(TravelGraph,
                    beacons_,
                    seed_,
                    current_,
                    dangerY_,
                    dangerX_,
                    pursuitDelay_,
                    waitStart_,
                    waitMove_,
                    nebulaMap_)
};

struct SectorNode {
    int level = 0;
    int row = 75;
    int type = 0;
    bool visited = false;
    std::string name;
    std::string title;
    std::string shortTitle;
    std::vector<int> neighbors;
    FTL_NATIVE_JSON(SectorNode, level, row, type, visited, name, title, shortTitle, neighbors)
};

class SectorGraph {
    std::vector<SectorNode> nodes_;
    int current_ = 0;

public:
    // StarMap::GenerateSectorMap/AddSectorColumn 0x5cb6f0/0x5ca680.
    void generate(RandomStream& random, const std::function<void(SectorNode&)>& selectName);
    bool select(int choice);

    std::vector<SectorNode>& nodes() {
        return nodes_;
    }

    const std::vector<SectorNode>& nodes() const {
        return nodes_;
    }

    const SectorNode& current() const {
        return nodes_.at(current_);
    }
    FTL_NATIVE_JSON(SectorGraph, nodes_, current_)
};
} // namespace ftl
