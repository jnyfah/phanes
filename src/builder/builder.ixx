module;

#include <filesystem>

export module builder;

import core;

export DirectoryTree build_tree(const std::filesystem::path& root);

// for bench marking
export DirectoryTree build_tree(const std::filesystem::path& root, std::size_t num_threads);