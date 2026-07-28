#!/usr/bin/env python3
import json, subprocess, os, sys

repo = os.getenv("REPO_PATH", ".")
try:
    branch = subprocess.check_output(["git", "-C", repo, "branch", "--show-current"], text=True).strip()
    status = subprocess.check_output(["git", "-C", repo, "status", "--porcelain"], text=True).strip()
    ahead_behind = subprocess.check_output(["git", "-C", repo, "rev-list", "--left-right", "--count", "HEAD...@{u}"], text=True, stderr=subprocess.DEVNULL).strip() if subprocess.run(["git", "-C", repo, "rev-parse", "@{u}"], capture_output=True).returncode == 0 else "0\t0"
    ahead, behind = ahead_behind.split("\t")
    
    staged = []
    unstaged = []
    for line in status.split("\n"):
        if not line: continue
        if line[0] in "MADRC":
            staged.append(line[3:])
        if line[1] in "MD":
            unstaged.append(line[3:])
    
    print(json.dumps({
        "repo_path": repo,
        "branch": branch,
        "ahead": int(ahead),
        "behind": int(behind),
        "dirty": len(status) > 0,
        "staged_files": staged,
        "unstaged_files": unstaged
    }))
except Exception as e:
    print(json.dumps({"error": str(e)}))
    sys.exit(1)
