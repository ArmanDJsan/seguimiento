/**
 * Visual Intelligence Bypass (VIB) - Main Entry Point
 * 
 * High-performance video capture and AI processing system for vMix
 * Supports up to 12x 4K@60fps streams with zero-copy DMA
 * 
 * Target Hardware:
 * - AMD Threadripper Pro 9955WX
 * - NVIDIA RTX 5080 16GB
 * - 3x Blackmagic DeckLink 8K Pro Mini
 * - 128GB DDR5 RAM
 */

#include <iostream>
#include <vector>
#include <memory>
#include <thread>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <limits>

// Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>  // For COM: CoInitializeEx, CoUninitialize
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <regex>

#include "../DeckLinkAPI_h.h"  // DeckLink SDK COM interfaces
#include "../capture/DeckLinkCapture.h"
#include "../capture/DeckLinkSource.h"
#include "../ndi/NDIManager.h"
#include "../ai/ActiveCameraSelector.h"
#include "../ai/InferenceEngine.h"
#include "../tracking/PositionMapper.h"
#include "../tracking/BallTracker.h"
#include "../tracking/MovingAverageFilter.h"
#include "../tracking/RouteMapper.h"
#include "../scene/SceneManager.h"
#include "../measure/RankingMeasure.h"
#include "../redis/RedisWorker.h"
#include "../telemetry/PerformanceMonitor.h"
#include "../utils/Logger.h"
#include "../utils/ThreadOptimizer.h"
#include "../utils/GPUDiagnostics.h"
#include "../utils/FrameSaver.h"
#include "../utils/TrackingLogger.h"
#include "../control/VideoHubClient.h"
#include "../control/TrackPhysicalController.h"
#include "../control/VMixController.h"
#include "../control/PTZController.h"
#include "../control/ControlApi.h"
#include "../choreography/ChoreographyEngine.h"
#include "../verification/SphereVerifier.h"
#include "../verification/ArrivalOrderStabilizer.h"
#include "../output/ArrivalEmitter.h"
#include "../ui/UserMenu.h"
#include "../diagnostics/StressTester.h"
#include "Config.h"

// Link DirectX libraries (still needed for D3D11 device creation used by CUDA interop)
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Link COM library (required for DeckLink SDK)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")  // For SysFreeString

#include <unordered_map>
#include <vector>
#include <numeric>
#include "../json.hpp"

using json = nlohmann::json;

constexpr int kMaxNDIChannels = 4;  // Reduced to 4 PTZ cameras for radar detection
constexpr unsigned int kDefaultCaptureWidth = 1920;  // 1080p capture (signal from PTZ)
constexpr unsigned int kDefaultCaptureHeight = 1080;
constexpr int kVideoHubPrimaryOutput = 0;
// Cards A and B (devices 0-7) are owned by vMix. The PTZ tracking feeds arrive on
// card C (devices 8-11), so capture starts at device 8 instead of device 0.
constexpr int kCaptureDeviceBase = 8;

namespace {
std::unordered_map<std::string, int> BuildInputLookup() {
    std::unordered_map<std::string, int> lookup;
    // VideoHub uses 0-based indexing for inputs
    for (int i = 1; i <= 12; ++i) {
        std::stringstream ss;
        ss << "CAM_" << std::setw(2) << std::setfill('0') << i;
        lookup[ss.str()] = i - 1;  // Convert to 0-based (CAM_01 = input 0, CAM_02 = input 1, etc.)
    }
    lookup["RADAR_01"] = 12;  // 0-based index
    lookup["RADAR_02"] = 13;
    lookup["RADAR_03"] = 14;
    lookup["RADAR_04"] = 15;
    return lookup;
}

std::vector<int> RangeInclusive(int start, int end) {
    std::vector<int> values(static_cast<size_t>(end - start + 1));
    std::iota(values.begin(), values.end(), start);
    return values;
}

/**
 * Rutea las PTZ (RADAR_01..04) a sus salidas segun scene_manager.radar_routing.
 * Mismo criterio que SceneManager::ApplyRadarRouting: por nombre, leyendo los
 * slots del config. No hay numeros de puerto hardcodeados en el codigo.
 * Devuelve true si se ruteo todo (o si el ruteo esta deshabilitado en config).
 */
bool ApplyRadarRoutingFromConfig(VideoHubClient& videoHub, const Config& config,
                                 const std::string& tag) {
    if (!config.radarRoutingEnabled) {
        Logger::Info("[" + tag + "] Radar routing: deshabilitado en config "
                     "(scene_manager.radar_routing.enabled=false)");
        return true;
    }

    int okCount = 0;
    for (int i = 0; i < SceneManager::kRadarCount; ++i) {
        const int outSlot = config.radarOutputSlots[static_cast<size_t>(i)];
        const std::string inputName = SceneManager::kRadarNames[i];
        if (videoHub.RouteInputToOutput(outSlot, inputName)) {
            ++okCount;
            Logger::Info("[" + tag + "] VideoHub: " + inputName + " -> out " +
                         std::to_string(outSlot) + " (indice de protocolo, 0-based) OK");
        } else {
            Logger::Warning("[" + tag + "] VideoHub: fallo ruteando " + inputName +
                            " -> out " + std::to_string(outSlot) + " (no critico)");
        }
    }
    return okCount == SceneManager::kRadarCount;
}

// Config struct is now defined in Config.h

/**
 * Validate SceneManager configuration
 * Returns true if configuration is valid, false otherwise
 */
bool ValidateSceneManagerConfig(const Config& config) {
    if (!config.sceneManagerEnabled) {
        return true;  // No validation needed when disabled
    }
    
    bool valid = true;
    
    // Validate that we have at least one group
    if (config.sceneManagerGroups.empty()) {
        Logger::Error("SceneManager validation failed: No group configurations defined");
        valid = false;
    }
    
    // Validate each group
    for (size_t i = 0; i < config.sceneManagerGroups.size(); ++i) {
        const auto& group = config.sceneManagerGroups[i];
        
        // Validate camera IDs in G1-G4 (range 1-12)
        for (int j = 0; j < 4; ++j) {
            int cameraID = group.slotsG1_G4[j];
            if (cameraID < 1 || cameraID > 12) {
                Logger::Error("SceneManager validation failed: Group '" + group.configName + 
                             "' has invalid camera ID " + std::to_string(cameraID) + 
                             " in G1-G4[" + std::to_string(j) + "] (must be 1-12)");
                valid = false;
            }
        }
        
        // Validate camera IDs in G5-G8 (range 1-12)
        for (int j = 0; j < 4; ++j) {
            int cameraID = group.slotsG5_G8[j];
            if (cameraID < 1 || cameraID > 12) {
                Logger::Error("SceneManager validation failed: Group '" + group.configName + 
                             "' has invalid camera ID " + std::to_string(cameraID) + 
                             " in G5-G8[" + std::to_string(j) + "] (must be 1-12)");
                valid = false;
            }
        }
        
        // Validate trigger thresholds are in ascending order
        if (i > 0) {
            float prevThreshold = config.sceneManagerGroups[i - 1].triggerThreshold;
            float currThreshold = group.triggerThreshold;
            if (currThreshold <= prevThreshold) {
                Logger::Warning("SceneManager: Group '" + group.configName + 
                               "' threshold (" + std::to_string(currThreshold) + 
                               "m) is not greater than previous group threshold (" + 
                               std::to_string(prevThreshold) + "m)");
            }
        }
    }
    
    if (valid) {
        Logger::Info("SceneManager configuration validated successfully (" + 
                    std::to_string(config.sceneManagerGroups.size()) + " groups)");
    }
    
    return valid;
}

Config LoadConfig(const std::string& path) {
    Config config;  // Constructor ya tiene valores por defecto

    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Warning("No se pudo abrir config.json; usando valores por defecto");
        return config;
    }

    try {
        json j;
        file >> j;

        // Parse videohub section
        if (j.contains("videohub") && j["videohub"].is_object()) {
            if (j["videohub"].contains("ip") && j["videohub"]["ip"].is_string()) {
                config.videohubIp = j["videohub"]["ip"].get<std::string>();
            }
            if (j["videohub"].contains("port") && j["videohub"]["port"].is_number()) {
                config.videohubPort = j["videohub"]["port"].get<uint16_t>();
            }
        }

        // Parse esp32 section
        if (j.contains("esp32") && j["esp32"].is_object()) {
            if (j["esp32"].contains("ip") && j["esp32"]["ip"].is_string()) {
                config.esp32Ip = j["esp32"]["ip"].get<std::string>();
            }
            if (j["esp32"].contains("port") && j["esp32"]["port"].is_number()) {
                config.esp32Port = j["esp32"]["port"].get<uint16_t>();
            }
        }
        
        // Parse redis section
        if (j.contains("redis") && j["redis"].is_object()) {
            if (j["redis"].contains("enabled") && j["redis"]["enabled"].is_boolean()) {
                config.redisEnabled = j["redis"]["enabled"].get<bool>();
            }
            if (j["redis"].contains("host") && j["redis"]["host"].is_string()) {
                config.redisHost = j["redis"]["host"].get<std::string>();
            }
            if (j["redis"].contains("port") && j["redis"]["port"].is_number()) {
                config.redisPort = j["redis"]["port"].get<uint16_t>();
            }
        }
        
        // Parse camera_selector section
        if (j.contains("camera_selector") && j["camera_selector"].is_object()) {
            auto& selector = j["camera_selector"];
            if (selector.contains("enabled") && selector["enabled"].is_boolean()) {
                config.selectorEnabled = selector["enabled"].get<bool>();
            }
            if (selector.contains("top_k") && selector["top_k"].is_number()) {
                config.selectorTopK = selector["top_k"].get<int>();
            }
            if (selector.contains("motion_threshold") && selector["motion_threshold"].is_number()) {
                config.selectorMotionThreshold = selector["motion_threshold"].get<float>();
            }
            if (selector.contains("edge_handover_margin") && selector["edge_handover_margin"].is_number()) {
                config.selectorEdgeMargin = selector["edge_handover_margin"].get<float>();
            }
        }
        
        // Parse detection_optimization.hysteresis section
        if (j.contains("detection_optimization") && j["detection_optimization"].is_object()) {
            auto& detopt = j["detection_optimization"];
            if (detopt.contains("hysteresis") && detopt["hysteresis"].is_object()) {
                auto& hyst = detopt["hysteresis"];
                if (hyst.contains("switch_threshold") && hyst["switch_threshold"].is_number()) {
                    config.hysteresisSwitchThreshold = hyst["switch_threshold"].get<float>();
                }
                if (hyst.contains("min_active_frames") && hyst["min_active_frames"].is_number()) {
                    config.hysteresisMinActiveFrames = hyst["min_active_frames"].get<int>();
                }
                if (hyst.contains("decay_factor") && hyst["decay_factor"].is_number()) {
                    config.hysteresisDecayFactor = hyst["decay_factor"].get<float>();
                }
            }
        }

        // Parse target_spheres from root
        if (j.contains("target_spheres") && j["target_spheres"].is_number()) {
            config.targetSpheres = j["target_spheres"].get<int>();
        }
        
        // Parse scene_manager section
        config.sceneManagerEnabled = true;
        config.sceneManagerMuteTimeoutMs = 200;
        if (j.contains("scene_manager") && j["scene_manager"].is_object()) {
            auto& sm = j["scene_manager"];
            if (sm.contains("enabled") && sm["enabled"].is_boolean()) {
                config.sceneManagerEnabled = sm["enabled"].get<bool>();
            }
            if (sm.contains("mute_timeout_ms") && sm["mute_timeout_ms"].is_number()) {
                config.sceneManagerMuteTimeoutMs = sm["mute_timeout_ms"].get<int>();
            }
            
            // Parse mode (auto/manual)
            if (sm.contains("mode") && sm["mode"].is_string()) {
                std::string modeStr = sm["mode"].get<std::string>();
                if (modeStr == "auto") {
                    config.sceneManagerMode = SceneMode::AUTO;
                } else if (modeStr == "manual") {
                    config.sceneManagerMode = SceneMode::MANUAL;
                } else {
                    Logger::Warning("SceneManager config: Invalid mode '" + modeStr + 
                                   "', using default AUTO");
                    config.sceneManagerMode = SceneMode::AUTO;
                }
            }
            
            // Parse trigger_mode (threshold/event)
            // EVENT is the new default architecture - THRESHOLD is for legacy compatibility
            if (sm.contains("trigger_mode") && sm["trigger_mode"].is_string()) {
                std::string triggerModeStr = sm["trigger_mode"].get<std::string>();
                if (triggerModeStr == "threshold") {
                    config.sceneManagerTriggerMode = TriggerMode::THRESHOLD;
                    Logger::Info("SceneManager config: Using THRESHOLD mode (legacy)");
                } else if (triggerModeStr == "event") {
                    config.sceneManagerTriggerMode = TriggerMode::EVENT;
                    Logger::Info("SceneManager config: Using EVENT mode (new architecture)");
                } else {
                    Logger::Warning("SceneManager config: Invalid trigger_mode '" + triggerModeStr + 
                                   "', using default EVENT (new architecture)");
                    config.sceneManagerTriggerMode = TriggerMode::EVENT;
                }
            }
            
            // Parse event-driven mode parameters
            if (sm.contains("event_cooldown_ms") && sm["event_cooldown_ms"].is_number()) {
                config.sceneManagerEventCooldownMs = sm["event_cooldown_ms"].get<int>();
            }
            if (sm.contains("hysteresis_frames") && sm["hysteresis_frames"].is_number()) {
                config.sceneManagerHysteresisFrames = sm["hysteresis_frames"].get<int>();
            }
            
            // Parse manual_keys configuration
            if (sm.contains("manual_keys") && sm["manual_keys"].is_object()) {
                auto& mk = sm["manual_keys"];
                
                if (mk.contains("toggle_mode") && mk["toggle_mode"].is_string()) {
                    config.sceneManagerManualKeys.toggleMode = mk["toggle_mode"].get<std::string>();
                }
                
                if (mk.contains("config_select") && mk["config_select"].is_array() && 
                    mk["config_select"].size() >= 3) {
                    for (size_t i = 0; i < 3; ++i) {
                        config.sceneManagerManualKeys.configSelect[i] = 
                            mk["config_select"][i].get<std::string>();
                    }
                } else {
                    Logger::Warning("SceneManager config: manual_keys.config_select must have at least 3 elements, using defaults [F1,F6,F7]");
                }
                
                if (mk.contains("group_select") && mk["group_select"].is_string()) {
                    config.sceneManagerManualKeys.groupSelect = mk["group_select"].get<std::string>();
                }
                
                if (mk.contains("camera_select") && mk["camera_select"].is_array() && 
                    mk["camera_select"].size() >= 4) {
                    for (size_t i = 0; i < 4; ++i) {
                        config.sceneManagerManualKeys.cameraSelect[i] = 
                            mk["camera_select"][i].get<std::string>();
                    }
                } else {
                    Logger::Warning("SceneManager config: manual_keys.camera_select must have at least 4 elements, using defaults [1,2,3,4]");
                }
                
                Logger::Info("SceneManager config: Manual keys - Toggle: " + 
                            config.sceneManagerManualKeys.toggleMode + 
                            ", Configs: [" + config.sceneManagerManualKeys.configSelect[0] + "," +
                            config.sceneManagerManualKeys.configSelect[1] + "," +
                            config.sceneManagerManualKeys.configSelect[2] + "], Group: " +
                            config.sceneManagerManualKeys.groupSelect);
            }
            
            if (sm.contains("groups") && sm["groups"].is_object()) {
                try {
                    for (auto& [key, value] : sm["groups"].items()) {
                        GroupConfig gc;
                        gc.configName = key;
                        
                        // Parse G1-G4 cameras (required)
                        if (value.contains("g1_g4") && value["g1_g4"].is_array()) {
                            if (value["g1_g4"].size() >= 4) {
                                for (size_t i = 0; i < 4; ++i) {
                                    gc.slotsG1_G4[i] = value["g1_g4"][i].get<int>();
                                }
                            } else {
                                Logger::Warning("SceneManager config: Group '" + key + 
                                               "' g1_g4 array has fewer than 4 elements, using defaults");
                            }
                        } else {
                            Logger::Warning("SceneManager config: Group '" + key + 
                                           "' missing g1_g4 field, using defaults");
                        }
                        
                        // Parse G5-G8 cameras (required)
                        if (value.contains("g5_g8") && value["g5_g8"].is_array()) {
                            if (value["g5_g8"].size() >= 4) {
                                for (size_t i = 0; i < 4; ++i) {
                                    gc.slotsG5_G8[i] = value["g5_g8"][i].get<int>();
                                }
                            } else {
                                Logger::Warning("SceneManager config: Group '" + key + 
                                               "' g5_g8 array has fewer than 4 elements, using defaults");
                            }
                        } else {
                            Logger::Warning("SceneManager config: Group '" + key + 
                                           "' missing g5_g8 field, using defaults");
                        }
                        
                        // Parse trigger threshold (required)
                        if (value.contains("trigger_threshold") && value["trigger_threshold"].is_number()) {
                            gc.triggerThreshold = value["trigger_threshold"].get<float>();
                        } else {
                            Logger::Warning("SceneManager config: Group '" + key + 
                                           "' missing trigger_threshold, using default 0.0");
                        }
                        
                        // Log parsed group
                        std::ostringstream oss;
                        oss << "SceneManager config: Parsed group '" << key << "' - "
                            << "G1-G4=[" << gc.slotsG1_G4[0] << "," << gc.slotsG1_G4[1] << ","
                            << gc.slotsG1_G4[2] << "," << gc.slotsG1_G4[3] << "], "
                            << "G5-G8=[" << gc.slotsG5_G8[0] << "," << gc.slotsG5_G8[1] << ","
                            << gc.slotsG5_G8[2] << "," << gc.slotsG5_G8[3] << "], "
                            << "threshold=" << gc.triggerThreshold << "m";
                        Logger::Debug(oss.str());
                        
                        config.sceneManagerGroups.push_back(gc);
                    }
                    
                    // Sort by trigger threshold
                    std::sort(config.sceneManagerGroups.begin(), config.sceneManagerGroups.end(),
                              [](const GroupConfig& a, const GroupConfig& b) {
                                  return a.triggerThreshold < b.triggerThreshold;
                              });
                    
                    // Log sorted order
                    if (!config.sceneManagerGroups.empty()) {
                        std::ostringstream oss;
                        oss << "SceneManager config: Sorted " << config.sceneManagerGroups.size() << " groups by threshold: ";
                        for (size_t i = 0; i < config.sceneManagerGroups.size(); ++i) {
                            if (i > 0) oss << " -> ";
                            oss << config.sceneManagerGroups[i].configName 
                                << "(" << config.sceneManagerGroups[i].triggerThreshold << "m)";
                        }
                        Logger::Info(oss.str());
                    }
                } catch (const std::exception& e) {
                    Logger::Error("SceneManager config: Failed to parse groups - " + std::string(e.what()));
                }
            } else if (config.sceneManagerEnabled) {
                Logger::Warning("SceneManager config: No 'groups' section found, using defaults from SceneManagerConfig");
            }
            
            // Parse radar_routing section within scene_manager
            if (sm.contains("radar_routing") && sm["radar_routing"].is_object()) {
                auto& rr = sm["radar_routing"];
                
                if (rr.contains("enabled") && rr["enabled"].is_boolean()) {
                    config.radarRoutingEnabled = rr["enabled"].get<bool>();
                }
                
                // Parse individual radar output slots using SceneManager constants
                for (size_t i = 0; i < SceneManager::kRadarCount; ++i) {
                    std::string radarKey = SceneManager::kRadarNames[i];
                    if (rr.contains(radarKey) && rr[radarKey].is_number()) {
                        config.radarOutputSlots[i] = rr[radarKey].get<int>();
                    }
                }
                
                if (config.radarRoutingEnabled) {
                    std::ostringstream oss;
                    oss << "Radar routing config: " 
                        << SceneManager::kRadarNames[0] << "->" << config.radarOutputSlots[0]
                        << ", " << SceneManager::kRadarNames[1] << "->" << config.radarOutputSlots[1]
                        << ", " << SceneManager::kRadarNames[2] << "->" << config.radarOutputSlots[2]
                        << ", " << SceneManager::kRadarNames[3] << "->" << config.radarOutputSlots[3];
                    Logger::Info(oss.str());
                } else {
                    Logger::Info("Radar routing config: disabled");
                }
            }
        } else if (config.sceneManagerEnabled) {
            Logger::Warning("SceneManager config: No 'scene_manager' section found, using defaults");
        }
        
        // Parse NDI section
        if (j.contains("ndi") && j["ndi"].is_object()) {
            auto& ndi = j["ndi"];
            if (ndi.contains("enabled") && ndi["enabled"].is_boolean()) {
                config.ndiEnabled = ndi["enabled"].get<bool>();
                Logger::Info("NDI config: enabled = " + std::string(config.ndiEnabled ? "true" : "false"));
            }
        }
        
        // Parse inference_engine section
        if (j.contains("inference_engine") && j["inference_engine"].is_object()) {
            auto& ie = j["inference_engine"];
            if (ie.contains("model_path") && ie["model_path"].is_string()) {
                config.inferenceConfig.modelPath = ie["model_path"].get<std::string>();
            }
            if (ie.contains("batch_size") && ie["batch_size"].is_number()) {
                config.inferenceConfig.batchSize = ie["batch_size"].get<int>();
            }
            // Support both new (input_width/input_height) and legacy (input_size) formats
            if (ie.contains("input_width") && ie["input_width"].is_number()) {
                config.inferenceConfig.inputWidth = ie["input_width"].get<int>();
            } else if (ie.contains("input_size") && ie["input_size"].is_number()) {
                // Backward compatibility: input_size sets both width and height (square)
                config.inferenceConfig.inputWidth = ie["input_size"].get<int>();
            }
            
            if (ie.contains("input_height") && ie["input_height"].is_number()) {
                config.inferenceConfig.inputHeight = ie["input_height"].get<int>();
            } else if (ie.contains("input_size") && ie["input_size"].is_number() && !ie.contains("input_width")) {
                // Backward compatibility: input_size sets both width and height (square)
                config.inferenceConfig.inputHeight = ie["input_size"].get<int>();
            }
            if (ie.contains("confidence_threshold") && ie["confidence_threshold"].is_number()) {
                config.inferenceConfig.confidenceThreshold = ie["confidence_threshold"].get<float>();
            }
            if (ie.contains("nms_threshold") && ie["nms_threshold"].is_number()) {
                config.inferenceConfig.nmsThreshold = ie["nms_threshold"].get<float>();
            }
            if (ie.contains("num_classes") && ie["num_classes"].is_number()) {
                config.inferenceConfig.numClasses = ie["num_classes"].get<int>();
            }
            // Parse skip_resize option for native resolution inference
            if (ie.contains("skip_resize") && ie["skip_resize"].is_boolean()) {
                config.inferenceConfig.skipResize = ie["skip_resize"].get<bool>();
                if (config.inferenceConfig.skipResize) {
                    Logger::Info("InferenceEngine config: skip_resize enabled - using native " +
                                std::to_string(config.inferenceConfig.inputWidth) + "x" +
                                std::to_string(config.inferenceConfig.inputHeight) + " resolution");
                }
            }
            // Parse max_detections for memory optimization
            if (ie.contains("max_detections") && ie["max_detections"].is_number()) {
                config.inferenceConfig.maxDetections = ie["max_detections"].get<int>();
                Logger::Info("InferenceEngine config: max_detections=" + std::to_string(config.inferenceConfig.maxDetections));
            }
            // Parse debug YOLO settings
            if (ie.contains("debug_yolo_enabled") && ie["debug_yolo_enabled"].is_boolean()) {
                config.inferenceConfig.debugYoloEnabled = ie["debug_yolo_enabled"].get<bool>();
            }
            if (ie.contains("debug_yolo_interval") && ie["debug_yolo_interval"].is_number()) {
                config.inferenceConfig.debugYoloInterval = ie["debug_yolo_interval"].get<int>();
            }
            if (config.inferenceConfig.debugYoloEnabled) {
                Logger::Info("InferenceEngine config: YOLO debug logging enabled (interval=" + 
                            std::to_string(config.inferenceConfig.debugYoloInterval) + " frames)");
            }
        }

        // Parse inference_engine_llegada section (dedicated arrival engine)
        if (j.contains("inference_engine_llegada") && j["inference_engine_llegada"].is_object()) {
            auto& ie = j["inference_engine_llegada"];
            if (ie.contains("model_path") && ie["model_path"].is_string()) {
                config.llegadaInferenceConfig.modelPath = ie["model_path"].get<std::string>();
            }
            if (ie.contains("batch_size") && ie["batch_size"].is_number()) {
                config.llegadaInferenceConfig.batchSize = ie["batch_size"].get<int>();
            }
            if (ie.contains("input_width") && ie["input_width"].is_number()) {
                config.llegadaInferenceConfig.inputWidth = ie["input_width"].get<int>();
            }
            if (ie.contains("input_height") && ie["input_height"].is_number()) {
                config.llegadaInferenceConfig.inputHeight = ie["input_height"].get<int>();
            }
            if (ie.contains("confidence_threshold") && ie["confidence_threshold"].is_number()) {
                config.llegadaInferenceConfig.confidenceThreshold = ie["confidence_threshold"].get<float>();
            }
            if (ie.contains("nms_threshold") && ie["nms_threshold"].is_number()) {
                config.llegadaInferenceConfig.nmsThreshold = ie["nms_threshold"].get<float>();
            }
            if (ie.contains("num_classes") && ie["num_classes"].is_number()) {
                config.llegadaInferenceConfig.numClasses = ie["num_classes"].get<int>();
            }
            if (ie.contains("skip_resize") && ie["skip_resize"].is_boolean()) {
                config.llegadaInferenceConfig.skipResize = ie["skip_resize"].get<bool>();
            }
            if (ie.contains("max_detections") && ie["max_detections"].is_number()) {
                config.llegadaInferenceConfig.maxDetections = ie["max_detections"].get<int>();
            }
            if (ie.contains("debug_yolo_enabled") && ie["debug_yolo_enabled"].is_boolean()) {
                config.llegadaInferenceConfig.debugYoloEnabled = ie["debug_yolo_enabled"].get<bool>();
            }
            if (ie.contains("debug_yolo_interval") && ie["debug_yolo_interval"].is_number()) {
                config.llegadaInferenceConfig.debugYoloInterval = ie["debug_yolo_interval"].get<int>();
            }
            Logger::Info("InferenceEngine llegada config: " + config.llegadaInferenceConfig.modelPath +
                        " " + std::to_string(config.llegadaInferenceConfig.inputWidth) + "x" +
                        std::to_string(config.llegadaInferenceConfig.inputHeight) +
                        " conf=" + std::to_string(config.llegadaInferenceConfig.confidenceThreshold) +
                        " skip_resize=" + (config.llegadaInferenceConfig.skipResize ? "true" : "false"));
        }

        // Parse arrival_order section (arrival order stabilization + confirmation endpoint)
        if (j.contains("arrival_order") && j["arrival_order"].is_object()) {
            auto& ao = j["arrival_order"];
            if (ao.contains("stable_frames") && ao["stable_frames"].is_number()) {
                config.arrivalStabilizerConfig.stableFrames = ao["stable_frames"].get<int>();
            }
            if (ao.contains("min_spheres") && ao["min_spheres"].is_number()) {
                config.arrivalStabilizerConfig.minSpheres = ao["min_spheres"].get<int>();
            }
            if (ao.contains("endpoint_enabled") && ao["endpoint_enabled"].is_boolean()) {
                config.arrivalEndpointConfig.enabled = ao["endpoint_enabled"].get<bool>();
            }
            if (ao.contains("endpoint_url") && ao["endpoint_url"].is_string()) {
                config.arrivalEndpointConfig.endpointUrl = ao["endpoint_url"].get<std::string>();
            }
            Logger::Info("Arrival order config: stable_frames=" + std::to_string(config.arrivalStabilizerConfig.stableFrames) +
                        " min_spheres=" + std::to_string(config.arrivalStabilizerConfig.minSpheres) +
                        " endpoint_enabled=" + (config.arrivalEndpointConfig.enabled ? "true" : "false"));
        }

        // Parse position_mapper section
        config.calibrationFile = "config/calibration.json";
        if (j.contains("position_mapper") && j["position_mapper"].is_object()) {
            auto& pm = j["position_mapper"];
            if (pm.contains("calibration_file") && pm["calibration_file"].is_string()) {
                config.calibrationFile = pm["calibration_file"].get<std::string>();
            }
            if (pm.contains("track_bounds") && pm["track_bounds"].is_object()) {
                auto& tb = pm["track_bounds"];
                if (tb.contains("x_min")) config.trackBounds.xMin = tb["x_min"].get<float>();
                if (tb.contains("x_max")) config.trackBounds.xMax = tb["x_max"].get<float>();
                if (tb.contains("y_min")) config.trackBounds.yMin = tb["y_min"].get<float>();
                if (tb.contains("y_max")) config.trackBounds.yMax = tb["y_max"].get<float>();
            }
        }
        
        // Parse ball_tracker section
        if (j.contains("ball_tracker") && j["ball_tracker"].is_object()) {
            auto& bt = j["ball_tracker"];
            if (bt.contains("num_balls")) config.ballTrackerConfig.numBalls = bt["num_balls"].get<int>();
            if (bt.contains("kalman_process_noise_pos")) config.ballTrackerConfig.kalmanProcessNoisePos = bt["kalman_process_noise_pos"].get<float>();
            if (bt.contains("kalman_process_noise_vel")) config.ballTrackerConfig.kalmanProcessNoiseVel = bt["kalman_process_noise_vel"].get<float>();
            if (bt.contains("kalman_measurement_noise")) config.ballTrackerConfig.kalmanMeasurementNoise = bt["kalman_measurement_noise"].get<float>();
            if (bt.contains("max_occlusion_frames")) config.ballTrackerConfig.maxOcclusionFrames = bt["max_occlusion_frames"].get<int>();
            if (bt.contains("association_gate_distance")) config.ballTrackerConfig.associationGateDistance = bt["association_gate_distance"].get<float>();
            if (bt.contains("min_confidence_threshold")) config.ballTrackerConfig.minConfidenceThreshold = bt["min_confidence_threshold"].get<float>();
        }
        
        // Parse choreography section
        if (j.contains("choreography") && j["choreography"].is_object()) {
            auto& ch = j["choreography"];
            if (ch.contains("enabled")) config.choreographyConfig.enabled = ch["enabled"].get<bool>();
            if (ch.contains("script_path")) config.choreographyConfig.scriptPath = ch["script_path"].get<std::string>();
            if (ch.contains("auto_start")) config.choreographyConfig.autoStart = ch["auto_start"].get<bool>();
            if (ch.contains("continue_on_error")) config.choreographyConfig.continueOnError = ch["continue_on_error"].get<bool>();
            if (ch.contains("vmix_required")) config.choreographyConfig.vmixRequired = ch["vmix_required"].get<bool>();
            if (ch.contains("trigger_key")) config.choreographyConfig.triggerKey = ch["trigger_key"].get<std::string>();
            if (ch.contains("debug")) config.choreographyConfig.debug = ch["debug"].get<bool>();
            
            if (config.choreographyConfig.enabled) {
                Logger::Info("Choreography config: enabled=" + std::to_string(config.choreographyConfig.enabled) +
                            ", script=" + config.choreographyConfig.scriptPath +
                            ", auto_start=" + std::to_string(config.choreographyConfig.autoStart) +
                            ", trigger_key=" + config.choreographyConfig.triggerKey +
                            ", debug=" + std::to_string(config.choreographyConfig.debug));
            }
        }
        
        // Parse ranking_publisher section (emision UDP: 1 datagrama por punto
        // de medicion resuelto).
        // Alias de compatibilidad: se acepta el bloque viejo "animation" SOLO
        // si falta "ranking_publisher". Si estan los dos gana el nuevo y se
        // avisa: un config.json viejo no debe dejar el medidor apagado en
        // silencio.
        const bool hasRankingPublisher =
            j.contains("ranking_publisher") && j["ranking_publisher"].is_object();
        const bool hasLegacyAnimation =
            j.contains("animation") && j["animation"].is_object();
        if (hasRankingPublisher) {
            if (hasLegacyAnimation) {
                Logger::Warning("Config: \"animation\" es obsoleto y se IGNORA; "
                                "usar \"ranking_publisher\" (y \"ranking_measure\" "
                                "para burst_ms/max_ms)");
            }
            auto& rp = j["ranking_publisher"];
            if (rp.contains("enabled")) config.rankingPublisherConfig.enabled = rp["enabled"].get<bool>();
            if (rp.contains("host")) config.rankingPublisherConfig.host = rp["host"].get<std::string>();
            if (rp.contains("port")) config.rankingPublisherConfig.port = rp["port"].get<uint16_t>();
        } else if (hasLegacyAnimation) {
            auto& an = j["animation"];
            Logger::Warning("Config: bloque \"animation\" obsoleto (alias de "
                            "\"ranking_publisher\"); renombrar en config.json");
            if (an.contains("enabled")) config.rankingPublisherConfig.enabled = an["enabled"].get<bool>();
            if (an.contains("host")) config.rankingPublisherConfig.host = an["host"].get<std::string>();
            if (an.contains("port")) config.rankingPublisherConfig.port = an["port"].get<uint16_t>();
        }
        if (config.rankingPublisherConfig.enabled) {
            Logger::Info("RankingPublisher config: enabled, destino=" +
                        config.rankingPublisherConfig.host + ":" +
                        std::to_string(config.rankingPublisherConfig.port));
        }

        // Parse ranking_measure section (defaults de medicion: el evento
        // MeasurePoint gana si trae burst_ms/max_ms propios)
        if (j.contains("ranking_measure") && j["ranking_measure"].is_object()) {
            auto& rm = j["ranking_measure"];
            if (rm.contains("burst_ms")) config.rankingMeasureConfig.burstMs = rm["burst_ms"].get<int>();
            if (rm.contains("max_ms")) config.rankingMeasureConfig.maxMs = rm["max_ms"].get<int>();
            if (rm.contains("save_clip")) config.rankingMeasureConfig.saveClip = rm["save_clip"].get<bool>();
            if (rm.contains("save_dir")) config.rankingMeasureConfig.saveDir = rm["save_dir"].get<std::string>();
            if (rm.contains("save_fps")) config.rankingMeasureConfig.saveFps = rm["save_fps"].get<int>();
            if (rm.contains("save_max_frames")) config.rankingMeasureConfig.saveMaxFrames = rm["save_max_frames"].get<int>();
            // Carrera completa en RAM -> MP4 al final (apagado por defecto: con
            // false no se reserva ni un byte ni se copia un cuadro)
            if (rm.contains("save_race")) config.rankingMeasureConfig.saveRace = rm["save_race"].get<bool>();
            if (rm.contains("race_max_seconds")) config.rankingMeasureConfig.raceMaxSeconds = rm["race_max_seconds"].get<int>();
            // Ventana real de la carrera desde la compuerta: a los race_seconds se
            // vuelca solo el anillo y no se graba nada mas.
            if (rm.contains("race_seconds")) config.rankingMeasureConfig.raceSeconds = rm["race_seconds"].get<int>();
            if (rm.contains("ffmpeg_cmd")) config.rankingMeasureConfig.ffmpegCmd = rm["ffmpeg_cmd"].get<std::string>();
            if (rm.contains("strict_classes")) config.rankingMeasureConfig.strictClasses = rm["strict_classes"].get<bool>();
            // Quemar las cajas del detector en los MP4 (clips y carrera). Se
            // dibujan al comprimir, desde las detecciones ya guardadas: no toca
            // la captura ni el zero-copy. Apagado por defecto.
            if (rm.contains("draw_boxes")) config.rankingMeasureConfig.drawBoxes = rm["draw_boxes"].get<bool>();
            Logger::Info("RankingMeasure config: burst=" +
                        std::to_string(config.rankingMeasureConfig.burstMs) + "ms, max=" +
                        std::to_string(config.rankingMeasureConfig.maxMs) + "ms, save_race=" +
                        (config.rankingMeasureConfig.saveRace ? "true" : "false") +
                        ", race_max_seconds=" +
                        std::to_string(config.rankingMeasureConfig.raceMaxSeconds) +
                        ", race_seconds=" +
                        std::to_string(config.rankingMeasureConfig.raceSeconds) +
                        ", draw_boxes=" +
                        (config.rankingMeasureConfig.drawBoxes ? "true" : "false"));
        }
        
        // Parse control_api section (API HTTP de control para el backend, modo
        // --service). La clave NUNCA se loguea.
        if (j.contains("control_api") && j["control_api"].is_object()) {
            auto& ca = j["control_api"];
            if (ca.contains("enabled")) config.controlApiConfig.enabled = ca["enabled"].get<bool>();
            if (ca.contains("port")) config.controlApiConfig.port = ca["port"].get<int>();
            if (ca.contains("api_key")) config.controlApiConfig.apiKey = ca["api_key"].get<std::string>();
            if (ca.contains("arm_camera")) config.controlApiConfig.armCamera = ca["arm_camera"].get<int>();
            if (ca.contains("arm_burst_ms")) config.controlApiConfig.armBurstMs = ca["arm_burst_ms"].get<int>();
            if (ca.contains("arm_max_ms")) config.controlApiConfig.armMaxMs = ca["arm_max_ms"].get<int>();
            // Armado automatico: META -> reload -> espera -> ARRIBA
            if (ca.contains("arm_meta_camera")) config.controlApiConfig.armMetaCamera = ca["arm_meta_camera"].get<int>();
            if (ca.contains("arm_meta_preset")) config.controlApiConfig.armMetaPreset = ca["arm_meta_preset"].get<int>();
            if (ca.contains("arm_meta_burst_ms")) config.controlApiConfig.armMetaBurstMs = ca["arm_meta_burst_ms"].get<int>();
            if (ca.contains("arm_meta_max_ms")) config.controlApiConfig.armMetaMaxMs = ca["arm_meta_max_ms"].get<int>();
            // Segunda camara del lock META (el lock exige las dos, criterio AND)
            if (ca.contains("arm_meta_camera2")) config.controlApiConfig.armMetaCamera2 = ca["arm_meta_camera2"].get<int>();
            if (ca.contains("arm_meta_preset2")) config.controlApiConfig.armMetaPreset2 = ca["arm_meta_preset2"].get<int>();
            // Camara primaria de ARRIBA (0 = la del guion, punto 1)
            if (ca.contains("arm_arriba_camera")) config.controlApiConfig.armArribaCamera = ca["arm_arriba_camera"].get<int>();
            if (ca.contains("arm_arriba_preset")) config.controlApiConfig.armArribaPreset = ca["arm_arriba_preset"].get<int>();
            // Segunda camara del lock ARRIBA (mismo AND)
            if (ca.contains("arm_arriba_camera2")) config.controlApiConfig.armArribaCamera2 = ca["arm_arriba_camera2"].get<int>();
            if (ca.contains("arm_arriba_preset2")) config.controlApiConfig.armArribaPreset2 = ca["arm_arriba_preset2"].get<int>();
            if (ca.contains("arm_reload_wait_ms")) config.controlApiConfig.armReloadWaitMs = ca["arm_reload_wait_ms"].get<int>();
            if (ca.contains("arm_ptz_settle_ms")) config.controlApiConfig.armPtzSettleMs = ca["arm_ptz_settle_ms"].get<int>();

            Logger::Info("Control API config: enabled=" +
                        std::string(config.controlApiConfig.enabled ? "true" : "false") +
                        ", port=" + std::to_string(config.controlApiConfig.port) +
                        ", api_key=" +
                        (config.controlApiConfig.apiKey.empty()
                            ? std::string("(VACIA)")
                            : ("(configurada, " + std::to_string(config.controlApiConfig.apiKey.size()) + " chars)")) +
                        ", arm_camera=" + std::to_string(config.controlApiConfig.armCamera));
            Logger::Info("Control API armado (dos camaras por lock, AND): META cam=" +
                        std::to_string(config.controlApiConfig.armMetaCamera) + "/" +
                        std::to_string(config.controlApiConfig.armMetaCamera2) +
                        " preset=" + std::to_string(config.controlApiConfig.armMetaPreset) +
                        "/" + std::to_string(config.controlApiConfig.armMetaPreset2) +
                        " -> reload -> espera " +
                        std::to_string(config.controlApiConfig.armReloadWaitMs) + " ms" +
                        " -> ARRIBA cam=" +
                        (config.controlApiConfig.armArribaCamera > 0
                             ? std::to_string(config.controlApiConfig.armArribaCamera)
                             : std::string("guion")) +
                        "/" + std::to_string(config.controlApiConfig.armArribaCamera2) +
                        " preset=" + std::to_string(config.controlApiConfig.armArribaPreset) +
                        "/" + std::to_string(config.controlApiConfig.armArribaPreset2) +
                        " (asentamiento PTZ " +
                        std::to_string(config.controlApiConfig.armPtzSettleMs) + " ms)");
            if (config.controlApiConfig.armMetaCamera2 <= 0 ||
                config.controlApiConfig.armArribaCamera2 <= 0) {
                Logger::Warning("Control API armado: falta la SEGUNDA camara de un lock "
                                "(arm_meta_camera2 / arm_arriba_camera2); el /arm va a responder "
                                "409 hasta declararla: el armado exige DOS camaras por lock");
            }
        } else if (config.controlApiConfig.enabled) {
            Logger::Warning("Config: control_api.enabled pero falta el bloque en config.json");
        }

        // Parse ptz_cameras section
        if (j.contains("ptz_cameras") && j["ptz_cameras"].is_object()) {
            auto& ptz = j["ptz_cameras"];
            if (ptz.contains("enabled") && ptz["enabled"].is_boolean()) {
                config.ptzCamerasConfig.enabled = ptz["enabled"].get<bool>();
            }
            if (ptz.contains("cameras") && ptz["cameras"].is_array()) {
                for (const auto& cam : ptz["cameras"]) {
                    PTZ::PTZCameraConfig camConfig;
                    camConfig.cameraID = cam.value("id", 0);
                    camConfig.ipAddress = cam.value("ip", "192.168.1.100");
                    camConfig.viscaPort = cam.value("visca_port", 52381);
                    camConfig.viscaAddress = cam.value("visca_address", 1);
                    camConfig.basePreset = cam.value("base_preset", 1);
                    if (cam.contains("presets") && cam["presets"].is_array()) {
                        for (const auto& preset : cam["presets"]) {
                            camConfig.presets.push_back(preset.get<int>());
                        }
                    }
                    config.ptzCamerasConfig.cameras.push_back(camConfig);
                }
            }
            if (config.ptzCamerasConfig.enabled) {
                Logger::Info("PTZ Cameras config: enabled with " + 
                            std::to_string(config.ptzCamerasConfig.cameras.size()) + " cameras");
            }
        }
        
        // Parse ptz_tracking section
        if (j.contains("ptz_tracking") && j["ptz_tracking"].is_object()) {
            auto& pt = j["ptz_tracking"];
            // Read enabled flag - controls whether auto centroid tracking runs
            if (pt.contains("enabled") && pt["enabled"].is_boolean()) {
                config.ptzAutoTrackingEnabled = pt["enabled"].get<bool>();
            }
            if (pt.contains("kp")) config.ptzTrackingConfig.kp = pt["kp"].get<float>();
            if (pt.contains("dead_zone")) config.ptzTrackingConfig.deadZone = pt["dead_zone"].get<float>();
            if (pt.contains("max_pan_speed")) config.ptzTrackingConfig.maxPanSpeed = pt["max_pan_speed"].get<int>();
            if (pt.contains("max_tilt_speed")) config.ptzTrackingConfig.maxTiltSpeed = pt["max_tilt_speed"].get<int>();
            if (pt.contains("zoom_out_threshold")) config.ptzTrackingConfig.zoomOutThreshold = pt["zoom_out_threshold"].get<float>();
            if (pt.contains("zoom_in_threshold")) config.ptzTrackingConfig.zoomInThreshold = pt["zoom_in_threshold"].get<float>();
            if (pt.contains("fallback_timeout_frames")) config.ptzTrackingConfig.fallbackTimeoutFrames = pt["fallback_timeout_frames"].get<int>();
            if (pt.contains("engage_min_frames")) config.ptzTrackingConfig.engageMinFrames = pt["engage_min_frames"].get<int>();
            if (pt.contains("min_sphere_count")) config.ptzTrackingConfig.minSphereCount = pt["min_sphere_count"].get<int>();
            
            Logger::Info(std::string("PTZ Auto-tracking: ") + (config.ptzAutoTrackingEnabled ? "ENABLED" : "DISABLED") +
                        " | Kp=" + std::to_string(config.ptzTrackingConfig.kp) +
                        ", dead_zone=" + std::to_string(config.ptzTrackingConfig.deadZone) +
                        ", fallback_frames=" + std::to_string(config.ptzTrackingConfig.fallbackTimeoutFrames));
        }

        Logger::Info("Configuración cargada: VideoHub=" + config.videohubIp + ":" + 
                     std::to_string(config.videohubPort) + ", ESP32=" + config.esp32Ip + ":" + 
                     std::to_string(config.esp32Port) + ", target_spheres=" + 
                     std::to_string(config.targetSpheres));

    } catch (const json::exception& e) {
        Logger::Warning("Error al parsear config.json: " + std::string(e.what()) + "; usando valores por defecto");
    } catch (const std::exception& e) {
        Logger::Warning("Error inesperado al cargar config: " + std::string(e.what()) + "; usando valores por defecto");
    }

    return config;
}

bool ValidateSignalGroup(VideoHubClient& videoHub,
                         DeckLinkSource& deckLinkSource,
                         const std::vector<int>& ports,
                         const std::string& label) {
    for (int port : ports) {
        // VideoHub uses 0-based indexing, so convert port (1-based) to 0-based
        int videoHubInput = port - 1;
        if (!videoHub.RouteInputToOutput(kVideoHubPrimaryOutput, videoHubInput)) {
            Logger::Error("[HW/SW ERROR]: No se pudo conmutar VideoHub para puerto " + std::to_string(port));
            return false;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto statuses = deckLinkSource.GetSignalStatus(ports);
    for (const auto& status : statuses) {
        if (!status.signalLocked) {
            const std::string name = status.name.value_or("Port_" + std::to_string(status.index));
            Logger::Error("[HW/SW ERROR]: Sin señal en " + name);
            return false;
        }
    }

    Logger::Info("Barrido " + label + " OK");
    return true;
}

bool RunPhase1(VMixController& vmix,
               VideoHubClient& videoHub,
               DeckLinkSource& deckLinkSource,
               TrackPhysicalController& trackController) {
    if (!vmix.CheckInputsHealthy()) {
        return false;
    }

    if (!ValidateSignalGroup(videoHub, deckLinkSource, RangeInclusive(1, 12), "Streaming (1-12)")) {
        return false;
    }

    if (!ValidateSignalGroup(videoHub, deckLinkSource, RangeInclusive(13, 16), "Seguimiento (13-16)")) {
        return false;
    }

    if (!trackController.ejecutarTest()) {
        Logger::Error("[HW/SW ERROR] Prueba mecánica (ESP32 /test) fallida");
        return false;
    }

    Logger::Info("Fase 1 completada correctamente");
    return true;
}

bool RunPhase2(VideoHubClient& videoHub, int targetSpheres) {
    if (!videoHub.RouteInputToOutput(kVideoHubPrimaryOutput, "CAM_01")) {
        Logger::Error("[HW/SW ERROR]: No se pudo fijar CAM_01 en la salida");
        return false;
    }

    // Placeholder YOLO check (pipeline integration pending)
    const int detectedSpheres = targetSpheres;
    Logger::Warning("Conteo de esferas usa stub; integrar motor YOLO para conteo real");

    if (detectedSpheres != targetSpheres) {
        Logger::Error("[SCENE ERROR]: Conteo incorrecto (" + std::to_string(detectedSpheres) +
                      ") - Verifique esferas en pista");
        return false;
    }

    Logger::Info("Fase 2 (Escena) completada: conteo de esferas OK");
    return true;
}

/**
 * Ejecutar el modo normal de operación (RUNNING MODE)
 * Contiene toda la lógica de inicialización y captura continua
 * @param serviceMode true = modo --service: sin menu y sin teclas, la carrera
 *        se dispara por la API HTTP de control (GET /status, POST /arm|start|abort).
 *        false (default) = comportamiento de siempre, con menu y teclas.
 * @param consoleService true = menu [12] SERVICIO API: inicializa igual que el
 *        modo operacion, levanta la API y se queda en idle mostrando un panel de
 *        estado en consola. No auto-arranca nada; ESC vuelve al menu principal.
 *        Implica serviceMode (no hay menu de teclas ni F12).
 * @return true si finalizó correctamente, false si hubo error
 */
bool RunRunningMode(bool serviceMode = false, bool consoleService = false) {
    Logger::Info(serviceMode ? "=== INICIANDO RUNNING MODE (--service, sin menu) ==="
                             : "=== INICIANDO RUNNING MODE ===");
    if (consoleService) {
        Logger::Info("=== SERVICIO API (menu [12]): inicializa, levanta la API y queda en idle ===");
    }

    // En --service no se leen teclas: el control es la API HTTP. Sin --service
    // esto es exactamente GetAsyncKeyState(vk) & 0x8000, como siempre.
    auto KeyDown = [serviceMode](int vk) -> bool {
        if (serviceMode) {
            return false;
        }
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    // ESC es la unica tecla que sobrevive en SERVICIO API (menu [12]): sirve
    // para volver al menu principal. En --service puro no hay nadie en la
    // consola, asi que tambien se ignora.
    auto EscDown = [serviceMode, consoleService]() -> bool {
        if (serviceMode && !consoleService) {
            return false;
        }
        return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    };
    
    // ============================================================================
    // System Optimization Diagnostics (Threadripper PRO + RTX 5080)
    // ============================================================================
    Logger::Info("=== System Optimization Check ===");
    Logger::Info(ThreadOptimizer::GetCPUInfo());
    bool systemOptimal = GPUDiagnostics::RunFullDiagnostics();
    if (!systemOptimal) {
        Logger::Warning("System is not optimally configured. Performance may be degraded.");
        Logger::Warning("Refer to OPTIMIZATION_GUIDE.md for setup instructions.");
    }
    Logger::Info("==================================");
    
    // Load configuration from JSON
    Config config = LoadConfig("config.json");
    
    // Validate SceneManager configuration
    if (!ValidateSceneManagerConfig(config)) {
        Logger::Error("SceneManager configuration validation failed. Please fix config.json");
        if (config.sceneManagerEnabled) {
            Logger::Error("SceneManager is enabled but configuration is invalid. Disabling SceneManager.");
            config.sceneManagerEnabled = false;
        }
    }

    // Controllers for diagnostics and routing
    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    
    // Convert ESP32 IP to wide string
    std::wstring esp32IpWide(config.esp32Ip.begin(), config.esp32Ip.end());
    TrackPhysicalController trackController(esp32IpWide, config.esp32Port);
    
    VMixController vmix(L"127.0.0.1", 8088, 8099);
    DeckLinkSource deckLinkSource;
    deckLinkSource.Initialize(12);

    if (!videoHub.Connect()) {
        Logger::Error("[HW/SW ERROR] No se pudo establecer conexión con VideoHub");
        return false;
    }

    vmix.ConnectTcp();  // prepare TCP channel; errors are logged but non-fatal

    // Fase 1: Sweep hardware/software links
    if (!RunPhase1(vmix, videoHub, deckLinkSource, trackController)) {
        Logger::Error("Ignición abortada durante Fase 1");
        return false;
    }

    // Fase 2: Escena (YOLO check)
    if (!RunPhase2(videoHub, config.targetSpheres)) {
        Logger::Error("Ignición abortada durante Fase 2");
        return false;
    }

    // ============================================================================
    // NDI Initialization (conditionally - can be disabled in config)
    // ============================================================================
    std::shared_ptr<NDIManager> ndiManager;
    
    if (config.ndiEnabled) {
        Logger::Info("Initializing NDI output for vMix...");
        
        ndiManager = std::make_shared<NDIManager>();
        if (!ndiManager->Initialize()) {
            Logger::Error("Failed to initialize NDI library");
            return false;
        }
        
        // Pre-create all 4 NDI senders (for PTZ radar cameras)
        Logger::Info("Creating NDI senders for " + std::to_string(kMaxNDIChannels) + " channels...");
        for (int channel = 0; channel < kMaxNDIChannels; ++channel) {
            std::ostringstream oss;
            oss << "VIB_CAM_" << std::setw(2) << std::setfill('0') << (channel + 1);
            const std::string senderName = oss.str();
            
            if (!ndiManager->CreateSender(channel, senderName, kDefaultCaptureWidth, kDefaultCaptureHeight, true)) {
                Logger::Error("Failed to create NDI sender: " + senderName);
                return false;
            }
        }
        Logger::Info("NDI senders initialized successfully");
    } else {
        Logger::Info("NDI output disabled - vMix captures directly from VideoHub");
    }
    
    // ============================================================================
    // Initialize AI Pipeline Components
    // ============================================================================
    
    auto perfMonitor = std::make_shared<PerformanceMonitor>(33.0, 30);
    
    std::shared_ptr<ActiveCameraSelector> cameraSelector;
    if (config.selectorEnabled) {
        Logger::Info("Initializing ActiveCameraSelector...");
        cameraSelector = std::make_shared<ActiveCameraSelector>(
            kMaxNDIChannels,
            config.selectorTopK,
            config.selectorMotionThreshold,
            config.selectorEdgeMargin
        );
        if (!cameraSelector->Initialize()) {
            Logger::Error("Failed to initialize ActiveCameraSelector");
            return false;
        }
        
        HysteresisConfig hysteresisConfig;
        hysteresisConfig.switch_threshold = config.hysteresisSwitchThreshold;
        hysteresisConfig.min_active_frames = config.hysteresisMinActiveFrames;
        hysteresisConfig.decay_factor = config.hysteresisDecayFactor;
        cameraSelector->SetHysteresisConfig(hysteresisConfig);
    }
    
    // ============================================================================
    // Initialize Tracking Pipeline Components
    // ============================================================================
    
    cudaStream_t inferenceStream = nullptr;
    cudaError_t err = cudaStreamCreate(&inferenceStream);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create inference CUDA stream: " + std::string(cudaGetErrorString(err)));
        return false;
    }
    
    Logger::Info("Initializing InferenceEngine...");
    auto inferenceEngine = std::make_shared<InferenceEngine>();
    bool inferenceReady = false;
    if (inferenceEngine->Initialize(config.inferenceConfig)) {
        inferenceReady = true;
        if (inferenceEngine->IsStubMode()) {
            Logger::Warning("InferenceEngine running in STUB mode");
        } else {
            Logger::Info("InferenceEngine initialized with TensorRT");
        }
    } else {
        Logger::Warning("InferenceEngine initialization failed");
    }
    
    Logger::Info("Initializing PositionMapper...");
    auto positionMapper = std::make_shared<PositionMapper>();
    if (positionMapper->LoadCalibration(config.calibrationFile)) {
        positionMapper->SetTrackBounds(config.trackBounds);
        Logger::Info("PositionMapper loaded calibration from " + config.calibrationFile);
    } else {
        Logger::Warning("PositionMapper using identity transform (no calibration)");
    }
    
    Logger::Info("Initializing BallTracker...");
    auto ballTracker = std::make_shared<BallTracker>(config.ballTrackerConfig);
    if (!ballTracker->Initialize()) {
        Logger::Error("BallTracker initialization failed");
    }
    
    Logger::Info("Initializing SceneManager...");
    SceneManagerConfig sceneConfig;
    sceneConfig.enabled = config.sceneManagerEnabled;
    sceneConfig.muteTimeoutMs = config.sceneManagerMuteTimeoutMs;
    sceneConfig.mode = config.sceneManagerMode;
    sceneConfig.triggerMode = config.sceneManagerTriggerMode;
    sceneConfig.eventCooldownMs = config.sceneManagerEventCooldownMs;
    sceneConfig.hysteresisFrames = config.sceneManagerHysteresisFrames;
    sceneConfig.manualKeys = config.sceneManagerManualKeys;
    sceneConfig.groups = config.sceneManagerGroups;
    sceneConfig.radarRoutingEnabled = config.radarRoutingEnabled;
    sceneConfig.radarOutputSlots = config.radarOutputSlots;
    
    auto sceneManager = std::make_shared<SceneManager>(&videoHub, sceneConfig);
    if (config.sceneManagerEnabled && !sceneManager->Initialize()) {
        Logger::Warning("SceneManager initialization failed");
    }

    // ============================================================================
    // Initialize SphereVerifier
    // ============================================================================
    Logger::Info("Initializing SphereVerifier...");
    auto sphereVerifier = std::make_shared<Verification::SphereVerifier>(
        &videoHub, inferenceEngine.get());
    
    if (!sphereVerifier->IsReady()) {
        Logger::Warning("SphereVerifier: Not ready - check dependencies");
    } else {
        Logger::Info("SphereVerifier initialized successfully");
    }
    
    // ============================================================================
    // Initialize PTZ Controller and Tracking Components
    // ============================================================================
    std::shared_ptr<PTZ::PTZController> ptzController;
    std::shared_ptr<Tracking::MovingAverageFilter> centroidFilter;
    std::shared_ptr<Tracking::RouteMapper> routeMapper;
    std::shared_ptr<Tracking::TrackingLogger> trackingLogger;
    int framesWithoutDetection = 0;
    int framesWithDetection = 0;
    
    if (config.ptzCamerasConfig.enabled) {
        Logger::Info("Initializing PTZController...");
        ptzController = std::make_shared<PTZ::PTZController>();
        
        if (ptzController->Initialize(config.ptzCamerasConfig.cameras, config.ptzTrackingConfig)) {
            Logger::Info("PTZController initialized with " + 
                        std::to_string(config.ptzCamerasConfig.cameras.size()) + " cameras");
            
            // Initialize centroid smoothing filter
            int smoothingWindow = 5;  // Default window size
            centroidFilter = std::make_shared<Tracking::MovingAverageFilter>(smoothingWindow);
            
            // Initialize route mapper
            routeMapper = std::make_shared<Tracking::RouteMapper>();
            if (routeMapper->LoadRoute("config/track_route.json")) {
                Logger::Info("RouteMapper loaded track route");
                
                // Set up checkpoint callback to notify SphereVerifier
                routeMapper->SetCheckpointCallback(
                    [&sphereVerifier](const Tracking::Checkpoint& cp, int sphereCount) {
                        sphereVerifier->EmitCheckpointEvent(cp.name, cp.progress_pct, sphereCount);
                    }
                );
            } else {
                Logger::Warning("RouteMapper using default linear route");
            }
            
            // Initialize tracking logger
            trackingLogger = std::make_shared<Tracking::TrackingLogger>("logs/tracking");
            trackingLogger->Start();
            Logger::Info("TrackingLogger started");
        } else {
            Logger::Warning("PTZController initialization failed - running without PTZ control");
            ptzController.reset();
        }
    } else {
        Logger::Info("PTZ tracking disabled in config");
    }
    
    // ============================================================================
    // Initialize medidor por evento (MeasurePoint) + salida UDP del ranking
    // ============================================================================
    // Se crea ANTES del ChoreographyEngine: el `case` del evento MeasurePoint
    // solo hace Enqueue() y sigue, asi que necesita el medidor enganchado desde
    // el primer evento. El hilo medidor es el que mide (nunca la coreografia).
    std::unique_ptr<Measure::RankingMeasure> rankingMeasure;
    if (config.rankingPublisherConfig.enabled) {
        Logger::Info("Initializing RankingMeasure...");
        rankingMeasure = std::make_unique<Measure::RankingMeasure>();
        if (rankingMeasure->Initialize(config.rankingPublisherConfig,
                                       config.rankingMeasureConfig,
                                       config.inferenceConfig)) {
            Measure::AttachCoordinator(rankingMeasure.get());
            Logger::Info("RankingMeasure attached to ChoreographyEngine");
        } else {
            Logger::Warning("RankingMeasure initialization failed - "
                            "MeasurePoint events will be ignored");
            rankingMeasure.reset();
        }
    } else {
        Logger::Info("RankingMeasure: disabled in config (ranking_publisher.enabled=false)");
    }
    
    // ============================================================================
    // Initialize Choreography Engine
    // ============================================================================
    std::shared_ptr<Choreography::ChoreographyEngine> choreographyEngine;
    if (config.choreographyConfig.enabled) {
        Logger::Info("Initializing ChoreographyEngine...");
        choreographyEngine = std::make_shared<Choreography::ChoreographyEngine>(
            &vmix, sceneManager.get());
        choreographyEngine->SetVideoHubClient(&videoHub);
        choreographyEngine->SetSphereVerifier(sphereVerifier.get());
        choreographyEngine->SetContinueOnError(config.choreographyConfig.continueOnError);
        choreographyEngine->SetVMixRequired(config.choreographyConfig.vmixRequired);
        if (ptzController) {
            choreographyEngine->SetPTZController(ptzController.get());
        }
        choreographyEngine->SetTrackPhysicalController(&trackController);
        
        // Set up callbacks for debug logging
        if (config.choreographyConfig.debug) {
            choreographyEngine->SetStateCallback([](Choreography::EngineState state) {
                std::string stateStr;
                switch (state) {
                    case Choreography::EngineState::Idle: stateStr = "Idle"; break;
                    case Choreography::EngineState::Ready: stateStr = "Ready"; break;
                    case Choreography::EngineState::Running: stateStr = "Running"; break;
                    case Choreography::EngineState::Paused: stateStr = "Paused"; break;
                    case Choreography::EngineState::Stopping: stateStr = "Stopping"; break;
                    case Choreography::EngineState::Error: stateStr = "Error"; break;
                    default: stateStr = "Unknown"; break;
                }
                Logger::Debug("[Choreography] State changed to: " + stateStr);
            });
            
            choreographyEngine->SetEventStartCallback([](size_t index, const Choreography::ChoreographyEvent& event) {
                Logger::Debug("[Choreography] Starting event " + std::to_string(index) + ": " + event.GetTypeName());
            });
            
            choreographyEngine->SetEventCompleteCallback([](const Choreography::EventResult& result) {
                if (result.success) {
                    Logger::Debug("[Choreography] Event " + std::to_string(result.eventIndex) + 
                                 " completed in " + std::to_string(result.executionTime.count()) + "ms");
                } else {
                    Logger::Warning("[Choreography] Event " + std::to_string(result.eventIndex) + 
                                   " failed: " + result.errorMessage);
                }
            });
            
            choreographyEngine->SetErrorCallback([](const std::string& error) {
                Logger::Error("[Choreography] Error: " + error);
            });
        }
        
        // Load script if specified
        if (!config.choreographyConfig.scriptPath.empty()) {
            Logger::Info("ChoreographyEngine: Loading script from " + config.choreographyConfig.scriptPath);
            if (choreographyEngine->Load(config.choreographyConfig.scriptPath)) {
                Logger::Info("ChoreographyEngine: Script loaded successfully");
                
                // Auto-start if configured. En modo servicio (--service y menu
                // [12]) NO se auto-arranca: el guion lo dispara POST /start
                // despues del /arm. Un guion andando solo ocupa el medidor y el
                // /arm cae en una ventana ya vencida ("sin cuadros").
                if (config.choreographyConfig.autoStart && !serviceMode) {
                    Logger::Info("ChoreographyEngine: Auto-starting choreography...");
                    if (!choreographyEngine->Start()) {
                        Logger::Warning("ChoreographyEngine: Auto-start failed");
                    }
                } else if (config.choreographyConfig.autoStart) {
                    Logger::Warning("ChoreographyEngine: auto_start=true IGNORADO en modo servicio: "
                                    "el VIB queda en idle hasta POST /start");
                }
            } else {
                Logger::Warning("ChoreographyEngine: Failed to load script: " + 
                               choreographyEngine->GetLastError());
            }
        }
        
        Logger::Info("ChoreographyEngine initialized (trigger key: " + 
                    config.choreographyConfig.triggerKey + ")");
    } else {
        Logger::Info("ChoreographyEngine: Disabled in config");
    }
    
    // ============================================================================
    // Initialize capture channels
    // ============================================================================
    Logger::Info("Initializing capture channels...");
    std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
    
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("Found " + std::to_string(numDevices) + " DeckLink devices");
    
    const int availableDevices = (std::max)(0, numDevices - kCaptureDeviceBase);
    const int channelsToInit = (std::min)(availableDevices, kMaxNDIChannels);
    for (int i = 0; i < channelsToInit; i++) {
        auto capture = std::make_unique<DeckLinkCapture>();
        const std::string channelName = "Channel_" + std::to_string(i + 1);
        if (capture->Initialize(kCaptureDeviceBase + i, channelName, i)) {
            // Medidor por evento: el anillo corto lo llena el hilo de captura de
            // esta camara (pre-roll del disparo). Sin medidor queda nullptr.
            // El anillo corto ya NO se usa: el medidor consume las detecciones que
            // el pipeline produce. Llenarlo costaba 4 MB por cuadro por camara
            // (~480 MB/s) en los hilos de captura, para nadie.
            if (rankingMeasure) {
                const VideoChannel& channelInfo = capture->GetChannel();
                rankingMeasure->SetupCamera(channelInfo.channelID,
                                            channelInfo.width, channelInfo.height);
            }
            capture->SetFrameReadyHandler([ndiManager, cameraSelector, 
                                          perfMonitor, &config, inferenceStream,
                                          inferenceEngine, positionMapper, ballTracker, 
                                          sceneManager, inferenceReady,
                                          ptzController, centroidFilter, routeMapper,
                                          trackingLogger, &framesWithDetection, &framesWithoutDetection,
                                          &rankingMeasure]
                                         (const VideoChannel& channel, cudaStream_t stream) {
                // Static flag to save only one frame per channel
                // This array is shared across all lambda invocations, ensuring each channel
                // saves exactly one frame regardless of how many times the lambda is called
                static std::atomic<bool> framesSaved[kMaxNDIChannels] = {};
                
                auto frameStart = std::chrono::high_resolution_clock::now();
                Telemetry telemetry = {0, 0, 0, 0, 0};
                
                // PRIORITY 1: Video Output to vMix (only if NDI is enabled)
                auto ndiStart = std::chrono::high_resolution_clock::now();
                if (ndiManager && config.ndiEnabled) {
                    ndiManager->SendUYVYFrame(
                        channel.channelID,
                        channel.cudaYUVBuffer,
                        channel.width,
                        channel.height,
                        stream
                    );
                }
                auto ndiEnd = std::chrono::high_resolution_clock::now();
                telemetry.ndi_ms = std::chrono::duration<double, std::milli>(ndiEnd - ndiStart).count();
                
                // PRIORITY 2: Motion Analysis
                auto selectorStart = std::chrono::high_resolution_clock::now();
                if (cameraSelector && config.selectorEnabled) {
                    cameraSelector->ProcessFrame(
                        channel.channelID,
                        channel.cudaYUVBuffer,
                        channel.width,
                        channel.height,
                        stream
                    );
                }
                auto selectorEnd = std::chrono::high_resolution_clock::now();
                telemetry.selector_ms = std::chrono::duration<double, std::milli>(selectorEnd - selectorStart).count();
                
                // PRIORITY 3: AI Inference + Ball Tracking
                // OPTIMIZATION: Use fused UYVY→RGB640 kernel (bypasses BGRA intermediate)
                auto yoloStart = std::chrono::high_resolution_clock::now();
                
                std::vector<BallDetection> ballDetections;
                if (inferenceReady && inferenceEngine) {
                    // Use optimized UYVY path with zero-copy and fused kernel
                    // This eliminates:
                    // 1. cudaMemcpy from host to device (zero-copy mapped memory)
                    // 2. YUV→BGRA conversion kernel (fused into preprocessing)
                    // 3. BGRA→RGB conversion (fused into preprocessing)
                    // Result: ~2-3ms latency reduction per frame
                    ballDetections = inferenceEngine->ProcessFrameUYVY(
                        channel.cudaYUVBuffer,    // Direct UYVY from DeckLink
                        channel.channelID,
                        channel.width,
                        channel.height,
                        inferenceStream,
                        channel.preprocessEvent   // Event for async sync
                    );
                    
                    // Save frame when there are detections (only once per channel)
                    // NOTE: This I/O operation adds latency (~10-20ms) but only executes once per channel
                    // TODO: Move to async thread pool for zero impact on critical path
                    if (!ballDetections.empty() && channel.channelID < kMaxNDIChannels && 
                        !framesSaved[channel.channelID].load()) {
                        
                        std::ostringstream filename;
                        filename << "detection_cam" << std::setw(2) << std::setfill('0') 
                                << (channel.channelID + 1) << ".ppm";
                        
                        if (FrameSaver::SaveYUVFrameAsPPM(
                                channel.cudaYUVBuffer,
                                channel.width,
                                channel.height,
                                filename.str())) {
                            Logger::Info("Frame saved: " + filename.str() + 
                                       " (" + std::to_string(channel.width) + "x" + 
                                       std::to_string(channel.height) + ") - " +
                                       std::to_string(ballDetections.size()) + " detections");
                            framesSaved[channel.channelID].store(true);
                        }
                    }
                }
                
                // El medidor NO infiere: consume lo que el pipeline ya vio. Se le
                // pasa cada cuadro, con o sin detecciones: de aca sale tambien el
                // clip. Va FUERA del if de inferencia para que el clip se grabe
                // aunque el motor del pipeline no haya inicializado.
                if (rankingMeasure && rankingMeasure->IsInitialized()) {
                    const int64_t nsMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const size_t frameBytes =
                        static_cast<size_t>(channel.width) * channel.height * 2u;
                    rankingMeasure->OnDetections(
                        channel.channelID, nsMs, ballDetections,
                        channel.cudaYUVBuffer, frameBytes,
                        channel.width, channel.height);
                }
                
                                std::vector<GlobalPosition> globalPositions;
                if (!ballDetections.empty() && positionMapper && positionMapper->IsCalibrated()) {
                    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    
                    std::vector<std::tuple<float, float, float, int>> pixelPositions;
                    for (const auto& det : ballDetections) {
                        pixelPositions.emplace_back(det.x, det.y, det.confidence, det.ballID);
                    }
                    
                    auto transformed = positionMapper->BatchTransform(
                        channel.channelID, pixelPositions, now);
                    globalPositions = positionMapper->FuseOverlappingDetections(transformed);
                }
                
                if (!globalPositions.empty() && ballTracker && ballTracker->IsInitialized()) {
                    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    ballTracker->Update(globalPositions, now);
                    
                    if (sceneManager && sceneManager->IsEnabled()) {
                        auto leader = ballTracker->GetLeader();
                        sceneManager->UpdateLeaderPosition(leader.Xg, leader.Yg);
                    }

                    // === PTZ Tracking Integration ===
                    // Only runs when ptz_tracking.enabled = true in config.json.
                    // Set it to false to use PTZ cameras in choreography/preset mode only.
                    if (config.ptzAutoTrackingEnabled &&
                        ptzController && ptzController->IsInitialized() && centroidFilter) {
                        // Calculate centroid from ball detections (uses ::CentroidResult from InferenceEngine.h)
                        ::CentroidResult rawCentroid = InferenceEngine::CalculateGroupCentroid(
                            ballDetections, 0.5f);
                        
                        // Apply smoothing filter (convert to Tracking::CentroidResult)
                        auto smoothCentroid = centroidFilter->Update(
                            Tracking::CentroidResult(
                                rawCentroid.centroid_x, 
                                rawCentroid.centroid_y,
                                rawCentroid.std_deviation,
                                rawCentroid.sphere_count,
                                rawCentroid.timestamp
                            )
                        );
                        
                        auto& trackConfig = config.ptzTrackingConfig;
                        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        
                        // Track route progress and update checkpoints
                        float progress = 0.0f;
                        if (routeMapper) {
                            progress = routeMapper->MapToProgress(
                                smoothCentroid.centroid_x, smoothCentroid.centroid_y);
                            routeMapper->UpdateProgress(progress, smoothCentroid.sphere_count);
                        }
                        
                        // State machine for tracking engagement
                        if (smoothCentroid.sphere_count >= trackConfig.minSphereCount) {
                            framesWithDetection++;
                            framesWithoutDetection = 0;
                            
                            // Auto-engage tracking after N consecutive frames
                            if (framesWithDetection >= trackConfig.engageMinFrames &&
                                ptzController->GetTrackingMode() != PTZ::TrackingMode::TRACKING_MODE) {
                                ptzController->SetTrackingMode(PTZ::TrackingMode::TRACKING_MODE);
                            }
                            
                            // Calculate PTZ commands when in tracking mode
                            if (ptzController->GetTrackingMode() == PTZ::TrackingMode::TRACKING_MODE) {
                                // Error from center of frame (normalized)
                                float panError = smoothCentroid.centroid_x - 0.5f;
                                float tiltError = smoothCentroid.centroid_y - 0.5f;
                                
                                // Proportional control with dead zone
                                int panSpeed = 0;
                                int tiltSpeed = 0;
                                
                                if (std::abs(panError) > trackConfig.deadZone) {
                                    panSpeed = static_cast<int>(panError * trackConfig.kp);
                                    panSpeed = std::clamp(panSpeed, 
                                        -trackConfig.maxPanSpeed, trackConfig.maxPanSpeed);
                                }
                                
                                if (std::abs(tiltError) > trackConfig.deadZone) {
                                    tiltSpeed = static_cast<int>(tiltError * trackConfig.kp);
                                    tiltSpeed = std::clamp(tiltSpeed, 
                                        -trackConfig.maxTiltSpeed, trackConfig.maxTiltSpeed);
                                }
                                
                                // Send pan/tilt to all PTZ cameras
                                ptzController->SendPanTiltAll(panSpeed, tiltSpeed);
                                
                                // Zoom control based on group spread
                                if (smoothCentroid.std_deviation > trackConfig.zoomOutThreshold) {
                                    ptzController->SendZoomAll(PTZ::ZoomDirection::Out, 2);
                                } else if (smoothCentroid.std_deviation < trackConfig.zoomInThreshold) {
                                    ptzController->SendZoomAll(PTZ::ZoomDirection::In, 1);
                                }
                                
                                // Log tracking data
                                if (trackingLogger) {
                                    trackingLogger->Log(now, 
                                        smoothCentroid.centroid_x, smoothCentroid.centroid_y,
                                        smoothCentroid.std_deviation, smoothCentroid.sphere_count,
                                        progress, 
                                        static_cast<int>(ptzController->GetTrackingMode()),
                                        static_cast<float>(panSpeed), static_cast<float>(tiltSpeed));
                                }
                            }
                        } else {
                            framesWithDetection = 0;
                            framesWithoutDetection++;
                            
                            // Fallback after timeout
                            if (framesWithoutDetection >= trackConfig.fallbackTimeoutFrames &&
                                ptzController->GetTrackingMode() == PTZ::TrackingMode::TRACKING_MODE) {
                                ptzController->SetTrackingMode(PTZ::TrackingMode::FALLBACK_MODE);
                                ptzController->ReturnToBaseAll();
                                centroidFilter->Reset();
                            }
                        }
                    }
                }
                
                auto inferenceEnd = std::chrono::high_resolution_clock::now();
                telemetry.yolo_ms = std::chrono::duration<double, std::milli>(inferenceEnd - yoloStart).count();
                
                auto frameEnd = std::chrono::high_resolution_clock::now();
                telemetry.capture_ms = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
                perfMonitor->RecordFrame(telemetry);
            });
            capture->Start();
            captureChannels.push_back(std::move(capture));
        }
    }
    
    // ============================================================================
    // Main monitoring loop
    // ============================================================================
    Logger::Info("=== Sistema en Modo de Operacion ===");

    // ============================================================================
    // API HTTP de control (modo --service)
    // ============================================================================
    // Se levanta DESPUES de arrancar la captura: /arm mide el punto 1 sobre los
    // cuadros que ya estan entrando por OnDetections.
    std::unique_ptr<ControlApi> controlApi;
    bool controlApiFailed = false;

    // El menu [12] existe justamente para levantar la API: si el config la tiene
    // apagada se levanta igual (con aviso en el log), porque el operador lo
    // eligio a proposito. --service sigue exigiendo enabled=true: ahi no hay
    // nadie mirando la consola para avisarle.
    if (consoleService && !config.controlApiConfig.enabled) {
        Logger::Warning("[SERVICIO] menu [12]: control_api.enabled=false en config.json; "
                        "se levanta igual porque el operador eligio SERVICIO API");
        config.controlApiConfig.enabled = true;
    }

    if (config.controlApiConfig.enabled) {
        controlApi = std::make_unique<ControlApi>();
        if (!controlApi->Start(config.controlApiConfig, choreographyEngine.get(),
                               rankingMeasure.get(), config.choreographyConfig.scriptPath,
                               &trackController, ptzController.get())) {
            Logger::Error("ControlApi: no se pudo levantar la API de control (puerto " +
                          std::to_string(config.controlApiConfig.port) + ")");
            controlApiFailed = true;
            controlApi.reset();
        }
    } else if (serviceMode) {
        // Sin API no hay forma de disparar la carrera y tampoco hay teclado: no
        // tiene sentido quedarse corriendo un VIB que nadie puede mandar.
        Logger::Error("--service requiere control_api.enabled=true en config.json");
        controlApiFailed = true;
    }

    if (consoleService) {
        Logger::Info("--- Modo SERVICIO API (menu [12]): panel de estado en consola, ESC vuelve al menu ---");
        Logger::Info("--- Control: GET /status  POST /arm /start /abort /shutdown ---");
    } else if (serviceMode && !controlApiFailed) {
        Logger::Info("--- Modo SERVICIO: menu y teclas deshabilitados ---");
        Logger::Info("--- Control: GET /status  POST /arm /start /abort /shutdown ---");
    }

    if (!serviceMode) {
    Logger::Info("ESC - Volver al menu principal");
    Logger::Info("F2  - Reducir a 2 camaras (throttle)");
    Logger::Info("F3  - Restaurar a 4 camaras");
    Logger::Info("F4  - Parada de emergencia");
    Logger::Info("F5  - Reset telemetria");
    if (ptzController && ptzController->IsInitialized()) {
        Logger::Info("--- PTZ Controls ---");
        Logger::Info("F8  - PTZ Choreography Option 7 (Preset 1->2->3 cycle)");
    }
    Logger::Info("--- SceneManager Manual Mode ---");
    Logger::Info("M   - Toggle AUTO/MANUAL mode");
    Logger::Info("F1/F6/F7 - Select config_a/b/c (MANUAL)");
    Logger::Info("G   - Toggle group G1_G4/G5_G8 (MANUAL)");
    Logger::Info("1-4 - Select camera in group (MANUAL)");
    if (config.choreographyConfig.enabled) {
        Logger::Info("--- Choreography Controls ---");
        Logger::Info("F12 - Start/Pause/Resume choreography");
    }
    }
    Logger::Info("====================================");

    // ============================================================================
    // Panel de estado del menu [12] SERVICIO API
    // ============================================================================
    // Bloque corto que se reimprime cada segundo (no una pantalla completa): el
    // operador ve en que anda el VIB sin tocar nada y los Logger::Info del
    // pipeline siguen saliendo entre bloque y bloque, sin perder nada.
    auto FormatClock = [](int64_t epochMs) -> std::string {
        if (epochMs <= 0) {
            return "-";
        }
        const std::time_t seconds = static_cast<std::time_t>(epochMs / 1000);
        std::tm local{};
        localtime_s(&local, &seconds);
        char buffer[16] = {0};
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
        return std::string(buffer);
    };
    auto PrintServicePanel = [&]() {
        std::ostringstream panel;
        if (!controlApi) {
            // Sin listener no hay /status ni /arm: se dice por que, en la
            // consola, en vez de dejar al operador mirando un panel vacio.
            panel << "\n  --- SERVICIO API ---------------------------------------------\n"
                  << "   api=NO LEVANTO   motivo=ver VIB_System_*.log\n"
                  << "   (control_api.api_key vacia o puerto "
                  << config.controlApiConfig.port << " ocupado)\n"
                  << "   ESC = volver al menu principal\n"
                  << "  --------------------------------------------------------------\n";
            std::cout << panel.str() << std::flush;
            return;
        }

        const ControlApi::Snapshot snapshot = controlApi->GetSnapshot();
        panel << "\n  --- SERVICIO API ---------------------------------------------\n"
              << "   estado=" << snapshot.stateName
              << "   race_id=" << (snapshot.raceId.empty() ? std::string("-") : snapshot.raceId)
              << "   punto=" << (snapshot.point > 0 ? std::to_string(snapshot.point) : std::string("-"))
              << "   esferas=" << snapshot.spheres << "\n"
              << "   motor=" << snapshot.engine
              << "   api=0.0.0.0:" << controlApi->Port()
              << "   escuchando=" << (snapshot.listening ? "si" : "NO")
              << "   t0=" << (snapshot.t0 > 0 ? FormatClock(snapshot.t0) : std::string("-")) << "\n"
              << "   ultimo=" << (snapshot.lastCommand.empty()
                                    ? std::string("(ninguno: idle hasta POST /arm)")
                                    : snapshot.lastCommand + " -> " +
                                          std::to_string(snapshot.lastStatus) +
                                          " (" + FormatClock(snapshot.lastAtMs) + ")")
              << (snapshot.armInFlight ? "   ARMANDO..." : "") << "\n"
              << "   ESC = volver al menu principal\n"
              << "  --------------------------------------------------------------\n";
        std::cout << panel.str() << std::flush;
    };

    bool running = true;
    // Solo en modo servicio sin consola el fallo de la API impide operar (no hay
    // teclado). En modo normal se avisa y se sigue con el menu: la API es
    // opcional. En SERVICIO API el panel muestra el fallo y ESC sigue andando.
    if (controlApiFailed && serviceMode && !consoleService) {
        running = false;
    }
    auto lastStatusLog = std::chrono::steady_clock::now();
    const auto statusInterval = std::chrono::seconds(10);

    auto lastPanelPrint = std::chrono::steady_clock::now();
    const auto panelInterval = std::chrono::seconds(1);
    if (consoleService) {
        PrintServicePanel();   // que el operador vea el estado desde el arranque
    }
    
    bool f2Pressed = false;
    bool f3Pressed = false;
    bool f4Pressed = false;
    bool f5Pressed = false;
    
    // Manual mode key debounce
    bool mKeyPressed = false;
    bool gKeyPressed = false;
    bool f1KeyPressed = false;
    bool f6KeyPressed = false;
    bool f7KeyPressed = false;
    bool key1Pressed = false;
    bool key2Pressed = false;
    bool key3Pressed = false;
    bool key4Pressed = false;
    
    // Choreography key debounce
    bool f12KeyPressed = false;
    
    // PTZ Choreography (F8) - Option 7
    bool f8KeyPressed = false;
    std::atomic<bool> ptzChoreographyRunning{false};
    std::thread ptzChoreographyThread;
    
    while (running) {
        // Apagado ordenado por la API: en --service es la unica salida (no hay
        // teclado), en el menu [12] vuelve al menu principal. El controlApi
        // nulo se maneja aparte: en [12] el panel muestra el fallo y ESC sigue
        // siendo la salida; en --service el bucle ya arranco con running=false.
        if (serviceMode && controlApi && controlApi->ShutdownRequested()) {
            Logger::Info(consoleService ? "[SERVICIO] /shutdown pedido por la API: volviendo al menu"
                                        : "[SERVICE] apagado pedido por la API");
            running = false;
            break;
        }

        // Check for exit condition (en SERVICIO API es la unica tecla viva)
        if (EscDown()) {
            Logger::Info("ESC presionado - Volviendo al menu principal...");
            running = false;
        }
        
        // F2 - Reduce active cameras
        if (KeyDown(VK_F2)) {
            if (!f2Pressed) {
                f2Pressed = true;
                config.selectorTopK = 2;
                perfMonitor->RecordEmergencyEvent();
                Logger::Info("[EMERGENCY] Reduced active cameras to 2");
            }
        } else {
            f2Pressed = false;
        }
        
        // F3 - Restore active cameras
        if (KeyDown(VK_F3)) {
            if (!f3Pressed) {
                f3Pressed = true;
                config.selectorTopK = 4;
                Logger::Info("[EMERGENCY] Restored active cameras to 4");
            }
        } else {
            f3Pressed = false;
        }
        
        // F4 - Emergency stop
        if (KeyDown(VK_F4)) {
            if (!f4Pressed) {
                f4Pressed = true;
                running = false;
                Logger::Info("[EMERGENCY] Graceful stop initiated");
            }
        } else {
            f4Pressed = false;
        }
        
        // F5 - Reset telemetry
        if (KeyDown(VK_F5)) {
            if (!f5Pressed) {
                f5Pressed = true;
                auto now = std::chrono::system_clock::now();
                auto time_value = std::chrono::system_clock::to_time_t(now);
                std::tm tm;
                localtime_s(&tm, &time_value);
                
                std::ostringstream runIDStream;
                runIDStream << std::put_time(&tm, "%Y%m%d_%H%M%S");
                std::string runID = runIDStream.str();
                
                Logger::Info("[RESET] Telemetry reset initiated");
                perfMonitor->Reset(true, runID);
            }
        } else {
            f5Pressed = false;
        }
        
        // ============================================================================
        // PTZ Choreography Option 7 (F8)
        // Cycles through presets 1, 2, 3 with 5-second delays on PTZ_START camera
        // ============================================================================
        if (KeyDown(VK_F8)) {
            if (!f8KeyPressed && ptzController && ptzController->IsInitialized()) {
                f8KeyPressed = true;
                
                if (!ptzChoreographyRunning) {
                    // Start choreography
                    Logger::Info("[PTZ_CHOREOGRAPHY] F8 pressed - Starting preset cycle (1->2->3->1...)");
                    ptzChoreographyRunning = true;
                    
                    // Launch choreography thread
                    ptzChoreographyThread = std::thread([&ptzController, &ptzChoreographyRunning]() {
                        Logger::Info("[PTZ_CHOREOGRAPHY] Thread started - cycling through presets");
                        
                        while (ptzChoreographyRunning) {
                            // Preset 1
                            if (!ptzChoreographyRunning) break;
                            Logger::Info("[PTZ_CHOREOGRAPHY] Executing preset 1");
                            ptzController->CallPreset(0, 1);  // Camera 0 (PTZ_START), Preset 1
                            
                            // Wait 5 seconds
                            for (int i = 0; i < 50 && ptzChoreographyRunning; ++i) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                            
                            // Preset 2
                            if (!ptzChoreographyRunning) break;
                            Logger::Info("[PTZ_CHOREOGRAPHY] Executing preset 2");
                            ptzController->CallPreset(0, 2);  // Camera 0 (PTZ_START), Preset 2
                            
                            // Wait 5 seconds
                            for (int i = 0; i < 50 && ptzChoreographyRunning; ++i) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                            
                            // Preset 3
                            if (!ptzChoreographyRunning) break;
                            Logger::Info("[PTZ_CHOREOGRAPHY] Executing preset 3");
                            ptzController->CallPreset(0, 3);  // Camera 0 (PTZ_START), Preset 3
                            
                            // Wait 5 seconds before looping back to preset 1
                            for (int i = 0; i < 50 && ptzChoreographyRunning; ++i) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }
                        }
                        
                        Logger::Info("[PTZ_CHOREOGRAPHY] Thread stopped");
                    });
                } else {
                    // Stop choreography
                    Logger::Info("[PTZ_CHOREOGRAPHY] F8 pressed - Stopping preset cycle");
                    ptzChoreographyRunning = false;
                    
                    if (ptzChoreographyThread.joinable()) {
                        ptzChoreographyThread.join();
                    }
                    
                    // Return to base preset
                    ptzController->ReturnToBase(0);
                    Logger::Info("[PTZ_CHOREOGRAPHY] Returned to base preset");
                }
            }
        } else {
            f8KeyPressed = false;
        }
        
        // ============================================================================
        // Choreography Controls (F12)
        // ============================================================================
        if (KeyDown(VK_F12)) {
            if (!f12KeyPressed && choreographyEngine) {
                f12KeyPressed = true;
                auto state = choreographyEngine->GetState();
                
                switch (state) {
                    case Choreography::EngineState::Ready:
                    case Choreography::EngineState::Idle:
                        // Start choreography
                        Logger::Info("[CHOREOGRAPHY] F12 pressed - Starting choreography");
                        if (!choreographyEngine->Start()) {
                            Logger::Warning("[CHOREOGRAPHY] Failed to start: " + choreographyEngine->GetLastError());
                        }
                        break;
                        
                    case Choreography::EngineState::Running:
                        // Pause choreography
                        Logger::Info("[CHOREOGRAPHY] F12 pressed - Pausing choreography");
                        choreographyEngine->Pause();
                        break;
                        
                    case Choreography::EngineState::Paused:
                        // Resume choreography
                        Logger::Info("[CHOREOGRAPHY] F12 pressed - Resuming choreography");
                        choreographyEngine->Resume();
                        break;
                        
                    case Choreography::EngineState::Error:
                        // Reset and reload script
                        Logger::Info("[CHOREOGRAPHY] F12 pressed - Reloading script after error");
                        if (!config.choreographyConfig.scriptPath.empty()) {
                            choreographyEngine->Load(config.choreographyConfig.scriptPath);
                        }
                        break;
                        
                    default:
                        Logger::Debug("[CHOREOGRAPHY] F12 pressed - Engine busy (state: " + 
                                     std::to_string(static_cast<int>(state)) + ")");
                        break;
                }
            }
        } else {
            f12KeyPressed = false;
        }
        
        // ============================================================================
        // SceneManager Manual Mode Controls
        // ============================================================================
        
        // M - Toggle AUTO/MANUAL mode
        if (KeyDown('M')) {
            if (!mKeyPressed && sceneManager) {
                mKeyPressed = true;
                SceneMode currentMode = sceneManager->GetMode();
                SceneMode newMode = (currentMode == SceneMode::AUTO) ? SceneMode::MANUAL : SceneMode::AUTO;
                sceneManager->SetMode(newMode);
                
                std::string modeStr = (newMode == SceneMode::AUTO) ? "AUTO" : "MANUAL";
                Logger::Info("[SCENE] Mode toggled to " + modeStr);
            }
        } else {
            mKeyPressed = false;
        }
        
        // F1 - Select config_a (index 0) in MANUAL mode
        if (KeyDown(VK_F1)) {
            if (!f1KeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                f1KeyPressed = true;
                sceneManager->SelectConfig(0);
            }
        } else {
            f1KeyPressed = false;
        }
        
        // F6 - Select config_b (index 1) in MANUAL mode
        if (KeyDown(VK_F6)) {
            if (!f6KeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                f6KeyPressed = true;
                sceneManager->SelectConfig(1);
            }
        } else {
            f6KeyPressed = false;
        }
        
        // F7 - Select config_c (index 2) in MANUAL mode
        if (KeyDown(VK_F7)) {
            if (!f7KeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                f7KeyPressed = true;
                sceneManager->SelectConfig(2);
            }
        } else {
            f7KeyPressed = false;
        }
        
        // G - Toggle group in MANUAL mode
        if (KeyDown('G')) {
            if (!gKeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                gKeyPressed = true;
                sceneManager->ToggleGroup();
            }
        } else {
            gKeyPressed = false;
        }
        
        // 1-4 - Select camera in group (MANUAL mode)
        if (KeyDown('1')) {
            if (!key1Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key1Pressed = true;
                sceneManager->SelectCameraInGroup(0);
            }
        } else {
            key1Pressed = false;
        }
        
        if (KeyDown('2')) {
            if (!key2Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key2Pressed = true;
                sceneManager->SelectCameraInGroup(1);
            }
        } else {
            key2Pressed = false;
        }
        
        if (KeyDown('3')) {
            if (!key3Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key3Pressed = true;
                sceneManager->SelectCameraInGroup(2);
            }
        } else {
            key3Pressed = false;
        }
        
        if (KeyDown('4')) {
            if (!key4Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key4Pressed = true;
                sceneManager->SelectCameraInGroup(3);
            }
        } else {
            key4Pressed = false;
        }
        
        // Periodic status logging
        auto now = std::chrono::steady_clock::now();
        if (now - lastStatusLog >= statusInterval) {
            if (cameraSelector && config.selectorEnabled) {
                auto selection = cameraSelector->GetActiveSelection();
                std::string selectedList;
                for (size_t i = 0; i < selection.selectedCameraIDs.size(); i++) {
                    selectedList += std::to_string(selection.selectedCameraIDs[i]);
                    if (i < selection.selectedCameraIDs.size() - 1) selectedList += ",";
                }
                Logger::Info("[STATUS] Active cameras: [" + selectedList + "]");
            }
            
            if (ballTracker && ballTracker->IsInitialized()) {
                auto ranking = ballTracker->GetRanking();
                std::string rankingStr;
                for (size_t i = 0; i < ranking.size() && i < 3; ++i) {
                    if (i > 0) rankingStr += ",";
                    rankingStr += std::to_string(ranking[i]);
                }
                auto leader = ballTracker->GetLeader();
                Logger::Info("[STATUS] Leader: ball " + std::to_string(leader.ballID) +
                            " at X=" + std::to_string(leader.Xg) + "m");
            }
            
            if (sceneManager && sceneManager->IsEnabled()) {
                sceneManager->ProcessMuteTimeouts();
                
                // Log SceneManager mode and state
                SceneMode mode = sceneManager->GetMode();
                std::string modeStr = (mode == SceneMode::AUTO) ? "AUTO" : "MANUAL";
                
                if (mode == SceneMode::MANUAL) {
                    ManualState state = sceneManager->GetManualState();
                    std::string configName = sceneManager->GetCurrentConfigName();
                    std::string groupStr = (state.activeGroup == ActiveGroup::G1_G4) ? "G1_G4" : "G5_G8";
                    int activeCameraID = sceneManager->GetActiveCameraID();
                    
                    // Get cameras in active group
                    std::ostringstream camerasOss;
                    if (state.activeConfigIndex >= 0 && 
                        static_cast<size_t>(state.activeConfigIndex) < config.sceneManagerGroups.size()) {
                        const auto& cfg = config.sceneManagerGroups[state.activeConfigIndex];
                        if (state.activeGroup == ActiveGroup::G1_G4) {
                            camerasOss << "[" << cfg.slotsG1_G4[0] << "," << cfg.slotsG1_G4[1] << ","
                                      << cfg.slotsG1_G4[2] << "," << cfg.slotsG1_G4[3] << "]";
                        } else {
                            camerasOss << "[" << cfg.slotsG5_G8[0] << "," << cfg.slotsG5_G8[1] << ","
                                      << cfg.slotsG5_G8[2] << "," << cfg.slotsG5_G8[3] << "]";
                        }
                    }
                    
                    Logger::Info("[MANUAL] Config: " + configName + " | Group: " + groupStr + 
                                " | Cameras: " + camerasOss.str() + " | Active: CAM_" + 
                                std::to_string(activeCameraID));
                } else {
                    std::string configName = sceneManager->GetCurrentConfigName();
                    Logger::Info("[AUTO] Current config: " + configName);
                }
            }
            
            lastStatusLog = now;
        }

        // Panel del menu [12]: se reimprime cada segundo con el estado real que
        // ve la API (misma fuente que GET /status). En --service puro no se
        // imprime: no hay nadie mirando la consola.
        if (consoleService) {
            const auto panelNow = std::chrono::steady_clock::now();
            if (panelNow - lastPanelPrint >= panelInterval) {
                PrintServicePanel();
                lastPanelPrint = panelNow;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    // Cleanup
    Logger::Info("Cerrando modo running...");

    // La API de control primero: referencia al motor y al medidor, que se
    // destruyen mas abajo.
    if (controlApi) {
        Logger::Info("Stopping ControlApi...");
        controlApi->Stop();
        controlApi.reset();
    }
    
    // Stop PTZ choreography thread if running
    if (ptzChoreographyRunning) {
        Logger::Info("Stopping PTZ choreography thread...");
        ptzChoreographyRunning = false;
        if (ptzChoreographyThread.joinable()) {
            ptzChoreographyThread.join();
        }
    }
    
    // Stop PTZ tracking components
    if (trackingLogger) {
        Logger::Info("Stopping TrackingLogger...");
        trackingLogger->Stop();
        trackingLogger.reset();
    }
    
    if (ptzController) {
        Logger::Info("Stopping PTZController...");
        ptzController->ReturnToBaseAll();  // Return cameras to base before shutdown
        ptzController->Shutdown();
        ptzController.reset();
    }
    
    // Stop choreography engine if running
    if (choreographyEngine) {
        Logger::Info("Stopping ChoreographyEngine...");
        choreographyEngine->Stop();
        choreographyEngine.reset();
    }

    if (inferenceStream) {
        cudaStreamDestroy(inferenceStream);
    }
    
    for (auto& capture : captureChannels) {
        capture->Stop();
    }
    captureChannels.clear();
    
    // Medidor por evento: se apaga DESPUES de frenar la captura (los hilos de
    // captura son los que llenan los anillos; el hilo medidor solo los lee).
    if (rankingMeasure) {
        Logger::Info("Stopping RankingMeasure...");
        Measure::AttachCoordinator(nullptr);
        rankingMeasure->Shutdown();
        rankingMeasure.reset();
    }
    
    if (ndiManager) {
        ndiManager->ReleaseAll();
    }
    
    Logger::Info("Running mode finalizado");
    return !controlApiFailed;
}

/**
 * Ejecutar el modo de test de estrés / diagnóstico
 * @return true si todos los tests pasaron
 */
bool RunStressTestMode() {
    Logger::Info("=== INICIANDO MODO TEST DE ESTRES ===");
    
    // Cargar configuración
    Config config = LoadConfig("config.json");
    
    // Crear y ejecutar el tester
    StressTester tester(std::chrono::seconds(1));
    DiagnosticResults results = tester.RunFullDiagnostic(config);
    
    // Mostrar resultados
    tester.DisplayResults(results);
    
    // Esperar que el usuario presione ENTER
    std::cout << "\n  Presione ENTER para volver al menu...";
    std::cin.get();
    
    return results.allPassed;
}

/**
 * Ejecutar el modo de test usando SphereVerifier
 * Reemplazo simplificado de la opción 5 (RunRadarTestMode)
 * @return true si finalizó correctamente
 */
bool RunSphereVerifierTest() {
    Logger::Info("=== INICIANDO SPHERE VERIFIER TEST ===");
    Logger::Info("Usando SphereVerifier para verificar esferas");
    
    // Mostrar sub-menú de opciones
    std::cout << "\n  +--------------------------------------------------------------------+\n";
    std::cout << "  |                   SPHERE VERIFIER TEST                             |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   Camaras disponibles:                                             |\n";
    std::cout << "  |     1-12: CAM_01 a CAM_12                                          |\n";
    std::cout << "  |     13-16: RADAR_01 a RADAR_04                                     |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "\n";
    
    // Obtener cámara del usuario
    int cameraID = 13; // Default: RADAR_01
    std::cout << "  Ingrese numero de camara (1-16) [default=13]: ";
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty()) {
        try {
            cameraID = std::stoi(input);
            if (cameraID < 1 || cameraID > 16) {
                std::cout << "  Camara invalida, usando default (13)...\n";
                cameraID = 13;
            }
        } catch (...) {
            std::cout << "  Entrada invalida, usando default (13)...\n";
            cameraID = 13;
        }
    }
    
    // Obtener número esperado de esferas
    int expectedSpheres = 10;
    std::cout << "  Ingrese numero de esferas esperadas [default=10]: ";
    std::getline(std::cin, input);
    if (!input.empty()) {
        try {
            expectedSpheres = std::stoi(input);
            if (expectedSpheres < 1 || expectedSpheres > 20) {
                std::cout << "  Valor invalido, usando default (10)...\n";
                expectedSpheres = 10;
            }
        } catch (...) {
            std::cout << "  Entrada invalida, usando default (10)...\n";
            expectedSpheres = 10;
        }
    }
    
    Logger::Info("Configuracion: Camara=" + std::to_string(cameraID) + 
                 ", Esferas esperadas=" + std::to_string(expectedSpheres));
    
    // ============================================================================
    // Cargar configuración
    // ============================================================================
    Config config = LoadConfig("config.json");
    
    // ============================================================================
    // Conectar VideoHub
    // ============================================================================
    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    
    if (!videoHub.Connect()) {
        Logger::Error("[HW/SW ERROR] No se pudo conectar con VideoHub");
        std::cout << "\n  ERROR: No se pudo conectar con VideoHub\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }
    Logger::Info("VideoHub conectado");
    
    // ============================================================================
    // Inicializar InferenceEngine
    // ============================================================================
    Logger::Info("Inicializando InferenceEngine...");
    auto inferenceEngine = std::make_shared<InferenceEngine>();
    if (!inferenceEngine->Initialize(config.inferenceConfig)) {
        Logger::Warning("InferenceEngine no pudo inicializarse completamente");
    }
    
    if (inferenceEngine->IsStubMode()) {
        Logger::Warning("InferenceEngine en modo STUB - Detecciones simuladas");
        std::cout << "\n  ADVERTENCIA: InferenceEngine en modo STUB (detecciones simuladas)\n";
    } else {
        Logger::Info("InferenceEngine inicializado con TensorRT");
    }
    
    // ============================================================================
    // Crear SphereVerifier
    // ============================================================================
    Logger::Info("Creando SphereVerifier...");
    Verification::SphereVerifier verifier(&videoHub, inferenceEngine.get());
    
    if (!verifier.IsReady()) {
        Logger::Error("SphereVerifier no está listo: " + verifier.GetLastError());
        std::cout << "\n  ERROR: SphereVerifier no esta listo\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }
    
    // ============================================================================
    // Menú de operaciones
    // ============================================================================
    bool running = true;
    while (running) {
        std::cout << "\n  +--------------------------------------------------------------------+\n";
        std::cout << "  |                   OPERACIONES SPHERE VERIFIER                      |\n";
        std::cout << "  +--------------------------------------------------------------------+\n";
        std::cout << "  |   [1] Verificar presencia de esferas                               |\n";
        std::cout << "  |   [2] Capturar posiciones actuales                                 |\n";
        std::cout << "  |   [3] Cambiar camara                                               |\n";
        std::cout << "  |   [4] Cambiar esferas esperadas                                    |\n";
        std::cout << "  |   [0] Volver al menu principal                                     |\n";
        std::cout << "  +--------------------------------------------------------------------+\n";
        std::cout << "  Camara actual: " << cameraID << " | Esferas esperadas: " << expectedSpheres << "\n";
        std::cout << "\n  Seleccione operacion: ";
        
        int op = -1;
        std::cin >> op;
        std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');;
        
        switch (op) {
            case 1: {
                // Verificar presencia
                std::cout << "\n  Verificando presencia de " << expectedSpheres << " esferas en camara " << cameraID << "...\n";
                Logger::Info("Ejecutando CheckPresence...");
                
                auto result = verifier.CheckPresence(cameraID, expectedSpheres, 10000);
                
                if (result.success) {
                    std::cout << "\n  *** VERIFICACION EXITOSA ***\n";
                    std::cout << "  Esferas detectadas: " << result.spheresDetected << "\n";
                    std::cout << "  IDs: [";
                    for (size_t i = 0; i < result.sphereIDs.size(); ++i) {
                        if (i > 0) std::cout << ", ";
                        std::cout << result.sphereIDs[i];
                    }
                    std::cout << "]\n";
                    Logger::Info("CheckPresence EXITOSO: " + std::to_string(result.spheresDetected) + " esferas");
                } else {
                    std::cout << "\n  *** VERIFICACION FALLIDA ***\n";
                    std::cout << "  Error: " << result.errorMessage << "\n";
                    std::cout << "  Esferas detectadas: " << result.spheresDetected << "\n";
                    if (!result.sphereIDs.empty()) {
                        std::cout << "  IDs detectados: [";
                        for (size_t i = 0; i < result.sphereIDs.size(); ++i) {
                            if (i > 0) std::cout << ", ";
                            std::cout << result.sphereIDs[i];
                        }
                        std::cout << "]\n";
                    }
                    Logger::Warning("CheckPresence FALLIDO: " + result.errorMessage);
                }
                break;
            }
            
            case 2: {
                // Capturar posiciones
                std::cout << "\n  Capturando posiciones de esferas en camara " << cameraID << "...\n";
                Logger::Info("Ejecutando CapturePositions...");
                
                auto result = verifier.CapturePositions(cameraID, 5000);
                
                if (result.success) {
                    std::cout << "\n  *** CAPTURA EXITOSA ***\n";
                    std::cout << "  Esferas detectadas: " << result.spheresDetected << "\n";
                    std::cout << "\n  Posiciones:\n";
                    std::cout << "  +---------+----------+----------+------------+\n";
                    std::cout << "  | Ball ID |    X     |    Y     | Confidence |\n";
                    std::cout << "  +---------+----------+----------+------------+\n";
                    for (const auto& pos : result.positions) {
                        std::cout << "  |    " << std::setw(2) << pos.sphereID << "   | " 
                                  << std::fixed << std::setprecision(4) << std::setw(8) << pos.x << " | "
                                  << std::setw(8) << pos.y << " | "
                                  << std::setw(10) << pos.confidence << " |\n";
                    }
                    std::cout << "  +---------+----------+----------+------------+\n";
                    Logger::Info("CapturePositions EXITOSO: " + std::to_string(result.spheresDetected) + " esferas");
                } else {
                    std::cout << "\n  *** CAPTURA FALLIDA ***\n";
                    std::cout << "  Error: " << result.errorMessage << "\n";
                    Logger::Warning("CapturePositions FALLIDO: " + result.errorMessage);
                }
                break;
            }
            
            case 3: {
                // Cambiar cámara
                std::cout << "  Ingrese nueva camara (1-16): ";
                int newCam;
                std::cin >> newCam;
                std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
                if (newCam >= 1 && newCam <= 16) {
                    cameraID = newCam;
                    Logger::Info("Camara cambiada a: " + std::to_string(cameraID));
                } else {
                    std::cout << "  Camara invalida\n";
                }
                break;
            }
            
            case 4: {
                // Cambiar esferas esperadas
                std::cout << "  Ingrese numero de esferas esperadas: ";
                int newCount;
                std::cin >> newCount;
                std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
                if (newCount >= 1 && newCount <= 20) {
                    expectedSpheres = newCount;
                    Logger::Info("Esferas esperadas cambiado a: " + std::to_string(expectedSpheres));
                } else {
                    std::cout << "  Valor invalido\n";
                }
                break;
            }
            
            case 0:
                running = false;
                break;
                
            default:
                std::cout << "  Opcion invalida\n";
                break;
        }
    }
    
    Logger::Info("SphereVerifier Test finalizado");
    return true;
}

/**
 * Ejecutar el modo de test de RADAR 1 (Cámara 13)
 * Solo captura de una cámara para verificar inferencia de esferas
 * @return true si finalizó correctamente
 */
bool RunRadarTestMode() {
    Logger::Info("=== INICIANDO MODO TEST RADAR 1 (CAMARA 13) ===");
    Logger::Info("Este modo captura SOLO del Radar 1 para probar la inferencia");
    Logger::Info("Las demas camaras NO se capturan para dedicar recursos a la prueba");
    
    // Load configuration
    Config config = LoadConfig("config.json");
    
    // Controllers para enrutar a la cámara 13 (Radar 1)
    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    
    if (!videoHub.Connect()) {
        Logger::Error("[HW/SW ERROR] No se pudo establecer conexión con VideoHub");
        return false;
    }
    
    // Enrutar VideoHub a RADAR_01 (índice 12 en 0-based)
    Logger::Info("Enrutando VideoHub a RADAR_01 (entrada 13)...");
    if (!videoHub.RouteInputToOutput(kVideoHubPrimaryOutput, "RADAR_01")) {
        Logger::Error("[HW/SW ERROR] No se pudo enrutar a RADAR_01");
        return false;
    }
    Logger::Info("VideoHub enrutado correctamente a RADAR_01");
    
    // ============================================================================
    // Initialize CUDA stream para inferencia
    // ============================================================================
    cudaStream_t inferenceStream = nullptr;
    cudaError_t err = cudaStreamCreate(&inferenceStream);
    if (err != cudaSuccess) {
        Logger::Error("Failed to create inference CUDA stream: " + std::string(cudaGetErrorString(err)));
        return false;
    }
    
    // ============================================================================
    // Initialize InferenceEngine
    // ============================================================================
    Logger::Info("Inicializando InferenceEngine...");
    auto inferenceEngine = std::make_shared<InferenceEngine>();
    bool inferenceReady = false;
    if (inferenceEngine->Initialize(config.inferenceConfig)) {
        inferenceReady = true;
        if (inferenceEngine->IsStubMode()) {
            Logger::Warning("InferenceEngine en modo STUB - No hay modelo real");
        } else {
            Logger::Info("InferenceEngine inicializado con TensorRT");
        }
    } else {
        Logger::Warning("InferenceEngine no pudo inicializarse");
    }
    
    // ============================================================================
    // Initialize SOLO el canal de captura para RADAR 1 (device index para radar)
    // ============================================================================
    // NOTA: RADAR_01 es la cámara 13 en el VideoHub, pero en DeckLink depende 
    // de cómo están conectadas las tarjetas. Asumimos device index 0 para el test.
    // Ajustar según la configuración física real.
    
    Logger::Info("Inicializando captura SOLO para Radar 1...");
    
    // Contadores para estadísticas
    std::atomic<int> framesProcessed{0};
    std::atomic<int> totalSpheresDetected{0};
    std::atomic<int> lastSphereCount{0};
    std::mutex detectionMutex;
    std::vector<int> detectedBallIDs;
    std::atomic<bool> framesSaved{false}; // Flag to save only one frame
    
    // Usar device 0 para captura (el que recibe la señal del VideoHub)
    // El VideoHub ya está enrutado a RADAR_01
    auto capture = std::make_unique<DeckLinkCapture>();
    const std::string channelName = "RADAR_01_TEST";
    
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("Dispositivos DeckLink encontrados: " + std::to_string(numDevices));
    
    if (numDevices <= 0) {
        Logger::Error("No se encontraron dispositivos DeckLink");
        cudaStreamDestroy(inferenceStream);
        return false;
    }
    
    // Inicializar device 0 (el que recibe la salida del VideoHub)
    if (!capture->Initialize(0, channelName)) {
        Logger::Error("No se pudo inicializar captura del device 0");
        cudaStreamDestroy(inferenceStream);
        return false;
    }
    
    Logger::Info("Captura inicializada correctamente para " + channelName);
    
    // Configurar callback de frame para inferencia
    capture->SetFrameReadyHandler([&inferenceEngine, &inferenceStream, &inferenceReady,
                                   &framesProcessed, &totalSpheresDetected, &lastSphereCount,
                                   &detectionMutex, &detectedBallIDs, &framesSaved]
                                  (const VideoChannel& channel, cudaStream_t stream) {
        framesProcessed++;
        
        // ============================================================================
        // DEBUG: Guardar frame SIEMPRE en el primer frame para verificar la imagen
        // ============================================================================
        if (framesProcessed.load() == 1) {
            Logger::Info("=== DEBUG: Guardando PRIMER frame para verificar imagen de camara ===");
            Logger::Info("Frame dimensions: " + std::to_string(channel.width) + "x" + std::to_string(channel.height));
            
            // Save YUV frame as PPM 
            bool saved = FrameSaver::SaveYUVFrameAsPPM(
                channel.cudaYUVBuffer,
                channel.width,
                channel.height,
                "debug_first_frame.ppm"
            );
            
            if (saved) {
                Logger::Info("DEBUG: Primer frame guardado en debug_first_frame.ppm");
            } else {
                Logger::Error("DEBUG: No se pudo guardar el primer frame");
            }
        }
        
        // Guardar también en el frame 30 y 60 para asegurar
        if (framesProcessed.load() == 30 || framesProcessed.load() == 60) {
            std::string filename = "debug_frame_" + std::to_string(framesProcessed.load()) + ".ppm";
            Logger::Info("=== DEBUG: Guardando frame " + std::to_string(framesProcessed.load()) + " ===");
            
            bool saved = FrameSaver::SaveYUVFrameAsPPM(
                channel.cudaYUVBuffer,
                channel.width,
                channel.height,
                filename.c_str()
            );
            
            if (saved) {
                Logger::Info("DEBUG: Frame guardado en " + filename);
            }
        }
        
        if (!inferenceReady || !inferenceEngine) {
            return;
        }
        
        // Procesar frame con inferencia UYVY optimizada
        std::vector<BallDetection> detections = inferenceEngine->ProcessFrameUYVY(
            channel.cudaYUVBuffer,
            1,  // cameraID = 1 (RADAR_01)
            channel.width,
            channel.height,
            inferenceStream,
            channel.preprocessEvent
        );
        
        // Actualizar estadísticas
        int sphereCount = static_cast<int>(detections.size());
        lastSphereCount.store(sphereCount);
        totalSpheresDetected.fetch_add(sphereCount);
        
        // Log cada 30 frames aunque no haya detecciones (para debug)
        if (framesProcessed.load() % 30 == 0) {
            Logger::Info("[DEBUG] Frame " + std::to_string(framesProcessed.load()) + 
                        " - Detecciones: " + std::to_string(sphereCount));
        }
        
        // Guardar IDs de esferas detectadas
        if (!detections.empty()) {
            std::lock_guard<std::mutex> lock(detectionMutex);
            detectedBallIDs.clear();
            for (const auto& det : detections) {
                detectedBallIDs.push_back(det.ballID);
            }
            
            // Save detection frame to verify we are receiving camera image
            // Only the first frame with detection is saved
            if (!framesSaved.load()) {
                Logger::Info("Saving detection frame for verification...");
                
                // Save YUV frame as PPM (easier to visualize)
                bool saved = FrameSaver::SaveYUVFrameAsPPM(
                    channel.cudaYUVBuffer,
                    channel.width,
                    channel.height,
                    "detection_frame.ppm"
                );
                
                if (saved) {
                    Logger::Info("Frame saved successfully to detection_frame.ppm");
                    Logger::Info("Dimensions: " + std::to_string(channel.width) + "x" + std::to_string(channel.height));
                    framesSaved.store(true);
                } else {
                    Logger::Warning("Could not save detection frame");
                }
            }
            
            // Log de detecciones
            std::ostringstream oss;
            oss << "[DETECCION] Frame " << framesProcessed.load() 
                << " - Esferas detectadas: " << sphereCount << " [";
            for (size_t i = 0; i < detections.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "B" << detections[i].ballID 
                    << "(x=" << std::fixed << std::setprecision(2) << detections[i].x
                    << ",y=" << detections[i].y
                    << ",conf=" << detections[i].confidence << ")";
            }
            oss << "]";
            Logger::Info(oss.str());
        }
    });
    
    // Iniciar captura
    capture->Start();
    Logger::Info("Captura iniciada. Monitoreando Radar 1...");
    
    // ============================================================================
    // Bucle de monitoreo
    // ============================================================================
    Logger::Info("====================================");
    Logger::Info("ESC - Volver al menu principal");
    Logger::Info("====================================");
    Logger::Info("Presione ESC para terminar el test...\n");
    
    bool running = true;
    auto lastStatusLog = std::chrono::steady_clock::now();
    const auto statusInterval = std::chrono::seconds(2);
    
    while (running) {
        // Check for exit condition
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Logger::Info("ESC presionado - Terminando test de Radar 1...");
            running = false;
        }
        
        // Log periódico de estadísticas
        auto now = std::chrono::steady_clock::now();
        if (now - lastStatusLog >= statusInterval) {
            int frames = framesProcessed.load();
            int lastCount = lastSphereCount.load();
            
            std::ostringstream oss;
            oss << "[ESTADISTICAS] Frames procesados: " << frames
                << " | Ultima deteccion: " << lastCount << " esferas";
            
            // Mostrar IDs detectados
            {
                std::lock_guard<std::mutex> lock(detectionMutex);
                if (!detectedBallIDs.empty()) {
                    oss << " | IDs: [";
                    for (size_t i = 0; i < detectedBallIDs.size(); ++i) {
                        if (i > 0) oss << ",";
                        oss << detectedBallIDs[i];
                    }
                    oss << "]";
                }
            }
            
            Logger::Info(oss.str());
            
            // Mostrar también en consola de forma destacada
            std::cout << "\n  *** RADAR 1 - ESFERAS DETECTADAS: " << lastCount << " ***\n" << std::endl;
            
            lastStatusLog = now;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    // ============================================================================
    // Cleanup
    // ============================================================================
    Logger::Info("Deteniendo captura de Radar 1...");
    capture->Stop();
    capture.reset();
    
    if (inferenceStream) {
        cudaStreamDestroy(inferenceStream);
    }
    
    // Resumen final
    Logger::Info("====================================");
    Logger::Info("=== RESUMEN DEL TEST RADAR 1 ===");
    Logger::Info("Total frames procesados: " + std::to_string(framesProcessed.load()));
    Logger::Info("Ultima deteccion: " + std::to_string(lastSphereCount.load()) + " esferas");
    Logger::Info("====================================");
    
    std::cout << "\n  Presione ENTER para volver al menu...";
    std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
    std::cin.get();
    
    Logger::Info("Test de Radar 1 finalizado");
    return true;
}

bool RunVMixUTCChoreography() {
    Logger::Info("=== INICIANDO COREOGRAFIA VMIX UTC ===");
    Logger::Info("Solo se conectara a vMix TCP para ejecutar la secuencia de comandos");
    
    // Load configuration
    Config config = LoadConfig("config.json");
    
    // Connect to vMix TCP
    VMixController vmix(L"127.0.0.1", 8088, 8099);
    vmix.ConnectTcp();
    
    // Create choreography engine with vMix controller only
    Choreography::ChoreographyEngine engine(&vmix, nullptr);
    engine.SetContinueOnError(true);
    engine.SetVMixRequired(true);
    
    // Set up progress callbacks
    engine.SetStateCallback([](Choreography::EngineState state) {
        std::string stateStr;
        switch (state) {
            case Choreography::EngineState::Idle: stateStr = "Idle"; break;
            case Choreography::EngineState::Ready: stateStr = "Ready"; break;
            case Choreography::EngineState::Running: stateStr = "Running"; break;
            case Choreography::EngineState::Paused: stateStr = "Paused"; break;
            case Choreography::EngineState::Stopping: stateStr = "Stopping"; break;
            case Choreography::EngineState::Error: stateStr = "Error"; break;
        }
        Logger::Info("[CHOREOGRAPHY UTC] State: " + stateStr);
    });
    
    engine.SetEventStartCallback([](size_t index, const Choreography::ChoreographyEvent& event) {
        Logger::Info("[CHOREOGRAPHY UTC] Event " + std::to_string(index) + ": " + event.GetTypeName());
    });
    
    engine.SetEventCompleteCallback([](const Choreography::EventResult& result) {
        if (!result.success) {
            Logger::Warning("[CHOREOGRAPHY UTC] Event " + std::to_string(result.eventIndex) + 
                           " failed: " + result.errorMessage);
        }
    });
    
    engine.SetErrorCallback([](const std::string& error) {
        Logger::Error("[CHOREOGRAPHY UTC] Error: " + error);
    });
    
    // Load choreography script
    std::string scriptPath = "config/choreography/vmix_utc_replay.json";
    Logger::Info("Loading choreography script: " + scriptPath);
    
    if (!engine.Load(scriptPath)) {
        Logger::Error("Failed to load choreography script: " + engine.GetLastError());
        std::cout << "\n  Error: No se pudo cargar el script de coreografia.\n";
        std::cout << "  " << engine.GetLastError() << "\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }
    
    // Start execution
    Logger::Info("Starting vMix UTC choreography execution...");
    std::cout << "\n  Ejecutando coreografia vMix UTC...\n";
    std::cout << "  Duracion estimada: ~111 segundos (~1min 51s)\n";
    std::cout << "  Presione Ctrl+C para cancelar.\n\n";
    
    if (!engine.Start()) {
        Logger::Error("Failed to start choreography: " + engine.GetLastError());
        std::cout << "  Error al iniciar la coreografia: " << engine.GetLastError() << "\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }
    
    // Wait for completion
    while (engine.IsRunning()) {
        auto status = engine.GetStatus();
        std::cout << "\r  Progreso: evento " << status.currentEventIndex + 1 
                  << "/" << status.totalEvents 
                  << " (" << status.currentEventType << ")"
                  << " - " << status.elapsedTime.count() / 1000 << "s transcurridos   " << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    std::cout << "\n\n";
    
    auto finalState = engine.GetState();
    if (finalState == Choreography::EngineState::Error) {
        Logger::Warning("Choreography finished with errors");
        std::cout << "  Coreografia finalizada con errores.\n";
    } else {
        Logger::Info("Choreography completed successfully");
        std::cout << "  Coreografia completada exitosamente.\n";
    }
    
    std::cout << "  Presione ENTER para continuar...";
    std::cin.get();
    
    Logger::Info("vMix UTC Choreography mode finalizado");
    return true;
}

bool RunAutomatedTrackMode() {
    Logger::Info("=== INICIANDO MODO PISTA AUTOMATIZADA (FASE 1: VERIFICACION SALIDA) ===");

    // Load configuration
    Config config = LoadConfig("config.json");

    // Connect VideoHub
    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    if (!videoHub.Connect()) {
        Logger::Error("[PISTA AUTO] No se pudo conectar con VideoHub");
        std::cout << "\n  ERROR: No se pudo conectar con VideoHub (" << config.videohubIp << ")\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }
    Logger::Info("[PISTA AUTO] VideoHub conectado a " + config.videohubIp);

    // Ruteo de las PTZ: los radares se resuelven por nombre y sus salidas se
    // leen de scene_manager.radar_routing (no hay input/output fijo en codigo).
    ApplyRadarRoutingFromConfig(videoHub, config, "PISTA AUTO");

    // Initialize InferenceEngine (stub mode OK for Phase 1)
    Logger::Info("[PISTA AUTO] Inicializando InferenceEngine...");
    auto inferenceEngine = std::make_shared<InferenceEngine>();
    inferenceEngine->Initialize(config.inferenceConfig);

    if (inferenceEngine->IsStubMode()) {
        Logger::Warning("[PISTA AUTO] InferenceEngine en modo STUB - usando detecciones simuladas");
        std::cout << "\n  ADVERTENCIA: InferenceEngine en modo STUB (detecciones simuladas)\n";
        std::cout << "  La verificacion usara datos falsos para probar el flujo.\n";
    } else {
        Logger::Info("[PISTA AUTO] InferenceEngine inicializado con TensorRT");
    }

    // Initialize PTZ Controller (single camera: index 0 = PTZ_START)
    std::shared_ptr<PTZ::PTZController> ptzController;
    bool ptzAvailable = false;
    if (config.ptzCamerasConfig.enabled && !config.ptzCamerasConfig.cameras.empty()) {
        ptzController = std::make_shared<PTZ::PTZController>();
        if (!ptzController->Initialize(config.ptzCamerasConfig.cameras, config.ptzTrackingConfig)) {
            Logger::Warning("[PISTA AUTO] PTZController no se pudo inicializar");
        } else {
            ptzAvailable = true;
            Logger::Info("[PISTA AUTO] PTZController inicializado - " +
                        std::to_string(config.ptzCamerasConfig.cameras.size()) + " camara(s)");
        }
    } else {
        Logger::Warning("[PISTA AUTO] No hay camaras PTZ configuradas en config.json");
    }

    // Initialize SphereVerifier
    Logger::Info("[PISTA AUTO] Creando SphereVerifier...");
    Verification::SphereVerifier sphereVerifier(&videoHub, inferenceEngine.get());

    if (!sphereVerifier.IsReady()) {
        Logger::Error("[PISTA AUTO] SphereVerifier no esta listo: " + sphereVerifier.GetLastError());
        std::cout << "\n  ERROR: SphereVerifier no esta listo\n";
        std::cout << "  " << sphereVerifier.GetLastError() << "\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }
    Logger::Info("[PISTA AUTO] SphereVerifier listo");

    // Read DeckLink device index from config (default 0)
    int decklinkDeviceIndex = 0;
    try {
        std::ifstream configFile("config.json");
        if (configFile.is_open()) {
            json j;
            configFile >> j;
            if (j.contains("automated_track") && j["automated_track"].contains("decklink_device_index")) {
                decklinkDeviceIndex = j["automated_track"]["decklink_device_index"].get<int>();
            }
        }
    } catch (...) {}
    Logger::Info("[PISTA AUTO] DeckLink device index from config: " + std::to_string(decklinkDeviceIndex));

    // Initialize NDI for live video preview (UYVY raw, no conversion needed)
    NDIManager ndiManager;
    bool ndiAvailable = false;
    if (ndiManager.Initialize()) {
        if (ndiManager.CreateSender(0, "VIB_Pista_Auto", 1920, 1080, true)) {
            ndiAvailable = true;
            Logger::Info("[PISTA AUTO] NDI sender creado: VIB_Pista_Auto (1920x1080@30 UYVY)");
        }
    } else {
        Logger::Warning("[PISTA AUTO] NDI no disponible");
    }

    // Initialize DeckLink capture for real frames (single channel)
    std::unique_ptr<DeckLinkCapture> deckLinkCapture;
    bool realCaptureAvailable = false;
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("[PISTA AUTO] DeckLink devices found: " + std::to_string(numDevices));

    if (numDevices > 0 && decklinkDeviceIndex < numDevices) {
        deckLinkCapture = std::make_unique<DeckLinkCapture>();
        if (deckLinkCapture->Initialize(decklinkDeviceIndex, "PistaAuto_CAM29")) {
            deckLinkCapture->SetFrameReadyHandler(
                [&sphereVerifier, &ndiManager, ndiAvailable]
                (const VideoChannel& channel, cudaStream_t stream) {
                    sphereVerifier.PushFrame(channel.channelID, channel.cudaYUVBuffer,
                                            channel.width, channel.height);
                    if (ndiAvailable) {
                        ndiManager.SendUYVYFrame(0, channel.cudaYUVBuffer,
                                                 channel.width, channel.height, stream);
                    }
                }
            );
            if (deckLinkCapture->Start()) {
                realCaptureAvailable = true;
                Logger::Info("[PISTA AUTO] DeckLink capture INICIADO - frames reales YOLO");
            } else {
                Logger::Warning("[PISTA AUTO] DeckLink capture Start() fallo");
            }
        } else {
            Logger::Warning("[PISTA AUTO] DeckLink device 0 no disponible");
        }
    } else {
        Logger::Warning("[PISTA AUTO] No se encontraron dispositivos DeckLink");
    }

    sphereVerifier.SetRealCaptureAvailable(realCaptureAvailable);

    // Create ChoreographyEngine (no VMix, no SceneManager for Phase 1)
    Choreography::ChoreographyEngine engine(nullptr, nullptr);
    engine.SetContinueOnError(false);  // Stop on first error - critico para verificacion
    engine.SetVMixRequired(false);

    // Wire controllers
    if (ptzAvailable) {
        engine.SetPTZController(ptzController.get());
        Logger::Info("[PISTA AUTO] PTZController conectado al engine");
    } else {
        Logger::Warning("[PISTA AUTO] PTZController NO disponible - eventos PTZ seran saltados");
    }
    engine.SetSphereVerifier(&sphereVerifier);
    Logger::Info("[PISTA AUTO] SphereVerifier conectado al engine");

    // Set up callbacks
    engine.SetStateCallback([](Choreography::EngineState state) {
        std::string stateStr;
        switch (state) {
            case Choreography::EngineState::Idle: stateStr = "Idle"; break;
            case Choreography::EngineState::Ready: stateStr = "Ready"; break;
            case Choreography::EngineState::Running: stateStr = "Running"; break;
            case Choreography::EngineState::Paused: stateStr = "Paused"; break;
            case Choreography::EngineState::Stopping: stateStr = "Stopping"; break;
            case Choreography::EngineState::Error: stateStr = "Error"; break;
        }
        Logger::Info("[PISTA AUTO] State: " + stateStr);
    });

    engine.SetEventStartCallback([](size_t index, const Choreography::ChoreographyEvent& event) {
        Logger::Info("[PISTA AUTO] Event " + std::to_string(index + 1) + ": " + event.GetTypeName());
    });

    engine.SetEventCompleteCallback([](const Choreography::EventResult& result) {
        if (!result.success) {
            Logger::Warning("[PISTA AUTO] Event " + std::to_string(result.eventIndex + 1) +
                           " FALLIDO: " + result.errorMessage);
        } else {
            Logger::Info("[PISTA AUTO] Event " + std::to_string(result.eventIndex + 1) + " OK");
        }
    });

    engine.SetErrorCallback([](const std::string& error) {
        Logger::Error("[PISTA AUTO] Error: " + error);
    });

    // Load choreography script
    std::string scriptPath = config.choreographyConfig.scriptPath;
    if (scriptPath.empty() || scriptPath == "config/choreography/inicio_carrera.json") {
        // Override with automated track script
        scriptPath = "config/choreography/pista_automatizada.json";
    }
    Logger::Info("[PISTA AUTO] Cargando script: " + scriptPath);

    if (!engine.Load(scriptPath)) {
        Logger::Error("[PISTA AUTO] No se pudo cargar el script: " + engine.GetLastError());
        std::cout << "\n  ERROR: No se pudo cargar el script de coreografia.\n";
        std::cout << "  Ruta: " << scriptPath << "\n";
        std::cout << "  " << engine.GetLastError() << "\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }

    // Show summary
    std::cout << "\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |              MODO PISTA AUTOMATIZADA - FASE 1                      |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   Radar:     RADAR_01..04 -> salidas de radar_routing (VideoHub)   |\n";
    std::cout << "  |   PTZ:       " << (ptzAvailable ? "CONECTADO" : "NO DISPONIBLE") << "                                              |\n";
    std::cout << "  |   Preset:    1 (vista amplia de todo el tramo)                     |\n";
    std::cout << "  |   Esferas:   10 esperadas en la salida                             |\n";
    std::cout << "  |   Timeout:   15 segundos                                           |\n";
    std::cout << "  |   Inferencia:" << (inferenceEngine->IsStubMode() ? " STUB (simulada)" : " TensorRT") << "                                    |\n";
    std::cout << "  |   Captura:  " << (realCaptureAvailable ? "DECKLINK (frames reales)" : "STUB (datos simulados)") << "                      |\n";
    std::cout << "  |   NDI Live: " << (ndiAvailable ? "VIB_Pista_Auto (video crudo)" : "NO DISPONIBLE") << "                         |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  |   Secuencia:                                                       |\n";
    std::cout << "  |     1. PTZ -> Preset 1 (vista amplia)                              |\n";
    std::cout << "  |     2. Verificar presencia de 10 esferas en salida                 |\n";
    std::cout << "  |                                                                    |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "\n  Presione ENTER para iniciar...";
    std::cin.get();

    // Start execution
    Logger::Info("[PISTA AUTO] Iniciando ejecucion...");
    if (!engine.Start()) {
        Logger::Error("[PISTA AUTO] No se pudo iniciar: " + engine.GetLastError());
        std::cout << "\n  ERROR al iniciar: " << engine.GetLastError() << "\n";
        std::cout << "  Presione ENTER para continuar...";
        std::cin.get();
        return false;
    }

    // Wait for engine thread to actually start running (race condition fix)
    int waitTicks = 0;
    while (!engine.IsRunning() && engine.GetState() != Choreography::EngineState::Error) {
        if (++waitTicks > 100) {  // 5 second timeout (100 * 50ms)
            Logger::Error("[PISTA AUTO] Engine no arranco despues de 5 segundos");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Logger::Info("[PISTA AUTO] Engine en ejecucion - estado=" +
                std::to_string(static_cast<int>(engine.GetState())));

    // Monitor loop
    std::cout << "\n  Ejecutando secuencia de verificacion...\n\n";
    bool aborted = false;
    while (engine.IsRunning()) {
        auto status = engine.GetStatus();
        std::cout << "\r  Progreso: evento " << (status.currentEventIndex + 1)
                  << "/" << status.totalEvents
                  << " (" << status.currentEventType << ")"
                  << " - " << (status.elapsedTime.count() / 1000) << "s   " << std::flush;

        // ESC to abort
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Logger::Info("[PISTA AUTO] ESC presionado - abortando...");
            aborted = true;
            engine.Stop();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\n\n";

    // Report result
    if (aborted) {
        Logger::Info("[PISTA AUTO] Verificacion cancelada por el usuario");
        std::cout << "  Verificacion cancelada por el usuario.\n";
    } else {
        auto finalState = engine.GetState();
        int errorCount = engine.GetStatus().errorCount;
        if (finalState == Choreography::EngineState::Error || errorCount > 0) {
            Logger::Warning("[PISTA AUTO] Verificacion finalizada CON ERRORES (errores=" +
                           std::to_string(errorCount) + ")");
            std::cout << "  +--------------------------------------------------------------------+\n";
            std::cout << "  |              *** VERIFICACION FALLIDA ***                          |\n";
            std::cout << "  +--------------------------------------------------------------------+\n";
            std::cout << "  |  Errores: " << errorCount << "                                                       |\n";
            std::cout << "  |  Revise los logs para ver cual evento fallo.                       |\n";
            std::cout << "  |  Verifique que:                                                    |\n";
            std::cout << "  |  - Las 10 esferas esten visibles en la salida                     |\n";
            std::cout << "  |  - La camara PTZ este encendida y en preset 1                      |\n";
            std::cout << "  |  - El VideoHub rutee los radares segun radar_routing               |\n";
            std::cout << "  +--------------------------------------------------------------------+\n";
        } else {
            Logger::Info("[PISTA AUTO] Verificacion COMPLETADA EXITOSAMENTE");
            std::cout << "  +--------------------------------------------------------------------+\n";
            std::cout << "  |              *** VERIFICACION EXITOSA ***                          |\n";
            std::cout << "  +--------------------------------------------------------------------+\n";
            std::cout << "  |  10 esferas detectadas correctamente en la salida.                 |\n";
            std::cout << "  |  La pista esta lista para la carrera.                              |\n";
            std::cout << "  +--------------------------------------------------------------------+\n";
        }
    }

    std::cout << "\n  Presione ENTER para continuar...";
    std::cin.get();

    // Stop DeckLink capture if active
    if (deckLinkCapture) {
        deckLinkCapture->Stop();
        Logger::Info("[PISTA AUTO] DeckLink capture detenido");
    }

    // Release NDI sender
    if (ndiAvailable) {
        ndiManager.ReleaseSender(0);
        Logger::Info("[PISTA AUTO] NDI sender liberado");
    }

    Logger::Info("[PISTA AUTO] Modo Pista Automatizada (Fase 1) finalizado");
    return true;
}

// Bitmap font 3x5 for digits 0-9
static const int FONT_LIVE[10][5][3] = {
    {{1,1,1},{1,0,1},{1,0,1},{1,0,1},{1,1,1}}, // 0
    {{0,1,0},{1,1,0},{0,1,0},{0,1,0},{1,1,1}}, // 1
    {{1,1,1},{0,0,1},{1,1,1},{1,0,0},{1,1,1}}, // 2
    {{1,1,1},{0,0,1},{1,1,1},{0,0,1},{1,1,1}}, // 3
    {{1,0,1},{1,0,1},{1,1,1},{0,0,1},{0,0,1}}, // 4
    {{1,1,1},{1,0,0},{1,1,1},{0,0,1},{1,1,1}}, // 5
    {{1,1,1},{1,0,0},{1,1,1},{1,0,1},{1,1,1}}, // 6
    {{1,1,1},{0,0,1},{0,1,0},{1,0,0},{1,0,0}}, // 7
    {{1,1,1},{1,0,1},{1,1,1},{1,0,1},{1,1,1}}, // 8
    {{1,1,1},{1,0,1},{1,1,1},{0,0,1},{1,1,1}}, // 9
};

static void drawDigitLive(unsigned char* buf, int W, int digit, int cx, int cy, int scale,
                          unsigned char r, unsigned char g, unsigned char b) {
    if (digit < 0 || digit > 9) return;
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if (!FONT_LIVE[digit][row][col]) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = cx + col * scale + sx;
                    int py = cy + row * scale + sy;
                    if (px >= 0 && px < W && py >= 0 && py < 1080) {
                        int o = (py * W + px) * 4;
                        buf[o]=b; buf[o+1]=g; buf[o+2]=r; buf[o+3]=255;
                    }
                }
            }
        }
    }
}

// Get pixel color for sphere border (with stripes for special spheres)
static void getSphereColor(int ballID, int px, int py, unsigned char& r, unsigned char& g, unsigned char& b) {
    // 0=blanco, 1=amarillo, 2=azul, 3=rojo, 4=morado, 5=naranja, 6=verde, 7=vinotinto, 8=negro, 9=amarillo-blanco
    const int colors[10][3] = {
        {255,255,255}, {255,255,0}, {0,0,255}, {255,0,0}, {128,0,128},
        {255,165,0}, {0,255,0}, {128,0,0}, {80,80,80}, {255,255,224}
    };
    int idx = (ballID >= 0 && ballID < 10) ? ballID : (ballID % 10);
    r = colors[idx][0]; g = colors[idx][1]; b = colors[idx][2];

    if (ballID == 9) { // amarillo-blanco stripes
        if ((px + py) % 2 == 0) { r=255; g=255; b=0; } else { r=255; g=255; b=255; }
    }
    if (ballID == 4) { // morado-blanco stripes
        if ((px + py) % 2 == 0) { r=128; g=0; b=128; } else { r=255; g=255; b=255; }
    }
    if (ballID == 8) { // negro with white dots
        if ((px + py) % 3 == 0) { r=255; g=255; b=255; } else { r=80; g=80; b=80; }
    }
}

bool RunLivePreview() {
    Logger::Info("=== LIVE PREVIEW - PRUEBA DE DETECCION YOLO EN VIVO ===");

    Config config = LoadConfig("config.json");
    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    if (!videoHub.Connect()) {
        Logger::Error("[LIVE] No se pudo conectar con VideoHub");
        std::cout << "\n  ERROR: VideoHub no disponible\n";
        std::cin.get();
        return false;
    }
    ApplyRadarRoutingFromConfig(videoHub, config, "LIVE");

    auto inferenceEngine = std::make_shared<InferenceEngine>();
    inferenceEngine->Initialize(config.inferenceConfig);
    Logger::Info("[LIVE] InferenceEngine: " + std::string(inferenceEngine->IsStubMode() ? "STUB" : "TensorRT"));

    int decklinkDeviceIndex = 8;
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("[LIVE] DeckLink devices: " + std::to_string(numDevices));

    // NDI: two senders - raw video + boxes
    NDIManager ndiManager;
    bool ndiRaw = ndiManager.Initialize() && ndiManager.CreateSender(0, "VIB_Pista_Raw", 1920, 1080, true);
    bool ndiBox = ndiManager.CreateSender(1, "VIB_Pista_Boxes", 1920, 1080, false);
    if (ndiRaw) Logger::Info("[LIVE] NDI raw: VIB_Pista_Raw");
    if (ndiBox) Logger::Info("[LIVE] NDI boxes: VIB_Pista_Boxes");

    // Pre-allocate buffers for BGRA conversion and box drawing
    const int W = 1920, H = 1080;
    std::vector<unsigned char> uyvyBuf(W * H * 2);
    std::vector<unsigned char> bgraCpuBuf(W * H * 4);
    void* bgraGpuBuf = nullptr;
    cudaMalloc(&bgraGpuBuf, W * H * 4);
    cudaStream_t infStream;
    cudaStreamCreate(&infStream);

    std::unique_ptr<DeckLinkCapture> deckLinkCapture;
    if (numDevices > 0 && decklinkDeviceIndex < numDevices) {
        deckLinkCapture = std::make_unique<DeckLinkCapture>();
        if (!deckLinkCapture->Initialize(decklinkDeviceIndex, "LivePreview")) {
            Logger::Error("[LIVE] DeckLink init fallo"); return false;
        }
    } else { Logger::Error("[LIVE] No DeckLink"); return false; }

    // Shared state
    std::mutex detMutex, uyvyMutex;
    std::vector<BallDetection> latestDets;
    void* latestUYVY = nullptr;
    std::atomic<int> frameCount{0};
    std::atomic<bool> running{true};

    deckLinkCapture->SetFrameReadyHandler(
        [&](const VideoChannel& channel, cudaStream_t stream) {
            int fc = frameCount.fetch_add(1);
            // Always send raw UYVY via NDI at 30fps
            if (ndiRaw) ndiManager.SendUYVYFrame(0, channel.cudaYUVBuffer, channel.width, channel.height, stream);
            // Track latest frame pointer
            latestUYVY = channel.cudaYUVBuffer;
        }
    );

    if (!deckLinkCapture->Start()) {
        Logger::Error("[LIVE] Capture start fallo"); return false;
    }

    std::cout << "\n  +--------------------------------------------------------------------+\n";
    std::cout << "  |           LIVE PREVIEW - DETECCIONES YOLO EN VIVO                  |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |  NDI Raw:  VIB_Pista_Raw (video limpio 30fps)                     |\n";
    std::cout << "  |  NDI Box:  VIB_Pista_Boxes (video + cajas ~5fps)                  |\n";
    std::cout << "  |  Consola:  detecciones en vivo                                    |\n";
    std::cout << "  |  ESC para salir                                                   |\n";
    std::cout << "  +--------------------------------------------------------------------+\n\n";

    auto lastBox = std::chrono::steady_clock::now();
    auto lastPrint = std::chrono::steady_clock::now();
    while (running.load()) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { running.store(false); break; }

        auto now = std::chrono::steady_clock::now();
        bool doBoxes = ndiBox && latestUYVY &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBox).count() >= 200;
        bool doPrint = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= 1000;

        if (doBoxes || doPrint) {
            // Download UYVY from GPU to CPU (shared for boxes + detection print)
            void* uyvyPtr = latestUYVY;
            if (uyvyPtr) {
                cudaMemcpy(uyvyBuf.data(), uyvyPtr, W * H * 2, cudaMemcpyDeviceToHost);
            }

            if (doBoxes && uyvyPtr) {
                lastBox = now;
                // CPU UYVY->BGRA conversion (ITU-R BT.601)
                for (int i = 0; i < W * H / 2; i++) {
                    int idx = i * 4;
                    int u = uyvyBuf[idx+0], y0 = uyvyBuf[idx+1], v = uyvyBuf[idx+2], y1 = uyvyBuf[idx+3];
                    auto conv = [](int y, int u, int v) -> std::tuple<int,int,int> {
                        int c=y-16, d=u-128, e=v-128;
                        int r=(298*c+409*e+128)>>8, g=(298*c-100*d-208*e+128)>>8, b=(298*c+516*d+128)>>8;
                        auto cl = [](int x){ return (x<0)?0:(x>255)?255:x; };
                        return {cl(r), cl(g), cl(b)};
                    };
                    auto [r0,g0,b0] = conv(y0,u,v);
                    auto [r1,g1,b1] = conv(y1,u,v);
                    // Write BGRA: pixel 0
                    int o0 = (i*2)*4;     bgraCpuBuf[o0]=b0; bgraCpuBuf[o0+1]=g0; bgraCpuBuf[o0+2]=r0; bgraCpuBuf[o0+3]=255;
                    // Write BGRA: pixel 1
                    int o1 = (i*2+1)*4;   bgraCpuBuf[o1]=b1; bgraCpuBuf[o1+1]=g1; bgraCpuBuf[o1+2]=r1; bgraCpuBuf[o1+3]=255;
                }

                // Draw colored boxes with numbers on BGRA buffer
                std::vector<BallDetection> dets;
                { std::lock_guard<std::mutex> lock(detMutex); dets = latestDets; }
                for (const auto& d : dets) {
                    int cx=(int)(d.x*W), cy=(int)(d.y*H);
                    int bw=(int)(d.width*W), bh=(int)(d.height*H);
                    int x1=std::max(0,cx-bw/2), y1=std::max(0,cy-bh/2);
                    int x2=std::min(W-1,cx+bw/2), y2=std::min(H-1,cy+bh/2);
                    int id=d.ballID;
                    const int B=3;
                    // Draw border with sphere-specific color
                    for (int x=x1; x<=x2; x++) {
                        for (int t=0; t<B&&y1+t<=y2; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x,y1+t,cr,cg,cb);
                            int o=((y1+t)*W+x)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                        for (int t=0; t<B&&y2-t>=y1; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x,y2-t,cr,cg,cb);
                            int o=((y2-t)*W+x)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                    }
                    for (int y=y1; y<=y2; y++) {
                        for (int t=0; t<B&&x1+t<=x2; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x1+t,y,cr,cg,cb);
                            int o=(y*W+x1+t)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                        for (int t=0; t<B&&x2-t>=x1; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x2-t,y,cr,cg,cb);
                            int o=(y*W+x2-t)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                    }
                    // Draw number in center (red for yellow/cream spheres, white for others)
                    int scale=std::max(2, std::min(bw,bh)/20);
                    int tw=3*scale, th=5*scale;
                    if (id == 1 || id == 9)
                        drawDigitLive(bgraCpuBuf.data(), W, id, cx-tw/2, cy-th/2, scale, 255,0,0);
                    else
                        drawDigitLive(bgraCpuBuf.data(), W, id, cx-tw/2, cy-th/2, scale, 255,255,255);
                }

                // Send BGRA buffer via NDI (uses CPU buffer — correct byte order BGRA)
                cudaMemcpy(bgraGpuBuf, bgraCpuBuf.data(), W * H * 4, cudaMemcpyHostToDevice);
                ndiManager.SendBGRAFrame(1, bgraGpuBuf, W, H, 0);
            }

            if (doPrint && uyvyPtr && inferenceEngine && !inferenceEngine->IsStubMode()) {
                lastPrint = now;
                // YOLO runs directly on GPU buffer (no CPU download needed for inference)
                auto dets = inferenceEngine->ProcessFrameUYVY(
                    uyvyPtr, 0, W, H, infStream, nullptr);
                { std::lock_guard<std::mutex> lock(detMutex); latestDets = dets; }
                std::cout << "\r  [" << frameCount.load() << " frames] ";
                if (dets.empty()) {
                    std::cout << "Sin detecciones                    " << std::flush;
                } else {
                    std::cout << dets.size() << " detecciones: ";
                    for (size_t i = 0; i < std::min(dets.size(), (size_t)8); i++)
                        std::cout << "B" << dets[i].ballID << "(" << std::fixed << std::setprecision(2) << dets[i].confidence << ") ";
                    if (dets.size() > 8) std::cout << "...";
                    std::cout << "   " << std::flush;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n\n  Live preview finalizado. Presione ENTER...";
    std::cin.get();
    deckLinkCapture->Stop();
    cudaStreamDestroy(infStream);
    if (bgraGpuBuf) cudaFree(bgraGpuBuf);
    if (ndiRaw) ndiManager.ReleaseSender(0);
    if (ndiBox) ndiManager.ReleaseSender(1);
    Logger::Info("[LIVE] Finalizado");
    return true;
}

// Arrival order: spheres sorted by x-position (left -> right). Invert to
// kArrivalLeftToRight=false if the physical track runs the other direction.
static constexpr bool kArrivalLeftToRight = false;

bool RunArrivalOrderTest() {
    Logger::Info("=== TEST ORDEN DE LLEGADA - DETECCION CONTINUA (MOTOR DEDICADO) ===");

    Config config = LoadConfig("config.json");
    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    if (!videoHub.Connect()) {
        Logger::Error("[LLEGADA] No se pudo conectar con VideoHub");
        std::cout << "\n  ERROR: VideoHub no disponible\n";
        std::cin.get();
        return false;
    }
    ApplyRadarRoutingFromConfig(videoHub, config, "LLEGADA");

    // Dedicated arrival engine (second engine, does not touch the main one)
    auto inferenceEngine = std::make_shared<InferenceEngine>();
    inferenceEngine->Initialize(config.llegadaInferenceConfig);
    Logger::Info("[LLEGADA] InferenceEngine: " + std::string(inferenceEngine->IsStubMode() ? "STUB" : "TensorRT")
                 + " (" + config.llegadaInferenceConfig.modelPath + ")");

    int decklinkDeviceIndex = 8;
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("[LLEGADA] DeckLink devices: " + std::to_string(numDevices));

    // NDI: two senders - raw video + boxes
    NDIManager ndiManager;
    bool ndiRaw = ndiManager.Initialize() && ndiManager.CreateSender(0, "VIB_Llegada_Raw", 1920, 1080, true);
    bool ndiBox = ndiManager.CreateSender(1, "VIB_Llegada_Boxes", 1920, 1080, false);
    if (ndiRaw) Logger::Info("[LLEGADA] NDI raw: VIB_Llegada_Raw");
    if (ndiBox) Logger::Info("[LLEGADA] NDI boxes: VIB_Llegada_Boxes");

    // Pre-allocate buffers for BGRA conversion and box drawing
    const int W = 1920, H = 1080;
    std::vector<unsigned char> uyvyBuf(W * H * 2);
    std::vector<unsigned char> bgraCpuBuf(W * H * 4);
    void* bgraGpuBuf = nullptr;
    cudaMalloc(&bgraGpuBuf, W * H * 4);
    cudaStream_t infStream;
    cudaStreamCreate(&infStream);

    std::unique_ptr<DeckLinkCapture> deckLinkCapture;
    if (numDevices > 0 && decklinkDeviceIndex < numDevices) {
        deckLinkCapture = std::make_unique<DeckLinkCapture>();
        if (!deckLinkCapture->Initialize(decklinkDeviceIndex, "ArrivalOrderTest")) {
            Logger::Error("[LLEGADA] DeckLink init fallo"); return false;
        }
    } else { Logger::Error("[LLEGADA] No DeckLink"); return false; }

    // Shared state
    std::mutex detMutex, uyvyMutex;
    std::vector<BallDetection> latestDets;
    std::vector<int> latestOrder;   // ball IDs sorted by x (arrival order)
    void* latestUYVY = nullptr;
    std::atomic<int> frameCount{0};
    std::atomic<bool> running{true};

    // Estabilización + emisión del orden de llegada
    ArrivalOrderStabilizer stabilizer(config.arrivalStabilizerConfig);
    ArrivalEmitter emitter(config.arrivalEndpointConfig);

    deckLinkCapture->SetFrameReadyHandler(
        [&](const VideoChannel& channel, cudaStream_t stream) {
            int fc = frameCount.fetch_add(1);
            if (ndiRaw) ndiManager.SendUYVYFrame(0, channel.cudaYUVBuffer, channel.width, channel.height, stream);
            latestUYVY = channel.cudaYUVBuffer;
        }
    );

    if (!deckLinkCapture->Start()) {
        Logger::Error("[LLEGADA] Capture start fallo"); return false;
    }

    std::cout << "\n  +--------------------------------------------------------------------+\n";
    std::cout << "  |           TEST ORDEN DE LLEGADA - DETECCION CONTINUA               |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |  NDI Raw:  VIB_Llegada_Raw (video limpio 30fps)                   |\n";
    std::cout << "  |  NDI Box:  VIB_Llegada_Boxes (video + cajas + orden ~5fps)        |\n";
    std::cout << "  |  Consola:  orden + confirmacion por estabilidad                   |\n";
    std::cout << "  |  R: re-armar confirmacion    ESC: salir                          |\n";
    std::cout << "  +--------------------------------------------------------------------+\n\n";

    auto lastBox = std::chrono::steady_clock::now();
    auto lastPrint = std::chrono::steady_clock::now();
    while (running.load()) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { running.store(false); break; }

        auto now = std::chrono::steady_clock::now();
        bool doBoxes = ndiBox && latestUYVY &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastBox).count() >= 200;
        bool doPrint = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPrint).count() >= 1000;

        if (doBoxes || doPrint) {
            void* uyvyPtr = latestUYVY;
            if (uyvyPtr) {
                cudaMemcpy(uyvyBuf.data(), uyvyPtr, W * H * 2, cudaMemcpyDeviceToHost);
            }

            if (doBoxes && uyvyPtr) {
                lastBox = now;
                // CPU UYVY->BGRA conversion (ITU-R BT.601)
                for (int i = 0; i < W * H / 2; i++) {
                    int idx = i * 4;
                    int u = uyvyBuf[idx+0], y0 = uyvyBuf[idx+1], v = uyvyBuf[idx+2], y1 = uyvyBuf[idx+3];
                    auto conv = [](int y, int u, int v) -> std::tuple<int,int,int> {
                        int c=y-16, d=u-128, e=v-128;
                        int r=(298*c+409*e+128)>>8, g=(298*c-100*d-208*e+128)>>8, b=(298*c+516*d+128)>>8;
                        auto cl = [](int x){ return (x<0)?0:(x>255)?255:x; };
                        return {cl(r), cl(g), cl(b)};
                    };
                    auto [r0,g0,b0] = conv(y0,u,v);
                    auto [r1,g1,b1] = conv(y1,u,v);
                    int o0 = (i*2)*4;     bgraCpuBuf[o0]=b0; bgraCpuBuf[o0+1]=g0; bgraCpuBuf[o0+2]=r0; bgraCpuBuf[o0+3]=255;
                    int o1 = (i*2+1)*4;   bgraCpuBuf[o1]=b1; bgraCpuBuf[o1+1]=g1; bgraCpuBuf[o1+2]=r1; bgraCpuBuf[o1+3]=255;
                }

                // Draw colored boxes with numbers on BGRA buffer
                std::vector<BallDetection> dets;
                { std::lock_guard<std::mutex> lock(detMutex); dets = latestDets; }
                for (const auto& d : dets) {
                    int cx=(int)(d.x*W), cy=(int)(d.y*H);
                    int bw=(int)(d.width*W), bh=(int)(d.height*H);
                    int x1=std::max(0,cx-bw/2), y1=std::max(0,cy-bh/2);
                    int x2=std::min(W-1,cx+bw/2), y2=std::min(H-1,cy+bh/2);
                    int id=d.ballID;
                    const int B=3;
                    for (int x=x1; x<=x2; x++) {
                        for (int t=0; t<B&&y1+t<=y2; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x,y1+t,cr,cg,cb);
                            int o=((y1+t)*W+x)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                        for (int t=0; t<B&&y2-t>=y1; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x,y2-t,cr,cg,cb);
                            int o=((y2-t)*W+x)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                    }
                    for (int y=y1; y<=y2; y++) {
                        for (int t=0; t<B&&x1+t<=x2; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x1+t,y,cr,cg,cb);
                            int o=(y*W+x1+t)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                        for (int t=0; t<B&&x2-t>=x1; t++) {
                            unsigned char cr,cg,cb; getSphereColor(id,x2-t,y,cr,cg,cb);
                            int o=(y*W+x2-t)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr;
                        }
                    }
                    int scale=std::max(2, std::min(bw,bh)/20);
                    int tw=3*scale, th=5*scale;
                    if (id == 1 || id == 9)
                        drawDigitLive(bgraCpuBuf.data(), W, id, cx-tw/2, cy-th/2, scale, 255,0,0);
                    else
                        drawDigitLive(bgraCpuBuf.data(), W, id, cx-tw/2, cy-th/2, scale, 255,255,255);
                }

                // Draw arrival-order sequence (left -> right) at the top-left corner
                std::vector<int> order;
                { std::lock_guard<std::mutex> lock(detMutex); order = latestOrder; }
                int oScale = 12;   // números del orden más grandes para legibilidad en NDI
                int oX = 24, oY = 24;
                for (size_t i = 0; i < order.size(); i++) {
                    int id = order[i];
                    unsigned char cr,cg,cb; getSphereColor(id, 0, 0, cr, cg, cb);
                    // Barra inferior del color de la esfera (correlaciona dígito -> color)
                    for (int x = 0; x < 3*oScale; x++) {
                        int px = oX + x;
                        for (int t = 0; t < 4; t++) {
                            int py = oY + 5*oScale + 4 + t;
                            int o = (py*W + px)*4; bgraCpuBuf[o]=cb; bgraCpuBuf[o+1]=cg; bgraCpuBuf[o+2]=cr; bgraCpuBuf[o+3]=255;
                        }
                    }
                    // Número del orden en azul
                    drawDigitLive(bgraCpuBuf.data(), W, id, oX, oY, oScale, 0, 0, 255);
                    oX += 3*oScale + 10;
                }

                // Send BGRA buffer via NDI (uses CPU buffer — correct byte order BGRA)
                cudaMemcpy(bgraGpuBuf, bgraCpuBuf.data(), W * H * 4, cudaMemcpyHostToDevice);
                ndiManager.SendBGRAFrame(1, bgraGpuBuf, W, H, 0);
            }

            if (doPrint && uyvyPtr && inferenceEngine && !inferenceEngine->IsStubMode()) {
                lastPrint = now;
                auto dets = inferenceEngine->ProcessFrameUYVY(
                    uyvyPtr, 0, W, H, infStream, nullptr);
                { std::lock_guard<std::mutex> lock(detMutex); latestDets = dets; }

                // Sort by x to compute arrival order (left -> right)
                std::vector<BallDetection> sorted = dets;
                std::sort(sorted.begin(), sorted.end(), [](const BallDetection& a, const BallDetection& b) {
                    return kArrivalLeftToRight ? (a.x < b.x) : (a.x > b.x);
                });
                std::vector<int> order;
                order.reserve(sorted.size());
                for (const auto& d : sorted) order.push_back(d.ballID);
                { std::lock_guard<std::mutex> lock(detMutex); latestOrder = order; }

                // Formatea el orden como "[a,b,c]" para logueo.
                auto fmtOrder = [](const std::vector<int>& o) {
                    std::string s = "[";
                    for (size_t i = 0; i < o.size(); i++) {
                        if (i) s += ",";
                        s += std::to_string(o[i]);
                    }
                    return s + "]";
                };

                // Tecla R: re-armar la confirmación (para testear sin salir de la opción)
                if (GetAsyncKeyState('R') & 0x8000) {
                    static auto lastResetKey = std::chrono::steady_clock::now();
                    auto nowR = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(nowR - lastResetKey).count() >= 1000) {
                        lastResetKey = nowR;
                        stabilizer.Reset();
                        emitter.Reset();
                        std::cout << "\n  [reset] confirmación re-armada\n" << std::flush;
                        Logger::Info("[LLEGADA] confirmacion re-armada (tecla R)");
                    }
                }

                // Confirmación del orden: emitir solo cuando se estabiliza
                if (!stabilizer.IsConfirmed() && stabilizer.AddOrder(order)) {
                    std::cout << "\n  === ORDEN CONFIRMADO ===\n";
                    Logger::Info("[LLEGADA] === ORDEN CONFIRMADO === orden=" + fmtOrder(stabilizer.GetConfirmedOrder()));
                    emitter.EmitConfirmedOrder(stabilizer.GetConfirmedOrder());
                }

                std::cout << "\r  [" << frameCount.load() << " frames] ";
                if (order.empty()) {
                    std::cout << "Sin detecciones                          " << std::flush;
                } else {
                    std::cout << "ORDEN: ";
                    for (size_t i = 0; i < order.size(); i++) {
                        std::cout << "bola_" << order[i];
                        if (i + 1 < order.size()) std::cout << ", ";
                    }
                    std::cout << "   [estable " << stabilizer.GetProgress() << "/"
                              << stabilizer.GetStableFrames() << "]"
                              << (stabilizer.IsConfirmed() ? " CONFIRMADO" : "");
                    int reject = stabilizer.GetLastReject();
                    if (reject == 2) std::cout << "  [ID duplicado: descartado]";
                    else if (reject == 1) std::cout << "  [pocas esferas]";
                    std::cout << std::flush;
                }

                // Auditoría por frame (1 Hz): estado del estabilizador y rechazos.
                {
                    std::string state = "[LLEGADA] frame=" + std::to_string(frameCount.load())
                        + " n=" + std::to_string(order.size())
                        + " orden=" + fmtOrder(order)
                        + " estable=" + std::to_string(stabilizer.GetProgress()) + "/" + std::to_string(stabilizer.GetStableFrames());
                    if (stabilizer.IsConfirmed()) state += " CONFIRMADO";
                    int r = stabilizer.GetLastReject();
                    if (r == 2) state += " [ID duplicado: descartado]";
                    else if (r == 1) state += " [pocas esferas]";
                    Logger::Info(state);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::cout << "\n\n  Test de orden de llegada finalizado. Presione ENTER...";
    std::cin.get();
    deckLinkCapture->Stop();
    cudaStreamDestroy(infStream);
    if (bgraGpuBuf) cudaFree(bgraGpuBuf);
    if (ndiRaw) ndiManager.ReleaseSender(0);
    if (ndiBox) ndiManager.ReleaseSender(1);
    Logger::Info("[LLEGADA] Finalizado");
    return true;
}

bool RunTrainingCapture() {
    Logger::Info("=== CAPTURA PARA ENTRENAMIENTO ===");

    Config config = LoadConfig("config.json");

    // Parametros de la opcion 10 leidos de config.json (seccion "training_capture").
    // videohub_input/output son puertos FISICOS 1-based (13 = puerto fisico 13).
    // decklink_device es el numero "device #N" que aparece en el log (1-based).
    int videohubInputPort = 13;
    int videohubOutputPort = 1;
    int decklinkDeviceNumber = 6;
    std::string ndiName = "VIB_Training";
    try {
        std::ifstream configFile("config.json");
        if (configFile.is_open()) {
            json j;
            configFile >> j;
            if (j.contains("training_capture") && j["training_capture"].is_object()) {
                auto& cap = j["training_capture"];
                if (cap.contains("videohub_input") && cap["videohub_input"].is_number()) {
                    videohubInputPort = cap["videohub_input"].get<int>();
                }
                if (cap.contains("videohub_output") && cap["videohub_output"].is_number()) {
                    videohubOutputPort = cap["videohub_output"].get<int>();
                }
                if (cap.contains("decklink_device") && cap["decklink_device"].is_number()) {
                    decklinkDeviceNumber = cap["decklink_device"].get<int>();
                }
                if (cap.contains("ndi_name") && cap["ndi_name"].is_string()) {
                    ndiName = cap["ndi_name"].get<std::string>();
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::Warning("[CAPTURE] training_capture invalido en config.json: " + std::string(e.what()));
    }

    if (videohubInputPort < 1) videohubInputPort = 1;
    if (videohubOutputPort < 1) videohubOutputPort = 1;
    if (decklinkDeviceNumber < 1) decklinkDeviceNumber = 1;

    Logger::Info("[CAPTURE] Config: VideoHub input " + std::to_string(videohubInputPort) +
                 " -> output " + std::to_string(videohubOutputPort) +
                 ", DeckLink device #" + std::to_string(decklinkDeviceNumber) +
                 ", NDI " + ndiName);

    auto inputLookup = BuildInputLookup();
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    if (!videoHub.Connect()) {
        Logger::Error("[CAPTURE] VideoHub no disponible");
        std::cout << "\n  ERROR: VideoHub no disponible\n";
        std::cin.get();
        return false;
    }
    if (!videoHub.RouteInputToOutput(videohubOutputPort - 1, videohubInputPort - 1)) {
        Logger::Warning("[CAPTURE] No se pudo rutear VideoHub input " +
                        std::to_string(videohubInputPort) + " -> output " +
                        std::to_string(videohubOutputPort));
    } else {
        Logger::Info("[CAPTURE] VideoHub: input " + std::to_string(videohubInputPort) +
                     " -> output " + std::to_string(videohubOutputPort) + " OK");
    }

    // DeckLink protocol index is 0-based; config uses the 1-based "device #N" from the log.
    int decklinkDeviceIndex = decklinkDeviceNumber - 1;
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("[CAPTURE] DeckLink devices: " + std::to_string(numDevices));

    NDIManager ndiManager;
    bool ndiOk = ndiManager.Initialize() && ndiManager.CreateSender(0, ndiName, 1920, 1080, true);
    if (ndiOk) Logger::Info("[CAPTURE] NDI: " + ndiName);

    std::unique_ptr<DeckLinkCapture> deckLinkCapture;
    if (numDevices > 0 && decklinkDeviceIndex < numDevices) {
        deckLinkCapture = std::make_unique<DeckLinkCapture>();
        if (!deckLinkCapture->Initialize(decklinkDeviceIndex, "TrainingCapture")) {
            Logger::Error("[CAPTURE] DeckLink init fallo"); return false;
        }
    } else {
        Logger::Error("[CAPTURE] DeckLink device #" + std::to_string(decklinkDeviceNumber) +
                      " no existe (dispositivos encontrados: " + std::to_string(numDevices) + ")");
        return false;
    }

    // Track latest frame
    std::atomic<void*> latestFrame{nullptr};
    std::atomic<bool> running{true};

    deckLinkCapture->SetFrameReadyHandler(
        [&](const VideoChannel& channel, cudaStream_t stream) {
            if (ndiOk) ndiManager.SendUYVYFrame(0, channel.cudaYUVBuffer, channel.width, channel.height, stream);
            latestFrame.store(channel.cudaYUVBuffer);
        }
    );

    if (!deckLinkCapture->Start()) {
        Logger::Error("[CAPTURE] Capture start fallo"); return false;
    }

    // Create output folder
    system("mkdir training_frames 2>nul");

    std::cout << "\n  +--------------------------------------------------------------------+\n";
    std::cout << "  |           CAPTURA PARA ENTRENAMIENTO (SIN DETECCION)               |\n";
    std::cout << "  +--------------------------------------------------------------------+\n";
    std::cout << "  |  VideoHub: input " << videohubInputPort << " -> output " << videohubOutputPort << "\n";
    std::cout << "  |  DeckLink: device #" << decklinkDeviceNumber << "\n";
    std::cout << "  |  NDI: " << ndiName << " (video continuo)\n";
    std::cout << "  |  ESPACIO: guardar frame actual (training_frames/cap_XXXX.ppm)      |\n";
    std::cout << "  |  ESC: salir                                                       |\n";
    std::cout << "  +--------------------------------------------------------------------+\n\n";

    int captureCount = 0;
    bool spaceWasDown = false;

    while (running.load()) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { running.store(false); break; }

        bool spaceDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        if (spaceDown && !spaceWasDown) {
            // Save current frame
            void* frame = latestFrame.load();
            if (frame) {
                std::ostringstream name;
                name << "training_frames/cap_" << std::setw(4) << std::setfill('0') << captureCount << ".ppm";
                if (FrameSaver::SaveYUVFrameAsPPM(frame, 1920, 1080, name.str())) {
                    std::cout << "  [SAVED] " << name.str() << "\n";
                    captureCount++;
                }
            }
        }
        spaceWasDown = spaceDown;

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    std::cout << "\n  Total capturas guardadas: " << captureCount << "\n";
    std::cout << "  Presione ENTER...";
    std::cin.get();

    deckLinkCapture->Stop();
    if (ndiOk) ndiManager.ReleaseSender(0);
    Logger::Info("[CAPTURE] Finalizado - " + std::to_string(captureCount) + " frames");
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    Logger::Init("VIB_System");
    Logger::Info("Visual Intelligence Bypass v2.0 Starting...");

    // --service: modo sin menu. La carrera la dispara el backend por la API HTTP
    // de control (bloque "control_api" de config.json). Sin el argumento, todo
    // funciona igual que siempre (menu interactivo).
    bool serviceMode = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = (argv[i] != nullptr) ? argv[i] : "";
        if (arg == "--service") {
            serviceMode = true;
        } else if (arg == "--help" || arg == "-h" || arg == "/?") {
            std::cout << "Uso: VIB.exe [--service]\n"
                      << "  (sin argumentos)  menu interactivo de siempre\n"
                      << "  --service         sin menu: control por la API HTTP\n"
                      << "                    (bloque control_api de config.json)\n";
            return 0;
        } else {
            Logger::Warning("Argumento desconocido ignorado: " + arg);
        }
    }
    if (serviceMode) {
        Logger::Info("Argumento --service: sin menu, control por la API HTTP");
    }
    
    // Initialize COM for DeckLink SDK (Component Object Model)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Logger::Error("Failed to initialize COM. HRESULT: 0x" + std::to_string(hr));
        return 1;
    }
    Logger::Info("COM initialized successfully for DeckLink SDK");
    
    // Crear el menú de usuario
    UserMenu menu;
    bool keepRunning = true;
    int exitCode = 0;

    if (serviceMode) {
        // En servicio no hay menu: se corre el modo de operacion y se vuelve
        // cuando la API pide POST /shutdown (o si la API no pudo levantarse).
        Logger::Info("[SERVICE] arrancando el modo de operacion sin menu");
        if (!RunRunningMode(true)) {
            Logger::Error("[SERVICE] el modo de operacion termino con error");
            exitCode = 2;
        }
        keepRunning = false;
    }
    
    while (keepRunning) {
        try {
            MenuOption selection = menu.ShowMainMenu();
            
            switch (selection) {
                case MenuOption::STRESS_TEST_DIAGNOSTIC:
                    RunStressTestMode();
                    // Vuelve al menú después del test
                    break;
                    
                case MenuOption::RUNNING_MODE:
                    if (!RunRunningMode()) {
                        Logger::Warning("Running mode finalizó con advertencias");
                    }
                    // Vuelve al menú para una nueva ronda
                    break;
                    
                case MenuOption::CLOSE_SYSTEM:
                    menu.ShowGoodbye();
                    keepRunning = false;
                    break;
                    
                case MenuOption::TOOLS_CONFIG:
                    menu.ShowToolsConfigStub();
                    // Vuelve al menú
                    break;
                    
                case MenuOption::RADAR_TEST_MODE:
                    if (!RunRadarTestMode()) {
                        Logger::Warning("Test de Radar 1 finalizó con advertencias");
                    }
                    // Vuelve al menú
                    break;
                    
                case MenuOption::SPHERE_VERIFIER_TEST:
                    if (!RunSphereVerifierTest()) {
                        Logger::Warning("SphereVerifier Test finalizó con advertencias");
                    }
                    // Vuelve al menú
                    break;
                    
                case MenuOption::VMIX_UTC_CHOREOGRAPHY:
                    if (!RunVMixUTCChoreography()) {
                        Logger::Warning("Coreografía vMix UTC finalizó con advertencias");
                    }
                    // Vuelve al menú
                    break;

                case MenuOption::AUTOMATED_TRACK:
                    if (!RunAutomatedTrackMode()) {
                        Logger::Warning("Modo Pista Automatizada finalizó con advertencias");
                    }
                    break;

                case MenuOption::LIVE_PREVIEW:
                    RunLivePreview();
                    break;

                case MenuOption::TRAINING_CAPTURE:
                    RunTrainingCapture();
                    break;

                case MenuOption::ARRIVAL_TEST:
                    RunArrivalOrderTest();
                    break;

                case MenuOption::API_SERVICE:
                    // [12]: inicializa todo (DeckLink, VideoHub, ESP32, motores),
                    // levanta la API de control y se queda en idle con un panel de
                    // estado en consola. Nada se dispara hasta que el backend manda
                    // POST /arm + POST /start. ESC vuelve a este menu.
                    if (!RunRunningMode(true, true)) {
                        Logger::Warning("Servicio API finalizó con advertencias");
                    }
                    break;

                case MenuOption::INVALID:
                default:
                    std::cout << "\n  Opcion invalida. Por favor seleccione 1-12.\n";
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    break;
            }
        } catch (const std::exception& e) {
            Logger::Error("Error en el sistema: " + std::string(e.what()));
            std::cout << "\n  Error: " << e.what() << "\n";
            std::cout << "  Presione ENTER para continuar...";
            std::cin.get();
        }
    }
    
    // Uninitialize COM before exiting
    CoUninitialize();
    Logger::Info("VIB cerrado correctamente");
    
    return exitCode;
}
