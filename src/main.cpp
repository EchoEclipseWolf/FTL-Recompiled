#include "blueprints.hpp"
#include "log.hpp"
#include "menu.hpp"
#include "settings.hpp"
#include <SDL3/SDL_main.h>
#include <array>
#include <charconv>
#include <cmath>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using ftl::Json;

std::string platformKeyName(SDL_Keycode key) {
    // TranslateSILKey5780f0 folds keypad digits and Enter onto the ordinary
    // keys. The operators retain their separate original SDL key numbers.
    if (key == SDLK_KP_ENTER) {
        return "Enter";
    }
    for (const auto& [code, name] :
         std::array<std::pair<SDL_Keycode, const char*>, 10>{{{SDLK_KP_0, "0"},
                                                              {SDLK_KP_1, "1"},
                                                              {SDLK_KP_2, "2"},
                                                              {SDLK_KP_3, "3"},
                                                              {SDLK_KP_4, "4"},
                                                              {SDLK_KP_5, "5"},
                                                              {SDLK_KP_6, "6"},
                                                              {SDLK_KP_7, "7"},
                                                              {SDLK_KP_8, "8"},
                                                              {SDLK_KP_9, "9"}}}) {
        if (key == code) {
            return name;
        }
    }
    return SDL_GetKeyName(key);
}

const char* mouseButtonName(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return "left";
    case SDL_BUTTON_RIGHT:
        return "right";
    case SDL_BUTTON_MIDDLE:
        return "middle";
    case SDL_BUTTON_X1:
        return "x1";
    case SDL_BUTTON_X2:
        return "x2";
    default:
        return "other";
    }
}

void reply(const Json& j) {
    std::cout << j.dump() << '\n' << std::flush;
}

int bootstrap(const ftl::Archive& archive) {
    std::uint64_t ticks = 0;
    bool paused = false;
    reply({{"ok", true},
           {"event", "ready"},
           {"protocol", 1},
           {"stage", "bootstrap"},
           {"pointer_bits", 64},
           {"cpp", __cplusplus},
           {"asset_entries", archive.count()},
           {"gameplay_available", false}});
    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "quit") {
            reply({{"ok", true}, {"event", "bye"}});
            return 0;
        }
        if (command == "ping") {
            reply({{"ok", true}, {"event", "pong"}});
        } else if (command == "state") {
            reply({{"ok", true},
                   {"scene", "bootstrap"},
                   {"ticks", ticks},
                   {"paused", paused},
                   {"asset_entries", archive.count()},
                   {"gameplay_available", false}});
        } else if (command == "pause" || command == "resume") {
            paused = command == "pause";
            reply({{"ok", true}, {"paused", paused}});
        } else if (command.starts_with("step ")) {
            std::uint32_t n = 0;
            auto s = std::string_view(command).substr(5);
            auto p = std::from_chars(s.data(), s.data() + s.size(), n);
            if (p.ec != std::errc{} || p.ptr != s.data() + s.size() || n > 1000000) {
                reply({{"ok", false}, {"error", "invalid_step_count"}});
            } else {
                if (!paused) {
                    ticks += n;
                }
                reply({{"ok", true}, {"ticks", ticks}});
            }
        } else {
            reply({{"ok", false}, {"error", "unsupported_command"}});
        }
    }
    return 0;
}

std::uint32_t count(const Json& q, const char* key, std::uint32_t max) {
    if (!q.contains(key) || !q[key].is_number_integer()) {
        throw std::runtime_error("invalid_integer");
    }
    auto n = q[key].get<std::int64_t>();
    if (n < 0 || std::uint64_t(n) > max) {
        throw std::runtime_error("integer_out_of_range");
    }
    return std::uint32_t(n);
}

Json capabilities() {
    return {{"commands",
             {"capabilities",
              "state",
              "audio_state",
              "native_save",
              "native_continue",
              "load_fixture",
              "dev",
              "input",
              "step",
              "advance_time",
              "time_control",
              "time_state",
              "render_benchmark",
              "capture",
              "resize",
              "quit"}},
            {"fixtures",
             {"fresh",
              "continue",
              "first_launch",
              "hangar",
              "hangar_all",
              "hangar_fresh",
              "live_damage",
              "live_ion"}},
            {"input", {"semantic", "pixel", "key", "text", "focus"}},
            {"developer_ops",
             {"resources",
              "grant",
              "crew",
              "system",
              "reactor",
              "limit",
              "enemy",
              "event",
              "hazard",
              "ion"}},
            {"languages", ftl::languages},
            {"capture_format", "bmp"},
            {"logical_size", {1280, 720}},
            {"tick_hz", 60},
            {"simulation_available", true},
            {"implemented",
             {"main_menu",
              "new_game_confirmation",
              "cancel",
              "exit",
              "hangar",
              "launch",
              "opening_event",
              "user_modal_pause",
              "crew_selection",
              "queued_crew_movement",
              "stations",
              "doors",
              "system_power",
              "reactor",
              "oxygen",
              "baseline_crew_health"}},
            {"draft_features",
             {"voyage_panels",
              "native_save_continue",
              "native_profile",
              "menu_support",
              "player_hacking_mind_clone"}},
            {"deferred",
             {"original_binary_save_compatibility",
              "original_visual_runtime_acceptance:computer_use_paused",
              "windows_release_and_linux:user_paused"}},
            {"audit_phase", 11},
            {"source_coverage_and_differences", "docs/research/P11-index.md"}};
}

int automation(ftl::Renderer& renderer,
               ftl::Menu& menu,
               ftl::Audio& audio,
               const ftl::Archive& archive,
               bool eagerRender) {
    menu.renderBoundary();
    menu.render();
    renderer.present();
    // Drawing never mutates gameplay or consumes RNG (docs/in-progress/TIMING.md),
    // so a frame is only drawn when something reads it: a capture, or an input
    // whose hit-testing uses the last rendered layout. Each command still ends
    // at the application boundary, exactly as if its frame had been drawn.
    // --eager-render draws after every command, as protocol v2 originally did.
    bool stale = false;
    const auto refresh = [&] {
        if (stale) {
            menu.render();
            renderer.present();
            stale = false;
        }
    };
    reply({{"ok", true},
           {"event", "ready"},
           {"schema_version", 2},
           {"stage", menu.state().at("scene")},
           {"pointer_bits", 64},
           {"cpp", __cplusplus},
           {"asset_entries", archive.count()}});
    std::string line;
    while (std::getline(std::cin, line)) {
        Json id = nullptr;
        try {
            if (line.size() > 1024 * 1024) {
                throw std::runtime_error("request_too_large");
            }
            auto q = Json::parse(line);
            if (q.is_object() && q.contains("id")) {
                id = q["id"];
            }
            if (!q.is_object() || q.value("schema_version", 0) != 2 || id.is_null() ||
                (!id.is_string() && !id.is_number_integer())) {
                throw std::runtime_error("invalid_envelope");
            }
            auto cmd = q.at("command").get<std::string>();
            if (cmd != "step" && cmd != "state") {
                refresh();
            }
            Json result = Json::object();
            Json devResult;
            bool draw = false;
            bool done = false;
            if (cmd == "capabilities") {
                result = capabilities();
            } else if (cmd == "native_save" || cmd == "native_continue") {
                const auto path = q.at("path").get<std::string>();
                if (path.empty()) {
                    throw std::runtime_error("native_persistence_path_empty");
                }
                if (cmd == "native_save") {
                    menu.saveRun(path);
                } else {
                    menu.continueRun(path);
                }
                draw = true;
            } else if (cmd == "state") {
                result = menu.state();
            } else if (cmd == "render_benchmark") {
                const auto frames = count(q, "frames", 10000);
                if (frames == 0) {
                    throw std::runtime_error("frames_must_be_positive");
                }
                const auto seconds = q.value("seconds_per_frame", 1.0 / 240);
                const auto period = ftl::TimeController::framePeriod(q.value("paced_fps", 0.0));
                std::vector<double> samples;
                std::vector<double> intervals;
                samples.reserve(frames);
                intervals.reserve(frames);
                auto deadline = SDL_GetTicksNS();
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    const auto begin = SDL_GetTicksNS();
                    menu.advance(seconds);
                    menu.renderBoundary();
                    menu.render();
                    renderer.present();
                    samples.push_back(double(SDL_GetTicksNS() - begin) / 1e6);
                    if (period > 0) {
                        deadline += Uint64(period * 1e9);
                        const auto now = SDL_GetTicksNS();
                        if (deadline > now) {
                            SDL_DelayPrecise(deadline - now);
                        } else {
                            deadline = now;
                        }
                    }
                    intervals.push_back(double(SDL_GetTicksNS() - begin) / 1e6);
                }
                result = {{"frame_ms", samples},
                          {"interval_ms", intervals},
                          {"backend", renderer.backend()},
                          {"clock", menu.timing()},
                          {"state", menu.state()}};
            } else if (cmd == "time_control") {
                menu.simulationSpeed(q.at("simulation_speed").get<double>());
                result = menu.timing();
            } else if (cmd == "time_state") {
                result = menu.timing();
            } else if (cmd == "advance_time") {
                // A list applies its advances back to back, exactly as the same
                // render-less requests would one by one, without a round trip each.
                const auto& seconds = q.at("seconds");
                if (seconds.is_array()) {
                    for (const auto& value : seconds) {
                        menu.advance(value.get<double>());
                    }
                } else {
                    menu.advance(seconds.get<double>());
                }
                draw = q.value("render", true);
                if (!draw) {
                    result = menu.timing();
                }
            } else if (cmd == "audio_state") {
                result = audio.state();
            } else if (cmd == "load_fixture") {
                menu.fixture(q.at("fixture").get<std::string>(),
                             q.contains("seed") ? count(q, "seed", UINT32_MAX) : 0,
                             q.value("values", Json::object()));
                draw = true;
            } else if (cmd == "dev") {
                devResult = menu.devCommand(q);
                draw = true;
            } else if (cmd == "step") {
                menu.step(count(q, "ticks", 1000000));
                draw = true;
            } else if (cmd == "resize") {
                renderer.resize(int(count(q, "width", 7680)), int(count(q, "height", 4320)));
                draw = true;
            } else if (cmd == "input") {
                const auto kind = q.at("kind").get<std::string>();
                if (kind == "semantic") {
                    menu.semantic(q.at("target").get<std::string>(),
                                  q.value("action", std::string("click")));
                } else if (kind == "focus") {
                    menu.focus(q.at("focused").get<bool>());
                } else if (kind == "key") {
                    menu.key(q.at("key").get<std::string>());
                } else if (kind == "key_up") {
                    menu.keyUp(q.at("key").get<std::string>());
                } else if (kind == "text") {
                    menu.inputText(q.at("text").get<std::string>());
                } else if (kind == "pixel") {
                    float x = q.at("x").get<float>();
                    float y = q.at("y").get<float>();
                    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 100000 ||
                        std::abs(y) > 100000) {
                        throw std::runtime_error("invalid_coordinates");
                    }
                    auto space = q.value("space", std::string("logical"));
                    if (space == "window") {
                        auto p = renderer.logical(x, y);
                        x = p.x;
                        y = p.y;
                    } else if (space != "logical") {
                        throw std::runtime_error("unsupported_coordinate_space");
                    }
                    auto action = q.value("action", std::string("click"));
                    menu.modifier(q.value("shift", false));
                    if (action == "hover") {
                        menu.move(x, y);
                    } else if (action == "click") {
                        menu.click(x, y);
                    } else if (action == "middle_click") {
                        menu.middleClick();
                    } else if (action == "right_click") {
                        menu.rightClick(x, y);
                    } else if (action == "down") {
                        menu.pointerDown(x, y);
                    } else if (action == "up") {
                        menu.pointerUp(x, y);
                    } else {
                        throw std::runtime_error("unsupported_input_action");
                    }
                } else {
                    throw std::runtime_error("unsupported_input_kind");
                }
                draw = true;
            } else if (cmd == "capture") {
                auto path = std::filesystem::path(q.at("path").get<std::string>());
                if (path.extension() != ".bmp") {
                    throw std::runtime_error("capture_requires_bmp_extension");
                }
                renderer.capture(path);
                result = {{"path", std::filesystem::absolute(path).string()},
                          {"width", 1280},
                          {"height", 720}};
            } else if (cmd == "quit") {
                if (!menu.requestExit()) {
                    throw std::runtime_error("native_exit_save_failed");
                }
                done = true;
                result = {{"event", "bye"}};
            } else {
                throw std::runtime_error("unsupported_command");
            }
            if (draw) {
                menu.renderBoundary();
                stale = true;
                if (eagerRender) {
                    refresh();
                }
                result = menu.state();
                if (!devResult.is_null()) {
                    result["dev_result"] = devResult;
                }
            }
            reply({{"schema_version", 2}, {"id", id}, {"ok", true}, {"result", result}});
            if (done || menu.quitting()) {
                return 0;
            }
        } catch (const std::exception& e) {
            reply({{"schema_version", 2}, {"id", id}, {"ok", false}, {"error", e.what()}});
        }
    }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    static_assert(sizeof(void*) == 8);
    std::filesystem::path data = std::filesystem::absolute(argv[0]).parent_path() / "ftl.dat";
    std::string fixture = "fresh";
    std::string language = "en";
    bool legacy = false;
    bool json = false;
    bool hidden = false;
    bool audioRequested = false;
    bool mute = false;
    bool catalog = false;
    bool accelerated = false;
    bool eagerRender = false;
    bool software = false;
    bool startCombat = false;
    bool liveSeeds = false;
    std::filesystem::path settingsPath = data.parent_path() / "recomp-settings.json";
    bool explicitSettings = false;
    bool explicitLanguage = false;
    bool explicitWidth = false;
    bool explicitHeight = false;
    double simulationSpeed = 1;
    double renderFps = 240;
    int width = 1280;
    int height = 720;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];
            if (arg == "--automation-stdio") {
                legacy = true;
            } else if (arg == "--inspect-catalog") {
                catalog = true;
            } else if (arg == "--automation-json") {
                json = true;
            } else if (arg == "--live-seeds") {
                liveSeeds = true;
            } else if (arg == "--hidden") {
                hidden = true;
            } else if (arg == "--audio") {
                audioRequested = true;
            } else if (arg == "--accelerated") {
                accelerated = true;
            } else if (arg == "--software") {
                software = true;
            } else if (arg == "--eager-render") {
                eagerRender = true;
            } else if (arg == "--start-combat") {
                startCombat = true;
            } else if (arg == "--mute") {
                mute = true;
            } else if (arg == "--data" && i + 1 < argc) {
                data = argv[++i];
            } else if (arg == "--fixture" && i + 1 < argc) {
                fixture = argv[++i];
            } else if (arg == "--language" && i + 1 < argc) {
                language = argv[++i];
                explicitLanguage = true;
            } else if (arg == "--settings" && i + 1 < argc) {
                settingsPath = argv[++i];
                explicitSettings = true;
            } else if (arg == "--width" && i + 1 < argc) {
                width = std::stoi(argv[++i]);
                explicitWidth = true;
            } else if (arg == "--height" && i + 1 < argc) {
                height = std::stoi(argv[++i]);
                explicitHeight = true;
            } else if (arg == "--simulation-speed" && i + 1 < argc) {
                simulationSpeed = std::stod(argv[++i]);
            } else if (arg == "--render-fps" && i + 1 < argc) {
                renderFps = std::stod(argv[++i]);
            } else if (arg == "--help") {
                std::cout
                    << "FTLRecomp P11 system audit\n--data PATH --fixture "
                       "fresh|continue|first_launch "
                       "--language en|de|es|fr|it|pl|pt|ru|zh-Hans\n--automation-json --hidden "
                       "(deterministic rendered protocol v2)\n--automation-stdio (legacy P00 "
                       "harness)\n--width N --height N --mute\n--audio (enable real-time playback "
                       "in automation)\n--accelerated | --software\n--eager-render (draw after "
                       "every "
                       "automation command)\n"
                       "--live-seeds (randomize New Game in automation)\n--simulation-speed 0..100 "
                       "--render-fps 0..10000 (0 "
                       "uncapped; default 240)\n--start-combat (visible paused seeded battle)\n";
                return 0;
            } else {
                throw std::runtime_error("invalid_argument");
            }
        }
        if (width < 320 || height < 180 || width > 7680 || height > 4320 || (legacy && json) ||
            (startCombat && (json || legacy || hidden || catalog))) {
            throw std::runtime_error("invalid_argument");
        }
        const auto executable = std::filesystem::absolute(argv[0]);
        ftl::logging::start(
            ftl::logging::readConfig(executable.parent_path() / "FTLRecomp-logging.ini"),
            executable.parent_path() / (executable.stem().string() + ".log"),
            !json && !legacy && !catalog && !hidden);
        ftl::logging::info(
            "app", "{} starting, data {}", executable.filename().string(), data.string());
        double framePeriod = ftl::TimeController::framePeriod(renderFps);
        ftl::TimeState validation;
        ftl::TimeController::speed(validation, simulationSpeed);
        const ftl::Archive archive(data);
        if (legacy) {
            return bootstrap(archive);
        }
        if (catalog) {
            reply(ftl::BlueprintCatalog(archive).snapshot());
            return 0;
        }
        const bool persist = (!json || explicitSettings) && !startCombat;
        auto settings = persist ? ftl::Settings::read(settingsPath) : ftl::Settings{};
        if (explicitLanguage) {
            settings.language = language;
            settings.languageSelected = true;
        }
        if (explicitWidth) {
            settings.width = width;
        }
        if (explicitHeight) {
            settings.height = height;
        }
        settings.validate();
        language = settings.language;
        width = settings.width;
        height = settings.height;
        ftl::Renderer renderer(archive, hidden, width, height, !software && (!json || accelerated));
        ftl::Audio audio(archive,
                         !mute && (!json || audioRequested),
                         settings.musicVolume,
                         settings.soundVolume);
        // CApp::OnInit403430 seeds random32 from the boundary's epoch seconds.
        // Explicit automation fixtures provide their own deterministic seed.
        const auto startupSeed = !json || liveSeeds ? std::uint32_t(std::time(nullptr)) : 0u;
        ftl::Menu menu(renderer, archive, audio, startupSeed);
        menu.volumes(settings.musicVolume, settings.soundVolume);
        menu.hotkeys(settings.hotkeys);
        menu.options(settings);
        menu.renderCap(renderFps);
        menu.simulationSpeed(simulationSpeed);
        menu.language(language);
        menu.fixture(persist && !settings.languageSelected ? "first_launch" : fixture, startupSeed);
        menu.liveSeeds(!json || liveSeeds);
        if (persist) {
            menu.persistence((settingsPath.parent_path() / "recomp-run-v1.json").string());
        }
        if (startCombat) {
            menu.startCombat();
        }
        auto saveSettings = [&] {
            if (persist) {
                settings.musicVolume = menu.musicVolume();
                settings.soundVolume = menu.soundVolume();
                settings.hotkeys = menu.hotkeys();
                settings.vsync = menu.vsyncDesired();
                settings.frameLimit = menu.frameLimit();
                settings.focusPause = menu.focusPause();
                settings.fullscreen = menu.fullscreenDesired();
                settings.lastFullscreen = menu.lastFullscreen();
                settings.difficulty = menu.difficultyDesired();
                settings.gameplay = menu.gameplayOptions();
                settings.language = menu.locale();
                settings.languageSelected = menu.languageSelected();
                SDL_GetWindowSize(renderer.window(), &settings.width, &settings.height);
                settings.write(settingsPath);
            }
        };
        if (json) {
            const auto result = automation(renderer, menu, audio, archive, eagerRender);
            saveSettings();
            return result;
        }
        double updateMillisecondsPerTick = 0;

        struct CpuSample {
            double activeSeconds;
            double updateMilliseconds;
            std::uint64_t ticks;
        };

        std::deque<CpuSample> cpuSamples;
        double sampleSeconds = 0;
        double sampleMilliseconds = 0;
        std::uint64_t sampleTicks = 0;
        double refreshSeconds = 0;
        double presentingFps = 0;
        std::uint64_t presentedFrames = 0;
        auto presentationSampleStarted = SDL_GetTicksNS();
        auto updateTitle = [&] {
            std::ostringstream speedText;
            speedText << std::fixed << std::setprecision(2) << simulationSpeed;
            std::ostringstream cpuText;
            cpuText << std::fixed << std::setprecision(2) << updateMillisecondsPerTick;
            std::ostringstream fpsText;
            fpsText << std::fixed << std::setprecision(1) << presentingFps;
            const auto title = "FTLRecomp | simulation " + speedText.str() + "x | presenting " +
                               fpsText.str() + " FPS | game CPU " + cpuText.str() +
                               " ms/tick | F8 1280x720, F9 speed, F10 cap, F12 dev";
            SDL_SetWindowTitle(renderer.window(), title.c_str());
        };
        updateTitle();
        SDL_StartTextInput(renderer.window());
        auto previous = SDL_GetTicksNS();
        auto deadline = previous;
        bool activeAtPrevious = menu.gameplayClockActive();
        // Wall time at which a held left button was first seen physically
        // released without its mouse-up event; 0 while held or after an event.
        std::uint64_t physicalReleaseAt = 0;
        constexpr std::uint64_t lostReleaseGraceNs = 250'000'000;
        while (!menu.quitting()) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    if (menu.requestExit()) {
                        saveSettings();
                        ftl::logging::info("app", "exiting");
                        ftl::logging::stop();
                        return 0;
                    }
                    continue;
                }
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    SDL_CaptureMouse(false);
                    menu.focus(false);
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    menu.focus(true);
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    physicalReleaseAt = 0;
                    const auto p = renderer.logical(event.button.x, event.button.y);
                    ftl::logging::verbose(
                        "input",
                        "mouse {} {} window=({:.1f},{:.1f}) logical=({:.1f},{:.1f}) clicks={} "
                        "mouse_id={} t={:.3f}s",
                        mouseButtonName(event.button.button),
                        event.button.down ? "down" : "up",
                        event.button.x,
                        event.button.y,
                        p.x,
                        p.y,
                        event.button.clicks,
                        event.button.which,
                        double(event.button.timestamp) / 1e9);
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    auto p = renderer.logical(event.motion.x, event.motion.y);
                    menu.move(p.x, p.y);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    auto p = renderer.logical(event.button.x, event.button.y);
                    menu.modifier((SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
                    SDL_CaptureMouse(true);
                    menu.pointerDown(p.x, p.y);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                           event.button.button == SDL_BUTTON_LEFT) {
                    auto p = renderer.logical(event.button.x, event.button.y);
                    menu.pointerUp(p.x, p.y);
                    SDL_CaptureMouse(false);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           event.button.button == SDL_BUTTON_RIGHT) {
                    auto p = renderer.logical(event.button.x, event.button.y);
                    menu.rightClick(p.x, p.y);
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                           event.button.button == SDL_BUTTON_MIDDLE) {
                    menu.middleClick();
                } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                    menu.inputText(event.text.text);
                } else if (event.type == SDL_EVENT_KEY_UP) {
                    menu.keyUp(platformKeyName(event.key.key));
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    // CEvent::OnEvent5786c0 dispatches key-down only when
                    // the platform repeat flag is clear; text input has its
                    // separate event stream.
                    if (event.key.repeat) {
                        menu.repeatTextKey(platformKeyName(event.key.key));
                        continue;
                    }
                    if (!event.key.repeat && event.key.key == SDLK_F9) {
                        const auto changed = SDL_GetTicksNS();
                        menu.advance(double(changed - previous) / 1e9);
                        previous = changed;
                        simulationSpeed = simulationSpeed < 1    ? 1
                                          : simulationSpeed < 5  ? 5
                                          : simulationSpeed < 10 ? 10
                                                                 : .1;
                        menu.simulationSpeed(simulationSpeed);
                        updateTitle();
                        continue;
                    }
                    if (!event.key.repeat && event.key.key == SDLK_F10) {
                        renderFps = renderFps == 0    ? 60
                                    : renderFps < 144 ? 144
                                    : renderFps < 240 ? 240
                                                      : 0;
                        framePeriod = ftl::TimeController::framePeriod(renderFps);
                        menu.renderCap(renderFps);
                        deadline = SDL_GetTicksNS();
                        updateTitle();
                        continue;
                    }
                    // Development control: return a windowed session to the
                    // original game's 1280x720 client size. Shadows the
                    // default crew-8 hotkey.
                    if (event.key.key == SDLK_F8) {
                        if (renderer.fullscreen() != 0) {
                            ftl::logging::info("app", "F8 window reset ignored in fullscreen");
                            continue;
                        }
                        SDL_RestoreWindow(renderer.window());
                        renderer.resize(1280, 720);
                        SDL_SyncWindow(renderer.window());
                        ftl::logging::info("app", "F8 window reset to 1280x720");
                        continue;
                    }
                    const std::pair<SDL_Keycode, const char*> keys[] = {
                        {SDLK_U, "U"},           {SDLK_C, "C"},
                        {SDLK_B, "B"},           {SDLK_Q, "Q"},
                        {SDLK_J, "J"},           {SDLK_SLASH, "Slash"},
                        {SDLK_F1, "F1"},         {SDLK_F2, "F2"},
                        {SDLK_F3, "F3"},         {SDLK_F4, "F4"},
                        {SDLK_F5, "F5"},         {SDLK_F6, "F6"},
                        {SDLK_F7, "F7"},         {SDLK_F8, "F8"},
                        {SDLK_F12, "F12"},       {SDLK_A, "A"},
                        {SDLK_S, "S"},           {SDLK_D, "D"},
                        {SDLK_F, "F"},           {SDLK_G, "G"},
                        {SDLK_T, "T"},           {SDLK_V, "V"},
                        {SDLK_Z, "Z"},           {SDLK_X, "X"},
                        {SDLK_SPACE, "Space"},   {SDLK_1, "1"},
                        {SDLK_2, "2"},           {SDLK_3, "3"},
                        {SDLK_4, "4"},           {SDLK_ESCAPE, "Escape"},
                        {SDLK_RETURN, "Enter"},  {SDLK_BACKSPACE, "Backspace"},
                        {SDLK_DELETE, "Delete"}, {SDLK_HOME, "Home"},
                        {SDLK_END, "End"},       {SDLK_LEFT, "Left"},
                        {SDLK_RIGHT, "Right"},   {SDLK_UP, "Up"},
                        {SDLK_DOWN, "Down"}};
                    bool dispatched = false;
                    for (auto [key, name] : keys) {
                        if (event.key.key == key) {
                            dispatched = true;
                            menu.key((event.key.mod & SDL_KMOD_SHIFT) ? "Shift+" + std::string(name)
                                                                      : name);
                        }
                    }
                    if (!dispatched) {
                        const std::string name = platformKeyName(event.key.key);
                        menu.key((event.key.mod & SDL_KMOD_SHIFT) ? "Shift+" + name : name);
                    }
                }
            }
            // Recover a missed platform mouse-up without dropping equipment at
            // an unrelated coordinate. Normal release still owns valid swaps.
            // The physical button state changes before its mouse-up message is
            // pumped, so a fast click can look released here while its up event
            // is still in flight. Only a release that persists with no queued
            // mouse-up is treated as lost.
            if ((SDL_GetGlobalMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0) {
                physicalReleaseAt = 0;
            } else if (physicalReleaseAt == 0) {
                physicalReleaseAt = SDL_GetTicksNS();
            }
            const bool releaseLost = [&] {
                if (physicalReleaseAt == 0 ||
                    SDL_GetTicksNS() - physicalReleaseAt < lostReleaseGraceNs) {
                    return false;
                }
                SDL_PumpEvents();
                return SDL_PeepEvents(nullptr,
                                      0,
                                      SDL_PEEKEVENT,
                                      SDL_EVENT_MOUSE_BUTTON_UP,
                                      SDL_EVENT_MOUSE_BUTTON_UP) <= 0;
            }();
            if (menu.reconcilePointer(!releaseLost)) {
                ftl::logging::warning("input",
                                      "left button released {:.0f} ms ago with no mouse-up event; "
                                      "pending press cancelled without a click",
                                      double(SDL_GetTicksNS() - physicalReleaseAt) / 1e6);
                SDL_CaptureMouse(false);
            }
            const auto now = SDL_GetTicksNS();
            if (renderFps != menu.renderCap()) {
                renderFps = menu.renderCap();
                framePeriod = ftl::TimeController::framePeriod(renderFps);
                deadline = now;
                updateTitle();
            }
            const double wallSeconds = double(now - previous) / 1e9;
            const bool activeBefore = menu.gameplayClockActive();
            const auto ticksBefore = menu.gameplayTicks();
            const auto updateStarted = SDL_GetTicksNS();
            menu.advance(wallSeconds);
            const auto updateFinished = SDL_GetTicksNS();
            const bool activeAfter = menu.gameplayClockActive();
            if (activeAtPrevious && activeBefore && activeAfter && wallSeconds > 0) {
                const auto ticks = menu.gameplayTicks() - ticksBefore;
                const double updateMilliseconds =
                    ticks > 0 ? double(updateFinished - updateStarted) / 1e6 : 0;
                cpuSamples.push_back({wallSeconds, updateMilliseconds, ticks});
                sampleSeconds += wallSeconds;
                sampleMilliseconds += updateMilliseconds;
                sampleTicks += ticks;
                while (sampleSeconds > 5 && cpuSamples.size() > 1) {
                    const auto& oldest = cpuSamples.front();
                    sampleSeconds -= oldest.activeSeconds;
                    sampleMilliseconds -= oldest.updateMilliseconds;
                    sampleTicks -= oldest.ticks;
                    cpuSamples.pop_front();
                }
                refreshSeconds += wallSeconds;
                if (refreshSeconds >= 1) {
                    updateMillisecondsPerTick =
                        sampleTicks > 0 ? sampleMilliseconds / double(sampleTicks) : 0;
                    refreshSeconds = std::fmod(refreshSeconds, 1.0);
                    updateTitle();
                }
            }
            activeAtPrevious = activeAfter;
            previous = now;
            menu.renderBoundary();
            menu.render();
            renderer.present();
            // Count completed presentations against unscaled wall time, including
            // pauses, menus, simulation work, and frame pacing between frames.
            const auto presentedAt = SDL_GetTicksNS();
            ++presentedFrames;
            const double presentationSeconds =
                double(presentedAt - presentationSampleStarted) / 1e9;
            if (presentationSeconds >= 1) {
                presentingFps = double(presentedFrames) / presentationSeconds;
                presentedFrames = 0;
                presentationSampleStarted = presentedAt;
                updateTitle();
            }
            if (framePeriod > 0) {
                deadline += Uint64(framePeriod * 1e9);
                const auto finished = SDL_GetTicksNS();
                if (deadline > finished) {
                    SDL_DelayPrecise(deadline - finished);
                } else {
                    deadline = finished;
                }
            }
        }
        saveSettings();
        ftl::logging::info("app", "exiting");
        ftl::logging::stop();
        return 0;
    } catch (const std::exception& e) {
        ftl::logging::error("app", "fatal: {}", e.what());
        reply({{"ok", false}, {"error", e.what()}});
        return 1;
    }
}
