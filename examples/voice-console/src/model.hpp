#pragma once
// voice-console model: pipeline state (JSON from voice.py) + a headless
// harness subprocess (cortex-mk3 --ephemeral --no-ansi) that streams its
// rendered blocks back for the TUI. No product domain in inkcell — this
// lives in the example app.

#include <json/json.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace vc {

struct AppConfig {
    std::string bin;        // cortex-mk3 binary path
    std::string manifest;   // voice agent manifest path
    std::string state_path; // voice.py state JSON path
};

// Snapshot of the voice pipeline, parsed from voice.py --state-out JSON.
struct VoiceState {
    std::string stage = "idle";
    std::string transcript;
    std::string response;
    std::string wake_word;
    double ts = 0.0;
    float level = 0.0f;          // decaying mic peak, 0..1
    float wake_peak = 0.0f;      // highest wake confidence seen
    float wake_threshold = 0.4f; // trigger threshold
    std::string wake_model;      // best wake model name
    std::map<std::string, int> latency;  // stage -> ms (sequential deltas)

    // Returns false on any read/parse error (caller treats as "offline").
    bool load(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if (body.empty())
            return false;

        Json::Value root;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream ss(body);
        if (!Json::parseFromStream(rb, ss, &root, &errs))
            return false;

        stage = root.get("stage", "idle").asString();
        transcript = root.get("transcript", "").asString();
        response = root.get("response", "").asString();
        wake_word = root.get("wake_word", "").asString();
        ts = root.get("ts", 0.0).asDouble();
        level = (float)root.get("level", 0.0).asDouble();
        wake_peak = (float)root.get("wake_peak", 0.0).asDouble();
        wake_model = root.get("wake_model", "").asString();
        wake_threshold = (float)root.get("wake_threshold", 0.4).asDouble();
        latency.clear();
        const Json::Value& lat = root["latency_ms"];
        if (lat.isObject()) {
            for (const auto& k : lat.getMemberNames())
                latency[k] = lat[k].asInt();
        }
        return true;
    }
};

// One headless harness turn. fork/exec (argv, no shell — injection-safe),
// stdout+stderr piped back and collected into a bounded line buffer.
class HarnessRun {
   public:
    ~HarnessRun() { stop(); }

    void start(const AppConfig& cfg, const std::string& prompt) {
        if (running())
            return;
        int fds[2];
        if (pipe(fds) != 0)
            return;
        pid_ = fork();
        if (pid_ == 0) {
            // Child: exec harness, not the shell. argv array avoids prompt
            // injection via shell metacharacters.
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            ::close(fds[0]);
            ::close(fds[1]);
            execl(cfg.bin.c_str(), "cortex-mk3", "-m", cfg.manifest.c_str(), "run",
                  "-p", prompt.c_str(), "--ephemeral", "--no-ansi", (char*)nullptr);
            _exit(127);
        }
        if (pid_ < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return;
        }
        ::close(fds[1]);
        FILE* f = fdopen(fds[0], "r");
        {
            std::lock_guard<std::mutex> lk(mu_);
            lines_.clear();
            running_ = true;
            prompt_ = prompt;
        }
        th_ = std::thread([this, f] {
            char buf[4096];
            std::string carry;
            while (fgets(buf, (int)sizeof(buf), f)) {
                carry += buf;
                std::size_t nl;
                while ((nl = carry.find('\n')) != std::string::npos) {
                    push_line(carry.substr(0, nl));
                    carry.erase(0, nl + 1);
                }
            }
            if (!carry.empty())
                push_line(carry);
            fclose(f);
            { std::lock_guard<std::mutex> lk(mu_); running_ = false; }
        });
    }

    bool running() const {
        std::lock_guard<std::mutex> lk(mu_);
        return running_;
    }

    std::string prompt() const {
        std::lock_guard<std::mutex> lk(mu_);
        return prompt_;
    }

    // Snapshot the current buffered lines (for one frame).
    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lk(mu_);
        return std::vector<std::string>(lines_.begin(), lines_.end());
    }

    void stop() {
        std::thread t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::swap(t, th_);
        }
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            ::waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
        if (t.joinable())
            t.join();
    }

   private:
    void push_line(const std::string& raw) {
        // Strip ANSI (some CLI notices still carry escapes despite --no-ansi).
        std::string l;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\x1b' && i + 1 < raw.size() && raw[i + 1] == '[') {
                i += 2;
                while (i < raw.size() && !((raw[i] >= 'A' && raw[i] <= 'Z') ||
                                            (raw[i] >= 'a' && raw[i] <= 'z')))
                    ++i;
                continue;
            }
            l += raw[i];
        }
        // Drop CLI status noise ([manifest] / [sandbox] / [dev_mode] …); keep
        // only the conversation/protocol blocks the console cares about.
        if (l.rfind("[", 0) == 0) {
            const bool keep = l.rfind("[thought]", 0) == 0 ||
                              l.rfind("[response]", 0) == 0 ||
                              l.rfind("[action", 0) == 0 ||
                              l.rfind("[result", 0) == 0;
            if (!keep)
                return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (!lines_.empty() || !l.empty())  // drop a leading empty line
            lines_.push_back(l);
        while (lines_.size() > 400)
            lines_.pop_front();
    }

    mutable std::mutex mu_;
    std::thread th_;
    std::deque<std::string> lines_;
    std::string prompt_;
    bool running_ = false;
    pid_t pid_ = -1;
};

}  // namespace vc
