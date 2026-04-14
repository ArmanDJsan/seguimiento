/**
 * InferenceEngine.cpp
 * 
 * Implementation of TensorRT-based inference engine for YOLO ball detection
 * 
 * TensorRT integration for high-performance batch inference on RTX GPUs.
 * Optimized for processing 12 camera streams simultaneously.
 */

#include "InferenceEngine.h"
#include "../utils/Logger.h"
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cstring>

// External preprocessing kernel functions from PreprocessKernel.cu
extern "C" {
    cudaError_t LaunchPreprocessBatch(
        void** srcBuffers,
        float* dstRGB,
        const int* srcWidths,
        const int* srcHeights,
        int dstWidth, int dstHeight,
        int numFrames,
        bool isUYVY,
        cudaStream_t stream);
    
    cudaError_t LaunchPreprocessBGRA(
        const void* srcBGRA,
        float* dstRGB,
        int srcWidth, int srcHeight,
        int dstWidth, int dstHeight,
        int batchIdx,
        cudaStream_t stream);
}

// TensorRT Logger implementation
void InferenceEngine::TRTLogger::log(Severity severity, const char* msg) noexcept {
    // Filter out verbose info/warning messages in production
    switch (severity) {
        case Severity::kINTERNAL_ERROR:
        case Severity::kERROR:
            Logger::Error(std::string("TensorRT: ") + msg);
            break;
        case Severity::kWARNING:
            Logger::Warning(std::string("TensorRT: ") + msg);
            break;
        case Severity::kINFO:
            Logger::Info(std::string("TensorRT: ") + msg);
            break;
        case Severity::kVERBOSE:
            // Suppress verbose messages in production
            break;
    }
}

InferenceEngine::InferenceEngine()
    : m_initialized(false)
    , m_stubMode(true)
    , m_runtime(nullptr)
    , m_engine(nullptr)
    , m_context(nullptr)
    , m_inputBuffer(nullptr)
    , m_outputBuffer(nullptr)
    , m_inputSize(0)
    , m_outputSize(0)
    , m_maxDetections(8400)
    , m_outputStride(0)
    , m_hostOutput(nullptr)
{
    std::memset(&m_lastTelemetry, 0, sizeof(m_lastTelemetry));
}

InferenceEngine::~InferenceEngine() {
    FreeBuffers();
    
    // Free TensorRT resources in reverse order of creation
    if (m_context) {
        delete m_context;
        m_context = nullptr;
    }
    // Note: Engine and Runtime use reference counting in TensorRT 10
    // delete is not called directly; they are destroyed when their refcount reaches 0
    m_engine = nullptr;
    m_runtime = nullptr;
}

bool InferenceEngine::Initialize(const InferenceEngineConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        Logger::Warning("InferenceEngine: Already initialized");
        return true;
    }
    
    m_config = config;
    
    Logger::Info("InferenceEngine: Initializing with model: " + config.modelPath);
    Logger::Info("InferenceEngine: Batch size=" + std::to_string(config.batchSize) + 
                 ", Input=" + std::to_string(config.inputWidth) + "x" + std::to_string(config.inputHeight) +
                 ", FP16=" + std::string(config.useFP16 ? "enabled" : "disabled"));
    
    // Try to load the TensorRT engine
    if (!LoadEngine(config.modelPath)) {
        Logger::Warning("InferenceEngine: Could not load engine, using stub mode");
        m_stubMode = true;
    } else {
        m_stubMode = false;
        
        // Allocate buffers for inference
        if (!AllocateBuffers()) {
            Logger::Error("InferenceEngine: Failed to allocate GPU buffers");
            m_stubMode = true;
        }
    }
    
    m_initialized = true;
    
    if (m_stubMode) {
        Logger::Warning("InferenceEngine: Running in STUB mode - no real inference");
    } else {
        Logger::Info("InferenceEngine: TensorRT engine loaded successfully");
    }
    
    return true;
}

bool InferenceEngine::Initialize(const std::string& modelPath, bool useFallback) {
    InferenceEngineConfig config;
    config.modelPath = modelPath;
    
    bool result = Initialize(config);
    
    if (!result && !useFallback) {
        return false;
    }
    
    return true;
}

std::vector<BallDetection> InferenceEngine::ProcessFrame(
    void* cudaBGRABuffer, int cameraID,
    unsigned int width, unsigned int height,
    cudaStream_t stream) {
    
    if (!m_initialized) {
        return {};
    }
    
    // Process as single-frame batch
    void* buffers[1] = { cudaBGRABuffer };
    int cameras[1] = { cameraID };
    unsigned int widths[1] = { width };
    unsigned int heights[1] = { height };
    
    return ProcessBatch(buffers, cameras, widths, heights, 1, stream);
}

std::vector<BallDetection> InferenceEngine::ProcessBatch(
    void** cudaBuffers,
    const int* cameraIDs,
    const unsigned int* widths,
    const unsigned int* heights,
    int numFrames,
    cudaStream_t stream) {
    
    if (!m_initialized || numFrames <= 0) {
        return {};
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    std::vector<BallDetection> detections;
    
    if (m_stubMode) {
        // Stub mode: return simulated detections for testing
        auto preprocessEnd = std::chrono::high_resolution_clock::now();
        
        // Simulate some processing time
        int64_t now = GetCurrentTimeMs();
        
        // Generate fake detections (one ball per camera, random positions)
        for (int i = 0; i < numFrames; ++i) {
            // Simulate detecting a ball in each frame
            BallDetection det;
            det.ballID = (i % 10) + 1;  // Ball IDs 1-10
            det.cameraID = cameraIDs[i];
            det.x = 0.3f + 0.1f * (i % 3);  // Spread across frame
            det.y = 0.4f + 0.1f * (i % 2);
            det.width = 0.05f;
            det.height = 0.05f;
            det.confidence = 0.85f;
            det.timestamp = now;
            detections.push_back(det);
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        
        m_lastTelemetry.preprocess_ms = 0.5f;
        m_lastTelemetry.inference_ms = 1.0f;
        m_lastTelemetry.postprocess_ms = 0.3f;
        m_lastTelemetry.total_ms = std::chrono::duration<float, std::milli>(endTime - startTime).count();
        m_lastTelemetry.detectionsCount = static_cast<int>(detections.size());
        m_lastTelemetry.batchSize = numFrames;
        
        return detections;
    }
    
    // Real TensorRT inference:
    // 1. Preprocess frames
    auto preprocessStart = std::chrono::high_resolution_clock::now();
    PreprocessBatch(cudaBuffers, widths, heights, numFrames, stream);
    cudaStreamSynchronize(stream);  // Ensure preprocessing is complete
    auto preprocessEnd = std::chrono::high_resolution_clock::now();
    
    // 2. Run TensorRT inference
    auto inferenceStart = std::chrono::high_resolution_clock::now();
    
    // Set up I/O tensor addresses for TensorRT 10.x API
    if (!m_context->setTensorAddress(m_inputTensorName.c_str(), m_inputBuffer)) {
        Logger::Error("InferenceEngine: Failed to set input tensor address");
        return {};
    }
    if (!m_context->setTensorAddress(m_outputTensorName.c_str(), m_outputBuffer)) {
        Logger::Error("InferenceEngine: Failed to set output tensor address");
        return {};
    }
    
    // Execute inference asynchronously on the stream
    if (!m_context->enqueueV3(stream)) {
        Logger::Error("InferenceEngine: TensorRT inference execution failed");
        return {};
    }
    
    auto inferenceEnd = std::chrono::high_resolution_clock::now();
    
    // 3. Copy results to host asynchronously
    cudaMemcpyAsync(m_hostOutput, m_outputBuffer, m_outputSize, cudaMemcpyDeviceToHost, stream);
    cudaStreamSynchronize(stream);  // Wait for copy to complete
    
    // 4. Post-process
    auto postprocessStart = std::chrono::high_resolution_clock::now();
    detections = PostProcess(m_hostOutput, cameraIDs, numFrames);
    ApplyNMS(detections);
    auto postprocessEnd = std::chrono::high_resolution_clock::now();
    
    // Update telemetry
    m_lastTelemetry.preprocess_ms = std::chrono::duration<float, std::milli>(preprocessEnd - preprocessStart).count();
    m_lastTelemetry.inference_ms = std::chrono::duration<float, std::milli>(inferenceEnd - inferenceStart).count();
    m_lastTelemetry.postprocess_ms = std::chrono::duration<float, std::milli>(postprocessEnd - postprocessStart).count();
    m_lastTelemetry.total_ms = std::chrono::duration<float, std::milli>(postprocessEnd - startTime).count();
    m_lastTelemetry.detectionsCount = static_cast<int>(detections.size());
    m_lastTelemetry.batchSize = numFrames;
    
    return detections;
}

void InferenceEngine::SetConfidenceThreshold(float threshold) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.confidenceThreshold = threshold;
}

void InferenceEngine::SetNMSThreshold(float threshold) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.nmsThreshold = threshold;
}

InferenceTelemetry InferenceEngine::GetLastTelemetry() const {
    return m_lastTelemetry;
}

bool InferenceEngine::LoadEngine(const std::string& enginePath) {
    // Check if file exists
    std::ifstream file(enginePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        Logger::Warning("InferenceEngine: Engine file not found: " + enginePath);
        return false;
    }
    
    size_t fileSize = static_cast<size_t>(file.tellg());
    file.seekg(0, std::ios::beg);
    
    if (fileSize == 0) {
        Logger::Warning("InferenceEngine: Engine file is empty");
        return false;
    }
    
    Logger::Info("InferenceEngine: Engine file size: " + std::to_string(fileSize / 1024 / 1024) + " MB");
    
    // Read engine data
    std::vector<char> engineData(fileSize);
    if (!file.read(engineData.data(), fileSize)) {
        Logger::Error("InferenceEngine: Failed to read engine file");
        return false;
    }
    file.close();
    
    // Create TensorRT runtime
    m_runtime = nvinfer1::createInferRuntime(m_trtLogger);
    if (!m_runtime) {
        Logger::Error("InferenceEngine: Failed to create TensorRT runtime");
        return false;
    }
    
    // Deserialize the CUDA engine from the engine data
    m_engine = m_runtime->deserializeCudaEngine(engineData.data(), fileSize);
    if (!m_engine) {
        Logger::Error("InferenceEngine: Failed to deserialize TensorRT engine");
        return false;
    }
    
    // Create execution context
    m_context = m_engine->createExecutionContext();
    if (!m_context) {
        Logger::Error("InferenceEngine: Failed to create TensorRT execution context");
        return false;
    }
    
    // Discover input/output tensor names and shapes
    int numIOTensors = m_engine->getNbIOTensors();
    Logger::Info("InferenceEngine: Engine has " + std::to_string(numIOTensors) + " I/O tensors");
    
    for (int i = 0; i < numIOTensors; ++i) {
        const char* tensorName = m_engine->getIOTensorName(i);
        nvinfer1::TensorIOMode ioMode = m_engine->getTensorIOMode(tensorName);
        nvinfer1::Dims dims = m_engine->getTensorShape(tensorName);
        
        std::string dimsStr;
        for (int d = 0; d < dims.nbDims; ++d) {
            if (d > 0) dimsStr += "x";
            dimsStr += std::to_string(dims.d[d]);
        }
        
        if (ioMode == nvinfer1::TensorIOMode::kINPUT) {
            m_inputTensorName = tensorName;
            Logger::Info("InferenceEngine: Input tensor '" + m_inputTensorName + "' shape: " + dimsStr);
            
            // Verify input dimensions match config
            // Expected: [batch, channels, height, width] = [12, 3, 640, 640]
            if (dims.nbDims >= 4) {
                int engineBatch = dims.d[0];
                int engineHeight = dims.d[2];
                int engineWidth = dims.d[3];
                
                if (engineBatch < m_config.batchSize) {
                    Logger::Warning("InferenceEngine: Engine batch size (" + std::to_string(engineBatch) + 
                                   ") < configured batch size (" + std::to_string(m_config.batchSize) + ")");
                }
                if (engineHeight != m_config.inputHeight || engineWidth != m_config.inputWidth) {
                    Logger::Warning("InferenceEngine: Engine input size (" + 
                                   std::to_string(engineWidth) + "x" + std::to_string(engineHeight) +
                                   ") != configured size (" + 
                                   std::to_string(m_config.inputWidth) + "x" + std::to_string(m_config.inputHeight) + ")");
                    m_config.inputWidth = engineWidth;
                    m_config.inputHeight = engineHeight;
                }
            }
        } else if (ioMode == nvinfer1::TensorIOMode::kOUTPUT) {
            m_outputTensorName = tensorName;
            Logger::Info("InferenceEngine: Output tensor '" + m_outputTensorName + "' shape: " + dimsStr);
            
            // Parse output dimensions to determine detection format
            // Typical YOLO output: [batch, num_detections, 5+num_classes] or [batch, 5+num_classes, num_detections]
            if (dims.nbDims >= 2) {
                // Determine which dimension is detections vs attributes
                if (dims.nbDims == 3) {
                    // Common format: [batch, detections, attributes] or [batch, attributes, detections]
                    if (dims.d[1] > dims.d[2]) {
                        // [batch, detections, attributes]
                        m_maxDetections = dims.d[1];
                        m_outputStride = dims.d[2];
                    } else {
                        // [batch, attributes, detections] - transposed format
                        m_maxDetections = dims.d[2];
                        m_outputStride = dims.d[1];
                    }
                } else if (dims.nbDims == 2) {
                    // [total_detections, attributes]
                    m_maxDetections = dims.d[0] / m_config.batchSize;
                    m_outputStride = dims.d[1];
                }
                
                Logger::Info("InferenceEngine: Detected output format - max_detections=" + 
                            std::to_string(m_maxDetections) + ", stride=" + std::to_string(m_outputStride));
            }
        }
    }
    
    if (m_inputTensorName.empty() || m_outputTensorName.empty()) {
        Logger::Error("InferenceEngine: Failed to find input/output tensors");
        return false;
    }
    
    Logger::Info("InferenceEngine: TensorRT engine loaded successfully from " + enginePath);
    return true;
}

bool InferenceEngine::AllocateBuffers() {
    // Calculate input buffer size
    m_inputSize = static_cast<size_t>(m_config.batchSize) * kInputChannels * 
                  m_config.inputWidth * m_config.inputHeight * sizeof(float);
    
    // Calculate output buffer size using discovered tensor dimensions
    // Use discovered values if available, otherwise use defaults
    if (m_outputStride == 0) {
        m_outputStride = 5 + m_config.numClasses;  // Default: [x, y, w, h, conf, class_scores...]
    }
    m_outputSize = static_cast<size_t>(m_config.batchSize) * m_maxDetections * 
                   m_outputStride * sizeof(float);
    
    Logger::Info("InferenceEngine: Allocating buffers - Input: " + 
                 std::to_string(m_inputSize / (1024 * 1024)) + " MB (" +
                 std::to_string(m_config.batchSize) + "x" + std::to_string(kInputChannels) + "x" +
                 std::to_string(m_config.inputHeight) + "x" + std::to_string(m_config.inputWidth) + ")");
    Logger::Info("InferenceEngine: Output: " + 
                 std::to_string(m_outputSize / (1024 * 1024)) + " MB (" +
                 std::to_string(m_config.batchSize) + "x" + std::to_string(m_maxDetections) + "x" +
                 std::to_string(m_outputStride) + ")");
    
    // Allocate device memory for input
    cudaError_t err = cudaMalloc(&m_inputBuffer, m_inputSize);
    if (err != cudaSuccess) {
        Logger::Error("InferenceEngine: Failed to allocate input buffer: " + 
                      std::string(cudaGetErrorString(err)));
        return false;
    }
    
    // Allocate device memory for output
    err = cudaMalloc(&m_outputBuffer, m_outputSize);
    if (err != cudaSuccess) {
        cudaFree(m_inputBuffer);
        m_inputBuffer = nullptr;
        Logger::Error("InferenceEngine: Failed to allocate output buffer: " + 
                      std::string(cudaGetErrorString(err)));
        return false;
    }
    
    // Allocate pinned host memory for results (faster GPU→CPU transfer)
    err = cudaMallocHost(&m_hostOutput, m_outputSize);
    if (err != cudaSuccess) {
        FreeBuffers();
        Logger::Error("InferenceEngine: Failed to allocate host output buffer: " + 
                      std::string(cudaGetErrorString(err)));
        return false;
    }
    
    Logger::Info("InferenceEngine: Allocated buffers - Input: " + 
                 std::to_string(m_inputSize / 1024 / 1024) + " MB, Output: " +
                 std::to_string(m_outputSize / 1024 / 1024) + " MB");
    
    return true;
}

void InferenceEngine::FreeBuffers() {
    if (m_inputBuffer) {
        cudaFree(m_inputBuffer);
        m_inputBuffer = nullptr;
    }
    if (m_outputBuffer) {
        cudaFree(m_outputBuffer);
        m_outputBuffer = nullptr;
    }
    if (m_hostOutput) {
        cudaFreeHost(m_hostOutput);
        m_hostOutput = nullptr;
    }
}

void InferenceEngine::PreprocessBatch(void** cudaBuffers, const unsigned int* widths,
                                       const unsigned int* heights, int numFrames, 
                                       cudaStream_t stream) {
    // Call CUDA preprocessing kernel for each frame in the batch
    // The kernel performs: resize, BGRA→RGB conversion, normalization to [0,1], NCHW layout
    
    float* inputFloat = static_cast<float*>(m_inputBuffer);
    
    for (int i = 0; i < numFrames; ++i) {
        cudaError_t err = LaunchPreprocessBGRA(
            cudaBuffers[i],
            inputFloat,
            static_cast<int>(widths[i]),
            static_cast<int>(heights[i]),
            m_config.inputWidth,
            m_config.inputHeight,
            i,  // batch index
            stream
        );
        
        if (err != cudaSuccess) {
            Logger::Error("InferenceEngine: Preprocessing failed for frame " + std::to_string(i) + 
                         ": " + std::string(cudaGetErrorString(err)));
        }
    }
}

std::vector<BallDetection> InferenceEngine::PostProcess(const float* rawOutput,
                                                         const int* cameraIDs, 
                                                         int numFrames) {
    std::vector<BallDetection> detections;
    
    if (!rawOutput) {
        return detections;
    }
    
    // Parse YOLO output format
    // Using discovered dimensions from engine loading
    // Typically: [batch, num_detections, 5 + num_classes]
    // where [x, y, w, h, conf, class_scores...]
    
    int64_t now = GetCurrentTimeMs();
    
    // Use member variables that were set during engine loading
    int stride = (m_outputStride > 0) ? m_outputStride : (5 + m_config.numClasses);
    int numClasses = stride - 5;
    
    for (int b = 0; b < numFrames; ++b) {
        const float* batchOutput = rawOutput + b * m_maxDetections * stride;
        
        for (int d = 0; d < m_maxDetections; ++d) {
            const float* det = batchOutput + d * stride;
            
            float objectness = det[4];
            if (objectness < m_config.confidenceThreshold) {
                continue;
            }
            
            // Find best class
            int bestClass = 0;
            float bestScore = 0;
            for (int c = 0; c < numClasses; ++c) {
                if (det[5 + c] > bestScore) {
                    bestScore = det[5 + c];
                    bestClass = c;
                }
            }
            
            float confidence = objectness * bestScore;
            if (confidence < m_config.confidenceThreshold) {
                continue;
            }
            
            BallDetection bd;
            bd.ballID = bestClass + 1;  // 1-indexed ball IDs
            bd.cameraID = cameraIDs[b];
            bd.x = det[0];
            bd.y = det[1];
            bd.width = det[2];
            bd.height = det[3];
            bd.confidence = confidence;
            bd.timestamp = now;
            
            detections.push_back(bd);
        }
    }
    
    return detections;
}

void InferenceEngine::ApplyNMS(std::vector<BallDetection>& detections) {
    if (detections.size() <= 1) {
        return;
    }
    
    // Sort by confidence (descending)
    std::sort(detections.begin(), detections.end(),
              [](const BallDetection& a, const BallDetection& b) {
                  return a.confidence > b.confidence;
              });
    
    std::vector<bool> suppressed(detections.size(), false);
    
    auto computeIoU = [](const BallDetection& a, const BallDetection& b) -> float {
        float x1 = std::max(a.x - a.width / 2, b.x - b.width / 2);
        float y1 = std::max(a.y - a.height / 2, b.y - b.height / 2);
        float x2 = std::min(a.x + a.width / 2, b.x + b.width / 2);
        float y2 = std::min(a.y + a.height / 2, b.y + b.height / 2);
        
        float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
        float areaA = a.width * a.height;
        float areaB = b.width * b.height;
        float unionArea = areaA + areaB - intersection;
        
        return (unionArea > 0) ? intersection / unionArea : 0.0f;
    };
    
    for (size_t i = 0; i < detections.size(); ++i) {
        if (suppressed[i]) continue;
        
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (suppressed[j]) continue;
            
            // Only apply NMS within same camera
            if (detections[i].cameraID != detections[j].cameraID) continue;
            
            float iou = computeIoU(detections[i], detections[j]);
            if (iou > m_config.nmsThreshold) {
                suppressed[j] = true;
            }
        }
    }
    
    // Remove suppressed detections
    auto it = std::remove_if(detections.begin(), detections.end(),
                             [&suppressed, &detections](const BallDetection& d) {
                                 size_t idx = &d - detections.data();
                                 return suppressed[idx];
                             });
    detections.erase(it, detections.end());
}

int64_t InferenceEngine::GetCurrentTimeMs() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}
