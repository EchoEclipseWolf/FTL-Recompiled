#pragma once
#include "resources.hpp"
#include "travel.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ftl {
// Owns XML documents so node handles remain valid for nested event choices.
class EventCatalog {
    std::vector<pugi::xml_document> documents_;
    std::map<std::string, pugi::xml_node> events_;
    std::map<std::string, pugi::xml_node> lists_;
    std::map<std::string, pugi::xml_node> shipEvents_;
    std::map<std::string, pugi::xml_node> shipEventLists_;
    std::map<std::string, pugi::xml_node> texts_;
    std::map<std::string, std::vector<pugi::xml_node>> textEntries_;
    std::map<std::string, pugi::xml_node> sectors_;
    std::map<std::string, pugi::xml_node> sectorTypes_;
    // Final EventsParser name counter (1845 for the shipped ftl.dat).
    std::uint32_t originalNames_ = 0;

public:
    explicit EventCatalog(const Archive& archive);
    pugi::xml_node event(const std::string& name) const;
    pugi::xml_node shipEvent(const std::string& name) const;
    pugi::xml_node sector(const std::string& name) const;
    pugi::xml_node imageList(const std::string& name) const;
    std::string selectSector(int type,
                             int level,
                             bool advanced,
                             std::vector<std::string>& used,
                             RandomStream& random) const;

    std::uint32_t originalNameCount() const {
        return originalNames_;
    }

    std::vector<std::string> eventIds() const {
        std::vector<std::string> ids;
        ids.reserve(events_.size());
        for (const auto& entry : events_) {
            ids.push_back(entry.first);
        }
        return ids;
    }

    pugi::xml_node resolve(pugi::xml_node node, RandomStream& random) const;
    // LocationEvent::ClearEvent0x5ec800 operates on a materialized event.
    // _nativeBeacon records the runtime exit flag; cachedShip is the retained
    // actualBlueprint state rather than the presence of an authored ship tag.
    void clearLocationEvent(pugi::xml_node event, bool force, bool cachedShip) const;
    std::string materialize(pugi::xml_node node,
                            RandomStream& random,
                            bool advanced,
                            std::vector<std::string>& used,
                            const std::function<void(pugi::xml_node, RandomStream&)>& freeze = {},
                            bool ignoreUnique = false,
                            int forceRandomIndex = 0,
                            const std::function<std::string(pugi::xml_node)>& textValue = {}) const;
    std::string text(pugi::xml_node node,
                     RandomStream& random,
                     TextLibrary& library,
                     const std::string& locale) const;

    // The TextString a <text> node resolves to: a text id, or literal text.
    struct TextSource {
        bool literal = true;
        std::string value;
    };

    TextSource textSource(pugi::xml_node node, RandomStream& random) const;
};
} // namespace ftl
