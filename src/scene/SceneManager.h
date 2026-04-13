/**
 * SceneManager.h
 * 
 * Dynamic camera group switching using leapfrogging strategy
 * Manages 12 streaming cameras over 8 physical slots via VideoHub
 * 
 * Architecture:
 * - G1-G8: 8 capture slots for streaming (routed via VideoHub)
 * - f1-f4: 4 fixed zenith cameras (always connected, no routing)
 * - Leapfrogging: Inactive group rotates ahead based on leader position
 * 
 * Performance target: <10ms for group switch including VideoHub command
 */

#pragma once

#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <cstdint>

// Forward declaration
class VideoHubClient;

/**
 * Scene mode - Auto or Manual operation
 */
enum class SceneMode {
    AUTO,    // Automatic leapfrogging based on leader position
    MANUAL   // Manual control via keyboard
};

/**
 * Active group in manual mode
 */
enum class ActiveGroup {
    G1_G4,   // Cameras in slots G1-G4
    G5_G8    // Cameras in slots G5-G8
};

/**
 * Manual mode state
 */
struct ManualState {
    int activeConfigIndex;           // Active configuration (0-2 for config_a/b/c)
    ActiveGroup activeGroup;         // Active group (G1_G4 or G5_G8)
    int selectedCameraInGroup;       // Selected camera within group (0-3)
    
    ManualState()
        : activeConfigIndex(0)
        , activeGroup(ActiveGroup::G1_G4)
        , selectedCameraInGroup(0) {}
};

/**
 * Manual keys configuration
 */
struct ManualKeysConfig {
    std::string toggleMode;                  // Key to toggle between AUTO/MANUAL (e.g., "M")
    std::array<std::string, 3> configSelect; // Keys for config_a, config_b, config_c (e.g., F1, F6, F7)
    std::string groupSelect;                 // Key to toggle group (e.g., "G")
    std::array<std::string, 4> cameraSelect; // Keys for camera 0-3 in group (e.g., "1", "2", "3", "4")
    
    ManualKeysConfig()
        : toggleMode("M")
        , configSelect{"F1", "F6", "F7"}
        , groupSelect("G")
        , cameraSelect{"1", "2", "3", "4"} {}
};

/**
 * Group configuration - defines which cameras are routed to which slots
 */
struct GroupConfig {
    std::string configName;              // "config_a", "config_b", etc.
    std::array<int, 4> slotsG1_G4;       // Cameras routed to slots G1-G4 (1-based camera IDs)
    std::array<int, 4> slotsG5_G8;       // Cameras routed to slots G5-G8 (1-based camera IDs)
    float triggerThreshold;               // Leader X position that triggers next config
    
    GroupConfig()
        : configName("default")
        , slotsG1_G4{1, 2, 3, 4}
        , slotsG5_G8{5, 6, 7, 8}
        , triggerThreshold(0.0f) {}
};

/**
 * Slot assignment state
 */
struct SlotAssignment {
    int slotIndex;        // Physical slot (0-7 for G1-G8)
    int cameraID;         // Currently assigned camera (1-12)
    bool isMuted;         // True during transition period
    int64_t muteEndTime;  // When mute expires
};

/**
 * Scene manager configuration
 */
struct SceneManagerConfig {
    bool enabled = true;
    SceneMode mode = SceneMode::AUTO;         // Operating mode (AUTO or MANUAL)
    int muteTimeoutMs = 200;                   // Mute duration after switch (signal stabilization)
    std::vector<GroupConfig> groups;           // Ordered list of group configurations
    ManualKeysConfig manualKeys;               // Manual mode key mappings
    
    // Default configuration with 2 groups
    SceneManagerConfig() {
        GroupConfig configA;
        configA.configName = "config_a";
        configA.slotsG1_G4 = {1, 2, 3, 12};
        configA.slotsG5_G8 = {4, 5, 6, 7};
        configA.triggerThreshold = 25.0f;
        
        GroupConfig configB;
        configB.configName = "config_b";
        configB.slotsG1_G4 = {8, 9, 10, 12};
        configB.slotsG5_G8 = {4, 5, 6, 7};
        configB.triggerThreshold = 50.0f;
        
        groups.push_back(configA);
        groups.push_back(configB);
    }
};

/**
 * SceneManager - Dynamic camera group switching
 * 
 * Thread safety: All public methods are thread-safe
 */
class SceneManager {
public:
    static constexpr int kStreamingSlots = 8;     // G1-G8
    static constexpr int kMaxCameras = 12;        // CAM_01 to CAM_12
    static constexpr int kZenithSlots = 4;        // f1-f4 (fixed, not managed here)
    
    /**
     * Callback for slot mute state changes
     * @param slotIndex Slot that changed (0-7)
     * @param isMuted New mute state
     */
    using MuteCallback = std::function<void(int slotIndex, bool isMuted)>;
    
    /**
     * Constructor
     * @param videoHub VideoHub client for routing commands
     * @param config Scene manager configuration
     */
    SceneManager(VideoHubClient* videoHub, const SceneManagerConfig& config = SceneManagerConfig());
    ~SceneManager();
    
    /**
     * Initialize scene manager and apply initial configuration
     * @return true if successful
     */
    bool Initialize();
    
    /**
     * Update with new leader position
     * Evaluates if group switch is needed based on thresholds
     * @param Xg Leader X position in global coordinates (meters)
     * @param Yg Leader Y position (for logging)
     */
    void UpdateLeaderPosition(float Xg, float Yg);
    
    /**
     * Force switch to specific group configuration
     * @param configIndex Index of configuration to switch to
     * @return true if switch initiated
     */
    bool TriggerGroupSwitch(int configIndex);
    
    /**
     * Process mute timeouts
     * Call periodically (e.g., every frame) to release mutes
     */
    void ProcessMuteTimeouts();
    
    /**
     * Get current active slots (not muted)
     * @return Vector of slot indices that are currently active
     */
    std::vector<int> GetActiveSlots() const;
    
    /**
     * Get currently muted slots
     * @return Vector of slot indices that are muted
     */
    std::vector<int> GetMutedSlots() const;
    
    /**
     * Check if specific slot is muted
     * @param slotIndex Slot to check (0-7)
     * @return true if slot is currently muted
     */
    bool IsSlotMuted(int slotIndex) const;
    
    /**
     * Get camera assigned to specific slot
     * @param slotIndex Slot to query (0-7)
     * @return Camera ID (1-12) or 0 if invalid
     */
    int GetSlotCamera(int slotIndex) const;
    
    /**
     * Get current group configuration index
     */
    int GetCurrentConfigIndex() const { return m_currentConfigIndex; }
    
    /**
     * Get current group configuration name
     */
    std::string GetCurrentConfigName() const;
    
    /**
     * Set callback for mute state changes
     */
    void SetMuteCallback(MuteCallback callback) { m_muteCallback = callback; }
    
    /**
     * Check if scene manager is enabled
     */
    bool IsEnabled() const { return m_config.enabled; }
    
    /**
     * Get last known leader position
     */
    float GetLastLeaderX() const { return m_lastLeaderX; }
    
    /**
     * Set operating mode (AUTO or MANUAL)
     * @param mode New mode to set
     */
    void SetMode(SceneMode mode);
    
    /**
     * Get current operating mode
     * @return Current mode (AUTO or MANUAL)
     */
    SceneMode GetMode() const { return m_mode; }
    
    /**
     * Select configuration manually (MANUAL mode only)
     * @param configIndex Configuration index (0-2 for config_a/b/c)
     * @return true if selection successful
     */
    bool SelectConfig(int configIndex);
    
    /**
     * Select group manually (MANUAL mode only)
     * @param group Group to select (G1_G4 or G5_G8)
     */
    void SelectGroup(ActiveGroup group);
    
    /**
     * Toggle between groups (MANUAL mode only)
     * Switches between G1_G4 and G5_G8
     */
    void ToggleGroup();
    
    /**
     * Select camera within active group (MANUAL mode only)
     * @param index Camera index within group (0-3)
     * @return true if selection successful
     */
    bool SelectCameraInGroup(int index);
    
    /**
     * Get current manual mode state
     * @return Manual state structure
     */
    ManualState GetManualState() const;
    
    /**
     * Get active camera ID in manual mode
     * Calculates based on current config + group + camera selection
     * @return Camera ID (1-12) or 0 if invalid
     */
    int GetActiveCameraID() const;

private:
    VideoHubClient* m_videoHub;
    SceneManagerConfig m_config;
    std::array<SlotAssignment, kStreamingSlots> m_slotAssignments;
    std::atomic<int> m_currentConfigIndex;
    float m_lastLeaderX;
    float m_lastLeaderY;
    mutable std::mutex m_mutex;
    MuteCallback m_muteCallback;
    bool m_initialized;
    
    // Manual mode state
    SceneMode m_mode;
    ManualState m_manualState;
    ManualKeysConfig m_manualKeysConfig;
    
    // Helper methods
    void ApplyGroupConfig(int configIndex);
    bool SendVideoHubRouting(int slotIndex, int cameraID);
    void MuteSlot(int slotIndex);
    void UnmuteSlot(int slotIndex);
    int64_t GetCurrentTimeMs() const;
    int EvaluateDesiredConfig() const;
    
    /**
     * Internal version of ProcessMuteTimeouts that assumes mutex is already held.
     * Call this from methods that already hold m_mutex to avoid recursive lock deadlock.
     */
    void ProcessMuteTimeoutsInternal();
};
