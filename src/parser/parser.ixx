module;

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

export module parser;

// Each action is self-contained data
export struct SummaryAction
{
};
export struct ExtensionsAction
{
};
export struct EmptyDirsAction
{
};
export struct SymlinksAction
{
};
export struct ErrorsAction
{
};
export struct LargestFilesAction
{
    std::size_t n;
};
export struct LargestDirsAction
{
    std::size_t n;
};
export struct RecentAction
{
    std::chrono::seconds duration;
};

export struct MetricsAction
{
};

export struct StatsAction
{
};

using Action = std::variant<SummaryAction,
                            ExtensionsAction,
                            EmptyDirsAction,
                            SymlinksAction,
                            ErrorsAction,
                            LargestFilesAction,
                            LargestDirsAction,
                            RecentAction,
                            MetricsAction,
                            StatsAction>;

struct ParseResult
{
    std::filesystem::path path;
    bool success = true;
    std::vector<Action> actions;
    std::vector<std::string> errors;
};

using Handler = void (*)(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);

struct FlagSpec
{
    std::string_view name;
    bool requires_value;
    Handler handler;
    std::string_view description;
    std::string_view value_hint;
};

export void handle_summary(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_largest_files(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_largest_dir(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_recent(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_extensions(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_empty_dir(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_symlinks(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_errors(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_metrics(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_stats(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);

constexpr size_t N = 10;
constexpr std::array<FlagSpec, N> flag_table{
    FlagSpec{"--summary", false, handle_summary, "Show overall scan summary", ""},
    FlagSpec{"--largest-files", true, handle_largest_files, "List the N largest files", "<N>"},
    FlagSpec{"--largest-dirs", true, handle_largest_dir, "List the N largest directories", "<N>"},
    FlagSpec{"--recent", true, handle_recent, "List files modified within the last N seconds", "<N>"},
    FlagSpec{"--extensions", false, handle_extensions, "Show file extension statistics", ""},
    FlagSpec{"--empty-dirs", false, handle_empty_dir, "List empty directories", ""},
    FlagSpec{"--symlinks", false, handle_symlinks, "List symbolic links", ""},
    FlagSpec{"--errors", false, handle_errors, "Show filesystem errors encountered", ""},
    FlagSpec{"--metrics", false, handle_metrics, "Show per-directory depth and size metrics", ""},
    FlagSpec{"--stats", false, handle_stats, "Show aggregate directory statistics", ""},
};

export auto parse_positive_size(std::string_view,
                                std::vector<std::string>& errors,
                                std::string_view flag_name) -> std::optional<size_t>;

export auto parse(std::span<std::string_view> args) -> ParseResult;
export void print_help(std::ostream& os);