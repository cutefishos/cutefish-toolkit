// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <jpeglib.h>
#include <jxl/encode.h>
#include <tiffio.h>
#include <webp/encode.h>

#include "base/event_loop.h"
#include "base/paths.h"
#include "base/timer.h"
#include "ui/application.h"
#include "ui/image_view.h"
#include "ui/painter.h"
#include "ui/window.h"

using namespace cutefish::ui;
namespace fs = std::filesystem;

namespace {

constexpr Color kRed = Color::rgb(0xFF0000);
constexpr Color kBlue = Color::rgb(0x0000FF);
constexpr Color kWhite = Color::rgb(0xFFFFFF);

// One per process: ctest runs the tests of this suite side by side.
const fs::path kDirectory = "/tmp/cutefish-toolkit-image-test-" + std::to_string(getpid());
// The thumbnail cache keys are this fixed path's, so build trees testing at
// the same time take turns through the lock file next to it.
const fs::path kThumbnailDirectory = "/tmp/cutefish-toolkit-thumbnail-test";
const fs::path kThumbnailLock = "/tmp/cutefish-toolkit-thumbnail-test.lock";

bool near(Color a, Color b, int tolerance = 6)
{
    return std::abs(a.r - b.r) <= tolerance && std::abs(a.g - b.g) <= tolerance && std::abs(a.b - b.b) <= tolerance;
}

// Red on the left half, blue on the right, as RGBA bytes.
std::vector<uint8_t> halves(int width, int height)
{
    std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t* p = &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4];
            const bool left = x < width / 2;
            p[0] = left ? 255 : 0;
            p[1] = 0;
            p[2] = left ? 0 : 255;
            p[3] = 255;
        }
    }
    return pixels;
}

std::string writePng(const std::string& name, int width, int height, const fs::path& directory = kDirectory)
{
    Image image(width, height);
    const std::vector<uint8_t> pixels = halves(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4];
            image.setPixel(x, y, Color::rgb(uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]));
        }
    const std::string path = (directory / name).string();
    EXPECT_TRUE(image.savePng(path));
    return path;
}

std::string writeWebp(int width, int height)
{
    const std::vector<uint8_t> pixels = halves(width, height);
    uint8_t* output = nullptr;
    const size_t size = WebPEncodeLosslessRGBA(pixels.data(), width, height, width * 4, &output);
    const std::string path = (kDirectory / "halves.webp").string();
    FILE* file = std::fopen(path.c_str(), "wb");
    std::fwrite(output, 1, size, file);
    std::fclose(file);
    WebPFree(output);
    return path;
}

std::string writeTiff(int width, int height)
{
    const std::vector<uint8_t> pixels = halves(width, height);
    const std::string path = (kDirectory / "halves.tif").string();
    TIFF* tiff = TIFFOpen(path.c_str(), "w");
    TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    const uint16_t extra[] = {EXTRASAMPLE_UNASSALPHA};
    TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, extra);
    for (int y = 0; y < height; ++y)
        TIFFWriteScanline(tiff, const_cast<uint8_t*>(&pixels[size_t(y) * size_t(width) * 4]), uint32_t(y), 0);
    TIFFClose(tiff);
    return path;
}

std::string writeJxl(int width, int height)
{
    const std::vector<uint8_t> pixels = halves(width, height);
    JxlEncoder* encoder = JxlEncoderCreate(nullptr);
    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = uint32_t(width);
    info.ysize = uint32_t(height);
    info.bits_per_sample = 8;
    info.num_extra_channels = 1;
    info.alpha_bits = 8;
    info.uses_original_profile = JXL_TRUE;
    JxlEncoderSetBasicInfo(encoder, &info);
    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, JXL_FALSE);
    JxlEncoderSetColorEncoding(encoder, &color);
    JxlEncoderFrameSettings* settings = JxlEncoderFrameSettingsCreate(encoder, nullptr);
    JxlEncoderSetFrameLossless(settings, JXL_TRUE);
    const JxlPixelFormat format{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    JxlEncoderAddImageFrame(settings, &format, pixels.data(), pixels.size());
    JxlEncoderCloseInput(encoder);
    std::vector<uint8_t> output(64 * 1024);
    uint8_t* next = output.data();
    size_t available = output.size();
    JxlEncoderProcessOutput(encoder, &next, &available);
    output.resize(output.size() - available);
    JxlEncoderDestroy(encoder);
    const std::string path = (kDirectory / "halves.jxl").string();
    FILE* file = std::fopen(path.c_str(), "wb");
    std::fwrite(output.data(), 1, output.size(), file);
    std::fclose(file);
    return path;
}

// A JPEG whose EXIF orientation says to turn it 90 degrees clockwise.
std::string writeTurnedJpeg(int width, int height)
{
    const std::vector<uint8_t> rgba = halves(width, height);
    const std::string path = (kDirectory / "turned.jpg").string();
    FILE* file = std::fopen(path.c_str(), "wb");
    jpeg_compress_struct info;
    jpeg_error_mgr error;
    info.err = jpeg_std_error(&error);
    jpeg_create_compress(&info);
    jpeg_stdio_dest(&info, file);
    info.image_width = JDIMENSION(width);
    info.image_height = JDIMENSION(height);
    info.input_components = 3;
    info.in_color_space = JCS_RGB;
    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 95, TRUE);
    jpeg_start_compress(&info, TRUE);
    // "Exif\0\0", a little-endian TIFF header and one IFD entry: orientation 6.
    const uint8_t exif[] = {'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 0x01, 3, 0,
                            1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0};
    jpeg_write_marker(&info, JPEG_APP0 + 1, exif, sizeof(exif));
    std::vector<uint8_t> row(size_t(width) * 3);
    while (info.next_scanline < info.image_height) {
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c)
                row[size_t(x) * 3 + size_t(c)] = rgba[(size_t(info.next_scanline) * size_t(width) + size_t(x)) * 4 + size_t(c)];
        JSAMPROW pointer = row.data();
        jpeg_write_scanlines(&info, &pointer, 1);
    }
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);
    std::fclose(file);
    return path;
}

// Runs the loop until every view has its picture, or five seconds pass.
bool waitForLoads(const std::vector<ImageView*>& views)
{
    auto& loop = Application::instance()->loop();
    const auto loading = [&views] {
        for (ImageView* view : views)
            if (view->status() == ImageView::Status::Loading)
                return true;
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    cutefish::base::Timer poll(&loop);
    poll.onTimeout = [&] {
        if (!loading() || std::chrono::steady_clock::now() > deadline)
            loop.quit();
    };
    poll.start(std::chrono::milliseconds(5));
    loop.run();
    return !loading();
}

struct Shown
{
    std::unique_ptr<Window> window;
    ImageView* view = nullptr;
};

Shown show(const std::string& path, SizeF size, ImageView::FillMode mode, double scale = 1,
           std::function<void(ImageView*)> setUp = {})
{
    Shown shown;
    shown.window = std::make_unique<Window>();
    shown.window->setScale(scale);
    shown.window->resize(size);
    shown.window->setBackgroundColor(kWhite);
    auto view = std::make_unique<ImageView>();
    shown.view = view.get();
    view->setFillMode(mode);
    if (setUp)
        setUp(view.get());
    view->setSource(path);
    shown.window->setContentView(std::move(view));
    shown.window->show();
    shown.window->grab();
    return shown;
}

struct FileLock
{
    explicit FileLock(const fs::path& path)
        : fd(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600))
    {
        if (fd >= 0)
            flock(fd, LOCK_EX);
    }
    ~FileLock()
    {
        if (fd >= 0)
            close(fd);
    }
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    int fd;
};

class ImageViewTest : public testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        fs::remove_all(kDirectory);
        fs::create_directories(kDirectory);
    }
    static void TearDownTestSuite() { fs::remove_all(kDirectory); }
};

} // namespace

TEST_F(ImageViewTest, StretchesFitsCropsAndPads)
{
    const std::string path = writePng("halves.png", 40, 20);

    Shown stretch = show(path, {40, 20}, ImageView::FillMode::Stretch);
    ASSERT_TRUE(waitForLoads({stretch.view}));
    EXPECT_EQ(stretch.view->status(), ImageView::Status::Ready);
    Image image = stretch.window->grab();
    EXPECT_TRUE(near(image.pixel(5, 10), kRed));
    EXPECT_TRUE(near(image.pixel(35, 10), kBlue));

    // A 40 x 20 picture in a 40 x 40 view: centred between white bands.
    Shown fit = show(path, {40, 40}, ImageView::FillMode::PreserveAspectFit);
    ASSERT_TRUE(waitForLoads({fit.view}));
    image = fit.window->grab();
    EXPECT_TRUE(near(image.pixel(5, 5), kWhite));
    EXPECT_TRUE(near(image.pixel(5, 20), kRed));
    EXPECT_TRUE(near(image.pixel(35, 20), kBlue));
    EXPECT_TRUE(near(image.pixel(35, 35), kWhite));

    // Scaled to 80 x 40 and cut to the middle: half red, half blue.
    Shown crop = show(path, {40, 40}, ImageView::FillMode::PreserveAspectCrop);
    ASSERT_TRUE(waitForLoads({crop.view}));
    image = crop.window->grab();
    EXPECT_TRUE(near(image.pixel(5, 5), kRed));
    EXPECT_TRUE(near(image.pixel(35, 35), kBlue));

    Shown pad = show(path, {60, 60}, ImageView::FillMode::Pad);
    ASSERT_TRUE(waitForLoads({pad.view}));
    image = pad.window->grab();
    EXPECT_TRUE(near(image.pixel(5, 30), kWhite));
    EXPECT_TRUE(near(image.pixel(15, 30), kRed));
    EXPECT_TRUE(near(image.pixel(45, 30), kBlue));
    EXPECT_TRUE(near(image.pixel(30, 15), kWhite));
    EXPECT_EQ(pad.view->sizeHint(), (SizeF{40, 20}));
}

TEST_F(ImageViewTest, DecodesAtTheDeviceSize)
{
    const std::string path = writePng("large.png", 400, 200);
    Shown shown = show(path, {40, 20}, ImageView::FillMode::Stretch, 1.5);
    ASSERT_TRUE(waitForLoads({shown.view}));
    const Image image = shown.window->grab();
    ASSERT_EQ(image.width(), 60);
    EXPECT_TRUE(near(image.pixel(10, 15), kRed));
    EXPECT_TRUE(near(image.pixel(50, 15), kBlue));
}

TEST_F(ImageViewTest, ReportsStatus)
{
    std::vector<ImageView::Status> seen;
    Shown missing = show((kDirectory / "missing.png").string(), {20, 20}, ImageView::FillMode::Stretch, 1,
                         [&seen](ImageView* view) {
                             view->onStatusChanged = [&seen](ImageView::Status status) { seen.push_back(status); };
                         });
    ASSERT_TRUE(waitForLoads({missing.view}));
    EXPECT_EQ(missing.view->status(), ImageView::Status::Error);
    EXPECT_EQ(seen, (std::vector{ImageView::Status::Loading, ImageView::Status::Error}));
    missing.view->setSource({});
    EXPECT_EQ(missing.view->status(), ImageView::Status::Null);
}

// A file written over under the same name, as a service that keeps one copy
// per account does, shows its new picture when it is set again.
TEST_F(ImageViewTest, DecodesAFileWrittenOverAgain)
{
    const std::string path = writePng("written-over.png", 40, 20);
    Shown shown = show(path, {60, 60}, ImageView::FillMode::Pad);
    ASSERT_TRUE(waitForLoads({shown.view}));
    EXPECT_EQ(shown.view->sizeHint(), (SizeF{40, 20}));

    shown.view->setSource(path);
    EXPECT_EQ(shown.view->status(), ImageView::Status::Ready);

    writePng("written-over.png", 20, 40);
    shown.view->setSource(path);
    // The old picture stays while the new one decodes.
    EXPECT_EQ(shown.view->status(), ImageView::Status::Ready);
    auto& loop = Application::instance()->loop();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    cutefish::base::Timer poll(&loop);
    poll.onTimeout = [&] {
        shown.window->grab();
        if (shown.view->sizeHint() == SizeF{20, 40} || std::chrono::steady_clock::now() > deadline)
            loop.quit();
    };
    poll.start(std::chrono::milliseconds(5));
    loop.run();
    EXPECT_EQ(shown.view->sizeHint(), (SizeF{20, 40}));
    const Image image = shown.window->grab();
    EXPECT_TRUE(near(image.pixel(25, 15), kRed));
    EXPECT_TRUE(near(image.pixel(35, 15), kBlue));
    EXPECT_TRUE(near(image.pixel(10, 30), kWhite));
}

TEST_F(ImageViewTest, CutsToRoundedCorners)
{
    const std::string path = writePng("corners.png", 40, 40);
    Shown shown = show(path, {40, 40}, ImageView::FillMode::Stretch, 1,
                       [](ImageView* view) { view->setCornerRadius(12); });
    ASSERT_TRUE(waitForLoads({shown.view}));
    const Image image = shown.window->grab();
    EXPECT_TRUE(near(image.pixel(0, 0), kWhite));
    EXPECT_TRUE(near(image.pixel(39, 39), kWhite));
    EXPECT_TRUE(near(image.pixel(12, 20), kRed));
}

TEST_F(ImageViewTest, ShowsPixels)
{
    // Half-transparent red, straight and premultiplied.
    Window window;
    window.resize({20, 10});
    window.setBackgroundColor(kWhite);
    auto content = std::make_unique<View>();
    auto* straight = content->addChild<ImageView>();
    straight->setGeometry({0, 0, 10, 10});
    auto* premultiplied = content->addChild<ImageView>();
    premultiplied->setGeometry({10, 0, 10, 10});
    std::vector<uint32_t> straightPixels(100, 0x80FF0000);
    std::vector<uint32_t> premultipliedPixels(100, 0x80800000);
    straight->setPixels(10, 10, straightPixels.data(), 40, false);
    premultiplied->setPixels(10, 10, premultipliedPixels.data(), 40, true);
    EXPECT_EQ(straight->status(), ImageView::Status::Ready);
    window.setContentView(std::move(content));
    window.show();
    const Image image = window.grab();
    const Color pink = Color::rgb(0xFF7F7F);
    EXPECT_TRUE(near(image.pixel(5, 5), pink, 3));
    EXPECT_TRUE(near(image.pixel(15, 5), pink, 3));
}

TEST_F(ImageViewTest, ReadsOtherFormats)
{
    for (const std::string& path : {writeWebp(40, 20), writeTiff(40, 20), writeJxl(40, 20)}) {
        Shown shown = show(path, {40, 20}, ImageView::FillMode::Stretch);
        ASSERT_TRUE(waitForLoads({shown.view})) << path;
        ASSERT_EQ(shown.view->status(), ImageView::Status::Ready) << path;
        const Image image = shown.window->grab();
        EXPECT_TRUE(near(image.pixel(5, 10), kRed)) << path;
        EXPECT_TRUE(near(image.pixel(35, 10), kBlue)) << path;
    }
}

TEST_F(ImageViewTest, TurnsByExifOnlyWhenAsked)
{
    const std::string path = writeTurnedJpeg(40, 20);
    Shown plain = show(path, {40, 40}, ImageView::FillMode::Pad);
    ASSERT_TRUE(waitForLoads({plain.view}));
    EXPECT_EQ(plain.view->sizeHint(), (SizeF{40, 20}));

    Shown turned = show(path, {40, 40}, ImageView::FillMode::Pad, 1,
                        [](ImageView* view) { view->setAutoTransform(true); });
    ASSERT_TRUE(waitForLoads({turned.view}));
    EXPECT_EQ(turned.view->sizeHint(), (SizeF{20, 40}));
    // Turned clockwise, the left half goes to the top.
    const Image image = turned.window->grab();
    EXPECT_TRUE(near(image.pixel(20, 8), kRed, 30));
    EXPECT_TRUE(near(image.pixel(20, 32), kBlue, 30));
}

TEST_F(ImageViewTest, SharesTheFreedesktopThumbnailCache)
{
    const FileLock lock(kThumbnailLock);
    ASSERT_GE(lock.fd, 0);
    fs::remove_all(kThumbnailDirectory);
    fs::create_directories(kThumbnailDirectory);
    const std::string path = writePng("a b[1].png", 300, 150, kThumbnailDirectory);
    // The URI key is file:///tmp/cutefish-toolkit-thumbnail-test/a%20b[1].png.
    const fs::path cached = fs::path(cutefish::base::paths::cacheHome()) / "thumbnails/large/c32fda64f3ffbd3ab831d5adc027bd5e.png";
    fs::remove(cached);

    Shown shown = show(path, {200, 100}, ImageView::FillMode::PreserveAspectCrop, 1,
                       [](ImageView* view) { view->setThumbnail(true); });
    ASSERT_TRUE(waitForLoads({shown.view}));
    EXPECT_EQ(shown.view->status(), ImageView::Status::Ready);
    ASSERT_TRUE(fs::exists(cached));
    struct stat info;
    ASSERT_EQ(stat(cached.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600u);
    // The largest flavour that holds 200 is 256, and the picture keeps its aspect.
    EXPECT_EQ(shown.view->sizeHint(), (SizeF{256, 128}));
    fs::remove(cached);

    const std::string broken = (kThumbnailDirectory / "broken.png").string();
    FILE* file = std::fopen(broken.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    std::fputs("not an image", file);
    std::fclose(file);
    const fs::path marker = fs::path(cutefish::base::paths::cacheHome())
            / "thumbnails/fail/cutefish/7727b713e72d1077247ab07020b1c845.png";
    fs::remove(marker);
    Shown failed = show(broken, {100, 100}, ImageView::FillMode::Stretch, 1,
                        [](ImageView* view) { view->setThumbnail(true); });
    ASSERT_TRUE(waitForLoads({failed.view}));
    EXPECT_EQ(failed.view->status(), ImageView::Status::Error);
    EXPECT_TRUE(fs::exists(marker));
    fs::remove(marker);
    fs::remove_all(kThumbnailDirectory);
}

TEST_F(ImageViewTest, DrawsAsAMaskedLayer)
{
    // Reference pixels of the wallpaper selection's image (mipmapped and
    // smooth, masked through layers by a rounded rectangle of radius 12 with
    // continuous corners) showing this picture at 150%.
    std::vector<uint32_t> pixels(256 * 160);
    for (int y = 0; y < 160; ++y)
        for (int x = 0; x < 256; ++x)
            pixels[size_t(y) * 256 + size_t(x)] = x < 128 ? 0xFFE6281E : 0xFF143CDC;
    Window window;
    window.setScale(1.5);
    window.resize({300, 200});
    window.setBackgroundColor(kWhite);
    auto content = std::make_unique<View>();
    auto* view = content->addChild<ImageView>();
    // The item as the wallpaper selection anchors it, 0.375 / dpr down and right.
    view->setGeometry({30.1666 + 4 + 0.25, 40.5 + 4 + 0.25, 128.75 - 8, 80.333 - 8});
    view->setFillMode(ImageView::FillMode::PreserveAspectCrop);
    view->setMipmap(true);
    view->setCornerRadius(12, Corners::Continuous);
    view->setRendering(ImageView::Rendering::MaskedLayer);
    view->setPixels(256, 160, pixels.data(), 256 * 4, false);
    window.setContentView(std::move(content));
    window.show();
    const Image image = window.grab();
    const struct
    {
        int x, y;
        uint32_t rgb;
    } expected[] = {
        {52, 100, 0xE6281F},  {53, 100, 0xE6281E},  {142, 100, 0x4538B0}, {143, 100, 0x173CD9},
        {231, 100, 0x1B42DD}, {232, 100, 0x3255E0}, {233, 100, 0xFFFFFF}, {140, 67, 0xE62A20},
        {140, 68, 0xE6281E},  {140, 174, 0xEF756F}, {140, 175, 0xFFFFFF}, {52, 68, 0xFFFFFF},
        {54, 70, 0xFFFFFF},   {60, 75, 0xE6281E},   {230, 174, 0xFFFFFF}, {225, 170, 0x5572E6},
        {52, 175, 0xFFFFFF},
    };
    for (const auto& point : expected)
        EXPECT_TRUE(near(image.pixel(point.x, point.y), Color::rgb(point.rgb), 2))
                << point.x << "," << point.y << " " << std::hex << image.pixel(point.x, point.y).argb();
}

TEST_F(ImageViewTest, ShowsPicturesThatFinishTogetherTogether)
{
    // Results arriving within a refresh of one another reach the views in one
    // turn of the loop, so they show in the same frame.
    Window window;
    window.resize({240, 40});
    auto content = std::make_unique<View>();
    std::vector<ImageView*> views;
    std::vector<std::chrono::steady_clock::time_point> readyAt(6);
    for (int i = 0; i < 6; ++i) {
        const std::string path = writePng("together" + std::to_string(i) + ".png", 400, 200);
        auto* view = content->addChild<ImageView>();
        view->setGeometry({double(i) * 40, 0, 40, 20});
        view->onStatusChanged = [&readyAt, i](ImageView::Status status) {
            if (status == ImageView::Status::Ready)
                readyAt[size_t(i)] = std::chrono::steady_clock::now();
        };
        view->setSource(path);
        views.push_back(view);
    }
    window.setContentView(std::move(content));
    window.show();
    window.grab();
    ASSERT_TRUE(waitForLoads(views));
    const auto [first, last] = std::minmax_element(readyAt.begin(), readyAt.end());
    const double spread = std::chrono::duration<double, std::milli>(*last - *first).count();
    EXPECT_LT(spread, 1.0);
}

TEST_F(ImageViewTest, DrawsSvgAsTheIconLoaderDoes)
{
    // Style sheet classes, as theme icons use them.
    const std::string path = (kDirectory / "styled.svg").string();
    FILE* file = std::fopen(path.c_str(), "wb");
    std::fputs("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
               "<style>.plate{fill:#ff0000}</style><rect class=\"plate\" width=\"16\" height=\"16\"/></svg>",
               file);
    std::fclose(file);
    Shown shown = show(path, {32, 32}, ImageView::FillMode::PreserveAspectFit);
    ASSERT_TRUE(waitForLoads({shown.view}));
    EXPECT_TRUE(near(shown.window->grab().pixel(16, 16), kRed));
}

namespace {

class ImagePainter : public View
{
public:
    explicit ImagePainter(Image image) : m_image(std::move(image)) {}

protected:
    void paint(Painter& painter) override { painter.drawImage(m_image, {10, 10, 20, 20}); }

private:
    Image m_image;
};

} // namespace

TEST_F(ImageViewTest, PainterScalesImagesIntoRects)
{
    Image picture(44, 44);
    for (int y = 0; y < 44; ++y)
        for (int x = 0; x < 44; ++x)
            picture.setPixel(x, y, x < 22 ? kRed : kBlue);
    Window window;
    window.setScale(2);
    window.resize({40, 40});
    window.setBackgroundColor(kWhite);
    window.setContentView(std::make_unique<ImagePainter>(picture));
    window.show();
    const Image image = window.grab();
    EXPECT_TRUE(near(image.pixel(19, 19), kWhite));
    EXPECT_TRUE(near(image.pixel(24, 40), kRed));
    EXPECT_TRUE(near(image.pixel(56, 40), kBlue));
    EXPECT_TRUE(near(image.pixel(61, 61), kWhite));
}

