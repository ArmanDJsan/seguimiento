/**
 * ChoreographyEvent.h
 * 
 * Event definitions for vMix choreography automation.
 * Supports all standard vMix TCP commands plus NDI slot routing.
 * 
 * Usage:
 *   auto event = ChoreographyEvent::CutDirect("guid-here", -1);
 *   auto timerEvent = ChoreographyEvent::Timer(2000);
 */

#pragma once

#include <string>
#include <variant>
#include <optional>

namespace Choreography {

/**
 * Event types supported by the choreography engine
 */
enum class EventType {
    // vMix Commands
    CutDirect,              // Cut directly to input
    QuickPlay,              // QuickPlay input
    Play,                   // Play input
    Pause,                  // Pause input
    Restart,                // Restart input
    AudioOn,                // Enable audio for input
    AudioOff,               // Disable audio for input
    StartRecording,         // Start recording
    StopRecording,          // Stop recording
    StartMultiCorder,       // Start MultiCorder recording
    StopMultiCorder,        // Stop MultiCorder recording
    BrowserReload,          // Reload browser input
    
    // Overlay commands
    OverlayInputIn,         // Show overlay (with layer)
    OverlayInputOut,        // Hide overlay (layer only)
    
    // Replay commands
    ReplayStartRecording,   // Start replay recording
    ReplayStopRecording,    // Stop replay recording
    ReplayMarkIn,           // Mark replay in point
    ReplayMarkOut,          // Mark replay out point
    ReplayLive,             // Switch replay to live
    ReplaySetSpeed,         // Set replay speed (percentage)
    ReplaySelectLastEvent,  // Select last replay event
    
    // Internal/Custom
    Timer,                  // Wait for specified milliseconds
    NDISlotChange,          // Change camera on NDI slot via VideoHub
    
    // Scene control
    SceneSwitch,            // Switch to a specific scene configuration
    
    // Sphere verification events
    SpherePresenceCheck,    // Verify presence of spheres
    SpherePositionCapture,  // Capture positions of spheres
    SphereArrivalWait,      // Wait for sphere arrivals
    
    // PTZ commands
    PTZPreset,              // Call a preset on a PTZ camera (1-indexed camera number)

    // ESP32 commands
    ESP32Command,           // Send an HTTP command to the ESP32 device

    // Meta events
    Comment,                // Comment (no-op, for documentation)
    Label                   // Label for jump targets (future use)
};

/**
 * Parameters for events that require a GUID
 */
struct GuidParam {
    std::string guid;
    
    GuidParam() = default;
    explicit GuidParam(const std::string& g) : guid(g) {}
};

/**
 * Parameters for CutDirect (guid + optional layer)
 */
struct CutDirectParams {
    std::string guid;
    int layer;  // -1 for no layer specification
    
    CutDirectParams() : layer(-1) {}
    CutDirectParams(const std::string& g, int l = -1) : guid(g), layer(l) {}
};

/**
 * Parameters for Overlay operations
 */
struct OverlayParams {
    std::string guid;  // Empty for Out operations
    int layer;         // 1-4 for overlay channels
    
    OverlayParams() : layer(1) {}
    OverlayParams(const std::string& g, int l) : guid(g), layer(l) {}
    explicit OverlayParams(int l) : layer(l) {}  // For Out operations
};

/**
 * Parameters for Timer
 */
struct TimerParams {
    int milliseconds;
    
    TimerParams() : milliseconds(1000) {}
    explicit TimerParams(int ms) : milliseconds(ms) {}
};

/**
 * Parameters for Replay speed
 */
struct ReplaySpeedParams {
    int percentage;  // 0-200, where 100 is normal speed
    
    ReplaySpeedParams() : percentage(100) {}
    explicit ReplaySpeedParams(int pct) : percentage(pct) {}
};

/**
 * Parameters for NDI Slot Change
 */
struct NDISlotParams {
    int slot;       // 0-7 for G1-G8
    int cameraID;   // 1-12 for CAM_01-CAM_12
    
    NDISlotParams() : slot(0), cameraID(1) {}
    NDISlotParams(int s, int c) : slot(s), cameraID(c) {}
};

/**
 * Parameters for Scene Switch
 */
struct SceneSwitchParams {
    int configIndex;  // Index into scene_manager groups
    
    SceneSwitchParams() : configIndex(0) {}
    explicit SceneSwitchParams(int idx) : configIndex(idx) {}
};

/**
 * Parameters for Comment/Label
 */
struct TextParam {
    std::string text;
    
    TextParam() = default;
    explicit TextParam(const std::string& t) : text(t) {}
};

/**
 * Parameters for PTZ Preset
 */
struct PTZPresetParams {
    int cameraNumber;   // 1-indexed camera number (1-4)
    int presetNumber;   // Preset number (1-255)
    
    PTZPresetParams() : cameraNumber(1), presetNumber(1) {}
    PTZPresetParams(int cam, int preset) : cameraNumber(cam), presetNumber(preset) {}
};

/**
 * Parameters for ESP32 Command
 */
struct ESP32CommandParams {
    std::string command;  // Logical command name: "iniciar", "finalizar", "reload", etc.
    
    ESP32CommandParams() = default;
    explicit ESP32CommandParams(const std::string& cmd) : command(cmd) {}
};

/**
 * Parameters for Sphere Verification
 */
struct SphereVerificationParams {
    int cameraID;           // Camera to use (1-12)
    int expectedSpheres;    // Number of spheres expected (for presence check/arrival wait)
    int timeoutMs;          // Timeout in milliseconds
    std::string mode;       // "presence", "positions", "arrivals"
    
    SphereVerificationParams()
        : cameraID(1), expectedSpheres(10), timeoutMs(5000), mode("presence") {}
    
    SphereVerificationParams(int cam, int expected, int timeout, const std::string& m)
        : cameraID(cam), expectedSpheres(expected), timeoutMs(timeout), mode(m) {}
};

/**
 * Union of all possible event parameters
 */
using EventParams = std::variant<
    std::monostate,      // For events with no parameters
    GuidParam,
    CutDirectParams,
    OverlayParams,
    TimerParams,
    ReplaySpeedParams,
    NDISlotParams,
    SceneSwitchParams,
    TextParam,
    SphereVerificationParams,
    PTZPresetParams,
    ESP32CommandParams
>;

/**
 * ChoreographyEvent - Represents a single event in a choreography sequence
 */
struct ChoreographyEvent {
    EventType type;
    EventParams params;
    std::string comment;  // Optional comment for debugging
    
    ChoreographyEvent() : type(EventType::Comment) {}
    ChoreographyEvent(EventType t, EventParams p = std::monostate{})
        : type(t), params(std::move(p)) {}
    
    // Factory methods for easy event creation
    
    // vMix Commands
    static ChoreographyEvent CutDirect(const std::string& guid, int layer = -1) {
        return {EventType::CutDirect, CutDirectParams(guid, layer)};
    }
    
    static ChoreographyEvent QuickPlay(const std::string& guid) {
        return {EventType::QuickPlay, GuidParam(guid)};
    }
    
    static ChoreographyEvent Play(const std::string& guid) {
        return {EventType::Play, GuidParam(guid)};
    }
    
    static ChoreographyEvent Pause(const std::string& guid) {
        return {EventType::Pause, GuidParam(guid)};
    }
    
    static ChoreographyEvent Restart(const std::string& guid) {
        return {EventType::Restart, GuidParam(guid)};
    }
    
    static ChoreographyEvent AudioOn(const std::string& guid) {
        return {EventType::AudioOn, GuidParam(guid)};
    }
    
    static ChoreographyEvent AudioOff(const std::string& guid) {
        return {EventType::AudioOff, GuidParam(guid)};
    }
    
    static ChoreographyEvent StartRecording() {
        return {EventType::StartRecording};
    }
    
    static ChoreographyEvent StopRecording() {
        return {EventType::StopRecording};
    }
    
    static ChoreographyEvent StartMultiCorder() {
        return {EventType::StartMultiCorder};
    }
    
    static ChoreographyEvent StopMultiCorder() {
        return {EventType::StopMultiCorder};
    }
    
    static ChoreographyEvent BrowserReload(const std::string& guid) {
        return {EventType::BrowserReload, GuidParam(guid)};
    }
    
    // Overlay commands
    static ChoreographyEvent OverlayInputIn(const std::string& guid, int layer) {
        return {EventType::OverlayInputIn, OverlayParams(guid, layer)};
    }
    
    static ChoreographyEvent OverlayInputOut(int layer) {
        return {EventType::OverlayInputOut, OverlayParams(layer)};
    }
    
    // Replay commands
    static ChoreographyEvent ReplayStartRecording() {
        return {EventType::ReplayStartRecording};
    }
    
    static ChoreographyEvent ReplayStopRecording() {
        return {EventType::ReplayStopRecording};
    }
    
    static ChoreographyEvent ReplayMarkIn() {
        return {EventType::ReplayMarkIn};
    }
    
    static ChoreographyEvent ReplayMarkOut() {
        return {EventType::ReplayMarkOut};
    }
    
    static ChoreographyEvent ReplayLive() {
        return {EventType::ReplayLive};
    }
    
    static ChoreographyEvent ReplaySetSpeed(int percentage) {
        return {EventType::ReplaySetSpeed, ReplaySpeedParams(percentage)};
    }
    
    static ChoreographyEvent ReplaySelectLastEvent() {
        return {EventType::ReplaySelectLastEvent};
    }
    
    // Internal/Custom
    static ChoreographyEvent Timer(int milliseconds) {
        return {EventType::Timer, TimerParams(milliseconds)};
    }
    
    static ChoreographyEvent NDISlotChange(int slot, int cameraID) {
        return {EventType::NDISlotChange, NDISlotParams(slot, cameraID)};
    }
    
    static ChoreographyEvent SceneSwitch(int configIndex) {
        return {EventType::SceneSwitch, SceneSwitchParams(configIndex)};
    }
    
    // Sphere verification events
    static ChoreographyEvent SpherePresenceCheck(int cameraID, int expectedSpheres, int timeoutMs = 5000) {
        return {EventType::SpherePresenceCheck, SphereVerificationParams(cameraID, expectedSpheres, timeoutMs, "presence")};
    }
    
    static ChoreographyEvent SpherePositionCapture(int cameraID, int timeoutMs = 5000) {
        return {EventType::SpherePositionCapture, SphereVerificationParams(cameraID, 0, timeoutMs, "positions")};
    }
    
    static ChoreographyEvent SphereArrivalWait(int cameraID, int expectedSpheres, int timeoutMs = 60000) {
        return {EventType::SphereArrivalWait, SphereVerificationParams(cameraID, expectedSpheres, timeoutMs, "arrivals")};
    }
    
    // PTZ commands
    static ChoreographyEvent PTZPreset(int cameraNumber, int presetNumber) {
        return {EventType::PTZPreset, PTZPresetParams(cameraNumber, presetNumber)};
    }
    
    // ESP32 commands
    static ChoreographyEvent ESP32Command(const std::string& command) {
        return {EventType::ESP32Command, ESP32CommandParams(command)};
    }
    
    // Meta events
    static ChoreographyEvent Comment(const std::string& text) {
        ChoreographyEvent e{EventType::Comment, TextParam(text)};
        e.comment = text;
        return e;
    }
    
    static ChoreographyEvent Label(const std::string& name) {
        return {EventType::Label, TextParam(name)};
    }
    
    // Utility methods
    std::string GetTypeName() const {
        switch (type) {
            case EventType::CutDirect: return "CutDirect";
            case EventType::QuickPlay: return "QuickPlay";
            case EventType::Play: return "Play";
            case EventType::Pause: return "Pause";
            case EventType::Restart: return "Restart";
            case EventType::AudioOn: return "AudioOn";
            case EventType::AudioOff: return "AudioOff";
            case EventType::StartRecording: return "StartRecording";
            case EventType::StopRecording: return "StopRecording";
            case EventType::StartMultiCorder: return "StartMultiCorder";
            case EventType::StopMultiCorder: return "StopMultiCorder";
            case EventType::BrowserReload: return "BrowserReload";
            case EventType::OverlayInputIn: return "OverlayInputIn";
            case EventType::OverlayInputOut: return "OverlayInputOut";
            case EventType::ReplayStartRecording: return "ReplayStartRecording";
            case EventType::ReplayStopRecording: return "ReplayStopRecording";
            case EventType::ReplayMarkIn: return "ReplayMarkIn";
            case EventType::ReplayMarkOut: return "ReplayMarkOut";
            case EventType::ReplayLive: return "ReplayLive";
            case EventType::ReplaySetSpeed: return "ReplaySetSpeed";
            case EventType::ReplaySelectLastEvent: return "ReplaySelectLastEvent";
            case EventType::Timer: return "Timer";
            case EventType::NDISlotChange: return "NDISlotChange";
            case EventType::SceneSwitch: return "SceneSwitch";
            case EventType::SpherePresenceCheck: return "SpherePresenceCheck";
            case EventType::SpherePositionCapture: return "SpherePositionCapture";
            case EventType::SphereArrivalWait: return "SphereArrivalWait";
            case EventType::PTZPreset: return "PTZPreset";
            case EventType::ESP32Command: return "ESP32Command";
            case EventType::Comment: return "Comment";
            case EventType::Label: return "Label";
            default: return "Unknown";
        }
    }
    
    bool RequiresVMix() const {
        switch (type) {
            case EventType::Timer:
            case EventType::NDISlotChange:
            case EventType::SceneSwitch:
            case EventType::SpherePresenceCheck:
            case EventType::SpherePositionCapture:
            case EventType::SphereArrivalWait:
            case EventType::PTZPreset:
            case EventType::ESP32Command:
            case EventType::Comment:
            case EventType::Label:
                return false;
            default:
                return true;
        }
    }
    
    bool RequiresVideoHub() const {
        return type == EventType::NDISlotChange ||
               type == EventType::SpherePresenceCheck ||
               type == EventType::SpherePositionCapture ||
               type == EventType::SphereArrivalWait;
    }
    
    bool RequiresSceneManager() const {
        return type == EventType::SceneSwitch || type == EventType::NDISlotChange;
    }
    
    bool RequiresSphereVerifier() const {
        return type == EventType::SpherePresenceCheck ||
               type == EventType::SpherePositionCapture ||
               type == EventType::SphereArrivalWait;
    }
};

} // namespace Choreography
