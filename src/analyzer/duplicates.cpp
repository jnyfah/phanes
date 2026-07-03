module;

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <generator>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstddef>

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
import phanes_uring;

struct HashError
{
    std::string reason;
};
using Hash = std::uint64_t;

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

// Todo
// change name smaple hash 
// use cpp array

struct Meta
{
    size_t file_idx;
    int region;
};

struct Acc
{
    size_t tag[3]; // tag of each region:  0=front, 1=mid, 2=back
    int res[3]; // bytes read per region
    bool has[3]{}; // did this region arrive?
    int arrived = 0; // how many of this file's regions are in
};

auto sample_hash_group(Uring& ring, const DuplicateGroup& group, PhanesHashState& state, const DirectoryTree& tree)
    ->std::unordered_map<Hash, std::vector<FileId>>
{
    constexpr std::uintmax_t SAMPLE = 4096;
    std::unordered_map<Hash, std::vector<FileId>> result;
#ifdef __unix__

    size_t count = 0;
    const auto& file_size = group.size;

    size_t expected = 1;                     // front always
    if (group.size > 2 * SAMPLE) expected++; // back
    if (group.size > 3 * SAMPLE) expected++; // mid

    std::vector<Meta> meta;
    std::vector<Acc> acc(group.files.size());

    for (size_t i = 0; i < group.files.size(); i++)
    {
        const auto path = tree.files[group.files[i]].path.c_str();
        if (auto tag = ring.submit(path, SAMPLE, 0); tag)
        {
            meta.resize(*tag + 1);
            meta[*tag] = {i, 0};
            count++;
        }

        if (group.size > 3 * SAMPLE)
        {
            if (auto tag = ring.submit(path, SAMPLE, static_cast<off_t>(file_size / 2 - SAMPLE / 2)); tag)
            {
                meta.resize(*tag + 1);
                meta[*tag] = {i, 1};
                count++;
            }
        }

        if (group.size > 2 * SAMPLE)
        {
            if (auto tag = ring.submit(path, SAMPLE, static_cast<off_t>(file_size) - static_cast<off_t>(SAMPLE)); tag)
            {
                meta.resize(*tag + 1);
                meta[*tag] = {i, 2};
                count++;
            }
        }
    }

    while (count > 0)
    {
        auto res = ring.next();
        if (!res)
        {
            continue;
        }

        auto [fid, region] = meta[res->tag];
        acc[fid].tag[region] = res->tag;
        acc[fid].has[region] = true;
        acc[fid].res[region] = res->res;
        acc[fid].arrived++;
        count--;

        if (acc[fid].arrived == expected)
        {
            phanes_hash_reset(state);

            for (int i = 0; i < 3; i++)
            {
                if (acc[fid].has[i] && acc[fid].res[i] > 0)
                {
                    phanes_hash_update(state,
                                       reinterpret_cast<const std::uint8_t*>(ring.data(acc[fid].tag[i]).data()),
                                       static_cast<size_t>(acc[fid].res[i]));
                }
            }

            Hash h = phanes_hash_digest(state);
            result[h].push_back(group.files[fid]);

            for (int i = 0; i < 3; i++)
            {
                if (acc[fid].has[i])
                {
                    ring.release(acc[fid].tag[i]);
                }
            }
        }
    }

    ring.reset();
    return result;

#endif
}

// Full file hash
auto hash_file(const std::filesystem::path& path, PhanesHashState& state) -> std::expected<Hash, HashError>
{
#ifdef __unix__
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    phanes_hash_reset(state);

    char buf[262144];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
    {
        phanes_hash_update(state, reinterpret_cast<const std::uint8_t*>(buf), static_cast<size_t>(n));
    }

    ::close(fd);

    if (n < 0)
    {
        return std::unexpected(HashError{"read error: " + path.string()});
    }

    return phanes_hash_digest(state);
#else
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    phanes_hash_reset(state);

    char buf[262144];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
    {
        phanes_hash_update(state, reinterpret_cast<const std::uint8_t*>(buf), static_cast<size_t>(file.gcount()));
    }

    return phanes_hash_digest(state);
#endif
}

std::generator<DuplicateGroup> group_files_by_size(const DirectoryTree& tree)
{
    // readable files
    std::vector<FileId> ids;
    for (const auto file : tree.files)
    {
        if (file.readable && !file.is_symlink && file.size > 0 && !is_cloud_placeholder(file.path))
        {
            ids.push_back(file.id);
        }
    }

    // sort
    std::ranges::sort(ids, {}, [&](FileId id) { return tree.files[id].size; });

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

    // sort the group
    std::ranges::sort(size_groups,
                      [&](const DuplicateGroup& a, const DuplicateGroup& b)
                      {
                          if (a.size != b.size)
                          {
                              return a.size > b.size;
                          }
                          return tree.files[a.files[0]].path < tree.files[b.files[0]].path;
                      });

    const std::size_t total = size_groups.size();
    const std::size_t n_threads = std::jthread::hardware_concurrency();

    auto tasks_owner = std::make_unique<LockFreeDeque<std::size_t>>(n_threads);
    auto& tasks = *tasks_owner;
    // get the largest group
    auto it = std::max_element(size_groups.begin(),
                               size_groups.end(),
                               [](const DuplicateGroup& a, const DuplicateGroup& b)
                               { return (a.files.size() < b.files.size()); });

    for (std::size_t i = 0; i < total; ++i)
    {
        tasks.push_back(i); // add tasks
    }

    std::mutex mtx;
    std::condition_variable cv;
    std::deque<DuplicateGroup> ready;
    std::atomic<std::size_t> finished{0};

    auto worker = [&](std::size_t id)
    {
        Uring uring;
        // to do get the max files in id instead as entries
        uring.init(it->files.size());
        PhanesHashState state; // one per thread, reused across every file (reset per hash)

        while (!tasks.empty())
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
                auto by_hash = sample_hash_group(uring, group, state, tree);
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

            // stage 1 — sample hash
            // std::unordered_map<Hash, std::vector<FileId>> by_sample;
            // for (FileId id : group.files)
            // {
            //     auto hash = sample_hash_file(&uring, tree.files[id].path, tree.files[id].size, state, tree);
            //     if (hash)
            //     {
            //         by_sample[*hash].push_back(id);
            //     }
            // }

            // stage 2 — full hash only for survivors
            // std::unordered_map<Hash, std::vector<FileId>> by_full;
            // for (auto& [sample, candidates] : by_sample)
            // {
            //     if (candidates.size() < 2)
            //     {
            //         continue; //  can't be a duplicate
            //     }

            //     for (FileId id : candidates)
            //     {
            //         auto hash = hash_file(tree.files[id].path, state);
            //         if (hash)
            //         {
            //             by_full[*hash].push_back(id);
            //         }
            //     }
            // }

            // for (auto& [hash, files] : by_full)
            // {
            //     if (files.size() >= 2)
            //     {
            //         {
            //             std::lock_guard lock(mtx);
            //             ready.push_back({group.size, std::move(files)});
            //         }
            //         cv.notify_one();
            //     }
            // }
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
