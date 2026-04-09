/**
 * BallTracker.cpp
 * 
 * Implementation of multi-object ball tracker with Kalman filtering
 */

#include "BallTracker.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <chrono>

BallTracker::BallTracker(const BallTrackerConfig& config)
    : m_config(config)
    , m_initialized(false)
    , m_lastUpdateTime(0)
{
}

BallTracker::~BallTracker() = default;

bool BallTracker::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Initialize all track states
    for (int i = 0; i < kMaxBalls; ++i) {
        auto& track = m_tracks[i];
        track.kalman.Configure(
            m_config.kalmanProcessNoisePos,
            m_config.kalmanProcessNoiseVel,
            m_config.kalmanMeasurementNoise,
            1.0f / 30.0f  // 30 FPS
        );
        track.kalman.Reset();
        track.ball.ballID = i + 1;  // Ball IDs are 1-indexed
        track.ball.trackConfidence = 0.0f;
        track.isActive = false;
    }
    
    m_initialized = true;
    Logger::Info("BallTracker: Initialized with " + std::to_string(m_config.numBalls) + " balls");
    return true;
}

void BallTracker::Update(const std::vector<GlobalPosition>& detections, int64_t timestamp) {
    if (!m_initialized) {
        Logger::Warning("BallTracker: Update called before initialization");
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Filter detections by confidence
    std::vector<GlobalPosition> validDetections;
    for (const auto& det : detections) {
        if (det.confidence >= m_config.minConfidenceThreshold) {
            validDetections.push_back(det);
        }
    }
    
    // Compute assignment between tracks and detections
    auto assignment = ComputeAssignment(validDetections);
    
    // Update assigned tracks
    for (const auto& [trackIdx, detIdx] : assignment.assignments) {
        UpdateTrackWithDetection(trackIdx, validDetections[detIdx], timestamp);
    }
    
    // Handle unassigned tracks (occlusion)
    HandleUnassignedTracks(assignment.unassignedTracks, timestamp);
    
    // Handle unassigned detections (new balls or ID recovery)
    HandleUnassignedDetections(validDetections, assignment.unassignedDetections, timestamp);
    
    m_lastUpdateTime = timestamp;
}

void BallTracker::Predict() {
    if (!m_initialized) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (int i = 0; i < kMaxBalls; ++i) {
        auto& track = m_tracks[i];
        if (track.isActive) {
            track.kalman.Predict();
            
            float x, y, vx, vy;
            track.kalman.GetPosition(x, y);
            track.kalman.GetVelocity(vx, vy);
            track.kalman.PredictPosition(1.0f / 30.0f, track.ball.predictedXg, track.ball.predictedYg);
            
            track.ball.Xg = x;
            track.ball.Yg = y;
            track.ball.Vx = vx;
            track.ball.Vy = vy;
            track.ball.positionUncertainty = track.kalman.GetPositionUncertainty();
            track.ball.framesSinceUpdate++;
            track.ball.isVisible = false;
            
            // Decay confidence when not seen
            track.ball.trackConfidence *= 0.95f;
        }
    }
}

std::vector<TrackedBall> BallTracker::GetAllBalls() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<TrackedBall> result;
    result.reserve(kMaxBalls);
    
    for (int i = 0; i < kMaxBalls; ++i) {
        if (m_tracks[i].isActive) {
            result.push_back(m_tracks[i].ball);
        }
    }
    
    return result;
}

TrackedBall BallTracker::GetBall(int ballID) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (ballID >= 1 && ballID <= kMaxBalls) {
        return m_tracks[ballID - 1].ball;
    }
    return TrackedBall();
}

TrackedBall BallTracker::GetLeader() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    TrackedBall leader;
    float maxX = -std::numeric_limits<float>::max();
    
    for (int i = 0; i < kMaxBalls; ++i) {
        if (m_tracks[i].isActive && m_tracks[i].ball.Xg > maxX) {
            maxX = m_tracks[i].ball.Xg;
            leader = m_tracks[i].ball;
        }
    }
    
    return leader;
}

std::vector<int> BallTracker::GetRanking() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Collect active balls with their positions
    std::vector<std::pair<int, float>> ballPositions;
    for (int i = 0; i < kMaxBalls; ++i) {
        if (m_tracks[i].isActive) {
            ballPositions.emplace_back(m_tracks[i].ball.ballID, m_tracks[i].ball.Xg);
        }
    }
    
    // Sort by X position descending (leader first)
    std::sort(ballPositions.begin(), ballPositions.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    // Extract ball IDs
    std::vector<int> ranking;
    ranking.reserve(ballPositions.size());
    for (const auto& [id, pos] : ballPositions) {
        ranking.push_back(id);
    }
    
    return ranking;
}

void BallTracker::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (int i = 0; i < kMaxBalls; ++i) {
        m_tracks[i].kalman.Reset();
        m_tracks[i].ball = TrackedBall();
        m_tracks[i].ball.ballID = i + 1;
        m_tracks[i].isActive = false;
    }
    
    m_lastUpdateTime = 0;
    Logger::Info("BallTracker: Reset all tracks");
}

int BallTracker::GetVisibleBallCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    int count = 0;
    for (int i = 0; i < kMaxBalls; ++i) {
        if (m_tracks[i].isActive && m_tracks[i].ball.isVisible) {
            count++;
        }
    }
    return count;
}

BallTracker::AssignmentResult BallTracker::ComputeAssignment(
    const std::vector<GlobalPosition>& detections) {
    
    AssignmentResult result;
    
    if (detections.empty()) {
        // All active tracks are unassigned
        for (int i = 0; i < kMaxBalls; ++i) {
            if (m_tracks[i].isActive) {
                result.unassignedTracks.push_back(i);
            }
        }
        return result;
    }
    
    // Count active tracks
    std::vector<int> activeTracks;
    for (int i = 0; i < kMaxBalls; ++i) {
        if (m_tracks[i].isActive) {
            activeTracks.push_back(i);
        }
    }
    
    if (activeTracks.empty()) {
        // All detections are unassigned
        for (size_t i = 0; i < detections.size(); ++i) {
            result.unassignedDetections.push_back(static_cast<int>(i));
        }
        return result;
    }
    
    // Build cost matrix
    const float kMaxCost = 1e6f;
    size_t numTracks = activeTracks.size();
    size_t numDets = detections.size();
    size_t n = std::max(numTracks, numDets);
    
    std::vector<std::vector<float>> costMatrix(n, std::vector<float>(n, kMaxCost));
    
    for (size_t i = 0; i < numTracks; ++i) {
        for (size_t j = 0; j < numDets; ++j) {
            float dist = ComputeDistance(m_tracks[activeTracks[i]].ball, detections[j]);
            if (dist <= m_config.associationGateDistance) {
                costMatrix[i][j] = dist;
            }
            // Distances beyond gate remain at kMaxCost
        }
    }
    
    // Run Hungarian algorithm
    std::vector<int> assignment(n, -1);
    HungarianAlgorithm(costMatrix, assignment);
    
    // Parse assignments
    std::vector<bool> detAssigned(numDets, false);
    std::vector<bool> trackAssigned(numTracks, false);
    
    for (size_t i = 0; i < numTracks; ++i) {
        int j = assignment[i];
        if (j >= 0 && static_cast<size_t>(j) < numDets && costMatrix[i][j] < kMaxCost) {
            result.assignments.emplace_back(activeTracks[i], j);
            detAssigned[j] = true;
            trackAssigned[i] = true;
        }
    }
    
    // Collect unassigned
    for (size_t i = 0; i < numTracks; ++i) {
        if (!trackAssigned[i]) {
            result.unassignedTracks.push_back(activeTracks[i]);
        }
    }
    for (size_t j = 0; j < numDets; ++j) {
        if (!detAssigned[j]) {
            result.unassignedDetections.push_back(static_cast<int>(j));
        }
    }
    
    return result;
}

float BallTracker::ComputeDistance(const TrackedBall& track, const GlobalPosition& detection) const {
    // If detection has a known ball ID, add bonus for matching ID
    float dx = track.Xg - detection.Xg;
    float dy = track.Yg - detection.Yg;
    float dist = std::sqrt(dx * dx + dy * dy);
    
    // ID matching bonus
    if (detection.ballID > 0 && detection.ballID == track.ballID) {
        dist *= 0.5f;  // 50% distance reduction for matching ID
    }
    
    return dist;
}

void BallTracker::UpdateTrackWithDetection(int trackIdx, const GlobalPosition& detection, 
                                            int64_t timestamp) {
    auto& track = m_tracks[trackIdx];
    
    // Update Kalman filter
    track.kalman.Update(detection.Xg, detection.Yg);
    
    // Update ball state
    float x, y, vx, vy;
    track.kalman.GetPosition(x, y);
    track.kalman.GetVelocity(vx, vy);
    track.kalman.PredictPosition(1.0f / 30.0f, track.ball.predictedXg, track.ball.predictedYg);
    
    track.ball.Xg = x;
    track.ball.Yg = y;
    track.ball.Vx = vx;
    track.ball.Vy = vy;
    track.ball.positionUncertainty = track.kalman.GetPositionUncertainty();
    track.ball.trackAge++;
    track.ball.framesSinceUpdate = 0;
    track.ball.isVisible = true;
    track.ball.lastSourceCamera = detection.sourceCameraID;
    track.ball.lastUpdateTime = timestamp;
    
    // Increase confidence when consistently seen
    track.ball.trackConfidence = std::min(1.0f, 
        track.ball.trackConfidence + 0.1f * detection.confidence);
}

void BallTracker::HandleUnassignedTracks(const std::vector<int>& unassigned, int64_t timestamp) {
    for (int trackIdx : unassigned) {
        auto& track = m_tracks[trackIdx];
        
        // Predict position using Kalman
        track.kalman.Predict();
        
        float x, y, vx, vy;
        track.kalman.GetPosition(x, y);
        track.kalman.GetVelocity(vx, vy);
        track.kalman.PredictPosition(1.0f / 30.0f, track.ball.predictedXg, track.ball.predictedYg);
        
        track.ball.Xg = x;
        track.ball.Yg = y;
        track.ball.Vx = vx;
        track.ball.Vy = vy;
        track.ball.positionUncertainty = track.kalman.GetPositionUncertainty();
        track.ball.framesSinceUpdate++;
        track.ball.isVisible = false;
        
        // Decay confidence
        track.ball.trackConfidence *= 0.9f;
        
        // Mark track as lost if too many frames without update
        if (track.ball.framesSinceUpdate > m_config.maxOcclusionFrames) {
            track.isActive = false;
            Logger::Debug("BallTracker: Lost track for ball " + std::to_string(track.ball.ballID));
        }
    }
}

void BallTracker::HandleUnassignedDetections(const std::vector<GlobalPosition>& detections,
                                              const std::vector<int>& unassigned, 
                                              int64_t timestamp) {
    for (int detIdx : unassigned) {
        const auto& det = detections[detIdx];
        
        // Try to find an inactive track to assign this detection
        int targetTrack = -1;
        
        // If detection has a ball ID, try to reactivate that specific track
        if (det.ballID > 0 && det.ballID <= kMaxBalls) {
            int idx = det.ballID - 1;
            if (!m_tracks[idx].isActive) {
                targetTrack = idx;
            }
        }
        
        // Otherwise find first inactive track
        if (targetTrack < 0) {
            for (int i = 0; i < kMaxBalls; ++i) {
                if (!m_tracks[i].isActive) {
                    targetTrack = i;
                    break;
                }
            }
        }
        
        if (targetTrack >= 0) {
            auto& track = m_tracks[targetTrack];
            track.kalman.Initialize(det.Xg, det.Yg);
            track.ball.ballID = targetTrack + 1;
            track.ball.Xg = det.Xg;
            track.ball.Yg = det.Yg;
            track.ball.Vx = 0;
            track.ball.Vy = 0;
            track.ball.predictedXg = det.Xg;
            track.ball.predictedYg = det.Yg;
            track.ball.trackAge = 0;
            track.ball.framesSinceUpdate = 0;
            track.ball.trackConfidence = det.confidence * 0.5f;
            track.ball.isVisible = true;
            track.ball.lastSourceCamera = det.sourceCameraID;
            track.ball.lastUpdateTime = timestamp;
            track.isActive = true;
            
            Logger::Debug("BallTracker: Started tracking ball " + std::to_string(track.ball.ballID));
        }
    }
}

void BallTracker::HungarianAlgorithm(const std::vector<std::vector<float>>& costMatrix,
                                      std::vector<int>& assignment) {
    // NOTE: This is a simplified GREEDY assignment algorithm, not the true Hungarian algorithm.
    // 
    // Limitations of greedy approach:
    // - May not find optimal global assignment when costs are similar
    // - Can produce suboptimal results with many overlapping detections
    // - Time complexity: O(n²) vs O(n³) for true Hungarian
    //
    // For production with high ball density or frequent occlusions, consider:
    // - scipy.optimize.linear_sum_assignment (via C++ bindings)
    // - dlib::linear_assignment() from dlib library
    // - Eigen-based Hungarian implementation
    //
    // The greedy approach works well for racing scenarios where balls are
    // typically well-separated and moving predictably.
    
    size_t n = costMatrix.size();
    assignment.resize(n, -1);
    std::vector<bool> colUsed(n, false);
    
    // For each row, find minimum cost unassigned column
    for (size_t i = 0; i < n; ++i) {
        float minCost = std::numeric_limits<float>::max();
        int minCol = -1;
        
        for (size_t j = 0; j < n; ++j) {
            if (!colUsed[j] && costMatrix[i][j] < minCost) {
                minCost = costMatrix[i][j];
                minCol = static_cast<int>(j);
            }
        }
        
        if (minCol >= 0 && minCost < 1e5f) {
            assignment[i] = minCol;
            colUsed[minCol] = true;
        }
    }
}
