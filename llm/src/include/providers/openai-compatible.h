#pragma once

#include <string>

namespace lbug {
namespace main {
class ClientContext;
}

namespace llm_extension {

struct CompletionRequest {
    std::string systemPrompt;
    std::string userPrompt;
    std::string model;
    std::string baseURL;
    std::string apiKey;
    main::ClientContext* clientContext;
};

class OpenAICompatibleCompletion {
public:
    std::string complete(const CompletionRequest& request) const;
};

} // namespace llm_extension
} // namespace lbug
