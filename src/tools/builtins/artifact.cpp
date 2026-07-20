// src/tools/builtins/artifact.cpp — project-local artifact store builtin
#include "artifact.hpp"
#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace cortex::mk3::tools::builtins {

static fs::path artifactRoot(const Json::Value& p) {
    std::string root = p.get("root", ".cortex/artifacts").asString();
    return fs::path(root).lexically_normal();
}

static bool safePart(const std::string& s) {
    if (s.empty() || s == "." || s == "..")
        return false;
    return s.find('/') == std::string::npos && s.find('\\') == std::string::npos;
}

static std::string keyToPath(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.')
            out += c;
        else
            out += '_';
    }
    if (out.empty())
        out = "artifact";
    return out;
}

static std::string readFile(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool writeAtomic(const fs::path& target, const std::string& content, std::string& err) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        err = "failed to create directory: " + ec.message();
        return false;
    }
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp = target.parent_path() / ("." + target.filename().string() + ".tmp." + std::to_string(stamp));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to open temp file: " + tmp.string();
            return false;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            fs::remove(tmp, ec);
            err = "failed writing temp file: " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        err = "failed to replace artifact file: " + ec.message();
        return false;
    }
    return true;
}

static int parseVersionFile(const fs::path& p) {
    std::string name = p.filename().string();
    if (name.size() < 7 || name[0] != 'v' || name.substr(name.size() - 5) != ".data")
        return 0;
    try {
        return std::stoi(name.substr(1, name.size() - 6));
    } catch (...) {
        return 0;
    }
}

static int latestVersion(const fs::path& dir) {
    if (!fs::exists(dir))
        return 0;
    int latest = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file())
            continue;
        latest = std::max(latest, parseVersionFile(e.path()));
    }
    return latest;
}

static Json::Value tagsArray(const Json::Value& p) {
    Json::Value tags(Json::arrayValue);
    if (p.isMember("tags") && p["tags"].isArray()) {
        for (const auto& t : p["tags"])
            tags.append(t.asString());
    } else if (p.isMember("tag")) {
        tags.append(p["tag"].asString());
    }
    return tags;
}

static bool hasTag(const Json::Value& meta, const std::string& tag) {
    if (tag.empty())
        return true;
    if (!meta.isMember("tags") || !meta["tags"].isArray())
        return false;
    for (const auto& t : meta["tags"])
        if (t.asString() == tag)
            return true;
    return false;
}

static std::string jsonContentString(const Json::Value& v) {
    if (v.isString())
        return v.asString();
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    return Json::writeString(w, v);
}

static fs::path artifactDir(const fs::path& root, const std::string& ns, const std::string& key) {
    return root / ns / keyToPath(key);
}

static std::string artifactId(const std::string& ns, const std::string& key, int version) {
    return ns + "/" + key + "@v" + std::to_string(version);
}

static std::string opPut(const Json::Value& p) {
    std::string ns = p.get("namespace", "default").asString();
    std::string key = p.get("key", p.get("name", "").asString()).asString();
    if (!safePart(ns))
        return jsonErr("namespace must be a safe path segment");
    if (key.empty())
        return jsonErr("key is required");
    if (!p.isMember("content"))
        return jsonErr("content is required");

    fs::path root = artifactRoot(p);
    fs::path dir = artifactDir(root, ns, key);
    int version = latestVersion(dir) + 1;
    std::string content = jsonContentString(p["content"]);
    fs::path dataPath = dir / ("v" + std::to_string(version) + ".data");
    fs::path metaPath = dir / ("v" + std::to_string(version) + ".json");

    Json::Value meta;
    meta["namespace"] = ns;
    meta["key"] = key;
    meta["safe_key"] = keyToPath(key);
    meta["version"] = version;
    meta["artifact_id"] = artifactId(ns, key, version);
    meta["content_type"] = p.get("content_type", p["content"].isString() ? "text/plain" : "application/json").asString();
    meta["bytes"] = static_cast<Json::UInt64>(content.size());
    meta["tags"] = tagsArray(p);
    meta["created_ms"] = static_cast<Json::Int64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (p.isMember("metadata"))
        meta["metadata"] = p["metadata"];

    std::string err;
    if (!writeAtomic(dataPath, content, err))
        return jsonErr(err);
    if (!writeAtomic(metaPath, jsonStr(meta), err))
        return jsonErr(err);

    Json::Value r;
    r["success"] = true;
    r["artifact"] = meta;
    r["path"] = dataPath.string();
    r["output"] = "stored " + meta["artifact_id"].asString();
    return jsonStr(r);
}

static bool loadMeta(const fs::path& path, Json::Value& meta) {
    std::ifstream f(path);
    if (!f)
        return false;
    Json::CharReaderBuilder b;
    std::string errs;
    return Json::parseFromStream(b, f, &meta, &errs);
}

static std::string opGet(const Json::Value& p) {
    std::string ns = p.get("namespace", "default").asString();
    std::string key = p.get("key", p.get("name", "").asString()).asString();
    if (!safePart(ns))
        return jsonErr("namespace must be a safe path segment");
    if (key.empty())
        return jsonErr("key is required");
    fs::path dir = artifactDir(artifactRoot(p), ns, key);
    int version = p.get("version", 0).asInt();
    if (version <= 0)
        version = latestVersion(dir);
    if (version <= 0)
        return jsonErr("artifact not found: " + ns + "/" + key);
    fs::path dataPath = dir / ("v" + std::to_string(version) + ".data");
    fs::path metaPath = dir / ("v" + std::to_string(version) + ".json");
    if (!fs::exists(dataPath))
        return jsonErr("artifact version not found: " + artifactId(ns, key, version));
    Json::Value meta;
    loadMeta(metaPath, meta);
    Json::Value r;
    r["success"] = true;
    r["artifact"] = meta;
    r["content"] = readFile(dataPath);
    r["path"] = dataPath.string();
    r["output"] = "loaded " + artifactId(ns, key, version);
    return jsonStr(r);
}

static std::vector<Json::Value> collectArtifacts(const fs::path& root, const std::string& ns,
                                                 const std::string& prefix,
                                                 const std::string& tag, int limit) {
    std::vector<Json::Value> items;
    fs::path base = root / ns;
    if (!fs::exists(base))
        return items;
    for (const auto& dir : fs::directory_iterator(base)) {
        if (!dir.is_directory())
            continue;
        int v = latestVersion(dir.path());
        if (v <= 0)
            continue;
        Json::Value meta;
        if (!loadMeta(dir.path() / ("v" + std::to_string(v) + ".json"), meta))
            continue;
        std::string key = meta.get("key", "").asString();
        if (!prefix.empty() && key.rfind(prefix, 0) != 0)
            continue;
        if (!hasTag(meta, tag))
            continue;
        items.push_back(meta);
        if ((int)items.size() >= limit)
            break;
    }
    return items;
}

static std::string opList(const Json::Value& p) {
    std::string ns = p.get("namespace", "default").asString();
    if (!safePart(ns))
        return jsonErr("namespace must be a safe path segment");
    std::string prefix = p.get("prefix", "").asString();
    std::string tag = p.get("tag", "").asString();
    int limit = std::clamp(p.get("limit", 100).asInt(), 1, 1000);
    auto items = collectArtifacts(artifactRoot(p), ns, prefix, tag, limit);
    Json::Value arr(Json::arrayValue);
    for (auto& item : items)
        arr.append(item);
    Json::Value r;
    r["success"] = true;
    r["namespace"] = ns;
    r["artifacts"] = arr;
    r["count"] = static_cast<Json::UInt64>(items.size());
    return jsonStr(r);
}

static std::string opSearch(const Json::Value& p) {
    std::string query = p.get("query", "").asString();
    if (query.empty())
        return jsonErr("query is required");
    std::string ns = p.get("namespace", "default").asString();
    int limit = std::clamp(p.get("limit", 50).asInt(), 1, 500);
    auto items = collectArtifacts(artifactRoot(p), ns, "", "", 10000);
    Json::Value arr(Json::arrayValue);
    for (auto& meta : items) {
        fs::path dataPath = artifactDir(artifactRoot(p), ns, meta["key"].asString()) /
                            ("v" + std::to_string(meta["version"].asInt()) + ".data");
        std::string hay = meta["key"].asString() + "\n" + jsonStr(meta) + "\n" + readFile(dataPath);
        if (hay.find(query) == std::string::npos)
            continue;
        arr.append(meta);
        if ((int)arr.size() >= limit)
            break;
    }
    Json::Value r;
    r["success"] = true;
    r["query"] = query;
    r["artifacts"] = arr;
    r["count"] = arr.size();
    return jsonStr(r);
}

static std::string opDelete(const Json::Value& p) {
    std::string ns = p.get("namespace", "default").asString();
    std::string key = p.get("key", p.get("name", "").asString()).asString();
    if (!safePart(ns))
        return jsonErr("namespace must be a safe path segment");
    if (key.empty())
        return jsonErr("key is required");
    fs::path dir = artifactDir(artifactRoot(p), ns, key);
    std::error_code ec;
    bool removed = false;
    if (p.isMember("version")) {
        int v = p["version"].asInt();
        removed = fs::remove(dir / ("v" + std::to_string(v) + ".data"), ec) || removed;
        removed = fs::remove(dir / ("v" + std::to_string(v) + ".json"), ec) || removed;
    } else {
        removed = fs::remove_all(dir, ec) > 0;
    }
    Json::Value r;
    r["success"] = removed && !ec;
    r["removed"] = removed;
    if (ec)
        r["error"] = ec.message();
    return jsonStr(r);
}

static std::string opStats(const Json::Value& p) {
    fs::path root = artifactRoot(p);
    Json::Value r;
    r["success"] = true;
    r["root"] = root.string();
    int artifacts = 0, versions = 0;
    uint64_t bytes = 0;
    if (fs::exists(root)) {
        for (const auto& e : fs::recursive_directory_iterator(root)) {
            if (!e.is_regular_file())
                continue;
            if (e.path().extension() == ".data") {
                ++versions;
                std::error_code ec;
                bytes += e.file_size(ec);
            }
        }
        for (const auto& ns : fs::directory_iterator(root))
            if (ns.is_directory())
                for (const auto& key : fs::directory_iterator(ns.path()))
                    if (key.is_directory() && latestVersion(key.path()) > 0)
                        ++artifacts;
    }
    r["artifacts"] = artifacts;
    r["versions"] = versions;
    r["bytes"] = static_cast<Json::UInt64>(bytes);
    return jsonStr(r);
}

static std::string artifactRaw(const Json::Value& p) {
    std::string op = p.get("op", p.get("action", "put").asString()).asString();
    if (op == "put" || op == "create" || op == "store")
        return opPut(p);
    if (op == "get" || op == "read")
        return opGet(p);
    if (op == "list")
        return opList(p);
    if (op == "search")
        return opSearch(p);
    if (op == "delete" || op == "remove")
        return opDelete(p);
    if (op == "stats")
        return opStats(p);
    return jsonErr("unknown artifact op: " + op);
}

std::string artifact(const Json::Value& p) {
    return artifactStreaming(p, {});
}

std::string artifactStreaming(const Json::Value& p,
                              const std::function<void(const std::string&, bool)>& stream) {
    std::string raw = artifactRaw(p);
    if (stream) {
        Json::Value parsed;
        Json::CharReaderBuilder r;
        std::string errs;
        std::istringstream ss(raw);
        if (Json::parseFromStream(r, ss, &parsed, &errs)) {
            if (parsed.isMember("output") && parsed["output"].isString())
                stream(parsed["output"].asString() + "\n", false);
            else if (parsed.isMember("content") && parsed["content"].isString())
                stream(parsed["content"].asString(), false);
            else
                stream(jsonStr(parsed) + "\n", false);
        } else {
            stream(raw + "\n", false);
        }
    }
    return raw;
}

}  // namespace cortex::mk3::tools::builtins
