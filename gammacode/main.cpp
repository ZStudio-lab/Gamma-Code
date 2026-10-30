#include <algorithm>
#include <cctype>
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
#include "tools/registry.hpp"
#include "ui/launcher.hpp"
#include "ui/render.hpp"

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
  out << R"(Commands:
  /model <name>     switch the Ollama model
  /mode <plan|ask|auto>  switch permission mode (default ask)
  /context          show message count and estimated token usage
  /compact          force context compaction
  /clear            start a fresh conversation (removes the saved session)
  /exit             quit gamma
  /help             this list

Tools available:
  read_file(path, start_line?, end_line?)
  write_file(path, content)            no prompt (auto is default)
  edit_file(path, old_string, new_string)  unique match; no prompt
  list_dir(path?)                      lists a directory
  glob(pattern)                        finds files by pattern
  grep(pattern, path?, glob?)          regex search over files
  run_command(cmd, timeout_seconds?)   no prompt (use --no-yolo to gate it)

Modes: auto (default, nothing asks) · ask (writes/shell prompt) · plan (read-only)

Keys: ESC (or Ctrl+C) while the model streams = stop its output.
      In the launcher, N = new session; Esc works anywhere you'd guess.
)";
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
      std::cout << "  ⏺ restored conversation · " << (snapshot.messages - 1)
                << " messages · ≈" << snapshot.tokens
                << " tokens · /clear starts fresh\n";
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
      std::cout << "  ✗ " << error.what() << "\n";
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
      std::cout << "bye ✨\n";
      return false;
    } else if (command == "/model") {
      if (argument.empty()) {
        std::cout << "model: " << options_.model << "\n";
      } else {
        options_.model = argument;
        std::cout << "model: " << options_.model << " (verify with `ollama list`)\n";
      }
    } else if (command == "/mode") {
      if (argument.empty()) {
        std::cout << mode_line(options_) << "  (choose plan|ask|auto)\n";
      } else {
        gamma::Mode new_mode = options_.mode;
        if (gamma::parse_mode(argument, new_mode)) {
          options_.mode = new_mode;
          std::cout << "mode: " << gamma::mode_name(options_.mode) << "\n";
        } else {
          std::cout << "  ✗ unknown mode '" << argument << "' (plan|ask|auto)\n";
        }
      }
    } else if (command == "/context") {
      const AgentLoop::Stats snapshot = loop_.stats();
      std::cout << "messages: " << snapshot.messages << "   ≈tokens: " << snapshot.tokens
                << "   budget: " << snapshot.budget << "\n";
    } else if (command == "/compact") {
      const bool handled = loop_.compact(true);
      if (!handled) {
        std::cout << "  (nothing to compact — history already well inside budget)\n";
      }
    } else if (command == "/clear") {
      loop_.reset_history();
      std::error_code removed;
      std::filesystem::remove(session_file_, removed); // a fresh start stays fresh
      std::cout << "history cleared (saved session removed).\n";
    } else {
      std::cout << "  ✗ unknown command '" << command << "'; /help lists them\n";
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

  const bool model_installed =
      std::find(installed.begin(), installed.end(), options.model) != installed.end();
  if (!model_installed) {
    // Hard fail for non-interactive paths and when nothing at all is pulled.
    // Interactively, the launcher offers a live model picker instead.
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

  // Full-screen launcher (arrow keys: model / mode / workspace / start).
  // Skipped with --no-ui, which goes straight to the REPL banner.
  // The alternate screen owns the viewport for the whole interactive session:
  // scrolling up shows only this session, never the shell's older history.
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