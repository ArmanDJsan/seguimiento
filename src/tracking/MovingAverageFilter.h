/**
 * MovingAverageFilter.h
 * 
 * Smoothing filter for centroid tracking using moving average
 * Implements circular buffer for efficient O(1) updates
 * 
 * Features:
 * - Configurable window size (default 5 frames)
 * - Separate filtering for X, Y, and standard deviation
 * - Handles sphere count changes gracefully
 * - Reset on detection loss for quick recovery
 * 
 * Usage:
 *   MovingAverageFilter filter(5);  // 5-frame window
 *   auto smoothed = filter.Update(centroid);
 */

#pragma once

#include <vector>
#include <cmath>
#include <cstdint>

namespace Tracking {

/**
 * Result of group centroid calculation
 */
struct CentroidResult {
    float centroid_x = 0.0f;      // Normalized X position [0.0, 1.0]
    float centroid_y = 0.0f;      // Normalized Y position [0.0, 1.0]
    float std_deviation = 0.0f;   // Standard deviation of sphere positions (pixels)
    int sphere_count = 0;         // Number of spheres in calculation
    int64_t timestamp = 0;        // Timestamp of measurement
    
    CentroidResult() = default;
    CentroidResult(float x, float y, float std, int count, int64_t ts = 0)
        : centroid_x(x), centroid_y(y), std_deviation(std)
        , sphere_count(count), timestamp(ts) {}
    
    bool IsValid() const { return sphere_count > 0; }
};

/**
 * MovingAverageFilter - Smoothing filter for centroid data
 * 
 * Thread safety: NOT thread-safe, use external synchronization
 */
class MovingAverageFilter {
public:
    static constexpr int kDefaultWindowSize = 5;
    static constexpr int kMaxWindowSize = 30;
    
    /**
     * Constructor
     * @param windowSize Number of frames in moving average window
     */
    explicit MovingAverageFilter(int windowSize = kDefaultWindowSize)
        : m_windowSize(std::min(std::max(windowSize, 1), kMaxWindowSize))
        , m_index(0)
        , m_count(0)
        , m_sumX(0.0f)
        , m_sumY(0.0f)
        , m_sumStd(0.0f)
        , m_sumCount(0)
    {
        m_bufferX.resize(m_windowSize, 0.0f);
        m_bufferY.resize(m_windowSize, 0.0f);
        m_bufferStd.resize(m_windowSize, 0.0f);
        m_bufferCount.resize(m_windowSize, 0);
    }
    
    /**
     * Update filter with new centroid measurement
     * @param input New centroid result
     * @return Smoothed centroid result
     */
    CentroidResult Update(const CentroidResult& input) {
        // If no spheres detected, don't add to buffer
        if (input.sphere_count == 0) {
            // Return last valid result or empty
            if (m_count > 0) {
                return GetCurrent();
            }
            return CentroidResult();
        }
        
        // Remove oldest value from sums if buffer is full
        if (m_count >= m_windowSize) {
            m_sumX -= m_bufferX[m_index];
            m_sumY -= m_bufferY[m_index];
            m_sumStd -= m_bufferStd[m_index];
            m_sumCount -= m_bufferCount[m_index];
        }
        
        // Add new values to buffer
        m_bufferX[m_index] = input.centroid_x;
        m_bufferY[m_index] = input.centroid_y;
        m_bufferStd[m_index] = input.std_deviation;
        m_bufferCount[m_index] = input.sphere_count;
        
        // Update sums
        m_sumX += input.centroid_x;
        m_sumY += input.centroid_y;
        m_sumStd += input.std_deviation;
        m_sumCount += input.sphere_count;
        
        // Advance index (circular)
        m_index = (m_index + 1) % m_windowSize;
        
        // Increment count (max = windowSize)
        if (m_count < m_windowSize) {
            m_count++;
        }
        
        return GetCurrent();
    }
    
    /**
     * Get current smoothed result without adding new data
     * @return Current smoothed centroid
     */
    CentroidResult GetCurrent() const {
        if (m_count == 0) {
            return CentroidResult();
        }
        
        CentroidResult result;
        result.centroid_x = m_sumX / static_cast<float>(m_count);
        result.centroid_y = m_sumY / static_cast<float>(m_count);
        result.std_deviation = m_sumStd / static_cast<float>(m_count);
        // Use rounding for integer averaging
        result.sphere_count = (m_sumCount + m_count / 2) / m_count;
        
        return result;
    }
    
    /**
     * Reset filter to initial state
     */
    void Reset() {
        m_index = 0;
        m_count = 0;
        m_sumX = 0.0f;
        m_sumY = 0.0f;
        m_sumStd = 0.0f;
        m_sumCount = 0;
        
        std::fill(m_bufferX.begin(), m_bufferX.end(), 0.0f);
        std::fill(m_bufferY.begin(), m_bufferY.end(), 0.0f);
        std::fill(m_bufferStd.begin(), m_bufferStd.end(), 0.0f);
        std::fill(m_bufferCount.begin(), m_bufferCount.end(), 0);
    }
    
    /**
     * Check if filter has valid data
     */
    bool HasData() const { return m_count > 0; }
    
    /**
     * Get number of samples in buffer
     */
    int GetSampleCount() const { return m_count; }
    
    /**
     * Get window size
     */
    int GetWindowSize() const { return m_windowSize; }

private:
    int m_windowSize;
    int m_index;    // Current position in circular buffer
    int m_count;    // Number of valid samples
    
    // Circular buffers
    std::vector<float> m_bufferX;
    std::vector<float> m_bufferY;
    std::vector<float> m_bufferStd;
    std::vector<int> m_bufferCount;
    
    // Running sums for O(1) average calculation
    float m_sumX;
    float m_sumY;
    float m_sumStd;
    int m_sumCount;
};

} // namespace Tracking
