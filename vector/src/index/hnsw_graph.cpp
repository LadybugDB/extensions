#include "index/hnsw_graph.h"

#include <cstring>
#include <unordered_set>

#include "index/hnsw_index_utils.h"
#include "main/client_context.h"
#include "storage/local_cached_column.h"
#include "storage/table/list_chunk_data.h"
#include "storage/table/node_table.h"
#include "storage/table/rel_table.h"
#include "transaction/transaction.h"

using namespace lbug::storage;

namespace lbug {
namespace vector_extension {

static uint8_t* alignPointer(uint8_t* ptr, uint64_t alignment) {
    const auto addr = reinterpret_cast<uintptr_t>(ptr);
    const auto alignedAddr = common::ceilDiv<uintptr_t>(addr, alignment) * alignment;
    return reinterpret_cast<uint8_t*>(alignedAddr);
}

EmbeddingHandle::EmbeddingHandle(common::offset_t offset, GetEmbeddingsScanState* lifetimeManager)
    : offsetInData(offset), lifetimeManager(lifetimeManager) {
    if (lifetimeManager) {
        DASSERT(!isNull());
        lifetimeManager->addEmbedding(*this);
    }
}

EmbeddingHandle::~EmbeddingHandle() {
    if (!isNull() && lifetimeManager) {
        lifetimeManager->reclaimEmbedding(*this);
    }
}

// NOLINTNEXTLINE Uses move assignment operator to assign to fields
EmbeddingHandle::EmbeddingHandle(EmbeddingHandle&& o) noexcept {
    *this = std::move(o);
}

EmbeddingHandle& EmbeddingHandle::operator=(EmbeddingHandle&& o) noexcept {
    if (this != &o) {
        offsetInData = o.offsetInData;
        lifetimeManager = o.lifetimeManager;
        o.offsetInData = common::INVALID_OFFSET;
    }
    return *this;
}

metric_func_t HNSWIndexEmbeddings::getMetricFunction(MetricType metric) const {
    return HNSWIndexUtils::getMetricsFunction(metric, info.typeInfo.getChildType());
}

struct InMemEmbeddingLocalState : GetEmbeddingsScanState {
    explicit InMemEmbeddingLocalState(const CachedColumn* data, const common::LogicalType& type)
        : data(data), type(type) {}

    void* getEmbeddingPtr(const EmbeddingHandle&) override;

    void addEmbedding(const EmbeddingHandle&) override {}
    void reclaimEmbedding(const EmbeddingHandle&) override {}

    const CachedColumn* data;
    const common::LogicalType& type;
};

struct QuantizedInMemEmbeddingLocalState : GetEmbeddingsScanState {
    explicit QuantizedInMemEmbeddingLocalState(const QuantizedEmbeddingView* views)
        : views(views) {}

    void* getEmbeddingPtr(const EmbeddingHandle& handle) override {
        return const_cast<QuantizedEmbeddingView*>(&views[handle.offsetInData]);
    }

    void addEmbedding(const EmbeddingHandle&) override {}
    void reclaimEmbedding(const EmbeddingHandle&) override {}

    const QuantizedEmbeddingView* views;
};

struct QuantizedOnDiskEmbeddingScanState final : GetEmbeddingsScanState {
    QuantizedOnDiskEmbeddingScanState(const transaction::Transaction* transaction,
        const storage::NodeTable* nodeTable, const QuantizedEmbeddingView* views)
        : transaction(transaction), nodeTable(nodeTable), views(views) {}

    void* getEmbeddingPtr(const EmbeddingHandle& handle) override {
        return const_cast<QuantizedEmbeddingView*>(&views[handle.offsetInData]);
    }

    void addEmbedding(const EmbeddingHandle&) override {}
    void reclaimEmbedding(const EmbeddingHandle&) override {}

    const transaction::Transaction* transaction;
    const storage::NodeTable* nodeTable;
    const QuantizedEmbeddingView* views;
};

struct PersistentQuantizedEmbeddingScanState final : GetEmbeddingsScanState {
    struct OwnedEmbedding {
        std::vector<uint8_t> payload;
        QuantizedEmbeddingView view;
        uint64_t refCount = 0;
    };

    PersistentQuantizedEmbeddingScanState(const transaction::Transaction* transaction,
        const storage::NodeTable* sourceTable, storage::NodeTable* quantizedTable,
        uint64_t strideBytes, uint64_t payloadBytes)
        : transaction(transaction), sourceTable(sourceTable), quantizedTable(quantizedTable),
          nodeIDVector(common::LogicalType::INTERNAL_ID()), validVector(common::LogicalType::UINT8()),
          scaleVector(common::LogicalType::FLOAT()), normSqVector(common::LogicalType::FLOAT()),
          payloadVector(quantizedTable->getColumn(4).getDataType().copy()),
          strideBytes(strideBytes), payloadBytes(payloadBytes) {
        auto state = std::make_shared<common::DataChunkState>();
        nodeIDVector.state = state;
        validVector.state = state;
        scaleVector.state = state;
        normSqVector.state = state;
        payloadVector.state = state;
        scanState = std::make_unique<storage::NodeTableScanState>(&nodeIDVector,
            std::vector{&validVector, &scaleVector, &normSqVector, &payloadVector}, state);
        scanState->setToTable(transaction, quantizedTable, {1, 2, 3, 4}, {});
    }

    void* getEmbeddingPtr(const EmbeddingHandle& handle) override {
        return &ownedEmbeddings.at(handle.offsetInData).view;
    }

    void addEmbedding(const EmbeddingHandle& handle) override {
        if (loadEmbedding(handle.offsetInData)) {
            ownedEmbeddings.at(handle.offsetInData).refCount++;
        }
    }

    bool loadEmbedding(common::offset_t offset) {
        if (auto it = ownedEmbeddings.find(offset); it != ownedEmbeddings.end()) {
            return true;
        }
        if (missingEmbeddings.contains(offset)) {
            return false;
        }
        nodeIDVector.setValue(0, common::internalID_t{offset, quantizedTable->getTableID()});
        nodeIDVector.state->getSelVectorUnsafe().setToUnfiltered(1);
        quantizedTable->initScanState(const_cast<transaction::Transaction*>(transaction),
            *scanState, quantizedTable->getTableID(), offset);
        if (!quantizedTable->lookup(transaction, *scanState)) {
            missingEmbeddings.insert(offset);
            return false;
        }
        if (validVector.isNull(0) || validVector.getValue<uint8_t>(0) == 0) {
            missingEmbeddings.insert(offset);
            return false;
        }
        loadEmbeddingFromOutput(offset, 0);
        return true;
    }

    void loadEmbeddings(std::span<const common::offset_t> offsets) {
        common::sel_t numSelectedValues = 0;
        auto& selVector = nodeIDVector.state->getSelVectorUnsafe();
        selVector.setToFiltered();
        for (auto i = 0u; i < offsets.size(); ++i) {
            const auto offset = offsets[i];
            if (ownedEmbeddings.contains(offset) || missingEmbeddings.contains(offset)) {
                continue;
            }
            if (!sourceTable->isVisible(transaction, offset)) {
                missingEmbeddings.insert(offset);
                continue;
            }
            nodeIDVector.setValue(i, common::internalID_t{offset, quantizedTable->getTableID()});
            selVector[numSelectedValues++] = i;
        }
        selVector.setSelSize(numSelectedValues);
        if (numSelectedValues == 0) {
            return;
        }
        // lookupMultiple takes Transaction* for scan-state initialization, although this path only
        // reads transaction visibility metadata. Keep this cast safe by ensuring lookupMultiple and
        // its callees do not mutate Transaction; pass a non-const transaction here if that changes.
        [[maybe_unused]] const auto lookupSuccess =
            quantizedTable->lookupMultiple<false>(const_cast<transaction::Transaction*>(transaction),
                *scanState);
        DASSERT(lookupSuccess);
        for (auto i = 0u; i < selVector.getSelSize(); ++i) {
            const auto pos = selVector[i];
            const auto offset = offsets[pos];
            if (validVector.isNull(pos) || validVector.getValue<uint8_t>(pos) == 0 ||
                scaleVector.isNull(pos) || normSqVector.isNull(pos) || payloadVector.isNull(pos)) {
                missingEmbeddings.insert(offset);
                continue;
            }
            loadEmbeddingFromOutput(offset, pos);
        }
    }

    void loadEmbeddingFromOutput(common::offset_t offset, common::sel_t pos) {
        auto owned = OwnedEmbedding{};
        owned.payload.resize(strideBytes);
        const auto entry = payloadVector.getValue<common::list_entry_t>(pos);
        std::memcpy(owned.payload.data(), common::ListVector::getListValues(&payloadVector, entry),
            payloadBytes);
        const auto scale = scaleVector.getValue<float>(pos);
        const auto normSq = normSqVector.getValue<float>(pos);
        owned.view = QuantizedEmbeddingView{owned.payload.data(), scale, normSq};
        ownedEmbeddings.emplace(offset, std::move(owned));
    }

    void reclaimEmbedding(const EmbeddingHandle& handle) override {
        auto it = ownedEmbeddings.find(handle.offsetInData);
        if (it == ownedEmbeddings.end()) {
            return;
        }
        if (--it->second.refCount == 0) {
            ownedEmbeddings.erase(it);
        }
    }

    const transaction::Transaction* transaction;
    const storage::NodeTable* sourceTable;
    storage::NodeTable* quantizedTable;
    common::ValueVector nodeIDVector;
    common::ValueVector validVector;
    common::ValueVector scaleVector;
    common::ValueVector normSqVector;
    common::ValueVector payloadVector;
    std::unique_ptr<storage::NodeTableScanState> scanState;
    uint64_t strideBytes;
    uint64_t payloadBytes;
    std::unordered_map<common::offset_t, OwnedEmbedding> ownedEmbeddings;
    std::unordered_set<common::offset_t> missingEmbeddings;
};

void* InMemEmbeddingLocalState::getEmbeddingPtr(const EmbeddingHandle& handle) {
    void* val = nullptr;
    if (!handle.isNull()) {
        common::TypeUtils::visit(type, [&]<typename T>(T) {
            auto [nodeGroupIdx, offsetInGroup] =
                StorageUtils::getNodeGroupIdxAndOffsetInChunk(handle.offsetInData);
            DASSERT(nodeGroupIdx < data->columnChunks.size());
            const auto& listChunk = data->columnChunks[nodeGroupIdx]->cast<ListChunkData>();
            val = &listChunk.getDataColumnChunk()
                       ->getData<T>()[listChunk.getListStartOffset(offsetInGroup)];
        });
        DASSERT(val != nullptr);
    }
    return val;
}

InMemEmbeddings::InMemEmbeddings(transaction::Transaction* transaction,
    common::ArrayTypeInfo typeInfo, common::table_id_t tableID, common::column_id_t columnID)
    : HNSWIndexEmbeddings{std::move(typeInfo)} {
    auto& cacheManager = transaction->getLocalCacheManager();
    const auto key = CachedColumn::getKey(tableID, columnID);
    if (cacheManager.contains(key)) {
        data = transaction->getLocalCacheManager().at(key).cast<CachedColumn>();
    } else {
        throw common::RuntimeException("Miss cached column, which should never happen.");
    }
}

std::unique_ptr<GetEmbeddingsScanState> InMemEmbeddings::constructScanState(
    transaction::Transaction*) const {
    return std::make_unique<InMemEmbeddingLocalState>(data, info.typeInfo.getChildType());
}

static bool isNull(common::offset_t offset, CachedColumn* data) {
    auto [nodeGroupIdx, offsetInGroup] = StorageUtils::getNodeGroupIdxAndOffsetInChunk(offset);
    DASSERT(nodeGroupIdx < data->columnChunks.size());
    const auto& listChunk = data->columnChunks[nodeGroupIdx]->cast<ListChunkData>();
    return listChunk.isNull(offsetInGroup);
}

EmbeddingHandle InMemEmbeddings::getEmbedding(common::offset_t offset,
    GetEmbeddingsScanState& scanState) const {
    if (isNull(offset, data)) {
        return EmbeddingHandle::createNullHandle();
    }
    return EmbeddingHandle{offset, &scanState};
}

std::vector<EmbeddingHandle> InMemEmbeddings::getEmbeddings(
    std::span<const common::offset_t> offsets, GetEmbeddingsScanState& scanState) const {
    std::vector<EmbeddingHandle> ret;
    ret.reserve(offsets.size());
    for (common::offset_t offset : offsets) {
        ret.push_back(getEmbedding(offset, scanState));
    }
    return ret;
}

template<ScalarQuantizationInputType T>
static void quantizeEmbedding(const T* src, uint64_t dimension, QuantizationType quantization,
    MetricType metric, uint8_t* payloadDst, float& scaleDst, float& normSqDst) {
    constexpr float zeroScale = 0.0f;
    constexpr float zeroNormSq = 0.0f;
    double maxAbs = 0.0;
    for (auto i = 0u; i < dimension; ++i) {
        maxAbs = std::max(maxAbs, std::abs(static_cast<double>(src[i])));
    }
    if (maxAbs == 0.0) {
        scaleDst = zeroScale;
        normSqDst = zeroNormSq;
        std::memset(payloadDst, 0,
            HNSWIndexUtils::getQuantizedCachedEmbeddingPayloadBytes(dimension, quantization));
        return;
    }
    const auto maxQuantizedValue = quantization == QuantizationType::SQ8 ? 127.0 : 32767.0;
    const auto scale = static_cast<float>(maxAbs / maxQuantizedValue);
    float normSq = 0.0f;
    if (quantization == QuantizationType::SQ8) {
        for (auto i = 0u; i < dimension; ++i) {
            const auto quantized =
                static_cast<int64_t>(std::llround(static_cast<double>(src[i]) / scale));
            const auto clamped = static_cast<int8_t>(std::clamp<int64_t>(quantized, -127, 127));
            payloadDst[i] = static_cast<uint8_t>(clamped);
            normSq += static_cast<float>(clamped) * static_cast<float>(clamped);
        }
        normSqDst = normSq;
        scaleDst = metric == MetricType::Cosine && normSq > 0.0f ? 1.0f / std::sqrt(normSq) :
                                                                    scale;
        return;
    }
    auto* payloadValues = reinterpret_cast<int16_t*>(payloadDst);
    for (auto i = 0u; i < dimension; ++i) {
        const auto quantized =
            static_cast<int64_t>(std::llround(static_cast<double>(src[i]) / scale));
        const auto clamped = static_cast<int16_t>(std::clamp<int64_t>(quantized, -32767, 32767));
        normSq += static_cast<float>(clamped) * static_cast<float>(clamped);
        payloadValues[i] = clamped;
    }
    normSqDst = normSq;
    scaleDst = metric == MetricType::Cosine && normSq > 0.0f ? 1.0f / std::sqrt(normSq) : scale;
}

static std::vector<uint8_t> initializeNullMask(common::offset_t numRows) {
    return std::vector<uint8_t>(numRows, 1);
}

QuantizedInMemEmbeddings::QuantizedInMemEmbeddings(const main::ClientContext* context,
    common::ArrayTypeInfo typeInfo, storage::NodeTable& table, common::column_id_t columnID,
    QuantizationType quantization, MetricType metric)
    : HNSWIndexEmbeddings{std::move(typeInfo)}, quantization(quantization),
      strideBytes(
          HNSWIndexUtils::getQuantizedCachedEmbeddingStride(info.getDimension(), quantization)),
      nullMask(initializeNullMask(table.getNumTotalRows(transaction::Transaction::Get(*context)))),
      payloadDataPtr(nullptr) {
    const auto numRows = table.getNumTotalRows(transaction::Transaction::Get(*context));
    scales.resize(numRows, 0.0f);
    normSqs.resize(numRows, 0.0f);
    views.resize(numRows);
    const auto alignmentPadding = 31;
    payloadBuffer = storage::MemoryManager::Get(*context)->allocateBuffer(false,
        numRows * strideBytes + alignmentPadding);
    payloadDataPtr = alignPointer(payloadBuffer->getData(), 32);
    auto onDiskEmbeddings = OnDiskEmbeddings(transaction::Transaction::Get(*context),
        storage::MemoryManager::Get(*context),
        common::ArrayTypeInfo{info.typeInfo.getChildType().copy(), info.getDimension()}, table,
        columnID);
    auto scanState = onDiskEmbeddings.constructScanState();
    for (common::offset_t offset = 0; offset < numRows; ++offset) {
        const auto embedding = onDiskEmbeddings.getEmbedding(offset, *scanState);
        if (embedding.isNull()) {
            nullMask[offset] = 1;
            continue;
        }
        nullMask[offset] = 0;
        const auto dst = payloadDataPtr + offset * strideBytes;
        switch (info.typeInfo.getChildType().getLogicalTypeID()) {
        case common::LogicalTypeID::FLOAT: {
            quantizeEmbedding(static_cast<const float*>(embedding.getPtr()), info.getDimension(),
                quantization, metric, dst, scales[offset], normSqs[offset]);
        } break;
        case common::LogicalTypeID::DOUBLE: {
            quantizeEmbedding(static_cast<const double*>(embedding.getPtr()), info.getDimension(),
                quantization, metric, dst, scales[offset], normSqs[offset]);
        } break;
        default: {
            UNREACHABLE_CODE;
        }
        }
        views[offset] = QuantizedEmbeddingView{dst, scales[offset], normSqs[offset]};
    }
}

std::unique_ptr<GetEmbeddingsScanState> QuantizedInMemEmbeddings::constructScanState(
    transaction::Transaction*) const {
    return std::make_unique<QuantizedInMemEmbeddingLocalState>(views.data());
}

EmbeddingHandle QuantizedInMemEmbeddings::getEmbedding(common::offset_t offset,
    GetEmbeddingsScanState& scanState) const {
    if (nullMask[offset] != 0) {
        return EmbeddingHandle::createNullHandle();
    }
    return EmbeddingHandle{offset, &scanState};
}

std::vector<EmbeddingHandle> QuantizedInMemEmbeddings::getEmbeddings(
    std::span<const common::offset_t> offsets, GetEmbeddingsScanState& scanState) const {
    std::vector<EmbeddingHandle> ret;
    ret.reserve(offsets.size());
    for (const auto offset : offsets) {
        ret.push_back(getEmbedding(offset, scanState));
    }
    return ret;
}

metric_func_t QuantizedInMemEmbeddings::getMetricFunction(MetricType metric) const {
    return HNSWIndexUtils::getQuantizedCachedMetricsFunction(metric, quantization);
}

CachedQuantizedColumn::CachedQuantizedColumn(const main::ClientContext* context,
    common::ArrayTypeInfo typeInfo, storage::NodeTable& sourceTable,
    storage::NodeTable& quantizedTable, common::column_id_t sourceColumnID,
    QuantizationType quantization, MetricType metric, common::offset_t numNodes)
    : LocalCacheObject{getKey(quantizedTable.getTableID())}, typeInfo{std::move(typeInfo)},
      strideBytes(
          HNSWIndexUtils::getQuantizedCachedEmbeddingStride(this->typeInfo.getNumElements(),
              quantization)),
      payloadBytes(HNSWIndexUtils::getQuantizedCachedEmbeddingPayloadBytes(
          this->typeInfo.getNumElements(), quantization)),
      nullMask(initializeNullMask(numNodes)), payloadDataPtr(nullptr), scales(numNodes, 0.0f),
      normSqs(numNodes, 0.0f), views(numNodes) {
    const auto alignmentPadding = 31;
    payloadBuffer = storage::MemoryManager::Get(*context)->allocateBuffer(false,
        numNodes * strideBytes + alignmentPadding);
    payloadDataPtr = alignPointer(payloadBuffer->getData(), 32);
    auto transaction = transaction::Transaction::Get(*context);

    auto state = std::make_shared<common::DataChunkState>();
    auto nodeIDVector = common::ValueVector(common::LogicalType::INTERNAL_ID());
    auto validVector = common::ValueVector(common::LogicalType::UINT8());
    auto scaleVector = common::ValueVector(common::LogicalType::FLOAT());
    auto normSqVector = common::ValueVector(common::LogicalType::FLOAT());
    auto payloadVector = common::ValueVector(quantizedTable.getColumn(4).getDataType().copy(),
        storage::MemoryManager::Get(*context));
    nodeIDVector.state = state;
    validVector.state = state;
    scaleVector.state = state;
    normSqVector.state = state;
    payloadVector.state = state;
    auto scanState = storage::NodeTableScanState(&nodeIDVector,
        std::vector{&validVector, &scaleVector, &normSqVector, &payloadVector}, state);
    scanState.setToTable(transaction, &quantizedTable, {1, 2, 3, 4}, {});
    scanState.source = storage::TableScanSource::COMMITTED;
    for (auto nodeGroupIdx = 0u; nodeGroupIdx < quantizedTable.getNumCommittedNodeGroups();
         ++nodeGroupIdx) {
        scanState.nodeGroupIdx = nodeGroupIdx;
        quantizedTable.initScanState(transaction, scanState);
        while (quantizedTable.scan(transaction, scanState)) {
            const auto& selVector = scanState.outState->getSelVector();
            selVector.forEach([&](auto pos) {
                if (validVector.isNull(pos) || scaleVector.isNull(pos) ||
                    normSqVector.isNull(pos) || payloadVector.isNull(pos)) {
                    return;
                }
                const auto entry = payloadVector.getValue<common::list_entry_t>(pos);
                setQuantizedRecord(nodeIDVector.readNodeOffset(pos),
                    validVector.getValue<uint8_t>(pos),
                    scaleVector.getValue<float>(pos), normSqVector.getValue<float>(pos),
                    common::ListVector::getListValues(&payloadVector, entry));
            });
        }
    }

    const auto numCommittedRows = quantizedTable.getNumTotalRows(nullptr);
    if (numCommittedRows < numNodes) {
        auto onDiskEmbeddings = OnDiskEmbeddings(transaction, storage::MemoryManager::Get(*context),
            common::ArrayTypeInfo{this->typeInfo.getChildType().copy(),
                this->typeInfo.getNumElements()},
            sourceTable, sourceColumnID);
        auto pointLookupState = onDiskEmbeddings.constructScanState(transaction);
        std::vector<uint8_t> payload(payloadBytes, 0);
        for (auto offset = numCommittedRows; offset < numNodes; ++offset) {
            const auto embedding = onDiskEmbeddings.getEmbedding(offset, *pointLookupState);
            if (embedding.isNull()) {
                continue;
            }
            auto scale = float{0.0f};
            auto normSq = float{0.0f};
            switch (this->typeInfo.getChildType().getLogicalTypeID()) {
            case common::LogicalTypeID::FLOAT: {
                quantizeEmbedding(static_cast<const float*>(embedding.getPtr()),
                    this->typeInfo.getNumElements(), quantization, metric, payload.data(), scale,
                    normSq);
            } break;
            case common::LogicalTypeID::DOUBLE: {
                quantizeEmbedding(static_cast<const double*>(embedding.getPtr()),
                    this->typeInfo.getNumElements(), quantization, metric, payload.data(), scale,
                    normSq);
            } break;
            default: {
                UNREACHABLE_CODE;
            }
            }
            setQuantizedRecord(offset, 1, scale, normSq, payload.data());
        }
    }
}

void CachedQuantizedColumn::setQuantizedRecord(common::offset_t offset, uint8_t valid, float scale,
    float normSq, const uint8_t* payload) {
    DASSERT(offset < getNumNodes());
    const auto dst = payloadDataPtr + offset * strideBytes;
    if (valid == 0) {
        nullMask[offset] = 1;
        scales[offset] = 0.0f;
        normSqs[offset] = 0.0f;
        views[offset] = QuantizedEmbeddingView{dst, scales[offset], normSqs[offset]};
        return;
    }
    std::memcpy(dst, payload, payloadBytes);
    scales[offset] = scale;
    normSqs[offset] = normSq;
    nullMask[offset] = 0;
    views[offset] = QuantizedEmbeddingView{dst, scales[offset], normSqs[offset]};
}

CachedQuantizedEmbeddings::CachedQuantizedEmbeddings(common::ArrayTypeInfo typeInfo,
    storage::NodeTable& sourceTable, std::shared_ptr<CachedQuantizedColumn> cachedColumn,
    QuantizationType quantization)
    : HNSWIndexEmbeddings{std::move(typeInfo)}, quantization(quantization), nodeTable(sourceTable),
      cachedColumn(std::move(cachedColumn)) {}

std::unique_ptr<GetEmbeddingsScanState> CachedQuantizedEmbeddings::constructScanState(
    transaction::Transaction* transaction) const {
    DASSERT(transaction != nullptr);
    return std::make_unique<QuantizedOnDiskEmbeddingScanState>(transaction, &nodeTable,
        cachedColumn->getViews());
}

EmbeddingHandle CachedQuantizedEmbeddings::getEmbedding(common::offset_t offset,
    GetEmbeddingsScanState& scanState) const {
    if (offset >= cachedColumn->getNumNodes()) {
        return EmbeddingHandle::createNullHandle();
    }
    const auto& quantizedScanState = scanState.cast<QuantizedOnDiskEmbeddingScanState>();
    if (!quantizedScanState.nodeTable->isVisibleNoLock(quantizedScanState.transaction, offset)) {
        return EmbeddingHandle::createNullHandle();
    }
    if (cachedColumn->isNull(offset)) {
        return EmbeddingHandle::createNullHandle();
    }
    return EmbeddingHandle{offset, &scanState};
}

metric_func_t CachedQuantizedEmbeddings::getMetricFunction(MetricType metric) const {
    return HNSWIndexUtils::getQuantizedCachedMetricsFunction(metric, quantization);
}

std::vector<EmbeddingHandle> CachedQuantizedEmbeddings::getEmbeddings(
    std::span<const common::offset_t> offsets, GetEmbeddingsScanState& scanState) const {
    std::vector<EmbeddingHandle> ret;
    ret.reserve(offsets.size());
    for (const auto offset : offsets) {
        ret.push_back(getEmbedding(offset, scanState));
    }
    return ret;
}

TableBackedQuantizedEmbeddings::TableBackedQuantizedEmbeddings(const main::ClientContext* context,
    common::ArrayTypeInfo typeInfo, storage::NodeTable& sourceTable,
    common::column_id_t sourceColumnID, storage::NodeTable& quantizedTable,
    QuantizationType quantization, MetricType metric, common::offset_t numNodes)
    : HNSWIndexEmbeddings{std::move(typeInfo)}, quantization(quantization),
      metric(metric), sourceTable(sourceTable), sourceColumnID(sourceColumnID),
      quantizedTable(quantizedTable),
      defaultTransaction(context == nullptr ? nullptr : transaction::Transaction::Get(*context)),
      numNodes(numNodes),
      strideBytes(
          HNSWIndexUtils::getQuantizedCachedEmbeddingStride(info.getDimension(), quantization)) {}

void TableBackedQuantizedEmbeddings::writeEmbedding(transaction::Transaction* transaction,
    common::offset_t offset, const EmbeddingHandle& embedding) const {
    std::vector<uint8_t> payload(strideBytes, 0);
    auto valid = uint8_t{0};
    auto scale = float{0.0f};
    auto normSq = float{0.0f};
    if (!embedding.isNull()) {
        valid = 1;
        switch (info.typeInfo.getChildType().getLogicalTypeID()) {
        case common::LogicalTypeID::FLOAT: {
            quantizeEmbedding(static_cast<const float*>(embedding.getPtr()), info.getDimension(),
                quantization, metric, payload.data(), scale, normSq);
        } break;
        case common::LogicalTypeID::DOUBLE: {
            quantizeEmbedding(static_cast<const double*>(embedding.getPtr()), info.getDimension(),
                quantization, metric, payload.data(), scale, normSq);
        } break;
        default: {
            UNREACHABLE_CODE;
        }
        }
    }
    writeQuantizedRecord(transaction, offset, valid, scale, normSq, payload.data());
}

void TableBackedQuantizedEmbeddings::rebuild(const main::ClientContext* context) const {
    auto transaction = transaction::Transaction::Get(*context);
    auto onDiskEmbeddings = OnDiskEmbeddings(transaction, storage::MemoryManager::Get(*context),
        common::ArrayTypeInfo{info.typeInfo.getChildType().copy(), info.getDimension()},
        sourceTable, sourceColumnID);
    auto scanState = onDiskEmbeddings.constructScanState(transaction);
    for (common::offset_t offset = 0; offset < numNodes; ++offset) {
        const auto embedding = onDiskEmbeddings.getEmbedding(offset, *scanState);
        writeEmbedding(transaction, offset, embedding);
    }
}

void TableBackedQuantizedEmbeddings::writeQuantizedRecord(transaction::Transaction* transaction,
    common::offset_t offset, uint8_t valid, float scale, float normSq,
    const uint8_t* payload) const {
    auto state = common::DataChunkState::getSingleValueDataChunkState();
    auto nodeIDVector = common::ValueVector(common::LogicalType::INTERNAL_ID());
    auto idVector = common::ValueVector(common::LogicalType::INT64());
    auto validVector = common::ValueVector(common::LogicalType::UINT8());
    auto scaleVector = common::ValueVector(common::LogicalType::FLOAT());
    auto normSqVector = common::ValueVector(common::LogicalType::FLOAT());
    auto payloadVector = common::ValueVector(quantizedTable.getColumn(4).getDataType().copy(),
        storage::MemoryManager::Get(*transaction->getClientContext()));
    nodeIDVector.state = state;
    idVector.state = state;
    validVector.state = state;
    scaleVector.state = state;
    normSqVector.state = state;
    payloadVector.state = state;
    nodeIDVector.setNull(0, false);
    idVector.setValue<int64_t>(0, static_cast<int64_t>(offset));
    validVector.setValue<uint8_t>(0, valid);
    scaleVector.setValue<float>(0, scale);
    normSqVector.setValue<float>(0, normSq);
    const auto entry = common::ListVector::addList(&payloadVector, info.getDimension());
    payloadVector.setValue<common::list_entry_t>(0, entry);
    const auto payloadBytes =
        HNSWIndexUtils::getQuantizedCachedEmbeddingPayloadBytes(info.getDimension(), quantization);
    std::memcpy(common::ListVector::getListValues(&payloadVector, entry), payload, payloadBytes);
    std::vector<common::ValueVector*> propertyVectors{
        &idVector, &validVector, &scaleVector, &normSqVector, &payloadVector};
    auto insertState =
        storage::NodeTableInsertState(nodeIDVector, idVector, std::move(propertyVectors));
    quantizedTable.initInsertState(transaction->getClientContext(), insertState);
    quantizedTable.insert(transaction, insertState);
}

std::unique_ptr<GetEmbeddingsScanState> TableBackedQuantizedEmbeddings::constructScanState(
    transaction::Transaction* transaction) const {
    auto scanTransaction = transaction == nullptr ? defaultTransaction : transaction;
    DASSERT(scanTransaction != nullptr);
    const auto payloadBytes =
        HNSWIndexUtils::getQuantizedCachedEmbeddingPayloadBytes(info.getDimension(), quantization);
    return std::make_unique<PersistentQuantizedEmbeddingScanState>(scanTransaction, &sourceTable,
        &quantizedTable, strideBytes, payloadBytes);
}

EmbeddingHandle TableBackedQuantizedEmbeddings::getEmbedding(common::offset_t offset,
    GetEmbeddingsScanState& scanState) const {
    if (offset >= numNodes) {
        return EmbeddingHandle::createNullHandle();
    }
    const auto& persistentScanState = scanState.cast<PersistentQuantizedEmbeddingScanState>();
    if (!sourceTable.isVisibleNoLock(persistentScanState.transaction, offset)) {
        return EmbeddingHandle::createNullHandle();
    }
    if (!const_cast<PersistentQuantizedEmbeddingScanState&>(persistentScanState).loadEmbedding(
            offset)) {
        return EmbeddingHandle::createNullHandle();
    }
    return EmbeddingHandle{offset, &scanState};
}

std::vector<EmbeddingHandle> TableBackedQuantizedEmbeddings::getEmbeddings(
    std::span<const common::offset_t> offsets, GetEmbeddingsScanState& scanState) const {
    auto& persistentScanState = scanState.cast<PersistentQuantizedEmbeddingScanState>();
    persistentScanState.loadEmbeddings(offsets);
    std::vector<EmbeddingHandle> ret;
    ret.reserve(offsets.size());
    for (const auto offset : offsets) {
        ret.push_back(getEmbedding(offset, scanState));
    }
    return ret;
}

metric_func_t TableBackedQuantizedEmbeddings::getMetricFunction(MetricType metric) const {
    return HNSWIndexUtils::getQuantizedCachedMetricsFunction(metric, quantization);
}

OnDiskEmbeddings::OnDiskEmbeddings(transaction::Transaction* transaction,
    storage::MemoryManager* mm, common::ArrayTypeInfo typeInfo, storage::NodeTable& nodeTable,
    common::column_id_t columnID)
    : HNSWIndexEmbeddings(std::move(typeInfo)), transaction(transaction), mm(mm),
      nodeTable(nodeTable), columnID(columnID) {}

std::unique_ptr<GetEmbeddingsScanState> OnDiskEmbeddings::constructScanState(
    transaction::Transaction* transaction_) const {
    return std::make_unique<OnDiskEmbeddingScanState>(transaction_ == nullptr ? transaction :
                                                                                transaction_,
        mm, nodeTable, columnID, info.getDimension());
}

EmbeddingHandle OnDiskEmbeddings::getEmbedding(common::offset_t offset,
    GetEmbeddingsScanState& embeddingScanState) const {
    auto& scanState = embeddingScanState.cast<OnDiskEmbeddingScanState>().getScanState();
    if (!nodeTable.isVisibleNoLock(transaction, offset)) {
        return EmbeddingHandle::createNullHandle();
    }
    scanState.nodeIDVector->setValue(0, common::internalID_t{offset, nodeTable.getTableID()});
    scanState.nodeIDVector->state->getSelVectorUnsafe().setToUnfiltered(1);
    const auto source = scanState.source;
    const auto nodeGroupIdx = scanState.nodeGroupIdx;
    if (transaction->isUnCommitted(nodeTable.getTableID(), offset)) {
        scanState.source = TableScanSource::UNCOMMITTED;
        scanState.nodeGroupIdx = StorageUtils::getNodeGroupIdx(
            transaction->getLocalRowIdx(nodeTable.getTableID(), offset));
    } else {
        scanState.source = TableScanSource::COMMITTED;
        scanState.nodeGroupIdx = StorageUtils::getNodeGroupIdx(offset);
    }
    if (source == scanState.source && nodeGroupIdx == scanState.nodeGroupIdx) {
        // If the scan state is already initialized for the same source and node group, we can skip
        // re-initialization.
    } else {
        nodeTable.initScanState(transaction, scanState);
    }
    const auto result = nodeTable.lookup<false>(transaction, scanState);
    DASSERT(scanState.outputVectors.size() == 1 &&
            scanState.outputVectors[0]->state->getSelVector()[0] == 0);
    if (!result || scanState.outputVectors[0]->isNull(0)) {
        return EmbeddingHandle::createNullHandle();
    }
    const auto value = scanState.outputVectors[0]->getValue<common::list_entry_t>(0);
    DASSERT(value.size == info.typeInfo.getNumElements());
    return EmbeddingHandle{value.offset, &embeddingScanState};
}

std::vector<EmbeddingHandle> OnDiskEmbeddings::getEmbeddings(
    std::span<const common::offset_t> offsets, GetEmbeddingsScanState& embeddingScanState) const {
    auto& scanState = embeddingScanState.cast<OnDiskEmbeddingScanState>().getScanState();
    DASSERT(scanState.nodeIDVector->state == scanState.outState);

    // We skip scanning deleted nodes
    common::sel_t numSelectedValues = 0;
    auto& selVector = scanState.nodeIDVector->state->getSelVectorUnsafe();
    selVector.setToFiltered();
    for (auto i = 0u; i < offsets.size(); i++) {
        if (nodeTable.isVisible(transaction, offsets[i])) {
            scanState.nodeIDVector->setValue(i,
                common::internalID_t{offsets[i], nodeTable.getTableID()});
            selVector[numSelectedValues] = i;
            ++numSelectedValues;
        }
    }
    selVector.setSelSize(numSelectedValues);

    DASSERT(
        scanState.outputVectors[0]->dataType.getLogicalTypeID() == common::LogicalTypeID::ARRAY);
    [[maybe_unused]] const auto lookupSuccess =
        nodeTable.lookupMultiple<false>(transaction, scanState);
    DASSERT(lookupSuccess);
    std::vector<EmbeddingHandle> embeddings;
    embeddings.reserve(offsets.size());
    DASSERT(selVector.getSelSize() == numSelectedValues);
    for (common::sel_t i = 0u; i < selVector.getSelSize(); i++) {
        const auto pos = selVector[i];
        while (embeddings.size() < pos) {
            // Return a null embedding for deleted nodes
            embeddings.emplace_back(EmbeddingHandle::createNullHandle());
        }
        if (scanState.outputVectors[0]->isNull(pos)) {
            embeddings.emplace_back(EmbeddingHandle::createNullHandle());
        } else {
            const auto value = scanState.outputVectors[0]->getValue<common::list_entry_t>(pos);
            DASSERT(value.size == info.typeInfo.getNumElements());
            embeddings.emplace_back(value.offset, &embeddingScanState);
        }
    }
    while (embeddings.size() < offsets.size()) {
        embeddings.emplace_back(EmbeddingHandle::createNullHandle());
    }
    return embeddings;
}

OnDiskEmbeddingScanState::OnDiskEmbeddingScanState(const transaction::Transaction* transaction,
    MemoryManager* mm, NodeTable& nodeTable, common::column_id_t columnID,
    common::offset_t embeddingDim)
    : embeddingDim(embeddingDim) {
    std::vector columnIDs{columnID};
    // The first ValueVector in scanChunk is reserved for nodeIDs.
    std::vector<common::LogicalType> types;
    types.emplace_back(common::LogicalType::INTERNAL_ID());
    types.emplace_back(nodeTable.getColumn(columnID).getDataType().copy());
    scanChunk = Table::constructDataChunk(mm, std::move(types));
    std::vector outVectors{&scanChunk.getValueVectorMutable(1)};
    scanState = std::make_unique<NodeTableScanState>(&scanChunk.getValueVectorMutable(0),
        outVectors, scanChunk.state);
    scanState->setToTable(transaction, &nodeTable, std::move(columnIDs));
}

void* OnDiskEmbeddingScanState::getEmbeddingPtr(const EmbeddingHandle& handle) {
    DASSERT(!handle.isNull());
    void* val = nullptr;
    const auto dataVector = common::ListVector::getDataVector(scanState->outputVectors[0]);
    common::TypeUtils::visit(
        dataVector->dataType,
        [&]<VectorElementType T>(
            T) { val = reinterpret_cast<T*>(dataVector->getData()) + handle.offsetInData; },
        [&](auto) { UNREACHABLE_CODE; });
    DASSERT(val != nullptr);
    return val;
}

void OnDiskEmbeddingScanState::addEmbedding(const EmbeddingHandle& handle) {
    DASSERT(!handle.isNull());
    DASSERT(handle.offsetInData % embeddingDim == 0);
    const auto offsetToAdd = handle.offsetInData / embeddingDim;
    allocatedOffsets.set(offsetToAdd);
    DASSERT(usedEmbeddingOffsets.empty() || offsetToAdd > usedEmbeddingOffsets.top());
    usedEmbeddingOffsets.push(offsetToAdd);
}

void OnDiskEmbeddingScanState::reclaimEmbedding(const EmbeddingHandle& handle) {
    DASSERT(!handle.isNull());
    DASSERT(handle.offsetInData % embeddingDim == 0);
    const auto offsetToRemove = handle.offsetInData / embeddingDim;
    allocatedOffsets.reset(offsetToRemove);

    /**
     * Reclaim space in output data vector
     * The general idea for managing the used space is:
     * - Each embedding is represented by its offset in the output list data vector
     * - When an embedding is allocated, it is marked as used in 'allocatedOffsets', it is unmarked
     * when it is freed.
     * - When an embedding is allocated, it is also pushed to a stack. The offsets of the stack
     * should be sorted in increasing order
     * - After each free, we pop unmarked offsets off the stack. We then shrink the output list data
     * vector based on the largest offset still on the stack.
     */
    while (!usedEmbeddingOffsets.empty() && !allocatedOffsets.test(usedEmbeddingOffsets.top())) {
        usedEmbeddingOffsets.pop();
    }
    const auto numDataElemsToResizeTo =
        usedEmbeddingOffsets.empty() ? 0 : (usedEmbeddingOffsets.top() + 1) * embeddingDim;
    for (auto* outputVector : scanState->outputVectors) {
        common::ListVector::resizeDataVector(outputVector, numDataElemsToResizeTo);
    }
}

namespace {
template<std::integral CompressedType>
struct TypedCompressedView final : CompressedOffsetsView {
    explicit TypedCompressedView(const uint8_t* data, common::offset_t numEntries)
        : dstNodes(reinterpret_cast<std::atomic<CompressedType>*>(const_cast<uint8_t*>(data)),
              numEntries),
          invalidOffset{std::numeric_limits<CompressedType>::max()} {}

    common::offset_t getNodeOffsetAtomic(common::offset_t idx) const override {
        return dstNodes[idx].load(std::memory_order_relaxed);
    }

    void setNodeOffsetAtomic(common::offset_t idx, common::offset_t nodeOffset) override {
        DASSERT(nodeOffset <= invalidOffset);
        dstNodes[idx].store(static_cast<CompressedType>(nodeOffset), std::memory_order_relaxed);
    }

    common::offset_t getInvalidOffset() const override { return invalidOffset; }

    std::span<std::atomic<CompressedType>> dstNodes;
    common::offset_t invalidOffset;
};

common::offset_t minNumBytesToStore(common::offset_t value) {
    const auto bitWidth = std::bit_width(value);
    static constexpr decltype(bitWidth) bitsPerByte = 8;
    return std::bit_ceil(static_cast<common::offset_t>(common::ceilDiv(bitWidth, bitsPerByte)));
}
} // namespace

CompressedNodeOffsetBuffer::CompressedNodeOffsetBuffer(MemoryManager* mm, common::offset_t numNodes,
    common::length_t maxDegree) {
    const auto numEntries = numNodes * maxDegree;
    // leave one extra slot open for INVALID_ENTRY
    switch (minNumBytesToStore(numNodes + 1)) {
    case 8: {
        buffer = mm->allocateBuffer(false, numEntries * sizeof(std::atomic<uint64_t>));
        view = std::make_unique<TypedCompressedView<uint64_t>>(buffer->getData(), numEntries);
    } break;
    case 4: {
        buffer = mm->allocateBuffer(false, numEntries * sizeof(std::atomic<uint32_t>));
        view = std::make_unique<TypedCompressedView<uint32_t>>(buffer->getData(), numEntries);
    } break;
    case 2: {
        buffer = mm->allocateBuffer(false, numEntries * sizeof(std::atomic<uint16_t>));
        view = std::make_unique<TypedCompressedView<uint16_t>>(buffer->getData(), numEntries);
    } break;
    case 1: {
        buffer = mm->allocateBuffer(false, numEntries * sizeof(std::atomic<uint8_t>));
        view = std::make_unique<TypedCompressedView<uint8_t>>(buffer->getData(), numEntries);
    } break;
    default:
        UNREACHABLE_CODE;
    }
}

compressed_offsets_t CompressedNodeOffsetBuffer::getNeighbors(common::offset_t nodeOffset,
    common::offset_t maxDegree, common::offset_t numNbrs) const {
    const auto startOffset = nodeOffset * maxDegree;
    return compressed_offsets_t{*view, startOffset, startOffset + numNbrs};
}

InMemHNSWGraph::InMemHNSWGraph(MemoryManager* mm, common::offset_t numNodes,
    common::length_t maxDegree)
    : numNodes{numNodes}, dstNodes(mm, numNodes, maxDegree), maxDegree{maxDegree} {
    csrLengthBuffer = mm->allocateBuffer(true, numNodes * sizeof(std::atomic<uint16_t>));
    csrLengths = reinterpret_cast<std::atomic<uint16_t>*>(csrLengthBuffer->getData());
    resetCSRLengthAndDstNodes();
}

void InMemHNSWGraph::resetCSRLengthAndDstNodes() {
    for (common::offset_t i = 0; i < numNodes; i++) {
        setCSRLength(i, 0);
    }
    for (common::offset_t i = 0; i < numNodes * maxDegree; i++) {
        setDstNodeInvalid(i);
    }
}

NodeToHNSWGraphOffsetMap::NodeToHNSWGraphOffsetMap(common::offset_t numNodesInTable,
    const common::NullMask* selectedNodes)
    : numNodesInGraph(selectedNodes->countNulls()), numNodesInTable(numNodesInTable),
      graphToNodeMap(std::make_unique<common::offset_t[]>(numNodesInGraph)) {
    common::offset_t curOffset = 0;
    for (common::offset_t i = 0; i < numNodesInTable; ++i) {
        if (selectedNodes->isNull(i)) {
            graphToNodeMap[curOffset] = i;
            ++curOffset;
        }
    }
}

common::offset_t NodeToHNSWGraphOffsetMap::nodeToGraphOffset(common::offset_t nodeOffset,
    [[maybe_unused]] bool exactMatch) const {
    if (isTrivialMapping()) {
        return nodeOffset;
    }
    const auto it =
        std::lower_bound(graphToNodeMap.get(), graphToNodeMap.get() + numNodesInGraph, nodeOffset);
    const common::offset_t pos = it - graphToNodeMap.get();
    DASSERT(!exactMatch || pos >= numNodesInGraph || *it == nodeOffset);
    return pos;
}

common::offset_t NodeToHNSWGraphOffsetMap::graphToNodeOffset(common::offset_t graphOffset) const {
    return isTrivialMapping() ? graphOffset : graphToNodeMap[graphOffset];
}

} // namespace vector_extension
} // namespace lbug
