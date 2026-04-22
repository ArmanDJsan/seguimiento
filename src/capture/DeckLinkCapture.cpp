/**
 * DeckLinkCapture.cpp
 * 
 * Implementation of zero-copy DMA capture from Blackmagic DeckLink cards
 * Uses CUDA cudaMallocPinned for direct GPU memory access on RTX 5080
 * No DirectX dependencies - pure CUDA pipeline
 */

// 1. Windows base headers (FUNDAMENTAL - must come first)
#include <winsock2.h>  // Network types (if needed, must come before windows.h)
// Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // Base Windows definitions
#include <objbase.h>   // COM interfaces (HRESULT, REFIID, IUnknown)

// 2. Blackmagic SDK headers (depend on Windows types)
#include "DeckLinkAPI_h.h" 

// 3. Project headers
#include "DeckLinkCapture.h"
#include "CudaColorConversion.h"
#include "../utils/Logger.h"
#include "../utils/ThreadOptimizer.h"

// 4. Standard library headers
#include <algorithm>

// CUDA error checking macro
#define CUDA_CHECK(call) \
    do { \
        cudaError_t err = call; \
        if (err != cudaSuccess) { \
            Logger::Error("CUDA error: " + std::string(cudaGetErrorString(err)) + \
                         " at " + __FILE__ + ":" + std::to_string(__LINE__)); \
            return false; \
        } \
    } while(0)

DeckLinkCapture::DeckLinkCapture()
    : m_allocatorProvider(nullptr)
    , m_deckLink(nullptr)
    , m_refCount(1)
    , m_fallbackBufferAllocated(false)  // FIX: Initialize fallback flag
{
    // Initialize channel structure
    m_channel.deckLinkInput = nullptr;
    m_channel.cudaYUVBuffer = nullptr;
    m_channel.cudaBGRABuffer = nullptr;
    m_channel.hostMappedYUV = nullptr;
    m_channel.stream = nullptr;
    m_channel.preprocessEvent = nullptr;
    m_channel.inferenceEvent = nullptr;
    m_channel.bufferSize = 0;
    m_channel.channelID = -1;
    m_channel.width = 1920;  // Default 1080p (PTZ radar cameras)
    m_channel.height = 1080;
    m_channel.isActive = false;
}

DeckLinkCapture::~DeckLinkCapture() {
    Stop();
    
    // Release DeckLink input interface
    if (m_channel.deckLinkInput) {
        m_channel.deckLinkInput->Release();
        m_channel.deckLinkInput = nullptr;
    }
    
    // Release DeckLink device
    if (m_deckLink) {
        m_deckLink->Release();
        m_deckLink = nullptr;
    }
    
    // Destroy CUDA events
    if (m_channel.preprocessEvent) {
        cudaEventDestroy(m_channel.preprocessEvent);
        m_channel.preprocessEvent = nullptr;
    }
    if (m_channel.inferenceEvent) {
        cudaEventDestroy(m_channel.inferenceEvent);
        m_channel.inferenceEvent = nullptr;
    }
    
    // Destroy CUDA stream before freeing buffers
    if (m_channel.stream) {
        cudaStreamSynchronize(m_channel.stream);
        cudaStreamDestroy(m_channel.stream);
        m_channel.stream = nullptr;
    }
    
    // Release CUDA resources
    // Note: cudaYUVBuffer could be from allocator OR from fallback cudaMalloc
    // We need to check if it needs to be freed
    // FIX: Free cudaYUVBuffer if it was allocated in fallback path
    if (m_channel.cudaYUVBuffer && m_fallbackBufferAllocated) {
        cudaFree(m_channel.cudaYUVBuffer);
        m_channel.cudaYUVBuffer = nullptr;
        m_fallbackBufferAllocated = false;
    } else if (m_channel.cudaYUVBuffer) {
        m_channel.cudaYUVBuffer = nullptr;  // Just clear pointer, allocator owns the memory
    }
    if (m_channel.cudaBGRABuffer) {
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
    }
    
    // Release the allocator provider (decrements ref count)
    if (m_allocatorProvider) {
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
    }
}

bool DeckLinkCapture::Initialize(int deviceIndex, const std::string& channelName) {
    Logger::Info("Initializing DeckLink capture for device " + std::to_string(deviceIndex));
    
    m_channel.channelName = channelName;
    m_channel.channelID = deviceIndex;
    
    // Calculate buffer size for 4K YUV 4:2:2 (2 bytes per pixel)
    m_channel.bufferSize = m_channel.width * m_channel.height * 2;
    
    // Create zero-copy allocator provider for DeckLink buffers (SDK 15.3+ interface)
    m_allocatorProvider = new DeckLinkCudaAllocatorProvider();
    if (!m_allocatorProvider) {
        Logger::Error("Failed to create DeckLink CUDA allocator provider");
        return false;
    }
    m_allocatorProvider->AddRef(); // Initial reference
    
    // Allocate BGRA buffer (still regular GPU memory for now)
    // BGRA is 4 bytes per pixel
    size_t bgraBufferSize = m_channel.width * m_channel.height * 4;
    cudaError_t err = cudaMalloc(&m_channel.cudaBGRABuffer, bgraBufferSize);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate CUDA BGRA buffer: " + 
                     std::string(cudaGetErrorString(err)));
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }

    // Create dedicated CUDA stream for this channel
    err = cudaStreamCreateWithFlags(&m_channel.stream, cudaStreamNonBlocking);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create CUDA stream: " +
                      std::string(cudaGetErrorString(err)));
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    // Create CUDA events for async synchronization
    err = cudaEventCreateWithFlags(&m_channel.preprocessEvent, cudaEventDisableTiming);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create preprocess event: " +
                      std::string(cudaGetErrorString(err)));
        cudaStreamDestroy(m_channel.stream);
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    err = cudaEventCreateWithFlags(&m_channel.inferenceEvent, cudaEventDisableTiming);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create inference event: " +
                      std::string(cudaGetErrorString(err)));
        cudaEventDestroy(m_channel.preprocessEvent);
        cudaStreamDestroy(m_channel.stream);
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    Logger::Info("Allocated CUDA buffers: BGRA=" + std::to_string(bgraBufferSize) + " bytes");
    Logger::Info("Zero-copy allocator provider will handle YUV buffers dynamically");
    
    // Initialize Blackmagic SDK
    // 1. Create IDeckLink interface
    IDeckLinkIterator* deckLinkIterator = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CDeckLinkIterator,
        nullptr,
        CLSCTX_ALL,
        IID_IDeckLinkIterator,
        (void**)&deckLinkIterator
    );
    
    if (FAILED(hr) || !deckLinkIterator) {
        Logger::Error("Failed to create DeckLink Iterator. HRESULT: 0x" + std::to_string(hr));
        cudaEventDestroy(m_channel.inferenceEvent);
        cudaEventDestroy(m_channel.preprocessEvent);
        cudaStreamDestroy(m_channel.stream);
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    // 2. Iterate to the specified device index
    m_deckLink = nullptr;
    for (int i = 0; i <= deviceIndex; i++) {
        if (m_deckLink) {
            m_deckLink->Release();
            m_deckLink = nullptr;
        }
        hr = deckLinkIterator->Next(&m_deckLink);
        if (hr != S_OK) {
            Logger::Error("Failed to get DeckLink device at index " + std::to_string(deviceIndex));
            deckLinkIterator->Release();
            cudaEventDestroy(m_channel.inferenceEvent);
            cudaEventDestroy(m_channel.preprocessEvent);
            cudaStreamDestroy(m_channel.stream);
            cudaFree(m_channel.cudaBGRABuffer);
            m_channel.cudaBGRABuffer = nullptr;
            m_allocatorProvider->Release();
            m_allocatorProvider = nullptr;
            return false;
        }
    }
    deckLinkIterator->Release();
    
    if (!m_deckLink) {
        Logger::Error("DeckLink device is null");
        cudaEventDestroy(m_channel.inferenceEvent);
        cudaEventDestroy(m_channel.preprocessEvent);
        cudaStreamDestroy(m_channel.stream);
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    // 3. Query for IDeckLinkInput
    hr = m_deckLink->QueryInterface(IID_IDeckLinkInput, (void**)&m_channel.deckLinkInput);
    if (FAILED(hr) || !m_channel.deckLinkInput) {
        Logger::Error("Failed to get IDeckLinkInput interface. HRESULT: 0x" + std::to_string(hr));
        m_deckLink->Release();
        m_deckLink = nullptr;
        cudaEventDestroy(m_channel.inferenceEvent);
        cudaEventDestroy(m_channel.preprocessEvent);
        cudaStreamDestroy(m_channel.stream);
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    // 4. Set frame callback (this object implements IDeckLinkInputCallback)
    hr = m_channel.deckLinkInput->SetCallback(this);
    if (FAILED(hr)) {
        Logger::Error("Failed to set DeckLink callback. HRESULT: 0x" + std::to_string(hr));
        m_channel.deckLinkInput->Release();
        m_channel.deckLinkInput = nullptr;
        m_deckLink->Release();
        m_deckLink = nullptr;
        cudaEventDestroy(m_channel.inferenceEvent);
        cudaEventDestroy(m_channel.preprocessEvent);
        cudaStreamDestroy(m_channel.stream);
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        m_allocatorProvider->Release();
        m_allocatorProvider = nullptr;
        return false;
    }
    
    // 5. Set video input format to 1080p@30fps (bmdModeHD1080p30)
    // Using YUV 4:2:2 8-bit format for compatibility
    // SDK 15.3+ uses EnableVideoInputWithAllocatorProvider for zero-copy DMA
    hr = m_channel.deckLinkInput->EnableVideoInputWithAllocatorProvider(
        bmdModeHD1080p30,           // 1080p 30fps mode (for PTZ radar cameras)
        bmdFormat8BitYUV,           // YUV 4:2:2 8-bit
        bmdVideoInputFlagDefault,   // Default flags
        m_allocatorProvider         // Zero-copy allocator provider
    );
    if (FAILED(hr)) {
        // Fall back to standard EnableVideoInput if allocator provider is not supported
        Logger::Warning("EnableVideoInputWithAllocatorProvider failed (HRESULT: 0x" + 
                       std::to_string(hr) + "), falling back to standard path");
        hr = m_channel.deckLinkInput->EnableVideoInput(
            bmdModeHD1080p30,           // 1080p 30fps mode
            bmdFormat8BitYUV,           // YUV 4:2:2 8-bit
            bmdVideoInputFlagDefault    // Default flags
        );
        if (FAILED(hr)) {
            Logger::Error("Failed to enable video input. HRESULT: 0x" + std::to_string(hr));
            m_channel.deckLinkInput->Release();
            m_channel.deckLinkInput = nullptr;
            m_deckLink->Release();
            m_deckLink = nullptr;
            cudaFree(m_channel.cudaBGRABuffer);
            m_channel.cudaBGRABuffer = nullptr;
            cudaEventDestroy(m_channel.inferenceEvent);
            cudaEventDestroy(m_channel.preprocessEvent);
            cudaStreamDestroy(m_channel.stream);
            m_allocatorProvider->Release();
            m_allocatorProvider = nullptr;
            return false;
        }
    } else {
        Logger::Info("✓ Zero-copy allocator provider registered with DeckLink");
    }
    
    Logger::Info("DeckLink capture initialized successfully: " + channelName);
    return true;
}

bool DeckLinkCapture::Start() {
    if (!m_channel.deckLinkInput) {
        Logger::Error("Cannot start capture: DeckLink input not initialized");
        return false;
    }
    
    Logger::Info("Starting capture on channel: " + m_channel.channelName);
    
    // Start C++20 jthread for capture loop
    // The thread will automatically join on Stop() due to stop_token
    m_captureThread = std::jthread([this](std::stop_token st) {
        CaptureThreadFunc(st);
    });
    
    // Start DeckLink streaming
    HRESULT hr = m_channel.deckLinkInput->StartStreams();
    if (FAILED(hr)) {
        Logger::Error("Failed to start DeckLink streams. HRESULT: 0x" + std::to_string(hr));
        if (m_captureThread.joinable()) {
            m_captureThread.request_stop();
            m_frameCv.notify_all();
            m_captureThread.join();
        }
        return false;
    }
    
    m_channel.isActive = true;
    Logger::Info("DeckLink streaming started successfully");
    return true;
}

void DeckLinkCapture::Stop() {
    if (!m_channel.isActive) {
        return;
    }
    
    Logger::Info("Stopping capture on channel: " + m_channel.channelName);
    
    // Stop DeckLink streaming first
    if (m_channel.deckLinkInput) {
        HRESULT hr = m_channel.deckLinkInput->StopStreams();
        if (FAILED(hr)) {
            Logger::Warning("Failed to stop DeckLink streams. HRESULT: 0x" + std::to_string(hr));
        }
    }
    
    // Request thread stop via stop_token (C++20)
    if (m_captureThread.joinable()) {
        m_captureThread.request_stop();
        m_frameCv.notify_all();
        // Ensure the capture thread completes before freeing CUDA buffers
        m_captureThread.join();
    }
    
    // Ensure all GPU work for this channel is completed
    if (m_channel.stream) {
        cudaStreamSynchronize(m_channel.stream);
    }
    
    m_channel.isActive = false;
}

void DeckLinkCapture::SetFrameReadyHandler(std::function<void(const VideoChannel&, cudaStream_t)> handler) {
    m_frameReadyHandler = std::move(handler);
}

void DeckLinkCapture::ExecuteInference() {
    // Placeholder hook for TensorRT 10.x integration
    // TODO: once TensorRT is wired, propagate inference status (return value or channel state)
    // and add automated tests to validate the frame pipeline.
}

void DeckLinkCapture::CaptureThreadFunc(std::stop_token stopToken) {
    Logger::Info("Capture thread started for: " + m_channel.channelName);
    
    // Optimize thread for video capture (cores 0-3, high priority)
    ThreadOptimizer::OptimizeForVideoCapture();
    
    while (!stopToken.stop_requested()) {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_frameCv.wait(lock, [&] {
            return stopToken.stop_requested() || !m_frameQueue.empty();
        });

        if (stopToken.stop_requested() && m_frameQueue.empty()) {
            break;
        }

        if (!m_frameQueue.empty()) {
            IDeckLinkVideoInputFrame* frame = m_frameQueue.front();
            m_frameQueue.pop();
            lock.unlock();

            // Process frame (transfer to CUDA memory)
            ProcessFrame(frame);

            // Release frame reference if DeckLink uses COM-style lifetime
            if (frame) {
                frame->Release();
            }
        }
    }
    
    Logger::Info("Capture thread stopped for: " + m_channel.channelName);
}

void DeckLinkCapture::OnFrameArrived(IDeckLinkVideoInputFrame* videoFrame) {
    if (!videoFrame || !m_channel.isActive) {
        return;
    }
    // Producer: enqueue frame and wake capture thread
    videoFrame->AddRef(); // retain while in queue
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_frameQueue.push(videoFrame);
    }
    m_frameCv.notify_one();
}
/* ------------------------------------copilot agent revisar si la nueva implementacion cumple con zero copy optimo-----------------
void DeckLinkCapture::ProcessFrame(IDeckLinkVideoInputFrame* videoFrame) {
    if (!videoFrame || !m_channel.isActive) {
        return;
    }

    // Get frame data pointer from DeckLink
    void* frameBuffer = nullptr;
    videoFrame->GetBytes(&frameBuffer);

    if (!frameBuffer) {
        Logger::Error("DeckLink frame buffer is null");
        return;
    }

    // Copy frame data to CUDA device memory (zero-copy DMA path)
    // Using cudaMemcpyAsync for asynchronous transfer in per-channel stream
    cudaError_t err = cudaMemcpyAsync(
        m_channel.cudaYUVBuffer,           // Destination: CUDA device memory
        frameBuffer,                       // Source: DeckLink pinned memory
        m_channel.bufferSize,              // Size
        cudaMemcpyHostToDevice,            // Transfer direction
        m_channel.stream                   // Per-channel CUDA stream
    );

    if (err != cudaSuccess) {
        Logger::Error("Failed to copy frame to CUDA: " +
                     std::string(cudaGetErrorString(err)));
        return;
    }

    // Launch CUDA kernel for YUV422 (YUY2) to BGRA8 conversion
    if (!ConvertYUV422ToBGRA(
            static_cast<uint8_t*>(m_channel.cudaYUVBuffer),
            static_cast<uchar4*>(m_channel.cudaBGRABuffer),
            static_cast<int>(m_channel.width),
            static_cast<int>(m_channel.height),
            m_channel.stream)) {
        Logger::Error("Failed to launch YUV->BGRA CUDA kernel");
        return;
    }

    // Hook for TensorRT integration (must stay on-GPU)
    ExecuteInference();

    // Forward to output handler (e.g., Spout zero-copy path)
    if (m_frameReadyHandler) {
        m_frameReadyHandler(m_channel, m_channel.stream);
    }
}
-----------------------------------copilot agent revisar si la nueva implementacion cumple con zero copy optimo-----------------
*/

void DeckLinkCapture::ProcessFrame(IDeckLinkVideoInputFrame* videoFrame) {
    if (!videoFrame || !m_channel.isActive) return;

    // Get the buffer interface
    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    HRESULT hr = videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer);

    if (SUCCEEDED(hr) && videoBuffer) {
        void* frameBuffer = nullptr;

        // Lock access for safe reading
        if (videoBuffer->StartAccess(bmdBufferAccessRead) == S_OK) {
            // Get the pointer (works with GetBytes now)
            if (SUCCEEDED(videoBuffer->GetBytes(&frameBuffer)) && frameBuffer) {
                
                // TRUE ZERO-COPY PATH: Check if this is a mapped buffer from our allocator provider
                void* devicePtr = m_allocatorProvider->GetDevicePointer(frameBuffer);
                
                if (devicePtr) {
                    // ✓ ZERO-COPY: DeckLink wrote directly to GPU-mapped memory
                    // No cudaMemcpy needed! Use device pointer directly
                    m_channel.cudaYUVBuffer = devicePtr;
                    m_channel.hostMappedYUV = frameBuffer;
                    
                    // OPTIMIZATION: Skip BGRA conversion - inference uses UYVY directly
                    // Only convert if frame handler needs BGRA (e.g., for NDI/MegaCanvas)
                    // The fused kernel in InferenceEngine processes UYVY→RGB at configured dimensions
                    
                    // For now, still do BGRA conversion for output compatibility
                    // TODO: Add flag to skip if only inference is needed
                    if (ConvertYUV422ToBGRA(
                        static_cast<uint8_t*>(m_channel.cudaYUVBuffer),
                        static_cast<uchar4*>(m_channel.cudaBGRABuffer),
                        m_channel.width, m_channel.height, m_channel.stream))
                    {
                        // Record event after conversion (for output pipeline sync)
                        cudaEventRecord(m_channel.preprocessEvent, m_channel.stream);
                        
                        // Note: ExecuteInference is a placeholder
                        // Real inference now happens in main.cpp via ProcessFrameUYVY
                        ExecuteInference();
                        
                        if (m_frameReadyHandler) {
                            m_frameReadyHandler(m_channel, m_channel.stream);
                        }
                    }
                } else {
                    // Fallback path: Standard copy (if allocator wasn't used)
                    Logger::Warning("Frame buffer not from CUDA allocator provider, using fallback copy");
                    
                    // Need temporary device buffer
                    if (!m_channel.cudaYUVBuffer) {
                        cudaError_t allocErr = cudaMalloc(&m_channel.cudaYUVBuffer, m_channel.bufferSize);
                        if (allocErr != cudaSuccess) {
                            Logger::Error("Failed to allocate fallback YUV buffer: " + 
                                         std::string(cudaGetErrorString(allocErr)));
                            videoBuffer->EndAccess(bmdBufferAccessRead);
                            videoBuffer->Release();
                            return;
                        }
                        m_fallbackBufferAllocated = true;  // FIX: Mark that we allocated this buffer
                        Logger::Info("Allocated fallback YUV buffer (" + std::to_string(m_channel.bufferSize) + " bytes)");
                    }
                    
                    cudaError_t err = cudaMemcpyAsync(
                        m_channel.cudaYUVBuffer,
                        frameBuffer,
                        m_channel.bufferSize,
                        cudaMemcpyHostToDevice,
                        m_channel.stream
                    );

                    if (err == cudaSuccess) {
                        if (ConvertYUV422ToBGRA(
                            static_cast<uint8_t*>(m_channel.cudaYUVBuffer),
                            static_cast<uchar4*>(m_channel.cudaBGRABuffer),
                            m_channel.width, m_channel.height, m_channel.stream))
                        {
                            cudaEventRecord(m_channel.preprocessEvent, m_channel.stream);
                            ExecuteInference();
                            if (m_frameReadyHandler) m_frameReadyHandler(m_channel, m_channel.stream);
                        }
                    } else {
                        Logger::Error("Fallback cudaMemcpy failed: " + std::string(cudaGetErrorString(err)));
                    }
                }
            }
            videoBuffer->EndAccess(bmdBufferAccessRead);
        }
        videoBuffer->Release();
    }
}

// COM Interface Implementation (IUnknown)
HRESULT STDMETHODCALLTYPE DeckLinkCapture::QueryInterface(REFIID iid, LPVOID* ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    
    if (iid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == IID_IDeckLinkInputCallback) {
        *ppv = static_cast<IDeckLinkInputCallback*>(this);
        AddRef();
        return S_OK;
    }
    
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::Release() {
    ULONG newRefCount = --m_refCount;
    if (newRefCount == 0) {
        // Note: Don't delete this here as it's managed by unique_ptr elsewhere
        // This is a simplified implementation for callback interface
    }
    return newRefCount;
}

// IDeckLinkInputCallback Implementation
HRESULT STDMETHODCALLTYPE DeckLinkCapture::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents notificationEvents,
    IDeckLinkDisplayMode* newDisplayMode,
    BMDDetectedVideoInputFormatFlags detectedSignalFlags)
{
    // Log format change but don't handle it for now
    Logger::Info("Video input format changed on channel: " + m_channel.channelName);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame* videoFrame,
    IDeckLinkAudioInputPacket* audioPacket)
{
    // Forward frame to our internal handler
    if (videoFrame) {
        OnFrameArrived(videoFrame);
    }
    return S_OK;
}

int DeckLinkCapture::EnumerateDevices() {
    Logger::Info("Enumerating DeckLink devices...");
    
    // Create DeckLink iterator using COM
    IDeckLinkIterator* deckLinkIterator = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_CDeckLinkIterator,
        nullptr,
        CLSCTX_ALL,
        IID_IDeckLinkIterator,
        (void**)&deckLinkIterator
    );
    
    if (FAILED(hr)) {
        Logger::Error("Failed to create DeckLink Iterator. HRESULT: 0x" + std::to_string(hr));
        Logger::Error("Ensure Blackmagic Desktop Video is installed and DeckLinkAPI64.dll is registered");
        return 0;
    }
    
    if (!deckLinkIterator) {
        Logger::Error("DeckLink Iterator is null despite successful CoCreateInstance");
        return 0;
    }
    
    Logger::Info("DeckLink Iterator created successfully");
    
    // Enumerate all DeckLink devices
    IDeckLink* deckLink = nullptr;
    int deviceCount = 0;
    
    while (deckLinkIterator->Next(&deckLink) == S_OK) {
        if (deckLink) {
            deviceCount++;
            
            // Get device name
            BSTR deviceNameBSTR = nullptr;
            if (deckLink->GetDisplayName(&deviceNameBSTR) == S_OK) {
                // Convert BSTR to std::string
                char deviceName[256];
                size_t convertedChars = 0;
                wcstombs_s(&convertedChars, deviceName, 256, deviceNameBSTR, 255);
                
                Logger::Info("Found DeckLink device #" + std::to_string(deviceCount) + 
                           ": " + std::string(deviceName));
                
                SysFreeString(deviceNameBSTR);
            }
            
            // Release device interface
            deckLink->Release();
            deckLink = nullptr;
        }
    }
    
    // Release iterator
    deckLinkIterator->Release();
    
    if (deviceCount == 0) {
        Logger::Warning("No DeckLink devices found. Check hardware connections and drivers.");
    } else {
        Logger::Info("Total DeckLink devices found: " + std::to_string(deviceCount));
    }
    
    return deviceCount;
}

// =============================================================================
// DeckLinkCudaVideoBuffer Implementation
// Implements IDeckLinkVideoBuffer using CUDA pinned mapped memory
// =============================================================================

DeckLinkCapture::DeckLinkCudaVideoBuffer::DeckLinkCudaVideoBuffer(void* buffer, unsigned int size)
    : m_refCount(1)
    , m_buffer(buffer)
    , m_size(size)
    , m_devicePtr(nullptr)
{
    // Pre-cache device pointer for zero-copy access
    if (m_buffer) {
        cudaError_t err = cudaHostGetDevicePointer(&m_devicePtr, m_buffer, 0);
        if (err != cudaSuccess) {
            Logger::Warning("DeckLinkCudaVideoBuffer: Failed to get device pointer: " + 
                           std::string(cudaGetErrorString(err)));
        }
    }
}

DeckLinkCapture::DeckLinkCudaVideoBuffer::~DeckLinkCudaVideoBuffer() {
    // Buffer memory is owned by the allocator, not the buffer object
    // Just clear our references
    m_buffer = nullptr;
    m_devicePtr = nullptr;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaVideoBuffer::QueryInterface(REFIID iid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    
    if (iid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == IID_IDeckLinkVideoBuffer) {
        *ppv = static_cast<IDeckLinkVideoBuffer*>(this);
        AddRef();
        return S_OK;
    }
    
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaVideoBuffer::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaVideoBuffer::Release() {
    ULONG newRefCount = --m_refCount;
    if (newRefCount == 0) {
        delete this;
    }
    return newRefCount;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaVideoBuffer::GetBytes(void** buffer) {
    if (!buffer) return E_POINTER;
    *buffer = m_buffer;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaVideoBuffer::StartAccess(BMDBufferAccessFlags flags) {
    // For CUDA pinned memory, no special access handling needed
    // Memory is always accessible from both CPU and GPU
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaVideoBuffer::EndAccess(BMDBufferAccessFlags flags) {
    // For CUDA pinned memory, no special cleanup needed
    return S_OK;
}

void* DeckLinkCapture::DeckLinkCudaVideoBuffer::GetDevicePointer() {
    return m_devicePtr;
}

// =============================================================================
// DeckLinkCudaBufferAllocator Implementation
// Implements IDeckLinkVideoBufferAllocator for specific buffer formats
// =============================================================================

DeckLinkCapture::DeckLinkCudaBufferAllocator::DeckLinkCudaBufferAllocator(
    unsigned int bufferSize, unsigned int width, unsigned int height, 
    unsigned int rowBytes, BMDPixelFormat pixelFormat)
    : m_refCount(1)
    , m_bufferSize(bufferSize)
    , m_width(width)
    , m_height(height)
    , m_rowBytes(rowBytes)
    , m_pixelFormat(pixelFormat)
{
    Logger::Info("DeckLinkCudaBufferAllocator: Created for " + std::to_string(width) + "x" + 
                std::to_string(height) + " format=" + std::to_string(pixelFormat));
}

DeckLinkCapture::DeckLinkCudaBufferAllocator::~DeckLinkCudaBufferAllocator() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Free all allocated buffers
    for (void* buffer : m_allocatedBuffers) {
        if (buffer) {
            cudaFreeHost(buffer);
        }
    }
    m_allocatedBuffers.clear();
    Logger::Info("DeckLinkCudaBufferAllocator: Destroyed, freed all buffers");
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaBufferAllocator::QueryInterface(REFIID iid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    
    if (iid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == IID_IDeckLinkVideoBufferAllocator) {
        *ppv = static_cast<IDeckLinkVideoBufferAllocator*>(this);
        AddRef();
        return S_OK;
    }
    
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaBufferAllocator::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaBufferAllocator::Release() {
    ULONG newRefCount = --m_refCount;
    if (newRefCount == 0) {
        delete this;
    }
    return newRefCount;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaBufferAllocator::AllocateVideoBuffer(
    IDeckLinkVideoBuffer** allocatedBuffer)
{
    if (!allocatedBuffer) return E_POINTER;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Allocate CUDA host memory with cudaHostAllocMapped flag
    // This creates memory that is:
    // 1. Pinned (page-locked) for fast DMA
    // 2. Mapped into GPU address space for zero-copy access
    void* hostPtr = nullptr;
    cudaError_t err = cudaHostAlloc(&hostPtr, m_bufferSize, cudaHostAllocMapped);
    
    if (err != cudaSuccess || !hostPtr) {
        Logger::Error("DeckLinkCudaBufferAllocator: Failed to allocate mapped memory: " + 
                     std::string(cudaGetErrorString(err)));
        return E_OUTOFMEMORY;
    }
    
    // Create video buffer wrapper first, then track the allocation
    // This ensures proper cleanup if the constructor fails
    DeckLinkCudaVideoBuffer* videoBuffer = nullptr;
    try {
        videoBuffer = new DeckLinkCudaVideoBuffer(hostPtr, m_bufferSize);
    } catch (...) {
        cudaFreeHost(hostPtr);
        Logger::Error("DeckLinkCudaBufferAllocator: Failed to create video buffer wrapper");
        return E_OUTOFMEMORY;
    }
    
    // Track allocation after successful buffer creation
    m_allocatedBuffers.push_back(hostPtr);
    *allocatedBuffer = videoBuffer;
    
    Logger::Info("DeckLinkCudaBufferAllocator: Allocated " + std::to_string(m_bufferSize) + 
                " bytes of CUDA mapped memory");
    
    return S_OK;
}

bool DeckLinkCapture::DeckLinkCudaBufferAllocator::IsOurBuffer(void* ptr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::find(m_allocatedBuffers.begin(), m_allocatedBuffers.end(), ptr) != m_allocatedBuffers.end();
}

void* DeckLinkCapture::DeckLinkCudaBufferAllocator::GetDevicePointer(void* hostPtr) {
    if (!hostPtr) return nullptr;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Check if this is one of our allocated buffers (inline check to avoid deadlock)
    auto it = std::find(m_allocatedBuffers.begin(), m_allocatedBuffers.end(), hostPtr);
    if (it == m_allocatedBuffers.end()) {
        return nullptr;
    }
    
    // Get the device pointer for this mapped host memory
    void* devicePtr = nullptr;
    cudaError_t err = cudaHostGetDevicePointer(&devicePtr, hostPtr, 0);
    
    if (err != cudaSuccess) {
        Logger::Error("DeckLinkCudaBufferAllocator: Failed to get device pointer: " + 
                     std::string(cudaGetErrorString(err)));
        return nullptr;
    }
    
    return devicePtr;
}

// =============================================================================
// DeckLinkCudaAllocatorProvider Implementation
// Implements IDeckLinkVideoBufferAllocatorProvider (SDK 15.3+ interface)
// =============================================================================

DeckLinkCapture::DeckLinkCudaAllocatorProvider::DeckLinkCudaAllocatorProvider()
    : m_refCount(1)
{
    Logger::Info("DeckLinkCudaAllocatorProvider: Initialized for DeckLink SDK 15.3+ zero-copy DMA");
}

DeckLinkCapture::DeckLinkCudaAllocatorProvider::~DeckLinkCudaAllocatorProvider() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Release all allocators
    for (auto* allocator : m_allocators) {
        if (allocator) {
            allocator->Release();
        }
    }
    m_allocators.clear();
    Logger::Info("DeckLinkCudaAllocatorProvider: Destroyed");
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaAllocatorProvider::QueryInterface(REFIID iid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    
    if (iid == IID_IUnknown) {
        *ppv = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    if (iid == IID_IDeckLinkVideoBufferAllocatorProvider) {
        *ppv = static_cast<IDeckLinkVideoBufferAllocatorProvider*>(this);
        AddRef();
        return S_OK;
    }
    
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaAllocatorProvider::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaAllocatorProvider::Release() {
    ULONG newRefCount = --m_refCount;
    if (newRefCount == 0) {
        delete this;
    }
    return newRefCount;
}

HRESULT STDMETHODCALLTYPE DeckLinkCapture::DeckLinkCudaAllocatorProvider::GetVideoBufferAllocator(
    unsigned int bufferSize,
    unsigned int width,
    unsigned int height,
    unsigned int rowBytes,
    BMDPixelFormat pixelFormat,
    IDeckLinkVideoBufferAllocator** allocator)
{
    if (!allocator) return E_POINTER;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Create new allocator for this format
    DeckLinkCudaBufferAllocator* newAllocator = new DeckLinkCudaBufferAllocator(
        bufferSize, width, height, rowBytes, pixelFormat);
    
    // Track the allocator
    m_allocators.push_back(newAllocator);
    newAllocator->AddRef();  // Extra ref for our tracking
    
    *allocator = newAllocator;
    
    Logger::Info("DeckLinkCudaAllocatorProvider: Created allocator for " + 
                std::to_string(width) + "x" + std::to_string(height) + 
                " bufferSize=" + std::to_string(bufferSize));
    
    return S_OK;
}

void* DeckLinkCapture::DeckLinkCudaAllocatorProvider::GetDevicePointer(void* hostPtr) {
    if (!hostPtr) return nullptr;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Search all allocators for this buffer
    // Note: Locking order is always provider mutex → allocator mutex (no deadlock)
    for (auto* allocator : m_allocators) {
        if (allocator) {
            // GetDevicePointer will check IsOurBuffer internally and return nullptr if not found
            void* devicePtr = allocator->GetDevicePointer(hostPtr);
            if (devicePtr) {
                return devicePtr;
            }
        }
    }
    
    return nullptr;
}

bool DeckLinkCapture::DeckLinkCudaAllocatorProvider::IsOurBuffer(void* ptr) {
    if (!ptr) return false;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Locking order is always provider mutex → allocator mutex (no deadlock)
    for (auto* allocator : m_allocators) {
        if (allocator && allocator->IsOurBuffer(ptr)) {
            return true;
        }
    }
    
    return false;
}
