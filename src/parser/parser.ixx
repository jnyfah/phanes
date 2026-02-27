module;

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

export module parser;

struct CLIOptions
{
    std::filesystem::path path;

    bool summary = false;

    std::optional<std::size_t> largest_files;
    std::optional<std::size_t> largest_dirs;

    bool extensions = false;
    bool empty_dirs = false;
    bool symlinks = false;

    std::optional<std::chrono::seconds> recent;
};

struct ParseResult
{
    bool success = true;
    CLIOptions options;
    std::vector<std::string> errors;
};

using Handler = void (*)(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);

struct FlagSpec
{
    std::string_view name;
    bool requires_value;
    Handler handler;
};

export void handle_summary(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);
export void
handle_largest_files(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);
export void
handle_largest_dir(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);
export void handle_recent(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);
export void
handle_extensions(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);
export void
handle_empty_dir(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);
export void
handle_symlinks(CLIOptions&, std::optional<std::string_view>, std::vector<std::string>&);

constexpr size_t N = 7;
constexpr std::array flag_table{FlagSpec{"--summary", false, handle_summary},
                                FlagSpec{"--largest-files", true, handle_largest_files},
                                FlagSpec{"--largest-dirs", true, handle_largest_dir},
                                FlagSpec{"--recent", true, handle_recent},
                                FlagSpec{"--extensions", false, handle_extensions},
                                FlagSpec{"--empty_dirs", false, handle_empty_dir},
                                FlagSpec{"--symlinks", false, handle_symlinks}};

export auto parse_positive_size(std::string_view,
                                std::vector<std::string>& errors,
                                std::string_view flag_name) -> std::optional<size_t>;

// if no flang print help

// Parse CLI
// ↓
// Validate flags
// ↓
// Build tree once
// ↓
// If structural reports requested → compute metrics once
// ↓
// Execute selected reports
// ↓
// Format output

// Require at least 2 arguments
// 2️⃣ First argument after program name → path
// 3️⃣ Remaining arguments → flags
// 4️⃣ Walk left to right
// 5️⃣ When flag requires value, consume next token

export auto parse(std::span<std::string_view> args) -> ParseResult;