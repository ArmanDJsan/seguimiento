/**
 * RedisWorker.h
 * 
 * Asynchronous worker thread for updating Redis with detection data
 * Decouples video processing from metadata updates
 * 
 * Philosophy: Redis is optional - video flow continues even if Redis fails
 * Implements retry logic and graceful degradation
 */

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include "../ai/YOLOProcessor.h"

// Forward declaration for Redis client
namespace sw {
    namespace redis {
        class Redis;
    }
}

/**
 * Redis worker for asynchronous data publishing
 * Runs at 60Hz (16ms intervals) synchronized with video frame rate
 * 
 * Feature flag support: Can be disabled via configuration
 */
class RedisWorker {
public:
    /**
     * Constructor
     * @param host Redis server host
     * @param port Redis server port
     * @param enabled Enable/disable Redis publishing
     */
    RedisWorker(const std::string& host, int port, bool enabled = true);
    ~RedisWorker();
    
    // Start/Stop the worker thread
    void Start();
    void Stop();
    
    // Update the detection array (called by main thread)
    void UpdateDetections(const std::vector<Detection>& detections);
    
    // Check if worker is running
    bool IsRunning() const { return m_running; }
    
    // Check if Redis is enabled
    bool IsEnabled() const { return m_enabled; }
    
    // Check if currently connected to Redis
    bool IsConnected() const { return m_connected; }
    
    // Get connection retry count
    int GetRetryCount() const { return m_retryCount; }
    
private:
    // Worker thread function
    void WorkerLoop();
    
    // Connection management with retry
    bool ConnectToRedis();
    void DisconnectFromRedis();
    bool Reconnect();
    
    // Serialize detections to JSON
    std::string SerializeToJSON(const std::vector<Detection>& detections);
    
    // Publish to Redis with error handling
    bool PublishDetections(const std::string& jsonData);
    
    // Redis connection info
    std::string m_host;
    int m_port;
    bool m_enabled;
    
    // Worker thread
    std::thread m_workerThread;
    std::atomic<bool> m_running;
    std::atomic<bool> m_connected;
    std::atomic<int> m_retryCount;
    
    // Shared detection array
    std::vector<Detection> m_detections;
    std::mutex m_dataMutex;
    
    // Redis connection handle (opaque pointer)
    sw::redis::Redis* m_redisClient;
    std::mutex m_redisMutex;
    
    // Retry configuration
    static constexpr int MAX_RETRY_ATTEMPTS = 5;
    static constexpr int RETRY_DELAY_MS = 1000;  // 1 second
};
