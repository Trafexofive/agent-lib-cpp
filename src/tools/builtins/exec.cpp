// src/tools/builtins/exec.cpp — exec native builtin
#include "builtins.hpp"
#include "common.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

namespace cortex::mk3::tools::builtins {

struct ExecCapture {
    int exitCode = -1;
    bool timedOut = false;
    bool truncated = false;
    long elapsedMs = 0;
    std::string output;
};

static ExecCapture runShellCommand(const std::string& cmd, const std::string& cwd, int timeoutSec,
                                   size_t maxBytes) {
    ExecCapture result;
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        result.output = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }

    auto start = std::chrono::steady_clock::now();
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            std::string msg = "chdir failed: " + cwd + "\n";
            write(STDERR_FILENO, msg.data(), msg.size());
            _exit(127);
        }
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        result.output = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }

    fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL, 0) | O_NONBLOCK);
    int status = 0;
    bool childDone = false;
    char buf[4096];
    while (!childDone) {
        auto now = std::chrono::steady_clock::now();
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (result.elapsedMs > timeoutSec * 1000L) {
            kill(pid, SIGTERM);
            usleep(100000);
            if (waitpid(pid, &status, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            result.timedOut = true;
            childDone = true;
            break;
        }

        fd_set set;
        FD_ZERO(&set);
        FD_SET(pipefd[0], &set);
        timeval tv{0, 50000};
        int ready = select(pipefd[0] + 1, &set, nullptr, nullptr, &tv);
        if (ready > 0 && FD_ISSET(pipefd[0], &set)) {
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                size_t remaining = maxBytes > result.output.size() ? maxBytes - result.output.size() : 0;
                size_t take = std::min<size_t>(remaining, static_cast<size_t>(n));
                result.output.append(buf, take);
                if (take < static_cast<size_t>(n))
                    result.truncated = true;
            }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
            childDone = true;
    }

    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        size_t remaining = maxBytes > result.output.size() ? maxBytes - result.output.size() : 0;
        size_t take = std::min<size_t>(remaining, static_cast<size_t>(n));
        result.output.append(buf, take);
        if (take < static_cast<size_t>(n))
            result.truncated = true;
    }
    close(pipefd[0]);
    if (!result.timedOut)
        waitpid(pid, &status, 0);
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    if (result.timedOut)
        result.exitCode = 124;
    else if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exitCode = 128 + WTERMSIG(status);
    return result;
}

std::string exec(const Json::Value& p) {
    std::string cmd = p.get("command", p.get("cmd", p.get("input", "").asString()).asString()).asString();
    if (cmd.empty())
        return jsonErr("command is required");
    std::string cwd = p.get("cwd", "").asString();
    int timeout = std::clamp(p.get("timeout", 30).asInt(), 1, 600);
    size_t maxBytes = static_cast<size_t>(std::clamp(p.get("max_bytes", 512 * 1024).asInt(), 1, 10 * 1024 * 1024));

    ExecCapture cap = runShellCommand(cmd, cwd, timeout, maxBytes);
    Json::Value r;
    r["success"] = !cap.timedOut;
    r["exit_code"] = cap.exitCode;
    r["timed_out"] = cap.timedOut;
    r["elapsed_ms"] = static_cast<Json::Int64>(cap.elapsedMs);
    r["truncated"] = cap.truncated;
    r["bytes"] = static_cast<Json::UInt64>(cap.output.size());
    r["output"] = cap.output;
    return jsonStr(r);
}

}  // namespace cortex::mk3::tools::builtins
