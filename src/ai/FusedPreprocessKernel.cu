/**
 * FusedPreprocessKernel.cu
 * 
 * Fused CUDA kernel for ultra-low-latency preprocessing
 * Combines UYVY→RGB conversion + bilinear resize (optional) + normalization in ONE kernel
 * 
 * For 1080p native inference (skip_resize=true):
 * - Input: UYVY 4:2:2 (1920×1080) from PTZ cameras via DeckLink
 * - Output: RGB float NCHW (1920×1080) for TensorRT - NO resize
 * 
 * For resized inference (skip_resize=false, legacy):
 * - Input: UYVY 4:2:2 (any resolution) from DeckLink
 * - Output: RGB float NCHW (configurable resolution) for TensorRT
 *   - Supports square formats (e.g., 640×640)
 *   - Supports 16:9 formats (e.g., 1280×720) to match training aspect ratio
 * 
 * Performance optimization for RTX 5080 with Threadripper PRO
 * Expected latency: <0.5ms for 1080p (vs. 1.5ms for two-pass 4K approach)
 */

#ifndef _HAS_STD_BYTE
#define _HAS_STD_BYTE 0
#endif
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cuda_runtime_api.h>
#include <cmath>

// UYVY format constants
#define UYVY_BYTES_PER_MACROPIXEL 4    // 4 bytes contain 2 pixels: U Y0 V Y1
#define UYVY_PIXELS_PER_MACROPIXEL 2   // 2 pixels share U and V values
#define UYVY_BYTES_PER_PIXEL 2         // Average bytes per pixel in UYVY

// Byte offsets within a macropixel [U Y0 V Y1]
#define UYVY_U_OFFSET 0    // U component at byte 0
#define UYVY_Y0_OFFSET 1   // First Y (even pixel) at byte 1
#define UYVY_V_OFFSET 2    // V component at byte 2
#define UYVY_Y1_OFFSET 3   // Second Y (odd pixel) at byte 3

/**
 * Device function: Convert single UYVY pixel to RGB
 * 
 * @param srcUYVY Source UYVY buffer
 * @param srcWidth Source width
 * @param px Pixel x coordinate
 * @param py Pixel y coordinate
 * @param ro Output red [0, 255]
 * @param go Output green [0, 255]
 * @param bo Output blue [0, 255]
 */
__device__ __forceinline__ void convertUYVYPixelToRGB(
    const unsigned char* __restrict__ srcUYVY,
    int srcWidth,
    int px, int py,
    float& ro, float& go, float& bo)
{
    // Calculate macropixel index and position within macropixel
    int macropixelIdx = px / UYVY_PIXELS_PER_MACROPIXEL;
    int pixelInMacro = px % UYVY_PIXELS_PER_MACROPIXEL;
    
    // Calculate byte offset
    int srcByteOffset = (py * srcWidth + macropixelIdx * UYVY_PIXELS_PER_MACROPIXEL) * UYVY_BYTES_PER_PIXEL;
    
    // Extract YUV components
    unsigned char u = srcUYVY[srcByteOffset + UYVY_U_OFFSET];
    unsigned char v = srcUYVY[srcByteOffset + UYVY_V_OFFSET];
    unsigned char y_val = (pixelInMacro == 0) 
        ? srcUYVY[srcByteOffset + UYVY_Y0_OFFSET]
        : srcUYVY[srcByteOffset + UYVY_Y1_OFFSET];
    
    // YUV to RGB conversion (BT.709)
    float yf = static_cast<float>(y_val);
    float uf = static_cast<float>(u) - 128.0f;
    float vf = static_cast<float>(v) - 128.0f;
    
    ro = yf + 1.5748f * vf;
    go = yf - 0.1873f * uf - 0.4681f * vf;
    bo = yf + 1.8556f * uf;
    
    // Clamp to [0, 255]
    ro = fmaxf(0.0f, fminf(255.0f, ro));
    go = fmaxf(0.0f, fminf(255.0f, go));
    bo = fmaxf(0.0f, fminf(255.0f, bo));
}

/**
 * Device function: Sample UYVY pixel with bilinear interpolation
 * 
 * @param srcUYVY Source UYVY buffer
 * @param srcWidth Source width
 * @param srcHeight Source height
 * @param x Floating-point x coordinate
 * @param y Floating-point y coordinate
 * @param r Output red channel [0, 1]
 * @param g Output green channel [0, 1]
 * @param b Output blue channel [0, 1]
 */
__device__ __forceinline__ void sampleUYVYBilinear(
    const unsigned char* __restrict__ srcUYVY,
    int srcWidth, int srcHeight,
    float x, float y,
    float& r, float& g, float& b)
{
    // Clamp coordinates
    x = fmaxf(0.0f, fminf(x, static_cast<float>(srcWidth - 1)));
    y = fmaxf(0.0f, fminf(y, static_cast<float>(srcHeight - 1)));
    
    // Get integer coordinates for bilinear sampling
    int x0 = static_cast<int>(x);
    int y0 = static_cast<int>(y);
    int x1 = min(x0 + 1, srcWidth - 1);
    int y1 = min(y0 + 1, srcHeight - 1);
    
    float dx = x - x0;
    float dy = y - y0;
    
    // Sample 4 corners and convert YUV→RGB for each
    float r00, g00, b00, r01, g01, b01, r10, g10, b10, r11, g11, b11;
    
    // Convert 4 corner pixels using helper function
    convertUYVYPixelToRGB(srcUYVY, srcWidth, x0, y0, r00, g00, b00);
    convertUYVYPixelToRGB(srcUYVY, srcWidth, x1, y0, r01, g01, b01);
    convertUYVYPixelToRGB(srcUYVY, srcWidth, x0, y1, r10, g10, b10);
    convertUYVYPixelToRGB(srcUYVY, srcWidth, x1, y1, r11, g11, b11);
    
    // Bilinear interpolation
    float r0 = r00 * (1.0f - dx) + r01 * dx;
    float r1 = r10 * (1.0f - dx) + r11 * dx;
    r = (r0 * (1.0f - dy) + r1 * dy) / 255.0f;  // Normalize to [0, 1]
    
    float g0 = g00 * (1.0f - dx) + g01 * dx;
    float g1 = g10 * (1.0f - dx) + g11 * dx;
    g = (g0 * (1.0f - dy) + g1 * dy) / 255.0f;
    
    float b0 = b00 * (1.0f - dx) + b01 * dx;
    float b1 = b10 * (1.0f - dx) + b11 * dx;
    b = (b0 * (1.0f - dy) + b1 * dy) / 255.0f;
}

/**
 * Fused kernel: UYVY 4K → RGB in one pass
 * 
 * This kernel eliminates the intermediate BGRA 4K buffer by directly:
 * 1. Reading UYVY 4:2:2 format
 * 2. Converting YUV→RGB with BT.709
 * 3. Bilinear resizing to target dimensions
 * 4. Normalizing to [0, 1]
 * 5. Writing to NCHW format for TensorRT
 * 
 * @param srcUYVY Source UYVY buffer (3840×2160)
 * @param dstRGB Destination RGB buffer (float, NCHW)
 * @param srcWidth Source width (3840)
 * @param srcHeight Source height (2160)
 * @param dstWidth Destination width (e.g., 640 or 1280)
 * @param dstHeight Destination height (e.g., 640 or 720)
 * @param batchIdx Batch index for output offset
 */
__global__ void FusedUYVYToRGB640Kernel(
    const unsigned char* __restrict__ srcUYVY,
    float* __restrict__ dstRGB,
    int srcWidth, int srcHeight,
    int dstWidth, int dstHeight,
    int batchIdx)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= dstWidth || y >= dstHeight) return;
    
    // Calculate source coordinates with bilinear sampling
    float scaleX = static_cast<float>(srcWidth) / dstWidth;
    float scaleY = static_cast<float>(srcHeight) / dstHeight;
    
    float srcX = (x + 0.5f) * scaleX - 0.5f;
    float srcY = (y + 0.5f) * scaleY - 0.5f;
    
    // Sample with bilinear interpolation and YUV→RGB conversion
    float r, g, b;
    sampleUYVYBilinear(srcUYVY, srcWidth, srcHeight, srcX, srcY, r, g, b);
    
    // Write to NCHW format: [batch, channel, height, width]
    int planeSize = dstWidth * dstHeight;
    int batchOffset = batchIdx * 3 * planeSize;
    int pixelIdx = y * dstWidth + x;
    
    dstRGB[batchOffset + 0 * planeSize + pixelIdx] = r;  // R channel
    dstRGB[batchOffset + 1 * planeSize + pixelIdx] = g;  // G channel
    dstRGB[batchOffset + 2 * planeSize + pixelIdx] = b;  // B channel
}

/**
 * Optimized version with shared memory (future enhancement)
 * Uses tile-based processing to maximize cache hits
 */
__global__ void FusedUYVYToRGB640KernelShared(
    const unsigned char* __restrict__ srcUYVY,
    float* __restrict__ dstRGB,
    int srcWidth, int srcHeight,
    int dstWidth, int dstHeight,
    int batchIdx)
{
    // Shared memory tile for source data (reserved for future optimization)
    // Suppress NVCC warning #550-D for intentionally unused variable (placeholder for future shared memory optimization)
#pragma nv_diag_suppress 550
    __shared__ unsigned char tile[32 * 32 * 2];  // 2 bytes per pixel (UYVY)
#pragma nv_diag_default 550
    // Prevent unused variable warning
    (void)tile;
    
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= dstWidth || y >= dstHeight) return;
    
    // Calculate source coordinates
    float scaleX = static_cast<float>(srcWidth) / dstWidth;
    float scaleY = static_cast<float>(srcHeight) / dstHeight;
    
    float srcX = (x + 0.5f) * scaleX - 0.5f;
    float srcY = (y + 0.5f) * scaleY - 0.5f;
    
    // TODO: Load tile into shared memory for better cache performance
    // For now, use direct sampling
    float r, g, b;
    sampleUYVYBilinear(srcUYVY, srcWidth, srcHeight, srcX, srcY, r, g, b);
    
    // Write to NCHW format
    int planeSize = dstWidth * dstHeight;
    int batchOffset = batchIdx * 3 * planeSize;
    int pixelIdx = y * dstWidth + x;
    
    dstRGB[batchOffset + 0 * planeSize + pixelIdx] = r;
    dstRGB[batchOffset + 1 * planeSize + pixelIdx] = g;
    dstRGB[batchOffset + 2 * planeSize + pixelIdx] = b;
}

// =============================================================================
// Host-side wrapper functions
// =============================================================================

extern "C" {

/**
 * Launch fused UYVY→RGB preprocessing kernel
 * 
 * This is the main entry point for the fused kernel.
 * Eliminates intermediate BGRA buffer for reduced latency.
 * Supports both square and 16:9 aspect ratio outputs.
 */
cudaError_t LaunchFusedUYVYPreprocess(
    const void* srcUYVY,
    float* dstRGB,
    int srcWidth, int srcHeight,
    int dstWidth, int dstHeight,
    int batchIdx,
    cudaStream_t stream)
{
    // Use 16×16 thread blocks for optimal occupancy on RTX 5080
    dim3 blockDim(16, 16);
    dim3 gridDim(
        (dstWidth + blockDim.x - 1) / blockDim.x,
        (dstHeight + blockDim.y - 1) / blockDim.y
    );
    
    FusedUYVYToRGB640Kernel<<<gridDim, blockDim, 0, stream>>>(
        static_cast<const unsigned char*>(srcUYVY),
        dstRGB,
        srcWidth, srcHeight,
        dstWidth, dstHeight,
        batchIdx
    );
    
    return cudaGetLastError();
}

/**
 * Launch optimized version with shared memory (future use)
 */
cudaError_t LaunchFusedUYVYPreprocessShared(
    const void* srcUYVY,
    float* dstRGB,
    int srcWidth, int srcHeight,
    int dstWidth, int dstHeight,
    int batchIdx,
    cudaStream_t stream)
{
    dim3 blockDim(16, 16);
    dim3 gridDim(
        (dstWidth + blockDim.x - 1) / blockDim.x,
        (dstHeight + blockDim.y - 1) / blockDim.y
    );
    
    // Calculate shared memory size
    size_t sharedMemSize = 32 * 32 * 2;  // Bytes for tile
    
    FusedUYVYToRGB640KernelShared<<<gridDim, blockDim, sharedMemSize, stream>>>(
        static_cast<const unsigned char*>(srcUYVY),
        dstRGB,
        srcWidth, srcHeight,
        dstWidth, dstHeight,
        batchIdx
    );
    
    return cudaGetLastError();
}

} // extern "C"
