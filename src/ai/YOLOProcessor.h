/**
 * YOLOProcessor.h
 * 
 * YOLO object detection using TensorRT for GPU acceleration
 * Supports batch processing for multiple camera streams
 * 
 * Optimized for VIB v2.0:
 * - Non-blocking CUDA stream support
 * - Dedicated stream for YOLO (doesn't block NDI)
 * - FP16 precision for RTX 5080 Tensor Cores
 */

#pragma once

#include <cuda_runtime.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <atomic>

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
 */
class YOLOProcessor {
public:
    /**
     * Constructor
     * @param batchSize Maximum batch size for inference (default 4)
     * @param useFP16 Use FP16 precision for Tensor Cores (default true)
     */
    YOLOProcessor(int batchSize = 4, bool useFP16 = true);
    ~YOLOProcessor();
    
    /**
     * Initialize TensorRT engine with YOLO model
     * @param modelPath Path to TensorRT engine file
     * @param useFallback If true, use stub mode when model unavailable
     * @return true if successful
     */
    bool Initialize(const std::string& modelPath, bool useFallback = true);
    
    /**
     * Set confidence threshold for detections
     */
    void SetConfidenceThreshold(float threshold) { m_confidenceThreshold = threshold; }
    
    /**
     * Set NMS (Non-Maximum Suppression) threshold
     */
    void SetNMSThreshold(float threshold) { m_nmsThreshold = threshold; }
    
    /**
     * Check if running in stub mode (no real inference)
     */
    bool IsStubMode() const { return m_stubMode; }
    
    /**
     * Process single frame from CUDA memory (non-blocking)
     * @param cudaBGRABuffer CUDA device pointer to BGRA frame
     * @param cameraID Camera identifier
     * @param width Frame width
     * @param height Frame height
     * @param stream CUDA stream for async execution
     * @return Detections (may be empty if still processing)
     */
    std::vector<Detection> ProcessFrame(void* cudaBGRABuffer, int cameraID,
                                        unsigned int width, unsigned int height,
                                        cudaStream_t stream);
    
    /**
     * Process single D3D11 texture (legacy interface)
     */
    std::vector<Detection> ProcessFrame(ID3D11Texture2D* texture, int cameraID);
    
    /**
     * Process batch of textures (more efficient for multiple cameras)
     */
    std::vector<Detection> ProcessBatch(const std::vector<ID3D11Texture2D*>& textures,
                                        const std::vector<int>& cameraIDs);
    
    /**
     * Get detection results array (for Redis worker)
     */
    const std::vector<Detection>& GetDetections() const { return m_detections; }
    
private:
    // Configuration
    int m_batchSize;
    bool m_useFP16;
    std::atomic<bool> m_stubMode;
    
    // TensorRT resources
    // TODO: Add TensorRT engine, context, buffers
    
    // Detection results
    std::vector<Detection> m_detections;
    
    // Model configuration
    int m_inputWidth;
    int m_inputHeight;
    int m_numClasses;
    float m_confidenceThreshold;
    float m_nmsThreshold;
};
