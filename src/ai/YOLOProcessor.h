/**
 * YOLOProcessor.h
 * 
 * YOLO object detection using TensorRT 10.x for GPU acceleration
 * Supports FP16 precision and batch processing for multiple cameras
 * 
 * Key features:
 * - TensorRT 10.x engine loading
 * - FP16 inference for RTX 5080 Tensor Cores
 * - Batch size 4 for optimal throughput
 * - Fallback stub mode for resilience
 */

#pragma once

#include <cuda_runtime.h>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <mutex>

// Forward declarations for TensorRT (avoid including heavy headers)
class IExecutionContext;
class ICudaEngine;
class IRuntime;
namespace nvinfer1 {
    class IExecutionContext;
    class ICudaEngine;
    class IRuntime;
    class ILogger;
}

/**
 * Detection result structure
 */
struct Detection {
    int cameraID;
    int objectID;
    float x, y;          // Normalized coordinates [0.0, 1.0]
    float width, height; // Normalized dimensions
    std::string label;
    float confidence;
    long long timestamp; // Milliseconds since epoch
};

/**
 * YOLO processor using TensorRT for real-time object detection
 * Thread-safe for concurrent inference requests
 */
class YOLOProcessor {
public:
    /**
     * Constructor
     * @param batchSize Maximum batch size (default 4)
     * @param useFP16 Use FP16 precision (default true for RTX 5080)
     */
    YOLOProcessor(int batchSize = 4, bool useFP16 = true);
    ~YOLOProcessor();
    
    /**
     * Initialize TensorRT engine with YOLO model
     * @param enginePath Path to .engine file (serialized TensorRT engine)
     * @param fallbackMode If true, use stub mode when engine load fails
     * @return true if successful (or fallback enabled)
     */
    bool Initialize(const std::string& enginePath, bool fallbackMode = false);
    
    /**
     * Check if running in stub mode (no real inference)
     * @return true if in fallback/stub mode
     */
    bool IsStubMode() const { return m_stubMode; }
    
    /**
     * Check if initialized
     */
    bool IsInitialized() const { return m_initialized; }
    
    /**
     * Process batch of frames from CUDA device memory (BGRA format)
     * @param cudaBGRABuffers Array of CUDA device pointers to BGRA frames
     * @param cameraIDs Camera identifiers for each frame
     * @param numFrames Number of frames (must be <= batch size)
     * @param width Frame width
     * @param height Frame height
     * @param streams CUDA streams for async execution (optional)
     * @return Vector of all detections across all frames
     */
    std::vector<Detection> ProcessBatch(
        void** cudaBGRABuffers,
        const int* cameraIDs,
        int numFrames,
        unsigned int width,
        unsigned int height,
        cudaStream_t* streams = nullptr);
    
    /**
     * Process single frame (convenience wrapper for ProcessBatch)
     * @param cudaBGRABuffer CUDA device pointer to BGRA frame
     * @param cameraID Camera identifier
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream (optional)
     */
    std::vector<Detection> ProcessFrame(
        void* cudaBGRABuffer,
        int cameraID,
        unsigned int width,
        unsigned int height,
        cudaStream_t stream = nullptr);
    
    /**
     * Get detection results from last inference
     */
    const std::vector<Detection>& GetDetections() const { return m_detections; }
    
    /**
     * Set detection parameters
     */
    void SetConfidenceThreshold(float threshold) { m_confidenceThreshold = threshold; }
    void SetNMSThreshold(float threshold) { m_nmsThreshold = threshold; }
    
    /**
     * Get configuration
     */
    int GetBatchSize() const { return m_batchSize; }
    int GetInputWidth() const { return m_inputWidth; }
    int GetInputHeight() const { return m_inputHeight; }
    
private:
    // TensorRT resources (opaque pointers to avoid header dependencies)
    nvinfer1::IRuntime* m_runtime;
    nvinfer1::ICudaEngine* m_engine;
    nvinfer1::IExecutionContext* m_context;
    nvinfer1::ILogger* m_logger;
    
    // CUDA buffers for inference
    void* m_inputBuffer;        // Device memory for batch input
    void* m_outputBuffer;       // Device memory for batch output
    cudaStream_t m_inferenceStream;
    
    // Detection results
    std::vector<Detection> m_detections;
    mutable std::mutex m_detectionsMutex;
    
    // Model configuration
    int m_batchSize;
    int m_inputWidth;
    int m_inputHeight;
    int m_numClasses;
    float m_confidenceThreshold;
    float m_nmsThreshold;
    bool m_useFP16;
    
    // State
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_stubMode;
    
    // Helper methods
    bool LoadEngine(const std::string& enginePath);
    bool AllocateBuffers();
    void FreeBuffers();
    std::vector<Detection> RunInference(void** inputs, const int* cameraIDs, 
                                        int numFrames, cudaStream_t* streams);
    std::vector<Detection> GenerateStubDetections(const int* cameraIDs, int numFrames);
    void PreprocessBatch(void** cudaBGRABuffers, int numFrames, 
                        unsigned int width, unsigned int height, cudaStream_t* streams);
    std::vector<Detection> PostProcess(const int* cameraIDs, int numFrames);
    std::vector<Detection> ApplyNMS(const std::vector<Detection>& detections);
};

