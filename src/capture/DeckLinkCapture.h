/**
 * DeckLinkCapture.h
 * 
 * Custom memory allocator implementation for zero-copy DMA
 * Implements IDeckLinkVideoBufferAllocatorProvider and IDeckLinkVideoInputCallback
 * for direct GPU memory access from Blackmagic DeckLink cards
 * 
 * Architecture: Uses CUDA cudaHostAllocMapped for zero-copy DMA on CUDA-capable GPUs
 * No DirectX dependencies - D3D11 only used for Spout output in separate pipeline
 * 
 * Capture mode: 4x 1080p@30fps for PTZ radar cameras (reduced from 12x 4K@60fps)
 * 
 * SDK Compatibility: Designed for Blackmagic DeckLink SDK 15.3+
 * Required interfaces:
 * - IDeckLinkInputCallback::VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*)
 * - IDeckLinkVideoBufferAllocatorProvider::GetVideoBufferAllocator(...)
 * - IDeckLinkVideoBufferAllocator::AllocateVideoBuffer(IDeckLinkVideoBuffer**)
 * - IDeckLinkVideoBuffer::GetBytes(void**), StartAccess(), EndAccess()
 */

#pragma once

// 1. Windows base headers (must come first for DeckLink SDK)
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

// 2. Blackmagic SDK header (defines IDeckLinkInputCallback and BMD types)
#include "../DeckLinkAPI_h.h"

// 3. CUDA and standard library headers
#include <cuda_runtime.h>
#include <string>
#include <memory>
#include <queue>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <stop_token>
#include <condition_variable>
#include <functional>

/**
 * Video channel structure for managing each capture input
 * Uses CUDA device memory for zero-copy DMA pipeline
 */
struct VideoChannel {
    IDeckLinkInput* deckLinkInput;
    void* cudaYUVBuffer;        // CUDA device memory for YUV input (mapped from host)
    void* cudaBGRABuffer;       // CUDA device memory for BGRA output
    void* hostMappedYUV;        // Host-side mapped pointer (for DeckLink to write to)
    cudaStream_t stream;        // Dedicated CUDA stream per channel
    cudaEvent_t preprocessEvent; // Event for async synchronization
    cudaEvent_t inferenceEvent;  // Event for inference completion
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
class DeckLinkCapture : public IDeckLinkInputCallback {
public:
    DeckLinkCapture();
    ~DeckLinkCapture();
    
    // Initialize capture from specific DeckLink device
    bool Initialize(int deviceIndex, const std::string& channelName);
    
    // Start/Stop capture with C++20 thread safety
    bool Start();
    void Stop();
    
    // Register a callback invoked after color conversion and inference hook
    void SetFrameReadyHandler(std::function<void(const VideoChannel&, cudaStream_t)> handler);
    
    // Get CUDA buffer for YOLO/TensorRT processing
    void* GetBGRABuffer() const { return m_channel.cudaBGRABuffer; }
    void* GetYUVBuffer() const { return m_channel.cudaYUVBuffer; }
    
    // Get channel info
    const VideoChannel& GetChannel() const { return m_channel; }
    
    // Static method to enumerate available devices
    static int EnumerateDevices();
    
    // COM interface implementation (IUnknown)
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv);
    virtual ULONG STDMETHODCALLTYPE AddRef();
    virtual ULONG STDMETHODCALLTYPE Release();
    
    // IDeckLinkInputCallback implementation
    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
        BMDVideoInputFormatChangedEvents notificationEvents,
        IDeckLinkDisplayMode* newDisplayMode,
        BMDDetectedVideoInputFormatFlags detectedSignalFlags);
    virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
        IDeckLinkVideoInputFrame* videoFrame,
        IDeckLinkAudioInputPacket* audioPacket);
    
private:
    // Forward declarations for nested classes
    class DeckLinkCudaVideoBuffer;
    class DeckLinkCudaBufferAllocator;
    
    /**
     * Custom video buffer for true zero-copy DMA
     * Implements IDeckLinkVideoBuffer using CUDA pinned memory
     */
    class DeckLinkCudaVideoBuffer : public IDeckLinkVideoBuffer {
    public:
        DeckLinkCudaVideoBuffer(void* buffer, unsigned int size);
        virtual ~DeckLinkCudaVideoBuffer();
        
        // IUnknown interface
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv);
        virtual ULONG STDMETHODCALLTYPE AddRef();
        virtual ULONG STDMETHODCALLTYPE Release();
        
        // IDeckLinkVideoBuffer interface
        virtual HRESULT STDMETHODCALLTYPE GetBytes(void** buffer);
        virtual HRESULT STDMETHODCALLTYPE StartAccess(BMDBufferAccessFlags flags);
        virtual HRESULT STDMETHODCALLTYPE EndAccess(BMDBufferAccessFlags flags);
        
        // Helper to get device pointer from host pointer
        void* GetDevicePointer();
        void* GetHostPointer() const { return m_buffer; }
        
    private:
        std::atomic<ULONG> m_refCount;
        void* m_buffer;
        unsigned int m_size;
        void* m_devicePtr;  // Cached device pointer
    };
    
    /**
     * Custom buffer allocator for zero-copy DMA
     * Implements IDeckLinkVideoBufferAllocator using CUDA pinned mapped memory
     */
    class DeckLinkCudaBufferAllocator : public IDeckLinkVideoBufferAllocator {
    public:
        DeckLinkCudaBufferAllocator(unsigned int bufferSize, unsigned int width, 
                                     unsigned int height, unsigned int rowBytes,
                                     BMDPixelFormat pixelFormat);
        virtual ~DeckLinkCudaBufferAllocator();
        
        // IUnknown interface
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv);
        virtual ULONG STDMETHODCALLTYPE AddRef();
        virtual ULONG STDMETHODCALLTYPE Release();
        
        // IDeckLinkVideoBufferAllocator interface
        virtual HRESULT STDMETHODCALLTYPE AllocateVideoBuffer(IDeckLinkVideoBuffer** allocatedBuffer);
        
        // Helper to check if a buffer is from this allocator
        bool IsOurBuffer(void* ptr);
        void* GetDevicePointer(void* hostPtr);
        
    private:
        std::atomic<ULONG> m_refCount;
        std::mutex m_mutex;
        std::vector<void*> m_allocatedBuffers;
        unsigned int m_bufferSize;
        unsigned int m_width;
        unsigned int m_height;
        unsigned int m_rowBytes;
        BMDPixelFormat m_pixelFormat;
    };
    
    /**
     * Allocator provider for DeckLink SDK 15.3+
     * Implements IDeckLinkVideoBufferAllocatorProvider
     * Creates buffer allocators on demand for each video format
     */
    class DeckLinkCudaAllocatorProvider : public IDeckLinkVideoBufferAllocatorProvider {
    public:
        DeckLinkCudaAllocatorProvider();
        virtual ~DeckLinkCudaAllocatorProvider();
        
        // IUnknown interface
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv);
        virtual ULONG STDMETHODCALLTYPE AddRef();
        virtual ULONG STDMETHODCALLTYPE Release();
        
        // IDeckLinkVideoBufferAllocatorProvider interface
        virtual HRESULT STDMETHODCALLTYPE GetVideoBufferAllocator(
            unsigned int bufferSize,
            unsigned int width,
            unsigned int height,
            unsigned int rowBytes,
            BMDPixelFormat pixelFormat,
            IDeckLinkVideoBufferAllocator** allocator);
        
        // Helper to get device pointer from host pointer (searches all allocators)
        void* GetDevicePointer(void* hostPtr);
        bool IsOurBuffer(void* ptr);
        
    private:
        std::atomic<ULONG> m_refCount;
        std::mutex m_mutex;
        std::vector<DeckLinkCudaBufferAllocator*> m_allocators;
    };
    
    // Frame callback implementation (IDeckLinkInputCallback)
    void OnFrameArrived(IDeckLinkVideoInputFrame* videoFrame);
    
    // Capture thread function (C++20 jthread)
    void CaptureThreadFunc(std::stop_token stopToken);
    void ProcessFrame(IDeckLinkVideoInputFrame* videoFrame);
    void ExecuteInference();
    
    // Channel data
    VideoChannel m_channel;
    
    // DeckLink device handle
    IDeckLink* m_deckLink;
    
    // COM reference count
    std::atomic<ULONG> m_refCount;
    
    // Custom allocator provider for zero-copy DMA (DeckLink SDK 15.3+ interface)
    DeckLinkCudaAllocatorProvider* m_allocatorProvider;
    
    // FIX: Track if cudaYUVBuffer was allocated via fallback path (needs explicit cudaFree)
    bool m_fallbackBufferAllocated;
    
    // C++20 thread management for safe shutdown
    std::jthread m_captureThread;
    
    // Frame queue for async processing
    std::queue<IDeckLinkVideoInputFrame*> m_frameQueue;
    std::mutex m_queueMutex;
    std::condition_variable_any m_frameCv;
    std::function<void(const VideoChannel&, cudaStream_t)> m_frameReadyHandler;
};
