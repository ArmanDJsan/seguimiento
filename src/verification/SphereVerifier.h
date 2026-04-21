/**
 * SphereVerifier.h
 * 
 * Sphere/ball verification system for race setup and validation
 * Integrates with InferenceEngine and VideoHub for automated checks
 * 
 * Features:
 * - Presence check: Verify all expected spheres are detected
 * - Position snapshot: Capture current positions of all spheres
 * - Arrival order: Track order of spheres crossing finish line
 * - Checkpoint pass: Monitor spheres passing through defined zones
 * 
 * Usage:
 *   SphereVerifier verifier(&videoHub, &inferenceEngine, &captureManager);
 *   auto result = verifier.CheckPresence(1, 10);  // Camera 1, expect 10 spheres
 *   if (result.success) {
 *       Logger::Info("All spheres detected!");
 *   }
 */

#pragma once

#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <mutex>        
#include <functional>  

// Forward declarations
class VideoHubClient;
class InferenceEngine;
struct BallDetection;
struct GlobalPosition;

namespace Verification {

/**
 * Rectangle for defining checkpoint zones
 */
struct Rect {
    float x = 0.0f;      // Normalized X position [0.0, 1.0]
    float y = 0.0f;      // Normalized Y position [0.0, 1.0]
    float width = 1.0f;  // Normalized width [0.0, 1.0]
    float height = 1.0f; // Normalized height [0.0, 1.0]
    
    Rect() = default;
    Rect(float x_, float y_, float w_, float h_)
        : x(x_), y(y_), width(w_), height(h_) {}
    
    bool Contains(float px, float py) const {
        return px >= x && px <= (x + width) &&
               py >= y && py <= (y + height);
    }
};

/**
 * Position information for a detected sphere
 */
struct SpherePosition {
    int sphereID;        // Ball identifier (1-10, or 0 if unknown)
    float x;             // Normalized X position [0.0, 1.0]
    float y;             // Normalized Y position [0.0, 1.0]
    float confidence;    // Detection confidence [0.0, 1.0]
    int64_t timestamp;   // Detection timestamp (ms since epoch)
    
    SpherePosition()
        : sphereID(0), x(0.0f), y(0.0f), confidence(0.0f), timestamp(0) {}
    
    SpherePosition(int id, float x_, float y_, float conf, int64_t ts)
        : sphereID(id), x(x_), y(y_), confidence(conf), timestamp(ts) {}
};

/**
 * Verification modes
 */
enum class VerificationMode {
    PRESENCE_CHECK,    // Verify that all expected spheres are present
    POSITION_SNAPSHOT, // Capture current positions of all detected spheres
    ARRIVAL_ORDER,     // Track order of spheres crossing finish line (sequential)
    CHECKPOINT_PASS    // Monitor spheres passing through checkpoint zone (sequential)
};

/**
 * Configuration for sphere verification
 */
struct VerificationConfig {
    int expectedSpheres = 10;           // Number of spheres expected
    int cameraID = 1;                   // Camera to use for verification (1-12)
    int timeoutMs = 5000;               // Maximum time to wait for detection
    float confidenceThreshold = 0.6f;   // Minimum confidence for valid detection
    Rect checkpointZone;                // Zone for CHECKPOINT_PASS mode
    int sampleFrames = 30;              // Number of frames to sample for stable detection
    int consecutiveFramesRequired = 3;  // Consecutive frames needed for confirmation
    
    VerificationConfig() {
        // Default checkpoint zone covers full frame
        checkpointZone = Rect(0.0f, 0.0f, 1.0f, 1.0f);
    }
};

/**
 * Result of verification operation
 */
struct VerificationResult {
    bool success;                           // True if verification succeeded
    int spheresDetected;                    // Number of spheres detected
    std::vector<int> sphereIDs;             // IDs of detected spheres (sorted)
    std::vector<SpherePosition> positions;  // Positions of detected spheres
    std::vector<int> arrivalOrder;          // Order of arrival (for ARRIVAL_ORDER mode)
    std::string errorMessage;               // Error description if failed
    int64_t timestamp;                      // Timestamp of result
    int cameraUsed;                         // Camera ID used for verification
    int framesProcessed;                    // Number of frames processed
    
    VerificationResult()
        : success(false), spheresDetected(0)
        , timestamp(0), cameraUsed(-1), framesProcessed(0) {}
};

/**
 * SphereVerifier - Main verification component
 * 
 * Thread safety: All public methods are thread-safe
 * 
 * Capture Management:
 * - Implements intelligent device capture reuse to avoid conflicts
 * - Tracks active camera captures in m_activeCaptures vector
 * - When a verification starts, checks if camera is already being captured
 * - If already captured, reuses the existing capture stream
 * - If not captured, starts capture and releases it when done
 * - Prevents "device already in use" conflicts when multiple verifications
 *   run simultaneously on the same camera
 */
class SphereVerifier {
public:
    /**
     * Constructor
     * @param videoHub VideoHub client for camera routing
     * @param inferenceEngine Inference engine for ball detection
     */
    SphereVerifier(VideoHubClient* videoHub, InferenceEngine* inferenceEngine);
    ~SphereVerifier();
    
    // Non-copyable
    SphereVerifier(const SphereVerifier&) = delete;
    SphereVerifier& operator=(const SphereVerifier&) = delete;
    
    /**
     * Execute verification with specified mode and configuration
     * @param mode Verification mode to use
     * @param config Configuration parameters
     * @return Verification result
     */
    VerificationResult Execute(VerificationMode mode, const VerificationConfig& config);
    
    /**
     * Convenience method: Check if all expected spheres are present
     * @param cameraID Camera to use (1-12)
     * @param expectedCount Number of spheres expected
     * @param timeoutMs Maximum time to wait (default 5000ms)
     * @return Verification result
     */
    VerificationResult CheckPresence(int cameraID, int expectedCount, int timeoutMs = 5000);
    
    /**
     * Convenience method: Capture current positions of all spheres
     * @param cameraID Camera to use (1-12)
     * @param timeoutMs Maximum time to wait (default 5000ms)
     * @return Verification result with positions
     */
    VerificationResult CapturePositions(int cameraID, int timeoutMs = 5000);
    
    /**
     * Convenience method: Wait for spheres to arrive at finish line
     * @param cameraID Camera to use (1-12)
     * @param expectedCount Number of spheres expected
     * @param timeoutMs Maximum time to wait (default 60000ms)
     * @return Verification result with arrival order
     */
    VerificationResult WaitForArrivals(int cameraID, int expectedCount, int timeoutMs = 60000);
    
    /**
     * Set camera frame callback for direct frame access
     * Used internally to capture frames from specific camera
     */
    void SetFrameCallback(std::function<void(int cameraID, void* cudaBuffer, 
                                             unsigned int width, unsigned int height)> callback);
    
    /**
     * Check if verifier is ready (has required dependencies)
     */
    bool IsReady() const;
    
    /**
     * Get last error message
     */
    const std::string& GetLastError() const { return m_lastError; }
    
    /**
     * Get list of cameras currently being captured (for debugging/diagnostics)
     */
    std::vector<int> GetActiveCaptures() const;

private:
    // Dependencies
    VideoHubClient* m_videoHub;
    InferenceEngine* m_inferenceEngine;
    
    // State
    std::string m_lastError;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_ready;
    
    // Frame capture state
    std::function<void(int, void*, unsigned int, unsigned int)> m_frameCallback;
    
    // Active capture tracking for smart reuse
    std::vector<int> m_activeCaptures;  // List of camera IDs currently being captured
    
    // Internal implementation methods
    VerificationResult ExecutePresenceCheck(const VerificationConfig& config);
    VerificationResult ExecutePositionSnapshot(const VerificationConfig& config);
    VerificationResult ExecuteArrivalOrder(const VerificationConfig& config);
    VerificationResult ExecuteCheckpointPass(const VerificationConfig& config);
    
    // Helper methods
    bool RouteCamera(int cameraID);
    std::vector<BallDetection> CaptureAndDetect(int cameraID, int frameCount, int timeoutMs);
    std::vector<SpherePosition> FilterAndConsolidate(const std::vector<BallDetection>& detections,
                                                       float confidenceThreshold);
    bool WaitForStableDetections(int cameraID, int expectedCount, float confidenceThreshold,
                                 int consecutiveFrames, int timeoutMs,
                                 std::vector<SpherePosition>& outPositions);
    void SetError(const std::string& error);
    int64_t GetCurrentTimeMs() const;
    
    // Capture management methods for smart reuse
    bool IsCameraBeingCaptured(int cameraID) const;
    bool StartCameraCapture(int cameraID);
    void StopCameraCapture(int cameraID);
    
    // Stub mode flag for when InferenceEngine is in stub mode
    bool m_stubMode;
};

} // namespace Verification
