/**
 * BallTracker.h
 * 
 * Multi-object tracker using Kalman filters for stable ball identity
 * Handles occlusions, camera handovers, and detection noise
 * 
 * Architecture:
 * - One Kalman filter per ball (10 balls)
 * - Hungarian algorithm for optimal detection-to-track association
 * - Gated distance threshold to reject outliers
 * - Ranking calculation based on track position (X coordinate)
 * 
 * Performance target: <1ms for full update with 10 balls
 */

#pragma once

#include "KalmanFilter.h"
#include "PositionMapper.h"
#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <cstdint>

/**
 * Configuration for ball tracker
 */
struct BallTrackerConfig {
    int numBalls = 10;                    // Number of balls to track
    float kalmanProcessNoisePos = 0.01f;  // Position process noise
    float kalmanProcessNoiseVel = 0.1f;   // Velocity process noise
    float kalmanMeasurementNoise = 0.5f;  // Measurement noise
    int maxOcclusionFrames = 30;          // Max frames before track is lost (~1s at 30fps)
    float associationGateDistance = 2.0f; // Max distance (meters) for association
    float minConfidenceThreshold = 0.3f;  // Min confidence to accept detection
};

/**
 * Tracked ball state
 */
struct TrackedBall {
    int ballID;                   // Ball identifier (1-10)
    float Xg, Yg;                 // Current global position (meters)
    float Vx, Vy;                 // Current velocity (m/s)
    float predictedXg, predictedYg;  // Predicted position (next frame)
    int trackAge;                 // Frames since track was created
    int framesSinceUpdate;        // Frames since last detection
    float trackConfidence;        // Overall track confidence [0.0, 1.0]
    float positionUncertainty;    // Position uncertainty (meters)
    bool isVisible;               // Currently detected
    int lastSourceCamera;         // Last camera that saw this ball
    int64_t lastUpdateTime;       // Timestamp of last update
    
    TrackedBall()
        : ballID(0), Xg(0), Yg(0), Vx(0), Vy(0)
        , predictedXg(0), predictedYg(0)
        , trackAge(0), framesSinceUpdate(0)
        , trackConfidence(0), positionUncertainty(10.0f)
        , isVisible(false), lastSourceCamera(-1)
        , lastUpdateTime(0) {}
};

/**
 * BallTracker - Multi-object tracker with stable identity
 * 
 * Thread safety: All public methods are thread-safe
 */
class BallTracker {
public:
    static constexpr int kMaxBalls = 10;
    
    /**
     * Constructor
     * @param config Tracker configuration
     */
    explicit BallTracker(const BallTrackerConfig& config = BallTrackerConfig());
    ~BallTracker();
    
    /**
     * Initialize tracker
     * @return true if successful
     */
    bool Initialize();
    
    /**
     * Update tracker with new detections
     * @param detections Global positions from PositionMapper
     * @param timestamp Current timestamp (ms since epoch)
     */
    void Update(const std::vector<GlobalPosition>& detections, int64_t timestamp);
    
    /**
     * Predict all tracks without measurement (occlusion handling)
     * Call when no detections are available
     */
    void Predict();
    
    /**
     * Get all tracked balls
     * @return Vector of tracked ball states
     */
    std::vector<TrackedBall> GetAllBalls() const;
    
    /**
     * Get specific ball by ID
     * @param ballID Ball identifier (1-10)
     * @return Tracked ball state (or default if not found)
     */
    TrackedBall GetBall(int ballID) const;
    
    /**
     * Get leader position (ball with highest X coordinate)
     * @return Leader's tracked state
     */
    TrackedBall GetLeader() const;
    
    /**
     * Get current ranking (balls sorted by X position descending)
     * @return Vector of ball IDs in ranking order (1st place first)
     */
    std::vector<int> GetRanking() const;
    
    /**
     * Reset all tracks
     */
    void Reset();
    
    /**
     * Check if tracker is initialized
     */
    bool IsInitialized() const { return m_initialized; }
    
    /**
     * Get number of visible (currently tracked) balls
     */
    int GetVisibleBallCount() const;
    
    /**
     * Get configuration
     */
    const BallTrackerConfig& GetConfig() const { return m_config; }

private:
    // Track state per ball
    struct TrackState {
        KalmanFilter2D kalman;
        TrackedBall ball;
        bool isActive;
        
        TrackState() : isActive(false) {}
    };
    
    BallTrackerConfig m_config;
    std::array<TrackState, kMaxBalls> m_tracks;
    std::atomic<bool> m_initialized;
    mutable std::mutex m_mutex;
    int64_t m_lastUpdateTime;
    
    // Hungarian algorithm for assignment
    struct AssignmentResult {
        std::vector<std::pair<int, int>> assignments;  // (trackIdx, detectionIdx)
        std::vector<int> unassignedTracks;
        std::vector<int> unassignedDetections;
    };
    
    // Helper methods
    AssignmentResult ComputeAssignment(const std::vector<GlobalPosition>& detections);
    float ComputeDistance(const TrackedBall& track, const GlobalPosition& detection) const;
    void UpdateTrackWithDetection(int trackIdx, const GlobalPosition& detection, int64_t timestamp);
    void HandleUnassignedTracks(const std::vector<int>& unassigned, int64_t timestamp);
    void HandleUnassignedDetections(const std::vector<GlobalPosition>& detections,
                                     const std::vector<int>& unassigned, int64_t timestamp);
    void UpdateRanking();
    void HungarianAlgorithm(const std::vector<std::vector<float>>& costMatrix,
                            std::vector<int>& assignment);
};
