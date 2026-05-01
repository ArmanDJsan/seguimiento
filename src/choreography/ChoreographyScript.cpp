/**
 * ChoreographyScript.cpp
 * 
 * Implementation of choreography script parser.
 * Supports JSON and DSL formats.
 */

#include "ChoreographyScript.h"
#include "../json.hpp"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>

using json = nlohmann::json;

namespace Choreography {

std::optional<Script> ChoreographyScript::LoadFromJsonFile(const std::string& filePath) {
    Logger::Debug("ChoreographyScript: Loading script from file: " + filePath);
    
    std::ifstream file(filePath);
    if (!file.is_open()) {
        SetError("Failed to open file: " + filePath);
        return std::nullopt;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    Logger::Debug("ChoreographyScript: Read " + std::to_string(content.size()) + " bytes from file");
    return LoadFromJsonString(content);
}

std::optional<Script> ChoreographyScript::LoadFromJsonString(const std::string& jsonStr) {
    Logger::Debug("ChoreographyScript: Parsing JSON string (" + std::to_string(jsonStr.size()) + " bytes)");
    
    try {
        json j = json::parse(jsonStr);
        
        Script script;
        
        // Parse metadata
        if (j.contains("name")) {
            script.metadata.name = j["name"].get<std::string>();
        }
        if (j.contains("description")) {
            script.metadata.description = j["description"].get<std::string>();
        }
        if (j.contains("author")) {
            script.metadata.author = j["author"].get<std::string>();
        }
        if (j.contains("version")) {
            script.metadata.version = j["version"].get<std::string>();
        }
        
        Logger::Debug("ChoreographyScript: Metadata - name='" + script.metadata.name + 
                     "', version='" + script.metadata.version + "'");
        
        // Parse events
        if (!j.contains("events") || !j["events"].is_array()) {
            SetError("Script must contain 'events' array");
            return std::nullopt;
        }
        
        size_t eventCount = j["events"].size();
        Logger::Debug("ChoreographyScript: Found " + std::to_string(eventCount) + " events in JSON");
        
        int validEvents = 0;
        int skippedEvents = 0;
        for (size_t i = 0; i < eventCount; ++i) {
            auto event = ParseJsonEvent(&j["events"][i]);
            if (event) {
                script.events.push_back(*event);
                validEvents++;
            } else {
                Logger::Warning("ChoreographyScript: Skipping invalid event at index " + std::to_string(i));
                skippedEvents++;
            }
        }
        
        if (script.events.empty()) {
            SetError("No valid events found in script");
            return std::nullopt;
        }
        
        Logger::Info("ChoreographyScript: Loaded '" + script.metadata.name + "' with " + 
                    std::to_string(script.events.size()) + " events" +
                    (skippedEvents > 0 ? " (" + std::to_string(skippedEvents) + " skipped)" : "") +
                    ", total duration: " + std::to_string(script.GetTotalDurationMs()) + "ms");
        
        return script;
        
    } catch (const json::exception& e) {
        SetError("JSON parsing error: " + std::string(e.what()));
        return std::nullopt;
    }
}

std::optional<ChoreographyEvent> ChoreographyScript::ParseJsonEvent(const void* jsonEvent) {
    const json& j = *static_cast<const json*>(jsonEvent);
    
    if (!j.contains("type")) {
        Logger::Warning("ChoreographyScript: Event missing 'type' field");
        return std::nullopt;
    }
    
    std::string type = j["type"].get<std::string>();
    
    // Convert type string to lowercase for comparison
    std::string typeLower = type;
    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);
    
    // Helper lambda to validate GUID for commands that require it
    auto validateGuid = [&](const std::string& guid, const std::string& cmdName) -> bool {
        if (guid.empty()) {
            Logger::Warning("ChoreographyScript: " + cmdName + " event has empty GUID - vMix command may fail");
            return false;  // Return false but still create event (warning only)
        }
        return true;
    };
    
    // Parse based on type
    if (typeLower == "cutdirect") {
        std::string guid = j.value("guid", "");
        int layer = j.value("layer", -1);
        validateGuid(guid, "CutDirect");
        Logger::Debug("ChoreographyScript: Parsed CutDirect(guid=" + guid.substr(0, 8) + "..., layer=" + std::to_string(layer) + ")");
        return ChoreographyEvent::CutDirect(guid, layer);
    }
    else if (typeLower == "quickplay") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "QuickPlay");
        return ChoreographyEvent::QuickPlay(guid);
    }
    else if (typeLower == "play") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "Play");
        return ChoreographyEvent::Play(guid);
    }
    else if (typeLower == "pause") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "Pause");
        return ChoreographyEvent::Pause(guid);
    }
    else if (typeLower == "restart") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "Restart");
        return ChoreographyEvent::Restart(guid);
    }
    else if (typeLower == "audioon") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "AudioOn");
        return ChoreographyEvent::AudioOn(guid);
    }
    else if (typeLower == "audiooff") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "AudioOff");
        return ChoreographyEvent::AudioOff(guid);
    }
    else if (typeLower == "startrecording") {
        Logger::Debug("ChoreographyScript: Parsed StartRecording");
        return ChoreographyEvent::StartRecording();
    }
    else if (typeLower == "stoprecording") {
        Logger::Debug("ChoreographyScript: Parsed StopRecording");
        return ChoreographyEvent::StopRecording();
    }
    else if (typeLower == "startmulticorder") {
        Logger::Debug("ChoreographyScript: Parsed StartMultiCorder");
        return ChoreographyEvent::StartMultiCorder();
    }
    else if (typeLower == "stopmulticorder") {
        Logger::Debug("ChoreographyScript: Parsed StopMultiCorder");
        return ChoreographyEvent::StopMultiCorder();
    }
    else if (typeLower == "browserreload") {
        std::string guid = j.value("guid", "");
        validateGuid(guid, "BrowserReload");
        return ChoreographyEvent::BrowserReload(guid);
    }
    else if (typeLower == "overlayinputin" || typeLower == "overlayinputxin") {
        std::string guid = j.value("guid", "");
        int layer = j.value("layer", 1);
        validateGuid(guid, "OverlayInputIn");
        Logger::Debug("ChoreographyScript: Parsed OverlayInputIn(layer=" + std::to_string(layer) + ")");
        return ChoreographyEvent::OverlayInputIn(guid, layer);
    }
    else if (typeLower == "overlayinputout" || typeLower == "overlayinputxout") {
        int layer = j.value("layer", 1);
        Logger::Debug("ChoreographyScript: Parsed OverlayInputOut(layer=" + std::to_string(layer) + ")");
        return ChoreographyEvent::OverlayInputOut(layer);
    }
    else if (typeLower == "replaystartrecording") {
        Logger::Debug("ChoreographyScript: Parsed ReplayStartRecording");
        return ChoreographyEvent::ReplayStartRecording();
    }
    else if (typeLower == "replaystoprecording") {
        Logger::Debug("ChoreographyScript: Parsed ReplayStopRecording");
        return ChoreographyEvent::ReplayStopRecording();
    }
    else if (typeLower == "replaymarkin") {
        Logger::Debug("ChoreographyScript: Parsed ReplayMarkIn");
        return ChoreographyEvent::ReplayMarkIn();
    }
    else if (typeLower == "replaymarkout") {
        Logger::Debug("ChoreographyScript: Parsed ReplayMarkOut");
        return ChoreographyEvent::ReplayMarkOut();
    }
    else if (typeLower == "replaylive") {
        Logger::Debug("ChoreographyScript: Parsed ReplayLive");
        return ChoreographyEvent::ReplayLive();
    }
    else if (typeLower == "replaysetspeed") {
        // Handle both "speed": 50 and "speed": "50%"
        int speed = 100;
        if (j.contains("speed")) {
            if (j["speed"].is_number()) {
                speed = j["speed"].get<int>();
            } else if (j["speed"].is_string()) {
                std::string speedStr = j["speed"].get<std::string>();
                // Remove % if present
                speedStr.erase(std::remove(speedStr.begin(), speedStr.end(), '%'), speedStr.end());
                try {
                    speed = std::stoi(speedStr);
                } catch (const std::exception& /*e*/) {
                    Logger::Warning("ChoreographyScript: Invalid speed value '" + speedStr + "', using default 100%");
                    speed = 100;
                }
            }
        }
        Logger::Debug("ChoreographyScript: Parsed ReplaySetSpeed(speed=" + std::to_string(speed) + "%)");
        return ChoreographyEvent::ReplaySetSpeed(speed);
    }
    else if (typeLower == "replayselectlastevent") {
        Logger::Debug("ChoreographyScript: Parsed ReplaySelectLastEvent");
        return ChoreographyEvent::ReplaySelectLastEvent();
    }
    else if (typeLower == "timer") {
        int ms = j.value("ms", j.value("milliseconds", 1000));
        Logger::Debug("ChoreographyScript: Parsed Timer(ms=" + std::to_string(ms) + ")");
        return ChoreographyEvent::Timer(ms);
    }
    else if (typeLower == "ndislotchange") {
        int slot = j.value("slot", 0);
        int camera = j.value("camera", 1);
        Logger::Debug("ChoreographyScript: Parsed NDISlotChange(slot=" + std::to_string(slot) + 
                     ", camera=" + std::to_string(camera) + ")");
        return ChoreographyEvent::NDISlotChange(slot, camera);
    }
    else if (typeLower == "sceneswitch") {
        int config = j.value("config", j.value("configIndex", 0));
        Logger::Debug("ChoreographyScript: Parsed SceneSwitch(config=" + std::to_string(config) + ")");
        return ChoreographyEvent::SceneSwitch(config);
    }
    else if (typeLower == "spherepresencecheck") {
        int cameraID = j.value("cameraID", j.value("camera", 1));
        int expectedSpheres = j.value("expectedSpheres", j.value("expected", 10));
        int timeoutMs = j.value("timeoutMs", j.value("timeout", 5000));
        Logger::Debug("ChoreographyScript: Parsed SpherePresenceCheck(camera=" + std::to_string(cameraID) + 
                     ", expected=" + std::to_string(expectedSpheres) + ", timeout=" + std::to_string(timeoutMs) + ")");
        return ChoreographyEvent::SpherePresenceCheck(cameraID, expectedSpheres, timeoutMs);
    }
    else if (typeLower == "spherepositioncapture") {
        int cameraID = j.value("cameraID", j.value("camera", 1));
        int timeoutMs = j.value("timeoutMs", j.value("timeout", 5000));
        Logger::Debug("ChoreographyScript: Parsed SpherePositionCapture(camera=" + std::to_string(cameraID) + 
                     ", timeout=" + std::to_string(timeoutMs) + ")");
        return ChoreographyEvent::SpherePositionCapture(cameraID, timeoutMs);
    }
    else if (typeLower == "spherearrivalwait") {
        int cameraID = j.value("cameraID", j.value("camera", 1));
        int expectedSpheres = j.value("expectedSpheres", j.value("expected", 10));
        int timeoutMs = j.value("timeoutMs", j.value("timeout", 60000));
        Logger::Debug("ChoreographyScript: Parsed SphereArrivalWait(camera=" + std::to_string(cameraID) + 
                     ", expected=" + std::to_string(expectedSpheres) + ", timeout=" + std::to_string(timeoutMs) + ")");
        return ChoreographyEvent::SphereArrivalWait(cameraID, expectedSpheres, timeoutMs);
    }
    else if (typeLower == "ptzpreset") {
        int camera = j.value("camera", 1);
        int preset = j.value("preset", 1);
        Logger::Debug("ChoreographyScript: Parsed PTZPreset(camera=" + std::to_string(camera) +
                     ", preset=" + std::to_string(preset) + ")");
        return ChoreographyEvent::PTZPreset(camera, preset);
    }
    else if (typeLower == "esp32command") {
        std::string command = j.value("command", "");
        Logger::Debug("ChoreographyScript: Parsed ESP32Command(command=" + command + ")");
        return ChoreographyEvent::ESP32Command(command);
    }
    else if (typeLower == "scriptstart") {
        std::string scriptName = j.value("script", j.value("name", ""));
        Logger::Debug("ChoreographyScript: Parsed ScriptStart(script=" + scriptName + ")");
        return ChoreographyEvent::ScriptStart(scriptName);
    }
    else if (typeLower == "scriptstop") {
        std::string scriptName = j.value("script", j.value("name", ""));
        Logger::Debug("ChoreographyScript: Parsed ScriptStop(script=" + scriptName + ")");
        return ChoreographyEvent::ScriptStop(scriptName);
    }
    else if (typeLower == "comment") {
        std::string text = j.value("text", "");
        // Don't log debug for comments - they're meta-events
        return ChoreographyEvent::Comment(text);
    }
    else if (typeLower == "label") {
        std::string name = j.value("name", "");
        Logger::Debug("ChoreographyScript: Parsed Label(name=" + name + ")");
        return ChoreographyEvent::Label(name);
    }
    
    Logger::Warning("ChoreographyScript: Unknown event type: " + type);
    return std::nullopt;
}

std::optional<Script> ChoreographyScript::LoadFromDslFile(const std::string& filePath) {
    std::ifstream file(filePath);
    if (!file.is_open()) {
        SetError("Failed to open file: " + filePath);
        return std::nullopt;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return LoadFromDslString(buffer.str());
}

std::optional<Script> ChoreographyScript::LoadFromDslString(const std::string& dslStr) {
    Script script;
    
    std::istringstream stream(dslStr);
    std::string line;
    int lineNum = 0;
    
    while (std::getline(stream, line)) {
        lineNum++;
        line = Trim(line);
        
        // Skip empty lines and pure comments
        if (line.empty()) continue;
        if (line[0] == '#') {
            // Hash comment - add as comment event
            script.events.push_back(ChoreographyEvent::Comment(line.size() > 1 ? line.substr(1) : ""));
            continue;
        }
        if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
            // Double-slash comment - add as comment event
            script.events.push_back(ChoreographyEvent::Comment(line.size() > 2 ? line.substr(2) : ""));
            continue;
        }
        
        // DSL lines start with *
        if (line[0] != '*') {
            Logger::Debug("ChoreographyScript: Skipping non-DSL line " + std::to_string(lineNum));
            continue;
        }
        
        auto event = ParseDslLine(line);
        if (event) {
            script.events.push_back(*event);
        } else {
            Logger::Warning("ChoreographyScript: Failed to parse line " + 
                          std::to_string(lineNum) + ": " + line);
        }
    }
    
    if (script.events.empty()) {
        SetError("No valid events found in DSL script");
        return std::nullopt;
    }
    
    script.metadata.name = "DSL Script";
    Logger::Info("ChoreographyScript: Loaded DSL script with " + 
                std::to_string(script.events.size()) + " events");
    
    return script;
}

std::optional<ChoreographyEvent> ChoreographyScript::ParseDslLine(const std::string& line) {
    // Format: *CommandName(param1, param2, ...)
    // Remove leading *
    std::string cmd = line.substr(1);
    
    // Find opening parenthesis
    size_t parenStart = cmd.find('(');
    if (parenStart == std::string::npos) {
        // Command with no parameters
        std::string cmdName = Trim(cmd);
        std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::tolower);
        
        if (cmdName == "startrecording") return ChoreographyEvent::StartRecording();
        if (cmdName == "stoprecording") return ChoreographyEvent::StopRecording();
        if (cmdName == "replaystartrecording") return ChoreographyEvent::ReplayStartRecording();
        if (cmdName == "replaystoprecording") return ChoreographyEvent::ReplayStopRecording();
        if (cmdName == "replaymarkin") return ChoreographyEvent::ReplayMarkIn();
        if (cmdName == "replaymarkout") return ChoreographyEvent::ReplayMarkOut();
        if (cmdName == "replaylive") return ChoreographyEvent::ReplayLive();
        if (cmdName == "replayselectlastevent") return ChoreographyEvent::ReplaySelectLastEvent();
        
        Logger::Debug("ChoreographyScript: Unknown command without params: " + cmdName);
        return std::nullopt;
    }
    
    // Extract command name and parameters
    std::string cmdName = Trim(cmd.substr(0, parenStart));
    std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::tolower);
    
    size_t parenEnd = cmd.rfind(')');
    if (parenEnd == std::string::npos || parenEnd <= parenStart) {
        Logger::Warning("ChoreographyScript: Malformed DSL line (missing closing parenthesis): " + line);
        return std::nullopt;
    }
    
    std::string paramsStr = cmd.substr(parenStart + 1, parenEnd - parenStart - 1);
    auto params = SplitParams(paramsStr);
    
    // Helper lambda for safe integer conversion
    auto safeStoi = [](const std::string& str, int defaultValue = 0) -> std::optional<int> {
        try {
            return std::stoi(str);
        } catch (const std::invalid_argument& e) {
            Logger::Warning("ChoreographyScript: Invalid integer value: '" + str + "' - " + e.what());
            return std::nullopt;
        } catch (const std::out_of_range& e) {
            Logger::Warning("ChoreographyScript: Integer out of range: '" + str + "' - " + e.what());
            return std::nullopt;
        }
    };
    
    // Parse based on command name (with try-catch for numeric conversions)
    try {
        if (cmdName == "cutdirect") {
            if (params.empty()) return std::nullopt;
            std::string guid = Trim(params[0]);
            int layer = -1;
            if (params.size() > 1) {
                auto layerOpt = safeStoi(Trim(params[1]));
                if (!layerOpt) return std::nullopt;
                layer = *layerOpt;
            }
            Logger::Debug("ChoreographyScript: Parsed CutDirect(guid=" + guid + ", layer=" + std::to_string(layer) + ")");
            return ChoreographyEvent::CutDirect(guid, layer);
        }
        else if (cmdName == "quickplay") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::QuickPlay(Trim(params[0]));
        }
        else if (cmdName == "play") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::Play(Trim(params[0]));
        }
        else if (cmdName == "pause") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::Pause(Trim(params[0]));
        }
        else if (cmdName == "restart") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::Restart(Trim(params[0]));
        }
        else if (cmdName == "audioon") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::AudioOn(Trim(params[0]));
        }
        else if (cmdName == "audiooff") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::AudioOff(Trim(params[0]));
        }
        else if (cmdName == "browserreload") {
            if (params.empty()) return std::nullopt;
            return ChoreographyEvent::BrowserReload(Trim(params[0]));
        }
        else if (cmdName == "overlayinputxin" || cmdName == "overlayinputin") {
            if (params.size() < 2) return std::nullopt;
            std::string guid = Trim(params[0]);
            auto layerOpt = safeStoi(Trim(params[1]));
            if (!layerOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed OverlayInputIn(guid=" + guid + ", layer=" + std::to_string(*layerOpt) + ")");
            return ChoreographyEvent::OverlayInputIn(guid, *layerOpt);
        }
        else if (cmdName == "overlayinputxout" || cmdName == "overlayinputout") {
            if (params.empty()) return std::nullopt;
            auto layerOpt = safeStoi(Trim(params[0]));
            if (!layerOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed OverlayInputOut(layer=" + std::to_string(*layerOpt) + ")");
            return ChoreographyEvent::OverlayInputOut(*layerOpt);
        }
        else if (cmdName == "replaysetspeed") {
            if (params.empty()) return std::nullopt;
            std::string speedStr = Trim(params[0]);
            speedStr.erase(std::remove(speedStr.begin(), speedStr.end(), '%'), speedStr.end());
            auto speedOpt = safeStoi(speedStr);
            if (!speedOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed ReplaySetSpeed(speed=" + std::to_string(*speedOpt) + "%)");
            return ChoreographyEvent::ReplaySetSpeed(*speedOpt);
        }
        else if (cmdName == "timer") {
            if (params.empty()) return std::nullopt;
            auto msOpt = safeStoi(Trim(params[0]));
            if (!msOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed Timer(ms=" + std::to_string(*msOpt) + ")");
            return ChoreographyEvent::Timer(*msOpt);
        }
        else if (cmdName == "ndislotchange") {
            if (params.size() < 2) return std::nullopt;
            auto slotOpt = safeStoi(Trim(params[0]));
            auto cameraOpt = safeStoi(Trim(params[1]));
            if (!slotOpt || !cameraOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed NDISlotChange(slot=" + std::to_string(*slotOpt) + 
                         ", camera=" + std::to_string(*cameraOpt) + ")");
            return ChoreographyEvent::NDISlotChange(*slotOpt, *cameraOpt);
        }
        else if (cmdName == "sceneswitch") {
            if (params.empty()) return std::nullopt;
            auto configOpt = safeStoi(Trim(params[0]));
            if (!configOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed SceneSwitch(config=" + std::to_string(*configOpt) + ")");
            return ChoreographyEvent::SceneSwitch(*configOpt);
        }
        else if (cmdName == "ptzpreset") {
            if (params.size() < 2) return std::nullopt;
            auto cameraOpt = safeStoi(Trim(params[0]));
            auto presetOpt = safeStoi(Trim(params[1]));
            if (!cameraOpt || !presetOpt) return std::nullopt;
            Logger::Debug("ChoreographyScript: Parsed PTZPreset(camera=" + std::to_string(*cameraOpt) +
                         ", preset=" + std::to_string(*presetOpt) + ")");
            return ChoreographyEvent::PTZPreset(*cameraOpt, *presetOpt);
        }
        else if (cmdName == "esp32command") {
            if (params.empty()) return std::nullopt;
            std::string command = Trim(params[0]);
            // Strip surrounding quotes if present
            if (command.size() >= 2 && command.front() == '"' && command.back() == '"') {
                command = command.substr(1, command.size() - 2);
            }
            Logger::Debug("ChoreographyScript: Parsed ESP32Command(command=" + command + ")");
            return ChoreographyEvent::ESP32Command(command);
        }
        else if (cmdName == "scriptstart") {
            if (params.empty()) return std::nullopt;
            std::string scriptName = Trim(params[0]);
            if (scriptName.size() >= 2 && scriptName.front() == '"' && scriptName.back() == '"') {
                scriptName = scriptName.substr(1, scriptName.size() - 2);
            }
            Logger::Debug("ChoreographyScript: Parsed ScriptStart(script=" + scriptName + ")");
            return ChoreographyEvent::ScriptStart(scriptName);
        }
        else if (cmdName == "scriptstop") {
            if (params.empty()) return std::nullopt;
            std::string scriptName = Trim(params[0]);
            if (scriptName.size() >= 2 && scriptName.front() == '"' && scriptName.back() == '"') {
                scriptName = scriptName.substr(1, scriptName.size() - 2);
            }
            Logger::Debug("ChoreographyScript: Parsed ScriptStop(script=" + scriptName + ")");
            return ChoreographyEvent::ScriptStop(scriptName);
        }
        
        Logger::Debug("ChoreographyScript: Unknown DSL command: " + cmdName);
        return std::nullopt;
        
    } catch (const std::exception& e) {
        Logger::Warning("ChoreographyScript: Exception parsing DSL command '" + cmdName + "': " + e.what());
        return std::nullopt;
    }
}

std::vector<std::string> ChoreographyScript::SplitParams(const std::string& paramsStr) {
    std::vector<std::string> params;
    std::string current;
    int depth = 0;
    
    for (char c : paramsStr) {
        if (c == '(' || c == '[' || c == '{') {
            depth++;
            current += c;
        }
        else if (c == ')' || c == ']' || c == '}') {
            depth--;
            current += c;
        }
        else if (c == ',' && depth == 0) {
            params.push_back(current);
            current.clear();
        }
        else {
            current += c;
        }
    }
    
    if (!current.empty()) {
        params.push_back(current);
    }
    
    return params;
}

std::string ChoreographyScript::Trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::optional<Script> ChoreographyScript::LoadFromFile(const std::string& filePath) {
    // Auto-detect format based on extension
    size_t dotPos = filePath.rfind('.');
    if (dotPos != std::string::npos) {
        std::string ext = filePath.substr(dotPos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == "json") {
            return LoadFromJsonFile(filePath);
        }
        else if (ext == "dsl" || ext == "txt" || ext == "vmix") {
            return LoadFromDslFile(filePath);
        }
    }
    
    // Try JSON first, then DSL
    auto result = LoadFromJsonFile(filePath);
    if (!result) {
        result = LoadFromDslFile(filePath);
    }
    return result;
}

bool ChoreographyScript::SaveToJsonFile(const Script& script, const std::string& filePath) {
    std::ofstream file(filePath);
    if (!file.is_open()) {
        SetError("Failed to create file: " + filePath);
        return false;
    }
    
    file << ToJsonString(script);
    return true;
}

std::string ChoreographyScript::ToJsonString(const Script& script) {
    json j;
    
    // Metadata
    j["name"] = script.metadata.name;
    if (!script.metadata.description.empty()) {
        j["description"] = script.metadata.description;
    }
    if (!script.metadata.author.empty()) {
        j["author"] = script.metadata.author;
    }
    j["version"] = script.metadata.version;
    
    // Events
    j["events"] = json::array();
    for (const auto& event : script.events) {
        json eventJson;
        eventJson["type"] = event.GetTypeName();
        
        // Add parameters based on type
        if (auto* p = std::get_if<CutDirectParams>(&event.params)) {
            eventJson["guid"] = p->guid;
            eventJson["layer"] = p->layer;
        }
        else if (auto* p = std::get_if<GuidParam>(&event.params)) {
            eventJson["guid"] = p->guid;
        }
        else if (auto* p = std::get_if<OverlayParams>(&event.params)) {
            if (!p->guid.empty()) {
                eventJson["guid"] = p->guid;
            }
            eventJson["layer"] = p->layer;
        }
        else if (auto* p = std::get_if<TimerParams>(&event.params)) {
            eventJson["ms"] = p->milliseconds;
        }
        else if (auto* p = std::get_if<ReplaySpeedParams>(&event.params)) {
            eventJson["speed"] = p->percentage;
        }
        else if (auto* p = std::get_if<NDISlotParams>(&event.params)) {
            eventJson["slot"] = p->slot;
            eventJson["camera"] = p->cameraID;
        }
        else if (auto* p = std::get_if<SceneSwitchParams>(&event.params)) {
            eventJson["config"] = p->configIndex;
        }
        else if (auto* p = std::get_if<SphereVerificationParams>(&event.params)) {
            eventJson["cameraID"] = p->cameraID;
            eventJson["expectedSpheres"] = p->expectedSpheres;
            eventJson["timeoutMs"] = p->timeoutMs;
            eventJson["mode"] = p->mode;
        }
        else if (auto* p = std::get_if<TextParam>(&event.params)) {
            eventJson["text"] = p->text;
        }
        
        if (!event.comment.empty()) {
            eventJson["comment"] = event.comment;
        }
        
        j["events"].push_back(eventJson);
    }
    
    return j.dump(2);
}

void ChoreographyScript::SetError(const std::string& error) {
    m_lastError = error;
    Logger::Error("ChoreographyScript: " + error);
}

} // namespace Choreography
