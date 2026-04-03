module;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <deque>
#include <unordered_map>
#include <numeric>
#include <ranges>
#include <vector>

module analyzer;

FileStats compute_file_stats(const DirectoryTree& tree)
{
    FileStats fs;
    for (const auto& file : tree.files)
    {
        fs.total_size += file.size;
        if (file.is_symlink)
        {
            ++fs.symlink_count;
            fs.symlink_ids.push_back(file.id);
        }
        if (file.size > fs.largest_file_size)
        {
            fs.largest_file_size = file.size;
            fs.largest_file_id = file.id;
        }
    }
    return fs;
}

SummaryReport
compute_summary(const DirectoryTree& tree, const DirectoryMetrics& metrics, size_t empty_dir, const FileStats& fs)
{
    SummaryReport sum{};

    sum.total_directories = tree.directories.size();
    sum.total_errors = tree.errors.size();
    sum.total_files = tree.files.size();
    sum.total_size = fs.total_size;
    sum.total_symlinks = fs.symlink_count;
    sum.largest_file_size = fs.largest_file_size;
    sum.largest_file = fs.largest_file_id;
    sum.total_empty_dir = empty_dir;
    sum.total_duration = tree.scan_finished - tree.scan_started;

    auto max_depth_itr = std::ranges::max_element(metrics.depth);
    sum.max_depth = *max_depth_itr;
    auto max_depth_dir = static_cast<DirectoryId>(std::distance(metrics.depth.begin(), max_depth_itr));
    sum.max_depth_dir = tree.directories[max_depth_dir].path.filename().string();

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

    std::unordered_map<std::string, std::pair<std::size_t, std::uintmax_t>> map;

    for (const auto& file : tree.files)
    {
        auto ext = file.path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        auto& [count, size] = map[ext];
        ++count;
        size += file.size;
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

        if (dir.files.empty() && dir.subdirs.empty())
        {
            dirid.push_back(id);
        }
    }

    return dirid;
}

const std::deque<ErrorRecord>& get_errors(const DirectoryTree& tree)
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

        if (dir.parent)
        {
            metrics.depth[id] = metrics.depth[*dir.parent] + 1;
        }

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

    auto non_root =
        std::views::iota(DirectoryId{0}, n) | std::views::filter([root](DirectoryId id) { return id != root; });

    std::size_t depth_sum = 0;
    std::size_t file_sum = 0;
    std::size_t non_root_count = 0;

    for (DirectoryId id : non_root)
    {
        ++non_root_count;
        depth_sum += metrics.depth[id];
        file_sum += metrics.recursive_file_count[id];

        if (metrics.recursive_file_count[id] > stats.max_files_count)
        {
            stats.max_files_count = metrics.recursive_file_count[id];
            stats.max_files_count_dir = id;
        }
        if (metrics.recursive_size[id] > stats.max_files_size)
        {
            stats.max_files_size = metrics.recursive_size[id];
            stats.max_files_size_dir = id;
        }
    }

    if (non_root_count > 0)
    {
        stats.average_directory_depth = static_cast<double>(depth_sum) / non_root_count;
        stats.average_files_per_directory = static_cast<double>(file_sum) / non_root_count;
    }

    return stats;
}
