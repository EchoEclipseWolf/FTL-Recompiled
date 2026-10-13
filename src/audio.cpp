#include "audio.hpp"
#include "log.hpp"
#include "resources.hpp"
#include "travel.hpp"
#include <SDL3/SDL.h>
#define STB_VORBIS_HEADER_ONLY
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <stb_vorbis.c>
#include <stdexcept>

namespace ftl {
namespace {
constexpr auto titleAsset = "audio/music/bp_MUS_TitleScreen.ogg";
constexpr auto hoverAsset = "audio/waves/ui/select_light1.wav";

void check(bool ok) {
    if (!ok) {
        throw std::runtime_error(SDL_GetError());
    }
}

struct StreamLock {
    SDL_AudioStream* stream;

    explicit StreamLock(SDL_AudioStream* value) : stream(value) {
        if (stream) {
            check(SDL_LockAudioStream(stream));
        }
    }

    ~StreamLock() {
        if (stream) {
            SDL_UnlockAudioStream(stream);
        }
    }

    StreamLock(const StreamLock&) = delete;
    StreamLock& operator=(const StreamLock&) = delete;
};
} // namespace

struct Audio::Impl {
    struct Sample {
        std::string path;
        float gain = 1;
        bool looping = false;
        bool failedDecode = false;
        std::vector<float> samples;
    };

    struct Clip {
        Sample* sample = nullptr;
        float gain = 0;
        bool pausesWithGame = true;
    };

    struct Voice {
        const Clip* clip = nullptr;
        std::size_t at = 0;
        bool looping = false;
        float gain = 1;
        std::size_t fadeSamples = 0;
        std::size_t fadeTotal = 0;
        bool managedLoop = false;
        std::uint64_t playedSamples = 0;
    };

    struct MusicFile {
        Bytes compressed;
        std::unique_ptr<stb_vorbis, decltype(&stb_vorbis_close)> decoder{nullptr, stb_vorbis_close};
    };

    struct MusicVoice {
        MusicFile* file = nullptr;
        bool combat = false;
        bool looping = false;
        bool retiring = false;
        bool done = false;
        float gain = 1;
        float from = 1;
        float target = 1;
        std::size_t remaining = 0;
        std::size_t total = 0;

        void slide(float value, double seconds, int rate) {
            from = gain;
            target = value;
            remaining = total = std::size_t(std::round(seconds * rate));
            if (!remaining) {
                gain = value;
            }
        }

        float frameGain() {
            if (remaining) {
                gain = from + (target - from) * (1.f - float(remaining) / float(total));
                --remaining;
            } else {
                gain = target;
                if (retiring) {
                    done = true;
                }
            }
            return gain;
        }
    };

    const Archive* archive = nullptr;
    RandomStream sessionRandom{0};
    std::string asset = titleAsset;
    std::map<std::string, std::pair<std::string, std::string>> musicTracks;
    std::map<std::string, MusicFile> musicFiles;
    std::vector<MusicVoice> musicVoices;
    std::vector<std::string> playlist;
    int currentMusic = -1;
    double musicDelay = -1;
    bool combatMusic = false;
    SDL_AudioStream* stream = nullptr;
    bool initialized = false;
    std::atomic<bool> active = false;
    std::atomic<bool> loopsPaused = false;
    std::map<std::string, Clip> clips;
    std::map<std::string, Sample> soundFiles;
    std::vector<Voice> voices;
    std::map<std::string, bool> requestedLoops;
    std::map<std::string, float> loopLimits, loopCounts;
    int rate = 0;
    float musicGain = 0, soundGain = 0;
    float effectGain = 0;
    std::atomic<bool> failed = false;
    std::atomic<std::uint64_t> frames = 0, musicFrames = 0, loops = 0, hoverEvents = 0,
                               hoverPlayed = 0, soundEvents = 0, soundPlayed = 0,
                               soundLoopWraps = 0;
    // Diagnostic only: the last requested aliases, for automation assertions.
    std::deque<std::string> recentSounds;

    ~Impl() {
        if (stream) {
            SDL_DestroyAudioStream(stream);
        }
        if (initialized) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }

    MusicFile& loadMusic(const std::string& path) {
        if (auto existing = musicFiles.find(path); existing != musicFiles.end()) {
            return existing->second;
        }
        MusicFile file;
        file.compressed = archive->read(path);
        int error = 0;
        file.decoder.reset(stb_vorbis_open_memory(
            file.compressed.data(), int(file.compressed.size()), &error, nullptr));
        if (!file.decoder) {
            throw std::runtime_error("cannot_decode_music: " + path);
        }
        const auto info = stb_vorbis_get_info(file.decoder.get());
        if (info.channels != 2 || info.sample_rate < 8000 || info.sample_rate > 192000 ||
            (rate && int(info.sample_rate) != rate)) {
            throw std::runtime_error("unsupported_music_format");
        }
        rate = int(info.sample_rate);
        return musicFiles.emplace(path, std::move(file)).first->second;
    }

    void switchTracks() {
        // SwitchTracks5adba0 advances the authored order without RNG. Its
        // explore-channel end sync5ae8e0 switches both paired channels.
        if (playlist.empty()) {
            return;
        }
        currentMusic = (currentMusic + 1) % int(playlist.size());
        const auto& [explore, combat] = musicTracks.at(playlist[currentMusic]);
        asset = explore;
        musicFrames = 0;
        loops = 0;
        musicVoices.clear();
        if (!stream) {
            return;
        }
        for (int role = 0; role < 2; ++role) {
            const auto& path = role == 0 ? explore : combat;
            if (path.empty()) {
                continue;
            }
            auto& file = musicFiles.at(path);
            if (!stb_vorbis_seek_start(file.decoder.get())) {
                failed = true;
                return;
            }
            const float gain =
                role == 0 ? float(!combatMusic || combat.empty()) : float(combatMusic);
            musicVoices.push_back(
                {&file, role != 0, playlist.size() == 1, false, false, gain, gain, gain});
        }
    }

    int decodeMusic(MusicVoice& voice, float* output, int wanted) {
        std::fill_n(output, wanted * 2, 0.f);
        int got = 0;
        while (got < wanted) {
            const int decoded = stb_vorbis_get_samples_float_interleaved(
                voice.file->decoder.get(), 2, output + got * 2, (wanted - got) * 2);
            if (decoded == 0) {
                if (stb_vorbis_get_error(voice.file->decoder.get()) != VORBIS__no_error) {
                    failed = true;
                    break;
                }
                if (!voice.looping) {
                    break;
                }
                if (!stb_vorbis_seek_start(voice.file->decoder.get())) {
                    failed = true;
                    break;
                }
                if (!voice.combat && !voice.retiring) {
                    ++loops;
                }
                const int first = stb_vorbis_get_samples_float_interleaved(
                    voice.file->decoder.get(), 2, output + got * 2, (wanted - got) * 2);
                if (!first) {
                    failed = true;
                    break;
                }
                got += first;
            } else {
                got += decoded;
            }
        }
        return got;
    }

    static void mixMusic(MusicVoice& voice, float* output, const float* samples, int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            const float gain = voice.frameGain();
            output[frame * 2] += samples[frame * 2] * gain;
            output[frame * 2 + 1] += samples[frame * 2 + 1] * gain;
        }
    }

    void fillMusic(float* output, int wanted) {
        std::fill_n(output, wanted * 2, 0.f);
        if (!active.load()) {
            return;
        }
        std::array<float, 2048> samples{};
        int at = 0;
        while (at < wanted && !failed.load()) {
            auto primary = std::ranges::find_if(musicVoices, [](const MusicVoice& voice) {
                return !voice.combat && !voice.retiring;
            });
            if (primary == musicVoices.end()) {
                break;
            }
            const int got = decodeMusic(*primary, samples.data(), wanted - at);
            if (!got) {
                if (failed.load()) {
                    break;
                }
                switchTracks();
                continue;
            }
            mixMusic(*primary, output + at * 2, samples.data(), got);
            auto combat = std::ranges::find_if(musicVoices, [](const MusicVoice& voice) {
                return voice.combat && !voice.retiring;
            });
            if (combat != musicVoices.end()) {
                decodeMusic(*combat, samples.data(), got);
                mixMusic(*combat, output + at * 2, samples.data(), got);
            }
            musicFrames += got;
            at += got;
        }
        for (auto& voice : musicVoices) {
            if (voice.retiring) {
                decodeMusic(voice, samples.data(), wanted);
                mixMusic(voice, output, samples.data(), wanted);
            }
        }
        std::erase_if(musicVoices, [](const MusicVoice& voice) {
            return voice.done;
        });
    }

    static void SDLCALL fill(void* userdata, SDL_AudioStream* stream, int additional, int) {
        auto& self = *static_cast<Impl*>(userdata);
        std::array<float, 2048> buffer{};
        while (additional > 0 && !self.failed.load()) {
            const int wanted = std::min(int(buffer.size() / 2), (additional + 7) / 8);
            const int got = wanted;
            self.fillMusic(buffer.data(), wanted);
            for (int i = 0; i < got * 2; ++i) {
                float effect = 0;
                for (auto& voice : self.voices) {
                    if (voice.managedLoop && !voice.fadeTotal && voice.clip->pausesWithGame &&
                        self.loopsPaused.load()) {
                        continue;
                    }
                    const auto& samples = voice.clip->sample->samples;
                    if (voice.looping && voice.at == samples.size()) {
                        voice.at = 0;
                        ++self.soundLoopWraps;
                    }
                    if (voice.at < samples.size()) {
                        const float fade = voice.fadeTotal
                                               ? float(voice.fadeSamples) / float(voice.fadeTotal)
                                               : 1.f;
                        effect += samples[voice.at++] * voice.gain * fade * self.effectGain;
                        ++voice.playedSamples;
                        if (voice.fadeSamples) {
                            --voice.fadeSamples;
                        }
                    }
                }
                buffer[i] = std::clamp(buffer[i] * self.musicGain + effect, -1.f, 1.f);
            }
            std::erase_if(self.voices, [](const Voice& voice) {
                return (voice.fadeTotal && !voice.fadeSamples) ||
                       (!voice.looping && voice.at >= voice.clip->sample->samples.size());
            });
            if (!SDL_PutAudioStreamData(stream, buffer.data(), got * 2 * int(sizeof(float)))) {
                self.failed = true;
                return;
            }
            self.frames += got;
            additional -= got * 2 * int(sizeof(float));
        }
    }
};

Audio::Audio(const Archive& archive, bool enabled, float musicVolume, float soundVolume)
    : impl_(std::make_unique<Impl>()) {
    auto& a = *impl_;
    a.archive = &archive;
    // Original SetMusicVolume 0x5aba80: 7500/10000; SetSoundVolume
    // 0x5abad0: 9000/10000, multiplied by sounds.xml hoverBeep volume 7/10.
    a.musicGain = musicVolume * .75f;
    a.soundGain = soundVolume * .9f * .7f;
    a.effectGain = soundVolume * .9f;
    // AE weapon and system aliases live in dlcSounds.xml, alongside the
    // original sounds.xml registry. Both point into audio/waves/.
    for (const auto* registry : {"data/sounds.xml", "data/dlcSounds.xml"}) {
        auto sounds = xmlResource(archive, registry);
        for (auto entry : sounds.child("FTL").children()) {
            if (std::string_view(entry.name()) == "music") {
                for (auto track : entry.children("track")) {
                    const std::string name = track.child_value("name");
                    const std::string explore = track.child_value("explore");
                    const std::string combat = track.child_value("combat");
                    if (!name.empty() && !explore.empty()) {
                        a.musicTracks[name] = {"audio/music/" + explore,
                                               combat.empty() ? "" : "audio/music/" + combat};
                    }
                }
                continue;
            }
            if (entry.text()) {
                const std::string path = std::string("audio/waves/") + entry.child_value();
                if (archive.contains(path)) {
                    auto& sample = a.soundFiles[path];
                    sample.path = path;
                    const float gain = entry.attribute("volume").as_int() / 10.f;
                    if (entry.attribute("volume")) {
                        // ParseSoundFile5aff90 changes the shared sample's
                        // default gain; the final definition for a file wins.
                        sample.gain = gain;
                    }
                    a.clips.insert_or_assign(entry.name(), Impl::Clip{&sample, gain});
                    // ParseSoundFile5aff90 keeps loop metadata separately, so
                    // a later non-loop alias does not erase its loop definition.
                    if (entry.attribute("loop")) {
                        sample.looping = true;
                        a.loopLimits[entry.name()] = float(entry.attribute("count").as_int(1));
                    }
                }
            }
        }
    }
    // ParseSoundFile5aff90 marks the distress loop pause=false; PauseLoops
    // 5aebc0 leaves its active channels playing while other managed loops pause.
    if (auto distress = a.clips.find("distress"); distress != a.clips.end()) {
        distress->second.pausesWithGame = false;
    }
    if (!enabled) {
        return;
    }
    try {
        a.loadMusic(titleAsset);
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, a.rate};
        check(SDL_InitSubSystem(SDL_INIT_AUDIO));
        a.initialized = true;
        a.stream =
            SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, Impl::fill, &a);
        if (!a.stream) {
            throw std::runtime_error(SDL_GetError());
        }
        a.playlist = {"title"};
        a.switchTracks();
        check(SDL_ResumeAudioStreamDevice(a.stream));
    } catch (const std::exception& error) {
        // CApp::OnExecute403700 continues after SoundControl::OnInit5ab870
        // fails. Preserve the game and expose the unavailable audio backend.
        logging::error("audio", "initialization failed: {}", error.what());
        a.failed = true;
        if (a.stream) {
            SDL_DestroyAudioStream(a.stream);
            a.stream = nullptr;
        }
        if (a.initialized) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            a.initialized = false;
        }
    }
}

Audio::~Audio() = default;

void Audio::sessionRandom(RandomStream& random) {
    // The recovered mixer and authored playlist traversal consume no RNG.
    // Any recovered sound selector shares the application stream explicitly.
    impl_->sessionRandom.bind(random);
}

void Audio::volumes(float music, float sound) {
    if (!std::isfinite(music) || !std::isfinite(sound) || music < 0 || music > 1 || sound < 0 ||
        sound > 1) {
        throw std::runtime_error("invalid_audio_volume");
    }
    auto& audio = *impl_;
    StreamLock lock(audio.stream);
    audio.musicGain = music * .75f;
    audio.soundGain = sound * .9f * .7f;
    audio.effectGain = sound * .9f;
}

void Audio::active(bool value) {
    impl_->active = value;
}

void Audio::music(const std::string& asset) {
    auto& a = *impl_;
    for (const auto& [name, tracks] : a.musicTracks) {
        if (tracks.first == asset) {
            playlist({name});
            return;
        }
    }
    // Compatibility for native diagnostic assets outside sounds.xml.
    const std::string name = "@asset:" + asset;
    {
        StreamLock lock(a.stream);
        a.musicTracks[name] = {asset, ""};
    }
    playlist({name});
}

void Audio::playlist(const std::vector<std::string>& tracks) {
    auto& a = *impl_;
    StreamLock lock(a.stream);
    if (tracks == a.playlist) {
        return;
    }
    for (const auto& name : tracks) {
        auto definition = a.musicTracks.find(name);
        if (definition == a.musicTracks.end()) {
            logging::warning("audio", "bad track name: {}", name);
            return;
        }
        if (a.stream) {
            try {
                a.loadMusic(definition->second.first);
                if (!definition->second.second.empty()) {
                    a.loadMusic(definition->second.second);
                }
            } catch (const std::exception& error) {
                logging::warning("audio", "could not load music track {}: {}", name, error.what());
                return;
            }
        }
    }
    // StartPlaylist5ae900 keeps the current channels if the first new track
    // is already playing; changing playlist order does not restart that track.
    if (!tracks.empty() && a.currentMusic >= 0 &&
        a.playlist.at(std::size_t(a.currentMusic)) == tracks.front()) {
        a.playlist = tracks;
        a.currentMusic = 0;
        return;
    }
    const bool hadPlaylist = !a.playlist.empty();
    if (hadPlaylist) {
        for (auto& voice : a.musicVoices) {
            if (!voice.retiring) {
                voice.retiring = true;
                voice.slide(0, .75, a.rate);
            }
        }
    }
    a.playlist = tracks;
    a.currentMusic = -1;
    a.musicDelay = hadPlaylist && !tracks.empty() ? 2.0 : -1.0;
    if (!tracks.empty() && a.musicDelay < 0) {
        a.switchTracks();
    }
}

void Audio::combatMusic(bool enabled) {
    auto& a = *impl_;
    StreamLock lock(a.stream);
    if (enabled == a.combatMusic) {
        return;
    }
    a.combatMusic = enabled;
    if (a.currentMusic < 0) {
        return;
    }
    const bool paired =
        !a.musicTracks.at(a.playlist.at(std::size_t(a.currentMusic))).second.empty();
    if (!paired) {
        return;
    }
    // StartCombat5ad2e0/StopCombat5ad740 change both channel gains over2000ms;
    // both streams advance throughout combat and game pause.
    for (auto& voice : a.musicVoices) {
        if (!voice.retiring) {
            // StopCombat5ad740 restores the explore channel to .8 rather
            // than SwitchTracks5adba0's initial1.0 (raw float0x3f4ccccd).
            const float target = voice.combat ? float(enabled) : enabled ? 0.f : .8f;
            voice.slide(target, 2.0, a.rate);
        }
    }
}

void Audio::advance(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) {
        throw std::runtime_error("invalid_audio_time");
    }
    auto& a = *impl_;
    StreamLock lock(a.stream);
    if (a.musicDelay > 0) {
        a.musicDelay -= seconds;
        if (a.musicDelay <= 0) {
            a.musicDelay = -1;
            a.switchTracks();
        }
    }
}

void Audio::hover() {
    ++impl_->hoverEvents;
    // Menu hover aliases use the same source sample channels as all other
    // effects, so rapid pointer changes can overlap rather than restarting
    // one special native beep channel.
    const auto played = impl_->soundPlayed.load();
    sound("hoverBeep");
    if (impl_->soundPlayed.load() != played) {
        ++impl_->hoverPlayed;
    }
}

void Audio::sound(const std::string& alias, bool looping, float volume) {
    if (!std::isfinite(volume) || volume < -1 || volume > 1) {
        throw std::runtime_error("invalid_sound_volume");
    }
    auto& a = *impl_;
    auto it = a.clips.find(alias);
    if (it == a.clips.end()) {
        // SoundControl::PlaySoundMix0x5af830 logs an unknown alias and returns
        // without playing; a missing sound must not interrupt the game.
        if (!alias.empty()) {
            logging::warning("audio", "unknown sound alias: {}", alias);
        }
        return;
    }
    ++a.soundEvents;
    a.recentSounds.push_back(alias);
    if (a.recentSounds.size() > 256) {
        a.recentSounds.pop_front();
    }
    if (!a.stream) {
        return;
    }
    auto& clip = it->second;
    auto& sample = *clip.sample;
    if (sample.failedDecode) {
        return;
    }
    if (sample.samples.empty()) {
        try {
            auto bytes = a.archive->read(sample.path);
            SDL_AudioSpec source{};
            Uint8* raw = nullptr;
            Uint32 length = 0;
            const bool wav = sample.path.ends_with(".wav");
            if (wav) {
                check(SDL_LoadWAV_IO(
                    SDL_IOFromConstMem(bytes.data(), bytes.size()), true, &source, &raw, &length));
            } else {
                int channels = 0;
                int rate = 0;
                short* decoded = nullptr;
                const int samples = stb_vorbis_decode_memory(
                    bytes.data(), int(bytes.size()), &channels, &rate, &decoded);
                std::unique_ptr<short, decltype(&std::free)> decodedOwner(decoded, std::free);
                if (samples <= 0 || channels < 1 || channels > 2) {
                    throw std::runtime_error("cannot_decode_sound: " + sample.path);
                }
                source = {SDL_AUDIO_S16, channels, rate};
                raw = reinterpret_cast<Uint8*>(decodedOwner.release());
                length = Uint32(samples * channels * int(sizeof(short)));
            }
            auto release = [wav](Uint8* data) {
                if (wav) {
                    SDL_free(data);
                } else {
                    std::free(data);
                }
            };
            std::unique_ptr<Uint8, decltype(release)> owned(raw, release);
            SDL_AudioSpec target{SDL_AUDIO_F32, 2, a.rate};
            Uint8* converted = nullptr;
            int convertedLength = 0;
            check(SDL_ConvertAudioSamples(
                &source, raw, int(length), &target, &converted, &convertedLength));
            std::unique_ptr<Uint8, decltype(&SDL_free)> pcm(converted, SDL_free);
            if (convertedLength <= 0 || convertedLength % (2 * int(sizeof(float)))) {
                throw std::runtime_error("empty_or_invalid_sound: " + sample.path);
            }
            std::vector<float> samples(convertedLength / int(sizeof(float)));
            std::memcpy(samples.data(), converted, convertedLength);
            StreamLock lock(a.stream);
            sample.samples = std::move(samples);
        } catch (const std::exception& error) {
            // AddSoundFile5abed0 logs a failed sample load and returns.
            sample.failedDecode = true;
            logging::warning("audio", "could not load sound {}: {}", sample.path, error.what());
            return;
        }
    }
    StreamLock lock(a.stream);
    std::erase_if(a.voices, [](const Impl::Voice& voice) {
        return (voice.fadeTotal && !voice.fadeSamples) ||
               (!voice.looping && voice.at >= voice.clip->sample->samples.size());
    });
    Impl::Voice* longest = nullptr;
    int channels = 0;
    for (auto& voice : a.voices) {
        if (voice.clip->sample == &sample) {
            ++channels;
            if (!longest || voice.playedSamples > longest->playedSamples) {
                longest = &voice;
            }
        }
    }
    if (channels >= 12) {
        // AddSoundFile5abed0 allows12 channels per shared sample and
        // BASS_SAMPLE_OVER_POS replaces the longest-playing channel.
        a.voices.erase(a.voices.begin() + (longest - a.voices.data()));
    }
    // PlaySoundMix5af830 overrides the sample-default volume only for a
    // positive explicit argument. Sample loop flags also apply to aliases.
    a.voices.push_back(
        {&clip, 0, looping || sample.looping, volume > 0 ? volume : sample.gain, 0, 0, looping});
    ++a.soundPlayed;
}

void Audio::loop(const std::string& alias, bool enabled) {
    updateLoop(alias, enabled ? 1.f : 0.f);
}

void Audio::updateLoop(const std::string& alias, float count) {
    if (!std::isfinite(count) || count < 0) {
        throw std::runtime_error("invalid_sound_loop_count");
    }
    auto& a = *impl_;
    if (!a.clips.contains(alias)) {
        if (count > 0) {
            sound(alias);
        }
        return;
    }
    const float limit = a.loopLimits.contains(alias) ? a.loopLimits.at(alias) : 1.f;
    const float accepted = std::min(count, limit);
    if (a.loopCounts[alias] == accepted) {
        return;
    }
    const bool enabled = accepted > 0;
    const bool previouslyEnabled = a.requestedLoops[alias];
    a.loopCounts[alias] = accepted;
    a.requestedLoops[alias] = enabled;
    if (enabled && !previouslyEnabled) {
        sound(alias, true, 1);
    }
    if (a.stream) {
        StreamLock lock(a.stream);
        const auto* clip = &a.clips.at(alias);
        for (auto& voice : a.voices) {
            if (voice.managedLoop && voice.clip == clip && !voice.fadeTotal) {
                if (enabled) {
                    voice.gain = clip->gain * accepted / limit;
                } else {
                    // UpdateSoundLoop5afc00 stops a loop with a 500ms fade.
                    // Retiring channels no longer belong to PauseLoops.
                    voice.fadeSamples = voice.fadeTotal = std::size_t(a.rate);
                }
            }
        }
    }
}

void Audio::pauseLoops(bool paused) {
    // SoundControl::PauseLoops5aebc0 pauses/resumes existing loop channels;
    // leave each sample cursor intact while one-shots and music keep playing.
    impl_->loopsPaused = paused;
}

void Audio::restartLoops() {
    auto& a = *impl_;
    StreamLock lock(a.stream);
    // SoundControl::Restart5af290 retires every owned channel, including
    // loops which ignore gameplay pause, and clears the requested counts.
    for (auto& voice : a.voices) {
        if (voice.managedLoop && !voice.fadeTotal) {
            voice.fadeSamples = voice.fadeTotal = std::size_t(a.rate);
        }
    }
    for (auto& [alias, requested] : a.requestedLoops) {
        requested = false;
        a.loopCounts[alias] = 0;
    }
}

nlohmann::json Audio::state() const {
    const auto& a = *impl_;
    StreamLock lock(a.stream);
    nlohmann::json channels = nlohmann::json::array();
    for (const auto& voice : a.musicVoices) {
        channels.push_back({{"combat", voice.combat},
                            {"gain", voice.gain},
                            {"target_gain", voice.target},
                            {"retiring", voice.retiring},
                            {"looping", voice.looping},
                            {"fade_frames_remaining", voice.remaining}});
    }
    return {{"enabled", a.stream != nullptr},
            {"device_open", a.stream != nullptr},
            {"active", a.active.load()},
            {"failed", a.failed.load()},
            {"driver", a.stream ? SDL_GetCurrentAudioDriver() : "disabled"},
            {"music_asset", a.asset},
            {"music_playlist", a.playlist},
            {"music_index", a.currentMusic},
            {"music_delay_seconds", a.musicDelay},
            {"combat_music", a.combatMusic},
            {"music_channels", std::move(channels)},
            {"hover_asset", hoverAsset},
            {"sample_rate", a.rate},
            {"channels", 2},
            {"music_gain", a.musicGain},
            {"hover_gain", a.soundGain},
            {"frames_submitted", a.frames.load()},
            {"music_frames", a.musicFrames.load()},
            {"loops", a.loops.load()},
            {"hover_events", a.hoverEvents.load()},
            {"hover_played", a.hoverPlayed.load()},
            {"requested_loops", a.requestedLoops},
            {"sound_loop_counts", a.loopCounts},
            {"loops_paused", a.loopsPaused.load()},
            {"sound_loop_wraps", a.soundLoopWraps.load()},
            {"sound_events", a.soundEvents.load()},
            {"recent_sounds", a.recentSounds},
            {"sound_played", a.soundPlayed.load()}};
}
} // namespace ftl
