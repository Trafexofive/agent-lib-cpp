// =============================================================================
// agent-lib-MK3 — GenericOpenAI Client Implementation
// =============================================================================

#include "generic_openai.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "../core/agent.hpp"  // g_running

namespace cortex::mk3::providers {

static bool sleepInterruptible(std::chrono::seconds total) {
    auto deadline = std::chrono::steady_clock::now() + total;
    while (g_running && std::chrono::steady_clock::now() < deadline) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        auto step = std::min(std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                             std::chrono::milliseconds(100));
        if (step.count() > 0)
            std::this_thread::sleep_for(step);
    }
    return g_running;
}

static std::string base64UrlDecode(std::string input) {
    for (char& c : input) {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }
    while (input.size() % 4)
        input.push_back('=');

    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (c == '=')
            break;
        int idx = chars.find(c);
        if (idx == (int)std::string::npos)
            continue;
        val = (val << 6) + idx;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string GenericOpenAIClient::resolveCodexAccountId(const std::string& token) {
    const char* home = std::getenv("HOME");
    if (home && home[0]) {
        std::ifstream f(std::string(home) + "/.codex/auth.json");
        if (f.good()) {
            Json::Value root;
            Json::CharReaderBuilder reader;
            std::string errs;
            if (Json::parseFromStream(reader, f, &root, &errs) && root.isMember("tokens") &&
                root["tokens"].isObject() && root["tokens"].isMember("account_id") &&
                root["tokens"]["account_id"].isString()) {
                return root["tokens"]["account_id"].asString();
            }
        }
    }

    size_t first = token.find('.');
    size_t second = first == std::string::npos ? std::string::npos : token.find('.', first + 1);
    if (first == std::string::npos || second == std::string::npos || second <= first + 1) {
        throw std::runtime_error(
            "openai-codex auth token is not a JWT and no account_id was found in "
            "~/.codex/auth.json");
    }

    std::string payload = base64UrlDecode(token.substr(first + 1, second - first - 1));
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(payload);
    if (!Json::parseFromStream(reader, ss, &root, &errs)) {
        throw std::runtime_error("failed to parse openai-codex JWT payload: " + errs);
    }
    const std::string claim = "https://api.openai.com/auth";
    if (root.isMember(claim) && root[claim].isObject() &&
        root[claim].isMember("chatgpt_account_id") &&
        root[claim]["chatgpt_account_id"].isString()) {
        return root[claim]["chatgpt_account_id"].asString();
    }
    throw std::runtime_error("failed to extract chatgpt_account_id from openai-codex token");
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
GenericOpenAIClient::GenericOpenAIClient(const OpenAIProviderConfig& cfg)
    : config_(cfg),
      apiKey_(cfg.resolveApiKey()),
      model_(cfg.defaultModel),
      maxTokens_(cfg.defaultMaxTokens) {
}

// ---------------------------------------------------------------------------
// Build OpenAI-compatible JSON request body
// ---------------------------------------------------------------------------
Json::Value GenericOpenAIClient::buildRequestBody(const ChatMessages& msgs, bool stream) const {
    if (config_.apiMode == "openai-codex-responses") {
        Json::Value body;
        body["model"] = model_;
        body["store"] = false;
        body["stream"] = true;
        body["text"]["verbosity"] = "low";
        body["include"].append("reasoning.encrypted_content");
        body["tool_choice"] = "auto";
        body["parallel_tool_calls"] = true;
        if (!config_.reasoningEffort.empty()) {
            body["reasoning"]["effort"] = config_.reasoningEffort;
            body["reasoning"]["summary"] = "auto";
        }

        std::string instructions;
        Json::Value input(Json::arrayValue);
        for (const auto& m : msgs) {
            if (m.role == ChatRole::SYSTEM) {
                if (!instructions.empty())
                    instructions += "\n\n";
                instructions += m.content;
                continue;
            }

            if (m.role == ChatRole::ASSISTANT) {
                Json::Value item;
                item["type"] = "message";
                item["role"] = "assistant";
                item["status"] = "completed";
                Json::Value part;
                part["type"] = "output_text";
                part["text"] = m.content;
                part["annotations"] = Json::arrayValue;
                item["content"].append(part);
                input.append(item);
            } else if (m.role == ChatRole::TOOL) {
                Json::Value item;
                item["type"] = "function_call_output";
                item["call_id"] = m.toolCallId;
                item["output"] = m.content;
                input.append(item);
            } else {
                Json::Value item;
                item["role"] = "user";
                Json::Value part;
                part["type"] = "input_text";
                part["text"] = m.content;
                item["content"].append(part);
                input.append(item);
            }
        }
        body["instructions"] = instructions.empty() ? "You are a helpful assistant." : instructions;
        if (input.empty()) {
            Json::Value fallback;
            fallback["role"] = "user";
            fallback["content"][0]["type"] = "input_text";
            fallback["content"][0]["text"] = "Continue from the embedded system/history prompt.";
            input.append(fallback);
        }
        body["input"] = input;
        return body;
    }

    Json::Value body;
    body["model"] = model_;
    body["temperature"] = temperature_;
    body["top_p"] = topP_;
    if (topK_ > 0 && config_.supportsTopK)
        body["top_k"] = topK_;
    if (!config_.reasoningEffort.empty())
        body["reasoning_effort"] = config_.reasoningEffort;
    if (presencePenalty_ != 0.0)
        body["presence_penalty"] = presencePenalty_;
    if (frequencyPenalty_ != 0.0)
        body["frequency_penalty"] = frequencyPenalty_;
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
    if (config_.apiMode == "openai-codex-responses") {
        std::string out;
        generateStream(msgs, [&](const std::string& token, bool) { out += token; });
        return out;
    }

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
        if (err.isObject() && err.isMember("message"))
            errMsg = err["message"].asString();
        else if (err.isString())
            errMsg = err.asString();
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
std::string GenericOpenAIClient::httpPost(const std::string& url, const Json::Value& body,
                                          StreamCallback cb, bool stream) {
    // Serialize body once (doesn't change across retries)
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string bodyStr = Json::writeString(writer, body);

    for (int retry = 0; retry <= maxRetries_; retry++) {
        CURL* curl = curl_easy_init();
        if (!curl)
            throw std::runtime_error("Failed to initialize CURL");

        std::string responseBuffer;
        StreamCtx ctx{cb, {}, {}, config_.apiMode == "openai-codex-responses", false};

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
        if (config_.apiMode == "openai-codex-responses") {
            std::string accountHeader = "chatgpt-account-id: " + resolveCodexAccountId(apiKey_);
            headers = curl_slist_append(headers, "Accept: text/event-stream");
            headers = curl_slist_append(headers, "OpenAI-Beta: responses=experimental");
            headers = curl_slist_append(headers, accountHeader.c_str());
            headers = curl_slist_append(headers, "originator: pi");
            headers = curl_slist_append(headers, "User-Agent: pi (linux; cortex-mk3)");
        }
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
            bool isRetryable =
                (res == CURLE_RECV_ERROR || res == CURLE_SEND_ERROR || res == CURLE_PARTIAL_FILE ||
                 res == CURLE_GOT_NOTHING || res == CURLE_OPERATION_TIMEDOUT);
            if (isRetryable && retry < maxRetries_) {
                int waitSec = std::min((1 << (retry + 1)) * 5, 120);
                if (!quietLogs_) {
                    std::cerr << "[MK3:RETRY] CURL error " << res << " — retrying in " << waitSec
                              << "s (attempt " << (retry + 1) << "/" << maxRetries_ << ")"
                              << std::endl;
                }
                if (!sleepInterruptible(std::chrono::seconds(waitSec)))
                    throw std::runtime_error("cancelled during retry backoff");
                continue;
            }
            throw std::runtime_error(std::string("CURL error: ") + curl_easy_strerror(res));
        }

        if (httpCode >= 400) {
            std::string errorBody =
                stream ? (ctx.lastErrorBody.empty() ? ctx.buffer : ctx.lastErrorBody)
                       : responseBuffer;
            bool isRetryable =
                (httpCode == 429) ||
                (httpCode == 413 && errorBody.find("rate_limit_exceeded") != std::string::npos);

            if (isRetryable && retry < maxRetries_) {
                int waitSec = std::min((1 << (retry + 1)) * 5, 120);
                if (!quietLogs_) {
                    std::cerr << "[MK3:RETRY] HTTP " << httpCode << " — retrying in " << waitSec
                              << "s (attempt " << (retry + 1) << "/" << maxRetries_ << ")"
                              << std::endl;
                }
                if (!sleepInterruptible(std::chrono::seconds(waitSec)))
                    throw std::runtime_error("cancelled during retry backoff");
                continue;
            }
            throw std::runtime_error("HTTP " + std::to_string(httpCode) +
                                     " — response: " + errorBody.substr(0, 500));
        }
        return responseBuffer;
    }  // retry loop
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

    // Process complete SSE events. Responses API/Codex can emit multi-line
    // `data:` JSON blocks, so join them before parsing.
    auto findEventEnd = [](const std::string& b, size_t& pos, size_t& len) -> bool {
        size_t lf = b.find("\n\n");
        size_t crlf = b.find("\r\n\r\n");
        if (lf == std::string::npos && crlf == std::string::npos)
            return false;
        if (crlf == std::string::npos || (lf != std::string::npos && lf < crlf)) {
            pos = lf;
            len = 2;
            return true;
        }
        pos = crlf;
        len = 4;
        return true;
    };
    while (true) {
        size_t pos = 0, eventLen = 0;
        if (!findEventEnd(ctx->buffer, pos, eventLen))
            break;
        std::string event = ctx->buffer.substr(0, pos);
        ctx->buffer.erase(0, pos + eventLen);

        std::vector<std::string> dataLines;
        std::istringstream es(event);
        std::string line;
        while (std::getline(es, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.rfind("data:", 0) == 0) {
                std::string data = line.substr(5);
                if (!data.empty() && data[0] == ' ')
                    data.erase(0, 1);
                dataLines.push_back(data);
            }
        }
        if (dataLines.empty())
            continue;

        std::string data;
        for (size_t i = 0; i < dataLines.size(); ++i) {
            if (i)
                data += "\n";
            data += dataLines[i];
        }
        auto trimCopy = [](std::string v) {
            while (!v.empty() &&
                   (v.back() == '\n' || v.back() == '\r' || v.back() == ' ' || v.back() == '\t'))
                v.pop_back();
            size_t start = 0;
            while (start < v.size() &&
                   (v[start] == '\n' || v[start] == '\r' || v[start] == ' ' || v[start] == '\t'))
                ++start;
            return v.substr(start);
        };
        data = trimCopy(data);
        if (data.empty())
            continue;

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
        if (!Json::parseFromStream(reader, ss, &root, &errs))
            continue;

        if (ctx->codexResponses) {
            std::string type =
                root.isMember("type") && root["type"].isString() ? root["type"].asString() : "";
            if (type == "response.created" && root.isMember("response") &&
                root["response"].isObject() && root["response"].isMember("id") &&
                root["response"]["id"].isString()) {
                continue;
            }

            if ((type == "response.output_text.delta" || type == "response.refusal.delta") &&
                root.isMember("delta") && root["delta"].isString()) {
                std::string codexDelta = root["delta"].asString();
                ctx->codexSawTextDelta = true;
                ctx->cb(codexDelta, false);
            } else if (type == "response.content_part.added" && root.isMember("part") &&
                       root["part"].isObject()) {
                const Json::Value& part = root["part"];
                std::string text;
                if (part.isMember("type") && part["type"].asString() == "output_text" &&
                    part.isMember("text") && part["text"].isString()) {
                    text = part["text"].asString();
                } else if (part.isMember("type") && part["type"].asString() == "refusal" &&
                           part.isMember("refusal") && part["refusal"].isString()) {
                    text = part["refusal"].asString();
                }
                if (!text.empty()) {
                    ctx->codexSawTextDelta = true;
                    ctx->cb(text, false);
                }
            } else if (!ctx->codexSawTextDelta && type == "response.output_text.done" &&
                       root.isMember("text") && root["text"].isString() &&
                       !root["text"].asString().empty()) {
                ctx->cb(root["text"].asString(), false);
            } else if (!ctx->codexSawTextDelta && type == "response.content_part.done" &&
                       root.isMember("part") && root["part"].isObject()) {
                const Json::Value& part = root["part"];
                std::string text;
                if (part.isMember("type") && part["type"].asString() == "output_text" &&
                    part.isMember("text") && part["text"].isString()) {
                    text = part["text"].asString();
                } else if (part.isMember("type") && part["type"].asString() == "refusal" &&
                           part.isMember("refusal") && part["refusal"].isString()) {
                    text = part["refusal"].asString();
                }
                if (!text.empty())
                    ctx->cb(text, false);
            } else if (!ctx->codexSawTextDelta && type == "response.output_item.done" &&
                       root.isMember("item") && root["item"].isObject()) {
                const Json::Value& item = root["item"];
                if (item.isMember("type") && item["type"].asString() == "message" &&
                    item.isMember("content") && item["content"].isArray()) {
                    std::string text;
                    for (const auto& p : item["content"]) {
                        if (!p.isObject() || !p.isMember("type"))
                            continue;
                        std::string partType = p["type"].asString();
                        if (partType == "output_text" && p.isMember("text") && p["text"].isString())
                            text += p["text"].asString();
                        else if (partType == "refusal" && p.isMember("refusal") &&
                                 p["refusal"].isString())
                            text += p["refusal"].asString();
                    }
                    if (!text.empty())
                        ctx->cb(text, false);
                }
            } else if (type == "response.completed" || type == "response.done" ||
                       type == "response.failed" || type == "response.cancelled" ||
                       type == "response.incomplete") {
                ctx->cb("", true);
            } else if (type == "error" || root.isMember("error")) {
                ctx->lastErrorBody += data;
            }
            continue;
        }

        auto& choices = root["choices"];
        if (choices.size() == 0)
            continue;

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
static int knownContextWindow(const std::string& provider, const std::string& modelId,
                              int fallback) {
    static const std::unordered_map<std::string, int> opencodeGo = {
        {"glm-5.2", 1000000},
        {"glm-5.1", 200000},
        {"glm-5", 204800},
        {"kimi-k2.7-code", 262144},
        {"kimi-k2.6", 262144},
        {"kimi-k2.5", 262144},
        {"deepseek-v4-pro", 1000000},
        {"deepseek-v4-flash", 1000000},
        {"qwen3.7-max", 1000000},
        {"qwen3.7-plus", 1000000},
        {"qwen3.6-plus", 1000000},
        {"qwen3.5-plus", 1000000},
        {"mimo-v2-pro", 1048576},
        {"mimo-v2-omni", 262144},
        {"mimo-v2.5-pro", 1048576},
        {"mimo-v2.5", 1048576},
        {"minimax-m3", 512000},
        {"minimax-m2.7", 204800},
        {"minimax-m2.5", 204800},
        {"hy3-preview", 256000},
    };

    std::string key = modelId;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (provider == "opencode-go" || provider == "opencode") {
        auto it = opencodeGo.find(key);
        if (it != opencodeGo.end())
            return it->second;
    }
    return fallback;
}

std::vector<ILlmProvider::ModelInfo> GenericOpenAIClient::listModels() {
    if (modelsFetched_)
        return cachedModels_;
    if (config_.apiMode == "openai-codex-responses") {
        cachedModels_.push_back({"gpt-5.5", "GPT-5.5", 272000, false});
        modelsFetched_ = true;
        return cachedModels_;
    }

    CURL* curl = curl_easy_init();
    if (!curl)
        return {};

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
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return {};

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream ss(response);
    if (!Json::parseFromStream(reader, ss, &root, &errs))
        return {};

    if (root.isMember("error"))
        return {};
    Json::Value& data = (root.isObject() && root.isMember("data")) ? root["data"] : root;
    if (!data.isArray())
        return {};

    for (auto& m : data) {
        if (!m.isObject() || !m.isMember("id") || !m["id"].isString())
            continue;
        ModelInfo info;
        info.id = m["id"].asString();
        info.name = (m.isMember("name") && m["name"].isString()) ? m["name"].asString() : info.id;
        int fallbackContext = config_.name == "openai-codex" ? 272000 : 65536;
        info.contextWindow = knownContextWindow(config_.name, info.id, fallbackContext);
        info.isFree = (info.id.find(":free") != std::string::npos ||
                       info.name.find(":free") != std::string::npos);
        if (!info.isFree && m.isMember("pricing") && m["pricing"].isObject()) {
            auto zeroish = [](const Json::Value& v) {
                if (v.isString())
                    return v.asString() == "0" || v.asString() == "0.0";
                if (v.isNumeric())
                    return v.asDouble() == 0.0;
                return false;
            };
            bool promptFree = m["pricing"].isMember("prompt") && zeroish(m["pricing"]["prompt"]);
            bool completionFree =
                m["pricing"].isMember("completion") && zeroish(m["pricing"]["completion"]);
            info.isFree = promptFree && completionFree;
        }
        cachedModels_.push_back(info);
    }

    modelsFetched_ = true;
    return cachedModels_;
}

}  // namespace cortex::mk3::providers
