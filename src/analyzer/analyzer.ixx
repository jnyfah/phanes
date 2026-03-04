module;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

export module analyzer;

import core;

export struct SummaryReport
{
    std::size_t total_directories;
    std::size_t total_files;
    std::uintmax_t total_size;
    std::size_t total_symlinks;
    std::size_t total_errors;
    std::optional<FileNode> largest_file;
    std::uintmax_t largest_file_size;
    std::size_t max_depth;
    std::chrono::seconds total_duration;
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
    DirectoryId max_files_dir;
    std::size_t max_files_count;
    double average_directory_depth;
    double average_files_per_directory;
};

export struct DirectoryMetrics
{
    std::vector<std::size_t> depth;
    std::vector<std::uintmax_t> recursive_size;
    std::vector<std::size_t> recursive_file_count;
};

export SummaryReport compute_summary(const DirectoryTree& tree);

export std::vector<FileId> compute_largest_N_Files(const DirectoryTree& tree, std::size_t N);

export std::vector<DirectoryId> compute_largest_N_Directories(const DirectoryTree& tree, std::size_t N);

export std::vector<ExtensionStats> compute_extension_stats(const DirectoryTree&);

export std::vector<FileId> compute_recent_files(const DirectoryTree& tree, std::chrono::seconds duration);

export std::vector<DirectoryId> compute_empty_directories(const DirectoryTree& tree);

export std::vector<FileId> compute_symlinks(const DirectoryTree& tree);

export DirectoryStats compute_directory_stats(const DirectoryTree& tree);

export std::vector<std::size_t> compute_directory_depths(const DirectoryTree& tree, const DirectoryMetrics& metrics);

export DirectoryMetrics compute_directory_metrics(const DirectoryTree& tree);

// setup readme
// add comments
// Duplicate Size Detector
// Directory aggregated size
// unit tests
// compute_directory_metrics should be called once! atthe start and reused !
// - cmake preset blog

// doxgye documentaytion

// fix includes
// do you need to use namespaces ?
// fix sonarcube
// if no flang print help