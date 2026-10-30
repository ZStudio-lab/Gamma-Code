#pragma once

#include <filesystem>
#include <string>

namespace gamma {

enum class Mode { Plan, Ask, Auto };

std::string mode_name(Mode mode);
// Parses "plan" | "ask" | "auto"; returns false on anything else.
bool parse_mode(const std::string& name, Mode& out);

// The absolute root all tool paths resolve against: the --workspace folder
// once activate_workspace() ran, else the process start directory. The
// prompt and the tools read it dynamically; it never escapes gamma.
const std::filesystem::path& workspace_root();

struct Options {
  std::string model = "qwen2.5-coder:7b";
  Mode mode = Mode::Auto; // overridable via GAMMA_MODE / --mode (plan/ask re-enable gates)
  bool yolo = true;       // run_command auto-executes; --no-yolo re-enables its prompt
  // 0 => derive budget: 80% of the model's context window
  // (7b/13b -> 32k, 70b -> 128k, unknown -> 32k).
  long context_budget = 0;
  std::string workspace;       // working dir for tools; empty => cwd
  bool no_ui = false;          // --no-ui: skip the launcher, straight to REPL
  std::string one_shot_prompt; // set by -p, single turn then exit
  bool one_shot = false;
  std::string base_url = "http://127.0.0.1:11434";
  bool show_help = false;
  // The session file this interactive run writes to (launcher "Sessions"
  // picker pre-selects an existing one; otherwise the REPL mints a fresh
  // uniquely-named one). Empty until then; one-shot mode ignores it.
  std::string session_file;
};

// Model-name-based context window guess: 7b/13b -> 32k, 70b -> 128k,
// anything else -> 32k.
long context_window_for(const std::string& model);

// The token budget actually enforced: explicit override, else 80% of window.
long budget_tokens(const Options& opts);

// Sets the process working directory to opts.workspace (unset => stay put).
// Throws std::runtime_error when the path does not exist or is not a dir.
void activate_workspace(Options& opts);

// Reads env vars first (GAMMA_MODEL, GAMMA_MODE, GAMMA_CONTEXT, GAMMA_WORKSPACE), then applies
// CLI overrides. Throws std::runtime_error on bad values, including a
// non-loopback --ollama-url (gamma is local-only, 127.0.0.1 by design).
Options parse_options(int argc, char** argv);

// Usage/help block for --help and argument errors.
std::string usage();

} // namespace gamma
