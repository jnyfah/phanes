module;

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

module builder:scheduler;

struct Worker
{
    Worker() = default;
    Worker(Worker&&) = delete;
    auto operator=(Worker&&) -> Worker& = delete;
    Worker(const Worker&) = delete;
    auto operator=(const Worker&) -> Worker& = delete;

    std::thread thread;
    std::mutex guard;
    std::deque<std::function<void()>> tasks;
};

static constexpr size_t NO_WORKER_ID = std::numeric_limits<size_t>::max();
static thread_local size_t current_worker_id = NO_WORKER_ID;
static thread_local std::mt19937 rng{std::hash<std::thread::id>{}(std::this_thread::get_id())};

class ThreadPool
{
  private:
    std::vector<std::unique_ptr<Worker>> workers;
    std::condition_variable condition;
    std::mutex global_guard;

    std::queue<std::function<void()>> global_queue;

    std::atomic<bool> is_active{true};
    std::atomic<size_t> idle_workers{0};

    auto try_steal(size_t self) -> std::function<void()>
    {
        const size_t N = workers.size();
        if (N <= 1)
        {
            return {};
        }

        // randomize the ids for the workers we are trying to steal from
        std::uniform_int_distribution<size_t> dist(0, N - 1);

        for (size_t attempt = 0; attempt < N; ++attempt)
        {
            size_t victim_id = dist(rng);
            if (victim_id == self)
            {
                continue;
            }

            auto& victim = *workers[victim_id];

            std::unique_lock lock(victim.guard, std::try_to_lock);

            if (!lock || victim.tasks.empty())
            {
                continue;
            }

            size_t steal_count = std::max<size_t>(1, victim.tasks.size() / 2);

            auto& stealer = *workers[self];

            std::function<void()> task;

            // move from victim to stealers queue
            for (size_t i = 0; i < steal_count; ++i)
            {
                auto t = std::move(victim.tasks.back());
                victim.tasks.pop_back();

                if (i == 0)
                {
                    task = std::move(t);
                }
                else
                {
                    std::lock_guard my_lock(stealer.guard);
                    stealer.tasks.push_front(std::move(t));
                }
            }
            return task;
        }
        return {};
    }

    void worker_loop(size_t id)
    {
        current_worker_id = id;
        constexpr size_t STEAL_ATTEMPTS = 64;

        while (true)
        {
            std::function<void()> task;

            // Try local
            {
                auto& w = *workers[id];
                std::lock_guard lock(w.guard);

                if (!w.tasks.empty())
                {
                    task = std::move(w.tasks.front());
                    w.tasks.pop_front();
                }
            }

            // try global
            if (!task)
            {
                std::lock_guard lock(global_guard);

                if (!global_queue.empty())
                {
                    task = std::move(global_queue.front());
                    global_queue.pop();
                }
            }

            // try steal
            if (!task)
            {
                for (size_t i = 0; i < STEAL_ATTEMPTS && !task; ++i)
                {
                    task = try_steal(id);
                    if (!task)
                    {
                        std::this_thread::yield();
                    }
                }
            }

            // shutdown
            if (!task)
            {
                idle_workers.fetch_add(1, std::memory_order_relaxed);

                std::unique_lock lock(global_guard);

                if (!is_active.load(std::memory_order_relaxed))
                {
                    idle_workers.fetch_sub(1, std::memory_order_relaxed);
                    return;
                }

                condition.wait(lock);

                idle_workers.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }
            // EXECUTE
            task();
        }
    }

  public:
    explicit ThreadPool(size_t threads = 8)
    {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            const auto& w = workers.emplace_back(std::make_unique<Worker>());
            w->thread = std::thread(&ThreadPool::worker_loop, this, i);
        }
    }

    void submit(std::function<void()> task)
    {
        if (current_worker_id == NO_WORKER_ID)
        {
            // add to global queue
            {
                std::lock_guard lock(global_guard);
                if (!is_active)
                {
                    throw std::runtime_error("ThreadPool stopped");
                }
                global_queue.push(std::move(task));
            }
            condition.notify_one();
        }
        else
        {
            auto& worker = *workers[current_worker_id];
            size_t local_size;
            {
                std::lock_guard lock(worker.guard);
                if (!is_active)
                {
                    throw std::runtime_error("ThreadPool stopped");
                }
                worker.tasks.push_back(std::move(task));
                local_size = worker.tasks.size();
            }

            // if this local queue we just added a task to has more than 2 tasks and we also have idle threads wake up
            // the idle thread
            if (local_size > 1 && idle_workers.load(std::memory_order_relaxed) > 0)
            {
                condition.notify_one();
            }
        }
    }

    ~ThreadPool()
    {
        is_active.store(false);

        condition.notify_all();

        for (const auto& worker : workers)
        {
            if (worker->thread.joinable())
            {
                worker->thread.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    auto operator=(const ThreadPool&) -> ThreadPool& = delete;
    ThreadPool(ThreadPool&&) = delete;
    auto operator=(ThreadPool&&) -> ThreadPool& = delete;
};

// try stealing multiple times before sleeping
// Better wake policy:
// Steal batches.