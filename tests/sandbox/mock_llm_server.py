#!/usr/bin/env python3
"""Mock LLM server for cortex-mk3 harness testing.
Returns deterministic XML protocol responses. No API calls, no cost.

Usage:
  ./mock_llm_server.py [--port 9090]

The server speaks OpenAI-compatible chat completions API at /v1/chat/completions
and determines the response by matching keywords in the user prompt.
"""

import json, sys, time, re
from http.server import HTTPServer, BaseHTTPRequestHandler

PORT = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[1] == '--port' else 9090

# ── Response templates ────────────────────────────────────────────

def think(text):
    return f"<thought>\n{text}\n</thought>"

def action(tool_type, name, id_, params):
    return f'<action type="{tool_type}" name="{name}" id="{id_}" mode="sync">{json.dumps(params)}</action>'

def response(text):
    return f'<response final="true">{text}</response>'

def tool_result(id_, status, data):
    return json.dumps({"id": id_, "status": status, "result": data})

# ── Scenario responses ────────────────────────────────────────────

RESPONSES = {
    # 1. Protocol smoke test
    "identity": [
        think("The user asked who I am. I should respond with my identity."),
        response("I am Cortex MK3, an AI agent using the XML action/response protocol. I communicate via action tags for tool calls and response tags for final answers.")
    ],
    "list files": [
        think("User wants a directory listing. Let me use the list tool."),
        action("tool", "list", "ls1", {"path": "."})
    ],
    "simple math": [
        think("The user asked what 7 times 9 is. I can answer directly."),
        response("63")
    ],

    # 2. Multi-turn tool orchestration  
    "read and analyze": [
        think("Step 1: List the directory structure."),
        action("tool", "list", "m1", {"path": "."})
    ],
    "m1_ok": [
        think("Directory listed. Now read the key file."),
        action("tool", "fs_read", "m2", {"path": "README.md", "limit": 10})
    ],
    "m2_ok": [
        think("README read. Now search for TODOs."),
        action("tool", "grep", "m3", {"pattern": "TODO", "path": "."})
    ],
    "m3_ok": [
        think("Got TODO results. Let me compose the analysis."),
        response("Project analysis: README describes Cortex MK3. Found 3 TODOs in the codebase. The project structure includes src/, config/, and tests/ directories.")
    ],

    # 3. Sub-agent spawn + collect
    "build the project": [
        think("This is a build task. Let me delegate to the builder agent."),
        action("agent", "builder", "b1", "async")
    ],
    "b1_ok": [
        think("Builder finished successfully."),
        response("Build completed successfully. All modules compiled without errors.")
    ],
    "b1_error": [
        think("Builder failed. Let me try a different approach."),
        action("agent", "builder", "b2", "async")
    ],

    # 4. Error recovery
    "read.*nonexistent|read.*ghost|config\\.json": [
        think("Let me try to read the requested file."),
        action("tool", "fs_read", "e1", {"path": "config.json" if "config.json" in sys.argv[-1] else "/nonexistent/path/ghost.txt"})
    ],
    "e1_error": [
        think("File not found. Let me list the directory to find alternatives."),
        action("tool", "list", "e2", {"path": "."})
    ],
    "e2_ok": [
        think("Found alternative files. Let me read one."),
        action("tool", "fs_read", "e3", {"path": "config.yml"})
    ],
    "e3_ok": [
        response("The requested file was not found. However, I found config.yml which contains the project configuration.")
    ],
    "bad.*command|invalid_command": [
        think("Let me try running the command."),
        action("tool", "exec", "e4", {"command": "invalid_command_xyz --flag"})
    ],
    "e4_error": [
        think("Command failed as expected. Let me report the error."),
        response("The command 'invalid_command_xyz --flag' failed with exit code 127: command not found.")
    ],
    "sleep.*timeout|timeout": [
        think("Running sleep with timeout."),
        action("tool", "exec", "e5", {"command": "sleep 10", "timeout": 2})
    ],
    "e5_timeout": [
        think("Command timed out. Let me report."),
        response("The 'sleep 10' command timed out after 2 seconds. This is expected behavior for timeout handling.")
    ],
    "empty.*params|no parameters": [
        think("Let me call the list tool with default parameters."),
        action("tool", "list", "e6", {"path": "."})
    ],

    # 5. Feed polling
    "ci.*status|ci.*failure|feed": [
        think("Let me poll the CI status feed."),
        action("feed", "ci-status", "f1", {"limit": 5})
    ],
    "f1_ok": [
        think("Got CI results. Let me analyze them."),
        response("CI status: 3 jobs passed (lint, build, test-unit), 1 failed (test-integration), 1 skipped (deploy). The integration test failure is the issue.")
    ],

    # 6. Relic integration
    "database.*reachable|relic|postgres": [
        think("Let me query the database relic."),
        action("relic", "postgres_main", "r1", {"query": "SELECT 1 AS ok, version() AS db_version"})
    ],
    "r1_ok": [
        think("Database responded. Let me report the status."),
        response("Database is reachable. PostgreSQL 16.2 is running. Connection test returned 1.")
    ],

    # 7. Stress: rapid turn cycling (multi-turn)
    "analyze this project": [
        think("Full project analysis requested. Starting with listing."),
        action("tool", "list", "s1", {"path": "."})
    ],
    "s1_ok": [
        think("Got directory listing. Now reading README."),
        action("tool", "fs_read", "s2", {"path": "README.md", "limit": 20})
    ],
    "s2_ok": [
        think("README read. Now searching for TODOs."),
        action("tool", "grep", "s3", {"pattern": "TODO", "path": "src/"})
    ],
    "s3_ok": [
        think("Got TODOs. Reading the first TODO file."),
        action("tool", "fs_read", "s4", {"path": "src/core/agent.hpp", "limit": 30})
    ],
    "s4_ok": [
        think("Got agent.hpp. Now pinning it to context."),
        action("tool", "context_pin", "s5", {"path": "src/core/agent.hpp"})
    ],
    "s5_ok": [
        think("File pinned. Now reading config to complete the analysis."),
        action("tool", "fs_read", "s6", {"path": "config/agents/assistant/config.yml"})
    ],
    "s6_ok": [
        think("Full picture gathered. Composing summary."),
        response("Project Analysis: Cortex MK3 agent library. Key files: agent.hpp (16KB), config.yml (assistant config). Found 3 TODOs in src/. README describes protocol and build instructions. Project compiles with g++ and depends on libcurl, jsoncpp.")
    ],
}

# ── Default fallback ──────────────────────────────────────────────

FALLBACK_RESPONSE = [
    think("Let me respond to the user's request."),
    response("I'm a Cortex MK3 agent. I can help with files, commands, searches, and analysis. What would you like me to do?")
]

# ── Multi-turn state ──────────────────────────────────────────────

# Track which tool was last called so we can return the matching result
last_action_id = None


def match_prompt(prompt):
    """Find the best matching response template for a prompt."""
    prompt_lower = prompt.lower()

    # Check for result continuation (tool results inline in history)
    global last_action_id
    for key, template in RESPONSES.items():
        if key.endswith("_ok") or key.endswith("_error") or key.endswith("_timeout"):
            continue  # Skip result templates for direct matching

    for key, template in RESPONSES.items():
        # Check if this is a continuation of a previous action
        if last_action_id and key.startswith(last_action_id + "_"):
            return RESPONSES[key]

    # Match keywords
    for key, template in RESPONSES.items():
        if key in prompt_lower:
            return template

    # Regex matching
    for key, template in RESPONSES.items():
        try:
            if re.search(key, prompt_lower):
                return template
        except re.error:
            pass

    return FALLBACK_RESPONSE


def extract_action_id(response_text):
    """Extract the action ID from an XML response."""
    match = re.search(r'id="([^"]+)"', response_text)
    return match.group(1) if match else None


class MockLLMHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != '/v1/chat/completions':
            self.send_error(404)
            return

        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)
        request = json.loads(body)

        messages = request.get('messages', [])
        # Get the last user message
        user_prompt = ""
        for msg in reversed(messages):
            if msg.get('role') == 'user':
                user_prompt = msg.get('content', '')
                break

        # Find matching template
        template = match_prompt(user_prompt)

        # If this is a new action (not a continuation), set up state
        global last_action_id
        for msg in reversed(messages):
            if msg.get('role') == 'assistant' and '<action ' in msg.get('content', ''):
                aid = extract_action_id(msg['content'])
                if aid:
                    last_action_id = aid
                break

        # Build response
        full_response = '\n'.join(template)

        # Track action ID for next turn
        new_aid = extract_action_id(full_response)
        if new_aid:
            last_action_id = new_aid

        # OpenAI-compatible response
        response_body = {
            "id": f"mock-{int(time.time()*1000)}",
            "object": "chat.completion",
            "created": int(time.time()),
            "model": "mock-harness",
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": full_response
                },
                "finish_reason": "stop"
            }],
            "usage": {
                "prompt_tokens": len(user_prompt) // 4,
                "completion_tokens": len(full_response) // 4,
                "total_tokens": (len(user_prompt) + len(full_response)) // 4
            }
        }

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(response_body).encode())

        # Log
        prompt_preview = user_prompt[:80].replace('\n', ' ')
        resp_preview = full_response[:80].replace('\n', ' ')
        print(f"[mock] prompt='{prompt_preview}...' → response='{resp_preview}...'", flush=True)

    def do_GET(self):
        if self.path == '/health':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(b'{"status":"ok","service":"mock-llm"}')
        else:
            self.send_error(404)

    def log_message(self, format, *args):
        pass  # Suppress default logging


def main():
    server = HTTPServer(('0.0.0.0', PORT), MockLLMHandler)
    print(f"=== Mock LLM Server ===\n  port: {PORT}\n  endpoint: http://0.0.0.0:{PORT}/v1/chat/completions\n  health:  http://0.0.0.0:{PORT}/health")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == '__main__':
    main()
