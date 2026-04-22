/**
 * ChoreographyEngine.h
 * 
 * vMix choreography automation engine.
 * Executes sequences of vMix commands, NDI routing changes, and scene switches.
 * 
 * Features:
 * - High-resolution timer for precise event timing
 * - Non-blocking execution in dedicated thread
 * - Integration with VMixController, SceneManager, and VideoHub
 * - Event callbacks for status monitoring
 * 
 * Usage:
 *   ChoreographyEngine engine(&vmixController, &sceneManager);
 *   engine.Load("choreography.json");
 *   engine.Start();
 *   // ... wait for completion or stop
 *   engine.Stop();
 */

#pragma once

#include "ChoreographyEvent.h"
#include "ChoreographyScript.h"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <string>

// Forward declarations
class VMixController;
class SceneManager;
class VideoHubClient;
class TrackPhysicalController;
namespace PTZ { class PTZController; }

namespace Verification {
    class SphereVerifier;
}

namespace Choreography {

/**
 * Engine state
 */
enum class EngineState {
    Idle,       // No script loaded or stopped
    Ready,      // Script loaded, ready to start
    Running,    // Executing events
    Paused,     // Paused mid-execution
    Stopping,   // Stop requested, finishing current event
    Error       // Error occurred
};

/**
 * Event execution result
 */
struct EventResult {
    bool success;
    std::string errorMessage;
    int eventIndex;
    std::chrono::milliseconds executionTime;
};

/**
 * Engine status for monitoring
 */
struct EngineStatus {
    EngineState state;
    std::string scriptName;
    size_t totalEvents;
    size_t currentEventIndex;
    std::string currentEventType;
    std::chrono::milliseconds elapsedTime;
    std::chrono::milliseconds estimatedRemainingTime;
    int errorCount;
};

/**
 * Callback types
 */
using StateCallback = std::function<void(EngineState newState)>;
using EventStartCallback = std::function<void(size_t index, const ChoreographyEvent& event)>;
using EventCompleteCallback = std::function<void(const EventResult& result)>;
using ErrorCallback = std::function<void(const std::string& error)>;

/**
 * ChoreographyEngine - Main automation engine
 */
class ChoreographyEngine {
public:
    /**
     * Constructor
     * @param vmixController VMix controller for vMix commands (can be null for testing)
     * @param sceneManager Scene manager for NDI routing and scene switches (can be null)
     */
    ChoreographyEngine(VMixController* vmixController = nullptr, 
                       SceneManager* sceneManager = nullptr);
    
    /**
     * Destructor - stops execution if running
     */
    ~ChoreographyEngine();
    
    // Non-copyable, non-movable
    ChoreographyEngine(const ChoreographyEngine&) = delete;
    ChoreographyEngine& operator=(const ChoreographyEngine&) = delete;
    
    // Script management
    
    /**
     * Load script from file (JSON or DSL format)
     * @param filePath Path to script file
     * @return true if loaded successfully
     */
    bool Load(const std::string& filePath);
    
    /**
     * Load script from string (JSON format)
     * @param jsonStr JSON script content
     * @return true if loaded successfully
     */
    bool LoadFromJson(const std::string& jsonStr);
    
    /**
     * Load script from DSL string
     * @param dslStr DSL script content
     * @return true if loaded successfully
     */
    bool LoadFromDsl(const std::string& dslStr);
    
    /**
     * Set script directly
     * @param script Script to use
     */
    void SetScript(const Script& script);
    
    /**
     * Clear loaded script
     */
    void ClearScript();
    
    // Execution control
    
    /**
     * Start execution from beginning
     * Non-blocking - runs in dedicated thread
     * @return true if started successfully
     */
    bool Start();
    
    /**
     * Stop execution
     * Waits for current event to complete, then stops
     */
    void Stop();
    
    /**
     * Pause execution
     * Current Timer events will complete before pausing
     */
    void Pause();
    
    /**
     * Resume from paused state
     */
    void Resume();
    
    /**
     * Skip current event (if Timer, jumps to next event)
     */
    void Skip();
    
    /**
     * Execute single event (for manual control)
     * @param event Event to execute
     * @return Result of execution
     */
    EventResult ExecuteEvent(const ChoreographyEvent& event);
    
    /**
     * Jump to specific event index
     * @param index Event index to jump to
     * @return true if jump successful
     */
    bool JumpToEvent(size_t index);
    
    // Status
    
    /**
     * Get current engine state
     */
    EngineState GetState() const { return m_state.load(); }
    
    /**
     * Check if running
     */
    bool IsRunning() const { return m_state.load() == EngineState::Running; }
    
    /**
     * Check if script is loaded
     */
    bool HasScript() const { return m_scriptLoaded; }
    
    /**
     * Get current status
     */
    EngineStatus GetStatus() const;
    
    /**
     * Get last error message
     */
    const std::string& GetLastError() const { return m_lastError; }
    
    // Callbacks
    
    void SetStateCallback(StateCallback callback) { m_stateCallback = callback; }
    void SetEventStartCallback(EventStartCallback callback) { m_eventStartCallback = callback; }
    void SetEventCompleteCallback(EventCompleteCallback callback) { m_eventCompleteCallback = callback; }
    void SetErrorCallback(ErrorCallback callback) { m_errorCallback = callback; }
    
    // Configuration
    
    /**
     * Set whether to continue on errors
     * Default: true (continue execution even if an event fails)
     */
    void SetContinueOnError(bool continueOnError) { m_continueOnError = continueOnError; }
    
    /**
     * Set whether VMix connection is required
     * If true, Start() will fail if VMix is not connected
     * Default: false
     */
    void SetVMixRequired(bool required) { m_vmixRequired = required; }
    
    /**
     * Set VideoHub client for NDI slot changes
     * (Alternative to using SceneManager)
     */
    void SetVideoHubClient(VideoHubClient* client) { m_videoHub = client; }
    
    /**
     * Set PTZ controller for PTZPreset events
     */
    void SetPTZController(PTZ::PTZController* ptz) { m_ptzController = ptz; }
    
    /**
     * Set TrackPhysicalController for ESP32Command events
     */
    void SetTrackPhysicalController(TrackPhysicalController* trackCtrl) { m_trackController = trackCtrl; }
    
    /**
     * Set SphereVerifier for sphere verification events
     */
    void SetSphereVerifier(Verification::SphereVerifier* verifier) { m_sphereVerifier = verifier; }

private:
    // Controllers
    VMixController* m_vmixController;
    SceneManager* m_sceneManager;
    VideoHubClient* m_videoHub;
    PTZ::PTZController* m_ptzController;
    TrackPhysicalController* m_trackController;
    Verification::SphereVerifier* m_sphereVerifier;
    
    // Script
    Script m_script;
    bool m_scriptLoaded;
    ChoreographyScript m_scriptParser;
    
    // State
    std::atomic<EngineState> m_state;
    std::atomic<size_t> m_currentEventIndex;
    std::atomic<bool> m_stopRequested;
    std::atomic<bool> m_pauseRequested;
    std::atomic<bool> m_skipRequested;
    std::chrono::steady_clock::time_point m_startTime;
    int m_errorCount;
    std::string m_lastError;
    
    // Threading
    std::thread m_executionThread;
    mutable std::mutex m_mutex;
    std::condition_variable m_pauseCV;
    
    // Callbacks
    StateCallback m_stateCallback;
    EventStartCallback m_eventStartCallback;
    EventCompleteCallback m_eventCompleteCallback;
    ErrorCallback m_errorCallback;
    
    // Configuration
    bool m_continueOnError;
    bool m_vmixRequired;
    
    // Internal methods
    void ExecutionThreadFunc();
    EventResult ExecuteEventInternal(const ChoreographyEvent& event);
    void SetState(EngineState state);
    void SetError(const std::string& error);
    
    // Event execution helpers
    bool SendVMixCommand(const std::string& command);
    std::string BuildVMixCommand(const ChoreographyEvent& event);
    bool ExecuteNDISlotChange(int slot, int cameraID);
    bool ExecuteSceneSwitch(int configIndex);
    bool ExecuteSphereVerification(const ChoreographyEvent& event, EventResult& result);
    
    // High-resolution timer
    void PreciseSleep(std::chrono::milliseconds duration);
    
    // Utility
    std::string GetStateString(EngineState state) const;
};

} // namespace Choreography
