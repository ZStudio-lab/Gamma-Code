#include "agent/messages.hpp"

#include "agent/formatter.hpp"

namespace gamma {

long estimate_tokens(const std::string& text) {
  return static_cast<long>((text.size() + 3) / 4);
}

long estimate_history_tokens(const std::vector<Message>& history) {
  long total = 0;
  for (const Message& message : history) {
    total += estimate_tokens(message.content);
  }
  return total;
}

nlohmann::json messages_to_json(const std::vector<Message>& history) {
  nlohmann::json out = nlohmann::json::array();
  for (const Message& message : history) {
    nlohmann::json entry;
    entry["role"] = message.role;
    entry["content"] = message.content;
    if (!message.tool_calls.empty()) {
      nlohmann::json calls = nlohmann::json::array();
      for (const ToolCall& call : message.tool_calls) {
        calls.push_back({
            {"function",
             {
                 {"name", call.name},
                 {"arguments", call.arguments},
             }},
        });
      }
      entry["tool_calls"] = calls;
    }
    out.push_back(entry);
  }
  return out;
}

std::vector<Message> messages_from_json(const nlohmann::json& array) {
  std::vector<Message> out;
  if (!array.is_array()) return out;
  for (const nlohmann::json& entry : array) {
    if (!entry.is_object()) continue;
    Message message;
    message.role = entry.value("role", "");
    if (message.role.empty()) continue; // no role, no message
    message.content = entry.value("content", "");
    if (entry.contains("tool_calls") && entry["tool_calls"].is_array()) {
      for (const nlohmann::json& raw_call : entry["tool_calls"]) {
        const nlohmann::json function =
            raw_call.contains("function") ? raw_call["function"] : nlohmann::json::object();
        ToolCall call;
        call.name = function.value("name", "");
        // Same defensive unboxing as assistant_from_json: a few finetunes
        // wrap arguments in a string; parse it exactly once, safely.
        nlohmann::json arguments =
            function.contains("arguments") ? function["arguments"] : nlohmann::json::object();
        if (arguments.is_string()) {
          try {
            arguments = nlohmann::json::parse(arguments.get<std::string>());
          } catch (const std::exception&) {
            arguments = nlohmann::json::object();
          }
        }
        call.arguments = arguments.is_object() ? arguments : nlohmann::json::object();
        message.tool_calls.push_back(std::move(call));
      }
    }
    out.push_back(std::move(message));
  }
  return out;
}

Message assistant_from_json(const nlohmann::json& message) {
  Message result;
  result.role = "assistant";
  if (message.contains("content") && message["content"].is_string()) {
    result.content = message["content"].get<std::string>();
  }
  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    for (const nlohmann::json& raw_call : message["tool_calls"]) {
      const nlohmann::json function =
          raw_call.contains("function") ? raw_call["function"] : nlohmann::json::object();
      ToolCall call;
      call.name = function.value("name", "");
      // Ollama sends `arguments` as a JSON object. Models fine-tuned on
      // OpenAI's wire format occasionally send it string-encoded instead;
      // accept that variant, parse it exactly once, and never treat an
      // object as a string.
      nlohmann::json arguments =
          function.contains("arguments") ? function["arguments"] : nlohmann::json::object();
      if (arguments.is_string()) {
        try {
          arguments = nlohmann::json::parse(arguments.get<std::string>());
        } catch (const std::exception&) {
          arguments = nlohmann::json::object();
        }
      }
      call.arguments = arguments.is_object() ? arguments : nlohmann::json::object();
      result.tool_calls.push_back(std::move(call));
    }
  }

  // No structured tool_calls? Run the command formatter over the raw text:
  // fenced ```json blocks, <tool_call> envelopes, and bare JSON command objects all
  // become ToolCalls; leftover prose stays in the message content.
  if (result.tool_calls.empty() && !result.content.empty()) {
    std::string remainder;
    result.tool_calls = parse_tool_calls_from_text(result.content, remainder);
    if (!result.tool_calls.empty()) {
      result.content = remainder;
    }
  }
  return result;
}

} // namespace gamma
