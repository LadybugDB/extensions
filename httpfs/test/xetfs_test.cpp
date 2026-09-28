#include "gtest/gtest.h"
#include "xetfs.h"

using namespace lbug::httpfs_extension;

TEST(XetFileSystemTest, MapsModelResolveURL) {
    EXPECT_EQ("https://huggingface.co/Qwen/Qwen-Image-Edit/resolve/main/model.safetensors",
        XetFileSystem::toHuggingFaceURL("xet://models/Qwen/Qwen-Image-Edit/main/"
                                        "model.safetensors"));
}

TEST(XetFileSystemTest, MapsDatasetResolveURL) {
    EXPECT_EQ("https://huggingface.co/datasets/org/repo/resolve/main/data/train.parquet",
        XetFileSystem::toHuggingFaceURL("xet://datasets/org/repo/resolve/main/"
                                        "data/train.parquet"));
}

TEST(XetFileSystemTest, MapsExplicitHubURL) {
    EXPECT_EQ("https://huggingface.co/org/repo/resolve/main/file.csv",
        XetFileSystem::toHuggingFaceURL("xet://huggingface.co/org/repo/resolve/main/file.csv"));
}

TEST(XetFileSystemTest, GlobKeepsXetPath) {
    XetFileSystem fs;
    const auto path =
        std::string{"xet://datasets/ladybugdb/small-kgs/main/kg_history/icebug-disk/schema.cypher"};
    EXPECT_EQ(std::vector<std::string>{path}, fs.glob(nullptr, path));
}

TEST(XetFileSystemTest, HfSchemeAliasesXet) {
    EXPECT_EQ("https://huggingface.co/Qwen/Qwen-Image-Edit/resolve/main/model.safetensors",
        XetFileSystem::toHuggingFaceURL("hf://models/Qwen/Qwen-Image-Edit/main/"
                                        "model.safetensors"));
    EXPECT_EQ("https://huggingface.co/datasets/org/repo/resolve/main/data/train.parquet",
        XetFileSystem::toHuggingFaceURL("hf://datasets/org/repo/resolve/main/"
                                        "data/train.parquet"));
    EXPECT_EQ("https://huggingface.co/org/repo/resolve/main/file.csv",
        XetFileSystem::toHuggingFaceURL("hf://huggingface.co/org/repo/resolve/main/file.csv"));
}

TEST(XetFileSystemTest, CanHandleBothSchemes) {
    XetFileSystem fs;
    EXPECT_TRUE(fs.canHandleFile("xet://models/org/repo/main/file.parquet"));
    EXPECT_TRUE(fs.canHandleFile("hf://models/org/repo/main/file.parquet"));
    EXPECT_FALSE(fs.canHandleFile("https://huggingface.co/org/repo/resolve/main/file.csv"));
}

TEST(XetFileSystemTest, GlobKeepsHfPath) {
    XetFileSystem fs;
    const auto path =
        std::string{"hf://datasets/ladybugdb/small-kgs/main/kg_history/icebug-disk/schema.cypher"};
    EXPECT_EQ(std::vector<std::string>{path}, fs.glob(nullptr, path));
}
