#pragma once

#include <cuda_runtime.h>
#include <cstdint>

/**
 * Launches CUDA kernel to convert YUV 4:2:2 (YUY2) frames to BGRA8.
 * @param yuv422Device Pointer to device memory containing YUY2 packed data.
 * @param bgraDevice Pointer to device memory for BGRA8 output.
 * @param width Frame width in pixels (must be even for 4:2:2).
 * @param height Frame height in pixels.
 * @param stream CUDA stream to enqueue the work on.
 * @return true if the kernel launch was successful, false otherwise.
 */
bool ConvertYUV422ToBGRA(const uint8_t* yuv422Device,
                         uchar4* bgraDevice,
                         int width,
                         int height,
                         cudaStream_t stream);
