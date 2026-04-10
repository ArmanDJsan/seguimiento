/**
 * SceneManager.cpp
 * 
 * Implementation of camera routing controller with Auto and Manual modes.
 */

#include "SceneManager.h"
#include "../control/VideoHubClient.h"
#include "../utils/Logger.h"
#include <Windows.h>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace VIB {

SceneManager::SceneManager()
    : m_enabled(false)
    , m_initialized(false)
    , m_mode(SceneMode::AUTO)
    , m_currentConfigIndex(0)
    , m_currentGroup(SceneGroup::G1_G4)
    , m_currentCameraPosition(0)
    , m_activeCamera(1)
    , m_videoHub(nullptr)
    , m_outputIndex(0)
    , m_muteTimeoutMs(200)
    , m_lastRoutingTime(0)
{
    // Default key configuration
    m_keyConfig.toggleMode = 'M';           // M key
    m_keyConfig.configSelect = { VK_F1, VK_F2, VK_F3 };
    m_keyConfig.groupToggle = 'G';          // G key
    m_keyConfig.cameraSelect = { '1', '2', '3', '4' };
}

bool SceneManager::Initialize(const std::vector<SceneConfig>& configs,
                              VideoHubClient* videoHub,
                              int outputIndex)
{
    if (configs.empty()) {
        Logger::Warning("[SceneManager] No configurations provided");
        m_initialized = false;
        return false;
    }

    if (!videoHub) {
        Logger::Warning("[SceneManager] VideoHub client is null");
        m_initialized = false;
        return false;
    }

    m_configs = configs;
    m_videoHub = videoHub;
    m_outputIndex = outputIndex;
    m_initialized = true;

    // Log configuration summary
    Logger::Info("[SceneManager] Initialized with " + std::to_string(configs.size()) + " configurations:");
    for (size_t i = 0; i < configs.size(); ++i) {
        std::ostringstream oss;
        oss << "  Config " << i << " (" << configs[i].name << "): ";
        oss << "g1_g4=[";
        for (size_t j = 0; j < configs[i].g1_g4.size(); ++j) {
            if (j > 0) oss << ",";
            oss << configs[i].g1_g4[j];
        }
        oss << "] g5_g8=[";
        for (size_t j = 0; j < configs[i].g5_g8.size(); ++j) {
            if (j > 0) oss << ",";
            oss << configs[i].g5_g8[j];
        }
        oss << "] threshold=" << configs[i].triggerThreshold;
        Logger::Info(oss.str());
    }

    return true;
}

void SceneManager::SetMode(SceneMode mode) {
    if (m_mode != mode) {
        m_mode = mode;
        std::string modeName = (mode == SceneMode::AUTO) ? "AUTO" : "MANUAL";
        Logger::Info("[SceneManager] Mode changed to: " + modeName);
    }
}

void SceneManager::SelectConfig(int configIndex) {
    if (configIndex < 0 || configIndex >= static_cast<int>(m_configs.size())) {
        Logger::Warning("[SceneManager] Invalid config index: " + std::to_string(configIndex));
        return;
    }

    if (m_currentConfigIndex != configIndex) {
        m_currentConfigIndex = configIndex;
        Logger::Info("[SceneManager] Config changed to: " + m_configs[configIndex].name);
        
        // Reset camera position to 0 when changing config
        m_currentCameraPosition = 0;
        ApplyCameraSelection();
    }
}

void SceneManager::SelectGroup(SceneGroup group) {
    if (m_currentGroup != group) {
        m_currentGroup = group;
        std::string groupName = (group == SceneGroup::G1_G4) ? "G1_G4" : "G5_G8";
        Logger::Info("[SceneManager] Group changed to: " + groupName);
        
        // Reset camera position when changing group
        m_currentCameraPosition = 0;
        ApplyCameraSelection();
    }
}

void SceneManager::ToggleGroup() {
    SceneGroup newGroup = (m_currentGroup == SceneGroup::G1_G4) 
                          ? SceneGroup::G5_G8 
                          : SceneGroup::G1_G4;
    SelectGroup(newGroup);
}

void SceneManager::SelectCameraInGroup(int position) {
    const auto& cameras = GetCurrentGroupCameras();
    
    if (position < 0 || position >= static_cast<int>(cameras.size())) {
        Logger::Warning("[SceneManager] Invalid camera position: " + std::to_string(position) +
                       " (group has " + std::to_string(cameras.size()) + " cameras)");
        return;
    }

    if (m_currentCameraPosition != position) {
        m_currentCameraPosition = position;
        ApplyCameraSelection();
    }
}

void SceneManager::UpdateFromDetection(float ballPosition) {
    if (m_mode != SceneMode::AUTO) {
        return;  // Ignore detection updates in manual mode
    }

    // Find appropriate config based on ball position
    int newConfigIndex = 0;
    for (int i = static_cast<int>(m_configs.size()) - 1; i >= 0; --i) {
        if (ballPosition >= m_configs[i].triggerThreshold) {
            newConfigIndex = i;
            break;
        }
    }

    if (newConfigIndex != m_currentConfigIndex) {
        m_currentConfigIndex = newConfigIndex;
        Logger::Info("[SceneManager] Auto-switched to config: " + m_configs[newConfigIndex].name +
                    " (ball position: " + std::to_string(ballPosition) + ")");
        ApplyCameraSelection();
    }
}

bool SceneManager::ProcessKeyInput(int vkCode) {
    if (!m_enabled || !m_initialized) {
        return false;
    }

    // Toggle mode (M key)
    if (vkCode == m_keyConfig.toggleMode) {
        SetMode((m_mode == SceneMode::AUTO) ? SceneMode::MANUAL : SceneMode::AUTO);
        return true;
    }

    // Manual mode only controls
    if (m_mode == SceneMode::MANUAL) {
        // Config selection (F1, F2, F3)
        for (size_t i = 0; i < m_keyConfig.configSelect.size() && i < m_configs.size(); ++i) {
            if (vkCode == m_keyConfig.configSelect[i]) {
                SelectConfig(static_cast<int>(i));
                return true;
            }
        }

        // Group toggle (G key)
        if (vkCode == m_keyConfig.groupToggle) {
            ToggleGroup();
            return true;
        }

        // Camera selection (1-4)
        for (size_t i = 0; i < m_keyConfig.cameraSelect.size(); ++i) {
            if (vkCode == m_keyConfig.cameraSelect[i]) {
                SelectCameraInGroup(static_cast<int>(i));
                return true;
            }
        }
    }

    return false;
}

std::vector<int> SceneManager::GetCurrentGroupCameras() const {
    if (m_currentConfigIndex < 0 || m_currentConfigIndex >= static_cast<int>(m_configs.size())) {
        return {};
    }

    const auto& config = m_configs[m_currentConfigIndex];
    return (m_currentGroup == SceneGroup::G1_G4) ? config.g1_g4 : config.g5_g8;
}

int SceneManager::GetCameraFromGroupPosition(int position) const {
    const auto& cameras = GetCurrentGroupCameras();
    if (position >= 0 && position < static_cast<int>(cameras.size())) {
        return cameras[position];
    }
    return 1;  // Default to camera 1
}

void SceneManager::ApplyCameraSelection() {
    int cameraId = GetCameraFromGroupPosition(m_currentCameraPosition);
    
    if (cameraId != m_activeCamera) {
        if (SendVideoHubRouting(cameraId)) {
            m_activeCamera = cameraId;
        }
    }
}

bool SceneManager::SendVideoHubRouting(int cameraId) {
    if (!m_videoHub) {
        Logger::Warning("[SceneManager] VideoHub not connected");
        return false;
    }

    // Rate limiting using mute timeout
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    
    if (nowMs - m_lastRoutingTime < static_cast<uint64_t>(m_muteTimeoutMs)) {
        Logger::Debug("[SceneManager] Routing muted (timeout not elapsed)");
        return false;
    }

    // Retry logic: 3 attempts with 50ms delays
    const int maxRetries = 3;
    const int retryDelayMs = 50;

    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        // VideoHub uses 0-based indexing, camera IDs are 1-based
        int sourceIndex = cameraId - 1;
        
        if (m_videoHub->RouteInputToOutput(m_outputIndex, sourceIndex)) {
            m_lastRoutingTime = nowMs;
            Logger::Info("[SceneManager] Routed CAM_" + 
                        std::to_string(cameraId) + 
                        " to output " + std::to_string(m_outputIndex) +
                        " (attempt " + std::to_string(attempt) + ")");
            return true;
        }

        if (attempt < maxRetries) {
            Logger::Warning("[SceneManager] Routing attempt " + std::to_string(attempt) + 
                           " failed, retrying in " + std::to_string(retryDelayMs) + "ms...");
            Sleep(retryDelayMs);
        }
    }

    Logger::Error("[SceneManager] Failed to route CAM_" + std::to_string(cameraId) +
                 " after " + std::to_string(maxRetries) + " attempts");
    return false;
}

std::string SceneManager::GetStatusString() const {
    if (!m_enabled) {
        return "[SceneManager DISABLED]";
    }
    if (!m_initialized) {
        return "[SceneManager NOT INITIALIZED]";
    }

    std::ostringstream oss;
    
    // Mode indicator
    oss << "╔═══════════════════════════════════════════════════════╗\n";
    oss << "║  SCENE MANAGER                                        ║\n";
    oss << "╠═══════════════════════════════════════════════════════╣\n";
    
    // Mode
    std::string modeName = (m_mode == SceneMode::AUTO) ? "AUTO" : "MANUAL";
    oss << "║  Modo: " << std::left << std::setw(10) << modeName;
    oss << "                   [M] para cambiar     ║\n";
    
    // Config
    std::string configName = (m_currentConfigIndex < static_cast<int>(m_configs.size())) 
                             ? m_configs[m_currentConfigIndex].name 
                             : "???";
    oss << "║  Config: " << std::left << std::setw(12) << configName;
    oss << " (F" << (m_currentConfigIndex + 1) << ")";
    oss << "           [F1-F3] para cambiar ║\n";
    
    // Group
    std::string groupName = (m_currentGroup == SceneGroup::G1_G4) ? "g1_g4" : "g5_g8";
    oss << "║  Grupo: " << std::left << std::setw(13) << groupName;
    oss << "                   [G] para cambiar     ║\n";
    
    // Cameras in group
    const auto& cameras = GetCurrentGroupCameras();
    oss << "║  Camaras: [";
    for (size_t i = 0; i < cameras.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << cameras[i];
    }
    oss << "]";
    // Pad to fit box
    size_t camStrLen = 12;  // "Camaras: [" base length
    for (size_t i = 0; i < cameras.size(); ++i) {
        camStrLen += (i > 0 ? 2 : 0) + std::to_string(cameras[i]).length();
    }
    camStrLen += 1;  // closing bracket
    for (size_t i = camStrLen; i < 55; ++i) oss << " ";
    oss << "║\n";
    
    // Active camera
    oss << "║  Activa: CAM_" << std::setw(2) << std::setfill('0') << m_activeCamera;
    oss << std::setfill(' ');
    oss << "                  [1-4] para cambiar   ║\n";
    
    oss << "╚═══════════════════════════════════════════════════════╝";
    
    return oss.str();
}

} // namespace VIB
