// GDS_LOUVAIN — Louvain community detection backed by icebug (NetworKit's PLM, Parallel
// Louvain Method). Coexists with the hand-rolled LOUVAIN for one release cycle. All bridge
// plumbing lives in GDSPerNodeScalarAlgo (gds_algo_template.h); this file is the descriptor.
// Prefer GDS_LEIDEN for new work — better partition quality on most graphs.
#include "function/algo_function.h"
#include "function/gds_algo_template.h"
#include <networkit/community/PLM.hpp>
#include <networkit/structures/Partition.hpp>

using namespace lbug::common;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

// No optional params (PLM defaults).
struct GDSLouvainOptionalParams final : public function::OptionalParams {
    explicit GDSLouvainOptionalParams(const binder::expression_vector& optionalParams) {
        if (!optionalParams.empty()) {
            throw BinderException{"Unknown optional parameter: " + optionalParams[0]->getAlias()};
        }
    }
    void evaluateParams(main::ClientContext*) override {}
    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSLouvainOptionalParams>(binder::expression_vector{});
    }
};

struct GDSLouvainDesc {
    static constexpr const char* name = GDSLouvainFunction::name;
    static constexpr const char* OUTPUT_COLUMN = "community_id";
    using OutputType = int64_t;
    using Params = GDSLouvainOptionalParams;
    using Extra = NoExtraArgs;

    static std::vector<int64_t> run(const NetworKit::GraphR& g, offset_t numNodes, const Params&,
        const Extra::type&) {
        NetworKit::PLM plm(g);
        plm.run();
        const auto& partition = plm.getPartition();
        std::vector<int64_t> communityIds(numNodes, -1);
        for (offset_t i = 0; i < numNodes && i < partition.numberOfElements(); ++i) {
            communityIds[i] = static_cast<int64_t>(partition[i]);
        }
        return communityIds;
    }
};

function_set GDSLouvainFunction::getFunctionSet() {
    return GDSPerNodeScalarAlgo<GDSLouvainDesc>::getFunctionSet();
}

} // namespace algo_extension
} // namespace lbug
