#include "utils/tokenizer.h"

#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "cppjieba/Jieba.hpp"
#include "mecab.h"
#include <algorithm>
#include <format>
#include <mutex>

namespace lbug {
namespace fts_extension {

using namespace common;

class SimpleTokenizer final : public ITokenizer {
public:
    std::vector<std::string> tokenize(const std::string& text) const override {
        return StringUtils::split(text, " ", true /* ignoreEmptyStringParts */);
    }

    std::string getName() const override {
        return "simple";
    }
};

class JiebaTokenizer final : public ITokenizer {
public:
    explicit JiebaTokenizer(const TokenizerParams& params) {
        auto dictDir =
            params.contains("jieba_dict_dir") ? params.at("jieba_dict_dir") : std::string{};
        jieba = std::make_shared<cppjieba::Jieba>(dictDir + "/jieba.dict.utf8",
            dictDir + "/hmm_model.utf8", dictDir + "/user.dict.utf8", dictDir + "/idf.utf8",
            dictDir + "/stop_words.utf8");
    }

    std::vector<std::string> tokenize(const std::string& text) const override {
        std::vector<std::string> tokens;
        jieba->CutForSearch(text, tokens);
        return tokens;
    }

    std::string getName() const override {
        return "jieba";
    }

private:
    std::shared_ptr<cppjieba::Jieba> jieba;
};

std::unordered_map<std::string, TokenizerFactory>& TokenizerRegistry::getRegistry() {
    static std::unordered_map<std::string, TokenizerFactory> registry;
    return registry;
}

// Japanese morphological analyzer. The ipadic dictionary is compiled to a
// UTF-8 binary dictionary at build time (see third_party/mecab).
class MeCabTokenizer final : public ITokenizer {
public:
    explicit MeCabTokenizer(const TokenizerParams& params) {
        auto dictDir =
            params.contains("mecab_dict_dir") ? params.at("mecab_dict_dir") : std::string{};
        tagger.reset(MeCab::createTagger((std::string("-d ") + dictDir).c_str()));
        if (!tagger) {
            throw BinderException{
                std::format("Failed to create mecab tagger with dict dir: '{}'.", dictDir)};
        }
    }

    std::vector<std::string> tokenize(const std::string& text) const override {
        std::vector<std::string> tokens;
        // parseToNode() returns nodes whose surface points into `text`, so the
        // surface must be copied out while `text` is still alive.
        const MeCab::Node* node = tagger->parseToNode(text.c_str());
        for (; node; node = node->next) {
            if (node->stat == MECAB_BOS_NODE || node->stat == MECAB_EOS_NODE) {
                continue;
            }
            if (node->surface == nullptr || node->length == 0) {
                continue;
            }
            tokens.emplace_back(node->surface, node->length);
        }
        return tokens;
    }

    std::string getName() const override {
        return "mecab";
    }

private:
    std::unique_ptr<MeCab::Tagger, decltype(&MeCab::deleteTagger)> tagger{nullptr,
        MeCab::deleteTagger};
};

void TokenizerRegistry::registerTokenizer(const std::string& name, TokenizerFactory factory) {
    getRegistry()[name] = std::move(factory);
}

bool TokenizerRegistry::isSupported(const std::string& name) {
    return getRegistry().contains(name);
}

std::vector<std::string> TokenizerRegistry::getSupportedNames() {
    std::vector<std::string> names;
    for (auto& [name, factory] : getRegistry()) {
        names.push_back(name);
    }
    return names;
}

std::unique_ptr<ITokenizer> TokenizerRegistry::create(const std::string& name,
    const TokenizerParams& params) {
    auto& registry = getRegistry();
    auto it = registry.find(name);
    if (it == registry.end()) {
        // Keep this message in sync with the registered tokenizers; the fts
        // error.test asserts on it verbatim.
        throw BinderException{"Unsupported tokenizer: " + name +
            ".\nSupported tokenizers: 'simple' (default), 'jieba' (Chinese), 'mecab' "
            "(Japanese)"};
    }
    return it->second(params);
}

namespace {

struct BuiltinTokenizers {
    BuiltinTokenizers() {
        TokenizerRegistry::registerTokenizer("simple",
            [](const TokenizerParams&) { return std::make_unique<SimpleTokenizer>(); });
        TokenizerRegistry::registerTokenizer("jieba",
            [](const TokenizerParams& params) {
                return std::make_unique<JiebaTokenizer>(params);
            });
        TokenizerRegistry::registerTokenizer("mecab",
            [](const TokenizerParams& params) {
                return std::make_unique<MeCabTokenizer>(params);
            });
    }
};

} // namespace

static BuiltinTokenizers builtinTokenizers;

std::shared_ptr<const ITokenizer> TokenizerPool::getOrCreate(const std::string& name,
    const TokenizerParams& params) {
    static std::mutex mutex;
    static std::unordered_map<std::string, std::weak_ptr<const ITokenizer>> pool;
    auto key = makeKey(name, params);
    std::lock_guard<std::mutex> guard{mutex};
    if (auto it = pool.find(key); it != pool.end()) {
        if (auto instance = it->second.lock(); instance) {
            return instance;
        }
    }
    auto instance = std::shared_ptr<const ITokenizer>(TokenizerRegistry::create(name, params));
    pool[key] = instance;
    return instance;
}

std::string TokenizerPool::makeKey(const std::string& name, const TokenizerParams& params) {
    std::vector<std::string> keys;
    keys.reserve(params.size());
    for (auto& [key, value] : params) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    std::string key = name;
    for (auto& paramKey : keys) {
        key += std::format("|{}={}", paramKey, params.at(paramKey));
    }
    return key;
}

} // namespace fts_extension
} // namespace lbug
