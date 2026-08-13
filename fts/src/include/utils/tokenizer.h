#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lbug {
namespace fts_extension {

// A tokenizer converts a piece of text (a document or a query) into a list of
// searchable terms. Each tokenizer is identified by a name and created from a
// set of string parameters (e.g. a dictionary directory).
class ITokenizer {
public:
    virtual ~ITokenizer() = default;

    virtual std::vector<std::string> tokenize(const std::string& text) const = 0;

    virtual std::string getName() const = 0;
};

using TokenizerParams = std::unordered_map<std::string, std::string>;
using TokenizerFactory =
    std::function<std::unique_ptr<ITokenizer>(const TokenizerParams& params)>;

// Tokenizers are registered by name. Adding a new tokenizer (e.g. 'mecab' for
// Japanese) only requires registering a factory here; TOKENIZE and
// CREATE_FTS_INDEX pick it up automatically.
class TokenizerRegistry {
public:
    static void registerTokenizer(const std::string& name, TokenizerFactory factory);

    static bool isSupported(const std::string& name);

    static std::unique_ptr<ITokenizer> create(const std::string& name,
        const TokenizerParams& params);

    static std::vector<std::string> getSupportedNames();

private:
    static std::unordered_map<std::string, TokenizerFactory>& getRegistry();
};

// Constructing a tokenizer can be expensive (the jieba tokenizer loads and
// indexes a dictionary on construction), so instances are cached per
// (name, params) and shared across index inserts and queries. Tokenizer
// instances must be safe for concurrent use (jieba queries are read-only
// after construction).
class TokenizerPool {
public:
    static std::shared_ptr<const ITokenizer> getOrCreate(const std::string& name,
        const TokenizerParams& params);

private:
    static std::string makeKey(const std::string& name, const TokenizerParams& params);
};

} // namespace fts_extension
} // namespace lbug
