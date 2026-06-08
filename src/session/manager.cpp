// =============================================================================
// agent-lib-MK3 — Session Manager Implementation
// =============================================================================

#include "manager.hpp"
#include <fstream>
#include <sstream>
#include <ctime>
#include <algorithm>
#include <iostream>

namespace cortex::mk3::session {

// ─── Helpers ──────────────────────────────────────────────────────────
static std::string nowISO() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

std::string SessionManager::iso8601() { return nowISO(); }

static const char* roleStr(SessionRecord::Role r) {
    switch (r) {
        case SessionRecord::USER: return "user";
        case SessionRecord::AGENT: return "agent";
        case SessionRecord::TOOL_CALL: return "tool_call";
        case SessionRecord::TOOL_RESULT: return "tool_result";
        case SessionRecord::SYSTEM: return "system";
    }
    return "system";
}

static SessionRecord::Role parseRole(const std::string& s) {
    if (s == "user") return SessionRecord::USER;
    if (s == "agent") return SessionRecord::AGENT;
    if (s == "tool_call") return SessionRecord::TOOL_CALL;
    if (s == "tool_result") return SessionRecord::TOOL_RESULT;
    return SessionRecord::SYSTEM;
}

// ─── SessionManager ───────────────────────────────────────────────────
SessionManager::SessionManager(const std::string& baseDir)
    : baseDir_(baseDir.empty() ? std::string(std::getenv("HOME")?:"/tmp") + "/.cortex/sessions" : baseDir) {}

std::string SessionManager::sessionPath(const std::string& id) const {
    std::string safe;
    for (char c : id) {
        if (std::isalnum((unsigned char)c) || c == '-' || c == '_') safe += c;
    }
    if (safe.empty()) safe = "default";
    return baseDir_ + "/" + safe + ".json";
}

Session SessionManager::load(const std::string& id) const {
    std::string path = sessionPath(id);
    Session s; s.id = id;
    if (!std::filesystem::exists(path)) return s;
    try {
        std::ifstream f(path); Json::Value root; f >> root;
        s.id = root.get("id", id).asString();
        s.agentName = root.get("agent_name", "").asString();
        s.model = root.get("model", "").asString();
        s.provider = root.get("provider", "").asString();
        s.created = root.get("created", "").asString();
        s.updated = root.get("updated", "").asString();
        if (root.isMember("records")) {
            for (auto& rv : root["records"]) {
                SessionRecord r;
                r.role = parseRole(rv.get("role","system").asString());
                r.content = rv.get("content","").asString();
                r.timestamp = rv.get("timestamp","").asString();
                r.metadata = rv.get("metadata","").asString();
                s.records.push_back(r);
            }
        }
        if (root.isMember("metadata")) {
            for (auto& k : root["metadata"].getMemberNames())
                s.metadata[k] = root["metadata"][k].asString();
        }
        return s;
    } catch (...) { return s; }
}

void SessionManager::save(const Session& s) const {
    std::filesystem::create_directories(baseDir_);
    Json::Value root;
    root["id"] = s.id;
    root["agent_name"] = s.agentName;
    root["model"] = s.model;
    root["provider"] = s.provider;
    root["created"] = s.created;
    root["updated"] = s.updated;
    Json::Value recs(Json::arrayValue);
    for (auto& r : s.records) {
        Json::Value rv;
        rv["role"] = roleStr(r.role);
        rv["content"] = r.content;
        rv["timestamp"] = r.timestamp;
        if (!r.metadata.empty()) rv["metadata"] = r.metadata;
        recs.append(rv);
    }
    root["records"] = recs;
    Json::Value meta(Json::objectValue);
    for (auto& [k,v] : s.metadata) meta[k] = v;
    root["metadata"] = meta;
    std::string path = sessionPath(s.id);
    std::ofstream f(path + ".tmp");
    Json::StreamWriterBuilder w; w["indentation"] = "  ";
    f << Json::writeString(w, root);
    f.close();
    std::filesystem::rename(path + ".tmp", path);
}

void SessionManager::remove(const std::string& id) const {
    std::filesystem::remove(sessionPath(id));
}

bool SessionManager::exists(const std::string& id) const {
    return std::filesystem::exists(sessionPath(id));
}

Session SessionManager::create(const std::string& id, const std::string& agent,
                                const std::string& model, const std::string& provider) const {
    Session s; s.id = id; s.agentName = agent;
    s.model = model; s.provider = provider;
    s.created = s.updated = nowISO();
    save(s); return s;
}

void SessionManager::appendRecord(const std::string& id, const SessionRecord& r) const {
    Session s = load(id);
    s.records.push_back(r);
    s.updated = nowISO();
    save(s);
}

std::vector<SessionManager::SessionInfo> SessionManager::list() const {
    std::vector<SessionInfo> result;
    if (!std::filesystem::exists(baseDir_)) return result;
    for (auto& e : std::filesystem::directory_iterator(baseDir_)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        try {
            std::ifstream f(e.path()); Json::Value root; f >> root;
            SessionInfo info;
            info.id = root.get("id","").asString();
            info.agentName = root.get("agent_name","").asString();
            info.model = root.get("model","").asString();
            info.updated = root.get("updated","").asString();
            info.turnCount = root.isMember("records") ? root["records"].size() : 0;
            result.push_back(info);
        } catch (...) {}
    }
    std::sort(result.begin(), result.end(), [](auto& a, auto& b) { return a.updated > b.updated; });
    return result;
}

void SessionManager::prune(const std::string& id, size_t maxRecords) const {
    Session s = load(id);
    if (s.records.size() <= maxRecords) return;
    size_t remove = s.records.size() - maxRecords;
    if (!s.records.empty()) remove = std::min(remove, s.records.size()-1);
    s.records.erase(s.records.begin()+1, s.records.begin()+1+remove);
    s.updated = nowISO(); save(s);
}

bool SessionManager::exportToFile(const std::string& id, const std::string& path) const {
    Session s = load(id);
    if (s.records.empty() && s.id.empty()) return false;
    Json::Value root;
    root["format"] = "cortex-session-portable"; root["version"] = "1.0";
    root["id"] = s.id; root["agent"] = s.agentName;
    root["model"] = s.model; root["provider"] = s.provider;
    root["created"] = s.created; root["updated"] = s.updated;
    Json::Value msgs(Json::arrayValue);
    for (auto& r : s.records) {
        Json::Value m; m["role"] = roleStr(r.role); m["content"] = r.content;
        if (!r.timestamp.empty()) m["timestamp"] = r.timestamp;
        msgs.append(m);
    }
    root["messages"] = msgs;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    Json::StreamWriterBuilder w; w["indentation"] = "  ";
    f << Json::writeString(w, root);
    return true;
}

Session SessionManager::importFromFile(const std::string& path) const {
    Session s;
    if (!std::filesystem::exists(path)) return s;
    std::ifstream f(path); Json::Value root; f >> root;
    s.id = root.get("id","").asString();
    s.agentName = root.get("agent", root.get("agent_name","")).asString();
    s.model = root.get("model","").asString();
    s.provider = root.get("provider","").asString();
    s.created = root.get("created","").asString();
    s.updated = root.get("updated","").asString();
    auto& msgs = root.isMember("messages") ? root["messages"] : root["records"];
    for (auto& mv : msgs) {
        SessionRecord r;
        r.role = parseRole(mv.get("role","system").asString());
        r.content = mv.get("content","").asString();
        r.timestamp = mv.get("timestamp","").asString();
        s.records.push_back(r);
    }
    save(s);
    return s;
}

// Legacy format
std::vector<SessionRecord> SessionManager::importLegacyHistory(const std::vector<std::string>& h) {
    std::vector<SessionRecord> recs;
    for (auto& line : h) {
        SessionRecord r;
        if (line.rfind("User:",0)==0) { r.role=SessionRecord::USER; r.content=line.substr(6); }
        else if (line.rfind("Agent:",0)==0) { r.role=SessionRecord::AGENT; r.content=line.substr(7); }
        else if (line.rfind("System (Action Result):",0)==0) { r.role=SessionRecord::TOOL_RESULT; r.content=line.substr(23); }
        else if (line.rfind("System:",0)==0) { r.role=SessionRecord::SYSTEM; r.content=line.substr(8); }
        else { r.role=SessionRecord::SYSTEM; r.content=line; }
        // trim
        while (!r.content.empty() && r.content[0]==' ') r.content.erase(0,1);
        recs.push_back(r);
    }
    return recs;
}

std::vector<std::string> SessionManager::exportLegacyHistory(const std::vector<SessionRecord>& recs) {
    std::vector<std::string> h;
    for (auto& r : recs) {
        switch (r.role) {
            case SessionRecord::USER: h.push_back("User: "+r.content); break;
            case SessionRecord::AGENT: h.push_back("Agent: "+r.content); break;
            case SessionRecord::TOOL_CALL: h.push_back("Tool Call: "+r.content); break;
            case SessionRecord::TOOL_RESULT: h.push_back("System (Action Result): "+r.content); break;
            default: h.push_back("System: "+r.content); break;
        }
    }
    return h;
}

} // namespace cortex::mk3::session
