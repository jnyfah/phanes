module;

#include <optional>
#include <ostream>
#include <vector>

export module executor;

import analyzer;
import core;
import parser;
import view;

export struct Executor
{
    const DirectoryTree& tree;
    std::ostream& os;

    mutable std::optional<DirectoryMetrics> metrics;
    mutable std::optional<std::vector<DirectoryId>> empty_dirs;

    const DirectoryMetrics& get_metrics() const
    {
        if (!metrics)
        {
            metrics = compute_directory_metrics(tree);
        }

        return *metrics;
    }

    const std::vector<DirectoryId>& get_empty_dir() const
    {
        if (!empty_dirs)
        {
            empty_dirs = compute_empty_directories(tree);
        }
        return *empty_dirs;
    }

    void operator()(SummaryAction) const
    {
        print_summary(os, compute_summary(tree, get_metrics(), get_empty_dir().size()));
    }

    void operator()(ExtensionsAction) const { print_extension_stats(os, compute_extension_stats(tree)); }

    void operator()(EmptyDirsAction) const { print_empty_directories(os, compute_empty_directories(tree), tree); }

    void operator()(SymlinksAction) const { print_symlinks(os, compute_symlinks(tree), tree); }

    void operator()(LargestFilesAction op) const { print_largest_files(os, compute_largest_N_Files(tree, op.n), tree); }

    void operator()(LargestDirsAction op) const
    {
        print_largest_directories(os, compute_largest_N_Directories(tree, get_metrics(), op.n), tree);
    }

    void operator()(RecentAction op) const
    {
        print_recent_files(os, compute_recent_files(tree, op.duration), op.duration, tree);
    }

    void operator()(ErrorsAction) const { print_errors(os, get_errors(tree)); }

    void operator()(StatsAction) const
    {
        print_directory_stats(os, compute_directory_stats(tree, get_metrics()), tree);
    }

    void operator()(MetricsAction) const { print_directory_metrics(os, compute_directory_metrics(tree), tree); }
};