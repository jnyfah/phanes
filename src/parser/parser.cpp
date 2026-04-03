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
        errors.emplace_back("Missing number of files for --largest-files");
        return;
    }
    auto value = parse_positive_size(*str, "--largest-files");
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
        errors.emplace_back("Missing number of directories for --largest-dirs");
        return;
    }

    auto value = parse_positive_size(*str, "--largest-dirs");
    if (!value)
    {
        errors.emplace_back(value.error());
        return;
    }

    actions.emplace_back(LargestDirsAction{*value});
}

void handle_recent(std::vector<Action>& actions, std::optional<std::string_view> str, std::vector<std::string>& errors)
{
    if (!str)
    {
        errors.emplace_back("Missing time specification for --recently modified");
        return;
    }

    size_t value{};
    auto [ptr, ec] = std::from_chars(str->data(), str->data() + str->size(), value);
    auto end = str->data() + str->size();

    // ptr stops where the number ends, anything after is the unit
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
    if (ec != std::errc())
    {
        errors.emplace_back("Invalid number for --recent");
        return;
    }
    if (value == 0)
    {
        errors.emplace_back("Value must be greater than 0 for recently modified");
        return;
    }

    switch (std::tolower(static_cast<unsigned char>(unit)))
    {
    case 's':
        actions.emplace_back(RecentAction{std::chrono::seconds(value)});
        break;
    case 'm':
        actions.emplace_back(RecentAction{std::chrono::minutes(value)});
        break;
    case 'h':
        actions.emplace_back(RecentAction{std::chrono::hours(value)});
        break;
    case 'd':
        actions.emplace_back(RecentAction{std::chrono::days(value)});
        break;
    default:
        errors.emplace_back(std::string("Unknown unit '") + unit + "' for --recent (s/m/h/d)");
        break;
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

void handle_errors(std::vector<Action>& actions, std::optional<std::string_view> str, std::vector<std::string>& errors)
{
    actions.emplace_back(ErrorsAction{});
}

void handle_metrics(std::vector<Action>& actions, std::optional<std::string_view> str, std::vector<std::string>& errors)
{
    actions.emplace_back(MetricsAction{});
}

void handle_stats(std::vector<Action>& actions, std::optional<std::string_view> str, std::vector<std::string>& errors)
{
    actions.emplace_back(StatsAction{});
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

        flag->handler(result.actions, value, errors);
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
