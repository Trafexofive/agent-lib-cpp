// =============================================================================
// agent-lib-MK3 — Internal Tools
// Ported from MK2 internal_tools.cpp. Same tools, MK3 namespaces.
// =============================================================================

#include "registry.hpp"
#include "../core/types.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <curl/curl.h>
#include <iostream>
#include <unistd.h>
#include "../utils/ansi.hpp"

namespace cortex::mk3::tools {

// ─── Helpers ──────────────────────────────────────────────────────────
static std::string jsonStr(const Json::Value& v) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    return Json::writeString(w, v);
}

static std::string jsonErr(const std::string& msg) {
    return jsonStr(ToolResult::fail(msg).toJson());
}

static std::string jsonOk(const Json::Value& data) {
    Json::Value r;
    r["success"] = true;
    r["result"] = data;
    return jsonStr(r);
}

static std::string shellEscape(const std::string& input) {
    std::string out(1, '\x27');
    for (char c : input) {
        if (c == '\x27') { out += "'\\"; out += '\x27'; out += '\x27'; }
        else out += c;
    }
    out += '\x27';
    return out;
}

static int runCmd(const std::string& cmd, std::string& output, int timeoutSec = 30) {
    // Use system timeout to enforce hard kill — fgets/pclose can hang forever on
    // commands that produce no output (sleep, tail -f, hanging network calls).
    std::string fullCmd = "timeout " + std::to_string(timeoutSec) + " " + cmd + " 2>&1";
    FILE* pipe = popen(fullCmd.c_str(), "r");
    if (!pipe) return -1;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }
    int rc = pclose(pipe);
    // timeout exits 124 on kill, 128+SIGTERM otherwise
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// CURL helpers
static size_t curlWrite(void* ptr, size_t sz, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(static_cast<char*>(ptr), sz * nmemb);
    return sz * nmemb;
}

// ─── Tool: exec ───────────────────────────────────────────────────────
static std::string toolExec(const Json::Value& p) {
    std::string cmd = p.get("command", p.get("cmd", p.get("input", "")).asString()).asString();
    if (cmd.empty()) return jsonErr("command is required");
    std::string cwd = p.get("cwd", "").asString();
    int timeout = p.get("timeout", 30).asInt();
    std::string fullCmd = cwd.empty() ? cmd : "cd " + shellEscape(cwd) + " && " + cmd;
    std::string out; int rc = runCmd(fullCmd, out, timeout);
    Json::Value r; r["success"] = true; r["exit_code"] = rc; r["output"] = out;
    return jsonStr(r);
}

// ─── Tool: list ───────────────────────────────────────────────────────
static std::string toolList(const Json::Value& p) {
    std::string path = p.get("path", ".").asString();
    std::string pattern = p.get("pattern", "").asString();
    bool recursive = p.get("recursive", false).asBool();
    std::string type = p.get("type", "all").asString();
    std::string cmd = recursive
        ? "find " + shellEscape(path) + (type=="dir"?" -type d":type=="file"?" -type f":"")
          + (pattern.empty()?"":" -name "+shellEscape(pattern)) + " 2>/dev/null | head -200"
        : "ls -la " + shellEscape(pattern.empty() ? path : path+"/"+pattern) + " 2>/dev/null";
    std::string out; int rc = runCmd(cmd, out, 15);
    int count = 0; std::istringstream iss(out); std::string line;
    while (std::getline(iss, line)) if (!line.empty()) count++;
    Json::Value r; r["success"] = true; r["count"] = count; r["results"] = out;
    return jsonStr(r);
}

// ─── Tool: grep ───────────────────────────────────────────────────────
static std::string toolGrep(const Json::Value& p) {
    std::string pattern = p.get("pattern", "").asString();
    if (pattern.empty()) return jsonErr("pattern is required");
    std::string path = p.get("path", ".").asString();
    std::string glob = p.get("glob", "*").asString();
    int ctx = p.get("context", 0).asInt();
    std::string cmd = "grep -rn --include=" + shellEscape(glob)
        + (ctx>0?" -C "+std::to_string(ctx):"")
        + " " + shellEscape(pattern) + " " + shellEscape(path) + " 2>/dev/null | head -100";
    std::string out; runCmd(cmd, out, 30);
    int matches = 0;
    std::istringstream lines(out);
    std::string line;
    while (std::getline(lines, line)) if (!line.empty()) matches++;
    Json::Value r;
    r["success"] = true;
    r["matches"] = matches;
    r["results"] = out;
    r["output"] = "matches=" + std::to_string(matches) + "\n" + out;
    return jsonStr(r);
}

// ─── Tool: fs_read ────────────────────────────────────────────────────
static std::string toolFsRead(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    if (path.empty()) return jsonErr("path is required");
    int offset = p.get("offset", 0).asInt();
    int limit = p.get("limit", 0).asInt();
    std::ifstream f(path); if (!f) return jsonErr("file not found: " + path);
    std::string content, line; int ln=0, total=0;
    while (std::getline(f, line)) { total++;
        if (offset>0 && ln<offset) { ln++; continue; }
        if (limit>0 && ln-offset >= limit) { ln++; continue; }
        content += line + "\n"; ln++;
    }
    Json::Value r; r["success"] = true; r["content"] = content;
    r["lines"] = total; r["truncated"] = (limit>0 && total>offset+limit);
    return jsonStr(r);
}

// ─── Tool: fs_write ───────────────────────────────────────────────────
static std::string toolFsWrite(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    std::string content = p.get("content", "").asString();
    if (path.empty()) return jsonErr("path is required");
    bool append = p.get("append", false).asBool();
    auto lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos) {
        std::string d; runCmd("mkdir -p " + shellEscape(path.substr(0, lastSlash)), d, 5);
    }
    std::ofstream f(path, append ? std::ios::app : std::ios::out);
    if (!f) return jsonErr("failed to write: " + path);
    f << content; f.close();
    Json::Value r; r["success"] = true; r["path"] = path;
    return jsonStr(r);
}

// ─── Tool: json ───────────────────────────────────────────────────────
static std::string toolJson(const Json::Value& p) {
    // Accept both names: the prompt/schema advertises `op`, older callers used
    // `action`. Keep both so manifests and existing prompts don't drift.
    std::string op = p.get("op", "").asString();
    if (op.empty()) op = p.get("action", "").asString();
    if (op.empty()) return jsonErr("op is required (parse|query|validate|pretty|minify)");

    auto parse = [&](Json::Value& v, std::string& err) -> bool {
        if (!p.isMember("data")) {
            err = "data is required";
            return false;
        }
        // Parser::resolveVars intentionally promotes strings that look like JSON
        // into Json::Value objects. Accept both raw JSON strings and already-
        // parsed object/array/scalar values.
        const Json::Value& dataVal = p["data"];
        if (!dataVal.isString()) {
            v = dataVal;
            return true;
        }
        Json::CharReaderBuilder r;
        std::istringstream ss(dataVal.asString());
        return Json::parseFromStream(r, ss, &v, &err);
    };

    if (op == "parse") {
        Json::Value v; std::string err;
        if (!parse(v, err)) return jsonErr("Invalid JSON: " + err);
        Json::Value out; out["success"] = true; out["value"] = v;
        return jsonStr(out);
    }

    if (op == "query") {
        Json::Value cur; std::string err;
        if (!parse(cur, err)) return jsonErr("Invalid JSON: " + err);
        std::string path = p.get("path", "").asString();
        if (path.empty()) path = p.get("query", "").asString();
        if (path.empty()) return jsonErr("query path is required");

        std::istringstream parts(path);
        std::string seg;
        while (std::getline(parts, seg, '.')) {
            if (seg.empty()) continue;
            if (cur.isObject()) {
                if (!cur.isMember(seg)) return jsonErr("query: key not found: " + seg);
                cur = cur[seg];
            } else if (cur.isArray()) {
                char* end = nullptr;
                long idx = std::strtol(seg.c_str(), &end, 10);
                if (!end || *end != '\0' || idx < 0 || idx >= (long)cur.size())
                    return jsonErr("query: invalid array index: " + seg);
                cur = cur[(Json::ArrayIndex)idx];
            } else {
                return jsonErr("query: cannot descend into scalar at: " + seg);
            }
        }
        Json::Value out; out["success"] = true; out["value"] = cur;
        return jsonStr(out);
    }

    if (op == "validate") {
        Json::Value v; std::string err;
        bool ok = parse(v, err);
        Json::Value out; out["valid"] = ok; if (!ok) out["errors"] = err; out["success"] = true;
        return jsonStr(out);
    }

    if (op == "pretty" || op == "minify") {
        Json::Value v; std::string err;
        if (!parse(v, err)) return jsonErr("Invalid JSON: " + err);
        Json::StreamWriterBuilder w; w["indentation"] = op=="pretty"?"  ":"";
        Json::Value out; out["success"] = true; out["formatted"] = Json::writeString(w, v);
        return jsonStr(out);
    }
    return jsonErr("Unknown json op: " + op);
}

// ─── Tool: web_fetch ──────────────────────────────────────────────────
static std::string toolWebFetch(const Json::Value& p) {
    std::string url = p.get("url", "").asString();
    if (url.empty()) return jsonErr("url is required");
    std::string method = p.get("method", "GET").asString();
    std::string data = p.get("data", "").asString();
    int timeout = p.get("timeout", 30).asInt();

    CURL* curl = curl_easy_init();
    if (!curl) return jsonErr("curl init failed");
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cortex-mk3/1.0");
    if (method == "POST" || method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        if (method == "PUT") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    }
    struct curl_slist* hdrs = nullptr;
    if (!data.empty()) hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    Json::Value r;
    if (res == CURLE_OK && httpCode >= 200 && httpCode < 400) {
        r["success"] = true; r["content"] = response; r["status"] = (int)httpCode;
    } else {
        r["success"] = false;
        r["error"] = res != CURLE_OK ? curl_easy_strerror(res) : "HTTP " + std::to_string(httpCode);
        r["status"] = (int)httpCode;
    }
    return jsonStr(r);
}

// ─── Tool: context_pin ────────────────────────────────────────────────
static std::string toolContextPin(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    if (path.empty()) return jsonErr("path is required");
    std::ifstream f(path); if (!f) return jsonErr("file not found: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string home = std::getenv("HOME") ?: "/tmp";
    std::string md = home + "/.cortex/pinned/";
    std::string d; runCmd("mkdir -p " + shellEscape(md), d, 5);
    std::string bn = path.substr(path.find_last_of('/')+1);
    { std::ofstream mc(md+bn+".content"); mc << content; }
    { std::ofstream mm(md+bn+".meta"); mm << path; }
    Json::Value r; r["success"] = true; r["path"] = path;
    r["size"] = (int)content.size(); r["mode"] = "pinned";
    return jsonStr(r);
}

// ─── Tool: context_peek ───────────────────────────────────────────────
static std::string toolContextPeek(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    if (path.empty()) return jsonErr("path is required");
    int cycles = p.get("cycles", 1).asInt();
    std::ifstream f(path); if (!f) return jsonErr("file not found: " + path);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string home = std::getenv("HOME") ?: "/tmp";
    std::string md = home + "/.cortex/ephemeral/";
    std::string d; runCmd("mkdir -p " + shellEscape(md), d, 5);
    std::string bn = path.substr(path.find_last_of('/')+1);
    { std::ofstream mc(md+bn+".content"); mc << content; }
    { std::ofstream mm(md+bn+".meta"); mm << path << "\n" << cycles; }
    Json::Value r; r["success"] = true; r["path"] = path;
    r["size"] = (int)content.size(); r["cycles"] = cycles; r["mode"] = "ephemeral";
    return jsonStr(r);
}

// ─── Tool: context_unpin ──────────────────────────────────────────────
static std::string toolContextUnpin(const Json::Value& p) {
    std::string path = p.get("path", "").asString();
    if (path.empty()) return jsonErr("path is required");
    std::string home = std::getenv("HOME") ?: "/tmp";
    std::string md = home + "/.cortex/pinned/";
    std::string bn = path.substr(path.find_last_of('/')+1);
    bool removed = false;
    if (std::filesystem::exists(md+bn+".content")) {
        std::filesystem::remove(md+bn+".content");
        std::filesystem::remove(md+bn+".meta"); removed = true;
    }
    Json::Value r; r["success"] = removed; r["path"] = path;
    return jsonStr(r);
}

// ─── Dispatch ─────────────────────────────────────────────────────────
static const std::map<std::string, std::string> ALIASES = {
    {"execute","exec"},{"cmd","exec"},{"run","exec"},
    {"search","grep"},{"find","grep"},
    {"read","fs_read"},{"cat","fs_read"},
    {"write","fs_write"},{"save","fs_write"},
    {"fetch","web_fetch"},{"curl","web_fetch"},{"http","web_fetch"},
    {"pin","context_pin"},{"peek","context_peek"},{"unpin","context_unpin"},
};

std::string dispatch(const std::string& toolName, const Json::Value& params) {
    auto& reg = ToolRegistry::instance();
    auto fn = reg.get(toolName);
    if (fn) return fn(params);
    auto it = ALIASES.find(toolName);
    if (it != ALIASES.end()) {
        fn = reg.get(it->second);
        if (fn) return fn(params);
    }
    return jsonErr("unknown tool: " + toolName);
}


// ── ask_tool — agent presents dialog cards to user, reads responses ──
static std::string toolAskTool(const Json::Value& p) {
    // Refuse to run if stdin is not a terminal (piped input)
    if (!isatty(STDIN_FILENO)) {
        return jsonErr("ask_tool requires interactive terminal (stdin is not a tty)");
    }

    std::string title = p.get("title", "Agent asks:").asString();
    std::string message = p.get("message", "").asString();
    std::cerr << "\n" << ansi::bold << ansi::green << "[AGENT] " << ansi::reset
              << title << "\n";
    if (!message.empty()) std::cerr << ansi::dim << message << ansi::reset << "\n";

    Json::Value results;
    if (p.isMember("cards")) {
        const Json::Value& cards = p["cards"];
        for (const auto& card : cards) {
            std::string id = card.get("id", "").asString();
            std::string ct = card.get("title", id).asString();
            std::string cm = card.get("message", "").asString();
            std::cerr << "\n" << ansi::cyan << "[" << id << "]" << ansi::reset
                      << " " << ct << "\n";
            if (!cm.empty()) std::cerr << ansi::dim << cm << ansi::reset << "\n";
            std::cerr << ansi::bold << ansi::green << "> " << ansi::reset << std::flush;
            std::string answer;
            if (!std::getline(std::cin, answer) || answer.empty()) {
                // EOF or empty — stop the card chain
                results[id] = "";
                break;
            }
            results[id] = answer;
        }
    } else {
        std::cerr << ansi::bold << ansi::green << "> " << ansi::reset << std::flush;
        std::string response;
        if (!std::getline(std::cin, response)) response = "";
        results["response"] = response;
    }

    Json::Value out;
    out["success"] = true;
    out["results"] = results;
    return jsonStr(out);
}

// ─── Register all defaults ────────────────────────────────────────────
void registerDefaults() {
    static bool done = false; if (done) return; done = true;
    auto& reg = ToolRegistry::instance();
    reg.registerFn("exec",          toolExec);
    reg.registerFn("list",          toolList);
    reg.registerFn("grep",          toolGrep);
    reg.registerFn("fs_read",       toolFsRead);
    reg.registerFn("fs_write",      toolFsWrite);
    reg.registerFn("json",          toolJson);
    reg.registerFn("web_fetch",     toolWebFetch);
    // context_pin / peek / unpin are handled directly in Agent::dispatchTool
    // because they need to mutate Agent state (pinned_/peeking_ maps).
    // Registering them here would shadow that path with the old write-only
    // implementations, so we leave them out of the registry.
    reg.registerFn("ask_tool",    toolAskTool);
}

} // namespace cortex::mk3::tools
