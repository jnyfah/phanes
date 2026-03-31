module;

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

module builder:deque;

// Typename T here is a trival type of DirectorId (size_t) defined in core

template <typename T>
class LockFreeDeque
{
    static_assert(std::is_trivially_copyable_v<T>, "LockFreeDeque<T> expects T to be trivially copyable");
    static_assert(std::is_trivially_destructible_v<T>, "LockFreeDeque<T> expects T to be trivially destructible");

  public:
    LockFreeDeque() : data(std::make_unique<T[]>(capacity))
    {
        static_assert((capacity & (capacity - 1)) == 0, "capacity must be power of two");
    }

    LockFreeDeque(const LockFreeDeque&) = delete;
    auto operator=(const LockFreeDeque&) -> LockFreeDeque& = delete;
    LockFreeDeque(LockFreeDeque&&) = delete;
    auto operator=(LockFreeDeque&&) -> LockFreeDeque& = delete;

    auto push_back(T item) noexcept -> bool
    {
        const auto b = back.load(std::memory_order_relaxed); // back is an owned variable by one owner, he alone does
                                                             // the writes so so relaxed is enough here.

        const auto f = front.load(
            std::memory_order_acquire); // front is shared with thieves, so read it with acquire when checking capacity.

        if (b - f >= capacity)
        {
            return false; // full
        }

        data[b & mask] = item;
        back.store(b + 1, std::memory_order_release); // then publish the new back
        return true;
    }

    auto pop_back() noexcept -> std::optional<T>
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

    auto steal_front() noexcept -> std::optional<T>
    {
        auto f = front.load(std::memory_order_acquire); // front is the shared point for thieves.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto b = back.load(std::memory_order_acquire); // back is the owner's published boundary of available work.

        if (f >= b)
        {
            return std::nullopt; // empty
        }

        const T value = data[f & mask];

        if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
            return std::nullopt;
        }

        return value;
    }

    [[nodiscard]] auto empty() const noexcept -> bool
    {
        const auto f = front.load(std::memory_order_acquire); // acq because we need to observe front and back
        const auto b = back.load(std::memory_order_acquire);

        return f >= b;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t
    {

        const auto f = front.load(std::memory_order_acquire); // acq because we need to observe front and back
        const auto b = back.load(std::memory_order_acquire);
        return (b > f) ? static_cast<std::size_t>(b - f) : 0;
    }

  private:
    static constexpr std::size_t capacity{64};
    static constexpr std::size_t mask{capacity - 1};
    std::unique_ptr<T[]> data;
    alignas(64) std::atomic<std::int64_t> front{0};
    alignas(64) std::atomic<std::int64_t> back{0};
};