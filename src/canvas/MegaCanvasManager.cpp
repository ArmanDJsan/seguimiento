/**
 * MegaCanvasManager.cpp
 * 
 * Implementation of 16K Mega-Canvas compositor
 * Handles VRAM bifurcation, atlas composition, and DXGI presentation
 */

#include "MegaCanvasManager.h"
#include "DXGIPresenter.h"
#include "AtlasCompositor.h"
#include "../utils/Logger.h"
#include "../utils/ThreadOptimizer.h"
#include "../json.hpp"

#include <cuda_d3d11_interop.h>
#include <chrono>
#include <fstream>

using json = nlohmann::json;

namespace {
    // VRAM size calculations
    constexpr size_t kAtlasSize = 
        static_cast<size_t>(MegaCanvasManager::CANVAS_WIDTH) * 
        MegaCanvasManager::CANVAS_HEIGHT * 4;  // BGRA = 4 bytes/pixel
    
    constexpr size_t kTensorRTBufferSize = 
        static_cast<size_t>(MegaCanvasManager::TENSORRT_WIDTH) * 
        MegaCanvasManager::TENSORRT_HEIGHT * 4;
    
    constexpr size_t kCellSize = 
        static_cast<size_t>(MegaCanvasManager::CELL_WIDTH) * 
        MegaCanvasManager::CELL_HEIGHT * 4;
}

MegaCanvasManager::MegaCanvasManager() {
    Logger::Info("MegaCanvasManager: Created");
    Logger::Info("  Canvas: " + std::to_string(CANVAS_WIDTH) + "x" + std::to_string(CANVAS_HEIGHT) + " (16K)");
    Logger::Info("  Cell: " + std::to_string(CELL_WIDTH) + "x" + std::to_string(CELL_HEIGHT) + " (4K native)");
    Logger::Info("  Grid: " + std::to_string(GRID_COLS) + "x" + std::to_string(GRID_ROWS));
    Logger::Info("  TensorRT: " + std::to_string(TENSORRT_WIDTH) + "x" + std::to_string(TENSORRT_HEIGHT));
    Logger::Info("  Estimated VRAM: " + std::to_string((kAtlasSize + NUM_CAMERAS * (kCellSize + kTensorRTBufferSize)) / (1024*1024)) + " MB");
}

MegaCanvasManager::~MegaCanvasManager() {
    Shutdown();
}

bool MegaCanvasManager::Initialize() {
    if (m_initialized.load(std::memory_order_acquire)) {
        Logger::Warning("MegaCanvasManager: Already initialized");
        return true;
    }

    Logger::Info("MegaCanvasManager: Initializing...");

    // Create DXGI presenter first
    m_presenter = std::make_unique<DXGIPresenter>();
    DXGIPresenter::Config presenterConfig;
    presenterConfig.width = CANVAS_WIDTH;
    presenterConfig.height = CANVAS_HEIGHT;
    presenterConfig.bufferCount = 2;
    presenterConfig.enableTearing = true;
    presenterConfig.enableWaitableObject = true;
    presenterConfig.borderless = true;
    presenterConfig.noRedirectionBitmap = true;
    presenterConfig.windowTitle = L"VIB MegaCanvas 16K (vMix WGC)";
    
    // Hidden mode (Ghost Window) configuration
    // Window positioned off-screen at virtual coordinates
    // Invisible to operator but vMix captures via WGC
    presenterConfig.hiddenMode = true;
    presenterConfig.mousePassthrough = true;     // Click-through
    presenterConfig.hideFromTaskbar = true;      // Not visible in taskbar/Alt+Tab
    presenterConfig.excludeFromCapture = false;  // Keep false - vMix needs WGC access
    presenterConfig.virtualX = -15360;           // Off-screen left (WGC compatible)
    presenterConfig.virtualY = 0;                // Off-screen Y

    if (!m_presenter->Initialize(presenterConfig)) {
        Logger::Error("MegaCanvasManager: Failed to initialize DXGI presenter");
        return false;
    }

    // Create CUDA stream for composition (high priority)
    int leastPriority, greatestPriority;
    cudaDeviceGetStreamPriorityRange(&leastPriority, &greatestPriority);
    
    cudaError_t err = cudaStreamCreateWithPriority(
        &m_compositeStream, 
        cudaStreamNonBlocking, 
        greatestPriority
    );
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to create composition stream: " + 
                     std::string(cudaGetErrorString(err)));
        return false;
    }

    // Create composition complete event
    err = cudaEventCreateWithFlags(&m_compositeComplete, cudaEventDisableTiming);
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to create composition event");
        cudaStreamDestroy(m_compositeStream);
        return false;
    }

    // Allocate 16K Atlas buffer
    m_atlasBufferSize = kAtlasSize;
    err = cudaMalloc(&m_atlasBuffer, m_atlasBufferSize);
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to allocate Atlas buffer (" + 
                     std::to_string(m_atlasBufferSize / (1024*1024)) + " MB): " +
                     std::string(cudaGetErrorString(err)));
        cudaEventDestroy(m_compositeComplete);
        cudaStreamDestroy(m_compositeStream);
        return false;
    }

    // Clear Atlas to black
    cudaMemset(m_atlasBuffer, 0, m_atlasBufferSize);

    // Setup CUDA/DX11 interop
    if (!SetupCudaDXInterop()) {
        Logger::Error("MegaCanvasManager: Failed to setup CUDA/DX11 interop");
        cudaFree(m_atlasBuffer);
        cudaEventDestroy(m_compositeComplete);
        cudaStreamDestroy(m_compositeStream);
        return false;
    }

    // Allocate device memory for camera info structs
    err = cudaMalloc(&m_deviceCameraInfos, sizeof(CameraSlot) * NUM_CAMERAS);
    if (err != cudaSuccess) {
        Logger::Warning("MegaCanvasManager: Failed to allocate camera info buffer (non-critical)");
        m_deviceCameraInfos = nullptr;
    }

    m_initialized.store(true, std::memory_order_release);
    Logger::Info("MegaCanvasManager: Initialized successfully");
    Logger::Info("  Atlas buffer: " + std::to_string(m_atlasBufferSize / (1024*1024)) + " MB");

    return true;
}

void MegaCanvasManager::Shutdown() {
    Logger::Info("MegaCanvasManager: Shutting down...");

    // Stop render thread first
    StopRenderThread();

    m_initialized.store(false, std::memory_order_release);

    // Teardown CUDA/DX interop
    TeardownCudaDXInterop();

    // Free camera buffers
    for (int i = 0; i < NUM_CAMERAS; i++) {
        FreeCameraBuffers(i);
    }

    // Free device camera infos
    if (m_deviceCameraInfos) {
        cudaFree(m_deviceCameraInfos);
        m_deviceCameraInfos = nullptr;
    }

    // Free Atlas buffer
    if (m_atlasBuffer) {
        cudaFree(m_atlasBuffer);
        m_atlasBuffer = nullptr;
    }

    // Destroy composition event and stream
    if (m_compositeComplete) {
        cudaEventDestroy(m_compositeComplete);
        m_compositeComplete = nullptr;
    }

    if (m_compositeStream) {
        cudaStreamDestroy(m_compositeStream);
        m_compositeStream = nullptr;
    }

    // Shutdown presenter
    if (m_presenter) {
        m_presenter->Shutdown();
        m_presenter.reset();
    }

    Logger::Info("MegaCanvasManager: Shutdown complete");
}

bool MegaCanvasManager::RegisterCamera(int cameraID, unsigned int width, unsigned int height) {
    if (cameraID < 0 || cameraID >= NUM_CAMERAS) {
        Logger::Error("MegaCanvasManager: Invalid camera ID: " + std::to_string(cameraID));
        return false;
    }

    auto& cam = m_cameras[cameraID];
    if (cam.isRegistered) {
        if (cam.width == width && cam.height == height) {
            return true;  // Already registered with same dimensions
        }
        FreeCameraBuffers(cameraID);  // Re-register with different dimensions
    }

    return AllocateCameraBuffers(cameraID, width, height);
}

bool MegaCanvasManager::AllocateCameraBuffers(int cameraID, unsigned int width, unsigned int height) {
    auto& cam = m_cameras[cameraID];
    
    // Calculate buffer sizes
    cam.bgraBufferSize = static_cast<size_t>(width) * height * 4;
    cam.tensorRTBufferSize = kTensorRTBufferSize;
    cam.width = width;
    cam.height = height;

    // Allocate 4K BGRA buffer
    cudaError_t err = cudaMalloc(&cam.cudaBGRABuffer, cam.bgraBufferSize);
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to allocate BGRA buffer for camera " + 
                     std::to_string(cameraID) + ": " + cudaGetErrorString(err));
        return false;
    }

    // Allocate TensorRT downscale buffer (640x640)
    err = cudaMalloc(&cam.cudaTensorRTBuffer, cam.tensorRTBufferSize);
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to allocate TensorRT buffer for camera " + 
                     std::to_string(cameraID));
        cudaFree(cam.cudaBGRABuffer);
        cam.cudaBGRABuffer = nullptr;
        return false;
    }

    // Create frame ready event
    err = cudaEventCreateWithFlags(&cam.frameReady, cudaEventDisableTiming);
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to create frame event for camera " + 
                     std::to_string(cameraID));
        cudaFree(cam.cudaTensorRTBuffer);
        cudaFree(cam.cudaBGRABuffer);
        cam.cudaTensorRTBuffer = nullptr;
        cam.cudaBGRABuffer = nullptr;
        return false;
    }

    // Create downscale complete event
    err = cudaEventCreateWithFlags(&cam.downscaleComplete, cudaEventDisableTiming);
    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: Failed to create downscale event for camera " + 
                     std::to_string(cameraID));
        cudaEventDestroy(cam.frameReady);
        cudaFree(cam.cudaTensorRTBuffer);
        cudaFree(cam.cudaBGRABuffer);
        cam.frameReady = nullptr;
        cam.cudaTensorRTBuffer = nullptr;
        cam.cudaBGRABuffer = nullptr;
        return false;
    }

    cam.isRegistered = true;
    cam.frameID.store(0, std::memory_order_release);
    cam.hasNewFrame.store(false, std::memory_order_release);
    cam.droppedFrames.store(0, std::memory_order_release);
    cam.processedFrames.store(0, std::memory_order_release);

    Logger::Info("MegaCanvasManager: Camera " + std::to_string(cameraID) + " registered (" +
                 std::to_string(width) + "x" + std::to_string(height) + ")");

    return true;
}

void MegaCanvasManager::FreeCameraBuffers(int cameraID) {
    if (cameraID < 0 || cameraID >= NUM_CAMERAS) return;

    auto& cam = m_cameras[cameraID];

    if (cam.downscaleComplete) {
        cudaEventDestroy(cam.downscaleComplete);
        cam.downscaleComplete = nullptr;
    }

    if (cam.frameReady) {
        cudaEventDestroy(cam.frameReady);
        cam.frameReady = nullptr;
    }

    if (cam.cudaTensorRTBuffer) {
        cudaFree(cam.cudaTensorRTBuffer);
        cam.cudaTensorRTBuffer = nullptr;
    }

    if (cam.cudaBGRABuffer) {
        cudaFree(cam.cudaBGRABuffer);
        cam.cudaBGRABuffer = nullptr;
    }

    cam.isRegistered = false;
    cam.width = 0;
    cam.height = 0;
}

void MegaCanvasManager::UpdateCameraFrame(int cameraID, void* cudaBGRASource,
                                          unsigned int width, unsigned int height,
                                          cudaStream_t sourceStream) {
    if (cameraID < 0 || cameraID >= NUM_CAMERAS) return;
    if (!m_initialized.load(std::memory_order_acquire)) return;

    auto& cam = m_cameras[cameraID];
    if (!cam.isRegistered) {
        // Auto-register camera
        if (!RegisterCamera(cameraID, width, height)) {
            return;
        }
    }

    // Check if previous frame was consumed
    if (cam.hasNewFrame.load(std::memory_order_acquire)) {
        cam.droppedFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // Calculate Atlas position for this camera
    int gridX = cameraID % GRID_COLS;
    int gridY = cameraID / GRID_COLS;
    int destX = gridX * CELL_WIDTH;
    int destY = gridY * CELL_HEIGHT;

    // VRAM Bifurcation: Copy to Atlas AND downscale for TensorRT in one kernel
    BifurcatedCopy(
        cudaBGRASource,
        m_atlasBuffer,
        cam.cudaTensorRTBuffer,
        width,
        height,
        CANVAS_WIDTH,
        destX,
        destY,
        TENSORRT_WIDTH,
        TENSORRT_HEIGHT,
        sourceStream
    );

    // Record frame ready event
    cudaEventRecord(cam.frameReady, sourceStream);

    // Update frame tracking
    cam.frameID.fetch_add(1, std::memory_order_release);
    cam.hasNewFrame.store(true, std::memory_order_release);
    cam.processedFrames.fetch_add(1, std::memory_order_relaxed);
}

void* MegaCanvasManager::GetTensorRTBuffer(int cameraID) const {
    if (cameraID < 0 || cameraID >= NUM_CAMERAS) return nullptr;
    return m_cameras[cameraID].cudaTensorRTBuffer;
}

bool MegaCanvasManager::ComposeAtlas(cudaStream_t compositeStream) {
    if (!m_initialized.load(std::memory_order_acquire)) return false;

    // Latest-Frame-Available pattern:
    // We don't wait for all cameras - just process what's available
    // The atlas already contains the previous frame data, so cameras
    // without new data will display their last known frame

    int camerasReady = 0;
    for (int i = 0; i < NUM_CAMERAS; i++) {
        auto& cam = m_cameras[i];
        if (cam.isRegistered && cam.hasNewFrame.load(std::memory_order_acquire)) {
            // Wait for this camera's frame to be ready (non-blocking query first)
            cudaError_t err = cudaEventQuery(cam.frameReady);
            if (err == cudaSuccess) {
                // Frame is ready - it's already been blitted in UpdateCameraFrame
                cam.hasNewFrame.store(false, std::memory_order_release);
                camerasReady++;
            } else if (err == cudaErrorNotReady) {
                // Frame still processing - use previous frame
                // Don't mark as consumed, try again next composition
            }
        }
    }

    // Record composition complete event
    cudaEventRecord(m_compositeComplete, compositeStream);

    return true;
}

bool MegaCanvasManager::SetupCudaDXInterop() {
    if (!m_presenter || !m_presenter->IsInitialized()) {
        Logger::Error("MegaCanvasManager: Presenter not ready for interop setup");
        return false;
    }

    ID3D11Texture2D* backBuffer = m_presenter->GetBackBuffer();
    if (!backBuffer) {
        Logger::Error("MegaCanvasManager: No backbuffer for interop");
        return false;
    }

    // Register D3D11 backbuffer with CUDA
    cudaError_t err = cudaGraphicsD3D11RegisterResource(
        &m_backBufferResource,
        backBuffer,
        cudaGraphicsRegisterFlagsNone
    );

    if (err != cudaSuccess) {
        Logger::Error("MegaCanvasManager: cudaGraphicsD3D11RegisterResource failed: " +
                     std::string(cudaGetErrorString(err)));
        return false;
    }

    m_interopReady = true;
    Logger::Info("MegaCanvasManager: CUDA/DX11 interop established");
    return true;
}

void MegaCanvasManager::TeardownCudaDXInterop() {
    if (m_backBufferResource) {
        cudaGraphicsUnregisterResource(m_backBufferResource);
        m_backBufferResource = nullptr;
    }
    m_interopReady = false;
}

bool MegaCanvasManager::Present() {
    if (!m_initialized.load(std::memory_order_acquire) || !m_interopReady) {
        return false;
    }

    // Wait for composition to complete
    cudaEventSynchronize(m_compositeComplete);

    // Map D3D11 backbuffer to CUDA
    cudaError_t err = cudaGraphicsMapResources(1, &m_backBufferResource, m_compositeStream);
    if (err != cudaSuccess) {
        Logger::Warning("MegaCanvasManager: cudaGraphicsMapResources failed: " +
                       std::string(cudaGetErrorString(err)));
        return false;
    }

    // Get CUDA array from mapped resource
    cudaArray_t backBufferArray;
    err = cudaGraphicsSubResourceGetMappedArray(&backBufferArray, m_backBufferResource, 0, 0);
    if (err != cudaSuccess) {
        cudaGraphicsUnmapResources(1, &m_backBufferResource, m_compositeStream);
        Logger::Warning("MegaCanvasManager: Failed to get mapped array");
        return false;
    }

    // Copy Atlas to backbuffer
    CopyAtlasToArray(m_atlasBuffer, backBufferArray, CANVAS_WIDTH, CANVAS_HEIGHT, m_compositeStream);

    // Unmap resource before present
    cudaGraphicsUnmapResources(1, &m_backBufferResource, m_compositeStream);

    // Sync before present
    cudaStreamSynchronize(m_compositeStream);

    // Present via DXGI
    return m_presenter->Present();
}

void MegaCanvasManager::StartRenderThread() {
    if (m_running.load(std::memory_order_acquire)) {
        Logger::Warning("MegaCanvasManager: Render thread already running");
        return;
    }

    m_running.store(true, std::memory_order_release);
    
    m_renderThread = std::jthread([this](std::stop_token stopToken) {
        RenderThreadFunc(stopToken);
    });

    Logger::Info("MegaCanvasManager: Render thread started");
}

void MegaCanvasManager::StopRenderThread() {
    if (!m_running.load(std::memory_order_acquire)) {
        return;
    }

    m_running.store(false, std::memory_order_release);

    if (m_renderThread.joinable()) {
        m_renderThread.request_stop();
        m_renderThread.join();
    }

    Logger::Info("MegaCanvasManager: Render thread stopped");
}

void MegaCanvasManager::RenderThreadFunc(std::stop_token stopToken) {
    Logger::Info("MegaCanvasManager: Render thread running");

    // Pin to CCD1 (cores 8-15) for render/AI workloads
    // This keeps video capture on CCD0 and render on CCD1
    ThreadOptimizer::SetThreadAffinity(ThreadOptimizer::AffinityProfile::RENDER_DEDICATED);
    ThreadOptimizer::SetThreadPriority(ThreadOptimizer::Priority::REALTIME);

    using namespace std::chrono;
    constexpr auto targetFrameTime = microseconds(33333);  // 30 FPS
    auto lastFrameTime = high_resolution_clock::now();

    while (!stopToken.stop_requested() && m_running.load(std::memory_order_acquire)) {
        auto frameStart = high_resolution_clock::now();

        // Wait for DXGI waitable object (synchronizes with display)
        if (m_presenter) {
            m_presenter->WaitForVBlank();
        }

        // Compose atlas from latest frames
        ComposeAtlas(m_compositeStream);

        // Present to DXGI swapchain
        if (!Present()) {
            Logger::Warning("MegaCanvasManager: Present failed in render loop");
        }

        // Calculate frame timing
        auto frameEnd = high_resolution_clock::now();
        auto frameTime = duration_cast<microseconds>(frameEnd - frameStart);
        
        m_avgCompositionTimeMs.store(
            static_cast<float>(frameTime.count()) / 1000.0f,
            std::memory_order_relaxed
        );

        // Sleep if we're ahead of target
        if (frameTime < targetFrameTime) {
            std::this_thread::sleep_for(targetFrameTime - frameTime);
        }

        lastFrameTime = frameEnd;
    }

    Logger::Info("MegaCanvasManager: Render thread exiting");
}

// Diagnostic methods
uint64_t MegaCanvasManager::GetDroppedFrames(int cameraID) const {
    if (cameraID < 0 || cameraID >= NUM_CAMERAS) return 0;
    return m_cameras[cameraID].droppedFrames.load(std::memory_order_relaxed);
}

uint64_t MegaCanvasManager::GetTotalProcessedFrames() const {
    uint64_t total = 0;
    for (int i = 0; i < NUM_CAMERAS; i++) {
        total += m_cameras[i].processedFrames.load(std::memory_order_relaxed);
    }
    return total;
}

float MegaCanvasManager::GetAverageCompositionTimeMs() const {
    return m_avgCompositionTimeMs.load(std::memory_order_relaxed);
}

float MegaCanvasManager::GetAveragePresentTimeMs() const {
    return m_avgPresentTimeMs.load(std::memory_order_relaxed);
}

size_t MegaCanvasManager::GetEstimatedVRAMUsage() const {
    size_t total = m_atlasBufferSize;
    for (int i = 0; i < NUM_CAMERAS; i++) {
        if (m_cameras[i].isRegistered) {
            total += m_cameras[i].bgraBufferSize;
            total += m_cameras[i].tensorRTBufferSize;
        }
    }
    return total;
}

// Configuration loading
MegaCanvasConfig LoadMegaCanvasConfig(const std::string& configPath) {
    MegaCanvasConfig config;

    std::ifstream file(configPath);
    if (!file.is_open()) {
        Logger::Warning("MegaCanvasManager: Config file not found, using defaults");
        return config;
    }

    try {
        json j;
        file >> j;

        if (j.contains("mega_canvas") && j["mega_canvas"].is_object()) {
            auto& mc = j["mega_canvas"];
            
            if (mc.contains("enabled")) config.enabled = mc["enabled"].get<bool>();
            
            if (mc.contains("grid_layout")) {
                auto& grid = mc["grid_layout"];
                if (grid.contains("columns")) config.gridCols = grid["columns"].get<int>();
                if (grid.contains("rows")) config.gridRows = grid["rows"].get<int>();
            }
            
            if (mc.contains("cell_resolution")) {
                auto& cell = mc["cell_resolution"];
                if (cell.contains("width")) config.cellWidth = cell["width"].get<int>();
                if (cell.contains("height")) config.cellHeight = cell["height"].get<int>();
            }
            
            if (mc.contains("tensorrt")) {
                auto& trt = mc["tensorrt"];
                if (trt.contains("width")) config.tensorRTWidth = trt["width"].get<int>();
                if (trt.contains("height")) config.tensorRTHeight = trt["height"].get<int>();
                if (trt.contains("high_priority_stream")) config.tensorRTHighPriorityStream = trt["high_priority_stream"].get<bool>();
            }
            
            if (mc.contains("presentation")) {
                auto& pres = mc["presentation"];
                if (pres.contains("flip_model")) config.flipModel = pres["flip_model"].get<bool>();
                if (pres.contains("vsync")) config.vsync = pres["vsync"].get<bool>();
                if (pres.contains("tearing_allowed")) config.tearingAllowed = pres["tearing_allowed"].get<bool>();
                if (pres.contains("max_frame_latency")) config.maxFrameLatency = pres["max_frame_latency"].get<int>();
            }
            
            if (mc.contains("fallback_to_ndi")) config.fallbackToNDI = mc["fallback_to_ndi"].get<bool>();
            
            if (mc.contains("window")) {
                auto& win = mc["window"];
                if (win.contains("borderless")) config.borderless = win["borderless"].get<bool>();
                if (win.contains("no_redirection_bitmap")) config.noRedirectionBitmap = win["no_redirection_bitmap"].get<bool>();
                if (win.contains("topmost")) config.topmost = win["topmost"].get<bool>();
                if (win.contains("x")) config.windowX = win["x"].get<int>();
                if (win.contains("y")) config.windowY = win["y"].get<int>();
            }
            
            // Hidden mode (Ghost Window) configuration
            if (mc.contains("hidden_mode")) {
                auto& hm = mc["hidden_mode"];
                if (hm.contains("enabled")) config.hiddenMode = hm["enabled"].get<bool>();
                if (hm.contains("mouse_passthrough")) config.mousePassthrough = hm["mouse_passthrough"].get<bool>();
                if (hm.contains("hide_from_taskbar")) config.hideFromTaskbar = hm["hide_from_taskbar"].get<bool>();
                if (hm.contains("exclude_from_standard_capture")) config.excludeFromCapture = hm["exclude_from_standard_capture"].get<bool>();
                
                if (hm.contains("virtual_coordinates")) {
                    auto& vc = hm["virtual_coordinates"];
                    if (vc.contains("x")) config.virtualX = vc["x"].get<int>();
                    if (vc.contains("y")) config.virtualY = vc["y"].get<int>();
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::Warning("MegaCanvasManager: Error parsing config: " + std::string(e.what()));
    }

    return config;
}
