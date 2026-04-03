module;

#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

module builder:scheduler;

import :deque;

struct Worker
{
    Worker() = default;
    Worker(Worker&&) = delete;
    auto operator=(Worker&&) -> Worker& = delete;
    Worker(const Worker&) = delete;
    auto operator=(const Worker&) -> Worker& = delete;

    std::jthread thread;
    LockFreeDeque<std::size_t> tasks;
};

template <typename Handler>
class ThreadPool
{
  private:
    static inline constexpr std::size_t no_worker_id = static_cast<std::size_t>(-1);
    static inline thread_local std::size_t current_worker_id = no_worker_id;

    std::vector<std::unique_ptr<Worker>> workers;
    std::condition_variable condition;
    std::mutex sleep_guard;

    std::stop_source pool_stop;
    std::atomic<size_t> idle_workers{0};
    std::atomic<size_t> pending_tasks{0};

    Handler handler;

    auto try_steal(Worker& self_worker, size_t self, size_t& start_counter) -> std::optional<std::size_t>
    {
        const size_t N = workers.size();
        if (N <= 1)
        {
            return std::nullopt;
        }

        const size_t start = start_counter++ % (N - 1);

        for (size_t k = 0; k < N - 1; ++k)
        {
            const size_t offset = 1 + ((start + k) % (N - 1));
            const size_t victim_id = (self + offset) % N;
            auto& victim = *workers[victim_id];

            auto task_id = victim.tasks.steal_front();
            if (!task_id.has_value())
            {
                continue;
            }

            size_t steal_count = victim.tasks.size() / 2;
            for (size_t j = 0; j < steal_count; ++j)
            {
                auto t = victim.tasks.steal_front();
                if (!t)
                {
                    break;
                }
                self_worker.tasks.push_back(*t);
            }

            return task_id;
        }

        return std::nullopt;
    }

    void worker_loop(std::stop_token st, size_t id)
    {
        current_worker_id = id;
        constexpr size_t STEAL_ATTEMPTS = 64;
        Worker& self_worker = *workers[id];
        size_t steal_start_counter = 0;

        while (true)
        {
            std::optional<std::size_t> task_id;

            // Try local
            {
                task_id = self_worker.tasks.pop_back();
            }

            // try steal
            if (!task_id.has_value())
            {
                for (size_t i = 0; i < STEAL_ATTEMPTS && !task_id.has_value(); ++i)
                {
                    task_id = try_steal(self_worker, id, steal_start_counter);
                }

                if (!task_id.has_value())
                {
                    std::this_thread::yield();
                }
            }

            // shutdown
            if (!task_id.has_value())
            {
                // Keep track of idle threads
                idle_workers.fetch_add(1, std::memory_order_relaxed);
                {
                    std::unique_lock lock(sleep_guard);
                    if (st.stop_requested())
                    {
                        // no longer a worker
                        idle_workers.fetch_sub(1, std::memory_order_relaxed);
                        return;
                    }

                    condition.wait(
                        lock,
                        [&] { return st.stop_requested() || pending_tasks.load(std::memory_order_relaxed) > 0; });
                }
                idle_workers.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }

            // EXECUTE
            handler(*task_id);
            pending_tasks.fetch_sub(1, std::memory_order_relaxed);
        }
    }

  public:
    explicit ThreadPool(Handler hand, size_t threads = std::jthread::hardware_concurrency()) : handler(std::move(hand))
    {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            auto& w = workers.emplace_back(std::make_unique<Worker>());
            w->thread = std::jthread([this, i, st = pool_stop.get_token()] { worker_loop(st, i); });
        }
    }

    void submit(std::size_t task_id)
    {
        pending_tasks.fetch_add(1, std::memory_order_relaxed);
        if (current_worker_id == no_worker_id)
        {
            // external submit: push directly to worker 0 and wake a sleeper
            if (pool_stop.stop_requested())
            {
                throw std::runtime_error("ThreadPool stopped");
            }
            workers[0]->tasks.push_back(task_id);
            condition.notify_one();
        }
        else
        {
            auto& worker = *workers[current_worker_id];
            if (pool_stop.stop_requested())
            {
                throw std::runtime_error("ThreadPool stopped");
            }
            worker.tasks.push_back(task_id);
            size_t local_size = worker.tasks.size();

            // if this local queue we just added a task_id to has more than 2 tasks and we also have idle threads wake
            // up the idle thread so they can start stealing!!!!!!
            if (local_size > 1 && idle_workers.load(std::memory_order_relaxed) > 0)
            {
                condition.notify_one();
            }
        }
    }

    ~ThreadPool()
    {
        pool_stop.request_stop();
        condition.notify_all();
    }

    ThreadPool(const ThreadPool&) = delete;
    auto operator=(const ThreadPool&) -> ThreadPool& = delete;
    ThreadPool(ThreadPool&&) = delete;
    auto operator=(ThreadPool&&) -> ThreadPool& = delete;
};
