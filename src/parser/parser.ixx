module;

#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
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

export using Action = std::variant<SummaryAction,
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
    std::vector<Action> actions;
};

using Handler = std::expected<Action, std::string> (*)(std::optional<std::string_view>);

struct FlagSpec
{
    std::string_view name;
    bool requires_value;
    Handler handler;
    std::string_view description;
    std::string_view value_hint;
};

export auto handle_summary(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_largest_files(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_largest_dir(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_recent(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_extensions(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_empty_dir(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_symlinks(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_errors(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_metrics(std::optional<std::string_view>) -> std::expected<Action, std::string>;
export auto handle_stats(std::optional<std::string_view>) -> std::expected<Action, std::string>;

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

export auto parse_positive_size(std::string_view str, std::string_view flag_name) -> std::expected<size_t, std::string>;

export auto parse(std::span<std::string_view> args) -> std::expected<ParseResult, std::vector<std::string>>;
export void print_help(std::ostream& os);