#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

import core;
import builder;
import analyzer;

namespace fs = std::filesystem;

// ============================================================
// RAII temp directory — removed recursively on destruction
// ============================================================

struct TempDir
{
    fs::path path;

    TempDir()
    {
        std::random_device rd;
        std::uniform_int_distribution<std::uint64_t> dist;
        do
        {
            path = fs::temp_directory_path() / std::format("phanes_test_{:016x}", dist(rd));
        } while (!fs::create_directory(path));
    }

    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    // Write a file with the given content (default empty)
    fs::path make_file(const fs::path& rel, std::string content = "x") const
    {
        auto full = path / rel;
        fs::create_directories(full.parent_path());
        std::ofstream(full) << content;
        return full;
    }

    fs::path make_dir(const fs::path& rel) const
    {
        auto full = path / rel;
        fs::create_directories(full);
        return full;
    }

    fs::path make_symlink(const fs::path& rel, const fs::path& target) const
    {
        auto full = path / rel;
        fs::create_symlink(target, full);
        return full;
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// ============================================================
// Helpers to search the tree by path (IDs are non-deterministic
// in concurrent builds, so we never assert on them directly)
// ============================================================

static const FileNode* find_file(const DirectoryTree& tree, const fs::path& p)
{
    for (const auto& f : tree.files)
        if (f.path == p)
            return &f;
    return nullptr;
}

static const DirectoryNode* find_dir(const DirectoryTree& tree, const fs::path& p)
{
    for (const auto& d : tree.directories)
        if (d.path == p)
            return &d;
    return nullptr;
}

// ============================================================
// Empty directory
// ============================================================

TEST(BuildTree, EmptyDirectory)
{
    TempDir tmp;
    auto tree = build_tree(tmp.path);

    ASSERT_TRUE(tree.root.has_value());
    EXPECT_EQ(tree.directories.size(), 1u);
    EXPECT_TRUE(tree.files.empty());
    EXPECT_TRUE(tree.errors.empty());

    EXPECT_EQ(tree.directories[*tree.root].path, fs::weakly_canonical(tmp.path));
    EXPECT_FALSE(tree.directories[*tree.root].parent.has_value());
}

// ============================================================
// Flat directory with files
// ============================================================

TEST(BuildTree, FlatDirectoryWithFiles)
{
    TempDir tmp;
    auto a = tmp.make_file("a.txt", "hello");
    auto b = tmp.make_file("b.txt", "world!");

    auto tree = build_tree(tmp.path);

    EXPECT_EQ(tree.directories.size(), 1u);
    EXPECT_EQ(tree.files.size(), 2u);
    EXPECT_TRUE(tree.errors.empty());

    auto* fa = find_file(tree, a);
    auto* fb = find_file(tree, b);
    ASSERT_NE(fa, nullptr);
    ASSERT_NE(fb, nullptr);

    EXPECT_EQ(fa->size, 5u); // "hello"
    EXPECT_EQ(fb->size, 6u); // "world!"
    EXPECT_FALSE(fa->is_symlink);
    EXPECT_FALSE(fb->is_symlink);
}

// ============================================================
// Nested structure — parent/child relationships
// ============================================================

TEST(BuildTree, NestedDirectoryStructure)
{
    TempDir tmp;
    tmp.make_dir("sub");
    auto f = tmp.make_file("sub/child.txt", "data");

    auto tree = build_tree(tmp.path);

    EXPECT_EQ(tree.directories.size(), 2u);
    EXPECT_EQ(tree.files.size(), 1u);
    EXPECT_TRUE(tree.errors.empty());

    auto* root = find_dir(tree, fs::weakly_canonical(tmp.path));
    auto* sub = find_dir(tree, fs::weakly_canonical(tmp.path / "sub"));
    ASSERT_NE(root, nullptr);
    ASSERT_NE(sub, nullptr);

    // sub's parent is root
    ASSERT_TRUE(sub->parent.has_value());
    EXPECT_EQ(*sub->parent, root->id);

    // root's subdirs contains sub's id
    auto& rootSubdirs = root->subdirs;
    EXPECT_NE(std::find(rootSubdirs.begin(), rootSubdirs.end(), sub->id), rootSubdirs.end());

    // the file is under sub
    auto* fc = find_file(tree, f);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->parent, sub->id);
}

// ============================================================
// Symlink — flagged and not sized
// ============================================================

TEST(BuildTree, SymlinkIsMarkedAndNotSized)
{
    TempDir tmp;
    auto target = tmp.make_file("real.txt", "1234567890"); // 10 bytes
    auto link = tmp.make_symlink("link.txt", target);

    auto tree = build_tree(tmp.path);

    auto* real = find_file(tree, target);
    auto* sym = find_file(tree, link);
    ASSERT_NE(real, nullptr);
    ASSERT_NE(sym, nullptr);

    EXPECT_FALSE(real->is_symlink);
    EXPECT_EQ(real->size, 10u);

    EXPECT_TRUE(sym->is_symlink);
    EXPECT_EQ(sym->size, 0u); // builder skips file_size() for symlinks
}

// ============================================================
// Non-existent path → error, no root
// ============================================================

TEST(BuildTree, NonExistentPath)
{
    auto tree = build_tree("/this/path/does/not/exist_phanes_test");

    EXPECT_FALSE(tree.root.has_value());
    EXPECT_TRUE(tree.directories.empty());
    EXPECT_TRUE(tree.files.empty());
    ASSERT_EQ(tree.errors.size(), 1u);
    EXPECT_EQ(tree.errors[0].kind, ErrorKind::NotFound);
    EXPECT_EQ(tree.errors[0].node_kind, NodeKind::Directory);
}

// ============================================================
// File path passed as root → IOError, no root
// ============================================================

TEST(BuildTree, FilePassedAsRoot)
{
    TempDir tmp;
    auto f = tmp.make_file("not_a_dir.txt");

    auto tree = build_tree(f);

    EXPECT_FALSE(tree.root.has_value());
    EXPECT_TRUE(tree.directories.empty());
    EXPECT_TRUE(tree.files.empty());
    ASSERT_EQ(tree.errors.size(), 1u);
    EXPECT_EQ(tree.errors[0].kind, ErrorKind::IOError);
}

// ============================================================
// Unreadable subdir, silently gets no children (not an error),
// because the builder uses skip_permission_denied.
// Skip when running as root since chmod has no effect then.
// ============================================================

TEST(BuildTree, UnreadableSubdirHasNoChildren)
{
#ifdef _WIN32
    GTEST_SKIP() << "Permission tests not supported on Windows";
#else
    if (getuid() == 0)
    {
        GTEST_SKIP() << "Running as root; permission checks are bypassed";
    }
#endif

    TempDir tmp;
    auto locked = tmp.make_dir("locked");
    tmp.make_file("locked/secret.txt", "hidden");
    fs::permissions(locked, fs::perms::none);

    auto tree = build_tree(tmp.path);

    // Restore so TempDir destructor can clean up
    fs::permissions(locked, fs::perms::all);

    // The locked dir should appear in the tree but have no children
    auto* d = find_dir(tree, fs::weakly_canonical(locked));
    ASSERT_NE(d, nullptr);
    EXPECT_TRUE(d->files.empty());
    EXPECT_TRUE(d->subdirs.empty());

    // The file inside is invisible — not scanned
    EXPECT_EQ(tree.files.size(), 0u);

    // No error recorded — skip_permission_denied swallows it silently
    EXPECT_TRUE(tree.errors.empty());
}

// ============================================================
// scan_started <= scan_finished
// ============================================================

TEST(BuildTree, ScanTimestampsAreOrdered)
{
    TempDir tmp;
    auto tree = build_tree(tmp.path);
    EXPECT_LE(tree.scan_started, tree.scan_finished);
}

// ============================================================
// Deeply nested tree — 3 levels, all dirs and files present
// ============================================================

TEST(BuildTree, DeeplyNestedThreeLevels)
{
    TempDir tmp;
    tmp.make_dir("l1/l2/l3");
    auto leaf_file = tmp.make_file("l1/l2/l3/leaf.txt", "deep");

    auto tree = build_tree(tmp.path);

    EXPECT_EQ(tree.directories.size(), 4u); // root, l1, l2, l3
    EXPECT_EQ(tree.files.size(), 1u);
    EXPECT_TRUE(tree.errors.empty());

    auto* root = find_dir(tree, fs::weakly_canonical(tmp.path));
    auto* l1 = find_dir(tree, fs::weakly_canonical(tmp.path / "l1"));
    auto* l2 = find_dir(tree, fs::weakly_canonical(tmp.path / "l1/l2"));
    auto* l3 = find_dir(tree, fs::weakly_canonical(tmp.path / "l1/l2/l3"));
    ASSERT_NE(root, nullptr);
    ASSERT_NE(l1, nullptr);
    ASSERT_NE(l2, nullptr);
    ASSERT_NE(l3, nullptr);

    // parent-child chain
    EXPECT_FALSE(root->parent.has_value());
    ASSERT_TRUE(l1->parent.has_value());
    EXPECT_EQ(*l1->parent, root->id);
    ASSERT_TRUE(l2->parent.has_value());
    EXPECT_EQ(*l2->parent, l1->id);
    ASSERT_TRUE(l3->parent.has_value());
    EXPECT_EQ(*l3->parent, l2->id);

    // leaf file is under l3
    auto* f = find_file(tree, leaf_file);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->parent, l3->id);

    // depth propagation through compute_directory_metrics
    auto metrics = compute_directory_metrics(tree);
    EXPECT_EQ(metrics.depth[l3->id], 3u);
}

// ============================================================
// Multiple subdirs each with files — concurrent flush path
// ============================================================

TEST(BuildTree, MultipleSubdirsWithFiles)
{
    TempDir tmp;
    auto fa = tmp.make_file("a/one.txt", "aaa");
    auto fb = tmp.make_file("b/two.txt", "bb");
    auto fc = tmp.make_file("c/three.txt", "c");

    auto tree = build_tree(tmp.path);

    EXPECT_EQ(tree.directories.size(), 4u); // root + a, b, c
    EXPECT_EQ(tree.files.size(), 3u);
    EXPECT_TRUE(tree.errors.empty());

    auto* f1 = find_file(tree, fa);
    auto* f2 = find_file(tree, fb);
    auto* f3 = find_file(tree, fc);
    ASSERT_NE(f1, nullptr);
    EXPECT_EQ(f1->size, 3u);
    ASSERT_NE(f2, nullptr);
    EXPECT_EQ(f2->size, 2u);
    ASSERT_NE(f3, nullptr);
    EXPECT_EQ(f3->size, 1u);

    // each file is inside the correct subdir
    auto* da = find_dir(tree, fs::weakly_canonical(tmp.path / "a"));
    auto* db = find_dir(tree, fs::weakly_canonical(tmp.path / "b"));
    auto* dc = find_dir(tree, fs::weakly_canonical(tmp.path / "c"));
    ASSERT_NE(da, nullptr);
    EXPECT_EQ(f1->parent, da->id);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(f2->parent, db->id);
    ASSERT_NE(dc, nullptr);
    EXPECT_EQ(f3->parent, dc->id);
}
