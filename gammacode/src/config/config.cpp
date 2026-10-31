#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <stdexcept>

namespace gamma {

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// "32" -> 32, "32k" -> 32768, "1m" -> 1048576. Throws on anything else.
long parse_token_count(const std::string& raw, const char* what_is) {
  std::string text = to_lower(raw);
  if (text.empty()) {
    throw std::runtime_error(
        std::format("{} must be a token count (optional k/m suffix), got '{}'", what_is, raw));
  }
  long multiplier = 1;
  const char last = text.back();
  if (last == 'k') {
    multiplier = 1024;
    text.pop_back();
  } else if (last == 'm') {
    multiplier = 1024 * 1024;
    text.pop_back();
  }
  std::size_t used = 0;
  long value = 0;
  try {
    value = std::stol(text, &used);
  } catch (const std::exception&) {
    value = -1; // fall through to the error below
  }
  if (used != text.size() || value <= 0) {
    throw std::runtime_error(
        std::format("{} must be a token count (optional k/m suffix), got '{}'", what_is, raw));
  }
  return value * multiplier;
}

// Extracts the host part of a URL (plain, IPv4 or [bracketed-IPv6] forms).
std::string url_host(const std::string& url) {
  const std::string scheme_marker = "//";
  const std::size_t scheme = url.find("://");
  // "://" match starts at the colon; the host begins after the second slash.
  const std::size_t start = (scheme == std::string::npos) ? 0 : scheme + scheme_marker.size() + 1;
  const std::size_t path = url.find('/', start);
  std::size_t end = (path == std::string::npos) ? url.size() : path;
  if (start < url.size() && url[start] == '[') {
    // IPv6 literal: [::1]:11434
    const std::size_t bracket = url.find(']', start);
    if (bracket == std::string::npos || bracket > end) {
      throw std::runtime_error(std::format("'{}' is not a valid URL", url));
    }
    return url.substr(start + 1, bracket - start - 1);
  }
  const std::size_t colon = url.find(':', start);
  if (colon != std::string::npos && colon < end)
    end = colon;
  return url.substr(start, end - start);
}

// Local-only enforcement: only 127.0.0.1, ::1 and localhost may be configured.
void enforce_loopback(const std::string& url, const char* flag_name) {
  const std::string host = url_host(url);
  if (host != "127.0.0.1" && host != "::1" && host != "localhost") {
    throw std::runtime_error(
        std::format("gamma is local-only: {} '{}' has non-loopback host '{}' — refusing to start",
                    flag_name, url, host));
  }
}

} // namespace

std::filesystem::path& workspace_root_ref() {
  static std::filesystem::path root = std::filesystem::current_path();
  return root;
}

const std::filesystem::path& workspace_root() { return workspace_root_ref(); }

std::string mode_name(Mode mode) {
  switch (mode) {
  case Mode::Plan:
    return "plan";
  case Mode::Ask:
    return "ask";
  case Mode::Auto:
    return "auto";
  }
  return "ask";
}

bool parse_mode(const std::string& name, Mode& out) {
  if (name == "plan") {
    out = Mode::Plan;
    return true;
  }
  if (name == "ask") {
    out = Mode::Ask;
    return true;
  }
  if (name == "auto") {
    out = Mode::Auto;
    return true;
  }
  return false;
}

void activate_workspace(Options& opts) {
  if (opts.workspace.empty()) {
    // Still anchor the tool root: the current start directory.
    workspace_root_ref() = std::filesystem::current_path();
    return;
  }
  const std::filesystem::path directory(opts.workspace);
  if (!std::filesystem::exists(directory)) {
    throw std::runtime_error(
        std::format("--workspace path does not exist: '{}'", opts.workspace));
  }
  if (!std::filesystem::is_directory(directory)) {
    throw std::runtime_error(
        std::format("--workspace is not a directory: '{}'", opts.workspace));
  }
  std::error_code error;
  const std::filesystem::path absolute =
      std::filesystem::weakly_canonical(directory, error);
  std::filesystem::current_path(error ? directory : absolute);
  opts.workspace = (error ? directory : absolute).string();
  workspace_root_ref() = std::filesystem::current_path();
}

long context_window_for(const std::string& model) {
  const std::string lowered = to_lower(model);
  if (lowered.find("70b") != std::string::npos)
    return 131072; // 128k
  if (lowered.find("13b") != std::string::npos)
    return 32768;
  if (lowered.find("7b") != std::string::npos)
    return 32768;
  return 32768; // unknown -> conservative default
}

long budget_tokens(const Options& opts) {
  if (opts.context_budget > 0)
    return opts.context_budget;
  return context_window_for(opts.model) * 4 / 5; // 80% of the context window
}

Options parse_options(int argc, char** argv) {
  Options opts;

  if (const char* env = std::getenv("GAMMA_MODEL")) {
    if (env[0] != '\0') {
      opts.model = env;
      opts.model_explicit = true;
    }
  }
  if (const char* env = std::getenv("GAMMA_MODE")) {
    if (env[0] != '\0' && !parse_mode(env, opts.mode)) {
      throw std::runtime_error(
          std::format("GAMMA_MODE must be one of plan|ask|auto, got '{}'", env));
    }
  }
  if (const char* env = std::getenv("GAMMA_WORKSPACE")) {
    if (env[0] != '\0') opts.workspace = env;
  }
  if (const char* env = std::getenv("GAMMA_CONTEXT")) {
    if (env[0] != '\0')
      opts.context_budget = parse_token_count(env, "GAMMA_CONTEXT");
  }

  auto need_value = [&](int i, const std::string& flag) -> std::string {
    if (i + 1 >= argc) {
      throw std::runtime_error(std::format("{} requires a value\n\n{}", flag, usage()));
    }
    return argv[i + 1];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      opts.show_help = true;
    } else if (arg == "-p" || arg == "--prompt") {
      opts.one_shot_prompt = need_value(i++, arg);
      opts.one_shot = true;
    } else if (arg == "--model" || arg == "-m") {
      opts.model = need_value(i++, arg);
      opts.model_explicit = true;
    } else if (arg == "--mode") {
      const std::string value = need_value(i++, arg);
      if (!parse_mode(value, opts.mode)) {
        throw std::runtime_error(
            std::format("--mode must be one of plan|ask|auto, got '{}'", value));
      }
    } else if (arg == "--workspace" || arg == "-w") {
      opts.workspace = need_value(i++, arg);
    } else if (arg == "--context") {
      opts.context_budget = parse_token_count(need_value(i++, arg), "--context");
    } else if (arg == "--no-ui") {
      opts.no_ui = true;
    } else if (arg == "--yolo") {
      // Legacy flag: auto-mode + silent run_command is the default already.
      opts.yolo = true;
    } else if (arg == "--no-yolo") {
      opts.yolo = false; // re-enable the run_command approval prompt
    } else if (arg == "--ollama-url") {
      opts.base_url = need_value(i++, arg);
    } else {
      throw std::runtime_error(std::format("unknown argument '{}'\n\n{}", arg, usage()));
    }
  }

  enforce_loopback(opts.base_url, "--ollama-url");
  return opts;
}

std::string usage() {
  return std::format(R"(gamma {} — local-only coding agent

Usage:
  gamma                     interactive REPL
  gamma -p "<prompt>"       one-shot
  gamma --model <name>      pin a model; default: newest installed (also GAMMA_MODEL)
  gamma --mode <plan|ask>   opt-in: re-enable permission gates (else fully automated)
  gamma --context <tokens>  default: 32k (also GAMMA_CONTEXT)
  gamma --no-yolo           re-approve run_command prompts (auto-run is default)
  gamma --no-ui             skip the full-screen launcher; go straight to the REPL
  gamma --workspace <dir>   working dir for all tools (also GAMMA_WORKSPACE)
  gamma --ollama-url <url>  must be loopback; default http://127.0.0.1:11434
  gamma --help

On first run gamma offers to install Ollama, then pulls the best qwen2.5-coder
model for this machine (1.5b / 3b / 7b / 14b / 32b). After that it starts
`ollama serve` when needed and uses the newest installed tool-capable model.
Set GAMMA_NO_SETUP=1 to skip that bootstrap and manage Ollama yourself.

Everything runs without permission prompts by default (mode auto): reads,
writes, edits and commands are executed straight away with their results
shown as breadcrumbs. --mode ask|plan re-enables the permission gates. All
traffic stays on 127.0.0.1 via Ollama.)",
                     GAMMA_VERSION);
}

} // namespace gamma
