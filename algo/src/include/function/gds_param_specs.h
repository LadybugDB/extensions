#pragma once

// Shared NetworKit knob specs for the table-driven GDS registry (gds_registry.cpp).
//
// Each spec is a tiny struct consumed by GDSGenericParams<Specs...> (gds_generic_params.h),
// which reuses function::OptionalParam<Spec> for parsing, literal evaluation and validation.
// A spec needs only:
//   static constexpr const char* NAME;     // lower-case match against the CALL-site alias
//   static constexpr LogicalTypeID TYPE;   // DOUBLE, INT64 or BOOL
//   static constexpr T DEFAULT_VALUE;
//   optional: static void validate(T);     // range check, run at bind time
//   optional: static constexpr bool REQUIRED = true;  // no usable default: CALL must pass it
//
// Reuse order: prefer the existing specs (function::DampingFactor / function::Tolerance from
// page_rank_config.h, MaxIterations from max_iterations_config.h) over adding near-duplicates
// here. Add a spec here only when no existing one matches the NetworKit default/semantics.

#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/config/page_rank_config.h"

namespace lbug {
namespace algo_extension {

// Normalized flag shared by most centrality measures. NetworKit's default differs per
// algorithm (false for Betweenness/Degree, true for Closeness/Harmonic), so the default is a
// template parameter: spell Normalized (false) or NormalizedDefaultTrue (true) in the row to
// match the wrapped constructor's default.
template<bool Default>
struct NormalizedT {
    static constexpr const char* NAME = "normalized";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::BOOL;
    static constexpr bool DEFAULT_VALUE = Default;
};
using Normalized = NormalizedT<false>;
using NormalizedDefaultTrue = NormalizedT<true>;

// Katz centrality knobs (NetworKit::KatzCentrality(G, alpha, beta, tol)).
struct KatzAlpha {
    static constexpr const char* NAME = "alpha";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    // 0 lets NetworKit pick a safe damping factor from the spectrum.
    static constexpr double DEFAULT_VALUE = 0.0;

    static void validate(double alpha) {
        if (alpha < 0) {
            throw common::BinderException{"Alpha must be non-negative."};
        }
    }
};

struct KatzBeta {
    static constexpr const char* NAME = "beta";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 0.1;
};

// Leiden resolution (NetworKit::ParallelLeidenView). > 1 favors more/smaller communities.
struct LeidenGamma {
    static constexpr const char* NAME = "gamma";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 1.0;

    static void validate(double gamma) {
        if (gamma <= 0) {
            throw common::BinderException{"Gamma must be positive."};
        }
    }
};

struct LeidenIterations {
    static constexpr const char* NAME = "iterations";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = 3;

    static void validate(int64_t iterations) {
        if (iterations <= 0) {
            throw common::BinderException{"Iterations must be positive."};
        }
    }
};

// Label-propagation (NetworKit::PLP(G, theta, maxIterations)). NetworKit's default for both is
// `none` (no cap); -1 is the CALL-site sentinel for it.
struct PLPTheta {
    static constexpr const char* NAME = "theta";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = -1;

    static void validate(int64_t theta) {
        if (theta < -1) {
            throw common::BinderException{"Theta must be -1 (no cap) or non-negative."};
        }
    }
};

struct PLPMaxIterations {
    static constexpr const char* NAME = "maxiterations";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = -1;

    static void validate(int64_t maxIterations) {
        if (maxIterations < -1) {
            throw common::BinderException{"Max iterations must be -1 (no cap) or non-negative."};
        }
    }
};

// Parallel k-core sampling (NetworKit::ParallelCoreDecomposition(G, normalized, useSampling)).
// High-degree sampling reduces atomic contention on dense graphs; a validation failure
// restarts the run without sampling, so results stay exact either way.
struct KCoreUseSampling {
    static constexpr const char* NAME = "usesampling";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::BOOL;
    static constexpr bool DEFAULT_VALUE = true;
};

// Cut clustering (NetworKit::CutClustering(G, alpha)) takes alpha as a *required* constructor
// argument, so the row declares it REQUIRED: CALL must pass alpha := .... DEFAULT_VALUE is
// only filler for OptionalParam's storage; evaluateParams() rejects an unset required param.
struct CutAlpha {
    static constexpr const char* NAME = "alpha";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 0.0;
    static constexpr bool REQUIRED = true;

    static void validate(double alpha) {
        if (alpha < 0) {
            throw common::BinderException{"Alpha must be non-negative."};
        }
    }
};

// FastRP embedding knobs (NetworKit::FastRP). Weights stay fixed at the python default
// (1, 1, 1); dimension/seed/normalization are exposed.
struct FastRPDimension {
    static constexpr const char* NAME = "dimension";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = 128;

    static void validate(int64_t dimension) {
        if (dimension <= 0) {
            throw common::BinderException{"Dimension must be positive."};
        }
    }
};

struct FastRPSeed {
    static constexpr const char* NAME = "seed";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = 42;

    static void validate(int64_t seed) {
        if (seed < 0) {
            throw common::BinderException{"Seed must be non-negative."};
        }
    }
};

struct FastRPNormalization {
    static constexpr const char* NAME = "normalization";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::BOOL;
    static constexpr bool DEFAULT_VALUE = true;
};

} // namespace algo_extension
} // namespace lbug
