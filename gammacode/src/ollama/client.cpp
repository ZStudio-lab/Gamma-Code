#include "ollama/client.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <string>

namespace gamma {

namespace {

// curl_global_init/cleanup once, before any easy handle exists.
std::once_flag& curl_once() {
  static std::once_flag flag;
  return flag;
}

void ensure_curl_ready() {
  std::call_once(curl_once(), [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// Appends one response-body chunk into a std::string (CURLOPT_WRITEFUNCTION).
std::size_t write_callback(const char* data, std::size_t size, std::size_t count, void* user) {
  auto* buffer = static_cast<std::string*>(user);
  buffer->append(data, size * count);
  return size * count;
}

// Tiny RAII wrapper for a curl_slist of request headers.
class CurlSlist {
public:
  CurlSlist() = default;
  ~CurlSlist() {
    if (raw_ != nullptr)
      curl_slist_free_all(raw_);
  }
  CurlSlist(const CurlSlist&) = delete;
  CurlSlist& operator=(const CurlSlist&) = delete;

  void append(const char* header) { raw_ = curl_slist_append(raw_, header); }
  curl_slist* get() const { return raw_; }

private:
  curl_slist* raw_ = nullptr;
};

std::string trim(const std::string& text) {
  auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  std::size_t begin = 0;
  while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin])))
    ++begin;
  std::size_t end = text.size();
  while (end > begin && is_space(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

// --- streaming chat -----------------------------------------------------------

// One streamed /api/chat parse state. Streamed Ollama bodies are NDJSON: a
// sequence of newline-delimited objects, each carrying a message *delta*; the
// last one has done:true and (usually) final stats.
struct StreamState {
  std::string pending;              // bytes not yet terminated by '\n'
  std::string content;              // assistant text accumulated so far
  nlohmann::json tool_calls = nlohmann::json::array(); // structured calls seen so far
  std::string error;                // first fatal wire/model error, if any
  bool user_stop = false;           // progress hook requested a mid-stream stop
  const OllamaClient::ProgressFn* progress = nullptr;
};

// Absorbs one finished NDJSON line. Returns false when the stream must abort
// (an error line) — the write callback then returns 0 to stop the transfer.
bool absorb_stream_line(StreamState& state, const std::string& line) {
  if (trim(line).empty()) return true;
  nlohmann::json event;
  try {
    event = nlohmann::json::parse(line);
  } catch (const std::exception& error) {
    state.error =
        "corrupt stream chunk from Ollama (" + std::string(error.what()) + "): " + trim(line).substr(0, 200);
    return false;
  }
  if (!event.is_object()) return true; // ignore stray lines

  if (event.contains("error")) {
    const nlohmann::json& text = event["error"];
    state.error = "Ollama stream error: " +
                  (text.is_string() ? text.get<std::string>() : text.dump());
    return false;
  }
  if (event.contains("message") && event["message"].is_object()) {
    const nlohmann::json& message = event["message"];
    if (message.contains("content") && message["content"].is_string()) {
      state.content += message["content"].get<std::string>();
    }
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
      // Structured calls arrive whole (not per-token); keep every one.
      for (const nlohmann::json& call : message["tool_calls"]) {
        state.tool_calls.push_back(call);
      }
    }
  }
    if (state.progress != nullptr && *state.progress != nullptr) {
      const bool keep = (*state.progress)(state.content);
      if (!keep) {
        state.user_stop = true; // the app asked to stop mid-stream
        return false;
      }
    }
    return true;
  }

std::size_t stream_write_callback(const char* data, std::size_t size, std::size_t count,
                                  void* user) {
  auto* state = static_cast<StreamState*>(user);
  if (state->user_stop) return 0; // runaway chunks after stop: kill the transfer
  const std::size_t bytes = size * count;
  state->pending.append(data, bytes);
  std::size_t newline = state->pending.find('\n');
  while (newline != std::string::npos) {
    const std::string line = state->pending.substr(0, newline);
    state->pending.erase(0, newline + 1);
    if (!absorb_stream_line(*state, line)) {
      return 0; // fatal error: abort the transfer (CURLE_WRITE_ERROR)
    }
    newline = state->pending.find('\n');
  }
  return bytes;
}

// --- streaming model pull ------------------------------------------------------

struct PullState {
  std::string pending;  // bytes not yet terminated by '\n'
  std::string error;    // first {"error": …} seen
  const OllamaClient::PullProgressFn* progress = nullptr;
};

bool absorb_pull_line(PullState& state, const std::string& line) {
  if (trim(line).empty()) return true;
  nlohmann::json event;
  try {
    event = nlohmann::json::parse(line);
  } catch (const std::exception& error) {
    state.error = "corrupt pull chunk from Ollama (" + std::string(error.what()) + "): " +
                  trim(line).substr(0, 200);
    return false;
  }
  if (!event.is_object()) return true;

  if (event.contains("error")) {
    const nlohmann::json& text = event["error"];
    state.error = "Ollama pull error: " +
                  (text.is_string() ? text.get<std::string>() : text.dump());
    return false;
  }

  OllamaClient::PullEvent update;
  if (event.contains("status") && event["status"].is_string())
    update.status = event["status"].get<std::string>();
  if (event.contains("digest") && event["digest"].is_string())
    update.digest = event["digest"].get<std::string>();
  if (event.contains("completed") && event["completed"].is_number())
    update.completed = event["completed"].get<long long>();
  if (event.contains("total") && event["total"].is_number())
    update.total = event["total"].get<long long>();

  if (state.progress != nullptr && *state.progress != nullptr) {
    (*state.progress)(update);
  }
  return true;
}

std::size_t pull_write_callback(const char* data, std::size_t size, std::size_t count,
                                void* user) {
  auto* state = static_cast<PullState*>(user);
  const std::size_t bytes = size * count;
  state->pending.append(data, bytes);
  std::size_t newline = state->pending.find('\n');
  while (newline != std::string::npos) {
    const std::string line = state->pending.substr(0, newline);
    state->pending.erase(0, newline + 1);
    if (!absorb_pull_line(*state, line)) return 0;
    newline = state->pending.find('\n');
  }
  return bytes;
}

} // namespace

OllamaClient::OllamaClient(std::string base_url) : base_url_(std::move(base_url)) {
  ensure_curl_ready();
}

std::string OllamaClient::post(const std::string& path, const std::string& body,
                               long timeout_seconds) {
  CurlSlist headers;
  headers.append("Content-Type: application/json");

  CurlHandle curl;
  std::string response;
  curl_easy_setopt(curl.get(), CURLOPT_URL, (base_url_ + path).c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &write_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout_seconds);

  const CURLcode status = curl_easy_perform(curl.get());
  if (status != CURLE_OK) {
    throw GammaError(std::format("could not reach Ollama at {} (curl: {})", base_url_,
                                 curl_easy_strerror(status)));
  }
  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != 200) {
    throw GammaError(std::format("Ollama answered HTTP {} for {}: {}", http_code, path,
                                 trim(response).substr(0, 400)));
  }
  return response;
}

std::string OllamaClient::get(const std::string& path, long timeout_seconds) {
  CurlHandle curl;
  std::string response;
  curl_easy_setopt(curl.get(), CURLOPT_URL, (base_url_ + path).c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &write_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout_seconds);
  curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L);

  const CURLcode status = curl_easy_perform(curl.get());
  if (status != CURLE_OK) {
    throw GammaError(std::format("could not reach Ollama at {} (curl: {})", base_url_,
                                 curl_easy_strerror(status)));
  }
  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != 200) {
    throw GammaError(std::format("Ollama answered HTTP {} for {}: {}", http_code, path,
                                 trim(response).substr(0, 400)));
  }
  return response;
}

Message OllamaClient::chat(const nlohmann::json& request, const ProgressFn& progress) {
  nlohmann::json body = request;
  body["stream"] = true;

  StreamState state;
  state.progress = &progress;
  CurlSlist headers;
  headers.append("Content-Type: application/json");

  CurlHandle curl;
  // CURLOPT_POSTFIELDS stores only a pointer: keep the body alive for the
  // whole call (a temporary .dump() in the setopt call would dangle).
  const std::string json_body = body.dump();
  curl_easy_setopt(curl.get(), CURLOPT_URL, (base_url_ + "/api/chat").c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, json_body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &stream_write_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  // No wall-clock timeout: a chat takes as long as the model takes. The only
  // abort is a dead stream — fewer than 1 byte/s for 120s means Ollama is not
  // responding at all, and that DOES raise GammaError so gamma never spins
  // forever. A slow-but-alive model can run all day; that costs nothing.
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 120L);
  // TCP keep-alive so intermediate stalls don't silently hang sockets.
  curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);

  const CURLcode status = curl_easy_perform(curl.get());
  if (!state.error.empty()) {
    throw GammaError(state.error);
  }
  if (status == CURLE_WRITE_ERROR && state.user_stop) {
    // User stop: the partial answer so far IS the result; no trailing reads.
  } else {
    if (status == CURLE_WRITE_ERROR) {
      throw GammaError("Ollama stream aborted");
    }
    if (status != CURLE_OK) {
      throw GammaError(std::format("could not reach Ollama at {} (curl: {})", base_url_,
                                   curl_easy_strerror(status)));
    }
    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
      throw GammaError(std::format("Ollama answered HTTP {} for /api/chat: {}", http_code,
                                   trim(state.content + state.pending).substr(0, 400)));
    }
    // A final chunk without a trailing newline is still a real chunk.
    if (!state.pending.empty()) {
      absorb_stream_line(state, state.pending);
      if (!state.error.empty()) {
        throw GammaError(state.error);
      }
    }
  }

  nlohmann::json reply = nlohmann::json::object();
  reply["content"] = state.content;
  if (!state.tool_calls.empty()) {
    reply["tool_calls"] = state.tool_calls;
  }
  return assistant_from_json(reply);
}

std::vector<std::string> OllamaClient::installed_models() {
  nlohmann::json tags;
  try {
    tags = nlohmann::json::parse(get("/api/tags", 10));
  } catch (const GammaError&) {
    throw;
  } catch (const std::exception& error) {
    throw GammaError(std::string("could not parse Ollama /api/tags response: ") + error.what());
  }
  if (!tags.is_object() || !tags.contains("models") || !tags["models"].is_array()) {
    throw GammaError("Ollama /api/tags response has no models array");
  }
  std::vector<std::string> names;
  for (const nlohmann::json& model : tags["models"]) {
    if (model.is_object() && model.contains("name") && model["name"].is_string()) {
      names.push_back(model["name"].get<std::string>());
      // Tags carry ":latest" when untagged; treat bare names as equivalent so
      // `qwen2.5-coder:7b` also matches `qwen2.5-coder:7b-latest` style tags.
      const std::size_t colon = names.back().find(':');
      if (colon != std::string::npos) {
        names.push_back(names.back().substr(0, colon));
      }
    }
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

std::vector<OllamaClient::ModelInfo> OllamaClient::installed_model_info() {
  nlohmann::json tags;
  try {
    tags = nlohmann::json::parse(get("/api/tags", 10));
  } catch (const GammaError&) {
    throw;
  } catch (const std::exception& error) {
    throw GammaError(std::string("could not parse Ollama /api/tags response: ") + error.what());
  }
  if (!tags.is_object() || !tags.contains("models") || !tags["models"].is_array()) {
    throw GammaError("Ollama /api/tags response has no models array");
  }
  std::vector<ModelInfo> models;
  for (const nlohmann::json& model : tags["models"]) {
    if (!model.is_object() || !model.contains("name") || !model["name"].is_string())
      continue;
    ModelInfo info;
    info.name = model["name"].get<std::string>();
    if (model.contains("modified_at") && model["modified_at"].is_string())
      info.modified_at = model["modified_at"].get<std::string>();
    if (model.contains("size") && model["size"].is_number())
      info.size = model["size"].get<long long>();
    models.push_back(std::move(info));
  }
  return models;
}

void OllamaClient::pull_model(const std::string& model, const PullProgressFn& progress) {
  if (model.empty()) throw GammaError("pull_model: empty model name");

  nlohmann::json body = {{"model", model}, {"stream", true}};
  const std::string json_body = body.dump();

  PullState state;
  state.progress = &progress;
  CurlSlist headers;
  headers.append("Content-Type: application/json");

  CurlHandle curl;
  curl_easy_setopt(curl.get(), CURLOPT_URL, (base_url_ + "/api/pull").c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, json_body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &pull_write_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  // A pull can take minutes on a slow link; only a fully dead stream aborts.
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 120L);
  curl_easy_setopt(curl.get(), CURLOPT_TCP_KEEPALIVE, 1L);

  const CURLcode status = curl_easy_perform(curl.get());
  if (!state.error.empty()) throw GammaError(state.error);
  if (status == CURLE_WRITE_ERROR) throw GammaError("Ollama pull aborted");
  if (status != CURLE_OK) {
    throw GammaError(std::format("could not reach Ollama at {} (curl: {})", base_url_,
                                 curl_easy_strerror(status)));
  }
  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != 200) {
    throw GammaError(std::format("Ollama answered HTTP {} for /api/pull: {}", http_code,
                                 trim(state.pending).substr(0, 400)));
  }
  if (!state.pending.empty()) {
    absorb_pull_line(state, state.pending);
    if (!state.error.empty()) throw GammaError(state.error);
  }
}

bool OllamaClient::model_supports_tools(const std::string& model) {
  try {
    const std::string response =
        post("/api/show", nlohmann::json{{"model", model}}.dump(), 15);
    const nlohmann::json info = nlohmann::json::parse(response);
    if (info.contains("capabilities") && info["capabilities"].is_array()) {
      for (const nlohmann::json& capability : info["capabilities"]) {
        if (capability.is_string() && capability.get<std::string>() == "tools") return true;
      }
      return false;
    }
  } catch (const std::exception&) {
    // Unknown → assume yes; the caller only uses this to rank installed models.
    return true;
  }
  return true;
}

} // namespace gamma
