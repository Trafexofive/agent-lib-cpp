#!/bin/bash
dir="${1:-$path}"
if [ -z "$dir" ]; then echo '{"error":"path required"}'; exit 1; fi
src=$(find "$dir" -name '*.cpp' -not -path '*/test/*' -not -path '*/tests/*' -not -path '*/testing/*' 2>/dev/null | wc -l)
test=$(find "$dir" -name '*test*' -o -name '*spec*' 2>/dev/null | wc -l)
pct=$( [ "$src" -gt 0 ] && echo "scale=1; $test * 100 / ($src + $test)" | bc 2>/dev/null || echo "0" )
echo "{\"coverage_percent\":$pct,\"tested_files\":0,\"untested_files\":0,\"total_source_files\":$src}"
