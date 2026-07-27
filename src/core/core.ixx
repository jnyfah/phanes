module;

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

export module core;
export using DirectoryId = std::size_t;
export using FileId = std::size_t;

export enum class NodeKind { File, Directory };

export enum class ErrorKind { PermissionDenied, NotFound, IOError, Unknown, FileError };

export struct NameRef
{
    std::uint32_t offset;
    std::uint32_t len;
};

export using NameChar = std::filesystem::path::value_type;
export using NameView = std::basic_string_view<NameChar>;

/* ---------- File node ---------- */

export struct FileNode
{
    FileId id;
    DirectoryId parent;

    NameRef name;

    std::uintmax_t size = 0;
    std::chrono::sys_time<std::chrono::seconds> modified;

    bool readable = true;
    bool is_symlink = false;
};

/* ---------- Directory node ---------- */

export struct DirectoryNode
{
    DirectoryId id;
    std::optional<DirectoryId> parent;

    std::filesystem::path path;

    std::vector<FileId> files;
    std::vector<DirectoryId> subdirs;

    bool readable = true;
};

/* ---------- Error record ---------- */

export struct ErrorRecord
{
    std::filesystem::path path;
    ErrorKind kind;
    NodeKind node_kind;
};

/* ---------- Scan tree ---------- */

export struct DirectoryTree
{
    std::optional<DirectoryId> root;

    std::deque<FileNode> files;
    std::deque<DirectoryNode> directories;
    std::deque<ErrorRecord> errors;

    std::vector<NameChar> file_names;

    std::chrono::sys_time<std::chrono::seconds> scan_started;
    std::chrono::sys_time<std::chrono::seconds> scan_finished;
};

export inline auto name_view(const DirectoryTree& tree, const FileNode& file) -> NameView
{
    return {tree.file_names.data() + file.name.offset, file.name.len};
}

export inline auto file_path(const DirectoryTree& tree, const FileNode& file) -> std::filesystem::path
{
    return tree.directories[file.parent].path / name_view(tree, file);
}
