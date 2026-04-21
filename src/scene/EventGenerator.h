/**
 * EventGenerator.h
 * 
 * Generates events for scene management based on tracking data and external triggers
 * Central event hub connecting ZoneChecker, BallTracker and SceneManager
 * 
 * Event types:
 * - ZONE_ENTRY: Leader entered a new zone
 * - LEADER_DETECTED: Leader identified in a specific camera
 * - EXTERNAL_TRIGGER: Manual trigger from camera or operator
 * 
 * Performance target: <1ms for event generation and dispatch
 */

#pragma once

#include "ZoneChecker.h"
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <functional>
#include <atomic>
#include <cstdint>

/**
 * Event types
 */
enum class EventType {
    ZONE_ENTRY,         // Leader crossed into a new zone
    LEADER_DETECTED,    // Leader detected in specific camera
    EXTERNAL_TRIGGER,   // Manual or external system trigger
    CONFIG_CHANGE       // Configuration change request
};

/**
 * Scene event structure
 */
struct SceneEvent {
    EventType type;
    int64_t timestamp;
    
    // ZONE_ENTRY fields
    ZoneType zone;                  // Target zone
    float positionMeters;           // Position where event occurred
    std::string suggestedConfig;    // Suggested config name
    float confidence;               // Detection confidence
    
    // LEADER_DETECTED fields
    int leaderId;                   // Track ID of the leader
    int cameraId;                   // Camera that detected the leader
    float leaderX;                  // Leader X position (meters)
    float leaderY;                  // Leader Y position (meters)
    float velocity;                 // Estimated velocity
    
    // EXTERNAL_TRIGGER fields
    std::string source;             // Source of trigger (e.g., "camera_1", "operator", "timing_system")
    std::string configOverride;     // Config to force (for external triggers)
    
    SceneEvent()
        : type(EventType::ZONE_ENTRY)
        , timestamp(0)
        , zone(ZoneType::ZONE_UNKNOWN)
        , positionMeters(0.0f)
        , suggestedConfig("")
        , confidence(0.0f)
        , leaderId(-1)
        , cameraId(-1)
        , leaderX(0.0f)
        , leaderY(0.0f)
        , velocity(0.0f)
        , source("")
        , configOverride("")
    {}
    
    /**
     * Create a ZONE_ENTRY event
     */
    static SceneEvent CreateZoneEntry(ZoneType zone, float position, 
                                       const std::string& config, float conf, int64_t ts) {
        SceneEvent event;
        event.type = EventType::ZONE_ENTRY;
        event.zone = zone;
        event.positionMeters = position;
        event.suggestedConfig = config;
        event.confidence = conf;
        event.timestamp = ts;
        return event;
    }
    
    /**
     * Create a LEADER_DETECTED event
     */
    static SceneEvent CreateLeaderDetected(int leaderId, int cameraId,
                                            float x, float y, float vel, 
                                            float conf, int64_t ts) {
        SceneEvent event;
        event.type = EventType::LEADER_DETECTED;
        event.leaderId = leaderId;
        event.cameraId = cameraId;
        event.leaderX = x;
        event.leaderY = y;
        event.velocity = vel;
        event.confidence = conf;
        event.timestamp = ts;
        return event;
    }
    
    /**
     * Create an EXTERNAL_TRIGGER event
     */
    static SceneEvent CreateExternalTrigger(const std::string& source,
                                             const std::string& config, int64_t ts) {
        SceneEvent event;
        event.type = EventType::EXTERNAL_TRIGGER;
        event.source = source;
        event.configOverride = config;
        event.timestamp = ts;
        return event;
    }
};

/**
 * EventGenerator configuration
 */
struct EventGeneratorConfig {
    bool enabled = true;
    int eventCooldownMs = 500;      // Minimum time between similar events
    int hysteresisFrames = 3;       // Frames required to confirm zone change
    float minConfidence = 0.5f;     // Minimum confidence to generate event
    int maxQueuedEvents = 100;      // Maximum events in queue before dropping
};

/**
 * EventGenerator - Central event hub
 * 
 * Thread safety: All public methods are thread-safe
 */
class EventGenerator {
public:
    /**
     * Callback for scene events
     */
    using EventCallback = std::function<void(const SceneEvent&)>;
    
    /**
     * Constructor
     * @param config EventGenerator configuration
     */
    explicit EventGenerator(const EventGeneratorConfig& config = EventGeneratorConfig());
    ~EventGenerator();
    
    /**
     * Initialize event generator
     * @return true if successful
     */
    bool Initialize();
    
    /**
     * Process zone crossing from ZoneChecker
     * @param crossing Zone crossing data
     */
    void OnZoneCrossing(const ZoneCrossing& crossing);
    
    /**
     * Process leader update from BallTracker
     * @param leaderId Track ID of the leader
     * @param cameraId Camera that detected the leader
     * @param x Leader X position (meters)
     * @param y Leader Y position (meters)
     * @param velocity Estimated velocity (m/s)
     * @param confidence Detection confidence
     */
    void OnLeaderUpdate(int leaderId, int cameraId, float x, float y, 
                        float velocity, float confidence);
    
    /**
     * Process external trigger (from camera, operator, or timing system)
     * @param source Source identifier
     * @param config Config to force (optional, empty = use auto)
     */
    void OnExternalTrigger(const std::string& source, const std::string& config = "");
    
    /**
     * Get next pending event
     * @param outEvent Output event
     * @return true if event was available
     */
    bool PopEvent(SceneEvent& outEvent);
    
    /**
     * Check if there are pending events
     */
    bool HasPendingEvents() const;
    
    /**
     * Get number of pending events
     */
    int GetPendingEventCount() const;
    
    /**
     * Clear all pending events
     */
    void ClearEvents();
    
    /**
     * Set callback for immediate event notification
     * @param callback Callback function
     */
    void SetEventCallback(EventCallback callback);
    
    /**
     * Check if enabled
     */
    bool IsEnabled() const { return m_config.enabled; }
    
    /**
     * Reset state (clears hysteresis counters and cooldowns)
     */
    void Reset();
    
    /**
     * Get statistics
     */
    struct Stats {
        int64_t totalEventsGenerated;
        int64_t zoneEntryEvents;
        int64_t leaderDetectedEvents;
        int64_t externalTriggerEvents;
        int64_t droppedEvents;
        int64_t cooldownSkipped;
    };
    Stats GetStats() const;

private:
    EventGeneratorConfig m_config;
    mutable std::mutex m_mutex;
    std::queue<SceneEvent> m_eventQueue;
    EventCallback m_eventCallback;
    bool m_initialized;
    
    // Cooldown tracking
    int64_t m_lastZoneEventTime;
    int64_t m_lastLeaderEventTime;
    
    // Hysteresis tracking for zone changes
    ZoneType m_pendingZone;
    int m_zoneConfirmationFrames;
    
    // Statistics
    mutable std::mutex m_statsMutex;
    Stats m_stats;
    
    // Helper methods
    bool IsOnCooldown(EventType type) const;
    void UpdateCooldown(EventType type, int64_t timestamp);
    void EnqueueEvent(const SceneEvent& event);
    int64_t GetCurrentTimeMs() const;
    void UpdateStats(EventType type, bool dropped);
};
