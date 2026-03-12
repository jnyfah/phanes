
module;
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
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
    std::queue<std::function<void()>> tasks;
};

static thread_local size_t current_worker_id = -1;

class ThreadPool
{
  private:
    std::vector<std::unique_ptr<Worker>> workers;
    std::condition_variable condition;
    std::mutex guard;
    bool is_active{true};
    std::queue<std::function<void()>> tasks;

    void worker_loop(size_t id)
    {
        current_worker_id = id;
        while (true)
        {
            std::function<void()> task;
            {

                auto& worker = *workers[id];
                std::unique_lock lock(worker.guard);

                if (!worker.tasks.empty())
                {
                    task = std::move(worker.tasks.front());
                    worker.tasks.pop();
                }
            }

            if (!task)
            {
                std::unique_lock lock(guard);
                if (!tasks.empty())
                {
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                else if (!is_active)
                {
                    return;
                }
                else
                {
                    condition.wait(lock, [this] { return !is_active || !tasks.empty(); });
                    continue;
                }
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
        if (current_worker_id == -1)
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
            const auto& worker = workers[current_worker_id];
            {
                std::lock_guard lock(worker->guard);
                if (!is_active)
                {
                    throw std::runtime_error("ThreadPool stopped");
                }
                worker->tasks.push(std::move(task));
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