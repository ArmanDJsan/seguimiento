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
    std::ifstream file(filePath);
    if (!file.is_open()) {
        SetError("Failed to open file: " + filePath);
        return std::nullopt;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return LoadFromJsonString(buffer.str());
}

std::optional<Script> ChoreographyScript::LoadFromJsonString(const std::string& jsonStr) {
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
        
        // Parse events
        if (!j.contains("events") || !j["events"].is_array()) {
            SetError("Script must contain 'events' array");
            return std::nullopt;
        }
        
        for (const auto& eventJson : j["events"]) {
            auto event = ParseJsonEvent(&eventJson);
            if (event) {
                script.events.push_back(*event);
            } else {
                // Log warning but continue parsing
                Logger::Warning("ChoreographyScript: Skipping invalid event in JSON");
            }
        }
        
        if (script.events.empty()) {
            SetError("No valid events found in script");
            return std::nullopt;
        }
        
        Logger::Info("ChoreographyScript: Loaded '" + script.metadata.name + "' with " + 
                    std::to_string(script.events.size()) + " events");
        
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
    
    // Parse based on type
    if (typeLower == "cutdirect") {
        std::string guid = j.value("guid", "");
        int layer = j.value("layer", -1);
        return ChoreographyEvent::CutDirect(guid, layer);
    }
    else if (typeLower == "quickplay") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::QuickPlay(guid);
    }
    else if (typeLower == "play") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::Play(guid);
    }
    else if (typeLower == "pause") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::Pause(guid);
    }
    else if (typeLower == "restart") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::Restart(guid);
    }
    else if (typeLower == "audioon") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::AudioOn(guid);
    }
    else if (typeLower == "audiooff") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::AudioOff(guid);
    }
    else if (typeLower == "startrecording") {
        return ChoreographyEvent::StartRecording();
    }
    else if (typeLower == "stoprecording") {
        return ChoreographyEvent::StopRecording();
    }
    else if (typeLower == "browserreload") {
        std::string guid = j.value("guid", "");
        return ChoreographyEvent::BrowserReload(guid);
    }
    else if (typeLower == "overlayinputin" || typeLower == "overlayinputxin") {
        std::string guid = j.value("guid", "");
        int layer = j.value("layer", 1);
        return ChoreographyEvent::OverlayInputIn(guid, layer);
    }
    else if (typeLower == "overlayinputout" || typeLower == "overlayinputxout") {
        int layer = j.value("layer", 1);
        return ChoreographyEvent::OverlayInputOut(layer);
    }
    else if (typeLower == "replaystartrecording") {
        return ChoreographyEvent::ReplayStartRecording();
    }
    else if (typeLower == "replaystoprecording") {
        return ChoreographyEvent::ReplayStopRecording();
    }
    else if (typeLower == "replaymarkin") {
        return ChoreographyEvent::ReplayMarkIn();
    }
    else if (typeLower == "replaymarkout") {
        return ChoreographyEvent::ReplayMarkOut();
    }
    else if (typeLower == "replaylive") {
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
                speed = std::stoi(speedStr);
            }
        }
        return ChoreographyEvent::ReplaySetSpeed(speed);
    }
    else if (typeLower == "replayselectlastevent") {
        return ChoreographyEvent::ReplaySelectLastEvent();
    }
    else if (typeLower == "timer") {
        int ms = j.value("ms", j.value("milliseconds", 1000));
        return ChoreographyEvent::Timer(ms);
    }
    else if (typeLower == "ndislotchange") {
        int slot = j.value("slot", 0);
        int camera = j.value("camera", 1);
        return ChoreographyEvent::NDISlotChange(slot, camera);
    }
    else if (typeLower == "sceneswitch") {
        int config = j.value("config", j.value("configIndex", 0));
        return ChoreographyEvent::SceneSwitch(config);
    }
    else if (typeLower == "comment") {
        std::string text = j.value("text", "");
        return ChoreographyEvent::Comment(text);
    }
    else if (typeLower == "label") {
        std::string name = j.value("name", "");
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
        
        return std::nullopt;
    }
    
    // Extract command name and parameters
    std::string cmdName = Trim(cmd.substr(0, parenStart));
    std::transform(cmdName.begin(), cmdName.end(), cmdName.begin(), ::tolower);
    
    size_t parenEnd = cmd.rfind(')');
    if (parenEnd == std::string::npos || parenEnd <= parenStart) {
        return std::nullopt;
    }
    
    std::string paramsStr = cmd.substr(parenStart + 1, parenEnd - parenStart - 1);
    auto params = SplitParams(paramsStr);
    
    // Parse based on command name
    if (cmdName == "cutdirect") {
        if (params.empty()) return std::nullopt;
        std::string guid = Trim(params[0]);
        int layer = params.size() > 1 ? std::stoi(Trim(params[1])) : -1;
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
        int layer = std::stoi(Trim(params[1]));
        return ChoreographyEvent::OverlayInputIn(guid, layer);
    }
    else if (cmdName == "overlayinputxout" || cmdName == "overlayinputout") {
        if (params.empty()) return std::nullopt;
        int layer = std::stoi(Trim(params[0]));
        return ChoreographyEvent::OverlayInputOut(layer);
    }
    else if (cmdName == "replaysetspeed") {
        if (params.empty()) return std::nullopt;
        std::string speedStr = Trim(params[0]);
        speedStr.erase(std::remove(speedStr.begin(), speedStr.end(), '%'), speedStr.end());
        int speed = std::stoi(speedStr);
        return ChoreographyEvent::ReplaySetSpeed(speed);
    }
    else if (cmdName == "timer") {
        if (params.empty()) return std::nullopt;
        int ms = std::stoi(Trim(params[0]));
        return ChoreographyEvent::Timer(ms);
    }
    else if (cmdName == "ndislotchange") {
        if (params.size() < 2) return std::nullopt;
        int slot = std::stoi(Trim(params[0]));
        int camera = std::stoi(Trim(params[1]));
        return ChoreographyEvent::NDISlotChange(slot, camera);
    }
    else if (cmdName == "sceneswitch") {
        if (params.empty()) return std::nullopt;
        int config = std::stoi(Trim(params[0]));
        return ChoreographyEvent::SceneSwitch(config);
    }
    
    return std::nullopt;
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
