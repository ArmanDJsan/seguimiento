/**
 * GPUMemoryMonitor.h
 * 
 * Simple GPU memory usage monitoring for detecting memory leaks
 * Tracks VRAM allocation trends and alerts on suspicious growth
 * 
 * FIX: Addressed memory leak from ActiveCameraSelector and DeckLinkCapture
 */

#pragma once

#include <cuda_runtime.h>
#include <atomic>
#include <chrono>
#include <string>

class GPUMemoryMonitor {
public:
    /**
     * Sample current GPU memory usage
     * @return true if successful
     */
    static bool Sample();
    
    /**
     * Get current memory usage in MB
     * @param free_mb Output: Free memory in MB
     * @param total_mb Output: Total memory in MB
     * @param used_mb Output: Used memory in MB
     * @return true if successful
     */
    static bool GetMemoryUsage(size_t& free_mb, size_t& total_mb, size_t& used_mb);
    
    /**
     * Check if memory is growing suspiciously fast (potential leak)
     * @param growth_mb_per_min Output: Growth rate in MB/min
     * @return true if growth exceeds threshold (100 MB/min)
     */
    static bool DetectLeak(float& growth_mb_per_min);
    
    /**
     * Get formatted memory status string
     */
    static std::string GetStatusString();
    
    /**
     * Reset baseline for leak detection
     */
    static void ResetBaseline();

private:
    static std::atomic<size_t> s_lastUsedMB;
    static std::atomic<long long> s_lastSampleTimeMs;
    static std::atomic<size_t> s_baselineUsedMB;
    static std::atomic<long long> s_baselineTimeMs;
};
