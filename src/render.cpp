#include "render.hpp"
#include "log.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stb_image.h>
#include <stdexcept>

namespace ftl {
namespace {
void check(bool ok) {
    if (!ok) {
        throw std::runtime_error(SDL_GetError());
    }
}

float sample(const Texture& texture, float u, float v, int channel) {
    const int x = int(std::floor(u));
    const int y = int(std::floor(v));
    auto pixel = [&](int px, int py) {
        return float(texture.pixels[(std::size_t(std::clamp(py, 0, texture.h - 1)) * texture.w +
                                     std::clamp(px, 0, texture.w - 1)) *
                                        4 +
                                    channel]);
    };
    return std::lerp(std::lerp(pixel(x, y), pixel(x + 1, y), u - x),
                     std::lerp(pixel(x, y + 1), pixel(x + 1, y + 1), u - x),
                     v - y);
}

std::vector<std::uint32_t> codepoints(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i++]);
        std::uint32_t cp = c;
        int n = 0;
        if (c >= 0xf0) {
            cp = c & 7;
            n = 3;
        } else if (c >= 0xe0) {
            cp = c & 15;
            n = 2;
        } else if (c >= 0xc0) {
            cp = c & 31;
            n = 1;
        }
        while (n-- && i < s.size()) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 63);
        }
        out.push_back(cp);
    }
    return out;
}
} // namespace

Renderer::Renderer(const Archive& a, bool hidden, int w, int h, bool accelerated) : archive_(a) {
    try {
        check(SDL_Init(SDL_INIT_VIDEO));
        window_ = SDL_CreateWindow(
            "FTLRecomp — Main Menu", w, h, SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
        if (!window_) {
            throw std::runtime_error(SDL_GetError());
        }
        if (accelerated) {
            renderer_ = SDL_CreateRenderer(window_, nullptr);
            if (renderer_) {
                check(SDL_SetRenderLogicalPresentation(
                    renderer_, 1280, 720, SDL_LOGICAL_PRESENTATION_LETTERBOX));
                check(SDL_SetRenderVSync(renderer_, 0));
                target_ = SDL_CreateTexture(
                    renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 1280, 720);
                if (!target_) {
                    throw std::runtime_error(SDL_GetError());
                }
                check(SDL_SetTextureScaleMode(target_, SDL_SCALEMODE_NEAREST));
                check(SDL_SetRenderTarget(renderer_, target_));
            }
        } else {
            surface_ = SDL_CreateSurface(1280, 720, SDL_PIXELFORMAT_RGBA32);
            if (!surface_) {
                throw std::runtime_error(SDL_GetError());
            }
            renderer_ = SDL_CreateSoftwareRenderer(surface_);
        }
        if (!renderer_) {
            throw std::runtime_error(SDL_GetError());
        }
        check(SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND));
        SDL_HideCursor();
    } catch (...) {
        // A throwing constructor does not run ~Renderer. Release partial SDL state.
        release();
        throw;
    }
}

Renderer::~Renderer() {
    release();
}

bool Renderer::setVsync(bool enabled) {
    if (surface_) {
        return false;
    }
    if (!SDL_SetRenderVSync(renderer_, enabled ? 1 : 0)) {
        vsyncAvailable_ = false;
        return false;
    }
    int actual = 0;
    vsyncAvailable_ = SDL_GetRenderVSync(renderer_, &actual) && actual == (enabled ? 1 : 0);
    return vsyncAvailable_;
}

bool Renderer::vsync() const {
    int actual = 0;
    return !surface_ && SDL_GetRenderVSync(renderer_, &actual) && actual != 0;
}

bool Renderer::setFullscreen(int mode) {
    if (mode < 0 || mode > 3) {
        SDL_SetError("Invalid fullscreen mode");
        return false;
    }
    if (mode == fullscreenMode_) {
        return true;
    }
    SDL_DisplayMode nativeMode{};
    const SDL_DisplayMode* desired = nullptr;
    if (mode == 3) {
        if (!SDL_GetClosestFullscreenDisplayMode(
                SDL_GetDisplayForWindow(window_), 1280, 720, 0, false, &nativeMode) ||
            nativeMode.w != 1280 || nativeMode.h != 720) {
            SDL_SetError("1280x720 native fullscreen mode unavailable");
            return false;
        }
        desired = &nativeMode;
    }
    const auto* previous = SDL_GetWindowFullscreenMode(window_);
    const std::optional<SDL_DisplayMode> previousMode =
        previous ? std::optional<SDL_DisplayMode>(*previous) : std::nullopt;
    const auto restore = [&] {
        SDL_SetWindowFullscreenMode(window_, previousMode ? &*previousMode : nullptr);
        SDL_SetWindowFullscreen(window_, fullscreenMode_ != 0);
    };
    if (!SDL_SetWindowFullscreenMode(window_, desired) ||
        !SDL_SetWindowFullscreen(window_, mode != 0) || !SDL_SyncWindow(window_)) {
        const std::string failure = SDL_GetError();
        restore();
        SDL_SetError("%s", failure.c_str());
        return false;
    }
    if (target_ &&
        !SDL_SetRenderLogicalPresentation(renderer_,
                                          1280,
                                          720,
                                          mode == 1 ? SDL_LOGICAL_PRESENTATION_STRETCH
                                                    : SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        const std::string failure = SDL_GetError();
        restore();
        SDL_SetError("%s", failure.c_str());
        return false;
    }
    fullscreenMode_ = mode;
    return true;
}

void Renderer::release() noexcept {
    maskedLayers_.clear();
    maskedBackgrounds_.clear();
    maskedStencils_.clear();
    fonts_.clear();
    textures_.clear();
    SDL_DestroyTexture(target_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroySurface(surface_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

void Renderer::beginMaskedLayer(const std::string& maskName, SDL_FPoint position) {
    auto& mask = image(maskName);
    MaskedLayer layer{
        &mask,
        transformed({position.x, position.y, float(mask.w), float(mask.h)}),
        std::vector<unsigned char>(!mask.missingGraphic && surface_ ? 1280 * 720 * 4 : 0),
        {}};
    if (mask.missingGraphic) {
        maskedLayers_.push_back(std::move(layer));
        return;
    }
    if (!surface_) {
        const auto depth = maskedLayers_.size();
        if (maskedBackgrounds_.size() <= depth) {
            maskedBackgrounds_.resize(depth + 1);
        }
        if (!maskedBackgrounds_[depth]) {
            auto background = std::make_unique<Texture>();
            background->w = 1280;
            background->h = 720;
            background->value = SDL_CreateTexture(
                renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 1280, 720);
            check(background->value != nullptr);
            check(SDL_SetTextureBlendMode(background->value, SDL_BLENDMODE_NONE));
            check(SDL_SetTextureScaleMode(background->value, SDL_SCALEMODE_NEAREST));
            maskedBackgrounds_[depth] = std::move(background);
        }
        auto* previousTarget = SDL_GetRenderTarget(renderer_);
        check(previousTarget != nullptr);
        const bool clipped = SDL_RenderClipEnabled(renderer_);
        SDL_Rect clip{};
        if (clipped) {
            check(SDL_GetRenderClipRect(renderer_, &clip));
        }
        check(SDL_SetRenderTarget(renderer_, maskedBackgrounds_[depth]->value));
        check(SDL_SetRenderClipRect(renderer_, nullptr));
        check(SDL_RenderTexture(renderer_, previousTarget, nullptr, nullptr));
        check(SDL_SetRenderTarget(renderer_, previousTarget));
        check(SDL_SetRenderClipRect(renderer_, clipped ? &clip : nullptr));
        maskedLayers_.push_back(std::move(layer));
        return;
    }
    check(SDL_FlushRenderer(renderer_));
    auto* pixels = surface_;
    if (!pixels || pixels->w != 1280 || pixels->h != 720) {
        throw std::runtime_error("invalid_masked_layer_surface");
    }
    check(SDL_LockSurface(pixels));
    for (int y = 0; y < 720; ++y) {
        const auto* row = static_cast<const unsigned char*>(pixels->pixels) + y * pixels->pitch;
        std::copy_n(row, 1280 * 4, layer.previous.data() + y * 1280 * 4);
    }
    SDL_UnlockSurface(pixels);
    maskedLayers_.push_back(std::move(layer));
}

const Renderer::MaskedStencil& Renderer::maskedStencil(const Texture& mask, Rect destination) {
    const auto key = std::to_string(reinterpret_cast<std::uintptr_t>(&mask)) + ':' +
                     std::to_string(destination.x) + ':' + std::to_string(destination.y) + ':' +
                     std::to_string(destination.w) + ':' + std::to_string(destination.h);
    if (const auto found = maskedStencils_.find(key); found != maskedStencils_.end()) {
        return found->second;
    }
    if (maskedStencils_.size() >= 32) {
        maskedStencils_.clear();
    }
    MaskedStencil stencil;
    const auto outside = [&](int x, int y) {
        const float u = (x + .5f - destination.x) / destination.w;
        const float v = (y + .5f - destination.y) / destination.h;
        if (u >= 0 && u < 1 && v >= 0 && v < 1) {
            return mask.pixels[(std::size_t(int(v * mask.h)) * mask.w + int(u * mask.w)) * 4 + 3] ==
                   0;
        }
        return true;
    };
    // Cache the complement of the original binary stencil as pixel-aligned
    // scanline quads. The GPU restores those pixels from the previous target;
    // no framebuffer readback or custom blend mode is required.
    for (int y = 0; y < 720; ++y) {
        int x = 0;
        while (x < 1280) {
            if (!outside(x, y)) {
                ++x;
                continue;
            }
            const int left = x;
            while (x < 1280 && outside(x, y)) {
                ++x;
            }
            const int base = int(stencil.vertices.size());
            for (const auto point :
                 std::array<SDL_FPoint, 4>{SDL_FPoint{float(left), float(y)},
                                           SDL_FPoint{float(x), float(y)},
                                           SDL_FPoint{float(x), float(y + 1)},
                                           SDL_FPoint{float(left), float(y + 1)}}) {
                stencil.vertices.push_back(
                    {point, {1, 1, 1, 1}, {point.x / 1280.f, point.y / 720.f}});
            }
            for (const int index : {0, 1, 2, 0, 2, 3}) {
                stencil.indices.push_back(base + index);
            }
        }
    }
    return maskedStencils_.emplace(key, std::move(stencil)).first->second;
}

void Renderer::endMaskedLayer() {
    if (maskedLayers_.empty()) {
        throw std::runtime_error("missing_masked_layer");
    }
    auto layer = std::move(maskedLayers_.back());
    maskedLayers_.pop_back();
    if (layer.mask->missingGraphic) {
        drawMissingGraphic(layer.destination);
        for (const auto& destination : layer.missingGraphics) {
            drawMissingGraphic(destination);
        }
        return;
    }
    if (!surface_) {
        const auto& stencil = maskedStencil(*layer.mask, layer.destination);
        const bool clipped = SDL_RenderClipEnabled(renderer_);
        SDL_Rect clip{};
        if (clipped) {
            check(SDL_GetRenderClipRect(renderer_, &clip));
        }
        check(SDL_SetRenderClipRect(renderer_, nullptr));
        if (!stencil.vertices.empty()) {
            check(SDL_RenderGeometry(renderer_,
                                     maskedBackgrounds_[maskedLayers_.size()]->value,
                                     stencil.vertices.data(),
                                     int(stencil.vertices.size()),
                                     stencil.indices.data(),
                                     int(stencil.indices.size())));
        }
        check(SDL_SetRenderClipRect(renderer_, clipped ? &clip : nullptr));
        for (const auto& destination : layer.missingGraphics) {
            drawMissingGraphic(destination);
        }
        return;
    }
    check(SDL_FlushRenderer(renderer_));
    auto* pixels = surface_;
    if (!pixels || pixels->w != 1280 || pixels->h != 720) {
        throw std::runtime_error("invalid_masked_layer_surface");
    }
    check(SDL_LockSurface(pixels));
    // RenderTarget55ff00 writes the authored texture into a binary stencil.
    // Restore prior pixels outside that stencil after every kind of drawing,
    // including software-rasterized primitives. This avoids SDL backends that
    // cannot implement the custom alpha-multiplication blend mode.
    for (int y = 0; y < 720; ++y) {
        auto* row = static_cast<unsigned char*>(pixels->pixels) + y * pixels->pitch;
        for (int x = 0; x < 1280; ++x) {
            const float u = (x + .5f - layer.destination.x) / layer.destination.w;
            const float v = (y + .5f - layer.destination.y) / layer.destination.h;
            const bool inside =
                u >= 0 && u < 1 && v >= 0 && v < 1 &&
                layer.mask->pixels[(std::size_t(int(v * layer.mask->h)) * layer.mask->w +
                                    int(u * layer.mask->w)) *
                                       4 +
                                   3] != 0;
            if (!inside) {
                std::copy_n(layer.previous.data() + (y * 1280 + x) * 4, 4, row + x * 4);
            }
        }
    }
    SDL_UnlockSurface(pixels);
    // Missing artwork is a requested diagnostic overlay, including inside
    // nested stencil contexts. Replay it after the authored mask is applied.
    for (const auto& destination : layer.missingGraphics) {
        drawMissingGraphic(destination);
    }
}

void Renderer::drawMissingGraphic(Rect transformedDestination) {
    if (transformedDestination.w <= 0 || transformedDestination.h <= 0) {
        return;
    }
    // Keep the requested diagnostic square at the caller's origin, even for
    // wide strips/glows. Authored tint, opacity and frame crop do not hide it.
    const float side = std::min(transformedDestination.w, transformedDestination.h);
    transformedDestination.w = side;
    transformedDestination.h = side;
    if (!maskedLayers_.empty()) {
        maskedLayers_.back().missingGraphics.push_back(transformedDestination);
    }
    check(SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255));
    const SDL_FRect destination{transformedDestination.x,
                                transformedDestination.y,
                                transformedDestination.w,
                                transformedDestination.h};
    check(SDL_RenderFillRect(renderer_, &destination));
}

std::unique_ptr<Texture> Renderer::texture(const unsigned char* pixels, int w, int h) {
    auto s = SDL_CreateSurfaceFrom(
        w, h, SDL_PIXELFORMAT_RGBA32, const_cast<unsigned char*>(pixels), w * 4);
    if (!s) {
        throw std::runtime_error(SDL_GetError());
    }
    auto t = std::make_unique<Texture>();
    t->w = w;
    t->h = h;
    t->pixels.assign(pixels, pixels + std::size_t(w) * h * 4);
    t->value = SDL_CreateTextureFromSurface(renderer_, s);
    SDL_DestroySurface(s);
    if (!t->value) {
        throw std::runtime_error(SDL_GetError());
    }
    check(SDL_SetTextureScaleMode(t->value, SDL_SCALEMODE_NEAREST));
    check(SDL_SetTextureBlendMode(t->value, SDL_BLENDMODE_BLEND));
    return t;
}

Texture& Renderer::image(const std::string& name) {
    const auto load = [&](const std::string& path, bool localizedCandidate) -> Texture* {
        if (auto it = textures_.find(path); it != textures_.end()) {
            return it->second->missingGraphic ? nullptr : it->second.get();
        }
        if (imageFailures_.contains(path)) {
            return nullptr;
        }
        if (!archive_.contains(path)) {
            imageFailures_.emplace(path, "missing archive entry");
            return nullptr;
        }
        auto bytes = archive_.read(path);
        int width;
        int height;
        int channels;
        auto pixels =
            stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 4);
        if (!pixels) {
            const auto* reason = stbi_failure_reason();
            auto failure = reason ? std::string(reason) : std::string("invalid PNG");
            // LoadImage5a0710 reports malformed localized artwork and lookup
            // then continues to the base asset. Missing overrides are normal.
            if (localizedCandidate) {
                logging::warning("render", "invalid_graphic: {} ({})", path, failure);
            }
            imageFailures_.emplace(path, std::move(failure));
            return nullptr;
        }
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> owned(pixels, stbi_image_free);
        auto loaded = texture(pixels, width, height);
        auto* result = loaded.get();
        textures_.emplace(path, std::move(loaded));
        return result;
    };
    // GetImageId5a1270 tries localization BEFORE the already cached base asset.
    // Explicit localized paths remain direct requests for diagnostic callers.
    if (name.starts_with("img/") && !name.starts_with("img/loc-")) {
        const auto localized = "img/loc-" + imageLanguage_ + '/' + name.substr(4);
        if (auto* result = load(localized, true)) {
            return *result;
        }
    }
    if (auto* result = load(name, false)) {
        return *result;
    }
    if (auto it = textures_.find(name); it != textures_.end()) {
        return *it->second;
    }
    const auto missingGraphic = [&](const std::string& reason) -> Texture& {
        // User-requested diagnostic fallback: keep missing/bad artwork visible.
        std::array<unsigned char, 32 * 32 * 4> rgba{};
        for (std::size_t pixel = 0; pixel < rgba.size(); pixel += 4) {
            rgba[pixel] = 255;
            rgba[pixel + 3] = 255;
        }
        auto fallback = texture(rgba.data(), 32, 32);
        fallback->missingGraphic = true;
        auto& result = *fallback;
        textures_.emplace(name, std::move(fallback));
        logging::warning("render", "missing_graphic: {} ({})", name, reason);
        return result;
    };
    return missingGraphic(imageFailures_.at(name));
}

Font& Renderer::font(const std::string& name) {
    if (auto it = fonts_.find(name); it != fonts_.end()) {
        return it->second;
    }
    auto b = archive_.read(name);
    if (b.size() < 24 || std::string_view(reinterpret_cast<char*>(b.data()), 4) != "FONT" ||
        b[4] != 1 || be16(b.data() + 14) != 16) {
        throw std::runtime_error("invalid_font_header");
    }
    const auto off = be32(b.data() + 8);
    const auto to = be32(b.data() + 16);
    const auto ts = be32(b.data() + 20);
    const auto count = be16(b.data() + 12);
    if (std::uint64_t(off) + std::uint64_t(count) * 16 > b.size() ||
        std::uint64_t(to) + ts > b.size() || ts < 32) {
        throw std::runtime_error("invalid_font_bounds");
    }
    auto p = b.data() + to;
    // Original SIL TEX v2, alpha-only format 0x40; original data is big endian.
    if (std::string_view(reinterpret_cast<char*>(p), 4) != "TEX\n" || p[4] != 2 || p[5] != 0x40) {
        throw std::runtime_error("unsupported_font_texture");
    }
    int w = be16(p + 8);
    int h = be16(p + 10);
    auto pixels = be32(p + 16);
    auto length = be32(p + 20);
    if (w <= 0 || h <= 0 || std::uint64_t(w) * h > length || std::uint64_t(pixels) + length > ts) {
        throw std::runtime_error("invalid_font_texture_bounds");
    }
    Bytes rgba(std::size_t(w) * h * 4, 255);
    for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        rgba[i * 4 + 3] = p[pixels + i];
    }
    Font f{texture(rgba.data(), w, h), b[5], b[6], {}};
    for (unsigned i = 0; i < count; ++i) {
        auto g = b.data() + off + i * 16;
        Glyph glyph{be16(g + 4),
                    be16(g + 6),
                    g[8],
                    g[9],
                    std::int8_t(g[10]),
                    std::int16_t(be16(g + 12)) / 256.f,
                    std::int16_t(be16(g + 14)) / 256.f};
        if (glyph.x + glyph.w > w || glyph.y + glyph.h > h) {
            throw std::runtime_error("invalid_glyph_bounds");
        }
        f.glyphs.emplace(be32(g), glyph);
    }
    return fonts_.emplace(name, std::move(f)).first->second;
}

Font& Renderer::sourceFont(int size, const std::string& characters, bool chinese) {
    // GetFontData5a3a50 normalizes6..8 to9,11 to12, then falls back to12.
    // Numeric ID0 always ignores language in the source.
    if (size == 0) {
        auto& base = font("fonts/num_font.font");
        const std::string key = "@source:num_font";
        if (!fonts_.contains(key)) {
            // PreloadFont5a3980 overrides the asset's baseline20 with12.
            Font corrected{texture(base.atlas->pixels.data(), base.atlas->w, base.atlas->h),
                           base.height,
                           12,
                           base.glyphs};
            fonts_.emplace(key, std::move(corrected));
        }
        return fonts_.at(key);
    }
    if (chinese) {
        return sourceChineseFont(size, characters);
    }
    const char* path = "fonts/JustinFont11Bold.font";
    switch (size) {
    case 5:
        path = "fonts/c&c.font";
        break;
    case 6:
    case 7:
    case 8:
    case 9:
        path = "fonts/JustinFont8.font";
        break;
    case 10:
        path = "fonts/JustinFont10.font";
        break;
    case 13:
        path = "fonts/JustinFont12Bold.font";
        break;
    case 14:
    case 16:
    case 24:
        path = "fonts/c&cnew.font";
        break;
    case 51:
    case 52:
    case 53:
    case 59:
        path = "fonts/HL1.font";
        break;
    case 61:
    case 62:
    case 63:
    case 64: {
        auto& base = font("fonts/HL2.font");
        const std::string key = "@source:HL2";
        if (!fonts_.contains(key)) {
            Font corrected{texture(base.atlas->pixels.data(), base.atlas->w, base.atlas->h),
                           base.height,
                           8,
                           base.glyphs};
            fonts_.emplace(key, std::move(corrected));
        }
        return fonts_.at(key);
    }
    default:
        break;
    }
    return font(path);
}

void Renderer::tooltip(const std::string& title,
                       const std::string& body,
                       SDL_FPoint mouse,
                       float wrapWidth,
                       bool chinese) {
    if (body.empty()) {
        return;
    }
    // MouseControl::MeasureTooltip4d68f0 / RenderTooltip4d6a30.
    if (wrapWidth <= 0) {
        wrapWidth = title.empty() ? 275.f : 350.f;
    }
    auto& heading = sourceFont(13, title, chinese);
    auto& content = sourceFont(12, body, chinese);
    const float headingScale = sourceFontScale(13, chinese);
    const float contentScale = sourceFontScale(12, chinese);
    const auto headings = title.empty()
                              ? std::vector<std::string>{}
                              : wrappedLines(heading, title, wrapWidth, headingScale, chinese);
    const auto lines = wrappedLines(content, body, wrapWidth, contentScale, chinese);
    float width = 0;
    for (const auto& line : headings) {
        width = std::max(width, measure(heading, line, headingScale));
    }
    for (const auto& line : lines) {
        width = std::max(width, measure(content, line, contentScale));
    }
    width = float(int(width) + 27);
    const float titleHeight = sourceLineHeight(13, heading) * float(headings.size());
    const float height =
        float(int(titleHeight + sourceLineHeight(12, content) * float(lines.size())) +
              (title.empty() ? 18 : 23));
    float x = float(int(mouse.x));
    float y = float(int(mouse.y) + 22);
    if (mouse.y + height > 670) {
        x += 16;
        y = std::trunc(y - (mouse.y + height - 670));
        if (y + height > mouse.y) {
            y = std::trunc(y - (y + height - mouse.y + 10));
        }
    }
    if (mouse.x + width > 1260) {
        x = std::trunc(x - (mouse.x + width - 1240));
    }
    fill({x, y, width, height}, {0, 0, 0, 242});
    for (const Rect edge : {Rect{x, y, width, 2},
                            Rect{x, y + height - 2, width, 2},
                            Rect{x, y, 2, height},
                            Rect{x + width - 2, y, 2, height}}) {
        fill(edge, {255, 255, 255, 255});
    }
    float penY = y + 13;
    for (const auto& line : headings) {
        text(heading, line, x + 15, penY, headingScale, {255, 255, 255, 255});
        penY += sourceLineHeight(13, heading);
    }
    penY = y + (title.empty() ? 11.f : 16.f) + titleHeight;
    for (const auto& line : lines) {
        text(content, line, x + 15, penY, contentScale, {255, 255, 255, 255});
        penY += sourceLineHeight(12, content);
    }
}

void Renderer::achievement(const AchievementDrawing& value,
                           SDL_FPoint position,
                           int style,
                           const std::string& newLabel,
                           bool chinese) {
    // CAchievement429fe0; style branch checked in disassembly42a210.
    if (style <= 1) {
        draw(value.image, position.x, position.y);
        if (!value.unlocked) {
            fill({position.x, position.y, float(value.dimension), float(value.dimension)},
                 {0, 0, 0, 191});
            draw("img/customizeUI/box_lock_on.png",
                 position.x + value.dimension / 2 - 8,
                 position.y + value.dimension / 2 - 11);
        }
    } else {
        draw(value.unlocked ? value.miniOn : value.miniOff, position.x, position.y);
    }
    if (value.fresh && !newLabel.empty()) {
        text(sourceFont(12, newLabel, chinese),
             newLabel,
             position.x + value.dimension / 2,
             position.y + (value.dimension == 32 ? 17 : 47),
             sourceFontScale(12, chinese),
             {255, 255, 50, 255},
             true);
    }
    if (value.victory && value.unlocked) {
        for (int layout = 0; layout < (value.twoLayouts ? 2 : 3); ++layout) {
            draw(value.layouts[layout] >= 0 ? "img/achievements/S_V_dot_on.png"
                                            : "img/achievements/S_V_dot_off.png",
                 position.x + (value.twoLayouts ? 6 : 1) + 10 * layout,
                 position.y + 24);
        }
    }
    if ((style == 0 || style == 1) && value.dimension == 32) {
        return;
    }
    if (style > 2) {
        return;
    }
    const float dimension = style == 2 ? 32.f : float(value.dimension);
    const SDL_Color color =
        style == 0 ? SDL_Color{200, 200, 200, 255} : SDL_Color{255, 230, 94, 255};
    for (const Rect edge : {Rect{position.x, position.y, dimension, 2},
                            Rect{position.x, position.y + dimension - 2, dimension, 2},
                            Rect{position.x, position.y, 2, dimension},
                            Rect{position.x + dimension - 2, position.y, 2, dimension}}) {
        fill(edge, color);
    }
}

int Renderer::systemInfoCenterCount(int system,
                                    int maximum,
                                    const std::function<std::string(int)>& levelDescription,
                                    bool chinese) {
    // InfoBox::CalcCenterCountForSystem546de0 measures only displayed levels.
    int widest = 0;
    const int shown = std::min(8, maximum + (system == 7 || system == 8 ? 1 : 0));
    for (int level = 0; level < shown; ++level) {
        const auto value = levelDescription(level);
        auto& selected = sourceFont(10, value, chinese);
        for (const auto& line :
             wrappedLines(selected, value, 9999, sourceFontScale(10, chinese), chinese)) {
            widest = std::max(widest, int(measure(selected, line, sourceFontScale(10, chinese))));
        }
    }
    const auto& left = image("img/upgradeUI/details_base_A.png");
    const auto& center = image("img/upgradeUI/details_base_B.png");
    return std::max(1, (widest + 143 - left.w + center.w - 1) / center.w);
}

void Renderer::windowFrame(Rect outer, bool background, const std::vector<Rect>& cutouts) {
    // WindowFrame 0x578c90/0x579af0: tiled pattern through a nine-slice mask,
    // followed by the eight border slices. Cache the native RGBA composite.
    const int w = int(outer.w);
    const int h = int(outer.h);
    if (w < 66 || h < 72 || w > 4096 || h > 4096) {
        throw std::runtime_error("invalid_window_frame_size");
    }
    for (const auto* path :
         {"img/window_base.png", "img/window_mask.png", "img/window_outline.png"}) {
        auto& artwork = image(path);
        if (artwork.missingGraphic) {
            draw(artwork, {outer.x, outer.y, 32, 32});
            return;
        }
    }
    auto key = "@window:" + imageLanguage_ + ':' + std::to_string(w) + ":" + std::to_string(h) +
               (background ? ":filled" : ":outline");
    for (const auto& cutout : cutouts) {
        for (float value : {cutout.x, cutout.y, cutout.w, cutout.h}) {
            key += ':';
            key += std::to_string(value);
        }
    }
    if (!textures_.contains(key)) {
        auto load = [&](const char* name) {
            const auto& artwork = image(name);
            if (artwork.w != 66 || artwork.h != 72) {
                throw std::runtime_error("invalid_window_frame_asset");
            }
            return artwork.pixels;
        };
        auto base = load("img/window_base.png");
        auto mask = load("img/window_mask.png");
        auto outline = load("img/window_outline.png");
        Bytes rgba(std::size_t(w) * h * 4, 0);
        auto slice = [](int p, int extent, int corner) {
            return p < corner ? p
                   : p >= extent - corner
                       ? 2 * corner + 2 - (extent - p)
                       : corner + std::min(1, (p - corner) * 2 / (extent - 2 * corner));
        };
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int sx = slice(x, w, 32);
                int sy = slice(y, h, 35);
                int si = (sy * 66 + sx) * 4;
                int di = (y * w + x) * 4;
                if (std::any_of(cutouts.begin(), cutouts.end(), [&](Rect rect) {
                        return rect.contains(x + .5f, y + .5f);
                    })) {
                    continue;
                }
                if (background && x >= 7 && y >= 11 && mask[si + 3] >= 128) {
                    int pi = ((28 + (y - 11) % 17) * 66 + 27 + (x - 7) % 10) * 4;
                    std::copy_n(base.data() + pi, 4, rgba.data() + di);
                }
                if (x < 32 || x >= w - 32 || y < 35 || y >= h - 35) {
                    int a = outline[si + 3];
                    int ba = rgba[di + 3];
                    int oa = a + (ba * (255 - a) + 127) / 255;
                    for (int c = 0; c < 3; ++c) {
                        rgba[di + c] =
                            oa ? Uint8((outline[si + c] * a + rgba[di + c] * ba * (255 - a) / 255) /
                                       oa)
                               : 0;
                    }
                    rgba[di + 3] = Uint8(oa);
                }
            }
        }
        textures_.emplace(key, texture(rgba.data(), w, h));
    }
    draw(*textures_.at(key), outer);
}

void Renderer::clear() {
    check(SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255));
    check(SDL_RenderClear(renderer_));
}

void Renderer::reactorWire(float x, float y, int capacity, int available) {
    // SystemControl::RenderPowerBar 0x4bb6a0: two passes outside the translated stencil.
    if (capacity < 0 || capacity > 30 || available < 0 || available > capacity) {
        throw std::runtime_error("invalid_reactor_power");
    }
    for (const auto* path : {"img/wireUI/wire_full.png", "img/wire_left_mask.png"}) {
        auto& artwork = image(path);
        if (artwork.missingGraphic) {
            draw(artwork, {x + 33, y + 15, 32, 32});
            return;
        }
    }
    const auto key = "@reactor:" + imageLanguage_ + ':' + std::to_string(capacity) + ":" +
                     std::to_string(available);
    if (!textures_.contains(key)) {
        auto load = [&](const char* name, int ew, int eh) {
            const auto& artwork = image(name);
            if (artwork.w != ew || artwork.h != eh) {
                throw std::runtime_error("invalid_wire_asset");
            }
            return artwork.pixels;
        };
        auto rgba = load("img/wireUI/wire_full.png", 56, 340);
        auto mask = load("img/wire_left_mask.png", 24, 280);
        auto blocked = [&](int px, int py, int shift) {
            int mx = px - 3;
            int my = py - 34 - shift;
            return mx >= 0 && mx < 24 && my >= 0 && my < 280 && mask[(my * 24 + mx) * 4 + 3] >= 128;
        };
        for (int py = 0; py < 340; ++py) {
            for (int px = 0; px < 56; ++px) {
                int i = (py * 56 + px) * 4 + 3;
                int a = rgba[i];
                const bool capacityBlocked = blocked(px, py, 1 - 9 * capacity);
                int first = capacityBlocked ? 0 : a / 2;
                // RenderPowerBar retains the capacity stencil when adding the
                // available-power mask; both must clip the second wire pass.
                int second =
                    available && !capacityBlocked && !blocked(px, py, -9 * available) ? a : 0;
                rgba[i] = Uint8(second + first * (255 - second) / 255);
            }
        }
        textures_.emplace(key, texture(rgba.data(), 56, 340));
    }
    draw(*textures_.at(key), {x + 33, y + 15, 56, 340});
}

Rect Renderer::transformed(Rect rect) const {
    return {pivot_.x + (rect.x - pivot_.x) * scale_,
            pivot_.y + (rect.y - pivot_.y) * scale_,
            rect.w * scale_,
            rect.h * scale_};
}

void Renderer::maskedImage(const std::string& name,
                           Rect destination,
                           const std::string& maskName,
                           SDL_FPoint maskPosition,
                           float alpha,
                           SDL_Color color) {
    auto& source = image(name);
    auto& mask = image(maskName);
    if (source.missingGraphic) {
        draw(source, destination);
        return;
    }
    if (mask.missingGraphic) {
        draw(mask, {maskPosition.x, maskPosition.y, float(mask.w), float(mask.h)});
        return;
    }
    // Cache the static image/mask composition; opacity remains a draw-time value.
    const auto key = "masked:" + imageLanguage_ + ':' + name + ":" + maskName + ":" +
                     std::to_string(destination.x - maskPosition.x) + ":" +
                     std::to_string(destination.y - maskPosition.y) + ":" +
                     std::to_string(destination.w) + ":" + std::to_string(destination.h);
    if (!textures_.contains(key)) {
        std::vector<unsigned char> pixels(std::size_t(mask.w) * mask.h * 4, 0);
        for (int y = 0; y < mask.h; ++y) {
            for (int x = 0; x < mask.w; ++x) {
                const float localX = x + maskPosition.x + .5f - destination.x;
                const float localY = y + maskPosition.y + .5f - destination.y;
                if (localX < 0 || localY < 0 || localX >= destination.w ||
                    localY >= destination.h) {
                    continue;
                }
                const int sx = std::min(source.w - 1, int(localX * source.w / destination.w));
                const int sy = std::min(source.h - 1, int(localY * source.h / destination.h));
                const auto from = (std::size_t(sy) * source.w + sx) * 4;
                const auto to = (std::size_t(y) * mask.w + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    pixels[to + channel] = source.pixels[from + channel];
                }
                pixels[to + 3] = mask.pixels[to + 3] ? source.pixels[from + 3] : 0;
            }
        }
        textures_.emplace(key, texture(pixels.data(), mask.w, mask.h));
    }
    auto& composed = *textures_.at(key);
    if (color.r == 255 && color.g == 255 && color.b == 255) {
        draw(composed,
             {maskPosition.x, maskPosition.y, float(composed.w), float(composed.h)},
             nullptr,
             1,
             alpha);
    } else {
        color.a = Uint8(float(color.a) * std::clamp(alpha, 0.f, 1.f));
        sprite(composed,
               {maskPosition.x, maskPosition.y, float(composed.w), float(composed.h)},
               {0, 0, float(composed.w), float(composed.h)},
               0,
               color);
    }
}

void Renderer::imageExcluding(const std::string& name,
                              Rect destination,
                              Rect exclusion,
                              int inset,
                              float alpha,
                              SDL_Color color) {
    // RenderPullout5412f0 excludes the TextButton's solid octagonal face:
    // STENCIL_SET ref1 followed by STENCIL_USE ref0/mask1 (CSurface591050).
    if (destination.w <= 0 || destination.h <= 0) {
        return;
    }
    auto& source = image(name);
    if (source.missingGraphic) {
        draw(source, destination);
        return;
    }
    color.a = Uint8(float(color.a) * std::clamp(alpha, 0.f, 1.f));
    const float right = destination.x + destination.w;
    const float bottom = destination.y + destination.h;
    auto strip = [&](float x, float y, float width, float height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        sprite(source,
               {x, y, width, height},
               {(x - destination.x) * source.w / destination.w,
                (y - destination.y) * source.h / destination.h,
                width * source.w / destination.w,
                height * source.h / destination.h},
               0,
               color);
    };
    for (int row = int(std::floor(destination.y)); row < int(std::ceil(bottom)); ++row) {
        const float top = std::max(float(row), destination.y);
        const float height = std::min(float(row + 1), bottom) - top;
        const float middle = top + height / 2;
        if (middle < exclusion.y || middle >= exclusion.y + exclusion.h || exclusion.w <= 0) {
            strip(destination.x, top, destination.w, height);
            continue;
        }
        const int faceRow = int(std::floor(middle - exclusion.y));
        const float corner =
            float(std::max(0, inset - std::min(faceRow, int(exclusion.h) - faceRow - 1)));
        const float leftEdge = std::clamp(exclusion.x + corner, destination.x, right);
        const float rightEdge =
            std::clamp(exclusion.x + exclusion.w - corner, destination.x, right);
        strip(destination.x, top, leftEdge - destination.x, height);
        strip(rightEdge, top, right - rightEdge, height);
    }
}

void Renderer::draw(Texture& t, Rect d, const Rect* s, float tint, float alpha, bool linear) {
    d = transformed(d);
    if (t.missingGraphic) {
        drawMissingGraphic(d);
        return;
    }
    if (linear && surface_) {
        filtered(t, d, s ? *s : Rect{0, 0, float(t.w), float(t.h)}, tint, alpha);
        return;
    }
    check(SDL_SetTextureScaleMode(t.value, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST));
    auto c = Uint8(std::clamp(tint * tint_, 0.f, 1.f) * 255);
    check(SDL_SetTextureColorMod(t.value, c, c, c));
    check(SDL_SetTextureAlphaMod(t.value, Uint8(std::clamp(alpha, 0.f, 1.f) * 255)));
    SDL_FRect dst{d.x, d.y, d.w, d.h};
    SDL_FRect src{};
    if (s) {
        src = {s->x, s->y, s->w, s->h};
    }
    check(SDL_RenderTexture(renderer_, t.value, s ? &src : nullptr, &dst));
}

void Renderer::filtered(Texture& t, Rect d, Rect s, float tint, float alpha) {
    if (t.missingGraphic) {
        drawMissingGraphic(d);
        return;
    }
    // SDL's software copy truncates destination rectangles to integers. Preserve
    // GL pixel-center sampling for Ship::OnRenderJump's fractional transform.
    if (d.w <= 0 || d.h <= 0) {
        return;
    }
    check(SDL_FlushRenderer(renderer_));
    SDL_Rect clip{0, 0, surface_->w, surface_->h};
    if (SDL_RenderClipEnabled(renderer_)) {
        check(SDL_GetRenderClipRect(renderer_, &clip));
    }
    check(SDL_LockSurface(surface_));
    const int left = std::max(std::clamp(int(std::ceil(d.x - .5f)), 0, surface_->w), clip.x);
    const int right =
        std::min(std::clamp(int(std::ceil(d.x + d.w - .5f)), 0, surface_->w), clip.x + clip.w);
    const int top = std::max(std::clamp(int(std::ceil(d.y - .5f)), 0, surface_->h), clip.y);
    const int bottom =
        std::min(std::clamp(int(std::ceil(d.y + d.h - .5f)), 0, surface_->h), clip.y + clip.h);
    for (int y = top; y < bottom; ++y) {
        const float v = s.y + (y + .5f - d.y) * s.h / d.h - .5f;
        auto* row = static_cast<unsigned char*>(surface_->pixels) + y * surface_->pitch;
        for (int x = left; x < right; ++x) {
            const float u = s.x + (x + .5f - d.x) * s.w / d.w - .5f;
            auto channel = [&](int c) {
                return sample(t, u, v, c);
            };
            const float opacity = channel(3) / 255 * std::clamp(alpha, 0.f, 1.f);
            for (int c = 0; c < 3; ++c) {
                row[x * 4 + c] = static_cast<unsigned char>(
                    std::clamp(std::round(std::lerp(
                                   float(row[x * 4 + c]), channel(c) * tint * tint_, opacity)),
                               0.f,
                               255.f));
            }
        }
    }
    SDL_UnlockSurface(surface_);
}

void Renderer::draw(const std::string& n, float x, float y, float tint, float alpha, bool linear) {
    auto& t = image(n);
    draw(t, {x, y, float(t.w), float(t.h)}, nullptr, tint, alpha, linear);
}

void Renderer::fill(Rect r, SDL_Color c) {
    r = transformed(r);
    check(SDL_SetRenderDrawColor(
        renderer_, Uint8(c.r * tint_), Uint8(c.g * tint_), Uint8(c.b * tint_), c.a));
    SDL_FRect f{r.x, r.y, r.w, r.h};
    check(SDL_RenderFillRect(renderer_, &f));
}

void Renderer::texturedLine(Texture& texture, SDL_FPoint from, SDL_FPoint to, SDL_Color color) {
    // StarMap::DrawConnection0x5cbe10: four-pixel strip, ten-pixel repeats,
    // bilinear texels and a continuous 1 -> .3 vertex-alpha gradient.
    from = {pivot_.x + (from.x - pivot_.x) * scale_, pivot_.y + (from.y - pivot_.y) * scale_};
    to = {pivot_.x + (to.x - pivot_.x) * scale_, pivot_.y + (to.y - pivot_.y) * scale_};
    const float transformScale = std::abs(scale_);
    if (texture.missingGraphic) {
        drawMissingGraphic({from.x, from.y, 32 * transformScale, 32 * transformScale});
        return;
    }
    const float length = std::hypot(to.x - from.x, to.y - from.y);
    if (length <= 0 || texture.w < 3 || texture.h < 1) {
        return;
    }
    const float dx = (to.x - from.x) / length;
    const float dy = (to.y - from.y) / length;
    const float half = texture.h / 2.f * transformScale;
    const float repeat = float(texture.w - 2) * transformScale;
    if (!surface_) {
        check(SDL_SetTextureScaleMode(texture.value, SDL_SCALEMODE_LINEAR));
        check(SDL_SetTextureColorMod(texture.value, 255, 255, 255));
        check(SDL_SetTextureAlphaMod(texture.value, 255));
        const int segments = int(std::ceil(length / repeat));
        for (int segment = 0; segment < segments; ++segment) {
            const float start = segment * repeat;
            const float end = std::min(length, start + repeat);
            auto vertex = [&](float along, float across, float u, float v) {
                return SDL_Vertex{
                    {from.x + dx * along - dy * across, from.y + dy * along + dx * across},
                    {color.r / 255.f * tint_,
                     color.g / 255.f * tint_,
                     color.b / 255.f * tint_,
                     color.a / 255.f * (1 - .7f * along / length)},
                    {u, v}};
            };
            const SDL_Vertex vertices[] = {vertex(start, -half, 1.f / texture.w, 0),
                                           vertex(end, -half, (texture.w - 1.f) / texture.w, 0),
                                           vertex(end, half, (texture.w - 1.f) / texture.w, 1),
                                           vertex(start, half, 1.f / texture.w, 1)};
            const int indices[] = {0, 1, 2, 0, 2, 3};
            check(SDL_RenderGeometry(renderer_, texture.value, vertices, 4, indices, 6));
        }
        return;
    }
    check(SDL_FlushRenderer(renderer_));
    SDL_Rect clip{0, 0, surface_->w, surface_->h};
    if (SDL_RenderClipEnabled(renderer_)) {
        check(SDL_GetRenderClipRect(renderer_, &clip));
    }
    check(SDL_LockSurface(surface_));
    const int left = std::max(
        std::clamp(int(std::floor(std::min(from.x, to.x) - half)), 0, surface_->w), clip.x);
    const int right = std::min(
        std::clamp(int(std::ceil(std::max(from.x, to.x) + half)), 0, surface_->w), clip.x + clip.w);
    const int top = std::max(
        std::clamp(int(std::floor(std::min(from.y, to.y) - half)), 0, surface_->h), clip.y);
    const int bottom = std::min(
        std::clamp(int(std::ceil(std::max(from.y, to.y) + half)), 0, surface_->h), clip.y + clip.h);
    for (int y = top; y < bottom; ++y) {
        auto* row = static_cast<unsigned char*>(surface_->pixels) + y * surface_->pitch;
        for (int x = left; x < right; ++x) {
            const float rx = x + .5f - from.x;
            const float ry = y + .5f - from.y;
            const float along = rx * dx + ry * dy;
            const float across = -rx * dy + ry * dx;
            if (along < 0 || along >= length || across < -half || across >= half) {
                continue;
            }
            const float start = std::floor(along / repeat) * repeat;
            const float u =
                .5f + float(texture.w - 2) * (along - start) / std::min(repeat, length - start);
            const float v = across / transformScale + texture.h / 2.f - .5f;
            const float opacity =
                sample(texture, u, v, 3) / 255 * (color.a / 255.f) * (1 - .7f * along / length);
            const float colors[] = {float(color.r), float(color.g), float(color.b)};
            for (int c = 0; c < 3; ++c) {
                const float value = sample(texture, u, v, c) * colors[c] / 255 * tint_;
                row[x * 4 + c] = static_cast<unsigned char>(std::clamp(
                    std::round(std::lerp(float(row[x * 4 + c]), value, opacity)), 0.f, 255.f));
            }
        }
    }
    SDL_UnlockSurface(surface_);
}

void Renderer::polygon(const std::vector<SDL_FPoint>& points, SDL_Color color) {
    if (points.size() < 3) {
        return;
    }
    std::vector<SDL_Vertex> vs;
    vs.reserve(points.size());
    for (auto p : points) {
        const auto position = transformed({p.x, p.y, 0, 0});
        p = {position.x, position.y};
        vs.push_back({p,
                      {color.r / 255.f * tint_,
                       color.g / 255.f * tint_,
                       color.b / 255.f * tint_,
                       color.a / 255.f},
                      {0, 0}});
    }
    std::vector<int> is;
    is.reserve((points.size() - 2) * 3);
    for (int i = 1; i < int(points.size()) - 1; ++i) {
        is.push_back(0);
        is.push_back(i);
        is.push_back(i + 1);
    }
    check(SDL_RenderGeometry(
        renderer_, nullptr, vs.data(), int(vs.size()), is.data(), int(is.size())));
}

void Renderer::texturedPie(const std::string& name,
                           SDL_FPoint origin,
                           SDL_FPoint center,
                           float radius,
                           float firstDegrees,
                           float lastDegrees) {
    // Draws image `name` at `origin` only inside the stencil pie that
    // CSurface::GL_CreatePiePartialPrimitive 0x596730 builds: one rim vertex
    // every ten degrees from firstDegrees while still below lastDegrees, at
    // most 37, fanned around `center` (relative to origin).
    auto& texture = image(name);
    if (texture.missingGraphic || !texture.value || texture.w <= 0 || texture.h <= 0 ||
        lastDegrees <= firstDegrees) {
        return;
    }
    std::vector<SDL_FPoint> rim;
    for (float degrees = firstDegrees; rim.size() < 37;) {
        const float radians = 3.1415927f * degrees / 180.f;
        rim.push_back(
            {center.x + std::cos(radians) * radius, center.y + std::sin(radians) * radius});
        degrees += 10.f;
        if (lastDegrees <= degrees) {
            break;
        }
    }
    if (rim.size() < 2) {
        return;
    }
    const auto vertex = [&](SDL_FPoint local) {
        const auto position = transformed({origin.x + local.x, origin.y + local.y, 0, 0});
        return SDL_Vertex{{position.x, position.y},
                          {tint_, tint_, tint_, 1.f},
                          {local.x / float(texture.w), local.y / float(texture.h)}};
    };
    std::vector<SDL_Vertex> vertices{vertex(center)};
    std::vector<int> indices;
    for (std::size_t index = 0; index < rim.size(); ++index) {
        vertices.push_back(vertex(rim[index]));
        if (index > 0) {
            indices.insert(indices.end(), {0, int(index), int(index) + 1});
        }
    }
    check(SDL_SetTextureScaleMode(texture.value, SDL_SCALEMODE_NEAREST));
    check(SDL_SetTextureColorMod(texture.value, 255, 255, 255));
    check(SDL_SetTextureAlphaMod(texture.value, 255));
    check(SDL_RenderGeometry(renderer_,
                             texture.value,
                             vertices.data(),
                             int(vertices.size()),
                             indices.data(),
                             int(indices.size())));
}

void Renderer::laser(SDL_FPoint from,
                     SDL_FPoint to,
                     int thickness,
                     SDL_Color color,
                     std::optional<Rect> ellipse,
                     bool outside) {
    // CSurface::GL_DrawLaser0x5998e0 draws two quads, opaque at the center
    // and transparent at both edges. Original width and height are integers.
    const float distance = std::hypot(to.x - from.x, to.y - from.y);
    const int width = int(distance);
    const int length = width / 2 * 2;
    if (length <= 0 || thickness <= 0) {
        return;
    }
    const SDL_FPoint direction{(to.x - from.x) / distance, (to.y - from.y) / distance};
    const float upper = float((1 - thickness) >> 1);
    const float lower = float(thickness / 2);
    const SDL_FPoint start{float(int(from.x)), float(int(from.y))};
    const auto point = [&](float along, float across) {
        return SDL_FPoint{start.x + direction.x * along - direction.y * across,
                          start.y + direction.y * along + direction.x * across};
    };
    if (!ellipse) {
        const auto vertex = [&](float along, float across, float alpha) {
            const auto p = point(along, across);
            const auto position = transformed({p.x, p.y, 0, 0});
            return SDL_Vertex{{position.x, position.y},
                              {color.r / 255.f * tint_,
                               color.g / 255.f * tint_,
                               color.b / 255.f * tint_,
                               color.a / 255.f * alpha},
                              {0, 0}};
        };
        const SDL_Vertex vertices[]{vertex(0, upper, 0),
                                    vertex(float(length), upper, 0),
                                    vertex(float(length), 0, 1),
                                    vertex(0, 0, 1),
                                    vertex(0, lower, 0),
                                    vertex(float(length), lower, 0)};
        const int indices[]{0, 1, 2, 0, 2, 3, 3, 2, 5, 3, 5, 4};
        check(SDL_RenderGeometry(renderer_, nullptr, vertices, 6, indices, 12));
        return;
    }
    // BeamWeapon434fa0 uses a binary ellipse stencil for the unreduced
    // exterior part and the reduced interior part. Rasterize the same mask
    // without modifying the scene or sampling gameplay state.
    const std::array<SDL_FPoint, 4> corners{
        point(0, upper), point(float(length), upper), point(float(length), lower), point(0, lower)};
    float left = corners[0].x;
    float right = left;
    float top = corners[0].y;
    float bottom = top;
    for (const auto& p : corners) {
        left = std::min(left, p.x);
        right = std::max(right, p.x);
        top = std::min(top, p.y);
        bottom = std::max(bottom, p.y);
    }
    const int x0 = int(std::floor(left));
    const int y0 = int(std::floor(top));
    const int imageWidth = std::max(1, int(std::ceil(right)) - x0);
    const int imageHeight = std::max(1, int(std::ceil(bottom)) - y0);
    Texture image;
    image.w = imageWidth;
    image.h = imageHeight;
    image.pixels.assign(std::size_t(imageWidth) * std::size_t(imageHeight) * 4, 0);
    const float rx = ellipse->w / 2.f;
    const float ry = ellipse->h / 2.f;
    for (int row = 0; row < imageHeight; ++row) {
        for (int column = 0; column < imageWidth; ++column) {
            const float x = float(x0 + column) + .5f;
            const float y = float(y0 + row) + .5f;
            const float dx = x - start.x;
            const float dy = y - start.y;
            const float along = dx * direction.x + dy * direction.y;
            const float across = -dx * direction.y + dy * direction.x;
            if (along < 0 || along >= length || across < upper || across >= lower) {
                continue;
            }
            const float ex = rx > 0 ? (x - ellipse->x - rx) / rx : 2.f;
            const float ey = ry > 0 ? (y - ellipse->y - ry) / ry : 2.f;
            const bool inside = ex * ex + ey * ey <= 1.f;
            if (inside == outside) {
                continue;
            }
            const float alpha = across < 0 ? 1.f - across / upper : 1.f - across / lower;
            auto* pixel = image.pixels.data() +
                          (std::size_t(row) * std::size_t(imageWidth) + std::size_t(column)) * 4;
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            pixel[3] = Uint8(std::clamp(alpha * color.a, 0.f, 255.f));
        }
    }
    image.value = SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, imageWidth, imageHeight);
    check(image.value != nullptr);
    check(SDL_UpdateTexture(image.value, nullptr, image.pixels.data(), imageWidth * 4));
    check(SDL_SetTextureBlendMode(image.value, SDL_BLENDMODE_BLEND));
    draw(image, {float(x0), float(y0), float(imageWidth), float(imageHeight)});
}

void Renderer::shieldArc(
    SDL_FPoint center, SDL_FPoint radii, int startDegrees, int endDegrees, float thickness) {
    if (endDegrees <= startDegrees || radii.x <= 0 || radii.y <= 0 || thickness <= 0) {
        return;
    }
    const int count = endDegrees - startDegrees;
    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(std::size_t(count) * 2);
    indices.reserve(std::size_t(count - 1) * 6);
    constexpr float radians = 3.14159265358979323846f / 180.f;
    const float innerX = std::max(0.f, radii.x - thickness);
    const float innerY = radii.y * innerX / radii.x;
    for (int index = 0; index < count; ++index) {
        const float angle = float(startDegrees + index) * radians;
        const float phase = 2.f * float(index) / float(count);
        const float fade = std::min(phase, 2.f - phase);
        const float alpha = fade * fade;
        for (int edge = 0; edge < 2; ++edge) {
            const float rx = edge == 0 ? radii.x : innerX;
            const float ry = edge == 0 ? radii.y : innerY;
            const auto point =
                transformed({center.x + std::cos(angle + (edge == 0 ? 0.f : .001f)) * rx,
                             center.y + std::sin(angle + (edge == 0 ? 0.f : .001f)) * ry,
                             0,
                             0});
            vertices.push_back(
                {{point.x, point.y}, {tint_, tint_, tint_, edge == 0 ? alpha : 0.f}, {0, 0}});
        }
        if (index > 0) {
            const int previous = (index - 1) * 2;
            const int current = index * 2;
            indices.insert(indices.end(),
                           {previous, previous + 1, current, previous + 1, current + 1, current});
        }
    }
    check(SDL_RenderGeometry(renderer_,
                             nullptr,
                             vertices.data(),
                             static_cast<int>(vertices.size()),
                             indices.data(),
                             static_cast<int>(indices.size())));
}

float Renderer::inkWidth(Font& font, const std::string& value, float scale) {
    // font_text_width622490 uses backend ink bounds, not text advance.
    const auto [left, right] = bounds(font, value, scale);
    return right - left;
}

float Renderer::measure(Font& f, const std::string& text, float scale) {
    float width = 0;
    for (auto cp : codepoints(text)) {
        if (auto it = f.glyphs.find(cp); it != f.glyphs.end()) {
            const auto& g = it->second;
            width = std::round(width + g.pre * scale) + (g.w + g.post) * scale;
        }
    }
    return std::round(width);
}

std::vector<std::string> Renderer::wrappedLines(
    Font& font, const std::string& text, float width, float scale, bool chinese) {
    // freetype::formatLines0x57a740. Punctuation tables are the original
    // uint32 arrays at0x8328c0/0x832920, retained in P11-text-wrap-original.json.
    constexpr std::array<std::uint32_t, 24> noEnd{
        36,    40,    163,   165,   183,   8216,  8220,  12296, 12298, 12300, 12302, 12304,
        12308, 12310, 12317, 65113, 65115, 65284, 65288, 65294, 65339, 65371, 65505, 65509};
    constexpr std::array<std::uint32_t, 43> noBegin{
        33,    37,    41,    44,    46,    58,    59,    63,    93,    125,   162,
        176,   183,   8217,  8221,  8224,  8225,  8250,  8451,  8758,  12289, 12290,
        12291, 12294, 12309, 12311, 12318, 65114, 65116, 65281, 65282, 65285, 65287,
        65289, 65292, 65294, 65306, 65307, 65311, 65281, 65341, 65373, 65374};
    const auto advance = [&](std::uint32_t cp) {
        const auto found = font.glyphs.find(cp);
        if (found == font.glyphs.end()) {
            return 0.f;
        }
        const auto& glyph = found->second;
        return std::round((glyph.pre + glyph.w + glyph.post) * scale);
    };
    const float spaceWidth = advance(32);
    const float hyphenWidth = advance(45);
    std::vector<std::string> lines;
    std::size_t cursor = 0;
    std::size_t start = 0;
    std::size_t previousEnd = 0;
    std::optional<std::size_t> breakAt;
    std::uint32_t previous = 0;
    float lineWidth = 0;
    float breakWidth = 0;
    bool softHyphen = false;
    for (;;) {
        std::uint32_t cp = 0;
        if (cursor < text.size()) {
            const auto first = static_cast<unsigned char>(text[cursor++]);
            cp = first;
            const int extra = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc0 ? 1 : 0;
            if (extra != 0) {
                cp &= extra == 3 ? 7 : extra == 2 ? 15 : 31;
                for (int index = 0; index < extra && cursor < text.size(); ++index) {
                    cp = (cp << 6) | (static_cast<unsigned char>(text[cursor++]) & 63);
                }
            }
        }
        if (cp == 10 || cp == 0) {
            lines.push_back(text.substr(start, previousEnd - start));
            if (cp == 0) {
                break;
            }
            start = previousEnd = cursor;
            previous = 0;
            lineWidth = breakWidth = 0;
            breakAt.reset();
            softHyphen = false;
            continue;
        }
        if (cp == 1) {
            if (width > 0 && lineWidth + hyphenWidth <= width) {
                breakAt = cursor;
                breakWidth = lineWidth;
                softHyphen = true;
            }
            previous = cp;
            previousEnd = cursor;
            continue;
        }
        if (width > 0) {
            bool canBreak = previous != 0 && previous != 32 && cp == 32;
            if (chinese && previous != 0) {
                canBreak = (previous < 33 || previous > 126 || cp < 33 || cp > 126) &&
                           std::find(noEnd.begin(), noEnd.end(), previous) == noEnd.end() &&
                           std::find(noBegin.begin(), noBegin.end(), cp) == noBegin.end();
            }
            if (canBreak) {
                breakAt = previousEnd;
                breakWidth = lineWidth;
                softHyphen = false;
            }
            float characterWidth = advance(cp);
            if (lineWidth > 0 && lineWidth + characterWidth > width) {
                if (!breakAt) {
                    breakAt = previousEnd;
                    breakWidth = lineWidth;
                }
                auto line = text.substr(start, *breakAt - start);
                if (softHyphen) {
                    line += '-';
                }
                lines.push_back(std::move(line));
                start = *breakAt;
                lineWidth -= breakWidth;
                const auto beforeSpaces = start;
                while (start < text.size() && text[start] == ' ') {
                    ++start;
                }
                if (start < cursor) {
                    lineWidth -= static_cast<float>(start - beforeSpaces) * spaceWidth;
                } else if (start > beforeSpaces) {
                    cursor = start;
                    characterWidth = 0;
                    cp = 32;
                    lineWidth = 0;
                }
                breakAt.reset();
                breakWidth = 0;
                softHyphen = false;
            }
            lineWidth += characterWidth;
        }
        previous = cp;
        previousEnd = cursor;
    }
    return lines;
}

std::pair<float, float> Renderer::bounds(Font& f, const std::string& text, float scale) {
    // font_bitmap_get_text_bounds 0x621190.
    float pen = 0;
    float left = 0;
    float right = 0;
    for (auto cp : codepoints(text)) {
        if (auto it = f.glyphs.find(cp); it != f.glyphs.end()) {
            const auto& g = it->second;
            float start = pen + g.pre * scale;
            left = std::min(left, std::round(start));
            right = std::max(right, std::round(start + g.w * scale));
            pen = std::round(start) + (g.w + g.post) * scale;
        }
    }
    return {left, right};
}

void Renderer::text(Font& f,
                    const std::string& text,
                    float x,
                    float y,
                    float scale,
                    SDL_Color color,
                    bool centered) {
    if (centered) {
        auto [left, right] = bounds(f, text, scale);
        x -= (left + right) / 2;
    }
    auto t = f.atlas->value;
    check(SDL_SetTextureColorMod(
        t, Uint8(color.r * tint_), Uint8(color.g * tint_), Uint8(color.b * tint_)));
    check(SDL_SetTextureAlphaMod(t, color.a));
    for (auto cp : codepoints(text)) {
        if (auto it = f.glyphs.find(cp); it != f.glyphs.end()) {
            const auto& g = it->second;
            x = std::round(x + g.pre * scale);
            SDL_FRect src{float(g.x), float(g.y), float(g.w), float(g.h)};
            const auto rect = transformed({std::round(x),
                                           std::round(y + (f.baseline - g.ascent) * scale),
                                           g.w * scale,
                                           g.h * scale});
            SDL_FRect dst{rect.x, rect.y, rect.w, rect.h};
            if (g.w && g.h) {
                check(SDL_RenderTexture(renderer_, t, &src, &dst));
            }
            x += (g.w + g.post) * scale;
        }
    }
}

void Renderer::textInput(const std::string& value,
                         std::size_t caretByte,
                         int size,
                         SDL_FPoint position,
                         bool caretVisible,
                         bool chinese) {
    auto& font = sourceFont(size, value, chinese);
    const float scale = sourceFontScale(size, chinese);
    text(font, value, position.x, position.y, scale, {255, 255, 255, 255}, true);
    if (!caretVisible) {
        return;
    }
    const auto [left, right] = bounds(font, value, scale);
    // TextInput4eeed0 uses print57b710's centered advance minus full inkwidth.
    const float originX =
        position.x + measure(font, value, scale) - (left + right) / 2.f - (right - left);
    const float prefixWidth =
        inkWidth(font, value.substr(0, std::min(caretByte, value.size())), scale);
    const float height = sourceLineHeight(size, font);
    fill({originX + prefixWidth - 2, position.y + height * .1f, 2, height * .8f},
         {255, 255, 50, 255});
}

void Renderer::textWithGlow(const std::string& value,
                            float x,
                            float y,
                            bool chinese,
                            SDL_Color color,
                            float alpha,
                            bool centered) {
    // DrawTextWithGlow0x5b25e0: source font59, integer word advances,
    // and a separate glow behind each space/NBSP-delimited word.
    auto& selected = sourceFont(59, value, chinese);
    const float scale = sourceFontScale(59, chinese);
    const int lineHeight = int(std::round(sourceLineHeight(59, selected)));
    const int spaceWidth = int(std::round(measure(selected, " ", scale)));
    const bool red = color.r == 253 && color.g == 84 && color.b == 70;
    auto& glow = image(red ? "img/warnings/backglow_warning_red.png"
                           : "img/warnings/backglow_warning_white.png");
    const auto glowAlpha = Uint8(std::clamp(alpha, 0.f, 1.f) * 255);
    color.a = Uint8(std::clamp(alpha, 0.f, 1.f) * color.a);
    int penY = int(y);
    int penX = int(x);
    std::size_t cursor = 0;
    bool lineStart = true;
    while (cursor < value.size()) {
        if (lineStart) {
            penX = int(x);
            if (centered) {
                const auto end = value.find('\n', cursor);
                const auto line =
                    value.substr(cursor, end == std::string::npos ? end : end - cursor);
                penX -= int(std::round(measure(selected, line, scale))) / 2;
            }
            lineStart = false;
        }
        if (value[cursor] == '\n') {
            ++cursor;
            penY += lineHeight;
            lineStart = true;
            continue;
        }
        if (value[cursor] == ' ' ||
            std::string_view(value).substr(cursor).starts_with("\xc2\xa0")) {
            cursor += value[cursor] == ' ' ? 1 : 2;
            penX += spaceWidth;
            continue;
        }
        auto end = cursor;
        while (end < value.size() && value[end] != ' ' && value[end] != '\n' &&
               !std::string_view(value).substr(end).starts_with("\xc2\xa0")) {
            ++end;
        }
        const auto word = value.substr(cursor, end - cursor);
        const int width = int(std::round(measure(selected, word, scale)));
        sprite(glow,
               {float(penX - 4), float(penY - 1), float(width + 8), float(lineHeight + 4)},
               {0, 0, float(glow.w), float(glow.h)},
               0,
               {255, 255, 255, glowAlpha});
        text(selected, word, float(penX), float(penY), scale, color);
        penX += width;
        cursor = end;
    }
}

void Renderer::setClip(std::optional<Rect> clip) {
    if (!clip) {
        check(SDL_SetRenderClipRect(renderer_, nullptr));
        return;
    }
    const auto transformedClip = transformed(*clip);
    SDL_Rect rect{int(std::floor(transformedClip.x)),
                  int(std::floor(transformedClip.y)),
                  int(std::ceil(transformedClip.w)),
                  int(std::ceil(transformedClip.h))};
    check(SDL_SetRenderClipRect(renderer_, &rect));
}

void Renderer::sprite(Texture& t,
                      Rect dst,
                      Rect src,
                      double angle,
                      SDL_Color color,
                      bool mirror,
                      const SDL_FPoint* center) {
    dst = transformed(dst);
    if (t.missingGraphic) {
        drawMissingGraphic(dst);
        return;
    }
    SDL_FPoint scaledCenter{};
    if (center) {
        scaledCenter = {center->x * scale_, center->y * scale_};
    }
    check(SDL_SetTextureColorMod(
        t.value, Uint8(color.r * tint_), Uint8(color.g * tint_), Uint8(color.b * tint_)));
    check(SDL_SetTextureAlphaMod(t.value, color.a));
    SDL_FRect source{src.x, src.y, src.w, src.h};
    SDL_FRect dest{dst.x, dst.y, dst.w, dst.h};
    check(SDL_RenderTextureRotated(renderer_,
                                   t.value,
                                   &source,
                                   &dest,
                                   angle,
                                   center ? &scaledCenter : nullptr,
                                   mirror ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE));
}

void Renderer::present() {
    if (target_) {
        check(SDL_SetRenderTarget(renderer_, nullptr));
        check(SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255));
        check(SDL_RenderClear(renderer_));
        check(SDL_RenderTexture(renderer_, target_, nullptr, nullptr));
        check(SDL_RenderPresent(renderer_));
        check(SDL_SetRenderTarget(renderer_, target_));
        return;
    }
    check(SDL_RenderPresent(renderer_));
    auto dest = SDL_GetWindowSurface(window_);
    if (!dest) {
        throw std::runtime_error(SDL_GetError());
    }
    check(SDL_FillSurfaceRect(dest, nullptr, SDL_MapSurfaceRGB(dest, 0, 0, 0)));
    float scale = std::min(dest->w / 1280.f, dest->h / 720.f);
    SDL_Rect d{int((dest->w - 1280 * scale) / 2),
               int((dest->h - 720 * scale) / 2),
               int(1280 * scale),
               int(720 * scale)};
    if (fullscreenMode_ == 1) {
        d = {0, 0, dest->w, dest->h};
    }
    check(SDL_BlitSurfaceScaled(surface_, nullptr, dest, &d, SDL_SCALEMODE_NEAREST));
    check(SDL_UpdateWindowSurface(window_));
}

void Renderer::capture(const std::filesystem::path& path) {
    check(SDL_FlushRenderer(renderer_));
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto* pixels = surface_ ? surface_ : SDL_RenderReadPixels(renderer_, nullptr);
    if (!pixels) {
        throw std::runtime_error(SDL_GetError());
    }
    const bool saved = SDL_SaveBMP(pixels, path.string().c_str());
    if (!surface_) {
        SDL_DestroySurface(pixels);
    }
    check(saved);
}

SDL_FPoint Renderer::logical(float x, float y) const {
    int w;
    int h;
    check(SDL_GetWindowSize(window_, &w, &h));
    if (fullscreenMode_ == 1) {
        return {x * 1280.f / float(w), y * 720.f / float(h)};
    }
    float scale = std::min(w / 1280.f, h / 720.f);
    return {(x - (w - 1280 * scale) / 2) / scale, (y - (h - 720 * scale) / 2) / scale};
}

void Renderer::resize(int w, int h) {
    if (w < 320 || h < 180 || w > 7680 || h > 4320) {
        throw std::runtime_error("invalid_window_size");
    }
    check(SDL_SetWindowSize(window_, w, h));
}
} // namespace ftl
