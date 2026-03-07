module;

#include <filesystem>

export module builder;

import core;

export DirectoryTree build_tree(const std::filesystem::path& root);