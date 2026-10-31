#include "ui/theme.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace gamma::ui::theme {

namespace {

bool g_colors = false;
bool g_truecolor = false;

// Gamma Aurora: violet leads, teal supports, rose/amber speak for states.
// The fallback column keeps classic 16-color hosts legible.
struct Spec {
  int r, g, b;
  const char* ansi; // 16-color fallback
};

Spec spec_for(Role role) {
  switch (role) {
    case Role::Text:      return {205, 214, 228, "\033[37m"};
    case Role::Muted:     return {139, 148, 172, "\033[90m"};
    case Role::Faint:     return {78, 86, 107, "\033[90m"};
    case Role::Accent:    return {162, 139, 255, "\033[95m"};
    case Role::Secondary: return {70, 216, 207, "\033[96m"};
    case Role::Success:   return {79, 221, 146, "\033[92m"};
    case Role::Error:     return {255, 112, 147, "\033[91m"};
    case Role::Warn:      return {255, 199, 107, "\033[93m"};
    case Role::Highlight: return {242, 243, 248, "\033[97m"};
    case Role::Code:      return {168, 183, 216, "\033[96m"};
  }
  return {205, 214, 228, ""};
}

} // namespace

bool colors_enabled() { return g_colors; }
void set_colors(bool enabled) { g_colors = enabled; }

bool truecolor() { return g_truecolor; }

void detect_capabilities() {
  g_truecolor = false;
  const char* wt = std::getenv("WT_SESSION");
  const char* cterm = std::getenv("COLORTERM");
  const char* conemu = std::getenv("ConEmuANSI");
  if (wt != nullptr && wt[0] != '\0') g_truecolor = true;
  if (cterm != nullptr &&
      (std::strstr(cterm, "truecolor") != nullptr ||
       std::strstr(cterm, "24bit") != nullptr)) {
    g_truecolor = true;
  }
  if (conemu != nullptr && conemu[0] == 'O') g_truecolor = true;
}

std::string paint(const std::string& text, Role role) {
  if (text.empty() || !g_colors) return text;
  const Spec spec = spec_for(role);
  if (g_truecolor) {
    return "\033[38;2;" + std::to_string(spec.r) + ";" + std::to_string(spec.g) +
           ";" + std::to_string(spec.b) + "m" + text + "\033[0m";
  }
  return std::string(spec.ansi) + text + "\033[0m";
}

std::string paint_bold(const std::string& text, Role role) {
  if (text.empty() || !g_colors) return text;
  const Spec spec = spec_for(role);
  if (g_truecolor) {
    return "\033[1;38;2;" + std::to_string(spec.r) + ";" + std::to_string(spec.g) +
           ";" + std::to_string(spec.b) + "m" + text + "\033[0m";
  }
  // "\033[95m" -> "\033[1;95m": bold + the same color inside one escape.
  std::string open = spec.ansi;
  if (open.size() >= 3) open = open.substr(0, 2) + "1;" + open.substr(2);
  return open + text + "\033[0m";
}

std::size_t visible_width(const std::string& text) {
  std::size_t width = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\033') {
      std::size_t j = i + 1;
      if (j < text.size() && text[j] == '[') {
        ++j;
        while (j < text.size() && !(text[j] >= '@' && text[j] <= '~')) ++j;
        if (j < text.size()) ++j; // consume the terminator
        i = j;
        continue;
      }
      ++i;
      continue;
    }
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++width;
    ++i;
  }
  return width;
}

std::string frame(const std::vector<std::string>& content_lines) {
  std::size_t width = 0;
  for (const std::string& line : content_lines) {
    width = std::max(width, visible_width(line));
  }

  const std::string ruled(width + 2, '-'); // interior margins are 1 space each
  std::string out;
  out += paint("+" + ruled + "+", Role::Faint) + "\n";
  for (const std::string& line : content_lines) {
    const std::string pad(width - visible_width(line), ' ');
    out += paint("|", Role::Faint) + " " + line + pad + " " +
           paint("|", Role::Faint) + "\n";
  }
  out += paint("+" + ruled + "+", Role::Faint);
  return out;
}

std::string frame_titled(const std::string& title_plain, Role title_role,
                         const std::vector<std::string>& content_lines) {
  std::size_t width = std::max(visible_width(title_plain), std::size_t{4});
  for (const std::string& line : content_lines) {
    width = std::max(width, visible_width(line));
  }

  const std::string ruled(width + 2, '-');
  const std::string tab = paint_bold(" " + title_plain + " ", title_role);
  const std::size_t rest = width + 2 - (visible_width(title_plain) + 2);

  std::string out;
  out += paint("+--", Role::Faint) + tab +
         paint(std::string(rest, '-') + "+", Role::Faint) + "\n";
  for (const std::string& line : content_lines) {
    const std::string pad(width - visible_width(line), ' ');
    out += paint("|", Role::Faint) + " " + line + pad + " " +
           paint("|", Role::Faint) + "\n";
  }
  out += paint("+" + ruled + "+", Role::Faint);
  return out;
}

// Renders text. Lines come back with NO indentation — the caller indents.
std::vector<std::string> render_markdown(const std::string& text) {
  std::vector<std::string> lines;
  constexpr std::size_t kMaxTotal = 320;   // whole-reply cap
  constexpr std::size_t kMaxCodeLines = 60; // per fenced block cap

  std::size_t begin = 0;
  bool in_code = false;
  std::size_t code_lines = 0;

  while (begin <= text.size() && lines.size() < kMaxTotal) {
    const std::size_t end = text.find('\n', begin);
    const std::string raw = (end == std::string::npos)
                                ? text.substr(begin)
                                : text.substr(begin, end - begin);
    begin = (end != std::string::npos) ? end + 1 : text.size() + 1;

    const bool fence = raw.starts_with("```");
    if (fence) {
      in_code = !in_code;
      code_lines = 0;
      lines.push_back(paint("|", Role::Faint));
      continue;
    }
    if (in_code) {
      if (++code_lines > kMaxCodeLines) continue;
      lines.push_back(paint("| ", Role::Faint) + paint(raw, Role::Code));
      continue;
    }
    if (raw.starts_with("#")) continue;
    if (raw.starts_with("- ") || raw.starts_with("* ")) {
      lines.push_back(paint("-", Role::Faint) + " " + raw.substr(2));
      continue;
    }
    if (raw.empty()) {
      lines.push_back("");
      continue;
    }
    // Inline `backtick` runs: paint each snippet in Secondary.
    std::size_t cursor = 0;
    std::size_t taken = 0;
    std::string styled;
    while (true) {
      const std::size_t open = raw.find('`', cursor);
      if (open == std::string::npos) break;
      const std::size_t close = raw.find('`', open + 1);
      if (close == std::string::npos) break;
      styled += raw.substr(cursor, open - cursor);
      styled += paint(raw.substr(open + 1, close - open - 1), Role::Secondary);
      cursor = close + 1;
      taken = cursor;
    }
    styled += raw.substr(taken);
    lines.push_back(styled);
  }
  while (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

void print_markdown_reply(const std::string& text) {
  const std::vector<std::string> lines = render_markdown(text);
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i == 0) {
      std::cout << "  " << paint("\xE2\x8F\xBA", Role::Accent) << " " << lines[0] << "\n";
      continue;
    }
    std::cout << (lines[i].empty() ? std::string("") : ("  " + lines[i])) << "\n";
  }
}

void print_markdown_continuation(const std::string& text) {
  for (const std::string& line : render_markdown(text)) {
    std::cout << (line.empty() ? std::string("") : ("  " + line)) << "\n";
  }
}

} // namespace gamma::ui::theme
