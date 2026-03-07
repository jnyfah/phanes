module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <ranges>
#include <stack>
#include <unordered_map>
#include <vector>

module analyzer;

struct string_hash
{
    using is_transparent = void;
    std::size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
};

SummaryReport compute_summary(const DirectoryTree& tree, const DirectoryMetrics& metrics, const size_t empty_dir)
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

    auto max_depth_itr = std::ranges::max_element(metrics.depth);
    sum.max_depth = *max_depth_itr;
    auto max_depth_dir = static_cast<DirectoryId>(std::distance(metrics.depth.begin(), max_depth_itr));
    sum.max_depth_dir = tree.directories[max_depth_dir].path.filename().string();
    sum.total_empty_dir = empty_dir;

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

    std::ranges::partial_sort(fileId,
                              fileId.begin() + N,
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

    std::unordered_map<std::string, std::pair<std::size_t, std::uintmax_t>, string_hash, std::equal_to<>> map;

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

std::vector<DirectoryId>
compute_largest_N_Directories(const DirectoryTree& tree, const DirectoryMetrics& metrics, std::size_t N)
{
    if (tree.directories.size() <= 1)
    {
        return {};
    }

    std::vector<DirectoryId> dirIds(tree.directories.size() - 1);
    std::ranges::iota(dirIds, 1);

    N = std::min(N, dirIds.size());

    std::ranges::partial_sort(dirIds,
                              dirIds.begin() + N,
                              [&](DirectoryId a, DirectoryId b)
                              { return metrics.recursive_size[a] > metrics.recursive_size[b]; });
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

        if ((dir.files.empty()) && dir.subdirs.empty())
        {
            dirid.push_back(id);
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

const std::vector<ErrorRecord>& get_errors(const DirectoryTree& tree)
{
    return tree.errors;
}

DirectoryMetrics compute_directory_metrics(const DirectoryTree& tree)
{
    const auto N = tree.directories.size();

    DirectoryMetrics metrics;
    metrics.depth.resize(N, 0);
    metrics.recursive_size.resize(N, 0);
    metrics.recursive_file_count.resize(N, 0);

    if (!tree.root)
    {
        return metrics;
    }

    for (size_t id = 0; id < N; ++id)
    {
        const auto& dir = tree.directories[id];

        // depth
        if (dir.parent)
        {
            metrics.depth[id] = metrics.depth[*dir.parent] + 1;
        }

        // direct file metrics
        metrics.recursive_file_count[id] = dir.files.size();

        for (auto fid : dir.files)
        {
            metrics.recursive_size[id] += tree.files[fid].size;
        }
    }

    for (auto id = N; id-- > 0;)
    {
        const auto& dir = tree.directories[id];

        if (!dir.parent)
        {
            continue;
        }

        auto parent = *dir.parent;

        metrics.recursive_size[parent] += metrics.recursive_size[id];
        metrics.recursive_file_count[parent] += metrics.recursive_file_count[id];
    }

    return metrics;
}

DirectoryStats compute_directory_stats(const DirectoryTree& tree, const DirectoryMetrics& metrics)
{

    DirectoryStats stats{};

    if (metrics.depth.empty())
    {
        return stats;
    }

    const DirectoryId root = *tree.root;
    const auto n = tree.directories.size();

    auto max_depth_itr = std::ranges::max_element(metrics.depth);
    stats.max_depth = *max_depth_itr;
    stats.max_depth_dir = static_cast<DirectoryId>(std::distance(metrics.depth.begin(), max_depth_itr));

    std::vector<DirectoryId> non_root;
    non_root.reserve(n - 1);
    for (DirectoryId id = 0; id < n; ++id)
    {
        if (id != root)
        {
            non_root.push_back(id);
        }
    }

    auto max_file_count_itr =
        std::ranges::max_element(non_root,
                                 [&](DirectoryId a, DirectoryId b)
                                 { return metrics.recursive_file_count[a] < metrics.recursive_file_count[b]; });
    stats.max_files_count = metrics.recursive_file_count[*max_file_count_itr];
    stats.max_files_count_dir = *max_file_count_itr;

    auto max_file_size_itr = std::ranges::max_element(
        non_root,
        [&](DirectoryId a, DirectoryId b) { return metrics.recursive_size[a] < metrics.recursive_size[b]; });
    stats.max_files_size = metrics.recursive_size[*max_file_size_itr];
    stats.max_files_size_dir = *max_file_size_itr;

    auto depth_sum = std::ranges::fold_left(non_root,
                                            std::size_t{0},
                                            [&](std::size_t acc, DirectoryId id) { return acc + metrics.depth[id]; });
    auto file_sum =
        std::ranges::fold_left(non_root,
                               std::size_t{0},
                               [&](std::size_t acc, DirectoryId id) { return acc + metrics.recursive_file_count[id]; });

    stats.average_directory_depth = static_cast<double>(depth_sum) / non_root.size();
    stats.average_files_per_directory = static_cast<double>(file_sum) / non_root.size();

    return stats;
}
