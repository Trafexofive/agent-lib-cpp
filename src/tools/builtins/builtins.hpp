// src/tools/builtins/builtins.hpp — aggregate of all native builtin entry points
// Prefer including the per-builtin header from implementation files.
// This aggregate exists for registration sites that need the full surface.
#pragma once

#include "exec.hpp"
#include "list.hpp"
#include "grep.hpp"
#include "fs_read.hpp"
#include "fs_write.hpp"
#include "json.hpp"
#include "web_fetch.hpp"
#include "ask_tool.hpp"
#include "sleep.hpp"
#include "artifact.hpp"

