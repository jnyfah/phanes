#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>

import view;

// ============================================================
// format_size
// ============================================================

TEST(FormatSize, ZeroBytes)
{
    EXPECT_EQ(format_size(0), "0 B");
}

TEST(FormatSize, SubKiloByte)
{
    EXPECT_EQ(format_size(512), "512 B");
}

TEST(FormatSize, ExactlyOneKB)
{
    EXPECT_EQ(format_size(1024), "1 KB");
}

TEST(FormatSize, FractionalKB)
{
    // 1536 bytes = 1.5 KB
    EXPECT_EQ(format_size(1536), "1.5 KB");
}

TEST(FormatSize, ExactlyOneMB)
{
    EXPECT_EQ(format_size(1024ULL * 1024), "1 MB");
}

TEST(FormatSize, ExactlyOneGB)
{
    EXPECT_EQ(format_size(1024ULL * 1024 * 1024), "1 GB");
}

TEST(FormatSize, ExactlyOneTB)
{
    EXPECT_EQ(format_size(1024ULL * 1024 * 1024 * 1024), "1 TB");
}

TEST(FormatSize, RoundsToTwoDecimalPlaces)
{
    // 1500 bytes = 1.46484... KB → rounds to 1.46
    EXPECT_EQ(format_size(1500), "1.46 KB");
}

TEST(FormatSize, UnitSuffixAlwaysPresent)
{
    auto result = format_size(999);
    EXPECT_NE(result.find("B"), std::string::npos);
}

// ============================================================
// format_duration
// ============================================================

TEST(FormatDuration, ZeroSeconds)
{
    EXPECT_EQ(format_duration(std::chrono::seconds(0)), "0s");
}

TEST(FormatDuration, Seconds)
{
    EXPECT_EQ(format_duration(std::chrono::seconds(30)), "30s");
}

TEST(FormatDuration, ExactlyOneMinute)
{
    // 60s → 1m, seconds = 0 → seconds block skipped
    auto result = format_duration(std::chrono::seconds(60));
    EXPECT_NE(result.find("1m"), std::string::npos);
    EXPECT_EQ(result.find('s'), std::string::npos);
}

TEST(FormatDuration, MinutesAndSeconds)
{
    auto result = format_duration(std::chrono::seconds(90));
    EXPECT_NE(result.find("1m"), std::string::npos);
    EXPECT_NE(result.find("30s"), std::string::npos);
}

TEST(FormatDuration, ExactlyOneHour)
{
    auto result = format_duration(std::chrono::seconds(3600));
    EXPECT_NE(result.find("1h"), std::string::npos);
    EXPECT_EQ(result.find('m'), std::string::npos);
    EXPECT_EQ(result.find('s'), std::string::npos);
}

TEST(FormatDuration, HoursMinutesSeconds)
{
    // 3661 = 1h 1m 1s
    auto result = format_duration(std::chrono::seconds(3661));
    EXPECT_NE(result.find("1h"), std::string::npos);
    EXPECT_NE(result.find("1m"), std::string::npos);
    EXPECT_NE(result.find("1s"), std::string::npos);
}

TEST(FormatDuration, ExactlyOneDay)
{
    auto result = format_duration(std::chrono::seconds(86400));
    EXPECT_NE(result.find("1d"), std::string::npos);
    EXPECT_EQ(result.find('h'), std::string::npos);
    EXPECT_EQ(result.find('m'), std::string::npos);
    EXPECT_EQ(result.find('s'), std::string::npos);
}

TEST(FormatDuration, DaysHoursMinutesSeconds)
{
    // 86400 + 3661 = 1d 1h 1m 1s
    auto result = format_duration(std::chrono::seconds(86400 + 3661));
    EXPECT_NE(result.find("1d"), std::string::npos);
    EXPECT_NE(result.find("1h"), std::string::npos);
    EXPECT_NE(result.find("1m"), std::string::npos);
    EXPECT_NE(result.find("1s"), std::string::npos);
}
