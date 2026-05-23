module;

#include "xxhash.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

module analyzer;

struct HashError
{
    std::string reason;
};
using Hash = std::uint64_t;

auto hash_file(const std::filesystem::path& path) -> std::expected<Hash, HashError>
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::unexpected(HashError{"cannot open: " + path.string()});
    }

    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);

    char buf[65536];
    while (file.read(buf, sizeof(buf)) || file.gcount() > 0)
    {
        XXH3_64bits_update(state, buf, static_cast<size_t>(file.gcount()));
    }

    Hash result = XXH3_64bits_digest(state);
    XXH3_freeState(state);
    return result;
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

    // hash each file, re-group by hash value
    std::vector<DuplicateGroup> result;

    for (auto& group : size_groups)
    {
        std::unordered_map<Hash, std::vector<FileId>> by_hash;

        for (FileId id : group.files)
        {
            auto hash = hash_file(tree.files[id].path);
            if (hash)
            {
                by_hash[*hash].push_back(id);
            }
            // if hashing failed (permission lost, file removed) we skip silently ??
        }

        for (auto& [hash, files] : by_hash)
        {
            if (files.size() >= 2)
            {
                result.push_back({group.size, std::move(files)});
            }
        }
    }

    return result;
}
