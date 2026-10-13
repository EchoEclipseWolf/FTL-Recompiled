#include "events.hpp"
#include <algorithm>
#include <array>
#include <functional>
#include <iostream>
#include <iterator>
#include <json.hpp>
#include <sstream>
#include <stdexcept>

namespace ftl {
namespace {
bool eventBoolean(pugi::xml_attribute attribute) {
    const std::string_view value = attribute.value();
    // EventsParser::ParseBoolean0x5be000 accepts only these three strings.
    return value == "true" || value == "TRUE" || value == "True";
}

void normalizeEventBoolean(pugi::xml_node node, const char* name) {
    if (auto attribute = node.attribute(name)) {
        attribute.set_value(eventBoolean(attribute) ? "true" : "false");
    }
}
} // namespace

EventCatalog::EventCatalog(const Archive& archive) {
    std::uint64_t inlineShipIndex = 0;
    std::uint64_t inlineEventIndex = 0;
    // EventsParser's shared name counter t_ (0x8cbba4): it starts at 0, is never
    // reset, and names every anonymous node the original parses
    // (docs/research/original-save-format/event-names.md). _originalName records
    // the name the original gives each event, ship and outcome node.
    std::function<std::string(pugi::xml_node, const std::string&)> originalEvent;
    std::function<std::string(pugi::xml_node)> originalShip;
    // ProcessEvent 0x5c11f0 / ProcessChoice 0x5c3c70: a load= reference first,
    // then the name, then <ship> children and each choice's first <event>.
    originalEvent = [&](pugi::xml_node node, const std::string& parent) {
        if (const auto load = node.attribute("load")) {
            return std::string(load.value());
        }
        const auto named = node.attribute("name");
        const std::string name =
            named ? named.value() : parent + " " + std::to_string(++originalNames_);
        node.append_attribute("_originalName").set_value(name.c_str());
        for (auto child : node.children()) {
            if (std::string_view(child.name()) == "ship") {
                originalShip(child);
            } else if (std::string_view(child.name()) == "choice") {
                if (auto event = child.child("event")) {
                    originalEvent(event, name);
                }
            }
        }
        return name;
    };
    // ProcessShipEvent 0x5bfc40: outcomes in this fixed order, first of each tag.
    originalShip = [&](pugi::xml_node node) {
        if (const auto load = node.attribute("load")) {
            return std::string(load.value());
        }
        const auto named = node.attribute("name");
        const std::string name = named ? named.value() : std::to_string(++originalNames_);
        node.append_attribute("_originalName").set_value(name.c_str());
        static constexpr std::array<std::pair<const char*, const char*>, 5> outcomes{
            {{"surrender", " surrender "},
             {"escape", " escape "},
             {"deadCrew", " dead crew "},
             {"destroyed", " destroyed "},
             {"gotaway", " got away "}}};
        for (const auto& [tag, suffix] : outcomes) {
            if (auto outcome = node.child(tag)) {
                originalEvent(outcome, name + suffix);
            }
        }
        return name;
    };
    // EventsParser::AddAllEvents 0x5c5ed0 unconditionally loads these in order.
    // Sector data is separately loaded by StarMap::GenerateSectorMap.
    for (const auto* file : {"events",
                             "dlcEvents",
                             "dlcEvents_anaerobic",
                             "newEvents",
                             "events_rebel",
                             "events_mantis",
                             "events_engi",
                             "events_pirate",
                             "events_fuel",
                             "events_nebula",
                             "events_ships",
                             "events_imageList",
                             "events_crystal",
                             "events_boss",
                             "events_zoltan",
                             "events_slug",
                             "events_rock",
                             "dlcEventsOverwrite",
                             "sector_data"}) {
        documents_.push_back(xmlResource(archive, std::string("data/") + file + ".xml"));
        auto root = documents_.back().child("FTL");
        if (!root) {
            root = documents_.back();
        }
        for (auto node : root.children()) {
            const std::string name = node.attribute("name").as_string();
            const std::string kind = node.name();
            if (name.empty()) {
                continue;
            }
            if (kind == "event") {
                events_[name] = node;
            } else if (kind == "eventList") {
                lists_[name] = node;
            } else if (kind == "ship") {
                shipEvents_[name] = node;
            } else if (kind == "shipList") {
                shipEventLists_[name] = node;
                // ProcessShipList 0x5c1090 also registers its inline templates.
                for (auto ship : node.children("ship")) {
                    if (!ship.attribute("load") && ship.attribute("name")) {
                        shipEvents_[ship.attribute("name").as_string()] = ship;
                    }
                }
            } else if (kind == "textList") {
                texts_[name] = node;
                // ProcessRandomText0x5bf6c0 copies referenced lists now, in
                // authored order. References are not additional runtime draws.
                std::vector<pugi::xml_node> entries;
                for (auto text : node.children("text")) {
                    if (auto load = text.attribute("load")) {
                        const auto prior = textEntries_.find(load.as_string());
                        if (prior != textEntries_.end()) {
                            entries.insert(
                                entries.end(), prior->second.begin(), prior->second.end());
                        } else {
                            std::clog << "Unknown text list while parsing: " << load.value()
                                      << '\n';
                        }
                    } else {
                        entries.push_back(text);
                    }
                }
                textEntries_[name] = std::move(entries);
            } else if (kind == "sectorDescription") {
                sectors_[name] = node;
            } else if (kind == "sectorType") {
                sectorTypes_[name] = node;
            }
        }
        // ProcessShipEvent registers anonymous inline templates at parse time.
        // Native staging keys are stable across this authored traversal; the
        // original shared numeric event-name counter remains separate evidence.
        std::function<void(pugi::xml_node)> registerShips;
        registerShips = [&](pugi::xml_node node) {
            if ((std::string_view(node.name()) == "ship" ||
                 std::string_view(node.name()) == "event") &&
                node.attribute("load")) {
                return;
            }
            if (std::string_view(node.name()) == "ship" && !node.attribute("load")) {
                normalizeEventBoolean(node, "hostile");
                std::string key = node.attribute("name").as_string();
                if (key.empty()) {
                    key = "_nativeInlineShip_" + std::to_string(++inlineShipIndex);
                }
                shipEvents_[key] = node;
                node.append_attribute("_nativeInlineShipTemplate").set_value(key.c_str());
            }
            for (auto child : node.children()) {
                registerShips(child);
            }
        };
        registerShips(root);
        // ProcessEvent registers every nested template after parsing its
        // children. Anonymous native keys retain identity without depending
        // on original x86 parser counter strings.
        std::function<void(pugi::xml_node)> registerEvents;
        registerEvents = [&](pugi::xml_node node) {
            const std::string_view kind = node.name();
            // Sector descriptions and eventCounts contain placement counts
            // named "event"; they must not replace encounter templates.
            if (kind == "sectorDescription" || kind == "sectorType" || kind == "eventCounts") {
                return;
            }
            const bool outcome = std::string_view(node.parent().name()) == "ship" &&
                                 (kind == "surrender" || kind == "escape" || kind == "destroyed" ||
                                  kind == "deadCrew" || kind == "gotaway") &&
                                 node == node.parent().child(node.name());
            const bool eventNode = kind == "event" || outcome;
            if ((eventNode || kind == "ship") && node.attribute("load")) {
                return;
            }
            std::string key;
            if (eventNode) {
                key = node.attribute("name").as_string();
                if (key.empty()) {
                    key = "_nativeEvent_" + std::to_string(++inlineEventIndex);
                }
                node.append_attribute("_nativeEventTemplate").set_value(key.c_str());
            }
            for (auto child : node.children()) {
                registerEvents(child);
            }
            if (eventNode) {
                events_[key] = node;
            }
        };
        registerEvents(root);
        // ProcessBaseNode 0x5c4300 names any unnamed top-level node, events or not.
        // AddAllEvents 0x5c5ed0 loads the 18 event files; sector_data names nothing.
        if (std::string_view(file) == "sector_data") {
            continue;
        }
        for (auto node : root.children()) {
            if (node.type() != pugi::node_element) {
                continue;
            }
            const auto named = node.attribute("name");
            const std::string name =
                named ? named.value() : "Base Event: " + std::to_string(++originalNames_);
            const std::string_view kind = node.name();
            if (kind == "event" || kind == "baseEvent") {
                originalEvent(node, name);
            } else if (kind == "eventList") {
                for (auto child : node.children("event")) {
                    originalEvent(child, name);
                }
            } else if (kind == "ship") {
                originalShip(node);
            } else if (kind == "shipList") {
                for (auto child : node.children("ship")) {
                    originalShip(child);
                }
            }
        }
    }
}

pugi::xml_node EventCatalog::imageList(const std::string& name) const {
    for (const auto& document : documents_) {
        auto root = document.child("FTL");
        if (!root) {
            root = document;
        }
        for (auto node : root.children("imageList")) {
            if (name == node.attribute("name").as_string() && !node.attribute("ui")) {
                return node;
            }
        }
    }
    return {};
}

pugi::xml_node EventCatalog::sector(const std::string& name) const {
    const auto found = sectors_.find(name);
    if (found == sectors_.end()) {
        throw std::runtime_error("unknown_sector: " + name);
    }
    return found->second;
}

std::string EventCatalog::selectSector(int type,
                                       int level,
                                       bool advanced,
                                       std::vector<std::string>& used,
                                       RandomStream& random) const {
    // EventGenerator::GetSectorDescription 0x5b6330 filters minSector and
    // previously allocated unique sectors before drawing from the category.
    const auto category = type == 2 ? "NEBULA" : type == 1 ? "HOSTILE" : "CIVILIAN";
    auto found = sectorTypes_.find(advanced ? std::string("OVERRIDE_") + category : category);
    if (found == sectorTypes_.end()) {
        found = sectorTypes_.find(category);
    }
    if (found == sectorTypes_.end()) {
        throw std::runtime_error("unknown_sector_category");
    }
    std::vector<std::string> candidates;
    for (auto entry : found->second.children("sector")) {
        std::string name = entry.text().as_string();
        const auto definition = sector(name);
        if (definition.attribute("minSector").as_int() <= level &&
            std::find(used.begin(), used.end(), name) == used.end()) {
            candidates.push_back(std::move(name));
        }
    }
    if (candidates.empty()) {
        throw std::runtime_error("sector_category_exhausted");
    }
    auto selected = candidates[random.next() % candidates.size()];
    if (sector(selected).attribute("unique").as_bool()) {
        used.push_back(selected);
    }
    return selected;
}

pugi::xml_node EventCatalog::event(const std::string& name) const {
    if (auto found = events_.find(name); found != events_.end()) {
        return found->second;
    }
    if (auto found = lists_.find(name); found != lists_.end()) {
        return found->second;
    }
    throw std::runtime_error("unknown_event: " + name);
}

pugi::xml_node EventCatalog::shipEvent(const std::string& name) const {
    if (auto found = shipEvents_.find(name); found != shipEvents_.end()) {
        return found->second;
    }
    return {};
}

std::string
EventCatalog::materialize(pugi::xml_node node,
                          RandomStream& random,
                          bool advanced,
                          std::vector<std::string>& used,
                          const std::function<void(pugi::xml_node, RandomStream&)>& freeze,
                          bool ignoreUnique,
                          int forceRandomIndex,
                          const std::function<std::string(pugi::xml_node)>& textValue) const {
    // GetBaseEvent/CreateEvent 0x5bb240/0x5b8940 build choice trees eagerly.
    // Preserve selected XML so entering or revisiting cannot reroll lists.
    pugi::xml_document document;
    pugi::xml_document fallbackTexts;
    std::function<bool(pugi::xml_node, pugi::xml_node, int)> build;
    auto selectedText = [&](pugi::xml_node textNode, bool consume) {
        if (textNode.attribute("load")) {
            const std::string name = textNode.attribute("load").as_string();
            const auto found = textEntries_.find(name);
            std::vector<std::pair<pugi::xml_node, std::string>> choices;
            int index = 0;
            if (found != textEntries_.end()) {
                for (auto candidate : found->second) {
                    const auto key = "text:" + found->first + ":" + std::to_string(index++);
                    if (std::find(used.begin(), used.end(), key) == used.end()) {
                        choices.emplace_back(candidate, key);
                    }
                }
            }
            if (choices.empty()) {
                if (!consume) {
                    // Source asserts for an unknown choice list; empty choice
                    // lists would divide by zero. Keep a bounded diagnostic.
                    throw std::runtime_error("empty_choice_text_list: " + name);
                }
                // A primary list failure logs and retains its literal name.
                std::clog << "Ran out of texts for text list: " << name << '\n';
                auto fallback = fallbackTexts.append_child("text");
                fallback.text().set(name.c_str());
                return fallback;
            }
            const auto selection = consume && forceRandomIndex > 0
                                       ? static_cast<std::uint32_t>(forceRandomIndex - 1)
                                       : random.next();
            const auto& chosen = choices[selection % choices.size()];
            textNode = chosen.first;
            // CreateEvent moves selected texts aside while more than one remains.
            // CreateEvent removes only the primary event text from the pool.
            // A choice's loaded text reads that pool without removing its draw.
            if (consume && !ignoreUnique && choices.size() > 1) {
                used.push_back(chosen.second);
            }
        }
        return textNode;
    };
    build = [&](pugi::xml_node source, pugi::xml_node output, int depth) -> bool {
        if (depth >= 64) {
            throw std::runtime_error("event_reference_cycle");
        }
        for (int reference = 0; reference < 256; ++reference) {
            if (source.attribute("load")) {
                const std::string name = source.attribute("load").as_string();
                // Overrides apply only while resolving an actual event list.
                const auto list = lists_.find(name);
                const auto concrete = events_.find(name);
                if (list != lists_.end()) {
                    source = list->second;
                } else if (concrete != events_.end()) {
                    source = concrete->second;
                } else {
                    return false;
                }
            } else if (std::string_view(source.name()) == "eventList") {
                const auto selection = forceRandomIndex > 0
                                           ? static_cast<std::uint32_t>(forceRandomIndex - 1)
                                           : random.next();
                const std::string name = source.attribute("name").as_string();
                const auto replacement = advanced ? lists_.find("OVERRIDE_" + name) : lists_.end();
                const auto list = replacement == lists_.end() ? source : replacement->second;
                std::vector<pugi::xml_node> candidates;
                for (auto candidate : list.children("event")) {
                    candidates.push_back(candidate);
                }
                if (candidates.empty()) {
                    throw std::runtime_error("event_list_exhausted");
                }
                source = candidates[selection % candidates.size()];
            } else {
                break;
            }
        }
        if (source.attribute("load") || std::string_view(source.name()) == "eventList") {
            throw std::runtime_error("event_reference_cycle");
        }
        const std::string name = source.attribute("name")
                                     ? source.attribute("name").as_string()
                                     : source.attribute("_nativeEventTemplate").as_string();
        if (!name.empty() && std::find(used.begin(), used.end(), name) != used.end()) {
            return false;
        }
        for (auto attribute : source.attributes()) {
            output.append_attribute(attribute.name()).set_value(attribute.value());
        }
        normalizeEventBoolean(output, "unique");
        pugi::xml_node primarySource;
        bool loadPrimaryText = false;
        for (auto textNode : source.children("text")) {
            primarySource = textNode;
            loadPrimaryText =
                loadPrimaryText || std::string_view(textNode.first_attribute().name()) == "load";
        }
        const bool primaryLoadAttribute =
            std::string_view(primarySource.first_attribute().name()) == "load";
        auto parsedPrimary =
            primarySource ? fallbackTexts.append_copy(primarySource) : pugi::xml_node{};
        if (parsedPrimary && !primaryLoadAttribute) {
            // ParseTextNode reads id or literal data; only a FIRST load
            // attribute takes ProcessEvent's additional load-list branch.
            parsedPrimary.remove_attribute("load");
        }
        pugi::xml_node lastCrew;
        for (auto child : source.children()) {
            if (std::string_view(child.name()) == "crewMember" ||
                std::string_view(child.name()) == "removeCrew") {
                lastCrew = child;
            }
        }
        const bool removesCrew = lastCrew && (std::string_view(lastCrew.name()) == "removeCrew" ||
                                              lastCrew.attribute("amount").as_int() == -1);
        const std::string_view parsedData =
            primaryLoadAttribute            ? primarySource.attribute("load").value()
            : parsedPrimary.attribute("id") ? parsedPrimary.attribute("id").value()
                                            : parsedPrimary.child_value();
        if (removesCrew && parsedData.empty()) {
            parsedPrimary = fallbackTexts.append_child("text");
            parsedPrimary.append_attribute("id").set_value("default_dead_crew");
        }
        if (loadPrimaryText && parsedPrimary &&
            (!primaryLoadAttribute || (removesCrew && parsedData.empty()))) {
            if (parsedPrimary.attribute("id") && !textValue) {
                throw std::runtime_error("event_localized_text_resolver_required");
            }
            const auto listName =
                textValue ? textValue(parsedPrimary) : std::string(parsedPrimary.child_value());
            parsedPrimary = fallbackTexts.append_child("text");
            parsedPrimary.append_attribute("load").set_value(listName.c_str());
        }
        const auto primaryText =
            parsedPrimary ? selectedText(parsedPrimary, true) : pugi::xml_node{};
        if (primaryText) {
            output.append_copy(primaryText);
        }
        for (auto child : source.children()) {
            if (std::string_view(child.name()) != "text" &&
                std::string_view(child.name()) != "choice") {
                output.append_copy(child);
            }
        }
        // ProcessEvent0x5c11f0 assigns scalar fields as it visits each tag.
        // Damage/status/choices are vectors; resource ranges are merged below.
        // A quest without its event attribute is ignored by ProcessEvent,
        // retaining an earlier quest assignment.
        for (auto quest = output.child("quest"); quest;) {
            const auto next = quest.next_sibling("quest");
            if (!quest.attribute("event")) {
                output.remove_child(quest);
            }
            quest = next;
        }
        for (const auto* tag : {"weapon",
                                "drone",
                                "augment",
                                "autoReward",
                                "ship",
                                "quest",
                                "modifyPursuit",
                                "upgrade",
                                "unlockShip",
                                "remove"}) {
            auto scalar = output.child(tag);
            while (scalar && scalar.next_sibling(tag)) {
                const auto next = scalar.next_sibling(tag);
                output.remove_child(scalar);
                scalar = next;
            }
        }
        if (auto ship = output.child("ship")) {
            const bool hostile = eventBoolean(ship.attribute("hostile"));
            auto attribute = ship.attribute("hostile");
            if (!attribute) {
                attribute = ship.append_attribute("hostile");
            }
            // ProcessShipEvent returns a false default even for load refs;
            // CreateEvent overwrites the copied profile's hostile value.
            attribute.set_value(hostile ? "true" : "false");
        }
        // Img fields update independently; omitted attributes retain the
        // preceding descriptor, including explicit empty-string assignments.
        if (auto image = output.child("img")) {
            while (auto next = image.next_sibling("img")) {
                for (const auto* field : {"back", "planet"}) {
                    if (const auto attribute = next.attribute(field)) {
                        auto value = image.attribute(field);
                        if (!value) {
                            value = image.append_attribute(field);
                        }
                        value.set_value(attribute.value());
                    }
                }
                output.remove_child(next);
            }
        }
        // Unknown environment/fleet spellings do not clear the previous
        // recognized enum. Environment targets are independent assignments.
        std::string environmentType;
        std::string environmentTarget = "all";
        pugi::xml_node fleet;
        for (auto child : source.children()) {
            const std::string_view kind = child.name();
            if (kind == "environment") {
                const std::string_view type = child.attribute("type").as_string();
                if (type == "asteroid" || type == "sun" || type == "nebula" || type == "storm" ||
                    type == "pulsar" || type == "PDS") {
                    environmentType = type;
                }
                const std::string_view target = child.attribute("target").as_string();
                if (target == "player" || target == "enemy" || target == "all") {
                    environmentTarget = target;
                }
            } else if (kind == "fleet") {
                const std::string_view value = child.child_value();
                if (value == "rebel" || value == "fed" || value == "battle") {
                    fleet = child;
                }
            }
        }
        while (output.remove_child("environment")) {
        }
        if (!environmentType.empty()) {
            auto environment = output.append_child("environment");
            environment.append_attribute("type").set_value(environmentType.c_str());
            environment.append_attribute("target").set_value(environmentTarget.c_str());
        }
        while (output.remove_child("fleet")) {
        }
        if (fleet) {
            output.append_copy(fleet);
        }
        // BoardingEvent attributes retain prior values when omitted later.
        if (auto boarding = output.child("boarders")) {
            while (auto next = boarding.next_sibling("boarders")) {
                for (auto attribute : next.attributes()) {
                    auto value = boarding.attribute(attribute.name());
                    if (!value) {
                        value = boarding.append_attribute(attribute.name());
                    }
                    value.set_value(attribute.value());
                }
                output.remove_child(next);
            }
            normalizeEventBoolean(boarding, "breach");
        }
        // Missing death prose receives a nonliteral fallback text key.
        pugi::xml_node crewDefinition;
        pugi::xml_node cloneDefinition;
        pugi::xml_attribute crewClass;
        std::map<std::string, int> crewSkills;
        int crewDefinitions = 0;
        for (auto child : source.children()) {
            if (std::string_view(child.name()) == "crewMember" ||
                std::string_view(child.name()) == "removeCrew") {
                crewDefinition = child;
                if (std::string_view(child.name()) == "removeCrew") {
                    cloneDefinition = child;
                }
                ++crewDefinitions;
                if (auto species = child.attribute("class")) {
                    crewClass = species;
                }
                if (std::string_view(child.name()) == "crewMember") {
                    for (const auto* skill :
                         {"pilot", "engines", "shields", "weapons", "repair", "combat"}) {
                        const auto level = child.attribute("all_skills")
                                               ? child.attribute("all_skills")
                                               : child.attribute(skill);
                        if (level) {
                            crewSkills[skill] = level.as_int();
                        }
                    }
                }
            }
        }
        if (crewDefinitions > 1) {
            // Crew amount/type/name share one resource field; skill rows
            // accumulate separately and later assignments replace each XP row.
            while (output.remove_child("crewMember")) {
            }
            while (output.remove_child("removeCrew")) {
            }
            auto crew = output.append_copy(crewDefinition);
            if (crewClass && !crew.attribute("class")) {
                crew.append_attribute("class").set_value(crewClass.value());
            }
            if (std::string_view(crew.name()) == "crewMember") {
                crew.remove_attribute("all_skills");
                for (const auto& [skill, level] : crewSkills) {
                    auto value = crew.attribute(skill.c_str());
                    if (!value) {
                        value = crew.append_attribute(skill.c_str());
                    }
                    value.set_value(level);
                }
            }
        }
        if (crewDefinition) {
            // Clone eligibility/prose are independent ResourcesTemplate fields:
            // later crewMember updates retain the last removeCrew assignment.
            auto crew = output.child(std::string_view(crewDefinition.name()) == "removeCrew"
                                         ? "removeCrew"
                                         : "crewMember");
            auto cloneable = crew.attribute("_nativeCloneable");
            if (!cloneable) {
                cloneable = crew.append_attribute("_nativeCloneable");
            }
            cloneable.set_value(std::string_view(cloneDefinition.child_value("clone")) == "true");
            crew.remove_child("_nativeCloneText");
            if (auto prose = cloneDefinition.child("text")) {
                crew.append_copy(prose).set_name("_nativeCloneText");
            }
        }
        // CreateEvent 0x5b8940 lets the selected EventText override the
        // template's planet/background descriptor before location creation.
        for (const auto* attribute : {"planet", "back"}) {
            const auto value = primaryText.attribute(attribute);
            if (!value || std::string_view(value.value()).empty()) {
                continue;
            }
            auto image = output.child("img");
            if (!image) {
                image = output.append_child("img");
            }
            auto target = image.attribute(attribute);
            if (!target) {
                target = image.append_attribute(attribute);
            }
            target.set_value(value.value());
        }
        // CreateEvent 0x5b8940 always makes all four selection checks, even
        // with no item_modify. PullMinMax defaults both endpoints to zero and
        // normalizes their order; SelectAmount draws when max is nonzero.
        const auto selectAmount = [&](pugi::xml_node range) {
            int minimum = range.attribute("min").as_int();
            int maximum = range.attribute("max").as_int();
            if (minimum > maximum) {
                std::swap(minimum, maximum);
            }
            if (minimum < -10000 || maximum > 10000) {
                throw std::runtime_error("invalid_event_resource_range");
            }
            const float chance = range.attribute("chance").as_float(-1.f);
            if (chance > 0.f && static_cast<float>(random.next() % 100) < chance * 100.f) {
                return 0;
            }
            return minimum + (maximum == 0
                                  ? 0
                                  : static_cast<int>(random.next() % static_cast<std::uint32_t>(
                                                                         maximum - minimum + 1)));
        };
        pugi::xml_node itemLimit;
        std::map<std::string, pugi::xml_node> resourceRanges;
        for (auto modify : output.children("item_modify")) {
            itemLimit = modify;
            for (auto item : modify.children()) {
                resourceRanges[item.attribute("type").as_string()] = item;
            }
        }
        int remaining =
            itemLimit.attribute("min").as_int() != 0 || itemLimit.attribute("max").as_int() != 0
                ? selectAmount(itemLimit)
                : 100;
        const auto resourceRange = [&](const char* type) {
            const auto found = resourceRanges.find(type);
            return found == resourceRanges.end() ? pugi::xml_node{} : found->second;
        };
        std::map<std::string, int> quantities;
        int divisor = 4;
        for (const auto* type : {"fuel", "drones", "missiles"}) {
            const auto check = random.next() % static_cast<std::uint32_t>(divisor--);
            const int value =
                remaining > static_cast<int>(check) ? selectAmount(resourceRange(type)) : 0;
            quantities[type] = value;
            remaining -= value > 0;
        }
        random.next(); // The fourth selection draw is discarded before scrap.
        quantities["scrap"] = remaining > 0 ? selectAmount(resourceRange("scrap")) : 0;
        if (auto modify = output.child("item_modify")) {
            // Source fields replace duplicate types rather than accumulating.
            modify.remove_children();
            auto steal = modify.attribute("steal");
            if (!steal) {
                steal = modify.append_attribute("steal");
            }
            steal.set_value(eventBoolean(itemLimit.attribute("steal")));
            for (const auto* type : {"fuel", "drones", "missiles", "scrap"}) {
                if (!resourceRanges.contains(type)) {
                    continue;
                }
                auto item = modify.append_child("item");
                item.append_attribute("type").set_value(type);
                item.append_attribute("min").set_value(quantities[type]);
                item.append_attribute("max").set_value(quantities[type]);
            }
            while (auto duplicate = modify.next_sibling("item_modify")) {
                output.remove_child(duplicate);
            }
        }
        if (freeze) {
            freeze(output, random);
        }
        // CreateEvent 0x5b8940 copies the ship template, then freezes its seed.
        if (auto ship = output.child("ship")) {
            std::string shipName = ship.attribute("load").as_string();
            if (const auto list = shipEventLists_.find(shipName); list != shipEventLists_.end()) {
                std::vector<pugi::xml_node> entries;
                for (auto entry : list->second.children("ship")) {
                    entries.push_back(entry);
                }
                if (entries.empty()) {
                    throw std::runtime_error("empty_ship_event_list");
                }
                const auto selected = entries[random.next() % entries.size()];
                shipName = selected.attribute("load")
                               ? selected.attribute("load").as_string()
                               : selected.attribute("_nativeInlineShipTemplate").as_string();
                ship.attribute("load").set_value(shipName.c_str());
            }
            if (!ship.attribute("load") || shipEvent(shipName)) {
                ship.append_attribute("_nativeShipSeed").set_value(random.next());
            }
        }
        for (auto choice : source.children("choice")) {
            auto copy = output.append_child("choice");
            for (auto attribute : choice.attributes()) {
                copy.append_attribute(attribute.name()).set_value(attribute.value());
            }
            normalizeEventBoolean(copy, "hidden");
            normalizeEventBoolean(copy, "blue");
            // Source constructs the child event before the choice's text.
            if (auto child = choice.child("event")) {
                if (!build(child, copy.append_child("event"), depth + 1)) {
                    return false;
                }
            }
            if (auto textNode = choice.child("text")) {
                copy.append_copy(selectedText(textNode, false));
            }
        }
        // The template defaults amount to -1, including events without a
        // boarders tag. CreateEvent rolls that default zero range after choices.
        auto boarders = output.child("boarders");
        if (boarders.attribute("amount").as_int(-1) == -1) {
            const int minimum = boarders.attribute("min").as_int();
            const int maximum = boarders.attribute("max").as_int();
            if (minimum < 0 || maximum < minimum || maximum > 100) {
                throw std::runtime_error("invalid_event_boarder_range");
            }
            const int count =
                minimum +
                static_cast<int>(random.next() % static_cast<std::uint32_t>(maximum - minimum + 1));
            if (boarders) {
                if (!boarders.attribute("amount")) {
                    boarders.append_attribute("amount");
                }
                boarders.attribute("amount").set_value(count);
            }
        }
        // CreateEvent5b8940 copies explicit statuses after boarding, then
        // appends the environment defaults. Storm falls through to nebula.
        // Raw StatusEffect constants837420/837410 both target all ships.
        const auto environmentStatus = [&](const char* type, const char* system, int amount) {
            auto status = output.append_child("status");
            status.append_attribute("type").set_value(type);
            status.append_attribute("system").set_value(system);
            status.append_attribute("amount").set_value(amount);
            status.append_attribute("target").set_value("all");
        };
        if (environmentType == "storm") {
            environmentStatus("divide", "reactor", 2);
        }
        if (environmentType == "storm" || environmentType == "nebula") {
            environmentStatus("limit", "sensors", 0);
        }
        if (!ignoreUnique && eventBoolean(source.attribute("unique")) && !name.empty()) {
            used.push_back(name);
        }
        return true;
    };
    bool prepared = false;
    for (int attempt = 0; attempt < 256 && !prepared; ++attempt) {
        document.reset();
        prepared = build(node, document.append_child("event"), 0);
    }
    if (!prepared) {
        throw std::runtime_error("event_unique_generation_exhausted");
    }
    std::ostringstream xml;
    document.save(xml, "", pugi::format_raw);
    return xml.str();
}

void EventCatalog::clearLocationEvent(pugi::xml_node event, bool force, bool cachedShip) const {
    // ClearEvent5ec800: force bypasses rebel-fleet protection. It does not
    // mean keepStore; only the runtime beacon branch preserves that field.
    if (!event || (!force && std::string_view(event.child_value("fleet")) == "rebel")) {
        return;
    }
    const bool beacon = event.attribute("_nativeBeacon").as_bool();
    const auto eraseTag = [&](const char* tag) {
        while (event.remove_child(tag)) {
        }
    };
    // Native reward freezing represents the original already-resolved
    // ResourceEvent. Clearing must never invoke its generation a second time.
    for (auto child : event.children()) {
        if (auto serialized = child.attribute("_nativeVoyage")) {
            auto frozen = nlohmann::json::parse(serialized.value());
            for (const auto* field : {"scrap", "fuel", "missiles", "drones"}) {
                frozen.at(field) = 0;
            }
            auto& effects = frozen.at("effects");
            effects.erase(std::remove_if(effects.begin(),
                                         effects.end(),
                                         [](const auto& effect) {
                                             const std::string kind =
                                                 effect.at("kind").template get<std::string>();
                                             return kind == "crew" || kind == "removeCrew" ||
                                                    kind == "crewSkills";
                                         }),
                          effects.end());
            serialized.set_value(frozen.dump().c_str());
            auto cleared = child.attribute("_nativeClearedBasicResources");
            if (!cleared) {
                cleared = child.append_attribute("_nativeClearedBasicResources");
            }
            cleared.set_value(true);
            // Equipment IDs in a frozen autoReward are deliberately retained:
            // its augment survives, but its weapon/drone does not. The caller
            // classifies these IDs through BlueprintCatalog while staging.
        } else if (std::string_view(child.name()) == "autoReward") {
            throw std::runtime_error("clear_location_event_requires_frozen_reward");
        }
    }
    for (const auto* tag : {"status", "damage", "weapon", "drone", "crewMember", "removeCrew"}) {
        eraseTag(tag);
    }
    // Source retains the boarder breach flag while clearing its species/count.
    if (auto boarders = event.child("boarders")) {
        for (const auto* field : {"min", "max", "amount"}) {
            auto attribute = boarders.attribute(field);
            if (!attribute) {
                attribute = boarders.append_attribute(field);
            }
            attribute.set_value(0);
        }
        auto species = boarders.attribute("class");
        if (!species) {
            species = boarders.append_attribute("class");
        }
        species.set_value("");
    }
    if (auto resources = event.child("item_modify")) {
        for (const auto* field : {"scrap", "fuel", "missiles", "drones"}) {
            while (resources.remove_child(field)) {
            }
        }
        for (auto item = resources.first_child(); item;) {
            const auto next = item.next_sibling();
            const std::string_view type = item.attribute("type").value();
            if (type == "scrap" || type == "fuel" || type == "missiles" || type == "drones") {
                resources.remove_child(item);
            }
            item = next;
        }
    }
    eraseTag("text");
    event.prepend_child("text"); // Empty literal, without id/load attributes.
    if (beacon) {
        // Only choice[0].event is cleared, and it receives force=false.
        if (auto child = event.child("choice").child("event")) {
            clearLocationEvent(child, false, child.attribute("_nativeCachedShip").as_bool());
        }
        return;
    }
    auto environment = event.child("environment");
    std::string type = environment.attribute("type").as_string();
    const auto appendStatus = [&](const char* operation, const char* system, int amount) {
        auto status = event.append_child("status");
        status.append_attribute("type").set_value(operation);
        status.append_attribute("system").set_value(system);
        status.append_attribute("amount").set_value(amount);
        status.append_attribute("target").set_value("all");
    };
    if (!cachedShip) {
        if (type == "storm" || type == "nebula") {
            environment.attribute("type").set_value("nebula");
            type = "nebula";
        } else {
            // Keep environmentTarget, which ClearEvent never resets.
            if (environment) {
                environment.attribute("type").set_value("normal");
            }
            type.clear();
        }
    }
    // Unlike CreateEvent, cached storm ClearEvent appends ONLY the reactor
    // status. A no-cache storm is converted to nebula and gets sensors only.
    if (type == "storm") {
        appendStatus("divide", "reactor", 2);
    } else if (type == "nebula") {
        appendStatus("limit", "sensors", 0);
    }
    for (const auto* tag :
         {"store", "reveal_map", "distressBeacon", "repair", "modifyPursuit", "choice"}) {
        eraseTag(tag);
    }
}

pugi::xml_node EventCatalog::resolve(pugi::xml_node node, RandomStream& random) const {
    for (int depth = 0; depth < 64; ++depth) {
        if (node.attribute("load")) {
            node = event(node.attribute("load").as_string());
        } else if (std::string_view(node.name()) == "eventList") {
            auto choices = node.children("event");
            const auto count = std::distance(choices.begin(), choices.end());
            if (count <= 0) {
                throw std::runtime_error("empty_event_list");
            }
            auto selected = choices.begin();
            std::advance(selected, random.next() % static_cast<std::uint32_t>(count));
            node = *selected;
        } else {
            return node;
        }
    }
    throw std::runtime_error("event_reference_cycle");
}

std::string EventCatalog::text(pugi::xml_node node,
                               RandomStream& random,
                               TextLibrary& library,
                               const std::string& locale) const {
    const auto source = textSource(node, random);
    return source.literal ? source.value : library.get(source.value, locale);
}

EventCatalog::TextSource EventCatalog::textSource(pugi::xml_node node, RandomStream& random) const {
    if (node.attribute("load")) {
        const auto found = textEntries_.find(node.attribute("load").as_string());
        if (found == textEntries_.end()) {
            throw std::runtime_error("unknown_text_list");
        }
        if (found->second.empty()) {
            throw std::runtime_error("empty_text_list");
        }
        node = found->second[random.next() % found->second.size()];
    }
    if (node.attribute("id")) {
        return {false, node.attribute("id").as_string()};
    }
    return {true, node.child_value()};
}
} // namespace ftl
