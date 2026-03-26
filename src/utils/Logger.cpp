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

std::ofstream Logger::s_logFile;
std::mutex Logger::s_logMutex;
LogLevel Logger::s_logLevel = LogLevel::INFO;

void Logger::Init(const std::string& logName) {
    std::lock_guard<std::mutex> lock(s_logMutex);
    
    std::string filename = logName + "_" + GetTimestamp() + ".log";
    s_logFile.open(filename, std::ios::out | std::ios::app);
    
    if (s_logFile.is_open()) {
        Log(LogLevel::INFO, "Logger initialized: " + filename);
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(s_logMutex);
    
    if (s_logFile.is_open()) {
        Log(LogLevel::INFO, "Logger shutdown");
        s_logFile.close();
    }
}

void Logger::Debug(const std::string& message) {
    Log(LogLevel::DEBUG, message);
}

void Logger::Info(const std::string& message) {
    Log(LogLevel::INFO, message);
}

void Logger::Warning(const std::string& message) {
    Log(LogLevel::WARNING, message);
}

void Logger::Error(const std::string& message) {
    Log(LogLevel::ERROR, message);
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (level < s_logLevel) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(s_logMutex);
    
    std::string timestamp = GetTimestamp();
    std::string levelStr = LevelToString(level);
    
    std::string logMessage = "[" + timestamp + "] [" + levelStr + "] " + message;
    
    // Console output
    std::cout << logMessage << std::endl;
    
    // File output
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
    ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    
    return ss.str();
}

std::string Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARN";
        case LogLevel::ERROR:   return "ERROR";
        default:                return "UNKNOWN";
    }
}
