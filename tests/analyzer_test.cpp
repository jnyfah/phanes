#include <chrono>
#include <deque>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

import core;
import analyzer;

// ============================================================
// Tree construction helpers
// ============================================================

using TimePoint = std::chrono::sys_time<std::chrono::seconds>;

static TimePoint now_sec()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

struct TreeBuilder
{
    DirectoryTree tree;
    TimePoint _now;

    TreeBuilder()
    {
        _now = now_sec();
        tree.scan_started = _now;
        tree.scan_finished = _now + std::chrono::seconds(1);
    }

    void add_root(std::filesystem::path path = "/test")
    {
        DirectoryNode root;
        root.id = tree.directories.size();
        root.parent = std::nullopt;
        root.path = std::move(path);
        tree.root = root.id;
        tree.directories.push_back(std::move(root));
    }

    DirectoryId add_dir(DirectoryId parent_id, std::filesystem::path path)
    {
        DirectoryId id = tree.directories.size();
        DirectoryNode dir;
        dir.id = id;
        dir.parent = parent_id;
        dir.path = std::move(path);
        // deque::push_back does not invalidate references to existing elements
        tree.directories[parent_id].subdirs.push_back(id);
        tree.directories.push_back(std::move(dir));
        return id;
    }

    FileId add_file(DirectoryId parent_id,
                    std::filesystem::path path,
                    std::uintmax_t size,
                    std::optional<TimePoint> modified = std::nullopt,
                    bool is_symlink = false)
    {
        FileId id = tree.files.size();
        FileNode file;
        file.id = id;
        file.parent = parent_id;
        file.path = std::move(path);
        file.size = size;
        file.modified = modified.value_or(_now);
        file.is_symlink = is_symlink;
        tree.directories[parent_id].files.push_back(id);
        tree.files.push_back(std::move(file));
        return id;
    }

    DirectoryTree build() { return std::move(tree); }
};

// ============================================================
// compute_file_stats
// ============================================================

TEST(ComputeFileStats, EmptyTree)
{
    DirectoryTree tree;
    auto stats = compute_file_stats(tree);
    EXPECT_EQ(stats.total_size, 0u);
    EXPECT_EQ(stats.symlink_count, 0u);
    EXPECT_EQ(stats.largest_file_size, 0u);
    EXPECT_FALSE(stats.largest_file_id.has_value());
    EXPECT_TRUE(stats.symlink_ids.empty());
}

TEST(ComputeFileStats, SingleFile)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 1024);
    auto stats = compute_file_stats(b.build());
    EXPECT_EQ(stats.total_size, 1024u);
    EXPECT_EQ(stats.symlink_count, 0u);
    EXPECT_EQ(stats.largest_file_size, 1024u);
    ASSERT_TRUE(stats.largest_file_id.has_value());
    EXPECT_EQ(*stats.largest_file_id, 0u);
}

TEST(ComputeFileStats, TotalSizeIsSum)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100);
    b.add_file(0, "/test/b.txt", 200);
    b.add_file(0, "/test/c.txt", 300);
    auto stats = compute_file_stats(b.build());
    EXPECT_EQ(stats.total_size, 600u);
}

TEST(ComputeFileStats, LargestFileTracked)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/small.txt", 50);
    FileId big_id = b.add_file(0, "/test/big.txt", 9999);
    b.add_file(0, "/test/medium.txt", 500);
    auto stats = compute_file_stats(b.build());
    EXPECT_EQ(stats.largest_file_size, 9999u);
    ASSERT_TRUE(stats.largest_file_id.has_value());
    EXPECT_EQ(*stats.largest_file_id, big_id);
}

TEST(ComputeFileStats, SymlinkIsCounted)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/regular.txt", 100);
    FileId link_id = b.add_file(0, "/test/link.txt", 0, std::nullopt, /*is_symlink=*/true);
    auto stats = compute_file_stats(b.build());
    EXPECT_EQ(stats.symlink_count, 1u);
    ASSERT_EQ(stats.symlink_ids.size(), 1u);
    EXPECT_EQ(stats.symlink_ids[0], link_id);
}

// ============================================================
// compute_empty_directories
// ============================================================

TEST(ComputeEmptyDirectories, NoDirectories)
{
    DirectoryTree tree;
    auto result = compute_empty_directories(tree);
    EXPECT_TRUE(result.empty());
}

TEST(ComputeEmptyDirectories, RootWithFilesIsNotEmpty)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100);
    auto result = compute_empty_directories(b.build());
    EXPECT_TRUE(result.empty());
}

TEST(ComputeEmptyDirectories, RootWithNoFilesOrSubdirs)
{
    TreeBuilder b;
    b.add_root();
    auto result = compute_empty_directories(b.build());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 0u);
}

TEST(ComputeEmptyDirectories, EmptySubdirIsDetected)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100); // root is not empty
    DirectoryId sub = b.add_dir(0, "/test/sub"); // sub has no files or subdirs
    auto result = compute_empty_directories(b.build());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], sub);
}

TEST(ComputeEmptyDirectories, RootWithSubdirIsNotEmpty)
{
    TreeBuilder b;
    b.add_root();
    b.add_dir(0, "/test/sub");
    auto result = compute_empty_directories(b.build());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 1u); 
}

// ============================================================
// compute_largest_N_Files
// ============================================================

TEST(ComputeLargestNFiles, EmptyTree)
{
    DirectoryTree tree;
    auto result = compute_largest_N_Files(tree, 5);
    EXPECT_TRUE(result.empty());
}

TEST(ComputeLargestNFiles, NGreaterThanFileCount)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100);
    b.add_file(0, "/test/b.txt", 200);
    auto result = compute_largest_N_Files(b.build(), 10);
    EXPECT_EQ(result.size(), 2u);
}

TEST(ComputeLargestNFiles, ExactlyNFiles)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100);
    b.add_file(0, "/test/b.txt", 200);
    b.add_file(0, "/test/c.txt", 300);
    auto result = compute_largest_N_Files(b.build(), 3);
    EXPECT_EQ(result.size(), 3u);
}

TEST(ComputeLargestNFiles, ReturnedInDescendingOrder)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/small.txt", 100);
    FileId large = b.add_file(0, "/test/large.txt", 500);
    FileId medium = b.add_file(0, "/test/medium.txt", 300);
    auto tree = b.build();
    auto result = compute_largest_N_Files(tree, 2);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], large);
    EXPECT_EQ(result[1], medium);
}

TEST(ComputeLargestNFiles, NIsZeroReturnsEmpty)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100);
    auto result = compute_largest_N_Files(b.build(), 0);
    EXPECT_TRUE(result.empty());
}

// ============================================================
// compute_directory_metrics
// ============================================================

TEST(ComputeDirectoryMetrics, EmptyTree)
{
    DirectoryTree tree;
    auto metrics = compute_directory_metrics(tree);
    EXPECT_TRUE(metrics.depth.empty());
    EXPECT_TRUE(metrics.recursive_size.empty());
    EXPECT_TRUE(metrics.recursive_file_count.empty());
}

TEST(ComputeDirectoryMetrics, RootHasDepthZero)
{
    TreeBuilder b;
    b.add_root();
    auto metrics = compute_directory_metrics(b.build());
    ASSERT_EQ(metrics.depth.size(), 1u);
    EXPECT_EQ(metrics.depth[0], 0u);
}

TEST(ComputeDirectoryMetrics, SubdirHasDepthOne)
{
    TreeBuilder b;
    b.add_root();
    b.add_dir(0, "/test/sub");
    auto metrics = compute_directory_metrics(b.build());
    ASSERT_EQ(metrics.depth.size(), 2u);
    EXPECT_EQ(metrics.depth[0], 0u);
    EXPECT_EQ(metrics.depth[1], 1u);
}

TEST(ComputeDirectoryMetrics, RecursiveSizeRollsUpToRoot)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub = b.add_dir(0, "/test/sub");
    b.add_file(sub, "/test/sub/a.txt", 100);
    b.add_file(sub, "/test/sub/b.txt", 200);
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    EXPECT_EQ(metrics.recursive_size[sub], 300u);
    EXPECT_EQ(metrics.recursive_size[0], 300u); // root accumulates from subdir
}

TEST(ComputeDirectoryMetrics, RecursiveFileCountRollsUp)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub = b.add_dir(0, "/test/sub");
    b.add_file(sub, "/test/sub/a.txt", 100);
    b.add_file(sub, "/test/sub/b.txt", 200);
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    EXPECT_EQ(metrics.recursive_file_count[sub], 2u);
    EXPECT_EQ(metrics.recursive_file_count[0], 2u);
}

TEST(ComputeDirectoryMetrics, DeepNestingDepthPropagates)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId l1 = b.add_dir(0, "/test/l1");
    DirectoryId l2 = b.add_dir(l1, "/test/l1/l2");
    auto metrics = compute_directory_metrics(b.build());
    EXPECT_EQ(metrics.depth[l1], 1u);
    EXPECT_EQ(metrics.depth[l2], 2u);
}

// ============================================================
// compute_largest_N_Directories
// ============================================================

TEST(ComputeLargestNDirectories, OnlyRootReturnsEmpty)
{
    TreeBuilder b;
    b.add_root();
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto result = compute_largest_N_Directories(tree, metrics, 5);
    EXPECT_TRUE(result.empty());
}

TEST(ComputeLargestNDirectories, SubdirsSortedByRecursiveSize)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub1 = b.add_dir(0, "/test/sub1");
    DirectoryId sub2 = b.add_dir(0, "/test/sub2");
    b.add_file(sub1, "/test/sub1/a.txt", 100);
    b.add_file(sub2, "/test/sub2/b.txt", 500);
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto result = compute_largest_N_Directories(tree, metrics, 1);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], sub2);
}

TEST(ComputeLargestNDirectories, NGreaterThanSubdirCount)
{
    TreeBuilder b;
    b.add_root();
    b.add_dir(0, "/test/sub1");
    b.add_dir(0, "/test/sub2");
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto result = compute_largest_N_Directories(tree, metrics, 100);
    EXPECT_EQ(result.size(), 2u);
}

// ============================================================
// compute_recent_files
// ============================================================

TEST(ComputeRecentFiles, FileWithinWindowIsIncluded)
{
    TreeBuilder b;
    b.add_root();
    auto recent = b._now - std::chrono::seconds(5);
    FileId fid = b.add_file(0, "/test/recent.txt", 100, recent);
    auto result = compute_recent_files(b.build(), std::chrono::seconds(10));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], fid);
}

TEST(ComputeRecentFiles, FileOutsideWindowIsExcluded)
{
    TreeBuilder b;
    b.add_root();
    auto old = b._now - std::chrono::hours(2);
    b.add_file(0, "/test/old.txt", 100, old);
    auto result = compute_recent_files(b.build(), std::chrono::seconds(10));
    EXPECT_TRUE(result.empty());
}

TEST(ComputeRecentFiles, EmptyTree)
{
    DirectoryTree tree;
    auto result = compute_recent_files(tree, std::chrono::seconds(60));
    EXPECT_TRUE(result.empty());
}

TEST(ComputeRecentFiles, ResultSortedByModifiedDescending)
{
    TreeBuilder b;
    b.add_root();
    auto older = b._now - std::chrono::seconds(50);
    auto newer = b._now - std::chrono::seconds(10);
    FileId fid_older = b.add_file(0, "/test/older.txt", 100, older);
    FileId fid_newer = b.add_file(0, "/test/newer.txt", 100, newer);
    auto result = compute_recent_files(b.build(), std::chrono::seconds(100));
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], fid_newer);
    EXPECT_EQ(result[1], fid_older);
}

TEST(ComputeRecentFiles, MixOfRecentAndOld)
{
    TreeBuilder b;
    b.add_root();
    auto old = b._now - std::chrono::hours(1);
    b.add_file(0, "/test/old.txt", 100, old);
    FileId fid_new = b.add_file(0, "/test/new.txt", 100, b._now - std::chrono::seconds(5));
    auto result = compute_recent_files(b.build(), std::chrono::seconds(30));
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], fid_new);
}

// ============================================================
// compute_extension_stats
// ============================================================

TEST(ComputeExtensionStats, EmptyTree)
{
    DirectoryTree tree;
    auto result = compute_extension_stats(tree);
    EXPECT_TRUE(result.empty());
}

TEST(ComputeExtensionStats, SingleExtensionAggregated)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 100);
    b.add_file(0, "/test/b.txt", 200);
    auto result = compute_extension_stats(b.build());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].extension, ".txt");
    EXPECT_EQ(result[0].count, 2u);
    EXPECT_EQ(result[0].total_size, 300u);
}

TEST(ComputeExtensionStats, MultipleExtensionsSortedBySizeDesc)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/a.txt", 50);
    b.add_file(0, "/test/b.cpp", 1000);
    b.add_file(0, "/test/c.hpp", 200);
    auto result = compute_extension_stats(b.build());
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].extension, ".cpp");
    EXPECT_EQ(result[1].extension, ".hpp");
    EXPECT_EQ(result[2].extension, ".txt");
}

TEST(ComputeExtensionStats, ExtensionsAreLowercased)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/image.PNG", 500);
    b.add_file(0, "/test/photo.png", 300);
    auto result = compute_extension_stats(b.build());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].extension, ".png");
    EXPECT_EQ(result[0].count, 2u);
    EXPECT_EQ(result[0].total_size, 800u);
}

TEST(ComputeExtensionStats, FileWithNoExtension)
{
    TreeBuilder b;
    b.add_root();
    b.add_file(0, "/test/Makefile", 1000);
    b.add_file(0, "/test/LICENSE", 500);
    auto result = compute_extension_stats(b.build());
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].extension, "");
    EXPECT_EQ(result[0].count, 2u);
    EXPECT_EQ(result[0].total_size, 1500u);
}

// ============================================================
// compute_largest_N_Directories
// ============================================================

TEST(ComputeLargestNDirectories, NIsZeroReturnsEmpty)
{
    TreeBuilder b;
    b.add_root();
    b.add_dir(0, "/test/sub");
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto result = compute_largest_N_Directories(tree, metrics, 0);
    EXPECT_TRUE(result.empty());
}

// ============================================================
// get_errors
// ============================================================

TEST(GetErrors, EmptyTreeReturnsEmptyDeque)
{
    DirectoryTree tree;
    EXPECT_TRUE(get_errors(tree).empty());
}

TEST(GetErrors, ReturnsTreeErrors)
{
    DirectoryTree tree;
    tree.errors.push_back({"/some/path", ErrorKind::NotFound, NodeKind::File});
    tree.errors.push_back({"/other", ErrorKind::PermissionDenied, NodeKind::Directory});
    const auto& errs = get_errors(tree);
    ASSERT_EQ(errs.size(), 2u);
    EXPECT_EQ(errs[0].kind, ErrorKind::NotFound);
    EXPECT_EQ(errs[1].kind, ErrorKind::PermissionDenied);
}

// ============================================================
// compute_directory_stats
// ============================================================

TEST(ComputeDirectoryStats, EmptyMetrics)
{
    DirectoryTree tree;
    DirectoryMetrics metrics;
    auto stats = compute_directory_stats(tree, metrics);
    EXPECT_EQ(stats.max_depth, 0u);
    EXPECT_EQ(stats.max_files_count, 0u);
    EXPECT_DOUBLE_EQ(stats.average_directory_depth, 0.0);
    EXPECT_DOUBLE_EQ(stats.average_files_per_directory, 0.0);
}

TEST(ComputeDirectoryStats, MaxDepthIsDeepestNonRoot)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub = b.add_dir(0, "/test/sub");
    b.add_dir(sub, "/test/sub/deep");
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto stats = compute_directory_stats(tree, metrics);
    EXPECT_EQ(stats.max_depth, 2u); // /test/sub/deep is at depth 2
    EXPECT_EQ(stats.max_depth_dir, 2u);
}

TEST(ComputeDirectoryStats, MaxFilesCountDir)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub1 = b.add_dir(0, "/test/sub1");
    DirectoryId sub2 = b.add_dir(0, "/test/sub2");
    b.add_file(sub1, "/test/sub1/a.txt", 10);
    b.add_file(sub2, "/test/sub2/b.txt", 20);
    b.add_file(sub2, "/test/sub2/c.txt", 30);
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto stats = compute_directory_stats(tree, metrics);
    EXPECT_EQ(stats.max_files_count, 2u);
    EXPECT_EQ(stats.max_files_count_dir, sub2);
}

TEST(ComputeDirectoryStats, AverageDepthIsCorrect)
{
    TreeBuilder b;
    b.add_root();
    b.add_dir(0, "/test/a");
    b.add_dir(0, "/test/b");
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto stats = compute_directory_stats(tree, metrics);
    EXPECT_DOUBLE_EQ(stats.average_directory_depth, 1.0);
}

TEST(ComputeDirectoryStats, AverageFilesPerDirectory)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub1 = b.add_dir(0, "/test/sub1");
    b.add_dir(0, "/test/sub2");
    b.add_file(sub1, "/test/sub1/a.txt", 10);
    b.add_file(sub1, "/test/sub1/b.txt", 20);
    b.add_file(sub1, "/test/sub1/c.txt", 30);
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto stats = compute_directory_stats(tree, metrics);
    EXPECT_DOUBLE_EQ(stats.average_files_per_directory, 1.5);
}

// ============================================================
// compute_summary
// ============================================================

TEST(ComputeSummary, BasicCounts)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub = b.add_dir(0, "/test/sub");
    b.add_file(0, "/test/a.txt", 100);
    b.add_file(sub, "/test/sub/b.txt", 200);
    b.tree.errors.push_back({"/bad", ErrorKind::NotFound, NodeKind::File});
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto fs = compute_file_stats(tree);
    auto empty_dirs = compute_empty_directories(tree);
    auto report = compute_summary(tree, metrics, empty_dirs.size(), fs);

    EXPECT_EQ(report.total_directories, 2u);
    EXPECT_EQ(report.total_files, 2u);
    EXPECT_EQ(report.total_size, 300u);
    EXPECT_EQ(report.total_errors, 1u);
    EXPECT_EQ(report.total_empty_dir, 0u); // sub has a file, root has a file
    EXPECT_EQ(report.largest_file_size, 200u);
    ASSERT_TRUE(report.largest_file.has_value());
}

TEST(ComputeSummary, EmptyDirCountPassedThrough)
{
    TreeBuilder b;
    b.add_root();
    b.add_dir(0, "/test/empty");
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto fs = compute_file_stats(tree);
    auto report = compute_summary(tree, metrics, /*empty_dir=*/1, fs);
    EXPECT_EQ(report.total_empty_dir, 1u);
}

TEST(ComputeSummary, ScanDurationIsFinishedMinusStarted)
{
    TreeBuilder b;
    b.add_root();
    b.tree.scan_started = b._now;
    b.tree.scan_finished = b._now + std::chrono::seconds(42);
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto fs = compute_file_stats(tree);
    auto report = compute_summary(tree, metrics, 0, fs);
    EXPECT_EQ(report.total_duration, std::chrono::seconds(42));
}

TEST(ComputeSummary, MaxDepthFromMetrics)
{
    TreeBuilder b;
    b.add_root();
    DirectoryId sub = b.add_dir(0, "/test/sub");
    b.add_dir(sub, "/test/sub/deep");
    auto tree = b.build();
    auto metrics = compute_directory_metrics(tree);
    auto fs = compute_file_stats(tree);
    auto report = compute_summary(tree, metrics, 0, fs);
    EXPECT_EQ(report.max_depth, 2u);
    EXPECT_EQ(report.max_depth_dir, "deep");
}
