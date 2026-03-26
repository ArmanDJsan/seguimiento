/**
 * RedisWorker.cpp
 * 
 * Implementation of Redis worker for asynchronous updates
 */

#include "RedisWorker.h"
#include "../utils/Logger.h"
#include <chrono>
#include <sstream>
#include <iomanip>

RedisWorker::RedisWorker(const std::string& host, int port)
    : m_host(host)
    , m_port(port)
    , m_running(false)
{
    Logger::Info("RedisWorker created for " + host + ":" + std::to_string(port));
}

RedisWorker::~RedisWorker() {
    Stop();
}

void RedisWorker::Start() {
    if (m_running) {
        Logger::Warning("RedisWorker already running");
        return;
    }
    
    Logger::Info("Starting Redis worker thread...");
    
    // TODO: Connect to Redis
    // m_redisClient = new redis::Redis(m_host, m_port);
    
    m_running = true;
    m_workerThread = std::thread(&RedisWorker::WorkerLoop, this);
    
    Logger::Info("Redis worker started successfully");
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
    
    // TODO: Disconnect from Redis
    // if (m_redisClient) {
    //     delete m_redisClient;
    //     m_redisClient = nullptr;
    // }
    
    Logger::Info("Redis worker stopped");
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
        
        // Update Redis
        // TODO: Send to Redis
        // m_redisClient->set("VMIX_DATA_STREAM", jsonData);
        // m_redisClient->publish("VMIX_UPDATE_CHANNEL", "new_data");
        
        Logger::Debug("Redis updated with " + std::to_string(currentDetections.size()) + 
                     " detections");
        
        // Sleep to maintain 60Hz rate
        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart);
        
        if (elapsed < frameInterval) {
            std::this_thread::sleep_for(frameInterval - elapsed);
        }
    }
    
    Logger::Info("Redis worker loop ended");
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
