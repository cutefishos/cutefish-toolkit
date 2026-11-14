// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "base/thread_pool.h"

#include <algorithm>

namespace cutefish::base {

ThreadPool::ThreadPool(int maxThreads)
    : m_maxThreads(std::max(maxThreads, 1))
{
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown()
{
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_queue.clear();
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads) {
        if (thread.joinable())
            thread.join();
    }
}

ThreadPool& ThreadPool::global()
{
    static ThreadPool pool;
    return pool;
}

void ThreadPool::post(std::function<void()> work)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_queue.push_back(std::move(work));
        if (m_idle == 0 && int(m_threads.size()) < m_maxThreads)
            m_threads.emplace_back([this] { threadMain(); });
    }
    m_wake.notify_one();
}

void ThreadPool::threadMain()
{
    std::unique_lock lock(m_mutex);
    while (true) {
        ++m_idle;
        m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
        --m_idle;
        if (m_stopping)
            return;
        std::function<void()> work = std::move(m_queue.front());
        m_queue.pop_front();
        lock.unlock();
        work();
        lock.lock();
    }
}

} // namespace cutefish::base
