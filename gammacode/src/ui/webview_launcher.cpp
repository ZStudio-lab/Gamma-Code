#include "ui/webview_launcher.hpp"

#include "ui/render.hpp"
#include "agent/loop.hpp"
#include "config/sessions.hpp"
#include "tools/registry.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
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
    post({{"type", "state"},
          {"model", options->model},
          {"models", models},
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
      if (message.role != "user" && message.role != "assistant") continue;
      if (message.role == "user" &&
          message.content.starts_with("[gamma] the previous Ollama call failed"))
        continue; // internal corrective note, not a real user bubble
      messages.push_back({{"role", message.role}, {"content", message.content}});
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
  void on_tool_result(const std::string& name, bool ok) override {
    context_.post({{"type", "tool-result"}, {"name", name}, {"ok", ok}});
  }
  void on_final(const std::string& content) override {
    context_.post({{"type", "final"}, {"text", content}});
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
      }
      context.send_state();
    } else if (action == "show-prompt") {
      context.post({{"type", "prompt"}, {"text", system_prompt_text()}});
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
  :root { --bg:#131314; --surface:#1e1e1f; --input:#282a2c; --hover:#2f3133;
          --line:#37393b; --accent:#a8c7fa; --text:#e3e2e6; --muted:#8e918f;
          --green:#34d399; --red:#ef4444; --code:#0d0e0f; }
  * { box-sizing:border-box; }
  html,body { height:100%; }
  body { margin:0; background-color:var(--bg); color:var(--text); display:flex;
         flex-direction:column; overflow:hidden; font:14px/1.55 Inter,"Segoe UI Variable Text","Segoe UI",system-ui,sans-serif; }
  code,pre,.mono { font-family:"Fira Code",Consolas,monospace; }
  ::-webkit-scrollbar { width:5px; height:5px; }
  ::-webkit-scrollbar-track { background:transparent; }
  ::-webkit-scrollbar-thumb { background:rgba(255,255,255,.15); border-radius:9999px; }
  ::-webkit-scrollbar-thumb:hover { background:rgba(255,255,255,.3); }
  svg { display:block; }

  /* ── header ─────────────────────────────────────────────────────────── */
  header { height:56px; padding:0 14px; display:flex; align-items:center; gap:8px;
           border-bottom:1px solid rgba(55,57,59,.35); background:rgba(19,19,20,.85); flex:none; z-index:20; }
  .iconbtn { padding:8px; border:0; background:transparent; color:var(--muted); border-radius:999px;
             cursor:pointer; transition:background .15s,color .15s; display:flex; }
  .iconbtn:hover { background:var(--hover); color:var(--text); }
  .spacer { flex:1; }
  .ver { color:var(--muted); font-size:10.5px; margin-right:4px; }

  /* ── drawers ────────────────────────────────────────────────────────── */
  .drawer { position:fixed; top:0; bottom:0; width:288px; background:var(--surface); z-index:40;
            display:flex; flex-direction:column; box-shadow:0 0 40px rgba(0,0,0,.5);
            transition:transform .3s ease-in-out; }
  .drawer.left { left:0; border-right:1px solid var(--line); transform:translateX(-100%); }
  .drawer.right { right:0; border-left:1px solid var(--line); transform:translateX(100%); }
  .drawer.open { transform:translateX(0); }
  .dhead { padding:14px 16px; border-bottom:1px solid var(--line); display:flex;
           justify-content:space-between; align-items:center; font-size:13.5px; font-weight:600;
           letter-spacing:.02em; }
  .dhead .x { border:0; background:transparent; color:var(--muted); cursor:pointer; padding:4px; }
  .dhead .x:hover { color:var(--text); }
  #backdrop { position:fixed; inset:0; background:rgba(0,0,0,.5); z-index:30; display:none; }
  #backdrop.open { display:block; }
  #newchat-btn { margin:12px 12px 6px; background:var(--accent); color:#131314; border:0;
                 border-radius:12px; padding:10px; font:600 13px inherit; cursor:pointer; }
  #newchat-btn:hover { filter:brightness(1.05); }
  .hrow { display:flex; align-items:center; gap:9px; margin:2px 8px; padding:10px 12px;
          border-radius:12px; cursor:pointer; font-size:12.5px; color:var(--muted); }
  .hrow:hover { background:rgba(47,49,51,.6); color:var(--text); }
  .hrow.on { background:var(--hover); color:var(--text); font-weight:500; }
  .hrow .t { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:1; }
  .hempty { color:var(--muted); font-size:12px; padding:12px 16px; }

  /* ── messages ───────────────────────────────────────────────────────── */
  main { flex:1; overflow-y:auto; }
  .canvas { max-width:768px; margin:0 auto; padding:26px 20px 20px; display:flex;
            flex-direction:column; gap:26px; }
  .mrow { display:flex; flex-direction:column; }
  .mrow.user { align-items:flex-end; }
  .mrow.assistant { align-items:flex-start; }
  .msg-content { width:100%; font-size:14px; line-height:1.65; }
  .mrow.user .msg-content { width:auto; max-width:85%; background:var(--surface);
                            border:1px solid var(--line); border-radius:16px;
                            padding:10px 16px; white-space:pre-wrap; }
  .streaming-cursor::after { content:'●'; display:inline-block; margin-left:4px;
                             color:var(--accent); animation:gpulse 1s infinite ease-in-out; }
  @keyframes gpulse { 0%,100% { opacity:.2; } 50% { opacity:1; } }
  .msg-content p { margin:0; white-space:pre-wrap; }
  .msg-content code.ic { background:var(--input); border-radius:5px; padding:1px 5px;
                         font-size:12.5px; font-family:"Fira Code",Consolas,monospace; }
  .msg-content b { font-weight:600; }
  .code-card { position:relative; margin:12px 0; border:1px solid var(--line);
               background:var(--code); border-radius:12px; overflow:hidden; }
  .code-head { display:flex; justify-content:space-between; align-items:center; padding:6px 12px;
               background:var(--surface); font-size:11px; color:var(--muted);
               border-bottom:1px solid var(--line); }
  .code-head .lang { text-transform:uppercase; letter-spacing:.08em; }
  .code-head .copy { display:flex; align-items:center; gap:5px; border:0; background:transparent;
                     color:var(--muted); font:11px inherit; cursor:pointer; padding:2px 4px;
                     border-radius:6px; }
  .code-head .copy:hover { color:var(--text); background:var(--hover); }
  .code-card pre { margin:0; padding:14px; overflow-x:auto; font-size:12.5px; line-height:1.6;
                   white-space:pre; color:var(--text); }
  .toolstep { display:flex; align-items:baseline; gap:8px; font-size:11.5px; color:var(--muted);
              padding:0 2px; }
  .toolstep .tname { color:var(--accent); font-weight:600; white-space:nowrap; }
  .toolstep .tsum { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; opacity:.8; }
  .toolstep .mark { margin-left:auto; font-size:10.5px; white-space:nowrap; }
  .toolstep .mark.ok { color:var(--green); }
  .toolstep .mark.fail { color:var(--red); }
  .notice { font-size:12px; color:var(--muted); text-align:center; padding:2px 40px; }
  .notice.err { color:var(--red); }

  /* ── welcome / empty state ──────────────────────────────────────────── */
  #welcome { height:100%; display:flex; flex-direction:column; align-items:center;
             justify-content:center; text-align:center; padding:12px; }
  #welcome .orb { width:48px; height:48px; border-radius:999px; background:var(--surface);
                  border:1px solid rgba(55,57,59,.8); display:flex; align-items:center;
                  justify-content:center; color:var(--accent); margin-bottom:16px;
                  box-shadow:inset 0 2px 8px rgba(0,0,0,.4); }
  #welcome h1 { font-size:22px; font-weight:500; letter-spacing:-.01em; margin:0 0 8px; }
  #welcome p.sub { font-size:12px; color:var(--muted); max-width:340px; margin:0 0 28px; }
  .suggest { display:grid; grid-template-columns:1fr 1fr; gap:10px; width:100%; max-width:430px; }
  .suggest button { padding:12px 14px; background:var(--surface); border:1px solid rgba(55,57,59,.6);
                    border-radius:12px; color:var(--text); font-size:12px; text-align:left;
                    cursor:pointer; display:flex; justify-content:space-between; gap:8px;
                    align-items:center; transition:background .15s; }
  .suggest button:hover { background:var(--hover); }
  .suggest button svg { color:var(--muted); flex:none; }
  .suggest button:hover svg { color:var(--accent); }

  /* ── floating scroll-to-bottom ──────────────────────────────────────── */
  #scroll-btn { position:absolute; bottom:152px; right:50%; transform:translateX(50%); z-index:20;
                display:none; }
  #scroll-btn.show { display:block; }
  #scroll-btn button { display:flex; align-items:center; gap:7px; padding:7px 14px;
                       background:var(--surface); color:var(--text); border:1px solid var(--line);
                       border-radius:999px; font-size:12px; font-weight:500; cursor:pointer;
                       box-shadow:0 8px 24px rgba(0,0,0,.5); }
  #scroll-btn button:hover { background:var(--hover); }

  /* ── composer ───────────────────────────────────────────────────────── */
  .composebar { flex:none; padding:12px 16px 10px; }
  .composer { max-width:768px; margin:0 auto; position:relative; background:var(--surface);
              border:1px solid rgba(55,57,59,.8); border-radius:24px;
              box-shadow:0 10px 34px rgba(0,0,0,.45); display:flex; flex-direction:column;
              align-items:stretch; padding:10px 14px 8px; transition:border-color .15s; }
  .composer:focus-within { border-color:var(--muted); }
  textarea { width:100%; resize:none; background:transparent; color:var(--text); border:0;
             outline:none; font:13.5px/1.5 inherit; max-height:160px; padding:2px 2px 4px;
             placeholder-color:var(--muted); }
  textarea::placeholder { color:var(--muted); }
  .composer-bottom { display:flex; align-items:center; gap:6px; min-height:34px; }
  .composer-bottom .pills { margin-left:0; }
  .composer-bottom .spacer { flex:1; }
  .sendbar { display:flex; gap:0; }
  button.circle { width:34px; height:34px; border-radius:999px; border:0; background:var(--accent);
                  color:#131314; cursor:pointer; display:flex; align-items:center;
                  justify-content:center; transition:transform .1s,filter .15s; }
  button.circle:hover { filter:brightness(1.08); }
  button.circle:active { transform:scale(.94); }
  button.circle.stop { background:var(--red); color:#fff; }
  /* chips: model + modes live at the bottom, next to the send button */
  .selwrap { position:relative; }
  select#model { appearance:none; background:transparent; color:var(--muted); border:1px solid rgba(55,57,59,.6);
                 font-weight:500; font-size:11.5px; border-radius:999px; padding:5px 24px 5px 11px;
                 max-width:210px; cursor:pointer; transition:border-color .15s,color .15s; outline:none; }
  select#model:hover { border-color:var(--muted); color:var(--text); }
  .selwrap::after { content:'\25BE'; position:absolute; right:8px; top:50%; transform:translateY(-50%);
                    color:var(--muted); font-size:9px; pointer-events:none; }
  .pills { display:flex; gap:2px; }
  .pill { border:0; background:transparent; color:var(--muted); font:11.5px/1 inherit;
          padding:7px 10px; border-radius:999px; cursor:pointer; }
  .pill:hover { color:var(--text); }
  .pill.on { background:var(--input); color:var(--accent); font-weight:600; }
  .hidden { display:none !important; }
  .iconbtn.sm { padding:6px; }
  .caption { max-width:768px; margin:8px auto 0; text-align:center; font-size:10.5px;
             color:var(--muted); font-family:"Fira Code",Consolas,monospace; min-height:14px; }

  /* ── dialogs ────────────────────────────────────────────────────────── */
  dialog { background:var(--surface); color:var(--text); border:1px solid var(--line);
           border-radius:16px; padding:20px 22px; width:min(560px, 88vw); }
  dialog::backdrop { background:rgba(0,0,0,.6); }
  dialog .dlabel { font-size:11px; letter-spacing:2px; text-transform:uppercase;
                   color:var(--accent); margin-bottom:8px; }
  dialog pre.plain { white-space:pre-wrap; font:12.5px/1.55 "Fira Code",Consolas,monospace;
                     background:var(--bg); border:1px solid var(--line); border-radius:10px;
                     padding:12px; max-height:40vh; overflow:auto; margin:0 0 14px; }
  dialog .drow { display:flex; gap:10px; justify-content:flex-end; }
  dialog .drow button { background:var(--accent); color:#131314; border:0; border-radius:10px;
                        padding:9px 18px; font:600 13px inherit; cursor:pointer; }
  dialog .drow button.ghost { background:transparent; color:var(--muted); border:1px solid var(--line); }
  dialog .drow button.ghost:hover { color:var(--text); }
  dialog .drow button.left { margin-right:auto; }
  .optrow { display:flex; flex-wrap:wrap; gap:8px; margin-bottom:10px; }
  .optrow button { background:var(--input); color:var(--text); border:1px solid var(--line);
                   border-radius:999px; padding:7px 14px; font:12.5px inherit; cursor:pointer; }
  .optrow button:hover { border-color:var(--accent); }
  dialog input[type=text] { width:100%; background:var(--input); color:var(--text);
        border:1px solid var(--line); border-radius:10px; padding:9px 12px; font:13px inherit;
        outline:none; margin-bottom:12px; }
  dialog input[type=text]:focus { border-color:var(--accent); }
</style></head>
<body>
  <header>
    <button class="iconbtn" id="history-btn" title="Chat History">
      <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="3.5" y="4" width="17" height="16" rx="2.5"/><path d="M9.5 4v16"/></svg>
    </button>
    <span style="font-size:13px; font-weight:600; color:var(--text); letter-spacing:.01em;">gamma</span>
    <span class="spacer"></span>
    <span class="ver" id="ver"></span>
    <button class="iconbtn" id="newchat-btn" title="New Chat">
      <svg width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M12 5v14M5 12h14"/></svg>
    </button>
    <button class="iconbtn" id="settings-btn" title="Workspace &amp; info">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 8h10M18 8h2M4 16h2M10 16h10"/><circle cx="16" cy="8" r="2"/><circle cx="8" cy="16" r="2"/></svg>
    </button>
  </header>

  <div class="drawer left" id="history-drawer">
    <div class="dhead"><span>History</span>
      <button class="x" onclick="closeDrawers()"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg></button>
    </div>
    <button id="drawer-newchat">&#43; New chat</button>
    <div id="history-list" style="flex:1; overflow-y:auto; padding:2px 0 12px;"></div>
  </div>

  <div class="drawer right" id="settings-drawer">
    <div class="dhead"><span>Session</span>
      <button class="x" onclick="closeDrawers()"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M6 6l12 12M18 6L6 18"/></svg></button>
    </div>
    <div style="padding:16px; display:flex; flex-direction:column; gap:16px; overflow-y:auto;">
      <div>
        <div style="font-size:10.5px; color:var(--muted); text-transform:uppercase; letter-spacing:.12em; margin-bottom:7px;">Workspace</div>
        <div class="mono" id="workspace" style="font-size:11.5px; background:var(--input); border:1px solid var(--line); border-radius:10px; padding:9px 12px; word-break:break-all; color:var(--text); cursor:pointer;" title="click to choose another folder"></div>
        <div style="font-size:10.5px; color:var(--muted); margin-top:6px;">click to change &middot; every tool path stays inside it</div>
      </div>
      <div>
        <div style="font-size:10.5px; color:var(--muted); text-transform:uppercase; letter-spacing:.12em; margin-bottom:7px;">Permission mode</div>
        <div id="mode-list" style="display:flex; flex-direction:column; gap:6px;"></div>
      </div>
      <div>
        <div style="font-size:10.5px; color:var(--muted); text-transform:uppercase; letter-spacing:.12em; margin-bottom:7px;">System prompt</div>
        <div class="hrow" id="prompt-open" style="margin:0; background:var(--input);" title="show the system prompt gamma runs on">
          <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="flex:none;"><path d="M8 9l-4 3 4 3M16 9l4 3-4 3"/></svg>
          <span class="t" style="font-size:11.5px;">view what gamma is told</span>
        </div>
      </div>
      <div style="font-size:11px; color:var(--muted); line-height:1.6;">gamma is fully local: model traffic never leaves <span class="mono" style="color:var(--text);">127.0.0.1</span>.</div>
    </div>
  </div>

  <div id="backdrop" onclick="closeDrawers()"></div>

  <main id="chat">
    <div class="canvas" id="chat-col">
      <div id="welcome">
        <div class="orb">
          <svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor"><path d="M12 2l1.9 5.6L20 9.5l-5.6 1.9L12 17l-2.4-5.6L4 9.5l5.6-1.9L12 2z"/></svg>
        </div>
        <h1>Hello, I'm gamma.</h1>
        <p class="sub">Your local coding agent &mdash; reads, edits and runs on your machine, nothing leaves it.</p>
        <div class="suggest">
          <button onclick="setPrompt('Explain what this project does and how it is structured.')"><span>"Explain this project"</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg></button>
          <button onclick="setPrompt('Find the biggest bug in this codebase and fix it.')"><span>"Find and fix a bug"</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg></button>
          <button onclick="setPrompt('Review my latest changes and suggest improvements.')"><span>"Review my changes"</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg></button>
          <button onclick="setPrompt('Add a README section that documents the tools gamma can use.')"><span>"Write a README section"</span><svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg></button>
        </div>
      </div>
    </div>
    <div id="scroll-btn">
      <button onclick="forceScrollToBottom()">
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" style="color:var(--accent);"><path d="M12 5v14M6 13l6 6 6-6"/></svg>
        <span>Scroll to bottom</span>
      </button>
    </div>
  </main>

  <div class="composebar">
    <div class="composer">
      <textarea id="input" rows="1" placeholder="Ask anything..."></textarea>
      <div class="composer-bottom">
        <div class="selwrap"><select id="model" title="model"></select></div>
        <button class="iconbtn sm" id="refresh-btn" title="Refresh models">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 5v6h-6"/></svg>
        </button>
        <div class="pills" id="modes"></div>
        <span class="spacer"></span>
        <div class="sendbar">
          <button class="circle hidden" id="stop-btn" title="Stop">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="currentColor"><rect x="5" y="5" width="14" height="14" rx="2"/></svg>
          </button>
          <button class="circle" id="send-btn" title="Send (Enter)">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 19V5M6 11l6-6 6 6"/></svg>
          </button>
        </div>
      </div>
    </div>
    <div class="caption" id="status">Ollama local instance &middot; local privacy preserved</div>
  </div>

  <dialog id="prompt-dialog">
    <div class="dlabel">system prompt</div>
    <pre class="plain" id="prompt-text" style="max-height:55vh;"></pre>
    <div class="drow" style="justify-content:flex-end;">
      <button class="ghost" onclick="document.getElementById('prompt-dialog').close()">Close</button>
    </div>
  </dialog>

  <dialog id="confirm-dialog">
    <div class="dlabel" id="confirm-name">permission</div>
    <pre class="plain" id="confirm-text"></pre>
    <div class="drow">
      <button class="ghost left" id="confirm-no">Deny</button>
      <button id="confirm-yes">Allow</button>
    </div>
  </dialog>

  <dialog id="ask-dialog">
    <div class="dlabel">gamma asks</div>
    <pre class="plain" id="ask-question"></pre>
    <div class="optrow" id="ask-options"></div>
    <input type="text" id="ask-input" placeholder="or type your own answer...">
    <div class="drow">
      <button class="ghost left" id="ask-skip">Skip</button>
      <button id="ask-send">Answer &#9656;</button>
    </div>
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
  const statusEl = $('status');
  const welcome = $('welcome');
  const historyDrawer = $('history-drawer');
  const settingsDrawer = $('settings-drawer');
  const backdrop = $('backdrop');
  const historyList = $('history-list');

  const IDLE_CAPTION = 'Ollama local instance \u00B7 local privacy preserved';

  let busy = false;
  let streamEl = null, streamBuf = '';
  let lastTool = null;
  let pendingConfirm = null, pendingAsk = null;
  let isUserScrolledUp = false;
  let currentModel = '';
  let lastSessions = [], lastActive = '';

  // ── tiny markdown: fence cards, `code`, **bold** (inputs escaped first) ──
  function fmt(text) {
    const parts = String(text).split(/```[a-zA-Z0-9_+\-#]*\n?/);
    let out = '';
    for (let i = 0; i < parts.length; i++) {
      if (i % 2 === 1) {
        const raw = parts[i].replace(/\n$/, '');
        out += '<div class="code-card"><div class="code-head"><span class="lang">code</span>' +
               '<button class="copy" onclick="copyCode(this)">' +
               '<svg width="11" height="11" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></svg>' +
               '<span style="display:none">' + esc(raw) + '</span>Copy</button></div>' +
               '<pre><code>' + esc(raw) + '</code></pre></div>';
      } else if (parts[i] !== '') {
        let p = esc(parts[i]);
        p = p.replace(/`([^`\n]+)`/g, '<code class="ic">$1</code>');
        p = p.replace(/\*\*([^*\n]+)\*\*/g, '<b>$1</b>');
        out += '<div class="prose" style="white-space:pre-wrap">' + p + '</div>';
      }
    }
    return out;
  }

  function copyCode(btn) {
    const hidden = btn.querySelector('span');
    const text = hidden ? hidden.textContent : '';
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).catch(function () {});
    } else {
      const ta = document.createElement('textarea');
      ta.value = text; document.body.appendChild(ta); ta.select();
      document.execCommand('copy'); document.body.removeChild(ta);
    }
    const label = btn.lastChild;
    const prev = btn.childNodes[btn.childNodes.length - 1];
    prev.textContent = ' Copied';
    setTimeout(function () { prev.textContent = ' Copy'; }, 1800);
  }

  function scrollDown() {
    if (!isUserScrolledUp) mainEl.scrollTop = mainEl.scrollHeight;
    updateScrollBtn();
  }
  function handleScroll() {
    const c = mainEl;
    isUserScrolledUp = c.scrollHeight - c.scrollTop - c.clientHeight > 60;
    updateScrollBtn();
  }
  function updateScrollBtn() {
    $('scroll-btn').classList.toggle('show', isUserScrolledUp);
  }
  function forceScrollToBottom() {
    isUserScrolledUp = false;
    mainEl.scrollTop = mainEl.scrollHeight;
    updateScrollBtn();
  }
  mainEl.addEventListener('scroll', handleScroll);

  function addUser(text) {
    const row = document.createElement('div');
    row.className = 'mrow user';
    const b = document.createElement('div');
    b.className = 'msg-content';
    b.textContent = text;
    row.appendChild(b);
    chatCol.appendChild(row);
    scrollDown();
  }

  function addNotice(text, err) {
    const n = document.createElement('div');
    n.className = 'notice' + (err ? ' err' : '');
    n.textContent = text;
    chatCol.appendChild(n);
    scrollDown();
  }

  function addToolStep(name, summary) {
    const s = document.createElement('div');
    s.className = 'toolstep';
    s.innerHTML = '<span class="tname">&#9656; ' + esc(name) + '</span>' +
                  (summary ? '<span class="tsum">' + esc(summary) + '</span>' : '');
    chatCol.appendChild(s);
    scrollDown();
    return s;
  }

  function setStatus(label) {
    statusEl.textContent = label ? label : IDLE_CAPTION;
  }

  function renderAssistant(text) {
    const row = document.createElement('div');
    row.className = 'mrow assistant';
    const b = document.createElement('div');
    b.className = 'msg-content';
    b.innerHTML = fmt(text);
    row.appendChild(b);
    chatCol.appendChild(row);
  }

  function ensureStream() {
    if (streamEl !== null) return;
    const row = document.createElement('div');
    row.className = 'mrow assistant';
    const b = document.createElement('div');
    b.className = 'msg-content streaming-cursor';
    row.appendChild(b);
    chatCol.appendChild(row);
    streamEl = b;
    streamBuf = '';
  }

  function endStream() {
    if (streamEl !== null) streamEl.classList.remove('streaming-cursor');
    streamEl = null; streamBuf = '';
  }

  function setBusy(on) {
    busy = on;
    sendBtn.classList.toggle('hidden', on);
    stopBtn.classList.toggle('hidden', !on);
  }

  function hideWelcome() { if (welcome.parentNode) welcome.remove(); }
  function showWelcome() {
    if (!welcome.parentNode) { chatCol.appendChild(welcome); }
    welcome.style.display = 'flex';
  }

  function renderTranscript(messages) {
    chatCol.innerHTML = '';
    endStream();
    lastTool = null;
    if (!messages.length) { chatCol.appendChild(welcome); setStatus(''); scrollDown(); return; }
    hideWelcome();
    for (const m of messages) {
      if (m.role === 'user') { addUser(m.text); continue; }
      if (m.role === 'tool') {
        const s = document.createElement('div');
        s.className = 'toolstep';
        s.innerHTML = '<span class="mark ' + (m.ok ? 'ok">&#10003; done' : 'fail">&#10007; failed') +
                      '</span>' + (m.text ? '<span class="tsum">' + esc(m.text) + '</span>' : '');
        chatCol.appendChild(s);
        continue;
      }
      for (const t of (m.tools || [])) addToolStep(t, '');
      if (m.text) renderAssistant(m.text);
    }
    setStatus('');
    scrollDown();
  }

  function renderModes(active) {
    const pills = $('modes');
    pills.innerHTML = '';
    const list = $('mode-list');
    list.innerHTML = '';
    const info = {
      plan: ['read-only', 'nothing is written or executed'],
      ask:  ['ask first', 'reads free; writes &amp; commands ask'],
      auto: ['hands-free', 'reads, writes, edits, commands at once'],
    };
    for (const p of ['plan', 'ask', 'auto']) {
      const b = document.createElement('button');
      b.className = 'pill' + (p === active ? ' on' : '');
      b.textContent = p;
      b.onclick = () => post({action:'set-mode', value:p});
      pills.appendChild(b);
      const row = document.createElement('button');
      row.className = 'hrow' + (p === active ? ' on' : '');
      row.style.margin = '0';
      row.innerHTML = '<span style="font-weight:600; color:' + (p === active ? 'var(--accent)' : 'var(--text)') +
                      '; text-transform:uppercase; font-size:11px;">' + p + '</span>' +
                      '<span class="t" style="font-size:11.5px;">' + info[p][0] + ' &middot; ' + info[p][1] + '</span>';
      row.onclick = () => { post({action:'set-mode', value:p}); };
      list.appendChild(row);
    }
  }

  function oneLineArgs(argsJson) {
    let summary = '';
    try {
      const o = JSON.parse(argsJson);
      if (o && typeof o === 'object') {
        for (const k of Object.keys(o)) {
          if (summary) summary += '  \u00B7  ';
          let v = typeof o[k] === 'string' ? o[k] : JSON.stringify(o[k]);
          v = v.replace(/\n/g, ' ');
          if (v.length > 60) v = v.slice(0, 60) + '\u2026';
          summary += k + '=' + v;
        }
      }
    } catch (e) { summary = String(argsJson); }
    return summary;
  }

  // C++ sends the bare session FILENAME as the active chat; find the full
  // path that ends with it (timestamp-prefixed names are unique).
  function isActiveSession(path, activeFilename) {
    if (!path || !activeFilename) return false;
    return path === activeFilename ||
           path.lastIndexOf('/' + activeFilename) === path.length - activeFilename.length - 1 ||
           path.lastIndexOf('\\' + activeFilename) === path.length - activeFilename.length - 1;
  }

  function renderHistory(items, active) {
    historyList.innerHTML = '';
    lastSessions = items || [];
    lastActive = active || '';
    if (!items.length) {
      historyList.innerHTML = '<div class="hempty">no saved sessions yet</div>';
      return;
    }
    for (const it of items) {
      const r = document.createElement('div');
      r.className = 'hrow' + (isActiveSession(it.path, active) ? ' on' : '');
      r.innerHTML = '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" style="flex:none;"><path d="M21 11.5a8.4 8.4 0 0 1-8.5 8.3 9 9 0 0 1-3.3-.6L4 20l1-4.1a8 8 0 0 1-1.5-4.4A8.4 8.4 0 0 1 12 3.2a8.4 8.4 0 0 1 9 8.3z"/></svg>' +
                    '<span class="t">' + esc(it.name) + '</span>';
      r.title = it.path;
      r.onclick = () => { closeDrawers(); post({action:'resume-session', path:it.path}); };
      historyList.appendChild(r);
    }
  }

  // ── page events from C++ ──
  window.chrome.webview.addEventListener('message', (ev) => {
    const d = ev.data;
    if (!d || !d.type) return;
    switch (d.type) {
      case 'state': {
        currentModel = d.model;
        $('workspace').textContent = d.workspace;
        $('ver').textContent = 'v' + d.version;
        const sel = $('model');
        sel.innerHTML = '';
        for (const m of d.models) {
          const o = document.createElement('option');
          o.value = m; o.textContent = m;
          if (m === d.model) o.selected = true;
          sel.appendChild(o);
        }
        sel.onchange = function () { post({action:'set-model', value:this.value}); };
        renderModes(d.mode);
        renderHistory(d.sessions || [], d.session || '');
        setBusy(false);
        setStatus('');
        break;
      }
      case 'status': setStatus(d.label); break;
      case 'delta': {
        setStatus('');
        hideWelcome();
        ensureStream();
        streamBuf += d.text;
        streamEl.innerHTML = fmt(streamBuf);
        scrollDown();
        break;
      }
      case 'final': {
        setStatus('');
        if (streamEl !== null) { streamBuf = d.text; streamEl.innerHTML = fmt(streamBuf); }
        else if (d.text) {
          hideWelcome();
          renderAssistant(d.text);
        }
        endStream();
        scrollDown();
        break;
      }
      case 'tool': endStream(); lastTool = addToolStep(d.name, oneLineArgs(d.args)); break;
      case 'tool-result': {
        if (lastTool) {
          const m = document.createElement('span');
          m.className = 'mark ' + (d.ok ? 'ok' : 'fail');
          m.innerHTML = d.ok ? '&#10003; done' : '&#10007; failed';
          lastTool.appendChild(m);
          lastTool = null;
        }
        break;
      }
      case 'notice': addNotice(d.text, !!d.error); break;
      case 'busy': {
        setBusy(!!d.value);
        if (!d.value) endStream();
        break;
      }
      case 'clear': {
        renderTranscript([]);
        break;
      }
      case 'load': renderTranscript(d.messages || []); break;
      case 'history': renderHistory(d.items || [], d.active || ''); break;
      case 'prompt': {
        $('prompt-text').textContent = d.text || '';
        $('prompt-dialog').showModal();
        break;
      }
      case 'confirm': {
        pendingConfirm = d;
        $('confirm-name').textContent = d.name || 'permission';
        $('confirm-text').textContent = d.text || '';
        $('confirm-dialog').showModal();
        break;
      }
      case 'ask': {
        pendingAsk = d;
        $('ask-question').textContent = d.question || '';
        const opts = $('ask-options');
        opts.innerHTML = '';
        for (const o of (d.options || [])) {
          const b = document.createElement('button');
          b.textContent = o;
          b.onclick = function () {
            post({action:'confirm-result', yes:true, text:o});
            pendingAsk = null; $('ask-dialog').close();
          };
          opts.appendChild(b);
        }
        $('ask-input').value = '';
        $('ask-dialog').showModal();
        break;
      }
    }
  });

  // ── drawers ──
  function openDrawer(which) {
    closeDrawers();
    (which === 'history' ? historyDrawer : settingsDrawer).classList.add('open');
    backdrop.classList.add('open');
    if (which === 'history') post({action:'load-history'});
  }
  function closeDrawers() {
    historyDrawer.classList.remove('open');
    settingsDrawer.classList.remove('open');
    backdrop.classList.remove('open');
  }
  $('history-btn').onclick = () => openDrawer('history');
  $('settings-btn').onclick = () => openDrawer('settings');
  $('newchat-btn').onclick = () => { closeDrawers(); if (!busy) post({action:'new-chat'}); };
  $('drawer-newchat').onclick = () => { closeDrawers(); if (!busy) post({action:'new-chat'}); };
  $('refresh-btn').onclick = () => post({action:'ready'});
  $('workspace').onclick = () => { if (!busy) { closeDrawers(); post({action:'choose-workspace'}); } };
  $('prompt-open').onclick = () => { closeDrawers(); post({action:'show-prompt'}); };

  // ── user input ──
  function heightSync() {
    input.style.height = 'auto';
    input.style.height = Math.min(input.scrollHeight, 160) + 'px';
  }
  function send() {
    const t = input.value.trim();
    if (!t || busy) return;
    hideWelcome();
    addUser(t);
    input.value = '';
    heightSync();
    setStatus('working...');
    post({action:'send', text:t});
  }
  sendBtn.onclick = send;
  stopBtn.onclick = () => post({action:'stop'});
  input.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); send(); }
  });
  input.addEventListener('input', heightSync);
  function setPrompt(txt) { input.value = txt; heightSync(); input.focus(); }

  // ── dialogs ──
  // C++ resolves every parked dialog through one action: `yes` decides
  // permission, `text` carries the ask tool's answer.
  $('confirm-yes').onclick = () => {
    post({action:'confirm-result', yes:true, text:''}); pendingConfirm = null;
    $('confirm-dialog').close();
  };
  $('confirm-no').onclick = () => {
    post({action:'confirm-result', yes:false, text:''}); pendingConfirm = null;
    $('confirm-dialog').close();
  };
  $('ask-send').onclick = () => {
    post({action:'confirm-result', yes:true, text:$('ask-input').value});
    pendingAsk = null;
    $('ask-dialog').close();
  };
  $('ask-skip').onclick = () => {
    // skipping answers with "empty" rather than "nobody answered"
    post({action:'confirm-result', yes:true, text:''});
    pendingAsk = null;
    $('ask-dialog').close();
  };
  $('ask-input').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); $('ask-send').click(); }
  });
  $('confirm-dialog').addEventListener('close', () => {
    if (pendingConfirm) { post({action:'confirm-result', yes:false, text:''}); pendingConfirm = null; }
  });
  $('ask-dialog').addEventListener('close', () => {
    // Escape counts as Skip: an empty answer, not a refusal
    if (pendingAsk) { post({action:'confirm-result', yes:true, text:''}); pendingAsk = null; }
  });

  post({action:'ready'});
</script></body></html>
)CHAT_HTML";

// ── window plumbing ──────────────────────────────────────────────────────────

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_SIZE: {
      auto* context = reinterpret_cast<Context*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      if (context != nullptr && context->controller != nullptr) {
        RECT client = {};
        GetClientRect(hwnd, &client);
        context->controller->put_Bounds(client);
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

ChatResult run_chat_app(Options& options, const std::vector<std::string>& models) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    return ChatResult::Unavailable;

  const WebView2Api api = load_webview2_api();
  if (!api.ok()) {
    MessageBoxW(nullptr, api.problem.c_str(), L"Gamma Code — native chat",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    CoUninitialize();
    return ChatResult::Unavailable;
  }

  OllamaClient client(options.base_url);
  const Registry registry = tools::default_registry();

  Context context;
  context.options = &options;
  context.models = models;
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
                     if (value.size() > 300) value = value.substr(0, 297) + "…";
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

  // ── window ──
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
  context.hwnd = CreateWindowExW(0, window_class.lpszClassName, L"Gamma Code",
                                 WS_OVERLAPPEDWINDOW, x, y, width, height, nullptr,
                                 nullptr, instance, nullptr);
  if (context.hwnd == nullptr) {
    ui::set_question_handler(nullptr);
    CoUninitialize();
    return ChatResult::Unavailable;
  }
  SetWindowLongPtrW(context.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&context));
  ShowWindow(context.hwnd, SW_SHOW);

  // auto-resume the newest saved session for this workspace? mint a fresh one
  context.options->session_file = sessions::new_session_path(options);
  context.session_file = context.options->session_file;

  // ── webview bring-up ──
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
    context.send_state();
  }

  // ── the click loop ──
  if (SUCCEEDED(got_webview) && context.webview != nullptr) {
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
      if (context.closing.load()) break;
    }
  }

  // ── teardown: stop any turn, unblock dialogs, join the worker ──
  context.closing = true;
  ui::request_output_stop();
  {
    std::lock_guard<std::mutex> lock(context.dialog_mutex);
    context.dialog_ready = true;
    context.dialog_cv.notify_all();
  }
  if (context.worker.joinable()) context.worker.join();
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
}

// ── the small chooser window: "Open Gamma Code in:" → GUI / CLI ──────────────
// A plain Win32 window (no webview: it must come up instantly, even before
// the runtime check). Owner-painted in the chat page's Gemini palette.

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
int run_chooser() {
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

AppResult run_app(Options& options, OllamaClient&, const Registry&,
                  const std::vector<std::string>& models) {
  const int chosen = run_chooser();
  if (chosen == 0) return AppResult::Exit;      // the X button / Alt+F4
  if (chosen == 2) return AppResult::Cli;       // the terminal front-end
  const ChatResult chat = run_chat_app(options, models);
  return chat == ChatResult::Done ? AppResult::Done : AppResult::Unavailable;
}

} // namespace gamma::ui

#else // !GAMMA_HAS_WEBVIEW

namespace gamma::ui {

bool webview_launcher_built() { return false; }

ChatResult run_chat_app(Options&, const std::vector<std::string>&) {
  return ChatResult::Unavailable;
}

AppResult run_app(Options&, OllamaClient&, const Registry&, const std::vector<std::string>&) {
  return AppResult::Unavailable;
}

} // namespace gamma::ui

#endif // GAMMA_HAS_WEBVIEW
