#!/usr/bin/env python3
"""
Cortex-Prime Python Example (via cffi)

This demonstrates the planned Python binding approach.
For now, this is a reference example — actual cffi bindings
will be in the cortex-py package.

Usage: LD_LIBRARY_PATH=.. python3 python_agent.py
"""

import ctypes
import os
import sys

# Load the shared library
lib_path = os.path.join(os.path.dirname(__file__), '..', 'libagent-mk2.so')
try:
    lib = ctypes.CDLL(lib_path)
except OSError:
    print(f"Failed to load {lib_path}")
    print("Build with: make lib")
    sys.exit(1)

# ─── Type definitions ───────────────────────────────────────────────────

class CortexError(ctypes.Structure):
    _fields_ = [
        ("code", ctypes.c_int),
        ("message", ctypes.c_char_p),
    ]

class CortexServerConfig(ctypes.Structure):
    _fields_ = [
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_int),
        ("threads", ctypes.c_int),
        ("api_key", ctypes.c_char_p),
        ("cors", ctypes.c_int),
    ]

# Opaque handles
cortex_agent_t = ctypes.c_void_p
cortex_server_t = ctypes.c_void_p

# Callback type: int (*cortex_token_cb)(const char* token, size_t len, void* user_data)
TOKEN_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p)

# ─── Function signatures ────────────────────────────────────────────────

lib.cortex_version.restype = ctypes.c_char_p
lib.cortex_version.argtypes = []

lib.cortex_agent_create.restype = cortex_agent_t
lib.cortex_agent_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(CortexError)]

lib.cortex_agent_destroy.restype = None
lib.cortex_agent_destroy.argtypes = [cortex_agent_t]

lib.cortex_agent_prompt.restype = ctypes.c_char_p  # caller must free
lib.cortex_agent_prompt.argtypes = [cortex_agent_t, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(CortexError)]

lib.cortex_agent_prompt_stream.restype = ctypes.c_char_p
lib.cortex_agent_prompt_stream.argtypes = [cortex_agent_t, ctypes.c_char_p, TOKEN_CB, ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(CortexError)]

lib.cortex_agent_interrupt.restype = None
lib.cortex_agent_interrupt.argtypes = [cortex_agent_t]

lib.cortex_agent_set_env.restype = ctypes.c_int
lib.cortex_agent_set_env.argtypes = [cortex_agent_t, ctypes.c_char_p, ctypes.c_char_p]

lib.cortex_agent_get_env.restype = ctypes.c_char_p
lib.cortex_agent_get_env.argtypes = [cortex_agent_t, ctypes.c_char_p]

lib.cortex_agent_list_tools.restype = ctypes.c_int
lib.cortex_agent_list_tools.argtypes = [cortex_agent_t, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]

lib.cortex_agent_clear_history.restype = None
lib.cortex_agent_clear_history.argtypes = [cortex_agent_t]

lib.cortex_agent_dispatch_tool.restype = ctypes.c_int
lib.cortex_agent_dispatch_tool.argtypes = [cortex_agent_t, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.POINTER(CortexError)]

lib.cortex_free.restype = None
lib.cortex_free.argtypes = [ctypes.c_void_p]

# ─── Streaming callback ─────────────────────────────────────────────────

@TOKEN_CB
def on_token(token, length, user_data):
    """Print each token as it streams from the LLM."""
    text = token[:length].decode('utf-8', errors='replace')
    print(text, end='', flush=True)
    return 0  # 0 = continue

# ─── Main ────────────────────────────────────────────────────────────────

def main():
    prompt = sys.argv[1] if len(sys.argv) > 1 else "What is 2+2?"
    manifest = b"config/agents/assistant/agent.yml"

    print(f"Cortex-Prime v{lib.cortex_version().decode()}")

    # Create agent
    error = CortexError()
    agent = lib.cortex_agent_create(manifest, None, None, ctypes.byref(error))
    if not agent:
        print(f"Failed to create agent: [{error.code}] {error.message.decode()}")
        return 1

    # List tools
    tool_names = (ctypes.c_char_p * 32)()
    count = lib.cortex_agent_list_tools(agent, tool_names, 32)
    tools = [tool_names[i].decode() for i in range(min(count, 32))]
    print(f"\nTools ({count}): {' '.join(tools)}\n")

    # Prompt with streaming
    print("Response: ", end='', flush=True)
    error = CortexError()
    result = lib.cortex_agent_prompt_stream(
        agent, prompt.encode(), on_token, None, b"py_session", ctypes.byref(error)
    )
    print("\n")

    if result:
        lib.cortex_free(result)

    # Direct tool dispatch
    tool_result = ctypes.c_char_p()
    error = CortexError()
    err = lib.cortex_agent_dispatch_tool(
        agent, b"list", b'{"path":"."}', ctypes.byref(tool_result), ctypes.byref(error)
    )
    if err == 0 and tool_result.value:
        print(f"Direct tool call: {tool_result.value.decode()[:200]}...")
        lib.cortex_free(tool_result)

    # Cleanup
    lib.cortex_agent_destroy(agent)
    print("\nDone.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
