module;
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <system_error>
#include <vector>
module builder;

import :scheduler;

struct Scanner
{
    auto build(const std::filesystem::path& root) -> DirectoryTree;

  private:
    void scan_directory(DirectoryId id);

    DirectoryTree tree;
    std::shared_mutex dir_mutex; // shared for reads/per-node writes, exclusive for push_back
    std::mutex guard; // exclusive for tree.files and tree.errors
    std::atomic_int active_tasks{0};
    std::atomic<DirectoryId> next_dir_id{0};
    std::atomic<FileId> next_file_id{0};
    std::function<void(DirectoryId)> submit_task;
};

void Scanner::scan_directory(DirectoryId id)
{
    const std::filesystem::path dirPath = [&]
    {
        std::shared_lock lock(dir_mutex);
        return tree.directories[id].path;
    }();

    // this variables will be local to each thread
    std::vector<DirectoryNode> local_dirs;
    std::vector<FileNode> local_files;
    std::vector<ErrorRecord> local_errors;

    std::error_code type_ec, size_ec, time_ec, itr_ec;

    for (std::filesystem::directory_iterator it(dirPath,
                                                std::filesystem::directory_options::skip_permission_denied,
                                                itr_ec);
         it != std::filesystem::directory_iterator();
         it.increment(itr_ec))
    {
        if (itr_ec && itr_ec != std::errc::permission_denied)
        {
            local_errors.push_back({dirPath, ErrorKind::NotFound, NodeKind::Directory});
            continue;
        }

        itr_ec.clear();
        const auto& entry = *it;

        auto status = entry.symlink_status(type_ec);
        if (type_ec)
        {
            local_errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
            continue;
        }

        switch (status.type())
        {
        case std::filesystem::file_type::directory:
        {
            DirectoryNode directory{};
            directory.parent = id;
            directory.path = entry.path();
            local_dirs.push_back(std::move(directory));
            break;
        }
        case std::filesystem::file_type::regular:
        case std::filesystem::file_type::symlink:
        {
            const bool is_symlink = status.type() == std::filesystem::file_type::symlink;

            FileNode file{};
            file.parent = id;
            file.path = entry.path();
            file.is_symlink = is_symlink;

            if (!is_symlink)
            {
                auto size = entry.file_size(size_ec);
                if (!size_ec)
                    file.size = size;
            }

            auto ftime = entry.last_write_time(time_ec);
            if (!time_ec)
            {
                auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
                file.modified = std::chrono::floor<std::chrono::seconds>(sysTime);
            }

            if (!is_symlink && size_ec && size_ec != std::errc::permission_denied &&
                size_ec != std::errc::no_such_file_or_directory)
            {
                local_errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
            }
            if (time_ec && time_ec != std::errc::permission_denied && time_ec != std::errc::no_such_file_or_directory)
            {
                local_errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
            }

            local_files.push_back(std::move(file));
            break;
        }
        case std::filesystem::file_type::unknown:
        {
            local_errors.push_back({entry.path(), ErrorKind::Unknown, NodeKind::File});
            continue;
        }
        default:
            continue;
        }
    }

    // flush all subdir, one lock acquisition per directory scan
    if (!local_dirs.empty())
    {
        active_tasks.fetch_add(local_dirs.size(), std::memory_order_relaxed);
        std::vector<DirectoryId> new_dir_ids;
        new_dir_ids.reserve(local_dirs.size());
        {
            std::unique_lock lock(dir_mutex);
            for (auto& dir : local_dirs)
            {
                DirectoryId dirId = next_dir_id.fetch_add(1, std::memory_order_relaxed);
                dir.id = dirId;
                new_dir_ids.push_back(dirId);
                tree.directories.push_back(std::move(dir));
                tree.directories[id].subdirs.push_back(dirId);
            }
        }
        for (DirectoryId dirId : new_dir_ids)
        {
            submit_task(dirId);
        }
    }

    // flush local files and errors, one lock acquisition per directory scan
    {
        std::lock_guard lock(guard);
        for (auto& files : local_files)
        {
            files.id = next_file_id.fetch_add(1, std::memory_order_relaxed);
            tree.files.push_back(files);
        }
        for (auto& error : local_errors)
        {
            tree.errors.push_back(std::move(error));
        }
    }

    // update this directory's file list
    if (!local_files.empty())
    {
        std::shared_lock lock(dir_mutex);
        for (const auto& files : local_files)
        {
            tree.directories[id].files.push_back(files.id);
        }
    }

    if (active_tasks.fetch_sub(1, std::memory_order_release) == 1)
    {
        active_tasks.notify_one();
    }
}

auto Scanner::build(const std::filesystem::path& root) -> DirectoryTree
{
    tree.scan_started = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    auto finish = [&]() -> DirectoryTree
    {
        tree.scan_finished = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return std::move(tree);
    };

    std::error_code ec;
    const auto normalizedRoot = std::filesystem::weakly_canonical(root, ec);

    if (ec && ec != std::errc::permission_denied)
    {
        tree.errors.push_back({root, ErrorKind::NotFound, NodeKind::Directory});
        return finish();
    }

    if (!std::filesystem::exists(normalizedRoot))
    {
        tree.errors.push_back({normalizedRoot, ErrorKind::NotFound, NodeKind::Directory});
        return finish();
    }

    if (!std::filesystem::is_directory(normalizedRoot))
    {
        tree.errors.push_back({normalizedRoot, ErrorKind::IOError, NodeKind::Directory});
        return finish();
    }

    DirectoryNode root_node{};
    root_node.id = next_dir_id.fetch_add(1, std::memory_order_relaxed);
    root_node.parent = std::nullopt;
    root_node.path = normalizedRoot;
    tree.directories.push_back(root_node);
    tree.root = root_node.id;

    ThreadPool pool([this](DirectoryId id) { scan_directory(id); });
    submit_task = [&pool](DirectoryId id) { pool.submit(id); };

    active_tasks.fetch_add(1, std::memory_order_relaxed);

    submit_task(tree.root.value());

    int expected;
    while ((expected = active_tasks.load(std::memory_order_relaxed)) != 0)
    {
        active_tasks.wait(expected);
    }

    return finish();
}

DirectoryTree build_tree(const std::filesystem::path& root)
{
    return Scanner{}.build(root);
}

// to do
// make scan time ms
