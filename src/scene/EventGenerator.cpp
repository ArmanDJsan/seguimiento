/**
 * EventGenerator.cpp
 * 
 * Implementation of event generation for scene management
 */

#include "EventGenerator.h"
#include "../utils/Logger.h"
#include <sstream>
#include <chrono>

EventGenerator::EventGenerator(const EventGeneratorConfig& config)
    : m_config(config)
    , m_initialized(false)
    , m_lastZoneEventTime(0)
    , m_lastLeaderEventTime(0)
    , m_pendingZone(ZoneType::ZONE_UNKNOWN)
    , m_zoneConfirmationFrames(0)
{
    // Initialize stats
    m_stats = {};
}

EventGenerator::~EventGenerator() = default;

bool EventGenerator::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled) {
        Logger::Info("EventGenerator: Disabled by configuration");
        m_initialized = true;
        return true;
    }
    
    Logger::Info("EventGenerator: Initialized with cooldown=" + 
                std::to_string(m_config.eventCooldownMs) + "ms, " +
                "hysteresis=" + std::to_string(m_config.hysteresisFrames) + " frames");
    
    m_initialized = true;
    return true;
}

void EventGenerator::OnZoneCrossing(const ZoneCrossing& crossing) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled || !m_initialized) {
        return;
    }
    
    // Check confidence threshold
    if (crossing.confidence < m_config.minConfidence) {
        Logger::Debug("EventGenerator: Zone crossing ignored - low confidence (" + 
                     std::to_string(crossing.confidence) + " < " +
                     std::to_string(m_config.minConfidence) + ")");
        return;
    }
    
    // Hysteresis: require multiple frames to confirm zone change
    if (crossing.toZone == m_pendingZone) {
        m_zoneConfirmationFrames++;
    } else {
        m_pendingZone = crossing.toZone;
        m_zoneConfirmationFrames = 1;
    }
    
    if (m_zoneConfirmationFrames < m_config.hysteresisFrames) {
        Logger::Debug("EventGenerator: Zone crossing pending confirmation (" + 
                     std::to_string(m_zoneConfirmationFrames) + "/" +
                     std::to_string(m_config.hysteresisFrames) + ")");
        return;
    }
    
    // Check cooldown
    if (IsOnCooldown(EventType::ZONE_ENTRY)) {
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        m_stats.cooldownSkipped++;
        Logger::Debug("EventGenerator: Zone event skipped - on cooldown");
        return;
    }
    
    // Create and enqueue event
    SceneEvent event = SceneEvent::CreateZoneEntry(
        crossing.toZone,
        crossing.positionMeters,
        crossing.suggestedConfig,
        crossing.confidence,
        crossing.timestamp
    );
    
    EnqueueEvent(event);
    UpdateCooldown(EventType::ZONE_ENTRY, crossing.timestamp);
    
    // Reset hysteresis
    m_pendingZone = ZoneType::ZONE_UNKNOWN;
    m_zoneConfirmationFrames = 0;
    
    Logger::Info("EventGenerator: ZONE_ENTRY event generated -> " + crossing.suggestedConfig);
}

void EventGenerator::OnLeaderUpdate(int leaderId, int cameraId, float x, float y,
                                     float velocity, float confidence) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled || !m_initialized) {
        return;
    }
    
    // Check confidence threshold
    if (confidence < m_config.minConfidence) {
        return;
    }
    
    // Check cooldown (leader events have higher rate limit)
    if (IsOnCooldown(EventType::LEADER_DETECTED)) {
        return;
    }
    
    int64_t timestamp = GetCurrentTimeMs();
    
    SceneEvent event = SceneEvent::CreateLeaderDetected(
        leaderId, cameraId, x, y, velocity, confidence, timestamp
    );
    
    EnqueueEvent(event);
    UpdateCooldown(EventType::LEADER_DETECTED, timestamp);
    
    Logger::Debug("EventGenerator: LEADER_DETECTED event - ball " + 
                 std::to_string(leaderId) + " at (" + 
                 std::to_string(x) + ", " + std::to_string(y) + ")");
}

void EventGenerator::OnExternalTrigger(const std::string& source, const std::string& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled || !m_initialized) {
        return;
    }
    
    int64_t timestamp = GetCurrentTimeMs();
    
    SceneEvent event = SceneEvent::CreateExternalTrigger(source, config, timestamp);
    
    EnqueueEvent(event);
    
    Logger::Info("EventGenerator: EXTERNAL_TRIGGER from '" + source + "'" +
                (config.empty() ? "" : " -> config '" + config + "'"));
}

bool EventGenerator::PopEvent(SceneEvent& outEvent) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_eventQueue.empty()) {
        return false;
    }
    
    outEvent = m_eventQueue.front();
    m_eventQueue.pop();
    return true;
}

bool EventGenerator::HasPendingEvents() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_eventQueue.empty();
}

int EventGenerator::GetPendingEventCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_eventQueue.size());
}

void EventGenerator::ClearEvents() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_eventQueue.empty()) {
        m_eventQueue.pop();
    }
    Logger::Debug("EventGenerator: Event queue cleared");
}

void EventGenerator::SetEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_eventCallback = callback;
}

void EventGenerator::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    while (!m_eventQueue.empty()) {
        m_eventQueue.pop();
    }
    
    m_lastZoneEventTime = 0;
    m_lastLeaderEventTime = 0;
    m_pendingZone = ZoneType::ZONE_UNKNOWN;
    m_zoneConfirmationFrames = 0;
    
    {
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        m_stats = {};
    }
    
    Logger::Info("EventGenerator: State reset");
}

EventGenerator::Stats EventGenerator::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

bool EventGenerator::IsOnCooldown(EventType type) const {
    // Note: Caller must hold m_mutex
    int64_t now = GetCurrentTimeMs();
    int64_t lastTime = 0;
    
    switch (type) {
        case EventType::ZONE_ENTRY:
            lastTime = m_lastZoneEventTime;
            break;
        case EventType::LEADER_DETECTED:
            lastTime = m_lastLeaderEventTime;
            break;
        default:
            return false;  // No cooldown for other types
    }
    
    return (now - lastTime) < m_config.eventCooldownMs;
}

void EventGenerator::UpdateCooldown(EventType type, int64_t timestamp) {
    // Note: Caller must hold m_mutex
    switch (type) {
        case EventType::ZONE_ENTRY:
            m_lastZoneEventTime = timestamp;
            break;
        case EventType::LEADER_DETECTED:
            m_lastLeaderEventTime = timestamp;
            break;
        default:
            break;
    }
}

void EventGenerator::EnqueueEvent(const SceneEvent& event) {
    // Note: Caller must hold m_mutex
    
    // Check queue size limit
    if (static_cast<int>(m_eventQueue.size()) >= m_config.maxQueuedEvents) {
        Logger::Warning("EventGenerator: Queue full, dropping oldest event");
        m_eventQueue.pop();
        UpdateStats(event.type, true);
    }
    
    m_eventQueue.push(event);
    UpdateStats(event.type, false);
    
    // Notify callback immediately (for synchronous processing)
    if (m_eventCallback) {
        m_eventCallback(event);
    }
}

int64_t EventGenerator::GetCurrentTimeMs() const {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}

void EventGenerator::UpdateStats(EventType type, bool dropped) {
    // Note: Must acquire m_statsMutex separately
    std::lock_guard<std::mutex> lock(m_statsMutex);
    
    m_stats.totalEventsGenerated++;
    
    if (dropped) {
        m_stats.droppedEvents++;
        return;
    }
    
    switch (type) {
        case EventType::ZONE_ENTRY:
            m_stats.zoneEntryEvents++;
            break;
        case EventType::LEADER_DETECTED:
            m_stats.leaderDetectedEvents++;
            break;
        case EventType::EXTERNAL_TRIGGER:
            m_stats.externalTriggerEvents++;
            break;
        default:
            break;
    }
}
