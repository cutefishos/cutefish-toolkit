// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include <jpeglib.h>
#include <jxl/encode.h>
#include <png.h>
#include <tiffio.h>
#include <webp/encode.h>

#include "base/event_loop.h"
#include "ui/animated_image.h"
#include "ui/application.h"
#include "ui/bitmap.h"
#include "ui/painter.h"
#include "ui/window.h"

using namespace cutefish;
using ui::Bitmap;

namespace fs = std::filesystem;

static const fs::path kDirectory = "/tmp/cutefish-toolkit-color-test-" + std::to_string(getpid());

static constexpr int kSize = 16;
// A colour inside both gamuts, and what it is in sRGB when the file's
// numbers are Display P3 ones.
static constexpr uint8_t kColor[3] = {200, 100, 50};
static constexpr uint8_t kColorInSrgb[3] = {215, 93, 31};

// The columns of each RGB space's matrix to XYZ, adapted to D50.
static constexpr double kDisplayP3[3][3] = {
    {0.515102, 0.241182, -0.00104941},
    {0.291965, 0.692236, 0.0418818},
    {0.157153, 0.0665819, 0.784378},
};
static constexpr double kSrgb[3][3] = {
    {0.4360747, 0.2225045, 0.0139322},
    {0.3850649, 0.7168786, 0.0971045},
    {0.1430804, 0.0606169, 0.7141733},
};

static void putU32(std::string& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(char((value >> shift) & 0xFF));
}

static void putFixed(std::string& out, double value)
{
    putU32(out, uint32_t(int32_t(std::lround(value * 65536))));
}

static void putXyz(std::string& out, double x, double y, double z)
{
    out += "XYZ ";
    putU32(out, 0);
    putFixed(out, x);
    putFixed(out, y);
    putFixed(out, z);
}

// A version 2 display profile: three primaries, the D50 white and the sRGB
// curve shared by the three channels.
static std::string iccProfile(const double columns[3][3])
{
    std::string tags[5];
    for (int i = 0; i < 3; ++i)
        putXyz(tags[i], columns[i][0], columns[i][1], columns[i][2]);
    putXyz(tags[3], 0.9642, 1.0, 0.8249);
    tags[4] = "para";
    putU32(tags[4], 0);
    putU32(tags[4], 3u << 16);
    for (const double value : {2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045})
        putFixed(tags[4], value);

    const char* signatures[] = {"rXYZ", "gXYZ", "bXYZ", "wtpt", "rTRC", "gTRC", "bTRC"};
    const int contents[] = {0, 1, 2, 3, 4, 4, 4};
    const uint32_t tableEnd = 128 + 4 + 7 * 12;
    uint32_t offsets[5];
    uint32_t offset = tableEnd;
    for (int i = 0; i < 5; ++i) {
        offsets[i] = offset;
        offset += uint32_t(tags[i].size());
    }

    std::string out;
    putU32(out, offset);
    putU32(out, 0);
    putU32(out, 0x02100000);
    out += "mntrRGB XYZ ";
    out.append(12, '\0');
    out += "acsp";
    out.append(28, '\0');
    putFixed(out, 0.9642);
    putFixed(out, 1.0);
    putFixed(out, 0.8249);
    out.resize(128, '\0');
    putU32(out, 7);
    for (int i = 0; i < 7; ++i) {
        out += signatures[i];
        putU32(out, offsets[contents[i]]);
        putU32(out, uint32_t(tags[contents[i]].size()));
    }
    for (const std::string& tag : tags)
        out += tag;
    return out;
}

static std::vector<uint8_t> solid(int channels)
{
    std::vector<uint8_t> pixels(size_t(kSize) * kSize * size_t(channels));
    for (size_t i = 0; i < pixels.size(); i += size_t(channels)) {
        for (int c = 0; c < 3; ++c)
            pixels[i + size_t(c)] = kColor[c];
        if (channels == 4)
            pixels[i + 3] = 255;
    }
    return pixels;
}

static std::string writeFile(const std::string& name, const std::string& data)
{
    const std::string path = (kDirectory / name).string();
    FILE* file = std::fopen(path.c_str(), "wb");
    std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
    return path;
}

static std::string readFile(const std::string& path)
{
    std::string data;
    FILE* file = std::fopen(path.c_str(), "rb");
    char buffer[4096];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        data.append(buffer, read);
    std::fclose(file);
    return data;
}

static std::string writePng(const std::string& name, const std::string& icc)
{
    const std::string path = (kDirectory / name).string();
    FILE* file = std::fopen(path.c_str(), "wb");
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, file);
    png_set_IHDR(png, info, kSize, kSize, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    if (!icc.empty())
        png_set_iCCP(png, info, "Test", PNG_COMPRESSION_TYPE_BASE,
                     reinterpret_cast<png_const_bytep>(icc.data()), png_uint_32(icc.size()));
    png_write_info(png, info);
    std::vector<uint8_t> pixels = solid(3);
    for (int y = 0; y < kSize; ++y)
        png_write_row(png, pixels.data() + size_t(y) * kSize * 3);
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    return path;
}

static std::string writeJpeg(const std::string& icc)
{
    const std::string path = (kDirectory / "p3.jpg").string();
    FILE* file = std::fopen(path.c_str(), "wb");
    jpeg_compress_struct info;
    jpeg_error_mgr error;
    info.err = jpeg_std_error(&error);
    jpeg_create_compress(&info);
    jpeg_stdio_dest(&info, file);
    info.image_width = kSize;
    info.image_height = kSize;
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 100, TRUE);
    jpeg_start_compress(&info, TRUE);
    jpeg_write_icc_profile(&info, reinterpret_cast<const JOCTET*>(icc.data()), unsigned(icc.size()));
    std::vector<uint8_t> pixels = solid(3);
    while (info.next_scanline < info.image_height) {
        JSAMPROW row = pixels.data() + size_t(info.next_scanline) * kSize * 3;
        jpeg_write_scanlines(&info, &row, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    std::fclose(file);
    return path;
}

static void putLe(std::string& out, uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(char((value >> (8 * i)) & 0xFF));
}

static std::string chunk(const char* fourcc, const std::string& payload)
{
    std::string out = fourcc;
    putLe(out, uint32_t(payload.size()), 4);
    out += payload;
    if (payload.size() % 2)
        out.push_back('\0');
    return out;
}

// The extended layout: a VP8X header, the ICCP chunk, then the image.
static std::string writeWebp(const std::string& icc)
{
    std::vector<uint8_t> pixels = solid(3);
    uint8_t* output = nullptr;
    const size_t size = WebPEncodeLosslessRGB(pixels.data(), kSize, kSize, kSize * 3, &output);
    const std::string simple(reinterpret_cast<const char*>(output), size);
    WebPFree(output);
    std::string header;
    header.push_back(0x20);
    header.append(3, '\0');
    putLe(header, kSize - 1, 3);
    putLe(header, kSize - 1, 3);
    const std::string body = "WEBP" + chunk("VP8X", header) + chunk("ICCP", icc) + simple.substr(12);
    std::string file = "RIFF";
    putLe(file, uint32_t(body.size()), 4);
    return writeFile("p3.webp", file + body);
}

static std::string writeTiff(const std::string& icc)
{
    const std::string path = (kDirectory / "p3.tif").string();
    TIFF* tiff = TIFFOpen(path.c_str(), "w");
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, kSize);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, kSize);
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff, TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.data());
    std::vector<uint8_t> pixels = solid(3);
    for (int y = 0; y < kSize; ++y)
        TIFFWriteScanline(tiff, pixels.data() + size_t(y) * kSize * 3, uint32_t(y), 0);
    TIFFClose(tiff);
    return path;
}

// Lossless keeps the file's own colours; lossy goes through the codec's.
static std::string writeJxl(const std::string& name, bool lossless)
{
    JxlEncoder* encoder = JxlEncoderCreate(nullptr);
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = kSize;
    info.ysize = kSize;
    info.bits_per_sample = 8;
    info.uses_original_profile = lossless ? JXL_TRUE : JXL_FALSE;
    JxlEncoderSetBasicInfo(encoder, &info);
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    color.primaries = JXL_PRIMARIES_P3;
    JxlEncoderSetColorEncoding(encoder, &color);
    JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
    if (lossless)
        JxlEncoderSetFrameLossless(settings, JXL_TRUE);
    else
        JxlEncoderSetFrameDistance(settings, 0.1f);
    const JxlPixelFormat format{3, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    std::vector<uint8_t> pixels = solid(3);
    JxlEncoderAddImageFrame(settings, &format, pixels.data(), pixels.size());
    JxlEncoderCloseInput(encoder);
    std::vector<uint8_t> output(64 * 1024);
    uint8_t* next = output.data();
    size_t available = output.size();
    JxlEncoderProcessOutput(encoder, &next, &available);
    output.resize(output.size() - available);
    JxlEncoderDestroy(encoder);
    return writeFile(name, std::string(output.begin(), output.end()));
}

static bool isColor(uint32_t argb, const uint8_t expected[3], int tolerance)
{
    const int channels[3] = {int((argb >> 16) & 0xFF), int((argb >> 8) & 0xFF), int(argb & 0xFF)};
    for (int c = 0; c < 3; ++c)
        if (std::abs(channels[c] - int(expected[c])) > tolerance)
            return false;
    return true;
}

static std::string describe(uint32_t argb)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%u %u %u", (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    return text;
}

class ColorProfileTest : public testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        fs::remove_all(kDirectory);
        fs::create_directories(kDirectory);
    }
    static void TearDownTestSuite() { fs::remove_all(kDirectory); }
};

TEST_F(ColorProfileTest, ConvertsTaggedImagesToSrgb)
{
    const std::string p3 = iccProfile(kDisplayP3);
    struct Case
    {
        std::string path;
        int tolerance;
    };
    const std::vector<Case> cases = {
        {writePng("p3.png", p3), 1},       {writeJpeg(p3), 3},
        {writeWebp(p3), 1},                {writeTiff(p3), 1},
        {writeJxl("p3.jxl", true), 1},     {writeJxl("p3-lossy.jxl", false), 3},
    };
    for (const Case& test : cases) {
        const Bitmap bitmap = Bitmap::decodeFile(test.path);
        ASSERT_FALSE(bitmap.isNull()) << test.path;
        EXPECT_TRUE(isColor(bitmap.pixel(8, 8), kColorInSrgb, test.tolerance))
                << test.path << ": " << describe(bitmap.pixel(8, 8));
        const Bitmap fromBytes = Bitmap::decode(readFile(test.path));
        ASSERT_FALSE(fromBytes.isNull()) << test.path;
        EXPECT_EQ(fromBytes, bitmap) << test.path;
    }

    // Scaled while decoding, as thumbnails are.
    const Bitmap thumbnail = Bitmap::decodeFile(cases[1].path, base::Size(4, 4));
    ASSERT_EQ(thumbnail.width(), 4);
    EXPECT_TRUE(isColor(thumbnail.pixel(2, 2), kColorInSrgb, 3)) << describe(thumbnail.pixel(2, 2));

    const Bitmap fromBytes = Bitmap::decode(readFile(cases[0].path));
    EXPECT_TRUE(isColor(fromBytes.pixel(8, 8), kColorInSrgb, 1)) << describe(fromBytes.pixel(8, 8));
}

TEST_F(ColorProfileTest, ConvertsHeifByIccAndByCodePoints)
{
    // AV1 files of kColor: Display P3 by an ICC profile and by code points,
    // and sRGB by code points. libheif installs no encoder with its decoders.
    const std::string directory = CUTEFISH_TOOLKIT_TEST_DATA_DIR "/heif/";
    const std::string byIcc = directory + "p3-icc.heif";
    const std::string byCodePoints = directory + "p3-nclx.heif";
    const std::string plain = directory + "srgb-nclx.heif";
    for (const std::string& path : {byIcc, byCodePoints}) {
        const Bitmap bitmap = Bitmap::decodeFile(path);
        ASSERT_FALSE(bitmap.isNull()) << path;
        EXPECT_TRUE(isColor(bitmap.pixel(8, 8), kColorInSrgb, 4)) << path << ": " << describe(bitmap.pixel(8, 8));
    }
    const Bitmap bitmap = Bitmap::decodeFile(plain);
    EXPECT_TRUE(isColor(bitmap.pixel(8, 8), kColor, 4)) << describe(bitmap.pixel(8, 8));
}

TEST_F(ColorProfileTest, LeavesUntaggedAndSrgbImagesAlone)
{
    for (const std::string& path : {writePng("untagged.png", {}), writePng("srgb.png", iccProfile(kSrgb))}) {
        const Bitmap bitmap = Bitmap::decodeFile(path);
        ASSERT_FALSE(bitmap.isNull()) << path;
        EXPECT_TRUE(isColor(bitmap.pixel(8, 8), kColor, 0)) << path << ": " << describe(bitmap.pixel(8, 8));
    }
}

namespace {

class AnimationView : public ui::View
{
public:
    explicit AnimationView(ui::AnimatedImage* image) : m_image(image) {}
    void paint(ui::Painter& painter) override { m_image->draw(painter, localRect(), false); }

private:
    ui::AnimatedImage* m_image;
};

} // namespace

TEST_F(ColorProfileTest, ConvertsWebpAnimationFrames)
{
    const std::string path = writeWebp(iccProfile(kDisplayP3));
    ui::Window window;
    window.resize({kSize, kSize});
    ui::AnimatedImage image(&window);
    window.setContentView(std::make_unique<AnimationView>(&image));
    window.show();
    bool loaded = false;
    image.onLoaded = [&](bool ok) {
        loaded = ok;
        ui::Application::instance()->loop().quit();
    };
    image.load(path);
    const int timeout = ui::Application::instance()->loop().startTimer(
            std::chrono::seconds(5), false, [] { ui::Application::instance()->loop().quit(); });
    ui::Application::instance()->loop().run();
    ui::Application::instance()->loop().stopTimer(timeout);
    ASSERT_TRUE(loaded);
    const ui::Image grabbed = window.grab();
    const ui::Color color = grabbed.pixel(8, 8);
    const uint32_t argb = 0xFF000000u | uint32_t(color.r) << 16 | uint32_t(color.g) << 8 | uint32_t(color.b);
    EXPECT_TRUE(isColor(argb, kColorInSrgb, 1)) << describe(argb);
}
