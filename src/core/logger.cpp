#include <opendriver/core/logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <iostream>

namespace opendriver::core {

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::Initialize(const std::string& log_file, LogLevel min_level, bool console) {
    try {
        std::vector<spdlog::sink_ptr> sinks;
        
        if (console) {
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            sinks.push_back(console_sink);
        }
        
        if (!log_file.empty()) {
            auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_file, 1024 * 1024 * 5, 3);
            sinks.push_back(file_sink);
        }
        
        logger = std::make_shared<spdlog::logger>("opendriver", sinks.begin(), sinks.end());
        spdlog::register_logger(logger);
        
        SetMinLevel(min_level);
        SetPattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
        
        logger->info("Logger initialized");
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    }
}

void Logger::Log(LogLevel level, const std::string& source, const std::string& message) {
    if (!logger) return;
    
    // 1. Log to spdlog (file/console)
    spdlog::level::level_enum spd_level;
    switch (level) {
        case LogLevel::Trace: spd_level = spdlog::level::trace; break;
        case LogLevel::Debug: spd_level = spdlog::level::debug; break;
        case LogLevel::Info:  spd_level = spdlog::level::info; break;
        case LogLevel::Warn:  spd_level = spdlog::level::warn; break;
        case LogLevel::Error: spd_level = spdlog::level::err; break;
        case LogLevel::Critical: spd_level = spdlog::level::critical; break;
        default: spd_level = spdlog::level::info; break;
    }
    
    logger->log(spd_level, "[{}] {}", source, message);

    // 2. Wrap into LogEntry
    LogEntry entry;
    entry.level = level;
    entry.source = source;
    entry.message = message;
    entry.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // 3. Notify listeners and update buffer
    std::lock_guard<std::mutex> lock(log_mutex);
    
    message_buffer.push_back(entry);
    if (message_buffer.size() > MAX_BUFFER_SIZE) {
        message_buffer.erase(message_buffer.begin());
    }

    for (const auto& listener : listeners) {
        if (listener) listener(entry);
    }
}

void Logger::AddListener(LogListener listener) {
    std::lock_guard<std::mutex> lock(log_mutex);
    listeners.push_back(listener);
}

std::vector<LogEntry> Logger::GetRecentMessages() const {
    std::lock_guard<std::mutex> lock(log_mutex);
    return message_buffer;
}

void Logger::SetMinLevel(LogLevel level) {
    if (!logger) return;
    
    switch (level) {
        case LogLevel::Trace: logger->set_level(spdlog::level::trace); break;
        case LogLevel::Debug: logger->set_level(spdlog::level::debug); break;
        case LogLevel::Info:  logger->set_level(spdlog::level::info); break;
        case LogLevel::Warn:  logger->set_level(spdlog::level::warn); break;
        case LogLevel::Error: logger->set_level(spdlog::level::err); break;
        case LogLevel::Critical: logger->set_level(spdlog::level::critical); break;
    }
}

void Logger::SetPattern(const std::string& pattern) {
    if (logger) {
        logger->set_pattern(pattern);
    }
}

void Logger::Shutdown() {
    if (logger) {
        logger->info("Logger shutting down");
        spdlog::shutdown();
        logger.reset();
    }
}

} // namespace opendriver::core
