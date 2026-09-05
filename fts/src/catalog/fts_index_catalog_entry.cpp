#include "catalog/fts_index_catalog_entry.h"

#include "catalog/catalog.h"
#include "common/serializer/buffer_reader.h"
#include "common/serializer/buffer_writer.h"
#include "common/string_utils.h"
#include "transaction/transaction.h"
#include "utils/fts_utils.h"
#include <format>

namespace lbug {
namespace fts_extension {

std::shared_ptr<common::BufferWriter> FTSIndexAuxInfo::serialize() const {
    auto bufferWriter = std::make_shared<common::BufferWriter>();
    auto serializer = common::Serializer(bufferWriter);
    config.serialize(serializer);
    return bufferWriter;
}

std::unique_ptr<FTSIndexAuxInfo> FTSIndexAuxInfo::deserialize(
    std::unique_ptr<common::BufferReader> reader) {
    common::Deserializer deserializer{std::move(reader)};
    auto config = FTSConfig::deserialize(deserializer);
    return std::make_unique<FTSIndexAuxInfo>(std::move(config));
}

std::string FTSIndexAuxInfo::getStopWordsName(const common::FileScanInfo& exportFileInfo) const {
    std::string stopWordsName = "default";
    switch (exportFileInfo.fileTypeInfo.fileType) {
    case common::FileType::UNKNOWN: {
        stopWordsName = config.stopWordsSource;
    } break;
    case common::FileType::CSV:
    case common::FileType::PARQUET: {
        if (config.stopWordsTableName != FTSUtils::getDefaultStopWordsTableName()) {
            stopWordsName =
                std::format("{}/{}.{}", exportFileInfo.filePaths[0], config.stopWordsTableName,
                    common::StringUtils::getLower(exportFileInfo.fileTypeInfo.fileTypeStr));
        }
    } break;
    default:
        UNREACHABLE_CODE;
    }
    return stopWordsName;
}

// True if the stopwords were bound from a node table (rather than a file). Such tables are
// exported as regular node tables in schema.cypher, so index.cypher can reference them by
// name instead of the legacy per-index parquet file.
static bool stopWordsIsTable(const FTSConfig& config, const main::ClientContext* context) {
    if (config.stopWordsTableName == FTSUtils::getDefaultStopWordsTableName()) {
        return false;
    }
    auto catalog = catalog::Catalog::Get(*context);
    return catalog->containsTable(transaction::Transaction::Get(*context), config.stopWordsSource);
}

std::string FTSIndexAuxInfo::toCypher(const catalog::IndexCatalogEntry& indexEntry,
    const catalog::ToCypherInfo& info) const {
    auto& indexToCypherInfo = info.constCast<catalog::IndexToCypherInfo>();
    std::string cypher;
    auto catalog = catalog::Catalog::Get(*indexToCypherInfo.context);
    auto transaction = transaction::Transaction::Get(*indexToCypherInfo.context);
    auto tableCatalogEntry = catalog->getTableCatalogEntry(transaction, indexEntry.getTableID());
    auto tableName = tableCatalogEntry->getName();
    std::string propertyStr;
    auto propertyIDs = indexEntry.getPropertyIDs();
    for (auto i = 0u; i < propertyIDs.size(); i++) {
        propertyStr +=
            std::format("'{}'{}", tableCatalogEntry->getProperty(propertyIDs[i]).getName(),
                i == propertyIDs.size() - 1 ? "" : ", ");
    }

    cypher += std::format("CALL CREATE_FTS_INDEX('{}', '{}', [{}], stemmer := '{}', "
                          "stopWords := '{}');",
        tableName, indexEntry.getIndexName(), std::move(propertyStr), config.stemmer,
        stopWordsIsTable(config, indexToCypherInfo.context) ?
            config.stopWordsSource :
            getStopWordsName(indexToCypherInfo.exportFileInfo));
    return cypher;
}

catalog::TableCatalogEntry* FTSIndexAuxInfo::getTableEntryToExport(
    const main::ClientContext* context) const {
    if (config.stopWordsTableName == FTSUtils::getDefaultStopWordsTableName()) {
        return nullptr;
    }
    auto transaction = transaction::Transaction::Get(*context);
    // Stopwords tables are created as internal tables by CREATE_FTS_INDEX.
    return catalog::Catalog::Get(*context)->getTableCatalogEntry(transaction,
        config.stopWordsTableName, true /* useInternal */);
}

} // namespace fts_extension
} // namespace lbug
