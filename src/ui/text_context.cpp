// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/text_context.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <strings.h>
#include <tuple>
#include <fontconfig/fontconfig.h>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkStream.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

#include "ui/line_break_unicode.h"
#include "ui/theme.h"

namespace cutefish::ui {

// Character fallbacks remembered before the cache starts over.
static constexpr size_t kMaximumFallbacks = 65536;

// "zh_CN.UTF-8" becomes "zh-CN"; "C" and "POSIX" name no language.
namespace {

// The fonts of one family out of fontconfig's sorted set.
class FamilyStyleSet final : public SkFontStyleSet
{
public:
    explicit FamilyStyleSet(std::vector<sk_sp<SkTypeface>> typefaces)
        : m_typefaces(std::move(typefaces))
    {
    }

    int count() override { return int(m_typefaces.size()); }

    void getStyle(int index, SkFontStyle* style, SkString* name) override
    {
        if (style)
            *style = m_typefaces[size_t(index)]->fontStyle();
        if (name)
            name->reset();
    }

    sk_sp<SkTypeface> createTypeface(int index) override { return m_typefaces[size_t(index)]; }
    sk_sp<SkTypeface> matchStyle(const SkFontStyle& pattern) override { return matchStyleCSS3(pattern); }

private:
    std::vector<sk_sp<SkTypeface>> m_typefaces;
};

static bool sameFamily(const SkString& a, const char* b)
{
    return strcasecmp(a.c_str(), b) == 0;
}

struct StyleRange
{
    float fontconfig;
    float css;
};

// Piecewise linear, as Skia maps fontconfig's weights and widths.
static int mapRanges(float value, const StyleRange* ranges, size_t count)
{
    if (value < ranges[0].fontconfig)
        return SkScalarRoundToInt(ranges[0].css);
    for (size_t i = 0; i + 1 < count; ++i) {
        if (value < ranges[i + 1].fontconfig) {
            const float result = (value - ranges[i].fontconfig) * (ranges[i + 1].css - ranges[i].css)
                    / (ranges[i + 1].fontconfig - ranges[i].fontconfig) + ranges[i].css;
            return SkScalarRoundToInt(result);
        }
    }
    return SkScalarRoundToInt(ranges[count - 1].css);
}

static int patternInteger(FcPattern* pattern, const char* object, int fallback)
{
    int value = 0;
    return FcPatternGetInteger(pattern, object, 0, &value) == FcResultMatch ? value : fallback;
}

// The style Skia's fontconfig typefaces report for the pattern.
static SkFontStyle styleFromPattern(FcPattern* pattern)
{
    static constexpr StyleRange weights[] = {
        {FC_WEIGHT_THIN, SkFontStyle::kThin_Weight},
        {FC_WEIGHT_EXTRALIGHT, SkFontStyle::kExtraLight_Weight},
        {FC_WEIGHT_LIGHT, SkFontStyle::kLight_Weight},
        {FC_WEIGHT_DEMILIGHT, 350},
        {FC_WEIGHT_BOOK, 380},
        {FC_WEIGHT_REGULAR, SkFontStyle::kNormal_Weight},
        {FC_WEIGHT_MEDIUM, SkFontStyle::kMedium_Weight},
        {FC_WEIGHT_DEMIBOLD, SkFontStyle::kSemiBold_Weight},
        {FC_WEIGHT_BOLD, SkFontStyle::kBold_Weight},
        {FC_WEIGHT_EXTRABOLD, SkFontStyle::kExtraBold_Weight},
        {FC_WEIGHT_BLACK, SkFontStyle::kBlack_Weight},
        {FC_WEIGHT_EXTRABLACK, SkFontStyle::kExtraBlack_Weight},
    };
    static constexpr StyleRange widths[] = {
        {FC_WIDTH_ULTRACONDENSED, SkFontStyle::kUltraCondensed_Width},
        {FC_WIDTH_EXTRACONDENSED, SkFontStyle::kExtraCondensed_Width},
        {FC_WIDTH_CONDENSED, SkFontStyle::kCondensed_Width},
        {FC_WIDTH_SEMICONDENSED, SkFontStyle::kSemiCondensed_Width},
        {FC_WIDTH_NORMAL, SkFontStyle::kNormal_Width},
        {FC_WIDTH_SEMIEXPANDED, SkFontStyle::kSemiExpanded_Width},
        {FC_WIDTH_EXPANDED, SkFontStyle::kExpanded_Width},
        {FC_WIDTH_EXTRAEXPANDED, SkFontStyle::kExtraExpanded_Width},
        {FC_WIDTH_ULTRAEXPANDED, SkFontStyle::kUltraExpanded_Width},
    };
    const int weight = mapRanges(float(patternInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR)), weights,
                                 std::size(weights));
    const int width = mapRanges(float(patternInteger(pattern, FC_WIDTH, FC_WIDTH_NORMAL)), widths, std::size(widths));
    SkFontStyle::Slant slant = SkFontStyle::kUpright_Slant;
    switch (patternInteger(pattern, FC_SLANT, FC_SLANT_ROMAN)) {
    case FC_SLANT_ITALIC:
        slant = SkFontStyle::kItalic_Slant;
        break;
    case FC_SLANT_OBLIQUE:
        slant = SkFontStyle::kOblique_Slant;
        break;
    default:
        break;
    }
    return SkFontStyle(weight, width, slant);
}

// One family's fonts as fontconfig lists them; a typeface is made only for
// the style picked.
class ListedFamilyStyleSet final : public SkFontStyleSet
{
public:
    using Creator = std::function<sk_sp<SkTypeface>(int index)>;

    ListedFamilyStyleSet(std::vector<SkFontStyle> styles, Creator creator)
        : m_styles(std::move(styles))
        , m_typefaces(m_styles.size())
        , m_creator(std::move(creator))
    {
    }

    int count() override { return int(m_styles.size()); }

    void getStyle(int index, SkFontStyle* style, SkString* name) override
    {
        if (style)
            *style = m_styles[size_t(index)];
        if (name)
            name->reset();
    }

    sk_sp<SkTypeface> createTypeface(int index) override
    {
        std::lock_guard lock(m_mutex);
        sk_sp<SkTypeface>& typeface = m_typefaces[size_t(index)];
        if (!typeface)
            typeface = m_creator(index);
        return typeface;
    }

    sk_sp<SkTypeface> matchStyle(const SkFontStyle& pattern) override { return matchStyleCSS3(pattern); }

private:
    std::vector<SkFontStyle> m_styles;
    std::mutex m_mutex;
    std::vector<sk_sp<SkTypeface>> m_typefaces;
    Creator m_creator;
};

// The system font manager with its character fallback remembered. SkParagraph
// keeps fallbacks only for one layout, so every layout asked fontconfig again
// for each character the requested fonts lack (about 2 ms per layout of a
// field of U+25CF); Chromium's FontCache keeps them across layouts.
class FallbackCachingFontMgr final : public SkFontMgr
{
public:
    explicit FallbackCachingFontMgr(sk_sp<SkFontMgr> fonts)
        : m_fonts(std::move(fonts))
    {
    }

    void clear()
    {
        std::lock_guard lock(m_mutex);
        m_fallbacks.clear();
        m_families.clear();
        m_styles.clear();
    }

protected:
    int onCountFamilies() const override { return m_fonts->countFamilies(); }
    void onGetFamilyName(int index, SkString* name) const override { m_fonts->getFamilyName(index, name); }
    sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override { return m_fonts->createStyleSet(index); }
    // Only the family's own fonts, or the best match's family for an alias
    // such as "monospace". Skia's own answer sorts every installed font,
    // checks each file can be read and loads every face of the family; here
    // the family's fonts come from fontconfig's cache, a match is asked only
    // for a family not installed, and only the face picked is loaded.
    sk_sp<SkFontStyleSet> onMatchFamily(const char family[]) const override
    {
        if (!family)
            return m_fonts->matchFamily(family);
        {
            std::lock_guard lock(m_mutex);
            if (const auto it = m_families.find(family); it != m_families.end())
                return it->second;
        }
        std::vector<ListedFont> fonts = listFamily(family);
        if (fonts.empty()) {
            const std::string name = matchedFamily(family);
            if (!name.empty())
                fonts = listFamily(name);
        }
        sk_sp<SkFontStyleSet> set;
        if (fonts.empty()) {
            set = matchFamilyFromAllFonts(family);
        } else {
            std::vector<SkFontStyle> styles;
            for (const ListedFont& font : fonts)
                styles.push_back(font.style);
            set = sk_make_sp<ListedFamilyStyleSet>(std::move(styles), [this, fonts = std::move(fonts)](int index) {
                return typefaceFor(fonts[size_t(index)]);
            });
        }
        std::lock_guard lock(m_mutex);
        m_families.emplace(family, set);
        return set;
    }

    // Skia's answer, kept to the family's own fonts.
    sk_sp<SkFontStyleSet> matchFamilyFromAllFonts(const char family[]) const
    {
        sk_sp<SkFontStyleSet> sorted = m_fonts->matchFamily(family);
        if (!sorted || !family)
            return sorted;
        std::vector<sk_sp<SkTypeface>> own;
        SkString first;
        std::vector<sk_sp<SkTypeface>> firstFamily;
        for (int i = 0; i < sorted->count(); ++i) {
            sk_sp<SkTypeface> typeface = sorted->createTypeface(i);
            if (!typeface)
                continue;
            SkString name;
            typeface->getFamilyName(&name);
            if (sameFamily(name, family))
                own.push_back(typeface);
            if (first.isEmpty())
                first = name;
            if (name.equals(first))
                firstFamily.push_back(typeface);
        }
        return sk_make_sp<FamilyStyleSet>(own.empty() ? std::move(firstFamily) : std::move(own));
    }

    // Each answer is a fontconfig match over every installed font (about
    // 1.5 ms with 3000 fonts), asked again for every layout of the same
    // font; misses are kept too.
    sk_sp<SkTypeface> onMatchFamilyStyle(const char family[], const SkFontStyle& style) const override
    {
        StyleKey key{family != nullptr, family ? family : "", style.weight(), style.width(), int(style.slant())};
        std::lock_guard lock(m_mutex);
        const auto found = m_styles.find(key);
        if (found != m_styles.end())
            return found->second;
        sk_sp<SkTypeface> typeface = m_fonts->matchFamilyStyle(family, style);
        if (m_styles.size() >= kMaximumFallbacks)
            m_styles.clear();
        m_styles.emplace(std::move(key), typeface);
        return typeface;
    }

    // Everything the answer depends on is in the key; misses are kept too.
    sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char family[], const SkFontStyle& style,
                                                  const char* bcp47[], int bcp47Count,
                                                  SkUnichar character) const override
    {
        std::string languages;
        for (int i = 0; i < bcp47Count; ++i)
            languages.append(bcp47[i]).push_back(',');
        Key key{family ? family : "", style.weight(), style.width(), int(style.slant()), languages, character};
        std::lock_guard lock(m_mutex);
        const auto found = m_fallbacks.find(key);
        if (found != m_fallbacks.end())
            return found->second;
        sk_sp<SkTypeface> typeface = m_fonts->matchFamilyStyleCharacter(family, style, bcp47, bcp47Count,
                                                                        character);
        if (m_fallbacks.size() >= kMaximumFallbacks)
            m_fallbacks.clear();
        m_fallbacks.emplace(std::move(key), typeface);
        return typeface;
    }

    sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int index) const override
    {
        return m_fonts->makeFromData(std::move(data), index);
    }

    sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream, int index) const override
    {
        return m_fonts->makeFromStream(std::move(stream), index);
    }

    sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream,
                                           const SkFontArguments& arguments) const override
    {
        return m_fonts->makeFromStream(std::move(stream), arguments);
    }

    sk_sp<SkTypeface> onMakeFromFile(const char path[], int index) const override
    {
        return m_fonts->makeFromFile(path, index);
    }

    sk_sp<SkTypeface> onLegacyMakeTypeface(const char family[], SkFontStyle style) const override
    {
        return m_fonts->legacyMakeTypeface(family, style);
    }

private:
    using Key = std::tuple<std::string, int, int, int, std::string, SkUnichar>;
    using StyleKey = std::tuple<bool, std::string, int, int, int>;

    struct ListedFont
    {
        SkFontStyle style;
        std::string family;
        std::string file;
        int index = 0;
    };

    // The static fonts whose first family is the one named.
    static std::vector<ListedFont> listFamily(const std::string& family)
    {
        FcPattern* pattern = FcPatternCreate();
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family.c_str()));
        FcPatternAddBool(pattern, FC_VARIABLE, FcFalse);
        FcObjectSet* objects = FcObjectSetBuild(FC_FAMILY, FC_FILE, FC_INDEX, FC_WEIGHT, FC_WIDTH, FC_SLANT, nullptr);
        FcFontSet* fonts = FcFontList(nullptr, pattern, objects);
        FcObjectSetDestroy(objects);
        FcPatternDestroy(pattern);
        std::vector<ListedFont> list;
        for (int i = 0; fonts && i < fonts->nfont; ++i) {
            FcPattern* font = fonts->fonts[i];
            FcChar8* first = nullptr;
            FcChar8* file = nullptr;
            if (FcPatternGetString(font, FC_FAMILY, 0, &first) != FcResultMatch
                || strcasecmp(reinterpret_cast<const char*>(first), family.c_str()) != 0
                || FcPatternGetString(font, FC_FILE, 0, &file) != FcResultMatch)
                continue;
            list.push_back({styleFromPattern(font), reinterpret_cast<const char*>(first),
                            reinterpret_cast<const char*>(file), patternInteger(font, FC_INDEX, 0)});
        }
        if (fonts)
            FcFontSetDestroy(fonts);
        return list;
    }

    // The family fontconfig answers with, for an alias or a family that is
    // not installed.
    static std::string matchedFamily(const char family[])
    {
        FcPattern* pattern = FcPatternCreate();
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8*>(family));
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result = FcResultNoMatch;
        FcPattern* match = FcFontMatch(nullptr, pattern, &result);
        FcPatternDestroy(pattern);
        if (!match)
            return {};
        FcChar8* name = nullptr;
        std::string matched;
        if (FcPatternGetString(match, FC_FAMILY, 0, &name) == FcResultMatch)
            matched = reinterpret_cast<const char*>(name);
        FcPatternDestroy(match);
        return matched;
    }

    // Made from the file without another match when it reports what
    // fontconfig's own typeface would: the shaper fakes bold and italic by
    // the style, and terminals look fonts up by the family. Named instances
    // of variable fonts need fontconfig's, which knows their axes.
    sk_sp<SkTypeface> typefaceFor(const ListedFont& font) const
    {
        const std::pair<std::string, int> key(font.file, font.index);
        {
            std::lock_guard lock(m_mutex);
            if (const auto it = m_typefaces.find(key); it != m_typefaces.end())
                return it->second;
        }
        sk_sp<SkTypeface> typeface;
        if (font.index <= 0xffff)
            typeface = m_fonts->makeFromFile(font.file.c_str(), font.index);
        if (typeface) {
            SkString name;
            typeface->getFamilyName(&name);
            if (!(typeface->fontStyle() == font.style) || !name.equals(font.family.c_str()))
                typeface.reset();
        }
        if (!typeface)
            typeface = m_fonts->matchFamilyStyle(font.family.c_str(), font.style);
        std::lock_guard lock(m_mutex);
        m_typefaces.emplace(key, typeface);
        return typeface;
    }

    sk_sp<SkFontMgr> m_fonts;
    mutable std::mutex m_mutex;
    mutable std::map<Key, sk_sp<SkTypeface>> m_fallbacks;
    mutable std::map<StyleKey, sk_sp<SkTypeface>> m_styles;
    mutable std::map<std::string, sk_sp<SkFontStyleSet>> m_families;
    mutable std::map<std::pair<std::string, int>, sk_sp<SkTypeface>> m_typefaces;
};

} // namespace

static std::string localeName(const std::string& ctypeLocale)
{
    std::string locale = ctypeLocale;
    locale = locale.substr(0, locale.find_first_of(".@"));
    if (locale == "C" || locale == "POSIX")
        return {};
    for (char& c : locale) {
        if (c == '_')
            c = '-';
    }
    return locale;
}

TextContext::TextContext(const std::string& ctypeLocale)
    : m_locale(localeName(ctypeLocale))
{
    // Skia's font manager lists every family when it is made, comparing each
    // font's families with all names found so far (about 30 ms with 3000
    // fonts); it gets the configuration before the fonts are read, so the
    // list stays empty, and nothing here enumerates families through it. The
    // same configuration becomes the current one, which the other fontconfig
    // calls would otherwise load again.
    FcConfig* config = FcInitLoadConfig();
    if (config) {
        m_fontManager = sk_make_sp<FallbackCachingFontMgr>(
            SkFontMgr_New_FontConfig(FcConfigReference(config), SkFontScanner_Make_FreeType()));
        FcConfigBuildFonts(config);
        FcConfigSetCurrent(config);
        FcConfigDestroy(config);
    } else {
        m_fontManager = sk_make_sp<FallbackCachingFontMgr>(
            SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType()));
    }
    m_fontCollection = sk_make_sp<skia::textlayout::FontCollection>();
    m_fontCollection->setDefaultFontManager(m_fontManager);
    m_fontCollection->enableFontFallback();
    // Lines wrap by line_breaks.h, also after breakable white space; ICU's
    // rules keep "WireGuard .conf" together.
    m_unicode = makeLineBreakUnicode(SkUnicodes::ICU::Make());
    readFontconfigDefaults();
}

TextContext::~TextContext() = default;

void TextContext::clearFontCaches()
{
    static_cast<FallbackCachingFontMgr*>(m_fontManager.get())->clear();
    m_fontCollection->clearCaches();
}

void TextContext::setLocale(const std::string& ctypeLocale)
{
    const std::string locale = localeName(ctypeLocale);
    if (locale == m_locale)
        return;
    m_locale = locale;
    clearFontCaches();
}

double TextContext::pixelSize(double logicalSize)
{
    return std::round(std::floor(logicalSize * 100 + 0.5) / 100);
}

skia::textlayout::TextStyle TextContext::style(double fontSize, int weight, Color color,
                                               const std::string& family) const
{
    const Theme& theme = Theme::current();
    const std::string& themeFamily = theme.fontFamily.empty() ? m_family : theme.fontFamily;
    skia::textlayout::TextStyle style;
    if (family.empty())
        style.setFontFamilies({SkString(themeFamily.c_str())});
    else
        style.setFontFamilies({SkString(family.c_str()), SkString(themeFamily.c_str())});
    style.setFontSize(SkScalar(pixelSize(fontSize > 0 ? fontSize : theme.fontSize())));
    style.setFontStyle(SkFontStyle(weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant));
    style.setColor(color.argb());
    style.setFontEdging(m_edging);
    style.setFontHinting(m_hinting);
    style.setSubpixel(m_subpixelPositioning);
    if (!m_locale.empty())
        style.setLocale(SkString(m_locale.c_str()));
    return style;
}

sk_sp<SkTypeface> TextContext::typeface(int weight, const std::string& family) const
{
    const Theme& theme = Theme::current();
    const std::string& themeFamily = theme.fontFamily.empty() ? m_family : theme.fontFamily;
    const SkFontStyle style(weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant);
    std::vector<sk_sp<SkTypeface>> typefaces;
    if (!family.empty())
        typefaces = m_fontCollection->findTypefaces({SkString(family.c_str())}, style);
    if (typefaces.empty())
        typefaces = m_fontCollection->findTypefaces({SkString(themeFamily.c_str())}, style);
    if (typefaces.empty())
        typefaces = m_fontCollection->findTypefaces({SkString(m_family.c_str())}, SkFontStyle());
    return typefaces.empty() ? nullptr : typefaces.front();
}

static int16_t readInt16(const std::vector<uint8_t>& table, size_t offset)
{
    return int16_t(uint16_t(table[offset] << 8 | table[offset + 1]));
}

static uint16_t readUInt16(const std::vector<uint8_t>& table, size_t offset)
{
    return uint16_t(table[offset] << 8 | table[offset + 1]);
}

static std::vector<uint8_t> tableData(const SkTypeface* typeface, const char* tag)
{
    const SkFontTableTag value = SkSetFourByteTag(tag[0], tag[1], tag[2], tag[3]);
    std::vector<uint8_t> data(typeface->getTableSize(value));
    if (!data.empty())
        typeface->getTableData(value, 0, data.size(), data.data());
    return data;
}

// The hhea and OS/2 line metrics, in font units.
const TextContext::Tables& TextContext::tables(const SkTypeface* typeface) const
{
    std::lock_guard lock(m_tablesMutex);
    Tables& tables = m_tables[typeface->uniqueID()];
    if (tables.valid)
        return tables;
    tables.valid = true;
    tables.unitsPerEm = std::max(1, typeface->getUnitsPerEm());

    const std::vector<uint8_t> hhea = tableData(typeface, "hhea");
    if (hhea.size() >= 10 && (readInt16(hhea, 4) != 0 || readInt16(hhea, 6) != 0)) {
        tables.ascent = readInt16(hhea, 4);
        tables.descent = -readInt16(hhea, 6);
    }
    const std::vector<uint8_t> os2 = tableData(typeface, "OS/2");
    if (os2.size() >= 78) {
        const uint16_t selection = readUInt16(os2, 62);
        // USE_TYPO_METRICS.
        if (selection & 0x80) {
            if (readInt16(os2, 68) != 0 || readInt16(os2, 70) != 0) {
                tables.ascent = readInt16(os2, 68);
                tables.descent = -readInt16(os2, 70);
            }
        } else if (readUInt16(os2, 74) != 0 || readUInt16(os2, 76) != 0) {
            tables.ascent = readUInt16(os2, 74);
            tables.descent = readUInt16(os2, 76);
        }
        if (os2.size() >= 90) {
            tables.xHeight = readInt16(os2, 86);
            tables.capHeight = readInt16(os2, 88);
            tables.hasCapHeight = readUInt16(os2, 0) >= 2;
        }
    }
    if (tables.ascent == 0 && tables.descent == 0) {
        SkFont font(sk_ref_sp(const_cast<SkTypeface*>(typeface)), SkScalar(tables.unitsPerEm));
        SkFontMetrics metrics;
        font.getMetrics(&metrics);
        tables.ascent = -metrics.fAscent;
        tables.descent = metrics.fDescent;
    }
    return tables;
}

// 26.6 fixed point, converted from reals by truncation and divided with
// rounding.
static int64_t fixedFromReal(double value)
{
    return int64_t(value * 64);
}

static int64_t fixedDivide(int64_t a, int64_t b)
{
    const bool negative = (a < 0) != (b < 0);
    const int64_t result = ((std::abs(a) << 6) + std::abs(b) / 2) / std::abs(b);
    return negative ? -result : result;
}

static int64_t fixedRound(int64_t value)
{
    return (value + 32) & -64;
}

// FreeType's FT_MulFix and FT_DivFix.
static int64_t mulFix(int64_t a, int64_t b)
{
    const bool negative = (a < 0) != (b < 0);
    const int64_t result = (std::abs(a) * std::abs(b) + 0x8000) >> 16;
    return negative ? -result : result;
}

static int64_t divFix(int64_t a, int64_t b)
{
    return ((a << 16) + b / 2) / b;
}

bool TextContext::roundsMetrics(double scale, bool hintedLayout) const
{
    return scale == 1 && hintedLayout && (m_fontconfigHinting == SkFontHinting::kNormal || m_fontconfigHinting == SkFontHinting::kFull);
}

VerticalMetrics TextContext::metrics(const SkTypeface* typeface, double pixelSize, double scale,
                                   bool hintedLayout) const
{
    VerticalMetrics result;
    if (!typeface)
        return result;
    const Tables& t = tables(typeface);
    const int64_t unitsPerEm = int64_t(t.unitsPerEm) * 64;
    int64_t ascent = fixedDivide(fixedFromReal(t.ascent * pixelSize), unitsPerEm);
    int64_t descent = fixedDivide(fixedFromReal(t.descent * pixelSize), unitsPerEm);
    if (roundsMetrics(scale, hintedLayout)) {
        ascent = fixedRound(ascent);
        descent = fixedRound(descent);
    }
    result.ascent = ascent / 64.0;
    result.descent = descent / 64.0;
    const int ppem = int(std::lround(pixelSize));
    if (t.xHeight > 0)
        result.xHeight = fixedDivide(int64_t(t.xHeight) * ppem * 64, unitsPerEm) / 64.0;
    if (t.hasCapHeight) {
        const int64_t yScale = divFix(int64_t(ppem) * 64, t.unitsPerEm);
        result.capHeight = mulFix(t.capHeight, yScale) / 64.0;
    }
    return result;
}

void TextContext::readFontconfigDefaults()
{
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>("sans-serif"));
    if (!m_locale.empty())
        FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8*>(m_locale.c_str()));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match)
        return;

    FcChar8* family = nullptr;
    if (FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch)
        m_family = reinterpret_cast<const char*>(family);

    FcBool antialias = FcTrue;
    FcPatternGetBool(match, FC_ANTIALIAS, 0, &antialias);
    int rgba = FC_RGBA_UNKNOWN;
    FcPatternGetInteger(match, FC_RGBA, 0, &rgba);
    FcBool hinting = FcTrue;
    FcPatternGetBool(match, FC_HINTING, 0, &hinting);
    int hintStyle = FC_HINT_SLIGHT;
    FcPatternGetInteger(match, FC_HINT_STYLE, 0, &hintStyle);
    FcPatternDestroy(match);

    switch (rgba) {
    case FC_RGBA_RGB:
        m_pixelGeometry = kRGB_H_SkPixelGeometry;
        break;
    case FC_RGBA_BGR:
        m_pixelGeometry = kBGR_H_SkPixelGeometry;
        break;
    case FC_RGBA_VRGB:
        m_pixelGeometry = kRGB_V_SkPixelGeometry;
        break;
    case FC_RGBA_VBGR:
        m_pixelGeometry = kBGR_V_SkPixelGeometry;
        break;
    default:
        m_pixelGeometry = kUnknown_SkPixelGeometry;
        break;
    }

    if (!antialias)
        m_edging = SkFont::Edging::kAlias;
    else if (m_pixelGeometry != kUnknown_SkPixelGeometry)
        m_edging = SkFont::Edging::kSubpixelAntiAlias;
    else
        m_edging = SkFont::Edging::kAntiAlias;

    if (!hinting) {
        m_fontconfigHinting = SkFontHinting::kNone;
    } else {
        switch (hintStyle) {
        case FC_HINT_NONE:
            m_fontconfigHinting = SkFontHinting::kNone;
            break;
        case FC_HINT_SLIGHT:
            m_fontconfigHinting = SkFontHinting::kSlight;
            break;
        case FC_HINT_MEDIUM:
            m_fontconfigHinting = SkFontHinting::kNormal;
            break;
        default:
            m_fontconfigHinting = SkFontHinting::kFull;
            break;
        }
    }
    // Horizontal hinting would round the advances, which stay unhinted;
    // vertical hinting keeps baselines and stems crisp.
    m_hinting = std::min(m_fontconfigHinting, SkFontHinting::kSlight);
    m_subpixelPositioning = true;
}

} // namespace cutefish::ui
