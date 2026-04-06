/**
 * RedisWorker.cpp
 * 
 * Implementation of Redis worker with resilient connection handling
 * Video flow continues even if Redis fails - graceful degradation
 */

#include "RedisWorker.h"
#include "../utils/Logger.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <thread>

// Redis C++ client - conditionally compile
#if __has_include("sw/redis++/redis++.h")
    #include "sw/redis++/redis++.h"
    #define HAS_REDIS_CLIENT 1
#else
    #define HAS_REDIS_CLIENT 0
    // Stub when redis++ not available
    namespace sw { namespace redis { class Redis {}; } }
#endif

RedisWorker::RedisWorker(const std::string& host, int port, bool enabled)
    : m_host(host)
    , m_port(port)
    , m_enabled(enabled)
    , m_running(false)
    , m_connected(false)
    , m_retryCount(0)
    , m_redisClient(nullptr)
{
    if (!m_enabled) {
        Logger::Info("RedisWorker created but disabled by configuration");
    } else {
        Logger::Info("RedisWorker created for " + host + ":" + std::to_string(port));
    }
}

RedisWorker::~RedisWorker() {
    Stop();
}

void RedisWorker::Start() {
    if (!m_enabled) {
        Logger::Info("RedisWorker disabled - skipping start");
        return;
    }
    
    if (m_running) {
        Logger::Warning("RedisWorker already running");
        return;
    }
    
    Logger::Info("Starting Redis worker thread...");
    
    // Attempt initial connection (non-blocking)
    if (!ConnectToRedis()) {
        Logger::Warning("Initial Redis connection failed - will retry in background");
    }
    
    m_running = true;
    m_workerThread = std::thread(&RedisWorker::WorkerLoop, this);
    
    Logger::Info("Redis worker started");
}

void RedisWorker::Stop() {
    if (!m_running) {
        return;
    }
    
    Logger::Info("Stopping Redis worker...");
    m_running = false;
    
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    
    DisconnectFromRedis();
    Logger::Info("Redis worker stopped");
}

bool RedisWorker::ConnectToRedis() {
#if HAS_REDIS_CLIENT
    std::lock_guard<std::mutex> lock(m_redisMutex);
    
    try {
        std::string connectionStr = "tcp://" + m_host + ":" + std::to_string(m_port);
        m_redisClient = new sw::redis::Redis(connectionStr);
        
        // Test connection with ping
        m_redisClient->ping();
        
        m_connected = true;
        m_retryCount = 0;
        Logger::Info("Connected to Redis successfully");
        return true;
        
    } catch (const std::exception& e) {
        Logger::Error("Redis connection failed: " + std::string(e.what()));
        if (m_redisClient) {
            delete m_redisClient;
            m_redisClient = nullptr;
        }
        m_connected = false;
        return false;
    }
#else
    Logger::Warning("Redis client library not available");
    m_connected = false;
    return false;
#endif
}

void RedisWorker::DisconnectFromRedis() {
    std::lock_guard<std::mutex> lock(m_redisMutex);
    
    if (m_redisClient) {
#if HAS_REDIS_CLIENT
        delete m_redisClient;
#endif
        m_redisClient = nullptr;
    }
    
    m_connected = false;
}

bool RedisWorker::Reconnect() {
    if (m_retryCount >= MAX_RETRY_ATTEMPTS) {
        Logger::Warning("Max retry attempts reached - giving up on Redis connection");
        return false;
    }
    
    Logger::Info("Attempting to reconnect to Redis (attempt " + 
                 std::to_string(m_retryCount + 1) + "/" + std::to_string(MAX_RETRY_ATTEMPTS) + ")");
    
    DisconnectFromRedis();
    std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
    
    m_retryCount++;
    return ConnectToRedis();
}

void RedisWorker::UpdateDetections(const std::vector<Detection>& detections) {
    std::lock_guard<std::mutex> lock(m_dataMutex);
    m_detections = detections;
}

void RedisWorker::WorkerLoop() {
    Logger::Info("Redis worker loop started");
    
    const auto frameInterval = std::chrono::milliseconds(16); // 60Hz
    
    while (m_running) {
        auto loopStart = std::chrono::steady_clock::now();
        
        // Get current detections
        std::vector<Detection> currentDetections;
        {
            std::lock_guard<std::mutex> lock(m_dataMutex);
            currentDetections = m_detections;
        }
        
        // Serialize to JSON
        std::string jsonData = SerializeToJSON(currentDetections);
        
        // Attempt to publish
        if (!PublishDetections(jsonData)) {
            // Connection lost or error - try to reconnect
            if (!Reconnect()) {
                // Failed to reconnect - continue loop but skip publishing
                Logger::Debug("Redis unavailable - skipping publish (video continues)");
            }
        }
        
        // Sleep to maintain 60Hz rate
        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart);
        
        if (elapsed < frameInterval) {
            std::this_thread::sleep_for(frameInterval - elapsed);
        }
    }
    
    Logger::Info("Redis worker loop ended");
}

bool RedisWorker::PublishDetections(const std::string& jsonData) {
    if (!m_connected) {
        return false;
    }
    
#if HAS_REDIS_CLIENT
    std::lock_guard<std::mutex> lock(m_redisMutex);
    
    if (!m_redisClient) {
        return false;
    }
    
    try {
        // Set key with detection data
        m_redisClient->set("VMIX_DATA_STREAM", jsonData);
        
        // Publish notification
        m_redisClient->publish("vmix_detections", "new_data");
        
        // Reset retry count on success
        if (m_retryCount > 0) {
            Logger::Info("Redis connection restored");
            m_retryCount = 0;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        Logger::Error("Redis publish failed: " + std::string(e.what()));
        m_connected = false;
        return false;
    }
#else
    Logger::Debug("Redis stub: would publish " + std::to_string(jsonData.length()) + " bytes");
    return true;
#endif
}

std::string RedisWorker::SerializeToJSON(const std::vector<Detection>& detections) {
    std::ostringstream json;
    
    json << "{\"detections\":[";
    
    for (size_t i = 0; i < detections.size(); i++) {
        const auto& det = detections[i];
        
        json << "{";
        json << "\"cameraID\":" << det.cameraID << ",";
        json << "\"objectID\":" << det.objectID << ",";
        json << "\"x\":" << std::fixed << std::setprecision(4) << det.x << ",";
        json << "\"y\":" << std::fixed << std::setprecision(4) << det.y << ",";
        json << "\"width\":" << std::fixed << std::setprecision(4) << det.width << ",";
        json << "\"height\":" << std::fixed << std::setprecision(4) << det.height << ",";
        json << "\"label\":\"" << det.label << "\",";
        json << "\"confidence\":" << std::fixed << std::setprecision(2) << det.confidence << ",";
        json << "\"timestamp\":" << det.timestamp;
        json << "}";
        
        if (i < detections.size() - 1) {
            json << ",";
        }
    }
    
    json << "],";
    json << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    json << "}";
    
    return json.str();
}
