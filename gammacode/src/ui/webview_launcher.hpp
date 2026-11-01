#pragma once

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

// The whole interactive experience: first the small native chooser window
// ("Open Gamma Code in:" with GUI / CLI buttons), then either the full chat
// app (an embedded WebView2 running an in-memory, fully local page — top bar
// with model/mode/history, streaming transcript with tool breadcrumbs,
// permission and ask dialogs) or the console front-end. The only network
// access is the user's own Ollama on 127.0.0.1.
AppResult run_app(Options& options, OllamaClient& client, const Registry& registry,
                  const std::vector<std::string>& models);

// The GUI chat app on its own (chooser included implicitly by run_app).
// Unavailable means the WebView2 runtime/loader is missing.
enum class ChatResult { Done, Unavailable };
ChatResult run_chat_app(Options& options, const std::vector<std::string>& models);

// True when this build embeds the WebView2 loader (compile-time SDK presence).
bool webview_launcher_built();

} // namespace gamma::ui
