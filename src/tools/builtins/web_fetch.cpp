// src/tools/builtins/web_fetch.cpp — web_fetch native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <curl/curl.h>

namespace cortex::mk3::tools::builtins {

static size_t curlWrite(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

std::string web_fetch(const Json::Value& p) {
    std::string url = p.get("url", "").asString();
    if (url.empty())
        return jsonErr("url is required");
    std::string method = p.get("method", "GET").asString();
    std::string data = p.get("data", "").asString();
    int timeout = p.get("timeout", 30).asInt();

    CURL* curl = curl_easy_init();
    if (!curl)
        return jsonErr("curl init failed");
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cortex-mk3/1.0");
    if (method == "POST" || method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        if (method == "PUT")
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    struct curl_slist* hdrs = nullptr;
    if (!data.empty())
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (hdrs)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    Json::Value r;
    if (res == CURLE_OK && httpCode >= 200 && httpCode < 400) {
        r["success"] = true;
        r["content"] = response;
        r["status"] = (int)httpCode;
    } else {
        r["success"] = false;
        r["error"] = res != CURLE_OK ? curl_easy_strerror(res) : "HTTP " + std::to_string(httpCode);
        r["status"] = (int)httpCode;
    }
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
