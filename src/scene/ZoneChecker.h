/**
 * ZoneChecker.h
 * 
 * Detects when tracked objects (leader) cross zone boundaries
 * Used for event-based scene triggering
 * 
 * Architecture:
 * - Defines zones as track segments (e.g., ZONE_START: 0-25m)
 * - Detects zone entry/exit events
 * - Triggers configuration changes based on zone crossings
 * 
 * Performance target: <0.5ms for zone checking
 */

#pragma once

#include <vector>
#include <string>
#include <array>
#include <mutex>
#include <functional>
#include <cstdint>

/**
 * Zone types for different track segments
 */
enum class ZoneType {
    ZONE_START,     // Starting area (0-25m default)
    ZONE_MID,       // Middle section (25-60m default)
    ZONE_FINISH,    // Finish area (60-100m default)
    ZONE_UNKNOWN    // Position outside defined zones
};

/**
 * Zone definition structure
 */
struct ZoneDefinition {
    ZoneType type;
    std::string name;               // Human-readable name
    float rangeMinMeters;           // Start of zone in track meters
    float rangeMaxMeters;           // End of zone in track meters
    std::string triggersConfig;     // Config name to trigger (e.g., "config_a")
    std::vector<int> coveredByPtz;  // PTZ cameras that cover this zone
    
    ZoneDefinition()
        : type(ZoneType::ZONE_UNKNOWN)
        , name("unknown")
        , rangeMinMeters(0.0f)
        , rangeMaxMeters(0.0f)
        , triggersConfig("")
        , coveredByPtz{}
    {}
    
    ZoneDefinition(ZoneType t, const std::string& n, float minM, float maxM, 
                   const std::string& cfg, const std::vector<int>& ptz = {})
        : type(t)
        , name(n)
        , rangeMinMeters(minM)
        , rangeMaxMeters(maxM)
        , triggersConfig(cfg)
        , coveredByPtz(ptz)
    {}
};

/**
 * Zone crossing event
 */
struct ZoneCrossing {
    ZoneType fromZone;              // Previous zone
    ZoneType toZone;                // New zone
    float positionMeters;           // Position where crossing occurred
    int64_t timestamp;              // When the crossing was detected
    float confidence;               // Detection confidence
    std::string suggestedConfig;    // Config to apply based on new zone
    
    ZoneCrossing()
        : fromZone(ZoneType::ZONE_UNKNOWN)
        , toZone(ZoneType::ZONE_UNKNOWN)
        , positionMeters(0.0f)
        , timestamp(0)
        , confidence(0.0f)
        , suggestedConfig("")
    {}
};

/**
 * ZoneChecker configuration
 */
struct ZoneCheckerConfig {
    std::vector<ZoneDefinition> zones;
    bool enabled = true;
    
    // Default configuration with 3 zones
    ZoneCheckerConfig() {
        zones.push_back(ZoneDefinition(
            ZoneType::ZONE_START, "ZONE_START", 
            0.0f, 25.0f, "config_a", {1}
        ));
        zones.push_back(ZoneDefinition(
            ZoneType::ZONE_MID, "ZONE_MID", 
            25.0f, 60.0f, "config_b", {2, 3}
        ));
        zones.push_back(ZoneDefinition(
            ZoneType::ZONE_FINISH, "ZONE_FINISH", 
            60.0f, 100.0f, "config_c", {4}
        ));
    }
};

/**
 * ZoneChecker - Zone boundary crossing detector
 * 
 * Thread safety: All public methods are thread-safe
 */
class ZoneChecker {
public:
    /**
     * Callback for zone crossing events
     */
    using ZoneCrossingCallback = std::function<void(const ZoneCrossing&)>;
    
    /**
     * Constructor
     * @param config Zone checker configuration
     */
    explicit ZoneChecker(const ZoneCheckerConfig& config = ZoneCheckerConfig());
    ~ZoneChecker();
    
    /**
     * Initialize zone checker
     * @return true if successful
     */
    bool Initialize();
    
    /**
     * Update with new leader position
     * Checks for zone crossings and triggers callbacks
     * @param positionMeters Leader X position in global coordinates (meters)
     * @param confidence Detection confidence
     * @param timestamp Current timestamp
     * @return true if a zone crossing was detected
     */
    bool UpdateLeaderPosition(float positionMeters, float confidence, int64_t timestamp);
    
    /**
     * Get current zone for a given position
     * @param positionMeters Position in track meters
     * @return Zone type (or ZONE_UNKNOWN if outside all zones)
     */
    ZoneType GetZoneForPosition(float positionMeters) const;
    
    /**
     * Get zone definition for a given zone type
     * @param zone Zone type
     * @return Zone definition (or empty if not found)
     */
    ZoneDefinition GetZoneDefinition(ZoneType zone) const;
    
    /**
     * Get config name triggered by a zone
     * @param zone Zone type
     * @return Config name (e.g., "config_a")
     */
    std::string GetConfigForZone(ZoneType zone) const;
    
    /**
     * Get current zone of the tracked leader
     */
    ZoneType GetCurrentLeaderZone() const { return m_currentZone; }
    
    /**
     * Get last known leader position
     */
    float GetLastLeaderPosition() const { return m_lastPositionMeters; }
    
    /**
     * Reset zone tracking state
     */
    void Reset();
    
    /**
     * Set callback for zone crossing events
     */
    void SetZoneCrossingCallback(ZoneCrossingCallback callback) { 
        std::lock_guard<std::mutex> lock(m_mutex);
        m_crossingCallback = callback; 
    }
    
    /**
     * Check if enabled
     */
    bool IsEnabled() const { return m_config.enabled; }
    
    /**
     * Get all zone definitions
     */
    std::vector<ZoneDefinition> GetAllZones() const;

private:
    ZoneCheckerConfig m_config;
    ZoneType m_currentZone;
    float m_lastPositionMeters;
    int64_t m_lastTimestamp;
    bool m_initialized;
    mutable std::mutex m_mutex;
    ZoneCrossingCallback m_crossingCallback;
    
    // Helper methods
    void NotifyZoneCrossing(const ZoneCrossing& crossing);
    int64_t GetCurrentTimeMs() const;
};
