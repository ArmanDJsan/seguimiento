#define _CRT_SECURE_NO_WARNINGS 

#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

std::ofstream Logger::s_logFile;
std::recursive_mutex Logger::s_logMutex;
LogLevel Logger::s_logLevel = LogLevel::VIB_INFO;

void Logger::Init(const std::string& logName) {
    std::lock_guard<std::recursive_mutex> lock(s_logMutex);

    std::string timestamp = GetTimestamp();
    // Reemplazamos caracteres no válidos para nombres de archivo en Windows
    std::string safeTimestamp = timestamp;
    for (auto& c : safeTimestamp) {
        if (c == ':' || c == ' ') c = '-';
    }

    std::string filename = logName + "_" + safeTimestamp + ".log";
    s_logFile.open(filename, std::ios::out | std::ios::app);

    if (s_logFile.is_open()) {
        Log(LogLevel::VIB_INFO, "Logger initialized: " + filename);
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_logMutex);

    if (s_logFile.is_open()) {
        Log(LogLevel::VIB_INFO, "Logger shutdown");
        s_logFile.close();
    }
}

void Logger::Debug(const std::string& message) { Log(LogLevel::VIB_DEBUG, message); }
void Logger::Info(const std::string& message) { Log(LogLevel::VIB_INFO, message); }
void Logger::Warning(const std::string& message) { Log(LogLevel::VIB_WARNING, message); }
void Logger::Error(const std::string& message) { Log(LogLevel::VIB_ERROR, message); }

void Logger::Log(LogLevel level, const std::string& message) {
    if (level < s_logLevel) {
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(s_logMutex);

    std::string timestamp = GetTimestamp();
    std::string levelStr = LevelToString(level);
    std::string logMessage = "[" + timestamp + "] [" + levelStr + "] " + message;

    std::cout << logMessage << std::endl;

    if (s_logFile.is_open()) {
        s_logFile << logMessage << std::endl;
        s_logFile.flush();
    }
}

std::string Logger::GetTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ) % 1000;

    std::stringstream ss;
    struct tm* timeinfo = std::localtime(&time);
    if (timeinfo) {
        ss << std::put_time(timeinfo, "%Y-%m-%d %H:%M:%S");
    }
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();

    return ss.str();
}

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::VIB_DEBUG:   return "DEBUG";
    case LogLevel::VIB_INFO:    return "INFO";
    case LogLevel::VIB_WARNING: return "WARN";
    case LogLevel::VIB_ERROR:   return "ERROR";
    default:                    return "UNKNOWN";
    }
}