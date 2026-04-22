/**
 * SceneManager.cpp
 * 
 * Implementation of dynamic camera group switching
 */

#include "SceneManager.h"
#include "../control/VideoHubClient.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

SceneManager::SceneManager(VideoHubClient* videoHub, const SceneManagerConfig& config)
    : m_videoHub(videoHub)
    , m_config(config)
    , m_currentConfigIndex(0)
    , m_lastLeaderX(0.0f)
    , m_lastLeaderY(0.0f)
    , m_initialized(false)
    , m_mode(config.mode)
    , m_triggerMode(config.triggerMode)
    , m_manualKeysConfig(config.manualKeys)
    , m_lastEventTime(0)
    , m_lastEventConfig("")
{
    // Initialize slot assignments
    for (int i = 0; i < kStreamingSlots; ++i) {
        m_slotAssignments[i].slotIndex = i;
        m_slotAssignments[i].cameraID = i + 1;  // Default: slot i gets camera i+1
        m_slotAssignments[i].isMuted = false;
        m_slotAssignments[i].muteEndTime = 0;
    }
    
    // Initialize manual state with defaults
    m_manualState.activeConfigIndex = 0;
    m_manualState.activeGroup = ActiveGroup::G1_G4;
    m_manualState.selectedCameraInGroup = 0;
}

SceneManager::~SceneManager() = default;

bool SceneManager::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled) {
        Logger::Info("SceneManager: Disabled by configuration");
        m_initialized = true;
        return true;
    }
    
    if (!m_videoHub) {
        Logger::Error("SceneManager: VideoHub client is null");
        return false;
    }
    
    if (!m_videoHub->IsConnected()) {
        Logger::Warning("SceneManager: VideoHub not connected, will apply config when available");
    }
    
    // Validate configuration
    if (m_config.groups.empty()) {
        Logger::Error("SceneManager: No group configurations defined");
        return false;
    }
    
    // Validate each group configuration
    bool hasValidationErrors = false;
    for (size_t i = 0; i < m_config.groups.size(); ++i) {
        const auto& group = m_config.groups[i];
        
        // Check camera IDs in G1-G4
        for (int j = 0; j < 4; ++j) {
            int cameraID = group.slotsG1_G4[j];
            if (cameraID < 1 || cameraID > kMaxCameras) {
                Logger::Error("SceneManager: Group '" + group.configName + 
                             "' has invalid camera ID " + std::to_string(cameraID) + 
                             " in slot G" + std::to_string(j + 1) + " (must be 1-" + 
                             std::to_string(kMaxCameras) + ")");
                hasValidationErrors = true;
            }
        }
        
        // Check camera IDs in G5-G8
        for (int j = 0; j < 4; ++j) {
            int cameraID = group.slotsG5_G8[j];
            if (cameraID < 1 || cameraID > kMaxCameras) {
                Logger::Error("SceneManager: Group '" + group.configName + 
                             "' has invalid camera ID " + std::to_string(cameraID) + 
                             " in slot G" + std::to_string(j + 5) + " (must be 1-" + 
                             std::to_string(kMaxCameras) + ")");
                hasValidationErrors = true;
            }
        }
    }
    
    if (hasValidationErrors) {
        Logger::Error("SceneManager: Configuration validation failed, cannot initialize");
        return false;
    }
    
    // Log configuration summary
    Logger::Info("SceneManager: Configuration summary:");
    std::string triggerModeStr = (m_config.triggerMode == TriggerMode::THRESHOLD) ? "THRESHOLD" : "EVENT";
    Logger::Info("SceneManager: Trigger mode: " + triggerModeStr);
    for (size_t i = 0; i < m_config.groups.size(); ++i) {
        const auto& group = m_config.groups[i];
        std::ostringstream oss;
        oss << "  Group " << i << " ('" << group.configName << "'): "
            << "G1-G4=[CAM_" << std::setw(2) << std::setfill('0') << group.slotsG1_G4[0] 
            << ",CAM_" << std::setw(2) << std::setfill('0') << group.slotsG1_G4[1]
            << ",CAM_" << std::setw(2) << std::setfill('0') << group.slotsG1_G4[2]
            << ",CAM_" << std::setw(2) << std::setfill('0') << group.slotsG1_G4[3] << "], "
            << "G5-G8=[CAM_" << std::setw(2) << std::setfill('0') << group.slotsG5_G8[0]
            << ",CAM_" << std::setw(2) << std::setfill('0') << group.slotsG5_G8[1]
            << ",CAM_" << std::setw(2) << std::setfill('0') << group.slotsG5_G8[2]
            << ",CAM_" << std::setw(2) << std::setfill('0') << group.slotsG5_G8[3] << "], "
            << "trigger=" << group.triggerThreshold << "m";
        Logger::Info(oss.str());
    }
    
    // Apply initial configuration
    ApplyGroupConfig(0);
    
    m_initialized = true;
    Logger::Info("SceneManager: Initialized with " + std::to_string(m_config.groups.size()) + 
                 " group configurations, mute timeout=" + std::to_string(m_config.muteTimeoutMs) + "ms");
    
    return true;
}

void SceneManager::UpdateLeaderPosition(float Xg, float Yg) {
    if (!m_config.enabled || !m_initialized) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_lastLeaderX = Xg;
    m_lastLeaderY = Yg;
    
    // In MANUAL mode, ignore leader position updates
    if (m_mode == SceneMode::MANUAL) {
        return;
    }
    
    // In EVENT trigger mode, leader position is handled by ZoneChecker/EventGenerator
    if (m_triggerMode == TriggerMode::EVENT) {
        // Just update position, events will trigger changes
        ProcessMuteTimeoutsInternal();
        return;
    }
    
    // THRESHOLD mode: Evaluate if we need to switch configurations
    int desiredConfig = EvaluateDesiredConfig();
    
    if (desiredConfig != m_currentConfigIndex.load()) {
        Logger::Info("SceneManager: Leader at X=" + std::to_string(Xg) + 
                     ", triggering switch from " + m_config.groups[m_currentConfigIndex].configName +
                     " to " + m_config.groups[desiredConfig].configName);
        ApplyGroupConfig(desiredConfig);
    }
    
    // Process any pending mute timeouts (use internal version since we already hold the mutex)
    ProcessMuteTimeoutsInternal();
}

bool SceneManager::TriggerGroupSwitch(int configIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (configIndex < 0 || static_cast<size_t>(configIndex) >= m_config.groups.size()) {
        Logger::Error("SceneManager: Invalid config index: " + std::to_string(configIndex));
        return false;
    }
    
    if (configIndex == m_currentConfigIndex.load()) {
        Logger::Debug("SceneManager: Already in config " + m_config.groups[configIndex].configName);
        return true;
    }
    
    ApplyGroupConfig(configIndex);
    return true;
}

void SceneManager::ProcessMuteTimeouts() {
    // Acquire mutex and delegate to internal implementation
    std::lock_guard<std::mutex> lock(m_mutex);
    ProcessMuteTimeoutsInternal();
}

void SceneManager::ProcessMuteTimeoutsInternal() {
    // NOTE: This method assumes m_mutex is already held by the caller.
    // Do NOT acquire the mutex here to avoid recursive lock deadlock.
    
    int64_t now = GetCurrentTimeMs();
    
    for (int i = 0; i < kStreamingSlots; ++i) {
        auto& slot = m_slotAssignments[i];
        if (slot.isMuted && now >= slot.muteEndTime) {
            // Unmute slot - directly modify here since we hold the mutex
            slot.isMuted = false;
            slot.muteEndTime = 0;
            
            Logger::Debug("SceneManager: Unmuted slot G" + std::to_string(i + 1));
            
            if (m_muteCallback) {
                m_muteCallback(i, false);
            }
        }
    }
}

std::vector<int> SceneManager::GetActiveSlots() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<int> active;
    for (int i = 0; i < kStreamingSlots; ++i) {
        if (!m_slotAssignments[i].isMuted) {
            active.push_back(i);
        }
    }
    return active;
}

std::vector<int> SceneManager::GetMutedSlots() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    std::vector<int> muted;
    for (int i = 0; i < kStreamingSlots; ++i) {
        if (m_slotAssignments[i].isMuted) {
            muted.push_back(i);
        }
    }
    return muted;
}

bool SceneManager::IsSlotMuted(int slotIndex) const {
    if (slotIndex < 0 || slotIndex >= kStreamingSlots) {
        return true;  // Invalid slots are effectively muted
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slotAssignments[slotIndex].isMuted;
}

int SceneManager::GetSlotCamera(int slotIndex) const {
    if (slotIndex < 0 || slotIndex >= kStreamingSlots) {
        return 0;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_slotAssignments[slotIndex].cameraID;
}

std::string SceneManager::GetCurrentConfigName() const {
    int idx = m_currentConfigIndex.load();
    if (idx >= 0 && static_cast<size_t>(idx) < m_config.groups.size()) {
        return m_config.groups[idx].configName;
    }
    return "unknown";
}

void SceneManager::ApplyGroupConfig(int configIndex) {
    if (configIndex < 0 || static_cast<size_t>(configIndex) >= m_config.groups.size()) {
        return;
    }
    
    const auto& config = m_config.groups[configIndex];
    
    Logger::Info("SceneManager: Applying group configuration '" + config.configName + "'");
    
    // Determine which slots need to change
    std::vector<int> slotsToSwitch;
    
    // Check G1-G4 (slots 0-3)
    for (int i = 0; i < 4; ++i) {
        int newCamera = config.slotsG1_G4[i];
        if (m_slotAssignments[i].cameraID != newCamera) {
            slotsToSwitch.push_back(i);
            m_slotAssignments[i].cameraID = newCamera;
        }
    }
    
    // Check G5-G8 (slots 4-7)
    for (int i = 0; i < 4; ++i) {
        int newCamera = config.slotsG5_G8[i];
        if (m_slotAssignments[i + 4].cameraID != newCamera) {
            slotsToSwitch.push_back(i + 4);
            m_slotAssignments[i + 4].cameraID = newCamera;
        }
    }
    
    // Send routing commands and mute switching slots
    for (int slotIdx : slotsToSwitch) {
        MuteSlot(slotIdx);
        SendVideoHubRouting(slotIdx, m_slotAssignments[slotIdx].cameraID);
    }
    
    m_currentConfigIndex.store(configIndex);
    
    // Log summary
    std::ostringstream oss;
    oss << "SceneManager: Config '" << config.configName << "' applied. "
        << "Switched " << slotsToSwitch.size() << " slots. "
        << "G1-G4=[" << config.slotsG1_G4[0] << "," << config.slotsG1_G4[1] << ","
        << config.slotsG1_G4[2] << "," << config.slotsG1_G4[3] << "], "
        << "G5-G8=[" << config.slotsG5_G8[0] << "," << config.slotsG5_G8[1] << ","
        << config.slotsG5_G8[2] << "," << config.slotsG5_G8[3] << "]";
    Logger::Info(oss.str());
    
    // Apply radar routing to ensure radars are always mapped correctly
    ApplyRadarRouting();
}

void SceneManager::ApplyRadarRouting() {
    if (!m_config.radarRoutingEnabled) {
        return;
    }
    
    if (!m_videoHub || !m_videoHub->IsConnected()) {
        Logger::Warning("SceneManager: Cannot apply radar routing - VideoHub not connected");
        return;
    }
    
    Logger::Debug("SceneManager: Applying radar routing...");
    
    // Route RADAR_01-04 to their configured output slots
    int successCount = 0;
    
    for (int i = 0; i < kRadarCount; ++i) {
        int outputSlot = m_config.radarOutputSlots[i];
        if (SendVideoHubRoutingByName(outputSlot, kRadarNames[i])) {
            successCount++;
        }
    }
    
    if (successCount == kRadarCount) {
        std::ostringstream oss;
        oss << "SceneManager: Radar routing applied - "
            << kRadarNames[0] << "->Out" << m_config.radarOutputSlots[0] << ", "
            << kRadarNames[1] << "->Out" << m_config.radarOutputSlots[1] << ", "
            << kRadarNames[2] << "->Out" << m_config.radarOutputSlots[2] << ", "
            << kRadarNames[3] << "->Out" << m_config.radarOutputSlots[3];
        Logger::Info(oss.str());
    } else {
        Logger::Warning("SceneManager: Radar routing partially failed (" + 
                       std::to_string(successCount) + "/" + std::to_string(kRadarCount) + " succeeded)");
    }
}

bool SceneManager::SendVideoHubRoutingByName(int outputIndex, const std::string& inputName) {
    if (!m_videoHub || !m_videoHub->IsConnected()) {
        Logger::Error("SceneManager: Cannot send routing command - VideoHub not connected (" + 
                     inputName + " -> Out" + std::to_string(outputIndex) + ")");
        return false;
    }
    
    bool success = false;
    for (int attempt = 0; attempt < kRoutingMaxRetries; ++attempt) {
        success = m_videoHub->RouteInputToOutput(outputIndex, inputName);
        
        if (success) {
            if (attempt > 0) {
                Logger::Info("SceneManager: Successfully routed " + inputName + 
                           " to output " + std::to_string(outputIndex) + 
                           " on retry attempt " + std::to_string(attempt + 1));
            } else {
                Logger::Debug("SceneManager: Routed " + inputName + 
                            " to output " + std::to_string(outputIndex));
            }
            return true;
        }
        
        // If not the last attempt, wait before retrying
        if (attempt < kRoutingMaxRetries - 1) {
            Logger::Warning("SceneManager: Routing attempt " + std::to_string(attempt + 1) + 
                          " failed for " + inputName + " to output " + 
                          std::to_string(outputIndex) + ", retrying...");
            std::this_thread::sleep_for(std::chrono::milliseconds(kRoutingRetryDelayMs));
        }
    }
    
    Logger::Error("SceneManager: Failed to route " + inputName + 
                 " to output " + std::to_string(outputIndex) + 
                 " after " + std::to_string(kRoutingMaxRetries) + " attempts.");
    
    return false;
}

bool SceneManager::SendVideoHubRouting(int slotIndex, int cameraID) {
    if (!m_videoHub || !m_videoHub->IsConnected()) {
        std::ostringstream errOss;
        errOss << "SceneManager: Cannot send routing command - VideoHub not connected (slot G" 
               << (slotIndex + 1) << " -> CAM_" << std::setw(2) << std::setfill('0') << cameraID << ")";
        Logger::Error(errOss.str());
        return false;
    }
    
    // Build camera name (CAM_01, CAM_02, etc.)
    std::ostringstream oss;
    oss << "CAM_" << std::setw(2) << std::setfill('0') << cameraID;
    std::string cameraName = oss.str();
    
    bool success = false;
    for (int attempt = 0; attempt < kRoutingMaxRetries; ++attempt) {
        // VideoHub output index = slotIndex (0-based)
        success = m_videoHub->RouteInputToOutput(slotIndex, cameraName);
        
        if (success) {
            if (attempt > 0) {
                Logger::Info("SceneManager: Successfully routed " + cameraName + 
                           " to slot G" + std::to_string(slotIndex + 1) + 
                           " on retry attempt " + std::to_string(attempt + 1));
            } else {
                Logger::Debug("SceneManager: Routed " + cameraName + 
                            " to slot G" + std::to_string(slotIndex + 1));
            }
            return true;
        }
        
        // If not the last attempt, wait before retrying
        if (attempt < kRoutingMaxRetries - 1) {
            Logger::Warning("SceneManager: Routing attempt " + std::to_string(attempt + 1) + 
                          " failed for " + cameraName + " to slot G" + 
                          std::to_string(slotIndex + 1) + ", retrying...");
            std::this_thread::sleep_for(std::chrono::milliseconds(kRoutingRetryDelayMs));
        }
    }
    
    // All retries failed
    Logger::Error("SceneManager: Failed to route " + cameraName + 
                 " to slot G" + std::to_string(slotIndex + 1) + 
                 " after " + std::to_string(kRoutingMaxRetries) + " attempts. Check VideoHub connection.");
    
    return false;
}

void SceneManager::MuteSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= kStreamingSlots) {
        return;
    }
    
    auto& slot = m_slotAssignments[slotIndex];
    slot.isMuted = true;
    slot.muteEndTime = GetCurrentTimeMs() + m_config.muteTimeoutMs;
    
    Logger::Debug("SceneManager: Muted slot G" + std::to_string(slotIndex + 1) + 
                  " for " + std::to_string(m_config.muteTimeoutMs) + "ms");
    
    if (m_muteCallback) {
        m_muteCallback(slotIndex, true);
    }
}

void SceneManager::UnmuteSlot(int slotIndex) {
    if (slotIndex < 0 || slotIndex >= kStreamingSlots) {
        return;
    }
    
    auto& slot = m_slotAssignments[slotIndex];
    
    if (slot.isMuted) {
        slot.isMuted = false;
        slot.muteEndTime = 0;
        
        Logger::Debug("SceneManager: Unmuted slot G" + std::to_string(slotIndex + 1));
        
        if (m_muteCallback) {
            m_muteCallback(slotIndex, false);
        }
    }
}

int64_t SceneManager::GetCurrentTimeMs() const {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

int SceneManager::EvaluateDesiredConfig() const {
    // Find the highest-threshold config that the leader has passed
    int desiredConfig = 0;
    
    for (size_t i = 0; i < m_config.groups.size(); ++i) {
        if (m_lastLeaderX >= m_config.groups[i].triggerThreshold) {
            desiredConfig = static_cast<int>(i);
        }
    }
    
    return desiredConfig;
}

void SceneManager::SetMode(SceneMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_mode == mode) {
        return;  // Already in this mode
    }
    
    SceneMode oldMode = m_mode;
    m_mode = mode;
    
    std::string modeStr = (mode == SceneMode::AUTO) ? "AUTO" : "MANUAL";
    std::string oldModeStr = (oldMode == SceneMode::AUTO) ? "AUTO" : "MANUAL";
    
    Logger::Info("SceneManager: Mode changed from " + oldModeStr + " to " + modeStr);
    
    // When switching to MANUAL, sync manual state with current configuration
    if (mode == SceneMode::MANUAL) {
        m_manualState.activeConfigIndex = m_currentConfigIndex.load();
        Logger::Info("SceneManager: MANUAL mode initialized with config " + 
                    m_config.groups[m_manualState.activeConfigIndex].configName);
    }
}

bool SceneManager::SelectConfig(int configIndex) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Validate config index
    if (configIndex < 0 || static_cast<size_t>(configIndex) >= m_config.groups.size()) {
        Logger::Error("SceneManager: Invalid config index " + std::to_string(configIndex));
        return false;
    }
    
    // Warn if called in AUTO mode
    if (m_mode == SceneMode::AUTO) {
        Logger::Warning("SceneManager: SelectConfig called in AUTO mode, ignoring");
        return false;
    }
    
    // Update manual state
    m_manualState.activeConfigIndex = configIndex;
    
    // Apply the configuration (all 8 cameras)
    ApplyGroupConfig(configIndex);
    
    Logger::Info("SceneManager: [MANUAL] Selected config " + 
                m_config.groups[configIndex].configName);
    
    return true;
}

void SceneManager::SelectGroup(ActiveGroup group) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_mode == SceneMode::AUTO) {
        Logger::Warning("SceneManager: SelectGroup called in AUTO mode, ignoring");
        return;
    }
    
    m_manualState.activeGroup = group;
    
    std::string groupStr = (group == ActiveGroup::G1_G4) ? "G1_G4" : "G5_G8";
    Logger::Info("SceneManager: [MANUAL] Selected group " + groupStr);
}

void SceneManager::ToggleGroup() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_mode == SceneMode::AUTO) {
        Logger::Warning("SceneManager: ToggleGroup called in AUTO mode, ignoring");
        return;
    }
    
    // Toggle between groups
    if (m_manualState.activeGroup == ActiveGroup::G1_G4) {
        m_manualState.activeGroup = ActiveGroup::G5_G8;
        Logger::Info("SceneManager: [MANUAL] Toggled to group G5_G8");
    } else {
        m_manualState.activeGroup = ActiveGroup::G1_G4;
        Logger::Info("SceneManager: [MANUAL] Toggled to group G1_G4");
    }
}

bool SceneManager::SelectCameraInGroup(int index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Validate index
    if (index < 0 || index > 3) {
        Logger::Error("SceneManager: Invalid camera index " + std::to_string(index) + 
                     " (must be 0-3)");
        return false;
    }
    
    if (m_mode == SceneMode::AUTO) {
        Logger::Warning("SceneManager: SelectCameraInGroup called in AUTO mode, ignoring");
        return false;
    }
    
    // Update manual state
    m_manualState.selectedCameraInGroup = index;
    
    // Calculate actual camera ID (no lock needed, we already hold it)
    int cameraID = 0;
    if (m_manualState.activeConfigIndex >= 0 && 
        static_cast<size_t>(m_manualState.activeConfigIndex) < m_config.groups.size() &&
        m_manualState.selectedCameraInGroup >= 0 && m_manualState.selectedCameraInGroup <= 3) {
        const auto& cfg = m_config.groups[m_manualState.activeConfigIndex];
        if (m_manualState.activeGroup == ActiveGroup::G1_G4) {
            cameraID = cfg.slotsG1_G4[m_manualState.selectedCameraInGroup];
        } else {
            cameraID = cfg.slotsG5_G8[m_manualState.selectedCameraInGroup];
        }
    }
    
    std::string groupStr = (m_manualState.activeGroup == ActiveGroup::G1_G4) ? "G1_G4" : "G5_G8";
    std::ostringstream oss;
    oss << "SceneManager: [MANUAL] Selected camera " << index 
        << " in group " << groupStr << " (CAM_" 
        << std::setw(2) << std::setfill('0') << cameraID << ")";
    Logger::Info(oss.str());
    
    return true;
}

ManualState SceneManager::GetManualState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_manualState;
}

int SceneManager::GetActiveCameraID() const {
    // This method assumes mutex is already held by caller in most cases,
    // but we'll lock it to be safe for external callers
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Validate state
    if (m_manualState.activeConfigIndex < 0 || 
        static_cast<size_t>(m_manualState.activeConfigIndex) >= m_config.groups.size()) {
        return 0;  // Invalid
    }
    
    if (m_manualState.selectedCameraInGroup < 0 || m_manualState.selectedCameraInGroup > 3) {
        return 0;  // Invalid
    }
    
    const auto& config = m_config.groups[m_manualState.activeConfigIndex];
    
    // Get camera ID based on active group
    if (m_manualState.activeGroup == ActiveGroup::G1_G4) {
        return config.slotsG1_G4[m_manualState.selectedCameraInGroup];
    } else {
        return config.slotsG5_G8[m_manualState.selectedCameraInGroup];
    }
}

bool SceneManager::OnEvent(int eventType, const std::string& configName, 
                            float confidence, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled || !m_initialized) {
        return false;
    }
    
    // In MANUAL mode, ignore events
    if (m_mode == SceneMode::MANUAL) {
        Logger::Debug("SceneManager: OnEvent ignored - MANUAL mode");
        return false;
    }
    
    // Check cooldown
    if (timestamp == 0) {
        timestamp = GetCurrentTimeMs();
    }
    
    if (IsEventOnCooldown(timestamp)) {
        Logger::Debug("SceneManager: OnEvent skipped - on cooldown");
        return false;
    }
    
    // Find config index by name
    int configIndex = FindConfigIndexByName(configName);
    if (configIndex < 0) {
        Logger::Warning("SceneManager: OnEvent - unknown config '" + configName + "'");
        return false;
    }
    
    // Check if already in this config
    if (configIndex == m_currentConfigIndex.load()) {
        Logger::Debug("SceneManager: OnEvent - already in config '" + configName + "'");
        return false;
    }
    
    // Log event type
    std::string eventTypeStr;
    switch (eventType) {
        case 0: eventTypeStr = "ZONE_ENTRY"; break;
        case 1: eventTypeStr = "LEADER_DETECTED"; break;
        case 2: eventTypeStr = "EXTERNAL_TRIGGER"; break;
        default: eventTypeStr = "UNKNOWN"; break;
    }
    
    Logger::Info("SceneManager: Processing " + eventTypeStr + " event -> " + configName +
                " (confidence: " + std::to_string(confidence) + ")");
    
    // Apply the configuration
    ApplyGroupConfig(configIndex);
    
    // Update cooldown state
    m_lastEventTime = timestamp;
    m_lastEventConfig = configName;
    
    return true;
}

bool SceneManager::TriggerConfigByName(const std::string& configName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    int configIndex = FindConfigIndexByName(configName);
    if (configIndex < 0) {
        Logger::Error("SceneManager: TriggerConfigByName - unknown config '" + configName + "'");
        return false;
    }
    
    if (configIndex == m_currentConfigIndex.load()) {
        Logger::Debug("SceneManager: Already in config " + configName);
        return true;
    }
    
    Logger::Info("SceneManager: Force switching to config '" + configName + "'");
    ApplyGroupConfig(configIndex);
    return true;
}

void SceneManager::SetTriggerMode(TriggerMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_triggerMode == mode) {
        return;
    }
    
    std::string oldModeStr = (m_triggerMode == TriggerMode::THRESHOLD) ? "THRESHOLD" : "EVENT";
    std::string newModeStr = (mode == TriggerMode::THRESHOLD) ? "THRESHOLD" : "EVENT";
    
    m_triggerMode = mode;
    
    Logger::Info("SceneManager: Trigger mode changed from " + oldModeStr + " to " + newModeStr);
    
    // Reset event state when switching modes
    m_lastEventTime = 0;
    m_lastEventConfig = "";
}

int SceneManager::FindConfigIndexByName(const std::string& configName) const {
    // Note: Caller must hold m_mutex
    for (size_t i = 0; i < m_config.groups.size(); ++i) {
        if (m_config.groups[i].configName == configName) {
            return static_cast<int>(i);
        }
    }
    return -1;  // Not found
}

bool SceneManager::IsEventOnCooldown(int64_t timestamp) const {
    // Note: Caller must hold m_mutex
    if (m_lastEventTime == 0) {
        return false;  // No previous event
    }
    
    int64_t elapsed = timestamp - m_lastEventTime;
    return elapsed < m_config.eventCooldownMs;
}
