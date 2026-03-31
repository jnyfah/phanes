module;

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>

module builder:deque;

// Typename T here is a trival type of DirectorId (size_t) defined in core

template <typename T>
class LockFreeDeque
{
  public:
    LockFreeDeque() : data(std::make_unique<T[]>(capacity))
    {
        assert((capacity & (capacity - 1)) == 0 && "capacity must be power of two");
    }

    auto push_back(T item) -> bool
    {
        const auto b = back.load(std::memory_order_relaxed); // this is an wned variable by one owner, he alone does the
                                                             // writes so no need for an order
        const auto f =
            front.load(std::memory_order_acquire); // acq because we have so many theives so ordering in necessary

        if (b - f >= capacity)
        {
            return false;
        }

        data[b & mask] = item;
        back.store(b + 1, std::memory_order_release);
        return true;
    }

    auto pop_back() -> std::optional<T>
    {
        auto b = back.load(std::memory_order_relaxed);

        if (b == 0)
        {
            return std::nullopt; // guard against underflow as b cant be -1
        }

        b = b - 1;
        back.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst); // storing back needs to happen before loading front
                                                             // compulsory, so we can compare the  sucessfully
        auto f = front.load(std::memory_order_relaxed);

        if (f > b)
        {
            // if the front has advanced past the back it means that the queue is empty
            back.store(f, std::memory_order_relaxed); // relaxed because its the owner doing this, and we dont need to
                                                      // announce that back changed
            return std::nullopt;
        }

        // if there is one item left, CAS to see who wins the race to pick the last item
        if (f == b)
        {
            // am I the one to change front ??
            if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                // if no then update the back to b
                back.store(f, std::memory_order_relaxed);
                return std::nullopt;
            }

            // if yes then update back
            back.store(f + 1, std::memory_order_relaxed);
            return data[b & mask];
        }
        return data[b & mask];
    }

    auto steal_front() -> std::optional<T>
    {
        auto f = front.load(std::memory_order_acquire); // acq because som theives might be stealing as we are too
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto b = back.load(std::memory_order_acquire); // acq because owner might be doing some modification to back

        if (f >= b)
        {
            return std::nullopt;
        }

        T x = data[f & mask];

        if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
            return std::nullopt;
        }

        return x;
    }

    [[nodiscard]] auto empty() const -> bool
    {
        const size_t f = front.load(std::memory_order_acquire); // acq because we need to observe front and back
        const size_t b = back.load(std::memory_order_acquire);

        return f >= b;
    }

    [[nodiscard]] auto size() const -> std::size_t
    {

        const size_t f = front.load(std::memory_order_acquire); // acq because we need to observe front and back
        const size_t b = back.load(std::memory_order_acquire);
        return (b >= f) ? (b - f) : 0;
    }

  private:
    static constexpr std::size_t capacity{64};
    static constexpr std::size_t mask{capacity - 1};
    std::unique_ptr<T[]> data;
    alignas(64) std::atomic_size_t front{0};
    alignas(64) std::atomic_size_t back{0};
};