// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "ui/image_info.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <string_view>

#include <jxl/decode.h>
#include <libheif/heif.h>
#include <tiffio.h>
#include <zlib.h>

#include "base/file.h"
#include "ui/image_formats.h"

namespace cutefish::ui {

static uint32_t be16(const unsigned char* p)
{
    return uint32_t(p[0]) << 8 | p[1];
}

static uint32_t be32(const unsigned char* p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

static uint32_t le16(const unsigned char* p)
{
    return uint32_t(p[1]) << 8 | p[0];
}

static uint32_t le24(const unsigned char* p)
{
    return uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

static uint32_t le32(const unsigned char* p)
{
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

static std::string readHead(const std::string& path, size_t limit)
{
    std::ifstream file(path, std::ios::binary);
    std::string data(limit, '\0');
    file.read(data.data(), std::streamsize(limit));
    data.resize(size_t(file.gcount()));
    return data;
}

// An APNG carries an acTL chunk before its first IDAT.
static void readPng(const std::string& data, ImageInfo& info)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 24 || data.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0)
        return;
    info.readable = true;
    info.width = int(be32(p + 16));
    info.height = int(be32(p + 20));
    size_t offset = 8;
    while (offset + 8 <= data.size()) {
        const uint32_t length = be32(p + offset);
        const std::string_view type(data.data() + offset + 4, 4);
        if (type == "acTL") {
            info.animated = offset + 12 <= data.size() && be32(p + offset + 8) > 1;
            return;
        }
        if (type == "IDAT")
            return;
        offset += 12 + size_t(length);
    }
}

// The orientation tag of an Exif APP1 segment; 1 when there is none.
static int exifOrientation(const unsigned char* p, size_t size)
{
    if (size < 14 || std::string_view(reinterpret_cast<const char*>(p), 6) != std::string_view("Exif\0\0", 6))
        return 1;
    const unsigned char* tiff = p + 6;
    const size_t length = size - 6;
    const bool little = tiff[0] == 'I';
    auto u16 = [&](size_t at) { return at + 2 <= length ? (little ? le16(tiff + at) : be16(tiff + at)) : 0; };
    auto u32 = [&](size_t at) { return at + 4 <= length ? (little ? le32(tiff + at) : be32(tiff + at)) : 0; };
    const size_t ifd = u32(4);
    const uint32_t entries = u16(ifd);
    for (uint32_t i = 0; i < entries; ++i) {
        const size_t entry = ifd + 2 + i * 12;
        if (entry + 12 > length)
            break;
        if (u16(entry) == 0x0112)
            return int(u16(entry + 8));
    }
    return 1;
}

static void readJpeg(const std::string& path, ImageInfo& info)
{
    const std::string data = readHead(path, 256 * 1024);
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 4 || p[0] != 0xff || p[1] != 0xd8)
        return;
    info.readable = true;
    int orientation = 1;
    size_t offset = 2;
    while (offset + 4 <= data.size()) {
        if (p[offset] != 0xff) {
            ++offset;
            continue;
        }
        const unsigned char marker = p[offset + 1];
        if (marker == 0xff) {
            ++offset;
            continue;
        }
        const uint32_t length = be16(p + offset + 2);
        if (marker == 0xe1)
            orientation = exifOrientation(p + offset + 4, std::min<size_t>(length - 2, data.size() - offset - 4));
        // Start of frame, baseline to lossless, not DHT, JPG or DAC.
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (offset + 9 > data.size())
                return;
            info.height = int(be16(p + offset + 5));
            info.width = int(be16(p + offset + 7));
            // Orientations 5 to 8 turn the image a quarter.
            if (orientation >= 5 && orientation <= 8)
                std::swap(info.width, info.height);
            return;
        }
        offset += 2 + length;
    }
}

// Counts image descriptors up to the second, skipping the frames' data.
static void readGif(const std::string& path, ImageInfo& info)
{
    std::ifstream file(path, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 13 || (data.compare(0, 6, "GIF87a") != 0 && data.compare(0, 6, "GIF89a") != 0))
        return;
    info.readable = true;
    info.width = int(le16(p + 6));
    info.height = int(le16(p + 8));
    size_t offset = 13;
    if (p[10] & 0x80)
        offset += 3u << ((p[10] & 7) + 1);
    int frames = 0;
    auto skipBlocks = [&] {
        while (offset < data.size() && p[offset] != 0)
            offset += 1 + p[offset];
        ++offset;
    };
    while (offset < data.size()) {
        const unsigned char block = p[offset];
        if (block == 0x2c) {
            if (++frames > 1)
                break;
            if (offset + 10 > data.size())
                break;
            const unsigned char flags = p[offset + 9];
            offset += 10;
            if (flags & 0x80)
                offset += 3u << ((flags & 7) + 1);
            ++offset;
            skipBlocks();
        } else if (block == 0x21) {
            offset += 2;
            skipBlocks();
        } else {
            break;
        }
    }
    info.animated = frames > 1;
}

static void readWebp(const std::string& data, ImageInfo& info)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 30 || data.compare(0, 4, "RIFF") != 0 || data.compare(8, 4, "WEBP") != 0)
        return;
    info.readable = true;
    const std::string_view chunk(data.data() + 12, 4);
    if (chunk == "VP8X") {
        info.animated = p[20] & 0x02;
        info.width = int(le24(p + 24)) + 1;
        info.height = int(le24(p + 27)) + 1;
    } else if (chunk == "VP8L") {
        const uint32_t bits = le32(p + 21);
        info.width = int(bits & 0x3fff) + 1;
        info.height = int((bits >> 14) & 0x3fff) + 1;
    } else if (chunk == "VP8 ") {
        info.width = int(le16(p + 26) & 0x3fff);
        info.height = int(le16(p + 28) & 0x3fff);
    }
}

static void readBmp(const std::string& data, ImageInfo& info)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 26 || data.compare(0, 2, "BM") != 0)
        return;
    info.readable = true;
    info.width = int(int32_t(le32(p + 18)));
    info.height = std::abs(int(int32_t(le32(p + 22))));
}

// An SVG's size: width and height in user units
// (pixels), else the view box's.
static void readSvg(const std::string& data, ImageInfo& info)
{
    const size_t start = data.find("<svg");
    if (start == std::string::npos)
        return;
    info.readable = true;
    info.vector = true;
    const size_t end = data.find('>', start);
    const std::string tag = data.substr(start, end == std::string::npos ? std::string::npos : end - start);
    auto attribute = [&](const char* name) -> std::string {
        const std::regex pattern(std::string("\\s") + name + "\\s*=\\s*[\"']([^\"']*)[\"']");
        std::smatch match;
        return std::regex_search(tag, match, pattern) ? match[1].str() : std::string();
    };
    auto length = [](const std::string& value) -> double {
        if (value.empty() || value.find('%') != std::string::npos)
            return 0;
        char* rest = nullptr;
        const double number = std::strtod(value.c_str(), &rest);
        const std::string unit = rest ? std::string(rest) : std::string();
        if (unit == "pt")
            return number * 1.25;
        if (unit == "pc")
            return number * 15;
        if (unit == "mm")
            return number * 3.543307;
        if (unit == "cm")
            return number * 35.43307;
        if (unit == "in")
            return number * 90;
        return number;
    };
    double width = length(attribute("width"));
    double height = length(attribute("height"));
    if (width <= 0 || height <= 0) {
        const std::string box = attribute("viewBox");
        double x = 0, y = 0, w = 0, h = 0;
        if (std::sscanf(box.c_str(), "%lf%*[ ,]%lf%*[ ,]%lf%*[ ,]%lf", &x, &y, &w, &h) == 4) {
            width = w;
            height = h;
        }
    }
    info.width = int(width + 0.5);
    info.height = int(height + 0.5);
}

// The start of a gzip stream's contents, up to limit bytes.
static std::string gunzipHead(const std::string& data, size_t limit)
{
    z_stream stream = {};
    // 16 more window bits read the gzip wrapper.
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK)
        return {};
    std::string out(limit, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = uInt(out.size());
    inflate(&stream, Z_SYNC_FLUSH);
    out.resize(limit - stream.avail_out);
    inflateEnd(&stream);
    return out;
}

// The first directory's size, as libtiff reads it without the strips.
static void readTiff(const std::string& path, ImageInfo& info)
{
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);
    std::unique_ptr<TIFF, decltype(&TIFFClose)> tiff(TIFFOpen(path.c_str(), "r"), TIFFClose);
    if (!tiff)
        return;
    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height);
    info.readable = true;
    info.width = int(width);
    info.height = int(height);
}

// HEIF and AVIF: the primary image's size with its rotation applied, from
// the boxes alone.
static void readHeif(const std::string& path, ImageInfo& info)
{
    std::unique_ptr<heif_context, decltype(&heif_context_free)> context(heif_context_alloc(), heif_context_free);
    if (!context || heif_context_read_from_file(context.get(), path.c_str(), nullptr).code != heif_error_Ok)
        return;
    heif_image_handle* raw = nullptr;
    if (heif_context_get_primary_image_handle(context.get(), &raw).code != heif_error_Ok || !raw)
        return;
    std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle(raw, heif_image_handle_release);
    info.readable = true;
    info.width = heif_image_handle_get_width(handle.get());
    info.height = heif_image_handle_get_height(handle.get());
}

// The basic info at the start of the codestream; orientations 5 to 8 turn
// the image a quarter.
static void readJxl(const std::string& data, ImageInfo& info)
{
    std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
    if (!decoder || JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS)
        return;
    JxlDecoderSetInput(decoder.get(), reinterpret_cast<const uint8_t*>(data.data()), data.size());
    JxlDecoderCloseInput(decoder.get());
    if (JxlDecoderProcessInput(decoder.get()) != JXL_DEC_BASIC_INFO)
        return;
    JxlBasicInfo basic;
    if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS)
        return;
    info.readable = true;
    info.width = int(basic.xsize);
    info.height = int(basic.ysize);
    if (basic.orientation >= JXL_ORIENT_TRANSPOSE)
        std::swap(info.width, info.height);
}

// The largest icon of the directory, which is the one decoded; 0 in an
// entry means 256, or a PNG that gives its own size.
static void readIco(const std::string& data, ImageInfo& info)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 6 || le16(p) != 0 || le16(p + 2) != 1)
        return;
    const size_t count = le16(p + 4);
    for (size_t i = 0; i < count && 6 + (i + 1) * 16 <= data.size(); ++i) {
        const unsigned char* entry = p + 6 + i * 16;
        int width = entry[0] ? entry[0] : 256;
        int height = entry[1] ? entry[1] : 256;
        const size_t offset = le32(entry + 12);
        if (offset + 24 <= data.size() && data.compare(offset, 8, "\x89PNG\r\n\x1a\n") == 0) {
            width = int(be32(p + offset + 16));
            height = int(be32(p + offset + 20));
        }
        if (width * int64_t(height) > info.width * int64_t(info.height)) {
            info.width = width;
            info.height = height;
        }
    }
    info.readable = count > 0;
}

static void readTga(const std::string& data, ImageInfo& info)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    if (data.size() < 18)
        return;
    info.readable = true;
    info.width = int(le16(p + 12));
    info.height = int(le16(p + 14));
}

// Netpbm: "P1" to "P6", then the width and the height, comments skipped.
static void readPnm(const std::string& data, ImageInfo& info)
{
    if (data.size() < 3 || data[0] != 'P' || data[1] < '1' || data[1] > '6')
        return;
    size_t position = 2;
    auto number = [&]() -> int {
        while (position < data.size()) {
            if (data[position] == '#') {
                while (position < data.size() && data[position] != '\n')
                    ++position;
            } else if (std::isspace(static_cast<unsigned char>(data[position]))) {
                ++position;
            } else {
                break;
            }
        }
        int value = 0;
        while (position < data.size() && std::isdigit(static_cast<unsigned char>(data[position])) && value < (1 << 24))
            value = value * 10 + (data[position++] - '0');
        return value;
    };
    info.readable = true;
    info.width = number();
    info.height = number();
}

static void readXbm(const std::string& data, ImageInfo& info)
{
    std::smatch match;
    const std::string head = data.substr(0, 1024);
    if (std::regex_search(head, match, std::regex("#define\\s+\\S*width\\s+(\\d+)")))
        info.width = std::atoi(match[1].str().c_str());
    if (std::regex_search(head, match, std::regex("#define\\s+\\S*height\\s+(\\d+)")))
        info.height = std::atoi(match[1].str().c_str());
    info.readable = info.width > 0 && info.height > 0;
}

// The PNG an ICNS file is decoded from, its largest.
static void readIcns(const std::string& path, ImageInfo& info)
{
    const std::optional<std::string> data = base::file::read(path);
    if (!data)
        return;
    const std::string_view png = image_formats::icnsPng(*data);
    if (png.empty())
        return;
    readPng(std::string(png.substr(0, 64)), info);
}

const std::vector<std::string>& imageMimeTypes()
{
    static const std::vector<std::string> types = {
        "image/png",
        "image/apng",
        "image/jpeg",
        "image/gif",
        "image/webp",
        "image/bmp",
        "image/svg+xml",
        "image/svg+xml-compressed",
        "image/tiff",
        "image/heif",
        "image/avif",
        "image/jxl",
        "image/vnd.microsoft.icon",
        "image/x-tga",
        "image/x-portable-pixmap",
        "image/x-portable-graymap",
        "image/x-portable-bitmap",
        "image/x-xbitmap",
        "image/x-icns",
    };
    return types;
}

// A size a header can give: positive, and no side over 2^20 pixels; any
// other is no size.
static ImageInfo saneSize(ImageInfo info)
{
    constexpr int kLargest = 1 << 20;
    if (info.width <= 0 || info.height <= 0 || info.width > kLargest || info.height > kLargest)
        info.width = info.height = 0;
    return info;
}

static ImageInfo readHeader(const std::string& path, const std::string& mimeType)
{
    ImageInfo info;
    if (mimeType == "image/jpeg") {
        readJpeg(path, info);
        return info;
    }
    if (mimeType == "image/gif") {
        readGif(path, info);
        return info;
    }
    if (mimeType == "image/tiff") {
        readTiff(path, info);
        return info;
    }
    if (mimeType == "image/heif" || mimeType == "image/avif") {
        readHeif(path, info);
        return info;
    }
    if (mimeType == "image/x-icns") {
        readIcns(path, info);
        return info;
    }
    const std::string head = readHead(path, 64 * 1024);
    if (mimeType == "image/png" || mimeType == "image/apng")
        readPng(head, info);
    else if (mimeType == "image/webp")
        readWebp(head, info);
    else if (mimeType == "image/bmp")
        readBmp(head, info);
    else if (mimeType == "image/svg+xml")
        readSvg(head, info);
    else if (mimeType == "image/svg+xml-compressed")
        readSvg(gunzipHead(head, 64 * 1024), info);
    else if (mimeType == "image/jxl")
        readJxl(head, info);
    else if (mimeType == "image/vnd.microsoft.icon")
        readIco(head, info);
    else if (mimeType == "image/x-tga")
        readTga(head, info);
    else if (mimeType == "image/x-portable-pixmap" || mimeType == "image/x-portable-graymap"
             || mimeType == "image/x-portable-bitmap")
        readPnm(head, info);
    else if (mimeType == "image/x-xbitmap")
        readXbm(head, info);
    return info;
}

ImageInfo readImageInfo(const std::string& path, const std::string& mimeType)
{
    return saneSize(readHeader(path, mimeType));
}

} // namespace cutefish::ui
