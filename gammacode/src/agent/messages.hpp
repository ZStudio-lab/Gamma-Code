#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace gamma {

// A single model-issued tool invocation. `arguments` is a JSON *object* —
// Ollama delivers it that way (OpenAI sends a string; some fine-tunes inherit
// that habit, and assistant_from_json() unboxes it defensively, parsing
// exactly once).
struct ToolCall {
  std::string name;
  nlohmann::json arguments;
};

struct Message {
  std::string role; // "system" | "user" | "assistant" | "tool"
  std::string content;
  std::vector<ToolCall> tool_calls; // populated on assistant messages
};

// Rough token estimate: chars / 4, rounded up.
long estimate_tokens(const std::string& text);

// Total estimated tokens across a history.
long estimate_history_tokens(const std::vector<Message>& history);

// Serializes history into the Ollama /api/chat message array format.
nlohmann::json messages_to_json(const std::vector<Message>& history);

// Parses an Ollama assistant message (the "message" field of the response).
Message assistant_from_json(const nlohmann::json& message);

// Inverse of messages_to_json for saved sessions: a JSON array of
// {role, content, tool_calls?} objects back into history entries. Skips
// malformed entries; tolerant of missing fields. No formatter runs here —
// saved tool calls are stored structured already.
std::vector<Message> messages_from_json(const nlohmann::json& array);

} // namespace gamma
