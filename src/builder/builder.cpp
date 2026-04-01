module;
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <system_error>
module builder;

import :scheduler;

struct Scanner
{
    auto build(const std::filesystem::path& root) -> DirectoryTree;

  private:
    void scan_directory(DirectoryId id);

    DirectoryTree tree;
    std::mutex guard;
    std::condition_variable done_cv;
    int active_tasks{0};
    DirectoryId next_dir_id{0};
    FileId next_file_id{0};
    std::function<void(DirectoryId)> submit_task;
};

void Scanner::scan_directory(DirectoryId id)
{
    const std::filesystem::path dirPath = [&]
    {
        std::lock_guard lock(guard);
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
            DirectoryId dirId;
            {
                std::lock_guard lock(guard);
                dirId = next_dir_id++;
                DirectoryNode directory{};
                directory.id = dirId;
                directory.parent = id;
                directory.path = entry.path();
                tree.directories.push_back(directory);
                tree.directories[id].subdirs.push_back(dirId);
                ++active_tasks;
            }
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

            {
                std::lock_guard lock(guard);
                file.id = next_file_id++;
                tree.files.push_back(file);
                tree.directories[id].files.push_back(file.id);

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

    {
        std::lock_guard lock(guard);
        if (--active_tasks == 0)
            done_cv.notify_one();
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
    root_node.id = next_dir_id++;
    root_node.parent = std::nullopt;
    root_node.path = normalizedRoot;
    tree.directories.push_back(root_node);
    tree.root = root_node.id;

    ThreadPool pool(8, [this](DirectoryId id) { scan_directory(id); });
    submit_task = [&pool](DirectoryId id) { pool.submit(id); };

    {
        std::lock_guard lock(guard);
        ++active_tasks;
    }
    submit_task(tree.root.value());

    {
        std::unique_lock lock(guard);
        done_cv.wait(lock, [this] { return active_tasks == 0; });
    }

    return finish();
}

DirectoryTree build_tree(const std::filesystem::path& root)
{
    return Scanner{}.build(root);
}

// to do
// make scan time ms
