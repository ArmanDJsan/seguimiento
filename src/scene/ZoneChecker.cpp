/**
 * ZoneChecker.cpp
 * 
 * Implementation of zone boundary crossing detection
 */

#include "ZoneChecker.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <sstream>
#include <chrono>

ZoneChecker::ZoneChecker(const ZoneCheckerConfig& config)
    : m_config(config)
    , m_currentZone(ZoneType::ZONE_UNKNOWN)
    , m_lastPositionMeters(0.0f)
    , m_lastTimestamp(0)
    , m_initialized(false)
{
}

ZoneChecker::~ZoneChecker() = default;

bool ZoneChecker::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled) {
        Logger::Info("ZoneChecker: Disabled by configuration");
        m_initialized = true;
        return true;
    }
    
    // Validate zones
    if (m_config.zones.empty()) {
        Logger::Error("ZoneChecker: No zones defined");
        return false;
    }
    
    // Sort zones by start position
    std::sort(m_config.zones.begin(), m_config.zones.end(),
              [](const ZoneDefinition& a, const ZoneDefinition& b) {
                  return a.rangeMinMeters < b.rangeMinMeters;
              });
    
    // Log configuration
    Logger::Info("ZoneChecker: Initialized with " + std::to_string(m_config.zones.size()) + " zones:");
    for (const auto& zone : m_config.zones) {
        std::ostringstream oss;
        oss << "  " << zone.name << ": " 
            << zone.rangeMinMeters << "m - " << zone.rangeMaxMeters << "m"
            << " -> triggers '" << zone.triggersConfig << "'";
        Logger::Info(oss.str());
    }
    
    m_initialized = true;
    return true;
}

bool ZoneChecker::UpdateLeaderPosition(float positionMeters, float confidence, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (!m_config.enabled || !m_initialized) {
        return false;
    }
    
    // Determine current zone
    ZoneType newZone = ZoneType::ZONE_UNKNOWN;
    std::string newConfig = "";
    
    for (const auto& zone : m_config.zones) {
        if (positionMeters >= zone.rangeMinMeters && positionMeters < zone.rangeMaxMeters) {
            newZone = zone.type;
            newConfig = zone.triggersConfig;
            break;
        }
    }
    
    // Check if zone changed
    bool crossingDetected = false;
    if (newZone != m_currentZone && newZone != ZoneType::ZONE_UNKNOWN) {
        // Zone crossing detected
        ZoneCrossing crossing;
        crossing.fromZone = m_currentZone;
        crossing.toZone = newZone;
        crossing.positionMeters = positionMeters;
        crossing.timestamp = timestamp;
        crossing.confidence = confidence;
        crossing.suggestedConfig = newConfig;
        
        // Get zone names for logging
        std::string fromName = (m_currentZone == ZoneType::ZONE_UNKNOWN) ? "OUTSIDE" : 
                               GetZoneDefinition(m_currentZone).name;
        std::string toName = GetZoneDefinition(newZone).name;
        
        Logger::Info("ZoneChecker: Zone crossing detected: " + fromName + " -> " + toName +
                    " at " + std::to_string(positionMeters) + "m" +
                    ", suggesting config '" + newConfig + "'");
        
        // Update state BEFORE callback to prevent re-entry issues
        m_currentZone = newZone;
        m_lastPositionMeters = positionMeters;
        m_lastTimestamp = timestamp;
        
        // Notify callback
        NotifyZoneCrossing(crossing);
        crossingDetected = true;
    } else {
        // No crossing, just update position
        m_lastPositionMeters = positionMeters;
        m_lastTimestamp = timestamp;
        if (newZone != ZoneType::ZONE_UNKNOWN) {
            m_currentZone = newZone;
        }
    }
    
    return crossingDetected;
}

ZoneType ZoneChecker::GetZoneForPosition(float positionMeters) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (const auto& zone : m_config.zones) {
        if (positionMeters >= zone.rangeMinMeters && positionMeters < zone.rangeMaxMeters) {
            return zone.type;
        }
    }
    
    return ZoneType::ZONE_UNKNOWN;
}

ZoneDefinition ZoneChecker::GetZoneDefinition(ZoneType zone) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (const auto& z : m_config.zones) {
        if (z.type == zone) {
            return z;
        }
    }
    
    return ZoneDefinition();
}

std::string ZoneChecker::GetConfigForZone(ZoneType zone) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    for (const auto& z : m_config.zones) {
        if (z.type == zone) {
            return z.triggersConfig;
        }
    }
    
    return "";
}

void ZoneChecker::Reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_currentZone = ZoneType::ZONE_UNKNOWN;
    m_lastPositionMeters = 0.0f;
    m_lastTimestamp = 0;
    
    Logger::Info("ZoneChecker: State reset");
}

std::vector<ZoneDefinition> ZoneChecker::GetAllZones() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.zones;
}

void ZoneChecker::NotifyZoneCrossing(const ZoneCrossing& crossing) {
    // Note: Caller must hold m_mutex, callback is invoked with lock held
    // This is intentional for thread safety, but callbacks should be lightweight
    if (m_crossingCallback) {
        m_crossingCallback(crossing);
    }
}

int64_t ZoneChecker::GetCurrentTimeMs() const {
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
    return ms.count();
}
