#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import parser;

// Helper: build a vector of string_view args and wrap as span
static auto make_args(std::initializer_list<std::string_view> args)
{
    return std::vector<std::string_view>(args);
}

// ============================================================
// parse_positive_size
// ============================================================

TEST(ParsePositiveSize, ValidPositiveNumber)
{
    auto result = parse_positive_size("5", "--largest-files");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 5u);
}

TEST(ParsePositiveSize, LargeNumber)
{
    auto result = parse_positive_size("1000", "--largest-files");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1000u);
}

TEST(ParsePositiveSize, ZeroIsError)
{
    auto result = parse_positive_size("0", "--largest-files");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Value must be greater than 0 for --largest-files");
}

TEST(ParsePositiveSize, NonNumericIsError)
{
    auto result = parse_positive_size("abc", "--largest-files");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Not a number for --largest-files");
}

TEST(ParsePositiveSize, EmptyStringIsError)
{
    auto result = parse_positive_size("", "--largest-files");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Missing value for --largest-files");
}

// ============================================================
// handle_recent
// ============================================================

TEST(HandleRecent, SecondsUnit)
{
    auto result = handle_recent(std::optional<std::string_view>{"60s"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<RecentAction>(*result).duration, std::chrono::seconds(60));
}

TEST(HandleRecent, MinutesUnit)
{
    auto result = handle_recent(std::optional<std::string_view>{"30m"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<RecentAction>(*result).duration, std::chrono::seconds(1800));
}

TEST(HandleRecent, HoursUnit)
{
    auto result = handle_recent(std::optional<std::string_view>{"2h"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<RecentAction>(*result).duration, std::chrono::seconds(7200));
}

TEST(HandleRecent, DaysUnit)
{
    auto result = handle_recent(std::optional<std::string_view>{"1d"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<RecentAction>(*result).duration, std::chrono::seconds(86400));
}

TEST(HandleRecent, UpperCaseUnitIsAccepted)
{
    auto result = handle_recent(std::optional<std::string_view>{"2H"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<RecentAction>(*result).duration, std::chrono::seconds(7200));
}

TEST(HandleRecent, ZeroValueIsError)
{
    auto result = handle_recent(std::optional<std::string_view>{"0h"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Value must be greater than 0 for --recent");
}

TEST(HandleRecent, NonNumericIsError)
{
    // "abc": from_chars fails immediately, ptr+1 != end triggers multi-char unit error
    auto result = handle_recent(std::optional<std::string_view>{"abc"});
    ASSERT_FALSE(result.has_value());
}

TEST(HandleRecent, UnknownUnitIsError)
{
    auto result = handle_recent(std::optional<std::string_view>{"1x"});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("Unknown unit"), std::string::npos);
}

TEST(HandleRecent, MissingValueIsError)
{
    auto result = handle_recent(std::nullopt);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), "Missing time specification for --recent");
}

// ============================================================
// parse
// ============================================================

TEST(Parse, NoArgsReturnsError)
{
    auto args = make_args({});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1u);
    EXPECT_EQ(result.error()[0], "no argument passed");
}

TEST(Parse, PathOnlySucceeds)
{
    auto args = make_args({"/some/path"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->path, std::filesystem::path("/some/path"));
    EXPECT_TRUE(result->actions.empty());
}

TEST(Parse, SummaryFlag)
{
    auto args = make_args({"/path", "--summary"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->actions.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<SummaryAction>(result->actions[0]));
}

TEST(Parse, LargestFilesFlag)
{
    auto args = make_args({"/path", "--largest-files", "10"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->actions.size(), 1u);
    EXPECT_EQ(std::get<LargestFilesAction>(result->actions[0]).n, 10u);
}

TEST(Parse, RecentFlag)
{
    auto args = make_args({"/path", "--recent", "2h"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->actions.size(), 1u);
    EXPECT_EQ(std::get<RecentAction>(result->actions[0]).duration, std::chrono::seconds(7200));
}

TEST(Parse, MultipleFlags)
{
    auto args = make_args({"/path", "--summary", "--extensions", "--symlinks"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->actions.size(), 3u);
    EXPECT_EQ(result->path, std::filesystem::path("/path"));
}

TEST(Parse, UnknownFlagIsError)
{
    auto args = make_args({"/path", "--unknown"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
    ASSERT_EQ(result.error().size(), 1u);
    EXPECT_NE(result.error()[0].find("Unknown option"), std::string::npos);
}

TEST(Parse, MultipleUnknownFlagsAreCollected)
{
    auto args = make_args({"/path", "--foo", "--bar"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().size(), 2u);
}

TEST(Parse, LargestFilesMissingValueIsError)
{
    auto args = make_args({"/path", "--largest-files"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
    EXPECT_FALSE(result.error().empty());
}

TEST(Parse, LargestFilesZeroIsError)
{
    auto args = make_args({"/path", "--largest-files", "0"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
}

TEST(Parse, RecentInvalidTimeIsError)
{
    auto args = make_args({"/path", "--recent", "abc"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
}

TEST(Parse, MixedValidAndUnknownFlagsReturnsError)
{
    // Even though --summary succeeds, --unknown makes the whole parse fail
    auto args = make_args({"/path", "--summary", "--unknown"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().size(), 1u);
}

TEST(Parse, AllNoValueFlagsProduceCorrectActionTypes)
{
    auto args = make_args({"/path", "--extensions", "--empty-dirs", "--symlinks", "--errors", "--metrics", "--stats"});
    auto result = parse(std::span<const std::string_view>(args));
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->actions.size(), 6u);
    EXPECT_TRUE(std::holds_alternative<ExtensionsAction>(result->actions[0]));
    EXPECT_TRUE(std::holds_alternative<EmptyDirsAction>(result->actions[1]));
    EXPECT_TRUE(std::holds_alternative<SymlinksAction>(result->actions[2]));
    EXPECT_TRUE(std::holds_alternative<ErrorsAction>(result->actions[3]));
    EXPECT_TRUE(std::holds_alternative<MetricsAction>(result->actions[4]));
    EXPECT_TRUE(std::holds_alternative<StatsAction>(result->actions[5]));
}
