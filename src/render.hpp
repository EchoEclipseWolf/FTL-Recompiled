#pragma once
#include "archive.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftl {
struct Rect {
    float x;
    float y;
    float w;
    float h;

    bool contains(float px, float py) const {
        return px > x && px < x + w && py > y && py < y + h;
    }
};

struct Texture {
    std::vector<unsigned char> pixels;
    SDL_Texture* value = nullptr;
    int w = 0;
    int h = 0;
    bool missingGraphic = false;

    Texture() = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    ~Texture() {
        SDL_DestroyTexture(value);
    }
};

struct Glyph {
    int x;
    int y;
    int w;
    int h;
    int ascent;
    float pre;
    float post;
};

struct Font {
    std::unique_ptr<Texture> atlas;
    int height;
    int baseline;
    std::unordered_map<std::uint32_t, Glyph> glyphs;
    int lineHeight = 0;
};

struct AchievementDrawing {
    std::string image;
    std::string miniOn;
    std::string miniOff;
    int dimension = 64;
    bool unlocked = false;
    bool fresh = false;
    bool victory = false;
    bool twoLayouts = false;
    std::array<int, 3> layouts{-1, -1, -1};
};

class Renderer {
    const Archive& archive_;
    SDL_Window* window_ = nullptr;
    float tint_ = 1;
    float scale_ = 1;
    SDL_FPoint pivot_{};
    Rect transformed(Rect rect) const;
    void filtered(Texture& texture, Rect destination, Rect source, float tint, float alpha);
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* target_ = nullptr;
    int fullscreenMode_ = 0;
    bool vsyncAvailable_ = false;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures_;
    std::unordered_map<std::string, std::string> imageFailures_;
    std::string imageLanguage_ = "en";
    std::unordered_map<std::string, Font> fonts_;

    struct MaskedLayer {
        const Texture* mask;
        Rect destination;
        std::vector<unsigned char> previous;
        std::vector<Rect> missingGraphics;
    };

    struct MaskedStencil {
        std::vector<SDL_Vertex> vertices;
        std::vector<int> indices;
    };

    std::vector<MaskedLayer> maskedLayers_;
    std::vector<std::unique_ptr<Texture>> maskedBackgrounds_;
    std::unordered_map<std::string, MaskedStencil> maskedStencils_;
    const MaskedStencil& maskedStencil(const Texture& mask, Rect destination);
    std::unique_ptr<Texture> texture(const unsigned char* pixels, int w, int h);
    void drawMissingGraphic(Rect transformedDestination);
    void release() noexcept;

public:
    explicit Renderer(const Archive& archive,
                      bool hidden = false,
                      int width = 1280,
                      int height = 720,
                      bool accelerated = false);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Texture& image(const std::string& name);

    void imageLanguage(const std::string& value) {
        imageLanguage_ = value;
    }

    void beginMaskedLayer(const std::string& maskName, SDL_FPoint position);
    void endMaskedLayer();
    Font& font(const std::string& name);
    Font& chineseFont(int pixels, bool outline, const std::string& characters, int sourceSize = 0);

    Font& sourceChineseFont(int size, const std::string& characters) {
        if (size == 0) {
            return sourceFont(0, characters);
        }
        size = size >= 6 && size <= 8 ? 9 : size == 11 ? 12 : size;
        return chineseFont(0, false, characters, size);
    }

    Font& sourceFont(int size, const std::string& characters, bool chinese = false);
    void achievement(const AchievementDrawing& value,
                     SDL_FPoint position,
                     int style = 0,
                     const std::string& newLabel = "",
                     bool chinese = false);
    void tooltip(const std::string& title,
                 const std::string& body,
                 SDL_FPoint mouse,
                 float wrapWidth = 0,
                 bool chinese = false);
    int systemInfoCenterCount(int system,
                              int maximum,
                              const std::function<std::string(int)>& levelDescription,
                              bool chinese = false);

    static float sourceFontScale(int size, bool chinese = false) {
        if (chinese) {
            return 1;
        }
        switch (size) {
        case 14:
        case 16:
        case 18:
        case 52:
        case 59:
        case 62:
            return 2;
        case 20:
        case 24:
        case 53:
        case 63:
            return 3;
        case 64:
            return 4;
        default:
            return 1;
        }
    }

    static float sourceLineHeight(int size, const Font& font) {
        if (font.lineHeight != 0) {
            return float(font.lineHeight);
        }
        // ResourceControl::PreloadFont5a2b10 and font_data::init_bitmap57a5a0.
        switch (size) {
        case 0:
            return 15;
        case 5:
            return 11;
        case 6:
        case 7:
        case 8:
        case 9:
            return 15;
        case 10:
            return 17;
        case 11:
        case 12:
        case 16:
            return 20;
        case 13:
            return 22;
        case 14:
        case 18:
            return 34;
        case 20:
        case 24:
            return 53;
        case 51:
        case 61:
            return 10;
        case 52:
        case 62:
            return 24;
        case 53:
        case 63:
            return 38;
        case 59:
            return 18;
        case 64:
            return 52;
        default:
            return 20;
        }
    }

    void clear();
    bool setVsync(bool enabled);
    bool vsync() const;
    bool setFullscreen(int mode);

    int fullscreen() const {
        return fullscreenMode_;
    }

    bool vsyncAvailable() const {
        return vsyncAvailable_;
    }

    void setTint(float tint) {
        tint_ = tint;
    }

    void setTransform(float scale = 1, SDL_FPoint pivot = {}) {
        scale_ = scale;
        pivot_ = pivot;
    }

    void setClip(std::optional<Rect> clip);

    void draw(Texture& t,
              Rect dst,
              const Rect* src = nullptr,
              float tint = 1,
              float alpha = 1,
              bool linear = false);
    void sprite(Texture& t,
                Rect dst,
                Rect src,
                double angle = 0,
                SDL_Color color = {255, 255, 255, 255},
                bool mirror = false,
                const SDL_FPoint* center = nullptr);
    void draw(const std::string& name,
              float x,
              float y,
              float tint = 1,
              float alpha = 1,
              bool linear = false);
    void maskedImage(const std::string& name,
                     Rect destination,
                     const std::string& maskName,
                     SDL_FPoint maskPosition,
                     float alpha = 1,
                     SDL_Color color = {255, 255, 255, 255});
    void imageExcluding(const std::string& name,
                        Rect destination,
                        Rect exclusion,
                        int inset,
                        float alpha = 1,
                        SDL_Color color = {255, 255, 255, 255});
    void fill(Rect r, SDL_Color color);
    void texturedLine(Texture& texture, SDL_FPoint from, SDL_FPoint to, SDL_Color color);
    void polygon(const std::vector<SDL_FPoint>& points, SDL_Color color);
    void texturedPie(const std::string& name,
                     SDL_FPoint origin,
                     SDL_FPoint center,
                     float radius,
                     float firstDegrees,
                     float lastDegrees);
    void laser(SDL_FPoint from,
               SDL_FPoint to,
               int thickness,
               SDL_Color color,
               std::optional<Rect> ellipse = std::nullopt,
               bool outside = false);
    void shieldArc(
        SDL_FPoint center, SDL_FPoint radii, int startDegrees, int endDegrees, float thickness);
    void windowFrame(Rect outer, bool background = true, const std::vector<Rect>& cutouts = {});
    void reactorWire(float x, float y, int capacity, int available);
    float measure(Font& font, const std::string& text, float scale = 1);
    float inkWidth(Font& font, const std::string& value, float scale = 1);
    std::vector<std::string> wrappedLines(
        Font& font, const std::string& text, float width, float scale = 1, bool chinese = false);
    std::pair<float, float> bounds(Font& font, const std::string& text, float scale = 1);
    void text(Font& font,
              const std::string& text,
              float x,
              float y,
              float scale = 1,
              SDL_Color color = {255, 255, 255, 255},
              bool centered = false);
    void textWithGlow(const std::string& value,
                      float x,
                      float y,
                      bool chinese,
                      SDL_Color color,
                      float alpha = 1,
                      bool centered = false);
    void textInput(const std::string& value,
                   std::size_t caretByte,
                   int size,
                   SDL_FPoint position,
                   bool caretVisible,
                   bool chinese = false);
    void present();
    void capture(const std::filesystem::path& path);
    SDL_FPoint logical(float x, float y) const;
    void resize(int w, int h);

    const char* backend() const {
        return SDL_GetRendererName(renderer_);
    }

    SDL_Window* window() const {
        return window_;
    }
};
} // namespace ftl
