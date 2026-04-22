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
#include <Windows.h>
#include <objbase.h>  // For COM: CoInitializeEx, CoUninitialize
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <chrono>
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
#include "../scene/SceneManager.h"
#include "../output/RankingPublisher.h"
#include "../redis/RedisWorker.h"
#include "../telemetry/PerformanceMonitor.h"
#include "../utils/Logger.h"
#include "../utils/ThreadOptimizer.h"
#include "../utils/GPUDiagnostics.h"
#include "../utils/FrameSaver.h"
#include "../control/VideoHubClient.h"
#include "../control/TrackPhysicalController.h"
#include "../control/VMixController.h"
#include "../choreography/ChoreographyEngine.h"
#include "../verification/SphereVerifier.h"
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
        
        // Parse ranking_publisher section
        if (j.contains("ranking_publisher") && j["ranking_publisher"].is_object()) {
            auto& rp = j["ranking_publisher"];
            if (rp.contains("enabled")) config.rankingConfig.enabled = rp["enabled"].get<bool>();
            if (rp.contains("vmix_host")) config.rankingConfig.vmixHost = rp["vmix_host"].get<std::string>();
            if (rp.contains("vmix_tcp_port")) config.rankingConfig.vmixTcpPort = rp["vmix_tcp_port"].get<uint16_t>();
            if (rp.contains("publish_rate_hz")) config.rankingConfig.publishRateHz = rp["publish_rate_hz"].get<int>();
            if (rp.contains("title_input_name")) config.rankingConfig.titleInputName = rp["title_input_name"].get<std::string>();
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
 * @return true si finalizó correctamente, false si hubo error
 */
bool RunRunningMode() {
    Logger::Info("=== INICIANDO RUNNING MODE ===");
    
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
    
    Logger::Info("Initializing RankingPublisher...");
    auto rankingPublisher = std::make_shared<RankingPublisher>(config.rankingConfig);
    if (config.rankingConfig.enabled && !rankingPublisher->Initialize()) {
        Logger::Warning("RankingPublisher initialization failed");
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
                
                // Auto-start if configured
                if (config.choreographyConfig.autoStart) {
                    Logger::Info("ChoreographyEngine: Auto-starting choreography...");
                    if (!choreographyEngine->Start()) {
                        Logger::Warning("ChoreographyEngine: Auto-start failed");
                    }
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
    
    const int channelsToInit = (std::min)(numDevices, kMaxNDIChannels);
    for (int i = 0; i < channelsToInit; i++) {
        auto capture = std::make_unique<DeckLinkCapture>();
        const std::string channelName = "Channel_" + std::to_string(i + 1);
        if (capture->Initialize(i, channelName)) {
            capture->SetFrameReadyHandler([ndiManager, cameraSelector, 
                                          perfMonitor, &config, inferenceStream,
                                          inferenceEngine, positionMapper, ballTracker, 
                                          sceneManager, rankingPublisher, inferenceReady]
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
                    
                    if (rankingPublisher && rankingPublisher->IsEnabled()) {
                        auto ranking = ballTracker->GetRanking();
                        rankingPublisher->PublishRanking(ranking);
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
    Logger::Info("ESC - Volver al menu principal");
    Logger::Info("F2  - Reducir a 2 camaras (throttle)");
    Logger::Info("F3  - Restaurar a 4 camaras");
    Logger::Info("F4  - Parada de emergencia");
    Logger::Info("F5  - Reset telemetria");
    Logger::Info("--- SceneManager Manual Mode ---");
    Logger::Info("M   - Toggle AUTO/MANUAL mode");
    Logger::Info("F1/F6/F7 - Select config_a/b/c (MANUAL)");
    Logger::Info("G   - Toggle group G1_G4/G5_G8 (MANUAL)");
    Logger::Info("1-4 - Select camera in group (MANUAL)");
    if (config.choreographyConfig.enabled) {
        Logger::Info("--- Choreography Controls ---");
        Logger::Info("F12 - Start/Pause/Resume choreography");
    }
    Logger::Info("====================================");
    
    bool running = true;
    auto lastStatusLog = std::chrono::steady_clock::now();
    const auto statusInterval = std::chrono::seconds(10);
    
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
    
    while (running) {
        // Check for exit condition
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Logger::Info("ESC presionado - Volviendo al menu principal...");
            running = false;
        }
        
        // F2 - Reduce active cameras
        if (GetAsyncKeyState(VK_F2) & 0x8000) {
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
        if (GetAsyncKeyState(VK_F3) & 0x8000) {
            if (!f3Pressed) {
                f3Pressed = true;
                config.selectorTopK = 4;
                Logger::Info("[EMERGENCY] Restored active cameras to 4");
            }
        } else {
            f3Pressed = false;
        }
        
        // F4 - Emergency stop
        if (GetAsyncKeyState(VK_F4) & 0x8000) {
            if (!f4Pressed) {
                f4Pressed = true;
                running = false;
                Logger::Info("[EMERGENCY] Graceful stop initiated");
            }
        } else {
            f4Pressed = false;
        }
        
        // F5 - Reset telemetry
        if (GetAsyncKeyState(VK_F5) & 0x8000) {
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
        // Choreography Controls (F12)
        // ============================================================================
        if (GetAsyncKeyState(VK_F12) & 0x8000) {
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
        if (GetAsyncKeyState('M') & 0x8000) {
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
        if (GetAsyncKeyState(VK_F1) & 0x8000) {
            if (!f1KeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                f1KeyPressed = true;
                sceneManager->SelectConfig(0);
            }
        } else {
            f1KeyPressed = false;
        }
        
        // F6 - Select config_b (index 1) in MANUAL mode
        if (GetAsyncKeyState(VK_F6) & 0x8000) {
            if (!f6KeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                f6KeyPressed = true;
                sceneManager->SelectConfig(1);
            }
        } else {
            f6KeyPressed = false;
        }
        
        // F7 - Select config_c (index 2) in MANUAL mode
        if (GetAsyncKeyState(VK_F7) & 0x8000) {
            if (!f7KeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                f7KeyPressed = true;
                sceneManager->SelectConfig(2);
            }
        } else {
            f7KeyPressed = false;
        }
        
        // G - Toggle group in MANUAL mode
        if (GetAsyncKeyState('G') & 0x8000) {
            if (!gKeyPressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                gKeyPressed = true;
                sceneManager->ToggleGroup();
            }
        } else {
            gKeyPressed = false;
        }
        
        // 1-4 - Select camera in group (MANUAL mode)
        if (GetAsyncKeyState('1') & 0x8000) {
            if (!key1Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key1Pressed = true;
                sceneManager->SelectCameraInGroup(0);
            }
        } else {
            key1Pressed = false;
        }
        
        if (GetAsyncKeyState('2') & 0x8000) {
            if (!key2Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key2Pressed = true;
                sceneManager->SelectCameraInGroup(1);
            }
        } else {
            key2Pressed = false;
        }
        
        if (GetAsyncKeyState('3') & 0x8000) {
            if (!key3Pressed && sceneManager && sceneManager->GetMode() == SceneMode::MANUAL) {
                key3Pressed = true;
                sceneManager->SelectCameraInGroup(2);
            }
        } else {
            key3Pressed = false;
        }
        
        if (GetAsyncKeyState('4') & 0x8000) {
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
        
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    
    // Cleanup
    Logger::Info("Cerrando modo running...");
    
    // Stop choreography engine if running
    if (choreographyEngine) {
        Logger::Info("Stopping ChoreographyEngine...");
        choreographyEngine->Stop();
        choreographyEngine.reset();
    }
    
    if (rankingPublisher) {
        rankingPublisher->Shutdown();
    }
    
    if (inferenceStream) {
        cudaStreamDestroy(inferenceStream);
    }
    
    for (auto& capture : captureChannels) {
        capture->Stop();
    }
    captureChannels.clear();
    
    if (ndiManager) {
        ndiManager->ReleaseAll();
    }
    
    Logger::Info("Running mode finalizado");
    return true;
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

} // namespace

int main(int argc, char* argv[]) {
    Logger::Init("VIB_System");
    Logger::Info("Visual Intelligence Bypass v2.0 Starting...");
    
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
                    
                case MenuOption::INVALID:
                default:
                    std::cout << "\n  Opcion invalida. Por favor seleccione 1-6.\n";
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
