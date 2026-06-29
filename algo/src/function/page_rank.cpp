#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/exception/runtime.h"
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
#include "graph/graph.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"
#include <optional>

using namespace lbug::processor;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;
using namespace lbug::catalog;

namespace lbug {
namespace algo_extension {

namespace {

static constexpr char KEY_PROPERTY_FIELD[] = "keyproperty";
static constexpr char WEIGHTS_FIELD[] = "weights";

struct TableTeleportSpec {
    std::string tableName;
    std::string keyProperty;
    Value weights;
};

static const Value* getStructFieldVal(const Value& structVal, const std::string& fieldName) {
    if (structVal.getDataType().getLogicalTypeID() != LogicalTypeID::STRUCT) {
        return nullptr;
    }
    auto lowerFieldName = StringUtils::getLower(fieldName);
    for (auto i = 0u; i < StructType::getNumFields(structVal.getDataType()); ++i) {
        auto& field = StructType::getField(structVal.getDataType(), i);
        if (StringUtils::getLower(field.getName()) == lowerFieldName) {
            return NestedVal::getChildVal(&structVal, i);
        }
    }
    return nullptr;
}

static void validateWeightsMap(const Value& weightsVal) {
    if (weightsVal.getDataType().getLogicalTypeID() != LogicalTypeID::MAP) {
        throw BinderException{"Teleportation weights must be a MAP."};
    }
    auto weightSum = 0.0;
    for (auto i = 0u; i < weightsVal.getChildrenSize(); ++i) {
        auto* entry = NestedVal::getChildVal(&weightsVal, i);
        auto* weightVal = NestedVal::getChildVal(entry, 1);
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

static TableTeleportSpec parseTableTeleportSpec(const std::string& tableName, const Value& tableVal) {
    if (tableVal.getDataType().getLogicalTypeID() != LogicalTypeID::STRUCT) {
        throw BinderException{std::format(
            "Teleportation weights for table {} must be a STRUCT with keyProperty and weights.",
            tableName)};
    }
    auto* keyPropertyVal = getStructFieldVal(tableVal, KEY_PROPERTY_FIELD);
    auto* weightsVal = getStructFieldVal(tableVal, WEIGHTS_FIELD);
    if (keyPropertyVal == nullptr || weightsVal == nullptr) {
        throw BinderException{std::format(
            "Teleportation weights for table {} must contain keyProperty and weights.", tableName)};
    }
    if (keyPropertyVal->getDataType().getLogicalTypeID() != LogicalTypeID::STRING) {
        throw BinderException{"Teleportation keyProperty must be a STRING."};
    }
    validateWeightsMap(*weightsVal);
    return TableTeleportSpec{tableName, keyPropertyVal->getValue<std::string>(), *weightsVal};
}

static std::vector<TableTeleportSpec> parseTeleportationWeights(const Value& configVal) {
    if (configVal.getDataType().getLogicalTypeID() != LogicalTypeID::STRUCT) {
        throw BinderException{
            "Teleportation weights must be a STRUCT mapping node table names to weight specs."};
    }
    std::vector<TableTeleportSpec> specs;
    for (auto i = 0u; i < StructType::getNumFields(configVal.getDataType()); ++i) {
        auto& field = StructType::getField(configVal.getDataType(), i);
        auto* tableVal = NestedVal::getChildVal(&configVal, i);
        specs.push_back(parseTableTeleportSpec(field.getName(), *tableVal));
    }
    if (specs.empty()) {
        throw BinderException{"Teleportation weights must specify at least one node table."};
    }
    return specs;
}

static const NativeGraphEntryTableInfo* findNodeTableInfo(const NativeGraphEntry& graphEntry,
    const std::string& tableName) {
    for (auto& nodeInfo : graphEntry.nodeInfos) {
        if (StringUtils::getLower(nodeInfo.entry->getName()) == StringUtils::getLower(tableName)) {
            return &nodeInfo;
        }
    }
    return nullptr;
}

static void validateTeleportationWeightsAgainstGraph(const Value& configVal,
    const NativeGraphEntry& graphEntry) {
    for (auto& spec : parseTeleportationWeights(configVal)) {
        auto* nodeInfo = findNodeTableInfo(graphEntry, spec.tableName);
        if (nodeInfo == nullptr) {
            throw BinderException{std::format("Unknown node table in teleportation weights: {}.",
                spec.tableName)};
        }
        if (!nodeInfo->entry->containsProperty(spec.keyProperty)) {
            throw BinderException{std::format("Unknown property {} on node table {}.",
                spec.keyProperty, spec.tableName)};
        }
        auto propertyTypeID = nodeInfo->entry->getProperty(spec.keyProperty).getType().getLogicalTypeID();
        auto mapKeyTypeID = MapType::getKeyType(spec.weights.getDataType()).getLogicalTypeID();
        if (propertyTypeID != mapKeyTypeID) {
            throw BinderException{std::format(
                "Teleportation weight keys for table {} must have type {} to match property {}.",
                spec.tableName, nodeInfo->entry->getProperty(spec.keyProperty).getType().toString(),
                spec.keyProperty)};
        }
    }
}

static std::optional<double> lookupWeight(const Value& weightsMap, const Value& key) {
    for (auto i = 0u; i < weightsMap.getChildrenSize(); ++i) {
        auto* entry = NestedVal::getChildVal(&weightsMap, i);
        auto* keyVal = NestedVal::getChildVal(entry, 0);
        if (*keyVal == key) {
            return NestedVal::getChildVal(entry, 1)->getValue<double>();
        }
    }
    return std::nullopt;
}

static bool isNodeActive(NodeOffsetMaskMap* nodeMask, table_id_t tableID, offset_t offset) {
    if (nodeMask == nullptr) {
        return true;
    }
    nodeMask->pin(tableID);
    return nodeMask->valid(offset);
}

} // namespace

struct TeleportationWeightsParam {
    std::shared_ptr<Expression> param = nullptr;
    std::optional<Value> paramVal;

    TeleportationWeightsParam() = default;

    explicit TeleportationWeightsParam(std::shared_ptr<Expression> param)
        : param{std::move(param)} {}

    static void validate(const Value& configVal) { parseTeleportationWeights(configVal); }

    static void validate(const Value& configVal, const NativeGraphEntry& graphEntry) {
        validate(configVal);
        validateTeleportationWeightsAgainstGraph(configVal, graphEntry);
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

static Value readChunkProperty(const graph::VertexScanState::Chunk& chunk, sel_t pos,
    LogicalTypeID typeID) {
    switch (typeID) {
    case LogicalTypeID::STRING:
        return Value(LogicalType::STRING(), chunk.getProperties<string_t>(0)[pos].getAsString());
    case LogicalTypeID::INT64:
        return Value(chunk.getProperties<int64_t>(0)[pos]);
    case LogicalTypeID::INT32:
        return Value(chunk.getProperties<int32_t>(0)[pos]);
    case LogicalTypeID::INT16:
        return Value(chunk.getProperties<int16_t>(0)[pos]);
    case LogicalTypeID::INT8:
        return Value(chunk.getProperties<int8_t>(0)[pos]);
    case LogicalTypeID::UINT64:
        return Value(chunk.getProperties<uint64_t>(0)[pos]);
    case LogicalTypeID::UINT32:
        return Value(chunk.getProperties<uint32_t>(0)[pos]);
    case LogicalTypeID::UINT16:
        return Value(chunk.getProperties<uint16_t>(0)[pos]);
    case LogicalTypeID::UINT8:
        return Value(chunk.getProperties<uint8_t>(0)[pos]);
    default:
        throw RuntimeException{std::format(
            "Unsupported teleportation key property type: {}.", LogicalTypeUtils::toString(typeID))};
    }
}

static PValues buildTeleportConstants(Graph* graph, const NativeGraphEntry& graphEntry,
    const table_id_map_t<offset_t>& maxOffsetMap, MemoryManager* mm, const Value& configVal,
    double teleportScale, NodeOffsetMaskMap* nodeMask) {
    PValues rawWeights(maxOffsetMap, mm, 0);
    auto weightSum = 0.0;
    for (auto& spec : parseTeleportationWeights(configVal)) {
        auto* nodeInfo = findNodeTableInfo(graphEntry, spec.tableName);
        if (nodeInfo == nullptr) {
            continue;
        }
        auto tableID = nodeInfo->entry->getTableID();
        auto maxOffset = maxOffsetMap.at(tableID);
        auto keyTypeID = MapType::getKeyType(spec.weights.getDataType()).getLogicalTypeID();
        auto scanState = graph->prepareVertexScan(nodeInfo->entry, {spec.keyProperty});
        rawWeights.pinTable(tableID);
        for (auto chunk : graph->scanVertices(0, maxOffset, *scanState)) {
            auto nodeIDs = chunk.getNodeIDs();
            for (auto i = 0u; i < chunk.size(); ++i) {
                auto offset = nodeIDs[i].offset;
                if (!isNodeActive(nodeMask, tableID, offset)) {
                    continue;
                }
                auto propertyValue = readChunkProperty(chunk, i, keyTypeID);
                auto weight = lookupWeight(spec.weights, propertyValue);
                if (!weight.has_value()) {
                    continue;
                }
                rawWeights.setValue(offset, weight.value());
                weightSum += weight.value();
            }
        }
    }
    if (weightSum <= 0) {
        throw RuntimeException{
            "Teleportation weights did not match any node in the projected graph."};
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
        teleportConstants = buildTeleportConstants(graph, pageRankBindData->graphEntry, maxOffsetMap,
            mm, config.teleportationWeights.getParamVal(), teleportScale,
            sharedState->getGraphNodeMaskMap());
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
    for (auto& optionalParam : input->optionalParamsLegacy) {
        if (StringUtils::getLower(optionalParam->getAlias()) == TeleportationWeights::NAME) {
            auto value = ExpressionUtil::evaluateAsLiteralValue(*optionalParam);
            TeleportationWeightsParam::validate(value, graphEntry);
        }
    }
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
