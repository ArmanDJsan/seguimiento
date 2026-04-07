/**
 * MegaCanvas Integration Example
 * 
 * This file shows how to integrate MegaCanvasManager into main.cpp
 * Replace the NDI output section with MegaCanvas output
 * 
 * Key integration points:
 * 1. Initialize MegaCanvasManager after DeckLink capture
 * 2. Register cameras after DeckLink initialization
 * 3. Update frame handler to use MegaCanvas instead of NDI
 * 4. Start render thread before main loop
 * 5. Access TensorRT buffers for YOLO inference
 */

#include "../canvas/MegaCanvasManager.h"
#include "../canvas/NDIFallback.h"
#include "../utils/Logger.h"

/**
 * Example initialization sequence for MegaCanvas
 * Call this after COM initialization and GPU diagnostics
 */
bool InitializeMegaCanvas(
    std::unique_ptr<MegaCanvasManager>& megaCanvas,
    int numCameras,
    unsigned int width,
    unsigned int height
) {
    Logger::Info("=== Initializing Mega-Canvas System ===");
    
    // Create MegaCanvas manager
    megaCanvas = std::make_unique<MegaCanvasManager>();
    
    // Initialize (creates DXGI presenter, allocates VRAM)
    if (!megaCanvas->Initialize()) {
        Logger::Error("MegaCanvas initialization failed");
        
        // Check if NDI fallback is enabled
        if (NDIFallback::IsFallbackEnabled()) {
            Logger::Warning("Activating NDI fallback...");
            if (GetNDIFallback().Activate()) {
                Logger::Info("NDI fallback activated successfully");
                megaCanvas.reset();  // Don't use MegaCanvas
                return true;
            }
        }
        
        return false;
    }
    
    // Register cameras
    for (int i = 0; i < numCameras; i++) {
        if (!megaCanvas->RegisterCamera(i, width, height)) {
            Logger::Warning("Failed to register camera " + std::to_string(i));
        }
    }
    
    // Start render thread (30Hz fixed rate)
    megaCanvas->StartRenderThread();
    
    Logger::Info("Mega-Canvas initialized: " + 
                 std::to_string(MegaCanvasManager::CANVAS_WIDTH) + "x" +
                 std::to_string(MegaCanvasManager::CANVAS_HEIGHT) + " (16K)");
    Logger::Info("VRAM usage: " + 
                 std::to_string(megaCanvas->GetEstimatedVRAMUsage() / (1024*1024)) + " MB");
    
    return true;
}

/**
 * Example frame handler replacement
 * This replaces the NDI SendFrame calls with MegaCanvas UpdateCameraFrame
 */
void OnFrameReady_MegaCanvas(
    MegaCanvasManager* megaCanvas,
    int cameraID,
    void* cudaBGRABuffer,
    unsigned int width,
    unsigned int height,
    cudaStream_t stream
) {
    if (!megaCanvas) {
        // If MegaCanvas not available, try NDI fallback
        GetNDIFallback().SendFrame(cameraID, cudaBGRABuffer, width, height, stream);
        return;
    }
    
    // Update frame in MegaCanvas (performs VRAM bifurcation)
    // This does:
    // 1. Blit frame to Atlas at camera position (1:1 native 4K)
    // 2. Downscale to 640x640 for TensorRT
    megaCanvas->UpdateCameraFrame(cameraID, cudaBGRABuffer, width, height, stream);
}

/**
 * Example TensorRT integration
 * Get the 640x640 downscaled buffer for YOLO inference
 */
void ProcessTensorRT_MegaCanvas(
    MegaCanvasManager* megaCanvas,
    int cameraID,
    void** tensorRTBuffer
) {
    if (!megaCanvas) {
        *tensorRTBuffer = nullptr;
        return;
    }
    
    // Get the 640x640 buffer that was created during VRAM bifurcation
    *tensorRTBuffer = megaCanvas->GetTensorRTBuffer(cameraID);
}

/**
 * Example shutdown sequence
 */
void ShutdownMegaCanvas(std::unique_ptr<MegaCanvasManager>& megaCanvas) {
    if (megaCanvas) {
        Logger::Info("Shutting down Mega-Canvas...");
        megaCanvas->StopRenderThread();
        megaCanvas->Shutdown();
        megaCanvas.reset();
    }
    
    // Also shutdown NDI fallback if active
    GetNDIFallback().Deactivate();
}

/**
 * Example: Modified main() integration
 * 
 * The following shows the key changes needed in main.cpp:
 * 
 * 1. Add includes at top:
 *    #include "../canvas/MegaCanvasManager.h"
 *    #include "../canvas/NDIFallback.h"
 * 
 * 2. Declare MegaCanvas after config loading:
 *    std::unique_ptr<MegaCanvasManager> megaCanvas;
 * 
 * 3. Initialize MegaCanvas after DeckLink setup:
 *    if (!InitializeMegaCanvas(megaCanvas, kNumCameras, kDefaultWidth, kDefaultHeight)) {
 *        Logger::Error("Failed to initialize output system");
 *        return 1;
 *    }
 * 
 * 4. Replace NDI frame handler in DeckLinkCapture callbacks:
 *    // OLD: ndiManager->SendBGRAFrame(channelID, cudaBGRABuffer, width, height, stream);
 *    // NEW:
 *    OnFrameReady_MegaCanvas(megaCanvas.get(), channelID, cudaBGRABuffer, width, height, stream);
 * 
 * 5. For TensorRT/YOLO, get the 640x640 buffer:
 *    void* tensorRTBuffer = megaCanvas->GetTensorRTBuffer(cameraID);
 *    // Use tensorRTBuffer for YOLO inference instead of the 4K buffer
 * 
 * 6. Shutdown at end:
 *    ShutdownMegaCanvas(megaCanvas);
 */

// ============================================================================
// COMPLETE FRAME HANDLER EXAMPLE
// ============================================================================

/**
 * Complete frame handler that integrates:
 * - MegaCanvas Atlas composition
 * - TensorRT inference on downscaled buffer
 * - Motion detection
 * - Camera selection
 * 
 * This replaces the existing SetFrameReadyHandler lambda in main.cpp
 */
/*
void CompleteFrameHandler(
    MegaCanvasManager* megaCanvas,
    ActiveCameraSelector* selector,
    YOLOProcessor* yolo,
    PerformanceMonitor* perfMon,
    int cameraID,
    const VideoChannel& channel,
    cudaStream_t stream
) {
    // 1. Update MegaCanvas (VRAM bifurcation: Atlas + TensorRT buffer)
    if (megaCanvas) {
        megaCanvas->UpdateCameraFrame(
            cameraID,
            channel.cudaBGRABuffer,  // 4K BGRA from color conversion
            channel.width,
            channel.height,
            stream
        );
    }
    
    // 2. Get TensorRT buffer (640x640) for YOLO
    void* tensorRTBuffer = megaCanvas ? megaCanvas->GetTensorRTBuffer(cameraID) : nullptr;
    
    // 3. Run YOLO inference on downscaled buffer
    if (yolo && tensorRTBuffer) {
        // TensorRT inference on 640x640 is much faster than 4K
        yolo->ProcessFrame(
            tensorRTBuffer,
            MegaCanvasManager::TENSORRT_WIDTH,
            MegaCanvasManager::TENSORRT_HEIGHT,
            stream
        );
    }
    
    // 4. Process motion detection (uses 4K buffer for accuracy)
    if (selector) {
        selector->ProcessFrame(
            cameraID,
            channel.cudaBGRABuffer,
            channel.width,
            channel.height,
            stream
        );
    }
    
    // 5. Record telemetry
    if (perfMon) {
        perfMon->RecordFrameProcessed(cameraID);
    }
}
*/
