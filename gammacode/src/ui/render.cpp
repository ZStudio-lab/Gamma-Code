#include "ui/render.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <conio.h>
#include <io.h>
#define GAMMA_ISATTY _isatty
#define GAMMA_FILENO _fileno
#else
#include <unistd.h>
#define GAMMA_ISATTY isatty
#define GAMMA_FILENO fileno
#endif

#ifdef _WIN32
#include <Windows.h> // (conio/io included above) — second include is harmless
#endif

namespace gamma::ui {

// Defined next to stream_status; used by print_tool_call for the window title.
const char* tool_verb(const std::string& name);

namespace {

bool g_colors = false;
bool g_spinner_allowed = false; // colors AND real terminal (not captured/redirected)

#ifdef _WIN32
// Console-title state: the original title is remembered so the hosting shell
// window can be left clean when gamma exits; the last sent title dedupes the
// per-chunk status calls so SetConsoleTitleW only runs on real changes.
std::mutex g_title_mutex;
std::wstring g_last_title;
std::wstring g_original_title;

std::wstring utf8_to_utf16(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      wide.data(), size);
  wide.resize(static_cast<std::size_t>(size - 1)); // drop the counted terminator
  return wide;
}

void set_title_locked(const std::wstring& title) {
  if (title == g_last_title) return; // per-chunk dedup
  g_last_title = title;
  SetConsoleTitleW(title.c_str());
}
#endif

std::string wrap(const char* code, const std::string& text) {
  if (!g_colors) return text;
  return std::string(code) + text + "\033[0m";
}

std::string bold(const std::string& text) { return wrap("\033[1m", text); }
std::string dim(const std::string& text) { return wrap("\033[90m", text); }
std::string red(const std::string& text) { return wrap("\033[31m", text); }
std::string green(const std::string& text) { return wrap("\033[32m", text); }
std::string yellow(const std::string& text) { return wrap("\033[33m", text); }
std::string cyan(const std::string& text) { return wrap("\033[36m", text); }
std::string bright_cyan(const std::string& text) { return wrap("\033[1;96m", text); }

// Display width in columns (counts UTF-8 code points).
std::size_t display_width(const std::string& text) {
  std::size_t columns = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++columns; // skip continuation bytes
  }
  return columns;
}

// Flattens tool arguments for a one-line display. Values are truncated —
// this is a summary for humans; the model always sees the full arguments.
std::string summarize_arguments(const nlohmann::json& arguments) {
  std::string text;
  if (!arguments.is_object()) return text;
  for (auto it = arguments.begin(); it != arguments.end(); ++it) {
    if (!text.empty()) text += ", ";
    std::string value =
        it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    if (value.size() > 64) value = value.substr(0, 61) + "…";
    text += it.key() + "=" + value;
  }
  return text;
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < text.size()) {
    const std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      lines.push_back(text.substr(begin));
      break;
    }
    lines.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

std::string repeat(const std::string& glyph, std::size_t count) {
  std::string out;
  out.reserve(glyph.size() * count);
  for (std::size_t i = 0; i < count; ++i) out += glyph;
  return out;
}

// --- unified diff preview for the permission box ------------------------------

constexpr std::size_t kDiffContextLines = 3; // ± context around a hunk
constexpr std::size_t kMaxDiffLines = 200;  // keep the prompt box bounded

bool read_file_text(const std::string& path, std::string& content) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

std::size_t count_newlines(const std::string& text, std::size_t begin, std::size_t length) {
  std::size_t count = 0;
  const std::size_t end = std::min(begin + length, text.size());
  for (std::size_t i = begin; i < end; ++i) {
    if (text[i] == '\n') ++count;
  }
  return count;
}

// Classic single-hunk unified diff of one replacement (old_string appears
// exactly once in `content` — the caller verifies). Lines get color: "-"
// red, "+" green, "@@" yellow. Output capped at kMaxDiffLines.
std::string build_edit_diff(const std::string& content, const std::string& old_string,
                            const std::string& new_string, bool& ok) {
  ok = false;
  if (old_string.empty()) return "";

  const std::size_t occurrences = [&] {
    std::size_t count = 0;
    if (content.find(old_string) == std::string::npos) return count;
    for (std::size_t pos = content.find(old_string); pos != std::string::npos;
         pos = content.find(old_string, pos + 1)) {
      ++count;
    }
    return count;
  }();
  if (occurrences != 1) return ""; // caller falls back to a plain message

  const std::size_t at = content.find(old_string);
  const std::vector<std::string> file_lines = split_lines(content);
  const std::size_t total = file_lines.size();
  const std::vector<std::string> removed = split_lines(old_string);
  const std::vector<std::string> added = split_lines(new_string);

  // 1-based span of lines touched by the replacement.
  const std::size_t first_line = count_newlines(content, 0, at) + 1;
  const std::size_t last_line = first_line + count_newlines(content, at, old_string.size());

  // Unchanged text on the same line before/after the match (partial-line hits).
  const std::size_t line_start = (at == 0) ? 0 : content.rfind('\n', at - 1) + 1;
  const std::string head = content.substr(line_start, at - line_start);
  const std::size_t tail_pos = at + old_string.size();
  std::string tail;
  if (tail_pos < content.size() && content[tail_pos - 1] != '\n') {
    // Tail lives on the same line only when the match didn't end on a newline.
    const std::size_t nl = content.find('\n', tail_pos);
    tail = content.substr(tail_pos, (nl == std::string::npos ? content.size() : nl) - tail_pos);
  }

  std::size_t lo = (first_line > kDiffContextLines) ? first_line - kDiffContextLines : 1;
  std::size_t hi = std::min(total, last_line + kDiffContextLines);

  std::vector<std::string> diff;
  for (std::size_t i = lo; i < first_line; ++i) {
    diff.push_back("  " + file_lines[i - 1]);
  }
  if (!head.empty()) diff.push_back("  " + head);
  for (const std::string& line : removed) {
    diff.push_back(red("- " + line));
  }
  for (const std::string& line : added) {
    diff.push_back(green("+ " + line));
  }
  if (!tail.empty()) diff.push_back("  " + tail);
  if (tail.empty() && last_line < total) {
    for (std::size_t i = last_line; i < hi; ++i) {
      diff.push_back("  " + file_lines[i]); // file_lines[i] is line i+1
    }
  }

  // Header drawn last so the counts reflect what is actually shown.
  const std::size_t extra =
      (head.empty() ? 0 : 1) + (tail.empty() ? (hi - last_line) : 1);
  const std::size_t old_count = (first_line - lo) + removed.size() + extra;
  const std::size_t new_count = (first_line - lo) + added.size() + extra;
  diff.insert(diff.begin(),
              yellow("@@ -" + std::to_string(lo) + "," + std::to_string(old_count) + " +" +
                     std::to_string(lo) + "," + std::to_string(new_count) + " @@"));

  ok = true;
  std::string out;
  for (std::size_t i = 0; i < diff.size() && i <= kMaxDiffLines; ++i) {
    if (i == kMaxDiffLines) {
      out += dim("… (" + std::to_string(diff.size() - i) + " more diff lines truncated)") + "\n";
      break;
    }
    out += diff[i] + "\n";
  }
  return out;
}

} // namespace

void init_terminal() {
#ifdef _WIN32
  // Enable ANSI escape processing for classic Windows consoles.
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode = 0;
  if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(65001); // UTF-8 for γ, ✻, ⏺
  }
  // Own the console window: remember its original title, then name the
  // window after us (the embedded icon covers the taskbar/Explorer).
  WCHAR original[1024];
  const DWORD copied = GetConsoleTitleW(original, 1024);
  {
    const std::lock_guard<std::mutex> lock(g_title_mutex);
    g_original_title = (copied > 0) ? std::wstring(original, copied)
                                    : std::wstring(L"Command Prompt");
    set_title_locked(utf8_to_utf16("Gamma Code"));
  }
#endif
  const char* no_color = std::getenv("NO_COLOR");
  g_colors = (no_color == nullptr || no_color[0] == '\0');
  g_spinner_allowed = g_colors && (GAMMA_ISATTY(GAMMA_FILENO(stderr)) != 0);
}

void set_status_title(const std::string& status) {
#ifdef _WIN32
  // Trailing guard space: console hosts have been observed eating the final
  // character of a freshly set title; the space takes the hit so the visible
  // text never loses a letter (the same trick classic shells use).
  std::string text = status.empty() ? "Gamma Code " : "Gamma Code — " + status;
  const std::wstring wide = utf8_to_utf16(text + " ");
  const std::lock_guard<std::mutex> lock(g_title_mutex);
  set_title_locked(wide);
#endif
}

void restore_window_title() {
#ifdef _WIN32
  const std::lock_guard<std::mutex> lock(g_title_mutex);
  set_title_locked(g_original_title);
#endif
}

void enter_alt_screen() {
  if (!GAMMA_ISATTY(GAMMA_FILENO(stdout))) return;
  std::cout << "\033[?1049h\033[H"; // jump to the alt buffer + home cursor
  std::cout.flush();
}

void leave_alt_screen() {
  if (!GAMMA_ISATTY(GAMMA_FILENO(stdout))) return;
  std::cout << "\033[?1049l"; // back to the shell's own screen
  std::cout.flush();
}

std::string prompt_string() {
  // Dynamic: the selected folder leads the prompt, so you always see where
  // tools are rooted. Falls back to just "γ ❯" when leaf extraction fails.
  std::string leaf = workspace_root().filename().string();
  if (leaf.empty()) {
    leaf = workspace_root().string();
    if (leaf.size() > 24) leaf = "…" + leaf.substr(leaf.size() - 23);
  }
  if (leaf.size() > 24) {
    leaf = "…" + leaf.substr(leaf.size() - 23);
  }
  return yellow("⟨" + leaf + "⟩") + " " + bright_cyan("γ ❯") + " ";
}

void print_banner(const Options& options) {
  const std::string title = "  Gamma Code  —  " + std::string(GAMMA_VERSION) + "  ";
  const std::string subtitle = "  local-only coding agent · Ollama on 127.0.0.1 only  ";
  const std::size_t width = std::max(display_width(title), display_width(subtitle));

  std::cout << yellow("╭" + repeat("─", width) + "╮") << "\n";
  std::cout << yellow("│") << bright_cyan(bold(title))
            << repeat(" ", width - display_width(title)) << yellow("│") << "\n";
  std::cout << yellow("│") << dim(subtitle)
            << repeat(" ", width - display_width(subtitle)) << yellow("│") << "\n";
  std::cout << yellow("╰" + repeat("─", width) + "╯") << "\n";
  std::cout << dim("  model ") << bold(options.model)
            << dim("  ·  mode ") << cyan(mode_name(options.mode))
            << dim("  ·  budget ") << std::to_string(budget_tokens(options))
            << dim(" tokens") << "\n";
  if (!options.workspace.empty()) {
    std::cout << dim("  workspace ") << options.workspace << "\n";
  }
  std::cout << dim("  /help for commands · fully automated: tools run without prompts") << "\n";
}

void print_tool_call(const std::string& name, const nlohmann::json& arguments) {
  // The window title mirrors execution, not just streaming.
  set_status_title(tool_verb(name) ? std::string(tool_verb(name)) : "running " + name + "…");
  const std::string summary = summarize_arguments(arguments);
  std::cout << "  " << bright_cyan("⏺") << " " << bold(name);
  if (!summary.empty()) std::cout << dim("(" + summary + ")");
  std::cout << "\n";
}

void print_tool_result(bool ok, const std::string& output, const std::string& error) {
  set_status_title(""); // tool finished — back to the bare title
  if (!ok) {
    std::string shown = error.empty() ? "failed" : error;
    if (shown.size() > 300) shown = shown.substr(0, 297) + "…";
    std::cout << "  " << red("✗") << " " << red(shown) << "\n";
    return;
  }
  const std::vector<std::string> lines = split_lines(output);
  const std::size_t shown = std::min<std::size_t>(lines.size(), 3);
  for (std::size_t i = 0; i < shown; ++i) {
    std::cout << dim("  └ " + lines[i]) << "\n";
  }
  if (lines.size() > shown) {
    std::cout << dim("  └ … (+" + std::to_string(lines.size() - shown) + " more lines)") << "\n";
  }
}

void print_assistant_message(const std::string& text) {
  std::cout << "  " << bright_cyan("⏺") << " " << text << "\n";
}

bool ask_yes_no(const std::string& proposal) {
  const std::vector<std::string> lines = split_lines(proposal);
  std::size_t width = 18; // room for the "permission" label
  for (const std::string& line : lines) {
    width = std::max(width, display_width(line));
  }

  std::cout << "\n";
  std::cout << yellow("╭─ permission " + repeat("─", width - 12) + "╮") << "\n";
  for (const std::string& line : lines) {
    const std::string pad = repeat(" ", width - display_width(line));
    std::cout << yellow("│") << " " << line << pad << " " << yellow("│") << "\n";
  }
  std::cout << yellow("╰" + repeat("─", width + 2) + "╯") << "\n";
  std::cout << cyan("  allow? ") << dim("[y/N] ") << std::flush;

  std::string answer;
  if (!std::getline(std::cin, answer)) return false; // EOF ⇒ no
  return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
}

std::string proposal_text(const std::string& tool_name, const nlohmann::json& arguments) {
  if (tool_name == "run_command" && arguments.contains("cmd") && arguments["cmd"].is_string()) {
    return "gamma wants to run command:\n" + bold(arguments["cmd"].get<std::string>());
  }
  if (tool_name == "write_file" && arguments.contains("path") && arguments["path"].is_string()) {
    std::string text = "gamma wants to write file:\n" + bold(arguments["path"].get<std::string>());
    if (arguments.contains("content") && arguments["content"].is_string()) {
      const std::string content = arguments["content"].get<std::string>();
      const std::vector<std::string> lines = split_lines(content);
      text += "\n" + dim(std::to_string(content.size()) + " bytes · " +
                         std::to_string(lines.size()) + " lines");
      // Overwriting an existing file? Show a capped whole-file diff; for a new
      // file a short head preview is enough.
      std::string existing;
      if (read_file_text(arguments["path"].get<std::string>(), existing)) {
        bool diff_ok = false;
        const std::string diff = build_edit_diff(existing, existing, content, diff_ok);
        if (diff_ok) {
          text += "\n" + diff;
          return text;
        }
      }
      const std::size_t preview = std::min<std::size_t>(lines.size(), 5);
      for (std::size_t i = 0; i < preview; ++i) {
        text += "\n" + dim("  + ") + lines[i];
      }
    }
    return text;
  }
  if (tool_name == "edit_file" && arguments.contains("path") && arguments["path"].is_string() &&
      arguments.contains("old_string") && arguments["old_string"].is_string() &&
      arguments.contains("new_string") && arguments["new_string"].is_string()) {
    const std::string path = arguments["path"].get<std::string>();
    const std::string old_string = arguments["old_string"].get<std::string>();
    const std::string new_string = arguments["new_string"].get<std::string>();
    std::string text = "gamma wants to edit file:\n" + bold(path);

    std::string content;
    if (old_string.empty()) {
      text += "\n" + dim("(empty old_string — will be rejected)");
      return text;
    }
    if (!read_file_text(path, content)) {
      text += "\n" + dim("(file unreadable — the edit itself will fail)");
      return text;
    }
    bool diff_ok = false;
    const std::string diff = build_edit_diff(content, old_string, new_string, diff_ok);
    if (diff_ok) {
      text += "\n" + diff;
      return text;
    }
    // old_string missing / not unique: the exact reason comes back from the
    // tool on execution; here we just do not show a diff.
    text += "\n" + dim("(preview unavailable: old_string not found exactly once)");
    return text;
  }
  return "gamma wants to call " + bold(tool_name + "(" + summarize_arguments(arguments) + ")");
}

// Verb per tool, shown while the model builds/executes that call. Returns
// nullptr for names with no dedicated phrase.
const char* tool_verb(const std::string& name) {
  static constexpr struct Verb {
    const char* name;
    const char* label;
  } kVerbs[] = {
      {"read_file", "reading a file…"},
      {"write_file", "writing a file…"},
      {"edit_file", "editing a file…"},
      {"list_dir", "listing a folder…"},
      {"glob", "searching filenames…"},
      {"grep", "searching text…"},
      {"run_command", "running a command…"},
  };
  for (const Verb& verb : kVerbs) {
    if (name == verb.name) return verb.label;
  }
  return nullptr;
}

std::string stream_status(const std::string& content) {
  // Newest tool-name mention wins: the model may plan several calls in one
  // response, and the stream end reflects the latest one it is inside.
  std::size_t at = content.rfind("\"name\"");
  while (at != std::string::npos) {
    const std::size_t colon = content.find(':', at + 6);
    if (colon != std::string::npos) {
      const std::size_t q1 = content.find('"', colon + 1);
      if (q1 != std::string::npos) {
        const std::size_t q2 = content.find('"', q1 + 1);
        if (q2 != std::string::npos) {
          const std::string name = content.substr(q1 + 1, q2 - q1 - 1);
          if (const char* verb = tool_verb(name)) return verb;
          return "calling " + name + "…";
        }
      }
    }
    if (at == 0) break;
    at = content.rfind("\"name\"", at - 1);
  }

  // A fence/tool-call marker without a name yet means one is forming.
  if (content.find("```") != std::string::npos ||
      content.find("<tool_call>") != std::string::npos) {
    return "composing a tool call…";
  }
  return "thinking…";
}

Spinner::~Spinner() {
  stop();
}

void Spinner::set_label(const std::string& label) {
  {
    const std::lock_guard<std::mutex> lock(label_mutex_);
    label_ = label;
  }
  set_status_title(label); // mirror the live phase in the window title
}

void Spinner::start() {
  if (!g_spinner_allowed) return; // captured/non-TTY output: keep logs clean
  if (spinning_.exchange(true)) return;
  {
    const std::lock_guard<std::mutex> lock(label_mutex_);
    label_ = "thinking…";
  }
  set_status_title("thinking…");
  const auto started = std::chrono::steady_clock::now();
  thread_ = std::thread(
      [](Spinner* self, std::chrono::steady_clock::time_point begin) {
        static constexpr const char* kFrames[4] = {"✻", "✦", "✶", "✷"};
        int frame = 0;
        while (self->spinning_.load()) {
          long long seconds =
              std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now() - begin)
                  .count();
          char buffer[32];
          std::snprintf(buffer, sizeof(buffer), "%lld", seconds);
          std::string label;
          {
            const std::lock_guard<std::mutex> lock(self->label_mutex_);
            label = self->label_;
          }
          std::fprintf(stderr, "\r%s %s (%ss)",
                       yellow(kFrames[frame % 4]).c_str(), label.c_str(), buffer);
          std::fflush(stderr);
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          ++frame;
        }
      },
      this, started);
}

void Spinner::stop() {
  if (!spinning_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  set_status_title(""); // request over — back to the bare window title
  if (g_colors) {
    std::fprintf(stderr, "\r\033[2K"); // wipe the indicator line
    std::fflush(stderr);
  }
}

// --- stop-output keys ----------------------------------------------------------

#ifdef _WIN32
bool g_stop_latched = false; // one ESC press stops the WHOLE answer
#endif

StreamStopWindow::StreamStopWindow() {
#ifdef _WIN32
  g_stop_latched = false; // fresh request, fresh stop latch
  HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode)) {
    // Unbuffered + unprocessed input: Ctrl+C/Ctrl+Break stop acting as
    // process killers and appear as plain keystrokes while streaming.
    SetConsoleMode(input, mode & ~(ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT |
                                   ENABLE_ECHO_INPUT));
    input_handle_ = input;
    saved_mode_ = mode;
    grabbed_ = true;
  }
#endif
}

StreamStopWindow::~StreamStopWindow() {
#ifdef _WIN32
  if (grabbed_) SetConsoleMode(static_cast<HANDLE>(input_handle_), saved_mode_);
#endif
}

bool stop_output_pressed() {
#ifdef _WIN32
  if (g_stop_latched) return true;
  if (_kbhit()) {
    const int code = _getch();
    if (code == 27 || code == 3) { // ESC or Ctrl+C
      g_stop_latched = true;
      return true;
    }
    if (code == 0 || code == 224) {
      if (_kbhit()) (void)_getch(); // swallow the arrow/function second byte
    }
  }
  return false;
#else
  return false;
#endif
}

} // namespace gamma::ui
