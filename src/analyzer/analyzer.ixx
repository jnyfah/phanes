module;

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

export module analyzer;

import core;

export struct SummaryReport
{
    std::size_t total_directories;
    std::size_t total_files;
    std::uintmax_t total_size;
    std::size_t total_symlinks;
    std::size_t total_empty_dir;
    std::size_t total_errors;
    std::optional<FileId> largest_file;
    std::uintmax_t largest_file_size;
    std::size_t max_depth;
    std::chrono::seconds total_duration;
    std::string max_depth_dir;
};

export struct ExtensionStats
{
    std::string extension;
    std::size_t count;
    std::uintmax_t total_size;
};

export struct DirectoryStats
{
    std::size_t max_depth;
    DirectoryId max_depth_dir;

    std::size_t max_files_count;
    DirectoryId max_files_count_dir;

    std::size_t max_files_size;
    DirectoryId max_files_size_dir;

    double average_directory_depth;
    double average_files_per_directory;
};

export struct DirectoryMetrics
{
    std::vector<std::size_t> depth;
    std::vector<std::uintmax_t> recursive_size;
    std::vector<std::size_t> recursive_file_count;
};

// pre-computed per-file aggregatesto be shared across multiple analyzer operations
export struct FileStats
{
    std::uintmax_t total_size{0};
    std::size_t symlink_count{0};
    std::uintmax_t largest_file_size{0};
    std::optional<FileId> largest_file_id;
    std::vector<FileId> symlink_ids;
};

export FileStats compute_file_stats(const DirectoryTree& tree);

export SummaryReport
compute_summary(const DirectoryTree& tree, const DirectoryMetrics& metrics, size_t empty_dir, const FileStats& fs);

export std::vector<FileId> compute_largest_N_Files(const DirectoryTree& tree, std::size_t N);

export std::vector<DirectoryId>
compute_largest_N_Directories(const DirectoryTree& tree, const DirectoryMetrics& metrics, std::size_t N);

export std::vector<ExtensionStats> compute_extension_stats(const DirectoryTree&);

export std::vector<FileId> compute_recent_files(const DirectoryTree& tree, std::chrono::seconds duration);

export std::vector<DirectoryId> compute_empty_directories(const DirectoryTree& tree);

export DirectoryStats compute_directory_stats(const DirectoryTree& tree, const DirectoryMetrics& metrics);

export DirectoryMetrics compute_directory_metrics(const DirectoryTree& tree);

export const std::deque<ErrorRecord>& get_errors(const DirectoryTree& tree);
