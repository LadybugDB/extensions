#include "storage/lance_reader.h"

#include "common/exception/binder.h"
#include "common/exception/runtime.h"
#include <format>

namespace lbug {
namespace lance_extension {

using namespace common;

std::string LanceReader::toDuckDBCastSpelling(const LogicalType& type) {
    switch (type.getLogicalTypeID()) {
    case LogicalTypeID::BOOL:
        return "BOOLEAN";
    case LogicalTypeID::INT8:
        return "TINYINT";
    case LogicalTypeID::INT16:
        return "SMALLINT";
    case LogicalTypeID::INT32:
        return "INTEGER";
    case LogicalTypeID::INT64:
        return "BIGINT";
    case LogicalTypeID::UINT8:
        return "UTINYINT";
    case LogicalTypeID::UINT16:
        return "USMALLINT";
    case LogicalTypeID::UINT32:
        return "UINTEGER";
    case LogicalTypeID::UINT64:
        return "UBIGINT";
    case LogicalTypeID::INT128:
        return "HUGEINT";
    case LogicalTypeID::UINT128:
        return "UHUGEINT";
    case LogicalTypeID::FLOAT:
        return "FLOAT";
    case LogicalTypeID::DOUBLE:
        return "DOUBLE";
    case LogicalTypeID::STRING:
        return "VARCHAR";
    case LogicalTypeID::BLOB:
        return "BLOB";
    case LogicalTypeID::DATE:
        return "DATE";
    case LogicalTypeID::TIMESTAMP:
        return "TIMESTAMP";
    case LogicalTypeID::TIMESTAMP_SEC:
        return "TIMESTAMP_S";
    case LogicalTypeID::TIMESTAMP_MS:
        return "TIMESTAMP_MS";
    case LogicalTypeID::TIMESTAMP_NS:
        return "TIMESTAMP_NS";
    case LogicalTypeID::TIMESTAMP_TZ:
        return "TIMESTAMPTZ";
    case LogicalTypeID::INTERVAL:
        return "INTERVAL";
    case LogicalTypeID::UUID:
        return "UUID";
    case LogicalTypeID::DECIMAL:
        return std::format("DECIMAL({},{})", DecimalType::getPrecision(type),
            DecimalType::getScale(type));
    case LogicalTypeID::LIST:
        return toDuckDBCastSpelling(ListType::getChildType(type)) + "[]";
    case LogicalTypeID::ARRAY:
        return std::format("{}[{}]", toDuckDBCastSpelling(ArrayType::getChildType(type)),
            ArrayType::getNumElements(type));
    case LogicalTypeID::STRUCT:
    case LogicalTypeID::MAP:
    case LogicalTypeID::UNION:
        throw BinderException{std::format(
            "Lance-backed tables do not support {} columns in v1. Flatten nested objects into "
            "top-level columns (see docs/lance.md).",
            LogicalTypeUtils::toString(type.getLogicalTypeID()))};
    default:
        throw BinderException{std::format(
            "Lance-backed tables do not support column type {} in v1.", type.toString())};
    }
}

LanceReader::LanceReader(std::shared_ptr<LanceConnector> connector_, std::string datasetPath_,
    std::vector<ProjectedColumn> projection_, main::ClientContext* context_)
    : connector{std::move(connector_)}, datasetPath{std::move(datasetPath_)},
      projection{std::move(projection_)}, context{context_}, converter{[this] {
          std::vector<LogicalType> types;
          types.reserve(projection.size());
          for (auto& col : projection) {
              types.push_back(col.ladybugType.copy());
          }
          return types;
      }()} {}

std::string LanceReader::buildRangeQuery(uint64_t offset, uint64_t limit) const {
    std::string selectList;
    if (projection.empty()) {
        // No property columns requested (e.g. COUNT over node IDs, which are positional):
        // read one arbitrary column and discard the values; only the row count matters.
        // The caller substitutes a surrogate projection (see LanceNodeTable::scanInternal).
        throw RuntimeException{"LanceReader requires a non-empty projection"};
    }
    for (auto i = 0u; i < projection.size(); ++i) {
        if (i > 0) {
            selectList += ", ";
        }
        selectList += std::format("CAST({} AS {})",
            LanceConnector::quoteIdent(projection[i].datasetName), projection[i].castSpelling);
    }
    auto query =
        std::format("SELECT {} FROM '{}'", selectList, LanceConnector::escapeLiteral(datasetPath));
    if (offset > 0) {
        query += std::format(" OFFSET {}", offset);
    }
    if (limit != UINT64_MAX) {
        query += std::format(" LIMIT {}", limit);
    }
    return query;
}

void LanceReader::openRange(uint64_t offset, uint64_t limit) {
    if (!connection) {
        connection = connector->newConnection(context);
    }
    activeResult = connector->executeOn(*connection, buildRangeQuery(offset, limit));
}

uint64_t LanceReader::next(DataChunk& out) {
    if (!activeResult) {
        return 0;
    }
    std::unique_ptr<duckdb::DataChunk> chunk;
    try {
        chunk = activeResult->Fetch();
    } catch (std::exception& e) {
        throw RuntimeException{
            std::string{"Lance scan failed for '"} + datasetPath + "': " + e.what()};
    }
    if (!chunk || chunk->size() == 0) {
        activeResult.reset();
        return 0;
    }
    converter.convertDuckDBResultToVector(*chunk, out);
    return out.state->getSelVector().getSelSize();
}

std::vector<LanceDatasetColumn> LanceReader::fetchSchema(LanceConnector& connector,
    main::ClientContext* context, const std::string& datasetPath) {
    auto conn = connector.newConnection(context);
    auto result = connector.executeOn(*conn, LanceConnector::schemaQuery(datasetPath));
    const auto& types = result->GetTypes();
    const auto& names = result->GetNames();
    if (types.size() != names.size()) {
        throw RuntimeException{"Failed to read Lance dataset schema for '" + datasetPath + "'"};
    }
    std::vector<LanceDatasetColumn> schema;
    for (auto i = 0u; i < types.size(); ++i) {
        const auto typeStr = types[i].ToString();
        schema.push_back(LanceDatasetColumn{names[i].GetIdentifierName(), typeStr,
            duckdb_extension::DuckDBTypeConverter::convertDuckDBType(typeStr)});
    }
    return schema;
}

uint64_t LanceReader::countRows(LanceConnector& connector, main::ClientContext* context,
    const std::string& datasetPath) {
    auto conn = connector.newConnection(context);
    auto result = connector.executeOn(*conn, LanceConnector::countRowsQuery(datasetPath));
    auto chunk = result->Fetch();
    if (!chunk || chunk->size() == 0) {
        throw RuntimeException{"Failed to count rows of Lance dataset '" + datasetPath + "'"};
    }
    // COUNT(*) returns BIGINT.
    return static_cast<uint64_t>(chunk->GetValue(0, 0).GetValue<int64_t>());
}

} // namespace lance_extension
} // namespace lbug
