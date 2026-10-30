#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

namespace gamma {

// Every tool result — and every failure inside a tool, including parse
// errors — funnels through this struct. No exceptions cross the tool
// boundary.
struct ToolResult {
  bool ok = false;
  std::string output;
  std::string error;
};

struct Tool {
  std::string name;
  std::string description;
  nlohmann::json schema; // JSON Schema for `parameters`, sent to Ollama
  std::function<ToolResult(const nlohmann::json& arguments)> run;

  // Permission-gate attributes:
  bool readonly = false;       // auto-approved in ask mode
  bool prompt_in_auto = false; // still prompts in auto mode (run_command)
};

class Registry {
public:
  void add(Tool tool);

  const Tool* find(const std::string& name) const;

  // The "tools" request payload for /api/chat.
  nlohmann::json tool_schemas() const;

private:
  // unique_ptr storage keeps the lookup pointers stable across add() calls
  // (plain vector reallocation would otherwise dangle them). No raw ownership.
  std::vector<std::unique_ptr<Tool>> tools_;
  std::unordered_map<std::string, const Tool*> index_;
};

namespace tools {

// Registered at startup; grows every build step.
Tool make_read_file_tool();
Tool make_write_file_tool();
Tool make_edit_file_tool();
Tool make_list_dir_tool();
Tool make_glob_tool();
Tool make_grep_tool();
Tool make_run_command_tool();
Registry default_registry();

// The permission gate. `confirm` prints the full proposed action and waits
// for y/N (the human's voice in ask/auto flows). Denials are tool errors,
// never crashes.
bool permitted(const Tool& tool, const nlohmann::json& arguments, Mode mode, bool yolo,
               const std::function<bool(const Tool&, const nlohmann::json&)>& confirm);

// Tool path arguments resolve inside the workspace root (see the rule table
// above); every fs tool routes its "path" through this.
std::string resolve_tool_path(const std::string& path);

} // namespace tools

} // namespace gamma
