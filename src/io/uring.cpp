module;

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

export module phanes_io;

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
    __u32* head{};
    __u32* tail{};
    __u32* mask{};
    __u32* entries{};
    __u32* flags{};
    __u32* array{};
};

struct CqRing
{
    __u32* head{};
    __u32* tail{};
    __u32* mask{};
    __u32* entries{};
    void* cqes{};
};

export struct Data
{
    std::vector<std::byte> buf;
    int fd = -1;
};

export struct Result
{
    size_t tag;
    int res;
};

export class Ring
{
  public:
    Ring() = default;

    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    Ring(Ring&& other) noexcept { adopt(other); }

    Ring& operator=(Ring&& other) noexcept
    {
        if (this != &other)
        {
            teardown();
            adopt(other);
        }
        return *this;
    }

    ~Ring() { teardown(); }

    auto data(size_t tag) const -> const std::vector<std::byte>& { return buffer[tag].buf; }

    void release(size_t tag)
    {
        if (tag >= buffer.size())
        {
            return;
        }
        if (buffer[tag].fd >= 0)
        {
            ::close(buffer[tag].fd);
            buffer[tag].fd = -1;
        }
        free_slots.push_back(tag);
    }

    void reset()
    {

        drain();

        for (auto& d : buffer)
        {
            if (d.fd >= 0)
            {
                ::close(d.fd);
                d.fd = -1;
            }
        }
        for (int fd : files)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }
        free_slots.clear();
        for (std::size_t i = 0; i < buffer.size(); ++i)
        {
            free_slots.push_back(i);
        }
        files.clear();
        free_files.clear();
    }

    // open a file once so many reads can be submitted against it via submit(handle, ...).
    auto open(const std::filesystem::path& file) -> std::expected<size_t, ErrorKind>
    {
        int fd = ::open(file.c_str(), O_RDONLY);
        if (fd < 0)
        {
            return std::unexpected(ErrorKind::FileError);
        }
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL); // full-file reads are sequential

        size_t fh;
        if (!free_files.empty())
        {
            fh = free_files.back();
            free_files.pop_back();
            files[fh] = fd;
        }
        else
        {
            fh = files.size();
            files.push_back(fd);
        }
        return fh;
    }

    void close_file(size_t fh)
    {
        if (fh >= files.size() || files[fh] < 0)
        {
            return;
        }
        ::close(files[fh]);
        files[fh] = -1;
        free_files.push_back(fh);
    }

    // single-shot: opens the file and submits one read; the slot owns the fd and closes it on release.
    auto submit(const std::filesystem::path& file, size_t len, int64_t offset) -> std::expected<size_t, ErrorKind>
    {
        if (sq_full())
        {
            return std::unexpected(ErrorKind::IOError); // backpressure, not a failure
        }

        // open the file
        int fd = ::open(file.c_str(), O_RDONLY);
        if (fd < 0)
        {
            return std::unexpected(ErrorKind::FileError);
        }
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);

        size_t tag = alloc_slot(len);
        buffer[tag].fd = fd; // slot owns this fd
        queue_read(tag, fd, len, offset);
        return tag;
    }

    // read against an already-open handle; the slot does not own the fd (close_file does).
    auto submit(size_t fh, size_t len, int64_t offset) -> std::expected<size_t, ErrorKind>
    {
        if (sq_full())
        {
            return std::unexpected(ErrorKind::IOError);
        }
        size_t tag = alloc_slot(len);
        buffer[tag].fd = -1; // the open handle owns the fd, not the slot
        queue_read(tag, files[fh], len, offset);
        return tag;
    }

    auto next() -> std::expected<Result, ErrorKind>
    {
        auto head = *cring.head;

        for (;;)
        {
            // user does not own tail, so read with acquire
            const auto tail = std::atomic_ref<__u32>(*cring.tail).load(std::memory_order_acquire);

            if (head != tail)
            {
                // a completion is ready. push any queued SQEs to the kernel on the
                // way past, but do not block for them.
                if (pending > 0)
                {
                    int ret = io_uring_enter(ring, pending, 0, IORING_ENTER_GETEVENTS);
                    if (ret >= 0)
                    {
                        pending -= static_cast<unsigned>(ret);
                        inflight += static_cast<unsigned>(ret);
                    }
                    else if (errno != EINTR && errno != EAGAIN && errno != EBUSY)
                    {
                        return std::unexpected(ErrorKind::IOError);
                    }
                }
                break; // leave the for loop
            }

            if (pending == 0 && inflight == 0)
            {
                return std::unexpected(ErrorKind::IOError);
            }

            int ret = io_uring_enter(ring, pending, 1, IORING_ENTER_GETEVENTS);
            if (ret < 0)
            {
                // failure
                if (errno == EINTR)
                {
                    continue;
                }
                return std::unexpected(ErrorKind::IOError);
            }
            pending -= static_cast<unsigned>(ret);
            inflight += static_cast<unsigned>(ret);
        }

        const auto* cqe = &reinterpret_cast<io_uring_cqe*>(cring.cqes)[head & *cring.mask];
        const auto tag = cqe->user_data;
        const auto res = cqe->res; // bytes read, or a negative errno

        --inflight;

        std::atomic_ref<__u32>(*cring.head).store(head + 1, std::memory_order_release);

        return Result{static_cast<size_t>(tag), res};
    }

    void drain()
    {
        while (pending > 0 || inflight > 0)
        {
            if (!next())
            {
                break;
            }
        }
    }

    auto init(unsigned entries) -> std::expected<void, ErrorKind>
    {
        ring = io_uring_setup(entries, &param);
        if (ring < 0)
        {
            ring = -1;
            return std::unexpected(ErrorKind::IOError);
        }

        // size of each ring = byte offset to its last field + (element count * element size)
        sring_size = param.sq_off.array + (param.sq_entries * sizeof(unsigned));
        cring_size = param.cq_off.cqes + (param.cq_entries * sizeof(io_uring_cqe));

        // can both rings fit in one MMAP?
        if (param.features & IORING_FEAT_SINGLE_MMAP)
        {
            sring_size = std::max(sring_size, cring_size);
            cring_size = sring_size;
        }

        void* p =
            ::mmap(nullptr, sring_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring, IORING_OFF_SQ_RING);
        if (p == MAP_FAILED)
        {
            return std::unexpected(ErrorKind::IOError);
        }
        sq_ptr = static_cast<std::byte*>(p);

        if (param.features & IORING_FEAT_SINGLE_MMAP)
        {
            cq_ptr = sq_ptr;
        }
        else
        {
            p = ::mmap(nullptr,
                       cring_size,
                       PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE,
                       ring,
                       IORING_OFF_CQ_RING);
            if (p == MAP_FAILED)
            {
                return std::unexpected(ErrorKind::IOError);
            }
            cq_ptr = static_cast<std::byte*>(p);
        }

        sqes_sz = param.sq_entries * sizeof(io_uring_sqe);
        p = ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, ring, IORING_OFF_SQES);
        if (p == MAP_FAILED)
        {
            return std::unexpected(ErrorKind::IOError);
        }
        sqe = static_cast<io_uring_sqe*>(p);

        // fill user data
        // address of shared memory + offset
        sring.head = reinterpret_cast<__u32*>(sq_ptr + param.sq_off.head);
        sring.tail = reinterpret_cast<__u32*>(sq_ptr + param.sq_off.tail);
        sring.entries = reinterpret_cast<__u32*>(sq_ptr + param.sq_off.ring_entries);
        sring.flags = reinterpret_cast<__u32*>(sq_ptr + param.sq_off.flags);
        sring.mask = reinterpret_cast<__u32*>(sq_ptr + param.sq_off.ring_mask);
        sring.array = reinterpret_cast<__u32*>(sq_ptr + param.sq_off.array);

        cring.head = reinterpret_cast<__u32*>(cq_ptr + param.cq_off.head);
        cring.tail = reinterpret_cast<__u32*>(cq_ptr + param.cq_off.tail);
        cring.entries = reinterpret_cast<__u32*>(cq_ptr + param.cq_off.ring_entries);
        cring.mask = reinterpret_cast<__u32*>(cq_ptr + param.cq_off.ring_mask);
        cring.cqes = reinterpret_cast<io_uring_cqe*>(cq_ptr + param.cq_off.cqes);

        return {};
    }

  private:
    auto sq_full() const -> bool
    {
        const auto sq_head = std::atomic_ref<__u32>(*sring.head).load(std::memory_order_acquire);
        return (*sring.tail - sq_head) >= param.sq_entries;
    }

    auto alloc_slot(size_t len) -> size_t
    {
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
        if (buffer[tag].buf.size() < len)
        {
            buffer[tag].buf.resize(len);
        }
        return tag;
    }

    void queue_read(size_t tag, int fd, size_t len, int64_t offset)
    {
        auto tail = *sring.tail;
        const auto index = tail & *sring.mask;

        auto* sqentry = &sqe[index];
        std::memset(sqentry, 0, sizeof(*sqentry));
        sqentry->fd = fd;
        sqentry->opcode = IORING_OP_READ;
        sqentry->len = static_cast<__u32>(len);
        sqentry->addr = reinterpret_cast<std::uint64_t>(buffer[tag].buf.data());
        sqentry->user_data = tag;
        sqentry->off = static_cast<__u64>(offset);
        sring.array[index] = index;

        // release so the kernel can observe the write
        std::atomic_ref<__u32>(*sring.tail).store(tail + 1, std::memory_order_release);
        pending++; // queued, but not yet submitted to the kernel
    }

    void adopt(Ring& other) noexcept
    {
        sring = std::exchange(other.sring, {});
        cring = std::exchange(other.cring, {});
        sring_size = std::exchange(other.sring_size, 0);
        cring_size = std::exchange(other.cring_size, 0);
        sqes_sz = std::exchange(other.sqes_sz, 0);
        param = other.param;
        sqe = std::exchange(other.sqe, nullptr);
        ring = std::exchange(other.ring, -1);
        sq_ptr = std::exchange(other.sq_ptr, nullptr);
        cq_ptr = std::exchange(other.cq_ptr, nullptr);
        pending = std::exchange(other.pending, 0);
        inflight = std::exchange(other.inflight, 0);
        buffer = std::move(other.buffer);
        free_slots = std::move(other.free_slots);
        files = std::move(other.files);
        free_files = std::move(other.free_files);
        other.buffer.clear();
        other.files.clear();
    }

    void teardown() noexcept
    {
        if (ring >= 0)
        {
            drain();
            ::close(ring);
            ring = -1;
        }

        if (cq_ptr && cq_ptr != sq_ptr)
        {
            ::munmap(cq_ptr, cring_size);
        }
        cq_ptr = nullptr;

        if (sq_ptr)
        {
            ::munmap(sq_ptr, sring_size);
        }
        sq_ptr = nullptr;

        if (sqe)
        {
            ::munmap(sqe, sqes_sz);
        }
        sqe = nullptr;

        for (const auto& d : buffer)
        {
            if (d.fd >= 0)
            {
                ::close(d.fd);
            }
        }
        for (int fd : files)
        {
            if (fd >= 0)
            {
                ::close(fd);
            }
        }
        buffer.clear();
        files.clear();
        free_slots.clear();
        free_files.clear();
        pending = 0;
        inflight = 0;
    }

    SqRing sring{};
    CqRing cring{};

    size_t sring_size{};
    size_t cring_size{};
    std::size_t sqes_sz{};

    io_uring_params param{};
    io_uring_sqe* sqe{};

    int ring = -1;
    std::vector<Data> buffer;

    std::byte* sq_ptr{nullptr};
    std::byte* cq_ptr{nullptr};

    std::vector<size_t> free_slots; // indices ready to reuse
    unsigned pending = 0; // SQEs queued in the ring but not yet handed to the kernel
    unsigned inflight = 0; // SQEs the kernel has accepted but not yet completed

    std::vector<int> files;
    std::vector<size_t> free_files;
};