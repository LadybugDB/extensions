#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/task_system/progress_bar.h"
#include "common/types/value/nested.h"
#include "function/algo_function.h"
#include "function/config/max_iterations_config.h"
#include "function/config/page_rank_config.h"
#include "function/degrees.h"
#include "function/gds/gds_utils.h"
#include "function/gds/gds_vertex_compute.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <optional>

using namespace lbug::processor;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

struct TeleportationWeightsParam {
    std::shared_ptr<Expression> param = nullptr;
    std::optional<Value> paramVal;

    TeleportationWeightsParam() = default;

    explicit TeleportationWeightsParam(std::shared_ptr<Expression> param)
        : param{std::move(param)} {}

    static void validate(const Value& mapVal) {
        if (mapVal.getDataType().getLogicalTypeID() != LogicalTypeID::MAP) {
            throw BinderException{"Teleportation weights must be a MAP(INTERNAL_ID, DOUBLE)."};
        }
        auto weightSum = 0.0;
        for (auto i = 0u; i < mapVal.getChildrenSize(); ++i) {
            auto* entry = NestedVal::getChildVal(&mapVal, i);
            auto* keyVal = NestedVal::getChildVal(entry, 0);
            auto* weightVal = NestedVal::getChildVal(entry, 1);
            if (keyVal->getDataType().getLogicalTypeID() != LogicalTypeID::INTERNAL_ID) {
                throw BinderException{"Teleportation weight keys must be INTERNAL_ID."};
            }
            if (weightVal->getDataType().getLogicalTypeID() != LogicalTypeID::DOUBLE) {
                throw BinderException{"Teleportation weight values must be DOUBLE."};
            }
            auto weight = weightVal->getValue<double>();
            if (weight < 0) {
                throw BinderException{"Teleportation weights must be non-negative."};
            }
            weightSum += weight;
        }
        if (weightSum <= 0) {
            throw BinderException{"Sum of teleportation weights must be positive."};
        }
    }

    void evaluateParam(main::ClientContext* /*context*/) {
        if (!param) {
            paramVal.reset();
            return;
        }
        auto value = ExpressionUtil::evaluateAsLiteralValue(*param);
        validate(value);
        paramVal = std::move(value);
    }

    bool isSet() const { return param != nullptr; }

    const Value& getParamVal() const { return paramVal.value(); }
};

struct PageRankOptionalParams final : public MaxIterationOptionalParams {
    OptionalParam<DampingFactor> dampingFactor;
    OptionalParam<Tolerance> tolerance;
    OptionalParam<NormalizeInitial> normalize;
    TeleportationWeightsParam teleportationWeights;

    explicit PageRankOptionalParams(const expression_vector& optionalParams);

    // For copy only
    PageRankOptionalParams(OptionalParam<MaxIterations> maxIterations,
        OptionalParam<DampingFactor> dampingFactor, OptionalParam<Tolerance> tolerance,
        OptionalParam<NormalizeInitial> normalize, TeleportationWeightsParam teleportationWeights)
        : MaxIterationOptionalParams{maxIterations}, dampingFactor{std::move(dampingFactor)},
          tolerance{std::move(tolerance)}, normalize{std::move(normalize)},
          teleportationWeights{std::move(teleportationWeights)} {}

    void evaluateParams(main::ClientContext* context) override {
        MaxIterationOptionalParams::evaluateParams(context);
        dampingFactor.evaluateParam(context);
        tolerance.evaluateParam(context);
        normalize.evaluateParam(context);
        teleportationWeights.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<PageRankOptionalParams>(maxIterations, dampingFactor, tolerance,
            normalize, teleportationWeights);
    }
};

PageRankOptionalParams::PageRankOptionalParams(const expression_vector& optionalParams)
    : MaxIterationOptionalParams{constructMaxIterationParam(optionalParams)} {
    for (auto& optionalParam : optionalParams) {
        auto paramName = StringUtils::getLower(optionalParam->getAlias());
        if (paramName == DampingFactor::NAME) {
            dampingFactor = function::OptionalParam<DampingFactor>(optionalParam);
        } else if (paramName == MaxIterations::NAME) {
            continue;
        } else if (paramName == Tolerance::NAME) {
            tolerance = function::OptionalParam<Tolerance>(optionalParam);
        } else if (paramName == NormalizeInitial::NAME) {
            normalize = function::OptionalParam<NormalizeInitial>(optionalParam);
        } else if (paramName == TeleportationWeights::NAME) {
            teleportationWeights = TeleportationWeightsParam{optionalParam};
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
}

struct PageRankBindData final : public GDSBindData {
    PageRankBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        std::shared_ptr<Expression> nodeOutput,
        std::unique_ptr<PageRankOptionalParams> optionalParams)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{nodeOutput}} {
        this->optionalParams = std::move(optionalParams);
    }

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<PageRankBindData>(*this);
    }
};

static void addCAS(std::atomic<double>& origin, double valToAdd) {
    auto expected = origin.load(std::memory_order_relaxed);
    auto desired = expected + valToAdd;
    while (!origin.compare_exchange_strong(expected, desired)) {
        desired = expected + valToAdd;
    }
}

// Represents PageRank value for all nodes
class PValues {
public:
    PValues(table_id_map_t<offset_t> maxOffsetMap, storage::MemoryManager* mm, double val) {
        for (const auto& [tableID, maxOffset] : maxOffsetMap) {
            valueMap.allocate(tableID, maxOffset, mm);
            pinTable(tableID);
            for (auto i = 0u; i < maxOffset; ++i) {
                values[i].store(val, std::memory_order_relaxed);
            }
        }
    }

    void pinTable(table_id_t tableID) { values = valueMap.getData(tableID); }

    double getValue(offset_t offset) { return values[offset].load(std::memory_order_relaxed); }

    void addValueCAS(offset_t offset, double val) { addCAS(values[offset], val); }

    void setValue(offset_t offset, double val) {
        values[offset].store(val, std::memory_order_relaxed);
    }

private:
    std::atomic<double>* values = nullptr;
    GDSDenseObjectManager<std::atomic<double>> valueMap;
};

class PageRankAuxiliaryState : public GDSAuxiliaryState {
public:
    PageRankAuxiliaryState(Degrees& degrees, PValues& pCurrent, PValues& pNext)
        : degrees{degrees}, pCurrent{pCurrent}, pNext{pNext} {}

    void beginFrontierCompute(table_id_t fromTableID, table_id_t toTableID) override {
        degrees.pinTable(toTableID);
        pCurrent.pinTable(toTableID);
        pNext.pinTable(fromTableID);
    }

    void switchToDense(ExecutionContext*, Graph*) override {}

private:
    Degrees& degrees;
    PValues& pCurrent;
    PValues& pNext;
};

// Sum the weight (current rank / degree) for each incoming edge.
class PNextUpdateEdgeCompute : public EdgeCompute {
public:
    PNextUpdateEdgeCompute(Degrees& degrees, PValues& pCurrent, PValues& pNext)
        : degrees{degrees}, pCurrent{pCurrent}, pNext{pNext} {}

    std::vector<nodeID_t> edgeCompute(nodeID_t boundNodeID, graph::NbrScanState::Chunk& chunk,
        bool) override {
        if (chunk.size() > 0) {
            double valToAdd = 0;
            chunk.forEach([&](auto neighbors, auto, auto i) {
                auto nbrNodeID = neighbors[i];
                valToAdd +=
                    pCurrent.getValue(nbrNodeID.offset) / degrees.getValue(nbrNodeID.offset);
            });
            pNext.addValueCAS(boundNodeID.offset, valToAdd);
        }
        return {};
    }

    std::unique_ptr<EdgeCompute> copy() override {
        return std::make_unique<PNextUpdateEdgeCompute>(degrees, pCurrent, pNext);
    }

private:
    Degrees& degrees;
    PValues& pCurrent;
    PValues& pNext;
};

// Evaluate rank = above result * dampingFactor + teleport (uniform or per-node)
class PNextUpdateVertexCompute : public GDSVertexCompute {
public:
    PNextUpdateVertexCompute(double dampingFactor, double uniformTeleport, PValues* perNodeTeleport,
        PValues& pNext, NodeOffsetMaskMap* nodeMask)
        : GDSVertexCompute{nodeMask}, dampingFactor{dampingFactor},
          uniformTeleport{uniformTeleport}, perNodeTeleport{perNodeTeleport}, pNext{pNext} {}

    void beginOnTableInternal(table_id_t tableID) override {
        pNext.pinTable(tableID);
        if (perNodeTeleport != nullptr) {
            perNodeTeleport->pinTable(tableID);
        }
    }

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            auto teleport =
                perNodeTeleport != nullptr ? perNodeTeleport->getValue(i) : uniformTeleport;
            pNext.setValue(i, pNext.getValue(i) * dampingFactor + teleport);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<PNextUpdateVertexCompute>(dampingFactor, uniformTeleport,
            perNodeTeleport, pNext, nodeMask);
    }

private:
    double dampingFactor;
    double uniformTeleport;
    PValues* perNodeTeleport;
    PValues& pNext;
};

class PDiffVertexCompute : public GDSVertexCompute {
public:
    PDiffVertexCompute(std::atomic<double>& diff, PValues& pCurrent, PValues& pNext,
        NodeOffsetMaskMap* nodeMask)
        : GDSVertexCompute{nodeMask}, diff{diff}, pCurrent{pCurrent}, pNext{pNext} {}

    void beginOnTableInternal(table_id_t tableID) override {
        pCurrent.pinTable(tableID);
        pNext.pinTable(tableID);
    }

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            auto next = pNext.getValue(i);
            auto current = pCurrent.getValue(i);
            if (next > current) {
                addCAS(diff, next - current);
            } else {
                addCAS(diff, current - next);
            }
            pCurrent.setValue(i, 0);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<PDiffVertexCompute>(diff, pCurrent, pNext, nodeMask);
    }

private:
    std::atomic<double>& diff;
    PValues& pCurrent;
    PValues& pNext;
};

class PageRankResultVertexCompute : public GDSResultVertexCompute {
public:
    PageRankResultVertexCompute(storage::MemoryManager* mm, GDSFuncSharedState* sharedState,
        PValues& pNext)
        : GDSResultVertexCompute{mm, sharedState}, pNext{pNext} {
        nodeIDVector = createVector(LogicalType::INTERNAL_ID());
        rankVector = createVector(LogicalType::DOUBLE());
    }

    void beginOnTableInternal(table_id_t tableID) override { pNext.pinTable(tableID); }

    void vertexCompute(offset_t startOffset, offset_t endOffset, table_id_t tableID) override {
        for (auto i = startOffset; i < endOffset; ++i) {
            if (skip(i)) {
                continue;
            }
            auto nodeID = nodeID_t{i, tableID};
            nodeIDVector->setValue<nodeID_t>(0, nodeID);
            rankVector->setValue<double>(0, pNext.getValue(i));
            localFT->append(vectors);
        }
    }

    std::unique_ptr<VertexCompute> copy() override {
        return std::make_unique<PageRankResultVertexCompute>(mm, sharedState, pNext);
    }

private:
    PValues& pNext;
    std::unique_ptr<ValueVector> nodeIDVector;
    std::unique_ptr<ValueVector> rankVector;
};

static PValues buildTeleportConstants(const table_id_map_t<offset_t>& maxOffsetMap,
    MemoryManager* mm, const Value& mapVal, double teleportScale) {
    PValues rawWeights(maxOffsetMap, mm, 0);
    auto weightSum = 0.0;
    for (auto i = 0u; i < mapVal.getChildrenSize(); ++i) {
        auto* entry = NestedVal::getChildVal(&mapVal, i);
        auto nodeID = NestedVal::getChildVal(entry, 0)->getValue<internalID_t>();
        auto weight = NestedVal::getChildVal(entry, 1)->getValue<double>();
        rawWeights.pinTable(nodeID.tableID);
        rawWeights.setValue(nodeID.offset, weight);
        weightSum += weight;
    }

    PValues teleportConstants(maxOffsetMap, mm, 0);
    for (const auto& [tableID, maxOffset] : maxOffsetMap) {
        teleportConstants.pinTable(tableID);
        rawWeights.pinTable(tableID);
        for (auto offset = 0u; offset < maxOffset; ++offset) {
            teleportConstants.setValue(offset,
                teleportScale * rawWeights.getValue(offset) / weightSum);
        }
    }
    return teleportConstants;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction);
    auto numNodes = graph->getNumNodes(transaction);
    auto pageRankBindData = input.bindData->constPtrCast<PageRankBindData>();
    auto& config = pageRankBindData->optionalParams->constCast<PageRankOptionalParams>();
    auto initialValue = config.normalize.getParamVal() ? (double)1 / numNodes : (double)1;
    auto mm = MemoryManager::Get(*clientContext);
    auto p1 = PValues(maxOffsetMap, mm, initialValue);
    auto p2 = PValues(maxOffsetMap, mm, 0);
    PValues* pCurrent = &p1;
    PValues* pNext = &p2;
    auto currentIter = 1u;
    auto currentFrontier =
        DenseFrontier::getVisitedFrontier(input.context, graph, sharedState->getGraphNodeMaskMap());
    auto nextFrontier =
        DenseFrontier::getVisitedFrontier(input.context, graph, sharedState->getGraphNodeMaskMap());
    auto degrees = Degrees(maxOffsetMap, mm);
    DegreesUtils::computeDegree(input.context, graph, sharedState->getGraphNodeMaskMap(), &degrees,
        ExtendDirection::FWD);
    auto frontierPair =
        std::make_unique<DenseFrontierPair>(std::move(currentFrontier), std::move(nextFrontier));
    auto computeState = GDSComputeState(std::move(frontierPair), nullptr, nullptr);
    auto dampingFactor = config.dampingFactor.getParamVal();
    double uniformTeleport = 0;
    PValues teleportConstants(maxOffsetMap, mm, 0);
    PValues* perNodeTeleport = nullptr;
    if (!config.teleportationWeights.isSet()) {
        uniformTeleport = (1 - dampingFactor) * initialValue;
    } else {
        auto teleportScale = (1 - dampingFactor) * initialValue * numNodes;
        teleportConstants = buildTeleportConstants(maxOffsetMap, mm,
            config.teleportationWeights.getParamVal(), teleportScale);
        perNodeTeleport = &teleportConstants;
    }
    while (currentIter < config.maxIterations.getParamVal()) {
        computeState.frontierPair->resetCurrentIter();
        computeState.frontierPair->setActiveNodesForNextIter();
        computeState.edgeCompute =
            std::make_unique<PNextUpdateEdgeCompute>(degrees, *pCurrent, *pNext);
        computeState.auxiliaryState =
            std::make_unique<PageRankAuxiliaryState>(degrees, *pCurrent, *pNext);
        GDSUtils::runAlgorithmEdgeCompute(input.context, computeState, graph, ExtendDirection::BWD,
            1);
        auto pNextUpdateVC = PNextUpdateVertexCompute(dampingFactor, uniformTeleport, perNodeTeleport,
            *pNext, sharedState->getGraphNodeMaskMap());
        GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, pNextUpdateVC);
        std::atomic<double> diff;
        diff.store(0);
        auto pDiffVC =
            PDiffVertexCompute(diff, *pCurrent, *pNext, sharedState->getGraphNodeMaskMap());
        GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, pDiffVC);
        std::swap(pCurrent, pNext);
        if (diff.load() < config.tolerance.getParamVal()) { // Converged.
            break;
        }
        auto progress = static_cast<double>(currentIter) / numNodes;
        ProgressBar::Get(*clientContext)->updateProgress(input.context->queryID, progress);
        currentIter++;
    }
    auto outputVC = std::make_unique<PageRankResultVertexCompute>(mm, sharedState, *pCurrent);
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *outputVC);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static constexpr char RANK_COLUMN_NAME[] = "rank";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(RANK_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<PageRankBindData>(std::move(columns), std::move(graphEntry), nodeOutput,
        std::make_unique<PageRankOptionalParams>(input->optionalParamsLegacy));
}

function_set PageRankFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(PageRankFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
