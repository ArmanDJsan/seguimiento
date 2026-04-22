/**
 * RouteMapper.cpp
 * 
 * Implementation of track route mapping for PTZ camera coordination
 */

#include "RouteMapper.h"
#include "../json.hpp"
#include "../utils/Logger.h"
#include <fstream>
#include <algorithm>
#include <limits>

using json = nlohmann::json;

namespace Tracking {

RouteMapper::RouteMapper()
    : m_totalLength(0.0f)
    , m_lastProgress(0.0f)
    , m_checkpointCallback(nullptr)
{
    // Default checkpoints at 25%, 50%, 75%, and finish
    m_checkpoints = {
        Checkpoint(25.0f, "QUARTER"),
        Checkpoint(50.0f, "HALF"),
        Checkpoint(75.0f, "THREE_QUARTER"),
        Checkpoint(100.0f, "FINISH")
    };
}

bool RouteMapper::LoadRoute(const std::string& filePath) {
    try {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            Logger::Error("RouteMapper: Failed to open route file: " + filePath);
            return false;
        }
        
        json config = json::parse(file);
        
        // Parse waypoints
        if (config.contains("waypoints") && config["waypoints"].is_array()) {
            m_waypoints.clear();
            for (const auto& wp : config["waypoints"]) {
                Waypoint waypoint;
                waypoint.x = wp.value("x", 0.0f);
                waypoint.y = wp.value("y", 0.5f);
                waypoint.progress_pct = wp.value("progress_pct", 0.0f);
                waypoint.name = wp.value("name", "");
                m_waypoints.push_back(waypoint);
            }
        }
        
        // Parse custom checkpoints if defined
        if (config.contains("checkpoints") && config["checkpoints"].is_array()) {
            m_checkpoints.clear();
            for (const auto& cp : config["checkpoints"]) {
                Checkpoint checkpoint;
                checkpoint.progress_pct = cp.value("progress_pct", 0.0f);
                checkpoint.name = cp.value("name", "");
                checkpoint.crossed = false;
                m_checkpoints.push_back(checkpoint);
            }
        }
        
        // Build segments from waypoints
        BuildSegments();
        
        Logger::Info("RouteMapper: Loaded route with " + 
                    std::to_string(m_waypoints.size()) + " waypoints, " +
                    std::to_string(m_checkpoints.size()) + " checkpoints");
        
        return true;
    }
    catch (const std::exception& e) {
        Logger::Error("RouteMapper: Failed to parse route file: " + std::string(e.what()));
        return false;
    }
}

void RouteMapper::SetRoute(const std::vector<Waypoint>& waypoints) {
    m_waypoints = waypoints;
    BuildSegments();
}

void RouteMapper::BuildSegments() {
    m_segments.clear();
    m_totalLength = 0.0f;
    
    if (m_waypoints.size() < 2) {
        // Create default linear route if no waypoints
        if (m_waypoints.empty()) {
            m_waypoints = {
                Waypoint(0.0f, 0.5f, 0.0f, "START"),
                Waypoint(1.0f, 0.5f, 100.0f, "FINISH")
            };
        }
        return;
    }
    
    // Build segments between consecutive waypoints
    for (size_t i = 0; i < m_waypoints.size() - 1; ++i) {
        const auto& wp1 = m_waypoints[i];
        const auto& wp2 = m_waypoints[i + 1];
        
        float length = Distance(wp1.x, wp1.y, wp2.x, wp2.y);
        
        TrackSegment segment(
            static_cast<int>(i),
            static_cast<int>(i + 1),
            length,
            wp1.progress_pct,
            wp2.progress_pct
        );
        
        m_segments.push_back(segment);
        m_totalLength += length;
    }
}

float RouteMapper::MapToProgress(float centroidX, float centroidY) const {
    if (m_waypoints.empty()) {
        // Default: simple linear mapping based on X coordinate
        return centroidX * 100.0f;
    }
    
    if (m_segments.empty()) {
        return centroidX * 100.0f;
    }
    
    // Find nearest segment
    int nearestIdx = GetNearestSegment(centroidX, centroidY);
    if (nearestIdx < 0 || nearestIdx >= static_cast<int>(m_segments.size())) {
        return centroidX * 100.0f;
    }
    
    // Project point onto segment and interpolate progress
    float t = ProjectOntoSegment(centroidX, centroidY, nearestIdx);
    const auto& seg = m_segments[nearestIdx];
    
    // Clamp t to [0, 1]
    t = std::max(0.0f, std::min(1.0f, t));
    
    // Interpolate progress along segment
    float progress = seg.startProgress + t * (seg.endProgress - seg.startProgress);
    
    // Clamp to valid range
    return std::max(0.0f, std::min(100.0f, progress));
}

int RouteMapper::GetNearestSegment(float centroidX, float centroidY) const {
    if (m_segments.empty()) {
        return -1;
    }
    
    float minDist = std::numeric_limits<float>::max();
    int nearestIdx = 0;
    
    for (size_t i = 0; i < m_segments.size(); ++i) {
        float dist = DistanceToSegment(centroidX, centroidY, static_cast<int>(i));
        if (dist < minDist) {
            minDist = dist;
            nearestIdx = static_cast<int>(i);
        }
    }
    
    return nearestIdx;
}

float RouteMapper::DistanceToSegment(float x, float y, int segmentIdx) const {
    if (segmentIdx < 0 || segmentIdx >= static_cast<int>(m_segments.size())) {
        return std::numeric_limits<float>::max();
    }
    
    const auto& seg = m_segments[segmentIdx];
    const auto& wp1 = m_waypoints[seg.startIdx];
    const auto& wp2 = m_waypoints[seg.endIdx];
    
    // Vector from wp1 to wp2
    float dx = wp2.x - wp1.x;
    float dy = wp2.y - wp1.y;
    float segLenSq = dx * dx + dy * dy;
    
    if (segLenSq < 1e-10f) {
        // Degenerate segment (point)
        return Distance(x, y, wp1.x, wp1.y);
    }
    
    // Parameter t for projection onto line
    float t = ((x - wp1.x) * dx + (y - wp1.y) * dy) / segLenSq;
    t = std::max(0.0f, std::min(1.0f, t));
    
    // Closest point on segment
    float projX = wp1.x + t * dx;
    float projY = wp1.y + t * dy;
    
    return Distance(x, y, projX, projY);
}

float RouteMapper::ProjectOntoSegment(float x, float y, int segmentIdx) const {
    if (segmentIdx < 0 || segmentIdx >= static_cast<int>(m_segments.size())) {
        return 0.0f;
    }
    
    const auto& seg = m_segments[segmentIdx];
    const auto& wp1 = m_waypoints[seg.startIdx];
    const auto& wp2 = m_waypoints[seg.endIdx];
    
    // Vector from wp1 to wp2
    float dx = wp2.x - wp1.x;
    float dy = wp2.y - wp1.y;
    float segLenSq = dx * dx + dy * dy;
    
    if (segLenSq < 1e-10f) {
        return 0.0f;
    }
    
    // Parameter t for projection onto line
    float t = ((x - wp1.x) * dx + (y - wp1.y) * dy) / segLenSq;
    return t;
}

bool RouteMapper::HasCrossedMilestone(float lastProgress, float currentProgress, float milestone) const {
    // Check if milestone was crossed in either direction
    return (lastProgress < milestone && currentProgress >= milestone) ||
           (lastProgress > milestone && currentProgress <= milestone);
}

void RouteMapper::SetCheckpointCallback(CheckpointCallback callback) {
    m_checkpointCallback = callback;
}

void RouteMapper::UpdateProgress(float progress, int sphereCount) {
    // Check each checkpoint
    for (auto& checkpoint : m_checkpoints) {
        if (!checkpoint.crossed && 
            HasCrossedMilestone(m_lastProgress, progress, checkpoint.progress_pct)) {
            checkpoint.crossed = true;
            
            Logger::Info("RouteMapper: Checkpoint crossed - " + checkpoint.name + 
                        " (" + std::to_string(checkpoint.progress_pct) + "%)");
            
            // Fire callback if set
            if (m_checkpointCallback) {
                m_checkpointCallback(checkpoint, sphereCount);
            }
        }
    }
    
    m_lastProgress = progress;
}

void RouteMapper::ResetCheckpoints() {
    for (auto& checkpoint : m_checkpoints) {
        checkpoint.crossed = false;
    }
    m_lastProgress = 0.0f;
}

float RouteMapper::Distance(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace Tracking
