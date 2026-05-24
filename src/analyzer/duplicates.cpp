module;

#include "xxhash.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
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

module analyzer;

import phanes_deque;

struct HashError
{
    std::string reason;
};
using Hash = std::uint64_t;

// Fast hash, samples only 12kb
auto sample_hash_file(const std::filesystem::path& path,
                      std::uintmax_t file_size,
                      XXH3_state_t* state) -> std::expected<Hash, HashError>
{
    constexpr std::uintmax_t SAMPLE = 4096;

#ifdef __unix__
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    XXH3_64bits_reset(state);

    char buf[SAMPLE];

    // front
    ssize_t n = ::pread(fd, buf, SAMPLE, 0);
    if (n > 0)
    {
        XXH3_64bits_update(state, buf, static_cast<size_t>(n));
    }

    // middle
    if (file_size > 3 * SAMPLE)
    {
        n = ::pread(fd, buf, SAMPLE, static_cast<off_t>(file_size / 2 - SAMPLE / 2));
        if (n > 0)
        {
            XXH3_64bits_update(state, buf, static_cast<size_t>(n));
        }
    }

    // back
    if (file_size > SAMPLE)
    {
        n = ::pread(fd, buf, SAMPLE, static_cast<off_t>(file_size) - static_cast<off_t>(SAMPLE));
        if (n > 0)
        {
            XXH3_64bits_update(state, buf, static_cast<size_t>(n));
        }
    }

    ::close(fd);
    return XXH3_64bits_digest(state);

#else
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    XXH3_64bits_reset(state);

    char buf[SAMPLE];

    file.read(buf, SAMPLE);
    if (file.gcount() > 0)
    {
        XXH3_64bits_update(state, buf, static_cast<size_t>(file.gcount()));
    }

    if (file_size > 3 * SAMPLE)
    {
        file.seekg(static_cast<std::streamoff>(file_size / 2 - SAMPLE / 2));
        file.read(buf, SAMPLE);
        if (file.gcount() > 0)
        {
            XXH3_64bits_update(state, buf, static_cast<size_t>(file.gcount()));
        }
    }

    if (file_size > SAMPLE)
    {
        file.seekg(static_cast<std::streamoff>(file_size) - static_cast<std::streamoff>(SAMPLE));
        file.read(buf, SAMPLE);
        if (file.gcount() > 0)
        {
            XXH3_64bits_update(state, buf, static_cast<size_t>(file.gcount()));
        }
    }

    return XXH3_64bits_digest(state);
#endif
}

// Full file hash
auto hash_file(const std::filesystem::path& path, XXH3_state_t* state) -> std::expected<Hash, HashError>
{
#ifdef __unix__
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    XXH3_64bits_reset(state);

    char buf[262144];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
    {
        XXH3_64bits_update(state, buf, static_cast<size_t>(n));
    }

    ::close(fd);

    if (n < 0)
    {
        return std::unexpected(HashError{"read error: " + path.string()});
    }

    return XXH3_64bits_digest(state);
#else
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    XXH3_64bits_reset(state);

    char buf[262144];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
    {
        XXH3_64bits_update(state, buf, static_cast<size_t>(file.gcount()));
    }

    return XXH3_64bits_digest(state);
#endif
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

std::vector<DuplicateGroup> compute_duplicate_groups(const DirectoryTree& tree, std::size_t num_threads)
{
    auto size_groups = group_files_by_size(tree);
    if (size_groups.empty())
    {
        return {};
    }

    std::ranges::sort(size_groups,
                      [&](const DuplicateGroup& a, const DuplicateGroup& b)
                      {
                          if (a.size != b.size)
                              return a.size > b.size;
                          return tree.files[a.files[0]].path < tree.files[b.files[0]].path;
                      });

    const std::size_t total = size_groups.size();
    const std::size_t hw = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t n_threads = (num_threads == 0) ? hw * 2 : std::max(std::size_t{1}, num_threads);

    LockFreeDeque<std::size_t> tasks;
    for (std::size_t i = 0; i < total; ++i)
    {
        tasks.push_back(i); // add tasks
    }

    // one result slot per thread
    std::vector<std::vector<DuplicateGroup>> per_thread(n_threads);

    auto worker = [&](std::size_t thread_id)
    {
        auto& local = per_thread[thread_id];

        // one state allocation per thread — reused across every hash call this thread makes
        XXH3_state_t* state = XXH3_createState();

        while (!tasks.empty())
        {
            auto idx = tasks.steal_front();
            if (!idx)
            {
                continue; // CAS lost to another thread, retry
            }

            const auto& group = size_groups[*idx];

            // files less than 4kb
            if (group.size <= 4096)
            {
                std::unordered_map<Hash, std::vector<FileId>> by_hash;
                for (FileId id : group.files)
                {
                    auto hash = sample_hash_file(tree.files[id].path, tree.files[id].size, state);
                    if (hash)
                    {
                        by_hash[*hash].push_back(id);
                    }
                }
                // this are genuine duplicates
                for (auto& [hash, files] : by_hash)
                {
                    if (files.size() >= 2)
                    {
                        local.push_back({group.size, std::move(files)});
                    }
                }
                continue;
            }

            // stage 1 -- simple hash
            std::unordered_map<Hash, std::vector<FileId>> by_sample;
            for (const auto id : group.files)
            {
                auto hash = sample_hash_file(tree.files[id].path, tree.files[id].size, state);
                if (hash)
                {
                    by_sample[*hash].push_back(id);
                }
            }

            // groups that survived simple hash -- run full hash
            std::unordered_map<Hash, std::vector<FileId>> by_full;
            for (auto& [sample, candidates] : by_sample)
            {
                if (candidates.size() < 2)
                {
                    continue; // unique sample hash — can't be a duplicate
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
        threads.reserve(n_threads);
        for (std::size_t i = 0; i < n_threads; ++i)
        {
            threads.emplace_back(worker, i); // pass thread index so it knows its slot
        }
    }

    // merge all per-thread results
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
