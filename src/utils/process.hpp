// src/utils/process.hpp — shared bounded process runner
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cortex::mk3::process {

struct Spec {
    bool shell = false;
    std::string command;
    std::vector<std::string> argv;
    std::string cwd;
    std::string stdinText;
    std::map<std::string, std::string> env;
    bool clearEnv = false;
    int timeoutMs = 30000;
    size_t maxStdout = 512 * 1024;
    size_t maxStderr = 512 * 1024;
    // Called as output arrives. bool=true means stderr, false means stdout.
    std::function<void(const char* data, size_t size, bool stderrStream)> onOutput;
};

struct Result {
    int exitCode = -1;
    int termSignal = 0;
    bool timedOut = false;
    bool stdoutTruncated = false;
    bool stderrTruncated = false;
    long elapsedMs = 0;
    std::string stdoutText;
    std::string stderrText;

    bool success() const { return !timedOut && exitCode == 0; }
};

Result run(const Spec& spec);

}  // namespace cortex::mk3::process
