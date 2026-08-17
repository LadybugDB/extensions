#include "providers/openai-compatible.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "common/exception/connection.h"
#include "common/exception/interrupt.h"
#include "common/exception/runtime.h"
#include "common/json.h"
#include "main/client_context.h"
#include <curl/curl.h>

using namespace lbug::common;

namespace lbug {
namespace llm_extension {

namespace {

constexpr long TOTAL_TIMEOUT_SECONDS = 120;
constexpr long DEFAULT_CONNECT_TIMEOUT_SECONDS = 10;

void ensureCurlInitialized() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw RuntimeException("Could not initialize libcurl for AI_EXTRACT.");
        }
    });
}

struct CurlHandle {
    CurlHandle() : handle{curl_easy_init()} {}
    ~CurlHandle() {
        if (handle != nullptr) {
            curl_easy_cleanup(handle);
        }
    }

    CURL* handle;
};

struct CurlHeaders {
    ~CurlHeaders() { curl_slist_free_all(headers); }

    void append(const char* header) {
        auto* appended = curl_slist_append(headers, header);
        if (appended == nullptr) {
            throw RuntimeException("Could not allocate libcurl request headers for AI_EXTRACT.");
        }
        headers = appended;
    }

    curl_slist* headers = nullptr;
};

void ensureCurlOption(CURLcode result, const char* option) {
    if (result != CURLE_OK) {
        throw RuntimeException(std::string("Could not configure libcurl ") + option +
                               " for AI_EXTRACT: " + curl_easy_strerror(result));
    }
}

std::string normalizeBaseURL(std::string baseURL) {
    while (!baseURL.empty() && baseURL.back() == '/') {
        baseURL.pop_back();
    }
    if (baseURL.empty() ||
        (baseURL.rfind("http://", 0) != 0 && baseURL.rfind("https://", 0) != 0)) {
        throw RuntimeException("AI_EXTRACT endpoint must be a valid HTTP(S) base URL.");
    }
    if (std::any_of(baseURL.begin(), baseURL.end(),
            [](unsigned char c) { return std::iscntrl(c) || std::isspace(c); })) {
        throw RuntimeException(
            "AI_EXTRACT endpoint must not contain whitespace or control characters.");
    }
    const auto schemeEnd = baseURL.find("://") + 3;
    const auto authorityEnd = baseURL.find_first_of("/?#", schemeEnd);
    if (authorityEnd == schemeEnd || baseURL.find_first_of("?#") != std::string::npos) {
        throw RuntimeException("AI_EXTRACT endpoint must be a valid HTTP(S) base URL.");
    }
    return baseURL;
}

size_t writeResponse(char* data, size_t size, size_t count, void* userData) {
    const auto bytes = size * count;
    auto* response = static_cast<std::string*>(userData);
    response->append(data, bytes);
    return bytes;
}

int checkInterrupted(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* clientContext = static_cast<main::ClientContext*>(userData);
    if (clientContext->interrupted()) {
        return 1;
    }
    if (clientContext->hasTimeout() && clientContext->getTimeoutRemainingInMS() == 0) {
        clientContext->interrupt();
        return 1;
    }
    return 0;
}

long cappedTimeoutSeconds(main::ClientContext* clientContext) {
    auto timeout = TOTAL_TIMEOUT_SECONDS;
    if (clientContext->hasTimeout()) {
        const auto remainingMs = clientContext->getTimeoutRemainingInMS();
        if (remainingMs == 0) {
            clientContext->interrupt();
            throw InterruptException{};
        }
        timeout = std::min(timeout, std::max<long>(1, (remainingMs + 999) / 1000));
    }
    return timeout;
}

std::string buildPayload(const CompletionRequest& request) {
    auto* doc = yyjson_mut_doc_new(nullptr);
    auto* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "model", request.model.c_str());
    yyjson_mut_obj_add_real(doc, root, "temperature", 0);
    auto* messages = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "messages", messages);
    for (const auto& [role, content] :
        {std::pair{"system", request.systemPrompt}, std::pair{"user", request.userPrompt}}) {
        auto* message = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, message, "role", role);
        yyjson_mut_obj_add_strcpy(doc, message, "content", content.c_str());
        yyjson_mut_arr_append(messages, message);
    }
    yyjson_write_err writeError;
    char* payload = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, nullptr, nullptr, &writeError);
    if (payload == nullptr) {
        yyjson_mut_doc_free(doc);
        auto message =
            writeError.msg == nullptr ? "unknown JSON serialization error" : writeError.msg;
        throw RuntimeException(
            std::string("AI_EXTRACT could not serialize the provider request: ") + message);
    }
    std::string result(payload);
    free(payload);
    yyjson_mut_doc_free(doc);
    return result;
}

std::string parseCompletionContent(const std::string& body) {
    auto* doc = yyjson_read(body.c_str(), body.size(), 0);
    if (doc == nullptr) {
        throw RuntimeException("AI_EXTRACT provider returned malformed JSON.");
    }
    auto* root = yyjson_doc_get_root(doc);
    auto* choices =
        root != nullptr && yyjson_is_obj(root) ? yyjson_obj_get(root, "choices") : nullptr;
    auto* firstChoice =
        choices != nullptr && yyjson_is_arr(choices) ? yyjson_arr_get_first(choices) : nullptr;
    auto* message = firstChoice != nullptr && yyjson_is_obj(firstChoice) ?
                        yyjson_obj_get(firstChoice, "message") :
                        nullptr;
    auto* content =
        message != nullptr && yyjson_is_obj(message) ? yyjson_obj_get(message, "content") : nullptr;
    if (content == nullptr || !yyjson_is_str(content)) {
        yyjson_doc_free(doc);
        throw RuntimeException(
            "AI_EXTRACT provider response is missing choices[0].message.content.");
    }
    const auto* resultText = yyjson_get_str(content);
    if (resultText == nullptr) {
        yyjson_doc_free(doc);
        throw RuntimeException("AI_EXTRACT provider response content is null.");
    }
    std::string result(resultText);
    yyjson_doc_free(doc);
    return result;
}

} // namespace

std::string OpenAICompatibleCompletion::complete(const CompletionRequest& request) const {
    if (request.clientContext == nullptr) {
        throw RuntimeException("AI_EXTRACT requires an active client context.");
    }
    if (request.apiKey.empty()) {
        throw RuntimeException("AI_EXTRACT api_key must not be NULL or empty.");
    }
    ensureCurlInitialized();
    const auto timeout = cappedTimeoutSeconds(request.clientContext);
    const auto connectTimeout = std::min(DEFAULT_CONNECT_TIMEOUT_SECONDS, timeout);
    const auto url = normalizeBaseURL(request.baseURL) + "/chat/completions";
    const auto payload = buildPayload(request);

    CurlHandle curl;
    if (curl.handle == nullptr) {
        throw RuntimeException("Could not initialize libcurl request for AI_EXTRACT.");
    }
    std::string response;
    CurlHeaders headers;
    headers.append("Content-Type: application/json");
    const auto authorization = "Authorization: Bearer " + request.apiKey;
    headers.append(authorization.c_str());

    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str()), "URL");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_POST, 1L), "POST");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, payload.data()),
        "POSTFIELDS");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(payload.size())),
        "POSTFIELDSIZE_LARGE");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.headers),
        "HTTPHEADER");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, writeResponse),
        "WRITEFUNCTION");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &response), "WRITEDATA");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT, timeout), "TIMEOUT");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT, connectTimeout),
        "CONNECTTIMEOUT");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_FOLLOWLOCATION, 0L), "FOLLOWLOCATION");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_MAXREDIRS, 0L), "MAXREDIRS");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L), "NOSIGNAL");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L), "NOPROGRESS");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, checkInterrupted),
        "XFERINFOFUNCTION");
    ensureCurlOption(curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA, request.clientContext),
        "XFERINFODATA");

    const auto curlResult = curl_easy_perform(curl.handle);
    long status = 0;
    ensureCurlOption(curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status),
        "RESPONSE_CODE");

    if (curlResult == CURLE_ABORTED_BY_CALLBACK && request.clientContext->interrupted()) {
        throw InterruptException{};
    }
    if (curlResult != CURLE_OK) {
        throw ConnectionException(
            std::string("AI_EXTRACT request failed: ") + curl_easy_strerror(curlResult));
    }
    if (status < 200 || status >= 300) {
        throw ConnectionException(
            "AI_EXTRACT provider returned HTTP status " + std::to_string(status) + ".");
    }
    return parseCompletionContent(response);
}

} // namespace llm_extension
} // namespace lbug
