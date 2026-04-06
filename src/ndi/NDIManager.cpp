/**
 * NDIManager.cpp
 * 
 * Implementation of NDI sender management for zero-latency video output to vMix
 * 
 * Key design decisions:
 * 1. Zero-copy async sending via NDIlib_send_send_video_async_v2
 * 2. UYVY format preferred (native DeckLink format, vMix converts internally)
 * 3. CUDA pinned memory for efficient GPU->CPU transfer
 * 4. Per-channel CUDA events for synchronization
 * 
 * NDI SDK documentation reference:
 * "When one specifies an async completion callback using 
 *  NDIlib_send_set_video_async_completion then the SDK will send without 
 *  any memory copies."
 */

#include "NDIManager.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <cstring>

// NDI SDK include - conditionally compile based on SDK availability
#if __has_include("Processing.NDI.Lib.h")
    #include "Processing.NDI.Lib.h"
    #define HAS_NDI_SDK 1
#elif __has_include(<ndi/Processing.NDI.Lib.h>)
    #include <ndi/Processing.NDI.Lib.h>
    #define HAS_NDI_SDK 1
#else
    // Stub definitions when NDI SDK is not available
    // This allows compilation to succeed for development/testing
    #define HAS_NDI_SDK 0
    
    // Minimal NDI type stubs
    typedef void* NDIlib_send_instance_t;
    
    // FourCC constants
    enum NDIlib_FourCC_type_e {
        NDIlib_FourCC_type_UYVY = 0x59565955,  // 'UYVY' - YCbCr 4:2:2
        NDIlib_FourCC_type_BGRA = 0x41524742,  // 'BGRA' - 32-bit BGRA
        NDIlib_FourCC_type_BGRX = 0x58524742,  // 'BGRX' - 32-bit BGRX (no alpha)
        NDIlib_FourCC_type_RGBA = 0x41424752,  // 'RGBA' - 32-bit RGBA
        NDIlib_FourCC_type_RGBX = 0x58424752   // 'RGBX' - 32-bit RGBX (no alpha)
    };
    
    // Frame rate constants
    constexpr int NDIlib_frame_rate_numerator_30 = 30000;
    constexpr int NDIlib_frame_rate_denominator_30 = 1001;
    
    // Video frame structure (matches NDI SDK)
    struct NDIlib_video_frame_v2_t {
        int xres;
        int yres;
        NDIlib_FourCC_type_e FourCC;
        int frame_rate_N;
        int frame_rate_D;
        float picture_aspect_ratio;
        int line_stride_in_bytes;
        uint8_t* p_data;
        const char* p_metadata;
        int64_t timecode;
    };
    
    // Send creation settings
    struct NDIlib_send_create_t {
        const char* p_ndi_name;
        const char* p_groups;
        bool clock_video;
        bool clock_audio;
    };
    
    // Stub function declarations (no-op implementations)
    inline bool NDIlib_initialize() { return true; }
    inline void NDIlib_destroy() {}
    inline NDIlib_send_instance_t NDIlib_send_create(const NDIlib_send_create_t*) { return nullptr; }
    inline void NDIlib_send_destroy(NDIlib_send_instance_t) {}
    inline void NDIlib_send_send_video_v2(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t*) {}
    inline void NDIlib_send_send_video_async_v2(NDIlib_send_instance_t, const NDIlib_video_frame_v2_t*) {}
    
    // Async completion callback type
    typedef void (*NDIlib_send_video_async_completion_cb_t)(void* p_instance, const NDIlib_video_frame_v2_t* p_video_data, void* p_user_data);
    inline void NDIlib_send_set_video_async_completion(NDIlib_send_instance_t, NDIlib_send_video_async_completion_cb_t, void*) {}
#endif

namespace {
    // Constants for 4K@30fps (NTSC standard: 29.97fps = 30000/1001)
    // Industry standard: "30fps" commonly refers to 29.97fps NTSC
    constexpr int kFrameRateNumerator = 30000;
    constexpr int kFrameRateDenominator = 1001;  // Results in 29.97fps (NTSC drop-frame)
    constexpr float kAspectRatio = 16.0f / 9.0f;
    
    // Bytes per pixel for different formats
    constexpr size_t kBytesPerPixelUYVY = 2;   // YCbCr 4:2:2
    constexpr size_t kBytesPerPixelBGRA = 4;   // 32-bit BGRA
}

// NDIChannel destructor implementation
NDIManager::NDIChannel::~NDIChannel() {
    if (sender) {
#if HAS_NDI_SDK
        NDIlib_send_destroy(sender);
#endif
        sender = nullptr;
    }
    
    if (transferComplete) {
        cudaEventDestroy(transferComplete);
        transferComplete = nullptr;
    }
    
    if (pinnedBuffer) {
        cudaFreeHost(pinnedBuffer);
        pinnedBuffer = nullptr;
    }
}

NDIManager::NDIManager() 
    : m_initialized(false) {
    Logger::Info("NDIManager created");
}

NDIManager::~NDIManager() {
    ReleaseAll();
}

bool NDIManager::Initialize() {
    if (m_initialized) {
        return true;
    }
    
#if HAS_NDI_SDK
    if (!NDIlib_initialize()) {
        Logger::Error("Failed to initialize NDI library");
        return false;
    }
    Logger::Info("NDI library initialized successfully");
#else
    Logger::Warning("NDI SDK not available - using stub implementation");
    Logger::Warning("Install NDI SDK and rebuild for production use");
#endif
    
    m_initialized = true;
    return true;
}

bool NDIManager::CreateSender(int channelID, const std::string& senderName,
                               unsigned int width, unsigned int height,
                               bool useUYVY) {
    if (!m_initialized) {
        Logger::Error("NDIManager not initialized - call Initialize() first");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    
    // Check if channel already exists
    if (m_channels.find(channelID) != m_channels.end()) {
        Logger::Warning("NDI sender already exists for channel " + std::to_string(channelID));
        return true;
    }
    
    auto channel = std::make_unique<NDIChannel>();
    channel->name = senderName;
    channel->width = width;
    channel->height = height;
    channel->useUYVY = useUYVY;
    channel->isActive = false;
    
    // Calculate buffer size based on format
    size_t bytesPerPixel = useUYVY ? kBytesPerPixelUYVY : kBytesPerPixelBGRA;
    size_t bufferSize = static_cast<size_t>(width) * height * bytesPerPixel;
    
    // Allocate pinned memory for CUDA->CPU transfer
    if (!AllocatePinnedBuffer(*channel, bufferSize)) {
        Logger::Error("Failed to allocate pinned buffer for channel " + std::to_string(channelID));
        return false;
    }
    
    // Create CUDA event for transfer synchronization
    cudaError_t cudaErr = cudaEventCreateWithFlags(&channel->transferComplete, cudaEventDisableTiming);
    if (cudaErr != cudaSuccess) {
        Logger::Error("Failed to create CUDA event: " + std::string(cudaGetErrorString(cudaErr)));
        return false;
    }
    
#if HAS_NDI_SDK
    // Create NDI sender
    NDIlib_send_create_t sendDesc;
    sendDesc.p_ndi_name = senderName.c_str();
    sendDesc.p_groups = nullptr;        // Default group
    sendDesc.clock_video = true;        // Clock video for real-time sending
    sendDesc.clock_audio = false;       // No audio
    
    channel->sender = NDIlib_send_create(&sendDesc);
    if (!channel->sender) {
        Logger::Error("Failed to create NDI sender for: " + senderName);
        return false;
    }
    
    // Set up async completion callback for zero-copy operation
    // The callback is called when NDI no longer needs the frame buffer
    NDIlib_send_set_video_async_completion(
        channel->sender,
        [](void* p_instance, const NDIlib_video_frame_v2_t* p_video_data, void* p_user_data) {
            // Mark frame as no longer in flight
            auto* channelPtr = static_cast<NDIChannel*>(p_user_data);
            if (channelPtr) {
                channelPtr->frameInFlight.store(false, std::memory_order_release);
            }
        },
        channel.get()
    );
    
    Logger::Info("Created NDI sender: " + senderName + 
                 " (" + std::to_string(width) + "x" + std::to_string(height) + 
                 ", " + (useUYVY ? "UYVY" : "BGRA") + ")");
#else
    Logger::Warning("NDI sender created (stub): " + senderName);
#endif
    
    channel->isActive = true;
    m_channels[channelID] = std::move(channel);
    
    return true;
}

bool NDIManager::AllocatePinnedBuffer(NDIChannel& channel, size_t size) {
    if (channel.pinnedBuffer && channel.pinnedBufferSize >= size) {
        return true;  // Buffer already sufficient
    }
    
    // Free existing buffer if any
    if (channel.pinnedBuffer) {
        cudaFreeHost(channel.pinnedBuffer);
        channel.pinnedBuffer = nullptr;
        channel.pinnedBufferSize = 0;
    }
    
    // Allocate new pinned memory
    cudaError_t err = cudaMallocHost(&channel.pinnedBuffer, size);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate pinned memory: " + 
                     std::string(cudaGetErrorString(err)));
        return false;
    }
    
    channel.pinnedBufferSize = size;
    return true;
}

bool NDIManager::SendUYVYFrame(int channelID, void* cudaUYVYBuffer,
                                unsigned int width, unsigned int height,
                                cudaStream_t stream) {
    return SendFrameInternal(channelID, cudaUYVYBuffer, width, height, stream, true);
}

bool NDIManager::SendBGRAFrame(int channelID, void* cudaBGRABuffer,
                                unsigned int width, unsigned int height,
                                cudaStream_t stream) {
    return SendFrameInternal(channelID, cudaBGRABuffer, width, height, stream, false);
}

bool NDIManager::SendFrameInternal(int channelID, void* cudaBuffer,
                                    unsigned int width, unsigned int height,
                                    cudaStream_t stream, bool useUYVY) {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) {
        return false;
    }
    
    NDIChannel& channel = *it->second;
    
    // Check if previous frame is still in flight
    if (channel.frameInFlight.load(std::memory_order_acquire)) {
        // Previous async send hasn't completed - skip this frame
        // This prevents buffer overwriting and maintains real-time performance
        return true;  // Not an error, just flow control
    }
    
    size_t bytesPerPixel = useUYVY ? kBytesPerPixelUYVY : kBytesPerPixelBGRA;
    size_t lineStride = static_cast<size_t>(width) * bytesPerPixel;
    size_t bufferSize = lineStride * height;
    
    // Ensure pinned buffer is large enough
    if (!AllocatePinnedBuffer(channel, bufferSize)) {
        return false;
    }
    
    // Async copy from GPU to pinned host memory
    cudaError_t err = cudaMemcpyAsync(
        channel.pinnedBuffer,
        cudaBuffer,
        bufferSize,
        cudaMemcpyDeviceToHost,
        stream
    );
    
    if (err != cudaSuccess) {
        Logger::Error("CUDA memcpy failed: " + std::string(cudaGetErrorString(err)));
        return false;
    }
    
    // Record event to know when transfer is complete
    cudaEventRecord(channel.transferComplete, stream);
    
    // Wait for transfer to complete before sending
    // PERFORMANCE NOTE: This synchronous wait may cause frame drops under heavy load
    // with 12+ cameras. For production, consider implementing multi-buffering:
    // - Use 2-3 pinned buffers per channel
    // - Pipeline GPU transfer with NDI send (while one buffer is sending, 
    //   another is receiving)
    // - The frameInFlight flag already provides the foundation for this
    cudaEventSynchronize(channel.transferComplete);
    
#if HAS_NDI_SDK
    // Mark frame as in flight
    channel.frameInFlight.store(true, std::memory_order_release);
    
    // Prepare NDI video frame
    NDIlib_video_frame_v2_t ndiFrame;
    ndiFrame.xres = static_cast<int>(width);
    ndiFrame.yres = static_cast<int>(height);
    ndiFrame.FourCC = useUYVY ? NDIlib_FourCC_type_UYVY : NDIlib_FourCC_type_BGRA;
    ndiFrame.frame_rate_N = kFrameRateNumerator;
    ndiFrame.frame_rate_D = kFrameRateDenominator;
    ndiFrame.picture_aspect_ratio = kAspectRatio;
    ndiFrame.line_stride_in_bytes = static_cast<int>(lineStride);
    ndiFrame.p_data = static_cast<uint8_t*>(channel.pinnedBuffer);
    ndiFrame.p_metadata = nullptr;
    ndiFrame.timecode = 0;  // Use NDI's internal timecode
    
    // Send frame asynchronously (zero-copy with completion callback)
    NDIlib_send_send_video_async_v2(channel.sender, &ndiFrame);
#else
    // Stub: just mark as sent
    (void)useUYVY;
#endif
    
    return true;
}

bool NDIManager::SendFrameSync(int channelID, void* hostBuffer,
                                unsigned int width, unsigned int height,
                                bool useUYVY) {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) {
        return false;
    }
    
    NDIChannel& channel = *it->second;
    
#if HAS_NDI_SDK
    size_t bytesPerPixel = useUYVY ? kBytesPerPixelUYVY : kBytesPerPixelBGRA;
    size_t lineStride = static_cast<size_t>(width) * bytesPerPixel;
    
    NDIlib_video_frame_v2_t ndiFrame;
    ndiFrame.xres = static_cast<int>(width);
    ndiFrame.yres = static_cast<int>(height);
    ndiFrame.FourCC = useUYVY ? NDIlib_FourCC_type_UYVY : NDIlib_FourCC_type_BGRA;
    ndiFrame.frame_rate_N = kFrameRateNumerator;
    ndiFrame.frame_rate_D = kFrameRateDenominator;
    ndiFrame.picture_aspect_ratio = kAspectRatio;
    ndiFrame.line_stride_in_bytes = static_cast<int>(lineStride);
    ndiFrame.p_data = static_cast<uint8_t*>(hostBuffer);
    ndiFrame.p_metadata = nullptr;
    ndiFrame.timecode = 0;
    
    // Synchronous send - blocks until frame is sent
    NDIlib_send_send_video_v2(channel.sender, &ndiFrame);
#else
    (void)hostBuffer;
    (void)width;
    (void)height;
    (void)useUYVY;
    (void)channel;
#endif
    
    return true;
}

void NDIManager::ReleaseSender(int channelID) {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    
    auto it = m_channels.find(channelID);
    if (it != m_channels.end()) {
        Logger::Info("Releasing NDI sender for channel " + std::to_string(channelID));
        m_channels.erase(it);
    }
}

void NDIManager::ReleaseAll() {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    
    Logger::Info("Releasing all NDI senders");
    m_channels.clear();
    
#if HAS_NDI_SDK
    if (m_initialized) {
        NDIlib_destroy();
        m_initialized = false;
        Logger::Info("NDI library shut down");
    }
#endif
}

int NDIManager::GetActiveSenderCount() const {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    
    int count = 0;
    for (const auto& pair : m_channels) {
        if (pair.second && pair.second->isActive) {
            count++;
        }
    }
    return count;
}
