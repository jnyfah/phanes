module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <ranges>
#include <unordered_map>
#include <vector>

module analyzer;

SummaryReport compute_summary(const DirectoryTree& tree)
{

    SummaryReport sum{};

    sum.total_directories = tree.directories.size();
    sum.total_errors = tree.errors.size();
    sum.total_files = tree.files.size();

    sum.total_size = 0;
    std::size_t symlink = 0;
    std::uintmax_t current_max = 0;
    std::optional<FileNode> largest_file = std::nullopt;

    for (const auto& file : tree.files)
    {
        sum.total_size += file.size;
        if (file.is_symlink)
        {
            symlink++;
        }
        if (file.size > current_max)
        {
            current_max = file.size;
            largest_file = file;
        }
    }

    sum.total_symlinks = symlink;
    sum.largest_file_size = current_max;
    sum.largest_file = largest_file;
    sum.total_duration = tree.scan_finished - tree.scan_started;

    return sum;
}

std::vector<FileId> compute_largest_N_Files(const DirectoryTree& tree, std::size_t N)
{
    if (tree.files.empty())
    {
        return {};
    }

    std::vector<FileId> fileId;
    N = std::min(N, tree.files.size());

    fileId.resize(tree.files.size());
    std::ranges::iota(fileId.begin(), fileId.end(), 0);

    std::ranges::partial_sort(fileId.begin(),
                              fileId.begin() + N,
                              fileId.end(),
                              [&tree](const FileId& a, const FileId& b)
                              { return tree.files[a].size > tree.files[b].size; });

    fileId.resize(N);

    return fileId;
}

std::vector<ExtensionStats> compute_extension_stats(const DirectoryTree& tree)
{
    if (tree.files.empty())
    {
        return {};
    }

    std::unordered_map<std::string, std::pair<std::size_t, std::uintmax_t>> map;

    for (const auto& file : tree.files)
    {
        auto ext = file.path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto& [first, second] = map[ext];
        ++first;
        second += file.size;
    }

    std::vector<ExtensionStats> stats;
    stats.reserve(map.size());

    for (const auto& [ext, values] : map)
    {
        const auto& [count, total] = values;

        stats.push_back(ExtensionStats{ext, count, total});
    }

    std::ranges::sort(stats,
                      [](const ExtensionStats& a, const ExtensionStats& b) { return a.total_size > b.total_size; });

    return stats;
}

std::vector<DirectoryId> compute_largest_N_Directories(const DirectoryTree& tree, std::size_t N)
{
    if (tree.directories.empty())
    {
        return {};
    }

    N = std::min(N, tree.directories.size());

    std::vector<DirectoryId> dirIds(tree.directories.size());
    std::ranges::iota(dirIds, 0);

    std::vector<std::uintmax_t> dir_sizes(tree.directories.size(), 0);

    for (DirectoryId id = tree.directories.size(); id-- > 0;)
    {
        const auto& dir = tree.directories[id];

        std::uintmax_t size = 0;

        for (FileId fid : dir.files)
        {
            size += tree.files[fid].size;
        }

        for (DirectoryId Did : dir.subdirs)
        {
            size += dir_sizes[Did];
            ;
        }

        dir_sizes[id] = size;
    }

    std::ranges::partial_sort(dirIds.begin(),
                              dirIds.begin() + N,
                              dirIds.end(),
                              [&](DirectoryId a, DirectoryId b) { return dir_sizes[a] > dir_sizes[b]; });

    dirIds.resize(N);
    return dirIds;
}

std::vector<FileId> compute_recent_files(const DirectoryTree& tree, std::chrono::seconds duration)
{
    if (tree.files.empty())
    {
        return {};
    }

    std::vector<FileId> fileid;
    fileid.reserve(tree.files.size());

    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto cutoff = now - duration;
    for (const auto& file : tree.files)
    {
        if (file.modified >= cutoff)
        {
            fileid.push_back(file.id);
        }
    }

    std::ranges::sort(fileid, [&](FileId a, FileId b) { return tree.files[a].modified > tree.files[b].modified; });
    return fileid;
}

std::vector<DirectoryId> compute_empty_directories(const DirectoryTree& tree)
{
    if (tree.directories.empty())
    {
        return {};
    }

    std::vector<DirectoryId> dirid;

    for (DirectoryId id = 0; id < tree.directories.size(); ++id)
    {
        const auto& dir = tree.directories[id];

        if (dir.files.empty())
        {
            dirid.push_back(id);
        }

        for (DirectoryId Did : dir.subdirs)
        {
        }
    }

    return dirid;
}

std::vector<FileId> compute_symlinks(const DirectoryTree& tree)
{
    std::vector<FileId> symid;

    for (const auto& file : tree.files)
    {
        if (file.is_symlink)
        {
            symid.push_back(file.id);
        }
    }
    return symid;
}

// TODO
std::vector<std::size_t> compute_directory_depths(const DirectoryTree& tree, const DirectoryMetrics& metrics)
{
    std::vector<std::size_t> depths;

    return depths;
}

// TODO
DirectoryMetrics compute_directory_metrics(const DirectoryTree& tree)
{
    DirectoryMetrics metrics{};
    metrics.depth.resize(tree.directories.size());
    metrics.recursive_size.resize(tree.directories.size());
    metrics.recursive_file_count.resize(tree.directories.size());

    for (const auto& dir : tree.directories)
    {
    }

    return metrics;
}

// TODO
DirectoryStats compute_directory_stats(const DirectoryTree& tree)
{

    DirectoryStats stats{};

    return stats;
}