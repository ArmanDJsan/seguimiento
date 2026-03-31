#pragma once

#include <string>
#include <fstream>
#include <mutex>

// --- ESCUDO CONTRA MACROS DE WINDOWS ---
#ifdef DEBUG
#undef DEBUG
#endif
#ifdef INFO
#undef INFO
#endif
#ifdef WARNING
#undef WARNING
#endif
#ifdef ERROR
#undef ERROR
#endif
// ---------------------------------------

enum class LogLevel {
    VIB_DEBUG,
    VIB_INFO,
    VIB_WARNING,
    VIB_ERROR
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
    static std::recursive_mutex s_logMutex;
    static LogLevel s_logLevel;
};