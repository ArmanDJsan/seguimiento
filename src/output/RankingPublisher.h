/**
 * RankingPublisher.h
 * 
 * Real-time ranking publication to vMix via TCP commands
 * Sends ball position rankings to update GT Titles
 * 
 * Format: JSON {"p1": "3", "p2": "5", "p3": "1", ..., "p10": "8"}
 * where pN is the position and the value is the ball ID
 */

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <winsock2.h>

/**
 * Ranking publisher configuration
 */
struct RankingPublisherConfig {
    std::string vmixHost = "127.0.0.1";
    uint16_t vmixTcpPort = 8099;
    int publishRateHz = 30;           // Publish rate (times per second)
    std::string titleInputName = "RankingTitle";  // vMix title input name
    bool enabled = true;
};

/**
 * RankingPublisher - Send rankings to vMix
 * 
 * Thread safety: All public methods are thread-safe
 */
class RankingPublisher {
public:
    static constexpr int kMaxBalls = 10;
    
    /**
     * Constructor
     * @param config Publisher configuration
     */
    explicit RankingPublisher(const RankingPublisherConfig& config = RankingPublisherConfig());
    ~RankingPublisher();
    
    // Non-copyable
    RankingPublisher(const RankingPublisher&) = delete;
    RankingPublisher& operator=(const RankingPublisher&) = delete;
    
    /**
     * Initialize and connect to vMix
     * @return true if successful
     */
    bool Initialize();
    
    /**
     * Shutdown publisher
     */
    void Shutdown();
    
    /**
     * Publish ranking to vMix
     * @param ranking Vector of ball IDs in ranking order (1st place first)
     * @return true if sent successfully
     */
    bool PublishRanking(const std::vector<int>& ranking);
    
    /**
     * Check if connected to vMix
     */
    bool IsConnected() const { return m_connected; }
    
    /**
     * Check if publisher is enabled
     */
    bool IsEnabled() const { return m_config.enabled; }
    
    /**
     * Get last error message
     */
    std::string GetLastError() const;
    
    /**
     * Get number of successful publishes
     */
    uint64_t GetPublishCount() const { return m_publishCount; }
    
    /**
     * Get number of failed publishes
     */
    uint64_t GetFailCount() const { return m_failCount; }
    
    /**
     * Reconnect to vMix
     */
    bool Reconnect();

private:
    RankingPublisherConfig m_config;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_connected;
    mutable std::mutex m_mutex;
    std::string m_lastError;
    
    // Statistics
    std::atomic<uint64_t> m_publishCount;
    std::atomic<uint64_t> m_failCount;
    
    // Network
    SOCKET m_socket;
    bool m_winsockInitialized;
    
    // Rate limiting
    std::chrono::steady_clock::time_point m_lastPublishTime;
    std::chrono::milliseconds m_publishInterval;
    
    // Helper methods
    bool Connect();
    void Disconnect();
    bool SendCommand(const std::string& command);
    std::string BuildRankingJson(const std::vector<int>& ranking);
    std::string BuildVMixCommand(const std::string& json);
    void SetLastError(const std::string& error);
};
