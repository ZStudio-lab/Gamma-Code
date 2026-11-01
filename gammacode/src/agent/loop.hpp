#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "ollama/client.hpp"
#include "tools/registry.hpp"

namespace gamma {

// The shared system prompt applied automatically to every conversation.
const char* system_prompt_text();

// Everything a front-end needs to render a turn live. The console front-end
// prints these events itself; the native webview chat installs a sink that
// marshals them into the window. Console output is untouched when no sink
// is set.
class TurnSink {
 public:
  virtual ~TurnSink() = default;
  // The stream-safe prefix of the assistant message so far (tool-call JSON
  // is held back exactly like the console streamer holds it).
  virtual void on_delta(const std::string& streamable_text) = 0;
  // "thinking…", "searching text…", "waiting for your answer…" — deduped.
  virtual void on_status(const std::string& label) = 0;
  // A tool call is about to run (permission gate applies afterwards).
  virtual void on_tool(const std::string& name, const nlohmann::json& arguments) = 0;
  virtual void on_tool_result(const std::string& name, bool ok) = 0;
  // The complete assistant text of a turn that ended without tool calls.
  virtual void on_final(const std::string& content) = 0;
  // Compaction notices, stop notices, wire errors.
  virtual void on_notice(const std::string& text, bool error) = 0;
};

// One agentic turn: send history to Ollama, execute any tool calls
// (permission gate applies), append results, loop until the model answers
// without tool calls. Hard-capped at max_tool_iterations per user turn.
class AgentLoop {
public:
  // Approval prompt hook (prints the full proposal, waits for y/N).
  using ConfirmFn = std::function<bool(const Tool&, const nlohmann::json&)>;

  AgentLoop(const Options& options, OllamaClient& client, const Registry& registry,
            ConfirmFn confirm);

  // Live UI sink (null = console front-end owns the visuals). The sink is
  // called on whatever thread runs run_turn().
  void set_sink(TurnSink* sink) { sink_ = sink; }

  // Runs a single user turn ("user_text" is appended to history first).
  // Returns false only when the iteration cap forced a stop.
  bool run_turn(const std::string& user_text);

  // Token/message snapshot for the /context command.
  struct Stats {
    long tokens = 0;
    std::size_t messages = 0;
    long budget = 0;
  };
  Stats stats() const;

  const std::vector<Message>& history() const { return history_; }

  // /clear: restart the conversation with just the system prompt.
  void reset_history();

  // Session persistence (step 4): save/load the conversation (everything
  // except the system prompt, which is rebuilt) as a local JSON file so the
  // next launch resumes where it left off. save writes atomically
  // (temp file first); load returns false — leaving history untouched —
  // when the file is missing/unreadable/structurally empty.
  bool save_session(const std::string& path) const;
  bool load_session(const std::string& path);

  // Context compaction (step 3). Tier A trims old tool-result payloads; tier
  // B asks the model to summarize the older part of the conversation into a
  // single note. With force=false it is only active near the budget
  // (checked between loop iterations); force=true runs at least tier A (the
  // /compact command). Returns true when history actually changed.
  bool compact(bool force = false);

private:
  nlohmann::json build_request() const;
  // Appends role:"tool" with {"ok","output","error"} JSON, truncated to 8 KB.
  void append_tool_result(const Tool* tool, const std::string& tool_name, const ToolResult& result);
  // Tier A of compact(): shorten all but the last keep_recent tool payloads.
  std::size_t trim_tool_results(std::size_t keep_recent, std::size_t keep_output_bytes);

  const Options& options_;
  OllamaClient& client_;
  const Registry& registry_;
  ConfirmFn confirm_;
  TurnSink* sink_ = nullptr;
  std::vector<Message> history_;
};

} // namespace gamma
