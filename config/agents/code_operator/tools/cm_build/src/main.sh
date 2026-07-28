#!/usr/bin/env bash
set -euo pipefail

# Read JSON input from stdin
input=$(cat)

# Parse input with jq
path=$(echo "$input" | jq -r '.path // "."')
build_dir=$(echo "$input" | jq -r '.build_dir // "build"')
build_type=$(echo "$input" | jq -r '.build_type // "Release"')
generator=$(echo "$input" | jq -r '.generator // "Ninja"')
targets=$(echo "$input" | jq -r '.targets // [] | join(" ")')
jobs=$(echo "$input" | jq -r '.jobs // 0')
toolchain=$(echo "$input" | jq -r '.toolchain // ""')
cmake_args=$(echo "$input" | jq -r '.cmake_args // [] | join(" ")')
clean=$(echo "$input" | jq -r '.clean // false')

# Resolve absolute paths
project_root=$(realpath "$path")
build_path="${project_root}/${build_dir}"

# Clean if requested
if [[ "$clean" == "true" && -d "$build_path" ]]; then
    rm -rf "$build_path"
fi

# Configure
mkdir -p "$build_path"
cd "$build_path"

cmake_cmd=("cmake" "-G" "$generator" "-DCMAKE_BUILD_TYPE=$build_type" "$project_root")

if [[ -n "$toolchain" ]]; then
    cmake_cmd+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain")
fi

if [[ -n "$cmake_args" ]]; then
    cmake_cmd+=($cmake_args)
fi

# Run configure
configure_start=$(date +%s)
if ! "${cmake_cmd[@]}" 2>&1; then
    echo '{"success":false,"data":{},"error":"CMake configure failed"}'
    exit 0
fi
configure_end=$(date +%s)

# Build
build_cmd=("cmake" "--build" ".")
if [[ -n "$targets" ]]; then
    for target in $targets; do
        build_cmd+=("--target" "$target")
    done
fi
if [[ "$jobs" -gt 0 ]]; then
    build_cmd+=("-j" "$jobs")
fi

build_start=$(date +%s)
if ! "${build_cmd[@]}" 2>&1; then
    echo '{"success":false,"data":{},"error":"Build failed"}'
    exit 0
fi
build_end=$(date +%s)

# Collect artifacts
artifacts=()
while IFS= read -r -d '' file; do
    if [[ -f "$file" && -x "$file" ]]; then
        type="executable"
    elif [[ "$file" == *.so ]] || [[ "$file" == *.dylib ]] || [[ "$file" == *.dll ]]; then
        type="shared_library"
    elif [[ "$file" == *.a ]] || [[ "$file" == *.lib ]]; then
        type="static_library"
    elif [[ "$file" == *.h ]] || [[ "$file" == *.hpp ]]; then
        type="header"
    else
        type="other"
    fi
    size=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file" 2>/dev/null || echo 0)
    rel_path="${file#$build_path/}"
    artifacts+=("{\"path\":\"$rel_path\",\"size_bytes\":$size,\"type\":\"$type\"}")
done < <(find "$build_path" -type f \( -name "*.so" -o -name "*.dylib" -o -name "*.dll" -o -name "*.a" -o -name "*.lib" -o -executable \) -print0 2>/dev/null)

# Compile commands
compile_commands=""
if [[ -f "$build_path/compile_commands.json" ]]; then
    compile_commands="$build_path/compile_commands.json"
fi

# Build JSON output
artifacts_json=$(IFS=,; echo "[${artifacts[*]}]")
total_duration=$((build_end - configure_start))

jq -n \
    --argjson success true \
    --arg build_dir "$build_path" \
    --argjson artifacts "$artifacts_json" \
    --arg compile_commands "$compile_commands" \
    --argjson duration_seconds "$total_duration" \
    '{success: $success, data: {build_dir: $build_dir, artifacts: $artifacts, compile_commands: $compile_commands, duration_seconds: $duration_seconds}, error: ""}'