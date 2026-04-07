#pragma once

#include <Windows.h>
#include <thread>
#include <string>
#include "../utils/Logger.h"

/**
 * ThreadOptimizer - CPU Core Affinity and Thread Priority Management
 * 
 * Optimized for AMD Threadripper PRO 9955WX (Zen 5, 16-Core/32-Thread)
 * 
 * Architecture:
 * - CCD 0: Cores 0-7  (Physical cores 0-7, logical threads 0-15)
 * - CCD 1: Cores 8-15 (Physical cores 8-15, logical threads 16-31)
 * 
 * Mega-Canvas Strategy:
 * - CCD 0 (Cores 0-7): Video capture pipeline (DeckLink callbacks, color conversion)
 * - CCD 1 (Cores 8-15): Render loop, Atlas composition, TensorRT inference
 * 
 * Inter-CCD Latency: ~30ns penalty for memory access across CCDs
 * Keep related workloads on same CCD to minimize cache misses
 */
class ThreadOptimizer {
public:
    // Thread affinity profiles for different workload types
    // Extended for Mega-Canvas architecture with render/AI separation
    enum class AffinityProfile {
        // CCD 0 - Video Pipeline (Latency Critical)
        DECKLINK_INTERRUPTS,    // Cores 0-1: DeckLink IRQ handlers
        VIDEO_CAPTURE,          // Cores 2-3: DeckLinkCapture threads
        VIDEO_PROCESSING,       // Cores 4-5: Motion detection, color conversion
        NDI_FALLBACK,           // Cores 6-7: NDI output (if enabled)
        
        // CCD 1 - Render/AI Pipeline
        TENSORRT_INFERENCE,     // Cores 8-11: YOLO batch inference
        RENDER_DEDICATED,       // Core 12: Render loop (exclusive)
        ATLAS_COMPOSITOR,       // Core 13: Atlas composition (exclusive)
        REDIS_WORKER,           // Core 14: Data publishing
        BACKGROUND,             // Core 15: Background tasks
        
        UNRESTRICTED            // All cores (default OS scheduling)
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
     * Optimize the current thread for TensorRT inference
     * Sets affinity to TENSORRT_INFERENCE and priority to HIGH
     */
    static bool OptimizeForTensorRT() {
        bool affinityOk = SetThreadAffinity(AffinityProfile::TENSORRT_INFERENCE);
        bool priorityOk = SetThreadPriority(Priority::HIGH);
        return affinityOk && priorityOk;
    }

    /**
     * Optimize the current thread for render loop (Mega-Canvas)
     * Sets affinity to RENDER_DEDICATED and priority to REALTIME
     */
    static bool OptimizeForRender() {
        bool affinityOk = SetThreadAffinity(AffinityProfile::RENDER_DEDICATED);
        bool priorityOk = SetThreadPriority(Priority::REALTIME);
        return affinityOk && priorityOk;
    }

    /**
     * Optimize the current thread for Atlas composition
     * Sets affinity to ATLAS_COMPOSITOR and priority to HIGH
     */
    static bool OptimizeForAtlasComposition() {
        bool affinityOk = SetThreadAffinity(AffinityProfile::ATLAS_COMPOSITOR);
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
            // CCD 0 - Video Pipeline (Threads 0-15)
            case AffinityProfile::DECKLINK_INTERRUPTS:
                // Cores 0-1 (threads 0-3 on SMT) - Reserved for IRQ handlers
                return 0x0000000F;
                
            case AffinityProfile::VIDEO_CAPTURE:
                // Cores 2-3 (threads 4-7 on SMT) - DeckLink capture
                return 0x000000F0;
                
            case AffinityProfile::VIDEO_PROCESSING:
                // Cores 4-5 (threads 8-11 on SMT) - Motion, color conversion
                return 0x00000F00;
                
            case AffinityProfile::NDI_FALLBACK:
                // Cores 6-7 (threads 12-15 on SMT) - NDI backup output
                return 0x0000F000;
                
            // CCD 1 - Render/AI Pipeline (Threads 16-31)
            case AffinityProfile::TENSORRT_INFERENCE:
                // Cores 8-11 (threads 16-23 on SMT) - YOLO batch processing
                return 0x00FF0000;
                
            case AffinityProfile::RENDER_DEDICATED:
                // Core 12 only (thread 24, no SMT sibling) - Render loop
                return 0x01000000;
                
            case AffinityProfile::ATLAS_COMPOSITOR:
                // Core 13 only (thread 26, no SMT sibling) - Atlas composition
                return 0x04000000;
                
            case AffinityProfile::REDIS_WORKER:
                // Core 14 only (thread 28, no SMT sibling) - Data publishing
                return 0x10000000;
                
            case AffinityProfile::BACKGROUND:
                // Core 15 (threads 30-31 on SMT) - Background tasks
                return 0xC0000000;
                
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
            case AffinityProfile::DECKLINK_INTERRUPTS: return "DECKLINK_INTERRUPTS (cores 0-1)";
            case AffinityProfile::VIDEO_CAPTURE: return "VIDEO_CAPTURE (cores 2-3)";
            case AffinityProfile::VIDEO_PROCESSING: return "VIDEO_PROCESSING (cores 4-5)";
            case AffinityProfile::NDI_FALLBACK: return "NDI_FALLBACK (cores 6-7)";
            case AffinityProfile::TENSORRT_INFERENCE: return "TENSORRT_INFERENCE (cores 8-11)";
            case AffinityProfile::RENDER_DEDICATED: return "RENDER_DEDICATED (core 12)";
            case AffinityProfile::ATLAS_COMPOSITOR: return "ATLAS_COMPOSITOR (core 13)";
            case AffinityProfile::REDIS_WORKER: return "REDIS_WORKER (core 14)";
            case AffinityProfile::BACKGROUND: return "BACKGROUND (core 15)";
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
