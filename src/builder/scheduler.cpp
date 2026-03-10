
module;
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

module builder:scheduler;

class ThreadPool
{
  private:
    std::vector<std::thread> workers;
    std::condition_variable condition;
    std::mutex guard;
    bool is_active{true};
    std::queue<std::function<void()>> tasks;

    void worker_loop()
    {
        while (true)
        {
            std::function<void()> task;

            {
                std::unique_lock lock(guard);

                condition.wait(lock, [this] { return !is_active || !tasks.empty(); });
                if (!is_active && tasks.empty())
                {
                    return;
                }
                task = std::move(tasks.front());
                tasks.pop();
            }
            task();
        }
    }

  public:
    ThreadPool(size_t threads = 8)
    {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; i++)
        {
            workers.emplace_back(&ThreadPool::worker_loop, this);
        }
    }

    void submit(std::function<void()> task)
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

    ~ThreadPool()
    {

        {
            std::lock_guard lock(guard);
            is_active = false;
        }
        condition.notify_all();

        for (auto& worker : workers)
        {
            worker.join();
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    auto operator=(const ThreadPool&) -> ThreadPool& = delete;
    ThreadPool(ThreadPool&&) = delete;
    auto operator=(ThreadPool&&) -> ThreadPool& = delete;
};