/**
 * DeckLinkCapture.cpp
 * 
 * Implementation of zero-copy DMA capture from Blackmagic DeckLink cards
 * Uses CUDA cudaMallocPinned for direct GPU memory access on RTX 5080
 * No DirectX dependencies - pure CUDA pipeline
 */
#include <winsock2.h>  // Si usas red
#include <windows.h>   // BASE DE WINDOWS
#include <objbase.h>    // NECESARIO PARA COM/INTERFACES

#include "DeckLinkAPI_h.h" 
#include "DeckLinkCapture.h"
#include "CudaColorConversion.h"
#include "../utils/Logger.h"
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
    : m_allocator(nullptr)
{
    // Initialize channel structure
    m_channel.deckLinkInput = nullptr;
    m_channel.cudaYUVBuffer = nullptr;
    m_channel.cudaBGRABuffer = nullptr;
    m_channel.stream = nullptr;
    m_channel.bufferSize = 0;
    m_channel.channelID = -1;
    m_channel.width = 3840;  // Default 4K
    m_channel.height = 2160;
    m_channel.isActive = false;
}

DeckLinkCapture::~DeckLinkCapture() {
    Stop();
    
    // Destroy CUDA stream before freeing buffers
    if (m_channel.stream) {
        cudaStreamSynchronize(m_channel.stream);
        cudaStreamDestroy(m_channel.stream);
        m_channel.stream = nullptr;
    }
    
    // Release CUDA resources
    if (m_channel.cudaYUVBuffer) {
        cudaFree(m_channel.cudaYUVBuffer);
        m_channel.cudaYUVBuffer = nullptr;
    }
    if (m_channel.cudaBGRABuffer) {
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
    }
}

bool DeckLinkCapture::Initialize(int deviceIndex, const std::string& channelName) {
    Logger::Info("Initializing DeckLink capture for device " + std::to_string(deviceIndex));
    
    m_channel.channelName = channelName;
    m_channel.channelID = deviceIndex;
    
    // Calculate buffer size for 4K YUV 4:2:2 (2 bytes per pixel)
    m_channel.bufferSize = m_channel.width * m_channel.height * 2;
    
    // Allocate CUDA device memory for YUV input (from DeckLink)
    cudaError_t err = cudaMalloc(&m_channel.cudaYUVBuffer, m_channel.bufferSize);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate CUDA YUV buffer: " + 
                     std::string(cudaGetErrorString(err)));
        return false;
    }
    
    // Allocate CUDA device memory for BGRA output (for TensorRT/Spout)
    // BGRA is 4 bytes per pixel
    size_t bgraBufferSize = m_channel.width * m_channel.height * 4;
    err = cudaMalloc(&m_channel.cudaBGRABuffer, bgraBufferSize);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate CUDA BGRA buffer: " + 
                     std::string(cudaGetErrorString(err)));
        cudaFree(m_channel.cudaYUVBuffer);
        m_channel.cudaYUVBuffer = nullptr;
        return false;
    }

    // Create dedicated CUDA stream for this channel
    err = cudaStreamCreateWithFlags(&m_channel.stream, cudaStreamNonBlocking);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create CUDA stream: " +
                      std::string(cudaGetErrorString(err)));
        cudaFree(m_channel.cudaBGRABuffer);
        m_channel.cudaBGRABuffer = nullptr;
        cudaFree(m_channel.cudaYUVBuffer);
        m_channel.cudaYUVBuffer = nullptr;
        return false;
    }
    
    Logger::Info("Allocated CUDA buffers: YUV=" + std::to_string(m_channel.bufferSize) + 
                 " bytes, BGRA=" + std::to_string(bgraBufferSize) + " bytes");
    
    // TODO: Initialize Blackmagic SDK
    // 1. Create IDeckLink interface
    // 2. Query for IDeckLinkInput
    // 3. Set video input format (bmdModeHD1080p60 or bmdMode4K2160p60)
    // 4. Create custom allocator
    // 5. Set frame callback (IDeckLinkInputCallback interface)
    
    // Initialize custom allocator with cudaMallocPinned
    m_allocator = std::make_unique<CustomAllocator>();
    
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
    
    // TODO: Start DeckLink streaming
    // m_channel.deckLinkInput->StartStreams();
    
    m_channel.isActive = true;
    return true;
}

void DeckLinkCapture::Stop() {
    if (!m_channel.isActive) {
        return;
    }
    
    Logger::Info("Stopping capture on channel: " + m_channel.channelName);
    
    // Request thread stop via stop_token (C++20)
    if (m_captureThread.joinable()) {
        m_captureThread.request_stop();
        m_frameCv.notify_all();
        // Ensure the capture thread completes before freeing CUDA buffers
        m_captureThread.join();
    }
    
    // TODO: Stop DeckLink streaming
    // if (m_channel.deckLinkInput) {
    //     m_channel.deckLinkInput->StopStreams();
    // }
    
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

    // 1. Obtener la interfaz de buffer (Necesario en Windows para acceder a los bytes)
    IDeckLinkVideoBuffer* videoBuffer = nullptr;
    HRESULT hr = videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer);

    if (SUCCEEDED(hr) && videoBuffer) {
        void* frameBuffer = nullptr;

        // 2. Bloquear acceso para lectura segura
        if (videoBuffer->StartAccess(bmdBufferAccessRead) == S_OK) {
            // 3. Obtener el puntero (Ahora sí funciona GetBytes)
            if (SUCCEEDED(videoBuffer->GetBytes(&frameBuffer)) && frameBuffer) {

                // 4. Transferencia Directa a VRAM (DMA)
                // Al usar cudaMallocHost en tu CustomAllocator, esta copia es ultra rápida
                cudaError_t err = cudaMemcpyAsync(
                    m_channel.cudaYUVBuffer,
                    frameBuffer,
                    m_channel.bufferSize,
                    cudaMemcpyHostToDevice,
                    m_channel.stream
                );

                if (err == cudaSuccess) {
                    // 5. Conversión de color en GPU (Sin tocar la CPU)
                    if (ConvertYUV422ToBGRA(
                        static_cast<uint8_t*>(m_channel.cudaYUVBuffer),
                        static_cast<uchar4*>(m_channel.cudaBGRABuffer),
                        m_channel.width, m_channel.height, m_channel.stream))
                    {
                        ExecuteInference(); // YOLO en RTX 2050
                        if (m_frameReadyHandler) m_frameReadyHandler(m_channel, m_channel.stream);
                    }
                }
            }
            videoBuffer->EndAccess(bmdBufferAccessRead);
        }
        videoBuffer->Release(); // Liberar interfaz COM
    }
}

int DeckLinkCapture::EnumerateDevices() {
    // TODO: Enumerate DeckLink devices
    // Use IDeckLinkIterator to find all available devices
    Logger::Info("Enumerating DeckLink devices...");
    
    // Placeholder: return number of detected devices
    return 0;
}

// CustomAllocator implementation
// Uses CUDA cudaMallocPinned for zero-copy DMA with DeckLink cards
DeckLinkCapture::CustomAllocator::CustomAllocator() {
    Logger::Info("CustomAllocator initialized with CUDA pinned memory");
}

DeckLinkCapture::CustomAllocator::~CustomAllocator() {
    // Free all pinned buffers
    for (void* buffer : m_pinnedBuffers) {
        if (buffer) {
            cudaFreeHost(buffer);
        }
    }
    m_pinnedBuffers.clear();
}

void* DeckLinkCapture::CustomAllocator::AllocateBuffer(unsigned int bufferSize) {
    // Allocate pinned (page-locked) host memory that GPU can access directly
    // This is the key to zero-copy DMA performance on PCIe
    // cudaMallocPinned provides host memory that is mapped into the CUDA address space
    
    void* buffer = nullptr;
    cudaError_t err = cudaMallocHost(&buffer, bufferSize);
    
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate pinned memory: " + 
                     std::string(cudaGetErrorString(err)));
        return nullptr;
    }
    
    if (buffer) {
        m_pinnedBuffers.push_back(buffer);
        Logger::Info("Allocated " + std::to_string(bufferSize) + 
                    " bytes of CUDA pinned memory");
    }
    
    return buffer;
}

void DeckLinkCapture::CustomAllocator::ReleaseBuffer(void* buffer) {
    if (!buffer) {
        return;
    }
    
    // Free CUDA pinned host memory
    cudaError_t err = cudaFreeHost(buffer);
    if (err != cudaSuccess) {
        Logger::Error("Failed to free pinned memory: " + 
                     std::string(cudaGetErrorString(err)));
    }
    
    // Remove from tracked buffers
    auto it = std::find(m_pinnedBuffers.begin(), m_pinnedBuffers.end(), buffer);
    if (it != m_pinnedBuffers.end()) {
        m_pinnedBuffers.erase(it);
    }
}
