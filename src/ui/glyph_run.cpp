// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/glyph_run.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <cstring>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_DRIVER_H
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_SYNTHESIS_H
#include <limits>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unicode/uchar.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRSXform.h"
#include "include/core/SkStream.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skshaper/include/SkShaper.h"
#include "modules/skshaper/include/SkShaper_harfbuzz.h"
#include "modules/skunicode/include/SkUnicode.h"

#include "ui/application.h"
#include "ui/font_fallback.h"
#include "ui/painter.h"
#include "ui/text_context.h"

namespace cutefish::ui {

// FreeType's oblique shear, 0x0366A in 16.16.
static constexpr float kObliqueSkew = -float(0x0366A) / 65536.0f;
// Shaped clusters and character faces kept before each cache starts over: a
// terminal's output can make new clusters without end.
static constexpr size_t kMaximumCachedSingles = 65536;
static constexpr size_t kMaximumCachedClusters = 8192;

// The pixel size for a point size at 96 logical DPI: rounded to two
// decimals, then to whole pixels.
static double pixelSizeForPoints(double points)
{
    const double size = std::floor(points * 96.0 / 72.0 * 100 + 0.5) / 100;
    return std::max(1.0, std::floor(size + 0.5));
}

// 26.6 fixed point, rounded.
static double fixedReal(double value)
{
    return std::round(value * 64) / 64;
}

static int roundedInt(double value)
{
    return value >= 0 ? int(value + 0.5) : int(value - 0.5);
}

// FreeType's FT_MulFix and FT_DivFix.
static int64_t mulFix(int64_t a, int64_t b)
{
    const int64_t product = a * b;
    return product >= 0 ? (product + 0x8000) >> 16 : -((-product + 0x8000) >> 16);
}

static int64_t divFix(int64_t a, int64_t b)
{
    if (b == 0)
        return 0;
    const bool negative = (a < 0) != (b < 0);
    const int64_t quotient = ((std::abs(a) << 16) + std::abs(b) / 2) / std::abs(b);
    return negative ? -quotient : quotient;
}

static int16_t readInt16(const uint8_t* data)
{
    return int16_t(uint16_t(data[0]) << 8 | data[1]);
}

static uint16_t readUint16(const uint8_t* data)
{
    return uint16_t(uint16_t(data[0]) << 8 | data[1]);
}

static std::vector<uint8_t> tableData(const SkTypeface* typeface, const char tag[4])
{
    const SkFontTableTag table = SkSetFourByteTag(tag[0], tag[1], tag[2], tag[3]);
    const size_t size = typeface->getTableSize(table);
    std::vector<uint8_t> data(size);
    if (size)
        typeface->getTableData(table, 0, size, data.data());
    return data;
}

// Whether the face in the file is bold and italic, from its OS/2 table (the
// head table's macStyle without one). fontconfig reports the style it was
// asked for when it synthesises one, and the file's bytes carry none of that.
static void fileStyle(const SkTypeface* typeface, bool* bold, bool* italic)
{
    const std::vector<uint8_t> os2 = tableData(typeface, "OS/2");
    if (os2.size() >= 64) {
        const uint16_t selection = readUint16(os2.data() + 62);
        *bold = readUint16(os2.data() + 4) >= 600;
        *italic = (selection & 0x0001) || (selection & 0x0200);
        return;
    }
    const std::vector<uint8_t> head = tableData(typeface, "head");
    const uint16_t macStyle = head.size() >= 46 ? readUint16(head.data() + 44) : 0;
    *bold = macStyle & 0x0001;
    *italic = macStyle & 0x0002;
}

struct GlyphRendererPrivate
{
    struct Face
    {
        sk_sp<SkTypeface> typeface;
        // At the device size, with the synthetic styles the face needs.
        SkFont font;
        // Colour glyphs (emoji) go through Skia; the rest through FreeType
        // into the atlas.
        bool color = false;
        bool embolden = false;
        bool oblique = false;
        // Opened on first use over `data`, which FreeType reads from for as
        // long as the face lives: on the heap, so growing the face list
        // (fallback faces) never moves it.
        FT_Face ft = nullptr;
        bool ftFailed = false;
        std::unique_ptr<std::string> data;
    };

    // A rasterised glyph in the atlas: its rect there and its offset from
    // the pen, both device pixels; an empty rect draws nothing.
    struct AtlasGlyph
    {
        SkIRect rect = SkIRect::MakeEmpty();
        int left = 0;
        int top = 0;
    };

    struct Placement
    {
        uint16_t face;
        SkGlyphID glyph;
        float dx;
        float dy;
    };

    struct Shaped
    {
        std::vector<Placement> glyphs;
        float advance = 0;
    };

    GlyphRendererPrivate(const FontSpec& spec, double scale);
    ~GlyphRendererPrivate();

    FT_Face ftFace(int face);
    // Glyphs are hinted only at scale 1; scaled text draws unhinted.
    bool hinted() const { return scale == 1; }
    const AtlasGlyph& atlasGlyph(int face, SkGlyphID glyph);
    // Space for a glyph in the atlas, growing or emptying it when full.
    bool allocate(int width, int height, SkIPoint* position);

    int faceFor(sk_sp<SkTypeface> typeface);
    // The face drawing the character: the font's own when it has it, else
    // fontconfig's fallback; the font's own when nothing has it.
    int faceForCharacter(char32_t character);
    // The colour emoji font's face when it has the character, else -1.
    int emojiFace(char32_t character);
    const Shaped& shape(std::u32string_view cluster);
    Shaped shapeCluster(std::u32string_view cluster);

    FontSpec spec;
    double scale = 1;
    double pixelSize = 0;
    double devicePixelSize = 0;
    SkFontStyle style;
    sk_sp<SkFontMgr> fontManager;
    sk_sp<SkTypeface> emojiTypeface;
    std::unique_ptr<FontFallback> fallback;
    std::vector<Face> faces;
    std::unordered_map<SkTypefaceID, int> faceIndex;
    std::unordered_map<char32_t, int> characterFaces;
    std::unordered_map<char32_t, Shaped> singles;
    std::unordered_map<std::u32string, Shaped> clusters;
    std::unique_ptr<SkShaper> shaper;

    FT_Library library = nullptr;
    std::unordered_map<uint64_t, AtlasGlyph> atlasGlyphs;
    // White glyphs with coverage as alpha, premultiplied, tinted when drawn.
    SkBitmap atlas;
    sk_sp<SkImage> atlasImage;
    int shelfX = 0;
    int shelfY = 0;
    int shelfHeight = 0;
};

static sk_sp<SkTypeface> matchTypeface(SkFontMgr* manager, const std::string& family, SkFontStyle style)
{
    if (!manager)
        return nullptr;
    if (!family.empty()) {
        if (sk_sp<SkTypeface> typeface = manager->matchFamilyStyle(family.c_str(), style))
            return typeface;
    }
    // A missing family takes the theme's.
    const std::string& theme = Application::instance()->text().family();
    if (sk_sp<SkTypeface> typeface = manager->matchFamilyStyle(theme.c_str(), style))
        return typeface;
    return manager->legacyMakeTypeface(nullptr, style);
}

GlyphRendererPrivate::GlyphRendererPrivate(const FontSpec& font, double deviceScale)
    : spec(font)
    , scale(deviceScale)
    , pixelSize(pixelSizeForPoints(font.pointSize))
    , devicePixelSize(pixelSize * deviceScale)
    , style(font.bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight, SkFontStyle::kNormal_Width,
            font.italic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant)
{
    TextContext& text = Application::instance()->text();
    fontManager = text.fontCollection()->getFallbackManager();
    sk_sp<SkTypeface> primary = matchTypeface(fontManager.get(), spec.family, style);
    if (primary)
        faceFor(std::move(primary));
}

int GlyphRendererPrivate::faceFor(sk_sp<SkTypeface> typeface)
{
    const SkTypefaceID id = typeface->uniqueID();
    if (const auto it = faceIndex.find(id); it != faceIndex.end())
        return it->second;

    TextContext& text = Application::instance()->text();
    SkFont font(typeface, SkScalar(devicePixelSize));
    // Greyscale, whatever fontconfig's rgba says: the cells sit on
    // translucent backgrounds.
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(text.hinting());
    font.setSubpixel(false);
    font.setBaselineSnap(true);
    bool boldFace = false;
    bool italicFace = false;
    fileStyle(typeface.get(), &boldFace, &italicFace);
    const bool oblique = spec.italic && !italicFace;
    if (oblique)
        font.setSkewX(kObliqueSkew);
    // Sizes of 64 px or more are never emboldened. Clusters are placed one by
    // one, so the wider advance of an emboldened glyph moves nothing.
    const bool embolden = spec.bold && !boldFace && devicePixelSize < 64;
    if (embolden)
        font.setEmbolden(true);
    const auto hasTable = [&typeface](const char tag[4]) {
        return typeface->getTableSize(SkSetFourByteTag(tag[0], tag[1], tag[2], tag[3])) > 0;
    };
    const bool color = hasTable("CBDT") || hasTable("COLR") || hasTable("sbix") || hasTable("SVG ");

    const int index = int(faces.size());
    Face entry;
    entry.typeface = std::move(typeface);
    entry.font = font;
    entry.color = color;
    entry.embolden = embolden;
    entry.oblique = oblique;
    faces.push_back(std::move(entry));
    faceIndex.emplace(id, index);
    return index;
}

GlyphRendererPrivate::~GlyphRendererPrivate()
{
    for (Face& face : faces) {
        if (face.ft)
            FT_Done_Face(face.ft);
    }
    if (library)
        FT_Done_FreeType(library);
}

FT_Face GlyphRendererPrivate::ftFace(int index)
{
    Face& face = faces[size_t(index)];
    if (face.ft || face.ftFailed)
        return face.ft;
    face.ftFailed = true;
    if (!library) {
        if (FT_Init_FreeType(&library) != 0) {
            library = nullptr;
            return nullptr;
        }
        // CFF stems are darkened (Noto Sans CJK, the usual fallback), which
        // also moves their hinted edges: block elements sit a row higher.
        FT_Bool noStemDarkening = false;
        FT_Property_Set(library, "cff", "no-stem-darkening", &noStemDarkening);
    }
    // The font's own bytes: fontconfig's match flags (FC_EMBOLDEN, a matrix)
    // stay behind, and the synthetic styles are decided here.
    int ttcIndex = 0;
    std::unique_ptr<SkStreamAsset> stream = face.typeface->openStream(&ttcIndex);
    if (!stream)
        return nullptr;
    face.data = std::make_unique<std::string>(stream->getLength(), '\0');
    if (stream->read(face.data->data(), face.data->size()) != face.data->size())
        return nullptr;
    if (FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte*>(face.data->data()),
                           FT_Long(face.data->size()), ttcIndex, &face.ft) != 0) {
        face.ft = nullptr;
        return nullptr;
    }
    // At the device pixel size; hinted only at scale 1.
    FT_Set_Char_Size(face.ft, 0, FT_F26Dot6(std::lround(devicePixelSize * 64)), 0, 0);
    if (face.oblique) {
        FT_Matrix matrix{0x10000, 0x0366A, 0, 0x10000};
        FT_Set_Transform(face.ft, &matrix, nullptr);
    }
    face.ftFailed = false;
    return face.ft;
}

bool GlyphRendererPrivate::allocate(int width, int height, SkIPoint* position)
{
    constexpr int kWidth = 1024;
    constexpr int kMaximumHeight = 4096;
    if (width > kWidth)
        return false;
    if (atlas.isNull()) {
        atlas.allocN32Pixels(kWidth, 256);
        atlas.eraseColor(SK_ColorTRANSPARENT);
    }
    if (shelfX + width > kWidth) {
        shelfY += shelfHeight + 1;
        shelfX = 0;
        shelfHeight = 0;
    }
    if (shelfY + height > atlas.height()) {
        const int grown = std::max(atlas.height() * 2, shelfY + height);
        if (grown > kMaximumHeight) {
            // Full: start over; what is drawn next fills it again.
            atlasGlyphs.clear();
            atlas.eraseColor(SK_ColorTRANSPARENT);
            shelfX = shelfY = shelfHeight = 0;
        } else {
            SkBitmap larger;
            larger.allocN32Pixels(kWidth, grown);
            larger.eraseColor(SK_ColorTRANSPARENT);
            larger.writePixels(atlas.pixmap(), 0, 0);
            atlas = larger;
        }
    }
    *position = SkIPoint::Make(shelfX, shelfY);
    shelfX += width + 1;
    shelfHeight = std::max(shelfHeight, height);
    return true;
}

const GlyphRendererPrivate::AtlasGlyph& GlyphRendererPrivate::atlasGlyph(int face, SkGlyphID glyph)
{
    const uint64_t key = uint64_t(face) << 32 | glyph;
    if (const auto it = atlasGlyphs.find(key); it != atlasGlyphs.end())
        return it->second;
    AtlasGlyph result;
    FT_Face ft = ftFace(face);
    // Full hinting, without embedded bitmaps.
    const FT_Int32 hinting = hinted() ? FT_LOAD_TARGET_NORMAL : FT_LOAD_NO_HINTING;
    if (ft && FT_Load_Glyph(ft, glyph, FT_LOAD_DEFAULT | hinting | FT_LOAD_NO_BITMAP) == 0) {
        FT_GlyphSlot slot = ft->glyph;
        if (faces[size_t(face)].embolden)
            FT_GlyphSlot_Embolden(slot);
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) == 0 && slot->bitmap.width > 0 && slot->bitmap.rows > 0
            && slot->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            const int width = int(slot->bitmap.width);
            const int height = int(slot->bitmap.rows);
            SkIPoint position;
            if (allocate(width, height, &position)) {
                for (int y = 0; y < height; ++y) {
                    const uint8_t* source = slot->bitmap.buffer + ptrdiff_t(y) * slot->bitmap.pitch;
                    auto* target = atlas.getAddr32(position.x(), position.y() + y);
                    for (int x = 0; x < width; ++x) {
                        const uint32_t a = source[x];
                        target[x] = a << 24 | a << 16 | a << 8 | a;
                    }
                }
                result.rect = SkIRect::MakeXYWH(position.x(), position.y(), width, height);
                result.left = slot->bitmap_left;
                result.top = slot->bitmap_top;
                atlasImage.reset();
            }
        }
    }
    return atlasGlyphs.emplace(key, result).first->second;
}

// Emoji presentation goes to the colour emoji font first, even where the
// font has a glyph of its own.
static bool presentsAsEmoji(char32_t character)
{
    return u_hasBinaryProperty(UChar32(character), UCHAR_EMOJI_PRESENTATION);
}

int GlyphRendererPrivate::emojiFace(char32_t character)
{
    if (!emojiTypeface) {
        emojiTypeface = fontManager->matchFamilyStyle("emoji", SkFontStyle());
        if (!emojiTypeface)
            emojiTypeface = fontManager->matchFamilyStyle("Noto Color Emoji", SkFontStyle());
    }
    if (emojiTypeface && emojiTypeface->unicharToGlyph(SkUnichar(character)) != 0)
        return faceFor(emojiTypeface);
    return -1;
}

int GlyphRendererPrivate::faceForCharacter(char32_t character)
{
    if (faces.empty())
        return -1;
    if (const auto it = characterFaces.find(character); it != characterFaces.end())
        return it->second;
    if (characterFaces.size() >= kMaximumCachedSingles)
        characterFaces.clear();
    if (presentsAsEmoji(character)) {
        const int emoji = emojiFace(character);
        if (emoji >= 0) {
            characterFaces.emplace(character, emoji);
            return emoji;
        }
    }
    if (faces.front().typeface->unicharToGlyph(SkUnichar(character)) != 0)
        return 0;
    int index = 0;
    if (!fallback)
        fallback = std::make_unique<FontFallback>(spec.family, style);
    if (sk_sp<SkTypeface> typeface = fallback->typefaceFor(character))
        index = faceFor(std::move(typeface));
    characterFaces.emplace(character, index);
    return index;
}

// Collects the glyphs of a shaped cluster relative to its origin.
class ClusterHandler final : public SkShaper::RunHandler
{
public:
    ClusterHandler(GlyphRendererPrivate& renderer, GlyphRendererPrivate::Shaped& shaped)
        : m_renderer(renderer)
        , m_shaped(shaped)
    {
    }

    void beginLine() override {}
    void runInfo(const RunInfo&) override {}
    void commitRunInfo() override {}

    Buffer runBuffer(const RunInfo& info) override
    {
        m_glyphs.assign(info.glyphCount, 0);
        m_positions.assign(info.glyphCount, SkPoint::Make(0, 0));
        return {m_glyphs.data(), m_positions.data(), nullptr, nullptr, SkPoint::Make(m_x, 0)};
    }

    void commitRunBuffer(const RunInfo& info) override
    {
        const int face = m_renderer.faceFor(info.fFont.refTypeface());
        for (size_t i = 0; i < m_glyphs.size(); ++i)
            m_shaped.glyphs.push_back({uint16_t(face), m_glyphs[i], m_positions[i].x(), m_positions[i].y()});
        m_x += info.fAdvance.x();
        m_shaped.advance = m_x;
    }

    void commitLine() override {}

private:
    GlyphRendererPrivate& m_renderer;
    GlyphRendererPrivate::Shaped& m_shaped;
    std::vector<SkGlyphID> m_glyphs;
    std::vector<SkPoint> m_positions;
    float m_x = 0;
};

static std::string toUtf8(std::u32string_view text)
{
    std::string utf8;
    utf8.reserve(text.size());
    for (char32_t c : text) {
        if (c < 0x80) {
            utf8 += char(c);
        } else if (c < 0x800) {
            utf8 += char(0xC0 | (c >> 6));
            utf8 += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            utf8 += char(0xE0 | (c >> 12));
            utf8 += char(0x80 | ((c >> 6) & 0x3F));
            utf8 += char(0x80 | (c & 0x3F));
        } else {
            utf8 += char(0xF0 | (c >> 18));
            utf8 += char(0x80 | ((c >> 12) & 0x3F));
            utf8 += char(0x80 | ((c >> 6) & 0x3F));
            utf8 += char(0x80 | (c & 0x3F));
        }
    }
    return utf8;
}

GlyphRendererPrivate::Shaped GlyphRendererPrivate::shapeCluster(std::u32string_view cluster)
{
    Shaped shaped;
    if (faces.empty() || cluster.empty())
        return shaped;

    if (cluster.size() == 1) {
        const int face = faceForCharacter(cluster.front());
        // Copied: a lookup further on may add a face, moving the list.
        const SkFont font = faces[size_t(face)].font;
        const SkGlyphID glyph = faces[size_t(face)].typeface->unicharToGlyph(SkUnichar(cluster.front()));
        SkScalar advance = 0;
        font.getWidths({&glyph, 1}, {&advance, 1});
        shaped.glyphs.push_back({uint16_t(face), glyph, 0, 0});
        shaped.advance = advance;
        return shaped;
    }

    // An emoji sequence takes the emoji font; anything else the face
    // covering the whole cluster, else the first character's.
    int face = 0;
    const bool emoji = presentsAsEmoji(cluster.front())
            || cluster.find(U'\uFE0F') != std::u32string_view::npos
            || cluster.find(U'\u200D') != std::u32string_view::npos;
    if (emoji)
        face = std::max(0, emojiFace(cluster.front()));
    for (char32_t c : cluster) {
        if (face == 0 && faces.front().typeface->unicharToGlyph(SkUnichar(c)) == 0) {
            face = faceForCharacter(cluster.front());
            break;
        }
    }
    if (!shaper)
        shaper = SkShapers::HB::ShapeDontWrapOrReorder(Application::instance()->text().unicode(), nullptr);
    const std::string utf8 = toUtf8(cluster);
    const SkFont font = faces[size_t(face)].font;
    SkShaper::TrivialFontRunIterator fontRuns(font, utf8.size());
    SkShaper::TrivialBiDiRunIterator bidi(0, utf8.size());
    std::unique_ptr<SkShaper::ScriptRunIterator> script = SkShapers::HB::ScriptRunIterator(utf8.data(), utf8.size());
    const std::string& locale = Application::instance()->text().locale();
    SkShaper::TrivialLanguageRunIterator language(locale.empty() ? "en" : locale.c_str(), utf8.size());
    // Kerning off, so terminal cells keep their width.
    const SkShaper::Feature features[] = {{SkSetFourByteTag('k', 'e', 'r', 'n'), 0, 0, utf8.size()}};
    ClusterHandler handler(*this, shaped);
    if (shaper && script)
        shaper->shape(utf8.data(), utf8.size(), fontRuns, bidi, *script, language, features, 1,
                      std::numeric_limits<SkScalar>::max(), &handler);
    return shaped;
}

const GlyphRendererPrivate::Shaped& GlyphRendererPrivate::shape(std::u32string_view cluster)
{
    if (cluster.size() == 1) {
        const auto it = singles.find(cluster.front());
        if (it != singles.end())
            return it->second;
        if (singles.size() >= kMaximumCachedSingles)
            singles.clear();
        return singles.emplace(cluster.front(), shapeCluster(cluster)).first->second;
    }
    std::u32string key(cluster);
    const auto it = clusters.find(key);
    if (it != clusters.end())
        return it->second;
    if (clusters.size() >= kMaximumCachedClusters)
        clusters.clear();
    return clusters.emplace(std::move(key), shapeCluster(cluster)).first->second;
}

static std::vector<FontFamily> listFamilies(const std::string& only = {});

// The family's pitch as listFamilies() decides it. Only fonts naming the
// family can register it, so only they are listed.
static bool familyIsFixedPitch(const std::string& family)
{
    static std::mutex mutex;
    static std::map<std::string, bool> known;
    std::lock_guard lock(mutex);
    if (const auto it = known.find(family); it != known.end())
        return it->second;
    bool fixed = false;
    for (const FontFamily& candidate : listFamilies(family)) {
        if (candidate.name == family)
            fixed = candidate.fixedPitch;
    }
    known.emplace(family, fixed);
    return fixed;
}

FontMetrics::FontMetrics(const FontSpec& font, double scale, bool hintedLayout)
    : m_font(std::make_shared<GlyphRendererPrivate>(FontSpec{font.family, font.pointSize, font.bold, font.italic}, 1.0))
{
    m_pixelSize = m_font->pixelSize;
    if (m_font->faces.empty())
        return;
    const SkTypeface* typeface = m_font->faces.front().typeface.get();
    SkString familyName;
    typeface->getFamilyName(&familyName);
    m_family = familyName.c_str();
    m_fixedPitch = familyIsFixedPitch(m_family);

    const TextContext& text = Application::instance()->text();
    m_roundAdvances = text.roundsMetrics(scale, hintedLayout);
    const int unitsPerEm = std::max(1, typeface->getUnitsPerEm());
    const double perUnit = m_pixelSize / unitsPerEm;

    // FreeType's size metrics first.
    SkFont unscaled(sk_ref_sp(const_cast<SkTypeface*>(typeface)), SkScalar(m_pixelSize));
    unscaled.setHinting(SkFontHinting::kNone);
    SkFontMetrics skMetrics;
    unscaled.getMetrics(&skMetrics);
    m_ascent = fixedReal(-skMetrics.fAscent);
    m_descent = fixedReal(skMetrics.fDescent);
    m_leading = fixedReal(skMetrics.fLeading);

    // Then hhea, overridden by OS/2: its typo metrics with USE_TYPO_METRICS,
    // else its Windows ascent and descent.
    const std::vector<uint8_t> hhea = tableData(typeface, "hhea");
    if (hhea.size() >= 10) {
        const int16_t ascent = readInt16(hhea.data() + 4);
        const int16_t descent = readInt16(hhea.data() + 6);
        const int16_t lineGap = readInt16(hhea.data() + 8);
        if (ascent != 0 || descent != 0) {
            m_ascent = fixedReal(ascent * perUnit);
            m_descent = -fixedReal(descent * perUnit);
            m_leading = fixedReal(lineGap * perUnit);
        }
    }
    const std::vector<uint8_t> os2 = tableData(typeface, "OS/2");
    if (os2.size() >= 78) {
        const uint16_t selection = readUint16(os2.data() + 62);
        const int16_t typoAscent = readInt16(os2.data() + 68);
        const int16_t typoDescent = readInt16(os2.data() + 70);
        const int16_t typoLineGap = readInt16(os2.data() + 72);
        const uint16_t winAscent = readUint16(os2.data() + 74);
        const uint16_t winDescent = readUint16(os2.data() + 76);
        if (selection & 0x80) {
            if (typoAscent != 0 || typoDescent != 0) {
                m_ascent = fixedReal(typoAscent * perUnit);
                m_descent = -fixedReal(typoDescent * perUnit);
                m_leading = fixedReal(typoLineGap * perUnit);
            }
        } else if (winAscent != 0 || winDescent != 0) {
            m_ascent = fixedReal(winAscent * perUnit);
            m_descent = fixedReal(winDescent * perUnit);
            m_leading = 0;
        }
    }
    // Each rounded on its own, not the leading against the whole height.
    if (m_roundAdvances) {
        m_ascent = std::floor(m_ascent + 0.5);
        m_descent = std::floor(m_descent + 0.5);
        m_leading = std::floor(m_leading + 0.5);
    }

    // Underline from the post table through FreeType's fixed point.
    const std::vector<uint8_t> post = tableData(typeface, "post");
    if (post.size() >= 12) {
        const int64_t yScale = divFix(int64_t(m_pixelSize) * 64, unitsPerEm);
        const int16_t postThickness = readInt16(post.data() + 10);
        // FreeType moves the post table's top edge to the stem's centre.
        const auto facePosition = int16_t(readInt16(post.data() + 8) - postThickness / 2);
        const int64_t thickness = mulFix(postThickness, yScale);
        const int64_t center = -mulFix(facePosition, yScale);
        // The division truncates.
        const int64_t position = center - thickness / 2;
        m_lineThickness = thickness / 64.0;
        m_underlinePosition = position / 64.0;
    } else {
        const double score = (m_font->spec.bold ? 700 : 400) * m_pixelSize;
        m_lineThickness = std::floor(score / 7000);
        if (m_lineThickness < 2 && score >= 1050)
            m_lineThickness = 2;
        m_underlinePosition = std::floor((m_lineThickness * 2 + 3) / 6);
    }
    if (m_lineThickness < 1)
        m_lineThickness = 1;
}

double FontMetrics::strikeOutPosF() const
{
    return m_ascent / 3.0;
}

int FontMetrics::ascent() const
{
    return roundedInt(m_ascent);
}

int FontMetrics::descent() const
{
    return roundedInt(m_descent);
}

int FontMetrics::leading() const
{
    return roundedInt(m_leading);
}

int FontMetrics::height() const
{
    return ascent() + descent();
}

int FontMetrics::lineSpacing() const
{
    return leading() + ascent() + descent();
}

int FontMetrics::underlinePos() const
{
    return roundedInt(m_underlinePosition);
}

int FontMetrics::strikeOutPos() const
{
    const int position = ascent() / 3;
    return position > 0 ? position : 1;
}

int FontMetrics::lineWidth() const
{
    return roundedInt(m_lineThickness);
}

double FontMetrics::horizontalAdvanceF(std::string_view text) const
{
    if (!m_font || m_font->faces.empty())
        return 0;
    double total = 0;
    size_t i = 0;
    while (i < text.size()) {
        // UTF-8 to a code point.
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        char32_t c = lead;
        size_t length = 1;
        if (lead >= 0xF0 && i + 3 < text.size()) {
            c = char32_t(lead & 0x07) << 18 | char32_t(text[i + 1] & 0x3F) << 12
                    | char32_t(text[i + 2] & 0x3F) << 6 | char32_t(text[i + 3] & 0x3F);
            length = 4;
        } else if (lead >= 0xE0 && i + 2 < text.size()) {
            c = char32_t(lead & 0x0F) << 12 | char32_t(text[i + 1] & 0x3F) << 6 | char32_t(text[i + 2] & 0x3F);
            length = 3;
        } else if (lead >= 0xC0 && i + 1 < text.size()) {
            c = char32_t(lead & 0x1F) << 6 | char32_t(text[i + 1] & 0x3F);
            length = 2;
        }
        i += length;

        const int face = m_font->faceForCharacter(c);
        const SkTypeface* typeface = m_font->faces[size_t(face)].typeface.get();
        const SkGlyphID glyph = typeface->unicharToGlyph(SkUnichar(c));
        SkFont font(sk_ref_sp(const_cast<SkTypeface*>(typeface)), SkScalar(m_pixelSize));
        font.setHinting(SkFontHinting::kNone);
        font.setLinearMetrics(true);
        font.setSubpixel(true);
        SkScalar advance = 0;
        font.getWidths({&glyph, 1}, {&advance, 1});
        // Hinted advances are whole pixels; linear ones 26.6, truncated.
        total += m_roundAdvances ? std::round(double(advance)) : std::floor(double(advance) * 64) / 64;
    }
    return total;
}

int FontMetrics::horizontalAdvance(std::string_view text) const
{
    return roundedInt(horizontalAdvanceF(text));
}

GlyphRenderer::GlyphRenderer(const FontSpec& font, double scale)
    : d(std::make_shared<GlyphRendererPrivate>(font, scale))
{
}

GlyphRenderer::~GlyphRenderer() = default;

const FontSpec& GlyphRenderer::font() const
{
    return d->spec;
}

double GlyphRenderer::scale() const
{
    return d->scale;
}

double GlyphRenderer::advance(std::u32string_view cluster)
{
    return d->shape(cluster).advance;
}

size_t GlyphRenderer::cachedClusters() const
{
    return d->singles.size() + d->clusters.size();
}

void GlyphRenderer::clearCache()
{
    d->singles.clear();
    d->clusters.clear();
    d->characterFaces.clear();
    d->atlasGlyphs.clear();
    d->atlas.reset();
    d->atlasImage.reset();
    d->shelfX = d->shelfY = d->shelfHeight = 0;
}

static SkColor skColor(Color color, double opacity)
{
    const auto alpha = uint8_t(std::lround(color.a * std::clamp(opacity, 0.0, 1.0)));
    return SkColorSetARGB(alpha, color.r, color.g, color.b);
}

void GlyphRenderer::draw(Painter& painter, Color color, double baseline, std::span<const GlyphCluster> clusters,
                         SizeF stretch, PointF stretchOrigin)
{
    if (clusters.empty() || d->faces.empty())
        return;
    const double originX = painter.toDevice(painter.offset().x);
    const double originY = painter.toDevice(painter.offset().y);
    const float y = float(originY + std::round(baseline));

    // Atlas sprites for outline glyphs; colour glyphs per face for Skia.
    thread_local std::vector<SkRSXform> transforms;
    thread_local std::vector<SkRect> sources;
    thread_local std::vector<std::vector<std::pair<SkGlyphID, SkPoint>>> colorGlyphs;
    transforms.clear();
    sources.clear();
    if (colorGlyphs.size() < d->faces.size())
        colorGlyphs.resize(d->faces.size());
    bool anyColor = false;
    for (const GlyphCluster& cluster : clusters) {
        if (cluster.text.empty())
            continue;
        const GlyphRendererPrivate::Shaped& shaped = d->shape(cluster.text);
        if (colorGlyphs.size() < d->faces.size())
            colorGlyphs.resize(d->faces.size());
        const float x = float(originX + std::round(cluster.x));
        for (const GlyphRendererPrivate::Placement& placement : shaped.glyphs) {
            if (d->faces[placement.face].color) {
                colorGlyphs[placement.face].emplace_back(placement.glyph,
                                                         SkPoint::Make(x + placement.dx, y + placement.dy));
                anyColor = true;
                continue;
            }
            const GlyphRendererPrivate::AtlasGlyph& glyph = d->atlasGlyph(placement.face, placement.glyph);
            if (glyph.rect.isEmpty())
                continue;
            transforms.push_back(SkRSXform::Make(1, 0, std::round(x + placement.dx) + float(glyph.left),
                                                 std::round(y + placement.dy) - float(glyph.top)));
            sources.push_back(SkRect::Make(glyph.rect));
        }
    }

    SkCanvas* canvas = painter.canvas();
    const bool stretched = stretch.width != 1 || stretch.height != 1;
    if (stretched) {
        canvas->save();
        const auto pivotX = SkScalar(originX + stretchOrigin.x);
        const auto pivotY = SkScalar(originY + stretchOrigin.y);
        canvas->translate(pivotX, pivotY);
        canvas->scale(SkScalar(stretch.width), SkScalar(stretch.height));
        canvas->translate(-pivotX, -pivotY);
    }
    const SkColor tint = skColor(color, painter.opacity());
    if (!transforms.empty()) {
        if (!d->atlasImage)
            d->atlasImage = SkImages::RasterFromBitmap(d->atlas);
        thread_local std::vector<SkColor> colors;
        colors.assign(transforms.size(), tint);
        // The white coverage times the colour, as tinted atlas quads.
        canvas->drawAtlas(d->atlasImage.get(), transforms, sources, colors, SkBlendMode::kModulate,
                          SkSamplingOptions(), nullptr, nullptr);
    }
    if (anyColor) {
        SkTextBlobBuilder builder;
        for (size_t face = 0; face < d->faces.size(); ++face) {
            std::vector<std::pair<SkGlyphID, SkPoint>>& glyphs = colorGlyphs[face];
            if (glyphs.empty())
                continue;
            const SkTextBlobBuilder::RunBuffer& run = builder.allocRunPos(d->faces[face].font, int(glyphs.size()));
            for (size_t i = 0; i < glyphs.size(); ++i) {
                run.glyphs[i] = glyphs[i].first;
                run.points()[i] = glyphs[i].second;
            }
            glyphs.clear();
        }
        if (const sk_sp<SkTextBlob> blob = builder.make()) {
            SkPaint paint;
            paint.setColor(tint);
            paint.setAntiAlias(true);
            canvas->drawTextBlob(blob, 0, 0, paint);
        }
    }
    if (stretched)
        canvas->restore();
}

void fillDeviceRect(Painter& painter, const Rect& rect, Color color)
{
    if (rect.width <= 0 || rect.height <= 0 || color.a == 0)
        return;
    SkPaint paint;
    paint.setColor(skColor(color, painter.opacity()));
    paint.setAntiAlias(false);
    const int x = painter.toDevice(painter.offset().x) + rect.x;
    const int y = painter.toDevice(painter.offset().y) + rect.y;
    painter.canvas()->drawIRect(SkIRect::MakeXYWH(x, y, rect.width, rect.height), paint);
}

static const char* patternString(FcPattern* pattern, const char* object, int index)
{
    FcChar8* value = nullptr;
    if (FcPatternGetString(pattern, object, index, &value) != FcResultMatch)
        return nullptr;
    return reinterpret_cast<const char*>(value);
}

// Families as fontconfig lists them: each font's first family, and a further
// family of the same language naming a subfamily (another style); the rest are
// aliases. A family's pitch is that of the font registered last, then the
// generic Serif, Sans Serif and Monospace. Families with several foundries are
// listed once.
static std::vector<FontFamily> listFamilies(const std::string& only)
{
    // On the configuration the toolkit's fonts were loaded with.
    if (Application* application = Application::instance())
        application->text();
    // Families are found without regard to case: one entry, the first name.
    std::map<std::string, FontFamily> families;
    const auto registerFamily = [&families](const std::string& name, bool fixed) {
        std::string key = name;
        for (char& c : key)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        auto [it, added] = families.try_emplace(key, FontFamily{name, fixed});
        it->second.fixedPitch = fixed;
    };
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddBool(pattern, FC_VARIABLE, FcFalse);
    if (!only.empty())
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(only.c_str()));
    FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_SPACING, FC_FAMILYLANG, nullptr);
    FcFontSet* fonts = FcFontList(nullptr, pattern, objects);
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    if (fonts) {
        for (int i = 0; i < fonts->nfont; ++i) {
            FcPattern* font = fonts->fonts[i];
            const char* family = patternString(font, FC_FAMILY, 0);
            if (!family)
                continue;
            int spacing = FC_PROPORTIONAL;
            if (FcPatternGetInteger(font, FC_SPACING, 0, &spacing) != FcResultMatch)
                spacing = FC_PROPORTIONAL;
            const bool fixed = spacing >= FC_MONO;
            registerFamily(family, fixed);

            const char* language = patternString(font, FC_FAMILYLANG, 0);
            const char* style = patternString(font, FC_STYLE, 0);
            const std::string familyLanguage = language ? language : "";
            const std::string styleName = style ? style : "";
            for (int k = 1; const char* alternative = patternString(font, FC_FAMILY, k); ++k) {
                const char* alternativeStyle = patternString(font, FC_STYLE, k);
                const char* alternativeLanguage = patternString(font, FC_FAMILYLANG, k);
                const std::string altStyle = alternativeStyle ? alternativeStyle : styleName;
                const std::string altLanguage = alternativeLanguage ? alternativeLanguage : familyLanguage;
                if (altLanguage == familyLanguage && altStyle != styleName)
                    registerFamily(alternative, fixed);
            }
        }
        FcFontSetDestroy(fonts);
    }
    registerFamily("Serif", false);
    registerFamily("Sans Serif", false);
    registerFamily("Monospace", true);

    std::vector<FontFamily> list;
    list.reserve(families.size());
    for (const auto& [key, family] : families)
        list.push_back(family);
    return list;
}

std::vector<FontFamily> fontFamilies()
{
    return listFamilies();
}

} // namespace cutefish::ui
