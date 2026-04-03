module;

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

module parser;

auto handle_summary(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return SummaryAction{};
}

auto handle_largest_files(std::optional<std::string_view> str) -> std::expected<Action, std::string>
{
    if (!str)
    {
        return std::unexpected("Missing value for --largest-files");
    }
    auto value = parse_positive_size(*str, "--largest-files");

    if (!value)
    {
        return std::unexpected(value.error());
    }
    return LargestFilesAction{*value};
}

auto handle_largest_dir(std::optional<std::string_view> str) -> std::expected<Action, std::string>
{
    if (!str)
    {
        return std::unexpected("Missing value for --largest-dirs");
    }

    auto value = parse_positive_size(*str, "--largest-dirs");

    if (!value)
    {
        return std::unexpected(value.error());
    }

    return LargestDirsAction{*value};
}

auto handle_recent(std::optional<std::string_view> str) -> std::expected<Action, std::string>
{
    if (!str)
    {
        return std::unexpected("Missing time specification for --recent");
    }

    size_t value{};
    auto [ptr, ec] = std::from_chars(str->data(), str->data() + str->size(), value);
    auto end = str->data() + str->size();

    if (ptr == end)
    {
        return std::unexpected("Missing unit specifier for --recent (s/m/h/d)");
    }
    if (ptr + 1 != end)
    {
        return std::unexpected("Unit specifier for --recent must be a single letter (s/m/h/d)");
    }
    if (ec != std::errc())
    {
        return std::unexpected("Invalid number for --recent");
    }
    if (value == 0)
    {
        return std::unexpected("Value must be greater than 0 for --recent");
    }

    switch (std::tolower(static_cast<unsigned char>(*ptr)))
    {
    case 's':
        return RecentAction{std::chrono::seconds(value)};
    case 'm':
        return RecentAction{std::chrono::minutes(value)};
    case 'h':
        return RecentAction{std::chrono::hours(value)};
    case 'd':
        return RecentAction{std::chrono::days(value)};
    default:
        return std::unexpected(std::string("Unknown unit '") + *ptr + "' for --recent (s/m/h/d)");
    }
}

auto handle_extensions(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return ExtensionsAction{};
}

auto handle_empty_dir(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return EmptyDirsAction{};
}

auto handle_symlinks(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return SymlinksAction{};
}

auto handle_errors(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return ErrorsAction{};
}

auto handle_metrics(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return MetricsAction{};
}

auto handle_stats(std::optional<std::string_view>) -> std::expected<Action, std::string>
{
    return StatsAction{};
}

auto parse_positive_size(std::string_view str, std::string_view flag_name) -> std::expected<size_t, std::string>
{
    if (str.empty())
    {
        return std::unexpected("Missing value for " + std::string(flag_name));
    }

    size_t value{};
    auto [_, ec] = std::from_chars(str.data(), str.data() + str.size(), value);

    if (ec == std::errc::invalid_argument)
    {
        return std::unexpected("Not a number for " + std::string(flag_name));
    }
    if (ec == std::errc::result_out_of_range)
    {
        return std::unexpected("Value out of range for " + std::string(flag_name));
    }
    if (value == 0)
    {
        return std::unexpected("Value must be greater than 0 for " + std::string(flag_name));
    }

    return value;
}

auto parse(std::span<std::string_view> args) -> std::expected<ParseResult, std::vector<std::string>>
{
    ParseResult result{};
    std::vector<std::string> errors;

    if (args.empty())
    {
        errors.push_back("no argument passed");
        return std::unexpected(errors);
    }
    result.path = args[0]; // first arg is always the target path

    size_t i = 1;
    while (i < args.size())
    {
        auto token = args[i];
        auto flag = std::ranges::find(flag_table, token, &FlagSpec::name);
        if (flag == flag_table.end())
        {
            errors.push_back("Unknown option: " + std::string(token));
            ++i;
            continue;
        }

        std::optional<std::string_view> value = std::nullopt;
        if (flag->requires_value)
        {
            // consume the next token as the flag's value
            if (++i >= args.size())
            {
                errors.emplace_back("Missing value for " + std::string(flag->name));
                break;
            }
            value = args[i];
        }

        if (auto action = flag->handler(value))
        {
            result.actions.push_back(std::move(*action));
        }
        else
        {
            errors.push_back(std::move(action.error()));
        }

        ++i;
    }

    if (!errors.empty())
    {
        return std::unexpected(std::move(errors));
    }
    return result;
}

void print_help(std::ostream& os)
{
    os << "phanes — filesystem analysis tool\n\n";
    os << "Usage:\n";
    os << "  phanes <path> [flags...]\n\n";
    os << "Flags:\n";

    for (const auto& flag : flag_table)
    {
        std::string usage = std::string(flag.name);
        if (!flag.value_hint.empty())
        {
            usage += ' ';
            usage += flag.value_hint;
        }
        os << std::format("  {:<26}  {}\n", usage, flag.description);
    }

    os << "\nExamples:\n";
    os << "  phanes /home --summary\n";
    os << "  phanes /home --largest-files 10 --extensions\n";
    os << "  phanes /home --recent 86400 --symlinks\n";
}
