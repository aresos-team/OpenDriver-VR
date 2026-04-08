#include <opendriver/core/runtime.h>
#include <opendriver/core/platform.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <string>
#include <atomic>
#include <csignal>

#include <QApplication>
#include "ui/main_window.h"

using namespace opendriver::core;
namespace fs = std::filesystem;

// ============================================================================
// Signal handling — cross-platform graceful shutdown
// ============================================================================

static std::atomic<bool> g_shutdown_requested{false};

#if defined(OD_PLATFORM_WINDOWS)
#include <windows.h>

static BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT) {
        std::cout << "\n[OpenDriver] Shutdown requested." << std::endl;
        g_shutdown_requested = true;
        return TRUE; // handled
    }
    return FALSE;
}

static void InstallSignalHandlers() {
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
}

#else // Linux / macOS

static void SignalHandler(int signum) {
    // async-signal-safe: only write to atomic
    g_shutdown_requested = true;
}

static void InstallSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // don't interrupt slow syscalls
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    // Ignore SIGPIPE — common when IPC client disconnects mid-write
    signal(SIGPIPE, SIG_IGN);
}
#endif

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    // -----------------------------------------------------------------------
    // 1. Determine config directory (cross-platform)
    // -----------------------------------------------------------------------
    std::string config_dir;

    if (argc >= 2) {
        // Allow overriding via CLI: opendriver_runner /path/to/config
        config_dir = argv[1];
    } else {
        config_dir = GetDefaultConfigDir();
    }

    std::cout << "[OpenDriver] Starting runtime — config dir: " << config_dir << std::endl;

    // -----------------------------------------------------------------------
    // 2. Install signal handlers BEFORE initializing (so we handle early
    //    Ctrl+C during plugin loading without atexit mess)
    // -----------------------------------------------------------------------
    InstallSignalHandlers();

    // -----------------------------------------------------------------------
    // 3. Initialize runtime
    // -----------------------------------------------------------------------
    if (!Runtime::GetInstance().Initialize(config_dir)) {
        std::cerr << "[OpenDriver] Failed to initialize runtime." << std::endl;
        return 1;
    }

    std::cout << "[OpenDriver] Runtime initialized. Press Ctrl+C to stop." << std::endl;

    // -----------------------------------------------------------------------
    // 4. Main execution (Runner loop on bg thread, Qt GUI on main thread)
    // -----------------------------------------------------------------------

    // Create QApplication
    int qt_argc = 1;
    char qt_argv_str[] = "opendriver_runner";
    char* qt_argv[] = { qt_argv_str, nullptr };
    
    QApplication app(qt_argc, qt_argv);
    app.setApplicationName("OpenDriver");
    app.setApplicationVersion("0.2.0");

    // Start background thread for VR polling while GUI runs on main thread
    std::thread runner_thread([&]() {
        auto prev = std::chrono::steady_clock::now();
        while (!g_shutdown_requested) {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - prev).count();
            prev = now;
            Runtime::GetInstance().Tick(dt);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    opendriver::ui::MainWindow w(&Runtime::GetInstance());
    w.show();

    int result = app.exec();
    
    // Once Qt exits, signal shutdown for background thread
    g_shutdown_requested = true;
    if (runner_thread.joinable()) {
        runner_thread.join();
    }

    std::cout << "[OpenDriver] Shutting down." << std::endl;
    Runtime::GetInstance().Shutdown();
    std::cout << "[OpenDriver] Runtime stopped cleanly." << std::endl;

    return 0;
}
