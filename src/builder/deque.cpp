module;

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

export module phanes_deque;

struct alignas(64) PaddedEpoch
{
    std::atomic<std::uint64_t> value{EpochDomain::kInactive};
};

struct EpochDomain
{
    // UINT64_MAX is used to indicate that a thread is not currently active in the epoch domain.
    static constexpr std::uint64_t kInactive = ~std::uint64_t{0};

    alignas(64) std::atomic<std::uint64_t> global_epoch{0};

    std::vector<PaddedEpoch> local_epochs;

    explicit EpochDomain(std::size_t n) : local_epochs(n)
    {
        for (auto& epoch : local_epochs)
        {
            epoch.store(kInactive, std::memory_order_relaxed);
        }
    }

    // atomics cannot be copied, make that explicit
    EpochDomain(const EpochDomain&) = delete;
    auto operator=(const EpochDomain&) -> EpochDomain& = delete;
    EpochDomain(EpochDomain&&) = delete;
    auto operator=(EpochDomain&&) -> EpochDomain& = delete;
};

// RAAI guard
struct EpochGuard
{
    std::atomic<std::uint64_t>& slot;

    EpochGuard(std::atomic<std::uint64_t>& local_epoch, std::uint64_t global_epoch) : slot(local_epoch)
    {
        slot.store(global_epoch, std::memory_order_release);
    }

    ~EpochGuard() { slot.store(EpochDomain::kInactive, std::memory_order_release); }
};

// Typename T here is a trival type of DirectorId (size_t) defined in core, so this deque would only work for trivial
// types
export template <typename T>
concept DequeElement = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

export template <DequeElement T>
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
    explicit LockFreeDeque(std::int64_t cap = 1024,
                           std::size_t num_stealers =
                               std::max(std::size_t{1}, static_cast<std::size_t>(std::jthread::hardware_concurrency())))
        : domain(num_stealers)
    {
        buffer.store(new Buffer(cap), std::memory_order_relaxed);
    }

    ~LockFreeDeque() { delete buffer.load(std::memory_order_relaxed); }

    LockFreeDeque(const LockFreeDeque&) = delete;
    auto operator=(const LockFreeDeque&) -> LockFreeDeque& = delete;
    LockFreeDeque(LockFreeDeque&&) = delete;
    auto operator=(LockFreeDeque&&) -> LockFreeDeque& = delete;

    auto push_back(T item) -> void
    {
        const auto b = back.load(std::memory_order_relaxed); // back is an owned variable by one owner, and he alone
                                                             // does the writes so no need to sync with himself .

        const auto f = front.load(
            std::memory_order_acquire); // acq to sync with the thieves that update front, this can be relaxed

        auto* buf = buffer.load(
            std::memory_order_relaxed); // buffer is only written to by push_back function, so no sync needed
        if (b - f >= buf->capacity)
        {
            auto old_raw = buf;
            auto next = buf->resize(f, b);
            Buffer* next_raw = next.release();
            buffer.store(next_raw, std::memory_order_release); // sync with other threads that will read this
            buf = next_raw;

            retire(old_raw); // hand the old buffer to EBR to free later when no hazard exists
        }

        buf->data[b & buf->mask] = item;
        back.store(b + 1, std::memory_order_release); // we want to observe the write to the buffer data before we store
    }

    auto pop_back() noexcept -> std::optional<T>
    {
        auto b = back.load(std::memory_order_relaxed);

        b = b - 1;
        back.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);

        auto f = front.load(std::memory_order_relaxed);

        if (f > b)
        {
            back.store(f, std::memory_order_relaxed); // no real work to made visible to other threads so relaxed
            return std::nullopt;
        }
        auto* buf = buffer.load(std::memory_order_relaxed); // again here own thread is reading buffer, relaxed is fine

        // last item !!
        if (f == b)
        {
            // CAS incase anything has changed since we last read front, maybe some thief has stolen already and we are
            // empty
            if (!front.compare_exchange_strong(f, f + 1, std::memory_order_seq_cst, std::memory_order_relaxed))
            {
                back.store(f, std::memory_order_relaxed); // no real work to made visible to other threads so relaxed
                return std::nullopt;
            }

            // if yes then update back, so back and front are same now since we are empty
            back.store(f + 1, std::memory_order_relaxed);
            return buf->data[b & buf->mask];
        }
        return buf->data[b & buf->mask];
    }

    auto steal_front(size_t id) noexcept -> std::optional<T>
    {
        assert(id < domain.local_epochs.size() && "stealer id out of range for hazard domain");

        auto f = front.load(std::memory_order_relaxed); // we have so many thieves os sync with them  but
        std::atomic_thread_fence(std::memory_order_seq_cst);
        auto b = back.load(std::memory_order_acquire); // sync with owner thread

        if (f >= b)
        {
            return std::nullopt; // empty, nothing to steal
        }

        // Pin the current epoch before we read the buffer, so that the buffer is not freed while we are reading it
        EpochGuard guard(domain.local_epochs[id], domain.global_epoch.load(std::memory_order_seq_cst));
        std::atomic_thread_fence(std::memory_order_seq_cst);

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
        const auto f = front.load(std::memory_order_relaxed);
        const auto b = back.load(std::memory_order_relaxed);

        return f >= b;
    }

    [[nodiscard]] auto size() const noexcept -> std::size_t
    {
        const auto f = front.load(std::memory_order_relaxed);
        const auto b = back.load(std::memory_order_relaxed);
        return (b > f) ? static_cast<std::size_t>(b - f) : 0;
    }

  private:
    auto retire(Buffer* buf) -> void
    {
        const auto global_epoch = domain.global_epoch.load(std::memory_order_relaxed);
        retired[global_epoch % 3].push_back(
            std::unique_ptr<Buffer>(buf)); // put the old buffer in the retired list for this epoch
        try_advance_epoch(
            global_epoch); // try to advance the epoch, which may free some retired buffers if no hazard exists
    }

    auto try_advance_epoch(std::uint64_t current_epoch) -> void
    {
        const auto next_epoch = current_epoch + 1;
        for (const auto& slot : domain.local_epochs)
        {
            const auto local_epoch = slot.load(std::memory_order_seq_cst);
            if (local_epoch != EpochDomain::kInactive && local_epoch != current_epoch)
            {
                return; // found a hazard, cannot advance epoch
            }
        }

        // no hazard found, advance epoch
        domain.global_epoch.store(next_epoch, std::memory_order_release);

        // free buffer from two epochs ago
        auto& to_free = retired[(next_epoch + 1) % 3];
        to_free.clear();
    }

    alignas(64) std::atomic<std::int64_t> front{0};
    alignas(64) std::atomic<std::int64_t> back{0};
    alignas(64) std::atomic<Buffer*> buffer;
    std::array<std::vector<std::unique_ptr<Buffer>>, 3> retired;
    EpochDomain domain;
};