/**
 * PositionMapper.cpp
 * 
 * Implementation of homography-based coordinate transformation
 */

#include "PositionMapper.h"
#include "../utils/Logger.h"
#include "../json.hpp"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <unordered_map>

using json = nlohmann::json;

PositionMapper::PositionMapper()
    : m_isCalibrated(false)
{
    // Initialize default track bounds
    m_trackBounds.xMin = 0.0f;
    m_trackBounds.xMax = 100.0f;
    m_trackBounds.yMin = 0.0f;
    m_trackBounds.yMax = 6.0f;
}

PositionMapper::~PositionMapper() = default;

bool PositionMapper::LoadCalibration(const std::string& calibrationPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::ifstream file(calibrationPath);
    if (!file.is_open()) {
        Logger::Warning("PositionMapper: Could not open calibration file: " + calibrationPath);
        Logger::Info("PositionMapper: Using identity matrices (no transformation)");
        
        // Initialize with identity homographies for fallback
        for (int i = 0; i < kMaxCameras; ++i) {
            auto& hd = m_homographies[i];
            hd.H = {1.0f, 0.0f, 0.0f,   // Identity matrix
                    0.0f, 1.0f, 0.0f,
                    0.0f, 0.0f, 1.0f};
            hd.H_inv = hd.H;
            hd.coverage.cameraID = i;
            hd.coverage.xMin = 0.0f;
            hd.coverage.xMax = 100.0f;
            hd.coverage.yMin = 0.0f;
            hd.coverage.yMax = 6.0f;
            hd.coverage.isZenith = (i >= kStreamingCameras);
            hd.coverage.confidenceWeight = hd.coverage.isZenith ? 1.5f : 1.0f;
            hd.isValid = true;
        }
        m_isCalibrated = true;
        return true;
    }
    
    try {
        json j;
        file >> j;
        
        // Parse each camera's homography
        for (int camIdx = 0; camIdx < kMaxCameras; ++camIdx) {
            std::string camKey;
            if (camIdx < kStreamingCameras) {
                char buf[16];
                snprintf(buf, sizeof(buf), "camera_%d", camIdx + 1);
                camKey = buf;
            } else {
                char buf[16];
                snprintf(buf, sizeof(buf), "radar_%d", camIdx - kStreamingCameras + 1);
                camKey = buf;
            }
            
            auto& hd = m_homographies[camIdx];
            hd.coverage.cameraID = camIdx;
            hd.coverage.isZenith = (camIdx >= kStreamingCameras);
            hd.coverage.confidenceWeight = hd.coverage.isZenith ? 1.5f : 1.0f;
            
            if (j.contains(camKey) && j[camKey].is_object()) {
                auto& camData = j[camKey];
                
                // Parse homography matrix (3x3)
                if (camData.contains("homography") && camData["homography"].is_array()) {
                    auto& hMatrix = camData["homography"];
                    if (hMatrix.size() == 3) {
                        int idx = 0;
                        for (int row = 0; row < 3; ++row) {
                            if (hMatrix[row].is_array() && hMatrix[row].size() == 3) {
                                for (int col = 0; col < 3; ++col) {
                                    hd.H[idx++] = hMatrix[row][col].get<float>();
                                }
                            }
                        }
                        hd.isValid = true;
                        ComputeInverseHomography(camIdx);
                    }
                }
                
                // Parse coverage region
                if (camData.contains("coverage_region") && camData["coverage_region"].is_object()) {
                    auto& coverage = camData["coverage_region"];
                    if (coverage.contains("x_min")) hd.coverage.xMin = coverage["x_min"].get<float>();
                    if (coverage.contains("x_max")) hd.coverage.xMax = coverage["x_max"].get<float>();
                    if (coverage.contains("y_min")) hd.coverage.yMin = coverage["y_min"].get<float>();
                    if (coverage.contains("y_max")) hd.coverage.yMax = coverage["y_max"].get<float>();
                }
                
                // Optional confidence weight override
                if (camData.contains("confidence_weight")) {
                    hd.coverage.confidenceWeight = camData["confidence_weight"].get<float>();
                }
            } else {
                // Camera not in calibration file, use identity
                hd.H = {1.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f,
                        0.0f, 0.0f, 1.0f};
                hd.H_inv = hd.H;
                hd.coverage.xMin = 0.0f;
                hd.coverage.xMax = 100.0f;
                hd.coverage.yMin = 0.0f;
                hd.coverage.yMax = 6.0f;
                hd.isValid = true;
            }
        }
        
        m_isCalibrated = true;
        Logger::Info("PositionMapper: Loaded calibration from " + calibrationPath);
        return true;
        
    } catch (const json::exception& e) {
        Logger::Error("PositionMapper: JSON parse error: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        Logger::Error("PositionMapper: Error loading calibration: " + std::string(e.what()));
        return false;
    }
}

void PositionMapper::SetTrackBounds(const TrackBounds& bounds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_trackBounds = bounds;
}

GlobalPosition PositionMapper::PixelToGlobal(int cameraID, float pixelX, float pixelY) const {
    GlobalPosition result;
    result.sourceCameraID = cameraID;
    
    if (cameraID < 0 || cameraID >= kMaxCameras) {
        Logger::Warning("PositionMapper: Invalid camera ID: " + std::to_string(cameraID));
        return result;
    }
    
    const auto& hd = m_homographies[cameraID];
    if (!hd.isValid) {
        Logger::Warning("PositionMapper: Camera " + std::to_string(cameraID) + " not calibrated");
        return result;
    }
    
    // Apply homography transformation
    ApplyHomography(hd.H, pixelX, pixelY, result.Xg, result.Yg);
    result.confidence = 1.0f;  // Will be set by caller
    
    return result;
}

bool PositionMapper::GlobalToPixel(int cameraID, float Xg, float Yg,
                                    float& outPixelX, float& outPixelY) const {
    if (cameraID < 0 || cameraID >= kMaxCameras) {
        return false;
    }
    
    const auto& hd = m_homographies[cameraID];
    if (!hd.isValid) {
        return false;
    }
    
    // Apply inverse homography
    ApplyHomography(hd.H_inv, Xg, Yg, outPixelX, outPixelY);
    
    // Check if within normalized pixel range
    return (outPixelX >= 0.0f && outPixelX <= 1.0f &&
            outPixelY >= 0.0f && outPixelY <= 1.0f);
}

std::vector<GlobalPosition> PositionMapper::BatchTransform(
    int cameraID,
    const std::vector<std::tuple<float, float, float, int>>& pixelPositions,
    int64_t timestamp) const {
    
    std::vector<GlobalPosition> results;
    results.reserve(pixelPositions.size());
    
    if (cameraID < 0 || cameraID >= kMaxCameras) {
        return results;
    }
    
    const auto& hd = m_homographies[cameraID];
    if (!hd.isValid) {
        return results;
    }
    
    for (const auto& [px, py, conf, ballID] : pixelPositions) {
        GlobalPosition pos;
        pos.ballID = ballID;
        pos.confidence = conf;
        pos.sourceCameraID = cameraID;
        pos.timestamp = timestamp;
        
        ApplyHomography(hd.H, px, py, pos.Xg, pos.Yg);
        
        // Only include positions within track bounds
        if (ValidateBounds(pos)) {
            results.push_back(pos);
        }
    }
    
    return results;
}

std::vector<GlobalPosition> PositionMapper::FuseOverlappingDetections(
    const std::vector<GlobalPosition>& positions) const {
    
    if (positions.empty()) {
        return {};
    }
    
    // Group positions by ball ID
    std::unordered_map<int, std::vector<GlobalPosition>> byBallID;
    for (const auto& pos : positions) {
        byBallID[pos.ballID].push_back(pos);
    }
    
    std::vector<GlobalPosition> fusedResults;
    fusedResults.reserve(byBallID.size());
    
    for (auto& [ballID, ballPositions] : byBallID) {
        if (ballPositions.size() == 1) {
            // Single detection, no fusion needed
            fusedResults.push_back(ballPositions[0]);
        } else {
            // Multiple detections - weighted average
            float totalWeight = 0.0f;
            float weightedXg = 0.0f;
            float weightedYg = 0.0f;
            float maxConfidence = 0.0f;
            int64_t latestTimestamp = 0;
            int bestCamera = -1;
            
            for (const auto& pos : ballPositions) {
                float weight = CalculateFusionWeight(pos);
                totalWeight += weight;
                weightedXg += pos.Xg * weight;
                weightedYg += pos.Yg * weight;
                
                if (pos.confidence > maxConfidence) {
                    maxConfidence = pos.confidence;
                    bestCamera = pos.sourceCameraID;
                }
                if (pos.timestamp > latestTimestamp) {
                    latestTimestamp = pos.timestamp;
                }
            }
            
            if (totalWeight > 0.0f) {
                GlobalPosition fused;
                fused.ballID = ballID;
                fused.Xg = weightedXg / totalWeight;
                fused.Yg = weightedYg / totalWeight;
                fused.confidence = maxConfidence;
                fused.sourceCameraID = bestCamera;
                fused.timestamp = latestTimestamp;
                fusedResults.push_back(fused);
            }
        }
    }
    
    return fusedResults;
}

bool PositionMapper::ValidateBounds(const GlobalPosition& pos) const {
    return (pos.Xg >= m_trackBounds.xMin && pos.Xg <= m_trackBounds.xMax &&
            pos.Yg >= m_trackBounds.yMin && pos.Yg <= m_trackBounds.yMax);
}

CameraCoverage PositionMapper::GetCameraCoverage(int cameraID) const {
    if (cameraID >= 0 && cameraID < kMaxCameras) {
        return m_homographies[cameraID].coverage;
    }
    return CameraCoverage{};
}

bool PositionMapper::IsZenithCamera(int cameraID) const {
    return cameraID >= kStreamingCameras && cameraID < kMaxCameras;
}

bool PositionMapper::ComputeInverseHomography(int cameraID) {
    auto& hd = m_homographies[cameraID];
    const auto& H = hd.H;
    
    // Compute 3x3 matrix inverse using Cramer's rule
    // H = [a b c; d e f; g h i]
    float a = H[0], b = H[1], c = H[2];
    float d = H[3], e = H[4], f = H[5];
    float g = H[6], h = H[7], i = H[8];
    
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    
    if (std::abs(det) < 1e-8f) {
        Logger::Warning("PositionMapper: Singular homography matrix for camera " + std::to_string(cameraID));
        hd.H_inv = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        return false;
    }
    
    float invDet = 1.0f / det;
    
    hd.H_inv[0] = (e * i - f * h) * invDet;
    hd.H_inv[1] = (c * h - b * i) * invDet;
    hd.H_inv[2] = (b * f - c * e) * invDet;
    hd.H_inv[3] = (f * g - d * i) * invDet;
    hd.H_inv[4] = (a * i - c * g) * invDet;
    hd.H_inv[5] = (c * d - a * f) * invDet;
    hd.H_inv[6] = (d * h - e * g) * invDet;
    hd.H_inv[7] = (b * g - a * h) * invDet;
    hd.H_inv[8] = (a * e - b * d) * invDet;
    
    return true;
}

void PositionMapper::ApplyHomography(const std::array<float, 9>& H,
                                      float inX, float inY,
                                      float& outX, float& outY) const {
    // Homogeneous coordinates transformation
    // [outX']   [H0 H1 H2]   [inX]
    // [outY'] = [H3 H4 H5] * [inY]
    // [w    ]   [H6 H7 H8]   [1  ]
    
    float w = H[6] * inX + H[7] * inY + H[8];
    
    if (std::abs(w) < 1e-8f) {
        // Point at infinity, use large values
        outX = (H[0] * inX + H[1] * inY + H[2]) * 1e6f;
        outY = (H[3] * inX + H[4] * inY + H[5]) * 1e6f;
    } else {
        outX = (H[0] * inX + H[1] * inY + H[2]) / w;
        outY = (H[3] * inX + H[4] * inY + H[5]) / w;
    }
}

float PositionMapper::CalculateFusionWeight(const GlobalPosition& pos) const {
    // Base weight from camera type (zenith cameras get higher weight)
    float baseWeight = IsZenithCamera(pos.sourceCameraID) ? 1.5f : 1.0f;
    
    // Confidence-based weight
    float confWeight = pos.confidence;
    
    // Coverage center bonus: detections near center of camera FOV are more reliable
    const auto& coverage = m_homographies[pos.sourceCameraID].coverage;
    float centerX = (coverage.xMin + coverage.xMax) / 2.0f;
    float centerY = (coverage.yMin + coverage.yMax) / 2.0f;
    float rangeX = (coverage.xMax - coverage.xMin) / 2.0f;
    float rangeY = (coverage.yMax - coverage.yMin) / 2.0f;
    
    float distFromCenterX = std::abs(pos.Xg - centerX) / (rangeX + 0.001f);
    float distFromCenterY = std::abs(pos.Yg - centerY) / (rangeY + 0.001f);
    float centerBonus = 1.0f - 0.3f * std::min(1.0f, (distFromCenterX + distFromCenterY) / 2.0f);
    
    return baseWeight * confWeight * centerBonus;
}
