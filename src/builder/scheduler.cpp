module;

#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <random>
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

    std::thread thread;
    LockFreeDeque<std::size_t> tasks;
};

using Handler = std::function<void(std::size_t)>;

static constexpr size_t NO_WORKER_ID = std::numeric_limits<size_t>::max();
static thread_local size_t current_worker_id = NO_WORKER_ID;
static thread_local std::mt19937 rng{std::random_device{}()};

class ThreadPool
{
  private:
    std::vector<std::unique_ptr<Worker>> workers;
    std::condition_variable condition;
    std::mutex global_guard;

    std::queue<std::size_t> global_queue;

    std::atomic<bool> is_active{true};
    std::atomic<size_t> idle_workers{0};
    std::atomic<size_t> pending_tasks{0};

    Handler handler;

    auto try_steal(size_t self) -> std::optional<std::size_t>
    {
        const size_t N = workers.size();
        if (N <= 1)
        {
            return std::nullopt;
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

            auto task_id = victim.tasks.steal_front();
            if (!task_id)
            {
                continue;
            }

            size_t steal_count = victim.tasks.size() / 2;
            auto& stealer = *workers[self];
            for (size_t i = 0; i < steal_count; ++i)
            {
                auto t = victim.tasks.steal_front();
                if (!t)
                {
                    break;
                }
                stealer.tasks.push_back(*t);
            }

            return task_id;
        }
        return std::nullopt;
    }

    void worker_loop(size_t id)
    {
        current_worker_id = id;
        constexpr size_t STEAL_ATTEMPTS = 64;

        while (true)
        {
            std::optional<std::size_t> task_id;

            // Try local
            {
                auto& w = *workers[id];
                if (!w.tasks.empty())
                {
                    task_id = w.tasks.pop_back();
                }
            }

            // try global
            if (!task_id)
            {
                std::lock_guard lock(global_guard);

                if (!global_queue.empty())
                {
                    task_id = global_queue.front();
                    global_queue.pop();
                }
            }

            // try steal
            if (!task_id)
            {
                for (size_t i = 0; i < STEAL_ATTEMPTS && !task_id; ++i)
                {
                    task_id = try_steal(id);
                    if (!task_id)
                    {
                        std::this_thread::yield();
                    }
                }
            }

            // shutdown
            if (!task_id)
            {
                idle_workers.fetch_add(1, std::memory_order_relaxed);
                {
                    std::unique_lock lock(global_guard);
                    if (!is_active.load(std::memory_order_relaxed))
                    {
                        idle_workers.fetch_sub(1, std::memory_order_relaxed);
                        return;
                    }

                    condition.wait(lock,
                                   [&] {
                                       return !is_active.load(std::memory_order_relaxed) ||
                                           pending_tasks.load(std::memory_order_relaxed) > 0;
                                   });
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
    explicit ThreadPool(size_t threads, Handler hand) : handler(std::move(hand))
    {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            auto& w = workers.emplace_back(std::make_unique<Worker>());
            w->thread = std::thread(&ThreadPool::worker_loop, this, i);
        }
    }

    void submit(std::size_t task_id)
    {
        pending_tasks.fetch_add(1, std::memory_order_relaxed);
        if (current_worker_id == NO_WORKER_ID)
        {
            // add to global queue
            {
                std::lock_guard lock(global_guard);
                if (!is_active)
                {
                    throw std::runtime_error("ThreadPool stopped");
                }
                global_queue.push(task_id);
            }
            condition.notify_one();
        }
        else
        {
            auto& worker = *workers[current_worker_id];
            if (!is_active)
            {
                throw std::runtime_error("ThreadPool stopped");
            }
            worker.tasks.push_back(task_id);
            size_t local_size = worker.tasks.size();

            // if this local queue we just added a task_id to has more than 2 tasks and we also have idle threads wake
            // up the idle thread
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