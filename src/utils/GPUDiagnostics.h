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
      */
    static PCIeInfo GetPCIeInfo(int deviceId = 0) {
        PCIeInfo info = { 0, 0, 0.0, false };

        // Usamos IDs numéricos para evitar el error de "identificador no declarado"
        // 56 = cudaDevAttrPciMaxLinkWidth
        // 57 = cudaDevAttrPciMaxLinkGeneration
        int linkWidth = 0;
        int linkGen = 0;

        if (cudaDeviceGetAttribute(&linkWidth, (cudaDeviceAttr)56, deviceId) == cudaSuccess) {
            info.linkWidth = linkWidth;
        }

        if (cudaDeviceGetAttribute(&linkGen, (cudaDeviceAttr)57, deviceId) == cudaSuccess) {
            info.linkGen = linkGen;
        }

        double bandwidthPerLane = 0.0;
        switch (info.linkGen) {
        case 3: bandwidthPerLane = 0.985; break;
        case 4: bandwidthPerLane = 1.969; break;
        case 5: bandwidthPerLane = 3.938; break; // RTX 5080
        default: bandwidthPerLane = 0.0; break;
        }

        info.bandwidth_gbps = bandwidthPerLane * info.linkWidth;
        info.isOptimal = (info.linkWidth == 16 && info.linkGen >= 4);

        return info;
    }

    /**
     * Get comprehensive GPU information
     */
    static std::string GetGPUInfo(int deviceId = 0) {
        cudaDeviceProp props;
        if (cudaGetDeviceProperties(&props, deviceId) != cudaSuccess) {
            return "Failed to get GPU info";
        }

        // 15 = cudaDevAttrMemoryClockRate (en kHz)
        int memClockKHz = 0;
        cudaDeviceGetAttribute(&memClockKHz, (cudaDeviceAttr)15, deviceId);

        std::ostringstream oss;
        oss << "GPU: " << props.name << "\n";
        oss << "  Compute: " << props.major << "." << props.minor << "\n";
        oss << "  VRAM: " << (props.totalGlobalMem / (1024 * 1024 * 1024)) << " GB\n";
        oss << "  Memory Clock: " << (memClockKHz / 1000) << " MHz\n";

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
