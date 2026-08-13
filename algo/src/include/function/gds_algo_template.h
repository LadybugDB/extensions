#pragma once

// The GDS bridge factored by algorithm *shape*, so each new NetworKit algorithm is a small
// descriptor instead of a copied file. One class template per (input × output) equivalence
// class; an algorithm binds by supplying:
//
//   static constexpr const char* name;           // CALL surface, e.g. "GDS_PAGE_RANK"
//   static constexpr const char* OUTPUT_COLUMN;  // result column name, e.g. "rank"
//   using OutputType = double | int64_t;         // per-node scalar type
//   using Params = <OptionalParams subclass>;    // constructed from the optional param list
//   using Extra = <ExtraArgs policy>;            // NoExtraArgs, or e.g. SourceListArg
//   static std::vector<OutputType> run(const NetworKit::GraphR& g, common::offset_t numNodes,
//       const Params& params, const Extra::type& extra);
//
// The template supplies everything else: bind (graph entry, node output, columns, optional
// params, extra positional args), the shared zero-copy CSR bridge, GraphR construction, and
// result streaming through the GDS vertex-compute pipeline. Per-node *vector* outputs
// (embeddings) currently have a single member (GDS_NODE2VEC) and stay bespoke until a second
// member motivates a template.
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/types/value/nested.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/gds_csr_bridge.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <arrow/api.h>
#include <networkit/graph/GraphR.hpp>

namespace lbug {
namespace algo_extension {

template<typename T>
struct GDSOutputTypeTraits;
template<>
struct GDSOutputTypeTraits<double> {
    static common::LogicalType logicalType() { return common::LogicalType::DOUBLE(); }
};
template<>
struct GDSOutputTypeTraits<int64_t> {
    static common::LogicalType logicalType() { return common::LogicalType::INT64(); }
};

// Extra-positional-argument policy: nothing beyond the graph name.
struct NoExtraArgs {
    struct type {};
    static constexpr size_t NUM_ARGS = 0;
    static type bind(const function::TableFuncBindInput*, const char* /*funcName*/) { return {}; }
    static void validate(const type&, common::offset_t /*numNodes*/, const char* /*funcName*/) {}
};

// Extra-positional-argument policy: a non-empty LIST of node rowids (e.g. PPR sources).
struct SourceListArg {
    using type = std::vector<uint64_t>;
    static constexpr size_t NUM_ARGS = 1;

    static type bind(const function::TableFuncBindInput* input, const char* funcName) {
        const auto& value = input->getValue(1);
        value.validateType(common::LogicalTypeID::LIST);
        type sources;
        sources.reserve(common::NestedVal::getChildrenSize(&value));
        for (auto i = 0u; i < common::NestedVal::getChildrenSize(&value); ++i) {
            const auto* child = common::NestedVal::getChildVal(&value, i);
            child->validateType(common::LogicalTypeID::INT64);
            const auto source = child->getValue<int64_t>();
            if (source < 0) {
                throw common::BinderException{
                    std::string{funcName} + " source node offsets must be non-negative."};
            }
            sources.push_back(static_cast<uint64_t>(source));
        }
        if (sources.empty()) {
            throw common::BinderException{
                std::string{funcName} + " requires at least one source node."};
        }
        return sources;
    }

    static void validate(const type& sources, common::offset_t numNodes, const char* funcName) {
        for (const auto source : sources) {
            if (source >= numNodes) {
                throw common::BinderException{std::string{funcName} + " source node offset " +
                                              std::to_string(source) +
                                              " is out of range: the projected graph has " +
                                              std::to_string(numNodes) + " nodes."};
            }
        }
    }
};

// Per-node scalar output — the largest equivalence class (centrality, community detection).
template<typename DESC>
struct GDSPerNodeScalarAlgo {
    using OutputType = typename DESC::OutputType;

    struct BindData final : public function::GDSBindData {
        std::string graphName;
        typename DESC::Extra::type extra;

        BindData(binder::expression_vector columns, graph::NativeGraphEntry graphEntry,
            binder::expression_vector output, std::unique_ptr<function::OptionalParams> params,
            std::string graphName, typename DESC::Extra::type extra)
            : function::GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
              graphName{std::move(graphName)}, extra{std::move(extra)} {
            this->optionalParams = std::move(params);
        }

        std::unique_ptr<function::TableFuncBindData> copy() const override {
            return std::make_unique<BindData>(*this);
        }
    };

    class ResultVertexCompute final : public function::GDSResultVertexCompute {
    public:
        ResultVertexCompute(storage::MemoryManager* mm, function::GDSFuncSharedState* sharedState,
            const std::vector<OutputType>& values)
            : GDSResultVertexCompute{mm, sharedState}, values{values} {
            nodeIDVector = createVector(common::LogicalType::INTERNAL_ID());
            outputVector = createVector(GDSOutputTypeTraits<OutputType>::logicalType());
        }

        void beginOnTableInternal(common::table_id_t) override {}

        void vertexCompute(common::offset_t startOffset, common::offset_t endOffset,
            common::table_id_t tableID) override {
            for (auto i = startOffset; i < endOffset; ++i) {
                if (skip(i)) {
                    continue;
                }
                nodeIDVector->setValue<common::nodeID_t>(0, common::nodeID_t{i, tableID});
                outputVector->setValue<OutputType>(0, i < values.size() ? values[i] : OutputType{});
                localFT->append(vectors);
            }
        }

        std::unique_ptr<function::VertexCompute> copy() override {
            return std::make_unique<ResultVertexCompute>(mm, sharedState, values);
        }

    private:
        const std::vector<OutputType>& values;
        std::unique_ptr<common::ValueVector> nodeIDVector;
        std::unique_ptr<common::ValueVector> outputVector;
    };

    static common::offset_t tableFunc(const function::TableFuncInput& input,
        function::TableFuncOutput&) {
        auto clientContext = input.context->clientContext;
        auto transaction = transaction::Transaction::Get(*clientContext);
        auto sharedState = input.sharedState->template ptrCast<function::GDSFuncSharedState>();
        auto graph = sharedState->graph.get();
        auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
        // MVP: single node table (the common case; multi-table is a follow-up).
        if (maxOffsetMap.size() != 1) {
            throw common::BinderException{
                std::string{DESC::name} + " currently supports single-node-table graphs only."};
        }
        const auto tableID = maxOffsetMap.begin()->first;
        const auto numNodes = maxOffsetMap.begin()->second;
        auto mm = storage::MemoryManager::Get(*clientContext);
        auto bindData = input.bindData->template constPtrCast<BindData>();
        auto& params = bindData->optionalParams->template constCast<typename DESC::Params>();
        DESC::Extra::validate(bindData->extra, numNodes, DESC::name);

        // Undirected CSR — zero-copy from the projected graph's materialized arrow CSR when
        // available, scan fallback otherwise (see gds_csr_bridge.cpp) — then a zero-copy
        // GraphR and the descriptor's algorithm.
        auto csr =
            buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID, numNodes, mm);
        NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
        const auto values = DESC::run(g, numNodes, params, bindData->extra);

        auto outputVC = std::make_unique<ResultVertexCompute>(mm, sharedState, values);
        function::GDSUtils::runVertexCompute(input.context, function::GDSDensityState::DENSE, graph,
            *outputVC);
        sharedState->factorizedTablePool.mergeLocalTables();
        return 0;
    }

    static std::unique_ptr<function::TableFuncBindData> bindFunc(main::ClientContext* context,
        const function::TableFuncBindInput* input) {
        auto graphName = input->getLiteralVal<std::string>(0);
        auto extra = DESC::Extra::bind(input, DESC::name);
        auto graphEntry = function::GDSFunction::bindGraphEntry(*context, graphName);
        auto nodeOutput =
            function::GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
        binder::expression_vector columns;
        columns.push_back(nodeOutput->constCast<binder::NodeExpression>().getInternalID());
        columns.push_back(input->binder->createVariable(DESC::OUTPUT_COLUMN,
            GDSOutputTypeTraits<OutputType>::logicalType()));
        return std::make_unique<BindData>(std::move(columns), std::move(graphEntry),
            binder::expression_vector{nodeOutput},
            std::make_unique<typename DESC::Params>(input->optionalParamsLegacy),
            std::move(graphName), std::move(extra));
    }

    static function::function_set getFunctionSet() {
        function::function_set result;
        std::vector<common::LogicalTypeID> inputTypes{common::LogicalTypeID::ANY};
        for (auto i = 0u; i < DESC::Extra::NUM_ARGS; ++i) {
            inputTypes.push_back(common::LogicalTypeID::ANY);
        }
        auto func = std::make_unique<function::TableFunction>(std::string{DESC::name}, inputTypes);
        func->bindFunc = bindFunc;
        func->tableFunc = tableFunc;
        func->initSharedStateFunc = function::GDSFunction::initSharedState;
        func->initLocalStateFunc = function::TableFunction::initEmptyLocalState;
        func->canParallelFunc = [] { return false; };
        func->getLogicalPlanFunc = function::GDSFunction::getLogicalPlan;
        func->getPhysicalPlanFunc = function::GDSFunction::getPhysicalPlan;
        result.push_back(std::move(func));
        return result;
    }
};

} // namespace algo_extension
} // namespace lbug
