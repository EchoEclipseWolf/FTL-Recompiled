#include "run.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>

namespace ftl {
namespace {
int storeIndex(std::string_view value) {
    int result = -1;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? result : -1;
}

struct StoreConfirmation {
    Rect frame, yes, no;
    std::vector<std::string> lines;
    float textHeight;
};

StoreConfirmation confirmationLayout(Renderer& renderer,
                                     const std::string& message,
                                     const std::string& yes,
                                     const std::string& no,
                                     bool chinese) {
    // ConfirmWindow::UpdateSize0x544870 / UpdateButtonPosition0x545cf0.
    const auto labelWidth = [&](const std::string& label) {
        const auto bounds = renderer.bounds(
            renderer.sourceFont(62, label, chinese), label, Renderer::sourceFontScale(62, chinese));
        return std::round(bounds.second - bounds.first) + 6.f;
    };
    const float yesWidth = labelWidth(yes);
    const float noWidth = labelWidth(no);
    const float width = std::max(300.f, yesWidth + noWidth + 100);
    auto& font = renderer.sourceFont(52, message, chinese);
    auto lines = renderer.wrappedLines(
        font, message, width - 40, Renderer::sourceFontScale(52, chinese), chinese);
    const float textHeight =
        std::round(static_cast<float>(lines.size()) * Renderer::sourceLineHeight(52, font));
    const float x = std::floor((1280 - width) / 2);
    const float y = std::floor((720 - (90 + textHeight - 22)) / 2);
    const float buttonX = x + std::floor(width / 2) - std::floor((yesWidth + noWidth + 14) / 2);
    return {{x, y, width, textHeight + 68},
            {buttonX, y + textHeight + 28, yesWidth, 24},
            {buttonX + yesWidth + 14, y + textHeight + 28, noWidth, 24},
            std::move(lines),
            textHeight};
}

} // namespace

void Run::openStore() {
    if (!docked() || !graph_.beacons().at(graph_.current()).store) {
        return;
    }
    createStore();
    panel_ = "store";
    panelPage_ = 0;
    storeSelling_ = false;
    pendingStoreOffer_ = -1;
    cancelAim();
}

void Run::createStore() {
    if (!stores_.contains(graph_.current())) {
        std::vector<StoreOffer> offers;
        // Store::OnInit 0x4b7e30: distinct 2-4 categories; initial system bias.
        const int count = static_cast<int>(eventRandom_.next() % 3) + 2;
        std::vector<int> categories;
        const int installed = static_cast<int>(systems_.size());
        const int choices = !advanced_ && installed == 11 ? 4 : 5;
        categories.push_back(installed <= 10 && eventRandom_.next() % 2 == 1
                                 ? 4
                                 : static_cast<int>(eventRandom_.next() % choices));
        while (static_cast<int>(categories.size()) < count) {
            const int category = static_cast<int>(eventRandom_.next() % choices);
            if (std::ranges::find(categories, category) == categories.end()) {
                categories.push_back(category);
            }
        }
        if (!system(4) && std::ranges::find(categories, 4) == categories.end()) {
            for (auto& category : categories) {
                if (category == 1) {
                    for (int replacement = 0; replacement < choices; ++replacement) {
                        if (std::ranges::find(categories, replacement) == categories.end()) {
                            category = replacement;
                            break;
                        }
                    }
                }
            }
        }
        for (int category : categories) {
            const auto categoryStart = offers.size();
            // Store::CreateStoreBoxes 0x4b3ce0 draws rand % v5 + v6 for every
            // item and crew section (v5 = 1, v6 = 3 with the DLC, so the
            // advanced draw is spent too); the system section draws none.
            const int desired = category <= 3
                                    ? static_cast<int>(eventRandom_.next() % (advanced_ ? 1 : 2)) +
                                          (advanced_ ? 3 : 2)
                                    : 0;
            if (category <= 2) {
                const std::string kind = category == 0 ? "weapon" : category == 1 ? "drone" : "aug";
                auto pool = catalog_.rewardEquipmentPool(kind, advanced_, sectorRarities());

                struct Node {
                    std::string id;
                    int weight = 0;
                    int total = 0;
                };

                std::vector<Node> heap(1);
                for (const auto& [id, weight] : pool) {
                    if (weight >= 1 && weight <= 5) {
                        heap.push_back({id, weight, weight});
                    }
                }
                for (std::size_t i = heap.size() - 1; i > 1; --i) {
                    heap[i / 2].total += heap[i].total;
                }
                // RandomItems 0x72ec30: weighted sorted-map heap, without replacement.
                for (int item = 0; item < desired && heap.size() > 1 && heap[1].total > 0; ++item) {
                    int choice = static_cast<int>(eventRandom_.next() % heap[1].total) + 1;
                    std::size_t index = 1;
                    while (choice > heap[index].weight) {
                        choice -= heap[index].weight;
                        index *= 2;
                        if (choice > heap[index].total) {
                            choice -= heap[index].total;
                            ++index;
                        }
                    }
                    const int weight = heap[index].weight;
                    heap[index].weight = 0;
                    offers.push_back({heap[index].id, catalog_.equipment(heap[index].id).cost, 1});
                    for (; index > 0; index /= 2) {
                        heap[index].total -= weight;
                    }
                }
            } else if (category == 3) {
                std::map<std::string, std::pair<int, int>> crewPool;
                for (const auto& [id, weight] :
                     catalog_.crewRewardPool(advanced_, sectorRarities())) {
                    crewPool[id] = {6 - weight, catalog_.appearance(id).cost};
                }

                struct CrewNode {
                    std::string id;
                    int cost = 0;
                    int weight = 0;
                    int total = 0;
                };

                std::vector<CrewNode> heap(1);
                for (const auto& [id, data] : crewPool) {
                    if (data.first > 0 && data.first <= 5) {
                        heap.push_back({id, data.second, 6 - data.first, 6 - data.first});
                    }
                }
                for (std::size_t i = heap.size() - 1; i > 1; --i) {
                    heap[i / 2].total += heap[i].total;
                }
                // Each CrewStoreBox independently requests one random race, so duplicates are
                // allowed.
                for (int item = 0; item < desired && heap.size() > 1 && heap[1].total > 0; ++item) {
                    int choice = static_cast<int>(eventRandom_.next() % heap[1].total) + 1;
                    std::size_t index = 1;
                    while (choice > heap[index].weight) {
                        choice -= heap[index].weight;
                        index *= 2;
                        if (choice > heap[index].total) {
                            choice -= heap[index].total;
                            ++index;
                        }
                    }
                    StoreOffer offer{"crew:" + heap[index].id, heap[index].cost, 1};
                    const auto previousEnemy = enemy_;
                    stageCombatCrewReward(heap[index].id);
                    std::vector<std::string> usedNames;
                    usedNames.reserve(crew_.size());
                    for (const auto& member : crew_) {
                        usedNames.push_back(member.appearance.fullName);
                    }
                    // CrewStoreBox overwrites GetCrewBlueprint's initial name.
                    const auto storeName = catalog_.unusedCrewName(locale_, usedNames, [&] {
                        return eventRandom_.next();
                    });
                    enemy_.rewardCrewFullName = storeName.first;
                    enemy_.rewardCrewMale = storeName.second;
                    enemy_.rewardCrewName =
                        catalog_.shortCrewName(storeName.first, [&](const std::string& value) {
                            const auto [left, right] =
                                renderer_.bounds(localizedFont(6, value), value, textScale(6));
                            return right - left;
                        });
                    enemy_.rewardCrewAppearance =
                        heap[index].id == "human" && !storeName.second ? 1 : 0;
                    nlohmann::json metadata{{"name", enemy_.rewardCrewName},
                                            {"full_name", enemy_.rewardCrewFullName},
                                            {"appearance", enemy_.rewardCrewAppearance},
                                            {"male", enemy_.rewardCrewMale}};
                    enemy_ = previousEnemy;
                    // CrewBlueprint::ChooseColors 0x5def50: independently draw each unfixed layer.
                    int appearance = metadata.at("appearance").get<int>();
                    int stride = heap[index].id == "human" ? 2 : 1;
                    for (const auto& layer : catalog_.appearance(heap[index].id).layers) {
                        if (!layer.empty()) {
                            appearance +=
                                static_cast<int>(eventRandom_.next() % layer.size()) * stride;
                            stride *= static_cast<int>(layer.size());
                        }
                    }
                    metadata["appearance"] = appearance;
                    constexpr std::array<std::array<int, 2>, 8> ranges{
                        {{0, 0}, {0, 0}, {0, 1}, {0, 2}, {1, 2}, {1, 2}, {1, 3}, {2, 3}}};
                    const auto range = ranges[std::size_t(std::clamp(sector_ - 1, 0, 7))];
                    const int steps =
                        std::min(4,
                                 range[0] + static_cast<int>(eventRandom_.next() %
                                                             (range[1] - range[0] + 1)));
                    const int first = static_cast<int>(eventRandom_.next() % 6);
                    int second = first;
                    while (second == first) {
                        second = static_cast<int>(eventRandom_.next() % 6);
                    }
                    std::array<int, 6> skills{};
                    for (int step = 0; step < steps; ++step) {
                        auto& tier = skills[eventRandom_.next() % 2 == 0 ? first : second];
                        tier = std::min(2, tier + 1);
                    }
                    metadata["skills"] = skills;
                    offer.bonus = metadata.dump();
                    offers.push_back(std::move(offer));
                }
            } else {
                std::map<std::string, int> systemRarity;
                for (const auto* path : {"data/blueprints.xml",
                                         "data/dlcBlueprints.xml",
                                         "data/dlcBlueprintsOverwrite.xml"}) {
                    if (!advanced_ &&
                        std::string_view(path).find("dlc") != std::string_view::npos) {
                        continue;
                    }
                    auto doc = xmlResource(archive_, path);
                    auto root = doc.child("FTL");
                    if (!root) {
                        root = doc;
                    }
                    for (auto node : root.children("systemBlueprint")) {
                        systemRarity[node.attribute("name").as_string()] =
                            node.child("rarity").text().as_int();
                    }
                }
                // CreateStoreBoxes 0x4b3ce0, system section: candidates in id
                // order, leaving out the systems it will force (drones when the
                // store sells drones, shields, medbay/clone bay).
                const bool forceDrones =
                    !system(4) && std::ranges::find(categories, 1) != categories.end();
                const bool forceMedical = !system(5) && (!advanced_ || !system(13));
                std::vector<int> possible;
                for (const auto& slot : ship_.systems) {
                    const int id = slot.id;
                    if (system(id) || id == 11 || (!advanced_ && id > 10) ||
                        (forceDrones && id == 4) || (forceMedical && (id == 5 || id == 13)) ||
                        (!system(0) && id == 0) || catalog_.system(slot.name).cost <= 0 ||
                        systemRarity[slot.name] <= 0) {
                        continue;
                    }
                    possible.push_back(id);
                }
                std::ranges::sort(possible);
                int remaining = std::min(3, static_cast<int>(possible.size()));
                const auto offerSystem = [&](int id) {
                    const auto slot = std::ranges::find_if(ship_.systems, [id](const auto& sys) {
                        return sys.id == id;
                    });
                    if (slot == ship_.systems.end()) {
                        return;
                    }
                    StoreOffer offer{
                        "system:" + std::to_string(id), catalog_.system(slot->name).cost, 1};
                    if (id == 4) {
                        // SystemStoreBox 0x50a0c0 draws its free drone at construction.
                        offer.bonus = std::array<std::string, 3>{
                            "REPAIR", "COMBAT_1", "DEFENSE_1"}[eventRandom_.next() % 3];
                        offer.cost += catalog_.equipment(offer.bonus).cost / 2;
                    }
                    offers.push_back(std::move(offer));
                };
                if (forceDrones) {
                    --remaining;
                    offerSystem(4);
                }
                if (!system(0)) {
                    --remaining;
                    offerSystem(0);
                }
                if (forceMedical) {
                    --remaining;
                    offerSystem(advanced_ && (eventRandom_.next() & 1) == 0 ? 13 : 5);
                }
                // Selection sampling: keep candidate v with probability
                // remaining / (candidates - v), preserving id order.
                const int candidates = static_cast<int>(possible.size());
                for (int index = 0; index < candidates && remaining > 0; ++index) {
                    if (static_cast<int>(eventRandom_.next() %
                                         static_cast<unsigned>(candidates - index)) +
                            1 <=
                        remaining) {
                        offerSystem(possible[static_cast<std::size_t>(index)]);
                        --remaining;
                    }
                }
            }
            // Preserve three positions per category, including unavailable stock.
            while (offers.size() < categoryStart + 3) {
                offers.push_back({"unavailable", 0, 0});
            }
            for (std::size_t index = categoryStart; index < offers.size(); ++index) {
                offers[index].category = category;
            }
        }
        // ItemStoreBox 0x509570, SetPositions 0x4b6a20 (worldLevel is zero based).
        offers.insert(
            offers.begin(),
            {{"fuel", catalog_.item("fuel").cost, static_cast<int>(eventRandom_.next() % 5) + 3},
             {"missiles",
              catalog_.item("missiles").cost,
              static_cast<int>(eventRandom_.next() % 5) + 2},
             {"parts", catalog_.item("drones").cost, static_cast<int>(eventRandom_.next() % 3) + 2},
             {"repair",
              sector_ <= 3   ? 2
              : sector_ <= 6 ? 3
                             : 4,
              1}});
        stores_.emplace(graph_.current(), std::move(offers));
    }
}

std::vector<Run::VoyageButton> Run::storeButtons(bool renderStock) const {
    std::vector<VoyageButton> result;
    if (pendingStoreOffer_ >= 0 && !renderStock) {
        return {{"store:confirm", "Confirm system purchase", true},
                {"store:cancel", "Cancel", true}};
    }
    const auto found = stores_.find(graph_.current());
    if (found == stores_.end()) {
        return result;
    }
    for (std::size_t i = 0; i < found->second.size(); ++i) {
        const auto& offer = found->second[i];
        bool capacity = true;
        if (offer.item.starts_with("crew:")) {
            capacity = crewRoster().size() < std::size_t(devCrewLimit_);
        } else if (offer.item.starts_with("system:")) {
            const int id = storeIndex(std::string_view(offer.item).substr(7));
            const int replacing = id == 5 ? 13 : id == 13 ? 5 : -1;
            const bool passive = (id >= 6 && id <= 8) || id == 12;
            const auto count = std::ranges::count_if(systems_, [replacing](const auto& sys) {
                return sys.definition.id != replacing &&
                       !((sys.definition.id >= 6 && sys.definition.id <= 8) ||
                         sys.definition.id == 12);
            });
            capacity = !system(id) && (passive || count < 8);
        } else if (i >= 4 && offer.item != "unavailable") {
            capacity = inventoryCanHold(offer.item);
        } else if (offer.item == "repair") {
            capacity = hull_ < ship_.hull;
        }
        const auto label = offer.item.starts_with("crew:") && !offer.bonus.empty()
                               ? nlohmann::json::parse(offer.bonus).at("name").get<std::string>() +
                                     " (" + offer.item.substr(5) + ")"
                               : offer.item;
        result.push_back({"buy:" + std::to_string(i),
                          label + " / " + std::to_string(offer.cost) + " scrap / stock " +
                              std::to_string(offer.stock),
                          docked() && offer.stock > 0 && capacity});
    }
    const int repair = found->second.size() > 3 ? found->second[3].cost : 2;
    result.push_back({"store:repair_all",
                      "Repair all / " + std::to_string((ship_.hull - hull_) * repair) + " scrap",
                      hull_ < ship_.hull});
    for (const auto& kind : {"weapon", "drone", "augment", "cargo"}) {
        const auto& items = std::string_view(kind) == "weapon"    ? ship_.weapons
                            : std::string_view(kind) == "drone"   ? ship_.drones
                            : std::string_view(kind) == "augment" ? ship_.augments
                                                                  : cargo_;
        for (std::size_t i = 0; i < items.size(); ++i) {
            result.push_back({"sell:" + std::string(kind) + ":" + std::to_string(i),
                              "Sell " + items[i] + " / " +
                                  std::to_string(catalog_.equipment(items[i]).cost / 2) + " scrap",
                              true});
        }
    }
    return result;
}

bool Run::storeAction(const std::string& target, const std::string& action) {
    const bool recognized =
        target.starts_with("store:") || target.starts_with("swap:") ||
        target.starts_with("discard:") ||
        (panel_ == "store" && (target.starts_with("buy:") || target.starts_with("install:") ||
                               target.starts_with("sell:")));
    if (!recognized) {
        return false;
    }
    if (action != "click" || !docked()) {
        return true;
    }
    const auto resources = resourceAmounts();
    if (pendingStoreOffer_ >= 0 && target != "store:confirm" && target != "store:cancel") {
        return true;
    }
    if (target == "store:discard_overflow") {
        retireCrewDrone(overflowFactory_.drone);
        overflowEquipment_.clear();
        overflowFactory_ = {};
        return true;
    }
    if (target == "store:discard_overflow_augment") {
        overflowAugment_.clear();
        return true;
    }
    if (target.starts_with("swap:")) {
        const auto first = target.find(':', 5);
        const auto second = target.find(':', first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            inventorySwap(storeIndex(std::string_view(target).substr(5, first - 5)),
                          target.substr(first + 1, second - first - 1),
                          storeIndex(std::string_view(target).substr(second + 1)));
        }
        return true;
    }
    if (target.starts_with("discard:") || target.starts_with("sell:")) {
        const bool selling = target.starts_with("sell:");
        if (selling && panel_ != "store") {
            return true;
        }
        const auto start = selling ? 5U : 8U;
        const auto split = target.find(':', start);
        const std::string kind =
            split == std::string::npos ? "cargo" : target.substr(start, split - start);
        const int index = storeIndex(
            std::string_view(target).substr(split == std::string::npos ? start : split + 1));
        auto* items = kind == "weapon"    ? &ship_.weapons
                      : kind == "drone"   ? &ship_.drones
                      : kind == "augment" ? &ship_.augments
                      : kind == "cargo"   ? &cargo_
                                          : nullptr;
        if (!items || index < 0 || index >= static_cast<int>(items->size())) {
            return true;
        }
        if (kind == "weapon") {
            detachInventoryWeapon(index);
            weaponPower_.erase(weaponPower_.begin() + index);
            weaponDesired_.erase(weaponDesired_.begin() + index);
            combatWeapons_.erase(combatWeapons_.begin() + index);
            cancelAim();
        } else if (kind == "drone") {
            detachInventoryDrone(index);
            reindexDroneReferences(0, index, true);
            retireCrewDrone(combatDrones_[index]);
            combatDrones_.erase(combatDrones_.begin() + index);
        }
        if (selling) {
            scrap_ += catalog_.equipment((*items)[index]).cost / 2;
            audio_.sound("buy");
        }
        eraseInventorySlot(kind, index);
        if (kind == "cargo") {
            retireCrewDrone(cargoFactories_.at(index).drone);
            cargoFactories_.erase(cargoFactories_.begin() + index);
        }
        items->erase(items->begin() + index);
        inventorySelection_ = -1;
        commitCombatEquipmentReward();
        noteResourceChanges(resources);
        return true;
    }
    if (panel_ != "store" || !stores_.contains(graph_.current())) {
        return true;
    }
    if (target == "store:sell" || target == "store:buy") {
        storeSelling_ = target == "store:sell";
        return true;
    }
    if (target == "store:cancel") {
        pendingStoreOffer_ = -1;
        return true;
    }
    auto& offers = stores_.at(graph_.current());
    if (target == "store:repair_all") {
        if (offers.size() > 3 && hull_ < ship_.hull) {
            const int cost = (ship_.hull - hull_) * offers[3].cost;
            if (scrap_ >= cost) {
                scrap_ -= cost;
                hull_ = ship_.hull;
                progressionRun_.tutorial = tutorialOwned_;
                progressionRun_.flag("store_repair");
                audio_.sound("repairShip");
            } else {
                failScrapPurchase();
            }
        }
        noteResourceChanges(resources);
        return true;
    }
    int index = storeIndex(std::string_view(target).substr(target.find(':') + 1));
    const bool confirming = target == "store:confirm";
    if (confirming) {
        index = pendingStoreOffer_;
        pendingStoreOffer_ = -1;
    }
    if (target.starts_with("install:")) {
        const auto item = "system:" + std::to_string(index);
        const auto found = std::ranges::find_if(offers, [&](const auto& offer) {
            return offer.item == item;
        });
        index = found == offers.end() ? -1 : static_cast<int>(found - offers.begin());
    }
    if (index < 0 || index >= static_cast<int>(offers.size()) ||
        (pendingStoreOffer_ >= 0 && !confirming)) {
        return true;
    }
    auto& offer = offers[index];
    if (offer.stock <= 0) {
        return true;
    }
    const auto purchaseButtons = storeButtons();
    if (static_cast<std::size_t>(index) >= purchaseButtons.size() ||
        !purchaseButtons[index].enabled) {
        return true;
    }
    if (scrap_ < offer.cost) {
        failScrapPurchase();
        return true;
    }
    if (offer.item == "fuel") {
        ++fuel_;
        distress_ = false;
    } else if (offer.item == "missiles") {
        ++missiles_;
    } else if (offer.item == "parts") {
        ++parts_;
    } else if (offer.item == "repair") {
        if (hull_ >= ship_.hull) {
            return true;
        }
        ++hull_;
    } else if (offer.item.starts_with("crew:")) {
        const auto species = offer.item.substr(5);
        if (offer.bonus.empty()) {
            if (!hireCrew(species)) {
                return true;
            }
        } else {
            if (crewRoster().size() >= std::size_t(devCrewLimit_) || enemy_.rewardCrewPending) {
                return true;
            }
            const auto metadata = nlohmann::json::parse(offer.bonus);
            const auto previousEnemy = enemy_;
            enemy_.rewardCrewSpecies = species;
            enemy_.rewardCrewName = metadata.at("name").get<std::string>();
            enemy_.rewardCrewFullName = metadata.at("full_name").get<std::string>();
            enemy_.rewardCrewAppearance = metadata.at("appearance").get<int>();
            enemy_.rewardCrewColorsPrepared = true;
            enemy_.rewardCrewMale = metadata.at("male").get<bool>();
            enemy_.rewardCrewPending = true;
            const auto before = crew_.size();
            commitCombatCrewReward();
            enemy_ = previousEnemy;
            if (crew_.size() == before) {
                return true;
            }
            auto& member = crew_.back();
            const auto skills = metadata.at("skills").get<std::array<int, 6>>();
            constexpr std::array<int, 6> human{26, 26, 100, 116, 32, 14};
            constexpr std::array<int, 6> generic{30, 30, 110, 130, 36, 16};
            const auto& caps = species == "human" ? human : generic;
            for (int skill = 0; skill < 3; ++skill) {
                member.manningSkill[skill] = skills[skill];
                member.manningSkillProgress[skill] = skills[skill] * caps[skill] / 2;
            }
            member.weaponSkill = skills[3];
            member.weaponSkillProgress = skills[3] * caps[3] / 2;
            member.repairSkill = skills[4];
            member.repairSkillProgress = skills[4] * caps[4] / 2;
            member.combatSkill = skills[5];
            member.combatSkillProgress = skills[5] * caps[5] / 2;
        }
    } else if (offer.item.starts_with("system:")) {
        const int id = storeIndex(std::string_view(offer.item).substr(7));
        const auto slot = std::ranges::find_if(ship_.systems, [id](const auto& sys) {
            return sys.id == id;
        });
        if (slot == ship_.systems.end() || system(id)) {
            return true;
        }
        const int replacing = id == 5 ? 13 : id == 13 ? 5 : -1;
        const bool passive = (id >= 6 && id <= 8) || id == 12;
        const int installed =
            static_cast<int>(std::ranges::count_if(systems_, [replacing](const auto& sys) {
                return sys.definition.id != replacing &&
                       !((sys.definition.id >= 6 && sys.definition.id <= 8) ||
                         sys.definition.id == 12);
            }));
        if (!passive && installed >= 8) {
            return true;
        }
        if (!confirming && ((replacing >= 0 && system(replacing)) ||
                            (!passive && installed == 7 && (advanced_ || system(11))))) {
            pendingStoreOffer_ = index;
            return true;
        }
        if (!installSystem(id)) {
            return true;
        }
        if (!offer.bonus.empty()) {
            (void)inventoryAdd(offer.bonus, true);
        }
    } else if (!inventoryAdd(offer.item)) {
        return true;
    }
    scrap_ -= offer.cost;
    progressionRun_.tutorial = tutorialOwned_;
    progressionRun_.flag(offer.item == "repair" ? "store_repair" : "store_purchase");
    audio_.sound(offer.item == "repair" ? "repairShip" : "buy");
    if (offer.item != "repair") {
        --offer.stock;
    }
    noteResourceChanges(resources);
    return true;
}

bool Run::installSystem(int id) {
    const auto slot = std::ranges::find_if(ship_.systems, [id](const auto& definition) {
        return definition.id == id;
    });
    if (slot == ship_.systems.end() || system(id)) {
        return false;
    }
    const int replacing = id == 5 ? 13 : id == 13 ? 5 : -1;
    const bool passive = (id >= 6 && id <= 8) || id == 12;
    // ShipManager::AddSystem 0x49ca00 replaces the medical pointer in
    // place and upgrades the fresh replacement to the old maximum level.
    const auto previous = std::ranges::find_if(systems_, [replacing](const auto& sys) {
        return sys.definition.id == replacing;
    });
    auto installedSystem = playerSystemDefinition(*slot);
    installedSystem.installed = true;
    installedSystem.power = std::max(1, installedSystem.power);
    if (previous != systems_.end()) {
        installedSystem.power = std::max(installedSystem.power, previous->definition.power);
    }
    slot->slot = installedSystem.slot;
    slot->direction = installedSystem.direction;
    slot->installed = true;
    slot->power = installedSystem.power;
    if (previous != systems_.end()) {
        *previous = {installedSystem, passive ? installedSystem.power : 0, installedSystem.power};
        for (auto& definition : ship_.systems) {
            if (definition.id == replacing) {
                definition.installed = false;
            }
        }
    } else {
        systems_.push_back(
            {installedSystem, passive ? installedSystem.power : 0, installedSystem.power});
    }
    if (id == 5) {
        system(id)->damagingEffectTimes =
            initializeMedbayAnimations(room(installedSystem.room), eventRandom_);
    }
    if (id == 11 && !installedSystem.weapon.empty()) {
        initializeWeaponFactory(
            system(id)->artillery, catalog_.equipment(installedSystem.weapon), eventRandom_);
    } else if (id == 15) {
        hackingDrone_ = CombatDrone{.blueprint = "DRONE_HACKING"};
        initializeCrewDroneAnimation(hackingDrone_, eventRandom_);
    }
    if (id == 5 || id == 13) {
        // AddSystem reassigns room occupants after rebuilding medical slots.
        for (auto& member : crew_) {
            if (member.health > 0 && !member.intruder && member.room == installedSystem.room) {
                order(member, installedSystem.room, -1, true);
            }
        }
    }
    if (id == 5) {
        cloneQueue_.clear();
        cloneTimer_ = 0;
    }
    if (id == 0) {
        // AddSystem49ca00 fills the newly installed shield power and charge.
        while (power(0, true)) {
        }
        shieldBubbles_ = system(0)->power / 2;
        shieldCharge_ = 0;
    }
    updateManning();
    return true;
}

void Run::storeRender() {
    const float offset = std::max(283.f, managementPanelX()) - 340;
    const auto descriptionInfo =
        [&](const std::string& title, const std::string& body, const std::string& freeDrone) {
            const float infoX = 940 + offset;
            constexpr float infoY = 152;
            renderer_.windowFrame({infoX, infoY, 337, 135});
            float titleEndY = infoY + 13;
            for (const auto& line : wrappedText(16, title, 305)) {
                text(line, infoX + 18, titleEndY, 16);
                titleEndY += textLineHeight(16, title);
            }
            const auto lines =
                [&](const std::string& value, float width, float x, float y, SDL_Color color) {
                    for (const auto& line : wrappedText(10, value, width)) {
                        text(line, x, y, 10, false, color);
                        y += textLineHeight(10, value);
                    }
                    return y;
                };
            const float endY = lines(body, 305, infoX + 18, titleEndY + 15, {235, 245, 229, 255});
            if (!freeDrone.empty()) {
                const auto& bp = catalog_.equipment(freeDrone);
                const auto name =
                    bp.title.reference ? texts_.get(bp.title.value, locale_) : bp.title.value;
                auto label = texts_.get("free_drone", locale_);
                const auto marker = label.find("\\1");
                if (marker != std::string::npos) {
                    label.replace(marker, 2, name);
                }
                lines(label, 233, infoX + 90, endY + 8, {120, 255, 120, 255});
                renderDroneIcon(bp, infoX + 50, endY + 25);
            }
        };
    renderer_.setTint(pendingStoreOffer_ >= 0 ? .5f : 1.f);
    renderer_.draw(storeSelling_ ? "img/storeUI/store_sell_main.png"
                                 : "img/storeUI/store_buy_main.png",
                   340 + offset,
                   85);
    const auto buyTab = texts_.get("store_tab_buy", locale_);
    const auto sellTab = texts_.get("store_tab_sell", locale_);
    const bool smallTab =
        renderer_.measure(localizedFont(63, buyTab), buyTab, textScale(63)) > 110 ||
        renderer_.measure(localizedFont(63, sellTab), sellTab, textScale(63)) > 113;
    text(buyTab, 426 + offset, smallTab ? 98.f : 93.f, smallTab ? 62 : 63, true, {25, 49, 51, 255});
    text(
        sellTab, 588 + offset, smallTab ? 98.f : 93.f, smallTab ? 62 : 63, true, {25, 49, 51, 255});
    // CommandGui constructor0x523c70: Store and TabbedWindow share(340,85).
    const auto closeLabel = texts_.get("button_close", locale_);
    const float closeWidth = std::max(
        103.f,
        std::round(
            renderer_.bounds(localizedFont(63, closeLabel), closeLabel, textScale(63)).second -
            renderer_.bounds(localizedFont(63, closeLabel), closeLabel, textScale(63)).first) +
            6);
    const Rect closeFace{911 + offset - closeWidth, 555, closeWidth, 32};
    auto& closeBase = renderer_.image("img/storeUI/store_close_base.png");
    const float baseWidth = static_cast<float>(closeBase.w);
    const float baseHeight = static_cast<float>(closeBase.h);
    const float half = baseWidth / 2;
    renderer_.sprite(
        closeBase, {closeFace.x - 23, 548, half, baseHeight}, {0, 0, half, baseHeight});
    renderer_.sprite(closeBase,
                     {closeFace.x - 23 + half, 548, closeWidth - 103, baseHeight},
                     {half, 0, 1, baseHeight});
    renderer_.sprite(
        closeBase,
        {closeFace.x - 23 + half + closeWidth - 103, 548, baseWidth - half, baseHeight},
        {half, 0, baseWidth - half, baseHeight});
    const SDL_Color closeInk{25, 49, 51, 255};
    const SDL_Color closeColor = closeFace.contains(storeMouseX_, storeMouseY_)
                                     ? SDL_Color{120, 255, 120, 255}
                                     : SDL_Color{235, 245, 229, 255};
    renderer_.fill({closeFace.x + 4, closeFace.y, closeFace.w - 8, closeFace.h}, closeColor);
    for (int edge = 0; edge < 4; ++edge) {
        renderer_.fill({closeFace.x + static_cast<float>(edge),
                        closeFace.y + 4 - edge,
                        1,
                        closeFace.h - 8 + 2 * edge},
                       closeColor);
        renderer_.fill({closeFace.x + closeFace.w - 1 - edge,
                        closeFace.y + 4 - edge,
                        1,
                        closeFace.h - 8 + 2 * edge},
                       closeColor);
    }
    text(closeLabel, closeFace.x + closeFace.w / 2, 556, 63, true, closeInk);
    renderer_.draw("img/upgradeUI/Equipment/tabButtons/" +
                       std::string(storeSelling_ ? "sell_buy" : "buy_sell") +
                       (Rect{storeSelling_ ? 345.f + offset : 508.f + offset,
                             92,
                             storeSelling_ ? 150.f : 159.f,
                             36}
                                .contains(storeMouseX_, storeMouseY_)
                            ? "_select2.png"
                            : "_on.png"),
                   (storeSelling_ ? 340.f : 503.f) + offset,
                   85);
    // The tab artwork overlays the inactive triangular face, so redraw captions.
    text(buyTab, 426 + offset, smallTab ? 98.f : 93.f, smallTab ? 62 : 63, true, closeInk);
    text(sellTab, 588 + offset, smallTab ? 98.f : 93.f, smallTab ? 62 : 63, true, closeInk);
    if (storeSelling_) {
        // Equipment::OnRender 0x525dd0 draws the sell DropBox before the
        // equipment boxes and the held item, so the dragged icon stays on top.
        const auto body = texts_.get("sell_box_text", locale_);
        const auto bodyLines = wrappedText(52, body, 235);
        const float bodyHeight =
            std::round(textLineHeight(52, body) * static_cast<float>(bodyLines.size()));
        const float insert = bodyHeight - 13;
        // Equipment::MouseMove 0x525580: selectedImage is set only while an
        // item is dragged inside the box (bSellingItem).
        const bool sellingItem =
            inventoryDragIndex_ >= 0 &&
            Rect{59 + offset, 185, 260, 199}.contains(storeMouseX_, storeMouseY_);
        auto& art = renderer_.image(sellingItem ? "img/dropbox_sell_select2.png"
                                                : "img/dropbox_sell_on.png");
        renderer_.sprite(art, {65 + offset, 185, 272, 32}, {0, 0, 272, 32});
        renderer_.sprite(art, {65 + offset, 217, 272, 1}, {0, 32, 272, 1});
        renderer_.sprite(art, {65 + offset, 218, 272, 31}, {0, 33, 272, 31});
        renderer_.sprite(art, {65 + offset, 249, 272, insert + 1}, {0, 64, 272, 1});
        const float footerHeight = static_cast<float>(art.h) - 65;
        renderer_.sprite(
            art, {65 + offset, 250 + insert, 272, footerHeight}, {0, 65, 272, footerHeight});
        text(texts_.get("sell_box_title", locale_), 201 + offset, 195, 63, true, {25, 49, 51, 255});
        // DropBox::OnRender 0x4d8e70: body and sell text use COLOR_BUTTON_SELECT
        // while selected, otherwise COLOR_BUTTON_ON.
        const SDL_Color ink =
            sellingItem ? SDL_Color{255, 230, 94, 255} : SDL_Color{235, 245, 229, 255};
        float bodyY = 242;
        for (const auto& line : bodyLines) {
            text(line, 201 + offset, bodyY, 52, true, ink);
            bodyY += textLineHeight(52, body);
        }
        // DropBox::OnInit 0x4da1f0 sets sellText "sell_value"; Equipment::
        // MouseMove fills sellCostText with GetItemValue / 2 for the dragged
        // or hovered item, printed in font 0 beside the scrap icon.
        text(
            texts_.get("sell_value", locale_), 65 + offset + 102, 185 + insert + 85, 52, true, ink);
        const auto item =
            inventoryDragIndex_ >= 0 ? inventoryDraggedItem() : inventoryHoveredItem();
        if (!item.empty()) {
            text(std::to_string(catalog_.equipment(item).cost / 2),
                 65 + offset + 214,
                 185 + insert + 90,
                 0,
                 false,
                 ink);
        }
        inventoryRender();
        return;
    }
    const auto found = stores_.find(graph_.current());
    if (found == stores_.end()) {
        return;
    }
    const auto buttons = storeButtons(true);
    // FONT_HL1_2X_SHORT59: same22px bitmap as52,18px newline cadence.
    const auto repairText =
        [&](const std::string& label, float x, float y, SDL_Color color = {235, 245, 229, 255}) {
            std::istringstream paragraphs(label);
            std::string line;
            while (std::getline(paragraphs, line)) {
                text(line, x, y, 59, true, color);
                y += textLineHeight(59, label);
            }
        };

    for (int i = 0; i < 4 && i < static_cast<int>(found->second.size()); ++i) {
        const auto& offer = found->second[i];
        const float y = i == 3 ? 376.f : 168.f + static_cast<float>(i == 2 ? 99 : i * 50);
        const auto path = i == 0   ? "store_items_fuel"
                          : i == 1 ? "store_items_missiles"
                          : i == 2 ? "store_items_drones"
                                   : "store_repair";
        // ItemStoreBox 0x509570 / RepairStoreBox 0x5099f0 are StoreBox buttons:
        // Button::OnRender draws the "_select2" face while active and hovered
        // (GenericButton::MouseMove via StoreBox::MouseMove 0x50e330), and the
        // button hitbox is the face image size.
        const auto& face = renderer_.image("img/storeUI/" + std::string(path) + "_on.png");
        const bool hovered =
            Rect{358 + offset, y, static_cast<float>(face.w), static_cast<float>(face.h)}.contains(
                storeMouseX_, storeMouseY_);
        const bool activeHover = buttons[i].enabled && hovered;
        renderer_.draw(
            "img/storeUI/" + std::string(path) +
                (buttons[i].enabled ? (activeHover ? "_select2.png" : "_on.png") : "_off.png"),
            358 + offset,
            y);
        if (i < 3) {
            // StoreBox::OnRender 0x50c450: count/cost ink is constant 0x830430
            // (245,238,163) while active+hovered, otherwise COLOR_BUTTON_ON.
            const SDL_Color ink =
                activeHover ? SDL_Color{245, 238, 163, 255} : SDL_Color{235, 245, 229, 255};
            // StoreBox::OnRender 0x50c450 returns after the face when count <= 0.
            if (offer.stock > 0) {
                text(std::to_string(offer.stock), 428 + offset, y + 16, 0, false, ink);
                text(std::to_string(offer.cost), 488 + offset, y + 16, 0, false, ink);
            }
            // Store::MouseMove 0x4b28c0 fills the InfoBox for any hovered box
            // with count > 0, independent of the button's active state.
            if (offer.stock > 0 && hovered) {
                const auto& item = catalog_.item(i == 0 ? "fuel" : i == 1 ? "missiles" : "drones");
                const auto localized = [&](const LocalizedText& value) {
                    return value.reference ? texts_.get(value.value, locale_) : value.value;
                };
                descriptionInfo(localized(item.title), localized(item.description), "");
            }
        } else {
            // RepairStoreBox::OnRender 0x50de00: caption ink COLOR_BUTTON_SELECT_2
            // (255,230,94) when active+hovered, COLOR_BUTTON_ON when active,
            // COLOR_BUTTON_OFF (164,171,160) when inactive (also for repair_max).
            const SDL_Color caption = !buttons[i].enabled ? SDL_Color{164, 171, 160, 255}
                                      : activeHover       ? SDL_Color{255, 230, 94, 255}
                                                          : SDL_Color{235, 245, 229, 255};
            repairText(texts_.get("repair_one_button", locale_), 410 + offset, y + 12, caption);
            if (hull_ >= ship_.hull) {
                repairText(texts_.get("repair_max", locale_), 491 + offset, y + 12, caption);
            } else {
                text(std::to_string(offer.cost),
                     491 + offset,
                     y + 16,
                     0,
                     true,
                     activeHover ? SDL_Color{245, 238, 163, 255} : SDL_Color{235, 245, 229, 255});
            }
        }
    }
    constexpr SDL_Color headingInk{25, 49, 51, 255};
    text(texts_.get("store_title_items", locale_), 358 + offset, 130, 62, false, headingInk);
    text(texts_.get("store_title_repair", locale_), 358 + offset, 339, 62, false, headingInk);
    const bool repairAllActive = hull_ < ship_.hull;
    const auto& repairAllFace = renderer_.image("img/storeUI/store_repair_on.png");
    const bool repairAllHover = repairAllActive && Rect{358 + offset,
                                                        431,
                                                        static_cast<float>(repairAllFace.w),
                                                        static_cast<float>(repairAllFace.h)}
                                                       .contains(storeMouseX_, storeMouseY_);
    renderer_.draw(!repairAllActive ? "img/storeUI/store_repair_off.png"
                   : repairAllHover ? "img/storeUI/store_repair_select2.png"
                                    : "img/storeUI/store_repair_on.png",
                   358 + offset,
                   431);
    const SDL_Color repairAllCaption = !repairAllActive ? SDL_Color{164, 171, 160, 255}
                                       : repairAllHover ? SDL_Color{255, 230, 94, 255}
                                                        : SDL_Color{235, 245, 229, 255};
    repairText(texts_.get("repair_all_button", locale_), 410 + offset, 443, repairAllCaption);
    if (hull_ >= ship_.hull) {
        repairText(texts_.get("repair_max", locale_), 491 + offset, 443, repairAllCaption);
    } else {
        text(std::to_string((ship_.hull - hull_) * found->second[3].cost),
             491 + offset,
             447,
             0,
             true,
             repairAllHover ? SDL_Color{245, 238, 163, 255} : SDL_Color{235, 245, 229, 255});
    }
    const auto hullLabel = texts_.get("store_current_hull", locale_);
    repairText(hullLabel, 413 + offset, hullLabel.find('\n') == std::string::npos ? 512.f : 504.f);
    text(std::to_string(hull_), 483 + offset, 517, 0);
    std::string capacityTooltip;
    bool instantCapacityTooltip = false;
    const int groups = static_cast<int>((found->second.size() - 4) / 3);
    for (int row = 0; row < 2; ++row) {
        const int group = panelPage_ * 2 + row;
        if (group >= groups) {
            break;
        }
        const int start = 4 + group * 3;
        const auto& item = found->second[start].item;
        const int category = found->second[start].category;
        const bool systemOffer = category == 4 || item.starts_with("system:");
        const bool crewOffer = category == 3 || item.starts_with("crew:");
        const bool augOffer =
            category == 2 || (!systemOffer && !crewOffer && item != "unavailable" &&
                              catalog_.equipment(item).kind == "aug");
        const bool droneOffer =
            category == 1 || (!systemOffer && !crewOffer && !augOffer && item != "unavailable" &&
                              catalog_.equipment(item).kind == "drone");
        const float y = 168.f + row * 209.f;
        const auto headingKey = systemOffer                                 ? "store_title_systems"
                                : crewOffer                                 ? "store_title_crew"
                                : augOffer                                  ? "store_title_augments"
                                : droneOffer                                ? "store_title_drones"
                                : item == "unavailable"                     ? "store_title_weapons"
                                : catalog_.equipment(item).kind == "weapon" ? "store_title_weapons"
                                                                            : "store_title_drones";
        text(texts_.get(headingKey, locale_), 545 + offset, y - 38, 62, false, headingInk);
        for (int slot = 0; slot < 3; ++slot) {
            const int index = start + slot;
            const auto& offer = found->second[index];
            const float x = 548.f + offset + slot * 125.f;
            const float itemY = (systemOffer || augOffer) ? y + slot * 53.f : y + 20;
            const float itemX = (systemOffer || augOffer) ? 543.f + offset : x;
            const auto art = systemOffer  ? "store_systems"
                             : crewOffer  ? "store_buy_crew"
                             : augOffer   ? "store_weapons"
                             : droneOffer ? "store_buy_drones"
                                          : "store_buy_weapons";
            const bool hovered = Rect{itemX,
                                      itemY,
                                      (systemOffer || augOffer) ? 380.f : 109.f,
                                      (systemOffer || augOffer) ? 50.f : 110.f}
                                     .contains(storeMouseX_, storeMouseY_);
            renderer_.draw(
                "img/storeUI/" + std::string(art) +
                    (buttons[index].enabled ? (hovered ? "_select2.png" : "_on.png") : "_off.png"),
                itemX,
                itemY);
            std::string title;
            if (systemOffer && offer.item.starts_with("system:")) {
                const int id = storeIndex(std::string_view(offer.item).substr(7));
                const auto definition =
                    std::ranges::find_if(ship_.systems, [id](const auto& value) {
                        return value.id == id;
                    });
                if (definition != ship_.systems.end()) {
                    const auto& label = catalog_.system(definition->name).title;
                    title = label.reference ? texts_.get(label.value, locale_) : label.value;
                    if (offer.stock > 0) {
                        renderer_.draw("img/icons/s_" + definition->name + "_green1.png",
                                       itemX - 13,
                                       itemY - 12);
                    }
                }
            } else if (crewOffer && offer.item.starts_with("crew:")) {
                const auto& raceTitle = catalog_.appearance(offer.item.substr(5)).title;
                title =
                    raceTitle.reference ? texts_.get(raceTitle.value, locale_) : raceTitle.value;
                if (!offer.bonus.empty()) {
                    const auto metadata = nlohmann::json::parse(offer.bonus);
                    title = metadata.at("name").get<std::string>();
                    if (offer.stock > 0) {
                        const HangarCrew appearance{title,
                                                    offer.item.substr(5),
                                                    metadata.at("appearance").get<int>(),
                                                    0,
                                                    0,
                                                    metadata.at("full_name").get<std::string>()};
                        // CrewStoreBox::CrewStoreBox 0x508380 sets fIconScale 2 and
                        // pushIcon (0,5) unless the race is rock, crystal or mantis;
                        // StoreBox::OnRender 0x50c450 translates to pushIcon + (60,34)
                        // and CrewBlueprint::RenderIcon 0x5e0f00 centres walk_down
                        // frame 0 there at that scale: (int)(-35 * 2 * 0.5) = -35.
                        const auto& race = appearance.species;
                        const float pushY =
                            race == "rock" || race == "crystal" || race == "mantis" ? 0.f : 5.f;
                        painter_.crewSprite(appearance,
                                            itemX + 60 - 35,
                                            itemY + 34 + pushY - 35,
                                            2,
                                            false,
                                            "walk_down",
                                            0,
                                            false,
                                            false,
                                            false,
                                            false,
                                            false);
                    }
                }
            } else if (offer.item != "unavailable") {
                const auto& bp = catalog_.equipment(offer.item);
                const auto& label = augOffer ? bp.title : bp.shortTitle;
                title = label.reference ? texts_.get(label.value, locale_) : label.value;
                if (offer.stock > 0 && bp.kind == "weapon" && !bp.imagePath.empty()) {
                    SDL_FPoint pivot{static_cast<float>(bp.frameWidth) / 2,
                                     static_cast<float>(bp.frameHeight) / 2};
                    renderer_.sprite(
                        renderer_.image(bp.imagePath),
                        {itemX + 63 - pivot.x,
                         itemY + 35 - pivot.y,
                         static_cast<float>(bp.frameWidth),
                         static_cast<float>(bp.frameHeight)},
                        {static_cast<float>(bp.frameX + bp.chargedFrame * bp.frameWidth),
                         static_cast<float>(bp.frameY),
                         static_cast<float>(bp.frameWidth),
                         static_cast<float>(bp.frameHeight)},
                        90,
                        {255, 255, 255, 255},
                        true,
                        &pivot);
                } else if (offer.stock > 0 && bp.kind == "drone") {
                    renderDroneIcon(bp, itemX + 59, itemY + 32);
                }
            }
            if (hovered && pendingStoreOffer_ < 0) {
                // StoreBox MouseMove0x50e370..0x50e8b0: crew/armament tips
                // remain available over exhausted boxes; augment/system tips need stock.
                if (crewOffer && crewRoster().size() >= std::size_t(devCrewLimit_)) {
                    capacityTooltip = texts_.get("max_crew", locale_);
                } else if (offer.item != "unavailable" && !systemOffer && !crewOffer) {
                    const auto& bp = catalog_.equipment(offer.item);
                    if (augOffer && offer.stock > 0) {
                        if (!bp.stacking &&
                            std::ranges::find(ship_.augments, offer.item) != ship_.augments.end()) {
                            capacityTooltip = texts_.get("have_augment", locale_);
                        } else if (ship_.augments.size() >= 3) {
                            capacityTooltip = texts_.get("max_augments", locale_);
                        }
                        instantCapacityTooltip = !capacityTooltip.empty();
                    } else if (!augOffer && !inventoryCanHold(offer.item)) {
                        capacityTooltip =
                            texts_.get(droneOffer ? "max_drones" : "max_weapons", locale_);
                    }
                } else if (systemOffer && offer.stock > 0 && offer.item.starts_with("system:")) {
                    const int id = storeIndex(std::string_view(offer.item).substr(7));
                    const bool passive = (id >= 6 && id <= 8) || id == 12;
                    const int replacing = id == 5 ? 13 : id == 13 ? 5 : -1;
                    const auto activeSystems =
                        std::ranges::count_if(systems_, [replacing](const auto& sys) {
                            return sys.definition.id != replacing &&
                                   !((sys.definition.id >= 6 && sys.definition.id <= 8) ||
                                     sys.definition.id == 12);
                        });
                    if (!passive && activeSystems >= 8) {
                        capacityTooltip = texts_.get("sys_full", locale_);
                    }
                }
            }
            if (hovered && offer.item != "unavailable" && offer.stock > 0) {
                if (!systemOffer && !crewOffer) {
                    renderEquipmentInfo(catalog_.equipment(offer.item), 940 + offset, 152);
                } else if (systemOffer) {
                    const int id = storeIndex(std::string_view(offer.item).substr(7));
                    const auto systemSlot =
                        std::ranges::find_if(ship_.systems, [id](const auto& sys) {
                            return sys.id == id;
                        });
                    if (systemSlot != ship_.systems.end()) {
                        const auto& blueprint = catalog_.system(systemSlot->name);
                        if (id == 4) {
                            descriptionInfo(
                                title, texts_.get("system_drones", locale_), offer.bonus);
                        } else {
                            RunSystem staged;
                            staged.definition = *systemSlot;
                            staged.definition.power = blueprint.startPower;
                            renderSystemInfo(
                                staged,
                                blueprint.startPower,
                                940 + offset,
                                85,
                                static_cast<int>(682 - std::max(283.f, managementPanelX())));
                        }
                    }
                } else if (crewOffer) {
                    RunCrew staged;
                    staged.appearance.species = offer.item.substr(5);
                    staged.appearance.name = title;
                    std::array<int, 6> skills{};
                    if (!offer.bonus.empty()) {
                        const auto metadata = nlohmann::json::parse(offer.bonus);
                        staged.appearance.name = metadata.at("name").get<std::string>();
                        staged.appearance.fullName = metadata.at("full_name").get<std::string>();
                        staged.appearance.appearance = metadata.at("appearance").get<int>();
                        skills = metadata.at("skills").get<std::array<int, 6>>();
                    }
                    const std::array<int, 6> caps =
                        staged.appearance.species == "human"
                            ? std::array<int, 6>{26, 26, 100, 116, 32, 14}
                            : std::array<int, 6>{30, 30, 110, 130, 36, 16};
                    for (int skill = 0; skill < 3; ++skill) {
                        staged.manningSkill[skill] = skills[skill];
                        staged.manningSkillProgress[skill] = skills[skill] * caps[skill] / 2;
                    }
                    staged.weaponSkill = skills[3];
                    staged.weaponSkillProgress = skills[3] * caps[3] / 2;
                    staged.repairSkill = skills[4];
                    staged.repairSkillProgress = skills[4] * caps[4] / 2;
                    staged.combatSkill = skills[5];
                    staged.combatSkillProgress = skills[5] * caps[5] / 2;
                    renderCrewInfo(staged, 940 + offset, 85);
                }
            }
            if (offer.stock > 0) {
                // StoreBox::OnRender 0x50c450: title/cost ink is constant
                // 0x830430 (245,238,163) while active+hovered, else COLOR_BUTTON_ON.
                const SDL_Color ink = buttons[index].enabled && hovered
                                          ? SDL_Color{245, 238, 163, 255}
                                          : SDL_Color{235, 245, 229, 255};
                if (systemOffer || augOffer) {
                    text(title, itemX + 48, itemY + 2, 14, false, ink);
                    text(std::to_string(offer.cost), itemX + 345, itemY + 14, 0, false, ink);
                } else {
                    text(title, itemX + 59, itemY + 58, 6, true, ink);
                    text(std::to_string(offer.cost), itemX + 42, itemY + 86, 0, false, ink);
                }
            }
        }
    }
    if (groups > 2) {
        renderer_.draw(
            "img/upgradeUI/Equipment/tabButtons/store_page" + std::to_string(panelPage_ + 1) +
                (Rect{panelPage_ == 0 ? 810.f + offset : 692.f + offset, 96, 118, 32}.contains(
                     storeMouseX_, storeMouseY_)
                     ? "_select2.png"
                     : "_on.png"),
            692 + offset,
            96);
        text(
            texts_.get("store_tab_page1", locale_), 757 + offset, 103, 62, true, {25, 49, 51, 255});
        text(
            texts_.get("store_tab_page2", locale_), 863 + offset, 103, 62, true, {25, 49, 51, 255});
    }
    renderer_.setTint(1);
    if (!capacityTooltip.empty() && (instantCapacityTooltip || animation_ - hoverTick_ > 9)) {
        // MouseControl0x4d6e00/0x4d6a30: unscaled UI hover, cursor+(0,22),
        // font12,275px wrap, measured frame+27/+18,95% black and2px outline.
        auto& font = localizedFont(12, capacityTooltip);
        const auto lines = wrappedText(12, capacityTooltip, 275);
        float measured = 0;
        for (const auto& value : lines) {
            const auto [left, right] = renderer_.bounds(font, value, textScale(12));
            measured = std::max(measured, right - left);
        }
        const float width = measured + 27;
        const float height =
            18 + static_cast<float>(lines.size()) * textLineHeight(12, capacityTooltip);
        float x = mouseX_;
        float y = mouseY_ + 22;
        if (mouseY_ + height > 670) {
            x += 16;
            y -= mouseY_ + height - 670;
            if (y + height > mouseY_) {
                y = mouseY_ - height - 10;
            }
        }
        if (mouseX_ + width > 1260) {
            x -= mouseX_ + width - 1240;
        }
        renderer_.fill({x, y, width, height}, {0, 0, 0, 242});
        for (const auto& edge : std::array<Rect, 4>{{{x, y, width, 2},
                                                     {x, y + height - 2, width, 2},
                                                     {x, y, 2, height},
                                                     {x + width - 2, y, 2, height}}}) {
            renderer_.fill(edge, {255, 255, 255, 255});
        }
        for (const auto& value : lines) {
            text(value, x + 15, y + 11, 12, false, {255, 255, 255, 255});
            y += textLineHeight(12, capacityTooltip);
        }
    }
    if (pendingStoreOffer_ >= 0) {
        const auto& offer =
            stores_.at(graph_.current()).at(static_cast<std::size_t>(pendingStoreOffer_));
        const int id = storeIndex(std::string_view(offer.item).substr(7));
        const auto message = texts_.get(id == 5 && system(13)   ? "confirm_buy_medbay"
                                        : id == 13 && system(5) ? "confirm_buy_clonebay"
                                                                : "confirm_buy_last_system",
                                        locale_);
        const auto yes = texts_.get("confirm_yes", locale_);
        const auto no = texts_.get("confirm_no", locale_);
        const auto layout = confirmationLayout(renderer_, message, yes, no, locale_ == "zh-Hans");
        auto& art = renderer_.image("img/confirm_window.png");
        const auto r = layout.frame;
        const auto piece =
            [&](float dx, float dy, float w, float h, float sx, float sy, float sw, float sh) {
                renderer_.sprite(art, {r.x + dx, r.y + dy, w, h}, {sx, sy, sw, sh});
            };
        for (int row = 0; row < 3; ++row) {
            const float dy = row == 0 ? 0 : row == 1 ? 26 : layout.textHeight + 5;
            const float height = row == 0 ? 26 : row == 1 ? layout.textHeight - 21 : 17;
            const float sy = row == 0 ? 0.f : row == 1 ? 26.f : 27.f;
            const float sh = row == 1 ? 1 : height;
            piece(0, dy, 70, height, 0, sy, 70, sh);
            piece(70, dy, r.w - 139, height, 70, sy, 1, sh);
            piece(r.w - 69, dy, 69, height, 71, sy, 69, sh);
        }
        const float bottom = layout.textHeight + 22;
        const float yesX = layout.yes.x - r.x;
        const float noX = layout.no.x - r.x;
        piece(0, bottom, 20, 46, 0, 44, 20, 46);
        piece(20, bottom, yesX - 41, 46, 20, 44, 1, 46);
        piece(yesX - 21, bottom, 31, 46, 21, 44, 31, 46);
        piece(yesX + 10, bottom, layout.yes.w - 20, 46, 52, 44, 1, 46);
        piece(yesX + layout.yes.w - 10, bottom, 34, 46, 53, 44, 34, 46);
        piece(noX + 10, bottom, layout.no.w - 20, 46, 87, 44, 1, 46);
        piece(noX + layout.no.w - 10, bottom, 31, 46, 88, 44, 31, 46);
        piece(
            noX + layout.no.w + 21, bottom, r.w - 20 - noX - layout.no.w - 21, 46, 119, 44, 1, 46);
        piece(r.w - 20, bottom, 20, 46, 120, 44, 20, 46);
        float lineY = r.y + 17;
        for (const auto& line : layout.lines) {
            text(line, r.x + r.w / 2, lineY, 52, true);
            lineY += textLineHeight(52, message);
        }
        text(yes, layout.yes.x + layout.yes.w / 2, layout.yes.y, 62, true);
        text(no, layout.no.x + layout.no.w / 2, layout.no.y, 62, true);
        return;
    }
}

std::string Run::storeHoverSoundTarget(float x, float y) const {
    // Store UI buttons use GenericButton573850's enabled entry cue.
    if (pendingStoreOffer_ >= 0) {
        const auto& offer =
            stores_.at(graph_.current()).at(static_cast<std::size_t>(pendingStoreOffer_));
        const int id = storeIndex(std::string_view(offer.item).substr(7));
        const auto message = texts_.get(id == 5 && system(13)   ? "confirm_buy_medbay"
                                        : id == 13 && system(5) ? "confirm_buy_clonebay"
                                                                : "confirm_buy_last_system",
                                        locale_);
        const auto layout = confirmationLayout(renderer_,
                                               message,
                                               texts_.get("confirm_yes", locale_),
                                               texts_.get("confirm_no", locale_),
                                               locale_ == "zh-Hans");
        return layout.yes.contains(x, y)  ? "store:confirm"
               : layout.no.contains(x, y) ? "store:cancel"
                                          : "";
    }
    x -= std::max(283.f, managementPanelX()) - 340;
    const auto closeLabel = texts_.get("button_close", locale_);
    const auto bounds = renderer_.bounds(localizedFont(63, closeLabel), closeLabel, textScale(63));
    const float closeWidth = std::max(103.f, std::round(bounds.second - bounds.first) + 6);
    if (Rect{911 - closeWidth, 555, closeWidth, 32}.contains(x, y)) {
        return "store:close";
    }
    if (storeSelling_ && Rect{345, 92, 150, 36}.contains(x, y)) {
        return "store:buy";
    }
    if (!storeSelling_ && Rect{508, 92, 159, 36}.contains(x, y)) {
        return "store:sell";
    }
    const auto found = stores_.find(graph_.current());
    if (found == stores_.end()) {
        return {};
    }
    if (!storeSelling_ && found->second.size() > 10) {
        if (panelPage_ != 0 && Rect{692, 96, 118, 32}.contains(x, y)) {
            return "store:page0";
        }
        if (panelPage_ != 1 && Rect{810, 96, 118, 32}.contains(x, y)) {
            return "store:page1";
        }
    }
    if (storeSelling_) {
        return {};
    }
    const auto buttons = storeButtons();
    const auto enabled = [&](const std::string& target) {
        return std::ranges::any_of(buttons, [&](const auto& button) {
            return button.target == target && button.enabled;
        });
    };
    for (int i = 0; i < 4; ++i) {
        const float rowY = i == 3 ? 376.f : 168.f + static_cast<float>(i == 2 ? 99 : i * 50);
        const auto target = "buy:" + std::to_string(i);
        if (enabled(target) && Rect{358, rowY, 169, 43}.contains(x, y)) {
            return target;
        }
    }
    if (enabled("store:repair_all") && Rect{358, 431, 160, 50}.contains(x, y)) {
        return "store:repair_all";
    }
    for (int row = 0; row < 2; ++row) {
        const int start = 4 + (panelPage_ * 2 + row) * 3;
        if (start >= static_cast<int>(found->second.size())) {
            break;
        }
        const auto& item = found->second[start].item;
        const bool wide =
            item.starts_with("system:") || (!item.starts_with("crew:") && item != "unavailable" &&
                                            catalog_.equipment(item).kind == "aug");
        for (int slot = 0; slot < 3; ++slot) {
            const Rect rect = wide ? Rect{543, 168.f + row * 209.f + slot * 53.f, 380, 50}
                                   : Rect{548.f + slot * 125.f, 188.f + row * 209.f, 109, 110};
            const auto target = "buy:" + std::to_string(start + slot);
            if (enabled(target) && rect.contains(x, y)) {
                return target;
            }
        }
    }
    return {};
}

void Run::storeClick(float x, float y) {
    if (pendingStoreOffer_ >= 0) {
        const auto& offer =
            stores_.at(graph_.current()).at(static_cast<std::size_t>(pendingStoreOffer_));
        const int id = storeIndex(std::string_view(offer.item).substr(7));
        const auto message = texts_.get(id == 5 && system(13)   ? "confirm_buy_medbay"
                                        : id == 13 && system(5) ? "confirm_buy_clonebay"
                                                                : "confirm_buy_last_system",
                                        locale_);
        const auto layout = confirmationLayout(renderer_,
                                               message,
                                               texts_.get("confirm_yes", locale_),
                                               texts_.get("confirm_no", locale_),
                                               locale_ == "zh-Hans");
        if (layout.yes.contains(x, y)) {
            storeAction("store:confirm", "click");
        } else if (layout.no.contains(x, y)) {
            storeAction("store:cancel", "click");
        }
        return;
    }
    const float offset = std::max(283.f, managementPanelX()) - 340;
    x -= offset;
    const auto closeLabel = texts_.get("button_close", locale_);
    const float closeWidth = std::max(
        103.f,
        std::round(
            renderer_.bounds(localizedFont(63, closeLabel), closeLabel, textScale(63)).second -
            renderer_.bounds(localizedFont(63, closeLabel), closeLabel, textScale(63)).first) +
            6);
    if (Rect{911 - closeWidth, 555, closeWidth, 32}.contains(x, y)) {
        panel_.clear();
        return;
    }
    if (Rect{345, 92, 150, 36}.contains(x, y)) {
        storeAction("store:buy", "click");
        return;
    }
    if (Rect{508, 92, 159, 36}.contains(x, y)) {
        storeAction("store:sell", "click");
        return;
    }
    if (Rect{692, 96, 118, 32}.contains(x, y)) {
        panelPage_ = 0;
        return;
    }
    if (Rect{810, 96, 118, 32}.contains(x, y)) {
        panelPage_ = 1;
        return;
    }
    const auto buttons = storeButtons();
    if (storeSelling_) {
        inventoryClick(x + offset, y);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const float rowY = i == 3 ? 376.f : 168.f + static_cast<float>(i == 2 ? 99 : i * 50);
        if (Rect{358, rowY, 169, 43}.contains(x, y)) {
            storeAction("buy:" + std::to_string(i), "click");
            return;
        }
    }
    if (Rect{358, 431, 160, 50}.contains(x, y)) {
        storeAction("store:repair_all", "click");
        return;
    }
    const auto found = stores_.find(graph_.current());
    if (found == stores_.end()) {
        return;
    }
    for (int row = 0; row < 2; ++row) {
        const int start = 4 + (panelPage_ * 2 + row) * 3;
        if (start >= static_cast<int>(found->second.size())) {
            break;
        }
        const auto& item = found->second[start].item;
        const bool wide =
            item.starts_with("system:") || (!item.starts_with("crew:") && item != "unavailable" &&
                                            catalog_.equipment(item).kind == "aug");
        for (int slot = 0; slot < 3; ++slot) {
            const Rect rect = wide ? Rect{543, 168.f + row * 209.f + slot * 53.f, 380, 50}
                                   : Rect{548.f + slot * 125.f, 188.f + row * 209.f, 109, 110};
            if (rect.contains(x, y)) {
                storeAction("buy:" + std::to_string(start + slot), "click");
                return;
            }
        }
    }
}
} // namespace ftl
