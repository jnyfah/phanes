module;

#include "xxhash.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


module analyzer;

import phanes_deque;

struct HashError
{
    std::string reason;
};
using Hash = std::uint64_t;

// reads only the first 4KB
auto partial_hash_file(const std::filesystem::path& path) -> std::expected<Hash, HashError>
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    char buf[4096];
    file.read(buf, sizeof(buf));

    return XXH3_64bits(buf, static_cast<size_t>(file.gcount()));
}

// ifstream with 64KB buffer — explicit bulk reads beat mmap for cold-cache sequential scans
// mmap's lazy page-fault model loses when files are not in the page cache
auto hash_file(const std::filesystem::path& path, XXH3_state_t* state) -> std::expected<Hash, HashError>
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::unexpected(HashError{"cannot open: " + path.string()});

    XXH3_64bits_reset(state);

    char buf[65536];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
        XXH3_64bits_update(state, buf, static_cast<size_t>(file.gcount()));

    return XXH3_64bits_digest(state);
}

std::vector<DuplicateGroup> group_files_by_size(const DirectoryTree& tree)
{
    // readable files
    std::vector<FileId> ids;
    for (const auto file : tree.files)
    {
        if (file.readable && !file.is_symlink && file.size > 0)
        {
            ids.push_back(file.id);
        }
    }

    // sort
    std::ranges::sort(ids, {}, [&](FileId id) { return tree.files[id].size; });

    // group
    std::vector<DuplicateGroup> result;

    for (auto chunk :
         ids | std::views::chunk_by([&](FileId a, FileId b) { return tree.files[a].size == tree.files[b].size; }))
    {
        auto group = std::ranges::to<std::vector>(chunk);
        if (group.size() >= 2)
        {
            result.push_back({tree.files[group[0]].size, std::move(group)});
        }
    }

    return result;
}

std::vector<DuplicateGroup> compute_duplicate_groups(const DirectoryTree& tree)
{
    auto size_groups = group_files_by_size(tree);
    if (size_groups.empty())
    {
        return {};
    }

    // primary: largest first (load balance — big jobs distributed early)
    // secondary: path of first file (I/O locality — groups in same directory end up adjacent
    //            in the deque, so threads naturally read nearby files consecutively)
    std::ranges::sort(size_groups,
                      [&](const DuplicateGroup& a, const DuplicateGroup& b)
                      {
                          if (a.size != b.size)
                              return a.size > b.size;
                          return tree.files[a.files[0]].path < tree.files[b.files[0]].path;
                      });

    const std::size_t total = size_groups.size();
    const std::size_t num_threads = std::max(1u, std::thread::hardware_concurrency());

    LockFreeDeque<std::size_t> tasks;
    for (std::size_t i = 0; i < total; ++i)
    {
        tasks.push_back(i); // add tasks
    }

    // one result slot per thread
    std::vector<std::vector<DuplicateGroup>> per_thread(num_threads);

    auto worker = [&](std::size_t thread_id)
    {
        auto& local = per_thread[thread_id];

        // one state allocation per thread — reused across every file this thread hashes
        XXH3_state_t* state = XXH3_createState();

        while (!tasks.empty())
        {
            auto idx = tasks.steal_front();
            if (!idx)
            {
                continue; // CAS lost to another thread, retry
            }

            const auto& group = size_groups[*idx];

            // stage 1 — partial hash (first 4KB only)
            std::unordered_map<Hash, std::vector<FileId>> by_partial;
            for (FileId id : group.files)
            {
                auto hash = partial_hash_file(tree.files[id].path);
                if (hash)
                {
                    by_partial[*hash].push_back(id);
                }
            }

            // stage 2 — full hash only for files that survived stage 1
            std::unordered_map<Hash, std::vector<FileId>> by_full;
            for (auto& [partial, candidates] : by_partial)
            {
                if (candidates.size() < 2)
                {
                    continue; // unique partial hash — can't be a duplicate, skip
                }

                for (FileId id : candidates)
                {
                    auto hash = hash_file(tree.files[id].path, state);
                    if (hash)
                    {
                        by_full[*hash].push_back(id);
                    }
                }
            }

            for (auto& [hash, files] : by_full)
            {
                if (files.size() >= 2)
                {
                    local.push_back({group.size, std::move(files)});
                }
            }
        }
        XXH3_freeState(state);
    };

    {
        std::vector<std::jthread> threads;
        threads.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
        {
            threads.emplace_back(worker, i); // pass thread index so it knows its slot
        }
    }

    // merge all per-thread results — single thread, no contention
    std::vector<DuplicateGroup> result;
    for (auto& local : per_thread)
    {
        for (auto& g : local)
        {
            result.push_back(std::move(g));
        }
    }

    return result;
}
