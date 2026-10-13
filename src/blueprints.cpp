#include "blueprints.hpp"
#include "travel.hpp"
#include <algorithm>
#include <charconv>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ftl {
std::array<int, 6>
randomCrewSkills(int worldLevel, RandomStream& random, std::array<int, 6> progress) {
    constexpr std::array<std::array<int, 2>, 8> ranges{
        {{0, 0}, {0, 0}, {0, 1}, {0, 2}, {1, 2}, {1, 2}, {1, 3}, {2, 3}}};
    constexpr std::array<int, 6> caps{30, 30, 110, 130, 36, 16};
    const auto range = worldLevel > 7 ? std::array{0, 0}
                                      : ranges[static_cast<std::size_t>(std::max(0, worldLevel))];
    const int count =
        std::min(4,
                 range[0] + static_cast<int>(random.next() %
                                             static_cast<std::uint32_t>(range[1] - range[0] + 1)));
    const auto first = random.next() % 6;
    auto second = random.next() % 6;
    while (second == first) {
        second = random.next() % 6;
    }
    const std::array picked{first, second};
    for (int i = 0; i < count; ++i) {
        const auto skill = picked[random.next() % 2];
        progress[skill] = std::min(caps[skill], progress[skill] + caps[skill] / 2);
    }
    return progress;
}

std::map<int, std::vector<float>> initializeRoomFireAnimations(const ShipDefinition& ship,
                                                               const BlueprintCatalog& catalog,
                                                               RandomStream& random) {
    const auto animation = catalog.animation("fire_small");
    std::map<int, std::vector<float>> result;
    // Ship::Import486500 appends rooms in authored layout order. RandomStart
    // selects a timeline frame even when the tile is not burning.
    for (const auto& room : ship.rooms) {
        auto& elapsed = result[room.id];
        const int tiles = room.w * room.h / (35 * 35);
        elapsed.reserve(std::size_t(tiles));
        for (int tile = 0; tile < tiles; ++tile) {
            // An unknown source animation is inert. Do not reproduce the
            // undefined zero-frame RandomStart modulo or invent a draw.
            elapsed.push_back(animation.usable() ? float(random.next() % animation.frames) *
                                                       animation.duration / float(animation.frames)
                                                 : 0.f);
        }
    }
    return result;
}

std::vector<float> initializeMedbayAnimations(const RoomDefinition& room, RandomStream& random) {
    std::vector<float> result;
    result.reserve(std::size_t(room.w * room.h / (35 * 35)));
    // ShipSystem61bab0 creates11-frame,1-second effects with x outer/y inner.
    for (int x = 0; x < room.w / 35; ++x) {
        for (int y = 0; y < room.h / 35; ++y) {
            result.push_back(float(random.next() % 11) / 11.f);
        }
    }
    return result;
}

std::map<int, std::vector<FireAnimationState>>
initializeFireSpreaderAnimations(const ShipDefinition& ship, RandomStream& random) {
    std::vector<const RoomDefinition*> orderedRooms;
    orderedRooms.reserve(ship.rooms.size());
    for (const auto& room : ship.rooms) {
        orderedRooms.push_back(&room);
    }
    std::sort(orderedRooms.begin(), orderedRooms.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    std::map<int, std::vector<FireAnimationState>> result;
    // Spreader735cc0 visits room IDs then columns/rows. Fire48ed10 selects
    // largeFire's8-frame phase before the smoke mirror bit for every tile.
    for (const auto* room : orderedRooms) {
        const int columns = room->w / 35;
        const int rows = room->h / 35;
        auto& states = result[room->id];
        states.resize(std::size_t(columns) * std::size_t(rows));
        for (int x = 0; x < columns; ++x) {
            for (int y = 0; y < rows; ++y) {
                auto& state = states[std::size_t(y) * std::size_t(columns) + std::size_t(x)];
                state.elapsed = float(random.next() % 8) / 8.f;
                state.smokeMirrored = (random.next() & 1) != 0;
            }
        }
    }
    return result;
}

SystemDefinition playerSystemDefinition(SystemDefinition definition) {
    if (definition.slot != -1) {
        return definition;
    }
    switch (definition.id) {
    case 0:
        definition.slot = 0;
        definition.direction = "LEFT";
        break;
    case 1:
        definition.slot = 2;
        definition.direction = "DOWN";
        break;
    case 3:
        definition.slot = 1;
        definition.direction = "UP";
        break;
    case 6:
        definition.slot = 0;
        definition.direction = "RIGHT";
        break;
    case 7:
    case 8:
        definition.slot = 0;
        definition.direction = "UP";
        break;
    case 5:
    case 13:
        definition.slot = 1;
        break;
    default:
        break;
    }
    return definition;
}

std::string EquipmentDefinition::descriptionText(const TextLibrary& texts,
                                                 const std::string& locale,
                                                 bool advanced,
                                                 bool forTooltip) const {
    // WeaponBlueprint::GetDescription0x5e2070, and InfoBox::SetDescription
    // drone0x547b40. Tooltip weapon bodies replace the authored description.
    auto tr = [&](const std::string& key) {
        return texts.get(key, locale);
    };
    auto localized = [&](const LocalizedText& value) {
        return value.reference ? tr(value.value) : value.value;
    };
    auto replace = [](std::string& line, std::string_view token, const std::string& value) {
        std::size_t position = 0;
        while ((position = line.find(token, position)) != std::string::npos) {
            line.replace(position, token.size(), value);
            position += value.size();
        }
    };
    auto format = [&](const std::string& key, const std::string& value) {
        auto line = tr(key);
        replace(line, "\\1", value);
        return line;
    };
    auto number = [](float value) {
        // TextLibrary::DoFormatText<float>0x724c30 uses default stringstream
        // formatting after promoting the float to double, with no fixed flag.
        std::ostringstream stream;
        stream << double(value);
        return stream.str();
    };
    auto body = localized(description);
    if (kind != "weapon" && kind != "drone") {
        return body;
    }
    auto append = [&](const std::string& key) {
        body += " \n" + tr(key);
    };
    auto appendValue = [&](const std::string& key, const std::string& value) {
        body += " \n" + format(key, value);
    };
    if (kind == "drone") {
        body += " \n\n" + format("required_power", std::to_string(power));
        append("drone_required");
        return body;
    }
    if (forTooltip) {
        std::string typeKey;
        if (type == "LASER") {
            typeKey = ion > 0 ? "type_ion" : "type_laser";
        } else if (type == "MISSILES") {
            typeKey = missiles > 0 ? "type_missile" : "type_crystal";
        } else if (type == "BEAM") {
            typeKey = "type_beam";
        } else if (type == "BOMB") {
            typeKey = "type_bomb";
        } else if (type == "BURST") {
            typeKey = "type_burst";
        }
        std::string effects;
        if (missiles > 0) {
            effects += tr("type_missile_effect") + " \n";
        }
        if (ion > 0) {
            effects += tr("type_ion_effect") + " \n";
        }
        if (chargeLevels > 1) {
            typeKey += "_charge";
            effects += tr("type_charge_effect") + " \n";
        }
        if (boostCount > 0) {
            typeKey += "_chain";
            effects += tr("type_chain_effect") + " \n";
        }
        auto typeText = tr(typeKey);
        if (const auto flavor = localized(flavorType); !flavor.empty()) {
            // The source swaps the flavor (prefixed with a newline) into the
            // type string, rather than appending it to the resolved type.
            typeText = "\n" + flavor;
        }
        body = tr("type_desc");
        replace(body, "\\1", typeText);
        replace(body, "\\2", effects);
    } else {
        body += " \n\n" + format("required_power", std::to_string(power));
        appendValue("charge_time", number(cooldown));
    }
    if (boostCount > 1) {
        if (boostType == "damage") {
            append("boost_power_damage");
            appendValue("damage_cap",
                        number(float(ion > 0 ? ion : damage) + float(boostCount) * boostAmount));
        } else {
            append("boost_power_speed");
            if (!forTooltip) {
                appendValue("speed_cap", number(cooldown - float(boostCount) * boostAmount));
            }
        }
    }
    if (!forTooltip && missiles > 0) {
        append("requires_missiles");
    }
    if (type == "LASER" || (type == "MISSILES" && shots > 1)) {
        appendValue("shots", std::to_string(shots));
    } else if (type == "BURST") {
        appendValue("shots", std::to_string(miniCount));
    }
    if (chargeLevels > 1) {
        appendValue("charge", std::to_string(chargeLevels));
    }
    appendValue(type == "BEAM" ? "damage_room" : "damage_shot", std::to_string(damage));
    if (piercing != 0 && (!forTooltip || piercing != 5)) {
        appendValue("shield_piercing", std::to_string(piercing));
    }
    if (forTooltip) {
        std::string chances;
        for (auto [key, enabled] : std::array<std::pair<const char*, bool>, 3>{
                 {{"chance_of_breach", breachChance != 0},
                  {"chance_of_fire", fireChance != 0},
                  {"chance_of_stun", advanced && stunChance != 0}}}) {
            if (enabled) {
                if (!chances.empty()) {
                    chances += tr("chance_of_separator");
                }
                chances += tr(key);
            }
        }
        if (!chances.empty()) {
            appendValue("chance_of", chances);
        }
    } else {
        for (auto [key, chance] : std::array<std::pair<const char*, int>, 3>{
                 {{"fire_chance", fireChance},
                  {"breach_chance", breachChance},
                  {"stun_chance", advanced ? stunChance : 0}}}) {
            if (chance != 0) {
                appendValue(key,
                            tr(chance <= 3   ? "chance_low"
                               : chance <= 6 ? "chance_medium"
                                             : "chance_high"));
            }
        }
    }
    if (ion != 0) {
        appendValue("ion_damage", std::to_string(ion));
    }
    if (stun != 0) {
        appendValue("stun_damage", std::to_string(stun));
    }
    if (personnel > 0) {
        appendValue("personnel_damage", std::to_string(personnel * 15));
    }
    if (system != 0) {
        appendValue("system_damage", std::to_string(system));
    }
    if (hullBuster) {
        append("double_damage");
    }
    return body;
}

int BlueprintCatalog::effectiveRarity(const std::string& id,
                                      bool advanced,
                                      const std::map<std::string, int>& sectorRarities) const {
    // GenerateMap 0x5d4590 applies sector rarities after ResetRarities.
    if (const auto rarityOverride = sectorRarities.find(id);
        rarityOverride != sectorRarities.end()) {
        return rarityOverride->second;
    }
    if (!advanced) {
        const auto dlc = lists_.find("DLC_ITEMS");
        if (dlc != lists_.end() &&
            std::find(dlc->second.begin(), dlc->second.end(), id) != dlc->second.end()) {
            return 0;
        }
    }
    if (const auto item = equipment_.find(id); item != equipment_.end()) {
        return item->second.rarity;
    }
    if (const auto crew = appearances_.find(id); crew != appearances_.end()) {
        return crew->second.rarity;
    }
    throw std::runtime_error("unknown_rarity_blueprint: " + id);
}

std::vector<std::pair<std::string, int>> BlueprintCatalog::rewardEquipmentPool(
    std::string_view kind, bool advanced, const std::map<std::string, int>& sectorRarities) const {
    std::vector<std::pair<std::string, int>> result;
    for (const auto& [id, definition] : equipment_) {
        const int rarity = effectiveRarity(id, advanced, sectorRarities);
        if (definition.kind == kind && rarity != 0) {
            result.emplace_back(id, 6 - rarity);
        }
    }
    return result;
}

std::vector<std::pair<std::string, int>>
BlueprintCatalog::crewRewardPool(bool advanced,
                                 const std::map<std::string, int>& sectorRarities) const {
    std::vector<std::pair<std::string, int>> result;
    for (const auto& entry : appearances_) {
        const int rarity = effectiveRarity(entry.first, advanced, sectorRarities);
        if (rarity != 0) {
            result.emplace_back(entry.first, 6 - rarity);
        }
    }
    return result;
}

namespace {
LocalizedText label(pugi::xml_node node) {
    return {node.attribute("id") ? node.attribute("id").as_string() : node.child_value(),
            bool(node.attribute("id"))};
}

int bounded(int n, int lo, int hi, const char* field) {
    if (n < lo || n > hi) {
        throw std::runtime_error(std::string("invalid_blueprint_") + field);
    }
    return n;
}

nlohmann::json textState(const LocalizedText& t) {
    return {{"value", t.value}, {"reference", t.reference}};
}
} // namespace

int BlueprintCatalog::systemId(const std::string& name) {
    static const std::array<std::string, 16> names = {"shields",
                                                      "engines",
                                                      "oxygen",
                                                      "weapons",
                                                      "drones",
                                                      "medbay",
                                                      "pilot",
                                                      "sensors",
                                                      "doors",
                                                      "teleporter",
                                                      "cloaking",
                                                      "artillery",
                                                      "battery",
                                                      "clonebay",
                                                      "mind",
                                                      "hacking"};
    auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) {
        throw std::runtime_error("unknown_system: " + name);
    }
    return int(it - names.begin());
}

std::map<std::string, std::string> initializeWeaponAnimationExplosions(const Archive& archive,
                                                                       RandomStream& random) {
    std::map<std::string, std::string> registry;
    for (const auto* path : {"data/animations.xml", "data/dlcAnimations.xml"}) {
        if (!archive.contains(path)) {
            continue;
        }
        const auto document = xmlResource(archive, path);
        auto root = document.child("FTL");
        if (!root) {
            root = document;
        }
        // ImportAnimations589de0 walks authored siblings, constructing the
        // temporary before map[] creates a default value for a new key.
        for (const auto node : root.children("weaponAnim")) {
            const std::string name = node.attribute("name").as_string();
            const auto explosion = "explosion_big" + std::to_string(random.next() % 3 + 2);
            if (!registry.contains(name)) {
                // Default WeaponAnimation454f10 selects another explosion,
                // discarded by assignment of the parameter constructor455c50.
                static_cast<void>(random.next());
            }
            registry[name] = explosion;
        }
    }
    return registry;
}

void BlueprintCatalog::weaponAnimationExplosions(
    const std::map<std::string, std::string>& registry) {
    weaponAnimationExplosions_ = registry;
    for (auto& [id, definition] : equipment_) {
        auto found = registry.find(definition.art);
        if (found == registry.end() && definition.kind == "weapon") {
            found = registry.find("default");
        }
        definition.weaponExplosionAnimation = found == registry.end() ? "" : found->second;
    }
}

BlueprintCatalog::BlueprintCatalog(const Archive& archive) : archive_(archive) {
    // ProcessBaseNode5e9860 stores sorted, deduplicated name maps by language.
    // Race attributes do not partition this original name pool.
    const auto names = xmlResource(archive, "data/names.xml");
    for (const auto list : names.child("FTL").children("nameList")) {
        auto& pool = crewNames_[list.attribute("language").as_string()];
        const bool male = std::string_view(list.attribute("sex").as_string()) == "male";
        for (const auto name : list.children("name")) {
            pool[name.child_value()] = male;
            if (name.attribute("short")) {
                crewShortNames_[name.child_value()] = name.attribute("short").as_string();
            }
        }
    }
    load("data/blueprints.xml");
    load("data/autoBlueprints.xml");
    load("data/dlcPirateBlueprints.xml");
    load("data/dlcBlueprints.xml");
    load("data/dlcBlueprintsOverwrite.xml");
    load("data/bosses.xml");
    for (auto path : {"data/achievements.xml", "data/dlcAchievements.xml"}) {
        if (archive_.contains(path)) {
            auto doc = xmlResource(archive_, path);
            for (auto n : doc.child("FTL").children("achievement")) {
                if (n.child("ship")) {
                    achievements_[n.child_value("ship")].push_back(
                        {label(n.child("name")),
                         label(n.child("desc")),
                         n.child("multiDifficulty").text().as_int() != 0});
                }
            }
        }
    }
    std::map<std::string, EquipmentDefinition> weaponAnimations;
    // Sheets registered so far across both files, in ImportAnimations order.
    std::map<std::string, AnimationFrame> sheetsSoFar;
    for (const auto* path : {"data/animations.xml", "data/dlcAnimations.xml"}) {
        if (archive_.contains(path)) {
            auto doc = xmlResource(archive_, path);
            auto root = doc.child("FTL");
            if (!root) {
                root = doc;
            }
            // AnimationControl::ImportAnimations 0x589de0 walks the file once in
            // document order, so a weaponAnim binds the sheet defined so far:
            // animations.xml redefines bomb_fire/bomb_heal/bomb_ion sheets as
            // projectile strips after the launcher weaponAnims use them.
            for (auto n : root.children()) {
                const std::string_view kind = n.name();
                if (kind == "animSheet") {
                    animations_[n.attribute("name").as_string()] = {
                        "img/" + std::string(n.child_value()),
                        0,
                        0,
                        n.attribute("fw").as_int(),
                        n.attribute("fh").as_int(),
                        n.attribute("h").as_int()};
                    continue;
                }
                if (kind != "anim") {
                    continue;
                }
                if (auto it = animations_.find(n.child_value("sheet")); it != animations_.end()) {
                    auto f = it->second;
                    f.x = n.child("desc").attribute("x").as_int() * f.width;
                    f.y = f.sheetHeight - (n.child("desc").attribute("y").as_int() + 1) * f.height;
                    f.frames = n.child("desc").attribute("length").as_int(1);
                    f.duration = n.child("time").text().as_float(1);
                    animations_[n.attribute("name").as_string()] = f;
                }
            }
            for (auto n : root.children()) {
                const std::string_view kind = n.name();
                if (kind == "animSheet") {
                    sheetsSoFar[n.attribute("name").as_string()] = {
                        "img/" + std::string(n.child_value()),
                        0,
                        0,
                        n.attribute("fw").as_int(),
                        n.attribute("fh").as_int(),
                        n.attribute("h").as_int()};
                    continue;
                }
                if (kind != "weaponAnim") {
                    continue;
                }
                auto& e = weaponAnimations[n.attribute("name").as_string()];
                e = EquipmentDefinition{};
                e.chargedFrame = n.child("chargedFrame").text().as_int();
                e.fireFrame = n.child("fireFrame").text().as_int();
                e.animationFrames = n.child("desc").attribute("length").as_int(1);
                e.weaponAnimationTime = n.child("time").text().as_float(1);
                // ImportAnimations589de0 applies these optional layers
                // after constructing the authored WeaponAnimation.
                if (n.child("chargeImage")) {
                    e.chargeImage = "img/" + std::string(n.child_value("chargeImage"));
                }
                if (n.child("delayChargeAnim")) {
                    e.delayChargeAnimation = n.child("delayChargeAnim").text().as_float();
                }
                if (n.child("boost")) {
                    e.boostAnimation = n.child_value("boost");
                }
                e.fireX = n.child("firePoint").attribute("x").as_int();
                e.fireY = n.child("firePoint").attribute("y").as_int();
                e.chargeOffset = n.child("firePoint").attribute("charge").as_int();
                e.mountX = n.child("mountPoint").attribute("x").as_int();
                e.mountY = n.child("mountPoint").attribute("y").as_int();
                // GetWeapon588340 copies the registered weapon animation,
                // whose sheet alias may differ from its animation name.
                if (const auto sheet = sheetsSoFar.find(n.child_value("sheet"));
                    sheet != sheetsSoFar.end()) {
                    e.imagePath = sheet->second.path;
                    e.frameWidth = sheet->second.width;
                    e.frameHeight = sheet->second.height;
                    e.frameX = n.child("desc").attribute("x").as_int() * e.frameWidth;
                    e.frameY = sheet->second.sheetHeight -
                               (n.child("desc").attribute("y").as_int() + 1) * e.frameHeight;
                }
            }
            for (auto n : root.children("animSheet")) {
                for (auto& [id, e] : equipment_) {
                    if (e.art == n.attribute("name").as_string()) {
                        e.imagePath = "img/" + std::string(n.child_value());
                        e.frameWidth = n.attribute("fw").as_int();
                        e.frameHeight = n.attribute("fh").as_int();
                    }
                }
            }
        }
    }
    for (auto& [id, e] : equipment_) {
        if (e.kind != "weapon") {
            continue;
        }
        auto animation = weaponAnimations.find(e.art);
        if (animation == weaponAnimations.end()) {
            // Original missing weapon art recursively resolves "default";
            // copying it consumes no additional constructor RNG.
            animation = weaponAnimations.find("default");
        }
        if (animation == weaponAnimations.end()) {
            throw std::runtime_error("missing_default_weapon_animation");
        }
        const auto& source = animation->second;
        e.imagePath = source.imagePath;
        e.frameWidth = source.frameWidth;
        e.frameHeight = source.frameHeight;
        e.frameX = source.frameX;
        e.frameY = source.frameY;
        e.chargedFrame = source.chargedFrame;
        e.fireFrame = source.fireFrame;
        e.animationFrames = source.animationFrames;
        e.weaponAnimationTime = source.weaponAnimationTime;
        e.chargeImage = source.chargeImage;
        e.delayChargeAnimation = source.delayChargeAnimation;
        e.boostAnimation = source.boostAnimation;
        e.fireX = source.fireX;
        e.fireY = source.fireY;
        e.chargeOffset = source.chargeOffset;
        e.mountX = source.mountX;
        e.mountY = source.mountY;
    }
    for (auto& [id, s] : ships_) {
        layout(s);
    }
    auto roomDoc = xmlResource(archive_, "data/rooms.xml");
    auto roomRoot = roomDoc.child("FTL");
    if (!roomRoot) {
        roomRoot = roomDoc;
    }
    for (auto& [id, s] : ships_) {
        for (auto& sys : s.systems) {
            if (!sys.image.empty()) {
                const auto image = sys.image.starts_with("room_") ? sys.image.substr(5) : sys.image;
                for (auto n : roomRoot.children("roomLayout")) {
                    if (image == n.attribute("name").as_string()) {
                        if (auto glow = n.child("computerGlow")) {
                            sys.glowName = glow.attribute("name").as_string("glow");
                            sys.glowX = glow.attribute("x").as_int();
                            sys.glowY = glow.attribute("y").as_int();
                            sys.glowDirection = glow.attribute("dir").as_string();
                        }
                    }
                }
            }
        }
    }
    for (int family = 0; family < 10; ++family) {
        for (int type = 0; type < 3; ++type) {
            const auto* s = ship(family, type);
            if (!s) {
                if (type == 2 && family >= 8) {
                    continue;
                }
                throw std::runtime_error("missing_player_ship");
            }
            for (const auto& id : s->weapons) {
                if (equipment(id).kind != "weapon") {
                    throw std::runtime_error("invalid_ship_weapon");
                }
            }
            for (const auto& system : s->systems) {
                if (system.id == 11 &&
                    (system.weapon.empty() || equipment(system.weapon).kind != "weapon")) {
                    throw std::runtime_error("invalid_ship_artillery");
                }
            }
            for (const auto& id : s->drones) {
                const auto& drone = equipment(id);
                if (drone.kind != "drone" || ((drone.type == "DEFENSE" || drone.type == "COMBAT") &&
                                              (drone.droneWeapon.empty() ||
                                               equipment(drone.droneWeapon).kind != "weapon"))) {
                    throw std::runtime_error("invalid_ship_drone");
                }
            }
            for (const auto& id : s->augments) {
                if (equipment(id).kind != "aug") {
                    throw std::runtime_error("invalid_ship_augment");
                }
            }
        }
    }
}

std::pair<std::string, bool>
BlueprintCatalog::unusedCrewName(const std::string& locale,
                                 const std::vector<std::string>& used,
                                 const std::function<std::uint32_t()>& random) const {
    // GetCrewName5ddfe0/GetUnusedCrewName5e1ef0: locale pool or default,
    // one uniform sorted-map draw, up to21 attempts to avoid current names.
    const auto found = crewNames_.find(locale);
    const auto& pool = found == crewNames_.end() ? crewNames_.at("") : found->second;
    if (pool.empty()) {
        throw std::runtime_error("empty_crew_name_pool");
    }
    std::pair<std::string, bool> result;
    for (int attempt = 0; attempt < 21; ++attempt) {
        auto selected = pool.begin();
        std::advance(selected, static_cast<std::ptrdiff_t>(random() % pool.size()));
        result = *selected;
        if (std::find(used.begin(), used.end(), result.first) == used.end() &&
            std::find(currentCrewNames_.begin(), currentCrewNames_.end(), result.first) ==
                currentCrewNames_.end()) {
            break;
        }
    }
    // GetUnusedCrewName5e1ef0 records even names whose blueprints are later
    // discarded. Factory::OnLoop45e090 replaces this interim history with
    // the friendly roster; generation alone must not erase earlier draws.
    currentCrewNames_.push_back(result.first);
    return result;
}

std::string
BlueprintCatalog::shortCrewName(const std::string& name,
                                const std::function<float(const std::string&)>& measure) const {
    // CrewBlueprint::ShortenName5e5f90. Original delimiters are space,
    // U+00B7 at0x836f85 and U+30FB at0x836f88, then49px font6 bounds.
    if (const auto preset = crewShortNames_.find(name);
        preset != crewShortNames_.end() && !preset->second.empty()) {
        return preset->second;
    }
    if (int(measure(name)) <= 49) {
        return name;
    }
    constexpr std::array<std::string_view, 3> separators{" ", "\xc2\xb7", "\xe3\x83\xbb"};
    std::vector<std::string> tokens;
    std::size_t cursor = 0;
    do {
        auto end = name.size();
        for (const auto separator : separators) {
            const auto found = name.find(separator, cursor);
            if (found != std::string::npos) {
                end = std::min(end, found);
            }
        }
        tokens.push_back(name.substr(cursor, end - cursor));
        cursor = end;
        bool skipped = true;
        while (skipped) {
            skipped = false;
            for (const auto separator : separators) {
                if (std::string_view(name).substr(cursor).starts_with(separator)) {
                    cursor += separator.size();
                    skipped = true;
                }
            }
        }
    } while (cursor < name.size());
    if (measure(tokens.back()) < 49) {
        return tokens.back();
    }
    std::size_t shortest = 0;
    int shortestWidth = 1000;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const int width = int(measure(tokens[index]));
        if (width < shortestWidth) {
            shortest = index;
            shortestWidth = width;
        }
    }
    auto result = tokens[shortest];
    const float dotWidth = measure("..") - measure(".");
    float margin = 0;
    bool shortened = false;
    while (!result.empty() && measure(result) > 49 - margin) {
        result.pop_back();
        margin = dotWidth;
        shortened = true;
    }
    if (shortened) {
        result += '.';
    }
    return result;
}

void BlueprintCatalog::load(const std::string& path) {
    auto doc = xmlResource(archive_, path);
    auto root = doc.child("FTL");
    if (!root) {
        root = doc;
    }
    for (auto n : root.children()) {
        const std::string tag = n.name();
        const std::string id = n.attribute("name").as_string();
        if (tag == "blueprintList") {
            std::vector<std::string> entries;
            for (auto entry : n.children("name")) {
                entries.emplace_back(entry.child_value());
            }
            lists_[id] = std::move(entries);
            continue;
        }
        if (tag == "shipBlueprint") {
            ShipDefinition s;
            s.id = id;
            s.minSector = n.child("minSector").text().as_int(0);
            s.maxSector = n.child("maxSector").text().as_int(std::numeric_limits<int>::max());
            s.source = path;
            s.layout = n.attribute("layout").as_string();
            s.image = n.attribute("img").as_string();
            s.weaponLoad = n.child("weaponList").attribute("load").as_string();
            s.droneLoad = n.child("droneList").attribute("load").as_string();
            s.floorImage = n.child("floorImage") ? n.child_value("floorImage") : s.image;
            s.shieldImage = n.child_value("shieldImage");
            s.cloakImage = n.child_value("cloakImage");
            s.name = n.child("name") ? label(n.child("name"))
                                     : LocalizedText{"should not be seen", false};
            s.shipClass = label(n.child("class"));
            s.description = label(n.child("desc"));
            s.unlock = label(n.child("unlock"));
            s.hull = bounded(n.child("health").attribute("amount").as_int(), 1, 1000, "hull");
            s.reactor =
                bounded(n.child("maxPower").attribute("amount").as_int(), 0, 100, "reactor");
            s.weaponSlots = bounded(n.child("weaponSlots").text().as_int(4), 0, 8, "weapon_slots");
            s.droneSlots = bounded(n.child("droneSlots").text().as_int(4), 0, 8, "drone_slots");
            auto weapons = n.child("weaponList");
            auto drones = n.child("droneList");
            s.missiles = bounded(weapons.attribute("missiles").as_int(), 0, 999, "missiles");
            s.droneParts = bounded(drones.attribute("drones").as_int(), 0, 999, "drone_parts");
            for (auto w : weapons.children("weapon")) {
                s.weapons.emplace_back(w.attribute("name").as_string());
            }
            for (auto d : drones.children("drone")) {
                s.drones.emplace_back(d.attribute("name").as_string());
            }
            s.weaponCount = bounded(
                weapons.attribute("count").as_int(int(s.weapons.size())), 0, 8, "weapon_count");
            s.droneCount = bounded(
                drones.attribute("count").as_int(int(s.drones.size())), 0, 8, "drone_count");
            for (auto a : n.children("aug")) {
                s.augments.emplace_back(a.attribute("name").as_string());
            }
            for (auto c : n.children("crewCount")) {
                if (c.attribute("max")) {
                    s.maxCrew = bounded(c.attribute("max").as_int(), 0, 8, "max_crew");
                }
                s.crew.push_back({c.attribute("class").as_string("human"),
                                  bounded(c.attribute("amount").as_int(), 0, 8, "crew_count")});
            }
            for (auto sys : n.child("systemList").children()) {
                SystemDefinition d;
                d.name = sys.name();
                d.id = systemId(d.name);
                // ProcessShipBlueprint5e77b0 accumulates locations/weapons per
                // systemID while each present scalar replaces the shared template.
                const auto previous =
                    std::find_if(s.systems.rbegin(), s.systems.rend(), [&](const auto& value) {
                        return value.id == d.id;
                    });
                if (previous != s.systems.rend()) {
                    d.power = previous->power;
                    d.maxPower = previous->maxPower;
                    d.bp = previous->bp;
                    d.image = previous->image;
                    d.slot = previous->slot;
                    d.direction = previous->direction;
                }
                if (sys.attribute("img")) {
                    d.image = sys.attribute("img").as_string();
                }
                d.weapon = sys.attribute("weapon").as_string();
                d.room = sys.attribute("room").as_int(-1);
                d.power = bounded(sys.attribute("power").as_int(d.power), 0, 20, "system_power");
                d.maxPower = sys.attribute("max").as_int(d.maxPower);
                d.bp = sys.attribute("bp").as_int(d.bp);
                const std::string_view start = sys.attribute("start").value();
                d.installed = !sys.attribute("start") || start == "true" || start == "TRUE" ||
                              start == "True";
                if (d.image.empty() &&
                    archive_.contains("img/ship/interior/room_" + d.name + ".png")) {
                    d.image = "room_" + d.name;
                }
                d.slot = sys.child("slot").child("number").text().as_int(d.slot);
                if (sys.child("slot").child("direction")) {
                    d.direction = sys.child("slot").child_value("direction");
                }
                for (auto& value : s.systems) {
                    if (value.id == d.id) {
                        value.power = d.power;
                        value.maxPower = d.maxPower;
                        value.bp = d.bp;
                        value.image = d.image;
                        value.slot = d.slot;
                        value.direction = d.direction;
                    }
                }
                s.systems.push_back(d);
            }
            ships_[id] = std::move(s);
        } else if (tag == "systemBlueprint") {
            SystemBlueprint s;
            s.cost = n.child("cost").text().as_int();
            s.title = label(n.child("title"));
            s.description = label(n.child("desc"));
            s.maxPower = n.child("maxPower").text().as_int();
            s.startPower = n.child("startPower").text().as_int();
            for (auto c : n.child("upgradeCost").children()) {
                s.costs.push_back(c.text().as_int());
            }
            systems_[id] = std::move(s);
        } else if (tag == "itemBlueprint") {
            // ProcessItemBlueprint0x5decd0/StringToItem0x5a8b70. Items have
            // their own catalog and never enter weapon/drone/augment pools.
            ItemDefinition item;
            item.id = id;
            item.source = path;
            const std::string type = n.child_value("type");
            item.type = type == "ITEM_MISSILE" ? 0
                        : type == "ITEM_FUEL"  ? 1
                        : type == "ITEM_DRONE" ? 2
                                               : -1;
            item.cost = n.child("cost").text().as_int();
            item.rarity = n.child("rarity").text().as_int();
            item.bp = n.child("bp").text().as_int();
            item.title = label(n.child("title"));
            item.shortTitle = label(n.child("short"));
            item.description = label(n.child("desc"));
            item.tooltip = label(n.child("tooltip"));
            for (auto tip : n.children("tip")) {
                item.tip = label(tip);
                item.tip.reference = true;
            }
            items_[id] = std::move(item);
        } else if (tag == "crewBlueprint") {
            CrewAppearance c;
            c.source = path;
            c.cost = n.child("cost").text().as_int();
            c.rarity = n.child("rarity").text().as_int();
            c.shortTitle = label(n.child("short"));
            c.title = label(n.child("title"));
            c.description = label(n.child("desc"));
            for (auto power : n.child("powerList").children("power")) {
                c.powers.push_back(label(power));
            }
            for (auto layer : n.child("colorList").children("layer")) {
                std::vector<std::array<float, 4>> colors;
                for (auto color : layer.children("color")) {
                    colors.push_back({color.attribute("r").as_float(),
                                      color.attribute("g").as_float(),
                                      color.attribute("b").as_float(),
                                      color.attribute("a").as_float()});
                }
                if (!colors.empty()) {
                    c.layers.push_back(colors);
                }
            }
            appearances_[id] = std::move(c);
        } else if (tag == "weaponBlueprint" || tag == "droneBlueprint" || tag == "augBlueprint") {
            EquipmentDefinition e;
            e.id = id;
            e.kind = tag.substr(0, tag.find("Blueprint"));
            e.source = path;
            e.type = n.child_value("type");
            e.projectileImage = n.child_value("image");
            e.explosionAnimation = n.child_value("explosion");
            if (auto color = n.child("color")) {
                e.beamColor = {color.child("r").text().as_int(),
                               color.child("g").text().as_int(),
                               color.child("b").text().as_int()};
                for (auto& component : e.beamColor) {
                    component = std::clamp(component, 0, 255);
                }
            }
            auto sounds = [&](const char* group, std::vector<std::string>& output) {
                for (auto sound : n.child(group).children("sound")) {
                    output.emplace_back(sound.child_value());
                }
            };
            sounds("launchSounds", e.launchSounds);
            sounds("hitShipSounds", e.hitShipSounds);
            sounds("hitShieldSounds", e.hitShieldSounds);
            sounds("missSounds", e.missSounds);
            e.projectileSpeed = e.kind == "weapon" ? float(n.child("speed").text().as_int())
                                                   : n.child("speed").text().as_float();
            if (e.kind == "drone") {
                // ProcessDroneBlueprint 0x5de790 maps the older <level>2</level>
                // form to target type 4 even without a <target> tag.
                e.droneTarget = n.child("target")                       ? n.child_value("target")
                                : n.child("level").text().as_int() == 2 ? "LASERS"
                                                                        : "MISSILES";
                e.droneWeapon = n.child_value("weaponBlueprint");
                e.iconImage = n.child_value("iconImage");
                e.droneSpeed = e.projectileSpeed;
                e.droneDodge = bounded(n.child("dodge").text().as_int(), 0, 10, "drone_dodge");
            }
            e.art = n.child_value("weaponArt");
            if (e.art.empty()) {
                e.art = n.child_value("droneImage");
            }
            e.title = label(n.child("title"));
            e.shortTitle = label(n.child("short"));
            e.description = label(n.child("desc"));
            e.tooltip = label(n.child("tooltip"));
            for (auto tip : n.children("tip")) {
                // Blueprint tip child values are text-library keys, even without id.
                auto value = label(tip);
                value.reference = true;
                e.tips.push_back(std::move(value));
            }
            if (!e.tips.empty()) {
                e.tip = e.tips.back();
            }
            e.flavorType = label(n.child("flavorType"));
            e.iconImage = n.child_value("iconImage");
            e.locked = n.child("locked").text().as_bool();
            e.stacking = n.child("stackable").text().as_bool();
            e.cost = n.child("cost").text().as_int();
            e.rarity = n.child("rarity").text().as_int();
            e.power = n.child("power").text().as_int();
            e.damage = n.child("damage").text().as_int();
            e.shots = n.child("shots").text().as_int();
            // ProcessWeaponBlueprint0x5e41d0 assigns both beam endpoints.
            if (e.kind == "weapon" && e.type == "BEAM") {
                e.shots = 2;
            }
            e.cooldown = n.child("cooldown").text().as_float();
            if (e.kind == "drone") {
                // DroneBlueprint XML parsing in the original converts the
                // authored millisecond cooldown to game-time seconds.
                e.cooldown /= 1000.f;
            }
            e.value = n.child("value").text().as_float();
            e.missiles = n.child("missiles").text().as_int();
            e.piercing = n.child("sp").text().as_int();
            e.fireChance = n.child("fireChance").text().as_int();
            e.breachChance = n.child("breachChance").text().as_int();
            e.stunChance = n.child("stunChance").text().as_int();
            e.ion = n.child("ion").text().as_int();
            e.personnel = n.child("persDamage").text().as_int();
            e.system = n.child("sysDamage").text().as_int();
            e.stun = n.child("stun").text().as_int();
            e.length = n.child("length").text().as_int(e.kind == "weapon" ? -1 : 0);
            e.radius = n.child("radius").text().as_int();
            e.spin = n.child("spin").text().as_int();
            e.droneTargetable = n.child("drone_targetable").text().as_int(-1);
            e.chargeLevels = n.child("chargeLevels").text().as_int(1);
            e.hullBuster = n.child("hullBust").text().as_int() != 0;
            e.lockdown = n.child("lockdown").text().as_int() != 0;
            for (auto m : n.child("projectiles").children("projectile")) {
                EquipmentDefinition::MiniProjectile mini;
                mini.image = m.child_value();
                mini.count = bounded(m.attribute("count").as_int(), 0, 100, "mini_count");
                mini.fake = std::string_view(m.attribute("fake").value()) == "true";
                e.miniCount += mini.fake ? 0 : mini.count;
                e.miniProjectiles.push_back(std::move(mini));
            }
            e.boostCount = n.child("boost").child("count").text().as_int();
            e.boostAmount = n.child("boost").child("amount").text().as_float();
            e.boostType = n.child("boost").child_value("type");
            equipment_[id] = std::move(e);
        }
    }
}

void BlueprintCatalog::layout(ShipDefinition& s) {
    auto data = archive_.read("data/" + s.layout + ".txt");
    std::istringstream in(std::string(data.begin(), data.end()));
    std::string tag;
    auto number = [&]() {
        int value;
        if (!(in >> value)) {
            throw std::runtime_error("invalid_ship_layout: " + s.layout);
        }
        return bounded(value, -1000, 1000, "layout_value");
    };
    while (in >> tag) {
        if (tag == "X_OFFSET") {
            s.offsetX = number();
        } else if (tag == "Y_OFFSET") {
            s.offsetY = number();
        } else if (tag == "VERTICAL") {
            s.verticalShift = number();
        } else if (tag == "HORIZONTAL") {
            s.horizontalShift = number();
        } else if (tag == "ELLIPSE") {
            for (auto& v : s.ellipse) {
                v = number();
            }
        } else if (tag == "ROOM") {
            RoomDefinition r;
            r.id = number();
            r.x = number();
            r.y = number();
            r.w = number();
            r.h = number();
            s.rooms.push_back(r);
        } else if (tag == "DOOR") {
            DoorDefinition d;
            d.id = int(s.doors.size());
            d.x = number();
            d.y = number();
            d.roomA = number();
            d.roomB = number();
            d.vertical = number() != 0;
            s.doors.push_back(d);
        } else {
            throw std::runtime_error("unknown_layout_token: " + tag);
        }
    }
    std::set<int> roomIds;
    for (auto& r : s.rooms) {
        if (!roomIds.insert(r.id).second) {
            throw std::runtime_error("duplicate_room");
        }
        r.x = (r.x + s.offsetX) * 35;
        r.y = (r.y + s.offsetY) * 35;
        r.w = bounded(r.w, 1, 10, "room_width") * 35;
        r.h = bounded(r.h, 1, 10, "room_height") * 35;
    }
    for (auto& d : s.doors) {
        d.x = (d.x + s.offsetX) * 35;
        d.y = (d.y + s.offsetY) * 35;
        if ((d.roomA != -1 && !roomIds.contains(d.roomA)) ||
            (d.roomB != -1 && !roomIds.contains(d.roomB))) {
            throw std::runtime_error("invalid_door_room");
        }
    }
    for (const auto& d : s.systems) {
        if (!roomIds.contains(d.room)) {
            throw std::runtime_error("invalid_system_room");
        }
    }
    if (s.rooms.empty() || s.rooms.size() > 100 || s.doors.size() > 200) {
        throw std::runtime_error("invalid_layout_size");
    }
    auto xml = xmlResource(archive_, "data/" + s.layout + ".xml");
    auto root = xml.child("FTL");
    if (!root) {
        root = xml;
    }
    auto im = root.child("img");
    auto floor = root.child("offsets").child("floor");
    auto cloak = root.child("offsets").child("cloak");
    s.imageRect = {im.attribute("x").as_int(),
                   im.attribute("y").as_int(),
                   im.attribute("w").as_int(),
                   im.attribute("h").as_int()};
    s.floorOffset = {floor.attribute("x").as_int(), floor.attribute("y").as_int()};
    s.cloakOffset = {cloak.attribute("x").as_int(), cloak.attribute("y").as_int()};
    for (auto n : root.child("weaponMounts").children("mount")) {
        s.mounts.push_back({n.attribute("x").as_int(),
                            n.attribute("y").as_int(),
                            n.attribute("rotate").as_bool(),
                            n.attribute("mirror").as_bool(),
                            n.attribute("slide").as_string("none"),
                            n.attribute("gib").as_int(-1)});
    }
    auto explosion = root.child("explosion");
    for (int id = 1; id <= 16; ++id) {
        const auto name = "gib" + std::to_string(id);
        auto node = explosion.child(name.c_str());
        if (!node) {
            break;
        }
        ShipGib gib;
        gib.x = node.child("x").text().as_int();
        gib.y = node.child("y").text().as_int();
        gib.velocityMin = node.child("velocity").attribute("min").as_float();
        gib.velocityMax = node.child("velocity").attribute("max").as_float();
        gib.directionMin = node.child("direction").attribute("min").as_float();
        gib.directionMax = node.child("direction").attribute("max").as_float();
        gib.angularMin = node.child("angular").attribute("min").as_float();
        gib.angularMax = node.child("angular").attribute("max").as_float();
        s.gibs.push_back(gib);
    }
}

const ShipDefinition* BlueprintCatalog::ship(int family, int type) const {
    if (family < 0 || family >= 10 || type < 0 || type > 2) {
        return nullptr;
    }
    const auto id = std::string(families[family]) + (type ? "_" + std::to_string(type + 1) : "");
    auto it = ships_.find(id);
    return it == ships_.end() ? nullptr : &it->second;
}

const ShipDefinition& BlueprintCatalog::ship(const std::string& id) const {
    auto it = ships_.find(id);
    if (it == ships_.end()) {
        throw std::runtime_error("unknown_ship: " + id);
    }
    return it->second;
}

std::vector<std::string> BlueprintCatalog::shipIds() const {
    std::vector<std::string> result;
    result.reserve(ships_.size());
    for (const auto& entry : ships_) {
        result.push_back(entry.first);
    }
    return result;
}

std::vector<std::string> BlueprintCatalog::crewIds() const {
    std::vector<std::string> result;
    result.reserve(appearances_.size());
    for (const auto& entry : appearances_) {
        result.push_back(entry.first);
    }
    return result;
}

const ShipDefinition&
BlueprintCatalog::selectShip(const std::string& id, int sector, RandomStream& random) const {
    // GetShipBlueprint0x5e6710 recursively samples nested lists; duplicates retain weight.
    const auto resolve = [&](const auto& self,
                             const std::string& name,
                             int level,
                             int depth) -> const ShipDefinition& {
        if (depth > 64) {
            throw std::runtime_error("recursive_ship_blueprint_list");
        }
        const auto list = lists_.find(name);
        if (list == lists_.end()) {
            return ship(name);
        }
        if (list->second.empty()) {
            throw std::runtime_error("empty_ship_blueprint_list");
        }
        std::vector<const ShipDefinition*> eligible;
        for (const auto& child : list->second) {
            const auto& candidate = self(self, child, level, depth + 1);
            if (level == -1 || (level >= candidate.minSector && level <= candidate.maxSector)) {
                eligible.push_back(&candidate);
            }
        }
        if (!eligible.empty()) {
            return *eligible[random.next() % eligible.size()];
        }
        return self(self, list->second[random.next() % list->second.size()], -1, depth + 1);
    };
    return resolve(resolve, id, sector, 0);
}

const EquipmentDefinition& BlueprintCatalog::equipment(const std::string& id) const {
    if (id == "ENV_ASTEROID") {
        // Asteroid::Asteroid 0x453290. An environmental payload is not a
        // purchasable blueprint and must never enter stock/reward pools.
        static const EquipmentDefinition asteroid = [] {
            EquipmentDefinition value;
            value.id = "ENV_ASTEROID";
            value.kind = "weapon";
            value.type = "LASER";
            value.damage = 1;
            value.fireChance = 2;
            value.breachChance = 1;
            value.projectileSpeed = 35.f;
            value.droneTargetable = 1;
            value.spin = 10;
            value.hitShipSounds = {"hitHull1"};
            value.hitShieldSounds = {"hitShield1"};
            value.missSounds.clear();
            return value;
        }();
        return asteroid;
    }
    auto it = equipment_.find(id);
    if (it == equipment_.end()) {
        throw std::runtime_error("unknown_equipment: " + id);
    }
    return it->second;
}

const std::vector<std::string>& BlueprintCatalog::list(const std::string& id) const {
    auto it = lists_.find(id);
    if (it == lists_.end()) {
        throw std::runtime_error("unknown_blueprint_list: " + id);
    }
    return it->second;
}

nlohmann::json ShipDefinition::snapshot() const {
    nlohmann::json rooms_ = nlohmann::json::array();
    nlohmann::json doors_ = rooms_;
    nlohmann::json systems_ = rooms_;
    nlohmann::json crew_ = rooms_;
    for (const auto& r : rooms) {
        rooms_.push_back({{"id", r.id}, {"rect", {r.x, r.y, r.w, r.h}}});
    }
    for (const auto& d : doors) {
        doors_.push_back({{"id", d.id},
                          {"position", {d.x, d.y}},
                          {"rooms", {d.roomA, d.roomB}},
                          {"vertical", d.vertical}});
    }
    for (const auto& d : systems) {
        systems_.push_back({{"id", d.id},
                            {"name", d.name},
                            {"room", d.room},
                            {"power", d.power},
                            {"max_power", d.maxPower},
                            {"bp", d.bp},
                            {"installed", d.installed},
                            {"image", d.image},
                            {"weapon", d.weapon},
                            {"slot", d.slot},
                            {"direction", d.direction}});
    }
    for (const auto& c : crew) {
        crew_.push_back({{"species", c.species}, {"count", c.count}});
    }
    return {{"id", id},
            {"layout", layout},
            {"image", image},
            {"weapon_load", weaponLoad},
            {"drone_load", droneLoad},
            {"floor_image", floorImage},
            {"image_rect", imageRect},
            {"floor_offset", floorOffset},
            {"cloak_offset", cloakOffset},
            {"source", source},
            {"min_sector", minSector},
            {"max_sector", maxSector},
            {"name", textState(name)},
            {"class", textState(shipClass)},
            {"hull", hull},
            {"reactor", reactor},
            {"weapon_slots", weaponSlots},
            {"drone_slots", droneSlots},
            {"missiles", missiles},
            {"drone_parts", droneParts},
            {"max_crew", maxCrew},
            {"weapon_count", weaponCount},
            {"drone_count", droneCount},
            {"weapons", weapons},
            {"drones", drones},
            {"augments", augments},
            {"rooms", rooms_},
            {"doors", doors_},
            {"systems", systems_},
            {"crew", crew_}};
}

nlohmann::json BlueprintCatalog::snapshot() const {
    auto out = nlohmann::json::array();
    auto availability = nlohmann::json::array();
    for (int f = 0; f < 10; ++f) {
        for (int t = 0; t < 3; ++t) {
            const auto* definition = ship(f, t);
            availability.push_back({{"family", f},
                                    {"type", t},
                                    {"exists", definition != nullptr},
                                    {"requires_advanced", f == 9 || t == 2}});
            if (auto s = definition) {
                auto row = s->snapshot();
                row["family"] = f;
                row["type"] = t;
                out.push_back(row);
            }
        }
    }
    auto art = nlohmann::json::object();
    for (const auto& [id, e] : equipment_) {
        if (e.kind == "weapon") {
            auto tips = nlohmann::json::array();
            for (const auto& tip : e.tips) {
                tips.push_back(textState(tip));
            }
            const std::string iconPath =
                e.iconImage.empty() ? std::string{}
                                    : "img/systemUI/weapbox_icon_W_" + e.iconImage + ".png";
            art[id] = {{"frame_width", e.frameWidth},
                       {"frame_x", e.frameX},
                       {"frame_y", e.frameY},
                       {"frame_height", e.frameHeight},
                       {"frame", e.chargedFrame},
                       {"animation_time", e.weaponAnimationTime},
                       {"charge_image", e.chargeImage},
                       {"boost_animation", e.boostAnimation},
                       {"delay_charge_animation", e.delayChargeAnimation},
                       {"weapon_explosion_animation", e.weaponExplosionAnimation},
                       {"charge_offset", e.chargeOffset},
                       {"mount", {e.mountX, e.mountY}},
                       {"path", e.imagePath},
                       {"icon", e.iconImage},
                       {"icon_path", iconPath},
                       {"icon_available", !iconPath.empty() && archive_.contains(iconPath)},
                       {"locked", e.locked},
                       {"store_available", e.cost > 0 && e.rarity > 0 && !e.locked},
                       {"cost", e.cost},
                       {"rarity", e.rarity},
                       {"stacking", e.stacking},
                       {"title", textState(e.title)},
                       {"short_title", textState(e.shortTitle)},
                       {"description", textState(e.description)},
                       {"tooltip", textState(e.tooltip)},
                       {"tip", textState(e.tip)},
                       {"tips", std::move(tips)},
                       {"flavor_type", textState(e.flavorType)}};
            art[id]["payload"] = {{"source", e.source},
                                  {"type", e.type},
                                  {"power", e.power},
                                  {"cooldown", e.cooldown},
                                  {"damage", e.damage},
                                  {"shots", e.shots},
                                  {"missiles", e.missiles},
                                  {"piercing", e.piercing},
                                  {"ion", e.ion},
                                  {"personnel", e.personnel},
                                  {"system", e.system},
                                  {"fire_chance", e.fireChance},
                                  {"breach_chance", e.breachChance},
                                  {"stun_chance", e.stunChance},
                                  {"stun", e.stun},
                                  {"length", e.length},
                                  {"radius", e.radius},
                                  {"charge_levels", e.chargeLevels},
                                  {"hull_buster", e.hullBuster},
                                  {"lockdown", e.lockdown},
                                  {"boost_count", e.boostCount},
                                  {"boost_amount", e.boostAmount},
                                  {"boost_type", e.boostType}};
        }
    }
    auto drones = nlohmann::json::object();
    for (const auto& [id, e] : equipment_) {
        if (e.kind == "drone") {
            drones[id] = {{"type", e.type},
                          {"source", e.source},
                          {"art", e.art},
                          {"cost", e.cost},
                          {"rarity", e.rarity},
                          {"locked", e.locked},
                          {"icon", e.iconImage},
                          {"target", e.droneTarget},
                          {"weapon", e.droneWeapon},
                          {"power", e.power},
                          {"cooldown", e.cooldown},
                          {"speed", e.droneSpeed},
                          {"dodge", e.droneDodge}};
        }
    }
    auto allShips = nlohmann::json::object();
    for (const auto& [id, definition] : ships_) {
        allShips[id] = definition.snapshot();
    }
    auto races = nlohmann::json::object();
    for (const auto& [id, definition] : appearances_) {
        auto powers = nlohmann::json::array();
        for (const auto& power : definition.powers) {
            powers.push_back(textState(power));
        }
        races[id] = {{"source", definition.source},
                     {"title", textState(definition.title)},
                     {"short_title", textState(definition.shortTitle)},
                     {"description", textState(definition.description)},
                     {"cost", definition.cost},
                     {"rarity", definition.rarity},
                     {"powers", std::move(powers)},
                     {"color_layers", definition.layers}};
    }
    auto augments = nlohmann::json::object();
    for (const auto& [id, definition] : equipment_) {
        if (definition.kind == "aug") {
            augments[id] = {{"source", definition.source},
                            {"value", definition.value},
                            {"cost", definition.cost},
                            {"rarity", definition.rarity},
                            {"locked", definition.locked},
                            {"stacking", definition.stacking}};
        }
    }
    auto systemDefinitions = nlohmann::json::object();
    for (const auto& [id, definition] : systems_) {
        systemDefinitions[id] = {{"cost", definition.cost},
                                 {"start_power", definition.startPower},
                                 {"max_power", definition.maxPower},
                                 {"upgrade_costs", definition.costs},
                                 {"title", textState(definition.title)},
                                 {"description", textState(definition.description)}};
    }
    auto itemDefinitions = nlohmann::json::object();
    for (const auto& [id, definition] : items_) {
        itemDefinitions[id] = {{"source", definition.source},
                               {"type", definition.type},
                               {"cost", definition.cost},
                               {"rarity", definition.rarity},
                               {"bp", definition.bp},
                               {"title", textState(definition.title)},
                               {"short_title", textState(definition.shortTitle)},
                               {"description", textState(definition.description)},
                               {"tooltip", textState(definition.tooltip)},
                               {"tip", textState(definition.tip)}};
    }
    auto rewardPools = nlohmann::json::object();
    auto baseRewardPools = nlohmann::json::object();
    for (const std::string_view kind : {"weapon", "drone", "aug"}) {
        auto rows = nlohmann::json::array();
        for (const auto& [id, weight] : rewardEquipmentPool(kind)) {
            rows.push_back({{"id", id}, {"weight", weight}, {"stacking", equipment(id).stacking}});
        }
        rewardPools[std::string(kind)] = std::move(rows);
        auto baseRows = nlohmann::json::array();
        for (const auto& [id, weight] : rewardEquipmentPool(kind, false)) {
            baseRows.push_back(
                {{"id", id}, {"weight", weight}, {"stacking", equipment(id).stacking}});
        }
        baseRewardPools[std::string(kind)] = std::move(baseRows);
    }
    auto crewPools = nlohmann::json::object();
    for (const bool advanced : {false, true}) {
        auto rows = nlohmann::json::array();
        for (const auto& [id, weight] : crewRewardPool(advanced)) {
            rows.push_back({{"id", id}, {"weight", weight}, {"cost", appearance(id).cost}});
        }
        crewPools[advanced ? "advanced" : "base"] = std::move(rows);
    }
    return {{"schema_version", 1},
            {"ships", out},
            {"ship_availability", std::move(availability)},
            {"all_ship_definitions", std::move(allShips)},
            {"crew_definitions", std::move(races)},
            {"augment_definitions", std::move(augments)},
            {"system_definitions", std::move(systemDefinitions)},
            {"item_definitions", std::move(itemDefinitions)},
            {"weapon_art", art},
            {"drones", drones},
            {"reward_pools", std::move(rewardPools)},
            {"base_reward_pools", std::move(baseRewardPools)},
            {"crew_reward_pools", std::move(crewPools)},
            {"equipment_count", equipment_.size()},
            {"scope", "resource definitions; not active game state"}};
}
} // namespace ftl
