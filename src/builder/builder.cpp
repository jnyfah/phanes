module;

#include <chrono>
#include <filesystem>
#include <stack>
#include <system_error>

module builder;

DirectoryTree build_tree(const std::filesystem::path& root)
{
    DirectoryTree tree{};

    tree.scan_started = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    auto finish = [&]()
    {
        tree.scan_finished =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        return tree;
    };

    std::error_code ec;
    const auto normalizedRoot = std::filesystem::weakly_canonical(root, ec);

    if (ec)
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
        std::error_code type_ec, size_ec, time_ec, itr_ec;

        for (std::filesystem::directory_iterator it(
                 curDir.path,
                 std::filesystem::directory_options::skip_permission_denied,
                 itr_ec);
             it != std::filesystem::directory_iterator();
             it.increment(itr_ec))
        {

            if (itr_ec)
            {
                tree.errors.push_back({curDir.path, ErrorKind::NotFound, NodeKind::Directory});
                continue;
            }

            itr_ec.clear();
            const auto& entry = *it;
            bool symlink = entry.is_symlink(type_ec);

            if (entry.is_directory(type_ec) && !symlink)
            {
                DirectoryId dirId = next_dir_id++;

                DirectoryNode directory{};
                directory.id = dirId;
                directory.parent = curID;
                directory.path = entry.path();
                tree.directories.push_back(directory);

                tree.directories[curID].subdirs.push_back(dirId);
                toprocess.push({dirId});
            }
            else if (entry.is_regular_file(type_ec) || symlink)
            {

                FileId fileId = next_file_id++;
                FileNode file{};

                file.id = fileId;
                file.parent = curID;
                file.path = entry.path();

                tree.directories[curID].files.push_back(fileId);
                file.is_symlink = symlink;

                auto size = entry.file_size(size_ec);
                if (!size_ec)
                {
                    file.size = size;
                }
                else
                {
                    file.size = 0;
                    tree.errors.push_back({entry.path(), ErrorKind::FileError, NodeKind::File});
                }

                auto ftime = entry.last_write_time(time_ec);
                if (!time_ec)
                {
                    const auto sctp =
                        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            ftime - decltype(ftime)::clock::now() +
                            std::chrono::system_clock::now());

                    file.modified = std::chrono::floor<std::chrono::seconds>(sctp);
                }
                else
                {
                    tree.errors.push_back({curDir.path, ErrorKind::FileError, NodeKind::File});
                }

                tree.files.push_back(file);
            }
            else
            {
                tree.errors.push_back({entry, ErrorKind::Unknown, NodeKind::File});
            }
        }
    }

    return finish();
}
