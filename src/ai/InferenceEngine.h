/**
 * InferenceEngine.h
 * 
 * TensorRT 10 inference engine for YOLO ball detection
 * Optimized for batch processing of 12 camera streams
 * 
 * Architecture:
 * - TensorRT engine with FP16 precision
 * - Batch size 12 for simultaneous multi-camera inference
 * - CUDA stream-based async execution
 * - Integrated preprocessing (resize + normalize)
 * 
 * Performance target: <15ms total for batch of 12 frames
 */

#pragma once

#include <cuda_runtime.h>
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
    int ballID;             // Ball identifier (1-10, or 0 if unknown)
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
    int batchSize = 12;
    int inputWidth = 640;
    int inputHeight = 640;
    float confidenceThreshold = 0.6f;
    float nmsThreshold = 0.4f;
    bool useFP16 = true;
    int numClasses = 10;    // 10 ball types
};

/**
 * InferenceEngine - TensorRT-based YOLO inference
 * 
 * Thread safety: ProcessBatch is thread-safe, Initialize must be called once
 */
class InferenceEngine {
public:
    static constexpr int kMaxBatchSize = 12;
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

private:
    // Configuration
    InferenceEngineConfig m_config;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_stubMode;
    mutable std::mutex m_mutex;
    
    // TensorRT resources (opaque pointers to avoid header dependency)
    // In real implementation, these would be:
    // nvinfer1::IRuntime* m_runtime;
    // nvinfer1::ICudaEngine* m_engine;
    // nvinfer1::IExecutionContext* m_context;
    void* m_runtime;
    void* m_engine;
    void* m_context;
    
    // GPU buffers
    void* m_inputBuffer;        // Preprocessed input [B, C, H, W]
    void* m_outputBuffer;       // Raw output from network
    size_t m_inputSize;         // Size in bytes
    size_t m_outputSize;        // Size in bytes
    
    // Pinned host memory for results
    float* m_hostOutput;
    
    // Telemetry
    InferenceTelemetry m_lastTelemetry;
    
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
