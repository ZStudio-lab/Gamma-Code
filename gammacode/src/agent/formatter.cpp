#include "agent/formatter.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gamma {

namespace {

constexpr std::size_t kTagOpenWidth = 11;  // "<tool_call>" = 10 wide bytes + 1 from find()
constexpr std::size_t kTagCloseWidth = 12; // "<tool_call>" = 11 wide bytes + 1 from find()

struct Extracted {
  ToolCall call;
  std::size_t begin = 0;
  std::size_t end = 0; // exclusive
};

struct Span {
  std::size_t begin = 0;
  std::size_t end = 0; // exclusive
};

std::string trim_ascii(const std::string& text) {
  auto is_space = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };
  std::size_t begin = 0;
  while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// Parses one candidate JSON value into a ToolCall. Accepts {"name",
// "arguments"} (Ollama shape), {"function": {"name", "arguments"}} (OpenAI
// envelope), and `arguments` delivered as a JSON-encoded object string.
// Never parses the same JSON twice.
bool call_from_json(const nlohmann::json& parsed, ToolCall& call) {
  if (!parsed.is_object()) return false;

  const nlohmann::json* envelope = &parsed;
  if (parsed.contains("function") && parsed["function"].is_object()) {
    envelope = &parsed["function"];
  }
  if (!envelope->contains("name") || !(*envelope)["name"].is_string()) return false;
  if (!envelope->contains("arguments")) return false;

  nlohmann::json arguments = (*envelope)["arguments"];
  if (arguments.is_string()) {
    try {
      arguments = nlohmann::json::parse(arguments.get<std::string>());
    } catch (const std::exception&) {
      return false;
    }
  }
  if (!arguments.is_object()) return false;

  call.name = (*envelope)["name"].get<std::string>();
  if (call.name.empty()) return false;
  call.arguments = std::move(arguments);
  return true;
}

// First JSON object in `text` starting at or after `from`. Uses brace depth
// so strings containing braces don't fool the scanner.
bool find_json_object(const std::string& text, std::size_t from, std::size_t& begin_out,
                      std::size_t& end_out) {
  const std::size_t begin = text.find('{', from);
  if (begin == std::string::npos) return false;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (std::size_t i = begin; i < text.size(); ++i) {
    const char c = text[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (in_string) {
      if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) {
        begin_out = begin;
        end_out = i + 1;
        return true;
      }
    }
  }
  return false;
}

// Pulls every tool-call-looking JSON object out of a plain-text region
// (used bare, and inside fences/tags). Prose containing braces is skipped.
bool extract_from_region(const std::string& region, std::vector<Extracted>& calls) {
  bool any = false;
  std::size_t from = 0;
  while (true) {
    std::size_t begin = 0;
    std::size_t end = 0;
    if (!find_json_object(region, from, begin, end)) break;
    ToolCall call;
    try {
      if (call_from_json(nlohmann::json::parse(region.substr(begin, end - begin)), call)) {
        calls.push_back(Extracted{std::move(call), begin, end});
        any = true;
        from = end;
        continue;
      }
    } catch (const std::exception&) {
      // not JSON; keep scanning
    }
    from = begin + 1;
  }
  return any;
}

bool fence_label_matches(const std::string& info) {
  std::string lowered = trim_ascii(info);
  for (char& c : lowered) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return lowered.empty() || lowered == "json" || lowered == "tool_call" ||
         lowered == "tool-call" || lowered == "toolcall";
}

} // namespace

std::vector<ToolCall> parse_tool_calls_from_text(const std::string& content,
                                                 std::string& textual_remainder) {
  textual_remainder.clear();
  std::vector<ToolCall> calls;
  std::vector<Span> command_spans;
  std::vector<Span> dead_spans; // regions whose JSON must STAY prose (e.g. ```cpp fences)

  // Pass 1: fenced blocks (```json / ```tool_call / bare fences). Real code
  // fences like ```cpp stay in the message untouched.
  for (std::size_t cursor = 0; cursor < content.size();) {
    const std::size_t open = content.find("```", cursor);
    if (open == std::string::npos) break;
    const std::size_t label_end = content.find('\n', open);
    if (label_end == std::string::npos) break;
    const std::string label = content.substr(open + 3, label_end - (open + 3));
    if (!fence_label_matches(label)) {
      // Real code fence: its whole region is prose; pass 3 will skip it.
      const std::size_t close = content.find("```", label_end + 1);
      if (close == std::string::npos) break;
      dead_spans.push_back(Span{open, close + 3});
      cursor = close + 3;
      continue;
    }
    const std::size_t close = content.find("```", label_end + 1);
    if (close == std::string::npos) break;

    const std::string region = content.substr(label_end + 1, close - (label_end + 1));
    std::vector<Extracted> found;
    if (extract_from_region(region, found)) {
      for (Extracted& item : found) {
        command_spans.push_back(Span{open, close + 3});
        calls.push_back(std::move(item.call));
      }
      cursor = close + 3;
    } else {
      cursor = label_end + 1;
    }
  }

  // Pass 2: <tool_call> ... <tool_call> envelopes (Qwen/Hermes-style text commands).
  for (std::size_t cursor = 0; cursor < content.size();) {
    const std::size_t open = content.find("<tool_call>", cursor);
    if (open == std::string::npos) break;
    const std::size_t body_start = open + kTagOpenWidth;
    if (body_start > content.size()) break;
    const std::size_t close = content.find("<tool_call>", body_start);
    if (close == std::string::npos) break;

    const std::string region = content.substr(body_start, close - body_start);
    std::vector<Extracted> found;
    if (extract_from_region(region, found)) {
      for (Extracted& item : found) {
        command_spans.push_back(Span{open, close + kTagCloseWidth});
        calls.push_back(std::move(item.call));
      }
      cursor = close + kTagCloseWidth;
    } else {
      dead_spans.push_back(Span{open, close + kTagCloseWidth}); // a data block, not a command
      cursor = body_start;
    }
  }

  // Pass 3: bare JSON command objects directly in plain text (no fences, no
  // tags) — covers single-line, multi-call, and trailing-prose emissions.
  if (command_spans.empty()) {
    std::size_t from = 0;
    while (true) {
      std::size_t begin = 0;
      std::size_t end = 0;
      if (!find_json_object(content, from, begin, end)) break;
      ToolCall call;
      try {
        if (call_from_json(nlohmann::json::parse(content.substr(begin, end - begin)), call)) {
          const bool in_dead_zone =
              std::any_of(dead_spans.begin(), dead_spans.end(), [&](const Span& span) {
                return begin < span.end && span.begin < end;
              });
          if (in_dead_zone) {
            from = end; // still advance: don't rescan the same region
          } else {
            command_spans.push_back(Span{begin, end});
            calls.push_back(std::move(call));
            from = end;
          }
          continue;
        }
      } catch (const std::exception&) {
        // not JSON; keep scanning
      }
      from = begin + 1;
    }
    if (calls.empty()) return calls; // pure prose (or dead-zone-only JSON)
  }

  // Keep the prose that lives outside all command spans, in order.
  std::sort(command_spans.begin(), command_spans.end(),
            [](const Span& a, const Span& b) { return a.begin < b.begin; });
  std::string below;
  std::size_t cursor = 0;
  for (const Span& span : command_spans) {
    if (span.begin < cursor) continue; // overlap safety
    below += content.substr(cursor, span.begin - cursor);
    cursor = span.end;
  }
  below += content.substr(cursor);
  textual_remainder = trim_ascii(below);
  return calls;
}

} // namespace gamma
