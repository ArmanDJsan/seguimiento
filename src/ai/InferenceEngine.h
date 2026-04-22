/**
 * InferenceEngine.h
 * 
 * TensorRT 10 inference engine for YOLO ball detection
 * Optimized for batch processing of 4 PTZ camera streams at 1080p
 * 
 * Architecture:
 * - TensorRT engine with FP16 precision
 * - Batch size 4 for simultaneous multi-camera inference
 * - CUDA stream-based async execution
 * - Integrated preprocessing (optional resize + normalize)
 * - Supports 1080p native input for better small object detection
 * 
 * Performance target: <15ms total for batch of 4 frames @1080p
 */

#pragma once

#include <cuda_runtime.h>
#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <array>

/**
 * Ball detection result from inference
 */
struct BallDetection {
    int ballID;             // Ball identifier (0-9, matching YOLO class ID)
    int cameraID;           // Camera that detected this ball
    float x, y;             // Normalized pixel coordinates [0.0, 1.0]
    float width, height;    // Normalized bounding box dimensions
    float confidence;       // Detection confidence [0.0, 1.0]
    int64_t timestamp;      // Detection timestamp
    
    BallDetection()
        : ballID(0), cameraID(-1)
        , x(0), y(0), width(0), height(0)
        , confidence(0), timestamp(0) {}
};

/**
 * Result of group centroid calculation for PTZ tracking
 * Represents the center of mass of detected spheres
 */
struct CentroidResult {
    float centroid_x = 0.0f;      // Normalized X position [0.0, 1.0]
    float centroid_y = 0.0f;      // Normalized Y position [0.0, 1.0]
    float std_deviation = 0.0f;   // Standard deviation of sphere positions (normalized)
    int sphere_count = 0;         // Number of spheres in calculation
    int64_t timestamp = 0;        // Timestamp of measurement
    
    CentroidResult() = default;
    CentroidResult(float x, float y, float std, int count, int64_t ts = 0)
        : centroid_x(x), centroid_y(y), std_deviation(std)
        , sphere_count(count), timestamp(ts) {}
    
    bool IsValid() const { return sphere_count > 0; }
};

/**
 * Inference telemetry for performance monitoring
 */
struct InferenceTelemetry {
    float preprocess_ms;    // Preprocessing time
    float inference_ms;     // TensorRT inference time
    float postprocess_ms;   // Post-processing (NMS) time
    float total_ms;         // Total time
    int detectionsCount;    // Number of detections
    int batchSize;          // Actual batch size processed
};

/**
 * Inference engine configuration
 */
struct InferenceEngineConfig {
    std::string modelPath = "models/yolo26l_fp16_batch12.engine";
    int batchSize = 4;
    int inputWidth = 1920;   // Full HD for better small object detection
    int inputHeight = 1080;  // 1080p native - no resize needed
    float confidenceThreshold = 0.6f;
    float nmsThreshold = 0.4f;
    bool useFP16 = true;
    int numClasses = 10;    // 10 ball types
    bool skipResize = true; // When true, input is directly at native resolution
    int maxDetections = 300;  // Maximum detections to process (reduced from 8400 for memory optimization)
    bool debugYoloEnabled = false;  // Enable debug logging of YOLO raw output
    int debugYoloInterval = 300;    // Print debug info every N frames (when enabled)
};

/**
 * InferenceEngine - TensorRT-based YOLO inference
 * 
 * Thread safety:
 * - ProcessBatch/ProcessFrame are thread-safe via mutex serialization
 * - Initialize must be called once before any inference calls
 * - Concurrent calls from multiple threads are serialized (one at a time)
 * - For higher throughput, consider using multiple InferenceEngine instances
 */
class InferenceEngine {
public:
    static constexpr int kMaxBatchSize = 4;    // 4 PTZ cameras for radar detection
    static constexpr int kInputChannels = 3;  // RGB
    
    /**
     * Constructor
     */
    InferenceEngine();
    ~InferenceEngine();
    
    // Non-copyable
    InferenceEngine(const InferenceEngine&) = delete;
    InferenceEngine& operator=(const InferenceEngine&) = delete;
    
    /**
     * Initialize TensorRT engine
     * @param config Engine configuration
     * @return true if successful (or stub mode enabled)
     */
    bool Initialize(const InferenceEngineConfig& config);
    
    /**
     * Initialize with default config (for backward compatibility)
     * @param modelPath Path to TensorRT engine file
     * @param useFallback If true, use stub mode when model unavailable
     */
    bool Initialize(const std::string& modelPath, bool useFallback = true);
    
    /**
     * Process single frame from CUDA memory
     * @param cudaBGRABuffer CUDA device pointer to BGRA frame
     * @param cameraID Camera identifier
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream for async execution
     * @return Detections for this frame
     */
    std::vector<BallDetection> ProcessFrame(
        void* cudaBGRABuffer, int cameraID,
        unsigned int width, unsigned int height,
        cudaStream_t stream);
    
    /**
     * Process single UYVY frame with fused kernel (OPTIMIZED PATH)
     * Bypasses BGRA intermediate buffer for lower latency
     * @param cudaUYVYBuffer CUDA device pointer to UYVY frame
     * @param cameraID Camera identifier
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream for async execution
     * @param preprocessEvent Optional event to record after preprocessing
     * @return Detections for this frame
     */
    std::vector<BallDetection> ProcessFrameUYVY(
        void* cudaUYVYBuffer, int cameraID,
        unsigned int width, unsigned int height,
        cudaStream_t stream,
        cudaEvent_t preprocessEvent = nullptr);
    
    /**
     * Process batch of frames from CUDA memory
     * @param cudaBuffers Array of CUDA device pointers to BGRA frames
     * @param cameraIDs Camera identifiers for each frame
     * @param widths Frame widths
     * @param heights Frame heights
     * @param numFrames Number of frames in batch
     * @param stream CUDA stream for async execution
     * @return Detections for all frames
     */
    std::vector<BallDetection> ProcessBatch(
        void** cudaBuffers,
        const int* cameraIDs,
        const unsigned int* widths,
        const unsigned int* heights,
        int numFrames,
        cudaStream_t stream);
    
    /**
     * Set confidence threshold for detections
     */
    void SetConfidenceThreshold(float threshold);
    
    /**
     * Set NMS (Non-Maximum Suppression) threshold
     */
    void SetNMSThreshold(float threshold);
    
    /**
     * Check if running in stub mode (no real inference)
     */
    bool IsStubMode() const { return m_stubMode; }
    
    /**
     * Check if engine is initialized
     */
    bool IsInitialized() const { return m_initialized; }
    
    /**
     * Get last inference telemetry
     */
    InferenceTelemetry GetLastTelemetry() const;
    
    /**
     * Get configuration
     */
    const InferenceEngineConfig& GetConfig() const { return m_config; }
    
    /**
     * Calculate group centroid from detections for PTZ tracking
     * @param detections Vector of ball detections
     * @param confidenceThreshold Minimum confidence for inclusion (default 0.5)
     * @return CentroidResult with centroid position and statistics
     */
    static CentroidResult CalculateGroupCentroid(
        const std::vector<BallDetection>& detections,
        float confidenceThreshold = 0.5f);
    
    /**
     * Get last calculated centroid (from most recent ProcessBatch/ProcessFrame)
     * @return Last centroid result
     */
    CentroidResult GetLastCentroid() const;

private:
    /**
     * TensorRT Logger implementation
     * Routes TensorRT log messages through our Logger system
     */
    class TRTLogger : public nvinfer1::ILogger {
    public:
        void log(Severity severity, const char* msg) noexcept override;
    };
    
    // Configuration
    InferenceEngineConfig m_config;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_stubMode;
    mutable std::mutex m_mutex;
    
    // TensorRT logger instance (must outlive runtime)
    TRTLogger m_trtLogger;
    
    // TensorRT resources
    nvinfer1::IRuntime* m_runtime;
    nvinfer1::ICudaEngine* m_engine;
    nvinfer1::IExecutionContext* m_context;
    
    // Input/output tensor names (discovered from engine)
    std::string m_inputTensorName;
    std::string m_outputTensorName;
    
    // Dynamic shape support
    bool m_hasDynamicShapes;    // True if engine has dynamic batch dimension
    
    // GPU buffers
    void* m_inputBuffer;        // Preprocessed input [B, C, H, W]
    void* m_outputBuffer;       // Raw output from network
    size_t m_inputSize;         // Size in bytes
    size_t m_outputSize;        // Size in bytes
    int m_maxDetections;        // Max detections per frame (from engine output shape)
    int m_outputStride;         // Output stride per detection
    
    // Pinned host memory for results
    float* m_hostOutput;
    
    // Telemetry
    InferenceTelemetry m_lastTelemetry;
    
    // Last centroid for PTZ tracking
    CentroidResult m_lastCentroid;
    
    // Helper methods
    bool LoadEngine(const std::string& enginePath);
    bool AllocateBuffers();
    void FreeBuffers();
    void PreprocessBatch(void** cudaBuffers, const unsigned int* widths, 
                         const unsigned int* heights, int numFrames, cudaStream_t stream);
    std::vector<BallDetection> PostProcess(const float* rawOutput, 
                                            const int* cameraIDs, int numFrames);
    void ApplyNMS(std::vector<BallDetection>& detections);
    int64_t GetCurrentTimeMs() const;
};
