#include "function/query_fts_bind_data.h"

#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "catalog/catalog.h"
#include "catalog/fts_index_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "index/fts_index.h"
#include "libstemmer.h"
#include "main/client_context.h"
#include "re2.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "utils/fts_utils.h"
#include <format>

namespace lbug {
namespace fts_extension {

using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::storage;

QueryFTSOptionalParams::QueryFTSOptionalParams(const binder::expression_vector& optionalParams) {
    for (auto& optionalParam : optionalParams) {
        auto paramName = StringUtils::getLower(optionalParam->getAlias());
        if (paramName == K::NAME) {
            k = function::OptionalParam<K>(optionalParam);
        } else if (paramName == B::NAME) {
            b = function::OptionalParam<B>(optionalParam);
        } else if (paramName == Conjunctive::NAME) {
            conjunctive = function::OptionalParam<Conjunctive>(optionalParam);
        } else if (paramName == TopK::NAME) {
            topK = function::OptionalParam<TopK>(optionalParam);
        } else {
            throw common::BinderException{"Unknown optional parameter: " + paramName};
        }
    }
}

void QueryFTSOptionalParams::evaluateParams(main::ClientContext* context) {
    k.evaluateParam(context);
    b.evaluateParam(context);
    conjunctive.evaluateParam(context);
    topK.evaluateParam(context);
}

std::vector<std::string> QueryFTSBindData::getQueryTerms(main::ClientContext& context) const {
    auto queryInStr =
        ExpressionUtil::evaluateLiteral<std::string>(&context, query, LogicalType::STRING());
    auto config = auxInfo.config;
    FTSUtils::normalizeQuery(queryInStr, config.ignorePatternQuery,
        true /* protectWildcardChars */);
    auto terms = FTSUtils::tokenizeString(queryInStr, config);
    auto stopWordsTable =
        StorageManager::Get(context)
            ->getTable(catalog::Catalog::Get(context)
                           ->getTableCatalogEntry(transaction::Transaction::Get(context),
                               config.stopWordsTableName)
                           ->getTableID())
            ->ptrCast<NodeTable>();
    return FTSUtils::stemTerms(terms, auxInfo.config, MemoryManager::Get(context), stopWordsTable,
        transaction::Transaction::Get(context),
        optionalParams->constCast<QueryFTSOptionalParams>().conjunctive.getParamVal(),
        true /* isQuery */);
}

void QueryFTSBindData::refreshFromCatalog(main::ClientContext* context) {
    std::lock_guard guard{refreshMutex};
    context->setUseInternalCatalogEntry(true /* useInternalCatalogEntry */);
    try {
        auto catalog = catalog::Catalog::Get(*context);
        auto transaction = transaction::Transaction::Get(*context);
        auto tableEntry = catalog->getTableCatalogEntry(transaction, tableName);
        if (!catalog->containsIndex(transaction, tableEntry->getTableID(), indexName)) {
            throw common::BinderException{std::format(
                "Table {} doesn't have an index with name {}.", tableEntry->getName(), indexName)};
        }
        auto ftsIndexEntry = catalog->getIndex(transaction, tableEntry->getTableID(), indexName);
        auto termsEntry = catalog->getTableCatalogEntry(transaction,
            FTSUtils::getTermsTableName(tableEntry->getTableID(), indexName));
        auto docsEntry = catalog->getTableCatalogEntry(transaction,
            FTSUtils::getDocsTableName(tableEntry->getTableID(), indexName));
        auto appearsInEntry = catalog->getTableCatalogEntry(transaction,
            FTSUtils::getAppearsInTableName(tableEntry->getTableID(), indexName));
        graphEntry = graph::NativeGraphEntry({termsEntry, docsEntry}, {appearsInEntry});
        auxInfo.config = ftsIndexEntry->getAuxInfo().cast<FTSIndexAuxInfo>().config;
        auto nodeTable = StorageManager::Get(*context)
                             ->getTable(ftsIndexEntry->getTableID())
                             ->ptrCast<NodeTable>();
        auto index = nodeTable->getIndex(indexName);
        if (!index.has_value()) {
            throw common::BinderException{std::format(
                "Table {} doesn't have an index with name {}.", tableEntry->getName(), indexName)};
        }
        auto [numDocs_, avgDocLen_] = index.value()->cast<FTSIndex>().getStats(transaction);
        numDocs = numDocs_;
        avgDocLen = avgDocLen_;
    } catch (...) {
        context->setUseInternalCatalogEntry(false /* useInternalCatalogEntry */);
        throw;
    }
    context->setUseInternalCatalogEntry(false /* useInternalCatalogEntry */);
}

} // namespace fts_extension
} // namespace lbug
