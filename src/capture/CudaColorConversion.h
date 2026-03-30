#pragma once

#include <cuda_runtime.h>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

    /**
     * Lanza el kernel de CUDA para convertir frames YUV 4:2:2 (YUY2) a BGRA8.
     * @param yuv422Device Puntero a memoria de GPU con datos YUYV.
     * @param bgraDevice   Puntero a memoria de GPU para salida BGRA.
     * @param width        Ancho del frame (debe ser par).
     * @param height       Alto del frame.
     * @param stream       Stream de CUDA (por defecto 0).
     */
    bool ConvertYUV422ToBGRA(const uint8_t* yuv422Device,
        uchar4* bgraDevice,
        int width,
        int height,
        cudaStream_t stream = 0);

#ifdef __cplusplus
}
#endif