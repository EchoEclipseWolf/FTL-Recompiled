#include "run.hpp"
#include <algorithm>

namespace ftl {
void Run::renderCrewInfo(const RunCrew& member, float infoX, float infoY) {
    const auto& race = catalog_.appearance(member.appearance.species);
    renderer_.windowFrame({infoX, infoY + 27, 337, 182});
    auto title = member.appearance.name;
    title += " (";
    title += race.title.reference ? texts_.get(race.title.value, locale_) : race.title.value;
    title += ')';
    float titleEndY = infoY + 40;
    for (const auto& titleLine : wrappedText(16, title, 305)) {
        text(titleLine, infoX + 18, titleEndY, 16);
        titleEndY += textLineHeight(16, title);
    }
    const auto description = race.description.reference
                                 ? texts_.get(race.description.value, locale_)
                                 : race.description.value;
    float rowY = titleEndY + 15;
    const auto descriptionLines = wrappedText(10, description, 305);
    for (const auto& line : descriptionLines) {
        text(line, infoX + 18, rowY, 10);
        rowY += textLineHeight(10, description);
    }
    // The source adds one blank line between description and powers.
    rowY += textLineHeight(10, description);
    for (const auto& power : race.powers) {
        const std::string value = power.reference ? texts_.get(power.value, locale_) : power.value;
        const auto trait = "-" + value;
        for (const auto& line : wrappedText(10, trait, 305)) {
            text(line, infoX + 18, rowY, 10);
            rowY += textLineHeight(10, trait);
        }
    }
    renderer_.windowFrame({infoX, infoY + 219, 337, 256});
    text(texts_.get("crew_skills", locale_), infoX + 35, infoY + 234, 14);
    for (std::size_t skill = 0; skill < 6; ++skill) {
        const float sx = infoX + 46.f + static_cast<float>(skill % 2) * 155.f;
        const float iconY = infoY + 272.f + static_cast<float>(skill / 2) * 66.f;
        // InfoBox::OnRender 0x54c710: CrewBlueprint::RenderSkill(length 110,
        // height 9), then GetSkillTooltip 0x4768f0 with infoScreen = true.
        renderCrewSkill(member, int(skill), sx - 30, iconY, 110, 9);
        const auto caption = crewSkillTooltip(member, int(skill), true);
        float ty = iconY + 20;
        for (const auto& line : wrappedText(9, caption, 140)) {
            text(line, sx + 55, ty, 9, true);
            ty += textLineHeight(9, caption);
        }
    }
}

void Run::renderSystemInfo(
    const RunSystem& sys, int level, float infoX, float infoY, int forceWidth, bool detailed) {
    const auto& blueprint = catalog_.system(sys.definition.name);
    const int maximum = systemMaximum(sys);
    const int infoWidth = forceWidth > 0 ? forceWidth : static_cast<int>(682 - managementPanelX());
    auto& left = renderer_.image("img/upgradeUI/details_base_A.png");
    auto& center = renderer_.image("img/upgradeUI/details_base_B.png");
    auto& right = renderer_.image("img/upgradeUI/details_base_C.png");
    const int count = std::max(1, (infoWidth - left.w - right.w) / center.w);
    const float descriptionWidth = 305.f + static_cast<float>(center.w * (count - 1));
    // InfoBox::OnRender54c710 draws the complete detail layer before its
    // primary description window. Its maximum comes from the live system.
    if (detailed) {
        const float powerY = infoY + 200;
        const float rowWidth = static_cast<float>(left.w - 75 + count * center.w + 10);
        const int shown = maximum + (sys.definition.id == 7 || sys.definition.id == 8 ? 1 : 0);
        for (int row = 0; row < 8; ++row) {
            const int bar = 7 - row;
            const float rowY = powerY + 17.f + row * 26.f;
            renderer_.fill({infoX + 75, rowY, rowWidth, 24},
                           bar < shown ? SDL_Color{127, 104, 107, 255}
                                       : SDL_Color{53, 43, 45, 255});
            if (bar >= shown) {
                continue;
            }
            renderer_.draw("img/upgradeUI/details_scrap.png", infoX + 75, rowY);
            if (bar >= sys.definition.power) {
                const auto price = bar >= maximum
                                       ? std::string(" -")
                                       : std::to_string(blueprint.costs.at(std::size_t(bar - 1)));
                text(price, infoX + 105, rowY + 7, 0, false, {255, 255, 255, 255});
            }
            text(systemLevelDescription(sys.definition.id, bar),
                 infoX + 149,
                 rowY + 5,
                 10,
                 false,
                 {255, 255, 255, 255});
        }
        renderer_.draw("img/upgradeUI/details_base_A.png", infoX, powerY);
        for (int strip = 0; strip < count; ++strip) {
            renderer_.draw("img/upgradeUI/details_base_B.png",
                           infoX + static_cast<float>(left.w + strip * center.w),
                           powerY + 6);
        }
        renderer_.draw("img/upgradeUI/details_base_C.png",
                       infoX + static_cast<float>(left.w + count * center.w),
                       powerY + 6);
        for (int bar = 0; bar < maximum; ++bar) {
            renderer_.fill({infoX + 27, powerY + 202.f - bar * 26.f, 28, 18},
                           bar < sys.definition.power ? options_.colorblind
                                                            ? SDL_Color{243, 255, 238, 255}
                                                            : SDL_Color{100, 255, 100, 255}
                           : bar < level              ? SDL_Color{255, 255, 100, 255}
                                                      : SDL_Color{255, 255, 100, 64});
        }
    }
    const auto& titleSource = sys.definition.id == 11 && !sys.definition.weapon.empty()
                                  ? catalog_.equipment(sys.definition.weapon).title
                                  : blueprint.title;
    const auto& descriptionSource = sys.definition.id == 11 && !sys.definition.weapon.empty()
                                        ? catalog_.equipment(sys.definition.weapon).description
                                        : blueprint.description;
    const auto title =
        titleSource.reference ? texts_.get(titleSource.value, locale_) : titleSource.value;
    const auto description = descriptionSource.reference
                                 ? texts_.get(descriptionSource.value, locale_)
                                 : descriptionSource.value;
    const auto descriptionLines = wrappedText(10, description, descriptionWidth);
    const bool weak =
        sys.definition.power == 1 && (sys.definition.id == 0 || sys.definition.id == 3);
    const auto warning = weak ? texts_.get("weak_warning", locale_) : std::string{};
    const auto warningLines =
        weak ? wrappedText(10, warning, descriptionWidth) : std::vector<std::string>{};
    const float height = std::max(
        135.f,
        63.f + textLineHeight(10, description) * static_cast<float>(descriptionLines.size()) +
            textLineHeight(10, warning) * static_cast<float>(warningLines.size()));
    const float frameY = infoY + 67 - (height - 135);
    renderer_.windowFrame({infoX, frameY, descriptionWidth + 32, height});
    float titleEndY = frameY + 13;
    for (const auto& titleLine : wrappedText(16, title, descriptionWidth)) {
        text(titleLine, infoX + 18, titleEndY, 16, false, {255, 255, 255, 255});
        titleEndY += textLineHeight(16, title);
    }
    float descriptionY = titleEndY + (weak ? 10.f : 15.f);
    for (const auto& line : warningLines) {
        text(line, infoX + 18, descriptionY, 10, false, {255, 120, 120, 255});
        descriptionY += textLineHeight(10, warning);
    }
    for (const auto& line : descriptionLines) {
        text(line, infoX + 18, descriptionY, 10, false, {255, 255, 255, 255});
        descriptionY += textLineHeight(10, description);
    }
}
} // namespace ftl
