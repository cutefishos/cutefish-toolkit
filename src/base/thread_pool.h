// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/event_loop.h"

namespace cutefish::base {

// The result of work handed to a thread, on its way back to the loop. When
// the job is destroyed, reassigned or cancelled, the work still runs to its
// end but its result is dropped. Keep it in the object the result is for.
class [[nodiscard]] Job
{
public:
    Job() = default;
    ~Job() { cancel(); }

    Job(Job&& other) noexcept : m_state(std::move(other.m_state)) {}
    Job& operator=(Job&& other) noexcept
    {
        if (this != &other) {
            cancel();
            m_state = std::move(other.m_state);
        }
        return *this;
    }

    // Until the result is delivered or dropped.
    bool isPending() const { return m_state && !m_state->finished && !m_state->cancelled; }
    void cancel()
    {
        if (m_state)
            m_state->cancelled = true;
        m_state.reset();
    }
    // Lets the result arrive although the job goes.
    void detach() { m_state.reset(); }

    struct State
    {
        std::atomic<bool> cancelled = false;
        bool finished = false;
    };

private:
    friend class ThreadPool;
    explicit Job(std::shared_ptr<State> state) : m_state(std::move(state)) {}

    std::shared_ptr<State> m_state;
};

// Runs work on its threads and hands each result back to the loop of the
// thread that asked for it.
class ThreadPool
{
public:
    // Threads start as work arrives, up to this many.
    explicit ThreadPool(int maxThreads = int(std::thread::hardware_concurrency()));
    // Waits for the work that has started; the rest is dropped.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // As many threads as cores.
    static ThreadPool& global();

    // Waits for the work that has started; the rest, and work posted later,
    // is dropped.
    void shutdown();

    // Runs the work on a pool thread, and nothing after it.
    void post(std::function<void()> work);

    // Runs work() on a pool thread, then done(result) on the calling thread's
    // loop, or done() when work returns nothing.
    template<typename Work, typename Done>
    Job run(Work work, Done done);

private:
    void threadMain();

    const int m_maxThreads;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_threads;
    int m_idle = 0;
    bool m_stopping = false;
};

template<typename Work, typename Done>
Job ThreadPool::run(Work work, Done done)
{
    auto state = std::make_shared<Job::State>();
    // std::function copies; the callables and result may only move.
    auto shared = std::make_shared<std::pair<Work, Done>>(std::move(work), std::move(done));
    LoopHandle loop = EventLoop::current()->handle();
    post([state, shared, loop] {
        if constexpr (std::is_void_v<std::invoke_result_t<Work&>>) {
            shared->first();
            loop.post([state, shared] {
                if (state->cancelled)
                    return;
                state->finished = true;
                shared->second();
            });
        } else {
            using Result = std::invoke_result_t<Work&>;
            auto result = std::make_shared<Result>(shared->first());
            loop.post([state, shared, result] {
                if (state->cancelled)
                    return;
                state->finished = true;
                shared->second(std::move(*result));
            });
        }
    });
    return Job(std::move(state));
}

// Runs work() on the global pool and done(result) back on this thread's loop.
template<typename Work, typename Done>
Job runInThread(Work work, Done done)
{
    return ThreadPool::global().run(std::move(work), std::move(done));
}

} // namespace cutefish::base
