module;

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

module parser;

void handle_summary(CLIOptions& options,
                    std::optional<std::string_view> str,
                    std::vector<std::string>& errors)
{
    options.summary = true;
}
void handle_largest_files(CLIOptions& options,
                          std::optional<std::string_view> str,
                          std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.push_back("Missing value for --largest-files");
        return;
    }
    options.largest_files = parse_positive_size(str->data(), errors, "largest-files");
}
void handle_largest_dir(CLIOptions& options,
                        std::optional<std::string_view> str,
                        std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.push_back("Missing value for --largest-dir");
        return;
    }
    options.largest_dirs = parse_positive_size(str->data(), errors, "largest-dirs");
}

void handle_recent(CLIOptions& options,
                   std::optional<std::string_view> str,
                   std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.push_back("Missing value for --recently modified");
        return;
    }

    size_t value{};
    auto [ptr, ec] = std::from_chars(str->data(), str->data() + str->size(), value);

    auto begin = str->data();
    auto end = begin + str->size();

    if (ptr == end)
    {
        errors.emplace_back("Missing Unit specifier for recently modified (s/m/h/d)");
        return;
    }

    if (ptr + 1 != end)
    {
        errors.emplace_back("Unit specifier for recently modified  must be sigle letter (s/m/h/d)");
        return;
    }

    char unit = *ptr;
    if (ec == std::errc())
    {
        if (value == 0)
        {
            errors.emplace_back("Value must be greater than 0 for recently modified");
            return;
        }

        if (std::tolower(unit) == 'h')
        {
            options.recent = std::chrono::hours(value);
        }
        else if (std::tolower(unit) == 'm')
        {
            options.recent = std::chrono::minutes(value);
        }
        else if (std::tolower(unit) == 's')
        {
            options.recent = std::chrono::seconds(value);
        }
        else if (std::tolower(unit) == 'd')
        {
            options.recent = std::chrono::days(value);
        }
    }
}

void handle_extensions(CLIOptions& options,
                       std::optional<std::string_view> str,
                       std::vector<std::string>& errors)
{
    options.extensions = true;
}
void handle_empty_dir(CLIOptions& options,
                      std::optional<std::string_view> str,
                      std::vector<std::string>& errors)
{
    options.empty_dirs = true;
}
void handle_symlinks(CLIOptions& options,
                     std::optional<std::string_view> str,
                     std::vector<std::string>& errors)
{
    options.symlinks = true;
}

std::optional<size_t> parse_positive_size(std::string_view str,
                                          std::vector<std::string>& errors,
                                          std::string_view flag_name)
{

    if (str.empty())
    {
        std::string msg = "Missing value for ";
        msg += flag_name;
        errors.emplace_back(std::move(msg));
        return std::nullopt;
    }
    size_t value{};
    auto [_, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
    if (ec == std::errc())
    {
        if (value == 0)
        {
            std::string msg = "Value must be greater than 0 for ";
            msg += flag_name;
            errors.emplace_back(std::move(msg));
            return std::nullopt;
        }
    }
    else if (ec == std::errc::invalid_argument)
    {
        errors.push_back("This is not a number.\n");
        return std::nullopt;
    }
    else if (ec == std::errc::result_out_of_range)
    {
        std::string msg = "This number is larger than an int for ";
        msg += flag_name;
        errors.emplace_back(std::move(msg));
        return std::nullopt;
    }
    return value;
}

auto parse(std::span<std::string_view> args) -> ParseResult
{
    ParseResult result{};

    if (args.empty())
    {
        result.errors.push_back("no argument passed");
        return result;
    }
    result.options.path = args[0];

    size_t i = 1;
    while (i < args.size())
    {
        auto token = args[i];
        auto flag = std::ranges::find(flag_table, token, &FlagSpec::name);
        if (flag != flag_table.end())
        {
            if (flag->requires_value)
            {
                ++i;
                if (i >= args.size())
                {
                    std::string msg = "Missing value for ";
                    msg += flag->name;
                    result.errors.emplace_back(std::move(msg));
                    break;
                }
                flag->handler(result.options, args[i], result.errors);
                ++i;
            }
            else
            {
                flag->handler(result.options, std::nullopt, result.errors);
                ++i;
            }
        }
        else
        {
            result.errors.push_back("Unknown option: " + std::string(token));
            ++i;
        }
    }

    result.success = result.errors.empty();
    return result;
}
