/**
 * FrameSaver.h
 * 
 * Utility for saving video frames to disk without external dependencies
 * Supports saving CUDA buffers in PPM (Portable PixMap) format
 * PPM format is chosen because:
 * - No external dependencies required
 * - Simple ASCII/binary format
 * - Easy to view with most image viewers
 * - Perfect for debugging and verification
 */

#pragma once

#include <string>
#include <fstream>
#include <vector>
#include <cuda_runtime.h>
#include "Logger.h"

class FrameSaver {
public:
    /**
     * Save BGRA frame from CUDA device memory to PPM file
     * @param cudaBuffer CUDA device pointer to BGRA buffer
     * @param width Frame width
     * @param height Frame height
     * @param filename Output filename (e.g., "detection_frame.ppm")
     * @return true if successful
     */
    static bool SaveBGRAFrame(void* cudaBuffer, int width, int height, const std::string& filename) {
        if (!cudaBuffer) {
            Logger::Error("FrameSaver: cudaBuffer is null");
            return false;
        }

        // Allocate host memory for the frame
        size_t bufferSize = width * height * 4; // BGRA = 4 bytes per pixel
        std::vector<unsigned char> hostBuffer(bufferSize);

        // Copy from device to host
        cudaError_t err = cudaMemcpy(hostBuffer.data(), cudaBuffer, bufferSize, cudaMemcpyDeviceToHost);
        if (err != cudaSuccess) {
            Logger::Error("FrameSaver: cudaMemcpy failed - " + std::string(cudaGetErrorString(err)));
            return false;
        }

        // Open output file
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            Logger::Error("FrameSaver: Failed to open file " + filename);
            return false;
        }

        // Write PPM header (P6 = binary RGB format)
        file << "P6\n" << width << " " << height << "\n255\n";

        // Convert BGRA to RGB and write
        for (int i = 0; i < width * height; ++i) {
            int idx = i * 4;
            unsigned char b = hostBuffer[idx + 0];
            unsigned char g = hostBuffer[idx + 1];
            unsigned char r = hostBuffer[idx + 2];
            // Skip alpha channel (idx + 3)
            
            // Write as RGB
            file.put(r);
            file.put(g);
            file.put(b);
        }

        file.close();
        Logger::Info("FrameSaver: Saved frame to " + filename);
        return true;
    }

    /**
     * Save YUV frame from CUDA device memory to raw file
     * @param cudaBuffer CUDA device pointer to YUV buffer (UYVY format)
     * @param width Frame width
     * @param height Frame height
     * @param filename Output filename (e.g., "detection_frame.yuv")
     * @return true if successful
     */
    static bool SaveYUVFrame(void* cudaBuffer, int width, int height, const std::string& filename) {
        if (!cudaBuffer) {
            Logger::Error("FrameSaver: cudaBuffer is null");
            return false;
        }

        // UYVY format: 2 bytes per pixel (packed YUV)
        size_t bufferSize = width * height * 2;
        std::vector<unsigned char> hostBuffer(bufferSize);

        // Copy from device to host
        cudaError_t err = cudaMemcpy(hostBuffer.data(), cudaBuffer, bufferSize, cudaMemcpyDeviceToHost);
        if (err != cudaSuccess) {
            Logger::Error("FrameSaver: cudaMemcpy failed - " + std::string(cudaGetErrorString(err)));
            return false;
        }

        // Write raw YUV data
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            Logger::Error("FrameSaver: Failed to open file " + filename);
            return false;
        }

        file.write(reinterpret_cast<const char*>(hostBuffer.data()), bufferSize);
        file.close();

        Logger::Info("FrameSaver: Saved YUV frame to " + filename + " (" + 
                     std::to_string(width) + "x" + std::to_string(height) + ")");
        return true;
    }

    /**
     * Save YUV frame converted to RGB PPM format
     * Converts UYVY to RGB and saves as PPM for easy viewing
     * @param cudaBuffer CUDA device pointer to YUV buffer (UYVY format)
     * @param width Frame width
     * @param height Frame height
     * @param filename Output filename (e.g., "detection_frame.ppm")
     * @return true if successful
     */
    static bool SaveYUVFrameAsPPM(void* cudaBuffer, int width, int height, const std::string& filename) {
        if (!cudaBuffer) {
            Logger::Error("FrameSaver: cudaBuffer is null");
            return false;
        }

        // UYVY format: 2 bytes per pixel
        size_t bufferSize = width * height * 2;
        std::vector<unsigned char> hostBuffer(bufferSize);

        // Copy from device to host
        cudaError_t err = cudaMemcpy(hostBuffer.data(), cudaBuffer, bufferSize, cudaMemcpyDeviceToHost);
        if (err != cudaSuccess) {
            Logger::Error("FrameSaver: cudaMemcpy failed - " + std::string(cudaGetErrorString(err)));
            return false;
        }

        // Open output file
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            Logger::Error("FrameSaver: Failed to open file " + filename);
            return false;
        }

        // Write PPM header
        file << "P6\n" << width << " " << height << "\n255\n";

        // Convert UYVY to RGB
        // UYVY format: U0 Y0 V0 Y1 (2 pixels per 4 bytes)
        for (int i = 0; i < width * height / 2; ++i) {
            int idx = i * 4;
            // Cast to int to avoid sign extension issues
            int u = static_cast<int>(hostBuffer[idx + 0]);
            int y0 = static_cast<int>(hostBuffer[idx + 1]);
            int v = static_cast<int>(hostBuffer[idx + 2]);
            int y1 = static_cast<int>(hostBuffer[idx + 3]);

            // Convert to RGB using ITU-R BT.601 standard
            // Pixel 1
            int c0 = y0 - 16;
            int d = u - 128;
            int e = v - 128;
            int r0 = (298 * c0 + 409 * e + 128) >> 8;
            int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
            int b0 = (298 * c0 + 516 * d + 128) >> 8;

            // Clamp to 0-255
            r0 = (r0 < 0) ? 0 : (r0 > 255) ? 255 : r0;
            g0 = (g0 < 0) ? 0 : (g0 > 255) ? 255 : g0;
            b0 = (b0 < 0) ? 0 : (b0 > 255) ? 255 : b0;

            // Pixel 2
            int c1 = y1 - 16;
            int r1 = (298 * c1 + 409 * e + 128) >> 8;
            int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c1 + 516 * d + 128) >> 8;

            // Clamp to 0-255
            r1 = (r1 < 0) ? 0 : (r1 > 255) ? 255 : r1;
            g1 = (g1 < 0) ? 0 : (g1 > 255) ? 255 : g1;
            b1 = (b1 < 0) ? 0 : (b1 > 255) ? 255 : b1;

            // Write both pixels
            file.put(static_cast<unsigned char>(r0));
            file.put(static_cast<unsigned char>(g0));
            file.put(static_cast<unsigned char>(b0));
            file.put(static_cast<unsigned char>(r1));
            file.put(static_cast<unsigned char>(g1));
            file.put(static_cast<unsigned char>(b1));
        }

        file.close();
        Logger::Info("FrameSaver: Saved YUV frame as PPM to " + filename);
        return true;
    }
};
