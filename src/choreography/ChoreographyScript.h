/**
 * ChoreographyScript.h
 * 
 * Parser for choreography scripts in JSON and DSL formats.
 * 
 * Supported formats:
 * 
 * JSON:
 * {
 *   "name": "My Choreography",
 *   "events": [
 *     { "type": "CutDirect", "guid": "xxx-xxx", "layer": -1 },
 *     { "type": "Timer", "ms": 2000 },
 *     { "type": "NDISlotChange", "slot": 1, "camera": 3 }
 *   ]
 * }
 * 
 * DSL:
 * *CutDirect(guid, layer)
 * *Timer(2000)
 * *NDISlotChange(1, 3)
 */

#pragma once

#include "ChoreographyEvent.h"
#include <vector>
#include <string>
#include <optional>

namespace Choreography {

/**
 * Script metadata
 */
struct ScriptMetadata {
    std::string name;
    std::string description;
    std::string author;
    std::string version;
    
    ScriptMetadata() : name("Unnamed"), version("1.0") {}
};

/**
 * Parsed choreography script
 */
struct Script {
    ScriptMetadata metadata;
    std::vector<ChoreographyEvent> events;
    
    bool IsValid() const { return !events.empty(); }
    size_t EventCount() const { return events.size(); }
    
    // Calculate total duration (sum of all Timer events)
    int GetTotalDurationMs() const {
        int total = 0;
        for (const auto& event : events) {
            if (event.type == EventType::Timer) {
                if (auto* params = std::get_if<TimerParams>(&event.params)) {
                    total += params->milliseconds;
                }
            }
        }
        return total;
    }
};

/**
 * ChoreographyScript - Parser for choreography script files
 */
class ChoreographyScript {
public:
    ChoreographyScript() = default;
    
    /**
     * Load script from JSON file
     * @param filePath Path to JSON file
     * @return Parsed script or nullopt on error
     */
    std::optional<Script> LoadFromJsonFile(const std::string& filePath);
    
    /**
     * Load script from JSON string
     * @param jsonStr JSON content
     * @return Parsed script or nullopt on error
     */
    std::optional<Script> LoadFromJsonString(const std::string& jsonStr);
    
    /**
     * Load script from DSL file
     * DSL format: *CommandName(param1, param2, ...)
     * @param filePath Path to DSL file
     * @return Parsed script or nullopt on error
     */
    std::optional<Script> LoadFromDslFile(const std::string& filePath);
    
    /**
     * Load script from DSL string
     * @param dslStr DSL content
     * @return Parsed script or nullopt on error
     */
    std::optional<Script> LoadFromDslString(const std::string& dslStr);
    
    /**
     * Auto-detect format and load script
     * @param filePath Path to script file
     * @return Parsed script or nullopt on error
     */
    std::optional<Script> LoadFromFile(const std::string& filePath);
    
    /**
     * Save script to JSON file
     * @param script Script to save
     * @param filePath Output file path
     * @return true if successful
     */
    bool SaveToJsonFile(const Script& script, const std::string& filePath);
    
    /**
     * Convert script to JSON string
     * @param script Script to convert
     * @return JSON string
     */
    std::string ToJsonString(const Script& script);
    
    /**
     * Get last error message
     * @return Error message or empty string
     */
    const std::string& GetLastError() const { return m_lastError; }
    
private:
    std::string m_lastError;
    
    // JSON parsing helpers
    std::optional<ChoreographyEvent> ParseJsonEvent(const void* jsonEvent);
    
    // DSL parsing helpers
    std::optional<ChoreographyEvent> ParseDslLine(const std::string& line);
    std::vector<std::string> SplitParams(const std::string& paramsStr);
    std::string Trim(const std::string& str);
    
    void SetError(const std::string& error);
};

} // namespace Choreography
