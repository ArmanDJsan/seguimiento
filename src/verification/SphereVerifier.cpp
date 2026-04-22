/**
 * SphereVerifier.cpp
 * 
 * Implementation of sphere verification system
 */

#include "SphereVerifier.h"
#include "../control/VideoHubClient.h"
#include "../ai/InferenceEngine.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <thread>
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
    
    m_ready = true;
    Logger::Info("SphereVerifier: Initialized successfully");
}

SphereVerifier::~SphereVerifier() {
    Logger::Info("SphereVerifier: Shutting down");
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

// ============================================================================
// Internal Implementation
// ============================================================================

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
        
        // Still populate the detected spheres
        result.positions = positions;
        for (const auto& pos : positions) {
            result.sphereIDs.push_back(pos.sphereID);
        }
        std::sort(result.sphereIDs.begin(), result.sphereIDs.end());
        
        return result;
    }
    
    // Success!
    result.success = true;
    result.positions = positions;
    for (const auto& pos : positions) {
        result.sphereIDs.push_back(pos.sphereID);
    }
    std::sort(result.sphereIDs.begin(), result.sphereIDs.end());
    
    Logger::Info("SphereVerifier: Presence check PASSED - detected " + 
                std::to_string(result.spheresDetected) + " spheres");
    
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

std::vector<BallDetection> SphereVerifier::CaptureAndDetect(int cameraID, int frameCount, int timeoutMs) {
    std::vector<BallDetection> allDetections;
    
    if (m_stubMode) {
        // In stub mode, generate fake detections for testing
        Logger::Warning("SphereVerifier: Using stub detections (InferenceEngine in stub mode)");
        
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
    
    // TODO: Real implementation would:
    // 1. Register a frame callback with the capture system
    // 2. Wait for frames to arrive
    // 3. Call InferenceEngine->ProcessFrame() on each frame
    // 4. Accumulate detections
    
    Logger::Warning("SphereVerifier: Real frame capture not implemented - using stub mode");
    
    return allDetections;
}

std::vector<SpherePosition> SphereVerifier::FilterAndConsolidate(
    const std::vector<BallDetection>& detections,
    float confidenceThreshold)
{
    // Group detections by ball ID and average their positions
    std::unordered_map<int, std::vector<BallDetection>> grouped;
    
    for (const auto& det : detections) {
        if (det.confidence >= confidenceThreshold) {
            grouped[det.ballID].push_back(det);
        }
    }
    
    // Consolidate each group into a single position
    std::vector<SpherePosition> positions;
    for (const auto& [ballID, dets] : grouped) {
        if (dets.empty()) continue;
        
        // Average position and confidence
        float avgX = 0.0f;
        float avgY = 0.0f;
        float avgConf = 0.0f;
        int64_t latestTimestamp = 0;
        
        for (const auto& det : dets) {
            avgX += det.x;
            avgY += det.y;
            avgConf += det.confidence;
            if (det.timestamp > latestTimestamp) {
                latestTimestamp = det.timestamp;
            }
        }
        
        int count = static_cast<int>(dets.size());
        avgX /= count;
        avgY /= count;
        avgConf /= count;
        
        positions.emplace_back(ballID, avgX, avgY, avgConf, latestTimestamp);
    }
    
    // Sort by ball ID for consistency
    std::sort(positions.begin(), positions.end(),
              [](const SpherePosition& a, const SpherePosition& b) {
                  return a.sphereID < b.sphereID;
              });
    
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
    std::set<int> lastDetectedIDs;
    
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
        
        // Check if we have the expected count
        if (static_cast<int>(positions.size()) == expectedCount) {
            // Extract IDs
            std::set<int> currentIDs;
            for (const auto& pos : positions) {
                currentIDs.insert(pos.sphereID);
            }
            
            // Check if IDs match previous detection
            if (currentIDs == lastDetectedIDs) {
                consecutiveMatchCount++;
                
                if (consecutiveMatchCount >= consecutiveFrames) {
                    // Stable detection achieved!
                    outPositions = positions;
                    return true;
                }
            } else {
                // IDs changed, reset counter
                consecutiveMatchCount = 1;
                lastDetectedIDs = currentIDs;
            }
        } else {
            // Count doesn't match, reset
            consecutiveMatchCount = 0;
            lastDetectedIDs.clear();
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
