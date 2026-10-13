#include "run.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>

namespace ftl {
namespace {
int weaponFrame(const EquipmentDefinition& e, const CombatWeapon& w) {
    if (w.animationInitialized) {
        return std::clamp(w.animationFrame, 0, e.animationFrames - 1);
    }
    const float actualCharge = w.cooldown > 0 ? w.charge / w.cooldown : 0.f;
    const float visualCharge =
        std::max((actualCharge - e.delayChargeAnimation) / (1.f - e.delayChargeAnimation), 0.f);
    int frame = actualCharge >= 1.f ? e.chargedFrame : int(e.chargedFrame * visualCharge);
    if (w.volleyRemaining > 0) {
        frame = e.fireFrame - int(std::ceil(w.volleyTimer / weaponFireDuration(e) *
                                            (e.animationFrames - e.chargedFrame)));
        while (frame < e.chargedFrame) {
            frame += std::max(1, e.animationFrames - e.chargedFrame);
        }
    }
    return std::clamp(frame, 0, e.animationFrames - 1);
}
} // namespace

std::vector<int> Run::combatWeaponFrames(const ShipDefinition& ship,
                                         const std::vector<CombatWeapon>& weapons) const {
    std::vector<int> frames;
    for (std::size_t i = 0; i < weapons.size(); ++i) {
        const auto& w = weapons[i];
        const auto& e = catalog_.equipment(ship.weapons[i]);
        frames.push_back(weaponFrame(e, w));
    }
    return frames;
}

std::vector<int> Run::combatArtilleryFrames(const std::vector<RunSystem>& systems) const {
    std::vector<int> frames;
    for (const auto& system : systems) {
        if (system.definition.id == 11) {
            frames.push_back(
                weaponFrame(catalog_.equipment(system.definition.weapon), system.artillery));
        }
    }
    return frames;
}

void Run::renderHazards(const ShipDefinition& ship,
                        const std::map<int, RoomHazard>& hazards,
                        float x,
                        float y) {
    if (hazards.empty()) {
        return;
    }
    // Fire::OnInit 0x48ed10 uses largeFire, eight 32px frames over one second.
    auto& fire = renderer_.image("img/effects/largeFire.png");
    auto& breach = renderer_.image("img/effects/breach.png");
    auto& smoke = renderer_.image("img/effects/fire_smoke.png");
    auto& healing = renderer_.image("img/people/healing_strip11.png");
    const bool slugGel =
        std::ranges::find(ship.augments, std::string("SLUG_GEL")) != ship.augments.end();
    const float healElapsed = slugGelHealElapsed_[std::size_t(&ship == &enemy_.ship)];
    // ShipManager49abc0 draws every breach before Spreader<Fire>.
    for (const auto& room : ship.rooms) {
        const auto it = hazards.find(room.id);
        if (it == hazards.end()) {
            continue;
        }
        const int columns = std::max(1, room.w / 35);
        const float rx = x + ship.horizontalShift + room.x;
        const float ry = y + ship.verticalShift + room.y;
        for (int index = 0; index < it->second.breaches; ++index) {
            // OuterHull::OnRender 0x6189c0 selects a frame from remaining
            // breach damage, not from the global animation clock.
            const float remaining = std::size_t(index) < it->second.breachHealth.size()
                                        ? it->second.breachHealth[std::size_t(index)]
                                        : 100.f - it->second.breachRepair;
            const int breachFrame = std::clamp(int(remaining / 100.f * 7.f), 0, 6);
            const int tile = index < int(it->second.breachTiles.size())
                                 ? it->second.breachTiles[std::size_t(index)]
                                 : index;
            const float tx = rx + float(tile % columns * 35);
            const float ty = ry + float(tile / columns * 35);
            renderer_.sprite(
                breach, {tx + 7.f, ty + 7.f, 22, 19}, {float(breachFrame * 22), 0, 22, 19});
            // OuterHull::OnRender 0x6189c0 overlays the looping heal strip in
            // COLOR_PURPLE (255,50,255) at the tile corner when SLUG_GEL is fitted.
            if (slugGel) {
                const int healFrame = std::min(10, int(healElapsed * 11.f));
                renderer_.sprite(healing,
                                 {tx, ty, 32, 32},
                                 {float(healFrame * 32), 0, 32, 32},
                                 0,
                                 {255, 50, 255, 255});
            }
        }
    }
    const auto& animations =
        &ship == &enemy_.ship ? enemy_.fireSpreaderAnimations : playerFireSpreaderAnimations_;
    const auto stateFor = [&](int room, int tile) -> const FireAnimationState* {
        const auto phases = animations.find(room);
        return phases != animations.end() && tile >= 0 && std::size_t(tile) < phases->second.size()
                   ? &phases->second[std::size_t(tile)]
                   : nullptr;
    };
    for (const auto& room : ship.rooms) {
        const auto it = hazards.find(room.id);
        if (it == hazards.end()) {
            continue;
        }
        const int columns = std::max(1, room.w / 35);
        const float rx = x + ship.horizontalShift + room.x;
        const float ry = y + ship.verticalShift + room.y;
        for (int index = 0; index < it->second.fires; ++index) {
            const int tile = index < int(it->second.fireTiles.size())
                                 ? it->second.fireTiles[std::size_t(index)]
                                 : index;
            const float tx = rx + float(tile % columns * 35);
            const float ty = ry + float(tile / columns * 35);
            const auto* state = stateFor(room.id, tile);
            const int fireFrame = state ? std::clamp(int(state->elapsed * 8.f), 0, 7) : 0;
            renderer_.sprite(fire, {tx, ty, 32, 32}, {float(fireFrame * 32), 0, 32, 32});
        }
        // Fire::OnInit 0x48ed10: seven 34px frames over one second.
        // Fire::OnRender 0x48fa00 retains a half-alpha flame during smoke frame zero.
        for (const auto& effect : it->second.fireSmoke) {
            const float tx = rx + float(effect.tile % columns * 35);
            const float ty = ry + float(effect.tile / columns * 35);
            const int frame = std::clamp(int(effect.elapsed * 7.f), 0, 6);
            const auto* state = stateFor(room.id, effect.tile);
            const int fireFrame = state ? std::clamp(int(state->elapsed * 8.f), 0, 7) : 0;
            if (frame == 0) {
                renderer_.sprite(fire,
                                 {tx, ty, 32, 32},
                                 {float(fireFrame * 32), 0, 32, 32},
                                 0,
                                 {255, 255, 255, 128});
            }
            renderer_.sprite(smoke,
                             {tx, ty, 34, 34},
                             {float(frame * 34), 0, 34, 34},
                             0,
                             {255, 255, 255, 255},
                             state && state->smokeMirrored);
        }
    }
    // ShipManager::OnRender 0x49abc0 draws each FIRE_EXTINGUISHERS emitter after
    // Spreader<Fire> for rooms that are burning. ParticleEmitter::OnRender
    // 0x586730 centres the 24px cloud and uses alpha 4 * lifespan.
    if (std::ranges::find(ship.augments, std::string("FIRE_EXTINGUISHERS")) ==
        ship.augments.end()) {
        return;
    }
    const auto& emitters = fireExtinguishers_[std::size_t(&ship == &enemy_.ship)];
    auto& cloud = renderer_.image("img/people/firefight_cloud.png");
    for (const auto& [roomId, emitter] : emitters) {
        const auto it = hazards.find(roomId);
        if (it == hazards.end() || it->second.fires <= 0) {
            continue;
        }
        for (const auto& particle : emitter.particles) {
            if (!particle.alive) {
                continue;
            }
            const auto alpha = std::uint8_t(std::clamp(4.f * particle.life, 0.f, 1.f) * 255.f);
            renderer_.sprite(cloud,
                             {x + ship.horizontalShift + particle.x - 12.f,
                              y + ship.verticalShift + particle.y - 12.f,
                              24,
                              24},
                             {0, 0, 24, 24},
                             0,
                             {255, 255, 255, alpha});
        }
    }
}

void Run::renderLockdown(const ShipDefinition& ship,
                         const std::map<int, float>& rooms,
                         float x,
                         float y) {
    if (rooms.empty()) {
        return;
    }
    auto& seal = renderer_.image("img/icons/locking/s_lock.png");
    const float sx = x + ship.horizontalShift;
    const float sy = y + ship.verticalShift;
    // The bomb projectile draws its authored explosion_lockdown strip at impact.
    for (const auto& door : ship.doors) {
        if (!rooms.contains(door.roomA) && !rooms.contains(door.roomB)) {
            continue;
        }
        const float dx = sx + door.x + (door.vertical ? 0.f : 17.f);
        const float dy = sy + door.y + (door.vertical ? 17.f : 0.f);
        renderer_.sprite(seal, {dx - 11.5f, dy - 13.5f, 23, 27}, {0, 0, 23, 27});
    }
}

void Run::renderExplosionEffects(const std::vector<CombatExplosionEffect>& effects,
                                 float x,
                                 float y) {
    for (const auto& effect : effects) {
        const auto& animation = catalog_.animation(effect.animation);
        if (!animation.usable() || effect.elapsed > animation.duration) {
            continue;
        }
        const int frame = std::min(animation.frames - 1,
                                   int(effect.elapsed / animation.duration * animation.frames));
        renderer_.sprite(renderer_.image(animation.path),
                         {x + effect.position.x,
                          y + effect.position.y,
                          float(animation.width) * effect.scale,
                          float(animation.height) * effect.scale},
                         {float(animation.x + frame * animation.width),
                          float(animation.y),
                          float(animation.width),
                          float(animation.height)});
    }
}

void Run::renderShieldHits(int ship) {
    const auto drawShieldHits = [&](int target, const ShipDefinition& ship, float x, float y) {
        const auto* shields = target == 0 ? system(0) : nullptr;
        if (target == 1) {
            const auto found = std::find_if(
                enemy_.systems.begin(), enemy_.systems.end(), [](const RunSystem& entry) {
                    return entry.definition.id == 0;
                });
            if (found != enemy_.systems.end()) {
                shields = &*found;
            }
        }
        const int super = target == 0 ? superShields_ : enemy_.superShields;
        const bool powered = shields && std::clamp(shields->power, 0, shields->health) > 1;
        // Shields::OnRenderShields 0x60ccf0 skips the hit strip while
        // shields_shutdown is set. OnLoop 0x60e310 retains rendering with
        // super shields even when ordinary shield power is unavailable.
        if (!powered && super <= 0) {
            return;
        }
        int left = 10000;
        int top = 10000;
        int right = 0;
        int bottom = 0;
        for (const auto& room : ship.rooms) {
            left = std::min(left, room.x);
            top = std::min(top, room.y);
            right = std::max(right, room.x + room.w);
            bottom = std::max(bottom, room.y + room.h);
        }
        const float centerX = (left + right) / 2.f + ship.ellipse[2];
        const float centerY = (top + bottom) / 2.f + ship.ellipse[3] + (target == 1 ? 110.f : 0.f);
        for (const auto& hit : shieldHits_) {
            if (hit.ship != target) {
                continue;
            }
            const float angle =
                std::acos(std::clamp((hit.point.x - centerX) / float(ship.ellipse[0]), -1.f, 1.f));
            // Shields::OnLoop 0x60e310 advances current_size by dx*speedFactor
            // and current_thickness by6*dx*speedFactor, removing the hit on
            // the next step after the arc reaches its 40-degree half-width.
            // GL_DrawShield 0x5994e0 uses a white triangle strip with a
            // squared-alpha fade across the arc and a transparent inner edge.
            // OnRenderShields uses end_size, not current_size: the size counter
            // controls lifetime while the visible arc keeps its full width.
            const float centerDegrees =
                (hit.point.y < centerY ? -angle : angle) * 180.f / std::numbers::pi_v<float>;
            renderer_.shieldArc({x + centerX, y + centerY},
                                {float(ship.ellipse[0]), float(ship.ellipse[1])},
                                int(centerDegrees - 40.f),
                                int(centerDegrees + 40.f),
                                hit.currentThickness);
        }
    };
    drawShieldHits(ship,
                   ship == 0 ? ship_ : enemy_.ship,
                   ship == 0 ? playerX() + ship_.horizontalShift : enemyOriginX(),
                   ship == 0 ? 170.f + ship_.verticalShift : enemyOriginY());
}

void Run::renderShipMessages(int ship) {
    // DamageMessage::OnRender 0x59cca0 translates the message by `progress * 10` — up
    // for a floating-up message and down for `bFloatDown`, which is the `(v2 & 2) - 1`
    // sign the decompiler shows. Both constructors place their primitive at (-32, -46).
    // OnRender retains the constructor colour/alpha; it does not fade the primitive.
    const auto drawMessageWord =
        [&](const std::string& name, float x, float y, float progress, SDL_Color color) {
            const auto base = "img/numbers/text_" + name + ".png";
            auto& atlas = renderer_.image(base);
            renderer_.sprite(atlas,
                             {x - 32.f, y - 46.f - progress * 10.f, float(atlas.w), float(atlas.h)},
                             {0, 0, float(atlas.w), float(atlas.h)},
                             0,
                             color);
        };
    const auto drawResist = [&](const ResistMessage& message, float x, float y) {
        const float progress = message.elapsed;
        // DamageSystem 0x49931c-0x499331 overrides Ion Armor's RESIST
        // message colour after the white constructor has run.
        drawMessageWord("resist",
                        x + message.point.x,
                        y + message.point.y,
                        progress,
                        message.mind          ? SDL_Color{255, 0, 255, 255}
                        : message.superShield ? SDL_Color{40, 240, 40, 255}
                        : message.ion         ? SDL_Color{40, 210, 230, 255}
                                              : SDL_Color{255, 255, 255, 255});
    };
    // DamageMessage::OnRender 0x59cca0 draws the primitives in the message colour, which
    // DamageMessage::DamageMessage(float, int, Pointf, bool) 0x59d080 initialises to
    // white. Native assembles the original
    // `numbers/Text_*_L.png` digits; anchor and timing comparisons remain recorded.
    const auto drawDamageMessage = [&](const DamageMessage& message, float x, float y) {
        const float progress = message.elapsed;
        if (message.miss) {
            drawMessageWord(
                "miss", x + message.point.x, y + message.point.y, progress, {255, 255, 255, 255});
            return;
        }
        // Shields::CollisionReal 0x60d980 colours its absorbed-damage message
        // (0.157, 0.941, 0.157). PulsarDamage 0x498c40 uses (40,210,230)
        // for ion numbers; ordinary messages keep constructor white.
        const SDL_Color color = message.superShield    ? SDL_Color{40, 240, 40, 255}
                                : message.ion          ? SDL_Color{40, 210, 230, 255}
                                : message.systemDamage ? SDL_Color{255, 100, 100, 255}
                                                       : SDL_Color{255, 255, 255, 255};
        // DamageMessage 0x59d080 assembles the archived large digit atlas.
        // 0x59d2b3 starts at -32 and 0x59d802 advances 16 per digit,
        // regardless of texture width. The zero branch uses the same anchor.
        // The constructor uses abs(damage); signs at x=-50 belong only to its
        // bFloatDown branch, which no native DamageMessage site models yet.
        const auto digits = std::to_string(std::abs(message.amount));
        float left = x + message.point.x - 32.f;
        for (char digit : digits) {
            const auto path = "img/numbers/Text_" + std::string(1, digit) + "_L.png";
            auto& atlas = renderer_.image(path);
            renderer_.sprite(atlas,
                             {left,
                              y + message.point.y - 46.f - progress * 10.f,
                              float(atlas.w),
                              float(atlas.h)},
                             {0, 0, float(atlas.w), float(atlas.h)},
                             0,
                             color);
            left += 16.f;
        }
    };
    const float x = ship == 0 ? playerX() + ship_.horizontalShift : enemyOriginX();
    const float y = ship == 0 ? 170.f + ship_.verticalShift : enemyOriginY();
    // ShipManager::OnRender 0x49abc0 renders its system explosions immediately
    // before the damage messages.
    if (ship == 0 || ship == 1) {
        renderExplosionEffects(systemExplosions_[std::size_t(ship)], x, y);
    }
    // ShipManager49abc0 draws its messages before Shields60c0b0's independent
    // message list. Both appear after crew, system explosions and destruction.
    for (const bool shieldDomain : {false, true}) {
        struct MessageView {
            std::uint64_t order;
            const ResistMessage* resist = nullptr;
            const DamageMessage* damage = nullptr;
        };

        std::vector<MessageView> messages;
        for (const auto& message : resistMessages_) {
            if (message.ship == ship && message.shieldOwned == shieldDomain) {
                messages.push_back({message.messageOrder, &message, nullptr});
            }
        }
        for (const auto& message : damageMessages_) {
            if (message.ship == ship && message.shieldOwned == shieldDomain) {
                messages.push_back({message.messageOrder, nullptr, &message});
            }
        }
        std::ranges::stable_sort(messages, {}, &MessageView::order);
        for (const auto& message : messages) {
            if (message.resist) {
                drawResist(*message.resist, x, y);
            } else {
                drawDamageMessage(*message.damage, x, y);
            }
        }
    }
}

void Run::renderSpaceProjectiles(int ship, int layer) {
    // DamageMessage::OnRender 0x59cca0 translates the message by `progress * 10` — up
    // for a floating-up message and down for `bFloatDown`, which is the `(v2 & 2) - 1`
    // sign the decompiler shows. Both constructors place their primitive at (-32, -46).
    // OnRender retains the constructor colour/alpha; it does not fade the primitive.
    const auto drawMessageWord =
        [&](const std::string& name, float x, float y, float progress, SDL_Color color) {
            const auto base = "img/numbers/text_" + name + ".png";
            auto& atlas = renderer_.image(base);
            renderer_.sprite(atlas,
                             {x - 32.f, y - 46.f - progress * 10.f, float(atlas.w), float(atlas.h)},
                             {0, 0, float(atlas.w), float(atlas.h)},
                             0,
                             color);
        };
    for (const auto& p : projectiles_) {
        const auto& weapon = catalog_.equipment(p.weapon);
        // SpaceManager::OnRenderProjectiles0x418dc0. Crystal retaliation's
        // carrier owner is -1; ordinary projectiles use their creation owner.
        const int carrierOwner = p.crystalShard ? -1 : p.owner;
        const int forcedLayer = p.weapon == "PDS_SHOT" ? (p.arrived ? 1 : 2) : -1;
        const bool ownerLayer = layer == 0 || (layer == 2 && ship == carrierOwner) ||
                                (layer == 1 && (ship != carrierOwner || ship == p.targetShip));
        if (forcedLayer >= 0 ? forcedLayer != layer : !ownerLayer) {
            continue;
        }
        if (weapon.type == "BEAM" ? (ship != p.space && ship != p.targetShip) : p.space != ship) {
            continue;
        }
        const float originX = ship == 0 ? playerX() + ship_.horizontalShift : enemyOriginX();
        const float originY = ship == 0 ? 170.f + ship_.verticalShift : enemyOriginY();
        const float effectAlpha =
            userPause_ || event_ || mapOpen_ || !panel_.empty() ? 1.f : presentationAlpha_;
        const float x = originX + std::lerp(p.previous.x, p.position.x, effectAlpha);
        const float y = originY + std::lerp(p.previous.y, p.position.y, effectAlpha);
        if (p.weapon == "PDS_SHOT" && !p.resolved) {
            if (p.arrived) {
                const auto& explosion = catalog_.animation(p.explosionImage);
                const float age = p.age - p.spinAngle;
                if (explosion.usable() && age >= 0 && age <= explosion.duration) {
                    const int frame =
                        std::min(explosion.frames - 1,
                                 static_cast<int>(age / explosion.duration * explosion.frames));
                    renderer_.sprite(renderer_.image(explosion.path),
                                     {originX + p.target.x - explosion.width / 2.f,
                                      originY + p.target.y - explosion.height / 2.f,
                                      static_cast<float>(explosion.width),
                                      static_cast<float>(explosion.height)},
                                     {static_cast<float>(explosion.x + frame * explosion.width),
                                      static_cast<float>(explosion.y),
                                      static_cast<float>(explosion.width),
                                      static_cast<float>(explosion.height)});
                }
            }
            const auto& animation = catalog_.animation(p.projectileImage);
            if (!animation.usable()) {
                continue;
            }
            const float total = std::hypot(p.target.x - p.targetEnd.x, p.target.y - p.targetEnd.y);
            const float traveled =
                std::hypot(p.position.x - p.targetEnd.x, p.position.y - p.targetEnd.y);
            const float scale = 1.f + (total > 0 ? traveled / total : 0.f) * 7.f;
            const float width = animation.width * scale;
            const float height = animation.height * scale;
            const int frame =
                animation.duration > 0
                    ? std::clamp(static_cast<int>(std::fmod(p.age, animation.duration) /
                                                  animation.duration * animation.frames),
                                 0,
                                 animation.frames - 1)
                    : 0;
            renderer_.sprite(renderer_.image(animation.path),
                             {x - width / 2, y - height / 2, width, height},
                             {static_cast<float>(animation.x + frame * animation.width),
                              static_cast<float>(animation.y),
                              static_cast<float>(animation.width),
                              static_cast<float>(animation.height)},
                             std::atan2(p.direction.y, p.direction.x) * 180.f / 3.141592653589793f);
            if (p.missed) {
                renderer_.draw("img/numbers/text_miss.png", x, y);
            }
            continue;
        }
        if (p.weapon == "ENV_ASTEROID") {
            if (!p.resolved) {
                renderer_.sprite(renderer_.image(p.projectileImage),
                                 {x - 12, y - 12, 24, 24},
                                 {0, 0, 24, 24},
                                 p.spinAngle);
            } else if (p.resolved && !p.missed) {
                // Asteroid::OnRenderSpecific44f060 renders its retained
                // death animation at the projectile translation, unrotated.
                const auto& effect = catalog_.animation(p.explosionImage);
                if (effect.usable()) {
                    const int frame = std::clamp(
                        int(p.age / effect.duration * effect.frames), 0, effect.frames - 1);
                    renderer_.sprite(renderer_.image(effect.path),
                                     {x, y, float(effect.width), float(effect.height)},
                                     {float(effect.x + frame * effect.width),
                                      float(effect.y),
                                      float(effect.width),
                                      float(effect.height)});
                }
            }
            continue;
        }
        if (weapon.type == "BEAM") {
            if (p.resolved || !p.beamStepped) {
                continue;
            }
            const SDL_Color color{Uint8(weapon.beamColor[0]),
                                  Uint8(weapon.beamColor[1]),
                                  Uint8(weapon.beamColor[2]),
                                  255};
            const int damage = std::max(0, weapon.damage + p.damageBonus);
            const int fullThickness = 6 * std::max(1, damage);
            const bool destination = ship == p.targetShip;
            const SDL_FPoint start =
                destination ? p.beamVisualInitialized ? p.beamSubStart : p.target : p.beamOrigin;
            const SDL_FPoint end =
                destination ? SDL_FPoint{x, y}
                            : SDL_FPoint{originX + p.beamSubEnd.x, originY + p.beamSubEnd.y};
            const SDL_FPoint from{originX + start.x, originY + start.y};
            const auto effect =
                [&](const CombatExplosionEffect& item, SDL_FPoint anchor, SDL_Color tint) {
                    auto animation = catalog_.animation(item.animation);
                    if (item.animation == "fire_smoke") {
                        animation.path = "img/effects/fire_smoke.png";
                        animation.x = animation.y = 0;
                        animation.width = animation.height = 34;
                        animation.frames = 7;
                        animation.duration = .5f;
                    }
                    if (!animation.usable()) {
                        return;
                    }
                    const int frame =
                        std::clamp(int(item.elapsed / animation.duration * animation.frames),
                                   0,
                                   animation.frames - 1);
                    renderer_.sprite(renderer_.image(animation.path),
                                     {anchor.x + item.position.x,
                                      anchor.y + item.position.y,
                                      animation.width * item.scale,
                                      animation.height * item.scale},
                                     {float(animation.x + frame * animation.width),
                                      float(animation.y),
                                      float(animation.width),
                                      float(animation.height)},
                                     0,
                                     tint);
                };
            if (!destination) {
                renderer_.laser(from, end, fullThickness, color);
                continue;
            }
            if (p.beamPierced) {
                for (const auto& smoke : p.beamSmoke) {
                    effect(smoke, {originX, originY}, {255, 255, 255, 255});
                }
            }
            const int interiorDamage = p.beamPierced && damage == 0 ? 1 : p.beamLastDamage;
            renderer_.laser(from, end, 6 * interiorDamage, color);
            if (p.beamPierced) {
                for (const auto& contact : p.beamContacts) {
                    effect(contact, {x - 24.f, y - 33.f}, color);
                }
            }
            if (p.beamLastDamage < damage || (damage == 0 && !p.beamPierced)) {
                const auto& destinationShip = ship == 0 ? ship_ : enemy_.ship;
                int left = 10000;
                int top = 10000;
                int right = 0;
                int bottom = 0;
                for (const auto& room : destinationShip.rooms) {
                    left = std::min(left, room.x);
                    top = std::min(top, room.y);
                    right = std::max(right, room.x + room.w);
                    bottom = std::max(bottom, room.y + room.h);
                }
                const auto& ellipse = destinationShip.ellipse;
                const Rect shield{originX + (left + right) / 2.f + ellipse[2] - ellipse[0],
                                  originY + (top + bottom) / 2.f + ellipse[3] - ellipse[1] +
                                      (ship == 1 ? 110.f : 0.f),
                                  2.f * ellipse[0],
                                  2.f * ellipse[1]};
                renderer_.laser(from, end, fullThickness, color, shield, true);
            }
            continue;
        }
        if (weapon.type == "BOMB" && p.bombMissTick > 0) {
            const float age = float(Time::seconds(double(tick_ - p.bombMissTick)));
            if (age < 1.f) {
                drawMessageWord("miss", x, y + 10, age, {255, 255, 255, 255});
            }
        }
        if (p.resolved) {
            if (p.missed && weapon.type != "BOMB") {
                // ShipManager owns its MISS word, rendered with ship messages.
                // A missed defense aim has no ShipManager message constructor.
            } else {
                // Projectile death animations are authored effects, advanced by game time.
                const std::string effect =
                    !p.explosionImage.empty()                     ? p.explosionImage
                    : !weapon.explosionAnimation.empty()          ? weapon.explosionAnimation
                    : weapon.type == "MISSILES" || p.crystalShard ? "explosion_big2"
                    : weapon.missiles > 0                         ? "explosion_missile1"
                                                                  : "explosion1";
                const auto& animation = catalog_.animation(effect);
                if (animation.usable() && p.age <= animation.duration) {
                    const float scale = p.fake ? .25f : 1.f;
                    const int frame = std::min(animation.frames - 1,
                                               int(p.age / animation.duration * animation.frames));
                    renderer_.sprite(renderer_.image(animation.path),
                                     {x - animation.width * scale / 2.f,
                                      y - animation.height * scale / 2.f,
                                      float(animation.width) * scale,
                                      float(animation.height) * scale},
                                     {float(animation.x + frame * animation.width),
                                      float(animation.y),
                                      float(animation.width),
                                      float(animation.height)});
                }
                continue;
            }
        }
        if (weapon.type == "BOMB" && (!p.arrived || p.bombDelay <= 0)) {
            continue;
        }
        const auto& animation = catalog_.animation(p.projectileImage);
        if (!animation.usable()) {
            continue;
        }
        auto& image = renderer_.image(animation.path);
        const float dx =
            p.resolved || p.space == p.owner ? p.direction.x : p.target.x - p.position.x;
        const float dy =
            p.resolved || p.space == p.owner ? p.direction.y : p.target.y - p.position.y;
        const float phase = weapon.type == "BOMB"      ? animation.duration - p.bombAnimationElapsed
                            : animation.duration > 0.f ? std::fmod(p.age, animation.duration)
                                                       : 0.f;
        const int frame = p.flightFrame >= 0 ? p.flightFrame
                          : animation.frames > 1 && animation.duration > 0
                              ? std::clamp(int(phase / animation.duration * animation.frames),
                                           0,
                                           animation.frames - 1)
                              : 0;
        // Missile::OnRenderSpecific 0x44b2f0 rotates about position, then
        // translates only by half the width. Position is the missile's tip;
        // centering its height makes the body pass the collision point.
        const bool missile = weapon.type == "MISSILES" || p.crystalShard;
        const SDL_FPoint missilePivot{animation.width / 2.f, 0};
        renderer_.sprite(image,
                         {x - animation.width / 2.f,
                          missile ? y : y - animation.height / 2.f,
                          float(animation.width),
                          float(animation.height)},
                         {float(animation.x + frame * animation.width),
                          float(animation.y),
                          float(animation.width),
                          float(animation.height)},
                         weapon.type == "BOMB" ? 0
                         : weapon.spin > 0 && !missile
                             ? p.spinAngle
                             : std::atan2(dy, dx) * 180 / 3.141592653589793 + (missile ? 90 : 0),
                         {255, 255, 255, 255},
                         false,
                         missile ? &missilePivot : nullptr);
    }
}

void Run::renderSpaceDrones(int ship, int layer) {
    const float originX = ship == 0 ? playerX() + ship_.horizontalShift : enemyOriginX();
    const float originY = ship == 0 ? 170.f + ship_.verticalShift : enemyOriginY();
    const auto drawDroneLayer =
        [&](const std::string& path, float x, float y, float angle, float alpha = 1.f) {
            auto& image = renderer_.image(path);
            // CachedImage585700 uses signed integer division for CENTERED.
            const SDL_FPoint pivot{float(image.w / 2), float(image.h / 2)};
            renderer_.sprite(image,
                             {x - pivot.x, y - pivot.y, float(image.w), float(image.h)},
                             {0, 0, float(image.w), float(image.h)},
                             angle,
                             {255, 255, 255, Uint8(std::clamp(alpha, 0.f, 1.f) * 255.f)},
                             false,
                             &pivot);
        };
    const auto movementAngle = [](SDL_FPoint velocity) {
        const float length = std::hypot(velocity.x, velocity.y);
        const float normalizedX = length > 0 ? velocity.x / length : 0.f;
        int angle =
            int(std::acos(std::clamp(normalizedX, -1.f, 1.f)) * 180.f / std::numbers::pi_v<float>);
        if (velocity.y < 0) {
            angle = -angle;
        }
        return angle;
    };
    const auto drawDroneStun = [&](const CombatDrone& drone, float x, float y, bool integral) {
        // SpaceDrone422d00 masks the authored70px strip to32x32; InitPrimitives
        // 59c5f0 mirrors that cropped primitive, not the complete source frame.
        const auto& animation = catalog_.animation("stun_spark_big");
        if (!animation.usable()) {
            return;
        }
        const int frame = std::clamp(int(std::max(0.f, drone.spaceHackSparksElapsed) /
                                         animation.duration * animation.frames),
                                     0,
                                     animation.frames - 1);
        renderer_.sprite(renderer_.image(animation.path),
                         {integral ? originX + std::trunc(drone.position.x - 16.f) : x - 16.f,
                          integral ? originY + std::trunc(drone.position.y - 16.f) : y - 16.f,
                          32,
                          32},
                         {float(animation.x + frame * animation.width), float(animation.y), 32, 32},
                         0,
                         {255, 255, 255, 255},
                         drone.spaceHackSparksMirror);
    };
    for (const auto& [owner, registered] : registeredSpaceDrones()) {
        const auto& drone = *registered;
        if (!drone.deployed) {
            continue;
        }
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        const bool hacking = blueprint.type == "HACKING";
        const int space =
            hacking ? owner == 0 ? hackingDroneSpace_ : enemy_.hackingDroneSpace : drone.space;
        if (blueprint.type == "REPAIR" || blueprint.type == "BATTLE" ||
            (blueprint.type == "BOARDER" && drone.delivered) || space != ship ||
            ((blueprint.type == "BOARDER" || hacking) && owner == space && layer != 2) ||
            (blueprint.type != "BOARDER" && drone.health <= 0)) {
            continue;
        }
        const bool stunned = drone.ionStun > 0 || drone.controllerHackLevel > 1;
        const float droneX = originX + drone.position.x;
        const float droneY = originY + drone.position.y;
        const auto drawMiss = [&] {
            if (drone.spaceMissElapsed < 0 || drone.spaceMissElapsed > 1.f) {
                return;
            }
            const std::string base = "img/numbers/text_miss.png";
            auto& image = renderer_.image(base);
            renderer_.draw(
                image,
                {originX + drone.spaceMissPosition.x - 32.f,
                 originY + drone.spaceMissPosition.y - 46.f - drone.spaceMissElapsed * 10.f,
                 float(image.w),
                 float(image.h)});
        };
        if (drone.spaceExplosionElapsed >= 0) {
            const auto& effect = catalog_.animation(drone.constructorExplosionImage);
            if (effect.usable()) {
                const int frame =
                    std::clamp(int(drone.spaceExplosionElapsed / effect.duration * effect.frames),
                               0,
                               effect.frames - 1);
                renderer_.sprite(renderer_.image(effect.path),
                                 {droneX - float(effect.width / 2),
                                  droneY - float(effect.height / 2),
                                  float(effect.width),
                                  float(effect.height)},
                                 {float(effect.x + frame * effect.width),
                                  float(effect.y),
                                  float(effect.width),
                                  float(effect.height)});
            }
            drawMiss();
            continue;
        }
        const std::string path = "img/ship/drones/" + blueprint.art;
        if (blueprint.type == "BOARDER") {
            // BoarderPodDrone41f3e0 renders static body animation/base then its
            // powered owner-color sheet. No carrier Animation::Update exists.
            const bool ion = drone.blueprint == "BOARDER_ION";
            const std::string species = ion ? "boarder_ion" : "battle";
            const auto& animation =
                catalog_.animation(ion ? "boarder_ion_power_down" : "battle_walk_down");
            if (animation.usable()) {
                const int angle =
                    drone.poweredLastFrame
                        ? int(stunned ? drone.hackAngle : float(movementAngle(drone.velocity)))
                        : int(drone.lastAimingAngle);
                const SDL_FPoint pivot{float(animation.width / 2), float(animation.height / 2)};
                const Rect destination{droneX - pivot.x,
                                       droneY - pivot.y,
                                       float(animation.width),
                                       float(animation.height)};
                if (drone.powered && enemy_.present && !cloaked(1 - owner)) {
                    const std::string flamePath = "img/ship/drones/boarder_engine.png";
                    auto& flame = renderer_.image(flamePath);
                    renderer_.sprite(flame,
                                     {destination.x, destination.y, float(flame.w), float(flame.h)},
                                     {0, 0, float(flame.w), float(flame.h)},
                                     angle + 90.f,
                                     {255, 255, 255, 255},
                                     false,
                                     &pivot);
                }
                const Rect source{float(animation.x + (ion ? 4 : 0) * animation.width),
                                  float(animation.y),
                                  float(animation.width),
                                  float(animation.height)};
                const std::string base = "img/people/" + species + "_base.png";
                const std::string colored = "img/people/" + species + "_layer1.png";
                renderer_.sprite(renderer_.image(base),
                                 destination,
                                 source,
                                 angle + 90.f,
                                 {255, 255, 255, 255},
                                 false,
                                 &pivot);

                if (drone.powered) {
                    // Globals81ffe0 loads the exact byte-scale source colors.
                    const SDL_Color color =
                        owner == 0 ? SDL_Color{100, 255, 100, 255} : SDL_Color{255, 50, 50, 255};
                    renderer_.sprite(renderer_.image(colored),
                                     destination,
                                     source,
                                     angle + 90.f,
                                     color,
                                     false,
                                     &pivot);
                }
            }
            drawMiss();
            continue;
        }
        if (hacking) {
            const bool arrived = drone.delivered;
            const int angle = stunned && !arrived ? int(drone.hackAngle)
                              : arrived || (drone.velocity.x == 0 && drone.velocity.y == 0)
                                  ? int(drone.lastAimingAngle)
                                  : movementAngle(drone.velocity);
            const std::string bodyPath = path + (drone.powered ? "_on.png" : "_base.png");
            const auto drawHackingAnimation = [&](const std::string& alias, float elapsed) {
                const auto& animation = catalog_.animation(alias);
                if (!animation.usable()) {
                    return;
                }
                // RenderDrone44bc60 rotates all layers about the selected
                // static body's half dimensions. Generic registry aliases
                // have position(0,0), independent of Animation's empty ctor.
                SDL_FPoint pivot{};
                auto& body = renderer_.image(bodyPath);
                pivot = {float(body.w / 2), float(body.h / 2)};

                const int frame =
                    std::clamp(int(std::max(0.f, elapsed) / animation.duration * animation.frames),
                               0,
                               animation.frames - 1);
                renderer_.sprite(renderer_.image(animation.path),
                                 {droneX - pivot.x,
                                  droneY - pivot.y,
                                  float(animation.width),
                                  float(animation.height)},
                                 {float(animation.x + frame * animation.width),
                                  float(animation.y),
                                  float(animation.width),
                                  float(animation.height)},
                                 angle + 90.f,
                                 {255, 255, 255, 255},
                                 false,
                                 &pivot);
            };
            if (arrived && (drone.hackingExtendingElapsed >= 0 || drone.hackingFinishedSetup)) {
                if (drone.hackingExtendingElapsed >= 0) {
                    drawHackingAnimation("drone_hack_extend", drone.hackingExtendingElapsed);
                } else {
                    drawDroneLayer(bodyPath, droneX, droneY, angle + 90.f);
                    if (drone.powered) {
                        const float alpha = (.5f - std::abs(drone.hackingFlashElapsed - .5f)) / .5f;
                        drawDroneLayer("img/ship/drones/drone_hack_light1.png",
                                       droneX,
                                       droneY,
                                       angle + 90.f,
                                       alpha);
                    }
                }
            } else {
                drawHackingAnimation("drone_hack_land", drone.hackingFlyingElapsed);
            }
            if (stunned) {
                drawDroneStun(drone, droneX, droneY, true);
            }
            drawMiss();
            continue;
        }
        if (blueprint.type == "SHIELD") {
            const std::string shield = "img/ship/drones/drone_shield";
            const std::string off = shield + "_off.png";
            drawDroneLayer(!drone.powered ? off : shield + "_charged.png", droneX, droneY, 0);
            const float glow = drone.shieldGlow;
            const float triangle =
                glow <= 1.5f ? std::max(0.f, glow / 1.5f) : 1.f - (glow - 1.5f) / 1.5f;
            drawDroneLayer(shield + "_glow.png", droneX, droneY, 0, triangle * triangle);
            if (glow > 1.15f && glow < 1.55f) {
                drawDroneLayer(
                    shield + "_glow.png", droneX, droneY, 0, std::abs(glow - 1.5f) / .35f);
            }
            if (stunned) {
                drawDroneStun(drone, droneX, droneY, false);
            }
            drawMiss();
            continue;
        }
        if (blueprint.type == "DEFENSE") {
            drawDroneLayer(path + "_base.png", droneX, droneY, 0);
            if (drone.powered && !stunned) {
                // Pointf::Normalize returns zero for zero velocity; acos(0)
                // still gives source integer90 and the engine is drawn.
                drawDroneLayer("img/ship/drones/drone_engine.png",
                               droneX,
                               droneY,
                               float(movementAngle(drone.velocity) + 90));
            }
            const std::string gun = path + (drone.powered ? "_gun_on.png" : "_gun.png");
            drawDroneLayer(gun, droneX, droneY, drone.aimAngle + 90.f);
            if (drone.powered && blueprint.cooldown > 0) {
                drawDroneLayer(path + "_gun_charged.png",
                               droneX,
                               droneY,
                               drone.aimAngle + 90.f,
                               (blueprint.cooldown - drone.cooldown) / blueprint.cooldown);
            }

            if (stunned) {
                drawDroneStun(drone, droneX, droneY, false);
            }
            drawMiss();
            continue;
        }
        if (blueprint.type == "SHIP_REPAIR" && drone.pause > 0) {
            const std::string wavePath = "img/effects/drone_ship_repair_wave.png";
            auto& wave = renderer_.image(wavePath);
            for (const float phase : drone.repairWavePhases) {
                const SDL_FPoint pivot{phase * 143.f, phase * 65.f};
                renderer_.sprite(
                    wave,
                    {droneX - pivot.x, droneY - pivot.y, wave.w * phase, wave.h * phase},
                    {0, 0, float(wave.w), float(wave.h)},
                    drone.aimAngle + 180.f,
                    {255, 255, 255, Uint8(std::clamp(1.f - phase * phase, 0.f, 1.f) * 255.f)},
                    false,
                    &pivot);
            }
        }
        if (blueprint.type == "SHIP_REPAIR" &&
            std::ranges::find(spaceShipOwners_, owner) == spaceShipOwners_.end()) {
            drawMiss();
            continue;
        }
        // CombatDrone41f8a0 selects ONE powered charge/body image. Stun is
        // drawn after the body and before its independent engine flame.
        drawDroneLayer(path + (!drone.powered    ? "_base.png"
                               : drone.pause > 0 ? "_charged.png"
                                                 : "_on.png"),
                       droneX,
                       droneY,
                       drone.aimAngle + 90.f);
        if (stunned) {
            drawDroneStun(drone, droneX, droneY, false);
        }
        if (drone.powered && !stunned) {
            if (drone.pause > .2f) {
                drawDroneLayer(
                    "img/ship/drones/drone_engine.png", droneX, droneY, drone.oldHeading - 90.f);
            } else if (drone.progressToDestination < .2f) {
                drawDroneLayer(
                    "img/ship/drones/drone_engine.png", droneX, droneY, drone.heading + 90.f);
            }
        }
        drawMiss();
    }
}

std::vector<bool> Run::doorsHit(bool enemy, std::size_t count) const {
    std::vector<bool> hit(count, false);
    for (const auto& [id, tick] : doorHitTick_[enemy ? 1 : 0]) {
        if (id >= 0 && std::size_t(id) < count && float(animation_ - tick) + uiFraction_ < 30.f) {
            hit[std::size_t(id)] = true;
        }
    }
    return hit;
}

void Run::doorRenderLevels(bool enemy, ShipPaintState& view) const {
    // Door::OnRender 0x466bb0 reads each door's own doorLevel and iHacked.
    const auto& layout = enemy ? enemy_.ship : ship_;
    const auto& levels = enemy ? enemy_.doorLevel : voyageDoorLevel_;
    view.doorLevels.assign(layout.doors.size(), -1);
    view.doorHacks.assign(layout.doors.size(), -1);
    for (const auto& door : layout.doors) {
        if (door.id < 0 || std::size_t(door.id) >= layout.doors.size()) {
            continue;
        }
        if (const auto level = levels.find(door.id); level != levels.end()) {
            view.doorLevels[std::size_t(door.id)] = level->second;
        }
        view.doorHacks[std::size_t(door.id)] = doorHackLevel(door, enemy);
    }
}

namespace {
// ShipGraph::GetRoomNeighbors 0x45c460: rooms holding a tile centre one tile
// outside this room's edges (corners excluded), whether or not a door joins them.
bool roomsAdjacent(const ShipDefinition& ship, int room, int other) {
    const auto shape = std::ranges::find(ship.rooms, room, &RoomDefinition::id);
    const auto target = std::ranges::find(ship.rooms, other, &RoomDefinition::id);
    if (shape == ship.rooms.end() || target == ship.rooms.end() || room == other) {
        return false;
    }
    const int left = shape->x - 17;
    const int right = shape->x + shape->w + 18;
    const int top = shape->y - 17;
    const int bottom = shape->y + shape->h + 18;
    for (int x = left; x <= shape->x + shape->w + 22; x += 35) {
        for (int y = top; y <= shape->y + shape->h + 22; y += 35) {
            if ((x == left || x == right) && (y == top || y == bottom)) {
                continue;
            }
            if (x >= target->x && x < target->x + target->w && y >= target->y &&
                y < target->y + target->h) {
                return true;
            }
        }
    }
    return false;
}
} // namespace

bool Run::playerRoomVisible(int room) const {
    // ShipManager::OnLoop 0x4a05b0 blacks out the player's rooms unless the
    // sensors are present, not strongly hacked and have effective power;
    // UpdateCrewmembers 0x49ee10 clears rooms holding the player's crew or a
    // mind-controlled actor, and a telepathic player crew (Slug) adds its
    // room's neighbours to tempVision for CheckVision 0x491620.
    const auto* sensors = system(7);
    if (!nebula_ && sensors && (!sensors->hackActive || sensors->hackLevel <= 1) &&
        subsystemLevel(7) > 0) {
        return true;
    }
    if (std::ranges::any_of(crew_, [&](const auto& member) {
            return !member.outOfGame && !member.intruder && member.health > 0 &&
                   member.appearance.species == "slug" && roomsAdjacent(ship_, member.room, room);
        })) {
        return true;
    }
    return std::ranges::any_of(crew_,
                               [&](const auto& member) {
                                   return !member.outOfGame && !member.intruder &&
                                          member.room == room;
                               }) ||
           std::ranges::any_of(voyageBoarders_,
                               [&](const auto& member) {
                                   return !member.outOfGame && member.controlled &&
                                          member.room == room;
                               }) ||
           std::ranges::any_of(enemy_.crew,
                               [&](const auto& member) {
                                   return !member.outOfGame && member.intruder &&
                                          member.controlled && member.boardRoom == room;
                               }) ||
           std::ranges::any_of(combatDrones_, [&](const auto& drone) {
               const auto& type = catalog_.equipment(drone.blueprint).type;
               return drone.deployed && drone.delivered && (type == "REPAIR" || type == "BATTLE") &&
                      drone.room == room;
           });
}

bool Run::playerLifeSight() const {
    // ShipObject::HasEquipment "slug" (a slug crew member, matching the mind
    // control target rule) or the LIFE_SCANNER augment.
    return std::ranges::find(ship_.augments, std::string("LIFE_SCANNER")) != ship_.augments.end() ||
           std::ranges::any_of(crew_, [](const auto& member) {
               return (!member.outOfGame || member.cloneReady) &&
                      member.appearance.species == "slug";
           });
}

bool Run::enemyRoomVisible(int room) const {
    // CheckVision 0x491620 opens every hostile room while the player's sensors
    // are not strongly hacked and have effective power above one.
    const auto* sensors = system(7);
    if (!nebula_ && sensors && (!sensors->hackActive || sensors->hackLevel <= 1) &&
        subsystemLevel(7) >= 2) {
        return true;
    }
    if (std::ranges::any_of(crew_,
                            [&](const auto& member) {
                                return member.intruder && !member.outOfGame &&
                                       member.boardRoom == room;
                            }) ||
        std::ranges::any_of(enemy_.crew,
                            [&](const auto& member) {
                                return member.controlled && !member.intruder && !member.outOfGame &&
                                       member.room == room;
                            }) ||
        std::ranges::any_of(physicalCrewDrones(0), [&](const auto* drone) {
            return drone->deployed && drone->crewPhysicalShip == 1 && drone->crewRoom == room;
        })) {
        return true;
    }
    // Telepathic player crew aboard reveal the neighbouring rooms, and
    // ShipManager::OnLoop adds the room of every hostile system carrying the
    // player's hacking drone (bUnderAttack, iHackEffect > 0) to tempVision.
    if (std::ranges::any_of(crew_,
                            [&](const auto& member) {
                                return member.intruder && !member.outOfGame && member.health > 0 &&
                                       member.appearance.species == "slug" &&
                                       roomsAdjacent(enemy_.ship, member.boardRoom, room);
                            }) ||
        std::ranges::any_of(enemy_.systems, [&](const auto& sys) {
            return sys.hackActive && sys.hackLevel > 0 && sys.definition.room == room;
        })) {
        return true;
    }
    const auto shape = std::ranges::find(enemy_.ship.rooms, room, &RoomDefinition::id);
    if (shape == enemy_.ship.rooms.end()) {
        return false;
    }
    return std::ranges::any_of(projectiles_, [&](const auto& shot) {
        if (shot.owner != 0 || shot.targetShip != 1 || shot.missed || !shot.arrived ||
            catalog_.equipment(shot.weapon).type != "BOMB") {
            return false;
        }
        const auto effect = shot.explosionImage.empty() ? "explosion_big2" : shot.explosionImage;
        return (!shot.resolved || shot.age <= catalog_.animation(effect).duration) &&
               Rect{float(shape->x), float(shape->y), float(shape->w), float(shape->h)}.contains(
                   shot.position.x, shot.position.y);
    });
}

void Run::combatRender() {
    if (!enemy_.present) {
        return;
    }
    // Presentation only. Simulation, collision and RNG are owned by advanceCombat.
    const bool boss =
        campaign_.fighting || enemy_.ship.id == "BOSS_SPECIAL" || enemy_.ship.id == "BOSS_DEMO";
    const float boxX = boss ? 752.f : 872.f;
    const float boxY = boss ? 7.f : 45.f;
    const float statusX = boxX + (boss ? 12.f : 17.f);
    const float statusY = boxY + (boss ? 40.f : 45.f);
    const SDL_Color boxColor =
        hostile_ ? SDL_Color{255, 179, 179, 255} : SDL_Color{255, 255, 255, 255};
    const std::string mask =
        boss ? "img/combatUI/box_hostiles_boss_mask.png" : "img/combatUI/box_hostiles_mask.png";
    auto& box = renderer_.image(boss ? "img/combatUI/box_hostiles_boss.png"
                                     : "img/combatUI/box_hostiles2.png");
    renderer_.sprite(box,
                     {boxX, boxY, float(box.w), float(box.h)},
                     {0, 0, float(box.w), float(box.h)},
                     0,
                     boxColor);
    ShipPaintState view;
    view.colorblind = options_.colorblind;
    view.x = enemyOriginX() - enemy_.ship.horizontalShift;
    view.y = enemyOriginY() - enemy_.ship.verticalShift;
    view.tick = tick_;
    view.active = true;
    view.orderedPhysicalCrew = true;
    view.hostile = hostile_;
    // Level-two player sensors expose hostile rooms, independent of whether
    // the hostile ship has a sensor subsystem of its own.
    const auto* sensors = system(7);
    view.sensorsWorking = !nebula_ && sensors &&
                          (!sensors->hackActive || sensors->hackLevel <= 1) &&
                          subsystemLevel(7) >= 2;
    view.enginesOn = false;
    view.doors = enemy_.fakeDoors;
    view.doorFrames = enemy_.doorFrames;
    view.doorHit = doorsHit(true, enemy_.doorFrames.size());
    doorRenderLevels(true, view);
    view.doorLevel = 0;
    view.deployment = 1;
    if (enemy_.escapeJumpElapsed >= 0) {
        view.jumpProgress = std::clamp(2.f * (enemy_.escapeJumpElapsed - 1.f), 0.f, 1.f);
    }
    // The original hides room/systems overlays as soon as destruction starts,
    // while the intact exterior remains visible through the first bursts.
    // ShipManager::OnRender 0x49abc0 also skips the whole interior of a ship
    // that is not hostile (a surrender accepted, a neutral encounter) unless
    // player crew are aboard (bContainsPlayerCrew).
    view.rooms = !enemyExplosionActive_ && (hostile_ || enemyContainsPlayerCrew());
    if (enemyExplosionActive_) {
        view.destructionAge = explosionDuration_ - explosion_;
    }
    view.cloaked = enemy_.cloak.active;
    view.engineAnimationTimes = enemy_.engineAnimationTimes;
    view.explosion = &enemy_.explosionState;
    view.weaponStates = &enemy_.weapons;
    for (const auto& system : enemy_.systems) {
        if (system.definition.id == 11) {
            view.artilleryStates.push_back(&system.artillery);
        }
    }
    view.weaponFrames = combatWeaponFrames(enemy_.ship, enemy_.weapons);
    view.artilleryFrames = combatArtilleryFrames(enemy_.systems);
    view.systemFlash = std::fmod((static_cast<float>(animation_ % 45) + uiFraction_) / 60.f, .75f);
    for (const auto& station : enemy_.systems) {
        view.systemHealth[station.definition.id] = station.health;
        view.systemCapacity[station.definition.id] = station.definition.power;
        view.systemLocked[station.definition.id] =
            station.lockCount > 0 || station.lockCount == -1 || station.hackLevel > 1;
        view.systemHackEffect[station.definition.id] = station.hackLevel;
        if (station.definition.id == 5 && station.hackActive && station.hackLevel > 1) {
            view.damagingEffectTimes[station.definition.room] = station.damagingEffectTimes;
        }
        if (station.definition.id == 8) {
            view.doorLevel = std::clamp(std::min(station.power, station.health), 0, 4);
            view.doorsHacked = station.hackActive;
        }
        if (station.health != station.definition.power || station.power <= 0) {
            continue;
        }
        const auto operatorAtStation =
            std::find_if(enemy_.crew.begin(), enemy_.crew.end(), [&](const auto& crew) {
                return !crew.intruder && !crew.controlled && !crewHasOpponent(crew) &&
                       crew.health > 0 && crew.stunTime <= 0 && crew.path.empty() &&
                       crew.room == station.definition.room && crew.slot == station.definition.slot;
            });
        if (operatorAtStation != enemy_.crew.end()) {
            view.computerLevels[station.definition.id] =
                station.definition.id == 3 ? operatorAtStation->weaponSkill + 1 : 1;
        }
        if (station.definition.id == 8 && operatorAtStation != enemy_.crew.end()) {
            view.doorLevel = std::min(view.doorLevel + 1, 4);
        }
    }
    std::vector<HangarCrew> enemyAppearances;
    std::vector<int> boarderRooms;
    std::vector<const RunCrew*> boarders;
    for (const auto& member : crew_) {
        if (!member.intruder || member.outOfGame) {
            continue;
        }
        int room = member.boardRoom;
        const auto known = std::ranges::find_if(enemy_.ship.rooms, [room](const auto& candidate) {
            return candidate.id == room;
        });
        if (known == enemy_.ship.rooms.end()) {
            // A stale room id must not make the party vanish; fall back to the first
            // real room so the intruder stays visible and distinctly presented.
            room = enemy_.ship.rooms.empty() ? -1 : enemy_.ship.rooms.front().id;
        }
        boarderRooms.push_back(room);
        boarders.push_back(&member);
    }
    // CrewMember::ProvidesVision0x720dd0: player-owned crew and controlled
    // defenders expose the room they physically occupy aboard the target.
    const bool playerVision = enemyContainsPlayerCrew();
    view.containsPlayerCrew = playerVision;
    view.explicitVision = true;
    for (const auto* member : boarders) {
        view.visionRooms.push_back(member->boardRoom);
    }
    for (const auto& member : enemy_.crew) {
        if (member.controlled && !member.intruder && !member.outOfGame) {
            view.visionRooms.push_back(member.room);
        }
    }
    for (const auto* drone : physicalCrewDrones(0)) {
        if (drone->deployed && drone->crewPhysicalShip == 1 && drone->crewRoom >= 0) {
            view.visionRooms.push_back(drone->crewRoom);
            view.containsPlayerCrew = true;
        }
    }
    if (!enemyExplosionActive_ && (!enemy_.cloak.active || view.containsPlayerCrew)) {
        const SDL_FPoint mouse{float(int(mouseX_ - enemyOriginX())),
                               float(int(mouseY_ - enemyOriginY()))};
        const auto roomAt = [&](SDL_FPoint point) {
            for (const auto& room : enemy_.ship.rooms) {
                if (Rect{float(room.x), float(room.y), float(room.w), float(room.h)}.contains(
                        float(int(point.x)), float(int(point.y)))) {
                    return room.id;
                }
            }
            return -1;
        };
        view.hoverRoom = roomAt(mouse);
        if (aiming_ >= 0 && aiming_ < int(combatWeapons_.size())) {
            const auto& weapon = combatWeapons_[std::size_t(aiming_)];
            const auto& blueprint = catalog_.equipment(ship_.weapons[std::size_t(aiming_)]);
            if (blueprint.type == "BEAM" && weapon.targetFirstPending) {
                view.hoverRoom = -1;
                const float distance =
                    std::hypot(mouse.x - weapon.target.x, mouse.y - weapon.target.y);
                if (distance <= 10.f) {
                    view.highlightRooms.push_back(roomAt(weapon.target));
                } else {
                    // RenderTarget55ff00 walks one pixel from the first
                    // selected point to the authored-length preview end.
                    const SDL_FPoint direction{(mouse.x - weapon.target.x) / distance,
                                               (mouse.y - weapon.target.y) / distance};
                    for (int along = 1; along <= blueprint.length + 1; ++along) {
                        const float offset = std::min(float(along), float(blueprint.length));
                        const int room = roomAt({weapon.target.x + direction.x * offset,
                                                 weapon.target.y + direction.y * offset});
                        if (room >= 0 && std::ranges::find(view.highlightRooms, room) ==
                                             view.highlightRooms.end()) {
                            view.highlightRooms.push_back(room);
                        }
                    }
                }
            }
        }
    }
    for (const auto& shot : projectiles_) {
        if (shot.owner != 0 || shot.targetShip != 1 || !shot.arrived || shot.missed ||
            catalog_.equipment(shot.weapon).type != "BOMB") {
            continue;
        }
        const auto effect = shot.explosionImage.empty() ? "explosion_big2" : shot.explosionImage;
        if (shot.resolved && shot.age > catalog_.animation(effect).duration) {
            continue;
        }
        for (const auto& shape : enemy_.ship.rooms) {
            if (Rect{float(shape.x), float(shape.y), float(shape.w), float(shape.h)}.contains(
                    shot.position.x, shot.position.y)) {
                view.visionRooms.push_back(shape.id);
            }
        }
    }
    // ShipManager::OnRender 0x49abc0: a crew member in a blacked-out room is
    // drawn outline-only when the viewing ship (current_target, the player)
    // HasEquipment "slug" or "LIFE_SCANNER" (literals 0x82d4d2/0x82d4d7).
    const bool lifeSight = playerLifeSight();
    if ((view.sensorsWorking || playerVision || !view.visionRooms.empty() || lifeSight) &&
        view.rooms) {
        enemyAppearances.reserve(enemy_.crew.size() + boarders.size());
        for (const auto& member : enemy_.crew) {
            const bool visible = enemyRoomVisible(member.room);
            if (member.intruder || member.outOfGame || (!visible && !lifeSight)) {
                continue;
            }
            view.silhouetteCrew.resize(enemyAppearances.size());
            view.silhouetteCrew.push_back(!visible);
            auto appearance = member.appearance;
            appearance.room = member.room;
            appearance.slot = member.slot;
            enemyAppearances.push_back(std::move(appearance));
            view.crewPhysicalOrders.push_back(member.physicalCrewOrder);
            // CrewMember::OnRenderHealth runs only for rooms that are not blacked out.
            view.crewHealthRenderers.push_back(
                visible ? std::function<void()>([this, actor = &member] {
                    renderCrewHealth(*actor, enemyOriginX(), enemyOriginY());
                })
                        : std::function<void()>{});
            view.crewPositions.push_back(crewPresentationPoint(member));
            view.crewHealth.push_back(
                100.f * member.health /
                (crewMaximumHealth(member.appearance.species) + member.healthBoost));
            view.crewHealthAlpha.push_back(crewHealthAlpha(member));
            view.crewHealthFlash.push_back(member.health <= 25 && member.healthFlashTime < .5f);
            view.hostileCrew.push_back(true);
            view.stunnedCrew.push_back(member.stunTime > 0);
            view.selected.push_back(false);
            // CrewMember::OnLoop 0x4747f0 sets `bFighting` only for a member that has
            // stopped at a goal (AtFinalGoal 0x471210) with a live opponent; its
            // LABEL_110 branch checks repair first, then fighting, then the manning
            // strip. A stunned member takes the stunned branch before any of them and
            // CrewAnimation::OnUpdate 0x473600 forces it to the standing strip.
            // CrewMember::OnLoop 0x474d62: repairing = currentRepair && CanRepair().
            const bool repairing = member.repairKind != 0;
            std::string authored;
            for (const auto& station : enemy_.systems) {
                if (station.definition.room != member.room) {
                    continue;
                }
                if (station.health < station.definition.power) {
                    continue;
                }
                if (crewOperator(station.definition.id, true) == &member) {
                    authored = station.definition.direction;
                    std::transform(
                        authored.begin(), authored.end(), authored.begin(), [](unsigned char c) {
                            return char(std::tolower(c));
                        });
                }
            }
            const bool typing = !authored.empty() && authored != "none";
            const bool idle = member.path.empty() && member.health > 0 && member.stunTime <= 0;
            std::string animation =
                "walk_" + (member.path.empty() ? std::string("down") : member.facing);
            if (member.health <= 0) {
                animation = member.dying ? (member.facing == "left" ? "death_left" : "death_right")
                                         : "gone";
            } else if (idle && repairing) {
                animation = "repair";
            } else if (idle && typing) {
                animation = "type_" + authored;
            }
            // CrewAnimation::OnUpdate 0x473600 status 4: the extinguishing strip
            // towards the fire, over the emitter's smoke.
            const auto fire = crewFireAnimation(member, member.smoke);
            if (!fire.empty()) {
                animation = fire;
            }
            if (const auto combat = crewCombatAnimation(member); !combat.empty()) {
                animation = combat;
            }
            view.crewAnimations.push_back(animation);
            view.crewTicks.push_back(
                animation.starts_with("death_") ? std::uint64_t(member.deathAnimationElapsed * 60.f)
                : member.path.empty() && member.attackAnimation.empty() &&
                        !animation.starts_with("type_") && animation != "repair" &&
                        !animation.starts_with("fire_")
                    ? 0
                    : crewWalkTick(animation, true));
            if (!crewCombatAnimation(member).empty()) {
                view.crewTicks.back() = crewCombatTick(member);
            } else if (animation == "fire_down" && member.appearance.species == "rock") {
                view.crewTicks.back() =
                    std::uint64_t(std::min(59.f, member.fireWorkElapsed * 60.f));
            }
            view.crewUnderlays.push_back(nullptr);
            if (!fire.empty() && animation == fire) {
                view.crewUnderlays.back() = [this, smoke = &member.smoke](float cornerX,
                                                                          float cornerY) {
                    renderCrewSmoke(*smoke, cornerX, cornerY);
                };
            }
            if (member.health > 0 && (member.teleportArrive > 0 || member.teleportLeaveKind != 0)) {
                view.crewAnimations.back() =
                    member.teleportLeaveKind != 0 ? "teleport_leave" : "teleport_arrive";
                view.crewTicks.back() = crewTeleportArriveTick(member);
            }
        }
        for (const auto* member : boarders) {
            auto appearance = member->appearance;
            const auto boarderIndex =
                std::size_t(std::ranges::find(boarders, member) - boarders.begin());
            appearance.room = boarderRooms[boarderIndex];
            appearance.slot = member->boardPositionValid ? member->boardSlot : 0;
            enemyAppearances.push_back(std::move(appearance));
            view.crewPhysicalOrders.push_back(member->physicalCrewOrder);
            view.crewHealthRenderers.push_back([this, actor = member] {
                renderCrewHealth(*actor, enemyOriginX(), enemyOriginY());
            });
        }
        for (std::size_t index = 0; index < boarders.size(); ++index) {
            const auto& member = *boarders[index];
            // ShipPainter::draw draws a crew sprite at `crewPositions[i] - 17` and
            // resolves the tile from `crew.room`/`crew.slot` with the player ship's
            // room layout when no position is supplied. The party's tile therefore
            // comes from the enemy hull's own room rectangle: it is the tile anchor
            // shifted by the same half-tile the player path uses.

            view.crewPositions.push_back(crewPresentationPoint(member));
            view.crewHealth.push_back(
                100.f * member.health /
                (crewMaximumHealth(member.appearance.species) + member.healthBoost));
            view.crewHealthAlpha.push_back(crewHealthAlpha(member));
            view.crewHealthFlash.push_back(member.health <= 25 && member.healthFlashTime < .5f);
            view.hostileCrew.push_back(false);
            view.stunnedCrew.push_back(member.stunTime > 0);
            view.selected.push_back(member.selected);
            std::string animation =
                "walk_" + (member.boardPath.empty() ? std::string("down") : member.facing);
            if (member.health <= 0) {
                animation = member.dying ? (member.facing == "left" ? "death_left" : "death_right")
                                         : "gone";
            }
            // Only a mind-controlled visitor is no intruder and can reach status 4.
            const auto fire = crewFireAnimation(member, member.smoke);
            if (!fire.empty()) {
                animation = fire;
            }
            if (const auto combat = crewCombatAnimation(member); !combat.empty()) {
                animation = combat;
            }
            view.crewAnimations.push_back(animation);
            view.crewTicks.push_back(animation.starts_with("death_")
                                         ? std::uint64_t(member.deathAnimationElapsed * 60.f)
                                         : crewWalkTick(animation, false));
            if (!crewCombatAnimation(member).empty()) {
                view.crewTicks.back() = crewCombatTick(member);
            } else if (animation == "fire_down" && member.appearance.species == "rock") {
                view.crewTicks.back() =
                    std::uint64_t(std::min(59.f, member.fireWorkElapsed * 60.f));
            }
            view.crewUnderlays.push_back(nullptr);
            if (!fire.empty() && animation == fire) {
                view.crewUnderlays.back() = [this, smoke = &member.smoke](float cornerX,
                                                                          float cornerY) {
                    renderCrewSmoke(*smoke, cornerX, cornerY);
                };
            }
            if (member.health > 0 && (member.teleportArrive > 0 || member.teleportLeaveKind != 0)) {
                view.crewAnimations.back() =
                    member.teleportLeaveKind != 0 ? "teleport_leave" : "teleport_arrive";
                view.crewTicks.back() = crewTeleportArriveTick(member);
            }
        }
    }
    for (const int owner : {0, 1}) {
        for (const auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip == 1 && drone->crewRoom >= 0 &&
                enemyRoomVisible(drone->crewRoom)) {
                view.visitingCrew.push_back(
                    {drone->physicalCrewOrder,
                     [this, drone] {
                         renderCrewDrones(1, enemyOriginX(), enemyOriginY(), false, drone);
                     },
                     [this, drone] {
                         renderCrewDrones(1, enemyOriginX(), enemyOriginY(), true, drone);
                     }});
            }
        }
    }
    // RenderTarget55ff00 clips the complete ship and both projectile passes
    // through the authored binary texture stencil.
    renderer_.beginMaskedLayer(mask, {boxX, boxY});
    renderSpaceProjectiles(1, 2);
    renderSpaceDrones(1, 2);
    // ShipManager::OnLoop 0x4a05b0 uses target super shields and intruders to
    // update MindSystem::SetBlocked; boarding does not remove the shield image.
    if (!enemyExplosionActive_ && (enemy_.shields > 0 || enemy_.superShields > 0)) {
        int left = 10000;
        int top = 10000;
        int right = 0;
        int bottom = 0;
        for (const auto& room : enemy_.ship.rooms) {
            left = std::min(left, room.x);
            top = std::min(top, room.y);
            right = std::max(right, room.x + room.w);
            bottom = std::max(bottom, room.y + room.h);
        }
        const auto& ellipse = enemy_.ship.ellipse;
        const Rect destination{enemyOriginX() + (left + right) / 2 + ellipse[2] - ellipse[0],
                               enemyOriginY() + 110.f + (top + bottom) / 2 + ellipse[3] -
                                   ellipse[1],
                               2.f * ellipse[0],
                               2.f * ellipse[1]};
        if (enemy_.shields > 0) {
            renderer_.maskedImage("img/ship/enemy_shields.png",
                                  destination,
                                  mask,
                                  {boxX, boxY},
                                  .4f + .6f * enemy_.shields / 4.f);
        }
        if (enemy_.superShields > 0) {
            renderer_.maskedImage("img/ship/enemy_shields.png",
                                  destination,
                                  mask,
                                  {boxX, boxY},
                                  .55f + .35f * enemy_.superShields / 5.f,
                                  {102, 255, 105, 255});
        }
    }
    if (enemyExplosionActive_ && explosionDuration_ - explosion_ > 1.5f) {
        painter_.drawDestructionGibs(enemy_.ship,
                                     view,
                                     view.x + enemy_.ship.horizontalShift,
                                     view.y + enemy_.ship.verticalShift);
    }
    const float explosionAge = explosionDuration_ - explosion_;
    if (enemyExplosionActive_ && enemy_.explosionState.jumpOut && explosionAge > 3.f &&
        !enemy_.ship.gibs.empty()) {
        // Ship0x48a4b0 swaps its hull to GetFinalGib when progress>75%;
        // OnRenderExplosion0x481df0 fades that stationary gib through Jump.
        const auto& finalShip = enemy_.ship;
        const auto& gib = finalShip.gibs.back();
        const std::string path = "img/ships_glow/" + finalShip.image + "_gib" +
                                 std::to_string(finalShip.gibs.size()) + ".png";
        const float progress = std::clamp(explosionAge - 3.f, 0.f, 1.f);
        const int graphX = finalShip.rooms.empty()
                               ? 0
                               : std::ranges::min(finalShip.rooms, {}, &RoomDefinition::x).x;
        const int graphY = finalShip.rooms.empty()
                               ? 0
                               : std::ranges::min(finalShip.rooms, {}, &RoomDefinition::y).y;
        renderer_.draw(path,
                       enemyOriginX() + graphX + finalShip.imageRect[0] + gib.x,
                       enemyOriginY() + graphY + finalShip.imageRect[1] + gib.y,
                       1,
                       1 - progress);
        const float phase = std::clamp(progress / .75f, 0.f, 1.f);
        const float extent = phase <= 0 || phase >= 1 ? 0.f
                             : phase < .5f            ? phase * 1200.f + 64.f
                                                      : 600.f - (phase - .5f) * 1200.f;
        if (extent > 0) {
            int left = 10000;
            int top = 10000;
            int right = 0;
            int bottom = 0;
            for (const auto& room : finalShip.rooms) {
                left = std::min(left, room.x);
                top = std::min(top, room.y);
                right = std::max(right, room.x + room.w);
                bottom = std::max(bottom, room.y + room.h);
            }
            auto& glare = renderer_.image("img/effects/glowing_star.png");
            renderer_.draw(glare,
                           {enemyOriginX() + (left + right) / 2.f - extent / 2.f,
                            enemyOriginY() + bottom - phase * (bottom - top) - extent / 2.f,
                            extent,
                            extent});
        }
    }
    if (!enemyExplosionActive_ || explosionAge <= 1.5f) {
        // Draw the shield effect before the exterior so hull pieces can occlude it.
        if (!enemyExplosionActive_) {
            renderShieldHits(1);
        }
        painter_.draw(
            enemy_.ship,
            enemyAppearances,
            view,
            [&] {
                // Spreader<Fire>::OnRender 0x735e90 skips blacked-out rooms.
                if (view.rooms) {
                    auto visibleHazards = enemyHazards_;
                    std::erase_if(visibleHazards, [&](const auto& entry) {
                        return !enemyRoomVisible(entry.first);
                    });
                    renderHazards(enemy_.ship, visibleHazards, view.x, view.y);
                }
            },
            [&] {
                renderLockdown(enemy_.ship, enemy_.lockdownRooms, view.x, view.y);
            },
            [&] {
                renderCrewDrones(1, enemyOriginX(), enemyOriginY());
            },
            [&] {
                for (const auto& member : enemy_.crew) {
                    if (!member.intruder && enemyRoomVisible(member.room)) {
                        renderCrewHealth(member, enemyOriginX(), enemyOriginY());
                    }
                }
                for (const auto& member : crew_) {
                    if (member.intruder && enemyRoomVisible(member.boardRoom)) {
                        renderCrewHealth(member, enemyOriginX(), enemyOriginY());
                    }
                }
                renderCrewDrones(1, enemyOriginX(), enemyOriginY(), true);
            });
    }
    if (enemyExplosionActive_ && explosionDuration_ - explosion_ < 3.f) {
        renderExplosionEffects(enemy_.explosionState.effects,
                               view.x + enemy_.ship.horizontalShift,
                               view.y + enemy_.ship.verticalShift);
    }
    if (!enemyExplosionActive_) {
        renderShipMessages(1);
    }
    renderSpaceProjectiles(1, 1);
    renderSpaceDrones(1, 1);
    renderer_.endMaskedLayer();
    if (teleportArmed_ != 0 && teleportTarget_ >= 0) {
        const auto room = std::ranges::find_if(enemy_.ship.rooms, [&](const auto& candidate) {
            return candidate.id == teleportTarget_;
        });
        if (room != enemy_.ship.rooms.end()) {
            renderer_.draw(teleportArmed_ == 1 ? "img/misc/teleport_placed_send.png"
                                               : "img/misc/teleport_placed_return.png",
                           enemyOriginX() + room->x + room->w / 2.f - 20.f,
                           enemyOriginY() + room->y + room->h / 2.f - 20.f);
        }
    }
    renderWeaponAiming(1);
    if (enemy_.escaping && hostile_ && !enemyExplosionActive_) {
        const auto engine = std::ranges::find_if(enemy_.systems, [](const auto& s) {
            return s.definition.id == 1;
        });
        const auto pilot = std::ranges::find_if(enemy_.systems, [](const auto& s) {
            return s.definition.id == 6;
        });
        const bool charging = engine != enemy_.systems.end() && pilot != enemy_.systems.end() &&
                              engine->power > 0 && pilot->power > 0 && engine->hackLevel <= 1 &&
                              pilot->hackLevel <= 1;
        const bool imminent = charging && enemy_.escapeTimer <= 5.f;
        const float alpha = imminent ? 1.f - std::abs(enemy_.escapeWarningTime - .33f) / .33f : 1.f;
        renderer_.textWithGlow(texts_.get(!charging  ? "warning_ftl_delayed"
                                          : imminent ? "warning_ftl_imminent"
                                                     : "warning_ftl_charging",
                                          locale_),
                               boxX + box.w / 2,
                               boxY - 15,
                               locale_ == "zh-Hans",
                               {253, 84, 70, 255},
                               std::clamp(alpha, 0.f, 1.f),
                               true);
    }
    text(
        texts_.get("target_window", locale_), statusX - 2, statusY - 36, 62, false, {0, 0, 0, 128});
    auto statusLabel = [&](const std::string& label, float y) {
        const float width =
            std::round(renderer_.measure(localizedFont(61, label), label, textScale(61)));
        auto& left = renderer_.image("img/combatUI/box_hostiles_smallbar_left.png");
        auto& middle = renderer_.image("img/combatUI/box_hostiles_smallbar_middle.png");
        auto& right = renderer_.image("img/combatUI/box_hostiles_smallbar_right.png");
        const SDL_Color tint = boxColor;
        renderer_.sprite(left,
                         {statusX - 5, y, float(left.w), float(left.h)},
                         {0, 0, float(left.w), float(left.h)},
                         0,
                         tint);
        renderer_.sprite(middle,
                         {statusX + left.w - 5, y, width + 2 - left.w, float(middle.h)},
                         {0, 0, float(middle.w), float(middle.h)},
                         0,
                         tint);
        renderer_.sprite(right,
                         {statusX + width - 3, y, float(right.w), float(right.h)},
                         {0, 0, float(right.w), float(right.h)},
                         0,
                         tint);
        text(label, statusX - 3, y + 1, 61, false, {0, 0, 0, 128});
    };
    statusLabel(texts_.get("status_hull", locale_), statusY - 12);
    statusLabel(texts_.get("status_shields", locale_), statusY + 15);
    auto& hull = renderer_.image("img/combatUI/box_hostiles_hull2.png");
    const float hullWidth = float(hull.w) * enemy_.hull / 22;
    renderer_.sprite(hull,
                     {statusX, statusY, hullWidth, float(hull.h)},
                     {0, 0, hullWidth, float(hull.h)},
                     0,
                     {120, 255, 120, 255});
    const auto shields = std::ranges::find_if(enemy_.systems, [](const auto& system) {
        return system.definition.id == 0;
    });
    const int bubbles =
        shields == enemy_.systems.end()
            ? 0
            : std::clamp(std::min(shields->power, shields->health), 0, systemCapacity(*shields)) /
                  2;
    const bool hackedShields = shields != enemy_.systems.end() && shields->hackLevel > 1;
    for (int bubble = 0; bubble < bubbles; ++bubble) {
        const bool filled = bubble < enemy_.shields;
        renderer_.draw(hackedShields ? filled
                                           ? "img/combatUI/box_hostiles_shield2_hacked_charged.png"
                                           : "img/combatUI/box_hostiles_shield2_hacked.png"
                       : filled      ? "img/combatUI/box_hostiles_shield1.png"
                                     : "img/combatUI/box_hostiles_shield2.png",
                       statusX + 3.f + bubble * 23,
                       statusY + 30);
    }
    if (shields != enemy_.systems.end() && !enemyExplosionActive_ && enemy_.shieldCharge > 0) {
        renderer_.draw("img/combatUI/box_hostiles_shield_charge.png", statusX + 2, statusY + 54);
        renderer_.fill(
            {statusX + 5, statusY + 57, std::max(0.f, enemy_.shieldCharge / 5.f * 52.f), 6},
            hackedShields ? SDL_Color{207, 70, 253, 255} : SDL_Color{27, 132, 255, 255});
    }
    if (enemy_.superShields > 0) {
        const float x = statusX + 23 * bubbles + 13;
        if (enemy_.maxSuperShields == 5 || enemy_.maxSuperShields == 12) {
            renderer_.draw(enemy_.maxSuperShields == 12
                               ? "img/combatUI/box_hostiles_shield_super12.png"
                               : "img/combatUI/box_hostiles_shield_super5.png",
                           x,
                           statusY + 35);
        } else {
            renderer_.fill({x, statusY + 35, float(10 * enemy_.maxSuperShields + 6), 13},
                           {0, 0, 0, 128});
            const float width = float(10 * enemy_.maxSuperShields + 6);
            renderer_.fill({x, statusY + 35, width, 2}, {255, 255, 255, 255});
            renderer_.fill({x, statusY + 46, width, 2}, {255, 255, 255, 255});
            renderer_.fill({x, statusY + 35, 2, 13}, {255, 255, 255, 255});
            renderer_.fill({x + width - 2, statusY + 35, 2, 13}, {255, 255, 255, 255});
        }
        renderer_.fill({x + 3, statusY + 38, float(10 * enemy_.superShields), 7},
                       {100, 255, 100, 255});
    }
    const auto substitute = [&](const char* key, const std::string& value) {
        auto label = texts_.get(key, locale_);
        for (std::size_t start = 0; (start = label.find("\\1", start)) != std::string::npos;) {
            label.replace(start, 2, value);
            start += value.size();
        }
        return label;
    };
    const auto rightText = [&](const std::string& value, float y, SDL_Color color) {
        text(value,
             boxX + 380 + (boss ? 100 : 0) -
                 renderer_.measure(localizedFont(7, value), value, textScale(7)),
             y,
             7,
             false,
             color);
    };
    const auto hackingLevel = [&](int id) {
        const auto station = std::ranges::find_if(enemy_.systems, [id](const auto& system) {
            return system.definition.id == id;
        });
        return station == enemy_.systems.end() ? 0 : station->hackLevel;
    };
    if (hackingLevel(1) || hackingLevel(2) || hackingLevel(6)) {
        rightText(texts_.get(hackingLevel(1)   ? "engines_hacked"
                             : hackingLevel(2) ? "oxygen_hacked"
                                               : "pilot_hacked",
                             locale_),
                  boxY + 35,
                  {243, 255, 230, 255});
        float oxygen = 0;
        for (const auto& room : enemy_.ship.rooms) {
            const auto amount = enemy_.roomOxygen.find(room.id);
            if (amount != enemy_.roomOxygen.end()) {
                oxygen += amount->second;
            }
        }
        rightText(substitute(hackingLevel(2) ? "current_oxygen" : "current_evasion",
                             std::to_string(hackingLevel(2) ? int(oxygen / enemy_.ship.rooms.size())
                                                            : evasion(1))),
                  boxY + 50,
                  hackingLevel(1) > 1 || hackingLevel(2) > 1 || hackingLevel(6) > 1
                      ? SDL_Color{255, 51, 51, 255}
                      : SDL_Color{243, 255, 230, 255});
    } else {
        const auto name = enemy_.ship.shipClass;
        rightText(substitute("combat_class",
                             name.reference ? texts_.get(name.value, locale_) : name.value),
                  boxY + 35,
                  {243, 255, 230, 255});
        rightText(substitute("combat_relationship",
                             texts_.get(hostile_ ? "hostile" : "neutral", locale_)),
                  boxY + 50,
                  hostile_ ? SDL_Color{255, 51, 51, 255} : SDL_Color{243, 255, 230, 255});
    }

    renderEnemySystemHud();
}

void Run::renderWeaponAiming(int targetShip) {
    const float originX = targetShip == 0 ? playerX() + ship_.horizontalShift : enemyOriginX();
    const float originY = targetShip == 0 ? 170.f + ship_.verticalShift : enemyOriginY();
    const auto drawCircle = [&](SDL_FPoint point, int radius) {
        if (radius <= 0) {
            return;
        }
        // GL_DrawCircle598e20 is a filled disk with ten-degree segments.
        std::vector<SDL_FPoint> vertices;
        vertices.reserve(38);
        vertices.push_back({float(int(point.x)), float(int(point.y))});
        for (int angle = 0; angle <= 360; angle += 10) {
            const float radians = angle * std::numbers::pi_v<float> / 180.f;
            vertices.push_back({float(int(point.x)) + radius * std::cos(radians),
                                float(int(point.y)) + radius * std::sin(radians)});
        }
        renderer_.polygon(vertices, {255, 0, 0, 64});
    };
    const auto drawBeam = [&](SDL_FPoint from, SDL_FPoint to, bool yellow) {
        const float distance = std::hypot(to.x - from.x, to.y - from.y);
        if (int(distance) <= 0) {
            return;
        }
        auto& piece = renderer_.image(yellow ? "img/misc/beam_center_yellow.png"
                                             : "img/misc/beam_center.png");
        const float dx = (to.x - from.x) / distance;
        const float dy = (to.y - from.y) / distance;
        const double angle = std::atan2(dy, dx) * 180.0 / std::numbers::pi;
        const SDL_FPoint pivot{0, 9};
        // RenderBeamAiming552e60 repeats one-by-eighteen slices, beginning
        // one pixel before the selected start, rotated around that start.
        for (int index = 0; index < int(distance); ++index) {
            renderer_.sprite(piece,
                             {from.x + dx * (index - 1), from.y + dy * (index - 1) - 9, 1, 18},
                             {0, 0, float(piece.w), float(piece.h)},
                             angle,
                             {255, 255, 255, 255},
                             false,
                             &pivot);
        }
    };
    // RenderAiming553180 and RenderSelfAiming553450 show the factory's
    // remaining targets, one placed icon for a nonbeam regardless of count.
    for (std::size_t index = 0; index < combatWeapons_.size(); ++index) {
        const auto& weapon = combatWeapons_[index];
        if (int(weapon.targetSelf) != (targetShip == 0) || weapon.targetRoom < 0 ||
            weapon.targetFirstPending || weapon.volleyRemaining <= 0) {
            continue;
        }
        const auto& blueprint = catalog_.equipment(ship_.weapons[index]);
        const SDL_FPoint point{originX + weapon.target.x, originY + weapon.target.y};
        if (blueprint.type == "BEAM") {
            drawBeam(point,
                     {originX + weapon.targetEnd.x, originY + weapon.targetEnd.y},
                     weapon.autofire);
        } else {
            if (targetShip == 1) {
                drawCircle(point, blueprint.radius);
            }
            renderer_.draw("img/misc/crosshairs_placed" +
                               std::to_string(logicalArmamentSlot("weapon", int(index)) + 1) +
                               (weapon.autofire ? "_yellow.png" : ".png"),
                           point.x - 26,
                           point.y - 26);
        }
    }
    if (targetShip != 1 || aiming_ < 0 || aiming_ >= int(combatWeapons_.size()) || !hostile_ ||
        enemy_.hull <= 0) {
        return;
    }
    const auto& blueprint = catalog_.equipment(ship_.weapons[std::size_t(aiming_)]);
    const auto& weapon = combatWeapons_[std::size_t(aiming_)];
    const SDL_FPoint mouse{float(int(mouseX_ - originX)), float(int(mouseY_ - originY))};
    if (blueprint.type == "BEAM" && weapon.targetFirstPending) {
        const float dx = mouse.x - weapon.target.x;
        const float dy = mouse.y - weapon.target.y;
        const float distance = std::hypot(dx, dy);
        if (distance > 10.f) {
            drawBeam({originX + weapon.target.x, originY + weapon.target.y},
                     {originX + weapon.target.x + dx * blueprint.length / distance,
                      originY + weapon.target.y + dy * blueprint.length / distance},
                     autofire_ != forceAutofire_);
        }
        return;
    }
    if (enemy_.cloak.active && !enemyContainsPlayerCrew()) {
        return;
    }
    // The numbered live reticle is MouseControl's final cursor pass. Only
    // the weapon spread disk belongs to this enemy ship pass.
    for (const auto& room : enemy_.ship.rooms) {
        if (Rect{float(room.x), float(room.y), float(room.w), float(room.h)}.contains(mouse.x,
                                                                                      mouse.y)) {
            drawCircle({originX + room.x + room.w / 2.f, originY + room.y + room.h / 2.f},
                       blueprint.radius);
            break;
        }
    }
}

void Run::renderArmamentWarnings() {
    constexpr std::array<SDL_FPoint, 5> positions{
        SDL_FPoint{203, -25}, {220, -45}, {112, -25}, {115, -45}, {124, -60}};
    // WeaponControl550440 and DroneControl537980 follow all box labels and
    // precede the drag layer. All five InitText calls are centered, unflashed.
    for (std::size_t index = 0; index < armamentWarnings_.size(); ++index) {
        const auto& warning = armamentWarnings_[index];
        const int id = index < 2 ? 3 : 4;
        if (!system(id) || warning.elapsed < 0 || warning.elapsed > 2 || warning.text.empty()) {
            continue;
        }
        const auto& value = texts_.get(warning.text, locale_);
        const auto holder = systemHudPosition(id);
        const SDL_Color color =
            warning.broken ? SDL_Color{253, 84, 70, 255} : SDL_Color{235, 245, 229, 255};
        renderer_.textWithGlow(value,
                               holder.x + 32 + positions[index].x,
                               607 + positions[index].y,
                               locale_ == "zh-Hans",
                               color,
                               1.f,
                               true);
    }
}

Rect Run::gameOverButton(int index) {
    // Original GameOver::OpenText0x4c83a0, English TextButton bounds in reference016.
    constexpr std::array<Rect, 5> boxes{{{602, 446, 76, 24},
                                         {416, 482, 100, 24},
                                         {540, 482, 91, 24},
                                         {655, 482, 128, 24},
                                         {806, 482, 59, 24}}};
    return boxes.at(static_cast<std::size_t>(index));
}

void Run::gameOverRender() {
    renderer_.fill({0, 0, 1280, 720}, {0, 0, 0, 217});
    auto& box = renderer_.image("img/scoreUI/gameover_main.png");
    renderer_.sprite(box, {400, 199, 481, 276}, {0, 0, 481, 276});
    float x = 400;
    const auto strip = [&](float source, float width, float sourceWidth) {
        renderer_.sprite(box, {x, 475, width, 47}, {source, 276, sourceWidth, 47});
        x += width;
    };
    strip(0, 26, 26);
    strip(26, 80, 1);
    strip(107, 21, 21);
    strip(128, 2, 1);
    strip(128, 21, 21);
    strip(26, 71, 1);
    strip(107, 21, 21);
    strip(128, 2, 1);
    strip(128, 21, 21);
    strip(26, 108, 1);
    strip(107, 21, 21);
    strip(128, 1, 1);
    strip(128, 21, 21);
    strip(26, 39, 1);
    strip(455, 26, 26);
    const auto localizedText = [&](const std::string& value,
                                   float centerX,
                                   float top,
                                   int nativeFont,
                                   int chineseSize,
                                   SDL_Color color) {
        if (locale_ == "zh-Hans") {
            renderer_.text(renderer_.chineseFont(chineseSize, true, value),
                           value,
                           centerX,
                           top,
                           1,
                           color,
                           true);
        } else {
            text(value, centerX, top, nativeFont, true, color);
        }
    };
    localizedText(
        texts_.get(campaign_.victory ? "gameover_title_victory" : "gameover_title_gameover",
                   locale_),
        641,
        214,
        64,
        32,
        {25, 49, 51, 255});
    const std::array<std::string, 5> labels{"stats", "restart", "hangar", "mainmenu", "quit"};
    for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
        const auto rect = gameOverButton(i);
        const SDL_Color color = rect.contains(mouseX_, mouseY_) ? SDL_Color{255, 238, 128, 255}
                                                                : SDL_Color{235, 245, 229, 255};
        for (int row = 0; row < static_cast<int>(rect.h); ++row) {
            const float inset = static_cast<float>(std::max(0, 4 - std::min(row, 23 - row)));
            renderer_.fill({rect.x + inset, rect.y + row, rect.w - inset * 2, 1}, color);
        }
        localizedText(texts_.get("gameover_button_" + labels[i], locale_),
                      rect.x + rect.w / 2,
                      rect.y + 2,
                      62,
                      18,
                      {25, 49, 51, 255});
    }
    auto& font = locale_ == "zh-Hans" ? renderer_.sourceChineseFont(18, eventText_)
                                      : renderer_.font("fonts/c&cnew.font");
    float scale = locale_ == "zh-Hans" ? 1.f : 2.f;
    std::vector<std::string> lines;
    const auto wrap = [&] {
        lines = renderer_.wrappedLines(font, eventText_, 400, scale, locale_ == "zh-Hans");
    };
    wrap();
    // Keep long localized messages above the fixed score row, preserving the
    // source's normal English 34px line spacing.
    while (lines.size() > 3 && scale > .5f) {
        scale -= .1f;
        wrap();
    }
    const float lineHeight = lines.empty() ? 34.f : std::min(34.f, 100.f / float(lines.size()));
    float y = 274;
    for (const auto& line : lines) {
        renderer_.text(font, line, 640, y, scale, {255, 255, 255, 255}, true);
        y += lineHeight;
    }
    // ScoreKeeper::CalculateScore0x441b50: earned scrap, unique beacons and defeated ships.
    const float multiplier = difficulty_ == 1 ? 1.25f : difficulty_ == 2 ? 1.5f : 1.f;
    const auto baseScore = static_cast<std::int64_t>(scoreScrap_) +
                           10 * static_cast<std::int64_t>(scoreBeacons_) +
                           20 * static_cast<std::int64_t>(scoreShips_);
    const double scaledScore = multiplier * static_cast<float>(baseScore);
    const int score = static_cast<int>(std::clamp(scaledScore,
                                                  double(std::numeric_limits<int>::min()),
                                                  double(std::numeric_limits<int>::max())));
    auto scoreText = texts_.get("gameover_score", locale_);
    if (const auto position = scoreText.find("\\1"); position != std::string::npos) {
        scoreText.replace(position, 2, std::to_string(score));
    }
    if (locale_ == "zh-Hans") {
        renderer_.text(renderer_.chineseFont(24, true, scoreText),
                       scoreText,
                       640,
                       378,
                       1,
                       {255, 255, 255, 255},
                       true);
    } else {
        renderer_.text(font, scoreText, 640, 378, 3, {255, 255, 255, 255}, true);
    }
}
} // namespace ftl
