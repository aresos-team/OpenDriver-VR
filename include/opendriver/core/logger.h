#pragma once

#include <string>
#include <memory>
#include <functional>
#include <vector>
#include <mutex>
#include <chrono>
#include <spdlog/spdlog.h>

namespace opendriver::core {

// ============================================================================
// LOGGING STRUCTURES
// ============================================================================

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Critical = 5,
};

struct LogEntry {
    LogLevel level;
    std::string source;
    std::string message;
    long long timestamp; // epoch ms
};

using LogListener = std::function<void(const LogEntry&)>;

// ============================================================================
// LOGGER (singleton wrapper around spdlog)
// ============================================================================

class Logger {
public:
    static Logger& GetInstance();

    // ────────────────────────────────────────────────────────────────────
    // INITIALIZATION
    // ────────────────────────────────────────────────────────────────────

    /// Inicjalizacja logging'u
    /// 
    /// @param log_file: pełna ścieżka do pliku (np ~/.opendriver/opendriver.log)
    /// @param min_level: minimalny level do logowania
    /// @param console: czy logować do stdout
    void Initialize(const std::string& log_file, LogLevel min_level = LogLevel::Info, 
                    bool console = true);

    // ────────────────────────────────────────────────────────────────────
    // LOGGING
    // ────────────────────────────────────────────────────────────────────

    /// Log wiadomość
    /// Format: "[timestamp] [LEVEL] [source] message"
    /// 
    /// Przykład:
    ///   Log(LogLevel::Info, "gyroscope_tracker", "Tracking started");
    ///   Log(LogLevel::Error, "leap_motion", "Device not found");
    void Log(LogLevel level, const std::string& source, const std::string& message);

    // Shortcuts
    void Trace(const std::string& source, const std::string& msg) {
        Log(LogLevel::Trace, source, msg);
    }
    void Debug(const std::string& source, const std::string& msg) {
        Log(LogLevel::Debug, source, msg);
    }
    void Info(const std::string& source, const std::string& msg) {
        Log(LogLevel::Info, source, msg);
    }
    void Warn(const std::string& source, const std::string& msg) {
        Log(LogLevel::Warn, source, msg);
    }
    void Error(const std::string& source, const std::string& msg) {
        Log(LogLevel::Error, source, msg);
    }
    void Critical(const std::string& source, const std::string& msg) {
        Log(LogLevel::Critical, source, msg);
    }

    // ────────────────────────────────────────────────────────────────────
    // CONFIGURATION
    // ────────────────────────────────────────────────────────────────────

    /// Ustaw minimalny level do logowania
    void SetMinLevel(LogLevel level);

    /// Ustaw formatowanie (jeśli chcesz customize)
    void SetPattern(const std::string& pattern);

    // ────────────────────────────────────────────────────────────────────
    // SHUTDOWN
    // ────────────────────────────────────────────────────────────────────

    /// Zamknij logging (flush buffers, close file)
    void Shutdown();

    // ────────────────────────────────────────────────────────────────────
    // LISTENERS
    // ────────────────────────────────────────────────────────────────────

    /// Dodaj słuchacza do logów (np UI)
    void AddListener(LogListener listener);

    /// Pobierz ostatnie wiadomości z bufora
    std::vector<LogEntry> GetRecentMessages() const;

private:
    Logger() = default;
    ~Logger() = default;

    std::shared_ptr<spdlog::logger> logger;
    
    std::vector<LogListener> listeners;
    std::vector<LogEntry> message_buffer;
    mutable std::mutex log_mutex;
    const size_t MAX_BUFFER_SIZE = 500;
};

} // namespace opendriver::core
