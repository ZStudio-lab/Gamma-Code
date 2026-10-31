#pragma once

#include <string>
#include <vector>

namespace gamma::ui::theme {

// ── Gamma Aurora ────────────────────────────────────────────────────────────
// A role-based output palette, ours: violet/teal/rose on near-black, tuned
// unlike any other agent's UI. All console drawing flows through paint() so
// NO_COLOR, captured output and 256-color hosts degrade to plain text.
enum class Role {
  Text,       // body copy
  Muted,      // secondary copy
  Faint,      // borders, rules, chrome, hints
  Accent,     // primary violet — γ, bullets, titles, spinner
  Secondary,  // teal — tool names, values, inline code
  Success,    // soft green
  Error,      // rose
  Warn,       // amber
  Highlight,  // bold near-white — selected rows, key values
  Code,       // fenced code body
};

// True when the output stream should carry ANSI at all (init_terminal decides;
// NO_COLOR turns everything off; the launcher forces it on for its TUI).
bool colors_enabled();
void set_colors(bool enabled);

// True when 24-bit color is safe (Windows Terminal / ConEmu / COLORTERM);
// otherwise a 16-color fallback keeps classic conhost legible.
bool truecolor();

// One call turns the terminal-paint settings on: init_terminal() invokes it.
void detect_capabilities();

// The core. Every visible surface goes through this.
std::string paint(const std::string& text, Role role);

// Bold variant (bold + color).
std::string paint_bold(const std::string& text, Role role);

// ── width-aware frame renderer ──────────────────────────────────────────────
// ... box-drawing borders in Faint, ANSI-blind column widths, and lines that
// may already contain styled spans (the padding is computed from the visible
// width, so it never misaligns).

// Returns the decorated multi-line frame; content lines must NOT end with \n.
std::string frame(const std::vector<std::string>& content_lines);

// Frame with a left-hand title tab ("| permission ...") — used by the
// permission gate and the session restore bar.
std::string frame_titled(const std::string& title_plain, Role title_role,
                         const std::vector<std::string>& content_lines);

// Visible width of a possibly-styled string (ANSI escapes stripped first).
std::size_t visible_width(const std::string& text);

// ── markdown-lite ───────────────────────────────────────────────────────────
// Assistant messages: fenced code blocks get a dim left rule + code color
// (up to a line cap), inline `snippets` get the secondary color, and bullet
// lists get dim markers. Everything else paints as plain Text.

// Renders markdown-lite text; returns full styled lines (no trailing \n).
std::vector<std::string> render_markdown(const std::string& text);

// Prints it with an Accent bullet and line indentation — the assistant turn.
void print_markdown_reply(const std::string& text);

// Prints the REST of a reply whose beginning was already streamed live: same
// markdown-lite styling and indentation, but no second bullet. Used when a
// stream stopped at a code fence / tool-call marker and the tail is rendered
// once the message is complete.
void print_markdown_continuation(const std::string& text);

} // namespace gamma::ui::theme
