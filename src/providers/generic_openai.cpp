// =============================================================================
// agent-lib-MK3 — GenericOpenAI Client Implementation
// =============================================================================

#include "generic_openai.hpp"
#include "../core/agent.hpp"  // g_running
#include <sstream>
#include <iostream>
#include <regex>
#include <chrono>
#include <thread>
#include <filesystem>
#include <unistd.h>

namespace cortex::mk3::providers {

CodexCliProvider::CodexCliProvider() = default;

std::string CodexCliProvider::shellEscape(const std::string& input) {
    std::string out(1, '\'');
    for (char c : input) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

std::string CodexCliProvider::renderPrompt(const ChatMessages& msgs) {
    std::ostringstream ss;
    ss << "Complete the embedded Cortex MK3 transcript below. Treat it as the task content for this Codex exec run.\n"
       << "Base answers about available tools or sub-agents only on the embedded MK3 [system] message.\n"
       << "If the embedded MK3 [system] message has no <subagents> block, the correct answer is that no MK3 sub-agents are available.\n"
       << "Return the MK3 XML protocol text requested by the embedded transcript. Do not add commentary about Codex or this wrapper.\n\n";
    for (const auto& m : msgs) {
        ss << "[" << ChatMessage::roleName(m.role) << "]\n" << m.content << "\n\n";
    }
    return ss.str();
}

std::string CodexCliProvider::generate(const ChatMessages& msgs) {
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path() /
        ("cortex-codex-" + std::to_string(::getpid()) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path inputPath = base.string() + ".in";
    fs::path outputPath = base.string() + ".out";
    fs::path jsonlPath = base.string() + ".jsonl";
    fs::path errPath = base.string() + ".err";

    {
        std::ofstream in(inputPath);
        in << renderPrompt(msgs);
    }

    std::string cmd = "codex exec --json --model " + shellEscape(model_) +
        " -c " + shellEscape("model_reasoning_effort=\"" + reasoningEffort_ + "\"") +
        " --sandbox read-only --skip-git-repo-check --cd " + shellEscape(fs::current_path().string()) +
        " --output-last-message " + shellEscape(outputPath.string()) +
        " - < " + shellEscape(inputPath.string()) +
        " > " + shellEscape(jsonlPath.string()) +
        " 2> " + shellEscape(errPath.string());

    int rc = std::system(cmd.c_str());
    std::ifstream outFile(outputPath);
    std::ostringstream out;
    out << outFile.rdbuf();
    std::string result = out.str();

    std::ifstream errFile(errPath);
    std::ostringstream err;
    err << errFile.rdbuf();

    std::error_code ec;
    fs::remove(inputPath, ec);
    fs::remove(outputPath, ec);
    fs::remove(jsonlPath, ec);
    fs::remove(errPath, ec);

    if (rc != 0 || result.empty()) {
        throw std::runtime_error("codex exec failed: " + err.str());
    }
    return result;
}

void CodexCliProvider::generateStream(const ChatMessages& msgs, StreamCallback cb) {
    std::string result = generate(msgs);
    cb(result, true);
}

std::vector<ILlmProvider::ModelInfo> CodexCliProvider::listModels() {
    return {{"gpt-5.5", "GPT-5.5", 272000, false}};
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
GenericOpenAIClient::GenericOpenAIClient(const OpenAIProviderConfig& cfg)
    : config_(cfg), apiKey_(cfg.resolveApiKey()), model_(cfg.defaultModel), maxTokens_(cfg.defaultMaxTokens) {}

// ---------------------------------------------------------------------------
// Build OpenAI-compatible JSON request body
// ---------------------------------------------------------------------------
Json::Value GenericOpenAIClient::buildRequestBody(const ChatMessages& msgs, bool stream) const {
    Json::Value body;
    body["model"] = model_;
    body["temperature"] = temperature_;
    body["top_p"] = topP_;
    if (topK_ > 0 && config_.supportsTopK) body["top_k"] = topK_;
    if (!config_.reasoningEffort.empty()) body["reasoning_effort"] = config_.reasoningEffort;
    if (presencePenalty_ != 0.0) body["presence_penalty"] = presencePenalty_;
    if (frequencyPenalty_ != 0.0) body["frequency_penalty"] = frequencyPenalty_;
    body["max_tokens"] = maxTokens_;
    body["stream"] = stream;

    Json::Value messages(Json::arrayValue);
    for (const auto& m : msgs) {
        Json::Value msg;
        msg["role"] = ChatMessage::roleName(m.role);
        msg["content"] = m.content;
        if (m.role == ChatRole::TOOL) {
            msg["tool_call_id"] = m.toolCallId;
            msg["name"] = m.name;
        }
        messages.append(msg);
    }
    body["messages"] = messages;

    return body;
}

// ---------------------------------------------------------------------------
// Non-streaming generate
// ---------------------------------------------------------------------------
std::string GenericOpenAIClient::generate(const ChatMessages& msgs) {
    Json::Value body = buildRequestBody(msgs, false);
    std::string response = httpPost(config_.baseUrl + config_.chatEndpoint, body);

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(response);
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        throw std::runtime_error("Failed to parse JSON response: " + errs);
    }

    if (root.isMember("error")) {
        std::string errMsg = "unknown error";
        auto& err = root["error"];
        if (err.isObject() && err.isMember("message")) errMsg = err["message"].asString();
        else if (err.isString()) errMsg = err.asString();
        throw std::runtime_error("API error: " + errMsg);
    }

    auto& choices = root["choices"];
    if (choices.size() > 0) {
        auto& msg = choices[0]["message"];
        return msg["content"].asString();
    }

    return "";
}

// ---------------------------------------------------------------------------
// Streaming generate
// ---------------------------------------------------------------------------
void GenericOpenAIClient::generateStream(const ChatMessages& msgs, StreamCallback cb) {
    Json::Value body = buildRequestBody(msgs, true);
    httpPost(config_.baseUrl + config_.chatEndpoint, body, cb, true);
}

// ---------------------------------------------------------------------------
// HTTP POST (shared between streaming and non-streaming)
// ---------------------------------------------------------------------------
std::string GenericOpenAIClient::httpPost(const std::string& url,
                                           const Json::Value& body,
                                           StreamCallback cb, bool stream) {
    // Serialize body once (doesn't change across retries)
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string bodyStr = Json::writeString(writer, body);

    for (int retry = 0; retry <= maxRetries_; retry++) {
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("Failed to initialize CURL");

    std::string responseBuffer;
    StreamCtx ctx{cb, {}, {}};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)bodyStr.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    std::string authHeader = "Authorization: Bearer " + apiKey_;
    headers = curl_slist_append(headers, authHeader.c_str());
    for (const auto& [k, v] : config_.extraHeaders) {
        std::string h = k + ": " + v;
        headers = curl_slist_append(headers, h.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (stream) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abortCheckCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);
    }

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        bool isRetryable = (res == CURLE_RECV_ERROR || res == CURLE_SEND_ERROR
                         || res == CURLE_PARTIAL_FILE || res == CURLE_GOT_NOTHING
                         || res == CURLE_OPERATION_TIMEDOUT);
        if (isRetryable && retry < maxRetries_) {
            int waitSec = std::min((1 << (retry + 1)) * 5, 120);
            std::cerr << "[MK3:RETRY] CURL error " << res << " — retrying in "
                      << waitSec << "s (attempt " << (retry + 1) << "/" << maxRetries_ << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(waitSec));
            continue;
        }
        throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
    }

    if (httpCode >= 400) {
        std::string errorBody = stream ?
            (ctx.lastErrorBody.empty() ? ctx.buffer : ctx.lastErrorBody) : responseBuffer;
        bool isRetryable = (httpCode == 429) ||
            (httpCode == 413 && errorBody.find("rate_limit_exceeded") != std::string::npos);

        if (isRetryable && retry < maxRetries_) {
            int waitSec = std::min((1 << (retry + 1)) * 5, 120);
            std::cerr << "[MK3:RETRY] HTTP " << httpCode << " — retrying in "
                      << waitSec << "s (attempt " << (retry + 1) << "/" << maxRetries_ << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(waitSec));
            continue;
        }
        throw std::runtime_error("HTTP " + std::to_string(httpCode) +
            " — response: " + errorBody.substr(0, 500));
    }
    return responseBuffer;
    } // retry loop
    throw std::runtime_error("max retries exceeded");
}

// ---------------------------------------------------------------------------
// CURL write callback (non-streaming)
// ---------------------------------------------------------------------------
size_t GenericOpenAIClient::writeCb(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    size_t total = sz * nmemb;
    buf->append(static_cast<char*>(ptr), total);
    return total;
}

// ---------------------------------------------------------------------------
// CURL write callback (streaming SSE)
// ---------------------------------------------------------------------------
size_t GenericOpenAIClient::streamCb(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<StreamCtx*>(userdata);
    size_t total = sz * nmemb;
    std::string chunk(static_cast<char*>(ptr), total);
    // If chunk looks like a JSON error (not SSE format), save it directly
    if (chunk.size() > 2 && chunk[0] == '{') {
        ctx->lastErrorBody += chunk;
    }
    ctx->buffer += chunk;

    // Process complete SSE lines
    size_t pos;
    while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
        std::string line = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + 1);

        // Trim trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Skip empty lines and non-data lines
        if (line.empty() || line.rfind("data: ", 0) != 0) continue;

        std::string data = line.substr(6); // strip "data: "

        // Check for [DONE] signal
        if (data == "[DONE]") {
            ctx->cb("", true);
            continue;
        }

        // Parse JSON
        Json::Value root;
        Json::CharReaderBuilder reader;
        std::string errs;
        std::istringstream ss(data);
        if (!Json::parseFromStream(reader, ss, &root, &errs)) continue;

        auto& choices = root["choices"];
        if (choices.size() == 0) continue;

        auto& delta = choices[0]["delta"];

        // Check for content and reasoning_content (DeepSeek TTC/thinking)
        std::string content;
        bool isThinking = false;
        if (delta.isMember("content") && !delta["content"].isNull()) {
            content = delta["content"].asString();
        } else if (delta.isMember("reasoning_content") && !delta["reasoning_content"].isNull()) {
            content = delta["reasoning_content"].asString();
            isThinking = true;
        }

        if (!content.empty()) {
            // Prefix thinking tokens with SOH byte for downstream routing
            std::string token = isThinking ? std::string("\x01") + content : content;
            std::string finishReason;
            if (choices[0].isMember("finish_reason") && !choices[0]["finish_reason"].isNull())
                finishReason = choices[0]["finish_reason"].asString();

            bool isFinal = (finishReason == "stop" || finishReason == "length" ||
                           finishReason == "tool_calls");
            ctx->cb(token, isFinal);
        }
    }

    return total;
}

// ── CURL progress callback: abort transfer when g_running becomes false ──
int GenericOpenAIClient::abortCheckCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_running ? 0 : 1;  // return 1 to abort
}

// ---------------------------------------------------------------------------
// Model listing
// ---------------------------------------------------------------------------
std::vector<ILlmProvider::ModelInfo> GenericOpenAIClient::listModels() {
    if (modelsFetched_) return cachedModels_;

    CURL* curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    std::string url = config_.baseUrl + config_.modelsEndpoint;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    struct curl_slist* headers = nullptr;
    if (!apiKey_.empty()) {
        std::string auth = "Authorization: Bearer " + apiKey_;
        headers = curl_slist_append(headers, auth.c_str());
    }
    for (const auto& [k, v] : config_.extraHeaders) {
        std::string h = k + ": " + v;
        headers = curl_slist_append(headers, h.c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(response);
    if (!Json::parseFromStream(reader, ss, &root, &errs)) return {};

    if (root.isMember("error")) return {};
    Json::Value& data = (root.isObject() && root.isMember("data")) ? root["data"] : root;
    if (!data.isArray()) return {};

    for (auto& m : data) {
        if (!m.isObject() || !m.isMember("id") || !m["id"].isString()) continue;
        ModelInfo info;
        info.id = m["id"].asString();
        info.name = (m.isMember("name") && m["name"].isString()) ? m["name"].asString() : info.id;
        info.contextWindow = m.get("context_window", config_.name == "openai-codex" ? 272000 : 65536).asInt();
        info.isFree = (info.id.find(":free") != std::string::npos || info.name.find(":free") != std::string::npos);
        if (!info.isFree && m.isMember("pricing") && m["pricing"].isObject()) {
            auto zeroish = [](const Json::Value& v) {
                if (v.isString()) return v.asString() == "0" || v.asString() == "0.0";
                if (v.isNumeric()) return v.asDouble() == 0.0;
                return false;
            };
            bool promptFree = m["pricing"].isMember("prompt") && zeroish(m["pricing"]["prompt"]);
            bool completionFree = m["pricing"].isMember("completion") && zeroish(m["pricing"]["completion"]);
            info.isFree = promptFree && completionFree;
        }
        cachedModels_.push_back(info);
    }

    modelsFetched_ = true;
    return cachedModels_;
}

} // namespace cortex::mk3::providers
