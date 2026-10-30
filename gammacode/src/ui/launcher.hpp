#pragma once

#include <string>
#include <vector>

#include "config/config.hpp"

namespace gamma::ui {

// The full-screen launcher shown when gamma starts interactively.
// Arrow-key driven: model (fetched from Ollama), permission mode, workspace
// folder, a system-prompt preview, then Enter drops into the γ ❯ REPL.
// Returns false when the user chose to exit.
bool launcher(Options& options, const std::vector<std::string>& ollama_models);

// Arrow-key picker over the installed /api/tags model list. Returns the
// chosen index or -1 when canceled. Reused by the REPL's /model later.
int model_picker(const std::vector<std::string>& models, std::size_t preselect);

// Wipes the viewport (ANSI clear + home).
void clear_screen();

// The block-letter GAMMA logo + subtitle, colors on.
void print_logo();

} // namespace gamma::ui
