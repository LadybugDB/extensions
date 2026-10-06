#pragma once

#include <mutex>
#include <string>

#include "binder/expression/node_expression.h"
#include "catalog/fts_index_catalog_entry.h"
#include "function/fts_config.h"
#include "function/gds/gds.h"

namespace lbug {
namespace fts_extension {

struct QueryFTSOptionalParams : public function::OptionalParams {
    function::OptionalParam<K> k;
    function::OptionalParam<B> b;
    function::OptionalParam<Conjunctive> conjunctive;
    function::OptionalParam<TopK> topK;

    explicit QueryFTSOptionalParams(const binder::expression_vector& optionalParams);

    // For copy only.
    QueryFTSOptionalParams(function::OptionalParam<K> k, function::OptionalParam<B> b,
        function::OptionalParam<Conjunctive> conjunctive, function::OptionalParam<TopK> topK)
        : k{std::move(k)}, b{std::move(b)}, conjunctive{std::move(conjunctive)},
          topK{std::move(topK)} {}

    void evaluateParams(main::ClientContext* context) override;

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<QueryFTSOptionalParams>(k, b, conjunctive, topK);
    }
};

struct QueryFTSBindData final : public function::GDSBindData {
    std::shared_ptr<binder::Expression> query;
    // Owned copy of the FTS aux info. This must NOT be a reference/pointer into the
    // catalog: bind data outlives the bind transaction (prepared-plan cache reuses it
    // across executions on the same connection), so a catalog reference dangles on the
    // second execution and segfaults (see issue #1082).
    FTSIndexAuxInfo auxInfo;
    // Identity of the queried index. The prepared-plan cache reuses this bind data
    // across executions, so every execution must re-resolve these against the catalog
    // (see refreshFromCatalog): after DROP_FTS_INDEX the cached snapshot would
    // otherwise keep serving stale results, and after a recreate it would reference
    // dropped backing tables.
    std::string tableName;
    std::string indexName;
    common::table_id_t outputTableID;
    common::idx_t numDocs;
    double avgDocLen;
    mutable std::mutex refreshMutex;

    QueryFTSBindData(binder::expression_vector columns, graph::NativeGraphEntry graphEntry,
        std::shared_ptr<binder::Expression> docs, std::shared_ptr<binder::Expression> query,
        const FTSIndexAuxInfo& auxInfo, std::string tableName, std::string indexName,
        std::unique_ptr<QueryFTSOptionalParams> optionalParams, common::idx_t numDocs,
        double avgDocLen)
        : GDSBindData{std::move(columns), std::move(graphEntry), binder::expression_vector{docs}},
          query{std::move(query)}, auxInfo{auxInfo}, tableName{std::move(tableName)},
          indexName{std::move(indexName)},
          outputTableID{output[0]->constCast<binder::NodeExpression>().getTableIDs()[0]},
          numDocs{numDocs}, avgDocLen{avgDocLen} {
        auto& nodeExpr = output[0]->constCast<binder::NodeExpression>();
        DASSERT(nodeExpr.getNumEntries() == 1);
        outputTableID = nodeExpr.getEntry(0)->getTableID();
        this->optionalParams = std::move(optionalParams);
    }
    QueryFTSBindData(const QueryFTSBindData& other)
        : GDSBindData{other}, query{other.query}, auxInfo{other.auxInfo},
          tableName{other.tableName}, indexName{other.indexName},
          outputTableID{other.outputTableID}, numDocs{other.numDocs}, avgDocLen{other.avgDocLen} {}

    // Re-resolve the index against the current catalog and refresh the cached snapshot
    // (aux info, backing-table graph entry, index stats). Throws BinderException when the
    // index no longer exists so the parameterized path matches the literal path.
    // Must be called at execution time before the snapshot is used, because the
    // prepared-plan cache reuses this bind data across executions on the same connection.
    void refreshFromCatalog(main::ClientContext* context);

    std::vector<std::string> getQueryTerms(main::ClientContext& context) const;

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<QueryFTSBindData>(*this);
    }
};

} // namespace fts_extension
} // namespace lbug
