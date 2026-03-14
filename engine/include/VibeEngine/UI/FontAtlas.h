/*
 * FontAtlas — Rasterizes TrueType fonts into a GPU texture atlas using stb_truetype.
 *
 * Supports ASCII range (32–126) at a given pixel height. Each loaded font
 * produces a single atlas texture that the UIRenderer can sample for text quads.
 */
#pragma once

#include "VibeEngine/Renderer/Texture.h"
#include <string>
#include <array>
#include <memory>
#include <unordered_map>

namespace VE {

struct GlyphInfo {
    float U0, V0, U1, V1;   // UV coordinates in atlas
    float Width, Height;     // glyph size in pixels
    float BearingX, BearingY;// offset from baseline
    float Advance;           // horizontal advance in pixels
};

class FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas() = default;

    bool Load(const std::string& ttfPath, float pixelHeight);
    bool LoadDefault(float pixelHeight);

    const GlyphInfo& GetGlyph(char c) const;
    float GetLineHeight() const { return m_LineHeight; }
    float GetPixelHeight() const { return m_PixelHeight; }
    const std::shared_ptr<Texture2D>& GetAtlasTexture() const { return m_AtlasTexture; }

    // Measure text dimensions in pixels
    float MeasureTextWidth(const std::string& text) const;
    float MeasureTextHeight() const { return m_LineHeight; }

    static std::shared_ptr<FontAtlas> Create(const std::string& ttfPath, float pixelHeight);
    static std::shared_ptr<FontAtlas> CreateDefault(float pixelHeight = 32.0f);

private:
    void BuildAtlas(const unsigned char* fontData, float pixelHeight);

    std::array<GlyphInfo, 128> m_Glyphs{};
    std::shared_ptr<Texture2D> m_AtlasTexture;
    float m_PixelHeight = 32.0f;
    float m_LineHeight  = 32.0f;
};

// Global font cache
class FontLibrary {
public:
    static void Register(const std::string& name, const std::shared_ptr<FontAtlas>& font);
    static std::shared_ptr<FontAtlas> Get(const std::string& name);
    static bool Exists(const std::string& name);
    static void Clear();
    static std::shared_ptr<FontAtlas> GetDefault();

private:
    static std::unordered_map<std::string, std::shared_ptr<FontAtlas>>& GetFonts();
};

} // namespace VE
