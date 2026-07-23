module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <ioringapi.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

export module phanes_io;

import core;

export struct Data
{
    std::vector<std::byte> buf;
    HANDLE fd{INVALID_HANDLE_VALUE};
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

    auto init(unsigned entries) -> std::expected<void, ErrorKind>
    {
        IORING_CAPABILITIES caps{};
        HRESULT hr = ::QueryIoRingCapabilities(&caps);
        if (FAILED(hr) || caps.MaxVersion == IORING_VERSION_INVALID)
        {
            return std::unexpected(ErrorKind::IOError);
        }

        const auto version = (std::min)(caps.MaxVersion, IORING_VERSION_3);
        entries = (std::min)(entries, static_cast<unsigned>(caps.MaxSubmissionQueueSize));

        IORING_CREATE_FLAGS flags{};
        flags.Required = IORING_CREATE_REQUIRED_FLAGS_NONE;
        flags.Advisory = IORING_CREATE_ADVISORY_FLAGS_NONE;

        hr = ::CreateIoRing(version, flags, entries, entries * 2, &handle);
        if (FAILED(hr))
        {
            handle = nullptr;
            return std::unexpected(ErrorKind::IOError);
        }

        IORING_INFO info{};
        hr = ::GetIoRingInfo(handle, &info);
        if (FAILED(hr))
        {
            ::CloseIoRing(handle);
            handle = nullptr;
            return std::unexpected(ErrorKind::IOError);
        }

        sq_entries = info.SubmissionQueueSize;
        cq_entries = info.CompletionQueueSize;

        completion_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (completion_event)
        {
            have_event = SUCCEEDED(::SetIoRingCompletionEvent(handle, completion_event));
            if (!have_event)
            {
                ::CloseHandle(completion_event);
                completion_event = nullptr;
            }
        }

        return {};
    }

    auto data(size_t tag) const -> const std::vector<std::byte>& { return buffer[tag].buf; }

    void release(size_t tag)
    {
        if (tag >= buffer.size())
        {
            return;
        }
        if (buffer[tag].fd != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(buffer[tag].fd);
            buffer[tag].fd = INVALID_HANDLE_VALUE;
        }
        free_slots.push_back(tag);
    }

    void reset()
    {
        drain();

        for (auto& d : buffer)
        {
            if (d.fd != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(d.fd);
                d.fd = INVALID_HANDLE_VALUE;
            }
        }
        for (HANDLE fd : files)
        {
            if (fd != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(fd);
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
        HANDLE fd = ::CreateFileW(file.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr);
        if (fd == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(ErrorKind::FileError);
        }

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
        if (fh >= files.size() || files[fh] == INVALID_HANDLE_VALUE)
        {
            return;
        }
        ::CloseHandle(files[fh]);
        files[fh] = INVALID_HANDLE_VALUE;
        free_files.push_back(fh);
    }

    // single-shot: opens the file and submits one read; the slot owns the fd and closes it on release.
    auto submit(const std::filesystem::path& file, size_t len, int64_t offset) -> std::expected<size_t, ErrorKind>
    {
        if (len > (std::numeric_limits<UINT32>::max)())
        {
            return std::unexpected(ErrorKind::IOError);
        }
        if (pending >= sq_entries)
        {
            return std::unexpected(ErrorKind::Again); // backpressure, not a failure
        }

        HANDLE fd = ::CreateFileW(file.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr);
        if (fd == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(ErrorKind::FileError);
        }

        size_t tag = alloc_slot(len);
        buffer[tag].fd = fd; // slot owns this fd
        if (!queue_read(tag, fd, len, offset))
        {
            ::CloseHandle(fd);
            buffer[tag].fd = INVALID_HANDLE_VALUE;
            free_slots.push_back(tag);
            return std::unexpected(ErrorKind::IOError);
        }
        return tag;
    }

    // read against an already-open handle; the slot does not own the fd (close_file does).
    auto submit(size_t fh, size_t len, int64_t offset) -> std::expected<size_t, ErrorKind>
    {
        if (fh >= files.size() || files[fh] == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(ErrorKind::FileError);
        }
        if (len > (std::numeric_limits<UINT32>::max)())
        {
            return std::unexpected(ErrorKind::IOError);
        }
        if (pending >= sq_entries)
        {
            return std::unexpected(ErrorKind::Again);
        }

        size_t tag = alloc_slot(len);
        buffer[tag].fd = INVALID_HANDLE_VALUE; // the open handle owns the fd, not the slot
        if (!queue_read(tag, files[fh], len, offset))
        {
            free_slots.push_back(tag);
            return std::unexpected(ErrorKind::IOError);
        }
        return tag;
    }

    auto next() -> std::expected<Result, ErrorKind>
    {
        for (;;)
        {
            IORING_CQE cqe{};
            const HRESULT hr = ::PopIoRingCompletion(handle, &cqe);

            if (hr == S_OK)
            {
                --inflight;
                return Result{static_cast<size_t>(cqe.UserData), to_res(cqe)};
            }

            if (hr != S_FALSE)
            {
                return std::unexpected(ErrorKind::IOError);
            }

            if (pending == 0 && inflight == 0)
            {
                return std::unexpected(ErrorKind::Empty);
            }

            if (pending > 0)
            {
                UINT32 submitted = 0;
                const HRESULT s = ::SubmitIoRing(handle, 0, 0, &submitted);
                if (FAILED(s))
                {
                    return std::unexpected(ErrorKind::IOError);
                }

                pending -= (std::min)(pending, static_cast<unsigned>(submitted));
                inflight += static_cast<unsigned>(submitted);
                continue; // go round and pop
            }

            // pending == 0 && inflight > 0: wait for the kernel.
            if (have_event)
            {
                if (::WaitForSingleObject(completion_event, INFINITE) != WAIT_OBJECT_0)
                {
                    return std::unexpected(ErrorKind::IOError);
                }
            }
            else
            {
                UINT32 submitted = 0;
                if (FAILED(::SubmitIoRing(handle, 1, INFINITE, &submitted)))
                {
                    return std::unexpected(ErrorKind::IOError);
                }
            }
        }
    }

    void drain()
    {
        while (pending > 0 || inflight > 0)
        {
            if (!next())
            {
                break; // Empty, or an error we cannot recover from
            }
        }
    }

  private:
    static auto to_res(const IORING_CQE& cqe) -> int
    {
        if (SUCCEEDED(cqe.ResultCode))
        {
            return static_cast<int>(cqe.Information); // bytes actually read
        }

        const int code = static_cast<int>(HRESULT_CODE(cqe.ResultCode));
        return code != 0 ? -code : -1;
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

    auto queue_read(size_t tag, HANDLE fd, size_t len, int64_t offset) -> bool
    {
        auto fileRef = IoRingHandleRefFromHandle(fd);
        auto bufRef = IoRingBufferRefFromPointer(buffer[tag].buf.data());
        const HRESULT hr = ::BuildIoRingReadFile(handle,
                                                 fileRef,
                                                 bufRef,
                                                 static_cast<UINT32>(len),
                                                 static_cast<UINT64>(offset),
                                                 static_cast<UINT_PTR>(tag),
                                                 IOSQE_FLAGS_NONE);
        if (FAILED(hr))
        {
            return false;
        }
        pending++;
        return true;
    }

    void adopt(Ring& other) noexcept
    {
        handle = std::exchange(other.handle, nullptr);
        completion_event = std::exchange(other.completion_event, nullptr);
        have_event = std::exchange(other.have_event, false);
        sq_entries = std::exchange(other.sq_entries, 0);
        cq_entries = std::exchange(other.cq_entries, 0);
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
        if (handle)
        {
            // Reap outstanding reads before the buffers they target go away.
            drain();
            ::CloseIoRing(handle);
            handle = nullptr;
        }

        if (completion_event)
        {
            ::CloseHandle(completion_event);
            completion_event = nullptr;
        }
        have_event = false;

        for (const auto& d : buffer)
        {
            if (d.fd != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(d.fd);
            }
        }
        for (HANDLE fd : files)
        {
            if (fd != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(fd);
            }
        }

        buffer.clear();
        files.clear();
        free_slots.clear();
        free_files.clear();
        pending = 0;
        inflight = 0;
    }

    HIORING handle{};
    HANDLE completion_event{nullptr};
    bool have_event = false;

    std::vector<Data> buffer;
    std::vector<size_t> free_slots;

    unsigned pending = 0; // entries built into the SQ but not yet submitted
    unsigned inflight = 0; // entries the kernel accepted but has not completed
    unsigned sq_entries = 0;
    unsigned cq_entries = 0;

    std::vector<HANDLE> files;
    std::vector<size_t> free_files;
};
