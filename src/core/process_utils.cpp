#include <opendriver/core/process_utils.h>
#include <opendriver/core/platform.h>
#include <iostream>

#if defined(OD_PLATFORM_WINDOWS)
// ============================================================================
// WINDOWS IMPLEMENTATION
// ============================================================================
#include <sstream>

namespace opendriver::core {

ProcessHandle SpawnProcess(const std::string& executable,
                           const std::vector<std::string>& args,
                           bool env_clear) {
    ProcessHandle result;

    // Build quoted command-line string
    std::ostringstream cmd;
    cmd << "\"" << executable << "\"";
    for (size_t i = 1; i < args.size(); ++i) {
        cmd << " \"" << args[i] << "\"";
    }
    std::string cmd_str = cmd.str();

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Build environment block: inherit except strip LD_PRELOAD analog
    // On Windows there's no LD_PRELOAD, but we can strip OPENDRIVER_SHIM if set
    char* env_block = nullptr; // nullptr = inherit parent env

    BOOL ok = ::CreateProcessA(
        executable.c_str(),  // lpApplicationName
        cmd_str.data(),      // lpCommandLine (must be writable buffer)
        nullptr,             // lpProcessAttributes
        nullptr,             // lpThreadAttributes
        FALSE,               // bInheritHandles
        CREATE_NO_WINDOW,    // dwCreationFlags
        env_block,           // lpEnvironment
        nullptr,             // lpCurrentDirectory (inherit)
        &si,
        &pi
    );

    if (!ok) {
        DWORD err = ::GetLastError();
        char buf[256] = {};
        ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, buf, sizeof(buf), nullptr);
        std::cerr << "[process_utils] CreateProcess failed: " << buf << std::endl;
        return result;
    }

    result.process = pi.hProcess;
    result.thread  = pi.hThread;
    result.valid   = true;
    return result;
}

bool IsProcessRunning(const ProcessHandle& h) {
    if (!h.valid || h.process == INVALID_HANDLE_VALUE) return false;
    DWORD code = 0;
    if (!::GetExitCodeProcess(h.process, &code)) return false;
    return (code == STILL_ACTIVE);
}

void KillProcess(ProcessHandle& h) {
    if (!h.valid) return;
    if (h.process != INVALID_HANDLE_VALUE) {
        ::TerminateProcess(h.process, 1);
    }
    CloseProcessHandle(h);
}

void CloseProcessHandle(ProcessHandle& h) {
    if (h.thread  != INVALID_HANDLE_VALUE) { ::CloseHandle(h.thread);  h.thread  = INVALID_HANDLE_VALUE; }
    if (h.process != INVALID_HANDLE_VALUE) { ::CloseHandle(h.process); h.process = INVALID_HANDLE_VALUE; }
    h.valid = false;
}

} // namespace opendriver::core

#else
// ============================================================================
// LINUX/macOS IMPLEMENTATION
// ============================================================================
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <cerrno>

namespace opendriver::core {

ProcessHandle SpawnProcess(const std::string& executable,
                           const std::vector<std::string>& args,
                           bool env_clear) {
    ProcessHandle result;

    // Build argv array for execv
    std::vector<const char*> argv;
    argv.push_back(executable.c_str());
    for (size_t i = 1; i < args.size(); ++i) {
        argv.push_back(args[i].c_str());
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[process_utils] fork() failed: " << strerror(errno) << std::endl;
        return result;
    }

    if (pid == 0) {
        // ---- Child process ----
        // Clear LD_PRELOAD to avoid DRM shim/other pollution in child processes
        if (env_clear) {
            unsetenv("LD_PRELOAD");
        }

        // Redirect stdout/stderr to /dev/null to avoid mixing with parent's output
        // (comment out these lines if you want child console output)
        // int devnull = open("/dev/null", O_RDWR);
        // dup2(devnull, STDOUT_FILENO);
        // dup2(devnull, STDERR_FILENO);

        // Create new session so child is independent
        setsid();

        execv(executable.c_str(), const_cast<char* const*>(argv.data()));

        // execv only returns on error
        std::cerr << "[process_utils] execv failed for " << executable
                  << ": " << strerror(errno) << std::endl;
        _exit(127);
    }

    // ---- Parent process ----
    result.pid   = pid;
    result.valid = true;
    return result;
}

bool IsProcessRunning(const ProcessHandle& h) {
    if (!h.valid || h.pid <= 0) return false;
    // WNOHANG: don't block
    int status = 0;
    pid_t ret = waitpid(h.pid, &status, WNOHANG);
    if (ret == 0) return true;  // still running
    if (ret < 0 && errno == EINTR) return true;
    return false; // exited or error
}

void KillProcess(ProcessHandle& h) {
    if (!h.valid || h.pid <= 0) return;
    // Try graceful SIGTERM first, then force SIGKILL
    kill(h.pid, SIGTERM);
    // Reap to avoid zombie
    int status = 0;
    waitpid(h.pid, &status, WNOHANG);
    h.valid = false;
    h.pid   = -1;
}

void CloseProcessHandle(ProcessHandle& h) {
    // On POSIX, handles are just PIDs; nothing to close.
    // If you want to reap zombie, you can waitpid() here.
    if (h.valid && h.pid > 0) {
        int status = 0;
        waitpid(h.pid, &status, WNOHANG);
    }
    h.valid = false;
    h.pid   = -1;
}

} // namespace opendriver::core
#endif
