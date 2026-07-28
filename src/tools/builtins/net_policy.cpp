// src/tools/builtins/net_policy.cpp — see net_policy.hpp for the contract.

#include "net_policy.hpp"

#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <json/json.h>
#include <mutex>

namespace cortex::mk3::tools::builtins::net {

namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Match either an exact host or ".<suffix>" — i.e. "x.evil.com" matches suffix
// "evil.com" but "evil.com.attacker.com" does NOT (sentinel dot check).
bool suffixMatch(const std::string& host, const std::string& pattern) {
    if (host == pattern) return true;
    if (host.size() > pattern.size() &&
        host.compare(host.size() - pattern.size(), pattern.size(), pattern) == 0 &&
        host[host.size() - pattern.size() - 1] == '.')
        return true;
    return false;
}

bool isBlockedV4(uint32_t addr_host) {
    uint8_t a = (addr_host >> 24) & 0xff;
    uint8_t b = (addr_host >> 16) & 0xff;
    if (a == 127) return true;                       // loopback
    if (a == 10) return true;                        // RFC1918
    if (a == 169 && b == 254) return true;           // link-local incl. cloud metadata
    if (a == 172 && b >= 16 && b <= 31) return true; // RFC1918
    if (a == 192 && b == 168) return true;            // RFC1918
    if (a == 100 && b >= 64 && b <= 127) return true; // CGNAT
    if (a == 0) return true;                          // "this network"
    return false;
}

bool isBlockedV6(const in6_addr& a) {
    static const unsigned char loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (std::memcmp(&a, loopback, 16) == 0) return true;
    if (a.s6_addr[0] == 0xfe && (a.s6_addr[1] & 0xc0) == 0x80) return true; // fe80::/10
    if ((a.s6_addr[0] & 0xfe) == 0xfc) return true;                         // fc00::/7 ULA
    return false;
}

struct UrlParts {
    std::string scheme, host, port;
};

bool splitUrl(const std::string& url, UrlParts& out) {
    CURLU* h = curl_url();
    if (!h) return false;
    if (curl_url_set(h, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        return false;
    }
    char* scheme = nullptr;
    char* host = nullptr;
    char* port = nullptr;
    bool ok = (curl_url_get(h, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK) &&
              (curl_url_get(h, CURLUPART_HOST, &host, 0) == CURLUE_OK);
    // Port may legitimately fail (URL without explicit port); fall back below.
    bool gotPort = curl_url_get(h, CURLUPART_PORT, &port, 0) == CURLUE_OK;

    if (ok) {
        out.scheme = toLower(scheme);
        out.host = toLower(host);
        out.port = gotPort ? std::string(port)
                           : (out.scheme == "https" ? "443" : "80");
    }
    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (port) curl_free(port);
    curl_url_cleanup(h);
    return ok;
}

struct Resolution {
    bool blocked = true; // default blocked (fail-closed)
    std::string ip;
};

Resolution resolveAndCheck(const std::string& host, const Policy& policy) {
    Resolution r;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return r;

    char ipbuf[INET6_ADDRSTRLEN] = {0};
    for (auto* it = res; it; it = it->ai_next) {
        if (!it->ai_addr) continue;
        if (it->ai_family == AF_INET) {
            auto* sa = reinterpret_cast<sockaddr_in*>(it->ai_addr);
            uint32_t addr_host = ntohl(sa->sin_addr.s_addr);
            if (policy.block_private_ranges && isBlockedV4(addr_host)) continue;
            if (inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf))) {
                r.blocked = false;
                r.ip = ipbuf;
                break;
            }
        } else if (it->ai_family == AF_INET6) {
            auto* sa6 = reinterpret_cast<sockaddr_in6*>(it->ai_addr);
            if (policy.block_private_ranges && isBlockedV6(sa6->sin6_addr)) continue;
            if (inet_ntop(AF_INET6, &sa6->sin6_addr, ipbuf, sizeof(ipbuf))) {
                r.blocked = false;
                r.ip = ipbuf;
                break;
            }
        }
    }
    freeaddrinfo(res);
    return r;
}

std::mutex& policyMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

Policy& Policy::global() {
    static Policy p = [] {
        Policy def;
        if (const char* path = std::getenv("CORTEX_NET_POLICY_FILE"))
            Policy::loadFromFile(path);
        return def;
    }();
    return p;
}

bool Policy::loadFromFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(policyMutex());
    std::ifstream f(path);
    if (!f) return false;
    Json::Value v;
    Json::CharReaderBuilder rb;
    std::string errs;
    if (!Json::parseFromStream(rb, f, &v, &errs)) return false;

    Policy& p = global();
    if (v.isMember("allow_domains") && v["allow_domains"].isArray())
        for (auto& d : v["allow_domains"]) p.allow_domains.push_back(toLower(d.asString()));
    if (v.isMember("deny_domains") && v["deny_domains"].isArray())
        for (auto& d : v["deny_domains"]) p.deny_domains.push_back(toLower(d.asString()));
    if (v.isMember("block_private_ranges"))
        p.block_private_ranges = v["block_private_ranges"].asBool();
    if (v.isMember("max_redirects")) p.max_redirects = v["max_redirects"].asInt();
    if (v.isMember("max_bytes")) p.max_bytes = v["max_bytes"].asInt64();
    if (v.isMember("max_header_count")) p.max_header_count = v["max_header_count"].asInt();
    return true;
}

bool isBlockedHost(const std::string& host, const Policy& policy) {
    return resolveAndCheck(host, policy).blocked;
}

ValidationResult validate(const std::string& url, const Policy& policy) {
    ValidationResult out;
    UrlParts parts;
    if (!splitUrl(url, parts)) {
        out.error = "malformed URL";
        return out;
    }
    out.scheme = parts.scheme;
    out.host = parts.host;
    out.port = parts.port;

    if (out.scheme != "http" && out.scheme != "https") {
        out.error = "scheme not allowed: " + out.scheme;
        return out;
    }
    for (auto& d : policy.deny_domains) {
        if (suffixMatch(out.host, toLower(d))) {
            out.error = "host denied by policy: " + out.host;
            return out;
        }
    }
    if (!policy.allow_domains.empty()) {
        bool allowed = false;
        for (auto& d : policy.allow_domains) {
            if (suffixMatch(out.host, toLower(d))) { allowed = true; break; }
        }
        if (!allowed) {
            out.error = "host not in allowlist: " + out.host;
            return out;
        }
    }
    Resolution res = resolveAndCheck(out.host, policy);
    if (res.blocked) {
        out.error = "host resolves to a blocked network range: " + out.host;
        return out;
    }
    out.resolved_ip = res.ip;
    out.ok = true;
    return out;
}

}  // namespace cortex::mk3::tools::builtins::net
