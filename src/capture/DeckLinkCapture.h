/**
 * DeckLinkCapture.h
 * 
 * Custom memory allocator implementation for zero-copy DMA
 * Implements IDeckLinkMemoryAllocator and IDeckLinkVideoInputCallback
 * for direct GPU memory access from Blackmagic DeckLink cards
 * 
 * Architecture: Uses CUDA cudaMallocPinned for zero-copy DMA on RTX 5080
 * No DirectX dependencies - D3D11 only used for Spout output in separate pipeline
 */

#pragma once

#include <cuda_runtime.h>
#include <string>
#include <memory>
#include <queue>
#include <mutex>
#include <thread>
#include <stop_token>

// Forward declarations for Blackmagic SDK
class IDeckLink;
class IDeckLinkInput;
class IDeckLinkVideoInputFrame;
class IDeckLinkAudioInputPacket;

/**
 * Video channel structure for managing each capture input
 * Uses CUDA device memory for zero-copy DMA pipeline
 */
struct VideoChannel {
    IDeckLinkInput* deckLinkInput;
    void* cudaYUVBuffer;        // CUDA device memory for YUV input
    void* cudaRGBBuffer;        // CUDA device memory for RGB output
    size_t bufferSize;          // Buffer size in bytes
    std::string channelName;
    int channelID;
    unsigned int width;
    unsigned int height;
    bool isActive;
};

/**
 * Custom DeckLink capture class with zero-copy memory allocator
 * Philosophy: Maximum performance over development comfort
 * Architecture: CUDA-only pipeline, no DirectX dependencies
 */
class DeckLinkCapture {
public:
    DeckLinkCapture();
    ~DeckLinkCapture();
    
    // Initialize capture from specific DeckLink device
    bool Initialize(int deviceIndex, const std::string& channelName);
    
    // Start/Stop capture with C++20 thread safety
    bool Start();
    void Stop();
    
    // Get CUDA buffer for YOLO/TensorRT processing
    void* GetRGBBuffer() const { return m_channel.cudaRGBBuffer; }
    void* GetYUVBuffer() const { return m_channel.cudaYUVBuffer; }
    
    // Get channel info
    const VideoChannel& GetChannel() const { return m_channel; }
    
    // Static method to enumerate available devices
    static int EnumerateDevices();
    
private:
    // Custom memory allocator implementation
    // Uses cudaMallocPinned for zero-copy DMA on RTX 5080
    class CustomAllocator {
    public:
        CustomAllocator();
        ~CustomAllocator();
        
        void* AllocateBuffer(unsigned int bufferSize);
        void ReleaseBuffer(void* buffer);
        
    private:
        std::vector<void*> m_pinnedBuffers;
    };
    
    // Frame callback implementation (IDeckLinkInputCallback)
    void OnFrameArrived(IDeckLinkVideoInputFrame* videoFrame);
    
    // Capture thread function (C++20 jthread)
    void CaptureThreadFunc(std::stop_token stopToken);
    
    // Channel data
    VideoChannel m_channel;
    
    // Custom allocator
    std::unique_ptr<CustomAllocator> m_allocator;
    
    // C++20 thread management for safe shutdown
    std::jthread m_captureThread;
    
    // Frame queue for async processing
    std::queue<IDeckLinkVideoInputFrame*> m_frameQueue;
    std::mutex m_queueMutex;
};
