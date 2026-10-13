#include "run.hpp"
#include <algorithm>

namespace ftl {
void Run::showWriteError() {
    // ShowWriteError51f330 retains a once-until-success guard independently
    // of whether the extra ChoiceBox is still open.
    if (writeErrorLatched_) {
        return;
    }
    writeErrorLatched_ = true;
    writeErrorOpen_ = true;
    writeErrorHovered_ = false;
    writeErrorOpenedUiTick_ = animation_;
    writeErrorText_ = texts_.get("save_error", locale_);
    writeErrorChoiceText_ = "1. " + texts_.get("continue", locale_);
}

void Run::clearWriteError() {
    // ClearWriteError51f800 does not close the currently displayed box.
    writeErrorLatched_ = false;
}

Rect Run::writeErrorChoiceRect() const {
    const auto body = wrappedText(12, writeErrorText_, 565);
    const auto choices = wrappedText(12, writeErrorChoiceText_, 565);
    float width = 0;
    auto& font = localizedFont(12, writeErrorChoiceText_);
    for (const auto& line : choices) {
        width = std::max(width, renderer_.measure(font, line, textScale(12)));
    }
    return {352,
            200 + float(body.size()) * textLineHeight(12, writeErrorText_),
            float(int(width)),
            float(int(float(choices.size()) * textLineHeight(12, writeErrorChoiceText_) + 12))};
}

void Run::writeErrorMove(float x, float y) {
    const auto choice = writeErrorChoiceRect();
    // OnRender4ec490 stores the nominal measured rectangle; MouseMove4e82b0
    // performs inclusive comparisons without expanding that rectangle.
    writeErrorHovered_ = writeErrorOpen_ && x >= choice.x && x <= choice.x + choice.w &&
                         y >= choice.y && y <= choice.y + choice.h;
}

void Run::writeErrorClick(float x, float y) {
    if (!writeErrorOpen_) {
        return;
    }
    writeErrorMove(x, y);
    if (writeErrorHovered_ &&
        (options_.dialogKeys != 2 || animation_ - writeErrorOpenedUiTick_ >= 18)) {
        dismissWriteError();
    }
}

bool Run::writeErrorKey(const std::string& key) {
    if (!writeErrorOpen_) {
        return false;
    }
    // ChoiceBox::KeyDown4e85e0 accepts numeric keys only; Return/Space/Escape
    // do not select Continue. Other keys remain owned by the open extra box.
    if (options_.dialogKeys != 0 &&
        (options_.dialogKeys != 2 || animation_ - writeErrorOpenedUiTick_ >= 18) && key == "1") {
        dismissWriteError();
    }
    return true;
}

void Run::renderWriteError() {
    if (!writeErrorOpen_) {
        return;
    }
    // Extra ChoiceBox523c70 has centered=true,font12,n30=30. OnRender4ec490
    // uses start327,125 and WindowFrame(7,22,602,377): outer327,140,616,391.
    renderer_.setTint(1);
    renderer_.windowFrame({327, 140, 616, 391});
    float y = 170;
    for (const auto& line : wrappedText(12, writeErrorText_, 565)) {
        text(line, 352, y, 12, false, {255, 255, 255, 255});
        y += textLineHeight(12, writeErrorText_);
    }
    y += 40;
    for (const auto& line : wrappedText(12, writeErrorChoiceText_, 565)) {
        text(line,
             352,
             y,
             12,
             false,
             writeErrorHovered_ ? SDL_Color{243, 255, 80, 255} : SDL_Color{255, 255, 255, 255});
        y += textLineHeight(12, writeErrorChoiceText_);
    }
}
} // namespace ftl
