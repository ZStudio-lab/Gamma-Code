#include "setup/ollama_setup.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ui/render.hpp"
#include "ui/theme.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace gamma::setup {

namespace {

namespace fs = std::filesystem;
using ui::theme::paint;
using ui::theme::paint_bold;
using ui::theme::Role;

constexpr unsigned long long kGiB = 1024ull * 1024ull * 1024ull;
constexpr unsigned long long kMiB = 1024ull * 1024ull;

std::string lower_copy(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool stdout_is_tty() {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) != 0;
#else
  return isatty(fileno(stdout)) != 0;
#endif
}

bool stdin_is_tty() {
#ifdef _WIN32
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(fileno(stdin)) != 0;
#endif
}

std::string human_bytes(long long bytes) {
  char buffer[48];
  const double value = static_cast<double>(bytes);
  if (bytes >= static_cast<long long>(kGiB))
    std::snprintf(buffer, sizeof(buffer), "%.1f GB", value / static_cast<double>(kGiB));
  else if (bytes >= static_cast<long long>(kMiB))
    std::snprintf(buffer, sizeof(buffer), "%.0f MB", value / static_cast<double>(kMiB));
  else
    std::snprintf(buffer, sizeof(buffer), "%.0f KB", value / 1024.0);
  return buffer;
}

std::string human_gib(unsigned long long bytes) {
  const unsigned long long rounded = (bytes + kGiB / 2) / kGiB;
  return std::to_string(rounded) + " GB";
}

// One progress line on stdout, overwritten in place. `last_width` is the
// visible width of the previous frame so shorter lines fully erase it.
void draw_progress(const std::string& label, int percent, long long done, long long total,
                   std::size_t& last_width) {
  constexpr int kBarWidth = 20;
  const int filled = std::clamp(percent, 0, 100) * kBarWidth / 100;
  std::string bar;
  for (int i = 0; i < kBarWidth; ++i) bar += (i < filled) ? "█" : "░";

  const std::string left = "  " + label + " ";
  const std::string right = " " + std::to_string(std::clamp(percent, 0, 100)) + "%  " +
                            human_bytes(done) + "/" + human_bytes(total);
  const std::string plain = left + bar + right;
  const std::string styled = "  " + paint(label, Role::Muted) + " " +
                             paint(bar, Role::Secondary) + " " +
                             paint_bold(std::to_string(std::clamp(percent, 0, 100)) + "%",
                                        Role::Highlight) +
                             paint("  " + human_bytes(done) + "/" + human_bytes(total), Role::Muted);

  std::string line = "\r" + styled;
  if (last_width > plain.size()) line += std::string(last_width - plain.size(), ' ');
  last_width = plain.size();
  std::cout << line << std::flush;
}

/* ───────────────────────── hardware detection ───────────────────────── */

#ifdef _WIN32

std::string read_registry_string(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return {};
  if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
  std::wstring wide(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(wide.data()), &bytes) !=
      ERROR_SUCCESS)
    return {};
  wide.resize(bytes / sizeof(wchar_t));
  while (!wide.empty() && wide.back() == L'\0') wide.pop_back();
  if (wide.empty()) return {};
  const int size =
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0,
                          nullptr, nullptr);
  if (size <= 0) return {};
  std::string text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(), size,
                      nullptr, nullptr);
  return text;
}

unsigned long long read_registry_u64(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return 0;
  if (bytes == sizeof(unsigned long long)) {
    unsigned long long value = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) ==
        ERROR_SUCCESS)
      return value;
  }
  return 0;
}

Hardware detect_hardware_windows() {
  Hardware hardware;
  MEMORYSTATUSEX memory = {};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) hardware.ram_bytes = memory.ullTotalPhys;

  // GPU memory from the display-adapter class registry. qwMemorySize is the
  // dedicated VRAM reported by the driver; ignore software adapters (Basic
  // Display, Remote) and integrated graphics unless they are Intel Arc.
  HKEY adapters = nullptr;
  const wchar_t* kDisplayClass =
      L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDisplayClass, 0, KEY_READ, &adapters) !=
      ERROR_SUCCESS)
    return hardware;

  for (DWORD index = 0;; ++index) {
    wchar_t subkey[256];
    DWORD name_length = static_cast<DWORD>(std::size(subkey));
    const LSTATUS listed =
        RegEnumKeyExW(adapters, index, subkey, &name_length, nullptr, nullptr, nullptr, nullptr);
    if (listed != ERROR_SUCCESS) break;

    HKEY adapter = nullptr;
    if (RegOpenKeyExW(adapters, subkey, 0, KEY_READ, &adapter) != ERROR_SUCCESS) continue;
    const std::string description = read_registry_string(adapter, L"DriverDesc");
    const unsigned long long vram = read_registry_u64(adapter, L"HardwareInformation.qwMemorySize");
    RegCloseKey(adapter);

    const std::string lowered = lower_copy(description);
    const bool software = lowered.find("basic display") != std::string::npos ||
                          lowered.find("remote") != std::string::npos;
    const bool discrete = lowered.find("nvidia") != std::string::npos ||
                          lowered.find("radeon") != std::string::npos ||
                          lowered.find("arc") != std::string::npos;
    // Sanity cap: a bogus 4 EB reading is a shared-memory iGPU, not VRAM.
    if (software || !discrete || vram < kGiB || vram > 256ull * kGiB) continue;
    if (vram > hardware.vram_bytes) {
      hardware.vram_bytes = vram;
      hardware.gpu_name = description;
    }
  }
  RegCloseKey(adapters);
  return hardware;
}

#endif

/* ───────────────────────── process helpers ───────────────────────── */

#ifdef _WIN32

// Runs a command line to completion, hidden. Returns false when the process
// could not start or exceeded timeout_ms.
bool run_and_wait(const std::string& command_line, DWORD timeout_ms, DWORD& exit_code) {
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');

  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &process))
    return false;

  const DWORD waited = WaitForSingleObject(process.hProcess, timeout_ms);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5000);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return false;
  }
  exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  return true;
}

std::string serve_log_path() {
  const char* local = std::getenv("LOCALAPPDATA");
  fs::path base = (local != nullptr && local[0] != '\0')
                      ? fs::path(local)
                      : fs::path(std::getenv("TEMP") != nullptr ? std::getenv("TEMP") : ".");
  return (base / "Gamma" / "ollama-serve.log").string();
}

#else

bool run_and_wait(const std::string& command_line, int timeout_seconds, int& exit_code) {
  const std::string wrapped = command_line + " >/dev/null 2>&1";
  const int status = std::system(wrapped.c_str());
  if (status == -1) return false;
  exit_code = status;
  return true;
}

#endif

/* ───────────────────────── download ───────────────────────── */

struct DownloadState {
  std::ofstream* file = nullptr;
  bool tty = false;
  std::size_t last_width = 0;
  int last_percent = -1;
  std::string label = "downloading Ollama";
};

std::size_t download_write(char* data, std::size_t size, std::size_t count, void* user) {
  auto* state = static_cast<DownloadState*>(user);
  const std::size_t bytes = size * count;
  state->file->write(data, static_cast<std::streamsize>(bytes));
  return bytes;
}

int download_progress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
  auto* state = static_cast<DownloadState*>(user);
  if (!state->tty || total <= 0) return 0;
  const int percent = static_cast<int>(now * 100 / total);
  if (percent == state->last_percent) return 0;
  state->last_percent = percent;
  draw_progress(state->label, percent, static_cast<long long>(now),
                static_cast<long long>(total), state->last_width);
  return 0;
}

bool download_file(const std::string& url, const std::string& destination, bool tty) {
  std::ofstream file(destination, std::ios::binary | std::ios::trunc);
  if (!file.is_open()) {
    std::cerr << paint("  ✗ cannot write " + destination, Role::Error) << "\n";
    return false;
  }

  DownloadState state;
  state.file = &file;
  state.tty = tty;

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    std::cerr << paint("  ✗ curl_easy_init failed", Role::Error) << "\n";
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 6L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &download_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &download_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  const CURLcode status = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  file.close();

  if (state.tty) {
    std::cout << "\r\033[2K" << std::flush; // wipe the progress line
  }
  if (status != CURLE_OK) {
    std::cerr << paint(std::string("  ✗ download failed: ") + curl_easy_strerror(status),
                       Role::Error)
              << "\n";
    std::error_code ignored;
    fs::remove(destination, ignored);
    return false;
  }
  return true;
}

} // namespace

// PID of Ollama started by this process (0 if we didn't start it).
// Written by ensure_ollama_ready, read by the GUI startup worker.
static std::atomic<DWORD> g_started_ollama_pid{0};

/* ───────────────────────── public API ───────────────────────── */

Hardware detect_hardware() {
#ifdef _WIN32
  return detect_hardware_windows();
#else
  return Hardware{};
#endif
}

std::string describe_hardware(const Hardware& hardware) {
  std::string text = hardware.ram_bytes > 0 ? human_gib(hardware.ram_bytes) + " RAM" : "RAM unknown";
  if (hardware.vram_bytes > 0) {
    text += " · ";
    if (!hardware.gpu_name.empty())
      text += hardware.gpu_name + " (" + human_gib(hardware.vram_bytes) + " VRAM)";
    else
      text += human_gib(hardware.vram_bytes) + " VRAM";
  }
  return text;
}

std::string recommended_model(const Hardware& hardware) {
  // Q4-class footprints, with headroom for the OS and the context window:
  // 1.5b ≈ 1.2 GB · 3b ≈ 2.2 GB · 7b ≈ 4.7 GB · 14b ≈ 9 GB · 32b ≈ 20 GB.
  if (hardware.vram_bytes >= 24ull * kGiB || hardware.ram_bytes >= 64ull * kGiB)
    return "qwen2.5-coder:32b";
  if (hardware.vram_bytes >= 16ull * kGiB || hardware.ram_bytes >= 32ull * kGiB)
    return "qwen2.5-coder:14b";
  if (hardware.vram_bytes >= 8ull * kGiB || hardware.ram_bytes >= 16ull * kGiB)
    return "qwen2.5-coder:7b";
  if (hardware.vram_bytes >= 4ull * kGiB || hardware.ram_bytes >= 8ull * kGiB)
    return "qwen2.5-coder:3b";
  return "qwen2.5-coder:1.5b";
}

std::string find_ollama_executable() {
#ifdef _WIN32
  // Known install locations first: the Windows installer is per-user by
  // default. This also avoids resolving a stray ollama.exe from the current
  // directory before the real install.
  std::vector<fs::path> candidates;
  const char* local = std::getenv("LOCALAPPDATA");
  if (local != nullptr && local[0] != '\0') {
    candidates.push_back(fs::path(local) / "Programs" / "Ollama" / "ollama.exe");
    candidates.push_back(fs::path(local) / "Ollama" / "ollama.exe");
  }
  const char* program_files = std::getenv("ProgramFiles");
  if (program_files != nullptr && program_files[0] != '\0')
    candidates.push_back(fs::path(program_files) / "Ollama" / "ollama.exe");
  const char* program_files_x86 = std::getenv("ProgramFiles(x86)");
  if (program_files_x86 != nullptr && program_files_x86[0] != '\0')
    candidates.push_back(fs::path(program_files_x86) / "Ollama" / "ollama.exe");

  for (const fs::path& candidate : candidates) {
    std::error_code error;
    if (fs::is_regular_file(candidate, error)) return candidate.string();
  }

  // Fall back to PATH for portable/custom installs.
  char found[MAX_PATH];
  DWORD length = SearchPathA(nullptr, "ollama.exe", nullptr, MAX_PATH, found, nullptr);
  if (length > 0 && length < MAX_PATH) return std::string(found);
  if (length >= MAX_PATH) {
    std::string buffer(static_cast<std::size_t>(length), '\0');
    length = SearchPathA(nullptr, "ollama.exe", nullptr, length, buffer.data(), nullptr);
    if (length > 0 && length <= buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
  }
#else
  for (const char* candidate : {"/usr/local/bin/ollama", "/usr/bin/ollama"}) {
    std::error_code error;
    if (fs::is_regular_file(candidate, error)) return candidate;
  }
#endif
  return {};
}

bool server_reachable(const std::string& base_url) {
  try {
    OllamaClient probe(base_url);
    (void)probe.installed_models();
    return true;
  } catch (...) {
    return false;
  }
}

bool start_ollama_server(const std::string& executable, const std::string& base_url, DWORD* out_pid) {
#ifdef _WIN32
  const std::string log_path = serve_log_path();
  std::error_code error;
  fs::create_directories(fs::path(log_path).parent_path(), error);

  SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE log = CreateFileA(log_path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE null_input =
      CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = null_input;
  if (log != INVALID_HANDLE_VALUE) {
    startup.hStdOutput = log;
    startup.hStdError = log;
  }

  std::string command = "\"" + executable + "\" serve";
  std::vector<char> mutable_line(command.begin(), command.end());
  mutable_line.push_back('\0');

  PROCESS_INFORMATION process = {};
  const BOOL started =
      CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                     nullptr, nullptr, &startup, &process);

  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
  if (!started) return false;
  if (out_pid) *out_pid = process.dwProcessId;
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
#else
  const std::string command = "\"" + executable + "\" serve &";
  (void)std::system(command.c_str());
#endif

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    if (server_reachable(base_url)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  return false;
}

std::string install_ollama() {
  const std::string url = "https://ollama.com/download/OllamaSetup.exe";
  std::error_code error;
  const fs::path setup =
      fs::temp_directory_path(error) / "Gamma-OllamaSetup.exe";
  const bool tty = stdout_is_tty();

  std::cout << "  " << paint("downloading", Role::Muted) << " " << paint_bold("Ollama", Role::Highlight)
            << paint("  " + url, Role::Muted) << "\n";
  if (!download_file(url, setup.string(), tty)) return {};
  std::cout << "  " << paint("✔ download complete", Role::Success) << " "
            << paint("(" + human_bytes(static_cast<long long>(fs::file_size(setup, error))) + ")",
                     Role::Muted)
            << "\n";

  ui::Spinner spinner;
  spinner.start();
  spinner.set_label("installing Ollama");
  DWORD exit_code = 0;
  const std::string command =
      "\"" + setup.string() + "\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART";
  const bool ran = run_and_wait(command, 10u * 60u * 1000u, exit_code);
  spinner.stop();
  fs::remove(setup, error);

  if (!ran) {
    std::cout << "  " << paint("✗ the Ollama installer did not finish", Role::Error) << "\n"
              << "    download and run it manually: " << url << "\n";
    return {};
  }
  if (exit_code != 0) {
    std::cout << "  " << paint("✗ the Ollama installer exited with code " +
                                   std::to_string(exit_code),
                               Role::Error)
              << "\n";
    return {};
  }

  std::string executable = find_ollama_executable();
  if (executable.empty()) {
    // PATH is updated only for new processes; the per-user install dir is the
    // canonical location on Windows, so look there one more time.
    const char* local = std::getenv("LOCALAPPDATA");
    if (local != nullptr && local[0] != '\0') {
      const fs::path guess = fs::path(local) / "Programs" / "Ollama" / "ollama.exe";
      if (fs::is_regular_file(guess, error)) executable = guess.string();
    }
  }
  return executable;
}

bool ensure_ollama_ready(const std::string& base_url, bool interactive, DWORD* out_pid) {
  if (server_reachable(base_url)) return true;

  std::string executable = find_ollama_executable();
  if (executable.empty()) {
    std::cout << "\n  " << paint("Ollama is not installed.", Role::Warn) << "\n"
              << "  " << paint("Gamma needs it for local models on 127.0.0.1 — no cloud.", Role::Muted)
              << "\n";
    if (!interactive) {
      std::cout << "  install it from " << paint("https://ollama.com/download/windows", Role::Secondary)
                << ", then run gamma again.\n";
      return false;
    }
    std::cout << "\n  " << paint_bold("Install Ollama? [Yes/No] ", Role::Highlight) << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) answer.clear();
    const std::string normalized = lower_copy(answer);
    if (normalized != "y" && normalized != "yes") {
      std::cout << "  " << paint("no changes made.", Role::Muted) << "\n"
                << "  install it from " << paint("https://ollama.com/download/windows", Role::Secondary)
                << ", then run gamma again.\n";
      return false;
    }

    executable = install_ollama();
    if (executable.empty()) return false;
    std::cout << "  " << paint("✔ Ollama installed", Role::Success) << " "
              << paint(executable, Role::Muted) << "\n";
  }

  std::cout << "  " << paint("Ollama is not running", Role::Warn) << paint(" · starting it…", Role::Muted)
            << "\n";
  ui::Spinner spinner;
  spinner.start();
  spinner.set_label("starting Ollama");
  const bool started = start_ollama_server(executable, base_url, out_pid);
  spinner.stop();
  if (!started) {
    std::cout << "  " << paint("✗ Ollama did not answer on " + base_url, Role::Error) << "\n"
              << "  start it manually with " << paint("ollama serve", Role::Secondary)
              << " and run gamma again.\n";
    return false;
  }
  std::cout << "  " << paint("✔ Ollama ready", Role::Success) << " "
            << paint(base_url, Role::Muted) << "\n";
  if (out_pid && *out_pid != 0) {
    g_started_ollama_pid.store(*out_pid, std::memory_order_release);
  }
  return true;
}

// After ensure_ollama_ready returns true, call this to get the PID of the
// Ollama process we started (0 if we didn't start one).
DWORD get_started_ollama_pid() {
  return g_started_ollama_pid.load(std::memory_order_acquire);
}

bool pull_model_with_progress(OllamaClient& client, const std::string& model) {
  const bool tty = stdout_is_tty();
  std::size_t last_width = 0;
  bool success = false;

  std::cout << "  " << paint("pulling", Role::Muted) << " " << paint_bold(model, Role::Highlight)
            << paint("  (first run only — this downloads the weights)", Role::Muted) << "\n";
  try {
    client.pull_model(model, [&](const OllamaClient::PullEvent& event) {
      if (event.status == "success") {
        success = true;
        if (tty) std::cout << "\r\033[2K";
        return;
      }
      if (event.total > 0) {
        const int percent =
            static_cast<int>((event.completed * 100) / event.total);
        if (tty)
          draw_progress("pulling " + model, percent, event.completed, event.total, last_width);
        return;
      }
      // Status-only events ("pulling manifest", "verifying sha256 digest", …).
      if (!tty) return;
      if (event.status.find("manifest") != std::string::npos ||
          event.status.find("verifying") != std::string::npos ||
          event.status.find("writing") != std::string::npos) {
        if (last_width > 0) {
          std::cout << "\r\033[2K";
          last_width = 0;
        }
        std::cout << "  " << paint(event.status, Role::Muted) << "\n";
      }
    });
  } catch (const std::exception& error) {
    if (tty) std::cout << "\r\033[2K";
    std::cout << "  " << paint(std::string("✗ pull failed: ") + error.what(), Role::Error) << "\n";
    return false;
  }

  if (tty && last_width > 0) std::cout << "\r\033[2K";
  if (!success) {
    std::cout << "  " << paint("✗ Ollama did not confirm the pull", Role::Error) << "\n";
    return false;
  }
  std::cout << "  " << paint("✔ model ready", Role::Success) << " "
            << paint_bold(model, Role::Highlight) << "\n";
  return true;
}

bool stdio_is_interactive() {
  return stdin_is_tty() && stdout_is_tty();
}

} // namespace gamma::setup
