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
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "../capture/DeckLinkCapture.h"
#include "../spout/SpoutManager.h"
#include "../ai/YOLOProcessor.h"
#include "../redis/RedisWorker.h"
#include "../utils/Logger.h"

// Link DirectX libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main(int argc, char* argv[]) {
    Logger::Init("VIB_System");
    Logger::Info("Visual Intelligence Bypass v2.0 Starting...");
    
    try {
        // Initialize DirectX 11 Device (for Spout output only)
        // Note: DeckLinkCapture uses CUDA-based zero-copy, not D3D11
        Logger::Info("Initializing DirectX 11 for Spout output...");
        
        ID3D11Device* d3d11Device = nullptr;
        ID3D11DeviceContext* d3d11Context = nullptr;
        
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
        HRESULT hr = D3D11CreateDevice(
            nullptr,                    // Use default adapter (RTX 5080)
            D3D_DRIVER_TYPE_HARDWARE,   // Hardware acceleration
            nullptr,                    // No software module
            createDeviceFlags,          // BGRA support + debug flag in debug builds
            featureLevels,              // Feature levels to try
            _countof(featureLevels),    // Number of feature levels
            D3D11_SDK_VERSION,          // SDK version
            &d3d11Device,               // Output device
            &featureLevel,              // Actual feature level
            &d3d11Context               // Output context
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
        IDXGIDevice* dxgiDevice = nullptr;
        hr = d3d11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
        if (SUCCEEDED(hr)) {
            IDXGIAdapter* adapter = nullptr;
            hr = dxgiDevice->GetAdapter(&adapter);
            if (SUCCEEDED(hr)) {
                DXGI_ADAPTER_DESC adapterDesc;
                adapter->GetDesc(&adapterDesc);
                
                // Convert wide string to regular string for logging
                char adapterName[128];
                size_t convertedChars = 0;
                wcstombs_s(&convertedChars, adapterName, 128, adapterDesc.Description, 127);
                Logger::Info("GPU: " + std::string(adapterName));
                Logger::Info("VRAM: " + std::to_string(adapterDesc.DedicatedVideoMemory / (1024*1024)) + " MB");
                
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        
        Logger::Info("D3D11 device ready for Spout interop");
        
        // Initialize capture channels
        // Note: DeckLinkCapture uses CUDA-only pipeline (no D3D11 dependency)
        // D3D11 device above is ONLY for SpoutManager (Phase 3)
        Logger::Info("Initializing capture channels...");
        std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
        
        // Auto-detect DeckLink devices
        int numDevices = DeckLinkCapture::EnumerateDevices();
        Logger::Info("Found " + std::to_string(numDevices) + " DeckLink devices");
        
        // TODO: Create DeckLinkCapture instances for each device
        // Example:
        // for (int i = 0; i < numDevices; i++) {
        //     auto capture = std::make_unique<DeckLinkCapture>();
        //     if (capture->Initialize(i, "Channel_" + std::to_string(i+1))) {
        //         capture->Start();
        //         captureChannels.push_back(std::move(capture));
        //     }
        // }
        
        // Initialize Spout senders for each channel
        Logger::Info("Initializing Spout senders...");
        SpoutManager spoutManager;
        
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
        
        // Release D3D11 resources
        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }
        
        Logger::Info("Visual Intelligence Bypass shutdown complete");
        
    } catch (const std::exception& e) {
        Logger::Error("Fatal error: " + std::string(e.what()));
        return 1;
    }
    
    return 0;
}
