#pragma once
#include "archive.hpp"
#include <json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace ftl {
class RandomStream;

// Native SDL playback; never exposes original x86 handles or layouts.
class Audio {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    Audio(const Archive& archive, bool enabled, float musicVolume, float soundVolume);
    ~Audio();
    void volumes(float music, float sound);
    void sessionRandom(RandomStream& random);
    void hover();
    void sound(const std::string& alias, bool looping = false, float volume = -1);
    void loop(const std::string& alias, bool enabled);
    void updateLoop(const std::string& alias, float count);
    void pauseLoops(bool paused);
    void restartLoops();
    void active(bool value);
    void music(const std::string& asset);
    void playlist(const std::vector<std::string>& tracks);
    void combatMusic(bool enabled);
    void advance(double seconds);
    nlohmann::json state() const;
};
} // namespace ftl
