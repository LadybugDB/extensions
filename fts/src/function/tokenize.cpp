#include "function/tokenize.h"

#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/string_t.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "expression_evaluator/expression_evaluator_utils.h"
#include "function/scalar_function.h"
#include "re2.h"
#include "utils/tokenizer.h"

namespace lbug {
namespace fts_extension {

using namespace function;
using namespace common;

struct TokenizerBindData final : public FunctionBindData {
    std::shared_ptr<const ITokenizer> tokenizer;

    TokenizerBindData(common::logical_type_vec_t paramTypes,
        std::shared_ptr<const ITokenizer> tokenizer)
        : FunctionBindData{std::move(paramTypes),
              common::LogicalType::LIST(common::LogicalType::STRING())},
          tokenizer{std::move(tokenizer)} {}

    std::unique_ptr<FunctionBindData> copy() const override {
        return std::make_unique<TokenizerBindData>(copyVector(paramTypes), tokenizer);
    }
};

static void addTokensToVector(const std::vector<std::string>& tokens, list_entry_t& result,
    common::ValueVector& resultVector) {
    result = ListVector::addList(&resultVector, tokens.size());
    for (auto i = 0u; i < tokens.size(); i++) {
        ListVector::getDataVector(&resultVector)->setValue(result.offset + i, tokens[i]);
    }
}

struct TokenizeOp {
    static void operation(string_t& text, string_t& /*tokenizerName*/, string_t& /*extraParam*/,
        list_entry_t& result, common::ValueVector& resultVector, void* dataPtr) {
        auto bindData = reinterpret_cast<TokenizerBindData*>(dataPtr);
        auto tokens = bindData->tokenizer->tokenize(text.getAsString());
        addTokensToVector(tokens, result, resultVector);
    }
};

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    if (input.arguments[1]->expressionType != ExpressionType::LITERAL) {
        throw BinderException{"The tokenizer parameter must be a literal expression."};
    }
    if (input.arguments[2]->expressionType != ExpressionType::LITERAL) {
        throw BinderException{"The tokenizer parameter must be a literal expression."};
    }
    auto value = evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(input.arguments[1],
        input.context);
    auto tokenizerName = common::StringUtils::getLower(value.getValue<std::string>());
    if (tokenizerName.empty()) {
        tokenizerName = "simple";
    }
    // The third argument is the tokenizer's extra parameter; for 'jieba' it is
    // the dictionary directory.
    auto extraParam = evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(
        input.arguments[2], input.context)
                          .getValue<std::string>();
    TokenizerParams params;
    if (tokenizerName == "jieba") {
        params["jieba_dict_dir"] = extraParam;
    } else if (tokenizerName == "mecab") {
        params["mecab_dict_dir"] = extraParam;
    }
    auto tokenizer = TokenizerPool::getOrCreate(tokenizerName, params);
    input.definition->ptrCast<ScalarFunction>()->execFunc =
        ScalarFunction::TernaryRegexExecFunction<string_t, string_t, string_t, list_entry_t,
            TokenizeOp>;
    return std::make_unique<TokenizerBindData>(
        binder::ExpressionUtil::getDataTypes(input.arguments), std::move(tokenizer));
}

function::function_set TokenizeFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::STRING, LogicalTypeID::STRING,
            LogicalTypeID::STRING},
        LogicalTypeID::LIST);
    function->bindFunc = bindFunc;
    result.push_back(std::move(function));
    return result;
}

} // namespace fts_extension
} // namespace lbug
