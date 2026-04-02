module;

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

module builder:deque;

template <typename T>
concept DequeElement = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// Typename T here is a trival type of DirectorId (size_t) defined in core, so this deque would only work for trivial
// types
template <DequeElement T>
class LockFreeDeque
{

  private:
    struct Buffer
    {
        std::int64_t capacity;
        std::int64_t mask;
        std::unique_ptr<T[]> data;

        explicit Buffer(std::int64_t c)
            : capacity(c), mask(c - 1), data(std::make_unique<T[]>(static_cast<std::size_t>(c)))
        {
            assert(capacity && (!(capacity & (capacity - 1))) && "Capacity must be buf power of 2!");
        }

        auto resize(std::int64_t f, std::int64_t b) const -> std::unique_ptr<Buffer>
        {
            auto next = std::make_unique<Buffer>(capacity * 2);
            for (std::int64_t i = f; i < b; ++i)
            {
                next->data[i & next->mask] = data[i & mask];
            }
            return next;
        }

        Buffer(const Buffer&) = delete;
        auto operator=(const Buffer&) -> Buffer& = delete;
    };

  public:
    explicit LockFreeDeque(std::int64_t cap = 1024)
    {
        auto first = std::make_unique<Buffer>(cap);
        buffer.store(first.get(), std::memory_order_relaxed);
        oldBuffer.push_back(std::move(first));
    }

    ~LockFreeDeque() = default;

    LockFreeDeque(const LockFreeDeque&) = delete;
    auto operator=(const LockFreeDeque&) -> LockFreeDeque& = delete;
    LockFreeDeque(LockFreeDeque&&) = delete;
    auto operator=(LockFreeDeque&&) -> LockFreeDeque& = delete;

    auto push_back(T item) -> void
    {
        const auto b = back.load(std::memory_order_relaxed); // back is an owned variable by one owner, he alone does
                                                             // the writes so so relaxed is enough here.

        const auto f = front.load(
            std::memory_order_acquire); // front is shared with thieves, so read it with acquire when checking capacity.

        auto* buf = buffer.load(std::memory_order_relaxed);
        if (b - f >= buf->capacity)
        {
            auto next = buf->resize(f, b);
            Buffer* next_raw = next.get();
            oldBuffer.push_back(std::move(next));
            buffer.store(next_raw, std::memory_order_release);
            buf = next_raw;
        }

        buf->data[b & buf->mask] = item;
        back.store(b + 1, std::memory_order_release); // then publish the new back
    }

    auto pop_back() noexcept -> std::optional<T>
    {
        auto b = back.load(std::memory_order_relaxed);

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
        auto* buf = buffer.load(std::memory_order_relaxed);

        // if there is one item left, CAS to see who wins the race to pick the last item
        if (f == b)
        {
            // am I the one to change front ??
            if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                // if no, a thief took the last item; restore back to match the new front
                back.store(f, std::memory_order_relaxed);
                return std::nullopt;
            }

            // if yes then update back
            back.store(f + 1, std::memory_order_relaxed);
            return buf->data[b & buf->mask];
        }
        return buf->data[b & buf->mask];
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

        auto* buf = buffer.load(std::memory_order_relaxed);
        const T value = buf->data[f & buf->mask];

        if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
        {
            return std::nullopt;
        }

        return value;
    }

    [[nodiscard]] auto empty() const noexcept -> bool
    {
        const auto f = front.load(
            std::memory_order_relaxed); // relaxed: just reading counters, no associated memory to synchronize
        const auto b = back.load(std::memory_order_relaxed);

        return f >= b;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t
    {
        const auto f =
            front.load(std::memory_order_relaxed); // relaxed: result is inherently approximate (two separate loads),
        const auto b = back.load(std::memory_order_relaxed); //          no acquire-release pair to complete here
        return (b > f) ? static_cast<std::size_t>(b - f) : 0;
    }

  private:
    alignas(64) std::atomic<std::int64_t> front{0};
    alignas(64) std::atomic<std::int64_t> back{0};
    alignas(64) std::atomic<Buffer*> buffer;
    std::vector<std::unique_ptr<Buffer>> oldBuffer;
};