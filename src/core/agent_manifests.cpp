// src/core/agent_manifests.cpp — manifest reload + session tool persistence
#include <chrono>
#include <filesystem>
#include <fstream>

#include "agent.hpp"
#include "manifest_loader.hpp"

namespace cortex::mk3 {

namespace {

bool isConfigStagingDir(const std::string& dir) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(dir, ec);
    if (ec)
        p = std::filesystem::path(dir);
    return p.filename() == "staging" && !p.parent_path().empty() &&
           p.parent_path().filename() == "config";
}

}  // namespace

int Agent::reloadManifests(bool backup) {
    std::string dir = config_.manifestDir.empty() ? "./manifests" : config_.manifestDir;
    if (isConfigStagingDir(dir)) {
        std::cerr << "[manifest] skipping recursive reload for config/staging; use explicit "
                     "manifest imports\n";
        return 0;
    }
    if (!std::filesystem::exists(dir))
        return 0;
    if (backup) {
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        std::string backupDir = dir + "/_backups/" + std::to_string(ts);
        std::filesystem::create_directories(backupDir);
    }
    int count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file() || it->path().extension() != ".yml")
            continue;
        auto schema = ManifestLoader::loadToolManifest(it->path().string());
        if (schema.name.empty() || disabledBuiltins_.count(schema.name))
            continue;
        // Skip non-tool manifests by checking kind field in YAML
        auto readYaml = [](const std::string& p) {
            std::ifstream f(p);
            if (!f)
                return std::string();
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        };
        auto yaml = readYaml(it->path().string());
        if (!yaml.empty()) {
            auto root = ManifestYaml::parse(yaml);
            std::string kind = ManifestYaml::get(root, "kind");
            if (kind == "Agent" || kind == "Workflow" || kind == "Feed" || kind == "Relic")
                continue;
        }
        ToolDef td;
        td.name = schema.name;
        td.description = schema.description;
        td.isNative = false;
        if (!schema.runtime.empty() && !schema.entrypoint.empty()) {
            td.scriptRuntime = schema.runtime;
            td.scriptPath = (it->path().parent_path() / schema.entrypoint).lexically_normal().string();
            td.buildCommand = schema.buildCommand;
            td.buildCwd = schema.buildCwd.empty()
                              ? it->path().parent_path().string()
                              : (it->path().parent_path() / schema.buildCwd).lexically_normal().string();
            td.buildOutput = schema.buildOutput.empty()
                                 ? ""
                                 : (it->path().parent_path() / schema.buildOutput).lexically_normal().string();
        } else {
            // Not a script tool — skip
            continue;
        }
        tools_[td.name] = tools::Tool(td, td.scriptPath, td.scriptRuntime);
        count++;
    }
    // Persist loaded tools to session manifest (survives restarts)
    if (count > 0)
        saveSessionTools();
    return count;
}

void Agent::saveSessionTools() {
    std::string dir = config_.manifestDir.empty() ? "./manifests" : config_.manifestDir;
    if (isConfigStagingDir(dir))
        return;
    std::string sessionDir = dir + "/_session";
    std::filesystem::create_directories(sessionDir);
    Json::Value arr(Json::arrayValue);
    for (auto& [name, tool] : tools_) {
        if (tool.isNative() || tool.scriptPath().empty())
            continue;
        Json::Value t;
        t["name"] = name;
        t["description"] = tool.description();
        t["scriptRuntime"] = tool.scriptRuntime();
        t["scriptPath"] = tool.scriptPath();
        t["buildCommand"] = tool.buildCommand();
        t["buildCwd"] = tool.buildCwd();
        t["buildOutput"] = tool.buildOutput();
        arr.append(t);
    }
    std::ofstream f(sessionDir + "/tools.json");
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    f << Json::writeString(w, arr);
}

void Agent::loadSessionTools() {
    std::string dir = config_.manifestDir.empty() ? "./manifests" : config_.manifestDir;
    if (isConfigStagingDir(dir))
        return;
    std::string sessionFile = dir + "/_session/tools.json";
    if (!std::filesystem::exists(sessionFile))
        return;
    std::ifstream f(sessionFile);
    if (!f)
        return;
    Json::Value arr;
    Json::CharReaderBuilder r;
    std::string errs;
    if (!Json::parseFromStream(r, f, &arr, &errs))
        return;
    for (auto& t : arr) {
        if (!t.isMember("name"))
            continue;
        ToolDef td;
        td.name = t["name"].asString();
        td.description = t.get("description", "").asString();
        td.isNative = false;
        td.scriptRuntime = t.get("scriptRuntime", "").asString();
        td.scriptPath = t.get("scriptPath", "").asString();
        td.buildCommand = t.get("buildCommand", "").asString();
        td.buildCwd = t.get("buildCwd", "").asString();
        td.buildOutput = t.get("buildOutput", "").asString();
        if (!disabledBuiltins_.count(td.name))
            tools_[td.name] = tools::Tool(td, td.scriptPath, td.scriptRuntime);
    }
}

}  // namespace cortex::mk3
