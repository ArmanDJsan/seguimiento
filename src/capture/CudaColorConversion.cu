#include <cuda_runtime.h>
#include <device_launch_parameters.h> 
#include <vector_types.h>             
#include <cstdint>

#include "CudaColorConversion.h"

// Evitar conflictos con macros de Windows que chocan con el Logger
#ifdef ERROR
#undef ERROR
#endif

__device__ __forceinline__ int clampToByte(int value) {
    return value < 0 ? 0 : (value > 255 ? 255 : value);
}

__device__ __forceinline__ uchar4 yuvToBGRA(int y, int u, int v) {
    // ITU-R BT.601 conversion
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;

    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;

    return make_uchar4(
        (unsigned char)clampToByte(b),
        (unsigned char)clampToByte(g),
        (unsigned char)clampToByte(r),
        255u);
}

__global__ void YUV422ToBGRAKernel(const uchar4* __restrict__ yuv422,
                                   uchar4* __restrict__ bgra,
                                   int width,
                                   int height) {
    const int pairIndex = blockIdx.x * blockDim.x + threadIdx.x;
    const int totalPairs = (width * height) >> 1; 

    if (pairIndex >= totalPairs) return;

    const uchar4 yuyv = yuv422[pairIndex];
    const int y0 = (int)yuyv.x;
    const int u  = (int)yuyv.y;
    const int y1 = (int)yuyv.z;
    const int v  = (int)yuyv.w;

    const int basePixel = pairIndex << 1;
    bgra[basePixel]     = yuvToBGRA(y0, u, v);
    bgra[basePixel + 1] = yuvToBGRA(y1, u, v);
}

extern "C" bool ConvertYUV422ToBGRA(const uint8_t* yuv422Device,
                                    uchar4* bgraDevice,
                                    int width,
                                    int height,
                                    cudaStream_t stream) {
    if (!yuv422Device || !bgraDevice || width <= 0 || height <= 0 || (width & 1)) {
        return false;
    }

    const int totalPairs = (width * height) >> 1;
    const int blockSize = 256;
    const int gridSize = (totalPairs + blockSize - 1) / blockSize;

    YUV422ToBGRAKernel<<<gridSize, blockSize, 0, stream>>>(
        reinterpret_cast<const uchar4*>(yuv422Device),
        bgraDevice,
        width,
        height);

    return cudaGetLastError() == cudaSuccess;
}