// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <chrono>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "base/event_loop.h"

using namespace cutefish::base;
using namespace std::chrono_literals;

TEST(EventLoopTest, PostFromAnotherThreadWakesTheLoop)
{
    EventLoop loop;
    std::thread thread([&loop] { loop.post([&loop] { loop.quit(7); }); });
    EXPECT_EQ(loop.run(), 7);
    thread.join();
}

TEST(EventLoopTest, TimersFireInOrder)
{
    EventLoop loop;
    std::vector<int> fired;
    loop.startTimer(20ms, false, [&] { fired.push_back(2); });
    loop.startTimer(5ms, false, [&] { fired.push_back(1); });
    loop.startTimer(40ms, false, [&] { loop.quit(); });
    loop.run();
    EXPECT_EQ(fired, (std::vector<int>{1, 2}));
}

TEST(EventLoopTest, RepeatingTimerStopsFromItsCallback)
{
    EventLoop loop;
    int count = 0;
    int timer = 0;
    timer = loop.startTimer(2ms, true, [&] {
        if (++count == 3)
            loop.stopTimer(timer);
    });
    loop.startTimer(50ms, false, [&] { loop.quit(); });
    loop.run();
    EXPECT_EQ(count, 3);
}

TEST(EventLoopTest, PostedTasksMayPostMore)
{
    EventLoop loop;
    int depth = 0;
    std::function<void()> task = [&] {
        if (++depth < 5)
            loop.post(task);
        else
            loop.quit();
    };
    loop.post(task);
    loop.run();
    EXPECT_EQ(depth, 5);
}

TEST(EventLoopTest, DroppingPostedTasksMayStopTimers)
{
    auto loop = std::make_unique<EventLoop>();
    const int timer = loop->startTimer(1s, false, [] {});
    EventLoop* raw = loop.get();
    // Owned by the task, as a window a closing dialog posts for deletion.
    auto stopper = std::shared_ptr<int>(new int(0), [raw, timer](int* value) {
        raw->stopTimer(timer);
        delete value;
    });
    loop->post([stopper] {});
    stopper.reset();
    loop.reset();
}

// As a host's loop drives it: wait on the descriptor, then dispatch.
static bool waitReadable(int fd, int timeoutMs)
{
    pollfd entry{fd, POLLIN, 0};
    return poll(&entry, 1, timeoutMs) == 1 && (entry.revents & POLLIN);
}

TEST(EventLoopTest, DispatchesFromAnotherLoop)
{
    EventLoop loop;
    EXPECT_FALSE(waitReadable(loop.fd(), 0));
    // Nothing waits in dispatch(), even with nothing to do.
    loop.dispatch();

    bool fired = false;
    loop.startTimer(5ms, false, [&fired] { fired = true; });
    ASSERT_TRUE(waitReadable(loop.fd(), 1000));
    loop.dispatch();
    EXPECT_TRUE(fired);
    EXPECT_FALSE(waitReadable(loop.fd(), 0));

    int ran = 0;
    std::thread thread([&loop, &ran] { loop.post([&ran] { ++ran; }); });
    thread.join();
    ASSERT_TRUE(waitReadable(loop.fd(), 1000));
    loop.dispatch();
    EXPECT_EQ(ran, 1);

    // A task a task posts runs in the same dispatch.
    loop.post([&loop, &ran] { loop.post([&ran] { ++ran; }); });
    loop.dispatch();
    EXPECT_EQ(ran, 2);
}

// A host that queues the input a descriptor delivers handles it before what
// the same read posted, such as a frame, runs.
TEST(EventLoopTest, DispatchReceivedLeavesWhatTheDescriptorsPostForTheNextDispatch)
{
    EventLoop loop;
    int pipe[2];
    ASSERT_EQ(pipe2(pipe, O_NONBLOCK | O_CLOEXEC), 0);
    std::string order;
    const int watch = loop.watch(pipe[0], EPOLLIN, [&](uint32_t) {
        char byte;
        while (read(pipe[0], &byte, 1) == 1) {
        }
        order += 'r';
        loop.post([&order] { order += 'p'; });
    });
    loop.post([&order] { order += 'b'; });
    ASSERT_EQ(write(pipe[1], "x", 1), 1);
    ASSERT_TRUE(waitReadable(loop.fd(), 1000));
    loop.dispatchReceived();
    EXPECT_EQ(order, "br");
    order += 'h';
    EXPECT_TRUE(waitReadable(loop.fd(), 0));
    loop.dispatchReceived();
    EXPECT_EQ(order, "brhp");
    EXPECT_FALSE(waitReadable(loop.fd(), 0));
    loop.unwatch(watch);
    close(pipe[0]);
    close(pipe[1]);
}

TEST(EventLoopTest, DispatchRunsDueTimersAtOnce)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        EventLoop loop;
        int fired = 0;
        // Due at once: the descriptor would turn readable only after the slack.
        loop.startTimer(std::chrono::nanoseconds(0), false, [&] { ++fired; });
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        loop.dispatch();
        ASSERT_EQ(fired, 1) << "attempt " << attempt;
        // Not again when its descriptor turns readable later.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        loop.dispatch();
        ASSERT_EQ(fired, 1);
    }
}

TEST(EventLoopTest, DueRepeatingTimerSkipsMissedExpiries)
{
    EventLoop loop;
    int fired = 0;
    const int id = loop.startTimer(std::chrono::milliseconds(2), true, [&] { ++fired; });
    std::this_thread::sleep_for(std::chrono::milliseconds(9));
    loop.dispatch();
    EXPECT_EQ(fired, 1);
    loop.dispatch();
    EXPECT_EQ(fired, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    loop.dispatch();
    EXPECT_EQ(fired, 2);
    loop.stopTimer(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    loop.dispatch();
    EXPECT_EQ(fired, 2);
}

TEST(EventLoopTest, DueTimerMayStopAnother)
{
    EventLoop loop;
    int second = 0;
    int secondId = 0;
    loop.startTimer(std::chrono::nanoseconds(0), false, [&] { loop.stopTimer(secondId); });
    secondId = loop.startTimer(std::chrono::nanoseconds(0), false, [&] { ++second; });
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    loop.dispatch();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    loop.dispatch();
    EXPECT_EQ(second, 0);
}

TEST(EventLoopTest, PrepareHookRemovedByAnotherDoesNotRun)
{
    EventLoop loop;
    int second = 0;
    int secondId = 0;
    const int first = loop.addPrepareHook([&] { loop.removePrepareHook(secondId); });
    secondId = loop.addPrepareHook([&] { ++second; });
    loop.dispatch();
    EXPECT_EQ(second, 0);
    loop.removePrepareHook(first);
}
