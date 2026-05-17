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
