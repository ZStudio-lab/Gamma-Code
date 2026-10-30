#include "tools/registry.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gamma::tools {

namespace {

namespace fs = std::filesystem;

std::string read_error_for(const std::string& path) {
  return "cannot open '" + path + "' (missing or unreadable)";
}

// Reads a whole file as bytes. Empty ok flag on success.
bool read_whole_file(const std::string& path, std::string& content) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Counts occurrences of `needle` (overlaps included) — used for the 0-or-1
// uniqueness rule of edit_file.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  if (needle.empty()) return count;
  for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + 1)) {
    ++count;
  }
  return count;
}

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

bool matches_glob(const std::string& path_text, const std::string& glob) {
  try {
    const std::regex rx(glob_to_regex(glob));
    return std::regex_match(path_text, rx);
  } catch (const std::exception&) {
    return false; // invalid pattern matches nothing
  }
}

} // namespace

Tool make_read_file_tool() {
  Tool tool;
  tool.name = "read_file";
  tool.description =
      "Read a text file and get numbered lines. Set start_line/end_line (1-based, inclusive) to view a range; a full read of files over 2000 lines returns only the first 500 and last 200 lines plus a marker.";
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

    std::string output;
    if (!has_start && !has_end && total > 2000) {
      // Long-file mechanism: head + tail, telling the model the middle needs
      // explicit line ranges.
      const std::size_t head = 500;
      const std::size_t tail = 200;
      const std::size_t omitted = total - head - tail;
      for (std::size_t i = 0; i < head; ++i) {
        output += std::to_string(i + 1) + ": " + lines[i] + "\n";
      }
      output += "… [" + std::to_string(omitted) +
                " lines omitted — call read_file again with start_line/end_line to view the "
                "middle (lines " +
                std::to_string(head + 1) + "–" + std::to_string(total - tail) + ")] …\n";
      for (std::size_t i = total - tail; i < total; ++i) {
        output += std::to_string(i + 1) + ": " + lines[i] + "\n";
      }
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
      for (long n = first; n <= last; ++n) {
        output += std::to_string(n) + ": " + lines[static_cast<std::size_t>(n) - 1] + "\n";
      }
      if (first > 1 || last < static_cast<long>(total)) {
        output += "… [" + std::to_string(first) + "-" + std::to_string(last) + " of " +
                  std::to_string(total) + " lines] …\n";
      }
      return ToolResult{true, output, ""};
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
      output += std::to_string(i + 1) + ": " + lines[i] + "\n";
    }
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
    if (destination.has_parent_path() && !destination.parent_path().empty()) {
      std::error_code error;
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
                      "wrote " + std::to_string(content.size()) + " bytes to '" + path + "'",
                      ""};
  };

  return tool;
}

Tool make_edit_file_tool() {
  Tool tool;
  tool.name = "edit_file";
  tool.description =
      "Replace an exact 'old_string' with 'new_string' in a file. old_string must appear exactly once; use a larger snippet for context when ambiguous.";
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
                {"description", "Exact text to replace; must be unique in the file"},
            }},
           {"new_string",
            {
                {"type", "string"},
                {"description", "Replacement text"},
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
    if (old_string.empty()) {
      return {false, "", "edit_file: 'old_string' must not be empty"};
    }

    std::string content;
    if (!read_whole_file(resolved, content)) {
      return ToolResult{false, "", "edit_file: " + read_error_for(resolved)};
    }

    const std::size_t occurrences = count_occurrences(content, old_string);
    if (occurrences == 0) {
      return ToolResult{false, "",
                        "edit_file: old_string not found in '" + resolved +
                            "' (copy exact text from the file; check line endings)"};
    }
    if (occurrences > 1) {
      return ToolResult{false, "",
                        "edit_file: old_string appears " + std::to_string(occurrences) +
                            " times in '" + resolved +
                            "' — it must be unique. Include more surrounding lines."};
    }

    const std::size_t at = content.find(old_string);
    content.replace(at, old_string.size(), new_string);
    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      return ToolResult{false, "", "edit_file: " + read_error_for(resolved)};
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out.good()) {
      return ToolResult{false, "", "edit_file: failed while writing '" + resolved + "'"};
    }
    return ToolResult{true,
                      "edited '" + resolved + "': 1 occurrence replaced (" +
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
      "Find files following a glob pattern relative to the workspace. '*' and '?' don't cross folders; '**' does. Case-sensitive.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"pattern",
            {
                {"type", "string"},
                {"description", "Glob pattern, e.g. src/**/*.cpp or *.txt"},
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

    try {
      const std::regex rx(glob_to_regex(pattern));
      std::vector<std::string> hits;
      constexpr std::size_t kMaxHits = 500;
      std::error_code error;
      for (fs::recursive_directory_iterator it(".", fs::directory_options::skip_permission_denied,
                                               error);
           it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) break;
        const fs::path relative = fs::relative(it->path(), error);
        if (error) {
          error.clear();
          continue;
        }
        std::string text = relative.generic_string();
        if (!std::regex_match(text, rx)) continue;
        hits.push_back(text);
        if (hits.size() >= kMaxHits) {
          hits.push_back("… (capped at " + std::to_string(kMaxHits) + " Matches)");
          break;
        }
      }
      if (error && hits.empty()) {
        return ToolResult{false, "", "glob: cannot scan workspace: " + error.message()};
      }
      if (hits.empty()) return ToolResult{true, "(no matches)", ""};
      std::sort(hits.begin(), hits.end());
      std::string output;
      for (const std::string& hit : hits) output += hit + "\n";
      return ToolResult{true, output, ""};
    } catch (const std::exception& exception) {
      return ToolResult{false, "", std::string("glob: invalid pattern: ") + exception.what()};
    }
  };

  return tool;
}

Tool make_grep_tool() {
  Tool tool;
  tool.name = "grep";
  tool.description =
      "Search files line-by-line for 'pattern' (std::regex syntax) and return hits as file:line: text. Set 'glob' to restrict which files match.";
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

    std::error_code error;
    std::vector<std::string> targets; // files to scan (directories resolved below)
    if (fs::is_regular_file(path, error)) {
      targets.push_back(fs::absolute(path).generic_string());
    } else {
      for (fs::recursive_directory_iterator it(path, fs::directory_options::skip_permission_denied,
                                               error);
           it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) break;
        if (!it->is_regular_file(error)) continue;
        const fs::path relative = fs::relative(it->path(), error);
        error.clear();
        const std::string text = relative.generic_string();
        if (!glob.empty() && !matches_glob(text, glob)) continue;
        targets.push_back(it->path().generic_string());
      }
      if (error) error.clear(); // tolerate unreadable subtrees; hits so far stay valid
    }

    // The pattern itself is a user-provided regex; a bad pattern is a tool
    // error, not a crash.
    std::regex rx;
    try {
      rx.assign(pattern);
    } catch (const std::exception& exception) {
      return ToolResult{false, "", std::string("grep: invalid pattern: ") + exception.what()};
    }

    std::vector<std::string> hits;
    constexpr std::size_t kMaxHits = 200;
    constexpr std::uintmax_t kMaxFileSize = 5 * 1024 * 1024; // skip huge binaries
    bool hit_cap_reached = false;
    for (const std::string& target : targets) {
      if (hit_cap_reached) break;
      std::error_code size_error;
      if (fs::file_size(target, size_error) > kMaxFileSize) continue;
      std::ifstream file(target, std::ios::binary);
      if (!file.is_open()) continue;
      std::string line;
      long line_number = 0;
      while (std::getline(file, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        try {
          if (!std::regex_search(line, rx)) continue;
        } catch (const std::exception&) {
          continue; // malformed line content; keep scanning
        }
        hits.push_back(target + ":" + std::to_string(line_number) + ": " + line);
        if (hits.size() >= kMaxHits) {
          hits.push_back("… stop after " + std::to_string(kMaxHits) + " matches (results cut short)");
          hit_cap_reached = true;
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
