// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

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

TEST_F(ImageInfoTest, TypesWithoutAHeaderReaderAreReadableWithoutASize)
{
    const ImageInfo info = readImageInfo(write("a.tif", std::string("II*\0", 4)), "image/tiff");
    EXPECT_TRUE(info.readable);
    EXPECT_EQ(info.width, 0);
    EXPECT_NE(std::ranges::find(imageMimeTypes(), "image/heif"), imageMimeTypes().end());
}
