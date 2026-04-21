/**
 * GPUMemoryMonitor.cpp
 * 
 * Implementation of simple GPU memory leak detector
 */

#include "GPUMemoryMonitor.h"
#include "Logger.h"
#include <sstream>
#include <iomanip>

// Static member initialization
std::atomic<size_t> GPUMemoryMonitor::s_lastUsedMB{0};
std::atomic<long long> GPUMemoryMonitor::s_lastSampleTimeMs{0};
std::atomic<size_t> GPUMemoryMonitor::s_baselineUsedMB{0};
std::atomic<long long> GPUMemoryMonitor::s_baselineTimeMs{0};

bool GPUMemoryMonitor::Sample() {
    size_t free_bytes, total_bytes;
    cudaError_t err = cudaMemGetInfo(&free_bytes, &total_bytes);
    
    if (err != cudaSuccess) {
        Logger::Error("GPUMemoryMonitor: cudaMemGetInfo failed: " + 
                     std::string(cudaGetErrorString(err)));
        return false;
    }
    
    size_t used_bytes = total_bytes - free_bytes;
    size_t used_mb = used_bytes / (1024 * 1024);
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    s_lastUsedMB.store(used_mb);
    s_lastSampleTimeMs.store(now);
    
    // Initialize baseline on first sample
    if (s_baselineUsedMB.load() == 0) {
        s_baselineUsedMB.store(used_mb);
        s_baselineTimeMs.store(now);
    }
    
    return true;
}

bool GPUMemoryMonitor::GetMemoryUsage(size_t& free_mb, size_t& total_mb, size_t& used_mb) {
    size_t free_bytes, total_bytes;
    cudaError_t err = cudaMemGetInfo(&free_bytes, &total_bytes);
    
    if (err != cudaSuccess) {
        return false;
    }
    
    free_mb = free_bytes / (1024 * 1024);
    total_mb = total_bytes / (1024 * 1024);
    used_mb = (total_bytes - free_bytes) / (1024 * 1024);
    
    return true;
}

bool GPUMemoryMonitor::DetectLeak(float& growth_mb_per_min) {
    size_t current_used = s_lastUsedMB.load();
    long long current_time = s_lastSampleTimeMs.load();
    size_t baseline_used = s_baselineUsedMB.load();
    long long baseline_time = s_baselineTimeMs.load();
    
    if (baseline_used == 0 || baseline_time == 0) {
        growth_mb_per_min = 0.0f;
        return false;  // Not enough data yet
    }
    
    long long elapsed_ms = current_time - baseline_time;
    if (elapsed_ms < 60000) {  // Less than 1 minute
        growth_mb_per_min = 0.0f;
        return false;  // Not enough time elapsed
    }
    
    long long growth_mb = static_cast<long long>(current_used) - static_cast<long long>(baseline_used);
    float elapsed_min = elapsed_ms / 60000.0f;
    growth_mb_per_min = growth_mb / elapsed_min;
    
    // Alert threshold: 100 MB/min sustained growth
    const float LEAK_THRESHOLD_MB_PER_MIN = 100.0f;
    
    if (growth_mb_per_min > LEAK_THRESHOLD_MB_PER_MIN) {
        Logger::Warning("GPU Memory leak detected: " + 
                       std::to_string(growth_mb_per_min) + " MB/min growth rate");
        return true;
    }
    
    return false;
}

std::string GPUMemoryMonitor::GetStatusString() {
    size_t free_mb, total_mb, used_mb;
    if (!GetMemoryUsage(free_mb, total_mb, used_mb)) {
        return "GPU Memory: Query failed";
    }
    
    float growth_rate;
    DetectLeak(growth_rate);
    
    std::ostringstream oss;
    oss << "GPU Memory: " << used_mb << " MB / " << total_mb << " MB ("
        << std::fixed << std::setprecision(1) 
        << (100.0f * used_mb / total_mb) << "%)";
    
    if (std::abs(growth_rate) > 1.0f) {
        oss << " | Growth: " << std::showpos << std::fixed << std::setprecision(1) 
            << growth_rate << " MB/min";
    }
    
    return oss.str();
}

void GPUMemoryMonitor::ResetBaseline() {
    size_t current_used = s_lastUsedMB.load();
    long long current_time = s_lastSampleTimeMs.load();
    
    s_baselineUsedMB.store(current_used);
    s_baselineTimeMs.store(current_time);
    
    Logger::Info("GPU Memory baseline reset: " + std::to_string(current_used) + " MB");
}
