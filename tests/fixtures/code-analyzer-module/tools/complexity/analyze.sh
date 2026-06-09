#!/bin/bash
# Complexity analyzer — counts branching constructs per function
file="${1:-$path}"
if [ -z "$file" ]; then
  echo '{"error":"path required"}'
  exit 1
fi
if [ ! -f "$file" ]; then
  echo '{"error":"file not found: '"$file"'"}'
  exit 1
fi
# Count functions and their branching complexity
total=$(grep -cE '^\s*(void|int|bool|auto|std::|string|size_t|Json::|virtual\s+)?[A-Za-z_][A-Za-z0-9_:]*\s*\([^)]*\)\s*(const\s*)?\{' "$file" 2>/dev/null || echo 0)
max_score=0
hotspots="[]"
# Find the most complex function (most if/for/while/switch/case/&&/||)
while IFS= read -r func; do
  body=$(sed -n "/$func/,/^}/p" "$file" 2>/dev/null)
  score=$(echo "$body" | grep -cE '\b(if|for|while|switch|case|catch)\b' 2>/dev/null || echo 0)
  if [ "$score" -gt "$max_score" ]; then
    max_score=$score
  fi
done < <(grep -oP '^\s*(?:virtual\s+)?(?:\w+(?:::))*(\w+)\s*\([^)]*\)\s*(?:const\s*)?\{' "$file" | grep -oP '\w+(?=\s*\()' | head -50)
avg=$( [ "$total" -gt 0 ] && echo "scale=1; $max_score / $total" | bc 2>/dev/null || echo "0.0" )
echo "{\"file\":\"$file\",\"total_functions\":$total,\"max_complexity\":$max_score,\"average_complexity\":$avg,\"hotspots\":$hotspots}"
