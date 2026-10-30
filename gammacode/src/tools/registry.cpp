#include "tools/registry.hpp"

#include <iostream>
#include <utility>

namespace gamma {

void Registry::add(Tool tool) {
  tools_.push_back(std::make_unique<Tool>(std::move(tool)));
  const std::string name = tools_.back()->name;
  index_[name] = tools_.back().get();
}

const Tool* Registry::find(const std::string& name) const {
  const auto it = index_.find(name);
  return (it == index_.end()) ? nullptr : it->second;
}

nlohmann::json Registry::tool_schemas() const {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& tool : tools_) {
    out.push_back({
        {"type", "function"},
        {"function",
         {
             {"name", tool->name},
             {"description", tool->description},
             {"parameters", tool->schema},
         }},
    });
  }
  return out;
}

namespace tools {

// Resolves a tool's path argument against the workspace root. This is the
// anti-"/path/to/.." rule:
//   ""              → the workspace root itself
//   relative        → rooted inside the workspace (e.g. "notes.txt" and
//                     "../x" both stay anchored to the selected folder)
//   absolute no-drv → "C:/…/workspace/<tail>" (LLMs love emitting
//                     "/path/to/file"; that lands IN the workspace)
//   absolute drive  → used verbatim: "D:\work\..." is an explicit place
// Lexically normalized first; no exceptions cross the tool boundary.
std::string resolve_tool_path(const std::string& raw);

Registry default_registry() {
  Registry registry;
  registry.add(make_read_file_tool());
  registry.add(make_write_file_tool());
  registry.add(make_edit_file_tool());
  registry.add(make_list_dir_tool());
  registry.add(make_glob_tool());
  registry.add(make_grep_tool());
  registry.add(make_run_command_tool());
  // Step 3+ adds: ask_user.
  return registry;
}

std::string resolve_tool_path(const std::string& raw) {
  namespace fs = std::filesystem;
  if (raw.empty()) return workspace_root().string();
  const fs::path given = fs::path(raw).lexically_normal();
  const bool driveless_root = !given.root_path().empty() && given.root_name().empty();
  if (given.is_absolute() && !driveless_root) {
    // Full "C:\..."-style location: an explicitly named place, verbatim.
    return given.lexically_normal().string();
  }
  // Everything else — relative paths AND drive-less "/path/to/…" style
  // absolutes (the classic model artifact) — lands inside the workspace.
  // On Windows, appending a path with a root would replace the root
  // entirely, so only the tail is appended.
  return (workspace_root() / given.relative_path()).lexically_normal().string();
}

bool permitted(const Tool& tool, const nlohmann::json& arguments, Mode mode, bool yolo,
               const std::function<bool(const Tool&, const nlohmann::json&)>& confirm) {
  switch (mode) {
  case Mode::Plan:
    if (!tool.readonly) {
      std::cout << "  ✗ " << tool.name
                << " is not allowed in plan mode (plan permits read-only tools only: "
                   "read_file, list_dir, glob, grep, ask_user)\n";
      return false;
    }
    return true;
  case Mode::Ask:
    if (tool.readonly)
      return true;
    return confirm(tool, arguments);
  case Mode::Auto:
    if (!tool.prompt_in_auto || yolo)
      return true;
    return confirm(tool, arguments);
  }
  return false;
}

} // namespace tools

} // namespace gamma
