/**
 * ActiveCameraSelector.cpp
 * 
 * Implementation of GPU-accelerated camera selection
 */

#include "ActiveCameraSelector.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>

// Forward declarations of CUDA kernel wrappers
extern "C" {
    cudaError_t LaunchMotionDetection(
        const void* prevFrame, const void* currFrame,
        float* motionMap, float* partialSums,
        unsigned int width, unsigned int height,
        cudaStream_t stream);
    
    cudaError_t LaunchEdgeActivityCalculation(
        const float* motionMap, float* edgeScore,
        unsigned int width, unsigned int height,
        float edgeMargin, cudaStream_t stream);
}

ActiveCameraSelector::ActiveCameraSelector(int numCameras, int topK, 
                                          float motionThreshold, float edgeMargin)
    : m_numCameras(numCameras)
    , m_topK(topK)
    , m_motionThreshold(motionThreshold)
    , m_edgeMargin(edgeMargin)
    , m_initialized(false)
    , m_warmupFrameCount(0)
    , m_isStable(false)
{
    Logger::Info("ActiveCameraSelector created: " + std::to_string(numCameras) + 
                 " cameras, Top-" + std::to_string(topK) + " selection");
}

ActiveCameraSelector::~ActiveCameraSelector() {
    // Free all camera resources
    for (int i = 0; i < m_numCameras; i++) {
        FreeCameraResources(i);
    }
}

bool ActiveCameraSelector::Initialize() {
    if (m_initialized) {
        Logger::Warning("ActiveCameraSelector already initialized");
        return true;
    }
    
    // Initialize camera states
    m_cameraStates.resize(m_numCameras);
    
    for (int i = 0; i < m_numCameras; i++) {
        auto& state = m_cameraStates[i];
        state.previousFrame = nullptr;
        state.currentFrame = nullptr;
        state.motionBuffer = nullptr;
        state.hostMotionScore = nullptr;
        state.processingComplete = nullptr;
        state.hasHistory = false;
        state.width = 0;
        state.height = 0;
        state.frameSize = 0;
        
        state.metrics.cameraID = i;
        state.metrics.motionScore = 0.0f;
        state.metrics.edgeActivity = 0.0f;
        state.metrics.isActive = false;
        state.metrics.frameCount = 0;
        state.metrics.consecutiveActiveFrames = 0;
        state.metrics.decayedScore = 0.0f;
    }
    
    m_initialized = true;
    Logger::Info("ActiveCameraSelector initialized successfully");
    return true;
}

bool ActiveCameraSelector::AllocateCameraResources(int cameraID, 
                                                   unsigned int width, 
                                                   unsigned int height) {
    if (cameraID < 0 || cameraID >= m_numCameras) {
        return false;
    }
    
    auto& state = m_cameraStates[cameraID];
    std::lock_guard<std::recursive_mutex> lock(state.frameMutex);
    
    // Check if resources already allocated for this resolution
    if (state.width == width && state.height == height && state.previousFrame != nullptr) {
        return true;
    }
    
    // Free existing resources if any
    if (state.previousFrame) {
        cudaFree(state.previousFrame);
        cudaFree(state.currentFrame);
        cudaFree(state.motionBuffer);
        cudaFreeHost(state.hostMotionScore);
        if (state.processingComplete) {
            cudaEventDestroy(state.processingComplete);
        }
    }
    
    // Calculate sizes
    size_t frameSize = width * height * 2;  // UYVY: 2 bytes per pixel
    size_t motionMapSize = width * height * sizeof(float);
    
    // Allocate CUDA device memory for frames
    cudaError_t err = cudaMalloc(&state.previousFrame, frameSize);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate previous frame for camera " + std::to_string(cameraID));
        return false;
    }
    
    err = cudaMalloc(&state.currentFrame, frameSize);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate current frame for camera " + std::to_string(cameraID));
        cudaFree(state.previousFrame);
        state.previousFrame = nullptr;
        return false;
    }
    
    // Allocate motion map
    err = cudaMalloc(&state.motionBuffer, motionMapSize);
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate motion buffer for camera " + std::to_string(cameraID));
        cudaFree(state.previousFrame);
        cudaFree(state.currentFrame);
        state.previousFrame = nullptr;
        state.currentFrame = nullptr;
        return false;
    }
    
    // Allocate pinned host memory for motion score
    err = cudaMallocHost(&state.hostMotionScore, sizeof(float));
    if (err != cudaSuccess) {
        Logger::Error("Failed to allocate pinned memory for camera " + std::to_string(cameraID));
        cudaFree(state.previousFrame);
        cudaFree(state.currentFrame);
        cudaFree(state.motionBuffer);
        state.previousFrame = nullptr;
        state.currentFrame = nullptr;
        state.motionBuffer = nullptr;
        return false;
    }
    
    // Create CUDA event
    err = cudaEventCreateWithFlags(&state.processingComplete, cudaEventDisableTiming);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create CUDA event for camera " + std::to_string(cameraID));
        cudaFree(state.previousFrame);
        cudaFree(state.currentFrame);
        cudaFree(state.motionBuffer);
        cudaFreeHost(state.hostMotionScore);
        state.previousFrame = nullptr;
        state.currentFrame = nullptr;
        state.motionBuffer = nullptr;
        state.hostMotionScore = nullptr;
        return false;
    }
    
    state.width = width;
    state.height = height;
    state.frameSize = frameSize;
    state.hasHistory = false;
    
    Logger::Debug("Allocated resources for camera " + std::to_string(cameraID) + 
                  " (" + std::to_string(width) + "x" + std::to_string(height) + ")");
    
    return true;
}

void ActiveCameraSelector::FreeCameraResources(int cameraID) {
    if (cameraID < 0 || cameraID >= m_numCameras) {
        return;
    }
    
    auto& state = m_cameraStates[cameraID];
    std::lock_guard<std::recursive_mutex> lock(state.frameMutex);
    
    if (state.previousFrame) {
        cudaFree(state.previousFrame);
        state.previousFrame = nullptr;
    }
    if (state.currentFrame) {
        cudaFree(state.currentFrame);
        state.currentFrame = nullptr;
    }
    if (state.motionBuffer) {
        cudaFree(state.motionBuffer);
        state.motionBuffer = nullptr;
    }
    if (state.hostMotionScore) {
        cudaFreeHost(state.hostMotionScore);
        state.hostMotionScore = nullptr;
    }
    if (state.processingComplete) {
        cudaEventDestroy(state.processingComplete);
        state.processingComplete = nullptr;
    }
    
    state.width = 0;
    state.height = 0;
    state.frameSize = 0;
    state.hasHistory = false;
}

bool ActiveCameraSelector::ProcessFrame(int cameraID, void* cudaYUVBuffer,
                                        unsigned int width, unsigned int height,
                                        cudaStream_t stream) {
    if (!m_initialized) {
        Logger::Error("ActiveCameraSelector not initialized");
        return false;
    }
    
    if (cameraID < 0 || cameraID >= m_numCameras) {
        Logger::Error("Invalid camera ID: " + std::to_string(cameraID));
        return false;
    }
    
    auto& state = m_cameraStates[cameraID];
    std::lock_guard<std::recursive_mutex> lock(state.frameMutex);
    
    // Allocate resources if needed
    if (!AllocateCameraResources(cameraID, width, height)) {
        return false;
    }
    
    // Copy current frame data
    cudaError_t err = cudaMemcpyAsync(state.currentFrame, cudaYUVBuffer,
                                     state.frameSize, cudaMemcpyDeviceToDevice, stream);
    if (err != cudaSuccess) {
        Logger::Error("Failed to copy frame for camera " + std::to_string(cameraID));
        return false;
    }
    
    // If we have history, calculate motion
    if (state.hasHistory) {
        float motionScore = CalculateMotionScore(cameraID, stream);
        float edgeActivity = CalculateEdgeActivity(cameraID, stream);
        
        // Update metrics
        state.metrics.motionScore = motionScore;
        state.metrics.edgeActivity = edgeActivity;
        state.metrics.frameCount++;
        
        // Record event for synchronization
        cudaEventRecord(state.processingComplete, stream);
    }
    
    // Swap buffers: current becomes previous
    void* temp = state.previousFrame;
    state.previousFrame = state.currentFrame;
    state.currentFrame = temp;
    state.hasHistory = true;
    
    return true;
}

float ActiveCameraSelector::CalculateMotionScore(int cameraID, cudaStream_t stream) {
    auto& state = m_cameraStates[cameraID];
    
    // Allocate temporary buffer for partial sums
    unsigned int numPixels = state.width * state.height;
    unsigned int threadsPerBlock = 256;
    unsigned int numBlocks = (numPixels + threadsPerBlock * 2 - 1) / (threadsPerBlock * 2);
    
    float* devicePartialSums;
    cudaMalloc(&devicePartialSums, numBlocks * sizeof(float));
    
    // Launch motion detection kernels
    cudaError_t err = LaunchMotionDetection(
        state.previousFrame,
        state.currentFrame,
        static_cast<float*>(state.motionBuffer),
        devicePartialSums,
        state.width,
        state.height,
        stream
    );
    
    if (err != cudaSuccess) {
        Logger::Error("Motion detection kernel failed for camera " + std::to_string(cameraID));
        cudaFree(devicePartialSums);
        return 0.0f;
    }
    
    // Copy partial sums to host and finalize reduction
    std::vector<float> hostPartialSums(numBlocks);
    cudaMemcpyAsync(hostPartialSums.data(), devicePartialSums, numBlocks * sizeof(float),
                   cudaMemcpyDeviceToHost, stream);
    
    // Record event instead of blocking synchronization
    cudaEvent_t copyComplete;
    cudaEventCreate(&copyComplete);
    cudaEventRecord(copyComplete, stream);
    
    // Wait for event (non-blocking alternative - can be queried later if needed)
    cudaEventSynchronize(copyComplete);
    
    // Final reduction on CPU
    float totalMotion = 0.0f;
    for (unsigned int i = 0; i < numBlocks; i++) {
        totalMotion += hostPartialSums[i];
    }
    
    // Normalize by number of pixels
    float normalizedScore = totalMotion / static_cast<float>(numPixels);
    
    cudaEventDestroy(copyComplete);
    cudaFree(devicePartialSums);
    
    return normalizedScore;
}

float ActiveCameraSelector::CalculateEdgeActivity(int cameraID, cudaStream_t stream) {
    auto& state = m_cameraStates[cameraID];
    
    float* deviceEdgeScore;
    cudaMalloc(&deviceEdgeScore, sizeof(float));
    
    cudaError_t err = LaunchEdgeActivityCalculation(
        static_cast<float*>(state.motionBuffer),
        deviceEdgeScore,
        state.width,
        state.height,
        m_edgeMargin,
        stream
    );
    
    if (err != cudaSuccess) {
        Logger::Error("Edge activity calculation failed for camera " + std::to_string(cameraID));
        cudaFree(deviceEdgeScore);
        return 0.0f;
    }
    
    float edgeScore;
    cudaMemcpyAsync(&edgeScore, deviceEdgeScore, sizeof(float),
                   cudaMemcpyDeviceToHost, stream);
    cudaStreamSynchronize(stream);
    
    // Normalize by edge pixels
    unsigned int edgeWidth = static_cast<unsigned int>(state.width * m_edgeMargin);
    unsigned int edgeHeight = static_cast<unsigned int>(state.height * m_edgeMargin);
    unsigned int edgePixels = 2 * edgeWidth * state.height + 
                              2 * edgeHeight * (state.width - 2 * edgeWidth);
    
    float normalizedEdge = (edgePixels > 0) ? (edgeScore / edgePixels) : 0.0f;
    
    cudaFree(deviceEdgeScore);
    
    return normalizedEdge;
}

std::vector<int> ActiveCameraSelector::SelectTopK() {
    // Create list of cameras with their scores
    std::vector<std::pair<float, int>> scoreMap;
    
    for (int i = 0; i < m_numCameras; i++) {
        auto& state = m_cameraStates[i];
        float rawScore = state.metrics.motionScore;
        
        // Apply hysteresis: decay inactive cameras, boost active ones
        if (state.metrics.isActive) {
            // Camera is currently active - use raw score
            state.metrics.decayedScore = rawScore;
            state.metrics.consecutiveActiveFrames++;
        } else {
            // Camera is inactive - apply decay factor
            state.metrics.decayedScore *= m_hysteresisConfig.decay_factor;
            // Update with current score if higher
            if (rawScore > state.metrics.decayedScore) {
                state.metrics.decayedScore = rawScore;
            }
            state.metrics.consecutiveActiveFrames = 0;
        }
        
        // Only consider cameras above threshold
        if (state.metrics.decayedScore >= m_motionThreshold) {
            scoreMap.push_back({state.metrics.decayedScore, i});
        }
    }
    
    // Sort descending by decayed score
    std::sort(scoreMap.begin(), scoreMap.end(), 
              [](const auto& a, const auto& b) { return a.first > b.first; });
    
    // Apply hysteresis logic: protect currently active cameras from being replaced
    std::vector<int> selected;
    std::vector<int> currentlyActive;
    
    // Collect currently active cameras
    for (int i = 0; i < m_numCameras; i++) {
        if (m_cameraStates[i].metrics.isActive) {
            currentlyActive.push_back(i);
        }
    }
    
    // First, keep active cameras that meet minimum frame count requirement
    for (int camID : currentlyActive) {
        auto& state = m_cameraStates[camID];
        if (state.metrics.consecutiveActiveFrames < m_hysteresisConfig.min_active_frames) {
            // Camera must stay active for minimum frames
            selected.push_back(camID);
        }
    }
    
    // Then, add cameras from scoreMap that pass hysteresis threshold
    for (const auto& [score, camID] : scoreMap) {
        if (selected.size() >= static_cast<size_t>(m_topK)) {
            break;
        }
        
        // Skip if already in selected list
        if (std::find(selected.begin(), selected.end(), camID) != selected.end()) {
            continue;
        }
        
        // Check if replacing an active camera
        bool replacingActive = false;
        int lowestActiveIdx = -1;
        float lowestActiveScore = std::numeric_limits<float>::max();
        
        for (int activeID : currentlyActive) {
            // Skip if already selected (protected by min_active_frames)
            if (std::find(selected.begin(), selected.end(), activeID) != selected.end()) {
                continue;
            }
            
            float activeScore = m_cameraStates[activeID].metrics.decayedScore;
            if (activeScore < lowestActiveScore) {
                lowestActiveScore = activeScore;
                lowestActiveIdx = activeID;
            }
        }
        
        if (lowestActiveIdx >= 0) {
            // Only replace if new score is significantly higher (hysteresis threshold)
            float requiredScore = lowestActiveScore * (1.0f + m_hysteresisConfig.switch_threshold);
            if (score > requiredScore) {
                // Replace the lowest scoring active camera
                selected.push_back(camID);
                replacingActive = true;
            } else {
                // Keep the current active camera
                selected.push_back(lowestActiveIdx);
            }
        } else {
            // No active camera to replace, just add
            selected.push_back(camID);
        }
    }
    
    // Update active status
    for (int i = 0; i < m_numCameras; i++) {
        bool nowActive = std::find(selected.begin(), selected.end(), i) != selected.end();
        m_cameraStates[i].metrics.isActive = nowActive;
        if (!nowActive) {
            m_cameraStates[i].metrics.consecutiveActiveFrames = 0;
        }
    }
    
    // Warmup logging: Track progress until min_active_frames is reached
    if (!m_isStable) {
        int currentWarmup = m_warmupFrameCount.load();
        if (currentWarmup < m_hysteresisConfig.min_active_frames) {
            currentWarmup++;
            m_warmupFrameCount.store(currentWarmup);
            Logger::Info("Selector warming up... (" + std::to_string(currentWarmup) + 
                        "/" + std::to_string(m_hysteresisConfig.min_active_frames) + " frames)");
            
            if (currentWarmup >= m_hysteresisConfig.min_active_frames) {
                m_isStable.store(true);
                Logger::Info("Selector stable (" + std::to_string(m_hysteresisConfig.min_active_frames) + 
                            "/" + std::to_string(m_hysteresisConfig.min_active_frames) + " frames)");
            }
        }
    }
    
    return selected;
}

std::vector<int> ActiveCameraSelector::DetermineNeighborHandover(const std::vector<int>& selected) {
    std::vector<int> preActivated;
    
    // For each selected camera, check if objects are near edges
    for (int cameraID : selected) {
        if (m_cameraStates[cameraID].metrics.edgeActivity > m_motionThreshold * 2.0f) {
            // High edge activity - pre-activate neighbors
            // Simple neighbor logic: adjacent camera IDs
            int leftNeighbor = (cameraID > 0) ? cameraID - 1 : m_numCameras - 1;
            int rightNeighbor = (cameraID < m_numCameras - 1) ? cameraID + 1 : 0;
            
            // Add neighbors if not already selected
            if (std::find(selected.begin(), selected.end(), leftNeighbor) == selected.end()) {
                if (std::find(preActivated.begin(), preActivated.end(), leftNeighbor) == preActivated.end()) {
                    preActivated.push_back(leftNeighbor);
                }
            }
            if (std::find(selected.begin(), selected.end(), rightNeighbor) == selected.end()) {
                if (std::find(preActivated.begin(), preActivated.end(), rightNeighbor) == preActivated.end()) {
                    preActivated.push_back(rightNeighbor);
                }
            }
        }
    }
    
    return preActivated;
}

CameraSelectionResult ActiveCameraSelector::GetActiveSelection() {
    std::lock_guard<std::mutex> lock(m_selectionMutex);
    
    // Select top K cameras with hysteresis
    std::vector<int> selected = SelectTopK();
    
    // Note: Edge handover disabled for track cameras (no camera overlap)
    // Track cameras have distinct coverage areas without spatial overlap
    std::vector<int> preActivated;  // Empty - handover not applicable
    
    // Calculate average motion score
    float avgScore = 0.0f;
    if (!selected.empty()) {
        for (int cameraID : selected) {
            avgScore += m_cameraStates[cameraID].metrics.motionScore;
        }
        avgScore /= selected.size();
    }
    
    // Build result
    CameraSelectionResult result;
    result.selectedCameraIDs = selected;
    result.preActivatedIDs = preActivated;  // Empty
    result.averageMotionScore = avgScore;
    result.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    
    m_lastSelection = result;
    
    return result;
}

CameraMotionMetrics ActiveCameraSelector::GetCameraMetrics(int cameraID) const {
    if (cameraID < 0 || cameraID >= m_numCameras) {
        CameraMotionMetrics empty = {cameraID, 0.0f, 0.0f, false, 0};
        return empty;
    }
    
    return m_cameraStates[cameraID].metrics;
}

std::vector<CameraMotionMetrics> ActiveCameraSelector::GetAllMetrics() const {
    std::vector<CameraMotionMetrics> allMetrics;
    for (const auto& state : m_cameraStates) {
        allMetrics.push_back(state.metrics);
    }
    return allMetrics;
}

void ActiveCameraSelector::Reset() {
    Logger::Info("Resetting ActiveCameraSelector");
    
    for (auto& state : m_cameraStates) {
        std::lock_guard<std::recursive_mutex> lock(state.frameMutex);
        state.hasHistory = false;
        state.metrics.motionScore = 0.0f;
        state.metrics.edgeActivity = 0.0f;
        state.metrics.isActive = false;
        state.metrics.frameCount = 0;
        state.metrics.consecutiveActiveFrames = 0;
        state.metrics.decayedScore = 0.0f;
    }
    
    std::lock_guard<std::mutex> lock(m_selectionMutex);
    m_lastSelection = CameraSelectionResult();
}
