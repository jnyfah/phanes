module;

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

export module phanes_uring;

import core;

static auto io_uring_setup(unsigned entries, io_uring_params* p) -> int
{
    return static_cast<int>(::syscall(SYS_io_uring_setup, entries, p));
}

static auto io_uring_enter(int ring_fd, unsigned to_submit, unsigned min_complete, unsigned flags) -> int
{
    return static_cast<int>(::syscall(SYS_io_uring_enter, ring_fd, to_submit, min_complete, flags, nullptr, 0));
}

struct SqRing
{
    __u32* head;
    __u32* tail;
    __u32* mask;
    __u32* entries;
    __u32* flags;
    __u32* array;
};

struct CqRing
{
    __u32* head;
    __u32* tail;
    __u32* mask;
    __u32* entries;
    void* cqes;
};

export struct Data
{
    std::vector<std::byte> buf;
    int fd;
};

export struct Result
{
    size_t tag;
    int res;
};

export class Uring
{
  public:
    Uring() = default;

    Uring(const Uring&) = delete;
    Uring& operator=(const Uring&) = delete;

    Uring(Uring&&) noexcept = default;
    Uring& operator=(Uring&&) noexcept = default;

    ~Uring()
    {
        if (ring >= 0)
        {
            ::close(ring);
        }

        if (cq_ptr && cq_ptr != sq_ptr)
        {
            ::munmap(cq_ptr, cring_size);
        }

        if (sq_ptr)
        {
            ::munmap(sq_ptr, sring_size);
        }

        if (sqe)
        {
            ::munmap(sqe, sqes_sz);
        }

        for (const auto& data : buffer)
        {
            if (data.fd >= 0)
            {
                ::close(data.fd);
            }
        }
    }

    auto data(size_t tag) const -> const std::vector<std::byte>& { return buffer[tag].buf; }

    void release(size_t tag)
    {
        ::close(buffer[tag].fd);
        buffer[tag].fd = -1;
        free_slots.push_back(tag);
    }

    void reset()
    {
        for (const auto& d : buffer)
        {
            if (d.fd >= 0)
            {
                ::close(d.fd);
            }
        }
        buffer.clear();
        free_slots.clear();
        pending = 0;
    }

    auto submit(const char* file, size_t len, off_t offset) -> std::expected<size_t, ErrorKind>
    {
        // SQ-full guard
        auto sq_head = std::atomic_ref<__u32>(*sring.head).load(std::memory_order_acquire);
        if ((*sring.tail - sq_head) >= param.sq_entries)
        {
            return std::unexpected(ErrorKind::IOError);
        }

        int fd = ::open(file, O_RDONLY);
        if (fd < 0)
        {
            return std::unexpected(ErrorKind::FileError);
        }

        size_t tag;

        if (!free_slots.empty())
        {
            tag = free_slots.back();
            free_slots.pop_back();
        }
        else
        {
            tag = buffer.size();
            buffer.emplace_back();
        }

        buffer[tag].buf.resize(len);
        buffer[tag].fd = fd;

        auto tail = *sring.tail;
        auto index = tail & *sring.mask;

        // fill in data into the SQE
        auto sqentry = &sqe[index];
        std::memset(sqentry, 0, sizeof(*sqentry));
        sqentry->fd = fd;
        sqentry->opcode = IORING_OP_READ;
        sqentry->len = len;
        sqentry->addr = reinterpret_cast<std::uint64_t>(buffer[tag].buf.data());
        sqentry->user_data = tag;
        sqentry->off = offset;

        // sq ring references sqe via index
        sring.array[index] = index;

        // release so the kernel can observe the write
        std::atomic_ref<__u32>(*sring.tail).store(tail + 1, std::memory_order_release);
        pending++; // queued, but not yet submitted to the kernel
        return tag;
    }

    auto next() -> std::expected<Result, ErrorKind>
    {
        auto head = *cring.head;

        for (;;)
        {
            // user does not own tail, so read with acquire
            auto tail = std::atomic_ref<__u32>(*cring.tail).load(std::memory_order_acquire);

            // all completions are ready and there is nothing left to submit in io_uring_enter
            // so leave the loop
            if (head != tail && pending == 0)
            {
                break;
            }

            // if the cqe is not empty, sumbit all queued sqe bu dont wait for results since there are pending results
            // in the cqe already
            unsigned min_complete = (head == tail) ? 1u : 0u;
            int ret = io_uring_enter(ring, pending, min_complete, IORING_ENTER_GETEVENTS);
            if (ret < 0)
            {
                // failure
                if (errno == EINTR)
                {
                    continue;
                }
                return std::unexpected(ErrorKind::IOError);
            }
            pending = 0;
        }

        auto* cqe = &reinterpret_cast<io_uring_cqe*>(cring.cqes)[head & *cring.mask];
        auto tag = cqe->user_data;
        auto res = cqe->res;

        // release so kernel can see this update
        std::atomic_ref<__u32>(*cring.head).store(head + 1, std::memory_order_release);

        return Result{tag, res};
    }

    auto init(unsigned entries) -> std::expected<void, ErrorKind>
    {
        ring = io_uring_setup(entries, &param);
        if (ring < 0)
        {
            return std::unexpected(ErrorKind::IOError);
        }

        // size of ring = size of elements before array + (no of array elements* size of array type)
        sring_size = param.sq_off.array + (param.sq_entries * sizeof(unsigned));
        cring_size = param.cq_off.cqes + (param.cq_entries * sizeof(io_uring_cqe));

        // can both rings fit in one MMAP?
        if (param.features & IORING_FEAT_SINGLE_MMAP)
        {
            sring_size = std::max(sring_size, cring_size);
            cring_size = sring_size;
        }

        // set MMAP
        sq_ptr =
            ::mmap(nullptr, sring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring, IORING_OFF_SQ_RING);
        if (sq_ptr == MAP_FAILED)
        {
            return std::unexpected(ErrorKind::IOError);
        }

        if (param.features & IORING_FEAT_SINGLE_MMAP)
        {
            cq_ptr = sq_ptr;
        }
        else
        {
            cq_ptr = ::mmap(nullptr,
                            cring_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_POPULATE,
                            ring,
                            IORING_OFF_CQ_RING);
            if (cq_ptr == MAP_FAILED)
                return std::unexpected(ErrorKind::IOError);
        }

        sqes_sz = param.sq_entries * sizeof(io_uring_sqe);
        sqe = static_cast<io_uring_sqe*>(
            ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring, IORING_OFF_SQES));

        if (sqe == MAP_FAILED)
        {
            return std::unexpected(ErrorKind::IOError);
        }

        // fill user data
        auto sq_char = static_cast<std::byte*>(sq_ptr);

        // address of shared memory + offset
        sring.head = reinterpret_cast<__u32*>(sq_char + param.sq_off.head);
        sring.tail = reinterpret_cast<__u32*>(sq_char + param.sq_off.tail);
        sring.entries = reinterpret_cast<__u32*>(sq_char + param.sq_off.ring_entries);
        sring.flags = reinterpret_cast<__u32*>(sq_char + param.sq_off.flags);
        sring.mask = reinterpret_cast<__u32*>(sq_char + param.sq_off.ring_mask);
        sring.array = reinterpret_cast<__u32*>(sq_char + param.sq_off.array);

        auto cq_char = static_cast<std::byte*>(cq_ptr);
        cring.head = reinterpret_cast<__u32*>(cq_char + param.cq_off.head);
        cring.tail = reinterpret_cast<__u32*>(cq_char + param.cq_off.tail);
        cring.mask = reinterpret_cast<__u32*>(cq_char + param.cq_off.ring_mask);
        cring.cqes = reinterpret_cast<io_uring_cqe*>(cq_char + param.cq_off.cqes);

        return {};
    }

  private:
    SqRing sring{};
    CqRing cring{};

    size_t sring_size{};
    size_t cring_size{};
    std::size_t sqes_sz{};

    io_uring_params param{};
    io_uring_sqe* sqe{};

    int ring = -1;
    std::vector<Data> buffer;

    void* sq_ptr{nullptr};
    void* cq_ptr{nullptr};

    std::vector<size_t> free_slots; // indices ready to reuse
    unsigned pending = 0; // SQEs queued but not yet handed to the kernel
};