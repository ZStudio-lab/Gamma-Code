#pragma once

#include <functional>
#include <string>
#include <vector>

#include "agent/loop.hpp"
#include "config/config.hpp"
#include "ollama/client.hpp"
#include "tools/registry.hpp"

namespace gamma::ui {

// Outcome of the native front-end chooser.
enum class AppResult {
  Cli,        // user chose the terminal — main() runs the console launcher + REPL
  Done,       // the GUI chat ran and finished (the window was closed cleanly)
  Exit,       // the user closed / quit during the chooser (nothing ran)
  Unavailable // no WebView2 SDK/runtime — main() falls back to the console
};

// "Opening Ollama" bootstrap, run by whichever front-end the chooser picked:
// bring the local server up if it is missing, fetch the installed-model list,
// and auto-pick a model. Runs on the GUI's worker thread (the window is up
// while it works — the page shows "opening Ollama…" in its own status bar) or
// inline at the start of the console front-end. Returns "" on success,
// otherwise a short human-readable failure line for the active front-end.
using StartupHook =
    std::function<std::string(Options& options, std::vector<std::string>& installed)>;

// Chooser → GUI → Done / Cli / Exit / Unavailable. The chooser window appears
// FIRST; no Ollama traffic happens on the launch path at all.
AppResult run_app(Options& options, const StartupHook& startup);

// Chooser only (0=closed, 1=GUI, 2=CLI) — exported so runners can branch
// between the two front-ends before any startup work.
int run_chooser();

// The GUI chat app on its own (chooser included implicitly by run_app).
// `startup` runs on a worker thread while the page shows "opening Ollama…";
// Unavailable means the WebView2 runtime/loader is missing.
enum class ChatResult { Done, Unavailable };
ChatResult run_chat_app(Options& options, const StartupHook& startup);

// True when this build embeds the WebView2 loader (compile-time SDK presence).
bool webview_launcher_built();

} // namespace gamma::ui
