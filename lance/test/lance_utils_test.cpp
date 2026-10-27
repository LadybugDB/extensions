#include <string>

#include "gtest/gtest.h"
#include "storage/lance_utils.h"

using namespace lbug::lance_extension;

// ─────────────────────────────────────────────────────────────
// joinPath
// ─────────────────────────────────────────────────────────────
TEST(LanceUtils_JoinPath, EmptyBase) {
    EXPECT_EQ("file.lance", LanceUtils::joinPath("", "file.lance"));
}

TEST(LanceUtils_JoinPath, BaseWithoutTrailingSlash) {
    EXPECT_EQ("/base/file.lance", LanceUtils::joinPath("/base", "file.lance"));
}

TEST(LanceUtils_JoinPath, BaseWithTrailingSlash) {
    EXPECT_EQ("/base/file.lance", LanceUtils::joinPath("/base/", "file.lance"));
}

TEST(LanceUtils_JoinPath, S3URI) {
    EXPECT_EQ("s3://bucket/prefix/file.lance",
        LanceUtils::joinPath("s3://bucket/prefix", "file.lance"));
}

// ─────────────────────────────────────────────────────────────
// Layout paths
// ─────────────────────────────────────────────────────────────
TEST(LanceUtils_Paths, NodeTableInDir) {
    EXPECT_EQ("/data/nodes_user.lance", LanceUtils::constructNodeTablePath("/data", "user"));
}

TEST(LanceUtils_Paths, NodeTableSingleDataset) {
    EXPECT_EQ("/data/nodes_user.lance",
        LanceUtils::constructNodeTablePath("/data/nodes_user.lance", "user"));
}

TEST(LanceUtils_Paths, CSRPaths) {
    auto paths = LanceUtils::constructCSRPaths("/data", "follows");
    EXPECT_EQ("/data/indices_follows.lance", paths.indices);
    EXPECT_EQ("/data/indptr_follows.lance", paths.indptr);
}

TEST(LanceUtils_Paths, FlatRelTableInDir) {
    EXPECT_EQ("/data/rels_follows.lance",
        LanceUtils::constructFlatRelTablePath("/data", "follows"));
}

TEST(LanceUtils_Paths, FlatRelTableSingleDataset) {
    EXPECT_EQ("/data/rels.lance",
        LanceUtils::constructFlatRelTablePath("/data/rels.lance", "follows"));
}

TEST(LanceUtils_Paths, IsSingleDatasetPath) {
    EXPECT_TRUE(LanceUtils::isSingleDatasetPath("/data/nodes_user.lance"));
    EXPECT_TRUE(LanceUtils::isSingleDatasetPath("/data/nodes_user.LANCE"));
    EXPECT_FALSE(LanceUtils::isSingleDatasetPath("/data"));
    EXPECT_FALSE(LanceUtils::isSingleDatasetPath("s3://bucket/prefix"));
}
