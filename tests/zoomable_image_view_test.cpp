// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include "base/event_loop.h"
#include "base/timer.h"
#include "ui/application.h"
#include "ui/image.h"
#include "ui/test_input.h"
#include "ui/window.h"
#include "ui/zoomable_image_view.h"

using namespace cutefish::ui;
namespace fs = std::filesystem;

namespace {

// One per process: ctest runs the tests of this suite side by side.
const fs::path kDirectory = "/tmp/cutefish-toolkit-zoomable-image-test-" + std::to_string(getpid());

std::string writePng(const std::string& name, int width, int height)
{
    fs::create_directories(kDirectory);
    Image image(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            image.setPixel(x, y, Color::rgb(0xFF0000));
    const std::string path = (kDirectory / name).string();
    EXPECT_TRUE(image.savePng(path));
    return path;
}

class ZoomableImageViewTest : public testing::Test
{
protected:
    void SetUp() override
    {
        window = std::make_unique<Window>();
        window->resize({100, 100});
        auto content = std::make_unique<ZoomableImageView>();
        view = content.get();
        window->setContentView(std::move(content));
        window->show();
        window->grab();
    }

    static void TearDownTestSuite() { fs::remove_all(kDirectory); }

    void show(const std::string& path)
    {
        view->setImage(path, readImageInfo(path, "image/png"), {}, 1);
        window->grab();
    }

    std::unique_ptr<Window> window;
    ZoomableImageView* view = nullptr;
};

} // namespace

TEST_F(ZoomableImageViewTest, ZoomsAboutTheAnchorAndFitsAgainOnANewImage)
{
    const std::string path = writePng("wide.png", 200, 100);
    show(path);
    EXPECT_FALSE(view->isZoomed());

    // The centre of the fitted 100 x 50 frame stays under the view's centre.
    view->zoomTo(1, view->center(), false);
    EXPECT_TRUE(view->isZoomed());
    EXPECT_DOUBLE_EQ(view->scrollPosition().x, 50);
    EXPECT_DOUBLE_EQ(view->scrollPosition().y, 0);

    show(path);
    EXPECT_FALSE(view->isZoomed());
    EXPECT_DOUBLE_EQ(view->scrollPosition().x, 0);
}

TEST_F(ZoomableImageViewTest, SmallImagesAreNotEnlargedToFit)
{
    show(writePng("small.png", 20, 10));
    view->zoomTo(1, view->center(), false);
    EXPECT_FALSE(view->isZoomed());
    view->zoomTo(2, view->center(), false);
    EXPECT_TRUE(view->isZoomed());
}

TEST_F(ZoomableImageViewTest, APinchZoomsAboutThePointer)
{
    show(writePng("pinch.png", 400, 400));
    const PointF pointer {30, 70};
    TestInput::movePointer(*window, pointer);
    const PointF before = view->imagePoint(pointer);
    GestureEvent event;
    event.type = GestureEvent::Type::PinchBegin;
    TestInput::gesture(*window, event);
    event.type = GestureEvent::Type::PinchUpdate;
    event.scale = 3;
    TestInput::gesture(*window, event);
    event.type = GestureEvent::Type::PinchEnd;
    TestInput::gesture(*window, event);
    EXPECT_DOUBLE_EQ(view->zoom(), 0.75);
    const PointF after = view->imagePoint(pointer);
    EXPECT_NEAR(after.x, before.x, 0.5);
    EXPECT_NEAR(after.y, before.y, 0.5);

    // Not below the fit.
    event.type = GestureEvent::Type::PinchBegin;
    TestInput::gesture(*window, event);
    event.type = GestureEvent::Type::PinchUpdate;
    event.scale = 0.1;
    TestInput::gesture(*window, event);
    EXPECT_DOUBLE_EQ(view->zoom(), 0.25);
}

TEST_F(ZoomableImageViewTest, AQuarterTurnTurnsThePictureWithoutDecodingAgain)
{
    // Red on the left half, blue on the right.
    Image image(200, 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 200; ++x)
            image.setPixel(x, y, Color::rgb(x < 100 ? 0xFF0000 : 0x0000FF));
    const std::string path = (kDirectory / "halves.png").string();
    fs::create_directories(kDirectory);
    ASSERT_TRUE(image.savePng(path));
    show(path);
    // Until the fitted decode is in.
    auto& loop = Application::instance()->loop();
    cutefish::base::Timer poll(&loop);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    poll.onTimeout = [&] {
        if (!view->isLoading() || std::chrono::steady_clock::now() > deadline)
            loop.quit();
    };
    poll.start(std::chrono::milliseconds(5));
    loop.run();

    view->setQuarterTurns(1);
    Image shown = window->grab();
    // Turned clockwise: the left half is on top, in a 50 x 100 frame.
    EXPECT_EQ(shown.pixel(50, 25), Color::rgb(0xFF0000));
    EXPECT_EQ(shown.pixel(50, 75), Color::rgb(0x0000FF));
    EXPECT_EQ(view->imagePoint({50, 0}).y, 0);
    EXPECT_FALSE(view->isLoading());

    show(path);
    EXPECT_EQ(view->quarterTurns(), 0);
}

// Under a floating header 20 tall the image fits and centres below it, and
// only a zoomed image pans under it.
TEST_F(ZoomableImageViewTest, ATopInsetFitsBelowTheHeaderAndPansUnderIt)
{
    view->setTopInset(20);
    show(writePng("tall.png", 100, 200));
    window->grab();
    // 80 tall below the header: a 40 x 80 frame, centred there.
    EXPECT_DOUBLE_EQ(view->zoom(), 0.4);
    EXPECT_EQ(view->center(), (PointF {50, 60}));
    const PointF top = view->imagePoint({50, 20});
    EXPECT_NEAR(top.y, 0, 1e-9);
    EXPECT_DOUBLE_EQ(view->contentSize().height, 100);

    view->zoomTo(1, view->center(), false);
    // 200 tall from 20 down: 120 past the view's bottom at the most.
    view->setScrollPosition({0, 1000});
    EXPECT_DOUBLE_EQ(view->scrollPosition().y, 120);
    EXPECT_NEAR(view->imagePoint({50, 99.5}).y, 199.5, 1e-9);
    view->setScrollPosition({0, 0});
    EXPECT_NEAR(view->imagePoint({50, 20}).y, 0, 1e-9);
}
