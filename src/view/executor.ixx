module;

#include <future>
#include <optional>
#include <ostream>
#include <sstream>
#include <variant>
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
    mutable std::optional<FileStats> file_stats;

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

    const FileStats& get_file_stats() const
    {
        if (!file_stats)
        {
            file_stats = compute_file_stats(tree);
        }
        return *file_stats;
    }

    // Caches
    void prewarm() const
    {
        get_metrics();
        get_empty_dir();
        get_file_stats();
    }

    std::string operator()(SummaryAction) const
    {
        std::ostringstream out;
        print_summary(out, compute_summary(tree, get_metrics(), get_empty_dir().size(), get_file_stats()), tree);
        return out.str();
    }

    std::string operator()(ExtensionsAction) const
    {
        std::ostringstream out;
        print_extension_stats(out, compute_extension_stats(tree));
        return out.str();
    }

    std::string operator()(EmptyDirsAction) const
    {
        std::ostringstream out;
        print_empty_directories(out, get_empty_dir(), tree);
        return out.str();
    }

    std::string operator()(SymlinksAction) const
    {
        std::ostringstream out;
        print_symlinks(out, get_file_stats().symlink_ids, tree);
        return out.str();
    }

    std::string operator()(LargestFilesAction op) const
    {
        std::ostringstream out;
        print_largest_files(out, compute_largest_N_Files(tree, op.n), tree);
        return out.str();
    }

    std::string operator()(LargestDirsAction op) const
    {
        std::ostringstream out;
        print_largest_directories(out, compute_largest_N_Directories(tree, get_metrics(), op.n), tree);
        return out.str();
    }

    std::string operator()(RecentAction op) const
    {
        std::ostringstream out;
        print_recent_files(out, compute_recent_files(tree, op.duration), op.duration, tree);
        return out.str();
    }

    std::string operator()(ErrorsAction) const
    {
        std::ostringstream out;
        print_errors(out, get_errors(tree));
        return out.str();
    }

    std::string operator()(StatsAction) const
    {
        std::ostringstream out;
        print_directory_stats(out, compute_directory_stats(tree, get_metrics()), tree);
        return out.str();
    }

    std::string operator()(MetricsAction) const
    {
        std::ostringstream out;
        print_directory_metrics(out, get_metrics(), tree);
        return out.str();
    }

    void run(const std::vector<Action>& actions) const
    {
        // get shared data
        prewarm();

        std::vector<std::future<std::string>> futures;
        futures.reserve(actions.size());

        // launch multiple analysis in pararell
        for (const auto& action : actions)
        {
            futures.push_back(std::async(std::launch::async, [this, action] { return std::visit(*this, action); }));
        }

        for (auto& f : futures)
        {
            os << f.get();
        }
    }
};
