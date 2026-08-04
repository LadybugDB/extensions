#include "index/hnsw_index_utils.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "simsimd.h"
#include "transaction/transaction_context.h"
#include <format>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace lbug {
namespace vector_extension {

bool HNSWIndexUtils::indexExists(const main::ClientContext& context,
    const transaction::Transaction* transaction, const catalog::TableCatalogEntry* tableEntry,
    const std::string& indexName) {
    return catalog::Catalog::Get(context)->containsIndex(transaction, tableEntry->getTableID(),
        indexName);
}

bool HNSWIndexUtils::validateIndexExistence(const main::ClientContext& context,
    const catalog::TableCatalogEntry* tableEntry, const std::string& indexName,
    IndexOperation indexOperation, common::ConflictAction conflictAction) {
    auto transaction = transaction::Transaction::Get(context);
    switch (indexOperation) {
    case IndexOperation::CREATE: {
        if (indexExists(context, transaction, tableEntry, indexName)) {
            switch (conflictAction) {
            case common::ConflictAction::ON_CONFLICT_THROW:
                throw common::BinderException{std::format("Index {} already exists in table {}.",
                    indexName, tableEntry->getName())};
            case common::ConflictAction::ON_CONFLICT_DO_NOTHING:
                return true;
            default:
                UNREACHABLE_CODE;
            }
        }
        return false;
    } break;
    case IndexOperation::DROP: {
        if (!indexExists(context, transaction, tableEntry, indexName)) {
            switch (conflictAction) {
            case common::ConflictAction::ON_CONFLICT_THROW:
                throw common::BinderException{
                    std::format("Table {} doesn't have an index with name {}.",
                        tableEntry->getName(), indexName)};
            case common::ConflictAction::ON_CONFLICT_DO_NOTHING:
                return false;
            default:
                UNREACHABLE_CODE;
            }
        }
        return true;
    } break;
    case IndexOperation::QUERY: {
        if (!indexExists(context, transaction, tableEntry, indexName)) {
            throw common::BinderException{std::format(
                "Table {} doesn't have an index with name {}.", tableEntry->getName(), indexName)};
        }
        return true;
    } break;
    default: {
        UNREACHABLE_CODE;
    }
    }
}

catalog::NodeTableCatalogEntry* HNSWIndexUtils::bindNodeTable(const main::ClientContext& context,
    const std::string& tableName) {
    binder::Binder::validateTableExistence(context, tableName);
    auto transaction = transaction::Transaction::Get(context);
    const auto tableEntry =
        catalog::Catalog::Get(context)->getTableCatalogEntry(transaction, tableName);
    binder::Binder::validateNodeTableType(tableEntry);
    return tableEntry->ptrCast<catalog::NodeTableCatalogEntry>();
}

void HNSWIndexUtils::validateAutoTransaction(const main::ClientContext& context,
    const std::string& funcName) {
    if (!transaction::TransactionContext::Get(context)->isAutoTransaction()) {
        throw common::BinderException{
            std::format("{} is only supported in auto transaction mode.", funcName)};
    }
}

void HNSWIndexUtils::validateColumnType(const catalog::TableCatalogEntry& tableEntry,
    const std::string& columnName) {
    binder::Binder::validateColumnExistence(&tableEntry, columnName);
    auto& type = tableEntry.getProperty(columnName).getType();
    validateColumnType(type);
}

void HNSWIndexUtils::validateDirectInt8IndexConfig(const common::LogicalType& columnType,
    const HNSWIndexConfig& config) {
    const auto& childType =
        columnType.getExtraTypeInfo()->constPtrCast<common::ArrayTypeInfo>()->getChildType();
    if (childType.getLogicalTypeID() != common::LogicalTypeID::INT8) {
        return; // FLOAT/DOUBLE keep existing metric/quantization rules elsewhere.
    }
    if (config.metric != MetricType::Cosine) {
        throw common::BinderException(
            "Direct INT8 vector indexes only support the cosine metric.");
    }
    if (config.quantization != QuantizationType::NONE) {
        throw common::BinderException(
            "Direct INT8 vector indexes require quantization := 'none'.");
    }
}

template<auto Func, VectorElementType T>
static double computeDistance(const void* left, const void* right, uint32_t dimension) {
    double distance = 0.0;
    Func(static_cast<const T*>(left), static_cast<const T*>(right), dimension, &distance);
    return distance;
}

template<auto FUNC_F32, auto FUNC_F64>
static metric_func_t computeDistanceFuncDispatch(const common::LogicalType& type) {
    switch (type.getLogicalTypeID()) {
    case common::LogicalTypeID::FLOAT: {
        return computeDistance<FUNC_F32, float>;
    }
    case common::LogicalTypeID::DOUBLE: {
        return computeDistance<FUNC_F64, double>;
    }
    default: {
        throw common::BinderException(
            "VECTOR_INDEX only supports FLOAT/DOUBLE/INT8 ARRAY columns.");
    }
    }
}

template<ScalarQuantizationInputType T>
struct ScalarQuantizedVector {
    explicit ScalarQuantizedVector(const T* vector, uint32_t dimension, int8_t maxQuantizedValue)
        : values(dimension), scale{0.0} {
        double maxAbs = 0.0;
        for (auto i = 0u; i < dimension; ++i) {
            maxAbs = std::max(maxAbs, std::abs(static_cast<double>(vector[i])));
        }
        if (maxAbs == 0.0) {
            return;
        }
        scale = maxAbs / maxQuantizedValue;
        for (auto i = 0u; i < dimension; ++i) {
            const auto quantized =
                static_cast<int64_t>(std::llround(static_cast<double>(vector[i]) / scale));
            values[i] = static_cast<int8_t>(
                std::clamp<int64_t>(quantized, -maxQuantizedValue, maxQuantizedValue));
        }
    }

    std::vector<int8_t> values;
    double scale;
};

template<auto Func>
static double computeDistanceI8(const int8_t* left, const int8_t* right, uint32_t dimension) {
    simsimd_distance_t distance = 0.0;
    Func(reinterpret_cast<const simsimd_i8_t*>(left), reinterpret_cast<const simsimd_i8_t*>(right),
        dimension, &distance);
    return distance;
}

static double computeDotI16(const int16_t* left, const int16_t* right, uint32_t dimension) {
#if defined(__AVX2__)
    __m256i sum64Lo = _mm256_setzero_si256();
    __m256i sum64Hi = _mm256_setzero_si256();
    uint32_t i = 0;
    for (; i + 16 <= dimension; i += 16) {
        const auto leftVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left + i));
        const auto rightVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right + i));
        const auto pairwise32 = _mm256_madd_epi16(leftVec, rightVec);
        const auto pairwise32Lo = _mm256_castsi256_si128(pairwise32);
        const auto pairwise32Hi = _mm256_extracti128_si256(pairwise32, 1);
        sum64Lo = _mm256_add_epi64(sum64Lo, _mm256_cvtepi32_epi64(pairwise32Lo));
        sum64Hi = _mm256_add_epi64(sum64Hi, _mm256_cvtepi32_epi64(pairwise32Hi));
    }
    alignas(32) int64_t partialSums[8];
    _mm256_store_si256(reinterpret_cast<__m256i*>(partialSums), sum64Lo);
    _mm256_store_si256(reinterpret_cast<__m256i*>(partialSums + 4), sum64Hi);
    int64_t dot = 0;
    for (const auto partial : partialSums) {
        dot += partial;
    }
    for (; i < dimension; ++i) {
        dot += static_cast<int64_t>(left[i]) * static_cast<int64_t>(right[i]);
    }
    return static_cast<double>(dot);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    int64x2_t sum64Lo = vdupq_n_s64(0);
    int64x2_t sum64Hi = vdupq_n_s64(0);
    uint32_t i = 0;
    for (; i + 8 <= dimension; i += 8) {
        const auto leftVec = vld1q_s16(left + i);
        const auto rightVec = vld1q_s16(right + i);
        const auto mulLo = vmull_s16(vget_low_s16(leftVec), vget_low_s16(rightVec));
        const auto mulHi = vmull_s16(vget_high_s16(leftVec), vget_high_s16(rightVec));
        sum64Lo = vaddq_s64(sum64Lo, vpaddlq_s32(mulLo));
        sum64Hi = vaddq_s64(sum64Hi, vpaddlq_s32(mulHi));
    }
    int64_t dot = vgetq_lane_s64(sum64Lo, 0) + vgetq_lane_s64(sum64Lo, 1) +
                  vgetq_lane_s64(sum64Hi, 0) + vgetq_lane_s64(sum64Hi, 1);
    for (; i < dimension; ++i) {
        dot += static_cast<int64_t>(left[i]) * static_cast<int64_t>(right[i]);
    }
    return static_cast<double>(dot);
#else
    int64_t dot = 0;
    for (auto i = 0u; i < dimension; ++i) {
        dot += static_cast<int64_t>(left[i]) * static_cast<int64_t>(right[i]);
    }
    return static_cast<double>(dot);
#endif
}

static constexpr uint64_t QUANTIZED_HEADER_BYTES = sizeof(float) * 2;
static constexpr uint64_t QUANTIZED_SIMD_ALIGNMENT = 32;

static uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return common::ceilDiv(value, alignment) * alignment;
}

static uint64_t getQuantizedPayloadOffsetInternal(QuantizationType quantization) {
    return quantization == QuantizationType::SQ8 ?
               alignUp(QUANTIZED_HEADER_BYTES, QUANTIZED_SIMD_ALIGNMENT) :
               QUANTIZED_HEADER_BYTES;
}

static uint64_t getQuantizedEmbeddingPayloadBytesInternal(uint64_t dimension,
    QuantizationType quantization) {
    switch (quantization) {
    case QuantizationType::SQ8:
        return dimension;
    case QuantizationType::SQ16:
        return dimension * sizeof(int16_t);
    default:
        return 0;
    }
}

static uint64_t getQuantizedCachedEmbeddingPayloadBytesInternal(uint64_t dimension,
    QuantizationType quantization) {
    return getQuantizedEmbeddingPayloadBytesInternal(dimension, quantization);
}

template<ScalarQuantizationInputType T>
static double computeScalarQuantizedDistance(const void* left, const void* right,
    uint32_t dimension, MetricType metric, int8_t maxQuantizedValue) {
    const auto* leftVector = static_cast<const T*>(left);
    const auto* rightVector = static_cast<const T*>(right);
    const ScalarQuantizedVector leftQuantized{leftVector, dimension, maxQuantizedValue};
    const ScalarQuantizedVector rightQuantized{rightVector, dimension, maxQuantizedValue};
    switch (metric) {
    case MetricType::Cosine: {
        if (leftQuantized.scale == 0.0 || rightQuantized.scale == 0.0) {
            return 1.0;
        }
        const auto leftNormSq = computeDistanceI8<simsimd_dot_i8>(leftQuantized.values.data(),
            leftQuantized.values.data(), dimension);
        const auto rightNormSq = computeDistanceI8<simsimd_dot_i8>(rightQuantized.values.data(),
            rightQuantized.values.data(), dimension);
        if (leftNormSq == 0.0 || rightNormSq == 0.0) {
            return 1.0;
        }
        const auto dot = computeDistanceI8<simsimd_dot_i8>(leftQuantized.values.data(),
            rightQuantized.values.data(), dimension);
        const auto similarity = dot / std::sqrt(leftNormSq * rightNormSq);
        return 1.0 - std::clamp(similarity, -1.0, 1.0);
    }
    case MetricType::L2: {
        const auto leftNormSq = computeDistanceI8<simsimd_dot_i8>(leftQuantized.values.data(),
            leftQuantized.values.data(), dimension);
        const auto rightNormSq = computeDistanceI8<simsimd_dot_i8>(rightQuantized.values.data(),
            rightQuantized.values.data(), dimension);
        const auto dot = computeDistanceI8<simsimd_dot_i8>(leftQuantized.values.data(),
            rightQuantized.values.data(), dimension);
        const auto l2sq = leftQuantized.scale * leftQuantized.scale * leftNormSq +
                          rightQuantized.scale * rightQuantized.scale * rightNormSq -
                          2.0 * leftQuantized.scale * rightQuantized.scale * dot;
        return std::sqrt(std::max(0.0, l2sq));
    }
    case MetricType::L2_SQUARE: {
        const auto leftNormSq = computeDistanceI8<simsimd_dot_i8>(leftQuantized.values.data(),
            leftQuantized.values.data(), dimension);
        const auto rightNormSq = computeDistanceI8<simsimd_dot_i8>(rightQuantized.values.data(),
            rightQuantized.values.data(), dimension);
        const auto dot = computeDistanceI8<simsimd_dot_i8>(leftQuantized.values.data(),
            rightQuantized.values.data(), dimension);
        return std::max(0.0, leftQuantized.scale * leftQuantized.scale * leftNormSq +
                                 rightQuantized.scale * rightQuantized.scale * rightNormSq -
                                 2.0 * leftQuantized.scale * rightQuantized.scale * dot);
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

static double computeCachedSQ8Distance(const void* left, const void* right, uint32_t dimension,
    MetricType metric) {
    const auto& leftView = *static_cast<const QuantizedEmbeddingView*>(left);
    const auto& rightView = *static_cast<const QuantizedEmbeddingView*>(right);
    const auto* leftValues = reinterpret_cast<const int8_t*>(leftView.payload);
    const auto* rightValues = reinterpret_cast<const int8_t*>(rightView.payload);
    switch (metric) {
    case MetricType::Cosine: {
        if (leftView.scale == 0.0f || rightView.scale == 0.0f) {
            return 1.0;
        }
        const auto dot = computeDistanceI8<simsimd_dot_i8>(leftValues, rightValues, dimension);
        const auto similarity = static_cast<double>(leftView.scale) *
                                static_cast<double>(rightView.scale) * dot;
        return 1.0 - std::clamp(similarity, -1.0, 1.0);
    }
    case MetricType::L2: {
        const auto dot = computeDistanceI8<simsimd_dot_i8>(leftValues, rightValues, dimension);
        const auto l2sq = leftView.scale * leftView.scale * leftView.normSq +
                          rightView.scale * rightView.scale * rightView.normSq -
                          2.0 * leftView.scale * rightView.scale * dot;
        return std::sqrt(std::max(0.0, l2sq));
    }
    case MetricType::L2_SQUARE: {
        const auto dot = computeDistanceI8<simsimd_dot_i8>(leftValues, rightValues, dimension);
        return std::max(0.0, leftView.scale * leftView.scale * leftView.normSq +
                                 rightView.scale * rightView.scale * rightView.normSq -
                                 2.0 * leftView.scale * rightView.scale * dot);
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

static double computeCachedSQ16Distance(const void* left, const void* right, uint32_t dimension,
    MetricType metric) {
    const auto& leftView = *static_cast<const QuantizedEmbeddingView*>(left);
    const auto& rightView = *static_cast<const QuantizedEmbeddingView*>(right);
    const auto* leftValues = reinterpret_cast<const int16_t*>(leftView.payload);
    const auto* rightValues = reinterpret_cast<const int16_t*>(rightView.payload);
    switch (metric) {
    case MetricType::Cosine: {
        if (leftView.scale == 0.0f || rightView.scale == 0.0f) {
            return 1.0;
        }
        const auto dot = computeDotI16(leftValues, rightValues, dimension);
        const auto similarity = static_cast<double>(leftView.scale) *
                                static_cast<double>(rightView.scale) * dot;
        return 1.0 - std::clamp(similarity, -1.0, 1.0);
    }
    case MetricType::L2: {
        const auto dot = computeDotI16(leftValues, rightValues, dimension);
        const auto l2sq = leftView.scale * leftView.scale * leftView.normSq +
                          rightView.scale * rightView.scale * rightView.normSq -
                          2.0 * leftView.scale * rightView.scale * dot;
        return std::sqrt(std::max(0.0, l2sq));
    }
    case MetricType::L2_SQUARE: {
        const auto dot = computeDotI16(leftValues, rightValues, dimension);
        return std::max(0.0, leftView.scale * leftView.scale * leftView.normSq +
                                 rightView.scale * rightView.scale * rightView.normSq -
                                 2.0 * leftView.scale * rightView.scale * dot);
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

metric_func_t HNSWIndexUtils::getMetricsFunction(MetricType metric, const common::LogicalType& type,
    QuantizationType quantization) {
    if (quantization != QuantizationType::NONE && metric != MetricType::DotProduct) {
        return HNSWIndexUtils::getQuantizedCachedMetricsFunction(metric, quantization);
    }
    if (type.getLogicalTypeID() == common::LogicalTypeID::INT8) {
        if (metric != MetricType::Cosine || quantization != QuantizationType::NONE) {
            throw common::BinderException(
                "Direct INT8 vector indexes only support cosine with quantization := 'none'.");
        }
        return computeDistance<simsimd_cos_i8, int8_t>;
    }
    switch (metric) {
    case MetricType::Cosine: {
        return computeDistanceFuncDispatch<simsimd_cos_f32, simsimd_cos_f64>(type);
    }
    case MetricType::DotProduct: {
        return computeDistanceFuncDispatch<simsimd_dot_f32, simsimd_dot_f64>(type);
    }
    case MetricType::L2: {
        return computeDistanceFuncDispatch<simsimd_l2_f32, simsimd_l2_f64>(type);
    }
    case MetricType::L2_SQUARE: {
        return computeDistanceFuncDispatch<simsimd_l2sq_f32, simsimd_l2sq_f64>(type);
    }
    default: {
        UNREACHABLE_CODE;
    }
    }
}

metric_func_t HNSWIndexUtils::getQuantizedCachedMetricsFunction(MetricType metric,
    QuantizationType quantization) {
    if (quantization == QuantizationType::SQ8) {
        return [metric](const void* left, const void* right, uint32_t dimension) {
            return computeCachedSQ8Distance(left, right, dimension, metric);
        };
    }
    if (quantization == QuantizationType::SQ16) {
        return [metric](const void* left, const void* right, uint32_t dimension) {
            return computeCachedSQ16Distance(left, right, dimension, metric);
        };
    }
    throw common::RuntimeException("Quantized cached metric function requires SQ8 or SQ16.");
}

uint64_t HNSWIndexUtils::getQuantizedEmbeddingPayloadOffset(QuantizationType quantization) {
    return getQuantizedPayloadOffsetInternal(quantization);
}

uint64_t HNSWIndexUtils::getQuantizedEmbeddingStride(uint64_t dimension,
    QuantizationType quantization) {
    return alignUp(getQuantizedPayloadOffsetInternal(quantization) +
                       getQuantizedEmbeddingPayloadBytesInternal(dimension, quantization),
        quantization == QuantizationType::SQ8 ? QUANTIZED_SIMD_ALIGNMENT : 1);
}

uint64_t HNSWIndexUtils::getQuantizedCachedEmbeddingStride(uint64_t dimension,
    QuantizationType quantization) {
    return alignUp(getQuantizedCachedEmbeddingPayloadBytesInternal(dimension, quantization),
        quantization == QuantizationType::SQ8 ? QUANTIZED_SIMD_ALIGNMENT : 1);
}

uint64_t HNSWIndexUtils::getQuantizedEmbeddingPayloadBytes(uint64_t dimension,
    QuantizationType quantization) {
    return getQuantizedEmbeddingPayloadBytesInternal(dimension, quantization);
}

uint64_t HNSWIndexUtils::getQuantizedCachedEmbeddingPayloadBytes(uint64_t dimension,
    QuantizationType quantization) {
    return getQuantizedCachedEmbeddingPayloadBytesInternal(dimension, quantization);
}

void HNSWIndexUtils::validateColumnType(const common::LogicalType& type) {
    if (type.getLogicalTypeID() == common::LogicalTypeID::ARRAY) {
        auto& childType =
            type.getExtraTypeInfo()->constPtrCast<common::ArrayTypeInfo>()->getChildType();
        if (childType.getLogicalTypeID() == common::LogicalTypeID::FLOAT ||
            childType.getLogicalTypeID() == common::LogicalTypeID::DOUBLE ||
            childType.getLogicalTypeID() == common::LogicalTypeID::INT8) {
            return;
        }
    }
    throw common::BinderException(
        "VECTOR_INDEX only supports FLOAT/DOUBLE/INT8 ARRAY columns.");
}

} // namespace vector_extension
} // namespace lbug
