#include "ui/theme.hpp"

#include "agent/loop.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <utility>

#include "agent/messages.hpp"
#include "agent/formatter.hpp"
#include "ui/render.hpp"

namespace gamma {

namespace {

// Emitted at the start of every conversation.
// Rules the model must follow: use ONLY the provided toolset, act instead of
// describing, and never answer with "copy this code" — do the edit itself.
// The tool-calling section is deliberately verbose: tiny models (0.5B-3B)
// fail far more often on the CALL MECHANISM than on the task itself.
const char* kSystemPromptText = R"SYSTEM_PROMPT(
You are gamma, a coding agent operating on the user's machine. You complete
coding tasks by acting, not by narrating.

SESSION NAMING:
At the START of every new conversation, you MUST output a session name as your
VERY FIRST message. This name will be used to label the conversation in the UI.
Format: SESSION_NAME: <short descriptive name>
Example: SESSION_NAME: Fix login bug in auth module
The name should be 3-6 words, descriptive of the task. Do NOT include this in
your thinking — output it as plain text before any tool calls or prose.

HOW TOOL CALLS WORK — the mechanism (models fail on this, so read closely):
1. A tool call is made through your tool-calling mechanism as a structured
   call carrying the tool's EXACT name from the list and one arguments
   object. Plain text you write — including JSON pasted into a code fence —
   is only an explanation shown to the user; it NEVER executes anything.
2. Arguments are strict JSON: keys and strings in double quotes, numbers
   unquoted, booleans true/false, arrays with []. No comments, no trailing
   commas, no markdown, no single quotes, no newlines inside keys.
3. Make ONE tool call per response. The tool's result arrives as the very
   next message; decide your next step only after reading it.
4. If a call fails or is rejected, the result says why — correct the mistake
   (typo in the path, wrong argument shape, missing required key) and retry.
   Never repeat the identical failing call unchanged.
5. Never invent tools, and never simulate a tool's output in your reply.

WORKED EXAMPLES — multiple tasks showing the FULL flow (paths are illustrations):

EXAMPLE 1 — Simple read task ("what does main.cpp do?"):
  SESSION_NAME: Explain main.cpp
  step 1  you call:        list_dir  {"path": "."}
          (the next message shows the directory contents)
  step 2  you call:        read_file  {"path": "src/main.cpp"}
          (the next message contains the file contents)
  step 3  task done — you reply in plain prose with NO tool call:
          "main.cpp is the entry point. It initializes the server..."

EXAMPLE 2 — Edit task ("rename tick() to update() in src/main.cpp"):
  SESSION_NAME: Rename tick to update in main
  step 1  you call:        read_file  {"path": "src/main.cpp"}
          (the next message contains the file contents)
  step 2  you call:        edit_file  {"path": "src/main.cpp",
                                      "old_string": "void tick() {",
                                      "new_string": "void update() {"}
          (the next message says the edit succeeded)
  step 3  task done — you reply in plain prose with NO tool call:
          "Renamed tick() to update() in src/main.cpp."

EXAMPLE 3 — Multi-file task ("add logging to all .cpp files"):
  SESSION_NAME: Add logging to all cpp files
  step 1  you call:        glob  {"pattern": "**/*.cpp"}
          (the next message lists all .cpp files)
  step 2  you call:        read_file  {"path": "src/a.cpp"}
          (read each file, then edit)
  step 3  you call:        edit_file  {"path": "src/a.cpp",
                                      "old_string": "#include <iostream>",
                                      "new_string": "#include <iostream>\n#include \"logger.h\""}
          (repeat for each file)
  step N  task done — you reply in plain prose with NO tool call:
          "Added logging to 5 cpp files."

Actions, not words: description in prose instead of steps is a failure.

FINISHING:
- When the task is complete — or the question is answerable without tools —
  send a short plain prose reply with NO tool call. That reply is the answer
  the user sees. A finished task must not end with another call.

TOOLS — syntax shape ONLY (values in <> are placeholders, NOT real names; see
the SHAPE rule below):
  read_file    {"path": "<file>", "start_line": 1, "end_line": 120}
               start_line/end_line are optional — omit them to read the file whole
  write_file   {"path": "<file>", "content": "<entire new file text>"}
  edit_file    {"path": "<file>", "old_string": "<exact text taken from a read_file>",
                "new_string": "<replacement text>"}
               plus optional "replace_all": true (default false)
  list_dir     {"path": "<folder>"}          (optional; {"path": "."} lists the root)
  glob         {"pattern": "<wildcard>", "path": "<folder>"}   (path optional)
  grep         {"pattern": "<text or regex>", "path": "<folder>", "glob": "<mask>"}
               only "pattern" is required; path and glob are optional
  run_command  {"cmd": "<command line>", "timeout_seconds": 300}
               timeout_seconds optional (default set by gamma)
  ask          {"question": "<one question>", "options": ["<choice>", "<choice>"]}
               options optional; or just ask and wait
  web_search   {"query": "<keywords>", "max_results": 5}       (max_results optional)
Keys not shown, optional keys with no value, or invented keys are rejected.

SHAPE RULE (violations wasted turns every time — read twice):
The JSON above and in WORKED EXAMPLES shows the SHAPE of a call. Paths like
"<file>" and names like "src/app.cpp" are ILLUSTRATIONS — they may not exist
in this workspace. Real values come only from discovery: START file work
with list_dir ({"path": "."}) or glob to learn what actually exists, THEN
read/edit the real names you saw in tool output. NEVER pass a path you have
not seen from a tool result or from the user.
No other tools exist and none may be invented or referenced.

TOOL CALL TRAINING — common mistakes to AVOID:
✗ WRONG:  "I'll read the file now" + read_file {"path": "src/main.cpp"}
  (prose + tool call in same response — only the tool call executes)
✗ WRONG:  read_file {"path": "src/main.cpp"}  "I'm reading the file"
  (prose after tool call — just make the call, result comes next)
✗ WRONG:  read_file {"path": "src/main.cpp"} then edit_file {...}
  (two tool calls in one response — only the first executes)
✗ WRONG:  {"path": "src/main.cpp"}  (missing tool name — must use mechanism)
✓ CORRECT:  read_file {"path": "src/main.cpp"}  (ONE call, pure JSON args)
✓ CORRECT:  [tool result arrives]  read_file {"path": "src/other.cpp"}
  (wait for result, then next call)

RULES:
1. Use ONLY the tools listed above; every required key, no extra keys,
   values that satisfy the SHAPE rule — real paths from real tool output.
   Paths are relative to the WORKING DIRECTORY below.
2. ALWAYS read a file (or the region) before editing it: edit_file needs
   old_string copied EXACTLY from the current file text, byte for byte,
   and it must appear exactly once for a clean edit.
3. After edits, verify: re-read the changed region or run a build/tests
   with run_command before claiming success.
4. NEVER tell the user to copy, paste, or apply code themselves. Any change
   that belongs on disk goes through write_file or edit_file, performed by
   you, before you finish.
5. Never claim you did something you did not do via a tool. Tool calls and
   their results are shown to the user automatically — do not paste tool
   output or long file contents back into your reply.
6. If a task needs a decision only the user can make, call ask with a
   specific question and concrete options instead of guessing. For facts
   the codebase cannot answer (docs, versions, error messages), use
   web_search — the query leaves the machine, so keep it free of code and
   secrets.
7. Large projects: scans (glob, grep) skip .git, dependency folders and
   gitignored paths, and long results come back capped with a hint. When
   results are capped, narrow the search (grep a subfolder, glob a stricter
   pattern, read a specific range) instead of retrying it unchanged.
8. Work step by step: explore first, then change, then verify. In each
   step, prefer the smallest tool that answers the question.)SYSTEM_PROMPT";

constexpr int kMaxIterations = 40;
constexpr std::size_t kToolResultLimit = 8 * 1024; // 8 KB

// The prompt is rebuilt per conversation so the WORKING DIRECTORY section
// always reflects the live workspace (it can even change between sessions).
std::string make_system_prompt(const Options& options) {
  std::error_code error;
  std::string directory = options.workspace;
  if (directory.empty()) {
    directory = std::filesystem::current_path(error).string();
  } else {
    // Windows paths arrive with \; normalize forward so models do not get
    // confused by escaped quotes when they echo paths back.
  }
  if (directory.empty()) directory = ".";
  return std::string(kSystemPromptText) +
         "\n\nWORKING DIRECTORY (the only place your tool paths resolve in):\n"
         "  " + directory +
         "\nThe paths in the tool list above are shape examples — real files live inside"
         "\nthe working directory above. Relative paths such as \"src/main.cpp\" resolve"
         "\nagainst it; use \"list_dir\" with an empty/\".\" path to see what is actually there.";
}

Message make_system_message(const Options& options) {
  Message system;
  system.role = "system";
  system.content = make_system_prompt(options);
  return system;
}

} // namespace

const char* system_prompt_text() { return kSystemPromptText; }

// --- context compaction (step 3) ----------------------------------------------

std::size_t AgentLoop::trim_tool_results(std::size_t keep_recent,
                                         std::size_t keep_output_bytes) {
  // Collect tool-message indices first; erasing never happens here, only
  // in-place content replacement, so indices stay valid.
  std::vector<std::size_t> tool_indices;
  for (std::size_t i = 0; i < history_.size(); ++i) {
    if (history_[i].role == "tool") tool_indices.push_back(i);
  }
  if (tool_indices.size() <= keep_recent) return 0;

  const std::size_t to_trim = tool_indices.size() - keep_recent;
  const std::string marker = " … [trimmed: older tool result]";
  std::size_t changed = 0;
  for (std::size_t i = 0; i < to_trim; ++i) {
    Message& tool_message = history_[tool_indices[i]];
    nlohmann::json payload;
    try {
      payload = nlohmann::json::parse(tool_message.content);
    } catch (const std::exception&) {
      continue; // not a parseable payload; leave untouched
    }
    if (!payload.is_object() || !payload.contains("output") || !payload["output"].is_string()) {
      continue;
    }
    std::string output = payload["output"].get<std::string>();
    if (output.size() <= keep_output_bytes) continue;
    output = output.substr(0, keep_output_bytes) + marker;
    payload["output"] = output;
    tool_message.content = payload.dump();
    ++changed;
  }
  return changed;
}

bool AgentLoop::compact(bool force) {
  const long budget = budget_tokens(options_);
  long total = estimate_history_tokens(history_);
  if (!force && total <= budget) return false; // only force runs any time

  bool changed = false;

  // Tier A: old tool payloads get truncated (they are the usual bulk).
  const std::size_t trimmed = trim_tool_results(4, 600);
  if (trimmed > 0) {
    changed = true;
    total = estimate_history_tokens(history_);
    if (sink_ != nullptr) {
      sink_->on_notice("compacted: trimmed " + std::to_string(trimmed) +
                       " old tool result(s) · ≈" + std::to_string(total) + " tokens",
                       false);
    } else {
      std::cout << "  " << gamma::ui::theme::paint("► compacted", gamma::ui::theme::Role::Accent)
                << gamma::ui::theme::paint(": trimmed ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(trimmed), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint(" old tool result(s) · ≈", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(total), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint(" tokens", gamma::ui::theme::Role::Muted) << "\n";
    }
  }

  // Tier B: summarize everything except the system prompt and the last few
  // exchanges into a single note the conversation can rely on. Beautiful and
  // lossy is fine — 8 KB payloads can always be re-read from disk.
  if (total > budget || (force && history_.size() > 8)) {
    constexpr std::size_t kKeepRecent = 6; // untouched tail
    if (history_.size() <= kKeepRecent + 1) return changed;

    std::string transcript;
    for (std::size_t i = 1; i + kKeepRecent < history_.size(); ++i) {
      std::string line = "- " + history_[i].role + ": ";
      std::string content = history_[i].content;
      if (content.size() > 400) content = content.substr(0, 400) + "…";
      for (const ToolCall& call : history_[i].tool_calls) {
        line += "[" + call.name + "] ";
      }
      line += content;
      transcript += line + "\n";
    }

    nlohmann::json request;
    request["model"] = options_.model;
    request["messages"] = nlohmann::json::array({
        nlohmann::json{{"role", "user"},
                       {"content",
                        "Summarize this coding-agent conversation so far. Keep: files touched"
                        " and their purpose, key decisions, current state, unfinished next steps."
                        " Under 300 words. Transcript:\n" + transcript}},
    });
    Message summary_message;
    {
      ui::Spinner spinner;
      spinner.start();
      spinner.set_label("compacting history…");
      try {
        summary_message = client_.chat(request, [](const std::string&) { return true; });
      } catch (const std::exception& error) {
        const std::string notice = "compaction summary failed (" + std::string(error.what()) +
                                   "); only tool results were trimmed.";
        if (sink_ != nullptr) {
          sink_->on_notice(notice, true);
        } else {
          std::cout << "  "
                    << gamma::ui::theme::paint("✗ " + notice, gamma::ui::theme::Role::Error)
                    << "\n";
        }
        return changed;
      }
    }

    std::string summary_text = summary_message.content;
    if (summary_text.size() > 4000) summary_text = summary_text.substr(0, 4000) + "…";
    Message summary;
    summary.role = "user";
    summary.content =
        "[gamma context summary — older messages were compacted into this note]\n" + summary_text;

    const auto insert_at = history_.begin() + 1;
    const auto range_end = history_.begin() + static_cast<long>(history_.size() - kKeepRecent);
    const long before = estimate_history_tokens(history_);
    history_.erase(insert_at, range_end);
    history_.insert(insert_at, std::move(summary));
    // The untouched tail can still hold full-size tool payloads; cap them so
    // summary + tail actually fit the budget (results are re-readable from
    // disk, and facts survive in the summary).
    trim_tool_results(0, 600);
    const long after = estimate_history_tokens(history_);
    changed = true;
    if (sink_ != nullptr) {
      sink_->on_notice("compacted: " + std::to_string(history_.size() - 1) +
                       " exchanges summarized · ≈" + std::to_string(before) + "→" +
                       std::to_string(after) + " tokens", false);
    } else {
      std::cout << "  " << gamma::ui::theme::paint("► compacted", gamma::ui::theme::Role::Accent)
                << gamma::ui::theme::paint(": ", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(history_.size() - 1), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint(" exchanges summarized · ≈", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(before), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint("→", gamma::ui::theme::Role::Muted)
                << gamma::ui::theme::paint_bold(std::to_string(after), gamma::ui::theme::Role::Highlight)
                << gamma::ui::theme::paint(" tokens", gamma::ui::theme::Role::Muted) << "\n";
    }
  }
  return changed;
}

AgentLoop::AgentLoop(const Options& options, OllamaClient& client, const Registry& registry,
                     ConfirmFn confirm)
    : options_(options), client_(client), registry_(registry), confirm_(std::move(confirm)) {
  history_.push_back(make_system_message(options_));
}

void AgentLoop::reset_history() {
  if (!history_.empty() && history_.front().role == "system") {
    history_.erase(history_.begin() + 1, history_.end());
  } else {
    history_.clear();
  }
  history_.push_back(make_system_message(options_));
}

// Re-syncs the system prompt after options change (workspace switched via
// the GUI picker) without touching the conversation itself.
void AgentLoop::refresh_system_message() {
  if (!history_.empty() && history_.front().role == "system") {
    history_.front().content = make_system_prompt(options_);
  }
}

// --- session persistence -------------------------------------------------------

bool AgentLoop::save_session(const std::string& path) const {
  // Everything except the system prompt; it is rebuilt from code on load so
  // prompt improvements apply to resumed sessions too.
  std::size_t first = 0;
  if (!history_.empty() && history_.front().role == "system") first = 1;

  nlohmann::json file;
  file["format"] = "gamma-session";
  file["version"] = 1;
  file["model"] = options_.model;
  file["messages"] =
      messages_to_json(std::vector<Message>(history_.begin() + static_cast<long>(first),
                                            history_.end()));

  // Atomic-ish write: temp file, then swap in. A crash mid-write can never
  // corrupt the previous session.
  const std::filesystem::path temp = path + ".tmp";
  try {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << file.dump(2);
    out.flush();
    if (!out.good()) return false;
    out.close();
    std::error_code error;
    std::filesystem::remove(path, error); // Windows rename refuses existing targets
    std::filesystem::rename(temp, path, error);
    return !error;
  } catch (const std::exception&) {
    return false;
  }
}

bool AgentLoop::load_session(const std::string& path) {
  nlohmann::json file;
  try {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    file = nlohmann::json::parse(in);
  } catch (const std::exception&) {
    return false; // unreadable or corrupt: start fresh, never half-load
  }
  std::vector<Message> loaded =
      messages_from_json(file.contains("messages") ? file["messages"] : nlohmann::json{});
  if (loaded.empty()) return false;

  history_.clear();
  history_.push_back(make_system_message(options_));
  for (Message& message : loaded) {
    if (message.role == "system") continue; // never load a stale system entry
    history_.push_back(std::move(message));
  }
  return true;
}

AgentLoop::Stats AgentLoop::stats() const {
  Stats snapshot;
  snapshot.tokens = estimate_history_tokens(history_);
  snapshot.messages = history_.size();
  snapshot.budget = budget_tokens(options_);
  return snapshot;
}

nlohmann::json AgentLoop::build_request() const {
  nlohmann::json request;
  request["model"] = options_.model;
  request["messages"] = messages_to_json(history_);
  request["tools"] = registry_.tool_schemas();
  request["stream"] = false;
  return request;
}

// Appends role:"tool" holding {"ok","output","error"} JSON text. The payload
// is capped at 8 KB by shrinking the output FIELD (never the JSON envelope),
// so compaction can always parse these payloads back later.
void AgentLoop::append_tool_result(const Tool* tool, const std::string& tool_name,
                                   const ToolResult& result) {
  nlohmann::json serialized;
  serialized["ok"] = result.ok;
  if (!tool_name.empty())
    serialized["name"] = tool_name;
  // Truncate to 8 KB WITHOUT breaking payload JSON: shrink the "output"
  // field itself (tier A of compaction needs to parse these payloads later).
  if (!result.error.empty())
    serialized["error"] = result.error;
  if (!result.output.empty()) {
    std::string output = result.output;
    while (true) {
      serialized["output"] = output;
      if (serialized.dump().size() <= kToolResultLimit || output.empty()) break;
      const std::size_t keep =
          (output.size() * 3) / 4; // geometric shrink — always terminates
      output = output.substr(0, keep) + "\n… [tool result trimmed to fit the output budget]";
    }
  }

  const std::string payload = serialized.dump();

  Message tool_message;
  tool_message.role = "tool";
  tool_message.content = payload;
  history_.push_back(std::move(tool_message));
  (void)tool;
}

bool AgentLoop::run_turn(const std::string& user_text) {
  Message user;
  user.role = "user";
  user.content = user_text;
  history_.push_back(std::move(user));

  int iterations = 0;
  std::string last_error;
  std::string turn_failure;
  for (;;) {
    if (++iterations > kMaxIterations) {
      const std::string notice = "per-turn iteration cap (" + std::to_string(kMaxIterations) +
                                 ") reached; stopping this turn so nothing spins forever.";
      if (sink_ != nullptr) {
        sink_->on_notice(notice, false);
      } else {
        std::cout << "  "
                  << gamma::ui::theme::paint("⚠ " + notice, gamma::ui::theme::Role::Warn)
                  << "\n";
      }
      return false;
    }

    // Compaction fires only once history reaches the budget; between
    // iterations is the only safe point (never mid-stream or mid-approval).
    compact(false);

    bool user_stopped = false;
    Message assistant;
    std::size_t streamed = 0; // bytes of assistant.content already shown live
    {
      ui::Spinner spinner;
      ui::StreamStopWindow stop_window; // while streaming: ESC / Ctrl+C stop
      // Live prose: safe chunks print as they arrive; the first one stops the
      // spinner before touching stdout so the two never overwrite each other.
      ui::StreamPrinter printer([&spinner] { spinner.stop(); });
      if (sink_ == nullptr) spinner.start(); // console-only visuals
      std::string last_label;                // webview sink: deduped status
      std::size_t last_shown = 0;            // webview sink: deduped prefix
      // Stalled-stream timeouts are worth an automatic retry: a cold model
      // (first load) or a busy Ollama often recovers within seconds. Retries
      // only fire while NOTHING has streamed yet — a partial answer can't be
      // replayed without duplicating text the user already saw.
      constexpr unsigned kMaxTimeoutRetries = 2;
      unsigned attempts = 0;
      bool turn_failed = false;
      for (;;) {
        ++attempts;
        try {
          // Streaming chat: prose is shown in real time, while the indicator's
          // label tracks what the model is doing whenever there is no prose yet
          // (notably while it builds a tool call); ESC stops the whole answer.
          assistant =
              client_.chat(build_request(),
                           [&, this](const std::string& content) {
                             if (sink_ != nullptr) {
                               // Webview front-end: the same stream-safe prefix
                               // rule the console printer applies — but deltas
                               // carry ONLY the newly-revealed tail. The page
                               // appends (streamBuf += text), so re-sending the
                               // whole cumulative prefix repeated the text over
                               // and over; on_final() still carries the full
                               // text for the definitive render.
                               const std::size_t shown = streamable_prefix(content);
                               if (shown > last_shown) {
                                 sink_->on_delta(content.substr(last_shown, shown - last_shown));
                                 last_shown = shown;
                               }
                               const std::string label = ui::stream_status(content);
                               if (label != last_label) {
                                 sink_->on_status(label);
                                 last_label = label;
                               }
                             } else {
                               printer.feed(content);
                               if (!printer.wrote()) {
                                 spinner.set_label(ui::stream_status(content));
                               }
                             }
                             const bool keep = (sink_ != nullptr)
                                                   ? !ui::external_stop_pressed()
                                                   : !ui::stop_output_pressed();
                             if (!keep) user_stopped = true;
                             return keep;
                           });
          break;
        } catch (const std::exception& error) {
          const std::string what = error.what();
          const bool nothing_shown =
              (sink_ != nullptr) ? last_shown == 0 : printer.shown() == 0;
          if (what.find("timed out") != std::string::npos &&
              attempts <= kMaxTimeoutRetries && nothing_shown) {
            const std::string notice =
                "timed out while waiting for a response — retrying (attempt " +
                std::to_string(attempts + 1) + " of " +
                std::to_string(kMaxTimeoutRetries + 1) + ")";
            if (sink_ != nullptr) {
              sink_->on_notice(notice, false);
            } else {
              std::cout << "  "
                        << gamma::ui::theme::paint("⏳ " + notice, gamma::ui::theme::Role::Warn)
                        << "\n";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(900));
            last_label.clear(); // the retry streams a fresh status + fresh text
            continue;
          }
          turn_failed = true;
          turn_failure = what;
          break;
        }
      }
      if (turn_failed) {
        printer.finish();
        // Wire/parse failure: no assistant message exists yet to append, so
        // hand the failure back to the model as a corrective note. The loop
        // cap still applies — gamma cannot hang spinning on a broken server.
        Message note;
        note.role = "user";
        note.content = std::string("[gamma] the previous Ollama call failed (") + turn_failure +
                       "). Respond normally; retry tools only if you believe it was transient.";
        history_.push_back(std::move(note));
        // Same failure repeated: show it the first few times only, cap noise.
        if (turn_failure != last_error || iterations <= 3) {
          if (sink_ != nullptr) {
            sink_->on_notice("ollama error: " + turn_failure, true);
          } else {
            std::cout << "  " << gamma::ui::theme::paint("✗ ollama error: " + turn_failure,
                                                  gamma::ui::theme::Role::Error)
                      << "\n";
          }
        }
        last_error = turn_failure;
        continue;
      }
      streamed = printer.shown();
      printer.finish();
    }

    if (user_stopped) {
      // A stopped response is only a partial thought. Its text stays in
      // history (visible below), but planning a tool call from a truncated
      // stream is unsafe, so NOTHING in it executes.
      if (sink_ != nullptr) {
        sink_->on_notice("output stopped — any tool calls in that response were not executed",
                         false);
      } else {
        std::cout << "  "
                  << gamma::ui::theme::paint("(output stopped — ESC) · any tool calls in that response "
                                     "were not executed",
                                     gamma::ui::theme::Role::Warn)
                  << "\n";
      }
      assistant.tool_calls.clear();
    }

    history_.push_back(assistant); // keep an intact local copy for this round

    // Extract session name from the first assistant message if present.
    // Format: "SESSION_NAME: <name>" at the very start of content.
    if (sink_ != nullptr && assistant.tool_calls.empty()) {
      const std::string& content = assistant.content;
      if (content.rfind("SESSION_NAME:", 0) == 0) {
        std::size_t colon_pos = content.find(':');
        if (colon_pos != std::string::npos) {
          std::string name = content.substr(colon_pos + 1);
          // trim whitespace
          while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
          while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
          if (!name.empty()) {
            sink_->on_session_name(name);
          }
        }
      }
    }

    if (assistant.tool_calls.empty()) {
      // Live streaming already showed the safe prefix (all of it for plain
      // prose). Print only what was held back — a fence or trailing text — so
      // the final answer is never duplicated; nothing streamed ⇒ full reply.
      if (sink_ != nullptr) {
        sink_->on_final(assistant.content);
        return true;
      }
      if (streamed == 0) {
        ui::print_assistant_message(assistant.content);
      } else if (streamed < assistant.content.size()) {
        ui::print_assistant_continuation(assistant.content.substr(streamed));
      }
      return true;
    }

    // Execute tool calls: dispatch → permission → run → tool message.
    // Every failure path (incl. unknown tools) is a tool error; no crashes.
    // Iterate the LOCAL copy: append_tool_result() pushes into history_,
    // which reallocates — a range-for over history_.back() would dangle.
    for (const ToolCall& call : assistant.tool_calls) {
      const Tool* tool = registry_.find(call.name);
      if (tool == nullptr) {
        ToolResult unknown;
        unknown.ok = false;
        unknown.error = "unknown tool";
        append_tool_result(nullptr, call.name, unknown);
        continue;
      }

      if (sink_ != nullptr) {
        sink_->on_tool(call.name, call.arguments);
      } else {
        ui::print_tool_call(call.name, call.arguments);
      }
      const bool approved =
          tools::permitted(*tool, call.arguments, options_.mode, options_.yolo, confirm_);
      if (!approved) {
        ToolResult denied;
        denied.ok = false;
        denied.error = "user denied";
        append_tool_result(tool, call.name, denied);
        continue;
      }

      ToolResult result;
      try {
        result = tool->run(call.arguments);
      } catch (const std::exception& error) {
        result.ok = false;
        result.error = std::string("tool crashed: ") + error.what();
      }
      append_tool_result(tool, call.name, result);
      if (sink_ != nullptr) {
        // The UI gets a bounded slice for the expandable detail view; the
        // full result lives in history via append_tool_result().
        std::string shown_output = result.output;
        if (shown_output.size() > 4000)
          shown_output = shown_output.substr(0, 4000) + "\n… [truncated]";
        sink_->on_tool_result(call.name, result.ok, shown_output, result.error);
      } else {
        ui::print_tool_result(result.ok, result.output, result.error);
      }
    }
    // Loop back: history now contains the assistant tool calls plus their
    // results, so the model sees the effects and continues. (Context
    // compaction hooks in here at step 3, between full iterations only.)
  }
}

} // namespace gamma
