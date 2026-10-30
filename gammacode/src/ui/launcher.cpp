#include "ui/launcher.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "config/sessions.hpp"
#include "ui/theme.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <conio.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "agent/loop.hpp"

namespace gamma::ui {

namespace {

// The launcher is a full-screen TUI: all colors flow through the shared
// theme (roles), so the palette stays in one place across the app.
using theme::Role;

std::string bold(const std::string& text) { return theme::paint_bold(text, Role::Highlight); }
std::string dim(const std::string& text) { return theme::paint(text, Role::Muted); }
std::string yellow(const std::string& text) { return theme::paint(text, Role::Warn); }
std::string bright_cyan(const std::string& text) { return theme::paint_bold(text, Role::Accent); }
std::string accent(const std::string& text) { return theme::paint(text, Role::Accent); }
std::string faint(const std::string& text) { return theme::paint(text, Role::Faint); }

void clear_view() {
  // Full repaint: home + clear the viewport and — the belt-and-suspenders —
  // erase the saved-lines (scrollback) region too (`CSI 3J`), so a launcher
  // frame can never stack up in scroll history, even on terminals where the
  // alternate-screen buffer is unavailable. Ignored silently if unsupported.
  std::cout << "\033[H\033[2J\033[3J" << std::flush;
}

std::string repeat(const std::string& glyph, std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out += glyph;
  return out;
}

std::vector<std::string> wrap_pl(const std::string& text, std::size_t width) {
  std::vector<std::string> out;
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = std::min(begin + width, text.size());
    const std::size_t newline = text.find('\n', begin);
    if (newline != std::string::npos && newline <= end) {
      out.push_back(text.substr(begin, newline - begin));
      begin = newline + 1;
      continue;
    }
    if (end < text.size()) {
      const std::size_t space = text.rfind(' ', end);
      if (space > begin + 8) end = space + 1;
    }
    out.push_back(text.substr(begin, end - begin));
    begin = end;
  }
  return out;
}

enum class Key { Up, Down, Left, Right, Home, End, Delete, Enter, Esc, Backspace, Char, Eof };

#ifdef _WIN32
#else
class RawMode {
 public:
  RawMode() {
    tcgetattr(STDIN_FILENO, &original_);
    termios raw = original_;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  }
  ~RawMode() { tcsetattr(STDIN_FILENO, TCSANOW, &original_); }
  RawMode(const RawMode&) = delete;
  RawMode& operator=(const RawMode&) = delete;

 private:
  termios original_{};
};

// Waits up to `ms` for input; true when a byte can be read right away.
bool byte_ready_within(int ms) {
  pollfd fds{STDIN_FILENO, POLLIN, 0};
  return poll(&fds, 1, ms) > 0;
}
#endif

// One keypress. Owns the terminal only while the launcher is up; replxx is
// never active at the same time.
Key read_key(char& ch) {
  ch = 0;
#ifdef _WIN32
  const int c = _getch();
  if (c <= 0) return Key::Eof;
  if (c == 0 || c == 0xE0) { // function/arrow prefix
    const int arrow = _getch();
    if (arrow <= 0) return Key::Eof;
    if (arrow == 72) return Key::Up;
    if (arrow == 80) return Key::Down;
    if (arrow == 75) return Key::Left;
    if (arrow == 77) return Key::Right;
    if (arrow == 71) return Key::Home;
    if (arrow == 79) return Key::End;
    if (arrow == 83) return Key::Delete;
    return Key::Esc;
  }
  if (c == 13 || c == 10) return Key::Enter;
  if (c == 27) return Key::Esc;
  if (c == 8) return Key::Backspace;
  ch = static_cast<char>(c);
  return Key::Char;
#else
  char byte = 0;
  if (::read(STDIN_FILENO, &byte, 1) != 1) return Key::Eof;
  if (byte == '\r' || byte == '\n') return Key::Enter;
  if (byte == 127) return Key::Backspace;
  if (byte != 27) {
    ch = byte;
    return Key::Char;
  }
  // Escape: an escape sequence (*) follows within milliseconds.
  if (!byte_ready_within(30)) return Key::Esc;
  if (::read(STDIN_FILENO, &byte, 1) != 1 || byte != '[') return Key::Esc;
  if (::read(STDIN_FILENO, &byte, 1) != 1) return Key::Eof;
  if (byte == 'A') return Key::Up;
  if (byte == 'B') return Key::Down;
  if (byte == 'C') return Key::Right;
  if (byte == 'D') return Key::Left;
  if (byte == 'H') return Key::Home;
  if (byte == 'F') return Key::End;
  if (byte == '3') { // Delete: "ESC [ 3 ~"
    if (::read(STDIN_FILENO, &byte, 1) != 1 || byte != '~') return Key::Esc;
    return Key::Delete;
  }
  return Key::Esc;
#endif
}

// Scrollable arrow-key list. Returns the chosen index or -1 on back-out.
int menu_engine(const std::string& title,
                const std::vector<std::string>& rows,
                int preselect,
                const std::string& footer) {
  constexpr std::size_t kWindow = 12;
  int highlight = (preselect >= 0 && preselect < static_cast<int>(rows.size())) ? preselect : 0;
  std::size_t scroll_top = 0;
  const std::size_t window = std::min(kWindow, rows.size());

  while (true) {
    clear_view();
    // Menu title in the Accent role; the selected row gets a violet ▸ marker
    // and Highlight text; unselected rows stay Muted against the dark bg.
    std::cout << "  " << bright_cyan(title) << "\n\n";
    for (std::size_t visible = 0; visible < window; ++visible) {
      const std::size_t index = scroll_top + visible;
      if (index >= rows.size()) break;
      const bool selected = static_cast<int>(index) == highlight;
      if (selected) {
        std::cout << "  " << accent("▸") << " " << bold(rows[index]) << "\n";
      } else {
        std::cout << "    " << dim(rows[index]) << "\n";
      }
    }
    if (rows.size() > window) {
      std::cout << faint("    … " + std::to_string(highlight + 1) + "/" +
                         std::to_string(rows.size()) + " · ↑/↓ to scroll")
                << "\n";
    }
    std::cout << "\n  " << dim(footer) << std::flush;

    char ch = 0;
    switch (read_key(ch)) {
      case Key::Up:
        if (highlight > 0) {
          --highlight;
          if (static_cast<std::size_t>(highlight) < scroll_top) scroll_top = highlight;
        }
        break;
      case Key::Down:
        if (highlight + 1 < static_cast<int>(rows.size())) {
          ++highlight;
          if (static_cast<std::size_t>(highlight) >= scroll_top + window) {
            scroll_top = static_cast<std::size_t>(highlight) + 1 - window;
          }
        }
        break;
      case Key::Enter:
        return highlight;
      case Key::Esc:
      case Key::Eof:
        return -1;
      case Key::Backspace:
        break;
      case Key::Char:
      default:
        if (ch == 'q' || ch == 'Q') return -1;
        break;
    }
  }
}

// Single-line editor for the workspace path: the WHOLE value is editable —
// ←/→ move the cursor, Backspace/Delete erase on either side, typing inserts
// at the cursor. Enter validates (must be an existing directory, or empty to
// keep the current one) and saves; Esc backs out untouched. Invalid paths
// stay in the editor with an error line visible, so typos never silently
// discard input.
bool text_prompt(const char* title, const std::string& initial, std::string& value) {
  std::string current = initial;
  std::size_t cursor = current.size(); // joke-free caret: insert position
  std::string error;

  const auto draw = [&]() {
    // Visible width of a byte string: continuation bytes (0b10xxxxxx) take no
    // column on their own.
    const auto columns = [](const std::string& text) {
      std::size_t width = 0;
      for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++width;
      }
      return width;
    };
    // Scroll the INSIDE of the box so the caret is always visible.
    constexpr std::size_t kBoxWidth = 70;
    std::string shown_left = current.substr(0, cursor);
    while (columns(shown_left) + 1 > kBoxWidth) shown_left.erase(0, 1); // 1 = caret column
    std::string shown_right = current.substr(shown_left.size());
    while (columns(shown_left) + columns(shown_right) > kBoxWidth) {
      shown_right.pop_back();
    }
    const std::size_t used = columns(shown_left) + 1 + columns(shown_right);

    std::cout << "\033[2J\033[H";
    std::cout << "  " << bright_cyan(title) << "\n\n";
    std::cout << "  " << faint("╭" + repeat("─", 72) + "╮") << "\n";
    std::cout << "  " << faint("│") << " " << shown_left << accent("▏") << shown_right
              << repeat(" ", kBoxWidth - used + 1) << faint("│") << "\n";
    std::cout << "  " << faint("╰" + repeat("─", 72) + "╯") << "\n";
    std::cout << "  "
              << dim("type a folder · ←/→ move · Backspace/Delete · Enter save · Esc cancel")
              << "\n";
    if (!error.empty()) {
      std::cout << "  " << yellow("✗ " + error) << "\n";
    }
    std::cout.flush();
  };

  draw();
  while (true) {
    char ch = 0;
    switch (read_key(ch)) {
      case Key::Enter: {
        while (!current.empty() && current.back() == ' ') current.pop_back();
        if (current.empty() || std::filesystem::is_directory(current)) {
          value = current;
          clear_view();
          return true;
        }
        error = "not a directory — keep typing, press Esc to cancel";
        draw();
        break;
      }
      case Key::Esc:
      case Key::Eof:
        clear_view();
        return false;
      case Key::Left:
        if (cursor > 0) --cursor;
        error.clear();
        draw();
        break;
      case Key::Right:
        if (cursor < current.size()) ++cursor;
        error.clear();
        draw();
        break;
      case Key::Home:
        cursor = 0;
        draw();
        break;
      case Key::End:
        cursor = current.size();
        draw();
        break;
      case Key::Backspace:
        if (cursor > 0) {
          current.erase(cursor - 1, 1);
          --cursor;
          error.clear();
          draw();
        }
        break;
      case Key::Delete:
        if (cursor < current.size()) {
          current.erase(cursor, 1);
          error.clear();
          draw();
        }
        break;
      case Key::Up:
      case Key::Down:
        break;
      case Key::Char:
      default:
        if (static_cast<unsigned char>(ch) >= 32) {
          current.insert(cursor, 1, ch);
          ++cursor;
          error.clear();
          draw();
        }
        break;
    }
  }
}

std::string mode_row(const Options& options) {
  switch (options.mode) {
    case Mode::Plan: return "plan   · read-only tools (read/list/glob/grep/ask)";
    case Mode::Ask: return "ask    · reads free; writes & shell ask first";
    case Mode::Auto: return "auto   · fully automated: nothing asks (default)";
  }
  return "auto";
}

} // namespace

void clear_screen() {
  clear_view();
}

// Alternate screen buffer for full-screen menus: the VT 1049 pair keeps
// launcher/picker frames OUT of scrollback, so scrolling up after leaving
// shows the REPL transcript — not a wall of old menu screens. Pushed on
// construction, popped on every exit path (RAII, covers early returns).
struct AltBuffer {
  AltBuffer() {
    std::cout << "\033[?1049h\033[H";
    std::cout.flush();
  }
  ~AltBuffer() {
    std::cout << "\033[?1049l";
    std::cout.flush();
  }
};

void print_logo() {
  // The block-art wordmark carries the Accent role; the tagline sits beneath
  // in Muted with the version called out in Highlight.
  std::cout << theme::paint(R"GNU(
   ██████╗  █████╗ ███╗   ███╗███╗   ███╗ █████╗
  ██╔════╝ ██╔══██╗████╗ ████║████╗ ████║██╔══██╗
  ██║  ███╗███████║██╔████╔██║██╔████╔██║███████║
  ██║   ██║██╔══██║██║╚██╔╝██║██║╚██╔╝██║██╔══██╗
  ╚██████╔╝██║  ██║██║ ╚═╝ ██║██║ ╚═╝ ██║██║  ██║
   ╚═════╝ ╚═╝  ╚═╝╚═╝     ╚═╝╚═╝     ╚═╝╚═╝  ╚═╝)GNU",
                            Role::Accent)
            << "\n";
  std::cout << "  " << bold("γ Gamma Code " GAMMA_VERSION)
            << dim("   ·   local-only coding agent · Ollama on 127.0.0.1") << "\n\n";
}

int model_picker(const std::vector<std::string>& models, std::size_t preselect) {
  AltBuffer screen_guard; // /model picker from the REPL also skips scrollback
  const std::size_t safe_preselect = models.empty() ? 0 : std::min(preselect, models.size() - 1);
  return menu_engine("Model · installed on Ollama", models,
                     static_cast<int>(safe_preselect),
                     "↑/↓ move · Enter select · Esc back");
}

bool launcher(Options& options, const std::vector<std::string>& ollama_models) {
#ifndef _WIN32
  const RawMode raw_mode_guard; // restores cooked mode on every exit path
#endif
  // NOTE: no AltBuffer here — main() enters the alternate screen for the whole
  // interactive session, so the launcher and the REPL share one frame.
  theme::set_colors(true); // the launcher is a full-screen TUI — colors are the point
  theme::detect_capabilities();

  std::size_t model_index = 0;
  for (std::size_t i = 0; i < ollama_models.size(); ++i) {
    if (ollama_models[i] == options.model) model_index = i;
  }
  const bool model_installed =
      std::find(ollama_models.begin(), ollama_models.end(), options.model) !=
      ollama_models.end();

  enum Row { Model = 0, ModeRow, Workspace, Sessions, SystemPrompt, StartChat, Exit, RowCount };
  int highlight = Model;

  while (true) {
    clear_view();
    print_logo();

    const std::string model_value =
        options.model + (model_installed ? "" : theme::paint("   (not installed — pick below)", Role::Warn));

    // Rows: label + value; the selected row gets a violet ▌ bar and Highlight
    // text; unselected rows read as Muted/Faint chrome.
    const std::vector<std::string> value_by_row = {
        model_value,
        mode_row(options),
        options.workspace.empty()
            ? std::string("(current dir) ") + std::filesystem::current_path().string()
            : options.workspace,
        std::to_string(sessions::saved_sessions().size()) + " saved · pick one to resume",
        std::string("gamma's system prompt applies automatically — preview below"),
        "enter the " + theme::paint("γ ❯", Role::Accent) + " REPL",
        "",
    };
    for (std::size_t i = 0; i < value_by_row.size(); ++i) {
      static const char* kLabels[RowCount] = {"Model", "Mode", "Workspace", "Sessions",
                                              "System prompt", "Start Chat", "Exit"};
      const bool selected = static_cast<int>(i) == highlight;
      if (selected) {
        std::cout << "  " << accent("▌") << " " << bold(kLabels[i]) << "   "
                  << theme::paint(value_by_row[i], Role::Highlight) << "\n";
      } else {
        std::cout << "    " << faint(kLabels[i]) << "   " << dim(value_by_row[i]) << "\n";
      }
    }

    std::cout << "\n  " << dim("↑/↓ move · Enter select · N new session · q quit") << "\n\n";
    std::cout << dim("  system prompt applied to every turn:") << "\n";
    for (const std::string& line : wrap_pl(system_prompt_text(), 76)) {
      std::cout << faint("    " + line) << "\n";
    }
    std::cout.flush();

    char ch = 0;
    switch (read_key(ch)) {
      case Key::Up:
        if (highlight > Model) --highlight;
        break;
      case Key::Down:
        if (highlight + 1 < static_cast<int>(RowCount)) ++highlight;
        break;
      case Key::Enter: {
        if (highlight == Model) {
          const int picked = model_picker(ollama_models, model_index);
          if (picked >= 0 && static_cast<std::size_t>(picked) < ollama_models.size()) {
            model_index = static_cast<std::size_t>(picked);
            options.model = ollama_models[model_index];
          }
        } else if (highlight == ModeRow) {
          const std::vector<std::string> mode_rows = {
              "plan — read_file / list_dir / glob / grep / ask_user only",
              "ask  — reads free; write_file, edit_file, run_command ask first",
              "auto — fully automated: reads, writes, edits, commands (default)",
          };
          const int picked = menu_engine("Permission mode", mode_rows,
                                         static_cast<int>(options.mode),
                                         "↑/↓ move · Enter select · Esc back");
          if (picked >= 0) options.mode = static_cast<Mode>(picked);
        } else if (highlight == Workspace) {
          std::string candidate;
          const std::string initial =
              options.workspace.empty()
                  ? std::filesystem::current_path().string()
                  : options.workspace;
          // text_prompt only returns true for a valid directory (or empty =
          // keep the current one); invalid input stays in the editor.
          if (text_prompt("Workspace folder", initial, candidate)) {
            options.workspace = candidate; // empty ⇒ keep current dir
          }
        } else if (highlight == Sessions) {
          // Resume menu: everything saved on disk, newest first, plus
          // "New session". Picking a session restores its WHOLE conversation
          // (turns, replies, tool calls and results) and drops the user
          // straight into the REPL; it keeps saving into that same file.
          const std::vector<std::string> files = sessions::saved_sessions();
          std::vector<std::string> rows;
          rows.push_back("New session · a fresh conversation with its own name");
          for (const std::string& file : files) {
            rows.push_back(sessions::session_display_name(file));
          }
          const int picked =
              menu_engine("Sessions · saved conversations", rows, 0,
                          "↑/↓ move · Enter select · Esc back");
          if (picked > 0) { // picked > 0 = an existing session; 0 = keep menu
            options.session_file = files[static_cast<std::size_t>(picked - 1)];
            clear_view();
            return true; // Start the REPL with that conversation restored
          }
        } else if (highlight == SystemPrompt) {
          // informational row — the preview below is the full text already
        } else if (highlight == StartChat) {
          clear_view();
          return true;
        } else if (highlight == Exit) {
          clear_view();
          std::cout << dim("  bye ") << theme::paint("✨", Role::Accent) << "\n";
          return false;
        }
        break;
      }
      case Key::Char:
      default:
        if (ch == 'q' || ch == 'Q') {
          clear_view();
          std::cout << dim("  bye ") << theme::paint("✨", Role::Accent) << "\n";
          return false;
        }
        if (ch == 'n' || ch == 'N') {
          // Hotkey: brand-new session straight into the REPL (Decided: 
          // faster than hunting the Sessions row; options.session_file
          // stays empty so the REPL mints a fresh unique name).
          clear_view();
          return true;
        }
        break;
    }
  }
}

} // namespace gamma::ui
