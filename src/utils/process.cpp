// src/utils/process.cpp — shared bounded process runner
#include "process.hpp"

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

extern char** environ;

namespace cortex::mk3::process {

static bool setNonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void appendBounded(std::string& dst, const char* data, size_t n, size_t maxBytes,
                          bool& truncated) {
    size_t remaining = maxBytes > dst.size() ? maxBytes - dst.size() : 0;
    size_t take = std::min(remaining, n);
    dst.append(data, take);
    if (take < n)
        truncated = true;
}

static std::vector<char*> argvPointers(std::vector<std::string>& args) {
    std::vector<char*> out;
    out.reserve(args.size() + 1);
    for (auto& s : args)
        out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

static std::vector<std::string> buildEnv(const Spec& spec) {
    std::vector<std::string> env;
    if (!spec.clearEnv) {
        for (char** e = environ; e && *e; ++e)
            env.emplace_back(*e);
    }
    for (const auto& [k, v] : spec.env) {
        std::string prefix = k + "=";
        env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& item) {
                      return item.rfind(prefix, 0) == 0;
                  }),
                  env.end());
        env.push_back(prefix + v);
    }
    return env;
}

Result run(const Spec& spec) {
    Result result;
    int outPipe[2], errPipe[2], inPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0) {
        result.stderrText = std::string("pipe failed: ") + std::strerror(errno);
        return result;
    }

    auto start = std::chrono::steady_clock::now();
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        close(outPipe[0]);
        close(errPipe[0]);
        close(inPipe[1]);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        dup2(inPipe[0], STDIN_FILENO);
        close(outPipe[1]);
        close(errPipe[1]);
        close(inPipe[0]);

        if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0) {
            std::string msg = "chdir failed: " + spec.cwd + "\n";
            write(STDERR_FILENO, msg.data(), msg.size());
            _exit(127);
        }

        std::vector<std::string> envStrings = buildEnv(spec);
        std::vector<char*> envp = argvPointers(envStrings);
        if (spec.shell) {
            std::vector<std::string> args = {"sh", "-c", spec.command};
            std::vector<char*> argv = argvPointers(args);
            execve("/bin/sh", argv.data(), envp.data());
        } else {
            if (spec.argv.empty())
                _exit(127);
            std::vector<std::string> args = spec.argv;
            std::vector<char*> argv = argvPointers(args);
            execvpe(argv[0], argv.data(), envp.data());
        }
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);
    close(inPipe[0]);
    if (pid < 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        close(inPipe[1]);
        result.stderrText = std::string("fork failed: ") + std::strerror(errno);
        return result;
    }
    setpgid(pid, pid);
    setNonblocking(outPipe[0]);
    setNonblocking(errPipe[0]);
    setNonblocking(inPipe[1]);

    size_t stdinWritten = 0;
    bool stdinClosed = false;
    int status = 0;
    bool childDone = false;
    char buf[4096];

    while (!childDone) {
        auto now = std::chrono::steady_clock::now();
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (result.elapsedMs > spec.timeoutMs) {
            kill(-pid, SIGTERM);
            usleep(100000);
            if (waitpid(pid, &status, WNOHANG) == 0) {
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            result.timedOut = true;
            childDone = true;
            break;
        }

        fd_set readSet, writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        FD_SET(outPipe[0], &readSet);
        FD_SET(errPipe[0], &readSet);
        int maxFd = std::max(outPipe[0], errPipe[0]);
        if (!stdinClosed) {
            FD_SET(inPipe[1], &writeSet);
            maxFd = std::max(maxFd, inPipe[1]);
        }
        timeval tv{0, 50000};
        int ready = select(maxFd + 1, &readSet, &writeSet, nullptr, &tv);
        if (ready > 0) {
            if (FD_ISSET(outPipe[0], &readSet)) {
                ssize_t n;
                while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
                    appendBounded(result.stdoutText, buf, static_cast<size_t>(n), spec.maxStdout,
                                  result.stdoutTruncated);
                    if (spec.onOutput)
                        spec.onOutput(buf, static_cast<size_t>(n), false);
                }
            }
            if (FD_ISSET(errPipe[0], &readSet)) {
                ssize_t n;
                while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) {
                    appendBounded(result.stderrText, buf, static_cast<size_t>(n), spec.maxStderr,
                                  result.stderrTruncated);
                    if (spec.onOutput)
                        spec.onOutput(buf, static_cast<size_t>(n), true);
                }
            }
            if (!stdinClosed && FD_ISSET(inPipe[1], &writeSet)) {
                const std::string& in = spec.stdinText;
                if (stdinWritten >= in.size()) {
                    close(inPipe[1]);
                    stdinClosed = true;
                } else {
                    ssize_t n = write(inPipe[1], in.data() + stdinWritten, in.size() - stdinWritten);
                    if (n > 0)
                        stdinWritten += static_cast<size_t>(n);
                    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        close(inPipe[1]);
                        stdinClosed = true;
                    }
                }
            }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
            childDone = true;
    }

    if (!stdinClosed)
        close(inPipe[1]);
    ssize_t n;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
        appendBounded(result.stdoutText, buf, static_cast<size_t>(n), spec.maxStdout,
                      result.stdoutTruncated);
        if (spec.onOutput)
            spec.onOutput(buf, static_cast<size_t>(n), false);
    }
    while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) {
        appendBounded(result.stderrText, buf, static_cast<size_t>(n), spec.maxStderr,
                      result.stderrTruncated);
        if (spec.onOutput)
            spec.onOutput(buf, static_cast<size_t>(n), true);
    }
    close(outPipe[0]);
    close(errPipe[0]);

    if (!result.timedOut)
        waitpid(pid, &status, 0);
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    if (result.timedOut)
        result.exitCode = 124;
    else if (WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
        result.termSignal = WTERMSIG(status);
        result.exitCode = 128 + result.termSignal;
    }
    return result;
}

}  // namespace cortex::mk3::process
