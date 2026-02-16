module;

#include <filesystem>

export module builder;

import core;

export void output();

// input paths, absolute paths ?? or relative paths?? or both ??

export DirectoryTree build_tree(const std::filesystem::path& root);