#!/usr/bin/env bash
set -euo pipefail

input=$(cat)

repo_path=$(echo "$input" | jq -r '.repo_path // "."')
mode=$(echo "$input" | jq -r '.mode // "unstaged"')
stat_only=$(echo "$input" | jq -r '.stat_only // false')
no_color=$(echo "$input" | jq -r '.no_color // false')
context_lines=$(echo "$input" | jq -r '.context_lines // 3')
paths=$(echo "$input" | jq -r '.paths // [] | join(" ")')

if [[ ! -d "$repo_path/.git" ]]; then
    echo '{"success":false,"data":{},"error":"not a git repository: '"$repo_path"'"}'
    exit 0
fi

cd "$repo_path"

args=("diff")

case "$mode" in
    unstaged) ;;
    staged|cached) args+=("--cached") ;;
    all) args+=("HEAD") ;;
    *) 
        echo '{"success":false,"data":{},"error":"invalid mode: '"$mode"'"}'
        exit 0
        ;;
esac

if [[ "$stat_only" == "true" ]]; then
    args+=("--stat")
fi

if [[ "$no_color" == "true" ]]; then
    args+=("--no-color")
else
    args+=("--color=never")
fi

args+=("-U$context_lines")

if [[ -n "$paths" ]]; then
    args+=("--")
    args+=($paths)
fi

output=$(git "${args[@]}" 2>&1)
exit_code=$?

if [[ "$stat_only" == "true" ]]; then
    # Parse the last line of git diff --stat which is like:
    #  70 files changed, 1234 insertions(+), 567 deletions(-)
    last_line=$(echo "$output" | tail -1)
    files=0
    insertions=0
    deletions=0
    if [[ $last_line =~ ([0-9]+)[[:space:]]+files?[[:space:]]+changed ]]; then
        files=${BASH_REMATCH[1]}
    fi
    if [[ $last_line =~ ([0-9]+)[[:space:]]+insertions? ]]; then
        insertions=${BASH_REMATCH[1]}
    fi
    if [[ $last_line =~ ([0-9]+)[[:space:]]+deletions? ]]; then
        deletions=${BASH_REMATCH[1]}
    fi
    
    jq -n \
        --argjson success true \
        --arg diff "" \
        --argjson files "$files" \
        --argjson insertions "$insertions" \
        --argjson deletions "$deletions" \
        '{success: $success, data: {diff: $diff, stats: {files_changed: $files, insertions: $insertions, deletions: $deletions}}, error: ""}'
else
    escaped=$(printf '%s' "$output" | jq -Rs .)
    # Get stats from shortstat
    stats=$(git "${args[@]}" --shortstat 2>/dev/null | tail -1)
    files=0
    insertions=0
    deletions=0
    if [[ $stats =~ ([0-9]+)[[:space:]]+files?[[:space:]]+changed ]]; then
        files=${BASH_REMATCH[1]}
    fi
    if [[ $stats =~ ([0-9]+)[[:space:]]+insertions? ]]; then
        insertions=${BASH_REMATCH[1]}
    fi
    if [[ $stats =~ ([0-9]+)[[:space:]]+deletions? ]]; then
        deletions=${BASH_REMATCH[1]}
    fi
    
    jq -n \
        --argjson success true \
        --arg diff "$escaped" \
        --argjson files "$files" \
        --argjson insertions "$insertions" \
        --argjson deletions "$deletions" \
        '{success: $success, data: {diff: $diff, stats: {files_changed: $files, insertions: $insertions, deletions: $deletions}}, error: ""}'
fi
