module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <ioringapi.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
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

    Ring(Ring&&) noexcept = default;
    Ring& operator=(Ring&&) noexcept = default;

    ~Ring()
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
        sq_entries = entries;
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

    auto submit(const std::filesystem::path& file, size_t len, int64_t offset) -> std::expected<size_t, ErrorKind>
    {
        // SQ-full guard: cannot queue more than the submission queue holds before a submit drains it
        if (pending >= sq_entries)
        {
            return std::unexpected(ErrorKind::IOError);
        }

        // path is native wide (wchar_t) on Windows, so CreateFileW takes file.c_str() directly
        HANDLE fd = ::CreateFileW(file.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
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

        auto fileRef = IoRingHandleRefFromHandle(fd);
        auto bufRef = IoRingBufferRefFromPointer(buffer[tag].buf.data());

        // queue in submission queue
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

        pending++;
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
    unsigned sq_entries = 0; // submission queue capacity, from init
};
