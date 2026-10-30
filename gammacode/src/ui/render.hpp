#pragma once

#include <atomic>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

namespace gamma::ui {

// One-time console setup: enables ANSI/VT on Windows and colors detection.
// Call once at the top of main() before any output. Also names the console
// window "Gamma Code" and (on Windows) shows the embedded app icon.
void init_terminal();

// Console window title: "Gamma Code", with a live "— <status>" suffix while
// the assistant works ("thinking…", "reading a file…", "editing a file…").
// An empty status restores the bare title; deduplicated so per-chunk calls
// are cheap. No-op when there is no console window.
void set_status_title(const std::string& status);

// Puts the console window's title back to what it was before gamma changed
// it. Call on every exit path so the hosting shell window is left clean.
void restore_window_title();

// Alternate screen buffer for interactive sessions: while active, the
// console keeps gamma's frames entirely separate from the shell's history,
// so scrolling up shows ONLY the current session — nothing from before
// gamma started. enter before the interactive portion of the session and
// leave on every exit path; nested use (e.g. an in-session picker) is fine.
void enter_alt_screen();
void leave_alt_screen();

// The REPL prompt, colored to match the UI (plain "γ ❯ " without colors).
std::string prompt_string();

// Claude Code-style header: bordered title box plus a dim model/mode/
// workspace/budget line.
void print_banner(const Options& options);

// "⏺ read_file(path=…)" breadcrumbs with a dim argument summary.
void print_tool_call(const std::string& name, const nlohmann::json& arguments);

// Dim "  └ …" preview (first 3 lines); full error in red on failure.
void print_tool_result(bool ok, const std::string& output, const std::string& error);

// The model's final text, prefixed with a cyan bullet.
void print_assistant_message(const std::string& text);

// Approval prompt used by the permission gate: a bordered proposal box
// (Claude Code-style) then a y/N read on stdin.
bool ask_yes_no(const std::string& proposal);

// Human-facing description of a proposed action. run_command shows the full
// command; edit_file shows the diff (step 2); anything else gets a summary.
std::string proposal_text(const std::string& tool_name, const nlohmann::json& arguments);

// Animated "✻ writing a file… (3s)" indicator on a background thread while
// gamma waits on Ollama. The label CHANGES while the streamed response
// arrives (streaming_status maps the accumulating text to a verb), so the
// indicator always says what the model is doing right now — never a stale
// generic "thinking…". Only runs while a blocking curl call is in flight and
// is always joined before any replxx call or approval prompt; it also stays
// silent when stderr is not a TTY so captured output stays clean.
class Spinner {
 public:
  Spinner() = default;
  ~Spinner(); // idempotent stop
  Spinner(const Spinner&) = delete;
  Spinner& operator=(const Spinner&) = delete;

  void start();
  void stop();

  // Thread-safe; takes effect on the next animation frame.
  void set_label(const std::string& label);

 private:
  std::thread thread_;
  std::atomic<bool> spinning_{false};
  std::mutex label_mutex_;
  std::string label_;
};

// Stop-output keys while the model streams. While a StreamStopWindow is
// alive, the console input is unbuffered so ESC (and Ctrl+C) arrive as plain
// keystrokes that poll_stop_key() reports; the original input mode is
// restored on destruction, so the REPL is never left in the raw state.
class StreamStopWindow {
 public:
  StreamStopWindow();
  ~StreamStopWindow();
  StreamStopWindow(const StreamStopWindow&) = delete;
  StreamStopWindow& operator=(const StreamStopWindow&) = delete;

 private:
#ifdef _WIN32
  void* input_handle_ = nullptr; // HANDLE
  unsigned long saved_mode_ = 0;
  bool grabbed_ = false;
#endif
};

// True when the user pressed ESC or Ctrl+C since the last streamed answer
// started. Latched: once true it stays true until the next StreamStopWindow
// opens. Call it repeatedly from the streaming progress hook.
bool stop_output_pressed();

// Maps the assistant text accumulated so far (from a streamed /api/chat
// response) to a one-line status: the most recent tool the model is building
// a call for, or plain "thinking…".
std::string stream_status(const std::string& accumulated_content);

} // namespace gamma::ui
