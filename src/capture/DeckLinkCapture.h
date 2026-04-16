/**
 * DeckLinkCapture.h
 * 
 * Custom memory allocator implementation for zero-copy DMA
 * Implements IDeckLinkMemoryAllocator and IDeckLinkVideoInputCallback
 * for direct GPU memory access from Blackmagic DeckLink cards
 * 
 * Architecture: Uses CUDA cudaMallocPinned for zero-copy DMA on RTX 5080
 * No DirectX dependencies - D3D11 only used for Spout output in separate pipeline
 * 
 * SDK Compatibility: Designed for Blackmagic DeckLink SDK 15.3
 * Required interfaces:
 * - IDeckLinkInputCallback::VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*)
 * - IDeckLinkMemoryAllocator::AllocateBuffer(uint32_t, void**)
 * - IDeckLinkMemoryAllocator::ReleaseBuffer(void*)
 * - IDeckLinkMemoryAllocator::Commit()
 * - IDeckLinkMemoryAllocator::Decommit()
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
    // Custom memory allocator for true zero-copy DMA
    // Implements IDeckLinkMemoryAllocator_v14_2_1 interface
    // Uses cudaHostAlloc with cudaHostAllocMapped for direct GPU access
    class DeckLinkCudaAllocator : public IDeckLinkMemoryAllocator_v14_2_1 {
    public:
        DeckLinkCudaAllocator();
        virtual ~DeckLinkCudaAllocator();
        
        // IUnknown interface
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv);
        virtual ULONG STDMETHODCALLTYPE AddRef();
        virtual ULONG STDMETHODCALLTYPE Release();
        
        // IDeckLinkMemoryAllocator_v14_2_1 interface
        virtual HRESULT STDMETHODCALLTYPE AllocateBuffer(unsigned int bufferSize, void** allocatedBuffer);
        virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer(void* buffer);
        virtual HRESULT STDMETHODCALLTYPE Commit();
        virtual HRESULT STDMETHODCALLTYPE Decommit();
        
        // Helper to get device pointer from host pointer
        void* GetDevicePointer(void* hostPtr);
        
    private:
        std::atomic<ULONG> m_refCount;
        std::mutex m_mutex;
        std::vector<void*> m_allocatedBuffers;
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
    
    // Custom allocator for zero-copy DMA
    DeckLinkCudaAllocator* m_allocator;
    
    // C++20 thread management for safe shutdown
    std::jthread m_captureThread;
    
    // Frame queue for async processing
    std::queue<IDeckLinkVideoInputFrame*> m_frameQueue;
    std::mutex m_queueMutex;
    std::condition_variable_any m_frameCv;
    std::function<void(const VideoChannel&, cudaStream_t)> m_frameReadyHandler;
};
