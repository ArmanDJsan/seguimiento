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

#include "../capture/DeckLinkCapture.h"
#include "../spout/SpoutManager.h"
#include "../ai/YOLOProcessor.h"
#include "../redis/RedisWorker.h"
#include "../utils/Logger.h"

int main(int argc, char* argv[]) {
    Logger::Init("VIB_System");
    Logger::Info("Visual Intelligence Bypass v2.0 Starting...");
    
    try {
        // Initialize DirectX 11 Device
        Logger::Info("Initializing DirectX 11...");
        // TODO: Initialize D3D11 device and context
        
        // Initialize capture channels
        Logger::Info("Initializing capture channels...");
        std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
        
        // Auto-detect DeckLink devices
        int numDevices = DeckLinkCapture::EnumerateDevices();
        Logger::Info("Found " + std::to_string(numDevices) + " DeckLink devices");
        
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
        
        Logger::Info("Visual Intelligence Bypass shutdown complete");
        
    } catch (const std::exception& e) {
        Logger::Error("Fatal error: " + std::string(e.what()));
        return 1;
    }
    
    return 0;
}
