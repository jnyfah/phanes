module;

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>
#include <chrono>

export module parser;

// Each action is self-contained data
struct SummaryAction
{
};
struct ExtensionsAction
{
};
struct EmptyDirsAction
{
};
struct SymlinksAction
{
};
struct LargestFilesAction
{
    std::size_t n;
};
struct LargestDirsAction
{
    std::size_t n;
};
struct RecentAction
{
    std::chrono::seconds duration;
};

using Action = std::variant<SummaryAction,
                            ExtensionsAction,
                            EmptyDirsAction,
                            SymlinksAction,
                            LargestFilesAction,
                            LargestDirsAction,
                            RecentAction>;

struct ParseResult
{
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
};

export void handle_summary(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_largest_files(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_largest_dir(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_recent(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_extensions(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_empty_dir(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_symlinks(std::vector<Action>&, std::optional<std::string_view>, std::vector<std::string>&);

constexpr size_t N = 7;
constexpr std::array<FlagSpec, N> flag_table{FlagSpec{"--summary", false, handle_summary},
                                             FlagSpec{"--largest-files", true, handle_largest_files},
                                             FlagSpec{"--largest-dirs", true, handle_largest_dir},
                                             FlagSpec{"--recent", true, handle_recent},
                                             FlagSpec{"--extensions", false, handle_extensions},
                                             FlagSpec{"--empty-dirs", false, handle_empty_dir},
                                             FlagSpec{"--symlinks", false, handle_symlinks}};

export auto parse_positive_size(std::string_view,
                                std::vector<std::string>& errors,
                                std::string_view flag_name) -> std::optional<size_t>;

export auto parse(std::span<std::string_view> args) -> ParseResult;