// GDS_LEIDEN — Leiden community detection backed by icebug (NetworKit's ParallelLeidenView).
// Higher partition quality and performance than Louvain/PLM on most graphs — prefer this for
// new work. All bridge plumbing lives in GDSPerNodeScalarAlgo (gds_algo_template.h); this file
// is the descriptor: name, output column, optional params, and the NetworKit invocation.
#include "common/string_utils.h"
#include "function/algo_function.h"
#include "function/gds_algo_template.h"
#include <networkit/community/ParallelLeidenView.hpp>
#include <networkit/structures/Partition.hpp>

using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

struct LeidenGamma {
    // Resolution parameter: > 1 favors more/smaller communities, < 1 fewer/larger.
    static constexpr const char* NAME = "gamma";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 1.0;

    static void validate(double gamma) {
        if (gamma <= 0) {
            throw BinderException{"Gamma must be positive."};
        }
    }
};

struct LeidenIterations {
    static constexpr const char* NAME = "iterations";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = 3;

    static void validate(int64_t iterations) {
        if (iterations <= 0) {
            throw BinderException{"Iterations must be positive."};
        }
    }
};

struct GDSLeidenOptionalParams final : public function::OptionalParams {
    OptionalParam<LeidenGamma> gamma;
    OptionalParam<LeidenIterations> iterations;

    explicit GDSLeidenOptionalParams(const expression_vector& optionalParams) {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == LeidenGamma::NAME) {
                gamma = function::OptionalParam<LeidenGamma>(optionalParam);
            } else if (paramName == LeidenIterations::NAME) {
                iterations = function::OptionalParam<LeidenIterations>(optionalParam);
            } else {
                throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    GDSLeidenOptionalParams(OptionalParam<LeidenGamma> gamma,
        OptionalParam<LeidenIterations> iterations)
        : gamma{std::move(gamma)}, iterations{std::move(iterations)} {}

    void evaluateParams(main::ClientContext* context) override {
        gamma.evaluateParam(context);
        iterations.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSLeidenOptionalParams>(gamma, iterations);
    }
};

struct GDSLeidenDesc {
    static constexpr const char* name = GDSLeidenFunction::name;
    static constexpr const char* OUTPUT_COLUMN = "community_id";
    using OutputType = int64_t;
    using Params = GDSLeidenOptionalParams;
    using Extra = NoExtraArgs;

    static std::vector<int64_t> run(const NetworKit::GraphR& g, offset_t numNodes,
        const Params& params, const Extra::type&) {
        NetworKit::ParallelLeidenView leiden(g, static_cast<int>(params.iterations.getParamVal()),
            /*randomize=*/true, params.gamma.getParamVal());
        leiden.run();
        const auto& partition = leiden.getPartition();
        std::vector<int64_t> communityIds(numNodes, -1);
        for (offset_t i = 0; i < numNodes && i < partition.numberOfElements(); ++i) {
            communityIds[i] = static_cast<int64_t>(partition[i]);
        }
        return communityIds;
    }
};

function_set GDSLeidenFunction::getFunctionSet() {
    return GDSPerNodeScalarAlgo<GDSLeidenDesc>::getFunctionSet();
}

} // namespace algo_extension
} // namespace lbug
