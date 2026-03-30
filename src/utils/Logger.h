/**
 * Logger.h
 * 
 * Simple logging utility for VIB system
 */

#pragma once

#include <string>
#include <fstream>
#include <mutex>

namespace VIB {

    enum class LogLevel {
        VIB_DEBUG,
        VIB_INFO,
        VIB_WARN,
        VIB_CRITICAL
    };

    class Logger {
    public:
        static void Init(const std::string& logName);
        static void Shutdown();

        static void Debug(const std::string& message);
        static void Info(const std::string& message);
        static void Warning(const std::string& message);
        static void Error(const std::string& message);

        static void SetLogLevel(LogLevel level) { s_logLevel = level; }

    private:
        static void Log(LogLevel level, const std::string& message);
        static std::string GetTimestamp();
        static std::string LevelToString(LogLevel level);

        static std::ofstream s_logFile;
        static std::mutex s_logMutex;
        static LogLevel s_logLevel;
    };

} // namespace VIB