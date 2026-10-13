#include "run.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace ftl {
namespace {
constexpr SDL_Color white{235, 245, 229, 255}, green{80, 255, 80, 255}, ink{25, 49, 51, 255};
}

Font& Run::localizedFont(int size, const std::string& value) const {
    return renderer_.sourceFont(size, value, locale_ == "zh-Hans");
}

float Run::textScale(int size) const {
    return size == 0 ? 1.f : Renderer::sourceFontScale(size, locale_ == "zh-Hans");
}

float Run::textLineHeight(int size, const std::string& value) const {
    return Renderer::sourceLineHeight(size, localizedFont(size, value));
}

std::vector<std::string> Run::wrappedText(int size, const std::string& value, float width) const {
    return renderer_.wrappedLines(
        localizedFont(size, value), value, width, textScale(size), locale_ == "zh-Hans");
}

void Run::text(const std::string& s, float x, float y, int size, bool center, SDL_Color color) {
    renderer_.text(localizedFont(size, s), s, x, y, textScale(size), color, center);
}

void Run::hud(bool front) {
    if (!front) {
        crewHud();
    }
    const int weaponX = systemHud(front);
    abilityHud(front);
    if (!front) {
        for (int id : {3, 4}) {
            if (const auto* source = system(id)) {
                renderSystemHudBox(*source, systemHudPosition(id));
            }
        }
    }
    subsystemHud(front);
    // ArmamentControl5584b0 has no draw calls on its base pass.
    if (front) {
        if (system(3)) {
            weaponHud(weaponX);
        }
        droneHud();
        if (system(3)) {
            weaponHud(weaponX, ArmamentPass::Labels);
        }
        droneHud(ArmamentPass::Labels);
        renderArmamentWarnings();
        if (system(3)) {
            weaponHud(weaponX, ArmamentPass::Dragging);
        }
        droneHud(ArmamentPass::Dragging);
    }
}

void Run::shipStatusHud() {
    auto status = [&](const char* n, float x, float y) {
        renderer_.draw(std::string("img/statusUI/") + n + ".png", x, y);
    };
    const bool hullBright = hudWarnings_[0].running && hudWarnings_[0].flash <= .330000015f;
    status(hullBright ? "top_hull_red" : "top_hull", 0, 0);
    const auto hullLabel = texts_.get("status_hull", locale_);
    const float labelWidth =
        std::round(renderer_.measure(localizedFont(62, hullLabel), hullLabel, textScale(62)));
    auto& label = renderer_.image(hullBright ? "img/statusUI/top_hull_red_label.png"
                                             : "img/statusUI/top_hull_label.png");
    renderer_.sprite(label, {0, 0, 16, 28}, {0, 0, 16, 28});
    renderer_.sprite(label, {16, 0, labelWidth - 15, 28}, {16, 0, 1, 28});
    renderer_.sprite(label, {labelWidth + 1, 0, 34, 28}, {17, 0, 34, 28});
    renderer_.sprite(label, {labelWidth + 35, 0, 121 - (labelWidth + 35), 28}, {51, 0, 1, 28});
    auto& mask = renderer_.image("img/statusUI/top_hull_bar_mask.png");
    const SDL_Color hullColor = hull_ <= 9    ? SDL_Color{255, 92, 92, 255}
                                : hull_ <= 19 ? SDL_Color{255, 230, 92, 255}
                                              : SDL_Color{120, 255, 120, 255};
    renderer_.sprite(mask,
                     {11, 0, 360.f * hull_ / ship_.hull, 65},
                     {0, 0, 360.f * hull_ / ship_.hull, 65},
                     0,
                     hullColor);
    text(hullLabel, 9, 9, 62, false, ink);
    const auto* shields = system(0);
    const int shieldMaximum = shields ? std::max(0, shields->power) / 2 : 0;
    const bool shieldsHacked = shields && shields->hackActive && shields->hackLevel > 1;
    const float scrapAge =
        scrapFailureAnimation_ > 0 && animation_ >= scrapFailureAnimation_ - 1
            ? (float(animation_ - (scrapFailureAnimation_ - 1)) + uiFraction_) * Time::step
            : -1.f;
    // ShipStatus's shared flash starts on event6 and is initially stopped at0.
    const bool sharedFlashBright = scrapAge < 0 || std::fmod(scrapAge, .66000003f) <= .330000015f;
    const bool shieldBright = hudWarnings_[1].running && hudWarnings_[1].flash <= .330000015f;
    status(shieldBright         ? "top_shields4_red"
           : shieldMaximum == 0 ? "top_shields4_off"
                                : "top_shields4_on",
           0,
           43);
    if (shieldsHacked && sharedFlashBright) {
        status("top_shields4_purple", 0, 43);
    }
    if (shields) {
        for (int i = 0; i < shieldMaximum; ++i) {
            status(shieldsHacked        ? i < shieldBubbles_ ? "top_shieldsquare1_hacked_charged"
                                                             : "top_shieldsquare1_hacked"
                   : i < shieldBubbles_ ? "top_shieldsquare1_on"
                                        : "top_shieldsquare1_off",
                   30.f + i * 23,
                   47);
        }
    }
    if (superShields_ > 0) {
        // ShipStatus::OnInit 0x53c5a0 positions super-shield bars at
        // energyShieldPos (30, 76) + (3, 3). The separate box image is not
        // part of ShipStatus::RenderShields 0x539440.
        float barX = 33.f;
        for (int bar = 0; bar < superShields_; ++bar) {
            renderer_.draw(bar == 2 ? "img/statusUI/energy_shield_bar2.png"
                                    : "img/statusUI/energy_shield_bar.png",
                           barX,
                           79.f);
            barX += bar == 2 ? 18.f : 19.f;
        }
    } else if (system(0) && shieldCharge_ > 0.f) {
        // With no super shield, the same source draws normal recharge in the
        // 92-by-6 strip below the shield circles.
        renderer_.fill({33.f, 79.f, std::clamp(shieldCharge_ / 5.f, 0.f, 1.f) * 92.f, 6.f},
                       shieldsHacked ? SDL_Color{207, 70, 253, 255} : SDL_Color{27, 132, 255, 255});
    }
    const bool oxygenBright = hudWarnings_[2].running && hudWarnings_[2].flash <= .330000015f;
    const bool evadeRed = evasion() == 0;
    status(oxygenBright ? evadeRed ? "top_evade_oxygen_both_red" : "top_evade_oxygen_down_red"
           : evadeRed   ? "top_evade_oxygen_up_red"
                        : "top_evade_oxygen",
           1,
           89);
    const auto hacked = [&](int id) {
        const auto* station = system(id);
        return station && station->hackActive && station->hackLevel > 1;
    };
    const bool evadePurple = (hacked(1) || hacked(6)) && sharedFlashBright;
    const bool oxygenPurple = hacked(2) && sharedFlashBright;
    if (evadePurple) {
        status("top_evade_oxygen_up_purple", 1, 89);
    }
    if (oxygenPurple) {
        status("top_evade_oxygen_down_purple", 1, 89);
    }
    status(fuel_ <= 3 ? "top_fuel_on_red" : "top_fuel_on", 122, 43);
    status(missiles_ == 0 ? "top_missiles_on_red" : "top_missiles_on", 188, 43);
    status(parts_ == 0 ? "top_drones_on_red" : "top_drones_on", 258, 43);
    const bool scrapFailed =
        scrapAge >= 0 && scrapAge < 1.98f && std::fmod(scrapAge, .66000003f) <= .330000015f;
    status(scrapFailed ? "top_scrap_red" : "top_scrap", 377, 0);
    constexpr SDL_Color resources{243, 255, 230, 255};
    text(std::to_string(fuel_), 173, 58, 0, true, resources);
    text(std::to_string(missiles_), 240, 58, 0, true, resources);
    text(std::to_string(parts_), 307, 58, 0, true, resources);
    text(std::to_string(scrap_), 460, 23, 0, true, resources);
    constexpr std::array<SDL_FPoint, 4> feedbackPositions{
        {{465, 70}, {180, 104}, {247, 104}, {314, 104}}};
    for (const auto& message : resourceFeedback_) {
        const float age = float(animation_ - message.start) + uiFraction_;
        if (age >= 60.f) {
            continue;
        }
        // DamageMessage 0x59d080/0x59cca0: stock shadowed white images,
        // full alpha for one second, floating down ten pixels.
        const float x = feedbackPositions[message.resource].x + float(message.jitter);
        const float y = feedbackPositions[message.resource].y - 46.f + age / 6.f;
        if (message.delta != 0) {
            renderer_.draw(message.delta > 0 ? "img/numbers/Text_plus.png"
                                             : "img/numbers/Text_minus.png",
                           x - 50.f,
                           y);
        }
        float digitX = x - 32.f;
        for (const char digit : std::to_string(std::abs(message.delta))) {
            std::string digitImage = "img/numbers/Text_";
            digitImage += digit;
            digitImage += "_L.png";
            renderer_.draw(digitImage, digitX, y);
            digitX += 16.f;
        }
    }
    auto percent = [&](int value, float y, bool purple) {
        // RenderEvadeOxygen53ab50 explicitly ignores language for font10.
        auto& font = renderer_.sourceFont(10, "", false);
        const float scale = Renderer::sourceFontScale(10, false);
        const auto number = std::to_string(value);
        const SDL_Color color = purple ? SDL_Color{207, 70, 253, 255} : white;
        renderer_.text(font, number, 82 - renderer_.measure(font, number, scale), y, scale, color);
        renderer_.text(font, "%", 82, y, scale, color);
    };
    percent(evasion(), 99, evadePurple);
    percent(system(2) ? int(std::round(oxygen_)) : 0, 121, oxygenPurple);

    const auto warningLines = [&](const HudWarning& warning,
                                  SDL_FPoint origin,
                                  std::array<SDL_FPoint, 3> points,
                                  SDL_FPoint textOrigin,
                                  const std::string& bottom) {
        if (!warning.running) {
            return;
        }
        const SDL_Color color{
            253, 84, 70, static_cast<Uint8>(255.f * (warning.flash <= .330000015f ? .9f : .4f))};
        for (std::size_t index = 1; index < points.size(); ++index) {
            const auto a = points[index - 1];
            const auto b = points[index];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float length = std::hypot(dx, dy);
            const SDL_FPoint normal{-dy / length, dx / length};
            renderer_.polygon({{origin.x + a.x + normal.x, origin.y + a.y + normal.y},
                               {origin.x + b.x + normal.x, origin.y + b.y + normal.y},
                               {origin.x + b.x - normal.x, origin.y + b.y - normal.y},
                               {origin.x + a.x - normal.x, origin.y + a.y - normal.y}},
                              color);
        }
        renderer_.textWithGlow(texts_.get("warning_warning", locale_),
                               origin.x + textOrigin.x,
                               origin.y + textOrigin.y,
                               locale_ == "zh-Hans",
                               color,
                               1,
                               true);
        renderer_.textWithGlow(texts_.get(bottom, locale_),
                               origin.x + textOrigin.x,
                               origin.y + textOrigin.y + 23,
                               locale_ == "zh-Hans",
                               color,
                               1,
                               true);
    };
    warningLines(hudWarnings_[0],
                 {340, 28},
                 {{{20, 22}, {52, 54}, {142, 54}}},
                 {97, 33},
                 "warning_hull_" + std::to_string(hullWarningLevel_));
    warningLines(
        hudWarnings_[1], {110, 72}, {{{19, 21}, {40, 42}, {140, 42}}}, {93, 21}, "warning_shields");
    warningLines(
        hudWarnings_[2], {89, 123}, {{{10, 13}, {27, 47}, {127, 47}}}, {78, 26}, "warning_oxygen");
}

void Run::advanceHudUi(std::uint32_t ticks) {
    const auto functional = [](const RunSystem* station) {
        return station && station->power > 0 && station->health > 0 &&
               (!station->hackActive || station->hackLevel <= 1);
    };
    const bool fly = functional(system(1)) && functional(system(6)) && hasCrewManning(6);
    const auto* shields = system(0);
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        const auto blink = [](float& value, float& previous, float& direction) {
            previous = value;
            value += direction * Time::step;
            if (value > .75f) {
                value = 1.5f - value;
                direction = -1;
            } else if (value < 0) {
                value = -value;
                direction = 1;
            }
        };
        blink(ftlBlink_, previousFtlBlink_, ftlBlinkDirection_);
        if ((userPause_ || event_) && jumpTicks_ == 0 && arrivalTicks_ == 0) {
            blink(pauseBlink_, previousPauseBlink_, pauseBlinkDirection_);
        } else {
            previousPauseBlink_ = pauseBlink_;
        }
        // AnimationTracker59ade0 clamps at the endpoint; a looping tracker
        // restarts on the following update instead of retaining the overshoot.
        for (std::size_t index = 0; index < hudWarnings_.size(); ++index) {
            auto& warning = hudWarnings_[index];
            const float duration = index == 3 ? 3.f : 6.f;
            if (warning.running) {
                if (warning.loop && warning.elapsed == duration) {
                    warning.elapsed = 0;
                }
                warning.elapsed += Time::step;
                if (warning.elapsed > duration) {
                    warning.elapsed = duration;
                    if (!warning.loop) {
                        warning.running = false;
                        warning.elapsed = 0;
                    }
                }
            }
            if (warning.flashRunning) {
                if (warning.flash == .66000003f) {
                    warning.flash = 0;
                }
                warning.flash = std::min(warning.flash + Time::step, .66000003f);
            }
        }
        const auto start = [&](std::size_t index) {
            auto& warning = hudWarnings_[index];
            if (!warning.running) {
                warning.running = warning.flashRunning = true;
                warning.elapsed = warning.flash = 0;
                audio_.sound("warning");
            }
        };
        const auto condition = [&](std::size_t index, bool active) {
            hudWarnings_[index].loop = true;
            if (active) {
                start(index);
            } else {
                hudWarnings_[index].running = false;
                hudWarnings_[index].elapsed = 0;
            }
        };
        if (hull_ < hudLastHull_) {
            for (const int threshold : {25, 50, 75}) {
                const float crossing = float(ship_.hull) * float(threshold) / 100.f;
                if (float(hull_) <= crossing && float(hudLastHull_) > crossing) {
                    hullWarningLevel_ = threshold;
                    hudWarnings_[0].loop = threshold == 25;
                    start(0);
                    break;
                }
            }
        } else if (hull_ > hudLastHull_ && float(hull_) > float(ship_.hull) * .25f) {
            hudWarnings_[0].loop = hudWarnings_[0].running = false;
            hudWarnings_[0].elapsed = 0;
        }
        hudLastHull_ = hull_;
        condition(1, shields && shields->health <= 1 && shields->definition.power > 1);
        condition(2,
                  (system(2) ? int(std::round(oxygen_)) : 0) <= 24 &&
                      ship_.id.find("ANAEROBIC") == std::string::npos);
        condition(3, system(1) && system(1)->health == 0);
        previousFtlPullout_ = ftlPullout_;
        ftlPullout_ = std::clamp(ftlPullout_ + (fly ? -320.f : 320.f) * Time::step, -40.f, 0.f);
        shieldWarningTime_ = hudWarnings_[1].flash;
    }
}

void Run::resourceHud() {
    auto status = [&](const char* n, float x, float y) {
        renderer_.draw(std::string("img/statusUI/") + n + ".png", x, y);
    };
    const bool enginesCritical = system(1) && system(1)->health == 0;
    // FTLButton5428b0/WarningMessage5b3c70: 0.66s flash, independent of game pause.
    const bool warningBright = hudWarnings_[3].flash <= .330000015f;
    const bool redWarning = enginesCritical && warningBright;
    const float flashAlpha = warningBright ? .9f : .4f;
    const bool tutorialJumpAllowed =
        !tutorialOwned_ || tutorial_.current().name == "DAMAGE_SYSTEM_7";
    const auto functional = [](const RunSystem* station) {
        return station && station->power > 0 && station->health > 0 &&
               (!station->hackActive || station->hackLevel <= 1);
    };
    const bool enginesFunction = functional(system(1));
    const bool pilotFunction = functional(system(6)) && hasCrewManning(6);
    const bool fly = enginesFunction && pilotFunction;
    const bool jumpEnabled =
        tutorialJumpAllowed && !defeated_ && (fuel_ > 0 || !(enemy_.present && hostile_));
    const float pullout = std::lerp(previousFtlPullout_, ftlPullout_, uiAlpha_);
    if (pullout > -40.f) {
        const float blink = std::lerp(previousFtlBlink_, ftlBlink_, uiAlpha_) / .75f;
        const auto part = [&](const std::string& image, float x, float y, float alpha = 1.f) {
            auto& art = renderer_.image(image);
            renderer_.imageExcluding(
                image, {x, y + pullout, float(art.w), float(art.h)}, {536, 35, 74, 29}, 3, alpha);
        };
        part(redWarning ? "img/buttons/FTL/ftl_pullout_red.png" : "img/buttons/FTL/FTL_pullout.png",
             535,
             63);
        part(pilotFunction ? "img/buttons/FTL/FTL_pilot_on.png"
                           : "img/buttons/FTL/FTL_pilot_off1.png",
             549,
             80);
        part(enginesFunction ? "img/buttons/FTL/FTL_engine_on.png"
                             : "img/buttons/FTL/FTL_engine_off1.png",
             576,
             79);
        if (!pilotFunction) {
            part("img/buttons/FTL/FTL_pilot_off2.png", 549, 80, blink);
        }
        if (!enginesFunction) {
            part("img/buttons/FTL/FTL_engine_off2.png", 576, 79, blink);
        }
    }
    // RenderPullout5412f0 draws the masked moving panel before the fixed base.
    renderer_.draw(redWarning ? "img/buttons/FTL/FTL_base_red.png" : "img/buttons/FTL/FTL_base.png",
                   redWarning ? 518.f : 524.f,
                   redWarning ? 16.f : 22.f);
    text(texts_.get("ftl_drive", locale_),
         573,
         18,
         51,
         true,
         redWarning ? SDL_Color{253, 84, 70, 255} : white);
    if (enginesCritical) {
        // FTLButton5418e0: origin(599,96), polyline(9,11)->(64,66)->(155,66).
        const SDL_Color warning{253, 84, 70, static_cast<Uint8>(255.f * flashAlpha)};
        for (int pixel = 0; pixel < 55; ++pixel) {
            renderer_.fill({608.f + pixel, 107.f + pixel, 1, 1}, warning);
        }
        renderer_.fill({663, 162, 91, 1}, warning);
        for (int row = 0; row < 2; ++row) {
            const auto label = texts_.get(row == 0 ? "warning_warning" : "warning_engine", locale_);
            renderer_.textWithGlow(
                label, 709, 142.f + row * 23, locale_ == "zh-Hans", warning, 1, true);
        }
    }
    const bool charged = ftlCharge_ >= 85.f;
    if (charged) {
        for (int row = 0; row < 29; ++row) {
            float inset = float(std::max(0, 3 - std::min(row, 28 - row)));
            renderer_.fill({536 + inset, 35.f + row, 74 - 2 * inset, 1},
                           jumpEnabled && fly ? SDL_Color{235, 245, 0, 255}
                           : fuel_ == 0       ? SDL_Color{253, 84, 70, 255}
                                              : SDL_Color{164, 171, 160, 255});
        }
        text(texts_.get("button_jump", locale_),
             573,
             39,
             62,
             true,
             jumpEnabled && fly && Rect{536, 35, 74, 29}.contains(mouseX_, mouseY_)
                 ? SDL_Color{62, 125, 131, 255}
             : jumpEnabled && fly ? SDL_Color{37, 74, 77, 255}
                                  : ink);
        if ((!jumpEnabled || !fly) && fuel_ == 0) {
            renderer_.fill({538, 48, 70, 2}, ink);
        }
    } else if (ftlCharge_ > 0) {
        auto& loading = renderer_.image(fly ? "img/buttons/FTL/FTL_loadingbars.png"
                                            : "img/buttons/FTL/FTL_loadingbars_off.png");
        const float width = std::floor(74.f * ftlCharge_ / 85.f);
        renderer_.sprite(loading,
                         {536, 35, width, static_cast<float>(loading.h)},
                         {0, 0, width, static_cast<float>(loading.h)});
    }
    if (fly && (!charged || jumpEnabled)) {
        const float blink = std::lerp(previousFtlBlink_, ftlBlink_, uiAlpha_) / .75f;
        text(texts_.get(charged ? "ftl_ready" : "ftl_charging", locale_),
             573,
             72,
             61,
             true,
             {235, 245, static_cast<Uint8>(229.f * (1.f - blink)), 255});
    }
    const bool shipEnabled = shipButtonActive();
    status(!shipEnabled                                       ? "top_ship_off"
           : Rect{625, 22, 74, 55}.contains(mouseX_, mouseY_) ? "top_ship_select2"
                                                              : "top_ship_on",
           625,
           22);
    if (hudStoreAvailable()) {
        // CommandGui::OnInit51dcd0: TextButton(714,34,width,31), inset3,
        // stock base at702,22. OnLoop5220b0 moves Options with auto width.
        const auto hit = hudStoreRect();
        const float width = hit.w - 12;
        auto& base = renderer_.image("img/statusUI/top_store_base.png");
        const float half = std::floor(float(base.w) / 2);
        const float extra = width - 78;
        renderer_.sprite(base, {702, 22, half, float(base.h)}, {0, 0, half, float(base.h)});
        if (extra > 0) {
            renderer_.sprite(
                base, {702 + half, 22, extra, float(base.h)}, {half, 0, 1, float(base.h)});
        }
        renderer_.sprite(base,
                         {702 + half + extra, 22, float(base.w) - half, float(base.h)},
                         {half, 0, float(base.w) - half, float(base.h)});
        const SDL_Color face =
            hit.contains(mouseX_, mouseY_) ? SDL_Color{255, 230, 94, 255} : white;
        // TextButton::InitPrimitives577040 uses scanline corner strips.
        for (int row = 0; row < 3; ++row) {
            const float trim = float(3 - row);
            renderer_.fill({714 + trim, 34.f + row, width - 2 * trim, 1}, face);
            renderer_.fill({714 + trim, 64.f - row, width - 2 * trim, 1}, face);
        }
        renderer_.fill({714, 37, width, 25}, face);
        const auto label = texts_.get("button_store", locale_);
        text(label, 714 + width / 2, 34 + 15 + 2 - textLineHeight(62, label) / 2, 62, true, ink);
    }
    const auto options = hudOptionsRect();
    status(options.contains(mouseX_, mouseY_) ? "top_optionswrench_select2"
                                              : "top_optionswrench_on",
           options.x,
           options.y);
}

void Run::crewHud() {
    const auto living = crewRoster();
    for (std::size_t i = 0; i < living.size(); ++i) {
        auto& c = crew_[living[i]];
        float y = 155.f + i * 30;
        bool healthFlash =
            c.health > 0 && c.health <= 25 && (animation_ - c.lowHealthTick) % 60 < 30;
        // CrewBox::OnRender 0x504240: selectionState 2 (potential, pointed
        // by CrewControl) is (235,235,235); selection (1) takes precedence.
        const bool potential = !c.selected && (crewBoxPointed_ == int(i) || crewPointerHit(c));
        auto bc = (c.dead || c.dying) ? SDL_Color{133, 231, 237, 255}
                  : c.controlled      ? SDL_Color{255, 0, 255, 255}
                  : c.selected        ? SDL_Color{120, 255, 120, 255}
                  : c.stunTime > 0    ? SDL_Color{255, 255, 100, 255}
                  : potential         ? SDL_Color{235, 235, 235, 255}
                  : healthFlash       ? SDL_Color{255, 100, 100, 255}
                                      : SDL_Color{100, 100, 100, 255};
        if (crewBoxHover_ == int(i)) {
            // Hovered: skillBoxBackground/skillBoxOutline from the CrewBox
            // constructor 0x500840 replace the box primitives (2px lines).
            renderer_.fill({10, y, 169, 27}, {bc.r, bc.g, bc.b, 64});
            renderer_.fill({99, y + 27, 80, 119}, {bc.r, bc.g, bc.b, 64});
            renderer_.fill({10, y, 168, 2}, bc);
            renderer_.fill({10, y + 25, 91, 2}, bc);
            renderer_.fill({10, y, 2, 27}, bc);
            renderer_.fill({99, y + 27, 2, 119}, bc);
            renderer_.fill({99, y + 144, 80, 2}, bc);
            renderer_.fill({177, y, 2, 146}, bc);
        } else {
            renderer_.fill({10, y, 86, 27}, {bc.r, bc.g, bc.b, 64});
            renderer_.fill({10, y, 86, 2}, bc);
            renderer_.fill({10, y + 25, 86, 2}, bc);
            renderer_.fill({10, y, 2, 27}, bc);
            renderer_.fill({94, y, 2, 27}, bc);
        }
        float healthWidth =
            c.health > 0
                ? float(std::max(1,
                                 int(49 * c.health /
                                     (crewMaximumHealth(c.appearance.species) + c.healthBoost))))
                : 0;
        renderer_.fill(
            {43, y + 19, healthWidth, 4},
            {Uint8(c.health < crewMaximumHealth(c.appearance.species) + c.healthBoost ? 255 : 0),
             Uint8(std::min(255.f, healthWidth / 24.5f * 255)),
             0,
             255});
        // CrewBox::OnRenderSkillLevel 0x5003e0: the health warning during the
        // first half of each flash, otherwise RenderSkillUpAnimation 0x4703f0
        // at (box.x + box.w + 2, box.y) for the first running tracker.
        if (healthFlash) {
            renderer_.draw("img/warnings/health_low.png", 94, y - 5);
        } else if (const auto running = std::ranges::find_if(c.skillUpRemaining,
                                                             [](float t) {
                                                                 return t > 0;
                                                             });
                   running != c.skillUpRemaining.end()) {
            const auto slot = std::size_t(running - c.skillUpRemaining.begin());
            const bool gain = slot < 6;
            static constexpr std::array<const char*, 6> icons{
                "pilot", "engines", "shields", "weapons", "repair", "combat"};
            const SDL_Color tint =
                gain ? SDL_Color{100, 255, 100, 255} : SDL_Color{255, 50, 50, 255};
            auto& icon =
                renderer_.image("img/people/skill_" + std::string(icons[slot % 6]) + "_white.png");
            renderer_.sprite(icon,
                             {98, y, float(icon.w), float(icon.h)},
                             {0, 0, float(icon.w), float(icon.h)},
                             0,
                             tint);
            auto& arrow = renderer_.image("img/icons/arrow.png");
            renderer_.sprite(arrow,
                             {123, y + 7, float(arrow.w), float(arrow.h)},
                             {0, 0, float(arrow.w), float(arrow.h)},
                             gain ? 180 : 0,
                             tint);
        }
    }
    renderCrewRosterSkills();
    float stationY = 154.f + living.size() * 30;
    renderer_.draw("img/statusUI/button_station_base.png", 11, stationY - 4);
    renderer_.draw(Rect{23, stationY + 8, 20, 20}.contains(mouseX_, mouseY_)
                       ? "img/statusUI/button_station_assign_select2.png"
                       : "img/statusUI/button_station_assign_on.png",
                   23,
                   stationY + 8);
    renderer_.draw("img/statusUI/button_station_base.png", 52, stationY - 4);
    renderer_.draw(Rect{64, stationY + 8, 20, 20}.contains(mouseX_, mouseY_)
                       ? "img/statusUI/button_station_return_select2.png"
                       : "img/statusUI/button_station_return_on.png",
                   64,
                   stationY + 8);
    // CrewControl4f3e00 completes backgrounds/buttons, then traverses all
    // portrait icons before traversing all labels. A HUD portrait is always
    // source walk frame zero with its ordinary yellow border.
    for (std::size_t i = 0; i < living.size(); ++i) {
        const auto& member = crew_[living[i]];
        const float offset = member.appearance.species == "crystal" ? 2.f : 0.f;
        painter_.crewSprite(member.appearance, 10 + offset, 153.f + i * 30, 1, false);
    }
    for (std::size_t i = 0; i < living.size(); ++i) {
        const auto& member = crew_[living[i]];
        const bool ignoreLanguage =
            std::ranges::all_of(member.appearance.name, [](unsigned char byte) {
                return byte <= 0xd1;
            });
        auto& font = renderer_.sourceFont(
            6, member.appearance.name, !ignoreLanguage && locale_ == "zh-Hans");
        renderer_.text(font, member.appearance.name, 43, 157.f + i * 30, 1.f, {200, 200, 200, 255});
    }
    // CrewControl4f3e00 finishes its roster and clone overlays before the
    // drag rectangle; SystemControl's base pass follows this control.
    if (dragging_ && !event_ && !mapOpen_ && jumpTicks_ == 0 && aiming_ < 0 &&
        armamentAt(dragStart_.x, dragStart_.y, false) < 0 &&
        armamentAt(dragStart_.x, dragStart_.y, true) < 0 &&
        std::hypot(mouseX_ - dragStart_.x, mouseY_ - dragStart_.y) > 7) {
        Rect box{std::min(mouseX_, dragStart_.x),
                 std::min(mouseY_, dragStart_.y),
                 std::abs(mouseX_ - dragStart_.x),
                 std::abs(mouseY_ - dragStart_.y)};
        for (auto edge : std::array<Rect, 4>{{{box.x - 1, box.y - 1, box.w + 2, 2},
                                              {box.x - 1, box.y + box.h - 1, box.w + 2, 2},
                                              {box.x - 1, box.y - 1, 2, box.h + 2},
                                              {box.x + box.w - 1, box.y - 1, 2, box.h + 2}}}) {
            renderer_.fill(edge, {255, 255, 255, 255});
        }
    }
}

int Run::subsystemHudSpacing() const {
    const auto label = texts_.get("subsystems_label", locale_);
    const int width =
        int(std::round(renderer_.measure(localizedFont(62, label), label, textScale(62))));
    return std::max(0, (width - 140) / 3);
}

SDL_FPoint Run::systemHudPosition(int id) const {
    // CreateSystemBoxes4bc2b0; SystemControl position=(0,382) and the four
    // subsystem positions are retained even when a subsystem is absent.
    const auto present = [&](int type) {
        return std::ranges::any_of(systems_, [type](const auto& entry) {
            return entry.definition.id == type;
        });
    };
    if (id == 6 || id == 7 || id == 8 || id == 12) {
        const int spacing = subsystemHudSpacing();
        const int offset = id == 6   ? 0
                           : id == 7 ? 36 + spacing
                           : id == 8 ? 72 + 2 * spacing
                                     : 123 + 3 * spacing;
        return {1015.f + offset, 633.f};
    }
    int cursor = 22;
    for (const int type : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15}) {
        if (present(type)) {
            if (type == id) {
                return {float(cursor + 36), 651.f};
            }
            cursor += type == 9 || type == 10 || type == 14 || type == 15 ? 54 : 36;
        }
    }
    if (id == 3) {
        return {float(cursor + 36), 651.f};
    }
    if (present(3)) {
        cursor += ship_.weaponSlots == 2 ? 278 : ship_.weaponSlots == 3 ? 375 : 472;
    }
    return id == 4 ? SDL_FPoint{float(cursor), 651.f} : SDL_FPoint{-1.f, -1.f};
}

Rect Run::abilityButtonRect(int id) const {
    const auto position = systemHudPosition(id);
    const auto* source = system(id);
    const int power = source ? std::clamp(source->power, 1, 3) : 1;
    if (id == 10) {
        return {position.x + 46, position.y - 50 + 59 - 12 * power, 20, float(7 + 12 * power)};
    }
    if (id == 12) {
        return {position.x + 45, position.y - 27, 20, 43};
    }
    if (id == 14) {
        return {position.x + 46, position.y - 15, 20, 31};
    }
    return {position.x + 46, position.y - 5, 20, 20};
}

int Run::systemHud(bool front) {
    const int weaponX = int(systemHudPosition(3).x + 32);
    if (front) {
        return weaponX;
    }
    int usedReactor = 0;
    int usedBattery = 0;
    for (const auto& s : systems_) {
        const int id = s.definition.id;
        if ((id < 6 || id > 8) && id != 12) {
            usedReactor += s.power - s.batteryPower - s.bonusPower;
            usedBattery += s.batteryPower;
        }
    }
    const int capacity = reactorCapacity();
    const int available = std::max(0, capacity - usedReactor);
    for (int i = 0; i < ship_.reactor; ++i) {
        if (i < available) {
            renderer_.fill({12, 686.f - i * 9, 28, 7}, green);
        } else {
            const SDL_Color color =
                i >= capacity ? SDL_Color{40, 210, 230, 255} : SDL_Color{255, 255, 255, 255};
            const float y = 686.f - i * 9;
            renderer_.fill({12, y, 28, 1}, color);
            renderer_.fill({12, y + 6, 28, 1}, color);
            renderer_.fill({12, y + 1, 1, 5}, color);
            renderer_.fill({39, y + 1, 1, 5}, color);
            if (i >= capacity) {
                // GetPowerBars4b9ca0 truncates the diagonal pixel row.
                for (int column = 0; column < 28; ++column) {
                    const int row = int((1.f - float(column) / 27.f) * 6.f);
                    renderer_.fill({12.f + column, y + row, 1, 1}, color);
                }
            }
        }
    }
    renderer_.reactorWire(0, 382, ship_.reactor, available);
    for (int bar = 0; bar < batteryPower(); ++bar) {
        const bool free = bar < batteryPower() - usedBattery;
        renderer_.fill({46, 686.f - bar * 9, 20, 7}, {255, 230, 94, 255});
        if (!free) {
            renderer_.fill({47, 687.f - bar * 9, 18, 5}, {0, 0, 0, 255});
        }
    }
    // RenderPowerBar4bb6a0 connects adjacent boxes before rendering their icons.
    std::array<int, 18> boxes{};
    std::size_t count = 0;
    for (const int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15, 3, 4, 6, 7, 8, 12}) {
        if (system(id)) {
            boxes[count++] = id;
        }
    }
    const auto needsPower = [](int id) {
        return (id < 6 || id > 8) && id != 12;
    };
    for (std::size_t index = 1; index < count; ++index) {
        if (!needsPower(boxes[index])) {
            continue;
        }
        const int previous = boxes[index - 1];
        const bool cap = index + 1 == count || !needsPower(boxes[index + 1]);
        std::string wire;
        if (previous == 3) {
            wire = ship_.weaponSlots == 2   ? "456_2weapon_cap"
                   : ship_.weaponSlots == 3 ? "456_3weapon_cap"
                                            : "456_cap";
        } else {
            const bool wide = previous == 9 || previous == 10 || previous == 14 || previous == 15;
            wire = std::string(wide ? "54" : "36") + (cap ? "_cap" : "");
        }
        const auto position = systemHudPosition(previous);
        const auto path = "img/wireUI/wire_" + wire + ".png";
        renderer_.draw(path, position.x + 31, position.y + 31, 1, .5f);
        if (availablePower() > 0) {
            renderer_.draw(path, position.x + 31, position.y + 31);
        }
    }
    // SystemControl4bc040 draws this backing before every SystemBox.
    const int spacing = subsystemHudSpacing();
    auto& panel = renderer_.image("img/box_subsystems4.png");
    if (spacing == 0) {
        renderer_.draw(panel, {1022, 667, float(panel.w), float(panel.h)});
    } else {
        float x = 1022;
        const std::array<std::pair<int, int>, 7> pieces{
            {{0, 43}, {43, 1}, {44, 35}, {79, 1}, {80, 42}, {122, 1}, {123, 47}}};
        for (const auto& [offset, width] : pieces) {
            const float stretched = width == 1 ? float(spacing + 1) : float(width);
            renderer_.sprite(panel,
                             {x, 667, stretched, float(panel.h)},
                             {float(offset), 0, float(width), float(panel.h)});
            x += stretched;
        }
    }
    text(texts_.get("subsystems_label", locale_),
         1102.f + 3 * spacing / 2,
         680,
         62,
         true,
         {40, 78, 81, 255});
    return weaponX;
}

void Run::renderSystemHudBox(const RunSystem& source,
                             SDL_FPoint position,
                             bool enemy,
                             bool showPower) {
    const int id = source.definition.id;
    const bool disabled = source.health == 0;
    const bool passive = (id >= 6 && id <= 8) || id == 12;
    // RenderPowerBoxes 0x615f80 reads powerState.first for every system. Ion
    // (IonDamage 0x6174c0) lowers a passive system's raw power, and
    // CheckMaxPower 0x6150d0 only restores it once the lock ends.
    const int power = passive
                          ? (systemLocked(source) ? std::min(source.power, systemCapacity(source))
                                                  : systemCapacity(source))
                          : source.power;
    const auto suffix = disabled                                  ? "_red1.png"
                        : source.health < source.definition.power ? "_orange1.png"
                        : power > 0                               ? "_green1.png"
                                                                  : "_grey1.png";
    std::string path = "img/icons/s_" + source.definition.name + suffix;
    // SystemBox::MouseMove 0x4d3b30 selects an unlocked system (player boxes
    // only when it needs power; CombatControl::MouseMove 0x567790 also feeds
    // the enemy boxes) while the pointer is strictly inside location+(21..42);
    // RenderSystemSymbol 0x617cc0 then uses the selected "...2.png" icon
    // (ShipSystem ctor 0x61bab0 builds iconPrimitives[state][selected]).
    const bool selected = (enemy || !passive) && !systemLocked(source) &&
                          mouseX_ > position.x + 21 && mouseX_ < position.x + 42 &&
                          mouseY_ > position.y + 21 && mouseY_ < position.y + 42;
    if (selected) {
        path.replace(path.size() - 5, 1, "2");
    }
    const auto alternate = "img/colorblind/" + path.substr(4);
    if (options_.colorblind && archive_.contains(alternate)) {
        path = alternate;
    }
    // SystemBox4ca770 can expose a hacked target's power without sensors.
    showPower = showPower || (source.hackLevel > 0 && source.hackActive);
    // SystemBox4ca770 renders power bars before the system symbol.
    for (int bar = 0; showPower && bar < source.definition.power; ++bar) {
        const float shieldGap = id == 0 ? 4.f * (bar / 2) : 0.f;
        const Rect box{position.x + 24, position.y + 8 - 8.f * bar - shieldGap, 16, 6};
        const bool damaged = disabled || bar >= source.health;
        const bool battery = bar >= source.power - source.batteryPower - source.bonusPower &&
                             bar < source.power - source.bonusPower;
        const bool bonus = bar >= source.power - source.bonusPower && bar < source.power;
        const bool capped = bar >= systemCapacity(source) && !damaged;
        // RenderPowerBoxes 0x615f80 on a selected system: every intact bar not
        // lost to a temporary cap gets bar_backgroundglow twice (unless hacked
        // past level one); an unpowered damaged bar gets the red glow and a
        // capped bar the blue one (ShipSystem ctor 0x61bab0: 35x32 at -10,-12).
        if (selected) {
            const auto glow = [&](const char* name, int times) {
                for (int pass = 0; pass < times; ++pass) {
                    // GL_CreateImagePrimitive stretches the 32x32 texture to 35x32.
                    renderer_.draw(renderer_.image(std::string("img/icons/") + name),
                                   {box.x - 10, box.y - 12, 35, 32});
                }
            };
            if (!damaged && !capped) {
                if (!(source.hackActive && source.hackLevel > 1)) {
                    glow("bar_backgroundglow.png", 2);
                }
            } else if (damaged) {
                glow("bar_backgroundglow_red.png", 1);
            } else {
                glow("bar_backgroundglow_blue.png", 1);
            }
        }
        const SDL_Color color =
            damaged
                ? (options_.colorblind ? SDL_Color{227, 65, 63, 255} : SDL_Color{255, 50, 50, 255})
            : capped ? SDL_Color{133, 231, 237, 255}
            : bar < power
                ? (source.hackActive && source.hackLevel > 1 ? SDL_Color{207, 70, 253, 255}
                   : source.ion > 0                          ? SDL_Color{133, 231, 237, 255}
                   : bonus                                   ? SDL_Color{255, 255, 255, 255}
                   : options_.colorblind                     ? SDL_Color{243, 255, 238, 255}
                                                             : SDL_Color{100, 255, 100, 255})
                : SDL_Color{255, 255, 255, 255};
        if (bar >= power || damaged || capped) {
            // GetPowerBars4b9ca0 leaves the empty atlas interior transparent.
            renderer_.fill({box.x, box.y, 16, 1}, color);
            renderer_.fill({box.x, box.y + 5, 16, 1}, color);
            renderer_.fill({box.x, box.y + 1, 1, 4}, color);
            renderer_.fill({box.x + 15, box.y + 1, 1, 4}, color);
        } else {
            renderer_.fill(box, color);
        }
        if (battery && !damaged && !capped) {
            renderer_.fill({box.x, box.y, 16, 1}, {230, 110, 30, 255});
            renderer_.fill({box.x, box.y + 5, 16, 1}, {230, 110, 30, 255});
            renderer_.fill({box.x, box.y, 1, 6}, {230, 110, 30, 255});
            renderer_.fill({box.x + 15, box.y, 1, 6}, {230, 110, 30, 255});
        }
        if (damaged || capped) {
            for (int column = 0; column < 16; ++column) {
                const int row = int(std::round((1.f - float(column) / 15.f) * 5.f));
                renderer_.fill({box.x + column, box.y + row, 1, 1}, color);
            }
        }
        if (damaged && bar == source.health && source.repair > 0) {
            const float partial = std::floor(16.f * source.repair / 100.f);
            renderer_.fill({box.x + 16 - partial, box.y, partial, 6}, {255, 255, 50, 255});
        }
        // ShipSystem::RenderPowerBoxes 0x615f80: fDamageOverTime (sabotage or
        // fire) fills partialDamageRect from the left of the top intact bar in
        // COLOR_RED (colorblind COLOR_CB_RED is not applied to this rect).
        if (!damaged && bar == source.health - 1 && source.fireDamage > 0) {
            const float partial = float(int(16.f * source.fireDamage / 100.f));
            renderer_.fill({box.x, box.y, partial, 6}, {255, 50, 50, 255});
        }
    }
    const bool levelBoostable = id == 7 || id == 8;
    const auto blockedBoost = [&](bool countLimit) {
        return (countLimit && source.powerLimit < source.definition.power + int(levelBoostable)) ||
               source.occupied || source.onFire || source.ion > 0 || source.hackLevel > 1 ||
               source.health < source.definition.power || power <= 0 ||
               (source.hackLevel > 0 && source.hackActive) || source.powerLoss > 0 ||
               source.powerDivide > 1;
    };
    if (showPower && (id == 0 || id == 1 || id == 3 || id == 6 || levelBoostable) &&
        !blockedBoost(!levelBoostable)) {
        std::string image;
        if (levelBoostable) {
            image = source.manningLevel > 0 ? "manning_bar_on"
                    : blockedBoost(true)    ? "manning_bar_ion"
                                            : "manning_bar_off";
        } else {
            image = source.manningLevel <= 0   ? "manning_outline"
                    : source.manningLevel >= 3 ? "manning_yellow"
                    : source.manningLevel == 2 ? "manning_green"
                                               : "manning_white";
        }
        const auto asset = "img/systemUI/" + image + ".png";
        const auto colorblindAsset = "img/colorblind/systemUI/" + image + ".png";
        // RenderPowerBoxes615f00 places every human glyph at the same
        // primitive origin, including the unmanned outline. Shield pairs add
        // four pixels to the stack; level-boostable bars use their own origin.
        const float shieldGap = id == 0 && source.definition.power > 0
                                    ? 4.f * ((source.definition.power - 1) / 2)
                                    : 0.f;
        renderer_.draw(
            options_.colorblind && archive_.contains(colorblindAsset) ? colorblindAsset : asset,
            position.x + (levelBoostable ? 24.f : 25.f),
            position.y - 8.f * source.definition.power - shieldGap + (levelBoostable ? 7.f : -8.f));
    }
    if (showPower && source.definition.power > 0) {
        // RenderPowerBoxes 0x615f80 status stack, from the top of the power bars
        // (the boost glyph never shows with any of these states).
        const float shieldGap = id == 0 ? 4.f * ((source.definition.power - 1) / 2) : 0.f;
        float top = position.y + 16.f - 8.f * source.definition.power - shieldGap;
        const bool hackShown = source.hackLevel > 0 && source.hackActive;
        if (source.lockCount > 0 || source.lockCount == -1 || source.hackLevel > 1) {
            // lockOutline: 2px around the bars, 3px clear of them, ion blue,
            // white for an ability's indefinite lock, purple while strong-hacked.
            SDL_Color lockInk = source.lockCount == -1 ? SDL_Color{255, 255, 255, 255}
                                                       : SDL_Color{133, 231, 237, 255};
            std::string lock = source.lockCount == -1 ? "img/icons/locking/s_lock_white.png"
                                                      : "img/icons/locking/s_lock.png";
            if (source.hackActive && source.hackLevel > 1) {
                lockInk = {207, 70, 253, 255};
                lock.clear();
            }
            const Rect outline{position.x + 21, top - 3, 22, position.y + 20 - top};
            renderer_.fill({outline.x, outline.y, outline.w, 2}, lockInk);
            renderer_.fill({outline.x, outline.y + outline.h - 2, outline.w, 2}, lockInk);
            renderer_.fill({outline.x, outline.y + 2, 2, outline.h - 4}, lockInk);
            renderer_.fill({outline.x + outline.w - 2, outline.y + 2, 2, outline.h - 4}, lockInk);
            top -= 3;
            if (!lock.empty()) {
                renderer_.draw(lock, position.x + 20, top - 24);
                top -= 21;
            }
        }
        if (hackShown) {
            renderer_.draw("img/icons/s_hacked.png", position.x + 16, top - 27);
            top -= 22;
        }
        if (source.occupied && source.fireDamage != 0 && source.health > 0) {
            renderer_.draw("img/icons/s_sabatoge.png", position.x + 15, top - 29);
            top -= 24;
        }
        if (source.onFire) {
            renderer_.draw("img/icons/s_fire2.png", position.x + 15, top - 27);
        }
    }
    if (!enemy && source.lockCount > 0) {
        // SystemBox::OnRender 0x4ca770 (player boxes pass ignoreStatus=false):
        // a positive lock replaces the system symbol with timerCircle[count]
        // and the timer lines inside a pie of 30-degree steps that shrinks as
        // lockTimer runs (lastTimerStencilCount = int(12 - 12*t + 1)).
        const std::string shape = passive ? "s_octa_" : "s_ring_";
        renderer_.draw("img/icons/locking/" + shape +
                           std::to_string(std::min(source.lockCount, 9)) + "_base.png",
                       position.x,
                       position.y);
        const float fraction = std::clamp(source.lockElapsed / 5.f, 0.f, 1.f);
        const int steps = int(12.f - fraction * 12.f + 1.f);
        renderer_.texturedPie("img/icons/locking/" + shape + "timer.png",
                              position,
                              {32, 32},
                              17,
                              -85,
                              float(30 * steps - 85));
        return;
    }
    if (enemy && !showPower && options_.colorblind) {
        if (disabled) {
            renderer_.draw(
                "img/colorblind/icons/icon_broken_x_large.png", position.x + 27, position.y + 7);
        } else if (source.ion > 0 || source.hackLevel > 1) {
            renderer_.draw("img/icons/locking/s_lock.png", position.x + 20, position.y - 5);
        }
    }
    if (archive_.contains(path)) {
        // RenderSystemSymbol 0x617cc0 renders a selected symbol three times.
        for (int pass = 0; pass < (selected ? 3 : 1); ++pass) {
            renderer_.draw(path, position.x, position.y);
        }
    }
}

void Run::abilityHud(bool front) {
    // Desktop SystemBox.tapped is false; only the original touch handler sets it.
    if (front) {
        return;
    }
    const auto drawButton = [&](const std::string& prefix,
                                SDL_FPoint point,
                                Rect hit,
                                bool ready,
                                bool selected = false) {
        const auto suffix =
            ready ? (selected || hit.contains(mouseX_, mouseY_) ? "_select2.png" : "_on.png")
                  : "_off.png";
        renderer_.draw("img/systemUI/" + prefix + suffix, point.x, point.y);
    };
    const auto cooldownBox =
        [&](SDL_FPoint position, int level, float fraction, const std::string& base = "") {
            if (level < 0) {
                return;
            }
            level = std::clamp(level, 1, 4);
            const std::string prefix = "img/systemUI/button_cloaking" + std::to_string(level);
            renderer_.draw(
                base.empty() ? prefix + "_base.png" : base, position.x + 36, position.y - 50);
            if (fraction > 0) {
                auto& charge = renderer_.image(prefix + "_charging_on.png");
                const float top = float(66 - 4 * int(std::ceil(fraction * (3 * level + 2))));
                const float clipped = std::clamp(top, 0.f, float(charge.h));
                renderer_.sprite(charge,
                                 {position.x + 36,
                                  position.y + 42 - float(charge.h) + clipped,
                                  float(charge.w),
                                  float(charge.h) - clipped},
                                 {0, clipped, float(charge.w), float(charge.h) - clipped});
            }
        };
    for (int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15}) {
        const auto* source = system(id);
        if (!source) {
            continue;
        }
        const auto position = systemHudPosition(id);
        renderSystemHudBox(*source, position);
        const bool functioning = source->health > 0 && source->power > 0;
        const bool unlocked = source->ion <= 0 && (!source->hackActive || source->hackLevel <= 1);
        if (id == 10) {
            const int level = std::clamp(source->power, 1, 3);
            cooldownBox(position,
                        level,
                        cloak_.active && cloak_.duration > 0
                            ? 1.f - cloak_.elapsed / cloak_.duration
                            : -1.f);
            if (!cloak_.active) {
                drawButton("button_cloaking" + std::to_string(level),
                           {position.x + 36, position.y - 50},
                           abilityButtonRect(id),
                           functioning && unlocked && cloak_.cooldown <= 0);
            }
        } else if (id == 14) {
            cooldownBox(position,
                        2,
                        mind_.active && mind_.duration > 0 ? 1.f - mind_.elapsed / mind_.duration
                                                           : -1.f);
            if (!mind_.active) {
                drawButton("button_cloaking2",
                           {position.x + 36, position.y - 50},
                           abilityButtonRect(id),
                           functioning && unlocked && mind_.cooldown <= 0 && enemy_.present &&
                               hostile_,
                           abilityAim_ == "mind");
            }
        } else if (id == 11) {
            // ArtilleryBox::OnRender 0x4cb660 (SystemControl::CreateSystemBoxes
            // 0x4bc2b0 builds it for the player only): buttonOffset is
            // location+(35,-40); button_artillery_<GetEffectivePower, 0 as 1>
            // and a 5px charge bar whose full height is that power's cooldown,
            // white or COLOR_PURPLE_HACKING while strongly hacked.
            const auto& weapon = source->artillery;
            const auto& blueprint = catalog_.equipment(source->definition.weapon);
            const int power = std::max(1, source->power);
            const float cooldown =
                weapon.cooldown > 0
                    ? weapon.cooldown
                    : std::max(.1f,
                               blueprint.cooldown *
                                   (1.5f - std::clamp(source->power, 0, source->health) * .25f));
            const int height =
                int(weapon.charge / cooldown * ((1.5f - float(power) * .25f) * blueprint.cooldown));
            const SDL_FPoint button{position.x + 35, position.y - 40};
            // ftl.dat ships frames 1-4; a missing GetImageId texture blits nothing.
            const auto frame = "img/systemUI/button_artillery_" + std::to_string(power) + ".png";
            if (archive_.contains(frame)) {
                renderer_.draw(frame, button.x, button.y);
            }
            renderer_.fill({button.x + 9, button.y + float(59 - height), 5, float(height)},
                           source->hackLevel > 1 && source->hackActive
                               ? SDL_Color{207, 70, 253, 255}
                               : SDL_Color{255, 255, 255, 255});
        } else if (id == 9) {
            const SDL_FPoint button{position.x + 35, position.y - 36};
            renderer_.draw("img/systemUI/button_teleport_base.png", button.x, button.y);
            const bool barrier =
                enemy_.superShields > 0 &&
                std::ranges::find(ship_.augments, "ZOLTAN_BYPASS") == ship_.augments.end();
            const bool prepared = std::ranges::any_of(crew_, [&](const auto& member) {
                return !member.dead && !member.dying && !member.intruder && !member.controlled &&
                       member.path.empty() && member.room == source->definition.room;
            });
            drawButton("button_teleport_top",
                       button,
                       {button.x + 10, button.y + 10, 20, 20},
                       functioning && unlocked && enemy_.present && !cloaked(1) && !barrier &&
                           prepared,
                       abilityAim_ == "teleport_send");
            drawButton("button_teleport_bottom",
                       button,
                       {button.x + 10, button.y + 33, 20, 20},
                       functioning && unlocked && enemy_.present && !cloaked(1) && !barrier,
                       abilityAim_ == "teleport_return");
        } else if (id == 15) {
            const int level = std::clamp(source->power, 1, 3);
            if (hackingTarget_ >= 0) {
                cooldownBox(position,
                            level,
                            hacking_.active && hacking_.duration > 0
                                ? 1.f - hacking_.elapsed / hacking_.duration
                                : -1.f);
                if (!hacking_.active) {
                    drawButton("button_cloaking" + std::to_string(level),
                               {position.x + 36, position.y - 41},
                               {position.x + 46,
                                position.y - 41 + 50 - 12 * level,
                                20,
                                float(7 + 12 * level)},
                               functioning && unlocked && hacking_.cooldown <= 0 &&
                                   hackingDrone_.delivered);
                }
            } else {
                const SDL_FPoint button{position.x + 36, position.y - 39};
                renderer_.draw("img/systemUI/button_hack_base.png", button.x, button.y);
                drawButton("button_hack_drone",
                           {position.x + 36, position.y - 41},
                           {position.x + 46, position.y - 5, 20, 20},
                           functioning && unlocked && parts_ > 0 && enemy_.present && hostile_,
                           abilityAim_ == "hack");
            }
        }
    }
}

Rect Run::autofireRect() const {
    const auto label = texts_.get("button_autofire", locale_);
    const float width =
        std::round(renderer_.measure(localizedFont(62, label), label, textScale(62))) + 8;
    return {systemHudPosition(3).x + 32 + (ship_.weaponSlots == 3 ? 311.f : 408.f) - width,
            670,
            width,
            24};
}

bool Run::armamentDragging(bool drone, int dense) const {
    return dense >= 0 && dragging_ && !event_ && !mapOpen_ && jumpTicks_ == 0 &&
           armamentAt(dragStart_.x, dragStart_.y, drone) == dense &&
           int(std::hypot(mouseX_ - dragStart_.x, mouseY_ - dragStart_.y)) > 6;
}

void Run::armamentLabels(const EquipmentDefinition& blueprint,
                         int key,
                         SDL_FPoint position,
                         bool drone,
                         SDL_Color color) {
    const auto title = blueprint.shortTitle.reference
                           ? texts_.get(blueprint.shortTitle.value, locale_)
                           : blueprint.shortTitle.value;
    float y = position.y + 3;
    for (const auto& line : wrappedText(7, title, drone ? 10049.f : 55.f)) {
        text(line, position.x + (drone ? 26.f : 34.f), y, 7, false, color);
        y += textLineHeight(7, title);
    }
    if (key >= 0) {
        const int offset = key == 1 ? 5 : key == 4 || key == 7 ? 3 : 4;
        text(std::to_string(key), position.x + 83 + offset, position.y + 25, 5, false, color);
    }
}

void Run::weaponHud(int x, ArmamentPass pass) {
    if (pass == ArmamentPass::Body) {
        auto& holder =
            renderer_.image("img/box_weapons_bottom" + std::to_string(ship_.weaponSlots) + ".png");
        renderer_.draw(holder, {float(x), 607, float(holder.w), float(holder.h)});
        auto& wl = renderer_.image("img/box_weapons_bottom_label.png");
        const auto label = texts_.get("weapons_label", locale_);
        const float labelWidth =
            std::round(renderer_.measure(localizedFont(62, label), label, textScale(62)));
        renderer_.sprite(wl, {float(x + 18), 668, 3, 27}, {0, 0, 3, 27});
        renderer_.sprite(wl, {float(x + 21), 668, labelWidth - 11, 27}, {3, 0, 1, 27});
        renderer_.sprite(wl, {x + labelWidth + 10, 668, 35, 27}, {4, 0, 35, 27});
        renderer_.sprite(
            wl, {x + labelWidth + 45, 668, holder.w - labelWidth - 58, 27}, {39, 0, 1, 27});
        renderer_.sprite(wl, {float(x + holder.w - 13), 668, 13, 27}, {40, 0, 13, 27});
        text(label, float(x + 19), 667, 62, false, {40, 78, 81, 255});
    }
    auto outline = [&](Rect box, SDL_Color c, float thickness) {
        renderer_.fill({box.x, box.y, box.w, thickness}, c);
        renderer_.fill({box.x, box.y + box.h - thickness, box.w, thickness}, c);
        renderer_.fill({box.x, box.y, thickness, box.h}, c);
        renderer_.fill({box.x + box.w - thickness, box.y, thickness, box.h}, c);
    };
    float maxCooldown = 1;
    for (auto& id : ship_.weapons) {
        maxCooldown = std::max(maxCooldown, catalog_.equipment(id).cooldown);
    }
    for (int i = 0; i < ship_.weaponSlots; ++i) {
        float wx = float(x + 16 + 97 * i);
        float wy = 623;
        const int dense = logicalArmamentIndex("weapon", i);
        const bool dragged = armamentDragging(false, dense);
        if (pass == ArmamentPass::Dragging ? !dragged : dragged) {
            continue;
        }
        if (pass == ArmamentPass::Dragging) {
            wx += mouseX_ - dragStart_.x;
            wy += mouseY_ - dragStart_.y;
        }
        bool equipped = dense >= 0;
        bool ready = false;
        if (equipped && dense < static_cast<int>(combatWeapons_.size())) {
            const auto& state = combatWeapons_[dense];
            const auto& blueprint = catalog_.equipment(ship_.weapons[dense]);
            ready =
                blueprint.chargeLevels > 1
                    ? state.chargeLevel >= std::max(1, state.goalChargeLevel)
                    : state.charge >= (state.cooldown > 0 ? state.cooldown : blueprint.cooldown);
        }
        SDL_Color border = equipped ? (weaponPower_[dense] ? (ready ? SDL_Color{120, 255, 120, 255}
                                                                    : SDL_Color{243, 255, 230, 255})
                                                           : SDL_Color{150, 150, 150, 255})
                                    : SDL_Color{190, 190, 190, 255};
        if (equipped) {
            // WeaponBox::StatusColor4f5920 uses armed/armedForAutofire input
            // flags. Queued targets color only the separate charge bar.
            if (weaponPower_[dense] && aiming_ == dense) {
                border = autofire_ != forceAutofire_ ? SDL_Color{255, 230, 94, 255}
                                                     : SDL_Color{255, 120, 120, 255};
            }
            if (weaponPower_[dense] && system(3) && system(3)->hackActive &&
                system(3)->hackLevel > 1) {
                border = {207, 70, 253, 255};
            }
        }
        if (pass == ArmamentPass::Labels) {
            if (equipped) {
                armamentLabels(catalog_.equipment(ship_.weapons[dense]),
                               weaponKeyLabels_.at(i),
                               {wx, wy},
                               false,
                               border);
            }
            continue;
        }
        renderer_.fill({wx + (equipped ? 8 : 0), wy, equipped ? 87.f : 95.f, 39},
                       {100, 100, 100, 64});
        if (!equipped) {
            outline({wx + 8, wy, 87, 39}, border, 2);
            continue;
        }
        // ArmamentBox::MouseMove 0x4fae80 hovers a non-empty 95x39 box (strict
        // bounds); RenderBox 0x4f9600 then draws hoverHighlight
        // (bar_weapon_minibox_glow at -10,-10, ctor 0x4fb6d0) in StatusColor
        // before the outline, unless the tutorial flashes this power box.
        const bool tutorialFlash = tutorialOwned_ && tutorial_.running() &&
                                   ((tutorial_.current().name == "COMBAT_1" && i == 0) ||
                                    (tutorial_.current().name == "COMBAT_5" && i == 2));
        if (pass != ArmamentPass::Dragging && !tutorialFlash && mouseX_ > wx && mouseX_ < wx + 95 &&
            mouseY_ > wy && mouseY_ < wy + 39) {
            auto& glow = renderer_.image("img/bar_weapon_minibox_glow.png");
            renderer_.sprite(glow,
                             {wx - 10, wy - 10, float(glow.w), float(glow.h)},
                             {0, 0, float(glow.w), float(glow.h)},
                             0,
                             border);
        }
        renderer_.fill({wx + 8, wy, 2, 39}, border);
        renderer_.fill({wx + 8, wy, 87, 2}, border);
        renderer_.fill({wx + 93, wy, 2, 39}, border);
        renderer_.fill({wx, wy + 37, 95, 2}, border);
        auto& e = catalog_.equipment(ship_.weapons[dense]);
        // SetWeapon's combatIcon belongs to ArmamentBox's large-icon view;
        // WeaponBox::RenderBox 0x4f6960 does not draw it in this HUD view.
        if (e.boostCount > 0 && (e.boostType == "cooldown" || e.boostType == "damage") &&
            dense < int(combatWeapons_.size())) {
            // WeaponBox::OnRender 0x4f6960: clock for cooldown chains,
            // arrow for damage chains, with a capped boost-level numeral.
            const int level =
                weaponPower_[dense] ? std::min(e.boostCount, combatWeapons_[dense].boostLevel) : 0;
            const bool cooldown = e.boostType == "cooldown";
            renderer_.draw(std::string("img/combatUI/icon_") + (cooldown ? "clock_" : "arrow_") +
                               (level > 0 ? "on.png" : "off.png"),
                           wx + (cooldown ? 68.f : 72.f),
                           wy + 3.f);
            if (level > 0) {
                text(std::to_string(level), wx + 84.f, wy + 4.f, 7, false, border);
            }
        }
        if (e.chargeLevels > 1 && dense < int(combatWeapons_.size())) {
            // WeaponBox::RenderBox 0x4f6960 draws one archived chain pip per
            // charge level: used levels use icon_chain_use, unused powered
            // levels icon_chain_on, and unused unpowered levels icon_chain_off.
            const auto& weapon = combatWeapons_[dense];
            const std::string pip = weaponPower_[dense] ? "on" : "off";
            const float pipY = wy + (e.missiles > 0 ? 26.f : 25.f);
            const float pipX = wx + float(34 + ((6 * (4 - e.chargeLevels)) >> 1));
            for (int level = 0; level < e.chargeLevels; ++level) {
                const auto state = weapon.chargeLevel > level ? "use" : pip;
                const auto path = "img/combatUI/icon_chain_" + std::string(state) + ".png";
                if (archive_.contains(path)) {
                    renderer_.draw(path, pipX + 11.f * level, pipY);
                }
            }
        }
        for (int bar = 0; bar < e.power; ++bar) {
            if (weaponPower_[dense]) {
                renderer_.fill({wx + 12, wy + 28 - bar * 8, 16, 7}, border);
            } else {
                outline({wx + 12, wy + 28 - bar * 8, 16, 7}, border, 1);
            }
        }
        const float cooldownHeight = std::max(1.f, std::floor(e.cooldown / maxCooldown * 33));
        if (dense < static_cast<int>(combatWeapons_.size()) && combatWeapons_[dense].cooldown > 0) {
            const auto& w = combatWeapons_[dense];
            const float height =
                std::clamp(cooldownHeight * w.charge / w.cooldown, 0.f, cooldownHeight);
            // WeaponBox::RenderBox 0x4f6960 inherits StatusColor for an
            // untargeted charge bar, then overrides queued manual/autofire targets.
            SDL_Color chargeColor = border;
            if (w.targetRoom >= 0 || !w.targetRooms.empty()) {
                chargeColor =
                    w.autofire ? SDL_Color{255, 230, 94, 255} : SDL_Color{255, 120, 120, 255};
            }
            renderer_.fill({wx + 3, wy + 36 - height, 4, height}, chargeColor);
        }
        outline({wx, wy + 33 - cooldownHeight, 10, cooldownHeight + 6}, border, 2);
        // UpdateCooldownBox 0x4f6430 closes the space above a shorter charge
        // column with a triangle and the remainder of an eight-pixel cap.
        const float missing = 33 - cooldownHeight;
        if (missing > 0) {
            const float cap = std::min(8.f, missing);
            renderer_.polygon({{wx + 1, wy + missing},
                               {wx + cap + 1, wy + missing - cap},
                               {wx + cap + 1, wy + missing}},
                              border);
            if (cap < 8) {
                renderer_.fill({wx + cap + 1, wy + missing - cap, 8 - cap, cap}, border);
            }
        }
        outline({wx + 83, wy + 24, 12, 15}, border, 2);
        if (e.missiles) {
            renderer_.draw(
                "img/Missile.png", wx + 39, wy + 10, 1, weaponPower_[dense] ? 1.f : 150.f / 255);
        }
        if (pass == ArmamentPass::Dragging) {
            armamentLabels(e, weaponKeyLabels_.at(i), {wx, wy}, false, border);
        }
    }
    if (pass == ArmamentPass::Body && ship_.weaponSlots > 2) {
        const auto button = autofireRect();
        auto& af = renderer_.image("img/box_weapons_autofire_base.png");
        renderer_.sprite(af, {button.x - 11, button.y - 10, 17, 50}, {0, 0, 17, 50});
        renderer_.sprite(af, {button.x + 6, button.y - 10, button.w - 11, 50}, {17, 0, 1, 50});
        renderer_.sprite(af, {button.x + button.w - 5, button.y - 10, 19, 50}, {18, 0, 19, 50});
        // WeaponControl::SetAutofiring 0x553640 sets the selected (hover) colour
        // to the active colour blended 40% toward white; MouseMove 0x554ef0
        // hovers the button only while no armament is being dragged.
        const bool autofireHovered =
            button.contains(mouseX_, mouseY_) &&
            !(dragging_ && armamentAt(dragStart_.x, dragStart_.y, false) >= 0);
        const SDL_Color autofireFace =
            autofire_
                ? (autofireHovered ? SDL_Color{255, 240, 158, 255} : SDL_Color{255, 230, 94, 255})
                : (autofireHovered ? SDL_Color{200, 205, 198, 255} : SDL_Color{164, 171, 160, 255});
        for (int row = 0; row < 24; ++row) {
            float inset = float(std::max(0, 4 - std::min(row, 23 - row)));
            renderer_.fill({button.x + inset, button.y + row, button.w - 2 * inset, 1},
                           autofireFace);
        }
        text(texts_.get("button_autofire", locale_),
             button.x + button.w / 2,
             button.y + 2,
             62,
             true,
             ink);
    }
}

void Run::droneHud(ArmamentPass pass) {
    const auto* system = this->system(4);
    if (!system || ship_.droneSlots <= 0) {
        return;
    }
    // DroneControl::OnRender 0x5392a0 shares the ArmamentControl holder.
    // LinkShip 0x559330 sizes it by installed slots, including empty ones.
    const float holderX = systemHudPosition(4).x + 32;
    if (pass == ArmamentPass::Body) {
        auto& holder = renderer_.image("img/box_weapons_bottom" +
                                       std::to_string(std::clamp(ship_.droneSlots, 2, 4)) + ".png");
        renderer_.draw(holder, {holderX, 607, float(holder.w), float(holder.h)});
        auto& label = renderer_.image("img/box_weapons_bottom_label.png");
        const auto holderLabel = texts_.get("drones_label", locale_);
        const float labelWidth = std::round(
            renderer_.measure(localizedFont(62, holderLabel), holderLabel, textScale(62)));
        renderer_.sprite(label, {holderX + 18, 668, 3, 27}, {0, 0, 3, 27});
        renderer_.sprite(label, {holderX + 21, 668, labelWidth - 11, 27}, {3, 0, 1, 27});
        renderer_.sprite(label, {holderX + labelWidth + 10, 668, 35, 27}, {4, 0, 35, 27});
        renderer_.sprite(label,
                         {holderX + labelWidth + 45, 668, holder.w - labelWidth - 58, 27},
                         {39, 0, 1, 27});
        renderer_.sprite(label, {holderX + holder.w - 13, 668, 13, 27}, {40, 0, 13, 27});
        text(holderLabel, holderX + 19, 667, 62, false, {40, 78, 81, 255});
    }
    for (int index = 0; index < ship_.droneSlots; ++index) {
        const int dense = logicalArmamentIndex("drone", index);
        const bool equipped = dense >= 0;
        const bool dragged = armamentDragging(true, dense);
        if (pass == ArmamentPass::Dragging ? !dragged : dragged) {
            continue;
        }
        float x = holderX + 16.f + 97.f * index;
        float wy = 623;
        if (pass == ArmamentPass::Dragging) {
            x += mouseX_ - dragStart_.x;
            wy += mouseY_ - dragStart_.y;
        }
        if (!equipped) {
            if (pass == ArmamentPass::Labels) {
                continue;
            }
            renderer_.fill({x, wy, 95, 39}, {100, 100, 100, 64});
            // ArmamentBox::RenderBox 0x4f9600 (via DroneBox::RenderBox 0x556010)
            // draws emptyBackground plus the grey (190,190,190) emptyOutline.
            const SDL_Color grey{190, 190, 190, 255};
            renderer_.fill({x + 8, wy, 87, 2}, grey);
            renderer_.fill({x + 8, wy + 37, 87, 2}, grey);
            renderer_.fill({x + 8, wy, 2, 39}, grey);
            renderer_.fill({x + 93, wy, 2, 39}, grey);
            continue;
        }
        const auto& drone = combatDrones_[dense];
        const auto& blueprint = catalog_.equipment(drone.blueprint);
        // DroneBox::StatusColor 0x555220 uses the archived green/grey
        // primitive colors and the (207,70,253) hacking tint when an
        // occupied, powered drone has a level-two-or-three hack effect.
        const SDL_Color color =
            !drone.powered ? (drone.rebuildTimer > 0 ? SDL_Color{255, 100, 100, 255}
                                                     : SDL_Color{150, 150, 150, 255})
            : system->hackActive && system->hackLevel > 1 ? SDL_Color{207, 70, 253, 255}
                                                          : SDL_Color{120, 255, 120, 255};
        if (pass == ArmamentPass::Labels) {
            armamentLabels(blueprint, droneKeyLabels_.at(index), {x, wy}, true, color);
            continue;
        }
        renderer_.fill({x + 8, wy, 87, 39}, {100, 100, 100, 64});
        // DroneBox::RenderBox 0x556010 defers to ArmamentBox::RenderBox 0x4f9600,
        // which draws the hover glow before the outline (see weaponHud).
        if (pass != ArmamentPass::Dragging && mouseX_ > x && mouseX_ < x + 95 && mouseY_ > wy &&
            mouseY_ < wy + 39) {
            auto& glow = renderer_.image("img/bar_weapon_minibox_glow.png");
            renderer_.sprite(glow,
                             {x - 10, wy - 10, float(glow.w), float(glow.h)},
                             {0, 0, float(glow.w), float(glow.h)},
                             0,
                             color);
        }
        renderer_.fill({x + 8, wy, 87, 2}, color);
        renderer_.fill({x + 8, wy, 2, 39}, color);
        renderer_.fill({x + 93, wy, 2, 39}, color);
        renderer_.fill({x, wy + 37, 95, 2}, color);
        // ArmamentBox::RenderBox 0x4f9600 uses 16x7 power bars with an
        // eight-pixel vertical pitch. Each drone sizes to its blueprint.
        for (int bar = 0; bar < blueprint.power; ++bar) {
            const float barY = wy + 28.f - bar * 8.f;
            if (drone.powered) {
                renderer_.fill({x + 12, barY, 16, 7}, color);
            } else {
                renderer_.fill({x + 12, barY, 16, 1}, color);
                renderer_.fill({x + 12, barY + 6, 16, 1}, color);
                renderer_.fill({x + 12, barY, 1, 7}, color);
                renderer_.fill({x + 27, barY, 1, 7}, color);
            }
        }
        if (drone.deployed) {
            // DroneBox::RenderBox 0x556010 overlays the 32x32 drone glyph
            // at local (34,10) after drawing an occupied armament box.
            auto& deployed = renderer_.image("img/ui_icons/icon_drones.png");
            renderer_.sprite(deployed, {x + 34, wy + 10, 32, 32}, {0, 0, 32, 32}, 0.f, color);
        }
        renderer_.fill({x + 83, wy + 24, 12, 15}, {30, 49, 51, 220});
        renderer_.fill({x + 83, wy + 24, 12, 1}, color);
        renderer_.fill({x + 83, wy + 38, 12, 1}, color);
        if (pass == ArmamentPass::Dragging) {
            armamentLabels(blueprint, droneKeyLabels_.at(index), {x, wy}, true, color);
        }
    }
}

void Run::subsystemHud(bool front) {
    if (front) {
        return;
    }
    for (int id : {6, 7, 8, 12}) {
        const auto* source = system(id);
        if (!source) {
            continue;
        }
        const auto position = systemHudPosition(id);
        renderSystemHudBox(*source, position);
        if (id == 8) {
            const SDL_FPoint button{position.x + 35, position.y - 36};
            renderer_.draw("img/systemUI/button_door_base.png", button.x, button.y);
            const bool active = source->health > 0 && source->ion <= 0 &&
                                (!source->hackActive || source->hackLevel <= 1);
            const auto part = [&](const std::string& name, float y) {
                const Rect hit{button.x + 10, button.y + y, 20, 20};
                renderer_.draw(
                    "img/systemUI/button_door_" + name +
                        (active ? hit.contains(mouseX_, mouseY_) ? "_select2.png" : "_on.png"
                                : "_off.png"),
                    button.x,
                    button.y);
            };
            part("bottom", 33);
            part("top", 10);
        } else if (id == 12) {
            const SDL_FPoint button{position.x + 35, position.y - 49};
            renderer_.draw("img/systemUI/button_battery3_base.png", button.x, button.y);
            if (battery_.active && battery_.duration > 0) {
                auto& bar = renderer_.image("img/systemUI/button_cloaking3_charging_on.png");
                const float fraction = std::max(0.f, 1.f - battery_.elapsed / battery_.duration);
                const float top =
                    std::clamp(float(66 - 4 * int(std::ceil(fraction * 11))), 0.f, float(bar.h));
                renderer_.sprite(bar,
                                 {position.x + 36,
                                  position.y + 42 - float(bar.h) + top,
                                  float(bar.w),
                                  float(bar.h) - top},
                                 {0, top, float(bar.w), float(bar.h) - top});
            } else {
                const bool ready = source->health > 0 && source->ion <= 0 &&
                                   battery_.cooldown <= 0 &&
                                   (!source->hackActive || source->hackLevel <= 1);
                renderer_.draw(
                    "img/systemUI/button_cloaking3" +
                        std::string(ready ? abilityButtonRect(id).contains(mouseX_, mouseY_)
                                                ? "_select2.png"
                                                : "_on.png"
                                          : "_off.png"),
                    button.x,
                    button.y);
            }
        }
    }
}

int Run::resourceBoxWidth(const EventResources& resources) {
    int width = 0;
    for (const int amount : std::array<int, 4>{
             resources.fuel, resources.missiles, resources.drones, resources.scrap}) {
        if (amount) {
            width += (std::abs(amount) <= 9 ? 45 : 50) + (amount < 0 ? 5 : 0);
        }
    }
    return width;
}

void Run::resourceBox(
    const EventResources& resources, float x, float y, bool choice, SDL_Color color, bool frame) {
    const std::array<std::pair<const char*, int>, 4> entries{{{"fuel", resources.fuel},
                                                              {"missiles", resources.missiles},
                                                              {"drones", resources.drones},
                                                              {"scrap", resources.scrap}}};
    const int width = resourceBoxWidth(resources);
    if (width == 0) {
        return;
    }
    if (!choice) {
        x -= width / 2;
    }
    const int padding = 5;
    y -= 16;
    if (frame) {
        renderer_.fill({x, y, static_cast<float>(width + padding), 32}, {0, 0, 0, 128});
        for (const Rect edge :
             std::array<Rect, 4>{{{x, y, static_cast<float>(width + padding), 2},
                                  {x, y + 30, static_cast<float>(width + padding), 2},
                                  {x, y, 2, 32},
                                  {x + width + padding - 2, y, 2, 32}}}) {
            renderer_.fill(edge, color);
        }
    }
    float offset = 5;
    for (const auto& [name, amount] : entries) {
        if (!amount) {
            continue;
        }
        auto& icon = renderer_.image(std::string("img/ui_icons/icon_") + name + ".png");
        renderer_.sprite(icon,
                         {x + offset, y, 32, 32},
                         {0, 0, static_cast<float>(icon.w), static_cast<float>(icon.h)},
                         0,
                         amount > 0 ? SDL_Color{100, 255, 100, 255} : SDL_Color{255, 50, 50, 255});
        text(std::to_string(amount), x + offset + 30, y + 9, 10, false, color);
        offset += (std::abs(amount) <= 9 ? 45 : 50) + (amount < 0 ? 5 : 0);
    }
}

void Run::drawRewardEntries(const nlohmann::json& entries, float left, float top, SDL_Color color) {
    // ChoiceBox::RenderResources 0x4e92e0: each entry's row offset comes from
    // rewardLayout; labels take the box's text colour.
    for (const auto& reward : entries) {
        const float row = top + reward.value("y", 0.f);
        // Icons are translated to (x + icon_x, row + 16); labels print at row + 9.
        const float iconX = left + reward.at("icon_x").get<float>();
        const float iconY = row + 16;
        if (reward.at("kind") == "equipment") {
            const auto& bp = catalog_.equipment(reward.at("id").get<std::string>());
            if (bp.kind == "weapon" && !bp.imagePath.empty()) {
                SDL_FPoint pivot{static_cast<float>(bp.frameWidth) / 2,
                                 static_cast<float>(bp.frameHeight) / 2};
                renderer_.sprite(renderer_.image(bp.imagePath),
                                 {iconX - pivot.x,
                                  iconY - pivot.y,
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
            } else if (bp.kind == "drone") {
                renderDroneIcon(bp, iconX, iconY);
            }
        } else {
            // CrewBlueprint::RenderIcon 0x5e0f00 centres the 35 px frame on
            // the translation: (int)(-35 * 0.5) = -17.
            HangarCrew crew;
            crew.species = reward.at("species").get<std::string>();
            crew.appearance = reward.value("appearance", 0);
            painter_.crewSprite(
                crew, iconX - 17, iconY - 17, 1, false, "walk_down", 0, false, false, false, false);
            // CrewBlueprint::RenderSkillIcon 0x5df7a0: 26 x 26 at row + 2,
            // yellow (colour-blind blue) at a full skill, green from half,
            // COLOR_CB_WHITE below that.
            static constexpr std::array<const char*, 6> skillNames{
                "pilot", "engines", "shields", "weapons", "repair", "combat"};
            for (const auto& icon : reward.value("skill_icons", nlohmann::json::array())) {
                const float fraction = icon.at("fraction").get<float>();
                auto& art = renderer_.image(
                    "img/people/skill_" +
                    std::string(skillNames.at(icon.at("skill").get<std::size_t>())) + "_white.png");
                renderer_.sprite(art,
                                 {left + icon.at("x").get<float>(), row + 2, 26, 26},
                                 {0, 0, float(art.w), float(art.h)},
                                 0,
                                 fraction >= 1
                                     ? (options_.colorblind ? SDL_Color{0, 147, 224, 255}
                                                            : SDL_Color{255, 255, 50, 255})
                                 : fraction >= .5f ? SDL_Color{100, 255, 100, 255}
                                                   : SDL_Color{243, 255, 238, 255});
            }
        }
        const auto prefix = reward.value("prefix", std::string{});
        if (!prefix.empty()) {
            text(prefix, left + reward.at("prefix_x").get<float>(), row + 9, 12, false, color);
        }
        text(reward.at("label").get<std::string>(),
             left + reward.at("label_x").get<float>(),
             row + 9,
             10,
             false,
             color);
    }
}

void Run::eventDialog() {
    const float offset = enemy_.present ? -150.f : 0.f;
    const auto rewards = eventRewardEntries();
    // MeasureResources 0x4ebaf0: width is the final n5, or the bare resource
    // width when only resources are shown (RenderResources subtracts 25).
    const int rewardWidth =
        rewards.empty() ? resourceBoxWidth(pendingResources_) : rewards.back().at("end").get<int>();
    const int rewardHeight = static_cast<int>(eventRewardHeight());
    // ChoiceBox::OnRender 0x4ec490 builds WindowFrame(7, 22, 602, ...) and,
    // when the measured reward is wider than 602, WindowFrame(4 - ((w - 577)
    // >> 1), 22, w + 25, ...); the outer rect adds the 7 px frame margin.
    const int frameWidth = rewardWidth > 602 ? rewardWidth + 25 : 602;
    const int frameX = rewardWidth > 602 ? 4 - ((rewardWidth - 577) >> 1) : 7;
    renderer_.windowFrame({320.f + static_cast<float>(frameX) + offset,
                           140,
                           static_cast<float>(frameWidth + 14),
                           391});
    float y = 170;
    for (const auto& line : eventLines()) {
        text(line, 352 + offset, y, eventFontSize_);
        y += textLineHeight(eventFontSize_, eventText_);
    }
    if (rewardWidth > 0 && rewardHeight > 0) {
        // OnRender 0x4ec490 passes (start.x + 282 - 5, text end + n30 + 5) to
        // PrintResourceBox 0x4ebde0, which centres the box on that point: rect
        // (x - w / 2, y - h / 2, w + 5, h), half-alpha black with a two-pixel
        // white outline. The row starts (h - 32) / 2 below the top when h > 32.
        const int centerY = static_cast<int>(y + 30.f + 5.f);
        const float left = static_cast<float>(629 - rewardWidth / 2) + offset;
        const float top = static_cast<float>(centerY - rewardHeight / 2);
        const float width = static_cast<float>(rewardWidth + 5);
        const float height = static_cast<float>(rewardHeight);
        const float row = top + static_cast<float>(rewardHeight > 32 ? (rewardHeight - 32) / 2 : 0);
        renderer_.fill({left, top, width, height}, {0, 0, 0, 128});
        for (const Rect edge : std::array<Rect, 4>{{{left, top, width, 2},
                                                    {left, top + height - 2, width, 2},
                                                    {left, top, 2, height},
                                                    {left + width - 2, top, 2, height}}}) {
            renderer_.fill(edge, {255, 255, 255, 255});
        }
        if (pendingResources_.visible()) {
            resourceBox(pendingResources_, left, row + 16, true, {255, 255, 255, 255}, false);
        }
        drawRewardEntries(rewards, left, row, {255, 255, 255, 255});
    }
    for (std::size_t index = 0; index < choices_.size(); ++index) {
        const float top = choiceRow(index);
        const auto& choice = choices_[index];
        const auto color =
            !choice.enabled ? SDL_Color{150, 150, 150, 255}
            : Rect{352 + offset, top - 10, 565, choiceHeight(index)}.contains(mouseX_, mouseY_)
                ? SDL_Color{243, 255, 80, 255}
            : choice.blueChoice ? SDL_Color{0, 195, 255, 255}
                                : SDL_Color{255, 255, 255, 255};
        const auto layout = choiceRewardLayout(index);
        // OnRender 0x4ec490 pushes the text down by (int)((box h - y) * .5) when
        // the box is taller than the measured text height y, else by 10; the
        // native row top already carries the plain push of 10. The original's
        // plain row (10 + y + 12) is 32 for one line, so y = native height - 22.
        const auto lines = choiceLines(index);
        const float measured = std::max(32.f,
                                        textLineHeight(eventFontSize_, choice.text) *
                                                static_cast<float>(lines.size()) +
                                            12.f) -
                               22.f;
        const float push = static_cast<float>(layout.height) > measured
                               ? static_cast<float>(static_cast<int>(
                                     (static_cast<float>(layout.height) - measured) * .5f))
                               : 10.f;
        const float textTop = top + push - 10.f;
        float lineTop = textTop;
        for (const auto& line : lines) {
            text(line, 352 + offset, lineTop, eventFontSize_, false, color);
            lineTop += textLineHeight(eventFontSize_, choice.text);
        }
        if (layout.width > 0) {
            // ChoiceBox4ec490 passes the text's midpoint minus 3;
            // PrintResourceBox 0x4ebde0 draws (x, y - h / 2, w + 5, h) at half
            // alpha with a two-pixel outline in the text colour, and
            // RenderResources centres each row by its own width.
            const float boxX = 352 + offset + choiceTextWidth(index);
            const float centerY = textTop + (lineTop - textTop) / 2 - 3;
            const float boxY = static_cast<float>(static_cast<int>(centerY) - layout.height / 2);
            const float boxW = static_cast<float>(layout.width + 5);
            const float boxH = static_cast<float>(layout.height);
            renderer_.fill({boxX, boxY, boxW, boxH}, {0, 0, 0, 128});
            for (const Rect edge : std::array<Rect, 4>{{{boxX, boxY, boxW, 2},
                                                        {boxX, boxY + boxH - 2, boxW, 2},
                                                        {boxX, boxY, 2, boxH},
                                                        {boxX + boxW - 2, boxY, 2, boxH}}}) {
                renderer_.fill(edge, color);
            }
            const auto resources = choiceResources(index);
            if (resources.visible()) {
                const float rowX =
                    boxX + static_cast<float>(
                               layout.row1Width ? (layout.width - layout.row1Width) / 2 : 0);
                resourceBox(resources, rowX, boxY + 16, true, color, false);
            }
            const float itemsX =
                boxX +
                static_cast<float>(layout.row2Width ? (layout.width - layout.row2Width) / 2 : 0);
            drawRewardEntries(layout.entries, itemsX, boxY, color);
        }
    }
}

float Run::travelPresentationSeconds() const {
    const float current =
        jumpTicks_ > 0 ? 2.f - jumpTicks_ * Time::step : arrivalTicks_ * Time::step;
    return std::lerp(previousJump_, current, presentationAlpha_);
}

void Run::renderAchievementPopup() {
    if (!achievementPopupRunning_ || achievementPopupQueue_.empty()) {
        return;
    }
    const auto& popup = achievementPopupQueue_.front();
    std::string image;
    std::string title;
    std::string header = texts_.get("ach_unlocked", locale_);
    int dimension = 64;
    const auto localized = [&](const LocalizedText& value) {
        return value.reference ? texts_.get(value.value, locale_) : value.value;
    };
    const auto* ship = popup.family >= 0 ? catalog_.ship(popup.family, 0) : nullptr;
    if (popup.id.empty() && ship) {
        image = "img/achievements/" + ship->layout + "_1.png";
        header = texts_.get(popup.layout == 0 ? "ship_unlocked" : "layout_unlocked", locale_);
        title = localized(ship->shipClass);
        if (popup.layout > 0) {
            title += "\n" + texts_.get(popup.layout == 1 ? "type_b" : "type_c", locale_);
        }
    } else if (popup.id.ends_with("_VICTORY") || popup.id.ends_with("_QUEST")) {
        const bool victory = popup.id.ends_with("_VICTORY");
        image = victory ? "img/achievements/S_V_on.png" : "img/achievements/S_Q_on.png";
        dimension = 32;
        title = texts_.get(victory ? "victory_ach_title" : "quest_ach_title", locale_);
        if (ship) {
            if (const auto at = title.find("\\1"); at != std::string::npos) {
                title.replace(at, 2, localized(ship->shipClass));
            }
        }
    } else {
        auto document = xmlResource(archive_, "data/achievements.xml");
        const auto row =
            document.child("FTL").find_child_by_attribute("achievement", "id", popup.id.c_str());
        if (!row) {
            return;
        }
        image = "img/" + std::string(row.child("img").child_value());
        title = texts_.get(row.child("name").attribute("id").as_string(), locale_);
    }
    // AchievementTracker42e7c0 and CAchievement429fe0, pure presentation.
    renderer_.draw("img/achievements/popup_box.png", 970, 0);
    AchievementDrawing drawing;
    drawing.image = image;
    drawing.dimension = dimension;
    drawing.unlocked = true;
    drawing.victory = popup.id.ends_with("_VICTORY");
    drawing.twoLayouts = popup.family >= 8;
    if (const auto* award = progression_.achievement(popup.id)) {
        drawing.layouts = award->layouts;
    }
    renderer_.achievement(drawing, {1024.f - dimension / 2.f, 51.f - dimension / 2.f});
    text(header, 1075, 20, 11, false, {255, 255, 255, 255});
    const auto lines = wrappedText(13, title, 195);
    const float lineHeight = textLineHeight(13, title);
    float y = static_cast<float>(lines.size()) * lineHeight <= 25.f ? 50.f : 41.f;
    for (const auto& line : lines) {
        text(line, 1075, y, 13, false, {255, 255, 255, 255});
        y += lineHeight;
    }
}

void Run::renderBoundary() {
    // CommandGui520a70's static full_focus describes the previous rendered
    // frame. Prepare that UI latch at the application boundary, keeping draws
    // independent of gameplay mutation and RNG.
    fullFocusForFrame_ = fullFocusLatch_;
    fullFocusLatch_ = false;
    if (jumpTicks_ == 0 && arrivalTicks_ == 0) {
        if (externalModal_ || defeated_ || panel_ == "pause" ||
            panel_.starts_with("pause_confirm:") || mapOpen_ || (!event_ && !panel_.empty())) {
            fullFocusLatch_ = true;
        }
        // Primary ChoiceBox and InputBox retain FocusWindow's false default.
    }
    if (panel_.starts_with("pause_confirm:") || writeErrorOpen_) {
        fullFocusLatch_ = true;
    }
}

void Run::render() {
    renderer_.clear();
    // CommandGui::RenderStatic520a70 tints the scene and controls before
    // drawing a focused window, then draws that window at full brightness.
    renderer_.setTint(fullFocusForFrame_ ? .15f : 1.f);
    const bool jumping = jumpTicks_ > 0 || arrivalTicks_ > 0;
    const float jumpTime = travelPresentationSeconds();
    const float jumpProgress = jumping ? std::clamp((jumpTime - 1) * 2, 0.f, 1.f) : -1.f;
    const float zoomProgress = jumping ? std::min(1.f, jumpTime * 2) : 0;
    int shipLeft = 0;
    int shipTop = 0;
    if (!ship_.rooms.empty()) {
        shipLeft = ship_.rooms.front().x;
        shipTop = ship_.rooms.front().y;
        for (const auto& room : ship_.rooms) {
            shipLeft = std::min(shipLeft, room.x);
            shipTop = std::min(shipTop, room.y);
        }
    }
    const SDL_FPoint shipCenter{playerX() + ship_.horizontalShift + shipLeft + ship_.imageRect[0] +
                                    ship_.imageRect[2] / 2.f,
                                170.f + ship_.verticalShift + shipTop + ship_.imageRect[1] +
                                    ship_.imageRect[3] / 2.f};
    // SpaceManager::OnRenderBackground4179a0 gives static hazard imagery
    // priority in lowend mode and omits the animated cloud/planet branch.
    const std::string lowHazard = environment_.stormActive      ? "storm"
                                  : nebula_                     ? "nebula"
                                  : environment_.asteroidActive ? "asteroid"
                                  : environment_.fireActive     ? "sun"
                                  : environment_.pulsarActive   ? "pulsar"
                                                                : "";
    if (options_.lowend && !lowHazard.empty()) {
        auto& backdrop = renderer_.image("img/stars/low_" + lowHazard + ".png");
        const float damage = 1.f - static_cast<float>(hull_) / ship_.hull;
        const auto red = static_cast<Uint8>(255.f * (userPause_ ? damage * .4f + .4f : 1.f));
        const auto other =
            static_cast<Uint8>(255.f * (userPause_ ? .4f : 1.f - damage * damage * .5f));
        renderer_.sprite(backdrop,
                         {0, 0, float(backdrop.w), float(backdrop.h)},
                         {0, 0, float(backdrop.w), float(backdrop.h)},
                         0,
                         {red, other, other, 255});
    } else if (nebula_) {
        nebulaScene_.render(renderer_,
                            userPause_,
                            static_cast<float>(hull_) / ship_.hull,
                            (userPause_ || event_ || mapOpen_) ? uiAlpha_ : presentationAlpha_);
    } else if (!eventBackground_.empty()) {
        renderer_.draw(eventBackground_, 0, 0, userPause_ ? .4f : 1.f);
        if (!eventPlanet_.empty()) {
            auto& planet = renderer_.image(eventPlanet_);
            const auto tint = static_cast<Uint8>(userPause_ ? 102 : 255);
            renderer_.sprite(planet,
                             {static_cast<float>(eventPlanetX_),
                              static_cast<float>(eventPlanetY_),
                              static_cast<float>(planet.w),
                              static_cast<float>(planet.h)},
                             {0, 0, static_cast<float>(planet.w), static_cast<float>(planet.h)},
                             eventPlanetRotation_,
                             {tint, tint, tint, 255});
        }
    } else if (encounter_ == "REBEL") {
        renderer_.draw("img/stars/bg_lonelyRedStar.png", 0, 0, userPause_ ? .4f : 1.f);
        renderer_.draw(
            "img/stars/planet_gas_yellow.png", playerX() + 125, 296, userPause_ ? .4f : 1.f);
    } else {
        renderer_.draw("img/stars/bg_darknebula.png", 0, 0, userPause_ ? .4f : 1.f);
        renderer_.draw("img/stars/planet_bigblue.png", 776, 53, userPause_ ? .4f : 1.f);
    }
    renderer_.setTransform(1 - .05f * zoomProgress, shipCenter);
    renderEnvironment();
    // CommandGui::OnRender scales asteroid scenery independently of the ship.
    renderer_.setTransform(1 - .3f * zoomProgress, shipCenter);
    renderAsteroidScroll();
    renderer_.setTransform(1 - .75f * zoomProgress, shipCenter);
    renderSpaceProjectiles(0, 2);
    renderSpaceDrones(0, 2);
    if (auto* s = system(0);
        !playerExplosionStarted_ && s && (shieldBubbles_ > 0 || superShields_ > 0)) {
        auto path = "img/ship/" + (ship_.shieldImage.empty() ? ship_.image : ship_.shieldImage) +
                    "_shields1.png";
        if (archive_.contains(path)) {
            auto& im = renderer_.image(path);
            int minx = 10000;
            int miny = 10000;
            int maxx = 0;
            int maxy = 0;
            for (auto& room : ship_.rooms) {
                minx = std::min(minx, room.x);
                miny = std::min(miny, room.y);
                maxx = std::max(maxx, room.x + room.w);
                maxy = std::max(maxy, room.y + room.h);
            }
            const float x = playerX() + (minx + maxx) / 2 + ship_.ellipse[2] - im.w / 2;
            const float y = 170.f + (miny + maxy) / 2 + ship_.ellipse[3] - im.h / 2;
            if (shieldBubbles_ > 0) {
                renderer_.draw(path,
                               x,
                               y,
                               1,
                               (.4f + .6f * shieldBubbles_ / 3.5f) *
                                   (1 - std::max(0.f, jumpProgress)));
            }
            if (superShields_ > 0) {
                renderer_.sprite(im,
                                 {x, y, float(im.w), float(im.h)},
                                 {0, 0, float(im.w), float(im.h)},
                                 0,
                                 {102,
                                  255,
                                  105,
                                  Uint8((.55f + .35f * superShields_ / 5.f) * 255.f *
                                        (1 - std::max(0.f, jumpProgress)))});
            }
        }
    }

    if (!playerExplosionStarted_ && !jumping) {
        renderShieldHits(0);
    }
    ShipPaintState v;
    v.colorblind = options_.colorblind;
    v.weaponSlotIndices = weaponSlots_;
    v.cloaked = cloak_.active;
    v.x = playerX();
    v.tick = tick_;
    v.enginesOn = system(1) && system(1)->power > 0;
    v.engineAnimationTimes = playerEngineAnimationTimes_;
    if (const auto* medbay = system(5); medbay && medbay->hackActive && medbay->hackLevel > 1) {
        v.damagingEffectTimes[medbay->definition.room] = medbay->damagingEffectTimes;
    }
    v.doorFrames = doorFrames_;
    v.doorHit = doorsHit(false, doorFrames_.size());
    v.doorLevel = subsystemLevel(8);
    v.doorsHacked = system(8) && system(8)->hackActive;
    doorRenderLevels(false, v);
    if (!(userPause_ || event_ || mapOpen_) && previousDoors_.size() == v.doorFrames.size()) {
        for (std::size_t d = 0; d < v.doorFrames.size(); ++d) {
            v.doorFrames[d] = std::lerp(previousDoors_[d], v.doorFrames[d], presentationAlpha_);
        }
    }
    v.roomOxygen = roomOxygen_;
    v.active = true;
    if (tutorialOwned_ && tutorial_.current().name == "DAMAGE_SYSTEM_2" && !event_ &&
        jumpTicks_ == 0 && arrivalTicks_ == 0) {
        const float phase =
            std::fmod(static_cast<float>(animation_ % 90) + uiFraction_, 90.f) / 90.f;
        v.crewHighlightAlpha = static_cast<Uint8>((1.f - std::abs(2.f * phase - 1.f)) * 255.f);
    }
    // ShipManager::OnLoop 0x4a05b0: sensors present, not strongly hacked and
    // with effective power above zero lift the player's room blackout.
    v.sensorsWorking = !nebula_ && system(7) &&
                       (!system(7)->hackActive || system(7)->hackLevel <= 1) &&
                       subsystemLevel(7) > 0;
    v.explicitVision = true;
    for (const auto& member : crew_) {
        if (!member.outOfGame && !member.intruder) {
            v.visionRooms.push_back(member.room);
            v.containsPlayerCrew = true;
        }
    }
    for (const auto& member : voyageBoarders_) {
        if (!member.outOfGame && member.controlled) {
            v.visionRooms.push_back(member.room);
            v.containsPlayerCrew = true;
        }
    }
    for (const auto& member : enemy_.crew) {
        if (!member.outOfGame && member.intruder && member.controlled) {
            v.visionRooms.push_back(member.boardRoom);
            v.containsPlayerCrew = true;
        }
    }
    for (const auto& drone : combatDrones_) {
        const auto& type = catalog_.equipment(drone.blueprint).type;
        if (drone.deployed && drone.delivered && (type == "REPAIR" || type == "BATTLE")) {
            // ProvidesVision73de80 uses deployed, independently of HP/death.
            v.visionRooms.push_back(drone.room);
            v.containsPlayerCrew = true;
        }
    }
    v.rooms = !jumping;
    v.jumpProgress = jumpProgress;
    if (playerExplosionStarted_) {
        v.destructionAge = float(Time::seconds(double(tick_ - playerExplosionStartTick_)));
        v.explosion = &playerExplosionState_;
    }
    if (!event_ && !mapOpen_ && jumpTicks_ == 0) {
        for (int id : {0, 1, 5, 13, 2, 9, 10, 11, 14, 15, 3, 4}) {
            if (const auto* source = system(id)) {
                const auto position = systemHudPosition(id);
                if (Rect{position.x + 24,
                         position.y + 8 - 8.f * source->definition.power,
                         16,
                         8.f * source->definition.power + 55}
                        .contains(mouseX_, mouseY_)) {
                    v.hoverSystem = id;
                }
            }
        }
        for (int id : {6, 7, 8, 12}) {
            const auto position = systemHudPosition(id);
            if (system(id) &&
                Rect{position.x + 18, position.y + 11, 28, 28}.contains(mouseX_, mouseY_)) {
                v.hoverSystem = id;
            }
        }
        float lx = mouseX_ - playerX() - ship_.horizontalShift;
        float ly = mouseY_ - 170 - ship_.verticalShift;
        for (const auto& d : ship_.doors) {
            float dx = d.x + (d.vertical ? 0.f : 17.f);
            float dy = d.y + (d.vertical ? 17.f : 0.f);
            if (Rect{dx - (d.vertical ? 6.f : 12.f),
                     dy - (d.vertical ? 12.f : 6.f),
                     d.vertical ? 12.f : 24.f,
                     d.vertical ? 24.f : 12.f}
                    .contains(lx, ly)) {
                v.hoverDoor = d.id;
            }
        }
    }
    if (!event_ && !mapOpen_ && std::any_of(crew_.begin(), crew_.end(), [](const auto& c) {
            return c.selected;
        })) {
        v.hoverRoom = roomAt(mouseX_ - playerX() - ship_.horizontalShift,
                             mouseY_ - 170 - ship_.verticalShift);
    }
    for (const auto& sys : systems_) {
        if (sys.definition.id == 0 || sys.definition.id == 1 || sys.definition.id == 3 ||
            sys.definition.id == 6) {
            v.computerLevels[sys.definition.id] = sys.operatorActive ? 1 : 0;
        }
    }
    std::vector<HangarCrew> appearances;
    for (auto& c : crew_) {
        auto appearance = c.appearance;
        appearance.room = c.room;
        appearance.slot = c.slot;
        appearances.push_back(appearance);
        std::string anim = "walk_" + (c.path.empty() ? std::string("down") : c.facing);
        if (c.health <= 0) {
            anim = c.outOfGame ? "gone" : (c.facing == "left" ? "death_left" : "death_right");
        }
        for (const auto& sys : systems_) {
            if (c.health <= 0) {
                break;
            }
            // CrewMember::OnLoop 0x474d62 passes repairing = currentRepair &&
            // CanRepair() to CrewAnimation::OnUpdate 0x473600: the member's own
            // repair task (system, fire or breach), not a damaged room system.
            if (c.path.empty() && c.health > 0 && c.repairKind != 0) {
                anim = "repair";
                break;
            }
            // SetCurrentSystem46e210 gates active manning with BlockedBoosted6155f0;
            // OnLoop4747f0 uses that active state for the typing animation.
            if (crewOperator(sys.definition.id) == &c) {
                auto direction = sys.definition.direction;
                std::transform(
                    direction.begin(), direction.end(), direction.begin(), [](unsigned char a) {
                        return char(std::tolower(a));
                    });
                if (!direction.empty() && direction != "none") {
                    anim = "type_" + direction;
                }
            }
        }
        // CrewAnimation::OnUpdate 0x473600 status 4: the extinguishing strip
        // towards the fire, over the emitter's smoke.
        const auto fire = crewFireAnimation(c, c.smoke);
        if (!fire.empty()) {
            anim = fire;
        }
        if (const auto combat = crewCombatAnimation(c); !combat.empty()) {
            anim = combat;
        }
        const bool rockFire = anim == "fire_down" && c.appearance.species == "rock";
        v.crewAnimations.push_back(anim);
        v.crewTicks.push_back(
            anim.starts_with("death_") ? std::uint64_t(c.deathAnimationElapsed * 60.f)
            : c.path.empty() && c.attackAnimation.empty() && !anim.starts_with("type_") &&
                    anim != "repair" && !anim.starts_with("fire_")
                ? 0
                : crewWalkTick(anim, false));
        if (!crewCombatAnimation(c).empty()) {
            v.crewTicks.back() = crewCombatTick(c);
        } else if (rockFire) {
            v.crewTicks.back() = std::uint64_t(std::min(59.f, c.fireWorkElapsed * 60.f));
        }
        v.crewUnderlays.push_back(nullptr);
        if (anim == fire && !fire.empty()) {
            v.crewUnderlays.back() = [this, smoke = &c.smoke](float cornerX, float cornerY) {
                renderCrewSmoke(*smoke, cornerX, cornerY);
            };
        }
        if (c.health > 0 && (c.teleportArrive > 0 || c.teleportLeaveKind != 0)) {
            v.crewAnimations.back() =
                c.teleportLeaveKind != 0 ? "teleport_leave" : "teleport_arrive";
            v.crewTicks.back() = crewTeleportArriveTick(c);
        }
        v.crewHealth.push_back(100.f * c.health /
                               (crewMaximumHealth(c.appearance.species) + c.healthBoost));
        if (c.intruder) {
            v.crewHealth.back() = 0;
            v.crewAnimations.back() = "gone";
        }
        v.crewHealthAlpha.push_back(c.intruder ? 0.f : crewHealthAlpha(c));
        v.crewHealthFlash.push_back(c.health <= 25 && c.healthFlashTime < .5f);
        v.hostileCrew.push_back(false);
        v.stunnedCrew.push_back(c.stunTime > 0);
        if (!c.path.empty()) {
            v.targets.push_back(slotPoint(c.targetRoom, c.targetSlot));
        }
        const auto index = v.crewPositions.size();
        const auto previous =
            index < previousCrew_.size() ? previousCrew_[index] : SDL_FPoint{c.x, c.y};
        const float crewAlpha =
            userPause_ || event_ || mapOpen_ || !panel_.empty() ? 1.f : presentationAlpha_;
        v.crewPositions.push_back(
            {std::lerp(previous.x, c.x, crewAlpha), std::lerp(previous.y, c.y, crewAlpha)});
        v.crewPositions.back().y += crewPresentationPoint(c).y - c.y;
        v.selected.push_back(c.selected);
    }
    v.doors = doors_;
    v.weaponFrames = combatWeaponFrames(ship_, combatWeapons_);
    v.weaponStates = &combatWeapons_;
    v.artilleryFrames = combatArtilleryFrames(systems_);
    for (const auto& source : systems_) {
        if (source.definition.id == 11) {
            v.artilleryStates.push_back(&source.artillery);
        }
    }
    // ShipSystem ctor 0x61bab0 starts flashTracker (0.75 s, looping) and
    // ShipSystem::OnRender updates it on frame time.
    v.systemFlash = std::fmod((static_cast<float>(animation_ % 45) + uiFraction_) / 60.f, .75f);
    for (const auto& s : systems_) {
        v.systemHealth[s.definition.id] = s.health;
        v.systemCapacity[s.definition.id] = s.definition.power;
        v.systemLocked[s.definition.id] = s.lockCount > 0 || s.lockCount == -1 || s.hackLevel > 1;
        v.systemHackEffect[s.definition.id] = s.hackLevel;
    }
    const float actorX = playerX() + ship_.horizontalShift;
    const float actorY = 170.f + ship_.verticalShift;
    const auto visibleRoom = [&](int room) {
        return v.sensorsWorking || std::ranges::find(v.visionRooms, room) != v.visionRooms.end();
    };
    // ShipManager::OnRender 0x49abc0: with slug crew or LIFE_SCANNER aboard, a
    // visitor in a blacked-out room is drawn outline-only (red silhouette).
    const bool lifeSight = playerLifeSight();
    const auto renderForeignBody = [&](const RunCrew& member) {
        const bool silhouette = !visibleRoom(member.room);
        if ((silhouette && !lifeSight) || member.outOfGame) {
            return;
        }
        const auto position = crewPresentationPoint(member);
        auto animation = member.path.empty() ? std::string("walk_down") : "walk_" + member.facing;
        const auto fire = crewFireAnimation(member, member.smoke);
        if (member.health <= 0) {
            animation = member.facing == "left" ? "death_left" : "death_right";
        } else if (const auto combat = crewCombatAnimation(member); !combat.empty()) {
            animation = combat;
        } else if (!fire.empty()) {
            animation = fire;
        } else if (member.controlled && member.repairKind != 0 && member.path.empty() &&
                   member.stunTime <= 0) {
            // A mind-controlled visitor is no intruder, so CanRepair holds and
            // CrewAnimation::OnUpdate 0x473600 plays status 2.
            animation = "repair";
        } else if (member.controlled) {
            for (const auto& sys : systems_) {
                if (crewOperator(sys.definition.id) == &member) {
                    auto direction = sys.definition.direction;
                    std::transform(direction.begin(),
                                   direction.end(),
                                   direction.begin(),
                                   [](unsigned char value) {
                                       return char(std::tolower(value));
                                   });
                    if (!direction.empty() && direction != "none") {
                        animation = "type_" + direction;
                    }
                    break;
                }
            }
        }
        auto animationTick = member.health <= 0 ? std::uint64_t(member.deathAnimationElapsed * 60.f)
                             : !crewCombatAnimation(member).empty() ? crewCombatTick(member)
                             : animation == "fire_down" && member.appearance.species == "rock"
                                 ? std::uint64_t(std::min(59.f, member.fireWorkElapsed * 60.f))
                             : member.path.empty() && !animation.starts_with("type_") &&
                                     animation != "repair" && !animation.starts_with("fire_")
                                 ? 0
                                 : crewWalkTick(animation, true);
        if (member.health > 0 && (member.teleportArrive > 0 || member.teleportLeaveKind != 0)) {
            animation = member.teleportLeaveKind != 0 ? "teleport_leave" : "teleport_arrive";
            animationTick = crewTeleportArriveTick(member);
        }
        if (!fire.empty() && animation == fire && !silhouette) {
            renderCrewSmoke(member.smoke, actorX + position.x - 17, actorY + position.y - 17);
        }
        painter_.crewSprite(member.appearance,
                            actorX + position.x - 17,
                            actorY + position.y - 17,
                            1,
                            false,
                            animation,
                            animationTick,
                            true,
                            options_.colorblind,
                            member.stunTime > 0,
                            true,
                            true,
                            silhouette);
    };
    v.orderedPhysicalCrew = true;
    for (std::size_t index = 0; index < crew_.size(); ++index) {
        const auto* member = &crew_[index];
        v.crewPhysicalOrders.push_back(member->physicalCrewOrder);
        v.crewHealthRenderers.push_back([&, member, index] {
            if (member->intruder || member->outOfGame || !visibleRoom(member->room)) {
                return;
            }
            const auto point = crewPresentationPoint(*member);
            const auto displayed = v.crewPositions[index];
            renderCrewHealth(
                *member, actorX + displayed.x - point.x, actorY + displayed.y - point.y);
        });
    }
    const auto visit = [&](const RunCrew& member) {
        const auto* actor = &member;
        v.visitingCrew.push_back({member.physicalCrewOrder,
                                  [&, actor] {
                                      renderForeignBody(*actor);
                                  },
                                  [&, actor] {
                                      if (!actor->outOfGame && visibleRoom(actor->room)) {
                                          renderCrewHealth(*actor, actorX, actorY);
                                      }
                                  }});
    };
    for (const auto& member : voyageBoarders_) {
        visit(member);
    }
    for (const auto& member : enemy_.crew) {
        if (member.intruder) {
            visit(member);
        }
    }
    for (int owner : {0, 1}) {
        for (const auto* drone : physicalCrewDrones(owner)) {
            if (drone->crewPhysicalShip != 0) {
                continue;
            }
            v.visitingCrew.push_back({drone->physicalCrewOrder,
                                      [&, drone] {
                                          renderCrewDrones(0, actorX, actorY, false, drone);
                                      },
                                      [&, drone] {
                                          renderCrewDrones(0, actorX, actorY, true, drone);
                                      }});
        }
    }
    painter_.draw(
        ship_,
        appearances,
        v,
        [&] {
            // Ship::OnRenderBreaches 0x482910 and Spreader<Fire>::OnRender
            // 0x735e90 skip blacked-out rooms before Ship::OnRenderBlackout.
            auto visibleHazards = playerHazards_;
            std::erase_if(visibleHazards, [&](const auto& entry) {
                return !visibleRoom(entry.first);
            });
            renderHazards(ship_, visibleHazards, v.x, v.y);
        },
        [&] {
            renderLockdown(ship_, lockdownRooms_, v.x, v.y);
        },
        [&] {
            for (const auto& member : voyageBoarders_) {
                renderForeignBody(member);
            }
            for (const auto& member : enemy_.crew) {
                if (member.intruder) {
                    renderForeignBody(member);
                }
            }
            renderCrewDrones(0, actorX, actorY);
        },
        [&] {
            for (std::size_t index = 0; index < crew_.size(); ++index) {
                const auto& member = crew_[index];
                if (!member.intruder && visibleRoom(member.room)) {
                    // Keep health overlays aligned with the same interpolated body.
                    const auto point = crewPresentationPoint(member);
                    const auto displayed = v.crewPositions[index];
                    renderCrewHealth(
                        member, actorX + displayed.x - point.x, actorY + displayed.y - point.y);
                }
            }
            for (const auto& member : voyageBoarders_) {
                if (visibleRoom(member.room)) {
                    renderCrewHealth(member, actorX, actorY);
                }
            }
            for (const auto& member : enemy_.crew) {
                if (member.intruder && visibleRoom(member.room)) {
                    renderCrewHealth(member, actorX, actorY);
                }
            }
            renderCrewDrones(0, actorX, actorY, true);
        });
    painter_.drawDestructionExplosions(ship_, v);
    if (!jumping) {
        renderShipMessages(0);
    }
    renderSpaceProjectiles(0, 1);
    renderSpaceDrones(0, 1);
    renderWeaponAiming(0);
    renderer_.setTransform();
    if (zoomProgress > 0) {
        renderer_.setTransform(1 - .3f * zoomProgress, shipCenter);
        renderAsteroidScroll(1);
        renderer_.setTransform();
    }
    resourceHud();
    if (!jumping) {
        combatRender();
    }
    hud(false);
    if ((userPause_ || event_) && jumpTicks_ == 0 && arrivalTicks_ == 0) {
        const float pulse = std::lerp(previousPauseBlink_, pauseBlink_, uiAlpha_) / .75f;
        for (const auto& [image, alpha] : std::array<std::pair<const char*, float>, 2>{
                 {{"img/Text_pause1.png", 1.f}, {"img/Text_pause2.png", pulse}}}) {
            auto& art = renderer_.image(image);
            renderer_.draw(image, 640.f - float(art.w) / 2.f, 558.f - float(art.h) / 2.f, 1, alpha);
        }
        auto message = texts_.get("paused_text", locale_);
        std::size_t token = 0;
        while ((token = message.find("\\1", token)) != std::string::npos) {
            message.replace(token, 2, pauseKeyName_);
            token += pauseKeyName_.size();
        }
        float lineY = 580;
        for (const auto& line : wrappedText(5, message, 1280)) {
            text(line, 640, lineY, 5, true, {255, 255, 255, Uint8(255 * (.6f + .3f * pulse))});
            lineY += textLineHeight(5, message);
        }
    }
    // CommandGui::RenderStatic0x520a70 renders SpaceStatus after scenery,
    // ships, controls and pause, before map/dialog/management overlays.
    renderEnvironmentWarnings();
    hud(true);
    // ShipStatus precedes foreground flashes and modal windows. GameOver,
    // MenuScreen, OptionsScreen and ConfirmWindow retain the scene tint here.
    const bool statusDim =
        externalModal_ || defeated_ || panel_ == "pause" || panel_.starts_with("pause_confirm:");
    if (!statusDim) {
        renderer_.setTint(1);
    }
    shipStatusHud();
    renderEnvironmentFlash();
    renderer_.setTint(1);
    if (!externalModal_) {
        if (defeated_) {
            gameOverRender();
        } else if (mapOpen_) {
            travelMap();
        } else if (event_ && arrivalTicks_ == 0) {
            eventDialog();
        } else if (!message_.empty() && animation_ - messageTick_ < 180) {
            text(message_, 10, 154.f + crew_.size() * 30 + 48, 10);
        }
        if (!defeated_ && !mapOpen_ && (!event_ || arrivalTicks_ != 0)) {
            voyageRender();
        }
    }
    if (devUiOpen_) {
        devUiRender();
    }
    if (!externalModal_ && !event_ && !defeated_ && !jumping && panel_ != "pause" &&
        !panel_.starts_with("pause_confirm:")) {
        tutorialRender();
    }
}

void Run::renderApplicationPopup() {
    renderer_.setTint(1);
    renderAchievementPopup();
}

void Run::renderOuter() {
    // CApp::OnRender403070 draws achievements and MouseControl after the
    // active CommandGui/modal window, including externally hosted options.
    renderer_.setTint(1);
    if (writeErrorOpen_) {
        if (mouseX_ >= 0 && mouseY_ >= 0) {
            renderer_.draw(pointerValid_ ? "img/mouse/pointerValid.png"
                                         : "img/mouse/pointerInvalid.png",
                           mouseX_,
                           mouseY_);
        }
        return;
    }
    if (!externalModal_ && !event_ && !mapOpen_ && !defeated_ && panel_ == "pause") {
        renderPauseTooltip();
    }
    if (!externalModal_ && jumpTicks_ == 0 && arrivalTicks_ == 0 && !event_ && !mapOpen_ &&
        !defeated_ && (panel_.empty() || panel_ == "sector")) {
        tooltip();
    }
    // MouseControl::OnRender 0x4d6e00 draws invalidPointer unless a GUI
    // element marked the pointer valid this loop (see updatePointer).
    const char* const pointer =
        pointerValid_ ? "img/mouse/pointerValid.png" : "img/mouse/pointerInvalid.png";
    if (externalModal_ || mapOpen_ || defeated_ ||
        (jumpTicks_ == 0 && arrivalTicks_ == 0 &&
         (event_ || (!panel_.empty() && panel_ != "sector")))) {
        if (mouseX_ >= 0 && mouseY_ >= 0) {
            renderer_.draw(pointer, mouseX_, mouseY_);
        }
        return;
    }
    if (mouseX_ >= 0 && !abilityAim_.empty()) {
        // OnRender: teleport out1/in1 when valid, out2/in2 otherwise; mind
        // uses mouse_mind.png when valid and mouse_mind_valid.png when not.
        const auto image = abilityAim_ == "teleport_send"
                               ? (pointerValid_ ? "img/mouse/mouse_teleport_out1.png"
                                                : "img/mouse/mouse_teleport_out2.png")
                           : abilityAim_ == "teleport_return"
                               ? (pointerValid_ ? "img/mouse/mouse_teleport_in1.png"
                                                : "img/mouse/mouse_teleport_in2.png")
                           : abilityAim_ == "hack" ? "img/mouse/mouse_hacking.png"
                           : pointerValid_         ? "img/mouse/mouse_mind.png"
                                                   : "img/mouse/mouse_mind_valid.png";
        renderer_.draw(image, mouseX_, mouseY_);
        return;
    }
    if (mouseX_ >= 0) {
        const int doorMode = doorCursorMode();
        if (doorMode > 0) {
            // MouseControl::OnRender0x4d6e00 replaces the pointer with the full
            // original32px, five-frame strip; opening runs forward, closing reverse.
            const float cursorTime = (doorMode == 2) != doorCursorReverse_
                                         ? (doorMode == 2 ? .65f : 0.f)
                                         : doorCursorTime_;
            const int frame = std::clamp(static_cast<int>(cursorTime / .65f * 5), 0, 4);
            renderer_.sprite(renderer_.image("img/mouse/mouse_door_strip.png"),
                             {mouseX_, mouseY_, 32, 32},
                             {static_cast<float>(frame * 32), 0, 32, 32});
        } else {
            renderer_.draw(aiming_ >= 0
                               ? "img/mouse/mouse_crosshairs" +
                                     std::string(autofire_ != forceAutofire_ ? "3_" : "2_") +
                                     std::to_string(logicalArmamentSlot("weapon", aiming_) + 1) +
                                     ".png"
                               : pointer,
                           mouseX_,
                           mouseY_);
            if (aiming_ >= 0) {
                // MouseControl::OnRender0x4d6e00 draws the numbered reticle,
                // then its pointer layer at the same32px origin (4d7080/4d70fd).
                // SetSilentValid chooses the base layer on a valid target;
                // the stock files named valid/valid2 represent invalid targets.
                renderer_.draw(weaponCursorValid() ? "img/mouse/mouse_crosshairs.png"
                               : autofire_ != forceAutofire_
                                   ? "img/mouse/mouse_crosshairs_valid2.png"
                                   : "img/mouse/mouse_crosshairs_valid.png",
                               mouseX_,
                               mouseY_);
            }
        }
    }
}

void Run::renderDroneIcon(const EquipmentDefinition& bp, float centerX, float centerY) {
    // DroneBlueprint::RenderIcon 0x5e0820, drawn about the caller's centre.
    // Type ids from DroneSystem::StringToDrone 0x612620: DEFENSE 0, COMBAT 1,
    // REPAIR 2, BATTLE 3, BOARDER 4.
    const auto half = [](int size) {
        return float(((size > 0) - size) >> 1);
    };
    if (bp.type == "REPAIR" || bp.type == "BATTLE" || bp.type == "BOARDER") {
        // Crew-type drones: walk_down frame 0 of the base sprite, then layer1
        // tinted COLOR_GREEN (100,255,100). BOARDER uses the battle body.
        const std::string name = bp.type == "REPAIR"      ? "repair"
                                 : bp.id == "BOARDER_ION" ? "boarder_ion"
                                                          : "battle";
        const auto& strip = catalog_.animation(name + "_walk_down");
        if (!strip.usable()) {
            return;
        }
        const Rect source{float(strip.x), float(strip.y), float(strip.width), float(strip.height)};
        const Rect target{centerX + half(strip.width),
                          centerY + half(strip.height),
                          float(strip.width),
                          float(strip.height)};
        const auto base = "img/people/" + name + "_base.png";
        const auto layer = "img/people/" + name + "_layer1.png";
        if (archive_.contains(base)) {
            renderer_.sprite(renderer_.image(base), target, source);
        }
        if (archive_.contains(layer)) {
            renderer_.sprite(renderer_.image(layer), target, source, 0, {100, 255, 100, 255});
        }
        return;
    }
    const auto path = "img/ship/drones/" + bp.art + "_charged.png";
    if (!archive_.contains(path)) {
        return;
    }
    auto& image = renderer_.image(path);
    const float offset = bp.type == "DEFENSE" || bp.id == "COMBAT_BEAM_2" ? 4.f : 0.f;
    const float x = centerX + half(image.w);
    const float y = centerY + offset + half(image.h);
    renderer_.draw(path, x, y);
    if (bp.type == "DEFENSE") {
        // The defense gun overlay is blitted at 64 x 64 at the same origin.
        const auto gun = "img/ship/drones/" + bp.art + "_gun_on.png";
        if (archive_.contains(gun)) {
            auto& overlay = renderer_.image(gun);
            renderer_.sprite(overlay, {x, y, 64, 64}, {0, 0, float(overlay.w), float(overlay.h)});
        }
    }
}

} // namespace ftl
