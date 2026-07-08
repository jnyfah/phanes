module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <ioringapi.h> // CreateIoRing / BuildIoRingReadFile / SubmitIoRing / PopIoRingCompletion

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

export module phanes_ioring;

import core;

// Windows IoRing backend, mirroring the Linux Uring class (init / submit / next / data / release / reset).
//
// Unlike io_uring, the Win32 IoRing API owns the submission/completion ring memory, so there is no
// mmap or manual head/tail: BuildIoRingReadFile queues a read, SubmitIoRing hands the queue to the
// kernel (and can wait), PopIoRingCompletion drains one completion.
//
// NOTE: Windows-only. Needs a recent Windows SDK (ioringapi.h) and Win11 22H2+ at runtime for
// IORING_VERSION_3. Not compiled on Linux — CMake selects uring.cpp there and this on WIN32.

export struct Data
{
    std::vector<std::byte> buf;
    HANDLE fd{INVALID_HANDLE_VALUE};
};

export struct Result
{
    size_t tag;
    int res; // bytes read, or < 0 on failure  (mirrors Uring's res)
};

export class IoRing
{
  public:
    IoRing() = default;

    IoRing(const IoRing&) = delete;
    IoRing& operator=(const IoRing&) = delete;

    IoRing(IoRing&&) noexcept = default;
    IoRing& operator=(IoRing&&) noexcept = default;

    ~IoRing()
    {
        for (const auto& d : buffer)
        {
            if (d.fd != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(d.fd);
            }
        }
        if (handle)
        {
            ::CloseIoRing(handle);
        }
    }

    auto init(unsigned entries) -> std::expected<void, ErrorKind>
    {
        IORING_CREATE_FLAGS flags{};
        flags.Required = IORING_CREATE_REQUIRED_FLAGS_NONE;
        flags.Advisory = IORING_CREATE_ADVISORY_FLAGS_NONE;

        // submission + completion queue both sized to `entries`
        HRESULT hr = ::CreateIoRing(IORING_VERSION_3, flags, entries, entries, &handle);
        if (FAILED(hr))
        {
            return std::unexpected(ErrorKind::IOError);
        }
        return {};
    }

    auto data(size_t tag) const -> const std::vector<std::byte>& { return buffer[tag].buf; }

    void release(size_t tag)
    {
        if (buffer[tag].fd != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(buffer[tag].fd);
        }
        buffer[tag].fd = INVALID_HANDLE_VALUE;
        free_slots.push_back(tag);
    }

    void reset()
    {
        for (const auto& d : buffer)
        {
            if (d.fd != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(d.fd);
            }
        }
        buffer.clear();
        free_slots.clear();
        pending = 0;
    }

    auto submit(const char* file, size_t len, long long offset) -> std::expected<size_t, ErrorKind>
    {
        HANDLE fd = ::CreateFileA(
            file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fd == INVALID_HANDLE_VALUE)
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

        // the API owns the SQE; we just describe the read. user_data (tag) comes back on the completion.
        IORING_HANDLE_REF fileRef = IoRingHandleRefFromHandle(fd);
        IORING_BUFFER_REF bufRef = IoRingBufferRefFromPointer(buffer[tag].buf.data());

        HRESULT hr = ::BuildIoRingReadFile(handle,
                                           fileRef,
                                           bufRef,
                                           static_cast<UINT32>(len),
                                           static_cast<UINT64>(offset),
                                           static_cast<UINT_PTR>(tag),
                                           IOSQE_FLAGS_NONE);
        if (FAILED(hr))
        {
            ::CloseHandle(fd);
            buffer[tag].fd = INVALID_HANDLE_VALUE;
            free_slots.push_back(tag);
            return std::unexpected(ErrorKind::IOError);
        }

        pending++; // queued, but not yet submitted to the kernel
        return tag;
    }

    auto next() -> std::expected<Result, ErrorKind>
    {
        for (;;)
        {
            IORING_CQE cqe{};
            HRESULT hr = ::PopIoRingCompletion(handle, &cqe);
            if (hr == S_OK) // a completion was available
            {
                // flush any still-queued reads (submit, do not wait) so they get processed
                if (pending > 0)
                {
                    UINT32 submitted = 0;
                    ::SubmitIoRing(handle, 0, 0, &submitted);
                    pending = 0;
                }

                int res = SUCCEEDED(cqe.ResultCode) ? static_cast<int>(cqe.Information) : -1;
                return Result{static_cast<size_t>(cqe.UserData), res};
            }

            // completion queue empty (S_FALSE): submit queued reads and block for at least one
            UINT32 submitted = 0;
            HRESULT s = ::SubmitIoRing(handle, 1, INFINITE, &submitted);
            pending = 0;
            if (FAILED(s))
            {
                return std::unexpected(ErrorKind::IOError);
            }
        }
    }

  private:
    HIORING handle{};
    std::vector<Data> buffer;
    std::vector<size_t> free_slots;
    unsigned pending = 0;
};
