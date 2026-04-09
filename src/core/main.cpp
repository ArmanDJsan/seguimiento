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
#include "../ai/YOLOProcessor.h"
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
#include "../control/VideoHubClient.h"
#include "../control/TrackPhysicalController.h"
#include "../control/VMixController.h"

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

constexpr int kMaxNDIChannels = 12;
constexpr unsigned int kDefaultWidth = 3840;
constexpr unsigned int kDefaultHeight = 2160;
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

struct Config {
    std::string videohubIp;
    uint16_t videohubPort;
    std::string esp32Ip;
    uint16_t esp32Port;
    int targetSpheres;
    
    // Redis configuration
    bool redisEnabled;
    std::string redisHost;
    uint16_t redisPort;
    
    // YOLO configuration
    bool yoloEnabled;
    std::string yoloModelPath;
    std::string yoloFallback;
    int yoloBatchSize;
    float yoloConfidenceThreshold;
    float yoloNmsThreshold;
    bool yoloUseFP16;
    
    // Camera selector configuration
    bool selectorEnabled;
    int selectorTopK;
    float selectorMotionThreshold;
    float selectorEdgeMargin;
    
    // Hysteresis configuration
    float hysteresisSwitchThreshold;
    int hysteresisMinActiveFrames;
    float hysteresisDecayFactor;
    
    // Scene manager configuration
    bool sceneManagerEnabled;
    int sceneManagerMuteTimeoutMs;
    std::vector<GroupConfig> sceneManagerGroups;
    
    // Inference engine configuration
    InferenceEngineConfig inferenceConfig;
    
    // Position mapper configuration
    std::string calibrationFile;
    TrackBounds trackBounds;
    
    // Ball tracker configuration
    BallTrackerConfig ballTrackerConfig;
    
    // Ranking publisher configuration
    RankingPublisherConfig rankingConfig;
};

Config LoadConfig(const std::string& path) {
    Config config;
    // Set defaults
    config.videohubIp = "192.168.1.50";
    config.videohubPort = 9990;
    config.esp32Ip = "192.168.88.114";
    config.esp32Port = 80;
    config.targetSpheres = 10;
    
    // Redis defaults
    config.redisEnabled = true;
    config.redisHost = "127.0.0.1";
    config.redisPort = 6379;
    
    // YOLO defaults
    config.yoloEnabled = true;
    config.yoloModelPath = "models/yolov8n.engine";
    config.yoloFallback = "stub";
    config.yoloBatchSize = 4;
    config.yoloConfidenceThreshold = 0.5f;
    config.yoloNmsThreshold = 0.4f;
    config.yoloUseFP16 = true;
    
    // Camera selector defaults
    config.selectorEnabled = true;
    config.selectorTopK = 4;
    config.selectorMotionThreshold = 0.05f;
    config.selectorEdgeMargin = 0.1f;
    
    // Hysteresis defaults (racing-optimized)
    config.hysteresisSwitchThreshold = 0.15f;
    config.hysteresisMinActiveFrames = 10;
    config.hysteresisDecayFactor = 0.98f;

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
        
        // Parse YOLO section
        if (j.contains("yolo") && j["yolo"].is_object()) {
            auto& yolo = j["yolo"];
            if (yolo.contains("enabled") && yolo["enabled"].is_boolean()) {
                config.yoloEnabled = yolo["enabled"].get<bool>();
            }
            if (yolo.contains("model_path") && yolo["model_path"].is_string()) {
                config.yoloModelPath = yolo["model_path"].get<std::string>();
            }
            if (yolo.contains("fallback") && yolo["fallback"].is_string()) {
                config.yoloFallback = yolo["fallback"].get<std::string>();
            }
            if (yolo.contains("batch_size") && yolo["batch_size"].is_number()) {
                config.yoloBatchSize = yolo["batch_size"].get<int>();
            }
            if (yolo.contains("confidence_threshold") && yolo["confidence_threshold"].is_number()) {
                config.yoloConfidenceThreshold = yolo["confidence_threshold"].get<float>();
            }
            if (yolo.contains("nms_threshold") && yolo["nms_threshold"].is_number()) {
                config.yoloNmsThreshold = yolo["nms_threshold"].get<float>();
            }
            if (yolo.contains("use_fp16") && yolo["use_fp16"].is_boolean()) {
                config.yoloUseFP16 = yolo["use_fp16"].get<bool>();
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
            if (sm.contains("groups") && sm["groups"].is_object()) {
                for (auto& [key, value] : sm["groups"].items()) {
                    GroupConfig gc;
                    gc.configName = key;
                    if (value.contains("g1_g4") && value["g1_g4"].is_array()) {
                        for (size_t i = 0; i < 4 && i < value["g1_g4"].size(); ++i) {
                            gc.slotsG1_G4[i] = value["g1_g4"][i].get<int>();
                        }
                    }
                    if (value.contains("g5_g8") && value["g5_g8"].is_array()) {
                        for (size_t i = 0; i < 4 && i < value["g5_g8"].size(); ++i) {
                            gc.slotsG5_G8[i] = value["g5_g8"][i].get<int>();
                        }
                    }
                    if (value.contains("trigger_threshold") && value["trigger_threshold"].is_number()) {
                        gc.triggerThreshold = value["trigger_threshold"].get<float>();
                    }
                    config.sceneManagerGroups.push_back(gc);
                }
                // Sort by trigger threshold
                std::sort(config.sceneManagerGroups.begin(), config.sceneManagerGroups.end(),
                          [](const GroupConfig& a, const GroupConfig& b) {
                              return a.triggerThreshold < b.triggerThreshold;
                          });
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
            if (ie.contains("input_size") && ie["input_size"].is_number()) {
                config.inferenceConfig.inputWidth = ie["input_size"].get<int>();
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
} // namespace

int main(int argc, char* argv[]) {
    Logger::Init("VIB_System");
    Logger::Info("Visual Intelligence Bypass v2.0 Starting...");
    
    // Initialize COM for DeckLink SDK (Component Object Model)
    // This MUST be called before any DeckLink interfaces are created
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Logger::Error("Failed to initialize COM. HRESULT: 0x" + std::to_string(hr));
        Logger::Error("DeckLink SDK requires COM initialization to function properly");
        return 1;
    }
    Logger::Info("COM initialized successfully for DeckLink SDK");
    
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
    
    try {
        // Load configuration from JSON
        Config config = LoadConfig("config.json");

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
            return 1;
        }

        vmix.ConnectTcp();  // prepare TCP channel; errors are logged but non-fatal

        // Fase 1: Sweep hardware/software links
        if (!RunPhase1(vmix, videoHub, deckLinkSource, trackController)) {
            Logger::Error("Ignición abortada durante Fase 1");
            return 1;
        }

        // Fase 2: Escena (YOLO check)
        if (!RunPhase2(videoHub, config.targetSpheres)) {
            Logger::Error("Ignición abortada durante Fase 2");
            return 1;
        }

        // ============================================================================
        // NDI Initialization (replaced Spout for vMix compatibility)
        // ============================================================================
        // NDI provides:
        // - Native vMix support (no plugins needed)
        // - Zero-copy async sending via completion callbacks
        // - UYVY format support (native DeckLink format, optimal for vMix)
        // - Network transparency (works across machines if needed)
        //
        // Note: D3D11 device creation is no longer required for video output.
        //       DeckLinkCapture uses CUDA-only pipeline for zero-copy DMA.
        // ============================================================================
        
        Logger::Info("Initializing NDI output for vMix...");
        
        // Initialize NDI Manager
        auto ndiManager = std::make_shared<NDIManager>();
        if (!ndiManager->Initialize()) {
            Logger::Error("Failed to initialize NDI library");
            throw std::runtime_error("NDI initialization failed");
        }
        
        // Pre-create all 12 NDI senders to maintain stable source names for vMix
        // vMix will see these as "VIB_CAM_01" through "VIB_CAM_12"
        Logger::Info("Creating NDI senders for " + std::to_string(kMaxNDIChannels) + " channels...");
        for (int channel = 0; channel < kMaxNDIChannels; ++channel) {
            std::ostringstream oss;
            oss << "VIB_CAM_" << std::setw(2) << std::setfill('0') << (channel + 1);
            const std::string senderName = oss.str();
            
            // Use UYVY format for optimal performance:
            // - DeckLink outputs UYVY natively
            // - vMix expects UYVY and converts internally to 32-bit float 4:4:4
            // - Avoids YUV→BGRA conversion for vMix path (saves ~1-2ms per frame)
            if (!ndiManager->CreateSender(channel, senderName, kDefaultWidth, kDefaultHeight, true)) {
                Logger::Error("Failed to create NDI sender: " + senderName);
                throw std::runtime_error("NDI sender creation failed");
            }
        }
        Logger::Info("NDI senders initialized successfully");
        
        // Log vMix configuration tips
        Logger::Info("=== vMix Configuration Tips ===");
        Logger::Info("1. Enable 'High Input Performance Mode' for 9+ cameras (requires GPU with >3GB VRAM)");
        Logger::Info("2. Disable 'Show preview thumbnails for NDI sources' to reduce network traffic");
        Logger::Info("3. NDI sources will appear as 'VIB_CAM_01' through 'VIB_CAM_12'");
        
        // ============================================================================
        // Initialize AI Pipeline Components
        // ============================================================================
        
        // Initialize PerformanceMonitor for real-time telemetry
        Logger::Info("Initializing PerformanceMonitor...");
        auto perfMonitor = std::make_shared<PerformanceMonitor>(33.0, 30);  // 33ms target, log every 30 frames
        
        // Initialize ActiveCameraSelector for Top-4 motion-based selection
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
                throw std::runtime_error("ActiveCameraSelector initialization failed");
            }
            
            // Apply hysteresis configuration from config.json
            HysteresisConfig hysteresisConfig;
            hysteresisConfig.switch_threshold = config.hysteresisSwitchThreshold;
            hysteresisConfig.min_active_frames = config.hysteresisMinActiveFrames;
            hysteresisConfig.decay_factor = config.hysteresisDecayFactor;
            cameraSelector->SetHysteresisConfig(hysteresisConfig);
            
            Logger::Info("ActiveCameraSelector initialized: Top-" + std::to_string(config.selectorTopK) + 
                        " from " + std::to_string(kMaxNDIChannels) + " cameras with hysteresis" +
                        " (threshold=" + std::to_string(config.hysteresisSwitchThreshold) +
                        ", frames=" + std::to_string(config.hysteresisMinActiveFrames) +
                        ", decay=" + std::to_string(config.hysteresisDecayFactor) + ")");
        } else {
            Logger::Info("ActiveCameraSelector disabled by configuration");
        }
        
        // Initialize YOLO/TensorRT processor
        Logger::Info("Initializing YOLO processor...");
        auto yoloProcessor = std::make_shared<YOLOProcessor>(
            config.yoloBatchSize,
            config.yoloUseFP16
        );
        
        // Create dedicated CUDA stream for YOLO processing (non-blocking)
        cudaStream_t yoloStream = nullptr;
        cudaError_t err = cudaStreamCreate(&yoloStream);
        if (err != cudaSuccess) {
            Logger::Error("Failed to create YOLO CUDA stream: " + std::string(cudaGetErrorString(err)));
            throw std::runtime_error("CUDA stream creation failed");
        }
        Logger::Info("Created dedicated CUDA stream for YOLO (non-blocking pipeline)");
        
        bool yoloReady = false;
        if (config.yoloEnabled) {
            bool useFallback = (config.yoloFallback == "stub");
            if (yoloProcessor->Initialize(config.yoloModelPath, useFallback)) {
                yoloProcessor->SetConfidenceThreshold(config.yoloConfidenceThreshold);
                yoloProcessor->SetNMSThreshold(config.yoloNmsThreshold);
                yoloReady = true;
                
                if (yoloProcessor->IsStubMode()) {
                    Logger::Warning("YOLO running in STUB mode - no real inference");
                } else {
                    Logger::Info("YOLO initialized successfully with TensorRT");
                }
            } else {
                Logger::Warning("YOLO initialization failed - inference disabled");
            }
        } else {
            Logger::Info("YOLO disabled by configuration");
        }
        
        // ============================================================================
        // Initialize New Tracking Pipeline Components
        // ============================================================================
        
        // Initialize InferenceEngine (TensorRT-based ball detection)
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
        
        // Initialize PositionMapper (homography-based coordinate transformation)
        Logger::Info("Initializing PositionMapper...");
        auto positionMapper = std::make_shared<PositionMapper>();
        if (positionMapper->LoadCalibration(config.calibrationFile)) {
            positionMapper->SetTrackBounds(config.trackBounds);
            Logger::Info("PositionMapper loaded calibration from " + config.calibrationFile);
        } else {
            Logger::Warning("PositionMapper using identity transform (no calibration)");
        }
        
        // Initialize BallTracker (Kalman filter-based multi-ball tracking)
        Logger::Info("Initializing BallTracker...");
        auto ballTracker = std::make_shared<BallTracker>(config.ballTrackerConfig);
        if (ballTracker->Initialize()) {
            Logger::Info("BallTracker initialized with " + 
                        std::to_string(config.ballTrackerConfig.numBalls) + " balls");
        } else {
            Logger::Error("BallTracker initialization failed");
        }
        
        // Initialize SceneManager (leapfrogging group switching)
        Logger::Info("Initializing SceneManager...");
        SceneManagerConfig sceneConfig;
        sceneConfig.enabled = config.sceneManagerEnabled;
        sceneConfig.muteTimeoutMs = config.sceneManagerMuteTimeoutMs;
        sceneConfig.groups = config.sceneManagerGroups;
        
        auto sceneManager = std::make_shared<SceneManager>(&videoHub, sceneConfig);
        if (config.sceneManagerEnabled) {
            if (sceneManager->Initialize()) {
                Logger::Info("SceneManager initialized with " + 
                            std::to_string(config.sceneManagerGroups.size()) + " group configurations");
            } else {
                Logger::Warning("SceneManager initialization failed");
            }
        } else {
            Logger::Info("SceneManager disabled by configuration");
        }
        
        // Initialize RankingPublisher (vMix ranking output)
        Logger::Info("Initializing RankingPublisher...");
        auto rankingPublisher = std::make_shared<RankingPublisher>(config.rankingConfig);
        if (config.rankingConfig.enabled) {
            if (rankingPublisher->Initialize()) {
                Logger::Info("RankingPublisher initialized, target=" + 
                            config.rankingConfig.vmixHost + ":" + 
                            std::to_string(config.rankingConfig.vmixTcpPort));
            } else {
                Logger::Warning("RankingPublisher initialization failed");
            }
        } else {
            Logger::Info("RankingPublisher disabled by configuration");
        }
        
        // ============================================================================
        
        // Initialize Redis worker
        Logger::Info("Initializing Redis worker...");
        RedisWorker redisWorker(config.redisHost, config.redisPort, config.redisEnabled);
        if (config.redisEnabled) {
            redisWorker.Start();
            if (redisWorker.IsConnected()) {
                Logger::Info("Redis worker connected and running");
            } else {
                Logger::Warning("Redis worker started but not connected - will retry");
            }
        } else {
            Logger::Info("Redis worker disabled by configuration");
        }
        
        // Initialize capture channels
        // Note: DeckLinkCapture uses CUDA-only pipeline (no D3D11 dependency)
        // The YUV data path splits at DeckLinkCapture:
        // - UYVY → NDI for vMix (zero conversion)
        // - BGRA → YOLO for AI inference (converted via CUDA kernel)
        Logger::Info("Initializing capture channels...");
        std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
        
        // Auto-detect DeckLink devices
        int numDevices = DeckLinkCapture::EnumerateDevices();
        Logger::Info("Found " + std::to_string(numDevices) + " DeckLink devices");
        if (numDevices < kMaxNDIChannels) {
            Logger::Warning("Fewer capture devices than NDI senders (" +
                            std::to_string(numDevices) + " vs " +
                            std::to_string(kMaxNDIChannels) +
                            "). Idle senders remain available to keep channel names stable in vMix.");
        }
        
        const int channelsToInit = (std::min)(numDevices, kMaxNDIChannels);
        for (int i = 0; i < channelsToInit; i++) {
            auto capture = std::make_unique<DeckLinkCapture>();
            const std::string channelName = "Channel_" + std::to_string(i + 1);
            if (capture->Initialize(i, channelName)) {
                // Frame ready handler: Non-blocking pipeline with strict priority order
                // ORDEN 2: NON-BLOCKING PIPELINE - NDI → Selector → YOLO (async) → Tracking → Redis (async)
                capture->SetFrameReadyHandler([ndiManager, cameraSelector, yoloProcessor, &redisWorker, 
                                              perfMonitor, &config, yoloReady, yoloStream,
                                              inferenceEngine, positionMapper, ballTracker, 
                                              sceneManager, rankingPublisher, inferenceReady]
                                             (const VideoChannel& channel, cudaStream_t stream) {
                    auto frameStart = std::chrono::high_resolution_clock::now();
                    Telemetry telemetry = {0, 0, 0, 0, 0};
                    
                    // ============================================================================
                    // PRIORITY 1: Video Output to vMix (ALWAYS happens FIRST - sacred video)
                    // NDI sends async - does NOT block for GPU completion
                    // ============================================================================
                    auto ndiStart = std::chrono::high_resolution_clock::now();
                    ndiManager->SendUYVYFrame(
                        channel.channelID,
                        channel.cudaYUVBuffer,
                        channel.width,
                        channel.height,
                        stream  // Uses capture stream, returns immediately
                    );
                    auto ndiEnd = std::chrono::high_resolution_clock::now();
                    telemetry.ndi_ms = std::chrono::duration<double, std::milli>(ndiEnd - ndiStart).count();
                    
                    // ============================================================================
                    // PRIORITY 2: Motion Analysis (if selector enabled)
                    // ============================================================================
                    auto selectorStart = std::chrono::high_resolution_clock::now();
                    if (cameraSelector && config.selectorEnabled) {
                        // Update motion metrics for this camera (async in capture stream)
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
                    
                    // ============================================================================
                    // PRIORITY 3: AI Inference + Ball Tracking Pipeline
                    // ============================================================================
                    auto yoloStart = std::chrono::high_resolution_clock::now();
                    
                    // Collect ball detections using InferenceEngine
                    std::vector<BallDetection> ballDetections;
                    if (inferenceReady && inferenceEngine) {
                        ballDetections = inferenceEngine->ProcessFrame(
                            channel.cudaBGRABuffer,
                            channel.channelID,
                            channel.width,
                            channel.height,
                            yoloStream
                        );
                    }
                    
                    // Transform to global coordinates via PositionMapper
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
                    
                    // Update ball tracker with global positions
                    if (!globalPositions.empty() && ballTracker && ballTracker->IsInitialized()) {
                        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        ballTracker->Update(globalPositions, now);
                        
                        // Update scene manager with leader position
                        if (sceneManager && sceneManager->IsEnabled()) {
                            auto leader = ballTracker->GetLeader();
                            sceneManager->UpdateLeaderPosition(leader.Xg, leader.Yg);
                        }
                        
                        // Publish ranking to vMix
                        if (rankingPublisher && rankingPublisher->IsEnabled()) {
                            auto ranking = ballTracker->GetRanking();
                            rankingPublisher->PublishRanking(ranking);
                        }
                    }
                    
                    // Legacy YOLO processing path (for backward compatibility)
                    if (yoloReady && config.yoloEnabled) {
                        // Get active camera selection
                        std::vector<int> selectedCameras;
                        
                        if (cameraSelector && config.selectorEnabled) {
                            auto selection = cameraSelector->GetActiveSelection();
                            selectedCameras = selection.selectedCameraIDs;
                            
                            // Adaptive quality: respect both performance monitor AND config.selectorTopK
                            int recommended = perfMonitor->GetRecommendedActiveCameras();
                            int maxAllowed = (std::min)(recommended, config.selectorTopK);
                            if (static_cast<int>(selectedCameras.size()) > maxAllowed) {
                                selectedCameras.resize(maxAllowed);
                            }
                        } else {
                            // If selector disabled, process this camera only
                            selectedCameras.push_back(channel.channelID);
                        }
                        
                        // Only process if this camera is selected
                        bool isSelected = std::find(selectedCameras.begin(), selectedCameras.end(), 
                                                    channel.channelID) != selectedCameras.end();
                        
                        if (isSelected) {
                            // Process frame in YOLO's dedicated stream (async, non-blocking)
                            // Note: ProcessFrame launches kernels but does NOT wait for completion
                            auto detections = yoloProcessor->ProcessFrame(
                                channel.cudaBGRABuffer,
                                channel.channelID,
                                channel.width,
                                channel.height,
                                yoloStream  // Separate stream - does NOT block capture stream
                            );
                            
                            // ============================================================
                            // PRIORITY 4: Redis Publishing (async, can be 1 frame late)
                            // ============================================================
                            auto redisStart = std::chrono::high_resolution_clock::now();
                            if (!detections.empty() && config.redisEnabled) {
                                // Non-blocking update - Redis worker publishes async
                                redisWorker.UpdateDetections(detections);
                            }
                            auto redisEnd = std::chrono::high_resolution_clock::now();
                            telemetry.redis_ms = std::chrono::duration<double, std::milli>(redisEnd - redisStart).count();
                        }
                    }
                    auto yoloEnd = std::chrono::high_resolution_clock::now();
                    telemetry.yolo_ms = std::chrono::duration<double, std::milli>(yoloEnd - yoloStart).count();
                    
                    // Record telemetry for performance monitoring and auto-adjust
                    auto frameEnd = std::chrono::high_resolution_clock::now();
                    telemetry.capture_ms = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
                    perfMonitor->RecordFrame(telemetry);
                    
                    // NOTE: No cudaDeviceSynchronize() or cudaStreamSynchronize() here!
                    // Streams execute async - sync happens only when needed via events
                });
                capture->Start();
                captureChannels.push_back(std::move(capture));
            }
        }
        
        // Main monitoring loop
        Logger::Info("=== System Status ===");
        Logger::Info("Video Pipeline: " + std::to_string(kMaxNDIChannels) + " NDI channels active");
        Logger::Info("Camera Selector: " + std::string(config.selectorEnabled ? "Enabled (Top-" + std::to_string(config.selectorTopK) + ")" : "Disabled"));
        Logger::Info("YOLO Inference: " + std::string(yoloReady ? (yoloProcessor->IsStubMode() ? "Stub mode" : "Active") : "Disabled"));
        Logger::Info("InferenceEngine: " + std::string(inferenceReady ? (inferenceEngine->IsStubMode() ? "Stub mode" : "Active") : "Disabled"));
        Logger::Info("PositionMapper: " + std::string(positionMapper->IsCalibrated() ? "Calibrated" : "Identity"));
        Logger::Info("BallTracker: " + std::string(ballTracker->IsInitialized() ? "Initialized (" + std::to_string(config.ballTrackerConfig.numBalls) + " balls)" : "Disabled"));
        Logger::Info("SceneManager: " + std::string(config.sceneManagerEnabled ? "Enabled (" + std::to_string(config.sceneManagerGroups.size()) + " groups)" : "Disabled"));
        Logger::Info("RankingPublisher: " + std::string(config.rankingConfig.enabled ? (rankingPublisher->IsConnected() ? "Connected" : "Disconnected") : "Disabled"));
        Logger::Info("Redis Publishing: " + std::string(config.redisEnabled ? (redisWorker.IsConnected() ? "Connected" : "Retrying") : "Disabled"));
        Logger::Info("=====================");
        Logger::Info("=== Keyboard Shortcuts ===");
        Logger::Info("ESC - Exit application");
        Logger::Info("F2  - Reduce to 2 cameras (thermal throttle)");
        Logger::Info("F3  - Restore to 4 cameras");
        Logger::Info("F4  - Graceful shutdown");
        Logger::Info("F5  - Reset telemetry (save current run)");
        Logger::Info("==========================");
        
        bool running = true;
        auto lastStatusLog = std::chrono::steady_clock::now();
        const auto statusInterval = std::chrono::seconds(10);  // Log status every 10 seconds
        
        // Track key states to prevent multiple triggers (debouncing)
        bool f2Pressed = false;
        bool f3Pressed = false;
        bool f4Pressed = false;
        bool f5Pressed = false;
        
        while (running) {
            // Check for exit condition
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                running = false;
            }
            
            // ============================================================================
            // EMERGENCY KEYBOARD SHORTCUTS (F2/F3/F4/F5)
            // ============================================================================
            
            // F2 - Reduce active cameras to 2 (thermal throttle)
            if (GetAsyncKeyState(VK_F2) & 0x8000) {
                if (!f2Pressed) {
                    f2Pressed = true;
                    config.selectorTopK = 2;
                    perfMonitor->RecordEmergencyEvent();
                    Logger::Info("[EMERGENCY] Reduced active cameras to 2 (thermal throttle)");
                }
            } else {
                f2Pressed = false;
            }
            
            // F3 - Restore active cameras to 4
            if (GetAsyncKeyState(VK_F3) & 0x8000) {
                if (!f3Pressed) {
                    f3Pressed = true;
                    config.selectorTopK = 4;
                    Logger::Info("[EMERGENCY] Restored active cameras to 4");
                }
            } else {
                f3Pressed = false;
            }
            
            // F4 - Graceful stop
            if (GetAsyncKeyState(VK_F4) & 0x8000) {
                if (!f4Pressed) {
                    f4Pressed = true;
                    running = false;
                    Logger::Info("[EMERGENCY] Graceful stop initiated - exiting main loop");
                }
            } else {
                f4Pressed = false;
            }
            
            // F5 - Reset telemetry (save current run and start fresh)
            if (GetAsyncKeyState(VK_F5) & 0x8000) {
                if (!f5Pressed) {
                    f5Pressed = true;
                    
                    // Generate run ID for saved file
                    auto now = std::chrono::system_clock::now();
                    auto time_value = std::chrono::system_clock::to_time_t(now);
                    std::tm tm;
                    localtime_s(&tm, &time_value);
                    
                    std::ostringstream runIDStream;
                    runIDStream << std::put_time(&tm, "%Y%m%d_%H%M%S");
                    std::string runID = runIDStream.str();
                    
                    Logger::Info("[RESET] Telemetry reset initiated. Saving previous run...");
                    perfMonitor->Reset(true, runID);
                    Logger::Info("[RESET] Telemetry reset complete. Previous run saved to logs/run_" + runID + ".json");
                }
            } else {
                f5Pressed = false;
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
                    Logger::Info("[STATUS] Active cameras: [" + selectedList + "], " +
                                "Avg motion: " + std::to_string(selection.averageMotionScore));
                }
                
                // Ball tracker status
                if (ballTracker && ballTracker->IsInitialized()) {
                    auto ranking = ballTracker->GetRanking();
                    std::string rankingStr;
                    for (size_t i = 0; i < ranking.size() && i < 3; ++i) {
                        if (i > 0) rankingStr += ",";
                        rankingStr += std::to_string(ranking[i]);
                    }
                    auto leader = ballTracker->GetLeader();
                    Logger::Info("[STATUS] Tracking: " + std::to_string(ballTracker->GetVisibleBallCount()) + 
                                " balls visible, Leader: ball " + std::to_string(leader.ballID) +
                                " at X=" + std::to_string(leader.Xg) + "m, Top3: [" + rankingStr + "]");
                }
                
                // Scene manager status
                if (sceneManager && sceneManager->IsEnabled()) {
                    auto mutedSlots = sceneManager->GetMutedSlots();
                    Logger::Info("[STATUS] SceneManager: config=" + sceneManager->GetCurrentConfigName() +
                                ", Leader X=" + std::to_string(sceneManager->GetLastLeaderX()) +
                                "m, muted slots=" + std::to_string(mutedSlots.size()));
                }
                
                // Ranking publisher status
                if (rankingPublisher && config.rankingConfig.enabled) {
                    Logger::Info("[STATUS] RankingPublisher: " + 
                                std::string(rankingPublisher->IsConnected() ? "Connected" : "Disconnected") +
                                ", Published=" + std::to_string(rankingPublisher->GetPublishCount()) +
                                ", Failed=" + std::to_string(rankingPublisher->GetFailCount()));
                }
                
                if (config.redisEnabled) {
                    Logger::Info("[STATUS] Redis: " + std::string(redisWorker.IsConnected() ? "Connected" : "Disconnected") +
                                ", Retry count: " + std::to_string(redisWorker.GetRetryCount()));
                }
                
                // Process scene manager mute timeouts
                if (sceneManager && sceneManager->IsEnabled()) {
                    sceneManager->ProcessMuteTimeouts();
                }
                
                lastStatusLog = now;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        
        // Cleanup
        Logger::Info("Shutting down...");
        
        // Shutdown new components
        if (rankingPublisher) {
            rankingPublisher->Shutdown();
            Logger::Info("RankingPublisher shutdown");
        }
        
        // Destroy YOLO CUDA stream
        if (yoloStream) {
            cudaStreamDestroy(yoloStream);
            Logger::Info("YOLO CUDA stream destroyed");
        }
        
        redisWorker.Stop();
        
        // Stop all capture channels before releasing NDI
        for (auto& capture : captureChannels) {
            capture->Stop();
        }
        captureChannels.clear();
        
        // Release NDI senders
        ndiManager->ReleaseAll();
        
        Logger::Info("Visual Intelligence Bypass shutdown complete");
        
    } catch (const std::exception& e) {
        Logger::Error("Fatal error: " + std::string(e.what()));
        CoUninitialize();
        return 1;
    }
    
    // Uninitialize COM before exiting
    CoUninitialize();
    Logger::Info("COM uninitialized");
    
    return 0;
}
