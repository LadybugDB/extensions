#pragma once

// GDSGenericParams<Specs...>: the single OptionalParams implementation behind every row of
// the GDS registry. The Specs... pack *is* the row's parameter signature: each spec reuses
// function::OptionalParam<Spec>, so parsing, literal evaluation, type coercion and validate()
// behave exactly like the hand-rolled Params structs it replaces.
//
// An empty pack means "no optional params" (any CALL-site optional param is an error) —
// this replaces the per-algorithm "throw on unknown param" boilerplate.

#include <tuple>

#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "function/table/optional_params.h"

namespace lbug {
namespace algo_extension {

template<typename Spec>
static constexpr bool specIsRequired() {
    if constexpr (requires { Spec::REQUIRED; }) {
        return Spec::REQUIRED;
    } else {
        return false;
    }
}

template<typename... Specs>
struct GDSGenericParams final : public function::OptionalParams {
    std::tuple<function::OptionalParam<Specs>...> params;

    explicit GDSGenericParams(const binder::expression_vector& optionalParams) {
        for (auto& optionalParam : optionalParams) {
            auto paramName = common::StringUtils::getLower(optionalParam->getAlias());
            const bool matched = (tryBind<Specs>(paramName, optionalParam) || ... || false);
            if (!matched) {
                throw common::BinderException{
                    "Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    // Copy shares the underlying expressions, matching the hand-rolled Params copy ctors.
    GDSGenericParams(const GDSGenericParams&) = default;

    template<typename Spec>
    typename function::OptionalParam<Spec>::T get() const {
        return std::get<function::OptionalParam<Spec>>(params).getParamVal();
    }

    void evaluateParams(main::ClientContext* context) override {
        std::apply([context](auto&... ps) { (ps.evaluateParam(context), ...); }, params);
        (checkRequired<Specs>(), ...);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<GDSGenericParams>(*this);
    }

private:
    template<typename Spec>
    bool tryBind(const std::string& paramName, const std::shared_ptr<binder::Expression>& expr) {
        if (paramName == Spec::NAME) {
            std::get<function::OptionalParam<Spec>>(params) = function::OptionalParam<Spec>(expr);
            return true;
        }
        return false;
    }

    template<typename Spec>
    void checkRequired() const {
        if constexpr (specIsRequired<Spec>()) {
            if (!std::get<function::OptionalParam<Spec>>(params).isSet()) {
                throw common::BinderException{
                    std::string{"Missing required parameter: "} + Spec::NAME + "."};
            }
        }
    }
};

} // namespace algo_extension
} // namespace lbug
