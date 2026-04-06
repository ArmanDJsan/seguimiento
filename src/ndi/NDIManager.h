/**
 * NDIManager.h
 * 
 * Manages NDI senders for multiple video channels
 * Sends zero-latency video to vMix via NDI (Network Device Interface)
 * 
 * Key advantages over Spout:
 * - vMix native NDI support (no plugin needed)
 * - Zero-copy async sending via completion callbacks
 * - Network-transparent (works across machines if needed)
 * - UYVY format support (native DeckLink format, no conversion needed for vMix)
 * 
 * Architecture notes:
 * - Uses async completion callbacks for zero-copy sending
 * - Supports both UYVY (optimal) and BGRA (for YOLO) formats
 * - CUDA interop for direct GPU memory access
 */

#pragma once

#include <cuda_runtime.h>
#include <string>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>

// Forward declaration for NDI SDK types
// Full definitions come from Processing.NDI.Lib.h (NDI SDK 6 required)
struct NDIlib_send_instance_type;
typedef NDIlib_send_instance_type* NDIlib_send_instance_t;

/**
 * NDI video frame structure (compatible with NDIlib_video_frame_v2_t)
 * Used internally to manage frame data and async completion
 */
struct NDIVideoFrame {
    int width;
    int height;
    int fourCC;             // NDIlib_FourCC_type_UYVY or NDIlib_FourCC_type_BGRA
    int frameRateN;         // Frame rate numerator (e.g., 30000)
    int frameRateD;         // Frame rate denominator (e.g., 1001 for 29.97fps)
    float aspectRatio;      // Pixel aspect ratio
    int lineStride;         // Bytes per line
    void* data;             // Pointer to video data
    bool isInFlight;        // True if frame is being sent asynchronously
    cudaStream_t stream;    // Associated CUDA stream for sync
};

/**
 * Manages multiple NDI senders for video output to vMix
 * 
 * Thread safety:
 * - CreateSender/ReleaseSender/ReleaseAll are NOT thread-safe
 * - SendFrame/SendFrameAsync are thread-safe per channel
 */
class NDIManager {
public:
    /**
     * Initialize NDI Manager
     * Note: Unlike SpoutManager, NDI does not require a D3D11 device
     */
    NDIManager();
    ~NDIManager();

    /**
     * Initialize the NDI library
     * Must be called once before creating any senders
     * @return true if NDI library initialized successfully
     */
    bool Initialize();

    /**
     * Check if NDI SDK is available
     * @return true if NDI SDK is loaded and operational
     */
    bool IsInitialized() const { return m_initialized; }

    /**
     * Create an NDI sender for a video channel
     * @param channelID Unique channel identifier (0-11 for 12 cameras)
     * @param senderName NDI source name (appears in vMix as "VIB_CAM_01" etc.)
     * @param width Frame width in pixels (3840 for 4K)
     * @param height Frame height in pixels (2160 for 4K)
     * @param useUYVY True for UYVY format (optimal), false for BGRA
     * @return true if sender created successfully
     */
    bool CreateSender(int channelID, const std::string& senderName,
                      unsigned int width, unsigned int height,
                      bool useUYVY = true);

    /**
     * Send a frame from CUDA device memory (UYVY format)
     * Uses async completion callback for zero-copy
     * @param channelID Channel identifier
     * @param cudaUYVYBuffer CUDA device pointer to UYVY frame data
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream for synchronization
     * @return true if frame was queued for sending
     */
    bool SendUYVYFrame(int channelID, void* cudaUYVYBuffer,
                       unsigned int width, unsigned int height,
                       cudaStream_t stream);

    /**
     * Send a frame from CUDA device memory (BGRA format)
     * Useful for sending the same frame to NDI that YOLO uses
     * @param channelID Channel identifier
     * @param cudaBGRABuffer CUDA device pointer to BGRA frame data
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream for synchronization
     * @return true if frame was queued for sending
     */
    bool SendBGRAFrame(int channelID, void* cudaBGRABuffer,
                       unsigned int width, unsigned int height,
                       cudaStream_t stream);

    /**
     * Send a frame synchronously (blocking)
     * @param channelID Channel identifier
     * @param hostBuffer CPU memory pointer to frame data
     * @param width Frame width
     * @param height Frame height
     * @param useUYVY True for UYVY format, false for BGRA
     * @return true if frame was sent
     */
    bool SendFrameSync(int channelID, void* hostBuffer,
                       unsigned int width, unsigned int height,
                       bool useUYVY = true);

    /**
     * Release a specific NDI sender
     * @param channelID Channel to release
     */
    void ReleaseSender(int channelID);

    /**
     * Release all NDI senders and shutdown
     */
    void ReleaseAll();

    /**
     * Get the number of active senders
     */
    int GetActiveSenderCount() const;

private:
    /**
     * Internal NDI channel structure
     */
    struct NDIChannel {
        NDIlib_send_instance_t sender = nullptr;
        std::string name;
        unsigned int width = 0;
        unsigned int height = 0;
        bool isActive = false;
        bool useUYVY = true;
        
        // Pinned host memory for CUDA->CPU transfer
        void* pinnedBuffer = nullptr;
        size_t pinnedBufferSize = 0;
        
        // CUDA event for async synchronization
        cudaEvent_t transferComplete = nullptr;
        
        // Frame in flight tracking
        std::atomic<bool> frameInFlight{false};
        
        NDIChannel() = default;
        ~NDIChannel();
    };

    /**
     * Internal helper to allocate pinned memory for a channel
     */
    bool AllocatePinnedBuffer(NDIChannel& channel, size_t size);

    /**
     * Internal helper to send frame with specified format
     * Note: The current implementation uses cudaEventSynchronize which may cause
     * frame drops under heavy load. For production environments with 12+ cameras,
     * consider implementing multi-buffering to pipeline GPU transfers with NDI sends.
     */
    bool SendFrameInternal(int channelID, void* cudaBuffer,
                           unsigned int width, unsigned int height,
                           cudaStream_t stream, bool useUYVY);

    // NDI initialization state
    bool m_initialized = false;
    
    // Channel map (channelID -> channel data)
    std::map<int, std::unique_ptr<NDIChannel>> m_channels;
    
    // Mutex for channel map access
    mutable std::mutex m_channelsMutex;
};
