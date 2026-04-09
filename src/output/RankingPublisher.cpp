/**
 * RankingPublisher.cpp
 * 
 * Implementation of ranking publication to vMix
 */

#include "RankingPublisher.h"
#include "../utils/Logger.h"
#include <ws2tcpip.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "Ws2_32.lib")

RankingPublisher::RankingPublisher(const RankingPublisherConfig& config)
    : m_config(config)
    , m_initialized(false)
    , m_connected(false)
    , m_publishCount(0)
    , m_failCount(0)
    , m_socket(INVALID_SOCKET)
    , m_winsockInitialized(false)
    , m_publishInterval(1000 / config.publishRateHz)
{
    m_lastPublishTime = std::chrono::steady_clock::now() - m_publishInterval;
}

RankingPublisher::~RankingPublisher() {
    Shutdown();
}

bool RankingPublisher::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) {
        return true;
    }
    
    if (!m_config.enabled) {
        Logger::Info("RankingPublisher: Disabled by configuration");
        m_initialized = true;
        return true;
    }
    
    // Initialize Winsock
    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        SetLastError("Winsock initialization failed: " + std::to_string(result));
        Logger::Error("RankingPublisher: " + m_lastError);
        return false;
    }
    m_winsockInitialized = true;
    
    // Connect to vMix
    if (!Connect()) {
        // Connection failure is not fatal - we can retry later
        Logger::Warning("RankingPublisher: Initial connection failed, will retry");
    }
    
    m_initialized = true;
    Logger::Info("RankingPublisher: Initialized, target=" + m_config.vmixHost + 
                 ":" + std::to_string(m_config.vmixTcpPort) +
                 ", rate=" + std::to_string(m_config.publishRateHz) + "Hz");
    
    return true;
}

void RankingPublisher::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    Disconnect();
    
    if (m_winsockInitialized) {
        WSACleanup();
        m_winsockInitialized = false;
    }
    
    m_initialized = false;
    Logger::Info("RankingPublisher: Shutdown complete. Published: " + 
                 std::to_string(m_publishCount.load()) + ", Failed: " + 
                 std::to_string(m_failCount.load()));
}

bool RankingPublisher::PublishRanking(const std::vector<int>& ranking) {
    if (!m_initialized || !m_config.enabled) {
        return false;
    }
    
    // Rate limiting
    auto now = std::chrono::steady_clock::now();
    if (now - m_lastPublishTime < m_publishInterval) {
        return true;  // Skip this update, not enough time elapsed
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Try to reconnect if not connected
    if (!m_connected) {
        if (!Connect()) {
            m_failCount++;
            return false;
        }
    }
    
    // Build and send command
    std::string json = BuildRankingJson(ranking);
    std::string command = BuildVMixCommand(json);
    
    if (SendCommand(command)) {
        m_publishCount++;
        m_lastPublishTime = now;
        return true;
    } else {
        m_failCount++;
        Disconnect();  // Force reconnect on next attempt
        return false;
    }
}

std::string RankingPublisher::GetLastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

bool RankingPublisher::Reconnect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    Disconnect();
    return Connect();
}

bool RankingPublisher::Connect() {
    if (m_connected) {
        return true;
    }
    
    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) {
        SetLastError("Failed to create socket: " + std::to_string(WSAGetLastError()));
        return false;
    }
    
    // Set socket timeout
    DWORD timeout = 1000;  // 1 second
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
    
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_config.vmixTcpPort);
    
    if (inet_pton(AF_INET, m_config.vmixHost.c_str(), &serverAddr.sin_addr) != 1) {
        SetLastError("Invalid vMix IP address: " + m_config.vmixHost);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }
    
    if (connect(m_socket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        SetLastError("Connection failed: " + std::to_string(WSAGetLastError()));
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }
    
    m_connected = true;
    Logger::Info("RankingPublisher: Connected to vMix at " + m_config.vmixHost + 
                 ":" + std::to_string(m_config.vmixTcpPort));
    
    return true;
}

void RankingPublisher::Disconnect() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    m_connected = false;
}

bool RankingPublisher::SendCommand(const std::string& command) {
    if (!m_connected || m_socket == INVALID_SOCKET) {
        return false;
    }
    
    int totalSent = 0;
    int remaining = static_cast<int>(command.size());
    const char* data = command.c_str();
    
    while (remaining > 0) {
        int sent = send(m_socket, data + totalSent, remaining, 0);
        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            SetLastError("Send failed: " + std::to_string(error));
            return false;
        }
        totalSent += sent;
        remaining -= sent;
    }
    
    return true;
}

std::string RankingPublisher::BuildRankingJson(const std::vector<int>& ranking) {
    std::ostringstream oss;
    oss << "{";
    
    for (size_t i = 0; i < ranking.size() && i < kMaxBalls; ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << "\"p" << (i + 1) << "\":\"" << ranking[i] << "\"";
    }
    
    // Fill remaining positions with empty
    for (size_t i = ranking.size(); i < kMaxBalls; ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << "\"p" << (i + 1) << "\":\"\"";
    }
    
    oss << "}";
    return oss.str();
}

std::string RankingPublisher::BuildVMixCommand(const std::string& json) {
    // vMix TCP API format for setting title text:
    // FUNCTION SetText Input=<InputName> SelectedName=<FieldName> Value=<Value>
    //
    // For DataSource/JSON approach, we use:
    // FUNCTION DataSourceSelectRow Value=1 [followed by data]
    //
    // Simpler approach: Set text fields directly
    // Or use: FUNCTION SetDynamicInput1 Value=<JSON>
    //
    // Using the simplest approach: ACTS command (ActiveTitle SetText)
    
    std::ostringstream oss;
    
    // vMix API command to set JSON data
    // Using FUNCTION command format
    oss << "FUNCTION SetText Input=" << m_config.titleInputName 
        << " SelectedName=RankingData Value=" << json << "\r\n";
    
    return oss.str();
}

void RankingPublisher::SetLastError(const std::string& error) {
    m_lastError = error;
    Logger::Debug("RankingPublisher: " + error);
}
