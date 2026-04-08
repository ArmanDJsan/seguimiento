/**
 * MegaCanvasManager.h
 * 
 * High-performance 16K Mega-Canvas compositor for vMix integration
 * Replaces NDI output with direct DXGI SwapChain presentation
 * 
 * Architecture:
 * - 15360x6480 (4x3 grid of native 4K cells) - 16K Virtual Canvas
 * - VRAM Bifurcation: SDI → Atlas 4K + TensorRT 640x640 downscale
 * - DXGI FLIP_DISCARD for DWM bypass and sub-frame latency
 * - WGC-friendly borderless window for vMix capture
 * 
 * Target Hardware:
 * - NVIDIA RTX 5080 16GB GDDR7 (Blackwell architecture)
 * - AMD Threadripper PRO 9955WX (Zen 5, 16C/32T)
 * 
 * Performance Target:
 * - Glass-to-Glass latency: <33.3ms (1 frame @ 30fps)
 * - Zero frame drops under normal operation
 */

#pragma once

#include <cuda_runtime.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <mutex>
#include <memory>
#include <functional>

using Microsoft::WRL::ComPtr;

// Forward declarations
class DXGIPresenter;

/**
 * MegaCanvasManager - 16K Atlas Compositor
 * 
 * Key Features:
 * - Latest-Frame-Available: Never blocks waiting for cameras
 * - GPU Fence synchronization: No CPU stalls
 * - Dual output: 4K to Atlas + 640x640 to TensorRT batch
 */
class MegaCanvasManager {
public:
    // Canvas specifications for 4K native per cell (4x3 grid)
    static constexpr int NUM_CAMERAS = 12;
    static constexpr int GRID_COLS = 4;
    static constexpr int GRID_ROWS = 3;
    static constexpr int CELL_WIDTH = 3840;     // Native 4K width
    static constexpr int CELL_HEIGHT = 2160;    // Native 4K height
    static constexpr int CANVAS_WIDTH = GRID_COLS * CELL_WIDTH;   // 15360 (16K)
    static constexpr int CANVAS_HEIGHT = GRID_ROWS * CELL_HEIGHT; // 6480
    
    // TensorRT inference dimensions
    static constexpr int TENSORRT_WIDTH = 640;
    static constexpr int TENSORRT_HEIGHT = 640;
    
    // VRAM estimation (BGRA32 format)
    // Atlas: 15360 × 6480 × 4 bytes = ~398 MB
    // 12x TensorRT buffers: 12 × 640 × 640 × 4 = ~20 MB
    // Total: ~420 MB (well within 16GB)
    
    /**
     * Camera slot for Latest-Frame-Available pattern
     */
    struct CameraSlot {
        // 4K BGRA buffer for Atlas composition
        void* cudaBGRABuffer = nullptr;         // CUDA device pointer
        size_t bgraBufferSize = 0;
        
        // 640x640 downscaled buffer for TensorRT
        void* cudaTensorRTBuffer = nullptr;     // CUDA device pointer
        size_t tensorRTBufferSize = 0;
        
        // GPU synchronization
        cudaEvent_t frameReady = nullptr;       // Signaled when frame is ready
        cudaEvent_t downscaleComplete = nullptr;// Signaled when downscale done
        
        // Frame tracking for Latest-Frame pattern
        std::atomic<uint64_t> frameID{0};       // Monotonic frame counter
        std::atomic<bool> hasNewFrame{false};   // Flag for new data
        
        // Camera metadata
        unsigned int width = 0;
        unsigned int height = 0;
        bool isRegistered = false;
        
        // Statistics
        std::atomic<uint64_t> droppedFrames{0}; // Frames overwritten before read
        std::atomic<uint64_t> processedFrames{0};
    };

    MegaCanvasManager();
    ~MegaCanvasManager();

    // Disable copy/move (CUDA resources are non-trivial)
    MegaCanvasManager(const MegaCanvasManager&) = delete;
    MegaCanvasManager& operator=(const MegaCanvasManager&) = delete;
    MegaCanvasManager(MegaCanvasManager&&) = delete;
    MegaCanvasManager& operator=(MegaCanvasManager&&) = delete;

    /**
     * Initialize the Mega-Canvas system
     * Creates DXGI presenter, allocates VRAM buffers, sets up CUDA/DX interop
     * @return true if initialization successful
     */
    bool Initialize();

    /**
     * Shutdown and release all resources
     */
    void Shutdown();

    /**
     * Register a camera for Atlas composition
     * Allocates 4K buffer + 640x640 TensorRT buffer in VRAM
     * @param cameraID Camera index (0-11)
     * @param width Source frame width (typically 3840)
     * @param height Source frame height (typically 2160)
     * @return true if registration successful
     */
    bool RegisterCamera(int cameraID, unsigned int width, unsigned int height);

    /**
     * Update camera frame with VRAM bifurcation
     * - Copies BGRA frame to Atlas slot (1:1)
     * - Simultaneously downscales to 640x640 for TensorRT
     * Non-blocking: Only updates pointers and signals events
     * 
     * @param cameraID Camera index (0-11)
     * @param cudaBGRASource Source BGRA buffer from DeckLinkCapture
     * @param width Source width
     * @param height Source height
     * @param sourceStream CUDA stream for async operations
     */
    void UpdateCameraFrame(int cameraID, void* cudaBGRASource,
                          unsigned int width, unsigned int height,
                          cudaStream_t sourceStream);

    /**
     * Get TensorRT buffer for batch inference
     * Returns the 640x640 downscaled buffer for the specified camera
     * @param cameraID Camera index (0-11)
     * @return CUDA device pointer to 640x640 BGRA buffer, or nullptr
     */
    void* GetTensorRTBuffer(int cameraID) const;

    /**
     * Compose the 16K Atlas from Latest-Frame-Available
     * Uses GPU fences - does not block waiting for cameras
     * @param compositeStream CUDA stream for composition
     * @return true if composition submitted successfully
     */
    bool ComposeAtlas(cudaStream_t compositeStream);

    /**
     * Copy Atlas to DXGI backbuffer and present
     * Uses CUDA/DX11 interop for zero-copy transfer
     * @return true if presentation successful
     */
    bool Present();

    /**
     * Start the render thread (30Hz fixed rate)
     * Thread is pinned to CCD1 (cores 8-15) per ThreadOptimizer
     */
    void StartRenderThread();

    /**
     * Stop the render thread gracefully
     */
    void StopRenderThread();

    /**
     * Check if system is initialized and running
     */
    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }

    /**
     * Get DXGIPresenter for external access
     */
    DXGIPresenter* GetPresenter() const { return m_presenter.get(); }

    // Diagnostics and telemetry
    uint64_t GetDroppedFrames(int cameraID) const;
    uint64_t GetTotalProcessedFrames() const;
    float GetAverageCompositionTimeMs() const;
    float GetAveragePresentTimeMs() const;
    size_t GetEstimatedVRAMUsage() const;

private:
    // Internal composition helpers
    bool AllocateCameraBuffers(int cameraID, unsigned int width, unsigned int height);
    void FreeCameraBuffers(int cameraID);
    bool SetupCudaDXInterop();
    void TeardownCudaDXInterop();
    
    // Render thread function
    void RenderThreadFunc(std::stop_token stopToken);

    // Camera slots (12 cameras)
    std::array<CameraSlot, NUM_CAMERAS> m_cameras;
    
    // 16K Atlas buffer in VRAM
    void* m_atlasBuffer = nullptr;              // CUDA device memory
    size_t m_atlasBufferSize = 0;
    cudaSurfaceObject_t m_atlasSurface = 0;     // For surface writes
    cudaArray_t m_atlasArray = nullptr;         // CUDA array backing
    
    // Composition stream and synchronization
    cudaStream_t m_compositeStream = nullptr;
    cudaEvent_t m_compositeComplete = nullptr;
    
    // DXGI presentation
    std::unique_ptr<DXGIPresenter> m_presenter;
    
    // CUDA/DX11 interop resources
    cudaGraphicsResource_t m_backBufferResource = nullptr;
    bool m_interopReady = false;
    
    // Render thread (C++20 jthread)
    std::jthread m_renderThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    
    // Telemetry
    std::atomic<float> m_avgCompositionTimeMs{0.0f};
    std::atomic<float> m_avgPresentTimeMs{0.0f};
    
    // Device info arrays for CUDA kernel
    void* m_deviceCameraInfos = nullptr;
};

/**
 * Configuration structure for MegaCanvasManager
 * Loaded from config.json "mega_canvas" section
 */
struct MegaCanvasConfig {
    bool enabled = true;
    
    // Grid layout
    int gridCols = 4;
    int gridRows = 3;
    
    // Cell resolution (native 4K)
    int cellWidth = 3840;
    int cellHeight = 2160;
    
    // TensorRT downscale
    int tensorRTWidth = 640;
    int tensorRTHeight = 640;
    bool tensorRTHighPriorityStream = true;  // Use high-priority stream for inference
    
    // Presentation settings
    bool flipModel = true;          // DXGI_SWAP_EFFECT_FLIP_DISCARD
    bool vsync = false;             // VSync off for minimum latency
    bool tearingAllowed = true;     // Allow tearing for VRR displays
    int maxFrameLatency = 1;        // Minimum latency
    
    // Fallback behavior
    bool fallbackToNDI = false;     // If WGC fails, use NDI
    
    // Window settings
    bool borderless = true;         // WS_POPUP
    bool noRedirectionBitmap = true;// WS_EX_NOREDIRECTIONBITMAP for WGC
    bool topmost = false;           // HWND_TOPMOST (optional)
    int windowX = 0;                // Window position X
    int windowY = 0;                // Window position Y
    
    // Hidden mode (Ghost Window)
    bool hiddenMode = true;         // Enable headless/virtual window mode
    bool mousePassthrough = true;   // WS_EX_TRANSPARENT + WS_EX_LAYERED
    bool hideFromTaskbar = true;    // WS_EX_TOOLWINDOW
    bool excludeFromCapture = false;// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
                                    // WARNING: Keep false to ensure vMix WGC works
    int virtualX = -15360;          // DEPRECATED: Auto-positioned at screenWidth-1 for vMix WGC
    int virtualY = 0;               // DEPRECATED: Auto-positioned at screenHeight-1 for vMix WGC
                                    // vMix requires >=1 pixel visible to detect window
};

/**
 * Load MegaCanvas configuration from JSON
 */
MegaCanvasConfig LoadMegaCanvasConfig(const std::string& configPath);
