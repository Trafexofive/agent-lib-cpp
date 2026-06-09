// ─────────────────────────────────────────────────────────────────────────────
// agent-lib-MK3 — Tool dispatch, execution, management, reload, persistence
// ─────────────────────────────────────────────────────────────────────────────
#include "agent.hpp"
#include "../feeds/feed_engine.hpp"
#include "../relics/builtin_relics.hpp"
#include "../tools/dispatch.hpp"
#include "../utils/ansi.hpp"
#include "dispatch.hpp"
#include "manifest_loader.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>

namespace cortex::mk3 {
Json::Value Agent::dispatchTool(const protocol::ParsedAction &action) {
    // ── Meta-tools: reload manifests, toggle builtins ──
    if (action.name == "disable_builtin" || action.name == "enable_builtin") {
        return toggleBuiltin(action.params, action.name == "enable_builtin");
    }
    if (action.name == "reload_manifests") {
        Json::Value r;
        bool backup = action.params.get("backup", false).asBool();
        r["loaded"] = reloadManifests(backup);
        r["success"] = true;
        return r;
    }

    // ── Relic dispatch ──
    if (action.type == protocol::ActionType::RELIC) {
        auto result = relics::RelicDispatcher::instance().dispatch(
            action.name, action.params.get("endpoint", "").asString(), action.params);
        Json::Value r;
        r["success"] = result.success;
        r["output"] = result.success ? result.data : result.error;
        if (result.success && !result.data.empty()) r["data"] = result.data;
        return r;
    }

    // ── Sandbox validation ──
    if (sandboxPolicy_.enabled) {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        std::string paramsStr = Json::writeString(w, action.params);
        std::string blockReason =
            sandboxPolicy_.validate(action.name, paramsStr);
        if (!blockReason.empty()) {
            Json::Value err;
            err["success"] = false;
            err["error"] = blockReason;
            return err;
        }
    }

    // Script tools (path-imported, not native)
    auto it = tools_.find(action.name);
    if (it != tools_.end() && !it->second.isNative &&
        !it->second.scriptPath.empty()) {
        return executeScriptTool(it->second, action.params);
    }

    return dispatch::dispatchTool(action);
}

Json::Value Agent::executeScriptTool(const ToolDef &tool,
                                     const Json::Value &params) {
    // Synchronous execution — blocks but returns actual output
    std::string cmd = tool.scriptRuntime + " " + tool.scriptPath;

    // Write params to tmpfile for script to read
    std::string tmpFile = "/tmp/cortex-tool-" + tool.name + ".json";
    {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        std::ofstream tf(tmpFile);
        tf << Json::writeString(w, params);
    }
    std::string cmdWithArg = cmd + " " + tmpFile;

    auto start = std::chrono::steady_clock::now();
    FILE *p = popen((cmdWithArg + " 2>&1").c_str(), "r");
    if (!p) {
        unlink(tmpFile.c_str());
        Json::Value err;
        err["success"] = false;
        err["error"] = "failed to launch: " + cmdWithArg;
        return err;
    }
    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p)) output += buf;
    int exitCode = pclose(p);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    unlink(tmpFile.c_str());

    Json::Value r;
    r["success"] = (exitCode == 0);
    r["exit"] = exitCode;
    r["ms"] = (Json::Int64)elapsed;
    r["output"] = output;
    if (exitCode != 0) r["error"] = "exit code " + std::to_string(exitCode);
    return r;
}

void Agent::addTool(ToolDef tool) {
    tools_[tool.name] = std::move(tool);
}

void Agent::removeTool(const std::string &name) {
    tools_.erase(name);
    disabledBuiltins_.insert(name);
}

Json::Value Agent::toggleBuiltin(const Json::Value& params, bool enable) {
    Json::Value r;
    r["success"] = true;
    std::string name = params.get("name", "").asString();
    if (name.empty() && params.isMember("names") && params["names"].isArray() && params["names"].size() > 0)
        name = params["names"][0].asString();
    if (name.empty()) {
        r["success"] = false;
        r["error"] = "name or names required";
        return r;
    }
    if (enable) {
        disabledBuiltins_.erase(name);
        r["enabled"] = name;
    } else {
        tools_.erase(name);
        disabledBuiltins_.insert(name);
        r["disabled"] = name;
    }
    return r;
}

bool Agent::hasTool(const std::string &name) const {
    // Only consider tools explicitly added to this agent — not the global
    // registry
    return tools_.count(name);
}

std::vector<std::string> Agent::toolNames() const {
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    auto push = [&](const std::string &n) {
        if (seen.insert(n).second)
            names.push_back(n);
    };
    for (auto &name : tools::ToolRegistry::instance().list())
        push(name);
    for (auto &[name, _] : tools_)
        push(name);
    return names;
}

// ═══════════════════════════════════════════════════════════════════════
// Sub-Agent Management
// ═══════════════════════════════════════════════════════════════════════

void Agent::addSubAgent(std::shared_ptr<Agent> agent) {
    subAgents_[agent->name()] = std::move(agent);
}

void Agent::removeSubAgent(const std::string &name) {
    subAgents_.erase(name);
}

bool Agent::hasSubAgent(const std::string &name) const {
    return subAgents_.count(name);
}

Agent *Agent::getSubAgent(const std::string &name) const {
    auto it = subAgents_.find(name);
    return (it != subAgents_.end()) ? it->second.get() : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════
// Environment
// ═══════════════════════════════════════════════════════════════════════

void Agent::setEnv(const std::string &key, const std::string &val) {
    env_[key] = val;
}

std::string Agent::getEnv(const std::string &key,
                          const std::string &def) const {
    auto it = env_.find(key);
    return (it != env_.end()) ? it->second : def;
}

// ── Harvest pending tools: complete results + progressive partial output ──


int Agent::reloadManifests(bool backup) {
    std::string dir = config_.manifestDir.empty() ? "./manifests" : config_.manifestDir;
    if (!std::filesystem::exists(dir)) return 0;
    if (backup) {
        auto ts = std::chrono::system_clock::now().time_since_epoch().count();
        std::string backupDir = dir + "/_backups/" + std::to_string(ts);
        std::filesystem::create_directories(backupDir);
    }
    int count = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dir);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file() || it->path().extension() != ".yml") continue;
        auto schema = ManifestLoader::loadToolManifest(it->path().string());
        if (schema.name.empty() || disabledBuiltins_.count(schema.name)) continue;
        // Skip non-tool manifests by checking kind field in YAML
        auto readYaml = [](const std::string& p) {
            std::ifstream f(p);
            if (!f) return std::string();
            return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        };
        auto yaml = readYaml(it->path().string());
        if (!yaml.empty()) {
            auto root = ManifestYaml::parse(yaml);
            std::string kind = ManifestYaml::get(root, "kind");
            if (kind == "Agent" || kind == "Workflow" || kind == "Feed" || kind == "Relic") continue;
        }
        ToolDef td;
        td.name = schema.name;
        td.description = schema.description;
        td.isNative = false;
        if (!schema.runtime.empty() && !schema.entrypoint.empty()) {
            td.scriptRuntime = schema.runtime;
            td.scriptPath = (it->path().parent_path() / schema.entrypoint).string();
        } else {
            // Not a script tool — skip
            continue;
        }
        tools_[td.name] = td;
        count++;
    }
    // Persist loaded tools to session manifest (survives restarts)
    if (count > 0) saveSessionTools();
    return count;
}

void Agent::saveSessionTools() {
    std::string dir = config_.manifestDir.empty() ? "./manifests" : config_.manifestDir;
    std::string sessionDir = dir + "/_session";
    std::filesystem::create_directories(sessionDir);
    Json::Value arr(Json::arrayValue);
    for (auto& [name, tool] : tools_) {
        if (tool.isNative || tool.scriptPath.empty()) continue;
        Json::Value t;
        t["name"] = name;
        t["description"] = tool.description;
        t["scriptRuntime"] = tool.scriptRuntime;
        t["scriptPath"] = tool.scriptPath;
        arr.append(t);
    }
    std::ofstream f(sessionDir + "/tools.json");
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    f << Json::writeString(w, arr);
}

void Agent::loadSessionTools() {
    std::string dir = config_.manifestDir.empty() ? "./manifests" : config_.manifestDir;
    std::string sessionFile = dir + "/_session/tools.json";
    if (!std::filesystem::exists(sessionFile)) return;
    std::ifstream f(sessionFile);
    if (!f) return;
    Json::Value arr;
    Json::CharReaderBuilder r;
    std::string errs;
    if (!Json::parseFromStream(r, f, &arr, &errs)) return;
    for (auto& t : arr) {
        if (!t.isMember("name")) continue;
        ToolDef td;
        td.name = t["name"].asString();
        td.description = t.get("description", "").asString();
        td.isNative = false;
        td.scriptRuntime = t.get("scriptRuntime", "").asString();
        td.scriptPath = t.get("scriptPath", "").asString();
        if (!disabledBuiltins_.count(td.name)) tools_[td.name] = td;
    }
}



} // namespace cortex::mk3
