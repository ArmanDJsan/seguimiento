/**
 * PTZController.h
 * 
 * VISCA over IP controller for coordinated PTZ camera tracking
 * Supports up to 4 PTZ cameras (1080p 30fps) for centroid tracking
 * 
 * Features:
 * - UDP VISCA protocol implementation
 * - Non-blocking socket communication with timeout
 * - Thread-safe command execution (shared mutex with InferenceEngine)
 * - Preset management for fallback positions
 * - Proportional control for smooth pan/tilt/zoom
 * 
 * Usage:
 *   PTZController controller;
 *   controller.Initialize(config.ptz_cameras);
 *   controller.SendPanTilt(0, panSpeed, tiltSpeed);  // Camera 0
 *   controller.SendZoomAll(ZoomDirection::Out, 2);   // Zoom out all
 */

#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET SocketHandle;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SocketHandle;
#define INVALID_SOCKET_VALUE (-1)
#endif

namespace PTZ {

/**
 * Zoom direction for PTZ cameras
 */
enum class ZoomDirection {
    In,     // Zoom in (tele)
    Out,    // Zoom out (wide)
    Stop    // Stop zooming
};

/**
 * Focus direction for PTZ cameras
 */
enum class FocusDirection {
    Near,   // Focus near
    Far,    // Focus far
    Stop,   // Stop focusing
    Auto    // Auto focus
};

/**
 * PTZ tracking mode states
 */
enum class TrackingMode {
    PRESET_MODE,    // Cameras in fixed predefined positions
    TRACKING_MODE,  // Actively following centroid
    FALLBACK_MODE   // Returning to preset due to lost detections
};

/**
 * Configuration for a single PTZ camera
 */
struct PTZCameraConfig {
    int cameraID = 0;               // Camera identifier (0-3)
    std::string ipAddress;          // Camera IP address
    int viscaPort = 52381;          // VISCA UDP port (default Sony/Panasonic)
    int viscaAddress = 1;           // VISCA device address (1-7)
    std::vector<int> presets;       // Preset positions for this camera
    int basePreset = 1;             // Default preset for fallback
    
    PTZCameraConfig() = default;
    PTZCameraConfig(int id, const std::string& ip, int port = 52381)
        : cameraID(id), ipAddress(ip), viscaPort(port), viscaAddress(1), basePreset(1) {}
};

/**
 * Global PTZ tracking configuration
 */
struct PTZTrackingConfig {
    float kp = 50.0f;                    // Proportional gain for pan/tilt
    float deadZone = 0.05f;              // Dead zone around center (normalized)
    int maxPanSpeed = 24;                // Max pan speed (VISCA units)
    int maxTiltSpeed = 20;               // Max tilt speed (VISCA units)
    float zoomOutThreshold = 0.15f;      // Std deviation (normalized) to trigger zoom out
    float zoomInThreshold = 0.05f;       // Std deviation (normalized) to trigger zoom in
    int fallbackTimeoutFrames = 60;      // Frames without detection before fallback (2s @ 30fps)
    int engageMinFrames = 3;             // Consecutive frames needed to engage tracking
    int minSphereCount = 3;              // Minimum spheres to engage tracking
    
    PTZTrackingConfig() = default;
};

/**
 * VISCA command result
 */
struct VISCAResult {
    bool success;
    std::string errorMessage;
    int64_t responseTimeMs;
    
    VISCAResult() : success(false), responseTimeMs(0) {}
    static VISCAResult Ok() { VISCAResult r; r.success = true; return r; }
    static VISCAResult Error(const std::string& msg) { 
        VISCAResult r; r.success = false; r.errorMessage = msg; return r; 
    }
};

/**
 * PTZController - VISCA over IP controller for PTZ cameras
 * 
 * Thread safety: All public methods are thread-safe via shared mutex
 */
class PTZController {
public:
    static constexpr int kMaxCameras = 4;
    static constexpr int kSocketTimeoutMs = 100;
    
    /**
     * Constructor
     */
    PTZController();
    ~PTZController();
    
    // Non-copyable
    PTZController(const PTZController&) = delete;
    PTZController& operator=(const PTZController&) = delete;
    
    /**
     * Initialize PTZ controller with camera configurations
     * @param cameras Vector of PTZ camera configurations
     * @param trackingConfig Tracking parameters
     * @return true if successful
     */
    bool Initialize(const std::vector<PTZCameraConfig>& cameras,
                   const PTZTrackingConfig& trackingConfig = PTZTrackingConfig());
    
    /**
     * Shutdown and release resources
     */
    void Shutdown();
    
    /**
     * Check if controller is initialized
     */
    bool IsInitialized() const { return m_initialized; }
    
    // === Individual Camera Commands ===
    
    /**
     * Send pan/tilt command to specific camera
     * @param cameraID Camera identifier (0-3)
     * @param panSpeed Pan speed (-24 to +24, negative=left)
     * @param tiltSpeed Tilt speed (-20 to +20, negative=down)
     * @return Command result
     */
    VISCAResult SendPanTilt(int cameraID, int panSpeed, int tiltSpeed);
    
    /**
     * Stop pan/tilt movement
     * @param cameraID Camera identifier (0-3)
     * @return Command result
     */
    VISCAResult StopPanTilt(int cameraID);
    
    /**
     * Send zoom command to specific camera
     * @param cameraID Camera identifier (0-3)
     * @param direction Zoom direction
     * @param speed Zoom speed (0-7)
     * @return Command result
     */
    VISCAResult SendZoom(int cameraID, ZoomDirection direction, int speed);
    
    /**
     * Send focus command to specific camera
     * @param cameraID Camera identifier (0-3)
     * @param direction Focus direction
     * @param speed Focus speed (0-7, ignored for Auto)
     * @return Command result
     */
    VISCAResult SendFocus(int cameraID, FocusDirection direction, int speed = 3);
    
    /**
     * Call a preset position
     * @param cameraID Camera identifier (0-3)
     * @param presetNumber Preset number (1-255)
     * @return Command result
     */
    VISCAResult CallPreset(int cameraID, int presetNumber);
    
    /**
     * Return camera to base preset
     * @param cameraID Camera identifier (0-3)
     * @return Command result
     */
    VISCAResult ReturnToBase(int cameraID);
    
    // === All Cameras Commands ===
    
    /**
     * Send pan/tilt command to all cameras
     * @param panSpeed Pan speed (-24 to +24)
     * @param tiltSpeed Tilt speed (-20 to +20)
     */
    void SendPanTiltAll(int panSpeed, int tiltSpeed);
    
    /**
     * Stop pan/tilt on all cameras
     */
    void StopPanTiltAll();
    
    /**
     * Send zoom command to all cameras
     * @param direction Zoom direction
     * @param speed Zoom speed (0-7)
     */
    void SendZoomAll(ZoomDirection direction, int speed);
    
    /**
     * Return all cameras to base preset
     */
    void ReturnToBaseAll();
    
    // === Tracking State Management ===
    
    /**
     * Set current tracking mode
     * @param mode New tracking mode
     */
    void SetTrackingMode(TrackingMode mode);
    
    /**
     * Get current tracking mode
     * @return Current mode
     */
    TrackingMode GetTrackingMode() const { return m_currentMode; }
    
    /**
     * Get tracking configuration
     */
    const PTZTrackingConfig& GetTrackingConfig() const { return m_trackingConfig; }
    
    /**
     * Get camera count
     */
    int GetCameraCount() const { return static_cast<int>(m_cameras.size()); }
    
    /**
     * Get reference to the mutex (for external synchronization)
     * Allows sharing mutex with InferenceEngine for coordinated access
     */
    std::mutex& GetMutex() { return m_viscaMutex; }

private:
    // State
    std::atomic<bool> m_initialized;
    std::vector<PTZCameraConfig> m_cameras;
    PTZTrackingConfig m_trackingConfig;
    TrackingMode m_currentMode;
    mutable std::mutex m_viscaMutex;
    
    // Socket per camera for concurrent command sending
    std::vector<SocketHandle> m_sockets;
    std::vector<sockaddr_in> m_cameraAddresses;
    
    // VISCA protocol helpers
    std::vector<uint8_t> BuildVISCACommand(int viscaAddress, const std::vector<uint8_t>& payload);
    VISCAResult SendCommand(int cameraID, const std::vector<uint8_t>& command);
    
    // Pan/Tilt specific VISCA commands
    std::vector<uint8_t> BuildPanTiltCommand(int viscaAddress, int panSpeed, int tiltSpeed);
    std::vector<uint8_t> BuildPanTiltStopCommand(int viscaAddress);
    std::vector<uint8_t> BuildZoomCommand(int viscaAddress, ZoomDirection direction, int speed);
    std::vector<uint8_t> BuildFocusCommand(int viscaAddress, FocusDirection direction, int speed);
    std::vector<uint8_t> BuildPresetCommand(int viscaAddress, int presetNumber);
    
    // Socket management
    bool CreateSockets();
    void CloseSockets();
    bool SetSocketNonBlocking(SocketHandle socket);
    
    // Helpers
    int64_t GetCurrentTimeMs() const;
};

} // namespace PTZ
