#pragma once
// Session id resolution, metadata persistence, and interactive resume pickers.

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

#include "src/cli/list_picker.hpp"
#include "src/cli/options.hpp"
#include "src/core/agent_catalog.hpp"
#include "src/session/controller.hpp"
#include "src/session/manager.hpp"
#include "src/ui/model/ui_prefs.hpp"

#include <filesystem>

namespace fs = std::filesystem;

namespace cortex::mk3::cli {

static std::vector<session::SessionManager::SessionInfo> sortedSessions() {
    ui::loadUiPrefs();
    bool global = ui::uiPrefShadow().globalSessions;
    // Global view is store-only (no CWD-legacy) so the list is identical
    // from any CWD; per-project keeps the CWD-local fallback + slug filter.
    session::SessionManager sm;
    auto sessions = sm.list(!global);
    if (!global) {
        std::vector<session::SessionManager::SessionInfo> scoped;
        scoped.reserve(sessions.size());
        for (auto& s : sessions)
            if (session::sessionInCurrentProject(s.id))
                scoped.push_back(std::move(s));
        sessions = std::move(scoped);
    }
    std::sort(sessions.begin(), sessions.end(),
              [](const auto& a, const auto& b) { return a.updated > b.updated; });
    return sessions;
}

// Session id mint lives in session::mintSessionId (controller.hpp).
static std::string newSessionId() {
    // Unified mint (session audit F6) — same scheme as hub lazy-arm / create / fork.
    return session::mintSessionId();
}

static std::string humanTime(const std::string& iso);
static std::string humanAge(const std::string& iso);
static std::string pickSessionInteractive(bool defaultIfEmpty) {
    auto sessions = sortedSessions();
    if (sessions.empty())
        return defaultIfEmpty ? newSessionId() : "";
    if (!isatty(STDIN_FILENO))
        return sessions[0].id;

    // Precompute display rows; run_list_picker styles each row at render time.
    struct Row {
        std::string id;
        std::string shortId;  // last 16 chars
        std::string name;
        std::string age;  // "2h ago"
        size_t turnCount = 0;
        std::string model;
    };
    std::vector<Row> rows;
    rows.reserve(sessions.size());
    session::SessionManager sm;
    for (const auto& s : sessions) {
        Row r;
        r.id = s.id;
        r.shortId = s.id.size() > 16 ? "…" + s.id.substr(s.id.size() - 15) : s.id;
        r.name = s.agentName;
        r.age = humanAge(s.updated);
        if (r.age.empty())
            r.age = s.updated.substr(11, 5);  // HH:MM
        r.turnCount = s.turnCount;
        r.model = s.model;
        if (sm.exists(s.id)) {
            Session loaded = sm.load(s.id);
            auto it = loaded.metadata.find("name");
            if (it != loaded.metadata.end() && !it->second.empty())
                r.name = it->second;
        }
        rows.push_back(std::move(r));
    }

    // Shared row renderer: pads columns and highlights the selection.
    auto render_row = [&](int i, bool isSel) -> std::string {
        const auto& r = rows[(size_t)i];
        std::string num = std::to_string(i + 1);
        while (num.size() < 3)
            num = " " + num;
        std::string shortId = r.shortId;
        while (shortId.size() < 16)
            shortId = " " + shortId;
        std::string agePad = r.age;
        while (agePad.size() < 9)
            agePad = " " + agePad;
        std::string msg = std::to_string(r.turnCount) + " msg";
        while (msg.size() < 7)
            msg = " " + msg;
        // Truncate fields to fit a typical 100-col terminal.
        std::string model = r.model.size() > 28 ? r.model.substr(0, 25) + "…" : r.model;
        std::string name = r.name.size() > 24 ? r.name.substr(0, 21) + "…" : r.name;
        std::ostringstream o;
        if (isSel) {
            o << "\033[1;36m│\033[0m  \033[7;36m" << num << " " << shortId << "  " << agePad
              << "  " << msg << "  " << std::left << std::setw(28) << model << "  "
              << std::setw(24) << name << "\033[0m";
        } else {
            o << "\033[2m│\033[0m  \033[2;34m" << num << "\033[0m " << shortId << "  \033[2m"
              << agePad << "  " << msg << "  \033[0m" << std::left << std::setw(28) << model
              << "  \033[3m" << std::setw(24) << name << "\033[0m";
        }
        return o.str();
    };

    cli::ListPickerConfig cfg;
    cfg.title = "┌─ Resume session";
    cfg.hint = std::to_string(rows.size()) +
               " total — j/k or ↑↓ move, 1-9 jump, d/u page, g/G top/bottom, "
               "Enter select, q/Esc cancel";
    int idx = cli::run_list_picker((int)rows.size(), render_row, cfg);
    // User cancelled — never mint a new session.
    if (idx < 0)
        return "";
    return rows[(size_t)idx].id;
}

static std::string resolveSessionId(const CliConfig& cli, bool defaultIfEmpty) {
    if (cli.resumePicker) {
        std::string picked = pickSessionInteractive(defaultIfEmpty);
        // If the user cancelled the picker, picked is "". Don't fall back to a
        // brand-new session — the caller treats empty as "no session" and
        // exits cleanly.
        return picked;
    }
    if (cli.continueSession) {
        auto sessions = sortedSessions();
        if (sessions.empty())
            return defaultIfEmpty ? newSessionId() : "";
        // Prefer the most recent session that actually has an agent reply
        // (not just a user prompt). A bare user prompt usually means the user
        // exited before the LLM finished — resuming that just shows a
        // half-typed hello and a frozen prompt, which is worse than resuming
        // the previous real session.
        session::SessionManager sm;
        for (const auto& s : sessions) {
            auto loaded = sm.load(s.id);
            for (const auto& r : loaded.records) {
                if (r.role == SessionRecord::AGENT || r.role == SessionRecord::TOOL_CALL ||
                    r.role == SessionRecord::TOOL_RESULT) {
                    return s.id;
                }
            }
        }
        return sessions[0].id;
    }
    if (!cli.sessionId.empty())
        return cli.sessionId;
    return defaultIfEmpty ? newSessionId() : "";
}

static void applySessionMetadata(CliConfig& cli, const std::string& sessionId) {
    if (sessionId.empty())
        return;
    session::SessionManager sm;
    if (!sm.exists(sessionId))
        return;
    auto session = sm.load(sessionId);
    auto get = [&](const std::string& key) -> std::string {
        auto it = session.metadata.find(key);
        return it == session.metadata.end() ? "" : it->second;
    };
    if (cli.manifestPath.empty())
        cli.manifestPath = get("manifest_path");
    // Resume identity: if manifest_path was never saved (older sessions) or
    // the path vanished, resolve by session.agentName so --continue does not
    // silently fall through to cortext-builtin-agent while the transcript
    // still looks like the original agent.
    if (cli.manifestPath.empty() && !session.agentName.empty() &&
        session.agentName != "cortext-builtin-agent" && session.agentName != "cortex") {
        std::string err;
        std::string resolved =
            catalog::resolveAgent(session.agentName, cli.manifestDir, &err);
        if (!resolved.empty())
            cli.manifestPath = resolved;
    } else if (!cli.manifestPath.empty()) {
        // Stale absolute path → try resolve by stem/name.
        std::error_code ec;
        if (!fs::is_regular_file(cli.manifestPath, ec)) {
            std::string name = session.agentName;
            if (name.empty()) {
                fs::path p(cli.manifestPath);
                name = p.parent_path().filename().string();
                if (name.empty() || name == "." || name == "/")
                    name = p.stem().string();
            }
            std::string err;
            std::string resolved = catalog::resolveAgent(name, cli.manifestDir, &err);
            if (!resolved.empty())
                cli.manifestPath = resolved;
            else
                cli.manifestPath.clear();  // fall through cleanly rather than load fail later
        }
    }
    if (cli.harnessPromptPath.empty())
        cli.harnessPromptPath = get("harness_path");
    if (cli.systemPromptPath.empty())
        cli.systemPromptPath = get("system_prompt_path");
    if (cli.personaPath.empty())
        cli.personaPath = get("persona_path");
    // Cognitive engine on resume — priority later is:
    //   CLI explicit > session file > agent.yml > config file > hardcoded
    // Use top-level session.provider/model FIRST (always written by save),
    // then metadata keys (older / partial files). Mark FromSession so config
    // defaults cannot clobber agent.yml on -c when session had no engine.
    std::string sessProv = session.provider;
    if (sessProv.empty())
        sessProv = get("provider");
    std::string sessModel = session.model;
    if (sessModel.empty())
        sessModel = get("model");
    if (!cli.providerSet && !sessProv.empty()) {
        cli.provider = sessProv;
        cli.providerFromSession = true;
    }
    if (!cli.modelSet && !sessModel.empty()) {
        cli.model = sessModel;
        cli.modelFromSession = true;
    }
    // Prefer session agentName when provider restore left name empty later.
    if (!session.agentName.empty())
        cli.sessionAgentName = session.agentName;
}

static void persistSessionMetadata(const std::string& sessionId, const CliConfig& cli,
                                   const AgentConfig& acfg) {
    if (sessionId.empty())
        return;
    session::SessionManager sm;
    auto session = sm.exists(sessionId)
                       ? sm.load(sessionId)
                       : sm.create(sessionId, acfg.name, acfg.model, acfg.provider);

    // Identity: never demote a real agent to cortext-builtin-agent on -c resume.
    // That was the --continue bug — bare resume without -m rebuilt acfg as
    // builtin, then this write stamped the session permanently as builtin.
    const std::string prevName = session.agentName;
    const bool acfgBuiltin =
        acfg.name.empty() || acfg.name == "cortext-builtin-agent" || acfg.name == "cortex";
    const bool prevReal =
        !prevName.empty() && prevName != "cortext-builtin-agent" && prevName != "cortex";
    if (!acfgBuiltin) {
        session.agentName = acfg.name;
    } else if (prevReal) {
        session.agentName = prevName;  // keep on-disk identity
    } else if (!cli.sessionAgentName.empty() && cli.sessionAgentName != "cortext-builtin-agent") {
        session.agentName = cli.sessionAgentName;
    } else if (session.agentName.empty()) {
        session.agentName = acfg.name.empty() ? "cortext-builtin-agent" : acfg.name;
    }

    // Engine tracks live /model switches; always refresh.
    if (!acfg.model.empty()) session.model = acfg.model;
    if (!acfg.provider.empty()) session.provider = acfg.provider;
    session.metadata["cwd"] = fs::current_path().string();
    if (!session.provider.empty()) session.metadata["provider"] = session.provider;
    if (!session.model.empty()) session.metadata["model"] = session.model;

    // Manifest path: prefer live cli, else keep prior, else backfill from agent name.
    if (!cli.manifestPath.empty()) {
        session.metadata["manifest_path"] = cli.manifestPath;
    } else if (session.metadata.count("manifest_path") == 0 && prevReal) {
        std::string err;
        std::string resolved =
            catalog::resolveAgent(session.agentName, cli.manifestDir, &err);
        if (!resolved.empty())
            session.metadata["manifest_path"] = resolved;
    } else if (session.metadata.count("manifest_path") == 0 && !acfgBuiltin &&
               !acfg.name.empty()) {
        std::string err;
        std::string resolved = catalog::resolveAgent(acfg.name, cli.manifestDir, &err);
        if (!resolved.empty())
            session.metadata["manifest_path"] = resolved;
    }

    if (!acfg.harnessPath.empty())
        session.metadata["harness_path"] = acfg.harnessPath;
    if (!acfg.systemPromptPath.empty())
        session.metadata["system_prompt_path"] = acfg.systemPromptPath;
    if (!acfg.personaPath.empty())
        session.metadata["persona_path"] = acfg.personaPath;
    if (!cli.sessionName.empty())
        session.metadata["name"] = cli.sessionName;
    else if (session.metadata.count("name"))
        session.metadata.erase("name");
    session.updated = session::SessionManager::iso8601();
    sm.save(session);
}

// ═══════════════════════════════════════════════════════════════════════
// Resume banner — printed to stderr so stdout stays clean for piping
// ═══════════════════════════════════════════════════════════════════════
static std::string humanTime(const std::string& iso) {
    if (iso.empty())
        return "unknown";
    // Accept "YYYY-MM-DDTHH:MM:SSZ" or "YYYY-MM-DDTHH:MM:SS.fffZ".
    if (iso.size() >= 16) {
        std::string out = iso.substr(0, 16);
        if (out.size() > 10)
            out[10] = ' ';
        return out;
    }
    return iso;
}

static std::string humanAge(const std::string& iso) {
    if (iso.empty())
        return "";
    std::tm tm{};
    if (iso.size() < 19 || !strptime(iso.substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &tm))
        return "";
    auto t = timegm(&tm);
    auto now = std::time(nullptr);
    auto diff = static_cast<long>(now - t);
    if (diff < 0)
        return "in the future";
    if (diff < 60)
        return std::to_string(diff) + "s ago";
    if (diff < 3600)
        return std::to_string(diff / 60) + "m ago";
    if (diff < 86400)
        return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

static void printResumeBanner(const std::string& sessionId, const std::string& kind,
                              size_t messageCount = 0, const std::string& forkSource = "") {
    if (sessionId.empty())
        return;
    session::SessionManager sm;
    auto session = sm.load(sessionId);
    std::string name = session.metadata.count("name") ? session.metadata.at("name") : "";
    std::string agent = session.agentName;
    std::string model = session.model;
    std::string provider = session.provider;
    std::string manifest =
        session.metadata.count("manifest_path") ? session.metadata.at("manifest_path") : "";
    std::string created = humanTime(session.created);
    std::string updated = humanTime(session.updated);
    std::string age = humanAge(session.updated);
    size_t turns = messageCount > 0 ? messageCount : session.records.size();

    std::cerr << "\033[2m[session]\033[0m \033[1m" << kind << " session: " << sessionId
              << "\033[0m";
    if (!name.empty())
        std::cerr << " (\033[3m" << name << "\033[0m)";
    std::cerr << "\n";
    if (!forkSource.empty())
        std::cerr << "\033[2m[session]\033[0m   forked from: " << forkSource << "\n";
    if (!agent.empty())
        std::cerr << "\033[2m[session]\033[0m   agent:     " << agent << "\n";
    if (!provider.empty() || !model.empty())
        std::cerr << "\033[2m[session]\033[0m   model:     " << provider << "/" << model << "\n";
    if (!manifest.empty())
        std::cerr << "\033[2m[session]\033[0m   manifest:  " << manifest << "\n";
    std::cerr << "\033[2m[session]\033[0m   created:   " << created << "\n";
    std::cerr << "\033[2m[session]\033[0m   updated:   " << updated << " (" << age << ")\n";
    std::cerr << "\033[2m[session]\033[0m   messages:  " << turns << "\n";
    if (turns > 0)
        std::cerr
            << "\033[2m[session]\033[0m Last records will render in the TUI above the prompt.\n";
    else
        std::cerr << "\033[2m[session]\033[0m (no records — this is an empty session)\n";
    std::cerr << "\033[2m[session]\033[0m To start fresh: \033[1mcortex-mk3 --no-session\033[0m\n";
}

}  // namespace cortex::mk3::cli
