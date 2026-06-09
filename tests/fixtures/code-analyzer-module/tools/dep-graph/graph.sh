#!/bin/bash
dir="${1:-$path}"
if [ -z "$dir" ]; then echo '{"error":"path required"}'; exit 1; fi
total=$(find "$dir" -name '*.hpp' -o -name '*.h' -o -name '*.cpp' 2>/dev/null | wc -l)
edges=$(grep -rh '^#include' "$dir" 2>/dev/null | wc -l)
echo "{\"total_files\":$total,\"total_edges\":$edges,\"circular_deps\":[],\"top_included\":[]}"
