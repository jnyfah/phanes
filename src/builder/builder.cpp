module;

#include <chrono>
#include <filesystem>
#include <stack>
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

    std::stack<DirectoryId> toprocess;
    toprocess.push(node.id);

    while (!toprocess.empty())
    {
        auto curID = toprocess.top();
        auto& curDir = tree.directories[curID];
        toprocess.pop();

        // error codes
        std::error_code type_ec, size_ec, time_ec, itr_ec;

        for (std::filesystem::directory_iterator it(curDir.path,
                                                    std::filesystem::directory_options::skip_permission_denied,
                                                    itr_ec);
             it != std::filesystem::directory_iterator();
             it.increment(itr_ec))
        {

            if (itr_ec && itr_ec != std::errc::permission_denied)
            {
                tree.errors.push_back({curDir.path, ErrorKind::NotFound, NodeKind::Directory});
                continue;
            }

            itr_ec.clear();
            const auto& entry = *it;

            auto status = entry.symlink_status(type_ec);
            if (type_ec)
            {
                tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
                continue;
            }

            switch (status.type())
            {
            case std::filesystem::file_type::directory:
            {
                DirectoryId dirId = next_dir_id++;
                DirectoryNode directory{};
                directory.id = dirId;
                directory.parent = curID;
                directory.path = entry.path();
                tree.directories.push_back(directory);
                tree.directories[curID].subdirs.push_back(dirId);
                toprocess.push({dirId});
                break;
            }
            case std::filesystem::file_type::regular:
            case std::filesystem::file_type::symlink:
            {
                const bool symlink = status.type() == std::filesystem::file_type::symlink;
                FileId fileId = next_file_id++;
                FileNode file{};
                file.id = fileId;
                file.parent = curID;
                file.path = entry.path();
                file.is_symlink = symlink;
                tree.directories[curID].files.push_back(fileId);

                if (!symlink)
                {
                    auto size = entry.file_size(size_ec);
                    if (!size_ec)
                        file.size = size;
                    else if (size_ec != std::errc::permission_denied && size_ec != std::errc::no_such_file_or_directory)
                        tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
                }

                auto ftime = entry.last_write_time(time_ec);
                if (!time_ec)
                {
                    auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
                    file.modified = std::chrono::floor<std::chrono::seconds>(sysTime);
                }
                else if (time_ec != std::errc::permission_denied && time_ec != std::errc::no_such_file_or_directory)
                    tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});

                tree.files.push_back(file);
                break;
            }
            case std::filesystem::file_type::unknown:
            {
                tree.errors.push_back({entry.path(), ErrorKind::Unknown, NodeKind::File});
                continue;
            }
            default:
                continue; // skip sockets, fifo etc
            }
        }
    }

    return finish();
}
