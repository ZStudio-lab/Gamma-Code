#include "tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gamma::tools {

namespace {

namespace fs = std::filesystem;

// Output budgets, mirroring what good agent harnesses do: no single tool call
// may flood the context window. Oversized results are cut at a line boundary
// with an explicit "how to continue" hint instead of silently truncating.
constexpr std::size_t kMaxOutputBytes = 50 * 1024;  // 50 KB per tool result
constexpr std::size_t kMaxLineChars = 2000;         // per-line clip (minified bundles)

// Traversal budgets for the scan tools.
constexpr std::size_t kGlobMaxHits = 500;
constexpr std::size_t kGrepMaxHits = 200;
constexpr std::size_t kGrepMaxHitsPerFile = 50;
constexpr std::uintmax_t kGrepMaxFileSize = 5 * 1024 * 1024;  // skip huge files
constexpr std::size_t kBinarySampleBytes = 8192;

std::string read_error_for(const std::string& path) {
  return "cannot open '" + path + "' (missing or unreadable)";
}

std::string trimmed(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// Reads a whole file as bytes. Empty ok flag on success.
bool read_whole_file(const std::string& path, std::string& content) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

/* ───────────────── binary detection (read/grep guard) ───────────────── */

bool binary_extension(const std::string& path) {
  static const std::set<std::string> kBinary{
      ".zip", ".tar", ".gz", ".7z", ".exe",  ".dll", ".so",   ".dylib", ".lib",
      ".a",   ".o",   ".obj", ".bin", ".dat", ".class", ".jar", ".war",  ".wasm",
      ".pyc", ".pyo", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".ico",  ".pdf",
      ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".mp3", ".mp4",  ".avi",
      ".mov", ".ttf", ".otf", ".woff", ".woff2", ".sqlite", ".db"};
  const std::size_t dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext;
  for (std::size_t i = dot; i < path.size(); ++i)
    ext += static_cast<char>(std::tolower(static_cast<unsigned char>(path[i])));
  return kBinary.count(ext) != 0;
}

// Content sniff: a NUL byte anywhere in the first 8 KB, or more than 30%
// control characters, marks the file as binary. Mirrors ripgrep's heuristic.
bool file_looks_binary(const std::string& path) {
  if (binary_extension(path)) return true;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::string sample(kBinarySampleBytes, '\0');
  in.read(sample.data(), static_cast<std::streamsize>(sample.size()));
  sample.resize(static_cast<std::size_t>(in.gcount()));
  if (sample.empty()) return false;
  std::size_t control = 0;
  for (const char c : sample) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if (byte == 0) return true;
    if (byte < 9 || (byte > 13 && byte < 32)) ++control;
  }
  return control * 10 > sample.size() * 3;
}

/* ───────────────── "did you mean" suggestions ───────────────── */

// When a path misses, list up to three siblings that share text with the
// requested name — model path typos are one of the most common tool
// failures on large trees. Full-name containment matches first ("main.cp"
// → "main.cpp"); stem containment catches near-misses like "big2.txt"
// → "big.txt" that differ in the extension's tail.
std::string suggest_nearby(const std::string& path, std::size_t max_suggestions = 3) {
  const fs::path requested(path);
  const fs::path parent =
      requested.has_parent_path() && !requested.parent_path().empty()
          ? requested.parent_path()
          : fs::path(".");
  auto lowered_of = [](const std::string& text) {
    std::string lowered;
    for (const char c : text)
      lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lowered;
  };
  const std::string needle = lowered_of(requested.filename().string());
  const std::size_t needle_dot = needle.rfind('.');
  const std::string needle_stem =
      (needle_dot == std::string::npos || needle_dot == 0)
          ? needle
          : needle.substr(0, needle_dot);

  std::vector<std::string> suggestions;
  std::error_code error;
  for (const fs::directory_entry& entry : fs::directory_iterator(parent, error)) {
    if (error) break;
    const std::string name = entry.path().filename().string();
    const std::string lowered = lowered_of(name);
    const std::size_t dot = lowered.rfind('.');
    const std::string stem =
        (dot == std::string::npos || dot == 0) ? lowered : lowered.substr(0, dot);
    // Loose ends shorter than 3 chars make the stem check noisy; require a
    // real overlap before offering a guess.
    const bool name_hit = lowered.find(needle) != std::string::npos ||
                          needle.find(lowered) != std::string::npos;
    const bool stem_hit = needle_stem.size() >= 3 && stem.size() >= 3 &&
                          (stem.find(needle_stem) != std::string::npos ||
                           needle_stem.find(stem) != std::string::npos);
    if (!name_hit && !stem_hit) continue;
    std::string shown = name;
    if (entry.is_directory(error)) shown += "/";
    suggestions.push_back((parent / shown).generic_string());
    if (suggestions.size() >= max_suggestions) break;
  }
  if (suggestions.empty()) return {};
  std::string text = "\n\nDid you mean one of these?";
  for (const std::string& suggestion : suggestions) text += "\n  " + suggestion;
  return text;
}

/* ───────────────── output caps ───────────────── */

std::string clip_line(const std::string& line) {
  if (line.size() <= kMaxLineChars) return line;
  return line.substr(0, kMaxLineChars) + "… (line truncated to " +
         std::to_string(kMaxLineChars) + " chars)";
}

// Appends a "how to continue" marker once the emitted text crosses the byte
// budget. Called with the number of lines already emitted so read_file can
// point at the exact continuation offset.
bool over_budget(std::size_t emitted_bytes) { return emitted_bytes > kMaxOutputBytes; }

/* ───────────────── gitignore-lite traversal filter ───────────────── */

// Defined below the filter helpers; IgnoreRules::add rewrites ignore globs
// into regexes with the same syntax.
std::string glob_to_regex(const std::string& glob);

// A pragmatic subset of .gitignore semantics, enough to keep scans out of
// build outputs and dependency folders on large trees:
//   - '#' comments, blank lines
//   - 'name'            any path segment named `name` (file or dir)
//   - 'name/'           any directory segment named `name` (prune on descend)
//   - '*.ext'           basename glob, any depth
//   - '/a/b', 'a/b'     root-anchored (any pattern containing '/')
//   - glob bodies: '*' and '?' stay within one segment; '**' crosses them
// Negation ('!') rules are not supported — a match is always an ignore.
// Only the workspace root's .gitignore is read; VCS internals are skipped
// unconditionally.
class IgnoreRules {
 public:
  void load(const fs::path& root) {
    std::ifstream in(root / ".gitignore");
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      add(line);
    }
  }

  // True when `relative` ('/'-separated, no leading slash) should be skipped.
  bool ignored(const std::string& relative, bool is_dir) const {
    for (const Rule& rule : rules_) {
      if (rule.dir_only && !is_dir) continue;
      if (!rule.plain.empty()) {
        if (segment_equals(relative, rule.plain)) return true;
        continue;
      }
      if (std::regex_match(relative, rule.rx)) return true;
      // Anchored directory rules ("build", "/build", "build/") must also
      // prune everything below, which the dir-level check already covers —
      // callers test each directory before descending into it.
    }
    return false;
  }

 private:
  struct Rule {
    std::regex rx;         // used when plain is empty; full-match on relative path
    std::string plain;     // single-segment name; any depth
    bool dir_only = false;
  };

  static bool segment_equals(const std::string& relative, const std::string& name) {
    std::size_t begin = 0;
    while (begin <= relative.size()) {
      std::size_t end = relative.find('/', begin);
      if (end == std::string::npos) end = relative.size();
      if (relative.compare(begin, end - begin, name) == 0) return true;
      begin = end + 1;
    }
    return false;
  }

  void add(const std::string& raw) {
    std::string text = trimmed(raw);
    if (text.empty() || text[0] == '#') return;
    if (text == "/" || text == "!") return;

    Rule rule;
    if (text.back() == '/') {
      rule.dir_only = true;
      text.pop_back();
      if (text.empty()) return;
    }
    const bool anchored = text.find('/') != std::string::npos;
    if (!text.empty() && text.front() == '/') text.erase(text.begin());
    if (text.empty()) return;

    const bool has_glob =
        text.find('*') != std::string::npos || text.find('?') != std::string::npos ||
        text.find('[') != std::string::npos;
    if (!anchored && !has_glob) {
      rule.plain = text; // e.g. "node_modules", "build", "*.log" without slash
      rules_.push_back(std::move(rule));
      return;
    }

    std::string body = glob_to_regex(text);
    std::string pattern;
    if (anchored) {
      pattern = "^" + body + (rule.dir_only ? "$" : "(/.*)?$");
    } else {
      pattern = "^(.*/)?" + body + (rule.dir_only ? "$" : "(/.*)?$");
    }
    try {
      rule.rx.assign(pattern, std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::exception&) {
      return; // malformed pattern matches nothing
    }
    rules_.push_back(std::move(rule));
  }

  std::vector<Rule> rules_;
};

// Dirs that are never worth scanning even without a .gitignore (VCS internals
// and the most common dependency caches).
bool always_skip_dir(const std::string& name) {
  static const std::set<std::string> kSkip{
      ".git", ".hg", ".svn", ".jj", "__pycache__", "node_modules",
      ".gradle", ".idea", ".vs", ".vscode", "DerivedData"};
  return kSkip.count(name) != 0;
}

// Shared recursive-scan loop for glob/grep: walks `root`, prunes ignored and
// VCS directories on descend, and calls visit(relative_path, absolute_path)
// for every regular file. Returns false when the visitor asked to stop.
template <typename Visit>
bool scan_tree(const fs::path& root, const IgnoreRules& rules, Visit&& visit) {
  std::error_code error;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, error);
  if (error) return true;
  while (it != fs::recursive_directory_iterator()) {
    if (error) return true; // unreadable subtree: keep what we have
    const fs::path absolute = it->path();
    std::error_code rel_error;
    const std::string relative = fs::relative(absolute, root, rel_error).generic_string();
    if (rel_error || relative.empty()) {
      error.clear();
      it.increment(error);
      continue;
    }
    std::error_code type_error;
    const bool is_dir = it->is_directory(type_error);
    if (!type_error && is_dir) {
      if (always_skip_dir(absolute.filename().string()) ||
          rules.ignored(relative, true)) {
        it.disable_recursion_pending();
        it.increment(error);
        continue;
      }
      it.increment(error);
      continue;
    }
    if (!type_error && it->is_regular_file(type_error)) {
      if (!rules.ignored(relative, false)) {
        if (!visit(relative, absolute)) return false;
      }
    }
    it.increment(error);
  }
  return true;
}

/* ───────────────── glob → regex (unchanged semantics) ───────────────── */

// Translates a glob (*, ?, otherwise literal) into an ECMAScript regex that
// full-matches relative paths with '/' separators. '*' and '?' do not cross
// path separators; '**' does (so "src/**/*.cpp" works).
std::string glob_to_regex(const std::string& glob) {
  std::string rx;
  for (std::size_t i = 0; i < glob.size(); ++i) {
    const char c = glob[i];
    if (c == '*' && i + 1 < glob.size() && glob[i + 1] == '*') {
      rx += ".*";
      ++i; // consume the second '*'
      continue;
    }
    switch (c) {
      case '*': rx += "[^/\\\\]*"; break;
      case '?': rx += "[^/\\\\]"; break;
      case '.': case '\\': case '+': case '(': case ')': case '|':
      case '^': case '$': case '[': case ']': case '{': case '}':
        rx += '\\';
        rx += c;
        break;
      default: rx += c; break;
    }
  }
  return rx;
}

bool matches_glob(const std::string& path_text, const std::regex& glob_rx) {
  try {
    return std::regex_match(path_text, glob_rx);
  } catch (const std::exception&) {
    return false;
  }
}

/* ───────────────── edit_file matching machinery ───────────────── */

std::size_t count_newlines(const std::string& text) {
  return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

// Dominant line ending of a file: CRLF only when \r\n is the majority style.
bool content_is_crlf(const std::string& content) {
  std::size_t crlf = 0;
  for (std::size_t pos = content.find("\r\n"); pos != std::string::npos;
       pos = content.find("\r\n", pos + 2)) {
    ++crlf;
  }
  const std::size_t lf = count_newlines(content);
  return crlf * 2 > lf;
}

std::string to_crlf(std::string text) {
  std::string out;
  out.reserve(text.size() + count_newlines(text));
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) out += '\r';
    out += text[i];
  }
  return out;
}

std::string to_lf(std::string text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') continue;
    out += text[i];
  }
  return out;
}

// Levenshtein distance over (capped) trimmed lines — the block-anchor
// fallback only needs a cheap similarity signal, not exactness.
double line_similarity(const std::string& a, const std::string& b) {
  constexpr std::size_t kCap = 300;
  std::string x = a.size() > kCap ? a.substr(0, kCap) : a;
  std::string y = b.size() > kCap ? b.substr(0, kCap) : b;
  const std::size_t n = x.size();
  const std::size_t m = y.size();
  if (n == 0 && m == 0) return 1.0;
  if (n == 0 || m == 0) return 0.0;
  std::vector<std::size_t> previous(m + 1), current(m + 1);
  for (std::size_t j = 0; j <= m; ++j) previous[j] = j;
  for (std::size_t i = 1; i <= n; ++i) {
    current[0] = i;
    for (std::size_t j = 1; j <= m; ++j) {
      const std::size_t substitution = previous[j - 1] + (x[i - 1] == y[j - 1] ? 0 : 1);
      current[j] = std::min({substitution, previous[j] + 1, current[j - 1] + 1});
    }
    std::swap(previous, current);
  }
  return 1.0 - static_cast<double>(previous[m]) / static_cast<double>(std::max(n, m));
}

std::vector<std::string> split_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      if (begin < text.size()) lines.push_back(text.substr(begin));
      break;
    }
    std::string line = text.substr(begin, end - begin);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(line);
    begin = end + 1;
  }
  return lines;
}

// Byte span of `line_count` lines starting at line `first` (0-based),
// excluding the trailing newline of the last line.
bool span_of_lines(const std::string& content, std::size_t first, std::size_t line_count,
                   std::size_t& start, std::size_t& end) {
  std::size_t offset = 0;
  std::size_t index = 0;
  while (index < first && offset < content.size()) {
    if (content[offset] == '\n') ++index;
    ++offset;
  }
  if (index < first) return false;
  start = offset;
  std::size_t counted = 0;
  while (counted < line_count && offset < content.size()) {
    if (content[offset] == '\n') {
      ++counted;
      if (counted == line_count) break;
    }
    ++offset;
  }
  if (counted < line_count) {
    if (offset >= content.size() && counted == line_count - 1) {
      // last line without trailing newline
      end = content.size();
      return true;
    }
    return false;
  }
  // walk back the newline that ended the last line
  end = offset;
  return true;
}

struct EditMatch {
  std::size_t start = 0;
  std::size_t end = 0;
  const char* via = "exact match";
};

// Every accepted span (descending order at the call site keeps the offsets
// valid across replacements). `replace_all=false` requires exactly one.
struct MatchOutcome {
  std::vector<EditMatch> hits;
  std::string error;  // non-empty → hard failure with this message
};

// 1) exact  2) line-ending tolerant  3) trimmed-line window  4) block anchors
MatchOutcome find_edit_matches(const std::string& content, const std::string& old_string,
                               bool replace_all) {
  MatchOutcome outcome;
  const bool file_crlf = content_is_crlf(content);

  auto accept = [&outcome, replace_all](std::vector<EditMatch> spans,
                                        const char* via) -> bool {
    if (spans.empty()) return false;
    if (spans.size() > 1 && !replace_all) {
      outcome.error = "old_string appears " + std::to_string(spans.size()) +
                      " times — it must be unique. Include more surrounding lines, or pass"
                      " replace_all:true to replace every occurrence.";
      return true;
    }
    for (EditMatch& span : spans) span.via = via;
    outcome.hits = std::move(spans);
    return true;
  };

  // 1 — byte-exact
  {
    std::vector<EditMatch> spans;
    for (std::size_t pos = content.find(old_string); pos != std::string::npos;
         pos = content.find(old_string, pos + 1)) {
      spans.push_back(EditMatch{pos, pos + old_string.size(), "exact match"});
    }
    if (accept(std::move(spans), "exact match")) return outcome;
    if (!outcome.error.empty()) return outcome;
  }

  if (old_string.find('\n') == std::string::npos) {
    outcome.error = "old_string not found in file (copy exact text from the file)";
    return outcome;
  }

  // 2 — line-ending tolerant: the model sent \n but the file is \r\n (or the
  // reverse). Re-key the needle to the file's dominant ending.
  {
    std::vector<EditMatch> spans;
    if (file_crlf) {
      const std::string needle = to_crlf(old_string);
      for (std::size_t pos = content.find(needle); pos != std::string::npos;
           pos = content.find(needle, pos + 1)) {
        spans.push_back(EditMatch{pos, pos + needle.size(), "line-ending match"});
      }
    } else {
      const std::string needle = to_lf(old_string);
      for (std::size_t pos = content.find(needle); pos != std::string::npos;
           pos = content.find(needle, pos + 1)) {
        spans.push_back(EditMatch{pos, pos + needle.size(), "line-ending match"});
      }
    }
    if (accept(std::move(spans), "line-ending match")) return outcome;
    if (!outcome.error.empty()) return outcome;
  }

  std::vector<std::string> search_lines = split_lines(to_lf(old_string));
  while (!search_lines.empty() && trimmed(search_lines.back()).empty())
    search_lines.pop_back();
  if (search_lines.empty()) {
    outcome.error = "old_string not found in file";
    return outcome;
  }

  const std::vector<std::string> content_lines = split_lines(to_lf(content));

  // 3 — trimmed-line window: every line matches after trimming whitespace.
  // Rescues indentation drift, list-marker changes and tab/space mixes.
  {
    std::vector<EditMatch> spans;
    if (content_lines.size() >= search_lines.size()) {
      for (std::size_t base = 0; base + search_lines.size() <= content_lines.size(); ++base) {
        bool matches = true;
        for (std::size_t j = 0; j < search_lines.size(); ++j) {
          if (trimmed(content_lines[base + j]) != trimmed(search_lines[j])) {
            matches = false;
            break;
          }
        }
        if (!matches) continue;
        std::size_t start = 0;
        std::size_t end = 0;
        if (!span_of_lines(content, base, search_lines.size(), start, end)) continue;
        spans.push_back(EditMatch{start, end, "trimmed-line match"});
      }
    }
    if (accept(std::move(spans), "trimmed-line match")) return outcome;
    if (!outcome.error.empty()) return outcome;
  }

  // 4 — block anchors: first+last lines must match (trimmed); the middle is
  // compared by average similarity so a mangled interior line still lands.
  if (search_lines.size() >= 3) {
    struct Candidate {
      std::size_t first = 0;
      std::size_t last = 0;
      double similarity = 0.0;
    };
    std::vector<Candidate> candidates;
    const std::string first_anchor = trimmed(search_lines.front());
    const std::string last_anchor = trimmed(search_lines.back());
    const std::size_t expected = search_lines.size();
    const std::size_t max_delta = std::max<std::size_t>(1, expected / 4);

    for (std::size_t i = 0; i + 2 < content_lines.size(); ++i) {
      if (trimmed(content_lines[i]) != first_anchor) continue;
      for (std::size_t j = i + 2; j < content_lines.size(); ++j) {
        if (trimmed(content_lines[j]) != last_anchor) continue;
        const std::size_t actual = j - i + 1;
        if (actual > expected + max_delta || expected > actual + max_delta) break;
        double similarity = 1.0;
        const std::size_t middles = std::min(expected - 2, actual - 2);
        if (middles > 0) {
          double total = 0.0;
          for (std::size_t k = 1; k <= middles; ++k) {
            total += line_similarity(trimmed(content_lines[i + k]),
                                     trimmed(search_lines[k]));
          }
          similarity = total / static_cast<double>(middles);
        }
        candidates.push_back(Candidate{i, j, similarity});
        break; // only the first closing anchor per opening anchor
      }
    }

    std::vector<EditMatch> spans;
    for (const Candidate& candidate : candidates) {
      if (candidate.similarity < 0.65) continue;
      std::size_t start = 0;
      std::size_t end = 0;
      if (!span_of_lines(content, candidate.first, candidate.last - candidate.first + 1,
                         start, end))
        continue;
      spans.push_back(EditMatch{start, end, "block-anchor match"});
    }
    // keep the single best fuzzy hit when several survive the threshold
    if (spans.size() > 1 && !replace_all) {
      const EditMatch* best = &spans.front();
      for (const EditMatch& span : spans)
        if (span.end - span.start > best->end - best->start) best = &span;
      std::vector<EditMatch> only{*best};
      if (accept(std::move(only), "block-anchor match")) return outcome;
    } else {
      if (accept(std::move(spans), "block-anchor match")) return outcome;
    }
  }

  outcome.error =
      "old_string not found in file (copy exact text from the file; matching"
      " already tolerates line-ending, indentation and minor content drift)";
  return outcome;
}

} // namespace

Tool make_read_file_tool() {
  Tool tool;
  tool.name = "read_file";
  tool.description =
      "Read a text file and get numbered lines. Set start_line/end_line (1-based, inclusive) to view a range; a full read of files over 2000 lines returns only the first 500 and last 200 lines plus a marker. Lines are clipped at 2000 chars and output at 50 KB with a continuation hint. Binary files are refused.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"path",
            {
                {"type", "string"},
                {"description", "Path to the text file"},
            }},
           {"start_line",
            {
                {"type", "integer"},
                {"description", "First line to read (1-based, inclusive)"},
            }},
           {"end_line",
            {
                {"type", "integer"},
                {"description", "Last line to read (1-based, inclusive)"},
            }},
       }},
      {"required", nlohmann::json::array({"path"})},
  };
  tool.readonly = true;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("path") || !arguments["path"].is_string()) {
      return {false, "", "read_file requires a 'path' string"};
    }
    const std::string path = resolve_tool_path(arguments["path"].get<std::string>());
    if (arguments["path"].get<std::string>().empty()) {
      return {false, "", "read_file: 'path' must not be empty"};
    }

    const bool has_start =
        arguments.contains("start_line") && arguments["start_line"].is_number_integer();
    const bool has_end =
        arguments.contains("end_line") && arguments["end_line"].is_number_integer();
    const long start = has_start ? arguments["start_line"].get<long>() : -1;
    const long end = has_end ? arguments["end_line"].get<long>() : -1;
    if (has_start && has_end && start > end) {
      return ToolResult{false, "",
                        "read_file: start_line (" + std::to_string(start) +
                            ") is greater than end_line (" + std::to_string(end) + ")"};
    }

    std::error_code error;
    if (fs::is_directory(path, error)) {
      return ToolResult{false, "", "read_file: '" + path + "' is a directory (use list_dir)"};
    }
    if (!fs::is_regular_file(path, error)) {
      return ToolResult{false, "",
                        "read_file: " + read_error_for(path) + suggest_nearby(path)};
    }
    if (file_looks_binary(path)) {
      return ToolResult{false, "",
                        "read_file: '" + path + "' is binary and cannot be shown as text"};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
      return ToolResult{false, "", "read_file: " + read_error_for(path)};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back(); // CRLF-safe
      lines.push_back(line);
    }
    const std::size_t total = lines.size();

    // Emission helper: clips long lines, stops at the byte budget and tells
    // the model exactly how to continue.
    const auto emit = [&lines](std::size_t first_index, std::size_t last_index_exclusive,
                               bool numbered) {
      std::string output;
      for (std::size_t i = first_index; i < last_index_exclusive && i < lines.size(); ++i) {
        std::string row = clip_line(lines[i]);
        if (numbered) row = std::to_string(i + 1) + ": " + row;
        row += "\n";
        if (over_budget(output.size() + row.size())) {
          const std::size_t shown = i - first_index;
          const std::size_t next_line = first_index + shown + 1;
          output += "… [output capped at 50 KB — showing lines " +
                    std::to_string(first_index + 1) + "-" + std::to_string(i) + " of " +
                    std::to_string(lines.size()) + "; call read_file again with start_line=" +
                    std::to_string(next_line) + "] …\n";
          return output;
        }
        output += row;
      }
      return output;
    };

    std::string output;
    if (!has_start && !has_end && total > 2000) {
      // Long-file mechanism: head + tail, telling the model the middle needs
      // explicit line ranges.
      const std::size_t head = 500;
      const std::size_t tail = 200;
      const std::size_t omitted = total - head - tail;
      output += emit(0, head, true);
      output += "… [" + std::to_string(omitted) +
                " lines omitted — call read_file again with start_line/end_line to view the "
                "middle (lines " +
                std::to_string(head + 1) + "–" + std::to_string(total - tail) + ")] …\n";
      output += emit(total - tail, total, true);
      return ToolResult{true, output, ""};
    }

    if (has_start || has_end) {
      const long first = std::max<long>(1, has_start ? start : 1);
      const long last =
          std::min<long>(static_cast<long>(total), has_end ? end : static_cast<long>(total));
      if (first > last || first > static_cast<long>(total)) {
        return ToolResult{
            false, "",
            "read_file: requested line range " + std::to_string(first) + "-" +
                std::to_string(has_end ? end : static_cast<long>(total)) +
                " is outside the file (" + std::to_string(total) + " lines)"};
      }
      output += emit(static_cast<std::size_t>(first) - 1, static_cast<std::size_t>(last),
                     true);
      if (first > 1 || last < static_cast<long>(total)) {
        output += "… [" + std::to_string(first) + "-" + std::to_string(last) + " of " +
                  std::to_string(total) + " lines] …\n";
      }
      return ToolResult{true, output, ""};
    }

    output = emit(0, total, true);
    output += "(end of file — " + std::to_string(total) + " lines)\n";
    return ToolResult{true, output, ""};
  };

  return tool;
}

Tool make_write_file_tool() {
  Tool tool;
  tool.name = "write_file";
  tool.description =
      "Create or overwrite a file with the given content (parent directories are created as needed). Returns the byte count.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"path",
            {
                {"type", "string"},
                {"description", "Destination file path"},
            }},
           {"content",
            {
                {"type", "string"},
                {"description", "Full file content to write"},
            }},
       }},
      {"required", nlohmann::json::array({"path", "content"})},
  };
  tool.readonly = false;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("path") || !arguments["path"].is_string()) {
      return {false, "", "write_file requires a 'path' string"};
    }
    if (!arguments.contains("content") || !arguments["content"].is_string()) {
      return {false, "", "write_file requires a 'content' string"};
    }
    const std::string path = resolve_tool_path(arguments["path"].get<std::string>());
    const std::string content = arguments["content"].get<std::string>();

    const fs::path destination(path);
    std::error_code error;
    const bool existed = fs::exists(destination, error);
    if (destination.has_parent_path() && !destination.parent_path().empty()) {
      fs::create_directories(destination.parent_path(), error);
      if (error && !fs::is_directory(destination.parent_path())) {
        return ToolResult{false, "",
                          "write_file: cannot create parent directory '" +
                              destination.parent_path().string() + "': " + error.message()};
      }
    }

    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return ToolResult{false, "", "write_file: " + read_error_for(path)};
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) {
      return ToolResult{false, "", "write_file: failed while writing '" + path + "'"};
    }
    return ToolResult{true,
                      std::string(existed ? "overwrote " : "created ") + "'" + path + "' (" +
                          std::to_string(content.size()) + " bytes)",
                      ""};
  };

  return tool;
}

Tool make_edit_file_tool() {
  Tool tool;
  tool.name = "edit_file";
  tool.description =
      "Replace 'old_string' with 'new_string' in a file. old_string must be unique unless replace_all is true. Matching tolerates line-ending (CRLF/LF), indentation and minor content drift, but exact text is always best.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"path",
            {
                {"type", "string"},
                {"description", "File to edit (must already exist)"},
            }},
           {"old_string",
            {
                {"type", "string"},
                {"description", "Text to replace; must be unique in the file"},
            }},
           {"new_string",
            {
                {"type", "string"},
                {"description", "Replacement text"},
            }},
           {"replace_all",
            {
                {"type", "boolean"},
                {"description", "Replace every occurrence of old_string (default false)"},
            }},
       }},
      {"required", nlohmann::json::array({"path", "old_string", "new_string"})},
  };
  tool.readonly = false;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("path") || !arguments["path"].is_string()) {
      return {false, "", "edit_file requires a 'path' string"};
    }
    if (!arguments.contains("old_string") || !arguments["old_string"].is_string()) {
      return {false, "", "edit_file requires an 'old_string' string"};
    }
    if (!arguments.contains("new_string") || !arguments["new_string"].is_string()) {
      return {false, "", "edit_file requires a 'new_string' string"};
    }
    const std::string path_arg = arguments["path"].get<std::string>();
    const std::string resolved = resolve_tool_path(path_arg);
    if (path_arg.empty()) {
      return {false, "", "edit_file: 'path' must not be empty"};
    }
    const std::string old_string = arguments["old_string"].get<std::string>();
    const std::string new_string = arguments["new_string"].get<std::string>();
    const bool replace_all = arguments.contains("replace_all") &&
                             arguments["replace_all"].is_boolean() &&
                             arguments["replace_all"].get<bool>();
    if (old_string.empty()) {
      return {false, "", "edit_file: 'old_string' must not be empty"};
    }
    if (old_string == new_string) {
      return {false, "", "edit_file: old_string and new_string are identical — nothing to do"};
    }

    std::error_code error;
    if (!fs::is_regular_file(resolved, error)) {
      return ToolResult{false, "",
                        "edit_file: " + read_error_for(resolved) + suggest_nearby(resolved)};
    }

    std::string content;
    if (!read_whole_file(resolved, content)) {
      return ToolResult{false, "", "edit_file: " + read_error_for(resolved)};
    }
    if (file_looks_binary(resolved)) {
      return ToolResult{false, "",
                        "edit_file: '" + resolved + "' looks binary — refusing to edit"};
    }

    const MatchOutcome outcome = find_edit_matches(content, old_string, replace_all);
    if (!outcome.error.empty()) {
      return ToolResult{false, "", "edit_file: " + outcome.error};
    }
    if (outcome.hits.empty()) {
      return ToolResult{false, "", "edit_file: old_string not found (copy exact text)"};
    }

    // Re-key new_string to the file's dominant line ending so a \n-quoted
    // replacement does not produce mixed endings in a CRLF file.
    const bool file_crlf = content_is_crlf(content);
    const std::string replacement =
        file_crlf ? to_crlf(new_string) : to_lf(new_string);

    std::vector<EditMatch> spans = outcome.hits;
    std::sort(spans.begin(), spans.end(),
              [](const EditMatch& a, const EditMatch& b) { return a.start > b.start; });
    std::size_t previous_start = std::string::npos;
    std::size_t applied = 0;
    for (const EditMatch& span : spans) {
      if (previous_start != std::string::npos && span.end > previous_start) continue; // overlap
      content.replace(span.start, span.end - span.start, replacement);
      previous_start = span.start;
      ++applied;
    }

    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return ToolResult{false, "", "edit_file: " + read_error_for(resolved)};
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) {
      return ToolResult{false, "", "edit_file: failed while writing '" + resolved + "'"};
    }
    return ToolResult{true,
                      "edited '" + resolved + "': " + std::to_string(applied) +
                          " occurrence(s) replaced via " + outcome.hits.front().via + " (" +
                          std::to_string(old_string.size()) + " → " +
                          std::to_string(new_string.size()) + " bytes)",
                      ""};
  };

  return tool;
}

Tool make_list_dir_tool() {
  Tool tool;
  tool.name = "list_dir";
  tool.description =
      "List a directory's entries with type/size (bytes). Directories first, then names.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"path",
            {
                {"type", "string"},
                {"description", "Directory path (default: workspace root)"},
            }},
       }},
      {"required", nlohmann::json::array({})},
  };
  tool.readonly = true;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    std::string path = workspace_root().string();
    if (arguments.contains("path") && arguments["path"].is_string() &&
        !arguments["path"].get<std::string>().empty()) {
      path = resolve_tool_path(arguments["path"].get<std::string>());
    }

    std::error_code error;
    if (!fs::is_directory(path, error)) {
      return ToolResult{false, "", "list_dir: '" + path + "' is not a directory"};
    }

    std::vector<std::string> directories;
    std::vector<std::string> files;
    for (const fs::directory_entry& entry : fs::directory_iterator(path, error)) {
      std::string name = entry.path().filename().string();
      if (entry.is_directory(error)) {
        name += "/";
        directories.push_back(name);
      } else {
        files.push_back(name + " (" + std::to_string(entry.file_size(error)) + " bytes)");
      }
    }
    if (error) {
      return ToolResult{false, "", "list_dir: cannot read '" + path + "': " + error.message()};
    }

    std::sort(directories.begin(), directories.end());
    std::sort(files.begin(), files.end());

    std::string output;
    for (const std::string& name : directories) output += name + "\n";
    for (const std::string& name : files) output += name + "\n";
    if (output.empty()) output = "(empty directory)\n";
    return ToolResult{true, output, ""};
  };

  return tool;
}

Tool make_glob_tool() {
  Tool tool;
  tool.name = "glob";
  tool.description =
      "Find files by glob pattern relative to the workspace root ('*' and '?' don't cross "
      "folders; '**' does). Optional 'path' restricts scanning to a subdirectory. Results are "
      "newest-modified first, capped at 500 with a hint; VCS/dependency folders and gitignored "
      "paths (root .gitignore) are never scanned.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"pattern",
            {
                {"type", "string"},
                {"description", "Glob pattern, e.g. src/**/*.cpp or *.txt"},
            }},
           {"path",
            {
                {"type", "string"},
                {"description", "Directory to scan (default: workspace root)"},
            }},
       }},
      {"required", nlohmann::json::array({"pattern"})},
  };
  tool.readonly = true;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("pattern") || !arguments["pattern"].is_string()) {
      return {false, "", "glob requires a 'pattern' string"};
    }
    const std::string pattern = arguments["pattern"].get<std::string>();
    if (pattern.empty()) {
      return {false, "", "glob: 'pattern' must not be empty"};
    }
    std::string scan_root = workspace_root().string();
    if (arguments.contains("path") && arguments["path"].is_string() &&
        !arguments["path"].get<std::string>().empty()) {
      scan_root = resolve_tool_path(arguments["path"].get<std::string>());
    }
    std::error_code dir_error;
    if (!fs::is_directory(scan_root, dir_error)) {
      return ToolResult{false, "", "glob: 'path' must be a directory: '" + scan_root + "'"};
    }

    std::regex rx;
    try {
      rx.assign(glob_to_regex(pattern), std::regex::ECMAScript | std::regex::optimize);
    } catch (const std::exception& exception) {
      return ToolResult{false, "", std::string("glob: invalid pattern: ") + exception.what()};
    }

    IgnoreRules rules;
    rules.load(workspace_root());

    struct Hit {
      std::string path;
      std::int64_t mtime = 0;
    };
    std::vector<Hit> hits;
    bool capped = false;
    scan_tree(fs::path(scan_root), rules,
              [&](const std::string&, const fs::path& absolute) {
                // Patterns and results stay workspace-relative so the same
                // pattern means the same thing with or without 'path'.
                std::error_code relative_error;
                const fs::path relative = fs::relative(absolute, workspace_root(), relative_error);
                if (relative_error || relative.empty()) return true;
                const std::string text = relative.generic_string();
                if (!matches_glob(text, rx)) return true;
                std::error_code time_error;
                const auto stamp = fs::last_write_time(absolute, time_error);
                std::int64_t mtime = 0;
                if (!time_error) {
                  mtime = std::chrono::duration_cast<std::chrono::seconds>(
                              stamp.time_since_epoch())
                              .count();
                }
                hits.push_back(Hit{text, mtime});
                if (hits.size() >= kGlobMaxHits) {
                  capped = true;
                  return false;
                }
                return true;
              });

    if (hits.empty()) return ToolResult{true, "(no matches)", ""};
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
      if (a.mtime != b.mtime) return a.mtime > b.mtime; // newest first
      return a.path < b.path;
    });

    std::string output;
    for (const Hit& hit : hits) output += hit.path + "\n";
    if (capped) {
      output += "… (results capped at " + std::to_string(kGlobMaxHits) +
                " — refine the pattern or add a 'path' to narrow)\n";
    }
    return ToolResult{true, output, ""};
  };

  return tool;
}

Tool make_grep_tool() {
  Tool tool;
  tool.description =
      "Search files line-by-line for 'pattern' (std::regex syntax) and return hits as file:line: text. Set 'glob' to restrict which files match. Skips VCS dirs, dependency folders, gitignored paths (root .gitignore) and binary files; output is capped with a hint.";
  tool.name = "grep";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"pattern",
            {
                {"type", "string"},
                {"description", "Regular expression to search for"},
            }},
           {"path",
            {
                {"type", "string"},
                {"description", "File or directory to search (default: workspace root)"},
            }},
           {"glob",
            {
                {"type", "string"},
                {"description", "Only scan files matching this glob (e.g. *.cpp)"},
            }},
       }},
      {"required", nlohmann::json::array({"pattern"})},
  };
  tool.readonly = true;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("pattern") || !arguments["pattern"].is_string()) {
      return {false, "", "grep requires a 'pattern' string"};
    }
    const std::string pattern = arguments["pattern"].get<std::string>();
    std::string path = workspace_root().string();
    if (arguments.contains("path") && arguments["path"].is_string() &&
        !arguments["path"].get<std::string>().empty()) {
      path = resolve_tool_path(arguments["path"].get<std::string>());
    }
    std::string glob;
    if (arguments.contains("glob") && arguments["glob"].is_string()) {
      glob = arguments["glob"].get<std::string>();
    }

    // The pattern itself is a user-provided regex; a bad pattern is a tool
    // error, not a crash. The optional include-glob compiles once, not per
    // candidate file.
    std::regex rx;
    try {
      rx.assign(pattern);
    } catch (const std::exception& exception) {
      return ToolResult{false, "", std::string("grep: invalid pattern: ") + exception.what()};
    }
    std::regex glob_rx;
    const bool has_glob = !glob.empty();
    if (has_glob) {
      try {
        glob_rx.assign(glob_to_regex(glob), std::regex::ECMAScript | std::regex::optimize);
      } catch (const std::exception& exception) {
        return ToolResult{false, "",
                          std::string("grep: invalid glob: ") + exception.what()};
      }
    }

    std::error_code error;
    std::vector<fs::path> targets; // files to scan
    if (fs::is_regular_file(path, error)) {
      targets.push_back(fs::path(path));
    } else {
      IgnoreRules rules;
      rules.load(workspace_root());
      const fs::path base(path);
      scan_tree(base, rules,
                [&](const std::string& relative, const fs::path& absolute) {
                  if (has_glob && !matches_glob(relative, glob_rx)) return true;
                  targets.push_back(absolute);
                  return true;
                });
    }

    std::vector<std::string> hits;
    bool capped = false;
    for (const fs::path& target : targets) {
      if (capped) break;
      std::error_code size_error;
      if (fs::file_size(target, size_error) > kGrepMaxFileSize) continue;
      if (file_looks_binary(target.string())) continue; // skip binaries like ripgrep
      std::ifstream file(target, std::ios::binary);
      if (!file.is_open()) continue;
      std::string line;
      long line_number = 0;
      std::size_t per_file = 0;
      while (std::getline(file, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        try {
          if (!std::regex_search(line, rx)) continue;
        } catch (const std::exception&) {
          continue; // malformed line content; keep scanning
        }
        hits.push_back(target.generic_string() + ":" + std::to_string(line_number) + ": " +
                       clip_line(line));
        ++per_file;
        if (per_file >= kGrepMaxHitsPerFile) {
          hits.push_back("… more matches in this file (per-file cap " +
                         std::to_string(kGrepMaxHitsPerFile) + ") — narrow with 'path'");
          break;
        }
        if (hits.size() >= kGrepMaxHits) {
          hits.push_back("… stop after " + std::to_string(kGrepMaxHits) +
                         " matches — narrow with 'path' or 'glob'");
          capped = true;
          break;
        }
      }
    }

    if (hits.empty()) return ToolResult{true, "(no matches)", ""};
    std::string output;
    for (const std::string& hit : hits) output += hit + "\n";
    return ToolResult{true, output, ""};
  };

  return tool;
}

} // namespace gamma::tools
