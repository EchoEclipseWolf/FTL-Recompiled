#pragma once
#include "archive.hpp"
#include <array>
#include <cstdint>
#include <pugixml.hpp>
#include <string_view>

namespace ftl {
inline constexpr std::array<std::string_view, 9> languages{
    "en", "de", "es", "fr", "it", "pl", "pt", "ru", "zh-Hans"};
bool supportedLanguage(std::string_view language);
bool sourceNameCharacterAllowed(std::uint32_t codepoint, std::string_view locale);
pugi::xml_document xmlResource(const Archive& archive, const std::string& path);

class TextLibrary {
    const Archive& archive_;
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        dictionaries_;
    void load(const std::string& language) const;

public:
    explicit TextLibrary(const Archive& archive) : archive_(archive) {}

    std::string get(const std::string& name, const std::string& language = "en") const;
};
} // namespace ftl
