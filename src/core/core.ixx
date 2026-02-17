module;

#include <filesystem>
#include <vector>
#include <cstdint>
#include <chrono>
#include <optional>

export module core;
export using DirectoryId = std::size_t;
export using FileId = std::size_t;


export enum class NodeKind {
    File,
    Directory
};

export enum class ErrorKind {
    PermissionDenied,
    NotFound,
    IOError,
    Unknown,
    FileError
};

/* ---------- File node ---------- */

export struct FileNode {
    FileId id;
    DirectoryId parent;

    std::filesystem::path path;

    std::uintmax_t size = 0;
    std::chrono::sys_time<std::chrono::seconds> modified;

    bool readable = true;
    bool is_symlink = false;
};

/* ---------- Directory node ---------- */

export struct DirectoryNode {
    DirectoryId id;
    std::optional<DirectoryId> parent;

    std::filesystem::path path;

    std::vector<FileId> files;
    std::vector<DirectoryId> subdirs;

    bool readable = true;
};

/* ---------- Error record ---------- */

export struct ErrorRecord {
    std::filesystem::path path;
    ErrorKind kind;
    NodeKind node_kind;
};

/* ---------- Scan result / tree ---------- */

export struct DirectoryTree {
    std::optional<DirectoryId> root;

    std::vector<FileNode> files;
    std::vector<DirectoryNode> directories;
    std::vector<ErrorRecord> errors;

    std::chrono::sys_time<std::chrono::seconds> scan_started;
    std::chrono::sys_time<std::chrono::seconds> scan_finished;
};
