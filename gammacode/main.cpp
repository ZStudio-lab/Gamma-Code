#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <replxx.hxx>

#include "agent/loop.hpp"
#include "config/config.hpp"
#include "config/sessions.hpp"
#include "ollama/client.hpp"
#include "setup/ollama_setup.hpp"
#include "tools/registry.hpp"
#include "ui/launcher.hpp"
#include "ui/render.hpp"
#include "ui/theme.hpp"
#include "ui/webview_launcher.hpp"

namespace gamma_repl {

using gamma::AgentLoop;
using gamma::OllamaClient;
using gamma::Options;
using gamma::Registry;

// Slash command /mode: prints current or switches mode.
std::string mode_line(const Options& options) {
  return "mode: " + gamma::mode_name(options.mode);
}

void print_help(std::ostream& out) {
  using gamma::ui::theme::paint;
  using gamma::ui::theme::paint_bold;
  using gamma::ui::theme::Role;
  out << "\n" << paint_bold("Commands", Role::Accent) << "\n";
  struct Cmd { const char* name; const char* args; const char* desc; };
  static constexpr Cmd kCmds[] = {
      {"/model", "<name>", "switch the Ollama model"},
      {"/mode", "<plan|ask|auto>", "switch permission mode (default ask)"},
      {"/context", "", "message count + estimated token usage"},
      {"/compact", "", "force context compaction"},
      {"/clear", "", "fresh conversation (removes the saved session)"},
      {"/exit", "", "quit gamma"},
      {"/help", "", "this list"},
  };
  for (const Cmd& cmd : kCmds) {
    out << "  " << paint(cmd.name, Role::Secondary);
    if (cmd.args[0] != '\0') out << " " << paint(cmd.args, Role::Warn);
    out << "  " << paint(cmd.desc, Role::Muted) << "\n";
  }
  out << "\n" << paint_bold("Tools", Role::Accent) << "\n";
  struct ToolDoc { const char* sig; const char* note; };
  static constexpr ToolDoc kTools[] = {
      {"read_file(path, start_line?, end_line?)", "capped long reads with continuation markers"},
      {"write_file(path, content)", "no prompt (auto is default); parents created"},
      {"edit_file(path, old, new, replace_all?)", "endings/indent tolerated; replace_all for every hit"},
      {"list_dir(path?)", "lists a directory"},
      {"glob(pattern, path?)", "skips build/dependency dirs"},
      {"grep(pattern, path?, glob?)", "skips binaries; capped at 200 hits"},
      {"run_command(cmd, timeout_seconds?)", "no prompt (use --no-yolo to gate it)"},
      {"ask(question, options?)", "pauses the REPL; the answer comes back to the model"},
      {"web_search(query, max_results?)", "DuckDuckGo; GAMMA_NO_WEB=1 removes it"},
  };
  for (const ToolDoc& tool : kTools) {
    out << "  " << paint(tool.sig, Role::Text);
    if (tool.note[0] != '\0') out << "  " << paint(tool.note, Role::Muted);
    out << "\n";
  }
  out << "\n" << paint_bold("Modes", Role::Accent) << "\n";
  out << "  " << paint("auto", Role::Secondary) << paint(" (default, nothing asks) · ", Role::Muted)
      << paint("ask", Role::Secondary) << paint(" (writes/shell prompt) · ", Role::Muted)
      << paint("plan", Role::Secondary) << paint(" (read-only)", Role::Muted) << "\n";
  out << "\n" << paint_bold("Keys", Role::Accent) << "\n";
  out << "  " << paint("Esc / Ctrl+C", Role::Warn) << paint(" while the model streams = stop its output", Role::Muted) << "\n";
  out << "  " << paint("N", Role::Warn) << paint(" in the launcher = new session · Esc backs out of menus", Role::Muted) << "\n\n";
}

std::vector<std::string> split_command(const std::string& line) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin < line.size()) {
    while (begin < line.size() && line[begin] == ' ')
      ++begin;
    if (begin >= line.size())
      break;
    std::size_t end = line.find(' ', begin);
    if (end == std::string::npos)
      end = line.size();
    parts.push_back(line.substr(begin, end - begin));
    begin = end + 1;
  }
  return parts;
}

class ReplSession {
 public:
  ReplSession(Options& options, OllamaClient& client, const Registry& registry)
      : options_(options),
        // Empty when the launcher's Start Chat made a new chat (filled in
        // run()); non-empty when the Sessions picker chose a saved one.
        session_file_(options.session_file),
        loop_(options_, client, registry,
              [](const gamma::Tool& tool, const nlohmann::json& arguments) {
                return gamma::ui::ask_yes_no(gamma::ui::proposal_text(tool.name, arguments));
              }) {}

  void run() { // interactive REPL
    gamma::ui::print_banner(options_);
    if (session_file_.empty()) {
      // Every conversation gets its own unique session file (see the
      // Sessions menu); timestamp-named so the list sorts newest-first.
      session_file_ = gamma::sessions::new_session_path(options_);
    } else if (!loop_.load_session(session_file_)) {
      // A picked file that no longer loads (deleted/corrupt): keep going
      // with a brand-new session rather than a confusing half-restore.
      session_file_ = gamma::sessions::new_session_path(options_);
    } else {
      // FULL restore: every message — user turns, replies, tool calls and
      // their JSON results — is back in history; compaction also re-applies.
      const gamma::AgentLoop::Stats snapshot = loop_.stats();
      std::cout << "  " << gamma::ui::theme::paint("► restored", gamma::ui::theme::Role::Accent)
                << gamma::ui::theme::paint(" conversation · ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(snapshot.messages - 1), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint(" messages · ≈", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(snapshot.tokens), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint(" tokens · /clear starts fresh", gamma::ui::theme::Role::Muted)
                << "\n";
    }
    while (true) {
      // Prompt is dynamic: it always shows the current workspace folder.
      std::string prompt = gamma::ui::prompt_string();
      const char* raw = rx_.input(prompt.c_str());
      std::string line = (raw == nullptr) ? std::string("/exit") : raw;
      if (!line.empty())
        rx_.history_add(line);
      if (!handle(line))
        break;
    }
    gamma::ui::restore_window_title(); // leave the shell window as we found it
    gamma::ui::leave_alt_screen(); // back to the normal buffer on session end
  }

 private:
  // Returns false when the REPL should exit.
  bool handle(const std::string& line) {
    if (line.empty() || line == "\r" || line == "\n")
      return true;
    if (line.starts_with('/'))
      return handle_slash(line);

    try {
      loop_.run_turn(line);
      // Session persistence: the current conversation is written after
      // every turn, so an exit or crash resumes cleanly next launch.
      loop_.save_session(session_file_);
    } catch (const std::exception& error) {
      std::cout << "  " << gamma::ui::theme::paint(std::string("✗ ") + error.what(),
                                            gamma::ui::theme::Role::Error)
                << "\n";
    }
    return true;
  }

  bool handle_slash(const std::string& line) {
    const std::vector<std::string> parts = split_command(line);
    const std::string command = parts.empty() ? "" : parts[0];
    const std::string argument = parts.size() > 1 ? parts[1] : "";
    if (command == "/help") {
      print_help(std::cout);
    } else if (command == "/exit" || command == "/quit") {
      std::cout << gamma::ui::theme::paint("bye", gamma::ui::theme::Role::Muted) << " "
                << gamma::ui::theme::paint("γ", gamma::ui::theme::Role::Accent) << "\n";
      return false;
    } else if (command == "/model") {
      if (argument.empty()) {
        std::cout << gamma::ui::theme::paint("model: ", gamma::ui::theme::Role::Muted)
                  << gamma::ui::theme::paint_bold(options_.model, gamma::ui::theme::Role::Highlight) << "\n";
      } else {
        options_.model = argument;
        std::cout << gamma::ui::theme::paint("model: ", gamma::ui::theme::Role::Muted)
                  << gamma::ui::theme::paint_bold(options_.model, gamma::ui::theme::Role::Highlight)
                  << gamma::ui::theme::paint("  (verify with `ollama list`)", gamma::ui::theme::Role::Muted)
                  << "\n";
      }
    } else if (command == "/mode") {
      if (argument.empty()) {
        std::cout << gamma::ui::theme::paint("mode: ", gamma::ui::theme::Role::Muted)
                  << gamma::ui::theme::paint_bold(gamma::mode_name(options_.mode), gamma::ui::theme::Role::Highlight)
                  << gamma::ui::theme::paint("  (choose plan|ask|auto)", gamma::ui::theme::Role::Muted) << "\n";
      } else {
        gamma::Mode new_mode = options_.mode;
        if (gamma::parse_mode(argument, new_mode)) {
          options_.mode = new_mode;
          std::cout << gamma::ui::theme::paint("mode: ", gamma::ui::theme::Role::Muted)
                    << gamma::ui::theme::paint_bold(gamma::mode_name(options_.mode), gamma::ui::theme::Role::Highlight)
                    << "\n";
        } else {
          std::cout << "  " << gamma::ui::theme::paint("✗ unknown mode '" + argument + "' (plan|ask|auto)",
                                                gamma::ui::theme::Role::Error) << "\n";
        }
      }
    } else if (command == "/context") {
      const AgentLoop::Stats snapshot = loop_.stats();
      std::cout << gamma::ui::theme::paint("messages: ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(snapshot.messages), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint("   ≈tokens: ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(snapshot.tokens), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint("   budget: ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(snapshot.budget), gamma::ui::theme::Role::Highlight)
                << "\n";
    } else if (command == "/compact") {
      const bool handled = loop_.compact(true);
      if (!handled) {
        std::cout << "  "
                  << gamma::ui::theme::paint("(nothing to compact — history already well inside budget)",
                                      gamma::ui::theme::Role::Muted) << "\n";
      }
    } else if (command == "/clear") {
      loop_.reset_history();
      std::error_code removed;
      std::filesystem::remove(session_file_, removed); // a fresh start stays fresh
      std::cout << gamma::ui::theme::paint("history cleared (saved session removed).",
                                    gamma::ui::theme::Role::Muted) << "\n";
    } else {
      std::cout << "  " << gamma::ui::theme::paint("✗ unknown command '" + command +
                                            "'; /help lists them",
                                            gamma::ui::theme::Role::Error) << "\n";
    }
    return true;
  }

  Options& options_;
  std::string session_file_; // saved-conversation path for this workspace
  AgentLoop loop_;
  replxx::Replxx rx_;
};

} // namespace gamma_repl

int main(int argc, char** argv) {
  gamma::ui::init_terminal();
  gamma::Options options;
  try {
    options = gamma::parse_options(argc, argv);
  } catch (const std::exception& error) {
    gamma::ui::restore_window_title();
    std::cerr << "gamma: " << error.what() << "\n";
    return 2;
  }
  if (options.show_help) {
    gamma::ui::restore_window_title();
    std::cout << gamma::usage() << "\n";
    return 0;
  }

  gamma::OllamaClient client(options.base_url);

  // Automatic bootstrap: make sure a local server is up (offering to install
  // Ollama when it is missing), then choose a model. GAMMA_NO_SETUP=1 keeps
  // the old "bring your own server" behavior for automation.
  const char* no_setup_env = std::getenv("GAMMA_NO_SETUP");
  const bool skip_setup =
      no_setup_env != nullptr && no_setup_env[0] != '\0' && std::string(no_setup_env) != "0";
  const bool interactive = gamma::setup::stdio_is_interactive();
  if (!skip_setup && !gamma::setup::ensure_ollama_ready(options.base_url, interactive)) {
    gamma::ui::restore_window_title();
    return 1;
  }

  // Startup handshake: fetch /api/tags up front — the launcher needs the
  // installed-model list, and every other check builds on it.
  std::vector<std::string> installed;
  try {
    installed = client.installed_models();
  } catch (const std::exception& error) {
    gamma::ui::restore_window_title();
    std::cerr << "gamma: cannot reach Ollama at '" << options.base_url << "': " << error.what()
              << "\nstart Ollama (`ollama serve`) and try again.\n";
    return 1;
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
      if (!interactive) {
        gamma::ui::restore_window_title();
        std::cerr << "gamma: no Ollama models are installed.\n"
                     "pull one first, e.g. `ollama pull qwen2.5-coder:7b`.\n";
        return 1;
      }
      // First run: pick by hardware, then pull with a live progress bar.
      const gamma::setup::Hardware hardware = gamma::setup::detect_hardware();
      const std::string model = gamma::setup::recommended_model(hardware);
      std::cout << "\n  "
                << gamma::ui::theme::paint("no models installed", gamma::ui::theme::Role::Warn)
                << gamma::ui::theme::paint(" · ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint(gamma::setup::describe_hardware(hardware),
                                           gamma::ui::theme::Role::Muted)
                << "\n";
      if (!gamma::setup::pull_model_with_progress(client, model)) {
        gamma::ui::restore_window_title();
        return 1;
      }
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
        std::cout << "  " << gamma::ui::theme::paint("using ", gamma::ui::theme::Role::Muted)
                  << gamma::ui::theme::paint_bold(options.model,
                                                  gamma::ui::theme::Role::Highlight)
                  << gamma::ui::theme::paint("  (newest installed)",
                                             gamma::ui::theme::Role::Muted)
                  << "\n";
      }
    }
  } else if (!model_installed && installed.empty() && interactive) {
    // Explicit model on a machine with nothing pulled yet: fetch exactly the
    // model that was asked for instead of failing.
    if (!gamma::setup::pull_model_with_progress(client, options.model)) {
      gamma::ui::restore_window_title();
      return 1;
    }
    refresh_installed();
    model_installed = true;
  }

  if (!model_installed) {
    // Hard fail for non-interactive paths. Interactively with other models
    // present, the launcher offers a live model picker instead.
    if (options.one_shot || options.no_ui || installed.empty()) {
      gamma::ui::restore_window_title();
      std::cerr << "gamma: model '" << options.model
                << "' is not installed on Ollama.\n"
                   "ollama pull "
                << options.model << "\n";
      return 1;
    }
  }

  const gamma::Registry registry = gamma::tools::default_registry();

  // All tool paths (one-shot included) resolve relative to the workspace.
  try {
    gamma::activate_workspace(options);
  } catch (const std::exception& error) {
    gamma::ui::restore_window_title();
    std::cerr << "gamma: " << error.what() << "\n";
    return 2;
  }

  if (options.one_shot) {
    gamma::AgentLoop loop(
        options, client, registry, [](const gamma::Tool& tool, const nlohmann::json& arguments) {
          return gamma::ui::ask_yes_no(gamma::ui::proposal_text(tool.name, arguments));
        });
    try {
      const bool completed = loop.run_turn(options.one_shot_prompt);
      gamma::ui::restore_window_title();
      return completed ? 0 : 2;
    } catch (const std::exception& error) {
      gamma::ui::restore_window_title();
      std::cerr << "gamma: " << error.what() << "\n";
      return 2;
    }
  }

  // The native front-end chooser: a small window asking "Open Gamma Code
  // in:" with GUI / CLI buttons. GUI resizes into the full chat app (an
  // embedded WebView2 running an in-memory, fully local page); CLI hands the
  // session to the console TUI. Skipped with --no-ui (straight to the REPL)
  // or GAMMA_NO_WEBVIEW=1 / when this build has no WebView2 SDK.
  const char* no_webview_env = std::getenv("GAMMA_NO_WEBVIEW");
  const bool skip_webview = no_webview_env != nullptr && no_webview_env[0] != '\0' &&
                            std::string(no_webview_env) != "0";
  if (!options.no_ui && !skip_webview && gamma::ui::webview_launcher_built()) {
    const gamma::ui::AppResult result =
        gamma::ui::run_app(options, client, registry, installed);
    if (result == gamma::ui::AppResult::Done) {
      gamma::ui::restore_window_title();
      return 0; // the GUI chat ran and finished
    }
    if (result == gamma::ui::AppResult::Exit) {
      gamma::ui::restore_window_title();
      return 0; // closed during the chooser without deciding
    }
    // AppResult::Cli → the console flow below; Unavailable → same fallback.
  }

  // Console flow (CLI choice, no WebView2, or --no-ui): the alternate screen
  // owns the viewport for the whole interactive session — scrolling up shows
  // only this session, never the shell's older history.
  gamma::ui::enter_alt_screen();
  if (!options.no_ui) {
    if (!gamma::ui::launcher(options, installed)) {
      gamma::ui::leave_alt_screen();
      gamma::ui::restore_window_title();
      return 0; // user chose Exit (or Esc / q)
    }
    gamma::ui::clear_screen();
  }

  try {
    gamma::activate_workspace(options);
  } catch (const std::exception& error) {
    gamma::ui::restore_window_title();
    std::cerr << "gamma: " << error.what() << "\n";
    return 2;
  }

  // No launcher (--no-ui): no Sessions menu is shown, so fall back to
  // auto-resuming this workspace's most recent saved conversation.
  if (options.no_ui && options.session_file.empty()) {
    const std::vector<std::string> saved = gamma::sessions::saved_sessions();
    std::string workspace = options.workspace;
    if (workspace.empty()) {
      std::error_code error;
      workspace = std::filesystem::current_path(error).string();
    }
    const std::string leaf =
        std::filesystem::path(workspace).filename().string() + "_";
    for (const std::string& file : saved) {
      const std::string name = std::filesystem::path(file).filename().string();
      if (name.rfind(leaf, 0) == 0) { // prefix match: this workspace's file
        options.session_file = file;
        break;
      }
    }
  }

  gamma_repl::ReplSession(options, client, registry).run();
  gamma::ui::restore_window_title();
  return 0;
}