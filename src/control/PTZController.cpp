/**
 * PTZController.cpp
 * 
 * Implementation of VISCA over IP controller for PTZ camera tracking
 * 
 * VISCA Protocol Reference:
 * - All commands start with 0x81 (address byte for camera 1)
 * - Commands end with 0xFF (terminator)
 * - Pan/Tilt uses absolute/relative positioning with speed control
 * - Zoom uses variable speed control
 */

#include "PTZController.h"
#include "../utils/Logger.h"
#include <chrono>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <errno.h>
#endif

namespace PTZ {

// VISCA Protocol Constants
namespace VISCA {
    constexpr uint8_t HEADER = 0x80;        // Address header
    constexpr uint8_t TERMINATOR = 0xFF;    // Command terminator
    constexpr uint8_t COMMAND = 0x01;       // Command type
    constexpr uint8_t INQUIRY = 0x09;       // Inquiry type
    
    // Command categories
    constexpr uint8_t CAT_CAMERA = 0x04;    // Camera control
    constexpr uint8_t CAT_PAN_TILT = 0x06;  // Pan/Tilt control
    
    // Pan/Tilt commands
    constexpr uint8_t PT_DRIVE = 0x01;      // Pan/Tilt drive
    constexpr uint8_t PT_ABSOLUTE = 0x02;   // Absolute position
    constexpr uint8_t PT_RELATIVE = 0x03;   // Relative position
    constexpr uint8_t PT_HOME = 0x04;       // Home position
    constexpr uint8_t PT_RESET = 0x05;      // Reset position
    
    // Zoom commands
    constexpr uint8_t ZOOM_STOP = 0x00;
    constexpr uint8_t ZOOM_TELE = 0x02;     // Zoom in
    constexpr uint8_t ZOOM_WIDE = 0x03;     // Zoom out
    constexpr uint8_t ZOOM_TELE_VAR = 0x20; // Zoom in variable speed
    constexpr uint8_t ZOOM_WIDE_VAR = 0x30; // Zoom out variable speed
    
    // Focus commands
    constexpr uint8_t FOCUS_STOP = 0x00;
    constexpr uint8_t FOCUS_FAR = 0x02;
    constexpr uint8_t FOCUS_NEAR = 0x03;
    constexpr uint8_t FOCUS_AUTO = 0x38;
    constexpr uint8_t FOCUS_MANUAL = 0x38;
    
    // Preset commands
    constexpr uint8_t PRESET_SET = 0x01;
    constexpr uint8_t PRESET_RECALL = 0x02;
    constexpr uint8_t PRESET_RESET = 0x00;
}

PTZController::PTZController()
    : m_initialized(false)
    , m_currentMode(TrackingMode::PRESET_MODE)
{
#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

PTZController::~PTZController() {
    Shutdown();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool PTZController::Initialize(const std::vector<PTZCameraConfig>& cameras,
                               const PTZTrackingConfig& trackingConfig) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (m_initialized) {
        Logger::Warning("PTZController already initialized");
        return true;
    }
    
    if (cameras.empty()) {
        Logger::Warning("PTZController: No cameras configured, running in stub mode");
        m_initialized = true;
        return true;
    }
    
    if (cameras.size() > kMaxCameras) {
        Logger::Error("PTZController: Too many cameras (max " + std::to_string(kMaxCameras) + ")");
        return false;
    }
    
    m_cameras = cameras;
    m_trackingConfig = trackingConfig;
    
    // Create UDP sockets for each camera
    if (!CreateSockets()) {
        Logger::Error("PTZController: Failed to create sockets");
        return false;
    }
    
    Logger::Info("PTZController initialized with " + std::to_string(cameras.size()) + " cameras");
    for (const auto& cam : m_cameras) {
        Logger::Info("  Camera " + std::to_string(cam.cameraID) + 
                    ": " + cam.ipAddress + ":" + std::to_string(cam.viscaPort));
    }
    
    m_initialized = true;
    return true;
}

void PTZController::Shutdown() {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (!m_initialized) return;
    
    // Stop all movements
    for (size_t i = 0; i < m_cameras.size(); ++i) {
        auto stopCmd = BuildPanTiltStopCommand(m_cameras[i].viscaAddress);
        SendCommand(static_cast<int>(i), stopCmd);
    }
    
    CloseSockets();
    m_cameras.clear();
    m_initialized = false;
    
    Logger::Info("PTZController shutdown complete");
}

bool PTZController::CreateSockets() {
    m_sockets.clear();
    m_cameraAddresses.clear();
    
    for (const auto& cam : m_cameras) {
        // Create UDP socket
        SocketHandle sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET_VALUE) {
            Logger::Error("PTZController: Failed to create socket for camera " + 
                         std::to_string(cam.cameraID));
            CloseSockets();
            return false;
        }
        
        // Set non-blocking
        if (!SetSocketNonBlocking(sock)) {
            Logger::Warning("PTZController: Failed to set non-blocking for camera " + 
                           std::to_string(cam.cameraID));
        }
        
        // Set socket timeout
#ifdef _WIN32
        DWORD timeout = kSocketTimeoutMs;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = kSocketTimeoutMs * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        
        // Setup camera address
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(cam.viscaPort));
        inet_pton(AF_INET, cam.ipAddress.c_str(), &addr.sin_addr);
        
        m_sockets.push_back(sock);
        m_cameraAddresses.push_back(addr);
    }
    
    return true;
}

void PTZController::CloseSockets() {
    for (auto sock : m_sockets) {
        if (sock != INVALID_SOCKET_VALUE) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
        }
    }
    m_sockets.clear();
    m_cameraAddresses.clear();
}

bool PTZController::SetSocketNonBlocking(SocketHandle socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

// === VISCA Command Building ===

std::vector<uint8_t> PTZController::BuildVISCACommand(int viscaAddress, 
                                                       const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> command;
    command.reserve(payload.size() + 2);
    
    // Address byte: 0x80 + device address (1-7)
    command.push_back(VISCA::HEADER | static_cast<uint8_t>(viscaAddress & 0x07));
    
    // Payload
    command.insert(command.end(), payload.begin(), payload.end());
    
    // Terminator
    command.push_back(VISCA::TERMINATOR);
    
    return command;
}

std::vector<uint8_t> PTZController::BuildPanTiltCommand(int viscaAddress, 
                                                         int panSpeed, int tiltSpeed) {
    // Clamp speeds
    panSpeed = std::clamp(panSpeed, -24, 24);
    tiltSpeed = std::clamp(tiltSpeed, -20, 20);
    
    // VISCA Pan/Tilt Drive command
    // 8x 01 06 01 VV WW 03 03 FF
    // VV = Pan speed (01-18h), WW = Tilt speed (01-14h)
    // Direction is encoded in command: 01=left/up, 02=right/down, 03=stop
    
    uint8_t absPS = static_cast<uint8_t>(std::abs(panSpeed));
    uint8_t absTS = static_cast<uint8_t>(std::abs(tiltSpeed));
    
    // Ensure minimum speed of 1 when moving
    if (absPS == 0 && panSpeed != 0) absPS = 1;
    if (absTS == 0 && tiltSpeed != 0) absTS = 1;
    
    // Direction bytes
    uint8_t panDir = 0x03;   // Stop
    uint8_t tiltDir = 0x03;  // Stop
    
    if (panSpeed > 0) panDir = 0x02;       // Right
    else if (panSpeed < 0) panDir = 0x01;  // Left
    
    if (tiltSpeed > 0) tiltDir = 0x02;     // Down
    else if (tiltSpeed < 0) tiltDir = 0x01; // Up
    
    std::vector<uint8_t> payload = {
        VISCA::COMMAND,         // 0x01
        VISCA::CAT_PAN_TILT,    // 0x06
        VISCA::PT_DRIVE,        // 0x01
        absPS,                  // Pan speed
        absTS,                  // Tilt speed
        panDir,                 // Pan direction
        tiltDir                 // Tilt direction
    };
    
    return BuildVISCACommand(viscaAddress, payload);
}

std::vector<uint8_t> PTZController::BuildPanTiltStopCommand(int viscaAddress) {
    std::vector<uint8_t> payload = {
        VISCA::COMMAND,         // 0x01
        VISCA::CAT_PAN_TILT,    // 0x06
        VISCA::PT_DRIVE,        // 0x01
        0x01,                   // Min pan speed
        0x01,                   // Min tilt speed
        0x03,                   // Pan stop
        0x03                    // Tilt stop
    };
    
    return BuildVISCACommand(viscaAddress, payload);
}

std::vector<uint8_t> PTZController::BuildZoomCommand(int viscaAddress, 
                                                      ZoomDirection direction, int speed) {
    // Clamp speed
    speed = std::clamp(speed, 0, 7);
    
    // VISCA Zoom command: 8x 01 04 07 XX FF
    // XX: 00=Stop, 02=Tele, 03=Wide, 2p=Tele Variable, 3p=Wide Variable
    
    uint8_t zoomByte;
    switch (direction) {
        case ZoomDirection::In:
            zoomByte = VISCA::ZOOM_TELE_VAR | static_cast<uint8_t>(speed);
            break;
        case ZoomDirection::Out:
            zoomByte = VISCA::ZOOM_WIDE_VAR | static_cast<uint8_t>(speed);
            break;
        case ZoomDirection::Stop:
        default:
            zoomByte = VISCA::ZOOM_STOP;
            break;
    }
    
    std::vector<uint8_t> payload = {
        VISCA::COMMAND,         // 0x01
        VISCA::CAT_CAMERA,      // 0x04
        0x07,                   // Zoom
        zoomByte
    };
    
    return BuildVISCACommand(viscaAddress, payload);
}

std::vector<uint8_t> PTZController::BuildFocusCommand(int viscaAddress, 
                                                       FocusDirection direction, int speed) {
    // Clamp speed
    speed = std::clamp(speed, 0, 7);
    
    // VISCA Focus command: 8x 01 04 08 XX FF
    
    std::vector<uint8_t> payload;
    
    if (direction == FocusDirection::Auto) {
        // Auto focus: 8x 01 04 38 02 FF
        payload = {
            VISCA::COMMAND,
            VISCA::CAT_CAMERA,
            0x38,               // Focus mode
            0x02                // Auto
        };
    } else {
        uint8_t focusByte;
        switch (direction) {
            case FocusDirection::Near:
                focusByte = 0x30 | static_cast<uint8_t>(speed);  // Near variable
                break;
            case FocusDirection::Far:
                focusByte = 0x20 | static_cast<uint8_t>(speed);  // Far variable
                break;
            case FocusDirection::Stop:
            default:
                focusByte = VISCA::FOCUS_STOP;
                break;
        }
        
        payload = {
            VISCA::COMMAND,
            VISCA::CAT_CAMERA,
            0x08,               // Focus
            focusByte
        };
    }
    
    return BuildVISCACommand(viscaAddress, payload);
}

std::vector<uint8_t> PTZController::BuildPresetCommand(int viscaAddress, int presetNumber) {
    // Clamp preset number
    presetNumber = std::clamp(presetNumber, 0, 255);
    
    // VISCA Preset Recall: 8x 01 04 3F 02 PP FF
    std::vector<uint8_t> payload = {
        VISCA::COMMAND,
        VISCA::CAT_CAMERA,
        0x3F,                   // Memory
        VISCA::PRESET_RECALL,   // 0x02 = Recall
        static_cast<uint8_t>(presetNumber)
    };
    
    return BuildVISCACommand(viscaAddress, payload);
}

// === Command Sending ===

VISCAResult PTZController::SendCommand(int cameraID, const std::vector<uint8_t>& command) {
    if (cameraID < 0 || cameraID >= static_cast<int>(m_sockets.size())) {
        return VISCAResult::Error("Invalid camera ID");
    }
    
    auto startTime = GetCurrentTimeMs();
    
    // Send command via UDP
    int sent = sendto(m_sockets[cameraID], 
                      reinterpret_cast<const char*>(command.data()),
                      static_cast<int>(command.size()),
                      0,
                      reinterpret_cast<const sockaddr*>(&m_cameraAddresses[cameraID]),
                      sizeof(sockaddr_in));
    
    if (sent < 0) {
        return VISCAResult::Error("Send failed");
    }
    
    // For UDP VISCA, we don't wait for response (fire-and-forget for speed)
    // Response handling can be added for reliability if needed
    
    VISCAResult result = VISCAResult::Ok();
    result.responseTimeMs = GetCurrentTimeMs() - startTime;
    
    return result;
}

// === Public Commands ===

VISCAResult PTZController::SendPanTilt(int cameraID, int panSpeed, int tiltSpeed) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (!m_initialized || cameraID >= static_cast<int>(m_cameras.size())) {
        return VISCAResult::Error("Not initialized or invalid camera");
    }
    
    // Apply dead zone using floating point comparison for precision
    float deadZonePan = m_trackingConfig.deadZone * static_cast<float>(m_trackingConfig.maxPanSpeed);
    float deadZoneTilt = m_trackingConfig.deadZone * static_cast<float>(m_trackingConfig.maxTiltSpeed);
    
    if (static_cast<float>(std::abs(panSpeed)) < deadZonePan) {
        panSpeed = 0;
    }
    if (static_cast<float>(std::abs(tiltSpeed)) < deadZoneTilt) {
        tiltSpeed = 0;
    }
    
    auto command = BuildPanTiltCommand(m_cameras[cameraID].viscaAddress, panSpeed, tiltSpeed);
    return SendCommand(cameraID, command);
}

VISCAResult PTZController::StopPanTilt(int cameraID) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (!m_initialized || cameraID >= static_cast<int>(m_cameras.size())) {
        return VISCAResult::Error("Not initialized or invalid camera");
    }
    
    auto command = BuildPanTiltStopCommand(m_cameras[cameraID].viscaAddress);
    return SendCommand(cameraID, command);
}

VISCAResult PTZController::SendZoom(int cameraID, ZoomDirection direction, int speed) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (!m_initialized || cameraID >= static_cast<int>(m_cameras.size())) {
        return VISCAResult::Error("Not initialized or invalid camera");
    }
    
    auto command = BuildZoomCommand(m_cameras[cameraID].viscaAddress, direction, speed);
    return SendCommand(cameraID, command);
}

VISCAResult PTZController::SendFocus(int cameraID, FocusDirection direction, int speed) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (!m_initialized || cameraID >= static_cast<int>(m_cameras.size())) {
        return VISCAResult::Error("Not initialized or invalid camera");
    }
    
    auto command = BuildFocusCommand(m_cameras[cameraID].viscaAddress, direction, speed);
    return SendCommand(cameraID, command);
}

VISCAResult PTZController::CallPreset(int cameraID, int presetNumber) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (!m_initialized || cameraID >= static_cast<int>(m_cameras.size())) {
        return VISCAResult::Error("Not initialized or invalid camera");
    }
    
    Logger::Info("PTZController: Camera " + std::to_string(cameraID) + 
                " calling preset " + std::to_string(presetNumber));
    
    auto command = BuildPresetCommand(m_cameras[cameraID].viscaAddress, presetNumber);
    return SendCommand(cameraID, command);
}

VISCAResult PTZController::ReturnToBase(int cameraID) {
    if (!m_initialized || cameraID >= static_cast<int>(m_cameras.size())) {
        return VISCAResult::Error("Not initialized or invalid camera");
    }
    
    return CallPreset(cameraID, m_cameras[cameraID].basePreset);
}

// === All Cameras Commands ===

void PTZController::SendPanTiltAll(int panSpeed, int tiltSpeed) {
    for (size_t i = 0; i < m_cameras.size(); ++i) {
        SendPanTilt(static_cast<int>(i), panSpeed, tiltSpeed);
    }
}

void PTZController::StopPanTiltAll() {
    for (size_t i = 0; i < m_cameras.size(); ++i) {
        StopPanTilt(static_cast<int>(i));
    }
}

void PTZController::SendZoomAll(ZoomDirection direction, int speed) {
    for (size_t i = 0; i < m_cameras.size(); ++i) {
        SendZoom(static_cast<int>(i), direction, speed);
    }
}

void PTZController::ReturnToBaseAll() {
    Logger::Info("PTZController: All cameras returning to base presets");
    for (size_t i = 0; i < m_cameras.size(); ++i) {
        ReturnToBase(static_cast<int>(i));
    }
}

// === Tracking State ===

void PTZController::SetTrackingMode(TrackingMode mode) {
    std::lock_guard<std::mutex> lock(m_viscaMutex);
    
    if (m_currentMode != mode) {
        const char* modeNames[] = {"PRESET_MODE", "TRACKING_MODE", "FALLBACK_MODE"};
        Logger::Info("PTZController: Mode changed to " + 
                    std::string(modeNames[static_cast<int>(mode)]));
        
        m_currentMode = mode;
    }
}

// === Helpers ===

int64_t PTZController::GetCurrentTimeMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

} // namespace PTZ
