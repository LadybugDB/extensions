// GDS_PAGE_RANK — PageRank backed by icebug (a NetworKit fork with zero-copy Arrow CSR ingest).
// Same CALL surface as the hand-rolled PAGE_RANK, but the compute is delegated to libnetworkit.
// All bridge plumbing lives in GDSPerNodeScalarAlgo (gds_algo_template.h); this file is the
// algorithm descriptor: name, output column, optional params, and the NetworKit invocation.
#include "common/string_utils.h"
#include "function/algo_function.h"
#include "function/config/max_iterations_config.h"
#include "function/config/page_rank_config.h"
#include "function/gds_algo_template.h"
#include <networkit/centrality/PageRank.hpp>

using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

// Reuse the PAGE_RANK optional params (dampingFactor, tolerance, maxIterations, normalize).
struct GDSPageRankOptionalParams final : public MaxIterationOptionalParams {
    OptionalParam<DampingFactor> dampingFactor;
    OptionalParam<Tolerance> tolerance;

    explicit GDSPageRankOptionalParams(const expression_vector& optionalParams)
        : MaxIterationOptionalParams{constructMaxIterationParam(optionalParams)} {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == DampingFactor::NAME) {
                dampingFactor = function::OptionalParam<DampingFactor>(optionalParam);
            } else if (paramName == Tolerance::NAME) {
                tolerance = function::OptionalParam<Tolerance>(optionalParam);
            } else if (paramName == MaxIterations::NAME) {
                continue;
            } else {
                throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    GDSPageRankOptionalParams(OptionalParam<MaxIterations> maxIterations,
        OptionalParam<DampingFactor> dampingFactor, OptionalParam<Tolerance> tolerance)
        : MaxIterationOptionalParams{maxIterations}, dampingFactor{std::move(dampingFactor)},
          tolerance{std::move(tolerance)} {}

    void evaluateParams(main::ClientContext* context) override {
        MaxIterationOptionalParams::evaluateParams(context);
        dampingFactor.evaluateParam(context);
        tolerance.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSPageRankOptionalParams>(maxIterations, dampingFactor, tolerance);
    }
};

struct GDSPageRankDesc {
    static constexpr const char* name = GDSPageRankFunction::name;
    static constexpr const char* OUTPUT_COLUMN = "rank";
    using OutputType = double;
    using Params = GDSPageRankOptionalParams;
    using Extra = NoExtraArgs;

    static std::vector<double> run(const NetworKit::GraphR& g, offset_t /*numNodes*/,
        const Params& params, const Extra::type&) {
        NetworKit::PageRank pr(g, params.dampingFactor.getParamVal(),
            params.tolerance.getParamVal());
        pr.run();
        return pr.scores();
    }
};

function_set GDSPageRankFunction::getFunctionSet() {
    return GDSPerNodeScalarAlgo<GDSPageRankDesc>::getFunctionSet();
}

} // namespace algo_extension
} // namespace lbug
