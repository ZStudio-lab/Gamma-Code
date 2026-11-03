// Stub: compiled when the WebView2 SDK was not available at configure time.
// The native chat window is unavailable and the console front-end is used.
#include "ui/webview_launcher.hpp"

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
