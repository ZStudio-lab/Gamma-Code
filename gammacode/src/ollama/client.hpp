#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "agent/messages.hpp"

namespace gamma {

// Thrown for wire-level failures (connection, HTTP status, unparseable body).
// Callers catch it and surface the text as a tool error — nothing crosses the
// tool boundary as an exception.
struct GammaError : public std::runtime_error {
  using std::runtime_error::runtime_error;
};

// RAII wrapper around a libcurl easy handle.
class CurlHandle {
public:
  CurlHandle() : handle_(curl_easy_init()) {
    if (handle_ == nullptr) {
      throw GammaError("curl_easy_init failed");
    }
  }
  ~CurlHandle() {
    if (handle_ != nullptr)
      curl_easy_cleanup(handle_);
  }
  CurlHandle(const CurlHandle&) = delete;
  CurlHandle& operator=(const CurlHandle&) = delete;
  CurlHandle(CurlHandle&&) = delete;
  CurlHandle& operator=(CurlHandle&&) = delete;

  CURL* get() const { return handle_; }

private:
  CURL* handle_;
};

// Talks to Ollama on 127.0.0.1 (or a loopback base URL validated at startup).
// This is the only loopback HTTP in gamma; the setup module makes one
// user-approved HTTPS download of the official Ollama installer.
class OllamaClient {
public:
  // One installed model, as reported by /api/tags. modified_at is RFC 3339,
  // so it also sorts lexicographically (newest last).
  struct ModelInfo {
    std::string name;
    std::string modified_at;
    long long size = 0;
  };

  // One NDJSON event from POST /api/pull: "pulling manifest", a layer's byte
  // progress (completed/total), "verifying sha256 digest", "success", …
  struct PullEvent {
    std::string status;
    std::string digest;
    long long completed = 0;
    long long total = 0;
  };
  using PullProgressFn = std::function<void(const PullEvent& event)>;

  // Called once per streamed NDJSON chunk with the assistant content
  // accumulated SO FAR — the UI uses it to show what the model is doing
  // right now (thinking / building a tool call / which tool). Returning
  // false is the stop-output request: the transfer aborts cleanly and
  // chat() returns everything received so far (a partial message) without
  // throwing. Called on the thread that runs chat(), never concurrently
  // with itself.
  using ProgressFn = std::function<bool(const std::string& content)>;

  explicit OllamaClient(std::string base_url);

  // POST /api/chat, streamed (stream:true internally). Returns the complete
  // assistant message once the stream ends — or a partial one when the
  // caller's ProgressFn said stop. There is no fixed call timeout: the
  // transfer may take as long as the model keeps producing tokens. The only
  // other abort is a DEAD stream — no bytes for 120s raises GammaError.
  // Throws GammaError for wire failures (never on a clean user stop).
  Message chat(const nlohmann::json& request, const ProgressFn& progress = nullptr);

  // GET /api/tags → installed model names. Throws GammaError.
  std::vector<std::string> installed_models();

  // GET /api/tags → exact names with modified_at and size. Used at startup to
  // pick the most recently updated model when none was requested explicitly.
  std::vector<ModelInfo> installed_model_info();

  // POST /api/pull, streamed. Blocks until the pull finishes; progress() is
  // called for every NDJSON event (status lines and byte progress). Throws
  // GammaError on a wire failure or an {"error": …} event.
  void pull_model(const std::string& model, const PullProgressFn& progress = nullptr);

  // POST /api/show → whether the model advertises the "tools" capability.
  // Ollama builds that omit capabilities are treated as tool-capable (that
  // was the only kind of model those builds could run).
  bool model_supports_tools(const std::string& model);

  const std::string& base_url() const { return base_url_; }

private:
  std::string post(const std::string& path, const std::string& body, long timeout_seconds);
  std::string get(const std::string& path, long timeout_seconds);

  std::string base_url_;
};

} // namespace gamma
