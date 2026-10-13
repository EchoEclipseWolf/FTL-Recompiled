#include "render.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include FT_ADVANCES_H
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace ftl {
namespace {
struct Face {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    FT_Stroker stroker = nullptr;

    Face() = default;
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;

    ~Face() {
        if (stroker) {
            FT_Stroker_Done(stroker);
        }
        if (face) {
            FT_Done_Face(face);
        }
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

// FreeType replaces the glyph pointer when stroking/converting. Keep ownership
// in one scope so every error path releases whichever glyph is currently live.
struct ScopedGlyph {
    FT_Glyph value = nullptr;

    ScopedGlyph() = default;
    ScopedGlyph(const ScopedGlyph&) = delete;
    ScopedGlyph& operator=(const ScopedGlyph&) = delete;

    ~ScopedGlyph() {
        if (value) {
            FT_Done_Glyph(value);
        }
    }
};

void check(FT_Error e) {
    if (e) {
        throw std::runtime_error("freetype_error: " + std::to_string(e));
    }
}
} // namespace

Font& Renderer::chineseFont(int pixels,
                            bool outline,
                            const std::string& characters,
                            int sourceSize) {
    int baselineOffset = outline ? 2 : pixels == 16 ? -2 : 0;
    int heightOverride = 0;
    int lineHeightOffset = 0;
    if (sourceSize != 0) {
        // ResourceControl::GetChineseFont0x5a25a0 maps source font IDs to
        // independent pixel sizes and baseline overrides.
        outline = sourceSize >= 62 && sourceSize <= 64;
        switch (sourceSize) {
        case 5:
            pixels = 11;
            baselineOffset = 1;
            break;
        case 9:
            pixels = 13;
            baselineOffset = 1;
            break;
        case 10:
            pixels = 14;
            baselineOffset = 0;
            break;
        case 12:
            pixels = 16;
            baselineOffset = -2;
            break;
        case 13:
            pixels = 17;
            baselineOffset = -3;
            break;
        case 14:
        case 16:
            pixels = 20;
            baselineOffset = 7;
            break;
        case 18:
            pixels = 32;
            baselineOffset = -3;
            break;
        case 20:
            pixels = 48;
            baselineOffset = -4;
            break;
        case 24:
            pixels = 27;
            baselineOffset = 13;
            break;
        case 51:
        case 61:
            pixels = 11;
            baselineOffset = 0;
            break;
        case 52:
            pixels = 18;
            baselineOffset = 0;
            break;
        case 53:
            pixels = 27;
            baselineOffset = 1;
            break;
        case 59:
            pixels = 18;
            baselineOffset = 1;
            break;
        case 62:
            pixels = 16;
            baselineOffset = 2;
            break;
        case 63:
            pixels = 24;
            baselineOffset = 2;
            break;
        case 64:
            pixels = 31;
            baselineOffset = 3;
            break;
        default:
            pixels = sourceSize;
            baselineOffset = 0;
            break;
        }
        heightOverride = int(std::round(pixels * 4.f / 3.f));
        switch (sourceSize) {
        case 5:
            lineHeightOffset = -2;
            break;
        case 10:
            lineHeightOffset = 1;
            break;
        case 12:
            lineHeightOffset = 2;
            break;
        case 13:
            lineHeightOffset = 3;
            break;
        case 14:
            lineHeightOffset = 11;
            break;
        case 16:
        case 18:
        case 51:
        case 59:
        case 61:
            lineHeightOffset = -3;
            break;
        case 20:
            lineHeightOffset = -2;
            break;
        case 24:
            lineHeightOffset = 22;
            break;
        case 52:
            lineHeightOffset = 3;
            break;
        case 53:
            lineHeightOffset = 7;
            break;
        case 62:
            lineHeightOffset = 6;
            break;
        case 63:
            lineHeightOffset = 11;
            break;
        case 64:
            lineHeightOffset = 17;
            break;
        default:
            break;
        }
    }
    auto key = "zh:" + std::to_string(pixels) + ":source:" + std::to_string(sourceSize) +
               (outline ? ":outline:" : ":plain:") + characters;
    if (auto it = fonts_.find(key); it != fonts_.end()) {
        return it->second;
    }
    // formatLines57a740 may introduce a visible hyphen at byte1 and needs
    // space advances even when the requested string contains neither glyph.
    std::set<std::uint32_t> chars{32, 45};
    for (std::size_t i = 0; i < characters.size();) {
        auto c = static_cast<unsigned char>(characters[i++]);
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
        while (n-- && i < characters.size()) {
            cp = (cp << 6) | (static_cast<unsigned char>(characters[i++]) & 63);
        }
        if (cp >= 32) {
            chars.insert(cp);
        }
    }
    auto bytes = archive_.read("fonts/zh-Hans/black.ttf");
    Face f;
    check(FT_Init_FreeType(&f.library));
    check(FT_New_Memory_Face(f.library, bytes.data(), FT_Long(bytes.size()), 0, &f.face));
    check(FT_Set_Char_Size(f.face, 0, pixels * 64, 0, 72));
    if (outline) {
        check(FT_Stroker_New(f.library, &f.stroker));
        FT_Stroker_Set(f.stroker, 128, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
    }
    int cell = pixels * 2 + 12;
    int columns = 16;
    int w = cell * columns;
    int h = cell * std::max(1, (int(chars.size()) + columns - 1) / columns);
    Bytes rgba(std::size_t(w) * h * 4, 255);
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        rgba[i] = 0;
    }
    Font out{nullptr,
             int(std::round(float(f.face->height) * pixels / f.face->units_per_EM)),
             int(std::round(float(f.face->ascender) * pixels / f.face->units_per_EM)),
             {}};
    out.baseline += baselineOffset;
    if (sourceSize != 0) {
        out.lineHeight = out.height + lineHeightOffset;
        out.height = heightOverride;
    }
    int idx = 0;
    for (auto cp : chars) {
        const auto glyph = FT_Get_Char_Index(f.face, cp);
        FT_Fixed advance = 0;
        check(FT_Get_Advance(f.face, glyph, FT_LOAD_NO_HINTING, &advance));
        check(FT_Load_Glyph(f.face, glyph, FT_LOAD_NO_BITMAP));
        ScopedGlyph stroked;
        int left = 0;
        int top = 0;
        int gw = 0;
        int gh = 0;
        if (outline) {
            check(FT_Get_Glyph(f.face->glyph, &stroked.value));
            check(FT_Glyph_StrokeBorder(&stroked.value, f.stroker, false, true));
            check(FT_Glyph_To_Bitmap(&stroked.value, FT_RENDER_MODE_NORMAL, nullptr, true));
            auto b = reinterpret_cast<FT_BitmapGlyph>(stroked.value);
            left = b->left;
            top = b->top;
            gw = int(b->bitmap.width);
            gh = int(b->bitmap.rows);
        }
        check(FT_Render_Glyph(f.face->glyph, FT_RENDER_MODE_NORMAL));
        if (!outline) {
            left = f.face->glyph->bitmap_left;
            top = f.face->glyph->bitmap_top;
            gw = int(f.face->glyph->bitmap.width);
            gh = int(f.face->glyph->bitmap.rows);
        }
        if (gw > cell || gh > cell) {
            throw std::runtime_error("glyph_exceeds_atlas_cell");
        }
        int x = (idx % columns) * cell;
        int y = (idx / columns) * cell;
        ++idx;
        const auto& base = f.face->glyph->bitmap;
        for (int row = 0; row < gh; ++row) {
            for (int col = 0; col < gw; ++col) {
                int alpha = 0;
                if (outline) {
                    auto b = reinterpret_cast<FT_BitmapGlyph>(stroked.value);
                    alpha = b->bitmap.buffer[row * b->bitmap.pitch + col];
                }
                int bx = col + left - f.face->glyph->bitmap_left;
                int by = row - top + f.face->glyph->bitmap_top;
                if (bx >= 0 && by >= 0 && bx < int(base.width) && by < int(base.rows)) {
                    int a = base.buffer[by * base.pitch + bx];
                    alpha = outline ? std::max(0, alpha - a) : a;
                }
                // Original global sil_freetype_antialias is 1 (live value),
                // independent of the texture filtering flag font_set_antialias.
                rgba[(std::size_t(y + row) * w + x + col) * 4 + 3] =
                    static_cast<unsigned char>(alpha);
            }
        }
        float adv = std::round(advance / 65536.f);
        out.glyphs.emplace(cp, Glyph{x, y, gw, gh, top, float(left), adv - left - gw});
    }
    out.atlas = texture(rgba.data(), w, h);
    return fonts_.emplace(std::move(key), std::move(out)).first->second;
}
} // namespace ftl
