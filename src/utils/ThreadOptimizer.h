#pragma once

// Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <thread>
#include <string>
#include "../utils/Logger.h"

/**
 * ThreadOptimizer - CPU Core Affinity and Thread Priority Management
 * 
 * Optimized for AMD Threadripper PRO 9955WX (16-Core/32-Thread)
 * 
 * Architecture:
 * - CCD 0: Cores 0-7  (Physical cores 0-7, logical threads 0-15)
 * - CCD 1: Cores 8-15 (Physical cores 8-15, logical threads 16-31)
 * 
 * Strategy:
 * - Video pipeline threads (Cap/Sel/NDI) pinned to CCD 0 (cores 0-7)
 * - Avoids inter-CCD latency (~30ns penalty for memory access)
 * - High priority for time-critical video processing
 */
class ThreadOptimizer {
public:
    // Thread affinity profiles for different workload types
    enum class AffinityProfile {
        VIDEO_CAPTURE,      // Cores 0-3 (High priority, low latency)
        VIDEO_PROCESSING,   // Cores 4-7 (High priority, CCD 0)
        BACKGROUND,         // Cores 8-15 (Normal priority, CCD 1)
        UNRESTRICTED        // All cores (default OS scheduling)
    };

    // Thread priority levels
    enum class Priority {
        REALTIME,           // THREAD_PRIORITY_TIME_CRITICAL (use sparingly)
        HIGH,               // THREAD_PRIORITY_HIGHEST
        ABOVE_NORMAL,       // THREAD_PRIORITY_ABOVE_NORMAL
        NORMAL              // THREAD_PRIORITY_NORMAL
    };

    /**
     * Set CPU affinity for the current thread
     * @param profile Affinity profile determining which cores to use
     * @return true if affinity was set successfully
     */
    static bool SetThreadAffinity(AffinityProfile profile) {
        DWORD_PTR affinityMask = GetAffinityMask(profile);
        HANDLE hThread = GetCurrentThread();
        
        DWORD_PTR result = SetThreadAffinityMask(hThread, affinityMask);
        if (result == 0) {
            DWORD error = GetLastError();
            Logger::Warning("Failed to set thread affinity. Error: " + std::to_string(error));
            return false;
        }
        
        Logger::Info("Thread affinity set: " + GetProfileName(profile) + 
                    " (mask: 0x" + ToHexString(affinityMask) + ")");
        return true;
    }

    /**
     * Set thread priority for the current thread
     * @param priority Priority level to set
     * @return true if priority was set successfully
     */
    static bool SetThreadPriority(Priority priority) {
        int winPriority = GetWindowsPriority(priority);
        HANDLE hThread = GetCurrentThread();
        
        if (!::SetThreadPriority(hThread, winPriority)) {
            DWORD error = GetLastError();
            Logger::Warning("Failed to set thread priority. Error: " + std::to_string(error));
            return false;
        }
        
        Logger::Info("Thread priority set: " + GetPriorityName(priority));
        return true;
    }

    /**
     * Optimize the current thread for video capture
     * Sets affinity to VIDEO_CAPTURE and priority to HIGH
     */
    static bool OptimizeForVideoCapture() {
        bool affinityOk = SetThreadAffinity(AffinityProfile::VIDEO_CAPTURE);
        bool priorityOk = SetThreadPriority(Priority::HIGH);
        return affinityOk && priorityOk;
    }

    /**
     * Optimize the current thread for video processing (motion detection, NDI)
     * Sets affinity to VIDEO_PROCESSING and priority to HIGH
     */
    static bool OptimizeForVideoProcessing() {
        bool affinityOk = SetThreadAffinity(AffinityProfile::VIDEO_PROCESSING);
        bool priorityOk = SetThreadPriority(Priority::HIGH);
        return affinityOk && priorityOk;
    }

    /**
     * Get CPU information for logging
     */
    static std::string GetCPUInfo() {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        
        return "CPU: " + std::to_string(sysInfo.dwNumberOfProcessors) + 
               " logical processors, " + std::to_string(sysInfo.dwNumberOfProcessors / 2) + 
               " physical cores (estimated)";
    }

private:
    static DWORD_PTR GetAffinityMask(AffinityProfile profile) {
        switch (profile) {
            case AffinityProfile::VIDEO_CAPTURE:
                // Cores 0-3 (threads 0-7 on SMT)
                // 0x000000FF = bits 0-7 set
                return 0x000000FF;
                
            case AffinityProfile::VIDEO_PROCESSING:
                // Cores 4-7 (threads 8-15 on SMT)
                // 0x0000FF00 = bits 8-15 set
                return 0x0000FF00;
                
            case AffinityProfile::BACKGROUND:
                // Cores 8-15 (threads 16-31 on SMT)
                // 0xFFFF0000 = bits 16-31 set
                return 0xFFFF0000;
                
            case AffinityProfile::UNRESTRICTED:
            default:
                // All cores
                return 0xFFFFFFFF;
        }
    }

    static int GetWindowsPriority(Priority priority) {
        switch (priority) {
            case Priority::REALTIME:
                return THREAD_PRIORITY_TIME_CRITICAL;
            case Priority::HIGH:
                return THREAD_PRIORITY_HIGHEST;
            case Priority::ABOVE_NORMAL:
                return THREAD_PRIORITY_ABOVE_NORMAL;
            case Priority::NORMAL:
            default:
                return THREAD_PRIORITY_NORMAL;
        }
    }

    static std::string GetProfileName(AffinityProfile profile) {
        switch (profile) {
            case AffinityProfile::VIDEO_CAPTURE: return "VIDEO_CAPTURE (cores 0-3)";
            case AffinityProfile::VIDEO_PROCESSING: return "VIDEO_PROCESSING (cores 4-7)";
            case AffinityProfile::BACKGROUND: return "BACKGROUND (cores 8-15)";
            case AffinityProfile::UNRESTRICTED: return "UNRESTRICTED (all cores)";
            default: return "UNKNOWN";
        }
    }

    static std::string GetPriorityName(Priority priority) {
        switch (priority) {
            case Priority::REALTIME: return "REALTIME";
            case Priority::HIGH: return "HIGH";
            case Priority::ABOVE_NORMAL: return "ABOVE_NORMAL";
            case Priority::NORMAL: return "NORMAL";
            default: return "UNKNOWN";
        }
    }

    static std::string ToHexString(DWORD_PTR value) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(value));
        return std::string(buffer);
    }
};
