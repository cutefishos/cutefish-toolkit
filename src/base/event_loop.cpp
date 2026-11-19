// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "base/auto_reset.h"

namespace cutefish::base {

static thread_local EventLoop* s_current = nullptr;
// The loop whose run() is innermost on the calling thread.
static thread_local EventLoop* s_running = nullptr;

bool LoopHandle::post(std::function<void()> task) const
{
    if (!m_link)
        return false;
    std::lock_guard lock(m_link->mutex);
    if (!m_link->loop)
        return false;
    m_link->loop->post(std::move(task));
    return true;
}

EventLoop::EventLoop()
    : m_previous(s_current)
{
    s_current = this;
    m_handle.m_link = std::make_shared<LoopHandle::Link>();
    m_handle.m_link->loop = this;
    m_epoll = epoll_create1(EPOLL_CLOEXEC);
    m_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    watch(m_wakeFd, EPOLLIN, [this](uint32_t) {
        uint64_t count;
        while (read(m_wakeFd, &count, sizeof(count)) > 0) {
        }
    });
}

EventLoop::~EventLoop()
{
    // A hook may add hooks.
    while (!m_destroyHooks.empty()) {
        Task hook = std::move(m_destroyHooks.back());
        m_destroyHooks.pop_back();
        hook();
    }
    // A dropped task may own views whose destructors still unwatch here.
    dropPosted();
    {
        std::lock_guard lock(m_handle.m_link->mutex);
        m_handle.m_link->loop = nullptr;
    }
    if (s_current == this)
        s_current = m_previous;
    for (const auto& [id, watch] : m_watches) {
        if (watch->ownsFd)
            close(watch->fd);
    }
    close(m_wakeFd);
    close(m_epoll);
}

int EventLoop::watch(int fd, uint32_t events, FdCallback callback)
{
    const int id = m_nextId++;
    epoll_event event{};
    event.events = events;
    event.data.u64 = uint64_t(id);
    if (epoll_ctl(m_epoll, EPOLL_CTL_ADD, fd, &event) < 0) {
        perror("epoll_ctl");
        return 0;
    }

    auto watch = std::make_shared<Watch>();
    watch->fd = fd;
    watch->callback = std::move(callback);
    m_watches.emplace(id, std::move(watch));
    return id;
}

EventLoop* EventLoop::current()
{
    return s_current;
}

LoopHandle EventLoop::handle() const
{
    return m_handle;
}

void EventLoop::setWatchEvents(int id, uint32_t events)
{
    const auto it = m_watches.find(id);
    if (it == m_watches.end())
        return;

    epoll_event event{};
    event.events = events;
    event.data.u64 = uint64_t(id);
    epoll_ctl(m_epoll, EPOLL_CTL_MOD, it->second->fd, &event);
}

void EventLoop::unwatch(int id)
{
    const auto it = m_watches.find(id);
    if (it == m_watches.end())
        return;

    epoll_ctl(m_epoll, EPOLL_CTL_DEL, it->second->fd, nullptr);
    if (it->second->ownsFd)
        close(it->second->fd);
    m_watches.erase(it);
    m_timers.erase(id);
}

static void arm(int fd, std::chrono::steady_clock::time_point deadline)
{
    // steady_clock is CLOCK_MONOTONIC.
    const auto sinceBoot = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch());
    const int64_t nanoseconds = std::max<int64_t>(sinceBoot.count(), 1);
    itimerspec spec{};
    spec.it_value.tv_sec = time_t(nanoseconds / 1000000000);
    spec.it_value.tv_nsec = long(nanoseconds % 1000000000);
    timerfd_settime(fd, TFD_TIMER_ABSTIME, &spec, nullptr);
}

int EventLoop::startTimer(std::chrono::nanoseconds interval, bool repeat, Task task)
{
    const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) {
        perror("timerfd_create");
        return 0;
    }

    auto timer = std::make_shared<TimerEntry>();
    timer->fd = fd;
    timer->interval = std::max(interval, std::chrono::nanoseconds(1));
    timer->deadline = std::chrono::steady_clock::now() + timer->interval;
    timer->repeat = repeat;
    timer->task = std::move(task);
    // Absolute and one-shot: repeats are rearmed from the deadline.
    arm(fd, timer->deadline);
    timer->armed = timer->deadline;

    const int id = watch(fd, EPOLLIN, nullptr);
    if (!id) {
        close(fd);
        return 0;
    }
    auto& watch = m_watches.at(id);
    watch->ownsFd = true;
    watch->callback = [this, id, fd](uint32_t) {
        uint64_t expirations;
        // Nothing when the due-timer pass rearmed it first.
        if (read(fd, &expirations, sizeof(expirations)) <= 0)
            return;
        // A restart pushed the deadline past what the descriptor was set to.
        const auto it = m_timers.find(id);
        if (it != m_timers.end() && it->second->deadline > std::chrono::steady_clock::now()) {
            arm(fd, it->second->deadline);
            it->second->armed = it->second->deadline;
            return;
        }
        fireTimer(id);
    };
    m_timers.emplace(id, std::move(timer));
    return id;
}

bool EventLoop::restartTimer(int id, std::chrono::nanoseconds interval)
{
    const auto it = m_timers.find(id);
    if (it == m_timers.end())
        return false;
    TimerEntry& timer = *it->second;
    timer.interval = std::max(interval, std::chrono::nanoseconds(1));
    timer.deadline = std::chrono::steady_clock::now() + timer.interval;
    if (timer.deadline < timer.armed) {
        arm(timer.fd, timer.deadline);
        timer.armed = timer.deadline;
    }
    return true;
}

void EventLoop::fireTimer(int id)
{
    const auto it = m_timers.find(id);
    if (it == m_timers.end())
        return;
    // Kept alive while it runs: the task may stop its own timer.
    const std::shared_ptr<TimerEntry> timer = it->second;
    if (!timer->repeat) {
        unwatch(id);
    } else {
        // Missed expiries are skipped, not run back to back.
        const auto now = std::chrono::steady_clock::now();
        timer->deadline += timer->interval;
        if (timer->deadline <= now)
            timer->deadline = now + timer->interval;
        arm(timer->fd, timer->deadline);
        timer->armed = timer->deadline;
    }
    timer->task();
}

void EventLoop::runDueTimers()
{
    if (m_timers.empty())
        return;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::chrono::steady_clock::time_point, int>> due;
    for (const auto& [id, timer] : m_timers) {
        if (timer->deadline <= now)
            due.emplace_back(timer->deadline, id);
    }
    std::sort(due.begin(), due.end());
    for (const auto& [deadline, id] : due) {
        // An earlier task may have stopped or restarted it.
        const auto it = m_timers.find(id);
        if (it != m_timers.end() && it->second->deadline <= now)
            fireTimer(id);
    }
}

void EventLoop::stopTimer(int id)
{
    unwatch(id);
}

void EventLoop::post(Task task)
{
    {
        std::lock_guard lock(m_postedMutex);
        m_posted.push_back(std::move(task));
    }
    // run() looks at the queue before it waits.
    if (s_running == this)
        return;
    const uint64_t one = 1;
    [[maybe_unused]] const auto written = write(m_wakeFd, &one, sizeof(one));
}

void EventLoop::dropPosted()
{
    // Destroying a task may post another.
    while (true) {
        std::vector<Task> posted;
        {
            std::lock_guard lock(m_postedMutex);
            posted.swap(m_posted);
        }
        if (posted.empty())
            break;
    }
}

void EventLoop::addDestroyHook(Task hook)
{
    m_destroyHooks.push_back(std::move(hook));
}

int EventLoop::addPrepareHook(Task hook)
{
    const int id = m_nextId++;
    m_prepareHooks.emplace_back(id, std::move(hook));
    return id;
}

void EventLoop::removePrepareHook(int id)
{
    std::erase_if(m_prepareHooks, [id](const auto& hook) { return hook.first == id; });
}

int EventLoop::run()
{
    const AutoReset<EventLoop*> running(s_running, this);
    m_running = true;
    while (m_running) {
        runPosted();
        runPrepareHooks();
        if (!m_running)
            break;
        if (!dispatchEvents(hasPosted() ? 0 : -1))
            return 1;
    }
    return m_exitCode;
}

void EventLoop::dispatch()
{
    runPosted();
    dispatchEvents(0);
    runPosted();
    // Flushes what the work above queued, as before a wait.
    runPrepareHooks();
}

void EventLoop::dispatchReceived()
{
    runPosted();
    dispatchEvents(0);
    runPrepareHooks();
    // The wake descriptor may have been drained after the tasks were posted.
    if (hasPosted()) {
        const uint64_t one = 1;
        [[maybe_unused]] const auto written = write(m_wakeFd, &one, sizeof(one));
    }
}

void EventLoop::runPrepareHooks()
{
    // A hook may remove hooks; one removed before its turn does not run.
    const auto hooks = m_prepareHooks;
    for (const auto& [id, hook] : hooks) {
        const bool present = std::any_of(m_prepareHooks.begin(), m_prepareHooks.end(),
                                         [id](const auto& entry) { return entry.first == id; });
        if (present)
            hook();
    }
}

bool EventLoop::dispatchEvents(int timeout)
{
    epoll_event events[32];
    const int count = epoll_wait(m_epoll, events, 32, timeout);
    if (count < 0) {
        if (errno == EINTR)
            return true;
        perror("epoll_wait");
        return false;
    }

    for (int i = 0; i < count; ++i) {
        const auto it = m_watches.find(int(events[i].data.u64));
        if (it == m_watches.end())
            continue;
        const auto watch = it->second;
        if (watch->callback)
            watch->callback(events[i].events);
    }
    runDueTimers();
    return true;
}

void EventLoop::quit(int exitCode)
{
    m_exitCode = exitCode;
    m_running = false;
    if (m_quitHandler)
        m_quitHandler(exitCode);
}

void EventLoop::setQuitHandler(std::function<void(int exitCode)> handler)
{
    m_quitHandler = std::move(handler);
}

void EventLoop::runPosted()
{
    std::vector<Task> tasks;
    {
        std::lock_guard lock(m_postedMutex);
        tasks.swap(m_posted);
    }
    for (const auto& task : tasks)
        task();
}

bool EventLoop::hasPosted()
{
    std::lock_guard lock(m_postedMutex);
    return !m_posted.empty();
}

} // namespace cutefish::base
