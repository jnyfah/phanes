module;

#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <print>
#include <ostream>

module view;

std::string format_size(std::uint64_t bytes)
{
    constexpr std::array<std::string_view, 6> units{"B", "KB", "MB", "GB", "TB", "PB"};

    auto size = static_cast<double>(bytes);
    std::size_t unit_index = 0;

    while (size >= 1024.0 && unit_index < units.size() - 1)
    {
        size *= (1.0 / 1024.0);
        ++unit_index;
    }

    size = std::round(size * 100.0) / 100.0;

    std::string buffer(64, '\0');
    auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), size);

    if (ec != std::errc{})
    {
        return std::format("{:.2f} {}", size, units[unit_index]);
    }

    std::string result(buffer.data(), ptr);
    result += " ";
    result += units[unit_index];

    return result;
}

std::string format_duration(std::chrono::seconds total)
{
    auto seconds_total = total.count();

    const auto days = seconds_total / 86400;
    seconds_total %= 86400;

    const auto hours = seconds_total / 3600;
    seconds_total %= 3600;

    const auto minutes = seconds_total / 60;
    const auto seconds = seconds_total % 60;

    std::string buffer(64, '\0');
    char* ptr = buffer.data();

    auto append_number = [&](auto value)
    {
        auto [p, ec] = std::to_chars(ptr, buffer.data() + buffer.size(), value);
        if (ec == std::errc{})
            ptr = p;
    };

    if (days > 0)
    {
        append_number(days);
        *ptr++ = 'd';
        *ptr++ = ' ';
    }

    if (hours > 0)
    {
        append_number(hours);
        *ptr++ = 'h';
        *ptr++ = ' ';
    }

    if (minutes > 0)
    {
        append_number(minutes);
        *ptr++ = 'm';
        *ptr++ = ' ';
    }

    if (seconds > 0 || ptr == buffer)
    {
        append_number(seconds);
        *ptr++ = 's';
    }

    return std::string(buffer.data(), ptr);
}

void print_summary(std::ostream& os, const SummaryReport& report, const DirectoryTree& tree)
{
    std::println(os, "Scan Summary");
    std::println(os, "------------------\n");

    std::print(os, "{:<18}: {}\n", "Directories", report.total_directories);
    std::print(os, "{:<18}: {}\n", "Files", report.total_files);
    std::print(os, "{:<18}: {}\n", "Symlinks", report.total_symlinks);
    std::print(os, "{:<18}: {}\n", "Errors", report.total_errors);
    std::print(os, "{:<18}: {}\n", "Total Size", format_size(report.total_size));

    if (report.largest_file)
    {
        const auto& file = tree.files[*report.largest_file];
        std::print(os, "{:<18}: {} ({})\n", "Largest File", file.path.filename().string(), format_size(file.size));
    }
    else
    {
        std::print(os, "{:<18}: {}\n", "Largest File", "NIL");
    }

    std::print(os, "{:<18}: {}\n", "Max Depth", report.max_depth);
    std::print(os, "{:<18}: {}\n", "Max Depth dir", report.max_depth_dir);
    std::print(os, "{:<18}: {}\n", "Scan Duration", format_duration(report.total_duration));
    std::println(os);
}

void print_largest_files(std::ostream& os, const std::vector<FileId>& files, const DirectoryTree& tree)
{
    std::println(os, "Largest {} Files", files.size());
    std::println(os, "------------------------------------\n");
    std::print(os, "{:<40} {:>12}  {}\n", "Filename", "Size", "Location");
    std::print(os, "{:-<40} {:-<12}  {:-<20}\n", "", "", "");

    for (const auto file_id : files)
    {
        const auto& file = tree.files[file_id];
        std::print(os,
                   "{:<40} {:>12}  {}\n",
                   file.path.filename().string(),
                   format_size(file.size),
                   file.path.parent_path().string());
    }

    std::println(os);
}

void print_largest_directories(std::ostream& os, const std::vector<DirectoryId>& directories, const DirectoryTree& tree)
{
    std::println(os, "Largest {} Directories", directories.size());
    std::println(os, "--------------------------------------------\n");
    std::print(os, "{:<40} {:>12} {:>12}  {}\n", "Directory", "Files", "Subdirs", "Location");
    std::print(os, "{:-<40} {:-<12} {:-<12}  {:-<20}\n", "", "", "", "");

    for (auto dir_id : directories)
    {
        const auto& dir = tree.directories[dir_id];
        std::print(os,
                   "{:<40} {:>12} {:>12}  {}\n",
                   dir.path.filename().string(),
                   dir.files.size(),
                   dir.subdirs.size(),
                   dir.path.parent_path().string());
    }

    std::println(os);
}

void print_empty_directories(std::ostream& os, const std::vector<DirectoryId>& directories, const DirectoryTree& tree)
{
    std::println(os, "Empty Directories");
    std::println(os, "--------------------------------------\n");
    std::print(os, "{:<40}  {}\n", "Directory", "Location");
    std::print(os, "{:-<40} {:-<20}\n", "", "");

    for (auto dir_id : directories)
    {
        const auto& dir = tree.directories[dir_id];
        std::print(os, "{:<40}  {}\n", dir.path.filename().string(), dir.path.parent_path().string());
    }

    std::println(os);
}

void print_symlinks(std::ostream& os, const std::vector<FileId>& files, const DirectoryTree& tree)
{
    std::println(os, "Symlinks");
    std::println(os, "--------------------------------------\n");
    std::print(os, "{:<40} {:>12}  {}\n", "Filename", "Size", "Location");
    std::print(os, "{:-<40} {:-<12}  {:-<20}\n", "", "", "");

    for (const auto file_id : files)
    {
        const auto& file = tree.files[file_id];
        std::print(os,
                   "{:<40} {:>12}  {}\n",
                   file.path.filename().string(),
                   format_size(file.size),
                   file.path.parent_path().string());
    }

    std::println(os);
}

void print_recent_files(std::ostream& os,
                        const std::vector<FileId>& files,
                        const std::chrono::seconds& duration,
                        const DirectoryTree& tree)
{
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    std::println(os, "Files modified within the last {}", format_duration(duration));
    std::println(os, "--------------------------------------------\n");
    std::print(os, "{:<35} {:>12} {:<20} {:>12}\n", "Filename", "Size", "Location", "Modified");
    std::print(os, "{:-<35} {:-<12} {:-<20} {:-<12}\n", "", "", "", "");

    for (const auto file_id : files)
    {
        const auto& file = tree.files[file_id];
        std::print(os,
                   "{:<35} {:>12} {:<20} {:>12}\n",
                   file.path.filename().string(),
                   format_size(file.size),
                   file.path.parent_path().string(),
                   format_duration(now - file.modified) + " ago");
    }

    std::println(os);
}

void print_extension_stats(std::ostream& os, const std::vector<ExtensionStats>& stats)
{
    std::println(os, "File Extension Statistics ({} types)", stats.size());
    std::println(os, "--------------------------------------------\n");
    std::print(os, "{:<20} {:>12} {:>14}\n", "Extension", "Count", "Total Size");
    std::print(os, "{:-<20} {:-<12} {:-<14}\n", "", "", "");

    for (const auto& stat : stats)
    {
        std::string ext = stat.extension.empty() ? "[no ext]" : stat.extension;
        std::print(os, "{:<20} {:>12} {:>14}\n", ext, stat.count, format_size(stat.total_size));
    }

    std::println(os);
}

void print_errors(std::ostream& os, const std::deque<ErrorRecord>& errors)
{
    if (errors.empty())
    {
        std::println(os, "No errors recorded.");
        return;
    }

    auto kind_to_string = [](ErrorKind kind) -> std::string_view
    {
        switch (kind)
        {
        case ErrorKind::PermissionDenied:
            return "PermissionDenied";
        case ErrorKind::NotFound:
            return "NotFound";
        case ErrorKind::IOError:
            return "IOError";
        case ErrorKind::FileError:
            return "FileError";
        case ErrorKind::Unknown:
            return "Unknown";
        }
        return "Unknown";
    };

    auto node_to_string = [](NodeKind kind) -> std::string_view
    {
        switch (kind)
        {
        case NodeKind::File:
            return "File";
        case NodeKind::Directory:
            return "Directory";
        }
        return "Unknown";
    };

    std::println(os, "Errors");
    std::println(os, "--------------------------------------\n");
    std::print(os, "{:<18} {:<12} {}\n", "Type", "Node", "Path");
    std::print(os, "{:-<18} {:-<12} {:-<1}\n", "", "", "");

    for (const auto& err : errors)
    {
        std::print(os,
                   "{:<18} {:<12} {}\n",
                   kind_to_string(err.kind),
                   node_to_string(err.node_kind),
                   err.path.string());
    }
}

void print_directory_stats(std::ostream& os, const DirectoryStats& stats, const DirectoryTree& tree)
{
    std::println(os, "Directory Statistics");
    std::println(os, "--------------------------------------------\n");

    std::print(os, "{:<35} {}\n", "Max Depth", stats.max_depth);
    std::print(os, "{:<35} {}\n", "Deepest Directory", tree.directories[stats.max_depth_dir].path.string());
    std::print(os, "{:<35} {}\n", "Most Files (recursive)", stats.max_files_count);
    std::print(os,
               "{:<35} {}\n",
               "Directory with Most Files",
               tree.directories[stats.max_files_count_dir].path.string());
    std::print(os, "{:<35} {}\n", "Largest Directory (recursive)", format_size(stats.max_files_size));
    std::print(os, "{:<35} {}\n", "Largest Directory Path", tree.directories[stats.max_files_size_dir].path.string());
    std::print(os, "{:<35} {:.2f}\n", "Average Directory Depth", stats.average_directory_depth);
    std::print(os, "{:<35} {:.2f}\n", "Average Files per Directory", stats.average_files_per_directory);

    std::println(os);
}

void print_directory_metrics(std::ostream& os, const DirectoryMetrics& metrics, const DirectoryTree& tree)
{
    std::println(os, "Directory Metrics ({} directories)", tree.directories.size());
    std::println(os, "--------------------------------------------------\n");
    std::print(os, "{:<40} {:>8} {:>14} {:>10}\n", "Directory", "Depth", "Size", "Files");
    std::print(os, "{:-<40} {:-<8} {:-<14} {:-<10}\n", "", "", "", "");

    for (DirectoryId id = 0; id < tree.directories.size(); ++id)
    {
        const auto& dir = tree.directories[id];
        std::print(os,
                   "{:<40} {:>8} {:>14} {:>10}\n",
                   dir.path.filename().string(),
                   metrics.depth[id],
                   format_size(metrics.recursive_size[id]),
                   metrics.recursive_file_count[id]);
    }

    std::println(os);
}
