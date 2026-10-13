#include "hangar.hpp"
#include "log.hpp"
#include "time.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ftl {
namespace {
constexpr SDL_Color pale{235, 245, 229, 255}, ink{25, 49, 51, 255}, yellow{255, 230, 94, 255},
    dim{164, 171, 160, 255};
constexpr std::array<int, 10> order{0, 3, 4, 7, 2, 5, 6, 1, 9, 8};

std::string ui(const std::string& path) {
    return "img/customizeUI/" + path + ".png";
}

std::vector<std::string> armory(const ShipDefinition& ship, bool drones, bool equipped = true) {
    // CreateArmory4a6520 equips only the authored count when its system exists.
    const int system = drones ? 4 : 3;
    if (std::none_of(ship.systems.begin(), ship.systems.end(), [system](const auto& value) {
            return value.id == system && value.installed;
        })) {
        return {};
    }
    const auto& ids = drones ? ship.drones : ship.weapons;
    auto count = std::min(ids.size(), std::size_t(drones ? ship.droneCount : ship.weaponCount));
    if (equipped) {
        // AddDrone refuses at slot_count. AddWeapon610ce0 tests strict <,
        // allowing slot_count+1; its refusal occurs BEFORE factory birth.
        count = std::min(count, std::size_t(drones ? ship.droneSlots : ship.weaponSlots + 1));
    }
    return {ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::pair<int, int> initialCrewSlot(const ShipDefinition& ship,
                                    const std::vector<HangarCrew>& crew,
                                    int preferredRoom) {
    // CrewMember::SetRoom46f800 -> Ship::GetAvailableRoom485bb0 and
    // Room::GetEmptySlot46a0f0. Medical furniture occupies its slot before crew.
    auto emptySlot = [&](const RoomDefinition& room) {
        const int count = room.w * room.h / 1225;
        auto occupied = [&](int slot) {
            return std::any_of(crew.begin(),
                               crew.end(),
                               [&](const auto& member) {
                                   return member.room == room.id && member.slot == slot;
                               }) ||
                   std::any_of(ship.systems.begin(), ship.systems.end(), [&](const auto& system) {
                       return system.installed && (system.id == 5 || system.id == 13) &&
                              system.room == room.id && system.slot == slot;
                   });
        };
        int primary = -1;
        for (const auto& system : ship.systems) {
            if (system.installed && system.room == room.id &&
                (system.id == 0 || system.id == 1 || system.id == 3 || system.id == 6 ||
                 system.id == 7 || system.id == 8) &&
                system.slot >= 0 && system.slot < count) {
                primary = system.slot;
            }
        }
        if (primary >= 0 && !occupied(primary)) {
            return primary;
        }
        for (int slot = 0; slot < count; ++slot) {
            if (!occupied(slot)) {
                return slot;
            }
        }
        return -1;
    };
    auto available = [&](int id) -> std::pair<int, int> {
        const auto found =
            std::find_if(ship.rooms.begin(), ship.rooms.end(), [&](const auto& room) {
                return room.id == id;
            });
        return found == ship.rooms.end() ? std::pair{-1, -1} : std::pair{id, emptySlot(*found)};
    };
    if (const auto slot = available(preferredRoom); slot.second >= 0) {
        return slot;
    }
    // GetConnectedRooms45bb10 keeps authored door order and ignores openness.
    for (const auto& door : ship.doors) {
        if (door.roomA == preferredRoom || door.roomB == preferredRoom) {
            const int neighbor = door.roomA == preferredRoom ? door.roomB : door.roomA;
            if (const auto slot = available(neighbor); slot.second >= 0) {
                return slot;
            }
        }
    }
    std::vector<const RoomDefinition*> rooms;
    rooms.reserve(ship.rooms.size());
    for (const auto& room : ship.rooms) {
        rooms.push_back(&room);
    }
    std::sort(rooms.begin(), rooms.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });
    const auto preferred = std::find_if(rooms.begin(), rooms.end(), [&](const auto* room) {
        return room->id == preferredRoom;
    });
    std::pair<int, int> best{-1, -1};
    int closest = std::numeric_limits<int>::max();
    for (const auto* room : rooms) {
        const auto slot = available(room->id);
        if (slot.second < 0) {
            continue;
        }
        if (preferred == rooms.end()) {
            return slot;
        }
        const float distance =
            std::hypot(float(room->x - (*preferred)->x) + float(room->w - (*preferred)->w) / 2.f,
                       float(room->y - (*preferred)->y) + float(room->h - (*preferred)->h) / 2.f);
        // Original stores the winning Euclidean distance in an integer.
        if (best.second < 0 || float(closest) > distance) {
            best = slot;
            closest = int(distance);
        }
    }
    // The original all-full branch indexes past its room vector. Retain a
    // missing placement safely instead of reproducing that undefined access.
    return best;
}
} // namespace

std::string Hangar::label(const LocalizedText& value) {
    return value.reference ? texts_.get(value.value, locale_) : value.value;
}

std::string Hangar::shortCrewName(const std::string& value) {
    const bool chinese =
        locale_ == "zh-Hans" && std::any_of(value.begin(), value.end(), [](unsigned char byte) {
            return byte > 0xd1;
        });
    auto& font = renderer_.sourceFont(6, value + "..", chinese);
    return catalog_.shortCrewName(value, [&](const std::string& text) {
        const auto bounds = renderer_.bounds(font, text);
        return bounds.second - bounds.first;
    });
}

Font& Hangar::localizedFont(int font, const std::string& value) {
    return renderer_.sourceFont(font, value, locale_ == "zh-Hans");
}

void Hangar::text(
    const std::string& value, float x, float y, int font, bool center, SDL_Color color) {
    renderer_.text(localizedFont(font, value),
                   value,
                   x,
                   y,
                   Renderer::sourceFontScale(font, locale_ == "zh-Hans"),
                   color,
                   center);
}

void Hangar::open(const std::string& locale, std::uint32_t seed, const std::string& profile) {
    locale_ = locale;
    nativeNames_ = profile == "native";
    if (!sessionRandomBound_) {
        random_ = RandomStream(seed);
    }
    open_ = true;
    list_ = start_ = false;
    listTutorialPage_ = -1;
    tutorialChoicePending_ = encourageShipList_ = false;
    tutorialOpenSeconds_ = 0;
    rooms_ = true;
    // Deliberate override: default new hangar sessions to Advanced Edition enabled.
    advanced_ = true;
    difficulty_ = 1;
    customCrew_ = -1;
    editing_.clear();
    hover_.clear();
    tick_ = hoverTick_ = 0;
    sceneAnimationTime_ = 0;
    sceneAnimationFrames_.fill(0);
    walkingManX_ = 1245;
    walkingManY_ = 136;
    for (auto& a : unlocked_) {
        a.fill(false);
    }
    if (profile == "all_unlocked") {
        for (int f = 0; f < 10; ++f) {
            for (int t = 0; t < 3; ++t) {
                unlocked_[f][t] = catalog_.ship(f, t) != nullptr;
            }
        }
    } else if (profile == "reference") {
        for (int f : {0, 3, 4, 1}) {
            unlocked_[f][0] = true;
        }
        unlocked_[0][1] = unlocked_[3][1] = unlocked_[4][1] = true;
    } else if (profile == "fresh" || profile == "native") {
        unlocked_[0][0] = true;
    } else {
        throw std::runtime_error("unknown_hangar_profile");
    }
    for (auto& a : achievements_) {
        a.fill(profile == "all_unlocked");
    }
    for (auto& a : freshAchievements_) {
        a.fill(false);
    }
    for (auto& values : achievementDifficulties_) {
        values.fill(profile == "all_unlocked" ? 0 : -1);
    }
    for (int family = 0; family < 10; ++family) {
        victoryLayouts_[family].fill(-1);
        if (profile == "all_unlocked") {
            for (int layout = 0; layout < (family >= 8 ? 2 : 3); ++layout) {
                victoryLayouts_[family][layout] = 0;
            }
        }
    }
    // Immutable research fixture, reference/P02/017-profile-achievements.json.
    if (profile == "reference") {
        for (int f : {0, 3}) {
            for (int i = 0; i < 4; ++i) {
                achievements_[f][i] = true;
            }
        }
        achievements_[4][0] = achievements_[4][1] = achievements_[1][4] = true;
        for (int family = 0; family < 10; ++family) {
            for (int index = 0; index < 5; ++index) {
                if (achievements_[family][index]) {
                    achievementDifficulties_[family][index] = 0;
                }
            }
            if (achievements_[family][3]) {
                victoryLayouts_[family][0] = 0;
            }
        }
    }
    choose(0, 0);
}

void Hangar::progression(const ProgressionLedger& ledger) {
    auto document = xmlResource(archive_, "data/achievements.xml");
    for (int family = 0; family < 10; ++family) {
        for (int layout = 0; layout < 3; ++layout) {
            unlocked_[family][layout] =
                catalog_.ship(family, layout) && ledger.unlocked(family, layout);
        }
        achievements_[family].fill(false);
        freshAchievements_[family].fill(false);
        achievementDifficulties_[family].fill(-1);
        victoryLayouts_[family].fill(-1);
        int index = 0;
        for (const auto row : document.child("FTL").children("achievement")) {
            if (std::string(row.child("ship").child_value()) !=
                BlueprintCatalog::families[family]) {
                continue;
            }
            if (index == 3) {
                break;
            }
            const auto* award = ledger.achievement(row.attribute("id").as_string());
            achievements_[family][index] = award && award->unlocked;
            freshAchievements_[family][index] = award && award->fresh;
            achievementDifficulties_[family][index++] = award ? award->difficulty : -1;
        }
        const std::string base = BlueprintCatalog::families[family];
        const auto* victory = ledger.achievement(base + "_VICTORY");
        const auto* quest = ledger.achievement(base + "_QUEST");
        achievements_[family][3] = victory && victory->unlocked;
        achievements_[family][4] = quest && quest->unlocked;
        freshAchievements_[family][3] = victory && victory->fresh;
        freshAchievements_[family][4] = quest && quest->fresh;
        achievementDifficulties_[family][3] = victory ? victory->difficulty : -1;
        achievementDifficulties_[family][4] = quest ? quest->difficulty : -1;
        if (victory) {
            victoryLayouts_[family] = victory->layouts;
        }
    }
    hover_.clear();
    if (!unlocked_[family_][type_]) {
        choose(0, 0);
    }
}

void Hangar::choose(int family, int type) {
    if (family < 0 || family >= 10 || type < 0 || type > 2 || !catalog_.ship(family, type) ||
        !unlocked_[family][type]) {
        return;
    }
    previewTick_ = tick_;
    family_ = family;
    type_ = type;
    selectedShip_ = *catalog_.ship(family_, type_);
    for (auto& system : selectedShip_.systems) {
        if (system.installed) {
            system = playerSystemDefinition(system);
        }
    }
    const auto& s = selected();
    name_ = label(s.name);
    crew_.clear();
    weaponFactories_.clear();
    artilleryFactories_.clear();
    droneFactories_.clear();
    droneTrash_.clear();
    hackingFactory_ = CombatDrone{.blueprint = "DRONE_HACKING"};
    // Ship::Import486500 constructs every room before ExplosionAnimation.
    roomFireAnimationTimes_ = initializeRoomFireAnimations(s, catalog_, random_);
    medbayAnimationTimes_.clear();
    // ShipManager4a6e00 calls Ship::OnInit48be50 before creating systems.
    // ExplosionAnimation487f10 retains randomized gib vectors for this actor.
    initializeShipExplosion(explosionState_, s, random_);
    // Ship::OnInit48be50 always randomizes both4-frame,.5-second engines,
    // including ships whose engine flames are never rendered.
    for (auto& elapsed : engineAnimationTimes_) {
        elapsed = float(random_.next() % 4) * .125f;
    }
    catalog_.setUsedCrewNames({});
    customCrew_ = -1;
    editing_.clear();
    const auto weaponFactory = [&](const std::string& id) {
        const auto& blueprint = catalog_.equipment(id);
        CombatWeapon factory;
        // ProjectileFactory40b380 draws even for chargeLevels==1.
        factory.goalChargeLevel = int(random_.next() % std::max(1, blueprint.chargeLevels)) + 1;
        factory.weaponExplosionImage = blueprint.weaponExplosionAnimation;
        factory.animationPowered = true;
        // OnRender40bf80's hangar forceVisual sets the idle charged frame.
        // Preserve that visual state without changing factory cooldown/charge.
        factory.animationFrame = blueprint.chargedFrame;
        return factory;
    };
    // OnInit4a6e00 creates systems (including artillery), then armory,
    // then all initial crew blueprints and finally their live actors.
    for (const auto& system : s.systems) {
        if (system.installed && system.id == 5) {
            const auto room = std::find_if(s.rooms.begin(), s.rooms.end(), [&](const auto& value) {
                return value.id == system.room;
            });
            medbayAnimationTimes_[system.room] = initializeMedbayAnimations(*room, random_);
        } else if (system.installed && system.id == 11 && !system.weapon.empty()) {
            artilleryFactories_.push_back(weaponFactory(system.weapon));
        } else if (system.installed && system.id == 15) {
            // HackingSystem6080d0 constructs its embedded SpaceDrone here.
            initializeDroneConstructor(hackingFactory_, catalog_, locale_, random_);
            const auto room = std::find_if(s.rooms.begin(), s.rooms.end(), [&](const auto& value) {
                return value.id == system.room;
            });
            // ShipGraph::GetRoomCenter 0x45b020 halves the integer rectangle.
            hackingFactory_.position = {float(room->x + room->w / 2), float(room->y + room->h / 2)};
        }
    }
    for (const auto& id : armory(s, false)) {
        weaponFactories_.push_back(weaponFactory(id));
    }
    for (const auto& id : armory(s, true, false)) {
        CombatDrone factory{.blueprint = id};
        initializeDroneConstructor(factory, catalog_, locale_, random_);
        initializeDroneLocation(factory, s, 0, catalog_, random_);
        const auto& blueprint = catalog_.equipment(id);
        if (blueprint.type == "REPAIR" || blueprint.type == "BATTLE") {
            // CreateCrewDrone49bd20 appends the undeployed body before
            // ordinary crew; derived CrewDrone438130 overrides bDead=true.
            factory.crewPhysicalShip = 0;
            factory.crewRoom = factory.crewSlot = -1;
            factory.crewPosition = {};
            factory.crewDead = true;
        }
        // AddDrone613380 performs constructor/registration before checking
        // slot_count. Refusal leaves the same actor alive in its factories.
        if (droneFactories_.size() < static_cast<std::size_t>(s.droneSlots)) {
            droneFactories_.push_back(std::move(factory));
        } else {
            droneTrash_.push_back(std::move(factory));
        }
    }
    const std::array<std::string, 8> names{
        "Maloney", "Banks", "Alison", "Charlie", "Ari", "Sam", "Robin", "Alex"};

    struct InitialCrew {
        std::string fullName;
        std::string species;
        bool male = true;
    };

    std::vector<InitialCrew> initialCrew;
    for (const auto& group : s.crew) {
        for (int i = 0; i < group.count; ++i) {
            if (nativeNames_) {
                const auto [fullName, male] = catalog_.unusedCrewName(locale_, {}, [&] {
                    return random_.next();
                });
                initialCrew.push_back({fullName, group.species, male});
            } else {
                initialCrew.push_back({names[initialCrew.size() % names.size()], group.species});
            }
        }
    }
    std::vector<int> initialRooms;
    for (int id : {6, 1, 3, 0, 2, 7, 8, 10, 5, 4, 15, 9, 11}) {
        const auto system =
            std::find_if(s.systems.begin(), s.systems.end(), [&](const auto& value) {
                return value.installed && value.id == id;
            });
        if (system != s.systems.end()) {
            // AddInitialCrew4a3610 contains each system ID once, even artillery.
            initialRooms.push_back(system->room);
        }
    }
    for (std::size_t index = 0; index < initialCrew.size(); ++index) {
        const auto& initial = initialCrew[index];
        const int preferredRoom = index < initialRooms.size()
                                      ? initialRooms[index]
                                      : int(random_.next() % s.rooms.size());
        // The random preferred room precedes this actor's constructor draws.
        const auto [room, slot] = initialCrewSlot(s, crew_, preferredRoom);
        const float shootGoal = float(random_.next() % 300 + 1000) / 1000.f;
        if (nativeNames_) {
            int colors = 0;
            int stride = 1;
            for (const auto& layer : catalog_.appearance(initial.species).layers) {
                if (!layer.empty()) {
                    colors += int(random_.next() % layer.size()) * stride;
                    stride *= int(layer.size());
                }
            }
            const int appearance =
                initial.species == "human" ? colors * 2 + (initial.male ? 0 : 1) : colors;
            HangarCrew crew{shortCrewName(initial.fullName),
                            initial.species,
                            appearance,
                            room,
                            slot,
                            initial.fullName};
            crew.shootGoal = shootGoal;
            crew_.push_back(std::move(crew));
            // Factory::CreateCrewmember45e770 refreshes current names
            // after adding each actor, including hangar preview crew.
            std::vector<std::string> used;
            used.reserve(crew_.size());
            for (const auto& member : crew_) {
                used.push_back(member.fullName);
            }
            catalog_.setUsedCrewNames(used);
        } else {
            HangarCrew crew{
                initial.fullName, initial.species, int(random_.next() % 8), room, slot, {}};
            crew.shootGoal = shootGoal;
            crew_.push_back(std::move(crew));
        }
    }
    if (!nativeNames_) {
        for (auto& c : crew_) {
            c.fullName = c.name == "Maloney" ? "Johnny Maloney" : c.name;
        }
    }
    // ShipManager4a6e00 initializes the whole FireSpreader after initial crew.
    fireSpreaderAnimations_ = initializeFireSpreaderAnimations(s, random_);
    crewNameBlinks_.assign(crew_.size(), {});
    // OnInit immediately calls the manager loop before SaveCrewPositions.
    // Startup CFPS has zero delta; later births use the accepted fixed step.
    advanceShipAnimations(constructorLoopDelta_, true);
    customizeMode_ = false;
}

void Hangar::advanceShipAnimations(float seconds, bool systemAnimations) {
    // Manager4a05b0 advances systems before Ship and environment. Once the
    // builder sets iCustomizeMode=2, it skips system OnLoop entirely.
    if (systemAnimations) {
        for (auto& [room, clocks] : medbayAnimationTimes_) {
            for (auto& elapsed : clocks) {
                elapsed = Time::loopAnimation(elapsed, 1.f, seconds);
            }
        }
    }
    for (auto& elapsed : engineAnimationTimes_) {
        elapsed = Time::loopAnimation(elapsed, .5f, seconds);
    }
    for (auto& [room, clocks] : roomFireAnimationTimes_) {
        for (auto& elapsed : clocks) {
            elapsed =
                Time::loopAnimation(elapsed, catalog_.animation("fire_small").duration, seconds);
        }
    }
    for (auto& [room, states] : fireSpreaderAnimations_) {
        for (auto& state : states) {
            state.elapsed = Time::loopAnimation(state.elapsed, 1.f, seconds);
        }
    }
}

void Hangar::step(std::uint32_t ticks) {
    fraction_ = 0;
    tick_ += ticks;
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        const auto advanceBlink =
            [](NameInputBlink& blink, std::size_t caret, bool renderBeforeUpdate) {
                blink.renderElapsed = blink.elapsed;
                if (blink.caret != caret) {
                    blink.elapsed = 0;
                }
                blink.caret = caret;
                blink.elapsed += float(Time::step);
                // TimerHelper::Done5a9640 loops only AFTER the strict endpoint.
                if (blink.elapsed > 1.f) {
                    blink.elapsed = 0;
                }
                if (!renderBeforeUpdate) {
                    blink.renderElapsed = blink.elapsed;
                }
            };
        if (editing_ == "ship") {
            advanceBlink(shipNameBlink_, caret_, false);
        }
        if (customCrew_ >= 0) {
            auto& blink = crewNameBlinks_.at(static_cast<std::size_t>(customCrew_));
            advanceBlink(blink, editing_ == "crew" ? caret_ : blink.caret, true);
        }
        advanceShipAnimations(Time::step, !customizeMode_);
        // ShipBuilder4df500 sets this after the first manager loop.
        customizeMode_ = true;
        for (auto& factory : weaponFactories_) {
            factory.animationSlide = std::min(.5f, factory.animationSlide + float(Time::step));
        }
        for (auto& factory : artilleryFactories_) {
            factory.animationSlide = std::min(.5f, factory.animationSlide + float(Time::step));
        }
        if (listPulseSeconds_ == .75) {
            listPulseSeconds_ = 0;
        }
        listPulseSeconds_ = std::min(.75, listPulseSeconds_ + 1.0 / Time::tickHz);
        // Animation::Update59bd50 computes old/new timeline frames, clamps the
        // endpoint, then resets on the following tick. Only custom_type draws
        // RNG, retrying until its displayed frame differs from the old one.
        const int oldFrame = std::min(3, int(sceneAnimationTime_ * 4));
        if (sceneAnimationTime_ == 1.f) {
            sceneAnimationTime_ = 0;
        }
        sceneAnimationTime_ = std::min(1.f, sceneAnimationTime_ + float(Time::step));
        const int newFrame = std::min(3, int(sceneAnimationTime_ * 4));
        if (oldFrame != newFrame) {
            const int previous = sceneAnimationFrames_[0];
            do {
                sceneAnimationFrames_[0] = int(random_.next() % 4);
            } while (sceneAnimationFrames_[0] == previous);
        }
        sceneAnimationFrames_[1] = sceneAnimationFrames_[2] =
            std::min(8, int(sceneAnimationTime_ * 9));
        // ShipBuilder::OnLoop4df500 uses unscaled CFPS speed16 for this walk.
        const float speed = float(16.0 / Time::tickHz);
        walkingManY_ += speed;
        if (walkingManY_ > 400) {
            walkingManX_ += speed;
        }
        if (walkingManY_ > 460) {
            walkingManY_ = 100;
            walkingManX_ = 1275;
            walkingManX_ -= speed;
        } else if (walkingManX_ > 1245 && walkingManY_ < 400) {
            walkingManX_ -= speed;
        }
        sceneAnimationFrames_[3] = newFrame;
    }
    if (list_ && listTutorialPage_ >= 0) {
        tutorialOpenSeconds_ += double(ticks) / Time::tickHz;
        if (tutorialChoicePending_) {
            tutorialChoicePending_ = false;
            listTutorialPage_ = listTutorialPage_ == 0 ? 1 : -1;
            hover_.clear();
            move(mx_, my_);
        }
    }
    if (!openedList_ && !encourageShipList_) {
        for (int family = 0; family < 10; ++family) {
            for (int layout = 0; layout < 3; ++layout) {
                if ((family || layout) && unlocked_[family][layout]) {
                    encourageShipList_ = true;
                    // Source OnRender4de3f0 writes1 once the prompt appears.
                    // Keep drawing pure under the accepted native UI clock.
                    openedList_ = 1;
                }
            }
        }
    }
    if (ticks) {
        std::vector<std::string> names;
        names.reserve(crew_.size());
        for (const auto& crew : crew_) {
            names.push_back(crew.fullName);
        }
        catalog_.setUsedCrewNames(names);
    }
}

std::vector<std::string> Hangar::listTutorialLines() {
    const auto body =
        texts_.get(listTutorialPage_ == 0 ? "list_tutorial" : "list_tutorial_2", locale_);
    return renderer_.wrappedLines(localizedFont(12, body),
                                  body,
                                  565,
                                  Renderer::sourceFontScale(12, locale_ == "zh-Hans"),
                                  locale_ == "zh-Hans");
}

Rect Hangar::listTutorialChoice() {
    const auto body =
        texts_.get(listTutorialPage_ == 0 ? "list_tutorial" : "list_tutorial_2", locale_);
    auto& font = localizedFont(12, body);
    const float bodyHeight =
        float(listTutorialLines().size() * Renderer::sourceLineHeight(12, font));
    const auto choice = "1. " + texts_.get("continue", locale_);
    auto& choiceFont = localizedFont(12, choice);
    const auto lines = renderer_.wrappedLines(choiceFont,
                                              choice,
                                              565,
                                              Renderer::sourceFontScale(12, locale_ == "zh-Hans"),
                                              locale_ == "zh-Hans");
    float width = 0;
    for (const auto& line : lines) {
        const auto bounds =
            renderer_.bounds(choiceFont, line, Renderer::sourceFontScale(12, locale_ == "zh-Hans"));
        width = std::max(width, bounds.second - bounds.first);
    }
    return {352,
            200 + bodyHeight,
            float(int(width)),
            float(lines.size() * Renderer::sourceLineHeight(12, choiceFont) + 12)};
}

void Hangar::listTutorial() {
    // Centered ChoiceBox4ec490: WindowFrame(7,22,602,377) at327,125.
    renderer_.windowFrame({327, 140, 616, 391});
    const auto body =
        texts_.get(listTutorialPage_ == 0 ? "list_tutorial" : "list_tutorial_2", locale_);
    const auto lines = listTutorialLines();
    const float height = Renderer::sourceLineHeight(12, localizedFont(12, body));
    float y = 170;
    for (const auto& line : lines) {
        text(line, 352, y, 12, false, {255, 255, 255, 255});
        y += float(height);
    }
    const auto choice = "1. " + texts_.get("continue", locale_);
    const auto choiceLines =
        renderer_.wrappedLines(localizedFont(12, choice),
                               choice,
                               565,
                               Renderer::sourceFontScale(12, locale_ == "zh-Hans"),
                               locale_ == "zh-Hans");
    y = listTutorialChoice().y + 10;
    for (const auto& line : choiceLines) {
        text(line,
             352,
             y,
             12,
             false,
             hover_ == "list_tutorial_continue" ? SDL_Color{243, 255, 80, 255}
                                                : SDL_Color{255, 255, 255, 255});
        y += float(Renderer::sourceLineHeight(12, localizedFont(12, choice)));
    }
}

std::vector<Hangar::Button> Hangar::buttons(bool includeTutorial) {
    if (list_) {
        if (includeTutorial && listTutorialPage_ >= 0) {
            return {{"list_tutorial_continue",
                     texts_.get("continue", locale_),
                     listTutorialChoice(),
                     12}};
        }
        std::vector<Button> out;
        out.reserve(34); // Three tabs, close button, and at most thirty ship choices.
        for (int i = 0; i < 3; ++i) {
            out.push_back({"list_type:" + std::to_string(i),
                           texts_.get("typebutton_type_" + std::string(1, char('a' + i)), locale_),
                           {float(685 + 87 * i), 122, 83, 22},
                           62,
                           listType_ != i,
                           listType_ == i});
        }
        const std::array<int, 10> listOrder{0, 3, 4, 7, 9, 1, 6, 5, 2, 8};
        for (int i = 0; i < 10; ++i) {
            int f = listOrder[i];
            out.push_back({"ship:" + std::to_string(f),
                           "",
                           {float(i % 5 == 4 ? 976 : 136 + (i % 5) * 205),
                            float((i < 5 ? 161 : 338) +
                                  (unlocked_[f][0] && catalog_.ship(f, listType_) ? 0 : 20)),
                            191,
                            float(unlocked_[f][0] ? 161 : 121)},
                           62,
                           bool(catalog_.ship(f, listType_)) && unlocked_[f][listType_]});
        }
        return out;
    }
    if (customCrew_ >= 0) {
        const auto rename = texts_.get("crewbox_rename", locale_);
        const auto accept = texts_.get("crewbox_accept", locale_);
        auto fits = [&](const std::string& value) {
            const auto bounds = renderer_.bounds(localizedFont(62, value), value);
            return (bounds.second - bounds.first) *
                       Renderer::sourceFontScale(62, locale_ == "zh-Hans") <=
                   91;
        };
        const int font = fits(rename) && fits(accept) ? 62 : 12;
        return {{"crew_previous", "<", {111, 559, 32, 28}},
                {"crew_next", ">", {263, 559, 32, 28}},
                {"crew_rename", rename, {105, 635, 94, 24}, font},
                {"crew_accept", accept, {206, 635, 94, 24}, font}};
    }
    const bool canCycle = std::any_of(unlocked_.begin(), unlocked_.end(), [&](const auto& layouts) {
        return &layouts != &unlocked_[family_] && layouts[type_];
    });
    const int unlockedFamilies =
        int(std::count_if(unlocked_.begin(), unlocked_.end(), [](const auto& layouts) {
            return layouts[0];
        }));
    std::vector<Button> out{
        {"start",
         texts_.get("start_button", locale_),
         {1082, 28, 152, 48},
         64,
         advanced_ || (family_ != 9 && type_ != 2)},
        {"rename", texts_.get("ship_rename_button", locale_), {14, 16, 94, 32}},
        {"previous", "<", {33, 145, 32, 28}, 62, canCycle},
        {"next", ">", {126, 145, 32, 28}, 62, canCycle},
        {"list", texts_.get("ship_list_button", locale_), {70, 145, 51, 28}},
        {"random",
         texts_.get("ship_random_button", locale_),
         {19, 178, 152, 32},
         62,
         unlockedFamilies > 1},
        {"rooms",
         texts_.get(rooms_ ? "ship_hiderooms_button" : "ship_showrooms_button", locale_),
         {20, 301, 151, 28}},
        {"easy", texts_.get("easy", locale_), {977, 14, 95, 24}, 62, true, difficulty_ == 0},
        {"normal", texts_.get("normal", locale_), {977, 40, 95, 24}, 62, true, difficulty_ == 1},
        {"hard", texts_.get("hard", locale_), {977, 66, 95, 24}, 62, true, difficulty_ == 2},
        {"advanced_off",
         texts_.get("advanced_off_button", locale_),
         {995, 425, 107, 22},
         62,
         true,
         !advanced_},
        {"advanced_on",
         texts_.get("advanced_on_button", locale_),
         {1127, 425, 100, 22},
         62,
         true,
         advanced_}};
    for (int i = 0; i < (family_ >= 8 ? 2 : 3); ++i) {
        out.push_back({"type:" + std::to_string(i),
                       std::string(1, char('A' + i)),
                       {float(32 + 45 * i + (family_ >= 8 ? 22 : 0)), 248, 36, 34},
                       63,
                       unlocked_[family_][i] && type_ != i,
                       type_ == i});
    }
    for (int i = 0; i < int(crew_.size()); ++i) {
        out.push_back({"crew:" + std::to_string(i),
                       texts_.get("crewbox_customize", locale_),
                       {float(70 + (i % 2) * 160), float(585 + (i / 2) * 93), 100, 20},
                       51});
    }
    return out;
}

void Hangar::move(float x, float y) {
    if (mx_ != x || my_ != y) {
        hoverTick_ = tick_;
    }
    mx_ = x;
    my_ = y;
    std::string next;
    for (const auto& b : buttons()) {
        if (b.id.starts_with("ship:") && !catalog_.ship(std::stoi(b.id.substr(5)), listType_)) {
            continue; // ShipButton: nonexistent Type C buttons are inactive.
        }
        if (b.contains(x, y)) {
            next = b.id;
        }
    }
    if (next != hover_ && !next.empty() && listTutorialPage_ < 0) {
        audio_.hover();
    }
    hover_ = next;
}

void Hangar::activate(const std::string& id) {
    if (editing_ == "ship" && id != "rename") {
        stopEditing();
    }
    if (id == "list") {
        list_ = true;
        listType_ = type_;
        listTutorialPage_ = openedList_ != 2 ? 0 : -1;
        tutorialOpenSeconds_ = 0;
        tutorialChoicePending_ = encourageShipList_ = false;
        openedList_ = 2;
        hover_.clear();
    } else if (id == "list_tutorial_continue") {
        if (dialogKeys_ != 2 || tutorialOpenSeconds_ >= .3) {
            tutorialChoicePending_ = true;
        }
    } else if (id == "rooms") {
        rooms_ = !rooms_;
    } else if (id == "start") {
        start_ = true;
    } else if (id == "rename") {
        if (editing_ == "ship") {
            stopEditing();
        } else {
            editing_ = "ship";
        }
        caret_ = name_.size();
    } else if (id == "easy") {
        difficulty_ = 0;
    } else if (id == "normal") {
        difficulty_ = 1;
    } else if (id == "hard") {
        difficulty_ = 2;
    } else if (id == "advanced_on") {
        advanced_ = true;
    } else if (id == "advanced_off") {
        advanced_ = false;
    } else if (id.starts_with("list_type:")) {
        listType_ = std::stoi(id.substr(10));
    } else if (id.starts_with("ship:")) {
        choose(std::stoi(id.substr(5)), listType_);
        list_ = false;
    } else if (id.starts_with("type:")) {
        const int type = std::stoi(id.substr(5));
        if (type != type_) {
            choose(family_, type);
        }
    } else if (id == "previous" || id == "next") {
        int index = int(std::find(order.begin(), order.end(), family_) - order.begin());
        int delta = id == "next" ? 1 : 9;
        for (int i = 1; i <= 10; ++i) {
            int f = order[(index + delta * i) % 10];
            if (f != family_ && unlocked_[f][type_]) {
                choose(f, type_);
                break;
            }
        }
    } else if (id == "random") {
        std::vector<std::pair<int, int>> choices;
        for (int t = 0; t < 3; ++t) {
            for (int f = 0; f < 10; ++f) {
                if (unlocked_[f][t]) {
                    choices.emplace_back(f, t);
                }
            }
        }
        auto [f, t] = choices[random_.next() % choices.size()];
        choose(f, t);
    } else if (id.starts_with("crew:")) {
        customCrew_ = std::stoi(id.substr(5));
        editing_.clear();
    } else if (id == "crew_accept") {
        customCrew_ = -1;
        editing_.clear();
    } else if (id == "crew_rename") {
        editing_ = editing_ == "crew" ? "" : "crew";
        caret_ = crew_.at(customCrew_).fullName.size();
    } else if (id == "crew_next" || id == "crew_previous") {
        auto& c = crew_.at(customCrew_);
        const bool human = c.species == "human";
        const int sexes = human ? 2 : 1;
        const int sex = human ? 1 - c.appearance % 2 : 0;
        // CrewCustomizeBox52d710 toggles sex first. Left advances every
        // color layer on the female transition; right reverses every layer
        // on the male transition. Nonhuman species cycle on every click.
        const bool cycle = !human || (id == "crew_previous" ? sex == 1 : sex == 0);
        const int direction = id == "crew_previous" ? 1 : -1;
        int choices = c.appearance / sexes;
        int packed = 0;
        int stride = 1;
        for (const auto& colors : catalog_.appearance(c.species).layers) {
            const int count = int(colors.size());
            const int choice = choices % count;
            choices /= count;
            packed += (cycle ? (choice + direction + count) % count : choice) * stride;
            stride *= count;
        }
        c.appearance = packed * sexes + sex;
    }
}

void Hangar::click(float x, float y) {
    logging::ClickTrace trace("hangar", x, y, false);
    move(x, y);
    for (const auto& b : buttons()) {
        if (b.contains(x, y)) {
            trace.hit("button '" + b.id + "'" + (b.enabled ? "" : " (disabled)"));
            if (b.enabled) {
                activate(b.id);
            }
            return;
        }
    }
    if (list_ && listTutorialPage_ >= 0) {
        trace.hit("ship list tutorial page (blocks clicks)");
        return;
    }
    if (list_ && !Rect{112, 110, 1080, 412}.contains(x, y)) {
        trace.hit("outside ship list (closes list)");
        list_ = false;
    }
    if (!editing_.empty()) {
        trace.hit("outside text field (stops editing '" + editing_ + "')");
        stopEditing();
    }
}

void Hangar::stopEditing() {
    if (editing_ == "ship" && name_.empty()) {
        // ShipBuilder::OnLoop4df500 resolves an empty accepted ship name.
        name_ = texts_.get("nameless_one", locale_);
    }
    editing_.clear();
}

void Hangar::key(const std::string& key) {
    if (list_ && listTutorialPage_ >= 0) {
        if (key == "1" && dialogKeys_ != 0) {
            activate("list_tutorial_continue");
        }
        return;
    }
    if (!editing_.empty()) {
        auto& value = editing_ == "ship" ? name_ : crew_.at(customCrew_).fullName;
        auto previous = [&]() {
            if (caret_) {
                --caret_;
                while (caret_ && (static_cast<unsigned char>(value[caret_]) & 0xc0) == 0x80) {
                    --caret_;
                }
            }
        };
        auto next = [&]() {
            if (caret_ < value.size()) {
                ++caret_;
                while (caret_ < value.size() &&
                       (static_cast<unsigned char>(value[caret_]) & 0xc0) == 0x80) {
                    ++caret_;
                }
            }
        };
        if (key == "Backspace" && caret_) {
            auto end = caret_;
            previous();
            value.erase(caret_, end - caret_);
        } else if (key == "Delete" && caret_ < value.size()) {
            auto begin = caret_;
            next();
            value.erase(begin, caret_ - begin);
            caret_ = begin;
        } else if (key == "Left") {
            previous();
        } else if (key == "Right") {
            next();
        } else if (key == "Home") {
            caret_ = 0;
        } else if (key == "End") {
            caret_ = value.size();
        } else if (key == "Clear") {
            value.clear();
            caret_ = 0;
        }
        if (editing_ == "crew") {
            crew_.at(customCrew_).name = shortCrewName(value);
        }
        if (key == "Enter" || key == "Escape") {
            stopEditing();
        }
        return;
    }
    if (key == "Escape") {
        if (list_) {
            list_ = false;
        } else if (customCrew_ >= 0) {
            customCrew_ = -1;
        } else {
            open_ = false;
        }
        return;
    }
    if (list_ || customCrew_ >= 0) {
        return;
    }
    if (key == "Right") {
        activate("next");
    } else if (key == "Left") {
        activate("previous");
    } else if (key == "Up" || key == "Down") {
        for (int i = 1; i <= 3; ++i) {
            int t = (type_ + (key == "Up" ? i : 3 - i)) % 3;
            if (t != type_ && unlocked_[family_][t]) {
                choose(family_, t);
                break;
            }
        }
    }
}

void Hangar::inputText(const std::string& value) {
    if (editing_.empty()) {
        return;
    }
    auto& target = editing_ == "ship" ? name_ : crew_.at(customCrew_).fullName;
    std::size_t characters = 0;
    for (unsigned char c : target) {
        if ((c & 0xc0) != 0x80) {
            ++characters;
        }
    }
    const std::size_t maximum = editing_ == "ship" ? 16 : 15;
    for (std::size_t pos = 0; pos < value.size() && characters < maximum;) {
        const auto first = static_cast<unsigned char>(value[pos]);
        const std::size_t bytes = first < 128                    ? 1
                                  : first >= 194 && first <= 223 ? 2
                                  : first >= 224 && first <= 239 ? 3
                                  : first >= 240 && first <= 244 ? 4
                                                                 : 0;
        if (bytes == 0 || bytes > value.size() - pos) {
            ++pos;
            continue;
        }
        std::uint32_t codepoint = first & (bytes == 1 ? 127U : (1U << (7U - bytes)) - 1U);
        bool valid = true;
        for (std::size_t offset = 1; offset < bytes; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[pos + offset]);
            if ((continuation & 192U) != 128U) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (continuation & 63U);
        }
        const std::uint32_t minimum = bytes == 1 ? 0 : bytes == 2 ? 128 : bytes == 3 ? 2048 : 65536;
        if (!valid || codepoint < minimum || codepoint > 1114111 ||
            (codepoint >= 55296 && codepoint <= 57343)) {
            ++pos;
            continue;
        }
        if (sourceNameCharacterAllowed(codepoint, locale_)) {
            target.insert(caret_, value.substr(pos, bytes));
            caret_ += bytes;
            ++characters;
        }
        pos += bytes;
    }
    if (editing_ == "crew") {
        crew_.at(customCrew_).name = shortCrewName(target);
    }
}

void Hangar::semantic(const std::string& target, const std::string& action) {
    if (action != "hover" && action != "click") {
        throw std::runtime_error("unsupported_input_action");
    }
    for (const auto& b : buttons()) {
        if (b.id == target) {
            float x = b.rect.x + b.rect.w / 2;
            float y = b.rect.y + b.rect.h / 2;
            if (action == "hover") {
                move(x, y);
            } else {
                click(x, y);
            }
            return;
        }
    }
    throw std::runtime_error("target_not_available");
}

void Hangar::button(const Button& source) {
    auto b = source;
    if (b.id.starts_with("crew:")) {
        b.rect = {b.rect.x + 4, b.rect.y + 4, 92, 12};
    }

    if (b.id == "previous" || b.id == "next" || b.id == "crew_previous" || b.id == "crew_next") {
        auto& t = renderer_.image(ui(!b.enabled       ? "button_arrow_off"
                                     : hover_ == b.id ? "button_arrow_select2"
                                                      : "button_arrow_on"));
        renderer_.sprite(t,
                         b.rect,
                         {0, 0, float(t.w), float(t.h)},
                         0,
                         {255, 255, 255, 255},
                         b.id == "next" || b.id == "crew_next");
        return;
    }
    SDL_Color c = b.selected ? yellow : !b.enabled ? dim : hover_ == b.id ? yellow : pale;
    int corner = b.font == 51                       ? 2
                 : b.id == "rename" || b.font >= 63 ? 6
                 : b.id.starts_with("advanced_") || b.id.starts_with("list_type:") ||
                         b.id == "crew_rename" || b.id == "crew_accept"
                     ? 3
                     : 4;
    for (int row = 0; row < int(b.rect.h); ++row) {
        float inset = float(std::max(0, corner - std::min(row, int(b.rect.h) - row - 1)));
        renderer_.fill({b.rect.x + inset, b.rect.y + row, b.rect.w - 2 * inset, 1}, c);
    }
    int offset = b.font == 64 ? 6 : b.font == 63 ? 4 : b.font == 62 ? 2 : b.font == 12 ? 3 : 0;
    auto title = b.label;
    if (b.font >= 60) {
        for (auto& ch : title) {
            if (ch >= 'a' && ch <= 'z') {
                ch -= 32;
            }
        }
    }
    auto& font = localizedFont(b.font, title);
    const int textHeight = int(Renderer::sourceLineHeight(b.font, font));
    const auto lines =
        renderer_.wrappedLines(font,
                               title,
                               0,
                               Renderer::sourceFontScale(b.font, locale_ == "zh-Hans"),
                               locale_ == "zh-Hans");
    float y = b.rect.y + int(b.rect.h) / 2 - textHeight * int(lines.size()) / 2 + offset;
    for (const auto& line : lines) {
        text(line, b.rect.x + int(b.rect.w) / 2, y, b.font, true, ink);
        y += float(textHeight);
    }
}

void Hangar::shipPreview() {
    ShipPaintState v;
    v.x = 365;
    v.y = 30;
    v.tick = tick_;
    v.engineAnimationTimes = engineAnimationTimes_;
    v.weaponStates = &weaponFactories_;
    for (const auto& artillery : artilleryFactories_) {
        v.artilleryStates.push_back(&artillery);
    }
    // ShipSystem::OnRenderEffects 0x613ff0 draws these timers only while
    // a medbay is under an active level > 1 hack. Preview systems are healthy.
    v.colorblind = colorblind_;
    v.deployment = (float(tick_ - previewTick_) + fraction_) / 30.f;
    v.rooms = rooms_;
    auto ship = selected();
    ship.weapons = armory(ship, false);
    ship.drones = armory(ship, true);
    for (const auto& member : crew_) {
        std::string animation = "walk_down";
        for (const auto& system : ship.systems) {
            // CrewMember::OnLoop 0x4747f0 uses active manning for typing.
            // Preview weapons/drone factories have no allocated power.
            if (!system.installed || member.room != system.room || member.slot != system.slot ||
                (system.id != 0 && system.id != 1 && system.id != 6 && system.id != 7 &&
                 system.id != 8)) {
                continue;
            }
            auto direction = system.direction;
            std::transform(
                direction.begin(), direction.end(), direction.begin(), [](unsigned char value) {
                    return char(std::tolower(value));
                });
            if (direction == "up") {
                direction = "down";
            } else if (direction == "down") {
                direction = "up";
            }
            if (!direction.empty() && direction != "none") {
                animation = "type_" + direction;
            }
        }
        v.crewAnimations.push_back(animation);
        v.crewTicks.push_back(animation.starts_with("type_") ? tick_ : 0);
    }
    ShipPainter(renderer_, archive_, catalog_).draw(ship, crew_, v);
}

void Hangar::equipment(bool labels) {
    const auto& s = selected();
    const auto eq = [](const std::string& name) {
        return "img/upgradeUI/Equipment/" + name + ".png";
    };
    for (int i = 0; customCrew_ < 0 && i < std::max(4, int(crew_.size())); ++i) {
        float x = 70.f + (i % 2) * 160;
        float y = 519.f + (i / 2) * 93;
        if (labels) {
            if (i < int(crew_.size())) {
                text(crew_[i].name, x + 50, y + 49, 6, true, {255, 255, 255, 255});
            }
            continue;
        }
        renderer_.draw(
            eq(i < int(crew_.size())
                   ? (Rect{x, y, 100, 86}.contains(mx_, my_) ? "box_crew_selected" : "box_crew_on")
                   : "box_crew_off"),
            x,
            y);
        if (i < int(crew_.size())) {
            renderer_.draw(ui(hover_ == "crew:" + std::to_string(i) ? "box_crewcustom_selected"
                                                                    : "box_crewcustom_on"),
                           x,
                           y + 62);
        }
        if (i < int(crew_.size())) {
            const auto& c = crew_[i];
            crewSprite(c, x + 15, y - 10, 2);
        }
    }
    auto boxes = [&](const std::vector<std::string>& ids, int count, float y, bool drones) {
        for (int i = 0; i < count; ++i) {
            float x = 435.f + 58 * (4 - count) + i * 117;
            if (labels) {
                if (i < int(ids.size())) {
                    const auto& e = catalog_.equipment(ids[i]);
                    text(label(e.shortTitle), x + 60, y + 58, 6, true, {255, 255, 255, 255});
                }
                continue;
            }
            renderer_.draw(eq(std::string(drones ? "box_drones_" : "box_weapons_") +
                              (i < int(ids.size())
                                   ? (Rect{x, y, 109, 76}.contains(mx_, my_) ? "selected" : "on")
                                   : "off")),
                           x,
                           y);
            if (i < int(ids.size())) {
                const auto& e = catalog_.equipment(ids[i]);
                if (drones) {
                    if (e.type == "BATTLE" || e.type == "REPAIR") {
                        auto species =
                            e.type == "BATTLE" ? std::string("battle") : std::string("repair");
                        const auto& f = catalog_.animation(species + "_walk_down");
                        if (!f.usable()) {
                            continue;
                        }
                        Rect src{float(f.x), float(f.y), float(f.width), float(f.height)};
                        Rect dst{x + 60 - f.width / 2.f,
                                 y + 32 - f.height / 2.f,
                                 float(f.width),
                                 float(f.height)};
                        renderer_.sprite(renderer_.image(f.path), dst, src);
                        auto layer = "img/people/" + species + "_layer1.png";
                        if (archive_.contains(layer)) {
                            renderer_.sprite(
                                renderer_.image(layer), dst, src, 0, {100, 255, 100, 255});
                        }
                    } else {
                        auto path = "img/ship/drones/" + e.art + "_charged.png";
                        if (archive_.contains(path)) {
                            auto& im = renderer_.image(path);
                            renderer_.draw(path, x + 60 - im.w / 2.f, y + 32 - im.h / 2.f);
                        }
                    }
                } else if (!e.imagePath.empty() && e.frameWidth > 0 && e.frameHeight > 0) {
                    SDL_FPoint pivot{float(e.frameWidth / 2), float(e.frameHeight / 2)};
                    renderer_.sprite(renderer_.image(e.imagePath),
                                     {x + 63 - pivot.x,
                                      y + 35 - pivot.y,
                                      float(e.frameWidth),
                                      float(e.frameHeight)},
                                     {float(e.frameX + e.chargedFrame * e.frameWidth),
                                      float(e.frameY),
                                      float(e.frameWidth),
                                      float(e.frameHeight)},
                                     90,
                                     {255, 255, 255, 255},
                                     true,
                                     &pivot);
                }
            }
        }
    };
    boxes(armory(s, false), s.weaponSlots, 514, false);
    bool droneSystem = std::any_of(s.systems.begin(), s.systems.end(), [](const auto& v) {
        return v.id == 4 && v.installed;
    });
    if (droneSystem) {
        boxes(armory(s, true), s.droneSlots, 624, true);
    }
    for (int i = 0; i < 3; ++i) {
        if (!labels) {
            renderer_.draw(eq(i < int(s.augments.size()) ? "box_augment_on" : "box_augment_off"),
                           990,
                           float(529 + 60 * i));
        } else if (i < int(s.augments.size())) {
            text(label(catalog_.equipment(s.augments[i]).title),
                 1107,
                 float(543 + 60 * i),
                 13,
                 true,
                 {255, 255, 255, 255});
        }
    }
}

void Hangar::systems() {
    const auto& s = selected();
    std::vector<SystemDefinition> systems;
    for (const auto& sys : s.systems) {
        if (sys.installed) {
            systems.push_back(sys);
        }
    }
    std::sort(systems.begin(), systems.end(), [](auto& a, auto& b) {
        return a.id < b.id;
    });
    for (int i = 0; i < int(systems.size()); ++i) {
        float x = 373.f + i * 38;
        renderer_.draw(
            ui(Rect{x, 400, 38, 76}.contains(mx_, my_) ? "box_system_select2" : "box_system_on"),
            x,
            380);
        for (int p = 0; p < systems[i].power; ++p) {
            renderer_.fill({x + 11, 433.f - p * 8, 16, 6}, {80, 255, 80, 255});
        }
        auto path = "img/icons/s_" + systems[i].name + "_green1.png";
        if (archive_.contains(path)) {
            renderer_.draw(path, x - 13, 425);
        } else {
            text(systems[i].name.substr(0, 1), x + 19, 451, 13, true);
        }
    }
}

void Hangar::crewSprite(const HangarCrew& c, float x, float y, float scale) {
    ShipPainter(renderer_, archive_, catalog_).crewSprite(c, x, y, scale);
}

void Hangar::sceneAnimations() {
    // Open4e2b50 and immutable Point initializers: decorative animations precede
    // the name box/equipment. Rendering reads frames prepared by the UI clock.
    const std::array<std::string, 4> names{
        "custom_type", "custom_repair", "custom_repair", "custom_rock"};
    const std::array<SDL_FPoint, 4> positions{
        {{87, 96}, {1090, 100}, {1077, 342}, {float(int(walkingManX_)), float(int(walkingManY_))}}};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto& frame = catalog_.animation(names[index]);
        if (!frame.usable()) {
            continue;
        }
        renderer_.sprite(
            renderer_.image(frame.path),
            {positions[index].x, positions[index].y, float(frame.width), float(frame.height)},
            {float(frame.x + sceneAnimationFrames_[index] * frame.width),
             float(frame.y),
             float(frame.width),
             float(frame.height)},
            0,
            {255, 255, 255, 255},
            index == 1);
    }
}

AchievementDrawing Hangar::achievementDrawing(int family, int index) const {
    const auto badge = index < 3 ? std::to_string(index + 1) : index == 3 ? "V" : "Q";
    AchievementDrawing result;
    result.miniOn = "img/achievements/S_" + badge + "_on.png";
    result.miniOff = "img/achievements/S_" + badge + "_off.png";
    result.dimension = index < 3 ? 64 : 32;
    result.unlocked = achievements_[family][index];
    result.image =
        index < 3 ? "img/achievements/" + catalog_.ship(family, 0)->image + "_" + badge + ".png"
        : result.unlocked ? result.miniOn
                          : result.miniOff;
    result.fresh = freshAchievements_[family][index];
    result.victory = index == 3;
    result.twoLayouts = family >= 8;
    result.layouts = victoryLayouts_[family];
    return result;
}

void Hangar::shipList() {
    renderer_.draw(ui("ship_list_main_2"), 112, 110);
    text(texts_.get("shiplist_title", locale_), 359, 118, 63, false, ink);
    const auto shipButtons = buttons(false);
    // ShipSelect::OnInit515930@5170ce..517474: one seven-slice frame
    // behind all three Type buttons, starting three pixels above/left.
    auto& typeFrame = renderer_.image(ui("type_button_frame"));
    const auto& typeButton = shipButtons.front().rect;
    const float typeWidth = typeButton.w;
    const std::array<float, 7> sourceX{0, 12, 13, 12, 13, 37, 38};
    const std::array<float, 7> sourceWidth{12, 1, 24, 1, 24, 1, 12};
    const std::array<float, 7> destinationWidth{
        12, typeWidth - 19, 24, typeWidth - 20, 24, typeWidth - 19, 12};
    float frameX = typeButton.x - 3;
    for (std::size_t i = 0; i < sourceX.size(); ++i) {
        renderer_.sprite(typeFrame,
                         {frameX, typeButton.y - 3, destinationWidth[i], 28},
                         {sourceX[i], 0, sourceWidth[i], 28});
        frameX += destinationWidth[i];
    }
    for (const auto& b : shipButtons) {
        if (!b.id.starts_with("ship:")) {
            continue;
        }
        int f = std::stoi(b.id.substr(5));
        auto s = catalog_.ship(f, listType_);
        if (unlocked_[f][0] && s) {
            renderer_.draw(
                ui(hover_ == b.id ? "ship_list_button_lower_select2" : "ship_list_button_lower_on"),
                b.rect.x,
                b.rect.y + 107);
            for (int i = 0; i < (f == 0 || f == 3 || f == 9 ? 4 : 5); ++i) {
                const Rect rect{b.rect.x + 6 + 37 * i, b.rect.y + 123, 32, 32};
                renderer_.achievement(
                    achievementDrawing(f, i), {rect.x, rect.y}, rect.contains(mx_, my_) ? 2 : 3);
            }
        }
        renderer_.draw(ui(!s               ? "ship_list_button_off"
                          : hover_ == b.id ? "ship_list_button_select2"
                                           : "ship_list_button_on"),
                       b.rect.x,
                       b.rect.y);
        if (s) {
            auto& im = renderer_.image(ui("miniship_" + s->image));
            renderer_.draw(im, {b.rect.x, b.rect.y, float(im.w), float(im.h)});
            if (!b.enabled) {
                // ShipButton517c40 draws the ordinary mini ship first, then
                // its opaque black overlay; image alpha applies both times.
                renderer_.draw(im, {b.rect.x, b.rect.y, float(im.w), float(im.h)}, nullptr, 0.f);
                renderer_.draw(ui(hover_ == b.id ? "box_lock_selected" : "box_lock_on"),
                               b.rect.x + 90,
                               b.rect.y + 50);
            }
        } else {
            text(texts_.get("no_type_c", locale_), b.rect.x + 95, b.rect.y + 50, 12, true);
        }
    }
    // ShipSelect::Open 0x515930 and UnlockArrow 0x512130/0x512460.
    for (int i = 0; i < 7; ++i) {
        const int from = order[i];
        const int to = order[i + 1];
        if (unlocked_[to][0]) {
            continue;
        }
        int status = unlocked_[from][0] ? (from == 0 ? 2 : 1) : 0;
        bool down = i == 3;
        bool right = i < 3;
        auto asset = status == 2 ? "arrow_5_right"
                                 : std::string("arrow_") + (status ? "v_" : "lock_") +
                                       (down ? "down" : "left");
        float x = right  ? float(136 + 205 * i + 191)
                  : down ? 838.f
                         : float(751 - 205 * (i - 4) - 43);
        float y = right ? 227.f : down ? float(321 - (unlocked_[from][0] ? 0 : 20)) : 404.f;
        auto& im = renderer_.image(ui(asset));
        renderer_.sprite(im,
                         {x, y, float(im.w), float(im.h)},
                         {0, 0, float(im.w), float(im.h)},
                         0,
                         {255, 255, 255, 255},
                         right && status != 2);
    }
    // ShipSelect::OnRender518240 places its InfoBox before Type buttons.
    tooltip();
    for (const auto& b : shipButtons) {
        if (!b.id.starts_with("ship:")) {
            button(b);
        }
    }
}

void Hangar::render() {
    renderer_.clear();
    renderer_.setTint(list_ ? .25f : 1.f);
    renderer_.draw(ui("custom_background"), 0, 0);
    renderer_.draw(ui("box_shipequip"), 0, 478);
    renderer_.draw(ui("box_shipselect"), 0, 106);
    renderer_.draw(ui("box_shipach"), -1, 355);
    {
        // ShipBuilder::Open 0x4e77b6: preserve ends and separator, stretch the two button bays.
        auto& im = renderer_.image(ui("box_advanced"));
        float x = 980;
        for (auto [sx, sw, dw] : std::array<std::array<float, 3>, 5>{
                 {{0, 24, 24}, {24, 1, 89}, {115, 43, 43}, {158, 1, 82}, {243, 24, 24}}}) {
            Rect src{sx, 0, sw, float(im.h)};
            renderer_.draw(im, {x, 384, dw, float(im.h)}, &src);
            x += dw;
        }
    }
    text(texts_.get("hangar_frame_ship", locale_), 19, 117, 62, false, ink);
    text(texts_.get("hangar_frame_layout", locale_), 19, 219, 62, false, ink);
    text(texts_.get("hangar_frame_crew", locale_), 19, 489, 62, false, ink);
    text(texts_.get("equipment_frame_weapons", locale_), 388, 489, 62, false, ink);
    text(texts_.get("equipment_frame_drones", locale_), 388, 599, 62, false, ink);
    text(texts_.get("equipment_frame_augments", locale_), 954, 489, 62, false, ink);
    text(texts_.get("hangar_achievements_title", locale_), 124, 374, 13, true);
    text(texts_.get("hangar_advanced_title", locale_), 1111, 400, 13, true);
    if (!advanced_ && (family_ == 9 || type_ == 2)) {
        renderer_.setTint(.5f);
    }
    shipPreview();
    renderer_.draw(ui("box_start"), 963, 0);
    if (!advanced_ && (family_ == 9 || type_ == 2)) {
        renderer_.setTint(1);
        text(texts_.get("hangar_advanced_warning", locale_), 630, 213, 62, true);
    }
    sceneAnimations();
    renderer_.draw(ui("box_shipname"), 6, 8);
    equipment(false);
    equipment(true);
    if (customCrew_ >= 0) {
        renderer_.draw(ui("box_crew_details"), 100, 542);
        for (const auto& b : buttons()) {
            if (b.id == "crew_rename" || b.id == "crew_accept") {
                button(b);
            }
        }
        const auto& fullName = crew_.at(customCrew_).fullName;
        renderer_.textInput(
            fullName,
            caret_,
            14,
            {202, 599},
            editing_ == "crew" &&
                crewNameBlinks_.at(static_cast<std::size_t>(customCrew_)).renderElapsed < .5f,
            locale_ == "zh-Hans");
        const auto& f = catalog_.animation(crew_.at(customCrew_).species + "_walk_down");
        if (f.usable()) {
            crewSprite(crew_.at(customCrew_),
                       static_cast<float>(205 - 3 * (f.width / 2)),
                       static_cast<float>(579 - 3 * (f.height / 2)),
                       3);
        }
        if (editing_ == "crew") {
            text(texts_.get("rename", locale_), 197, 672, 12, true, {255, 255, 255, 255});
        }
    }
    if (customCrew_ >= 0) {
        for (const auto& b : buttons()) {
            if (b.id == "crew_previous" || b.id == "crew_next") {
                button(b);
            }
        }
    }
    systems();
    {
        bool list = list_;
        int crew = customCrew_;
        list_ = false;
        customCrew_ = -1;
        for (const auto& b : buttons()) {
            if (crew < 0 || !b.id.starts_with("crew:")) {
                button(b);
            }
        }
        list_ = list;
        customCrew_ = crew;
    }
    if (!list_ && encourageShipList_) {
        auto& arrow = renderer_.image("img/tutorial/arrow2.png");
        const Rect destination{145, 141, 164, 38};
        const Rect source{0, 0, float(arrow.w), float(arrow.h)};
        renderer_.sprite(arrow, destination, source, 0, {255, 0, 0, 255});
        const float alpha = 1.f - std::abs(float(listPulseSeconds_ / .375) - 1.f);
        renderer_.sprite(arrow,
                         destination,
                         source,
                         0,
                         {255, 255, 255, Uint8(std::clamp(alpha, 0.f, 1.f) * 255)});
        renderer_.windowFrame({315, 98, 337, 135});
        const auto body = texts_.get("tutorial_list_open", locale_);
        const auto lines =
            renderer_.wrappedLines(localizedFont(12, body),
                                   body,
                                   308,
                                   Renderer::sourceFontScale(12, locale_ == "zh-Hans"),
                                   locale_ == "zh-Hans");
        float y = 118;
        for (const auto& line : lines) {
            text(line, 335, y, 12, false, {255, 255, 255, 255});
            y += float(Renderer::sourceLineHeight(12, localizedFont(12, body)));
        }
    }
    renderer_.textInput(name_,
                        caret_,
                        24,
                        {273, 7},
                        editing_ == "ship" && shipNameBlink_.renderElapsed < .5f,
                        locale_ == "zh-Hans");
    if (editing_ == "ship") {
        const auto prompt = texts_.get("rename", locale_);
        const auto [left, right] = renderer_.bounds(
            localizedFont(12, prompt), prompt, Renderer::sourceFontScale(12, locale_ == "zh-Hans"));
        const float x = float(std::max(6, 227 - int(std::round(right - left)) / 2));
        text(prompt, x, 65, 12, false, {255, 255, 255, 255});
    }
    const auto& ship = selected();
    for (const auto [id, y] : std::array<std::pair<int, float>, 2>{{{3, 539.f}, {4, 649.f}}}) {
        if (std::none_of(ship.systems.begin(), ship.systems.end(), [&](const auto& system) {
                return system.installed && system.id == id;
            })) {
            text(texts_.get("equipment_no_system", locale_), 662, y, 63, true);
        }
    }
    for (int i = 0; i < 3; ++i) {
        float x = float(21 + i * 70);
        renderer_.achievement(achievementDrawing(family_, i),
                              {x, 404},
                              Rect{x, 404, 64, 64}.contains(mx_, my_) ? 1 : 0,
                              texts_.get("new_achievement", locale_),
                              locale_ == "zh-Hans");
    }
    renderer_.achievement(achievementDrawing(family_, 3),
                          {244, 438},
                          Rect{244, 438, 32, 32}.contains(mx_, my_) ? 2 : 3,
                          texts_.get("new_achievement", locale_),
                          locale_ == "zh-Hans");
    if (family_ != 0 && family_ != 3 && family_ != 9) {
        renderer_.achievement(achievementDrawing(family_, 4),
                              {244, 404},
                              Rect{244, 404, 32, 32}.contains(mx_, my_) ? 2 : 3,
                              texts_.get("new_achievement", locale_),
                              locale_ == "zh-Hans");
    }
    renderer_.setTint(1);
    if (list_) {
        renderer_.setTint(listTutorialPage_ >= 0 ? .5f : 1.f);
        shipList();
        renderer_.setTint(1);
        if (listTutorialPage_ >= 0) {
            listTutorial();
        }
    }
    if (!list_) {
        tooltip();
    }
}

void Hangar::renderOuter() {
    achievementTooltips();
    if (mx_ >= 0 && editing_.empty()) {
        renderer_.draw(hover_.empty() ? "img/mouse/pointerInvalid.png"
                                      : "img/mouse/pointerValid.png",
                       mx_,
                       my_);
    }
}

std::string Hangar::equipmentDescription(const EquipmentDefinition& e) {
    return e.descriptionText(texts_, locale_, advanced_);
}

std::string Hangar::systemLevelDescription(int system, int level) {
    // ShipSystem::GetLevelDescription 0x6192b0, zero-based power level.
    auto tr = [&](const std::string& id) {
        return texts_.get(id, locale_);
    };
    auto format = [&](const std::string& id, const std::string& a, const std::string& b = "") {
        auto s = tr(id);
        if (auto p = s.find("\\1"); p != std::string::npos) {
            s.replace(p, 2, a);
        }
        if (auto p = s.find("\\2"); p != std::string::npos) {
            s.replace(p, 2, b);
        }
        return s;
    };
    const int n = level + 1;
    switch (system) {
    case 0:
        return level % 2 ? tr("shields_" + std::to_string(level / 2)) : "";
    case 1: {
        std::ostringstream f;
        f << 1.f + level * .25f;
        return format(
            "engine", std::to_string(std::array{5, 10, 15, 20, 25, 28, 31, 35}.at(level)), f.str());
    }
    case 2:
        return format("oxygen_on", std::to_string(level ? level * 3 : 1));
    case 3:
    case 4:
        return tr("system_power");
    case 5:
        return format("medbay_healing", std::array<std::string, 3>{"1", "1.5", "3"}.at(level));
    case 6:
        return tr("pilot_" + std::to_string(n));
    case 7:
        return tr("sensor_" + std::to_string(n));
    case 8:
        return tr("door_" + std::to_string(n));
    case 9:
        return format("teleporter_on", std::to_string(25 - 5 * n));
    case 10:
        return format("cloak", std::to_string(5 * n));
    case 11:
        return tr("artillery_" + std::to_string(n));
    case 12:
        return format("battery_power", std::to_string(2 * n));
    case 13:
        return format("clone_full",
                      std::to_string(std::array{12, 9, 7}.at(level)),
                      std::to_string(std::array{8, 16, 25}.at(level)));
    case 14:
        return tr("mind_" + std::to_string(n));
    case 15:
        return format("hacking_duration", std::to_string(std::array{4, 7, 10}.at(level)));
    default:
        throw std::runtime_error("unsupported_system_description");
    }
}

void Hangar::systemPanel(const SystemDefinition& system) {
    const auto& bp = catalog_.system(system.name);
    // SetSystem54b810 / CalcCenterCount546de0 / MouseMove4dc9a0.
    const int maximum = system.maxPower >= 0 ? system.maxPower : bp.maxPower;
    const int current = std::min(bp.maxPower, std::max(1, system.power));
    const int count = renderer_.systemInfoCenterCount(
        system.id,
        maximum,
        [&](int level) {
            return systemLevelDescription(system.id, level);
        },
        locale_ == "zh-Hans");
    const auto& left = renderer_.image("img/upgradeUI/details_base_A.png");
    const auto& center = renderer_.image("img/upgradeUI/details_base_B.png");
    const auto& right = renderer_.image("img/upgradeUI/details_base_C.png");
    const int systemWidth = left.w + center.w * count + right.w;
    const float x = float(1282 - systemWidth);
    const float y = 230;
    const int shown = maximum + (system.id == 7 || system.id == 8 ? 1 : 0);
    const float rowWidth = float(left.w - 75 + count * center.w + 10);
    for (int row = 0; row < 8; ++row) {
        const int level = 7 - row;
        const float ry = y + 17 + 26 * row;
        renderer_.fill({x + 75, ry, rowWidth, 24},
                       row >= 8 - shown ? SDL_Color{127, 104, 107, 255}
                                        : SDL_Color{53, 43, 45, 255});
        if (row < 8 - shown) {
            continue;
        }
        renderer_.draw("img/upgradeUI/details_scrap.png", x + 75, ry);
        if (level >= current) {
            auto cost = level >= maximum ? " -" : std::to_string(bp.costs.at(level - 1));
            text(cost, x + 105, ry + 7, 0, false, {255, 255, 255, 255});
        }
        text(systemLevelDescription(system.id, level),
             x + 149,
             ry + 5,
             10,
             false,
             {255, 255, 255, 255});
    }
    renderer_.draw("img/upgradeUI/details_base_A.png", x, y);
    for (int tile = 0; tile < count; ++tile) {
        renderer_.draw(
            "img/upgradeUI/details_base_B.png", x + float(left.w + tile * center.w), y + 6);
    }
    renderer_.draw("img/upgradeUI/details_base_C.png", x + float(left.w + count * center.w), y + 6);
    for (int i = 0; i < maximum; ++i) {
        renderer_.fill({x + 27, y + 202 - 26 * i, 28, 18},
                       i < current ? (colorblind_ ? SDL_Color{243, 255, 238, 255}
                                                  : SDL_Color{100, 255, 100, 255})
                                   : SDL_Color{255, 255, 100, 64});
    }
    auto title = label(bp.title);
    auto body = label(bp.description);
    if (system.id == 11 && !system.weapon.empty()) {
        const auto& weapon = catalog_.equipment(system.weapon);
        title = label(weapon.title);
        body = label(weapon.description);
    }
    const int descriptionWidth = 305 + center.w * (count - 1);
    const auto lines = renderer_.wrappedLines(
        localizedFont(10, body), body, float(descriptionWidth), 1, locale_ == "zh-Hans");
    const auto warning = current == 1 && (system.id == 0 || system.id == 3)
                             ? texts_.get("weak_warning", locale_)
                             : std::string{};
    const auto warningLines = warning.empty() ? std::vector<std::string>{}
                                              : renderer_.wrappedLines(localizedFont(10, warning),
                                                                       warning,
                                                                       float(descriptionWidth),
                                                                       1,
                                                                       locale_ == "zh-Hans");
    const float bodyHeight = Renderer::sourceLineHeight(10, localizedFont(10, body));
    const float warningHeight = Renderer::sourceLineHeight(10, localizedFont(10, warning));
    const float height = std::max(135.f,
                                  63.f + bodyHeight * float(lines.size()) +
                                      warningHeight * float(warningLines.size()));
    const float frameY = 97 - (height - 135);
    renderer_.windowFrame({x, frameY, float(descriptionWidth + 32), height});
    float textY = frameY + 13;
    const auto titles = renderer_.wrappedLines(localizedFont(16, title),
                                               title,
                                               float(descriptionWidth),
                                               Renderer::sourceFontScale(16, locale_ == "zh-Hans"),
                                               locale_ == "zh-Hans");
    for (const auto& line : titles) {
        text(line, x + 18, textY, 16, false, {255, 255, 255, 255});
        textY += Renderer::sourceLineHeight(16, localizedFont(16, title));
    }
    textY += warningLines.empty() ? 15.f : 10.f;
    for (const auto& line : warningLines) {
        text(line, x + 18, textY, 10, false, {255, 120, 120, 255});
        textY += warningHeight;
    }
    for (const auto& line : lines) {
        text(line, x + 18, textY, 10, false, {255, 255, 255, 255});
        textY += bodyHeight;
    }
}

void Hangar::infoPanel(const std::string& title,
                       const std::string& body,
                       float height,
                       float x,
                       float y,
                       float width) {
    auto& bodyFont = localizedFont(10, body);
    auto& titleFont = localizedFont(16, title);
    const float titleScale = locale_ == "zh-Hans" ? 1.f : 2.f;
    const auto lines = renderer_.wrappedLines(bodyFont, body, width - 32, 1, locale_ == "zh-Hans");
    const auto titles =
        renderer_.wrappedLines(titleFont, title, width - 32, titleScale, locale_ == "zh-Hans");
    const float bodyHeight = Renderer::sourceLineHeight(10, bodyFont);
    const float titleHeight = Renderer::sourceLineHeight(16, titleFont);
    float w = width;
    float h = std::max(height, 63 + bodyHeight * float(lines.size()));
    renderer_.windowFrame({x, y, w, h});
    for (std::size_t index = 0; index < titles.size(); ++index) {
        text(titles[index],
             x + 18,
             y + 13 + titleHeight * float(index),
             16,
             false,
             {255, 255, 255, 255});
    }
    float ty = y + 28 + titleHeight * float(titles.size());
    for (const auto& line : lines) {
        text(line, x + 18, ty, 10, false, {255, 255, 255, 255});
        ty += bodyHeight;
    }
}

void Hangar::achievementTooltip(int family, int index) {
    std::string title;
    std::string body;
    bool multiDifficulty = false;
    if (index < 3) {
        const auto& a = catalog_.achievements(BlueprintCatalog::families[family]).at(index);
        title = label(a.title);
        body = label(a.description);
        multiDifficulty = a.multiDifficulty;
    } else {
        auto cls = label(catalog_.ship(family, 0)->shipClass);
        auto format = [&](const char* id) {
            auto s = texts_.get(id, locale_);
            if (auto p = s.find("\\1"); p != std::string::npos) {
                s.replace(p, 2, cls);
            }
            return s;
        };
        title = format(index == 3 ? "victory_ach_title" : "quest_ach_title");
        body = format(index == 3 ? "victory_ach" : "quest_ach");
    }
    if (index == 3) {
        std::string awarded;
        constexpr std::array<const char*, 3> difficulties{"easy", "normal", "hard"};
        for (int layout = 0; layout < 3; ++layout) {
            const int difficulty = victoryLayouts_[family][layout];
            if (difficulty < 0) {
                continue;
            }
            auto line = texts_.get("achieved_with", locale_);
            const auto replace = [&](const char* token, const std::string& value) {
                std::size_t position = 0;
                while ((position = line.find(token, position)) != std::string::npos) {
                    line.replace(position, 2, value);
                    position += value.size();
                }
            };
            replace("\\1", texts_.get("type_" + std::string(1, char('a' + layout)), locale_));
            replace("\\2", texts_.get(difficulties[std::size_t(difficulty)], locale_));
            awarded += (awarded.empty() ? "" : "\n") + line;
        }
        if (!awarded.empty()) {
            body += " \n\n" + awarded;
        }
    } else if (multiDifficulty && achievements_[family][index] &&
               achievementDifficulties_[family][index] >= 0) {
        constexpr std::array<const char*, 3> difficultyKeys{"ach_easy", "ach_normal", "ach_hard"};
        body += " \n\n" +
                texts_.get(difficultyKeys[std::size_t(achievementDifficulties_[family][index])],
                           locale_);
    }
    renderer_.tooltip(title, body, {mx_, my_}, 350, locale_ == "zh-Hans");
}

void Hangar::achievementTooltips() {
    if (list_) {
        if (listTutorialPage_ >= 0) {
            return;
        }
        // ShipSelect513bb0 checks unlock arrows before achievement/button
        // input and requests an instant generic tooltip for their full shape.
        for (std::size_t i = 0; i + 1 < order.size(); ++i) {
            if (unlocked_[order[i + 1]][0]) {
                continue;
            }
            const bool right = i < 3;
            const bool down = i == 3;
            const float x = right  ? 136.f + 205.f * float(i) + 191.f
                            : down ? 838.f
                                   : 751.f - 205.f * float(i - 4) - 43.f;
            const float y = right  ? 227.f
                            : down ? 321.f - (unlocked_[order[i]][0] ? 0.f : 20.f)
                                   : 404.f;
            if (!Rect{x, y, down ? 30.f : 43.f, down ? 85.f : 28.f}.contains(mx_, my_)) {
                continue;
            }
            auto body = texts_.get(i == 0 ? "ship_unlock_sector5" : "ship_unlock_victory", locale_);
            for (const auto& [token, value] : std::array<std::pair<const char*, std::string>, 2>{
                     {{"\\1", label(catalog_.ship(order[i], 0)->shipClass)},
                      {"\\2", label(catalog_.ship(order[i + 1], 0)->shipClass)}}}) {
                std::size_t position = 0;
                while ((position = body.find(token, position)) != std::string::npos) {
                    body.replace(position, 2, value);
                    position += value.size();
                }
            }
            renderer_.tooltip({}, body, {mx_, my_}, 0, locale_ == "zh-Hans");
            return;
        }
        for (const auto& b : buttons()) {
            if (!b.id.starts_with("ship:") || b.id != hover_) {
                continue;
            }
            const int family = std::stoi(b.id.substr(5));
            if (!unlocked_[family][0]) {
                continue;
            }
            for (int index = 0; index < (family == 0 || family == 3 || family == 9 ? 4 : 5);
                 ++index) {
                if (Rect{b.rect.x + 6 + 37 * index, b.rect.y + 123, 32, 32}.contains(mx_, my_)) {
                    achievementTooltip(family, index);
                    return;
                }
            }
        }
        return;
    }
    for (int index = 0; index < 3; ++index) {
        if (Rect{float(21 + 70 * index), 404, 64, 64}.contains(mx_, my_)) {
            achievementTooltip(family_, index);
            return;
        }
    }
    if (Rect{244, 438, 32, 32}.contains(mx_, my_)) {
        achievementTooltip(family_, 3);
    } else if (family_ != 0 && family_ != 3 && family_ != 9 &&
               Rect{244, 404, 32, 32}.contains(mx_, my_)) {
        achievementTooltip(family_, 4);
    }
}

void Hangar::tooltip() {
    if (list_) {
        for (const auto& b : buttons()) {
            if (b.id == hover_ && b.id.starts_with("ship:")) {
                int f = std::stoi(b.id.substr(5));
                auto s = catalog_.ship(f, listType_);
                if (!s) {
                    return;
                }
                auto format = [&](const char* key, const std::string& value) {
                    auto v = texts_.get(key, locale_);
                    if (auto p = v.find("\\1"); p != std::string::npos) {
                        v.replace(p, 2, value);
                    }
                    return v;
                };
                bool locked = !unlocked_[f][listType_];
                auto title =
                    f == 8 && locked
                        ? texts_.get("unidentified", locale_)
                        : label(s->shipClass) +
                              format("shipselect_type_suffix",
                                     texts_.get("type_" + std::string(1, char('a' + listType_)),
                                                locale_));
                auto body = label(locked ? s->unlock : s->description);
                if (locked && listType_ == 0) {
                    if (f == 8) {
                        body += " \n\n" +
                                format("alt_unlock", texts_.get("alt_unlock_crystal", locale_));
                    } else if (f != 0 && f != 3 && f != 9) {
                        const std::array<int, 10> previous{0, 6, 7, 0, 3, 2, 5, 4, 0, 0};
                        int before = previous[f];
                        body += " \n\n" +
                                format("alt_unlock",
                                       format(unlocked_[before][0] ? "alt_unlock_victory"
                                                                   : "alt_unlock_victory_locked",
                                              label(catalog_.ship(before, 0)->shipClass)));
                    }
                }
                infoPanel(title, body, locked && listType_ == 0 ? 176.f : 135.f, 453, 527, 374);
            }
        }
        return;
    }
    if (hover_ == "easy" || hover_ == "normal" || hover_ == "hard" || hover_ == "advanced_on" ||
        hover_ == "advanced_off") {
        infoPanel(texts_.get(hover_ + "_title", locale_), texts_.get(hover_ + "_text", locale_));
        return;
    }
    const auto& s = selected();
    if (customCrew_ < 0) {
        for (int i = 0; i < int(crew_.size()); ++i) {
            if (Rect{float(70 + 160 * (i % 2)), float(519 + 93 * (i / 2)), 100, 86}.contains(mx_,
                                                                                             my_)) {
                const auto& c = crew_[i];
                const auto& species = catalog_.appearance(c.species);
                auto body = label(species.description) + "\n";
                for (const auto& p : species.powers) {
                    body += "\n-" + label(p);
                }
                infoPanel(c.name + " (" + label(species.title) + ")", body, 182);
                return;
            }
        }
    }
    int systemIndex = 0;
    for (int id = 0; id < 16; ++id) {
        for (const auto& sys : s.systems) {
            if (sys.installed && sys.id == id) {
                if (Rect{float(373 + 38 * systemIndex), 400, 38, 76}.contains(mx_, my_)) {
                    systemPanel(sys);
                    return;
                }
                ++systemIndex;
            }
        }
    }
    const auto weapons = armory(s, false);
    for (int i = 0; i < int(weapons.size()); ++i) {
        if (Rect{435.f + 58 * (4 - s.weaponSlots) + 117 * i, 514, 109, 76}.contains(mx_, my_)) {
            const auto& e = catalog_.equipment(weapons[i]);
            infoPanel(label(e.title), equipmentDescription(e), 266);
            return;
        }
    }
    const auto drones = armory(s, true);
    for (int i = 0; i < int(drones.size()); ++i) {
        if (Rect{435.f + 58 * (4 - s.droneSlots) + 117 * i, 624, 109, 76}.contains(mx_, my_)) {
            const auto& e = catalog_.equipment(drones[i]);
            infoPanel(label(e.title), equipmentDescription(e), 176);
            return;
        }
    }
    for (int i = 0; i < int(s.augments.size()); ++i) {
        if (Rect{988, float(529 + 60 * i), 234, 39}.contains(mx_, my_)) {
            const auto& e = catalog_.equipment(s.augments[i]);
            infoPanel(label(e.title), label(e.description));
            return;
        }
    }
}

nlohmann::json Hangar::state() {
    auto bs = nlohmann::json::array();
    for (const auto& b : buttons()) {
        bs.push_back({{"id", b.id},
                      {"hitbox", {b.rect.x, b.rect.y, b.rect.w, b.rect.h}},
                      {"enabled", b.enabled},
                      {"selected", b.selected}});
    }
    auto cs = nlohmann::json::array();
    for (int i = 0; i < int(crew_.size()); ++i) {
        const auto& c = crew_[i];
        cs.push_back({{"id", i},
                      {"name", c.name},
                      {"full_name", c.fullName},
                      {"species", c.species},
                      {"appearance", c.appearance},
                      {"room", c.room},
                      {"slot", c.slot},
                      {"shoot_goal", c.shootGoal},
                      {"shoot_elapsed", c.shootElapsed}});
    }
    return {{"scene", "hangar"},
            {"main_menu_open", true},
            {"hangar_open", open_},
            {"user_pause", false},
            {"modal_pause", false},
            {"simulation_tick", 0},
            {"animation_tick", tick_},
            {"ship_list_open", list_},
            {"list_type", listType_},
            {"opened_list", openedList_},
            {"ship_list_tutorial_page", listTutorialPage_},
            {"ship_list_tutorial_open_seconds", tutorialOpenSeconds_},
            {"ship_list_encouragement", encourageShipList_},
            {"family", family_},
            {"type", type_},
            {"difficulty", difficulty_},
            {"advanced", advanced_},
            {"show_rooms", rooms_},
            {"ship_name", name_},
            {"crew", cs},
            {"weapon_factories", weaponFactories_},
            {"artillery_factories", artilleryFactories_},
            {"drone_factories", droneFactories_},
            {"drone_trash", droneTrash_},
            {"hacking_factory", hackingFactory_},
            {"explosion_state", explosionState_},
            {"engine_animation_times", engineAnimationTimes_},
            {"room_fire_animation_times", roomFireAnimationTimes_},
            {"medbay_animation_times", medbayAnimationTimes_},
            {"fire_spreader_animations", fireSpreaderAnimations_},
            {"editing", editing_},
            {"caret_byte", caret_},
            {"customizing_crew", customCrew_},
            {"blueprint", selected().snapshot()},
            {"buttons", bs},
            {"launch_requested", start_},
            {"start_enabled", advanced_ || (family_ != 9 && type_ != 2)},
            {"gameplay_available", false}};
}
} // namespace ftl
