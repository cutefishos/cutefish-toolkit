// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/font_file.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fontconfig/fontconfig.h>
#include <optional>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontScanner.h"
#include "include/core/SkPaint.h"
#include "include/core/SkStream.h"
#include "include/core/SkTypeface.h"
#include "include/ports/SkFontScanner_FreeType.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skparagraph/include/TypefaceFontProvider.h"

#include "base/strings.h"
#include "ui/application.h"
#include "ui/painter.h"
#include "ui/text_context.h"
#include "ui/text_lines.h"

namespace textlayout = skia::textlayout;

namespace cutefish::ui {

static constexpr double kUnboundedWidth = 1e6;
static constexpr char kSampleFamily[] = "sample";
static constexpr size_t kCachedLines = 32;

// A name comes once for each language the file gives it in.
static std::string englishString(FcPattern* pattern, const char* object, const char* languageObject)
{
    std::string first;
    FcChar8* value = nullptr;
    for (int i = 0; FcPatternGetString(pattern, object, i, &value) == FcResultMatch; ++i) {
        FcChar8* language = nullptr;
        if (FcPatternGetString(pattern, languageObject, i, &language) == FcResultMatch
            && std::string_view(reinterpret_cast<const char*>(language)) == "en")
            return reinterpret_cast<const char*>(value);
        if (i == 0)
            first = reinterpret_cast<const char*>(value);
    }
    return first;
}

static std::vector<std::pair<char32_t, char32_t>> ranges(const FcCharSet* characters)
{
    std::vector<std::pair<char32_t, char32_t>> result;
    FcChar32 map[FC_CHARSET_MAP_SIZE];
    FcChar32 next = 0;
    for (FcChar32 page = FcCharSetFirstPage(characters, map, &next); page != FC_CHARSET_DONE;
         page = FcCharSetNextPage(characters, map, &next)) {
        for (int word = 0; word < FC_CHARSET_MAP_SIZE; ++word) {
            for (int bit = 0; bit < 32; ++bit) {
                if (!(map[word] & (FcChar32(1) << bit)))
                    continue;
                const char32_t character = page + char32_t(word * 32 + bit);
                if (!result.empty() && result.back().second + 1 == character)
                    result.back().second = character;
                else
                    result.emplace_back(character, character);
            }
        }
    }
    return result;
}

bool FontFace::covers(char32_t character) const
{
    const auto range = std::upper_bound(coverage.begin(), coverage.end(), character,
                                        [](char32_t value, const auto& entry) { return value < entry.first; });
    return range != coverage.begin() && character <= std::prev(range)->second;
}

bool FontFace::covers(std::string_view text) const
{
    const std::u32string characters = base::strings::utf8ToUtf32(text);
    return std::all_of(characters.begin(), characters.end(), [this](char32_t character) { return covers(character); });
}

std::string FontFace::characters(char32_t first, char32_t last, int count) const
{
    std::string result;
    for (const auto& [start, end] : coverage) {
        if (end < first)
            continue;
        for (char32_t character = std::max(start, first); character <= std::min(end, last) && count > 0; ++character) {
            base::strings::appendUtf8(result, character);
            --count;
        }
        if (start > last || count <= 0)
            break;
    }
    return result;
}

std::vector<FontFace> readFontFaces(const std::string& path)
{
    std::vector<FontFace> faces;
    std::vector<bool> variable;
    FcFontSet* set = FcFontSetCreate();
    int count = 0;
    FcFreeTypeQueryAll(reinterpret_cast<const FcChar8*>(path.c_str()), static_cast<unsigned int>(-1), nullptr, &count,
                       set);
    for (int i = 0; i < set->nfont; ++i) {
        FcPattern* pattern = set->fonts[i];
        FcChar8* format = nullptr;
        if (FcPatternGetString(pattern, FC_FONTFORMAT, 0, &format) != FcResultMatch)
            continue;
        const std::string_view formatName = reinterpret_cast<const char*>(format);
        if (formatName != "TrueType" && formatName != "CFF")
            continue;

        FontFace face;
        FcPatternGetInteger(pattern, FC_INDEX, 0, &face.index);
        face.family = englishString(pattern, FC_FAMILY, FC_FAMILYLANG);
        face.style = englishString(pattern, FC_STYLE, FC_STYLELANG);
        FcChar8* name = nullptr;
        if (FcPatternGetString(pattern, FC_POSTSCRIPT_NAME, 0, &name) == FcResultMatch)
            face.postScriptName = reinterpret_cast<const char*>(name);
        FcPatternGetInteger(pattern, FC_FONTVERSION, 0, &face.revision);
        double weight = FC_WEIGHT_REGULAR;
        FcPatternGetDouble(pattern, FC_WEIGHT, 0, &weight);
        face.weight = int(std::lround(FcWeightToOpenTypeDouble(weight)));
        int slant = FC_SLANT_ROMAN;
        FcPatternGetInteger(pattern, FC_SLANT, 0, &slant);
        face.italic = slant != FC_SLANT_ROMAN;
        FcCharSet* characters = nullptr;
        if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &characters) == FcResultMatch)
            face.coverage = ranges(characters);

        // The pattern for a variable font as a whole, with ranges for its axes.
        FcBool whole = FcFalse;
        FcPatternGetBool(pattern, FC_VARIABLE, 0, &whole);
        variable.push_back(whole == FcTrue);
        faces.push_back(std::move(face));
    }
    FcFontSetDestroy(set);

    // Named instances stand for a variable font; one without any keeps its whole pattern.
    if (std::find(variable.begin(), variable.end(), false) != variable.end()) {
        std::vector<FontFace> instances;
        for (size_t i = 0; i < faces.size(); ++i) {
            if (!variable[i])
                instances.push_back(std::move(faces[i]));
        }
        faces = std::move(instances);
    }
    return faces;
}

int defaultFontFace(const std::vector<FontFace>& faces)
{
    int best = 0;
    int bestDistance = -1;
    for (size_t i = 0; i < faces.size(); ++i) {
        const int distance = std::abs(faces[i].weight - 400) + (faces[i].italic ? 1000 : 0);
        if (bestDistance < 0 || distance < bestDistance) {
            best = int(i);
            bestDistance = distance;
        }
    }
    return best;
}

std::vector<std::string> installedFontFiles(const FontFace& face)
{
    std::vector<std::string> files;
    if (face.postScriptName.empty())
        return files;
    FcConfig* config = FcInitLoadConfigAndFonts();
    if (!config)
        return files;
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_POSTSCRIPT_NAME, reinterpret_cast<const FcChar8*>(face.postScriptName.c_str()));
    FcObjectSet* objects = FcObjectSetBuild(FC_FILE, FC_FONTVERSION, nullptr);
    FcFontSet* fonts = FcFontList(config, pattern, objects);
    FcObjectSetDestroy(objects);
    FcPatternDestroy(pattern);
    if (fonts) {
        for (int i = 0; i < fonts->nfont; ++i) {
            int revision = 0;
            FcPatternGetInteger(fonts->fonts[i], FC_FONTVERSION, 0, &revision);
            FcChar8* file = nullptr;
            if (revision != face.revision || FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
                continue;
            const std::string path = reinterpret_cast<const char*>(file);
            if (std::find(files.begin(), files.end(), path) == files.end())
                files.push_back(path);
        }
        FcFontSetDestroy(fonts);
    }
    FcConfigDestroy(config);
    return files;
}

Bitmap renderFontGlyphs(const std::string& path, int index, const std::string& text, double pixelSize, Color color)
{
    std::unique_ptr<SkStreamAsset> stream = SkStream::MakeFromFile(path.c_str());
    if (!stream)
        return {};
    const sk_sp<SkTypeface> typeface = SkFontScanner_Make_FreeType()->MakeFromStream(
            std::move(stream), SkFontArguments().setCollectionIndex(index));
    if (!typeface)
        return {};

    SkFont font(typeface, SkScalar(pixelSize));
    font.setEdging(SkFont::Edging::kAntiAlias);
    font.setHinting(SkFontHinting::kNone);
    font.setSubpixel(true);
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(color.argb());

    SkRect ink = SkRect::MakeEmpty();
    font.measureText(text.data(), text.size(), SkTextEncoding::kUTF8, &ink, &paint);
    if (ink.isEmpty())
        return {};
    const SkIRect bounds = ink.roundOut();
    Bitmap bitmap(bounds.width(), bounds.height(), Bitmap::Format::Argb32Premultiplied);
    if (bitmap.isNull())
        return {};
    SkBitmap target;
    if (!target.installPixels(SkImageInfo::Make(bitmap.width(), bitmap.height(), kBGRA_8888_SkColorType,
                                                kPremul_SkAlphaType),
                              bitmap.bits(), bitmap.stride()))
        return {};
    SkCanvas canvas(target);
    canvas.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, SkScalar(-bounds.left()),
                          SkScalar(-bounds.top()), font, paint);
    return bitmap;
}

struct FontSample::Private
{
    struct Line
    {
        std::string text;
        double size = 0;
        uint32_t color = 0;
        std::unique_ptr<textlayout::Paragraph> paragraph;
    };

    // Any colour when none is given, as for measuring.
    textlayout::Paragraph* line(const std::string& text, double size, std::optional<Color> color);

    sk_sp<SkTypeface> typeface;
    sk_sp<textlayout::FontCollection> collection;
    std::vector<Line> lines;
};

textlayout::Paragraph* FontSample::Private::line(const std::string& text, double size, std::optional<Color> color)
{
    const uint32_t argb = color.value_or(Color()).argb();
    for (const Line& line : lines) {
        if (line.text == text && line.size == size && (!color || line.color == argb))
            return line.paragraph.get();
    }

    const TextContext& context = Application::instance()->text();
    textlayout::TextStyle style;
    style.setFontFamilies({SkString(kSampleFamily)});
    style.setFontStyle(typeface->fontStyle());
    style.setFontSize(SkScalar(TextContext::pixelSize(size)));
    style.setColor(argb);
    style.setFontEdging(context.edging());
    style.setFontHinting(context.hinting());
    style.setSubpixel(context.subpixelPositioning());
    textlayout::ParagraphStyle paragraphStyle;
    paragraphStyle.setTextStyle(style);
    auto builder = textlayout::ParagraphBuilder::make(paragraphStyle, collection, context.unicode());
    builder->pushStyle(style);
    builder->addText(text.data(), text.size());
    builder->pop();
    std::unique_ptr<textlayout::Paragraph> paragraph = builder->Build();
    paragraph->layout(SkScalar(kUnboundedWidth));

    if (lines.size() >= kCachedLines)
        lines.clear();
    lines.push_back({text, size, argb, std::move(paragraph)});
    return lines.back().paragraph.get();
}

FontSample::FontSample(const std::string& path, int index)
    : d(std::make_unique<Private>())
{
    d->typeface = Application::instance()->text().typefaceFromFile(path, index);
    if (!d->typeface)
        return;
    auto provider = sk_make_sp<textlayout::TypefaceFontProvider>();
    provider->registerTypeface(d->typeface, SkString(kSampleFamily));
    d->collection = sk_make_sp<textlayout::FontCollection>();
    d->collection->setAssetFontManager(std::move(provider));
    d->collection->disableFontFallback();
}

FontSample::~FontSample() = default;

bool FontSample::isValid() const
{
    return d->typeface != nullptr;
}

SizeF FontSample::measure(const std::string& text, double size) const
{
    if (!isValid() || text.empty())
        return {};
    textlayout::Paragraph* paragraph = d->line(text, size, std::nullopt);
    return {std::ceil(double(paragraph->getMaxIntrinsicWidth())), std::ceil(double(paragraph->getHeight()))};
}

void FontSample::draw(Painter& painter, const std::string& text, double size, PointF position, Color color) const
{
    if (!isValid() || text.empty())
        return;
    textlayout::Paragraph* paragraph = d->line(text, size, color);
    const double scale = painter.scale();
    const PointF offset = painter.offset();
    SkCanvas* canvas = painter.canvas();
    canvas->save();
    canvas->translate(SkScalar(std::round((offset.x + position.x) * scale)),
                      SkScalar(std::round((offset.y + position.y) * scale)));
    canvas->scale(SkScalar(scale), SkScalar(scale));
    paintParagraph(canvas, paragraph, 0, 0);
    canvas->restore();
}

} // namespace cutefish::ui
