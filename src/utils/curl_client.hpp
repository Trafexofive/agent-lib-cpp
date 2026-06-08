#pragma once
// =============================================================================
// agent-lib-MK3 — CurlClient — RAII wrapper around libcurl HTTP requests
// Ported from MK2 (src/utils/CurlClient.hpp) — adapted for mk3 namespace.
// =============================================================================
#include <curl/curl.h>
#include <string>
#include <map>
#include <functional>
#include <memory>

namespace cortex::mk3::utils {

// ─────────────────────────────────────────────────────────────────────────
// CurlResponse — standardised result from any HTTP call
// ─────────────────────────────────────────────────────────────────────────
struct CurlResponse {
    long statusCode = 0;
    std::string body;
    std::string error;
    bool success() const { return statusCode >= 200 && statusCode < 300; }
    bool aborted() const { return statusCode == 499; }  // our sentinel for interrupt
};

// ─────────────────────────────────────────────────────────────────────────
// CurlClient — static methods, no instantiation needed
// ─────────────────────────────────────────────────────────────────────────
class CurlClient {
public:
    using StreamCallback = std::function<void(const std::string& chunk)>;

    struct Config {
        int timeoutSeconds;
        bool followRedirects;
        bool verifySsl;
        std::string userAgent;
        std::string acceptEncoding;

        Config()
            : timeoutSeconds(30)
            , followRedirects(true)
            , verifySsl(true)
            , userAgent("Cortex-MK3/0.1.0")
            , acceptEncoding() {}
    };

    // ── Simple HTTP GET ─────────────────────────────────────────────────
    static CurlResponse get(const std::string& url,
                            const std::map<std::string, std::string>& headers = {},
                            const Config& cfg = Config());

    // ── HTTP POST with string body ──────────────────────────────────────
    static CurlResponse post(const std::string& url,
                             const std::string& body,
                             const std::map<std::string, std::string>& headers = {},
                             const Config& cfg = Config());

    // ── HTTP POST with JSON body (auto-serializes + sets Content-Type) ──
    static CurlResponse postJson(const std::string& url,
                                 const std::string& jsonBody,
                                 const std::map<std::string, std::string>& extraHeaders = {},
                                 const Config& cfg = Config());

    // ── HTTP with arbitrary method ──────────────────────────────────────
    static CurlResponse request(const std::string& method,
                                const std::string& url,
                                const std::string& body,
                                const std::map<std::string, std::string>& headers = {},
                                const Config& cfg = Config());

    // ── Streaming HTTP POST (for SSE) ───────────────────────────────────
    static CurlResponse postStream(const std::string& url,
                                   const std::string& body,
                                   StreamCallback callback,
                                   const std::map<std::string, std::string>& headers = {},
                                   const Config& cfg = Config());

    // ── Interrupt checking ──────────────────────────────────────────────
    static void setInterruptChecker(std::function<bool()> checker);
    static void clearInterruptChecker();

private:
    static std::function<bool()> s_interruptChecker;
};

// =========================================================================
// Inline implementation
// =========================================================================

// ── Static state ─────────────────────────────────────────────────────────
inline std::function<bool()> CurlClient::s_interruptChecker;

inline void CurlClient::setInterruptChecker(std::function<bool()> checker) {
    s_interruptChecker = std::move(checker);
}
inline void CurlClient::clearInterruptChecker() {
    s_interruptChecker = nullptr;
}

// ── CURL write callback: append data to string ───────────────────────────
namespace {
inline size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    size_t total = size * nmemb;
    str->append(static_cast<char*>(contents), total);
    return total;
}

// ── Streaming write callback: forward chunks + accumulate body ───────────
struct StreamData {
    CurlClient::StreamCallback callback;
    std::string body;
};

inline size_t writeToStream(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* data = static_cast<StreamData*>(userp);
    size_t total = size * nmemb;
    std::string chunk(static_cast<char*>(contents), total);
    if (data->callback) {
        data->callback(chunk);
    }
    data->body.append(chunk);
    return total;
}

// ── Progress callback for interrupt checking ────────────────────────────
inline int progressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* checker = static_cast<std::function<bool()>*>(clientp);
    if (checker && *checker && (*checker)()) {
        return 1;  // abort transfer
    }
    return 0;  // continue
}

// ── Build curl header list from map ─────────────────────────────────────
inline struct curl_slist* buildHeaders(const std::map<std::string, std::string>& headers) {
    struct curl_slist* list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        list = curl_slist_append(list, header.c_str());
    }
    return list;
}
}  // anonymous namespace

// ── RAII guard: cleans up curl handle + header list on scope exit ───────
struct CurlGuard {
    CURL* handle = nullptr;
    struct curl_slist* headers = nullptr;
    ~CurlGuard() {
        if (headers) curl_slist_free_all(headers);
        if (handle) curl_easy_cleanup(handle);
    }
};

// ── Core request ─────────────────────────────────────────────────────────
inline CurlResponse CurlClient::request(const std::string& method,
                                        const std::string& url,
                                        const std::string& body,
                                        const std::map<std::string, std::string>& headers,
                                        const Config& cfg) {
    CurlResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "Failed to initialize curl handle";
        return resp;
    }
    CurlGuard guard{curl, nullptr};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // Method
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    // Body
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    // Headers
    auto* hdrList = buildHeaders(headers);
    guard.headers = hdrList;
    if (hdrList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrList);
    }

    // Response buffer
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

    // Timeouts
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)cfg.timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // Follow redirects
    if (cfg.followRedirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    }

    // SSL verification
    if (!cfg.verifySsl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    // Accept encoding
    if (!cfg.acceptEncoding.empty()) {
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, cfg.acceptEncoding.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    }

    // User agent
    if (!cfg.userAgent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, cfg.userAgent.c_str());
    }

    // Interrupt checking
    if (s_interruptChecker) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s_interruptChecker);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    // Execute
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        resp.error = curl_easy_strerror(res);
        resp.statusCode = (res == CURLE_ABORTED_BY_CALLBACK) ? 499 : 0;
        return resp;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    return resp;
}

// ── Streaming POST (for SSE) ─────────────────────────────────────────────
inline CurlResponse CurlClient::postStream(const std::string& url,
                                           const std::string& body,
                                           StreamCallback callback,
                                           const std::map<std::string, std::string>& headers,
                                           const Config& cfg) {
    CurlResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "Failed to initialize curl handle";
        return resp;
    }
    CurlGuard guard{curl, nullptr};

    StreamData streamData{std::move(callback), {}};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());

    auto* hdrList = buildHeaders(headers);
    guard.headers = hdrList;
    if (hdrList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrList);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToStream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &streamData);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)cfg.timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, cfg.followRedirects ? 1L : 0L);

    if (!cfg.verifySsl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (!cfg.acceptEncoding.empty()) {
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, cfg.acceptEncoding.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    }

    if (s_interruptChecker) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &s_interruptChecker);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }

    CURLcode res = curl_easy_perform(curl);
    resp.body = std::move(streamData.body);
    if (res != CURLE_OK) {
        resp.error = curl_easy_strerror(res);
        resp.statusCode = (res == CURLE_ABORTED_BY_CALLBACK) ? 499 : 0;
        return resp;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
    return resp;
}

// ── Convenience methods ──────────────────────────────────────────────────
inline CurlResponse CurlClient::get(const std::string& url,
                                    const std::map<std::string, std::string>& headers,
                                    const Config& cfg) {
    return request("GET", url, "", headers, cfg);
}

inline CurlResponse CurlClient::post(const std::string& url,
                                     const std::string& body,
                                     const std::map<std::string, std::string>& headers,
                                     const Config& cfg) {
    return request("POST", url, body, headers, cfg);
}

inline CurlResponse CurlClient::postJson(const std::string& url,
                                         const std::string& jsonBody,
                                         const std::map<std::string, std::string>& extraHeaders,
                                         const Config& cfg) {
    auto merged = extraHeaders;
    merged["Content-Type"] = "application/json";
    return request("POST", url, jsonBody, merged, cfg);
}

}  // namespace cortex::mk3::utils
