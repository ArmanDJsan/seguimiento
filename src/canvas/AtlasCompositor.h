/**
 * AtlasCompositor.h
 * 
 * CUDA kernel declarations for 16K Mega-Canvas composition
 * Header for AtlasCompositor.cu
 */

#pragma once

#include <cuda_runtime.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Blit camera frame to Atlas at specified position
 * 1:1 copy, no scaling
 * 
 * @param sourceBuffer Source BGRA buffer (device memory)
 * @param atlasBuffer Atlas BGRA buffer (device memory)
 * @param sourceWidth Source frame width (3840)
 * @param sourceHeight Source frame height (2160)
 * @param atlasWidth Atlas total width (15360)
 * @param destX Destination X offset in atlas
 * @param destY Destination Y offset in atlas
 * @param stream CUDA stream for async execution
 * @return true if kernel launched successfully
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
);

/**
 * Downscale camera frame for TensorRT inference
 * Bilinear interpolation from 4K to 640x640
 * 
 * @param sourceBuffer Source BGRA buffer (device memory)
 * @param tensorRTBuffer Destination 640x640 buffer (device memory)
 * @param sourceWidth Source frame width (3840)
 * @param sourceHeight Source frame height (2160)
 * @param destWidth Destination width (640)
 * @param destHeight Destination height (640)
 * @param stream CUDA stream for async execution
 * @return true if kernel launched successfully
 */
bool DownscaleForTensorRT(
    const void* sourceBuffer,
    void* tensorRTBuffer,
    int sourceWidth,
    int sourceHeight,
    int destWidth,
    int destHeight,
    cudaStream_t stream
);

/**
 * VRAM Bifurcation: Combined blit + downscale in single kernel
 * Performs both operations concurrently for maximum efficiency
 * 
 * @param sourceBuffer Source BGRA buffer (device memory)
 * @param atlasBuffer Atlas destination (device memory)
 * @param tensorRTBuffer TensorRT destination 640x640 (device memory)
 * @param sourceWidth Source width (3840)
 * @param sourceHeight Source height (2160)
 * @param atlasWidth Atlas total width (15360)
 * @param destX Atlas destination X offset
 * @param destY Atlas destination Y offset
 * @param tensorRTWidth TensorRT output width (640)
 * @param tensorRTHeight TensorRT output height (640)
 * @param stream CUDA stream for async execution
 * @return true if kernel launched successfully
 */
bool BifurcatedCopy(
    const void* sourceBuffer,
    void* atlasBuffer,
    void* tensorRTBuffer,
    int sourceWidth,
    int sourceHeight,
    int atlasWidth,
    int destX,
    int destY,
    int tensorRTWidth,
    int tensorRTHeight,
    cudaStream_t stream
);

/**
 * Clear Atlas buffer to black
 * 
 * @param atlasBuffer Atlas buffer (device memory)
 * @param width Atlas width (15360)
 * @param height Atlas height (6480)
 * @param stream CUDA stream
 * @return true if successful
 */
bool ClearAtlas(
    void* atlasBuffer,
    int width,
    int height,
    cudaStream_t stream
);

/**
 * Copy Atlas to CUDA array (for D3D11 interop)
 * 
 * @param atlasBuffer Source atlas buffer (device memory)
 * @param destArray Destination CUDA array (mapped from D3D11)
 * @param width Atlas width
 * @param height Atlas height
 * @param stream CUDA stream
 * @return true if successful
 */
bool CopyAtlasToArray(
    const void* atlasBuffer,
    cudaArray_t destArray,
    int width,
    int height,
    cudaStream_t stream
);

#ifdef __cplusplus
}
#endif
