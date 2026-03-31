/**
 * Logger.cpp - FIXED (Recursive Mutex)
 */

#define _CRT_SECURE_NO_WARNINGS

#include "Logger.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>

 // Cambiamos a std::recursive_mutex para permitir llamadas anidadas
std::ofstream Logger::s_logFile;
std::recursive_mutex Logger::s_logMutex;
LogLevel Logger::s_logLevel = LogLevel::LOG_INFO;

void Logger::Init(const std::string& logName) {
    // El hilo bloquea aquí...
    std::lock_guard<std::recursive_mutex> lock(s_logMutex);

    std::string filename = logName + "_" + GetTimestamp() + ".log";
    s_logFile.open(filename, std::ios::out | std::ios::app);

    if (s_logFile.is_open()) {
        // ...y aquí Log() vuelve a bloquearlo sin problemas gracias al recursive_mutex
        Log(LogLevel::LOG_INFO, "Logger initialized: " + filename);
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_logMutex);

    if (s_logFile.is_open()) {
        Log(LogLevel::LOG_INFO, "Logger shutdown");
        s_logFile.close();
    }
}

void Logger::Debug(const std::string& message) { Log(LogLevel::LOG_DEBUG, message); }
void Logger::Info(const std::string& message) { Log(LogLevel::LOG_INFO, message); }
void Logger::Warning(const std::string& message) { Log(LogLevel::LOG_WARNING, message); }
void Logger::Error(const std::string& message) { Log(LogLevel::LOG_ERROR, message); }

void Logger::Log(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(s_logLevel)) {
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
        // Formato seguro para nombres de archivo (cambiado : por -)
        ss << std::put_time(timeinfo, "%Y-%m-%d %H-%M-%S");
    }
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();

    return ss.str();
}

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
    case LogLevel::LOG_DEBUG:   return "DEBUG";
    case LogLevel::LOG_INFO:    return "INFO";
    case LogLevel::LOG_WARNING: return "WARN";
    case LogLevel::LOG_ERROR:   return "ERROR";
    default:                    return "UNKNOWN";
    }
}