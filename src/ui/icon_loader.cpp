// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/icon_loader.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "include/codec/SkCodec.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkData.h"
#include "include/core/SkPaint.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "modules/skresources/include/SkResources.h"
#include "modules/svg/include/SkSVGDOM.h"
#include "modules/svg/include/SkSVGSVG.h"

#include "base/file.h"
#include "base/paths.h"
#include "base/thread_pool.h"
#include "ui/embedded_files.h"
#include "ui/image_formats.h"
#include "ui/theme.h"

namespace cutefish::ui {

// Enough for every icon a few windows show at two scales.
static constexpr size_t kCacheLimit = 512;
static const char* const kExtensions[] = {".png", ".svg"};
// ICNS application icons put their plate 824 px wide on a 1024 px canvas; the
// icon theme's plates are 56 of 64. The picture grows by the ratio, around its
// centre, so both plates come out alike.
static constexpr double kIcnsScale = (56.0 / 64) / (824.0 / 1024);
// The icon theme's plate, drawn under applications' own icons from outside
// the theme; their visible part gets as wide as the theme's logos on it.
static constexpr const char* kPlateName = "cutefish-application-plate";
static constexpr double kPlateContent = 44.0 / 64;
static constexpr double kPlateExtent = 56.0 / 64;
// A solid part this square, solid along all four sides outside its corners,
// is a plate of the icon's own, drawn as large as the theme's plate instead of
// on it; round and shaped logos leave at least 6% of that area clear.
static constexpr double kOwnPlateAspect = 1.02;
static constexpr double kOwnPlateCorner = 0.25;
static constexpr double kOwnPlateFill = 0.99;
// Coverage below this is a soft shadow or glow, not the visible part.
static constexpr uint8_t kInkAlpha = 16;
// The drawing size an SVG icon's visible part is measured at.
static constexpr int kInkSize = 256;

// Where icon themes live, first match first.
static std::vector<std::string> iconBases()
{
    std::vector<std::string> bases{base::paths::home() + "/.icons", base::paths::dataHome() + "/icons"};
    for (const std::string& directory : base::paths::dataDirectories())
        bases.push_back(directory + "/icons");
    return bases;
}

static std::string trim(const std::string& text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    const size_t end = text.find_last_not_of(" \t\r\n");
    return start == std::string::npos ? std::string() : text.substr(start, end - start + 1);
}

static std::vector<std::string> split(const std::string& text)
{
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        part = trim(part);
        if (!part.empty())
            parts.push_back(part);
    }
    return parts;
}

// The value of an attribute in a start tag's attribute text, and where it is.
static std::optional<std::pair<size_t, size_t>> attributeValue(const std::string& attributes, std::string_view name)
{
    for (size_t at = attributes.find(name); at != std::string::npos; at = attributes.find(name, at + 1)) {
        if (at > 0 && !std::isspace(static_cast<unsigned char>(attributes[at - 1])))
            continue;
        size_t i = at + name.size();
        while (i < attributes.size() && std::isspace(static_cast<unsigned char>(attributes[i])))
            ++i;
        if (i >= attributes.size() || attributes[i] != '=')
            continue;
        ++i;
        while (i < attributes.size() && std::isspace(static_cast<unsigned char>(attributes[i])))
            ++i;
        if (i >= attributes.size() || (attributes[i] != '"' && attributes[i] != '\''))
            continue;
        const size_t end = attributes.find(attributes[i], i + 1);
        if (end == std::string::npos)
            return std::nullopt;
        return std::pair{i + 1, end};
    }
    return std::nullopt;
}

// The bounds of path data made of straight lines only (M, L, H, V, Z, in
// either case); nothing for curves or arcs.
static std::optional<SkRect> lineBounds(const std::string& data)
{
    std::optional<SkRect> bounds;
    double x = 0;
    double y = 0;
    double startX = 0;
    double startY = 0;
    char command = 0;
    size_t i = 0;
    const auto skip = [&] {
        while (i < data.size() && (std::isspace(static_cast<unsigned char>(data[i])) || data[i] == ','))
            ++i;
    };
    const auto number = [&](double& out) {
        skip();
        const char* begin = data.c_str() + i;
        char* end = nullptr;
        out = std::strtod(begin, &end);
        if (end == begin)
            return false;
        i += size_t(end - begin);
        return true;
    };
    // By hand: SkRect::join skips the empty rect a point is.
    const auto add = [&] {
        const SkScalar px = SkScalar(x);
        const SkScalar py = SkScalar(y);
        if (!bounds)
            bounds = SkRect::MakeLTRB(px, py, px, py);
        else
            bounds->setLTRB(std::min(bounds->left(), px), std::min(bounds->top(), py), std::max(bounds->right(), px),
                            std::max(bounds->bottom(), py));
    };
    while (true) {
        skip();
        if (i >= data.size())
            break;
        if (std::isalpha(static_cast<unsigned char>(data[i])))
            command = data[i++];
        const bool relative = std::islower(static_cast<unsigned char>(command));
        double a = 0;
        double b = 0;
        switch (std::tolower(static_cast<unsigned char>(command))) {
        case 'm':
        case 'l':
            if (!number(a) || !number(b))
                return std::nullopt;
            x = relative ? x + a : a;
            y = relative ? y + b : b;
            if (std::tolower(static_cast<unsigned char>(command)) == 'm') {
                startX = x;
                startY = y;
                // Further pairs are lines.
                command = relative ? 'l' : 'L';
            }
            break;
        case 'h':
            if (!number(a))
                return std::nullopt;
            x = relative ? x + a : a;
            break;
        case 'v':
            if (!number(a))
                return std::nullopt;
            y = relative ? y + a : a;
            break;
        case 'z':
            x = startX;
            y = startY;
            break;
        default:
            return std::nullopt;
        }
        add();
    }
    return bounds;
}

// What Skia's SVG renderer leaves out: <style> rules for
// class selectors, put into each element's style attribute ahead of its own
// declarations, and base64 data URIs broken over lines.
static std::string prepareSvg(std::string svg)
{
    for (size_t at = svg.find("data:"); at != std::string::npos; at = svg.find("data:", at + 5)) {
        const size_t end = svg.find_first_of("\"'", at);
        if (end == std::string::npos)
            break;
        std::string value = svg.substr(at, end - at);
        std::erase_if(value, [](char c) { return std::isspace(static_cast<unsigned char>(c)); });
        svg.replace(at, end - at, value);
    }

    // Skia lays a pattern out in user space whatever its units; one in the
    // bounding box units of the rect it fills gets that box as its transform.
    for (size_t at = svg.find("<pattern"); at != std::string::npos; at = svg.find("<pattern", at + 1)) {
        size_t end = svg.find('>', at);
        if (end == std::string::npos)
            break;
        std::string tag = svg.substr(at, end - at);
        const auto value = [&tag](std::string_view name) {
            const auto found = attributeValue(tag, name);
            return found ? tag.substr(found->first, found->second - found->first) : std::string();
        };
        const std::string units = value("patternUnits");
        if (value("patternContentUnits") != "objectBoundingBox" || (!units.empty() && units != "objectBoundingBox")
            || !value("patternTransform").empty() || value("id").empty())
            continue;
        const size_t use = svg.find("url(#" + value("id") + ")");
        const size_t element = use == std::string::npos ? std::string::npos : svg.rfind('<', use);
        if (element == std::string::npos || svg.find('>', element) < use)
            continue;
        const std::string filler = svg.substr(element, svg.find('>', element) - element);
        std::optional<SkRect> box;
        if (filler.starts_with("<rect")) {
            const auto number = [&filler](std::string_view name, double fallback) {
                const auto found = attributeValue(filler, name);
                return found ? std::atof(filler.substr(found->first, found->second - found->first).c_str()) : fallback;
            };
            box = SkRect::MakeXYWH(SkScalar(number("x", 0)), SkScalar(number("y", 0)), SkScalar(number("width", 0)),
                                   SkScalar(number("height", 0)));
        } else if (filler.starts_with("<path")) {
            // Colloid's shadows fill a rectangle drawn as a path; as a rect it
            // takes the transform like one (Skia's own box for a path gives
            // a one-pixel tile, a uniform grey).
            const auto d = attributeValue(filler, "d");
            if (d) {
                box = lineBounds(filler.substr(d->first, d->second - d->first));
                if (box) {
                    char geometry[160];
                    snprintf(geometry, sizeof(geometry), "x=\"%g\" y=\"%g\" width=\"%g\" height=\"%g\"",
                             double(box->x()), double(box->y()), double(box->width()), double(box->height()));
                    std::string rect = filler;
                    // From the attribute's name to its closing quote.
                    const size_t name = rect.rfind(" d=", d->first);
                    rect.replace(name + 1, d->second + 1 - (name + 1), geometry);
                    rect.replace(0, 5, "<rect");
                    svg.replace(element, filler.size(), rect);
                    // The pattern may come after the element.
                    if (element < at) {
                        at += rect.size() - filler.size();
                        end += rect.size() - filler.size();
                    }
                }
            }
        }
        if (!box || box->width() <= 0 || box->height() <= 0)
            continue;
        char transform[160];
        snprintf(transform, sizeof(transform), " patternTransform=\"matrix(%g 0 0 %g %g %g)\"", double(box->width()),
                 double(box->height()), double(box->x()), double(box->y()));
        svg.insert(end, transform);
    }

    std::unordered_map<std::string, std::string> rules;
    for (size_t at = svg.find("<style"); at != std::string::npos; at = svg.find("<style", at + 1)) {
        const size_t open = svg.find('>', at);
        const size_t close = svg.find("</style>", at);
        if (open == std::string::npos || close == std::string::npos)
            break;
        std::string css = svg.substr(open + 1, close - open - 1);
        for (const char* marker : {"<![CDATA[", "]]>"}) {
            if (const size_t found = css.find(marker); found != std::string::npos)
                css.erase(found, std::strlen(marker));
        }
        size_t from = 0;
        for (size_t brace = css.find('{'); brace != std::string::npos; brace = css.find('{', from)) {
            const size_t end = css.find('}', brace);
            if (end == std::string::npos)
                break;
            // Skia stops at an empty declaration, so they are joined without any.
            std::string declarations;
            std::stringstream parts(css.substr(brace + 1, end - brace - 1));
            std::string part;
            while (std::getline(parts, part, ';')) {
                part = trim(part);
                if (!part.empty())
                    declarations += part + ";";
            }
            std::stringstream selectors(css.substr(from, brace - from));
            std::string selector;
            while (std::getline(selectors, selector, ',')) {
                selector = trim(selector);
                const bool simpleClass = selector.size() > 1 && selector[0] == '.'
                        && selector.find_first_of(" .>+~:[#\t\n", 1) == std::string::npos;
                if (simpleClass)
                    rules[selector.substr(1)] += declarations;
            }
            from = end + 1;
        }
    }
    if (rules.empty())
        return svg;

    std::string out;
    out.reserve(svg.size());
    size_t from = 0;
    for (size_t tag = svg.find('<'); tag != std::string::npos; tag = svg.find('<', from)) {
        const size_t end = svg.find('>', tag);
        if (end == std::string::npos)
            break;
        std::string element = svg.substr(tag, end - tag);
        out.append(svg, from, tag - from);
        from = end;
        const auto classes = attributeValue(element, "class");
        if (element.size() < 2 || !std::isalpha(static_cast<unsigned char>(element[1])) || !classes) {
            out += element;
            continue;
        }
        std::string declarations;
        std::stringstream names(element.substr(classes->first, classes->second - classes->first));
        std::string name;
        while (names >> name) {
            if (const auto rule = rules.find(name); rule != rules.end())
                declarations += rule->second;
        }
        if (!declarations.empty()) {
            if (const auto style = attributeValue(element, "style"))
                element.insert(style->first, declarations);
            else
                element.insert(classes->second + 1, " style=\"" + declarations + "\"");
        }
        out += element;
    }
    out.append(svg, from, std::string::npos);
    return out;
}

// Pictures decoded from data URIs lately, by their URI: application icons
// share one shadow picture.
static constexpr size_t kDataUriImages = 8;

namespace {

class DecodedImageAsset final : public skresources::ImageAsset
{
public:
    explicit DecodedImageAsset(sk_sp<SkImage> image)
        : m_image(std::move(image))
    {
    }

    bool isMultiFrame() override { return false; }
    sk_sp<SkImage> getFrame(float) override { return m_image; }

private:
    sk_sp<SkImage> m_image;
};

class SharedDataUris final : public skresources::ResourceProviderProxyBase
{
public:
    SharedDataUris()
        : ResourceProviderProxyBase(skresources::DataURIResourceProviderProxy::Make(
                  nullptr, skresources::ImageDecodeStrategy::kPreDecode))
    {
    }

    sk_sp<skresources::ImageAsset> loadImageAsset(const char path[], const char name[],
                                                  const char id[]) const override
    {
        if (!name || std::strncmp(name, "data:", 5) != 0)
            return ResourceProviderProxyBase::loadImageAsset(path, name, id);
        const std::string_view uri(name);
        {
            std::lock_guard lock(mutex());
            auto& images = decoded();
            for (auto it = images.begin(); it != images.end(); ++it) {
                if (it->first == uri) {
                    images.splice(images.begin(), images, it);
                    return sk_make_sp<DecodedImageAsset>(it->second);
                }
            }
        }
        const sk_sp<skresources::ImageAsset> asset = ResourceProviderProxyBase::loadImageAsset(path, name, id);
        sk_sp<SkImage> image = asset ? asset->getFrameData(0).image : nullptr;
        if (!image)
            return nullptr;
        std::lock_guard lock(mutex());
        auto& images = decoded();
        images.emplace_front(std::string(uri), image);
        if (images.size() > kDataUriImages)
            images.pop_back();
        return sk_make_sp<DecodedImageAsset>(std::move(image));
    }

private:
    static std::mutex& mutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    // Most recently used first.
    static std::list<std::pair<std::string, sk_sp<SkImage>>>& decoded()
    {
        static std::list<std::pair<std::string, sk_sp<SkImage>>> images;
        return images;
    }
};

} // namespace

static sk_sp<SkSVGDOM> loadSvg(sk_sp<SkData> data)
{
    const std::string text = prepareSvg(std::string(static_cast<const char*>(data->data()), data->size()));
    SkMemoryStream stream(text.data(), text.size(), false);
    // Icons carry their raster parts, such as shadows, as data URIs.
    return SkSVGDOM::Builder().setResourceProvider(sk_make_sp<SharedDataUris>()).make(stream);
}

IconLoader& IconLoader::instance()
{
    static IconLoader loader = [] {
        // Codecs are opt-in: themes ship PNG files and PNG data inside SVG files.
        SkCodecs::Register(SkPngDecoder::Decoder());
        SkCodecs::Register(SkJpegDecoder::Decoder());
        return IconLoader();
    }();
    return loader;
}

sk_sp<SkImage> IconLoader::cached(const std::string& key)
{
    const auto it = m_index.find(key);
    if (it == m_index.end())
        return nullptr;
    m_images.splice(m_images.end(), m_images, it->second);
    return it->second->second;
}

void IconLoader::store(const std::string& key, sk_sp<SkImage> image)
{
    if (const auto it = m_index.find(key); it != m_index.end()) {
        it->second->second = std::move(image);
        m_images.splice(m_images.end(), m_images, it->second);
        return;
    }
    m_images.emplace_back(key, std::move(image));
    m_index[key] = std::prev(m_images.end());
    while (m_images.size() > kCacheLimit) {
        m_index.erase(m_images.front().first);
        m_images.pop_front();
    }
}

static sk_sp<SkImage> renderSvg(const sk_sp<SkSVGDOM>& svg, int size, const Color* tint, int height = 0)
{
    const int width = size;
    if (height <= 0)
        height = size;
    if (!svg || width <= 0)
        return nullptr;
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    if (!surface)
        return nullptr;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    // The drawing fills the size it is drawn at, whatever size its file
    // declares: the view box, or the file's own size without one, fits centred,
    // keeping its aspect whatever the file asks.
    if (SkSVGSVG* root = svg->getRoot()) {
        const SkSVGLength fileWidth = root->getWidth();
        const SkSVGLength fileHeight = root->getHeight();
        const auto absolute = [](const SkSVGLength& length) {
            return length.value() > 0 && (length.unit() == SkSVGLength::Unit::kNumber
                                          || length.unit() == SkSVGLength::Unit::kPX);
        };
        if (!root->getViewBox().has_value() && absolute(fileWidth) && absolute(fileHeight))
            root->setViewBox(SkRect::MakeWH(fileWidth.value(), fileHeight.value()));
        root->setPreserveAspectRatio(SkSVGPreserveAspectRatio());
        root->setWidth(SkSVGLength(100, SkSVGLength::Unit::kPercentage));
        root->setHeight(SkSVGLength(100, SkSVGLength::Unit::kPercentage));
    }
    svg->setContainerSize(SkSize::Make(SkScalar(width), SkScalar(height)));
    if (tint) {
        // A single-colour glyph takes the colour wherever it has coverage.
        SkPaint paint;
        paint.setColorFilter(SkColorFilters::Blend(tint->argb(), SkBlendMode::kSrcIn));
        canvas->saveLayer(nullptr, &paint);
        svg->render(canvas);
        canvas->restore();
    } else {
        svg->render(canvas);
    }
    return surface->makeImageSnapshot();
}

sk_sp<SkImage> IconLoader::glyph(const std::string& name, int size, Color color, double scale)
{
    // At 12 logical pixels and below the chevrons thin to under one; they
    // have heavier drawings for that.
    std::string file = name;
    if (size / scale <= 12 + 1e-6 && name.starts_with("chevron-"))
        file += "-small";
    char suffix[48];
    snprintf(suffix, sizeof(suffix), "@%d#%08x", size, color.argb());
    const std::string key = "glyph:" + file + suffix;
    if (sk_sp<SkImage> image = cached(key))
        return image;

    sk_sp<SkData> data;
    if (const auto it = m_glyphs.find(name); it != m_glyphs.end()) {
        data = SkData::MakeWithoutCopy(it->second.data(), it->second.size());
    } else {
        const EmbeddedFile* embedded = findEmbeddedFile((file + ".svg").c_str());
        if (!embedded)
            embedded = findEmbeddedFile((name + ".svg").c_str());
        if (!embedded)
            return nullptr;
        data = SkData::MakeWithoutCopy(embedded->data, embedded->size);
    }
    sk_sp<SkImage> image = renderSvg(loadSvg(std::move(data)), size, &color);
    if (image)
        store(key, image);
    return image;
}

IconLoader::GtkCache::GtkCache(const std::string& base)
{
    namespace fs = std::filesystem;
    const fs::path file = base + "/icon-theme.cache";
    std::error_code error;
    const auto cacheTime = fs::last_write_time(file, error);
    if (error || cacheTime < fs::last_write_time(base, error) || error)
        return;
    const std::optional<std::string> contents = base::file::read(file.string());
    if (contents)
        m_data.assign(contents->begin(), contents->end());
    if (m_data.empty() || read16(0) != 1)
        return;
    m_valid = true;
    // Stale when any directory it lists changed after it.
    const uint32_t list = read32(8);
    const uint32_t count = read32(list);
    for (uint32_t i = 0; i < count && m_valid; ++i) {
        const uint32_t offset = read32(list + 4 + 4 * i);
        if (!m_valid || offset >= m_data.size()) {
            m_valid = false;
            break;
        }
        const auto directoryTime =
            fs::last_write_time(base + "/" + reinterpret_cast<const char*>(m_data.data() + offset), error);
        if (error || cacheTime < directoryTime)
            m_valid = false;
    }
}

uint16_t IconLoader::GtkCache::read16(size_t offset)
{
    if (m_data.size() < 2 || offset > m_data.size() - 2 || (offset & 1)) {
        m_valid = false;
        return 0;
    }
    return uint16_t(m_data[offset + 1] | m_data[offset] << 8);
}

uint32_t IconLoader::GtkCache::read32(size_t offset)
{
    if (m_data.size() < 4 || offset > m_data.size() - 4 || (offset & 3)) {
        m_valid = false;
        return 0;
    }
    return uint32_t(m_data[offset + 3]) | uint32_t(m_data[offset + 2]) << 8 | uint32_t(m_data[offset + 1]) << 16
            | uint32_t(m_data[offset]) << 24;
}

std::vector<IconLoader::GtkCache::Image> IconLoader::GtkCache::lookup(const std::string& name)
{
    std::vector<Image> directories;
    if (!m_valid || name.empty())
        return directories;
    // GTK's icon_name_hash, on signed chars.
    uint32_t hash = uint32_t(static_cast<signed char>(name[0]));
    for (size_t i = 1; i < name.size(); ++i)
        hash = (hash << 5) - hash + uint32_t(static_cast<signed char>(name[i]));
    const uint32_t table = read32(4);
    const uint32_t buckets = read32(table);
    if (!m_valid || buckets == 0) {
        m_valid = false;
        return directories;
    }
    // A chain ends with 0xffffffff, which must not wrap round in 32 bits: a
    // miss then read past the end and took the whole cache for stale.
    for (uint32_t bucket = read32(table + 4 + (hash % buckets) * 4);
         bucket > 0 && size_t(bucket) + 12 <= m_data.size(); bucket = read32(bucket)) {
        const uint32_t nameOffset = read32(bucket + 4);
        if (nameOffset >= m_data.size() || name != reinterpret_cast<const char*>(m_data.data() + nameOffset))
            continue;
        const uint32_t list = read32(8);
        const uint32_t count = read32(list);
        const uint32_t images = read32(bucket + 8);
        const uint32_t length = read32(images);
        if (!m_valid || images + 4 + 8 * size_t(length) > m_data.size()) {
            m_valid = false;
            return {};
        }
        for (uint32_t j = 0; j < length && m_valid; ++j) {
            const uint32_t index = read16(images + 4 + 8 * j);
            // GTK's HAS_SUFFIX_SVG and HAS_SUFFIX_PNG.
            const uint16_t flags = read16(images + 4 + 8 * j + 2);
            const uint32_t offset = read32(list + 4 + index * 4);
            if (!m_valid || index >= count || offset >= m_data.size()) {
                m_valid = false;
                return {};
            }
            directories.push_back({reinterpret_cast<const char*>(m_data.data() + offset), (flags & 4) != 0,
                                   (flags & 2) != 0});
        }
        return directories;
    }
    return directories;
}

IconLoader::IconTheme& IconLoader::theme(const std::string& name)
{
    IconTheme& theme = m_themes[name];
    if (theme.loaded) {
        // Installing the first Flatpak application creates its icon directory.
        const bool appeared = std::any_of(theme.missing.begin(), theme.missing.end(), [](const std::string& root) {
            return std::filesystem::is_directory(root);
        });
        if (!appeared)
            return theme;
        theme = IconTheme();
        m_paths.clear();
    }
    theme.loaded = true;

    for (const std::string& base : iconBases()) {
        const std::string root = base + "/" + name;
        if (std::filesystem::is_directory(root)) {
            theme.bases.push_back(root);
            theme.caches.push_back(std::make_shared<GtkCache>(root));
        } else {
            theme.missing.push_back(root);
        }
    }
    std::ifstream index;
    for (const std::string& root : theme.bases) {
        index.open(root + "/index.theme");
        if (index.is_open())
            break;
    }
    if (!index.is_open())
        return theme;
    theme.found = true;

    // Sections of index.theme, keyed by group name.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> groups;
    std::string group;
    std::string line;
    while (std::getline(index, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            group = line.substr(1, line.size() - 2);
            continue;
        }
        const size_t equals = line.find('=');
        if (equals != std::string::npos)
            groups[group][trim(line.substr(0, equals))] = trim(line.substr(equals + 1));
    }

    auto& main = groups["Icon Theme"];
    theme.parents = split(main["Inherits"]);
    std::vector<std::string> directories = split(main["Directories"]);
    for (const std::string& scaled : split(main["ScaledDirectories"]))
        directories.push_back(scaled);
    for (const std::string& path : directories) {
        auto& entry = groups[path];
        Directory directory;
        directory.path = path;
        directory.size = std::atoi(entry["Size"].c_str());
        directory.scale = std::max(1, std::atoi(entry["Scale"].c_str()));
        const std::string& type = entry["Type"];
        directory.type = type == "Fixed" ? 'F' : type == "Scalable" ? 'S' : 'T';
        directory.minimum = entry.count("MinSize") ? std::atoi(entry["MinSize"].c_str()) : directory.size;
        directory.maximum = entry.count("MaxSize") ? std::atoi(entry["MaxSize"].c_str()) : directory.size;
        if (entry.count("Threshold"))
            directory.threshold = std::atoi(entry["Threshold"].c_str());
        directory.generic = entry["Context"] != "Applications" && entry["Context"] != "MimeTypes";
        directory.applications = entry["Context"] == "Applications";
        theme.directories.push_back(directory);
    }
    return theme;
}

// directorySizeDistance in qiconloader.cpp, its Threshold quirk included.
static int sizeDistance(char type, int size, int minimum, int maximum, int threshold, int scale, int iconSize,
                        int iconScale)
{
    const int scaled = iconSize * iconScale;
    switch (type) {
    case 'F':
        return std::abs(size * scale - scaled);
    case 'S':
        if (scaled < minimum * scale)
            return minimum * scale - scaled;
        if (scaled > maximum * scale)
            return scaled - maximum * scale;
        return 0;
    default:
        if (scaled < (size - threshold) * scale)
            return minimum * scale - scaled;
        if (scaled > (size + threshold) * scale)
            return scaled - maximum * scale;
        return 0;
    }
}

// directoryMatchesSize in qiconloader.cpp.
static bool matchesSize(char type, int size, int minimum, int maximum, int threshold, int scale, int iconSize,
                        int iconScale)
{
    if (scale != iconScale)
        return false;
    switch (type) {
    case 'F':
        return size == iconSize;
    case 'S':
        return iconSize <= maximum && iconSize >= minimum;
    default:
        return iconSize >= size - threshold && iconSize <= size + threshold;
    }
}

// The theme lookup: the theme, its parents depth first, the first with the name
// giving every file it has of it, pictures before drawings; an exact size
// first, else the closest. Foreign is set for an application icon that only
// a parent or hicolor has.
std::string IconLoader::lookup(const std::string& name, const std::string& requested, int size, int scale,
                               bool* foreign)
{
    struct Entry
    {
        const Directory* directory;
        std::string path;
    };
    const std::string& current = Theme::current().iconTheme;
    std::string found;
    const bool shortened = name.size() < requested.size();
    std::vector<std::string> visited;
    std::vector<Entry> entries;
    const auto search = [&](const auto& self, const std::string& themeName) -> void {
        if (themeName.empty() || std::find(visited.begin(), visited.end(), themeName) != visited.end())
            return;
        visited.push_back(themeName);
        IconTheme& iconTheme = theme(themeName);
        for (size_t i = 0; i < iconTheme.bases.size(); ++i) {
            const std::string& root = iconTheme.bases[i];
            // The cache's directories for the name, in its order, and the
            // suffixes it records save the stats.
            GtkCache& cache = *iconTheme.caches[i];
            const std::vector<GtkCache::Image> cached = cache.lookup(name);
            if (cache.isValid()) {
                for (const GtkCache::Image& image : cached) {
                    const auto found = std::find_if(iconTheme.directories.begin(), iconTheme.directories.end(),
                                                    [&image](const Directory& d) { return d.path == image.directory; });
                    if (found == iconTheme.directories.end() || (shortened && !found->generic))
                        continue;
                    const std::string base = root + "/" + found->path + "/" + name;
                    if (image.png)
                        entries.insert(entries.begin(), {&*found, base + ".png"});
                    else if (image.svg)
                        entries.push_back({&*found, base + ".svg"});
                }
                continue;
            }
            for (const Directory& directory : iconTheme.directories) {
                if (shortened && !directory.generic)
                    continue;
                const std::string base = root + "/" + directory.path + "/" + name;
                if (std::filesystem::exists(base + ".png"))
                    entries.insert(entries.begin(), {&directory, base + ".png"});
                else if (std::filesystem::exists(base + ".svg"))
                    entries.push_back({&directory, base + ".svg"});
            }
        }
        if (!entries.empty()) {
            found = themeName;
            return;
        }
        std::vector<std::string> parents = iconTheme.parents;
        if (themeName != "hicolor" && std::find(parents.begin(), parents.end(), "hicolor") == parents.end())
            parents.push_back("hicolor");
        for (const std::string& parent : parents) {
            self(self, parent);
            if (!entries.empty())
                return;
        }
    };
    search(search, current);

    const Entry* best = nullptr;
    for (const Entry& entry : entries) {
        const Directory& d = *entry.directory;
        if (matchesSize(d.type, d.size, d.minimum, d.maximum, d.threshold, d.scale, size, scale)) {
            best = &entry;
            break;
        }
    }
    if (!best) {
        int bestDistance = INT_MAX;
        for (const Entry& entry : entries) {
            const Directory& d = *entry.directory;
            const int distance = sizeDistance(d.type, d.size, d.minimum, d.maximum, d.threshold, d.scale, size, scale);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = &entry;
            }
        }
    }
    if (!best)
        return {};
    if (foreign)
        *foreign = found != current && best->directory->applications;
    return best->path;
}

std::string IconLoader::themeIconPath(const std::string& name, int size, double scale)
{
    return resolve(name, size, scale).path;
}

IconLoader::Resolved IconLoader::resolve(const std::string& name, int size, double scale)
{
    // A desktop entry's Icon may name a file by its absolute path.
    if (name.starts_with('/'))
        return {std::filesystem::is_regular_file(name) ? name : std::string(), {}};

    const int iconScale = int(std::ceil(scale));
    const std::string key = name + "@" + std::to_string(size) + "x" + std::to_string(iconScale) + "/"
            + Theme::current().iconTheme;
    const auto it = m_paths.find(key);
    if (it != m_paths.end())
        return it->second;

    // A missing "a-b-c" falls back to "a-b", then "a"; then /usr/share/pixmaps
    // is looked in for the name itself.
    std::string path;
    bool foreign = false;
    for (std::string candidate = name; !candidate.empty();) {
        path = lookup(candidate, name, size, iconScale, &foreign);
        if (!path.empty())
            break;
        const size_t dash = candidate.rfind('-');
        candidate = dash == std::string::npos ? std::string() : candidate.substr(0, dash);
    }
    for (const char* extension : kExtensions) {
        if (!path.empty())
            break;
        if (std::filesystem::exists("/usr/share/pixmaps/" + name + extension)) {
            path = "/usr/share/pixmaps/" + name + extension;
            foreign = true;
        }
    }
    Resolved resolved{path, {}};
    if (foreign && !name.ends_with("-symbolic"))
        resolved.plate = lookup(kPlateName, kPlateName, 64, 1, nullptr);
    m_paths.emplace(key, resolved);
    return resolved;
}

// An ICNS file's picture for the size, as a desktop entry's Icon may name
// one; any other file as it is.
static sk_sp<SkData> iconFileData(const std::string& path, int pixels, bool* icns = nullptr)
{
    sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
    if (!data)
        return nullptr;
    const std::string_view bytes(static_cast<const char*>(data->data()), data->size());
    if (!image_formats::isIcns(bytes))
        return data;
    if (icns)
        *icns = true;
    const std::string_view png = image_formats::icnsPng(bytes, int(std::ceil(pixels * kIcnsScale)));
    if (png.empty())
        return nullptr;
    return SkData::MakeSubset(data.get(), size_t(png.data() - bytes.data()), png.size());
}

struct Ink
{
    SkIRect bounds;
    // The pixels at half coverage or more, and their share of those bounds
    // outside four corner squares of kOwnPlateCorner of the shorter side.
    SkIRect solidBounds;
    double sideFill = 0;
    // solidBounds to the partial coverage of their edge pixels across the
    // middle, where a plate's sides are straight.
    SkRect edges;
};

// Where a straight edge crosses from the outside pixel to the first solid one.
static double edgeAt(int first, uint8_t inner, uint8_t outer, int direction)
{
    const double covered = inner < 255 ? inner / 255.0 : 1 + outer / 255.0;
    return direction > 0 ? first + 1 - covered : first + covered;
}

// The pixels with at least kInkAlpha coverage; nothing for a blank picture.
static std::optional<Ink> measureInk(const sk_sp<SkImage>& image)
{
    SkBitmap bitmap;
    if (!bitmap.tryAllocPixels(SkImageInfo::MakeA8(image->width(), image->height()))
        || !image->readPixels(bitmap.pixmap(), 0, 0))
        return std::nullopt;
    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    for (int y = 0; y < bitmap.height(); ++y) {
        const uint8_t* row = bitmap.getAddr8(0, y);
        for (int x = 0; x < bitmap.width(); ++x) {
            if (row[x] < kInkAlpha)
                continue;
            left = std::min(left, x);
            right = std::max(right, x + 1);
            top = std::min(top, y);
            bottom = std::max(bottom, y + 1);
        }
    }
    if (left == INT_MAX)
        return std::nullopt;
    bool solid = false;
    int solidLeft = INT_MAX;
    int solidTop = INT_MAX;
    int solidRight = INT_MIN;
    int solidBottom = INT_MIN;
    for (int y = top; y < bottom; ++y) {
        const uint8_t* row = bitmap.getAddr8(0, y);
        for (int x = left; x < right; ++x) {
            if (row[x] < 128)
                continue;
            solid = true;
            solidLeft = std::min(solidLeft, x);
            solidRight = std::max(solidRight, x + 1);
            solidTop = std::min(solidTop, y);
            solidBottom = std::max(solidBottom, y + 1);
        }
    }
    Ink ink;
    ink.bounds = SkIRect::MakeLTRB(left, top, right, bottom);
    if (!solid)
        return ink;
    ink.solidBounds = SkIRect::MakeLTRB(solidLeft, solidTop, solidRight, solidBottom);

    const int corner = int(std::lround(std::min(ink.solidBounds.width(), ink.solidBounds.height())
                                       * kOwnPlateCorner));
    size_t area = 0;
    size_t filled = 0;
    for (int y = solidTop; y < solidBottom; ++y) {
        const bool cornerRow = y < solidTop + corner || y >= solidBottom - corner;
        const uint8_t* row = bitmap.getAddr8(0, y);
        for (int x = solidLeft; x < solidRight; ++x) {
            if (cornerRow && (x < solidLeft + corner || x >= solidRight - corner))
                continue;
            ++area;
            if (row[x] >= 128)
                ++filled;
        }
    }
    ink.sideFill = area > 0 ? double(filled) / double(area) : 0;

    const auto at = [&bitmap](int x, int y) -> uint8_t {
        if (x < 0 || y < 0 || x >= bitmap.width() || y >= bitmap.height())
            return 0;
        return *bitmap.getAddr8(x, y);
    };
    const int middleX = (solidLeft + solidRight) / 2;
    const int middleY = (solidTop + solidBottom) / 2;
    ink.edges = SkRect::MakeLTRB(
        SkScalar(edgeAt(solidLeft, at(solidLeft, middleY), at(solidLeft - 1, middleY), 1)),
        SkScalar(edgeAt(solidTop, at(middleX, solidTop), at(middleX, solidTop - 1), 1)),
        SkScalar(edgeAt(solidRight - 1, at(solidRight - 1, middleY), at(solidRight, middleY), -1)),
        SkScalar(edgeAt(solidBottom - 1, at(middleX, solidBottom - 1), at(middleX, solidBottom), -1)));
    return ink;
}

// The theme's plate at a size, drawn once: every icon on it at that size
// shares the drawing. Any thread.
static sk_sp<SkImage> plateImage(const std::string& plate, int size)
{
    static std::mutex mutex;
    static std::unordered_map<std::string, sk_sp<SkImage>> plates;
    const std::string key = plate + "@" + std::to_string(size);
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = plates.find(key); it != plates.end())
            return it->second;
    }
    sk_sp<SkData> data = SkData::MakeFromFileName(plate.c_str());
    sk_sp<SkImage> image = data ? renderSvg(loadSvg(std::move(data)), size, nullptr) : nullptr;
    if (!image)
        return nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (plates.size() >= 32)
        plates.clear();
    plates.emplace(key, image);
    return image;
}

static bool hasOwnPlate(const Ink& ink)
{
    const int longer = std::max(ink.solidBounds.width(), ink.solidBounds.height());
    const int shorter = std::min(ink.solidBounds.width(), ink.solidBounds.height());
    return shorter > 0 && longer <= shorter * kOwnPlateAspect && ink.sideFill >= kOwnPlateFill;
}

// An application's own icon on the theme's plate, its visible part
// kPlateContent of the size at the longer side and centred; one with a plate
// of its own takes the theme plate's place and size instead.
static sk_sp<SkImage> renderOnPlate(const std::string& plate, sk_sp<SkData> data, bool svg, int size)
{
    sk_sp<SkSVGDOM> drawing;
    sk_sp<SkImage> picture;
    if (svg) {
        drawing = loadSvg(std::move(data));
        picture = renderSvg(drawing, kInkSize, nullptr);
    } else if (std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data))) {
        picture = std::get<0>(codec->getImage());
    }
    if (!picture)
        return nullptr;

    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size, size));
    if (!surface)
        return nullptr;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    const std::optional<Ink> measured = measureInk(picture);
    const bool ownPlate = measured && hasOwnPlate(*measured);
    if (!ownPlate) {
        const sk_sp<SkImage> base = plateImage(plate, size);
        if (!base)
            return nullptr;
        canvas->drawImage(base, 0, 0);
    }
    if (!measured)
        return surface->makeImageSnapshot();

    // An own plate's soft edge and shadow stay outside the theme plate's size.
    const SkRect ink = ownPlate ? measured->edges : SkRect::Make(measured->bounds);
    const double extent = ownPlate ? kPlateExtent : kPlateContent;
    const double factor = size * extent / std::max(ink.width(), ink.height());
    const double centre = size / 2.0;
    if (drawing) {
        // Drawn again at the size that gives the visible part its extent, so
        // its edges stay sharp.
        const double drawn = kInkSize * factor;
        canvas->translate(SkScalar(centre - (ink.left() + ink.width() / 2.0) * factor),
                          SkScalar(centre - (ink.top() + ink.height() / 2.0) * factor));
        drawing->setContainerSize(SkSize::Make(SkScalar(drawn), SkScalar(drawn)));
        drawing->render(canvas);
    } else {
        // The cubic filter reads 4 x 4 pixels, too few for a picture several
        // times larger: it is halved first, each pixel the mean of four.
        SkRect source = ink;
        double remaining = factor;
        while (remaining < 0.5) {
            const int width = std::max(1, picture->width() / 2);
            const int height = std::max(1, picture->height() / 2);
            sk_sp<SkSurface> half = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
            if (!half)
                return nullptr;
            half->getCanvas()->clear(SK_ColorTRANSPARENT);
            half->getCanvas()->drawImageRect(picture, SkRect::MakeIWH(width, height),
                                             SkSamplingOptions(SkFilterMode::kLinear));
            source = SkRect::MakeLTRB(source.left() * width / picture->width(),
                                      source.top() * height / picture->height(),
                                      source.right() * width / picture->width(),
                                      source.bottom() * height / picture->height());
            remaining *= double(picture->width()) / width;
            picture = half->makeImageSnapshot();
        }
        const SkRect target = SkRect::MakeXYWH(SkScalar(centre - ink.width() * factor / 2),
                                               SkScalar(centre - ink.height() * factor / 2),
                                               SkScalar(ink.width() * factor), SkScalar(ink.height() * factor));
        const SkSamplingOptions sampling = remaining < 1 ? SkSamplingOptions(SkCubicResampler::Mitchell())
                                                         : SkSamplingOptions(SkFilterMode::kLinear);
        // Antialiased, or the target's edges snap to whole pixels.
        SkPaint paint;
        paint.setAntiAlias(true);
        canvas->drawImageRect(picture, source, target, sampling, &paint, SkCanvas::kStrict_SrcRectConstraint);
    }
    return surface->makeImageSnapshot();
}

sk_sp<SkImage> IconLoader::rasterise(const std::string& path, int size, const Color* tint, const std::string& plate)
{
    bool icns = false;
    sk_sp<SkData> data = iconFileData(path, size, &icns);
    if (!data)
        return nullptr;
    if (!plate.empty() && !icns && !tint)
        return renderOnPlate(plate, std::move(data), path.ends_with(".svg"), size);
    if (path.ends_with(".svg"))
        return renderSvg(loadSvg(std::move(data)), size, tint);

    std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data));
    if (!codec)
        return nullptr;
    auto [decoded, result] = codec->getImage();
    if (!decoded)
        return nullptr;
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(size, size));
    if (!surface)
        return nullptr;
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    const SkScalar extent = SkScalar(icns ? size * kIcnsScale : size);
    // A larger picture is scaled down smoothly; a smaller
    // one stays as it is and the item stretches it, sampled linearly.
    const bool larger = decoded->width() > extent || decoded->height() > extent;
    const SkSamplingOptions sampling = larger ? SkSamplingOptions(SkCubicResampler::Mitchell())
                                              : SkSamplingOptions(SkFilterMode::kLinear);
    const SkScalar inset = (SkScalar(size) - extent) / 2;
    surface->getCanvas()->drawImageRect(decoded, SkRect::MakeXYWH(inset, inset, extent, extent), sampling);
    return surface->makeImageSnapshot();
}

void IconLoader::registerGlyph(const std::string& name, std::string svg)
{
    m_glyphs[name] = std::move(svg);
    // Drawings of the name made before are stale.
    for (auto it = m_images.begin(); it != m_images.end();) {
        if (it->first.starts_with("glyph:" + name + "@")) {
            m_index.erase(it->first);
            it = m_images.erase(it);
        } else {
            ++it;
        }
    }
}

sk_sp<SkImage> IconLoader::renderSvgFile(const std::string& path, int width, int height)
{
    // The codecs for data URIs register with the loader.
    instance();
    sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
    return data ? renderSvg(loadSvg(std::move(data)), width, nullptr, height) : nullptr;
}

sk_sp<SkImage> IconLoader::svg(const std::string& path, int width, int height, const Color* tint)
{
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "@%dx%d#%08x%s", width, height, tint ? tint->argb() : 0, tint ? "t" : "");
    const std::string key = "svg:" + path + suffix;
    if (sk_sp<SkImage> image = cached(key))
        return image;
    sk_sp<SkData> data = SkData::MakeFromFileName(path.c_str());
    if (!data)
        return nullptr;
    sk_sp<SkImage> image = renderSvg(loadSvg(std::move(data)), width, tint, height);
    if (image)
        store(key, image);
    return image;
}

sk_sp<SkImage> IconLoader::pixmap(const std::string& name, int pixels)
{
    if (pixels <= 0)
        return nullptr;
    const std::string key = "pixmap:" + Theme::current().iconTheme + "/" + name + "@" + std::to_string(pixels);
    if (sk_sp<SkImage> image = cached(key))
        return image;
    const Resolved resolved = resolve(name, pixels, 1);
    const std::string& path = resolved.path;
    if (path.empty())
        return nullptr;
    sk_sp<SkImage> image;
    if (path.ends_with(".svg") || !resolved.plate.empty()) {
        image = rasterise(path, pixels, nullptr, resolved.plate);
    } else if (bool icns = false; sk_sp<SkData> data = iconFileData(path, pixels, &icns)) {
        // A larger picture is scaled down and a smaller one left as it is.
        if (std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data))) {
            auto [decoded, result] = codec->getImage();
            if (decoded && (icns || decoded->width() > pixels || decoded->height() > pixels))
                image = rasterise(path, pixels, nullptr);
            else
                image = decoded;
        }
    }
    if (image)
        store(key, image);
    return image;
}

sk_sp<SkImage> IconLoader::themeIcon(const std::string& name, int size, int iconScale, int pixels)
{
    const std::string key = "theme:" + Theme::current().iconTheme + "/" + name + "@" + std::to_string(size) + "x"
            + std::to_string(iconScale) + ":" + std::to_string(pixels);
    if (sk_sp<SkImage> image = cached(key))
        return image;
    const Resolved resolved = resolve(name, size, iconScale);
    if (resolved.path.empty())
        return nullptr;
    sk_sp<SkImage> image = rasterise(resolved.path, pixels, nullptr, resolved.plate);
    if (image)
        store(key, image);
    return image;
}

void IconLoader::preloadThemeIcons(const std::vector<std::string>& names, int size, double scale,
                                   std::function<void()> done)
{
    const int device = int(std::lround(size * scale));
    struct Pending
    {
        std::string key;
        std::string path;
        std::string plate;
        sk_sp<SkImage> image;
    };
    auto pending = std::make_shared<std::vector<Pending>>();
    for (const std::string& name : names) {
        const std::string key = "theme:" + Theme::current().iconTheme + "/" + name + "@" + std::to_string(size) + "x"
                + std::to_string(device);
        if (cached(key) || std::any_of(pending->begin(), pending->end(), [&key](const Pending& p) { return p.key == key; }))
            continue;
        Resolved resolved = resolve(name, size, scale);
        if (!resolved.path.empty())
            pending->push_back({key, std::move(resolved.path), std::move(resolved.plate), nullptr});
    }
    if (pending->empty()) {
        if (done)
            done();
        return;
    }
    // One job per icon, so the pool's threads share them; the last one in
    // stores them all.
    auto remaining = std::make_shared<size_t>(pending->size());
    auto finished = std::make_shared<std::function<void()>>(std::move(done));
    for (size_t i = 0; i < pending->size(); ++i) {
        base::ThreadPool::global()
            .run([pending, i, device] {
                     Pending& entry = (*pending)[i];
                     entry.image = rasterise(entry.path, device, nullptr, entry.plate);
                 },
                 [this, pending, remaining, finished] {
                     if (--*remaining > 0)
                         return;
                     for (Pending& entry : *pending) {
                         if (entry.image && !cached(entry.key))
                             store(entry.key, std::move(entry.image));
                     }
                     if (*finished)
                         (*finished)();
                 })
            .detach();
    }
}

sk_sp<SkImage> IconLoader::themeIcon(const std::string& name, int size, double scale)
{
    const int device = int(std::lround(size * scale));
    const std::string key = "theme:" + Theme::current().iconTheme + "/" + name + "@" + std::to_string(size) + "x"
            + std::to_string(device);
    if (sk_sp<SkImage> image = cached(key))
        return image;
    const Resolved resolved = resolve(name, size, scale);
    if (resolved.path.empty())
        return nullptr;
    sk_sp<SkImage> image = rasterise(resolved.path, device, nullptr, resolved.plate);
    if (image)
        store(key, image);
    return image;
}

} // namespace cutefish::ui
