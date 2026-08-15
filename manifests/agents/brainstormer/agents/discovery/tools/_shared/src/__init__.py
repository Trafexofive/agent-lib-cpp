"""Shared support library for the discovery sub-agent's read-only tools.

Everything here is side-effect free: HTTP reads, text/image extraction, and
local analysis. No writes, no state, no mutations — mirroring the discovery
agent's read-only directive.
"""
