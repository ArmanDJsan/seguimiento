/**
 * SphereVerifier.cpp
 * 
 * Implementation of sphere verification system
 */

#include "SphereVerifier.h"
#include "../control/VideoHubClient.h"
#include "../ai/InferenceEngine.h"
#include "../utils/Logger.h"
#include "../utils/FrameSaver.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdio>
#include <unordered_map>
#include <set>
#include <functional> // REQUERIDO para std::function
#include <mutex>      // REQUERIDO para std::lock_guard y m_mutex

namespace Verification {

SphereVerifier::SphereVerifier(VideoHubClient* videoHub, InferenceEngine* inferenceEngine)
    : m_videoHub(videoHub)
    , m_inferenceEngine(inferenceEngine)
    , m_ready(false)
    , m_stubMode(false)
{
    // Validate dependencies
    if (!m_videoHub || !m_inferenceEngine) {
        SetError("SphereVerifier: Missing required dependencies (VideoHub or InferenceEngine)");
        Logger::Error(m_lastError);
        return;
    }

    // Check if inference engine is in stub mode
    m_stubMode = m_inferenceEngine->IsStubMode();
    if (m_stubMode) {
        Logger::Warning("SphereVerifier: InferenceEngine is in stub mode - will simulate detections");
    }

    // Create CUDA stream for inference during verification
    cudaError_t cudaErr = cudaStreamCreate(&m_inferenceStream);
    if (cudaErr != cudaSuccess) {
        Logger::Warning("SphereVerifier: Failed to create CUDA stream - will use default stream");
        m_inferenceStream = 0;
    }

    m_ready = true;
    Logger::Info("SphereVerifier: Initialized successfully");
}

SphereVerifier::~SphereVerifier() {
    Logger::Info("SphereVerifier: Shutting down");
    if (m_inferenceStream && m_inferenceStream != (cudaStream_t)0) {
        cudaStreamDestroy(m_inferenceStream);
    }
}

bool SphereVerifier::IsReady() const {
    return m_ready.load();
}

VerificationResult SphereVerifier::Execute(VerificationMode mode, const VerificationConfig& config) {
    if (!IsReady()) {
        VerificationResult result;
        result.success = false;
        result.errorMessage = "SphereVerifier not ready";
        return result;
    }
    
    Logger::Info("SphereVerifier: Starting verification mode " + std::to_string(static_cast<int>(mode)) +
                 " on camera " + std::to_string(config.cameraID));
    
    switch (mode) {
        case VerificationMode::PRESENCE_CHECK:
            return ExecutePresenceCheck(config);
        
        case VerificationMode::POSITION_SNAPSHOT:
            return ExecutePositionSnapshot(config);
        
        case VerificationMode::ARRIVAL_ORDER:
            return ExecuteArrivalOrder(config);
        
        case VerificationMode::CHECKPOINT_PASS:
            return ExecuteCheckpointPass(config);
        
        default:
            VerificationResult result;
            result.success = false;
            result.errorMessage = "Unknown verification mode";
            return result;
    }
}

VerificationResult SphereVerifier::CheckPresence(int cameraID, int expectedCount, int timeoutMs) {
    VerificationConfig config;
    config.cameraID = cameraID;
    config.expectedSpheres = expectedCount;
    config.timeoutMs = timeoutMs;
    return Execute(VerificationMode::PRESENCE_CHECK, config);
}

VerificationResult SphereVerifier::CapturePositions(int cameraID, int timeoutMs) {
    VerificationConfig config;
    config.cameraID = cameraID;
    config.timeoutMs = timeoutMs;
    return Execute(VerificationMode::POSITION_SNAPSHOT, config);
}

VerificationResult SphereVerifier::WaitForArrivals(int cameraID, int expectedCount, int timeoutMs) {
    VerificationConfig config;
    config.cameraID = cameraID;
    config.expectedSpheres = expectedCount;
    config.timeoutMs = timeoutMs;
    return Execute(VerificationMode::ARRIVAL_ORDER, config);
}

void SphereVerifier::SetFrameCallback(std::function<void(int, void*, unsigned int, unsigned int)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameCallback = callback;
}

void SphereVerifier::SetDetectionCallback(std::function<void(const std::vector<BallDetection>&)> callback) {
    m_detectionCallback = callback;
}

void SphereVerifier::SetRealCaptureAvailable(bool available) {
    m_realCaptureAvailable.store(available);
    if (available) {
        Logger::Info("SphereVerifier: Real frame capture ENABLED - using DeckLink frames");
    } else {
        Logger::Info("SphereVerifier: Real frame capture DISABLED - using fallback");
    }
}

void SphereVerifier::PushFrame(int cameraID, void* cudaUYVYBuffer, unsigned int width, unsigned int height) {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_pendingFrameBuffer = cudaUYVYBuffer;
    m_pendingFrameWidth = width;
    m_pendingFrameHeight = height;
    m_pendingFrameCamera = cameraID;
    m_pendingFrameReady = true;
    m_frameCV.notify_one();
}

bool SphereVerifier::WaitForFrame(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_frameMutex);
    if (m_pendingFrameReady) {
        return true;  // Frame already available
    }
    return m_frameCV.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                              [this] { return m_pendingFrameReady; });
}

// ============================================================================
// Internal Implementation
// ============================================================================

// Forward declarations for drawing functions
static void DrawDigit(int digit, int cx, int cy, int scale,
                      std::fstream& fs, int imgWidth,
                      std::streampos dataStart, unsigned char* color);

void SphereVerifier::SaveResultImage(const std::vector<SpherePosition>& positions,
                                     const std::string& label) {
    void* frameBuffer;
    unsigned int width, height;
    {
        std::lock_guard<std::mutex> lock(m_savedFrameMutex);
        frameBuffer = m_savedFrameBuffer;
        width = m_savedFrameWidth;
        height = m_savedFrameHeight;
    }
    if (!frameBuffer || width == 0) return;

    std::ostringstream filename;
    filename << "result_" << label << ".ppm";

    if (!FrameSaver::SaveYUVFrameAsPPM(frameBuffer, width, height, filename.str())) return;

    // Draw numbered boxes for each verified position (bright green, 3px)
    std::fstream fs(filename.str(), std::ios::in | std::ios::out | std::ios::binary);
    if (!fs.is_open()) return;

    std::string line;
    for (int i = 0; i < 3; i++) { std::getline(fs, line); if (!line.empty() && line.back()=='\r') line.pop_back(); }
    std::streampos dataStart = fs.tellg();

    Logger::Info("[RESULT] Saved " + filename.str() + " with " + std::to_string(positions.size()) + " spheres");

    // Draw ROI rectangle (dashed white, 1px)
    const int ROI_X1 = 552, ROI_Y1 = 324, ROI_X2 = 626, ROI_Y2 = 741;
    unsigned char white[3] = {255,255,255};
    auto drawROIPixel = [&](int x, int y) {
        if (x >= 0 && x < (int)width && y >= 0 && y < (int)height) {
            // Dashed: only draw every 4th pixel
            if (((x + y) / 4) % 2 == 0) {
                fs.seekp(dataStart + (std::streampos)(y * width + x) * 3);
                fs.write((char*)white, 3);
            }
        }
    };
    for (int x = ROI_X1; x <= ROI_X2; x++) { drawROIPixel(x, ROI_Y1); drawROIPixel(x, ROI_Y2); }
    for (int y = ROI_Y1; y <= ROI_Y2; y++) { drawROIPixel(ROI_X1, y); drawROIPixel(ROI_X2, y); }

    for (const auto& pos : positions) {
        int cx = static_cast<int>(pos.x * width);
        int cy = static_cast<int>(pos.y * height);
        int boxSize = 15;
        int x1 = (std::max)(0, cx - boxSize);
        int y1 = (std::max)(0, cy - boxSize);
        int x2 = (std::min)((int)width - 1, cx + boxSize);
        int y2 = (std::min)((int)height - 1, cy + boxSize);

        // Green box for presence verification
        unsigned char green[3] = {0, 255, 0};
        const int B = 3;
        for (int x = x1; x <= x2; x++) {
            for (int t = 0; t < B && y1+t <= y2; t++) {
                fs.seekp(dataStart + (std::streampos)((y1+t)*width + x)*3);
                fs.write((char*)green, 3);
            }
            for (int t = 0; t < B && y2-t >= y1; t++) {
                fs.seekp(dataStart + (std::streampos)((y2-t)*width + x)*3);
                fs.write((char*)green, 3);
            }
        }
        for (int y = y1; y <= y2; y++) {
            for (int t = 0; t < B && x1+t <= x2; t++) {
                fs.seekp(dataStart + (std::streampos)(y*width + x1+t)*3);
                fs.write((char*)green, 3);
            }
            for (int t = 0; t < B && x2-t >= x1; t++) {
                fs.seekp(dataStart + (std::streampos)(y*width + x2-t)*3);
                fs.write((char*)green, 3);
            }
        }

        // Draw sequential ID number (0,1,2...)
        int digit = pos.sphereID;
        if (digit >= 0 && digit <= 9) {
            int scale = 2;
            int textW = 3 * scale;
            int textH = 5 * scale;
            int tx = cx - textW/2;
            int ty = cy - textH/2;
            unsigned char white[3] = {255,255,255};
            DrawDigit(digit, tx, ty, scale, fs, (int)width, dataStart, green);
        }

        Logger::Info("[RESULT]   Sphere " + std::to_string(pos.sphereID) +
                    " at (" + std::to_string(cx) + "," + std::to_string(cy) +
                    ") conf=" + std::to_string(pos.confidence).substr(0,5));
    }
    fs.close();
}

VerificationResult SphereVerifier::ExecutePresenceCheck(const VerificationConfig& config) {
    VerificationResult result;
    result.cameraUsed = config.cameraID;
    result.timestamp = GetCurrentTimeMs();
    
    // Route to the specified camera
    if (!RouteCamera(config.cameraID)) {
        result.success = false;
        result.errorMessage = "Failed to route camera " + std::to_string(config.cameraID);
        Logger::Error("SphereVerifier: " + result.errorMessage);
        return result;
    }
    
    // Wait for stable detections
    std::vector<SpherePosition> positions;
    bool stable = WaitForStableDetections(
        config.cameraID,
        config.expectedSpheres,
        config.confidenceThreshold,
        config.consecutiveFramesRequired,
        config.timeoutMs,
        positions
    );
    
    if (!stable) {
        result.success = false;
        result.spheresDetected = static_cast<int>(positions.size());
        result.errorMessage = "Failed to detect stable spheres. Expected " +
                             std::to_string(config.expectedSpheres) +
                             ", detected " + std::to_string(result.spheresDetected);
        Logger::Error("SphereVerifier: " + result.errorMessage);
        SaveResultImage(positions, "presence_fail");
        return result;
    }

    // Check if we have the expected count
    result.spheresDetected = static_cast<int>(positions.size());
    if (result.spheresDetected != config.expectedSpheres) {
        result.success = false;
        result.errorMessage = "Sphere count mismatch. Expected " +
                             std::to_string(config.expectedSpheres) +
                             ", detected " + std::to_string(result.spheresDetected);
        Logger::Error("SphereVerifier: " + result.errorMessage);
        result.positions = positions;
        for (const auto& pos : positions) { result.sphereIDs.push_back(pos.sphereID); }
        std::sort(result.sphereIDs.begin(), result.sphereIDs.end());
        SaveResultImage(positions, "presence_mismatch");
        return result;
    }

    // Success!
    result.success = true;
    result.positions = positions;
    for (const auto& pos : positions) { result.sphereIDs.push_back(pos.sphereID); }
    std::sort(result.sphereIDs.begin(), result.sphereIDs.end());

    Logger::Info("SphereVerifier: Presence check PASSED - detected " +
                std::to_string(result.spheresDetected) + " spheres");
    SaveResultImage(positions, "presence_ok");

    return result;
}

VerificationResult SphereVerifier::ExecutePositionSnapshot(const VerificationConfig& config) {
    VerificationResult result;
    result.cameraUsed = config.cameraID;
    result.timestamp = GetCurrentTimeMs();
    
    // Route to the specified camera
    if (!RouteCamera(config.cameraID)) {
        result.success = false;
        result.errorMessage = "Failed to route camera " + std::to_string(config.cameraID);
        Logger::Error("SphereVerifier: " + result.errorMessage);
        return result;
    }
    
    // Capture frames and detect
    auto detections = CaptureAndDetect(config.cameraID, config.sampleFrames, config.timeoutMs);
    
    if (detections.empty()) {
        result.success = false;
        result.errorMessage = "No spheres detected in snapshot";
        Logger::Warning("SphereVerifier: " + result.errorMessage);
        return result;
    }
    
    // Filter and consolidate detections
    auto positions = FilterAndConsolidate(detections, config.confidenceThreshold);
    
    result.success = true;
    result.spheresDetected = static_cast<int>(positions.size());
    result.positions = positions;
    
    for (const auto& pos : positions) {
        result.sphereIDs.push_back(pos.sphereID);
    }
    std::sort(result.sphereIDs.begin(), result.sphereIDs.end());
    
    Logger::Info("SphereVerifier: Position snapshot captured - " + 
                std::to_string(result.spheresDetected) + " spheres");
    
    return result;
}

VerificationResult SphereVerifier::ExecuteArrivalOrder(const VerificationConfig& config) {
    VerificationResult result;
    result.cameraUsed = config.cameraID;
    result.timestamp = GetCurrentTimeMs();
    
    // TODO: Implement arrival order tracking
    // This requires continuous monitoring and tracking spheres as they cross finish line
    
    Logger::Warning("SphereVerifier: ARRIVAL_ORDER mode not fully implemented yet (stub)");
    
    result.success = false;
    result.errorMessage = "ARRIVAL_ORDER mode not fully implemented yet";
    
    return result;
}

VerificationResult SphereVerifier::ExecuteCheckpointPass(const VerificationConfig& config) {
    VerificationResult result;
    result.cameraUsed = config.cameraID;
    result.timestamp = GetCurrentTimeMs();
    
    // TODO: Implement checkpoint pass monitoring
    // This requires continuous monitoring and tracking spheres as they pass through zone
    
    Logger::Warning("SphereVerifier: CHECKPOINT_PASS mode not fully implemented yet (stub)");
    
    result.success = false;
    result.errorMessage = "CHECKPOINT_PASS mode not fully implemented yet";
    
    return result;
}

// ============================================================================
// Helper Methods
// ============================================================================

bool SphereVerifier::RouteCamera(int cameraID) {
    if (!m_videoHub) {
        SetError("VideoHub not available");
        return false;
    }
    
    // Assuming output 0 is the primary output for verification
    // Camera IDs are 1-based, VideoHub expects 0-based indexing
    std::string cameraName = std::string("CAM_") + (cameraID < 10 ? "0" : "") + std::to_string(cameraID);
    
    bool success = m_videoHub->RouteInputToOutput(0, cameraName);
    if (!success) {
        Logger::Error("SphereVerifier: Failed to route camera " + cameraName);
        return false;
    }
    
    // Allow some time for video to stabilize after routing
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    Logger::Debug("SphereVerifier: Routed camera " + cameraName);
    return true;
}

// Simple 3x5 bitmap font for digits 0-9 (5 rows, 3 cols, 1=on 0=off)
static const int FONT[10][5][3] = {
    {{1,1,1},{1,0,1},{1,0,1},{1,0,1},{1,1,1}}, // 0
    {{0,1,0},{1,1,0},{0,1,0},{0,1,0},{1,1,1}}, // 1
    {{1,1,1},{0,0,1},{1,1,1},{1,0,0},{1,1,1}}, // 2
    {{1,1,1},{0,0,1},{1,1,1},{0,0,1},{1,1,1}}, // 3
    {{1,0,1},{1,0,1},{1,1,1},{0,0,1},{0,0,1}}, // 4
    {{1,1,1},{1,0,0},{1,1,1},{0,0,1},{1,1,1}}, // 5
    {{1,1,1},{1,0,0},{1,1,1},{1,0,1},{1,1,1}}, // 6
    {{1,1,1},{0,0,1},{0,1,0},{1,0,0},{1,0,0}}, // 7
    {{1,1,1},{1,0,1},{1,1,1},{1,0,1},{1,1,1}}, // 8
    {{1,1,1},{1,0,1},{1,1,1},{0,0,1},{1,1,1}}, // 9
};

static void DrawDigit(int digit, int cx, int cy, int scale,
                      std::fstream& fs, int imgWidth,
                      std::streampos dataStart, unsigned char* color) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (!FONT[digit][row][col]) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = cx + col * scale + sx;
                    int py = cy + row * scale + sy;
                    if (px >= 0 && px < imgWidth && py >= 0 && py < 6000) {
                        fs.seekp(dataStart + (std::streampos)(py * imgWidth + px) * 3);
                        fs.write((char*)color, 3);
                    }
                }
            }
        }
    }
}
static void DrawBoxesOnPPM(const std::string& filename, int imgWidth, int imgHeight,
                           const std::vector<BallDetection>& detections) {
    // PPM P6 format: "P6\nW H\n255\n" + raw RGB
    std::fstream fs(filename, std::ios::in | std::ios::out | std::ios::binary);
    if (!fs.is_open()) return;

    // Skip to pixel data
    std::string line;
    for (int i = 0; i < 3; i++) {
        std::getline(fs, line);
        // Handle Windows \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }
    std::streampos dataStart = fs.tellg();

    // Color palette matching physical sphere colors (R,G,B for PPM)
    // 0=blanco, 1=amarillo, 2=azul, 3=rojo, 4=morado, 5=naranja, 6=verde, 7=vinotinto, 8=negro, 9=amarillo-blanco
    // Returns color for a given ballID and pixel position (for striping)
    auto getPixelColor = [](int ballID, int px, int py) -> std::tuple<int,int,int> {
        const int colors[10][3] = {
            {255,255,255}, // 0 blanco
            {255,255,0},   // 1 amarillo
            {0,0,255},     // 2 azul
            {255,0,0},     // 3 rojo
            {128,0,128},   // 4 morado
            {255,165,0},   // 5 naranja
            {0,255,0},     // 6 verde
            {128,0,0},     // 7 vinotinto
            {80,80,80},    // 8 negro (gris para ser visible)
            {255,255,224}  // 9 amarillo-blanco
        };
        int idx = (ballID >= 0 && ballID < 10) ? ballID : (ballID % 10);

        // Sphere 9: alternate yellow/white stripes
        if (ballID == 9) {
            return ((px + py) % 2 == 0) ? std::make_tuple(255,255,0)   // yellow
                                         : std::make_tuple(255,255,255); // white
        }
        // Sphere 4: alternate purple/white stripes
        if (ballID == 4) {
            return ((px + py) % 2 == 0) ? std::make_tuple(128,0,128)   // purple
                                         : std::make_tuple(255,255,255); // white
        }
        // Sphere 8: white dots on dark border for visibility
        if (ballID == 8) {
            return ((px + py) % 3 == 0) ? std::make_tuple(255,255,255) // white dot
                                         : std::make_tuple(80,80,80);   // dark gray
        }
        return {colors[idx][0], colors[idx][1], colors[idx][2]};
    };

    // Draw each detection box (3px border, colored by sphere color)
    for (const auto& d : detections) {
        int cx = static_cast<int>(d.x * imgWidth);
        int cy = static_cast<int>(d.y * imgHeight);
        int bw = static_cast<int>(d.width * imgWidth);
        int bh = static_cast<int>(d.height * imgHeight);
        int x1 = (std::max)(0, cx - bw/2);
        int y1 = (std::max)(0, cy - bh/2);
        int x2 = (std::min)(imgWidth - 1, cx + bw/2);
        int y2 = (std::min)(imgHeight - 1, cy + bh/2);

        unsigned char color[3];

        // Draw 3px border: top, bottom, left, right (per-pixel color for stripes)
        const int BORDER = 3;
        int ballID = d.ballID;
        for (int x = x1; x <= x2; x++) {
            for (int t = 0; t < BORDER && y1 + t <= y2; t++) {
                auto [r,g,b] = getPixelColor(ballID, x, y1 + t);
                color[0]=(unsigned char)r; color[1]=(unsigned char)g; color[2]=(unsigned char)b;
                fs.seekp(dataStart + (std::streampos)((y1 + t) * imgWidth + x) * 3);
                fs.write((char*)color, 3);
            }
            for (int t = 0; t < BORDER && y2 - t >= y1; t++) {
                auto [r,g,b] = getPixelColor(ballID, x, y2 - t);
                color[0]=(unsigned char)r; color[1]=(unsigned char)g; color[2]=(unsigned char)b;
                fs.seekp(dataStart + (std::streampos)((y2 - t) * imgWidth + x) * 3);
                fs.write((char*)color, 3);
            }
        }
        for (int y = y1; y <= y2; y++) {
            for (int t = 0; t < BORDER && x1 + t <= x2; t++) {
                auto [r,g,b] = getPixelColor(ballID, x1 + t, y);
                color[0]=(unsigned char)r; color[1]=(unsigned char)g; color[2]=(unsigned char)b;
                fs.seekp(dataStart + (std::streampos)(y * imgWidth + x1 + t) * 3);
                fs.write((char*)color, 3);
            }
            for (int t = 0; t < BORDER && x2 - t >= x1; t++) {
                auto [r,g,b] = getPixelColor(ballID, x2 - t, y);
                color[0]=(unsigned char)r; color[1]=(unsigned char)g; color[2]=(unsigned char)b;
                fs.seekp(dataStart + (std::streampos)(y * imgWidth + x2 - t) * 3);
                fs.write((char*)color, 3);
            }

            // Draw ballID number centered inside the box
            int digit = d.ballID;
            if (digit >= 0 && digit <= 9) {
                // White text with black outline for readability
                int scale = (std::max)(2, (std::min)(bw, bh) / 20);
                int textW = 3 * scale;
                int textH = 5 * scale;
                int tx = cx - textW / 2;
                int ty = cy - textH / 2;
                unsigned char white[3] = {255,255,255};
                unsigned char black[3] = {0,0,0};
                DrawDigit(digit, tx, ty, scale, fs, imgWidth, dataStart, black);
            }
        }
    }
    fs.close();
}

std::vector<BallDetection> SphereVerifier::CaptureAndDetect(int cameraID, int frameCount, int timeoutMs) {
    std::vector<BallDetection> allDetections;

    // === PATH 1: Real frame capture via DeckLink ===
    if (m_realCaptureAvailable.load() && m_inferenceEngine && !m_stubMode) {
        int framesPerCapture = (std::max)(1, frameCount);
        int timeoutPerFrame = timeoutMs / (std::max)(1, framesPerCapture);

        for (int f = 0; f < framesPerCapture; f++) {
            // Wait for a real frame from the DeckLink capture callback
            if (!WaitForFrame(timeoutPerFrame)) {
                Logger::Warning("SphereVerifier: Timeout waiting for real frame " +
                               std::to_string(f + 1) + "/" + std::to_string(framesPerCapture));
                continue;
            }

            // Grab the pending frame
            void* frameBuffer;
            unsigned int width, height;
            int camID;
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                frameBuffer = m_pendingFrameBuffer;
                width = m_pendingFrameWidth;
                height = m_pendingFrameHeight;
                camID = m_pendingFrameCamera;
                m_pendingFrameReady = false;
            }

            if (!frameBuffer) continue;

            // Save frame for result image
            {
                std::lock_guard<std::mutex> lock(m_savedFrameMutex);
                m_savedFrameBuffer = frameBuffer;
                m_savedFrameWidth = width;
                m_savedFrameHeight = height;
            }

            // Run real YOLO inference on the captured frame
            auto detections = m_inferenceEngine->ProcessFrameUYVY(
                frameBuffer, camID, width, height,
                m_inferenceStream, nullptr
            );

            if (!detections.empty()) {
                Logger::Debug("SphereVerifier: Frame " + std::to_string(f + 1) +
                             " captured " + std::to_string(detections.size()) + " detections");
                // Notify live preview callback (NDI boxes)
                if (m_detectionCallback) {
                    m_detectionCallback(detections);
                }
            }

            // Save last frame as PPM with detection overlay info
            if (f == framesPerCapture - 1 && !detections.empty()) {
                static int saveCount = 0;
                std::ostringstream filename;
                filename << "detection_verify_" << std::setw(2) << std::setfill('0')
                        << (saveCount % 100) << ".ppm";
                if (FrameSaver::SaveYUVFrameAsPPM(frameBuffer, width, height, filename.str())) {
                    // Draw detection boxes on the saved PPM
                    DrawBoxesOnPPM(filename.str(), width, height, detections);
                    Logger::Info("[DETECTIONS] Frame saved with boxes: " + filename.str() +
                                " (" + std::to_string(width) + "x" + std::to_string(height) +
                                ") detections=" + std::to_string(detections.size()));
                    for (const auto& d : detections) {
                        int px = static_cast<int>(d.x * width);
                        int py = static_cast<int>(d.y * height);
                        int pw = static_cast<int>(d.width * width);
                        int ph = static_cast<int>(d.height * height);
                        Logger::Info("[DETECTIONS]   B" + std::to_string(d.ballID) +
                                    " box=(" + std::to_string(px - pw/2) + "," + std::to_string(py - ph/2) +
                                    "," + std::to_string(px + pw/2) + "," + std::to_string(py + ph/2) +
                                    ") conf=" + std::to_string(d.confidence).substr(0, 5));
                    }
                }
                saveCount++;
            }

            allDetections.insert(allDetections.end(), detections.begin(), detections.end());
        }

        if (!allDetections.empty()) {
            return allDetections;
        }
        // Real capture ran but found nothing — return empty to report actual result
        Logger::Info("SphereVerifier: Real capture found 0 detections in " +
                    std::to_string(framesPerCapture) + " frames");
        return allDetections;  // Empty = no spheres detected (real result)
    }

    // === PATH 2: Stub/fallback detections ===
    if (m_stubMode) {
        Logger::Warning("SphereVerifier: Using stub detections (InferenceEngine in stub mode)");
    } else {
        Logger::Warning("SphereVerifier: Real frame capture not available - falling back to stub detections");
    }

    // Generate detections for 10 balls in a grid pattern
    for (int i = 0; i < 10; i++) {
        BallDetection det;
        det.ballID = i + 1;
        det.cameraID = cameraID;
        det.x = 0.1f + (i % 5) * 0.18f;  // 5 columns
        det.y = 0.2f + (i / 5) * 0.4f;   // 2 rows
        det.width = 0.05f;
        det.height = 0.05f;
        det.confidence = 0.85f + (i % 3) * 0.05f;
        det.timestamp = GetCurrentTimeMs();
        allDetections.push_back(det);
    }

    return allDetections;
}

std::vector<SpherePosition> SphereVerifier::FilterAndConsolidate(
    const std::vector<BallDetection>& detections,
    float confidenceThreshold)
{
    // Region of Interest: only count detections within the track area
    // Prevents false positives from background objects outside the track
    const float ROI_X1 = 552.0f / 1920.0f;   // Left edge of track area
    const float ROI_Y1 = 324.0f / 1080.0f;   // Top edge
    const float ROI_X2 = 626.0f / 1920.0f;   // Right edge
    const float ROI_Y2 = 741.0f / 1080.0f;   // Bottom edge

    // Spatial dedup: count physically distinct spheres regardless of ballID.
    std::vector<SpherePosition> positions;
    std::set<std::pair<int,int>> seenPositions;

    for (const auto& det : detections) {
        if (det.confidence < confidenceThreshold) continue;
        // ROI filter: center must be inside track rectangle
        if (det.x < ROI_X1 || det.x > ROI_X2 || det.y < ROI_Y1 || det.y > ROI_Y2) continue;

        // Round to 10px grid to merge very close detections of the same sphere
        int rx = static_cast<int>(std::round(det.x * 1920.0f / 10.0f));
        int ry = static_cast<int>(std::round(det.y * 1080.0f / 10.0f));
        auto key = std::make_pair(rx, ry);
        if (seenPositions.count(key)) continue;
        seenPositions.insert(key);

        int uniqueID = static_cast<int>(positions.size());
        positions.emplace_back(uniqueID, det.x, det.y, det.confidence, det.timestamp);
    }

    Logger::Info("[VERIFY] Distinct spheres detected: " + std::to_string(positions.size()));

    return positions;
}

bool SphereVerifier::WaitForStableDetections(
    int cameraID,
    int expectedCount,
    float confidenceThreshold,
    int consecutiveFrames,
    int timeoutMs,
    std::vector<SpherePosition>& outPositions)
{
    auto startTime = std::chrono::steady_clock::now();
    int consecutiveMatchCount = 0;
    
    while (true) {
        // Check timeout
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime
        ).count();
        
        if (elapsed > timeoutMs) {
            Logger::Warning("SphereVerifier: Timeout waiting for stable detections");
            return false;
        }
        
        // Capture and detect
        auto detections = CaptureAndDetect(cameraID, 5, 500);
        auto positions = FilterAndConsolidate(detections, confidenceThreshold);
        
        // Check if we have at least the expected count
        if (static_cast<int>(positions.size()) >= expectedCount) {
            consecutiveMatchCount++;

            if (consecutiveMatchCount >= consecutiveFrames) {
                // Stable detection achieved!
                outPositions = positions;
                return true;
            }
        } else {
            // Count below threshold, reset
            consecutiveMatchCount = 0;
        }
        
        // Small delay before next attempt
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void SphereVerifier::SetError(const std::string& error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastError = error;
}

int64_t SphereVerifier::GetCurrentTimeMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// === Checkpoint Event Support ===

void SphereVerifier::SetCheckpointCallback(CheckpointEventCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_checkpointCallback = callback;
    Logger::Info("SphereVerifier: Checkpoint callback registered");
}

void SphereVerifier::EmitCheckpointEvent(const std::string& checkpointName, 
                                          float progress, int sphereCount) {
    CheckpointEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        callback = m_checkpointCallback;
    }
    
    Logger::Info("SphereVerifier: Checkpoint event - " + checkpointName + 
                 " at " + std::to_string(progress) + "% with " + 
                 std::to_string(sphereCount) + " spheres");
    
    if (callback) {
        try {
            callback(checkpointName, progress, sphereCount);
        } catch (const std::exception& e) {
            Logger::Error("SphereVerifier: Checkpoint callback exception: " + 
                         std::string(e.what()));
        }
    }
}

} // namespace Verification
