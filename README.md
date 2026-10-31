# Gamma Code

A local-only, terminal-native AI coding agent for Windows. It talks to **Ollama
on `127.0.0.1:11434` and nothing else** — no cloud, no API keys, no telemetry.
Not the prompt, not the code, not the conversation ever leaves the machine.

- **Windows only.** Single binary, `gamma.exe`.
- **No toolchain for users.** Prebuilt installer. On first launch gamma offers to
  install [Ollama for Windows](https://ollama.com/download/windows), pulls the
  largest `qwen2.5-coder` model that fits the machine, and starts the local
  server. After that it reuses the newest installed model.
- **C++20, no frameworks.** libcurl + nlohmann/json + replxx, all fetched by CMake at build time.

Website: <https://zstudio-lab.github.io/Gamma-Code/>

## What it does

Gamma runs a full agent loop: it reads your code, plans edits, runs tools, and
reports back in an interactive terminal REPL (`γ ❯`).

| Tool | What it does |
|---|---|
| `read_file` | Numbered lines; `start_line`/`end_line` ranges; large reads capped (first 500 + last 200 of 2,000+ line files, 50 KB output) with the exact range to request next; binary files refused |
| `write_file` / `edit_file` | Create or overwrite (parents created); exact-match replacement tolerant of CRLF/LF and indentation with a block-anchor fallback, `replace_all` for repeated hits, ambiguity refused. In `ask` mode every edit shows a unified diff first |
| `list_dir` / `glob` / `grep` | Folders, path patterns (optionally rooted at a subdirectory), and regex over contents. Honors the root `.gitignore`, skips VCS/dependency folders and binaries; caps: 500 paths, 200 grep hits (50 per file), `file:line:` format |
| `run_command` | `cmd.exe /c`, 60‑second timeout, stdout and stderr captured separately |
| compaction | Tiered: truncate old tool output → summarize old turns → hard-drop oldest. Never interrupts a tool call mid-flight |

Replies stream live: assistant prose prints as the model writes it, with
markdown-aware styling. Tool-call JSON is never streamed — it stays behind the
normal spinner — so you see the model think, not its raw wire format. `Esc` or
`Ctrl`+`C` stops the answer mid-stream, and a truncated tool call is never run.

Sessions persist per workspace under `%LOCALAPPDATA%\Gamma\sessions\` and are
restored automatically — full conversation history, including tool results.

## Permission modes

| Mode | Behavior |
|---|---|
| `auto` *(default)* | Hands-free. Reads, writes, edits, and commands run immediately; every call still prints a breadcrumb. `--no-yolo` adds a `y/N` prompt for `run_command` |
| `ask` | Reads auto-approved; writes, edits, and shell commands prompt `y/N` with the full diff or command shown first |
| `plan` | Read-only. Writes and commands are denied with an explanatory tool error |

Loopback-only is enforced, not just documented: the binary hard-codes
`http://127.0.0.1:11434` and refuses to start if a non-loopback `--ollama-url`
is passed. Model traffic never leaves the machine. The only other network
request in the codebase is the first-run HTTPS download of the official Ollama
installer — and only after you answer `Install Ollama? [Yes/No]` with yes.

## Quick start

```powershell
gamma
```

On first run gamma asks `Install Ollama? [Yes/No]` when Ollama is missing,
downloads and installs it with visible progress, pulls a model chosen for the
machine’s RAM/VRAM (1.5b → 32b), and starts `ollama serve`. If models already
exist, it picks the newest tool-capable one automatically.

Prefer to manage it yourself:

```powershell
ollama pull qwen2.5-coder:7b
gamma --model qwen2.5-coder:7b
```

One-shot mode, no REPL:

```powershell
gamma -p "explain main.cpp"
```

Useful keys: `Esc` while the model streams stops its output; `N` in the launcher
starts a new session; the launcher lists **System prompt** but never prints it
inline — press Enter there to read it; `/help` lists slash commands.

Set `GAMMA_NO_SETUP=1` to skip the Ollama bootstrap (useful in scripts and CI).

## Install

Prebuilt installer (Inno Setup) and a PowerShell one-liner live in
[Releases](https://github.com/ZStudio-lab/Gamma-Code/releases); the site's
PowerShell path verifies the binary's SHA-256 before installing. winget works
too — it installs from this repo's manifest, so no community submission is
needed:

```powershell
$v = "0.1.2"  # latest release
irm "https://raw.githubusercontent.com/ZStudio-lab/Gamma-Code/main/installer/winget/manifest/G/GammaCode/GammaCode.Gamma/$v/GammaCode.Gamma.yaml" -OutFile "$env:TEMP\GammaCode.Gamma.yaml"
winget install --manifest "$env:TEMP\GammaCode.Gamma.yaml" --accept-package-agreements --accept-source-agreements
```

To build from source:

```powershell
cmake -B build -S gammacode
cmake --build build -j 8
# binary: build\gamma.exe
```

Requirements: CMake 3.25+, a C++20 compiler (MSVC or clang/llvm-mingw), and
network access once for CMake's FetchContent.

## Status

v0.1.2. Local sessions, live streaming, compaction, three permission modes, the
launcher and automatic Ollama setup (detect → install → hardware-matched model
pull → serve → newest-model selection) work on Windows 10 22H2+ and Windows 11.
v0.1.2 hardened the file tools for large projects: gitignore-aware scans,
CRLF/indentation-tolerant edits with `replace_all`, capped reads with
continuation markers, binary detection, and "did you mean" path suggestions.
Shipped with an Inno Setup installer, a portable zip, a PowerShell installer,
and a winget manifest that installs straight from this repo.

## License

[MIT](LICENSE).
