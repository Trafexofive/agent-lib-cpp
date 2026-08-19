#pragma once
// Composer bang (!cmd / !!cmd) and @path Tab complete. Bounded process::run.

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "src/ui/chat/notification.hpp"
#include "src/ui/model/inkcell_app_model.hpp"
#include "src/utils/process.hpp"

namespace cortex::mk3::ui::chat {

inline std::string trimTrailingWs(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' ||
                          s.back() == '\r'))
        s.pop_back();
    return s;
}

inline std::string trimSubmit(std::string s) { return trimTrailingWs(std::move(s)); }

inline bool runBangCommand(::cortex::mk3::ui::ShellModel& model, const std::string& raw) {
    std::string text = trimSubmit(raw);
    if (text.empty() || text[0] != '!') return false;
    bool insert = text.size() >= 2 && text[1] == '!';
    std::string cmd = insert ? text.substr(2) : text.substr(1);
    while (!cmd.empty() && (cmd.front() == ' ' || cmd.front() == '\t')) cmd.erase(cmd.begin());
    if (cmd.empty()) return false;

    process::Spec spec;
    spec.shell = true;
    spec.command = cmd;
    spec.timeoutMs = 30000;
    spec.maxStdout = 256 * 1024;
    spec.maxStderr = 64 * 1024;
    process::Result pr = process::run(spec);
    std::string out = pr.stdoutText;
    if (!pr.stderrText.empty()) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += pr.stderrText;
    }
    if (pr.timedOut) {
        if (!out.empty() && out.back() != '\n') out += '\n';
        out += "[timed out 30s]";
    }

    TimelineRow row;
    row.kind = TimelineKind::Log;
    row.title = insert ? "!!" : "!";
    row.body = "$ " + cmd + "\n" + out;
    row.ok = pr.success();
    model.pushRow(std::move(row));

    if (insert) {
        model.composer.value = out;
        model.composer.cursor = static_cast<int>(model.composer.value.size());
    } else {
        model.composer.value.clear();
        model.composer.cursor = 0;
        model.composer.scroll_row = 0;
    }
    model.rebuildViews();
    Notification n;
    n.id = "bang";
    n.source = "composer";
    n.severity = pr.success() ? "info" : "warn";
    n.lifetimeMs = 2200;
    n.title = (insert ? "!! " : "! ") + cmd + (pr.timedOut ? " · timeout" : "");
    model.notificationStack.push(std::move(n));
    return true;
}

inline bool completeAtPath(::cortex::mk3::ui::ShellModel& model, bool reverse) {
    const std::string& v = model.composer.value;
    int cur = std::max(0, std::min(model.composer.cursor, static_cast<int>(v.size())));
    int at = -1;
    for (int i = cur - 1; i >= 0; --i) {
        char c = v[static_cast<size_t>(i)];
        if (c == '@') {
            at = i;
            break;
        }
        if (c == ' ' || c == '\n' || c == '\t') break;
    }
    if (at < 0) return false;
    std::string token = v.substr(static_cast<size_t>(at + 1), static_cast<size_t>(cur - at - 1));
    if (token.find("..") != std::string::npos) return true;

    namespace fs = std::filesystem;
    fs::path prefix(token);
    fs::path dir = prefix.has_parent_path() && !prefix.parent_path().empty()
                       ? prefix.parent_path()
                       : fs::path(".");
    std::string stem = prefix.filename().string();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) dir = ".";

    std::vector<std::string> matches;
    for (fs::directory_iterator it(dir, ec); it != fs::directory_iterator() && !ec; ++it) {
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (stem.empty() || name.rfind(stem, 0) == 0) {
            std::string rel = (dir == ".") ? name : (dir / name).generic_string();
            if (it->is_directory(ec)) rel += "/";
            matches.push_back(rel);
        }
        if (matches.size() > 40) break;
    }
    std::sort(matches.begin(), matches.end());
    if (matches.empty()) return true;

    std::string pick;
    if (matches.size() == 1) {
        pick = matches.front();
    } else {
        if (model.tabMatches != matches) {
            model.tabMatches = matches;
            model.tabMatchIndex = reverse ? static_cast<int>(matches.size()) - 1 : 0;
        } else if (reverse) {
            model.tabMatchIndex =
                (model.tabMatchIndex - 1 + static_cast<int>(matches.size())) %
                static_cast<int>(matches.size());
        } else {
            model.tabMatchIndex = (model.tabMatchIndex + 1) % static_cast<int>(matches.size());
        }
        if (model.tabMatchIndex < 0) model.tabMatchIndex = 0;
        pick = matches[static_cast<size_t>(model.tabMatchIndex)];
    }
    std::string out = v.substr(0, static_cast<size_t>(at + 1)) + pick;
    if (cur < static_cast<int>(v.size())) out += v.substr(static_cast<size_t>(cur));
    model.composer.value = out;
    model.composer.cursor = at + 1 + static_cast<int>(pick.size());
    return true;
}

}  // namespace cortex::mk3::ui::chat
