#pragma once

#include <Windows.h>
#include <string>
#include <sstream>
#include <cuda_runtime.h>
#include "../utils/Logger.h"

/**
 * GPUDiagnostics - Hardware Accelerated GPU Scheduling and PCIe Bandwidth Checks
 * 
 * Critical for RTX 5080 performance on Windows 11:
 * - Hardware Accelerated GPU Scheduling (HAGS) must be enabled
 * - PCIe link must negotiate at x16 (not x8 or x4)
 * - GPU Copy engine usage must be monitored to avoid saturation
 */
class GPUDiagnostics {
public:
    struct PCIeInfo {
        int linkWidth;          // PCIe lanes (1, 4, 8, 16)
        int linkGen;            // PCIe generation (3, 4, 5)
        double bandwidth_gbps;  // Theoretical bandwidth in GB/s
        bool isOptimal;         // true if x16 Gen4/Gen5
    };

    struct GPUSchedulingInfo {
        bool hagsSupported;     // Hardware supports HAGS
        bool hagsEnabled;       // HAGS is currently enabled
        std::string status;     // Human-readable status
    };

    /**
     * Check if Hardware Accelerated GPU Scheduling is enabled
     * @return Scheduling information
     */
    static GPUSchedulingInfo CheckHardwareAcceleratedScheduling() {
        GPUSchedulingInfo info = {false, false, ""};
        
        // Check registry for HAGS status
        // HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\GraphicsDrivers
        // Value: HwSchMode (DWORD)
        // 1 = Disabled, 2 = Enabled
        
        HKEY hKey;
        LONG result = RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
            0,
            KEY_READ,
            &hKey
        );
        
        if (result != ERROR_SUCCESS) {
            info.status = "Cannot access registry (may need admin)";
            Logger::Warning("HAGS check failed: " + info.status);
            return info;
        }
        
        DWORD hwSchMode = 0;
        DWORD dataSize = sizeof(DWORD);
        result = RegQueryValueExA(
            hKey,
            "HwSchMode",
            NULL,
            NULL,
            reinterpret_cast<LPBYTE>(&hwSchMode),
            &dataSize
        );
        
        RegCloseKey(hKey);
        
        if (result != ERROR_SUCCESS) {
            info.status = "HwSchMode registry value not found (default disabled)";
            info.hagsSupported = false;
            info.hagsEnabled = false;
        } else {
            info.hagsSupported = true;
            info.hagsEnabled = (hwSchMode == 2);
            info.status = info.hagsEnabled ? "Enabled" : "Disabled (set HwSchMode=2 to enable)";
        }
        
        if (info.hagsEnabled) {
            Logger::Info("Hardware Accelerated GPU Scheduling: ENABLED");
        } else {
            Logger::Warning("Hardware Accelerated GPU Scheduling: DISABLED");
            Logger::Warning("  To enable: Settings > System > Display > Graphics > Change default graphics settings");
            Logger::Warning("  Or: Set HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers\\HwSchMode = 2");
        }
        
        return info;
    }

    /**
     * Get PCIe link information from CUDA
     * @param deviceId CUDA device ID
     * @return PCIe information
     */
    static PCIeInfo GetPCIeInfo(int deviceId = 0) {
        PCIeInfo info = {0, 0, 0.0, false};
        
        cudaDeviceProp props;
        cudaError_t err = cudaGetDeviceProperties(&props, deviceId);
        if (err != cudaSuccess) {
            Logger::Error("Failed to get CUDA device properties: " + 
                         std::string(cudaGetErrorString(err)));
            return info;
        }
        
        // Get PCIe link width (number of lanes)
        int linkWidth = 0;
        err = cudaDeviceGetAttribute(&linkWidth, cudaDevAttrPciMaxLinkWidth, deviceId);
        if (err == cudaSuccess) {
            info.linkWidth = linkWidth;
        }
        
        // Get PCIe generation (3, 4, 5, 6)
        int linkGen = 0;
        err = cudaDeviceGetAttribute(&linkGen, cudaDevAttrPciMaxLinkGeneration, deviceId);
        if (err == cudaSuccess) {
            info.linkGen = linkGen;
        }
        
        // Calculate theoretical bandwidth
        // PCIe bandwidth per lane per direction (GB/s):
        // Gen 3: ~1.0 GB/s
        // Gen 4: ~2.0 GB/s
        // Gen 5: ~4.0 GB/s
        double bandwidthPerLane = 0.0;
        switch (info.linkGen) {
            case 3: bandwidthPerLane = 0.985; break;  // ~1 GB/s
            case 4: bandwidthPerLane = 1.969; break;  // ~2 GB/s
            case 5: bandwidthPerLane = 3.938; break;  // ~4 GB/s
            case 6: bandwidthPerLane = 7.877; break;  // ~8 GB/s (future)
            default: bandwidthPerLane = 0.0; break;
        }
        
        info.bandwidth_gbps = bandwidthPerLane * info.linkWidth;
        info.isOptimal = (info.linkWidth == 16 && info.linkGen >= 4);
        
        // Log PCIe information
        std::ostringstream oss;
        oss << "PCIe Link: Gen" << info.linkGen << " x" << info.linkWidth 
            << " (" << info.bandwidth_gbps << " GB/s bidirectional)";
        
        if (info.isOptimal) {
            Logger::Info(oss.str() + " - OPTIMAL");
        } else {
            Logger::Warning(oss.str() + " - SUBOPTIMAL (expected x16 Gen4+)");
            if (info.linkWidth < 16) {
                Logger::Warning("  PCIe link negotiated at x" + std::to_string(info.linkWidth) + 
                               " instead of x16. Check motherboard slot.");
            }
            if (info.linkGen < 4) {
                Logger::Warning("  PCIe Gen" + std::to_string(info.linkGen) + 
                               " detected. RTX 5080 supports Gen5.");
            }
        }
        
        return info;
    }

    /**
     * Get comprehensive GPU information
     */
    static std::string GetGPUInfo(int deviceId = 0) {
        cudaDeviceProp props;
        cudaError_t err = cudaGetDeviceProperties(&props, deviceId);
        if (err != cudaSuccess) {
            return "Failed to get GPU info: " + std::string(cudaGetErrorString(err));
        }
        
        std::ostringstream oss;
        oss << "GPU: " << props.name << "\n";
        oss << "  Compute Capability: " << props.major << "." << props.minor << "\n";
        oss << "  Total VRAM: " << (props.totalGlobalMem / (1024*1024*1024)) << " GB\n";
        oss << "  CUDA Cores: " << props.multiProcessorCount * 128 << " (estimated)\n";
        oss << "  Memory Clock: " << (props.memoryClockRate / 1000) << " MHz\n";
        oss << "  Memory Bus Width: " << props.memoryBusWidth << "-bit\n";
        
        // Get free VRAM
        size_t freeMem = 0, totalMem = 0;
        cudaMemGetInfo(&freeMem, &totalMem);
        oss << "  VRAM Free: " << (freeMem / (1024*1024*1024)) << " GB / " 
            << (totalMem / (1024*1024*1024)) << " GB";
        
        return oss.str();
    }

    /**
     * Run full diagnostics and log results
     */
    static bool RunFullDiagnostics() {
        Logger::Info("=== GPU Diagnostics ===");
        
        // Check HAGS
        auto hagsInfo = CheckHardwareAcceleratedScheduling();
        
        // Check PCIe
        auto pcieInfo = GetPCIeInfo(0);
        
        // Get GPU info
        std::string gpuInfo = GetGPUInfo(0);
        Logger::Info(gpuInfo);
        
        Logger::Info("=======================");
        
        // Determine if system is optimally configured
        bool optimal = hagsInfo.hagsEnabled && pcieInfo.isOptimal;
        
        if (!optimal) {
            Logger::Warning("System configuration is NOT optimal for VIB:");
            if (!hagsInfo.hagsEnabled) {
                Logger::Warning("  [!] Enable Hardware Accelerated GPU Scheduling");
            }
            if (!pcieInfo.isOptimal) {
                Logger::Warning("  [!] Verify PCIe x16 Gen4/Gen5 connection");
            }
        } else {
            Logger::Info("System configuration is OPTIMAL for VIB");
        }
        
        return optimal;
    }

    /**
     * Monitor GPU Copy engine usage (requires NVML library)
     * Note: This is a placeholder - full implementation requires NVIDIA Management Library
     */
    static void LogGPUCopyEngineUsage() {
        // Placeholder for NVML integration
        // Would require linking nvml.lib and including nvml.h
        Logger::Debug("GPU Copy engine monitoring requires NVML integration");
    }
};
