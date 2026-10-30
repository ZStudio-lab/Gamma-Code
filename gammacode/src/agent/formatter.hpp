#pragma once

#include <string>
#include <vector>

#include "agent/messages.hpp"

namespace gamma {

// The command formatter: overlays the model's raw text and extracts tool
// calls so the app gets the AI's commands even when the model answers with
// text instead of the structured `tool_calls` field.
//
// Recognized shapes (any mix, multiple allowed):
//   {"name": "read_file", "arguments": {"path": "x"}}   whole content, bare
//   ```json {"name": ..., "arguments": {...}} ```       fenced JSON blocks
//   ```tool_call ...```                                 fenced tool_call blocks
//   {"function": {"name": ..., "arguments": {...}}}     OpenAI-style envelope
//   arguments as a JSON-encoded object STRING           parsed exactly once
//
// `textual_remainder` receives whatever prose was outside the extracted
// blocks (empty when the whole content was command text).
std::vector<ToolCall> parse_tool_calls_from_text(const std::string& content,
                                                 std::string& textual_remainder);

} // namespace gamma
