#include "run.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>

// Export of the live run as the original game's continue.sav. The field
// mapping is documented in docs/research/original-save-format/.
namespace ftl {
namespace {
std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("original_save_read_failed");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
        throw std::runtime_error("original_save_write_failed");
    }
}

std::string timestamp() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d-%H%M%S}", now);
}

original_save::Milli milli(float value) {
    return original_save::Milli::from(value);
}

original_save::MilliPoint milliPoint(SDL_FPoint point) {
    return {milli(point.x), milli(point.y)};
}

// -FLT_MAX "unset" points: cvttss2si of the scaled value yields INT_MIN.
constexpr original_save::MilliPoint unsetPoint{{std::numeric_limits<std::int32_t>::min()},
                                               {std::numeric_limits<std::int32_t>::min()}};

// One Animation::SaveState record for a strip that has played `elapsed` seconds.
original_save::Animation
strip(const AnimationFrame& animation, float elapsed, bool running, bool loop) {
    original_save::Animation saved{running ? 1 : 0, loop ? 1 : 0, 0, {}, {1000}, {}, {}};
    if (!running || !animation.usable()) {
        return saved;
    }
    const float time = loop ? std::fmod(elapsed, animation.duration) : elapsed;
    const float progress = std::clamp(time / animation.duration, 0.f, 1.f);
    saved.currentFrame =
        std::clamp(int(progress * float(animation.frames)), 0, animation.frames - 1);
    saved.progress = milli(progress);
    return saved;
}

// Globals::GetDirection 0x5a8b00.
int direction(std::string name) {
    std::ranges::transform(name, name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return name == "down" ? 0 : name == "right" ? 1 : name == "up" ? 2 : name == "left" ? 3 : 4;
}

// ShipSystem ctor 0x61bab0 bBoostable: the systems whose room gets a primary slot.
bool primarySystem(int id, bool enemy) {
    return id == 0 || id == 1 || id == 3 || id == 6 || id == 7 || id == 8 || (enemy && id == 11);
}

std::vector<RoomDefinition> roomsById(const ShipDefinition& ship) {
    auto rooms = ship.rooms;
    std::ranges::sort(rooms, {}, &RoomDefinition::id);
    return rooms;
}
} // namespace

original_save::Context Run::originalSaveContext() const {
    original_save::Context context;
    context.layout = [this](const std::string& blueprint) {
        const auto& ship = catalog_.ship(blueprint);
        original_save::ShipLayout layout;
        // Ship::Import 0x486500 indexes vRoomList by room id.
        auto rooms = ship.rooms;
        std::ranges::sort(rooms, {}, &RoomDefinition::id);
        for (std::size_t index = 0; index < rooms.size(); ++index) {
            if (rooms[index].id != static_cast<int>(index)) {
                throw std::runtime_error("original_save_room_ids_not_dense");
            }
            layout.roomSquares.push_back((rooms[index].w / 35) * (rooms[index].h / 35));
        }
        // Ship::Import: a DOOR whose second room is -1 is an outer airlock.
        for (const auto& door : ship.doors) {
            ++(door.roomB == -1 ? layout.airlocks : layout.doors);
        }
        layout.artillery = static_cast<int>(std::ranges::count_if(ship.systems, [](const auto& s) {
            return s.id == 11 && s.installed;
        }));
        return layout;
    };
    context.droneType = [this](const std::string& drone) {
        return catalog_.equipment(drone).type;
    };
    return context;
}

// One ExportShip crew record (ship.md section 2). `owned`: the player owns the
// member (CrewMember::iShipId == 0); `aboardEnemy`: it stands on the enemy ship.
original_save::Crew Run::originalCrew(const RunCrew& member, bool owned, bool aboardEnemy) const {
    original_save::Crew crew;
    const bool away = owned && member.intruder;
    crew.name = member.appearance.fullName;
    crew.species = member.appearance.species;
    crew.drone = 0;
    // CrewMember::GetIntegerHealth 0x471b20.
    crew.health = member.dead ? 0 : std::max(1, static_cast<int>(member.health));
    // GetLocation 0x471140 rounds; currentSlot is the reserved destination.
    crew.x = static_cast<int>(std::round(away ? member.boardX : member.x));
    crew.y = static_cast<int>(std::round(away ? member.boardY : member.y));
    crew.room = away ? member.boardTargetRoom : member.targetRoom;
    crew.slot = away ? member.boardTargetSlot : member.targetSlot;
    (void)aboardEnemy;
    crew.playerControlled = owned ? 1 : 0;
    crew.cloneReady = member.cloneReady ? 1 : 0;
    crew.deathNumber = member.deathNumber == 0 ? -1 : static_cast<int>(member.deathNumber);
    // blueprint.colorChoices: one entry per colour layer, unpacked from the
    // native mixed-radix appearance (hangar.cpp; human adds the sex digit).
    int rest = member.appearance.appearance / (crew.species == "human" ? 2 : 1);
    for (const auto& layer : catalog_.appearance(crew.species).layers) {
        if (layer.empty()) {
            crew.colors.push_back(0);
            continue;
        }
        crew.colors.push_back(rest % static_cast<int>(layer.size()));
        rest /= static_cast<int>(layer.size());
    }
    crew.mindControlled = member.controlled ? 1 : 0;
    if (owned && !away) {
        crew.savedSlot = member.savedSlot;
        crew.savedRoom = member.savedRoom;
    }
    crew.skills = {member.manningSkillProgress[0],
                   member.manningSkillProgress[1],
                   member.manningSkillProgress[2],
                   member.weaponSkillProgress,
                   member.repairSkillProgress,
                   member.combatSkillProgress};
    crew.male = member.recordMale ? 1 : 0;
    int masteries = 0;
    for (const int tiers : member.earnedSkillTiers) {
        masteries += (tiers & 1) + ((tiers >> 1) & 1);
    }
    crew.stats = {member.statistics[0],
                  member.statistics[1],
                  member.statistics[2],
                  member.statistics[3],
                  masteries};
    auto& state = crew.state;
    state.stunTime = milli(member.stunTime);
    state.healthBoost = static_cast<int>(member.healthBoost);
    state.deathNumber = crew.deathNumber;
    if (member.controlled) {
        // Run::advanceCrewCombat's damage boost (MindSystem DAMAGE_BOOST 0x837d50).
        constexpr std::array<float, 4> boost{1.f, 1.f, 1.25f, 2.f};
        const auto& systems = owned ? enemy_.systems : systems_;
        const auto mind = std::ranges::find_if(systems, [](const auto& entry) {
            return entry.definition.id == 14;
        });
        const int level = mind == systems.end() ? 0
                          : mind->hackActive && mind->hackLevel > 1
                              ? mind->health
                              : std::min(mind->power, mind->health);
        state.mindDamageBoost = milli(boost[std::size_t(std::clamp(level, 0, 3))]);
    }
    state.cloneDying = 0;
    state.deathCount = static_cast<int>(crewDeathCount_);
    for (std::size_t skill = 0; skill < state.skillsEarned.size(); ++skill) {
        state.skillsEarned[skill] = {member.earnedSkillTiers[skill] & 1,
                                     (member.earnedSkillTiers[skill] >> 1) & 1};
    }
    // A pending teleport cannot be carried by ExportShip; the member is idle.
    if (crew.species == "crystal") {
        crew.crystal.powerCooldownFirst = milli(owned ? member.crystalPowerElapsed : 10.f);
        crew.crystal.powerCooldownSecond = milli(50.f);
        crew.crystal.cloned = member.cloned ? 1 : 0;
    }
    return crew;
}

// ShipManager::ExportShip 0x4a4680 for the player (false) or nearby (true) ship.
original_save::Ship Run::originalShip(bool enemy) const {
    original_save::Ship ship;
    const auto& definition = enemy ? enemy_.ship : ship_;
    const auto& systems = enemy ? enemy_.systems : systems_;
    const auto system = [&](int id) -> const RunSystem* {
        const auto found = std::ranges::find_if(systems, [id](const auto& entry) {
            return entry.definition.id == id;
        });
        return found == systems.end() ? nullptr : &*found;
    };
    ship.blueprintName = definition.id;
    // ExportShip writes myBlueprint.name, resolved in the current language.
    ship.shipName = !enemy                      ? name_
                    : definition.name.reference ? texts_.get(definition.name.value, locale_)
                                                : definition.name.value;
    ship.imageFile = definition.image;
    ship.hostile = enemy ? (hostile_ ? 1 : 0) : 1;
    ship.jumpTimer = milli(enemy ? 0.f : ftlCharge_);
    ship.jumping = enemy && enemy_.escapeJumpElapsed >= 0 ? 1 : 0;
    ship.jumpAnimationTime = milli(enemy ? std::max(0.f, enemy_.escapeJumpElapsed) : 0.f);
    ship.hull = enemy ? enemy_.hull : hull_;
    // The enemy ShipManager keeps its constructor fuel and has no scrap reader.
    ship.fuel = enemy ? 16 : fuel_;
    ship.droneCount = enemy ? enemy_.droneParts : parts_;
    ship.missileCount = enemy ? enemy_.missiles : missiles_;
    ship.scrap = enemy ? 0 : scrap_;

    // Crew physically aboard this ship, in CrewMemberFactory order (ship.md 2.1).
    std::vector<std::pair<std::uint64_t, original_save::Crew>> aboard;
    const auto listed = [](const RunCrew& member) {
        return !(member.dying || member.dead || member.outOfGame) || member.cloneReady;
    };
    for (const auto& member : crew_) {
        if (member.intruder == enemy && listed(member)) {
            aboard.emplace_back(member.physicalCrewOrder, originalCrew(member, true, enemy));
        }
    }
    if (enemy) {
        for (std::size_t index = 0; index < enemy_.crew.size(); ++index) {
            const auto& member = enemy_.crew[index];
            const bool boarding = std::ranges::any_of(voyageBoarders_, [&](const RunCrew& other) {
                return other.enemyCrewOrigin == int(index) && !other.outOfGame;
            });
            if (!boarding && listed(member)) {
                aboard.emplace_back(member.physicalCrewOrder, originalCrew(member, false, true));
            }
        }
    } else {
        for (const auto& member : voyageBoarders_) {
            if (listed(member)) {
                aboard.emplace_back(member.physicalCrewOrder, originalCrew(member, false, false));
            }
        }
    }
    std::ranges::stable_sort(aboard, {}, &decltype(aboard)::value_type::first);
    for (auto& entry : aboard) {
        ship.crew.push_back(std::move(entry.second));
    }
    // ShipBlueprint::customCrew is discarded after OnInit on load (ship.md C1);
    // the player's own roster stands in for the hangar list.
    for (const auto& member : ship.crew) {
        if (member.playerControlled == (enemy ? 0 : 1)) {
            ship.customCrew.emplace_back(member.species, member.name);
        }
    }

    ship.reactorPower = definition.reactor;
    for (const auto& entry : systems) {
        const auto id = static_cast<std::size_t>(entry.definition.id);
        if (id >= ship.systems.size()) {
            throw std::runtime_error("original_save_unknown_system");
        }
        original_save::SystemRecord record;
        record.capacity = entry.definition.power;
        // powerState.first holds reactor bars only; battery and zoltan bars are separate.
        record.power = std::max(0, entry.power - entry.batteryPower - entry.bonusPower);
        record.damage = entry.definition.power - entry.health;
        record.lockCount = entry.lockCount;
        record.lockTimerX5000 = static_cast<int>(5000.0f * entry.lockElapsed);
        record.repairOverTime = static_cast<int>(entry.repair);
        record.damageOverTime = static_cast<int>(entry.fireDamage);
        record.batteryPower = entry.batteryPower;
        record.hackLevel = entry.hackActive ? entry.hackLevel : 0;
        record.hacked = entry.hackLevel > 0 && entry.hackActive ? 1 : 0;
        record.tempPowerCap = entry.powerLimit > 7 ? 1000 : entry.powerLimit;
        record.tempPowerLoss = entry.powerLoss;
        record.tempDividePower = entry.powerDivide;
        ship.systems[id].push_back(record);
    }
    if (system(13)) {
        ship.clone.timeToClone = milli(enemy ? enemy_.cloneTimer : cloneTimer_);
        ship.clone.timeGoal = milli(enemy ? enemy_.cloneDuration : cloneDuration_);
        ship.clone.deathTime = milli(enemy ? enemy_.cloneDeathTimer : cloneDeathTimer_);
    }
    if (system(12)) {
        const auto& battery = enemy ? enemy_.battery : battery_;
        ship.battery.turnedOn = battery.active ? 1 : 0;
        for (const auto& entry : systems) {
            ship.battery.batteryPower += entry.batteryPower;
        }
        ship.battery.timerX1000 =
            battery.active && battery.duration > 0
                ? static_cast<int>(1000.0f * (battery.elapsed / battery.duration))
                : 0;
    }
    if (system(0)) {
        auto& shields = ship.shields;
        shields.power = enemy ? enemy_.shields : shieldBubbles_;
        shields.superFirst = enemy ? enemy_.superShields : superShields_;
        shields.superSecond =
            enemy ? enemy_.maxSuperShields
            : std::ranges::find(ship_.augments, "ENERGY_SHIELD") != ship_.augments.end() ? 5
                                                                                         : 0;
        // Native charges 0..5 at 2.5 units a second; Shields::charger runs to 2.0 s.
        shields.charger = milli((enemy ? enemy_.shieldCharge : shieldCharge_) / 2.5f);
        shields.upRunning = shields.power > 0 ? 1 : 0;
        shields.upProgress = milli(shields.power > 0 ? 1.f : 0.f);
    }
    if (system(10)) {
        const auto& cloak = enemy ? enemy_.cloak : cloak_;
        ship.cloak.turnedOn = cloak.active ? 1 : 0;
        ship.cloak.timerRunning = cloak.active ? 1 : 0;
        ship.cloak.currentGoal = milli(cloak.active ? cloak.duration : .5f);
        ship.cloak.currentTime = milli(cloak.active ? cloak.elapsed : 0.f);
    }

    const auto& oxygen = enemy ? enemy_.roomOxygen : roomOxygen_;
    const auto& hazards = enemy ? enemyHazards_ : playerHazards_;
    const bool oxygenSystem = system(2) != nullptr;
    const auto rooms = roomsById(definition);
    for (const auto& room : rooms) {
        original_save::Room saved;
        if (oxygenSystem) {
            const auto level = oxygen.find(room.id);
            saved.oxygen = level == oxygen.end() ? 100 : static_cast<int>(level->second);
        } else {
            saved.oxygen = 0;
        }
        const int columns = room.w / 35;
        const int rows = room.h / 35;
        const auto hazard = hazards.find(room.id);
        // Spreader<Fire>::GetSpreadables 0x735950 walks the room column by column.
        for (int column = 0; column < columns; ++column) {
            for (int row = 0; row < rows; ++row) {
                const int tile = row * columns + column;
                original_save::Fire fire;
                if (hazard != hazards.end()) {
                    const auto& fires = hazard->second;
                    const auto burning = std::ranges::find(fires.fireTiles, tile);
                    if (burning != fires.fireTiles.end()) {
                        const auto index = std::size_t(burning - fires.fireTiles.begin());
                        fire.damage = index < fires.fireHealth.size()
                                          ? static_cast<int>(fires.fireHealth[index])
                                          : 100;
                        fire.death = index < fires.fireDeathTimers.size()
                                         ? static_cast<int>(fires.fireDeathTimers[index])
                                         : -1;
                    }
                    const auto start = fires.fireStartTimers.find(tile);
                    if (start != fires.fireStartTimers.end()) {
                        fire.start = static_cast<int>(start->second);
                    }
                }
                saved.fires.push_back(fire);
            }
        }
        for (const auto& entry : systems) {
            if (entry.definition.room == room.id && primarySystem(entry.definition.id, enemy) &&
                entry.definition.slot >= 0) {
                saved.primarySlot = entry.definition.slot;
                saved.primaryDirection = direction(entry.definition.direction);
            }
        }
        ship.rooms.push_back(std::move(saved));
    }
    // Ship::GetHullBreaches 0x483a50: damaged OuterHulls, rooms in id order, column-major.
    for (const auto& room : rooms) {
        const auto hazard = hazards.find(room.id);
        if (hazard == hazards.end()) {
            continue;
        }
        const int columns = room.w / 35;
        for (int column = 0; column < columns; ++column) {
            for (int row = 0; row < room.h / 35; ++row) {
                const auto& tiles = hazard->second.breachTiles;
                const auto found = std::ranges::find(tiles, row * columns + column);
                if (found == tiles.end()) {
                    continue;
                }
                const auto index = std::size_t(found - tiles.begin());
                const float health = index < hazard->second.breachHealth.size()
                                         ? hazard->second.breachHealth[index]
                                         : 100.f;
                if (health > 0) {
                    ship.breaches.push_back(
                        {room.x / 35 + column, room.y / 35 + row, static_cast<int>(health)});
                }
            }
        }
    }
    // Ship::SaveState 0x483930: inner doors, then outer airlocks, layout order.
    const auto& open = enemy ? enemy_.doors : doors_;
    const auto& fake = enemy ? enemy_.fakeDoors : fakeDoors_;
    const auto& baseHealth = enemy ? enemy_.doorBaseHealth : voyageDoorBaseHealth_;
    const auto& doorDamage = enemy ? enemy_.doorDamage : voyageDoorDamage_;
    const auto& lastBase = enemy ? enemy_.doorLastBaseHealth : voyageDoorLastBaseHealth_;
    const auto& levels = enemy ? enemy_.doorLevel : voyageDoorLevel_;
    for (const bool airlocks : {false, true}) {
        for (const auto& door : definition.doors) {
            if ((door.roomB == -1) != airlocks) {
                continue;
            }
            original_save::Door saved;
            const auto base = baseHealth.find(door.id);
            saved.baseHealth = base == baseHealth.end() || base->second <= 0 ? 4 : base->second;
            const auto damage = doorDamage.find(door.id);
            saved.health = saved.baseHealth -
                           (damage == doorDamage.end() ? 0 : static_cast<int>(damage->second));
            const auto last = lastBase.find(door.id);
            saved.lastBase = last == lastBase.end() ? saved.baseHealth : last->second;
            const auto id = std::size_t(door.id);
            saved.open = id < open.size() && open[id] ? 1 : 0;
            saved.fakeOpen = id < fake.size() && fake[id] ? 1 : 0;
            saved.hacked = doorHackLevel(door, enemy);
            const auto level = levels.find(door.id);
            saved.blast =
                saved.hacked > 0 ? 2 : std::max(0, (level == levels.end() ? 0 : level->second) - 1);
            (airlocks ? ship.airlocks : ship.doors).push_back(saved);
        }
    }
    ship.cloakingTracker = milli((enemy ? enemy_.cloak : cloak_).active ? .5f : 0.f);

    for (std::size_t index = 0; index < definition.weapons.size(); ++index) {
        const auto& powered = enemy ? enemy_.powered : weaponPower_;
        ship.weapons.push_back(
            {definition.weapons[index], index < powered.size() && powered[index] ? 1 : 0});
    }
    for (const auto& drone : enemy ? enemy_.drones : combatDrones_) {
        // Flagship surge drones live only in ShipManager::superDrones.
        if (drone.bossSurge) {
            continue;
        }
        original_save::Drone saved;
        saved.name = drone.blueprint;
        saved.powered = drone.powered ? 1 : 0;
        saved.deployed = drone.deployed ? 1 : 0;
        const auto& type = catalog_.equipment(drone.blueprint).type;
        if (type == "REPAIR" || type == "BATTLE") {
            // CrewDrone::GetWorldLocation 0x4375a0 truncates; GetDroneSlot 0x73de10.
            saved.x = static_cast<int>(drone.crewPosition.x);
            saved.y = static_cast<int>(drone.crewPosition.y);
            saved.room = drone.targetRoom >= 0 ? drone.targetRoom : drone.crewRoom;
            saved.slot = drone.crewSlot;
            saved.health = static_cast<int>(drone.health);
        } else {
            saved.x = saved.y = saved.room = saved.slot = -1;
            saved.health = 1;
        }
        ship.drones.push_back(saved);
    }
    // ShipInfo::augList is a std::map: ascending names, duplicates repeated.
    ship.augments = definition.augments;
    std::ranges::sort(ship.augments);
    if (!system(3)) {
        ship.weapons.clear();
    }
    if (!system(4)) {
        ship.drones.clear();
    }
    return ship;
}

std::string Run::originalEventName(const std::string& key) const {
    if (!key.starts_with("_native")) {
        return key;
    }
    // EventsParser names anonymous events with a shared parse counter
    // (event-names.md); the catalog records that name beside the native key.
    const auto name = events_.event(key).attribute("_originalName");
    if (!name) {
        throw std::runtime_error("no original name for inline event " + key);
    }
    return name.value();
}

std::string Run::originalShipEventName(const std::string& key) const {
    if (!key.starts_with("_native")) {
        return key;
    }
    const auto name = events_.shipEvent(key).attribute("_originalName");
    if (!name) {
        throw std::runtime_error("no original name for inline ship " + key);
    }
    return name.value();
}

// One in-flight or queued projectile (battle.md section 3).
original_save::Projectile Run::originalProjectile(const CombatProjectile& p, int selfId) const {
    original_save::Projectile saved;
    // ENV_ASTEROID is the catalog's synthetic Asteroid::Asteroid 0x453290 payload.
    const EquipmentDefinition* blueprint =
        p.weapon == "ENV_ASTEROID" || catalog_.containsEquipment(p.weapon)
            ? &catalog_.equipment(p.weapon)
            : nullptr;
    const std::string type = p.weapon == "PDS_SHOT"       ? "PDS"
                             : p.weapon == "ENV_ASTEROID" ? "ASTEROID"
                             : blueprint                  ? blueprint->type
                                                          : "";
    const bool beam = type == "BEAM";
    saved.tag = type == "LASER" || type == "BURST" ? 1
                : type == "ASTEROID"               ? 2
                : type == "MISSILES"               ? 3
                : type == "BOMB"                   ? 4
                : beam                             ? 5
                : type == "PDS"                    ? 6
                                                   : 0;
    if (saved.tag == 0) {
        return saved;
    }
    // A beam's base position is its emission point; target is the moving head.
    saved.position = milliPoint(beam ? p.beamOrigin : p.position);
    saved.lastPosition = milliPoint(beam ? p.beamOrigin : p.previous);
    saved.speedMagnitude = milli(p.speed / 16.f);
    saved.target = milliPoint(beam ? p.position : p.target);
    constexpr float degrees = 180.f / std::numbers::pi_v<float>;
    saved.heading = milli(beam ? p.beamHeading
                          : p.space == p.owner && saved.tag != 6
                              ? (p.owner == 0 ? 0.f : 270.f)
                              : std::atan2(p.direction.y, p.direction.x) * degrees);
    saved.ownerId = p.owner;
    saved.selfId = selfId;
    if (blueprint && !p.fake) {
        saved.damage = blueprint->damage;
        saved.shieldPiercing = blueprint->piercing;
        saved.fireChance = blueprint->fireChance;
        saved.breachChance = blueprint->breachChance;
        saved.ionDamage = blueprint->ion;
        saved.systemDamage = blueprint->system;
        saved.crewDamage = blueprint->personnel;
        saved.hullBuster = blueprint->hullBuster ? 1 : 0;
        saved.lockdown = blueprint->lockdown ? 1 : 0;
        saved.stunChance = blueprint->stunChance;
        saved.stun = blueprint->stun;
        // ProjectileFactory::Update 0x40d6c8 adds the damage boost to the first
        // nonzero of damage, ion, crew and system damage.
        if (p.damageBonus != 0) {
            for (int* field :
                 {&saved.damage, &saved.ionDamage, &saved.crewDamage, &saved.systemDamage}) {
                if (*field != 0) {
                    *field += p.damageBonus;
                    break;
                }
            }
        }
    }
    saved.crystalShard = p.crystalShard ? 1 : 0;
    saved.damageSelfId = selfId;
    saved.destinationSpace = p.targetShip;
    saved.currentSpace = p.space;
    saved.targetId = p.targetShip;
    saved.dead = p.dead ? 1 : 0;
    saved.deathAnimationName = p.explosionImage.empty() ? "explosion_missile1" : p.explosionImage;
    saved.flightAnimationName = saved.tag == 2 ? "" : p.projectileImage;
    const auto& flight = catalog_.animation(p.projectileImage);
    if (p.flightFrame >= 0) {
        // Flak pellets: a random authored frame with the tracker stopped.
        saved.flightAnimation = {0, 1, p.flightFrame, {}, {1000}, {}, {}};
    } else {
        saved.flightAnimation = strip(flight, p.age, true, true);
    }
    if (!beam) {
        saved.speed = milliPoint({p.position.x - p.previous.x, p.position.y - p.previous.y});
    }
    saved.missed = p.missed || (p.resolved && p.intercepted) ? 1 : 0;
    saved.hitTarget = p.checkedShield && !p.missed && !p.resolved ? 1 : 0;
    if (blueprint && !p.soundsInitialized) {
        saved.hitSolidSound = blueprint->hitShipSounds.empty() ? "" : blueprint->hitShipSounds[0];
    } else {
        saved.hitSolidSound = p.hitShipSound;
    }
    saved.hitShieldSound = p.hitShieldSound;
    saved.missSound = p.missSound;
    saved.entryAngle = static_cast<int>(std::lround(p.entryAngle * degrees));
    saved.passedTarget =
        p.space == p.targetShip &&
                (p.arrived || (p.position.x == p.target.x && p.position.y == p.target.y))
            ? 1
            : 0;
    // Targetable::type: class default, then the blueprint's dronesTargetable.
    saved.targetType = saved.tag == 1 || saved.tag == 6 ? 4
                       : saved.tag == 3                 ? 1
                       : saved.tag == 2                 ? 2
                                                        : 5;
    if (p.droneTargetable != -1) {
        saved.targetType = p.droneTargetable == 0 ? 4 : 2;
    }
    switch (saved.tag) {
    case 1:
        saved.spinAngle = milli(p.spinAngle);
        saved.spinSpeed = milli(
            blueprint && blueprint->spin > 0 && !p.crystalShard ? float(blueprint->spin) : 0.f);
        break;
    case 4:
        saved.bombMissed = p.missed ? 1 : 0;
        saved.explosiveDelay = milli(p.bombDelay);
        saved.superShield = p.bombSuperShield ? 1 : 0;
        saved.superShieldBypass = p.bombBypass ? 1 : 0;
        saved.flightReverse = p.arrived ? 1 : 0;
        break;
    case 5:
        saved.subEnd = milliPoint(p.beamSubEnd);
        saved.subStart = milliPoint(p.beamSubStart);
        saved.finalEnd = milliPoint(p.position);
        saved.target2 = milliPoint(p.targetEnd);
        saved.target1 = milliPoint(p.target);
        saved.beamLifespan = milli(p.resolved ? -1.f : 1.f);
        saved.length = milli(blueprint ? float(blueprint->length) : 0.f);
        saved.drift = milli(p.beamDrift);
        // The -2147483600 "none" sentinel overflows writeFloat to INT_MIN.
        saved.lastCollision = std::abs(p.beamLastCollision.x) >= 2147483.f
                                  ? unsetPoint
                                  : milliPoint(p.beamLastCollision);
        saved.animationTimer = milli(p.beamAnimationTimer);
        saved.lastDamage = p.beamLastDamage;
        saved.startHeading = milli(p.owner == 0 ? 0.f : 270.f);
        saved.piercedShield = p.beamPierced ? 1 : 0;
        saved.oneSpace = p.targetShip == p.owner ? 1 : 0;
        break;
    case 6: {
        saved.startPoint = milliPoint(p.targetEnd);
        saved.pdsPassedTarget = p.arrived ? 1 : 0;
        const float total = std::hypot(p.target.x - p.targetEnd.x, p.target.y - p.targetEnd.y);
        const float done = std::hypot(p.position.x - p.targetEnd.x, p.position.y - p.targetEnd.y);
        saved.currentScale = milli(total > 0 ? done / total * 7.f + 1.f : 1.f);
        saved.pdsMissed = p.missed ? 1 : 0;
        saved.explosion =
            strip(catalog_.animation(p.explosionImage), p.age - p.spinAngle, p.arrived, false);
        break;
    }
    default:
        break;
    }
    return saved;
}

// SpaceDrone::SaveState 0x41db40 (battle.md section 4.2).
original_save::SpaceDroneState Run::originalSpaceDrone(const CombatDrone& drone) const {
    original_save::SpaceDroneState saved;
    const auto& type = catalog_.equipment(drone.blueprint).type;
    saved.destroyedTimer = milli(drone.rebuildTimer);
    saved.currentSpace = saved.destinationSpace = drone.space;
    saved.currentLocation = milliPoint(drone.position);
    saved.lastLocation =
        milliPoint({drone.position.x - drone.velocity.x, drone.position.y - drone.velocity.y});
    saved.destinationLocation = milliPoint(drone.destination);
    saved.pointTarget = unsetPoint;
    saved.targetLocation = drone.hasAimTarget ? milliPoint(drone.aimTarget) : unsetPoint;
    saved.targetSpeed = type == "DEFENSE" ? milliPoint(drone.defenseTargetVelocity) : unsetPoint;
    saved.pause = milli(drone.pause);
    saved.additionalPause = milli(drone.additionalPause);
    saved.weaponCooldown = milli(drone.cooldown);
    saved.currentAngle = milli(drone.orbitAngle == -1000.f ? 0.f : drone.orbitAngle);
    saved.aimingAngle = milli(drone.aimAngle);
    saved.lastAimingAngle = milli(drone.lastAimingAngle);
    saved.lifespan =
        type == "SHIP_REPAIR" || drone.bossSurge ? drone.lifespan : std::numeric_limits<int>::max();
    saved.disrupted = drone.defenseDisrupted ? 1 : 0;
    saved.poweredLastFrame = drone.poweredLastFrame ? 1 : 0;
    saved.ionStun = milli(drone.ionStun);
    saved.desiredAimingAngle = milli(drone.desiredAimAngle);
    saved.deployedLastFrame = drone.deployedLastFrame ? 1 : 0;
    saved.speedVector = milliPoint(drone.velocity);
    saved.explosion = strip(catalog_.animation("explosion1"),
                            drone.spaceExplosionElapsed,
                            drone.spaceExplosionElapsed >= 0,
                            false);
    return saved;
}

// ProjectileFactory::SaveState 0x40c900 (battle.md section 4.6).
original_save::WeaponFactory Run::originalWeapon(const CombatWeapon& weapon,
                                                 const std::string& id,
                                                 int owner,
                                                 const std::map<std::uint64_t, int>& ids) const {
    original_save::WeaponFactory saved;
    const auto& blueprint = catalog_.equipment(id);
    saved.cooldownFirst = milli(weapon.charge);
    float cooldown = weapon.cooldown;
    if (cooldown <= 0) {
        // Not yet updated by a combat frame: ProjectileFactory always holds the
        // effective cooldown (blueprint, boost, then the manning skill factor).
        constexpr std::array<float, 4> skillCooldown{1.f, .9f, .85f, .8f};
        const auto& systems = owner == 0 ? systems_ : enemy_.systems;
        const auto weaponsSystem = std::ranges::find_if(systems, [](const auto& entry) {
            return entry.definition.id == 3;
        });
        const int manning = weaponsSystem != systems.end() &&
                                    weaponsSystem->health == weaponsSystem->definition.power
                                ? std::clamp(weaponsSystem->manningLevel, 0, 3)
                                : 0;
        cooldown =
            std::max(.1f,
                     blueprint.cooldown - (blueprint.boostType == "cooldown"
                                               ? float(weapon.boostLevel) * blueprint.boostAmount
                                               : 0.f)) *
            skillCooldown[std::size_t(manning)];
    }
    saved.cooldownSecond = milli(cooldown);
    saved.boostLevel = weapon.boostLevel;
    saved.chargeLevel = weapon.chargeLevel;
    // Fire 0x40cbe0 stores the aim list reversed; Update pops from the back.
    const auto reversed = [](auto first, auto last) {
        std::vector<original_save::Point> points;
        for (auto it = last; it != first;) {
            --it;
            points.push_back({static_cast<int>(it->x), static_cast<int>(it->y)});
        }
        return points;
    };
    if (weapon.volleyRemaining > 0 &&
        std::size_t(weapon.volleyIndex) <= weapon.volleyPoints.size()) {
        saved.targets =
            reversed(weapon.volleyPoints.begin() + weapon.volleyIndex, weapon.volleyPoints.end());
    } else if (weapon.queued) {
        saved.targets = reversed(weapon.targetPoints.begin(), weapon.targetPoints.end());
    }
    const auto& last = weapon.volleyPoints.empty() ? weapon.targetPoints : weapon.volleyPoints;
    saved.lastTargets = reversed(last.begin(), last.end());
    saved.autoFiring = weapon.autofire ? 1 : 0;
    saved.fireWhenReady = weapon.queued ? 1 : 0;
    const bool aimed =
        !weapon.targetPoints.empty() || !weapon.volleyPoints.empty() || weapon.firedOnce;
    saved.targetId = !aimed ? -1 : weapon.volleySelf || weapon.targetSelf ? owner : 1 - owner;
    // WeaponAnimation::SaveState 0x456da0.
    const int frames = std::max(1, blueprint.animationFrames);
    const int charged = std::clamp(blueprint.chargedFrame, 0, frames - 1);
    const float duration =
        weaponFireDuration(blueprint) * float(frames) / float(std::max(1, frames - charged));
    saved.visual.animation = {1,
                              0,
                              weapon.animationFrame,
                              milli(duration > 0 ? weapon.animationElapsed / duration : 0.f),
                              {1000},
                              {},
                              {}};
    saved.visual.slideProgress = milli(weapon.animationSlide / .5f);
    saved.visual.firing = weapon.animationFiring ? 1 : 0;
    saved.visual.fireShot = weapon.animationFireShot ? 1 : 0;
    saved.visual.boostLevel = weapon.animationBoostLevel;
    const auto lastShot = ids.find(weapon.lastProjectileId);
    saved.lastProjectileId = lastShot == ids.end() ? -1 : lastShot->second;
    for (const auto& shot : weapon.pendingProjectiles) {
        saved.queued.push_back(originalProjectile(shot, ids.at(shot.id)));
    }
    return saved;
}

// ShipManager::ExportBattleState 0x496a60 (battle.md section 4).
original_save::BattleState Run::originalBattle(bool enemy,
                                               const std::map<std::uint64_t, int>& ids) const {
    original_save::BattleState saved;
    const auto& systems = enemy ? enemy_.systems : systems_;
    const auto system = [&](int id) -> const RunSystem* {
        const auto found = std::ranges::find_if(systems, [id](const auto& entry) {
            return entry.definition.id == id;
        });
        return found == systems.end() ? nullptr : &*found;
    };
    if (system(4)) {
        for (const auto& drone : enemy ? enemy_.drones : combatDrones_) {
            if (drone.bossSurge) {
                continue;
            }
            original_save::BattleDrone entry;
            entry.deployed = drone.deployed ? 1 : 0;
            entry.powered = drone.powered ? 1 : 0;
            entry.space = originalSpaceDrone(drone);
            entry.combat = {milliPoint(drone.lastDestination),
                            milli(drone.progressToDestination),
                            milli(drone.heading),
                            milli(drone.oldHeading)};
            entry.shieldGlow = milli(drone.shieldGlow);
            entry.boarder = {drone.deployed && !drone.delivered && drone.health <= 0 ? 1 : 0,
                             drone.delivered ? 1 : 0,
                             milliPoint(drone.basePosition),
                             static_cast<int>(drone.health),
                             static_cast<int>(drone.crewPosition.x),
                             static_cast<int>(drone.crewPosition.y),
                             drone.crewRoom,
                             drone.crewSlot};
            saved.drones.push_back(entry);
        }
    }
    if (const auto* hackingSystem = system(15)) {
        const auto& drone = enemy ? enemy_.hackingDrone : hackingDrone_;
        const auto& ability = enemy ? enemy_.hacking : hacking_;
        const int target = enemy ? enemy_.hackingTarget : hackingTarget_;
        auto& hacking = saved.hacking;
        hacking.currentSystem = target;
        hacking.dronePowered = drone.powered ? 1 : 0;
        hacking.droneDeployed = drone.deployed ? 1 : 0;
        hacking.hacking = target >= 0 && drone.deployed ? 1 : 0;
        // HACKING_EFFECT_TIMER 0x837cb8, reset to the table value every frame.
        constexpr std::array<float, 4> effect{10.f, 4.f, 7.f, 10.f};
        const int level = std::clamp(std::min(hackingSystem->power, hackingSystem->health), 0, 3);
        hacking.effectFirst = milli(ability.elapsed);
        hacking.effectSecond = milli(effect[std::size_t(level)]);
        hacking.lockedOut = ability.active ? 1 : 0;
        hacking.drone = originalSpaceDrone(drone);
        hacking.finalDestination = milliPoint(drone.delivered ? drone.destination : SDL_FPoint{});
        hacking.arrived = drone.delivered ? 1 : 0;
        hacking.finishedSetup = drone.hackingFinishedSetup ? 1 : 0;
        hacking.flying = strip(catalog_.animation("hack_drone_land"),
                               drone.hackingFlyingElapsed,
                               drone.hackingFlyingElapsed >= 0,
                               false);
        hacking.extending = strip(catalog_.animation("hack_drone_extend"),
                                  drone.hackingExtendingElapsed,
                                  drone.hackingExtendingElapsed >= 0,
                                  false);
    }
    if (const auto* mindSystem = system(14)) {
        // MIND_CONTROL_TIMER 0x837d90; idle is first == second.
        constexpr std::array<float, 4> timer{14.f, 14.f, 20.f, 28.f};
        const int level = std::clamp(mindSystem->hackActive && mindSystem->hackLevel > 1
                                         ? mindSystem->health
                                         : std::min(mindSystem->power, mindSystem->health),
                                     0,
                                     3);
        const auto& ability = enemy ? enemy_.mind : mind_;
        saved.mind.controlSecond = milli(timer[std::size_t(level)]);
        saved.mind.controlFirst =
            ability.active ? milli(ability.elapsed) : saved.mind.controlSecond;
    }
    if (system(3)) {
        const auto& weapons = enemy ? enemy_.weapons : combatWeapons_;
        const auto& names = enemy ? enemy_.ship.weapons : ship_.weapons;
        for (std::size_t index = 0; index < weapons.size() && index < names.size(); ++index) {
            saved.weapons.push_back(originalWeapon(weapons[index], names[index], enemy, ids));
        }
    }
    for (const auto& entry : systems) {
        if (entry.definition.id == 11) {
            saved.artillery.push_back(
                originalWeapon(entry.artillery, entry.definition.weapon, enemy, ids));
        }
    }
    if (enemy) {
        for (const auto& drone : enemy_.drones) {
            if (!drone.bossSurge) {
                continue;
            }
            saved.superDrones.push_back({drone.blueprint,
                                         originalSpaceDrone(drone),
                                         {milliPoint(drone.lastDestination),
                                          milli(drone.progressToDestination),
                                          milli(drone.heading),
                                          milli(drone.oldHeading)},
                                         drone.deployed ? 1 : 0,
                                         drone.health <= 0 || drone.spaceExplosionElapsed >= 0,
                                         drone.powered ? 1 : 0});
        }
    }
    return saved;
}

// Everything SaveGame writes after StarMap (voyage.md section 7, battle.md).
void Run::originalEncounter(original_save::SaveGame& save) const {
    // ShipEvent::Save: the current encounter's ShipEvent and its outcome names.
    if (!encounter_.empty()) {
        auto& event = save.shipEvent;
        event.present = 1;
        event.shipSeed = std::max(0, encounterShipSeed_);
        const auto description = events_.shipEvent(encounter_);
        const auto outcome = [&](const char* name) {
            const auto node = description.child(name);
            if (!node) {
                return std::string{};
            }
            return node.attribute("load")
                       ? std::string(node.attribute("load").value())
                       : originalEventName(node.attribute("_nativeEventTemplate").value());
        };
        event.surrender = outcome("surrender");
        event.escape = outcome("escape");
        event.destroyed = outcome("destroyed");
        event.deadCrew = outcome("deadCrew");
        event.gotAway = outcome("gotaway");
    }
    save.generatedEvent = generatedEvent_.empty() ? "" : originalEventName(generatedEvent_);
    save.lastMainTextLiteral = lastMainTextLiteral_ ? 1 : 0;
    save.lastMainText = lastMainText_;
    save.lastSelectedCrewSeed = static_cast<int>(selectedVoyageCrewSeed_);
    save.choiceHistory = choiceHistory_;

    if (enemy_.present) {
        save.enemyBoss = campaign_.fighting ? 1 : 0;
        save.enemy = originalShip(true);
        auto& ai = save.enemyAi;
        ai.surrendered = surrenderOffered_ ? 1 : 0;
        ai.escaping = enemy_.escaping ? 1 : 0;
        ai.destroyed = 0;
        ai.surrenderThreshold = enemy_.surrenderThreshold;
        ai.escapeThreshold = enemy_.escapeThreshold;
        ai.escapeTimer = milli(enemy_.escapeTimer);
        ai.stalemateTriggered = enemy_.stalemateTrigger ? 1 : 0;
        ai.stalemateTimer = milli(enemy_.stalemateTimer);
        ai.timesTeleported = enemy_.teleportWaves;
        // ShipAI::OnInit 0x600ac0: crew minus the boarding party size.
        const int start = enemy_.teleportStartingCrew != 0 ? enemy_.teleportStartingCrew
                                                           : static_cast<int>(enemy_.crew.size());
        const int send = enemy_.boardingAi == 2   ? 3 * start / 4
                         : enemy_.boardingAi == 1 ? start / 2
                                                  : 0;
        ai.crewNeeded = start - send;
    }

    // SpaceManager::SaveSpace 0x4166a0.
    auto& space = save.space;
    space.sunLevel = environment_.fireActive ? 1 : 0;
    space.pulsarLevel = environment_.pulsarActive ? 1 : 0;
    space.pds = environment_.pdsActive ? 1 : 0;
    space.environmentTarget = environment_.pdsActive ? environment_.pdsTarget : 2;
    if (environment_.asteroidActive) {
        // LoadSpace 0x41b5e0 reads this as AsteroidGenerator::numberOfShips.
        space.asteroids = original_save::AsteroidState{
            enemy_.present ? 2 : 1,
            static_cast<int>(1000.0f * (float(environment_.asteroidStateTicks) / 60.f)),
            environment_.asteroidState,
            static_cast<int>(1000.0f * (float(environment_.asteroidSpawnTicks) / 60.f)),
            environment_.asteroidTarget};
    }
    space.flashX1000 = environment_.flashAnimationTime >= 0
                           ? static_cast<int>(1000.0f * environment_.flashAnimationTime)
                           : 0;
    space.flashTimerX1000 = static_cast<int>(1000.0f * environment_.flashTime);
    space.randomPdsTimer = milli(environment_.pdsAmbient);
    // Dense ids stand in for Globals::GetNextSpaceId; only uniqueness matters.
    std::map<std::uint64_t, int> ids;
    const auto assign = [&](const CombatProjectile& shot) {
        ids.try_emplace(shot.id, 100 + static_cast<int>(ids.size()));
    };
    for (const auto& shot : projectiles_) {
        assign(shot);
    }
    for (const auto* weapons : {&combatWeapons_, &enemy_.weapons}) {
        for (const auto& weapon : *weapons) {
            for (const auto& shot : weapon.pendingProjectiles) {
                assign(shot);
            }
        }
    }
    for (const auto* systems : {&systems_, &enemy_.systems}) {
        for (const auto& entry : *systems) {
            for (const auto& shot : entry.artillery.pendingProjectiles) {
                assign(shot);
            }
        }
    }
    for (const auto& shot : projectiles_) {
        // A resolved shot only plays its impact strip; the original keeps it but
        // it carries no gameplay state.
        if (!shot.resolved) {
            space.projectiles.push_back(originalProjectile(shot, ids.at(shot.id)));
        }
    }

    save.playerBattle = originalBattle(false, ids);
    if (save.enemy) {
        save.enemyBattle = originalBattle(true, ids);
    }
    // CompleteShip::teleTargetRoom: the room a leaving party is heading for.
    const auto leavingRoom = [](const std::vector<RunCrew>& members, int kind) {
        for (const auto& member : members) {
            if (member.teleportLeaveKind == kind && member.teleportLeaveRoom >= 0) {
                return member.teleportLeaveRoom;
            }
        }
        return -1;
    };
    const int playerRoom = leavingRoom(crew_, 1);
    save.playerTeleportTargetRoom = playerRoom >= 0        ? playerRoom
                                    : teleportTarget_ >= 0 ? teleportTarget_
                                                           : 0;
    const int enemyRoom = leavingRoom(enemy_.crew, 3);
    save.enemyTeleportTargetRoom = enemyRoom >= 0 ? enemyRoom : 0;
    save.combatControl = autofire_ ? 1 : 0;
    // BossShip::SaveBoss 0x405e40.
    save.boss.currentStage = campaign_.stage;
    save.boss.nextStage = campaign_.nextStage;
    save.boss.powerGoal = milli(campaign_.surgeGoal > 0 ? campaign_.surgeGoal : 30.f);
    save.boss.powerTime = milli(campaign_.surgeElapsed);
    const auto counts = std::min<std::size_t>(campaign_.crewCounts.size(), 20);
    save.boss.crewCounts.assign(campaign_.crewCounts.begin(),
                                campaign_.crewCounts.begin() + std::ptrdiff_t(counts));
}

// StarMap::SaveGame 0x5c7f30 (voyage.md section 5) and Store::SaveStore 0x4b3590.
void Run::originalStarMap(original_save::SaveGame& save) const {
    auto& map = save.starMap;
    if (sectorMapSeed_ < 0) {
        throw std::runtime_error("this run predates sector-seed tracking; start a new run");
    }
    map.sectorMapSeed = static_cast<int>(sectorMapSeed_);
    map.currentSectorSeed = static_cast<int>(graph_.seed());
    // The waiting animation saves its start: a wait in progress is rolled back.
    map.dangerX = distressWaitTicks_ > 0 ? graph_.waitStart() : graph_.dangerX();
    map.dangerY = graph_.dangerY();
    map.pursuitDelay = graph_.pursuitDelay();
    map.currentLocation = locationBeacon_;
    map.waitedLast = waitedLast_ ? 1 : 0;
    // StarMap ctor: -1 until SetupNoFuel draws one; it survives later jumps.
    map.fuelEventSeed = fuelEventSeed_ != 0 ? static_cast<int>(fuelEventSeed_) : -1;
    map.lastEscapeEvent = lastEscapeEvent_;
    map.mapRevealed = mapRevealed_ ? 1 : 0;
    map.bossLevel = campaign_.active ? 1 : 0;
    map.bossLocation = campaign_.active ? campaign_.position : 0;
    map.bossJumping = campaign_.active && campaign_.jumping ? 1 : 0;
    map.reversedPath = campaign_.active && campaign_.reversed ? 1 : 0;
    map.arrivedAtBase = campaign_.active ? campaign_.baseTurns : 0;
    // SaveGame first clears the current sector's neighbours (hover highlight).
    const auto& nodes = sectorGraph_.nodes();
    const auto& neighbours = sectorGraph_.current().neighbors;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const bool neighbour = std::ranges::find(neighbours, int(index)) != neighbours.end();
        map.sectorsVisited.push_back(nodes[index].visited && !neighbour ? 1 : 0);
    }
    map.worldLevel = sector_ - 1;
    map.secretSector = secretSector_ ? 1 : 0;

    const auto image = [](const std::string& path) {
        // Location images are stored relative to img/; "NONE" means no image.
        if (path.empty()) {
            return std::string("NONE");
        }
        return path.starts_with("img/") ? path.substr(4) : path;
    };
    const auto& beacons = graph_.beacons();
    const int nextMove = graph_.nextFleetMove();
    const auto inCircle = [](float dx, float dy) {
        // Pointf::RelativeDistance: squared distance, against 767^2.
        return dx * dx + dy * dy < 588289.0f;
    };
    for (std::size_t index = 0; index < beacons.size(); ++index) {
        const auto& beacon = beacons[index];
        original_save::Location location;
        location.visited = beacon.visited;
        if (beacon.visited) {
            location.spaceImage = image(beacon.backgroundImage);
            location.planetImage = image(beacon.planetImage);
            location.planetX = beacon.planetX;
            location.planetY = beacon.planetY;
            location.planetRotation = beacon.planetRotation;
        }
        location.known = beacon.known ? 1 : 0;
        const bool current = int(index) == locationBeacon_;
        if (beacon.visited && current && enemy_.present && hostile_ && !encounter_.empty()) {
            location.ship = original_save::HostileShip{
                originalShipEventName(encounter_), encounterAutoBlueprint_, encounterShipSeed_};
        } else if (beacon.visited && !beacon.savedShip.is_null()) {
            const auto& saved = beacon.savedShip;
            const auto encounter = saved.at("encounter").get<std::string>();
            std::string autoBlueprint = saved.value("auto_blueprint", std::string{});
            if (autoBlueprint.empty()) {
                const auto description = events_.shipEvent(encounter);
                autoBlueprint = description.attribute("auto_blueprint")
                                    .as_string(description.attribute("blueprint").as_string());
            }
            location.ship = original_save::HostileShip{
                originalShipEventName(encounter), autoBlueprint, saved.value("ship_seed", 0)};
        }
        const float dx = float(beacon.x) - float(map.dangerX);
        const float dy = float(beacon.y) - float(map.dangerY);
        if (campaign_.active) {
            location.fleet = beacon.fleet ? 1 : 0;
            location.fleetChanging =
                std::ranges::find(campaign_.changing, int(index)) != campaign_.changing.end();
        } else {
            location.fleet = inCircle(dx, dy) ? 1 : 0;
            // RenderLabels 0x5c91b2 marks beacons inside the next danger circle.
            location.fleetChanging = !location.fleet && inCircle(dx - float(nextMove), dy) ? 1 : 0;
        }
        const auto store = stores_.find(int(index));
        if (store != stores_.end()) {
            const auto& offers = store->second;
            original_save::Store saved;
            for (std::size_t first = 4; first + 3 <= offers.size(); first += 3) {
                original_save::StoreSection section;
                section.type = offers[first].category;
                for (std::size_t box = 0; box < 3; ++box) {
                    const auto& offer = offers[first + box];
                    auto& entry = section.boxes[box];
                    if (offer.item == "unavailable") {
                        continue;
                    }
                    entry.count = offer.stock;
                    if (offer.item.starts_with("crew:")) {
                        entry.blueprint = offer.item.substr(5);
                    } else if (offer.item.starts_with("system:")) {
                        static constexpr std::array<const char*, 16> names{"shields",
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
                        entry.blueprint = names.at(std::size_t(std::stoi(offer.item.substr(7))));
                        // SystemStoreBox::GetExtraData 0x506250: the free drone.
                        entry.value = offer.bonus == "REPAIR"     ? 1
                                      : offer.bonus == "COMBAT_1" ? 2
                                                                  : 0;
                    } else {
                        entry.blueprint = offer.item;
                    }
                }
                saved.sections.push_back(section);
            }
            for (std::size_t resource = 0; resource < 3 && resource < offers.size(); ++resource) {
                saved.resources[resource] = offers[resource].stock;
            }
            location.store = std::move(saved);
        }
        map.locations.push_back(std::move(location));
    }
    for (const auto& [name, beacon] : addedQuests_) {
        map.addedQuests.emplace_back(originalEventName(name), beacon);
    }
    map.delayedQuests = pendingQuests_;
}

original_save::SaveGame Run::originalSave() const {
    original_save::SaveGame save;
    // Header (WorldManager::SaveGame 0x5f5410). random32 is the native RandomStream.
    save.useSystemRandom = 0;
    save.dlc = advanced_ ? 1 : 0;
    // ScoreKeeper::SaveGame 0x43e5a0; see voyage.md section 2.
    auto& score = save.score;
    score.difficulty = difficulty_;
    score.stats = {scoreShips_, scoreBeacons_, scoreScrap_, scoreCrew_};
    score.shipName = name_;
    score.blueprintName = ship_.id;
    score.sector = sector_;
    score.flag = 0; // victory: a won run is never saved
    // AchievementTracker::gameFlags: only the keys the original sets, in std::map order.
    static constexpr std::array originalFlags{
        "blue_alien",      "dead_crew",   "destroyed_rock", "env_danger",   "fired_shot",
        "higho2",          "killed_crew", "lost_crew",      "nebula",       "offensive_drone",
        "reactor_upgrade", "ruwen",       "store_purchase", "store_repair", "suffocated_crew",
        "system_upgrade",  "teleported",  "used_drone",     "used_missile", "weapon_upgrade"};
    for (const auto& [key, amount] : progressionRun_.flags) {
        if (amount > 0 && std::ranges::find(originalFlags, key) != originalFlags.end()) {
            score.flags.emplace_back(key, amount);
        }
    }
    save.player = originalShip(false);
    // CommandGui::ExportData: cargo boxes, then the over-capacity boxes.
    save.cargo = cargo_;
    if (!overflowEquipment_.empty()) {
        save.cargo.push_back(overflowEquipment_);
    }
    if (!overflowAugment_.empty()) {
        save.cargo.push_back(overflowAugment_);
    }
    originalStarMap(save);
    originalEncounter(save);
    return save;
}

nlohmann::json Run::exportOriginalSave(const std::string& folder, bool closeOriginal) {
    if (!open_ || defeated_) {
        throw std::runtime_error("active_run_required");
    }
    if (tutorialOwned_) {
        throw std::runtime_error("tutorial_runs_cannot_be_exported");
    }
    // WorldManager::SaveGame 0x5f5410 first completes a pending jump
    // (CheckForNewLocation) and commits the upgrade cart (ConfirmUpgrades).
    if (jumpTicks_ > 0 || arrivalTicks_ > 0) {
        throw std::runtime_error("wait until the jump finishes");
    }
    if (!upgradePurchases_.empty()) {
        throw std::runtime_error("accept or undo the pending upgrades first");
    }
    if (enemyExplosionActive_) {
        throw std::runtime_error("wait until the enemy ship finishes exploding");
    }
    const std::filesystem::path directory =
        folder.empty() ? original_save::defaultSaveFolder() : std::filesystem::path(folder);
    if (directory.empty() || !std::filesystem::is_directory(directory)) {
        throw std::runtime_error("original_save_folder_not_found");
    }
    const auto context = originalSaveContext();
    const auto bytes = original_save::serialize(originalSave(), &context);
    // Read the result back with the original's order before touching the file.
    if (original_save::serialize(original_save::parse(bytes, context), &context) != bytes) {
        throw std::runtime_error("original_save_self_check_failed");
    }
    // The original rewrites continue.sav in its own folder when it quits. User
    // instruction (7 October 2026): the developer button discards the original's
    // session, terminating FTLGame.exe so it cannot save over the export.
    // Exports to any other folder (tests) leave a running original alone.
    std::error_code sameFolder;
    if (std::filesystem::equivalent(directory, original_save::defaultSaveFolder(), sameFolder)) {
        if (closeOriginal) {
            original_save::closeOriginalGame();
        } else if (original_save::originalGameRunning()) {
            throw std::runtime_error("close FTLGame.exe before exporting");
        }
    }
    const auto destination = directory / "continue.sav";
    nlohmann::json result{{"path", destination.string()}, {"bytes", bytes.size()}};
    if (std::filesystem::exists(destination)) {
        const auto backups =
            (nativeDataDirectory_.empty() ? directory
                                          : std::filesystem::path(nativeDataDirectory_)) /
            "original-save-backups";
        std::filesystem::create_directories(backups);
        const auto backup = backups / ("continue-" + timestamp() + ".sav");
        std::filesystem::copy_file(
            destination, backup, std::filesystem::copy_options::overwrite_existing);
        if (readFile(backup) != readFile(destination)) {
            throw std::runtime_error("original_save_backup_mismatch");
        }
        result["backup"] = backup.string();
    }
    const auto temporary = directory / "continue.sav.recomp-tmp";
    writeFile(temporary, bytes);
    if (readFile(temporary) != bytes) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("original_save_verify_failed");
    }
    std::filesystem::remove(destination);
    std::filesystem::rename(temporary, destination);
    return result;
}

nlohmann::json Run::originalSaveRoundTrip(const std::string& file) const {
    const auto bytes = readFile(file);
    const auto context = originalSaveContext();
    const auto save = original_save::parse(bytes, context);
    const auto written = original_save::serialize(save, &context);
    return {{"bytes", bytes.size()},
            {"roundtrip", written == bytes},
            {"player", save.player.blueprintName},
            {"enemy", save.enemy ? save.enemy->blueprintName : ""},
            {"crew", save.player.crew.size()},
            {"locations", save.starMap.locations.size()},
            {"original_event_names", events_.originalNameCount()}};
}
} // namespace ftl
