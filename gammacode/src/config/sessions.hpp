#pragma once

#include <string>
#include <vector>

#include "config/config.hpp"

namespace gamma::sessions {

// Mints a brand-new, uniquely named session file under
// `%LOCALAPPDATA%\Gamma\sessions\<workspace-leaf>_<timestamp>.json`
// (timestamp in the name makes sessions sort newest-first and keeps them
// unique; a counter suffix guards same-second collisions).
std::string new_session_path(const Options& options);

// Every saved session on disk, newest first.
std::vector<std::string> saved_sessions();

// Human line for the picker: "<leaf> · <timestamp> · <N> msgs".
std::string session_display_name(const std::string& path);

} // namespace gamma::sessions
