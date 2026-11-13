// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include "ui/image.h"
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
