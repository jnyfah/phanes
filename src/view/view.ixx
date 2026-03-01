module;

#include <chrono>
#include <ostream>

export module view;

import analyzer;
import core;

export auto format_size(std::uint64_t bytes) -> std::string;
export auto format_duration(std::chrono::seconds total) -> std::string;
export void print_summary(std::ostream& os, const SummaryReport& report);
export void print_largest_files(std::ostream& os, const std::vector<FileId>& files, const DirectoryTree& tree);
export void
print_largest_directories(std::ostream& os, const std::vector<DirectoryId>& directories, const DirectoryTree& tree);
export void
print_empty_directories(std::ostream& os, const std::vector<DirectoryId>& directories, const DirectoryTree& tree);
export void print_symlinks(std::ostream& os, const std::vector<FileId>& files, const DirectoryTree& tree);

export void print_recent_files(std::ostream& os,
                               const std::vector<FileId>& files,
                               const std::chrono::seconds& duration,
                               const DirectoryTree& tree);

// Todo
export void print_extension_stats(std::ostream& os, std::vector<ExtensionStats> stats);
export void print_directory_stats(std::ostream& os, const DirectoryStats& stats);
export void print_directory_depths(std::ostream& os, const std::vector<std::size_t>& size);
export void print_directory_metrics(std::ostream& os, const DirectoryMetrics& metrics);
