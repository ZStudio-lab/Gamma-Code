#include "config/sessions.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <ctime>

#include <nlohmann/json.hpp>

namespace gamma::sessions {

namespace {

std::filesystem::path sessions_dir() {
  const char* appdata = std::getenv("LOCALAPPDATA");
  std::filesystem::path dir =
      (appdata != nullptr && appdata[0] != '\0')
          ? std::filesystem::path(appdata) / "Gamma" / "sessions"
          : std::filesystem::temp_directory_path() / "gamma-sessions";
  std::error_code error;
  std::filesystem::create_directories(dir, error); // best effort
  return dir;
}

std::string workspace_leaf(const Options& options) {
  std::string workspace = options.workspace;
  if (workspace.empty()) {
    std::error_code error;
    workspace = std::filesystem::current_path(error).string();
  }
  std::string leaf = std::filesystem::path(workspace).filename().string();
  if (leaf.empty()) leaf = "session";
  return leaf;
}

} // namespace

std::string new_session_path(const Options& options) {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  std::ostringstream stamp;
  stamp << std::put_time(&local, "%Y-%m-%d_%H-%M-%S");

  const std::filesystem::path dir = sessions_dir();
  std::filesystem::path candidate = dir / (workspace_leaf(options) + "_" + stamp.str() + ".json");
  std::error_code error;
  int bump = 2;
  while (std::filesystem::exists(candidate, error)) {
    // Same workspace leaf started twice within one second: disambiguate.
    candidate = dir / (workspace_leaf(options) + "_" + stamp.str() + "_" +
                       std::to_string(bump) + ".json");
    ++bump;
  }
  return candidate.string();
}

std::vector<std::string> saved_sessions() {
  std::vector<std::string> out;
  std::error_code error;
  std::filesystem::directory_iterator it(sessions_dir(), error);
  if (error) return out; // no directory / no permission: nothing saved yet
  for (const std::filesystem::directory_entry& entry : it) {
    if (entry.path().extension() == ".json") {
      out.push_back(entry.path().string());
    }
  }
  // Filenames carry the timestamp up-front, so a plain reverse sort puts the
  // newest session first — exactly the pick order the menu wants.
  std::sort(out.begin(), out.end(), std::greater<std::string>{});
  return out;
}

std::string session_display_name(const std::string& path) {
  namespace fs = std::filesystem;
  const std::string stem = fs::path(path).stem().string();

  // The trailing 19 characters are the timestamp "YYYY-MM-DD_HH-MM-SS";
  // everything before it (minus the joining underscore) is the workspace leaf.
  std::string leaf = "session";
  std::string stamp;
  if (stem.size() > 20) {
    stamp = stem.substr(stem.size() - 19);
    leaf = stem.substr(0, stem.size() - 20);
  } else if (!stem.empty()) {
    leaf = stem;
  }
  std::replace(stamp.begin(), stamp.end(), '_', ' ');

  std::size_t messages = 0;
  try {
    std::ifstream in(path, std::ios::binary);
    if (in.is_open()) {
      const nlohmann::json file = nlohmann::json::parse(in);
      if (file.contains("messages") && file["messages"].is_array()) {
        messages = file["messages"].size();
      }
    }
  } catch (const std::exception&) {
    // Corrupt/foreign file: it still shows up, just without a message count.
  }

  return leaf + " · " + stamp + " · " + std::to_string(messages) + " msgs";
}

bool delete_session(const std::string& path) {
  namespace fs = std::filesystem;
  // Guard: refuse anything outside the sessions directory, whatever the
  // caller (a webview message is untrusted input).
  std::error_code error;
  const fs::path sessions = fs::weakly_canonical(sessions_dir(), error);
  const fs::path wanted = fs::weakly_canonical(fs::path(path), error);
  const std::string prefix = sessions.string() + static_cast<char>(fs::path::preferred_separator);
  const std::string wanted_text = wanted.string();
  if (!wanted_text.starts_with(prefix)) return false;
  const bool existed = fs::exists(wanted, error);
  fs::remove(wanted, error);
  return existed && !error;
}

std::size_t delete_all_sessions() {
  namespace fs = std::filesystem;
  std::error_code error;
  fs::directory_iterator it(sessions_dir(), error);
  if (error) return 0;
  std::size_t removed = 0;
  for (const fs::directory_entry& entry : it) {
    if (entry.path().extension() != ".json") continue;
    if (fs::remove(entry.path(), error) && !error) ++removed;
  }
  return removed;
}

// └─ last-used workspace memory ──────────────────────────────────────────────
// One plain line in %LOCALAPPDATA%\Gamma\last_workspace.txt. Best effort by
// design: an unreadable or missing file just means "nothing remembered".

std::filesystem::path workspace_memory_file() {
  const char* appdata = std::getenv("LOCALAPPDATA");
  if (appdata != nullptr && appdata[0] != '\0')
    return std::filesystem::path(appdata) / "Gamma" / "last_workspace.txt";
  return std::filesystem::temp_directory_path() / "gamma-last-workspace.txt";
}

std::string last_workspace() {
  std::error_code error;
  std::ifstream in(workspace_memory_file(), std::ios::binary);
  if (!in.is_open()) return "";
  std::string line;
  std::getline(in, line);
  // trim whitespace/CR the notepad way
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                           line.back() == ' ' || line.back() == '\t'))
    line.pop_back();
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
    line.erase(line.begin());
  if (line.empty()) return "";
  if (!std::filesystem::exists(std::filesystem::path(line), error)) return "";
  return line; // a vanished folder is not a workspace
}

void remember_workspace(const std::string& path) {
  if (path.empty()) return;
  std::error_code error;
  const std::filesystem::path file = workspace_memory_file();
  std::filesystem::create_directories(file.parent_path(), error);
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) return;
  out << path << "\n";
}

// Rename a session by updating its JSON "name" field.
// The file path stays the same; only the display name changes.
bool rename_session(const std::string& path, const std::string& new_name) {
  if (path.empty() || new_name.empty()) return false;
  std::error_code error;
  if (!std::filesystem::exists(std::filesystem::path(path), error)) return false;
  try {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;
    nlohmann::json file;
    in >> file;
    file["name"] = new_name;
    in.close();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << file.dump(2);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

} // namespace gamma::sessions
