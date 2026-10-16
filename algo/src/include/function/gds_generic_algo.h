#pragma once

// Generic builders behind the GDS registry (gds_registry.cpp).
//
// A registry row is one call to makeNodeScoreFunc / makePartitionFunc / makeEmbeddingFunc:
//
//   makeNodeScoreFunc<DampingFactor, Tolerance>(
//       \"GDS_X\", \"score\", GDSExtraKind::NONE, GDSGraphMode::UNDIRECTED,
//       [](const NetworKit::GraphR& g, common::offset_t, const auto& params, const auto&) {
//           NetworKit::SomeCentrality algo(g, params.template get<DampingFactor>(), ...);
//           algo.run();
//           return algo.scores();
//       });
//
// The Specs... pack is the row's parameter signature (see gds_generic_params.h); the lambda
// body is the row's algorithm invocation. Everything else — CALL binding, the CSR bridge,
// GraphR construction, result streaming — is shared. Adding an algorithm is one table row:
// no new file, no CMake change, no extension-load change.
//
// Shapes:
//   makeNodeScoreFunc  — double per node (centrality scores, SSSP distances, core numbers).
//   makePartitionFunc  — int64 per node (community / component ids from a NetworKit Partition).
//   makeEmbeddingFunc  — fixed-width float/double vector per node (Node2Vec, FastRP).
//
// Algo-type dimensions expressed per row:
//   GDSExtraKind  — NONE (graph name only), SOURCE_LIST (PPR-style LIST of rowids),
//                   SINGLE_SOURCE (BFS-style single INT64 rowid).
//   GDSGraphMode  — UNDIRECTED (symmetrized simple projection, zero-copy when possible),
//                   DIRECTED (out/in CSR pair scanned from storage).

#include <functional>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/types/value/nested.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/gds_csr_bridge.h"
#include "function/gds_generic_params.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <arrow/api.h>
#include <networkit/graph/GraphR.hpp>
#include <networkit/structures/Partition.hpp>

namespace lbug {
namespace algo_extension {

enum class GDSExtraKind : uint8_t { NONE, SOURCE_LIST, SINGLE_SOURCE };
enum class GDSGraphMode : uint8_t { UNDIRECTED, DIRECTED };

// Positional extra arguments after the graph name. SINGLE_SOURCE stores its one rowid in
// sources[0]; SOURCE_LIST stores the whole list.
struct GDSExtraArgs {
    std::vector<uint64_t> sources;
};

// Flatten a NetworKit partition to per-node int64 ids. Nodes outside the partition's range
// (should not happen on a dense ids graph) map to -1 rather than crashing.
inline std::vector<int64_t> partitionToIds(const NetworKit::Partition& partition,
    common::offset_t numNodes) {
    std::vector<int64_t> ids(numNodes, -1);
    const auto count = std::min<common::offset_t>(numNodes, partition.numberOfElements());
    for (common::offset_t i = 0; i < count; ++i) {
        ids[i] = static_cast<int64_t>(partition[i]);
    }
    return ids;
}

// SSSP distances use huge sentinels for unreachable nodes; surface those as -1 so the column
// stays readable (reachable weighted distances are orders of magnitude smaller).
inline double normalizeDistance(double d) {
    return d >= 1e100 ? -1.0 : d;
}

namespace detail {

// Appends the CALL-signature types for the extra positional args (used when building the
// TableFunction; no bind input is available there yet).
inline void appendExtraSignature(GDSExtraKind kind, std::vector<common::LogicalTypeID>& inputTypes,
    std::vector<std::string>& paramNames, std::vector<common::LogicalType>& paramTypes) {
    if (kind == GDSExtraKind::SOURCE_LIST) {
        inputTypes.push_back(common::LogicalTypeID::LIST);
        paramNames.emplace_back("sources");
        paramTypes.push_back(common::LogicalType::LIST(common::LogicalType::INT64()));
    } else if (kind == GDSExtraKind::SINGLE_SOURCE) {
        inputTypes.push_back(common::LogicalTypeID::INT64);
        paramNames.emplace_back("source");
        paramTypes.push_back(common::LogicalType::INT64());
    }
}

inline GDSExtraArgs bindExtraArgs(GDSExtraKind kind, const function::TableFuncBindInput* input,
    const char* funcName) {
    GDSExtraArgs extra;
    if (kind == GDSExtraKind::SOURCE_LIST) {
        const auto& value = input->getValue(1);
        value.validateType(common::LogicalTypeID::LIST);
        extra.sources.reserve(common::NestedVal::getChildrenSize(&value));
        for (auto i = 0u; i < common::NestedVal::getChildrenSize(&value); ++i) {
            const auto* child = common::NestedVal::getChildVal(&value, i);
            child->validateType(common::LogicalTypeID::INT64);
            const auto source = child->getValue<int64_t>();
            if (source < 0) {
                throw common::BinderException{
                    std::string{funcName} + " source node offsets must be non-negative."};
            }
            extra.sources.push_back(static_cast<uint64_t>(source));
        }
        if (extra.sources.empty()) {
            throw common::BinderException{
                std::string{funcName} + " requires at least one source node."};
        }
    } else if (kind == GDSExtraKind::SINGLE_SOURCE) {
        const auto source = input->getLiteralVal<int64_t>(1);
        if (source < 0) {
            throw common::BinderException{
                std::string{funcName} + " source node offset must be non-negative."};
        }
        extra.sources.push_back(static_cast<uint64_t>(source));
    }
    return extra;
}

inline void validateExtraArgs(const GDSExtraArgs& extra, common::offset_t numNodes,
    const char* funcName) {
    for (const auto source : extra.sources) {
        if (source >= numNodes) {
            throw common::BinderException{std::string{funcName} + " source node offset " +
                                          std::to_string(source) +
                                          " is out of range: the projected graph has " +
                                          std::to_string(numNodes) + " nodes."};
        }
    }
}

// Functions whose extras include nested types (SOURCE_LIST's LIST(INT64)) cannot use the
// default input-type path: the binder builds LogicalTypes from the declared LogicalTypeIDs,
// and a bare LIST ID has no child info. Infer the nested type from the CALL-site argument
// instead (same mechanism as query_hnsw_index's ARRAY param).
inline void setInferInputTypes(GDSExtraKind kind, function::TableFunction* func) {
    if (kind == GDSExtraKind::SOURCE_LIST) {
        func->inferInputTypes = [](const binder::expression_vector& params) {
            std::vector<common::LogicalType> types;
            types.push_back(common::LogicalType::STRING());
            types.push_back(params[1]->getDataType().copy());
            return types;
        };
    }
}

template<typename T>
struct ScalarLogicalType;
template<>
struct ScalarLogicalType<double> {
    static common::LogicalType get() { return common::LogicalType::DOUBLE(); }
};
template<>
struct ScalarLogicalType<int64_t> {
    static common::LogicalType get() { return common::LogicalType::INT64(); }
};

template<typename OutputType, typename Params>
struct GenericScalarBindData final : public function::GDSBindData {
    using Runner = std::function<std::vector<OutputType>(const NetworKit::GraphR&, common::offset_t,
        const Params&, const GDSExtraArgs&)>;

    std::string funcName;
    std::string graphName;
    GDSExtraArgs extra;
    Runner runner;
    GDSGraphMode mode;

    GenericScalarBindData(binder::expression_vector columns, graph::NativeGraphEntry graphEntry,
        binder::expression_vector output, std::unique_ptr<function::OptionalParams> params,
        std::string funcName, std::string graphName, GDSExtraArgs extra, Runner runner,
        GDSGraphMode mode)
        : function::GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          funcName{std::move(funcName)}, graphName{std::move(graphName)}, extra{std::move(extra)},
          runner{std::move(runner)}, mode{mode} {
        this->optionalParams = std::move(params);
    }

    std::unique_ptr<function::TableFuncBindData> copy() const override {
        return std::make_unique<GenericScalarBindData>(*this);
    }
};

template<typename OutputType>
class GenericScalarVertexCompute final : public function::GDSResultVertexCompute {
public:
    GenericScalarVertexCompute(storage::MemoryManager* mm,
        function::GDSFuncSharedState* sharedState, const std::vector<OutputType>& values)
        : GDSResultVertexCompute{mm, sharedState}, values{values} {
        nodeIDVector = createVector(common::LogicalType::INTERNAL_ID());
        outputVector = createVector(ScalarLogicalType<OutputType>::get());
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
        return std::make_unique<GenericScalarVertexCompute>(mm, sharedState, values);
    }

private:
    const std::vector<OutputType>& values;
    std::unique_ptr<common::ValueVector> nodeIDVector;
    std::unique_ptr<common::ValueVector> outputVector;
};

template<typename FloatT>
struct EmbeddingLogicalType;
template<>
struct EmbeddingLogicalType<float> {
    static common::LogicalType get() {
        return common::LogicalType::LIST(common::LogicalType::FLOAT());
    }
};
template<>
struct EmbeddingLogicalType<double> {
    static common::LogicalType get() {
        return common::LogicalType::LIST(common::LogicalType::DOUBLE());
    }
};

template<typename FloatT>
class GenericEmbeddingVertexCompute final : public function::GDSResultVertexCompute {
public:
    GenericEmbeddingVertexCompute(storage::MemoryManager* mm,
        function::GDSFuncSharedState* sharedState, const std::vector<std::vector<FloatT>>& features)
        : GDSResultVertexCompute{mm, sharedState}, features{features} {
        nodeIDVector = createVector(common::LogicalType::INTERNAL_ID());
        embVector = createVector(EmbeddingLogicalType<FloatT>::get());
    }

    void beginOnTableInternal(common::table_id_t) override {}

    void vertexCompute(common::offset_t startOffset, common::offset_t endOffset,
        common::table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            nodeIDVector->setValue<common::nodeID_t>(0, common::nodeID_t{i, tableID});
            const auto& emb = i < features.size() ? features[i] : empty;
            auto entry = common::ListVector::addList(embVector.get(), emb.size());
            embVector->setValue<common::list_entry_t>(0, entry);
            auto* dataVector = common::ListVector::getDataVector(embVector.get());
            for (auto k = 0u; k < emb.size(); ++k) {
                if constexpr (std::is_same_v<FloatT, float>) {
                    dataVector->setValue<float>(entry.offset + k, emb[k]);
                } else {
                    dataVector->setValue<double>(entry.offset + k, emb[k]);
                }
            }
            localFT->append(vectors);
        }
    }

    std::unique_ptr<function::VertexCompute> copy() override {
        return std::make_unique<GenericEmbeddingVertexCompute>(mm, sharedState, features);
    }

private:
    const std::vector<std::vector<FloatT>>& features;
    const std::vector<FloatT> empty;
    std::unique_ptr<common::ValueVector> nodeIDVector;
    std::unique_ptr<common::ValueVector> embVector;
};

} // namespace detail

// Per-node double output (centrality scores, distances, core numbers, ...).
template<typename... Specs, typename Runner>
function::function_set makeNodeScoreFunc(std::string funcName, std::string outputColumn,
    GDSExtraKind extraKind, GDSGraphMode mode, Runner&& runner) {
    using Params = GDSGenericParams<Specs...>;
    using BindData = detail::GenericScalarBindData<double, Params>;
    typename BindData::Runner erasedRunner = std::forward<Runner>(runner);
    function::function_set result;
    std::vector<common::LogicalTypeID> inputTypes{common::LogicalTypeID::STRING};
    std::vector<std::string> paramNames{"graph_name"};
    std::vector<common::LogicalType> paramTypes;
    paramTypes.push_back(common::LogicalType::STRING());
    auto tableFunc = [erasedRunner](const function::TableFuncInput& input,
                         function::TableFuncOutput&) {
        auto clientContext = input.context->clientContext;
        auto transaction = transaction::Transaction::Get(*clientContext);
        auto sharedState = input.sharedState->template ptrCast<function::GDSFuncSharedState>();
        auto graph = sharedState->graph.get();
        auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
        if (maxOffsetMap.size() != 1) {
            throw common::BinderException{"GDS functions currently support single-node-table "
                                          "graphs only."};
        }
        const auto tableID = maxOffsetMap.begin()->first;
        const auto numNodes = maxOffsetMap.begin()->second;
        auto mm = storage::MemoryManager::Get(*clientContext);
        auto bindData = input.bindData->template constPtrCast<BindData>();
        auto& params = bindData->optionalParams->template constCast<Params>();
        detail::validateExtraArgs(bindData->extra, numNodes, bindData->funcName.c_str());
        std::vector<double> values;
        if (bindData->mode == GDSGraphMode::DIRECTED) {
            auto csr = buildDirectedCSR(graph, tableID, numNodes);
            NetworKit::GraphR g(numNodes, /*directed=*/true, csr.outIndices, csr.outIndptr,
                csr.inIndices, csr.inIndptr);
            values = bindData->runner(g, numNodes, params, bindData->extra);
        } else {
            auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID,
                numNodes, mm);
            NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
            values = bindData->runner(g, numNodes, params, bindData->extra);
        }
        auto outputVC =
            std::make_unique<detail::GenericScalarVertexCompute<double>>(mm, sharedState, values);
        function::GDSUtils::runVertexCompute(input.context, function::GDSDensityState::DENSE, graph,
            *outputVC);
        sharedState->factorizedTablePool.mergeLocalTables();
        return 0;
    };
    auto bindFunc = [funcName, outputColumn, extraKind, mode, erasedRunner](
                        main::ClientContext* context, const function::TableFuncBindInput* input) {
        auto graphName = input->getLiteralVal<std::string>(0);
        auto extra = detail::bindExtraArgs(extraKind, input, funcName.c_str());
        auto graphEntry = function::GDSFunction::bindGraphEntry(*context, graphName);
        auto nodeOutput =
            function::GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
        binder::expression_vector columns;
        columns.push_back(nodeOutput->constCast<binder::NodeExpression>().getInternalID());
        columns.push_back(
            input->binder->createVariable(outputColumn, common::LogicalType::DOUBLE()));
        return std::unique_ptr<function::TableFuncBindData>(std::make_unique<BindData>(
            std::move(columns), std::move(graphEntry), binder::expression_vector{nodeOutput},
            std::make_unique<Params>(input->optionalParamsLegacy), funcName, std::move(graphName),
            std::move(extra), erasedRunner, mode));
    };
    // The CALL signature's positional types must be fixed here (bind sees only values, not
    // types): mirror the bind-time logic for the extra args.
    detail::appendExtraSignature(extraKind, inputTypes, paramNames, paramTypes);
    auto func = std::make_unique<function::TableFunction>(funcName, inputTypes);
    detail::setInferInputTypes(extraKind, func.get());
    func->parameterNames = std::move(paramNames);
    func->parameterTypes = std::move(paramTypes);
    func->bindFunc = std::move(bindFunc);
    func->tableFunc = std::move(tableFunc);
    func->initSharedStateFunc = function::GDSFunction::initSharedState;
    func->initLocalStateFunc = function::TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = function::GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = function::GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

// Per-node int64 output (community / component ids).
template<typename... Specs, typename Runner>
function::function_set makePartitionFunc(std::string funcName, std::string outputColumn,
    GDSExtraKind extraKind, GDSGraphMode mode, Runner&& runner) {
    using Params = GDSGenericParams<Specs...>;
    using BindData = detail::GenericScalarBindData<int64_t, Params>;
    typename BindData::Runner erasedRunner = std::forward<Runner>(runner);
    function::function_set result;
    std::vector<common::LogicalTypeID> inputTypes{common::LogicalTypeID::STRING};
    std::vector<std::string> paramNames{"graph_name"};
    std::vector<common::LogicalType> paramTypes;
    paramTypes.push_back(common::LogicalType::STRING());
    auto tableFunc = [erasedRunner](const function::TableFuncInput& input,
                         function::TableFuncOutput&) {
        auto clientContext = input.context->clientContext;
        auto transaction = transaction::Transaction::Get(*clientContext);
        auto sharedState = input.sharedState->template ptrCast<function::GDSFuncSharedState>();
        auto graph = sharedState->graph.get();
        auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
        if (maxOffsetMap.size() != 1) {
            throw common::BinderException{"GDS functions currently support single-node-table "
                                          "graphs only."};
        }
        const auto tableID = maxOffsetMap.begin()->first;
        const auto numNodes = maxOffsetMap.begin()->second;
        auto mm = storage::MemoryManager::Get(*clientContext);
        auto bindData = input.bindData->template constPtrCast<BindData>();
        auto& params = bindData->optionalParams->template constCast<Params>();
        detail::validateExtraArgs(bindData->extra, numNodes, bindData->funcName.c_str());
        std::vector<int64_t> values;
        if (bindData->mode == GDSGraphMode::DIRECTED) {
            auto csr = buildDirectedCSR(graph, tableID, numNodes);
            NetworKit::GraphR g(numNodes, /*directed=*/true, csr.outIndices, csr.outIndptr,
                csr.inIndices, csr.inIndptr);
            values = bindData->runner(g, numNodes, params, bindData->extra);
        } else {
            auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID,
                numNodes, mm);
            NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
            values = bindData->runner(g, numNodes, params, bindData->extra);
        }
        auto outputVC =
            std::make_unique<detail::GenericScalarVertexCompute<int64_t>>(mm, sharedState, values);
        function::GDSUtils::runVertexCompute(input.context, function::GDSDensityState::DENSE, graph,
            *outputVC);
        sharedState->factorizedTablePool.mergeLocalTables();
        return 0;
    };
    auto bindFunc = [funcName, outputColumn, extraKind, mode, erasedRunner](
                        main::ClientContext* context, const function::TableFuncBindInput* input) {
        auto graphName = input->getLiteralVal<std::string>(0);
        auto extra = detail::bindExtraArgs(extraKind, input, funcName.c_str());
        auto graphEntry = function::GDSFunction::bindGraphEntry(*context, graphName);
        auto nodeOutput =
            function::GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
        binder::expression_vector columns;
        columns.push_back(nodeOutput->constCast<binder::NodeExpression>().getInternalID());
        columns.push_back(
            input->binder->createVariable(outputColumn, common::LogicalType::INT64()));
        return std::unique_ptr<function::TableFuncBindData>(std::make_unique<BindData>(
            std::move(columns), std::move(graphEntry), binder::expression_vector{nodeOutput},
            std::make_unique<Params>(input->optionalParamsLegacy), funcName, std::move(graphName),
            std::move(extra), erasedRunner, mode));
    };
    detail::appendExtraSignature(extraKind, inputTypes, paramNames, paramTypes);
    auto func = std::make_unique<function::TableFunction>(funcName, inputTypes);
    detail::setInferInputTypes(extraKind, func.get());
    func->parameterNames = std::move(paramNames);
    func->parameterTypes = std::move(paramTypes);
    func->bindFunc = std::move(bindFunc);
    func->tableFunc = std::move(tableFunc);
    func->initSharedStateFunc = function::GDSFunction::initSharedState;
    func->initLocalStateFunc = function::TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = function::GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = function::GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

// Per-node embedding output (LIST(FLOAT) for float, LIST(DOUBLE) for double).
template<typename FloatT, typename... Specs, typename Runner>
function::function_set makeEmbeddingFunc(std::string funcName, GDSExtraKind extraKind,
    GDSGraphMode mode, Runner&& runner) {
    using Params = GDSGenericParams<Specs...>;
    using RunnerT = std::function<std::vector<std::vector<FloatT>>(const NetworKit::GraphR&,
        common::offset_t, const Params&, const GDSExtraArgs&)>;
    struct BindData final : public function::GDSBindData {
        std::string funcName;
        std::string graphName;
        GDSExtraArgs extra;
        RunnerT runner;
        GDSGraphMode mode;

        BindData(binder::expression_vector columns, graph::NativeGraphEntry graphEntry,
            binder::expression_vector output, std::unique_ptr<function::OptionalParams> params,
            std::string funcName, std::string graphName, GDSExtraArgs extra, RunnerT runner,
            GDSGraphMode mode)
            : function::GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
              funcName{std::move(funcName)}, graphName{std::move(graphName)},
              extra{std::move(extra)}, runner{std::move(runner)}, mode{mode} {
            this->optionalParams = std::move(params);
        }

        std::unique_ptr<function::TableFuncBindData> copy() const override {
            return std::make_unique<BindData>(*this);
        }
    };
    RunnerT erasedRunner = std::forward<Runner>(runner);
    function::function_set result;
    std::vector<common::LogicalTypeID> inputTypes{common::LogicalTypeID::STRING};
    std::vector<std::string> paramNames{"graph_name"};
    std::vector<common::LogicalType> paramTypes;
    paramTypes.push_back(common::LogicalType::STRING());
    auto tableFunc = [erasedRunner](const function::TableFuncInput& input,
                         function::TableFuncOutput&) {
        auto clientContext = input.context->clientContext;
        auto transaction = transaction::Transaction::Get(*clientContext);
        auto sharedState = input.sharedState->template ptrCast<function::GDSFuncSharedState>();
        auto graph = sharedState->graph.get();
        auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
        if (maxOffsetMap.size() != 1) {
            throw common::BinderException{"GDS functions currently support single-node-table "
                                          "graphs only."};
        }
        const auto tableID = maxOffsetMap.begin()->first;
        const auto numNodes = maxOffsetMap.begin()->second;
        auto mm = storage::MemoryManager::Get(*clientContext);
        auto bindData = input.bindData->template constPtrCast<BindData>();
        auto& params = bindData->optionalParams->template constCast<Params>();
        detail::validateExtraArgs(bindData->extra, numNodes, bindData->funcName.c_str());
        std::vector<std::vector<FloatT>> features;
        if (bindData->mode == GDSGraphMode::DIRECTED) {
            auto csr = buildDirectedCSR(graph, tableID, numNodes);
            NetworKit::GraphR g(numNodes, /*directed=*/true, csr.outIndices, csr.outIndptr,
                csr.inIndices, csr.inIndptr);
            features = bindData->runner(g, numNodes, params, bindData->extra);
        } else {
            auto csr = buildUndirectedCSR(clientContext, bindData->graphName, graph, tableID,
                numNodes, mm);
            NetworKit::GraphR g(numNodes, /*directed=*/false, csr.indices, csr.indptr);
            features = bindData->runner(g, numNodes, params, bindData->extra);
        }
        auto outputVC = std::make_unique<detail::GenericEmbeddingVertexCompute<FloatT>>(mm,
            sharedState, features);
        function::GDSUtils::runVertexCompute(input.context, function::GDSDensityState::DENSE, graph,
            *outputVC);
        sharedState->factorizedTablePool.mergeLocalTables();
        return 0;
    };
    auto bindFunc = [funcName, extraKind, mode, erasedRunner](main::ClientContext* context,
                        const function::TableFuncBindInput* input) {
        auto graphName = input->getLiteralVal<std::string>(0);
        auto extra = detail::bindExtraArgs(extraKind, input, funcName.c_str());
        auto graphEntry = function::GDSFunction::bindGraphEntry(*context, graphName);
        auto nodeOutput =
            function::GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
        binder::expression_vector columns;
        columns.push_back(nodeOutput->constCast<binder::NodeExpression>().getInternalID());
        columns.push_back(input->binder->createVariable("embedding",
            detail::EmbeddingLogicalType<FloatT>::get()));
        return std::unique_ptr<function::TableFuncBindData>(std::make_unique<BindData>(
            std::move(columns), std::move(graphEntry), binder::expression_vector{nodeOutput},
            std::make_unique<Params>(input->optionalParamsLegacy), funcName, std::move(graphName),
            std::move(extra), erasedRunner, mode));
    };
    detail::appendExtraSignature(extraKind, inputTypes, paramNames, paramTypes);
    auto func = std::make_unique<function::TableFunction>(funcName, inputTypes);
    detail::setInferInputTypes(extraKind, func.get());
    func->parameterNames = std::move(paramNames);
    func->parameterTypes = std::move(paramTypes);
    func->bindFunc = std::move(bindFunc);
    func->tableFunc = std::move(tableFunc);
    func->initSharedStateFunc = function::GDSFunction::initSharedState;
    func->initLocalStateFunc = function::TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = function::GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = function::GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
