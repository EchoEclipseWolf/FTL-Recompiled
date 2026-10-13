#include "log.hpp"
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ftl::logging {
namespace {
constexpr std::string_view defaultConfig =
    "; FTLRecomp logging settings. Read once at startup; restart the game after editing.\n"
    "; level: Verbose, Info, Warning, Error or Off. Messages below the level are hidden.\n"
    "level = Verbose\n"
    "; console: show the log console window beside the game (true/false).\n"
    "console = true\n"
    "; file: also write <executable name>.log next to the game, replaced on each launch.\n"
    "file = true\n";

enum class ConsoleColor : std::uint8_t { None, Ansi, Attributes };

struct State {
    std::mutex mutex;
    Level threshold = Level::Warning;
    bool console = true;
    ConsoleColor color = ConsoleColor::None;
    std::ofstream file;
#ifdef _WIN32
    HANDLE handle = nullptr;
    WORD defaultAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
#endif
};

State& state() {
    static State instance;
    return instance;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return result;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::optional<Level> parseLevel(std::string_view text) {
    const auto value = lower(text);
    if (value == "verbose" || value == "debug" || value == "trace") {
        return Level::Verbose;
    }
    if (value == "info" || value == "log" || value == "display") {
        return Level::Info;
    }
    if (value == "warning" || value == "warn") {
        return Level::Warning;
    }
    if (value == "error") {
        return Level::Error;
    }
    if (value == "off" || value == "none") {
        return Level::Off;
    }
    return std::nullopt;
}

std::optional<bool> parseBool(std::string_view text) {
    const auto value = lower(text);
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    return std::nullopt;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    std::array<char, 16> clock{};
    std::strftime(clock.data(), clock.size(), "%H:%M:%S", &local);
    return std::format("{}.{:03}", clock.data(), millis);
}

// Conventional console colours: Verbose grey, Info white, Warning yellow, Error red.
std::string_view ansi(Level level) {
    switch (level) {
    case Level::Verbose:
        return "\x1b[90m";
    case Level::Warning:
        return "\x1b[93m";
    case Level::Error:
        return "\x1b[91m";
    default:
        return "\x1b[97m";
    }
}

void forwardSdlLog(void*, int, SDL_LogPriority priority, const char* message) {
    const Level level = priority <= SDL_LOG_PRIORITY_DEBUG  ? Level::Verbose
                        : priority == SDL_LOG_PRIORITY_INFO ? Level::Info
                        : priority == SDL_LOG_PRIORITY_WARN ? Level::Warning
                                                            : Level::Error;
    if (enabled(level)) {
        write(level, "sdl", message ? message : "");
    }
}

#ifdef _WIN32
// Console-subsystem builds always own or share a console, but launchers such as
// Start-Process -WindowStyle Hidden leave it invisible. Allocate one only when
// none is attached, and hide it only when no other process shares it.
void prepareConsole(State& s, bool show) {
    HWND window = GetConsoleWindow();
    if (show && !window && AllocConsole()) {
        FILE* stream = nullptr;
        (void)freopen_s(&stream, "CONOUT$", "w", stderr);
        window = GetConsoleWindow();
    }
    if (window) {
        std::array<DWORD, 2> processes{};
        const bool owned = GetConsoleProcessList(processes.data(), DWORD(processes.size())) <= 1;
        if (show && !IsWindowVisible(window)) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
        } else if (!show && owned) {
            ShowWindow(window, SW_HIDE);
        }
    }
    if (show) {
        SetConsoleTitleW(L"FTLRecomp log");
    }
    s.handle = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (s.handle == nullptr || s.handle == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(s.handle, &mode)) {
        s.color = ConsoleColor::None;
        return;
    }
    if (SetConsoleMode(s.handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        s.color = ConsoleColor::Ansi;
        return;
    }
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(s.handle, &info)) {
        s.defaultAttributes = info.wAttributes;
    }
    s.color = ConsoleColor::Attributes;
}

WORD attributes(Level level) {
    switch (level) {
    case Level::Verbose:
        return FOREGROUND_INTENSITY;
    case Level::Warning:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    case Level::Error:
        return FOREGROUND_RED | FOREGROUND_INTENSITY;
    default:
        return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    }
}
#endif

#ifndef _WIN32
void prepareConsole(State& s, bool) {
    s.color = isatty(STDERR_FILENO) ? ConsoleColor::Ansi : ConsoleColor::None;
}
#endif

void emitConsole(State& s, Level level, std::string_view line) {
    std::string text;
    if (s.color == ConsoleColor::Ansi) {
        text = std::format("{}{}\x1b[0m\n", ansi(level), line);
    } else {
        text = std::format("{}\n", line);
    }
#ifdef _WIN32
    if (s.color == ConsoleColor::Attributes) {
        SetConsoleTextAttribute(s.handle, attributes(level));
    }
#endif
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
#ifdef _WIN32
    if (s.color == ConsoleColor::Attributes) {
        SetConsoleTextAttribute(s.handle, s.defaultAttributes);
    }
#endif
}
} // namespace

std::string_view name(Level level) {
    switch (level) {
    case Level::Verbose:
        return "Verbose";
    case Level::Info:
        return "Info";
    case Level::Warning:
        return "Warning";
    case Level::Error:
        return "Error";
    case Level::Off:
        return "Off";
    }
    return "Unknown";
}

Config readConfig(const std::filesystem::path& path) {
    Config config;
    std::ifstream input(path);
    if (!input) {
        std::ofstream output(path, std::ios::binary);
        output << defaultConfig;
        if (!output) {
            config.issues.push_back("could not create " + path.string());
        }
        return config;
    }
    std::string line;
    int number = 0;
    while (std::getline(input, line)) {
        ++number;
        const auto content = trim(std::string_view(line).substr(0, line.find_first_of(";#")));
        if (content.empty()) {
            continue;
        }
        const auto equals = content.find('=');
        const auto key = equals == std::string_view::npos ? std::string{}
                                                          : lower(trim(content.substr(0, equals)));
        const auto value = equals == std::string_view::npos ? std::string_view{}
                                                            : trim(content.substr(equals + 1));
        bool understood = false;
        if (key == "level") {
            if (const auto level = parseLevel(value)) {
                config.level = *level;
                understood = true;
            }
        } else if (key == "console" || key == "file") {
            if (const auto flag = parseBool(value)) {
                (key == "console" ? config.console : config.file) = *flag;
                understood = true;
            }
        }
        if (!understood) {
            config.issues.push_back(
                std::format("{} line {} ignored: '{}'", path.filename().string(), number, content));
        }
    }
    return config;
}

void start(const Config& config, const std::filesystem::path& logFile, bool interactive) {
    auto& s = state();
    {
        const std::scoped_lock lock(s.mutex);
        s.threshold = interactive ? config.level : std::max(config.level, Level::Warning);
        s.console = !interactive || config.console;
        prepareConsole(s, interactive && config.console);
        if (interactive && config.file && config.level != Level::Off) {
            s.file.open(logFile, std::ios::binary | std::ios::trunc);
        }
    }
    SDL_SetLogOutputFunction(forwardSdlLog, nullptr);
    if (interactive) {
        info("log",
             "level {} | console {} | file {}",
             name(config.level),
             config.console ? "on" : "off",
             s.file.is_open() ? logFile.string() : std::string("off"));
    }
    for (const auto& issue : config.issues) {
        warning("log", "{}", issue);
    }
    if (interactive && config.file && config.level != Level::Off && !s.file.is_open()) {
        warning("log", "could not open log file {}", logFile.string());
    }
}

void stop() {
    auto& s = state();
    const std::scoped_lock lock(s.mutex);
    if (s.file.is_open()) {
        s.file.close();
    }
}

bool enabled(Level level) {
    return level != Level::Off && level >= state().threshold;
}

void write(Level level, std::string_view category, std::string_view message) {
    auto& s = state();
    const auto line =
        std::format("[{}] [{:<7}] [{}] {}", timestamp(), name(level), category, message);
    const std::scoped_lock lock(s.mutex);
    if (s.console) {
        emitConsole(s, level, line);
    }
    if (s.file.is_open()) {
        s.file << line << '\n';
        s.file.flush();
    }
}
} // namespace ftl::logging
