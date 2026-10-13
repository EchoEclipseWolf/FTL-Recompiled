#include "resources.hpp"
#include <algorithm>
#include <stdexcept>

namespace ftl {
bool supportedLanguage(std::string_view language) {
    return std::find(languages.begin(), languages.end(), language) != languages.end();
}

bool sourceNameCharacterAllowed(std::uint32_t codepoint, std::string_view locale) {
    // TextInput::OnTextInput4ef6d0 ALLOW_LANGUAGE and the265-entry source table.
    if (codepoint >= 32 && codepoint <= 126) {
        return true;
    }
    if (locale.starts_with("zh")) {
        return (codepoint >= 8192 && codepoint <= 8303) || codepoint == 12288 ||
               (codepoint >= 19968 && codepoint <= 40959);
    }
    return codepoint == 160 || codepoint == 161 || codepoint == 171 || codepoint == 187 ||
           codepoint == 191 || (codepoint >= 192 && codepoint <= 382) || codepoint == 1025 ||
           (codepoint >= 1040 && codepoint <= 1103) || codepoint == 1105 || codepoint == 8221 ||
           codepoint == 8222 || codepoint == 8364;
}

pugi::xml_document xmlResource(const Archive& archive, const std::string& path) {
    auto bytes = archive.read(path);
    pugi::xml_document doc;
    const auto result = doc.load_buffer(bytes.data(),
                                        bytes.size(),
                                        pugi::parse_default | pugi::parse_fragment,
                                        pugi::encoding_utf8);
    if (!result) {
        throw std::runtime_error("invalid_xml: " + path + ": " + result.description());
    }
    return doc;
}

void TextLibrary::load(const std::string& language) const {
    if (!supportedLanguage(language)) {
        throw std::runtime_error("unsupported_language");
    }
    auto& dict = dictionaries_[language];
    const std::vector<std::string> paths =
        language == "en" ? std::vector<std::string>{"data/text_misc.xml",
                                                    "data/text_blueprints.xml",
                                                    "data/text_tooltips.xml",
                                                    "data/text_achievements.xml",
                                                    "data/text_events.xml",
                                                    "data/text_sectorname.xml",
                                                    "data/text_tutorial.xml"}
                         : std::vector<std::string>{"data/text-" + language + ".xml"};
    for (const auto& path : paths) {
        auto doc = xmlResource(archive_, path);
        auto root = doc.child("FTL");
        if (!root) {
            root = doc;
        }
        for (auto node : root.children("text")) {
            auto name = node.attribute("name").as_string();
            std::string value = node.child_value();
            for (std::size_t p = 0; (p = value.find("\\n", p)) != std::string::npos; ++p) {
                value.replace(p, 2, "\n");
            }
            dict[name] = std::move(value);
        }
    }
}

std::string TextLibrary::get(const std::string& name, const std::string& language) const {
    if (!dictionaries_.contains(language)) {
        load(language);
    }
    auto& dict = dictionaries_.at(language);
    if (auto it = dict.find(name); it != dict.end()) {
        return it->second;
    }
    if (language != "en") {
        return get(name, "en");
    }
    // TextLibrary::GetText0x5a9f40 falls back to the base dictionary first,
    // then returns this literal diagnostic text for an absent key.
    return "Could not find: " + name;
}
} // namespace ftl
