/**
 * Config.h
 * 
 * Estructuras de configuración compartidas para el sistema VIB
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "../scene/SceneManager.h"       // GroupConfig
#include "../ai/InferenceEngine.h"       // InferenceEngineConfig
#include "../tracking/PositionMapper.h"  // TrackBounds
#include "../tracking/BallTracker.h"     // BallTrackerConfig
#include "../output/RankingPublisher.h"  // RankingPublisherConfig

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
    ManualKeysConfig sceneManagerManualKeys;
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
    
    // Choreography configuration
    ChoreographyConfig choreographyConfig;
    
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
        , calibrationFile("config/calibration.json")
    {}
};
