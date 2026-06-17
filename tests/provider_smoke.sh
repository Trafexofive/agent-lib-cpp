#!/usr/bin/env bash
# =============================================================================
# Provider Smoke Test — opencode-go
# Verifies the API key is set and a simple "hello" completes.
# =============================================================================
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

BASE_URL="https://opencode.ai/zen/go/v1"
MODEL="deepseek-v4-flash"

echo "=== opencode-go smoke test ==="
echo "Base URL: $BASE_URL"
echo "Model:    $MODEL"

# Check API key
if [ -z "${OPENCODE_API_KEY:-}" ]; then
    echo -e "${RED}FAIL: OPENCODE_API_KEY not set${NC}"
    echo "  Get one at https://opencode.ai/auth"
    exit 1
fi
echo -e "${GREEN}OK: OPENCODE_API_KEY found ($(echo ${OPENCODE_API_KEY} | head -c 8)...)${NC}"

# Simple chat completion request
echo ""
echo "--- Sending hello request ---"
RESPONSE=$(curl -s -w "\n%{http_code}" "$BASE_URL/chat/completions" \
    -H "Authorization: Bearer $OPENCODE_API_KEY" \
    -H "Content-Type: application/json" \
    -H "X-Title: Cortex-MK3" \
    -H "HTTP-Referer: https://github.com/Cortex-Prime-MK1" \
    -d '{
        "model": "'"$MODEL"'",
        "messages": [
            {"role": "user", "content": "Say hello in one word"}
        ],
        "max_tokens": 10
    }')

HTTP_CODE=$(echo "$RESPONSE" | tail -1)
BODY=$(echo "$RESPONSE" | sed '$d')

if [ "$HTTP_CODE" != "200" ]; then
    echo -e "${RED}FAIL: HTTP $HTTP_CODE${NC}"
    echo "$BODY" | python3 -m json.tool 2>/dev/null || echo "$BODY"
    exit 1
fi

echo -e "${GREEN}OK: HTTP 200${NC}"

# Extract the response text
TEXT=$(echo "$BODY" | python3 -c "
import sys, json
data = json.load(sys.stdin)
print(data['choices'][0]['message']['content'].strip())
" 2>/dev/null || echo "parse error")

echo "Response: \"$TEXT\""

if [[ "$TEXT" =~ [Hh]ello|[Hh]i|[Hh]ey|[Yy]o ]]; then
    echo -e "${GREEN}PASS: opencode-go API is working${NC}"
else
    echo -e "${YELLOW}WARN: Got response but unexpected format: '$TEXT'${NC}"
fi
