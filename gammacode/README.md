# Gamma Code

A local-only-first, terminal-native coding agent. Model traffic goes to Ollama
on 127.0.0.1 only — no cloud inference, no API keys, no telemetry; `web_search`
is the one outbound tool and `GAMMA_NO_WEB=1` removes it. Reads, writes, edits,
searches, runs commands, pauses for human decisions (`ask`), and looks things
up on the web. Replies stream live; the default `auto` mode is hands-free
(`--no-yolo` re-adds the `run_command` prompt). On first launch it offers to
install Ollama, pulls a model matched to the machine's RAM/VRAM, starts
`ollama serve`, and thereafter uses the newest installed tool-capable model.
Handles long files via line-ranged reads and tiered context compaction.

Binary: `gamma` · Namespace: `gamma` · Version: 0.1.2

<!-- README fully fleshed out in step 5: local-only guarantee, build steps,
     permission modes, compaction strategy with a long-file example. -->

## Status

- [x] Step 1 — Ollama client (libcurl), REPL, read_file, run_command with approval
- [x] Step 2 — write_file / edit_file (diff preview) / list_dir / glob / grep
- [x] Step 3 — context compaction (tier A tool-result trim + tier B model summary)
- [x] Step 4 — slash commands, launcher, streaming replies, automatic Ollama setup
- [ ] Step 5 — test suite and expanded developer docs

## Future work (planned, not started)

- **Step 6 — native Windows UI rewrite** (the largest step): replace the full
  terminal UI (launcher, spinner, permission boxes) with a real native Windows
  UI — actual windows, buttons, dialogs, proper text widgets — while the agent
  core (`agent/`, `tools/`, `ollama/`) stays untouched underneath. Same
  local-only guarantee, same automation defaults.
