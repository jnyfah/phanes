module;

#include <algorithm>
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
#include <string>
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
// change name sample hash
// use cpp array
// do full hash

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

struct Active
{
    FileId id;
    std::uint64_t size; // total file size
    std::uint64_t offset; // next chunk to read
    PhanesHashState state; // THIS file's running hash (one per active file!)
};

auto sample_hash_group(Uring& ring, const DuplicateGroup& group, PhanesHashState& state, const DirectoryTree& tree)
    -> std::unordered_map<Hash, std::vector<FileId>>
{
    constexpr std::uintmax_t SAMPLE = 4096;
    std::unordered_map<Hash, std::vector<FileId>> result;
#ifdef __unix__

    size_t count = 0;
    const auto& file_size = group.size;

    size_t expected = 1; // front always
    if (group.size > 2 * SAMPLE)
        expected++; // back
    if (group.size > 3 * SAMPLE)
        expected++; // mid

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

auto hash_file(Uring& ring,
               const std::unordered_map<Hash, std::vector<FileId>>& by_sample,
               PhanesHashState& state,
               const DirectoryTree& tree,
               int entries) -> std::unordered_map<Hash, std::vector<FileId>>
{

    std::unordered_map<Hash, std::vector<FileId>> result;

    // how many files are in the sqe
    int in_flight = 0;

    // for groups in the map
    constexpr std::uint64_t CHUNK = 4ull * 1024 * 1024;

    // 1. flatten all candidates (from buckets with >=2) into a worklist
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

    // 2. SEED: chunk-0 of the first W files, CHANGE ENTRIES
    while (in_flight < entries && next_file < worklist.size())
    {
        // initial submit for all files that can fit into W
        FileId id = worklist[next_file++];
        size_t a = active.size();
        active.push_back({id, tree.files[id].size, 0, {}});
        phanes_hash_reset(active[a].state);

        const char* path = tree.files[id].path.c_str();
        if (auto t = ring.submit(path, CHUNK, 0); t)
        {
            if (*t >= tag_to_active.size())
            {
                tag_to_active.resize(*t + 1);
            }
            tag_to_active[*t] = a;
            in_flight++;
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

        // read error: drop this chunk, recycle its slot
        if (r->res < 0)
        {
            ring.release(r->tag);
            continue;
        }

        phanes_hash_update(active[index].state,
                           reinterpret_cast<const std::uint8_t*>(ring.data(r->tag).data()),
                           static_cast<size_t>(r->res));
        active[index].offset += r->res;
        ring.release(r->tag); // this chunk's slot is finished — recycle the correct tag

        if (active[index].offset < active[index].size)
        {
            // submit next chunk
            const char* path = tree.files[active[index].id].path.c_str();
            if (auto t = ring.submit(path, CHUNK, active[index].offset); t)
            {
                // should we resize here ??
                if (*t >= tag_to_active.size())
                {
                    tag_to_active.resize(*t + 1);
                }
                tag_to_active[*t] = index;
                in_flight++;
            }
        }
        else
        {
            // file done
            Hash h = phanes_hash_digest(active[index].state);
            result[h].push_back(active[index].id);

            // reuse this finished slot for the next waiting file
            if (next_file < worklist.size())
            {
                FileId id = worklist[next_file++];
                active[index].id = id;
                active[index].size = tree.files[id].size;
                active[index].offset = 0;
                phanes_hash_reset(active[index].state);

                const char* path = tree.files[id].path.c_str();
                if (auto t = ring.submit(path, CHUNK, 0); t)
                {
                    if (*t >= tag_to_active.size())
                    {
                        tag_to_active.resize(*t + 1);
                    }
                    tag_to_active[*t] = index;
                    in_flight++;
                }
            }
        }
    }

    return result;
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

    auto tasks_owner = std::make_unique<LockFreeDeque<std::size_t>>(1024, n_threads);
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
        uring.init(it->files.size());
        PhanesHashState state;

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
            auto by_sample = sample_hash_group(uring, group, state, tree);

            // stage 2 — full hash only for survivors
            auto by_full = hash_file(uring, by_sample, state, tree, 10);

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
