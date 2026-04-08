#pragma once

// ============================================================================
// CROSS-PLATFORM DETECTION
// ============================================================================

#if defined(_WIN32) || defined(_WIN64)
    #define OD_PLATFORM_WINDOWS 1
    #define OD_PLATFORM_NAME "Windows"
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    // Shared library suffix
    #define OD_LIB_SUFFIX ".dll"
    #define OD_LIB_PREFIX ""
    // IPC socket path (named pipe prefix on Windows)
    #define OD_IPC_ADDRESS "\\\\.\\pipe\\opendriver"
    // Config dir
    #define OD_CONFIG_ENV "APPDATA"
    #define OD_CONFIG_SUBDIR "\\opendriver"
    // Process separator
    #define OD_PATH_SEP "\\"
#elif defined(__linux__)
    #define OD_PLATFORM_LINUX 1
    #define OD_PLATFORM_NAME "Linux"
    #include <unistd.h>
    // Shared library suffix
    #define OD_LIB_SUFFIX ".so"
    #define OD_LIB_PREFIX "lib"
    // IPC socket path (Unix Domain Socket on Linux)
    #define OD_IPC_ADDRESS "/tmp/opendriver.sock"
    // Config dir
    #define OD_CONFIG_ENV "HOME"
    #define OD_CONFIG_SUBDIR "/.config/opendriver"
    // Process separator
    #define OD_PATH_SEP "/"
#elif defined(__APPLE__)
    #define OD_PLATFORM_MACOS 1
    #define OD_PLATFORM_NAME "macOS"
    #include <unistd.h>
    #define OD_LIB_SUFFIX ".dylib"
    #define OD_LIB_PREFIX "lib"
    #define OD_IPC_ADDRESS "/tmp/opendriver.sock"
    #define OD_CONFIG_ENV "HOME"
    #define OD_CONFIG_SUBDIR "/.config/opendriver"
    #define OD_PATH_SEP "/"
#else
    #error "Unsupported platform"
#endif

// ============================================================================
// DLL EXPORT/IMPORT
// ============================================================================

#if defined(OD_PLATFORM_WINDOWS)
    #define OD_EXPORT __declspec(dllexport)
    #define OD_IMPORT __declspec(dllimport)
#else
    #define OD_EXPORT __attribute__((visibility("default")))
    #define OD_IMPORT
#endif

// ============================================================================
// HELPER: get config directory in a cross-platform way
// ============================================================================
#include <string>
#include <cstdlib>

namespace opendriver::core {

inline std::string GetDefaultConfigDir() {
    const char* env = std::getenv(OD_CONFIG_ENV);
    if (env) {
        return std::string(env) + OD_CONFIG_SUBDIR;
    }
    return ".opendriver"; // fallback
}

} // namespace opendriver::core
