/**
 * PositionMapper.h
 * 
 * Homography-based coordinate transformation from pixel to global coordinates
 * Converts camera pixel positions to track-relative 2D positions (meters)
 * 
 * Architecture:
 * - Pre-calibrated 3x3 homography matrix per camera (16 cameras total)
 * - Pixel (x,y) -> Global (Xg, Yg) transformation
 * - Multi-camera detection fusion for overlapping coverage zones
 * 
 * Performance target: <0.1ms for batch transform of 100 detections
 */

#pragma once

#include <vector>
#include <array>
#include <string>
#include <mutex>
#include <cstdint>

/**
 * Global position in track coordinates (meters from track origin)
 */
struct GlobalPosition {
    int ballID;             // Ball identifier (1-10, or 0 if unknown)
    float Xg;               // X position in meters (along track length)
    float Yg;               // Y position in meters (across track width)
    float confidence;       // Detection confidence [0.0, 1.0]
    int sourceCameraID;     // Camera that detected this position
    int64_t timestamp;      // Milliseconds since epoch
    
    GlobalPosition()
        : ballID(0), Xg(0.0f), Yg(0.0f), confidence(0.0f)
        , sourceCameraID(-1), timestamp(0) {}
        
    GlobalPosition(int ball, float x, float y, float conf, int cam, int64_t ts)
        : ballID(ball), Xg(x), Yg(y), confidence(conf)
        , sourceCameraID(cam), timestamp(ts) {}
};

/**
 * Track boundary definition
 */
struct TrackBounds {
    float xMin = 0.0f;      // Start of track (meters)
    float xMax = 100.0f;    // End of track (meters)
    float yMin = 0.0f;      // Left edge (meters)
    float yMax = 6.0f;      // Right edge (meters)
};

/**
 * Camera coverage region in global coordinates
 */
struct CameraCoverage {
    int cameraID;
    float xMin, xMax;       // Track segment covered
    float yMin, yMax;       // Track width covered
    bool isZenith;          // True for overhead cameras (RADAR_01-04)
    float confidenceWeight; // Weight for fusion (zenith cameras have higher weight)
};

/**
 * PositionMapper - Homography-based coordinate transformation
 * 
 * Thread safety: All public methods are thread-safe
 */
class PositionMapper {
public:
    static constexpr int kMaxCameras = 16;
    static constexpr int kStreamingCameras = 12;  // CAM_01 to CAM_12
    static constexpr int kZenithCameras = 4;      // RADAR_01 to RADAR_04 (fixed overhead)
    
    /**
     * Constructor
     */
    PositionMapper();
    ~PositionMapper();
    
    /**
     * Load calibration data from JSON file
     * @param calibrationPath Path to calibration.json
     * @return true if loaded successfully
     */
    bool LoadCalibration(const std::string& calibrationPath);
    
    /**
     * Set track bounds for validation
     * @param bounds Track boundary definition
     */
    void SetTrackBounds(const TrackBounds& bounds);
    
    /**
     * Check if calibration is loaded and valid
     */
    bool IsCalibrated() const { return m_isCalibrated; }
    
    /**
     * Transform pixel coordinates to global coordinates
     * @param cameraID Camera identifier (0-15)
     * @param pixelX Normalized pixel X [0.0, 1.0]
     * @param pixelY Normalized pixel Y [0.0, 1.0]
     * @return Global position (Xg, Yg)
     */
    GlobalPosition PixelToGlobal(int cameraID, float pixelX, float pixelY) const;
    
    /**
     * Transform global coordinates back to pixel coordinates (inverse)
     * @param cameraID Camera identifier (0-15)
     * @param Xg Global X position (meters)
     * @param Yg Global Y position (meters)
     * @param outPixelX Output normalized pixel X
     * @param outPixelY Output normalized pixel Y
     * @return true if transformation successful and within camera FOV
     */
    bool GlobalToPixel(int cameraID, float Xg, float Yg, 
                       float& outPixelX, float& outPixelY) const;
    
    /**
     * Batch transform multiple detections to global coordinates
     * @param cameraID Camera identifier
     * @param pixelPositions Vector of (x, y, confidence, ballID) tuples
     * @param timestamp Detection timestamp
     * @return Vector of global positions
     */
    std::vector<GlobalPosition> BatchTransform(
        int cameraID,
        const std::vector<std::tuple<float, float, float, int>>& pixelPositions,
        int64_t timestamp) const;
    
    /**
     * Fuse overlapping detections from multiple cameras
     * Uses weighted average based on camera confidence and distance from coverage center
     * @param positions Vector of global positions from multiple cameras
     * @return Fused positions (one per unique ball)
     */
    std::vector<GlobalPosition> FuseOverlappingDetections(
        const std::vector<GlobalPosition>& positions) const;
    
    /**
     * Validate if a global position is within track bounds
     * @param pos Position to validate
     * @return true if within track bounds
     */
    bool ValidateBounds(const GlobalPosition& pos) const;
    
    /**
     * Get camera coverage region
     * @param cameraID Camera identifier
     * @return Coverage region definition
     */
    CameraCoverage GetCameraCoverage(int cameraID) const;
    
    /**
     * Check if a camera is a zenith (overhead) camera
     * @param cameraID Camera identifier
     * @return true if zenith camera (higher confidence weight)
     */
    bool IsZenithCamera(int cameraID) const;

private:
    // Homography matrix: 3x3 stored as array of 9 floats (row-major)
    // H = [h0 h1 h2; h3 h4 h5; h6 h7 h8]
    // [Xg]   [h0 h1 h2]   [px]
    // [Yg] = [h3 h4 h5] * [py]
    // [w ]   [h6 h7 h8]   [1 ]
    // Then Xg_final = Xg/w, Yg_final = Yg/w
    struct HomographyData {
        std::array<float, 9> H;         // Forward homography (pixel -> global)
        std::array<float, 9> H_inv;     // Inverse homography (global -> pixel)
        CameraCoverage coverage;
        bool isValid;
        
        HomographyData() : H{}, H_inv{}, isValid(false) {}
    };
    
    std::array<HomographyData, kMaxCameras> m_homographies;
    TrackBounds m_trackBounds;
    bool m_isCalibrated;
    mutable std::mutex m_mutex;
    
    // Helper methods
    bool ComputeInverseHomography(int cameraID);
    void ApplyHomography(const std::array<float, 9>& H, 
                        float inX, float inY,
                        float& outX, float& outY) const;
    float CalculateFusionWeight(const GlobalPosition& pos) const;
};
