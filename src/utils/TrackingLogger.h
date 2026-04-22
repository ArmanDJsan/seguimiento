/**
 * TrackingLogger.h
 * 
 * Async logging utility for PTZ tracking debug data
 * Writes centroid positions, progress, and sphere counts to CSV files
 * 
 * Features:
 * - Asynchronous file writing to minimize frame processing latency
 * - Daily log rotation
 * - CSV format for easy analysis
 * - Configurable logging rate
 * 
 * Usage:
 *   TrackingLogger logger("logs/tracking");
 *   logger.Start();
 *   logger.Log(timestamp, centroid, progress);
 *   logger.Stop();
 */

#pragma once

#include <string>
#include <fstream>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace Tracking {

// Forward declaration
struct CentroidResult;

/**
 * Log entry for tracking data
 */
struct TrackingLogEntry {
    int64_t timestamp;
    float centroid_x;
    float centroid_y;
    float std_deviation;
    int sphere_count;
    float progress_pct;
    int tracking_mode;      // 0=PRESET, 1=TRACKING, 2=FALLBACK
    float pan_speed;
    float tilt_speed;
    
    TrackingLogEntry()
        : timestamp(0), centroid_x(0), centroid_y(0)
        , std_deviation(0), sphere_count(0), progress_pct(0)
        , tracking_mode(0), pan_speed(0), tilt_speed(0) {}
};

/**
 * TrackingLogger - Async CSV logger for tracking debug
 * 
 * Thread safety: Log() is thread-safe
 */
class TrackingLogger {
public:
    static constexpr int kMaxQueueSize = 10000;
    static constexpr int kFlushIntervalMs = 1000;
    
    /**
     * Constructor
     * @param basePath Base directory for log files (e.g., "logs/tracking")
     */
    explicit TrackingLogger(const std::string& basePath = "logs/tracking")
        : m_basePath(basePath)
        , m_running(false)
        , m_currentDate("")
    {
    }
    
    ~TrackingLogger() {
        Stop();
    }
    
    // Non-copyable
    TrackingLogger(const TrackingLogger&) = delete;
    TrackingLogger& operator=(const TrackingLogger&) = delete;
    
    /**
     * Start the async logging thread
     * @return true if started successfully
     */
    bool Start() {
        if (m_running) return true;
        
        // Create log directory if needed
        try {
            std::filesystem::create_directories(m_basePath);
        } catch (const std::exception& e) {
            // Log directory creation failure but continue - logging may still work
            // if directory already exists or is created by another process
            std::cerr << "TrackingLogger: Warning - could not create directory '" 
                      << m_basePath << "': " << e.what() << std::endl;
        }
        
        m_running = true;
        m_writerThread = std::thread(&TrackingLogger::WriterLoop, this);
        
        return true;
    }
    
    /**
     * Stop the async logging thread
     */
    void Stop() {
        if (!m_running) return;
        
        m_running = false;
        m_cv.notify_one();
        
        if (m_writerThread.joinable()) {
            m_writerThread.join();
        }
        
        // Flush remaining entries
        FlushQueue();
        
        if (m_file.is_open()) {
            m_file.close();
        }
    }
    
    /**
     * Log a tracking entry
     * @param entry Tracking log entry
     */
    void Log(const TrackingLogEntry& entry) {
        if (!m_running) return;
        
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            
            // Drop oldest entries if queue is full
            if (m_queue.size() >= kMaxQueueSize) {
                m_queue.pop();
            }
            
            m_queue.push(entry);
        }
        
        m_cv.notify_one();
    }
    
    /**
     * Convenience method to log centroid data
     * @param timestamp Current timestamp
     * @param centroid_x Centroid X position
     * @param centroid_y Centroid Y position
     * @param std_deviation Standard deviation
     * @param sphere_count Number of spheres
     * @param progress Progress percentage
     * @param mode Tracking mode
     * @param panSpeed Current pan speed
     * @param tiltSpeed Current tilt speed
     */
    void Log(int64_t timestamp, float centroid_x, float centroid_y,
             float std_deviation, int sphere_count, float progress,
             int mode = 0, float panSpeed = 0, float tiltSpeed = 0) {
        TrackingLogEntry entry;
        entry.timestamp = timestamp;
        entry.centroid_x = centroid_x;
        entry.centroid_y = centroid_y;
        entry.std_deviation = std_deviation;
        entry.sphere_count = sphere_count;
        entry.progress_pct = progress;
        entry.tracking_mode = mode;
        entry.pan_speed = panSpeed;
        entry.tilt_speed = tiltSpeed;
        Log(entry);
    }
    
    /**
     * Check if logger is running
     */
    bool IsRunning() const { return m_running; }
    
    /**
     * Get current log file path
     */
    std::string GetCurrentLogPath() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_currentLogPath;
    }

private:
    std::string m_basePath;
    std::atomic<bool> m_running;
    std::string m_currentDate;
    std::string m_currentLogPath;
    
    std::ofstream m_file;
    std::queue<TrackingLogEntry> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_writerThread;
    
    /**
     * Writer thread main loop
     */
    void WriterLoop() {
        while (m_running) {
            std::unique_lock<std::mutex> lock(m_mutex);
            
            // Wait for entries or shutdown
            m_cv.wait_for(lock, std::chrono::milliseconds(kFlushIntervalMs),
                         [this]() { return !m_queue.empty() || !m_running; });
            
            // Process all queued entries
            while (!m_queue.empty()) {
                TrackingLogEntry entry = m_queue.front();
                m_queue.pop();
                lock.unlock();
                
                WriteEntry(entry);
                
                lock.lock();
            }
        }
    }
    
    /**
     * Flush remaining entries to file
     */
    void FlushQueue() {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        while (!m_queue.empty()) {
            WriteEntry(m_queue.front());
            m_queue.pop();
        }
        
        if (m_file.is_open()) {
            m_file.flush();
        }
    }
    
    /**
     * Write a single entry to file
     */
    void WriteEntry(const TrackingLogEntry& entry) {
        // Check if we need to rotate log file (new day)
        std::string currentDate = GetDateString();
        if (currentDate != m_currentDate) {
            RotateLogFile(currentDate);
        }
        
        if (!m_file.is_open()) return;
        
        // Write CSV line
        m_file << entry.timestamp << ","
               << std::fixed << std::setprecision(6)
               << entry.centroid_x << ","
               << entry.centroid_y << ","
               << entry.std_deviation << ","
               << entry.sphere_count << ","
               << entry.progress_pct << ","
               << entry.tracking_mode << ","
               << entry.pan_speed << ","
               << entry.tilt_speed << "\n";
    }
    
    /**
     * Rotate to a new log file for the given date
     */
    void RotateLogFile(const std::string& newDate) {
        if (m_file.is_open()) {
            m_file.close();
        }
        
        m_currentDate = newDate;
        m_currentLogPath = m_basePath + "/LOG_TRACKING_DEBUG_" + newDate + ".csv";
        
        // Check if file exists to determine if we need header
        bool needsHeader = !std::filesystem::exists(m_currentLogPath);
        
        m_file.open(m_currentLogPath, std::ios::app);
        
        if (m_file.is_open() && needsHeader) {
            // Write CSV header
            m_file << "timestamp,centroid_x,centroid_y,std_deviation,"
                   << "sphere_count,progress_pct,tracking_mode,"
                   << "pan_speed,tilt_speed\n";
        }
    }
    
    /**
     * Get current date as string YYYY-MM-DD
     */
    static std::string GetDateString() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
#ifdef _WIN32
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d");
        return oss.str();
    }
};

} // namespace Tracking
