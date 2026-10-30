/**
 * Config.h
 * 
 * Estructuras de configuración compartidas para el sistema VIB
 */

#pragma once

#include <string>
#include <vector>
#include <array>
#include <cstdint>

#include "../scene/SceneManager.h"       // GroupConfig
#include "../ai/InferenceEngine.h"       // InferenceEngineConfig
#include "../tracking/PositionMapper.h"  // TrackBounds
#include "../tracking/BallTracker.h"     // BallTrackerConfig
#include "../output/RankingPublisher.h"  // RankingPublisherConfig
#include "../output/ArrivalEmitter.h"    // ArrivalEndpointConfig
#include "../verification/ArrivalOrderStabilizer.h"  // ArrivalStabilizerConfig
#include "../measure/AnimationPublisher.h" // AnimationPublisherConfig
#include "../control/PTZController.h"    // PTZCameraConfig, PTZTrackingConfig

/**
 * Choreography configuration
 */
struct ChoreographyConfig {
    bool enabled;
    std::string scriptPath;
    bool autoStart;
    bool continueOnError;
    bool vmixRequired;
    std::string triggerKey;
    bool debug;
    
    ChoreographyConfig()
        : enabled(false)
        , scriptPath("")
        , autoStart(false)
        , continueOnError(true)
        , vmixRequired(false)
        , triggerKey("F12")
        , debug(false)
    {}
};

/**
 * PTZ Cameras configuration
 */
struct PTZCamerasConfig {
    bool enabled;
    std::vector<PTZ::PTZCameraConfig> cameras;
    
    PTZCamerasConfig()
        : enabled(false)
    {}
};

/**
 * Configuración principal del sistema VIB
 */
struct Config {
    // VideoHub configuration
    std::string videohubIp;
    uint16_t videohubPort;
    
    // ESP32 configuration
    std::string esp32Ip;
    uint16_t esp32Port;
    
    // General
    int targetSpheres;
    
    // Redis configuration
    bool redisEnabled;
    std::string redisHost;
    uint16_t redisPort;
    
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
    SceneMode sceneManagerMode;
    // Trigger mode determines how scene changes are initiated:
    // - THRESHOLD (legacy): Based on leader X position thresholds
    // - EVENT (default): Based on zone crossing events from ZoneChecker/EventGenerator
    //   EVENT mode is the new architecture providing more precise control
    TriggerMode sceneManagerTriggerMode;
    int sceneManagerEventCooldownMs;      // Minimum ms between event-triggered changes
    int sceneManagerHysteresisFrames;     // Frames required to confirm zone change
    ManualKeysConfig sceneManagerManualKeys;
    std::vector<GroupConfig> sceneManagerGroups;
    
    // Radar routing configuration - maps RADAR_01-04 to VideoHub outputs
    // When enabled, radar routing is applied each time a scene config (config_a/b/c) is applied
    // This ensures radars are always correctly routed for stability
    bool radarRoutingEnabled;                 // Enable/disable automatic radar routing
    std::array<int, 4> radarOutputSlots;      // VideoHub output indices for RADAR_01-04
                                              // Uses 0-based VideoHub protocol indices
                                              // Default: {8, 9, 10, 11} meaning physical outputs 9-12 on the device
    
    // NDI configuration - when disabled, vMix captures directly from VideoHub
    bool ndiEnabled;
    
    // Inference engine configuration
    InferenceEngineConfig inferenceConfig;

    // Dedicated "llegada" (arrival) inference engine — second engine, does not touch inferenceConfig.
    // Used by menu option 11 to determine arrival order from the finish-line camera.
    InferenceEngineConfig llegadaInferenceConfig;

    // Arrival order confirmation (menu option 11): stabilization criteria + emission endpoint.
    ArrivalStabilizerConfig arrivalStabilizerConfig;
    ArrivalEndpointConfig arrivalEndpointConfig;

    // Position mapper configuration
    std::string calibrationFile;
    TrackBounds trackBounds;
    
    // Ball tracker configuration
    BallTrackerConfig ballTrackerConfig;
    
    // Ranking publisher configuration
    RankingPublisherConfig rankingConfig;
    
    // Choreography configuration
    ChoreographyConfig choreographyConfig;
    
    // Salida de animacion del medidor por evento (bloque "animation"):
    // un datagrama UDP por punto de medicion resuelto.
    AnimationPublisherConfig animationConfig;
    
    // PTZ cameras configuration
    PTZCamerasConfig ptzCamerasConfig;
    
    // PTZ tracking configuration
    PTZ::PTZTrackingConfig ptzTrackingConfig;
    
    // When false, PTZ cameras execute choreography/presets only and do NOT
    // perform automatic centroid-based pan/tilt/zoom tracking.
    // Set ptz_tracking.enabled = false in config.json to disable auto-tracking.
    bool ptzAutoTrackingEnabled;
    
    // Constructor con valores por defecto
    Config() 
        : videohubIp("192.168.1.50")
        , videohubPort(9990)
        , esp32Ip("192.168.88.114")
        , esp32Port(80)
        , targetSpheres(10)
        , redisEnabled(true)
        , redisHost("127.0.0.1")
        , redisPort(6379)
        , selectorEnabled(true)
        , selectorTopK(4)
        , selectorMotionThreshold(0.05f)
        , selectorEdgeMargin(0.1f)
        , hysteresisSwitchThreshold(0.15f)
        , hysteresisMinActiveFrames(10)
        , hysteresisDecayFactor(0.98f)
        , sceneManagerEnabled(true)
        , sceneManagerMuteTimeoutMs(200)
        , sceneManagerMode(SceneMode::AUTO)
        , sceneManagerTriggerMode(TriggerMode::EVENT)
        , sceneManagerEventCooldownMs(500)
        , sceneManagerHysteresisFrames(3)
        , radarRoutingEnabled(true)
        , radarOutputSlots{8, 9, 10, 11}  // Default: RADAR_01-04 -> outputs 8-11
        , ndiEnabled(false)
        , calibrationFile("config/calibration.json")
        , ptzAutoTrackingEnabled(true)
    {
        // Defaults for the dedicated arrival (llegada) engine.
        // Trained at 1088x1920 (native TensorRT input), so skipResize=false resizes the
        // 1080p camera frame up to 1088. Batch 1 (static engine). Lower confidence (0.3)
        // to keep the dark sphere (bola_8) and avoid noise (validation FPs ~0.02-0.03).
        llegadaInferenceConfig.modelPath = "models/llegada_best.engine";
        llegadaInferenceConfig.batchSize = 1;
        llegadaInferenceConfig.inputWidth = 1920;
        llegadaInferenceConfig.inputHeight = 1088;
        llegadaInferenceConfig.confidenceThreshold = 0.3f;
        llegadaInferenceConfig.nmsThreshold = 0.4f;
        llegadaInferenceConfig.useFP16 = true;
        llegadaInferenceConfig.numClasses = 10;
        llegadaInferenceConfig.skipResize = false;
        llegadaInferenceConfig.maxDetections = 300;
        llegadaInferenceConfig.debugYoloEnabled = false;
        llegadaInferenceConfig.debugYoloInterval = 300;
    }
};
