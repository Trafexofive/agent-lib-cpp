#pragma once
// Thin façade: re-exports the inkcell app surface for the CLI and any other
// consumer. The real modules — boot, assembly, agent turn, launcher, runtime,
// repl — live as focused headers in src/ui/app/. Consumers include this for
// the ui:: entry points (runInkcellRepl / runInkcellOneShot / …).

#include "src/ui/app/agent_launcher.hpp"
#include "src/ui/app/agent_turn.hpp"
#include "src/ui/app/app_assembly.hpp"
#include "src/ui/app/inkcell_runtime.hpp"
#include "src/ui/app/model_boot.hpp"
#include "src/ui/app/repl.hpp"

namespace cortex::mk3::ui {

// Closed namespace block — keeps this file a pure re-export. All symbols
// originate in the included leaves above.

} // namespace cortex::mk3::ui