#include "function/drop_fts_index.h"

#include "catalog/catalog.h"
#include "catalog/fts_index_catalog_entry.h"
#include "function/fts_bind_data.h"
#include "function/fts_index_utils.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "processor/execution_context.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "utils/fts_utils.h"
#include <format>

namespace lbug {
namespace fts_extension {

using namespace lbug::common;
using namespace lbug::main;
using namespace lbug::function;

static std::unique_ptr<TableFuncBindData> bindFunc(ClientContext* context,
    const TableFuncBindInput* input) {
    FTSIndexUtils::validateAutoTransaction(*context, DropFTSFunction::name);
    auto indexName = input->getLiteralVal<std::string>(1);
    const auto tableEntry = FTSIndexUtils::bindNodeTable(*context,
        input->getLiteralVal<std::string>(0), indexName, FTSIndexUtils::IndexOperation::DROP);
    return std::make_unique<FTSBindData>(tableEntry->getName(), tableEntry->getTableID(), indexName,
        binder::expression_vector{});
}

std::string dropFTSIndexQuery(ClientContext& context, const TableFuncBindData& bindData) {
    context.setUseInternalCatalogEntry(true /* useInternalCatalogEntry */);
    auto ftsBindData = bindData.constPtrCast<FTSBindData>();
    auto catalog = catalog::Catalog::Get(context);
    auto transaction = transaction::Transaction::Get(context);
    auto query = std::format("CALL _DROP_FTS_INDEX('{}', '{}');", ftsBindData->tableName,
        ftsBindData->indexName);
    // Drop the rel table before the node tables it references. IF EXISTS keeps a
    // previously-interrupted DROP retryable: _DROP_FTS_INDEX above removes the catalog
    // index entry first, so a retry can no longer rely on the entry being present.
    query += std::format("DROP TABLE IF EXISTS `{}`;",
        FTSUtils::getAppearsInTableName(ftsBindData->tableID, ftsBindData->indexName));
    // Transient build table; residue if CREATE was interrupted before its final DROP.
    query += std::format("DROP TABLE IF EXISTS `{}`;",
        FTSUtils::getAppearsInfoTableName(ftsBindData->tableID, ftsBindData->indexName));
    query += std::format("DROP TABLE IF EXISTS `{}`;",
        FTSUtils::getDocsTableName(ftsBindData->tableID, ftsBindData->indexName));
    query += std::format("DROP TABLE IF EXISTS `{}`;",
        FTSUtils::getTermsTableName(ftsBindData->tableID, ftsBindData->indexName));
    // Per-index stopwords copy (the shared default table is never dropped).
    auto indexEntry = catalog->getIndex(transaction, ftsBindData->tableID, ftsBindData->indexName);
    auto stopWordsTableName =
        indexEntry->getAuxInfo().cast<FTSIndexAuxInfo>().config.stopWordsTableName;
    if (stopWordsTableName != FTSUtils::getDefaultStopWordsTableName()) {
        query += std::format("DROP TABLE IF EXISTS `{}`;", stopWordsTableName);
    }
    query += std::format("DROP MACRO IF EXISTS `{}`;",
        FTSUtils::getTokenizeMacroName(ftsBindData->tableID, ftsBindData->indexName));
    return query;
}

static offset_t internalTableFunc(const TableFuncInput& input, TableFuncOutput& /*output*/) {
    auto& ftsBindData = *input.bindData->constPtrCast<FTSBindData>();
    auto& context = *input.context;
    catalog::Catalog::Get(*context.clientContext)
        ->dropIndex(transaction::Transaction::Get(*context.clientContext), ftsBindData.tableID,
            ftsBindData.indexName);
    storage::StorageManager::Get(*context.clientContext)
        ->getTable(ftsBindData.tableID)
        ->cast<storage::NodeTable>()
        .dropIndex(ftsBindData.indexName);
    return 0;
}

function_set InternalDropFTSFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::STRING});
    func->tableFunc = internalTableFunc;
    func->bindFunc = bindFunc;
    func->initSharedStateFunc = SimpleTableFunc::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    functionSet.push_back(std::move(func));
    return functionSet;
}

function_set DropFTSFunction::getFunctionSet() {
    function_set functionSet;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::STRING});
    func->tableFunc = TableFunction::emptyTableFunc;
    func->bindFunc = bindFunc;
    func->initSharedStateFunc = SimpleTableFunc::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->rewriteFunc = dropFTSIndexQuery;
    func->canParallelFunc = [] { return false; };
    func->isReadOnly = false;
    functionSet.push_back(std::move(func));
    return functionSet;
}

} // namespace fts_extension
} // namespace lbug
