// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cutefish::base {

class EventLoop;

// Lets another thread post to a loop that may be gone by the time it does;
// what it posts then is dropped.
class LoopHandle
{
public:
    LoopHandle() = default;

    // Returns false when the loop is gone.
    bool post(std::function<void()> task) const;

private:
    friend class EventLoop;
    struct Link
    {
        std::mutex mutex;
        EventLoop* loop = nullptr;
    };

    std::shared_ptr<Link> m_link;
};

// Dispatches file descriptors, timers and posted tasks on the thread that
// calls run(). Everything except post() and handle() belongs to that thread.
class EventLoop
{
public:
    using Task = std::function<void()>;
    using FdCallback = std::function<void(uint32_t events)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // The loop created last on the calling thread that still exists, or null.
    static EventLoop* current();

    LoopHandle handle() const;

    // Returns an id for unwatch(), or 0 when the descriptor cannot be watched.
    int watch(int fd, uint32_t events, FdCallback callback);
    // Changes the epoll events a watch waits for.
    void setWatchEvents(int id, uint32_t events);
    void unwatch(int id);

    int startTimer(std::chrono::nanoseconds interval, bool repeat, Task task);
    // Moves a running timer's next expiry to `interval` from now, with that
    // interval from then on, without a system call when the expiry only
    // moves later. False when the timer is not running.
    bool restartTimer(int id, std::chrono::nanoseconds interval);
    void stopTimer(int id);

    // Runs the task on the loop's next iteration; callable from any thread.
    void post(Task task);
    // Destroys the tasks posted but not run yet, without running them.
    void dropPosted();

    // Runs first thing when the loop is destroyed, for what lives as long as
    // the loop does.
    void addDestroyHook(Task hook);

    // Runs before every wait, e.g. to flush a connection.
    int addPrepareHook(Task hook);
    void removePrepareHook(int id);

    int run();
    void quit(int exitCode = 0);
    // For a loop run inside another one, which run() does not return from:
    // quit() calls the handler, so the outer loop can end the program.
    void setQuitHandler(std::function<void(int exitCode)> handler);

    // For a loop run inside another one, such as a host toolkit's loop: the
    // descriptor turns readable whenever there is work, timers and posted
    // tasks included, and dispatch() does that work without blocking.
    int fd() const { return m_epoll; }
    void dispatch();
    // Like dispatch(), but the tasks posted while the descriptors are read
    // wait for the next dispatch, which the descriptor stays readable for: a
    // host that queues the input it is handed handles it before, say, a frame
    // posted from the same read is drawn.
    void dispatchReceived();

private:
    struct Watch
    {
        int fd = -1;
        bool ownsFd = false;
        FdCallback callback;
    };

    struct TimerEntry
    {
        int fd = -1;
        std::chrono::steady_clock::time_point deadline;
        // What the descriptor is set to: a restart that only moves the
        // deadline later leaves it, and an early expiry rearms.
        std::chrono::steady_clock::time_point armed;
        std::chrono::nanoseconds interval{1};
        bool repeat = false;
        Task task;
    };

    // Runs the timer, and unwatches or rearms it first.
    void fireTimer(int id);
    // Timers whose deadline has passed, whether or not their descriptor has
    // turned readable yet: timer slack delays that by tens of microseconds.
    void runDueTimers();
    void runPosted();
    bool hasPosted();
    void runPrepareHooks();
    // False when waiting failed.
    bool dispatchEvents(int timeout);

    EventLoop* m_previous = nullptr;
    LoopHandle m_handle;
    int m_epoll = -1;
    int m_wakeFd = -1;
    int m_nextId = 1;
    bool m_running = false;
    int m_exitCode = 0;
    std::function<void(int)> m_quitHandler;
    // Shared so a callback can unwatch itself while it runs.
    std::unordered_map<int, std::shared_ptr<Watch>> m_watches;
    std::unordered_map<int, std::shared_ptr<TimerEntry>> m_timers;
    std::vector<std::pair<int, Task>> m_prepareHooks;
    std::vector<Task> m_destroyHooks;
    std::mutex m_postedMutex;
    std::vector<Task> m_posted;
};

} // namespace cutefish::base
