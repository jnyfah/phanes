module;
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <system_error>
module builder;

import :scheduler;

DirectoryTree build_tree(const std::filesystem::path& root)
{
    DirectoryTree tree{};

    // scan start time
    tree.scan_started = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    // scan finished time
    auto finish = [&]()
    {
        tree.scan_finished = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return tree;
    };

    std::error_code ec;
    // get absolute path and normalize
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

    DirectoryId next_dir_id = 0;
    FileId next_file_id = 0;

    DirectoryNode node{};
    node.id = next_dir_id++;
    node.parent = std::nullopt;
    node.path = normalizedRoot;

    tree.directories.push_back(node);
    tree.root = node.id;

    std::mutex guard;
    std::condition_variable done_cv;
    int active_tasks = 0;

    std::function<void(DirectoryId)> scan_directory;

    ThreadPool pool(8, [&](DirectoryId id) { scan_directory(id); });

    scan_directory = [&](DirectoryId id)
    {
        const std::filesystem::path dirPath = [&]
        {
            std::lock_guard lock(guard);
            return tree.directories[id].path;
        }();

        // error codes
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
                pool.submit(dirId);
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
                    std::lock_guard lock(guard); // single lock acquisition
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
                continue; // skip sockets, fifo etc
            }
        }
        {
            std::lock_guard lock(guard);
            if (--active_tasks == 0)
                done_cv.notify_one();
        }
    };

    {
        std::lock_guard lock(guard);
        ++active_tasks;
    }
    pool.submit(tree.root.value());

    {
        std::unique_lock lock(guard);
        done_cv.wait(lock, [&] { return active_tasks == 0; });
    }

    return finish();
}
