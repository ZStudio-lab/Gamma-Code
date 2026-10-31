#include "tools/registry.hpp"

#include <string>

#include <nlohmann/json.hpp>

#include "ui/render.hpp"

namespace gamma::tools {

// ask — hand the turn to the human. The REPL is paused; the model receives
// exactly what the user typed (or the picked option) as the tool result.
Tool make_ask_tool() {
  Tool tool;
  tool.name = "ask";
  tool.description =
      "Ask the user a question and wait for their answer. Use when a decision needs human input (requirements, trade-offs, confirmation of something destructive or ambiguous). Provide 2-4 concrete options when possible; free text is allowed. Do not use it for information you can find with the other tools.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"question",
            {
                {"type", "string"},
                {"description", "The question to show the user (specific and self-contained)"},
            }},
           {"options",
            {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description",
                 "2-4 concrete choices; the user can also type something else. Omit for free text."},
            }},
       }},
      {"required", nlohmann::json::array({"question"})},
  };
  tool.readonly = true; // pauses for input but changes nothing on disk

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    if (!arguments.contains("question") || !arguments["question"].is_string() ||
        arguments["question"].get<std::string>().empty()) {
      return {false, "", "ask requires a non-empty 'question' string"};
    }
    const std::string question = arguments["question"].get<std::string>();

    std::vector<std::string> options;
    if (arguments.contains("options") && arguments["options"].is_array()) {
      for (const nlohmann::json& option : arguments["options"]) {
        if (option.is_string() && !option.get<std::string>().empty())
          options.push_back(option.get<std::string>());
        if (options.size() >= 6) break; // keep the picker readable
      }
    }
    if (options.size() == 1) options.clear(); // a single "option" is not a choice

    const ui::UserAnswer answer = ui::ask_user_question(question, options);
    if (!answer.answered) {
      return ToolResult{false, "",
                        "ask: nobody is available to answer (non-interactive run) — proceed"
                        " with the safest reasonable choice and say so plainly in your reply"};
    }
    if (answer.text.empty()) {
      return ToolResult{true, "user answered: (empty — they skipped the question)", ""};
    }
    if (!options.empty()) {
      return ToolResult{true, "user answered: " + answer.text, ""};
    }
    return ToolResult{true, "user answered: " + answer.text, ""};
  };

  return tool;
}

} // namespace gamma::tools
