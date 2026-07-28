// src/tools/builtins/net_policy.hpp — shared SSRF / domain / IP-range guard.
//
// All agent-influenced URLs (the URL itself or anything pulled from search
// results) MUST go through `net::validate()` before hitting libcurl. The
// returned `resolved_ip` MUST be forced into curl via CURLOPT_RESOLVE so DNS
// rebinding can't bypass validation between resolve-request and connect.
//
// Operator-controlled URLs (e.g. the operator's own SearxNG instance or
// CORTEX_SEARXNG_URL env) explicitly do NOT use validate() — the network
// policy applies to URLs the agent or its tool outputs chose.

#pragma once

#include <string>
#include <vector>

namespace cortex::mk3::tools::builtins::net {

struct Policy {
    // Empty = allow all except denied routes.
    std::vector<std::string> allow_domains;
    // Suffix match (".corp.internal" matches "x.corp.internal"). All comparisons
    // happen on the lowercase host string.
    std::vector<std::string> deny_domains;
    bool block_private_ranges = true;
    int max_redirects = 5;
    long max_bytes = 10L * 1024 * 1024;
    int max_header_count = 32;

    static Policy& global();
    // Merges fields from a JSON file (deny/add lists + limits) into global(),
    // preserving any field already set when the file doesn't define it.
    // Returns false if the file is unreadable or unparsable.
    static bool loadFromFile(const std::string& path);
};

struct ValidationResult {
    bool ok = false;
    std::string error;
    std::string scheme;
    std::string host;
    std::string port;
    // The exact IP the validation passed; callers pin it with CURLOPT_RESOLVE.
    std::string resolved_ip;
};

// Full check: scheme allowlist, allow/deny list match, A/AAAA resolution,
// then IP-range guard (RFC1918, loopback, link-local incl. 169.254.169.254 —
// cloud metadata — ULA fc00::/7, etc). Fails closed: unresolvable = blocked.
// Empty allow_domains with at least one deny::match denies that suffix;
// without any deny::match and empty allow::list, the URL is allowed provided
// it resolves to a non-blocked network range.
ValidationResult validate(const std::string& url, const Policy& policy);

// Bool wrapper.
bool isBlockedHost(const std::string& host, const Policy& policy);

}  // namespace cortex::mk3::tools::builtins::net
