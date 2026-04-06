/**
 * ActiveCameraSelector.h
 * 
 * GPU-accelerated motion detection and Top-K camera selection
 * Reduces YOLO processing from 12 cameras to 4 most active cameras
 * 
 * Architecture:
 * - Level 1: CUDA compute shader calculates inter-frame motion (<0.5ms)
 * - Level 2: Top-K selection identifies 4 cameras with highest activity
 * - Level 3: Edge handover pre-activates neighbor cameras when objects near border
 * 
 * Performance target: <1ms total selection time for 12 cameras
 */

#pragma once

#include <cuda_runtime.h>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>

/**
 * Motion metrics for a single camera
 */
struct CameraMotionMetrics {
    int cameraID;
    float motionScore;          // [0.0, 1.0] normalized motion intensity
    float edgeActivity;         // [0.0, 1.0] activity near frame edges
    bool isActive;              // Selected for YOLO processing
    unsigned long long frameCount;
};

/**
 * Active camera selection result
 */
struct CameraSelectionResult {
    std::vector<int> selectedCameraIDs;  // Top-K camera IDs for YOLO
    std::vector<int> preActivatedIDs;    // Neighbor cameras for next cycle
    float averageMotionScore;
    unsigned long long timestamp;
};

/**
 * ActiveCameraSelector - GPU-accelerated camera selection
 * 
 * Thread safety: All methods are thread-safe
 */
class ActiveCameraSelector {
public:
    /**
     * Constructor
     * @param numCameras Total number of cameras (default 12)
     * @param topK Number of cameras to select (default 4)
     * @param motionThreshold Minimum motion score to consider active (default 0.05)
     * @param edgeMargin Percentage of frame considered "edge" (default 0.1 = 10%)
     */
    ActiveCameraSelector(int numCameras = 12, int topK = 4, 
                         float motionThreshold = 0.05f, float edgeMargin = 0.1f);
    ~ActiveCameraSelector();

    /**
     * Initialize CUDA resources
     * @return true if successful
     */
    bool Initialize();

    /**
     * Process a frame from a camera to update motion metrics
     * @param cameraID Camera identifier (0-11)
     * @param cudaYUVBuffer CUDA device pointer to current frame (UYVY format)
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream for async execution
     * @return true if successful
     */
    bool ProcessFrame(int cameraID, void* cudaYUVBuffer, 
                      unsigned int width, unsigned int height,
                      cudaStream_t stream);

    /**
     * Get current Top-K active cameras
     * @return Selection result with camera IDs ranked by activity
     */
    CameraSelectionResult GetActiveSelection();

    /**
     * Get motion metrics for a specific camera
     * @param cameraID Camera identifier
     * @return Motion metrics (score = 0 if invalid camera)
     */
    CameraMotionMetrics GetCameraMetrics(int cameraID) const;

    /**
     * Get all camera metrics
     * @return Vector of metrics for all cameras
     */
    std::vector<CameraMotionMetrics> GetAllMetrics() const;

    /**
     * Check if selector is initialized
     */
    bool IsInitialized() const { return m_initialized; }

    /**
     * Reset all motion tracking (e.g., after scene change)
     */
    void Reset();

private:
    // Configuration
    int m_numCameras;
    int m_topK;
    float m_motionThreshold;
    float m_edgeMargin;
    
    // State
    std::atomic<bool> m_initialized;
    
    // Per-camera tracking
    struct CameraState {
        void* previousFrame;        // CUDA device memory for previous frame
        void* currentFrame;         // CUDA device memory for current frame
        void* motionBuffer;         // CUDA device memory for motion map
        float* hostMotionScore;     // Pinned host memory for motion score
        cudaEvent_t processingComplete;
        CameraMotionMetrics metrics;
        std::mutex frameMutex;
        unsigned int width;
        unsigned int height;
        size_t frameSize;
        bool hasHistory;
    };
    
    std::vector<CameraState> m_cameraStates;
    
    // Thread safety for selection
    mutable std::mutex m_selectionMutex;
    CameraSelectionResult m_lastSelection;
    
    // Helper methods
    bool AllocateCameraResources(int cameraID, unsigned int width, unsigned int height);
    void FreeCameraResources(int cameraID);
    float CalculateMotionScore(int cameraID, cudaStream_t stream);
    float CalculateEdgeActivity(int cameraID, cudaStream_t stream);
    std::vector<int> SelectTopK();
    std::vector<int> DetermineNeighborHandover(const std::vector<int>& selected);
};
