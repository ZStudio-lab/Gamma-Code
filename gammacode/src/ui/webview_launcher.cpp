#include "ui/webview_launcher.hpp"

#include "ui/render.hpp"
#include "agent/loop.hpp"
#include "config/sessions.hpp"
#include "tools/registry.hpp"
#include "setup/ollama_setup.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef GAMMA_HAS_WEBVIEW

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <Dwmapi.h>
#include <tlhelp32.h>
#include <ShlObj.h> // IFileOpenDialog — the native folder picker
#include <WebView2.h>

// ── mingw uuid plumbing (global scope only!) ────────────────────────────────
// guiddef.h declares the `__mingw_uuidof` primary template at global scope;
// clang lowers `__uuidof(T)` to `__mingw_uuidof<T>()` calls. Specializations
// must therefore sit in the GLOBAL namespace too (not inside gamma::ui) or
// they never match. The CRT interfaces (IUnknown, IFileOpenDialog) are
// generated via __CRT_UUID_DECL in their own headers; only the three WebView2
// handler interfaces QueryInterface checks need these.
#if defined(__clang__) && (defined(__MINGW32__) || defined(__MINGW64__))
template <> const GUID& __mingw_uuidof<
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>() {
  return IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler;
}
template <> const GUID& __mingw_uuidof<
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>() {
  return IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler;
}
template <> const GUID& __mingw_uuidof<ICoreWebView2WebMessageReceivedEventHandler>() {
  return IID_ICoreWebView2WebMessageReceivedEventHandler;
}
#endif

namespace gamma::ui {

namespace {

namespace fs = std::filesystem;

// ── WebView2Loader.dll, runtime-loaded ──────────────────────────────────────
// The MSVC-built static loader uses the MSVC C++ ABI and cannot link into a
// MinGW binary, so the two loader entry points are resolved through
// LoadLibrary + GetProcAddress instead. The DLL ships next to gamma.exe
// (the build copies it there; the installer stages it too).
struct WebView2Api {
  using CreateEnvironmentFn = HRESULT (__stdcall *)(
      PCWSTR browser_executable_folder, PCWSTR user_data_folder,
      ICoreWebView2EnvironmentOptions* environment_options,
      ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* handler);
  using BrowserVersionFn = HRESULT (__stdcall *)(PCWSTR browser_executable_folder,
                                                 LPWSTR* version);

  CreateEnvironmentFn create_environment = nullptr;
  BrowserVersionFn browser_version = nullptr;
  std::wstring problem; // human-readable reason when unusable

  bool ok() const { return create_environment != nullptr && browser_version != nullptr; }
};

WebView2Api load_webview2_api() {
  WebView2Api api;
  HMODULE module = LoadLibraryW(L"WebView2Loader.dll");
  if (module == nullptr) {
    api.problem = L"WebView2Loader.dll was not found next to gamma.exe";
    return api;
  }
  api.create_environment = reinterpret_cast<WebView2Api::CreateEnvironmentFn>(
      GetProcAddress(module, "CreateCoreWebView2EnvironmentWithOptions"));
  api.browser_version = reinterpret_cast<WebView2Api::BrowserVersionFn>(
      GetProcAddress(module, "GetAvailableCoreWebView2BrowserVersionString"));
  if (!api.ok()) {
    api.problem = L"WebView2Loader.dll is missing its entry points";
    return api;
  }
  LPWSTR version = nullptr;
  const HRESULT have_version = api.browser_version(nullptr, &version);
  if (FAILED(have_version) || version == nullptr) {
    api.problem =
        L"the WebView2 Runtime is not installed (Windows 10/11 usually ship it)";
    if (version != nullptr) CoTaskMemFree(version);
    return api;
  }
  CoTaskMemFree(version);
  return api;
}

// ── shared app state (UI thread + worker thread) ────────────────────────────

constexpr UINT WM_APP_DRAIN = WM_APP + 71; // worker → UI: outbound queue has data

std::string wide_to_utf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                       static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                       nullptr);
  std::string text(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), text.data(),
                      size, nullptr, nullptr);
  return text;
}

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                      size);
  return wide;
}

// Find the ollama.exe process PID if running
DWORD find_ollama_pid() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"ollama.exe") == 0) {
        CloseHandle(snapshot);
        return entry.th32ProcessID;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return 0;
}

// Kill process by PID
void kill_pid(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
  if (h) {
    TerminateProcess(h, 0);
    CloseHandle(h);
  }
}

// Kill process by PID and its children (process tree), then sweep for any remaining ollama.exe
void kill_process_tree(DWORD pid) {
  // First kill children
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
      do {
        if (entry.th32ParentProcessID == pid) {
          kill_process_tree(entry.th32ProcessID);
        }
      } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
  }
  // Then kill self
  kill_pid(pid);
}

// If we started Ollama, kill ALL ollama.exe processes as a final sweep
void kill_all_ollama() {
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, L"ollama.exe") == 0) {
        kill_pid(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
}

struct Context {
  Options* options = nullptr;
  std::vector<std::string> models;
  OllamaClient* client = nullptr;
  const Registry* registry = nullptr;
  AgentLoop* loop = nullptr;

  HWND hwnd = nullptr;
  ICoreWebView2* webview = nullptr;
  ICoreWebView2Controller* controller = nullptr;

  std::string session_file;
  std::atomic<bool> busy{false};
  std::atomic<bool> closing{false};

  // chat turns run here; the UI thread joins it before starting a new turn
  std::thread worker;
  // the Opening Ollama bootstrap (joined at teardown before this dies)
  std::thread startup_worker;

  // model list is refilled by the bootstrap on the startup thread while the
  // UI thread reads it in send_state()
  std::mutex models_mutex;

  // true until the 'Opening Ollama' bootstrap finishes; the page keeps the
  // composer quiet and shows the state in its own status line.
  std::atomic<bool> starting{false};

  // Ollama lifecycle: did we start the server? if so, track the PID to kill on exit.
  bool owns_ollama = false;
  DWORD ollama_pid = 0;

  // worker → UI JSON events; the UI thread drains on WM_APP_DRAIN
  std::mutex queue_mutex;
  std::deque<std::string> outbound;

  // modal bridge (permission gate + ask tool): the worker blocks until the
  // UI thread resolves the dialog
  std::mutex dialog_mutex;
  std::condition_variable dialog_cv;
  nlohmann::json dialog;      // {type:"confirm"|"ask", id, ...}
  std::atomic<uint64_t> next_dialog_id{1};
  bool dialog_ready = false;
  bool dialog_yes = false;
  std::string dialog_text;

  void post(nlohmann::json payload) {
    {
      const std::lock_guard<std::mutex> lock(queue_mutex);
      outbound.push_back(payload.dump());
    }
    if (hwnd != nullptr) PostMessageW(hwnd, WM_APP_DRAIN, 0, 0);
  }

  void drain() { // UI thread only
    std::deque<std::string> batch;
    {
      const std::lock_guard<std::mutex> lock(queue_mutex);
      batch.swap(outbound);
    }
    if (webview == nullptr) return;
    for (const std::string& payload : batch) {
      webview->PostWebMessageAsJson(utf8_to_wide(payload).c_str());
    }
  }

  void send_state() {
    nlohmann::json sessions = nlohmann::json::array();
    try {
      std::vector<std::string> saved = sessions::saved_sessions();
      std::sort(saved.begin(), saved.end(), std::greater<std::string>());
      for (std::size_t i = 0; i < saved.size() && i < 20; ++i) {
        sessions.push_back({{"path", saved[i]},
                            {"name", fs::path(saved[i]).filename().string()}});
      }
    } catch (const std::exception&) {
      sessions = nlohmann::json::array();
    }
    std::vector<std::string> model_list;
    {
      const std::lock_guard<std::mutex> lock(models_mutex);
      model_list = models;
    }
    // Context usage for the ring indicator: approx tokens of history vs budget.
    long long ctx_used = 0, ctx_budget = 0;
    if (loop != nullptr) {
      const AgentLoop::Stats snapshot = loop->stats();
      ctx_used = static_cast<long long>(snapshot.tokens);
      ctx_budget = static_cast<long long>(snapshot.budget);
    }
    post({{"type", "state"},
          {"model", options->model},
          {"models", model_list},
          {"ctx_used", ctx_used},
          {"ctx_budget", ctx_budget},
          {"starting", starting.load()},
          {"mode", mode_name(options->mode)},
          {"workspace",
           options->workspace.empty() ? std::string("(current dir)") : options->workspace},
          {"session", fs::path(session_file).filename().string()},
          {"sessions", sessions},
          {"version", GAMMA_VERSION}});
  }

  void send_transcript() {
    nlohmann::json messages = nlohmann::json::array();
    for (const Message& message : loop->history()) {
      if (message.role == "system") continue; // never shown to the user
      if (message.role == "user") {
        if (message.content.starts_with("[gamma] the previous Ollama call failed"))
          continue; // internal corrective note, not a real user bubble
      }
      nlohmann::json item = {{"role", message.role},
                             {"content", message.content},
                             // user/assistant bubbles render from "text"; tool
                             // payload rows parse "content" as their JSON.
                             {"text", message.content}};
      if (!message.tool_calls.empty()) {
        // Tool breadcrumbs survive a reload: the page rebuilds its expandable
        // steps from these (plus the trailing tool-role payload rows below).
        nlohmann::json calls = nlohmann::json::array();
        for (const ToolCall& call : message.tool_calls) {
          calls.push_back({{"name", call.name}, {"arguments", call.arguments.dump()}});
        }
        item["tools"] = calls;
      }
      messages.push_back(std::move(item));
    }
    post({{"type", "load"}, {"messages", messages}});
  }

  // Worker thread → UI dialog → blocking answer. Shared by the permission
  // gate (confirm) and the ask tool (free text / options).
  bool show_dialog(const nlohmann::json& payload, std::string& answer) {
    {
      std::lock_guard<std::mutex> lock(dialog_mutex);
      dialog = payload;
      dialog_ready = false;
      dialog_yes = false;
      dialog_text.clear();
    }
    post(payload);
    std::unique_lock<std::mutex> lock(dialog_mutex);
    dialog_cv.wait(lock, [this] { return dialog_ready || closing.load(); });
    answer = dialog_text;
    return dialog_yes && !closing.load();
  }
};

// The agent-loop sink: forwards every turn event into the window. Called on
// the worker thread; post() marshals to the UI thread.
class WebSink : public TurnSink {
 public:
  explicit WebSink(Context& context) : context_(context) {}

  void on_delta(const std::string& streamable_text) override {
    context_.post({{"type", "delta"}, {"text", streamable_text}});
  }
  void on_status(const std::string& label) override {
    context_.post({{"type", "status"}, {"label", label}});
  }
  void on_tool(const std::string& name, const nlohmann::json& arguments) override {
    context_.post({{"type", "tool"}, {"name", name}, {"args", arguments.dump()}});
  }
  void on_tool_result(const std::string& name, bool ok,
                      const std::string& output, const std::string& error) override {
    context_.post({{"type", "tool-result"}, {"name", name}, {"ok", ok},
                   {"output", output}, {"error", error}});
  }
  void on_final(const std::string& content) override {
    context_.post({{"type", "final"}, {"text", content}});
  }
  void on_session_name(const std::string& name) override {
    context_.post({{"type", "session-name"}, {"name", name}});
  }
  void on_notice(const std::string& text, bool error) override {
    context_.post({{"type", "notice"}, {"text", text}, {"error", error}});
  }

 private:
  Context& context_;
};

// ── minimal COM handler boilerplate (no WRL: llvm-mingw has no wrl/clients) ─

template <typename Base>
class HandlerBase : public Base {
 public:
  explicit HandlerBase(Context& context) : context_(context) {}

  ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() noexcept override {
    const ULONG remaining = --ref_;
    if (remaining == 0) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) noexcept override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(Base)) {
      *out = static_cast<Base*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

 protected:
  virtual ~HandlerBase() = default;
  Context& context_;

 private:
  std::atomic<ULONG> ref_{1};
};

class EnvironmentHandler
    : public HandlerBase<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
 public:
  using HandlerBase::HandlerBase;
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                   ICoreWebView2Environment* environment) noexcept override {
    if (SUCCEEDED(result) && environment != nullptr) environment->AddRef();
    result_ = result;
    environment_ = environment;
    return S_OK;
  }
  bool done() const { return result_ != E_FAIL; }
  HRESULT result() const { return result_; }
  ICoreWebView2Environment* detach() {
    ICoreWebView2Environment* environment = environment_;
    environment_ = nullptr;
    return environment;
  }

 private:
  HRESULT result_ = E_FAIL;
  ICoreWebView2Environment* environment_ = nullptr;
};

class ControllerHandler
    : public HandlerBase<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  using HandlerBase::HandlerBase;
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                   ICoreWebView2Controller* controller) noexcept override {
    if (SUCCEEDED(result) && controller != nullptr) controller->AddRef();
    result_ = result;
    controller_ = controller;
    return S_OK;
  }
  bool done() const { return result_ != E_FAIL; }
  HRESULT result() const { return result_; }
  ICoreWebView2Controller* detach() {
    ICoreWebView2Controller* controller = controller_;
    controller_ = nullptr;
    return controller;
  }

 private:
  HRESULT result_ = E_FAIL;
  ICoreWebView2Controller* controller_ = nullptr;
};

std::wstring pick_folder(HWND owner) {
  std::wstring chosen;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IFileOpenDialog),
                              reinterpret_cast<void**>(&dialog))))
    return chosen;
  DWORD flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(L"Choose gamma's workspace folder");
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
        chosen = path;
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
  return chosen;
}

// Multi-select native file picker for "attach files".
// Returns UTF-8 paths; the caller reads contents and posts them to the page.
std::vector<std::string> pick_files(HWND owner) {
  std::vector<std::string> files;
  IFileOpenDialog* dialog = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IFileOpenDialog),
                              reinterpret_cast<void**>(&dialog))))
    return files;
  DWORD flags = 0;
  dialog->GetOptions(&flags);
  dialog->SetOptions(flags | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
                     FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
  dialog->SetTitle(L"Attach files to the conversation");
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dialog->GetResults(&items)) && items != nullptr) {
      DWORD count = 0;
      if (SUCCEEDED(items->GetCount(&count))) {
        for (DWORD i = 0; i < count; ++i) {
          IShellItem* item = nullptr;
          if (SUCCEEDED(items->GetItemAt(i, &item)) && item != nullptr) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
              files.push_back(wide_to_utf8(path));
              CoTaskMemFree(path);
            }
            item->Release();
          }
        }
      }
      items->Release();
    }
  }
  dialog->Release();
  return files;
}

// JS → C++: every click lands here. Chat actions run the agent on a worker
// thread; dialogs resolve the blocked worker.
class MessageHandler : public HandlerBase<ICoreWebView2WebMessageReceivedEventHandler> {
 public:
  using HandlerBase::HandlerBase;

  HRESULT STDMETHODCALLTYPE
  Invoke(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) noexcept override {
    LPWSTR raw = nullptr;
    if (args == nullptr || FAILED(args->get_WebMessageAsJson(&raw)) || raw == nullptr)
      return S_OK;
    const std::string json = wide_to_utf8(raw);
    CoTaskMemFree(raw);

    nlohmann::json message;
    try {
      message = nlohmann::json::parse(json, nullptr, false);
    } catch (const std::exception&) {
      return S_OK;
    }
    if (!message.is_object() || !message.contains("action")) return S_OK;
    const std::string action = message.value("action", std::string());
    Context& context = context_;

    if (action == "send" && message.contains("text")) {
      if (context.busy.load()) return S_OK; // one turn at a time
      std::string text = message.value("text", std::string());
      if (text.empty()) return S_OK;
      if (context.worker.joinable()) context.worker.join();
      context.worker = std::thread([message = std::move(text), &context]() mutable {
        ui::clear_output_stop();
        context.busy = true;
        context.post({{"type", "busy"}, {"value", true}});
        WebSink sink(context);
        context.loop->set_sink(&sink);
        const bool completed = context.loop->run_turn(std::move(message));
        context.loop->set_sink(nullptr);
        if (completed) (void)context.loop->save_session(context.session_file);
        context.post({{"type", "busy"}, {"value", false}});
        context.send_state(); // refresh the sessions list
        context.busy = false;
      });
    } else if (action == "stop") {
      ui::request_output_stop();
    } else if (action == "set-model" && message.contains("value")) {
      context.options->model = message.value("value", std::string());
      context.send_state();
    } else if (action == "set-mode" && message.contains("value")) {
      Mode parsed = context.options->mode;
      if (parse_mode(message.value("value", std::string()), parsed))
        context.options->mode = parsed;
      context.send_state();
    } else if (action == "new-chat") {
      if (context.busy.load()) return S_OK;
      context.loop->reset_history();
      context.options->session_file = sessions::new_session_path(*context.options);
      context.session_file = context.options->session_file;
      context.post({{"type", "clear"}});
      context.send_state();
    } else if (action == "resume-session" && message.contains("path")) {
      if (context.busy.load()) return S_OK;
      const std::string path = message.value("path", std::string());
      if (context.loop->load_session(path)) {
        context.options->session_file = path;
        context.session_file = path;
        context.send_transcript();
        context.send_state();
      } else {
        context.post({{"type", "notice"},
                      {"text", "could not load that session file"}, {"error", true}});
      }
    } else if (action == "choose-workspace") {
      const std::wstring picked = pick_folder(context.hwnd);
      if (!picked.empty()) {
        context.options->workspace = wide_to_utf8(picked);
        try {
          // re-anchor the process CWD and the tool-path root
          gamma::activate_workspace(*context.options);
        } catch (const std::exception&) {
          // keep the session alive even if the folder vanished between
          // the dialog and the chdir
        }
        // the system prompt embeds the workspace — keep it fresh (history
        // is preserved; only the prompt line changes)
        context.loop->refresh_system_message();
        // and the next launch re-opens this folder instead of the CWD
        gamma::sessions::remember_workspace(context.options->workspace);
      }
      context.send_state();
    } else if (action == "show-prompt") {
      // the LIVE prompt: static instructions plus the actual working dir
      context.post({{"type", "prompt"}, {"text", context.loop->current_system_prompt()}});
    } else if (action == "ready") {
      // the page asks for state after its script attaches (initial
      // send_state() can race the navigation)
      context.send_state();
    } else if (action == "load-history") {
      // the history drawer opened — refresh the saved-session list
      context.send_state();
    } else if (action == "delete-session") {
      const std::string path = message.value("path", "");
      sessions::delete_session(path); // path guard lives in the module
      context.send_state();
    } else if (action == "clear-history") {
      sessions::delete_all_sessions();
      context.send_state();
    } else if (action == "rename-session") {
      // Rename the current session file with the model-provided name
      if (!context.session_file.empty() && message.contains("name")) {
        const std::string new_name = message.value("name", "");
        if (!new_name.empty()) sessions::rename_session(context.session_file, new_name);
      }
      context.send_state();
    } else if (action == "win-min") {
      ShowWindow(context.hwnd, SW_MINIMIZE);
    } else if (action == "win-max") {
      ShowWindow(context.hwnd, IsZoomed(context.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (action == "win-close") {
      DestroyWindow(context.hwnd);
    } else if (action == "attach-files") {
      // Native multi-select picker; contents go back to the page as
      // @filename chips the user can review before sending.
      const std::vector<std::string> files = pick_files(context.hwnd);
      if (!files.empty()) {
        nlohmann::json payload = {{"type", "files-attached"}, {"files", nlohmann::json::array()}};
        for (const std::string& path : files) {
          std::ifstream in(path, std::ios::binary);
          if (!in) continue;
          std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
          payload["files"].push_back({{"path", path}, {"content", content}});
        }
        context.post(std::move(payload));
      }
    } else if (action == "list-files") {
      // Return relative paths of files in the workspace for @ autocomplete
      std::string workspace = context.options ? context.options->workspace : std::string();
      if (workspace.empty()) {
        workspace = std::filesystem::current_path().string();
      }
      nlohmann::json payload = {{"type", "file-list"}, {"files", nlohmann::json::array()}};
      std::error_code ec;
      for (auto& entry : std::filesystem::recursive_directory_iterator(workspace, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        // Skip .git, node_modules, build, etc.
        std::string rel = std::filesystem::relative(entry.path(), workspace).string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (rel.find(".git/") == 0 || rel.find("node_modules/") == 0 ||
            rel.find("build/") == 0 || rel.find(".vs/") == 0 ||
            rel.find("out/") == 0 || rel.find("bin/") == 0 || rel.find("obj/") == 0)
          continue;
        payload["files"].push_back(rel);
        if (payload["files"].size() >= 500) break; // cap
      }
      context.post(std::move(payload));
    } else if (action == "confirm-result") {
      std::lock_guard<std::mutex> lock(context.dialog_mutex);
      context.dialog_yes = message.value("yes", false);
      context.dialog_text = message.value("text", std::string());
      context.dialog_ready = true;
      context.dialog_cv.notify_all();
    }
    return S_OK;
  }
};

// ── the page ─────────────────────────────────────────────────────────────────
// The whole chat front-end: top bar (model, mode, new chat, history,
// settings), streaming transcript with tool breadcrumbs, permission + ask
// dialogs, suggestion-chip empty state, input bar. In-memory; Aurora palette.

const char* kChatHtml = R"CHAT_HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Gamma Code</title>
<style>
  :root { color-scheme:dark; --bg:#131314; --surface:#1e1e1f; --input:#282a2c; --hover:#2f3133;
          --line:#37393b; --accent:#a8c7fa; --text:#e3e2e6; --muted:#8e918f;
          --green:#34d399; --red:#ef4444; --code:#0d0e0f; }
  * { box-sizing:border-box; }
  html,body { height:100%; }
  body { margin:0; background:var(--bg); color:var(--text); display:flex; flex-direction:column;
         overflow:hidden; font:14px/1.55 Inter,"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif; }
  code,pre,.mono { font-family:"Fira Code",Consolas,monospace; }
  ::-webkit-scrollbar { width:5px; height:5px; }
  ::-webkit-scrollbar-track { background:transparent; }
  ::-webkit-scrollbar-thumb { background:rgba(255,255,255,.15); border-radius:9999px; }
  svg { display:block; }

  /* ── titlebar header ── */
  header { height:40px; flex:0 0 40px; display:flex; align-items:center; gap:6px; padding:0 10px;
           border-bottom:1px solid var(--line); -webkit-app-region:drag; }
  header .iconbtn { -webkit-app-region:no-drag; }
  .iconbtn { padding:7px; border:0; background:transparent; color:var(--muted); border-radius:999px;
             cursor:pointer; transition:background .15s,color .15s; display:flex; align-items:center; justify-content:center; }
  .iconbtn:hover { background:var(--hover); color:var(--text); }
  .wordmark { font-size:13px; font-weight:600; letter-spacing:.01em; color:var(--text); }
  .spacer { flex:1; }
  .ver { color:var(--muted); font-size:10.5px; margin-right:4px; }
  .winctl { display:flex; gap:2px; margin-left:6px; -webkit-app-region:no-drag; }
  .winctl .iconbtn { width:32px; height:32px; padding:0; border-radius:7px; }
  .winctl .iconbtn:hover { background:rgba(255,255,255,.10); color:var(--text); border-radius:7px; }
  .winctl .iconbtn.close:hover { background:#8b2626; color:#fff; }

  /* ── chat column ── */
  #chat-col { flex:1; overflow-y:auto; padding:20px 0 0; scroll-behavior:smooth; }
  #chat-col .colwrap { max-width:860px; margin:0 auto; padding:0 24px 8px; }

  #welcome { display:flex; flex-direction:column; align-items:center; text-align:center;
             padding:70px 20px 30px; -webkit-user-select:none; }
  #welcome .star { width:46px; height:46px; border-radius:999px; background:var(--surface);
                   display:flex; align-items:center; justify-content:center; color:var(--accent);
                   animation:floaty 4s ease-in-out infinite; }
  @keyframes floaty { 0%,100% { transform:translateY(0); } 50% { transform:translateY(-6px); } }
  #welcome h1 { font-size:26px; font-weight:600; margin:16px 0 6px; color:var(--text); }
  #welcome p { color:var(--muted); margin:0 0 26px; max-width:420px; font-size:13.5px; }
  #welcome .cards { display:grid; grid-template-columns:1fr 1fr; gap:12px; width:100%; max-width:640px; }
  #welcome .card { text-align:left; background:var(--surface); border:1px solid var(--line);
                   border-radius:14px; padding:14px 16px; cursor:pointer; color:var(--text);
                   font-size:13.5px; display:flex; align-items:center; justify-content:space-between; gap:8px;
                   transition:background .15s; }
  #welcome .card:hover { background:var(--hover); }
  #welcome .card .arr { color:var(--accent); }

  .turn { margin:0 0 18px; }
  .u-turn { display:flex; justify-content:flex-end; }
  .u-bubble { max-width:76%; background:var(--surface); border:1px solid var(--line);
              border-radius:18px 18px 4px 18px; padding:10px 16px; white-space:pre-wrap; word-wrap:break-word; }
  .g-turn { display:flex; gap:12px; }
  .g-avatar { width:28px; height:28px; border-radius:999px; background:var(--accent); color:#131314;
              display:flex; align-items:center; justify-content:center; flex:0 0 28px; margin-top:2px;
              font-size:11px; font-weight:700; }
  .g-body { flex:1; min-width:0; }
  .g-body .prose { white-space:pre-wrap; word-wrap:break-word; }
  .stream-cursor { display:inline-block; width:7px; height:15px; background:var(--accent);
                   border-radius:2px; vertical-align:-2px; margin-left:3px;
                   animation:cursor-pulse 1s ease-in-out infinite; }
  @keyframes cursor-pulse { 0%,100% { opacity:1; } 50% { opacity:.25; } }

  /* ── code cards ── */
  .codecard { background:var(--code); border:1px solid var(--line); border-radius:10px; margin:10px 0; overflow:hidden; }
  .codecard .lang { display:flex; align-items:center; gap:6px; padding:6px 12px; font-size:10px;
                    color:var(--muted); letter-spacing:.08em; text-transform:uppercase;
                    border-bottom:1px solid var(--line); }
  .codecard .lang .copy { margin-left:auto; cursor:pointer; padding:2px 8px; border-radius:6px; }
  .codecard .lang .copy:hover { background:var(--hover); color:var(--text); }
  .codecard pre { margin:0; padding:12px 14px; overflow-x:auto; font-size:12.5px; line-height:1.6; color:#e3e2e6; }

  /* ── thought block ── */
  .thought { border:1px solid var(--line); border-radius:10px; background:var(--surface);
             margin:10px 0; overflow:hidden; }
  .thought .thead { display:flex; align-items:center; gap:7px; padding:7px 12px; cursor:pointer;
                    color:var(--muted); font-size:12px; font-style:italic; }
  .thought .thead:hover { color:var(--text); }
  .thought .twisty { transition:transform .15s; font-size:9px; }
  .thought.open .twisty { transform:rotate(90deg); }
  .thought .tpreview { overflow:hidden; color:var(--muted); font-size:12px; font-style:italic;
                       white-space:nowrap; text-overflow:ellipsis; flex:1; }
  .thought .tbody { display:none; padding:4px 14px 12px; color:var(--muted); font-size:12.5px;
                    font-style:italic; white-space:pre-wrap; word-wrap:break-word; border-top:1px solid var(--line); }
  .thought.open .tbody { display:block; }

  /* ── tool steps ── */
  .toolstep { display:flex; align-items:flex-start; gap:8px; margin:6px 0; }
  .toolstep.exp-lead { flex-direction:column; }
  .toolstep .row { display:flex; align-items:center; gap:8px; }
  .toolstep .twisty { color:var(--muted); font-size:9px; cursor:pointer; transition:transform .15s; user-select:none; }
  .toolstep.open .twisty { transform:rotate(90deg); }
  .toolstep .tname { color:var(--muted); font-size:12.5px; display:flex; align-items:center; gap:6px; }
  .toolstep .mark { font-size:11px; }
  .toolstep .mark.ok { color:var(--green); }
  .toolstep .mark.fail { color:var(--red); }
  .toolstep .mark.pending::before { content:''; width:10px; height:10px; border-radius:999px;
    border:2px solid var(--line); border-top-color:var(--accent); display:inline-block;
    vertical-align:-2px; animation:spin 1s linear infinite; }
  @keyframes spin { to { transform:rotate(360deg); } }
  .tdetail { display:none; width:100%; background:var(--code); border:1px solid var(--line);
             border-radius:8px; padding:8px 12px; margin:4px 0 6px; font-size:11.5px; }
  .toolstep.open .tdetail { display:block; }
  .tdetail .lk { color:var(--muted); letter-spacing:.08em; font-size:9.5px; margin:8px 0 3px; text-transform:uppercase; }
  .tdetail .lk:first-child { margin-top:0; }
  .tdetail pre { margin:0; white-space:pre-wrap; word-wrap:break-word; color:#b9bdc5; max-height:260px; overflow-y:auto; }

  /* ── status chip ── */
  .statuschip { display:inline-flex; align-items:center; gap:7px; background:var(--surface);
                border:1px solid var(--line); border-radius:999px; padding:4px 12px 4px 9px;
                margin:2px 0 4px; font-size:12px; color:var(--text); cursor:default; user-select:none; }
  .statuschip .dotspin { width:11px; height:11px; border-radius:999px; border:2px solid var(--line);
                         border-top-color:var(--accent); animation:spin 1.1s linear infinite; flex:0 0 11px; }
  .statuschip.past .dotspin { border:0; animation:none; }
  .statuschip.past .dotspin::before { content:'\2713'; color:var(--green); font-size:11px; line-height:11px; }
  .statuschip.past { border-color:var(--line); color:var(--muted); }
  .statuschip .chev { margin-left:2px; color:var(--muted); font-size:9px; cursor:pointer; transition:transform .15s; }
  .statuschip.open .chev { transform:rotate(90deg); }
  .statuschip-detail { display:none; margin:4px 0 8px; background:var(--surface);
                       border:1px solid var(--line); border-radius:10px; padding:10px 14px; }
  .statuschip-detail.open { display:block; }
  .statuschip-detail .drow { display:flex; align-items:center; gap:8px; padding:3px 0;
                             font-size:12px; color:var(--muted); }
  .statuschip-detail .drow .mark { font-size:11px; }
  .statuschip-detail .drow .mark.ok { color:var(--green); }
  .statuschip-detail .drow .mark.fail { color:var(--red); }

  /* ── notices / toasts ── */
  .notice { margin:10px 0; color:var(--red); font-size:12.5px; }
  .toast { position:fixed; left:50%; bottom:110px; transform:translateX(-50%);
           background:var(--surface); border:1px solid var(--line); color:var(--text);
           border-radius:10px; padding:9px 16px; font-size:12.5px; box-shadow:0 8px 30px rgba(0,0,0,.5);
           z-index:90; animation:toast-in .2s ease-out; }
  @keyframes toast-in { from { opacity:0; transform:translate(-50%, 8px); } to { opacity:1; } }

  /* ── opening overlay ── */
  #boot { position:fixed; inset:0; background:var(--bg); z-index:200; display:none;
          flex-direction:column; align-items:center; justify-content:center; gap:18px; }
  #boot.show { display:flex; }
  #boot .ring { width:44px; height:44px; border-radius:999px; border:3px solid var(--line);
                border-top-color:var(--accent); animation:spin 1s linear infinite; }
  #boot .label { color:var(--muted); font-size:13.5px; }

  /* ── composer ── */
  .composebar { flex:0 0 auto; padding:8px 24px 14px; }
  .composer { max-width:860px; margin:0 auto; background:var(--surface); border:1px solid var(--line);
              border-radius:24px; padding:10px 14px 8px; box-shadow:0 4px 24px rgba(0,0,0,.35); }
  .freq { display:flex; flex-wrap:wrap; gap:6px; padding:2px 2px 6px; }
  .freq:empty { display:none; }
  .attach-chip { display:inline-flex; align-items:center; gap:6px; background:var(--accent); color:#131314;
                 border-radius:999px; padding:4px 10px; font-size:11.5px; cursor:pointer; max-width:280px; }
  .attach-chip .fn { font-weight:600; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .attach-chip .rm { width:16px; height:16px; border-radius:999px; display:flex; align-items:center; justify-content:center;
                     background:rgba(19,19,20,.18); font-size:10px; cursor:pointer; flex:0 0 16px; }
  .attach-chip .rm:hover { background:rgba(19,19,20,.35); color:#fff; }
  .attach-open { display:none; width:100%; margin:4px 0; }
  .attach-open pre { background:var(--code); border:1px solid var(--line); border-radius:8px; padding:8px 10px;
                     max-height:180px; overflow:auto; font-size:11px; color:#b9bdc5; white-space:pre-wrap; word-wrap:break-word; }
  textarea#input { width:100%; border:0; background:transparent; resize:none; color:var(--text);
                   font:14px/1.55 inherit; font-family:inherit; outline:none; max-height:160px; padding:2px 2px 0; }
  textarea#input::placeholder { color:var(--muted); }
  .composer-bottom { display:flex; align-items:center; gap:10px; padding-top:2px; -webkit-app-region:no-drag; }
  select { background:var(--input); color:var(--text); border:0; border-radius:999px; padding:5px 26px 5px 12px;
           font-size:12px; appearance:none; cursor:pointer; outline:none;
           background-image:url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='9' height='9' viewBox='0 0 24 24' fill='none' stroke='%238e918f' stroke-width='2.6' stroke-linecap='round'><path d='M6 9l6 6 6-6'/></svg>");
           background-repeat:no-repeat; background-position:right 9px center; }
  select:hover { background-color:var(--hover); }
  .ringwrap { display:flex; align-items:center; gap:5px; }
  .ringwrap svg { width:22px; height:22px; }
  .ringwrap .ringlabel { color:var(--muted); font-size:10px; }
  .foot { text-align:center; color:var(--muted); font-size:10.5px; padding:0 0 6px; user-select:none; }
  .circle { width:34px; height:34px; border-radius:999px; border:0; background:var(--accent); color:#131314;
            display:flex; align-items:center; justify-content:center; cursor:pointer; flex:0 0 34px; }
  .circle:hover { filter:brightness(1.08); }
  .circle.stop { background:var(--red); color:#fff; }
  .hidden { display:none !important; }

  /* ── autocomplete ── */
  .ac { position:fixed; background:var(--surface); border:1px solid var(--line);
        border-radius:8px; box-shadow:0 8px 24px rgba(0,0,0,.4); z-index:80;
        min-width:220px; max-width:360px; max-height:280px; overflow:auto; }
  .ac .item { padding:6px 10px; cursor:pointer; font-size:12.5px; display:flex; gap:8px; }
  .ac .item:hover, .ac .item.sel { background:var(--hover); }
  .ac .item .sigil { color:var(--accent); font-weight:600; flex:0 0 14px; }
  .ac .item .path { color:var(--text); white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .ac .item .path.muted { color:var(--muted); font-size:11px; }
  .ac .divider { height:1px; background:var(--line); margin:4px 0; }

  /* ── drawers ── */
  #backdrop { position:fixed; inset:0; background:rgba(0,0,0,.45); opacity:0; pointer-events:none;
              transition:opacity .18s; z-index:60; }
  #backdrop.open { opacity:1; pointer-events:auto; }
  .drawer { position:fixed; top:0; left:0; bottom:0; width:330px; background:var(--surface);
            border-right:1px solid var(--line); z-index:70; transform:translateX(-100%);
            transition:transform .2s ease-out, visibility 0s .25s; display:flex; flex-direction:column;
            visibility:hidden; }
  .drawer.open { transform:translateX(0); visibility:visible;
                 transition:transform .2s ease-out, visibility 0s; }
  .drawer:not(.open) { visibility:hidden; }
  .drawer.right { left:auto; right:0; border-right:0; border-left:1px solid var(--line);
                  transform:translateX(100%); }
  .drawer.right.open { transform:translateX(0); }
  #backdrop { position:fixed; inset:0; background:rgba(0,0,0,.45); opacity:0; pointer-events:none;
              transition:opacity .18s; z-index:60; }
  #backdrop.open { opacity:1; pointer-events:auto; }
  .drawer .dhead { display:flex; align-items:center; gap:8px; padding:14px 16px 10px;
                   font-size:14px; font-weight:600; }
  .drawer .dbody { flex:1; overflow-y:auto; padding:0 10px 12px; }
  .sessionrow { display:flex; align-items:center; gap:8px; padding:9px 10px; border-radius:10px;
                cursor:pointer; color:var(--text); font-size:12.5px; }
  .sessionrow:hover { background:var(--hover); }
  .sessionrow.active { background:var(--hover); }
  .sessionrow .sname { flex:1; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .sessionrow .sdel { color:var(--muted); width:22px; height:22px; border-radius:6px; display:flex;
                      align-items:center; justify-content:center; font-size:10px; flex:0 0 22px; }
  .sessionrow .sdel:hover { color:var(--red); background:rgba(239,68,68,.12); }
  .drawer .dfoot { padding:10px 16px 14px; border-top:1px solid var(--line); }
  .ghostbtn { width:100%; text-align:center; background:transparent; border:1px solid var(--line);
              color:var(--muted); border-radius:10px; padding:8px; font-size:12px; cursor:pointer; }
  .ghostbtn:hover { color:var(--text); background:var(--hover); }
  .setgroup { padding:10px 6px; border-bottom:1px solid var(--line); }
  .setgroup .sl { color:var(--muted); font-size:10px; letter-spacing:.09em; text-transform:uppercase; margin:0 0 6px; }
  .setitem { display:flex; align-items:center; gap:8px; padding:7px 8px; border-radius:9px;
             cursor:pointer; color:var(--text); font-size:12.5px; }
  .setitem:hover { background:var(--hover); }
  .setitem .muted { color:var(--muted); font-size:11px; flex:1; white-space:nowrap; overflow:hidden;
                    text-overflow:ellipsis; text-align:right; }
  .aboutbit { color:var(--muted); font-size:11.5px; padding:7px 8px; }

  /* ── dialogs ── */
  dialog { background:var(--surface); color:var(--text); border:1px solid var(--line); border-radius:14px;
           padding:18px; width:min(420px, 86vw); box-shadow:0 18px 60px rgba(0,0,0,.6); }
  dialog::backdrop { background:rgba(0,0,0,.5); }
  dialog h3 { margin:0 0 8px; font-size:14.5px; }
  dialog .dtext { color:var(--text); font-size:13px; white-space:pre-wrap; }
  dialog .dmuted { color:var(--muted); }
  dialog .drowbtns { display:flex; gap:8px; justify-content:flex-end; margin-top:16px; }
  dialog .btn { border:1px solid var(--line); background:transparent; color:var(--text);
                border-radius:9px; padding:7px 16px; font-size:12.5px; cursor:pointer; }
  dialog .btn:hover { background:var(--hover); }
  dialog .btn.accent { background:var(--accent); color:#131314; border-color:transparent; font-weight:600; }
  dialog .btn.accent:hover { filter:brightness(1.08); }
  dialog input { width:100%; margin-top:10px; background:var(--input); color:var(--text); border:1px solid var(--line);
                 border-radius:9px; padding:8px 10px; font-size:13px; outline:none; }
  dialog textarea { width:100%; margin-top:10px; background:var(--input); color:var(--text); border:1px solid var(--line);
                    border-radius:9px; padding:8px 10px; font-size:12px; height:200px; outline:none;
                    resize:none; font-family:"Fira Code",Consolas,monospace; white-space:pre-wrap; }
  dialog select { width:100%; margin-top:10px; background:var(--input); border-radius:9px; padding:8px 10px;
                  color:var(--text); font-size:13px; appearance:auto; }
</style></head>
<body>
<header>
  <button class="iconbtn" id="history-btn" title="Chat History">
    <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="3.5" y="4" width="17" height="16" rx="2.5"/><path d="M9.5 4v16"/></svg>
  </button>
  <span class="wordmark">Gamma Code</span>
  <span class="spacer"></span>
  <span class="ver" id="ver"></span>
  <button class="iconbtn" id="newchat-btn" title="New Chat">
    <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>
  </button>
  <button class="iconbtn" id="settings-btn" title="Workspace & info">
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 8h10M18 8h2M4 16h2M10 16h10"/><circle cx="16" cy="8" r="2"/><circle cx="8" cy="16" r="2"/></svg>
  </button>
  <div class="winctl">
    <button class="iconbtn" id="win-min" title="Minimize"><svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M5 12h14"/></svg></button>
    <button class="iconbtn" id="win-max" title="Maximize"><svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><rect x="5" y="5" width="14" height="14" rx="1.5"/></svg></button>
    <button class="iconbtn close" id="win-close" title="Close"><svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg></button>
  </div>
</header>

<main id="chat-col"><div class="colwrap" id="chat"></div></main>

<div class="composebar">
  <div class="composer" id="composer">
    <div class="freq" id="attachrow"></div>
    <textarea id="input" rows="1" placeholder="Ask anything..."></textarea>
    <div class="composer-bottom">
      <button class="iconbtn" id="attach-btn" title="Attach files">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>
      </button>
      <div class="ringwrap" id="ctxringw" title="context usage">
        <svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9" fill="none" stroke="#37393b" stroke-width="4"/>
          <circle id="ctxring" cx="12" cy="12" r="9" fill="none" stroke="#a8c7fa" stroke-width="4"
                  stroke-linecap="round" stroke-dasharray="56.5" stroke-dashoffset="56.5"
                  transform="rotate(-90 12 12)"/></svg>
        <span class="ringlabel" id="ringlabel"></span>
      </div>
      <select id="model" title="model"></select>
      <select id="mode" title="mode">
        <option value="plan">plan &#183; read-only</option>
        <option value="ask">ask &#183; ask-first</option>
        <option value="auto">auto &#183; hands-free</option>
      </select>
      <span class="spacer"></span>
      <button class="circle stop hidden" id="stop-btn" title="Stop">
        <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>
      </button>
      <button class="circle" id="send-btn" title="Send (Enter)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M12 19V5M5 12l7-7 7 7"/></svg>
      </button>
    </div>
  </div>
  <div class="foot" id="foot">Ollama local instance &#183; local privacy preserved</div>
</div>

<div id="boot"><div class="ring"></div><div class="label">Opening Ollama&#8230;</div></div>
<div id="backdrop"></div>

<div class="drawer" id="history-drawer">
  <div class="dhead"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="3.5" y="4" width="17" height="16" rx="2.5"/><path d="M9.5 4v16"/></svg>Chat History</div>
  <div class="dbody" id="sessionlist"></div>
  <div class="dfoot"><button class="ghostbtn" id="clear-history-btn">Clear all</button></div>
</div>

<div class="drawer right" id="settings-drawer">
  <div class="dhead"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 8h10M18 8h2M4 16h2M10 16h10"/><circle cx="16" cy="8" r="2"/><circle cx="8" cy="16" r="2"/></svg>Settings</div>
  <div class="dbody">
    <div class="setgroup">
      <div class="sl">Workspace</div>
      <div class="setitem" id="workspace"><svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"/></svg><span>Change folder</span><span class="muted" id="workspace-name"></span></div>
      <div class="setitem" id="prompt-open"><svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M9 12h6M9 16h4M6 3h9l5 5v13H6z"/></svg><span>System prompt</span></div>
    </div>
    <div class="setgroup"><div class="sl">About</div>
      <div class="aboutbit" id="about-ver">gamma &#183; local-only</div>
      <div class="aboutbit">Traffic stays on 127.0.0.1 &#8212; nothing leaves this machine.</div>
    </div>
  </div>
</div>

<dialog id="confirm-dialog">
  <h3>Confirm</h3>
  <div class="dtext" id="confirm-text"></div>
  <div class="drowbtns"><button class="btn" id="confirm-no">Deny</button><button class="btn accent" id="confirm-yes">Allow</button></div>
</dialog>

<dialog id="ask-dialog">
  <h3>gamma wants to know</h3>
  <div class="dtext" id="ask-question" style="margin-bottom:8px"></div>
  <select id="ask-select" class="hidden"></select>
  <input id="ask-input" class="hidden" autocomplete="off">
  <div class="drowbtns"><button class="btn" id="ask-no">Cancel</button><button class="btn accent" id="ask-yes">Send</button></div>
</dialog>

<dialog id="prompt-dialog">
  <h3>Live system prompt</h3>
  <textarea id="prompt-text" readonly></textarea>
  <div class="drowbtns"><button class="btn" id="prompt-close">Close</button></div>
</dialog>

<script>
  const post = (msg) => window.chrome.webview.postMessage(msg);
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => '&#' + c.charCodeAt(0) + ';');
  const $ = (id) => document.getElementById(id);
  const chatCol = $('chat-col');
  const mainEl = $('chat');
  const input = $('input');
  const sendBtn = $('send-btn');
  const stopBtn = $('stop-btn');
  const welcome = document.createElement('div');
  welcome.id = 'welcome';
  welcome.innerHTML = `
    <div class="star"><svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor"><path d="M12 2l2.4 6.2L21 10l-6.6 1.8L12 18l-2.4-6.2L3 10l6.6-1.8z"/></svg></div>
    <h1>Hello, I'm gamma.</h1>
    <p>Your local coding agent &#8212; reads, edits and runs on your machine, nothing leaves it.</p>
    <div class="cards">
      <div class="card" data-fill="Explain this project"><span>"Explain this project"</span><span class="arr">&#8594;</span></div>
      <div class="card" data-fill="Find and fix a bug"><span>"Find and fix a bug"</span><span class="arr">&#8594;</span></div>
      <div class="card" data-fill="Review my changes"><span>"Review my changes"</span><span class="arr">&#8594;</span></div>
      <div class="card" data-fill="Write a README section"><span>"Write a README section"</span><span class="arr">&#8594;</span></div>
    </div>`;
  welcome.querySelectorAll('.card').forEach((c) => {
    c.onclick = () => { input.value = c.dataset.fill; input.focus(); heightSync(); };
  });

  let busy = false;
  let started = true;          // bootstrap finished?
  let chipEl = null;           // status chip in the live turn
  let chipDetail = null;
  let chipOpen = false;
  let chipPast = false;
  let chipCounts = {};         // { Read: 2, Edit: 1 }
  let chipRows = [];           // [{name, ok}]
  let activeSession = '';
  let modelVal = '';
  let attachFiles = [];        // [{path, content, open}]
  let pendingConfirm = null;
  let pendingAsk = null;

  const PAST = { working:'worked', thinking:'thought', composing:'composed', reading:'read',
                 writing:'wrote', editing:'edited', listing:'listed', searching:'searched',
                 running:'ran', waiting:'answered', writing:'edited', fixing:'fixed' };
  // tool name -> chip group
  const GROUPOF = { read:'Read', write:'Write', edit:'Edit', list:'List', search:'Search',
                    run:'Run', web:'Web', ask:'Ask', mkdir:'Write', glob:'List' };
  const EXCLUDE = { thought:1, worked:1, session:1 };

  function heightSync() {
    input.style.height = 'auto';
    input.style.height = Math.min(input.scrollHeight, 160) + 'px';
  }

  // ── autocomplete ──
  let acFiles = [];
  let acEl = null;
  let acSel = -1;
  let acType = null; // '@' or '/'
  let acStart = 0;

  function requestFileList() {
    post({ action: 'list-files' });
  }

  function showAC(type, startPos) {
    acType = type;
    acStart = startPos;
    acSel = -1;
    const text = input.value.slice(0, startPos);
    const prefix = text.slice(text.lastIndexOf(type) + 1).toLowerCase();
    const filtered = acFiles.filter(f => f.toLowerCase().includes(prefix));
    if (!filtered.length) { hideAC(); return; }
    if (acEl) acEl.remove();
    acEl = document.createElement('div');
    acEl.className = 'ac';
    filtered.forEach((f, i) => {
      const item = document.createElement('div');
      item.className = 'item';
      item.innerHTML = '<span class="sigil">' + (type === '@' ? '@' : '/') + '</span>' +
        '<span class="path">' + esc(f) + '</span>';
      item.onclick = () => pickAC(f);
      item.onmouseenter = () => { acSel = i; updateACSel(); };
      acEl.appendChild(item);
    });
    document.body.appendChild(acEl);
    positionAC();
    updateACSel();
  }

  function positionAC() {
    if (!acEl) return;
    const rect = input.getBoundingClientRect();
    const lines = input.value.slice(0, acStart).split('\n');
    const lineHeight = 21.7; // 14px * 1.55
    const top = rect.top + (lines.length - 1) * lineHeight + 4;
    const left = rect.left + (input.value.slice(0, acStart).split('\n').pop().length || 0) * 8.4;
    acEl.style.top = top + 'px';
    acEl.style.left = left + 'px';
  }

  function updateACSel() {
    if (!acEl) return;
    acEl.querySelectorAll('.item').forEach((it, i) => it.classList.toggle('sel', i === acSel));
  }

  function hideAC() {
    if (acEl) { acEl.remove(); acEl = null; }
    acType = null;
  }

  function pickAC(file) {
    const before = input.value.slice(0, acStart - acType.length);
    const after = input.value.slice(acStart);
    input.value = before + (acType === '@' ? '@' : '/') + file + ' ' + after;
    heightSync();
    hideAC();
    input.focus();
  }

  function handleACInput() {
    const val = input.value;
    const pos = input.selectionStart;
    // Find last @ or / before cursor
    const at = val.lastIndexOf('@', pos - 1);
    const sl = val.lastIndexOf('/', pos - 1);
    if (at > sl && at >= 0 && (at === 0 || /\s/.test(val[at - 1]))) {
      showAC('@', at);
    } else if (sl >= 0 && (sl === 0 || /\s/.test(val[sl - 1]))) {
      showAC('/', sl);
    } else {
      hideAC();
    }
  }

  input.addEventListener('input', (e) => {
    heightSync();
    handleACInput();
  });
  input.addEventListener('keydown', (e) => {
    if (acEl) {
      if (e.key === 'ArrowDown') { e.preventDefault(); acSel = Math.min(acSel + 1, acEl.children.length - 1); updateACSel(); return; }
      if (e.key === 'ArrowUp') { e.preventDefault(); acSel = Math.max(acSel - 1, 0); updateACSel(); return; }
      if (e.key === 'Enter' || e.key === 'Tab') { e.preventDefault(); if (acSel >= 0) pickAC(acEl.children[acSel].querySelector('.path').textContent); return; }
      if (e.key === 'Escape') { e.preventDefault(); hideAC(); return; }
    }
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); }
  });
  input.addEventListener('blur', () => setTimeout(hideAC, 150));
  input.addEventListener('scroll', positionAC);
  window.addEventListener('resize', positionAC);

  function setBusy(on) {
    busy = on;
    sendBtn.classList.toggle('hidden', on);
    stopBtn.classList.toggle('hidden', !on);
  }
  function hideWelcome() { if (welcome.parentNode) welcome.remove(); }
  function showWelcome() {
    if (!welcome.parentNode) mainEl.appendChild(welcome);
  }
  function scrollDown() { chatCol.scrollTop = chatCol.scrollHeight; }

  function addUser(t) {
    const d = document.createElement('div');
    d.className = 'turn u-turn';
    d.innerHTML = '<div class="u-bubble">' + esc(t) + '</div>';
    mainEl.appendChild(d);
    scrollDown();
  }
  function addTurn() {
    const d = document.createElement('div');
    d.className = 'turn g-turn';
    d.innerHTML = '<div class="g-avatar">&#947;</div><div class="g-body"><div class="prose"></div></div>';
    mainEl.appendChild(d);
    return d;
  }
  function makeStep(name, args) {
    const d = document.createElement('div');
    d.className = 'toolstep';
    const oneLine = args ? oneLineArgs(args, name) : '';
    d.innerHTML = '<div class="row"><span class="twisty">&#9654;</span>' +
      '<span class="tname"><span class="mark pending"></span><span>' + esc(TITLEOF[name] || name) +
      (oneLine ? ' <span style="color:var(--muted)">' + esc(oneLine) + '</span>' : '') +
      '</span></span></div><div class="tdetail"><div class="lk">Called with</div><pre class="aargs"></pre>' +
      '<div class="lk">Result</div><pre class="rret"></pre></div>';
    const tw = d.querySelector('.twisty');
    d.querySelector('.row').addEventListener('click', () => {
      d.classList.toggle('open');
      tw.innerHTML = d.classList.contains('open') ? '&#9660;' : '&#9654;';
    });
    mainEl.appendChild(d);
    return d;
  }
  const TITLEOF = { read:'Read', write:'Write', edit:'Edit', list:'List', search:'Search',
                    run:'Run', web:'Web', ask:'Ask' };
  function oneLineArgs(argsJson, name) {
    try {
      const a = JSON.parse(argsJson);
      if (a.path) return a.path;
      if (a.query) return '"' + a.query + '"';
      if (a.pattern) return a.pattern;
      if (a.command) return a.command;
      if (a.question) return a.question;
      return '';
    } catch (e) { return ''; }
  }
  function toolGroup(n) {
    const base = String(n || '').toLowerCase();
    const k = base.replace(/[^a-z]/g, '');
    return GROUPOF[k] || (k.charAt(0).toUpperCase() + k.slice(1));
  }

  // ── status chip ──
  function setStatusChip(label) {
    if (!label) return;
    if (!chipEl || !chipEl.parentNode) makeChip();
    if (chipPast) chipPast = false;
    chipEl.querySelector('.slabel').innerHTML = esc(label) + (label.slice(-1) === '\u2026' ? '' : '&#8230;');
    chipEl.classList.remove('past');
  }
  function makeChip() {
    chipEl = document.createElement('div');
    chipEl.className = 'statuschip';
    chipEl.innerHTML = '<span class="dotspin"></span><span class="slabel"></span>' +
      '<span class="chev">&#9654;</span>';
    chipDetail = document.createElement('div');
    chipDetail.className = 'statuschip-detail';
    const tw = chipEl.querySelector('.chev');
    chipEl.querySelector('.chev').addEventListener('click', (e) => {
      e.stopPropagation();
      chipOpen = !chipOpen;
      chipEl.classList.toggle('open', chipOpen);
      chipDetail.classList.toggle('open', chipOpen);
    });
    insertChip();
  }
  function insertChip() {
    if (!chipEl) return;
    if (chipDetail.nextSibling === null && mainEl.lastChild === chipEl) { /* fine */ }
    mainEl.appendChild(chipEl);
    mainEl.appendChild(chipDetail);
    scrollDown();
  }
  function chipGroupSummary() {
    return Object.keys(chipCounts).sort().map((k) => esc(k) + ' (' + chipCounts[k] + ')').join(', ');
  }
  function updateChipFromTools() {
    if (!chipEl) makeChip();
    const s = chipGroupSummary();
    if (s) chipEl.querySelector('.slabel').innerHTML = s;
  }
  function chipToPast(verb) {
    if (!chipEl) verb = verb || 'worked';
    if (!chipEl) return;
    chipPast = true;
    chipEl.classList.add('past');
    const label = chipEl.querySelector('.slabel');
    const prev = label.innerHTML;
    const first = (chipCounts && Object.keys(chipCounts).length) ? chipGroupSummary() : '';
    label.innerHTML = first || 'Worked';
    chipEl.classList.remove('open');
  }
  function dropChip() {
    if (chipEl) { chipEl.remove(); chipEl = null; }
    if (chipDetail) { chipDetail.remove(); chipDetail = null; }
    chipOpen = false; chipPast = false; chipCounts = {}; chipRows = [];
  }

  // ── tool steps ──
  let lastTool = null;
  function addToolStep(name, argsJson) {
    addChipRow(name);
    lastTool = makeStep(name, argsJson);
    const a = lastTool.querySelector('.aargs');
    try { a.textContent = JSON.stringify(JSON.parse(argsJson), null, 2); } catch (e) { a.textContent = argsJson || '{}'; }
    scrollDown();
  }
  function addChipRow(name) {
    const g = toolGroup(name);
    if (EXCLUDE[String(name).toLowerCase()]) return;
    chipCounts[g] = (chipCounts[g] || 0) + 1;
    chipRows.push({ name, ok: null });
    updateChipFromTools();
  }
  function finishTool(name, ok, output, error) {
    if (lastTool) {
      const mark = lastTool.querySelector('.mark');
      mark.classList.remove('pending');
      mark.classList.add(ok ? 'ok' : 'fail');
      mark.innerHTML = ok ? '&#10003;' : '&#10007;';
      const r = lastTool.querySelector('.rret');
      r.textContent = (ok ? output : error) || (ok ? '(no output)' : '');
      if (!ok || (output && output.length < 500)) lastTool.classList.add('open');
    }
  }
  function makeThought() {
    const d = document.createElement('div');
    d.className = 'thought';
    d.innerHTML = '<div class="thead"><span class="twisty">&#9654;</span><span>Thought</span>' +
      '<span class="tpreview"></span></div><div class="tbody"></div>';
    d.querySelector('.thead').addEventListener('click', () => d.classList.toggle('open'));
    return d;
  }
  function makeCodeCard(lang) {
    const d = document.createElement('div');
    d.className = 'codecard';
    d.innerHTML = '<div class="lang"><span>' + esc(lang || 'code') + '</span>' +
      '<span class="copy">copy</span></div><pre></pre>';
    d.querySelector('.copy').addEventListener('click', () => {
      const t = d.querySelector('pre').textContent;
      navigator.clipboard.writeText(t);
    });
    return d;
  }

  // ── streaming: markdown-lite incremental builder ──
  let streamTurn = null;      // current g-turn
  let proseEl = null;         // current prose node
  let cursorEl = null;
  let pendingText = '';       // un-consumed streamed text
  let inFence = null;         // {kind:'code'|'thought', lang, el, buf}
  let shownText = '';         // text already processed

  function beginTurn() {
    streamTurn = addTurn();
    proseEl = streamTurn.querySelector('.prose');
    cursorEl = document.createElement('span');
    cursorEl.className = 'stream-cursor';
    proseEl.appendChild(cursorEl);
  }
  function endStream() {
    // flush leftover fence/pending
    feed('');
    if (cursorEl && cursorEl.parentNode) cursorEl.remove();
    cursorEl = null;
    if (streamTurn && !streamTurn.querySelector('.prose') === false) {}
  }
  function openFence(lang, kind, fenceElParent) { }
  function closeFence() { }

  function fmt(text) { return esc(text); }

  function feed(delta) {
    if (!delta) return;
    if (!chipPast && chipEl) chipToPast();        // prose started &#8594; "Working" becomes "worked"
    if (!streamTurn) { beginTurn(); hideWelcome(); }
    // If we see a thought fence opening, flip chip to "thinking…"
    if (inFence && inFence.kind === 'thought' && chipEl && !chipPast) {
      chipEl.querySelector('.slabel').innerHTML = 'thinking&#8230;';
    }
    pendingText += delta;
    let buf = '';
    let i = 0;
    while (i < pendingText.length) {
      const rest = pendingText.slice(i);
      if (inFence) {
        const closeIdx = rest.indexOf('```');
        if (closeIdx >= 0) {
          inFence.buf += rest.slice(0, closeIdx);
          buf += rest.slice(0, closeIdx);
          i += closeIdx + 3;
          // closing: trailing content on close line ignored (assume newline)
          const nl = pendingText.slice(i);
          if (nl.startsWith('\n')) i += 1;
          if (inFence.kind === 'thought') {
            inFence.el.querySelector('.tbody').textContent = inFence.buf.trim().replace(/^#?thought[:\s]*/i, '');
            inFence.el.querySelector('.tpreview').textContent = inFence.buf.trim().split('\n')[0].slice(0, 60);
          } else {
            inFence.el.querySelector('pre').textContent = inFence.buf;
          }
          inFence = null;
          continue;
        }
        inFence.buf += rest;
        buf += rest;
        i += rest.length;
        break;
      }
      const openIdx = rest.indexOf('```');
      if (openIdx < 0) { buf += rest; i += rest.length; break; }
      // text before fence
      buf += rest.slice(0, openIdx);
      i += openIdx + 3;
      const nlIdx = pendingText.indexOf('\n', i);
      const lang = (nlIdx >= 0 ? pendingText.slice(i, nlIdx) : pendingText.slice(i)).trim().toLowerCase();
      if (nlIdx >= 0) i = nlIdx + 1;
      const isThought = lang === 'thought' || lang === 'thinking';
      const fenceEl = isThought ? makeThought() : makeCodeCard(lang);
      mainEl.insertBefore(fenceEl, cursorEl.parentNode === proseEl ? streamTurn : null) ;
      streamTurn.appendChild(fenceEl);
      inFence = { kind: isThought ? 'thought' : 'code', lang, el: fenceEl, buf: '' };
      if (isThought) fenceEl.classList.add('open');
    }
    if (buf.length) touchProse(buf);
    pendingText = '';
  }
  function touchProse(buf) {
    // append to the current text node right before cursor
    const tn = cursorEl && cursorEl.previousSibling && cursorEl.previousSibling.nodeType === 3
      ? cursorEl.previousSibling : null;
    if (tn) { tn.textContent += buf; }
    else {
      const node = document.createTextNode(buf);
      proseEl.insertBefore(node, cursorEl);
    }
    scrollDown();
  }

  // ── attach chips ──
  function renderAttach() {
    const row = $('attachrow');
    row.innerHTML = '';
    attachFiles.forEach((f, idx) => {
      const chip = document.createElement('span');
      chip.className = 'attach-chip';
      const fname = f.path.split(/[\\/]/).pop();
      chip.innerHTML = '<span style="font-weight:600">@</span><span class="fn">' + esc(fname) + '</span>' +
        '<span class="rm">&#10005;</span>';
      chip.title = f.path;
      chip.onclick = (e) => {
        if (e.target === chip.querySelector('.rm')) {
          attachFiles.splice(idx, 1);
          renderAttach();
          return;
        }
        f.open = !f.open;
        renderOpen();
      };
      row.appendChild(chip);
    });
    renderOpen();
  }
  function renderOpen() {
    let box = $('attach-open');
    if (!box) {
      box = document.createElement('div');
      box.className = 'attach-open';
      box.id = 'attach-open';
      $('attachrow').appendChild(box);
    }
    const open = attachFiles[0];
    if (open && open.open) {
      box.style.display = 'block';
      box.innerHTML = '<pre>' + esc(open.content.slice(0, 8000)) + '</pre>';
    } else {
      box.style.display = 'none';
      box.innerHTML = '';
    }
  }

  // ── drawers ──
  const historyDrawer = $('history-drawer');
  const settingsDrawer = $('settings-drawer');
  const backdrop = $('backdrop');
  function closeDrawers() {
    historyDrawer.classList.remove('open');
    settingsDrawer.classList.remove('open');
    backdrop.classList.remove('open');
  }
  function openDrawer(which) {
    closeDrawers();
    (which === 'history' ? historyDrawer : settingsDrawer).classList.add('open');
    backdrop.classList.add('open');
    if (which === 'history') post({ action: 'load-history' });
  }
  backdrop.onclick = closeDrawers;

  function renderSessions(sessions) {
    const list = $('sessionlist');
    list.innerHTML = '';
    if (!sessions || !sessions.length) {
      list.innerHTML = '<div class="aboutbit" style="text-align:center;padding:20px 0;color:var(--muted)">No saved sessions yet.</div>';
      return;
    }
    sessions.forEach((s) => {
      const bare = String(s.name || '').replace(/\.json$/i, '');
      const isActive = s.path === activeSession || activeSession.endsWith(s.name);
      const row = document.createElement('div');
      row.className = 'sessionrow' + (isActive ? ' active' : '');
      row.innerHTML = '<span class="sname">' + esc(s.name || bare) + '</span>' +
        '<span class="sdel" title="Delete">&#10005;</span>';
      row.onclick = (e) => {
        if (e.target === row.querySelector('.sdel')) {
          post({ action: 'delete-session', path: s.path });
          return;
        }
        closeDrawers();
        if (!busy) post({ action: 'resume-session', path: s.path });
      };
      list.appendChild(row);
    });
  }

  // ── context ring ──
  function renderRing(used, budget) {
    const ring = $('ctxring');
    const label = $('ringlabel');
    if (!ring) return;
    if (!budget || budget <= 0) { ring.style.opacity = '.3'; label.textContent = ''; return; }
    ring.style.opacity = '1';
    const frac = Math.min(1, used / budget);
    ring.setAttribute('stroke-dashoffset', String(56.5 * (1 - frac)));
    label.textContent = fmtK(used) + ' / ' + fmtK(budget);
  }
  function fmtK(n) {
    n = Number(n) || 0;
    return n >= 1000 ? (Math.round(n / 100) / 10) + 'k' : String(n);
  }

  // ── dialogs ──
  function showConfirm(text) {
    pendingConfirm = true;
    $('confirm-text').textContent = text;
    $('confirm-dialog').showModal();
  }
  function answerConfirm(yes) {
    $('confirm-dialog').close();
    post({ action: 'confirm-result', yes: yes, text: '' });
  }
  $('confirm-yes').onclick = () => answerConfirm(true);
  $('confirm-no').onclick = () => answerConfirm(false);

  function showAsk(q, options) {
    pendingAsk = true;
    $('ask-question').textContent = q;
    const sel = $('ask-select');
    const inp = $('ask-input');
    if (options && options.length) {
      sel.classList.remove('hidden');
      sel.innerHTML = '';
      options.forEach((o) => {
        const b = document.createElement('option');
        b.value = o; b.textContent = o;
        sel.appendChild(b);
      });
      inp.classList.add('hidden');
    } else {
      sel.classList.add('hidden');
      inp.classList.remove('hidden');
      inp.value = '';
    }
    $('ask-dialog').showModal();
  }
  $('ask-yes').onclick = () => {
    const t = $('ask-select').classList.contains('hidden') ? $('ask-input').value : $('ask-select').value;
    $('ask-dialog').close();
    post({ action: 'confirm-result', yes: true, text: t });
  };
  $('ask-no').onclick = () => {
    $('ask-dialog').close();
    post({ action: 'confirm-result', yes: false, text: '' });
  };
  $('prompt-close').onclick = () => $('prompt-dialog').close();

  // ── inbound events ──
  window.chrome.webview.addEventListener('message', (e) => {
    const m = e.data;
    if (!m || !m.type) return;
    switch (m.type) {
      case 'state': {
        activeSession = m.session || '';
        $('ver').textContent = 'v' + (m.version || '');
        $('about-ver').textContent = 'gamma \u00b7 local-only \u00b7 v' + (m.version || '');
        $('workspace-name').textContent = m.workspace || '';
        modelVal = m.model || '';
        const selM = $('model');
        selM.innerHTML = '';
        (m.models || []).forEach((mm) => {
          const o = document.createElement('option');
          o.value = mm; o.textContent = mm;
          if (mm === modelVal) o.selected = true;
          selM.appendChild(o);
        });
        if (!m.models || !m.models.length) {
          const o = document.createElement('option');
          o.value = ''; o.textContent = modelVal || 'no models';
          selM.appendChild(o);
        }
        $('mode').value = m.mode || 'auto';
        renderSessions(m.sessions || []);
        renderRing(m.ctx_used, m.ctx_budget);
        bootShow(!!m.starting);
        break;
      }
      case 'file-list': {
        acFiles = m.files || [];
        break;
      }
      case 'session-name': {
        // session renamed live &#8212; refresh list lazily
        break;
      }
      case 'delta': {
        feed(m.text || '');
        break;
      }
      case 'status': {
        setStatusChip(m.label);
        break;
      }
      case 'tool': {
        addToolStep(m.name, m.args);
        break;
      }
      case 'tool-result': {
        finishTool(m.name, m.ok, m.output, m.error);
        break;
      }
      case 'final': {
        feed(String(m.text || ''));
        endStream();
        if (chipEl) chipToPast();
        break;
      }
      case 'notice': {
        const t = document.createElement('div');
        t.className = 'toast';
        t.style.color = m.error ? 'var(--red)' : 'var(--text)';
        t.textContent = m.text;
        document.body.appendChild(t);
        setTimeout(() => t.remove(), 4200);
        break;
      }
      case 'busy': {
        setBusy(!!m.value);
        break;
      }
      case 'clear': {
        mainEl.innerHTML = '';
        lastTool = null; streamTurn = null;
        dropChip();
        showWelcome();
        break;
      }
      case 'load': {
        renderTranscript(m.messages || []);
        break;
      }
      case 'prompt': {
        $('prompt-text').value = m.text || '';
        $('prompt-dialog').showModal();
        break;
      }
      case 'confirm': {
        showConfirm(m.text || '');
        break;
      }
      case 'ask': {
        showAsk(m.question || '', m.options);
        break;
      }
      case 'files-attached': {
        (m.files || []).forEach((f) => {
          attachFiles.push({ path: f.path, content: f.content || '', open: false });
        });
        renderAttach();
        const t = document.createElement('div');
        t.className = 'toast';
        t.textContent = (m.files || []).length + ' file(s) attached';
        document.body.appendChild(t);
        setTimeout(() => t.remove(), 2600);
        break;
      }
    }
  });

  function renderTranscript(messages) {
    mainEl.innerHTML = '';
    dropChip();
    streamTurn = null; proseEl = null; cursorEl = null; lastTool = null;
    if (!messages.length) { showWelcome(); return; }
    hideWelcome();
    messages.forEach((msg) => {
      if (msg.role === 'user') { addUser(msg.text || msg.content || ''); return; }
      if (msg.role === 'tool') {
        // payload row accompany&#8203;ing pending tool call
        let parsed = null;
        try { parsed = JSON.parse(msg.content || '{}'); } catch (e) {}
        if (parsed) {
          // graft: find last pending toolstep by name
          const steps = mainEl.querySelectorAll('.toolstep');
          let target = null;
          for (let i = steps.length - 1; i >= 0; --i) {
            const mark = steps[i].querySelector('.mark');
            if (mark && mark.classList.contains('pending')) { target = steps[i]; break; }
          }
          if (target) {
            finishTool(parsed.name || '', !!parsed.ok, parsed.output || '', parsed.error || '');
          }
        }
        return;
      }
      if (msg.role === 'assistant') {
        if (msg.tools && msg.tools.length) {
          msg.tools.forEach((tc) => addToolStep(tc.name, tc.arguments));
        }
        const text = msg.text || msg.content || '';
        if (text) {
          beginTurn();
          feed(text);
          endStream();
        }
        return;
      }
    });
    scrollDown();
  }

  // ── send / stop ──
  function send() {
    const t = input.value.trim();
    const ATTACH = attachFiles.slice();
    const filesPart = ATTACH.map((f) => {
      const name = f.path.split(/[\\/]/).pop();
      return '[file: ' + name + ']\n' + f.content;
    }).join('\n\n');
    const fullText = t + (filesPart ? (t ? '\n\n' : '') + filesPart : '');
    if (!fullText.trim() || busy) return;
    hideWelcome();
    addUser(fullText);
    input.value = '';
    heightSync();
    attachFiles = [];
    renderAttach();
    dropChip();
    makeChip();
    chipEl.querySelector('.slabel').innerHTML = 'Working&#8230;';
    setBusy(true);
    post({ action: 'send', text: fullText });
    scrollDown();
  }
  sendBtn.onclick = send;
  stopBtn.onclick = () => { post({ action: 'stop' }); };

  // ── wiring ──
  $('history-btn').onclick = () => openDrawer('history');
  $('settings-btn').onclick = () => openDrawer('settings');
  $('newchat-btn').onclick = () => { closeDrawers(); if (!busy) post({ action: 'new-chat' }); };
  $('clear-history-btn').onclick = () => post({ action: 'clear-history' });
  $('workspace').onclick = () => { if (!busy) { closeDrawers(); post({ action: 'choose-workspace' }); } };
  $('prompt-open').onclick = () => { closeDrawers(); post({ action: 'show-prompt' }); };
  $('attach-btn').onclick = () => { post({ action: 'attach-files' }); };
  $('model').onchange = (e) => { if (e.target.value) post({ action: 'set-model', value: e.target.value }); };
  $('mode').onchange = (e) => { post({ action: 'set-mode', value: e.target.value }); };
  $('win-min').onclick = (e) => { e.stopPropagation(); post({ action: 'win-min' }); };
  $('win-max').onclick = (e) => { e.stopPropagation(); post({ action: 'win-max' }); };
  $('win-close').onclick = (e) => { e.stopPropagation(); post({ action: 'win-close' }); };

  function bootShow(on) {
    started = !on;
    $('boot').classList.toggle('show', !!on);
  }

  // TEMP debug badge
  function dbg(tag) {
    const hd = historyDrawer;
    const cs = getComputedStyle(hd);
    const pr = document.querySelector('.prose');
    const ml = (window.ML || []).join(' | ');
    $('foot').textContent = 'DBG ' + tag + ' cls=[' + hd.className + '] vis=' + cs.visibility +
      ' proseLen=' + (pr ? pr.textContent.length : -1) +
      ' turns=' + mainEl.childElementCount + ' ML=' + ml;
  }
  window.addEventListener('error', (ev) => {
    window.ML = window.ML || [];
    window.ML.push('ERR:' + String(ev.message).slice(0, 90));
    dbg('jserror');
  });
  window.chrome.webview.addEventListener('message', (e) => {
    const mm = e.data || {};
    if (!mm.type) return;
    window.ML = window.ML || [];
    const len = String(mm.text !== undefined ? mm.text : (mm.output || (mm.files ? mm.files.length + 'files' : '') || '')).length;
    window.ML.push('[' + Math.round(performance.now() / 1000) + 's ' + mm.type + (len ? ' len' + len : '') + ']');
    if (window.ML.length > 6) window.ML.shift();
    if (mm.type === 'state') dbg('state');
  });

  // initial
  dbg('pre-init');
  closeDrawers();          // force clean boot state - drawers never open at startup
  dbg('post-init');
  setTimeout(() => dbg('t500'), 500);
  setTimeout(() => dbg('t1500'), 1500);
  showWelcome();
  requestFileList();
  post({ action: 'ready' });
</script>
</body></html>)CHAT_HTML";

// ── window plumbing ──────────────────────────────────────────────────────────

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_NCCALCSIZE: {
      // Frameless window: the whole window rect is client area (our dark
      // <header> paints the title bar region behind the DWM frame).
      if (wparam) return 0;
      break;
    }
    case WM_NCHITTEST: {
      // Drag the window from the top ~36px (the page's <header> strip), but
      // leave the rightmost controls area (min/max/close) to the page.
      RECT client = {};
      GetClientRect(hwnd, &client);
      const POINT pt = {static_cast<int>(static_cast<short>(lparam & 0xFFFF)),
                        static_cast<int>(static_cast<short>((lparam >> 16) & 0xFFFF))};
      POINT client_pt = pt;
      ScreenToClient(hwnd, &client_pt);
      const bool maximized = IsZoomed(hwnd);
      const int title_h = maximized ? 44 : 36;
      const int controls_w = 110; // min/max/close zone stays clickable
      if (client_pt.y >= 0 && client_pt.y <= title_h &&
          client_pt.x < client.right - controls_w) {
        return HTCAPTION;
      }
      // Resize borders (8px) so the frameless window can still be resized.
      const int edge = 8;
      const bool on_left = client_pt.x <= edge;
      const bool on_right = client_pt.x >= client.right - edge;
      const bool on_top = client_pt.y <= edge;
      const bool on_bottom = client_pt.y >= client.bottom - edge;
      if (on_top && on_left) return HTTOPLEFT;
      if (on_top && on_right) return HTTOPRIGHT;
      if (on_bottom && on_left) return HTBOTTOMLEFT;
      if (on_bottom && on_right) return HTBOTTOMRIGHT;
      if (on_left) return HTLEFT;
      if (on_right) return HTRIGHT;
      if (on_top) return HTTOP;
      if (on_bottom) return HTBOTTOM;
      break;
    }
    case WM_SYSCOMMAND: {
      // Handle minimize/maximize/close from custom titlebar buttons
      const WPARAM w = wparam;
      if (w == 0xF020) { // SC_MINIMIZE
        ShowWindow(hwnd, SW_MINIMIZE);
        return 0;
      }
      if (w == 0xF030) { // SC_MAXIMIZE
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return 0;
      }
      if (w == 0xF060) { // SC_CLOSE
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_SIZE: {
      auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (context != nullptr && context->controller != nullptr) {
        RECT client = {};
        GetClientRect(hwnd, &client);
        context->controller->put_Bounds(client);
      }
      // Tell the page whether we're maximized so it can adjust header padding
      // (Windows clips the top ~8px of a maximized frameless window).
      if (context != nullptr && context->webview != nullptr) {
        const wchar_t* script =
            IsZoomed(hwnd) ? L"document.body.classList.add('maximized');"
                           : L"document.body.classList.remove('maximized');";
        context->webview->ExecuteScript(script, nullptr);
      }
      return 0;
    }
    case WM_APP_DRAIN: {
      auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (context != nullptr) context->drain();
      return 0;
    }
    case WM_CLOSE:
      if (auto* context =
              reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        context->closing = true;
        ui::request_output_stop(); // abort any in-flight stream
        {
          std::lock_guard<std::mutex> lock(context->dialog_mutex);
          context->dialog_ready = true; // unblock a parked permission/ask dialog
          context->dialog_cv.notify_all();
        }
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  return 0;
}

fs::path user_data_dir() {
  const char* local = std::getenv("LOCALAPPDATA");
  fs::path base = (local != nullptr && local[0] != '\0')
                      ? fs::path(local)
                      : fs::temp_directory_path();
  return base / "Gamma" / "webview";
}

bool pump_until(const std::function<bool()>& done, MSG& message) {
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
    if (done()) return true;
  }
  return false;
}

} // namespace

bool webview_launcher_built() { return true; }

struct ChooserState {
  int hover = -1; // button under the cursor (0 = GUI, 1 = CLI)
  int chosen = 0; // 0 none, 1 GUI, 2 CLI
  bool tracking = false;
};

struct ChooserLayout {
  RECT gui;
  RECT cli;
};

ChooserLayout chooser_layout(const RECT& client) {
  const int button_w = 180;
  const int button_h = 78;
  const int gap = 18;
  const int total = button_w * 2 + gap;
  const int x0 = client.left + ((client.right - client.left) - total) / 2;
  const int y0 = client.top + ((client.bottom - client.top) - button_h) / 2 + 34;
  ChooserLayout layout{};
  layout.gui = RECT{x0, y0, x0 + button_w, y0 + button_h};
  layout.cli = RECT{x0 + button_w + gap, y0, x0 + button_w * 2 + gap, y0 + button_h};
  return layout;
}

void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF fill_color,
                     HPEN border_pen) {
  HBRUSH brush = CreateSolidBrush(fill_color);
  const HPEN old_pen = static_cast<HPEN>(SelectObject(dc, border_pen));
  const HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(dc, brush));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
  SelectObject(dc, old_brush);
  SelectObject(dc, old_pen);
  DeleteObject(brush);
}

void draw_centered_text(HDC dc, const RECT& rect, const wchar_t* text, int height,
                        int weight, COLORREF color) {
  HFONT font = CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Segoe UI");
  const HFONT old_font = static_cast<HFONT>(SelectObject(dc, font));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  RECT bounds = rect;
  DrawTextW(dc, text, -1, &bounds,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, old_font);
  DeleteObject(font);
}

LRESULT CALLBACK chooser_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  ChooserState* state = reinterpret_cast<ChooserState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (message) {
    case WM_MOUSEMOVE: {
      if (!state->tracking) {
        // get WM_MOUSELEAVE once the cursor exits the window
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        if (TrackMouseEvent(&track)) state->tracking = true;
      }
      RECT client = {};
      GetClientRect(hwnd, &client);
      const POINT point{static_cast<int>(static_cast<short>(LOWORD(lparam))),
                        static_cast<int>(static_cast<short>(HIWORD(lparam)))};
      const ChooserLayout layout = chooser_layout(client);
      const int hovering = PtInRect(&layout.gui, point)   ? 0
                           : PtInRect(&layout.cli, point) ? 1
                                                          : -1;
      if (hovering != state->hover) {
        state->hover = hovering;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      state->tracking = false;
      if (state->hover != -1) {
        state->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      RECT client = {};
      GetClientRect(hwnd, &client);
      const POINT point{static_cast<int>(static_cast<short>(LOWORD(lparam))),
                        static_cast<int>(static_cast<short>(HIWORD(lparam)))};
      const ChooserLayout layout = chooser_layout(client);
      if (PtInRect(&layout.gui, point)) state->chosen = 1;
      if (PtInRect(&layout.cli, point)) state->chosen = 2;
      if (state->chosen != 0) DestroyWindow(hwnd);
      return 0;
    }
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) { state->chosen = 1; DestroyWindow(hwnd); }
      if (wparam == VK_ESCAPE) { state->chosen = 2; DestroyWindow(hwnd); }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT paint = {};
      HDC dc = BeginPaint(hwnd, &paint);
      RECT client = {};
      GetClientRect(hwnd, &client);
      // canvas
      const HBRUSH bg = CreateSolidBrush(RGB(19, 19, 20));
      FillRect(dc, &client, bg);
      DeleteObject(bg);
      // brand
      draw_centered_text(dc, RECT{client.left, client.top + 44, client.right,
                                  client.top + 118},
                         L"\x3B3", 64, 700, RGB(168, 199, 250));
      draw_centered_text(dc, RECT{client.left, client.top + 116, client.right,
                                  client.top + 148},
                         L"Gamma Code", 17, 600, RGB(227, 226, 230));
      draw_centered_text(dc, RECT{client.left, client.top + 152, client.right,
                                  client.top + 178},
                         L"Open Gamma Code in:", 12, 400, RGB(142, 145, 143));
      // buttons
      const ChooserLayout layout = chooser_layout(client);
      HPEN gui_pen =
          CreatePen(PS_SOLID, 1, state->hover == 0 ? RGB(168, 199, 250)
                                                   : RGB(55, 57, 59));
      fill_round_rect(dc, layout.gui, 14,
                      state->hover == 0 ? RGB(43, 49, 66) : RGB(30, 30, 31), gui_pen);
      DeleteObject(gui_pen);
      draw_centered_text(dc, RECT{layout.gui.left, layout.gui.top + 14, layout.gui.right,
                                  layout.gui.top + 44},
                         L"GUI", 20, 700, RGB(227, 226, 230));
      draw_centered_text(dc, RECT{layout.gui.left, layout.gui.top + 44, layout.gui.right,
                                  layout.gui.bottom - 10},
                         L"native chat window", 11, 400, RGB(142, 145, 143));
      HPEN cli_pen = CreatePen(PS_SOLID, 1, state->hover == 1 ? RGB(96, 205, 181)
                                                              : RGB(55, 57, 59));
      fill_round_rect(dc, layout.cli, 14,
                      state->hover == 1 ? RGB(43, 56, 56) : RGB(30, 30, 31), cli_pen);
      DeleteObject(cli_pen);
      draw_centered_text(dc, RECT{layout.cli.left, layout.cli.top + 14, layout.cli.right,
                                  layout.cli.top + 44},
                         L"CLI", 20, 700, RGB(227, 226, 230));
      draw_centered_text(dc, RECT{layout.cli.left, layout.cli.top + 44, layout.cli.right,
                                  layout.cli.bottom - 10},
                         L"terminal", 11, 400, RGB(142, 145, 143));
      draw_centered_text(dc, RECT{client.left, client.bottom - 40, client.right, client.bottom},
                         L"local-only \x00B7 Ollama on 127.0.0.1", 10, 400, RGB(101, 104, 102));
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1; // WM_PAINT fills everything; no flicker
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

// Returns 0 (closed), 1 (GUI) or 2 (CLI).

ChatResult run_chat_app(Options& options, const StartupHook& startup) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return ChatResult::Unavailable;

  const WebView2Api api = load_webview2_api();
  if (!api.ok()) {
    MessageBoxW(nullptr, api.problem.c_str(), L"Gamma Code â€” native chat",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    CoUninitialize();
    return ChatResult::Unavailable;
  }

  OllamaClient client(options.base_url);
  const Registry registry = tools::default_registry();

  Context context;
  context.options = &options;
  context.client = &client;
  context.registry = &registry;

  // The permission gate parks a dialog in the window and waits for the
  // user's Allow/Deny; the ask tool uses the same bridge.
  AgentLoop loop(options, client, registry,
                 [&context](const Tool& tool, const nlohmann::json& arguments) {
                   std::string summary = tool.name + "(";
                   bool first = true;
                   for (auto it = arguments.begin(); it != arguments.end(); ++it) {
                     if (!first) summary += ", ";
                     first = false;
                     std::string value = it.value().is_string()
                                             ? it.value().get<std::string>()
                                             : it.value().dump();
                     if (value.size() > 300) value = value.substr(0, 297) + "â€¦";
                     summary += it.key() + ": " + value;
                   }
                   summary += ")";
                   std::string answer;
                   return context.show_dialog(
                       {{"type", "confirm"},
                        {"id", context.next_dialog_id.load()},
                        {"text", "gamma wants to run " + summary}},
                       answer);
                 });
  context.loop = &loop;

  // The ask tool asks in the window too.
  ui::set_question_handler([&context](const std::string& question,
                                      const std::vector<std::string>& ask_options) {
    std::string answer;
    nlohmann::json payload = {{"type", "ask"},
                              {"id", context.next_dialog_id.load()},
                              {"question", question}};
    if (!ask_options.empty()) payload["options"] = ask_options;
    const bool answered = context.show_dialog(payload, answer);
    return UserAnswer{answered, answer};
  });

  // Window setup
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class = {};
  window_class.lpfnWndProc = &window_proc;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"GammaCodeChatApp";
  RegisterClassW(&window_class);

  RECT work = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int width = 1180;
  const int height = 820;
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;
  const DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_SYSMENU);
  context.hwnd = CreateWindowExW(0, window_class.lpszClassName, L"Gamma Code",
                                 style, x, y, width, height, nullptr,
                                 nullptr, instance, nullptr);
  if (context.hwnd == nullptr) {
    ui::set_question_handler(nullptr);
    CoUninitialize();
    return ChatResult::Unavailable;
  }
  SetWindowLongPtrW(context.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));

  // Extend the client area into the title bar region so the WebView content
  // (our dark <header>) paints behind the system frame.
  MARGINS margins = {-1, -1, -1, -1};
  DwmExtendFrameIntoClientArea(context.hwnd, &margins);

  ShowWindow(context.hwnd, SW_SHOW);

  // auto-resume the newest saved session for this workspace? mint a fresh one
  context.options->session_file = sessions::new_session_path(options);
  context.session_file = context.options->session_file;

  // WebView bring-up
  std::error_code error;
  fs::create_directories(user_data_dir(), error);

  EnvironmentHandler* environment_handler = new EnvironmentHandler(context);
  const HRESULT environment_created =
      api.create_environment(nullptr, (user_data_dir() / "chat").wstring().c_str(),
                             nullptr, environment_handler);
  MSG message;
  if (FAILED(environment_created) ||
      !pump_until([environment_handler] { return environment_handler->done(); }, message)) {
    environment_handler->Release();
    ui::set_question_handler(nullptr);
    CoUninitialize();
    return ChatResult::Unavailable;
  }
  if (FAILED(environment_handler->result())) {
    environment_handler->Release();
    ui::set_question_handler(nullptr);
    CoUninitialize();
    return ChatResult::Unavailable;
  }
  ICoreWebView2Environment* environment = environment_handler->detach();
  environment_handler->Release();

  ControllerHandler* controller_handler = new ControllerHandler(context);
  environment->CreateCoreWebView2Controller(context.hwnd, controller_handler);
  const bool controller_ready =
      pump_until([controller_handler] { return controller_handler->done(); }, message);
  if (!controller_ready || FAILED(controller_handler->result())) {
    controller_handler->Release();
    environment->Release();
    ui::set_question_handler(nullptr);
    CoUninitialize();
    return ChatResult::Unavailable;
  }
  context.controller = controller_handler->detach();
  controller_handler->Release();

  HRESULT got_webview = context.controller->get_CoreWebView2(&context.webview);
  EventRegistrationToken token{};
  if (SUCCEEDED(got_webview) && context.webview != nullptr) {
    RECT client_rect = {};
    GetClientRect(context.hwnd, &client_rect);
    context.controller->put_Bounds(client_rect);

    MessageHandler* message_handler = new MessageHandler(context);
    context.webview->add_WebMessageReceived(message_handler, &token);
    message_handler->Release(); // the webview holds its own reference

    context.webview->NavigateToString(utf8_to_wide(kChatHtml).c_str());
    // The chooser picked GUI moments ago — the model list is still empty and
    // Ollama may not even be up yet. The bootstrap runs on its own thread
    // while the page shows "opening Ollama…" in its own status line; the
    // completed state (with models + auto-picked model) replaces it.
    context.starting = true;
    // Ollama lifecycle: check if server already running BEFORE bootstrap
    const DWORD ollama_pid_before = find_ollama_pid();
    context.owns_ollama = (ollama_pid_before == 0);
    context.send_state();
    context.startup_worker = std::thread([&context, &options, startup]() {
      std::vector<std::string> models;
      const std::string problem = startup ? startup(options, models) : std::string();
      {
        const std::lock_guard<std::mutex> lock(context.models_mutex);
        context.models = std::move(models);
      }
      context.starting = false;
      // If we didn't own Ollama before, get the PID from the bootstrap (more reliable than scanning)
      if (context.owns_ollama) {
        context.ollama_pid = gamma::setup::get_started_ollama_pid();
      }
      if (!problem.empty()) {
        context.post({{"type", "notice"}, {"text", problem}, {"error", true}});
      }
      context.send_state();
    });
  }

  // The click loop
  if (SUCCEEDED(got_webview) && context.webview != nullptr) {
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
      if (context.closing.load()) break;
    }
  }

  // Teardown: stop any turn, unblock dialogs, join the workers
  context.closing = true;
  ui::request_output_stop();
  if (context.startup_worker.joinable()) context.startup_worker.join();
  {
    std::lock_guard<std::mutex> lock(context.dialog_mutex);
    context.dialog_ready = true;
    context.dialog_cv.notify_all();
  }
  if (context.worker.joinable()) context.worker.join();
  // Ollama lifecycle: if we started it, kill it (and its children)
  if (context.owns_ollama && context.ollama_pid != 0) {
    kill_process_tree(context.ollama_pid);
    // Final sweep: kill any remaining ollama.exe processes
    kill_all_ollama();
  }
  if (context.webview != nullptr) context.webview->remove_WebMessageReceived(token);

  DestroyWindow(context.hwnd);
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    if (message.message == WM_QUIT) break;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  if (context.webview != nullptr) {
    context.webview->Release();
    context.webview = nullptr;
  }
  if (context.controller != nullptr) {
    context.controller->Release();
    context.controller = nullptr;
  }
  environment->Release();
  ui::set_question_handler(nullptr);
  CoUninitialize();
  return ChatResult::Done;
}int run_chooser() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW window_class = {};
  window_class.lpfnWndProc = &chooser_proc;
  window_class.hInstance = instance;
  window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"GammaCodeChooser";
  RegisterClassW(&window_class);

  RECT work = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int width = 520;
  const int height = 400;
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;
  HWND hwnd = CreateWindowExW(
      0, window_class.lpszClassName, L"Gamma Code",
      WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), x, y, width, height,
      nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) return 0;
  ChooserState state;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
  ShowWindow(hwnd, SW_SHOW);

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return state.chosen;
}

AppResult run_app(Options& options, const StartupHook& startup) {
  // Single-window app: straight to the chat UI (no chooser, no CLI fallback).
  const ChatResult chat = run_chat_app(options, startup);
  return chat == ChatResult::Done ? AppResult::Done : AppResult::Exit;
}

} // namespace gamma::ui

#else // !GAMMA_HAS_WEBVIEW

namespace gamma::ui {

bool webview_launcher_built() { return false; }

int run_chooser() { return 0; }

ChatResult run_chat_app(Options&, const StartupHook&) {
  return ChatResult::Unavailable;
}

AppResult run_app(Options&, const StartupHook&) {
  return AppResult::Unavailable;
}

} // namespace gamma::ui

#endif // GAMMA_HAS_WEBVIEW
