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
#include <sstream>
#include <iomanip>

// YOLO output format constants
// Base attributes: [x, y, w, h, objectness]
constexpr int kYoloBaseAttributes = 5;

// Default max detections per frame for YOLOv8-style models
constexpr int kDefaultMaxDetections = 8400;

// External preprocessing kernel functions from PreprocessKernel.cu and FusedPreprocessKernel.cu
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
    
    // Fused UYVY→RGB kernel (supports configurable dimensions)
    cudaError_t LaunchFusedUYVYPreprocess(
        const void* srcUYVY,
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
    , m_hasDynamicShapes(false)
    , m_inputBuffer(nullptr)
    , m_outputBuffer(nullptr)
    , m_inputSize(0)
    , m_outputSize(0)
    , m_maxDetections(kDefaultMaxDetections)
    , m_outputStride(0)
    , m_hostOutput(nullptr)
{
    std::memset(&m_lastTelemetry, 0, sizeof(m_lastTelemetry));
}

InferenceEngine::~InferenceEngine() {
    FreeBuffers();
    
    // Free TensorRT resources in reverse order of creation
    // Note: In TensorRT 10.x, IExecutionContext should be deleted, not destroy()
    // The destroy() pattern is deprecated in TensorRT 8+
    if (m_context) {
        delete m_context;
        m_context = nullptr;
    }
    if (m_engine) {
        delete m_engine;
        m_engine = nullptr;
    }
    if (m_runtime) {
        delete m_runtime;
        m_runtime = nullptr;
    }
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

std::vector<BallDetection> InferenceEngine::ProcessFrameUYVY(
    void* cudaUYVYBuffer, int cameraID,
    unsigned int width, unsigned int height,
    cudaStream_t stream,
    cudaEvent_t preprocessEvent) {
    
    if (!m_initialized || !cudaUYVYBuffer) {
        return {};
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    std::vector<BallDetection> detections;
    
    if (m_stubMode) {
        // Stub mode: return simulated detection
        int64_t now = GetCurrentTimeMs();
        BallDetection det;
        det.ballID = 0;  // Ball ID 0 (B0)
        det.cameraID = cameraID;
        det.x = 0.5f;
        det.y = 0.5f;
        det.width = 0.05f;
        det.height = 0.05f;
        det.confidence = 0.85f;
        det.timestamp = now;
        detections.push_back(det);
        return detections;
    }
    
    // Real TensorRT inference with FUSED UYVY kernel
    // This path eliminates the intermediate BGRA buffer
    
    // 1. Fused preprocessing: UYVY→RGB at configured dimensions in one kernel
    auto preprocessStart = std::chrono::high_resolution_clock::now();
    
    float* inputFloat = static_cast<float*>(m_inputBuffer);
    cudaError_t err = LaunchFusedUYVYPreprocess(
        cudaUYVYBuffer,
        inputFloat,
        static_cast<int>(width),
        static_cast<int>(height),
        m_config.inputWidth,
        m_config.inputHeight,
        0,  // batch index 0 for single frame
        stream
    );
    
    if (err != cudaSuccess) {
        Logger::Error("InferenceEngine: Fused UYVY preprocessing failed: " + 
                     std::string(cudaGetErrorString(err)));
        return {};
    }
    
    // Record event after preprocessing (if provided)
    if (preprocessEvent) {
        cudaEventRecord(preprocessEvent, stream);
    }
    
    auto preprocessEnd = std::chrono::high_resolution_clock::now();
    
    // 2. Run TensorRT inference (WITHOUT mutex - using event-based sync instead)
    auto inferenceStart = std::chrono::high_resolution_clock::now();
    
    // For dynamic shape engines, set the input shape before inference
    if (m_hasDynamicShapes) {
        nvinfer1::Dims inputDims;
        inputDims.nbDims = 4;
        inputDims.d[0] = 1;  // Batch size = 1 for single frame
        inputDims.d[1] = kInputChannels;  // RGB channels
        inputDims.d[2] = m_config.inputHeight;
        inputDims.d[3] = m_config.inputWidth;
        
        if (!m_context->setInputShape(m_inputTensorName.c_str(), inputDims)) {
            Logger::Error("InferenceEngine: Failed to set input shape for dynamic engine");
            return {};
        }
    }
    
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
    // IMPORTANT: No mutex here - each camera has its own stream and this is async
    if (!m_context->enqueueV3(stream)) {
        Logger::Error("InferenceEngine: TensorRT inference execution failed");
        return {};
    }
    
    // Copy results to host asynchronously
    cudaMemcpyAsync(m_hostOutput, m_outputBuffer, m_outputSize, cudaMemcpyDeviceToHost, stream);
    
    // Wait for stream to complete (only this camera's work)
    cudaStreamSynchronize(stream);
    
    auto inferenceEnd = std::chrono::high_resolution_clock::now();
    
    // 3. Post-process
    auto postprocessStart = std::chrono::high_resolution_clock::now();
    int cameras[1] = { cameraID };
    detections = PostProcess(m_hostOutput, cameras, 1);
    ApplyNMS(detections);
    auto postprocessEnd = std::chrono::high_resolution_clock::now();
    
    // Update telemetry
    m_lastTelemetry.preprocess_ms = std::chrono::duration<float, std::milli>(preprocessEnd - preprocessStart).count();
    m_lastTelemetry.inference_ms = std::chrono::duration<float, std::milli>(inferenceEnd - inferenceStart).count();
    m_lastTelemetry.postprocess_ms = std::chrono::duration<float, std::milli>(postprocessEnd - postprocessStart).count();
    m_lastTelemetry.total_ms = std::chrono::duration<float, std::milli>(postprocessEnd - startTime).count();
    m_lastTelemetry.detectionsCount = static_cast<int>(detections.size());
    m_lastTelemetry.batchSize = 1;
    
    return detections;
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
            det.ballID = i % 10;  // Ball IDs 0-9 (B0-B9)
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
    // IMPORTANT: TensorRT IExecutionContext is NOT thread-safe. Multiple threads
    // calling enqueueV3() on the same context simultaneously causes "Called with an
    // already loaded binary graph" Myelin errors. We use mutex serialization here.
    // For better parallelism, consider using multiple execution contexts (one per stream).
    
    // 1. Preprocess frames (launches CUDA kernels on stream)
    auto preprocessStart = std::chrono::high_resolution_clock::now();
    PreprocessBatch(cudaBuffers, widths, heights, numFrames, stream);
    // Note: No sync needed here - TensorRT inference will wait for preprocessing
    // kernels on the same stream to complete (implicit stream ordering)
    auto preprocessEnd = std::chrono::high_resolution_clock::now();
    
    // 2. Run TensorRT inference (serialized via mutex for thread safety)
    auto inferenceStart = std::chrono::high_resolution_clock::now();
    
    {
        // Acquire lock before accessing execution context
        // This serializes inference calls from multiple capture threads
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // For dynamic shape engines, set the input shape before inference
        if (m_hasDynamicShapes) {
            nvinfer1::Dims inputDims;
            inputDims.nbDims = 4;
            inputDims.d[0] = numFrames;  // Actual batch size
            inputDims.d[1] = kInputChannels;  // RGB channels
            inputDims.d[2] = m_config.inputHeight;
            inputDims.d[3] = m_config.inputWidth;
            
            if (!m_context->setInputShape(m_inputTensorName.c_str(), inputDims)) {
                Logger::Error("InferenceEngine: Failed to set input shape for dynamic engine");
                return {};
            }
        }
        
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
        // This implicitly waits for preprocessing kernels to complete (same stream)
        if (!m_context->enqueueV3(stream)) {
            Logger::Error("InferenceEngine: TensorRT inference execution failed");
            return {};
        }
        
        // Copy results to host asynchronously while still holding the lock
        // This ensures the output buffer isn't overwritten by another thread
        cudaMemcpyAsync(m_hostOutput, m_outputBuffer, m_outputSize, cudaMemcpyDeviceToHost, stream);
        
        // Wait for copy to complete before releasing lock
        // This is required because the output buffer is shared
        cudaStreamSynchronize(stream);
    }
    // Lock released here - other threads can now run inference
    
    auto inferenceEnd = std::chrono::high_resolution_clock::now();
    
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
            
            // Check for dynamic shapes (indicated by -1 in dimensions)
            m_hasDynamicShapes = false;
            for (int d = 0; d < dims.nbDims; ++d) {
                if (dims.d[d] == -1) {
                    m_hasDynamicShapes = true;
                    Logger::Info("InferenceEngine: Detected dynamic shape in dimension " + std::to_string(d));
                    break;
                }
            }
            
            // Verify input dimensions match config
            // Expected: [batch, channels, height, width] e.g., [12, 3, 720, 1280] or [12, 3, 640, 640]
            if (dims.nbDims >= 4) {
                int engineBatch = static_cast<int>(dims.d[0]);
                int engineHeight = static_cast<int>(dims.d[2]);
                int engineWidth = static_cast<int>(dims.d[3]);
                
                // For dynamic batch size, engineBatch will be -1
                if (engineBatch > 0 && engineBatch < m_config.batchSize) {
                    Logger::Warning("InferenceEngine: Engine batch size (" + std::to_string(engineBatch) + 
                                   ") < configured batch size (" + std::to_string(m_config.batchSize) + 
                                   "). Will process at most " + std::to_string(engineBatch) + " frames per batch.");
                }
                if (engineHeight != m_config.inputHeight || engineWidth != m_config.inputWidth) {
                    // Note: We update config to match engine dimensions for correct buffer allocation
                    // The engine was compiled with specific dimensions that must be honored
                    Logger::Warning("InferenceEngine: Adapting to engine input size (" + 
                                   std::to_string(engineWidth) + "x" + std::to_string(engineHeight) +
                                   ") from configured (" + 
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
                        m_maxDetections = static_cast<int>(dims.d[1]);
                        m_outputStride = static_cast<int>(dims.d[2]);
                    } else {
                        // [batch, attributes, detections] - transposed format
                        m_maxDetections = static_cast<int>(dims.d[2]);
                        m_outputStride = static_cast<int>(dims.d[1]);
                    }
                } else if (dims.nbDims == 2) {
                    // [total_detections, attributes]
                    m_maxDetections = static_cast<int>(dims.d[0]) / m_config.batchSize;
                    m_outputStride = static_cast<int>(dims.d[1]);
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
        m_outputStride = kYoloBaseAttributes + m_config.numClasses;  // Default: [x, y, w, h, conf, class_scores...]
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
    // Typically: [batch, num_detections, kYoloBaseAttributes + num_classes]
    // where kYoloBaseAttributes = [x, y, w, h, conf]
    
    int64_t now = GetCurrentTimeMs();
    
    // Use member variables that were set during engine loading
    int stride = (m_outputStride > 0) ? m_outputStride : (kYoloBaseAttributes + m_config.numClasses);
    int numClasses = stride - kYoloBaseAttributes;
    
    // ============================================================================
    // DEBUG: Print raw YOLO output for verification
    // ============================================================================
    static int debugFrameCount = 0;
    debugFrameCount++;
    
    // Print raw output every 30 frames (approx 1 second at 30fps)
    if (debugFrameCount % 30 == 1) {
        Logger::Info("=== DEBUG YOLO RAW OUTPUT (Frame " + std::to_string(debugFrameCount) + ") ===");
        Logger::Info("Output format: maxDetections=" + std::to_string(m_maxDetections) + 
                     ", stride=" + std::to_string(stride) + ", numClasses=" + std::to_string(numClasses));
        Logger::Info("Confidence threshold: " + std::to_string(m_config.confidenceThreshold));
        
        // Print first 10 detections raw values
        int printCount = std::min(10, m_maxDetections);
        for (int d = 0; d < printCount; ++d) {
            const float* det = rawOutput + d * stride;
            std::ostringstream oss;
            oss << "Det[" << d << "]: ";
            for (int s = 0; s < stride; ++s) {
                oss << std::fixed << std::setprecision(4) << det[s];
                if (s < stride - 1) oss << ", ";
            }
            Logger::Info(oss.str());
        }
        
        // Find and print max confidence value in entire output
        float maxConf = 0.0f;
        int maxConfIdx = -1;
        for (int d = 0; d < m_maxDetections; ++d) {
            const float* det = rawOutput + d * stride;
            if (det[4] > maxConf) {
                maxConf = det[4];
                maxConfIdx = d;
            }
        }
        Logger::Info("Max confidence found: " + std::to_string(maxConf) + " at detection index " + std::to_string(maxConfIdx));
        Logger::Info("=== END DEBUG YOLO RAW OUTPUT ===");
    }
    
    for (int b = 0; b < numFrames; ++b) {
        const float* batchOutput = rawOutput + b * m_maxDetections * stride;
        
        for (int d = 0; d < m_maxDetections; ++d) {
            const float* det = batchOutput + d * stride;
            
            // Index 4 is objectness confidence (after x, y, w, h)
            float objectness = det[4];
            if (objectness < m_config.confidenceThreshold) {
                continue;
            }
            
            // Determine class ID based on output format
            int bestClass = 0;
            float confidence = objectness;
            
            if (numClasses == 1) {
                // Direct class ID format: [x, y, w, h, objectness, class_id]
                // The model outputs the class ID directly at index 5
                bestClass = static_cast<int>(det[5]);
                confidence = objectness;  // Use objectness as final confidence
                
                // Debug log for first few detections
                static int debugDetCount = 0;
                if (debugDetCount < 5) {
                    Logger::Info("[CLASS_DEBUG] Detection " + std::to_string(debugDetCount) + 
                                ": classID=" + std::to_string(bestClass) + 
                                " (raw value=" + std::to_string(det[5]) + 
                                "), ballID will be B" + std::to_string(bestClass));
                    debugDetCount++;
                }
            } else {
                // Multi-class probability format: [x, y, w, h, objectness, class_score_0, class_score_1, ...]
                // Find best class from probability scores
                float bestScore = 0;
                for (int c = 0; c < numClasses; ++c) {
                    if (det[kYoloBaseAttributes + c] > bestScore) {
                        bestScore = det[kYoloBaseAttributes + c];
                        bestClass = c;
                    }
                }
                confidence = objectness * bestScore;
            }
            
            // Apply confidence threshold check uniformly for both formats
            // Note: For numClasses==1, this is redundant with the objectness check above,
            // but kept for code clarity and consistency
            if (confidence < m_config.confidenceThreshold) {
                continue;
            }
            
            BallDetection bd;
            bd.ballID = bestClass;  // 0-based ball IDs (B0-B9, matching YOLO class IDs 0-9)
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
