/**
 * ChoreographyEngine.cpp
 * 
 * Implementation of vMix choreography automation engine.
 */

#include "ChoreographyEngine.h"
#include "../control/VMixController.h"
#include "../control/PTZController.h"
#include "../control/TrackPhysicalController.h"
#include "../scene/SceneManager.h"
#include "../control/VideoHubClient.h"
#include "../verification/SphereVerifier.h"
#include "../utils/Logger.h"
#include <sstream>
#include <iomanip>

namespace Choreography {

ChoreographyEngine::ChoreographyEngine(VMixController* vmixController, SceneManager* sceneManager)
    : m_vmixController(vmixController)
    , m_sceneManager(sceneManager)
    , m_videoHub(nullptr)
    , m_ptzController(nullptr)
    , m_trackController(nullptr)
    , m_sphereVerifier(nullptr)
    , m_scriptLoaded(false)
    , m_state(EngineState::Idle)
    , m_currentEventIndex(0)
    , m_stopRequested(false)
    , m_pauseRequested(false)
    , m_skipRequested(false)
    , m_errorCount(0)
    , m_continueOnError(true)
    , m_vmixRequired(false)
{
    Logger::Info("ChoreographyEngine: Initialized");
}

ChoreographyEngine::~ChoreographyEngine() {
    Stop();
    if (m_executionThread.joinable()) {
        m_executionThread.join();
    }
    Logger::Info("ChoreographyEngine: Shutdown");
}

bool ChoreographyEngine::Load(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state == EngineState::Running || m_state == EngineState::Paused) {
        SetError("Cannot load script while running");
        return false;
    }
    
    auto script = m_scriptParser.LoadFromFile(filePath);
    if (!script) {
        SetError("Failed to load script: " + m_scriptParser.GetLastError());
        return false;
    }
    
    m_script = *script;
    m_scriptLoaded = true;
    m_currentEventIndex = 0;
    m_errorCount = 0;
    SetState(EngineState::Ready);
    
    Logger::Info("ChoreographyEngine: Loaded '" + m_script.metadata.name + 
                "' with " + std::to_string(m_script.events.size()) + " events, " +
                "total duration: " + std::to_string(m_script.GetTotalDurationMs()) + "ms");
    
    return true;
}

bool ChoreographyEngine::LoadFromJson(const std::string& jsonStr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state == EngineState::Running || m_state == EngineState::Paused) {
        SetError("Cannot load script while running");
        return false;
    }
    
    auto script = m_scriptParser.LoadFromJsonString(jsonStr);
    if (!script) {
        SetError("Failed to parse JSON: " + m_scriptParser.GetLastError());
        return false;
    }
    
    m_script = *script;
    m_scriptLoaded = true;
    m_currentEventIndex = 0;
    m_errorCount = 0;
    SetState(EngineState::Ready);
    
    return true;
}

bool ChoreographyEngine::LoadFromDsl(const std::string& dslStr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state == EngineState::Running || m_state == EngineState::Paused) {
        SetError("Cannot load script while running");
        return false;
    }
    
    auto script = m_scriptParser.LoadFromDslString(dslStr);
    if (!script) {
        SetError("Failed to parse DSL: " + m_scriptParser.GetLastError());
        return false;
    }
    
    m_script = *script;
    m_scriptLoaded = true;
    m_currentEventIndex = 0;
    m_errorCount = 0;
    SetState(EngineState::Ready);
    
    return true;
}

void ChoreographyEngine::SetScript(const Script& script) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state == EngineState::Running || m_state == EngineState::Paused) {
        Logger::Warning("ChoreographyEngine: Cannot set script while running");
        return;
    }
    
    m_script = script;
    m_scriptLoaded = true;
    m_currentEventIndex = 0;
    m_errorCount = 0;
    SetState(EngineState::Ready);
}

void ChoreographyEngine::ClearScript() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_state == EngineState::Running || m_state == EngineState::Paused) {
        Logger::Warning("ChoreographyEngine: Cannot clear script while running");
        return;
    }
    
    m_script = Script();
    m_scriptLoaded = false;
    m_currentEventIndex = 0;
    SetState(EngineState::Idle);
}

bool ChoreographyEngine::Start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_scriptLoaded) {
        SetError("No script loaded");
        return false;
    }
    
    if (m_state == EngineState::Running) {
        Logger::Warning("ChoreographyEngine: Already running");
        return true;
    }
    
    // Attempt TCP connection when a controller is available but not yet connected
    if (m_vmixController && !m_vmixController->IsTcpConnected()) {
        Logger::Info("ChoreographyEngine: vMix TCP not connected, attempting to connect...");
        if (!m_vmixController->ConnectTcp()) {
            Logger::Warning("ChoreographyEngine: vMix TCP connection failed at start");
            if (m_vmixRequired) {
                SetError("vMix TCP not connected");
                return false;
            }
        }
    }
    
    // Stop any existing thread
    if (m_executionThread.joinable()) {
        m_stopRequested = true;
        m_pauseCV.notify_all();
        m_executionThread.join();
    }
    
    // Reset state
    m_stopRequested = false;
    m_pauseRequested = false;
    m_skipRequested = false;
    m_currentEventIndex = 0;
    m_errorCount = 0;
    m_startTime = std::chrono::steady_clock::now();
    
    // Start execution thread
    m_executionThread = std::thread(&ChoreographyEngine::ExecutionThreadFunc, this);
    
    Logger::Info("ChoreographyEngine: Started execution of '" + m_script.metadata.name + "'");
    Logger::Debug("ChoreographyEngine: Start config - vmixRequired=" + std::to_string(m_vmixRequired) +
                 ", continueOnError=" + std::to_string(m_continueOnError) +
                 ", vmixConnected=" + std::to_string(m_vmixController ? m_vmixController->IsTcpConnected() : false) +
                 ", videoHubAvailable=" + std::to_string(m_videoHub != nullptr));
    return true;
}

void ChoreographyEngine::Stop() {
    Logger::Debug("ChoreographyEngine: Stop() called, current state=" + GetStateString(m_state.load()));
    
    m_stopRequested = true;
    m_pauseCV.notify_all();
    
    // Only join if thread is joinable (prevents double-join)
    if (m_executionThread.joinable()) {
        Logger::Debug("ChoreographyEngine: Waiting for execution thread to finish...");
        m_executionThread.join();
        Logger::Debug("ChoreographyEngine: Execution thread joined");
    }
    
    SetState(EngineState::Ready);
    Logger::Info("ChoreographyEngine: Stopped");
}

void ChoreographyEngine::Pause() {
    if (m_state == EngineState::Running) {
        m_pauseRequested = true;
        size_t currentIdx = m_currentEventIndex.load();
        Logger::Info("ChoreographyEngine: Pause requested at event " + std::to_string(currentIdx));
    } else {
        Logger::Debug("ChoreographyEngine: Pause ignored (not running, state=" + GetStateString(m_state.load()) + ")");
    }
}

void ChoreographyEngine::Resume() {
    if (m_state == EngineState::Paused) {
        m_pauseRequested = false;
        m_pauseCV.notify_all();
        SetState(EngineState::Running);
        size_t currentIdx = m_currentEventIndex.load();
        Logger::Info("ChoreographyEngine: Resuming from event " + std::to_string(currentIdx));
    } else {
        Logger::Debug("ChoreographyEngine: Resume ignored (not paused, state=" + GetStateString(m_state.load()) + ")");
    }
}

void ChoreographyEngine::Skip() {
    m_skipRequested = true;
    size_t currentIdx = m_currentEventIndex.load();
    Logger::Info("ChoreographyEngine: Skip requested for event " + std::to_string(currentIdx));
}

EventResult ChoreographyEngine::ExecuteEvent(const ChoreographyEvent& event) {
    Logger::Debug("ChoreographyEngine: ExecuteEvent called for " + event.GetTypeName());
    return ExecuteEventInternal(event);
}

bool ChoreographyEngine::JumpToEvent(size_t index) {
    if (index >= m_script.events.size()) {
        SetError("Invalid event index: " + std::to_string(index));
        return false;
    }
    
    m_currentEventIndex = index;
    Logger::Info("ChoreographyEngine: Jumped to event " + std::to_string(index));
    return true;
}

EngineStatus ChoreographyEngine::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    EngineStatus status;
    status.state = m_state.load();
    status.scriptName = m_scriptLoaded ? m_script.metadata.name : "";
    status.totalEvents = m_scriptLoaded ? m_script.events.size() : 0;
    status.currentEventIndex = m_currentEventIndex.load();
    status.errorCount = m_errorCount;
    
    // Safely access current event type with bounds check to prevent race condition
    const size_t eventCount = m_scriptLoaded ? m_script.events.size() : 0;
    if (m_scriptLoaded && status.currentEventIndex < eventCount) {
        status.currentEventType = m_script.events[status.currentEventIndex].GetTypeName();
    } else {
        status.currentEventType = "None";
    }
    
    auto now = std::chrono::steady_clock::now();
    status.elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime);
    
    // Calculate remaining time (sum of remaining Timer events) with bounds check
    int remainingMs = 0;
    if (m_scriptLoaded) {
        for (size_t i = status.currentEventIndex; i < eventCount; ++i) {
            if (m_script.events[i].type == EventType::Timer) {
                if (auto* params = std::get_if<TimerParams>(&m_script.events[i].params)) {
                    remainingMs += params->milliseconds;
                }
            }
        }
    }
    status.estimatedRemainingTime = std::chrono::milliseconds(remainingMs);
    
    return status;
}

void ChoreographyEngine::ExecutionThreadFunc() {
    SetState(EngineState::Running);
    Logger::Debug("ChoreographyEngine: Execution thread started, " + 
                 std::to_string(m_script.events.size()) + " events to process");
    
    while (!m_stopRequested && m_currentEventIndex < m_script.events.size()) {
        // Check for pause
        if (m_pauseRequested) {
            SetState(EngineState::Paused);
            std::unique_lock<std::mutex> lock(m_mutex);
            m_pauseCV.wait(lock, [this] { 
                return !m_pauseRequested || m_stopRequested; 
            });
            if (m_stopRequested) break;
            SetState(EngineState::Running);
        }
        
        size_t eventIndex = m_currentEventIndex.load();
        const auto& event = m_script.events[eventIndex];
        
        // Notify event start
        if (m_eventStartCallback) {
            m_eventStartCallback(eventIndex, event);
        }
        
        Logger::Debug("ChoreographyEngine: Executing event " + std::to_string(eventIndex) + 
                     ": " + event.GetTypeName());
        
        // Execute event
        auto startTime = std::chrono::steady_clock::now();
        EventResult result = ExecuteEventInternal(event);
        auto endTime = std::chrono::steady_clock::now();
        result.eventIndex = static_cast<int>(eventIndex);
        result.executionTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        // Notify event complete
        if (m_eventCompleteCallback) {
            m_eventCompleteCallback(result);
        }
        
        // Handle errors
        if (!result.success) {
            m_errorCount++;
            Logger::Error("ChoreographyEngine: Event " + std::to_string(eventIndex) + 
                         " failed: " + result.errorMessage);
            
            if (!m_continueOnError) {
                SetState(EngineState::Error);
                SetError(result.errorMessage);
                break;
            }
        }
        
        // Advance to next event
        m_currentEventIndex++;
        m_skipRequested = false;
    }
    
    if (m_state == EngineState::Running) {
        SetState(EngineState::Ready);
        Logger::Info("ChoreographyEngine: Execution completed, " + 
                    std::to_string(m_errorCount) + " errors");
    }
}

EventResult ChoreographyEngine::ExecuteEventInternal(const ChoreographyEvent& event) {
    EventResult result;
    result.success = true;
    
    switch (event.type) {
        case EventType::Timer: {
            auto* params = std::get_if<TimerParams>(&event.params);
            if (params) {
                PreciseSleep(std::chrono::milliseconds(params->milliseconds));
            }
            break;
        }
        
        case EventType::Comment:
        case EventType::Label:
            // No-op events
            break;
        
        case EventType::NDISlotChange: {
            auto* params = std::get_if<NDISlotParams>(&event.params);
            if (params) {
                result.success = ExecuteNDISlotChange(params->slot, params->cameraID);
                if (!result.success) {
                    result.errorMessage = "Failed to change NDI slot";
                }
            }
            break;
        }
        
        case EventType::SceneSwitch: {
            auto* params = std::get_if<SceneSwitchParams>(&event.params);
            if (params) {
                result.success = ExecuteSceneSwitch(params->configIndex);
                if (!result.success) {
                    result.errorMessage = "Failed to switch scene";
                }
            }
            break;
        }
        
        case EventType::SpherePresenceCheck:
        case EventType::SpherePositionCapture:
        case EventType::SphereArrivalWait: {
            result.success = ExecuteSphereVerification(event, result);
            break;
        }
        
        case EventType::PTZPreset: {
            auto* params = std::get_if<PTZPresetParams>(&event.params);
            if (params) {
                if (!m_ptzController) {
                    Logger::Warning("ChoreographyEngine: PTZPreset event - no PTZController set, skipping");
                    result.success = true;  // Non-fatal: skip gracefully
                } else {
                    // JSON uses 1-indexed camera numbers; PTZController uses 0-indexed IDs
                    int cameraIndex = params->cameraNumber - 1;
                    Logger::Debug("ChoreographyEngine: PTZPreset camera=" +
                                  std::to_string(params->cameraNumber) +
                                  " (index=" + std::to_string(cameraIndex) +
                                  ") preset=" + std::to_string(params->presetNumber));
                    auto viscaResult = m_ptzController->CallPreset(cameraIndex, params->presetNumber);
                    result.success = viscaResult.success;
                    if (!result.success) {
                        result.errorMessage = "PTZ preset failed: " + viscaResult.errorMessage;
                    }
                }
            }
            break;
        }
        
        case EventType::ESP32Command: {
            auto* params = std::get_if<ESP32CommandParams>(&event.params);
            if (params) {
                if (!m_trackController) {
                    Logger::Warning("ChoreographyEngine: ESP32Command event - no TrackPhysicalController set, skipping");
                    result.success = true;  // Non-fatal: skip gracefully
                } else {
                    Logger::Debug("ChoreographyEngine: ESP32Command command=" + params->command);
                    // Map logical command names to TrackPhysicalController methods
                    if (params->command == "iniciar") {
                        result.success = m_trackController->openGate();
                    } else if (params->command == "finalizar") {
                        result.success = m_trackController->closeGates();
                    } else if (params->command == "reload") {
                        result.success = m_trackController->openElevator();
                    } else if (params->command == "test") {
                        result.success = m_trackController->executeTest();
                    } else {
                        Logger::Warning("ChoreographyEngine: Unknown ESP32 command '" +
                                        params->command + "'");
                        result.success = false;
                        result.errorMessage = "Unknown ESP32 command: " + params->command;
                    }
                    if (!result.success && result.errorMessage.empty()) {
                        result.errorMessage = "ESP32 command '" + params->command + "' failed";
                    }
                }
            }
            break;
        }
        
        default: {
            // vMix commands
            std::string command = BuildVMixCommand(event);
            if (!command.empty()) {
                result.success = SendVMixCommand(command);
                if (!result.success) {
                    result.errorMessage = "Failed to send vMix command";
                }
            } else {
                result.success = false;
                result.errorMessage = "Failed to build vMix command";
            }
            break;
        }
    }
    
    return result;
}

std::string ChoreographyEngine::BuildVMixCommand(const ChoreographyEvent& event) {
    std::ostringstream cmd;
    cmd << "FUNCTION ";
    
    switch (event.type) {
        case EventType::CutDirect: {
            auto* params = std::get_if<CutDirectParams>(&event.params);
            if (params) {
                cmd << "CutDirect Input=" << params->guid;
                // Include Layer parameter if specified (>= 0)
                if (params->layer >= 0) {
                    cmd << " Layer=" << params->layer;
                }
                Logger::Debug("ChoreographyEngine: BuildVMixCommand CutDirect Input=" + params->guid + 
                             (params->layer >= 0 ? " Layer=" + std::to_string(params->layer) : ""));
            }
            break;
        }
        
        case EventType::QuickPlay: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "QuickPlay Input=" << params->guid;
            }
            break;
        }
        
        case EventType::Play: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "Play Input=" << params->guid;
            }
            break;
        }
        
        case EventType::Pause: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "Pause Input=" << params->guid;
            }
            break;
        }
        
        case EventType::Restart: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "Restart Input=" << params->guid;
            }
            break;
        }
        
        case EventType::AudioOn: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "AudioOn Input=" << params->guid;
            }
            break;
        }
        
        case EventType::AudioOff: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "AudioOff Input=" << params->guid;
            }
            break;
        }
        
        case EventType::StartRecording:
            cmd << "StartRecording";
            break;
        
        case EventType::StopRecording:
            cmd << "StopRecording";
            break;
        
        case EventType::StartMultiCorder:
            cmd << "StartMultiCorder";
            break;
        
        case EventType::StopMultiCorder:
            cmd << "StopMultiCorder";
            break;
        
        case EventType::BrowserReload: {
            auto* params = std::get_if<GuidParam>(&event.params);
            if (params) {
                cmd << "BrowserReload Input=" << params->guid;
            }
            break;
        }
        
        case EventType::OverlayInputIn: {
            auto* params = std::get_if<OverlayParams>(&event.params);
            if (params) {
                cmd << "OverlayInput" << params->layer << "In Input=" << params->guid;
            }
            break;
        }
        
        case EventType::OverlayInputOut: {
            auto* params = std::get_if<OverlayParams>(&event.params);
            if (params) {
                cmd << "OverlayInput" << params->layer << "Out";
            }
            break;
        }
        
        case EventType::ReplayStartRecording:
            cmd << "ReplayStartRecording";
            break;
        
        case EventType::ReplayStopRecording:
            cmd << "ReplayStopRecording";
            break;
        
        case EventType::ReplayMarkIn:
            cmd << "ReplayMarkIn";
            break;
        
        case EventType::ReplayMarkOut:
            cmd << "ReplayMarkOut";
            break;
        
        case EventType::ReplayLive:
            cmd << "ReplayLive";
            break;
        
        case EventType::ReplaySetSpeed: {
            auto* params = std::get_if<ReplaySpeedParams>(&event.params);
            if (params) {
                cmd << "ReplaySpeed Value=" << params->percentage;
            }
            break;
        }
        
        case EventType::ReplaySelectLastEvent:
            cmd << "ReplaySelectLastEvent";
            break;
        
        default:
            return "";
    }
    
    cmd << "\r\n";
    return cmd.str();
}

bool ChoreographyEngine::SendVMixCommand(const std::string& command) {
    if (!m_vmixController) {
        Logger::Warning("ChoreographyEngine: No VMixController, skipping command: " + 
                       command.substr(0, command.find('\r')));
        return true;  // Not an error if controller not set
    }
    
    // SendTcpCommand handles reconnection internally; no early-return here
    bool success = m_vmixController->SendTcpCommand(command);
    if (success) {
        Logger::Debug("ChoreographyEngine: vMix command sent successfully: " + 
                     command.substr(0, command.find('\r')));
    } else {
        Logger::Error("ChoreographyEngine: vMix send failed for command: " + 
                     command.substr(0, command.find('\r')));
    }
    return success;
}

bool ChoreographyEngine::ExecuteNDISlotChange(int slot, int cameraID) {
    Logger::Debug("ChoreographyEngine: ExecuteNDISlotChange(slot=" + std::to_string(slot) + 
                 ", cameraID=" + std::to_string(cameraID) + ")");
    
    // Validate parameters
    if (slot < 0 || slot > 7) {
        Logger::Error("ChoreographyEngine: Invalid slot index: " + std::to_string(slot) + 
                     " (must be 0-7 for G1-G8)");
        return false;
    }
    if (cameraID < 1 || cameraID > 12) {
        Logger::Error("ChoreographyEngine: Invalid camera ID: " + std::to_string(cameraID) + 
                     " (must be 1-12 for CAM_01-CAM_12)");
        return false;
    }
    
    // NOTE: NDISlotChange currently uses VideoHub directly for individual slot routing.
    // SceneManager's TriggerGroupSwitch() changes all 8 slots at once based on predefined
    // configurations, which is better suited for the SceneSwitch event type.
    // For granular per-slot control, we route directly through VideoHub.
    
    // Try VideoHub directly
    if (m_videoHub) {
        // Build camera name
        std::ostringstream cameraName;
        cameraName << "CAM_" << std::setw(2) << std::setfill('0') << cameraID;
        
        Logger::Debug("ChoreographyEngine: Routing via VideoHub: slot G" + 
                     std::to_string(slot + 1) + " <- " + cameraName.str());
        
        bool success = m_videoHub->RouteInputToOutput(slot, cameraName.str());
        if (success) {
            Logger::Info("ChoreographyEngine: NDI slot change successful via VideoHub: slot G" + 
                        std::to_string(slot + 1) + " -> " + cameraName.str());
        } else {
            Logger::Error("ChoreographyEngine: NDI slot change failed via VideoHub: slot G" + 
                         std::to_string(slot + 1) + " -> " + cameraName.str());
        }
        return success;
    }
    
    // If VideoHub not available but SceneManager is, log info
    if (m_sceneManager) {
        Logger::Info("ChoreographyEngine: NDI slot change logged (no VideoHub): slot G" + 
                    std::to_string(slot + 1) + " -> CAM_" + std::to_string(cameraID));
        // SceneManager doesn't support individual slot routing, would need full config switch
        return true;  // Not an error, just informational
    }
    
    Logger::Warning("ChoreographyEngine: No VideoHub or SceneManager available for NDI slot change");
    return true;  // Not an error, just skip
}

bool ChoreographyEngine::ExecuteSceneSwitch(int configIndex) {
    Logger::Debug("ChoreographyEngine: ExecuteSceneSwitch(configIndex=" + std::to_string(configIndex) + ")");
    
    if (!m_sceneManager) {
        Logger::Warning("ChoreographyEngine: No SceneManager, skipping scene switch to config " + 
                       std::to_string(configIndex));
        return true;
    }
    
    bool success = m_sceneManager->TriggerGroupSwitch(configIndex);
    if (success) {
        Logger::Info("ChoreographyEngine: Switched to scene config " + std::to_string(configIndex));
    } else {
        Logger::Error("ChoreographyEngine: Failed to switch to scene config " + std::to_string(configIndex));
    }
    return success;
}

void ChoreographyEngine::PreciseSleep(std::chrono::milliseconds duration) {
    auto start = std::chrono::high_resolution_clock::now();
    auto end = start + duration;
    
    Logger::Debug("ChoreographyEngine: PreciseSleep starting for " + std::to_string(duration.count()) + "ms");
    
    // Use a combination of sleep and spin-wait for precision
    // Sleep for most of the duration, then spin for the last few ms
    auto spinThreshold = std::chrono::milliseconds(5);
    auto sleepDuration = duration - spinThreshold;
    
    if (sleepDuration > std::chrono::milliseconds::zero()) {
        // Check for skip/stop every 100ms
        auto checkInterval = std::chrono::milliseconds(100);
        while (std::chrono::high_resolution_clock::now() < start + sleepDuration) {
            if (m_stopRequested || m_skipRequested) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - start);
                Logger::Debug("ChoreographyEngine: PreciseSleep interrupted at " + 
                             std::to_string(elapsed.count()) + "ms" +
                             (m_skipRequested ? " (skip)" : " (stop)"));
                return;
            }
            
            auto remaining = (start + sleepDuration) - std::chrono::high_resolution_clock::now();
            auto sleepTime = (std::min<std::chrono::steady_clock::duration>)(remaining, checkInterval);
            std::this_thread::sleep_for(sleepTime);
        }
    }
    
    // Spin-wait for remaining time (for precision)
    while (std::chrono::high_resolution_clock::now() < end) {
        if (m_stopRequested || m_skipRequested) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start);
            Logger::Debug("ChoreographyEngine: PreciseSleep interrupted at " + 
                         std::to_string(elapsed.count()) + "ms (spin phase)");
            return;
        }
        std::this_thread::yield();
    }
    
    auto actualDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start);
    Logger::Debug("ChoreographyEngine: PreciseSleep completed - requested=" + 
                 std::to_string(duration.count()) + "ms, actual=" + 
                 std::to_string(actualDuration.count()) + "ms");
}

void ChoreographyEngine::SetState(EngineState state) {
    EngineState oldState = m_state.exchange(state);
    
    if (oldState != state) {
        Logger::Debug("ChoreographyEngine: State transition: " + 
                     GetStateString(oldState) + " -> " + GetStateString(state));
        
        if (m_stateCallback) {
            Logger::Debug("ChoreographyEngine: Invoking state callback");
            m_stateCallback(state);
        }
    }
}

void ChoreographyEngine::SetError(const std::string& error) {
    m_lastError = error;
    // Note: m_errorCount is incremented in ExecutionThreadFunc, not here
    // This function is also called for non-execution errors (e.g., load failures)
    Logger::Error("ChoreographyEngine: " + error);
    
    if (m_errorCallback) {
        Logger::Debug("ChoreographyEngine: Invoking error callback");
        m_errorCallback(error);
    }
}

std::string ChoreographyEngine::GetStateString(EngineState state) const {
    switch (state) {
        case EngineState::Idle: return "Idle";
        case EngineState::Ready: return "Ready";
        case EngineState::Running: return "Running";
        case EngineState::Paused: return "Paused";
        case EngineState::Stopping: return "Stopping";
        case EngineState::Error: return "Error";
        default: return "Unknown";
    }
}

bool ChoreographyEngine::ExecuteSphereVerification(const ChoreographyEvent& event, EventResult& result) {
    if (!m_sphereVerifier) {
        result.errorMessage = "SphereVerifier not available";
        Logger::Error("ChoreographyEngine: " + result.errorMessage);
        return false;
    }
    
    auto* params = std::get_if<SphereVerificationParams>(&event.params);
    if (!params) {
        result.errorMessage = "Invalid sphere verification parameters";
        Logger::Error("ChoreographyEngine: " + result.errorMessage);
        return false;
    }
    
    Logger::Info("ChoreographyEngine: Executing sphere verification - mode=" + params->mode +
                 ", camera=" + std::to_string(params->cameraID) +
                 ", expected=" + std::to_string(params->expectedSpheres) +
                 ", timeout=" + std::to_string(params->timeoutMs) + "ms");
    
    // Execute the appropriate verification based on event type
    Verification::VerificationResult verifyResult;
    
    switch (event.type) {
        case EventType::SpherePresenceCheck:
            verifyResult = m_sphereVerifier->CheckPresence(params->cameraID, params->expectedSpheres, params->timeoutMs);
            break;
        
        case EventType::SpherePositionCapture:
            verifyResult = m_sphereVerifier->CapturePositions(params->cameraID, params->timeoutMs);
            break;
        
        case EventType::SphereArrivalWait:
            verifyResult = m_sphereVerifier->WaitForArrivals(params->cameraID, params->expectedSpheres, params->timeoutMs);
            break;
        
        default:
            result.errorMessage = "Unknown sphere verification event type";
            Logger::Error("ChoreographyEngine: " + result.errorMessage);
            return false;
    }
    
    // Log the result
    if (verifyResult.success) {
        Logger::Info("ChoreographyEngine: Sphere verification succeeded - detected " + 
                    std::to_string(verifyResult.spheresDetected) + " spheres");
    } else {
        result.errorMessage = verifyResult.errorMessage;
        Logger::Error("ChoreographyEngine: Sphere verification failed - " + verifyResult.errorMessage);
    }
    
    return verifyResult.success;
}

} // namespace Choreography
