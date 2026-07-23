module;

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <generator>
#include <memory>
#include <mutex>
#include <ranges>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __unix__
#include <fcntl.h>
#include <unistd.h>
#else
#include <fstream>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

module analyzer;

import phanes_deque;
import phanes_hasher;

import phanes_io;

using Hash = std::uint64_t;
using HashMap = std::unordered_map<Hash, std::vector<FileId>>;

// On Windows, OneDrive "Files On-Demand" files are placeholders: opening their
// content triggers a download (hydration). We do NOT hash those
static auto is_cloud_placeholder(const std::filesystem::path& path) -> bool
{
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    return (attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) !=
        0;
#else
    (void)path;
    return false;
#endif
}

struct Meta
{
    size_t file_idx;
    int region;
};

struct Acc
{
    std::array<std::size_t, 3> tag{};
    std::array<int, 3> res{};
    std::array<bool, 3> has{};
    int arrived = 0;
    int submitted = 0;
    std::size_t handle = 0;
};

struct Active
{
    FileId id;
    std::uint64_t size;
    std::uint64_t offset;
    std::size_t handle;
    PhanesHashState state;
};

auto prefilter_group(Ring& ring, const DuplicateGroup& group, PhanesHashState& state, const DirectoryTree& tree)
    -> HashMap
{
    constexpr std::uintmax_t SAMPLE = 4096;
    HashMap result;

    constexpr int WINDOW = 512;
    const auto file_size = group.size;

    unsigned expected = 1; // front always
    if (group.size > 2 * SAMPLE)
        expected++; // back
    if (group.size > 3 * SAMPLE)
        expected++; // mid

    std::vector<Meta> meta;
    std::vector<Acc> acc(group.files.size());

    size_t next_file = 0;
    int in_flight = 0; // reads submitted but not yet released (in flight + held for reassembly)

    // queue every applicable region of one file
    auto submit_file = [&](size_t fi)
    {
        const FileNode& f = tree.files[group.files[fi]];
        std::string_view leaf{tree.file_names.data() + f.name.offset, f.name.len};
        const std::filesystem::path path = tree.directories[f.parent].path / std::string(leaf);

        // open once; the up-to-three region reads share this handle
        auto fh = ring.open(path);
        if (!fh)
        {
            acc[fi].submitted = 0;
            return;
        }
        acc[fi].handle = *fh;

        int n = 0;
        auto add = [&](int64_t off, int r)
        {
            if (auto tag = ring.submit(*fh, SAMPLE, off); tag)
            {
                if (*tag >= meta.size())
                {
                    meta.resize(*tag + 1);
                }
                meta[*tag] = {fi, r};
                n++;
            }
        };
        add(0, 0); // front
        if (group.size > 3 * SAMPLE)
            add(static_cast<int64_t>(file_size / 2 - SAMPLE / 2), 1); // middle
        if (group.size > 2 * SAMPLE)
            add(static_cast<int64_t>(file_size) - static_cast<int64_t>(SAMPLE), 2); // back
        acc[fi].submitted = n;
        in_flight += n;

        if (n == 0)
        {
            ring.close_file(*fh);
        }
    };

    // submit all regions
    auto pump = [&]
    {
        while (next_file < group.files.size() && in_flight + static_cast<int>(expected) <= WINDOW)
        {
            submit_file(next_file++);
        }
    };

    pump();

    while (in_flight > 0)
    {
        auto res = ring.next();
        if (!res)
        {
            continue;
        }

        auto [fid, region] = meta[res->tag];
        auto& a = acc[fid];
        a.tag[region] = res->tag;
        a.res[region] = res->res;
        a.has[region] = true;
        a.arrived++;

        // a file settles once all of its submitted reads are back
        if (a.arrived == a.submitted)
        {
            if (a.submitted == static_cast<int>(expected)) // fully sampled -> hash in region order
            {
                phanes_hash_reset(state);
                for (size_t r = 0; r < a.has.size(); ++r)
                {
                    if (a.has[r] && a.res[r] > 0)
                    {
                        phanes_hash_update(state, ring.data(a.tag[r]).data(), static_cast<size_t>(a.res[r]));
                    }
                }
                result[phanes_hash_digest(state)].push_back(group.files[fid]);
            }

            // release this file's slots, close its handle, and pull in more work
            for (size_t r = 0; r < a.has.size(); ++r)
            {
                if (a.has[r])
                {
                    ring.release(a.tag[r]);
                    in_flight--;
                }
            }
            ring.close_file(a.handle);
            pump();
        }
    }

    ring.reset();
    return result;
}

auto hash_file(Ring& ring, const HashMap& by_sample, PhanesHashState& state, const DirectoryTree& tree) -> HashMap
{

    HashMap result;

    // chunk size per read, and how many chunks in flight per worker
    // peak full-hash memory ≈ n_threads * WINDOW * CHUNK
    constexpr std::uint64_t CHUNK = 4ULL * 1024 * 1024;
    constexpr int WINDOW = 16;

    int in_flight = 0;

    // flatten all candidates into a worklist
    std::vector<FileId> worklist;
    for (auto& [h, cands] : by_sample)
    {
        if (cands.size() >= 2)
        {
            for (FileId id : cands)
            {
                worklist.push_back(id);
            }
        }
    }

    std::vector<Active> active;
    std::vector<size_t> tag_to_active;
    size_t next_file = 0;

    // chunk-0 of the first WINDOW files
    while (in_flight < WINDOW && next_file < worklist.size())
    {
        // initial submit for all files that can fit into W
        FileId id = worklist[next_file++];

        const FileNode& f = tree.files[id];
        std::string_view leaf{tree.file_names.data() + f.name.offset, f.name.len};
        const std::filesystem::path path = tree.directories[f.parent].path / std::string(leaf);

        auto fh = ring.open(path);
        if (!fh)
        {
            continue; // can't open, skip this file
        }

        size_t index = active.size();
        active.push_back({id, tree.files[id].size, 0, *fh, {}});
        phanes_hash_reset(active[index].state);

        // clamp to file size: a small file must not claim a full CHUNK buffer
        const auto len = static_cast<size_t>(std::min<std::uint64_t>(CHUNK, active[index].size));
        if (auto t = ring.submit(*fh, len, 0); t)
        {
            if (*t >= tag_to_active.size())
            {
                tag_to_active.resize(*t + 1);
            }
            tag_to_active[*t] = index;
            in_flight++;
        }
        else
        {
            ring.close_file(*fh);
            active.pop_back(); // submit failed; undo the slot
        }
    }

    while (in_flight > 0)
    {

        auto r = ring.next();
        if (!r)
        {
            continue;
        }
        in_flight--;

        auto index = tag_to_active[r->tag];

        // read error: drop this chunk and close the file (only one chunk per file is in flight)
        if (r->res < 0)
        {
            ring.close_file(active[index].handle);
            ring.release(r->tag);
            continue;
        }

        phanes_hash_update(active[index].state, ring.data(r->tag).data(), static_cast<size_t>(r->res));
        active[index].offset += r->res;
        ring.release(r->tag);

        if (active[index].offset < active[index].size)
        {
            // submit next chunk against the same open handle, clamped to what remains
            const auto len =
                static_cast<size_t>(std::min<std::uint64_t>(CHUNK, active[index].size - active[index].offset));
            if (auto t = ring.submit(active[index].handle, len, active[index].offset); t)
            {
                if (*t >= tag_to_active.size())
                {
                    tag_to_active.resize(*t + 1);
                }
                tag_to_active[*t] = index;
                in_flight++;
            }
            else
            {
                ring.close_file(active[index].handle); // couldn't queue next chunk; abandon
            }
        }
        else
        {
            // file done
            Hash h = phanes_hash_digest(active[index].state);
            result[h].push_back(active[index].id);
            ring.close_file(active[index].handle);

            // reuse this finished slot for the next waiting file
            if (next_file < worklist.size())
            {
                FileId id = worklist[next_file++];

                const FileNode& f = tree.files[id];
                std::string_view leaf{tree.file_names.data() + f.name.offset, f.name.len};
                const std::filesystem::path path = tree.directories[f.parent].path / std::string(leaf);

                if (auto fh = ring.open(path); fh)
                {
                    active[index].id = id;
                    active[index].size = tree.files[id].size;
                    active[index].offset = 0;
                    active[index].handle = *fh;
                    phanes_hash_reset(active[index].state);

                    const auto len = static_cast<size_t>(std::min<std::uint64_t>(CHUNK, active[index].size));
                    if (auto t = ring.submit(*fh, len, 0); t)
                    {
                        if (*t >= tag_to_active.size())
                        {
                            tag_to_active.resize(*t + 1);
                        }
                        tag_to_active[*t] = index;
                        in_flight++;
                    }
                    else
                    {
                        ring.close_file(*fh);
                    }
                }
            }
        }
    }

    ring.reset();
    return result;
}

std::generator<DuplicateGroup> group_files_by_size(const DirectoryTree& tree)
{
    // readable files
    std::vector<FileId> ids;
    for (const auto& file : tree.files)
    {
        if (!file.readable || file.is_symlink || file.size == 0)
        {
            continue;
        }
        std::string_view leaf{tree.file_names.data() + file.name.offset, file.name.len};
        const std::filesystem::path path = tree.directories[file.parent].path / std::string(leaf);
        if (!is_cloud_placeholder(path))
        {
            ids.push_back(file.id);
        }
    }

    // sort by size
    std::ranges::sort(ids, {}, [&](FileId id) { return tree.files[id].size; });

    // group by file size
    for (auto chunk :
         ids | std::views::chunk_by([&](FileId a, FileId b) { return tree.files[a].size == tree.files[b].size; }))
    {
        auto group = std::ranges::to<std::vector>(chunk);
        if (group.size() >= 2)
        {
            co_yield DuplicateGroup{tree.files[group[0]].size, std::move(group)};
        }
    }
}

std::generator<DuplicateGroup> compute_duplicate_groups(const DirectoryTree& tree, std::size_t num_threads)
{
    // group each file in the tree by size
    auto size_groups = std::ranges::to<std::vector>(group_files_by_size(tree));
    if (size_groups.empty())
    {
        co_return;
    }

    const std::size_t total = size_groups.size();
    const std::size_t hw = std::max(1u, std::jthread::hardware_concurrency());
    const std::size_t n_threads = (num_threads == 0) ? hw : std::max(std::size_t{1}, num_threads);

    auto tasks_owner = std::make_unique<LockFreeDeque<std::size_t>>(1024, n_threads);
    auto& tasks = *tasks_owner;
    constexpr unsigned ring_entries = 1024;

    for (std::size_t i = 0; i < total; ++i)
    {
        tasks.push_back(i); // add tasks to deque
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<DuplicateGroup> ready;
    std::atomic<std::size_t> finished{0};

    auto worker = [&](std::size_t id)
    {
        Ring uring;
        const bool ok = uring.init(ring_entries).has_value();
        PhanesHashState state;

        while (ok && !tasks.empty())
        {
            auto idx = tasks.steal_front(id);
            if (!idx)
            {
                continue; // CAS lost to another thread, retry
            }

            const auto& group = size_groups[*idx];

            // files less than 4kb
            if (group.size <= 4096)
            {
                auto by_hash = prefilter_group(uring, group, state, tree);
                // this are genuine duplicates
                for (auto& [hash, files] : by_hash)
                {
                    if (files.size() >= 2)
                    {
                        {
                            std::lock_guard lock(mtx);
                            ready.push_back({group.size, std::move(files)});
                        }
                        cv.notify_one();
                    }
                }
                continue;
            }

            // files > 4kb
            // sample hash
            auto by_sample = prefilter_group(uring, group, state, tree);

            // full hash only for survivors
            auto by_full = hash_file(uring, by_sample, state, tree);

            for (auto& [hash, files] : by_full)
            {
                if (files.size() >= 2)
                {
                    {
                        std::lock_guard lock(mtx);
                        ready.push_back({group.size, std::move(files)});
                    }
                    cv.notify_one();
                }
            }
        }

        // last thread out signals the generator body to stop waiting
        if (++finished == n_threads)
        {
            cv.notify_all();
        }
    };

    {
        std::vector<std::jthread> threads;
        threads.reserve(n_threads);
        for (std::size_t i = 0; i < n_threads; ++i)
        {
            threads.emplace_back(worker, i);
        }

        // drain shared queue while workers run by yielding each confirmed group to the caller
        while (true)
        {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return !ready.empty() || finished == n_threads; });

            while (!ready.empty())
            {
                auto group = std::move(ready.front());
                ready.pop_front();
                lock.unlock();
                co_yield std::move(group); // caller prints this, workers keep running
                lock.lock();
            }

            if (finished == n_threads)
            {
                break;
            }
        }
    }
}
