#pragma once

#include "hnsw_config.h"
#include "storage/table/list_chunk_data.h"

namespace lbug {
namespace catalog {
class TableCatalogEntry;
class NodeTableCatalogEntry;
} // namespace catalog

namespace main {
class ClientContext;
}

namespace vector_extension {
template<typename T>
concept VectorElementType = std::is_floating_point_v<T>;
using metric_func_t = std::function<double(const void*, const void*, uint32_t)>;

struct QuantizedEmbeddingView {
    const uint8_t* payload;
    float scale;
    float normSq;
};

struct HNSWIndexUtils {
    enum class IndexOperation { CREATE, QUERY, DROP };

    static bool indexExists(const main::ClientContext& context,
        const transaction::Transaction* transaction, const catalog::TableCatalogEntry* tableEntry,
        const std::string& indexName);

    static bool validateIndexExistence(const main::ClientContext& context,
        const catalog::TableCatalogEntry* tableEntry, const std::string& indexName,
        IndexOperation indexOperation,
        common::ConflictAction conflictAction = common::ConflictAction::ON_CONFLICT_THROW);

    static catalog::NodeTableCatalogEntry* bindNodeTable(const main::ClientContext& context,
        const std::string& tableName);

    static void validateAutoTransaction(const main::ClientContext& context,
        const std::string& funcName);

    static metric_func_t getMetricsFunction(MetricType metric, const common::LogicalType& type,
        QuantizationType quantization = QuantizationType::NONE);
    static metric_func_t getQuantizedCachedMetricsFunction(MetricType metric,
        QuantizationType quantization);
    static uint64_t getQuantizedEmbeddingPayloadOffset(QuantizationType quantization);
    static uint64_t getQuantizedEmbeddingStride(uint64_t dimension,
        QuantizationType quantization);
    static uint64_t getQuantizedCachedEmbeddingStride(uint64_t dimension,
        QuantizationType quantization);
    static uint64_t getQuantizedEmbeddingPayloadBytes(uint64_t dimension,
        QuantizationType quantization);
    static uint64_t getQuantizedCachedEmbeddingPayloadBytes(uint64_t dimension,
        QuantizationType quantization);

    static void validateColumnType(const catalog::TableCatalogEntry& tableEntry,
        const std::string& columnName);

    static std::string getUpperGraphTableName(common::table_id_t tableID,
        const std::string& indexName) {
        return "_" + std::to_string(tableID) + "_" + indexName + "_UPPER";
    }
    static std::string getLowerGraphTableName(common::table_id_t tableID,
        const std::string& indexName) {
        return "_" + std::to_string(tableID) + "_" + indexName + "_LOWER";
    }
    static std::string getQuantizedEmbeddingsTableName(common::table_id_t tableID,
        const std::string& indexName) {
        return "_" + std::to_string(tableID) + "_" + indexName + "_QEMB";
    }

    template<typename T>
    static T* getEmbedding(const storage::ColumnChunkData& embeddings, common::offset_t offset,
        common::length_t dimension) {
        auto& listChunk = embeddings.cast<storage::ListChunkData>();
        return &listChunk.getDataColumnChunk()->getData<T>()[offset * dimension];
    }

private:
    static void validateColumnType(const common::LogicalType& type);
};

} // namespace vector_extension
} // namespace lbug
