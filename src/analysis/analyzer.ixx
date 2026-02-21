module;

#include <cstdint>
#include <optional>
#include <string>
#include <chrono>


export module analysis;

import core;


struct SummaryReport {
    std::size_t total_directories;
    std::size_t total_files;
    std::uintmax_t total_size;
    std::size_t total_symlinks;
    std::size_t total_errors;
    std::optional<FileId> largest_file;
    std::uintmax_t largest_file_size;
    std::size_t max_depth;
    std::chrono::seconds total_duration;
};

struct ExtensionStats {
    std::string extension;
    std::size_t count;
    std::uintmax_t total_size;
};

export SummaryReport compute_summary(const DirectoryTree&);

export std::vector<FileId> compute_largest_N_Files(const DirectoryTree& tree, std::size_t N);

export std::vector<DirectoryId> compute_largest_N_Directories(const DirectoryTree& tree, std::size_t N);

export std::vector<ExtensionStats> compute_extension_stats(const DirectoryTree&);

// calculate total size in terms of GB and KB
// set up CI/CD
// setup readme
// add comments
// Duplicate Size Detector
// Directory aggregated size
// remove root from largets N directories 



