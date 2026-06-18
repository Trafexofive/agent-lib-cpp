// =============================================================================
// agent-lib-MK3 — Built-in feed pollers
// =============================================================================

#include "feed_engine.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <ctime>
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

    // Git detection
    std::string gitCmd = "git -C " + std::string(cwd) + " rev-parse --show-toplevel 2>/dev/null";
    FILE* gp = popen(gitCmd.c_str(), "r");
    if (gp) {
        char gitRoot[4096] = {};
        if (fgets(gitRoot, sizeof(gitRoot), gp)) {
            std::string root(gitRoot);
            if (!root.empty() && root.back() == '\n')
                root.pop_back();
            ss << "\nGit repo: " << root;

            // Branch
            std::string brCmd = "git -C " + root + " rev-parse --abbrev-ref HEAD 2>/dev/null";
            FILE* bp = popen(brCmd.c_str(), "r");
            if (bp) {
                char branch[256] = {};
                if (fgets(branch, sizeof(branch), bp)) {
                    std::string br(branch);
                    if (!br.empty() && br.back() == '\n')
                        br.pop_back();
                    ss << " | Branch: " << br;
                }
                pclose(bp);
            }

            // Dirty
            std::string dirtyCmd = "git -C " + root + " diff --quiet 2>/dev/null; echo $?";
            FILE* dp = popen(dirtyCmd.c_str(), "r");
            if (dp) {
                char d[4] = {};
                if (fgets(d, sizeof(d), dp)) {
                    ss << " | Dirty: " << (d[0] == '0' ? "no" : "yes");
                }
                pclose(dp);
            }

            // Commit
            std::string hashCmd = "git -C " + root + " rev-parse --short HEAD 2>/dev/null";
            FILE* hp = popen(hashCmd.c_str(), "r");
            if (hp) {
                char hash[41] = {};
                if (fgets(hash, sizeof(hash), hp)) {
                    std::string h(hash);
                    if (!h.empty() && h.back() == '\n')
                        h.pop_back();
                    ss << " | Commit: " << h;
                }
                pclose(hp);
            }
        }
        pclose(gp);
    }

    r.summary = ss.str();
    return r;
}

void registerFeeds() {
    auto& engine = FeedEngine::instance();
    engine.registerFeed("system_clock", pollSystemClock);
    engine.registerFeed("system_stats", pollSystemStats);
    engine.registerFeed("working_directory", pollWorkingDirectory);
}

}  // namespace cortex::mk3::feeds
