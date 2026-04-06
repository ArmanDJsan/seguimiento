#define _HAS_STD_BYTE 0
#include <cuda_runtime.h>
#include "CudaColorConversion.h"

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
        static_cast<unsigned char>(clampToByte(b)),
        static_cast<unsigned char>(clampToByte(g)),
        static_cast<unsigned char>(clampToByte(r)),
        255u);
}

__global__ void YUV422ToBGRAKernel(const uchar4* __restrict__ yuv422,
    uchar4* __restrict__ bgra,
    int width,
    int height) {
    const int pairIndex = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    const int totalPairs = (width * height) >> 1;

    if (pairIndex >= totalPairs) return;

    const uchar4 yuyv = yuv422[pairIndex];
    const int y0 = static_cast<int>(yuyv.x);
    const int u = static_cast<int>(yuyv.y);
    const int y1 = static_cast<int>(yuyv.z);
    const int v = static_cast<int>(yuyv.w);

    const int basePixel = pairIndex << 1;
    bgra[basePixel] = yuvToBGRA(y0, u, v);
    bgra[basePixel + 1] = yuvToBGRA(y1, u, v);
}

// ESTO ES LO QUE SOLUCIONA EL ERROR LNK2001
extern "C" {
    bool ConvertYUV422ToBGRA(const uint8_t* yuv422Device,
        uchar4* bgraDevice,
        int width,
        int height,
        cudaStream_t stream) {
        if (!yuv422Device || !bgraDevice || width <= 0 || height <= 0 || (width & 1)) {
            return false;
        }

        const int totalPairs = (width * height) >> 1;
        constexpr int blockSize = 256;
        const int gridSize = (totalPairs + blockSize - 1) / blockSize;

        YUV422ToBGRAKernel << <gridSize, blockSize, 0, stream >> > (
            reinterpret_cast<const uchar4*>(yuv422Device),
            bgraDevice,
            width,
            height);

        return cudaGetLastError() == cudaSuccess;
    }
}