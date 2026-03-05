module;

#include <ostream>

export module executor;

import analyzer;
import core;
import parser;
import view;

export struct Executor
{
    const DirectoryTree& tree;
    std::ostream& os;

    void operator()(SummaryAction) const { print_summary(os, compute_summary(tree)); }

    void operator()(ExtensionsAction) const { print_extension_stats(os, compute_extension_stats(tree)); }

    void operator()(EmptyDirsAction) const { print_empty_directories(os, compute_empty_directories(tree), tree); }

    void operator()(SymlinksAction) const { print_symlinks(os, compute_symlinks(tree), tree); }

    void operator()(LargestFilesAction op) const { print_largest_files(os, compute_largest_N_Files(tree, op.n), tree); }

    void operator()(LargestDirsAction op) const
    {
        print_largest_directories(os, compute_largest_N_Directories(tree, op.n), tree);
    }

    void operator()(RecentAction op) const
    {
        print_recent_files(os, compute_recent_files(tree, op.duration), op.duration, tree);
    }
};