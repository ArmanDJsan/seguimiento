#pragma once
#include <cuda_runtime.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

    bool ConvertYUV422ToBGRA(const uint8_t* yuv422Device,
        uchar4* bgraDevice,
        int width,
        int height,
        cudaStream_t stream);

#ifdef __cplusplus
}
#endif