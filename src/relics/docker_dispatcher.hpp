// ─────────────────────────────────────────────────────────────────────────────
// Docker Relic Dispatcher — manages Docker relic lifecycle and HTTP routing
// Managed relics: auto-start via docker-compose, health-check, keep running
// Remote relics: direct HTTP call, no lifecycle management
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <curl/curl.h>
#include <json/json.h>

#include "../core/mini_yaml.hpp"
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace cortex {
namespace mk3 {
namespace relics {

namespace fs = std::filesystem;

// ── Relic definition (loaded from relic.yml) ──
struct DockerRelicDef {
    std::string name;
    std::string mode;  // managed | remote
    std::string summary;
    int port = 0;
    std::string composeDir;   // path to directory with docker-compose.yml
    std::string composeFile;  // optional explicit compose file
    std::string envFile;      // optional --env-file for compose
    std::string projectName;  // optional docker compose project name
    std::string healthPath = "/health";
    std::vector<std::string> endpoints;
};

// ── Dispatch result ──
struct DockerRelicResult {
    bool success = false;
    std::string data;  // JSON response body
    std::string error;
    int httpStatus = 0;
};

// ── Docker Relic Dispatcher ──
class DockerRelicDispatcher {
   public:
    static DockerRelicDispatcher& instance() {
        static DockerRelicDispatcher d;
        return d;
    }

    // Static: parse a relic directory and produce a DockerRelicDef. Used
    // by both the legacy loadRelic() path and the unified
    // DockerManagedRelic (Reliquary) path. The def is fully self-contained
    // — callers can copy or move it freely.
    static bool loadDefFromDir(const std::string& relicDir, DockerRelicDef& def) {
        fs::path manifestPath = fs::path(relicDir) / "relic.yml";
        std::ifstream f(manifestPath);
        if (!f)
            return false;

        std::string yaml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto root = ManifestYaml::parse(yaml);

        def = DockerRelicDef{};
        def.composeDir = relicDir;
        def.name = ManifestYaml::get(root, "name");
        def.mode = ManifestYaml::get(root, "mode");
        def.summary = ManifestYaml::get(root, "summary");
        def.composeFile = ManifestYaml::get(root, "compose_file");
        def.envFile = ManifestYaml::get(root, "env_file");
        def.projectName = ManifestYaml::get(root, "project_name");
        def.healthPath = ManifestYaml::get(root, "health_path", "/health");
        std::string portStr = ManifestYaml::get(root, "port", "0");
        if (!portStr.empty()) {
            try {
                def.port = std::stoi(portStr);
            } catch (...) {
                def.port = 0;
            }
        }

        if (def.name.empty() || def.mode.empty())
            return false;

        // Auto-detect endpoints from comments or app directory
        if (def.port == 0) {
            // Try to read port from docker-compose.yml
            fs::path composePath = fs::path(relicDir) / "docker-compose.yml";
            std::ifstream cf(composePath);
            if (cf) {
                std::string cline;
                while (std::getline(cf, cline)) {
                    size_t p = cline.find("\"");
                    size_t pp = cline.rfind(":");
                    if (pp != std::string::npos && p != std::string::npos) {
                        std::string portStr = cline.substr(pp + 1, p - pp - 1);
                        try {
                            def.port = std::stoi(portStr);
                            break;
                        } catch (...) {
                        }
                    }
                }
            }
        }
        return true;
    }

    // Load relic definition from manifest directory
    bool loadRelic(const std::string& relicDir) {
        DockerRelicDef def;
        if (!loadDefFromDir(relicDir, def))
            return false;
        relics_[def.name] = std::move(def);
        return true;
    }

    // Load all relics from a manifests/relics/ directory
    void loadAllFrom(const std::string& relicsDir) {
        if (!fs::exists(relicsDir))
            return;
        for (auto& entry : fs::directory_iterator(relicsDir)) {
            if (entry.is_directory()) {
                loadRelic(entry.path().string());
            }
        }
    }

    // Dispatch a relic action
    DockerRelicResult dispatch(const std::string& relicName, const std::string& endpoint,
                               const Json::Value& params) {
        DockerRelicResult result;
        auto it = relics_.find(relicName);
        if (it == relics_.end()) {
            result.error = "Unknown relic: " + relicName;
            return result;
        }

        auto& def = it->second;

        // Remote relics: direct HTTP call
        if (def.mode == "remote") {
            std::string url = endpoint;  // endpoint IS the URL for remote relics
            return httpCall(url, params);
        }

        // Managed (Docker) relics: ensure container up, then HTTP call
        if (def.mode == "managed") {
            if (!ensureContainerUp(def)) {
                result.error = "Failed to start container for " + relicName;
                return result;
            }

            std::string url = "http://localhost:" + std::to_string(def.port) + "/" + endpoint;
            return httpCall(url, params);
        }

        result.error = "Unknown relic mode: " + def.mode;
        return result;
    }

    // Check health for all managed relics
    std::map<std::string, bool> healthCheckAll() {
        std::map<std::string, bool> status;
        for (auto& [name, def] : relics_) {
            if (def.mode != "managed")
                continue;
            std::string url = "http://localhost:" + std::to_string(def.port) + def.healthPath;
            auto r = httpCall(url, Json::Value());
            status[name] = r.success;
        }
        return status;
    }

    // List loaded relics
    std::vector<std::string> listRelics() const {
        std::vector<std::string> names;
        for (auto& [name, _] : relics_)
            names.push_back(name);
        return names;
    }

    const DockerRelicDef* getRelic(const std::string& name) const {
        auto it = relics_.find(name);
        return (it != relics_.end()) ? &it->second : nullptr;
    }

   private:
    std::map<std::string, DockerRelicDef> relics_;

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

    // Ensure Docker container is running for a managed relic
    bool ensureContainerUp(const DockerRelicDef& def) {
        // Check if already running via health endpoint
        std::string healthUrl = "http://localhost:" + std::to_string(def.port) + def.healthPath;
        auto r = httpCall(healthUrl, Json::Value());
        if (r.success)
            return true;

        // Container not running — start it via docker compose/docker-compose
        std::string composeCmd = "docker compose";
        std::string projectArg =
            def.projectName.empty() ? "" : " --project-name " + shellQuote(def.projectName);
        std::string fileArg = def.composeFile.empty() ? "" : " -f " + shellQuote(def.composeFile);
        std::string envArg = def.envFile.empty() ? "" : " --env-file " + shellQuote(def.envFile);
        std::string cmd = "cd " + shellQuote(def.composeDir) + " && " + composeCmd + projectArg +
                          envArg + fileArg + " up -d 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        if (!p)
            return false;

        char buf[1024];
        std::string output;
        while (fgets(buf, sizeof(buf), p))
            output += buf;
        int rc = pclose(p);

        if (rc != 0)
            return false;

        // Wait for health check (retry up to 10 times, 500ms apart)
        for (int i = 0; i < 10; i++) {
            usleep(500000);
            auto r = httpCall(healthUrl, Json::Value());
            if (r.success)
                return true;
        }
        return false;
    }

    // HTTP call helper
    static DockerRelicResult httpCall(const std::string& url, const Json::Value& body) {
        DockerRelicResult result;

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

        // If body is non-empty, POST it as JSON
        std::string bodyStr;
        if (!body.isNull() && !body.empty()) {
            Json::StreamWriterBuilder w;
            w["indentation"] = "";
            bodyStr = Json::writeString(w, body);
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
};

}  // namespace relics
}  // namespace mk3
}  // namespace cortex
