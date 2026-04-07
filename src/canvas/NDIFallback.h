/**
 * NDIFallback.h
 * 
 * NDI fallback wrapper for MegaCanvas failure scenarios
 * NDI output is disabled by default - only activates if:
 * 1. MegaCanvas fails to initialize
 * 2. DXGI SwapChain creation fails
 * 3. WGC capture is not available
 * 
 * This keeps NDI code in the project but doesn't use it for primary output
 */

#pragma once

#include "../ndi/NDIManager.h"
#include <memory>
#include <atomic>

/**
 * NDIFallback - Conditional NDI output manager
 * 
 * Usage:
 * 1. Initialize MegaCanvas first
 * 2. If MegaCanvas fails, call NDIFallback::Activate()
 * 3. NDI will then receive frames instead of MegaCanvas
 */
class NDIFallback {
public:
    NDIFallback();
    ~NDIFallback();

    /**
     * Check if fallback should be used
     * @return true if NDI fallback is configured in config.json
     */
    static bool IsFallbackEnabled();

    /**
     * Activate NDI fallback
     * Called automatically if MegaCanvas fails
     * @return true if NDI initialized successfully
     */
    bool Activate();

    /**
     * Deactivate NDI fallback
     * Call when switching back to MegaCanvas
     */
    void Deactivate();

    /**
     * Check if fallback is currently active
     */
    bool IsActive() const { return m_active.load(std::memory_order_acquire); }

    /**
     * Get the underlying NDI manager
     * @return Pointer to NDIManager, or nullptr if not active
     */
    NDIManager* GetNDIManager();

    /**
     * Send frame via NDI (only if active)
     * @param cameraID Camera index (0-11)
     * @param cudaBGRABuffer CUDA device pointer
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream
     * @return true if frame sent (or not active), false on error
     */
    bool SendFrame(int cameraID, void* cudaBGRABuffer,
                   unsigned int width, unsigned int height,
                   cudaStream_t stream);

private:
    std::unique_ptr<NDIManager> m_ndiManager;
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_initialized{false};
};

/**
 * Global NDI fallback instance
 * Thread-safe singleton pattern
 */
NDIFallback& GetNDIFallback();
