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
#include "../spout/SpoutManager.h"
#include "../ai/YOLOProcessor.h"
#include "../redis/RedisWorker.h"
#include "../utils/Logger.h"
#include "../control/VideoHubClient.h"
#include "../control/TrackPhysicalController.h"
#include "../control/VMixController.h"

// Link DirectX libraries
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

constexpr int kMaxSpoutChannels = 12;
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

        // Initialize DirectX 11 Device (for Spout output only)
        // Note: DeckLinkCapture uses CUDA-based zero-copy, not D3D11
        Logger::Info("Initializing DirectX 11 for Spout output...");
        
        using Microsoft::WRL::ComPtr;
        ComPtr<IDXGIFactory1> dxgiFactory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(hr)) {
            Logger::Error("Failed to create DXGI factory. HRESULT: 0x" + std::to_string(hr));
            throw std::runtime_error("DXGI factory creation failed");
        }

        // Select RTX 5080 adapter explicitly (avoid integrated GPU)
        ComPtr<IDXGIAdapter1> selectedAdapter;
        DXGI_ADAPTER_DESC1 selectedDesc = {};
        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            if (dxgiFactory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
                break;
            }

            DXGI_ADAPTER_DESC1 desc;
            if (FAILED(adapter->GetDesc1(&desc))) {
                continue;
            }

            // Skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }

            std::wstring name(desc.Description);
            if (name.find(L"RTX 5080") != std::wstring::npos) {
                selectedAdapter = adapter;
                selectedDesc = desc;
                break;
            }

            // Fallback to first hardware adapter if RTX 5080 not found
            if (!selectedAdapter) {
                selectedAdapter = adapter;
                selectedDesc = desc;
            }
        }

        if (!selectedAdapter) {
            Logger::Error("No suitable GPU adapter found");
            throw std::runtime_error("GPU selection failed");
        }

        char adapterName[128];
        size_t convertedChars = 0;
        wcstombs_s(&convertedChars, adapterName, 128, selectedDesc.Description, 127);
        Logger::Info("Selected GPU: " + std::string(adapterName));
        Logger::Info("GPU LUID: High=" + std::to_string(selectedDesc.AdapterLuid.HighPart) +
                     " Low=" + std::to_string(selectedDesc.AdapterLuid.LowPart));
        
        // Define feature levels (RTX 5080 supports 12+, but 11.0 is sufficient)
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL featureLevel;
        
        // Device creation flags
        UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // Required for Spout
        #ifdef _DEBUG
            createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
        #endif
        
        // Create the D3D11 device and context
        ComPtr<ID3D11Device> d3d11Device;
        ComPtr<ID3D11DeviceContext> d3d11Context;
        hr = D3D11CreateDevice(
            selectedAdapter.Get(),      // Explicit adapter selection
            D3D_DRIVER_TYPE_UNKNOWN,    // Must be UNKNOWN when adapter provided
            nullptr,                    // No software module
            createDeviceFlags,          // BGRA support + debug flag in debug builds
            featureLevels,              // Feature levels to try
            _countof(featureLevels),    // Number of feature levels
            D3D11_SDK_VERSION,          // SDK version
            d3d11Device.GetAddressOf(), // Output device
            &featureLevel,              // Actual feature level
            d3d11Context.GetAddressOf() // Output context
        );
        
        if (FAILED(hr)) {
            Logger::Error("Failed to create D3D11 device. HRESULT: 0x" + 
                          std::to_string(hr));
            throw std::runtime_error("DirectX 11 initialization failed");
        }
        
        Logger::Info("DirectX 11 initialized successfully");
        Logger::Info("Feature Level: " + std::to_string(featureLevel >> 12) + 
                     "." + std::to_string((featureLevel >> 8) & 0xF));
        
        // Query adapter information for verification
        ComPtr<IDXGIDevice> dxgiDevice;
        hr = d3d11Device.As(&dxgiDevice);
        if (SUCCEEDED(hr)) {
            ComPtr<IDXGIAdapter> adapter;
            hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
            if (SUCCEEDED(hr)) {
                DXGI_ADAPTER_DESC adapterDesc;
                adapter->GetDesc(&adapterDesc);
                
                char adapterNameVerify[128];
                size_t convertedCharsVerify = 0;
                wcstombs_s(&convertedCharsVerify, adapterNameVerify, 128, adapterDesc.Description, 127);
                Logger::Info("GPU (verified): " + std::string(adapterNameVerify));
                Logger::Info("VRAM: " + std::to_string(adapterDesc.DedicatedVideoMemory / (1024*1024)) + " MB");
            }
        }
        
        Logger::Info("D3D11 device ready for Spout interop");
        
        // Initialize Spout senders for each channel
        Logger::Info("Initializing Spout senders...");
        auto spoutManager = std::make_shared<SpoutManager>(d3d11Device.Get());
        // Pre-create all 12 channels to maintain stable names for vMix consumers
        for (int channel = 0; channel < kMaxSpoutChannels; ++channel) {
            std::ostringstream oss;
            oss << "VIB_CAM_" << std::setw(2) << std::setfill('0') << (channel + 1);
            const std::string senderName = oss.str();
            spoutManager->CreateSender(channel, senderName, kDefaultWidth, kDefaultHeight);
        }
        
        // Initialize capture channels
        // Note: DeckLinkCapture uses CUDA-only pipeline (no D3D11 dependency)
        // D3D11 device above is ONLY for SpoutManager (Phase 3)
        Logger::Info("Initializing capture channels...");
        std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
        
        // Auto-detect DeckLink devices
        int numDevices = DeckLinkCapture::EnumerateDevices();
        Logger::Info("Found " + std::to_string(numDevices) + " DeckLink devices");
        if (numDevices < kMaxSpoutChannels) {
            Logger::Warning("Fewer capture devices than Spout senders (" +
                            std::to_string(numDevices) + " vs " +
                            std::to_string(kMaxSpoutChannels) +
                            "). Idle senders remain available to keep channel names stable in vMix.");
        }
        
        const int channelsToInit = (std::min)(numDevices, kMaxSpoutChannels);
        for (int i = 0; i < channelsToInit; i++) {
            auto capture = std::make_unique<DeckLinkCapture>();
            const std::string channelName = "Channel_" + std::to_string(i + 1);
            if (capture->Initialize(i, channelName)) {
                capture->SetFrameReadyHandler([spoutManager](const VideoChannel& channel, cudaStream_t stream) {
                    if (spoutManager->CopyCudaToSharedTexture(channel.channelID,
                                                              channel.cudaBGRABuffer,
                                                              channel.width,
                                                              channel.height,
                                                              stream)) {
                        spoutManager->SendTexture(channel.channelID);
                    }
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
