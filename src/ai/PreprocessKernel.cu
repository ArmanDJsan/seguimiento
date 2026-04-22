/**
 * PreprocessKernel.cu
 * 
 * CUDA kernel for preprocessing frames for TensorRT inference
 * Performs: Resize + Color conversion (BGRA→RGB) + Normalization
 * 
 * Optimized for RTX 5080 with compute_100,sm_100
 */

#ifndef _HAS_STD_BYTE
#define _HAS_STD_BYTE 0
#endif
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cuda_runtime_api.h>
#include <cmath>

/**
 * Bilinear interpolation helper
 */
__device__ __forceinline__ float bilinearInterpolate(
    const unsigned char* img,
    int width, int height, int channels,
    float x, float y, int c)
{
    // Clamp coordinates
    x = fmaxf(0.0f, fminf(x, static_cast<float>(width - 1)));
    y = fmaxf(0.0f, fminf(y, static_cast<float>(height - 1)));
    
    int x0 = static_cast<int>(x);
    int y0 = static_cast<int>(y);
    int x1 = min(x0 + 1, width - 1);
    int y1 = min(y0 + 1, height - 1);
    
    float dx = x - x0;
    float dy = y - y0;
    
    // Get pixel values
    float v00 = static_cast<float>(img[(y0 * width + x0) * channels + c]);
    float v01 = static_cast<float>(img[(y0 * width + x1) * channels + c]);
    float v10 = static_cast<float>(img[(y1 * width + x0) * channels + c]);
    float v11 = static_cast<float>(img[(y1 * width + x1) * channels + c]);
    
    // Bilinear interpolation
    float v0 = v00 * (1.0f - dx) + v01 * dx;
    float v1 = v10 * (1.0f - dx) + v11 * dx;
    
    return v0 * (1.0f - dy) + v1 * dy;
}

/**
 * Kernel: Preprocess single frame
 * BGRA 4K → RGB normalized to [0,1]
 * 
 * @param srcBGRA Source BGRA buffer (4K resolution)
 * @param dstRGB Destination RGB buffer (float, NCHW format)
 * @param srcWidth Source width
 * @param srcHeight Source height
 * @param dstWidth Destination width (e.g., 640 or 1280)
 * @param dstHeight Destination height (e.g., 640 or 720)
 * @param batchIdx Batch index for output offset
 */
__global__ void PreprocessFrameKernel(
    const unsigned char* srcBGRA,
    float* dstRGB,
    int srcWidth, int srcHeight,
    int dstWidth, int dstHeight,
    int batchIdx)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= dstWidth || y >= dstHeight) return;
    
    // Calculate source coordinates (with letterboxing support)
    float scaleX = static_cast<float>(srcWidth) / dstWidth;
    float scaleY = static_cast<float>(srcHeight) / dstHeight;
    
    float srcX = (x + 0.5f) * scaleX - 0.5f;
    float srcY = (y + 0.5f) * scaleY - 0.5f;
    
    // BGRA order: B=0, G=1, R=2, A=3
    // We need RGB output
    float b = bilinearInterpolate(srcBGRA, srcWidth, srcHeight, 4, srcX, srcY, 0);
    float g = bilinearInterpolate(srcBGRA, srcWidth, srcHeight, 4, srcX, srcY, 1);
    float r = bilinearInterpolate(srcBGRA, srcWidth, srcHeight, 4, srcX, srcY, 2);
    
    // Normalize to [0, 1]
    r /= 255.0f;
    g /= 255.0f;
    b /= 255.0f;
    
    // Write to NCHW format: [batch, channel, height, width]
    int planeSize = dstWidth * dstHeight;
    int batchOffset = batchIdx * 3 * planeSize;
    int pixelIdx = y * dstWidth + x;
    
    dstRGB[batchOffset + 0 * planeSize + pixelIdx] = r;  // R channel
    dstRGB[batchOffset + 1 * planeSize + pixelIdx] = g;  // G channel
    dstRGB[batchOffset + 2 * planeSize + pixelIdx] = b;  // B channel
}

/**
 * Kernel: Preprocess UYVY frame
 * UYVY 4K → RGB normalized to [0,1]
 * 
 * UYVY Format (4:2:2 packed):
 * - Each macropixel contains 2 pixels in 4 bytes: [U0 Y0 V0 Y1]
 * - U and V are shared between adjacent pixel pairs
 * - Y (luminance) is stored per-pixel
 * 
 * @param srcUYVY Source UYVY buffer (4K resolution)
 * @param dstRGB Destination RGB buffer (float, NCHW format)
 * @param srcWidth Source width
 * @param srcHeight Source height
 * @param dstWidth Destination width (e.g., 640 or 1280)
 * @param dstHeight Destination height (e.g., 640 or 720)
 * @param batchIdx Batch index for output offset
 */

// UYVY format constants
#define UYVY_BYTES_PER_MACROPIXEL 4    // 4 bytes contain 2 pixels: U Y0 V Y1
#define UYVY_PIXELS_PER_MACROPIXEL 2   // 2 pixels share U and V values
#define UYVY_BYTES_PER_PIXEL 2         // Average bytes per pixel in UYVY

// Byte offsets within a macropixel
#define UYVY_U_OFFSET 0    // U component at byte 0
#define UYVY_Y0_OFFSET 1   // First Y (even pixel) at byte 1
#define UYVY_V_OFFSET 2    // V component at byte 2
#define UYVY_Y1_OFFSET 3   // Second Y (odd pixel) at byte 3

__global__ void PreprocessUYVYKernel(
    const unsigned char* srcUYVY,
    float* dstRGB,
    int srcWidth, int srcHeight,
    int dstWidth, int dstHeight,
    int batchIdx)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    
    if (x >= dstWidth || y >= dstHeight) return;
    
    // Calculate source coordinates
    float scaleX = static_cast<float>(srcWidth) / dstWidth;
    float scaleY = static_cast<float>(srcHeight) / dstHeight;
    
    int srcX = static_cast<int>((x + 0.5f) * scaleX);
    int srcY = static_cast<int>((y + 0.5f) * scaleY);
    
    srcX = min(max(srcX, 0), srcWidth - 1);
    srcY = min(max(srcY, 0), srcHeight - 1);
    
    // Calculate macropixel index and position within macropixel
    int macropixelIdx = srcX / UYVY_PIXELS_PER_MACROPIXEL;
    int pixelInMacro = srcX % UYVY_PIXELS_PER_MACROPIXEL;
    
    // Calculate byte offset: row_offset + macropixel_offset
    // Each row has (srcWidth / 2) macropixels, each 4 bytes
    int srcByteOffset = (srcY * srcWidth + macropixelIdx * UYVY_PIXELS_PER_MACROPIXEL) * UYVY_BYTES_PER_PIXEL;
    
    // Extract YUV components
    unsigned char u = srcUYVY[srcByteOffset + UYVY_U_OFFSET];
    unsigned char v = srcUYVY[srcByteOffset + UYVY_V_OFFSET];
    unsigned char y_val = (pixelInMacro == 0) 
        ? srcUYVY[srcByteOffset + UYVY_Y0_OFFSET]   // Even pixel: use Y0
        : srcUYVY[srcByteOffset + UYVY_Y1_OFFSET];  // Odd pixel: use Y1
    
    // YUV to RGB conversion (BT.709)
    float yf = static_cast<float>(y_val);
    float uf = static_cast<float>(u) - 128.0f;
    float vf = static_cast<float>(v) - 128.0f;
    
    float r = yf + 1.5748f * vf;
    float g = yf - 0.1873f * uf - 0.4681f * vf;
    float b = yf + 1.8556f * uf;
    
    // Clamp and normalize to [0, 1]
    r = fmaxf(0.0f, fminf(255.0f, r)) / 255.0f;
    g = fmaxf(0.0f, fminf(255.0f, g)) / 255.0f;
    b = fmaxf(0.0f, fminf(255.0f, b)) / 255.0f;
    
    // Write to NCHW format
    int planeSize = dstWidth * dstHeight;
    int batchOffset = batchIdx * 3 * planeSize;
    int pixelIdx = y * dstWidth + x;
    
    dstRGB[batchOffset + 0 * planeSize + pixelIdx] = r;  // R channel
    dstRGB[batchOffset + 1 * planeSize + pixelIdx] = g;  // G channel
    dstRGB[batchOffset + 2 * planeSize + pixelIdx] = b;  // B channel
}

// Host-side wrapper functions
extern "C" {

/**
 * Launch preprocessing for BGRA batch
 */
cudaError_t LaunchPreprocessBGRA(
    const void* srcBGRA,
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
    
    PreprocessFrameKernel<<<gridDim, blockDim, 0, stream>>>(
        static_cast<const unsigned char*>(srcBGRA),
        dstRGB,
        srcWidth, srcHeight,
        dstWidth, dstHeight,
        batchIdx
    );
    
    return cudaGetLastError();
}

/**
 * Launch preprocessing for UYVY batch
 */
cudaError_t LaunchPreprocessUYVY(
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
    
    PreprocessUYVYKernel<<<gridDim, blockDim, 0, stream>>>(
        static_cast<const unsigned char*>(srcUYVY),
        dstRGB,
        srcWidth, srcHeight,
        dstWidth, dstHeight,
        batchIdx
    );
    
    return cudaGetLastError();
}

/**
 * Launch batch preprocessing for multiple frames
 */
cudaError_t LaunchPreprocessBatch(
    void** srcBuffers,
    float* dstRGB,
    const int* srcWidths,
    const int* srcHeights,
    int dstWidth, int dstHeight,
    int numFrames,
    bool isUYVY,
    cudaStream_t stream)
{
    for (int i = 0; i < numFrames; ++i) {
        cudaError_t err;
        
        if (isUYVY) {
            err = LaunchPreprocessUYVY(
                srcBuffers[i], dstRGB,
                srcWidths[i], srcHeights[i],
                dstWidth, dstHeight,
                i, stream
            );
        } else {
            err = LaunchPreprocessBGRA(
                srcBuffers[i], dstRGB,
                srcWidths[i], srcHeights[i],
                dstWidth, dstHeight,
                i, stream
            );
        }
        
        if (err != cudaSuccess) {
            return err;
        }
    }
    
    return cudaSuccess;
}

} // extern "C"
