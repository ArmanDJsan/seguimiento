/**
 * SceneManager.h
 * 
 * Camera routing controller with Auto and Manual modes.
 * - Auto mode: Selects cameras based on ball position detection (leapfrogging)
 * - Manual mode: Keyboard-based selection of config, group, and camera
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// Forward declaration
class VideoHubClient;

namespace VIB {

/**
 * Operating mode for SceneManager
 */
enum class SceneMode {
    AUTO,   // Automatic camera selection based on ball tracking
    MANUAL  // Manual keyboard-based selection
};

/**
 * Group identifier within a configuration
 */
enum class SceneGroup {
    G1_G4 = 0,  // First group of cameras
    G5_G8 = 1   // Second group of cameras
};

/**
 * Configuration structure for a scene group
 */
struct SceneConfig {
    std::string name;           // e.g., "config_a", "config_b", "config_c"
    std::vector<int> g1_g4;     // Camera IDs for group G1-G4 (up to 4 cameras)
    std::vector<int> g5_g8;     // Camera IDs for group G5-G8 (up to 4 cameras)
    float triggerThreshold;     // Ball position threshold to trigger this config (0-100)
    std::string description;    // Human-readable description
};

/**
 * Key mapping configuration for manual mode
 */
struct ManualKeyConfig {
    int toggleMode;             // Key to toggle Auto/Manual (default: 'M')
    std::vector<int> configSelect;  // Keys for config selection (F1, F2, F3)
    int groupToggle;            // Key to toggle group (default: 'G')
    std::vector<int> cameraSelect;  // Keys 1-4 for camera position in group
};

/**
 * SceneManager class
 * 
 * Manages camera routing through VideoHub with support for
 * automatic (leapfrogging) and manual keyboard control.
 */
class SceneManager {
public:
    SceneManager();
    ~SceneManager() = default;

    /**
     * Initialize the scene manager with configurations
     * @param configs Vector of scene configurations
     * @param videoHub Pointer to VideoHub client for routing
     * @param outputIndex VideoHub output index to route to
     * @return true if initialization successful
     */
    bool Initialize(const std::vector<SceneConfig>& configs, 
                   VideoHubClient* videoHub,
                   int outputIndex = 0);

    /**
     * Check if SceneManager is enabled and properly initialized
     */
    bool IsEnabled() const { return m_enabled && m_initialized; }

    /**
     * Set operating mode
     */
    void SetMode(SceneMode mode);
    SceneMode GetMode() const { return m_mode; }

    /**
     * Manual mode controls
     */
    void SelectConfig(int configIndex);       // 0 = config_a, 1 = config_b, 2 = config_c
    void SelectGroup(SceneGroup group);
    void ToggleGroup();                        // Switch between G1_G4 and G5_G8
    void SelectCameraInGroup(int position);   // 0-3 within the active group

    /**
     * Auto mode - update based on ball detection
     * @param ballPosition Normalized ball position (0-100 along track)
     */
    void UpdateFromDetection(float ballPosition);

    /**
     * Process keyboard input
     * @param vkCode Virtual key code
     * @return true if key was handled
     */
    bool ProcessKeyInput(int vkCode);

    /**
     * Get current state
     */
    int GetActiveCamera() const { return m_activeCamera; }
    int GetActiveConfigIndex() const { return m_currentConfigIndex; }
    SceneGroup GetActiveGroup() const { return m_currentGroup; }
    int GetActiveCameraPosition() const { return m_currentCameraPosition; }

    /**
     * Get human-readable status string for display
     */
    std::string GetStatusString() const;

    /**
     * Get cameras in the currently active group
     */
    std::vector<int> GetCurrentGroupCameras() const;

    /**
     * Set enabled state
     */
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    /**
     * Set mute timeout (delay before allowing new routing)
     */
    void SetMuteTimeout(int timeoutMs) { m_muteTimeoutMs = timeoutMs; }

    /**
     * Configure key mappings for manual mode
     */
    void SetKeyConfig(const ManualKeyConfig& keyConfig) { m_keyConfig = keyConfig; }

private:
    /**
     * Send routing command to VideoHub
     * @param cameraId Camera ID to route (1-12)
     * @return true if routing succeeded
     */
    bool SendVideoHubRouting(int cameraId);

    /**
     * Apply camera selection and route
     */
    void ApplyCameraSelection();

    /**
     * Get camera ID from current group and position
     */
    int GetCameraFromGroupPosition(int position) const;

    // State
    bool m_enabled;
    bool m_initialized;
    SceneMode m_mode;
    int m_currentConfigIndex;
    SceneGroup m_currentGroup;
    int m_currentCameraPosition;  // 0-3 within the group
    int m_activeCamera;           // Actual camera ID (1-12)

    // Configuration
    std::vector<SceneConfig> m_configs;
    VideoHubClient* m_videoHub;
    int m_outputIndex;
    int m_muteTimeoutMs;
    ManualKeyConfig m_keyConfig;

    // Timing for rate limiting
    uint64_t m_lastRoutingTime;
};

} // namespace VIB
