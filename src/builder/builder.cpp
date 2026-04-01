module;
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <system_error>
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

    std::error_code type_ec, size_ec, time_ec, itr_ec;

    for (std::filesystem::directory_iterator it(dirPath,
                                                std::filesystem::directory_options::skip_permission_denied,
                                                itr_ec);
         it != std::filesystem::directory_iterator();
         it.increment(itr_ec))
    {
        if (itr_ec && itr_ec != std::errc::permission_denied)
        {
            std::lock_guard lock(guard);
            tree.errors.push_back({dirPath, ErrorKind::NotFound, NodeKind::Directory});
            continue;
        }

        itr_ec.clear();
        const auto& entry = *it;

        auto status = entry.symlink_status(type_ec);
        if (type_ec)
        {
            std::lock_guard lock(guard);
            tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
            continue;
        }

        switch (status.type())
        {
        case std::filesystem::file_type::directory:
        {
            DirectoryNode directory{};
            directory.parent = id;
            directory.path = entry.path();
            DirectoryId dirId;
            {
                std::unique_lock lock(dir_mutex);
                dirId = next_dir_id.fetch_add(1, std::memory_order_relaxed);
                directory.id = dirId;
                tree.directories.push_back(directory);
            }
            {
                std::shared_lock lock(dir_mutex);
                tree.directories[id].subdirs.push_back(dirId);
            }
            active_tasks.fetch_add(1, std::memory_order_relaxed);
            submit_task(dirId);
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

            file.id = next_file_id.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lock(guard);
                tree.files.push_back(file);

                if (!is_symlink && size_ec && size_ec != std::errc::permission_denied &&
                    size_ec != std::errc::no_such_file_or_directory)
                {
                    tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
                }
                if (time_ec && time_ec != std::errc::permission_denied &&
                    time_ec != std::errc::no_such_file_or_directory)
                {
                    tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
                }
            }
            {
                std::shared_lock lock(dir_mutex);
                tree.directories[id].files.push_back(file.id);
            }
            break;
        }
        case std::filesystem::file_type::unknown:
        {
            std::lock_guard lock(guard);
            tree.errors.push_back({entry.path(), ErrorKind::Unknown, NodeKind::File});
            continue;
        }
        default:
            continue;
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

    ThreadPool pool(8, [this](DirectoryId id) { scan_directory(id); });
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
