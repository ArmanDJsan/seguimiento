/**
 * AtlasCompositor.cu
 * 
 * CUDA kernels for 4K Mega-Canvas composition (TEST)
 * 
 * Key Operations:
 * 1. BlitToAtlas: Copy HD frame to Atlas position (1:1 native) - TEST: reduced from 4K
 * 2. DownscaleForTensorRT: Bilinear downscale to 640x640
 * 3. BifurcatedCopy: Combined blit + downscale in single kernel launch
 * 
 * Performance Targets:
 * - Blit 4K to Atlas: <0.5ms
 * - Downscale 4K → 640x640: <0.2ms
 * - Combined bifurcation: <0.6ms total per camera
 */

// Workaround for conflict between CUDA headers and C++17 std::byte
// CUDA defines its own byte type which conflicts with std::byte
#define _HAS_STD_BYTE 0
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <device_launch_parameters.h>

// Include guard for C++ linkage
extern "C" {

/**
 * Camera information structure for batch composition
 */
struct CameraInfo {
    void* sourceBuffer;         // Source BGRA buffer (4K)
    int sourceWidth;            // Source width (3840)
    int sourceHeight;           // Source height (2160)
    int sourcePitch;            // Source pitch in bytes
    
    int destX;                  // Destination X in Atlas
    int destY;                  // Destination Y in Atlas
    
    bool hasNewFrame;           // Flag: frame available
};

/**
 * Kernel: Blit single camera to Atlas position
 * Simple 1:1 copy with coalesced memory access
 */
__global__ void BlitToAtlasKernel(
    const uchar4* __restrict__ source,
    uchar4* __restrict__ atlas,
    int sourceWidth,
    int sourceHeight,
    int sourcePitch,           // In uchar4 elements
    int atlasWidth,
    int destX,
    int destY
) {
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= sourceWidth || y >= sourceHeight) return;
    
    // Read from source
    const uchar4 pixel = source[y * sourcePitch + x];
    
    // Write to atlas at offset position
    const int atlasX = destX + x;
    const int atlasY = destY + y;
    atlas[atlasY * atlasWidth + atlasX] = pixel;
}

/**
 * Kernel: Bilinear downscale to 640x640 for TensorRT
 * Uses texture sampling for quality
 */
__global__ void DownscaleKernel(
    const uchar4* __restrict__ source,
    uchar4* __restrict__ dest,
    int sourceWidth,
    int sourceHeight,
    int sourcePitch,           // In uchar4 elements
    int destWidth,             // 640
    int destHeight             // 640
) {
    const int destX = blockIdx.x * blockDim.x + threadIdx.x;
    const int destY = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (destX >= destWidth || destY >= destHeight) return;
    
    // Calculate source coordinates (floating point for bilinear)
    const float scaleX = static_cast<float>(sourceWidth) / destWidth;
    const float scaleY = static_cast<float>(sourceHeight) / destHeight;
    
    const float srcX = (destX + 0.5f) * scaleX - 0.5f;
    const float srcY = (destY + 0.5f) * scaleY - 0.5f;
    
    // Bilinear interpolation
    const int x0 = max(0, min(sourceWidth - 1, static_cast<int>(floorf(srcX))));
    const int y0 = max(0, min(sourceHeight - 1, static_cast<int>(floorf(srcY))));
    const int x1 = max(0, min(sourceWidth - 1, x0 + 1));
    const int y1 = max(0, min(sourceHeight - 1, y0 + 1));
    
    const float fx = srcX - floorf(srcX);
    const float fy = srcY - floorf(srcY);
    
    // Sample 4 pixels
    const uchar4 p00 = source[y0 * sourcePitch + x0];
    const uchar4 p10 = source[y0 * sourcePitch + x1];
    const uchar4 p01 = source[y1 * sourcePitch + x0];
    const uchar4 p11 = source[y1 * sourcePitch + x1];
    
    // Interpolate
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;
    
    uchar4 result;
    result.x = static_cast<unsigned char>(p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11);
    result.y = static_cast<unsigned char>(p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11);
    result.z = static_cast<unsigned char>(p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11);
    result.w = 255;  // Alpha always opaque
    
    dest[destY * destWidth + destX] = result;
}

/**
 * Kernel: Combined blit + downscale (VRAM Bifurcation)
 * Single kernel launch performs both operations
 * 
 * Grid organization:
 * - Blocks 0 to N-1: Blit to atlas
 * - Blocks N to 2N-1: Downscale for TensorRT
 */
__global__ void BifurcatedCopyKernel(
    const uchar4* __restrict__ source,
    uchar4* __restrict__ atlas,
    uchar4* __restrict__ tensorRT,
    int sourceWidth,           // 1920 - TEST: reduced from 3840
    int sourceHeight,          // 1080 - TEST: reduced from 2160
    int sourcePitch,           // In uchar4 elements
    int atlasWidth,            // 3840 - TEST: reduced from 15360
    int destX,                 // Atlas offset X
    int destY,                 // Atlas offset Y
    int tensorRTWidth,         // 640
    int tensorRTHeight,        // 640
    int blitBlocksY            // Number of Y blocks for blit
) {
    // Determine if this block handles blit or downscale
    const bool isBlit = (blockIdx.y < blitBlocksY);
    
    if (isBlit) {
        // Blit path: 1:1 copy to atlas
        const int x = blockIdx.x * blockDim.x + threadIdx.x;
        const int y = blockIdx.y * blockDim.y + threadIdx.y;
        
        if (x >= sourceWidth || y >= sourceHeight) return;
        
        const uchar4 pixel = source[y * sourcePitch + x];
        const int atlasX = destX + x;
        const int atlasY = destY + y;
        atlas[atlasY * atlasWidth + atlasX] = pixel;
    } else {
        // Downscale path: bilinear to 640x640
        const int localBlockY = blockIdx.y - blitBlocksY;
        const int destXPos = blockIdx.x * blockDim.x + threadIdx.x;
        const int destYPos = localBlockY * blockDim.y + threadIdx.y;
        
        if (destXPos >= tensorRTWidth || destYPos >= tensorRTHeight) return;
        
        // Calculate source coordinates
        const float scaleX = static_cast<float>(sourceWidth) / tensorRTWidth;
        const float scaleY = static_cast<float>(sourceHeight) / tensorRTHeight;
        
        const float srcX = (destXPos + 0.5f) * scaleX - 0.5f;
        const float srcY = (destYPos + 0.5f) * scaleY - 0.5f;
        
        // Bilinear interpolation
        const int x0 = max(0, min(sourceWidth - 1, static_cast<int>(floorf(srcX))));
        const int y0 = max(0, min(sourceHeight - 1, static_cast<int>(floorf(srcY))));
        const int x1 = max(0, min(sourceWidth - 1, x0 + 1));
        const int y1 = max(0, min(sourceHeight - 1, y0 + 1));
        
        const float fx = srcX - floorf(srcX);
        const float fy = srcY - floorf(srcY);
        
        const uchar4 p00 = source[y0 * sourcePitch + x0];
        const uchar4 p10 = source[y0 * sourcePitch + x1];
        const uchar4 p01 = source[y1 * sourcePitch + x0];
        const uchar4 p11 = source[y1 * sourcePitch + x1];
        
        const float w00 = (1.0f - fx) * (1.0f - fy);
        const float w10 = fx * (1.0f - fy);
        const float w01 = (1.0f - fx) * fy;
        const float w11 = fx * fy;
        
        uchar4 result;
        result.x = static_cast<unsigned char>(p00.x * w00 + p10.x * w10 + p01.x * w01 + p11.x * w11);
        result.y = static_cast<unsigned char>(p00.y * w00 + p10.y * w10 + p01.y * w01 + p11.y * w11);
        result.z = static_cast<unsigned char>(p00.z * w00 + p10.z * w10 + p01.z * w01 + p11.z * w11);
        result.w = 255;
        
        tensorRT[destYPos * tensorRTWidth + destXPos] = result;
    }
}

/**
 * Host function: Blit camera frame to Atlas
 */
bool BlitCameraToAtlas(
    const void* sourceBuffer,
    void* atlasBuffer,
    int sourceWidth,
    int sourceHeight,
    int atlasWidth,
    int destX,
    int destY,
    cudaStream_t stream
) {
    if (!sourceBuffer || !atlasBuffer) return false;
    
    // Block size: 16x16 is optimal for 2D image processing
    dim3 blockSize(16, 16);
    dim3 gridSize(
        (sourceWidth + blockSize.x - 1) / blockSize.x,
        (sourceHeight + blockSize.y - 1) / blockSize.y
    );
    
    BlitToAtlasKernel<<<gridSize, blockSize, 0, stream>>>(
        static_cast<const uchar4*>(sourceBuffer),
        static_cast<uchar4*>(atlasBuffer),
        sourceWidth,
        sourceHeight,
        sourceWidth,  // Assuming contiguous (pitch = width)
        atlasWidth,
        destX,
        destY
    );
    
    return cudaGetLastError() == cudaSuccess;
}

/**
 * Host function: Downscale camera frame for TensorRT
 */
bool DownscaleForTensorRT(
    const void* sourceBuffer,
    void* tensorRTBuffer,
    int sourceWidth,
    int sourceHeight,
    int destWidth,
    int destHeight,
    cudaStream_t stream
) {
    if (!sourceBuffer || !tensorRTBuffer) return false;
    
    dim3 blockSize(16, 16);
    dim3 gridSize(
        (destWidth + blockSize.x - 1) / blockSize.x,
        (destHeight + blockSize.y - 1) / blockSize.y
    );
    
    DownscaleKernel<<<gridSize, blockSize, 0, stream>>>(
        static_cast<const uchar4*>(sourceBuffer),
        static_cast<uchar4*>(tensorRTBuffer),
        sourceWidth,
        sourceHeight,
        sourceWidth,
        destWidth,
        destHeight
    );
    
    return cudaGetLastError() == cudaSuccess;
}

/**
 * Host function: VRAM Bifurcation - simultaneous blit + downscale
 * Single kernel launch performs both operations concurrently
 */
bool BifurcatedCopy(
    const void* sourceBuffer,
    void* atlasBuffer,
    void* tensorRTBuffer,
    int sourceWidth,           // 1920 - TEST: reduced from 3840
    int sourceHeight,          // 1080 - TEST: reduced from 2160
    int atlasWidth,            // 3840 - TEST: reduced from 15360
    int destX,                 // Atlas offset X
    int destY,                 // Atlas offset Y
    int tensorRTWidth,         // 640
    int tensorRTHeight,        // 640
    cudaStream_t stream
) {
    if (!sourceBuffer || !atlasBuffer || !tensorRTBuffer) return false;
    
    dim3 blockSize(16, 16);
    
    // Calculate grid for blit (4K)
    const int blitGridX = (sourceWidth + blockSize.x - 1) / blockSize.x;
    const int blitGridY = (sourceHeight + blockSize.y - 1) / blockSize.y;
    
    // Calculate grid for downscale (640x640)
    const int downscaleGridX = (tensorRTWidth + blockSize.x - 1) / blockSize.x;
    const int downscaleGridY = (tensorRTHeight + blockSize.y - 1) / blockSize.y;
    
    // Combined grid: use max X, sum Y
    dim3 gridSize(
        max(blitGridX, downscaleGridX),
        blitGridY + downscaleGridY
    );
    
    BifurcatedCopyKernel<<<gridSize, blockSize, 0, stream>>>(
        static_cast<const uchar4*>(sourceBuffer),
        static_cast<uchar4*>(atlasBuffer),
        static_cast<uchar4*>(tensorRTBuffer),
        sourceWidth,
        sourceHeight,
        sourceWidth,
        atlasWidth,
        destX,
        destY,
        tensorRTWidth,
        tensorRTHeight,
        blitGridY
    );
    
    return cudaGetLastError() == cudaSuccess;
}

/**
 * Host function: Clear Atlas to black
 */
bool ClearAtlas(
    void* atlasBuffer,
    int width,
    int height,
    cudaStream_t stream
) {
    if (!atlasBuffer) return false;
    
    size_t size = static_cast<size_t>(width) * height * sizeof(uchar4);
    cudaError_t err = cudaMemsetAsync(atlasBuffer, 0, size, stream);
    
    return err == cudaSuccess;
}

/**
 * Host function: Copy Atlas to D3D11 texture array (for CUDA/DX interop)
 */
bool CopyAtlasToArray(
    const void* atlasBuffer,
    cudaArray_t destArray,
    int width,
    int height,
    cudaStream_t stream
) {
    if (!atlasBuffer || !destArray) return false;
    
    cudaError_t err = cudaMemcpy2DToArrayAsync(
        destArray,
        0, 0,
        atlasBuffer,
        width * sizeof(uchar4),  // Source pitch
        width * sizeof(uchar4),  // Width in bytes
        height,
        cudaMemcpyDeviceToDevice,
        stream
    );
    
    return err == cudaSuccess;
}

} // extern "C"
