/**
 * Logger.cpp
 * 
 * Implementation of logging system
 */

#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace VIB {

    std::ofstream Logger::s_logFile;
    std::mutex Logger::s_logMutex;
    LogLevel Logger::s_logLevel = LogLevel::VIB_INFO;

    void Logger::Init(const std::string& logName) {
        std::lock_guard<std::mutex> lock(s_logMutex);
        std::string filename = logName + ".log";
        s_logFile.open(filename, std::ios::out | std::ios::app);
        if (s_logFile.is_open()) {
            Log(LogLevel::VIB_INFO, "Sistema VIB iniciado en Windows 11 Pro.");
        }
    }

    void Logger::Shutdown() {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_logFile.is_open()) {
            s_logFile.close();
        }
    }

    void Logger::Debug(const std::string& message) { Log(LogLevel::VIB_DEBUG, message); }
    void Logger::Info(const std::string& message) { Log(LogLevel::VIB_INFO, message); }
    void Logger::Warning(const std::string& message) { Log(LogLevel::VIB_WARN, message); }
    void Logger::Error(const std::string& message) { Log(LogLevel::VIB_CRITICAL, message); }

    void Logger::Log(LogLevel level, const std::string& message) {
        if (level < s_logLevel) return;
        std::lock_guard<std::mutex> lock(s_logMutex);
        std::string logMessage = "[" + GetTimestamp() + "] [" + LevelToString(level) + "] " + message;
        std::cout << logMessage << std::endl;
        if (s_logFile.is_open()) {
            s_logFile << logMessage << std::endl;
            s_logFile.flush();
        }
    }

    std::string Logger::LevelToString(LogLevel level) {
        switch (level) {
        case LogLevel::VIB_DEBUG:    return "DEBUG";
        case LogLevel::VIB_INFO:     return "INFO";
        case LogLevel::VIB_WARN:     return "WARN";
        case LogLevel::VIB_CRITICAL: return "CRITICAL";
        default:                     return "UNKNOWN";
        }
    }

    std::string Logger::GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%H:%M:%S");
        return ss.str();
    }

} // namespace VIB
