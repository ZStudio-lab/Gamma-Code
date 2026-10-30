# Gamma Code

A local-only, terminal-native coding agent. Talks to Ollama on 127.0.0.1 only.
Reads, writes, edits, searches, runs commands, and asks you before it does
anything dangerous. Handles long files via line-ranged reads and tiered
context compaction.

Binary: `gamma` · Namespace: `gamma` · Version: 0.1.0

<!-- README fully fleshed out in step 5: local-only guarantee, build steps,
     permission modes, compaction strategy with a long-file example. -->

## Status

- [x] Step 1 — Ollama client (libcurl), REPL, read_file, run_command with approval
- [x] Step 2 — write_file / edit_file (diff preview) / list_dir / glob / grep
- [x] Step 3 — context compaction (tier A tool-result trim + tier B model summary)
- [ ] Step 4 — ask_user + slash commands finalized
- [ ] Step 5 — tests + full README

## Future work (planned, not started)

- **Step 6 — native Windows UI rewrite** (the largest step): replace the full
  terminal UI (launcher, spinner, permission boxes) with a real native Windows
  UI — actual windows, buttons, dialogs, proper text widgets — while the agent
  core (`agent/`, `tools/`, `ollama/`) stays untouched underneath. Same
  local-only guarantee, same automation defaults.
