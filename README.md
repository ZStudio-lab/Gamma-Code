# Gamma Code

A local-only, terminal-native AI coding agent for Windows. It talks to **Ollama
on `127.0.0.1:11434` and nothing else** — no cloud, no API keys, no telemetry.
Not the prompt, not the code, not the conversation ever leaves the machine.

- **Windows only.** Single binary, `gamma.exe`.
- **No toolchain for users.** Prebuilt installer; the only runtime prerequisites
  are [Ollama for Windows](https://ollama.com/download/windows) and a pulled model.
- **C++20, no frameworks.** libcurl + nlohmann/json + replxx, all fetched by CMake at build time.

Website: <https://zstudio-lab.github.io/Gamma-Code/>

## What it does

Gamma runs a full agent loop: it reads your code, plans edits, runs tools, and
reports back in an interactive terminal REPL (`γ ❯`).

| Tool | What it does |
|---|---|
| `read_file` | Numbered lines; files over 2,000 lines split head + tail with a hint to request ranges |
| `write_file` / `edit_file` | Create or overwrite; exact-match replacement that refuses ambiguous matches. Every edit shows a unified diff first |
| `glob` / `grep` | Path patterns and regex over contents (capped at 200 hits, `file:line:` format) |
| `run_command` | `cmd.exe /c` (or PowerShell with `--shell psh`), 60‑second timeout, stdout and stderr captured separately |
| `ask_user` | The model pauses and asks you for something it cannot infer |
| compaction | Tiered: truncate old tool output → summarize old turns → hard-drop oldest. Never interrupts a tool call mid-flight |

Sessions persist per workspace under `%LOCALAPPDATA%\Gamma\sessions\` and are
restored automatically — full conversation history, including tool results.

## Permission modes

| Mode | Behavior |
|---|---|
| `plan` | Read-only. Writes and commands are denied with an explanatory tool error |
| `ask` *(default)* | Reads auto-approved; writes, edits, and shell commands prompt `y/N` with the full diff or command shown first |
| `auto` | Most operations auto-approved; `run_command` still prompts unless `--yolo` |

Loopback-only is enforced, not just documented: the binary hard-codes
`http://127.0.0.1:11434` and refuses to start if a non-loopback `--ollama-url`
is passed. There is no other outbound HTTP in the codebase.

## Quick start

```powershell
ollama pull qwen2.5-coder:7b   # one-time
gamma
```

One-shot mode, no REPL:

```powershell
gamma -p "explain main.cpp"
```

Useful keys: `Esc` while the model streams stops its output; `N` in the launcher
starts a new session; `/help` lists slash commands.

## Install

Prebuilt `.msi` and a PowerShell one-liner are on the way — see the
[website](https://zstudio-lab.github.io/Gamma-Code/). Meanwhile, build from
source:

```powershell
cmake -B build -S gammacode
cmake --build build -j 8
# binary: build\gamma.exe
```

Requirements: CMake 3.25+, a C++20 compiler (MSVC or clang/llvm-mingw), and
network access once for CMake's FetchContent.

## Status

v0.1.0. Local sessions, streaming, compaction, three permission modes and the
launcher work on Windows 10 22H2+ and Windows 11. Installer + winget package in
progress.

## License

[MIT](LICENSE).
