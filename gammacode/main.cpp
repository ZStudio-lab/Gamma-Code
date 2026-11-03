#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "config/sessions.hpp"
#include "ollama/client.hpp"
#include "setup/ollama_setup.hpp"
#include "tools/registry.hpp"
#include "ui/theme.hpp"
#include "ui/webview_launcher.hpp"

// └─ shared "Opening Ollama" bootstrap ────────────────────────────────────────
// Runs on the GUI's worker thread while the page shows "opening Ollama…".
// Brings up the local server if it is missing, fetches the installed-model
// list, and auto-picks a model. Returns "" on success or a short
// human-readable failure line (the GUI surfaces it as a notice).
// If ollama_pid is non-null and we started Ollama, receives the PID (Windows only).
std::string open_ollama(gamma::Options& options, gamma::OllamaClient& client,
                        std::vector<std::string>& installed, bool interactive,
                        DWORD* ollama_pid = nullptr) {
  using gamma::ui::theme::paint;
  using gamma::ui::theme::paint_bold;
  using gamma::ui::theme::Role;

  // Automatic bootstrap: make sure a local server is up (offering to install
  // Ollama when it is missing), then choose a model. GAMMA_NO_SETUP=1 keeps
  // the old "bring your own server" behavior for automation.
  const char* no_setup_env = std::getenv("GAMMA_NO_SETUP");
  const bool skip_setup =
      no_setup_env != nullptr && no_setup_env[0] != '\0' && std::string(no_setup_env) != "0";
  if (!skip_setup && !gamma::setup::ensure_ollama_ready(options.base_url, interactive, ollama_pid))
    return "could not bring up a local Ollama server";

  // Startup handshake: fetch /api/tags — the GUI needs the installed-model
  // list, and every other check builds on it.
  try {
    installed = client.installed_models();
  } catch (const std::exception& error) {
    return "cannot reach Ollama at '" + options.base_url + "' (" + error.what() +
           ") — start `ollama serve` and try again";
  }

  auto refresh_installed = [&]() {
    try {
      installed = client.installed_models();
    } catch (const std::exception&) {
      // The pull itself succeeded; keep the model we just chose.
    }
  };

  bool model_installed =
      std::find(installed.begin(), installed.end(), options.model) != installed.end();

  if (!options.model_explicit) {
    if (installed.empty()) {
      if (!interactive)
        return "no Ollama models installed — pull one first, e.g. `ollama pull qwen2.5-coder:7b`";
      // First run: pick by hardware, then pull with a live progress bar.
      const gamma::setup::Hardware hardware = gamma::setup::detect_hardware();
      const std::string model = gamma::setup::recommended_model(hardware);
      if (!gamma::setup::pull_model_with_progress(client, model))
        return "model pull did not finish";
      options.model = model;
      refresh_installed();
      model_installed = true;
    } else {
      // Newest tool-capable model wins; /api/tags timestamps are RFC 3339 so
      // they sort lexicographically. Models that do not advertise "tools"
      // are skipped (embeddings, vision-only, …).
      std::vector<gamma::OllamaClient::ModelInfo> models;
      try {
        models = client.installed_model_info();
      } catch (const std::exception&) {
        models.clear();
      }
      std::sort(models.begin(), models.end(),
                [](const gamma::OllamaClient::ModelInfo& a,
                   const gamma::OllamaClient::ModelInfo& b) {
                  return a.modified_at > b.modified_at;
                });
      std::string chosen;
      for (const gamma::OllamaClient::ModelInfo& model : models) {
        if (client.model_supports_tools(model.name)) {
          chosen = model.name;
          break;
        }
      }
      if (chosen.empty() && !models.empty()) chosen = models.front().name;
      if (!chosen.empty()) {
        options.model = chosen;
        model_installed = true;
      }
    }
  } else if (!model_installed && installed.empty() && interactive) {
    // Explicit model on a machine with nothing pulled yet: fetch exactly the
    // model that was asked for instead of failing.
    if (!gamma::setup::pull_model_with_progress(client, options.model))
      return "model pull did not finish";
    refresh_installed();
    model_installed = true;
  }

  if (!model_installed) {
    // Hard fail for non-interactive paths. Interactively with other models
    // present, the launcher offers a live model picker instead.
    if (installed.empty())
      return "model '" + options.model + "' is not installed on Ollama — `ollama pull " +
             options.model + "`";
  }
  return "";
}

int main(int argc, char** argv) {
  // Parse options (GUI-only: no --no-ui, --one-shot, etc.)
  gamma::Options options;
  try {
    options = gamma::parse_options(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "gamma: " << error.what() << "\n";
    return 2;
  }
  if (options.show_help) {
    std::cout << gamma::usage() << "\n";
    return 0;
  }

  // Never silently run in "whatever the current directory is": re-open the
  // last workspace the user actually used (GUI picker or --workspace flag).
  // A folder that vanished since is ignored — that falls back to the CWD.
  if (options.workspace.empty()) options.workspace = gamma::sessions::last_workspace();

  gamma::OllamaClient client(options.base_url);
  const gamma::Registry registry = gamma::tools::default_registry();
  std::vector<std::string> installed;
  DWORD started_ollama_pid = 0; // will be set by open_ollama if we start Ollama

  // GUI startup hook: runs on the worker thread, interactive=false (the GUI
  // shows progress/errors in its own UI).
  const gamma::ui::StartupHook startup =
      [&](gamma::Options& gui_options, std::vector<std::string>& gui_models) -> std::string {
    return open_ollama(gui_options, client, gui_models, false, &started_ollama_pid);
  };

  // The native GUI chat app (chooser included implicitly by run_app).
  // The chooser appears FIRST; no Ollama traffic happens on the launch path.
  const char* no_webview_env = std::getenv("GAMMA_NO_WEBVIEW");
  const bool skip_webview = no_webview_env != nullptr && no_webview_env[0] != '\0' &&
                            std::string(no_webview_env) != "0";
  if (skip_webview || !gamma::ui::webview_launcher_built()) {
    // No WebView2 available: show a message box and exit cleanly.
    MessageBoxW(nullptr,
                L"Gamma Code requires the WebView2 Runtime (installed on Windows 10/11 by default).\n"
                L"Please install it from https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
                L"Gamma Code", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    return 1;
  }

  // All tool paths resolve relative to the workspace.
  try {
    gamma::activate_workspace(options);
  } catch (const std::exception& error) {
    std::cerr << "gamma: " << error.what() << "\n";
    return 2;
  }

  const gamma::ui::AppResult result = gamma::ui::run_app(options, startup);
  return (result == gamma::ui::AppResult::Done || result == gamma::ui::AppResult::Exit) ? 0 : 1;
}