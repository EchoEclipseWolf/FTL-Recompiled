#pragma once
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Native diagnostic logging. This has no original-game counterpart; it writes
// to the console (stderr) and an optional per-executable log file.
namespace ftl::logging {
enum class Level : std::uint8_t { Verbose, Info, Warning, Error, Off };

struct Config {
    Level level = Level::Verbose;
    bool console = true;
    bool file = true;
    // Unreadable lines from the settings file, reported once logging starts.
    std::vector<std::string> issues;
};

std::string_view name(Level level);
// Reads the key=value logging settings file, writing the defaults when absent.
Config readConfig(const std::filesystem::path& path);
// interactive: show (or allocate) the console window and honour the configured
// level. Automation runs keep stdout for protocol replies and receive only
// warnings and errors on stderr, without a log file.
void start(const Config& config, const std::filesystem::path& logFile, bool interactive);
void stop();
bool enabled(Level level);
void write(Level level, std::string_view category, std::string_view message);

template <class... Args>
void verbose(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Verbose)) {
        write(Level::Verbose, category, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void info(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Info)) {
        write(Level::Info, category, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void warning(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Warning)) {
        write(Level::Warning, category, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args>
void error(std::string_view category, std::format_string<Args...> format, Args&&... args) {
    if (enabled(Level::Error)) {
        write(Level::Error, category, std::format(format, std::forward<Args>(args)...));
    }
}

// Records one pointer-click resolution. The last hit() before destruction is
// logged at Verbose, so every early return in a click handler is reported.
class ClickTrace {
public:
    ClickTrace(std::string_view owner, float x, float y, bool right)
        : owner_(owner), x_(x), y_(y), right_(right) {}

    ClickTrace(const ClickTrace&) = delete;
    ClickTrace& operator=(const ClickTrace&) = delete;

    ~ClickTrace() {
        if (enabled(Level::Verbose)) {
            write(Level::Verbose,
                  "click",
                  std::format("{} {} ({:.1f},{:.1f}) -> {}",
                              owner_,
                              right_ ? "right" : "left",
                              x_,
                              y_,
                              target_.empty() ? "nothing" : target_));
        }
    }

    void hit(std::string target) {
        target_ = std::move(target);
    }

private:
    std::string_view owner_;
    float x_;
    float y_;
    bool right_;
    std::string target_;
};
} // namespace ftl::logging
