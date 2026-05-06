module;

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

module builder:scheduler;

import phanes_deque;

struct Worker
{
    Worker() = default;
    Worker(Worker&&) = delete;
    auto operator=(Worker&&) -> Worker& = delete;
    Worker(const Worker&) = delete;
    auto operator=(const Worker&) -> Worker& = delete;

    LockFreeDeque<std::size_t> tasks;
    std::jthread thread;
};

template <typename Handler>
class ThreadPool
{
  private:
    static inline constexpr std::size_t no_worker_id = static_cast<std::size_t>(-1);
    static inline thread_local std::size_t current_worker_id = no_worker_id;

    std::condition_variable condition;
    std::mutex sleep_guard;

    std::stop_source pool_stop;
    std::vector<std::unique_ptr<Worker>> workers;

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

    void worker_loop(std::stop_token st)
    {
        const size_t id = current_worker_id;
        constexpr size_t STEAL_ATTEMPTS = 64;
        Worker& self_worker = *workers[id];
        size_t steal_start_counter = 0;

        while (true)
        {
            std::optional<std::size_t> task_id;

            // Try local
            task_id = self_worker.tasks.pop_back();

            // try steal
            if (!task_id.has_value())
            {
                for (size_t i = 0; i < STEAL_ATTEMPTS && !task_id.has_value(); ++i)
                {
                    task_id = try_steal(self_worker, id, steal_start_counter);
                }
            }

            // no task
            if (!task_id.has_value())
            {
                std::unique_lock lock(sleep_guard);
                condition.wait(lock,
                               [&]
                               {
                                   if (st.stop_requested())
                                   {
                                       return true;
                                   }
                                   for (const auto& w : workers)
                                   {
                                       if (!w->tasks.empty())
                                       {
                                           return true;
                                       }
                                   }
                                   return false;
                               });
                if (st.stop_requested())
                {
                    return;
                }
                continue;
            }

            // EXECUTE
            handler(*task_id);
        }
    }

  public:
    explicit ThreadPool(Handler hand, size_t threads) : handler(std::move(hand))
    {
        workers.reserve(threads);
        for (size_t i = 0; i < threads; ++i)
        {
            workers.emplace_back(std::make_unique<Worker>());
        }
    }

    // Seed initial task before worker threads start executing,
    // so deque ownership rules (single-owner push/pop) are not violated.
    void start(std::size_t initial_task)
    {
        workers[0]->tasks.push_back(initial_task);
        for (size_t id = 0; id < workers.size(); ++id)
        {
            workers[id]->thread = std::jthread(
                [this, id]
                {
                    current_worker_id = id;
                    worker_loop(pool_stop.get_token());
                });
        }
    }

    void submit(std::size_t task_id)
    {
        auto& queue = workers[current_worker_id]->tasks;
        {
            std::lock_guard lock(sleep_guard);
            queue.push_back(task_id);
        }
        condition.notify_one();
    }

    ~ThreadPool()
    {
        {
            std::lock_guard lock(sleep_guard);
            pool_stop.request_stop();
        }
        condition.notify_all();
        for (const auto& w : workers)
        {
            if (w->thread.joinable())
            {
                w->thread.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    auto operator=(const ThreadPool&) -> ThreadPool& = delete;
    ThreadPool(ThreadPool&&) = delete;
    auto operator=(ThreadPool&&) -> ThreadPool& = delete;
};
