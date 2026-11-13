// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include <jxl/encode.h>
#include <tiffio.h>
#include <zlib.h>

#include "ui/image.h"
#include "ui/image_info.h"

using namespace cutefish::ui;
namespace fs = std::filesystem;

namespace {

// One per process: ctest runs the tests of this suite side by side.
const fs::path kDirectory = "/tmp/cutefish-toolkit-image-info-test-" + std::to_string(getpid());

std::string write(const std::string& name, const std::string& contents)
{
    fs::create_directories(kDirectory);
    const std::string path = (kDirectory / name).string();
    std::ofstream(path, std::ios::binary) << contents;
    return path;
}

std::string le16(int value)
{
    return {char(value & 0xff), char(value >> 8 & 0xff)};
}

std::string be32(uint32_t value)
{
    return {char(value >> 24), char(value >> 16 & 0xff), char(value >> 8 & 0xff), char(value & 0xff)};
}

std::string pngOf(int width, int height)
{
    return Image(width, height).encodePng();
}

std::string writeTiff(const std::string& name, int width, int height)
{
    const std::string path = (kDirectory / name).string();
    fs::create_directories(kDirectory);
    TIFF* tiff = TIFFOpen(path.c_str(), "w");
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
    std::string row(size_t(width), '\x80');
    for (int y = 0; y < height; ++y)
        TIFFWriteScanline(tiff, row.data(), uint32_t(y), 0);
    TIFFClose(tiff);
    return path;
}

// A grey JPEG XL with the orientation that turns it a quarter.
std::string jxlOf(int width, int height)
{
    JxlEncoder* encoder = JxlEncoderCreate(nullptr);
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = uint32_t(width);
    info.ysize = uint32_t(height);
    info.bits_per_sample = 8;
    info.num_color_channels = 1;
    info.orientation = JXL_ORIENT_ROTATE_90_CW;
    JxlEncoderSetBasicInfo(encoder, &info);
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_TRUE);
    JxlEncoderSetColorEncoding(encoder, &color);
    JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
    const std::string pixels(size_t(width) * size_t(height), '\x80');
    const JxlPixelFormat format {1, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlEncoderAddImageFrame(settings, &format, pixels.data(), pixels.size());
    JxlEncoderCloseInput(encoder);
    std::string output(64 * 1024, '\0');
    uint8_t* next = reinterpret_cast<uint8_t*>(output.data());
    size_t available = output.size();
    JxlEncoderProcessOutput(encoder, &next, &available);
    output.resize(output.size() - available);
    JxlEncoderDestroy(encoder);
    return output;
}

std::string gzipOf(const std::string& data)
{
    z_stream stream = {};
    deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::string out(data.size() + 256, '\0');
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = uInt(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = uInt(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(out.size() - stream.avail_out);
    deflateEnd(&stream);
    return out;
}

class ImageInfoTest : public testing::Test
{
protected:
    static void TearDownTestSuite() { fs::remove_all(kDirectory); }
};

} // namespace

TEST_F(ImageInfoTest, HeadersGiveSizeAndMotion)
{
    // A 3x2 PNG header; only the IHDR is read.
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\x0dIHDR", 8)
            + std::string("\0\0\0\x03\0\0\0\x02\x08\x06\0\0\0", 13) + std::string(4, '\0')
            + std::string("\0\0\0\0IEND", 8);
    ImageInfo info = readImageInfo(write("a.png", png), "image/png");
    EXPECT_TRUE(info.readable);
    EXPECT_EQ(info.width, 3);
    EXPECT_EQ(info.height, 2);
    EXPECT_FALSE(info.animated);

    info = readImageInfo(write("a.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 40 30\"/>"),
                         "image/svg+xml");
    EXPECT_TRUE(info.vector);
    EXPECT_EQ(info.width, 40);
    EXPECT_EQ(info.height, 30);

    // Two image descriptors make a GIF move.
    std::string gif = std::string("GIF89a", 6) + std::string("\x05\0\x04\0\0\0\0", 7);
    for (int frame = 0; frame < 2; ++frame)
        gif += std::string("\x2c\0\0\0\0\x05\0\x04\0\0", 10) + std::string("\x02\x02\x44\x01\0", 5);
    gif += ";";
    info = readImageInfo(write("a.gif", gif), "image/gif");
    EXPECT_EQ(info.width, 5);
    EXPECT_EQ(info.height, 4);
    EXPECT_TRUE(info.animated);
}

TEST_F(ImageInfoTest, OtherFilesAreNotReadable)
{
    EXPECT_FALSE(readImageInfo(write("a.txt", "plain text"), "image/png").readable);
    EXPECT_FALSE(readImageInfo(write("b.txt", "plain text"), "text/plain").readable);
}

TEST_F(ImageInfoTest, EveryTypeGivesItsSize)
{
    const auto size = [](const std::string& path, const char* type) {
        const ImageInfo info = readImageInfo(path, type);
        EXPECT_TRUE(info.readable) << type;
        return std::pair(info.width, info.height);
    };
    using Size = std::pair<int, int>;
    EXPECT_EQ(size(writeTiff("a.tif", 7, 5), "image/tiff"), Size(7, 5));
    // Turned a quarter by its orientation.
    EXPECT_EQ(size(write("a.jxl", jxlOf(7, 5)), "image/jxl"), Size(5, 7));
    // A 16 x 16 entry and a 48 x 32 PNG one, which is the larger.
    const std::string png = pngOf(48, 32);
    std::string ico = std::string("\0\0\1\0\2\0", 6);
    ico += std::string("\x10\x10\0\0\1\0\x20\0", 8) + std::string("\0\0\0\0", 4) + std::string("\0\0\0\0", 4);
    ico += std::string("\0\0\0\0\1\0\x20\0", 8) + std::string("\0\0\0\0", 4)
            + std::string({char(38), 0, 0, 0});
    ico += png;
    EXPECT_EQ(size(write("a.ico", ico), "image/vnd.microsoft.icon"), Size(48, 32));
    const std::string tga = std::string("\0\0\2", 3) + std::string(9, '\0') + le16(9) + le16(4) + "\x18" + '\0';
    EXPECT_EQ(size(write("a.tga", tga), "image/x-tga"), Size(9, 4));
    EXPECT_EQ(size(write("a.ppm", "P6\n# made by hand\n12 8\n255\n"), "image/x-portable-pixmap"), Size(12, 8));
    EXPECT_EQ(size(write("a.xbm", "#define a_width 10\n#define a_height 6\nstatic char a_bits[] = {};\n"),
                   "image/x-xbitmap"),
              Size(10, 6));
    const std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"30\" height=\"20\"/>";
    EXPECT_EQ(size(write("a.svgz", gzipOf(svg)), "image/svg+xml-compressed"), Size(30, 20));
    const std::string icns = "icns" + be32(uint32_t(16 + png.size())) + "ic08" + be32(uint32_t(8 + png.size())) + png;
    EXPECT_EQ(size(write("a.icns", icns), "image/x-icns"), Size(48, 32));
    EXPECT_NE(std::ranges::find(imageMimeTypes(), "image/heif"), imageMimeTypes().end());
}

TEST_F(ImageInfoTest, AFileDecodesToAnImageThatEncodesAsPng)
{
    const std::string path = write("b.png", pngOf(6, 4));
    const Image image = Image::fromFile(path);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.width(), 6);
    EXPECT_EQ(image.height(), 4);
    const ImageInfo info = readImageInfo(write("c.png", image.encodePng()), "image/png");
    EXPECT_EQ(info.width, 6);
    EXPECT_EQ(info.height, 4);
    EXPECT_TRUE(Image::fromFile(write("d.png", "not an image")).isNull());
}

TEST_F(ImageInfoTest, ImpossibleHeadersGiveNoSize)
{
    // An entry whose image starts near 4 GiB, past any file.
    std::string ico = std::string("\0\0\1\0\1\0", 6);
    ico += std::string("\x20\x20\0\0\1\0\x20\0", 8) + std::string("\0\0\0\0", 4) + std::string("\xf0\xff\xff\xff", 4);
    const ImageInfo icon = readImageInfo(write("far.ico", ico), "image/vnd.microsoft.icon");
    EXPECT_TRUE(icon.readable);
    EXPECT_EQ(icon.width, 32);

    // A BMP of negative width, and a PNG of 2^31 - 1 pixels a side.
    std::string bmp = "BM" + std::string(16, '\0') + std::string("\xf6\xff\xff\xff\x10\0\0\0", 8);
    EXPECT_EQ(readImageInfo(write("negative.bmp", bmp), "image/bmp").width, 0);
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string("\0\0\0\x0dIHDR", 8)
            + std::string("\x7f\xff\xff\xff\x7f\xff\xff\xff\x08\x06\0\0\0", 13) + std::string(4, '\0');
    const ImageInfo huge = readImageInfo(write("huge.png", png), "image/png");
    EXPECT_TRUE(huge.readable);
    EXPECT_EQ(huge.width, 0);
    EXPECT_EQ(huge.height, 0);
}
