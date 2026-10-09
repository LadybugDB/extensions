#pragma once
#include "catalog/catalog_entry/node_table_catalog_entry.h"
#include "function/fts_config.h"
#include "main/client_context.h"
#include <format>
#include <memory>

namespace cppjieba {
class Jieba;
}

namespace lbug {
namespace storage {
class NodeTable;
}

namespace fts_extension {

struct FTSUtils {

    static void normalizeQuery(std::string& query, const regex::RE2& ignorePattern,
        bool protectWildcardChars = false);

    static bool hasWildcardPattern(const std::string& term);

    static std::vector<std::string> stemTerms(std::vector<std::string> terms,
        const FTSConfig& config, storage::MemoryManager* mm, storage::NodeTable* stopwordsTable,
        transaction::Transaction* tx, bool isConjunctive, bool isQuery);

    static std::string getDefaultStopWordsTableName() {
        return std::format("default_english_stopwords");
    }

    static std::string getInternalTablePrefix(common::table_id_t tableID,
        const std::string& indexName) {
        return std::format("{}_{}", tableID, indexName);
    }

    static std::string getNonDefaultStopWordsTableName(common::table_id_t tableID,
        const std::string& indexName) {
        return std::format("{}_stopwords", getInternalTablePrefix(tableID, indexName));
    }

    static std::string getDocsTableName(common::table_id_t tableID, const std::string& indexName) {
        return std::format("{}_docs", getInternalTablePrefix(tableID, indexName));
    }

    static std::string getAppearsInfoTableName(common::table_id_t tableID,
        const std::string& indexName) {
        return std::format("{}_appears_info", getInternalTablePrefix(tableID, indexName));
    }

    static std::string getTermsTableName(common::table_id_t tableID, const std::string& indexName) {
        return std::format("{}_terms", getInternalTablePrefix(tableID, indexName));
    }

    static std::string getAppearsInTableName(common::table_id_t tableID,
        const std::string& indexName) {
        return std::format("{}_appears_in", getInternalTablePrefix(tableID, indexName));
    }

    static std::string getTokenizeMacroName(common::table_id_t tableID,
        const std::string& indexName) {
        return std::format("{}_tokenize", getInternalTablePrefix(tableID, indexName));
    }

    static std::vector<std::string> tokenizeString(std::string& str, const FTSConfig& tokenizer);

    // Process-level cppjieba cache keyed by dictionary directory. Constructing a
    // cppjieba::Jieba reloads the ~14MB dictionary set on every call, which dominated
    // QUERY_FTS_INDEX latency (~0.5-1.2s fixed cost per call). Missing dictionaries
    // raise a catchable BinderException here instead of cppjieba's XCHECK abort(),
    // which kills the host process and cannot be defended against from bindings.
    static std::shared_ptr<cppjieba::Jieba> getSharedJieba(const std::string& dictDir);
};

} // namespace fts_extension
} // namespace lbug

