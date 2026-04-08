#pragma once
// ============================================================================
// CROSS-PLATFORM PROCESS LAUNCHING
// Replaces system() with a safe, non-blocking process spawn
// ============================================================================
#include <opendriver/core/platform.h>
#include <string>
#include <vector>
#include <cstdint>

namespace opendriver::core {

// Opaque process handle
struct ProcessHandle {
#if defined(OD_PLATFORM_WINDOWS)
    HANDLE process = INVALID_HANDLE_VALUE;
    HANDLE thread  = INVALID_HANDLE_VALUE;
#else
    pid_t pid = -1;
#endif
    bool valid = false;
};

// ============================================================================
// SpawnProcess — safe cross-platform alternative to system()
//
// @param executable  Full path to the executable
// @param args        Argument list (argv[0] should be exe name)
// @param env_clear   Clear LD_PRELOAD / parent env pollution before launch
// @return            ProcessHandle (check .valid)
// ============================================================================
ProcessHandle SpawnProcess(const std::string& executable,
                           const std::vector<std::string>& args,
                           bool env_clear = true);

// Check if process is still running
bool IsProcessRunning(const ProcessHandle& h);

// Kill the process (graceful, then hard)
void KillProcess(ProcessHandle& h);

// Close handle resources (does NOT kill the process)
void CloseProcessHandle(ProcessHandle& h);

} // namespace opendriver::core
