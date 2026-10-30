#include "tools/registry.hpp"

#include <algorithm>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
// Windows fallback: CreateProcess("cmd.exe /c <cmd>") with the same pipe
// capture + hard-timeout semantics as the POSIX path below. Windows has no
// fork()/poll()/waitpid(), so the POSIX implementation cannot compile here.
#include <Windows.h>
#include <chrono>
#include <thread>
#else
// Spec path: fork() + execvp("/bin/sh", {"sh","-c",cmd}), pipe capture,
// poll() + waitpid() for the timeout.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace gamma::tools {

namespace {

constexpr int kDefaultTimeoutSeconds = 60;
constexpr int kMaxTimeoutSeconds = 3600;

ToolResult fail(const std::string& message) {
  return ToolResult{false, "", message};
}

#ifdef _WIN32

// one reader thread drains the pipe while the main thread waits on the
// process handle — otherwise a chatty child can deadlock on a full pipe.
struct ReadThreadState {
  HANDLE read_handle = nullptr;
  std::string* output = nullptr;
};

DWORD WINAPI read_pipe_worker(void* raw_state) {
  auto* state = static_cast<ReadThreadState*>(raw_state);
  char chunk[4096];
  DWORD got = 0;
  while (ReadFile(state->read_handle, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
    state->output->append(chunk, got);
  }
  return 0;
}

// Quote a command for cmd.exe /c. Windows has no argv exec, only the raw
// command line, so a leading/trailing quote pair is preserved as-is; this
// keeps `sh -c`-style single command strings working verbatim.
std::string wrap_command_for_cmd_exe(const std::string& command) {
  return "cmd.exe /d /s /c \"" + command + "\"";
}

#else

std::string describe_errno(const std::string& prefix) {
  return prefix + ": " + std::strerror(errno);
}

#endif

bool parse_command_arguments(const nlohmann::json& arguments, std::string& command,
                             long& timeout_seconds) {
  if (!arguments.contains("cmd") || !arguments["cmd"].is_string() ||
      arguments["cmd"].get<std::string>().empty()) {
    return false;
  }
  command = arguments["cmd"].get<std::string>();
  timeout_seconds = kDefaultTimeoutSeconds;
  if (arguments.contains("timeout_seconds") && arguments["timeout_seconds"].is_number_integer()) {
    timeout_seconds =
        std::clamp<long>(arguments["timeout_seconds"].get<long>(), 1, kMaxTimeoutSeconds);
  }
  return true;
}

} // namespace

Tool make_run_command_tool() {
  Tool tool;
  tool.name = "run_command";
  tool.description =
      "Run a shell command and capture stdout/stderr plus the exit code. Runs with a default "
      "timeout of 60 seconds (overridable via timeout_seconds). Asks the user for approval first.";
  tool.schema = nlohmann::json{
      {"type", "object"},
      {"properties",
       {
           {"cmd",
            {
                {"type", "string"},
                {"description", "The shell command to execute"},
            }},
           {"timeout_seconds",
            {
                {"type", "integer"},
                {"description", "Hard timeout in seconds (default 60, max 3600)"},
            }},
       }},
      {"required", nlohmann::json::array({"cmd"})},
  };
  tool.readonly = false;
  tool.prompt_in_auto = true;

  tool.run = [](const nlohmann::json& arguments) -> ToolResult {
    std::string command;
    long timeout_seconds = 0;
    if (!parse_command_arguments(arguments, command, timeout_seconds)) {
      return fail("run_command requires a non-empty 'cmd' string");
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
      return fail("run_command: CreatePipe failed");
    }

    // Child writes; parent and child must not share the write end with our
    // reader. Child gets both stdout and stderr on the same pipe.
    HANDLE child_write = write_end;
    if (!SetHandleInformation(write_end, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT) ||
        !SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(read_end);
      CloseHandle(write_end);
      return fail("run_command: SetHandleInformation failed");
    }

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = child_write;
    startup.hStdError = child_write;
    startup.hStdInput = nullptr; // no stdin for the child

    PROCESS_INFORMATION process = {};
    std::string command_line = wrap_command_for_cmd_exe(command);

    BOOL created = CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
      DWORD error = GetLastError();
      CloseHandle(read_end);
      CloseHandle(write_end);
      return fail("run_command: CreateProcess failed (error " + std::to_string(error) + ")");
    }
    // Now that the child inherited its own copy, drop ours.
    CloseHandle(write_end);

    ReadThreadState reader_state;
    reader_state.read_handle = read_end;
    std::string output;
    reader_state.output = &output;
    HANDLE reader = CreateThread(nullptr, 0, &read_pipe_worker, &reader_state, 0, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    bool timed_out = false;
    for (;;) {
      DWORD wait = WaitForSingleObject(process.hProcess, 50);
      if (wait == WAIT_OBJECT_0)
        break;
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        break;
      }
    }

    CloseHandle(read_end); // EOF for the reader thread
    WaitForSingleObject(reader, 5000);
    CloseHandle(reader);

    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);

    if (timed_out) {
      return fail("run_command: timed out after " + std::to_string(timeout_seconds) +
                  "s (process terminated)\n" + output);
    }
    output = "exit code " + std::to_string(static_cast<long>(exit_code)) + "\n" + output;
    return ToolResult{true, output, ""};
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);

    int pipe_fd[2];
    if (pipe(pipe_fd) != 0) {
      return fail(describe_errno("run_command: pipe failed"));
    }

    const pid_t pid = fork();
    if (pid < 0) {
      ::close(pipe_fd[0]);
      ::close(pipe_fd[1]);
      return fail(describe_errno("run_command: fork failed"));
    }
    if (pid == 0) {
      // Child: both stdout and stderr flow into one pipe; no stdin.
      dup2(pipe_fd[1], STDOUT_FILENO);
      dup2(pipe_fd[1], STDERR_FILENO);
      ::close(pipe_fd[0]);
      ::close(pipe_fd[1]);
      execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      _exit(127); // exec failed
    }

    ::close(pipe_fd[1]);
    std::string output;
    bool timed_out = false;
    for (;;) {
      struct pollfd fds = {pipe_fd[0], POLLIN, 0};
      const auto remaining = deadline - std::chrono::steady_clock::now();
      const int timeout_ms =
          remaining <= std::chrono::steady_clock::duration::zero()
              ? 0
              : static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
      const int ready = poll(&fds, 1, std::min(timeout_ms, 50));
      if (ready < 0) {
        if (errno == EINTR)
          continue;
        ::close(pipe_fd[0]);
        return fail(describe_errno("run_command: poll failed"));
      }
      char chunk[4096];
      const ssize_t got =
          (ready > 0 && (fds.revents & POLLIN) != 0) ? read(pipe_fd[0], chunk, sizeof(chunk)) : 0;
      if (got > 0) {
        output.append(chunk, static_cast<std::size_t>(got));
        continue;
      }
      if ((ready > 0 && (fds.revents & (POLLHUP | POLLERR)) != 0) || ready <= 0) {
        // Pipe closed (child exited) or no data pending — check status.
        int status = 0;
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
          ::close(pipe_fd[0]);
          if (WIFEXITED(status)) {
            output = "exit code " + std::to_string(WEXITSTATUS(status)) + "\n" + output;
            return ToolResult{true, output, ""};
          }
          if (WIFSIGNALED(status)) {
            output = "signaled\n" + output;
            return ToolResult{true, output, ""};
          }
          return fail("run_command: child in unknown state");
        }
        if (done < 0 && errno != EINTR) {
          ::close(pipe_fd[0]);
          return fail(describe_errno("run_command: waitpid failed"));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          timed_out = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    if (timed_out) {
      kill(pid, SIGKILL);
      int status = 0;
      waitpid(pid, &status, 0);
      ::close(pipe_fd[0]);
      return fail("run_command: timed out after " + std::to_string(timeout_seconds) +
                  "s (process killed)\n" + output);
    }
    return ToolResult{true, output, ""}; // unreachable; silences flow warnings
#endif
  };

  return tool;
}

} // namespace gamma::tools
