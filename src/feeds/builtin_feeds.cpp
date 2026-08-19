// =============================================================================
// agent-lib-MK3 — Built-in feed pollers
// =============================================================================

#include "feed_engine.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <chrono>
#include <ctime>

#include "../utils/process.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace cortex::mk3::feeds {

FeedResult pollSystemClock() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

    std::tm tm;
    localtime_r(&t, &tm);

    char iso[32], human[64], date[16], tim[16];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(human, sizeof(human), "%A, %B %d %Y %H:%M:%S", &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    strftime(tim, sizeof(tim), "%H:%M:%S", &tm);

    std::ostringstream ss;
    ss << "Current time: " << human << " (ISO: " << iso << "." << std::setfill('0') << std::setw(6)
       << us.count() << ")\n"
       << "Unix: " << t << " | Date: " << date << " | Time: " << tim;

    FeedResult r;
    r.name = "system_clock";
    r.summary = ss.str();
    return r;
}

FeedResult pollSystemStats() {
    FeedResult r;
    r.name = "system_stats";

    struct utsname uts;
    if (uname(&uts) != 0) {
        r.ok = false;
        return r;
    }

    long cpuCount = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpuCount < 1)
        cpuCount = 1;

    // Memory from /proc/meminfo
    long memTotalKb = 0, memAvailKb = 0;
    std::ifstream mem("/proc/meminfo");
    if (mem) {
        std::string line;
        while (std::getline(mem, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                sscanf(line.c_str(), "MemTotal: %ld kB", &memTotalKb);
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                sscanf(line.c_str(), "MemAvailable: %ld kB", &memAvailKb);
            }
        }
    }

    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    std::ostringstream ss;
    ss << "Host: " << hostname << "\n"
       << "Platform: " << uts.sysname << " " << uts.release << " | Arch: " << uts.machine << "\n"
       << "Kernel: " << uts.version << "\n"
       << "CPU cores: " << cpuCount << "\n"
       << "Memory: " << (memTotalKb / 1024) << " MB total, " << (memAvailKb / 1024)
       << " MB available\n"
       << "PID: " << getpid();

    r.summary = ss.str();
    return r;
}

FeedResult pollWorkingDirectory() {
    FeedResult r;
    r.name = "working_directory";

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        r.ok = false;
        return r;
    }

    std::ostringstream ss;
    ss << "CWD: " << cwd;

    auto gitOut = [](const std::string& cmd) -> std::string {
        process::Spec spec;
        spec.shell = true;
        spec.command = cmd;
        spec.timeoutMs = 2000;
        spec.maxStdout = 4096;
        spec.maxStderr = 256;
        process::Result pr = process::run(spec);
        if (!pr.success())
            return {};
        std::string s = pr.stdoutText;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        return s;
    };

    std::string root = gitOut("git -C " + std::string(cwd) + " rev-parse --show-toplevel");
    if (!root.empty()) {
        ss << "\nGit repo: " << root;
        std::string br = gitOut("git -C " + root + " rev-parse --abbrev-ref HEAD");
        if (!br.empty())
            ss << " | Branch: " << br;
        process::Spec dirtySpec;
        dirtySpec.shell = true;
        dirtySpec.command = "git -C " + root + " diff --quiet";
        dirtySpec.timeoutMs = 2000;
        dirtySpec.maxStdout = 64;
        dirtySpec.maxStderr = 64;
        process::Result dirty = process::run(dirtySpec);
        if (!dirty.timedOut)
            ss << " | Dirty: " << (dirty.exitCode == 0 ? "no" : "yes");
        std::string hash = gitOut("git -C " + root + " rev-parse --short HEAD");
        if (!hash.empty())
            ss << " | Commit: " << hash;
    }

    r.summary = ss.str();
    return r;
}

void registerFeeds() {
    auto& engine = FeedEngine::instance();
    engine.registerFeed("system_clock", pollSystemClock);
    engine.registerFeed("system_stats", pollSystemStats);
    engine.registerFeed("working_directory", pollWorkingDirectory);

    // ── Feed tools ──
    // Feeds without tools keep their old poll-only behavior. Feeds with tools
    // expose BOTH poll (existing) and tool calls. The model uses
    // <action type="feed" name="<feed>.<tool>" .../> to call a tool.

    // working_directory.refresh — forces a re-poll (useful when the model
    // suspects cwd or git state changed mid-turn).
    engine.registerFeedToolSpec("working_directory",
                                {"refresh", "Force a fresh poll of cwd / git state"});
    engine.registerFeedTool("working_directory", "refresh",
                            [](const Json::Value& /*params*/) -> Json::Value {
                                Json::Value r;
                                auto fr = FeedEngine::instance().pollOne("working_directory", true);
                                r["success"] = fr.ok;
                                if (fr.ok) {
                                    r["output"] = fr.summary;
                                    r["data"] = fr.json;
                                } else {
                                    r["error"] = fr.summary;
                                }
                                return r;
                            });

    // system_clock.refresh — same idea, forces a re-poll.
    engine.registerFeedToolSpec("system_clock",
                                {"refresh", "Force a fresh poll of the system clock"});
    engine.registerFeedTool("system_clock", "refresh",
                            [](const Json::Value& /*params*/) -> Json::Value {
                                Json::Value r;
                                auto fr = FeedEngine::instance().pollOne("system_clock", true);
                                r["success"] = fr.ok;
                                if (fr.ok) {
                                    r["output"] = fr.summary;
                                    r["data"] = fr.json;
                                } else {
                                    r["error"] = fr.summary;
                                }
                                return r;
                            });
}

}  // namespace cortex::mk3::feeds
