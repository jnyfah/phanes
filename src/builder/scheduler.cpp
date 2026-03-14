
module;

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <iterator>
#include <mutex>
#include <queue>
#include <random>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <utility>
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
static thread_local std::mt19937 rng{std::random_device{}()};

class ThreadPool
{
  private:
    std::vector<std::unique_ptr<Worker>> workers;
    std::condition_variable condition;
    std::mutex guard;
    bool is_active{true};
    std::queue<std::function<void()>> tasks;
    std::atomic<size_t> idle_count{0};

    auto try_steal(size_t id) -> std::function<void()>
    {

        std::vector<size_t> order;
        order.reserve(workers.size() - 1);

        auto indices = std::views::iota(size_t(0), workers.size());

        std::ranges::copy_if(indices, std::back_inserter(order), [id](size_t i) { return i != id; });

        std::ranges::shuffle(order, rng);

        for (size_t victim_id : order)
        {

            auto& victim = *workers[victim_id];

            std::unique_lock lock(victim.guard, std::try_to_lock);

            if (!lock || victim.tasks.empty())
            {
                continue;
            }

            const auto steal_count = std::max(size_t{1}, victim.tasks.size() / 2);
            std::vector<std::function<void()>> stolen;
            stolen.reserve(steal_count);

            for (size_t i = 0; i < steal_count; i++)
            {
                stolen.emplace_back(std::move(victim.tasks.back()));
                victim.tasks.pop_back();
            }

            lock.unlock();

            std::function<void()> task = stolen[0];

            if (stolen.size() > 1)
            {
                auto& w = *workers[id];
                std::lock_guard lock(w.guard);
                for (size_t i = 1; i < stolen.size(); i++)
                {
                    w.tasks.push_front(std::move(stolen[i]));
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
                std::lock_guard lock(guard);

                if (!tasks.empty())
                {
                    task = std::move(tasks.front());
                    tasks.pop();
                }
            }

            // try steal
            if (!task)
            {
                for (size_t attempt = 0; !task && attempt < STEAL_ATTEMPTS; ++attempt)
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
                ++idle_count;
                {
                    std::unique_lock lock(guard);

                    if (!is_active)
                    {
                        --idle_count;
                        return;
                    }
                    condition.wait(lock, [this] { return !is_active || !tasks.empty(); });
                }
                --idle_count;
                continue;
            }
            task();
        }
    }

  public:
    explicit ThreadPool(size_t threads = 8)
    {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            auto& w = workers.emplace_back(std::make_unique<Worker>());
            w->thread = std::thread(&ThreadPool::worker_loop, this, i);
        }
    }

    void submit(std::function<void()> task)
    {
        if (current_worker_id == NO_WORKER_ID)
        {
            {
                std::lock_guard lock(guard);
                if (!is_active)
                {
                    throw std::runtime_error("ThreadPool stopped");
                }
                tasks.push(std::move(task));
            }
            condition.notify_one();
        }
        else
        {
            auto& worker = *workers[current_worker_id];
            size_t local_size = 0;
            {
                std::lock_guard lock(worker.guard);
                if (!is_active)
                {
                    throw std::runtime_error("ThreadPool stopped");
                }
                worker.tasks.emplace_back(std::move(task));
                local_size = worker.tasks.size();
            }

            // if this local queue we just added a task to has more than 2 tasks and we also have idle threads wake up
            // the idle thread
            if (local_size > 1 && idle_count.load(std::memory_order_relaxed) > 0)
            {
                condition.notify_one();
            }
        }
    }

    ~ThreadPool()
    {

        {
            std::unique_lock lock(guard);
            is_active = false;
        }
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