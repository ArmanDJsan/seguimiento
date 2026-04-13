/**
 * ChoreographyEngine.cpp
 * 
 * Implementation of vMix choreography automation engine.
 */

#include "ChoreographyEngine.h"
#include "../control/VMixController.h"
#include "../scene/SceneManager.h"
#include "../control/VideoHubClient.h"
#include "../utils/Logger.h"
#include <sstream>
#include <iomanip>

namespace Choreography {

ChoreographyEngine::ChoreographyEngine(VMixController* vmixController, SceneManager* sceneManager)
    : m_vmixController(vmixController)
    , m_sceneManager(sceneManager)
    , m_videoHub(nullptr)
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
    
    if (m_vmixRequired && m_vmixController && !m_vmixController->IsTcpConnected()) {
        SetError("vMix TCP not connected");
        return false;
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
    return true;
}

void ChoreographyEngine::Stop() {
    m_stopRequested = true;
    m_pauseCV.notify_all();
    
    if (m_executionThread.joinable()) {
        m_executionThread.join();
    }
    
    SetState(EngineState::Ready);
    Logger::Info("ChoreographyEngine: Stopped");
}

void ChoreographyEngine::Pause() {
    if (m_state == EngineState::Running) {
        m_pauseRequested = true;
        Logger::Info("ChoreographyEngine: Pause requested");
    }
}

void ChoreographyEngine::Resume() {
    if (m_state == EngineState::Paused) {
        m_pauseRequested = false;
        m_pauseCV.notify_all();
        SetState(EngineState::Running);
        Logger::Info("ChoreographyEngine: Resumed");
    }
}

void ChoreographyEngine::Skip() {
    m_skipRequested = true;
    Logger::Info("ChoreographyEngine: Skip requested");
}

EventResult ChoreographyEngine::ExecuteEvent(const ChoreographyEvent& event) {
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
    
    if (m_scriptLoaded && status.currentEventIndex < m_script.events.size()) {
        status.currentEventType = m_script.events[status.currentEventIndex].GetTypeName();
    }
    
    auto now = std::chrono::steady_clock::now();
    status.elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime);
    
    // Calculate remaining time (sum of remaining Timer events)
    int remainingMs = 0;
    for (size_t i = status.currentEventIndex; i < m_script.events.size(); ++i) {
        if (m_script.events[i].type == EventType::Timer) {
            if (auto* params = std::get_if<TimerParams>(&m_script.events[i].params)) {
                remainingMs += params->milliseconds;
            }
        }
    }
    status.estimatedRemainingTime = std::chrono::milliseconds(remainingMs);
    status.errorCount = m_errorCount;
    
    return status;
}

void ChoreographyEngine::ExecutionThreadFunc() {
    SetState(EngineState::Running);
    
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
    
    if (!m_vmixController->IsTcpConnected()) {
        Logger::Error("ChoreographyEngine: vMix TCP not connected");
        return false;
    }
    
    bool success = m_vmixController->SendTcpCommand(command);
    if (success) {
        Logger::Debug("ChoreographyEngine: Sent vMix command: " + 
                     command.substr(0, command.find('\r')));
    }
    return success;
}

bool ChoreographyEngine::ExecuteNDISlotChange(int slot, int cameraID) {
    // Validate parameters
    if (slot < 0 || slot > 7) {
        Logger::Error("ChoreographyEngine: Invalid slot index: " + std::to_string(slot));
        return false;
    }
    if (cameraID < 1 || cameraID > 12) {
        Logger::Error("ChoreographyEngine: Invalid camera ID: " + std::to_string(cameraID));
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
        
        bool success = m_videoHub->RouteInputToOutput(slot, cameraName.str());
        if (success) {
            Logger::Info("ChoreographyEngine: NDI slot change via VideoHub: slot G" + 
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
    
    Logger::Warning("ChoreographyEngine: No VideoHub available for NDI slot change");
    return true;  // Not an error, just skip
}

bool ChoreographyEngine::ExecuteSceneSwitch(int configIndex) {
    if (!m_sceneManager) {
        Logger::Warning("ChoreographyEngine: No SceneManager, skipping scene switch");
        return true;
    }
    
    bool success = m_sceneManager->TriggerGroupSwitch(configIndex);
    if (success) {
        Logger::Info("ChoreographyEngine: Switched to scene config " + std::to_string(configIndex));
    }
    return success;
}

void ChoreographyEngine::PreciseSleep(std::chrono::milliseconds duration) {
    auto start = std::chrono::high_resolution_clock::now();
    auto end = start + duration;
    
    // Use a combination of sleep and spin-wait for precision
    // Sleep for most of the duration, then spin for the last few ms
    auto spinThreshold = std::chrono::milliseconds(5);
    auto sleepDuration = duration - spinThreshold;
    
    if (sleepDuration > std::chrono::milliseconds::zero()) {
        // Check for skip/stop every 100ms
        auto checkInterval = std::chrono::milliseconds(100);
        while (std::chrono::high_resolution_clock::now() < start + sleepDuration) {
            if (m_stopRequested || m_skipRequested) {
                return;
            }
            
            auto remaining = (start + sleepDuration) - std::chrono::high_resolution_clock::now();
            auto sleepTime = std::min(remaining, checkInterval);
            std::this_thread::sleep_for(sleepTime);
        }
    }
    
    // Spin-wait for remaining time (for precision)
    while (std::chrono::high_resolution_clock::now() < end) {
        if (m_stopRequested || m_skipRequested) {
            return;
        }
        std::this_thread::yield();
    }
}

void ChoreographyEngine::SetState(EngineState state) {
    EngineState oldState = m_state.exchange(state);
    
    if (oldState != state) {
        Logger::Debug("ChoreographyEngine: State changed from " + 
                     GetStateString(oldState) + " to " + GetStateString(state));
        
        if (m_stateCallback) {
            m_stateCallback(state);
        }
    }
}

void ChoreographyEngine::SetError(const std::string& error) {
    m_lastError = error;
    Logger::Error("ChoreographyEngine: " + error);
    
    if (m_errorCallback) {
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

} // namespace Choreography
