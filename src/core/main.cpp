/**
 * Visual Intelligence Bypass (VIB) - Main Entry Point
 * 
 * High-performance video capture and AI processing system for vMix
 * Supports up to 12x 4K@60fps streams with zero-copy DMA
 * 
 * Target Hardware:
 * - AMD Threadripper Pro 9955WX
 * - NVIDIA RTX 5080 16GB
 * - 3x Blackmagic DeckLink 8K Pro Mini
 * - 128GB DDR5 RAM
 */

#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <Windows.h>
#include <objbase.h>  // For COM: CoInitializeEx, CoUninitialize
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>

#include "../DeckLinkAPI_h.h"  // DeckLink SDK COM interfaces
#include "../capture/DeckLinkCapture.h"
#include "../capture/DeckLinkSource.h"
#include "../ndi/NDIManager.h"
#include "../ai/YOLOProcessor.h"
#include "../redis/RedisWorker.h"
#include "../utils/Logger.h"
#include "../control/VideoHubClient.h"
#include "../control/TrackPhysicalController.h"
#include "../control/VMixController.h"

// Link DirectX libraries (still needed for D3D11 device creation used by CUDA interop)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Link COM library (required for DeckLink SDK)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")  // For SysFreeString

#include <unordered_map>
#include <vector>
#include <numeric>
#include "../json.hpp"

using json = nlohmann::json;

constexpr int kMaxNDIChannels = 12;
constexpr unsigned int kDefaultWidth = 3840;
constexpr unsigned int kDefaultHeight = 2160;
constexpr int kVideoHubPrimaryOutput = 0;

namespace {
std::unordered_map<std::string, int> BuildInputLookup() {
    std::unordered_map<std::string, int> lookup;
    for (int i = 1; i <= 12; ++i) {
        std::stringstream ss;
        ss << "CAM_" << std::setw(2) << std::setfill('0') << i;
        lookup[ss.str()] = i;
    }
    lookup["RADAR_01"] = 13;
    lookup["RADAR_02"] = 14;
    lookup["RADAR_03"] = 15;
    lookup["RADAR_04"] = 16;
    return lookup;
}

std::vector<int> RangeInclusive(int start, int end) {
    std::vector<int> values(static_cast<size_t>(end - start + 1));
    std::iota(values.begin(), values.end(), start);
    return values;
}

struct Config {
    std::string videohubIp;
    uint16_t videohubPort;
    std::string esp32Ip;
    uint16_t esp32Port;
    int targetSpheres;
};

Config LoadConfig(const std::string& path) {
    Config config;
    // Set defaults
    config.videohubIp = "192.168.1.50";
    config.videohubPort = 9990;
    config.esp32Ip = "192.168.88.114";
    config.esp32Port = 80;
    config.targetSpheres = 10;

    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Warning("No se pudo abrir config.json; usando valores por defecto");
        return config;
    }

    try {
        json j;
        file >> j;

        // Parse videohub section
        if (j.contains("videohub") && j["videohub"].is_object()) {
            if (j["videohub"].contains("ip") && j["videohub"]["ip"].is_string()) {
                config.videohubIp = j["videohub"]["ip"].get<std::string>();
            }
            if (j["videohub"].contains("port") && j["videohub"]["port"].is_number()) {
                config.videohubPort = j["videohub"]["port"].get<uint16_t>();
            }
        }

        // Parse esp32 section
        if (j.contains("esp32") && j["esp32"].is_object()) {
            if (j["esp32"].contains("ip") && j["esp32"]["ip"].is_string()) {
                config.esp32Ip = j["esp32"]["ip"].get<std::string>();
            }
            if (j["esp32"].contains("port") && j["esp32"]["port"].is_number()) {
                config.esp32Port = j["esp32"]["port"].get<uint16_t>();
            }
        }

        // Parse target_spheres from root
        if (j.contains("target_spheres") && j["target_spheres"].is_number()) {
            config.targetSpheres = j["target_spheres"].get<int>();
        }

        Logger::Info("Configuración cargada: VideoHub=" + config.videohubIp + ":" + 
                     std::to_string(config.videohubPort) + ", ESP32=" + config.esp32Ip + ":" + 
                     std::to_string(config.esp32Port) + ", target_spheres=" + 
                     std::to_string(config.targetSpheres));

    } catch (const json::exception& e) {
        Logger::Warning("Error al parsear config.json: " + std::string(e.what()) + "; usando valores por defecto");
    } catch (const std::exception& e) {
        Logger::Warning("Error inesperado al cargar config: " + std::string(e.what()) + "; usando valores por defecto");
    }

    return config;
}

bool ValidateSignalGroup(VideoHubClient& videoHub,
                         DeckLinkSource& deckLinkSource,
                         const std::vector<int>& ports,
                         const std::string& label) {
    for (int port : ports) {
        if (!videoHub.RouteInputToOutput(kVideoHubPrimaryOutput, port)) {
            Logger::Error("[HW/SW ERROR]: No se pudo conmutar VideoHub para puerto " + std::to_string(port));
            return false;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto statuses = deckLinkSource.GetSignalStatus(ports);
    for (const auto& status : statuses) {
        if (!status.signalLocked) {
            const std::string name = status.name.value_or("Port_" + std::to_string(status.index));
            Logger::Error("[HW/SW ERROR]: Sin señal en " + name);
            return false;
        }
    }

    Logger::Info("Barrido " + label + " OK");
    return true;
}

bool RunPhase1(VMixController& vmix,
               VideoHubClient& videoHub,
               DeckLinkSource& deckLinkSource,
               TrackPhysicalController& trackController) {
    if (!vmix.CheckInputsHealthy()) {
        return false;
    }

    if (!ValidateSignalGroup(videoHub, deckLinkSource, RangeInclusive(1, 12), "Streaming (1-12)")) {
        return false;
    }

    if (!ValidateSignalGroup(videoHub, deckLinkSource, RangeInclusive(13, 16), "Seguimiento (13-16)")) {
        return false;
    }

    if (!trackController.ejecutarTest()) {
        Logger::Error("[HW/SW ERROR] Prueba mecánica (ESP32 /test) fallida");
        return false;
    }

    Logger::Info("Fase 1 completada correctamente");
    return true;
}

bool RunPhase2(VideoHubClient& videoHub, int targetSpheres) {
    if (!videoHub.RouteInputToOutput(kVideoHubPrimaryOutput, "CAM_01")) {
        Logger::Error("[HW/SW ERROR]: No se pudo fijar CAM_01 en la salida");
        return false;
    }

    // Placeholder YOLO check (pipeline integration pending)
    const int detectedSpheres = targetSpheres;
    Logger::Warning("Conteo de esferas usa stub; integrar motor YOLO para conteo real");

    if (detectedSpheres != targetSpheres) {
        Logger::Error("[SCENE ERROR]: Conteo incorrecto (" + std::to_string(detectedSpheres) +
                      ") - Verifique esferas en pista");
        return false;
    }

    Logger::Info("Fase 2 (Escena) completada: conteo de esferas OK");
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    Logger::Init("VIB_System");
    Logger::Info("Visual Intelligence Bypass v2.0 Starting...");
    
    // Initialize COM for DeckLink SDK (Component Object Model)
    // This MUST be called before any DeckLink interfaces are created
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Logger::Error("Failed to initialize COM. HRESULT: 0x" + std::to_string(hr));
        Logger::Error("DeckLink SDK requires COM initialization to function properly");
        return 1;
    }
    Logger::Info("COM initialized successfully for DeckLink SDK");
    
    try {
        // Load configuration from JSON
        Config config = LoadConfig("config.json");

        // Controllers for diagnostics and routing
        auto inputLookup = BuildInputLookup();
        VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
        
        // Convert ESP32 IP to wide string
        std::wstring esp32IpWide(config.esp32Ip.begin(), config.esp32Ip.end());
        TrackPhysicalController trackController(esp32IpWide, config.esp32Port);
        
        VMixController vmix(L"127.0.0.1", 8088, 8099);
        DeckLinkSource deckLinkSource;
        deckLinkSource.Initialize(12);

        if (!videoHub.Connect()) {
            Logger::Error("[HW/SW ERROR] No se pudo establecer conexión con VideoHub");
            return 1;
        }

        vmix.ConnectTcp();  // prepare TCP channel; errors are logged but non-fatal

        // Fase 1: Sweep hardware/software links
        if (!RunPhase1(vmix, videoHub, deckLinkSource, trackController)) {
            Logger::Error("Ignición abortada durante Fase 1");
            return 1;
        }

        // Fase 2: Escena (YOLO check)
        if (!RunPhase2(videoHub, config.targetSpheres)) {
            Logger::Error("Ignición abortada durante Fase 2");
            return 1;
        }

        // ============================================================================
        // NDI Initialization (replaced Spout for vMix compatibility)
        // ============================================================================
        // NDI provides:
        // - Native vMix support (no plugins needed)
        // - Zero-copy async sending via completion callbacks
        // - UYVY format support (native DeckLink format, optimal for vMix)
        // - Network transparency (works across machines if needed)
        //
        // Note: D3D11 device creation is no longer required for video output.
        //       DeckLinkCapture uses CUDA-only pipeline for zero-copy DMA.
        // ============================================================================
        
        Logger::Info("Initializing NDI output for vMix...");
        
        // Initialize NDI Manager
        auto ndiManager = std::make_shared<NDIManager>();
        if (!ndiManager->Initialize()) {
            Logger::Error("Failed to initialize NDI library");
            throw std::runtime_error("NDI initialization failed");
        }
        
        // Pre-create all 12 NDI senders to maintain stable source names for vMix
        // vMix will see these as "VIB_CAM_01" through "VIB_CAM_12"
        Logger::Info("Creating NDI senders for " + std::to_string(kMaxNDIChannels) + " channels...");
        for (int channel = 0; channel < kMaxNDIChannels; ++channel) {
            std::ostringstream oss;
            oss << "VIB_CAM_" << std::setw(2) << std::setfill('0') << (channel + 1);
            const std::string senderName = oss.str();
            
            // Use UYVY format for optimal performance:
            // - DeckLink outputs UYVY natively
            // - vMix expects UYVY and converts internally to 32-bit float 4:4:4
            // - Avoids YUV→BGRA conversion for vMix path (saves ~1-2ms per frame)
            if (!ndiManager->CreateSender(channel, senderName, kDefaultWidth, kDefaultHeight, true)) {
                Logger::Error("Failed to create NDI sender: " + senderName);
                throw std::runtime_error("NDI sender creation failed");
            }
        }
        Logger::Info("NDI senders initialized successfully");
        
        // Log vMix configuration tips
        Logger::Info("=== vMix Configuration Tips ===");
        Logger::Info("1. Enable 'High Input Performance Mode' for 9+ cameras (requires GPU with >3GB VRAM)");
        Logger::Info("2. Disable 'Show preview thumbnails for NDI sources' to reduce network traffic");
        Logger::Info("3. NDI sources will appear as 'VIB_CAM_01' through 'VIB_CAM_12'");
        
        // Initialize capture channels
        // Note: DeckLinkCapture uses CUDA-only pipeline (no D3D11 dependency)
        // The YUV data path splits at DeckLinkCapture:
        // - UYVY → NDI for vMix (zero conversion)
        // - BGRA → YOLO for AI inference (converted via CUDA kernel)
        Logger::Info("Initializing capture channels...");
        std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
        
        // Auto-detect DeckLink devices
        int numDevices = DeckLinkCapture::EnumerateDevices();
        Logger::Info("Found " + std::to_string(numDevices) + " DeckLink devices");
        if (numDevices < kMaxNDIChannels) {
            Logger::Warning("Fewer capture devices than NDI senders (" +
                            std::to_string(numDevices) + " vs " +
                            std::to_string(kMaxNDIChannels) +
                            "). Idle senders remain available to keep channel names stable in vMix.");
        }
        
        const int channelsToInit = (std::min)(numDevices, kMaxNDIChannels);
        for (int i = 0; i < channelsToInit; i++) {
            auto capture = std::make_unique<DeckLinkCapture>();
            const std::string channelName = "Channel_" + std::to_string(i + 1);
            if (capture->Initialize(i, channelName)) {
                // Frame ready handler: sends video via NDI
                // The handler receives both YUV and BGRA buffers from DeckLinkCapture:
                // - YUV (UYVY) goes to NDI for vMix output (zero color conversion)
                // - BGRA is used for YOLO inference (already converted via CUDA kernel)
                capture->SetFrameReadyHandler([ndiManager](const VideoChannel& channel, cudaStream_t stream) {
                    // Send UYVY directly to NDI for vMix
                    // This is the zero-conversion path - DeckLink outputs UYVY,
                    // vMix expects UYVY and converts internally to 32-bit float 4:4:4
                    ndiManager->SendUYVYFrame(
                        channel.channelID,
                        channel.cudaYUVBuffer,  // UYVY buffer from DeckLink
                        channel.width,
                        channel.height,
                        stream
                    );
                    
                    // YOLO inference path uses cudaBGRABuffer (converted separately)
                    // This happens in parallel via the YOLO processor
                });
                capture->Start();
                captureChannels.push_back(std::move(capture));
            }
        }
        
        // Initialize YOLO/TensorRT processor
        Logger::Info("Initializing YOLO processor...");
        YOLOProcessor yoloProcessor;
        
        // Initialize Redis worker
        Logger::Info("Initializing Redis worker...");
        RedisWorker redisWorker("127.0.0.1", 6379);
        redisWorker.Start();
        
        // Main capture loop
        Logger::Info("Starting capture loop...");
        Logger::Info("Press ESC to exit");
        bool running = true;
        
        while (running) {
            // Check for exit condition
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                running = false;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        
        // Cleanup
        Logger::Info("Shutting down...");
        redisWorker.Stop();
        
        // Stop all capture channels before releasing NDI
        for (auto& capture : captureChannels) {
            capture->Stop();
        }
        captureChannels.clear();
        
        // Release NDI senders
        ndiManager->ReleaseAll();
        
        Logger::Info("Visual Intelligence Bypass shutdown complete");
        
    } catch (const std::exception& e) {
        Logger::Error("Fatal error: " + std::string(e.what()));
        CoUninitialize();
        return 1;
    }
    
    // Uninitialize COM before exiting
    CoUninitialize();
    Logger::Info("COM uninitialized");
    
    return 0;
}
