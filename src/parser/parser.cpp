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

void handle_summary(std::vector<Action>& actions, std::optional<std::string_view> str, std::vector<std::string>& errors)
{
    actions.emplace_back(SummaryAction{});
}

void handle_largest_files(std::vector<Action>& actions,
                          std::optional<std::string_view> str,
                          std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.emplace_back("Missing value for --largest-files");
        return;
    }
    auto value = parse_positive_size(*str, errors, "--largest-files");
    if (!value)
    {
        errors.emplace_back("Invalid value for --largest-files");
        return;
    }

    actions.emplace_back(LargestFilesAction{*value});
}

void handle_largest_dir(std::vector<Action>& actions,
                        std::optional<std::string_view> str,
                        std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.emplace_back("Missing value for --largest-dirs");
        return;
    }

    auto value = parse_positive_size(*str, errors, "--largest-dirs");
    if (!value)
    {
        errors.emplace_back("Invalid value for --largest-dirs");
        return;
    }

    actions.emplace_back(LargestDirsAction{*value});
}

void handle_recent(std::vector<Action>& actions, std::optional<std::string_view> str, std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.emplace_back("Missing value for --recently modified");
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
            actions.emplace_back(RecentAction{std::chrono::hours(value)});
        }
        else if (std::tolower(unit) == 'm')
        {
            actions.emplace_back(RecentAction{std::chrono::minutes(value)});
        }
        else if (std::tolower(unit) == 's')
        {
            actions.emplace_back(RecentAction{std::chrono::seconds(value)});
        }
        else if (std::tolower(unit) == 'd')
        {
            actions.emplace_back(RecentAction{std::chrono::days(value)});
        }
    }
}

void handle_extensions(std::vector<Action>& actions,
                       std::optional<std::string_view> str,
                       std::vector<std::string>& errors)
{
    actions.emplace_back(ExtensionsAction{});
}
void handle_empty_dir(std::vector<Action>& actions,
                      std::optional<std::string_view> str,
                      std::vector<std::string>& errors)
{
    actions.emplace_back(EmptyDirsAction{});
}
void handle_symlinks(std::vector<Action>& actions,
                     std::optional<std::string_view> str,
                     std::vector<std::string>& errors)
{
    actions.emplace_back(SymlinksAction{});
}

std::optional<size_t>
parse_positive_size(std::string_view str, std::vector<std::string>& errors, std::string_view flag_name)
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
        errors.emplace_back("This is not a number.\n");
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

    size_t i = 0;
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
                flag->handler(result.actions, args[i], result.errors);
                ++i;
            }
            else
            {
                flag->handler(result.actions, std::nullopt, result.errors);
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
