#include "space.hpp"
#include "time.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ftl {
void NebulaScene::load(const nlohmann::json& layers) {
    if (!layers.is_array() || layers.empty() || layers.size() > 64) {
        throw std::runtime_error("invalid_nebula_snapshot");
    }
    std::vector<NebulaCloud> candidate;
    for (const auto& layer : layers) {
        NebulaCloud cloud;
        cloud.x = layer.at("position").at(0).get<int>();
        cloud.y = layer.at("position").at(1).get<int>();
        cloud.alpha = layer.at("alpha").get<float>();
        cloud.scale = layer.at("scale").get<float>();
        cloud.alphaRate = layer.at("alpha_rate").get<float>();
        cloud.trigger = layer.at("trigger").get<float>();
        cloud.successor = layer.at("successor").get<bool>();
        cloud.lightning = layer.value("lightning", storm_);
        cloud.flashTimer = layer.value("flash_timer", 100.f);
        cloud.flashElapsed = layer.value("flash_elapsed", 0.f);
        cloud.lightningRotation = layer.value("lightning_rotation", 0);
        cloud.flashing = layer.value("flashing", false);
        if (cloud.x < -100 || cloud.x > 1380 || cloud.y < -100 || cloud.y > 820 ||
            !std::isfinite(cloud.alpha) || cloud.alpha < 0 || cloud.alpha > 1 ||
            !std::isfinite(cloud.scale) || cloud.scale < 1 || cloud.scale > 10 ||
            !std::isfinite(cloud.alphaRate) || std::abs(cloud.alphaRate) > .1f ||
            !std::isfinite(cloud.trigger) || cloud.trigger < 0 || cloud.trigger > 1 ||
            !std::isfinite(cloud.flashTimer) || cloud.flashTimer < -1000000 ||
            cloud.flashTimer > 1000000 || !std::isfinite(cloud.flashElapsed) ||
            cloud.flashElapsed < 0 || cloud.flashElapsed > .25f || cloud.lightningRotation < 0 ||
            cloud.lightningRotation > 270 || cloud.lightningRotation % 90 != 0) {
            throw std::runtime_error("invalid_nebula_layer");
        }
        cloud.previousAlpha = cloud.alpha;
        cloud.previousScale = cloud.scale;
        candidate.push_back(cloud);
    }
    clouds_ = std::move(candidate);
}

NebulaCloud NebulaScene::cloud(int x, int y, RandomStream& random) {
    NebulaCloud result;
    result.x = x;
    result.y = y;
    result.scale = static_cast<float>(random.next() % 2 + 3);
    result.previousScale = result.scale;
    result.trigger = static_cast<float>(random.next() % 301 + 600) / 1000;
    result.flashTimer = static_cast<float>(random.next() % 201 + 100);
    result.lightning = storm_;
    return result;
}

void NebulaScene::reset(std::uint32_t seed, bool storm, RandomStream* random) {
    storm_ = storm;
    // SpaceManager::SetNebula0x41d360: jittered 4x3 grid, shuffled layers and warmup.
    if (!random) {
        random_ = RandomStream(seed);
    }
    auto& draws = random ? *random : random_;
    clouds_.clear();
    std::vector<std::pair<int, int>> points;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 3; ++row) {
            const int dx = row == 1 ? static_cast<int>(draws.next() % 131) - 65 : 0;
            const int dy =
                column == 1 || column == 2 ? static_cast<int>(draws.next() % 81) - 40 : 0;
            points.emplace_back(100 + 350 * column + dx, 200 + 230 * row + dy);
        }
    }
    while (!points.empty()) {
        const auto index = draws.next() % points.size();
        const auto [x, y] = points[index];
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
        clouds_.push_back(cloud(x, y, draws));
    }
    for (int tick = 0; tick < 800; ++tick) {
        advance(Time::originalStep, {}, random);
    }
}

void NebulaScene::setStorm(bool storm) {
    storm_ = storm;
    for (auto& cloud : clouds_) {
        cloud.lightning = storm;
    }
}

void NebulaScene::advance(float speed,
                          const std::function<void(int, float)>& lightning,
                          RandomStream* random) {
    auto& draws = random ? *random : random_;
    // NebulaCloud::OnLoop0x41a7d0 and SpaceManager::OnLoopNebulas0x41c4d0.
    for (std::size_t index = 0; index < clouds_.size();) {
        auto& current = clouds_[index];
        current.previousAlpha = current.alpha;
        current.previousScale = current.scale;
        const float cloudSpeed = speed == 0 ? .05f : speed;
        current.alpha += cloudSpeed * current.alphaRate;
        current.scale += cloudSpeed * .003f;
        if (current.alpha > 1) {
            current.alpha = 1;
            current.alphaRate = -.002f;
        } else if (current.alpha < current.trigger && current.alphaRate < 0) {
            current.alphaRate = -.01f;
        }
        current.flashTimer -= speed;
        const bool flashDone = current.flashing && current.flashElapsed + speed * .0625f > .25f;
        if (current.flashing) {
            current.flashElapsed = std::min(.25f, current.flashElapsed + speed * .0625f);
        }
        if (current.flashTimer < 0 && current.lightning) {
            current.lightningRotation = 90 * static_cast<int>(draws.next() % 4);
            current.flashing = true;
            current.flashElapsed = 0;
            current.flashTimer = 1000000;
            const float volume = static_cast<float>(draws.next() % 101 + 900) / 1000;
            const int alias = static_cast<int>(draws.next() % 4 + 1);
            if (lightning) {
                lightning(alias, volume);
            }
        } else if (flashDone) {
            current.flashTimer = static_cast<float>(draws.next() % 201 + 100);
            current.flashing = false;
        }
        if (current.alpha < current.trigger && current.alphaRate < 0 && !current.successor) {
            current.successor = true;
            const auto next = cloud(current.x, current.y, draws);
            clouds_.insert(clouds_.begin() + static_cast<std::ptrdiff_t>(index), next);
        } else if (current.alpha < 0) {
            clouds_.erase(clouds_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
    if (clouds_.size() > 64) {
        throw std::runtime_error("nebula_layer_bound_exceeded");
    }
}

void NebulaScene::render(Renderer& renderer, bool paused, float hullRatio, float fraction) const {
    // SpaceManager::OnRenderBackground0x4179a0. The original Y anchor also uses199.
    const float red = paused ? .4f + (1 - hullRatio) * .4f : 1;
    const float green = paused ? .4f : 1 - (1 - hullRatio) * (1 - hullRatio) * .5f;
    for (const auto& cloud : clouds_) {
        const float scale = std::lerp(cloud.previousScale, cloud.scale, fraction);
        const float alpha = std::lerp(cloud.previousAlpha, cloud.alpha, fraction);
        const SDL_Color color{static_cast<Uint8>(255 * red),
                              static_cast<Uint8>(255 * green),
                              static_cast<Uint8>(255 * green),
                              static_cast<Uint8>(255 * std::clamp(alpha, 0.f, 1.f))};
        if (cloud.flashing) {
            renderer.sprite(
                renderer.image("img/stars/lightning.png"),
                {cloud.x - 42.5f * scale, cloud.y - 77.5f * scale, 85 * scale, 155 * scale},
                {0, 0, 85, 155},
                static_cast<float>(cloud.lightningRotation),
                color);
        }
        auto& image = renderer.image(cloud.flashing    ? "img/stars/nebula_large_a.png"
                                     : cloud.lightning ? "img/stars/nebula_large_b.png"
                                                       : "img/stars/nebula_large_c.png");
        renderer.sprite(
            image,
            {cloud.x - 99.5f * scale, cloud.y - 99.5f * scale, 199 * scale, 155 * scale},
            {0, 0, 199, 155},
            0,
            color);
    }
}

nlohmann::json NebulaScene::state() const {
    auto result = nlohmann::json::array();
    for (const auto& cloud : clouds_) {
        result.push_back({{"position", {cloud.x, cloud.y}},
                          {"alpha", cloud.alpha},
                          {"scale", cloud.scale},
                          {"alpha_rate", cloud.alphaRate},
                          {"trigger", cloud.trigger},
                          {"successor", cloud.successor},
                          {"lightning", cloud.lightning},
                          {"flash_timer", cloud.flashTimer},
                          {"flash_elapsed", cloud.flashElapsed},
                          {"lightning_rotation", cloud.lightningRotation},
                          {"flashing", cloud.flashing}});
    }
    return result;
}
} // namespace ftl
