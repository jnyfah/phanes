module;

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

module builder:deque;

template <typename T>
class LockFreeDeque
{
  public:
    LockFreeDeque() : data(std::make_unique<T[]>(capacity)) {}

    auto push_back(T&& item) -> void
    {
        auto f = front.load(std::memory_order_relaxed);
        auto b = back.load(std::memory_order_acquire);

        if (b - f >= capacity)
        {
            return; // todo
        }

        data[b & mask] = std::move(item);
        std::atomic_thread_fence(std::memory_order_release);
        back.store(b + 1, std::memory_order_relaxed);
    }

    auto pop_back() -> std::optional<T>
    {
        auto b = back.load(std::memory_order_acquire);

        if (b == 0)
        {
            return std::nullopt; // guard against underflow as b cant be -1
        }

        b = b - 1;
        back.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto f = front.load(std::memory_order_relaxed);

        if (f > b)
        {
            // if the front has advanced past the back it means that the queue is empty
            back.store(f, std::memory_order_relaxed);
            return std::nullopt;
        }

        T item = std::move(data[b & mask]);

        // if there is one item left, CAS to see who wins the race to pick the last item
        if (f == b)
        {
            // am I the one to chnage front ??
            if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                // if no then update the back to b
                back.store(f, std::memory_order_relaxed);
                return std::nullopt;
            }

            // if yes then update back
            back.store(f + 1, std::memory_order_relaxed);
        }
        return item;
    }

    auto pop_front() -> std::optional<T>
    {
        auto f = front.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        auto b = back.load(std::memory_order_relaxed);

        if (f >= b)
        {
            return std::nullopt;
        }

        T item = std::move(data[f & mask]);

        if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
            return std::nullopt;
        }

        return item;
    }

    [[nodiscard]] auto empty() const -> bool
    {
        const size_t f = front.load(std::memory_order_acquire);
        const size_t b = back.load(std::memory_order_acquire);

        return f >= b;
    }

    [[nodiscard]] auto size() const -> std::size_t
    {

        const size_t f = front.load(std::memory_order_acquire);
        const size_t b = back.load(std::memory_order_acquire);
        return (b >= f) ? (b - f) : 0;
    }

  private:
    std::unique_ptr<T[]> data;
    alignas(64) std::atomic_size_t front{0};
    alignas(64) std::atomic_size_t back{0};
    const std::size_t capacity{16};
    const std::size_t mask = capacity - 1;
};