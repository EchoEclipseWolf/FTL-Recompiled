#include "menu.hpp"
#include <algorithm>
#include <array>
#include <sstream>

namespace ftl {
void Menu::creditsStep(std::uint32_t ticks) {
    if (modal_ != "credits") {
        return;
    }
    // CreditScreen::OnRender0x5597a0 accrues16factor/s *3.5 =56px/s.
    // Keep the source cadence in unscaled UI updates; rendering is read-only.
    for (std::uint32_t tick = 0; tick < ticks; ++tick) {
        creditPause_ -= Time::step;
        if (creditPause_ < 0) {
            const double next = creditScroll_ - 56 * Time::step;
            if (creditScroll_ > -720 && next <= -720) {
                creditScroll_ = -720;
                creditPause_ = 3;
            } else {
                creditScroll_ = next;
            }
        }
    }
    // CreditScreen::Done0x559720, integer rows of three Kickstarter names.
    if (creditScroll_ < -3906 - 40 * double(creditNames_.size() / 3)) {
        modal_.clear();
    }
}

void Menu::creditsRender() {
    renderer_.draw("img/stars/bg_darknebula.png", 0, 0, .5f);
    const auto alpha = Uint8(std::clamp(creditPause_ > 4 ? 5 - creditPause_ : 1.0, 0.0, 1.0) * 255);
    const auto line = [&](const std::string& value, int font, float x, int y, float width = 0) {
        const float screenY = y + float(int(creditScroll_));
        const float visibleY = y + float(creditScroll_);
        if (visibleY <= -50 || visibleY >= 720) {
            return;
        }
        auto& face = renderer_.sourceFont(font, value, locale_ == "zh-Hans");
        const float scale = Renderer::sourceFontScale(font, locale_ == "zh-Hans");
        const auto lines = renderer_.wrappedLines(face, value, width, scale, locale_ == "zh-Hans");
        float textY = screenY;
        for (const auto& text : lines) {
            renderer_.text(face, text, x, textY, scale, {255, 255, 255, alpha}, true);
            textY += Renderer::sourceLineHeight(font, face);
        }
    };
    // Victory starts with the surviving ship/crew, before the ordinary roll.
    // CreditScreen::Start0x55ca00 and OnRender0x5597a0.
    if (!creditShip_.empty()) {
        line(texts_.get("thank_ship", locale_), 14, 640, 150);
        line(creditShip_, 20, 640, 190);
        line(texts_.get("thank_crew", locale_), 14, 640, 270);
        line(creditCrew_, 18, 640, 310, 900);
        line(texts_.get("credit_victory", locale_), 14, 640, 510, 750);
    }
    // CreditScreen::OnRender0x5597a0, source localized text/order/coordinates.
    line(texts_.get("game_from", locale_), 14, 640, 775);
    line(texts_.get("subset_games", locale_), 18, 640, 815);
    line(texts_.get("created_by", locale_), 14, 640, 925);
    line(texts_.get("matthew_justin", locale_), 18, 640, 965);
    line(texts_.get("addition_code", locale_), 14, 640, 1075);
    line(texts_.get("andrew_church", locale_), 18, 640, 1115);
    line(texts_.get("music_by", locale_), 14, 640, 1205);
    line(texts_.get("ben_prunty", locale_), 18, 640, 1245);
    line(texts_.get("writing_by", locale_), 14, 640, 1335);
    line(texts_.get("tom_jubert", locale_), 18, 640, 1375);
    line(texts_.get("guest_writer", locale_), 14, 640, 1460);
    line(texts_.get("chris_a", locale_), 18, 640, 1500);
    line(texts_.get("additional_art", locale_), 14, 640, 1590);
    line(texts_.get("bryan_ma", locale_), 18, 640, 1630);
    line(texts_.get("editing", locale_), 14, 640, 1720);
    line(texts_.get("alison_w", locale_), 18, 640, 1760);
    line(texts_.get("additional_testing", locale_), 14, 640, 1850);
    line(texts_.get("family_friends", locale_), 18, 640, 1890, 750);
    line(texts_.get("credits_loc_riotloc", locale_), 18, 640, 2090, 750);
    line(texts_.get("credits_loc_project", locale_), 14, 640, 2180, 750);
    line(texts_.get("credits_loc_project_names", locale_), 18, 640, 2220, 750);
    line(texts_.get("credits_loc_fr", locale_), 14, 640, 2320, 750);
    line(texts_.get("credits_loc_fr_names", locale_), 18, 640, 2360, 750);
    line(texts_.get("credits_loc_it", locale_), 14, 640, 2460, 750);
    line(texts_.get("credits_loc_it_names", locale_), 18, 640, 2500, 750);
    line(texts_.get("credits_loc_de", locale_), 14, 640, 2600, 750);
    line(texts_.get("credits_loc_de_names", locale_), 18, 640, 2640, 750);
    line(texts_.get("credits_loc_es", locale_), 14, 640, 2706, 750);
    line(texts_.get("credits_loc_es_names", locale_), 18, 640, 2746, 750);
    line(texts_.get("credits_loc_pl", locale_), 14, 640, 2846, 750);
    line(texts_.get("credits_loc_pl_names", locale_), 18, 640, 2886, 750);
    line(texts_.get("credits_loc_pt_BR", locale_), 14, 640, 2986, 750);
    line(texts_.get("credits_loc_pt_BR_names", locale_), 18, 640, 3026, 750);
    line(texts_.get("credits_loc_ru", locale_), 14, 640, 3126, 750);
    line(texts_.get("credits_loc_ru_names", locale_), 18, 640, 3166, 750);
    line(texts_.get("kickstarter", locale_), 14, 640, 3366, 750);
    constexpr std::array<float, 3> columns{270, 640, 1010};
    for (std::size_t i = 0; i < creditNames_.size(); ++i) {
        line(creditNames_[i], 14, columns[i % 3], 3446 + 40 * int(i / 3));
    }
    const int end = 40 * int(creditNames_.size() / 3);
    line(texts_.get("third_party", locale_), 12, 640, end + 3546);
    line(texts_.get("folder", locale_), 12, 640, end + 3586);
    line(texts_.get("bass", locale_), 12, 640, end + 3666);
    line(texts_.get("rapidxml", locale_), 12, 640, end + 3716);
    line(texts_.get("freetype", locale_), 12, 640, end + 3766);
    // GetText0x5a9f40 supplies its source missing-key text when absent.
    line(texts_.get("font", locale_), 12, 640, end + 3866);
}
} // namespace ftl
