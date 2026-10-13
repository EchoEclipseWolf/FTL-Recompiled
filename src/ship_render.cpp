#include "ship_render.hpp"
#include "combat.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace ftl {
void ShipPainter::draw(const ShipDefinition& s,
                       const std::vector<HangarCrew>& crew_,
                       const ShipPaintState& v,
                       const std::function<void()>& floorEffects,
                       const std::function<void()>& wallEffects,
                       const std::function<void()>& crewBodies,
                       const std::function<void()>& crewHealth) {
    float x = v.x + s.horizontalShift;
    float y = v.y + s.verticalShift;
    // ShipGraph::ComputeCenter45aca0 supplies the room-bound corner to
    // OnRenderBase482070/GetShipCorner481f70. XML image coordinates are local
    // to that corner; room and door coordinates already contain layout offsets.
    int shipLeft = 0;
    int shipTop = 0;
    if (!s.rooms.empty()) {
        shipLeft = s.rooms.front().x;
        shipTop = s.rooms.front().y;
        for (const auto& room : s.rooms) {
            shipLeft = std::min(shipLeft, room.x);
            shipTop = std::min(shipTop, room.y);
        }
    }
    const float imageX = x + shipLeft + s.imageRect[0];
    const float imageY = y + shipTop + s.imageRect[1];
    const bool player = s.id.starts_with("PLAYER_SHIP");
    if (v.jumpProgress >= 0) {
        // Ship::OnRenderJump0x481950. The hull fades in place; a glare crosses its bounds.
        renderer_.draw((player ? "img/ship/" : "img/ships_glow/") + s.image + "_base.png",
                       imageX,
                       imageY,
                       1,
                       1 - v.jumpProgress,
                       true);
        const float phase = std::clamp(v.jumpProgress / .75f, 0.f, 1.f);
        const float extent = phase <= 0 || phase >= 1 ? 0.f
                             : phase < .5f            ? phase * 1200 + 64
                                                      : 600 - (phase - .5f) * 1200;
        if (extent > 0) {
            int left = 10000;
            int top = 10000;
            int right = 0;
            int bottom = 0;
            for (const auto& room : s.rooms) {
                left = std::min(left, room.x);
                top = std::min(top, room.y);
                right = std::max(right, room.x + room.w);
                bottom = std::max(bottom, room.y + room.h);
            }
            auto& glare = renderer_.image("img/effects/glowing_star.png");
            renderer_.draw(
                glare,
                {x + (player ? left + phase * (right - left) : (left + right) / 2.f) - extent / 2,
                 y + (player ? (top + bottom) / 2.f : bottom - phase * (bottom - top)) - extent / 2,
                 extent,
                 extent});
        }
        if (v.enginesOn && s.image.starts_with("kestral")) {
            for (int engine = 0; engine < 2; ++engine) {
                renderer_.sprite(
                    renderer_.image("img/effects/thrusters_on.png"),
                    {imageX + 60, imageY + (engine ? 305 : 39), 22, 70},
                    {float(
                         std::clamp(int(v.engineAnimationTimes[std::size_t(engine)] * 8.f), 0, 3)) *
                         22,
                     0,
                     22,
                     70},
                    0,
                    {255, 255, 255, Uint8((1 - phase) * 255.f)});
            }
        }
        return;
    }

    // ExplosionAnimation::Update 0x489930 sets bFinalBoom strictly after
    // 1.5 seconds; OnRender 0x47e490 then omits the intact hull primitive.
    // The detached weapon mounts are drawn with their owning pieces below.
    if (v.destructionAge > 1.5f) {
        drawDestructionGibs(s, v, x, y);
        return;
    }

    struct MountedWeapon {
        std::size_t mount;
        std::string id;
        int frame;
        const CombatWeapon* state;
    };

    std::vector<MountedWeapon> mounted;
    for (std::size_t i = 0; i < std::min(s.weapons.size(), s.mounts.size()); ++i) {
        const auto& e = catalog_.equipment(s.weapons[i]);
        const auto slot = std::ranges::find(v.weaponSlotIndices, static_cast<int>(i));
        const auto mount = v.weaponSlotIndices.empty() ? i
                           : slot == v.weaponSlotIndices.end()
                               ? s.mounts.size()
                               : static_cast<std::size_t>(slot - v.weaponSlotIndices.begin());
        if (mount >= s.mounts.size()) {
            continue;
        }
        mounted.push_back(
            {mount,
             s.weapons[i],
             i < v.weaponFrames.size() ? v.weaponFrames[i]
             : v.active                ? 0
                                       : e.chargedFrame,
             v.weaponStates && i < v.weaponStates->size() ? &(*v.weaponStates)[i] : nullptr});
    }
    std::size_t artilleryIndex = 0;
    for (const auto& system : s.systems) {
        if (system.id != 11 || !system.installed) {
            continue;
        }
        // RenderWeapons49a960 pads the ordinary factory list to four slots.
        const auto mount = std::max(std::size_t(4), s.weapons.size()) + artilleryIndex;
        const auto& e = catalog_.equipment(system.weapon);
        if (mount < s.mounts.size()) {
            mounted.push_back(
                {mount,
                 system.weapon,
                 artilleryIndex < v.artilleryFrames.size() ? v.artilleryFrames[artilleryIndex]
                 : v.active                                ? 0
                                                           : e.chargedFrame,
                 artilleryIndex < v.artilleryStates.size() ? v.artilleryStates[artilleryIndex]
                                                           : nullptr});
        }
        ++artilleryIndex;
    }
    for (const auto& weapon : mounted) {
        if (v.destructionAge < 0) {
            drawMountedWeapon(catalog_.equipment(weapon.id),
                              weapon.state,
                              weapon.frame,
                              s.mounts[weapon.mount],
                              {imageX, imageY},
                              v.deployment,
                              v.cloaked);
        }
    }
    drawDestructionGibs(s, v, x, y);
    const auto hullPath =
        (s.id.starts_with("PLAYER_SHIP") ? "img/ship/" : "img/ships_glow/") + s.image + "_base.png";
    // Ship::GetCloakAlpha 0x481ee0 retains half-alpha exterior visibility.
    renderer_.draw(hullPath, imageX, imageY, 1, v.cloaked ? .5f : 1.f);
    if (v.destructionAge >= 0) {
        // ShipManager::OnRender 0x49abc0 selects OnRenderExplosion and skips
        // the ordinary cloak, engines, floor, rooms and crew render path.
        return;
    }
    if (v.cloaked) {
        const auto cloakPath =
            "img/ship/" + (s.cloakImage.empty() ? s.image : s.cloakImage) + "_cloak.png";
        renderer_.draw(cloakPath, imageX + s.cloakOffset[0], imageY + s.cloakOffset[1], 1, .5f);
    }
    // The player hull floor is part of OnRenderBase482070, before engines
    // and the interior-visibility guard. Enemy ships have no floor primitive.
    if (player) {
        renderer_.draw("img/ship/" + s.floorImage + "_floor.png",
                       imageX + s.floorOffset[0],
                       imageY + s.floorOffset[1],
                       1,
                       v.cloaked ? .5f : 1.f);
    }
    if (v.enginesOn && (s.image.starts_with("kestral") || s.image.starts_with("fed_cruiser"))) {
        const bool fed = s.image.starts_with("fed_cruiser");
        for (int i = 0; i < 2; ++i) {
            renderer_.sprite(
                renderer_.image("img/effects/thrusters_on.png"),
                {imageX + (fed ? 17 : 60),
                 imageY + (fed ? (i ? 285 : 44) : (i ? 305 : 39)),
                 22,
                 70},
                {float(std::clamp(int(v.engineAnimationTimes[std::size_t(i)] * 8.f), 0, 3)) * 22,
                 0,
                 22,
                 70});
        }
    }
    if (!v.rooms || (v.cloaked && v.hostile && !v.containsPlayerCrew)) {
        return;
    }
    const bool sensors = v.sensorsWorking;
    auto visible = [&](int room) {
        if (v.explicitVision) {
            return sensors || std::ranges::find(v.visionRooms, room) != v.visionRooms.end();
        }
        return sensors || std::any_of(crew_.begin(), crew_.end(), [&](const auto& c) {
                   return c.room == room;
               });
    };
    for (const auto& room : s.rooms) {
        int oxygenTint = v.active && v.roomOxygen.contains(room.id)
                             ? int((100 - std::clamp(v.roomOxygen.at(room.id), 0.f, 100.f)) * .25f)
                             : 0;
        renderer_.fill({x + room.x, y + room.y, float(room.w), float(room.h)},
                       visible(room.id) ? SDL_Color{Uint8(230 + oxygenTint),
                                                    Uint8(226 - 2 * oxygenTint),
                                                    Uint8(219 - 2 * oxygenTint),
                                                    255}
                                        // Room::Room 0x4689a2 blackoutPrimitive: 55/255 grey.
                                        : SDL_Color{55, 55, 55, 255});
    }
    for (const auto& room : s.rooms) {
        if (v.active && visible(room.id) && v.roomOxygen.contains(room.id) &&
            v.roomOxygen.at(room.id) <= 5) {
            renderer_.draw("img/effects/low_o2_stripes_" +
                               std::string(room.w <= 35   ? "1x2"
                                           : room.h <= 35 ? "2x1"
                                                          : "2x2") +
                               ".png",
                           x + room.x,
                           y + room.y,
                           1,
                           .5f);
        }
    }
    for (const auto& room : s.rooms) {
        if (!visible(room.id)) {
            continue;
        }
        for (int a = 35; a < room.w; a += 35) {
            renderer_.fill({x + room.x + a - 1, y + room.y, 1, float(room.h)},
                           {174, 169, 163, 255});
        }
        for (int b = 35; b < room.h; b += 35) {
            renderer_.fill({x + room.x, y + room.y + b - 1, float(room.w), 1},
                           {174, 169, 163, 255});
        }
    }
    for (const auto& sys : s.systems) {
        if (sys.installed && sys.power > 0) {
            const auto room = std::find_if(s.rooms.begin(), s.rooms.end(), [&](const auto& a) {
                return a.id == sys.room;
            });
            if (room == s.rooms.end()) {
                continue;
            }
            // ShipSystem613ff0 effects precede this system's station image
            // in ShipManager49abc0's interior pass.
            const auto effects = v.damagingEffectTimes.find(sys.room);
            if (sys.id == 5 && visible(room->id) && effects != v.damagingEffectTimes.end()) {
                auto& hurt = renderer_.image("img/people/healing_hurt_strip11.png");
                const int rows = room->h / 35;
                for (int column = 0; column < room->w / 35; ++column) {
                    for (int row = 0; row < rows; ++row) {
                        const auto tile =
                            std::size_t(column) * std::size_t(rows) + std::size_t(row);
                        if (tile >= effects->second.size()) {
                            continue;
                        }
                        const int frame = std::clamp(int(effects->second[tile] * 11.f), 0, 10);
                        renderer_.sprite(
                            hurt,
                            {x + room->x + column * 35.f, y + room->y + row * 35.f, 32, 32},
                            {float(frame * 32), 0, 32, 32});
                    }
                }
            }
            if (sys.image.empty()) {
                continue;
            }
            auto path = "img/ship/interior/" + sys.image + ".png";
            const bool playerInterior = s.id.starts_with("PLAYER_SHIP");
            if (playerInterior && visible(room->id)) {
                renderer_.draw(path, x + room->x, y + room->y, 1, v.active ? .75f : 1.f);
            }
            if (!playerInterior && visible(room->id) && sys.slot >= 0) {
                // Room::OnRenderSystem 0x46a8f0 draws a single station console
                // on hostile ships; their full player-room interior art is absent.
                const int columns = room->w / 35;
                const Rect station{x + room->x + float(sys.slot % columns * 35),
                                   y + room->y + float(sys.slot / columns * 35),
                                   35,
                                   35};
                // Room::OnRenderSystem 0x46a8f0 uses the unrotated strip for
                // an upper wall (direction 2), rotating lower walls 180°.
                const double angle = sys.direction == "DOWN"    ? 180
                                     : sys.direction == "RIGHT" ? 90
                                     : sys.direction == "LEFT"  ? 270
                                                                : 0;
                auto& console = renderer_.image("img/ship/interior/computer1.png");
                renderer_.sprite(console, station, {0, 0, 35, 35}, angle);
                if (v.active && v.computerLevels.contains(sys.id) &&
                    v.computerLevels.at(sys.id) > 0) {
                    const auto glow = "img/ship/interior/computer1_glow" +
                                      std::to_string(std::min(3, v.computerLevels.at(sys.id))) +
                                      ".png";
                    auto& overlay = renderer_.image(glow);
                    renderer_.sprite(overlay, station, {0, 0, 35, 35}, angle);
                }
            }
            if (playerInterior && visible(room->id) && v.active &&
                v.computerLevels.contains(sys.id) && v.computerLevels.at(sys.id) > 0 &&
                !sys.glowName.empty()) {
                auto glow = "img/ship/interior/" + sys.glowName +
                            std::to_string(v.computerLevels.at(sys.id)) + ".png";
                auto& im = renderer_.image(glow);
                float gx = x + room->x + sys.glowX;
                float gy = y + room->y + sys.glowY;
                double angle = sys.glowDirection == "UP"      ? 180
                               : sys.glowDirection == "RIGHT" ? 90
                               : sys.glowDirection == "LEFT"  ? 270
                                                              : 0;
                if (angle == 90 || angle == 270) {
                    gx -= (im.w - im.h) * .5f;
                    gy += (im.w - im.h) * .5f;
                }
                renderer_.sprite(im,
                                 {float(int(gx)), float(int(gy)), float(im.w), float(im.h)},
                                 {0, 0, float(im.w), float(im.h)},
                                 angle,
                                 {255, 255, 255, 191});
            }
        }
    }
    // ShipManager::OnRender 0x49abc0 draws breaches/fire before paths,
    // highlights, walls and crew, after floor and station artwork.
    if (floorEffects) {
        floorEffects();
    }
    for (auto target : v.targets) {
        renderer_.draw("img/people/green_destination.png", x + target.x - 17, y + target.y - 17);
    }
    if (v.hoverRoom >= 0 || !v.highlightRooms.empty()) {
        for (const auto& room : s.rooms) {
            if (room.id == v.hoverRoom ||
                std::ranges::find(v.highlightRooms, room.id) != v.highlightRooms.end()) {
                auto outline = [&](float inset, float thickness, SDL_Color color) {
                    float rx = x + room.x + inset;
                    float ry = y + room.y + inset;
                    float rw = room.w - 2 * inset;
                    float rh = room.h - 2 * inset;
                    for (auto edge :
                         std::array<Rect, 4>{{{rx, ry, rw, thickness},
                                              {rx, ry + rh - thickness, rw, thickness},
                                              {rx, ry, thickness, rh},
                                              {rx + rw - thickness, ry, thickness, rh}}}) {
                        renderer_.fill(edge, color);
                    }
                };
                outline(0, 5, {255, 255, 0, 255});
                outline(5, 1, {255, 188, 0, 255});
            }
        }
    }
    // Ship::OnInit 0x48db81: two-pixel walls lie inside each room,
    // with a nineteen-pixel opening centred in a door's 35-pixel segment.
    for (const auto& room : s.rooms) {
        for (int side = 0; side < 4; ++side) {
            bool vertical = side < 2;
            int length = vertical ? room.h : room.w;
            int boundary =
                vertical ? room.x + (side == 1 ? room.w : 0) : room.y + (side == 3 ? room.h : 0);
            int start = vertical ? room.y : room.x;
            for (int offset = 0; offset < length; offset += 35) {
                bool door = std::any_of(s.doors.begin(), s.doors.end(), [&](const auto& d) {
                    return d.vertical == vertical && (vertical ? d.x : d.y) == boundary &&
                           (vertical ? d.y : d.x) == start + offset;
                });
                auto line = [&](int a, int count) {
                    if (vertical) {
                        renderer_.fill({x + boundary - (side == 1 ? 2 : 0),
                                        y + start + offset + a,
                                        2,
                                        float(count)},
                                       {0, 0, 0, 255});
                    } else {
                        renderer_.fill({x + start + offset + a,
                                        y + boundary - (side == 3 ? 2 : 0),
                                        float(count),
                                        2},
                                       {0, 0, 0, 255});
                    }
                };
                if (door) {
                    line(0, 8);
                    line(27, 8);
                } else {
                    line(0, 35);
                }
            }
        }
    }
    const auto doorSystem =
        std::find_if(s.systems.begin(), s.systems.end(), [](const auto& system) {
            return system.id == 8 && system.installed;
        });
    const int doorLevel = v.doorLevel >= 0                ? v.doorLevel
                          : doorSystem != s.systems.end() ? doorSystem->power
                                                          : 0;
    const auto perDoor = [](const std::vector<int>& values, int id) {
        return id >= 0 && id < int(values.size()) ? values[std::size_t(id)] : -1;
    };
    for (const auto& d : s.doors) {
        const int level =
            perDoor(v.doorLevels, d.id) >= 0 ? perDoor(v.doorLevels, d.id) : doorLevel;
        const bool hacked =
            perDoor(v.doorHacks, d.id) >= 0 ? perDoor(v.doorHacks, d.id) > 0 : v.doorsHacked;
        // Door::UpdateAnimations 0x4665a0 uses bottom-origin strip rows; Animation
        // 0x59c5f0 inverts texture V. Convert those rows to SDL's top-origin rectangles.
        const float doorRow = hacked ? 140.f : float((std::clamp(level, 1, 4) - 1) * 35);
        // Layout coordinates identify the tile boundary; Door constructor adds half a tile along
        // it.
        float dx = x + d.x + (d.vertical ? 0 : 17);
        float dy = y + d.y + (d.vertical ? 17 : 0);
        if (d.id == v.hoverDoor) {
            renderer_.sprite(renderer_.image("img/effects/door_highlight.png"),
                             {dx - 17, dy - 17, 35, 35},
                             {0, 0, 35, 35},
                             d.vertical ? 0 : 90,
                             {255, 255, 255, 191});
        }
        renderer_.sprite(renderer_.image("img/effects/door_sheet.png"),
                         {dx - 17, dy - 17, 35, 35},
                         {float(d.id < int(v.doorFrames.size()) ? int(v.doorFrames[d.id]) * 35
                                : d.id < int(v.doors.size()) && v.doors[d.id] ? 4 * 35
                                                                              : 0),
                          doorRow,
                          35,
                          35},
                         d.vertical ? 0 : 90,
                         // Door::OnRender 0x466bb0: gotHit red, else doorLevel 0
                         // (no door system, or broken/unpowered) tints (1,.6,.6).
                         d.id < int(v.doorHit.size()) && v.doorHit[d.id] ? SDL_Color{255, 0, 0, 255}
                         : level == 0 ? SDL_Color{255, 153, 153, 255}
                                      : SDL_Color{255, 255, 255, 255});
    }
    if (wallEffects) {
        wallEffects();
    }
    // ShipManager49abc0 draws system icons after walls/lockdown and before actors.
    for (const auto& sys : s.systems) {
        if (!sys.installed || sys.power <= 0) {
            continue;
        }
        const auto room = std::ranges::find(s.rooms, sys.room, &RoomDefinition::id);
        if (room == s.rooms.end()) {
            continue;
        }
        auto icon = "img/icons/s_" + sys.name + "_overlay.png";
        auto& image = renderer_.image(icon);
        // AnimationTracker::GetAlphaLevel 0x59c490 over the 0.75 s flashTracker.
        const float flashAlpha = (.375f - std::abs(v.systemFlash - .375f)) / .375f;
        renderer_.sprite(image,
                         {x + room->x + room->w / 2 - 16,
                          y + room->y + room->h / 2 - 16,
                          float(image.w),
                          float(image.h)},
                         {0, 0, float(image.w), float(image.h)},
                         0,
                         // ShipSystem::OnRender 0x6141d0 compares healthState with
                         // the live system level, not the blueprint's starting power.
                         // Locked/strongly hacked icons flash ion blue while the
                         // flashTracker alpha level is above one half.
                         v.systemLocked.contains(sys.id) && v.systemLocked.at(sys.id) &&
                                 flashAlpha > .5f
                             ? SDL_Color{133, 231, 237, 255}
                         : v.systemHealth.contains(sys.id) && v.systemHealth.at(sys.id) == 0
                             ? SDL_Color{255, 50, 50, 255}
                         : v.systemHealth.contains(sys.id) &&
                                 v.systemHealth.at(sys.id) < (v.systemCapacity.contains(sys.id)
                                                                  ? v.systemCapacity.at(sys.id)
                                                                  : sys.power)
                             ? SDL_Color{255, 160, 0, 255}
                             : SDL_Color{153, 153, 153, 255});
        // ShipSystem::OnRender 0x6141d0 keeps the base icon and adds its
        // separate border primitive: hacking purple while a hack drone is
        // attached (iHackEffect > 0), else yellow while selected.
        if (v.systemHackEffect.contains(sys.id) && v.systemHackEffect.at(sys.id) > 0) {
            auto& border = renderer_.image("img/icons/s_" + sys.name + "_overlay2.png");
            renderer_.sprite(border,
                             {x + room->x + room->w / 2 - 16,
                              y + room->y + room->h / 2 - 16,
                              float(border.w),
                              float(border.h)},
                             {0, 0, float(border.w), float(border.h)},
                             0,
                             {207, 70, 253, 255});
        } else if (sys.id == v.hoverSystem) {
            auto& border = renderer_.image("img/icons/s_" + sys.name + "_overlay2.png");
            renderer_.sprite(border,
                             {x + room->x + room->w / 2 - 16,
                              y + room->y + room->h / 2 - 16,
                              float(border.w),
                              float(border.h)},
                             {0, 0, float(border.w), float(border.h)},
                             0,
                             {255, 255, 50, 255});
        }
    }
    const auto drawCrewBody = [&](std::size_t index) {
        const auto& c = crew_[index];
        auto room = std::find_if(s.rooms.begin(), s.rooms.end(), [&](const auto& a) {
            return a.id == c.room;
        });
        const bool silhouette = index < v.silhouetteCrew.size() && v.silhouetteCrew[index];
        if (room == s.rooms.end() || (!visible(room->id) && !silhouette)) {
            return;
        }
        int cols = std::max(1, room->w / 35);
        auto path =
            "img/people/" + (c.species == "engi" ? std::string("energy") : c.species) + "_base.png";
        if (index < v.crewHealth.size() && v.crewHealth[index] <= 0 &&
            (index >= v.crewAnimations.size() || !v.crewAnimations[index].starts_with("death_"))) {
            return;
        }
        float cx = static_cast<float>(room->x + 35 * (c.slot % cols));
        float cy = static_cast<float>(room->y + 35 * (c.slot / cols));
        if (index < v.crewPositions.size()) {
            cx = v.crewPositions[index].x - 17.f;
            cy = v.crewPositions[index].y - 17.f;
        }
        // CrewMember::OnLoop 0x4747f0 calls CrewAnimation::SetDirection 0x473c50 at
        // its LABEL_118 with `currentTarget` — the opposing CrewTarget the member is
        // fighting — immediately before the animation update, so a fighting member
        // faces its opponent instead of the direction it last walked.
        std::string animation =
            index < v.crewAnimations.size() ? v.crewAnimations[index] : std::string("walk_down");
        // ShipManager49b3a0..49b3bc draws the tutorial circle before this member.
        if (v.crewHighlightAlpha > 0 && (index >= v.crewHealth.size() || v.crewHealth[index] > 0)) {
            auto& circle = renderer_.image("img/tutorial/player_circle.png");
            renderer_.sprite(circle,
                             {x + cx + 2, y + cy + 3, circle.w * .75f, circle.h * .75f},
                             {0, 0, static_cast<float>(circle.w), static_cast<float>(circle.h)},
                             0,
                             {255, 255, 255, v.crewHighlightAlpha});
        }
        if (index < v.crewUnderlays.size() && v.crewUnderlays[index]) {
            v.crewUnderlays[index](x + cx, y + cy);
        }
        crewSprite(c,
                   x + cx,
                   y + cy,
                   1,
                   index < v.selected.size() && v.selected[index],
                   animation,
                   index < v.crewTicks.size() ? v.crewTicks[index] : 0,
                   index < v.hostileCrew.size() && v.hostileCrew[index],
                   v.colorblind,
                   index < v.stunnedCrew.size() && v.stunnedCrew[index],
                   true,
                   true,
                   index < v.silhouetteCrew.size() && v.silhouetteCrew[index]);
    };
    if (v.orderedPhysicalCrew) {
        auto physical = v.visitingCrew;
        for (std::size_t index = 0; index < crew_.size(); ++index) {
            const auto order =
                index < v.crewPhysicalOrders.size() ? v.crewPhysicalOrders[index] : 0;
            const auto health = index < v.crewHealthRenderers.size() ? v.crewHealthRenderers[index]
                                                                     : std::function<void()>{};
            physical.push_back({order,
                                [&, index] {
                                    drawCrewBody(index);
                                },
                                health});
        }
        std::stable_sort(
            physical.begin(), physical.end(), [](const auto& first, const auto& second) {
                return first.physicalOrder < second.physicalOrder;
            });
        for (const auto& actor : physical) {
            if (actor.body) {
                actor.body();
            }
        }
        for (const auto& actor : physical) {
            if (actor.health) {
                actor.health();
            }
        }
        return;
    }
    for (std::size_t index = 0; index < crew_.size(); ++index) {
        drawCrewBody(index);
    }
    if (crewBodies) {
        crewBodies();
    }
    // All physical bodies precede the separate props/health/overlay traversal.
    if (crewHealth) {
        crewHealth();
        return;
    }
    for (const auto& c : crew_) {
        const auto index = std::size_t(&c - crew_.data());
        const auto room = std::ranges::find(s.rooms, c.room, &RoomDefinition::id);
        if (room == s.rooms.end()) {
            continue;
        }
        const int cols = std::max(1, room->w / 35);
        float cx = float(room->x + 35 * (c.slot % cols));
        float cy = float(room->y + 35 * (c.slot / cols));
        if (index < v.crewPositions.size()) {
            cx = v.crewPositions[index].x - 17.f;
            cy = v.crewPositions[index].y - 17.f;
        }
        const float healthAlpha =
            index < v.crewHealthAlpha.size()
                ? v.crewHealthAlpha[index]
                : (index < v.selected.size() && v.selected[index] ? 1.f : 0.f);
        if (v.active && healthAlpha > 0) {
            renderer_.draw(index < v.crewHealthFlash.size() && v.crewHealthFlash[index]
                               ? "img/people/health_box_red.png"
                               : "img/people/health_box.png",
                           x + cx + 2,
                           y + cy - 3,
                           1.f,
                           healthAlpha);
            renderer_.fill(
                {x + cx + 7,
                 y + cy,
                 std::floor(25 * (index < v.crewHealth.size()
                                      ? std::clamp(v.crewHealth[index] / 100, 0.f, 1.f)
                                      : 1.f)),
                 3},
                index < v.hostileCrew.size() && v.hostileCrew[index]
                    ? (v.colorblind ? SDL_Color{0, 147, 224, 255} : SDL_Color{255, 0, 0, 255})
                    : SDL_Color{0, 255, 0, 255});
        }
    }
}

void ShipPainter::drawMountedWeapon(const EquipmentDefinition& e,
                                    const CombatWeapon* state,
                                    int retainedFrame,
                                    const WeaponMount& m,
                                    SDL_FPoint base,
                                    float deployment,
                                    bool cloaked,
                                    float rotation,
                                    const SDL_FPoint* rotationCenter) {
    if (e.imagePath.empty() || e.frameWidth <= 0 || e.frameHeight <= 0) {
        return;
    }
    SDL_FPoint pivot{float(m.mirror ? e.frameWidth - e.mountX : e.mountX), float(e.mountY)};
    // GetSlide4570c0 retains each factory's half-second powered slide.
    const float left = 1.f - std::clamp(state ? state->animationSlide / .5f : deployment, 0.f, 1.f);
    float dx = 0;
    float dy = 0;
    // Globals::GetDirection 0x5a8ac0 maps authored up/down/left/right to
    // 2/0/3/1. GetSlide 0x4570c0 then moves in the opposite screen direction
    // while retracted, so the weapon withdraws into its mount.
    const std::string& slide = m.slide;
    if (slide == "right") {
        dx = -float((m.rotate ? e.frameHeight : e.frameWidth) / 2) * left;
    } else if (slide == "left") {
        dx = float((m.rotate ? e.frameHeight : e.frameWidth) / 2) * left;
    } else if (slide == "up") {
        dy = float((m.rotate ? e.frameWidth : e.frameHeight) / 2) * left;
    } else if (slide == "down") {
        dy = -float((m.rotate ? e.frameWidth : e.frameHeight) / 2) * left;
    }
    const SDL_FPoint origin{base.x + m.x - pivot.x + std::round(dx),
                            base.y + m.y - pivot.y + std::round(dy)};
    const auto drawLayer =
        [&](Texture& image, SDL_FPoint position, Rect source, Uint8 alpha, bool mirrored) {
            const SDL_FPoint center{pivot.x - position.x, pivot.y - position.y};
            SDL_FPoint point{origin.x + position.x, origin.y + position.y};
            if (rotationCenter) {
                const float radians = rotation * std::numbers::pi_v<float> / 180.f;
                const float px = point.x + center.x - rotationCenter->x;
                const float py = point.y + center.y - rotationCenter->y;
                point = {
                    rotationCenter->x + px * std::cos(radians) - py * std::sin(radians) - center.x,
                    rotationCenter->y + px * std::sin(radians) + py * std::cos(radians) - center.y};
            }
            renderer_.sprite(image,
                             {point.x, point.y, source.w, source.h},
                             source,
                             rotation + (m.rotate ? 90 : 0),
                             {255, 255, 255, alpha},
                             mirrored,
                             &center);
        };
    const auto drawAnimation = [&](const AnimationFrame& animation,
                                   int frame,
                                   SDL_FPoint position,
                                   Uint8 alpha,
                                   bool mirrored,
                                   int maskWidth = -1,
                                   int maskHeight = -1) {
        if (!animation.usable()) {
            return;
        }
        const int width = maskWidth < 0 ? animation.width : std::min(maskWidth, animation.width);
        const int height =
            maskHeight < 0 ? animation.height : std::min(maskHeight, animation.height);
        drawLayer(renderer_.image(animation.path),
                  position,
                  {float(animation.x +
                         std::clamp(frame, 0, std::max(0, animation.frames - 1)) * animation.width),
                   float(animation.y),
                   float(width),
                   float(height)},
                  alpha,
                  mirrored);
    };
    const Uint8 cloakAlpha = cloaked ? 127 : 255;
    const AnimationFrame* explosion = state && !state->weaponExplosionImage.empty()
                                          ? &catalog_.animation(state->weaponExplosionImage)
                                          : nullptr;
    const bool exploding = explosion && state->weaponExplosionElapsed >= 0 &&
                           state->weaponExplosionElapsed < explosion->duration;
    const int frame = state ? state->animationFrame : retainedFrame;
    // OnRender458140: body, charge image, boost, sparks, startup explosion.
    if (!exploding || state->weaponExplosionElapsed < explosion->duration * .8f) {
        drawLayer(renderer_.image(e.imagePath),
                  {0, 0},
                  {float(e.frameX + frame * e.frameWidth),
                   float(e.frameY),
                   float(e.frameWidth),
                   float(e.frameHeight)},
                  cloakAlpha,
                  m.mirror);
    }
    if (state && !e.chargeImage.empty() && !state->animationFiring && frame != e.chargedFrame) {
        const float actual = state->cooldown > 0 ? state->charge / state->cooldown : 0;
        const float charge =
            e.delayChargeAnimation < 1
                ? std::max(0.f, (actual - e.delayChargeAnimation) / (1.f - e.delayChargeAnimation))
                : 0;
        auto& image = renderer_.image(e.chargeImage);
        drawLayer(image,
                  {0, 0},
                  {0, 0, float(image.w), float(image.h)},
                  Uint8(std::clamp(charge, 0.f, 1.f) * 255.f),
                  m.mirror);
    }
    if (state && state->animationBoostLevel >= 0 && !e.boostAnimation.empty()) {
        drawAnimation(catalog_.animation(e.boostAnimation),
                      state->animationBoostLevel,
                      {0, 0},
                      cloakAlpha,
                      m.mirror);
    }
    if (state && state->hackSparksElapsed >= 0) {
        const auto& sparks = catalog_.animation("stun_spark_big");
        const int sparkFrame =
            int(state->hackSparksElapsed / std::max(.001f, sparks.duration) * sparks.frames);
        drawAnimation(sparks,
                      sparkFrame,
                      state->hackSparksPosition,
                      255,
                      state->hackSparksMirror,
                      e.frameWidth,
                      e.frameHeight);
    }
    if (exploding) {
        const int explosionFrame = int(state->weaponExplosionElapsed /
                                       std::max(.001f, explosion->duration) * explosion->frames);
        const int width = m.rotate ? e.frameHeight : e.frameWidth;
        const int height = m.rotate ? e.frameWidth : e.frameHeight;
        drawAnimation(*explosion,
                      explosionFrame,
                      {float(width / 2 - 32), float(height / 2 - 32)},
                      255,
                      false);
    }
}

void ShipPainter::drawDestructionGibs(const ShipDefinition& s,
                                      const ShipPaintState& v,
                                      float x,
                                      float y) {
    if (v.destructionAge < 0 || s.gibs.empty()) {
        return;
    }
    int left = 10000;
    int top = 10000;
    int right = 0;
    int bottom = 0;
    for (const auto& room : s.rooms) {
        left = std::min(left, room.x);
        top = std::min(top, room.y);
        right = std::max(right, room.x + room.w);
        bottom = std::max(bottom, room.y + room.h);
    }
    const SDL_FPoint half{float((right - left) / 2), float((bottom - top) / 2)};
    const float baseX = x + (s.rooms.empty() ? 0 : left) + s.imageRect[0];
    const float baseY = y + (s.rooms.empty() ? 0 : top) + s.imageRect[1];
    for (int id = int(s.gibs.size()); id >= 1; --id) {
        if (v.explosion && v.explosion->jumpOut && v.destructionAge > 3.f &&
            id == int(s.gibs.size())) {
            continue;
        }
        const auto& gib = s.gibs[std::size_t(id - 1)];
        const auto* motion = v.explosion && std::size_t(id) <= v.explosion->gibs.size()
                                 ? &v.explosion->gibs[std::size_t(id - 1)]
                                 : nullptr;
        const float dx = motion ? motion->offset.x : 0;
        const float dy = motion ? motion->offset.y : 0;
        const float rotation = motion ? motion->rotation : 0;
        // OnRender47e490 first translates by this piece's starting position,
        // then rotates around half the room-bound shipBox, before translating
        // the image corner. Its weapon subpass subtracts that starting position.
        const SDL_FPoint center{x + gib.x + dx + half.x, y + gib.y + dy + half.y};
        const auto drawWeapon =
            [&](std::size_t slot, const std::string& name, const CombatWeapon* state, int frame) {
                if (slot >= s.mounts.size() || s.mounts[slot].gib != id) {
                    return;
                }
                drawMountedWeapon(catalog_.equipment(name),
                                  state,
                                  frame,
                                  s.mounts[slot],
                                  {baseX + dx, baseY + dy},
                                  v.deployment,
                                  false,
                                  rotation,
                                  &center);
            };
        for (std::size_t index = 0; index < s.weapons.size(); ++index) {
            const auto slot = std::ranges::find(v.weaponSlotIndices, static_cast<int>(index));
            const auto mount = v.weaponSlotIndices.empty() ? index
                               : slot == v.weaponSlotIndices.end()
                                   ? s.mounts.size()
                                   : static_cast<std::size_t>(slot - v.weaponSlotIndices.begin());
            drawWeapon(mount,
                       s.weapons[index],
                       v.weaponStates && index < v.weaponStates->size() ? &(*v.weaponStates)[index]
                                                                        : nullptr,
                       index < v.weaponFrames.size() ? v.weaponFrames[index] : 0);
        }
        std::size_t artillery = 0;
        for (const auto& system : s.systems) {
            if (system.id != 11 || !system.installed) {
                continue;
            }
            drawWeapon(std::max(std::size_t(4), s.weapons.size()) + artillery,
                       system.weapon,
                       artillery < v.artilleryStates.size() ? v.artilleryStates[artillery]
                                                            : nullptr,
                       artillery < v.artilleryFrames.size() ? v.artilleryFrames[artillery] : 0);
            ++artillery;
        }
        const auto path = (s.id.starts_with("PLAYER_SHIP") ? "img/ship/" : "img/ships_glow/") +
                          s.image + "_gib" + std::to_string(id) + ".png";
        auto& image = renderer_.image(path);
        const SDL_FPoint topLeft{baseX + gib.x + dx, baseY + gib.y + dy};
        const SDL_FPoint pivot{center.x - topLeft.x, center.y - topLeft.y};
        renderer_.sprite(image,
                         {topLeft.x, topLeft.y, float(image.w), float(image.h)},
                         {0, 0, float(image.w), float(image.h)},
                         rotation,
                         {255, 255, 255, 255},
                         false,
                         &pivot);
    }
}

void ShipPainter::drawDestructionExplosions(const ShipDefinition& s, const ShipPaintState& v) {
    if (v.destructionAge < 0 || v.destructionAge >= 3 || !s.id.starts_with("PLAYER_SHIP") ||
        !v.explosion) {
        return;
    }
    // 0x47e490 draws the persisted update-owned effects before age3; the
    // Animation::OnRender0x59ca30 position is its top-left, with authored scale.
    const float baseX = v.x + s.horizontalShift;
    const float baseY = v.y + s.verticalShift;
    for (const auto& effect : v.explosion->effects) {
        const auto& animation = catalog_.animation(effect.animation);
        if (!animation.usable() || effect.elapsed > animation.duration) {
            continue;
        }
        const int frame = std::min(animation.frames - 1,
                                   int(effect.elapsed / animation.duration * animation.frames));
        renderer_.sprite(renderer_.image(animation.path),
                         {baseX + effect.position.x,
                          baseY + effect.position.y,
                          float(animation.width) * effect.scale,
                          float(animation.height) * effect.scale},
                         {float(animation.x + frame * animation.width),
                          float(animation.y),
                          float(animation.width),
                          float(animation.height)});
    }
}

void ShipPainter::crewSprite(const HangarCrew& c,
                             float x,
                             float y,
                             float scale,
                             bool selected,
                             const std::string& animation,
                             std::uint64_t tick,
                             bool hostile,
                             bool colorblind,
                             bool stunned,
                             bool outline,
                             bool scaledPortrait,
                             bool silhouette) {
    auto name = c.species == "human" && (c.appearance % 2) ? std::string("female") : c.species;
    const auto path = "img/people/" + name;
    const bool death = animation.starts_with("death_");
    // CrewAnimation::StartTeleportArrive 0x472eb0 plays the status-6
    // teleport strip once, in reverse.
    const bool arriving = animation == "teleport_arrive";
    // StartTeleport 0x472e40 plays the same strip once, forward.
    const bool leaving = animation == "teleport_leave";
    stunned = stunned && !death;
    const auto& frame =
        catalog_.animation(c.species + (scale == 1 || !scaledPortrait
                                            ? "_" + (stunned ? std::string("walk_down")
                                                     : arriving || leaving ? std::string("teleport")
                                                                           : animation)
                                            : "_portrait"));
    if (!frame.usable()) {
        return;
    }
    const int elapsedFrame =
        int((stunned ? 0 : tick) / std::max(1.f, frame.duration * 60 / frame.frames));
    int spriteFrame = death || arriving || leaving ? std::min(frame.frames - 1, elapsedFrame)
                                                   : elapsedFrame % frame.frames;
    if (arriving) {
        spriteFrame = frame.frames - 1 - spriteFrame;
    }
    Rect src{float(frame.x + spriteFrame * frame.width),
             float(frame.y),
             float(frame.width),
             float(frame.height)};
    Rect dst{x, y, frame.width * scale, frame.height * scale};
    if (death && (c.species == "battle" || c.species == "repair" || c.species == "boarder_ion") &&
        frame.path != path + "_base.png") {
        // CrewAnimation::OnRender 0x46c7f0, a drone (bDrone) in status 3: the
        // death animation keeps its own sheet (battle_death is
        // effects/explosion1_strip10.png, 32 x 32), is centred in the 35 px
        // cell at (17 - w / 2, 17 - h / 2) and drawn white, without layers.
        renderer_.sprite(renderer_.image(frame.path),
                         {x + float(17 - int(frame.width * scale) / 2),
                          y + float(17 - int(frame.height * scale) / 2),
                          frame.width * scale,
                          frame.height * scale},
                         src);
        return;
    }
    // CrewAnimation46c7f0 tints both the base and appearance layers while selected.
    const SDL_Color tint = animation.starts_with("death_") ? SDL_Color{255, 255, 255, 255}
                           : stunned                       ? SDL_Color{125, 125, 125, 255}
                           : selected && !hostile          ? SDL_Color{128, 255, 128, 255}
                                                           : SDL_Color{255, 255, 255, 255};
    // CrewAnimation46c7f0: creation allegiance colors the outline, not ship location.
    const SDL_Color outlineColor =
        hostile    ? (colorblind ? SDL_Color{0, 147, 224, 255} : SDL_Color{255, 50, 50, 255})
        : selected ? SDL_Color{100, 255, 100, 255}
                   : SDL_Color{255, 255, 50, 255};
    // CrewBlueprint::RenderIcon 0x5e0f00 (store boxes, choice rewards) draws
    // only the base and colour layers, without the _color outline sheet.
    if (silhouette) {
        // CrewAnimation::OnRender with outlineOnly: nothing while dying (status
        // 3), no outline sheet or colour layers, only the base strip in
        // COLOR_RED (255, 50, 50) at alpha 0.75 (1.0 when colour-blind).
        if (death) {
            return;
        }
        renderer_.sprite(renderer_.image(path + "_base.png"),
                         dst,
                         src,
                         0,
                         {255, 50, 50, Uint8(colorblind ? 255 : 191)});
        return;
    }
    if (outline && scale == 1 && c.species != "battle" && c.species != "repair" &&
        c.species != "boarder_ion" && !animation.starts_with("death_")) {
        auto colorPath = path + "_color.png";
        renderer_.sprite(renderer_.image(colorPath),
                         selected ? Rect{dst.x - dst.w * .0625f,
                                         dst.y - dst.h * .0625f,
                                         dst.w * 1.125f,
                                         dst.h * 1.125f}
                                  : dst,
                         src,
                         0,
                         outlineColor);
    }
    renderer_.sprite(renderer_.image(path + "_base.png"), dst, src, 0, tint);
    // BlueprintManager::GetCrewBlueprint 0x5e9690 falls back to a default
    // blueprint for a race no crewBlueprint names (the boarder_ion body of an
    // IonDrone), which has no colour layer sheets to tint.
    static const std::vector<std::vector<std::array<float, 4>>> noLayers;
    const auto& colors =
        catalog_.hasAppearance(c.species) ? catalog_.appearance(c.species).layers : noLayers;
    int choice = c.appearance / (c.species == "human" ? 2 : 1);
    for (int i = 0; i < int(colors.size()); ++i) {
        if (colors[i].empty()) {
            continue;
        }
        auto layer = path + "_layer" + std::to_string(i + 1) + ".png";
        const auto& color = colors[i][choice % colors[i].size()];
        choice /= int(colors[i].size());
        renderer_.sprite(renderer_.image(layer),
                         dst,
                         src,
                         0,
                         {Uint8(color[0] * tint.r / 255.f),
                          Uint8(color[1] * tint.g / 255.f),
                          Uint8(color[2] * tint.b / 255.f),
                          Uint8(color[3] * 255)});
    }
}
} // namespace ftl
