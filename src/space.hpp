#pragma once
#include "render.hpp"
#include "travel.hpp"
#include <functional>

namespace ftl {
struct NebulaCloud {
    int x = 0;
    int y = 0;
    float alpha = 0;
    float previousAlpha = 0;
    float previousScale = 3;
    float scale = 3;
    float alphaRate = .05f;
    float trigger = .6f;
    bool successor = false;
    float flashTimer = 100;
    float flashElapsed = 0;
    int lightningRotation = 0;
    bool lightning = false;
    bool flashing = false;

    friend void to_json(nlohmann::json& nlohmann_json_j, const NebulaCloud& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO,
                                                 x,
                                                 y,
                                                 alpha,
                                                 previousAlpha,
                                                 previousScale,
                                                 scale,
                                                 alphaRate,
                                                 trigger,
                                                 successor,
                                                 flashTimer,
                                                 flashElapsed,
                                                 lightningRotation,
                                                 lightning,
                                                 flashing))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j, NebulaCloud& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM,
                                                 x,
                                                 y,
                                                 alpha,
                                                 previousAlpha,
                                                 previousScale,
                                                 scale,
                                                 alphaRate,
                                                 trigger,
                                                 successor))
        const NebulaCloud nlohmann_json_default_obj{};
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT,
                                                 flashTimer,
                                                 flashElapsed,
                                                 lightningRotation,
                                                 lightning,
                                                 flashing))
        if (nlohmann_json_t.flashTimer < -1000000 || nlohmann_json_t.flashTimer > 1000000 ||
            nlohmann_json_t.flashElapsed < 0 || nlohmann_json_t.flashElapsed > .25f ||
            nlohmann_json_t.lightningRotation < 0 || nlohmann_json_t.lightningRotation > 270 ||
            nlohmann_json_t.lightningRotation % 90 != 0) {
            throw std::runtime_error("invalid_nebula_lightning");
        }
    }
};

class NebulaScene {
    RandomStream random_{0};
    std::vector<NebulaCloud> clouds_;
    bool storm_ = false;
    NebulaCloud cloud(int x, int y, RandomStream& random);

public:
    void reset(std::uint32_t seed, bool storm = false, RandomStream* random = nullptr);
    void setStorm(bool storm);
    void load(const nlohmann::json& layers);
    void advance(float speed,
                 const std::function<void(int, float)>& lightning = {},
                 RandomStream* random = nullptr);
    void render(Renderer& renderer, bool paused, float hullRatio, float fraction = 1) const;
    nlohmann::json state() const;

    friend void to_json(nlohmann::json& nlohmann_json_j, const NebulaScene& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(NLOHMANN_JSON_TO, random_, clouds_, storm_))
    }

    friend void from_json(const nlohmann::json& nlohmann_json_j, NebulaScene& nlohmann_json_t) {
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM, random_, clouds_))
        const NebulaScene nlohmann_json_default_obj{};
        NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FTL_NATIVE_JSON_FROM_DEFAULT, storm_))
        for (auto& cloud : nlohmann_json_t.clouds_) {
            cloud.lightning = nlohmann_json_t.storm_;
        }
    }
};
} // namespace ftl
