#pragma once

#include <string>

#include "ollama/client.hpp"

namespace gamma::setup {

// What the machine has, used to pick a model that will actually run.
struct Hardware {
  unsigned long long ram_bytes = 0;
  unsigned long long vram_bytes = 0; // largest discrete GPU, 0 when unknown
  std::string gpu_name;
};

Hardware detect_hardware();

// Best qwen2.5-coder tag for this machine (1.5b / 3b / 7b / 14b / 32b).
std::string recommended_model(const Hardware& hardware);

// "16 GB RAM · NVIDIA GeForce RTX 4060 (8 GB VRAM)" — for the setup line.
std::string describe_hardware(const Hardware& hardware);

// Locate ollama.exe: PATH first, then the documented per-user install dir.
// Empty when Ollama is not installed.
std::string find_ollama_executable();

// GET /api/tags with a short timeout. Never throws.
bool server_reachable(const std::string& base_url);

// Spawn `ollama serve` hidden and wait until /api/tags answers (max 30s).
// If out_pid is non-null, receives the PID of the started process (Windows only).
bool start_ollama_server(const std::string& executable, const std::string& base_url, DWORD* out_pid = nullptr);

// Download the official OllamaSetup.exe and run it silently. Returns the
// installed ollama.exe path, or empty on failure. Prints its own progress.
std::string install_ollama();

// Startup flow: make sure the local API is up, offering to install Ollama
// first when it is missing. A non-interactive run never prompts.
// If out_pid is non-null and we started Ollama, receives the PID (Windows only).
bool ensure_ollama_ready(const std::string& base_url, bool interactive, DWORD* out_pid = nullptr);

// After ensure_ollama_ready returns true, call this to get the PID of the
// Ollama process we started (0 if we didn't start one).
DWORD get_started_ollama_pid();

// Pull `model`, drawing one live progress line. Returns true on success.
bool pull_model_with_progress(OllamaClient& client, const std::string& model);

// True when stdin and stdout are both a real console (not redirected).
bool stdio_is_interactive();

} // namespace gamma::setup
