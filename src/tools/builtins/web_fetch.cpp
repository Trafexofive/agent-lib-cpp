// src/tools/builtins/web_fetch.cpp — web_fetch native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>

namespace cortex::mk3::tools::builtins {

struct FetchBuffer {
    std::string data;
    size_t maxBytes = 1024 * 1024;
    bool truncated = false;
};

static size_t curlWrite(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* b = static_cast<FetchBuffer*>(userdata);
    size_t bytes = sz * nmemb;
    size_t remaining = b->maxBytes > b->data.size() ? b->maxBytes - b->data.size() : 0;
    size_t take = std::min(bytes, remaining);
    b->data.append(static_cast<char*>(ptr), take);
    if (take < bytes)
        b->truncated = true;
    return bytes;
}

static std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

static bool validUrl(const std::string& url) {
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

static bool allowedMethod(const std::string& method) {
    return method == "GET" || method == "POST" || method == "PUT" || method == "PATCH" ||
           method == "DELETE" || method == "HEAD";
}

std::string web_fetch(const Json::Value& p) {
    if (!p.isMember("url") || !p["url"].isString())
        return jsonErr("url is required");
    std::string url = p["url"].asString();
    if (!validUrl(url))
        return jsonErr("url must start with http:// or https://");

    std::string method = upper(p.get("method", "GET").asString());
    if (!allowedMethod(method))
        return jsonErr("unsupported method: " + method);
    std::string data = p.get("data", "").asString();
    int timeout = std::clamp(p.get("timeout", 30).asInt(), 1, 120);
    int connectTimeout = std::clamp(p.get("connect_timeout", 10).asInt(), 1, timeout);
    size_t maxBytes = static_cast<size_t>(std::clamp(p.get("max_bytes", 1024 * 1024).asInt(), 1, 10 * 1024 * 1024));
    bool follow = p.get("follow_redirects", true).asBool();
    std::string userAgent = p.get("user_agent", "cortex-mk3/1.0").asString();

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        return jsonErr("curl init failed");

    FetchBuffer buffer;
    buffer.maxBytes = maxBytes;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, (long)connectTimeout);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 5L);

    if (method == "HEAD") {
        curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!data.empty())
            curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, data.c_str());
    }

    struct curl_slist* hdrs = nullptr;
    if (!data.empty())
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (p.isMember("headers") && p["headers"].isObject()) {
        for (const auto& key : p["headers"].getMemberNames()) {
            if (!p["headers"][key].isString())
                continue;
            std::string h = key + ": " + p["headers"][key].asString();
            hdrs = curl_slist_append(hdrs, h.c_str());
        }
    }
    if (hdrs)
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl.get());
    long httpCode = 0;
    char* contentType = nullptr;
    double totalTime = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &contentType);
    curl_easy_getinfo(curl.get(), CURLINFO_TOTAL_TIME, &totalTime);
    if (hdrs)
        curl_slist_free_all(hdrs);

    Json::Value r;
    r["success"] = (res == CURLE_OK && httpCode >= 200 && httpCode < 400);
    r["status"] = (int)httpCode;
    r["method"] = method;
    r["url"] = url;
    r["content"] = buffer.data;
    r["bytes"] = static_cast<Json::UInt64>(buffer.data.size());
    r["truncated"] = buffer.truncated;
    r["elapsed_ms"] = static_cast<Json::Int64>(totalTime * 1000.0);
    if (contentType)
        r["content_type"] = contentType;
    if (res != CURLE_OK)
        r["error"] = curl_easy_strerror(res);
    else if (httpCode < 200 || httpCode >= 400)
        r["error"] = "HTTP " + std::to_string(httpCode);
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
