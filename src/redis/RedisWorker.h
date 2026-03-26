/**
 * RedisWorker.h
 * 
 * Asynchronous worker thread for updating Redis with detection data
 * Decouples video processing from metadata updates
 * 
 * Philosophy: Set and forget - C++ produces, vMix consumes independently
 */

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "../ai/YOLOProcessor.h"

/**
 * Redis worker for asynchronous data publishing
 * Runs at 60Hz (16ms intervals) synchronized with video frame rate
 */
class RedisWorker {
public:
    RedisWorker(const std::string& host, int port);
    ~RedisWorker();
    
    // Start/Stop the worker thread
    void Start();
    void Stop();
    
    // Update the detection array (called by main thread)
    void UpdateDetections(const std::vector<Detection>& detections);
    
    // Check if worker is running
    bool IsRunning() const { return m_running; }
    
private:
    // Worker thread function
    void WorkerLoop();
    
    // Serialize detections to JSON
    std::string SerializeToJSON(const std::vector<Detection>& detections);
    
    // Redis connection info
    std::string m_host;
    int m_port;
    
    // Worker thread
    std::thread m_workerThread;
    std::atomic<bool> m_running;
    
    // Shared detection array
    std::vector<Detection> m_detections;
    std::mutex m_dataMutex;
    
    // TODO: Redis connection handle
    // redis::Redis* m_redisClient;
};
