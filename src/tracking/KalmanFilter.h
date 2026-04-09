/**
 * KalmanFilter.h
 * 
 * Simple 2D Kalman Filter for ball tracking
 * State: [x, y, vx, vy] - position and velocity
 * Measurement: [x, y] - position only
 * 
 * Optimized for:
 * - Low latency (<0.1ms per update)
 * - Smooth trajectory prediction during occlusions
 * - Racing ball dynamics (constant velocity model)
 */

#pragma once

#include <array>
#include <cmath>

/**
 * 2D Kalman Filter with position-velocity state
 */
class KalmanFilter2D {
public:
    /**
     * Constructor with default racing-optimized parameters
     */
    KalmanFilter2D()
        : m_initialized(false)
        , m_processNoisePos(0.01f)
        , m_processNoiseVel(0.1f)
        , m_measurementNoise(0.5f)
        , m_dt(1.0f / 30.0f)  // 30 FPS default
    {
        Reset();
    }
    
    /**
     * Configure filter parameters
     * @param processNoisePos Position process noise variance
     * @param processNoiseVel Velocity process noise variance
     * @param measurementNoise Measurement noise variance
     * @param dt Time step (seconds)
     */
    void Configure(float processNoisePos, float processNoiseVel, 
                   float measurementNoise, float dt = 1.0f / 30.0f) {
        m_processNoisePos = processNoisePos;
        m_processNoiseVel = processNoiseVel;
        m_measurementNoise = measurementNoise;
        m_dt = dt;
    }
    
    /**
     * Reset filter state
     */
    void Reset() {
        m_initialized = false;
        // State: [x, y, vx, vy]
        m_state = {0.0f, 0.0f, 0.0f, 0.0f};
        // Covariance matrix (4x4, stored as 16 elements row-major)
        // Initialize with high uncertainty
        m_P = {100.0f, 0.0f, 0.0f, 0.0f,
               0.0f, 100.0f, 0.0f, 0.0f,
               0.0f, 0.0f, 100.0f, 0.0f,
               0.0f, 0.0f, 0.0f, 100.0f};
    }
    
    /**
     * Initialize filter with first measurement
     * @param x Initial X position
     * @param y Initial Y position
     */
    void Initialize(float x, float y) {
        m_state[0] = x;
        m_state[1] = y;
        m_state[2] = 0.0f;  // Initial velocity unknown
        m_state[3] = 0.0f;
        
        // Reset covariance with low position uncertainty, high velocity uncertainty
        m_P = {1.0f, 0.0f, 0.0f, 0.0f,
               0.0f, 1.0f, 0.0f, 0.0f,
               0.0f, 0.0f, 10.0f, 0.0f,
               0.0f, 0.0f, 0.0f, 10.0f};
        
        m_initialized = true;
    }
    
    /**
     * Predict next state (call when no measurement available)
     */
    void Predict() {
        if (!m_initialized) return;
        
        // State transition: constant velocity model
        // x' = x + vx * dt
        // y' = y + vy * dt
        // vx' = vx
        // vy' = vy
        m_state[0] += m_state[2] * m_dt;
        m_state[1] += m_state[3] * m_dt;
        
        // Update covariance: P' = F * P * F^T + Q
        // F = [1  0  dt  0 ]
        //     [0  1  0   dt]
        //     [0  0  1   0 ]
        //     [0  0  0   1 ]
        
        // Simplified covariance update (assuming diagonal dominance)
        float dt2 = m_dt * m_dt;
        m_P[0] += 2.0f * m_dt * m_P[2] + dt2 * m_P[10] + m_processNoisePos;
        m_P[5] += 2.0f * m_dt * m_P[7] + dt2 * m_P[15] + m_processNoisePos;
        m_P[10] += m_processNoiseVel;
        m_P[15] += m_processNoiseVel;
        
        // Cross terms
        m_P[2] += m_dt * m_P[10];
        m_P[7] += m_dt * m_P[15];
        m_P[8] = m_P[2];  // Symmetric
        m_P[13] = m_P[7];
    }
    
    /**
     * Update state with new measurement
     * @param measX Measured X position
     * @param measY Measured Y position
     */
    void Update(float measX, float measY) {
        if (!m_initialized) {
            Initialize(measX, measY);
            return;
        }
        
        // Predict step first
        Predict();
        
        // Innovation (measurement residual)
        float innovX = measX - m_state[0];
        float innovY = measY - m_state[1];
        
        // Innovation covariance: S = H * P * H^T + R
        // H = [1 0 0 0; 0 1 0 0] (we only measure position)
        float S00 = m_P[0] + m_measurementNoise;
        float S11 = m_P[5] + m_measurementNoise;
        
        // Kalman gain: K = P * H^T * S^-1
        // Since S is diagonal (we assume independent x,y measurements):
        float K00 = m_P[0] / S00;   // K for x position
        float K10 = m_P[5] / S11;   // K for y position
        float K20 = m_P[2] / S00;   // K for vx from x measurement
        float K31 = m_P[7] / S11;   // K for vy from y measurement
        
        // Update state: x = x + K * innovation
        m_state[0] += K00 * innovX;
        m_state[1] += K10 * innovY;
        m_state[2] += K20 * innovX;
        m_state[3] += K31 * innovY;
        
        // Update covariance: P = (I - K*H) * P
        m_P[0] *= (1.0f - K00);
        m_P[2] *= (1.0f - K00);
        m_P[5] *= (1.0f - K10);
        m_P[7] *= (1.0f - K10);
        m_P[10] -= K20 * m_P[2];
        m_P[15] -= K31 * m_P[7];
    }
    
    /**
     * Get current estimated position
     * @param outX Output X position
     * @param outY Output Y position
     */
    void GetPosition(float& outX, float& outY) const {
        outX = m_state[0];
        outY = m_state[1];
    }
    
    /**
     * Get current estimated velocity
     * @param outVx Output X velocity
     * @param outVy Output Y velocity
     */
    void GetVelocity(float& outVx, float& outVy) const {
        outVx = m_state[2];
        outVy = m_state[3];
    }
    
    /**
     * Get predicted position at time t in the future
     * @param deltaT Time offset (seconds)
     * @param outX Output predicted X position
     * @param outY Output predicted Y position
     */
    void PredictPosition(float deltaT, float& outX, float& outY) const {
        outX = m_state[0] + m_state[2] * deltaT;
        outY = m_state[1] + m_state[3] * deltaT;
    }
    
    /**
     * Get position uncertainty (standard deviation)
     */
    float GetPositionUncertainty() const {
        return std::sqrt(m_P[0] + m_P[5]);  // sqrt(var_x + var_y)
    }
    
    /**
     * Check if filter is initialized
     */
    bool IsInitialized() const { return m_initialized; }
    
    /**
     * Get current state vector [x, y, vx, vy]
     */
    const std::array<float, 4>& GetState() const { return m_state; }

private:
    bool m_initialized;
    float m_processNoisePos;
    float m_processNoiseVel;
    float m_measurementNoise;
    float m_dt;
    
    std::array<float, 4> m_state;   // [x, y, vx, vy]
    std::array<float, 16> m_P;      // 4x4 covariance matrix
};
