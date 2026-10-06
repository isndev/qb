/**
 * @file qb/io/async/offload.cpp
 * @brief The offload pool and the per-thread completion port behind `co_await offload(...)`
 *
 * qb-io's only worker threads (Huly QB-69). Everything here is reached from the first `offload` of
 * the process, and nothing before: the pool is a block-scope static, the port a block-scope
 * `thread_local`, and no qb-io path that existed before them calls into this file.
 *
 * @author qb - C++ Actor Framework
 * @copyright Copyright (c) 2011-2026 qb - isndev (cpp.actor)
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * @ingroup IO
 */

#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

#include <qb/io/async/coroutine/offload.h>
#include <qb/io/async/listener.h>

namespace qb::io::async::detail {

namespace {
/// `offload_stats::discarded` += 1: a completed job whose frame will never resume. Defined below,
/// with the pool that keeps the counter.
void count_discarded() noexcept;
} // namespace

/**
 * @brief One thread's end of the offload: where the pool hands back what that thread awaits.
 * @details Created by the thread's first `offload`, closed when the thread exits.
 *
 * - `_watcher` is an `ev_async` on the thread's loop, started while a job of this thread is in
 *   flight (`_in_flight` > 0) and stopped once none is. Started, it is a referenced active watcher:
 *   `listener::has_work()` reads it, a qb-core core keeps pumping its loop and parks inside it.
 * - A pool thread hands a finished job back with `complete()`: it links the job and SENDS the
 *   watcher under `_mtx`, and only while the port is open. The mutex is what keeps the send off a
 *   loop being destroyed -- `close()` takes it before stopping the watcher -- and what orders
 *   `ev_async_start` (which creates the loop's wake pipe) before any send to it.
 * - `on_complete()`, the watcher's callback, runs on the loop: it takes the list under the mutex and
 *   resumes each job's frame, or discards a job whose frame is gone.
 *
 * Shared (`std::shared_ptr`) between the thread and every job in flight, so a pool thread finishing
 * after the thread exited still finds the mutex alive. The watcher itself is destroyed by
 * `close()`, on the owning thread: a pool thread that drops the last reference never touches it.
 */
class offload_port {
public:
    explicit offload_port(listener &owner) {
        _watcher.emplace(owner.loop());
        _watcher->set<offload_port, &offload_port::on_complete>(this);
    }

    offload_port(const offload_port &)            = delete;
    offload_port &operator=(const offload_port &) = delete;

    /// Owning thread: one more job in flight; the first one starts the watcher.
    void
    begin() noexcept {
        if (_in_flight++ == 0)
            _watcher->start();
    }

    /// Owning thread: the job `begin()` counted was never submitted.
    void
    cancel() noexcept {
        if (--_in_flight == 0)
            _watcher->stop();
    }

    /// Pool thread: hand `job` back to the owning loop, or drop it if that thread has exited.
    void
    complete(offload_job *job) noexcept {
        {
            std::lock_guard lk(_mtx);
            if (!_closed) {
                job->next = nullptr;
                if (_tail)
                    _tail->next = job;
                else
                    _head = job;
                _tail = job;
                _watcher->send();
                return;
            }
        }
        // The awaiting thread is gone, and its loop with it: nothing to resume. The only place a
        // result can be destroyed off its loop.
        count_discarded();
        job->release();
    }

    /// Owning thread, at its exit (before its listener is destroyed): no send after this returns.
    void
    close() noexcept {
        offload_job *pending = nullptr;
        {
            std::lock_guard lk(_mtx);
            _closed = true;
            pending = _head;
            _head = _tail = nullptr;
        }
        // On this thread, with the loop alive: a stopped watcher whose loop is gone could not be.
        _watcher.reset();
        while (pending) {
            offload_job *const next = pending->next;
            count_discarded(); // its frame can no longer resume: the thread is ending
            pending->release();
            pending = next;
        }
        _in_flight = 0;
    }

private:
    void on_complete(ev::async &, int) noexcept;

    std::mutex   _mtx;
    offload_job *_head   = nullptr; ///< completions not yet drained, FIFO (under `_mtx`)
    offload_job *_tail   = nullptr;
    bool         _closed = false; ///< under `_mtx`

    std::size_t              _in_flight = 0; ///< owning thread only
    std::optional<ev::async> _watcher;       ///< owning thread only (start / stop / destroy); `send` under `_mtx`
};

namespace {

/**
 * @brief The process-wide pool: a FIFO queue served by `_want` threads, started by the first job.
 * @details A block-scope static of `pool()`: constructed by the first `offload`, `set_offload_threads`
 *          or `current_offload_stats` -- none of which starts a thread but the first -- and destroyed
 *          at process exit, after every thread-local port of the exiting thread was closed. The
 *          destructor stops the threads: queued jobs are dropped, running ones finish first.
 */
class offload_pool {
public:
    offload_pool() = default;

    ~offload_pool() {
        std::deque<offload_job *> dropped;
        {
            std::lock_guard lk(_mtx);
            _stopping = true;
            dropped.swap(_queue);
        }
        _cv.notify_all();
        for (auto &t : _threads)
            if (t.joinable())
                t.join();
        for (auto *job : dropped)
            job->release();
    }

    bool
    set_threads(std::size_t n) noexcept {
        std::lock_guard lk(_mtx);
        if (n == 0 || !_threads.empty())
            return false;
        _want = n;
        return true;
    }

    void
    submit(offload_job *job) {
        std::unique_lock lk(_mtx);
        if (_threads.empty())
            start_locked();
        _queue.push_back(job);
        _queued.fetch_add(1, std::memory_order_relaxed);
        _submitted.fetch_add(1, std::memory_order_relaxed);
        lk.unlock();
        _cv.notify_one();
    }

    void
    count_discarded() noexcept {
        _discarded.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] offload_stats
    stats() const noexcept {
        offload_stats s;
        s.threads   = _running.load(std::memory_order_relaxed);
        s.submitted = _submitted.load(std::memory_order_relaxed);
        s.completed = _completed.load(std::memory_order_relaxed);
        s.discarded = _discarded.load(std::memory_order_relaxed);
        s.queued    = _queued.load(std::memory_order_relaxed);
        return s;
    }

private:
    /// Under `_mtx`. Starts what it can: a thread that fails to start leaves the ones already
    /// running, and only a pool with no thread at all throws -- the next submission retries.
    void
    start_locked() {
        _threads.reserve(_want);
        for (std::size_t i = 0; i < _want; ++i) {
            try {
                _threads.emplace_back([this] { work(); });
            } catch (...) {
                if (_threads.empty())
                    throw;
                break;
            }
        }
        _running.store(_threads.size(), std::memory_order_relaxed);
    }

    void
    work() noexcept {
        for (;;) {
            offload_job *job;
            {
                std::unique_lock lk(_mtx);
                _cv.wait(lk, [this] { return _stopping || !_queue.empty(); });
                if (_stopping)
                    return;
                job = _queue.front();
                _queue.pop_front();
                _queued.fetch_sub(1, std::memory_order_relaxed);
            }
            job->run();
            _completed.fetch_add(1, std::memory_order_relaxed);
            job->port->complete(job);
        }
    }

    std::mutex                _mtx;
    std::condition_variable   _cv;
    std::deque<offload_job *> _queue;   ///< under `_mtx`
    std::vector<std::thread>  _threads; ///< under `_mtx`
    std::size_t               _want     = 2;
    bool                      _stopping = false;

    std::atomic<std::uint64_t> _running{0};
    std::atomic<std::uint64_t> _submitted{0};
    std::atomic<std::uint64_t> _completed{0};
    std::atomic<std::uint64_t> _discarded{0};
    std::atomic<std::uint64_t> _queued{0};
};

offload_pool &
pool() {
    static offload_pool instance;
    return instance;
}

/// The calling thread's port. `listener::current` is reached FIRST, so its construction completes
/// before the holder's and the holder -- which closes the port -- is destroyed before the listener
/// whose loop the port's watcher lives on (thread-locals are destroyed in the reverse order of
/// their construction).
std::shared_ptr<offload_port> const &
this_thread_port() {
    listener &owner = listener::current;
    struct holder {
        std::shared_ptr<offload_port> port;
        explicit holder(listener &l)
            : port(std::make_shared<offload_port>(l)) {}
        ~holder() {
            port->close();
        }
    };
    thread_local holder h{owner};
    return h.port;
}

void
count_discarded() noexcept {
    pool().count_discarded();
}

} // namespace

void
offload_port::on_complete(ev::async &, int) noexcept {
    offload_job *job = nullptr;
    {
        std::lock_guard lk(_mtx);
        job   = _head;
        _head = _tail = nullptr;
    }
    while (job) {
        offload_job *const next = job->next;
        --_in_flight;
        if (job->awaiter)
            job->awaiter->on_event_ready(); // scheduled on this thread's current scheduler
        else
            count_discarded(); // the frame is gone: its result dies here, on its loop
        job->release();
        job = next;
    }
    if (_in_flight == 0)
        _watcher->stop();
}

void
offload_submit(offload_job *job) {
    auto const &port = this_thread_port();
    job->port        = port;
    job->refs.fetch_add(1, std::memory_order_relaxed); // the job's own reference, released by the drain
    port->begin();
    try {
        pool().submit(job);
    } catch (...) {
        port->cancel();
        job->refs.fetch_sub(1, std::memory_order_relaxed);
        job->port.reset();
        throw;
    }
}

} // namespace qb::io::async::detail

namespace qb::io::async {

bool
set_offload_threads(std::size_t n) noexcept {
    return detail::pool().set_threads(n);
}

offload_stats
current_offload_stats() noexcept {
    return detail::pool().stats();
}

} // namespace qb::io::async
