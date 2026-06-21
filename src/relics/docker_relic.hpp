#pragma once
// =============================================================================
// agent-lib-MK3 — DockerManagedRelic: a Relic backed by a Docker-managed
// HTTP-fronted service.
//
// A DockerManagedRelic wraps a DockerRelicDef (loaded from relic.yml) and
// implements the Relic interface. handle() routes requests to the backing
// service via the shared HTTP helper, and ensureContainerUp() lazily
// starts the container if it isn't already running.
//
// This is the bridge that lets the unified Reliquary dispatch to
// Docker-managed services without any per-relic coupling in the
// dispatch path.
// =============================================================================

#include <json/json.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../utils/process.hpp"
#include "docker_dispatcher.hpp"
#include "relic.hpp"

namespace cortex::mk3::relics {

class DockerManagedRelic : public Relic {
   public:
    // Build a DockerManagedRelic from an already-parsed DockerRelicDef.
    // The def is copied so the dispatcher can keep its own copy too.
    explicit DockerManagedRelic(DockerRelicDef def) : def_(std::move(def)) {}

    const std::string& name() const override {
        return def_.name;
    }

    std::string description() const override {
        return def_.summary;
    }

    std::vector<std::string> endpoints() const override {
        return def_.endpoints;
    }

    bool isHealthy() const override {
        std::string healthUrl =
            "http://localhost:" + std::to_string(def_.port) + def_.healthPath;
        auto r = httpCall(healthUrl, Json::Value());
        return r.success;
    }

    // Dispatch an endpoint. If the relic is managed and the container is
    // not up, ensureContainerUp() runs first. Remote relics skip the
    // container check and treat the endpoint as a full URL.
    RelicResult handle(const std::string& endpoint, const Json::Value& params) override {
        if (def_.mode == "remote") {
            auto r = httpCall(endpoint, params);
            return toRelicResult(r);
        }
        if (def_.mode == "managed") {
            if (!ensureContainerUp()) {
                return RelicResult::fail(
                    "Failed to start container for " + def_.name);
            }
            std::string url =
                "http://localhost:" + std::to_string(def_.port) + "/" + endpoint;
            auto r = httpCall(url, params);
            return toRelicResult(r);
        }
        return RelicResult::fail("Unknown relic mode: " + def_.mode);
    }

   private:
    DockerRelicDef def_;

    // Lazily start the Docker container backing this relic. Returns true
    // when the health endpoint responds OK within the timeout window.
    bool ensureContainerUp() {
        std::string healthUrl =
            "http://localhost:" + std::to_string(def_.port) + def_.healthPath;
        if (httpCall(healthUrl, Json::Value()).success)
            return true;

        std::string composeCmd = "docker compose";
        std::string projectArg =
            def_.projectName.empty() ? "" : " --project-name " + shellQuote(def_.projectName);
        std::string fileArg =
            def_.composeFile.empty() ? "" : " -f " + shellQuote(def_.composeFile);
        std::string envArg =
            def_.envFile.empty() ? "" : " --env-file " + shellQuote(def_.envFile);
        std::string cmd = "cd " + shellQuote(def_.composeDir) + " && " + composeCmd +
                          projectArg + envArg + fileArg + " up -d 2>&1";

        process::Spec spec;
        spec.shell = true;
        spec.command = cmd;
        spec.timeoutMs = 120000;  // container start may be slow
        spec.maxStdout = 256 * 1024;
        spec.maxStderr = 64 * 1024;
        process::Result pr = process::run(spec);
        if (!pr.success())
            return false;

        // Wait for health check (retry up to 10 times, 500ms apart)
        for (int i = 0; i < 10; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (httpCall(healthUrl, Json::Value()).success)
                return true;
        }
        return false;
    }

    static std::string shellQuote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'')
                out += "'\"'\"'";
            else
                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    // HTTP call to a fully-qualified URL. Used both for the health check
    // and the user request.
    struct HttpResult {
        bool success = false;
        int httpStatus = 0;
        std::string data;
        std::string error;
    };

    static HttpResult httpCall(const std::string& url, const Json::Value& body) {
        HttpResult result;
        CURL* curl = curl_easy_init();
        if (!curl) {
            result.error = "curl_init failed";
            return result;
        }

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

        if (!body.isNull() && !body.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            std::string bodyStr = Json::writeString(w, body);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            result.httpStatus = (int)httpCode;
            result.success = (httpCode >= 200 && httpCode < 300);
            result.data = response;
        } else {
            result.error = std::string("HTTP error: ") + curl_easy_strerror(res);
        }
        curl_easy_cleanup(curl);
        return result;
    }

    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        ((std::string*)userp)->append((char*)contents, total);
        return total;
    }

    // Convert the raw HttpResult to a structured RelicResult. Parses the
    // response body as JSON when possible; falls back to a string field.
    static RelicResult toRelicResult(const HttpResult& r) {
        if (r.success) {
            Json::Value parsed;
            Json::CharReaderBuilder rb;
            std::string errs;
            std::istringstream ss(r.data);
            if (Json::parseFromStream(rb, ss, &parsed, &errs))
                return RelicResult::ok(parsed);
            return RelicResult::ok(Json::Value(r.data));
        }
        return RelicResult::fail(r.error);
    }
};

// Build a DockerManagedRelic from a directory containing relic.yml. Returns
// nullptr if the manifest is missing or invalid.
inline std::shared_ptr<Relic> loadDockerRelicFromDir(const std::string& relicDir) {
    DockerRelicDef def;
    if (!DockerRelicDispatcher::loadDefFromDir(relicDir, def))
        return nullptr;
    return std::make_shared<DockerManagedRelic>(std::move(def));
}

}  // namespace cortex::mk3::relics