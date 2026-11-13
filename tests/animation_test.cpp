// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>

#include "ui/animation.h"
#include "ui/view.h"
#include "ui/window.h"

using namespace cutefish::ui;
using namespace std::chrono_literals;

static constexpr uint64_t kStart = 1000000000;

static uint64_t at(std::chrono::milliseconds offset)
{
    return kStart + uint64_t(std::chrono::nanoseconds(offset).count());
}

TEST(AnimationTest, FollowsFrameTimes)
{
    Window window;
    window.resize({10, 10});
    window.setContentView(std::make_unique<View>());
    window.show();

    Animation animation(&window);
    animation.setDuration(1200ms);
    animation.setEasing(Animation::Easing::InOutCubic);
    animation.setLoops(Animation::Infinite);
    animation.setAlternating(true);
    double value = -1;
    animation.onUpdate = [&value](double v) { value = v; };
    animation.start();

    // The first frame anchors the start.
    window.grab(at(0ms));
    EXPECT_DOUBLE_EQ(value, 0);
    window.grab(at(600ms));
    EXPECT_DOUBLE_EQ(value, 0.5);
    window.grab(at(1500ms));
    EXPECT_DOUBLE_EQ(value, 0.9375);
}

TEST(AnimationTest, PauseKeepsProgress)
{
    Window window;
    window.resize({10, 10});
    window.setContentView(std::make_unique<View>());
    window.show();

    Animation animation(&window);
    animation.setDuration(1000ms);
    animation.setEasing(Animation::Easing::Linear);
    double value = -1;
    animation.onUpdate = [&value](double v) { value = v; };
    animation.start();

    window.grab(at(0ms));
    window.grab(at(250ms));
    animation.pause();
    window.grab(at(5000ms));
    EXPECT_DOUBLE_EQ(value, 0.25);
    animation.resume();
    window.grab(at(6000ms));
    window.grab(at(6500ms));
    EXPECT_DOUBLE_EQ(value, 0.75);
}

TEST(AnimationTest, FinishesAtItsEnd)
{
    Window window;
    window.resize({10, 10});
    window.setContentView(std::make_unique<View>());
    window.show();

    Animation animation(&window);
    animation.setDuration(100ms);
    bool finished = false;
    double value = -1;
    animation.onUpdate = [&value](double v) { value = v; };
    animation.onFinished = [&finished] { finished = true; };
    animation.start();

    window.grab(at(0ms));
    window.grab(at(150ms));
    EXPECT_TRUE(finished);
    EXPECT_FALSE(animation.isRunning());
    EXPECT_DOUBLE_EQ(value, 1);
}

TEST(AnimationTest, WindowMayCloseFromATick)
{
    auto window = std::make_unique<Window>();
    window->resize({10, 10});
    window->setContentView(std::make_unique<View>());
    window->show();

    Animation closing(window.get());
    closing.setDuration(100ms);
    closing.onUpdate = [&window](double v) {
        if (v > 0)
            window.reset();
    };
    closing.start();
    Animation after(window.get());
    after.setDuration(100ms);
    int updates = 0;
    after.onUpdate = [&updates](double) { ++updates; };
    after.start();

    Window* raw = window.get();
    raw->grab(at(0ms));
    EXPECT_EQ(updates, 1);
    EXPECT_TRUE(raw->grab(at(50ms)).isNull());
    EXPECT_EQ(window, nullptr);
    EXPECT_EQ(updates, 1);
}
