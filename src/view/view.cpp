module;

#include <chrono>
#include <cmath>
#include <format>

module view;

std::string format_size(std::uint64_t bytes)
{
    constexpr std::array<std::string_view, 6> units{"B", "KB", "MB", "GB", "TB", "PB"};

    double size = static_cast<double>(bytes);
    std::size_t unit_index = 0;

    while (size >= 1024.0 && unit_index < units.size() - 1)
    {
        size *= (1.0 / 1024.0);
        ++unit_index;
    }

    size = std::round(size * 100.0) / 100.0;

    char buffer[64];
    auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), size);

    if (ec != std::errc{})
    {
        return std::format("{:.2f} {}", size, units[unit_index]);
    }

    std::string result(buffer, ptr);
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

    char buffer[64];
    char* ptr = buffer;

    auto append_number = [&](auto value)
    {
        auto [p, ec] = std::to_chars(ptr, buffer + sizeof(buffer), value);
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

    return std::string(buffer, ptr);
}

void print_summary(std::ostream& os, const SummaryReport& report)
{
    os << "Scan Summary\n";
    os << "────────────\n\n";

    os << std::format("{:<18}: {}\n", "Directories", report.total_directories);
    os << std::format("{:<18}: {}\n", "Files", report.total_files);
    os << std::format("{:<18}: {}\n", "Symlinks", report.total_symlinks);
    os << std::format("{:<18}: {}\n", "Errors", report.total_errors);

    auto total_size = format_size(report.total_size);
    os << std::format("{:<18}: {}\n", "Total Size", total_size);

    if (report.largest_file)
    {
        os << std::format("{:<18}: {} ({})\n",
                          "Largest File",
                          report.largest_file->path.filename().string(),
                          format_size(report.largest_file->size));
    }
    else
    {
        os << std::format("{:<18}: {}\n", "Largest File", "NIL");
    }

    os << std::format("{:<18}: {}\n", "Max Depth", report.max_depth);

    os << std::format("{:<18}: {}\n", "Scan Duration", format_duration(report.total_duration));
}

#include <format>

void print_largest_files(std::ostream& os, const std::vector<FileId>& files, const DirectoryTree& tree)
{
    os << std::format("Largest {} Files\n", files.size());
    os << "──────────────────────────────\n\n";

    os << std::format("{:<40} {:>12}  {}\n", "Filename", "Size", "Location");
    os << std::format("{:-<40} {:-<12}  {:-<}\n", "", "", "");

    for (const auto file_id : files)
    {
        const auto& file = tree.files[file_id];

        os << std::format("{:<40} {:>12}  {}\n",
                          file.path.filename().string(),
                          format_size(file.size),
                          file.path.parent_path().string());
    }

    os << '\n';
}

#include <format>

void print_largest_directories(std::ostream& os, const std::vector<DirectoryId>& directories, const DirectoryTree& tree)
{
    os << std::format("Largest {} Directories\n", directories.size());
    os << "──────────────────────────────────────────────\n\n";

    os << std::format("{:<40} {:>12} {:>12}  {}\n", "Directory", "Files", "Subdirs", "Location");

    os << std::format("{:-<40} {:-<12} {:-<12}  {:-<}\n", "", "", "", "");

    for (auto dir_id : directories)
    {
        const auto& dir = tree.directories[dir_id];

        os << std::format("{:<40} {:>12} {:>12}  {}\n",
                          dir.path.filename().string(),
                          dir.files.size(),
                          dir.subdirs.size(),
                          dir.path.parent_path().string());
    }

    os << '\n';
}

void print_empty_directories(std::ostream& os, const std::vector<DirectoryId>& directories, const DirectoryTree& tree)
{
    os << "Empty Directories\n";
    os << "────────────────────────────────────\n\n";

    os << std::format("{:<40}  {}\n", "Directory", "Location");

    os << std::format("{:-<40} {:-<}\n", "", "", "", "");

    for (auto dir_id : directories)
    {
        const auto& dir = tree.directories[dir_id];

        os << std::format("{:<40}  {}\n", dir.path.filename().string(), dir.path.parent_path().string());
    }

    os << '\n';
}

void print_symlinks(std::ostream& os, const std::vector<FileId>& files, const DirectoryTree& tree)
{
    os << "Symlinks \n";
    os << "────────────────────────────────────\n\n";

    os << std::format("{:<40} {:>12}  {}\n", "Filename", "Size", "Location");
    os << std::format("{:-<40} {:-<12}  {:-<}\n", "", "", "");

    for (const auto file_id : files)
    {
        const auto& file = tree.files[file_id];

        os << std::format("{:<40} {:>12}  {}\n",
                          file.path.filename().string(),
                          format_size(file.size),
                          file.path.parent_path().string());
    }

    os << '\n';
}

void print_recent_files(std::ostream& os,
                        const std::vector<FileId>& files,
                        const std::chrono::seconds& duration,
                        const DirectoryTree& tree)
{
    const auto time = format_duration(duration);
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    os << std::format("Files modified within the last {}\n", time);
    os << "────────────────────────────────────────────────────────────\n\n";

    os << std::format("{:<35} {:>12} {:<20} {:>12}\n", "Filename", "Size", "Location", "Modified");

    os << std::format("{:-<35} {:-<12} {:-<20} {:-<12}\n", "", "", "", "");

    for (const auto file_id : files)
    {
        const auto& file = tree.files[file_id];

        os << std::format("{:<35} {:>12} {:<20} {:>12}\n",
                          file.path.filename().string(),
                          format_size(file.size),
                          file.path.parent_path().string(),
                          format_duration(now - file.modified) + " ago");
    }

    os << '\n';
}

void print_extension_stats(std::ostream& os, const std::vector<ExtensionStats>& stats)
{
    os << std::format("File Extension Statistics ({} types)\n", stats.size());
    os << "────────────────────────────────────────────────────────\n\n";

    os << std::format("{:<20} {:>12} {:>14}\n", "Extension", "Count", "Total Size");

    os << std::format("{:-<20} {:-<12} {:-<14}\n", "", "", "");

    for (const auto& stat : stats)
    {
        std::string ext = stat.extension.empty() ? "[no ext]" : stat.extension;
        os << std::format("{:<20} {:>12} {:>14}\n", ext, stat.count, format_size(stat.total_size));
    }

    os << '\n';
}