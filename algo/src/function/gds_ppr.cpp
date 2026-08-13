// GDS_PPR — personalized PageRank backed by icebug (NetworKit's PageRank::forSources).
// Random walk with restart: teleportation is restricted to the caller-supplied source nodes
// (uniform over the set), so scores measure standing *relative to those anchors* rather than
// globally. This is the trust-propagation primitive: run it from a viewpoint's attested keys
// and every node inherits a confidence score relative to that viewpoint.
//
// CALL GDS_PPR('G', [rowid, ...]) — sources are node rowids, the same identity the
// materialized CSR is built on. All bridge plumbing lives in GDSPerNodeScalarAlgo
// (gds_algo_template.h); this file is the descriptor.
#include "common/string_utils.h"
#include "function/algo_function.h"
#include "function/config/page_rank_config.h"
#include "function/gds_algo_template.h"
#include <networkit/centrality/PageRank.hpp>

using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

struct GDSPprOptionalParams final : public function::OptionalParams {
    OptionalParam<DampingFactor> dampingFactor;
    OptionalParam<Tolerance> tolerance;

    explicit GDSPprOptionalParams(const expression_vector& optionalParams) {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == DampingFactor::NAME) {
                dampingFactor = function::OptionalParam<DampingFactor>(optionalParam);
            } else if (paramName == Tolerance::NAME) {
                tolerance = function::OptionalParam<Tolerance>(optionalParam);
            } else {
                throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    GDSPprOptionalParams(OptionalParam<DampingFactor> dampingFactor,
        OptionalParam<Tolerance> tolerance)
        : dampingFactor{std::move(dampingFactor)}, tolerance{std::move(tolerance)} {}

    void evaluateParams(main::ClientContext* context) override {
        dampingFactor.evaluateParam(context);
        tolerance.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSPprOptionalParams>(dampingFactor, tolerance);
    }
};

struct GDSPprDesc {
    static constexpr const char* name = GDSPprFunction::name;
    static constexpr const char* OUTPUT_COLUMN = "rank";
    using OutputType = double;
    using Params = GDSPprOptionalParams;
    using Extra = SourceListArg;

    static std::vector<double> run(const NetworKit::GraphR& g, offset_t /*numNodes*/,
        const Params& params, const Extra::type& sources) {
        const std::vector<NetworKit::node> nkSources{sources.begin(), sources.end()};
        auto pr = NetworKit::PageRank::forSources(g, nkSources, params.dampingFactor.getParamVal(),
            params.tolerance.getParamVal());
        pr.run();
        return pr.scores();
    }
};

function_set GDSPprFunction::getFunctionSet() {
    return GDSPerNodeScalarAlgo<GDSPprDesc>::getFunctionSet();
}

} // namespace algo_extension
} // namespace lbug
