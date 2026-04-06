/**
 * PerformanceMonitor.cpp
 * 
 * Implementation of real-time performance monitoring
 */

#include "PerformanceMonitor.h"
#include "../utils/Logger.h"
#include <sstream>
#include <iomanip>
#include <numeric>
#include <fstream>
#include <filesystem>
#include <ctime>

PerformanceMonitor::PerformanceMonitor(double targetFrameTime, int logInterval)
    : m_targetFrameTime(targetFrameTime)
    , m_logInterval(logInterval)
    , m_frameCount(0)
    , m_recommendedActiveCameras(4)  // Start with 4 cameras
    , m_maxFrameTime(0.0)
    , m_emergencyEventCount(0)
    , m_startTime(std::chrono::steady_clock::now())
    , m_consecutiveSlowFrames(0)
    , m_consecutiveFastFrames(0)
{
    Logger::Info("PerformanceMonitor initialized: target=" + 
                 std::to_string(targetFrameTime) + "ms, log every " + 
                 std::to_string(logInterval) + " frames");
}

PerformanceMonitor::~PerformanceMonitor() {
    // Final statistics
    if (m_frameCount > 0) {
        auto avgTelemetry = GetAverageTelemetry(static_cast<int>(m_recentFrames.size()));
        Logger::Info("Final Performance: " + std::to_string(avgTelemetry.Total()) + 
                    "ms avg over " + std::to_string(m_frameCount) + " frames");
    }
}

void PerformanceMonitor::RecordFrame(const Telemetry& telemetry) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // Add to history
    m_recentFrames.push_back(telemetry);
    if (m_recentFrames.size() > MAX_HISTORY) {
        m_recentFrames.pop_front();
    }
    
    m_frameCount++;
    
    // Track maximum frame time
    double total = telemetry.Total();
    double currentMax = m_maxFrameTime.load();
    if (total > currentMax) {
        m_maxFrameTime.store(total);
    }
    
    // Check auto-adjust
    CheckAutoAdjust(total);
    
    // Log periodically
    if (m_frameCount % m_logInterval == 0) {
        LogAndAdjust(telemetry);
    }
}

void PerformanceMonitor::LogAndAdjust(const Telemetry& current) {
    // Format: [PERF] Cap:2.1ms Sel:0.4ms YOLO:12.3ms NDI:0.8ms Redis:0.2ms Total:15.8ms Active:4/12
    std::ostringstream oss;
    oss << "[PERF] ";
    oss << "Cap:" << std::fixed << std::setprecision(1) << current.capture_ms << "ms ";
    oss << "Sel:" << std::fixed << std::setprecision(1) << current.selector_ms << "ms ";
    oss << "YOLO:" << std::fixed << std::setprecision(1) << current.yolo_ms << "ms ";
    oss << "NDI:" << std::fixed << std::setprecision(1) << current.ndi_ms << "ms ";
    oss << "Redis:" << std::fixed << std::setprecision(1) << current.redis_ms << "ms ";
    oss << "Total:" << std::fixed << std::setprecision(1) << current.Total() << "ms ";
    oss << "Active:" << m_recommendedActiveCameras << "/12";
    
    Logger::Info(oss.str());
}

void PerformanceMonitor::CheckAutoAdjust(double totalTime) {
    // Check if exceeding budget
    if (totalTime > SLOW_LIMIT) {
        m_consecutiveSlowFrames++;
        m_consecutiveFastFrames = 0;
        
        // Reduce active cameras if consistently slow
        if (m_consecutiveSlowFrames >= SLOW_THRESHOLD && m_recommendedActiveCameras > 2) {
            m_recommendedActiveCameras = 2;
            m_consecutiveSlowFrames = 0;
            Logger::Warning("WARN: Frame budget exceeded (" + std::to_string(totalTime) + 
                          "ms > " + std::to_string(SLOW_LIMIT) + 
                          "ms), reducing active cameras to 2");
        }
    } 
    // Check if comfortably under budget
    else if (totalTime < FAST_LIMIT) {
        m_consecutiveFastFrames++;
        m_consecutiveSlowFrames = 0;
        
        // Restore to 4 cameras if consistently fast
        if (m_consecutiveFastFrames >= FAST_THRESHOLD && m_recommendedActiveCameras < 4) {
            m_recommendedActiveCameras = 4;
            m_consecutiveFastFrames = 0;
            Logger::Info("INFO: Performance good (" + std::to_string(totalTime) + 
                        "ms < " + std::to_string(FAST_LIMIT) + 
                        "ms), restoring active cameras to 4");
        }
    } 
    else {
        // In acceptable range - reset counters
        m_consecutiveSlowFrames = 0;
        m_consecutiveFastFrames = 0;
    }
}

Telemetry PerformanceMonitor::GetAverageTelemetry(int numFrames) const {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    Telemetry avg = {0, 0, 0, 0, 0};
    
    if (m_recentFrames.empty()) {
        return avg;
    }
    
    // Calculate average over last N frames
    int count = std::min(numFrames, static_cast<int>(m_recentFrames.size()));
    auto start = m_recentFrames.end() - count;
    
    for (auto it = start; it != m_recentFrames.end(); ++it) {
        avg.capture_ms += it->capture_ms;
        avg.selector_ms += it->selector_ms;
        avg.yolo_ms += it->yolo_ms;
        avg.ndi_ms += it->ndi_ms;
        avg.redis_ms += it->redis_ms;
    }
    
    avg.capture_ms /= count;
    avg.selector_ms /= count;
    avg.yolo_ms /= count;
    avg.ndi_ms /= count;
    avg.redis_ms /= count;
    
    return avg;
}

void PerformanceMonitor::Reset(bool saveBeforeReset, const std::string& runID) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    
    // Save state before reset if requested
    if (saveBeforeReset && m_frameCount > 0) {
        // Generate runID if not provided
        std::string actualRunID = runID;
        if (actualRunID.empty()) {
            auto now = std::chrono::system_clock::now();
            auto time_value = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
            localtime_s(&tm, &time_value);
            
            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
            actualRunID = oss.str();
        }
        
        SaveTelemetryState(actualRunID);
    }
    
    // Reset all counters
    m_recentFrames.clear();
    m_frameCount = 0;
    m_maxFrameTime = 0.0;
    m_emergencyEventCount = 0;
    m_consecutiveSlowFrames = 0;
    m_consecutiveFastFrames = 0;
    m_recommendedActiveCameras = 4;
    m_startTime = std::chrono::steady_clock::now();
    
    Logger::Info("PerformanceMonitor reset");
}

void PerformanceMonitor::SaveTelemetryState(const std::string& runID) {
    // Create logs directory if it doesn't exist
    std::filesystem::create_directories("logs");
    
    // Calculate duration
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime).count();
    
    // Calculate average telemetry
    auto avg = GetAverageTelemetry(static_cast<int>(m_recentFrames.size()));
    
    // Build JSON manually (we already have nlohmann::json included in main.cpp)
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    json << "{\n";
    json << "  \"run_id\": \"" << runID << "\",\n";
    json << "  \"duration_seconds\": " << duration << ",\n";
    json << "  \"total_frames\": " << m_frameCount << ",\n";
    json << "  \"max_frame_time_ms\": " << m_maxFrameTime.load() << ",\n";
    json << "  \"avg_frame_time_ms\": " << avg.Total() << ",\n";
    json << "  \"avg_capture_ms\": " << avg.capture_ms << ",\n";
    json << "  \"avg_selector_ms\": " << avg.selector_ms << ",\n";
    json << "  \"avg_yolo_ms\": " << avg.yolo_ms << ",\n";
    json << "  \"avg_ndi_ms\": " << avg.ndi_ms << ",\n";
    json << "  \"avg_redis_ms\": " << avg.redis_ms << ",\n";
    json << "  \"emergency_events\": " << m_emergencyEventCount << "\n";
    json << "}\n";
    
    // Save to run-specific file
    std::string filename = "logs/run_" + runID + ".json";
    std::ofstream outFile(filename);
    if (outFile.is_open()) {
        outFile << json.str();
        outFile.close();
        Logger::Info("[RESET] Telemetry saved to " + filename);
    } else {
        Logger::Error("Failed to save telemetry to " + filename);
    }
    
    // Also update telemetry_state.json with current state
    std::ofstream stateFile("logs/telemetry_state.json");
    if (stateFile.is_open()) {
        stateFile << json.str();
        stateFile.close();
    }
}
