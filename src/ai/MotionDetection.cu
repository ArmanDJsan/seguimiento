/**
 * MotionDetection.cu
 * 
 * CUDA kernels for high-performance motion detection
 * Computes frame difference and motion scores in <0.5ms
 */
#ifndef _HAS_STD_BYTE
#define _HAS_STD_BYTE 0
#endif
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cmath>

/**
 * Kernel: Compute absolute difference between consecutive frames (UYVY format)
 * 
 * @param prevFrame Previous frame buffer (UYVY)
 * @param currFrame Current frame buffer (UYVY)
 * @param motionMap Output motion intensity map (grayscale)
 * @param width Frame width in pixels
 * @param height Frame height
 */
__global__ void ComputeFrameDifference(
    const unsigned char* prevFrame,
    const unsigned char* currFrame,
    float* motionMap,
    unsigned int width,
    unsigned int height)
{
    const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= width || y >= height) return;
    
    const unsigned int pixelIdx = y * width + x;
    
    // UYVY format: 2 bytes per pixel (Y, UV shared)
    // For motion, we only care about luminance (Y)
    const unsigned int byteIdx = pixelIdx * 2;
    
    // Extract Y component (luminance)
    // UYVY: U0 Y0 V0 Y1 U2 Y2 V2 Y3...
    // Y is at odd positions for pixel 0, 2, 4... and even positions for pixel 1, 3, 5...
    unsigned char y_prev, y_curr;
    
    if (x % 2 == 0) {
        // Even pixel: Y is at byteIdx + 1
        y_prev = prevFrame[byteIdx + 1];
        y_curr = currFrame[byteIdx + 1];
    } else {
        // Odd pixel: Y is at byteIdx + 3 (or byteIdx - 1 from next pair)
        y_prev = prevFrame[byteIdx - 1];
        y_curr = currFrame[byteIdx - 1];
    }
    
    // Compute absolute difference
    int diff = abs(static_cast<int>(y_curr) - static_cast<int>(y_prev));
    
    // Normalize to [0, 1]
    motionMap[pixelIdx] = static_cast<float>(diff) / 255.0f;
}

/**
 * Kernel: Reduce motion map to single score using parallel reduction
 * 
 * @param motionMap Input motion intensity map
 * @param partialSums Output partial sums (one per block)
 * @param numElements Total number of pixels
 */
__global__ void ReduceMotionScore(
    const float* motionMap,
    float* partialSums,
    unsigned int numElements)
{
    extern __shared__ float sharedMem[];
    
    unsigned int tid = threadIdx.x;
    unsigned int idx = blockIdx.x * blockDim.x * 2 + threadIdx.x;
    
    // Load two elements per thread and sum them
    float sum = 0.0f;
    if (idx < numElements) {
        sum += motionMap[idx];
    }
    if (idx + blockDim.x < numElements) {
        sum += motionMap[idx + blockDim.x];
    }
    
    sharedMem[tid] = sum;
    __syncthreads();
    
    // Parallel reduction in shared memory
    for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            sharedMem[tid] += sharedMem[tid + s];
        }
        __syncthreads();
    }
    
    // Write result for this block to global memory
    if (tid == 0) {
        partialSums[blockIdx.x] = sharedMem[0];
    }
}

/**
 * Kernel: Calculate edge activity (motion near frame borders)
 * 
 * @param motionMap Motion intensity map
 * @param edgeScore Output edge activity score
 * @param width Frame width
 * @param height Frame height
 * @param edgeMargin Percentage of frame considered edge (e.g., 0.1 = 10%)
 */
__global__ void CalculateEdgeActivity(
    const float* motionMap,
    float* edgeScore,
    unsigned int width,
    unsigned int height,
    float edgeMargin)
{
    extern __shared__ float sharedEdgeSum[];
    
    unsigned int tid = threadIdx.x;
    unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned int totalPixels = width * height;
    
    if (idx >= totalPixels) return;
    
    // Calculate pixel position
    unsigned int x = idx % width;
    unsigned int y = idx / width;
    
    // Determine if pixel is in edge region
    unsigned int edgeWidth = static_cast<unsigned int>(width * edgeMargin);
    unsigned int edgeHeight = static_cast<unsigned int>(height * edgeMargin);
    
    bool inEdge = (x < edgeWidth) || (x >= width - edgeWidth) ||
                  (y < edgeHeight) || (y >= height - edgeHeight);
    
    // Sum motion only in edge regions
    float contribution = inEdge ? motionMap[idx] : 0.0f;
    
    sharedEdgeSum[tid] = contribution;
    __syncthreads();
    
    // Parallel reduction
    for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            sharedEdgeSum[tid] += sharedEdgeSum[tid + s];
        }
        __syncthreads();
    }
    
    if (tid == 0) {
        atomicAdd(edgeScore, sharedEdgeSum[0]);
    }
}

// Host-side wrapper functions
extern "C" {

/**
 * Launch motion detection kernels
 * 
 * @param prevFrame Previous frame (CUDA device memory)
 * @param currFrame Current frame (CUDA device memory)
 * @param motionMap Motion map output (CUDA device memory)
 * @param partialSums Partial reduction sums (CUDA device memory)
 * @param width Frame width
 * @param height Frame height
 * @param stream CUDA stream for async execution
 */
cudaError_t LaunchMotionDetection(
    const void* prevFrame,
    const void* currFrame,
    float* motionMap,
    float* partialSums,
    unsigned int width,
    unsigned int height,
    cudaStream_t stream)
{
    // Configure kernel launch
    dim3 blockDim(16, 16);
    dim3 gridDim(
        (width + blockDim.x - 1) / blockDim.x,
        (height + blockDim.y - 1) / blockDim.y
    );
    
    // Launch frame difference kernel
    ComputeFrameDifference<<<gridDim, blockDim, 0, stream>>>(
        static_cast<const unsigned char*>(prevFrame),
        static_cast<const unsigned char*>(currFrame),
        motionMap,
        width,
        height
    );
    
    // Check for errors
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        return err;
    }
    
    // Configure reduction kernel
    unsigned int numPixels = width * height;
    unsigned int threadsPerBlock = 256;
    unsigned int numBlocks = (numPixels + threadsPerBlock * 2 - 1) / (threadsPerBlock * 2);
    
    // Launch reduction kernel
    ReduceMotionScore<<<numBlocks, threadsPerBlock, threadsPerBlock * sizeof(float), stream>>>(
        motionMap,
        partialSums,
        numPixels
    );
    
    return cudaGetLastError();
}

/**
 * Launch edge activity calculation
 * 
 * @param motionMap Motion intensity map (CUDA device memory)
 * @param edgeScore Output edge score (CUDA device memory, initialized to 0)
 * @param width Frame width
 * @param height Frame height
 * @param edgeMargin Edge margin percentage
 * @param stream CUDA stream
 */
cudaError_t LaunchEdgeActivityCalculation(
    const float* motionMap,
    float* edgeScore,
    unsigned int width,
    unsigned int height,
    float edgeMargin,
    cudaStream_t stream)
{
    unsigned int numPixels = width * height;
    unsigned int threadsPerBlock = 256;
    unsigned int numBlocks = (numPixels + threadsPerBlock - 1) / threadsPerBlock;
    
    // Initialize edge score to 0
    cudaMemsetAsync(edgeScore, 0, sizeof(float), stream);
    
    // Launch edge activity kernel
    CalculateEdgeActivity<<<numBlocks, threadsPerBlock, threadsPerBlock * sizeof(float), stream>>>(
        motionMap,
        edgeScore,
        width,
        height,
        edgeMargin
    );
    
    return cudaGetLastError();
}

} // extern "C"
