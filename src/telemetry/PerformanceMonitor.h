/**
 * PerformanceMonitor.h
 * 
 * Real-time performance monitoring and auto-adjustment
 * Tracks timing for each pipeline stage and adapts quality settings
 */

#pragma once

#include <chrono>
#include <string>
#include <deque>
#include <atomic>
#include <mutex>

/**
 * Telemetry data for a single frame
 */
struct Telemetry {
    double capture_ms;      // DeckLink capture time
    double selector_ms;     // ActiveCameraSelector time
    double yolo_ms;         // YOLO inference time
    double ndi_ms;          // NDI send time
    double redis_ms;        // Redis publish time
    
    /**
     * Calculate total frame processing time
     */
    double Total() const {
        return capture_ms + selector_ms + yolo_ms + ndi_ms + redis_ms;
    }
};

/**
 * Performance monitor with adaptive quality control
 * 
 * Auto-adjusts active camera count based on performance:
 * - If total > 33ms for 10 consecutive frames → reduce to 2 cameras
 * - If total < 20ms for 30 consecutive frames → restore to 4 cameras
 */
class PerformanceMonitor {
public:
    /**
     * Constructor
     * @param targetFrameTime Target frame time in milliseconds (default 33ms for 30fps)
     * @param logInterval Number of frames between log outputs (default 30)
     */
    PerformanceMonitor(double targetFrameTime = 33.0, int logInterval = 30);
    ~PerformanceMonitor();
    
    /**
     * Record telemetry for a frame
     * @param telemetry Frame timing data
     */
    void RecordFrame(const Telemetry& telemetry);
    
    /**
     * Get current recommended number of active cameras
     * @return Recommended active camera count (2 or 4)
     */
    int GetRecommendedActiveCameras() const { return m_recommendedActiveCameras; }
    
    /**
     * Get average telemetry over recent frames
     * @param numFrames Number of recent frames to average (default 30)
     */
    Telemetry GetAverageTelemetry(int numFrames = 30) const;
    
    /**
     * Get frame count
     */
    unsigned long long GetFrameCount() const { return m_frameCount; }
    
    /**
     * Reset statistics
     */
    void Reset();
    
private:
    // Configuration
    double m_targetFrameTime;      // Target frame time (33ms for 30fps)
    int m_logInterval;              // Frames between log outputs
    
    // State
    std::atomic<unsigned long long> m_frameCount;
    std::atomic<int> m_recommendedActiveCameras;
    
    // Recent telemetry buffer (circular)
    mutable std::mutex m_dataMutex;
    std::deque<Telemetry> m_recentFrames;
    static constexpr int MAX_HISTORY = 60;  // Keep last 60 frames (2 seconds @ 30fps)
    
    // Auto-adjust tracking
    int m_consecutiveSlowFrames;     // Frames exceeding budget
    int m_consecutiveFastFrames;     // Frames under target
    static constexpr int SLOW_THRESHOLD = 10;   // Reduce after 10 slow frames
    static constexpr int FAST_THRESHOLD = 30;   // Restore after 30 fast frames
    static constexpr double SLOW_LIMIT = 33.0;  // 33ms = budget exceeded
    static constexpr double FAST_LIMIT = 20.0;  // 20ms = comfortable margin
    
    // Helper methods
    void LogAndAdjust(const Telemetry& current);
    void CheckAutoAdjust(double totalTime);
};
