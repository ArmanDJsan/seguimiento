/**
 * RouteMapper.h
 * 
 * Maps centroid positions to track progress for PTZ camera coordination
 * Supports waypoint-based route definition for race tracks
 * 
 * Features:
 * - Waypoint interpolation for smooth progress mapping
 * - Progress percentage calculation (0-100%)
 * - Milestone/checkpoint detection
 * - Track segment identification
 * 
 * Usage:
 *   RouteMapper mapper;
 *   mapper.LoadRoute("config/track_route.json");
 *   float progress = mapper.MapToProgress(centroid);
 *   if (mapper.HasCrossedMilestone(25.0f, progress)) { ... }
 */

#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <functional>

namespace Tracking {

/**
 * A waypoint on the track route
 */
struct Waypoint {
    float x = 0.0f;              // Normalized X position [0.0, 1.0]
    float y = 0.0f;              // Normalized Y position [0.0, 1.0]
    float progress_pct = 0.0f;   // Progress percentage at this waypoint [0.0, 100.0]
    std::string name;            // Optional waypoint name
    
    Waypoint() = default;
    Waypoint(float x_, float y_, float prog, const std::string& n = "")
        : x(x_), y(y_), progress_pct(prog), name(n) {}
};

/**
 * Track segment between two waypoints
 */
struct TrackSegment {
    int startIdx = 0;            // Start waypoint index
    int endIdx = 0;              // End waypoint index
    float length = 0.0f;         // Segment length (normalized units)
    float startProgress = 0.0f;  // Progress at segment start
    float endProgress = 0.0f;    // Progress at segment end
    
    TrackSegment() = default;
    TrackSegment(int s, int e, float len, float sp, float ep)
        : startIdx(s), endIdx(e), length(len), startProgress(sp), endProgress(ep) {}
};

/**
 * Checkpoint/milestone definition
 */
struct Checkpoint {
    float progress_pct = 0.0f;   // Progress percentage at checkpoint
    std::string name;            // Checkpoint name
    bool crossed = false;        // Has been crossed this session
    
    Checkpoint() = default;
    Checkpoint(float prog, const std::string& n)
        : progress_pct(prog), name(n), crossed(false) {}
};

/**
 * Callback type for checkpoint events
 */
using CheckpointCallback = std::function<void(const Checkpoint& checkpoint, int sphereCount)>;

/**
 * RouteMapper - Maps centroid to track progress
 * 
 * Thread safety: NOT thread-safe, use external synchronization
 */
class RouteMapper {
public:
    /**
     * Constructor
     */
    RouteMapper();
    ~RouteMapper() = default;
    
    /**
     * Load route definition from JSON file
     * @param filePath Path to track_route.json
     * @return true if successful
     */
    bool LoadRoute(const std::string& filePath);
    
    /**
     * Set route from waypoints directly
     * @param waypoints Vector of waypoints defining the route
     */
    void SetRoute(const std::vector<Waypoint>& waypoints);
    
    /**
     * Map centroid position to progress percentage
     * @param centroidX Normalized X position [0.0, 1.0]
     * @param centroidY Normalized Y position [0.0, 1.0]
     * @return Progress percentage [0.0, 100.0]
     */
    float MapToProgress(float centroidX, float centroidY) const;
    
    /**
     * Map centroid result to progress percentage
     * @param centroid Centroid result from InferenceEngine
     * @return Progress percentage [0.0, 100.0]
     */
    template<typename CentroidType>
    float MapToProgress(const CentroidType& centroid) const {
        return MapToProgress(centroid.centroid_x, centroid.centroid_y);
    }
    
    /**
     * Get the nearest track segment for a position
     * @param centroidX Normalized X position
     * @param centroidY Normalized Y position
     * @return Nearest segment index, or -1 if no route
     */
    int GetNearestSegment(float centroidX, float centroidY) const;
    
    /**
     * Check if a milestone has been crossed
     * @param lastProgress Previous progress value
     * @param currentProgress Current progress value
     * @param milestone Milestone percentage to check
     * @return true if milestone was crossed
     */
    bool HasCrossedMilestone(float lastProgress, float currentProgress, float milestone) const;
    
    /**
     * Set checkpoint callback for milestone events
     * @param callback Function to call when checkpoint is crossed
     */
    void SetCheckpointCallback(CheckpointCallback callback);
    
    /**
     * Update with new progress and check for checkpoint crossings
     * @param progress Current progress percentage
     * @param sphereCount Number of spheres detected
     */
    void UpdateProgress(float progress, int sphereCount);
    
    /**
     * Reset all checkpoint crossed flags
     */
    void ResetCheckpoints();
    
    /**
     * Get all waypoints
     */
    const std::vector<Waypoint>& GetWaypoints() const { return m_waypoints; }
    
    /**
     * Get all checkpoints
     */
    const std::vector<Checkpoint>& GetCheckpoints() const { return m_checkpoints; }
    
    /**
     * Check if route is loaded
     */
    bool IsLoaded() const { return !m_waypoints.empty(); }
    
    /**
     * Get track length in normalized units
     */
    float GetTrackLength() const { return m_totalLength; }

private:
    std::vector<Waypoint> m_waypoints;
    std::vector<TrackSegment> m_segments;
    std::vector<Checkpoint> m_checkpoints;
    float m_totalLength;
    float m_lastProgress;
    CheckpointCallback m_checkpointCallback;
    
    // Helper methods
    void BuildSegments();
    float DistanceToSegment(float x, float y, int segmentIdx) const;
    float ProjectOntoSegment(float x, float y, int segmentIdx) const;
    static float Distance(float x1, float y1, float x2, float y2);
};

} // namespace Tracking
