/**
 * NDIFallback.cpp
 * 
 * Implementation of NDI fallback for MegaCanvas failure scenarios
 */

#include "NDIFallback.h"
#include "MegaCanvasManager.h"
#include "../utils/Logger.h"
#include "../json.hpp"

#include <fstream>
#include <mutex>

using json = nlohmann::json;

namespace {
    std::once_flag g_initFlag;
    std::unique_ptr<NDIFallback> g_instance;
}

NDIFallback::NDIFallback() {
    Logger::Info("NDIFallback: Created (inactive by default)");
}

NDIFallback::~NDIFallback() {
    Deactivate();
}

bool NDIFallback::IsFallbackEnabled() {
    // Check config.json for fallback_to_ndi setting
    std::ifstream file("config.json");
    if (!file.is_open()) {
        return false;  // Default: no fallback
    }

    try {
        json j;
        file >> j;
        
        if (j.contains("mega_canvas") && j["mega_canvas"].is_object()) {
            auto& mc = j["mega_canvas"];
            if (mc.contains("fallback_to_ndi")) {
                return mc["fallback_to_ndi"].get<bool>();
            }
        }
    } catch (...) {
        // Parsing error, use default
    }

    return false;
}

bool NDIFallback::Activate() {
    if (m_active.load(std::memory_order_acquire)) {
        Logger::Warning("NDIFallback: Already active");
        return true;
    }

    Logger::Warning("NDIFallback: Activating NDI fallback (MegaCanvas unavailable)");

    // Create NDI manager
    m_ndiManager = std::make_unique<NDIManager>();
    
    if (!m_ndiManager->Initialize()) {
        Logger::Error("NDIFallback: Failed to initialize NDI");
        m_ndiManager.reset();
        return false;
    }

    // Create 12 NDI senders (same as before MegaCanvas)
    constexpr int kNumCameras = 12;
    constexpr unsigned int kWidth = 3840;
    constexpr unsigned int kHeight = 2160;

    for (int i = 0; i < kNumCameras; i++) {
        char senderName[32];
        snprintf(senderName, sizeof(senderName), "VIB_CAM_%02d", i + 1);
        
        if (!m_ndiManager->CreateSender(i, senderName, kWidth, kHeight, false)) {
            Logger::Warning("NDIFallback: Failed to create sender for camera " + std::to_string(i));
        }
    }

    m_initialized.store(true, std::memory_order_release);
    m_active.store(true, std::memory_order_release);
    
    Logger::Info("NDIFallback: Activated with " + 
                 std::to_string(m_ndiManager->GetActiveSenderCount()) + " senders");
    
    return true;
}

void NDIFallback::Deactivate() {
    if (!m_active.load(std::memory_order_acquire)) {
        return;
    }

    Logger::Info("NDIFallback: Deactivating");

    m_active.store(false, std::memory_order_release);
    m_initialized.store(false, std::memory_order_release);

    if (m_ndiManager) {
        m_ndiManager->ReleaseAll();
        m_ndiManager.reset();
    }
}

NDIManager* NDIFallback::GetNDIManager() {
    if (!m_active.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return m_ndiManager.get();
}

bool NDIFallback::SendFrame(int cameraID, void* cudaBGRABuffer,
                            unsigned int width, unsigned int height,
                            cudaStream_t stream) {
    if (!m_active.load(std::memory_order_acquire)) {
        return true;  // Not active, silently succeed
    }

    if (!m_ndiManager) {
        return false;
    }

    return m_ndiManager->SendBGRAFrame(cameraID, cudaBGRABuffer, width, height, stream);
}

NDIFallback& GetNDIFallback() {
    std::call_once(g_initFlag, []() {
        g_instance = std::make_unique<NDIFallback>();
    });
    return *g_instance;
}
