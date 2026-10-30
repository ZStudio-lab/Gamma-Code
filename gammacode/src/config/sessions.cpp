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

} // namespace gamma::sessions
